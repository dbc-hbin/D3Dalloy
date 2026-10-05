// Shader Model 6.x feature fixture for owned-d3d12-sm6.cpp. Compile each entry
// with DXC 1.9: -HV 2021 -T <profile> -E <entry> [-enable-16bit-types].
// Every result is written to u0 (RWByteAddressBuffer) at the offsets listed per
// entry; LOD cases use analytical targets and independent native mip sampling.

RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);
RWStructuredBuffer<int64_t> S64 : register(u2);
cbuffer Params : register(b0) {
  uint4 p0; // x: divergence modulus, y: seed
};

// ---------------------------------------------------------------------------
// cs_wave (cs_6_5, 64 threads = two 32 lane waves). Per lane record of 32 dwords.
// ---------------------------------------------------------------------------
void waveBody(uint gi) {
  const uint base = gi * 128;
  const uint lane = WaveGetLaneIndex();
  int s = (int)(gi * 2654435761u) >> 8;    // signed values, some negative
  uint u = gi * 2246822519u;               // unsigned values with the top bit set
  float f = (float)((int)(gi % 7) - 3) * 0.5f;
  Out.Store(base + 0, WaveGetLaneCount());
  Out.Store(base + 4, lane);
  Out.Store(base + 8, WaveIsFirstLane() ? 1 : 0);
  Out.Store(base + 12, WaveActiveAllTrue(gi < 1000) ? 1 : 0);
  Out.Store(base + 16, WaveActiveAnyTrue(gi == 37) ? 1 : 0);
  Out.Store4(base + 20, WaveActiveBallot((gi % 3) == 0));
  Out.Store(base + 36, WaveReadLaneAt(u, 5 % WaveGetLaneCount()));
  Out.Store(base + 40, WaveReadLaneFirst(u));
  Out.Store(base + 44, WaveActiveSum(u));
  Out.Store(base + 48, WaveActiveProduct(gi | 1));
  Out.Store(base + 52, (uint)WaveActiveMin(s));
  Out.Store(base + 56, (uint)WaveActiveMax(s));
  Out.Store(base + 60, WaveActiveMin(u));
  Out.Store(base + 64, WaveActiveMax(u));
  Out.Store(base + 68, WaveActiveBitAnd(u | 0x10000000u));
  Out.Store(base + 72, WaveActiveBitOr(u & 0xff00ffu));
  Out.Store(base + 76, WaveActiveBitXor(u));
  Out.Store(base + 80, WaveActiveCountBits((u & 4) != 0));
  Out.Store(base + 84, WavePrefixSum(u));
  Out.Store(base + 88, WavePrefixProduct((gi & 3) + 1));
  Out.Store(base + 92, WavePrefixCountBits((u & 8) != 0));
  Out.Store(base + 96, asuint(WaveActiveSum(f)));
  Out.Store(base + 100, asuint(WaveActiveMin(f)));
  Out.Store(base + 104, QuadReadAcrossX(gi));
  Out.Store(base + 108, QuadReadAcrossY(gi));
  Out.Store(base + 112, QuadReadAcrossDiagonal(gi));
  Out.Store(base + 116, QuadReadLaneAt(gi, 2));
  Out.Store(base + 120, WaveActiveAllEqual(gi / 32) ? 1 : 0);
  Out.Store(base + 124, WaveActiveAllEqual(gi) ? 1 : 0);
}

[numthreads(64, 1, 1)] void cs_wave(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4)] [numthreads(64, 1, 1)] void cs_wave4(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(8)] [numthreads(64, 1, 1)] void cs_wave8(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(16)] [numthreads(64, 1, 1)] void cs_wave16(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4, 128, 4)] [numthreads(64, 1, 1)] void cs_wave_range4(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4, 128, 8)] [numthreads(64, 1, 1)] void cs_wave_range8(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4, 128, 16)] [numthreads(64, 1, 1)] void cs_wave_range16(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4, 128, 32)] [numthreads(64, 1, 1)] void cs_wave_range32(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4, 128, 64)] [numthreads(64, 1, 1)] void cs_wave_range64(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4, 16)] [numthreads(64, 1, 1)] void cs_wave_range16max(uint gi : SV_GroupIndex) { waveBody(gi); }
[WaveSize(4)] [numthreads(64, 1, 1)] void cs_wave_sm68fixed4(uint gi : SV_GroupIndex) { waveBody(gi); }

