// Final DXC SM6.7 overloads: no Texture3D SampleCmpLevel and no 1D/cube GatherRaw.
// The producer validates each shape separately, including status/no-status overloads.
#ifndef SHAPE
#define SHAPE 2
#endif
#ifndef RAW_BITS
#define RAW_BITS 32
#endif
RWByteAddressBuffer result : register(u0);
SamplerState pointSampler : register(s0);
SamplerComparisonState comparison : register(s1), borderComparison : register(s2);
SamplerComparisonState maxComparison : register(s3), minComparison : register(s4);
SamplerComparisonState biasComparison : register(s5);
#if defined(RAW)
#if RAW_BITS == 16
#define ELEMENT uint16_t
#elif RAW_BITS == 64
#define ELEMENT uint64_t
#else
#define ELEMENT uint
#endif
#if SHAPE == 2
Texture2D<ELEMENT> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
Texture2D<ELEMENT> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(p) (p)
#else
Texture2DArray<ELEMENT> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
Texture2DArray<ELEMENT> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(p) float3(p, 0)
#endif
void exercise() {
  uint status;
  vector<ELEMENT, 4> v;
  for (uint i = 0; i < 12; ++i) {
    if (i == 0) v = tex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(0, 0), status);
    if (i == 1) v = tex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)));
    if (i == 2) v = tex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(1, 0), status);
    if (i == 3) v = nullTex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(0, 0), status);
    if (i == 4) v = emptyTex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(0, 0), status);
    if (i == 5) v = clampTex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(0, 0), status);
    if (i == 6) v = tex.GatherRaw(pointSampler, LOCATION(float2(-.5, .5)), int2(0, 0), status);
    if (i == 7) v = tex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(-1, 0));
    if (i == 8) v = fractionalTex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(0, 0), status);
    if (i == 9) v = fractionalEmptyTex.GatherRaw(pointSampler, LOCATION(float2(.5, .5)), int2(0, 0), status);
#if SHAPE == 3
    if (i == 10) v = tex.GatherRaw(pointSampler, float3(.5, .5, .6), int2(0, 0), status);
    if (i == 11) v = tex.GatherRaw(pointSampler, float3(.5, .5, -99), int2(0, 0), status);
#else
    if (i >= 10) v = tex.GatherRaw(pointSampler, float2(.5, .5), int2(0, 0), status);
#endif
    result.Store4(i * 48, uint4(v));
#if RAW_BITS == 64
    result.Store4(i * 48 + 16, uint4(v >> 32));
#else
    result.Store4(i * 48 + 16, 0);
