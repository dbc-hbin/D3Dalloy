#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

// Stream-output contracts: PSO validation, SOSetTargets validation, SO_STATISTICS query heap
// Begin/End/Resolve, and GPU capture/assembly checked against a CPU model of D3D11.3 FS ch.8/14.

namespace {

constexpr DWORD kTimeoutMs = 15000;
constexpr std::uint32_t kGuard = 0xa5a5a5a5u;
int g_cases = 0;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}

void check(HRESULT hr, const char *step) {
  if (FAILED(hr))
    fail(step, hr);
}

void require(bool condition, const char *step) {
  if (!condition)
    fail(step);
}

void expect(HRESULT actual, HRESULT expected, const char *name) {
  if (actual != expected) {
    std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_HRESULT_MISMATCH case=%s expected=0x%08lx actual=0x%08lx\n",
                 name, static_cast<unsigned long>(expected), static_cast<unsigned long>(actual));
    fail(name, actual);
  }
  ++g_cases;
  std::printf("OWNED_D3D12_STREAM_OUTPUT_CASE_PASS case=%s hr=0x%08lx\n", name, static_cast<unsigned long>(actual));
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12-module");
  using BackendId = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backendId != nullptr, "owned-backend-id-export");
  const char *id = backendId();
  require(id && std::strcmp(id, "owned-graphics-d3d12") == 0, "owned-backend-id");
  std::printf("OWNED_D3D12_STREAM_OUTPUT_BACKEND id=%s\n", id);
}

ComPtr<ID3D12Resource> createBuffer(ID3D12Device *device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = heapType;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  // Default buffers back root UAVs and GPU-written indirect arguments; upload/readback heaps forbid the flag.
  if (heapType == D3D12_HEAP_TYPE_DEFAULT)
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&resource)), "create-buffer");
  return resource;
}

ComPtr<ID3DBlob> compile(const char *entry, const char *target) {
  static constexpr char source[] = R"hlsl(
struct VSOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};
VSOut vsMain(uint id : SV_VertexID) {
  VSOut o;
  o.position = float4(float(id), 1.0, 0.0, 1.0);
  o.uv = float2(float(id) * 0.5, 2.0);
  o.color = float4(0.25, 0.5, 0.75, 1.0);
  return o;
}
float4 psMain(VSOut i) : SV_Target0 { return i.color; }
)hlsl";
  ComPtr<ID3DBlob> code, errors;
  const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("D3DCompile", hr);
  }
  return code;
}

struct Fixture {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3DBlob> vs, ps;
};

D3D12_GRAPHICS_PIPELINE_STATE_DESC baseDesc(const Fixture &f) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = f.root.Get();
  desc.VS = {f.vs->GetBufferPointer(), f.vs->GetBufferSize()};
  desc.PS = {f.ps->GetBufferPointer(), f.ps->GetBufferSize()};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  return desc;
}

// Slot 0: SV_Position.xyzw (16 bytes). Slot 1: TEXCOORD0.xy, a 1-dword gap, COLOR0.z (16 bytes).
const D3D12_SO_DECLARATION_ENTRY kValidEntries[] = {
    {0, "SV_Position", 0, 0, 4, 0},
    {0, "TEXCOORD", 0, 0, 2, 1},
    {0, nullptr, 0, 0, 1, 1},
    {0, "COLOR", 0, 2, 1, 1},
};
const UINT kValidStrides[] = {16, 20};

HRESULT createSO(const Fixture &f, std::vector<D3D12_SO_DECLARATION_ENTRY> entries, std::vector<UINT> strides,
                 UINT rasterized = 0, ComPtr<ID3D12PipelineState> *out = nullptr,
                 const D3D12_CACHED_PIPELINE_STATE *cached = nullptr) {
  auto desc = baseDesc(f);
  desc.StreamOutput.pSODeclaration = entries.data();
  desc.StreamOutput.NumEntries = UINT(entries.size());
  desc.StreamOutput.pBufferStrides = strides.data();
  desc.StreamOutput.NumStrides = UINT(strides.size());
  desc.StreamOutput.RasterizedStream = rasterized;
  if (cached)
    desc.CachedPSO = *cached;
  ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr = f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  if (out)
    *out = pso;
  return hr;
}

std::vector<D3D12_SO_DECLARATION_ENTRY> validEntries() {
  return {std::begin(kValidEntries), std::end(kValidEntries)};
}
std::vector<UINT> validStrides() {
  return {std::begin(kValidStrides), std::end(kValidStrides)};
}

void runPipelineCases(const Fixture &f, ComPtr<ID3D12PipelineState> &soPso) {
  expect(createSO(f, validEntries(), validStrides(), 0, &soPso), S_OK, "pso_valid_rasterized_stream0");
  {
    ComPtr<ID3D12PipelineState> noRaster;
    expect(createSO(f, validEntries(), validStrides(), D3D12_SO_NO_RASTERIZED_STREAM, &noRaster), S_OK,
           "pso_valid_no_rasterized_stream");
  }
  expect(createSO(f, {{0, "sv_position", 0, 1, 2, 0}}, {8}), S_OK, "pso_case_insensitive_partial_components");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 4, 0}}, {2048}), S_OK, "pso_max_stride");

  auto entries = validEntries();
  entries[1].Stream = 1;
  expect(createSO(f, entries, validStrides()), E_INVALIDARG, "pso_stream1_without_gs");
  {
    auto desc = baseDesc(f);
    auto so = validEntries();
    auto strides = validStrides();
    desc.StreamOutput = {so.data(), UINT(so.size()), strides.data(), UINT(strides.size()), 0};
    desc.GS = desc.VS; // a vertex shader is not a valid geometry shader
    ComPtr<ID3D12PipelineState> pso;
    expect(f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), E_INVALIDARG, "pso_so_with_vs_as_gs");
  }
  expect(createSO(f, {{0, "SV_Position", 0, 0, 4, 4}}, {16, 16, 16, 16}), E_INVALIDARG, "pso_output_slot4");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 0, 0}}, {16}), E_INVALIDARG, "pso_component_count0");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 5, 0}}, {20}), E_INVALIDARG, "pso_component_count5");
  expect(createSO(f, {{0, "SV_Position", 0, 2, 3, 0}}, {16}), E_INVALIDARG, "pso_start_plus_count_gt4");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 2, 0}, {0, "SV_POSITION", 0, 1, 2, 0}}, {16}), E_INVALIDARG,
         "pso_overlapping_components");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 2, 0}, {0, "SV_Position", 0, 2, 2, 1}}, {8, 8}), S_OK,
         "pso_disjoint_components_same_element");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 4, 0}}, {16, 16, 16, 16, 16}), E_INVALIDARG, "pso_num_strides5");
  expect(createSO(f, validEntries(), {16}), E_INVALIDARG, "pso_referenced_slot_without_stride");
  expect(createSO(f, validEntries(), {16, 12}), E_INVALIDARG, "pso_stride_below_declared_size");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 4, 0}}, {2052}), E_INVALIDARG, "pso_stride_above_2048");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 4, 0}}, {18}), E_INVALIDARG, "pso_stride_not_multiple_of_4");
  expect(createSO(f, {{0, nullptr, 0, 0, 125, 0}, {0, "SV_Position", 0, 0, 4, 0}}, {2048}), E_INVALIDARG,
         "pso_more_than_128_components");
  expect(createSO(f, {{0, "SV_Position", 0, 0, 4, 0}, {0, nullptr, 0, 0, 4, 1}}, {16, 16}), E_INVALIDARG,
         "pso_gap_only_slot");
  expect(createSO(f, {{0, nullptr, 1, 0, 1, 0}, {0, "SV_Position", 0, 0, 4, 0}}, {20}), E_INVALIDARG,
         "pso_gap_with_semantic_index");
  expect(createSO(f, validEntries(), validStrides(), 1), E_INVALIDARG, "pso_rasterized_stream1_without_gs");
  expect(createSO(f, {{0, "NORMAL", 0, 0, 3, 0}}, {12}), E_INVALIDARG, "pso_missing_output_element");
  expect(createSO(f, {{0, "TEXCOORD", 0, 1, 2, 0}}, {8}), E_INVALIDARG, "pso_component_outside_output_mask");
  expect(createSO(f, {{0, "TEXCOORD", 1, 0, 2, 0}}, {8}), E_INVALIDARG, "pso_missing_semantic_index");

  // The cache key covers the SO declaration: the SO blob restores the SO PSO and is rejected
  // for the same pipeline without stream output or with a different declaration.
  ComPtr<ID3DBlob> blob;
  check(soPso->GetCachedBlob(&blob), "so-cached-blob");
  const D3D12_CACHED_PIPELINE_STATE cached = {blob->GetBufferPointer(), blob->GetBufferSize()};
  expect(createSO(f, validEntries(), validStrides(), 0, nullptr, &cached), S_OK, "pso_cache_roundtrip");
  {
    auto desc = baseDesc(f);
    desc.CachedPSO = cached;
    ComPtr<ID3D12PipelineState> pso;
    expect(f.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), E_INVALIDARG,
           "pso_cache_rejects_non_so_desc");
  }
  expect(createSO(f, validEntries(), {16, 24}, 0, nullptr, &cached), E_INVALIDARG, "pso_cache_rejects_other_strides");
  expect(createSO(f, validEntries(), validStrides(), D3D12_SO_NO_RASTERIZED_STREAM, nullptr, &cached), E_INVALIDARG,
         "pso_cache_rejects_other_rasterized_stream");
}

template <typename Record>
HRESULT recordAndClose(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, Record &&record,
                       ID3D12PipelineState *initial = nullptr) {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "case-allocator");
  check(device->CreateCommandList(0, type, allocator.Get(), initial, IID_PPV_ARGS(&list)), "case-list");
  record(list.Get());
  return list->Close();
}

void runTargetCases(const Fixture &f, ID3D12PipelineState *soPso) {
  ID3D12Device *device = f.device.Get();
  auto target = createBuffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_STREAM_OUT);
  const D3D12_GPU_VIRTUAL_ADDRESS base = target->GetGPUVirtualAddress();
  const D3D12_STREAM_OUTPUT_BUFFER_VIEW valid = {base, 192, base + 192};
  const auto direct = D3D12_COMMAND_LIST_TYPE_DIRECT;

  expect(recordAndClose(device, direct, [&](auto *list) {
           D3D12_STREAM_OUTPUT_BUFFER_VIEW views[4] = {valid, {}, {}, {base + 64, 64, base + 196}};
           list->SOSetTargets(0, 4, views);
           list->SOSetTargets(0, 4, nullptr);
           list->SOSetTargets(1, 1, views);
           D3D12_STREAM_OUTPUT_BUFFER_VIEW unbind = {0, 0, 0};
           list->SOSetTargets(1, 1, &unbind);
           list->SOSetTargets(4, 0, nullptr);
         }), S_OK, "so_set_targets_bind_unbind");
  expect(recordAndClose(device, direct, [&](auto *list) {
           D3D12_STREAM_OUTPUT_BUFFER_VIEW views[2] = {valid, valid};
           list->SOSetTargets(3, 2, views);
         }), E_INVALIDARG, "so_set_targets_slot_range");
  expect(recordAndClose(device, direct, [&](auto *list) { list->SOSetTargets(5, 0, nullptr); }), E_INVALIDARG,
         "so_set_targets_start_slot5");
  expect(recordAndClose(device, direct, [&](auto *list) {
           D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {base, 192, base + 194};
           list->SOSetTargets(0, 1, &view);
         }), E_INVALIDARG, "so_set_targets_filled_size_unaligned");
  expect(recordAndClose(device, direct, [&](auto *list) {
           D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {base, 192, 0};
           list->SOSetTargets(0, 1, &view);
         }), E_INVALIDARG, "so_set_targets_filled_size_null");
  expect(recordAndClose(device, direct, [&](auto *list) {
           D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {base, 192, base + 256};
           list->SOSetTargets(0, 1, &view);
         }), E_INVALIDARG, "so_set_targets_filled_size_out_of_range");
  expect(recordAndClose(device, direct, [&](auto *list) {
           D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {base + 128, 192, base + 64};
           list->SOSetTargets(0, 1, &view);
         }), E_INVALIDARG, "so_set_targets_size_exceeds_buffer");
  expect(recordAndClose(device, direct, [&](auto *list) {
           D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {0x10, 64, base};
           list->SOSetTargets(0, 1, &view);
         }), E_INVALIDARG, "so_set_targets_unresolved_location");
  expect(recordAndClose(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, [&](auto *list) {
           list->SOSetTargets(0, 1, &valid);
         }), E_INVALIDARG, "so_set_targets_bundle_rejected");

  // Draws with a stream-output PSO record; rasterizing VS-only adjacency remains unsupported.
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->SetGraphicsRootSignature(f.root.Get());
           list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
           list->SOSetTargets(0, 1, &valid);
           list->DrawInstanced(3, 1, 0, 0);
         }, soPso), S_OK, "draw_with_so_targets_records");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->SetGraphicsRootSignature(f.root.Get());
           list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ);
           list->DrawInstanced(4, 1, 0, 0);
         }, soPso), E_NOTIMPL, "draw_with_so_adjacency_notimpl");
}

