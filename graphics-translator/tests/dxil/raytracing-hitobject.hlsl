using namespace dx;
struct [raypayload] Payload {
  uint value : read(caller,closesthit,miss,anyhit) : write(caller,closesthit,miss,anyhit);
};
struct Attr { float2 bary; };
RaytracingAccelerationStructure Scene : register(t0);
reordercoherent RWStructuredBuffer<uint> Out : register(u0);
cbuffer LocalConstants : register(b0, space1) { uint localTag; };
GlobalRootSignature HoGlobalRS = { "DescriptorTable(SRV(t0, numDescriptors = 2), UAV(u0))" };
LocalRootSignature HoLocalRS = { "RootConstants(num32BitConstants = 1, b0, space = 1)" };
SubobjectToExportsAssociation HoLocalAssoc = { "HoLocalRS", "HoGroup;HoProcGroup;HoMiss;HoCoherenceGroup;HoCoherenceMiss" };
RaytracingShaderConfig HoShaderConfig = { 4, 8 };
RaytracingPipelineConfig HoPipelineConfig = { 2 };
TriangleHitGroup HoGroup = { "HoAnyHit", "HoClosestHit" };
ProceduralPrimitiveHitGroup HoProcGroup = { "", "HoProcClosestHit", "HoIntersection" };
TriangleHitGroup HoCoherenceGroup = { "", "HoCoherenceHit" };

[shader("raygeneration")]
void HoRayGen() {
  uint lane = DispatchRaysIndex().x;
  uint base = lane * 32;
  RayDesc ray = { float3(2.25,0.25,-1), 0, float3(0,0,1), 100 };
  Payload p = { 0 };
  HitObject saved = HitObject::MakeNop();
  // One traversal callsite executes twice. Retaining the first result must not
  // alias the second call's working block (the second ray is a miss).
  [loop] for (uint i = 0; i < 2; ++i) {
    RayDesc active = ray;
    if (i != 0) active.Origin.x = -10;
    HitObject current = HitObject::TraceRay(Scene, 0, 255, 0, 1, 0, active, p);
    if (i == 0) saved = current;
  }
  Out[base] = p.value;
  Out[base+1] = saved.IsHit();
  Out[base+2] = saved.GetGeometryIndex();
  Out[base+3] = saved.GetShaderTableIndex();
  Out[base+4] = saved.LoadLocalRootTableConstant(0);
  Attr attr; saved.GetAttributes(attr);
  Out[base+5] = asuint(attr.bary.x);
  Out[base+6] = asuint(saved.GetRayTCurrent());
  Out[base+7] = saved.GetInstanceID();
  HitObject changed = saved;
  changed.SetShaderTableIndex(0x10000000);
  Out[base+8] = changed.GetShaderTableIndex();
  Out[base+9] = saved.GetShaderTableIndex();
  MaybeReorderThread(saved, lane & 3, 2);
  HitObject::Invoke(saved, p);
  HitObject::Invoke(saved, p);
  Out[base+10] = p.value;
  HitObject::Invoke(changed, p);
  Out[base+11] = p.value;
  HitObject miss = HitObject::MakeMiss(0, 0x10001, ray);
  Out[base+12] = miss.IsMiss();
  Out[base+13] = miss.GetShaderTableIndex();
  HitObject::Invoke(miss, p);
  Out[base+14] = p.value;
  HitObject nop = HitObject::MakeNop();
  nop.SetShaderTableIndex(1);
  MaybeReorderThread(lane, 2);
  HitObject::Invoke(nop, p);
  Out[base+15] = p.value;
  Out[base+16] = nop.IsNop();
  Out[base+17] = nop.GetShaderTableIndex();
  RayQuery<0> query;
  query.TraceRayInline(Scene, 0, 255, ray);
  while(query.Proceed()) {
    if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) query.CommitNonOpaqueTriangleHit();
  }
  HitObject from = HitObject::FromRayQuery(query);
  Out[base+18] = from.IsHit();
  Out[base+19] = from.GetShaderTableIndex();
  HitObject::Invoke(from, p);
  Out[base+20] = p.value;
  // Query reset must not change the already-snapshotted hit object.
  RayDesc emptyRay = ray; emptyRay.Origin.x = -10;
  query.TraceRayInline(Scene, 0, 255, emptyRay);
  while(query.Proceed()) {}
  HitObject empty = HitObject::FromRayQuery(query);
  Out[base+21] = empty.IsNop();
  from.SetShaderTableIndex(1);
  HitObject::Invoke(from, p);
  Out[base+22] = p.value;
  Out[base+23] = from.GetGeometryIndex();
  Out[base+24] = asuint(saved.GetWorldRayOrigin().x);
  Out[base+25] = asuint(saved.GetObjectRayDirection().z);
  Out[base+26] = asuint(saved.GetObjectToWorld3x4()[0][0]);
  Out[base+27] = asuint(saved.GetWorldToObject3x4()[2][2]);
  Out[base+28] = saved.GetPrimitiveIndex();
  Out[base+29] = saved.GetRayFlags();
  Out[base+30] = saved.GetHitKind();
  Out[base+31] = asuint(saved.GetRayTMin());
}
[shader("anyhit")] void HoAnyHit(inout Payload p, Attr attr) { p.value += 1; }
[shader("closesthit")] void HoClosestHit(inout Payload p, Attr attr) { p.value += localTag; }
[shader("miss")] void HoMiss(inout Payload p) { p.value += localTag; }
[shader("intersection")] void HoIntersection() { Attr attr = {float2(0.5,0.75)}; ReportHit(1.5, 7, attr); }
[shader("closesthit")] void HoProcClosestHit(inout Payload p, Attr attr) { p.value += localTag; }

