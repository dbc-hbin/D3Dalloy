RasterizerOrderedBuffer<uint> Ordered[] : register(u0);
void main(float4 position : SV_Position, uint primitive : SV_PrimitiveID) {
  uint resource=NonUniformResourceIndex((uint)position.x & 1u);
  uint previous=Ordered[resource][(uint)position.x];
  Ordered[resource][(uint)position.x]=previous*4u+primitive+1u;
}
