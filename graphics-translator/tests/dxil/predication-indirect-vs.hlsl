RWByteAddressBuffer output_buffer : register(u0);

float4 main(float4 position : POSITION) : SV_Position {
  uint ignored;
  output_buffer.InterlockedAdd(0, 1, ignored);
  return position;
}