// ---------------------------------------------------------------------------
// cs_divergent (cs_6_5): wave ops inside divergent control flow; only lanes with
// (gi % p0.x) != 0 are active. Record of 16 dwords per lane; inactive lanes write
// a sentinel.
// ---------------------------------------------------------------------------
void divergentBody(uint gi) {
  const uint base = gi * 64;
  int s = (int)(gi * 2654435761u) >> 8;
  uint u = gi * 2246822519u;
  if ((gi % p0.x) != 0) {
    uint4 match = WaveMatch(gi % 5);
    Out.Store(base + 0, WaveActiveCountBits(true));
    Out.Store(base + 4, WaveIsFirstLane() ? 1 : 0);
    Out.Store(base + 8, WaveReadLaneFirst(gi));
    Out.Store(base + 12, WaveActiveSum(u));
    Out.Store(base + 16, (uint)WaveActiveMin(s));
    Out.Store(base + 20, WaveActiveMax(u));
    Out.Store(base + 24, WavePrefixSum(gi));
    Out.Store(base + 28, WavePrefixCountBits(true));
    Out.Store(base + 32, WaveActiveBallot(true).x);
    Out.Store(base + 36, match.x);
    Out.Store(base + 40, WaveMultiPrefixSum(gi, match));
    Out.Store(base + 44, WaveMultiPrefixCountBits((gi & 1) != 0, match));
    Out.Store(base + 48, WaveMultiPrefixProduct((gi & 3) + 1, match));
    Out.Store(base + 52, WaveMultiPrefixBitOr(1u << (gi & 31), match));
    Out.Store(base + 56, WaveMultiPrefixBitXor(u, match));
    Out.Store(base + 60, WaveMultiPrefixBitAnd(~(1u << (gi & 31)), match));
  } else {
    for (uint i = 0; i < 64; i += 4)
      Out.Store(base + i, 0xdeadbeefu);
  }
}

// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)] void cs_divergent(uint gi : SV_GroupIndex) { divergentBody(gi); }
[WaveSize(4)] [numthreads(64, 1, 1)] void cs_divergent4(uint gi : SV_GroupIndex) { divergentBody(gi); }
[WaveSize(8)] [numthreads(64, 1, 1)] void cs_divergent8(uint gi : SV_GroupIndex) { divergentBody(gi); }
[WaveSize(16)] [numthreads(64, 1, 1)] void cs_divergent16(uint gi : SV_GroupIndex) { divergentBody(gi); }

// cs_int64 (cs_6_6): 64 bit arithmetic, shifts, conversions, wave ops and raw
// 64 bit loads/stores. Raw holds 64 input qwords; 16 qwords of output per lane.
// ---------------------------------------------------------------------------
void int64Body(uint gi) {
  const uint base = gi * 128;
  int64_t a = Raw.Load<int64_t>(gi * 8);
  int64_t b = Raw.Load<int64_t>(((gi * 7 + 3) & 63) * 8);
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  uint shift = (gi * 13) & 63;
  int64_t bd = (b == 0 || (a == (int64_t)0x8000000000000000ull && b == -1)) ? 3 : b;
  uint64_t ubd = ub == 0 ? 5 : ub;
  Out.Store<int64_t>(base + 0, a + b);
  Out.Store<int64_t>(base + 8, a - b);
  Out.Store<int64_t>(base + 16, a * b);
  Out.Store<int64_t>(base + 24, a / bd);
  Out.Store<int64_t>(base + 32, a % bd);
  Out.Store<uint64_t>(base + 40, ua / ubd);
  Out.Store<uint64_t>(base + 48, ua % ubd);
  Out.Store<int64_t>(base + 56, a >> shift);
  Out.Store<uint64_t>(base + 64, ua >> shift);
  Out.Store<uint64_t>(base + 72, ua << shift);
  Out.Store<int64_t>(base + 80, min(a, b) ^ max(a, b));
  Out.Store<uint64_t>(base + 88, min(ua, ub) + countbits(ua) + firstbithigh(ua) + firstbitlow(ua) + reversebits(ua));
  Out.Store<int64_t>(base + 96, (int64_t)(int)(a >> 32) + (int64_t)(uint)ua + (int64_t)(float)(int)(a & 0xffff));
  Out.Store<uint64_t>(base + 104, WaveActiveSum(ua) ^ WaveActiveMax(ua));
  Out.Store<int64_t>(base + 112, WaveActiveMin(a) + WavePrefixSum(a));
  Out.Store<uint64_t>(base + 120, WaveReadLaneAt(ua, 7 % WaveGetLaneCount()) + WaveActiveBitXor(ub));
}