void runQueryCases(const Fixture &f) {
  ID3D12Device *device = f.device.Get();
  const auto direct = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12QueryHeap> heap, occlusion;
  D3D12_QUERY_HEAP_DESC desc = {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 8, 0};
  expect(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap)), S_OK, "so_statistics_heap_create");
  desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  check(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&occlusion)), "occlusion-heap");
  auto destination = createBuffer(device, 512, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);

  expect(recordAndClose(device, direct, [&](auto *list) {
           list->BeginQuery(occlusion.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
         }), E_INVALIDARG, "so_query_wrong_heap_type");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
         }), E_INVALIDARG, "occlusion_query_on_so_heap");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 8);
         }), E_INVALIDARG, "so_query_index_out_of_range");
  expect(recordAndClose(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, [&](auto *list) {
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
         }), E_INVALIDARG, "so_query_compute_list");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 0);
         }), E_INVALIDARG, "so_query_end_without_begin");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
           list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 0);
         }), E_INVALIDARG, "so_query_end_other_stream");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2, 0);
         }), E_INVALIDARG, "so_query_double_begin");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
         }), E_INVALIDARG, "so_query_open_at_close");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, destination.Get(), 4);
         }), E_INVALIDARG, "so_resolve_unaligned_offset");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 2, destination.Get(), 496);
         }), E_INVALIDARG, "so_resolve_destination_overflow");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, destination.Get(), 0);
         }), E_INVALIDARG, "so_resolve_wrong_heap_type");
  expect(recordAndClose(device, direct, [&](auto *list) {
           list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 7, 2, destination.Get(), 0);
         }), E_INVALIDARG, "so_resolve_range_out_of_heap");
  expect(recordAndClose(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, [&](auto *list) {
           list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
         }), E_INVALIDARG, "so_query_bundle_rejected");

  // GPU: STREAM0..3 on indices 1..4 over an empty range resolve to zeros at an 8-aligned,
  // non-16-aligned offset; the guard words around the 64 resolved bytes are preserved.
  constexpr UINT64 kBytes = 256;
  constexpr UINT64 kOffset = 24;
  auto upload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto resolved = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
  auto readback = createBuffer(device, kBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  {
    std::uint32_t *mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "upload-map");
    for (UINT i = 0; i < kBytes / 4; ++i)
      mapped[i] = kGuard;
    upload->Unmap(0, nullptr);
  }
  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
  check(device->CreateCommandAllocator(direct, IID_PPV_ARGS(&allocator)), "allocator");
  check(device->CreateCommandList(0, direct, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  list->CopyBufferRegion(resolved.Get(), 0, upload.Get(), 0, kBytes);
  for (UINT s = 0; s < 4; ++s)
    list->BeginQuery(heap.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + s), 1 + s);
  for (UINT s = 0; s < 4; ++s)
    list->EndQuery(heap.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + s), 1 + s);
  for (UINT s = 0; s < 4; ++s)
    list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + s), 1 + s, 1,
                           resolved.Get(), kOffset + 16 * s);
  // The copy promoted `resolved` to COPY_DEST (a write state): reading it needs an explicit barrier.
  D3D12_RESOURCE_BARRIER toSource = {};
  toSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toSource.Transition = {resolved.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                         D3D12_RESOURCE_STATE_COPY_SOURCE};
  list->ResourceBarrier(1, &toSource);
  list->CopyBufferRegion(readback.Get(), 0, resolved.Get(), 0, kBytes);
  check(list->Close(), "gpu-close");
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  check(queue->Signal(fence.Get(), 1), "signal");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "event");
  check(fence->SetEventOnCompletion(1, event), "set-event");
  require(WaitForSingleObject(event, kTimeoutMs) == WAIT_OBJECT_0, "wait");
  CloseHandle(event);

  const D3D12_RANGE range{0, SIZE_T(kBytes)};
  const std::uint8_t *bytes = nullptr;
  check(readback->Map(0, &range, reinterpret_cast<void **>(const_cast<std::uint8_t **>(&bytes))), "readback-map");
  for (UINT64 i = 0; i < kBytes; i += 4) {
    std::uint32_t word;
    std::memcpy(&word, bytes + i, 4);
    const bool inside = i >= kOffset && i < kOffset + 64;
    if (word != (inside ? 0u : kGuard)) {
      std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_RESOLVE_MISMATCH byte=%llu got=0x%08x\n",
                   static_cast<unsigned long long>(i), word);
      fail("resolve-readback");
    }
  }
  const D3D12_RANGE written{0, 0};
  readback->Unmap(0, &written);
  ++g_cases;
  std::printf("OWNED_D3D12_STREAM_OUTPUT_QUERY_GPU_PASS streams=0,1,2,3 written=0 needed=0 offset=%llu guards=1\n",
              static_cast<unsigned long long>(kOffset));
}


// ---- GPU capture cases -------------------------------------------------------------------------
//
// Every case runs real draws, reads the stream-output targets, BufferFilledSize words, SO_STATISTICS
// results, the render target and a VS invocation counter back, and compares them with a CPU model
// of D3D11.3 FS 8.12-8.13 (strip cut, incomplete primitives), 8.16 (VertexID = index value for
// indexed draws, 0-based position otherwise) and 14.5-14.6 (complete primitives only, overflow stops
// writes but not PrimitivesStorageNeeded, unbound referenced slot = full, unreferenced slot ignored).

constexpr char kCaptureShader[] = R"hlsl(
RWStructuredBuffer<uint> invocations : register(u0);
struct VSOut {
  float4 position : SV_Position;
  uint4 a : TEXCOORD0;
  uint2 b : TEXCOORD1;
  uint2 ticket : TEXCOORD2;
};
VSOut vsMain(uint id : SV_VertexID, uint inst : SV_InstanceID) {
  uint previous;
  InterlockedAdd(invocations[0], 1, previous);
  VSOut o;
  o.position = float4(0.0, 0.0, 0.0, 1.0);
  o.a = uint4(id, inst, 100 + id, 200 + inst);
  o.b = uint2(id * 3, 7);
  // Unique per invocation: a record mixing two invocations of one vertex breaks ticket.y == ~ticket.x.
  o.ticket = uint2(previous, ~previous);
  return o;
}
float4 psMain(VSOut i) : SV_Target0 { return float4(0.0, 1.0, 0.0, 1.0); }
)hlsl";

// The PS consumes SV_ClipDistance0, which selects the captured-pixel distance path for rasterized
// draws; without a rasterized stream the PS never runs and SO must capture the raw VS distance.
constexpr char kDistanceShader[] = R"hlsl(
RWStructuredBuffer<uint> invocations : register(u0);
struct VSOut {
  float4 position : SV_Position;
  uint4 a : TEXCOORD0;
  float clip : SV_ClipDistance0;
};
VSOut body(uint id, uint inst, float clip) {
  uint previous;
  InterlockedAdd(invocations[0], 1, previous);
  VSOut o;
  o.position = float4(0.0, 0.0, 0.0, 1.0);
  o.a = uint4(id, inst, 100 + id, 200 + inst);
  o.clip = clip;
  return o;
}
VSOut vsFinite(uint id : SV_VertexID, uint inst : SV_InstanceID) {
  return body(id, inst, float(id) * 0.5 - 1.0);
}
VSOut vsNonFinite(uint id : SV_VertexID, uint inst : SV_InstanceID) {
  return body(id, inst, asfloat((id & 1) ? 0xff800000u : 0x7f800000u));
}
float4 psMain(VSOut i) : SV_Target0 { return float4(0.0, i.clip < 0.0 ? 0.5 : 1.0, 0.0, 1.0); }
)hlsl";

// Outputs of both capture shaders for one vertex: a/b are kCaptureShader's TEXCOORD0/1, lo/hi are
// the packed TEXCOORD0 (.xy) / TEXCOORD1 (.zw) of tests/dxil/stream-output-packed.hlsl.
struct Vertex {
  std::uint32_t a[4];
  std::uint32_t b[2];
  std::uint32_t lo[2];
  std::uint32_t hi[2];
  std::uint32_t clip[1]; // kDistanceShader SV_ClipDistance0 bits
};