[shader("raygeneration")]
void HoProcRayGen() {
  uint base = DispatchRaysIndex().x * 32;
  RayDesc ray = {float3(4.25,0.25,-1), 0, float3(0,0,1), 100};
  Payload p = {0};
  HitObject hit = HitObject::TraceRay(Scene, 0, 255, 0, 1, 0, ray, p);
  Attr attr; hit.GetAttributes(attr);
  Out[base] = hit.IsHit();
  Out[base+1] = hit.GetHitKind();
  Out[base+2] = asuint(attr.bary.x);
  Out[base+3] = asuint(attr.bary.y);
  Out[base+4] = hit.GetShaderTableIndex();
  HitObject::Invoke(hit,p); HitObject::Invoke(hit,p);
  Out[base+5] = p.value;
  RayQuery<0> query; query.TraceRayInline(Scene,0,255,ray);
  while(query.Proceed()) {
    if(query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) query.CommitProceduralPrimitiveHit(1.5);
  }
  Attr custom = {float2(0.125,0.25)};
  HitObject from = HitObject::FromRayQuery(query,123,custom);
  RayDesc empty = ray; empty.Origin.x = -10;
  query.TraceRayInline(Scene,0,255,empty); while(query.Proceed()) {}
  from.GetAttributes(attr);
  Out[base+6] = from.IsHit();
  Out[base+7] = from.GetHitKind();
  Out[base+8] = asuint(attr.bary.x);
  Out[base+9] = asuint(attr.bary.y);
  Out[base+10] = asuint(from.GetRayTCurrent());
  Out[base+11] = from.GetInstanceIndex();
  from.SetShaderTableIndex(2); HitObject::Invoke(from,p);
  Out[base+12] = p.value;
}

struct Aggregate { HitObject hit; uint tag; };

[shader("raygeneration")]
void HoRayStateRayGen() {
  uint base = DispatchRaysIndex().x * 32;
  RayDesc ray = {float3(2.25,0.25,-1),0.125,float3(0,0,1),100};
  Payload p = {0};
  HitObject hit = HitObject::TraceRay(Scene,RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,255,0,1,0,ray,p);
  HitObject::Invoke(hit,p);
  Out[base] = p.value;
  Out[base+1] = hit.GetRayFlags();
  Out[base+2] = asuint(hit.GetRayTMin());
  Out[base+3] = asuint(hit.GetWorldRayDirection().z);
  Out[base+4] = asuint(hit.GetObjectRayOrigin().x);
  Out[base+5] = asuint(hit.GetObjectRayOrigin().y);
  Out[base+6] = asuint(hit.GetObjectRayOrigin().z);
  Out[base+7] = hit.GetInstanceIndex();
  RayQuery<0> query; query.TraceRayInline(Scene,0,255,ray);
  while(query.Proceed()) {
    if(query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) query.CommitNonOpaqueTriangleHit();
  }
  Attr custom = {float2(0.125,0.75)};
  HitObject from = HitObject::FromRayQuery(query,123,custom);
  Attr actual; from.GetAttributes(actual);
  Out[base+8] = from.GetHitKind();
  Out[base+9] = asuint(actual.bary.x);
  Out[base+10] = asuint(actual.bary.y);
  Out[base+11] = from.LoadLocalRootTableConstant(0);
  ray.Origin.x = -10;
  HitObject miss = HitObject::TraceRay(Scene,0,255,0,1,0,ray,p);
  Out[base+12] = p.value;
  Out[base+13] = miss.IsMiss();
  Out[base+14] = asuint(miss.GetRayTCurrent());
  HitObject::Invoke(miss,p);
  Out[base+15] = p.value;
  HitObject made = HitObject::MakeMiss(RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0,ray);
  HitObject::Invoke(made,p);
  Out[base+16] = p.value;
  Out[base+17] = made.GetRayFlags();
  Out[base+18] = asuint(made.GetRayTMin());
  Out[base+19] = asuint(made.GetWorldRayOrigin().x);
  Out[base+20] = asuint(made.GetWorldRayOrigin().y);
  Out[base+21] = asuint(made.GetWorldRayOrigin().z);
  Out[base+22] = asuint(made.GetWorldRayDirection().z);
  Out[base+23] = asuint(made.GetRayTCurrent());
}

