// SPDX-License-Identifier: LGPL-2.1-or-later
// Data-only PE preflight of an owned D3D12Core.dll: DOS/NT headers for one
// machine, the Wine loader DOS-stub markers, at most 96 sections, and named
// exports resolved through the export directory to file offsets. Forwarded
// exports never resolve.
use super::cache_wire::{input, range, u32_at};

const DOS_SIGNATURE: u16 = 0x5a4d; // "MZ"
const NT_SIGNATURE: u32 = 0x0000_4550; // "PE\0\0"
const DOS_HEADER_SIZE: u64 = 64;
const FILE_HEADER_SIZE: u64 = 20;
const SECTION_SIZE: u64 = 40;
const EXPORT_DIRECTORY_SIZE: u64 = 40;
const MAX_SECTIONS: u16 = 96;
const MAX_EXPORTS: u32 = 65536;
const DIRECTORY_EXPORT: u32 = 0;

enum Error {
    InvalidArgument = 1,
    Unsupported = 2,
    NotFound = 3,
}
type Result<T, E = Error> = core::result::Result<T, E>;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct Image {
    nt_offset: u64,
    sections_offset: u64,
    export_rva: u32,
    export_size: u32,
    section_count: u32,
    machine: u32,
    // Exactly as NumberOfNames/NumberOfFunctions and the three tables' file offsets.
    names_offset: u64,
    ordinals_offset: u64,
    functions_offset: u64,
    name_count: u32,
    function_count: u32,
}

fn u16_at(bytes: &[u8], offset: u64) -> Option<u16> {
    Some(u16::from_le_bytes(range(bytes, offset, 2)?.try_into().ok()?))
}

fn need<T>(value: Option<T>) -> Result<T> {
    value.ok_or(Error::Unsupported)
}

// File offset of `count` bytes at `rva`, from the first section whose raw data
// holds the whole range inside the file.
fn address(bytes: &[u8], sections: u64, count: u32, rva: u64, length: u64) -> Option<u64> {
    let size = bytes.len() as u64;
    (0..count as u64).find_map(|index| {
        let section = sections + index * SECTION_SIZE;
        let virtual_address = u32_at(bytes, section + 12)? as u64;
        let raw_size = u32_at(bytes, section + 16)? as u64;
        let raw_pointer = u32_at(bytes, section + 20)? as u64;
        let delta = rva.checked_sub(virtual_address)?;
        if delta > raw_size || length > raw_size - delta {
            return None;
        }
        let offset = raw_pointer + delta;
        (offset <= size && length <= size - offset).then_some(offset)
    })
}