#endif
    result.Store4(i * 48 + 32, uint4(i == 1 || i == 7 ? 1 : CheckAccessFullyMapped(status), 0x13579bdf, i, RAW_BITS));
  }
}
#else
#if SHAPE == 0
Texture1D<float> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
Texture1D<float> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(x) (x)
#define OFFSET 1
#elif SHAPE == 1
Texture1DArray<float> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
Texture1DArray<float> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(x) float2(x, 0)
#define OFFSET 1
#elif SHAPE == 2
Texture2D<float> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
Texture2D<float> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(x) float2(x, .25)
#define OFFSET int2(1, 0)
#elif SHAPE == 3
Texture2DArray<float> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
Texture2DArray<float> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(x) float3(x, .25, 0)
#define OFFSET int2(1, 0)
#elif SHAPE == 4
TextureCube<float> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
TextureCube<float> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(x) float3(0, 0, 1)
#else
TextureCubeArray<float> tex : register(t0), nullTex : register(t1), clampTex : register(t2), emptyTex : register(t3);
TextureCubeArray<float> fractionalTex : register(t4), fractionalEmptyTex : register(t5);
#define LOCATION(x) float4(0, 0, 1, 0)
#endif
// Explicit zero offsets prevent unsigned status lvalues binding an input offset.
#if SHAPE < 2
#define STATUS_ARGS 0, status
#elif SHAPE < 4
#define STATUS_ARGS int2(0, 0), status
#else
#define STATUS_ARGS status
#endif
#if defined(CMP_GRAD) || defined(CMP_BIAS)
// View mip0 is resource mip1 (4 texels): .25 fails, mip1 (.75) passes.
// Evaluate every quad lane before selecting the writer; bias needs real derivatives.
float2 compareCase(uint i, float quad = 0) {
  uint status;
  float reference = i == 9 || i == 10 || i == 11 ? -1 : i == 19 ? .875 : .5;
#ifdef CMP_GRAD
  if (i == 14) reference = .125;
#endif
  float clamp = i == 4 ? 1 : i == 5 ? .25 : i == 6 ? .75 : 0;
  float bias = i == 1 || i == 2 || i == 12 || i == 19 ? 1 : i == 3 ? -2 : i == 20 ? -.25 : i == 21 ? .75 : 0;
#if SHAPE < 2
  float location = .25 + quad * .25;
  float dx = i == 1 || i == 12 || i == 19 ? .5 : i == 3 ? .0625 : .25;
  float dy = i == 2 || i == 21 ? .5 : .03125;
#elif SHAPE < 4
  float2 location = float2(.25 + quad * .25, .25);
  // The view is 4x8, so a Y gradient of .25 selects mip1, not mip0.
  float2 dx = float2(i == 1 || i == 12 || i == 19 ? .5 : i == 3 ? .0625 : .25, .015625);
  float2 dy = float2(.03125, i == 2 || i == 21 ? .25 : .015625);
#else
  // At +Z, projected UV derivatives are half the X/Y direction derivatives.
  float3 location = float3(quad * .5, 0, 1);
  float3 dx = float3(i == 1 || i == 12 || i == 19 ? 1 : i == 3 ? .125 : .5, .0625, i == 20 ? 4 : 0);
  float3 dy = float3(.0625, i == 2 || i == 21 ? 1 : .0625, i == 20 ? 8 : 0);
#endif
#if SHAPE == 1
  float2 coord = float2(location, i == 17 ? .6 : i == 18 ? -99 : 0);
#elif SHAPE == 3
  float3 coord = float3(location, i == 17 ? .6 : i == 18 ? -99 : 0);
#elif SHAPE == 5
  float4 coord = float4(location, i == 17 ? .6 : i == 18 ? -99 : 0);
#else
  #define coord location
#endif
#if SHAPE < 2
  #define EXT_OFFSET (i == 16 ? 1 : 0)
  #define EXT_STATUS EXT_OFFSET, clamp, status
#elif SHAPE < 4
  #define EXT_OFFSET int2(i == 16 ? 1 : 0, 0)
  #define EXT_STATUS EXT_OFFSET, clamp, status
#else
  #define EXT_OFFSET
  #define EXT_STATUS clamp, status
#endif
#ifdef CMP_GRAD
  #define EXT_SAMPLE(t, s) t.SampleCmpGrad(s, coord, reference, dx, dy, EXT_STATUS)
  #define EXT_NO_STATUS tex.SampleCmpGrad(comparison, coord, reference, dx, dy)
#else
  #define EXT_SAMPLE(t, s) t.SampleCmpBias(s, coord, reference, bias, EXT_STATUS)
  #define EXT_NO_STATUS tex.SampleCmpBias(comparison, coord, reference, bias)
#endif
  float value;
  if (i == 7) value = EXT_SAMPLE(clampTex, comparison);
  else if (i == 8) value = EXT_SAMPLE(fractionalTex, comparison);
  else if (i == 9) value = EXT_SAMPLE(nullTex, comparison);
  else if (i == 10) value = EXT_SAMPLE(emptyTex, comparison);
  else if (i == 11) value = EXT_SAMPLE(fractionalEmptyTex, comparison);
  else if (i == 12) value = EXT_SAMPLE(tex, maxComparison);
  else if (i == 13) value = EXT_SAMPLE(tex, minComparison);
  else if (i == 14) {
#ifdef CMP_GRAD
    // Do not use sampler MipLODBias to decide explicit-gradient conformance.
    value = EXT_SAMPLE(tex, comparison);
#else
    value = EXT_SAMPLE(tex, biasComparison);
#endif
  }
  else if (i == 15) return float2(EXT_NO_STATUS, 1);
  else value = EXT_SAMPLE(tex, comparison);
  return float2(value, CheckAccessFullyMapped(status));
}
void exercise(float quad = 0, bool writer = true) {
  for (uint i = 0; i < 22; ++i) {
    float2 v = compareCase(i, quad);
    if (writer) result.Store2(i * 8, uint2(asuint(v.x), uint(v.y)));
  }
}
#else
float2 compareCase(uint i) {
  float lod = i == 1 || i == 11 ? 1 : i == 2 ? 100 : i == 3 ? -10 : i == 13 ? .25 : i == 14 ? .75 : 0;
  uint status;
  float v;
  if (i == 20) v = fractionalTex.SampleCmpLevel(comparison, LOCATION(.25), .5, lod, STATUS_ARGS);
  else if (i == 21) v = fractionalEmptyTex.SampleCmpLevel(comparison, LOCATION(.25), -1, lod, STATUS_ARGS);
  else if (i == 5) v = nullTex.SampleCmpLevel(comparison, LOCATION(.25), -1, lod, STATUS_ARGS);
  else if (i == 6) v = emptyTex.SampleCmpLevel(comparison, LOCATION(.25), -1, lod, STATUS_ARGS);
  else if (i == 7) v = clampTex.SampleCmpLevel(comparison, LOCATION(.25), .5, lod, STATUS_ARGS);
  else if (i == 8) return float2(tex.SampleCmpLevel(comparison, LOCATION(.25), .5, lod), 1);
  else if (i == 10) v = tex.SampleCmpLevel(borderComparison, LOCATION(-1.25), .5, lod, STATUS_ARGS);
  else if (i == 11) v = tex.SampleCmpLevel(maxComparison, LOCATION(.25), .5, lod, STATUS_ARGS);
  else if (i == 12) v = tex.SampleCmpLevel(minComparison, LOCATION(.25), .5, lod, STATUS_ARGS);
  else if (i == 16) v = tex.SampleCmpLevel(biasComparison, LOCATION(.25), .5, lod, STATUS_ARGS);
#if SHAPE == 1
  else if (i >= 17) v = tex.SampleCmpLevel(comparison, float2(.25, i == 17 ? .6 : i == 18 ? 99 : -99), .5, lod, STATUS_ARGS);
#elif SHAPE == 3
  else if (i >= 17) v = tex.SampleCmpLevel(comparison, float3(.25, .25, i == 17 ? .6 : i == 18 ? 99 : -99), .5, lod, STATUS_ARGS);
#elif SHAPE == 5
  else if (i >= 17) v = tex.SampleCmpLevel(comparison, float4(0, 0, 1, i == 17 ? .6 : i == 18 ? 99 : -99), .5, lod, STATUS_ARGS);
#endif
#if SHAPE < 4
  else if (i == 9) v = tex.SampleCmpLevel(comparison, LOCATION(.25), .5, lod, OFFSET, status);
  else if (i == 15) return float2(tex.SampleCmpLevel(comparison, LOCATION(.25), .5, lod, OFFSET), 1);
#endif
  else v = tex.SampleCmpLevel(comparison, LOCATION(.25), .5, lod, STATUS_ARGS);
  return float2(v, CheckAccessFullyMapped(status));
}
void exercise() {
  for (uint i = 0; i < 22; ++i) { float2 v = compareCase(i); result.Store2(i * 8, uint2(asuint(v.x), uint(v.y))); }
}
#endif
#endif
#ifndef LIBRARY
[numthreads(2,2,1)] void csMain(uint3 tid : SV_DispatchThreadID) {
#if defined(CMP_GRAD) || defined(CMP_BIAS)
  exercise(float(tid.x), all(tid == 0));
#else
  if (all(tid == 0)) exercise();
#endif
}
struct Vertex { float4 position : SV_Position; float2 value : TEXCOORD0; };
Vertex vsMain(uint id : SV_VertexID) {
  Vertex v; v.position = float4(id == 1 ? 3 : -1, id == 2 ? 3 : -1, 0, 1); v.value = 0;
#ifndef RAW
  if (id == 0) exercise();
#endif
  return v;
}
float4 psMain(Vertex v) : SV_Target {
#if defined(CMP_GRAD) || defined(CMP_BIAS)
  exercise(v.position.x - .5);
#else
  exercise();
#endif
  return 0;
}
Vertex nullVS(uint id : SV_VertexID) { Vertex v; v.position=float4(id==1?3:-1,id==2?3:-1,0,1); v.value=0; return v; }
float4 nullPS(Vertex v) : SV_Target { return 0; }
#ifndef RAW
[maxvertexcount(3)] void gsMain(triangle Vertex input[3], inout TriangleStream<Vertex> stream) {
  for (uint i=0;i<3;++i) { Vertex v=input[i]; v.value=compareCase(i); stream.Append(v); }
}
struct Patch { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Patch patchMain(InputPatch<Vertex,3> input) { Patch p; p.edge[0]=p.edge[1]=p.edge[2]=1; p.inside=1; return p; }
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("patchMain")]
Vertex hsMain(InputPatch<Vertex,3> input, uint id : SV_OutputControlPointID) { Vertex v=input[id]; v.value=compareCase(id); return v; }
[domain("tri")] Vertex dsMain(Patch p, float3 uv : SV_DomainLocation, const OutputPatch<Vertex,3> input) {
  Vertex v=input[0]; v.position=input[0].position*uv.x+input[1].position*uv.y+input[2].position*uv.z; v.value=compareCase(1); return v;
}
struct Payload { float2 value; };
#ifdef CMP_BIAS
  #define MESH_THREADS 2,2,1
