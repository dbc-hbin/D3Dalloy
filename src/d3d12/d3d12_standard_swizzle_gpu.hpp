#pragma once

#ifdef __METAL__
#include <metal_stdlib>
using SwizzleUint = metal::uint;
using SwizzleUlong = metal::ulong;
#else
#include <cstdint>
using SwizzleUint = uint32_t;
using SwizzleUlong = uint64_t;
#endif

namespace dxmt {

// Kernel arguments for one D3D12 texture subresource. The buffer bindings
// carry the allocation offsets; first_tile is subresource-relative to the
// canonical buffer binding, which begins at that placed resource's byte zero.
struct D3D12StandardSwizzleParams {
  SwizzleUlong first_tile;
  SwizzleUlong tiles_x;
  SwizzleUlong tiles_y;
  SwizzleUint units_x;
  SwizzleUint units_y;
  SwizzleUint units_z;
  SwizzleUint tile_width;
  SwizzleUint tile_height;
  SwizzleUint tile_depth;
  SwizzleUint x_mask;
  SwizzleUint y_mask;
  SwizzleUint z_mask;
  SwizzleUint bytes_per_unit;
  SwizzleUint row_pitch;
  SwizzleUint slice_pitch;
  SwizzleUint origin_x;
  SwizzleUint origin_y;
  SwizzleUint origin_z;
  SwizzleUint extent_x;
  SwizzleUint extent_y;
  SwizzleUint extent_z;
  // Packed 4:2:2 stores one 4-byte canonical pair as two RGBA8 native pixels.
  SwizzleUint physical_width;
  SwizzleUint packed422_variant;
};

#ifndef __METAL__
static_assert(sizeof(D3D12StandardSwizzleParams) == 104);
#endif

} // namespace dxmt
