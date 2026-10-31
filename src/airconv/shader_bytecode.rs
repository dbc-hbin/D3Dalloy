// SPDX-License-Identifier: LGPL-2.1-or-later
// Allocation-free little-endian shader containers, RDAT names, cache codecs and root-signature bounds.
#![no_std]

mod cache_pipeline;
mod cache_library;
mod cache_stage;
mod cache_wire;
mod dxbc_program;
mod dxbc_signature;

use core::{panic::PanicInfo, slice};

extern "C" {
    fn abort() -> !;
}

#[panic_handler]
fn panic(_: &PanicInfo<'_>) -> ! {
    // Invalid input returns an error below; a Rust panic cannot cross the C ABI.
    unsafe { abort() }
}

// Prebuilt core retains unwind metadata even with panic=abort at opt-level=0.
// These callback-free C entry points cannot unwind; terminate if reached.
#[no_mangle]
pub extern "C" fn rust_eh_personality() -> ! {
    unsafe { abort() }
}

fn word(bytes: &[u8], offset: usize) -> Option<u32> {
    let value = bytes.get(offset..offset.checked_add(4)?)?;
    Some(u32::from_le_bytes([value[0], value[1], value[2], value[3]]))
}

// Only fixed-width status values and repr(C) scalar records cross the ABI.
// Borrowed slices and typed errors remain entirely inside this module.
enum Error {
    InvalidArgument = 1,
    TruncatedContainer = 2,
    ContainerMagic = 3,
    ContainerVersion = 4,
    ContainerSize = 5,
    ChunkTable = 6,
    ChunkOffset = 7,
    ChunkSize = 8,
    ChunkLayout = 9,
    NotFound = 10,
    TruncatedDxil = 11,
    DxilEncoding = 12,
    BitcodeOffset = 13,
    BitcodeSize = 14,
    BitcodeMagic = 15,
    RdatLayout = 16,
    RdatString = 17,
    RdatKind = 18,
}

type Result<T> = core::result::Result<T, Error>;

#[derive(Clone, Copy)]
enum Layout {
    Legacy,
    RootSignature,
    Dxil,
}

impl Layout {
    fn from_abi(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::Legacy),
            1 => Ok(Self::RootSignature),
            2 => Ok(Self::Dxil),
            _ => Err(Error::InvalidArgument),
        }
    }
}

#[repr(C)]
pub struct ContainerInfo {
    size: u32,
    chunk_count: u32,
    version_major: u32,
    version_minor: u32,
    hash: [u8; 16],
}

#[repr(C)]
pub struct ChunkInfo {
    index: u32,
    fourcc: u32,
    offset: u32,
    size: u32,
}

#[repr(C)]
pub struct DxilProgram {
    program_version: u32,
    shader_kind: u32,
    bitcode_offset: u32,
    bitcode_size: u32,
}

#[repr(C)]
pub struct RdatInfo {
    strings_offset: u32,
    strings_size: u32,
    records_offset: u32,
    record_count: u32,
    record_stride: u32,
}

#[repr(C)]
pub struct RdatFunction {
    name_offset: u32,
    name_size: u32,
    public_name_offset: u32,
    public_name_size: u32,
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Span {
    offset: u32,
    size: u32,
}

#[repr(C)]
pub struct RdatSubobjects {
    present: u32,
    record_count: u32,
    record_stride: u32,
    records_offset: u32,
    // A zero `present` bit means the part is absent; bit 0 strings, 1 index arrays, 2 raw bytes.
    parts: u32,
    strings: Span,
    indices: Span,
    raw: Span,
}

#[repr(C)]
pub struct RdatSubobject {
    kind: u32,
    words: [u32; 4],
    name: Span,
    // Associated subobject, or any-hit, closest-hit and intersection names.
    strings: [Span; 3],
    root_signature: Span,
    export_count: u32,
    exports_offset: u32,
}

struct Chunk<'a> {
    index: u32,
    fourcc: u32,
    offset: u32,
    data: &'a [u8],
}

impl Chunk<'_> {
    fn info(&self) -> ChunkInfo {
        ChunkInfo {
            index: self.index,
            fourcc: self.fourcc,
            offset: self.offset,
            size: self.data.len() as u32,
        }
    }
}

struct Container<'a> {
    bytes: &'a [u8],
    index: &'a [u8],
}

