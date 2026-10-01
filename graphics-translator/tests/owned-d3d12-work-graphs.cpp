#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "../../src/d3d12/d3d12_interfaces_ext.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {
void Check(HRESULT hr, const char *step) {
  if (FAILED(hr)) { std::fprintf(stderr, "OWNED_D3D12_WORK_GRAPHS_FAIL step=%s hr=%08lx\n", step, (unsigned long)hr); throw std::runtime_error(step); }
}
void Require(bool condition, const char *step) { Check(condition ? S_OK : E_FAIL, step); }
std::vector<uint8_t> Shader(const char *name) {
  FILE *file = std::fopen(name, "rb"); Require(file != nullptr, "open shader");
  std::fseek(file, 0, SEEK_END); long size = std::ftell(file); std::rewind(file);
  std::vector<uint8_t> bytes(size); Require(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size(), "read shader");
  std::fclose(file); return bytes;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device *device, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                             D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
  desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
  ComPtr<ID3D12Resource> resource; Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
      nullptr, IID_PPV_ARGS(&resource)), "CreateCommittedResource"); return resource;
}
void Upload(ID3D12Resource *buffer, const void *bytes, size_t size, size_t offset = 0) {
  void *mapping = nullptr; D3D12_RANGE empty{0, 0}; Check(buffer->Map(0, &empty, &mapping), "Map upload");
  std::memcpy(static_cast<uint8_t *>(mapping) + offset, bytes, size); buffer->Unmap(0, nullptr);
}
}

int main() {
  try {
    ComPtr<ID3D12Device5> device; Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    struct CapabilityBuffer { D3D12_FEATURE_DATA_D3D12_OPTIONS21 options; UINT guard; } capabilities{};
    capabilities.guard = 0xa11c0ded;
    const auto options_feature = static_cast<D3D12_FEATURE>(dxmt::kFeatureD3D12Options21);
    Require(device->CheckFeatureSupport(options_feature, &capabilities.options, sizeof(capabilities.options) - 1) == E_INVALIDARG,
            "OPTIONS21 truncated structure rejected");
    Require(device->CheckFeatureSupport(options_feature, &capabilities.options, sizeof(capabilities)) == E_INVALIDARG,
            "OPTIONS21 oversized structure rejected");
    Check(device->CheckFeatureSupport(options_feature, &capabilities.options, sizeof(capabilities.options)), "OPTIONS21");
    Require(capabilities.options.WorkGraphsTier == D3D12_WORK_GRAPHS_TIER_1_0 && capabilities.guard == 0xa11c0ded,
            "Work Graph Tier1.0 exact typed capability");
    std::printf("OWNED_D3D12_WORK_GRAPHS_CAPS tier=%u options21_bytes=%u\n",
                UINT(capabilities.options.WorkGraphsTier), UINT(sizeof(capabilities.options)));
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter1> adapter; Check(factory->EnumAdapters1(0, &adapter), "adapter"); DXGI_ADAPTER_DESC1 adapter_desc{};
    Check(adapter->GetDesc1(&adapter_desc), "adapter description");
    std::printf("OWNED_D3D12_WORK_GRAPHS_BACKEND id=owned-graphics-d3d12\n");
    std::printf("OWNED_D3D12_WORK_GRAPHS_ADAPTER name=%ls vendor=%04x device=%04x\n", adapter_desc.Description, adapter_desc.VendorId, adapter_desc.DeviceId);
    D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 0; parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob; Check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr), "serialize root");
    ComPtr<ID3D12RootSignature> root; Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
    auto shader = Shader("work-graphs-nodes.dxil"); D3D12_DXIL_LIBRARY_DESC library{{shader.data(), shader.size()}, 0, nullptr};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};
    D3D12_NODE_MASK node_mask{1};
    std::array<D3D12_STATE_SUBOBJECT, 3> collection_subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}, {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK, &node_mask}}};
    D3D12_STATE_OBJECT_DESC collection_desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, UINT(collection_subobjects.size()), collection_subobjects.data()};
    ComPtr<ID3D12StateObject> collection; Check(device->CreateStateObject(&collection_desc, IID_PPV_ARGS(&collection)), "node shader collection");
    ComPtr<ID3D12WorkGraphProperties> collection_properties; Check(collection.As(&collection_properties), "collection properties");
    Require(collection_properties->GetNumWorkGraphs() == 0 && collection_properties->GetNumNodes(0) == UINT_MAX &&
            collection_properties->GetNumEntrypoints(0) == UINT_MAX &&
            collection_properties->GetEntrypointRecordSizeInBytes(0, 0) == UINT_MAX &&
            collection_properties->GetEntrypointRecordAlignmentInBytes(0, 0) == UINT_MAX, "invalid graph property sentinel");
    D3D12_EXISTING_COLLECTION_DESC existing{collection.Get(), 0, nullptr};
    D3D12_NODE orphan_node{}; orphan_node.NodeType = dxmt::kNodeTypeShader; orphan_node.Shader.Shader = L"terminal";
    D3D12_WORK_GRAPH_DESC orphan_graph{}; orphan_graph.ProgramName = L"orphan";
    orphan_graph.NumExplicitlyDefinedNodes = 1; orphan_graph.pExplicitlyDefinedNodes = &orphan_node;
    std::array<D3D12_STATE_SUBOBJECT, 2> orphan_subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
      {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &orphan_graph}}};
    D3D12_STATE_OBJECT_DESC orphan_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), 2, orphan_subobjects.data()};
    ComPtr<ID3D12StateObject> invalid_state;
    Require(device->CreateStateObject(&orphan_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "orphan explicit node rejected");
    D3D12_WORK_GRAPH_DESC graph{}; graph.ProgramName = L"bounded"; graph.Flags = dxmt::kWorkGraphFlagIncludeAllAvailableNodes;
    D3D12_WORK_GRAPH_DESC empty_graph{}; empty_graph.ProgramName = L"empty";
    D3D12_STATE_OBJECT_CONFIG config{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
    std::array<D3D12_STATE_SUBOBJECT, 5> subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing}, {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
      {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &graph},
      {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &empty_graph},
      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config}}};
    D3D12_STATE_OBJECT_DESC state_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), UINT(subobjects.size()), subobjects.data()};
    ComPtr<ID3D12StateObject> state; Check(device->CreateStateObject(&state_desc, IID_PPV_ARGS(&state)), "CreateStateObject");
    ComPtr<ID3D12WorkGraphProperties> initial_properties; Check(state.As(&initial_properties), "initial graph properties");
    ComPtr<ID3D12StateObjectProperties1> initial_state_properties; Check(state.As(&initial_state_properties), "initial state properties");
    UINT empty_index = initial_properties->GetWorkGraphIndex(L"empty");
    D3D12_PROGRAM_IDENTIFIER empty_identifier{};
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    initial_state_properties->GetProgramIdentifier(&empty_identifier, L"empty");
#else
    empty_identifier = initial_state_properties->GetProgramIdentifier(L"empty");
