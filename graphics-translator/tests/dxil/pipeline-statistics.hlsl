// Entry counters are independent of the implementation's statistics UAV. In particular,
// the VS counter detects a second, hidden VS execution during stream-output capture.
RWStructuredBuffer<uint> vertexEntries : register(u0);
RWStructuredBuffer<uint> pixelEntries : register(u1);
RWStructuredBuffer<uint> computeEntries : register(u2);

struct VertexInput { float4 position : POSITION; };
struct VertexOutput { float4 position : SV_Position; };

VertexOutput vsMain(VertexInput input) {
    uint previous;
    InterlockedAdd(vertexEntries[0], 1, previous);
    VertexOutput output;
    output.position = input.position;
    return output;
}

float4 psMain(VertexOutput input) : SV_Target0 {
    uint previous;
    InterlockedAdd(pixelEntries[0], 1, previous);
    return float4(0.25, 0.5, 0.75, 1.0);
}

[numthreads(4, 2, 1)]
void csMain(uint3 tid : SV_DispatchThreadID) {
    uint previous;
    InterlockedAdd(computeEntries[0], 1, previous);
}
