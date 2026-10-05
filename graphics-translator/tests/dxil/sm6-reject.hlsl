// Shader Model 6.x inputs the owned compiler must reject with a clear error
// (owned-d3d12-sm6.cpp). Compile with DXC 1.9: -HV 2021 -T <profile> -E <entry>.

RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);

// Virtual waves 4/8/16 and native32 are supported; cross-SIMD64/128 must reject.
[WaveSize(32)] [numthreads(32, 1, 1)]
void cs_wavesize32(uint gi : SV_GroupIndex) { Out.Store(gi * 4, WaveGetLaneCount()); }
[WaveSize(64)] [numthreads(64, 1, 1)]
void cs_wavesize64(uint gi : SV_GroupIndex) { Out.Store(gi * 4, WaveGetLaneCount()); }
[WaveSize(128)] [numthreads(128, 1, 1)]
void cs_wavesize128(uint gi : SV_GroupIndex) { Out.Store(gi * 4, WaveGetLaneCount()); }
[WaveSize(64, 128)] [numthreads(128, 1, 1)]
void cs_wavesize_range64only(uint gi : SV_GroupIndex) { Out.Store(gi * 4, WaveGetLaneCount()); }


// Positive groupshared64 input: maintained here alongside other historical rejection fixtures.
groupshared uint64_t Shared64;
[numthreads(1, 1, 1)]
void cs_gs64() { Shared64=0; InterlockedAdd(Shared64, 1); Out.Store<uint64_t>(0, Shared64); }

// SM6.9 input is accepted independently of the device's conservative SM6.5 advertisement.
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
