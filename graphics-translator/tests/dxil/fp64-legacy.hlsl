// FXC-produced DXBC 5.0/5.1; double values occupy register word pairs, not binary32.
RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);
#if FP64_COMPUTE
groupshared double Shared[4];
#endif
double getDouble(uint offset) {
  uint2 bits = Raw.Load2(offset);
  return asdouble(bits.x, bits.y);
}
void storeDouble(uint offset, double value) {
  uint lo, hi;
  asuint(value, lo, hi);
  Out.Store2(offset, uint2(lo, hi));
}
#if FP64_COMPUTE
[numthreads(4, 1, 1)]
void cs_fp64_legacy(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
  uint i = id.x, base = i * 24, o = i * 160;
  double a = getDouble(base), b = getDouble(base + 8), c = getDouble(base + 16);
  Shared[lane] = a;
  GroupMemoryBarrierWithGroupSync();
  storeDouble(o, a + b); storeDouble(o + 8, a - b);
  storeDouble(o + 16, a * b); storeDouble(o + 24, a / b);
  storeDouble(o + 32, fma(a, b, c)); storeDouble(o + 40, -a);
  Out.Store(o + 48, asuint((float)a)); Out.Store(o + 52, (uint)(int)a);
  Out.Store(o + 56, (uint)a);
  Out.Store(o + 80, (a < b ? 1 : 0) | (a == b ? 2 : 0) | (a != b ? 4 : 0) |
      (a <= b ? 8 : 0) | (a > b ? 16 : 0) | (a >= b ? 32 : 0));
  storeDouble(o + 88, (double)(int)Raw.Load(base));
  storeDouble(o + 96, (double)Raw.Load(base));
  storeDouble(o + 104, (double)asfloat(Raw.Load(base)));
  storeDouble(o + 112, Shared[(lane + 1) & 3]);
  storeDouble(o + 120, (i & 1) ? a : b);
  storeDouble(o + 128, min(a, b)); storeDouble(o + 136, max(a, b));
  storeDouble(o + 144, abs(a));
}
#endif
struct Vertex {
  float4 position : SV_Position;
  nointerpolation uint2 fused : FP64;
};
Vertex vs_fp64_legacy(uint id : SV_VertexID) {
  Vertex result;
  result.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
  // SM5.0 vertex shaders cannot read UAVs. VertexID supplies a runtime sign bit;
  // the three drawn vertices all evaluate (1+2^-52)*(1-2^-52)-1 exactly.
  double a = asdouble(1u, (id & 0x80000000u) | 0x3ff00000u);
  double fused = fma(a, asdouble(0xfffffffeu, 0x3fefffffu), -1.0);
  asuint(fused, result.fused.x, result.fused.y);
  return result;
}
void ps_fp64_legacy(Vertex input) {
  uint i = (uint)input.position.y * 8 + (uint)input.position.x, base = i * 24, o = i * 32;
  double a = getDouble(base), b = getDouble(base + 8), c = getDouble(base + 16);
  double vertex = asdouble(input.fused.x, input.fused.y), fused = fma(a, b, c);
  storeDouble(o, vertex); storeDouble(o + 8, a + b);
  storeDouble(o + 16, fused); storeDouble(o + 24, fused + vertex);
}
