// Mandatory 32-row MS output boundaries. Every lane is consumed, including raw integer flat bits.
struct Contract124 {
  float4 pos : SV_Position;
  float4 data[27] : CONTRACT;
  nointerpolation uint4 flat : FLAT;
  float4 perspective : PERSPECTIVE;
  noperspective float4 affine : AFFINE;
};

#define CONTRACT_FIELDS \
  float4 pos : SV_Position; \
  float4 data[27] : CONTRACT; \
  nointerpolation uint4 flat : FLAT; \
  float4 perspective : PERSPECTIVE; \
  noperspective float4 affine : AFFINE;
struct Contract125 { CONTRACT_FIELDS float tail : TAIL; };
struct Contract128Perspective { CONTRACT_FIELDS float4 tail : TAIL; };
struct Contract128Affine { CONTRACT_FIELDS noperspective float4 tail : TAIL; };
struct Contract128Flat { CONTRACT_FIELDS nointerpolation uint4 tail : TAIL; };

float4 contractPosition(uint v) {
  const float w = v == 0 ? 1 : v == 1 ? 2 : 4;
  const float2 pixel = v == 0 ? float2(0, 0) : v == 1 ? float2(128, 0) : float2(0, 128);
  return pixelToNdc(pixel, float2(64,64)) * w;
}
uint4 contractFlat(uint v) { return uint4(0x3f800001, 0x80000001, 0x7f7fffff, 0xffffffff) ^ v; }
#define CONTRACT_STORES \
  SetMeshOutputCounts(3, 1); \
  if (tid < 3) { \
    verts[tid].pos = contractPosition(tid); \
    for (uint r = 0; r < 27; ++r) verts[tid].data[r] = float4(r * 4 + 1, r * 4 + 2, r * 4 + 3, r * 4 + 4); \
    verts[tid].flat = contractFlat(tid); \
    verts[tid].perspective = float4(tid * 16, tid * 16 + 1, tid * 16 + 2, tid * 16 + 3); \
    verts[tid].affine = float4(tid * 16, tid * 16 + 1, tid * 16 + 2, tid * 16 + 3); \
  } \
  if (tid == 0) tris[0] = uint3(2,0,1);

