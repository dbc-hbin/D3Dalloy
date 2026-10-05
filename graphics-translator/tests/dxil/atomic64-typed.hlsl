// SM6.6 Typed64 contract: int64_t/uint64_t uses DXGI_FORMAT_R32G32_UINT.
// Official specification: HLSL_SM_6_6_Int64_and_Float_Atomics.html.
#ifndef SHAPE
#define SHAPE 2
#endif
#ifndef SIGNED
#define SIGNED 0
#endif
#if SIGNED
#define WORD int64_t
#else
#define WORD uint64_t
#endif
RWByteAddressBuffer output : register(u0);
#if SHAPE == 0
RWBuffer<WORD> cells : register(u1);
RWBuffer<uint2> ordinary : register(u2);
#define AT(i) (i)
#elif SHAPE == 1
RWTexture1D<WORD> cells : register(u1);
RWTexture1D<uint2> ordinary : register(u2);
#define AT(i) (i)
#elif SHAPE == 2
RWTexture2D<WORD> cells : register(u1);
RWTexture2D<uint2> ordinary : register(u2);
#define AT(i) uint2((i), 0)
#elif SHAPE == 3
RWTexture3D<WORD> cells : register(u1);
RWTexture3D<uint2> ordinary : register(u2);
#define AT(i) uint3((i), 0, 1)
#elif SHAPE == 4
RWTexture1DArray<WORD> cells : register(u1);
RWTexture1DArray<uint2> ordinary : register(u2);
#define AT(i) uint2((i), 1)
#elif SHAPE == 5
RWTexture2DArray<WORD> cells : register(u1);
RWTexture2DArray<uint2> ordinary : register(u2);
#define AT(i) uint3((i), 0, 1)
#endif

[numthreads(1,1,1)]
void operations() {
  WORD initial = (WORD)0xfffffff000000002ULL;
  WORD value = (WORD)0x0000000100000001ULL;
  WORD old;
  // Each operation starts independently, so a bad earlier operation cannot
  // manufacture the expected input of the following operation.
  for (uint op = 0; op < 9; ++op) {
    ordinary[AT(op)] = uint2((uint)initial, (uint)((uint64_t)initial >> 32));
    switch (op) {
    case 0: InterlockedAdd(cells[AT(op)], value, old); break;
    case 1: InterlockedAnd(cells[AT(op)], value, old); break;
    case 2: InterlockedOr(cells[AT(op)], value, old); break;
    case 3: InterlockedXor(cells[AT(op)], value, old); break;
    case 4: InterlockedMin(cells[AT(op)], value, old); break;
    case 5: InterlockedMax(cells[AT(op)], value, old); break;
    case 6: InterlockedExchange(cells[AT(op)], value, old); break;
    case 7: InterlockedCompareExchange(cells[AT(op)], initial, value, old); break;
    case 8: old = cells[AT(op)]; InterlockedCompareStore(cells[AT(op)], initial, value); break;
    }
    output.Store<uint64_t>(op * 16, (uint64_t)old);
    uint2 pair = ordinary[AT(op)];
    output.Store<uint64_t>(op * 16 + 8, (uint64_t)pair.x | ((uint64_t)pair.y << 32));
  }
  InterlockedCompareExchange(cells[AT(7)], initial, initial, old);
  output.Store<uint64_t>(144, (uint64_t)old);
  InterlockedCompareStore(cells[AT(8)], initial, initial);
  output.Store<uint64_t>(152, (uint64_t)cells[AT(8)]);
  ordinary[AT(9)] = uint2(0xffffffff, 0xffffffff);
  InterlockedAdd(cells[AT(9)], (WORD)1, old);
  output.Store<uint64_t>(160, (uint64_t)old);
  output.Store<uint64_t>(168, (uint64_t)cells[AT(9)]);
  ordinary[AT(10)] = uint2(0x12345678, 0x9abcdef0);
  ordinary[AT(10)].x = 0x87654321;
  output.Store<uint64_t>(176, (uint64_t)cells[AT(10)]);
  ordinary[AT(10)].y = 0x76543210;
  output.Store<uint64_t>(184, (uint64_t)cells[AT(10)]);
  InterlockedAdd(cells[AT(0xffffffff)], value, old);
  output.Store<uint64_t>(192, (uint64_t)old);
  output.Store<uint64_t>(200, (uint64_t)cells[AT(0xffffffff)]);
  ordinary[AT(0xffffffff)] = uint2(0x1234, 0x5678);
  output.Store<uint64_t>(208, (uint64_t)cells[AT(10)]);
}

[numthreads(32,1,1)]
void contention(uint3 tid : SV_DispatchThreadID) {
  WORD old;
  InterlockedAdd(cells[AT(11)], (WORD)0x100000001ULL, old);
  output.Store<uint64_t>(tid.x * 8, (uint64_t)old);
}

[numthreads(1,1,1)]
void snapshot() { output.Store<uint64_t>(8192, (uint64_t)cells[AT(11)]); }

#if SHAPE == 0
RWBuffer<uint> scalar32 : register(u3);
RWByteAddressBuffer raw : register(u4);
[numthreads(1,1,1)]
void aliases() {
  WORD old;
  ordinary[AT(0)] = uint2(0xffffffff, 0x12345678);
  uint old32;
  InterlockedAdd(scalar32[0], 1, old32);
  output.Store(0, old32);
  output.Store<uint64_t>(8, (uint64_t)cells[AT(0)]);
  InterlockedAdd(cells[AT(0)], (WORD)0x100000001ULL, old);
  output.Store<uint64_t>(16, (uint64_t)old);
  output.Store2(24, uint2(scalar32[0], scalar32[1]));
  raw.Store(4, 0x87654321);
  output.Store<uint64_t>(32, (uint64_t)cells[AT(0)]);
  InterlockedCompareExchange(scalar32[0], 1, 0xfedcba98, old32);
  output.Store(40, old32);
  output.Store<uint64_t>(48, (uint64_t)cells[AT(0)]);
}
#endif
