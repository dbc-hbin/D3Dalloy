#define wmain sm6_fixture_wmain
#include "owned-d3d12-sm6.cpp"
#undef wmain

namespace {
constexpr uint32_t kLanes = 32;
constexpr uint32_t kAtomicIterations = 512;
constexpr uint32_t kOrdinaryIterations = 1u << 22;
constexpr uint32_t kOrdinaryWord = 512;
constexpr uint32_t kOrdinarySeed = 0x12345678u;
constexpr uint32_t kOrdinaryTag = 0xa55a39c3u;
constexpr uint32_t kCellBytes = 4096;
constexpr uint32_t kOutputBytes = 32 + kLanes * 24 + 32;

void marker(const char *name, const char *phase) {
  char line[192];
  const int length = std::snprintf(line, sizeof(line),
      "OWNED_D3D12_ATOMIC64_CONTEXT_PHASE case=%s phase=%s\n", name, phase);
  if (length <= 0 || size_t(length) >= sizeof(line)) die("context marker length", E_FAIL);
  DWORD written = 0;
  if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(length), &written, nullptr) || written != DWORD(length))
    die("context marker write");
}

uint32_t ordinaryValue(uint32_t lane) {
  uint32_t multiplier = 1664525u, increment = 1013904223u;
  uint32_t product = 1, sum = 0;
  for (uint32_t count = kOrdinaryIterations; count; count >>= 1) {
    if (count & 1) { sum = sum * multiplier + increment; product *= multiplier; }
    increment *= multiplier + 1;
    multiplier *= multiplier;
  }
  return (product * (kOrdinarySeed + lane) + sum) ^ kOrdinaryTag;
}
uint64_t initialCell(uint32_t lane) {
  return (uint64_t(0x12345678u + lane) << 32) | (0xfffffff0u + lane);
}
template<class T> void store(std::vector<uint8_t> &bytes, size_t offset, T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
uint64_t hashBytes(const std::vector<uint8_t> &bytes) {
  uint64_t hash = 1469598103934665603ULL;
  for (uint8_t byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
  return hash;
}
void compareBytes(const char *name, const char *resource, const std::vector<uint8_t> &actual,
                  const std::vector<uint8_t> &expected) {
  EXPECT_EQ(name, "byte count", 0, actual.size(), expected.size());
  for (size_t byte = 0; byte < std::min(actual.size(), expected.size()); ++byte)
    EXPECT_EQ(name, resource, byte, actual[byte], expected[byte]);
}

void barrier(Gpu &gpu, ID3D12Resource *resource) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  gpu.list->ResourceBarrier(1, &barrier);
}
void bind(Gpu &gpu, ID3D12RootSignature *root, ID3D12PipelineState *pipeline,
    ID3D12Resource *output, ID3D12Resource *cells, bool atomic, uint32_t iterations = 0) {
  gpu.rawUav(0, output);
  gpu.structuredUav(1, cells, 8);
  gpu.rawUav(2, cells);
  gpu.list->SetComputeRootSignature(root);
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetComputeRootDescriptorTable(0, gpu.gpu(0));
  const uint32_t values[] = {kOrdinaryWord, iterations ? iterations : atomic ? kAtomicIterations : kOrdinaryIterations,
      kOrdinarySeed, kOrdinaryTag};
  gpu.list->SetComputeRoot32BitConstants(1, 4, values, 0);
  gpu.list->SetPipelineState(pipeline);
  gpu.list->Dispatch(1, 1, 1);
  barrier(gpu, output);
  if (atomic) barrier(gpu, cells);
}
UINT64 submit(Gpu &gpu) {
  ID3D12CommandList *lists[] = {gpu.list.Get()};
  gpu.queue->ExecuteCommandLists(1, lists);
  const UINT64 value = ++gpu.fenceValue;
  check(gpu.queue->Signal(gpu.fence.Get(), value), "context completion signal");
  return value;
}
void wait(Gpu &gpu, UINT64 value) {
  if (gpu.fence->GetCompletedValue() >= value) return;
  check(gpu.fence->SetEventOnCompletion(value, gpu.event), "context completion event");
  if (WaitForSingleObject(gpu.event, kFenceTimeoutMs) != WAIT_OBJECT_0) die("context completion timeout");
}
void reset(Gpu &gpu) {
  check(gpu.allocator->Reset(), "context allocator Reset");
  check(gpu.list->Reset(gpu.allocator.Get(), nullptr), "context list Reset");
}

void runCase(ID3D12Device *device, ID3D12RootSignature *root, ID3D12PipelineState *atomic_pipeline,
    ID3D12PipelineState *ordinary_pipeline, const char *name, bool atomic_first, bool same_view, bool replay) {
  marker(name, "setup");
  Gpu atomic(device), ordinary(device);
  auto cell_seed = filled(kCellBytes);
  for (uint32_t lane = 0; lane < kLanes; ++lane) store(cell_seed, lane * 8, initialCell(lane));
  auto cells = atomic.uavBuffer(cell_seed);
  auto output = atomic.uavBuffer(filled(kOutputBytes));
  auto ordinary_output = same_view ? cells : ordinary.uavBuffer(filled(kCellBytes));
  atomic.finish({}, nullptr);
  ordinary.finish({}, nullptr);
  bind(atomic, root, atomic_pipeline, output.Get(), cells.Get(), true);
  // Even unused descriptors stay inside the ordinary physical domain. A
  // conservative full-table capture cannot turn the disjoint case into aliasing.
  bind(ordinary, root, ordinary_pipeline, ordinary_output.Get(), ordinary_output.Get(), false);
  check(atomic.list->Close(), "close atomic context list");
  check(ordinary.list->Close(), "close ordinary context list");

  marker(name, "execute");
  UINT64 atomic_done = 0, ordinary_done = 0;
  if (atomic_first) {
    atomic_done = submit(atomic);
    Sleep(2); // Scheduling hint only; trace evidence must prove actual overlap.
    ordinary_done = submit(ordinary);
  } else {
    ordinary_done = submit(ordinary);
    Sleep(2);
    atomic_done = submit(atomic);
  }
  wait(atomic, atomic_done);
  wait(ordinary, ordinary_done);
  if (replay) {
    marker(name, "replay");
    // Identical closed recording and PSO, with the preceding context retired.
    // No descriptor, root-constant, command-link or allocator mutation occurs.
    atomic_done = submit(atomic);
    wait(atomic, atomic_done);
  }
  marker(name, "retired");
  reset(atomic);
  reset(ordinary);
  const auto atomic_result = atomic.finish({output.Get(), cells.Get()}, nullptr);
  const auto ordinary_result = same_view ? std::vector<std::vector<uint8_t>>{} :
      ordinary.finish({ordinary_output.Get()}, nullptr);
  auto expected_output = filled(kOutputBytes);
  auto expected_cells = cell_seed;
  auto expected_ordinary = same_view ? cell_seed : filled(kCellBytes);
  const uint32_t executions = replay ? 2 : 1;
  begin();
  for (uint32_t lane = 0; lane < kLanes; ++lane) {
    const uint64_t increment = 0x100000001ULL * kAtomicIterations;
    const uint64_t old = initialCell(lane) + (executions - 1) * increment;
    const uint64_t final = initialCell(lane) + executions * increment;
    store(expected_output, 32 + lane * 24, old);
    store(expected_output, 40 + lane * 24, final);
    store(expected_output, 48 + lane * 24, final);
    store(expected_cells, lane * 8, final);
    if (same_view) store(expected_ordinary, lane * 8, final);
    store(expected_ordinary, (kOrdinaryWord + lane) * 4, ordinaryValue(lane));
    if (same_view) store(expected_cells, (kOrdinaryWord + lane) * 4, ordinaryValue(lane));
    std::printf("OWNED_D3D12_ATOMIC64_CONTEXT_LANE case=%s lane=%u old=%016llx final=%016llx raw=%016llx ordinary=%08x\n",
        name, lane, static_cast<unsigned long long>(load<uint64_t>(atomic_result[0], 32 + lane * 24)),
        static_cast<unsigned long long>(load<uint64_t>(atomic_result[0], 40 + lane * 24)),
        static_cast<unsigned long long>(load<uint64_t>(atomic_result[0], 48 + lane * 24)),
        load<uint32_t>(same_view ? atomic_result[1] : ordinary_result[0], (kOrdinaryWord + lane) * 4));
  }
  compareBytes(name, "private-context output and guards", atomic_result[0], expected_output);
  compareBytes(name, "canonical cells and guards", atomic_result[1], expected_cells);
  compareBytes(name, "ordinary output and guards", same_view ? atomic_result[1] : ordinary_result[0], expected_ordinary);
  std::printf("OWNED_D3D12_ATOMIC64_CONTEXT_CASE case=%s atomic_first=%u same_view=%u executions=%u atomic_iterations=%u ordinary_iterations=%u output_hash=%016llx cells_hash=%016llx ordinary_hash=%016llx failures=%d\n",
      name, unsigned(atomic_first), unsigned(same_view), executions, kAtomicIterations, kOrdinaryIterations,
      static_cast<unsigned long long>(hashBytes(atomic_result[0])), static_cast<unsigned long long>(hashBytes(atomic_result[1])),
      static_cast<unsigned long long>(hashBytes(same_view ? atomic_result[1] : ordinary_result[0])), g_failures);
  end("atomic64_context_exact_buffers");
  marker(name, "complete");
}
// Every application-written resource has one queue writer. In the negative
// control only the two u2.Load views share 16 bytes. They are UAVs in the
// phase import/export plan, so its conservative physical domains overlap.
void runAtomicPair(ID3D12Device *device, ID3D12RootSignature *root, ID3D12PipelineState *pso, bool overlap) {
  const char *name = overlap ? "atomic_pair_shared_read_view" : "atomic_pair_disjoint";
  marker(name, "setup");
  Gpu first(device), second(device);
  auto initial = [&](uint32_t side, uint32_t lane) { return initialCell(lane) ^ (uint64_t(side) << 59); };
  auto a = filled(kCellBytes), b = a;
  for (uint32_t lane = 0; lane < kLanes; ++lane) {
    store(a, lane * 8, initial(0, lane)); store(b, lane * 8, initial(1, lane));
  }
  auto ca = first.uavBuffer(a), cb = second.uavBuffer(b);
  auto oa = first.uavBuffer(filled(kOutputBytes)), ob = second.uavBuffer(filled(kOutputBytes));
  constexpr uint32_t view_bytes = kLanes * sizeof(uint64_t), offset = view_bytes - 16;
  auto seed = filled(view_bytes + offset);
  for (unsigned j = 0; j < seed.size() / 8; ++j) store(seed, j * 8, uint64_t(0xabc0000000000000ULL + j));
  ComPtr<ID3D12Resource> reads;
  if (overlap) reads = first.uavBuffer(seed);
  first.finish({}, nullptr); second.finish({}, nullptr);
  bind(first, root, pso, oa.Get(), ca.Get(), true);
  bind(second, root, pso, ob.Get(), cb.Get(), true);
  if (overlap) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS; d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = view_bytes / 4; d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(reads.Get(), nullptr, &d, first.cpu(2));
    d.Buffer.FirstElement = offset / 4;
    device->CreateUnorderedAccessView(reads.Get(), nullptr, &d, second.cpu(2));
  }
  check(first.list->Close(), "pair first close"); check(second.list->Close(), "pair second close");
  marker(name, "execute");
  auto done_a = submit(first); Sleep(2); auto done_b = submit(second);
  wait(first, done_a); wait(second, done_b);
  marker(name, "retired");
  reset(first); reset(second);
  auto ra = overlap ? first.finish({ca.Get(), oa.Get(), reads.Get()}, nullptr) : first.finish({ca.Get(), oa.Get()}, nullptr);
  auto rb = second.finish({cb.Get(), ob.Get()}, nullptr);
  auto out_a = filled(kOutputBytes);
  auto out_b = filled(kOutputBytes);
  for (unsigned side = 0; side < 2; ++side) {
    auto &cells = side ? b : a;
    auto &out = side ? out_b : out_a;
    for (unsigned lane = 0; lane < kLanes; ++lane) {
      const auto old = initial(side, lane), final = uint64_t(old + uint64_t(kAtomicIterations) * 0x100000001ULL);
      store(cells, lane * 8, final); store(out, 32 + lane * 24, old);
      store(out, 40 + lane * 24, final);
      store(out, 48 + lane * 24, overlap ? load<uint64_t>(seed, (side ? offset : 0) + lane * 8) : final);
    }
  }
  begin();
  compareBytes(name, "first cells", ra[0], a); compareBytes(name, "second cells", rb[0], b);
  compareBytes(name, "first output", ra[1], out_a);
  compareBytes(name, "second output", rb[1], out_b);
  if (overlap) compareBytes(name, "shared read-only input", ra[2], seed);
  std::printf("OWNED_D3D12_ATOMIC64_CONTEXT_DOMAIN case=%s lanes=64 read_view_overlap=%u cells_overlap=0 "
      "bytes=%zu failures=%d\n", name, overlap ? 16 : 0, a.size()+b.size()+out_a.size()+out_b.size()+(overlap?seed.size():0), g_failures);
  end("atomic64_private_domains");
  marker(name, "complete");
}
} // namespace

