// All remaining DXR 1.1 RayQuery stages. Reuses the exact 24-lane triangle/AABB query;
// independent scene-derived expected words live in rayquery::Runner::verify.
#include "raytracing-rayquery.hlsl"

#ifndef RQS_LIB
struct Control { float4 position : SV_Position; uint lane : LANE; };
Control stage_vs(uint id : SV_VertexID) {
  Control v; v.position = float4(2, 2, 0, 1); v.lane = id; return v;
}
struct Factors { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Factors factors(InputPatch<Control, 1> p) {
  Factors f; f.edge[0] = f.edge[1] = f.edge[2] = f.inside = 1; return f;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(1)][patchconstantfunc("factors")]
Control stage_hs(InputPatch<Control, 1> p, uint cp : SV_OutputControlPointID) {
#ifdef RQS_HS
  traceLane(p[0].lane, 6);
#endif
  return p[0];
}
[domain("tri")]
float4 stage_ds(Factors f, float3 uv : SV_DomainLocation, const OutputPatch<Control, 1> p) : SV_Position {
#ifdef RQS_DS
  // Exactly one corner per patch: no duplicate UAV writers from the other two DS invocations.
  if (uv.x == 1) traceLane(p[0].lane, 7);
#endif
  return p[0].position;
}
[maxvertexcount(1)]
void stage_gs(point Control p[1], inout PointStream<Control> stream) {
  traceLane(p[0].lane, 8); stream.Append(p[0]);
}
struct Payload { uint lane; };
groupshared Payload payload;
[numthreads(1, 1, 1)]
void stage_as(uint3 group : SV_GroupID) {
  traceLane(group.x, 9); payload.lane = group.x; DispatchMesh(1, 1, 1, payload);
}
[outputtopology("triangle")][numthreads(1, 1, 1)]
void stage_ms(uint3 group : SV_GroupID, out vertices Control vertices[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3, 1); traceLane(group.x, 10);
  [unroll] for (uint i = 0; i < 3; ++i) { vertices[i].position = float4(2, 2, 0, 1); vertices[i].lane = group.x; }
  triangles[0] = uint3(0, 1, 2);
}
[outputtopology("triangle")][numthreads(1, 1, 1)]
void stage_ms_sink(in payload Payload p, out vertices Control vertices[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3, 1);
  [unroll] for (uint i = 0; i < 3; ++i) { vertices[i].position = float4(2, 2, 0, 1); vertices[i].lane = p.lane; }
  triangles[0] = uint3(0, 1, 2);
}

#endif
#ifdef RQS_LIB
struct RPayload { uint unused; };
struct Attributes { float2 uv; };
void launch(float x, uint flags) {
  RayDesc ray; ray.Origin = float3(x, .5, -1); ray.Direction = float3(0, 0, 1);
  ray.TMin = 0; ray.TMax = 100; RPayload p; p.unused = 0;
  TraceRay(Scene, flags, 0xff, 0, 1, 0, ray, p);
}
[shader("raygeneration")] void RQSClosestGen() { launch(10.5, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES); }
[shader("raygeneration")] void RQSAnyGen() { launch(10.5, RAY_FLAG_FORCE_NON_OPAQUE); }
[shader("raygeneration")] void RQSIntersectionGen() { launch(30.5, RAY_FLAG_FORCE_OPAQUE); }
[shader("raygeneration")] void RQSMissGen() { launch(50.5, RAY_FLAG_NONE); }
[shader("raygeneration")] void RQSCallableGen() { RPayload p; p.unused = 0; CallShader(0, p); }
[shader("closesthit")] void RQSClosest(inout RPayload p, Attributes a) { traceLane(DispatchRaysIndex().x, 11); }
[shader("anyhit")] void RQSAny(inout RPayload p, Attributes a) { traceLane(DispatchRaysIndex().x, 12); IgnoreHit(); }
[shader("intersection")] void RQSIntersection() {
  traceLane(DispatchRaysIndex().x, 13); Attributes a; a.uv = 0; ReportHit(3, 0, a);
}
[shader("miss")] void RQSMiss(inout RPayload p) { traceLane(DispatchRaysIndex().x, 14); }
[shader("miss")] void RQSNoMiss(inout RPayload p) {}
[shader("callable")] void RQSCallable(inout RPayload p) { traceLane(DispatchRaysIndex().x, 15); }
#endif