impl<'a> Container<'a> {
    fn header(bytes: &'a [u8], layout: Layout) -> Result<Self> {
        if bytes.len() < 32 {
            return Err(Error::TruncatedContainer);
        }
        if word(bytes, 0) != Some(u32::from_le_bytes(*b"DXBC")) {
            return Err(Error::ContainerMagic);
        }
        if word(bytes, 20) != Some(1) {
            return Err(Error::ContainerVersion);
        }
        let size = word(bytes, 24).ok_or(Error::TruncatedContainer)? as usize;
        if size < 32 || matches!(layout, Layout::Legacy) && size != bytes.len() {
            return Err(Error::ContainerSize);
        }
        // Root signatures and DXIL may be followed by caller-owned padding.
        // Neither the index nor any chunk may escape the declared extent.
        let bytes = bytes.get(..size).ok_or(Error::ContainerSize)?;
        let count = word(bytes, 28).ok_or(Error::TruncatedContainer)? as usize;
        let index_end = count
            .checked_mul(4)
            .and_then(|size| 32usize.checked_add(size))
            .ok_or(Error::ChunkTable)?;
        let index = bytes.get(32..index_end).ok_or(Error::ChunkTable)?;
        Ok(Self { bytes, index })
    }

    fn parse(bytes: &'a [u8], layout: Layout) -> Result<Self> {
        let container = Self::header(bytes, layout)?;
        let mut cursor = 32 + container.index.len();
        for index in 0..container.count() {
            let chunk = container.chunk(index)?;
            if !matches!(layout, Layout::Dxil) && chunk.offset as usize - 8 != cursor {
                return Err(Error::ChunkLayout);
            }
            cursor = (chunk.offset as usize)
                .checked_add(chunk.data.len())
                .ok_or(Error::ChunkSize)?;
        }
        Ok(container)
    }

    fn count(&self) -> u32 {
        (self.index.len() / 4) as u32
    }

    fn info(&self) -> ContainerInfo {
        let mut hash = [0u8; 16];
        hash.copy_from_slice(&self.bytes[4..20]);
        ContainerInfo {
            size: self.bytes.len() as u32,
            chunk_count: self.count(),
            version_major: 1,
            version_minor: 0,
            hash,
        }
    }

    fn chunk(&self, index: u32) -> Result<Chunk<'a>> {
        if index >= self.count() {
            return Err(Error::NotFound);
        }
        let entry = (index as usize).checked_mul(4).ok_or(Error::ChunkTable)?;
        let offset = word(self.index, entry).ok_or(Error::ChunkTable)? as usize;
        // Indexed DXIL chunks may alias, have gaps, or be reordered, but their
        // headers must not start inside the container header or index table.
        if offset < 32 + self.index.len() {
            return Err(Error::ChunkOffset);
        }
        let payload = offset.checked_add(8).ok_or(Error::ChunkOffset)?;
        let header = self.bytes.get(offset..payload).ok_or(Error::ChunkOffset)?;
        let size = word(header, 4).ok_or(Error::ChunkOffset)? as usize;
        let end = payload.checked_add(size).ok_or(Error::ChunkSize)?;
        let data = self.bytes.get(payload..end).ok_or(Error::ChunkSize)?;
        Ok(Chunk {
            index,
            fourcc: word(header, 0).ok_or(Error::ChunkOffset)?,
            offset: payload as u32,
            data,
        })
    }

    fn find(&self, fourcc: u32, start: u32) -> Result<Chunk<'a>> {
        for index in start..self.count() {
            let chunk = self.chunk(index)?;
            if chunk.fourcc == fourcc {
                return Ok(chunk);
            }
        }
        Err(Error::NotFound)
    }
}