[numthreads(64, 1, 1)] void cs_int64(uint gi : SV_GroupIndex) { int64Body(gi); }
[WaveSize(4)] [numthreads(64, 1, 1)] void cs_int64wave4(uint gi : SV_GroupIndex) { int64Body(gi); }
[WaveSize(8)] [numthreads(64, 1, 1)] void cs_int64wave8(uint gi : SV_GroupIndex) { int64Body(gi); }
[WaveSize(16)] [numthreads(64, 1, 1)] void cs_int64wave16(uint gi : SV_GroupIndex) { int64Body(gi); }

// ---------------------------------------------------------------------------
// cs_atomic64 (cs_6_6): contended 64 bit atomics through the lock table. Out
// qwords [0, 16) are the targets; each thread records its returned values.
// Dispatch: X groups of 1024 threads; p0.x = number of target addresses (1 or 16),
// p0.y = kind of the divergent branch.
// ---------------------------------------------------------------------------
void atomic64Body(uint id) {
  const uint target = (id * 2654435761u >> 7) % p0.x;
  uint64_t old;
  // Every thread adds 1: returned values form a permutation of 0..N-1 per target.
  Raw.InterlockedAdd64(target * 8, 1, old);
  Out.Store<uint64_t>(id * 16, old);
  // Divergent: odd threads run max/min/or on a second table; even threads exchange.
  uint64_t other;
  if (id & 1) {
    if ((id >> 1) & 1)
      Raw.InterlockedMax64(128 + target * 8, (uint64_t)id << 20, other);
    else
      Raw.InterlockedOr64(256 + target * 8, 1ull << (id & 63), other);
  } else {
    Raw.InterlockedCompareExchange64(384 + target * 8, 0, (uint64_t)id + 1, other);
  }
  Out.Store<uint64_t>(id * 16 + 8, other);
}
[numthreads(1024, 1, 1)]
void cs_atomic64(uint3 dt : SV_DispatchThreadID) { atomic64Body(dt.x); }

// cs_atomic64_ops (cs_6_6): single thread semantics of every op, edge values.
[numthreads(1, 1, 1)]
void cs_atomic64_ops() {
  uint64_t r[16];
  Raw.InterlockedAdd64(0, 0xffffffffull, r[0]);
  Raw.InterlockedAnd64(8, 0xf0f0f0f00f0f0f0full, r[1]);
  Raw.InterlockedOr64(16, 0x8000000000000001ull, r[2]);
  Raw.InterlockedXor64(24, 0xffffffffffffffffull, r[3]);
  int64_t sr[4];
  Raw.InterlockedMin64(32, (int64_t)-5, sr[0]);
  Raw.InterlockedMax64(40, (int64_t)0x8000000000000000ull, sr[1]);
  Raw.InterlockedMin64(48, 3ull, r[4]);
  Raw.InterlockedMax64(56, 0xfffffffffffffffeull, r[5]);
  Raw.InterlockedExchange64(64, 0x123456789abcdef0ull, r[6]);
  Raw.InterlockedCompareExchange64(72, 7, 8, r[7]);  // matches
  Raw.InterlockedCompareExchange64(80, 7, 8, r[8]);  // does not match
  Raw.InterlockedCompareStore64(88, 0x100000000ull, 42);
  InterlockedMax(S64[1], (int64_t)-1, sr[2]);
  InterlockedAdd(S64[2], (int64_t)-1, sr[3]);
  Raw.InterlockedAdd64(Raw.Load(96) * 0 + 4096 * 1024, 1, r[9]); // far out of range: no access
  for (uint i = 0; i < 10; ++i)
    Out.Store<uint64_t>(i * 8, r[i]);
  for (uint j = 0; j < 4; ++j)
    Out.Store<int64_t>(80 + j * 8, sr[j]);
}