groupshared Payload biasPayload;
#else
  #define MESH_THREADS 1,1,1
#endif
[numthreads(MESH_THREADS)] void asMain(uint3 tid : SV_GroupThreadID) { Payload p;
#ifdef CMP_BIAS
  float2 value=compareCase(1, float(tid.x));
  if (all(tid == 0)) biasPayload.value=value;
  GroupMemoryBarrierWithGroupSync();
  DispatchMesh(1,1,1,biasPayload);
#else
  p.value=compareCase(1);
  DispatchMesh(1,1,1,p);
#endif
}
[outputtopology("triangle")][numthreads(MESH_THREADS)] void msMain(uint3 tid : SV_GroupThreadID, in payload Payload p, out vertices Vertex verts[3], out indices uint3 triangles[1]) {
  SetMeshOutputCounts(3,1);
#ifdef CMP_BIAS
  if (all(tid == 0))
#endif
    triangles[0]=uint3(0,1,2);
  for(uint i=0;i<3;++i) {
#ifdef CMP_BIAS
    float2 value=p.value+compareCase(i, float(tid.x));
    if (all(tid == 0)) {
      verts[i].position=float4(i==1?3:-1,i==2?3:-1,0,1);
      verts[i].value=value;
    }
#else
    verts[i].position=float4(i==1?3:-1,i==2?3:-1,0,1);
    verts[i].value=p.value+compareCase(i);
#endif
  }
}
#endif
#endif
#ifdef LIBRARY
[shader("raygeneration")] void rayMain() { exercise(); }
struct [raypayload] RayPayload { uint value : read(caller,closesthit,miss,anyhit) : write(caller,closesthit,miss,anyhit); };
[shader("miss")] void missMain(inout RayPayload p) { exercise(); p.value=1; }
[shader("closesthit")] void closestMain(inout RayPayload p, BuiltInTriangleIntersectionAttributes a) { exercise(); p.value=2; }
[shader("anyhit")] void anyMain(inout RayPayload p, BuiltInTriangleIntersectionAttributes a) { exercise(); }
[shader("intersection")] void intersectionMain() { exercise(); }
struct CallableData { uint value; };
[shader("callable")] void callableMain(inout CallableData p) { exercise(); p.value=3; }
#endif
