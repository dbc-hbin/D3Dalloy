RWByteAddressBuffer output : register(u0);
RWStructuredBuffer<uint64_t> cells : register(u1);
RWByteAddressBuffer alias : register(u2);
ByteAddressBuffer cpu_input : register(t0);
RWByteAddressBuffer arguments : register(u3);
cbuffer Root : register(b0) { uint base_cell; uint increment; uint tag; uint mode; };

[numthreads(1, 1, 1)]
void aliases_and_map() {
  uint64_t input = cpu_input.Load<uint64_t>(0);
  cells[0] = input;
  uint64_t old;
  InterlockedAdd(cells[0], 0x100000001ULL, old);
  output.Store<uint64_t>(0, old);
  output.Store<uint64_t>(8, alias.Load<uint64_t>(0));
  alias.Store(4, 0x87654321);
  output.Store<uint64_t>(16, cells[0]);
  InterlockedCompareExchange(cells[0], 0x8765432100000001ULL + (input & 0xffffffffULL),
                            0x123456789abcdef0ULL, old);
  output.Store<uint64_t>(24, old);
  output.Store<uint64_t>(32, alias.Load<uint64_t>(0));
}

// All public indirect data is produced on the GPU, including a poison third
// command that must never run when the GPU-written count is respected.
[numthreads(1, 1, 1)]
void prepare() {
  for (uint i = 0; i < 8; ++i)
    cells[i] = (uint64_t(0x12345678 + i) << 32) | (0xfffffff0 + i);
  for (uint offset = 0; offset < 256; offset += 4) output.Store(offset, 0xcdcdcdcd);
  output.Store(96, 0);
  output.Store(100, 0xa11c0ded);
  for (uint command = 0; command < 3; ++command) {
    uint offset = command * 28;
    arguments.Store4(offset, uint4(command * 2, command == 0 ? 33 : 65, 0xface0000 + command, 0));
    arguments.Store3(offset + 16, uint3(mode == 2 ? 0 : (command == 0 ? 2 : 1), command == 1 ? 2 : 1, 1));
  }
  arguments.Store(96, mode == 1 ? 0 : 2);
  arguments.Store2(104, uint2(mode == 3 ? 0 : 1, 0));
}

[numthreads(1, 1, 1)]
void returning(uint3 id : SV_DispatchThreadID) {
  uint index = base_cell + id.x + id.y;
  uint64_t old;
  InterlockedAdd(cells[index], 0x100000000ULL + increment, old);
  output.Store<uint64_t>(index * 16, old);
  output.Store<uint64_t>(index * 16 + 8, cells[index]);
  output.Store(64 + index * 4, tag);
  uint ignored;
  output.InterlockedAdd(96, 1, ignored);
}

// A separately compiled ordinary-32 consumer must observe exported canonical
// words, including a 4-byte aligned vector that straddles three 64-bit cells.
[numthreads(1, 1, 1)]
void observe32() {
  for (uint i = 0; i < 8; ++i) output.Store2(128 + i * 8, alias.Load2(i * 8));
  output.Store4(192, alias.Load4(4));
  output.Store2(208, alias.Load2(4));
  output.Store(216, alias.Load(4));
}
