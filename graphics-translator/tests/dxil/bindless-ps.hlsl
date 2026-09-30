// SM 6.6 dynamic resources in a pixel shader: per-pixel divergent heap indices.
//
// A 4x4 render target; pixel (x, y) samples texture ResourceDescriptorHeap[600 + x]
// with sampler SamplerDescriptorHeap[y % 2 ? 7 : 6], both NonUniformResourceIndex.
// Texture 600 + k is a 1x1 R32_FLOAT texture holding 100 + k; sampler 6 is point
// clamp, 7 is point wrap - both return the single texel, so the oracle is exact:
//   out = (100 + x, y, 0, 1).
// Descriptor 999998 holds a 1x1 texture of 7.0 that the last column also samples,
// proving a near-limit index from the pixel stage.
//
// Rebuild with upstream DXC 1.9:
//   dxc -T ps_6_6 -E main -HV 2021 -Fo bindless-ps.dxil bindless-ps.hlsl
//   dxc -T vs_6_6 -E vs  -HV 2021 -Fo bindless-vs.dxil bindless-ps.hlsl

float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 main(float4 position : SV_Position) : SV_Target
{
    uint x = uint(position.x);
    uint y = uint(position.y);
    Texture2D<float> tex = ResourceDescriptorHeap[NonUniformResourceIndex(600 + x)];
    SamplerState s = SamplerDescriptorHeap[NonUniformResourceIndex(y % 2 ? 7 : 6)];
    float value = tex.SampleLevel(s, float2(0.5, 0.5), 0);
    if (x == 3) {
        Texture2D<float> far = ResourceDescriptorHeap[999998];
        value += far.SampleLevel(s, float2(0.5, 0.5), 0) * 1000.0;
    }
    return float4(value, y, 0, 1);
}
