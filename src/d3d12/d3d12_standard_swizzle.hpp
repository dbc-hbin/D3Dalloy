#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

namespace d3dalloy::standard_swizzle {

// D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE interleave within one tile.
// See Microsoft's D3D12_TEXTURE_LAYOUT diagram. '-' means a byte-within-unit
// bit; it belongs to the x-byte deposit mask but is zero at a unit origin.
constexpr uint32_t tile_bytes = 65536;

struct Pattern {
  uint16_t x_mask;
  uint16_t y_mask;
  uint16_t z_mask;
  uint16_t tile_width;  // format units (pixels, 4x4 BC blocks, or 2x1 packed pairs)
  uint16_t tile_height;
  uint16_t tile_depth;
};

constexpr Pattern pattern(bool volume, uint32_t bytes_per_unit) {
  // Masks are written MSB-to-LSB. The 32-bpp 2D example is x=0xaa8f,
  // y=0x5570, with the x argument expressed in bytes.
  const char *bits = nullptr;
  if (volume) {
    switch (bytes_per_unit) {
    case 1: bits = "XYZXYZYZZZYYXXXX"; break;
    case 2: bits = "XYZXYZYZZZYYXXX-"; break;
    case 4: bits = "XYZXYZXYZZYYXX--"; break;
    case 8: bits = "XYZXYZXXZZYYX---"; break;
    case 16: bits = "XYZXYZXXZZYY----"; break;
    default: return {};
    }
  } else {
    switch (bytes_per_unit) {
    case 1: bits = "XYXYXYXYYYYYXXXX"; break;
    case 2: bits = "XYXYXYXYXYYYXXX-"; break;
    case 4: bits = "XYXYXYXYXYYYXX--"; break;
    case 8: bits = "XYXYXYXYXXYYX---"; break;
    case 16: bits = "XYXYXYXYXXYY----"; break;
    default: return {};
    }
  }
  Pattern result{};
  uint32_t x_count = 0, y_count = 0, z_count = 0;
  for (unsigned i = 0; i < 16; ++i) {
    const uint16_t bit = uint16_t(1u << (15 - i));
    switch (bits[i]) {
    case 'X': case '-': result.x_mask |= bit; ++x_count; break;
    case 'Y': result.y_mask |= bit; ++y_count; break;
    case 'Z': result.z_mask |= bit; ++z_count; break;
    }
  }
  const uint32_t byte_bits = bytes_per_unit == 1 ? 0 :
      bytes_per_unit == 2 ? 1 : bytes_per_unit == 4 ? 2 : bytes_per_unit == 8 ? 3 : 4;
  result.tile_width = uint16_t(1u << (x_count - byte_bits));
  result.tile_height = uint16_t(1u << y_count);
  result.tile_depth = uint16_t(1u << z_count);
  return result;
}

constexpr uint16_t deposit(uint32_t value, uint16_t mask) {
  uint16_t result = 0;
  for (uint16_t bit = 1; bit; bit <<= 1) {
    if (mask & bit) {
      if (value & 1)
        result |= bit;
      value >>= 1;
    }
  }
  return result;
}

constexpr uint16_t intra_tile_offset(const Pattern &p, uint32_t x_byte, uint32_t y, uint32_t z = 0) {
  return uint16_t(deposit(x_byte, p.x_mask) | deposit(y, p.y_mask) | deposit(z, p.z_mask));
}

constexpr bool checked_add(uint64_t a, uint64_t b, uint64_t &out) {
  if (b > std::numeric_limits<uint64_t>::max() - a)
    return false;
  out = a + b;
  return true;
}
constexpr bool checked_mul(uint64_t a, uint64_t b, uint64_t &out) {
  if (b && a > std::numeric_limits<uint64_t>::max() / b)
    return false;
  out = a * b;
  return true;
}
constexpr uint64_t ceil_div(uint64_t value, uint64_t divisor) {
  return value / divisor + (value % divisor != 0);
}

struct Mip {
  uint64_t first_tile_in_slice = 0;
  uint64_t tiles_x = 0, tiles_y = 0, tiles_z = 0;
  uint64_t units_x = 0, units_y = 0, units_z = 0;
};

// A conservative adapter-defined mip-tail choice: reserve whole 64KB tiles
// for every subresource, even if a mip occupies less than one tile. This
// preserves the standard intra-tile pattern, avoids ambiguous packed-texel
// addresses, and provides a stable layout for Map and placed buffer aliases.
// GetResourceTiling must describe this exact choice; it is not a Windows
// vendor's packed-tail layout. Array slices occupy equal tile-aligned spans.
struct Layout {
  Pattern interleave{};
  std::array<Mip, 64> mips{};
  uint32_t mip_count = 0;
  uint32_t array_count = 0;
  uint32_t bytes_per_unit = 0;
  uint32_t block_width = 0, block_height = 0;
  uint64_t tiles_per_slice = 0;
  uint64_t total_tiles = 0;
  uint64_t size_bytes = 0;