// SV_ClipDistance0 of kDistanceShader: 1 = vsFinite, 2 = vsNonFinite (+inf even keys, -inf odd keys).
std::uint32_t modelClip(int distance, std::uint32_t key) {
  if (distance == 2)
    return (key & 1) ? 0xff800000u : 0x7f800000u;
  const float value = float(key) * 0.5f - 1.0f;
  std::uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

Vertex modelVertex(std::uint32_t key, std::uint32_t inst) {
  return {{key, inst, 100 + key, 200 + inst}, {key * 3, 7}, {key, inst}, {100 + key, 200 + inst}, {0}};
}

enum Source { kGap = -1, kA, kB, kTicket, kLo, kHi, kClip };

struct Piece {
  int source; // Source; kTicket is the invocation-unique (ticket, ~ticket) pair and has no fixed value
  UINT start, count;
};

struct Layout {
  std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
  std::vector<UINT> strides;
  std::array<std::vector<Piece>, 4> slots;
  UINT rasterized = 0;
};

// Slot 0: TEXCOORD0.xyzw with stride 20 (4 bytes past each vertex stay untouched).
// Slot 1: TEXCOORD1.y, a 1-dword gap, TEXCOORD1.x with stride 16 (swizzled, 4 tail bytes untouched).
Layout twoSlotLayout() {
  Layout l;
  l.entries = {{0, "TEXCOORD", 0, 0, 4, 0}, {0, "TEXCOORD", 1, 1, 1, 1}, {0, nullptr, 0, 0, 1, 1},
               {0, "TEXCOORD", 1, 0, 1, 1}};
  l.strides = {20, 16};
  l.slots[0] = {{0, 0, 4}};
  l.slots[1] = {{1, 1, 1}, {-1, 0, 1}, {1, 0, 1}};
  return l;
}

Layout oneSlotLayout(UINT rasterized = 0) {
  Layout l;
  l.entries = {{0, "TEXCOORD", 0, 0, 4, 0}};
  l.strides = {16};
  l.slots[0] = {{0, 0, 4}};
  l.rasterized = rasterized;
  return l;
}

// Slot 0: TEXCOORD2 (ticket, ~ticket) then TEXCOORD0.xy, stride 16.
Layout ticketLayout() {
  Layout l;
  l.entries = {{0, "TEXCOORD", 2, 0, 2, 0}, {0, "TEXCOORD", 0, 0, 2, 0}};
  l.strides = {16};
  l.slots[0] = {{kTicket, 0, 2}, {kA, 0, 2}};
  return l;
}

// Capture the original SM6.8 draw bases alongside the invocation-unique ticket.
// The full payload distinguishes command/instance boundaries from scheduler offsets.
Layout frontierLayout() {
  Layout l;
  l.entries = {{0, "TEXCOORD", 2, 0, 2, 0}, {0, "TEXCOORD", 0, 0, 4, 0}};
  l.strides = {24};
  l.slots[0] = {{kTicket, 0, 2}, {kA, 0, 4}};
  l.rasterized = D3D12_SO_NO_RASTERIZED_STREAM;
  return l;
}

// Packed DXIL shader: TEXCOORD1 occupies .zw of the register TEXCOORD0 uses for .xy, so its
// StartComponent 0 is .z. Slot 0: TEXCOORD1.xy then TEXCOORD0.y, stride 12.
Layout packedLayout() {
  Layout l;
  l.entries = {{0, "TEXCOORD", 1, 0, 2, 0}, {0, "texcoord", 0, 1, 1, 0}};
  l.strides = {12};
  l.slots[0] = {{kHi, 0, 2}, {kLo, 1, 1}};
  return l;
}

// kDistanceShader without a rasterized stream. Slot 0: TEXCOORD0.xyzw then SV_ClipDistance0, stride 20.
Layout distanceLayout() {
  Layout l;
  l.entries = {{0, "TEXCOORD", 0, 0, 4, 0}, {0, "SV_ClipDistance", 0, 0, 1, 0}};
  l.strides = {20};
  l.slots[0] = {{kA, 0, 4}, {kClip, 0, 1}};
  l.rasterized = D3D12_SO_NO_RASTERIZED_STREAM;
  return l;
}

struct Draw {
  D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  bool indexed = false;
  UINT count = 0;      // VertexCountPerInstance / IndexCountPerInstance
  UINT instances = 1;
  UINT start = 0;      // StartVertexLocation / StartIndexLocation
  INT baseVertex = 0;
  UINT startInstance = 0;
  bool indirect = false;
  UINT rootValue = 0;
  UINT vertexValue = 0;
  UINT indexByteOffset = 0;
  UINT sourceIndexOffset = 0;
  DXGI_FORMAT commandFormat = DXGI_FORMAT_UNKNOWN;
  UINT indexViewSize = UINT_MAX;
  bool nullIndexView = false;
};

struct Case {
  const char *name;
  Layout layout;
  std::vector<std::vector<Draw>> lists; // at most two command lists, executed together
  std::vector<std::uint32_t> indices;   // shared index buffer, stored as indexFormat
  DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  bool packedShader = false; // capture tests/dxil/stream-output-packed-vs.dxil instead of kCaptureShader
  bool frontierShader = false; // original SM6.8 bases and exact logical IA invocation count
  bool frontierCarry = false;  // every use of one shaded IA position must keep the same ticket
  int distance = 0;          // kDistanceShader VS: 0 none, 1 vsFinite, 2 vsNonFinite (PS reads SV_ClipDistance0)
  std::array<bool, 4> bound{true, true, false, false};
  std::array<UINT64, 4> size{512, 512, 512, 512};
  std::array<std::uint32_t, 4> filled{};
  int predicate = -1; // -1: none; 0: predicated off (EQUAL_ZERO on 0 -> skip); 1: predicated on
  int expectRt = -1;  // -1: unchecked; 0: untouched; 1: drawn
  UINT indirectCount = UINT_MAX; // GPU-written count; UINT_MAX selects all indirect records
  UINT maxIndirect = UINT_MAX;   // MaxCommandCount; UINT_MAX selects all indirect records
  bool mutating = false;
  bool mixedIndexFormats = false;
  bool conservative = false; // exercise the captured-vertex producer used by custom rasterization
  std::vector<Draw> repeatDraws; // same closed list, after a later GPU argument/count update
  UINT repeatCount = 0;
  Case(const char *n, Layout l, std::vector<std::vector<Draw>> d) : name(n), layout(std::move(l)), lists(std::move(d)) {}
};

constexpr UINT kTargetBytes = 1024;
constexpr UINT kFilledBase = 4 * kTargetBytes;
constexpr UINT kZeroOffset = kFilledBase + 48;
constexpr UINT kSoBufferBytes = kFilledBase + 64;
constexpr std::uint8_t kFill = 0xcd;

struct Model {
  std::array<std::vector<std::uint8_t>, 4> bytes;
  std::array<std::vector<UINT>, 4> tickets; // byte offsets of (ticket, ~ticket) pairs
  std::array<std::uint32_t, 4> filled{};
  std::vector<std::array<UINT64, 2>> perList;
  UINT64 maxInvocations = 0, minInvocations = 0;
  UINT64 primitiveVertices = 0; // vertices of complete primitives: the no-cache invocation count
};

UINT verticesPerPrimitive(D3D_PRIMITIVE_TOPOLOGY t) {
  if (t == D3D_PRIMITIVE_TOPOLOGY_POINTLIST)
    return 1;
  if (t == D3D_PRIMITIVE_TOPOLOGY_LINELIST || t == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ||
      t == D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ || t == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ)
    return 2;
  return 3;
}

UINT declaredIndexBytes(const Case &c, const Draw &d) {
  if (d.indexViewSize != UINT_MAX)
    return d.indexViewSize;
  const DXGI_FORMAT format = d.commandFormat == DXGI_FORMAT_UNKNOWN ? c.indexFormat : d.commandFormat;
  return c.mixedIndexFormats ? (format == DXGI_FORMAT_R16_UINT ? 8 : 16) : 256 - d.indexByteOffset;
}

UINT modelIndex(const Case &c, const Draw &d, UINT k) {
  const DXGI_FORMAT format = d.commandFormat == DXGI_FORMAT_UNKNOWN ? c.indexFormat : d.commandFormat;
  const UINT width = format == DXGI_FORMAT_R32_UINT ? 4 : 2;
  const UINT offset = (d.start + k) * width;
  const UINT size = declaredIndexBytes(c, d);
  if (d.nullIndexView || size < width || offset > size - width)
    return 0;
  const UINT source = d.sourceIndexOffset + offset / width;
  require(source < c.indices.size(), "so-oracle-index-initialized");
  return c.indices[source];
}

void modelDraw(const Case &c, const Draw &d, Model &m, std::array<UINT64, 2> &counts) {
  const Layout &l = c.layout;
  const UINT vpp = verticesPerPrimitive(d.topology);
  const bool adjacency = d.topology >= D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ &&
                         d.topology <= D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
  const UINT inputVpp = adjacency ? 2 * vpp : vpp;
  const bool strip = d.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ||
                     d.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
                     d.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
                     d.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
  // The strip cut applies to strip topologies only; elsewhere the cut value is an ordinary index.
  const DXGI_FORMAT format = d.commandFormat == DXGI_FORMAT_UNKNOWN ? c.indexFormat : d.commandFormat;
  const std::uint32_t cutValue = format == DXGI_FORMAT_R16_UINT ? 0xffffu : 0xffffffffu;
  const bool cut = strip && d.indexed &&
                   c.cut == (format == DXGI_FORMAT_R16_UINT ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                                                   : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF);
  std::uint32_t referenced = 0;
  for (UINT s = 0; s < 4; s++)
    if (!l.slots[s].empty())
      referenced |= 1u << s;
  if (c.predicate == 0)
    return;
  bool stopped = false;
  for (UINT s = 0; s < 4; s++)
    if ((referenced & (1u << s)) && !c.bound[s])
      stopped = true;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> unique;
  for (UINT inst = 0; inst < d.instances; inst++) {
    std::vector<std::uint32_t> window;
    UINT stripIndex = 0;
    for (UINT k = 0; k < d.count; k++) {
      std::uint32_t key = k;
      if (d.indexed) {
        key = modelIndex(c, d, k);
        if (cut && key == cutValue) {
          window.clear();
          stripIndex = 0;
          continue;
        }
      }
      m.maxInvocations++;
      std::vector<std::uint32_t> prim;
      if (adjacency) {
        window.push_back(key);
        if (window.size() == inputVpp) {
          // FS ch.14 streams only interior vertices; 8.14 keeps the leading vertex first.
          if (vpp == 2)
            prim = {window[1], window[2]};
          else
            prim = (stripIndex & 1) ? std::vector<std::uint32_t>{window[0], window[4], window[2]}
                                    : std::vector<std::uint32_t>{window[0], window[2], window[4]};
          if (strip) {
            window.erase(window.begin(), window.begin() + (vpp == 2 ? 1 : 2));
            stripIndex++;
          } else {
            window.clear();
          }
        }
      } else if (!strip) {
        window.push_back(key);
        if (window.size() == vpp) {
          prim = window;
          window.clear();
        }
      } else if (vpp == 2) {
        if (window.empty()) {
          window.push_back(key);
        } else {
          prim = {window[0], key};
          window[0] = key;
        }
      } else if (window.size() < 2) {
        window.push_back(key);
      } else {
        // Odd strip triangles swap their last two vertices so the leading vertex stays first.
        prim = (stripIndex & 1) ? std::vector<std::uint32_t>{window[0], key, window[1]}
                                : std::vector<std::uint32_t>{window[0], window[1], key};
        window = {window[1], key};
        stripIndex++;
      }
      if (prim.empty())
        continue;
      // Vertices of incomplete primitives may be skipped by the VS; complete ones must run.
      for (const auto key2 : prim)
        if (std::find(unique.begin(), unique.end(), std::make_pair(key2, inst)) == unique.end())
          unique.emplace_back(key2, inst);
      counts[1]++;
      m.primitiveVertices += vpp;
      for (UINT s = 0; s < 4 && !stopped; s++)
        if ((referenced & (1u << s)) &&
            (m.filled[s] > c.size[s] || UINT64(vpp) * l.strides[s] > c.size[s] - m.filled[s]))
          stopped = true;
      if (stopped)
        continue;
      for (UINT v = 0; v < vpp; v++) {
        Vertex vertex = modelVertex(prim[v], inst);
        if (c.frontierShader) {
          vertex.a[2] = d.indexed ? UINT(d.baseVertex) : d.start;
          vertex.a[3] = d.startInstance;
        }
        if (c.mutating)
          vertex.a[2] = d.rootValue + d.vertexValue;
        vertex.clip[0] = modelClip(c.distance, prim[v]);
        for (UINT s = 0; s < 4; s++) {
          if (!(referenced & (1u << s)))
            continue;
          UINT offset = m.filled[s] + v * l.strides[s];
          for (const auto &piece : l.slots[s]) {
            if (piece.source == kTicket)
              m.tickets[s].push_back(offset);
            for (UINT i = 0; i < piece.count; i++, offset += 4) {
              const std::uint32_t *words = piece.source == kA   ? vertex.a
                                           : piece.source == kB ? vertex.b
                                           : piece.source == kLo ? vertex.lo
                                           : piece.source == kHi ? vertex.hi
                                           : piece.source == kClip ? vertex.clip
                                                                 : nullptr;
              if (words)
                std::memcpy(m.bytes[s].data() + offset, &words[piece.start + i], 4);
            }
          }
        }
      }
      for (UINT s = 0; s < 4; s++)
        if (referenced & (1u << s))
          m.filled[s] += vpp * l.strides[s];
      counts[0]++;
    }
  }
  m.minInvocations += unique.size();
}

struct Recording {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
};

struct GpuContext {
  ID3D12Device *device = nullptr;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3DBlob> vs, ps;
  ComPtr<ID3DBlob> distanceVs[2], distancePs;
  std::vector<std::uint8_t> packedVs; // DXIL
  std::vector<std::uint8_t> frontierVs; // DXIL SM6.8, loaded only by focused frontier cases
  UINT targetBytes = kTargetBytes, filledBase = kFilledBase;
  UINT zeroOffset = kZeroOffset, soBufferBytes = kSoBufferBytes;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fenceValue = 0;
  ComPtr<ID3D12Resource> so, upload, readback, counter, predicate, indexBuffer, rt, rtReadback, queryReadback;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  ComPtr<ID3D12QueryHeap> queries;
  ComPtr<ID3D12RootSignature> indirectComputeRoot;
  ComPtr<ID3D12PipelineState> indirectCompute;
  ComPtr<ID3D12CommandSignature> indirectDraw, indirectIndexed, indirectMutating;
  ComPtr<ID3D12RootSignature> mutableRoot;
  ComPtr<ID3DBlob> mutableVs;
  ComPtr<ID3D12Resource> vbTen, vbTwenty;

  ~GpuContext() {
    if (event)
      CloseHandle(event);
  }

  void init(ID3D12Device *d, std::vector<std::uint8_t> packed, UINT targetSize = kTargetBytes) {
    device = d;
    packedVs = std::move(packed);
    targetBytes = targetSize;
    filledBase = 4 * targetBytes;
    zeroOffset = filledBase + 48;
    soBufferBytes = filledBase + 64;
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor = {0, 0};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
    ComPtr<ID3DBlob> blob, errors;
    check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "gpu-serialize-root");
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
          "gpu-root");
    ComPtr<ID3DBlob> compileErrors;
    const std::tuple<const char *, const char *, ComPtr<ID3DBlob> *> stages[] = {{"vsMain", "vs_5_0", &vs},
                                                                                 {"psMain", "ps_5_0", &ps}};
    for (const auto &[entry, target, out] : stages) {
      const HRESULT hr = D3DCompile(kCaptureShader, std::strlen(kCaptureShader), nullptr, nullptr, nullptr, entry,
                                    target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out->GetAddressOf(), &compileErrors);
      if (FAILED(hr)) {
        if (compileErrors)
          std::fwrite(compileErrors->GetBufferPointer(), 1, compileErrors->GetBufferSize(), stderr);
        fail("D3DCompile(capture)", hr);
      }
    }
    const std::tuple<const char *, const char *, ComPtr<ID3DBlob> *> distanceStages[] = {
        {"vsFinite", "vs_5_0", &distanceVs[0]}, {"vsNonFinite", "vs_5_0", &distanceVs[1]},
        {"psMain", "ps_5_0", &distancePs}};
    for (const auto &[entry, target, out] : distanceStages) {
      const HRESULT hr = D3DCompile(kDistanceShader, std::strlen(kDistanceShader), nullptr, nullptr, nullptr, entry,
                                    target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out->GetAddressOf(), &compileErrors);
      if (FAILED(hr)) {
        if (compileErrors)
          std::fwrite(compileErrors->GetBufferPointer(), 1, compileErrors->GetBufferSize(), stderr);
        fail("D3DCompile(distance)", hr);
      }
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "gpu-queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "gpu-fence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "gpu-event");
    so = createBuffer(device, soBufferBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    upload = createBuffer(device, soBufferBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = createBuffer(device, soBufferBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    counter = createBuffer(device, 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    predicate = createBuffer(device, 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    indexBuffer = createBuffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    queryReadback = createBuffer(device, 2 * 4 * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    rtReadback = createBuffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC queryDesc = {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 8, 0};
    check(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)), "gpu-query-heap");
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&rt)), "gpu-rt");
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "gpu-rtv-heap");
    device->CreateRenderTargetView(rt.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  void execute(const std::vector<ID3D12CommandList *> &lists) {
    queue->ExecuteCommandLists(UINT(lists.size()), lists.data());
    check(queue->Signal(fence.Get(), ++fenceValue), "gpu-signal");
    check(fence->SetEventOnCompletion(fenceValue, event), "gpu-set-event");
    require(WaitForSingleObject(event, kTimeoutMs) == WAIT_OBJECT_0, "gpu-wait");
  }

  void initIndirect() {
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    D3D12_COMMAND_SIGNATURE_DESC signature = {};
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&indirectDraw)), "so-draw-signature");
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    signature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&indirectIndexed)), "so-index-signature");
    D3D12_ROOT_PARAMETER graphicsParameters[2] = {};
    graphicsParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    graphicsParameters[0].Descriptor.ShaderRegister = 0;
    graphicsParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    graphicsParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    graphicsParameters[1].Constants.ShaderRegister = 0;
    graphicsParameters[1].Constants.Num32BitValues = 1;
    graphicsParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC graphicsRootDesc = {2, graphicsParameters, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> graphicsBlob, graphicsErrors;
    check(D3D12SerializeRootSignature(&graphicsRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                      &graphicsBlob, &graphicsErrors), "so-mutating-root-serialize");
    check(device->CreateRootSignature(0, graphicsBlob->GetBufferPointer(), graphicsBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&mutableRoot)), "so-mutating-root");
    D3D12_INDIRECT_ARGUMENT_DESC mutableArgs[5] = {};
    mutableArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    mutableArgs[0].Constant.RootParameterIndex = 1;
    mutableArgs[0].Constant.Num32BitValuesToSet = 1;
    mutableArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    mutableArgs[1].VertexBuffer.Slot = 31; // outside the shader's input layout; still a legal signature update
    mutableArgs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    mutableArgs[2].VertexBuffer.Slot = 0;
    mutableArgs[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    mutableArgs[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    signature.NumArgumentDescs = 5;
    signature.pArgumentDescs = mutableArgs;
    signature.ByteStride = 4 + 2 * sizeof(D3D12_VERTEX_BUFFER_VIEW) + sizeof(D3D12_INDEX_BUFFER_VIEW) +
                           sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    check(device->CreateCommandSignature(&signature, mutableRoot.Get(), IID_PPV_ARGS(&indirectMutating)),
          "so-mutating-signature");
    static constexpr char mutableSource[] = R"hlsl(
RWStructuredBuffer<uint> invocations : register(u0);
cbuffer CommandTag : register(b0) { uint rootTag; };
struct VSOut { float4 position : SV_Position; uint4 a : TEXCOORD0; };
VSOut vsMain(uint id : SV_VertexID, uint inst : SV_InstanceID, uint vertexTag : TEXCOORD0) {
  uint previous;
  InterlockedAdd(invocations[0], 1, previous);
  VSOut o;
  o.position = float4(0.0, 0.0, 0.0, 1.0);
  o.a = uint4(id, inst, rootTag + vertexTag, 200 + inst);
  return o;
}
)hlsl";
    const HRESULT mutableHr = D3DCompile(mutableSource, sizeof(mutableSource) - 1, nullptr, nullptr, nullptr,
                                         "vsMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                         &mutableVs, &graphicsErrors);
    if (FAILED(mutableHr)) {
      if (graphicsErrors)
        std::fwrite(graphicsErrors->GetBufferPointer(), 1, graphicsErrors->GetBufferSize(), stderr);
      fail("so-mutating-vs-compile", mutableHr);
    }
    vbTen = createBuffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    vbTwenty = createBuffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    for (const auto &[buffer, value] : {std::pair<ID3D12Resource *, UINT>{vbTen.Get(), 10},
                                       {vbTwenty.Get(), 20}}) {
      UINT *data = nullptr;
      check(buffer->Map(0, nullptr, reinterpret_cast<void **>(&data)), "so-vb-map");
      std::fill_n(data, 64, value);
      buffer->Unmap(0, nullptr);
    }

    D3D12_ROOT_PARAMETER parameters[3] = {};
    for (UINT i = 0; i < 2; i++) {
      parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      parameters[i].Descriptor.ShaderRegister = i;
      parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[2].Constants.ShaderRegister = 0;
    parameters[2].Constants.Num32BitValues = 3;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized, errors, shader;
    check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
          "so-indirect-root-serialize");
    check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(&indirectComputeRoot)), "so-indirect-root");
    static constexpr char source[] = R"hlsl(
