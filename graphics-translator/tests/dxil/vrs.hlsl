// Variable-rate shading fixture shaders (owned-d3d12-vrs.cpp). Frozen by build-vrs.sh with DXC 1.9 (6.1 profiles).
// One triangle covers the whole W x H target: ndc (-1, 1), (3, 1), (-1, -3) with clip w 1, 2, 1, so 1 / w is
// 1 - s.x / (4 W) at screen point s and the perspective-correct tex = 0.25 * clip.xy is 0.25 * ndc(s) * w(s). Window
// depth is depth_left + (depth_right - depth_left) * s.x / W. The provoking vertex's flat id is 0x51.
cbuffer Params : register(b0) {
  float depth_left;
  float depth_right;
  float color;
  uint unused;
};
RWByteAddressBuffer counter : register(u0);

struct VSOut {
  float4 pos : SV_Position;
  float2 tex : TEXCOORD0;
  nointerpolation uint flat_id : FLATID;
};

VSOut vs_main(uint id : SV_VertexID) {
  const float2 ndc = id == 1 ? float2(3, 1) : id == 2 ? float2(-1, -3) : float2(-1, 1);
  const float w = id == 1 ? 2.0 : 1.0;
  const float z = id == 1 ? 2.0 * depth_right - depth_left : depth_left;
  VSOut o;
  o.pos = float4(ndc * w, z * w, w);
  o.tex = ndc * w * 0.25;
  o.flat_id = 0x51 + id;
  return o;
}

struct PSOut {
  float4 c0 : SV_Target0;
  float4 c1 : SV_Target1;
};

// The coarse center position (SV_Position.xyz), center-interpolated tex, coarse screen derivatives and the flat
// provoking-vertex input.
PSOut ps_main(VSOut i) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(i.pos.xy, i.tex);
  o.c1 = float4(ddx(i.pos.x), ddy(i.pos.y), i.pos.z, float(i.flat_id));
  return o;
}

// Pull-model evaluation: VariableShadingRateTier 1 shades at fine rate.
PSOut ps_snapped(VSOut i) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(i.pos.xy, EvaluateAttributeSnapped(i.tex, int2(-8, -8)));
  o.c1 = float4(0, 0, 0, 1);
  return o;
}

// Counts every invocation, then discards coarse pixels whose center x mod 8 is below 4 (every fine pixel of a discarded coarse pixel
// keeps the clear color).
PSOut ps_discard(VSOut i) {
  counter.InterlockedAdd(0, 1);
  if (fmod(i.pos.x, 8.0) < 4.0) discard;
  PSOut o;
  o.c0 = float4(i.pos.xy, 1, 1);
  o.c1 = float4(0, 0, 0, 1);
  return o;
}

// SV_Coverage input: VariableShadingRateTier 1 shades at fine rate.
PSOut ps_coverage(VSOut i, uint mask : SV_Coverage) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(countbits(mask), mask, i.pos.xy);
  o.c1 = float4(0, 0, 0, 1);
  return o;
}

// Exports depth: D3D12 shades at fine rate.
PSOut ps_depth(VSOut i, out float depth : SV_Depth) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(i.pos.xy, 0, 1);
  o.c1 = float4(0, 0, 0, 1);
  depth = i.pos.z;
  return o;
}

// A flat color per draw for the per-sample depth test.
PSOut ps_flat(VSOut i) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(color, i.pos.xy, 1);
  o.c1 = float4(0, 0, 0, 1);
  return o;
}

struct VSRateOut {
  float4 pos : SV_Position;
  float2 tex : TEXCOORD0;
  nointerpolation uint flat_id : FLATID;
  nointerpolation uint rate : SV_ShadingRate;
};
VSRateOut vs_rate(uint id : SV_VertexID) {
  VSOut v = vs_main(id);
  VSRateOut o;
  o.pos = v.pos; o.tex = v.tex; o.flat_id = v.flat_id;
  o.rate = id == 0 ? unused : 0; // Prove that the leading vertex, not the last vertex, supplies the rate.
  return o;
}
PSOut ps_rate(VSOut i, uint rate : SV_ShadingRate) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(i.pos.xy, rate, i.flat_id);
  o.c1 = float4(ddx(i.pos.x), ddy(i.pos.y), i.tex);
  return o;
}
PSOut ps_centroid(VSOut i) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(i.pos.xy, EvaluateAttributeCentroid(i.tex));
  o.c1 = float4(ddx(i.pos.x), ddy(i.pos.y), 0, 1);
  return o;
}
PSOut ps_sample(VSOut i) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(i.pos.xy, EvaluateAttributeAtSample(i.tex, unused));
  o.c1 = float4(ddx(i.pos.x), ddy(i.pos.y), 0, 1);
  return o;
}
PSOut ps_wide_snapped(VSOut i) {
  counter.InterlockedAdd(0, 1);
  PSOut o;
  o.c0 = float4(i.pos.xy, EvaluateAttributeSnapped(i.tex, int2(23, -25)));
  o.c1 = float4(ddx(i.pos.x), ddy(i.pos.y), 0, 1);
  return o;
}

