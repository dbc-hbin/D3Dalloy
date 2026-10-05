// Official writable-MSAA syntax, isolated from ordinary uint64 texture loads.
#ifndef ARRAY
#define ARRAY 0
#endif
#if ARRAY
RWTexture2DMSArray<uint64_t, 2> cells : register(u0);
#define LOCATION uint3(0, 0, 0)
#else
RWTexture2DMS<uint64_t, 2> cells : register(u0);
#define LOCATION uint2(0, 0)
#endif
RWByteAddressBuffer output : register(u1);
[numthreads(1,1,1)]
void atomic_sample() {
  uint64_t old;
  InterlockedAdd(cells.sample[1][LOCATION], 0x100000001ULL, old);
  output.Store<uint64_t>(0, old);
}
