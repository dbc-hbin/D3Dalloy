// SM6.6 groupshared returning atomics: every integer operation and cross-SIMD contention.
RWByteAddressBuffer Out : register(u0);
groupshared uint64_t Shared;
groupshared int64_t SignedShared;

[numthreads(1, 1, 1)]
void cs_atomic64_groupops() {
  uint64_t old;
  Shared = 0x200000002ull;
  InterlockedAdd(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>(0, old); Out.Store<uint64_t>(8, Shared);
  Shared = 0x200000002ull;
  InterlockedAnd(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>(16, old); Out.Store<uint64_t>(24, Shared);
  Shared = 0x200000002ull;
  InterlockedOr(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>(32, old); Out.Store<uint64_t>(40, Shared);
  Shared = 0x200000002ull;
  InterlockedXor(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>(48, old); Out.Store<uint64_t>(56, Shared);
  int64_t signed_old;
  SignedShared = (int64_t)0xfffffff000000002ull;
  InterlockedMin(SignedShared, (int64_t)0xfffffff100000003ull, signed_old);
  Out.Store<int64_t>(64, signed_old); Out.Store<int64_t>(72, SignedShared);
  SignedShared = (int64_t)0xfffffff000000002ull;
  InterlockedMax(SignedShared, (int64_t)0xfffffff100000003ull, signed_old);
  Out.Store<int64_t>(80, signed_old); Out.Store<int64_t>(88, SignedShared);
  Shared = 0x200000002ull;
  InterlockedMin(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>(96, old); Out.Store<uint64_t>(104, Shared);
  Shared = 0x200000002ull;
  InterlockedMax(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>(112, old); Out.Store<uint64_t>(120, Shared);
  Shared = 0x200000002ull;
  InterlockedExchange(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>(128, old); Out.Store<uint64_t>(136, Shared);
  Shared = 0x200000002ull;
  InterlockedCompareExchange(Shared, 0x200000002ull, 0x300000003ull, old);
  Out.Store<uint64_t>(144, old);
  InterlockedCompareExchange(Shared, 0x200000002ull, 0x400000004ull, old);
  Out.Store<uint64_t>(152, old); Out.Store<uint64_t>(160, Shared);
  Shared = 0xffffffffffffffffull;
  InterlockedAdd(Shared, 1ull, old);
  Out.Store<uint64_t>(168, old); Out.Store<uint64_t>(176, Shared);
  InterlockedCompareStore(Shared, 0ull, 0x8000000000000000ull);
  Out.Store<uint64_t>(184, Shared);
}

[numthreads(1024, 1, 1)]
void cs_atomic64_groupstress(uint gi : SV_GroupIndex, uint3 group : SV_GroupID) {
  if (gi == 0) Shared = 0;
  GroupMemoryBarrierWithGroupSync();
  uint64_t old;
  InterlockedAdd(Shared, 0x100000001ull, old);
  Out.Store<uint64_t>((group.x * 1025 + gi) * 8, old);
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0) Out.Store<uint64_t>((group.x * 1025 + 1024) * 8, Shared);
}

// The lock table must not consume any of D3D12's 32KiB user groupshared capacity.
groupshared uint64_t FullCapacity[4096];
[numthreads(1024, 1, 1)]
void cs_atomic64_groupcapacity(uint gi : SV_GroupIndex) {
  for (uint j = 0; j < 4; ++j) FullCapacity[gi + j * 1024] = (uint64_t)(gi + j * 1024) * 0x100000001ull;
  GroupMemoryBarrierWithGroupSync();
  for (uint j = 0; j < 4; ++j) {
    uint index = gi + j * 1024;
    uint64_t old;
    InterlockedAdd(FullCapacity[index], 0x100000001ull, old);
    Out.Store<uint64_t>(index * 8, old);
  }
  GroupMemoryBarrierWithGroupSync();
  for (uint j = 0; j < 4; ++j) {
    uint index = gi + j * 1024;
    Out.Store<uint64_t>(32768 + index * 8, FullCapacity[index]);
  }
}
