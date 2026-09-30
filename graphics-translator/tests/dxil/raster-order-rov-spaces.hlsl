RasterizerOrderedByteAddressBuffer first : register(u0, space3);
RasterizerOrderedTexture2D<uint> second : register(u0, space7);
void main(float4 position : SV_Position, uint primitive : SV_PrimitiveID) {
  uint x = uint(position.x);
  uint previous = first.Load(x * 4u);
  first.Store(x * 4u, previous * 4u + primitive + 1u);
  uint2 xy = uint2(position.xy);
  uint before = second[xy];
  second[xy] = before * 4u + primitive + 1u;
}