HitObject rebound(HitObject h) { h.SetShaderTableIndex(0); return h; }
uint inspect(HitObject h) { return h.IsNop() ? 17 : h.GetShaderTableIndex(); }
[shader("raygeneration")]
void HoAggregateRayGen() {
  HitObject values[2];
  values[0] = HitObject::MakeNop();
  RayDesc ray = {float3(0,0,0),0,float3(0,0,1),10};
  values[1] = HitObject::MakeMiss(0,1,ray);
  uint lane = DispatchRaysIndex().x;
  Aggregate a = {values[lane & 1],11};
  Aggregate b = a; b.hit = rebound(a.hit);
  Out[lane] = inspect(b.hit) + a.hit.GetShaderTableIndex() + b.tag;
}

// Each dispatch index owns eight words. REORDER_SCOPE promises visibility for
// that same dispatch index across reorder points, not cross-invocation rendezvous.
// The host launches64 lanes (> the device maximum wave width), with no global
// spin/barrier or assumption about which lanes share a physical wave.
[shader("raygeneration")]
void HoCoherenceRayGen() {
  uint lane = DispatchRaysIndex().x;
  uint base = lane * 8;
  Out[base] = 0x43210000u + lane * 0x105u;
  Barrier(UAV_MEMORY, REORDER_SCOPE);
  MaybeReorderThread(lane & 7, 3);
  uint seen = Out[base];
  Out[base+2] = seen ^ 0x13579bdfu;
  Payload p = {seen ^ 0x13579bdfu};
  RayDesc ray = {float3((lane & 1) ? -10.0 : 2.25,0.25,-1),0,float3(0,0,1),100};
  HitObject hit = HitObject::TraceRay(Scene,RAY_FLAG_FORCE_OPAQUE,255,0,1,0,ray,p);
  Out[base+3] = p.value; // Traversal must not execute the deferred consumer.
  Out[base+1] = seen * 7u + 0x10203040u;
  Barrier(UAV_MEMORY, REORDER_SCOPE);
  MaybeReorderThread(hit, lane & 3, 2);
  HitObject::Invoke(hit,p);
  Out[base+4] = p.value; // CH/miss consumes both pre-reorder UAV messages.
  Out[base+5] = Out[base+1];
  Out[base+1] = p.value + 0x6a09e667u;
  Barrier(UAV_MEMORY, REORDER_SCOPE);
  MaybeReorderThread(hit);
  Out[base+6] = Out[base+1] ^ 0xbb67ae85u;
  Out[base+7] = hit.IsHit() ? 1 : 2;
}

void coherenceConsumer(inout Payload p) {
  uint base = DispatchRaysIndex().x * 8;
  uint observed = Out[base];
  uint second = Out[base+1];
  uint produced = (observed ^ 0x00ff00ffu) + (second ^ 0x55aa55aau) + (p.value + localTag) * 3u;
  p.value = produced ^ 0x2468ace0u;
}
[shader("closesthit")] void HoCoherenceHit(inout Payload p, Attr attr) { coherenceConsumer(p); }
[shader("miss")] void HoCoherenceMiss(inout Payload p) { coherenceConsumer(p); }
