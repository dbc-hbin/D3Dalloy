// Production-valid cache mismatch inputs with identical resource contracts.
// Build each entry with the existing compile-hlsl helper using cs_6_0.
cbuffer Parameters : register(b0) { uint tableIndex; uint operand; };
RWByteAddressBuffer output : register(u0);
static const uint values[8] = {
  0x13579bdf, 0x2468ace0, 0xdeadbeef, 0x31415926,
  0xabcdef01, 0x76543210, 0xfedcba98, 0x55aa33cc
};
[numthreads(1,1,1)] void cacheA() { output.Store(0, values[tableIndex & 7] + operand); }
[numthreads(1,1,1)] void cacheB() { output.Store(0, values[tableIndex & 7] ^ operand); }
