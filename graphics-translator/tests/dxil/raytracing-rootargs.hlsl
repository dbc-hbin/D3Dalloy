// ExecuteIndirect(DISPATCH_RAYS) with a root-argument-changing command signature, for
// owned-d3d12-raytracing-rootargs.inc. Frozen by build-raytracing-rootargs.sh.
// Global root signature: b0 = 2 root constants (k.x = slot tag, k.y = base), u0 = root UAV.

cbuffer Slot : register(b0) {
  uint2 k;
};
RWByteAddressBuffer output : register(u0);

[shader("raygeneration")]
void RARayGen() {
  const uint lane = DispatchRaysIndex().x;
  // Each slot writes its own 16-word row with its own constants: a dispatch that saw another
  // slot's root arguments lands in the wrong row or with the wrong tag.
  output.Store((k.x * 16 + lane) * 4, (k.x << 16) | (k.y + lane));
}
