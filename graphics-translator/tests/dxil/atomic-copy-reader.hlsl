globallycoherent RWByteAddressBuffer primary : register(u0);
globallycoherent RWByteAddressBuffer payload : register(u1);
RWByteAddressBuffer samples : register(u2);
cbuffer Base : register(b0) { uint baseIndex; };

// Bounded sampling: the high half of primary is fixed, so the low generation
// and its corresponding payload record are the only live observations. Load2
// would be two 32-bit loads, not an atomic 64-bit snapshot.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint first = 0xffffffff, last = 0, bad = 0;
  [loop] for (uint i = 0; i < 65536; ++i) {
    DeviceMemoryBarrier();
    uint g = primary.Load(0);
    if (g <= 128) {
      first = min(first, g);
      last = max(last, g);
      if (g > 0) {
        DeviceMemoryBarrier();
        if (payload.Load((g - 1) * 4) != (0x7135a9cd ^ (g * 0x9e3779b9)))
          bad++;
      }
      if (g == 128)
        break;
    } else {
      bad++;
    }
  }
  samples.Store3((baseIndex + id.x) * 12, uint3(first, last, bad + 1));
}
