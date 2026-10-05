using namespace dx;
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint64_t> Wide : register(u0);
cbuffer LocalTag : register(b0, space1) { uint localTag; }
struct [raypayload] Payload {
  uint value : read(caller,closesthit,miss,anyhit) : write(caller,closesthit,miss,anyhit);
};
GlobalRootSignature HoGlobalRS = { "DescriptorTable(SRV(t0, numDescriptors = 2), UAV(u0))" };
LocalRootSignature HoLocalRS = { "RootConstants(num32BitConstants = 1, b0, space = 1)" };
SubobjectToExportsAssociation HoLocalAssoc = { "HoLocalRS", "HoGroup;HoProcGroup;HoMiss;HoCallable" };
RaytracingShaderConfig HoShaderConfig = { 4, 8 };
RaytracingPipelineConfig HoPipelineConfig = { 2 };
TriangleHitGroup HoGroup = { "HoAnyHit", "HoClosestHit" };
ProceduralPrimitiveHitGroup HoProcGroup = { "", "HoProcClosestHit", "HoIntersection" };
LocalRootSignature ExtLocalRS = { "CBV(b0, space = 1)" };
SubobjectToExportsAssociation ExtLocalAssoc = { "ExtLocalRS", "ExtGroup;ExtProcGroup;ExtMiss;ExtCallable" };
TriangleHitGroup ExtGroup = { "ExtAnyHit", "ExtClosestHit" };
ProceduralPrimitiveHitGroup ExtProcGroup = { "ExtAnyHit", "ExtClosestHit", "ExtIntersection" };

uint64_t laneBase() { return (uint64_t(0x24680000u + DispatchRaysIndex().x) << 32) | (0xfedc0000u + DispatchRaysIndex().x); }

void consumeAtomic(inout Payload p) {
  uint at = DispatchRaysIndex().x * 4;
  uint64_t old;
  InterlockedAdd(Wide[at], uint64_t(0x100000007ull) + localTag, old);
  p.value = (p.value * 3u) ^ uint(old) ^ localTag;
}

[shader("raygeneration")]
void HoRayGen() {
  uint lane = DispatchRaysIndex().x;
  uint at = lane * 4;
  uint64_t seed = laneBase();
  Wide[at] = seed;
  uint64_t old;
  InterlockedAdd(Wide[at], uint64_t(0x200000021ull), old);
  Wide[at + 1] = old;
  Payload p = {lane * 3u + 5u};
  RayDesc ray;
  ray.Origin = float3((lane & 1u) ? -10.0 : 2.25, 0.25, -1.0);
  ray.Direction = float3(0, 0, 1);
  ray.TMin = 0;
  ray.TMax = 100;
  HitObject hit = HitObject::TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 255, 0, 1, 0, ray, p);
  MaybeReorderThread(hit, lane & 3u, 2);
  HitObject::Invoke(hit, p);
  CallShader(0, p);
  Wide[at + 2] = Wide[at];
  Wide[at + 3] = (uint64_t(p.value) << 32) | (hit.IsHit() ? 1u : 2u);
}

[shader("closesthit")]
void HoClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) { consumeAtomic(p); }

[shader("miss")]
void HoMiss(inout Payload p) { consumeAtomic(p); }

[shader("callable")]
void HoCallable(inout Payload p) {
  uint at = DispatchRaysIndex().x * 4;
  uint64_t before = Wide[at];
  uint64_t old;
  InterlockedCompareExchange(Wide[at], before, before ^ uint64_t(0x0102030405060708ull), old);
  p.value ^= uint(old) ^ localTag;
}

// These linked exports intentionally have no device64 atomic themselves. Their
// fixed visible/intersection ABI still belongs to the atomic whole pipeline.
[shader("anyhit")]
void HoAnyHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) { }

[shader("intersection")]
void HoIntersection() {
  BuiltInTriangleIntersectionAttributes a;
  a.barycentrics = float2(0.25, 0.5);
  ReportHit(1.5, 77, a);
}

[shader("closesthit")]
void HoProcClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) { consumeAtomic(p); }

void extCount() {
  uint64_t old;
  InterlockedAdd(Wide[241], uint64_t(1), old);
}

void extAdd(uint slot, uint64_t delta) {
  uint at = DispatchRaysIndex().x * 10;
  uint64_t old;
  InterlockedAdd(Wide[at], delta, old);
  Wide[at + slot] = old;
  extCount();
}

[shader("raygeneration")]
void ExtRayGen() {
  uint lane = DispatchRaysIndex().x, at = lane * 10;
  for (uint i = 0; i < 10; ++i) Wide[at + i] = 0;
  Wide[at] = laneBase();
  extAdd(1, uint64_t(0x200000021ull));
  uint64_t ticket;
  InterlockedAdd(Wide[240], uint64_t(0x100000001ull), ticket);
  Wide[at + 6] = ticket;
  Payload p = {0};
  RayDesc ray;
  ray.Origin = float3(lane % 3 == 0 ? 2.25 : lane % 3 == 1 ? 4.25 : -10.0, 0.25, -1);
  ray.Direction = float3(0, 0, 1);
  ray.TMin = 0;
  ray.TMax = 100;
  HitObject hit = HitObject::TraceRay(Scene, RAY_FLAG_FORCE_NON_OPAQUE, 255, 0, 1, 0, ray, p);
  MaybeReorderThread(hit, lane & 3, 2);
  HitObject::Invoke(hit, p);
  CallShader(0, p);
  Wide[at + 7] = hit.IsHit() ? 1 : 2;
  Wide[at + 8] = p.value;
}

[shader("anyhit")]
void ExtAnyHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) {
  extAdd(2, uint64_t(0x800000017ull));
  p.value += 1;
}

[shader("intersection")]
void ExtIntersection() {
  extAdd(3, uint64_t(0x400000013ull));
  BuiltInTriangleIntersectionAttributes a;
  a.barycentrics = float2(0.25, 0.5);
  ReportHit(1.5, 77, a);
}

[shader("closesthit")]
void ExtClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes a) {
  extAdd(4, uint64_t(0x100000007ull) + localTag);
  Wide[DispatchRaysIndex().x * 10 + 9] = localTag;
  p.value += 0x10000;
}

[shader("miss")]
void ExtMiss(inout Payload p) {
  extAdd(4, uint64_t(0x300000009ull) + localTag);
  Wide[DispatchRaysIndex().x * 10 + 9] = localTag;
  p.value += 0x20000;
}

[shader("callable")]
void ExtCallable(inout Payload p) {
  uint at = DispatchRaysIndex().x * 10;
  uint64_t before = Wide[at], old;
  InterlockedCompareExchange(Wide[at], before, before ^ uint64_t(0x0102030405060708ull), old);
  Wide[at + 5] = old;
  p.value += 0x1000000 + localTag;
  extCount();
}
