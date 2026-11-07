// ExecuteIndirect(DISPATCH_RAYS) with a root-argument-changing command signature, for
// owned-d3d12-raytracing-rootargs.inc. Frozen by build-raytracing-rootargs.sh.
// Global root signature: b0 = 2 root constants (k.x = slot tag, k.y = base), u0 = root UAV.

cbuffer Slot : register(b0) {
  uint2 k;
};
RWByteAddressBuffer output : register(u0);

#ifdef TRANSITION_COMPUTE
[numthreads(1, 1, 1)]
void TransitionCompute() {
  uint previous;
  output.InterlockedAdd(4, 1, previous);
}
#else
[shader("raygeneration")]
void RARayGen() {
  const uint lane = DispatchRaysIndex().x;
  // Each slot writes its own 16-word row with its own constants: a dispatch that saw another
  // slot's root arguments lands in the wrong row or with the wrong tag.
  output.Store((k.x * 16 + lane) * 4, (k.x << 16) | (k.y + lane));
}

// Independent atomic counters allow C -> RT -> C without a UAV dependency or barrier.
[shader("raygeneration")]
void TransitionRayGen() {
  uint previous;
  output.InterlockedAdd(0, 1, previous);
}

// A complete collection must resolve s0 from its local root, even though the
// application global root contains only t0/u0. Clamp and wrap differ in lane 1.
Texture2D<float> collectionTexture : register(t0);
SamplerState collectionSampler : register(s0);

[shader("raygeneration")]
void CollectionSamplerRayGen() {
  const uint lane = DispatchRaysIndex().x;
  output.Store(lane * 4, asuint(collectionTexture.SampleLevel(
      collectionSampler, float2(lane == 0 ? 0.25 : 1.25, 0.5), 0)));
}
#endif