// ---------------------------------------------------------------------------
// cs_minprec (cs_6_0, no -enable-16bit-types): min16float/min16int/min16uint.
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void cs_minprec(uint gi : SV_GroupIndex) {
  min16float a = (min16float)((float)gi * 0.25f);
  min16int b = (min16int)((int)gi - 32);
  min16uint c = (min16uint)(gi * 3);
  Out.Store(gi * 12 + 0, asuint((float)(a * (min16float)2.0 + (min16float)1.0)));
  Out.Store(gi * 12 + 4, (uint)(int)(b * (min16int)3 - (min16int)1));
  Out.Store(gi * 12 + 8, (uint)(c + (min16uint)7));
}

// ---------------------------------------------------------------------------
// cs_quad_deriv (cs_6_8): compute-shader quad derivatives (2D quad rule), QuadAny/
// QuadAll, IsHelperLane (false in compute), implicit LOD sampling and
// CalculateLevelOfDetail, BarrierByMemoryType / Handle. T0 is 64x64 R32_FLOAT with
// 7 mips, mip m filled with m + 1; S0 is point filtered, S1 is mip-linear.
// ---------------------------------------------------------------------------
groupshared uint Shared[64];
Texture2D<float> T0 : register(t0);
SamplerState S0 : register(s0);
SamplerState S1 : register(s1);
[numthreads(8, 8, 1)]
void cs_quad_deriv(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) {
  const uint base = gi * 48;
  // Texel-space gradients (4, 0) and (0, 2): LOD log2(4) = 2. uv_far: (256, 0) → LOD 8.
  const float2 uv = float2(gt.x * 4.0f, gt.y * 2.0f) / 64.0f;
  const float2 uv_far = float2(gt.x * 256.0f, 0.0f) / 64.0f;
  Out.Store(base + 24, asuint(T0.CalculateLevelOfDetail(S1, uv)));
  Out.Store(base + 28, asuint(T0.CalculateLevelOfDetailUnclamped(S1, uv)));
  Out.Store(base + 32, asuint(T0.CalculateLevelOfDetail(S1, uv_far)));
  Out.Store(base + 36, asuint(T0.CalculateLevelOfDetailUnclamped(S1, uv_far)));
  Out.Store(base + 40, asuint(T0.Sample(S0, uv)));
  Out.Store(base + 44, asuint(T0.SampleBias(S0, uv, 1.0f)));
  float v = (float)(gt.x * gt.x) + 10.0f * (float)(gt.y * gt.y * gt.y);
  Out.Store(base + 0, asuint(ddx_fine(v)));
  Out.Store(base + 4, asuint(ddy_fine(v)));
  Out.Store(base + 8, QuadAny(gt.x == 1 && gt.y == 1) ? 1 : 0);
  Out.Store(base + 12, QuadAll(((gt.x + gt.y) & 7) != 3) ? 1 : 0);
  Out.Store(base + 16, (IsHelperLane() ? 1 : 0) | (gt.x << 8) | (gt.y << 16));
  Shared[gi] = gi * 3;
  Barrier(GROUP_SHARED_MEMORY, GROUP_SCOPE | GROUP_SYNC);
  uint neighbour = Shared[63 - gi];
  Raw.Store(gi * 4, neighbour);
  Barrier(Raw, DEVICE_SCOPE);
  Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
  Out.Store(base + 20, Raw.Load((63 - gi) * 4));
}

