// Exact consumed-coverage control for the captured_layout cold/hit PSOs.
cbuffer Color : register(b0) { uint4 color; };
float4 main(uint covered : SV_Coverage) : SV_Target {
  return covered != 0 ? float4(color) / 255.0 : float4(0, 0, 0, 0);
}
