// Vertex fixture for the dxil / dxil-ia graphics lanes.
//
// Rebuild with the owned DXC front end (the same helper the other DXIL
// fixtures use), replacing the select-based shader this file supersedes:
//   build/owned-continuation-20260919/vertex/compile-hlsl vs_6_0 main \
//     graphics-translator/tests/dxil/vertex.hlsl \
//     graphics-translator/tests/dxil/vertex.dxil
// which is dxc -T vs_6_0 -E main -HV 2021 -O0.
//
// The corner is read from a module-scope constant array indexed by the runtime
// vertex id. DXC keeps that as a module-level immutable global
// (`@kCorners.v.1dim = internal constant [6 x float]`) reached through a
// dynamic getelementptr + load, and the front end relocates the global into
// constant storage, so the flat load has to be folded back onto that storage's
// address space. If the load is dereferenced as flat device memory instead,
// every corner reads as zero, the triangle degenerates and the graphics lane
// covers no pixels: the depth target keeps its 0.5 clear at the centre and the
// pixel shader's four UAV slots stay unwritten. Those two observables are what
// the `dxil` and `dxil-ia` suites assert, so this shader is the regression
// guard for the global-constant lowering.
//
// The observable contract is otherwise identical to the select-based form it
// replaced: one SV_VertexID input, two outputs in this order (a two-component
// varying, then SV_Position), the same corner set (-1,-1) (3,-1) (-1,3), the
// same varying scale (10,15) and the same SV_Position z/w (0,1). Only the
// corner source changed, so a failing run isolates the global-constant
// lowering rather than the varying or depth contract.

struct VSOut
{
    float2 uv : UV;
    float4 position : SV_Position;
};

/* Full-screen triangle in NDC, indexed by SV_VertexID. */
static const float2 kCorners[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
};

VSOut main(uint vertexId : SV_VertexID)
{
    VSOut output;
    float2 corner = kCorners[vertexId];
    output.uv = corner * float2(10.0, 15.0);
    output.position = float4(corner, 0.0, 1.0);
    return output;
}
