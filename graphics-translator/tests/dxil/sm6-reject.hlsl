// Shader Model 6.x inputs the owned compiler must reject with a clear error
// (owned-d3d12-sm6.cpp). Compile with DXC 1.9: -HV 2021 -T <profile> -E <entry>.

RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);

// Native32 remains the public maximum; unconditional compute may request wider waves.
[WaveSize(32)] [numthreads(32, 1, 1)]
void cs_wavesize32(uint gi : SV_GroupIndex) { Out.Store(gi * 4, WaveGetLaneCount()); }
// Wide waves are admitted only when DXIL has one unconditional compute block.
void wide_values(uint gi) {
  uint lane = WaveGetLaneIndex();
  uint width = WaveGetLaneCount();
  uint live = WaveActiveCountBits(true);
  uint value = gi + 1;
  uint4 ballot = WaveActiveBallot((lane & 1) == 0);
  uint4 match = WaveMatch(lane & 3);
  uint b = gi * 128;
  Out.Store4(b, uint4(width, lane, live, WaveIsFirstLane()));
  Out.Store4(b + 16, ballot);
  Out.Store4(b + 32, uint4(WaveReadLaneAt(value, (lane + 33) % live), WaveReadLaneFirst(value),
                          WaveActiveSum(value), WavePrefixSum(value)));
  Out.Store4(b + 48, uint4(WaveActiveMin(value), WaveActiveMax(value),
                          WaveActiveCountBits((lane & 1) == 0), WavePrefixCountBits((lane & 1) == 0)));
  Out.Store4(b + 64, match);
  Out.Store4(b + 80, uint4(WaveMultiPrefixSum(value, match), WaveMultiPrefixCountBits(true, match),
                          WaveActiveAnyTrue(lane == 33), WaveActiveAllTrue(lane < live)));
  Out.Store4(b + 96, uint4(WaveActiveAllEqual(value), WaveActiveAllEqual(width),
                          WaveActiveBitOr(1u << (lane & 31)), WaveActiveBitXor(value)));
  Out.Store(b + 112, asuint(WaveActiveSum((float)value)));
}
[WaveSize(64)] [numthreads(128, 1, 1)]
void cs_wavesize64(uint gi : SV_GroupIndex) { wide_values(gi); }
[WaveSize(128)] [numthreads(256, 1, 1)]
void cs_wavesize128(uint gi : SV_GroupIndex) { wide_values(gi); }
[WaveSize(64, 128)] [numthreads(128, 1, 1)]
void cs_wavesize_range64only(uint gi : SV_GroupIndex) { wide_values(gi); }
[WaveSize(64)] [numthreads(160, 1, 1)]
void cs_wavesize64_partial(uint gi : SV_GroupIndex) { wide_values(gi); }
[WaveSize(128)] [numthreads(288, 1, 1)]
void cs_wavesize128_partial(uint gi : SV_GroupIndex) { wide_values(gi); }
[WaveSize(64)] [numthreads(64, 1, 1)]
void cs_wavesize64_divergent(uint gi : SV_GroupIndex) {
  if ((gi & 1) != 0) Out.Store(gi * 4, WaveActiveSum(gi));
}
[WaveSize(128)] [numthreads(128, 1, 1)]
void cs_wavesize128_divergent(uint gi : SV_GroupIndex) {
  if ((gi & 1) != 0) Out.Store(gi * 4, WaveActiveSum(gi));
}
[WaveSize(64)] [numthreads(64, 1, 1)]
void cs_wavesize64_return(uint gi : SV_GroupIndex) {
  if (gi == 0) return;
  Out.Store(gi * 4, WaveActiveSum(gi));
}



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
