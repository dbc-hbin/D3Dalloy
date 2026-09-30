// Shader Model 6.2+ native 16 bit fixture for owned-d3d12-sm6.cpp. Compile with
// DXC 1.9: -HV 2021 -enable-16bit-types -T <profile> -E <entry>.


RWByteAddressBuffer Out : register(u0);
RWByteAddressBuffer Raw : register(u1);
RWStructuredBuffer<int64_t> S64 : register(u2);
cbuffer Params : register(b0) {
  uint4 p0; // x: divergence modulus, y: seed
};

// ---------------------------------------------------------------------------
// cs_16bit (cs_6_2, -enable-16bit-types): native half/int16 arithmetic,
// conversions, 16 bit raw buffer loads/stores and 16 bit cbuffer reads.
// ---------------------------------------------------------------------------
cbuffer Params16 : register(b1) {
  float16_t4 ch;
  int16_t4 ci;
  uint16_t4 cu;
};
[numthreads(64, 1, 1)]
void cs_16bit(uint gi : SV_GroupIndex) {
  const uint base = gi * 32;
  float16_t h = Raw.Load<float16_t>(gi * 2);
  int16_t i16 = Raw.Load<int16_t>(128 + gi * 2);
  uint16_t u16 = Raw.Load<uint16_t>(256 + gi * 2);
  float16_t r0 = h * ch.x + ch.y;           // mad in half
  float16_t r1 = max(h, ch.z) - min(h, ch.w);
  int16_t r2 = i16 * ci.x + (i16 >> 3) + ci.y;
  uint16_t r3 = (u16 >> (gi & 15)) ^ (u16 << 3) ^ cu.x;
  uint16_t r4 = u16 / (cu.y | 1) + u16 % (cu.y | 1);
  Out.Store<float16_t>(base + 0, r0);
  Out.Store<float16_t>(base + 2, r1);
  Out.Store<int16_t>(base + 4, r2);
  Out.Store<uint16_t>(base + 6, r3);
  Out.Store<uint16_t>(base + 8, r4);
  Out.Store<float16_t>(base + 10, (float16_t)(float)i16);   // int -> half
  Out.Store<int16_t>(base + 12, (int16_t)h);                  // half -> int (trunc)
  Out.Store<float>(base + 16, (float)h);                     // exact widen
  Out.Store<float16_t>(base + 20, (float16_t)((float)gi * 1100.25f)); // round to nearest even / inf
  Out.Store<uint16_t>(base + 22, asuint16(h));
  Out.Store<float16_t>(base + 24, WaveActiveSum((float16_t)(gi & 7) * (float16_t)0.5));
  Out.Store<int16_t>(base + 26, WaveActiveMin(i16));
  Out.Store<uint16_t>(base + 28, WaveActiveMax(u16));
  Out.Store<uint16_t>(base + 30, WaveReadLaneAt(u16, 3));
}

// ---------------------------------------------------------------------------
// cs_packed (cs_6_6, -enable-16bit-types): Dot2AddHalf, Dot4Add{I8,U8}Packed,
// pack/unpack 4x8 with clamping and sign extension.
// ---------------------------------------------------------------------------
[numthreads(64, 1, 1)]
void cs_packed(uint gi : SV_GroupIndex) {
  const uint base = gi * 48;
  uint a = Raw.Load(gi * 4);
  uint b = Raw.Load(256 + gi * 4);
  float16_t2 ha = float16_t2(Raw.Load<float16_t>(512 + gi * 4), Raw.Load<float16_t>(514 + gi * 4));
  float16_t2 hb = float16_t2(Raw.Load<float16_t>(768 + gi * 4), Raw.Load<float16_t>(770 + gi * 4));
  float acc = asfloat(Raw.Load(1024 + gi * 4));
  Out.Store(base + 0, asuint(dot2add(ha, hb, acc)));
  Out.Store(base + 4, (uint)dot4add_i8packed(a, b, (int)(gi * 1000) - 30000));
  Out.Store(base + 8, dot4add_u8packed(a, b, gi * 77777u));
  int16_t4 s4 = unpack_s8s16((int8_t4_packed)a);
  uint16_t4 u4 = unpack_u8u16((uint8_t4_packed)a);
  int32_t4 s32 = unpack_s8s32((int8_t4_packed)b);
  uint32_t4 u32 = unpack_u8u32((uint8_t4_packed)b);
  Out.Store(base + 12, (uint)(int)s4.x ^ ((uint)(int)s4.w << 16));
  Out.Store(base + 16, (uint)u4.y | ((uint)u4.z << 16));
  Out.Store(base + 20, (uint)s32.x + (uint)s32.y * 3 + (uint)s32.z * 5 + (uint)s32.w * 7);
  Out.Store(base + 24, u32.x + u32.y * 3 + u32.z * 5 + u32.w * 7);
  int32_t4 wide = int32_t4((int)a >> 20, (int)(a << 7) >> 22, (int)b >> 23, (int)gi - 32);
  Out.Store(base + 28, (uint)pack_u8(uint32_t4(wide)));
  Out.Store(base + 32, (uint)pack_s8(wide));
  Out.Store(base + 36, (uint)pack_clamp_u8(wide));
  Out.Store(base + 40, (uint)pack_clamp_s8(wide));
  Out.Store(base + 44, (uint)pack_clamp_s8(int16_t4((int16_t)(a & 0xffff), (int16_t)-300, (int16_t)127, (int16_t)-128)));
}


// ---------------------------------------------------------------------------
// vs_16bit / ps_16bit (6.2, -enable-16bit-types): 16 bit stage I/O. 8x8 target;
// each pixel writes three 16 bit values at (py * 8 + px) * 8.
// ---------------------------------------------------------------------------
struct V16 {
  float4 pos : SV_Position;
  nointerpolation uint16_t2 id : ID16;
  float16_t2 val : VAL16;
};
V16 vs_16bit(uint vid : SV_VertexID) {
  V16 o;
  const uint corner = vid % 3;
  o.pos = float4(corner == 1 ? 3.0f : -1.0f, corner == 2 ? -3.0f : 1.0f, 0.0f, 1.0f);
  o.id = uint16_t2((uint16_t)(vid + 40), (uint16_t)0xfffe);
  o.val = float16_t2(0.5, -3.0);
  return o;
}
float4 ps_16bit(V16 i) : SV_Target {
  const uint2 px = (uint2)i.pos.xy;
  const uint base = (px.y * 8 + px.x) * 8;
  Out.Store<uint16_t>(base + 0, i.id.y);
  Out.Store<float16_t>(base + 2, i.val.x * i.val.y + (float16_t)1.0);
  Out.Store<uint16_t>(base + 4, i.id.x);
  return float4(i.val.xyxy);
}
