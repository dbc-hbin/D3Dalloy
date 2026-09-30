RWTexture3D<uint> volume : register(u0);
RWByteAddressBuffer output : register(u1);

[numthreads(1, 1, 1)] void main() {
  uint width, height, depth;
  volume.GetDimensions(width, height, depth);
  output.Store(16, width);
  output.Store(20, height);
  output.Store(24, depth);
  volume[uint3(0, 0, 0)] = 101;
  volume[uint3(0, 0, 1)] = 202;
  volume[uint3(0, 0, 2)] = 303;
  output.Store(0, volume[uint3(0, 0, 1)]);
  output.Store(4, volume[uint3(0, 0, 2)]);
  uint before;
  InterlockedAdd(volume[uint3(1, 0, 1)], 7, before);
  output.Store(8, before);
  InterlockedAdd(volume[uint3(1, 0, 2)], 9, before);
  output.Store(12, before);
}