int wmain() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0,
      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 4};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 2;
  desc.Desc_1_1.pParameters = parameters;
  auto root = rootSignature(device.Get(), desc);
  ComPtr<ID3D12PipelineState> atomic, ordinary;
  check(computePso(device.Get(), root.Get(), readFile(L"atomic64-context-atomic.dxil"), atomic), "atomic context pipeline");
  check(computePso(device.Get(), root.Get(), readFile(L"atomic64-context-ordinary.dxil"), ordinary), "ordinary context pipeline");
  std::puts("OWNED_D3D12_ATOMIC64_CONTEXT_BACKEND id=owned-graphics-d3d12");
  {
    // Compile the native phase helpers and exercise both pipelines before the
    // bounded overlap cases. No observation from this warmup proves overlap.
    marker("warmup", "execute");
    Gpu warm(device.Get());
    auto cells = warm.uavBuffer(filled(kCellBytes));
    auto output = warm.uavBuffer(filled(kOutputBytes));
    auto scratch = warm.uavBuffer(filled(kCellBytes));
    bind(warm, root.Get(), atomic.Get(), output.Get(), cells.Get(), true, 1);
    bind(warm, root.Get(), ordinary.Get(), scratch.Get(), scratch.Get(), false, 1);
    warm.finish({}, nullptr);
    marker("warmup", "complete");
  }
  runCase(device.Get(), root.Get(), atomic.Get(), ordinary.Get(), "ordinary_first_disjoint", false, false, false);
  runCase(device.Get(), root.Get(), atomic.Get(), ordinary.Get(), "atomic_first_disjoint", true, false, true);
  runCase(device.Get(), root.Get(), atomic.Get(), ordinary.Get(), "ordinary_first_overlap", false, true, false);
  runCase(device.Get(), root.Get(), atomic.Get(), ordinary.Get(), "atomic_first_overlap", true, true, false);
  if (GetEnvironmentVariableA("OWNED_D3D12_ATOMIC64_DOMAINS", nullptr, 0)) {
    runAtomicPair(device.Get(), root.Get(), atomic.Get(), false);
    runAtomicPair(device.Get(), root.Get(), atomic.Get(), true);
  }
  std::printf("OWNED_D3D12_ATOMIC64_CONTEXT_RESULT cases=4 lanes=128 atomic_executions=5 warmup_atomic=1 failures=%d\n", g_failures);
  if (!g_failures) std::puts("OWNED_D3D12_ATOMIC64_CONTEXT_PASS");
  return g_failures ? 1 : 0;
}
