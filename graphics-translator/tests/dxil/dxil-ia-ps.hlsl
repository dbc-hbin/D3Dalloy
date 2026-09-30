// Reconstructed from the frozen dxil-ia PS DXIL: interpolate and return COLOR0.
// Rebuild: dxc -T ps_6_0 -E ps_main -HV 2021 -O3 -Fo dxil-ia-ps.dxil dxil-ia-ps.hlsl
struct Input {
  float4 position : SV_Position;
  float4 color : COLOR0;
};
float4 ps_main(Input input) : SV_Target0 {
  return input.color;
}
