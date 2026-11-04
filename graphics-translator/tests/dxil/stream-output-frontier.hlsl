// Focused VS capture oracle: original draw bases must survive every GPU cursor chunk.
// Compile vs_main as vs_6_8 with the ordinary public DXC producer.
RWStructuredBuffer<uint> invocations : register(u0);

struct VSOut {
  float4 position : SV_Position;
  uint4 values : TEXCOORD0;
  uint2 ticket : TEXCOORD2;
};

VSOut vs_main(uint id : SV_VertexID, uint instance : SV_InstanceID,
              uint originalVertex : SV_StartVertexLocation,
              uint originalInstance : SV_StartInstanceLocation) {
  uint previous;
  InterlockedAdd(invocations[0], 1, previous);
  VSOut output;
  output.position = float4(0, 0, 0, 1);
  output.values = uint4(id, instance, originalVertex, originalInstance);
  output.ticket = uint2(previous, ~previous);
  return output;
}
