#define wmain sm6_fixture_wmain
#include "owned-d3d12-sm6.cpp"
#undef wmain

namespace {

void bindPhase(Gpu &gpu, ID3D12RootSignature *root, ID3D12PipelineState *pipeline,
               ID3D12Resource *out, ID3D12Resource *cells, ID3D12Resource *arguments, uint32_t mode = 0) {
  gpu.rawUav(0, out);
  gpu.structuredUav(1, cells, 8);
  gpu.rawUav(2, cells);
  gpu.rawUav(3, arguments);
  gpu.list->SetComputeRootSignature(root);
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetComputeRootDescriptorTable(0, gpu.gpu(0));
  const uint32_t constants[] = {0, 33, 0xface0000, mode};
  gpu.list->SetComputeRoot32BitConstants(2, 4, constants, 0);
  gpu.list->SetPipelineState(pipeline);
}

void phaseBarrier(Gpu &gpu, ID3D12Resource *resource) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  gpu.list->ResourceBarrier(1, &barrier);
}

void checkPhase(const char *name, bool work, bool mixed, const std::vector<uint8_t> &returned,
                const std::vector<uint8_t> &observed, const std::vector<uint8_t> &copied) {
  begin();
  for (uint32_t i = 0; i < 8; ++i) {
    const uint64_t initial = (uint64_t(0x12345678 + i) << 32) | (0xfffffff0 + i);
    const bool touched = work && i < 4;
    const uint64_t expected = initial + (touched ? 0x100000000ULL + (mixed && i >= 2 ? 65 : 33) : 0);
    const uint64_t canonical = load<uint64_t>(copied, i * 8);
    const uint64_t ordinary = load<uint64_t>(observed, 128 + i * 8);
    if (i < 4) {
      const uint64_t old = load<uint64_t>(returned, i * 16);
      const uint64_t final = load<uint64_t>(returned, i * 16 + 8);
      const uint32_t root = load<uint32_t>(returned, 64 + i * 4);
      EXPECT_EQ(name, "return old", i, old, touched ? initial : 0xcdcdcdcdcdcdcdcdULL);
      EXPECT_EQ(name, "return final", i, final, touched ? expected : 0xcdcdcdcdcdcdcdcdULL);
      EXPECT_EQ(name, "root constant", i, root, touched ? 0xface0000 + (mixed && i >= 2) : kSentinel);
      std::printf("OWNED_D3D12_ATOMIC64_PHASE_RETURN case=%s index=%u old=0x%016llx final=0x%016llx root=%08x\n",
          name, i, static_cast<unsigned long long>(old), static_cast<unsigned long long>(final), root);
    }
    EXPECT_EQ(name, "canonical copy", i, canonical, expected);
    EXPECT_EQ(name, "ordinary32", i, ordinary, expected);
    std::printf("OWNED_D3D12_ATOMIC64_PHASE_CELL case=%s index=%u copy=0x%016llx ordinary=0x%016llx\n",
        name, i, static_cast<unsigned long long>(canonical), static_cast<unsigned long long>(ordinary));
  }
  const uint32_t count = load<uint32_t>(returned, 96), guard = load<uint32_t>(returned, 100);
  EXPECT_EQ(name, "work count", 0, count, work ? 4 : 0);
  EXPECT_EQ(name, "adjacent guard", 0, guard, 0xa11c0ded);
  std::printf("OWNED_D3D12_ATOMIC64_PHASE_COUNTS case=%s count=%u guard=%08x\n", name, count, guard);
  const uint32_t offsets[] = {192, 196, 200, 204, 208, 212, 216};
  const uint32_t source_offsets[] = {4, 8, 12, 16, 4, 8, 4};
  for (uint32_t i = 0; i < 7; ++i) {
    const auto value = load<uint32_t>(observed, offsets[i]);
    EXPECT_EQ(name, "unaligned/vector raw32", i, value, load<uint32_t>(copied, source_offsets[i]));
    std::printf("OWNED_D3D12_ATOMIC64_PHASE_RAW case=%s index=%u word=%08x\n", name, i, value);
  }
  end(name);
}

