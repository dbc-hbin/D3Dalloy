// Frozen regressions for review findings 4, 7 and 8. Bind through volatile
// descriptor tables (root signature 1.0) so buffer bounds must be preserved.
RWByteAddressBuffer Output : register(u0);
cbuffer Indices : register(b0) { uint4 indices; };
#ifdef REVIEW64
StructuredBuffer<uint64_t> Input64 : register(t0);
RWStructuredBuffer<uint64_t> Mutable64 : register(u1);
[numthreads(1, 1, 1)]
void cs_bounds64() {
  for (uint lane = 0; lane < 4; ++lane) {
    uint index = indices[lane];
    Output.Store<uint64_t>(lane * 32, Input64[index]);
    Mutable64[index] = 31;
    uint64_t old;
    InterlockedAdd(Mutable64[index], 7, old);
    Output.Store<uint64_t>(lane * 32 + 8, old);
    InterlockedCompareExchange(Mutable64[index], 38, 41, old);
    Output.Store<uint64_t>(lane * 32 + 16, old);
    Output.Store<uint64_t>(lane * 32 + 24, Mutable64[index]);
  }
}
#else
StructuredBuffer<uint> Input : register(t0);
StructuredBuffer<uint4> Wide : register(t1);
ByteAddressBuffer RawInput : register(t2);
RWStructuredBuffer<uint> Mutable : register(u1);
RWByteAddressBuffer RawMutable : register(u2);
Texture1D<float> Line : register(t3);
Texture1DArray<float> Lines : register(t4);
SamplerState Point : register(s0);
RWTexture1DArray<uint> AtomicLines : register(u3);
RWTexture2DArray<uint> AtomicImages : register(u4);
[numthreads(1, 1, 1)]
void cs_bounds() {
  for (uint lane = 0; lane < 4; ++lane) {
    uint index = indices[lane];
    Output.Store(lane * 16, Input[index]);
    Mutable[index] = 20;
    uint old;
    InterlockedAdd(Mutable[index], 3, old);
    Output.Store(lane * 16 + 4, old);
    InterlockedCompareExchange(Mutable[index], 23, 29, old);
    Output.Store(lane * 16 + 8, old);
    Output.Store(lane * 16 + 12, Mutable[index]);
  }
  // The starting byte is out of range; subsequent lanes must not wrap to zero.
  Output.Store4(64, RawInput.Load4(indices.w - 3));
  RawMutable.Store4(indices.w - 3, uint4(91, 92, 93, 94));
  // Stride and member/lane offsets both contribute to the widened address.
  Output.Store4(80, Wide[indices.z >> 2]);
}
[numthreads(1, 1, 1)]
void cs_gradients() {
  Output.Store(0, asuint(Line.SampleGrad(Point, 0.5, 0.25, 0)));
  Output.Store(4, asuint(Line.SampleGrad(Point, 0.5, 0.5, 0)));
  Output.Store(8, asuint(Lines.SampleGrad(Point, float2(0.5, 1), 0.25, 0)));
  Output.Store(12, asuint(Lines.SampleGrad(Point, float2(0.5, 1), 0.5, 0)));
  Output.Store(16, asuint(Line.SampleGrad(Point, 0.5, 0, 0.5)));
  Output.Store(20, asuint(Lines.SampleGrad(Point, float2(0.5, 1), 0, 0.5)));
}
[numthreads(1, 1, 1)]
void cs_array_atomics() {
  for (uint slice = 0; slice < 2; ++slice) {
    uint2 p1 = uint2(1, slice);
    uint3 p2 = uint3(1, 1, slice);
    AtomicLines[p1] = 10 + 100 * slice;
    AtomicImages[p2] = 20 + 100 * slice;
    // Interlocked operations do not imply a fence for ordinary texture accesses.
    DeviceMemoryBarrier();
    uint old;
    uint base = slice * 48;
    InterlockedAdd(AtomicLines[p1], 3, old); Output.Store(base, old);
    InterlockedExchange(AtomicLines[p1], 40 + 100 * slice, old); Output.Store(base + 4, old);
    InterlockedCompareExchange(AtomicLines[p1], 999, 50, old); Output.Store(base + 8, old);
    InterlockedCompareExchange(AtomicLines[p1], 40 + 100 * slice, 60 + 100 * slice, old);
    Output.Store(base + 12, old);
    DeviceMemoryBarrier();
    Output.Store(base + 16, AtomicLines[p1]);
    InterlockedAdd(AtomicImages[p2], 7, old); Output.Store(base + 24, old);
    InterlockedExchange(AtomicImages[p2], 80 + 100 * slice, old); Output.Store(base + 28, old);
    InterlockedCompareExchange(AtomicImages[p2], 999, 50, old); Output.Store(base + 32, old);
    InterlockedCompareExchange(AtomicImages[p2], 80 + 100 * slice, 90 + 100 * slice, old);
    Output.Store(base + 36, old);
    DeviceMemoryBarrier();
    Output.Store(base + 40, AtomicImages[p2]);
  }
  DeviceMemoryBarrier();
  Output.Store(20, AtomicLines[uint2(1, 0)]);
  Output.Store(44, AtomicImages[uint3(1, 1, 0)]);
  Output.Store(68, AtomicLines[uint2(1, 1)]);
  Output.Store(92, AtomicImages[uint3(1, 1, 1)]);
}
#endif
