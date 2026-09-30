// SM 6.6 descriptor-heap 64 bit atomics for owned-d3d12-sm6.cpp. Compile with
// DXC 1.9: -HV 2021 -T cs_6_6 -E cs_atomic64_heap.

// cs_atomic64_heap (cs_6_6): 64 bit atomics on SM 6.6 descriptor-heap resources
// (AtomicInt64OnDescriptorHeapResourceSupported). Heap slot 0 is Out, 1 is Raw.
[numthreads(64, 1, 1)]
void cs_atomic64_heap(uint gi : SV_GroupIndex) {
  RWByteAddressBuffer raw = ResourceDescriptorHeap[1];
  RWByteAddressBuffer dst = ResourceDescriptorHeap[0];
  uint64_t old;
  raw.InterlockedAdd64(8, 0x100000001ull, old);
  dst.Store<uint64_t>(gi * 8, old);
}