[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract124(uint tid:SV_GroupIndex, out vertices Contract124 verts[3], out indices uint3 tris[1]) {
  CONTRACT_STORES
}
[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract125(uint tid:SV_GroupIndex, out vertices Contract125 verts[3], out indices uint3 tris[1]) {
  CONTRACT_STORES
  if (tid < 3) verts[tid].tail = tid * 16;
}
[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract128_perspective(uint tid:SV_GroupIndex, out vertices Contract128Perspective verts[3], out indices uint3 tris[1]) {
  CONTRACT_STORES
  if (tid < 3) verts[tid].tail = float4(tid * 16, tid * 16 + 1, tid * 16 + 2, tid * 16 + 3);
}
[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract128_affine(uint tid:SV_GroupIndex, out vertices Contract128Affine verts[3], out indices uint3 tris[1]) {
  CONTRACT_STORES
  if (tid < 3) verts[tid].tail = float4(tid * 16, tid * 16 + 1, tid * 16 + 2, tid * 16 + 3);
}
[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract128_flat(uint tid:SV_GroupIndex, out vertices Contract128Flat verts[3], out indices uint3 tris[1]) {
  CONTRACT_STORES
  if (tid < 3) verts[tid].tail = contractFlat(tid);
}

float2 contractExpected(float4 position) {
  float2 lambda = position.xy / 128;
  float denominator = 1 - lambda.x - lambda.y + lambda.x / 2 + lambda.y / 4;
  return float2((lambda.x * 8 + lambda.y * 8) / denominator, lambda.x * 16 + lambda.y * 32);
}
#define CONTRACT_CHECKS \
  bool valid = true; \
  for (uint r = 0; r < 27; ++r) valid &= all(abs(v.data[r] - float4(r*4+1,r*4+2,r*4+3,r*4+4)) < 0.0005); \
  valid &= all(v.flat == contractFlat(2)); \
  float2 expected = contractExpected(v.pos); \
  valid &= all(abs(v.perspective - (expected.x + float4(0,1,2,3))) < 0.0005); \
  valid &= all(abs(v.affine - (expected.y + float4(0,1,2,3))) < 0.0005);
float4 ps_contract124(Contract124 v) : SV_Target { CONTRACT_CHECKS return valid ? float4(0,1,0,1) : float4(1,0,0,1); }
float4 ps_contract125(Contract125 v) : SV_Target { CONTRACT_CHECKS valid &= abs(v.tail-expected.x)<0.0005; return valid ? float4(0,1,0,1) : float4(1,0,0,1); }
float4 ps_contract128_perspective(Contract128Perspective v) : SV_Target { CONTRACT_CHECKS valid &= all(abs(v.tail-(expected.x+float4(0,1,2,3)))<0.0005); return valid ? float4(0,1,0,1) : float4(1,0,0,1); }
float4 ps_contract128_affine(Contract128Affine v) : SV_Target { CONTRACT_CHECKS valid &= all(abs(v.tail-(expected.y+float4(0,1,2,3)))<0.0005); return valid ? float4(0,1,0,1) : float4(1,0,0,1); }
float4 ps_contract128_flat(Contract128Flat v) : SV_Target { CONTRACT_CHECKS valid &= all(v.tail==contractFlat(2)); return valid ? float4(0,1,0,1) : float4(1,0,0,1); }

// The last legal row is captured: exercise every finite pull mode and its derivatives.
#define CONTRACT_PULL_CHECKS(axis) \
  float4 atCentroid = EvaluateAttributeCentroid(v.tail); \
  float4 atSample = EvaluateAttributeAtSample(v.tail, 0); \
  float4 snapped = EvaluateAttributeSnapped(v.tail, int2(3,-5)); \
  float4 samplePosition = v.pos; \
  samplePosition.xy -= k.y != 0 ? float2(0.5,0.5) : float2(0,0); \
  samplePosition.xy += k.x == 4 ? float2(-2,-6)/16 : float2(0,0); \
  float4 snappedPosition = v.pos; snappedPosition.xy += float2(3,-5)/16; \
  float4 centroidPosition = k.y != 0 ? samplePosition : v.pos; \
  float ce = contractExpected(centroidPosition).axis; \
  float se = contractExpected(samplePosition).axis; \
  float ne = contractExpected(snappedPosition).axis; \
  uint invalid = valid ? 0 : 128; \
  if (!all(abs(atCentroid-(ce+float4(0,1,2,3)))<0.0005)) invalid |= 1; \
  if (!all(abs(atSample-(se+float4(0,1,2,3)))<0.0005)) invalid |= 2; \
  if (!all(abs(snapped-(ne+float4(0,1,2,3)))<0.0005)) invalid |= 4; \
  if (!(abs(ddx(v.tail.x)-ddx(expected.axis))<0.0005 && abs(ddy(v.tail.x)-ddy(expected.axis))<0.0005)) invalid |= 8; \
  if (!(abs(ddx(atCentroid.x)-ddx(ce))<0.0005 && abs(ddy(atCentroid.x)-ddy(ce))<0.0005)) invalid |= 16; \
  if (!(abs(ddx(atSample.x)-ddx(se))<0.0005 && abs(ddy(atSample.x)-ddy(se))<0.0005)) invalid |= 32; \
  if (!(abs(ddx(snapped.x)-ddx(ne))<0.0005 && abs(ddy(snapped.x)-ddy(ne))<0.0005)) invalid |= 64;
float4 ps_contract128_pull_perspective(Contract128Perspective v, uint coverage:SV_Coverage) : SV_Target {
  CONTRACT_CHECKS
  valid &= coverage == ((1u << (k.x * (k.y != 0 ? 4 : 1))) - 1);
  CONTRACT_PULL_CHECKS(x)
  return invalid == 0 ? float4(0,1,0,1) : float4(invalid/255.0,0,0,1);
}
float4 ps_contract128_pull_affine(Contract128Affine v, uint coverage:SV_Coverage) : SV_Target {
  CONTRACT_CHECKS
  valid &= coverage == ((1u << (k.x * (k.y != 0 ? 4 : 1))) - 1);
  CONTRACT_PULL_CHECKS(y)
  return invalid == 0 ? float4(0,1,0,1) : float4(invalid/255.0,0,0,1);
}

// 128 scalars * 64 declared vertices = the complete 32KB D3D output budget,
// alongside the mandatory 28KB MS application threadgroup budget (DXIL validator).
groupshared uint contractShared[7168];
[outputtopology("triangle")][numthreads(128,1,1)]
void ms_contract128_budget(uint tid:SV_GroupIndex, out vertices Contract128Perspective verts[64], out indices uint3 tris[1]) {
  for (uint i = tid; i < 7168; i += 128) contractShared[i] = i + 1 + k.w;
  GroupMemoryBarrierWithGroupSync();
  CONTRACT_STORES
  if (tid < 3) {
    uint sum = 0;
    for (uint i = 0; i < 7168; ++i) sum += contractShared[(i*73)%7168];
    verts[tid].tail = float4(tid*16,tid*16+1,tid*16+2,tid*16+3) + float(sum - 25693696);
  }
}

// The mandatory row/scalar limit is shared by vertex and primitive signatures.
struct Contract128SplitVertex { float4 pos:SV_Position; float4 data[15]:CONTRACT; };
struct Contract128SplitPrimitive { nointerpolation uint4 primitive[16]:PRIMITIVE; };
struct Contract128SplitPixel {
  float4 pos:SV_Position;
  float4 data[15]:CONTRACT;
  nointerpolation uint4 primitive[16]:PRIMITIVE;
};
[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract128_split(uint tid:SV_GroupIndex, out vertices Contract128SplitVertex verts[3],
                          out indices uint3 tris[1], out primitives Contract128SplitPrimitive prim[1]) {
  SetMeshOutputCounts(3,1);
  if (tid<3) {
    verts[tid].pos=contractPosition(tid);
    for(uint r=0;r<15;++r) verts[tid].data[r]=float4(r*4+1,r*4+2,r*4+3,r*4+4);
  }
  if(tid==0) {
    tris[0]=uint3(2,0,1);
    for(uint r=0;r<16;++r) prim[0].primitive[r]=contractFlat(r);
  }
}
float4 ps_contract128_split(Contract128SplitPixel v):SV_Target {
  bool valid=true;
  for(uint r=0;r<15;++r) valid &= all(abs(v.data[r]-float4(r*4+1,r*4+2,r*4+3,r*4+4))<0.0005);
  for(uint r=0;r<16;++r) valid &= all(v.primitive[r]==contractFlat(r));
  return valid?float4(0,1,0,1):float4(1,0,0,1);
}

struct ContractCull {
  float4 pos : SV_Position;
  float4 color : COLOR0;
  float4 distance0 : SV_CullDistance0;
  float4 distance1 : SV_CullDistance1;
};
[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract_cull(uint tid:SV_GroupIndex, out vertices ContractCull verts[3], out indices uint3 tris[1], out primitives PCull prim[1]) {
  SetMeshOutputCounts(3,1);
  if (tid < 3) {
    verts[tid].pos = contractPosition(tid);
    verts[tid].color = float4(0,1,0,1);
    verts[tid].distance0 = float4(1,2,3,4);
    verts[tid].distance1 = float4(5,6,7,8);
    if (k.x == 0) verts[tid].distance1.w = -1; // one common negative plane culls
    if (k.x == 1) verts[tid].distance1.w = tid == 1 ? 1 : -1; // not all vertices: survives, not clipped
    if (k.x == 2) verts[tid].distance1.w = tid == 1 ? 0 : -1; // zero survives
    if (k.x == 3) verts[tid].distance1.w = tid == 1 ? asfloat(0x7fc01234) : -1; // ordered negative false
    if (k.x == 4) verts[tid].distance0 = tid == 0 ? float4(-1,1,1,1) : tid == 1 ? float4(1,-1,1,1) : float4(1,1,-1,1);
  }
  if (tid == 0) { tris[0] = uint3(2,0,1); prim[0].cull = k.x == 5; }
}
[outputtopology("line")][numthreads(4,1,1)]
void ms_contract_cull_line(uint tid:SV_GroupIndex, out vertices ContractCull verts[2], out indices uint2 lines[1], out primitives PCull prim[1]) {
  SetMeshOutputCounts(2,1);
  if (tid<2) {
    verts[tid].pos=pixelToNdc(float2(tid*64,16.5),float2(64,64));
    verts[tid].color=float4(0,1,0,1);
    verts[tid].distance0=float4(1,2,3,4);
    verts[tid].distance1=float4(5,6,7,8);
    if(k.x==0) verts[tid].distance1.w=-1;
    if(k.x==1) verts[tid].distance1.w=tid==1?1:-1;
    if(k.x==2) verts[tid].distance1.w=tid==1?0:-1;
    if(k.x==3) verts[tid].distance1.w=tid==1?asfloat(0x7fc01234):-1;
    if(k.x==4) verts[tid].distance0=tid==0?float4(-1,1,1,1):float4(1,-1,1,1);
  }
  if(tid==0) { lines[0]=uint2(1,0); prim[0].cull=k.x==5; }
}
float4 ps_contract_cull(ContractCull v) : SV_Target {
  bool valid = all(abs(v.distance0 - float4(1,2,3,4)) < 0.0005) && all(abs(v.distance1.xyz-float3(5,6,7))<0.0005);
  if (k.x == 3) valid &= isnan(v.distance1.w);
  if (k.x == 4) valid = true;
  return valid ? float4(0,1,0,1) : float4(1,0,0,1);
}

// Full 128-scalar signature packed from 124 independent user semantics.
struct Contract128Scalars {
  float4 pos : SV_Position;
  float a0 : SCALAR0;
  float a1 : SCALAR1;
  float a2 : SCALAR2;
  float a3 : SCALAR3;
  float a4 : SCALAR4;
  float a5 : SCALAR5;
  float a6 : SCALAR6;
  float a7 : SCALAR7;
  float a8 : SCALAR8;
  float a9 : SCALAR9;
  float a10 : SCALAR10;
  float a11 : SCALAR11;
  float a12 : SCALAR12;
  float a13 : SCALAR13;
  float a14 : SCALAR14;
  float a15 : SCALAR15;
  float a16 : SCALAR16;
  float a17 : SCALAR17;
  float a18 : SCALAR18;
  float a19 : SCALAR19;
  float a20 : SCALAR20;
  float a21 : SCALAR21;
  float a22 : SCALAR22;
  float a23 : SCALAR23;
  float a24 : SCALAR24;
  float a25 : SCALAR25;
  float a26 : SCALAR26;
  float a27 : SCALAR27;
  float a28 : SCALAR28;
  float a29 : SCALAR29;
  float a30 : SCALAR30;
  float a31 : SCALAR31;
  float a32 : SCALAR32;
  float a33 : SCALAR33;
  float a34 : SCALAR34;
  float a35 : SCALAR35;
  float a36 : SCALAR36;
  float a37 : SCALAR37;
  float a38 : SCALAR38;
  float a39 : SCALAR39;
  float a40 : SCALAR40;
  float a41 : SCALAR41;
  float a42 : SCALAR42;
  float a43 : SCALAR43;
  float a44 : SCALAR44;
  float a45 : SCALAR45;
  float a46 : SCALAR46;
  float a47 : SCALAR47;
  float a48 : SCALAR48;
  float a49 : SCALAR49;
  float a50 : SCALAR50;
  float a51 : SCALAR51;
  float a52 : SCALAR52;
  float a53 : SCALAR53;
  float a54 : SCALAR54;
  float a55 : SCALAR55;
  float a56 : SCALAR56;
  float a57 : SCALAR57;
  float a58 : SCALAR58;
  float a59 : SCALAR59;
  float a60 : SCALAR60;
  float a61 : SCALAR61;
  float a62 : SCALAR62;
  float a63 : SCALAR63;
  float a64 : SCALAR64;
  float a65 : SCALAR65;
  float a66 : SCALAR66;
  float a67 : SCALAR67;
  float a68 : SCALAR68;
  float a69 : SCALAR69;
  float a70 : SCALAR70;
  float a71 : SCALAR71;
  float a72 : SCALAR72;
  float a73 : SCALAR73;
  float a74 : SCALAR74;
  float a75 : SCALAR75;
  float a76 : SCALAR76;
  float a77 : SCALAR77;
  float a78 : SCALAR78;
  float a79 : SCALAR79;
  float a80 : SCALAR80;
  float a81 : SCALAR81;
  float a82 : SCALAR82;
  float a83 : SCALAR83;
  float a84 : SCALAR84;
  float a85 : SCALAR85;
  float a86 : SCALAR86;
  float a87 : SCALAR87;
  float a88 : SCALAR88;
  float a89 : SCALAR89;
  float a90 : SCALAR90;
  float a91 : SCALAR91;
  float a92 : SCALAR92;
  float a93 : SCALAR93;
  float a94 : SCALAR94;
  float a95 : SCALAR95;
  float a96 : SCALAR96;
  float a97 : SCALAR97;
  float a98 : SCALAR98;
  float a99 : SCALAR99;
  float a100 : SCALAR100;
  float a101 : SCALAR101;
  float a102 : SCALAR102;
  float a103 : SCALAR103;
  float a104 : SCALAR104;
  float a105 : SCALAR105;
  float a106 : SCALAR106;
  float a107 : SCALAR107;
  float a108 : SCALAR108;
  float a109 : SCALAR109;
  float a110 : SCALAR110;
  float a111 : SCALAR111;
  float a112 : SCALAR112;
  float a113 : SCALAR113;
  float a114 : SCALAR114;
  float a115 : SCALAR115;
  float a116 : SCALAR116;
  float a117 : SCALAR117;
  float a118 : SCALAR118;
  float a119 : SCALAR119;
  float a120 : SCALAR120;
  float a121 : SCALAR121;
  float a122 : SCALAR122;
  float a123 : SCALAR123;
};
[outputtopology("triangle")][numthreads(4,1,1)]
void ms_contract128_scalars(uint tid:SV_GroupIndex, out vertices Contract128Scalars verts[3], out indices uint3 tris[1]) {
  SetMeshOutputCounts(3,1);
  if (tid < 3) {
    verts[tid].pos = contractPosition(tid);
    verts[tid].a0 = 0.25;
    verts[tid].a1 = 1.25;
    verts[tid].a2 = 2.25;
    verts[tid].a3 = 3.25;
    verts[tid].a4 = 4.25;
    verts[tid].a5 = 5.25;
    verts[tid].a6 = 6.25;
    verts[tid].a7 = 7.25;
    verts[tid].a8 = 8.25;
    verts[tid].a9 = 9.25;
    verts[tid].a10 = 10.25;
    verts[tid].a11 = 11.25;
    verts[tid].a12 = 12.25;
    verts[tid].a13 = 13.25;
    verts[tid].a14 = 14.25;
    verts[tid].a15 = 15.25;
    verts[tid].a16 = 16.25;
    verts[tid].a17 = 17.25;
    verts[tid].a18 = 18.25;
    verts[tid].a19 = 19.25;
    verts[tid].a20 = 20.25;
    verts[tid].a21 = 21.25;
    verts[tid].a22 = 22.25;
    verts[tid].a23 = 23.25;
    verts[tid].a24 = 24.25;
    verts[tid].a25 = 25.25;
    verts[tid].a26 = 26.25;
    verts[tid].a27 = 27.25;
    verts[tid].a28 = 28.25;
    verts[tid].a29 = 29.25;
    verts[tid].a30 = 30.25;
    verts[tid].a31 = 31.25;
    verts[tid].a32 = 32.25;
    verts[tid].a33 = 33.25;
    verts[tid].a34 = 34.25;
    verts[tid].a35 = 35.25;
    verts[tid].a36 = 36.25;
    verts[tid].a37 = 37.25;
    verts[tid].a38 = 38.25;
    verts[tid].a39 = 39.25;
    verts[tid].a40 = 40.25;
    verts[tid].a41 = 41.25;
    verts[tid].a42 = 42.25;
    verts[tid].a43 = 43.25;
    verts[tid].a44 = 44.25;
    verts[tid].a45 = 45.25;
    verts[tid].a46 = 46.25;
    verts[tid].a47 = 47.25;
    verts[tid].a48 = 48.25;
    verts[tid].a49 = 49.25;
    verts[tid].a50 = 50.25;
    verts[tid].a51 = 51.25;
    verts[tid].a52 = 52.25;
    verts[tid].a53 = 53.25;
    verts[tid].a54 = 54.25;
    verts[tid].a55 = 55.25;
    verts[tid].a56 = 56.25;
    verts[tid].a57 = 57.25;
    verts[tid].a58 = 58.25;
    verts[tid].a59 = 59.25;
    verts[tid].a60 = 60.25;
    verts[tid].a61 = 61.25;
    verts[tid].a62 = 62.25;
    verts[tid].a63 = 63.25;
    verts[tid].a64 = 64.25;
    verts[tid].a65 = 65.25;
    verts[tid].a66 = 66.25;
    verts[tid].a67 = 67.25;
    verts[tid].a68 = 68.25;
    verts[tid].a69 = 69.25;
    verts[tid].a70 = 70.25;
    verts[tid].a71 = 71.25;
    verts[tid].a72 = 72.25;
    verts[tid].a73 = 73.25;
    verts[tid].a74 = 74.25;
    verts[tid].a75 = 75.25;
    verts[tid].a76 = 76.25;
    verts[tid].a77 = 77.25;
    verts[tid].a78 = 78.25;
    verts[tid].a79 = 79.25;
    verts[tid].a80 = 80.25;
    verts[tid].a81 = 81.25;
    verts[tid].a82 = 82.25;
    verts[tid].a83 = 83.25;
    verts[tid].a84 = 84.25;
    verts[tid].a85 = 85.25;
    verts[tid].a86 = 86.25;
    verts[tid].a87 = 87.25;
    verts[tid].a88 = 88.25;
    verts[tid].a89 = 89.25;
    verts[tid].a90 = 90.25;
    verts[tid].a91 = 91.25;
    verts[tid].a92 = 92.25;
    verts[tid].a93 = 93.25;
    verts[tid].a94 = 94.25;
    verts[tid].a95 = 95.25;
    verts[tid].a96 = 96.25;
    verts[tid].a97 = 97.25;
    verts[tid].a98 = 98.25;
    verts[tid].a99 = 99.25;
    verts[tid].a100 = 100.25;
    verts[tid].a101 = 101.25;
    verts[tid].a102 = 102.25;
    verts[tid].a103 = 103.25;
    verts[tid].a104 = 104.25;
    verts[tid].a105 = 105.25;
    verts[tid].a106 = 106.25;
    verts[tid].a107 = 107.25;
    verts[tid].a108 = 108.25;
    verts[tid].a109 = 109.25;
    verts[tid].a110 = 110.25;
    verts[tid].a111 = 111.25;
    verts[tid].a112 = 112.25;
    verts[tid].a113 = 113.25;
    verts[tid].a114 = 114.25;
    verts[tid].a115 = 115.25;
    verts[tid].a116 = 116.25;
    verts[tid].a117 = 117.25;
    verts[tid].a118 = 118.25;
    verts[tid].a119 = 119.25;
    verts[tid].a120 = 120.25;
    verts[tid].a121 = 121.25;
    verts[tid].a122 = 122.25;
    verts[tid].a123 = 123.25;
  }
  if (tid == 0) tris[0] = uint3(2,0,1);
}
float4 ps_contract128_scalars(Contract128Scalars v) : SV_Target {
  uint invalid = 255;
  if (!(abs(v.a0 - 0.25) < 0.0005)) invalid = min(invalid, 0);
  if (!(abs(v.a1 - 1.25) < 0.0005)) invalid = min(invalid, 1);
  if (!(abs(v.a2 - 2.25) < 0.0005)) invalid = min(invalid, 2);
  if (!(abs(v.a3 - 3.25) < 0.0005)) invalid = min(invalid, 3);
  if (!(abs(v.a4 - 4.25) < 0.0005)) invalid = min(invalid, 4);
  if (!(abs(v.a5 - 5.25) < 0.0005)) invalid = min(invalid, 5);
  if (!(abs(v.a6 - 6.25) < 0.0005)) invalid = min(invalid, 6);
  if (!(abs(v.a7 - 7.25) < 0.0005)) invalid = min(invalid, 7);
  if (!(abs(v.a8 - 8.25) < 0.0005)) invalid = min(invalid, 8);
  if (!(abs(v.a9 - 9.25) < 0.0005)) invalid = min(invalid, 9);
  if (!(abs(v.a10 - 10.25) < 0.0005)) invalid = min(invalid, 10);
  if (!(abs(v.a11 - 11.25) < 0.0005)) invalid = min(invalid, 11);
  if (!(abs(v.a12 - 12.25) < 0.0005)) invalid = min(invalid, 12);
  if (!(abs(v.a13 - 13.25) < 0.0005)) invalid = min(invalid, 13);
  if (!(abs(v.a14 - 14.25) < 0.0005)) invalid = min(invalid, 14);
  if (!(abs(v.a15 - 15.25) < 0.0005)) invalid = min(invalid, 15);
  if (!(abs(v.a16 - 16.25) < 0.0005)) invalid = min(invalid, 16);
  if (!(abs(v.a17 - 17.25) < 0.0005)) invalid = min(invalid, 17);
  if (!(abs(v.a18 - 18.25) < 0.0005)) invalid = min(invalid, 18);
  if (!(abs(v.a19 - 19.25) < 0.0005)) invalid = min(invalid, 19);
  if (!(abs(v.a20 - 20.25) < 0.0005)) invalid = min(invalid, 20);
  if (!(abs(v.a21 - 21.25) < 0.0005)) invalid = min(invalid, 21);
  if (!(abs(v.a22 - 22.25) < 0.0005)) invalid = min(invalid, 22);
  if (!(abs(v.a23 - 23.25) < 0.0005)) invalid = min(invalid, 23);
  if (!(abs(v.a24 - 24.25) < 0.0005)) invalid = min(invalid, 24);
  if (!(abs(v.a25 - 25.25) < 0.0005)) invalid = min(invalid, 25);
  if (!(abs(v.a26 - 26.25) < 0.0005)) invalid = min(invalid, 26);
  if (!(abs(v.a27 - 27.25) < 0.0005)) invalid = min(invalid, 27);
  if (!(abs(v.a28 - 28.25) < 0.0005)) invalid = min(invalid, 28);
  if (!(abs(v.a29 - 29.25) < 0.0005)) invalid = min(invalid, 29);
  if (!(abs(v.a30 - 30.25) < 0.0005)) invalid = min(invalid, 30);
  if (!(abs(v.a31 - 31.25) < 0.0005)) invalid = min(invalid, 31);
  if (!(abs(v.a32 - 32.25) < 0.0005)) invalid = min(invalid, 32);
  if (!(abs(v.a33 - 33.25) < 0.0005)) invalid = min(invalid, 33);
  if (!(abs(v.a34 - 34.25) < 0.0005)) invalid = min(invalid, 34);
  if (!(abs(v.a35 - 35.25) < 0.0005)) invalid = min(invalid, 35);
  if (!(abs(v.a36 - 36.25) < 0.0005)) invalid = min(invalid, 36);
  if (!(abs(v.a37 - 37.25) < 0.0005)) invalid = min(invalid, 37);
  if (!(abs(v.a38 - 38.25) < 0.0005)) invalid = min(invalid, 38);
  if (!(abs(v.a39 - 39.25) < 0.0005)) invalid = min(invalid, 39);
  if (!(abs(v.a40 - 40.25) < 0.0005)) invalid = min(invalid, 40);
  if (!(abs(v.a41 - 41.25) < 0.0005)) invalid = min(invalid, 41);
  if (!(abs(v.a42 - 42.25) < 0.0005)) invalid = min(invalid, 42);
  if (!(abs(v.a43 - 43.25) < 0.0005)) invalid = min(invalid, 43);
  if (!(abs(v.a44 - 44.25) < 0.0005)) invalid = min(invalid, 44);
  if (!(abs(v.a45 - 45.25) < 0.0005)) invalid = min(invalid, 45);
  if (!(abs(v.a46 - 46.25) < 0.0005)) invalid = min(invalid, 46);
  if (!(abs(v.a47 - 47.25) < 0.0005)) invalid = min(invalid, 47);
  if (!(abs(v.a48 - 48.25) < 0.0005)) invalid = min(invalid, 48);
  if (!(abs(v.a49 - 49.25) < 0.0005)) invalid = min(invalid, 49);
  if (!(abs(v.a50 - 50.25) < 0.0005)) invalid = min(invalid, 50);
  if (!(abs(v.a51 - 51.25) < 0.0005)) invalid = min(invalid, 51);
  if (!(abs(v.a52 - 52.25) < 0.0005)) invalid = min(invalid, 52);
  if (!(abs(v.a53 - 53.25) < 0.0005)) invalid = min(invalid, 53);
  if (!(abs(v.a54 - 54.25) < 0.0005)) invalid = min(invalid, 54);
  if (!(abs(v.a55 - 55.25) < 0.0005)) invalid = min(invalid, 55);
  if (!(abs(v.a56 - 56.25) < 0.0005)) invalid = min(invalid, 56);
  if (!(abs(v.a57 - 57.25) < 0.0005)) invalid = min(invalid, 57);
  if (!(abs(v.a58 - 58.25) < 0.0005)) invalid = min(invalid, 58);
  if (!(abs(v.a59 - 59.25) < 0.0005)) invalid = min(invalid, 59);
  if (!(abs(v.a60 - 60.25) < 0.0005)) invalid = min(invalid, 60);
  if (!(abs(v.a61 - 61.25) < 0.0005)) invalid = min(invalid, 61);
  if (!(abs(v.a62 - 62.25) < 0.0005)) invalid = min(invalid, 62);
  if (!(abs(v.a63 - 63.25) < 0.0005)) invalid = min(invalid, 63);
  if (!(abs(v.a64 - 64.25) < 0.0005)) invalid = min(invalid, 64);
  if (!(abs(v.a65 - 65.25) < 0.0005)) invalid = min(invalid, 65);
  if (!(abs(v.a66 - 66.25) < 0.0005)) invalid = min(invalid, 66);
  if (!(abs(v.a67 - 67.25) < 0.0005)) invalid = min(invalid, 67);
  if (!(abs(v.a68 - 68.25) < 0.0005)) invalid = min(invalid, 68);
  if (!(abs(v.a69 - 69.25) < 0.0005)) invalid = min(invalid, 69);
  if (!(abs(v.a70 - 70.25) < 0.0005)) invalid = min(invalid, 70);
  if (!(abs(v.a71 - 71.25) < 0.0005)) invalid = min(invalid, 71);
  if (!(abs(v.a72 - 72.25) < 0.0005)) invalid = min(invalid, 72);
  if (!(abs(v.a73 - 73.25) < 0.0005)) invalid = min(invalid, 73);
  if (!(abs(v.a74 - 74.25) < 0.0005)) invalid = min(invalid, 74);
  if (!(abs(v.a75 - 75.25) < 0.0005)) invalid = min(invalid, 75);
  if (!(abs(v.a76 - 76.25) < 0.0005)) invalid = min(invalid, 76);
  if (!(abs(v.a77 - 77.25) < 0.0005)) invalid = min(invalid, 77);
  if (!(abs(v.a78 - 78.25) < 0.0005)) invalid = min(invalid, 78);
  if (!(abs(v.a79 - 79.25) < 0.0005)) invalid = min(invalid, 79);
  if (!(abs(v.a80 - 80.25) < 0.0005)) invalid = min(invalid, 80);
  if (!(abs(v.a81 - 81.25) < 0.0005)) invalid = min(invalid, 81);
  if (!(abs(v.a82 - 82.25) < 0.0005)) invalid = min(invalid, 82);
  if (!(abs(v.a83 - 83.25) < 0.0005)) invalid = min(invalid, 83);
  if (!(abs(v.a84 - 84.25) < 0.0005)) invalid = min(invalid, 84);
  if (!(abs(v.a85 - 85.25) < 0.0005)) invalid = min(invalid, 85);
  if (!(abs(v.a86 - 86.25) < 0.0005)) invalid = min(invalid, 86);
  if (!(abs(v.a87 - 87.25) < 0.0005)) invalid = min(invalid, 87);
  if (!(abs(v.a88 - 88.25) < 0.0005)) invalid = min(invalid, 88);
  if (!(abs(v.a89 - 89.25) < 0.0005)) invalid = min(invalid, 89);
  if (!(abs(v.a90 - 90.25) < 0.0005)) invalid = min(invalid, 90);
  if (!(abs(v.a91 - 91.25) < 0.0005)) invalid = min(invalid, 91);
  if (!(abs(v.a92 - 92.25) < 0.0005)) invalid = min(invalid, 92);
  if (!(abs(v.a93 - 93.25) < 0.0005)) invalid = min(invalid, 93);
  if (!(abs(v.a94 - 94.25) < 0.0005)) invalid = min(invalid, 94);
  if (!(abs(v.a95 - 95.25) < 0.0005)) invalid = min(invalid, 95);
  if (!(abs(v.a96 - 96.25) < 0.0005)) invalid = min(invalid, 96);
  if (!(abs(v.a97 - 97.25) < 0.0005)) invalid = min(invalid, 97);
  if (!(abs(v.a98 - 98.25) < 0.0005)) invalid = min(invalid, 98);
  if (!(abs(v.a99 - 99.25) < 0.0005)) invalid = min(invalid, 99);
  if (!(abs(v.a100 - 100.25) < 0.0005)) invalid = min(invalid, 100);
  if (!(abs(v.a101 - 101.25) < 0.0005)) invalid = min(invalid, 101);
  if (!(abs(v.a102 - 102.25) < 0.0005)) invalid = min(invalid, 102);
  if (!(abs(v.a103 - 103.25) < 0.0005)) invalid = min(invalid, 103);
  if (!(abs(v.a104 - 104.25) < 0.0005)) invalid = min(invalid, 104);
  if (!(abs(v.a105 - 105.25) < 0.0005)) invalid = min(invalid, 105);
  if (!(abs(v.a106 - 106.25) < 0.0005)) invalid = min(invalid, 106);
  if (!(abs(v.a107 - 107.25) < 0.0005)) invalid = min(invalid, 107);
  if (!(abs(v.a108 - 108.25) < 0.0005)) invalid = min(invalid, 108);
  if (!(abs(v.a109 - 109.25) < 0.0005)) invalid = min(invalid, 109);
  if (!(abs(v.a110 - 110.25) < 0.0005)) invalid = min(invalid, 110);
  if (!(abs(v.a111 - 111.25) < 0.0005)) invalid = min(invalid, 111);
  if (!(abs(v.a112 - 112.25) < 0.0005)) invalid = min(invalid, 112);
  if (!(abs(v.a113 - 113.25) < 0.0005)) invalid = min(invalid, 113);
  if (!(abs(v.a114 - 114.25) < 0.0005)) invalid = min(invalid, 114);
  if (!(abs(v.a115 - 115.25) < 0.0005)) invalid = min(invalid, 115);
  if (!(abs(v.a116 - 116.25) < 0.0005)) invalid = min(invalid, 116);
  if (!(abs(v.a117 - 117.25) < 0.0005)) invalid = min(invalid, 117);
  if (!(abs(v.a118 - 118.25) < 0.0005)) invalid = min(invalid, 118);
  if (!(abs(v.a119 - 119.25) < 0.0005)) invalid = min(invalid, 119);
  if (!(abs(v.a120 - 120.25) < 0.0005)) invalid = min(invalid, 120);
  if (!(abs(v.a121 - 121.25) < 0.0005)) invalid = min(invalid, 121);
  if (!(abs(v.a122 - 122.25) < 0.0005)) invalid = min(invalid, 122);
  if (!(abs(v.a123 - 123.25) < 0.0005)) invalid = min(invalid, 123);
  return invalid == 255 ? float4(0,1,0,1) : float4(invalid / 255.0,0,0,1);
}

