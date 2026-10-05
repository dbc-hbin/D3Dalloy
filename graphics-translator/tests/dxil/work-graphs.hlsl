struct Seed { uint base; };
struct Record { uint value; };
RWByteAddressBuffer result : register(u0);

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeIsProgramEntry]
[NodeDispatchGrid(1,1,1)]
[NodeMaxRecursionDepth(29)]
[NumThreads(4,1,1)]
void entry(DispatchNodeInputRecord<Seed> seed, uint lane : SV_GroupIndex,
           [MaxRecords(1)] NodeOutput<Seed> entry,
           [MaxRecords(16)] NodeOutput<Record> aggregate) {
  uint remaining = GetRemainingRecursionLevels();
  GroupNodeOutputRecords<Record> records = aggregate.GetGroupNodeOutputRecords(16);
  for (uint i = lane; i < 16; i += 4) records[i].value = seed.Get().base + remaining * 16 + i;
  records.OutputComplete();
  GroupNodeOutputRecords<Seed> recurse = entry.GetGroupNodeOutputRecords(entry.IsValid() ? 1 : 0);
  if (entry.IsValid() && lane == 0) recurse.Get().base = seed.Get().base;
  recurse.OutputComplete();
}

[Shader("node")]
[NodeLaunch("coalescing")]
[WaveSize(4)]
[NumThreads(4,1,1)]
void aggregate([MaxRecords(8)] GroupNodeInputRecords<Record> records, uint lane : SV_GroupIndex,
               [MaxRecords(8)] NodeOutput<Record> terminal) {
  bool wave_ok = WaveGetLaneCount() == 4 && WaveActiveSum(lane) == 6 &&
                 WaveActiveBallot(true).x == 15 &&
                 WaveReadLaneAt(lane, (lane + 1) & 3) == ((lane + 1) & 3);
  GroupNodeOutputRecords<Record> output = terminal.GetGroupNodeOutputRecords(records.Count());
  for (uint i = lane; i < records.Count(); i += 4) output[i].value = records[i].value * (wave_ok ? 3 : 0);
  output.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void terminal(ThreadNodeInputRecord<Record> input) {
  uint previous;
  result.InterlockedAdd(0, input.Get().value, previous);
  result.InterlockedAdd(4, 1, previous);
}
