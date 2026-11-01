// SPDX-License-Identifier: LGPL-2.1-or-later
use super::cache_wire::{input, output, put_u32, put_u64, range, u32_at, u64_at};

const MAGIC: u64 = u64::from_le_bytes(*b"D3ALSTG1");
const VERSION: u32 = 2;
const HEADER_SIZE: u64 = 128;
const IDENTITY_OFFSET: u64 = 56;
const IDENTITY_SIZE: u64 = 48;
const CHECKSUM_OFFSET: u64 = 104;
const CHECKSUM_SIZE: u64 = 20;
const ENTRY_LIMIT: u64 = 8 * 1024 * 1024;
const RELOCATION_SIZE: u64 = 16;

enum Error {
    InvalidArgument = 1,
    InvalidData = 2,
    OutputSize = 3,
}
type Result<T> = core::result::Result<T, Error>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct StageRange {
    offset: u64,
    size: u64,
}

#[repr(C)]
pub struct StageLayout {
    library: StageRange,
    metadata_sizes: StageRange,
    metadata: StageRange,
    constants: StageRange,
    relocations: StageRange,
    constant_alignment: u64,
    checksum_header: StageRange,
    checksum_payload: StageRange,
    checksum: StageRange,
}

#[repr(C)]
pub struct StageParts {
    key: *const u8,
    library: *const u8,
    metadata_sizes: *const u8,
    metadata: *const u8,
    constants: *const u8,
    relocations: *const u8,
    identity: *const u8,
    key_size: u64,
    library_size: u64,
    metadata_sizes_size: u64,
    metadata_size: u64,
    constants_size: u64,
    relocations_size: u64,
    constant_alignment: u64,
}

struct Parts<'a> {
    key: &'a [u8],
    library: &'a [u8],
    metadata_sizes: &'a [u8],
    metadata: &'a [u8],
    constants: &'a [u8],
    relocations: &'a [u8],
    identity: &'a [u8],
    constant_alignment: u64,
}

fn file_size(size: u64) -> Result<()> {
    if !(HEADER_SIZE..=ENTRY_LIMIT).contains(&size) {
        return Err(Error::InvalidData);
    }
    Ok(())
}

fn add(left: u64, right: u64) -> Result<u64> {
    left.checked_add(right).ok_or(Error::InvalidData)
}

fn region(offset: &mut u64, size: u64) -> Result<StageRange> {
    let start = *offset;
    *offset = add(start, size)?;
    Ok(StageRange { offset: start, size })
}

fn layout(key_size: u64, library_size: u64, metadata_sizes_size: u64, metadata_size: u64,
          constants_size: u64, relocations_size: u64, constant_alignment: u64) -> Result<StageLayout> {
    let mut offset = add(HEADER_SIZE, key_size)?;
    let library = region(&mut offset, library_size)?;
    offset = add(offset, 4)?;
    let metadata_sizes = region(&mut offset, metadata_sizes_size)?;
    let metadata = region(&mut offset, metadata_size)?;
    let constants = region(&mut offset, constants_size)?;
    let relocations = region(&mut offset, relocations_size)?;
    file_size(offset)?;
    Ok(StageLayout {
        library, metadata_sizes, metadata, constants, relocations, constant_alignment,
        checksum_header: StageRange { offset: 0, size: CHECKSUM_OFFSET },
        checksum_payload: StageRange { offset: HEADER_SIZE, size: offset - HEADER_SIZE },
        checksum: StageRange { offset: CHECKSUM_OFFSET, size: CHECKSUM_SIZE },
    })
}

fn metadata_lengths(sizes: &[u8], data_size: u64) -> Result<()> {
    let mut sum = 0;
    for offset in (0..sizes.len()).step_by(4) {
        sum = add(sum, u32_at(sizes, offset as u64).ok_or(Error::InvalidData)? as u64)?;
        if sum > data_size {
            return Err(Error::InvalidData);
        }
    }
    if sum != data_size {
        return Err(Error::InvalidData);
    }
    Ok(())
}

