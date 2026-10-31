// DXC 1.9.2609.5: -T vs_6_0 -E sampleInterpolationVS / -T ps_6_0 -E sampleInterpolation.
// Wine's D3DCompile frontend does not accept the sample interpolation modifier.
struct SampleOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
struct Output { float4 color : SV_Target0; float depth : SV_Depth; };
SampleOut sampleInterpolationVS(uint id : SV_VertexID) {
  SampleOut o;
  o.position = float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, .375, 1);
  o.uv = float2((o.position.x + 1) * .25, (1 - o.position.y) * .25);
  return o;
}
Output sampleInterpolation(sample float2 uv : TEXCOORD0) {
  Output o;
  o.color = float4(uv, .5, 1);
  o.depth = .125 + uv.x * .5 + uv.y * .25;
  return o;
}
