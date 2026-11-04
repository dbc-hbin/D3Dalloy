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

namespace d3dalloy {

// Move packed coordinate bits into their places in the 16-bit tile offset.
// Applying the largest displacements first keeps the bit positions ordered.
struct SwizzleBitMoves {
  SwizzleUint mask[4];
};

#ifndef __METAL__
constexpr SwizzleBitMoves swizzle_bit_moves(SwizzleUint destinations) {
  SwizzleBitMoves moves{};
  SwizzleUint source = 0;
  for (SwizzleUint dest = 0; dest < 16; ++dest) {
    if (!(destinations & (1u << dest))) continue;
    SwizzleUint position = source++;
    for (SwizzleUint stage = 0; stage < 4; ++stage) {
      const SwizzleUint shift = 8u >> stage;
      if (dest - position >= shift) {
        moves.mask[stage] |= 1u << position;
        position += shift;
      }
    }
  }
  return moves;
}
#endif

inline SwizzleUint swizzle_deposit(SwizzleUint value, SwizzleBitMoves moves) {
  for (SwizzleUint stage = 0; stage < 4; ++stage) {
    const SwizzleUint mask = moves.mask[stage];
    value = (value & ~mask) | ((value & mask) << (8u >> stage));
  }
  return value;
}

// One dispatch covers exactly the active format units of one tile.
struct D3D12StandardSwizzleParams {
  SwizzleUlong tile_offset;
  SwizzleBitMoves x_moves, y_moves, z_moves;
  SwizzleUint bytes_per_unit;
  SwizzleUint row_pitch;
  SwizzleUint slice_pitch;
  // Packed 4:2:2 stores one 4-byte canonical pair as two RGBA8 native pixels.
  SwizzleUint tile_pixel_width;
  SwizzleUint packed422_variant;
};

#ifndef __METAL__
static_assert(sizeof(D3D12StandardSwizzleParams) == 80);
#endif

} // namespace d3dalloy
