// Reserved (tiled) resource shaders for graphics-translator/tests/owned-d3d12-tiled.cpp.
// Rebuild with DXC 1.9 under Wine (see build-sm6.sh for the environment):
//   dxc -nologo -HV 2021 -T cs_6_0 -E cs_write          -Fo tiled-write.dxil          tiled.hlsl
//   dxc -nologo -HV 2021 -T cs_6_0 -E cs_buffer_status  -Fo tiled-buffer-status.dxil  tiled.hlsl
//   dxc -nologo -HV 2021 -T cs_6_0 -E cs_texture_status -Fo tiled-texture-status.dxil tiled.hlsl
//
// tiled-dxbc-status.dxbc (SM 5.0 tiled feedback). Wine's d3dcompiler rejects the status argument
// ("E5017: Tiled resource status argument"), so the DXBC is the cs_5_0 output of Wine D3DCompile
// (D3DCOMPILE_SKIP_OPTIMIZATION) for the shader below with ld_raw -> ld_raw_s (opcode 226) and
// ld -> ld_s (223), each given a status dst r4.x/y/z, three check_access_fully_mapped (234)
// r5.x/y/z <- r4.x/y/z, the last store writing r5, dcl_temps 6, and the DXBC checksum recomputed:
//   ByteAddressBuffer b : register(t0); Texture2D<float4> t : register(t1); Buffer<uint> tb : register(t2);
//   RWByteAddressBuffer o : register(u0);
//   [numthreads(1,1,1)] void main(uint3 g : SV_GroupID) {
//     uint s0, s1, s2; uint w = b.Load(g.x * 65536 + 16, s0); float4 v = t.Load(int3(g.x * 128 + 5, 7, 0), 0, s1);
//     uint x = tb.Load(g.x * 16384 + 4, s2);
//     o.Store4(g.x * 32, uint4(w, asuint(v.x), x, g.x));
//     o.Store4(g.x * 32 + 16, uint4(CheckAccessFullyMapped(s0) ? ~0u : 0, ..(s1).., ..(s2).., 0)); }

// cs_write: group g writes `value + g` at byte `offset` of 64KB tile g, through a root UAV (GPU VA).
cbuffer WriteParams : register(b0) {
  uint writeValue;
  uint writeOffset;
};
RWByteAddressBuffer writeTarget : register(u0);

[numthreads(1, 1, 1)] void cs_write(uint3 group : SV_GroupID) {
  writeTarget.Store(group.x * 65536 + writeOffset, writeValue + group.x);
}

// cs_buffer_status: group g < 4 probes 64KB tile g of a reserved buffer through raw/typed SRVs and
// typed/raw UAVs (the UAVs view an alias of the same tiles); group 4 probes committed buffers.
ByteAddressBuffer rawReserved : register(t0);
Buffer<uint> typedReserved : register(t1);
ByteAddressBuffer rawCommitted : register(t2);
Buffer<uint> typedCommitted : register(t3);
RWBuffer<uint> typedUavReserved : register(u1);
RWByteAddressBuffer rawUavReserved : register(u2);
RWByteAddressBuffer bufferOutput : register(u0);

uint Mapped(uint status) { return CheckAccessFullyMapped(status) ? 1u : 0u; }

[numthreads(1, 1, 1)] void cs_buffer_status(uint3 group : SV_GroupID) {
  const uint tile = group.x;
  uint s0, s1, s2, s3, s4;
  if (tile < 4) {
    const uint byte = tile * 65536 + 16;
    const uint raw = rawReserved.Load(byte, s0);
    const uint typed = typedReserved.Load(byte / 4, s1);
    const uint uavTyped = typedUavReserved.Load(byte / 4, s2);
    const uint uavRaw = rawUavReserved.Load(byte + 16, s3);
    // Sixteen bytes ending 8 bytes into the next tile: fully mapped only when both tiles are.
    const uint4 straddle = rawUavReserved.Load4(tile * 65536 + 65528, s4);
    bufferOutput.Store4(tile * 48, uint4(raw, Mapped(s0), typed, Mapped(s1)));
    bufferOutput.Store4(tile * 48 + 16, uint4(uavTyped, Mapped(s2), uavRaw, Mapped(s3)));
    bufferOutput.Store4(tile * 48 + 32, uint4(Mapped(s4), straddle.x | straddle.w, 0, 0));
  } else {
    const uint raw = rawCommitted.Load(4, s0);
    const uint typed = typedCommitted.Load(2, s1);
    bufferOutput.Store4(4 * 48, uint4(raw, Mapped(s0), typed, Mapped(s1)));
  }
}

