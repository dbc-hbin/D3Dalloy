// Frozen DXIL for the feature-level suite (target-independent rasterization). Built by build-feature-level.sh.
#ifdef FEATURE_SM66
RWByteAddressBuffer rawAtomic : register(u1);
RWStructuredBuffer<uint64_t> structuredAtomic : register(u2);

void admissionBody(uint3 gt, uint gi) {
  RWByteAddressBuffer output = ResourceDescriptorHeap[0];
  uint64_t rawOld, structuredOld;
  rawAtomic.InterlockedAdd64(0, 1ull, rawOld);
  InterlockedAdd(structuredAtomic[0], 1ull, structuredOld);
  uint base = gi * 48;
  output.Store4(base, uint4(WaveGetLaneCount(), WaveActiveSum(1u), WaveGetLaneIndex(), IsHelperLane()));
  output.Store2(base + 16, uint2(asuint(ddx(float(gt.x))), asuint(ddy(float(gt.y)))));
  output.Store2(base + 24, uint2(uint(rawOld), uint(rawOld >> 32)));
  output.Store2(base + 32, uint2(uint(structuredOld), uint(structuredOld >> 32)));
  uint packed = (uint)pack_clamp_u8(int4(gi, -1, 300, 127));
  int4 unpacked = unpack_s8s32((int8_t4_packed)(gi | 0x7fff8000u));
  output.Store2(base + 40, uint2(packed, uint(unpacked.y)));
  if (gi == 0) {
    float old;
    rawAtomic.InterlockedExchangeFloat(8, asfloat(0x7fc12345u), old);
    output.Store(1536, asuint(old));
    rawAtomic.InterlockedCompareExchangeFloatBitwise(8, asfloat(0x7fc12345u), asfloat(0x80000000u), old);
    output.Store(1540, asuint(old));
    rawAtomic.InterlockedCompareStoreFloatBitwise(8, asfloat(0x80000000u), 1.0f);
    output.Store(1544, rawAtomic.Load(8));
  }
}
[WaveSize(4)] [numthreads(8, 4, 1)]
void cs_admission4(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) { admissionBody(gt, gi); }
[WaveSize(8)] [numthreads(8, 4, 1)]
void cs_admission8(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) { admissionBody(gt, gi); }
[WaveSize(16)] [numthreads(8, 4, 1)]
void cs_admission16(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) { admissionBody(gt, gi); }
[WaveSize(32)] [numthreads(8, 4, 1)]
void cs_admission32(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) { admissionBody(gt, gi); }
#else
cbuffer Tri : register(b0) { float4 p01; float4 p2; };
RWStructuredBuffer<uint2> result : register(u0);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 p = id == 0 ? p01.xy : id == 1 ? p01.zw : p2.xy;
  return float4(p, 0, 1);
}

// UAV-only forced raster: one pixel-frequency invocation; SV_Coverage is the full forced-pattern mask.
void ps_coverage(float4 pos : SV_Position, uint coverage : SV_Coverage) {
  uint index = uint(pos.y) * 16 + uint(pos.x);
  InterlockedOr(result[index].x, coverage);
  InterlockedAdd(result[index].y, 1);
}

float4 ps_color(float4 pos : SV_Position, uint coverage : SV_Coverage) : SV_Target {
  uint index = uint(pos.y) * 16 + uint(pos.x);
  InterlockedOr(result[index].x, coverage);
  InterlockedAdd(result[index].y, 1);
  return float4(0.25, 0.5, 0.75, 1);
}
#endif
