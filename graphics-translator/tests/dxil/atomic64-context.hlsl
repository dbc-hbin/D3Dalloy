RWByteAddressBuffer output : register(u0);
RWStructuredBuffer<uint64_t> cells : register(u1);
RWByteAddressBuffer alias : register(u2);
cbuffer Root : register(b0) { uint base_word; uint iterations; uint seed; uint tag; };

// Bounded independent cells exercise the same returning-atomic context without
// any shader waiting for a different dispatch or queue to make progress.
[numthreads(32, 1, 1)]
void atomic_work(uint3 id : SV_DispatchThreadID) {
  uint64_t first = 0;
  for (uint step = 0; step < iterations; ++step) {
    uint64_t old;
    InterlockedAdd(cells[id.x], 0x100000001ULL, old);
    if (step == 0) first = old;
  }
  output.Store<uint64_t>(32 + id.x * 24, first);
  output.Store<uint64_t>(40 + id.x * 24, cells[id.x]);
  output.Store<uint64_t>(48 + id.x * 24, alias.Load<uint64_t>(id.x * 8));
}

// Register-only ordinary work gives the other queue a finite overlap window.
// The CPU oracle evaluates the same modular affine recurrence by exponentiation.
[numthreads(32, 1, 1)]
void ordinary_work(uint3 id : SV_DispatchThreadID) {
  uint value = seed + id.x;
  for (uint step = 0; step < iterations; ++step) value = value * 1664525u + 1013904223u;
  output.Store((base_word + id.x) * 4, value ^ tag);
}
