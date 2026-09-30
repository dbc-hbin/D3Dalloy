cbuffer DrawOffset : register(b0) { float2 offsetPixels; };

float2 positionForID(uint id) {
  // UINT32_MAX is a valid vertex ID when strip cut is disabled. Never fetch a VB at that address.
  if (id == 0xffffffffu) return float2(12.5, 13.5);
  if (id == 0xffffu) return float2(3.5, 13.5);
  uint slot = id % 4u;
  if (slot == 0u) return float2(3.5, 3.5);
  if (slot == 1u) return float2(12.5, 3.5);
  if (slot == 2u) return float2(3.5, 12.5);
  return float2(12.5, 12.5);
}

float4 clipPosition(float2 pixel) {
  return float4(pixel.x / 8.0 - 1.0, 1.0 - pixel.y / 8.0, 0.5, 1.0);
}

float4 proceduralVS(uint id : SV_VertexID) : SV_Position {
  return clipPosition(positionForID(id) + offsetPixels);
}

float4 attributeVS(float2 pixel : POSITION, float2 instanceOffset : INSTANCEOFFSET) : SV_Position {
  return clipPosition(pixel + instanceOffset + offsetPixels);
}

float pixelPS() : SV_Target0 { return 1.0; }
