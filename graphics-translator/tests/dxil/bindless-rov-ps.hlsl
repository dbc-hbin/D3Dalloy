// SM 6.6 rasterizer ordered views taken from ResourceDescriptorHeap.
//
// kTriangles fullscreen triangles overlap every pixel of a 16x16 target. Each fragment
// does v = v * 4 + primitive + 1 on one element of three heap ROVs, so only an ordered
// read-modify-write per pixel gives the exact base-4 value (mod 2^32) at every pixel:
//   structured buffer  ResourceDescriptorHeap[base + 0 + (x & 1)]         [y * 16 + x]
//   typed buffer       ResourceDescriptorHeap[base + 2 + ((x >> 1) & 1)]  [y * 16 + x]
//   texture 2D         ResourceDescriptorHeap[base + 4 + (y & 1)]         [x, y]
// All indices are per-pixel divergent (NonUniformResourceIndex). A dependent spin widens
// the read-to-write window so an unordered implementation loses updates.
//
// Rebuild with upstream DXC 1.9:
//   dxc -T ps_6_6 -E main -HV 2021 -Fo bindless-rov-ps.dxil bindless-rov-ps.hlsl
//   dxc -T vs_6_6 -E vs   -HV 2021 -Fo bindless-rov-vs.dxil bindless-rov-ps.hlsl

cbuffer Root : register(b0) { uint base; };

struct V {
    float4 position : SV_Position;
    nointerpolation uint primitive : PRIMITIVE;
};

V vs(uint id : SV_VertexID)
{
    const uint corner = id % 3;
    V v;
    v.position = float4(corner == 1 ? 3.0 : -1.0, corner == 2 ? 3.0 : -1.0, 0, 1);
    v.primitive = id / 3;
    return v;
}

uint spin(uint value)
{
    for (uint k = 0; k < 64; ++k)
        value = value * 1664525u + 1013904223u;
    return value;
}

uint accumulate(uint prev, uint primitive)
{
    // Never true for the values this test produces; keeps the spin on the critical path.
    return prev * 4u + primitive + 1u + (spin(prev) == 0x9e3779b9u ? 1u : 0u);
}

uint main(V input) : SV_Target
{
    const uint x = uint(input.position.x);
    const uint y = uint(input.position.y);
    const uint element = y * 16 + x;
    RasterizerOrderedStructuredBuffer<uint> structured =
        ResourceDescriptorHeap[NonUniformResourceIndex(base + 0 + (x & 1))];
    RasterizerOrderedBuffer<uint> typed = ResourceDescriptorHeap[NonUniformResourceIndex(base + 2 + ((x >> 1) & 1))];
    RasterizerOrderedTexture2D<uint> image = ResourceDescriptorHeap[NonUniformResourceIndex(base + 4 + (y & 1))];
    structured[element] = accumulate(structured[element], input.primitive);
    typed[element] = accumulate(typed[element], input.primitive);
    image[uint2(x, y)] = accumulate(image[uint2(x, y)], input.primitive);
    return input.primitive;
}