fn measure(parts: &Parts<'_>) -> Result<StageLayout> {
    if parts.key.is_empty() || parts.key.len() as u64 > u32::MAX as u64 ||
        parts.library.len() < 4 || parts.library.len() as u64 > u32::MAX as u64 ||
        parts.metadata.len() as u64 > u32::MAX as u64 ||
        parts.metadata_sizes.len() as u64 > u32::MAX as u64 || parts.metadata_sizes.len() % 4 != 0 ||
        parts.constants.len() as u64 > u32::MAX as u64 || parts.constants.len() % 8 != 0 ||
        parts.relocations.len() as u64 > u32::MAX as u64 || parts.relocations.len() % 16 != 0 {
        return Err(Error::InvalidData);
    }
    metadata_lengths(parts.metadata_sizes, parts.metadata.len() as u64)?;
    let encoded_metadata_size = add(add(4, parts.metadata_sizes.len() as u64)?, parts.metadata.len() as u64)?;
    if encoded_metadata_size > u32::MAX as u64 {
        return Err(Error::InvalidData);
    }
    if !parts.constant_alignment.is_power_of_two() ||
        parts.constants.is_empty() && parts.constant_alignment != 1 {
        return Err(Error::InvalidData);
    }
    // The old writer did not inspect library magic or relocation contents, and
    // accepted alignments above UINT32_MAX. Keep those reader-only checks below.
    layout(parts.key.len() as u64, parts.library.len() as u64, parts.metadata_sizes.len() as u64,
           parts.metadata.len() as u64, parts.constants.len() as u64, parts.relocations.len() as u64,
           parts.constant_alignment)
}

fn encode(parts: &Parts<'_>, destination: &mut [u8]) -> Result<StageLayout> {
    let info = measure(parts)?;
    if destination.len() as u64 != add(HEADER_SIZE, info.checksum_payload.size)? {
        return Err(Error::OutputSize);
    }
    let mut header = [0; HEADER_SIZE as usize];
    put_u64(&mut header, 0, MAGIC).ok_or(Error::InvalidData)?;
    put_u32(&mut header, 8, VERSION).ok_or(Error::InvalidData)?;
    put_u32(&mut header, 12, parts.key.len() as u32).ok_or(Error::InvalidData)?;
    put_u64(&mut header, 16, parts.library.len() as u64).ok_or(Error::InvalidData)?;
    put_u64(&mut header, 24, 4 + parts.metadata_sizes.len() as u64 + parts.metadata.len() as u64)
        .ok_or(Error::InvalidData)?;
    put_u64(&mut header, 32, parts.constants.len() as u64).ok_or(Error::InvalidData)?;
    put_u64(&mut header, 40, parts.constant_alignment).ok_or(Error::InvalidData)?;
    put_u64(&mut header, 48, parts.relocations.len() as u64).ok_or(Error::InvalidData)?;
    header[IDENTITY_OFFSET as usize..CHECKSUM_OFFSET as usize].copy_from_slice(parts.identity);
    let count = ((parts.metadata_sizes.len() / 4) as u32).to_le_bytes();
    let (destination_header, mut remaining) = destination.split_at_mut(HEADER_SIZE as usize);
    destination_header.copy_from_slice(&header);
    for bytes in [parts.key, parts.library, &count, parts.metadata_sizes, parts.metadata,
                  parts.constants, parts.relocations] {
        let (part, rest) = remaining.split_at_mut(bytes.len());
        part.copy_from_slice(bytes);
        remaining = rest;
    }
    Ok(info)
}

fn decode(bytes: &[u8], key: &[u8], identity: &[u8]) -> Result<StageLayout> {
    file_size(bytes.len() as u64)?;
    if u64_at(bytes, 0) != Some(MAGIC) || u32_at(bytes, 8) != Some(VERSION) ||
        u32_at(bytes, 12).map(u64::from) != Some(key.len() as u64) ||
        range(bytes, IDENTITY_OFFSET, IDENTITY_SIZE) != Some(identity) {
        return Err(Error::InvalidData);
    }
    let library_size = u64_at(bytes, 16).ok_or(Error::InvalidData)?;
    let metadata_size = u64_at(bytes, 24).ok_or(Error::InvalidData)?;
    let constants_size = u64_at(bytes, 32).ok_or(Error::InvalidData)?;
    let constant_alignment = u64_at(bytes, 40).ok_or(Error::InvalidData)?;
    let relocations_size = u64_at(bytes, 48).ok_or(Error::InvalidData)?;
    let payload = add(add(add(add(key.len() as u64, library_size)?, metadata_size)?, constants_size)?, relocations_size)?;
    if payload > ENTRY_LIMIT - HEADER_SIZE || add(HEADER_SIZE, payload)? != bytes.len() as u64 ||
        library_size < 4 || library_size > u32::MAX as u64 || metadata_size > u32::MAX as u64 ||
        !constant_alignment.is_power_of_two() || constant_alignment > u32::MAX as u64 ||
        constants_size == 0 && constant_alignment != 1 ||
        constants_size > u32::MAX as u64 || relocations_size > u32::MAX as u64 ||
        constants_size % 8 != 0 || relocations_size % RELOCATION_SIZE != 0 ||
        relocations_size > constants_size / 8 * RELOCATION_SIZE {
        return Err(Error::InvalidData);
    }
    if range(bytes, HEADER_SIZE, key.len() as u64) != Some(key) ||
        range(bytes, HEADER_SIZE + key.len() as u64, 4) != Some(&b"MTLB"[..]) || metadata_size < 4 {
        return Err(Error::InvalidData);
    }
    let metadata_offset = HEADER_SIZE + key.len() as u64 + library_size;
    let metadata_count = u32_at(bytes, metadata_offset).ok_or(Error::InvalidData)? as u64;
    let metadata_sizes_size = metadata_count * 4;
    if metadata_sizes_size > metadata_size - 4 {
        return Err(Error::InvalidData);
    }
    let info = layout(key.len() as u64, library_size, metadata_sizes_size,
                      metadata_size - 4 - metadata_sizes_size, constants_size, relocations_size, constant_alignment)?;
    metadata_lengths(range(bytes, info.metadata_sizes.offset, info.metadata_sizes.size).ok_or(Error::InvalidData)?,
                     info.metadata.size)?;
    let relocations = range(bytes, info.relocations.offset, info.relocations.size).ok_or(Error::InvalidData)?;
    for offset in (0..relocations.len()).step_by(RELOCATION_SIZE as usize) {
        let source = u64_at(relocations, offset as u64).ok_or(Error::InvalidData)?;
        let target = u64_at(relocations, offset as u64 + 8).ok_or(Error::InvalidData)?;
        if constants_size < 8 || source > constants_size - 8 ||
            target > constants_size && target != u64::MAX && target != u64::MAX - 1 {
            return Err(Error::InvalidData);
        }
    }
    Ok(info)
}

