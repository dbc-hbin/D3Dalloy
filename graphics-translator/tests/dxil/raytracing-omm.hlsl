using namespace dx;
struct [raypayload] Payload {
  uint lane : read(caller,closesthit,miss,anyhit) : write(caller,closesthit,miss,anyhit);
  uint hit : read(caller,closesthit,miss,anyhit) : write(caller,closesthit,miss,anyhit);
};
struct LaneRay {
  float3 origin; float tmin; float3 direction; float tmax;
  uint flags, mask, contribution, multiplier, missIndex, reject, pad1, pad2;
};
RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<LaneRay> Rays : register(t1);
RWStructuredBuffer<uint> Out : register(u0);
GlobalRootSignature SoGlobalRS = { "DescriptorTable(SRV(t0, numDescriptors = 2), UAV(u0))" };
RaytracingShaderConfig OmmShaderConfig = { 8, 8 };
TriangleHitGroup OmmGroup = { "OmmAnyHit", "OmmClosestHit" };
[shader("raygeneration")]
void OmmRayGen() {
  uint lane = DispatchRaysIndex().x, base = lane * 32;
  LaneRay r = Rays[lane];
  RayDesc ray = {r.origin, r.tmin, r.direction, r.tmax};
  Out[base + 1] = 0;
  Payload p = {lane, 0};
  TraceRay(Scene, r.flags, r.mask, 0, 1, 0, ray, p);
  Out[base] = p.hit;
  Out[base + 6] = Out[base + 1];
#if OMM_LEGACY_QUERY
  RayQuery<0> q; // Before SM6.9, the pipeline flag supplies the opt-in.
#else
  RayQuery<0, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> q;
#endif
  q.TraceRayInline(Scene, r.flags, r.mask, ray);
  uint candidates = 0;
  while (q.Proceed()) {
    ++candidates;
    if (r.reject == 0) q.CommitNonOpaqueTriangleHit();
  }
  Out[base + 2] = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  Out[base + 3] = candidates;
#if !OMM_LEGACY_QUERY
  Out[base + 1] = 0;
  HitObject h = HitObject::TraceRay(Scene, r.flags, r.mask, 0, 1, 0, ray, p);
  Out[base + 4] = h.IsHit();
  Out[base + 5] = Out[base + 1];
  // SM6.9 queries without a feature flag do not inherit the pipeline opt-in.
  RayQuery<0> optedOut;
  optedOut.TraceRayInline(Scene, r.flags, r.mask, ray);
  uint ordinaryCandidates = 0;
  while (optedOut.Proceed()) {
    ++ordinaryCandidates;
    if (r.reject == 0) optedOut.CommitNonOpaqueTriangleHit();
  }
  Out[base + 8] = optedOut.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  Out[base + 9] = ordinaryCandidates;
#endif
}
[shader("anyhit")]
void OmmAnyHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) {
  ++Out[p.lane * 32 + 1];
  if (Rays[p.lane].reject != 0) IgnoreHit();
}
[shader("closesthit")]
void OmmClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) { p.hit = 1; }
[shader("miss")]
void OmmMiss(inout Payload p) { p.hit = 0; }
