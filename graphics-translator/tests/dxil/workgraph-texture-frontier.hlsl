struct Seed { uint index; };
globallycoherent RWTexture2D<uint> values : register(u0);
RWByteAddressBuffer output : register(u1);

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(1,1,1)]
[NumThreads(1,1,1)]
void texture_seed(DispatchNodeInputRecord<Seed> input,
                  [MaxRecords(1)] NodeOutput<Seed> texture_child) {
  uint i = input.Get().index;
  uint initial = values[uint2(i, 0)];
  values[uint2(i, 0)] = initial + 0x00010007u;
  output.Store(16 + i * 16, initial);
  output.InterlockedAdd(16 + 16 * 16, 1);
  // Publish the UAV write before making the descendant record available.
  Barrier(UAV_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
  GroupNodeOutputRecords<Seed> record = texture_child.GetGroupNodeOutputRecords(1);
  record.Get().index = i;
  record.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1,1,1)]
[NumThreads(1,1,1)]
void texture_child(DispatchNodeInputRecord<Seed> input) {
  uint i = input.Get().index;
  uint observed = values[uint2(i, 0)];
  uint final_value = observed + 0x0002000bu;
  values[uint2(i, 0)] = final_value;
  output.Store(20 + i * 16, observed);
  output.Store(24 + i * 16, final_value);
  output.InterlockedAdd(28 + i * 16, 1);
  output.InterlockedAdd(20 + 16 * 16, 1);
}