RWByteAddressBuffer arguments : register(u0);
RWByteAddressBuffer commandCount : register(u1);
cbuffer Fixup : register(b0) { uint firstVertexCount; uint executedCount; uint drawOffset; };
[numthreads(1, 1, 1)] void main() {
  arguments.Store(drawOffset, firstVertexCount);
  commandCount.Store(0, executedCount);
}
)hlsl";
    const HRESULT hr = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (FAILED(hr)) {
      if (errors)
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
      fail("so-indirect-cs-compile", hr);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = indirectComputeRoot.Get();
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&indirectCompute)), "so-indirect-compute");
  }

  ComPtr<ID3D12PipelineState> pipeline(const Case &c, bool streamOutput = true) {
    const Layout &l = c.layout;
    const auto topology = c.lists[0][0].topology;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = c.mutating ? mutableRoot.Get() : root.Get();
    D3D12_INPUT_ELEMENT_DESC vertexInput = {"TEXCOORD", 0, DXGI_FORMAT_R32_UINT, 0, 0,
                                             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    if (c.mutating) {
      desc.VS = {mutableVs->GetBufferPointer(), mutableVs->GetBufferSize()};
      desc.InputLayout = {&vertexInput, 1};
    } else if (c.frontierShader) {
      require(!frontierVs.empty(), "frontier-vs-input");
      desc.VS = {frontierVs.data(), frontierVs.size()};
    } else if (c.packedShader) {
      // DXIL VS without PS (DXBC and DXIL stages cannot be mixed in one pipeline).
      desc.VS = {packedVs.data(), packedVs.size()};
    } else if (c.distance) {
      // The baseline omits the PS so its invocation count never depends on the distance replay path.
      const auto &distance = distanceVs[c.distance - 1];
      desc.VS = {distance->GetBufferPointer(), distance->GetBufferSize()};
      if (streamOutput)
        desc.PS = {distancePs->GetBufferPointer(), distancePs->GetBufferSize()};
    } else {
      desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
      desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    }
    if (streamOutput)
      desc.StreamOutput = {l.entries.data(), UINT(l.entries.size()), l.strides.data(), UINT(l.strides.size()),
                           l.rasterized};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster = c.conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                                            : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.PrimitiveTopologyType = verticesPerPrimitive(topology) == 1   ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                 : verticesPerPrimitive(topology) == 2 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                                       : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.IBStripCutValue = c.cut;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "capture-pso");
    return pso;
  }

  D3D12_VERTEX_BUFFER_VIEW vertexView(const Draw &draw) const {
    ID3D12Resource *buffer = draw.vertexValue == 20 ? vbTwenty.Get() : vbTen.Get();
    return {buffer->GetGPUVirtualAddress(), 256, 4};
  }

  D3D12_INDEX_BUFFER_VIEW indexView(const Case &c, const Draw &draw) const {
    if (draw.nullIndexView && draw.indexViewSize == 0)
      return {};
    const DXGI_FORMAT format = draw.commandFormat == DXGI_FORMAT_UNKNOWN ? c.indexFormat : draw.commandFormat;
    return {draw.nullIndexView ? 0 : indexBuffer->GetGPUVirtualAddress() + draw.indexByteOffset,
            declaredIndexBytes(c, draw), format};
  }

  void runCase(const Case &c) {
    std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_GPU_CASE_PHASE case=%s begin\n", c.name);
    std::fflush(stderr);
    // Reset the SO buffer, filled sizes, predicate, VS counter and render target.
    std::uint8_t *mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "case-upload-map");
    std::memset(mapped, kFill, filledBase);
    std::memset(mapped + filledBase, 0, soBufferBytes - filledBase);
    for (UINT s = 0; s < 4; s++)
      std::memcpy(mapped + filledBase + 4 * s, &c.filled[s], 4);
    upload->Unmap(0, nullptr);
    if (!c.indices.empty()) {
      check(indexBuffer->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "case-index-map");
      if (c.mixedIndexFormats)
        std::memset(mapped, 0, 256);
      for (size_t i = 0; i < c.indices.size(); i++) {
        if (c.mixedIndexFormats) {
          if (i < 4) {
            const auto index = static_cast<std::uint16_t>(c.indices[i]);
            std::memcpy(mapped + 2 * i, &index, 2);
          } else {
            std::memcpy(mapped + 16 + 4 * (i - 4), &c.indices[i], 4);
          }
        } else if (c.indexFormat == DXGI_FORMAT_R16_UINT) {
          const auto index = static_cast<std::uint16_t>(c.indices[i]);
          std::memcpy(mapped + 2 * i, &index, 2);
        } else {
          std::memcpy(mapped + 4 * i, &c.indices[i], 4);
        }
      }
      indexBuffer->Unmap(0, nullptr);
    }
    std::vector<std::uint8_t> argumentBytes;
    std::vector<UINT64> indirectOffsets(c.lists.size());
    bool indexedArguments = false, hasIndirect = false;
    UINT firstCount = 0;
    for (size_t listIndex = 0; listIndex < c.lists.size(); listIndex++) {
      bool seenGroup = false, inGroup = false;
      for (const auto &draw : c.lists[listIndex]) {
        if (!draw.indirect) {
          inGroup = false;
          continue;
        }
        if (!inGroup) {
          require(!seenGroup, "so-indirect-one-group-per-list");
          indirectOffsets[listIndex] = argumentBytes.size();
          seenGroup = inGroup = true;
        }
        if (!hasIndirect) {
          firstCount = draw.count;
          indexedArguments = draw.indexed;
          hasIndirect = true;
        }
        require(draw.indexed == indexedArguments, "so-indirect-homogeneous-signature");
        if (c.mutating) {
          require(draw.indexed, "so-mutating-requires-indexed");
          const D3D12_VERTEX_BUFFER_VIEW unused = {vbTen->GetGPUVirtualAddress(), 256, 4};
          const auto vb = vertexView(draw);
          const auto ib = indexView(c, draw);
          const D3D12_DRAW_INDEXED_ARGUMENTS args = {draw.count, draw.instances, draw.start, draw.baseVertex,
                                                     draw.startInstance};
          for (const auto &[data, size] : {std::pair<const void *, size_t>{&draw.rootValue, 4},
                                          {&unused, sizeof(unused)}, {&vb, sizeof(vb)},
                                          {&ib, sizeof(ib)}, {&args, sizeof(args)}}) {
            const auto *start = reinterpret_cast<const std::uint8_t *>(data);
            argumentBytes.insert(argumentBytes.end(), start, start + size);
          }
        } else if (draw.indexed) {
          D3D12_DRAW_INDEXED_ARGUMENTS args = {draw.count, draw.instances, draw.start, draw.baseVertex,
                                               draw.startInstance};
          const auto *start = reinterpret_cast<const std::uint8_t *>(&args);
          argumentBytes.insert(argumentBytes.end(), start, start + sizeof(args));
        } else {
          D3D12_DRAW_ARGUMENTS args = {draw.count, draw.instances, draw.start, draw.startInstance};
          const auto *start = reinterpret_cast<const std::uint8_t *>(&args);
          argumentBytes.insert(argumentBytes.end(), start, start + sizeof(args));
        }
      }
    }
    ComPtr<ID3D12Resource> argumentUpload, argumentBuffer, countBuffer;
    const UINT fixupOffset = c.mutating ? 4 + 2 * sizeof(D3D12_VERTEX_BUFFER_VIEW) +
                                          sizeof(D3D12_INDEX_BUFFER_VIEW) : 0;
    if (hasIndirect) {
      require(indirectCompute && indirectDraw && indirectIndexed, "so-indirect-init");
      // The copy initializes records, but the first draw count and command count are deliberately
      // poisoned: a GPU compute pass overwrites both after command-list recording.
      const UINT poisoned = 0;
      std::memcpy(argumentBytes.data() + fixupOffset, &poisoned, 4);
      argumentUpload = createBuffer(device, argumentBytes.size(), D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
      argumentBuffer = createBuffer(device, argumentBytes.size(), D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
      countBuffer = createBuffer(device, 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      check(argumentUpload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "so-indirect-upload-map");
      std::memcpy(mapped, argumentBytes.data(), argumentBytes.size());
      argumentUpload->Unmap(0, nullptr);
    }
    Recording setup = makeRecording();
    setup.list->CopyBufferRegion(so.Get(), 0, upload.Get(), 0, soBufferBytes);
    setup.list->CopyBufferRegion(counter.Get(), 0, upload.Get(), zeroOffset, 16);
    setup.list->CopyBufferRegion(predicate.Get(), 0, upload.Get(), zeroOffset, 16);
    const float black[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    setup.list->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), black, 0, nullptr);
    if (hasIndirect) {
      setup.list->CopyBufferRegion(argumentBuffer.Get(), 0, argumentUpload.Get(), 0, argumentBytes.size());
      D3D12_RESOURCE_BARRIER toUav = {};
      toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      toUav.Transition = {argumentBuffer.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
      setup.list->ResourceBarrier(1, &toUav);
      setup.list->SetPipelineState(indirectCompute.Get());
      setup.list->SetComputeRootSignature(indirectComputeRoot.Get());
      setup.list->SetComputeRootUnorderedAccessView(0, argumentBuffer->GetGPUVirtualAddress());
      setup.list->SetComputeRootUnorderedAccessView(1, countBuffer->GetGPUVirtualAddress());
      const UINT stride = c.mutating ? 4 + 2 * sizeof(D3D12_VERTEX_BUFFER_VIEW) + sizeof(D3D12_INDEX_BUFFER_VIEW) +
                                          sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
                                      : indexedArguments ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
                                                         : sizeof(D3D12_DRAW_ARGUMENTS);
      const UINT counts[] = {firstCount, c.indirectCount == UINT_MAX ? UINT(argumentBytes.size() / stride)
                                                                   : c.indirectCount, fixupOffset};
      setup.list->SetComputeRoot32BitConstants(2, 3, counts, 0);
      setup.list->Dispatch(1, 1, 1);
      D3D12_RESOURCE_BARRIER uav[2] = {};
      for (UINT j = 0; j < 2; j++) {
        uav[j].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[j].UAV.pResource = j ? countBuffer.Get() : argumentBuffer.Get();
      }
      setup.list->ResourceBarrier(2, uav);
      toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
      toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
      setup.list->ResourceBarrier(1, &toUav);
      toUav.Transition.pResource = countBuffer.Get();
      setup.list->ResourceBarrier(1, &toUav);
    }
    check(setup.list->Close(), "case-setup-close");
    execute({setup.list.Get()});

    auto pso = pipeline(c);
    std::vector<Recording> recordings;
    std::vector<ID3D12CommandList *> lists;
    const D3D12_GPU_VIRTUAL_ADDRESS base = so->GetGPUVirtualAddress();
    for (size_t i = 0; i < c.lists.size(); i++) {
      recordings.push_back(makeRecording());
      auto *list = recordings.back().list.Get();
      if (!i) {
        // The setup execution's promoted states decayed to COMMON. Enter the write states explicitly so
        // the copy list's before-states hold even when every draw is predicated off or has no work.
        D3D12_RESOURCE_BARRIER toWrite[2] = {};
        for (UINT j = 0; j < 2; j++) {
          toWrite[j].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          toWrite[j].Transition = {j ? counter.Get() : so.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COMMON,
                                   j ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_STREAM_OUT};
        }
        list->ResourceBarrier(2, toWrite);
      }
      list->SetGraphicsRootSignature(c.mutating ? mutableRoot.Get() : root.Get());
      list->SetGraphicsRootUnorderedAccessView(0, counter->GetGPUVirtualAddress());
      list->SetPipelineState(pso.Get());
      auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      const D3D12_VIEWPORT viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
      const D3D12_RECT scissor = {0, 0, 1, 1};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      D3D12_STREAM_OUTPUT_BUFFER_VIEW views[4] = {};
      for (UINT s = 0; s < 4; s++)
        if (c.bound[s])
          views[s] = {base + UINT64(s) * targetBytes, c.size[s], base + filledBase + 4 * s};
      list->SOSetTargets(0, 4, views);
      if (!c.indices.empty()) {
        const D3D12_INDEX_BUFFER_VIEW ibv = {indexBuffer->GetGPUVirtualAddress(), 256, c.indexFormat};
        list->IASetIndexBuffer(&ibv);
      }
      if (c.mutating) {
        const UINT initial = 99;
        list->SetGraphicsRoot32BitConstant(1, initial, 0);
        const auto vb = vertexView(c.lists[i][0]);
        list->IASetVertexBuffers(0, 1, &vb);
      }
      for (UINT q = 0; q < 4; q++)
        list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + q), UINT(4 * i + q));
      if (c.predicate >= 0)
        list->SetPredication(predicate.Get(), 0,
                             c.predicate ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO);
      for (size_t drawIndex = 0; drawIndex < c.lists[i].size();) {
        const auto &draw = c.lists[i][drawIndex];
        list->IASetPrimitiveTopology(draw.topology);
        if (draw.indirect) {
          size_t end = drawIndex + 1;
          while (end < c.lists[i].size() && c.lists[i][end].indirect)
            end++;
          const UINT count = UINT(end - drawIndex);
          list->ExecuteIndirect(c.mutating ? indirectMutating.Get() :
                                    draw.indexed ? indirectIndexed.Get() : indirectDraw.Get(),
                                std::min(count, c.maxIndirect), argumentBuffer.Get(), indirectOffsets[i],
                                countBuffer.Get(), 0);
          drawIndex = end;
        } else if (draw.indexed) {
          if (c.mutating) {
            list->SetGraphicsRoot32BitConstant(1, draw.rootValue, 0);
            const auto vb = vertexView(draw);
            const auto ib = indexView(c, draw);
            list->IASetVertexBuffers(0, 1, &vb);
            list->IASetIndexBuffer(&ib);
          }
          list->DrawIndexedInstanced(draw.count, draw.instances, draw.start, draw.baseVertex, draw.startInstance);
          drawIndex++;
        } else {
          list->DrawInstanced(draw.count, draw.instances, draw.start, draw.startInstance);
          drawIndex++;
        }
      }
      if (c.predicate >= 0)
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
      for (UINT q = 0; q < 4; q++)
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + q), UINT(4 * i + q));
      for (UINT q = 0; q < 4; q++)
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + q),
                               UINT(4 * i + q), 1, queryReadback.Get(), UINT64(4 * i + q) * 16);
      check(list->Close(), "case-draw-close");
      lists.push_back(list);
    }
    Recording copy = makeRecording();
    D3D12_RESOURCE_BARRIER toSource[2] = {};
    for (UINT j = 0; j < 2; j++) {
      toSource[j].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      toSource[j].Transition = {j ? counter.Get() : so.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                j ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_STREAM_OUT,
                                D3D12_RESOURCE_STATE_COPY_SOURCE};
    }
    copy.list->ResourceBarrier(2, toSource);
    copy.list->CopyBufferRegion(readback.Get(), 0, so.Get(), 0, soBufferBytes);
    copy.list->CopyBufferRegion(readback.Get(), zeroOffset, counter.Get(), 0, 4);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {rt.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    copy.list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = rtReadback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = rt.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    copy.list->ResourceBarrier(1, &barrier);
    check(copy.list->Close(), "case-copy-close");
    lists.push_back(copy.list.Get());
    execute(lists);
    if (!c.repeatDraws.empty()) {
      require(c.lists.size() == 1 && hasIndirect, "so-repeat-one-list");
      Recording update = makeRecording();
      D3D12_RESOURCE_BARRIER transitions[2] = {};
      for (UINT j = 0; j < 2; j++) {
        // Buffers decay to COMMON when the previous ExecuteCommandLists completes.
        transitions[j].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transitions[j].Transition = {j ? countBuffer.Get() : argumentBuffer.Get(), 0,
                                     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
      }
      update.list->ResourceBarrier(2, transitions);
      update.list->SetPipelineState(indirectCompute.Get());
      update.list->SetComputeRootSignature(indirectComputeRoot.Get());
      update.list->SetComputeRootUnorderedAccessView(0, argumentBuffer->GetGPUVirtualAddress());
      update.list->SetComputeRootUnorderedAccessView(1, countBuffer->GetGPUVirtualAddress());
      const UINT counts[] = {c.repeatDraws[0].count, c.repeatCount, fixupOffset};
      update.list->SetComputeRoot32BitConstants(2, 3, counts, 0);
      update.list->Dispatch(1, 1, 1);
      D3D12_RESOURCE_BARRIER uav[2] = {};
      for (UINT j = 0; j < 2; j++) {
        uav[j].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav[j].UAV.pResource = j ? countBuffer.Get() : argumentBuffer.Get();
        transitions[j].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        transitions[j].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
      }
      update.list->ResourceBarrier(2, uav);
      update.list->ResourceBarrier(2, transitions);
      check(update.list->Close(), "so-repeat-update-close");
      execute({update.list.Get()});
      execute(lists);
    }

    // Model only the records selected by ExecuteIndirect. Keeping inactive slots as no-work
    // draws also lets the direct invocation oracle avoid submitting poisoned unused arguments.
    Case active = c;
    for (auto &draws : active.lists) {
      UINT command = 0;
      for (auto &draw : draws) {
        if (!draw.indirect) {
          command = 0;
          continue;
        }
        if (c.predicate == 0 || command >= c.indirectCount || command >= c.maxIndirect) {
          draw.count = 0;
          draw.instances = 0;
        }
        ++command;
      }
    }
    // Model.
    Model m;
    for (UINT s = 0; s < 4; s++) {
      m.bytes[s].assign(targetBytes, kFill);
      m.filled[s] = c.filled[s];
    }
    for (const auto &draws : active.lists) {
      std::array<UINT64, 2> counts{};
      for (const auto &draw : draws)
        modelDraw(c, draw, m, counts);
      if (c.repeatDraws.empty())
        m.perList.push_back(counts);
    }
    if (!c.repeatDraws.empty()) {
      std::array<UINT64, 2> counts{};
      for (const auto &draw : c.repeatDraws)
        modelDraw(c, draw, m, counts);
      m.perList.push_back(counts);
    }

    // Compare.
    const D3D12_RANGE range{0, SIZE_T(soBufferBytes)};
    const std::uint8_t *bytes = nullptr;
    check(readback->Map(0, &range, reinterpret_cast<void **>(const_cast<std::uint8_t **>(&bytes))), "case-readback");
    UINT tickets = 0;
    std::map<std::array<UINT, 4>, UINT> shadedTickets;
    std::map<UINT, std::array<UINT, 4>> ticketOwners;
    for (UINT s = 0; s < 4; s++) {
      // Each captured record is one invocation's outputs: its ticket pair must be consistent.
      for (const UINT offset : m.tickets[s]) {
        std::uint32_t pair[2];
        std::memcpy(pair, bytes + s * targetBytes + offset, 8);
        if (pair[1] != ~pair[0]) {
          std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_RECORD_MIXED case=%s slot=%u byte=%u ticket=0x%08x,0x%08x\n",
                       c.name, s, offset, pair[0], pair[1]);
          fail(c.name);
        }
        if (c.frontierCarry) {
          std::array<UINT, 4> key;
          std::memcpy(key.data(), bytes + s * targetBytes + offset + 8, sizeof(key));
          const auto [same, inserted] = shadedTickets.emplace(key, pair[0]);
          require(inserted || same->second == pair[0], "frontier-carry-keeps-shaded-ticket");
          if (inserted)
            require(ticketOwners.emplace(pair[0], key).second, "frontier-distinct-keys-have-distinct-tickets");
        }
        std::memcpy(m.bytes[s].data() + offset, pair, 8);
        tickets++;
      }
      for (UINT i = 0; i < targetBytes; i++) {
        if (bytes[s * targetBytes + i] != m.bytes[s][i]) {
          std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_TARGET_MISMATCH case=%s slot=%u byte=%u got=0x%02x want=0x%02x\n",
                       c.name, s, i, bytes[s * targetBytes + i], m.bytes[s][i]);
          fail(c.name);
        }
      }
      std::uint32_t filled;
      std::memcpy(&filled, bytes + filledBase + 4 * s, 4);
      if (filled != m.filled[s]) {
        std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_FILLED_MISMATCH case=%s slot=%u got=%u want=%u\n", c.name, s,
                     filled, m.filled[s]);
        fail(c.name);
      }
    }
    std::uint32_t invocations;
    std::memcpy(&invocations, bytes + zeroOffset, 4);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    // Capture must not add a VS pass: invocations cover every unique (key, instance) of a complete
    // primitive. Non-indexed draws match the same draws without stream output (native Metal may
    // shade a vertex shared by strip primitives more than once). Indexed stream-output draws are
    // issued over their IA positions, so they stay within the no-cache bound (D3D11.3 FS 8.4:
    // [infinite cache .. no cache]): one invocation per primitive vertex, or per IA position.
    std::uint32_t baseline = baselineInvocations(active);
    if (!c.repeatDraws.empty()) {
      Case second = c;
      second.lists = {c.repeatDraws};
      baseline += baselineInvocations(second);
    }
    bool indexed = false;
    for (const auto &draws : c.lists)
      for (const auto &draw : draws)
        indexed |= draw.indexed;
    const UINT64 nativeMaximum = std::max(m.maxInvocations, m.primitiveVertices);
    if (c.frontierShader)
      std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_FRONTIER_INVOCATIONS case=%s invocations=%u "
                   "native_baseline=%u logical_positions=%llu native_min=%llu native_max=%llu keys=%zu\n",
                   c.name, invocations, baseline, static_cast<unsigned long long>(m.maxInvocations),
                   static_cast<unsigned long long>(m.minInvocations),
                   static_cast<unsigned long long>(nativeMaximum), shadedTickets.size());
    // The focused frontier shades each logical IA position once. Its native no-SO control
    // can legally re-shade strip vertices, so compare that control to its cache interval.
    const bool invocationsOk = c.frontierShader ? baseline >= m.minInvocations && baseline <= nativeMaximum
                               : indexed ? invocations <= nativeMaximum : invocations == baseline;
    if (!invocationsOk || invocations < m.minInvocations) {
      std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_INVOCATIONS_MISMATCH case=%s got=%u baseline=%u min=%llu\n",
                   c.name, invocations, baseline, static_cast<unsigned long long>(m.minInvocations));
      fail(c.name);
    }
    if (c.frontierShader) {
      require(invocations == m.maxInvocations, "frontier-shades-each-logical-ia-position-once");
      if (c.frontierCarry)
        require(shadedTickets.size() == m.maxInvocations, "frontier-carry-covers-every-logical-key");
    }
    const D3D12_RANGE queryRange{0, SIZE_T(2 * 4 * 16)};
    const UINT64 *stats = nullptr;
    check(queryReadback->Map(0, &queryRange, reinterpret_cast<void **>(const_cast<UINT64 **>(&stats))), "case-query");
    for (size_t i = 0; i < c.lists.size(); i++) {
      for (UINT q = 0; q < 4; q++) {
        const UINT64 written = stats[(4 * i + q) * 2], needed = stats[(4 * i + q) * 2 + 1];
        const UINT64 wantWritten = q ? 0 : m.perList[i][0], wantNeeded = q ? 0 : m.perList[i][1];
        if (written != wantWritten || needed != wantNeeded) {
          std::fprintf(stderr,
                       "OWNED_D3D12_STREAM_OUTPUT_QUERY_MISMATCH case=%s list=%zu stream=%u got=(%llu,%llu) want=(%llu,%llu)\n",
                       c.name, i, q, static_cast<unsigned long long>(written), static_cast<unsigned long long>(needed),
                       static_cast<unsigned long long>(wantWritten), static_cast<unsigned long long>(wantNeeded));
          fail(c.name);
        }
      }
    }
    queryReadback->Unmap(0, &none);
    if (c.expectRt >= 0) {
      const D3D12_RANGE rtRange{0, 4};
      const std::uint8_t *pixel = nullptr;
      check(rtReadback->Map(0, &rtRange, reinterpret_cast<void **>(const_cast<std::uint8_t **>(&pixel))), "case-rt");
      const bool drawn = pixel[1] == 0xff;
      const bool untouched = pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 0;
      rtReadback->Unmap(0, &none);
      if (c.expectRt ? !drawn : !untouched) {
        std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_RT_MISMATCH case=%s expect=%d\n", c.name, c.expectRt);
        fail(c.name);
      }
    }
    UINT64 written = 0, needed = 0;
    for (const auto &counts : m.perList) {
      written += counts[0];
      needed += counts[1];
    }
    ++g_cases;
    std::printf("OWNED_D3D12_STREAM_OUTPUT_GPU_CASE_PASS case=%s written=%llu needed=%llu filled=%u,%u "
                "invocations=%u baseline=%u ia_positions=%llu tickets=%u\n",
                c.name, static_cast<unsigned long long>(written), static_cast<unsigned long long>(needed), m.filled[0],
                m.filled[1], invocations, baseline, static_cast<unsigned long long>(m.maxInvocations), tickets);
    if (c.frontierShader)
      std::printf("OWNED_D3D12_STREAM_OUTPUT_FRONTIER_PASS case=%s original_bases=1 exact_ia=1 "
                  "carry=%u keys=%zu invocations=%u target_bytes=%u\n", c.name,
                  UINT(c.frontierCarry), shadedTickets.size(), invocations, targetBytes);
    std::fflush(stdout);
    std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_GPU_CASE_PHASE case=%s end\n", c.name);
    std::fflush(stderr);
  }

  // VS invocations of the same draws with the same PSO minus its stream-output declaration.
  std::uint32_t baselineInvocations(const Case &c) {
    Recording setup = makeRecording();
    setup.list->CopyBufferRegion(counter.Get(), 0, upload.Get(), zeroOffset, 16);
    setup.list->CopyBufferRegion(predicate.Get(), 0, upload.Get(), zeroOffset, 16);
    // Both copies promoted to COPY_DEST (a write state); later lists of the same execution need
    // explicit transitions before the UAV counter and the predicate are used.
    D3D12_RESOURCE_BARRIER toUse[2] = {};
    for (UINT j = 0; j < 2; j++) {
      toUse[j].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      toUse[j].Transition = {j ? predicate.Get() : counter.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST,
                             j ? D3D12_RESOURCE_STATE_PREDICATION : D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    }
    setup.list->ResourceBarrier(2, toUse);
    check(setup.list->Close(), "baseline-setup-close");
    auto pso = pipeline(c, false);
    std::vector<Recording> recordings;
    std::vector<ComPtr<ID3D12Resource>> normalizedIndices;
    std::vector<ID3D12CommandList *> lists = {setup.list.Get()};
    for (const auto &draws : c.lists) {
      recordings.push_back(makeRecording());
      auto *list = recordings.back().list.Get();
      list->SetGraphicsRootSignature(c.mutating ? mutableRoot.Get() : root.Get());
      list->SetGraphicsRootUnorderedAccessView(0, counter->GetGPUVirtualAddress());
      list->SetPipelineState(pso.Get());
      auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
      list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      const D3D12_VIEWPORT viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
      const D3D12_RECT scissor = {0, 0, 1, 1};
      list->RSSetViewports(1, &viewport);
      list->RSSetScissorRects(1, &scissor);
      if (!c.indices.empty()) {
        const D3D12_INDEX_BUFFER_VIEW ibv = {indexBuffer->GetGPUVirtualAddress(), 256, c.indexFormat};
        list->IASetIndexBuffer(&ibv);
      }
      if (c.predicate >= 0)
        list->SetPredication(predicate.Get(), 0,
                             c.predicate ? D3D12_PREDICATION_OP_NOT_EQUAL_ZERO : D3D12_PREDICATION_OP_EQUAL_ZERO);
      for (const auto &draw : draws) {
        // The invocation oracle for a zero-instance draw is exactly zero; submitting a giant
        // no-work direct draw is unnecessary and some Metal validators check its short IB anyway.
        if (!draw.instances)
          continue;
        list->IASetPrimitiveTopology(draw.topology);
        const bool normalizeIndex = draw.indexed && (draw.nullIndexView || draw.indexViewSize != UINT_MAX);
        if (c.mutating) {
          list->SetGraphicsRoot32BitConstant(1, draw.rootValue, 0);
          const auto vb = vertexView(draw);
          list->IASetVertexBuffers(0, 1, &vb);
          if (!normalizeIndex) {
            const auto ib = indexView(c, draw);
            list->IASetIndexBuffer(&ib);
          }
        }
        UINT firstIndex = draw.start;
        if (normalizeIndex && draw.count) {
          // The direct oracle uses an ordinary in-bounds IB containing the specified zero
          // results, independently of the indirect shader's guarded pointer implementation.
          const DXGI_FORMAT format = draw.commandFormat == DXGI_FORMAT_UNKNOWN ? c.indexFormat : draw.commandFormat;
          const UINT width = format == DXGI_FORMAT_R32_UINT ? 4 : 2;
          auto indices = createBuffer(device, UINT64(draw.count) * width, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
          void *memory = nullptr;
          check(indices->Map(0, nullptr, &memory), "so-oracle-index-map");
          for (UINT k = 0; k < draw.count; k++) {
            const UINT index = modelIndex(c, draw, k);
            if (width == 2)
              static_cast<std::uint16_t *>(memory)[k] = std::uint16_t(index);
            else
              static_cast<std::uint32_t *>(memory)[k] = index;
          }
          indices->Unmap(0, nullptr);
          const D3D12_INDEX_BUFFER_VIEW ib = {indices->GetGPUVirtualAddress(), draw.count * width, format};
          list->IASetIndexBuffer(&ib);
          normalizedIndices.push_back(std::move(indices));
          firstIndex = 0;
        }
        if (draw.indexed)
          list->DrawIndexedInstanced(draw.count, draw.instances, firstIndex, draw.baseVertex, draw.startInstance);
        else
          list->DrawInstanced(draw.count, draw.instances, draw.start, draw.startInstance);
      }
      check(list->Close(), "baseline-close");
      lists.push_back(list);
    }
    Recording copy = makeRecording();
    D3D12_RESOURCE_BARRIER counterToSource = {};
    counterToSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    counterToSource.Transition = {counter.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    copy.list->ResourceBarrier(1, &counterToSource);
    copy.list->CopyBufferRegion(readback.Get(), zeroOffset, counter.Get(), 0, 4);
    check(copy.list->Close(), "baseline-copy-close");
    lists.push_back(copy.list.Get());
    execute(lists);
    const D3D12_RANGE range{zeroOffset, zeroOffset + 4};
    const std::uint8_t *bytes = nullptr;
    check(readback->Map(0, &range, reinterpret_cast<void **>(const_cast<std::uint8_t **>(&bytes))), "baseline-map");
    std::uint32_t invocations;
    std::memcpy(&invocations, bytes + zeroOffset, 4);
    const D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return invocations;
  }

  Recording makeRecording() {
    Recording r;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&r.list)), "list");
    return r;
  }
};

