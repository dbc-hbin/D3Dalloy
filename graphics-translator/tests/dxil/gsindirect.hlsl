cbuffer Slot : register(b0) { uint enabled; uint tag; };
cbuffer SlotCBV : register(b1) { float red; };
StructuredBuffer<float> slot_srv : register(t0);
RWStructuredBuffer<uint> slot_uav : register(u0);
struct V { float4 position : SV_Position; float red : RED; };
struct P { float4 position : SV_Position; float4 color : COLOR; };
V vs_main(float2 position : POSITION, uint instance : SV_InstanceID) {
  V o; o.position = float4(position + float2(instance * 0.125, 0), 0, 1);
  o.red = enabled ? red : 0; return o;
}
P make_point(float4 position, float r) {
  P o; o.position = position;
  o.color = enabled ? float4(r, slot_srv[0], asfloat(slot_uav[0]), tag / 255.0) : 0;
  return o;
}
[maxvertexcount(4)]
void gs_point(point V input[1], inout TriangleStream<P> stream) {
  if (!enabled) return;
  uint old; InterlockedAdd(slot_uav[1], 1, old);
  const float2 corners[4] = {float2(-1,1),float2(1,1),float2(-1,-1),float2(1,-1)};
  for (uint i=0;i<4;i++) stream.Append(make_point(input[0].position + float4(corners[i]/16.0,0,0), input[0].red));
  stream.RestartStrip();
}
struct T { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
[maxvertexcount(4)]
void gs_strip(triangle V input[3], inout TriangleStream<P> stream) {
  V point_input[1] = {input[0]}; gs_point(point_input, stream);
}
[maxvertexcount(4)]
void gs_adj(triangleadj V input[6], inout TriangleStream<P> stream) {
  V point_input[1] = {input[0]}; gs_point(point_input, stream);
}
T hs_factors(InputPatch<V,1> input) {
  T o; o.edge[0]=o.edge[1]=o.edge[2]=o.inside=enabled ? 1 : 0; return o;
}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")]
[outputcontrolpoints(1)][patchconstantfunc("hs_factors")][maxtessfactor(64.0)]
V hs_main(InputPatch<V,1> input, uint i : SV_OutputControlPointID) { return input[0]; }
[domain("tri")]
P ds_main(T constants, float3 uv : SV_DomainLocation, const OutputPatch<V,1> input) {
  float2 corner = float2(-1,1)*uv.x + float2(1,1)*uv.y + float2(-1,-1)*uv.z;
  uint old; InterlockedAdd(slot_uav[1],1,old);
  return make_point(input[0].position + float4(corner/16.0,0,0), input[0].red);
}
[maxvertexcount(3)]
void gs_tri(triangle P input[3], inout TriangleStream<P> stream) {
  for (uint i=0;i<3;i++) { P o=input[i]; o.color.a=tag/255.0; stream.Append(o); }
  stream.RestartStrip();
}
float4 ps_main(P input) : SV_Target { return enabled ? float4(input.color.rgb, tag / 255.0) : 0; }
