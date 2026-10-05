// Genuine SM6.6 non-ROV fragment contention: returning add versus a real CAS loop.
RWStructuredBuffer<uint64_t> cells : register(u0);
RWByteAddressBuffer records : register(u2);
cbuffer Control : register(b0) { uint target_cells; uint prefix; uint unused0; uint unused1; }
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 corner = float2((id << 1) & 2, id & 2);
  return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target {
  uint2 xy = uint2(position.xy);
  uint pixel = xy.y * 64 + xy.x;
  uint cell = prefix + pixel % target_cells;
  const uint64_t delta = 0x100000001ULL;
  uint64_t before;
#if CAS
  uint64_t observed;
  InterlockedOr(cells[cell], uint64_t(0), observed);
  // No attempt cap: only a successful atomic comparison can finish the add.
  [allow_uav_condition]
  for (;;) {
    uint64_t previous;
    InterlockedCompareExchange(cells[cell], observed, observed + delta, previous);
    if (previous == observed) { before = previous; break; }
    observed = previous;
  }
#else
  InterlockedAdd(cells[cell], delta, before);
#endif
  records.Store<uint64_t>(32 + pixel * 8, before);
  return float4(64.0 / 255.0, 128.0 / 255.0, 192.0 / 255.0, 1);
}
