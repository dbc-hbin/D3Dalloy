struct EntrySeed { uint3 grid; uint tag; uint scratch; };
struct [NodeTrackRWInputSharing] Seed {
  uint3 grid : SV_DispatchGrid;
  uint tag;
  uint scratch;
};
struct Relay {
  uint3 grid : SV_DispatchGrid;
  uint slot;
  uint value;
};
struct Leaf { uint slot; uint value; };
struct SharedSeed { uint3 grid : SV_DispatchGrid; uint tag; };
RWByteAddressBuffer result : register(u0);

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(1,1,1)]
[NumThreads(1,1,1)]
void frontier(DispatchNodeInputRecord<EntrySeed> input,
              [MaxRecords(1)] NodeOutput<Seed> producer) {
  GroupNodeOutputRecords<Seed> record = producer.GetGroupNodeOutputRecords(1);
  record.Get().grid = input.Get().grid;
  record.Get().tag = input.Get().tag;
  record.Get().scratch = input.Get().scratch;
  record.OutputComplete();
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(65,8,1)]
[NumThreads(1,1,1)]
void producer(globallycoherent RWDispatchNodeInputRecord<Seed> input, uint3 group : SV_GroupID,
              [MaxRecords(4)] NodeOutput<Relay> relay) {
  uint groupIndex = group.x + input.Get().grid.x * group.y;
  InterlockedAdd(input.Get().scratch, 1);
  GroupNodeOutputRecords<Relay> records = relay.GetGroupNodeOutputRecords(4);
  for (uint lane = 0; lane < 4; ++lane) {
    records[lane].grid = uint3(lane == 0 ? 0 : 1 + ((groupIndex + lane) & 1), 1, 1);
    records[lane].slot = groupIndex * 4 + lane;
    records[lane].value = input.Get().tag ^ (group.x + group.y * 65536) ^ ((lane + 1) * 0x9e3779b9u);
  }
  records.OutputComplete();
  Barrier(NODE_INPUT_MEMORY, DEVICE_SCOPE | GROUP_SYNC);
  if (input.FinishedCrossGroupSharing()) {
    result.InterlockedAdd(4, 1);
    result.Store(8, input.Get().scratch);
  }
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(2,1,1)]
[NumThreads(1,1,1)]
void relay(DispatchNodeInputRecord<Relay> input, uint3 group : SV_GroupID,
           [MaxRecords(1)] NodeOutput<Leaf> leaf) {
  GroupNodeOutputRecords<Leaf> output = leaf.GetGroupNodeOutputRecords(1);
  output.Get().slot = input.Get().slot * 2 + group.x;
  output.Get().value = input.Get().value ^ (group.x * 0x85ebca6bu);
  output.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void leaf(ThreadNodeInputRecord<Leaf> input) {
  result.InterlockedAdd(0, 1);
  result.InterlockedAdd(32 + input.Get().slot * 8, 1);
  result.Store(36 + input.Get().slot * 8, input.Get().value);
}

// A zero grid suppresses only this node. Its input is also delivered to the
// fixed-grid sibling, which treats SV_DispatchGrid as ordinary record data.
// Shared inputs are deliberately read-only; RW sharing is a different contract.
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeMaxDispatchGrid(1,1,1)]
[NumThreads(1,1,1)]
void zero_source(DispatchNodeInputRecord<SharedSeed> input) {
  result.InterlockedAdd(12, 1);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeShareInputOf("zero_source",0)]
[NodeDispatchGrid(1,1,1)]
[NumThreads(1,1,1)]
void shared_fixed(DispatchNodeInputRecord<SharedSeed> input) {
  result.InterlockedAdd(16, 1);
  result.Store(20, input.Get().tag);
}
