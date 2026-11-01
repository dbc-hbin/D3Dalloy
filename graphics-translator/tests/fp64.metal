#include <metal_stdlib>
using namespace metal;
#define BINARY(name) ulong fp64_##name(ulong, ulong) asm("d3dalloy.fp64." #name);
BINARY(add) BINARY(sub) BINARY(mul) BINARY(div) BINARY(rem) BINARY(min) BINARY(max)
#undef BINARY
ulong fp64_fma(ulong, ulong, ulong) asm("d3dalloy.fp64.fma");
ulong fp64_sqrt(ulong) asm("d3dalloy.fp64.sqrt");
ulong fp64_round(ulong, uint) asm("d3dalloy.fp64.round");
ulong fp64_from_i64(long) asm("d3dalloy.fp64.from.i64");
ulong fp64_from_u64(ulong) asm("d3dalloy.fp64.from.u64");
ulong fp64_from_f32(uint) asm("d3dalloy.fp64.from.f32");
uint fp64_to_f32(ulong) asm("d3dalloy.fp64.to.f32");
long fp64_to_i64(ulong) asm("d3dalloy.fp64.to.i64");
ulong fp64_to_u64(ulong) asm("d3dalloy.fp64.to.u64");
int fp64_to_i32(ulong) asm("d3dalloy.fp64.to.i32");
uint fp64_to_u32(ulong) asm("d3dalloy.fp64.to.u32");
bool fp64_eq(ulong, ulong) asm("d3dalloy.fp64.eq");
kernel void fp64_compute(device const ulong *input [[buffer(0)]], device ulong *output [[buffer(1)]], uint id [[thread_position_in_grid]]) {
  ulong a=input[id*3], b=input[id*3+1], c=input[id*3+2];
  device ulong *r=output+id*22;
  r[0]=fp64_add(a,b); r[1]=fp64_sub(a,b); r[2]=fp64_mul(a,b); r[3]=fp64_div(a,b);
  r[4]=fp64_rem(a,b); r[5]=fp64_fma(a,b,c); r[6]=fp64_sqrt(a);
  r[7]=fp64_round(a,0); r[8]=fp64_round(a,1); r[9]=fp64_round(a,2); r[10]=fp64_round(a,3);
  r[11]=fp64_from_i64(as_type<long>(a)); r[12]=fp64_from_u64(a); r[13]=fp64_to_f32(a);
  r[14]=fp64_from_f32(uint(a)); r[15]=as_type<ulong>(fp64_to_i64(a)); r[16]=fp64_to_u64(a);
  r[17]=ulong(long(fp64_to_i32(a))); r[18]=fp64_to_u32(a); r[19]=fp64_min(a,b); r[20]=fp64_max(a,b); r[21]=fp64_eq(a,b);
}
vertex float4 fp64_vertex(uint id [[vertex_id]]) {
  const float2 p[]={float2(-1,-1),float2(3,-1),float2(-1,3)};
  return float4(p[id],0,1);
}
fragment uint4 fp64_fragment(float4 p [[position]], device const ulong *input [[buffer(0)]]) {
  uint id=uint(p.x); ulong a=input[id*3],b=input[id*3+1],c=input[id*3+2];
  ulong f=fp64_fma(a,b,c), d=fp64_add(a,b);
  return uint4(uint(f),uint(f>>32),uint(d),uint(d>>32));
}