void indirectCases(ID3D12Device *device, ID3D12RootSignature *root, ID3D12PipelineState *prepare,
                   ID3D12PipelineState *returning, ID3D12PipelineState *observe) {
  Gpu gpu(device);
  D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
  args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  args[0].Constant = {2, 0, 4};
  args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{28, 2, args, 0};
  ComPtr<ID3D12CommandSignature> signature;
  check(device->CreateCommandSignature(&signature_desc, root, IID_PPV_ARGS(&signature)), "indirect signature");
  const char *names[] = {"indirect_nonzero", "indirect_count_zero", "indirect_dimension_zero", "indirect_predicate_zero"};
  for (uint32_t mode = 0; mode < 4; ++mode) {
    auto output = gpu.uavBuffer(filled(256));
    auto cells = gpu.uavBuffer(filled(64));
    auto arguments = gpu.uavBuffer(filled(128));
    bindPhase(gpu, root, prepare, output.Get(), cells.Get(), arguments.Get(), mode);
    gpu.list->Dispatch(1, 1, 1);
    phaseBarrier(gpu, nullptr);
    gpu.transition(arguments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    gpu.list->SetPipelineState(returning);
    gpu.list->SetPredication(arguments.Get(), 104, D3D12_PREDICATION_OP_EQUAL_ZERO);
    gpu.list->ExecuteIndirect(signature.Get(), 3, arguments.Get(), 0, arguments.Get(), 96);
    gpu.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    phaseBarrier(gpu, nullptr);
    gpu.list->SetPipelineState(observe);
    gpu.list->Dispatch(1, 1, 1);
    gpu.transition(arguments.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto result = gpu.finish({output.Get(), cells.Get(), arguments.Get()}, nullptr);
    checkPhase(names[mode], mode == 0, true, result[0], result[0], result[1]);
    begin();
    for (uint32_t command = 0; command < 3; ++command) {
      const uint32_t expected[] = {command * 2, command ? 65u : 33u, 0xface0000 + command, 0,
          mode == 2 ? 0u : (command ? 1u : 2u), command == 1 ? 2u : 1u, 1};
      for (uint32_t word = 0; word < 7; ++word) {
        const auto actual = load<uint32_t>(result[2], command * 28 + word * 4);
        EXPECT_EQ(names[mode], "GPU argument", command * 7 + word, actual, expected[word]);
        std::printf("OWNED_D3D12_ATOMIC64_PHASE_ARGUMENT case=%s index=%u word=%08x\n", names[mode], command * 7 + word, actual);
      }
    }
    const auto count = load<uint32_t>(result[2], 96);
    const auto predicate = load<uint64_t>(result[2], 104);
    EXPECT_EQ(names[mode], "GPU count", 0, count, mode == 1 ? 0 : 2);
    EXPECT_EQ(names[mode], "GPU predicate", 0, predicate, mode == 3 ? 0 : 1);
    std::printf("OWNED_D3D12_ATOMIC64_PHASE_CONTROL case=%s count=%u predicate=%llu\n", names[mode], count, static_cast<unsigned long long>(predicate));
    end("atomic64_gpu_arguments");
  }
}

void crossQueueCases(ID3D12Device *device, ID3D12RootSignature *root, ID3D12PipelineState *prepare,
                     ID3D12PipelineState *returning, ID3D12PipelineState *observe) {
  Gpu producer(device), consumer(device);
  auto properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 64;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> cells, opened;
  check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
      nullptr, IID_PPV_ARGS(&cells)), "shared atomic buffer");
  HANDLE handle = nullptr;
  check(device->CreateSharedHandle(cells.Get(), nullptr, GENERIC_ALL, nullptr, &handle), "share atomic buffer");
  check(device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened)), "open atomic buffer");
  CloseHandle(handle);
  ComPtr<ID3D12Fence> fence, remote;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), "shared handoff fence");
  check(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle), "share handoff fence");
  check(device->OpenSharedHandle(handle, IID_PPV_ARGS(&remote)), "open handoff fence");
  CloseHandle(handle);
  const char *names[] = {"crossqueue_atomic_to_ordinary", "crossqueue_ordinary_to_atomic"};
  for (uint32_t direction = 0; direction < 2; ++direction) {
    auto producer_out = producer.uavBuffer(filled(256));
    auto producer_args = producer.uavBuffer(filled(128));
    auto consumer_data = filled(256);
    const uint32_t zero = 0, guard = 0xa11c0ded;
    std::memcpy(consumer_data.data() + 96, &zero, 4);
    std::memcpy(consumer_data.data() + 100, &guard, 4);
    auto consumer_out = consumer.uavBuffer(consumer_data);
    auto consumer_args = consumer.uavBuffer(filled(128));
    producer.finish({}, nullptr);
    consumer.finish({}, nullptr);
    const UINT64 target = direction ? 7 : 10;
    const UINT64 before = remote->GetCompletedValue();
    HANDLE crossing = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!crossing) die("handoff crossing event");
    check(remote->SetEventOnCompletion(target, crossing), "register future handoff event");
    // Future consumer is submitted first. A worker holding exclusive phase
    // admission across this wait deadlocks the later, sole writer. No CPU wait.
    check(consumer.queue->Wait(remote.Get(), target), "enqueue future consumer wait");
    consumer.transition(opened.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bindPhase(consumer, root, direction ? returning : observe, consumer_out.Get(), opened.Get(), consumer_args.Get());
    if (direction) {
      consumer.list->Dispatch(4, 1, 1);
      phaseBarrier(consumer, nullptr);
    }
    consumer.list->SetPipelineState(observe);
    consumer.list->Dispatch(1, 1, 1);
    auto canonical = consumer.buffer(64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    consumer.transition(opened.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    consumer.list->CopyBufferRegion(canonical.Get(), 0, opened.Get(), 0, 64);
    consumer.transition(opened.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    check(consumer.list->Close(), "close future consumer");
    ID3D12CommandList *consumer_lists[] = {consumer.list.Get()};
    consumer.queue->ExecuteCommandLists(1, consumer_lists);
    const UINT64 done = ++consumer.fenceValue;
    check(consumer.queue->Signal(consumer.fence.Get(), done), "future consumer completion marker");
    begin();
    EXPECT_EQ(names[direction], "logical value before producer", 0, before, direction ? 3 : 0);
    const UINT64 pending = remote->GetCompletedValue(), completed = consumer.fence->GetCompletedValue();
    const DWORD event_before = WaitForSingleObject(crossing, 0);
    EXPECT_EQ(names[direction], "uncompleted producer", 0, pending, before);
    EXPECT_EQ(names[direction], "asynchronous future consumer", 0, completed, done - 1);
    EXPECT_EQ(names[direction], "uncompleted crossing", 0, event_before, WAIT_TIMEOUT);
    std::printf("OWNED_D3D12_ATOMIC64_PHASE_ASYNC case=%s pending=%llu completed=%llu submitted=%llu event=%lu\n",
        names[direction], static_cast<unsigned long long>(pending), static_cast<unsigned long long>(completed),
        static_cast<unsigned long long>(done), static_cast<unsigned long>(event_before));
    end("atomic64_future_wait_async");
    producer.transition(cells.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bindPhase(producer, root, prepare, producer_out.Get(), cells.Get(), producer_args.Get());
    producer.list->Dispatch(1, 1, 1);
    phaseBarrier(producer, nullptr);
    if (!direction) {
      producer.list->SetPipelineState(returning);
      producer.list->Dispatch(4, 1, 1);
      phaseBarrier(producer, nullptr);
    }
    producer.transition(cells.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    check(producer.list->Close(), "close late producer");
    ID3D12CommandList *producer_lists[] = {producer.list.Get()};
    producer.queue->ExecuteCommandLists(1, producer_lists);
    check(producer.queue->Signal(fence.Get(), target), "late producer logical handoff");
    check(consumer.fence->SetEventOnCompletion(done, consumer.event), "consumer completion event");
    if (WaitForSingleObject(consumer.event, kFenceTimeoutMs) != WAIT_OBJECT_0) die("future wait admission deadlock");
    const UINT64 after = remote->GetCompletedValue();
    begin();
    EXPECT_EQ(names[direction], "actual producer retirement", 0, after, target);
    const DWORD event_after = WaitForSingleObject(crossing, 0);
    EXPECT_EQ(names[direction], "threshold crossing", 0, event_after, WAIT_OBJECT_0);
    check(remote->Signal(3), "logical fence rewind");
    const UINT64 rewind = fence->GetCompletedValue();
    EXPECT_EQ(names[direction], "shared logical rewind", 0, rewind, 3);
    std::printf("OWNED_D3D12_ATOMIC64_PHASE_FENCE case=%s before=%llu after=%llu rewind=%llu event=%lu wait_first=1\n",
        names[direction], static_cast<unsigned long long>(before), static_cast<unsigned long long>(after),
        static_cast<unsigned long long>(rewind), static_cast<unsigned long>(event_after));
    end("atomic64_logical_handoff_rewind");
    CloseHandle(crossing);
    check(consumer.allocator->Reset(), "consumer allocator reset");
    check(consumer.list->Reset(consumer.allocator.Get(), nullptr), "consumer list reset");
    check(producer.allocator->Reset(), "producer allocator reset");
    check(producer.list->Reset(producer.allocator.Get(), nullptr), "producer list reset");
    const auto producer_result = producer.finish({producer_out.Get()}, nullptr);
    const auto consumer_result = consumer.finish({consumer_out.Get()}, nullptr);
    std::vector<uint8_t> copied(64);
    void *mapped = nullptr;
    D3D12_RANGE range{0, 64};
    check(canonical->Map(0, &range, &mapped), "map canonical handoff copy");
    std::memcpy(copied.data(), mapped, 64);
    canonical->Unmap(0, nullptr);
    checkPhase(names[direction], true, false, direction ? consumer_result[0] : producer_result[0], consumer_result[0], copied);
  }
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
  Gpu gpu(device.Get());
  std::printf("OWNED_D3D12_ATOMIC64_PHASE_BACKEND id=owned-graphics-d3d12\n");
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0,
      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants = {0, 0, 4};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 3;
  root_desc.Desc_1_1.pParameters = params;
  auto root = rootSignature(device.Get(), root_desc);
  ComPtr<ID3D12PipelineState> pipeline;
  check(computePso(device.Get(), root.Get(), readFile(L"atomic64-phase.dxil"), pipeline), "CreateComputePipelineState");

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 64;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
  D3D12_HEAP_DESC heap_desc{};
  heap_desc.SizeInBytes = allocation.SizeInBytes;
  heap_desc.Alignment = allocation.Alignment;
  heap_desc.Properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ComPtr<ID3D12Heap> memory;
  check(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&memory)), "CreateHeap");
  ComPtr<ID3D12Resource> cells, alias;
  check(device->CreatePlacedResource(memory.Get(), 0, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr, IID_PPV_ARGS(&cells)), "CreatePlacedResource(cells)");
  check(device->CreatePlacedResource(memory.Get(), 0, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      nullptr, IID_PPV_ARGS(&alias)), "CreatePlacedResource(alias)");
  auto input = gpu.buffer(64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint64_t *mapped_input = nullptr;
  check(input->Map(0, nullptr, reinterpret_cast<void **>(&mapped_input)), "Map(persistent input)");
  std::memset(mapped_input, 0, 64);

  for (uint32_t iteration = 0; iteration != 2; ++iteration) {
    begin();
    auto output = gpu.uavBuffer(filled(64));
    if (iteration) {
      gpu.transition(alias.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    gpu.rawUav(0, output.Get());
    gpu.structuredUav(1, cells.Get(), 8);
    gpu.rawUav(2, alias.Get());
    gpu.list->SetPipelineState(pipeline.Get());
    gpu.list->SetComputeRootSignature(root.Get());
    ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
    gpu.list->SetDescriptorHeaps(1, heaps);
    gpu.list->SetComputeRootDescriptorTable(0, gpu.gpu(0));
    gpu.list->SetComputeRootShaderResourceView(1, input->GetGPUVirtualAddress());
    gpu.list->Dispatch(1, 1, 1);
    // Legal CPU write after recording, before queue execution. Keep Map open
    // across both submissions; no per-store interception may be required.
    const uint64_t initial = iteration ? 0x1234567800000003ULL : 0x5555555500000000ULL;
    mapped_input[0] = initial;
    const auto result = gpu.finish({output.Get(), alias.Get()}, nullptr);
    EXPECT_EQ("phase", "mapped import", iteration, load<uint64_t>(result[0], 0), initial);
    EXPECT_EQ("phase", "placed raw alias", iteration, load<uint64_t>(result[0], 8), initial + 0x100000001ULL);
    const uint64_t modified = 0x8765432100000000ULL | uint32_t(initial + 1);
    EXPECT_EQ("phase", "ordinary store coherence", iteration, load<uint64_t>(result[0], 16), modified);
    EXPECT_EQ("phase", "CAS old", iteration, load<uint64_t>(result[0], 24), modified);
    EXPECT_EQ("phase", "CAS alias", iteration, load<uint64_t>(result[0], 32), 0x123456789abcdef0ULL);
    EXPECT_EQ("phase", "GPU export then transition/copy", iteration, load<uint64_t>(result[1], 0), 0x123456789abcdef0ULL);
    std::printf("OWNED_D3D12_ATOMIC64_PHASE_MAP iteration=%u words=", iteration);
    for (uint32_t word = 0; word < 5; ++word)
      std::printf("%016llx,", static_cast<unsigned long long>(load<uint64_t>(result[0], word * 8)));
    std::printf("%016llx\n", static_cast<unsigned long long>(load<uint64_t>(result[1], 0)));
    end("atomic64_phase_aliases_and_persistent_map");
  }
  input->Unmap(0, nullptr);
  ComPtr<ID3D12PipelineState> prepare, returning, observe;
  check(computePso(device.Get(), root.Get(), readFile(L"atomic64-phase-prepare.dxil"), prepare), "prepare pipeline");
  check(computePso(device.Get(), root.Get(), readFile(L"atomic64-phase-returning.dxil"), returning), "returning pipeline");
  check(computePso(device.Get(), root.Get(), readFile(L"atomic64-phase-observe32.dxil"), observe), "ordinary32 pipeline");
  indirectCases(device.Get(), root.Get(), prepare.Get(), returning.Get(), observe.Get());
  crossQueueCases(device.Get(), root.Get(), prepare.Get(), returning.Get(), observe.Get());
  std::printf("OWNED_D3D12_ATOMIC64_PHASE_RESULT failures=%d\n", g_failures);
  if (!g_failures) std::printf("OWNED_D3D12_ATOMIC64_PHASE_PASS indirect=4 crossqueue=2\n");
  return g_failures ? 1 : 0;
}
