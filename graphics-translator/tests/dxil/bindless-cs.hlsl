// SM 6.6 dynamic resources (ResourceDescriptorHeap / SamplerDescriptorHeap) fixture.
//
// Every resource is fetched straight out of the bound descriptor heaps by absolute
// index; the root signature only carries CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
// SAMPLER_HEAP_DIRECTLY_INDEXED and one root constant. Each thread writes 16 result
// words at output[i * 64]; graphics-translator/tests/owned-d3d12-bindless.cpp holds
// the exact oracle for every word.
//
// Heap layout (resource heap of 1,000,000 descriptors, sampler heap of 2048):
//   999999      RWByteAddressBuffer output (4096 bytes)
//   100..179    80 raw UAV views, 16 bytes each, of one buffer (> 64 UAVs, divergent)
//   200..203    4 CBVs, x = 0x1000 + k
//   300         Buffer<uint>            element e = 0x2000 + e     (16 elements)
//   301         StructuredBuffer<uint2> element e = (e, 0x2100 + e) (8 elements)
//   302         ByteAddressBuffer       dword d = 0x2200 + d       (16 dwords)
//   303         RWBuffer<uint>          4 atomic counters
//   304         RWStructuredBuffer<uint> with a UAV counter        (64 elements)
//   400         Texture1D<uint>         x = 0x3000 + x             (4 texels)
//   401         Texture2DArray<uint>    0x3100 + x + 2y + 4z       (2x2x3)
//   402         Texture3D<uint>         0x3200 + x + 2y + 4z       (2x2x2)
//   403         Texture2D<float4>       2x2, texel (x,y).x = 10 + x + 2y
//   404         TextureCube<float4>     1x1 faces, face f .x = 20 + f
//   405         Texture2D<float> (D32)  cleared to 0.5
//   406         RWTexture2D<uint>       4x4
//   500..503    null raw SRV / null raw UAV / null Texture2D SRV / null CBV
//   samplers: 0 point clamp, 5 comparison LESS point, 2047 point wrap
//
// Rebuild with upstream DXC 1.9:
//   dxc -T cs_6_6 -E main -HV 2021 -Fo bindless-cs.dxil bindless-cs.hlsl

struct Root { uint extraIndex; };
ConstantBuffer<Root> root : register(b0);

struct Values { uint4 v; };

static const uint kOutput = 999999;

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer output = ResourceDescriptorHeap[kOutput];
    uint base = i * 64;

    // > 64 distinct UAVs, one per lane, divergent index.
    RWByteAddressBuffer many = ResourceDescriptorHeap[NonUniformResourceIndex(100 + (i * 7) % 80)];
    many.Store(0, i + 1);

    ConstantBuffer<Values> cb = ResourceDescriptorHeap[NonUniformResourceIndex(200 + i % 4)];
    output.Store(base + 0, cb.v.x);

    Buffer<uint> typed = ResourceDescriptorHeap[300];
    output.Store(base + 4, typed.Load(i % 16));

    StructuredBuffer<uint2> structured = ResourceDescriptorHeap[301];
    output.Store(base + 8, structured[i % 8].y);

    ByteAddressBuffer raw = ResourceDescriptorHeap[302];
    output.Store(base + 12, raw.Load((i % 16) * 4));

    Texture1D<uint> t1 = ResourceDescriptorHeap[400];
    output.Store(base + 16, t1.Load(int2(i % 4, 0)));

    Texture2DArray<uint> t2a = ResourceDescriptorHeap[401];
    output.Store(base + 20, t2a.Load(int4(i % 2, (i / 2) % 2, i % 3, 0)));

    Texture3D<uint> t3 = ResourceDescriptorHeap[402];
    output.Store(base + 24, t3.Load(int4(i % 2, (i / 2) % 2, (i / 4) % 2, 0)));

    // Divergent sampler: clamp (0) keeps u = 1.25 on column 1, wrap (2047) moves it to column 0.
    Texture2D<float4> t2 = ResourceDescriptorHeap[403];
    SamplerState s = SamplerDescriptorHeap[NonUniformResourceIndex((i % 2) ? 0 : 2047)];
    output.Store(base + 28, asuint(t2.SampleLevel(s, float2(1.25, 0.25), 0).x));

    TextureCube<float4> cube = ResourceDescriptorHeap[404];
    static const float3 kFace[6] = {
        float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
        float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)
    };
    SamplerState pointClamp = SamplerDescriptorHeap[0];
    output.Store(base + 32, asuint(cube.SampleLevel(pointClamp, kFace[i % 6], 0).x));

    Texture2D<float> depth = ResourceDescriptorHeap[405];
    SamplerComparisonState cmp = SamplerDescriptorHeap[5];
    output.Store(base + 36, asuint(depth.SampleCmpLevelZero(cmp, float2(0.5, 0.5), (i % 2) ? 0.25 : 0.75)));

    // Null descriptors read zero; an index past the heap resolves to a null descriptor too.
    ByteAddressBuffer nullRaw = ResourceDescriptorHeap[500];
    ByteAddressBuffer pastHeap = ResourceDescriptorHeap[NonUniformResourceIndex(1000000 + i)];
    output.Store(base + 40, nullRaw.Load(0) | pastHeap.Load(0) | 0x5a000000);

    RWByteAddressBuffer nullUav = ResourceDescriptorHeap[501];
    nullUav.Store(0, 0xdeadbeef);
    Texture2D<uint> nullTex = ResourceDescriptorHeap[502];
    output.Store(base + 44, nullUav.Load(0) | nullTex.Load(int3(0, 0, 0)) | 0x5b000000);

    ConstantBuffer<Values> nullCb = ResourceDescriptorHeap[503];
    output.Store(base + 48, nullCb.v.x | 0x5c000000);

    RWBuffer<uint> atomics = ResourceDescriptorHeap[303];
    InterlockedAdd(atomics[i % 4], 1);

    RWStructuredBuffer<uint> counted = ResourceDescriptorHeap[304];
    uint slot = counted.IncrementCounter();
    counted[slot] = i + 1;
    output.Store(base + 52, slot);

    RWTexture2D<uint> image = ResourceDescriptorHeap[406];
    if (i < 16)
        image[uint2(i % 4, i / 4)] = 0x3300 + i;

    // A root-constant heap index, the path an application's material index takes.
    ByteAddressBuffer extra = ResourceDescriptorHeap[root.extraIndex];
    output.Store(base + 56, extra.Load(4));
    output.Store(base + 60, 0xb1d1e500 + i);
}