// ---------------------------------------------------------------------------
// Independent LOD regressions. Every quad is complete, with uniform control flow.
cbuffer LodParams : register(b1) {
  float4 lodBase;
  float4 lodDx;
  float4 lodDy;
};
[numthreads(2, 2, 1)]
void cs_lod(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) {
  float2 uv = (lodBase + gt.x * lodDx + gt.y * lodDy).xy;
  Out.Store(gi * 12, asuint(T0.CalculateLevelOfDetail(S1, uv)));
  Out.Store(gi * 12 + 4, asuint(T0.CalculateLevelOfDetailUnclamped(S1, uv)));
  // Sample uses native GPU filtering, not the CalculateLOD lowering under test.
  // Each resource mip m contains m + 1; p0.x is the view base mip.
  if (p0.y != 0)
    Out.Store(gi * 12 + 8, asuint(T0.Sample(S1, uv) - 1.0f - (float)p0.x));
}
Texture1D<float> Lod1D : register(t1);
Texture1DArray<float> Lod1DArray : register(t2);
Texture2DArray<float> Lod2DArray : register(t3);
Texture3D<float> Lod3D : register(t4);
TextureCube<float> LodCube : register(t5);
TextureCubeArray<float> LodCubeArray : register(t6);
[numthreads(2, 2, 1)]
void cs_lod_shapes(uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) {
  const uint base = gi * 64;
  float u = 0.25f + gt.x * (4.0f / 64.0f);
  float slice = (float)(gt.x + 2 * gt.y);
  float3 a = float3(0.25f + gt.x * (8.0f / 64.0f),
                    0.25f + gt.y * (2.0f / 32.0f), slice);
  float3 v = float3(0.25f + gt.x * (2.0f / 64.0f), 0.25f,
                    0.25f + gt.y * (4.0f / 8.0f));
  float3 direction = (lodBase + gt.x * lodDx + gt.y * lodDy).xyz;
  // Array LOD APIs take spatial coordinates only: these query array-view shape
  // semantics. Only the independent native Sample below receives a slice index.
  Out.Store(base, asuint(Lod1D.CalculateLevelOfDetail(S1, u)));
  Out.Store(base + 4, asuint(Lod1D.CalculateLevelOfDetailUnclamped(S1, u)));
  Out.Store(base + 8, asuint(Lod1DArray.CalculateLevelOfDetail(S1, u)));
  Out.Store(base + 12, asuint(Lod1DArray.CalculateLevelOfDetailUnclamped(S1, u)));
  Out.Store(base + 16, asuint(Lod2DArray.CalculateLevelOfDetail(S1, a.xy)));
  Out.Store(base + 20, asuint(Lod2DArray.CalculateLevelOfDetailUnclamped(S1, a.xy)));
  Out.Store(base + 24, asuint(Lod3D.CalculateLevelOfDetail(S1, v)));
  Out.Store(base + 28, asuint(Lod3D.CalculateLevelOfDetailUnclamped(S1, v)));
  Out.Store(base + 32, asuint(LodCube.CalculateLevelOfDetail(S1, direction)));
  Out.Store(base + 36, asuint(LodCube.CalculateLevelOfDetailUnclamped(S1, direction)));
  Out.Store(base + 40, asuint(LodCubeArray.CalculateLevelOfDetail(S1, direction)));
  Out.Store(base + 44, asuint(LodCubeArray.CalculateLevelOfDetailUnclamped(S1, direction)));
  if (p0.y != 0)
    Out.Store(base + 48, asuint(Lod2DArray.Sample(S1, a) - 1.0f));
}

// ---------------------------------------------------------------------------
// Graphics: SM 6.8 start locations, SM 6.1 barycentrics / attribute at vertex,
// SM 6.6 IsHelperLane, SM 6.7 QuadAny/QuadAll and helper exclusion in wave ops.
// ---------------------------------------------------------------------------
struct VSOut {
  float4 pos : SV_Position;
  nointerpolation uint4 ids : IDS;
  nointerpolation float vtx : VTX;
  float grad : GRAD;
};

