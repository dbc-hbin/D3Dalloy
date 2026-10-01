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
