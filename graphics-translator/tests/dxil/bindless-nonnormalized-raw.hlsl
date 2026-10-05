// DXC -T cs_6_7 -E main -HV 2021; valid nonnormalized GatherRaw coordinates, no offsets.
Texture2D<uint> t:register(t0),excluded:register(t1);
SamplerState samplerState:register(s0);
RWByteAddressBuffer result:register(u0);
[numthreads(1,1,1)] void main(){
  result.Store4(0,t.GatherRaw(samplerState,float2(1,1)));
  result.Store4(16,excluded.GatherRaw(samplerState,float2(1,1)));
}
