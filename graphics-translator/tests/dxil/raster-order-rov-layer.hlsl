RasterizerOrderedByteAddressBuffer ordered : register(u0);
void main(float4 position : SV_Position, uint primitive : SV_PrimitiveID,
          uint layer : SV_RenderTargetArrayIndex) {
  uint at = (layer * 2u + uint(position.x)) * 4u;
  uint previous = ordered.Load(at);
  ordered.Store(at, previous * 4u + primitive + 1u);
}
