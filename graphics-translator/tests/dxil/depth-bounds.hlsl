// Public depth-bounds oracle: no implementation assumptions or synthesized containers.
cbuffer Args : register(b0) {
  float incomingDepth; float exportedDepth; uint packedColor; uint livePrimitives;
  uint seedSamples; uint sampleCount; uint unused0; uint unused1;
};
RWByteAddressBuffer effects : register(u0);
float4 vs(uint id : SV_VertexID) : SV_Position {
  uint vertex = id % 3;
  float z = livePrimitives ? (id / 3 == 0 ? .75 : .125) : incomingDepth;
  return float4(vertex == 1 ? 3 : -1, vertex == 2 ? -3 : 1, z, 1);
}
float4 color(uint primitive) {
  uint c = livePrimitives ? (primitive == 0 ? 0xff0000ffu : 0xffff0000u) : packedColor;
  return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24) / 255.;
}
float4 plain(uint primitive : SV_PrimitiveID) : SV_Target { return color(primitive); }
struct Output { float4 color : SV_Target; float depth : SV_Depth; };
Output seed(uint sample : SV_SampleIndex) {
  Output o; o.color = 0; o.depth = seedSamples ? .25 + .125 * sample : exportedDepth; return o;
}
void effect(float4 position, uint sample) {
  uint old;
  effects.InterlockedAdd(((uint(position.y) * 4 + uint(position.x)) * sampleCount + sample) * 4, 1, old);
}
Output late(float4 position : SV_Position, uint sample : SV_SampleIndex, uint primitive : SV_PrimitiveID) {
  effect(position, sample);
  Output o; o.color = color(primitive); o.depth = livePrimitives ? (primitive == 0 ? .75 : .125) : exportedDepth; return o;
}
[earlydepthstencil]
float4 early(float4 position : SV_Position, uint sample : SV_SampleIndex, uint primitive : SV_PrimitiveID) : SV_Target {
  effect(position, sample); return color(primitive);
}
Output lateDiscard(float4 position : SV_Position, uint sample : SV_SampleIndex) {
  effect(position, sample); discard;
  Output o; o.color = 0; o.depth = exportedDepth; return o;
}
[earlydepthstencil]
float4 earlyDiscard(float4 position : SV_Position, uint sample : SV_SampleIndex) : SV_Target {
  effect(position, sample); discard; return 0;
}
#ifdef MSAA
Texture2DMS<float4> pixels : register(t0);
#else
Texture2D<float4> pixels : register(t0);
#endif
[numthreads(4,4,1)] void read(uint3 id : SV_DispatchThreadID) {
  for (uint sample = 0; sample < sampleCount; ++sample) {
#ifdef MSAA
    float4 c = pixels.Load(id.xy, sample);
#else
    float4 c = pixels.Load(int3(id.xy, 0));
#endif
    uint4 b = uint4(round(c * 255));
    effects.Store(((id.y * 4 + id.x) * sampleCount + sample) * 4, b.x | b.y << 8 | b.z << 16 | b.w << 24);
  }
}
