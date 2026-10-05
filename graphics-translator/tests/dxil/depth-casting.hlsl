// Same uploaded depth bytes through scalar integer, floating-point and byte-vector views.
#ifdef MSAA
Texture2DMSArray<uint> Words : register(t0);
Texture2DMSArray<float> Floats : register(t1);
Texture2DMSArray<uint4> Bytes : register(t2);
Texture2DMSArray<float> Depth : register(t3);
RWByteAddressBuffer Result : register(u0);
cbuffer Paint : register(b0) { uint Layer; }
float4 vsMain(uint vertex : SV_VertexID) : SV_Position {
  return float4(vertex == 1 ? 3 : -1, vertex == 2 ? 3 : -1, 0, 1);
}
float psMain(uint sample : SV_SampleIndex) : SV_Depth {
#if BITS == 16
  const uint values[] = {1, 0x03ff, 0x7e55, 0xffff};
  return float(values[(sample + Layer) & 3]) / 65535.f;
#else
  const uint values[] = {0x3e000000, 0x3ec00000, 0x3f200000, 0x3f600000};
  return asfloat(values[(sample + Layer) & 3]);
#endif
}
[numthreads(1, 1, 1)]
void main() {
  uint w, h, layers, samples;
  Words.GetDimensions(w, h, layers, samples);
  Result.Store4(0, uint4(w, h, layers, samples));
  uint fw, fh, flayers, fsamples;
  Floats.GetDimensions(fw, fh, flayers, fsamples);
  Result.Store4(16, uint4(fw, fh, flayers, fsamples));
  uint bw, bh, blayers, bsamples;
  Bytes.GetDimensions(bw, bh, blayers, bsamples);
  Result.Store4(32, uint4(bw, bh, blayers, bsamples));
  uint cursor = 48;
  for (uint layer = 0; layer < 2; ++layer) for (uint sample = 0; sample < 4; ++sample)
    for (uint y = 0; y < h; ++y) for (uint x = 0; x < w; ++x) {
      int3 location = int3(x, y, layer);
      uint4 bytes = Bytes.Load(location, sample);
      uint packed = bytes.x | bytes.y << 8;
#if BITS == 32
      packed |= bytes.z << 16 | bytes.w << 24;
#endif
      Result.Store4(cursor, uint4(Words.Load(location, sample), asuint(Floats.Load(location, sample)),
                                  packed, asuint(Depth.Load(location, sample))));
      cursor += 16;
    }
}
#else
#if BITS == 16
typedef uint16_t RawElement;
#else
typedef uint RawElement;
#endif
Texture2DArray<RawElement> Words : register(t0);
Texture2DArray<float> Floats : register(t1);
Texture2DArray<uint4> Bytes : register(t2);
Texture2DArray<float> Depth : register(t3);
SamplerState PointSampler : register(s0);
RWByteAddressBuffer Result : register(u0);
[numthreads(1, 1, 1)]
void main() {
  uint cursor = 144;
  for (uint mip = 0; mip < 3; ++mip) {
    uint w, h, layers, levels;
    Words.GetDimensions(mip, w, h, layers, levels);
    Result.Store4(mip * 48, uint4(w, h, layers, levels));
    uint fw, fh, flayers, flevels;
    Floats.GetDimensions(mip, fw, fh, flayers, flevels);
    Result.Store4(mip * 48 + 16, uint4(fw, fh, flayers, flevels));
    uint bw, bh, blayers, blevels;
    Bytes.GetDimensions(mip, bw, bh, blayers, blevels);
    Result.Store4(mip * 48 + 32, uint4(bw, bh, blayers, blevels));
    for (uint layer = 0; layer < 2; ++layer)
      for (uint y = 0; y < h; ++y) for (uint x = 0; x < w; ++x) {
        int4 location = int4(x, y, layer, mip);
        uint4 bytes = Bytes.Load(location);
        uint packed = bytes.x | (bytes.y << 8);
#if BITS == 32
        packed |= bytes.z << 16 | bytes.w << 24;
#endif
        Result.Store4(cursor, uint4(Words.Load(location), asuint(Floats.Load(location)),
                                    packed, asuint(Depth.Load(location))));
        cursor += 16;
      }
  }
  uint firstStatus, secondStatus;
  Result.Store4(cursor, uint4(Words.GatherRaw(PointSampler, float3(.5, .5, 0), int2(0, 0), firstStatus)));
  Result.Store4(cursor + 16, uint4(Words.GatherRaw(PointSampler, float3(.5, .5, 1), int2(0, 0), secondStatus)));
  Result.Store4(cursor + 32, uint4(CheckAccessFullyMapped(firstStatus), CheckAccessFullyMapped(secondStatus),
                                 0x13579bdf, BITS));
}
#endif
