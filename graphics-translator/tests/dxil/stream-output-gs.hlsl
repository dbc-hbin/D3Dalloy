// owned-d3d12-stream-output.cpp GS multi-stream cases: DXBC through D3DCompile (vs/gs_5_0), DXIL frozen as
// stream-output-gs-<entry>.dxil (vs/gs_6_0, dxc -HV 2021, build-gsts.sh).
//
// Input point p (SV_VertexID) emits, as point lists (D3D11.3 13.5.2):
//   stream 0: 1 point  VALUE = (p, 0, 0, 0xa0)
//   stream 1: 2 points TEX   = (p, k)                      k = 0..1
//   stream 2: 1 point  (declared by the GS, not by the stream-output declaration)
//   stream 3: 3 points IDA = 10p + k, IDB = (10p + k + 100, 7)   k = 0..2

struct VSOut {
  float4 position : SV_Position;
  uint id : VERTEXID;
};

VSOut vs_gs(uint id : SV_VertexID) {
  VSOut o;
  o.position = float4(float(id), 0, 0, 1);
  o.id = id;
  return o;
}

struct S0 {
  float4 position : SV_Position;
  uint4 value : VALUE;
};
struct S1 {
  float2 tex : TEX;
};
struct S3 {
  uint a : IDA;
  uint2 b : IDB;
};

[maxvertexcount(7)]
void gs_multi(point VSOut input[1], inout PointStream<S0> s0, inout PointStream<S1> s1, inout PointStream<S0> s2,
              inout PointStream<S3> s3) {
  const uint p = input[0].id;
  S0 a;
  a.position = input[0].position;
  a.value = uint4(p, 0, 0, 0xa0);
  s0.Append(a);
  [unroll] for (uint k = 0; k < 2; ++k) {
    S1 b;
    b.tex = float2(p, k);
    s1.Append(b);
  }
  s2.Append(a);
  [unroll] for (uint j = 0; j < 3; ++j) {
    S3 c;
    c.a = 10 * p + j;
    c.b = uint2(10 * p + j + 100, 7);
    s3.Append(c);
  }
}