// Only these FFI adapters create borrowed slices or dereference caller pointers.
unsafe fn parts<'a>(value: *const StageParts) -> Result<Parts<'a>> {
    let value = unsafe { value.as_ref() }.ok_or(Error::InvalidArgument)?;
    Ok(Parts {
        key: unsafe { input(value.key, value.key_size) }.ok_or(Error::InvalidArgument)?,
        library: unsafe { input(value.library, value.library_size) }.ok_or(Error::InvalidArgument)?,
        metadata_sizes: unsafe { input(value.metadata_sizes, value.metadata_sizes_size) }.ok_or(Error::InvalidArgument)?,
        metadata: unsafe { input(value.metadata, value.metadata_size) }.ok_or(Error::InvalidArgument)?,
        constants: unsafe { input(value.constants, value.constants_size) }.ok_or(Error::InvalidArgument)?,
        relocations: unsafe { input(value.relocations, value.relocations_size) }.ok_or(Error::InvalidArgument)?,
        identity: unsafe { input(value.identity, IDENTITY_SIZE) }.ok_or(Error::InvalidArgument)?,
        constant_alignment: value.constant_alignment,
    })
}

unsafe fn publish<T>(out: *mut T, value: Result<T>) -> u32 {
    if out.is_null() {
        return Error::InvalidArgument as u32;
    }
    match value {
        Ok(value) => { unsafe { out.write(value) }; 0 }
        Err(error) => error as u32,
    }
}

#[no_mangle]
pub extern "C" fn d3dalloy_stage_cache_file_size(size: u64) -> u32 {
    match file_size(size) {
        Ok(()) => 0,
        Err(error) => error as u32,
    }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_stage_cache_measure(value: *const StageParts, out: *mut u64) -> u32 {
    let result = (|| {
        let parts = unsafe { parts(value) }?;
        let info = measure(&parts)?;
        add(HEADER_SIZE, info.checksum_payload.size)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_stage_cache_encode(value: *const StageParts, destination: *mut u8,
                                                destination_size: u64, out: *mut StageLayout) -> u32 {
    if out.is_null() {
        return Error::InvalidArgument as u32;
    }
    let result = (|| {
        let parts = unsafe { parts(value) }?;
        let destination = unsafe { output(destination, destination_size) }.ok_or(Error::InvalidArgument)?;
        encode(&parts, destination)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_stage_cache_decode(data: *const u8, size: u64, key: *const u8,
                                                key_size: u64, identity: *const u8, out: *mut StageLayout) -> u32 {
    let result = (|| {
        file_size(size)?;
        if key_size == 0 || key_size > u32::MAX as u64 {
            return Err(Error::InvalidArgument);
        }
        let bytes = unsafe { input(data, size) }.ok_or(Error::InvalidArgument)?;
        let key = unsafe { input(key, key_size) }.ok_or(Error::InvalidArgument)?;
        let identity = unsafe { input(identity, IDENTITY_SIZE) }.ok_or(Error::InvalidArgument)?;
        decode(bytes, key, identity)
    })();
    unsafe { publish(out, result) }
}
