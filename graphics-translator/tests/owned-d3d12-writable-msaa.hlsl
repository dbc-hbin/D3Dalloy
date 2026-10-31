#ifndef SAMPLE_COUNT
#define SAMPLE_COUNT 4
#endif
#if defined(MSAA_FLOAT)
#define MSAA_VALUE float4
#elif defined(MSAA_SINT)
#define MSAA_VALUE int4
#else
#define MSAA_VALUE uint4
#endif
#ifdef SINGLE_MSAA
#define RW_MSAA RWTexture2DMS<MSAA_VALUE, SAMPLE_COUNT>
#define RO_MSAA Texture2DMS<MSAA_VALUE, SAMPLE_COUNT>
#define LOCATION(p) (p).xy
#else
#define RW_MSAA RWTexture2DMSArray<MSAA_VALUE, SAMPLE_COUNT>
#define RO_MSAA Texture2DMSArray<MSAA_VALUE, SAMPLE_COUNT>
#define LOCATION(p) (p)
#endif
globallycoherent RW_MSAA Target : register(u0);
RWByteAddressBuffer Output : register(u1);
RW_MSAA Alias : register(u2);
RO_MSAA Srv : register(t0);
cbuffer Params : register(b0) { uint Base; uint Layer; uint Layers; uint Unused; }
MSAA_VALUE pattern(uint3 p, uint s) {
  uint x = Base + p.x + 4 * p.y + 16 * p.z + 256 * s;
#ifdef MSAA_FLOAT
  return float4(x, x + 5, x + 17, x + 19) / 8.0f;
#elif defined(MSAA_RAW16)
  return uint4(x, x + 1, 0x7c01 + s, 0x8000);
#else
  return MSAA_VALUE(x, x ^ 0x80000000u, x + 17, 0x12345678u + s);
#endif
}
uint address(uint3 p, uint s) { return ((p.z * 16 + p.y * 4 + p.x) * SAMPLE_COUNT + s) * 32; }
[numthreads(4, 4, 1)]
void cs_msaa_store(uint3 p : SV_DispatchThreadID) {
  for (uint s = 0; s < SAMPLE_COUNT; ++s) Target.sample[s][LOCATION(p)] = pattern(p, s);
  DeviceMemoryBarrier();
  for (uint s = 0; s < SAMPLE_COUNT; ++s) {
    Output.Store4(address(p, s), asuint(Target.Load(LOCATION(p), s)));
    Output.Store4(address(p, s) + 16, asuint(Alias.Load(LOCATION(p), s)));
  }
}
[numthreads(4, 4, 1)]
void cs_msaa_read(uint3 p : SV_DispatchThreadID) {
  for (uint s = 0; s < SAMPLE_COUNT; ++s) {
    Output.Store4(address(p, s), asuint(Target.Load(LOCATION(p), s)));
    Output.Store4(address(p, s) + 16, asuint(Alias.Load(LOCATION(p), s)));
  }
  if (all(p == 0)) {
    uint w, h, a = 1, n;
#ifdef SINGLE_MSAA
    Target.GetDimensions(w, h, n);
#else
    Target.GetDimensions(w, h, a, n);
#endif
    Output.Store4(Layers * 16 * SAMPLE_COUNT * 32, uint4(w, h, a, n));
  }
}
[numthreads(1, 1, 1)]
void cs_msaa_unused() {
  Output.Store(0, Base);
}
[numthreads(4, 4, 1)]
void cs_msaa_heap(uint3 p : SV_DispatchThreadID) {
  RW_MSAA target = ResourceDescriptorHeap[0];
  RWByteAddressBuffer output = ResourceDescriptorHeap[1];
  RW_MSAA alias = ResourceDescriptorHeap[2];
  for (uint s = 0; s < SAMPLE_COUNT; ++s) target.sample[s][LOCATION(p)] = pattern(p, s);
  DeviceMemoryBarrier();
  for (uint s = 0; s < SAMPLE_COUNT; ++s) {
    output.Store4(address(p, s), asuint(target.Load(LOCATION(p), s)));
    output.Store4(address(p, s) + 16, asuint(alias.Load(LOCATION(p), s)));
  }
}
[numthreads(4, 4, 1)]
void cs_msaa_srv(uint3 p : SV_DispatchThreadID) {
  for (uint s = 0; s < SAMPLE_COUNT; ++s) {
    uint4 v = asuint(Srv.Load(LOCATION(p), s));
    Output.Store4(address(p, s), v);
    Output.Store4(address(p, s) + 16, v);
  }
}
[numthreads(4, 4, 1)]
void cs_msaa_oob(uint3 p : SV_DispatchThreadID) {
  Target.sample[SAMPLE_COUNT + Unused][LOCATION(p)] = 0;
  Target.sample[0][LOCATION(uint3(4, p.y, p.z))] = 0;
  Target.sample[0][LOCATION(uint3(p.x, 4, p.z))] = 0;
#ifndef SINGLE_MSAA
  Target.sample[0][uint3(p.xy, Layers)] = 0;
#endif
  DeviceMemoryBarrier();
  for (uint s = 0; s < SAMPLE_COUNT; ++s) {
    Output.Store4(address(p, s), asuint(Target.Load(LOCATION(p), s)));
    Output.Store4(address(p, s) + 16, asuint(Alias.Load(LOCATION(p), s)));
  }
}
float4 vs_msaa(uint id : SV_VertexID) : SV_Position {
  float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
  return float4(positions[id], 0, 1);
}
MSAA_VALUE ps_msaa(float4 p : SV_Position, uint s : SV_SampleIndex) : SV_Target0 {
  return pattern(uint3(uint2(p.xy), Layer), s);
}
