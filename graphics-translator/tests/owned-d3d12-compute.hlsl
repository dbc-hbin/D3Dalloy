Texture2D<float4> Source : register(t0);
RWStructuredBuffer<uint> Result : register(u0);

cbuffer FrameConstants : register(b0)
{
    uint frameIndex;
    uint renderWidth;
    uint renderHeight;
    uint patternSalt;
};

[numthreads(8, 8, 1)]
void csMain(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x >= renderWidth || threadId.y >= renderHeight)
        return;

    float4 color = Source.Load(int3(threadId.xy, 0));
    uint mask = (color.r > 0.5 ? 1u : 0u)
              | (color.g > 0.5 ? 2u : 0u)
              | (color.b > 0.5 ? 4u : 0u)
              | (color.a > 0.5 ? 8u : 0u);
    uint coordinateTag = (threadId.x ^ (threadId.y * 17u) ^ patternSalt) & 0xffu;
    uint index = threadId.y * renderWidth + threadId.x;
    Result[index] = 0xa5000000u | (coordinateTag << 16) | ((frameIndex & 0xffu) << 8) | mask;
}

[numthreads(64, 1, 1)]
void csBarrierProducer(uint3 threadId : SV_DispatchThreadID)
{
    uint elementCount = renderWidth * renderHeight;
    if (threadId.x >= elementCount)
        return;

    uint index = threadId.x;
    Result[index] = 0x13579bdfu ^ (index * 0x9e3779b9u) ^ frameIndex ^ (patternSalt << 24);
}

[numthreads(64, 1, 1)]
void csBarrierConsumer(uint3 threadId : SV_DispatchThreadID)
{
    uint elementCount = renderWidth * renderHeight;
    if (threadId.x >= elementCount)
        return;

    uint index = threadId.x;
    uint value = Result[index];
    Result[index] = ((value << 7) | (value >> 25)) ^ 0xa5a5a5a5u;
}
