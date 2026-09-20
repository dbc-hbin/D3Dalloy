#pragma once

#include <cstdint>

namespace dxmt::resource_contract {

inline bool
CheckedAdd(uint64_t a, uint64_t b, uint64_t &result) {
  if (b > UINT64_MAX - a)
    return false;
  result = a + b;
  return true;
}

inline bool
CheckedMul(uint64_t a, uint64_t b, uint64_t &result) {
  if (a && b > UINT64_MAX / a)
    return false;
  result = a * b;
  return true;
}

inline bool
RangeFits(uint64_t offset, uint64_t length, uint64_t total) {
  return offset <= total && length <= total - offset;
}

inline bool
ResolveRange(uint32_t first, uint32_t requested, uint32_t total, uint32_t &resolved) {
  if (first >= total)
    return false;
  resolved = requested == UINT32_MAX ? total - first : requested;
  return resolved && resolved <= total - first;
}

inline bool
PlacementFits(uint64_t offset, uint64_t size, uint64_t heap_size, uint64_t d3d_alignment, uint64_t metal_alignment) {
  if (!d3d_alignment || (offset % d3d_alignment))
    return false;
  if (metal_alignment && (offset % metal_alignment))
    return false;
  return RangeFits(offset, size, heap_size);
}

struct FootprintLayout {
  uint64_t row_size = 0;
  uint64_t row_count = 0;
  uint64_t slice_pitch = 0;
};

inline bool
ValidateFootprintRange(
    uint64_t width, uint64_t height, uint64_t depth, uint32_t bytes_per_block, uint32_t block_width,
    uint32_t row_pitch, uint64_t offset, uint64_t buffer_size,
    uint32_t placement_alignment, uint32_t pitch_alignment, FootprintLayout &layout
) {
  if (!width || !height || !depth || !bytes_per_block || !block_width || !row_pitch ||
      !placement_alignment || !pitch_alignment || (offset % placement_alignment) || (row_pitch % pitch_alignment))
    return false;

  const uint64_t block_columns = (width + block_width - 1) / block_width;
  layout.row_count = (height + block_width - 1) / block_width;
  if (!CheckedMul(block_columns, bytes_per_block, layout.row_size) || layout.row_size > row_pitch)
    return false;
  if (!CheckedMul(row_pitch, layout.row_count, layout.slice_pitch))
    return false;

  uint64_t required = layout.row_size;
  if (layout.row_count > 1) {
    uint64_t rows_before_last = 0;
    if (!CheckedMul(row_pitch, layout.row_count - 1, rows_before_last) || !CheckedAdd(required, rows_before_last, required))
      return false;
  }
  if (depth > 1) {
    uint64_t slices_before_last = 0;
    if (!CheckedMul(layout.slice_pitch, depth - 1, slices_before_last) || !CheckedAdd(required, slices_before_last, required))
      return false;
  }
  return RangeFits(offset, required, buffer_size);
}

} // namespace dxmt::resource_contract
