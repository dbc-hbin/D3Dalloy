// SPDX-License-Identifier: LGPL-2.1-or-later
// D3D12_PIPELINE_STATE_STREAM_DESC walk. Each subobject is
// `struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T data; }`, bounded by its
// own size and followed by the next at that size rounded up to sizeof(void *).
// The caller supplies sizeof/alignof(T) per handled type, so payload layouts
// stay those of the guest ABI the archive is built for.
use super::cache_wire::{input, u32_at};

const POINTER: u64 = core::mem::size_of::<usize>() as u64;
const TYPES: usize = 64;

enum Error {
    InvalidArgument = 1,
    Truncated = 2,
    UnknownType = 3,
    Duplicate = 4,
}
type Result<T> = core::result::Result<T, Error>;

// Revisions of one state share a definition: depth-stencil 0/1/2, rasterizer
// 0/1/2, and root signature object/serialized blob.
const DEPTH_STENCIL_TYPES: u64 = 1 << 11 | 1 << 21 | 1 << 26;
const RASTERIZER_TYPES: u64 = 1 << 10 | 1 << 27 | 1 << 28;
const ROOT_TYPES: u64 = 1 << 0 | 1 << 29;

// Zero size marks a type the runtime does not handle.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct Layout {
    size: u32,
    alignment: u32,
}

#[repr(C)]
pub struct Subobject {
    payload_offset: u64,
    next_offset: u64,
    defined: u64,
    kind: u32,
    reserved: u32,
}

fn align(value: u64, alignment: u64) -> Option<u64> {
    Some(value.checked_add(alignment - 1)? & !(alignment - 1))
}

// The subobject at `offset` of a stream with `defined` types already seen. Type
// range precedes the duplicate group, which precedes handled type and bounds.
fn next(bytes: &[u8], layouts: &[Layout; TYPES], offset: u64, defined: u64) -> Result<Subobject> {
    let kind = u32_at(bytes, offset).ok_or(Error::Truncated)?;
    if kind as usize >= TYPES {
        return Err(Error::UnknownType);
    }
    let bit = 1u64 << kind;
    let group = [DEPTH_STENCIL_TYPES, RASTERIZER_TYPES, ROOT_TYPES]
        .into_iter().find(|group| bit & group != 0).unwrap_or(bit);
    if defined & group != 0 {
        return Err(Error::Duplicate);
    }
    let layout = layouts[kind as usize];
    if layout.size == 0 {
        return Err(Error::UnknownType);
    }
    let alignment = (layout.alignment as u64).max(4);
    if !alignment.is_power_of_two() {
        return Err(Error::InvalidArgument);
    }
    let payload = align(4, alignment).ok_or(Error::InvalidArgument)?;
    let size = align(payload + layout.size as u64, alignment).ok_or(Error::InvalidArgument)?;
    if offset.checked_add(size).map_or(true, |end| end > bytes.len() as u64) {
        return Err(Error::Truncated);
    }
    Ok(Subobject {
        payload_offset: offset + payload,
        // At or past the end after the last subobject, which ends the walk.
        next_offset: offset + align(size, POINTER).ok_or(Error::InvalidArgument)?,
        defined: defined | bit,
        kind,
        reserved: 0,
    })
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_pso_stream_next(
    data: *const u8, length: u64, layouts: *const Layout, offset: u64, defined: u64, out: *mut Subobject,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        let layouts = unsafe { layouts.cast::<[Layout; TYPES]>().as_ref() }.ok_or(Error::InvalidArgument)?;
        if out.is_null() || offset >= bytes.len() as u64 {
            return Err(Error::InvalidArgument);
        }
        next(bytes, layouts, offset, defined)
    })();
    match result {
        Ok(value) => {
            unsafe { out.write(value) };
            0
        }
        Err(error) => error as u32,
    }
}
