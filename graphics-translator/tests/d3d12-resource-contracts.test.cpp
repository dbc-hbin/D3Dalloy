#include "d3d12_resource_contract.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace d3dalloy::resource_contract;

int main() {
  uint32_t count = 0;
  assert(ResolveRange(0, UINT32_MAX, 6, count) && count == 6);
  assert(ResolveRange(2, UINT32_MAX, 6, count) && count == 4);
  assert(ResolveRange(2, 3, 6, count) && count == 3);
  assert(!ResolveRange(6, UINT32_MAX, 6, count));
  assert(!ResolveRange(5, 2, 6, count));
  assert(!ResolveRange(1, 0, 6, count));

  constexpr uint64_t d3d_alignment = 64 * 1024;
  constexpr uint64_t metal_alignment = 4 * 1024;
  assert(PlacementFits(0, 64 * 1024, 128 * 1024, d3d_alignment, metal_alignment));
  assert(PlacementFits(64 * 1024, 64 * 1024, 128 * 1024, d3d_alignment, metal_alignment));
  assert(!PlacementFits(4 * 1024, 64 * 1024, 128 * 1024, d3d_alignment, metal_alignment));
  assert(!PlacementFits(64 * 1024, 64 * 1024 + 1, 128 * 1024, d3d_alignment, metal_alignment));
  assert(!PlacementFits(64 * 1024, 1, 128 * 1024, d3d_alignment, 128 * 1024));

  assert(RangeFits(16, 16, 32));
  assert(!RangeFits(17, 16, 32));
  assert(!RangeFits(UINT64_MAX - 3, 8, UINT64_MAX));

  uint64_t value = 0;
  assert(CheckedAdd(UINT64_MAX - 1, 1, value) && value == UINT64_MAX);
  assert(!CheckedAdd(UINT64_MAX, 1, value));
  assert(CheckedMul(UINT64_MAX / 4, 4, value));
  assert(!CheckedMul(UINT64_MAX, 2, value));

  value = 7;
  assert(CheckedAdd(value, 9, value) && value == 16);
  assert(CheckedMul(value, 3, value) && value == 48);
  assert(CheckedAlign(value, 64, value) && value == 64);
  assert(CheckedAlign(value, 64, value) && value == 64);
  assert(!CheckedAlign(value, 0, value) && value == 64);
  assert(!CheckedAlign(value, 3, value) && value == 64);
  value = UINT64_MAX;
  assert(!CheckedAdd(value, 1, value) && value == UINT64_MAX);
  assert(!CheckedMul(value, 2, value) && value == UINT64_MAX);
  assert(!CheckedAlign(value, 2, value) && value == UINT64_MAX);
  assert(CheckedMul(value, 0, value) && value == 0);

  FootprintLayout footprint{};
  assert(ValidateFootprintRange(64, 64, 1, 4, 1, 1, 256, 0, 16384, 512, 256, footprint));
  assert(footprint.row_size == 256 && footprint.row_count == 64 && footprint.slice_pitch == 16384);
  assert(ValidateFootprintRange(64, 64, 1, 4, 1, 1, 512, 0, 32512, 512, 256, footprint));
  assert(!ValidateFootprintRange(64, 64, 1, 4, 1, 1, 384, 0, 65536, 512, 256, footprint));
  assert(!ValidateFootprintRange(64, 64, 1, 4, 1, 1, 256, 256, 65536, 512, 256, footprint));
  assert(!ValidateFootprintRange(64, 64, 1, 4, 1, 1, 512, 0, 32511, 512, 256, footprint));
  assert(ValidateFootprintRange(5, 5, 1, 16, 4, 4, 256, 512, 2048, 512, 256, footprint));
  assert(footprint.row_size == 32 && footprint.row_count == 2 && footprint.slice_pitch == 512);

  std::puts("D3D12_RESOURCE_CONTRACTS_CPU_PASS ranges=1 placement=1 overflow=1 footprint=1 pitch512=1");
  return 0;
}
