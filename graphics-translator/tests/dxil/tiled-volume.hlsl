/* CPU-only native Microsoft producer recipe, from the repository root:
export WINEDEBUG=-all WINEPREFIX="$PWD/build/local/p3-20261003/tiled-volume/producer-prefix"
export DYLD_FALLBACK_LIBRARY_PATH="$PWD/toolchains/wine-runtime/lib"
wine="$PWD/toolchains/wine-runtime/bin/wine.real"
dxc="Z:$PWD/build/local/p3-20261003/matrix/stable/bin/x64/dxc.exe"
fxc="Z:$PWD/build/local/p3-20261003/fp64-reference/fxc-native/fxc.exe"
source="Z:$PWD/graphics-translator/tests/dxil/tiled-volume.hlsl"
for spec in 'read VolumeReadCS cs_6_0 cs_5_1' 'uint VolumeUintCS cs_6_7 cs_5_1' \
            'vs VolumeVS vs_6_0 vs_5_1' 'ps VolumeSamplePS ps_6_0 ps_5_1' \
            'color VolumeColorPS ps_6_0 ps_5_1'; do
  set -- $spec
  dxdefines=; fxdefines=
  if [ "$1" = uint ]; then dxdefines='-D VOLUME_UINT=1'; fxdefines='/D VOLUME_UINT=1 /D VOLUME_DXBC=1'; fi
  "$wine" "$dxc" -T "$3" -E "$2" $dxdefines \
    -Fo "Z:$PWD/graphics-translator/tests/dxil/tiled-volume-$1.dxil" "$source" || exit
  WINEDLLOVERRIDES=d3dcompiler_47=n "$wine" "$fxc" /nologo /T "$4" /E "$2" $fxdefines \
    /Fo "Z:$PWD/graphics-translator/tests/dxil/tiled-volume-$1.dxbc" "$source" || exit
done
The FXC directory must retain its native Microsoft d3dcompiler_47.dll.
Fixture argv order: tiled-volume-{read,uint,vs,ps,color}.dxil, then the same five .dxbc.
DXBC integer sampling is not a valid SM5 operation: VOLUME_DXBC uses the valid load/UAV paths.
*/
cbuffer Case : register(b0) {
  float4 UVLOD;
  float4 DXMinimum;
  float4 DYMip;
  uint4 Config;
};
SamplerState S : register(s0);
RWByteAddressBuffer O : register(u1);