fn image(bytes: &[u8], machine: u16) -> Result<Image> {
    let pe32_plus = machine != 0x014c;
    let optional_size: u64 = if pe32_plus { 240 } else { 224 };
    let optional_magic: u16 = if pe32_plus { 0x20b } else { 0x10b };
    // Offsets inside the optional header.
    let rva_count_offset: u64 = if pe32_plus { 108 } else { 92 };
    let lfanew = need(u32_at(bytes, 60))?;
    if need(u16_at(bytes, 0))? != DOS_SIGNATURE || lfanew > i32::MAX as u32 {
        return Err(Error::Unsupported);
    }
    let nt = lfanew as u64;
    let optional = nt + 4 + FILE_HEADER_SIZE;
    // The complete IMAGE_NT_HEADERS of this ABI must be in the file.
    need(range(bytes, nt, 4 + FILE_HEADER_SIZE + optional_size))?;
    let sections = need(u16_at(bytes, nt + 6))?;
    if need(u32_at(bytes, nt))? != NT_SIGNATURE || need(u16_at(bytes, optional))? != optional_magic ||
        need(u16_at(bytes, nt + 20))? as u64 != optional_size ||
        need(u32_at(bytes, optional + rva_count_offset))? <= DIRECTORY_EXPORT ||
        sections == 0 || sections > MAX_SECTIONS {
        return Err(Error::Unsupported);
    }
    // Wine reads these DOS-stub signatures before deciding native versus builtin.
    let marker = need(range(bytes, DOS_HEADER_SIZE, 32))?;
    if marker.starts_with(b"Wine builtin DLL\0") || marker.starts_with(b"Wine placeholder DLL\0") {
        return Err(Error::Unsupported);
    }
    if need(u16_at(bytes, nt + 4))? != machine {
        return Err(Error::Unsupported);
    }
    let sections_offset = nt + 4 + FILE_HEADER_SIZE + optional_size;
    need(range(bytes, sections_offset, sections as u64 * SECTION_SIZE))?;
    let directory = optional + rva_count_offset + 4;
    let export_rva = need(u32_at(bytes, directory))?;
    let export_size = need(u32_at(bytes, directory + 4))?;
    let count = sections as u32;
    let at = |rva: u64, length: u64| need(address(bytes, sections_offset, count, rva, length));
    let exports = at(export_rva as u64, EXPORT_DIRECTORY_SIZE)?;
    let name_count = need(u32_at(bytes, exports + 24))?;
    let function_count = need(u32_at(bytes, exports + 20))?;
    if name_count == 0 || name_count > MAX_EXPORTS || function_count == 0 || function_count > MAX_EXPORTS {
        return Err(Error::Unsupported);
    }
    let table = |field: u64| need(u32_at(bytes, exports + field)).map(u64::from);
    Ok(Image {
        nt_offset: nt,
        sections_offset,
        export_rva,
        export_size,
        section_count: count,
        machine: machine as u32,
        names_offset: at(table(32)?, name_count as u64 * 4)?,
        ordinals_offset: at(table(36)?, name_count as u64 * 2)?,
        functions_offset: at(table(28)?, function_count as u64 * 4)?,
        name_count,
        function_count,
    })
}

// File offset of `length` bytes of the first export named exactly `name`.
fn export(bytes: &[u8], image: &Image, name: &[u8], length: u64) -> Result<u64> {
    let at = |rva: u64, count: u64| address(bytes, image.sections_offset, image.section_count, rva, count);
    let wanted = name.len() as u64 + 1;
    for index in 0..image.name_count as u64 {
        let name_rva = need(u32_at(bytes, image.names_offset + index * 4))?;
        let Some(candidate) = at(name_rva as u64, wanted) else { continue };
        let candidate = need(range(bytes, candidate, wanted))?;
        if &candidate[..name.len()] != name || candidate[name.len()] != 0 {
            continue;
        }
        let ordinal = need(u16_at(bytes, image.ordinals_offset + index * 2))? as u32;
        if ordinal >= image.function_count {
            return Err(Error::NotFound);
        }
        let rva = need(u32_at(bytes, image.functions_offset + ordinal as u64 * 4))? as u64;
        // Export forwarders are never owned Core code/data.
        if rva >= image.export_rva as u64 && rva < image.export_rva as u64 + image.export_size as u64 {
            return Err(Error::NotFound);
        }
        return at(rva, length).ok_or(Error::NotFound);
    }
    Err(Error::NotFound)
}

unsafe fn publish<T>(out: *mut T, result: Result<T>) -> u32 {
    if out.is_null() {
        return Error::InvalidArgument as u32;
    }
    match result {
        Ok(value) => {
            unsafe { out.write(value) };
            0
        }
        Err(error) => error as u32,
    }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_pe_image(data: *const u8, length: u64, machine: u32, out: *mut Image) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        image(bytes, u16::try_from(machine).map_err(|_| Error::InvalidArgument)?)
    })();
    unsafe { publish(out, result) }
}

/// Image comes from a successful dxmt_pe_image over the same immutable bytes.
/// `name` holds `name_length` bytes without the terminator.
#[no_mangle]
pub unsafe extern "C" fn dxmt_pe_export(
    data: *const u8, length: u64, image: *const Image, name: *const u8, name_length: u64, export_length: u64,
    out: *mut u64,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        let image = unsafe { image.as_ref() }.ok_or(Error::InvalidArgument)?;
        let name = unsafe { input(name, name_length) }.ok_or(Error::InvalidArgument)?;
        if name.is_empty() || name.contains(&0) {
            return Err(Error::InvalidArgument);
        }
        export(bytes, image, name, export_length)
    })();
    unsafe { publish(out, result) }
}