VSOut vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID, uint sv : SV_StartVertexLocation,
              uint si : SV_StartInstanceLocation) {
  // Full screen triangle; vertex n carries value 100 + n.
  VSOut o;
  const uint corner = vid % 3;
  float2 xy = float2(corner == 1 ? 3.0f : -1.0f, corner == 2 ? -3.0f : 1.0f);
  o.pos = float4(xy, 0.0f, 1.0f);
  o.ids = uint4(vid, iid, sv, si);
  o.vtx = 100.0f + (float)corner;
  o.grad = xy.x;
  return o;
}

// 8x8 target. Every pixel writes a 32 dword record to u0.
float4 ps_main(VSOut i, float3 bary : SV_Barycentrics, noperspective float3 bary_np : SV_Barycentrics1) : SV_Target {
  const uint2 px = (uint2)i.pos.xy;
  const uint base = (px.y * 8 + px.x) * 128;
  Out.Store4(base + 0, i.ids);
  Out.Store3(base + 16, asuint(bary));
  Out.Store(base + 28, asuint(bary_np.x + bary_np.y + bary_np.z));
  Out.Store(base + 32, asuint(GetAttributeAtVertex(i.vtx, 0)));
  Out.Store(base + 36, asuint(GetAttributeAtVertex(i.vtx, 1)));
  Out.Store(base + 40, asuint(GetAttributeAtVertex(i.vtx, 2)));
  Out.Store(base + 44, IsHelperLane() ? 1 : 0);
  // Every pixel of the 8x8 target is covered, so waves hold whole quads.
  Out.Store(base + 48, WaveActiveCountBits(true));
  Out.Store(base + 52, QuadAny(px.x == 0 && px.y == 0) ? 1 : 0);
  Out.Store(base + 56, QuadAll(px.x != 3 || px.y != 2) ? 1 : 0);
  Out.Store(base + 60, asuint(ddx_fine(i.grad)));
  Out.Store(base + 64, QuadReadAcrossDiagonal(px.x + 10 * px.y));
  Out.Store3(base + 68, asuint(bary_np));
  return float4(bary, 1.0f);
}

// One triangle covering only the centre of pixel (5, 2): its quad (4..5, 2..3) has
// three helper lanes.
VSOut vs_tiny(uint vid : SV_VertexID) {
  VSOut o = (VSOut)0;
  const float2 centre = float2(0.375f, 0.375f);
  const float2 offsets[3] = {float2(-0.05f, -0.05f), float2(0.1f, -0.05f), float2(-0.05f, 0.1f)};
  o.pos = float4(centre + offsets[vid % 3], 0.0f, 1.0f);
  return o;
}

// Helper lanes are excluded from wave operations unless WaveOpsIncludeHelperLanes
// is set, and always take part in quad operations.
void helperBody(VSOut i, uint base) {
  const uint2 px = (uint2)i.pos.xy;
  base += (px.y * 8 + px.x) * 32;
  uint helper = IsHelperLane() ? 1 : 0;
  Out.Store(base + 0, WaveActiveSum(1u));
  Out.Store(base + 4, WaveActiveCountBits(true));
  Out.Store(base + 8, countbits(WaveActiveBallot(true).x));
  Out.Store(base + 12, helper);
  Out.Store(base + 16, QuadReadAcrossX(helper) + QuadReadAcrossY(helper) * 2 + QuadReadAcrossDiagonal(helper) * 4);
  Out.Store(base + 20, QuadAny(helper != 0) ? 1 : 0);
  Out.Store(base + 24, WaveActiveAllTrue(helper == 0) ? 1 : 0);
  Out.Store(base + 28, WaveIsFirstLane() ? 1 : 0);
}
float4 ps_helper(VSOut i) : SV_Target {
  helperBody(i, 4096);
  return 0.0f.xxxx;
}
[WaveOpsIncludeHelperLanes]
float4 ps_helper_all(VSOut i) : SV_Target {
  helperBody(i, 4096);
  return 0.0f.xxxx;
}

