#ifndef PROGRAMMABLE_OFFSETS
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
#else
Texture2D<float> offsetTexture : register(t0);
Texture2DArray<float> offsetArray : register(t1);
Texture1DArray<float> offsetLine : register(t2);
Texture2D<uint> offsetInteger : register(t3);
Texture2D<float> offsetMipView : register(t4);
Texture2D<float> offsetOne : register(t5);
Texture2DMS<float, 4> offsetMS : register(t6);
StructuredBuffer<int4> offsets : register(t7);
SamplerState pointSampler : register(s0);
SamplerComparisonState cmpSampler : register(s1);
SamplerState wrapSampler : register(s2);
SamplerState mirrorSampler : register(s3);
SamplerState borderSampler : register(s4);
SamplerState mirrorOnceSampler : register(s5);
RWStructuredBuffer<uint4> offsetResults : register(u0);
cbuffer Batch : register(b0) { uint batch; };

float4 offsetVS(uint vertexId : SV_VertexID) : SV_Position {
  float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 offsetPS(float4 position : SV_Position) : SV_Target0 {
  uint lane = (uint)position.x;
  int2 o = offsets[batch * 16 + lane].xy;
  uint base = (batch * 16 + lane) * 22;
  float2 uv = 64.75 / float2(127, 113);
  offsetResults[base] = asuint(float4(
      offsetTexture.Sample(pointSampler, uv, o),
      offsetTexture.SampleBias(pointSampler, uv, 0.0, o),
      offsetTexture.SampleCmp(cmpSampler, uv, 0.125, o),
      offsetTexture.SampleCmpLevelZero(cmpSampler, uv, 0.125, o)));
  offsetResults[base + 1] = asuint(float4(
      offsetArray.Sample(pointSampler, float3(uv, 1), o),
      offsetArray.SampleLevel(pointSampler, float3(uv, 1), 0, o),
      offsetArray.SampleGrad(pointSampler, float3(uv, 1), float2(1.0 / 127, 0), float2(0, 1.0 / 113), o),
      offsetArray.SampleCmp(cmpSampler, float3(uv, 1), 0.375, o)));
  offsetResults[base + 2] = asuint(float4(
      offsetLine.Sample(pointSampler, float2(uv.x, 1), o.x),
      offsetLine.SampleLevel(pointSampler, float2(uv.x, 1), 0, o.x),
      offsetLine.SampleGrad(pointSampler, float2(uv.x, 1), 1.0 / 127, 0, o.x),
      offsetLine.SampleCmp(cmpSampler, float2(uv.x, 1), 16448.0 / 65536, o.x)));
  return 0;
}

[numthreads(16, 1, 1)]
void offsetCS(uint3 tid : SV_DispatchThreadID) {
  uint lane = tid.x;
  int4 o = offsets[batch * 16 + lane];
  uint base = (batch * 16 + lane) * 22;
  float2 uv = 64.75 / float2(127, 113);
  float2 gatherUV = (batch == 0 ? 64.75 : 1.75) / float2(127, 113);
  offsetResults[base + 3] = asuint(float4(
      offsetTexture.SampleLevel(pointSampler, uv, 0, o.xy),
      offsetTexture.SampleGrad(pointSampler, uv, float2(1.0 / 127, 0), float2(0, 1.0 / 113), o.xy),
      offsetTexture.SampleLevel(pointSampler, uv, 0, int2(1, -1)),
      offsetTexture.Load(int3(64, 64, 0), o.xy)));
  offsetResults[base + 4] = asuint(offsetTexture.Gather(pointSampler, gatherUV, o.zw));
  offsetResults[base + 5] = asuint(offsetTexture.GatherCmp(cmpSampler, gatherUV, 0.125, o.zw));
  offsetResults[base + 6] = offsetInteger.GatherRaw(pointSampler, gatherUV, o.zw);
  offsetResults[base + 7] = asuint(offsetArray.Gather(pointSampler, float3(gatherUV, 1), o.zw));
  offsetResults[base + 8] = asuint(offsetArray.GatherCmp(cmpSampler, float3(gatherUV, 1), 0.375, o.zw));
  offsetResults[base + 9] = asuint(float4(
      offsetTexture.SampleCmpLevel(cmpSampler, uv, 0.125, 0, o.xy),
      offsetArray.Load(int4(64, 64, 1, 0), o.xy),
      offsetLine.Load(int3(64, 1, 0), o.x),
      offsetLine.SampleLevel(pointSampler, float2(uv.x, 1), 0, 1)));
  offsetResults[base + 10] = asuint(float4(
      offsetTexture.Sample(pointSampler, uv, o.xy),
      offsetTexture.SampleBias(pointSampler, uv, 0, o.xy),
      offsetTexture.SampleCmp(cmpSampler, uv, 0.125, o.xy),
      offsetTexture.Load(int3(int2(64, 64) + o.xy, 0))));
  offsetResults[base + 11] = asuint(offsetTexture.Gather(wrapSampler, gatherUV, o.zw));
  offsetResults[base + 12] = asuint(offsetTexture.Gather(mirrorSampler, gatherUV, o.zw));
  offsetResults[base + 13] = asuint(offsetTexture.Gather(borderSampler, gatherUV, o.zw));
  offsetResults[base + 14] = asuint(offsetTexture.Gather(wrapSampler, float2(16777216, -16777216), o.zw));
  offsetResults[base + 15] = asuint(offsetTexture.Gather(mirrorSampler,
      batch == 0 ? float2(16777216, -16777216) : float2(16777215, -16777215), o.zw));
  offsetResults[base + 16] = asuint(offsetTexture.Gather(pointSampler, float2(16777216, -16777216), o.zw));
  float2 boundaryUV = (batch == 0 ? 64.484375 : 64.515625) / float2(127, 113);
  offsetResults[base + 17] = asuint(offsetTexture.Gather(pointSampler, boundaryUV, o.zw));
  offsetResults[base + 18] = uint4(
      asuint(offsetMS.Load(int2(0, 0), 0, o.xy)),
      asuint(offsetMS.Load(int2(64, 64) + o.xy, 3)),
      offsetInteger.Load(int3(64, 64, 0), o.xy),
      offsetInteger.Load(int3(int2(64, 64) + o.xy, 0)));
  offsetResults[base + 19] = asuint(offsetTexture.Gather(mirrorOnceSampler,
      float2(-1.75 / 127, 1.75 / 113), o.zw));
  offsetResults[base + 20] = asuint(offsetMipView.Gather(pointSampler, gatherUV, o.zw));
  offsetResults[base + 21] = asuint(offsetOne.Gather(wrapSampler, float2(16777216, -16777216), o.zw));
}
#endif
