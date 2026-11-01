// SPDX-License-Identifier: LGPL-2.1-or-later
// Serialized acceleration structures read back from an application buffer for
// CopyRaytracingAccelerationStructure(DESERIALIZE): the 56-byte D3D12 header, an
// optional 16-byte OMM block plus its pointers, the 64-byte d3dalloy header at
// header_offset, then the shadow copy whose leading records are 96-byte geometries.
// Offsets of the returned spans are relative to the deserialized structure.
use super::cache_wire::{input, range, u32_at, u64_at};

const SERIALIZED_HEADER_SIZE: u64 = 56;
const HEADER_SIZE: u64 = 64;
const BLOCK_SIZE: u64 = 16;
const MINIMUM_SIZE: u64 = SERIALIZED_HEADER_SIZE + HEADER_SIZE;
const GEOMETRY_SIZE: u64 = 96;
const INSTANCE_SIZE: u64 = 64;
const TRANSFORM_SIZE: u64 = 48;
const INDEX_SIZE: u64 = 4;
const HEADER_MAGIC: u32 = 0x5341_5844; // "DXAS"
// NumBottomLevelAccelerationStructurePointersAfterHeader of a header followed by one block.
const BLOCKS_MARKER: u64 = 0xffff_ffff_0000_0001;
const HAS_TRANSFORM: u32 = 0x8000_0000;

const TOP_LEVEL: u32 = 0;
const BOTTOM_LEVEL: u32 = 1;
const MICROMAP_ARRAY: u32 = 2;
const TRIANGLES: u32 = 0;
const AABBS: u32 = 1;
const OMM_TRIANGLES: u32 = 2;

const PRESENT_DATA: u32 = 1;
const PRESENT_INDICES: u32 = 2;
const PRESENT_TRANSFORM: u32 = 4;
const PRESENT_OMM_INDICES: u32 = 8;

