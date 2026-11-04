// Shared by owned-d3d12-tessellation.cpp: DXBC through D3DCompile (vs/hs/ds/ps_5_0), DXIL frozen as
// tessellation-<entry>.dxil (vs/hs/ds/ps_6_0, dxc -HV 2021).

cbuffer Factors : register(b0) {
  float4 edges;   // tri: edges.xyz; quad: edges.xyzw
  float4 inside;  // tri: inside.x; quad: inside.xy
};

// DS log: word 0 = invocation count, word 1 unused, then (u, v) float pairs.
RWStructuredBuffer<uint> domain_log : register(u0);

struct VSOut {
  float4 position : POSITION;
};

VSOut vs_pass(float2 position : POSITION) {
  VSOut o;
  o.position = float4(position, 0, 1);
  return o;
}

// Distinguish scheduler instance cursors without changing tessellation factors or domain points.
// Two commands also fetch different VB positions in the focused frontier fixture.
VSOut vs_frontier(float2 position : POSITION, uint instance : SV_InstanceID) {
  VSOut o;
  o.position = float4(position, float(instance) / 64.0, 1);
  return o;
}

struct TriConstants {
  float edge[3] : SV_TessFactor;
  float inside : SV_InsideTessFactor;
};

struct QuadConstants {
  float edge[4] : SV_TessFactor;
  float inside[2] : SV_InsideTessFactor;
};

TriConstants hs_tri_constants(InputPatch<VSOut, 3> patch) {
  TriConstants c;
  c.edge[0] = edges.x;
  c.edge[1] = edges.y;
  c.edge[2] = edges.z;
  c.inside = inside.x;
  return c;
}

