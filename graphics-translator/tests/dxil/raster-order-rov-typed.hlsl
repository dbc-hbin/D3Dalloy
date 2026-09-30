RasterizerOrderedBuffer<uint> Ordered : register(u0);
void main(float4 position : SV_Position, uint primitive : SV_PrimitiveID) {
  uint index=(uint)position.x;
  uint previous=Ordered[index];
  Ordered[index]=previous*4u+primitive+1u;
}
