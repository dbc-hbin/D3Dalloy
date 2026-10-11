// DXC -T lib_6_6 -HV 2021 -enable-payload-qualifiers -Fo raytracing-paq.dxil
// Derived from DXR PAQ Detailed semantics / Example 1: caller initializes0,
// closesthit writes1 locally, but caller must still observe0 without write(closesthit).
struct [raypayload] Payload {
  uint callerValue : write(caller) : read(caller);
  uint stageValue : write(caller, closesthit, miss, anyhit) : read(caller, closesthit, miss, anyhit);
  uint inputValue : write(caller) : read(closesthit, miss, anyhit);
  uint lane : write(caller) : read(caller, closesthit, miss, anyhit);
  uint nested : write(caller, closesthit, miss) : read(caller, closesthit, miss);
};
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint> Out : register(u0);
GlobalRootSignature PaqGlobalRS = { "DescriptorTable(SRV(t0, numDescriptors = 2), UAV(u0))" };
RaytracingShaderConfig PaqShaderConfig = { 20, 8 };
RaytracingPipelineConfig PaqPipelineConfig = { 2 };
TriangleHitGroup PaqGroup = { "PaqAnyHit", "PaqClosestHit" };

[shader("raygeneration")]
void PaqRayGen() {
  uint lane = DispatchRaysIndex().x;
  Payload p = { 0, 100, 7, lane, 0 };
  RayDesc ray = { float3(lane == 1 ? -10 : 0.25, 0.25, -1), 0, float3(0,0,1), 100 };
  TraceRay(Scene, RAY_FLAG_FORCE_NON_OPAQUE | (lane == 5 ? RAY_FLAG_SKIP_CLOSEST_HIT_SHADER : 0),
           255, lane & 1, 1, lane & 1, ray, p);
  Out[lane * 16] = p.callerValue;
  Out[lane * 16 + 1] = p.stageValue;
  Out[lane * 16 + 2] = p.nested;
  // inputValue is not read(caller); its returned value is deliberately not asserted.
}
[shader("anyhit")]
void PaqAnyHit(inout Payload p, BuiltInTriangleIntersectionAttributes attributes) {
  uint base = p.lane * 16;
  Out[base + 5] = p.inputValue;
  p.callerValue = 3;
  p.inputValue = 9;
  Out[base + 4] = p.callerValue + p.inputValue;
  p.stageValue += 10;
  if (p.lane == 2) IgnoreHit();
  if (p.lane == 3) AcceptHitAndEndSearch();
}
[shader("closesthit")]
void PaqClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attributes) {
  uint base = p.lane * 16;
  Out[base + 7] = p.inputValue;
  p.callerValue = 1;
  Out[base + 6] = p.callerValue;
  p.stageValue += 1;
  if (p.lane == 4) {
    RayDesc ray = { float3(-10,0,-1), 0, float3(0,0,1), 100 };
    p.nested = 1;
    // A closest-hit is the caller of this nested TraceRay. Its unrestricted
    // working value1 must survive the nested miss's denied callerValue write2.
    TraceRay(Scene, 0, 255, p.lane & 1, 1, (p.lane + 1) & 1, ray, p);
    Out[base + 8] = p.callerValue;
  }
}
[shader("miss")]
void PaqMiss(inout Payload p) {
  uint base = p.lane * 16;
  Out[base + 7] = p.inputValue;
  p.callerValue = 2;
  Out[base + 6] = p.callerValue;
  p.stageValue += 2;
}
[shader("miss")]
void PaqMissAlt(inout Payload p) {
  uint base = p.lane * 16;
  Out[base + 7] = p.inputValue;
  p.callerValue = 2;
  Out[base + 6] = p.callerValue;
  p.stageValue += p.nested ? 20 : 2;
}
