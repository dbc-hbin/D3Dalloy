// SPDX-License-Identifier: LGPL-2.1-or-later
// DXBC ISGN/OSGN/OSG5/ISG1/OSG1/PCSG/PSG1 payload validation. C++ allocates and
// copies only after every record, register order, stream group and name is checked.
use super::cache_wire::{input, range, u32_at};

const STREAM_COUNT: usize = 4;

enum Error {
    InvalidArgument = 1,
    Header = 2,
    Records = 3,
    Name = 4,
    RegisterOrder = 5,
    StreamOrder = 6,
}
type Result<T, E = Error> = core::result::Result<T, E>;

#[derive(Clone, Copy)]
enum Format {
    // D3D10_INTERNALSHADER_PARAMETER: ISGN, OSGN, PCSG.
    Legacy,
    // D3D11_INTERNALSHADER_PARAMETER_FOR_GS: OSG5.
    Stream,
    // D3D11_INTERNALSHADER_PARAMETER_11_1: ISG1, OSG1, PSG1.
    Stream11_1,
}

impl Format {
    fn from_abi(value: u32) -> Result<Self> {
        match value {
            0 => Ok(Self::Legacy),
            1 => Ok(Self::Stream),
            2 => Ok(Self::Stream11_1),
            _ => Err(Error::InvalidArgument),
        }
    }

    fn stride(self) -> u64 {
        match self {
            Self::Legacy => 24,
            Self::Stream => 28,
            Self::Stream11_1 => 32,
        }
    }
}

#[repr(C)]
pub struct SignatureInfo {
    parameter_count: u32,
    records_offset: u32,
    format: u32,
    stream_count: u32,
    stream_parameters: [u32; STREAM_COUNT],
    // Sum of every name length plus its terminator.
    string_bytes: u64,
}

#[repr(C)]
pub struct SignatureParameter {
    stream: u32,
    semantic_index: u32,
    system_value: u32,
    component_type: u32,
    register_index: u32,
    mask: u32,
    never_writes_mask: u32,
    min_precision: u32,
    name_offset: u32,
    name_size: u32,
}

fn name(bytes: &[u8], offset: u32) -> Result<u32> {
    let suffix = bytes.get(offset as usize..).ok_or(Error::Name)?;
    let length = suffix.iter().position(|&byte| byte == 0).ok_or(Error::Name)?;
    Ok(length as u32)
}

fn record(bytes: &[u8], format: Format, records_offset: u32, index: u32) -> Result<SignatureParameter> {
    let offset = (index as u64)
        .checked_mul(format.stride())
        .and_then(|n| n.checked_add(records_offset as u64))
        .ok_or(Error::Records)?;
    let record = range(bytes, offset, format.stride()).ok_or(Error::Records)?;
    let field = |offset: u64| u32_at(record, offset).ok_or(Error::Records);
    let base = if matches!(format, Format::Legacy) { 0 } else { 4 };
    let name_offset = field(base)?;
    Ok(SignatureParameter {
        stream: if matches!(format, Format::Legacy) { 0 } else { field(0)? },
        semantic_index: field(base + 4)?,
        system_value: field(base + 8)?,
        component_type: field(base + 12)?,
        register_index: field(base + 16)?,
        mask: record[base as usize + 20] as u32,
        never_writes_mask: record[base as usize + 21] as u32,
        min_precision: if matches!(format, Format::Stream11_1) { field(28)? } else { 0 },
        name_offset,
        name_size: name(bytes, name_offset)?,
    })
}

fn parse(bytes: &[u8], format: Format, grouped: bool) -> Result<SignatureInfo> {
    if bytes.len() < 8 {
        return Err(Error::Header);
    }
    let count = u32_at(bytes, 0).ok_or(Error::Header)?;
    let records_offset = u32_at(bytes, 4).ok_or(Error::Header)?;
    let mut info = SignatureInfo {
        parameter_count: count,
        records_offset,
        format: format as u32,
        stream_count: 0,
        stream_parameters: [0; STREAM_COUNT],
        string_bytes: 0,
    };
    // An empty signature does not describe a record table.
    if count == 0 {
        return Ok(info);
    }
    let table = (count as u64).checked_mul(format.stride()).ok_or(Error::Records)?;
    // The legacy parser also required the header plus table to fit the blob.
    range(bytes, 8, table).ok_or(Error::Records)?;
    range(bytes, records_offset as u64, table).ok_or(Error::Records)?;
    let mut i = 0;
    // Grouped parsers expose one signature per stream. Records appear in
    // nondecreasing stream order 0..3 and registers are nondecreasing within
    // each stream; ungrouped parsers apply the register rule to the whole table.
    for stream in 0..if grouped { STREAM_COUNT } else { 1 } {
        let first = i;
        let mut last_register = 0;
        while i < count {
            let parameter = record(bytes, format, records_offset, i)?;
            if grouped && parameter.stream != stream as u32 {
                break;
            }
            if i > first && last_register > parameter.register_index {
                return Err(Error::RegisterOrder);
            }
            last_register = parameter.register_index;
            info.stream_parameters[stream] += 1;
            info.stream_count = stream as u32 + 1;
            info.string_bytes += parameter.name_size as u64 + 1;
            i += 1;
        }
    }
    // Remaining records have a stream outside 0..3 or out of order. Valid DXBC
    // cannot contain them: D3D11 has four streams and fxc emits them sorted.
    if i != count {
        return Err(Error::StreamOrder);
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
pub unsafe extern "C" fn d3dalloy_dxbc_signature_parse(
    data: *const u8, length: u64, format: u32, grouped: u32, out: *mut SignatureInfo,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        parse(bytes, Format::from_abi(format)?, grouped != 0)
    })();
    unsafe { publish(out, result) }
}

/// Info comes from a successful parse of this same immutable input.
#[no_mangle]
pub unsafe extern "C" fn d3dalloy_dxbc_signature_parameter(
    data: *const u8, length: u64, info: *const SignatureInfo, index: u32, out: *mut SignatureParameter,
) -> u32 {
    let result = (|| {
        let bytes = unsafe { input(data, length) }.ok_or(Error::InvalidArgument)?;
        let info = unsafe { info.as_ref() }.ok_or(Error::InvalidArgument)?;
        if index >= info.parameter_count {
            return Err(Error::InvalidArgument);
        }
        record(bytes, Format::from_abi(info.format)?, info.records_offset, index)
    })();
    unsafe { publish(out, result) }
}