QuadConstants hs_quad_constants(InputPatch<VSOut, 4> patch) {
  QuadConstants c;
  c.edge[0] = edges.x;
  c.edge[1] = edges.y;
  c.edge[2] = edges.z;
  c.edge[3] = edges.w;
  c.inside[0] = inside.x;
  c.inside[1] = inside.y;
  return c;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_integer(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("tri")]
[partitioning("pow2")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_pow2(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_fractional_odd(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_fractional_even(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_integer(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("pow2")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_pow2(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_fractional_odd(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_fractional_even(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}


struct DSOut {
  float4 position : SV_Position;
};

void log_point(float2 uv) {
  uint slot;
  InterlockedAdd(domain_log[0], 1, slot);
  if (slot < 16383) {
    domain_log[2 + slot * 2] = asuint(uv.x);
    domain_log[3 + slot * 2] = asuint(uv.y);
  }
}

[domain("tri")]
DSOut ds_tri(TriConstants c, float3 bary : SV_DomainLocation, const OutputPatch<VSOut, 3> patch) {
  log_point(bary.xy);
  DSOut o;
  o.position = bary.x * patch[0].position + bary.y * patch[1].position + bary.z * patch[2].position;
  return o;
}

[domain("quad")]
DSOut ds_quad(QuadConstants c, float2 uv : SV_DomainLocation, const OutputPatch<VSOut, 4> patch) {
  log_point(uv);
  DSOut o;
  float4 top = lerp(patch[0].position, patch[1].position, uv.x);
  float4 bottom = lerp(patch[3].position, patch[2].position, uv.x);
  o.position = lerp(top, bottom, uv.y);
  return o;
}

float4 ps_color(DSOut input) : SV_Target {
  return float4(0, 1, 0, 1);
}

// Forced early depth/stencil (PSInvocations under MSAA count once per (pixel, primitive) with a surviving sample).
[earlydepthstencil]
float4 ps_early(DSOut input) : SV_Target {
  return float4(0, 1, 0, 1);
}

// N-control-point patch lists: the HS reads every input control point (N in {1, 2, 5, 8, 11, 16, 17, 32}) and outputs one control point at
// the patch's mean position; factors are 1, so every patch is one quad of 4 domain points. ds_patch logs
// (SV_PrimitiveID, control-point count) for every domain point, so the fixture checks patch assembly and count.
struct PatchConstants {
  float edge[4] : SV_TessFactor;
  float inside[2] : SV_InsideTessFactor;
  uint count : COUNT;
};

PatchConstants hs_patch1_constants(InputPatch<VSOut, 1> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 1;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch1_constants")]
VSOut hs_patch1(InputPatch<VSOut, 1> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 1; ++k)
    sum += patch[k].position;
  o.position = sum / 1;
  return o;
}

PatchConstants hs_patch2_constants(InputPatch<VSOut, 2> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 2;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch2_constants")]
VSOut hs_patch2(InputPatch<VSOut, 2> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 2; ++k)
    sum += patch[k].position;
  o.position = sum / 2;
  return o;
}

PatchConstants hs_patch5_constants(InputPatch<VSOut, 5> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 5;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch5_constants")]
VSOut hs_patch5(InputPatch<VSOut, 5> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 5; ++k)
    sum += patch[k].position;
  o.position = sum / 5;
  return o;
}

PatchConstants hs_patch8_constants(InputPatch<VSOut, 8> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 8;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch8_constants")]
VSOut hs_patch8(InputPatch<VSOut, 8> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 8; ++k)
    sum += patch[k].position;
  o.position = sum / 8;
  return o;
}

PatchConstants hs_patch11_constants(InputPatch<VSOut, 11> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 11;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch11_constants")]
VSOut hs_patch11(InputPatch<VSOut, 11> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 11; ++k)
    sum += patch[k].position;
  o.position = sum / 11;
  return o;
}

PatchConstants hs_patch16_constants(InputPatch<VSOut, 16> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 16;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch16_constants")]
VSOut hs_patch16(InputPatch<VSOut, 16> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 16; ++k)
    sum += patch[k].position;
  o.position = sum / 16;
  return o;
}

PatchConstants hs_patch17_constants(InputPatch<VSOut, 17> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 17;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch17_constants")]
VSOut hs_patch17(InputPatch<VSOut, 17> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 17; ++k)
    sum += patch[k].position;
  o.position = sum / 17;
  return o;
}

PatchConstants hs_patch32_constants(InputPatch<VSOut, 32> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = c.edge[3] = 1;
  c.inside[0] = c.inside[1] = 1;
  c.count = 32;
  return c;
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(1)]
[patchconstantfunc("hs_patch32_constants")]
VSOut hs_patch32(InputPatch<VSOut, 32> patch, uint i : SV_OutputControlPointID) {
  VSOut o;
  float4 sum = 0;
  [unroll] for (uint k = 0; k < 32; ++k)
    sum += patch[k].position;
  o.position = sum / 32;
  return o;
}

[domain("quad")]
DSOut ds_patch(PatchConstants c, float2 uv : SV_DomainLocation, const OutputPatch<VSOut, 1> patch,
               uint primitive : SV_PrimitiveID) {
  uint slot;
  InterlockedAdd(domain_log[0], 1, slot);
  if (slot < 16383) {
    domain_log[2 + slot * 2] = primitive;
    domain_log[3 + slot * 2] = c.count;
  }
  DSOut o;
  // A 2x2-pixel quad around the mean position (32x32 target: one pixel is 2/32 in clip space).
  o.position = patch[0].position + float4((uv * 2 - 1) * (2.0 / 32.0), 0, 0);
  return o;
}

// Isoline domain and point output. Isoline factors: SV_TessFactor[0] = line density, [1] = line detail.
struct IsolineConstants {
  float factors[2] : SV_TessFactor;
};

IsolineConstants hs_isoline_constants(InputPatch<VSOut, 4> patch) {
  IsolineConstants c;
  c.factors[0] = edges.x;
  c.factors[1] = edges.y;
  return c;
}

[domain("isoline")]
[partitioning("integer")]
[outputtopology("line")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_integer_line(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("isoline")]
[partitioning("integer")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_integer_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("isoline")]
[partitioning("pow2")]
[outputtopology("line")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_pow2_line(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("isoline")]
[partitioning("pow2")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_pow2_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("isoline")]
[partitioning("fractional_odd")]
[outputtopology("line")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_fractional_odd_line(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("isoline")]
[partitioning("fractional_odd")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_fractional_odd_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("isoline")]
[partitioning("fractional_even")]
[outputtopology("line")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_fractional_even_line(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("isoline")]
[partitioning("fractional_even")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_isoline_constants")]
[maxtessfactor(64.0)]
VSOut hs_isoline_fractional_even_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("point")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_integer_point(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("tri")]
[partitioning("pow2")]
[outputtopology("point")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_pow2_point(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("point")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_fractional_odd_point(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("point")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_tri_constants")]
[maxtessfactor(64.0)]
VSOut hs_tri_fractional_even_point(InputPatch<VSOut, 3> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("integer")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_integer_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("pow2")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_pow2_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("fractional_odd")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_fractional_odd_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

[domain("quad")]
[partitioning("fractional_even")]
[outputtopology("point")]
[outputcontrolpoints(4)]
[patchconstantfunc("hs_quad_constants")]
[maxtessfactor(64.0)]
VSOut hs_quad_fractional_even_point(InputPatch<VSOut, 4> patch, uint i : SV_OutputControlPointID) {
  return patch[i];
}

// GS after tessellation (VS+HS+DS+GS): every tessellated triangle is emitted twice, as two 3-vertex strips separated
// by a cut (amplification + strip cut). TAG = (SV_PrimitiveID, copy) for the stream-output oracle; the GEOMETRY-only
// u1 log records one SV_PrimitiveID per GS invocation (word 0 = invocation count).
RWStructuredBuffer<uint> geometry_log : register(u1);

struct GSOut {
  float4 position : SV_Position;
  float2 tag : TAG;
};

[maxvertexcount(6)]
void gs_tsgs(triangle DSOut v[3], uint id : SV_PrimitiveID, inout TriangleStream<GSOut> stream) {
  uint slot;
  InterlockedAdd(geometry_log[0], 1, slot);
  if (slot < 16383)
    geometry_log[1 + slot] = id;
  for (uint copy = 0; copy < 2; ++copy) {
    for (uint k = 0; k < 3; ++k) {
      GSOut o;
      o.position = v[k].position;
      o.tag = float2(id, copy);
      stream.Append(o);
    }
    stream.RestartStrip();
  }
}

// Forced early depth after the GS (MSAA PSInvocations by the claim buffer).
[earlydepthstencil]
float4 ps_tsgs_early(GSOut input) : SV_Target {
  return float4(0, 1, 0, 1);
}

[domain("isoline")]
DSOut ds_isoline(IsolineConstants c, float2 uv : SV_DomainLocation, const OutputPatch<VSOut, 4> patch) {
  log_point(uv);
  DSOut o;
  float4 top = lerp(patch[0].position, patch[1].position, uv.x);
  float4 bottom = lerp(patch[3].position, patch[2].position, uv.x);
  o.position = lerp(top, bottom, uv.y);
  return o;
}
