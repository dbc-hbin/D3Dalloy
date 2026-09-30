// Reconstructed from the frozen dxil-ia VS DXIL input/output signatures and
// instructions. POSITION and TEXCOORD are per-vertex; COLOR1 is per-instance.
// Rebuild: dxc -T vs_6_0 -E vs_main -HV 2021 -O3 -Fo dxil-ia-vs.dxil dxil-ia-vs.hlsl
struct Input {
  float2 position : POSITION;
  float2 uv : TEXCOORD0;
  float4 color : COLOR1;
  uint vertex : SV_VertexID;
  uint instance : SV_InstanceID;
};
struct Output {
  float4 position : SV_Position;
  float4 color : COLOR0;
};
Output vs_main(Input input) {
  Output output;
  output.position = float4(input.position, 0.0, 1.0);
  output.color = input.color + float4(input.uv, float(input.vertex) * 0.01,
                                      float(input.instance) * 0.01);
  return output;
}
