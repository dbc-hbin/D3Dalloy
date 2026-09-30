// SM 5.1 DXBC unbounded arrays (Resource Binding Tier 3 layout): an unbounded UAV array
// spanning 80 views (more than 64 UAVs in one stage), unbounded SRV arrays in spaces 1/2
// and an unbounded sampler array, all indexed per lane / per pixel with
// NonUniformResourceIndex. Oracle: graphics-translator/tests/owned-d3d12-bindless.cpp.
//
// Rebuild with Microsoft FXC (d3dcompiler_47, D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES):
//   fxc /T cs_5_1 /E cs /enable_unbounded_descriptor_tables /Fo bindless-dxbc51-cs.dxbc bindless-dxbc51.hlsl
//   fxc /T vs_5_1 /E vs /enable_unbounded_descriptor_tables /Fo bindless-dxbc51-vs.dxbc bindless-dxbc51.hlsl
//   fxc /T ps_5_1 /E ps /enable_unbounded_descriptor_tables /Fo bindless-dxbc51-ps.dxbc bindless-dxbc51.hlsl
// Wine's vkd3d-based d3dcompiler rejects unbounded arrays, so the DXBC is frozen.
RWByteAddressBuffer uavs[] : register(u0, space0);
ByteAddressBuffer srvs[] : register(t0, space1);
Texture2D<float> textures[] : register(t0, space2);
SamplerState samplers[] : register(s0, space0);
RWByteAddressBuffer result : register(u0, space3);

[numthreads(64, 1, 1)] void cs(uint i : SV_DispatchThreadID) {
  uint v = (i * 7) % 80;
  uavs[NonUniformResourceIndex(v)].InterlockedMax(0, i + 1);
  result.Store(i * 8, srvs[NonUniformResourceIndex(i % 16)].Load(0));
  result.Store(i * 8 + 4, asuint(textures[NonUniformResourceIndex(i % 4)].SampleLevel(
      samplers[NonUniformResourceIndex(i % 2)], float2(0.5, 0.5), 0)));
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 p : SV_Position) : SV_Target {
  uint x = uint(p.x), y = uint(p.y);
  float t = textures[NonUniformResourceIndex(x)].SampleLevel(samplers[NonUniformResourceIndex(y % 2)], float2(0.5, 0.5), 0);
  return float4(t, asfloat(srvs[NonUniformResourceIndex(y * 4 + x)].Load(0)), 0, 1);
}
