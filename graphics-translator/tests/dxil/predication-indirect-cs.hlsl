RWByteAddressBuffer output_buffer : register(u0);
cbuffer Constants : register(b0) { uint amount; };

[numthreads(1, 1, 1)]
void main() {
  uint ignored;
  output_buffer.InterlockedAdd(0, amount, ignored);
}
