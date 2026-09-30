float4 main(uint id : SV_VertexID) : SV_Position {
  float2 xy = id % 3 == 0 ? float2(-1, -1) : id % 3 == 1 ? float2(3, -1) : float2(-1, 3);
  return float4(xy, 0, 1);
}