enum Error {
    InvalidArgument = 1,
    InvalidData = 2,
}
type Result<T> = core::result::Result<T, Error>;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct Layout {
    serialized_size: u64,
    header_offset: u64,
    blocks: u32,
    reserved: u32,
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct Span {
    offset: u64,
    size: u64,
}

#[repr(C)]
pub struct Info {
    header_offset: u64,
    shadow_size: u64,
    deserialized_size: u64,
    instances: Span,
    kind: u32,
    flags: u32,
    desc_count: u32,
    reserved: u32,
}

#[derive(Default)]
#[repr(C)]
pub struct Geometry {
    data: Span,
    indices: Span,
    transform: Span,
    omm_indices: Span,
    present: u32,
    reserved: u32,
}

fn field32(bytes: &[u8], offset: u64) -> Result<u32> {
    u32_at(bytes, offset).ok_or(Error::InvalidArgument)
}

fn field64(bytes: &[u8], offset: u64) -> Result<u64> {
    u64_at(bytes, offset).ok_or(Error::InvalidArgument)
}

fn check(okay: bool) -> Result<()> {
    if okay { Ok(()) } else { Err(Error::InvalidData) }
}

fn blocks(pointers: u64) -> bool {
    (pointers >> 32) as u32 == u32::MAX
}

// The serialized header against the readable source range. Without blocks the
// d3dalloy header follows the bottom-level pointers.
fn prefix(serialized: &[u8], available: u64) -> Result<Layout> {
    let size = field64(serialized, 32)?;
    let pointers = field64(serialized, 48)?;
    check(size >= MINIMUM_SIZE && size <= available)?;
    if blocks(pointers) {
        check(pointers == BLOCKS_MARKER)?;
        return Ok(Layout { serialized_size: size, header_offset: 0, blocks: 1, reserved: 0 });
    }
    check(pointers <= (size - MINIMUM_SIZE) / 8)?;
    Ok(Layout { serialized_size: size, header_offset: SERIALIZED_HEADER_SIZE + pointers * 8, blocks: 0, reserved: 0 })
}

// D3D12_RAYTRACING_SERIALIZED_BLOCK: Type at 0, NumBlockPointersAfterHeader at 8.
fn block(layout: &Layout, block: &[u8]) -> Result<Layout> {
    if layout.blocks == 0 {
        return Err(Error::InvalidArgument);
    }
    check(field32(block, 0)? == 0)?;
    let pointers = field64(block, 8)?;
    let room = layout.serialized_size.checked_sub(MINIMUM_SIZE + BLOCK_SIZE).ok_or(Error::InvalidData)?;
    check(pointers <= room / 8)?;
    Ok(Layout { header_offset: SERIALIZED_HEADER_SIZE + BLOCK_SIZE + pointers * 8, ..*layout })
}

fn tail(layout: &Layout) -> Result<u64> {
    layout.serialized_size.checked_sub(layout.header_offset)
        .and_then(|n| n.checked_sub(HEADER_SIZE)).ok_or(Error::InvalidArgument)
}

// Bytes of geometry records after the d3dalloy header; top levels have none.
fn geometry_bytes(layout: &Layout, header: &[u8]) -> Result<u64> {
    let kind = field32(header, 20)?;
    let count = field32(header, 28)? as u64;
    let bytes = if kind != TOP_LEVEL { count * GEOMETRY_SIZE } else { 0 };
    check(bytes <= tail(layout)?)?;
    Ok(bytes)
}

// A range of `count` elements of `stride` bytes inside [HEADER_SIZE, limit).
fn location(limit: u64, offset: u64, count: u64, stride: u64) -> Result<Span> {
    let size = count.checked_mul(stride).ok_or(Error::InvalidData)?;
    check(offset >= HEADER_SIZE && offset <= limit && size <= limit - offset)?;
    Ok(Span { offset, size })
}

fn record(geometry: &[u8], index: u32) -> Result<&[u8]> {
    range(geometry, index as u64 * GEOMETRY_SIZE, GEOMETRY_SIZE).ok_or(Error::InvalidArgument)
}

// One ShadowGeometry of a bottom level or micromap array. Formats other than the
// OMM array's are validated by the caller's vertex-format table.
fn spans(record: &[u8], kind: u32, limit: u64) -> Result<Geometry> {
    let kind_of = field32(record, 0)?;
    let vertex_format = field32(record, 8)?;
    let index_format = field32(record, 12)?;
    let vertex_count = field32(record, 16)?;
    let data_offset = field64(record, 32)?;
    let mut result = Geometry { present: PRESENT_DATA, ..Default::default() };
    if kind == MICROMAP_ARRAY {
        check(kind_of == OMM_TRIANGLES && index_format <= 12 && (1..=2).contains(&vertex_format))?;
        // Packed states of 4^subdivision microtriangles at 1 or 2 bits each.
        check(vertex_count == ((1u32 << (2 * index_format)) * vertex_format + 7) / 8)?;
        result.data = location(limit, data_offset, 1, vertex_count as u64)?;
        return Ok(result);
    }
    let index_count = field32(record, 20)? as u64;
    let primitives = field32(record, 24)? as u64;
    let stride_and_transform = field32(record, 28)?;
    let stride = (stride_and_transform & !HAS_TRANSFORM) as u64;
    if kind_of == TRIANGLES || kind_of == OMM_TRIANGLES {
        if index_format != 0 {
            let count = if primitives != 0 { index_count } else { 0 };
            result.indices = location(limit, field64(record, 40)?, count, INDEX_SIZE)?;
            result.present |= PRESENT_INDICES;
        }
        if kind_of == OMM_TRIANGLES && field32(record, 84)? != 0 {
            result.omm_indices = location(limit, field64(record, 72)?, primitives, field32(record, 80)? as u64)?;
            result.present |= PRESENT_OMM_INDICES;
        }
    } else {
        check(kind_of == AABBS)?;
    }
    let elements = match (primitives, kind_of) {
        (0, _) => 0,
        (_, AABBS) => primitives,
        _ => vertex_count as u64,
    };
    result.data = location(limit, data_offset, elements, stride)?;
    if stride_and_transform & HAS_TRANSFORM != 0 {
        let count = if primitives != 0 { 1 } else { 0 };
        result.transform = location(limit, field64(record, 48)?, count, TRANSFORM_SIZE)?;
        result.present |= PRESENT_TRANSFORM;
    }
    Ok(result)
}

// Every structural rule of a serialized structure. The driver identifier is
// the caller's; `layout` comes from prefix/block over these same header bytes.
fn parse(serialized: &[u8], layout: &Layout, header: &[u8], geometry: &[u8], destination: u64) -> Result<Info> {
    let size = field64(serialized, 32)?;
    let deserialized = field64(serialized, 40)?;
    let pointers = field64(serialized, 48)?;
    if layout.serialized_size != size || (layout.blocks != 0) != blocks(pointers) {
        return Err(Error::InvalidArgument);
    }
    let magic = field32(header, 16)?;
    let kind = field32(header, 20)?;
    let flags = field32(header, 24)?;
    let count = field32(header, 28)?;
    let shadow = field64(header, 32)?;
    let reserved0 = field32(header, 52)?;
    let header_deserialized = field64(header, 56)?;
    let header_offset = if layout.blocks != 0 {
        check(geometry.len() as u64 >= count as u64 * GEOMETRY_SIZE)?;
        let mut omm_pointers = 0u64;
        for index in 0..count {
            omm_pointers += (field64(record(geometry, index)?, 64)? != 0) as u64;
        }
        check(pointers == BLOCKS_MARKER && kind == BOTTOM_LEVEL && reserved0 != 0 && omm_pointers != 0 &&
              omm_pointers == reserved0 as u64 - 1 &&
              layout.header_offset == SERIALIZED_HEADER_SIZE + BLOCK_SIZE + omm_pointers * 8)?;
        layout.header_offset
    } else {
        pointers.checked_mul(8).and_then(|n| n.checked_add(SERIALIZED_HEADER_SIZE)).ok_or(Error::InvalidData)?
    };
    check(header_offset <= size && size - header_offset >= HEADER_SIZE)?;
    check(magic == HEADER_MAGIC && kind <= MICROMAP_ARRAY && shadow <= size - header_offset - HEADER_SIZE &&
          header_deserialized <= destination && header_deserialized >= HEADER_SIZE + shadow &&
          size == header_offset + HEADER_SIZE + shadow && deserialized == header_deserialized &&
          (layout.blocks != 0 || pointers == if kind == TOP_LEVEL { count as u64 } else { 0 }))?;
    let limit = HEADER_SIZE + shadow;
    let mut info = Info {
        header_offset,
        shadow_size: shadow,
        deserialized_size: header_deserialized,
        instances: Span::default(),
        kind,
        flags,
        desc_count: count,
        reserved: 0,
    };
    if kind == TOP_LEVEL {
        info.instances = location(limit, HEADER_SIZE, count as u64, INSTANCE_SIZE)?;
        return Ok(info);
    }
    check(count as u64 <= shadow / GEOMETRY_SIZE && count as u64 <= geometry.len() as u64 / GEOMETRY_SIZE)?;
    for index in 0..count {
        spans(record(geometry, index)?, kind, limit)?;
    }
    Ok(info)
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
pub unsafe extern "C" fn d3dalloy_rtas_serialized_prefix(
    serialized: *const u8, length: u64, available: u64, out: *mut Layout,
) -> u32 {
    let result = (|| prefix(unsafe { input(serialized, length) }.ok_or(Error::InvalidArgument)?, available))();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_rtas_serialized_block(
    layout: *const Layout, data: *const u8, length: u64, out: *mut Layout,
) -> u32 {
    let result = (|| {
        let layout = unsafe { layout.as_ref() }.ok_or(Error::InvalidArgument)?;
        block(layout, unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_rtas_serialized_geometry_bytes(
    layout: *const Layout, header: *const u8, length: u64, out: *mut u64,
) -> u32 {
    let result = (|| {
        let layout = unsafe { layout.as_ref() }.ok_or(Error::InvalidArgument)?;
        geometry_bytes(layout, unsafe { input(header, length) }.ok_or(Error::InvalidArgument)?)
    })();
    unsafe { publish(out, result) }
}

#[no_mangle]
pub unsafe extern "C" fn d3dalloy_rtas_serialized_parse(
    serialized: *const u8, serialized_length: u64, layout: *const Layout, header: *const u8, header_length: u64,
    geometry: *const u8, geometry_length: u64, destination_available: u64, out: *mut Info,
) -> u32 {
    let result = (|| {
        let serialized = unsafe { input(serialized, serialized_length) }.ok_or(Error::InvalidArgument)?;
        let layout = unsafe { layout.as_ref() }.ok_or(Error::InvalidArgument)?;
        let header = unsafe { input(header, header_length) }.ok_or(Error::InvalidArgument)?;
        let geometry = unsafe { input(geometry, geometry_length) }.ok_or(Error::InvalidArgument)?;
        parse(serialized, layout, header, geometry, destination_available)
    })();
    unsafe { publish(out, result) }
}

/// Info comes from a successful parse of these same immutable geometry records.
#[no_mangle]
pub unsafe extern "C" fn d3dalloy_rtas_serialized_geometry(
    info: *const Info, geometry: *const u8, length: u64, index: u32, out: *mut Geometry,
) -> u32 {
    let result = (|| {
        let info = unsafe { info.as_ref() }.ok_or(Error::InvalidArgument)?;
        let geometry = unsafe { input(geometry, length) }.ok_or(Error::InvalidArgument)?;
        if info.kind == TOP_LEVEL || info.kind > MICROMAP_ARRAY || index >= info.desc_count {
            return Err(Error::InvalidArgument);
        }
        let limit = HEADER_SIZE.checked_add(info.shadow_size).ok_or(Error::InvalidArgument)?;
        spans(record(geometry, index)?, info.kind, limit)
    })();
    unsafe { publish(out, result) }
}
