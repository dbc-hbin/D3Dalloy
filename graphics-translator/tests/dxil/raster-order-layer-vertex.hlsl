struct Output {
  float4 position : SV_Position;
  uint layer : SV_RenderTargetArrayIndex;
};
Output main(uint vertex : SV_VertexID) {
  uint primitive = vertex / 3u;
  uint corner = vertex % 3u;
  Output result;
  result.position = float4(corner == 1u ? 3.0 : -1.0,
                           corner == 2u ? 3.0 : -1.0, 0.0, 1.0);
  result.layer = primitive / 3u;
  return result;
}