#ifndef VOLUME_UINT
Texture3D<float4> T : register(t0);
[numthreads(1, 1, 1)]
void VolumeReadCS() {
  uint a, b, c;
  float4 x = T.SampleLevel(S, UVLOD.xyz, UVLOD.w, int3(0, 0, 0), a);
  float4 y = T.SampleGrad(S, UVLOD.xyz, DXMinimum.xyz, DYMip.xyz,
                          int3(0, 0, 0), DXMinimum.w, b);
  float4 z = T.Load(int4(Config.xyz, (uint)DYMip.w), int3(0, 0, 0), c);
  uint w, h, d, m;
  T.GetDimensions((uint)DYMip.w, w, h, d, m);
  O.Store4(Config.w, asuint(x));
  O.Store4(Config.w + 16, asuint(y));
  O.Store4(Config.w + 32, asuint(z));
  O.Store4(Config.w + 48, uint4(w, h, d, m));
  O.Store4(Config.w + 64, uint4(CheckAccessFullyMapped(a), CheckAccessFullyMapped(b),
                                  CheckAccessFullyMapped(c), 0));
}
struct Vertex {
  float4 position : SV_Position;
  float3 coordinate : TEXCOORD0;
};
Vertex VolumeVS(uint id : SV_VertexID) {
  float2 q = id == 0 ? float2(0, 0) : id == 1 ? float2(0, 2) : float2(2, 0);
  Vertex v;
  v.position = float4(q * 2 - 1, 0, 1);
  v.coordinate = UVLOD.xyz + 4 * (q.x * DXMinimum.xyz + q.y * DYMip.xyz);
  return v;
}
float4 VolumeSamplePS(Vertex v) : SV_Target {
  uint a, b;
  float4 x = T.Sample(S, v.coordinate, int3(0, 0, 0), DXMinimum.w, a);
  float4 y = T.SampleBias(S, v.coordinate, 1, int3(0, 0, 0), DXMinimum.w, b);
  uint offset = Config.w + 48 * ((uint)v.position.x + 4 * (uint)v.position.y);
  O.Store4(offset, asuint(x));
  O.Store4(offset + 16, asuint(y));
  O.Store4(offset + 32, uint4(asuint(T.CalculateLevelOfDetail(S, v.coordinate)),
      asuint(T.CalculateLevelOfDetailUnclamped(S, v.coordinate)),
      CheckAccessFullyMapped(a), CheckAccessFullyMapped(b)));
  return x;
}
float4 VolumeColorPS(Vertex v) : SV_Target {
  return UVLOD;
}
#else
Texture3D<uint> T : register(t0);
RWTexture3D<uint> V : register(u0);
Texture3D<uint2> B : register(t1);
RWTexture3D<uint2> W : register(u2);
RWTexture3D<uint4> H : register(u3);
[numthreads(1, 1, 1)]
void VolumeUintCS() {
  if (Config.w & 0x20000000u) {
    // Both UAVs reference the same resource, not two dispatch-synchronized shadows.
    uint address = Config.w & 0x1fffffffu;
    W[Config.xyz] = asuint(DXMinimum.xy);
    DeviceMemoryBarrier();
    O.Store4(address, H[Config.xyz]);
    H[Config.xyz] = asuint(DYMip);
    DeviceMemoryBarrier();
    O.Store4(address + 16, H[Config.xyz]);
    uint w, h, d, hw, hh, hd;
    W.GetDimensions(w, h, d); H.GetDimensions(hw, hh, hd);
    O.Store4(address + 32, uint4(W[Config.xyz], w, h));
    O.Store4(address + 48, uint4(d, hw, hh, hd));
    return;
  }
  if (Config.w & 0x10000000u) {
    uint address = Config.w & 0x0fffffffu;
    O.Store4(address, H[Config.xyz]);
    H[Config.xyz] = asuint(DXMinimum);
    DeviceMemoryBarrier();
    O.Store4(address + 16, H[Config.xyz]);
    uint w, h, d; H.GetDimensions(w, h, d);
    O.Store4(address + 32, uint4(w, h, d, 0));
    return;
  }
  if (Config.w & 0x40000000u) {
    uint address = Config.w & 0x3fffffffu;
    if (Config.w & 0x80000000u) {
      uint status;
      uint2 value = B.Load(int4(Config.xyz + uint3(0, 0, (uint)DYMip.w), 0), int3(0, 0, 0), status);
      O.Store4(address, uint4(value, CheckAccessFullyMapped(status), 0));
    } else {
      uint2 prior = W[Config.xyz];
      W[Config.xyz] = asuint(DXMinimum.xy);
      DeviceMemoryBarrier();
      O.Store4(address + 16, uint4(prior, W[Config.xyz]));
      uint w, h, d; W.GetDimensions(w, h, d);
      O.Store4(address + 32, uint4(w, h, d, 0));
    }
    return;
  }
  if (Config.w & 0x80000000u) {
    uint a, b;
    uint3 world = Config.xyz + uint3(0, 0, (uint)DYMip.w);
    uint x = T.Load(int4(world, 0), int3(0, 0, 0), a);
#ifdef VOLUME_DXBC
  // Integer SampleLevel was introduced in SM 6.7; DXBC covers the valid load/UAV routes.
  uint y = T.Load(int4(world, 0), int3(0, 0, 0), b);
#else
  uint y = T.SampleLevel(S, UVLOD.xyz, UVLOD.w, int3(0, 0, 0), b);
#endif
    O.Store4(Config.w & 0x7fffffffu, uint4(x, y, CheckAccessFullyMapped(a), CheckAccessFullyMapped(b)));
    return;
  }
  uint old = V[Config.xyz];
  V[Config.xyz] = asuint(DXMinimum.x);
  DeviceMemoryBarrier();
  uint atomicOld;
  InterlockedAdd(V[Config.xyz], 7, atomicOld);
  DeviceMemoryBarrier();
  uint exchanged;
  InterlockedCompareExchange(V[Config.xyz], asuint(DXMinimum.x) + 7, asuint(DXMinimum.x) + 11, exchanged);
  DeviceMemoryBarrier();
  uint w, h, d;
  V.GetDimensions(w, h, d);
  O.Store4(Config.w + 16, uint4(old, atomicOld, exchanged, V[Config.xyz]));
  O.Store4(Config.w + 32, uint4(w, h, d, 0));
}
#endif
