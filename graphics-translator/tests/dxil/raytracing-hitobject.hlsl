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


// A single procedural primitive reports far then near. This fixes the oracle's
// order in shader code, independently of native BVH visitation order.
ProceduralPrimitiveHitGroup HoFirstGroup = { "HoFirstAnyHit", "HoFirstClosestHit", "HoFirstIntersection" };
ProceduralPrimitiveHitGroup HoFirstOpaqueGroup = { "", "HoFirstClosestHit", "HoFirstIntersection" };
TriangleHitGroup HoFirstRejectGroup = { "HoFirstAnyHit", "" };

void writeNoHitState(HitObject hit, uint base, bool callbackSeen) {
  Out[base] = hit.GetRayFlags();
  Out[base+1] = asuint(hit.GetRayTMin());
  Out[base+2] = asuint(hit.GetRayTCurrent());
  Out[base+3] = hit.IsHit() ? 1 : hit.IsMiss() ? 2 : 0;
  float3 worldOrigin = hit.GetWorldRayOrigin(), worldDirection = hit.GetWorldRayDirection();
  float3 objectOrigin = hit.GetObjectRayOrigin(), objectDirection = hit.GetObjectRayDirection();
  [unroll] for (uint i = 0; i < 3; ++i) {
    Out[base+4+i] = asuint(worldOrigin[i]);
    Out[base+7+i] = asuint(worldDirection[i]);
    Out[base+10+i] = asuint(objectOrigin[i]);
    Out[base+13+i] = asuint(objectDirection[i]);
  }
  float3x4 objectToWorld = hit.GetObjectToWorld3x4(), worldToObject = hit.GetWorldToObject3x4();
  [unroll] for (uint row = 0; row < 3; ++row)
    [unroll] for (uint col = 0; col < 4; ++col) {
      Out[base+16+row*4+col] = asuint(objectToWorld[row][col]);
      Out[base+28+row*4+col] = asuint(worldToObject[row][col]);
    }
  Out[base+40] = hit.GetGeometryIndex();
  Out[base+41] = hit.GetInstanceIndex();
  Out[base+42] = hit.GetInstanceID();
  Out[base+43] = hit.GetPrimitiveIndex();
  Out[base+44] = hit.GetHitKind();
  Attr a; hit.GetAttributes(a);
  Out[base+45] = asuint(a.bary.x);
  Out[base+46] = asuint(a.bary.y);
  Out[base+47] = hit.GetShaderTableIndex();
  Out[base+48] = callbackSeen;
}

void firstHitTrace(bool deferred) {
  uint lane = DispatchRaysIndex().x;
  uint mode = lane & 7;
  [loop] for (uint iteration = 0; iteration < 2; ++iteration) {
    uint base = lane * 32 + iteration * 16;
    Out[base+1] = 0;
    Out[base+2] = 0;
    Payload p = {mode | (iteration << 4)};
    uint flags = mode == 6 ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_FORCE_NON_OPAQUE;
    if (mode == 1 || mode == 2 || mode == 5 || mode == 6 || mode == 7)
      flags |= RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
    if (mode == 5) flags |= RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;
    RayDesc ray = {float3(mode == 7 ? (iteration ? 2.25 : -10) : 4.25,0.25,-1.0 - 2.0 * iteration),
                   0,float3(0,0,1),100};
    if (deferred) {
      HitObject hit = HitObject::TraceRay(Scene,flags,255,mode == 6 ? 1 : 0,1,0,ray,p);
      Out[base+8] = p.value;
      Out[base+9] = hit.IsHit() ? 1 : 2;
      Out[base+10] = asuint(hit.GetRayTCurrent());
      if (hit.IsHit()) {
        Attr a; hit.GetAttributes(a);
        Out[base+11] = asuint(a.bary.x);
      }
      if (mode == 4 && iteration == 1) {
        // All reports were rejected after publishing nonzero candidate state.
        writeNoHitState(hit,256,(p.value & 0xff00) != 0);
        RayDesc madeRay = {float3(-7,0.375,-5),0.125,float3(0.125,-0.25,1.5),17.25};
        writeNoHitState(HitObject::MakeMiss(RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,0,madeRay),320,false);
        writeNoHitState(HitObject::MakeNop(),384,false);
      }
      if (mode == 7 && iteration == 1)
        writeNoHitState(hit,448,(p.value & 0xff00) != 0);
      HitObject::Invoke(hit,p);
    } else {
      TraceRay(Scene,flags,255,mode == 6 ? 1 : 0,1,0,ray,p);
      Out[base+8] = p.value;
      Out[base+9] = 0;
    }
    Out[base] = p.value;
  }
}
[shader("raygeneration")] void HoFirstRayGen() { firstHitTrace(false); }
[shader("raygeneration")] void HoFirstDeferredRayGen() { firstHitTrace(true); }

[shader("intersection")]
void HoFirstIntersection() {
  float3 objectOrigin = ObjectRayOrigin();
  if (objectOrigin.x < 0 || objectOrigin.x > 1 || objectOrigin.y < 0 || objectOrigin.y > 1) return;
  uint iteration = ObjectRayOrigin().z < -2 ? 1 : 0;
  uint base = DispatchRaysIndex().x * 32 + iteration * 16;
  Out[base+1] = 1;
  float originOffset = -ObjectRayOrigin().z;
  Attr farHit = {float2(0.75,0.125)};
  bool farAccepted = ReportHit(originOffset + 0.75,91,farHit);
  Out[base+12] = asuint(RayTCurrent());
  Out[base+3] = farAccepted;
  Out[base+2] |= 1;
  Attr nearHit = {float2(0.25,0.625)};
  bool nearAccepted = ReportHit(originOffset + 0.25,92,nearHit);
  Out[base+13] = asuint(RayTCurrent());
  Out[base+4] = nearAccepted;
  Out[base+2] |= 2;
}
[shader("anyhit")]
void HoFirstAnyHit(inout Payload p, Attr a) {
  uint mode = p.value & 7;
  uint iteration = (p.value >> 4) & 1;
  p.value += 0x100;
  if (mode == 7) IgnoreHit();
  if (mode == 4 && iteration == 1) IgnoreHit();
  if ((mode == 2 || (mode == 3 && iteration == 1)) && a.bary.x == 0.75) IgnoreHit();
  if (mode == 3 && iteration == 0) AcceptHitAndEndSearch();
}
[shader("closesthit")]
void HoFirstClosestHit(inout Payload p, Attr a) {
  uint base = DispatchRaysIndex().x * 32 + ((p.value >> 4) & 1) * 16;
  Out[base+5] = asuint(RayTCurrent());
  Out[base+6] = HitKind();
  Out[base+7] = asuint(a.bary.x);
  p.value |= 0x10000;
}
[shader("miss")] void HoFirstMiss(inout Payload p) { p.value |= 0x20000; }
