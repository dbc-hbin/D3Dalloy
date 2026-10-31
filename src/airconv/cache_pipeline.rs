// SPDX-License-Identifier: LGPL-2.1-or-later
use super::cache_wire::{input, output, put_u32, put_u64, range, range_mut, u32_at, u64_at};

const MAGIC: u64 = 0x004f5350544d5844;
const VERSION: u32 = 7;
const HEADER_SIZE: u64 = 216;
const DIGEST_BEGIN: u64 = 100;
const DIGEST_END: u64 = 120;
const STAGE_COUNT: usize = 3;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct Digest {
    bytes: [u8; 20],
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct StageRecord {
    library_size: u64,
    constant_size: u64,
    constant_alignment: u64,
    relocation_count: u64,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct Description {
    adapter: u64,
    stages: u32,
    reserved: u32,
    identity: [u8; 48],
    key: Digest,
    reserved_tail: u32,
    records: [StageRecord; STAGE_COUNT],
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct StageOffsets {
    library: u64,
    constants: u64,
    relocations: u64,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct Layout {
    size: u64,
    digest_begin: u64,
    digest_end: u64,
    offsets: [StageOffsets; STAGE_COUNT],
}

#[repr(C)]
pub struct Info {
    description: Description,
    layout: Layout,
    digest: Digest,
    reserved: u32,
}

enum Error {
    InvalidArgument = 1,
    VersionMismatch = 2,
}

type Result<T, E = Error> = core::result::Result<T, E>;

fn header(bytes: &[u8], length: u64) -> Result<()> {
    if bytes.len() < HEADER_SIZE as usize || u64_at(bytes, 0) != Some(MAGIC) {
        return Err(Error::InvalidArgument);
    }
    if u32_at(bytes, 8) != Some(VERSION) {
        return Err(Error::VersionMismatch);
    }
    if u64_at(bytes, 16) != Some(length) {
        return Err(Error::InvalidArgument);
    }
    Ok(())
}

fn digest_at(bytes: &[u8], offset: u64) -> Result<Digest> {
    Ok(Digest {
        bytes: range(bytes, offset, 20)
            .ok_or(Error::InvalidArgument)?
            .try_into()
            .map_err(|_| Error::InvalidArgument)?,
    })
}

fn region(size: &mut u64, length: u64, limit: u64) -> Result<u64> {
    let offset = *size;
    let end = offset.checked_add(length).ok_or(Error::InvalidArgument)?;
    let aligned = end.checked_add(7).ok_or(Error::InvalidArgument)? & !7;
    if aligned > limit {
        return Err(Error::InvalidArgument);
    }
    *size = aligned;
    Ok(offset)
}

fn layout(description: &Description, limit: u64) -> Result<Layout> {
    if limit < HEADER_SIZE {
        return Err(Error::InvalidArgument);
    }
    let mut result = Layout {
        size: HEADER_SIZE,
        digest_begin: DIGEST_BEGIN,
        digest_end: DIGEST_END,
        offsets: [StageOffsets::default(); STAGE_COUNT],
    };
    for (i, record) in description.records.iter().enumerate() {
        if description.stages & (1 << i) == 0 {
            if record.library_size != 0 || record.constant_size != 0
                || record.constant_alignment != 0 || record.relocation_count != 0
            {
                return Err(Error::InvalidArgument);
            }
            continue;
        }
        if record.library_size < 4 || !record.constant_alignment.is_power_of_two()
            || record.constant_alignment > u32::MAX as u64
            || record.constant_size > u32::MAX as u64
            || record.relocation_count > record.constant_size / 8
            || record.constant_size == 0 && record.constant_alignment != 1
        {
            return Err(Error::InvalidArgument);
        }
        result.offsets[i] = StageOffsets {
            library: region(&mut result.size, record.library_size, limit)?,
            constants: region(&mut result.size, record.constant_size, limit)?,
            relocations: region(&mut result.size, record.relocation_count * 16, limit)?,
        };
    }
    Ok(result)
}

fn decode(bytes: &[u8]) -> Result<Info> {
    header(bytes, bytes.len() as u64)?;
    let mut description = Description {
        adapter: u64_at(bytes, 24).ok_or(Error::InvalidArgument)?,
        stages: u32_at(bytes, 12).ok_or(Error::InvalidArgument)?,
        reserved: 0,
        identity: range(bytes, 32, 48).ok_or(Error::InvalidArgument)?
            .try_into().map_err(|_| Error::InvalidArgument)?,
        key: digest_at(bytes, 80)?,
        reserved_tail: 0,
        records: [StageRecord::default(); STAGE_COUNT],
    };
    for (i, record) in description.records.iter_mut().enumerate() {
        let offset = DIGEST_END + i as u64 * 32;
        *record = StageRecord {
            library_size: u64_at(bytes, offset).ok_or(Error::InvalidArgument)?,
            constant_size: u64_at(bytes, offset + 8).ok_or(Error::InvalidArgument)?,
            constant_alignment: u64_at(bytes, offset + 16).ok_or(Error::InvalidArgument)?,
            relocation_count: u64_at(bytes, offset + 24).ok_or(Error::InvalidArgument)?,
        };
    }
    let layout = layout(&description, bytes.len() as u64)?;
    if layout.size != bytes.len() as u64 {
        return Err(Error::InvalidArgument);
    }
    Ok(Info { description, layout, digest: digest_at(bytes, DIGEST_BEGIN)?, reserved: 0 })
}

fn validate_payload(bytes: &[u8], info: &Info) -> Result<()> {
    for (i, record) in info.description.records.iter().enumerate() {
        if info.description.stages & (1 << i) == 0 {
            continue;
        }
        let offsets = &info.layout.offsets[i];
        if range(bytes, offsets.library, 4) != Some(b"MTLB") {
            return Err(Error::InvalidArgument);
        }
        for j in 0..record.relocation_count {
            let offset = offsets.relocations.checked_add(j.checked_mul(16)
                .ok_or(Error::InvalidArgument)?).ok_or(Error::InvalidArgument)?;
            let destination = u64_at(bytes, offset).ok_or(Error::InvalidArgument)?;
            let target = u64_at(bytes, offset.checked_add(8).ok_or(Error::InvalidArgument)?)
                .ok_or(Error::InvalidArgument)?;
            if destination > record.constant_size.checked_sub(8).ok_or(Error::InvalidArgument)?
                || target > record.constant_size && target != u64::MAX && target != u64::MAX - 1
            {
                return Err(Error::InvalidArgument);
            }
        }
    }
    Ok(())
}

fn begin(description: &Description, bytes: &mut [u8]) -> Result<()> {
    let layout = layout(description, bytes.len() as u64)?;
    if layout.size != bytes.len() as u64 {
        return Err(Error::InvalidArgument);
    }
    bytes.fill(0);
    let write = (|| {
        put_u64(bytes, 0, MAGIC)?;
        put_u32(bytes, 8, VERSION)?;
        put_u32(bytes, 12, description.stages)?;
        put_u64(bytes, 16, layout.size)?;
        put_u64(bytes, 24, description.adapter)?;
        range_mut(bytes, 32, 48)?.copy_from_slice(&description.identity);
        range_mut(bytes, 80, 20)?.copy_from_slice(&description.key.bytes);
        for (i, record) in description.records.iter().enumerate() {
            let offset = DIGEST_END + i as u64 * 32;
            put_u64(bytes, offset, record.library_size)?;
            put_u64(bytes, offset + 8, record.constant_size)?;
            put_u64(bytes, offset + 16, record.constant_alignment)?;
            put_u64(bytes, offset + 24, record.relocation_count)?;
        }
        Some(())
    })();
    write.ok_or(Error::InvalidArgument)
}

// All pointer-to-reference conversions and output publication stay at the ABI.
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
pub unsafe extern "C" fn dxmt_pipeline_cache_key(
    data: *const u8, length: u64, out: *mut Digest,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length.min(HEADER_SIZE)) }.ok_or(Error::InvalidArgument)?;
        header(bytes, length)?;
        digest_at(bytes, 80)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_pipeline_cache_decode(
    data: *const u8, length: u64, out: *mut Info,
) -> u32 {
    let result = (|| {
        let prefix = unsafe { input(data, length.min(HEADER_SIZE)) }.ok_or(Error::InvalidArgument)?;
        header(prefix, length)?;
        decode(unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_pipeline_cache_layout(
    description: *const Description, out: *mut Layout,
) -> u32 {
    let result = (|| {
        let description = unsafe { description.as_ref() }.ok_or(Error::InvalidArgument)?;
        layout(description, usize::MAX as u64)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_pipeline_cache_validate_payload(
    data: *const u8, length: u64, info: *const Info,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        let info = unsafe { info.as_ref() }.ok_or(Error::InvalidArgument)?;
        validate_payload(bytes, info)
    })();
    match result { Ok(()) => 0, Err(error) => error as u32 }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_pipeline_cache_begin(
    description: *const Description, data: *mut u8, length: u64,
) -> u32 {
    let result = (|| {
        let description = unsafe { description.as_ref() }.ok_or(Error::InvalidArgument)?;
        let bytes = unsafe { output(data, length) }.ok_or(Error::InvalidArgument)?;
        begin(description, bytes)
    })();
    match result { Ok(()) => 0, Err(error) => error as u32 }
}

#[no_mangle]
pub unsafe extern "C" fn dxmt_pipeline_cache_set_digest(
    data: *mut u8, length: u64, digest: *const Digest,
) -> u32 {
    let result: Result<()> = (|| {
        let digest = unsafe { digest.as_ref() }.ok_or(Error::InvalidArgument)?;
        let bytes = unsafe { output(data, length) }.ok_or(Error::InvalidArgument)?;
        header(bytes, length)?;
        range_mut(bytes, DIGEST_BEGIN, 20).ok_or(Error::InvalidArgument)?
            .copy_from_slice(&digest.bytes);
        Ok(())
    })();
    match result { Ok(()) => 0, Err(error) => error as u32 }
}
