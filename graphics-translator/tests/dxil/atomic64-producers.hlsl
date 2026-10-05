// Each producer owns a distinct public UAV cell; rerunning capture during sample replay is observable.
RWByteAddressBuffer logs : register(u0);
RWStructuredBuffer<uint64_t> cells : register(u1);
cbuffer Control : register(b0) { uint child_x; uint child_y; uint child_z; uint grid_x; uint grid_y; uint unused0; uint unused1; uint unused2; }
struct Vertex { float4 position : SV_Position; nointerpolation uint good : TEXCOORD0; nointerpolation uint instance : TEXCOORD1; };
uint produce(uint stage, uint lane) {
  uint key = stage * 64 + lane;
  uint64_t initial = 0x1234567800000000ULL + uint64_t(key) * 256ULL;
  uint64_t old, cas;
  InterlockedAdd(cells[key], 1ULL, old);
  InterlockedCompareExchange(cells[key], initial + 1ULL, initial + 9ULL, cas);
  uint ignored;
  logs.InterlockedAdd(key * 32 + 16, 1, ignored);
  logs.Store<uint64_t>(key * 32, old);
  logs.Store<uint64_t>(key * 32 + 8, cas);
  return uint(old == initial && cas == initial + 1ULL);
}
Vertex corner(uint id) {
  Vertex v;
  float2 p = float2((id << 1) & 2, id & 2);
  v.position = float4(p * float2(2, -2) + float2(-1, 1), .5, 1);
  v.good = 1; v.instance = 0;
  return v;
}
Vertex vs_plain(uint id : SV_VertexID, uint instance : SV_InstanceID) { Vertex v = corner(id); v.instance = instance; return v; }
Vertex vs_atomic(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  Vertex v = corner(id); v.instance = instance; v.good = produce(0, instance * 3 + id); return v;
}
struct Factors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<Vertex, 3> patch) {
  Factors f; f.edge[0] = f.edge[1] = f.edge[2] = f.inside = 1; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("factors")]
Vertex hs_plain(InputPatch<Vertex, 3> patch, uint id : SV_OutputControlPointID) { return patch[id]; }
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("factors")]
Vertex hs_atomic(InputPatch<Vertex, 3> patch, uint id : SV_OutputControlPointID, uint primitive : SV_PrimitiveID) {
  Vertex v = patch[id]; v.good &= produce(1, patch[0].instance * 3 + id); return v;
}
[domain("tri")]
Vertex ds_plain(Factors f, float3 location : SV_DomainLocation, const OutputPatch<Vertex, 3> patch) {
  Vertex v; v.position = patch[0].position * location.x + patch[1].position * location.y + patch[2].position * location.z;
  v.good = patch[0].good & patch[1].good & patch[2].good; v.instance = patch[0].instance; return v;
}
[domain("tri")]
Vertex ds_atomic(Factors f, float3 location : SV_DomainLocation, const OutputPatch<Vertex, 3> patch, uint primitive : SV_PrimitiveID) {
  Vertex v = ds_plain(f, location, patch);
  uint corner_id = location.x > .5 ? 0 : location.y > .5 ? 1 : 2;
  v.good &= produce(2, patch[0].instance * 3 + corner_id); return v;
}
[maxvertexcount(3)][instance(2)]
void gs_atomic(triangle Vertex input[3], uint primitive : SV_PrimitiveID, uint instance : SV_GSInstanceID, inout TriangleStream<Vertex> stream) {
  uint good = produce(3, input[0].instance * 2 + instance);
  [unroll] for (uint i = 0; i < 3; ++i) { Vertex v = input[i]; v.good &= good; stream.Append(v); }
  stream.RestartStrip();
}
struct Payload { uint parent; uint good; };
groupshared Payload payload;
[numthreads(4,1,1)]
void as_atomic(uint lane : SV_GroupIndex, uint3 group : SV_GroupID) {
  uint parent = group.x + grid_x * group.y;
  uint good = produce(4, parent * 4 + lane);
  if (lane == 0) { payload.parent = parent; payload.good = good; }
  GroupMemoryBarrierWithGroupSync();
  DispatchMesh(child_x, child_y, child_z, payload);
}
[numthreads(4,1,1)][outputtopology("triangle")]
void ms_atomic(uint lane : SV_GroupIndex, uint3 group : SV_GroupID, out vertices Vertex vertices[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3, 1);
  uint index = group.x + grid_x * group.y + grid_x * grid_y * group.z;
  uint good = produce(5, index * 4 + lane);
  if (lane < 3) { vertices[lane] = corner(lane); vertices[lane].good = good; }
  if (lane == 0) triangles[0] = uint3(0,1,2);
}
[numthreads(4,1,1)][outputtopology("triangle")]
void ms_payload(uint lane : SV_GroupIndex, uint3 group : SV_GroupID, in payload Payload p, out vertices Vertex vertices[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3, 1);
  uint child = group.x + child_x * group.y + child_x * child_y * group.z;
  uint index = p.parent * child_x * child_y * child_z + child;
  uint good = produce(5, index * 4 + lane) & p.good;
  if (lane < 3) { vertices[lane] = corner(lane); vertices[lane].good = good; }
  if (lane == 0) triangles[0] = uint3(0,1,2);
}
// Additive UNORM bytes count every emitted triangle, not just the last overlapping one.
// Using SampleIndex in the value forces sample-frequency replay without producer side effects.
float4 ps_sample(Vertex v, uint sample : SV_SampleIndex) : SV_Target {
  float value = float((sample + 1) * 4) / 255.0;
  return float4(v.good ? 0 : value, v.good ? value : 0, 0, 16.0 / 255.0);
}
