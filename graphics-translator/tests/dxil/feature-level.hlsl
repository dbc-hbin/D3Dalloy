// Frozen DXIL for the feature-level suite (target-independent rasterization). Built by build-feature-level.sh.
cbuffer Tri : register(b0) { float4 p01; float4 p2; };
RWStructuredBuffer<uint2> result : register(u0);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 p = id == 0 ? p01.xy : id == 1 ? p01.zw : p2.xy;
  return float4(p, 0, 1);
}

// UAV-only forced raster: one pixel-frequency invocation; SV_Coverage is the full forced-pattern mask.
void ps_coverage(float4 pos : SV_Position, uint coverage : SV_Coverage) {
  uint index = uint(pos.y) * 16 + uint(pos.x);
  InterlockedOr(result[index].x, coverage);
  InterlockedAdd(result[index].y, 1);
}

float4 ps_color(float4 pos : SV_Position, uint coverage : SV_Coverage) : SV_Target {
  uint index = uint(pos.y) * 16 + uint(pos.x);
  InterlockedOr(result[index].x, coverage);
  InterlockedAdd(result[index].y, 1);
  return float4(0.25, 0.5, 0.75, 1);
}
