RWStructuredBuffer<uint> vertexEntries : register(u0);
RWStructuredBuffer<uint> pixelEntries : register(u1);

struct VertexInput {
  float4 position : POSITION;
  float4 distances : TEXCOORD0;
};
struct VertexOutput {
  float4 position : SV_Position;
  float2 clip0 : SV_ClipDistance0;
  float2 clip1 : SV_ClipDistance1;
  float2 cull0 : SV_CullDistance0;
};

VertexOutput vsDistances(VertexInput input) {
  uint old;
  InterlockedAdd(vertexEntries[0], 1, old);
  VertexOutput result;
  result.position = input.position;
  result.clip0 = float2(input.distances.x, 1);
  result.clip1 = float2(input.distances.y, 1);
  result.cull0 = float2(input.distances.z, 1);
  return result;
}

float4 psDiscard() : SV_Target0 {
  uint old;
  InterlockedAdd(pixelEntries[0], 1, old);
  discard;
  return float4(1, 0, 0, 1);
}

[earlydepthstencil]
float4 psEarly() : SV_Target0 {
  uint old;
  InterlockedAdd(pixelEntries[0], 1, old);
  return float4(1, 0, 0, 1);
}

struct DepthOutput { float4 color : SV_Target0; float depth : SV_Depth; };
DepthOutput psDepth() {
  uint old;
  InterlockedAdd(pixelEntries[0], 1, old);
  DepthOutput result;
  result.color = float4(1, 0, 0, 1);
  result.depth = 1;
  return result;
}

float4 psPixel() : SV_Target0 {
  uint old;
  InterlockedAdd(pixelEntries[0], 1, old);
  return float4(1, 0, 0, 1);
}

float4 psSample(uint sample : SV_SampleIndex) : SV_Target0 {
  uint old;
  InterlockedAdd(pixelEntries[sample], 1, old);
  return float4(1, 0, 0, 1);
}

struct MSVertex {
  float4 position : SV_Position;
  float2 varying : TEXCOORD0;
  centroid float2 centroidVarying : TEXCOORD1;
};

MSVertex vsMSAA(VertexInput input) {
  uint old;
  InterlockedAdd(vertexEntries[0], 1, old);
  MSVertex result;
  result.position = input.position;
  result.varying = input.distances.xy;
  result.centroidVarying = input.distances.xy;
  return result;
}

// DXIL positional-control and evaluation entries are compiled with
// -D MSAA_EVALUATE=1; Wine's D3DCompile ps_5_0 cannot parse those intrinsics.
// The sample-frequency path independently observes actual raster coverage.
float4 psMSCoverage(MSVertex input, uint sample : SV_SampleIndex) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedOr(pixelEntries[pixel], 1u << sample, old);
  InterlockedAdd(pixelEntries[32 + sample], 1, old);
#ifdef MSAA_EVALUATE
  float2 position = GetRenderTargetSamplePosition(sample);
  return float4(position + .5, (sample + .5) / 4, 1);
#else
  return float4(.125, .25, .375, 1);
#endif
}

#ifdef MSAA_EVALUATE
float4 psMSPosition8(float4 position : SV_Position, uint sample : SV_SampleIndex) : SV_Target0 {
  uint pixel = (uint)position.y * 8 + (uint)position.x;
  uint old;
  InterlockedOr(pixelEntries[pixel], 1u << sample, old);
  InterlockedAdd(pixelEntries[64 + sample], 1, old);
  return float4(GetRenderTargetSamplePosition(sample) + .5,
                (sample + .5) / GetRenderTargetSampleCount(), 1);
}
#endif

float4 psMSAtomic(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  return float4((old + 1) * .25, .5, .75, 1);
}

float4 psMSPrimitive(MSVertex input, uint primitive : SV_PrimitiveID) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1u << (primitive * 8), old);
  return float4((primitive + 1) * .25, .5, .75, 1);
}

float4 psMSDepthPosition(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  return float4(input.position.z, .5, .75, 1);
}

float4 psMSSampleDepthPosition(MSVertex input, uint sample : SV_SampleIndex) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[32 + sample], 1, old);
  return float4(input.position.z, .5, .75, 1);
}

#ifdef MSAA_EVALUATE
float4 psMSSampleInterpolatedDepthPosition(
    sample noperspective float4 position : SV_Position,
    uint sample : SV_SampleIndex) : SV_Target0 {
  uint old;
  InterlockedAdd(pixelEntries[32 + sample], 1, old);
  return float4(position.z, .5, .75, 1);
}
#endif

