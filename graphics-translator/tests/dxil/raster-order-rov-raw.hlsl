RasterizerOrderedByteAddressBuffer ordered : register(u0);
void main(float4 position : SV_Position, uint primitive : SV_PrimitiveID) {
  uint at = uint(position.x) * 4u;
  uint previous = ordered.Load(at);
  ordered.Store(at, previous * 4u + primitive + 1u);
}
