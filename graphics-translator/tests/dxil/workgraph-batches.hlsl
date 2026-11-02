struct Seed { uint3 grid : SV_DispatchGrid; uint tag; };
RWByteAddressBuffer result : register(u0);
[Shader("node")] [NodeLaunch("broadcasting")] [NodeIsProgramEntry]
[NodeMaxDispatchGrid(257,257,1)] [NumThreads(1,1,1)]
void leaf(DispatchNodeInputRecord<Seed> input, uint3 group:SV_GroupID) {
  uint index=group.x+input.Get().grid.x*group.y;
  result.Store(8+index*8,index^input.Get().tag);
  result.Store(12+index*8,group.x+group.y*65536);
  result.InterlockedAdd(0,1);
}
[Shader("node")] [NodeLaunch("broadcasting")] [NodeIsProgramEntry]
[NodeMaxDispatchGrid(257,257,1)] [NumThreads(1,1,1)]
void atomic_leaf(DispatchNodeInputRecord<Seed> input,uint3 group:SV_GroupID) {
  uint index=group.x+input.Get().grid.x*group.y;
  uint64_t old; result.InterlockedAdd64(0,1,old);
  result.Store<uint64_t>(8+index*8,old);
}
