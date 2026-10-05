// Genuine SM 6.6 producer for coupled full64/ordinary32 alias and raster-order effects.
#ifndef ROV
#define ROV 0
#endif
#ifndef TYPED
#define TYPED 0
#endif
#ifndef SAMPLE
#define SAMPLE 0
#endif
#ifndef EARLY
#define EARLY 0
#endif
#if ROV && TYPED
RasterizerOrderedBuffer<uint64_t> cells : register(u0);
#elif ROV
RasterizerOrderedStructuredBuffer<uint64_t> cells : register(u0);
#elif TYPED
RWBuffer<uint64_t> cells : register(u0);
#else
RWStructuredBuffer<uint64_t> cells : register(u0);
#endif
#if ROV
RasterizerOrderedBuffer<uint> alias32 : register(u1);
#else
RWBuffer<uint> alias32 : register(u1);
#endif
RWByteAddressBuffer records : register(u2);
cbuffer Control : register(b0) { uint sample_count; uint discard_column; uint depth_bits; uint unused; }
struct Vertex { float4 position : SV_Position; nointerpolation uint ordinal : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID, uint instance : SV_InstanceID) {
  float2 corner = float2((id << 1) & 2, id & 2);
  Vertex v;
  v.position = float4(corner * float2(2, -2) + float2(-1, 1), asfloat(depth_bits), 1);
  v.ordinal = instance;
  return v;
}
struct Result {
  float4 color : SV_Target;
#if !EARLY
  float depth : SV_Depth;
#endif
};
#if EARLY
[earlydepthstencil]
#endif
Result ps(Vertex v
#if SAMPLE
, uint sample : SV_SampleIndex
#endif
) {
#if !SAMPLE
  uint sample = 0;
#endif
  uint2 xy = uint2(v.position.xy);
  if (xy.x == discard_column) discard;
  uint pixel = (xy.y * 4 + xy.x) * sample_count + sample;
  // ROV orders the shared cell for overlapping primitives at the same pixel/sample.
  // Ordinary UAV cases use independent per-primitive cells: no guessed execution order.
  uint cell = pixel + (ROV ? 0 : v.ordinal * 16 * sample_count);
  uint64_t old;
  InterlockedAdd(cells[cell], 0x100000005ULL, old);
  uint alias_old;
  InterlockedAdd(alias32[cell * 2], 3, alias_old);
  uint64_t final_value = cells[cell];
  uint record = (v.ordinal * 16 * sample_count + pixel) * 32;
  records.Store<uint64_t>(record, old);
  records.Store<uint64_t>(record + 8, final_value);
  records.Store4(record + 16, uint4(alias_old, alias32[cell * 2], uint(old >> 32), uint(final_value >> 32)));
  Result result;
  result.color = float4(float(uint(old >> 32) - 0x12345678u) * 64 / 255, .5, float(sample * 64) / 255, 1);
#if !EARLY
  result.depth = .25;
#endif
  return result;
}
float4 probe(Vertex v) : SV_Target { return float4(0, 1, 0, 1); }
