// CPU-only: python3 graphics-translator/tests/run-bindless-collection-cpu.py
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

enum D3D12_DESCRIPTOR_RANGE_TYPE { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                 D3D12_DESCRIPTOR_RANGE_TYPE_CBV };
struct Range { uint64_t offset, count; D3D12_DESCRIPTOR_RANGE_TYPE type; };
struct Root {
  std::vector<Range> ranges{{1, 2, D3D12_DESCRIPTOR_RANGE_TYPE_SRV}};
  const auto &GetTableRanges(uint32_t) const { return ranges; }
  template<class F> void ForEachUsedDescriptorRange(const std::vector<Range> &used, uint32_t, F fn) const {
    for (const auto &range : used) fn(range.offset, range.count, range.type);
  }
};
struct D3D12_DESCRIPTOR_HEAP_DESC { uint32_t NumDescriptors; };
struct D3D12_GPU_DESCRIPTOR_HANDLE { uint64_t ptr; };
struct Heap {
  uint32_t size;
  uint64_t start = 4096;
  std::vector<uint64_t> visits;
  void GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *desc) const { desc->NumDescriptors = size; }
  void GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *handle) const { handle->ptr = start; }
  uint64_t GetDescriptor(uint64_t index) { assert(index < size); visits.push_back(index); return index; }
};
struct Table {
  Root *rootsig;
  Heap *heap;
  uint64_t base = 4096;
  uint32_t parameter = UINT32_MAX;
  bool restrict_to_shader = false;
  uint32_t shader_bindings = 0;
};
struct Probe {
  void append_descriptor(Heap *heap, uint64_t index) { heap->GetDescriptor(index); }
  void append_descriptor(uint64_t) {}
  void standard(std::span<const Table> tables) {
#include STANDARD_LOOP
  }
  void atomic(std::span<const Table> tables) {
#include ATOMIC_LOOP
  }
};
int main() {
  Probe probe;
  Root first, second;
  for (auto collect : {&Probe::standard, &Probe::atomic}) {
    Heap a{8, 4096, {}}, b{4, 4096, {}}, empty{0, 4096, {}};
    std::vector<Table> tables{{&first, &a}, {&second, &a}, {&first, &b}, {&first, &a}};
    (probe.*collect)(tables);
    assert((a.visits == std::vector<uint64_t>{0,1,2,3,4,5,6,7}));
    assert((b.visits == std::vector<uint64_t>{0,1,2,3}));
    (probe.*collect)(tables); // No cached discovery across calls/encoder phases.
    assert(a.visits.size() == 16 && b.visits.size() == 8);
    a.visits.clear();
    tables = {{&first, &a, 0}, {&first, &a, 4097}, {&first, &a, 4096 + 8 * 32},
              {&first, &empty}, {&second, &a, 4096 + 32}, {&first, &a}};
    (probe.*collect)(tables); // Invalid earlier full entries must not suppress the valid one.
    assert(a.visits.size() == 8 && empty.visits.empty());
    a.visits.clear();
    tables = {{&first, &a, 4096, 0}, {&second, &a}, {&first, &a, 4096, 0, true}};
    (probe.*collect)(tables); // Partial scans keep their original order and shader restrictions.
    assert((a.visits == std::vector<uint64_t>{1,2,0,1,2,3,4,5,6,7,1,2}));
    a.visits.clear();
    tables = {{&first, &a, 4096, 0}, {&second, &a, 4096, 0, true}};
    (probe.*collect)(tables); // Do not merge distinct explicit-table records.
    assert((a.visits == std::vector<uint64_t>{1,2,1,2}));
    a.visits.clear();
    a.start = 0;
    tables = {{&first, &a}};
    (probe.*collect)(tables);
    assert(a.visits.empty());
  }
  Heap heap{8, 4096, {}};
  first.ranges = {{3, 2, D3D12_DESCRIPTOR_RANGE_TYPE_CBV}};
  std::vector<Table> cbv{{&first, &heap, 4096, 0}};
  probe.standard(cbv);
  assert(heap.visits.empty());
  probe.atomic(cbv);
  assert((heap.visits == std::vector<uint64_t>{3,4}));
  heap.visits.clear();
  std::vector<Table> nulls{{nullptr, nullptr}, {nullptr, &heap, 4096, 0}, {nullptr, &heap}};
  probe.atomic(nulls);
  assert(heap.visits.size() == 8);
  std::puts("BINDLESS_COLLECTION_CPU_PASS full_heap_dedup invalid_entries explicit_tables phase_isolation");
}