Draw points(UINT count, UINT instances = 1, UINT start = 0, UINT startInstance = 0) {
  return {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, false, count, instances, start, 0, startInstance};
}
Draw drawOf(D3D_PRIMITIVE_TOPOLOGY topology, UINT count) {
  return {topology, false, count, 1, 0, 0, 0};
}

std::vector<std::uint8_t> readFile(const char *path) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail("open-packed-dxil", HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16ll * 1024 * 1024) {
    CloseHandle(file);
    fail("size-packed-dxil");
  }
  std::vector<std::uint8_t> bytes(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != bytes.size())
    fail("read-packed-dxil");
  return bytes;
}

// PSO validation that needs the packed DXIL shader and its UAV root signature.
void runPackedPipelineCases(const GpuContext &gpu) {
  auto create = [&](std::vector<D3D12_SO_DECLARATION_ENTRY> entries, std::vector<UINT> strides,
                    ID3D12RootSignature *root) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {gpu.packedVs.data(), gpu.packedVs.size()};
    desc.StreamOutput = {entries.data(), UINT(entries.size()), strides.data(), UINT(strides.size()),
                         D3D12_SO_NO_RASTERIZED_STREAM};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    return gpu.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  };
  const auto packed = packedLayout();
  expect(create(packed.entries, packed.strides, gpu.root.Get()), S_OK, "pso_packed_start_relative_to_element");
  expect(create({{0, "TEXCOORD", 1, 1, 2, 0}}, {8}, gpu.root.Get()), E_INVALIDARG, "pso_packed_component_outside_mask");
  expect(create({{0, "TEXCOORD", 0, 0, 2, 0}, {0, "TEXCOORD", 1, 0, 2, 1}}, {8, 8}, gpu.root.Get()), S_OK,
         "pso_packed_elements_do_not_overlap");
  // The root signature must opt into stream output (D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT).
  D3D12_ROOT_PARAMETER parameter = {};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, errors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "serialize-no-so-root");
  ComPtr<ID3D12RootSignature> noSoRoot;
  check(gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&noSoRoot)),
        "no-so-root");
  expect(create(packed.entries, packed.strides, noSoRoot.Get()), E_INVALIDARG, "pso_root_without_allow_stream_output");
  // The shader's embedded root signature lacks the flag as well.
  expect(create(packed.entries, packed.strides, nullptr), E_INVALIDARG,
         "pso_embedded_root_without_allow_stream_output");
}