fn dxil_program(container: &Container<'_>) -> Result<DxilProgram> {
    let chunk = container.find(u32::from_le_bytes(*b"DXIL"), 0)?;
    let program_version = word(chunk.data, 0).ok_or(Error::TruncatedDxil)?;
    let shader_kind = (program_version >> 16) & 0xff;
    let bitcode_magic = u32::from_le_bytes(*b"BC\xc0\xde");
    if program_version == bitcode_magic {
        return Ok(DxilProgram {
            program_version,
            shader_kind,
            bitcode_offset: chunk.offset,
            bitcode_size: chunk.data.len() as u32,
        });
    }
    // Program size and DXIL version are deliberately not constrained: existing
    // consumers interpret only ProgramVersion and this bitcode envelope.
    if chunk.data.len() < 24 || word(chunk.data, 8) != Some(u32::from_le_bytes(*b"DXIL")) {
        return Err(Error::DxilEncoding);
    }
    let offset = word(chunk.data, 16).ok_or(Error::DxilEncoding)? as usize;
    let size = word(chunk.data, 20).ok_or(Error::DxilEncoding)? as usize;
    let start = 8usize.checked_add(offset).ok_or(Error::BitcodeOffset)?;
    if offset < 16 || start > chunk.data.len() {
        return Err(Error::BitcodeOffset);
    }
    let end = start.checked_add(size).ok_or(Error::BitcodeSize)?;
    let bitcode = chunk.data.get(start..end).ok_or(Error::BitcodeSize)?;
    if word(bitcode, 0) != Some(bitcode_magic) {
        return Err(Error::BitcodeMagic);
    }
    Ok(DxilProgram {
        program_version,
        shader_kind,
        bitcode_offset: chunk.offset.checked_add(start as u32).ok_or(Error::BitcodeOffset)?,
        bitcode_size: bitcode.len() as u32,
    })
}

/// Visits every RDAT part after checking its extent. Only the fields used by our
/// consumers constrain the layout: versions, unknown parts and trailing record
/// fields remain extensible.
fn rdat_parts<'a>(chunk: &Chunk<'a>, mut visit: impl FnMut(u32, u32, &'a [u8])) -> Result<()> {
    let count = word(chunk.data, 4).ok_or(Error::RdatLayout)? as usize;
    let index_end = count.checked_mul(4).and_then(|n| n.checked_add(8)).ok_or(Error::RdatLayout)?;
    let index = chunk.data.get(8..index_end).ok_or(Error::RdatLayout)?;
    for entry in index.chunks_exact(4) {
        let offset = word(entry, 0).ok_or(Error::RdatLayout)? as usize;
        let start = offset.checked_add(8).ok_or(Error::RdatLayout)?;
        let header = chunk.data.get(offset..start).ok_or(Error::RdatLayout)?;
        let size = word(header, 4).ok_or(Error::RdatLayout)? as usize;
        let end = start.checked_add(size).ok_or(Error::RdatLayout)?;
        let payload = chunk.data.get(start..end).ok_or(Error::RdatLayout)?;
        let absolute = chunk.offset.checked_add(start as u32).ok_or(Error::RdatLayout)?;
        visit(word(header, 0).ok_or(Error::RdatLayout)?, absolute, payload);
    }
    Ok(())
}

impl RdatInfo {
    fn parse(container: &Container<'_>) -> Result<Self> {
        let chunk = container.find(u32::from_le_bytes(*b"RDAT"), 0)?;
        let mut strings = None;
        let mut functions = None;
        rdat_parts(&chunk, |kind, absolute, payload| match kind {
            1 => strings = Some((absolute, payload)),
            4 => functions = Some((absolute, payload)),
            _ => {}
        })?;
        // As in the existing reader, the last part of each recognized type wins.
        let (strings_offset, strings) = strings.ok_or(Error::RdatLayout)?;
        let (functions_offset, functions) = functions.ok_or(Error::RdatLayout)?;
        let record_count = word(functions, 0).ok_or(Error::RdatLayout)?;
        let record_stride = word(functions, 4).ok_or(Error::RdatLayout)?;
        if record_count == 0 || record_stride < 8 {
            return Err(Error::RdatLayout);
        }
        let end = (record_count as usize)
            .checked_mul(record_stride as usize)
            .and_then(|n| n.checked_add(8))
            .ok_or(Error::RdatLayout)?;
        functions.get(8..end).ok_or(Error::RdatLayout)?;
        Ok(Self {
            strings_offset,
            strings_size: strings.len() as u32,
            records_offset: functions_offset.checked_add(8).ok_or(Error::RdatLayout)?,
            record_count,
            record_stride,
        })
    }

