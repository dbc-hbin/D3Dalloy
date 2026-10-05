// Exact binary64 input/output; the binary32 conversions exercise both DXIL denorm modes.
RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);
groupshared double Shared[4];
double getDouble(uint offset) {
  uint2 bits = Raw.Load2(offset);
  return asdouble(bits.x, bits.y);
}
void storeDouble(uint offset, double value) {
  uint lo, hi;
  asuint(value, lo, hi);
  Out.Store2(offset, uint2(lo, hi));
}
void body(uint i, uint lane) {
  uint base = i * 24, o = i * 160;
  double a = getDouble(base), b = getDouble(base + 8), c = getDouble(base + 16);
  Shared[lane] = a;
  GroupMemoryBarrierWithGroupSync();
  storeDouble(o, a + b); storeDouble(o + 8, a - b);
  storeDouble(o + 16, a * b); storeDouble(o + 24, a / b);
  storeDouble(o + 32, fma(a, b, c)); storeDouble(o + 40, -a);
  Out.Store(o + 48, asuint((float)a)); Out.Store(o + 52, (uint)(int)a);
  Out.Store(o + 56, (uint)a);
  Out.Store<int64_t>(o + 64, (int64_t)a); Out.Store<uint64_t>(o + 72, (uint64_t)a);
  Out.Store(o + 80, (a < b ? 1 : 0) | (a == b ? 2 : 0) | (a != b ? 4 : 0) |
      (a <= b ? 8 : 0) | (a > b ? 16 : 0) | (a >= b ? 32 : 0));
  storeDouble(o + 88, (double)(int64_t)Raw.Load<uint64_t>(base));
  storeDouble(o + 96, (double)Raw.Load<uint64_t>(base));
  storeDouble(o + 104, (double)asfloat(Raw.Load(base)));
  storeDouble(o + 112, Shared[(lane + 1) & 3]);
  storeDouble(o + 120, (i & 1) ? a : b);
  storeDouble(o + 128, min(a, b)); storeDouble(o + 136, max(a, b));
  storeDouble(o + 144, abs(a));
}
[numthreads(4, 1, 1)]
void cs_fp64(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) { body(id.x, lane); }
[numthreads(4, 1, 1)]
void cs_fp64_ftz(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) { body(id.x, lane); }

[WaveSize(4)]
[numthreads(4, 1, 1)]
void cs_fp64_wave(uint3 id : SV_DispatchThreadID) {
  double a = getDouble(id.x * 8);
  uint lane = WaveGetLaneIndex(), o = id.x * 64;
  Out.Store(o, WaveActiveAllEqual(a));
  Out.Store(o + 4, lane);
  storeDouble(o + 8, WaveReadLaneFirst(a));
  storeDouble(o + 16, WaveReadLaneAt(a, (lane + 1) & 3));
  storeDouble(o + 24, WaveActiveSum(a));
  storeDouble(o + 32, WaveActiveProduct(a));
  storeDouble(o + 40, WaveActiveMin(a));
  storeDouble(o + 48, WaveActiveMax(a));
}

struct Vertex {
  float4 position : SV_Position;
  nointerpolation uint2 fused : FP64;
};
Vertex vs_fp64(uint id : SV_VertexID) {
  Vertex result;
  result.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
  double fused = fma(getDouble(0), getDouble(8), getDouble(16));
  asuint(fused, result.fused.x, result.fused.y);
  return result;
}
float4 ps_fp64(Vertex input) : SV_Target {
  uint i = (uint)input.position.y * 8 + (uint)input.position.x, base = i * 24, o = i * 32;
  double a = getDouble(base), b = getDouble(base + 8), c = getDouble(base + 16);
  double vertex = asdouble(input.fused.x, input.fused.y), fused = fma(a, b, c);
  storeDouble(o, vertex); storeDouble(o + 8, a + b);
  storeDouble(o + 16, fused); storeDouble(o + 24, fused + vertex);
  return float4(0.0, 1.0, 0.0, 1.0);
}
