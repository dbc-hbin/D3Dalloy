// Separate DXIL libraries exercise the state object's public function namespace.
struct HelperRecord { uint value; uint addend; };
#ifdef WG_LINK_HELPERS
RWByteAddressBuffer linkedResult : register(u3);
export uint RawValue(uint value) { return value * 7 + 2; }
export HelperRecord RawRecord(HelperRecord record) {
  record.value = record.value * 5 + 3;
  record.addend = record.addend * 2 + 1;
  return record;
}
export void WriteValue(uint value) {
  uint ignored;
  linkedResult.InterlockedAdd(0, value + 11, ignored);
  linkedResult.InterlockedAdd(4, 1, ignored);
}
#else
uint PublicValue(uint value);
HelperRecord PublicRecord(HelperRecord record);
void WriteValue(uint value);
RWByteAddressBuffer resultA : register(u0);
RWByteAddressBuffer resultB : register(u1);
cbuffer Bias : register(b0) { uint bias; }
struct Input { uint value; };
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
[NodeID("linkA")]
void LinkA(ThreadNodeInputRecord<Input> input) {
  uint ignored;
  HelperRecord record = { input.Get().value, input.Get().value + 4 };
  record = PublicRecord(record);
  resultA.InterlockedAdd(0, PublicValue(input.Get().value) + record.value + record.addend, ignored);
  resultA.InterlockedAdd(4, 1, ignored);
  WriteValue(input.Get().value);
}
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
[NodeID("linkB")]
void LinkB(ThreadNodeInputRecord<Input> input) {
  uint ignored;
  HelperRecord record = { input.Get().value, input.Get().value + 4 };
  record = PublicRecord(record);
  resultB.InterlockedAdd(0, PublicValue(input.Get().value) + record.value + record.addend + bias, ignored);
  resultB.InterlockedAdd(4, 1, ignored);
  WriteValue(input.Get().value);
}
Texture2D<float> sampleTexture : register(t0);
SamplerState localSampler : register(s0);
#ifdef WG_LINK_ROOT_SUBOBJECTS
GlobalRootSignature SampleGlobal = { "DescriptorTable(SRV(t0)), UAV(u0)" };
#endif
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
[NodeID("sampleLocal")]
void SampleLocal(ThreadNodeInputRecord<Input> input) {
  uint ignored;
  resultA.InterlockedAdd(0, (uint)sampleTexture.SampleLevel(localSampler, float2(0.5, 0.5), 0), ignored);
  resultA.InterlockedAdd(4, 1, ignored);
}
#endif
