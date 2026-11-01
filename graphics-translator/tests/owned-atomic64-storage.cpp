#include "../../src/d3d12/d3d12_atomic64_storage.hpp"
#include <cassert>
#include <cstdint>
#include <limits>

int main() {
  using namespace d3dalloy::atomic64;
  CanonicalCellMap cells;
  const CanonicalOwner buffer{CanonicalKind::Buffer, 41, 0};
  const CanonicalOwner placed{CanonicalKind::HeapBytes, 9, 0};
  const CanonicalOwner tile{CanonicalKind::HeapBytes, 9, 65536};
  const auto a = cells.Resolve(buffer, 0);
  assert(cells.Resolve(buffer, 7) == a);
  assert(cells.Resolve(buffer, 8) != a);
  const auto placed_cell = cells.Resolve(placed, 65544);
  assert(cells.Resolve(tile, 8) == placed_cell);
  assert(cells.Resolve({CanonicalKind::TextureUnits, 9, 65536}, 8) != placed_cell);
  assert(cells.Count() == 4);

  auto layout = ArenaLayout::Make(cells.Count(), 16384);
  assert(layout && layout->node_count == cells.Count() + 3 * 16384 + 1);
  assert(layout->bitmap_words == (layout->node_count + 31) / 32);
  assert(!ArenaLayout::Make(0, 0));
  assert(!ArenaLayout::Make(1, uint64_t(1) << 30));
  assert(!ArenaLayout::Make(UINT32_MAX, 1));
  assert(!ArenaLayout::Make(1, std::numeric_limits<uint64_t>::max()));
  assert(ArenaLayout::Make(0, 1)); // All-null bindings still have a valid empty phase.

  // Logical 8-byte cells must not cross one sparse page's canonical identity.
  assert(cells.Resolve({CanonicalKind::HeapBytes, 9, 0}, 65528) !=
         cells.Resolve({CanonicalKind::HeapBytes, 9, 65536}, 0));
  assert(cells.Resolve({CanonicalKind::HeapBytes, 9, 65536}, 0) ==
         cells.Resolve({CanonicalKind::HeapBytes, 9, 0}, 65536));
  return 0;
}
