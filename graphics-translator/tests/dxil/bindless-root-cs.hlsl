// Root-descriptor output plus one heap-indexed source, for the SetDescriptorHeaps /
// root signature ordering contract of SM 6.6 dynamic resources.
//
// Rebuild with upstream DXC 1.9:
//   dxc -T cs_6_6 -E main -HV 2021 -Fo bindless-root-cs.dxil bindless-root-cs.hlsl
struct Root { uint index; };
ConstantBuffer<Root> root : register(b0);
RWByteAddressBuffer result : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer source = ResourceDescriptorHeap[root.index];
    SamplerState unusedSampler = SamplerDescriptorHeap[0];
    Texture2D<float> heapTexture = ResourceDescriptorHeap[root.index + 1];
    result.Store(0, source.Load(0));
    result.Store(4, asuint(heapTexture.SampleLevel(unusedSampler, float2(0.5, 0.5), 0)));
    result.Store(8, 0xabc);
}