    fn function(&self, bytes: &[u8], index: u32) -> Result<RdatFunction> {
        if index >= self.record_count {
            return Err(Error::NotFound);
        }
        if self.record_stride < 8 {
            return Err(Error::RdatLayout);
        }
        let start = (index as usize)
            .checked_mul(self.record_stride as usize)
            .and_then(|n| n.checked_add(self.records_offset as usize))
            .ok_or(Error::RdatLayout)?;
        let end = start.checked_add(8).ok_or(Error::RdatLayout)?;
        let record = bytes.get(start..end).ok_or(Error::RdatLayout)?;
        let strings_end = (self.strings_offset as usize)
            .checked_add(self.strings_size as usize)
            .ok_or(Error::RdatLayout)?;
        let strings = bytes.get(self.strings_offset as usize..strings_end).ok_or(Error::RdatLayout)?;
        let name_offset = word(record, 0).ok_or(Error::RdatLayout)?;
        let public_name_offset = word(record, 4).ok_or(Error::RdatLayout)?;
        let name = rdat_name(strings, name_offset)?;
        let public_name = rdat_name(strings, public_name_offset)?;
        Ok(RdatFunction {
            name_offset: self.strings_offset.checked_add(name_offset).ok_or(Error::RdatString)?,
            name_size: name.len() as u32,
            public_name_offset: self.strings_offset.checked_add(public_name_offset).ok_or(Error::RdatString)?,
            public_name_size: public_name.len() as u32,
        })
    }
}

fn span(offset: u32, size: usize) -> Span {
    Span { offset, size: size as u32 }
}

impl RdatSubobjects {
    fn parse(container: &Container<'_>) -> Result<Self> {
        let mut result = RdatSubobjects {
            present: 0,
            record_count: 0,
            record_stride: 0,
            records_offset: 0,
            parts: 0,
            strings: Span::default(),
            indices: Span::default(),
            raw: Span::default(),
        };
        let chunk = match container.find(u32::from_le_bytes(*b"RDAT"), 0) {
            Ok(chunk) => chunk,
            Err(Error::NotFound) => return Ok(result),
            Err(error) => return Err(error),
        };
        // As for function names, the last part of each recognized type wins.
        let mut table = None;
        rdat_parts(&chunk, |kind, absolute, payload| match kind {
            1 => {
                result.parts |= 1;
                result.strings = span(absolute, payload.len());
            }
            2 => {
                result.parts |= 2;
                result.indices = span(absolute, payload.len());
            }
            5 => {
                result.parts |= 4;
                result.raw = span(absolute, payload.len());
            }
            6 => table = Some((absolute, payload)),
            _ => {}
        })?;
        let Some((table_offset, table)) = table else {
            return Ok(result);
        };
        let count = word(table, 0).ok_or(Error::RdatLayout)?;
        let stride = word(table, 4).ok_or(Error::RdatLayout)?;
        // Records hold { Kind, Name, four payload words }.
        let end = (count as u64).checked_mul(stride as u64).and_then(|n| n.checked_add(8));
        if stride < 24 || end.map_or(true, |end| end > table.len() as u64) {
            return Err(Error::RdatLayout);
        }
        result.present = 1;
        result.record_count = count;
        result.record_stride = stride;
        result.records_offset = table_offset + 8;
        Ok(result)
    }