// All pre-raster stages emit the same perspective triangle used by the VS reference.
[maxvertexcount(3)]
void gs_main(triangle VSOut input[3], inout TriangleStream<VSOut> output) {
  for (uint v = 0; v < 3; ++v) output.Append(input[v]);
}
struct HullFactors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
HullFactors hs_factors(InputPatch<VSOut, 3> patch) {
  HullFactors o; o.edge[0] = o.edge[1] = o.edge[2] = o.inside = 1; return o;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")]
[outputcontrolpoints(3)] [patchconstantfunc("hs_factors")]
VSOut hs_main(InputPatch<VSOut, 3> patch, uint id : SV_OutputControlPointID) { return patch[id]; }
[domain("tri")]
VSOut ds_main(HullFactors factors, const OutputPatch<VSOut, 3> patch, float3 bary : SV_DomainLocation) {
  VSOut o;
  o.pos = patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;
  o.tex = patch[0].tex * bary.x + patch[1].tex * bary.y + patch[2].tex * bary.z;
  o.flat_id = patch[0].flat_id;
  return o;
}
[numthreads(32, 1, 1)] [outputtopology("triangle")]
void ms_main(uint id : SV_GroupIndex, out vertices VSOut vertices[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3, 1);
  if (id < 3) vertices[id] = vs_main(id);
  if (id == 0) triangles[0] = uint3(0, 1, 2);
}
struct AmplificationPayload { uint parent; };
groupshared AmplificationPayload amplification_payload;
[numthreads(32, 1, 1)]
void as_main(uint id : SV_GroupIndex, uint3 group : SV_GroupID) {
  if (id == 0) amplification_payload.parent = group.x;
  DispatchMesh(2, 1, 1, amplification_payload);
}
[numthreads(32, 1, 1)] [outputtopology("triangle")]
void ms_as(uint id : SV_GroupIndex, in payload AmplificationPayload payload,
           out vertices VSOut vertices[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3, 1);
  if (id < 3) {
    vertices[id] = vs_main(id);
    vertices[id].flat_id = 0x51 + payload.parent;
  }
  if (id == 0) triangles[0] = uint3(0, 1, 2);
}

[maxvertexcount(3)]
void gs_rate(triangle VSOut input[3], inout TriangleStream<VSRateOut> output) {
  for (uint v = 0; v < 3; ++v) {
    VSRateOut o; o.pos = input[v].pos; o.tex = input[v].tex; o.flat_id = input[v].flat_id;
    o.rate = v == 0 ? unused : 0; output.Append(o);
  }
}
struct RatePrimitive { uint rate : SV_ShadingRate; };
[numthreads(32, 1, 1)] [outputtopology("triangle")]
void ms_rate(uint id : SV_GroupIndex, out vertices VSOut vertices[3], out indices uint3 triangles[1],
             out primitives RatePrimitive primitives[1]) {
  SetMeshOutputCounts(3, 1);
  if (id < 3) vertices[id] = vs_main(id);
  if (id == 0) { triangles[0] = uint3(0, 1, 2); primitives[0].rate = unused; }
}
[numthreads(32, 1, 1)] [outputtopology("triangle")]
void ms_as_rate(uint id : SV_GroupIndex, in payload AmplificationPayload payload,
                out vertices VSOut vertices[3], out indices uint3 triangles[1],
                out primitives RatePrimitive primitives[1]) {
  SetMeshOutputCounts(3, 1);
  if (id < 3) { vertices[id] = vs_main(id); vertices[id].flat_id = 0x51 + payload.parent; }
  if (id == 0) { triangles[0] = uint3(0, 1, 2); primitives[0].rate = unused; }
}

struct VSRateViewOut {
  float4 pos : SV_Position;
  float2 tex : TEXCOORD0;
  nointerpolation uint flat_id : FLATID;
  nointerpolation uint rate : SV_ShadingRate;
  uint viewport : SV_ViewportArrayIndex;
};
VSRateViewOut vs_rate_view(uint id : SV_VertexID) {
  VSRateOut v = vs_rate(id);
  VSRateViewOut o; o.pos = v.pos; o.tex = v.tex; o.flat_id = v.flat_id; o.rate = v.rate; o.viewport = 1;
  return o;
}
[maxvertexcount(3)]
void gs_rate_view(triangle VSOut input[3], inout TriangleStream<VSRateViewOut> output) {
  for (uint v = 0; v < 3; ++v) {
    VSRateViewOut o; o.pos = input[v].pos; o.tex = input[v].tex; o.flat_id = input[v].flat_id;
    o.rate = v == 0 ? unused : 0; o.viewport = 1; output.Append(o);
  }
}
struct RateViewPrimitive { uint rate : SV_ShadingRate; uint viewport : SV_ViewportArrayIndex; };
[numthreads(32, 1, 1)] [outputtopology("triangle")]
void ms_rate_view(uint id : SV_GroupIndex, out vertices VSOut vertices[3], out indices uint3 triangles[1],
                  out primitives RateViewPrimitive primitives[1]) {
  SetMeshOutputCounts(3, 1);
  if (id < 3) vertices[id] = vs_main(id);
  if (id == 0) { triangles[0] = uint3(0, 1, 2); primitives[0].rate = unused; primitives[0].viewport = 1; }
}
[numthreads(32, 1, 1)] [outputtopology("triangle")]
void ms_as_rate_view(uint id : SV_GroupIndex, in payload AmplificationPayload payload,
                     out vertices VSOut vertices[3], out indices uint3 triangles[1],
                     out primitives RateViewPrimitive primitives[1]) {
  SetMeshOutputCounts(3, 1);
  if (id < 3) { vertices[id] = vs_main(id); vertices[id].flat_id = 0x51 + payload.parent; }
  if (id == 0) { triangles[0] = uint3(0, 1, 2); primitives[0].rate = unused; primitives[0].viewport = 1; }
}
