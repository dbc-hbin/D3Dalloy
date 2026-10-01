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


// SV_ViewID under a VIEW_INSTANCING subobject (ViewInstancingTier 1): paired with vs_viewid, the draw replays once
// per enabled view, so the PS stores its own SV_ViewID and the VS value (attribute VIEW) per pixel at 64 + pixel * 8.
struct VViewInstanced {
  float4 pos : SV_Position;
  nointerpolation uint view : VIEW;
};
float4 ps_viewid_vi(VViewInstanced i, uint view : SV_ViewID) : SV_Target {
  const uint2 px = (uint2)i.pos.xy;
  Out.Store2(64 + (px.y * 8 + px.x) * 8, uint2(view, i.view));
  return float4((float)view, 0.0f, 0.0f, 1.0f);
}
