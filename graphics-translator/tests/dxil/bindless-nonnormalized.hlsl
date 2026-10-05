// Single-mip, non-array 1D/2D pixel-coordinate sampler contract.
// DXC: -HV 2021 -T {cs,vs,ps}_6_5 -E {cs,vs,ps}; GPU oracle in owned-d3d12-bindless.cpp.

Texture2D<float> t:register(t0);Texture1D<float> one:register(t1);Texture2D<float> missing:register(t2);
Texture2D<float4> excluded:register(t3);
Texture2D<float4> excludedMapped:register(t4);
Texture2D<float> odd:register(t5);
SamplerState pointS:register(s0),linearS:register(s1),minPoint:register(s2),minLinear:register(s3),maxPoint:register(s4),maxLinear:register(s5),normal:register(s6),whiteBorder:register(s7);
RWByteAddressBuffer output:register(u0);
[numthreads(1,1,1)]void cs(){
 output.Store(0,asuint(t.SampleLevel(pointS,float2(1.5,.5),0)));
 output.Store(4,asuint(t.SampleLevel(pointS,float2(1.5,.5),9)));
 output.Store(8,asuint(t.SampleLevel(pointS,float2(1.99,.75),0)));
 output.Store(12,asuint(t.SampleLevel(pointS,float2(2.01,.75),0)));
 output.Store(16,asuint(t.SampleLevel(linearS,float2(1,1),0)));
 output.Store(20,asuint(t.SampleLevel(minPoint,float2(1.5,.5),0)));
 output.Store(24,asuint(t.SampleLevel(minLinear,float2(1,1),0)));
 output.Store(28,asuint(t.SampleLevel(maxPoint,float2(1.5,.5),0)));
 output.Store(32,asuint(t.SampleLevel(maxLinear,float2(1,1),0)));
 output.Store(36,asuint(t.SampleLevel(normal,float2(.375,.25),0)));
 output.Store(40,asuint(t.SampleLevel(normal,float2(1.5,.5),0)));
 output.Store(44,asuint(t.SampleGrad(pointS,float2(1.5,.5),float2(16,0),float2(0,16))));
 output.Store4(48,asuint(t.GatherRed(pointS,float2(1,1))));
 output.Store(64,asuint(one.SampleLevel(pointS,1.5,0)));
 output.Store(68,asuint(missing.SampleLevel(pointS,float2(1.5,.5),0)));
 output.Store(72,asuint(t.SampleLevel(linearS,float2(.75,.75),0)));
 output.Store(76,asuint(t.SampleLevel(pointS,float2(3.75,1.75),0)));
 // Both coordinates are in range; the white border participates in the linear footprint.
 output.Store(80,asuint(t.SampleLevel(whiteBorder,float2(.25,.5),0)));
 output.Store(84,asuint(t.SampleLevel(linearS,float2(.25,.5),0)));
 // Positive view-relative resource clamp excludes this single-mip SRV, including missing alpha=1.
 output.Store4(88,asuint(excluded.SampleLevel(pointS,float2(1.5,.5),0)));
 output.Store4(104,asuint(excluded.GatherRed(pointS,float2(1,1))));
 output.Store4(120,asuint(excludedMapped.SampleLevel(pointS,float2(1.5,.5),0)));
 output.Store4(136,asuint(excluded.GatherAlpha(pointS,float2(1,1))));
 output.Store(152,asuint(odd.SampleLevel(pointS,float2(.99,.5),0)));
 output.Store(156,asuint(odd.SampleLevel(pointS,float2(1.01,.5),0)));
 output.Store(160,asuint(odd.SampleLevel(linearS,float2(1,.5),0)));
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 p : SV_Position) : SV_Target {
  return float4(t.Sample(pointS, float2(1.5,.5)),
                t.SampleBias(pointS, float2(1.5,.5), 4),
                t.Sample(linearS, float2(1,1)),
                t.Sample(normal, float2(1.5,.5)));
}

