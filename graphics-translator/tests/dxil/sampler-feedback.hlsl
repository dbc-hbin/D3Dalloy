// Sampler feedback fixture shaders (owned-d3d12-sampler-feedback.cpp), frozen by build-sampler-feedback.sh.
// KIND selects the feedback map type (SAMPLER_FEEDBACK_MIN_MIP or SAMPLER_FEEDBACK_MIP_REGION_USED);
// ARRAY selects FeedbackTexture2DArray / Texture2DArray.
#ifndef KIND
#define KIND SAMPLER_FEEDBACK_MIN_MIP
#endif

cbuffer Constants : register(b0) {
  float2 scale;   // PS: uv = SV_Position.xy * scale + offset
  float2 offset;
  float bias;     // PS bias
  float lclamp;   // PS clamp (WriteSamplerFeedback with clamp)
  uint count;     // CS: number of parameters
  uint layer;     // PS: array slice
};

struct Param {
  float4 uvl;    // u, v, layer, level (Level op)
  float4 grad;   // dudx, dvdx, dudy, dvdy
  float clampv;  // Grad op clamp
  uint op;       // 0 Grad, 1 Grad + clamp, 2 Level
  uint2 pad;
};

SamplerState samp : register(s0);
StructuredBuffer<Param> params : register(t1);

#ifdef ARRAY
Texture2DArray<float4> tex : register(t0);
FeedbackTexture2DArray<KIND> feedback : register(u0);
#define LOC(uv, l) float3(uv, l)
#else
Texture2D<float4> tex : register(t0);
FeedbackTexture2D<KIND> feedback : register(u0);
#define LOC(uv, l) (uv)
#endif

[numthreads(64, 1, 1)]
void cs_main(uint id : SV_DispatchThreadID) {
  if (id >= count)
    return;
  const Param p = params[id];
  if (p.op == 2) {
    feedback.WriteSamplerFeedbackLevel(tex, samp, LOC(p.uvl.xy, p.uvl.z), p.uvl.w);
  } else if (p.op == 1) {
    feedback.WriteSamplerFeedbackGrad(tex, samp, LOC(p.uvl.xy, p.uvl.z), p.grad.xy, p.grad.zw, p.clampv);
  } else {
    feedback.WriteSamplerFeedbackGrad(tex, samp, LOC(p.uvl.xy, p.uvl.z), p.grad.xy, p.grad.zw);
  }
}

float4 vs_main(uint id : SV_VertexID) : SV_Position {
  const float2 xy = float2((id << 1) & 2, id & 2);
  return float4(xy * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps_implicit(float4 pos : SV_Position) : SV_Target {
  feedback.WriteSamplerFeedback(tex, samp, LOC(pos.xy * scale + offset, layer));
  return 0;
}

float4 ps_clamp(float4 pos : SV_Position) : SV_Target {
  feedback.WriteSamplerFeedback(tex, samp, LOC(pos.xy * scale + offset, layer), lclamp);
  return 0;
}

float4 ps_bias(float4 pos : SV_Position) : SV_Target {
  feedback.WriteSamplerFeedbackBias(tex, samp, LOC(pos.xy * scale + offset, layer), bias);
  return 0;
}