DepthOutput psMSCentroidOutputDepth(centroid float4 position : SV_Position) {
  uint pixel = (uint)position.y * 4 + (uint)position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  DepthOutput result;
  result.color = float4(position.z, .5, .75, 1);
  result.depth = position.z + .0625;
  return result;
}

[earlydepthstencil]
float4 psMSSampleEarlyMask(MSVertex input, uint sample : SV_SampleIndex) : SV_Target0 {
  uint old;
  InterlockedAdd(pixelEntries[32 + sample], 1, old);
  return float4(.25, .5, .75, 1);
}

[earlydepthstencil]
float4 psMSEarlyNonlinearDerivative(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  float nonlinearX = input.varying.x * input.varying.x;
  float nonlinearY = input.varying.y * input.varying.y;
  return float4(ddx_fine(nonlinearX), ddy_fine(nonlinearY), .5, 1);
}

float4 psMSDiscard(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  if ((pixel & 1) != 0) discard;
  return float4(.25, .5, .75, 1);
}

float4 psMSZeroAlpha(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  return float4(.25, .5, .75, 0);
}

[earlydepthstencil]
float4 psMSEarly(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  return float4(.25, .5, .75, 1);
}

DepthOutput psMSDepth(MSVertex input) {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  DepthOutput result;
  result.color = float4(.25, .5, .75, 1);
  result.depth = .875;
  return result;
}

float4 psMSCentroid(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  float2 derivative = float2(ddx(input.varying.x), ddy(input.varying.y));
  return float4(input.centroidVarying, derivative);
}

#ifdef MSAA_EVALUATE
float4 psMSEvaluate(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  uint sample = (pixel * 3 + 1) % GetRenderTargetSampleCount();
  int2 offset = int2((int)(pixel % 4) - 2, (int)(pixel / 4) - 2);
  float2 atSample = EvaluateAttributeAtSample(input.varying, sample);
  float2 atOffset = EvaluateAttributeSnapped(input.varying, offset);
  return float4(atSample.x, atOffset.y, GetRenderTargetSamplePosition(sample) + .5);
}

// Across the 4x4 fixture, these sixteen evaluations per pixel visit every
// pair of the 16 legal snapped offsets. Both coordinates remain dynamic.
float4 psMSAllSnaps(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  float2 sum = 0;
  for (uint i = 0; i < 16; ++i) {
    int2 offset = int2((int)((pixel + i) & 15) - 8,
                       (int)((4 * pixel + 5 * i) & 15) - 8);
    sum += EvaluateAttributeSnapped(input.varying, offset) / 16;
  }
  return float4(sum * .5, .25, 1);
}

float4 psMSEvalCentroid(MSVertex input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  float2 evaluated = EvaluateAttributeCentroid(input.varying);
  return float4(evaluated, input.centroidVarying);
}
#endif

float psMSPrepass(uint sample : SV_SampleIndex) : SV_Depth {
  return (sample & 1) ? .75 : .25;
}

float psMSCheckerboardPrepass(float4 position : SV_Position, uint sample : SV_SampleIndex) : SV_Depth {
  uint pixel = (uint)position.y * 4 + (uint)position.x;
  return (((pixel / 4) + (pixel & 3)) & 1) ? .25 : .75;
}

struct MaskedOutput { float4 color : SV_Target0; uint mask : SV_Coverage; };
MaskedOutput psMSOutputMask(MSVertex input) {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  MaskedOutput result;
  result.color = float4(.25, .5, .75, 1);
  result.mask = (pixel & 1) ? 1u : 15u;
  return result;
}

struct ArrayVertex {
  float4 position : SV_Position;
  uint layer : SV_RenderTargetArrayIndex;
  uint viewport : SV_ViewportArrayIndex;
};
ArrayVertex vsMSArray(VertexInput input) {
  uint old;
  InterlockedAdd(vertexEntries[0], 1, old);
  ArrayVertex output;
  output.position = input.position;
  output.layer = 1;
  output.viewport = 1;
  return output;
}

struct MRTOutput { float4 color : SV_Target0; uint typed : SV_Target1; };
RWStructuredBuffer<uint> mrtEntries : register(u2);
MRTOutput psMSMRT(ArrayVertex input) {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(mrtEntries[pixel], 1, old);
  MRTOutput output;
  output.color = float4(.25, .5, .75, 1);
  output.typed = (pixel + 1) * 17;
  return output;
}

struct CentroidPositionInput {
  centroid float4 position : SV_Position;
  centroid float2 varying : TEXCOORD1;
};
float4 psMSCentroidPosition(CentroidPositionInput input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 4 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  return float4(input.position.xy / 4, input.position.z, input.position.w);
}
