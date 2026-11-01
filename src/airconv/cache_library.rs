// SPDX-License-Identifier: LGPL-2.1-or-later
// ID3D12PipelineLibrary outer container: 104-byte header, then per entry a
// 40-byte record, the UTF-16 name and the PSO blob, each padded to 8 bytes.
use super::cache_wire::{input, output, put_u32, put_u64, range, range_mut, u32_at, u64_at};

const MAGIC: u64 = u64::from_le_bytes(*b"D3ALPLB\0");
const VERSION: u32 = 1;
const HEADER_SIZE: u64 = 104;
const RECORD_SIZE: u64 = 40;
const DIGEST_BEGIN: u64 = 80;
const KIND_COMPUTE: u32 = 1;

enum Error {
    InvalidArgument = 1,
    VersionMismatch = 2,
    Overflow = 3,
}
type Result<T> = core::result::Result<T, Error>;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct Header {
    adapter: u64,
    size: u64,
    count: u32,
    reserved: u32,
    identity: [u8; 48],
    digest: [u8; 20],
    reserved_tail: u32,
}

#[repr(C)]
pub struct Entry {
    name_offset: u64,
    blob_offset: u64,
    blob_size: u64,
    next_offset: u64,
    name_bytes: u32,
    kind: u32,
    key: [u8; 20],
    reserved: u32,
}

#[repr(C)]
pub struct Extent {
    size: u64,
    count: u64,
}

#[repr(C)]
pub struct Record {
    name_bytes: u64,
    blob_size: u64,
    kind: u32,
    reserved: u32,
    key: [u8; 20],
    reserved_tail: u32,
}

fn pad8(value: u64) -> Result<u64> {
    Ok(value.checked_add(7).ok_or(Error::Overflow)? & !7)
}

fn entry_size(name_bytes: u64, blob_size: u64) -> Result<u64> {
    if name_bytes > u32::MAX as u64 || name_bytes & 1 != 0 {
        return Err(Error::InvalidArgument);
    }
    RECORD_SIZE
        .checked_add(pad8(name_bytes)?)
        .and_then(|n| n.checked_add(pad8(blob_size).ok()?))
        .ok_or(Error::Overflow)
}

fn extend(extent: &Extent, name_bytes: u64, blob_size: u64) -> Result<Extent> {
    if extent.size < HEADER_SIZE || extent.count >= u32::MAX as u64 || blob_size == 0 {
        return Err(Error::InvalidArgument);
    }
    let size = extent.size.checked_add(entry_size(name_bytes, blob_size)?).ok_or(Error::Overflow)?;
    // The serialized library must be addressable by the caller.
    if size > isize::MAX as u64 {
        return Err(Error::Overflow);
    }
    Ok(Extent { size, count: extent.count + 1 })
}

// Magic precedes version, and version precedes size and reserved fields.
fn header(bytes: &[u8]) -> Result<Header> {
    if bytes.len() < HEADER_SIZE as usize || u64_at(bytes, 0) != Some(MAGIC) {
        return Err(Error::InvalidArgument);
    }
    if u32_at(bytes, 8) != Some(VERSION) {
        return Err(Error::VersionMismatch);
    }
    let field = |offset| u32_at(bytes, offset).ok_or(Error::InvalidArgument);
    if u64_at(bytes, 16) != Some(bytes.len() as u64) || field(100)? != 0 {
        return Err(Error::InvalidArgument);
    }
    let copy = |offset, size| range(bytes, offset, size).ok_or(Error::InvalidArgument);
    Ok(Header {
        adapter: u64_at(bytes, 24).ok_or(Error::InvalidArgument)?,
        size: bytes.len() as u64,
        count: field(12)?,
        reserved: 0,
        identity: copy(32, 48)?.try_into().map_err(|_| Error::InvalidArgument)?,
        digest: copy(DIGEST_BEGIN, 20)?.try_into().map_err(|_| Error::InvalidArgument)?,
        reserved_tail: 0,
    })
}

fn entry(bytes: &[u8], offset: u64) -> Result<Entry> {
    if offset < HEADER_SIZE {
        return Err(Error::InvalidArgument);
    }
    let record = range(bytes, offset, RECORD_SIZE).ok_or(Error::InvalidArgument)?;
    let name_bytes = u32_at(record, 0).ok_or(Error::InvalidArgument)?;
    let kind = u32_at(record, 4).ok_or(Error::InvalidArgument)?;
    let blob_size = u64_at(record, 8).ok_or(Error::InvalidArgument)?;
    if name_bytes & 1 != 0 || u32_at(record, 36) != Some(0) || kind > KIND_COMPUTE || blob_size == 0 {
        return Err(Error::InvalidArgument);
    }
    let name_offset = offset + RECORD_SIZE;
    let blob_offset = name_offset.checked_add(pad8(name_bytes as u64)?).ok_or(Error::InvalidArgument)?;
    let next_offset = blob_offset.checked_add(pad8(blob_size).map_err(|_| Error::InvalidArgument)?)
        .ok_or(Error::InvalidArgument)?;
    if next_offset > bytes.len() as u64 {
        return Err(Error::InvalidArgument);
    }
    Ok(Entry {
        name_offset,
        blob_offset,
        blob_size,
        next_offset,
        name_bytes,
        kind,
        key: range(record, 16, 20).ok_or(Error::InvalidArgument)?.try_into().map_err(|_| Error::InvalidArgument)?,
        reserved: 0,
    })
}

