// Shader Model 6.x inputs the owned compiler must reject with a clear error
// (owned-d3d12-sm6.cpp). Compile with DXC 1.9: -HV 2021 -T <profile> -E <entry>.

RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);

// Wave size attribute: 32 is accepted, others must be rejected.
[WaveSize(32)] [numthreads(32, 1, 1)]
void cs_wavesize32(uint gi : SV_GroupIndex) { Out.Store(gi * 4, WaveGetLaneCount()); }
[WaveSize(64)] [numthreads(64, 1, 1)]
void cs_wavesize64(uint gi : SV_GroupIndex) { Out.Store(gi * 4, WaveGetLaneCount()); }

// FP64 must be rejected (DoublePrecisionFloatShaderOps = FALSE).
[numthreads(1, 1, 1)]
void cs_fp64() { double d = asfloat(Raw.Load(0)); Out.Store(0, (uint)(d * d)); }
// 64 bit groupshared atomics must be rejected (AtomicInt64OnGroupSharedSupported = FALSE).
groupshared uint64_t Shared64;
[numthreads(1, 1, 1)]
void cs_gs64() { InterlockedAdd(Shared64, 1); Out.Store<uint64_t>(0, Shared64); }

// Compiled as cs_6_9: above the highest supported shader model (6.8).
[numthreads(1, 1, 1)]
void cs_sm69() { Out.Store(0, 1); }

// Cap-gated SM 6.1-6.5 features the device does not report; pipeline creation must fail.
struct VRate {
  float4 pos : SV_Position;
  nointerpolation uint rate : SV_ShadingRate;
};
// SV_ShadingRate (VariableShadingRateTier is NOT_SUPPORTED): the VS writes it, the PS reads it.
VRate vs_shadingrate(uint vid : SV_VertexID) {
  VRate o;
  o.pos = float4(vid == 1 ? 3.0f : -1.0f, vid == 2 ? -3.0f : 1.0f, 0.0f, 1.0f);
  o.rate = 0x5; // D3D12_SHADING_RATE_2X2
  return o;
}
float4 ps_shadingrate(float4 pos : SV_Position, uint rate : SV_ShadingRate) : SV_Target {
  Out.Store(0, rate);
  return 0.0f.xxxx;
}

// Inline raytracing (RaytracingTier is NOT_SUPPORTED).
RaytracingAccelerationStructure Scene : register(t0);
[numthreads(1, 1, 1)]
void cs_rayquery() {
  RayQuery<RAY_FLAG_NONE> q;
  RayDesc ray;
  ray.Origin = float3(0.0f, 0.0f, -1.0f);
  ray.Direction = float3(0.0f, 0.0f, 1.0f);
  ray.TMin = 0.0f;
  ray.TMax = 10.0f;
  q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xff, ray);
  q.Proceed();
  Out.Store(0, q.CommittedStatus());
}

// Sampler feedback (SamplerFeedbackTier is NOT_SUPPORTED).
Texture2D<float4> Paired : register(t0);
SamplerState Point : register(s0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> Feedback : register(u2);
float4 ps_feedback(float4 pos : SV_Position) : SV_Target {
  Feedback.WriteSamplerFeedback(Paired, Point, pos.xy / 8.0f);
  return 0.0f.xxxx;
}

// SV_ViewID under a VIEW_INSTANCING subobject of count 2 (ViewInstancingTier is NOT_SUPPORTED).
float4 ps_viewid_vi(float4 pos : SV_Position, uint view : SV_ViewID) : SV_Target {
  return float4((float)view, 0.0f, 0.0f, 1.0f);
}

// Mesh shader stage in a pipeline state stream (MeshShaderTier is NOT_SUPPORTED).
struct MOut {
  float4 pos : SV_Position;
};
[outputtopology("triangle")] [numthreads(1, 1, 1)]
void ms_mesh(out vertices MOut verts[3], out indices uint3 tris[1]) {
  SetMeshOutputCounts(3, 1);
  verts[0].pos = float4(-1.0f, 1.0f, 0.0f, 1.0f);
  verts[1].pos = float4(3.0f, 1.0f, 0.0f, 1.0f);
  verts[2].pos = float4(-1.0f, -3.0f, 0.0f, 1.0f);
  tris[0] = uint3(0, 1, 2);
}
