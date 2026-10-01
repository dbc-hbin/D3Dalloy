StructuredBuffer<float4> vertices : register(t0);
RWStructuredBuffer<uint4> pixels : register(u0);
struct V {
  float4 position : SV_Position;
  float value : TEXCOORD0;
  float2 clip : SV_ClipDistance;
  float2 cull : SV_CullDistance;
};
V vertex(uint id) {
  V v;
  v.position = vertices[id];
  v.value = id;
  v.clip = float2(v.position.x + 0.1 * v.position.w, v.position.w);
  v.cull = float2(v.position.y + 0.8 * v.position.w, v.position.w);
  return v;
}
V vs_main(uint id : SV_VertexID) { return vertex(id); }
float4 ps_main(V v, uint coverage : SV_Coverage) : SV_Target {
  uint index = uint(v.position.y) * 32 + uint(v.position.x);
  InterlockedAdd(pixels[index].x, 1);
  InterlockedOr(pixels[index].y, coverage);
  pixels[index].z = asuint(v.position.z);
  pixels[index].w = asuint(v.clip.x);
  return float4(v.cull, v.value, 1);
}
[maxvertexcount(3)]
void gs_main(triangle V input[3], inout TriangleStream<V> output) {
  for (uint i = 0; i < 3; ++i) output.Append(input[i]);
}
struct Patch { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Patch hs_patch(InputPatch<V, 3> input) {
  Patch p; p.edges[0] = p.edges[1] = p.edges[2] = p.inside = 1; return p;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_patch")]
V hs_main(InputPatch<V, 3> input, uint id : SV_OutputControlPointID) { return input[id]; }
[domain("tri")]
V ds_main(Patch patch, float3 uvw : SV_DomainLocation, const OutputPatch<V, 3> input) {
  V v;
  v.position = input[0].position * uvw.x + input[1].position * uvw.y + input[2].position * uvw.z;
  v.value = input[0].value * uvw.x + input[1].value * uvw.y + input[2].value * uvw.z;
  v.clip = input[0].clip * uvw.x + input[1].clip * uvw.y + input[2].clip * uvw.z;
  v.cull = input[0].cull * uvw.x + input[1].cull * uvw.y + input[2].cull * uvw.z;
  return v;
}
#ifdef CONSERVATIVE_MESH
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void ms_main(uint3 group : SV_GroupID, out vertices V output_vertices[3], out indices uint3 output_indices[1]) {
  SetMeshOutputCounts(3, 1);
  for (uint i = 0; i < 3; ++i) output_vertices[i] = vertex(group.x * 3 + i);
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
  for (uint i = 0; i < 3; ++i) output_vertices[i] = vertex(payload_data.primitive * 3 + i);
  output_indices[0] = uint3(0, 1, 2);
}
#endif
