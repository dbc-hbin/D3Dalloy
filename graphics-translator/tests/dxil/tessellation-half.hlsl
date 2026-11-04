#include "tessellation.hlsl"

struct HalfInput {
  float4 position : POSITION;
  float16_t4 value : HALFVALUE;
};
// A producer may export an additional native-half semantic that the hull shader does not read.
struct HalfVertex {
  float4 position : POSITION;
  float16_t4 value : HALFVALUE;
  float16_t spare : UNUSED_HALF;
};
struct HalfOutput {
  float4 position : POSITION;
  uint valid : CHECK;
};
struct HalfConstants {
  float edge[3] : SV_TessFactor;
  float inside : SV_InsideTessFactor;
  uint valid : CHECK;
};
struct HalfPixel {
  float4 position : SV_Position;
  nointerpolation uint valid : CHECK;
};

HalfVertex vs_half(float2 position : POSITION, uint id : SV_VertexID) {
  HalfVertex o;
  o.position = float4(position, 0, 1);
  uint4 bits = (asuint(edges.x) + id * 4 + uint4(0, 1, 2, 3)) & 0xffff;
  o.value = asfloat16(uint16_t4(bits));
  o.spare = o.value.x;
  return o;
}
uint check_half(HalfInput input, uint id) {
  uint4 expected = (asuint(edges.x) + id * 4 + uint4(0, 1, 2, 3)) & 0xffff;
  uint4 actual = uint4(asuint16(input.value));
  // DXIL allows NaN payload canonicalization, but finite bits and signed zero
  // must survive an interstage transfer without arithmetic.
  return all((expected == actual) | (((expected & 0x7fff) > 0x7c00) & ((actual & 0x7fff) > 0x7c00)));
}
HalfConstants half_independent(InputPatch<HalfInput, 3> input) {
  HalfConstants o;
  o.edge[0] = o.edge[1] = o.edge[2] = o.inside = 1;
  o.valid = check_half(input[0], 0) & check_half(input[1], 1) & check_half(input[2], 2);
  return o;
}
HalfConstants half_dependent(InputPatch<HalfInput, 3> input, OutputPatch<HalfOutput, 3> output) {
  HalfConstants o = half_independent(input);
  o.valid &= output[0].valid & output[1].valid & output[2].valid;
  return o;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("half_independent")]
HalfOutput hs_half_independent(InputPatch<HalfInput, 3> input, uint id : SV_OutputControlPointID) {
  HalfOutput o;
  o.position = input[id].position;
  o.valid = check_half(input[id], id) & check_half(input[(id + 1) % 3], (id + 1) % 3);
  return o;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("half_dependent")]
HalfOutput hs_half_dependent(InputPatch<HalfInput, 3> input, uint id : SV_OutputControlPointID) {
  HalfOutput o;
  o.position = input[id].position;
  o.valid = check_half(input[id], id) & check_half(input[(id + 1) % 3], (id + 1) % 3);
  return o;
}
[domain("tri")]
HalfPixel ds_half(HalfConstants c, float3 bary : SV_DomainLocation, OutputPatch<HalfOutput, 3> input) {
  log_point(bary.xy);
  HalfPixel o;
  o.position = bary.x * input[0].position + bary.y * input[1].position + bary.z * input[2].position;
  o.valid = c.valid & input[0].valid & input[1].valid & input[2].valid;
  return o;
}
float4 ps_half(HalfPixel input) : SV_Target {
  return input.valid ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
}
