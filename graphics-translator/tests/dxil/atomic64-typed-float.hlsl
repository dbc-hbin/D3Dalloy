RWByteAddressBuffer output : register(u0);
RWBuffer<uint64_t> cells : register(u1);
RWBuffer<float> scalar : register(u3);
[numthreads(1,1,1)]
void float_atomics() {
  scalar[0] = asfloat(0x80000000);
  scalar[1] = 0.0;
  uint64_t snapshot;
  InterlockedAdd(cells[0], 0ULL, snapshot);
  output.Store<uint64_t>(0, snapshot);
  float old;
  InterlockedExchange(scalar[0], asfloat(0x7fc00123), old);
  output.Store(8, asuint(old));
  InterlockedAdd(cells[0], 0ULL, snapshot);
  output.Store<uint64_t>(16, snapshot);
  InterlockedCompareExchangeFloatBitwise(scalar[0], asfloat(0x7fc00123), 1.5, old);
  output.Store(24, asuint(old));
  InterlockedCompareExchangeFloatBitwise(scalar[0], asfloat(0x80000000), 2.5, old);
  output.Store(28, asuint(old));
  scalar[1] = asfloat(0x80000000);
  InterlockedCompareStoreFloatBitwise(scalar[1], asfloat(0x80000000), asfloat(0x7f800000));
  InterlockedAdd(cells[0], 0ULL, snapshot);
  output.Store<uint64_t>(32, snapshot);
}
