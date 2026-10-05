struct Record { uint index; uint guard; uint64_t value; };
RWByteAddressBuffer output : register(u0);
static const uint Records = 16;
static const uint Stride = 96;
static const uint64_t Base = 0xfffffffaFFFFF000ULL;
static const uint64_t Delta = 0x000000040000000FULL;
static const uint64_t Mask = 0xa5c35a3cf00d1234ULL;
static const uint64_t Mask2 = 0x13579bdf2468ace0ULL;
static const uint64_t Mask3 = 0x55aa55aaaa55aa55ULL;

[Shader("node")]
[NodeLaunch("thread")]
[NodeIsProgramEntry]
void seed(RWThreadNodeInputRecord<Record> input, [MaxRecords(1)] NodeOutput<Record> child) {
  uint i = input.Get().index;
  uint64_t old;
  InterlockedAdd(input.Get().value, Delta, old);
  output.Store<uint64_t>(i * Stride, old);
  output.Store<uint64_t>(i * Stride + 8, input.Get().value);
  ThreadNodeOutputRecords<Record> next = child.GetThreadNodeOutputRecords(1);
  next.Get().index = i;
  next.Get().guard = input.Get().guard;
  next.Get().value = input.Get().value ^ Mask;
  InterlockedXor(next.Get().value, Mask2, old);
  output.Store<uint64_t>(i * Stride + 16, old);
  output.Store<uint64_t>(i * Stride + 24, next.Get().value);
  output.InterlockedAdd(Records * Stride, 1);
  next.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void child(RWThreadNodeInputRecord<Record> input, [MaxRecords(1)] NodeOutput<Record> terminal) {
  uint i = input.Get().index;
  uint64_t expected = ((Base + uint64_t(i) * 0x100000001ULL + Delta) ^ Mask) ^ Mask2;
  uint64_t old;
  InterlockedCompareExchange(input.Get().value, expected, expected + Delta, old);
  output.Store<uint64_t>(i * Stride + 32, old);
  output.Store<uint64_t>(i * Stride + 40, input.Get().value);
  uint guard;
  InterlockedAdd(input.Get().guard, 3, guard);
  output.Store<uint64_t>(i * Stride + 48, uint64_t(guard) | (uint64_t(input.Get().guard) << 32));
  output.Store<uint64_t>(i * Stride + 56, input.Get().index);
  ThreadNodeOutputRecords<Record> next = terminal.GetThreadNodeOutputRecords(1);
  next.Get().index = i;
  next.Get().guard = input.Get().guard;
  next.Get().value = input.Get().value ^ Mask3;
  output.InterlockedAdd(Records * Stride + 4, 1);
  next.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void terminal(RWThreadNodeInputRecord<Record> input) {
  uint i = input.Get().index;
  uint64_t old;
  InterlockedExchange(input.Get().value, 0x8000000000000005ULL + i, old);
  output.Store<uint64_t>(i * Stride + 64, old);
  output.Store<uint64_t>(i * Stride + 72, input.Get().value);
  output.Store<uint64_t>(i * Stride + 80, uint64_t(input.Get().index) | (uint64_t(input.Get().guard) << 32));
  output.Store<uint64_t>(i * Stride + 88, 0xdeadbeefcafef00dULL);
  output.InterlockedAdd(Records * Stride + 8, 1);
}
