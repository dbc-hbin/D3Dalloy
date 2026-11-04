#include "../src/d3d12/d3d12_standard_swizzle.hpp"
#include "../src/d3d12/d3d12_standard_swizzle_gpu.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

using d3dalloy::standard_swizzle::Layout;
using d3dalloy::standard_swizzle::pattern;
using d3dalloy::standard_swizzle::tile_bytes;

// Independent bit-string oracle: consumes x BYTES from the least significant
// bit upward, and y/z in texel (or BC block) coordinates. The published
// pattern strings are MSB-first, with '-' positions fixed by unit alignment.
static uint16_t reference_offset(const char *bits, uint32_t x_bytes, uint32_t y, uint32_t z) {
  uint32_t px = 0, py = 0, pz = 0;
  uint16_t result = 0;
  for (int bit = 0; bit < 16; ++bit) {
    char axis = bits[15 - bit];
    bool set = false;
    if (axis == 'X' || axis == '-') { set = (x_bytes >> px) & 1; ++px; }
    if (axis == 'Y') { set = (y >> py) & 1; ++py; }
    if (axis == 'Z') { set = (z >> pz) & 1; ++pz; }
    if (set) result |= uint16_t(1u << bit);
  }
  return result;
}

int main() {
  constexpr std::array<const char *, 5> bits2d = {{
      "XYXYXYXYYYYYXXXX", "XYXYXYXYXYYYXXX-", "XYXYXYXYXYYYXX--",
      "XYXYXYXYXXYYX---", "XYXYXYXYXXYY----"}};
  constexpr std::array<const char *, 5> bits3d = {{
      "XYZXYZYZZZYYXXXX", "XYZXYZYZZZYYXXX-", "XYZXYZXYZZYYXX--",
      "XYZXYZXXZZYYX---", "XYZXYZXXZZYY----"}};
  constexpr std::array<uint32_t, 5> sizes = {{1, 2, 4, 8, 16}};
  for (bool volume : {false, true}) {
    for (unsigned i = 0; i < sizes.size(); ++i) {
      const uint32_t b = sizes[i];
      const auto p = pattern(volume, b);
      const auto xm = d3dalloy::swizzle_bit_moves(p.x_mask);
      const auto ym = d3dalloy::swizzle_bit_moves(p.y_mask);
      const auto zm = d3dalloy::swizzle_bit_moves(p.z_mask);
      assert(uint32_t(p.tile_width) * p.tile_height * p.tile_depth * b == tile_bytes);
      auto one = Layout::make(p.tile_width, p.tile_height, p.tile_depth, 1, 1, b, 1, 1, volume);
      assert(one && one->total_tiles == 1);
      std::vector<uint8_t> visited(tile_bytes);
      for (uint32_t z = 0; z < p.tile_depth; ++z)
        for (uint32_t y = 0; y < p.tile_height; ++y)
          for (uint32_t x = 0; x < p.tile_width; ++x)
            for (uint32_t k = 0; k < b; ++k) {
              auto address = one->offset(0, 0, x, y, z, k);
              assert(address && *address < tile_bytes);
              assert(*address == reference_offset(volume ? bits3d[i] : bits2d[i], x * b, y, z) + k);
              assert(*address == (d3dalloy::swizzle_deposit(x * b + k, xm) |
                                  d3dalloy::swizzle_deposit(y, ym) | d3dalloy::swizzle_deposit(z, zm)));
              assert(!visited[*address]);
              visited[*address] = 1;
            }
      for (auto mark : visited) assert(mark);
      auto edges = Layout::make(uint64_t(p.tile_width) + 1, uint64_t(p.tile_height) + 1,
                                volume ? uint64_t(p.tile_depth) + 1 : 1, 2, 3, b, 1, 1, volume);
      if (volume) {
        assert(!edges); // 3D textures have one volume, not an array.
        edges = Layout::make(uint64_t(p.tile_width) + 1, uint64_t(p.tile_height) + 1,
                             uint64_t(p.tile_depth) + 1, 1, 3, b, 1, 1, true);
      }
      assert(edges);
      const auto &m0 = edges->mips[0];
      assert(m0.tiles_x == 2 && m0.tiles_y == 2 && m0.tiles_z == (volume ? 2u : 1u));
      assert(edges->offset(0, 0, p.tile_width, 0).value() == tile_bytes);
      assert(edges->offset(0, 0, 0, p.tile_height).value() == m0.tiles_x * tile_bytes);
      if (volume)
        assert(edges->offset(0, 0, 0, 0, p.tile_depth).value() == 4 * tile_bytes);
      else
        assert(edges->offset(0, 1, 0, 0).value() == edges->tiles_per_slice * tile_bytes);
      assert(edges->offset(1, 0, 0, 0).value() == m0.tiles_x * m0.tiles_y * m0.tiles_z * tile_bytes);
      assert(!edges->offset(0, 0, uint64_t(p.tile_width) + 1, 0));
      assert(!edges->offset(0, 0, 0, 0, 0, b));
    }
  }
  // BC format units are 4x4 texel blocks; BC1/4 use 8-byte units, BC2/3/5/6/7 16.
  for (uint32_t bytes : {8u, 16u}) {
    auto bc = Layout::make(513, 257, 1, 2, 3, bytes, 4, 4);
    assert(bc && bc->mips[0].units_x == 129 && bc->mips[0].units_y == 65);
    assert(bc->offset(0, 0, 1, 0).value() == reference_offset(
        bytes == 8 ? bits2d[3] : bits2d[4], bytes, 0, 0));
    assert(bc->offset(0, 0, 128 / (bytes == 8 ? 1 : 2), 0).has_value());
  }
  // Paired 4:2:2 formats store two logical pixels per four-byte format unit.
  // The odd final pixel occupies a complete canonical pair, including G1.
  for (bool volume : {false, true}) {
    const auto p = pattern(volume, 4);
    auto pairs = Layout::make(uint64_t(p.tile_width) * 2 + 1, p.tile_height + 1,
                              volume ? uint64_t(p.tile_depth) + 1 : 1,
                              volume ? 1 : 2, 3, 4, 2, 1, volume);
    assert(pairs && pairs->mips[0].units_x == uint64_t(p.tile_width) + 1);
    assert(pairs->mips[0].tiles_x == 2 && pairs->mips[0].tiles_y == 2);
    assert(pairs->mips[0].tiles_z == (volume ? 2u : 1u));
    assert(pairs->offset(0, 0, p.tile_width, 0).value() == tile_bytes);
    assert(pairs->offset(0, 0, p.tile_width, 0, 0, 3).value() == tile_bytes + 3);
    assert(pairs->offset(0, 0, p.tile_width - 1, 0).value() ==
           reference_offset(volume ? bits3d[2] : bits2d[2], (p.tile_width - 1) * 4, 0, 0));
    assert(!pairs->offset(0, 0, p.tile_width + 1, 0));
  }
  assert(!Layout::make(16, 16, 1, 1, 1, 8, 2, 1));
  assert(!Layout::make(std::numeric_limits<uint64_t>::max(), 2, 1, 2, 64, 16));
  assert(!Layout::make(16, 16, 1, 1, 8, 4)); // exceeds full mip chain
  assert(!Layout::make(16, 16, 1, 1, 1, 12)); // RGB32 is excluded
}
