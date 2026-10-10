// Descriptor-copy/overwrite/mip/array/retirement regression, loaded by name beside
// owned-d3d12-bindless.exe in both the full suite and --view-pool mode.
// Wine's built-in D3DCompile lacks RWTexture2DArray<uint>::GetDimensions.
// Rebuild with official DXC 1.9 (validator enabled):
//   dxc -nologo -HV 2021 -O0 -T cs_6_0 -E main -Fo bindless-view-pool-cs.dxil bindless-view-pool.hlsl
Texture2DArray<uint> t : register(t0);
RWTexture2DArray<uint> w : register(u0);
RWStructuredBuffer<uint> o : register(u1);
cbuffer C : register(b0) { uint i; }

[numthreads(1, 1, 1)]
void main()
{
    uint a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, h = 0;
    if (i != 3)
        t.GetDimensions(0, a, b, c, d);
    if ((i & 3) != 3)
        w.GetDimensions(e, f, h);
    uint p = 1 + i * 8;
    o[p] = a;
    o[p + 1] = b;
    o[p + 2] = c;
    o[p + 3] = d;
    o[p + 4] = t.Load(int4(0, 0, 0, 0));
    o[p + 5] = e;
    o[p + 6] = h;
    o[p + 7] = w[uint3(0, 0, 0)];
    w[uint3(0, 0, 0)] = 9000 + i;
}
