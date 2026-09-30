RasterizerOrderedBuffer<uint> orderedBuffer : register(u0);
RasterizerOrderedTexture2D<uint> orderedTexture : register(u1);

void main(float4 position : SV_Position, uint primitive : SV_PrimitiveID) {
  uint at = uint(position.y) * 4 + uint(position.x);
  uint previous = orderedBuffer[at];
  orderedBuffer[at] = previous * 4u + primitive + 1u;
  uint2 xy = uint2(position.xy);
  uint beforeTexture = orderedTexture[xy];
  orderedTexture[xy] = beforeTexture * 4u + primitive + 1u;
}
