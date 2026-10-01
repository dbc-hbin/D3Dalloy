// Upstream DXC library-only exports and a separately compiled DXR consumer.
#if defined(LINK_HELPER)
GlobalRootSignature LinkGlobalRoot = { "DescriptorTable(SRV(t0, numDescriptors = 2), UAV(u0))" };
uint FinalValue(uint value);
float FinalValue(float value);
RWByteAddressBuffer HelperOutput : register(u0);
export uint BaseValue(uint value) {
  HelperOutput.Store(64u + value * 4u, 0xabc000u + value);
  return FinalValue(value) + 0x2300u;
}
export float BaseValue(float value) { return FinalValue(value) + 0.25f; }
#elif defined(LINK_FINAL)
export uint FinalValue(uint value) { return value * 3u + 17u; }
export float FinalValue(float value) { return value * 2.0f + 0.5f; }
export uint UnusedValue(uint value) { return value + 99u; }
#else
uint CollectionValue(uint value);
float CollectionValue(float value);
RWByteAddressBuffer Output : register(u0);
[shader("raygeneration")]
void LinkRayGen() {
  uint lane = DispatchRaysIndex().x;
  Output.Store(lane * 4u, CollectionValue(lane + 5u));
  Output.Store(16u + lane * 4u, asuint(CollectionValue(float(lane) + 2.25f)));
}
#endif
