// Rebuild with upstream DXC: dxc -T cs_6_0 -E main -HV 2021 -O0 -Fo register-space.dxil register-space.hlsl
cbuffer First : register(b0, space2) { uint first; };
cbuffer Second : register(b0, space9) { uint second; };
RWByteAddressBuffer output : register(u0);
[numthreads(1,1,1)] void main() { output.Store(0, first); output.Store(4, second); }