    fn part<'a>(&self, bytes: &'a [u8], bit: u32, part: Span) -> Result<&'a [u8]> {
        if self.parts & bit == 0 {
            return Err(Error::RdatLayout);
        }
        let end = (part.offset as usize).checked_add(part.size as usize).ok_or(Error::RdatLayout)?;
        bytes.get(part.offset as usize..end).ok_or(Error::RdatLayout)
    }

    // A possibly empty NUL-terminated name inside the string part.
    fn string(&self, bytes: &[u8], offset: u32) -> Result<Span> {
        let strings = self.part(bytes, 1, self.strings).map_err(|_| Error::RdatString)?;
        let suffix = strings.get(offset as usize..).ok_or(Error::RdatString)?;
        let length = suffix.iter().position(|&byte| byte == 0).ok_or(Error::RdatString)?;
        Ok(span(self.strings.offset.checked_add(offset).ok_or(Error::RdatString)?, length))
    }

    fn named(&self, bytes: &[u8], offset: u32) -> Result<Span> {
        let name = self.string(bytes, offset)?;
        if name.size == 0 {
            return Err(Error::RdatString);
        }
        Ok(name)
    }

    // Word offset of an index array { count, elements[count] }.
    fn index_array(&self, bytes: &[u8], offset: u32) -> Result<(u32, u32)> {
        let indices = self.part(bytes, 2, self.indices)?;
        let start = offset as u64 * 4;
        let count = word(indices, usize::try_from(start).map_err(|_| Error::RdatLayout)?).ok_or(Error::RdatLayout)?;
        let end = (offset as u64 + 1 + count as u64) * 4;
        if end > indices.len() as u64 {
            return Err(Error::RdatLayout);
        }
        // In bounds of the part, whose absolute extent fits u32.
        Ok((count, self.indices.offset + start as u32 + 4))
    }

    fn subobject(&self, bytes: &[u8], index: u32) -> Result<RdatSubobject> {
        if self.present == 0 || index >= self.record_count {
            return Err(Error::NotFound);
        }
        let start = (index as u64 * self.record_stride as u64)
            .checked_add(self.records_offset as u64)
            .and_then(|n| usize::try_from(n).ok())
            .ok_or(Error::RdatLayout)?;
        let record = bytes.get(start..start.checked_add(24).ok_or(Error::RdatLayout)?).ok_or(Error::RdatLayout)?;
        let field = |offset| word(record, offset).ok_or(Error::RdatLayout);
        let words = [field(8)?, field(12)?, field(16)?, field(20)?];
        let mut result = RdatSubobject {
            kind: field(0)?,
            words,
            // Every record is named, including kinds this reader cannot describe.
            name: self.named(bytes, field(4)?)?,
            strings: [Span::default(); 3],
            root_signature: Span::default(),
            export_count: 0,
            exports_offset: 0,
        };
        match result.kind {
            0 | 9 | 10 | 12 => {}
            1 | 2 => {
                let raw = self.part(bytes, 4, self.raw)?;
                if words[1] == 0 || words[0] as u64 + words[1] as u64 > raw.len() as u64 {
                    return Err(Error::RdatLayout);
                }
                // In bounds of the part, whose absolute extent fits u32.
                result.root_signature = span(self.raw.offset + words[0], words[1] as usize);
            }
            8 => {
                result.strings[0] = self.named(bytes, words[0])?;
                let (count, offset) = self.index_array(bytes, words[1])?;
                for i in 0..count {
                    let element = word(bytes, offset as usize + i as usize * 4).ok_or(Error::RdatLayout)?;
                    // Rejected names keep the old all-or-nothing association contract.
                    self.named(bytes, element)?;
                }
                result.export_count = count;
                result.exports_offset = offset;
            }
            11 => {
                // D3D12_HIT_GROUP_TYPE_TRIANGLES or _PROCEDURAL_PRIMITIVE; empty names are absent shaders.
                if words[0] > 1 {
                    return Err(Error::RdatLayout);
                }
                for i in 0..3 {
                    result.strings[i] = self.string(bytes, words[i + 1])?;
                }
            }
            _ => return Err(Error::RdatKind),
        }
        Ok(result)
    }
}

fn rdat_name(strings: &[u8], offset: u32) -> Result<&[u8]> {
    let suffix = strings.get(offset as usize..).ok_or(Error::RdatString)?;
    let length = suffix.iter().position(|&byte| byte == 0).ok_or(Error::RdatString)?;
    if length == 0 {
        return Err(Error::RdatString);
    }
    // Names are bytes, not Rust UTF-8 strings: preserve DXC's exact spelling.
    Ok(&suffix[..length])
}

/// The caller owns readable immutable storage for the requested byte span.
unsafe fn input<'a>(data: *const u8, length: u64) -> Result<&'a [u8]> {
    let length = usize::try_from(length).map_err(|_| Error::InvalidArgument)?;
    if data.is_null() || length > isize::MAX as usize {
        return Err(Error::InvalidArgument);
    }
    Ok(unsafe { slice::from_raw_parts(data, length) })
}

/// The caller supplies aligned writable storage. No output is changed on error.
unsafe fn publish<T>(out: *mut T, value: Result<T>) -> u32 {
    if out.is_null() {
        return Error::InvalidArgument as u32;
    }
    match value {
        Ok(value) => {
            unsafe { out.write(value) };
            0
        }
        Err(error) => error as u32,
    }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_container_parse(
    data: *const u8,
    length: u64,
    policy: u32,
    out: *mut ContainerInfo,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        Ok(Container::parse(bytes, Layout::from_abi(policy)?)?.info())
    })();
    unsafe { publish(out, result) }
}

