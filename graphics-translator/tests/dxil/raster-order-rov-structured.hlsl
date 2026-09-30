struct Item { uint value; uint sentinel; };
RasterizerOrderedStructuredBuffer<Item> Ordered : register(u0);
void main(float4 position : SV_Position, uint primitive : SV_PrimitiveID) {
  uint index=(uint)position.x;
  Item item=Ordered[index];
  item.value=item.value*4u+primitive+1u;
  Ordered[index]=item;
}
