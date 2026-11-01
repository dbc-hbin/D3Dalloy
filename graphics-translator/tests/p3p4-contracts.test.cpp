// CPU-only footprint boundary contracts. Quad semantics are exercised by the mesh GPU fixture.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/d3d12/d3d12_resource_contract.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>

int main() {
  using namespace d3dalloy::resource_contract;
  FootprintLayout layout;
  uint32_t footprint_checks = 0;
  for (const uint32_t bytes : {1u, 2u, 4u, 8u, 12u, 16u}) {
    for (const uint32_t block : {1u, 4u}) {
      // Three blocks per row, two rows per slice, three slices. Last row needs
      // no trailing padding. Both offsets and pitch deliberately violate 512/256.
      const uint64_t minimum = 29u * bytes;
      assert(ValidateFootprintRange(3 * block, 2 * block, 3, bytes, block, block,
                                    5 * bytes, bytes, minimum, bytes, bytes, layout));
      assert(layout.row_size == 3u * bytes && layout.row_count == 2 && layout.slice_pitch == 10u * bytes);
      assert(!ValidateFootprintRange(3 * block, 2 * block, 3, bytes, block, block,
                                     5 * bytes, bytes, minimum - 1, bytes, bytes, layout));
      assert(!ValidateFootprintRange(3 * block, 2 * block, 3, bytes, block, block,
                                     2 * bytes, bytes, minimum, bytes, bytes, layout));
      if (bytes > 1) {
        assert(!ValidateFootprintRange(3 * block, 2 * block, 3, bytes, block, block,
                                       5 * bytes + 1, bytes, minimum + 100, bytes, bytes, layout));
        assert(!ValidateFootprintRange(3 * block, 2 * block, 3, bytes, block, block,
                                       5 * bytes, bytes + 1, minimum + 100, bytes, bytes, layout));
      }
      ++footprint_checks;
    }
  }
  // Portable GetCopyableFootprints remains accepted for the 12-byte RGB32 format.
  assert(ValidateFootprintRange(3, 2, 1, 12, 1, 1, 256, 512, 804, 512, 256, layout));
  assert(!ValidateFootprintRange(3, 2, 1, 12, 1, 1, 256, 512, 803, 512, 256, layout));
  assert(!ValidateFootprintRange(3, 2, 3, 16, 1, 1, 80,
                                 std::numeric_limits<uint64_t>::max() - 15,
                                 std::numeric_limits<uint64_t>::max(), 16, 16, layout));
  std::printf("P3P4_CPU_CONTRACTS_PASS footprint_formats=%u\n", footprint_checks);
  return 0;
}