void runGSStreamCases(GpuContext &gpu, const std::vector<std::uint8_t> &dxilVs,
                      const std::vector<std::uint8_t> &dxilGs);

size_t runFrontierCases(ID3D12Device *device, const std::vector<std::uint8_t> &packedVs) {
  GpuContext gpu;
  // Existing cases keep their original target sizes. Only this focused group retains every
  // primitive on both sides of the 256-position/64-slot scheduler boundaries.
  gpu.init(device, packedVs, 2 * 1024 * 1024);
  gpu.initIndirect();
  gpu.frontierVs = readFile("stream-output-frontier-vs.dxil");
  auto indirect = [](Draw draw) { draw.indirect = true; return draw; };
  std::vector<Case> cases;
  {
    // Direct SO uses index pulling too. Check the shader-visible original signed
    // BaseVertex, then a nonindexed StartVertex, independently of indirect scheduling.
    Case c{"direct_frontier_original_bases", frontierLayout(),
           {{{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 1, 2, 1, -37, 19},
             points(2, 1, 11, 23)}}};
    c.frontierShader = true;
    c.indices = {111, 4};
    c.indexFormat = DXGI_FORMAT_R32_UINT;
    c.expectRt = 0;
    cases.push_back(std::move(c));
  }
  {
    // 34 producer chunks per instance: quantum 64 ends inside instance 1. The same IA
    // position occurs in several SO triangles, but its already-shaded ticket must be identical.
    Case c{"indirect_frontier_strip_carry_bases", frontierLayout(),
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, false, 33 * 256 + 3, 2, 17, 0, 9})}}};
    c.frontierShader = c.frontierCarry = true;
    c.size[0] = gpu.targetBytes;
    c.expectRt = 0;
    cases.push_back(std::move(c));
  }
  {
    // The original signed vertex base and instance base distinguish every command. Two
    // unsafe records lie past the GPU count. Replay changes that count, never the recording.
    Case c{"indirect_frontier_commands_replay_bases", frontierLayout(),
           {std::vector<Draw>(67, indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true,
                                          UINT_MAX, UINT_MAX, UINT_MAX, 0, 0}))}};
    c.frontierShader = true;
    c.indices = {111, 4};
    c.indexFormat = DXGI_FORMAT_R32_UINT;
    for (UINT command = 0; command < 65; ++command)
      c.lists[0][command] = indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 1, 1, 1,
                                     INT(3 * command) - 64, 7 + 5 * command});
    c.indirectCount = 65;
    c.repeatCount = 17;
    c.repeatDraws.assign(c.lists[0].begin(), c.lists[0].begin() + c.repeatCount);
    c.size[0] = gpu.targetBytes;
    c.expectRt = 0;
    cases.push_back(std::move(c));
  }
  {
    // Buffer overflow must not stop the GPU cursor, shader side effects, or StorageNeeded.
    // 33 chunks per instance cross quantum 64 even though only the first two points fit.
    Case c{"indirect_frontier_overflow_progress", frontierLayout(),
           {{indirect(points(8193, 2, 37, 19))}}};
    c.frontierShader = true;
    c.size[0] = 2 * 24;
    c.expectRt = 0;
    cases.push_back(std::move(c));
  }
  for (const auto &c : cases)
    gpu.runCase(c);
  return cases.size();
}

