#include "../../src/d3d12/d3d12_atomic64_storage.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <random>

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

  CanonicalCellMap sequential;
  constexpr uint32_t words=1024*1024/8;
  for (uint32_t i=0;i<words;++i) assert(sequential.Resolve(buffer,uint64_t(i)*8)==i);
  assert(sequential.Count()==words && sequential.RunCount()==1);
  for (uint32_t i=words;i--;) assert(sequential.Resolve(buffer,uint64_t(i)*8+7)==i);
  CanonicalCellMap mixed;
  std::map<CanonicalCell,uint32_t> reference;
  std::mt19937_64 random(0x6a09e667f3bcc909ull);
  for (unsigned i=0;i<200000;++i) {
    CanonicalOwner owner{CanonicalKind(random()%5),random()%7,(random()%3)*65536};
    owner.layout[0]=random()%3; owner.layout[7]=random()%2;
    const auto offset=random()%(2*65536);
    CanonicalCell key{owner,(owner.base+offset)&~uint64_t(7)};key.owner.base=0;
    auto [entry,inserted]=reference.emplace(key,uint32_t(reference.size()));
    (void)inserted;
    assert(mixed.Resolve(owner,offset)==entry->second);
    assert(mixed.Count()==reference.size());
  }
  bool rejected=false;
  try { mixed.Resolve({CanonicalKind::HeapBytes,9,UINT64_MAX-3},4); }
  catch (const std::length_error&) { rejected=true; }
  assert(rejected);
  std::printf("ATOMIC64_TOPOLOGY_RUNS_PASS sequential_cells=%u ordered_map_nodes=%zu random_alias_cases=200000 exact_first_seen_ids=1 layout_domains=1 overflow=1\n",
      words,sequential.RunCount());
  return 0;
}
