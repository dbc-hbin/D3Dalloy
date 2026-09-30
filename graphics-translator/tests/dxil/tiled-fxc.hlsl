// SM 5.0 DXBC tiled feedback for graphics-translator/tests/owned-d3d12-tiled.cpp (phase "dxbc-fxc").
// Frozen with Microsoft d3dcompiler_47 (FXC; sha256 1cf322cd3f610f6b0f634131b4ab615d135556302172febadfae9beeb340f231,
// a locally installed copy loaded as a native DLL through a throwaway LoadLibrary/D3DCompile driver with flags 0),
// because Wine's d3dcompiler rejects the status argument:
//   fxc /T cs_5_0 /E cs_main   /Fo tiled-fxc-cs.dxbc   tiled-fxc.hlsl
//   fxc /T ps_5_0 /E ps_main   /Fo tiled-fxc-ps.dxbc   tiled-fxc.hlsl
//   fxc /T vs_5_0 /E vs_main   /Fo tiled-fxc-vs.dxbc   tiled-fxc.hlsl
// cs_main group g (0..3) probes 64KB tile g of the reserved buffer and 128x128 tile g of row 0 of mip 0
// of the reserved texture. ps_main (implicit-LOD sample/gather feedback) writes the same texture tiles
// from a full-screen quad: one pixel per tile at (g*128+5, 7) of a 512x8 target.
Texture2D<float4> tex : register(t0);
StructuredBuffer<uint4> structured : register(t1);
Texture2DMS<float4> msaa : register(t2);
ByteAddressBuffer rootRaw : register(t3);
Texture2D<float> depthTex : register(t4); // committed R32_FLOAT (D32 is not needed for compare)
RWBuffer<uint> typedUav : register(u1);
RWByteAddressBuffer output : register(u0);
SamplerState pointSampler : register(s0);
SamplerComparisonState compareSampler : register(s1);

uint Mapped(uint status) { return CheckAccessFullyMapped(status) ? 1u : 0u; }

[numthreads(1, 1, 1)] void cs_main(uint3 group : SV_GroupID) {
  const uint g = group.x;
  const float2 uv = (float2(g * 128 + 5, 7) + 0.5) / 512.0;
  uint s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10;
  const float4 level = tex.SampleLevel(pointSampler, uv, 0, int2(0, 0), s0);
  const float4 grad = tex.SampleGrad(pointSampler, uv, float2(0, 0), float2(0, 0), int2(0, 0), 0.0, s1);
  const float4 red = tex.GatherRed(pointSampler, uv, int2(0, 0), s2);
  const uint4 st = structured.Load(g * 4096 + 1, s3);         // 16-byte elements: tile g
  const uint typed = typedUav.Load(g * 16384 + 2, s4);        // R32_UINT: tile g
  const uint root = rootRaw.Load(g * 65536 + 16, s5);         // root SRV (GPU VA) of the same buffer
  const float4 ms = msaa.Load(int2(1, 1), 0, int2(0, 0), s6); // committed MSAA: always mapped
  const float4 cmpl = tex.Load(int3(g * 128 + 5, 7, 0), int2(0, 0), s7);          // ld_s
  const float cmp = depthTex.SampleCmpLevelZero(compareSampler, uv, 0.5, int2(0, 0), s8); // committed depth
  const float4 gcmp = depthTex.GatherCmpRed(compareSampler, uv, 0.5, int2(0, 0), s9);   // committed depth
  const float4 po = tex.GatherRed(pointSampler, uv, int2(group.y, group.z), s10);      // gather4_po_s
  uint s11;
  const float4 pocmp = depthTex.GatherCmpRed(compareSampler, uv, 0.5, int2(group.y, group.z), s11); // gather4_po_c_s
  output.Store4(g * 64, uint4(Mapped(s0), Mapped(s1), Mapped(s2), Mapped(s3)));
  output.Store4(g * 64 + 16, uint4(Mapped(s4), Mapped(s5), Mapped(s6), Mapped(s7)));
  output.Store4(g * 64 + 32, uint4(st.x, typed, root, asuint(level.x + grad.x + red.x + ms.x + cmpl.x)));
  output.Store4(g * 64 + 48, uint4(Mapped(s8), Mapped(s9), Mapped(s10), Mapped(s11)));
  output.Store(256 + g * 4, asuint(cmp + gcmp.x + po.x + pocmp.x));
}

struct VSOut {
  float4 position : SV_Position;
};

VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  const float2 p = float2((id << 1) & 2, id & 2);
  o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

RWByteAddressBuffer psOutput : register(u1);

float4 ps_main(VSOut input) : SV_Target {
  // Implicit-LOD sampling runs in uniform control flow so derivatives (and so the mip, 0 here) are defined.
  const uint2 pixel = uint2(input.position.xy);
  const float2 uv = float2(pixel.x + 0.5, pixel.y + 0.5) / 512.0;
  uint s0, s1, s2, s3, s4;
  const float4 a = tex.Sample(pointSampler, uv, int2(0, 0), 0.0, s0);
  const float4 b = tex.SampleBias(pointSampler, uv, 0.0, int2(0, 0), 0.0, s1);
  const float4 c = tex.Gather(pointSampler, uv, int2(0, 0), s2);
  const float4 d = tex.Sample(pointSampler, uv, int2(0, 0), 1.0, s3);
  const float e = depthTex.SampleCmp(compareSampler, uv, 0.5, int2(0, 0), 1.0, s4); // sample_c_s, committed
  if (pixel.y == 7 && (pixel.x % 128) == 5) {
    const uint g = pixel.x / 128;
    psOutput.Store4(g * 32, uint4(Mapped(s0), Mapped(s1), Mapped(s2), Mapped(s3)));
    psOutput.Store(g * 32 + 16, Mapped(s4));
  }
  return a + b + c + d + e;
}
