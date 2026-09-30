// Stream-output fixture VS: TEXCOORD0 and TEXCOORD1 (uint2 each) share one packed output
// register (.xy / .zw), so a StartComponent of TEXCOORD1 is relative to .z.
// The embedded root signature deliberately lacks ALLOW_STREAM_OUTPUT.
RWStructuredBuffer<uint> invocations : register(u0);
struct VSOut {
  float4 position : SV_Position;
  uint2 lo : TEXCOORD0;
  uint2 hi : TEXCOORD1;
};
[RootSignature("RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), UAV(u0)")]
VSOut main(uint id : SV_VertexID, uint inst : SV_InstanceID) {
  uint previous;
  InterlockedAdd(invocations[0], 1, previous);
  VSOut o;
  o.position = float4(0.0, 0.0, 0.0, 1.0);
  o.lo = uint2(id, inst);
  o.hi = uint2(100 + id, 200 + inst);
  return o;
}
