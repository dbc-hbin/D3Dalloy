// SM 6.2 fp32 denorm modes for owned-d3d12-sm6.cpp. The same body is compiled once per
// mode, each entry with its own flag (build-sm6.sh):
//   cs_denorm_any       cs_6_2 -denorm any       ("fp32-denorm-mode"="any")
//   cs_denorm_ftz       cs_6_2 -denorm ftz       ("fp32-denorm-mode"="ftz")
//   cs_denorm_preserve  cs_6_2 -denorm preserve  ("fp32-denorm-mode"="preserve")
// Raw holds four dwords per thread at gi * 16: a, b, c and w (a small positive denormal
// for the wave sum). Each thread writes 11 dwords to Out at gi * 48 (the 12th stays the
// sentinel).

RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);

groupshared float Shared[64];

void denormBody(uint gi) {
  const float a = asfloat(Raw.Load(gi * 16 + 0));
  const float b = asfloat(Raw.Load(gi * 16 + 4));
  const float c = asfloat(Raw.Load(gi * 16 + 8));
  const float w = asfloat(Raw.Load(gi * 16 + 12));
  const uint base = gi * 48;
  Out.Store(base + 0, asuint(a + b));
  Out.Store(base + 4, asuint(a - b));
  Out.Store(base + 8, asuint(a * b));
  Out.Store(base + 12, asuint(a / b));
  Out.Store(base + 16, asuint(mad(a, b, c)));
  Out.Store(base + 20, asuint(dot(float2(a, c), float2(b, b))));
  Out.Store(base + 24, (a < b ? 1u : 0u) | (a == b ? 2u : 0u) | (a == 0.0f ? 4u : 0u) | (a != c ? 8u : 0u) |
                       (a >= c ? 16u : 0u));
  Shared[gi] = a;
  GroupMemoryBarrierWithGroupSync();
  Out.Store(base + 28, asuint(Shared[63 - gi]));
  Out.Store(base + 32, asuint(WaveActiveSum(w)));
  Out.Store(base + 36, asuint(-a));
  Out.Store<float>(base + 40, a);
}

[numthreads(64, 1, 1)] void cs_denorm_any(uint gi : SV_GroupIndex) { denormBody(gi); }
[numthreads(64, 1, 1)] void cs_denorm_ftz(uint gi : SV_GroupIndex) { denormBody(gi); }
[numthreads(64, 1, 1)] void cs_denorm_preserve(uint gi : SV_GroupIndex) { denormBody(gi); }
