#ifndef KIND
#define KIND 0
#endif
RWByteAddressBuffer output : register(u0);
RWBuffer<uint64_t> cells : register(u1);
#if KIND == 0
RWBuffer<uint4> formatted : register(u2);
#define INITIAL uint4(0x12345678, 0x87654321, 0xabcd1234, 0xfedc9876)
#define REPLACEMENT 0x23456789
#elif KIND == 1
RWBuffer<int4> formatted : register(u2);
#define INITIAL int4(-12, 34, -56, 78)
#define REPLACEMENT -23
#else
RWBuffer<float4> formatted : register(u2);
#define INITIAL float4(0.25, 0.5, 0.75, 1.0)
#define REPLACEMENT 0.125
#endif
[numthreads(1,1,1)]
void formats() {
  formatted[0] = INITIAL;
  uint64_t old;
  // Returning 64-bit atomic forces the body into an atomic phase and observes
  // bytes written through the independently formatted, differently sized UAV.
  InterlockedAdd(cells[0], 0ULL, old);
  output.Store<uint64_t>(0, old);
  output.Store4(8, asuint(formatted[0]));
  formatted[0].x = REPLACEMENT;
  InterlockedAdd(cells[0], 0ULL, old);
  output.Store<uint64_t>(24, old);
  output.Store4(32, asuint(formatted[0]));
}