/// Checked O(1) access after a successful parse of the same immutable buffer.
#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_container_chunk(
    data: *const u8,
    length: u64,
    policy: u32,
    index: u32,
    out: *mut ChunkInfo,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        Ok(Container::header(bytes, Layout::from_abi(policy)?)?.chunk(index)?.info())
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_container_find(
    data: *const u8,
    length: u64,
    policy: u32,
    fourcc: u32,
    start_index: u32,
    out: *mut ChunkInfo,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        let container = Container::parse(bytes, Layout::from_abi(policy)?)?;
        Ok(container.find(fourcc, start_index)?.info())
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_dxil_program(
    data: *const u8,
    length: u64,
    out: *mut DxilProgram,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        dxil_program(&Container::parse(bytes, Layout::Dxil)?)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_container_declared_size(
    data: *const u8,
    length: u64,
    out: *mut u32,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        word(bytes, 24).ok_or(Error::TruncatedContainer)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_rdat_parse(
    data: *const u8,
    length: u64,
    out: *mut RdatInfo,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        RdatInfo::parse(&Container::parse(bytes, Layout::Dxil)?)
    })();
    unsafe { publish(out, result) }
}

/// Info comes from a successful parse of this same immutable input.
#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_rdat_function(
    data: *const u8,
    length: u64,
    info: *const RdatInfo,
    index: u32,
    out: *mut RdatFunction,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        let info = unsafe { info.as_ref() }.ok_or(Error::InvalidArgument)?;
        info.function(bytes, index)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_rdat_subobjects(
    data: *const u8,
    length: u64,
    out: *mut RdatSubobjects,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        RdatSubobjects::parse(&Container::parse(bytes, Layout::Legacy)?)
    })();
    unsafe { publish(out, result) }
}

/// Table comes from a successful subobject parse of this same immutable input.
#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_rdat_subobject(
    data: *const u8,
    length: u64,
    table: *const RdatSubobjects,
    index: u32,
    out: *mut RdatSubobject,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        let table = unsafe { table.as_ref() }.ok_or(Error::InvalidArgument)?;
        table.subobject(bytes, index)
    })();
    unsafe { publish(out, result) }
}

/// Subobject comes from a successful association read of this same input and table.
#[no_mangle]
pub unsafe extern "C" fn dxmt_shader_rdat_export(
    data: *const u8,
    length: u64,
    table: *const RdatSubobjects,
    subobject: *const RdatSubobject,
    index: u32,
    out: *mut Span,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }?;
        let table = unsafe { table.as_ref() }.ok_or(Error::InvalidArgument)?;
        let subobject = unsafe { subobject.as_ref() }.ok_or(Error::InvalidArgument)?;
        if index >= subobject.export_count {
            return Err(Error::NotFound);
        }
        let position = (subobject.exports_offset as u64 + index as u64 * 4) as usize;
        let element = word(bytes, position).ok_or(Error::RdatLayout)?;
        table.named(bytes, element)
    })();
    unsafe { publish(out, result) }
}

fn region(bytes: &[u8], offset: u32, count: u32, stride: usize) -> Option<&[u8]> {
    // Unused offsets have no meaning. C++ must not form pointers from them.
    if count == 0 {
        return Some(&[]);
    }
    let offset = offset as usize;
    if offset % 4 != 0 {
        return None;
    }
    let size = (count as usize).checked_mul(stride)?;
    bytes.get(offset..offset.checked_add(size)?)
}

fn validate(bytes: &[u8]) -> Option<()> {
    // RTS0: six u32 header fields; parameter records contain three u32 fields.
    region(bytes, 0, 1, 24)?;
    let (range_stride, descriptor_stride, sampler_stride) = match word(bytes, 0)? {
        1 => (20, 8, 52),
        2 => (24, 12, 52),
        3 => (24, 12, 56),
        _ => return None,
    };
    let parameters = region(bytes, word(bytes, 8)?, word(bytes, 4)?, 12)?;
    for parameter in parameters.chunks_exact(12) {
        let offset = word(parameter, 8)?;
        match word(parameter, 0)? {
            0 => {
                let table = region(bytes, offset, 1, 8)?;
                region(bytes, word(table, 4)?, word(table, 0)?, range_stride)?;
            }
            1 => {
                region(bytes, offset, 1, 12)?;
            }
            2..=4 => {
                region(bytes, offset, 1, descriptor_stride)?;
            }
            _ => return None,
        }
    }
    region(bytes, word(bytes, 16)?, word(bytes, 12)?, sampler_stride)?;
    Some(())
}

/// The caller supplies an immutable, readable allocation of `length` bytes.
/// Returns one only when all ranges the C++ deserializer reads are valid.
#[no_mangle]
pub unsafe extern "C" fn dxmt_validate_root_signature(data: *const u8, length: u64) -> i32 {
    let Ok(bytes) = (unsafe { input(data, length) }) else {
        return 0;
    };
    i32::from(validate(bytes).is_some())
}
