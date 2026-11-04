#define main retained_typed_fixture_main
#include "owned-d3d12-atomic64-workgraph-typed.cpp"
#undef main
#include <algorithm>

namespace {
constexpr size_t GuardBytes = 64;
constexpr UINT MaxGroups = 66;
constexpr size_t Slots = MaxGroups * 4 * 2;
constexpr size_t OutputPayloadBytes = 32 + Slots * 8;
struct FrontierSeed { UINT grid[3]; UINT tag; UINT scratch; };
struct SharedSeed { UINT grid[3]; UINT tag; };
static_assert(sizeof(FrontierSeed) == 20 && sizeof(SharedSeed) == 16);

void Put32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
  Require(offset + sizeof(value) <= bytes.size(), "oracle write bounds");
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
std::vector<uint8_t> GuardedZero(size_t payload) {
  std::vector<uint8_t> bytes(GuardBytes + payload + GuardBytes, 0);
  for (UINT i = 0; i < GuardBytes / 4; ++i) {
    Put32(bytes, i * 4, 0x12345678u ^ (i * 0x9e3779b9u));
    Put32(bytes, GuardBytes + payload + i * 4, 0xfedcba98u ^ (i * 0x85ebca6bu));
  }
  return bytes;
}
std::vector<uint8_t> ActiveOracle(const FrontierSeed &seed, UINT &leaves) {
  auto oracle = GuardedZero(OutputPayloadBytes);
  const UINT groups = seed.grid[0] * seed.grid[1] * seed.grid[2];
  Require(groups && groups <= MaxGroups && seed.grid[2] == 1, "fixture input bounds");
  leaves = 0;
  for (UINT group = 0; group < groups; ++group) {
    const UINT x = group % seed.grid[0], y = group / seed.grid[0];
    for (UINT lane = 0; lane < 4; ++lane) {
      const UINT childGroups = lane == 0 ? 0 : 1 + ((group + lane) & 1);
      const UINT value = seed.tag ^ (x + y * 65536) ^ ((lane + 1) * 0x9e3779b9u);
      for (UINT child = 0; child < childGroups; ++child) {
        const size_t slot = (size_t(group) * 4 + lane) * 2 + child;
        Put32(oracle, GuardBytes + 32 + slot * 8, 1);
        Put32(oracle, GuardBytes + 36 + slot * 8, value ^ (child * 0x85ebca6bu));
        ++leaves;
      }
    }
  }
  Put32(oracle, GuardBytes, leaves);
  Put32(oracle, GuardBytes + 4, 1);
  Put32(oracle, GuardBytes + 8, groups);
  return oracle;
}

struct FrontierFixture {
  ComPtr<ID3D12Device5> device;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12StateObject> state;
  ComPtr<ID3D12StateObjectProperties1> ids;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12Resource> output, outputUpload, outputReadback;
  ComPtr<ID3D12Resource> backing, backingUpload, backingReadback;
  ComPtr<ID3D12Resource> input, inputUpload;
  D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{};
  std::vector<uint8_t> emptyOutput, emptyBacking;
  UINT activeEntry = UINT_MAX, zeroEntry = UINT_MAX;
  UINT64 serial = 0;
  HANDLE event = nullptr;

