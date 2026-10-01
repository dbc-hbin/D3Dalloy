// Conservative rasterization suite (owned-d3d12-conservative-raster.cpp). The fixture compiles this file to DXBC
// with D3DCompile (every stage but ps_main: vkd3d has no SV_Coverage input, so the DXBC paths use the DXIL PS) and
// loads the DXC 1.9 DXIL frozen by build-conservative-raster.sh as
// conservative-raster-<entry>.dxil.
StructuredBuffer<float4> vertices : register(t0);
// One record per pixel of the 32x32 target: invocations, SV_Coverage (OR), SV_Position.z, 0.
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
Texture2DMS<float> depth : register(t1);
RWStructuredBuffer<float> depths : register(u1);
[numthreads(8, 8, 1)]
void cs_depth(uint3 id : SV_DispatchThreadID) {
  for (uint s = 0; s < 4; s++)
    depths[(id.y * 32 + id.x) * 4 + s] = depth.Load(id.xy, s);
}