// ---------------------------------------------------------------------------
// vs_int64 / ps_int64 (6.0): 64 bit integer arithmetic in the graphics stages
// (Int64ShaderOps). The VS passes 64 bit products through two uint varyings; the PS
// recombines them and stores 64 bit results per pixel at (py * 8 + px) * 32.
// ---------------------------------------------------------------------------
struct V64 {
  float4 pos : SV_Position;
  nointerpolation uint2 wide : WIDE;
};
V64 vs_int64(uint vid : SV_VertexID) {
  V64 o;
  const uint corner = vid % 3;
  o.pos = float4(corner == 1 ? 3.0f : -1.0f, corner == 2 ? -3.0f : 1.0f, 0.0f, 1.0f);
  const int64_t a = (int64_t)0x7fffffff00000001ll * (int64_t)(vid + 3) - (int64_t)0x123456789ll;
  o.wide = uint2((uint)a, (uint)((uint64_t)a >> 32));
  return o;
}
float4 ps_int64(V64 i) : SV_Target {
  const uint2 px = (uint2)i.pos.xy;
  const uint base = (px.y * 8 + px.x) * 32;
  const int64_t a = (int64_t)(((uint64_t)i.wide.y << 32) | i.wide.x);
  const int64_t b = (int64_t)(px.x + 1) * -0x100000003ll + (int64_t)px.y;
  Out.Store<int64_t>(base + 0, a);
  Out.Store<int64_t>(base + 8, a * b + (a >> 7));
  Out.Store<uint64_t>(base + 16, (uint64_t)a / ((uint64_t)b | 1) ^ ((uint64_t)b << (px.x * 7)));
  Out.Store<int64_t>(base + 24, min(a, b) - (a % (b | 1)));
  return 0.0f.xxxx;
}

// ---------------------------------------------------------------------------
// vs_wave (vs_6_5): wave ops in a vertex shader. A draw of 45 vertices (not a multiple of
// 32) leaves partially filled waves. Each vertex writes a 12 dword record at vid * 48; the
// host regroups records by WaveReadLaneFirst(vid) and checks each wave against its members.
// ---------------------------------------------------------------------------
float4 vs_wave(uint vid : SV_VertexID) : SV_Position {
  const int s = (int)(vid * 2654435761u) >> 8;
  const uint base = vid * 48;
  Out.Store(base + 0, vid);
  Out.Store(base + 4, WaveGetLaneIndex());
  Out.Store(base + 8, WaveActiveBallot(true).x);
  Out.Store(base + 12, WaveReadLaneFirst(vid));
  Out.Store(base + 16, WaveActiveCountBits(true));
  Out.Store(base + 20, WaveActiveSum(vid));
  Out.Store(base + 24, WavePrefixSum(vid));
  Out.Store(base + 28, (uint)WaveActiveMin(s));
  Out.Store(base + 32, (uint)WaveActiveMax(s));
  Out.Store(base + 36, WaveActiveBallot((vid & 1) != 0).x);
  Out.Store(base + 40, WavePrefixCountBits((vid % 3) == 0));
  Out.Store(base + 44, WaveGetLaneCount());
  return float4(0.0f, 0.0f, 0.0f, 1.0f);
}
float4 ps_null() : SV_Target { return 0.0f.xxxx; }

// ---------------------------------------------------------------------------
// vs_viewid / ps_viewid (6.1): SV_ViewID with view instancing off (no VIEW_INSTANCING
// subobject) is 0 in every stage. The VS stores it at vid * 4 (3 dwords) and passes it
// on; the PS stores its own SV_ViewID and the VS value per pixel at 64 + pixel * 8.
// ---------------------------------------------------------------------------
struct VView {
  float4 pos : SV_Position;
  nointerpolation uint view : VIEW;
};
VView vs_viewid(uint vid : SV_VertexID, uint view : SV_ViewID) {
  VView o;
  const uint corner = vid % 3;
  o.pos = float4(corner == 1 ? 3.0f : -1.0f, corner == 2 ? -3.0f : 1.0f, 0.0f, 1.0f);
  o.view = view;
  Out.Store(vid * 4, view);
  return o;
}
float4 ps_viewid(VView i, uint view : SV_ViewID) : SV_Target {
  const uint2 px = (uint2)i.pos.xy;
  Out.Store2(64 + (px.y * 8 + px.x) * 8, uint2(view, i.view));
  return float4((float)view, (float)i.view, 0.0f, 1.0f);
}