#endif
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS empty_memory{}; initial_properties->GetWorkGraphMemoryRequirements(empty_index, &empty_memory);
    Require(initial_properties->GetNumWorkGraphs() == 2 && initial_properties->GetNumNodes(empty_index) == 0 &&
            initial_properties->GetNumEntrypoints(empty_index) == 0 && empty_memory.MinSizeInBytes == 0 &&
            empty_memory.MaxSizeInBytes == 0 && empty_memory.SizeGranularityInBytes == 0 &&
            empty_identifier.OpaqueData[0] == 0 && empty_identifier.OpaqueData[1] == 0 &&
            empty_identifier.OpaqueData[2] == 0 && empty_identifier.OpaqueData[3] == 0, "empty graph remains empty with available entrypoint shaders");
    D3D12_NODE_ID added_id{L"added", 0}; BOOL added_entry = TRUE;
    D3D12_COMMON_COMPUTE_NODE_OVERRIDES overrides{}; overrides.pNewName = &added_id; overrides.pProgramEntry = &added_entry;
    D3D12_NODE added_node{}; added_node.NodeType = dxmt::kNodeTypeShader; added_node.Shader.Shader = L"terminal";
    added_node.Shader.OverridesType = dxmt::kNodeOverridesCommonCompute; added_node.Shader.pCommonComputeNodeOverrides = &overrides;
    D3D12_WORK_GRAPH_DESC extension{}; extension.ProgramName = L"bounded"; extension.NumExplicitlyDefinedNodes = 1;
    extension.pExplicitlyDefinedNodes = &added_node;
    D3D12_STATE_SUBOBJECT extension_subobject{static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &extension};
    D3D12_STATE_OBJECT_DESC extension_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), 1, &extension_subobject};
    ComPtr<ID3D12Device7> additions_device; Check(device.As(&additions_device), "Device7");
    Require(additions_device->AddToStateObject(&extension_desc, state.Get(), IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "addition must opt in");
    std::array<D3D12_STATE_SUBOBJECT, 2> addition_subobjects{{extension_subobject, {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config}}};
    extension_desc.NumSubobjects = addition_subobjects.size(); extension_desc.pSubobjects = addition_subobjects.data();
    ComPtr<ID3D12StateObject> extended; Check(additions_device->AddToStateObject(&extension_desc, state.Get(), IID_PPV_ARGS(&extended)), "AddToStateObject");
    Require(additions_device->AddToStateObject(&extension_desc, state.Get(), IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "additions use latest version only");
    state = extended;
    ComPtr<ID3D12WorkGraphProperties> properties; Check(state.As(&properties), "WorkGraphProperties");
    ComPtr<ID3D12StateObjectProperties1> identifiers; Check(state.As(&identifiers), "StateObjectProperties1");
    UINT index = properties->GetWorkGraphIndex(L"bounded"); Require(index == 0, "graph index");
    Require(properties->GetNumNodes(index) == 4 && properties->GetNumEntrypoints(index) == 2, "extended node and entry count");
    UINT entry = properties->GetEntrypointIndex(index, {L"entry", 0}); Require(entry == 0, "entry index");
    UINT added = properties->GetEntrypointIndex(index, added_id); Require(added == 1, "appended entry index");
    Require(properties->GetEntrypointRecordSizeInBytes(index, entry) == 4, "entry record size");
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{}; properties->GetWorkGraphMemoryRequirements(index, &memory);
    Require(memory.MinSizeInBytes && memory.MaxSizeInBytes >= memory.MinSizeInBytes, "memory requirements");
    std::printf("OWNED_D3D12_WORK_GRAPHS_MEMORY exact_min=%llu max=%llu depth=32 fanout=16\n", memory.MinSizeInBytes, memory.MaxSizeInBytes);
    auto backing = Buffer(device.Get(), memory.MinSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto result = Buffer(device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto zero = Buffer(device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const uint64_t zero_value = 0; Upload(zero.Get(), &zero_value, sizeof(zero_value));
    auto readback = Buffer(device.Get(), 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto seeds = Buffer(device.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    std::array<uint32_t, 4> input{{100, 200, 300, 400}}; Upload(seeds.Get(), input.data(), sizeof(input));
    const uint32_t added_value = 42; Upload(seeds.Get(), &added_value, 4, 176);
    D3D12_NODE_GPU_INPUT gpu_input{entry, UINT(input.size()), {seeds->GetGPUVirtualAddress(), 0}};
    Upload(seeds.Get(), &gpu_input, sizeof(gpu_input), 32);
    std::array<D3D12_NODE_GPU_INPUT, 3> gpu_multi{{{entry, 2, {seeds->GetGPUVirtualAddress(), 4}},
        {entry, 2, {seeds->GetGPUVirtualAddress() + 8, 4}}, {added, 1, {seeds->GetGPUVirtualAddress() + 176, 4}}}};
    Upload(seeds.Get(), gpu_multi.data(), sizeof(gpu_multi), 64);
    D3D12_MULTI_NODE_GPU_INPUT multi_gpu{3, {seeds->GetGPUVirtualAddress() + 64, sizeof(D3D12_NODE_GPU_INPUT)}};
    Upload(seeds.Get(), &multi_gpu, sizeof(multi_gpu), 144);
    D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "event");
    uint32_t expected_sum = 0, expected_count = 0;
    for (uint32_t seed : input) for (uint32_t level = 0; level < 30; ++level) for (uint32_t record = 0; record < 16; ++record) {
      expected_sum += (seed + level * 16 + record) * 3; ++expected_count;
    }
    expected_sum += 42; ++expected_count;
    auto sharing_shader = Shader("work-graphs-sharing.dxil");
    D3D12_DXIL_LIBRARY_DESC sharing_library{{sharing_shader.data(), sharing_shader.size()}, 0, nullptr};
    D3D12_WORK_GRAPH_DESC sharing_graph{}; sharing_graph.ProgramName = L"sharing"; sharing_graph.Flags = dxmt::kWorkGraphFlagIncludeAllAvailableNodes;
    std::array<D3D12_STATE_SUBOBJECT, 2> sharing_subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &sharing_library},
      {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &sharing_graph}}};
    D3D12_STATE_OBJECT_DESC sharing_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), 2, sharing_subobjects.data()};
    ComPtr<ID3D12StateObject> sharing_state; Check(device->CreateStateObject(&sharing_desc, IID_PPV_ARGS(&sharing_state)), "sharing state");
    ComPtr<ID3D12WorkGraphProperties> sharing_properties; Check(sharing_state.As(&sharing_properties), "sharing properties");
    ComPtr<ID3D12StateObjectProperties1> sharing_identifiers; Check(sharing_state.As(&sharing_identifiers), "sharing identifiers");
    D3D12_NODE_ID array_peer{L"producer", 1};
    D3D12_NODE_OUTPUT_OVERRIDES producer_output{}; producer_output.OutputIndex = 2; producer_output.pNewName = &array_peer;
    D3D12_COMMON_COMPUTE_NODE_OVERRIDES counter_override{}; counter_override.pNewName = &array_peer;
    D3D12_COMMON_COMPUTE_NODE_OVERRIDES producer_override{}; producer_override.NumOutputOverrides = 1; producer_override.pOutputOverrides = &producer_output;
    BOOL cross_array_entry = TRUE; producer_override.pProgramEntry = &cross_array_entry;
    std::array<D3D12_NODE, 2> cross_array_nodes{};
    cross_array_nodes[0].NodeType = cross_array_nodes[1].NodeType = dxmt::kNodeTypeShader;
    cross_array_nodes[0].Shader.Shader = L"counter"; cross_array_nodes[0].Shader.OverridesType = dxmt::kNodeOverridesCommonCompute;
    cross_array_nodes[0].Shader.pCommonComputeNodeOverrides = &counter_override;
    cross_array_nodes[1].Shader.Shader = L"producer"; cross_array_nodes[1].Shader.OverridesType = dxmt::kNodeOverridesCommonCompute;
    cross_array_nodes[1].Shader.pCommonComputeNodeOverrides = &producer_override;
    D3D12_WORK_GRAPH_DESC cross_array_graph{}; cross_array_graph.ProgramName = L"cross-array";
    cross_array_graph.NumExplicitlyDefinedNodes = 2; cross_array_graph.pExplicitlyDefinedNodes = cross_array_nodes.data();
    std::array<D3D12_STATE_SUBOBJECT, 2> cross_array_subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &sharing_library},
      {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &cross_array_graph}}};
    D3D12_STATE_OBJECT_DESC cross_array_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), 2, cross_array_subobjects.data()};
    Require(device->CreateStateObject(&cross_array_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "different array entries cannot connect");
    Require(sharing_properties->GetNumNodes(0) == 6 && sharing_properties->GetNumEntrypoints(0) == 2, "sharing node topology");
    UINT sharing_entry = sharing_properties->GetEntrypointIndex(0, {L"producer", 0});
    UINT empty_entry = sharing_properties->GetEntrypointIndex(0, {L"empty", 0});
    Require(empty_entry != UINT_MAX && sharing_properties->GetEntrypointRecordSizeInBytes(0, empty_entry) == 0 &&
            sharing_properties->GetEntrypointRecordAlignmentInBytes(0, empty_entry) == 0, "empty entrypoint has no record storage or alignment");
    Require(sharing_properties->GetEntrypointRecordSizeInBytes(0, sharing_entry) == 16 &&
            sharing_properties->GetEntrypointRecordAlignmentInBytes(0, sharing_entry) == 4, "dynamic input record ABI");
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS sharing_memory{}; sharing_properties->GetWorkGraphMemoryRequirements(0, &sharing_memory);
    auto sharing_backing = Buffer(device.Get(), sharing_memory.MinSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const std::array<uint64_t, 6> local_values{{3, 5, 7, 11, 13, 17}}; Upload(seeds.Get(), local_values.data(), sizeof(local_values), 192);
    const auto sink0_local = sharing_properties->GetNodeLocalRootArgumentsTableIndex(0, sharing_properties->GetNodeIndex(0, {L"sink", 0}));
    const auto sink2_local = sharing_properties->GetNodeLocalRootArgumentsTableIndex(0, sharing_properties->GetNodeIndex(0, {L"sink", 2}));
    const auto sharing_local = sharing_properties->GetNodeLocalRootArgumentsTableIndex(0, sharing_properties->GetNodeIndex(0, {L"sharing", 0}));
    const auto counter_local = sharing_properties->GetNodeLocalRootArgumentsTableIndex(0, sharing_properties->GetNodeIndex(0, {L"counter", 0}));
    Require(sink2_local == 0 && sink0_local < local_values.size() && sharing_local < local_values.size() && counter_local < local_values.size(), "explicit local table index");
    std::array<bool, 6> assigned{};
    for (UINT node = 0; node < local_values.size(); ++node) {
      UINT slot = sharing_properties->GetNodeLocalRootArgumentsTableIndex(0, node);
      Require(slot < local_values.size() && !assigned[slot], "automatic local indices skip explicit slot"); assigned[slot] = true;
    }
    struct SharingSeed { uint32_t base, grid[3]; }; const SharingSeed sharing_seed{100, {3, 1, 1}};
    uint32_t sharing_sum = 0, sharing_count = 0;
    for (UINT record = 0; record < input.size(); ++record) {
      uint32_t seed = sharing_seed.base;
      for (UINT group = 0; group < 3; ++group) for (UINT lane = 0; lane < 4; ++lane) {
        uint32_t value = seed + group * 100 + lane;
        if (lane >= 2 || group != 1) { sharing_sum += value * local_values[lane >= 2 || group == 0 ? sink0_local : sink2_local]; ++sharing_count; }
        if (lane >= 2 || group == 0) { sharing_sum += value * 2 * local_values[sharing_local]; ++sharing_count; }
      }
      sharing_sum += (seed + 3) * local_values[counter_local]; ++sharing_count;
    }
    sharing_count += 24; // Sixteen GPU-generated empty records, plus four CPU and four GPU seeds.
    D3D12_NODE_GPU_INPUT empty_gpu{empty_entry, 4, 0, 0};
    for (UINT mode = 0; mode < 5; ++mode) {
      if (mode == 4) Upload(seeds.Get(), &empty_gpu, sizeof(empty_gpu), 96);
      ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
      ComPtr<ID3D12GraphicsCommandList10> list; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CommandList10");
      D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barrier.Transition.pResource = result.Get();
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      barrier.Transition.StateBefore = mode ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; list->ResourceBarrier(1, &barrier);
      list->CopyBufferRegion(result.Get(), 0, zero.Get(), 0, 8);
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; list->ResourceBarrier(1, &barrier);
      list->SetComputeRootSignature(root.Get()); list->SetComputeRootUnorderedAccessView(0, result->GetGPUVirtualAddress());
      D3D12_SET_PROGRAM_DESC program{}; program.Type = dxmt::kProgramTypeWorkGraph;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
      (mode == 4 ? sharing_identifiers.Get() : identifiers.Get())->GetProgramIdentifier(&program.WorkGraph.ProgramIdentifier, mode == 4 ? L"sharing" : L"bounded");
#else
      program.WorkGraph.ProgramIdentifier = (mode == 4 ? sharing_identifiers.Get() : identifiers.Get())->GetProgramIdentifier(mode == 4 ? L"sharing" : L"bounded");
#endif
      program.WorkGraph.Flags = mode == 0 || mode == 4 ? dxmt::kSetWorkGraphFlagInitialize : 0;
      program.WorkGraph.BackingMemory = mode == 4 ? D3D12_GPU_VIRTUAL_ADDRESS_RANGE{sharing_backing->GetGPUVirtualAddress(), sharing_memory.MinSizeInBytes} : D3D12_GPU_VIRTUAL_ADDRESS_RANGE{backing->GetGPUVirtualAddress(), memory.MinSizeInBytes};
      if (mode == 4) program.WorkGraph.NodeLocalRootArgumentsTable = {seeds->GetGPUVirtualAddress() + 192, sizeof(local_values), 8};
      list->SetProgram(&program);
      D3D12_DISPATCH_GRAPH_DESC dispatch{}; dispatch.Mode = mode == 4 ? 0 : mode;
      std::array<D3D12_NODE_CPU_INPUT, 3> cpu_multi{{{entry, 2, input.data(), 4}, {entry, 2, input.data() + 2, 4}, {added, 1, &added_value, 4}}};
      if (mode == 0) dispatch.NodeCPUInput = {entry, UINT(input.size()), input.data(), 4};
      if (mode == 1) dispatch.NodeGPUInput = seeds->GetGPUVirtualAddress() + 32;
      if (mode == 2) dispatch.MultiNodeCPUInput = {3, cpu_multi.data(), sizeof(D3D12_NODE_CPU_INPUT)};
      if (mode == 3) dispatch.MultiNodeGPUInput = seeds->GetGPUVirtualAddress() + 144;
      if (mode == 4) dispatch.NodeCPUInput = {sharing_entry, UINT(input.size()), &sharing_seed, 0};
      list->DispatchGraph(&dispatch);
      D3D12_DISPATCH_GRAPH_DESC added_dispatch{};
      added_dispatch.Mode = 0; added_dispatch.NodeCPUInput = {added, 1, &added_value, 4};
      if (mode < 2) list->DispatchGraph(&added_dispatch);
      if (mode == 4) {
        D3D12_DISPATCH_GRAPH_DESC empty_dispatch{}; empty_dispatch.Mode = 0;
        empty_dispatch.NodeCPUInput = {empty_entry, 4, nullptr, 0}; list->DispatchGraph(&empty_dispatch);
        empty_dispatch.Mode = 1; empty_dispatch.NodeGPUInput = seeds->GetGPUVirtualAddress() + 96; list->DispatchGraph(&empty_dispatch);
      }
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; list->ResourceBarrier(1, &barrier);
      list->CopyBufferRegion(readback.Get(), 0, result.Get(), 0, 8); Check(list->Close(), "Close");
      ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
      Check(queue->Signal(fence.Get(), mode + 1), "Signal"); Check(fence->SetEventOnCompletion(mode + 1, event), "SetEventOnCompletion");
      Require(WaitForSingleObject(event, 180000) == WAIT_OBJECT_0, "GPU completion"); Check(device->GetDeviceRemovedReason(), "device alive");
      void *mapping = nullptr; D3D12_RANGE range{0, 8}; Check(readback->Map(0, &range, &mapping), "Map result");
      std::array<uint32_t, 2> actual{}; std::memcpy(actual.data(), mapping, 8); readback->Unmap(0, nullptr);
      uint32_t oracle_sum = mode == 4 ? sharing_sum : expected_sum, oracle_count = mode == 4 ? sharing_count : expected_count;
      if (mode == 1) for (auto seed : input) oracle_sum -= (seed - input.front()) * 30 * 16 * 3;
      std::printf("OWNED_D3D12_WORK_GRAPHS_RESULT mode=%u sum=%u expected_sum=%u count=%u expected_count=%u\n", mode, actual[0], oracle_sum, actual[1], oracle_count);
      Require(actual[0] == oracle_sum && actual[1] == oracle_count, "exact CPU oracle");
      if (mode == 4) std::printf("OWNED_D3D12_WORK_GRAPHS_CASE_PASS shared_budget_sparse_array_input_sharing_local_roots_cross_group\n");
      else std::printf("OWNED_D3D12_WORK_GRAPHS_CASE_PASS mode_%u_min_backing_depth32_fanout16\n", mode);
      std::fflush(stdout);
    }
    // One state object carries two incompatible root layouts and a public function namespace.
    std::array<std::array<D3D12_ROOT_PARAMETER, 3>, 3> link_layouts{};
    link_layouts[0][0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; link_layouts[0][0].Descriptor.ShaderRegister = 0;
    link_layouts[0][1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; link_layouts[0][1].Descriptor.ShaderRegister = 3;
    link_layouts[1][0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    link_layouts[1][0].Constants = {0, 0, 1};
    link_layouts[1][1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; link_layouts[1][1].Descriptor.ShaderRegister = 1;
    link_layouts[1][2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; link_layouts[1][2].Descriptor.ShaderRegister = 3;
    link_layouts[2][0] = link_layouts[0][1]; link_layouts[2][1] = link_layouts[0][0];
    std::array<ComPtr<ID3D12RootSignature>, 3> link_roots;
    for (UINT layout = 0; layout < link_roots.size(); ++layout) {
      D3D12_ROOT_SIGNATURE_DESC desc{layout == 1 ? 3u : 2u, link_layouts[layout].data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      ComPtr<ID3DBlob> serialized; Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr), "link root serialization");
      Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&link_roots[layout])), "link root");
    }
    auto link_node_bytes = Shader("work-graphs-link-nodes.dxil"), link_helper_bytes = Shader("work-graphs-link-helpers.dxil");
    D3D12_DXIL_LIBRARY_DESC link_node_library{{link_node_bytes.data(), link_node_bytes.size()}, 0, nullptr};
    std::array<D3D12_EXPORT_DESC, 3> helper_exports{{
      {L"PublicValue", L"RawValue", D3D12_EXPORT_FLAG_NONE}, {L"WriteValue", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"PublicRecord", L"RawRecord", D3D12_EXPORT_FLAG_NONE}}};
    D3D12_DXIL_LIBRARY_DESC link_helper_library{{link_helper_bytes.data(), link_helper_bytes.size()}, UINT(helper_exports.size()), helper_exports.data()};
    D3D12_STATE_OBJECT_CONFIG node_config{static_cast<D3D12_STATE_OBJECT_FLAGS>(1)};
    D3D12_STATE_OBJECT_CONFIG helper_config{static_cast<D3D12_STATE_OBJECT_FLAGS>(3)};
    std::array<D3D12_STATE_SUBOBJECT, 2> node_collection_subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &link_node_library}, {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &node_config}}};
    std::array<D3D12_STATE_SUBOBJECT, 2> helper_collection_subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &link_helper_library}, {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &helper_config}}};
    D3D12_STATE_OBJECT_DESC node_collection_desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, UINT(node_collection_subobjects.size()), node_collection_subobjects.data()};
    D3D12_STATE_OBJECT_DESC helper_collection_desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, UINT(helper_collection_subobjects.size()), helper_collection_subobjects.data()};
    ComPtr<ID3D12StateObject> link_nodes, link_helpers, weak_helpers;
    node_config.Flags = D3D12_STATE_OBJECT_FLAG_NONE;
    Require(device->CreateStateObject(&node_collection_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "node collection requires external-definition permission");
    node_config.Flags = static_cast<D3D12_STATE_OBJECT_FLAGS>(1);
    Check(device->CreateStateObject(&node_collection_desc, IID_PPV_ARGS(&link_nodes)), "unbound node collection");
    helper_exports[0].ExportToRename = L"AbsentValue";
    Require(device->CreateStateObject(&helper_collection_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "missing public export rejected");
    helper_exports[0].ExportToRename = L"RawValue";
    helper_config.Flags = static_cast<D3D12_STATE_OBJECT_FLAGS>(2);
    Require(device->CreateStateObject(&helper_collection_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "complete collection resolves resource bindings");
    helper_config.Flags = static_cast<D3D12_STATE_OBJECT_FLAGS>(1);
    Check(device->CreateStateObject(&helper_collection_desc, IID_PPV_ARGS(&weak_helpers)), "helper collection without external-call permission");
    helper_config.Flags = static_cast<D3D12_STATE_OBJECT_FLAGS>(3);
    Check(device->CreateStateObject(&helper_collection_desc, IID_PPV_ARGS(&link_helpers)), "aliased public helper collection");
    std::array<D3D12_EXISTING_COLLECTION_DESC, 3> link_existing{{
      {link_nodes.Get(), 0, nullptr}, {link_helpers.Get(), 0, nullptr}, {weak_helpers.Get(), 0, nullptr}}};
    std::array<D3D12_GLOBAL_ROOT_SIGNATURE, 3> link_global{{
      {link_roots[0].Get()}, {link_roots[1].Get()}, {link_roots[2].Get()}}};
    std::array<LPCWSTR, 2> link_exports{{L"LinkA", L"LinkB"}};
    std::array<D3D12_NODE, 2> link_nodes_desc{};
    std::array<D3D12_WORK_GRAPH_DESC, 2> link_graphs{};
    std::array<LPCWSTR, 2> link_programs{{L"link-A", L"link-B"}};
    for (UINT p = 0; p < link_graphs.size(); ++p) {
      link_nodes_desc[p].NodeType = dxmt::kNodeTypeShader; link_nodes_desc[p].Shader.Shader = link_exports[p];
      link_graphs[p].ProgramName = link_programs[p]; link_graphs[p].NumExplicitlyDefinedNodes = 1;
      link_graphs[p].pExplicitlyDefinedNodes = &link_nodes_desc[p];
    }
    std::array<D3D12_STATE_SUBOBJECT, 8> link_subobjects{};
    std::array<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION, 2> link_associations{{
      {&link_subobjects[2], 1, &link_exports[0]}, {&link_subobjects[3], 1, &link_exports[1]}}};
    link_subobjects = {{{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &link_existing[0]},
                       {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &link_existing[1]},
                       {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &link_global[0]},
                       {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &link_global[1]},
                       {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &link_associations[0]},
                       {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &link_associations[1]},
                       {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &link_graphs[0]},
                       {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &link_graphs[1]}}};
    D3D12_STATE_OBJECT_DESC link_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), UINT(link_subobjects.size()), link_subobjects.data()};
    D3D12_STATE_OBJECT_CONFIG strict_config{D3D12_STATE_OBJECT_FLAG_NONE};
    link_subobjects[1] = {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &strict_config};
    Require(device->CreateStateObject(&link_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "missing external helper definition rejected");
    link_subobjects[1] = {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &link_existing[2]};
    Require(device->CreateStateObject(&link_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "helper external-call permission enforced");
    link_subobjects[1].pDesc = &link_existing[1];
    ComPtr<ID3D12StateObject> link_state; Check(device->CreateStateObject(&link_desc, IID_PPV_ARGS(&link_state)), "per-program global roots and ordinary-function linkage");
    std::array<D3D12_STATE_SUBOBJECT, 6> bound_node_subobjects{{
      node_collection_subobjects[0], node_collection_subobjects[1], link_subobjects[2], link_subobjects[3], {}, {}}};
    std::array<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION, 2> bound_associations{{
      {&bound_node_subobjects[2], 1, &link_exports[0]}, {&bound_node_subobjects[3], 1, &link_exports[1]}}};
    bound_node_subobjects[4] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &bound_associations[0]};
    bound_node_subobjects[5] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &bound_associations[1]};
    D3D12_STATE_OBJECT_DESC bound_node_desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, UINT(bound_node_subobjects.size()), bound_node_subobjects.data()};
    ComPtr<ID3D12StateObject> bound_nodes; Check(device->CreateStateObject(&bound_node_desc, IID_PPV_ARGS(&bound_nodes)), "root-bound node collection");
    D3D12_EXISTING_COLLECTION_DESC bound_existing{bound_nodes.Get(), 0, nullptr};
    link_subobjects[0].pDesc = &bound_existing; link_subobjects[2].pDesc = &link_global[2];
    Require(device->CreateStateObject(&link_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "imported root association is immutable");
    link_subobjects[0].pDesc = &link_existing[0]; link_subobjects[2].pDesc = &link_global[0];
    std::array<D3D12_STATE_SUBOBJECT, 4> rooted_helper_subobjects{{helper_collection_subobjects[0], helper_collection_subobjects[1],
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &link_global[2]}, {}}};
    LPCWSTR rooted_helper_export = L"PublicValue";
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION rooted_helper_association{&rooted_helper_subobjects[2], 1, &rooted_helper_export};
    rooted_helper_subobjects[3] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &rooted_helper_association};
    D3D12_STATE_OBJECT_DESC rooted_helper_desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, UINT(rooted_helper_subobjects.size()), rooted_helper_subobjects.data()};
    ComPtr<ID3D12StateObject> rooted_helpers; Check(device->CreateStateObject(&rooted_helper_desc, IID_PPV_ARGS(&rooted_helpers)), "root-associated ordinary function collection");
    D3D12_EXISTING_COLLECTION_DESC rooted_helper_existing{rooted_helpers.Get(), 0, nullptr}; link_subobjects[1].pDesc = &rooted_helper_existing;
    Require(device->CreateStateObject(&link_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "reachable ordinary function has incompatible root association");
    link_subobjects[1].pDesc = &link_existing[1];
    std::printf("OWNED_D3D12_WORK_GRAPHS_CASE_PASS collection_external_namespace_root_contract_negatives\n");
    ComPtr<ID3D12WorkGraphProperties> link_properties; Check(link_state.As(&link_properties), "link graph properties");
    ComPtr<ID3D12StateObjectProperties1> link_identifiers; Check(link_state.As(&link_identifiers), "link program identifiers");
    const std::array<UINT, 4> link_input{{1, 2, 3, 4}};
    auto link_result = Buffer(device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto helper_result = Buffer(device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto link_readback = Buffer(device.Get(), 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT p = 0; p < link_programs.size(); ++p) {
      UINT graph_index = link_properties->GetWorkGraphIndex(link_programs[p]);
      Require(graph_index != UINT_MAX && link_properties->GetNumNodes(graph_index) == 1 && link_properties->GetNumEntrypoints(graph_index) == 1, "linked graph topology");
      D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS requirements{}; link_properties->GetWorkGraphMemoryRequirements(graph_index, &requirements);
      auto link_backing = Buffer(device.Get(), requirements.MinSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "link allocator");
      ComPtr<ID3D12GraphicsCommandList10> list; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "link CommandList10");
      std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
      for (UINT r = 0; r < barriers.size(); ++r) {
        barriers[r].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; barriers[r].Transition.pResource = r ? helper_result.Get() : link_result.Get();
        barriers[r].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[r].Transition.StateBefore = p ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[r].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      }
      list->ResourceBarrier(UINT(barriers.size()), barriers.data());
      list->CopyBufferRegion(link_result.Get(), 0, zero.Get(), 0, 8); list->CopyBufferRegion(helper_result.Get(), 0, zero.Get(), 0, 8);
      for (auto &b : barriers) { b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; }
      list->ResourceBarrier(UINT(barriers.size()), barriers.data());
      list->SetComputeRootSignature(link_roots[p].Get());
      if (p) list->SetComputeRoot32BitConstant(0, 13, 0);
      list->SetComputeRootUnorderedAccessView(p ? 1 : 0, link_result->GetGPUVirtualAddress());
      list->SetComputeRootUnorderedAccessView(p ? 2 : 1, helper_result->GetGPUVirtualAddress());
      D3D12_SET_PROGRAM_DESC linked_program{}; linked_program.Type = dxmt::kProgramTypeWorkGraph;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
      link_identifiers->GetProgramIdentifier(&linked_program.WorkGraph.ProgramIdentifier, link_programs[p]);
#else
      linked_program.WorkGraph.ProgramIdentifier = link_identifiers->GetProgramIdentifier(link_programs[p]);
#endif
      linked_program.WorkGraph.Flags = dxmt::kSetWorkGraphFlagInitialize;
      linked_program.WorkGraph.BackingMemory = {link_backing->GetGPUVirtualAddress(), requirements.MinSizeInBytes}; list->SetProgram(&linked_program);
      D3D12_DISPATCH_GRAPH_DESC linked_dispatch{}; linked_dispatch.Mode = 0;
      UINT entry_index = link_properties->GetEntrypointIndex(graph_index, {p ? L"linkB" : L"linkA", 0});
      Require(entry_index != UINT_MAX, "linked entrypoint");
      linked_dispatch.NodeCPUInput = {entry_index, UINT(link_input.size()), link_input.data(), sizeof(UINT)}; list->DispatchGraph(&linked_dispatch);
      for (auto &b : barriers) { b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; }
      list->ResourceBarrier(UINT(barriers.size()), barriers.data());
      list->CopyBufferRegion(link_readback.Get(), 0, link_result.Get(), 0, 8); list->CopyBufferRegion(link_readback.Get(), 8, helper_result.Get(), 0, 8);
      Check(list->Close(), "link Close"); ID3D12CommandList *lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
      Check(queue->Signal(fence.Get(), 6 + p), "link Signal"); Check(fence->SetEventOnCompletion(6 + p, event), "link completion event");
      Require(WaitForSingleObject(event, 180000) == WAIT_OBJECT_0, "linked GPU completion"); Check(device->GetDeviceRemovedReason(), "linked device alive");
      void *mapping = nullptr; D3D12_RANGE range{0, 16}; Check(link_readback->Map(0, &range, &mapping), "link result map");
      std::array<UINT, 4> actual{}; std::memcpy(actual.data(), mapping, sizeof(actual)); link_readback->Unmap(0, nullptr);
      const UINT expected_link_sum = p ? 248 : 196;
      std::printf("OWNED_D3D12_WORK_GRAPHS_LINK_RESULT program=%c exact_min=%llu sum=%u expected_sum=%u count=%u expected_count=4 helper_sum=%u expected_helper_sum=54 helper_count=%u expected_helper_count=4\n",
                  p ? 'B' : 'A', static_cast<unsigned long long>(requirements.MinSizeInBytes), actual[0], expected_link_sum, actual[1], actual[2], actual[3]);
      Require(actual == std::array<UINT, 4>{{expected_link_sum, 4, 54, 4}}, "linked GPU exact CPU oracle");
      std::printf("OWNED_D3D12_WORK_GRAPHS_CASE_PASS global_roots_collection_functions_program_%c\n", p ? 'B' : 'A'); std::fflush(stdout);
    }
    // A local static-only root contributes sampler state but requires no local argument bytes.
    D3D12_DESCRIPTOR_RANGE texture_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    std::array<D3D12_ROOT_PARAMETER, 2> sample_parameters{};
    sample_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; sample_parameters[0].DescriptorTable = {1, &texture_range};
    sample_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; sample_parameters[1].Descriptor.ShaderRegister = 0;
    D3D12_STATIC_SAMPLER_DESC local_sampler{}; local_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    local_sampler.AddressU = local_sampler.AddressV = local_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    local_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; local_sampler.MaxAnisotropy = 1; local_sampler.MaxLOD = D3D12_FLOAT32_MAX;
    local_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC conflict_sampler = local_sampler; conflict_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    std::array<D3D12_ROOT_SIGNATURE_DESC, 3> sample_root_descs{{
      {UINT(sample_parameters.size()), sample_parameters.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE},
      {0, nullptr, 1, &local_sampler, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE},
      {UINT(sample_parameters.size()), sample_parameters.data(), 1, &conflict_sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE}}};
    std::array<ComPtr<ID3D12RootSignature>, 3> sample_roots;
    for (UINT r = 0; r < sample_roots.size(); ++r) {
      ComPtr<ID3DBlob> serialized; Check(D3D12SerializeRootSignature(&sample_root_descs[r], D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr), "sample root serialization");
      Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&sample_roots[r])), "sample root");
    }
    D3D12_EXPORT_DESC sample_export{L"SampleLocal", nullptr, D3D12_EXPORT_FLAG_NONE};
    D3D12_DXIL_LIBRARY_DESC sample_library{{link_node_bytes.data(), link_node_bytes.size()}, 1, &sample_export};
    D3D12_GLOBAL_ROOT_SIGNATURE sample_global{sample_roots[0].Get()}, sample_conflict{sample_roots[2].Get()};
    D3D12_LOCAL_ROOT_SIGNATURE sample_local{sample_roots[1].Get()};
    LPCWSTR sample_export_name = L"SampleLocal";
    D3D12_NODE sample_node{}; sample_node.NodeType = dxmt::kNodeTypeShader; sample_node.Shader.Shader = sample_export_name;
    D3D12_WORK_GRAPH_DESC sample_graph{}; sample_graph.ProgramName = L"local-sampler"; sample_graph.NumExplicitlyDefinedNodes = 1; sample_graph.pExplicitlyDefinedNodes = &sample_node;
    std::array<D3D12_STATE_SUBOBJECT, 5> sample_subobjects{};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION sample_association{&sample_subobjects[2], 1, &sample_export_name};
    sample_subobjects = {{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &sample_library},
                         {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &sample_global},
                         {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &sample_local},
                         {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &sample_association},
                         {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &sample_graph}}};
    D3D12_STATE_OBJECT_DESC sample_state_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), UINT(sample_subobjects.size()), sample_subobjects.data()};
    sample_subobjects[1].pDesc = &sample_conflict;
    Require(device->CreateStateObject(&sample_state_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "conflicting global and local static sampler rejected");
    sample_subobjects[1].pDesc = &sample_global;
    auto embedded_sample_bytes = Shader("work-graphs-root-subobjects.dxil");
    std::array<D3D12_EXPORT_DESC, 2> embedded_sample_exports{{{L"SampleLocal", nullptr, D3D12_EXPORT_FLAG_NONE},
      {L"PublishedSampleRoot", L"SampleGlobal", D3D12_EXPORT_FLAG_NONE}}};
    D3D12_DXIL_LIBRARY_DESC embedded_sample_library{{embedded_sample_bytes.data(), embedded_sample_bytes.size()}, UINT(embedded_sample_exports.size()), embedded_sample_exports.data()};
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION embedded_sample_association{L"SampleGlobal", 1, &sample_export_name};
    std::array<D3D12_STATE_SUBOBJECT, 4> embedded_sample_subobjects{{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &embedded_sample_library},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &embedded_sample_association},
      sample_subobjects[2], {}}};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION embedded_local_association{&embedded_sample_subobjects[2], 1, &sample_export_name};
    embedded_sample_subobjects[3] = {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &embedded_local_association};
    D3D12_STATE_OBJECT_DESC sample_collection_desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, UINT(embedded_sample_subobjects.size()), embedded_sample_subobjects.data()};
    Require(device->CreateStateObject(&sample_collection_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "embedded root original name is private after rename");
    embedded_sample_association.SubobjectToAssociate = L"PublishedSampleRoot";
    embedded_sample_exports[1].Name = L"SampleLocal";
    embedded_sample_association.SubobjectToAssociate = L"SampleLocal";
    Require(device->CreateStateObject(&sample_collection_desc, IID_PPV_ARGS(&invalid_state)) == E_INVALIDARG, "function and named root collision rejected");
    embedded_sample_exports[1].Name = L"PublishedSampleRoot";
    embedded_sample_association.SubobjectToAssociate = L"PublishedSampleRoot";
    ComPtr<ID3D12StateObject> sample_collection; Check(device->CreateStateObject(&sample_collection_desc, IID_PPV_ARGS(&sample_collection)), "complete sampled node collection resource roots");
    std::array<D3D12_EXPORT_DESC, 2> sample_import_exports{{{L"ImportedSample", L"SampleLocal", D3D12_EXPORT_FLAG_NONE},
      {L"ImportedRoot", L"PublishedSampleRoot", D3D12_EXPORT_FLAG_NONE}}};
    D3D12_EXISTING_COLLECTION_DESC sample_existing{sample_collection.Get(), UINT(sample_import_exports.size()), sample_import_exports.data()};
    LPCWSTR imported_sample_name = L"ImportedSample"; sample_node.Shader.Shader = imported_sample_name;
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION sample_import_association{L"ImportedRoot", 1, &imported_sample_name};
    D3D12_STATE_OBJECT_CONFIG sample_additions{D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS};
    std::array<D3D12_STATE_SUBOBJECT, 4> sample_import{{{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &sample_existing},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &sample_import_association},
      {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(dxmt::kStateSubobjectTypeWorkGraph), &sample_graph},
      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &sample_additions}}};
    sample_state_desc.NumSubobjects = sample_import.size(); sample_state_desc.pSubobjects = sample_import.data();
    ComPtr<ID3D12StateObject> sample_state; Check(device->CreateStateObject(&sample_state_desc, IID_PPV_ARGS(&sample_state)), "local static-only sampler union state");
    D3D12_EXPORT_DESC inherited_root_export{L"ImportedRoot", L"SampleGlobal", D3D12_EXPORT_FLAG_NONE};
    D3D12_DXIL_LIBRARY_DESC inherited_root_library{{embedded_sample_bytes.data(), embedded_sample_bytes.size()}, 1, &inherited_root_export};
    std::array<D3D12_STATE_SUBOBJECT, 2> inherited_root_subobjects{{
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &inherited_root_library},
      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &sample_additions}}};
    D3D12_STATE_OBJECT_DESC inherited_root_desc{static_cast<D3D12_STATE_OBJECT_TYPE>(dxmt::kStateObjectTypeExecutable), UINT(inherited_root_subobjects.size()), inherited_root_subobjects.data()};
    ComPtr<ID3D12StateObject> inherited_root_state;
    Check(additions_device->AddToStateObject(&inherited_root_desc, sample_state.Get(), IID_PPV_ARGS(&inherited_root_state)), "inherited named root identical redeclaration");
    sample_state = inherited_root_state;
    ComPtr<ID3D12WorkGraphProperties> sample_properties; Check(sample_state.As(&sample_properties), "sample graph properties");
    ComPtr<ID3D12StateObjectProperties1> sample_identifiers; Check(sample_state.As(&sample_identifiers), "sample program identifier");
    UINT sample_graph_index = sample_properties->GetWorkGraphIndex(L"local-sampler");
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS sample_memory{}; sample_properties->GetWorkGraphMemoryRequirements(sample_graph_index, &sample_memory);
    auto sample_backing = Buffer(device.Get(), sample_memory.MinSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto sample_result = Buffer(device.Get(), 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto sample_readback = Buffer(device.Get(), 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_DESC texture_desc{}; texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture_desc.Width = 1; texture_desc.Height = 1;
    texture_desc.DepthOrArraySize = 1; texture_desc.MipLevels = 1; texture_desc.Format = DXGI_FORMAT_R32_FLOAT; texture_desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES texture_heap{}; texture_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> sample_texture; Check(device->CreateCommittedResource(&texture_heap, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&sample_texture)), "sample texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT texture_footprint{}; UINT64 texture_bytes = 0;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &texture_footprint, nullptr, nullptr, &texture_bytes);
    auto texture_upload = Buffer(device.Get(), texture_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    const float texel = 17; Upload(texture_upload.Get(), &texel, sizeof(texel), texture_footprint.Offset);
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> srv_heap; Check(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap)), "sample SRV heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R32_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(sample_texture.Get(), &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
    ComPtr<ID3D12CommandAllocator> sample_allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&sample_allocator)), "sample allocator");
    ComPtr<ID3D12GraphicsCommandList10> sample_list; Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, sample_allocator.Get(), nullptr, IID_PPV_ARGS(&sample_list)), "sample CommandList10");
    D3D12_TEXTURE_COPY_LOCATION texture_dst{}; texture_dst.pResource = sample_texture.Get(); texture_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION texture_src{}; texture_src.pResource = texture_upload.Get(); texture_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; texture_src.PlacedFootprint = texture_footprint;
    sample_list->CopyTextureRegion(&texture_dst, 0, 0, 0, &texture_src, nullptr);
    D3D12_RESOURCE_BARRIER sample_barrier{}; sample_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; sample_barrier.Transition.pResource = sample_texture.Get();
    sample_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; sample_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; sample_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sample_list->ResourceBarrier(1, &sample_barrier);
    sample_barrier.Transition.pResource = sample_result.Get(); sample_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; sample_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    sample_list->ResourceBarrier(1, &sample_barrier); sample_list->CopyBufferRegion(sample_result.Get(), 0, zero.Get(), 0, 8);
    sample_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; sample_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; sample_list->ResourceBarrier(1, &sample_barrier);
    ID3D12DescriptorHeap *sample_heaps[]{srv_heap.Get()}; sample_list->SetDescriptorHeaps(1, sample_heaps);
    sample_list->SetComputeRootSignature(sample_roots[0].Get()); sample_list->SetComputeRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
    sample_list->SetComputeRootUnorderedAccessView(1, sample_result->GetGPUVirtualAddress());
    D3D12_SET_PROGRAM_DESC sample_program{}; sample_program.Type = dxmt::kProgramTypeWorkGraph;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    sample_identifiers->GetProgramIdentifier(&sample_program.WorkGraph.ProgramIdentifier, L"local-sampler");
