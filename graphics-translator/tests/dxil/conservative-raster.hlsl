// Public Tier3 proof: DXC 1.9.2609.5 ps_6_0 and native Microsoft FXC ps_5_1,
// frozen by build-conservative-raster.sh. Exact producer commands/hashes and retained
// shader provenance: conservative-raster-provenance.json. The legacy DXBC geometry
// is compiled by D3DCompile with CONSERVATIVE_LEGACY; Tier3 PS DXBC is always genuine FXC.
StructuredBuffer<float4> vertices : register(t0);
// Four dwords: invocations, coverage (or interpolated attribute), depth, InnerCoverage.
RWStructuredBuffer<uint4> pixels : register(u0);

struct V {
  float4 position : SV_Position;
  float value : TEXCOORD0;
};

V vs_main(uint id : SV_VertexID) {
  V v;
  v.position = vertices[id];
  v.value = id;
  return v;
}

float4 ps_main(V v, uint coverage : SV_Coverage) : SV_Target {
  uint2 p = uint2(v.position.xy);
  uint index = p.y * 32 + p.x;
  InterlockedAdd(pixels[index].x, 1);
  InterlockedOr(pixels[index].y, coverage);
  pixels[index].z = asuint(v.position.z);
  return float4(1, v.value, 0, 1);
}

// Separate entry points: SV_Coverage and SV_InnerCoverage are mutually exclusive.
#ifndef CONSERVATIVE_LEGACY
float4 ps_inner(V v, uint inner : SV_InnerCoverage, bool front : SV_IsFrontFace) : SV_Target {
  uint index = uint(v.position.y) * 32 + uint(v.position.x);
  InterlockedAdd(pixels[index].x, 1);
  pixels[index].y = asuint(v.value + (front ? 16 : 0));
  pixels[index].z = asuint(v.position.z);
  pixels[index].w = inner;
  return float4(1, v.value, 0, 1);
}

[earlydepthstencil]
float4 ps_inner_early(V v, uint inner : SV_InnerCoverage, bool front : SV_IsFrontFace) : SV_Target {
  return ps_inner(v, inner, front);
}

float4 ps_inner_late(V v, uint inner : SV_InnerCoverage, bool front : SV_IsFrontFace, out float depth_out : SV_Depth) : SV_Target {
  depth_out = 0.25; // No depth extrapolation ambiguity; depth tests must run after UAV writes.
  return ps_inner(v, inner, front);
}

float4 ps_helper(V v, uint inner : SV_InnerCoverage) : SV_Target {
  uint index = uint(v.position.y) * 32 + uint(v.position.x);
  float covered = float(inner & 1);
  InterlockedAdd(pixels[index].x, 1);
  pixels[index].z = asuint(v.position.z);
  pixels[index].w = asuint(abs(ddx_fine(covered)) + abs(ddy_fine(covered)));
  return 1;
}

#ifdef CONSERVATIVE_INVALID_INPUT
float4 ps_invalid(V v, uint coverage : SV_Coverage, uint inner : SV_InnerCoverage) : SV_Target {
  return float4(coverage, inner, v.value, 1);
}
#endif
#endif

[maxvertexcount(3)]
void gs_main(triangle V input[3], inout TriangleStream<V> output) {
  for (uint i = 0; i < 3; i++) output.Append(input[i]);
}

struct Patch {
  float edges[3] : SV_TessFactor;
  float inside : SV_InsideTessFactor;
};

Patch hs_patch(InputPatch<V, 3> input) {
  Patch p;
  p.edges[0] = p.edges[1] = p.edges[2] = 1;
  p.inside = 1;
  return p;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_patch")]
V hs_main(InputPatch<V, 3> input, uint id : SV_OutputControlPointID) {
  return input[id];
}

[domain("tri")]
V ds_main(Patch patch, float3 uvw : SV_DomainLocation, const OutputPatch<V, 3> input) {
  V v;
  v.position = input[0].position * uvw.x + input[1].position * uvw.y + input[2].position * uvw.z;
  v.value = input[0].value * uvw.x + input[1].value * uvw.y + input[2].value * uvw.z;
  return v;
}

#ifdef CONSERVATIVE_MESH
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint3 group : SV_GroupID, out vertices V output_vertices[3], out indices uint3 output_indices[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint i = 0; i < 3; i++) {
    output_vertices[i].position = vertices[group.x * 3 + i];
    output_vertices[i].value = group.x * 3 + i;
  }
  output_indices[0] = uint3(0, 1, 2);
}
struct MeshPayload { uint primitive; };
groupshared MeshPayload mesh_payload;
[numthreads(1, 1, 1)]
void as_main(uint3 group : SV_GroupID) {
  mesh_payload.primitive = group.x;
  DispatchMesh(1, 1, 1, mesh_payload);
}
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void ms_as(in payload MeshPayload payload_data, out vertices V output_vertices[3], out indices uint3 output_indices[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint i = 0; i < 3; i++) {
    output_vertices[i].position = vertices[payload_data.primitive * 3 + i];
    output_vertices[i].value = payload_data.primitive * 3 + i;
  }
  output_indices[0] = uint3(0, 1, 2);
}
#endif

// Reads every sample of the 4x depth target into depths[(y * 32 + x) * 4 + sample].
RWStructuredBuffer<float> depths : register(u1);
#ifdef CONSERVATIVE_STENCIL_READER
Texture2DMS<uint2> stencil : register(t1);
[numthreads(8, 8, 1)]
void cs_stencil(uint3 id : SV_DispatchThreadID) {
  for (uint s = 0; s < 4; s++)
    depths[(id.y * 32 + id.x) * 4 + s] = stencil.Load(id.xy, s).y;
}
#else
Texture2DMS<float> depth : register(t1);
[numthreads(8, 8, 1)]
void cs_depth(uint3 id : SV_DispatchThreadID) {
  for (uint s = 0; s < 4; s++)
    depths[(id.y * 32 + id.x) * 4 + s] = depth.Load(id.xy, s);
}
#endif