// cs_texture_status: group g < 16 probes 128x128 tile g of mip 0 of a reserved 512x512 RGBA8 texture
// by Load, SampleLevel and GatherRed; group 16 probes the first packed mip; group 17 a committed
// texture; group 18 min/max/average reduction samplers.
Texture2D<float4> reservedTexture : register(t0);
Texture2D<float4> committedTexture : register(t1);
Texture2D<float> reductionTexture : register(t2);
SamplerState pointSampler : register(s0);
SamplerState minimumSampler : register(s1);
SamplerState maximumSampler : register(s2);
SamplerState averageSampler : register(s3);
cbuffer TextureParams : register(b0) {
  uint packedMip;
};
RWByteAddressBuffer textureOutput : register(u0);

uint Pack(float4 value) {
  const uint4 bytes = uint4(round(saturate(value) * 255.0));
  return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

[numthreads(1, 1, 1)] void cs_texture_status(uint3 group : SV_GroupID) {
  const uint index = group.x;
  uint s0, s1, s2;
  if (index < 16) {
    const int2 texel = int2((index % 4) * 128 + 5, (index / 4) * 128 + 7);
    const float2 uv = (float2(texel) + 0.5) / 512.0;
    const float4 loaded = reservedTexture.Load(int3(texel, 0), int2(0, 0), s0);
    const float4 sampled = reservedTexture.SampleLevel(pointSampler, uv, 0, int2(0, 0), s1);
    const float4 gathered = reservedTexture.GatherRed(pointSampler, uv, int2(0, 0), s2);
    textureOutput.Store4(index * 16, uint4(Pack(loaded), Mapped(s0), Mapped(s1) | (Pack(sampled) == Pack(loaded) ? 2 : 0),
                                           Mapped(s2) | (uint(round(gathered.x * 255.0)) << 8)));
  } else if (index == 16) {
    const float4 loaded = reservedTexture.Load(int3(1, 1, packedMip), int2(0, 0), s0);
    const float4 sampled = reservedTexture.SampleLevel(pointSampler, float2(0.5, 0.5), packedMip, int2(0, 0), s1);
    textureOutput.Store4(16 * 16, uint4(Pack(loaded), Mapped(s0), Pack(sampled), Mapped(s1)));
  } else if (index == 17) {
    const float4 loaded = committedTexture.Load(int3(1, 1, 0), int2(0, 0), s0);
    const float4 sampled = committedTexture.SampleLevel(pointSampler, float2(0.5, 0.5), 0, int2(0, 0), s1);
    textureOutput.Store4(17 * 16, uint4(Pack(loaded), Mapped(s0), Pack(sampled), Mapped(s1)));
  } else {
    const float2 middle = float2(0.5, 0.5);
    textureOutput.Store4(18 * 16, uint4(asuint(reductionTexture.SampleLevel(minimumSampler, middle, 0)),
                                        asuint(reductionTexture.SampleLevel(maximumSampler, middle, 0)),
                                        asuint(reductionTexture.SampleLevel(averageSampler, middle, 0)), 0));
  }
}

// cs_root_status (tiled-root-status.dxil): group g < 4 probes 64KB tile g of a reserved buffer through
// a root SRV (t0) and a root UAV (u1); group 4 probes a committed buffer through the same root SRV slot
// of a second dispatch (the fixture rebinds it).
//   dxc -nologo -HV 2021 -T cs_6_0 -E cs_root_status -Fo tiled-root-status.dxil tiled.hlsl
ByteAddressBuffer rootSrv : register(t0, space1);
RWByteAddressBuffer rootUav : register(u1, space1);
RWByteAddressBuffer rootOutput : register(u0, space1);

[numthreads(1, 1, 1)] void cs_root_status(uint3 group : SV_GroupID) {
  uint s0, s1;
  const uint byte = group.x * 65536 + 16;
  const uint a = rootSrv.Load(byte, s0);
  const uint b = rootUav.Load(byte, s1);
  rootOutput.Store4(group.x * 16, uint4(a, Mapped(s0), b, Mapped(s1)));
}

// cs_rgb32_status (tiled-rgb32-status.dxil): an R32G32B32_UINT SRV (emulated as 12 byte elements) over a
// reserved buffer; group g loads element g*65536/12 + 1, which lies inside tile g.
//   dxc -nologo -HV 2021 -T cs_6_0 -E cs_rgb32_status -Fo tiled-rgb32-status.dxil tiled.hlsl
Buffer<uint3> rgb32 : register(t0, space2);
RWByteAddressBuffer rgb32Output : register(u0, space2);

[numthreads(1, 1, 1)] void cs_rgb32_status(uint3 group : SV_GroupID) {
  uint s0;
  const uint3 v = rgb32.Load(group.x * 65536 / 12 + 2, s0);
  rgb32Output.Store4(group.x * 16, uint4(v, Mapped(s0)));
}