void runCaptureCases(ID3D12Device *device, std::vector<std::uint8_t> packedVs) {
  GpuContext gpu;
  gpu.init(device, std::move(packedVs));
  runPackedPipelineCases(gpu);
  const auto one = oneSlotLayout();
  std::vector<Case> cases;
  cases.push_back({"point_list", one, {{points(5)}}});
  cases.push_back({"line_list_incomplete_dropped", one, {{drawOf(D3D_PRIMITIVE_TOPOLOGY_LINELIST, 5)}}});
  cases.push_back({"line_strip", one, {{drawOf(D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, 4)}}});
  cases.push_back({"triangle_list_incomplete_dropped", one, {{drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 7)}}});
  cases.push_back({"triangle_strip_odd_order", one, {{drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 6)}}});
  for (const auto &[directName, indexedName, topology, count] : {
           std::tuple{"adj_line_list_interior", "adj_indexed_line_list", D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ, 9u},
           std::tuple{"adj_line_strip_interior", "adj_indexed_line_strip_cut", D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ, 6u},
           std::tuple{"adj_triangle_list_interior", "adj_indexed_triangle_list", D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, 13u},
           std::tuple{"adj_triangle_strip_odd_order", "adj_indexed_triangle_strip_cut", D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, 11u}}) {
    Case direct{directName, oneSlotLayout(D3D12_SO_NO_RASTERIZED_STREAM),
                {{{topology, false, count, 2, 3, 0, 4}}}};
    direct.expectRt = 0;
    cases.push_back(direct);
    Case indexed{indexedName, direct.layout, {{{topology, true, count, 2, 1, -2, 4}}}};
    indexed.expectRt = 0;
    indexed.indices = {40}; // leading junk exercises StartIndexLocation
    for (UINT k = 0; k < count; k++)
      indexed.indices.push_back(3 + k / 2); // repeated indices still occupy distinct IA positions
    if (topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ||
        topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) {
      const UINT cut = topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ ? 0xffffu : 0xffffffffu;
      indexed.indexFormat = cut == 0xffffu ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
      indexed.cut = cut == 0xffffu ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                                 : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
      // An incomplete run between cuts must disappear; the final run restarts strip parity.
      indexed.indices.insert(indexed.indices.end(), {cut, 20, 21, 22, cut, 10, 11, 12, 13, 14, 15});
      indexed.lists[0][0].count = UINT(indexed.indices.size()) - 1;
    }
    cases.push_back(indexed);
  }
  {
    Case c{"adj_overflow_interior_primitive", oneSlotLayout(D3D12_SO_NO_RASTERIZED_STREAM),
           {{drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, 12)}}};
    c.size[0] = 80; // one 48-byte interior triangle fits; neither adjacency nor a partial triangle is written
    c.expectRt = 0;
    cases.push_back(c);
  }
  {
    Case c{"adj_strip_draw_boundary_reset", oneSlotLayout(D3D12_SO_NO_RASTERIZED_STREAM),
           {{drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, 5),
             drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, 8)}}};
    c.expectRt = 0;
    cases.push_back(c);
  }
  {
    // StartIndexLocation 1 skips the leading junk index; BaseVertexLocation does not change the key;
    // a repeated index and a 0xFFFF strip cut restart the strip.
    Case c{"indexed_strip_cut_repeat", one, {{{D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, true, 9, 1, 1, 5, 0}}}};
    c.indices = {40, 0, 1, 2, 3, 0xffff, 4, 5, 5, 6};
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    cases.push_back(c);
  }
  {
    Case c{"indexed_instanced_list", one, {{{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, true, 6, 2, 0, -3, 4}}}};
    c.indices = {7, 9, 8, 9, 7, 11};
    cases.push_back(c);
  }
  cases.push_back({"instanced_start_offsets", one, {{{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, false, 3, 3, 4, 0, 2}}}});
  cases.push_back({"two_slots_gap_stride", twoSlotLayout(), {{points(3)}}});
  {
    Case c{"append_two_draws_two_lists", twoSlotLayout(), {{points(2), points(1, 1, 4)}, {points(3, 1, 0, 1)}}};
    c.filled = {40, 16, 0, 0};
    cases.push_back(c);
  }
  {
    Case c{"exact_fit", one, {{drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6)}}};
    c.size = {96, 512, 512, 512};
    cases.push_back(c);
  }
  {
    // 40 bytes hold two 16-byte points; the third and later primitives are not written, needed keeps counting.
    Case c{"overflow_points", one, {{points(5)}}};
    c.size = {40, 512, 512, 512};
    cases.push_back(c);
  }
  {
    // 48 bytes hold one 32-byte line; the second line is not written partially.
    Case c{"overflow_no_partial_primitive", one, {{drawOf(D3D_PRIMITIVE_TOPOLOGY_LINELIST, 4)}}};
    c.size = {48, 512, 512, 512};
    cases.push_back(c);
  }
  {
    // Slot 1 is full, so nothing is written to slot 0 either; filled sizes stay.
    Case c{"overflow_one_slot_stops_all", twoSlotLayout(), {{points(4)}}};
    c.size = {512, 40, 512, 512};
    cases.push_back(c);
  }
  {
    Case c{"referenced_slot_unbound_is_full", twoSlotLayout(), {{points(3)}}};
    c.bound = {true, false, false, false};
    cases.push_back(c);
  }
  {
    Case c{"unreferenced_bound_slot_ignored", one, {{points(2)}}};
    c.bound = {true, false, true, false};
    c.filled = {0, 0, 8, 0};
    cases.push_back(c);
  }
  {
    Case c{"rasterized_stream0_draws", one, {{points(1)}}};
    c.expectRt = 1;
    cases.push_back(c);
  }
  {
    Case c{"no_rasterized_stream_rt_untouched", oneSlotLayout(D3D12_SO_NO_RASTERIZED_STREAM), {{points(3)}}};
    c.expectRt = 0;
    cases.push_back(c);
  }
  {
    Case c{"predicated_off", one, {{points(3)}}};
    c.predicate = 0;
    c.expectRt = 0;
    cases.push_back(c);
  }
  {
    Case c{"predicated_on", one, {{points(3)}}};
    c.predicate = 1;
    c.expectRt = 1;
    cases.push_back(c);
  }
  {
    // Outside strip topologies the cut value is an ordinary index (D3D12 IBStripCutValue).
    Case c{"indexed_list_ffff_not_cut", one, {{{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, true, 6, 1, 0, 2, 0}}}};
    c.indices = {0, 1, 0xffff, 2, 3, 4};
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    cases.push_back(c);
  }
  {
    // With the cut DISABLED, 0xFFFF is an ordinary strip vertex.
    Case c{"indexed_strip_cut_disabled_ffff", one, {{{D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, true, 5, 1, 0, 0, 0}}}};
    c.indices = {0, 1, 0xffff, 2, 3};
    cases.push_back(c);
  }
  {
    // Sparse keys: the capture holds one record per IA position, not per index-value range.
    Case c{"indexed_sparse_r16_instanced", one, {{{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 3, 2, 1, -7, 1}}}};
    c.indices = {9, 0, 0xfffe, 0xffff};
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    cases.push_back(c);
  }
  {
    Case c{"indexed_sparse_r32", one, {{{D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 3, 1, 0, 3, 0}}}};
    c.indices = {0, 0xfffffffeu, 0xffffffffu};
    c.indexFormat = DXGI_FORMAT_R32_UINT;
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    cases.push_back(c);
  }
  {
    Case c{"indexed_r32_line_strip_cut", one, {{{D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, true, 6, 1, 0, 0, 0}}}};
    c.indices = {5, 6, 0xffffffffu, 7, 8, 0x10000};
    c.indexFormat = DXGI_FORMAT_R32_UINT;
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    cases.push_back(c);
  }
  {
    // Vertices shared by strip primitives may be shaded more than once; each record stays one invocation's.
    Case c{"record_single_invocation_strip", ticketLayout(), {{drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 20)}}};
    cases.push_back(c);
  }
  {
    Case c{"record_single_invocation_indexed", ticketLayout(),
           {{{D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, true, 12, 2, 0, 0, 0}}}};
    c.indices = {0, 1, 2, 2, 1, 3, 0, 2, 4, 4, 2, 3};
    cases.push_back(c);
  }
  {
    Case c{"packed_dxil_element_components", packedLayout(), {{points(4, 2, 1, 3)}}};
    c.packedShader = true;
    cases.push_back(c);
  }
  gpu.initIndirect();
  const auto indirect = [](Draw draw) {
    draw.indirect = true;
    return draw;
  };
  {
    Case c{"indirect_draw_gpu_args_count", one, {{indirect(points(2, 2, 3, 4)), indirect(points(1, 1, 7, 2))}}};
    c.indirectCount = 2;
    cases.push_back(c);
  }
  {
    Case c{"indirect_draw_gpu_count_zero", one, {{indirect(points(0))}}};
    c.indirectCount = 0;
    c.expectRt = 0;
    cases.push_back(c);
  }
  {
    // MaxCommandCount is much larger than the initial metadata snapshot, but no record is live.
    // A readback capacity must not become a draw-count bound or force full-MaxCommandCount allocation.
    Case c{"indirect_bounded_snapshot_zero_count", one,
           {std::vector<Draw>(4097, indirect(points(UINT_MAX, UINT_MAX)))}};
    c.indirectCount = 0;
    c.expectRt = 0;
    cases.push_back(std::move(c));
  }
  {
    // The inactive records are deliberately unsafe to execute. The first snapshot fits the one
    // live command even though MaxCommandCount is larger than its bounded record storage.
    Case c{"indirect_bounded_snapshot_small_count", one,
           {std::vector<Draw>(4097, indirect(points(UINT_MAX, UINT_MAX)))}};
    c.lists[0][0] = indirect(points(2, 2, 3, 4));
    c.indirectCount = 1;
    cases.push_back(std::move(c));
  }
  {
    // Predication must reject the records before reading or sizing any of their huge draw counts.
    Case c{"indirect_bounded_snapshot_predicate_zero", one,
           {std::vector<Draw>(4097, indirect(points(UINT_MAX, UINT_MAX)))}};
    c.indirectCount = 4097;
    c.predicate = 0;
    c.expectRt = 0;
    cases.push_back(std::move(c));
  }
  {
    // A closed recording first needs one record, then the GPU increases its count beyond the
    // initial snapshot. The last live command proves that metadata growth did not truncate draws.
    Case c{"indirect_bounded_snapshot_replay_growth", one, {std::vector<Draw>(4097, indirect(points(0)))}};
    c.lists[0][0] = indirect(points(1));
    c.lists[0][2045] = indirect(points(2, 1, 5));
    c.indirectCount = 1;
    c.repeatCount = 2046;
    c.repeatDraws.assign(c.lists[0].begin(), c.lists[0].begin() + c.repeatCount);
    cases.push_back(std::move(c));
  }
  {
    Case c{"indirect_draw_count_clamped_to_max", one, {{indirect(points(1)), indirect(points(1, 1, 6))}}};
    c.indirectCount = 7;
    c.maxIndirect = 2;
    cases.push_back(c);
  }
  {
    Case c{"indirect_indexed_r16_start_base_instance", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 3, 2, 1, -3, 4})}}};
    c.indices = {19, 7, 8, 9};
    cases.push_back(c);
  }
  {
    // InstanceCount=0 is a no-work command even when IndexCountPerInstance is too large to
    // materialize as an SO position buffer. Nothing may touch the short IB or the SO target.
    Case c{"indirect_indexed_zero_instances_huge_count", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, UINT_MAX, 0, 0, 0, 0})}}};
    c.indices = {7};
    c.expectRt = 0;
    cases.push_back(c);
  }
  {
    Case c{"indirect_indexed_r32_strip_overflow", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, true, 6, 1, 0, 2, 1})}}};
    c.indices = {5, 6, 0xffffffffu, 7, 8, 9};
    c.indexFormat = DXGI_FORMAT_R32_UINT;
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    c.size[0] = 32; // exactly one line fits; remaining primitives count as storage needed
    cases.push_back(c);
  }
  {
    Case c{"indirect_predicate_skips_so_stats", one, {{indirect(points(3))}}};
    c.predicate = 0;
    c.expectRt = 0;
    cases.push_back(c);
  }
  {
    Case c{"indirect_predicate_executes", one, {{indirect(points(2))}}};
    c.predicate = 1;
    c.expectRt = 1;
    cases.push_back(c);
  }
  {
    Case c{"indirect_append_direct_indirect_direct", twoSlotLayout(),
           {{points(1), indirect(points(2, 1, 3)), points(1, 1, 7)}}};
    c.filled = {20, 16, 0, 0};
    c.size = {80, 48, 512, 512}; // last primitive overflows slot 1, but still counts as needed
    cases.push_back(c);
  }
  {
    Case c{"indirect_referenced_slot_unbound", twoSlotLayout(), {{indirect(points(2))}}};
    c.bound = {true, false, false, false};
    cases.push_back(c);
  }
  {
    Case c{"indirect_two_closed_lists_append", one,
           {{indirect(points(1, 1, 0))}, {indirect(points(2, 1, 4))}}};
    c.indirectCount = 1;
    cases.push_back(c);
  }
  {
    Case c{"indirect_packed_dxil_capture", packedLayout(), {{indirect(points(3, 1, 4, 2))}}};
    c.packedShader = true;
    cases.push_back(c);
  }
  {
    Case c{"indirect_mutating_mixed_ib_and_direct_rebind", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 2, 1, 1, 2, 3}),
             indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 2, 2, 0, 1, 4}),
             {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 1, 1, 2, 0, 5}}}};
    c.mutating = c.mixedIndexFormats = true;
    c.indices = {111, 4, 5, 6, 7, 9, 10, 11};
    auto &draws = c.lists[0];
    draws[0].rootValue = 5; draws[0].vertexValue = 10;
    draws[0].commandFormat = DXGI_FORMAT_R16_UINT;
    draws[1].rootValue = 7; draws[1].vertexValue = 20;
    draws[1].indexByteOffset = 16; draws[1].sourceIndexOffset = 4;
    draws[1].commandFormat = DXGI_FORMAT_R32_UINT;
    draws[2].rootValue = 12; draws[2].vertexValue = 10;
    draws[2].commandFormat = DXGI_FORMAT_R16_UINT;
    c.indirectCount = 2;
    cases.push_back(c);
  }
  {
    Case c{"indirect_mutating_r16_strip_cut", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, true, 6, 1, 0, 2, 3})}}};
    c.mutating = true;
    c.indices = {4, 5, 0xffffu, 6, 7, 8};
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    c.lists[0][0].rootValue = 37;
    c.lists[0][0].vertexValue = 20;
    cases.push_back(c);
  }
  {
    Case c{"indirect_mutating_bounded_views", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 3, 1, 1, 2, 3}),
             indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 3, 1, 1, 1, 4}),
             indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 2, 2, 0, 1, 5}),
             indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 2, 1, 0, 2, 6}),
             indirect({D3D_PRIMITIVE_TOPOLOGY_POINTLIST, true, 2, 1, 0, 1, 7})}}};
    c.mutating = c.mixedIndexFormats = true;
    c.indices = {4, 5, 6, 7, 8, 9, 10, 11};
    auto &draws = c.lists[0];
    draws[0].rootValue = 5; draws[0].vertexValue = 10;
    draws[0].commandFormat = DXGI_FORMAT_R16_UINT; draws[0].indexViewSize = 4;
    draws[1].rootValue = 7; draws[1].vertexValue = 20;
    draws[1].commandFormat = DXGI_FORMAT_R32_UINT; draws[1].indexViewSize = 10;
    draws[1].indexByteOffset = 16; draws[1].sourceIndexOffset = 4;
    draws[2].rootValue = 11; draws[2].vertexValue = 10;
    draws[2].commandFormat = DXGI_FORMAT_R16_UINT; draws[2].nullIndexView = true;
    draws[3].rootValue = 13; draws[3].vertexValue = 20;
    draws[3].commandFormat = DXGI_FORMAT_R32_UINT; draws[3].indexViewSize = 0;
    draws[3].indexByteOffset = 16; draws[3].sourceIndexOffset = 4;
    draws[4].rootValue = 17; draws[4].vertexValue = 10;
    draws[4].nullIndexView = true; draws[4].indexViewSize = 0;
    cases.push_back(std::move(c));
  }
  {
    Case c{"indirect_mutating_bounded_strip_cut", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, true, 5, 1, 0, 2, 3}),
             indirect({D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, true, 3, 1, 0, 1, 4})}}};
    c.mutating = true;
    c.indices = {4, 0xffffu, 5, 6};
    c.cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    c.lists[0][0].rootValue = 37; c.lists[0][0].vertexValue = 20;
    c.lists[0][0].indexViewSize = 6;
    c.lists[0][1].rootValue = 31; c.lists[0][1].vertexValue = 10;
    c.lists[0][1].nullIndexView = true;
    cases.push_back(std::move(c));
  }
  {
    Case c{"indirect_mutating_bounded_conservative_triangle", one,
           {{indirect({D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, true, 3, 1, 1, 2, 3})}}};
    c.mutating = c.conservative = true;
    c.indices = {4, 5, 6, 7};
    c.lists[0][0].rootValue = 9; c.lists[0][0].vertexValue = 10;
    c.lists[0][0].indexViewSize = 4;
    cases.push_back(std::move(c));
  }
  {
    Case c{"indirect_reexecute_closed_list_changed_gpu_args", one,
           {{indirect(points(2)), indirect(points(1, 1, 5))}}};
    c.indirectCount = 2;
    c.repeatCount = 1;
    c.repeatDraws = {indirect(points(1))};
    cases.push_back(c);
  }
  for (const auto &c : cases)
    gpu.runCase(c);
  const size_t frontierCases = runFrontierCases(device, gpu.packedVs);
  std::printf("OWNED_D3D12_STREAM_OUTPUT_CAPTURE_PASS cases=%zu\n", cases.size() + frontierCases);
  // A PS reading SV_ClipDistance0 must not select the captured-pixel distance path when no stream
  // is rasterized: SO holds the raw VS distance bits (finite, and +/-inf) and the RT stays untouched.
  std::vector<Case> distanceCases;
  {
    Case c{"no_raster_ps_clip_distance_finite", distanceLayout(),
           {{drawOf(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6)}}};
    c.distance = 1;
    c.expectRt = 0;
    distanceCases.push_back(c);
  }
  {
    Case c{"no_raster_ps_clip_distance_non_finite", distanceLayout(), {{points(4)}}};
    c.distance = 2;
    c.expectRt = 0;
    distanceCases.push_back(c);
  }
  for (const auto &c : distanceCases)
    gpu.runCase(c);
  std::printf("OWNED_D3D12_STREAM_OUTPUT_NO_RASTER_DISTANCE_PASS cases=%zu\n", distanceCases.size());
  runGSStreamCases(gpu, readFile("stream-output-gs-vs_gs.dxil"), readFile("stream-output-gs-gs_multi.dxil"));
}

/*
 * GS stream output to several streams (stream-output-gs.hlsl, DXBC from D3DCompile and frozen DXIL). Each case
 * declares a subset of the GS streams with mixed formats, components, gaps and multi-slot streams, draws N points
 * twice (the second draw appends after the first's BufferFilledSize) and checks against a CPU model:
 *   - every slot's bytes (records, gaps untouched = kFill, the bytes after the filled size untouched),
 *   - every slot's BufferFilledSize,
 *   - SO_STATISTICS_STREAM0..3 (NumPrimitivesWritten, PrimitivesStorageNeeded) around both draws; a stream
 *     without a declaration counts neither, and a stream overflows alone (D3D11.3 14.5).
 * Records of input point p, emission k (all point lists):
 *   stream 0: VALUE.xyzw = (p, 0, 0, 0xa0) as uint
 *   stream 1: TEX.xy = (float(p), float(k)), k < 2
 *   stream 3: IDA = 10p + k, IDB.xy = (10p + k + 100, 7), k < 3
 */
