struct PixelOutput { float4 color : SV_Target0; uint stencil : SV_StencilRef; };
PixelOutput main(float4 position : SV_Position) {
  PixelOutput output;
  output.color = float4(1, 0, 0, 1);
  output.stencil = (uint(position.x) & 1u) != 0u ? 0x35u : 0xA6u;
  return output;
}