  FrontierFixture() {
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &parameter}; ComPtr<ID3DBlob> blob;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "root blob");
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    auto shader = Shader("workgraph-frontier.dxil");
    D3D12_DXIL_LIBRARY_DESC library{{shader.data(), shader.size()}, 0, nullptr};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()}; D3D12_WORK_GRAPH_DESC graph{};
    graph.ProgramName = L"frontier"; graph.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    std::array<D3D12_STATE_SUBOBJECT, 3> sub{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &graph}}};
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_EXECUTABLE, UINT(sub.size()), sub.data()};
    Check(device->CreateStateObject(&desc, IID_PPV_ARGS(&state)), "frontier program");
    ComPtr<ID3D12WorkGraphProperties> properties; Check(state.As(&properties), "properties");
    Check(state.As(&ids), "program identifiers");
    const UINT graphIndex = properties->GetWorkGraphIndex(L"frontier");
    Require(graphIndex != UINT_MAX && properties->GetNumNodes(graphIndex) == 6 &&
            properties->GetNumEntrypoints(graphIndex) == 2, "frontier topology");
    activeEntry = properties->GetEntrypointIndex(graphIndex, {L"frontier", 0});
    zeroEntry = properties->GetEntrypointIndex(graphIndex, {L"zero_source", 0});
    Require(activeEntry != UINT_MAX && zeroEntry != UINT_MAX &&
            properties->GetEntrypointRecordSizeInBytes(graphIndex, activeEntry) == sizeof(FrontierSeed) &&
            properties->GetEntrypointRecordSizeInBytes(graphIndex, zeroEntry) == sizeof(SharedSeed), "record ABI");
    properties->GetWorkGraphMemoryRequirements(graphIndex, &memory);
    Require(memory.MinSizeInBytes && memory.MaxSizeInBytes >= memory.MinSizeInBytes &&
            memory.MinSizeInBytes <= SIZE_MAX - GuardBytes * 2, "backing requirements");
    emptyOutput = GuardedZero(OutputPayloadBytes); emptyBacking = GuardedZero(size_t(memory.MinSizeInBytes));
    output = Buffer(device.Get(), emptyOutput.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    outputUpload = Buffer(device.Get(), emptyOutput.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    outputReadback = Buffer(device.Get(), emptyOutput.size(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    backing = Buffer(device.Get(), emptyBacking.size(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    backingUpload = Buffer(device.Get(), emptyBacking.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    backingReadback = Buffer(device.Get(), emptyBacking.size(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    input = Buffer(device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    inputUpload = Buffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    Upload(outputUpload.Get(), emptyOutput.data(), emptyOutput.size());
    Upload(backingUpload.Get(), emptyBacking.data(), emptyBacking.size());
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "event");
    std::printf("OWNED_D3D12_WG_FRONTIER_MEMORY exact_min_range=%llu max=%llu guards=64,64 slots=%zu\n",
        (unsigned long long)memory.MinSizeInBytes, (unsigned long long)memory.MaxSizeInBytes, Slots);
  }
  ~FrontierFixture() { if (event) CloseHandle(event); }

  void SetInput(UINT entry, const void *record, UINT bytes) {
    std::array<uint8_t, 256> data{}; Require(bytes <= 64, "input payload size");
    std::memcpy(data.data(), record, bytes);
    D3D12_NODE_GPU_INPUT descriptor{entry, 1, {input->GetGPUVirtualAddress(), bytes}};
    std::memcpy(data.data() + 64, &descriptor, sizeof(descriptor));
    Upload(inputUpload.Get(), data.data(), data.size());
  }

  ComPtr<ID3D12GraphicsCommandList10> Record(ComPtr<ID3D12CommandAllocator> &allocator,
      UINT entry, const void *record, UINT bytes, bool gpu) {
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    ComPtr<ID3D12GraphicsCommandList10> list;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    list->CopyBufferRegion(output.Get(), 0, outputUpload.Get(), 0, emptyOutput.size());
    list->CopyBufferRegion(backing.Get(), 0, backingUpload.Get(), 0, emptyBacking.size());
    list->CopyBufferRegion(input.Get(), 0, inputUpload.Get(), 0, 256);
    std::array<D3D12_RESOURCE_BARRIER, 3> barriers{};
    const std::array<ID3D12Resource *, 3> resources{{output.Get(), backing.Get(), input.Get()}};
    for (size_t i = 0; i < barriers.size(); ++i) {
      barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barriers[i].Transition = {resources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    }
    barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress() + GuardBytes);
    D3D12_SET_PROGRAM_DESC program{}; program.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    ids->GetProgramIdentifier(&program.WorkGraph.ProgramIdentifier, L"frontier");
    program.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
    program.WorkGraph.BackingMemory = {backing->GetGPUVirtualAddress() + GuardBytes, memory.MinSizeInBytes};
    list->SetProgram(&program);
    std::vector<uint8_t> retained(bytes); std::memcpy(retained.data(), record, bytes);
    D3D12_DISPATCH_GRAPH_DESC dispatch{};
    dispatch.Mode = gpu ? D3D12_DISPATCH_MODE_NODE_GPU_INPUT : D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    if (gpu) dispatch.NodeGPUInput = input->GetGPUVirtualAddress() + 64;
    else dispatch.NodeCPUInput = {entry, 1, retained.data(), bytes};
    list->DispatchGraph(&dispatch);
    std::fill(retained.begin(), retained.end(), uint8_t(0xa5)); // CPU records must already be retained.
    for (auto &barrier : barriers) {
      barrier.Transition.StateBefore = barrier.Transition.StateAfter;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, emptyOutput.size());
    list->CopyBufferRegion(backingReadback.Get(), 0, backing.Get(), 0, emptyBacking.size());
    for (auto &barrier : barriers) {
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    list->ResourceBarrier(UINT(barriers.size()), barriers.data());
    Check(list->Close(), "close"); return list;
  }

  void Execute(ID3D12GraphicsCommandList10 *list, const std::vector<uint8_t> &oracle) {
    ID3D12CommandList *lists[]{list}; queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence.Get(), ++serial), "signal");
    Check(fence->SetEventOnCompletion(serial, event), "completion event");
    Require(WaitForSingleObject(event, 180000) == WAIT_OBJECT_0, "GPU completion");
    Check(device->GetDeviceRemovedReason(), "device alive");
    void *mapped = nullptr; D3D12_RANGE outputRange{0, emptyOutput.size()};
    Check(outputReadback->Map(0, &outputRange, &mapped), "output readback");
    const auto *actual = static_cast<const uint8_t *>(mapped); bool exact = true;
    for (size_t i = 0; i < oracle.size(); ++i) if (actual[i] != oracle[i]) {
      std::fprintf(stderr, "OWNED_D3D12_WG_FRONTIER_MISMATCH offset=%zu actual=%02x expected=%02x\n", i, actual[i], oracle[i]);
      exact = false; break;
    }
    outputReadback->Unmap(0, nullptr); Require(exact, "all output bytes and guards exact");
    D3D12_RANGE backingRange{0, emptyBacking.size()};
    Check(backingReadback->Map(0, &backingRange, &mapped), "backing guards readback");
    actual = static_cast<const uint8_t *>(mapped);
    const bool guards = std::equal(emptyBacking.begin(), emptyBacking.begin() + GuardBytes, actual) &&
        std::equal(emptyBacking.end() - GuardBytes, emptyBacking.end(), actual + emptyBacking.size() - GuardBytes);
    backingReadback->Unmap(0, nullptr); Require(guards, "exact minimum backing range guards");
  }

  void ReportActive(const char *name, const FrontierSeed &seed, UINT leaves, bool gpu, bool replay) {
    const UINT groups = seed.grid[0] * seed.grid[1] * seed.grid[2];
    std::printf("OWNED_D3D12_WG_FRONTIER_CASE name=%s gpu_input=%u replay=%u groups=%u leaves=%u "
        "zero_children=%u sharing_last=1 sharing_count=%u payload=exact hits=exact guards=ok tag=0x%08x\n",
        name, unsigned(gpu), unsigned(replay), groups, leaves, groups, groups, seed.tag);
  }
};
}

int main(int argc, char **argv) { try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool activeOnly = argc == 2 && std::strcmp(argv[1], "--active-only") == 0;
  Require(argc == 1 || activeOnly, "arguments");
  auto module = GetModuleHandleW(L"d3d12.dll");
  auto backend = reinterpret_cast<const char *(WINAPI *)()>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  Require(backend && backend() && !std::strcmp(backend(), "owned-graphics-d3d12"), "backend");
  std::printf("OWNED_D3D12_WG_FRONTIER_BACKEND id=%s\n", backend());
  FrontierFixture fixture;
  const FrontierSeed cpu{{13, 5, 1}, 0x13579bdfu, 0}; UINT leaves = 0;
  auto oracle = ActiveOracle(cpu, leaves); fixture.SetInput(fixture.activeEntry, &cpu, sizeof(cpu));
  ComPtr<ID3D12CommandAllocator> cpuAllocator;
  auto cpuList = fixture.Record(cpuAllocator, fixture.activeEntry, &cpu, sizeof(cpu), false);
  fixture.Execute(cpuList.Get(), oracle); fixture.ReportActive("cpu", cpu, leaves, false, false);
  const FrontierSeed gpu{{11, 6, 1}, 0x2468ace0u, 0};
  oracle = ActiveOracle(gpu, leaves); fixture.SetInput(fixture.activeEntry, &gpu, sizeof(gpu));
  ComPtr<ID3D12CommandAllocator> gpuAllocator;
  auto gpuList = fixture.Record(gpuAllocator, fixture.activeEntry, &gpu, sizeof(gpu), true);
  fixture.Execute(gpuList.Get(), oracle); fixture.ReportActive("gpu", gpu, leaves, true, false);
  const FrontierSeed replay{{7, 3, 1}, 0xdeadbeefu, 0};
  oracle = ActiveOracle(replay, leaves); fixture.SetInput(fixture.activeEntry, &replay, sizeof(replay));
  fixture.Execute(gpuList.Get(), oracle); fixture.ReportActive("gpu_replay", replay, leaves, true, true);
  if (!activeOnly) {
    const SharedSeed zero{{0, 1, 1}, 0x10293847u};
    oracle = GuardedZero(OutputPayloadBytes);
    Put32(oracle, GuardBytes + 16, 1); Put32(oracle, GuardBytes + 20, zero.tag);
    fixture.SetInput(fixture.zeroEntry, &zero, sizeof(zero));
    ComPtr<ID3D12CommandAllocator> zeroAllocator;
    auto zeroList = fixture.Record(zeroAllocator, fixture.zeroEntry, &zero, sizeof(zero), false);
    fixture.Execute(zeroList.Get(), oracle);
    std::puts("OWNED_D3D12_WG_FRONTIER_CASE name=zero_shared source_groups=0 shared_groups=1 payload=exact hits=exact guards=ok");
  }
  std::printf("OWNED_D3D12_WG_FRONTIER_PASS cases=%u active_cases=3 zero_shared=%u default_gpu_input=1 closed_list_replay=1 exact_min_range=1\n",
      activeOnly ? 3u : 4u, activeOnly ? 0u : 1u);
  return 0;
} catch (...) { std::puts("OWNED_D3D12_WG_FRONTIER_FAIL"); return 1; } }
