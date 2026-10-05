struct Seed { uint index; };
RWTexture2D<uint64_t> values : register(u0);
RWByteAddressBuffer output : register(u1);
[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void typed_publish(ThreadNodeInputRecord<Seed> input) {
  uint i = input.Get().index;
  uint2 position = uint2(i & 3, i >> 2);
  uint64_t old;
  InterlockedAdd(values[position], 0x000000040000000fULL, old);
  output.Store<uint64_t>(i * 16, old);
  output.Store<uint64_t>(i * 16 + 8, values[position]);
  output.InterlockedAdd(16 * 16, 1);
}
