Texture2D<float> depthTexture : register(t0);
Texture2DMS<float, 4> multisampleTexture : register(t1);
Texture2DArray<float> depthArray : register(t2);
SamplerComparisonState comparisonSampler : register(s0);
SamplerState lodSampler : register(s1);
RWStructuredBuffer<float4> results : register(u0);

struct VertexOutput {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VertexOutput vsMain(uint vertexId : SV_VertexID) {
  VertexOutput output;
  output.uv = float2((vertexId << 1) & 2, vertexId & 2);
  output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return output;
}

float4 psMain(VertexOutput input) : SV_Target0 {
  const float2 gatherCoordinate = float2(0.375, 0.625);
  results[0] = depthTexture.GatherCmp(comparisonSampler, gatherCoordinate, 0.5, int2(1, -1));
  results[1] = depthArray.GatherCmp(comparisonSampler, float3(gatherCoordinate, 1.0), 0.5, int2(-1, 1));

  float2 lodCoordinate = input.position.xy / 64.0;
  results[2] = float4(
      depthTexture.CalculateLevelOfDetail(lodSampler, lodCoordinate),
      depthTexture.CalculateLevelOfDetailUnclamped(lodSampler, lodCoordinate),
      0.0, 0.0);

  float2 texturePosition = multisampleTexture.GetSamplePosition(2);
  float2 targetPosition = GetRenderTargetSamplePosition(1);
  results[3] = float4(texturePosition, targetPosition);
  results[4] = float4((float)GetRenderTargetSampleCount(), 0.0, 0.0, 0.0);
  return 0.0.xxxx;
}
