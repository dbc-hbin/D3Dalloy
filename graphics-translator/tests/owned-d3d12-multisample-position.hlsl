struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V edge(uint i : SV_VertexID) {
  float2 p[6] = {float2(-1,1),float2(-.25,1),float2(-1,-1),
                float2(-.25,1),float2(-.25,-1),float2(-1,-1)};
  V o; o.p=float4(p[i],.375,1); o.uv=float2((p[i].x+1)*2,(1-p[i].y)*2); return o;
}
float4 positionPS(sample float4 p : SV_Position, sample float2 uv : TEXCOORD0,
                  uint s : SV_SampleIndex) : SV_Target {
  return float4(p.xy,uv.xy);
}
float positionDepthPS(sample float4 p : SV_Position, sample float2 uv : TEXCOORD0,
                      uint s : SV_SampleIndex) : SV_Depth {
  return .125 + p.x/16 + p.y/64 + uv.x/16 + uv.y/64;
}
