// Mesh/amplification shaders for owned-d3d12-mesh-shader.cpp (frozen by build-mesh-shader.sh).
// Render target: 64x64 unless noted. Root signature: b0 = 4 root constants, u0 = UAV (raw), t0 = SRV (raw).

cbuffer Constants : register(b0) {
  uint4 k;
};
RWByteAddressBuffer counters : register(u0);
ByteAddressBuffer input : register(t0);

struct VOut {
  float4 pos : SV_Position;
  float4 color : COLOR0;
};

struct VOutClip {
  float4 pos : SV_Position;
  float4 color : COLOR0;
  float clip : SV_ClipDistance0;
};

struct POut {
  nointerpolation float4 prim_color : PRIMCOLOR;
  uint id : SV_PrimitiveID;
};

struct PCull {
  bool cull : SV_CullPrimitive;
};

struct PLayer {
  uint layer : SV_RenderTargetArrayIndex;
  uint viewport : SV_ViewportArrayIndex;
  nointerpolation uint tag : TAG;
};

float4 pixelToNdc(float2 p, float2 size) {
  return float4(p.x / size.x * 2 - 1, 1 - p.y / size.y * 2, 0, 1);
}

// ---- ms_basic: one full-target triangle pair with per-vertex colors (red left, green right). -------------
[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_basic(uint tid : SV_GroupThreadID, out vertices VOut verts[4], out indices uint3 tris[2]) {
  SetMeshOutputCounts(4, 2);
  const float2 corner = float2(tid & 1, tid >> 1);
  verts[tid].pos = float4(corner.x * 2 - 1, 1 - corner.y * 2, 0.5, 1);
  verts[tid].color = float4(corner.x, 0.25, 0.75, 1);
  if (tid < 2)
    tris[tid] = tid == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
}

// ---- ms_grid: SV_GroupID / SV_DispatchThreadID tile; group (x, y) draws the 8x8 tile (x, y) with color. --
[outputtopology("triangle")]
[numthreads(2, 2, 1)]
void ms_grid(uint3 gid : SV_GroupID, uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex,
             out vertices VOut verts[4], out indices uint3 tris[2]) {
  SetMeshOutputCounts(4, 2);
  const float2 corner = float2(dtid.xy - gid.xy * 2);
  const float2 p = (float2(gid.xy) + corner) * 8;
  verts[gi].pos = pixelToNdc(p, float2(64, 64));
  verts[gi].color = float4(gid.x / 255.0, gid.y / 255.0, (k.x + gid.z) / 255.0, 1);
  if (gi < 2)
    tris[gi] = gi == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
}

// ---- as_main + ms_payload: each AS group (32 threads) culls odd tiles, launches one MS group per -----------
// surviving tile of its row, and passes the tile list in a 16 KiB payload; the MS reads the last word too.
struct Payload {
  uint row;
  uint count;
  uint tiles[8];
  uint pad[4085];
  uint tail;
};
groupshared Payload payload;

[numthreads(32, 1, 1)]
void as_main(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {
  if (gi == 0) {
    payload.row = gid.x;
    payload.count = 0;
    payload.tail = 0xabc00000u | gid.x;
  }
  GroupMemoryBarrierWithGroupSync();
  if (gi < 8 && ((gi + gid.x) & 1) == 0) {
    uint slot;
    InterlockedAdd(payload.count, 1, slot);
    payload.tiles[slot] = gi;
  }
  for (uint i = gi; i < 4085; i += 32)
    payload.pad[i] = i;
  GroupMemoryBarrierWithGroupSync();
  counters.InterlockedAdd(0, 1);
  DispatchMesh(payload.count, 1, 1, payload);
}

[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_payload(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID, in payload Payload p,
                out vertices VOut verts[4], out indices uint3 tris[2]) {
  SetMeshOutputCounts(4, 2);
  const uint tile = p.tiles[gid.x];
  const bool ok = p.tail == (0xabc00000u | p.row) && p.pad[4084] == 4084 && p.pad[gi * 1000] == gi * 1000 &&
                  gid.y == 0 && gid.z == 0;
  const float2 corner = float2(gi & 1, gi >> 1);
  verts[gi].pos = pixelToNdc((float2(tile, p.row) + corner) * 8, float2(64, 64));
  verts[gi].color = ok ? float4(tile / 255.0, p.row / 255.0, 1, 1) : float4(1, 0, 1, 1);
  if (gi < 2)
    tris[gi] = gi == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
  if (gi == 0)
    counters.InterlockedAdd(4, 1);
}

// ---- ms_cull: 8 tiles in one row; SV_CullPrimitive culls the triangles of odd tiles. -----------------------
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_cull(uint gi : SV_GroupIndex, out vertices VOut verts[32], out indices uint3 tris[16],
             out primitives PCull prims[16]) {
  SetMeshOutputCounts(32, 16);
  const uint tile = gi >> 2;
  const float2 corner = float2(gi & 1, (gi >> 1) & 1);
  verts[gi].pos = pixelToNdc((float2(tile, 0) + corner) * 8, float2(64, 64));
  verts[gi].color = float4(0, 1, 0, 1);
  if (gi < 16) {
    const uint base = (gi >> 1) * 4;
    tris[gi] = (gi & 1) == 0 ? uint3(base, base + 1, base + 2) : uint3(base + 1, base + 3, base + 2);
    prims[gi].cull = ((gi >> 1) & 1) != 0;
  }
}

// ---- ms_prim + ps_prim: per-primitive nointerpolation color and SV_PrimitiveID (written by the MS). -------
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_prim(uint gi : SV_GroupIndex, out vertices VOut verts[32], out indices uint3 tris[16],
             out primitives POut prims[16]) {
  SetMeshOutputCounts(32, 16);
  const uint tile = gi >> 2;
  const float2 corner = float2(gi & 1, (gi >> 1) & 1);
  verts[gi].pos = pixelToNdc((float2(tile, 1) + corner) * 8, float2(64, 64));
  verts[gi].color = float4(0, 0, 0, 1);
  if (gi < 16) {
    const uint base = (gi >> 1) * 4;
    tris[gi] = (gi & 1) == 0 ? uint3(base, base + 1, base + 2) : uint3(base + 1, base + 3, base + 2);
    prims[gi].prim_color = float4(gi / 255.0, 0.5, 0.25, 1);
    prims[gi].id = 100 + gi;
  }
}

float4 ps_prim(VOut v, POut p) : SV_Target {
  return float4(p.prim_color.x, p.prim_color.y, (p.id - 100) / 255.0, v.color.w);
}

// ---- ms_layer + ps_layer: SV_RenderTargetArrayIndex and SV_ViewportArrayIndex per primitive. --------------
// Primitive i covers the whole 16x16 viewport; it goes to layer i & 3 and viewport i >> 2 (4 viewports).
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_layer(uint gi : SV_GroupIndex, out vertices VOut verts[3], out indices uint3 tris[16],
              out primitives PLayer prims[16]) {
  SetMeshOutputCounts(3, 16);
  if (gi < 3) {
    verts[gi].pos = float4(gi == 1 ? 3 : -1, gi == 2 ? -3 : 1, 0, 1);
    verts[gi].color = float4(0, 0, 0, 1);
  }
  if (gi < 16) {
    tris[gi] = uint3(0, 1, 2);
    prims[gi].layer = gi & 3;
    prims[gi].viewport = gi >> 2;
    prims[gi].tag = gi;
  }
}

float4 ps_layer(VOut v, PLayer p) : SV_Target {
  return float4(p.tag / 255.0, p.layer / 255.0, p.viewport / 255.0, 1);
}

// ---- ms_lines: line topology; 8 horizontal lines at y = 2 + 4 i, each with its own color. ----------------
[outputtopology("line")]
[numthreads(16, 1, 1)]
void ms_lines(uint gi : SV_GroupIndex, out vertices VOut verts[16], out indices uint2 lines[8]) {
  SetMeshOutputCounts(16, 8);
  const uint line_index = gi >> 1;
  verts[gi].pos = pixelToNdc(float2((gi & 1) ? 64 : 0, 2.5 + 4 * line_index), float2(64, 64));
  verts[gi].color = float4(line_index / 255.0, 1, 0, 1);
  if (gi < 8)
    lines[gi] = uint2(gi * 2, gi * 2 + 1);
}

// ---- ms_max: 128 threads, 256 vertices, 256 primitives, groupshared staging. -----------------------------
// The vertices are a 16x16 lattice with 4-pixel spacing starting at pixel (0, 0), stored in reverse order
// through groupshared staging; the 256 triangles cover the first 128 lattice cells (cell c at (c % 15, c / 15)),
// two per cell, each with a per-primitive color naming the cell and half.
groupshared float2 staging[256];
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void ms_max(uint gi : SV_GroupIndex, out vertices VOut verts[256], out indices uint3 tris[256],
            out primitives POut prims[256]) {
  SetMeshOutputCounts(256, 256);
  for (uint v = gi; v < 256; v += 128)
    staging[v] = float2(v % 16, v / 16) * 4;
  GroupMemoryBarrierWithGroupSync();
  for (uint v = gi; v < 256; v += 128) {
    verts[v].pos = pixelToNdc(staging[255 - v], float2(64, 64));
    verts[v].color = float4(0, 0, 0, 1);
  }
  for (uint p = gi; p < 256; p += 128) {
    const uint cell = p >> 1;
    const uint x = cell % 15, y = cell / 15;
    // Lattice point (x, y) lives in vertex slot 255 - (y * 16 + x).
    const uint tl = 255 - (y * 16 + x), tr = tl - 1, bl = tl - 16, br = tl - 17;
    tris[p] = (p & 1) == 0 ? uint3(tl, tr, bl) : uint3(tr, br, bl);
    prims[p].prim_color = float4(cell / 255.0, (p & 1) / 255.0, 1, 1);
    prims[p].id = p;
  }
}

float4 ps_max(VOut v, POut p) : SV_Target {
  return p.prim_color;
}

// ---- ms_count: an empty mesh group that counts invocations (grid limit check). ---------------------------
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void ms_count(uint3 gid : SV_GroupID, out vertices VOut verts[3], out indices uint3 tris[1]) {
  // A degenerate triangle: it covers no pixel, so only the counters observe the group.
  SetMeshOutputCounts(3, 1);
  for (uint v = 0; v < 3; ++v) {
    verts[v].pos = float4(0, 0, 0, 1);
    verts[v].color = float4(0, 0, 0, 0);
  }
  tris[0] = uint3(0, 1, 2);
  counters.InterlockedAdd(0, 1);
  if (gid.x == 65534)
    counters.InterlockedAdd(4, 1);
  if (gid.y == 63)
    counters.InterlockedAdd(8, 1);
}

// ---- as_count + ms_count: AS groups that each launch k.x x k.y x k.z mesh groups. -----------------------
struct SmallPayload {
  uint value;
};
groupshared SmallPayload small;
[numthreads(1, 1, 1)]
void as_count(uint3 gid : SV_GroupID) {
  small.value = gid.x;
  DispatchMesh(k.x, k.y, k.z, small);
}

// ---- ps_color: vertex color passthrough. -------------------------------------------------------------------
float4 ps_color(VOut v) : SV_Target {
  return v.color;
}

// ---- ms_partial: components of one output row written in different places and a dynamically indexed -------
// vertex output row (staged through threadgroup memory), plus SV_ClipDistance clipping the right half.
struct VOutRows {
  float4 pos : SV_Position;
  float4 color[2] : COLOR0;
  float clip : SV_ClipDistance0;
};
[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_partial(uint tid : SV_GroupThreadID, out vertices VOutRows verts[4], out indices uint3 tris[2]) {
  SetMeshOutputCounts(4, 2);
  const float2 corner = float2(tid & 1, tid >> 1);
  verts[tid].pos = float4(corner.x * 2 - 1, 1 - corner.y * 2, 0.5, 1);
  // x == 0 clips at pixel column 32 (clip = 1 - 2 * corner.x, interpolated).
  verts[tid].clip = 1 - 2 * corner.x;
  verts[tid].color[k.x].xy = float2(0.5, 0.25);
  if (tid < 2)
    tris[tid] = tid == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
  GroupMemoryBarrierWithGroupSync();
  verts[tid].color[k.x].zw = float2(1, 1);
  verts[tid].color[1 - k.x] = float4(0, 0, 0, 0);
}

float4 ps_rows(float4 pos : SV_Position, float4 color0 : COLOR0, float4 color1 : COLOR1) : SV_Target {
  return color0 + color1;
}

// ---- ms_viewid: a full-target quad whose color names SV_ViewID (view instancing replay per view). ---------
[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_viewid(uint tid : SV_GroupThreadID, uint view : SV_ViewID, out vertices VOut verts[4],
               out indices uint3 tris[2]) {
  SetMeshOutputCounts(4, 2);
  const float2 corner = float2(tid & 1, tid >> 1);
  verts[tid].pos = float4(corner.x * 2 - 1, 1 - corner.y * 2, 0.5, 1);
  verts[tid].color = float4((view + 1) / 255.0, 0, 1, 1);
  if (tid < 2)
    tris[tid] = tid == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
}

// ---- ms_rtai: primitive i writes SV_RenderTargetArrayIndex = k.x + i (the full 11-bit range). --------------
[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_rtai(uint gi : SV_GroupIndex, out vertices VOut verts[3], out indices uint3 tris[4],
             out primitives PLayer prims[4]) {
  SetMeshOutputCounts(3, 4);
  if (gi < 3) {
    verts[gi].pos = float4(gi == 1 ? 3 : -1, gi == 2 ? -3 : 1, 0, 1);
    verts[gi].color = float4(0, 0, 0, 1);
  }
  tris[gi] = uint3(0, 1, 2);
  prims[gi].layer = k.x + gi;
  prims[gi].viewport = 0;
  prims[gi].tag = gi;
}

// ---- ms_rootk: root constant k.x selects the tile color and is added to u0 (argument-changing signatures). --
[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_rootk(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID, out vertices VOut verts[4],
              out indices uint3 tris[2]) {
  SetMeshOutputCounts(4, 2);
  const float2 corner = float2(gi & 1, gi >> 1);
  verts[gi].pos = pixelToNdc((float2(k.x, gid.x) + corner) * 8, float2(64, 64));
  verts[gi].color = float4(k.x / 255.0, k.y / 255.0, gid.x / 255.0, 1);
  if (gi < 2)
    tris[gi] = gi == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
  if (gi == 0)
    counters.InterlockedAdd(0, k.y);
}

// ---- ms_early + ps_early: group g draws triangles 2g and 2g+1 of t0 (per triangle 3 x (pixel x, pixel y, z) as
// floats, 64x64 target) for the forced-early-depth MSAA statistics case.
[outputtopology("triangle")]
[numthreads(6, 1, 1)]
void ms_early(uint gi : SV_GroupIndex, uint3 gid : SV_GroupID, out vertices VOut verts[6],
              out indices uint3 tris[2]) {
  SetMeshOutputCounts(6, 2);
  const float3 v = asfloat(input.Load3(((gid.x * 6 + gi) * 3) * 4));
  verts[gi].pos = float4(v.x / 32 - 1, 1 - v.y / 32, v.z, 1);
  verts[gi].color = float4(0, 1, 0, 1);
  if (gi < 2)
    tris[gi] = uint3(gi * 3, gi * 3 + 1, gi * 3 + 2);
}

[earlydepthstencil]
float4 ps_early(VOut v) : SV_Target {
  return v.color;
}

#include "mesh-shader-contract.hlsl"

// SM6.6 optional MS/AS derivatives. Every invocation writes its identities and numerical
// results, including a nonlinear field whose fine derivatives differ from the coarse ones.
Texture2D<float> derivative_texture : register(t1);
SamplerState derivative_sampler : register(s0);
SamplerComparisonState derivative_compare : register(s1);

void derivative_results(uint3 tid, uint3 dtid, uint3 gid, uint gi, uint threads, bool linear_quad) {
  uint2 q = linear_quad ? uint2(gi & 1, (gi >> 1) & 1) : (tid.xy & 1);
  float value = 100.0 * tid.z + 10.0 * tid.x + tid.y + tid.x * tid.y;
  float2 uv = 0.25 + float2(q) * 0.125;
  uint offset = (4 + (gid.x * threads + gi) * 24) * 4;
  counters.Store4(offset, uint4(tid, gi));
  counters.Store4(offset + 16, uint4(dtid, WaveGetLaneIndex() & 3));
  counters.Store4(offset + 32, asuint(float4(value, ddx_fine(value), ddy_fine(value), ddx_coarse(value))));
  counters.Store4(offset + 48, asuint(float4(ddy_coarse(value), QuadReadAcrossX(value),
      QuadReadAcrossY(value), QuadReadAcrossDiagonal(value))));
  counters.Store4(offset + 64, asuint(float4(QuadReadLaneAt(value, 3),
      derivative_texture.CalculateLevelOfDetail(derivative_sampler, uv),
      derivative_texture.CalculateLevelOfDetailUnclamped(derivative_sampler, uv),
      derivative_texture.Sample(derivative_sampler, uv))));
  half small = half(value);
  counters.Store4(offset + 80, asuint(float4(derivative_texture.SampleBias(derivative_sampler, uv, 1.0),
      derivative_texture.SampleCmp(derivative_compare, uv, q.x ? 0.3 : 0.2),
      float(ddx_fine(small)), float(ddy_fine(small)))));
}

struct DerivativePayload { uint tag; };
groupshared DerivativePayload derivative_payload;

#define MS_DERIVATIVE_ENTRY(NAME, X, Y, Z, LINEAR) \
[outputtopology("triangle")] [numthreads(X, Y, Z)] \
void NAME(uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID, \
          uint3 gid : SV_GroupID, uint gi : SV_GroupIndex, \
          out vertices VOut verts[1], out indices uint3 tris[1]) { \
  SetMeshOutputCounts(1, 0); \
  if (gi == 0) { verts[0].pos = float4(0, 0, 0, 1); verts[0].color = 0; } \
  derivative_results(tid, dtid, gid, gi, X * Y * Z, LINEAR); \
}

#define AS_DERIVATIVE_ENTRY(NAME, X, Y, Z, LINEAR) \
[numthreads(X, Y, Z)] \
void NAME(uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID, \
          uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) { \
  derivative_results(tid, dtid, gid, gi, X * Y * Z, LINEAR); \
  if (gi == 0) derivative_payload.tag = 0x12345678; \
  DispatchMesh(1, 1, 1, derivative_payload); \
}

MS_DERIVATIVE_ENTRY(ms_derivatives_1d, 32, 1, 1, true)
MS_DERIVATIVE_ENTRY(ms_derivatives_2d, 8, 4, 1, false)
MS_DERIVATIVE_ENTRY(ms_derivatives_3d, 4, 2, 2, false)
AS_DERIVATIVE_ENTRY(as_derivatives_1d, 32, 1, 1, true)
AS_DERIVATIVE_ENTRY(as_derivatives_2d, 8, 4, 1, false)
AS_DERIVATIVE_ENTRY(as_derivatives_3d, 4, 2, 2, false)
#ifdef DERIVATIVE_INVALID_SHAPE
// Deliberately validator-disabled negative inputs; never counted as genuine valid shaders.
MS_DERIVATIVE_ENTRY(ms_derivatives_invalid, 3, 1, 1, true)
AS_DERIVATIVE_ENTRY(as_derivatives_invalid, 3, 1, 1, true)
#endif

[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_derivative_payload(uint gi : SV_GroupIndex, in payload DerivativePayload p,
                           out vertices VOut verts[4], out indices uint3 tris[2]) {
  SetMeshOutputCounts(4, 2);
  verts[gi].pos = float4(float(gi & 1) * 2 - 1, 1 - float(gi >> 1) * 2, 0, 1);
  verts[gi].color = p.tag == 0x12345678 ? float4(0, 1, 0, 1) : float4(1, 0, 1, 1);
  if (gi < 2) tris[gi] = gi == 0 ? uint3(0, 1, 2) : uint3(1, 3, 2);
}

// Returning groupshared64 atomics across four physical SIMD groups, in both TG stages.
groupshared uint64_t atomic64_shared;
groupshared int64_t atomic64_signed;
void group_atomic64(uint gi, uint group) {
  if (gi == 0) {
    for (uint op = 0; op < 9; ++op) {
      const uint64_t initial = op == 4 || op == 5 ? 0xfffffff000000002ull : 0x200000002ull;
      const uint64_t value = op == 4 || op == 5 ? 0xfffffff100000003ull : 0x100000001ull;
      atomic64_shared = initial;
      atomic64_signed = int64_t(initial);
      uint64_t old;
      int64_t signed_old;
      switch (op) {
      case 0: InterlockedAdd(atomic64_shared, value, old); break;
      case 1: InterlockedAnd(atomic64_shared, value, old); break;
      case 2: InterlockedOr(atomic64_shared, value, old); break;
      case 3: InterlockedXor(atomic64_shared, value, old); break;
      case 4: InterlockedMin(atomic64_signed, int64_t(value), signed_old); old = uint64_t(signed_old); break;
      case 5: InterlockedMax(atomic64_signed, int64_t(value), signed_old); old = uint64_t(signed_old); break;
      case 6: InterlockedMin(atomic64_shared, value, old); break;
      case 7: InterlockedMax(atomic64_shared, value, old); break;
      default: InterlockedExchange(atomic64_shared, value, old); break;
      }
      counters.Store<uint64_t>(16 + (group * 153 + op * 2) * 8, old);
      counters.Store<uint64_t>(16 + (group * 153 + op * 2 + 1) * 8,
          op == 4 || op == 5 ? uint64_t(atomic64_signed) : atomic64_shared);
    }
    uint64_t old;
    atomic64_shared = 0x200000002ull;
    InterlockedCompareExchange(atomic64_shared, 0x200000002ull, 0x300000003ull, old);
    counters.Store<uint64_t>(16 + (group * 153 + 18) * 8, old);
    counters.Store<uint64_t>(16 + (group * 153 + 19) * 8, atomic64_shared);
    InterlockedCompareExchange(atomic64_shared, 0x200000002ull, 0, old);
    counters.Store<uint64_t>(16 + (group * 153 + 20) * 8, old);
    atomic64_shared = 0xffffffffffffffffull;
    InterlockedAdd(atomic64_shared, 1, old);
    counters.Store<uint64_t>(16 + (group * 153 + 21) * 8, old);
    counters.Store<uint64_t>(16 + (group * 153 + 22) * 8, atomic64_shared);
    InterlockedCompareStore(atomic64_shared, 0, 0x8000000000000000ull);
    counters.Store<uint64_t>(16 + (group * 153 + 23) * 8, atomic64_shared);
  }
  if (gi == 0) atomic64_shared = 0;
  GroupMemoryBarrierWithGroupSync();
  uint64_t old;
  InterlockedAdd(atomic64_shared, 0x100000001ull, old);
  counters.Store<uint64_t>(16 + (group * 153 + 24 + gi) * 8, old);
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0) counters.Store<uint64_t>(16 + (group * 153 + 152) * 8, atomic64_shared);
}
[outputtopology("triangle")] [numthreads(128, 1, 1)]
void ms_groupshared64(uint gi : SV_GroupIndex, uint3 group : SV_GroupID,
                      out vertices VOut verts[1], out indices uint3 tris[1]) {
  SetMeshOutputCounts(1, 0);
  if (gi == 0) { verts[0].pos = float4(0, 0, 0, 1); verts[0].color = 0; }
  group_atomic64(gi, group.x);
}
[numthreads(128, 1, 1)]
void as_groupshared64(uint gi : SV_GroupIndex, uint3 group : SV_GroupID) {
  group_atomic64(gi, group.x);
  if (gi == 0) derivative_payload.tag = 0x12345678;
  DispatchMesh(1, 1, 1, derivative_payload);
}
