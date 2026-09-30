#pragma once

#include "dxgiformat.h"
#include <cstdint>

namespace dxmt::resource_contract {

enum class EmulatedTextureStorage : uint8_t {
  None,
  RGB32,
  PackedR8G8_B8G8,
  PackedG8R8_G8B8,
};

inline EmulatedTextureStorage GetEmulatedTextureStorage(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32_TYPELESS:
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
    return EmulatedTextureStorage::RGB32;
  case DXGI_FORMAT_R8G8_B8G8_UNORM:
    return EmulatedTextureStorage::PackedR8G8_B8G8;
  case DXGI_FORMAT_G8R8_G8B8_UNORM:
    return EmulatedTextureStorage::PackedG8R8_G8B8;
  default:
    return EmulatedTextureStorage::None;
  }
}

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
CheckedAlign(uint64_t value, uint64_t alignment, uint64_t &result) {
  if (!alignment || (alignment & (alignment - 1)))
    return false;
  uint64_t sum = 0;
  if (!CheckedAdd(value, alignment - 1, sum))
    return false;
  result = sum & ~(alignment - 1);
  return true;
}

enum class FormatFamily : uint8_t {
  Unknown, R32G32B32A32, R32G32B32, R16G16B16A16, R32G32, R32G8X24,
  R10G10B10A2, R8G8B8A8, R16G16, R32, R24G8, R8G8, R16, R8, A8,
  R1, R9G9B9E5, R8G8B8G8, G8R8G8B8, B5G6R5, B5G5R5A1, B8G8R8A8,
  B8G8R8X8, R10G10B10XRBiasA2, B4G4R4A4, BC1, BC2, BC3, BC4, BC5, BC6H, BC7
};

inline FormatFamily
GetFormatFamily(DXGI_FORMAT format) {
  switch (format) {
#define DXMT_FORMAT_FAMILY_CASE(name) case DXGI_FORMAT_##name##_TYPELESS: case DXGI_FORMAT_##name##_FLOAT: \
  case DXGI_FORMAT_##name##_UINT: case DXGI_FORMAT_##name##_SINT: return FormatFamily::name
  DXMT_FORMAT_FAMILY_CASE(R32G32B32A32);
  DXMT_FORMAT_FAMILY_CASE(R32G32B32);
  DXMT_FORMAT_FAMILY_CASE(R32G32);
#undef DXMT_FORMAT_FAMILY_CASE
#define DXMT_FORMAT_FAMILY_CASE(name) case DXGI_FORMAT_##name##_TYPELESS: case DXGI_FORMAT_##name##_FLOAT: \
  case DXGI_FORMAT_##name##_UNORM: case DXGI_FORMAT_##name##_UINT: case DXGI_FORMAT_##name##_SNORM: \
  case DXGI_FORMAT_##name##_SINT: return FormatFamily::name
  DXMT_FORMAT_FAMILY_CASE(R16G16B16A16);
  DXMT_FORMAT_FAMILY_CASE(R16G16);
  DXMT_FORMAT_FAMILY_CASE(R16);
#undef DXMT_FORMAT_FAMILY_CASE
#define DXMT_FORMAT_FAMILY_CASE(name) case DXGI_FORMAT_##name##_TYPELESS: case DXGI_FORMAT_##name##_UNORM: \
  case DXGI_FORMAT_##name##_UNORM_SRGB: case DXGI_FORMAT_##name##_UINT: case DXGI_FORMAT_##name##_SNORM: \
  case DXGI_FORMAT_##name##_SINT: return FormatFamily::name
  DXMT_FORMAT_FAMILY_CASE(R8G8B8A8);
#undef DXMT_FORMAT_FAMILY_CASE
  case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: return FormatFamily::R10G10B10A2;
  case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: return FormatFamily::R8G8;
  case DXGI_FORMAT_D16_UNORM: return FormatFamily::R16;
  case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: return FormatFamily::R32;
  case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return FormatFamily::R32G8X24;
  case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return FormatFamily::R24G8;
  case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: return FormatFamily::R8;
#define DXMT_BC_FAMILY(n) case DXGI_FORMAT_BC##n##_TYPELESS: case DXGI_FORMAT_BC##n##_UNORM: case DXGI_FORMAT_BC##n##_UNORM_SRGB: return FormatFamily::BC##n
  DXMT_BC_FAMILY(1); DXMT_BC_FAMILY(2); DXMT_BC_FAMILY(3); DXMT_BC_FAMILY(7);
#undef DXMT_BC_FAMILY
  case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: return FormatFamily::BC4;
  case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: return FormatFamily::BC5;
  case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16: return FormatFamily::BC6H;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return FormatFamily::B8G8R8A8;
  case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return FormatFamily::B8G8R8X8;
  default: return FormatFamily::Unknown;
  }
}

inline bool
AreCopyCompatibleFormats(DXGI_FORMAT a, DXGI_FORMAT b) {
  if (a == b)
    return a != DXGI_FORMAT_UNKNOWN;
  const auto family = GetFormatFamily(a);
  return family != FormatFamily::Unknown && family == GetFormatFamily(b);
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
    uint64_t width, uint64_t height, uint64_t depth, uint32_t bytes_per_block,
    uint32_t block_width, uint32_t block_height,
    uint32_t row_pitch, uint64_t offset, uint64_t buffer_size,
    uint32_t placement_alignment, uint32_t pitch_alignment, FootprintLayout &layout
) {
  if (!width || !height || !depth || !bytes_per_block || !block_width || !block_height || !row_pitch ||
      !placement_alignment || !pitch_alignment || (offset % placement_alignment) || (row_pitch % pitch_alignment))
    return false;

  const uint64_t block_columns = (width + block_width - 1) / block_width;
  layout.row_count = (height + block_height - 1) / block_height;
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
