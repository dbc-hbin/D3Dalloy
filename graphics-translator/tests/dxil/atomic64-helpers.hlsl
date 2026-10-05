RWByteAddressBuffer output : register(u0);
RWStructuredBuffer<uint64_t> cells : register(u1);
ByteAddressBuffer source : register(t0);
cbuffer Control : register(b0) { uint samples; uint discard_column; uint2 unused; }
struct Vertex { float4 position : SV_Position; noperspective float2 varying : TEXCOORD0; };
Vertex vs_helpers(uint id : SV_VertexID) {
  float2 corner = float2((id << 1) & 2, id & 2);
  Vertex v;
  v.position = float4(corner * float2(2, -2) + float2(-1, 1), .5, 1);
  v.varying = corner * float2(7, 5);
  return v;
}
float4 record_helpers(Vertex v, uint sample, uint coverage, float2 evaluated) {
  uint2 xy = uint2(v.position.xy);
  // Upload SRV includes the complete 8x8 derivative tile, not only the 7x5 RTV.
  float read = float(source.Load((xy.y * 8 + xy.x) * 4));
  float dx = ddx_fine(read), dy = ddy_fine(read);
  float coarse_dx = ddx_coarse(read), coarse_dy = ddy_coarse(read);
  uint peer = QuadReadAcrossX(asuint(read));
  uint peer_y = QuadReadAcrossY(asuint(read));
  uint peer_diagonal = QuadReadAcrossDiagonal(asuint(read));
  uint helper_peer = QuadReadAcrossX(uint(IsHelperLane()));
  uint active = WaveActiveCountBits(true);
  if (xy.x == discard_column) discard;
  uint record = (xy.y * 8 + xy.x) * samples + sample;
  uint64_t old;
  InterlockedAdd(cells[record], 1ULL, old);
  uint base = record * 96;
  output.Store<uint64_t>(base, old);
  output.Store4(base + 8, uint4(asuint(dx), asuint(dy), asuint(coarse_dx), asuint(coarse_dy)));
  output.Store4(base + 24, uint4(peer, helper_peer, active, uint(IsHelperLane())));
  output.Store4(base + 40, uint4(asuint(v.position.x), asuint(v.position.y), coverage, sample));
  output.Store(base + 56, asuint(v.varying.x));
  output.Store(base + 60, WaveActiveCountBits(IsHelperLane()));
  float2 sample_position = GetRenderTargetSamplePosition(sample);
  output.Store4(base + 64, uint4(GetRenderTargetSampleCount(), asuint(sample_position.x),
      asuint(sample_position.y), asuint(v.varying.y)));
  output.Store4(base + 80, uint4(asuint(evaluated.x), asuint(evaluated.y), peer_y, peer_diagonal));
  return float4(.25, .5, .75, 1);
}
float4 ps_helpers(Vertex v, uint coverage : SV_Coverage) : SV_Target {
  return record_helpers(v, 0, coverage, EvaluateAttributeAtSample(v.varying, 0));
}
float4 ps_helpers_sample(Vertex v, uint sample : SV_SampleIndex, uint coverage : SV_Coverage) : SV_Target {
  return record_helpers(v, sample, coverage, EvaluateAttributeAtSample(v.varying, sample));
}
[WaveOpsIncludeHelperLanes]
float4 ps_helpers_inclusive(Vertex v, uint coverage : SV_Coverage) : SV_Target {
  return record_helpers(v, 0, coverage, EvaluateAttributeAtSample(v.varying, 0));
}
[earlydepthstencil]
float4 ps_helpers_early(Vertex v, uint coverage : SV_Coverage) : SV_Target {
  return record_helpers(v, 0, coverage, EvaluateAttributeAtSample(v.varying, 0));
}