struct GSStreamCase {
  const char *name;
  std::vector<D3D12_SO_DECLARATION_ENTRY> entries;
  std::vector<UINT> strides;
  UINT rasterized;
  std::array<UINT, 4> capacity; // slot sizes in bytes
};

struct GSStreamModel {
  std::array<std::vector<std::uint8_t>, 4> slots;
  std::array<UINT, 4> filled{};
  std::array<UINT64, 4> written{}, needed{};
};

// The dwords of component `c` of `semantic` for point p, emission k on `stream`.
std::uint32_t gsComponent(UINT stream, const char *semantic, UINT c, UINT p, UINT k) {
  auto f = [](float v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    return u;
  };
  if (stream == 0 && !std::strcmp(semantic, "VALUE"))
    return std::array<std::uint32_t, 4>{p, 0, 0, 0xa0}[c];
  if (stream == 1 && !std::strcmp(semantic, "TEX"))
    return c == 0 ? f(float(p)) : f(float(k));
  if (stream == 3 && !std::strcmp(semantic, "IDA"))
    return 10 * p + k;
  if (stream == 3 && !std::strcmp(semantic, "IDB"))
    return c == 0 ? 10 * p + k + 100 : 7;
  fail("gs-stream-model-semantic");
}

void modelGSDraw(const GSStreamCase &c, UINT points, UINT firstPoint, GSStreamModel &m) {
  constexpr UINT kEmitted[4] = {1, 2, 1, 3};
  for (UINT stream = 0; stream < 4; ++stream) {
    std::array<UINT, 4> slotBytes{};
    bool declared = false;
    for (const auto &e : c.entries)
      if (e.Stream == stream) {
        slotBytes[e.OutputSlot] += 4 * e.ComponentCount;
        declared = true;
      }
    if (!declared)
      continue;
    for (UINT p = firstPoint; p < firstPoint + points; ++p)
      for (UINT k = 0; k < kEmitted[stream]; ++k) {
        ++m.needed[stream];
        bool fits = true;
        for (UINT slot = 0; slot < 4; ++slot)
          if (slotBytes[slot] && m.filled[slot] + c.strides[slot] > c.capacity[slot])
            fits = false;
        if (!fits)
          continue;
        std::array<UINT, 4> offset{};
        for (const auto &e : c.entries) {
          if (e.Stream != stream)
            continue;
          for (UINT i = 0; i < e.ComponentCount; ++i)
            if (e.SemanticName) {
              const std::uint32_t word = gsComponent(stream, e.SemanticName, e.StartComponent + i, p, k);
              std::memcpy(m.slots[e.OutputSlot].data() + m.filled[e.OutputSlot] + offset[e.OutputSlot] + 4 * i,
                          &word, 4);
            }
          offset[e.OutputSlot] += 4 * e.ComponentCount;
        }
        for (UINT slot = 0; slot < 4; ++slot)
          if (slotBytes[slot])
            m.filled[slot] += c.strides[slot];
        ++m.written[stream];
      }
  }
}

void runGSStreamCases(GpuContext &gpu, const std::vector<std::uint8_t> &dxilVs,
                      const std::vector<std::uint8_t> &dxilGs) {
  ID3D12Device *device = gpu.device;
  static constexpr char kSource[] = R"hlsl(
struct VSOut { float4 position : SV_Position; uint id : VERTEXID; };
VSOut vs_gs(uint id : SV_VertexID) { VSOut o; o.position = float4(float(id), 0, 0, 1); o.id = id; return o; }
struct S0 { float4 position : SV_Position; uint4 value : VALUE; };
struct S1 { float2 tex : TEX; };
struct S3 { uint a : IDA; uint2 b : IDB; };
[maxvertexcount(7)]
void gs_multi(point VSOut input[1], inout PointStream<S0> s0, inout PointStream<S1> s1, inout PointStream<S0> s2,
              inout PointStream<S3> s3) {
  const uint p = input[0].id;
  S0 a; a.position = input[0].position; a.value = uint4(p, 0, 0, 0xa0); s0.Append(a);
  [unroll] for (uint k = 0; k < 2; ++k) { S1 b; b.tex = float2(p, k); s1.Append(b); }
  s2.Append(a);
  [unroll] for (uint j = 0; j < 3; ++j) { S3 c; c.a = 10 * p + j; c.b = uint2(10 * p + j + 100, 7); s3.Append(c); }
}
)hlsl";
  auto compileGS = [&](const char *entry, const char *target) {
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
      if (errors)
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
      fail("D3DCompile(gs-streams)", hr);
    }
    const auto *bytes = static_cast<const std::uint8_t *>(code->GetBufferPointer());
    return std::vector<std::uint8_t>(bytes, bytes + code->GetBufferSize());
  };
  const std::vector<std::uint8_t> dxbcVs = compileGS("vs_gs", "vs_5_0"), dxbcGs = compileGS("gs_multi", "gs_5_0");

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
  ComPtr<ID3DBlob> rootBlob, rootErrors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors),
        "gs-streams-root-serialize");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "gs-streams-root");

  const std::vector<GSStreamCase> cases = {
      // Streams 0 and 1 to one slot each; no rasterized stream.
      {"gs-streams-0-1", {{0, "VALUE", 0, 0, 4, 0}, {1, "TEX", 0, 0, 2, 1}}, {16, 8},
       D3D12_SO_NO_RASTERIZED_STREAM, {1024, 1024, 0, 0}},
      // Stream 3 split over slots 0 and 2 with a gap, stream 1 partial components to slot 3; stream 0 undeclared.
      {"gs-streams-3-split-1",
       {{3, "IDB", 0, 1, 1, 0}, {3, nullptr, 0, 0, 1, 0}, {3, "IDA", 0, 0, 1, 2}, {1, "TEX", 0, 1, 1, 3}},
       {12, 0, 4, 4}, D3D12_SO_NO_RASTERIZED_STREAM, {1024, 0, 1024, 1024}},
      // All declared streams; stream 3 overflows on slot 2 alone (5 of 18 records fit), stream 1 is exact.
      {"gs-streams-overflow",
       {{0, "VALUE", 0, 0, 4, 0}, {1, "TEX", 0, 0, 2, 1}, {3, "IDA", 0, 0, 1, 2}, {3, "IDB", 0, 0, 2, 3}},
       {16, 8, 4, 8}, D3D12_SO_NO_RASTERIZED_STREAM, {1024, 96, 20, 1024}},
      // Rasterized stream 0 (stream 2 is never streamed out; its outputs only feed nothing).
      {"gs-streams-raster0", {{1, "TEX", 0, 0, 2, 0}, {3, "IDA", 0, 0, 1, 1}}, {8, 4}, 0, {1024, 1024, 0, 0}},
  };
  constexpr UINT kPoints = 3, kSlotBytes = 1024, kFilledAt = 4 * kSlotBytes, kBytes = kFilledAt + 64;
  constexpr std::uint8_t kFillByte = 0xcd;

  for (int dxil = 0; dxil < 2; ++dxil) {
    const auto &vs = dxil ? dxilVs : dxbcVs;
    const auto &gs = dxil ? dxilGs : dxbcGs;
    for (const auto &c : cases) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
      desc.pRootSignature = root.Get();
      desc.VS = {vs.data(), vs.size()};
      desc.GS = {gs.data(), gs.size()};
      desc.StreamOutput = {c.entries.data(), UINT(c.entries.size()), c.strides.data(), UINT(c.strides.size()),
                           c.rasterized};
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = UINT_MAX;
      desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      desc.SampleDesc.Count = 1;
      ComPtr<ID3D12PipelineState> pso;
      check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), c.name);

      auto upload = createBuffer(device, kBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      {
        std::uint8_t *mapped = nullptr;
        check(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped)), "gs-streams-upload-map");
        std::memset(mapped, kFillByte, kFilledAt);
        std::memset(mapped + kFilledAt, 0, kBytes - kFilledAt);
        upload->Unmap(0, nullptr);
      }
      auto target = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      auto readback = createBuffer(device, kBytes + 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_QUERY_HEAP_DESC heapDesc = {D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 4, 0};
      ComPtr<ID3D12QueryHeap> queries;
      check(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queries)), "gs-streams-query-heap");

      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
            "gs-streams-allocator");
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                      IID_PPV_ARGS(&list)), "gs-streams-list");
      list->CopyBufferRegion(target.Get(), 0, upload.Get(), 0, kBytes);
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition = {target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_STREAM_OUT};
      list->ResourceBarrier(1, &barrier);
      list->SetGraphicsRootSignature(root.Get());
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
      D3D12_STREAM_OUTPUT_BUFFER_VIEW views[4] = {};
      for (UINT slot = 0; slot < c.strides.size(); ++slot)
        if (c.capacity[slot]) {
          const auto base = target->GetGPUVirtualAddress() + slot * kSlotBytes;
          views[slot] = {base, c.capacity[slot], target->GetGPUVirtualAddress() + kFilledAt + 16 * slot};
        }
      list->SOSetTargets(0, 4, views);
      for (UINT s = 0; s < 4; ++s)
        list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + s), s);
      // The second draw appends after the first's BufferFilledSize (StartVertexLocation only moves the fetch).
      list->DrawInstanced(kPoints, 1, 0, 0);
      list->DrawInstanced(kPoints, 1, kPoints, 0);
      for (UINT s = 0; s < 4; ++s) {
        list->EndQuery(queries.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + s), s);
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE(D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + s), s, 1,
                               readback.Get(), kBytes + 16 * s);
      }
      list->SOSetTargets(0, 0, nullptr);
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      list->ResourceBarrier(1, &barrier);
      list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, kBytes);
      check(list->Close(), "gs-streams-close");
      gpu.execute({list.Get()});
      check(device->GetDeviceRemovedReason(), "gs-streams-removed");

      GSStreamModel m;
      for (auto &slot : m.slots)
        slot.assign(kSlotBytes, kFillByte);
      modelGSDraw(c, kPoints, 0, m);
      modelGSDraw(c, kPoints, 0, m); // non-indexed SV_VertexID is 0-based in each draw (FS 8.16)

      const D3D12_RANGE range{0, SIZE_T(kBytes + 64)};
      const std::uint8_t *bytes = nullptr;
      check(readback->Map(0, &range, reinterpret_cast<void **>(const_cast<std::uint8_t **>(&bytes))),
            "gs-streams-map");
      bool ok = true;
      for (UINT slot = 0; slot < 4; ++slot) {
        for (UINT i = 0; i < kSlotBytes; ++i)
          if (bytes[slot * kSlotBytes + i] != m.slots[slot][i]) {
            std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_GS_STREAM_MISMATCH case=%s dxil=%d slot=%u byte=%u "
                         "got=0x%02x want=0x%02x\n", c.name, dxil, slot, i, bytes[slot * kSlotBytes + i],
                         m.slots[slot][i]);
            ok = false;
            break;
          }
        std::uint32_t filled = 0;
        std::memcpy(&filled, bytes + kFilledAt + 16 * slot, 4);
        if (filled != m.filled[slot]) {
          std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_GS_STREAM_FILLED case=%s dxil=%d slot=%u got=%u want=%u\n",
                       c.name, dxil, slot, filled, m.filled[slot]);
          ok = false;
        }
      }
      D3D12_QUERY_DATA_SO_STATISTICS stats[4];
      std::memcpy(stats, bytes + kBytes, sizeof(stats));
      for (UINT s = 0; s < 4; ++s)
        if (stats[s].NumPrimitivesWritten != m.written[s] || stats[s].PrimitivesStorageNeeded != m.needed[s]) {
          std::fprintf(stderr, "OWNED_D3D12_STREAM_OUTPUT_GS_STREAM_QUERY case=%s dxil=%d stream=%u got=(%llu,%llu) "
                       "want=(%llu,%llu)\n", c.name, dxil, s, (unsigned long long)stats[s].NumPrimitivesWritten,
                       (unsigned long long)stats[s].PrimitivesStorageNeeded, (unsigned long long)m.written[s],
                       (unsigned long long)m.needed[s]);
          ok = false;
        }
      const D3D12_RANGE none{0, 0};
      readback->Unmap(0, &none);
      if (!ok)
        fail(c.name);
      ++g_cases;
      std::printf("OWNED_D3D12_STREAM_OUTPUT_GS_STREAM_PASS case=%s shader=%s written=%llu,%llu,%llu,%llu "
                  "needed=%llu,%llu,%llu,%llu filled=%u,%u,%u,%u\n", c.name, dxil ? "dxil" : "dxbc",
                  (unsigned long long)m.written[0], (unsigned long long)m.written[1], (unsigned long long)m.written[2],
                  (unsigned long long)m.written[3], (unsigned long long)m.needed[0], (unsigned long long)m.needed[1],
                  (unsigned long long)m.needed[2], (unsigned long long)m.needed[3], m.filled[0], m.filled[1],
                  m.filled[2], m.filled[3]);
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    fail("usage: owned-d3d12-stream-output.exe stream-output-packed-vs.dxil", E_INVALIDARG);
  requireOwnedBackend();
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  Fixture f;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&f.device)), "device");
  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
  ComPtr<ID3DBlob> rootBlob, rootErrors;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors),
        "serialize-root");
  check(f.device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&f.root)), "root");
  f.vs = compile("vsMain", "vs_5_0");
  f.ps = compile("psMain", "ps_5_0");

  ComPtr<ID3D12PipelineState> soPso;
  runPipelineCases(f, soPso);
  runTargetCases(f, soPso.Get());
  runQueryCases(f);
  runCaptureCases(f.device.Get(), readFile(argv[1]));
  std::printf("OWNED_D3D12_STREAM_OUTPUT_VALIDATION_PASS cases=%d\n", g_cases);
  std::puts("OWNED_D3D12_STREAM_OUTPUT_PASS validation=1 capture=1");
  return 0;
}
