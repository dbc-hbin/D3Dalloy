#define main retained_typed_fixture_main
#include "owned-d3d12-atomic64-workgraph-typed.cpp"
#undef main

namespace {
constexpr UINT TextureRecords = 16;
constexpr size_t TextureGuardWords = 4;
constexpr size_t TexturePayloadWords = TextureRecords * 4 + 2;
constexpr size_t TextureOutputWords = TextureGuardWords * 2 + TexturePayloadWords;
constexpr UINT TextureBase = 0x12340000u;
constexpr UINT TextureStep = 0x00000101u;
constexpr UINT TextureSeedDelta = 0x00010007u;
constexpr UINT TextureChildDelta = 0x0002000bu;
using TextureOutput = std::array<uint32_t, TextureOutputWords>;

TextureOutput TextureInitialOutput() {
  TextureOutput words{};
  for (UINT i = 0; i < TextureGuardWords; ++i) {
    words[i] = 0x12345678u ^ (i * 0x9e3779b9u);
    words[TextureGuardWords + TexturePayloadWords + i] = 0xfedcba98u ^ (i * 0x85ebca6bu);
  }
  return words;
}
}

int main() { try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  Require(backend && backend() && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned backend");
  std::printf("OWNED_D3D12_WG_TEXTURE_FRONTIER_BACKEND id=%s\n", backend());
  ComPtr<ID3D12Device5> device;
  Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");

  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable = {1, &range};
  D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &parameter};
  ComPtr<ID3DBlob> blob;
  Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "root blob");
  ComPtr<ID3D12RootSignature> root;
  Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
  auto shader = Shader("workgraph-texture-frontier.dxil");
  D3D12_DXIL_LIBRARY_DESC library{{shader.data(), shader.size()}, 0, nullptr};
  D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};
  D3D12_WORK_GRAPH_DESC graph{};
  graph.ProgramName = L"texture_frontier";
  graph.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
  std::array<D3D12_STATE_SUBOBJECT, 3> objects{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH, &graph}}};
  D3D12_STATE_OBJECT_DESC stateDesc{D3D12_STATE_OBJECT_TYPE_EXECUTABLE, UINT(objects.size()), objects.data()};
  ComPtr<ID3D12StateObject> state;
  Check(device->CreateStateObject(&stateDesc, IID_PPV_ARGS(&state)), "texture frontier executable");
  ComPtr<ID3D12WorkGraphProperties> properties;
  Check(state.As(&properties), "graph properties");
  const UINT graphIndex = properties->GetWorkGraphIndex(L"texture_frontier");
  Require(graphIndex != UINT_MAX && properties->GetNumNodes(graphIndex) == 2 &&
      properties->GetNumEntrypoints(graphIndex) == 1, "two-node texture frontier topology");
  const UINT entry = properties->GetEntrypointIndex(graphIndex, {L"texture_seed", 0});
  Require(entry != UINT_MAX && properties->GetEntrypointRecordSizeInBytes(graphIndex, entry) == sizeof(UINT), "seed ABI");
  D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{};
  properties->GetWorkGraphMemoryRequirements(graphIndex, &memory);
  Require(memory.MinSizeInBytes && memory.MaxSizeInBytes >= memory.MinSizeInBytes, "backing requirements");
  auto backing = Buffer(device.Get(), memory.MinSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

  D3D12_RESOURCE_DESC textureDesc{};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  textureDesc.Width = TextureRecords; textureDesc.Height = 1;
  textureDesc.DepthOrArraySize = 1; textureDesc.MipLevels = 1;
  textureDesc.Format = DXGI_FORMAT_R32_UINT; textureDesc.SampleDesc.Count = 1;
  textureDesc.Layout = D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
  textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES defaultHeap{}; defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> texture;
  Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "STANDARD_SWIZZLE R32_UINT texture");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0; UINT64 rowBytes = 0, total = 0;
  device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
  Require(rows == 1 && rowBytes == TextureRecords * sizeof(UINT) && total >= rowBytes, "texture footprint");
  auto textureUpload = Buffer(device.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto textureReadback = Buffer(device.Get(), total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<uint8_t> seedBytes(total);
  for (UINT i = 0; i < TextureRecords; ++i) {
    const UINT initial = TextureBase + i * TextureStep;
    std::memcpy(seedBytes.data() + footprint.Offset + i * sizeof(UINT), &initial, sizeof(initial));
  }
  Upload(textureUpload.Get(), seedBytes.data(), seedBytes.size());
  TextureOutput initialOutput = TextureInitialOutput(), expectedOutput = initialOutput;
  for (UINT i = 0; i < TextureRecords; ++i) {
    const UINT initial = TextureBase + i * TextureStep;
    const size_t offset = TextureGuardWords + i * 4;
    expectedOutput[offset] = initial;
    expectedOutput[offset + 1] = initial + TextureSeedDelta;
    expectedOutput[offset + 2] = initial + TextureSeedDelta + TextureChildDelta;
    expectedOutput[offset + 3] = 1;
  }
  expectedOutput[TextureGuardWords + TextureRecords * 4] = TextureRecords;
  expectedOutput[TextureGuardWords + TextureRecords * 4 + 1] = TextureRecords;
  auto output = Buffer(device.Get(), sizeof(initialOutput), D3D12_HEAP_TYPE_DEFAULT,
      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto outputUpload = Buffer(device.Get(), sizeof(initialOutput), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto outputReadback = Buffer(device.Get(), sizeof(initialOutput), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  Upload(outputUpload.Get(), initialOutput.data(), sizeof(initialOutput));

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 2; heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "texture and raw UAV table");
  D3D12_CPU_DESCRIPTOR_HANDLE cpu{}; D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
  heap->GetCPUDescriptorHandleForHeapStart(&cpu); heap->GetGPUDescriptorHandleForHeapStart(&gpu);
  D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_UINT; view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, cpu);
  cpu.ptr += device->GetDescriptorHandleIncrementSize(heapDesc.Type);
  view = {}; view.Format = DXGI_FORMAT_R32_TYPELESS; view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = UINT(TextureOutputWords); view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(output.Get(), nullptr, &view, cpu);

  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  ComPtr<ID3D12CommandQueue> queue;
  Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
  ComPtr<ID3D12CommandAllocator> allocator;
  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  ComPtr<ID3D12GraphicsCommandList10> list;
  Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list10");
  D3D12_TEXTURE_COPY_LOCATION textureLocation{}, uploadLocation{}, readbackLocation{};
  textureLocation.pResource = texture.Get(); textureLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  uploadLocation.pResource = textureUpload.Get(); uploadLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  uploadLocation.PlacedFootprint = footprint;
  list->CopyTextureRegion(&textureLocation, 0, 0, 0, &uploadLocation, nullptr);
  list->CopyBufferRegion(output.Get(), 0, outputUpload.Get(), 0, sizeof(initialOutput));
  std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
  for (auto &barrier : barriers) barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition = {texture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  barriers[1].Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
      D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  list->ResourceBarrier(UINT(barriers.size()), barriers.data());
  ID3D12DescriptorHeap *heaps[]{heap.Get()};
  list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  ComPtr<ID3D12StateObjectProperties1> identifiers;
  Check(state.As(&identifiers), "program identifier");
  D3D12_SET_PROGRAM_DESC program{}; program.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
  identifiers->GetProgramIdentifier(&program.WorkGraph.ProgramIdentifier, L"texture_frontier");
  program.WorkGraph.Flags = D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
  program.WorkGraph.BackingMemory = {backing->GetGPUVirtualAddress(), memory.MinSizeInBytes};
  list->SetProgram(&program);
  std::array<UINT, TextureRecords> inputs{};
  for (UINT i = 0; i < TextureRecords; ++i) inputs[i] = i;
  D3D12_DISPATCH_GRAPH_DESC dispatch{}; dispatch.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
  dispatch.NodeCPUInput = {entry, TextureRecords, inputs.data(), sizeof(UINT)};
  list->DispatchGraph(&dispatch);
  D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav.UAV.pResource = texture.Get(); list->ResourceBarrier(1, &uav);
  for (auto &barrier : barriers) {
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }
  list->ResourceBarrier(UINT(barriers.size()), barriers.data());
  readbackLocation.pResource = textureReadback.Get();
  readbackLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  readbackLocation.PlacedFootprint = footprint;
  list->CopyTextureRegion(&readbackLocation, 0, 0, 0, &textureLocation, nullptr);
  list->CopyBufferRegion(outputReadback.Get(), 0, output.Get(), 0, sizeof(initialOutput));
  Check(list->Close(), "close");
  ComPtr<ID3D12Fence> fence;
  Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "event");
  ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
  Check(queue->Signal(fence.Get(), 1), "signal");
  Check(fence->SetEventOnCompletion(1, event), "completion event");
  Require(WaitForSingleObject(event, 180000) == WAIT_OBJECT_0, "GPU completion");
  Check(device->GetDeviceRemovedReason(), "device alive"); CloseHandle(event);

  void *pixels = nullptr, *results = nullptr;
  D3D12_RANGE pixelRange{0, size_t(total)}, resultRange{0, sizeof(initialOutput)};
  Check(textureReadback->Map(0, &pixelRange, &pixels), "map physical texture copy");
  Check(outputReadback->Map(0, &resultRange, &results), "map complete raw output");
  TextureOutput actualOutput{};
  std::memcpy(actualOutput.data(), results, sizeof(actualOutput));
  outputReadback->Unmap(0, nullptr);
  for (size_t word = 0; word < TextureOutputWords; ++word) {
    if (actualOutput[word] != expectedOutput[word])
      std::printf("OWNED_D3D12_WG_TEXTURE_FRONTIER_MISMATCH word=%zu actual=%08x expected=%08x\n",
          word, actualOutput[word], expectedOutput[word]);
    Require(actualOutput[word] == expectedOutput[word], "all raw output words, counts and guards");
  }
  for (UINT i = 0; i < TextureRecords; ++i) {
    UINT copied = 0;
    std::memcpy(&copied, static_cast<const uint8_t *>(pixels) + footprint.Offset + i * sizeof(UINT), sizeof(copied));
    const UINT expected = TextureBase + i * TextureStep + TextureSeedDelta + TextureChildDelta;
    const size_t offset = TextureGuardWords + i * 4;
    std::printf("OWNED_D3D12_WG_TEXTURE_FRONTIER_TEXEL index=%u initial=%08x child_observed=%08x final=%08x copy=%08x visits=%u\n",
        i, actualOutput[offset], actualOutput[offset + 1], actualOutput[offset + 2], copied, actualOutput[offset + 3]);
    Require(copied == expected, "published physical texture texel");
  }
  textureReadback->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_WG_TEXTURE_FRONTIER_PASS cases=1 nodes=2 cpu_records=%u seed_count=%u child_count=%u raw_words=%zu guards=8 physical_copy=PASS layout=STANDARD_SWIZZLE atomic64=0\n",
      TextureRecords, actualOutput[TextureGuardWords + TextureRecords * 4],
      actualOutput[TextureGuardWords + TextureRecords * 4 + 1], TextureOutputWords);
  return 0;
} catch (...) {
  std::puts("OWNED_D3D12_WG_TEXTURE_FRONTIER_FAIL");
  return 1;
} }