#else
    sample_program.WorkGraph.ProgramIdentifier = sample_identifiers->GetProgramIdentifier(L"local-sampler");
#endif
    sample_program.WorkGraph.Flags = dxmt::kSetWorkGraphFlagInitialize; sample_program.WorkGraph.BackingMemory = {sample_backing->GetGPUVirtualAddress(), sample_memory.MinSizeInBytes};
    // NodeLocalRootArgumentsTable stays {0,0,0}: the associated local root contains no parameters.
    sample_list->SetProgram(&sample_program);
    D3D12_DISPATCH_GRAPH_DESC sample_dispatch{}; sample_dispatch.Mode = 0;
    UINT sample_entry = sample_properties->GetEntrypointIndex(sample_graph_index, {L"sampleLocal", 0}); Require(sample_entry != UINT_MAX, "sample entrypoint");
    sample_dispatch.NodeCPUInput = {sample_entry, UINT(link_input.size()), link_input.data(), sizeof(UINT)}; sample_list->DispatchGraph(&sample_dispatch);
    sample_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; sample_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; sample_list->ResourceBarrier(1, &sample_barrier);
    sample_list->CopyBufferRegion(sample_readback.Get(), 0, sample_result.Get(), 0, 8); Check(sample_list->Close(), "sample Close");
    ID3D12CommandList *sample_lists[]{sample_list.Get()}; queue->ExecuteCommandLists(1, sample_lists);
    Check(queue->Signal(fence.Get(), 8), "sample Signal"); Check(fence->SetEventOnCompletion(8, event), "sample completion event");
    Require(WaitForSingleObject(event, 180000) == WAIT_OBJECT_0, "sample GPU completion"); Check(device->GetDeviceRemovedReason(), "sample device alive");
    void *sample_mapping = nullptr; D3D12_RANGE sample_range{0, 8}; Check(sample_readback->Map(0, &sample_range, &sample_mapping), "sample readback map");
    std::array<UINT, 2> sample_actual{}; std::memcpy(sample_actual.data(), sample_mapping, sizeof(sample_actual)); sample_readback->Unmap(0, nullptr);
    std::printf("OWNED_D3D12_WORK_GRAPHS_SAMPLER_RESULT exact_min=%llu sum=%u expected_sum=68 count=%u expected_count=4 local_argument_bytes=0\n", static_cast<unsigned long long>(sample_memory.MinSizeInBytes), sample_actual[0], sample_actual[1]);
    Require(sample_actual == std::array<UINT, 2>{{68, 4}}, "local static sampler exact CPU oracle");
    std::printf("OWNED_D3D12_WORK_GRAPHS_CASE_PASS local_static_sampler_union_exact\n");
    CloseHandle(event); std::printf("OWNED_D3D12_WORK_GRAPHS_PASS cases=9 tier=1_0\n"); return 0;
  } catch (...) { return 1; }
}
