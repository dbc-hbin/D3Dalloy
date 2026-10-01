struct Seed { uint base; uint3 grid : SV_DispatchGrid; };
struct Record { uint value; };
struct [NodeTrackRWInputSharing] Scratch { uint value; uint scratch; };
RWByteAddressBuffer result : register(u0);
cbuffer Local : register(b0) { uint multiplier; };
GlobalRootSignature WorkGraphGlobal = { "UAV(u0)" };
LocalRootSignature WorkGraphLocal = { "RootFlags(LOCAL_ROOT_SIGNATURE), RootConstants(num32BitConstants=1, b0)" };

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(3, 1, 1)]
[NumThreads(4, 1, 1)]
void producer(DispatchNodeInputRecord<Seed> input, uint3 group : SV_GroupID, uint lane : SV_GroupIndex,
              [NodeID("sink", 0)] [NodeArraySize(3)] [AllowSparseNodes] [MaxRecords(4)] NodeOutputArray<Record> array,
              [NodeID("sink", 0)] [MaxRecordsSharedWith(array)] NodeOutput<Record> common,
              [NodeID("counter")] [MaxRecords(1)] NodeOutput<Scratch> counting,
              [NodeID("empty")] [MaxRecords(4)] EmptyNodeOutput signaling) {
  uint value = input.Get().base + group.x * 100 + lane;
  uint arrayCount = lane < 2 && array[group.x].IsValid() ? 1 : 0;
  ThreadNodeOutputRecords<Record> a = array[group.x].GetThreadNodeOutputRecords(arrayCount);
  if (arrayCount) a.Get().value = value;
  a.OutputComplete();
  ThreadNodeOutputRecords<Record> b = common.GetThreadNodeOutputRecords(lane >= 2 ? 1 : 0);
  if (lane >= 2) b.Get().value = value;
  b.OutputComplete();
  GroupNodeOutputRecords<Scratch> c = counting.GetGroupNodeOutputRecords(group.x == 0 ? 1 : 0);
  if (group.x == 0 && lane == 0) { c.Get().value = input.Get().base; c.Get().scratch = 0; }
  c.OutputComplete();
  if (group.x == 0 && lane == 0) signaling.ThreadIncrementOutputCount(4);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeID("sink", 0)]
void sink0(ThreadNodeInputRecord<Record> input) {
  result.InterlockedAdd(0, input.Get().value * multiplier);
  result.InterlockedAdd(4, 1);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeID("sink", 2)]
[NodeLocalRootArgumentsTableIndex(0)]
void sink2(ThreadNodeInputRecord<Record> input) {
  result.InterlockedAdd(0, input.Get().value * multiplier);
  result.InterlockedAdd(4, 1);
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeShareInputOf("sink", 0)]
void sharing(ThreadNodeInputRecord<Record> input) {
  result.InterlockedAdd(0, input.Get().value * 2 * multiplier);
  result.InterlockedAdd(4, 1);
}

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(3, 1, 1)]
[NumThreads(4, 1, 1)]
void counter(RWDispatchNodeInputRecord<Scratch> input, uint lane : SV_GroupIndex) {
  if (lane == 0) InterlockedAdd(input.Get().scratch, 1);
  DeviceMemoryBarrierWithGroupSync();
  bool last = input.FinishedCrossGroupSharing();
  if (last && lane == 0) {
    result.InterlockedAdd(0, (input.Get().value + input.Get().scratch) * multiplier);
    result.InterlockedAdd(4, 1);
  }
}

[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void empty() { result.InterlockedAdd(4, 1); }
