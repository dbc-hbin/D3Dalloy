// DXC -T cs_6_7 -E table / heap -HV 2021 -enable-16bit-types, normal validator.
Texture2D<uint4> UintTex : register(t0);
Texture2D<int4> SintTex : register(t1);
Texture2D<uint> RawTex : register(t2);
Texture1D<uint4> OneD : register(t3);
Texture3D<uint4> Volume : register(t4);
Texture2D<uint4> U8 : register(t5);
Texture2D<int4> S8 : register(t6);
Texture2D<uint4> U16 : register(t7);
Texture2D<int4> S16 : register(t8);
Texture2D<uint16_t> Raw16 : register(t9);
Texture2D<uint64_t> Raw64 : register(t10);
Texture2D<uint4> EmptyUint : register(t11);
Texture2D<int4> EmptySint : register(t12);
Texture2D<uint> EmptyRaw : register(t13);
SamplerState Border : register(s0);
RWByteAddressBuffer Out : register(u0);
cbuffer Params : register(b0) { uint coordinateScale; };

void consume(Texture2D<uint4> u, Texture2D<int4> s, Texture2D<uint> raw,
             Texture1D<uint4> d1, Texture3D<uint4> d3, SamplerState samplerState,
             Texture2D<uint4> u8, Texture2D<int4> s8, Texture2D<uint4> u16, Texture2D<int4> s16,
             Texture2D<uint16_t> raw16, Texture2D<uint64_t> raw64, Texture2D<uint4> emptyUint,
             Texture2D<int4> emptySint, Texture2D<uint> emptyRaw) {
  float scale = coordinateScale;
  Out.Store4(0, u.SampleLevel(samplerState, float2(-1, -1) * scale, 0));
  Out.Store4(16, u.SampleLevel(samplerState, float2(.125, .125) * scale, 0));
  Out.Store4(32, asuint(s.SampleLevel(samplerState, float2(-1, -1) * scale, 0)));
  Out.Store4(48, asuint(s.SampleLevel(samplerState, float2(.125, .125) * scale, 0)));
  Out.Store4(64, u.GatherRed(samplerState, float2(0, .25) * scale));
  Out.Store4(80, asuint(s.GatherRed(samplerState, float2(0, .25) * scale)));
  Out.Store4(96, raw.GatherRaw(samplerState, float2(0, .25) * scale));
  // V/W must not contribute to a 1D border test; only W is outside for the volume.
  Out.Store4(112, d1.SampleLevel(samplerState, -scale, 0));
  if (coordinateScale == 1)
    Out.Store4(128, d3.SampleLevel(samplerState, float3(.125, .125, 2), 0));
  else
    Out.Store4(128, d1.SampleLevel(samplerState, .125 * scale, 0));
  Out.Store4(144, u.SampleLevel(samplerState, float2(.125, .125) * scale, 0, int2(-1, 0)));
  Out.Store4(160, u.SampleLevel(samplerState, float2(.125, 1) * scale, 0));
  Out.Store4(176, u.GatherGreen(samplerState, float2(0, .25) * scale));
  float2 outside = float2(-1, -1) * scale, inside = float2(.125, .125) * scale;
  Out.Store4(192, u8.SampleLevel(samplerState, outside, 0));
  Out.Store4(208, asuint(s8.SampleLevel(samplerState, outside, 0)));
  Out.Store4(224, u16.SampleLevel(samplerState, outside, 0));
  Out.Store4(240, asuint(s16.SampleLevel(samplerState, outside, 0)));
  Out.Store4(256, u8.SampleLevel(samplerState, inside, 0));
  Out.Store4(272, asuint(s8.SampleLevel(samplerState, inside, 0)));
  Out.Store4(288, u16.SampleLevel(samplerState, inside, 0));
  Out.Store4(304, asuint(s16.SampleLevel(samplerState, inside, 0)));
  Out.Store4(320, uint4(raw16.GatherRaw(samplerState, float2(0, .25) * scale)));
  vector<uint64_t, 4> wide = raw64.GatherRaw(samplerState, float2(0, .25) * scale);
  Out.Store4(336, uint4(wide));
  Out.Store4(352, uint4(wide >> 32));
  Out.Store4(368, emptyUint.SampleLevel(samplerState, outside, 0));
  Out.Store4(384, emptyUint.SampleLevel(samplerState, inside, 0));
  Out.Store4(400, asuint(emptySint.SampleLevel(samplerState, outside, 0)));
  Out.Store4(416, emptyUint.GatherRed(samplerState, float2(0, .25) * scale));
  Out.Store4(432, emptyRaw.GatherRaw(samplerState, float2(0, .25) * scale));
}

[numthreads(1, 1, 1)] void table() {
  consume(UintTex, SintTex, RawTex, OneD, Volume, Border, U8, S8, U16, S16, Raw16, Raw64, EmptyUint, EmptySint, EmptyRaw);
}
[numthreads(1, 1, 1)] void heap() {
  Texture2D<uint4> u = ResourceDescriptorHeap[0];
  Texture2D<int4> s = ResourceDescriptorHeap[1];
  Texture2D<uint> raw = ResourceDescriptorHeap[2];
  Texture1D<uint4> d1 = ResourceDescriptorHeap[3];
  Texture3D<uint4> d3 = ResourceDescriptorHeap[4];
  SamplerState samplerState = SamplerDescriptorHeap[7];
  Texture2D<uint4> u8 = ResourceDescriptorHeap[5];
  Texture2D<int4> s8 = ResourceDescriptorHeap[6];
  Texture2D<uint4> u16 = ResourceDescriptorHeap[7];
  Texture2D<int4> s16 = ResourceDescriptorHeap[8];
  Texture2D<uint16_t> raw16 = ResourceDescriptorHeap[9];
  Texture2D<uint64_t> raw64 = ResourceDescriptorHeap[10];
  Texture2D<uint4> emptyUint = ResourceDescriptorHeap[11];
  Texture2D<int4> emptySint = ResourceDescriptorHeap[12];
  Texture2D<uint> emptyRaw = ResourceDescriptorHeap[13];
  consume(u, s, raw, d1, d3, samplerState, u8, s8, u16, s16, raw16, raw64, emptyUint, emptySint, emptyRaw);
}
