// Shared by owned-d3d12-view-instancing.cpp, frozen as view-instancing-<entry>.dxil (6.1, dxc -HV 2021) by
// build-view-instancing.sh. Every pixel records uint4(tag, upstream, ps SV_ViewID, 0xc0de), where
// upstream = (stage << 8) | (previous stage SV_ViewID << 4) | SV_ViewID of the stage feeding the rasterizer:
// stage 1 = VS, 2 = GS (previous: VS), 3 = DS (previous: HS control point phase).

cbuffer Draw : register(b0) {
  uint tag;
};

struct VOut {
  float4 pos : SV_Position;
  nointerpolation uint up : VIEWUP;
};

float4 corner(uint vid) {
  const uint c = vid % 3;
  return float4(c == 1 ? 3.0f : -1.0f, c == 2 ? -3.0f : 1.0f, 0.0f, 1.0f);
}

VOut vs_main(uint vid : SV_VertexID, uint view : SV_ViewID) {
  VOut o;
  o.pos = corner(vid);
  o.up = 0x100u | view;
  return o;
}

uint4 ps_uint(VOut i, uint view : SV_ViewID) : SV_Target {
  return uint4(tag, i.up, view, 0xc0deu);
}

// UNORM variant (resolvable MSAA target): (tag, upstream & 0xff, ps SV_ViewID, 255) / 255.
float4 ps_unorm(VOut i, uint view : SV_ViewID) : SV_Target {
  return float4((float)tag, (float)(i.up & 0xffu), (float)view, 255.0f) / 255.0f;
}

[maxvertexcount(3)]
void gs_view(triangle VOut v[3], uint view : SV_ViewID, inout TriangleStream<VOut> stream) {
  for (uint i = 0; i < 3; ++i) {
    VOut o = v[i];
    o.up = 0x200u | ((v[i].up & 0xfu) << 4) | view;
    stream.Append(o);
  }
}

struct ControlPoint {
  float4 pos : CPPOS;
  uint up : CPVIEW;
};

ControlPoint vs_tess(uint vid : SV_VertexID) {
  ControlPoint o;
  o.pos = corner(vid);
  o.up = 0;
  return o;
}

struct PatchConstants {
  float edge[3] : SV_TessFactor;
  float inside : SV_InsideTessFactor;
};

PatchConstants hs_constants(InputPatch<ControlPoint, 3> patch) {
  PatchConstants c;
  c.edge[0] = c.edge[1] = c.edge[2] = 1.0f;
  c.inside = 1.0f;
  return c;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_constants")]
[maxtessfactor(1.0)]
ControlPoint hs_view(InputPatch<ControlPoint, 3> patch, uint i : SV_OutputControlPointID, uint view : SV_ViewID) {
  ControlPoint o = patch[i];
  o.up = view;
  return o;
}

[domain("tri")]
VOut ds_view(PatchConstants c, float3 uvw : SV_DomainLocation, const OutputPatch<ControlPoint, 3> patch,
             uint view : SV_ViewID) {
  VOut o;
  o.pos = patch[0].pos * uvw.x + patch[1].pos * uvw.y + patch[2].pos * uvw.z;
  o.up = 0x300u | ((patch[0].up & 0xfu) << 4) | view;
  return o;
}