  static std::optional<Layout> make(uint64_t width, uint64_t height, uint64_t depth,
                                    uint32_t arrays, uint32_t mip_levels, uint32_t unit_bytes,
                                    uint32_t block_w = 1, uint32_t block_h = 1, bool volume = false) {
    if (!width || !height || !depth || !arrays || !block_w || !block_h ||
        (volume && arrays != 1) || (!volume && depth != 1) ||
        !((block_w == 1 && block_h == 1) || (block_w == 4 && block_h == 4) ||
          (block_w == 2 && block_h == 1 && unit_bytes == 4)))
      return std::nullopt;
    Layout layout{};
    layout.interleave = pattern(volume, unit_bytes);
    if (!layout.interleave.tile_width)
      return std::nullopt;
    layout.bytes_per_unit = unit_bytes;
    layout.block_width = block_w;
    layout.block_height = block_h;
    layout.array_count = arrays;
    uint64_t max_dimension = width > height ? width : height;
    if (depth > max_dimension) max_dimension = depth;
    uint32_t full_mips = 0;
    do { ++full_mips; max_dimension >>= 1; } while (max_dimension);
    if (!mip_levels) mip_levels = full_mips;
    if (mip_levels > full_mips || mip_levels > layout.mips.size())
      return std::nullopt;
    layout.mip_count = mip_levels;
    for (uint32_t i = 0; i < mip_levels; ++i) {
      Mip &m = layout.mips[i];
      m.first_tile_in_slice = layout.tiles_per_slice;
      const uint64_t mw = i >= 64 ? 1 : width >> i;
      const uint64_t mh = i >= 64 ? 1 : height >> i;
      const uint64_t md = i >= 64 ? 1 : depth >> i;
      m.units_x = ceil_div(mw ? mw : 1, block_w);
      m.units_y = ceil_div(mh ? mh : 1, block_h);
      m.units_z = md ? md : 1;
      m.tiles_x = ceil_div(m.units_x, layout.interleave.tile_width);
      m.tiles_y = ceil_div(m.units_y, layout.interleave.tile_height);
      m.tiles_z = ceil_div(m.units_z, layout.interleave.tile_depth);
      uint64_t plane = 0, count = 0;
      if (!checked_mul(m.tiles_x, m.tiles_y, plane) ||
          !checked_mul(plane, m.tiles_z, count) ||
          !checked_add(layout.tiles_per_slice, count, layout.tiles_per_slice))
        return std::nullopt;
    }
    if (!checked_mul(layout.tiles_per_slice, arrays, layout.total_tiles) ||
        !checked_mul(layout.total_tiles, tile_bytes, layout.size_bytes))
      return std::nullopt;
    return layout;
  }

  // Coordinates are in format units; BC uses (x/4,y/4), packed 4:2:2 (x/2,y).
  // No wrapping or truncation is permitted for heap offsets/large resources.
  std::optional<uint64_t> offset(uint32_t mip, uint32_t array, uint64_t x, uint64_t y,
                                 uint64_t z = 0, uint32_t byte_in_unit = 0) const {
    if (mip >= mip_count || array >= array_count || byte_in_unit >= bytes_per_unit)
      return std::nullopt;
    const Mip &m = mips[mip];
    if (x >= m.units_x || y >= m.units_y || z >= m.units_z)
      return std::nullopt;
    const uint64_t tx = x / interleave.tile_width, ty = y / interleave.tile_height;
    const uint64_t tz = z / interleave.tile_depth;
    // Build is checked and tile indices cannot overflow its total count.
    const uint64_t tile = array * tiles_per_slice + m.first_tile_in_slice +
        ((tz * m.tiles_y + ty) * m.tiles_x + tx);
    const uint64_t tile_offset = intra_tile_offset(
        interleave, uint32_t((x % interleave.tile_width) * bytes_per_unit),
        uint32_t(y % interleave.tile_height), uint32_t(z % interleave.tile_depth));
    return tile * tile_bytes + tile_offset + byte_in_unit;
  }
};

static_assert(pattern(false, 4).x_mask == 0xaa8f && pattern(false, 4).y_mask == 0x5570);
static_assert(pattern(false, 1).tile_width == 256 && pattern(false, 1).tile_height == 256);
static_assert(pattern(false, 2).tile_width == 256 && pattern(false, 2).tile_height == 128);
static_assert(pattern(false, 4).tile_width == 128 && pattern(false, 4).tile_height == 128);
static_assert(pattern(false, 8).tile_width == 128 && pattern(false, 8).tile_height == 64);
static_assert(pattern(false, 16).tile_width == 64 && pattern(false, 16).tile_height == 64);
static_assert(pattern(true, 1).tile_width == 64 && pattern(true, 1).tile_height == 32 && pattern(true, 1).tile_depth == 32);
static_assert(pattern(true, 2).tile_width == 32 && pattern(true, 2).tile_height == 32 && pattern(true, 2).tile_depth == 32);
static_assert(pattern(true, 4).tile_width == 32 && pattern(true, 4).tile_height == 32 && pattern(true, 4).tile_depth == 16);
static_assert(pattern(true, 8).tile_width == 32 && pattern(true, 8).tile_height == 16 && pattern(true, 8).tile_depth == 16);
static_assert(pattern(true, 16).tile_width == 16 && pattern(true, 16).tile_height == 16 && pattern(true, 16).tile_depth == 16);

} // namespace d3dalloy::standard_swizzle
