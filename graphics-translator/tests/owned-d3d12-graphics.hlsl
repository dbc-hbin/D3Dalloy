cbuffer FrameConstants : register(b0)
{
    uint frameIndex;
    uint renderWidth;
    uint renderHeight;
    uint patternSalt;
};

struct VertexOutput
{
    float4 position : SV_Position;
};

VertexOutput vsMain(uint vertexId : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

float4 psMain(VertexOutput input) : SV_Target0
{
    uint2 pixel = uint2(input.position.xy);
    uint selector = (pixel.x + 3u * pixel.y + frameIndex + patternSalt) & 3u;

    if (selector == 0u)
        return float4(1.0, 0.0, 0.0, 0.0);
    if (selector == 1u)
        return float4(0.0, 1.0, 0.0, 0.0);
    if (selector == 2u)
        return float4(0.0, 0.0, 1.0, 0.0);
    return float4(0.0, 0.0, 0.0, 1.0);
}
