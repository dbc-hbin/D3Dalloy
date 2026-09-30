// Shared by owned-d3d12-geometry.cpp: DXBC through D3DCompile (vs/gs/ps_5_0), DXIL frozen as
// geometry-<entry>.dxil (vs/gs/ps_6_0, dxc -HV 2021). Ids are written as (n + 1) / 255 so the UNORM8 target stores
// exactly n + 1 (0 stays "not drawn").

cbuffer GeometryConstants : register(b0) {
  float4 gs_color;
};

struct VSOut {
  float4 position : SV_Position;
  uint id : VERTEXID;
};

// Each draw instance is moved 16 pixels right, so instanced draws land in separate halves of the target.
VSOut vs_pass(float2 position : POSITION, uint id : SV_VertexID, uint instance : SV_InstanceID) {
  VSOut o;
  o.position = float4(position.x + instance, position.y, 0, 1);
  o.id = id;
  return o;
}

struct GSOut {
  float4 position : SV_Position;
  float4 color : COLOR;
};

// Clip-space size of one pixel of the 32x32 target.
static const float kPixel = 2.0 / 32.0;

float encode(uint value) {
  return (value + 1) / 255.0;
}

// A 2x2-pixel quad (4-vertex strip) centred on `centre`, which the fixture places on a pixel corner.
void emit_quad(float4 centre, float4 color, inout TriangleStream<GSOut> stream) {
  const float2 corners[4] = {float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1)};
  [unroll] for (uint i = 0; i < 4; ++i) {
    GSOut o;
    o.position = centre + float4(corners[i] * kPixel, 0, 0);
    o.color = color;
    stream.Append(o);
  }
  stream.RestartStrip();
}

// Amplification + GEOMETRY-visible root constants: one point -> a 2x2-pixel quad.
[maxvertexcount(4)]
void gs_quad(point VSOut input[1], inout TriangleStream<GSOut> stream) {
  emit_quad(input[0].position, gs_color, stream);
}

// Triangle input (lists and strips) with instancing: GS instance i draws a quad at the first vertex moved 16 pixels
// down per instance; the colour records the other two vertices (input order), SV_PrimitiveID and SV_GSInstanceID.
// vkd3d's D3DCompile cannot emit dcl_gsinstances yet: the DXBC fixture compiles with VKD3D_NO_GS_INSTANCES and
// inserts the declaration fxc emits for [instance(2)] into the SHEX chunk itself.
#ifndef VKD3D_NO_GS_INSTANCES
[instance(2)]
#endif
[maxvertexcount(4)]
void gs_tri(triangle VSOut input[3], uint primitive : SV_PrimitiveID, uint instance : SV_GSInstanceID,
            inout TriangleStream<GSOut> stream) {
  float4 color = float4(encode(input[1].id), encode(input[2].id), encode(primitive), encode(instance));
  emit_quad(input[0].position - float4(0, instance, 0, 0), color, stream);
}

// Line input (lists and strips): a quad at the first vertex recording the second vertex and SV_PrimitiveID.
[maxvertexcount(4)]
void gs_linein(line VSOut input[2], uint primitive : SV_PrimitiveID, inout TriangleStream<GSOut> stream) {
  emit_quad(input[0].position, float4(encode(input[1].id), encode(primitive), 0, 1), stream);
}

// Line-adjacency input: a quad at vertex 1 recording vertices 0, 2, 3 and SV_PrimitiveID.
[maxvertexcount(4)]
void gs_lineadj(lineadj VSOut input[4], uint primitive : SV_PrimitiveID, inout TriangleStream<GSOut> stream) {
  float4 color = float4(encode(input[0].id), encode(input[2].id), encode(input[3].id), encode(primitive));
  emit_quad(input[1].position, color, stream);
}

// Triangle-adjacency input: a quad at vertex 0 recording the adjacent vertices 1, 3, 5, and a second quad 2 pixels
// to the right recording vertices 2, 4 and SV_PrimitiveID.
[maxvertexcount(8)]
void gs_triadj(triangleadj VSOut input[6], uint primitive : SV_PrimitiveID, inout TriangleStream<GSOut> stream) {
  emit_quad(input[0].position, float4(encode(input[1].id), encode(input[3].id), encode(input[5].id), 1), stream);
  emit_quad(input[0].position + float4(2 * kPixel, 0, 0, 0),
            float4(encode(input[2].id), encode(input[4].id), encode(primitive), 1), stream);
}

// Point input -> a horizontal line from 1.3 pixels left to 1.7 pixels right of the vertex (LineStream output): it
// covers exactly the three pixels centred at -1, 0, +1 under both the diamond-exit and centre-inclusion rules.
[maxvertexcount(2)]
void gs_line(point VSOut input[1], inout LineStream<GSOut> stream) {
  GSOut o;
  o.color = gs_color;
  o.position = input[0].position + float4(-1.3 * kPixel, 0, 0, 0);
  stream.Append(o);
  o.position = input[0].position + float4(1.7 * kPixel, 0, 0, 0);
  stream.Append(o);
}

// Point input -> PointStream output of 3 points spaced 2 pixels apart.
[maxvertexcount(3)]
void gs_points(point VSOut input[1], inout PointStream<GSOut> stream) {
  GSOut o;
  o.color = gs_color;
  [unroll] for (int i = -1; i <= 1; ++i) {
    o.position = input[0].position + float4(i * 2 * kPixel, 0, 0, 0);
    stream.Append(o);
  }
}

float4 ps_color(GSOut input) : SV_Target {
  return input.color;
}