fn begin(description: &Header, bytes: &mut [u8]) -> Result<()> {
    if bytes.len() as u64 != description.size || description.size < HEADER_SIZE {
        return Err(Error::InvalidArgument);
    }
    bytes.fill(0);
    (|| {
        put_u64(bytes, 0, MAGIC)?;
        put_u32(bytes, 8, VERSION)?;
        put_u32(bytes, 12, description.count)?;
        put_u64(bytes, 16, description.size)?;
        put_u64(bytes, 24, description.adapter)?;
        range_mut(bytes, 32, 48)?.copy_from_slice(&description.identity);
        Some(())
    })()
    .ok_or(Error::InvalidArgument)
}

fn put_entry(bytes: &mut [u8], offset: u64, record: &Record) -> Result<Entry> {
    if offset < HEADER_SIZE || record.kind > KIND_COMPUTE || record.blob_size == 0 {
        return Err(Error::InvalidArgument);
    }
    let size = entry_size(record.name_bytes, record.blob_size)?;
    let next_offset = offset.checked_add(size).ok_or(Error::Overflow)?;
    if next_offset > bytes.len() as u64 {
        return Err(Error::InvalidArgument);
    }
    (|| {
        put_u32(bytes, offset, record.name_bytes as u32)?;
        put_u32(bytes, offset + 4, record.kind)?;
        put_u64(bytes, offset + 8, record.blob_size)?;
        range_mut(bytes, offset + 16, 20)?.copy_from_slice(&record.key);
        Some(())
    })()
    .ok_or(Error::InvalidArgument)?;
    let name_offset = offset + RECORD_SIZE;
    Ok(Entry {
        name_offset,
        blob_offset: name_offset + pad8(record.name_bytes)?,
        blob_size: record.blob_size,
        next_offset,
        name_bytes: record.name_bytes as u32,
        kind: record.kind,
        key: record.key,
        reserved: 0,
    })
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
pub unsafe extern "C" fn d3dalloy_pipeline_library_extend(
    extent: *const Extent, name_bytes: u64, blob_size: u64, out: *mut Extent,
) -> u32 {
    let result = (|| extend(unsafe { extent.as_ref() }.ok_or(Error::InvalidArgument)?, name_bytes, blob_size))();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_pipeline_library_header(data: *const u8, length: u64, out: *mut Header) -> u32 {
    let result = (|| header(unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?))();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_pipeline_library_entry(
    data: *const u8, length: u64, offset: u64, out: *mut Entry,
) -> u32 {
    let result = (|| entry(unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?, offset))();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_pipeline_library_begin(description: *const Header, data: *mut u8, length: u64) -> u32 {
    let result = (|| {
        let description = unsafe { description.as_ref() }.ok_or(Error::InvalidArgument)?;
        begin(description, unsafe { output(data, length) }.ok_or(Error::InvalidArgument)?)
    })();
    match result { Ok(()) => 0, Err(error) => error as u32 }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_pipeline_library_put_entry(
    data: *mut u8, length: u64, offset: u64, record: *const Record, out: *mut Entry,
) -> u32 {
    let result = (|| {
        let record = unsafe { record.as_ref() }.ok_or(Error::InvalidArgument)?;
        if out.is_null() {
            return Err(Error::InvalidArgument);
        }
        put_entry(unsafe { output(data, length) }.ok_or(Error::InvalidArgument)?, offset, record)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_pipeline_library_set_digest(data: *mut u8, length: u64, digest: *const u8) -> u32 {
    let result: Result<()> = (|| {
        let digest = unsafe { input(digest, 20) }.ok_or(Error::InvalidArgument)?;
        let bytes = unsafe { output(data, length) }.ok_or(Error::InvalidArgument)?;
        header(bytes)?;
        range_mut(bytes, DIGEST_BEGIN, 20).ok_or(Error::InvalidArgument)?.copy_from_slice(digest);
        Ok(())
    })();
    match result { Ok(()) => 0, Err(error) => error as u32 }
}
