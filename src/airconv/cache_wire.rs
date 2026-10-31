// SPDX-License-Identifier: LGPL-2.1-or-later
use core::slice;

// The caller owns readable storage; writable destinations must not alias inputs.
// Empty cache regions may use null pointers. No borrowed view crosses the ABI.
pub(super) unsafe fn input<'a>(data: *const u8, length: u64) -> Option<&'a [u8]> {
    let length = usize::try_from(length).ok()?;
    if length == 0 {
        return Some(&[]);
    }
    if data.is_null() || length > isize::MAX as usize {
        return None;
    }
    Some(unsafe { slice::from_raw_parts(data, length) })
}

pub(super) unsafe fn output<'a>(data: *mut u8, length: u64) -> Option<&'a mut [u8]> {
    let length = usize::try_from(length).ok()?;
    if length == 0 {
        return Some(&mut []);
    }
    if data.is_null() || length > isize::MAX as usize {
        return None;
    }
    Some(unsafe { slice::from_raw_parts_mut(data, length) })
}

pub(super) fn range(bytes: &[u8], offset: u64, length: u64) -> Option<&[u8]> {
    let end = offset.checked_add(length)?;
    bytes.get(usize::try_from(offset).ok()?..usize::try_from(end).ok()?)
}

pub(super) fn range_mut(bytes: &mut [u8], offset: u64, length: u64) -> Option<&mut [u8]> {
    let end = offset.checked_add(length)?;
    bytes.get_mut(usize::try_from(offset).ok()?..usize::try_from(end).ok()?)
}

pub(super) fn u32_at(bytes: &[u8], offset: u64) -> Option<u32> {
    Some(u32::from_le_bytes(range(bytes, offset, 4)?.try_into().ok()?))
}

pub(super) fn u64_at(bytes: &[u8], offset: u64) -> Option<u64> {
    Some(u64::from_le_bytes(range(bytes, offset, 8)?.try_into().ok()?))
}

pub(super) fn put_u32(bytes: &mut [u8], offset: u64, value: u32) -> Option<()> {
    range_mut(bytes, offset, 4)?.copy_from_slice(&value.to_le_bytes());
    Some(())
}

pub(super) fn put_u64(bytes: &mut [u8], offset: u64, value: u64) -> Option<()> {
    range_mut(bytes, offset, 8)?.copy_from_slice(&value.to_le_bytes());
    Some(())
}
