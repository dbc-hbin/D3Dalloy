#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

[[noreturn]] void fail(const char *step, HRESULT actual = E_FAIL, HRESULT expected = S_OK) {
  std::fprintf(stderr, "OWNED_D3D12_BUNDLES_FAIL step=%s actual=%08lx expected=%08lx\n",
               step, static_cast<unsigned long>(actual), static_cast<unsigned long>(expected));
  throw std::runtime_error(step);
}
void expect(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected)
    fail(step, actual, expected);
}
void check(HRESULT actual, const char *step) {
  if (FAILED(actual))
    fail(step, actual);
}
void passed(const char *name) { std::printf("OWNED_D3D12_BUNDLES_CASE_PASS %s\n", name); }

struct Recording {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList2> list;
  Recording(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState *pso = nullptr) {
    check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, type, allocator.Get(), pso, IID_PPV_ARGS(&list)), "list");
  }
  void close() { expect(list->Close(), S_OK, "Close(valid)"); }
};

ComPtr<ID3D12CommandQueue> queue(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
  D3D12_COMMAND_QUEUE_DESC desc{};
  desc.Type = type;
  ComPtr<ID3D12CommandQueue> result;
  check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&result)), "queue");
  return result;
}
void submit(ID3D12CommandQueue *q, ID3D12CommandList *list) { q->ExecuteCommandLists(1, &list); }
void drain(ID3D12Device *device, ID3D12CommandQueue *q) {
  ComPtr<ID3D12Fence> done;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "done fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event)
    fail("CreateEvent");
  const HRESULT signal = q->Signal(done.Get(), 1);
  const HRESULT registration = done->SetEventOnCompletion(1, event);
  const DWORD waited = WaitForSingleObject(event, 15000);
  CloseHandle(event);
  check(signal, "queue Signal");
  check(registration, "SetEventOnCompletion");
  if (waited != WAIT_OBJECT_0 || done->GetCompletedValue() < 1)
    fail("bounded GPU completion");
}

ComPtr<ID3D12Resource> buffer(ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE heap,
                             D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES props{};
  props.Type = heap;
  props.CreationNodeMask = props.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                         IID_PPV_ARGS(&result)), "buffer");
  return result;
}
void upload(ID3D12Resource *resource, const void *data, size_t size) {
  void *mapped = nullptr;
  D3D12_RANGE no_read{};
  check(resource->Map(0, &no_read, &mapped), "Map(upload)");
  std::memcpy(mapped, data, size);
  D3D12_RANGE written{0, size};
  resource->Unmap(0, &written);
}
std::vector<unsigned char> readback(ID3D12Resource *resource, size_t size) {
  void *mapped = nullptr;
  D3D12_RANGE range{0, size};
  check(resource->Map(0, &range, &mapped), "Map(readback)");
  std::vector<unsigned char> result(size);
  std::memcpy(result.data(), mapped, size);
  D3D12_RANGE no_write{};
  resource->Unmap(0, &no_write);
  return result;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &b);
}
ComPtr<ID3D12RootSignature> root(ID3D12Device *device, const D3D12_ROOT_SIGNATURE_DESC &desc) {
  ComPtr<ID3DBlob> bytes, errors;
  check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "serialize root");
  ComPtr<ID3D12RootSignature> result;
  check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                    IID_PPV_ARGS(&result)), "root");
  return result;
}
ComPtr<ID3DBlob> shader(const char *source, const char *entry, const char *target) {
  ComPtr<ID3DBlob> bytes, errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), "owned-bundle-test", nullptr, nullptr,
                          entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytes, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    fail("shader", hr);
  }
  return bytes;
}
ComPtr<ID3D12DescriptorHeap> descriptorHeap(ID3D12Device *device) {
  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = 1;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> result;
  check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&result)), "descriptor heap");
  return result;
}

void testGraphics(ID3D12Device *device) {
  const char *source = R"(
    cbuffer Color : register(b0) { uint4 color; };
    float4 vs(float2 pos : POSITION) : SV_Position { return float4(pos, 0, 1); }
    float4 ps() : SV_Target { return float4(color) / 255.0; }
    float4 inverted() : SV_Target { return 1.0 - float4(color) / 255.0; }
  )";
  auto vs = shader(source, "vs", "vs_5_0");
  auto ps = shader(source, "ps", "ps_5_0");
  auto inverted = shader(source, "inverted", "ps_5_0");
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants.Num32BitValues = 4;
  D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter, 0, nullptr,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  auto signature = root(device, root_desc);
  D3D12_INPUT_ELEMENT_DESC input{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = signature.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.InputLayout = {&input, 1};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.SampleMask = UINT_MAX;
  desc.SampleDesc.Count = 1;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  ComPtr<ID3D12PipelineState> solid, inverse, incompatible;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&solid)), "solid PSO");
  desc.PS = {inverted->GetBufferPointer(), inverted->GetBufferSize()};
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&inverse)), "inverse PSO");
  desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&incompatible)), "float PSO");
  {
    Recording bad(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, solid.Get());
    bad.list->SetPipelineState(incompatible.Get());
    expect(bad.list->Close(), E_INVALIDARG, "bundle PSO attachment mismatch");
    expect(bad.list->Reset(bad.allocator.Get(), nullptr), E_INVALIDARG, "failed Close remains poisoned");
  }

  const float vertices[] = {-1, -1, -1, 3, 3, -1};
  const UINT16 indices[] = {0, 1, 2};
  auto vb = buffer(device, sizeof(vertices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto ib = buffer(device, sizeof(indices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(vb.Get(), vertices, sizeof(vertices));
  upload(ib.Get(), indices, sizeof(indices));
  Recording bundle(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, solid.Get());
  D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), sizeof(vertices), 2 * sizeof(float)};
  D3D12_INDEX_BUFFER_VIEW index_view{ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
  UINT blue_alpha[] = {19, 255};
  bundle.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  bundle.list->IASetVertexBuffers(0, 1, &view);
  bundle.list->IASetIndexBuffer(&index_view);
  bundle.list->SetGraphicsRoot32BitConstants(0, 2, blue_alpha, 2);
  bundle.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  bundle.close();
  // These are call-time arrays. Changing them must not change the recording.
  view = {};
  index_view = {};
  blue_alpha[0] = blue_alpha[1] = 0;

  D3D12_HEAP_PROPERTIES props{};
  props.Type = D3D12_HEAP_TYPE_DEFAULT;
  props.CreationNodeMask = props.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC texture_desc{};
  texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture_desc.Width = texture_desc.Height = 16;
  texture_desc.DepthOrArraySize = texture_desc.MipLevels = texture_desc.SampleDesc.Count = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&texture)), "texture");
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)), "RTV heap");
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  auto result = buffer(device, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  Recording parent(device, D3D12_COMMAND_LIST_TYPE_DIRECT, inverse.Get());
  parent.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, 16, 16, 0, 1};
  parent.list->RSSetViewports(1, &viewport);
  parent.list->SetGraphicsRootSignature(signature.Get());
  const float clear[] = {0, 0, 0, 0};
  parent.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  for (UINT i = 0; i < 2; ++i) {
    UINT rg[] = {i ? 180u : 40u, i ? 90u : 70u};
    D3D12_RECT scissor{LONG(i * 8), 0, LONG((i + 1) * 8), 8};
    parent.list->RSSetScissorRects(1, &scissor);
    parent.list->SetPipelineState(inverse.Get());
    parent.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
    parent.list->SetGraphicsRoot32BitConstants(0, 2, rg, 0);
    parent.list->ExecuteBundle(bundle.list.Get());
  }
  UINT rg[] = {90, 160};
  D3D12_RECT bottom{0, 8, 16, 16};
  parent.list->RSSetScissorRects(1, &bottom);
  parent.list->SetGraphicsRoot32BitConstants(0, 2, rg, 0);
  parent.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  expect(bundle.list->Reset(bundle.allocator.Get(), solid.Get()), E_FAIL, "Reset bundle referenced by open parent");
  transition(parent.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = texture.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = result.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  parent.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  parent.close();
  expect(bundle.list->Reset(bundle.allocator.Get(), solid.Get()), E_FAIL, "Reset bundle referenced by closed parent");

  // A rejected Reset must preserve both the executable recording and its
  // pending bundle references; the pixel comparison below tests the recording.
  Recording busy_parent(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  expect(parent.list->Reset(busy_parent.allocator.Get(), nullptr), E_INVALIDARG, "Reset with busy allocator");
  expect(bundle.list->Reset(bundle.allocator.Get(), solid.Get()), E_FAIL, "failed parent Reset preserves bundle reference");
  busy_parent.close();

  auto q = queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  ComPtr<ID3D12Fence> gate;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");
  struct OpenGateOnExit {
    ID3D12Fence *fence;
    ~OpenGateOnExit() { fence->Signal(1); }
  } open_gate{gate.Get()};
  check(q->Wait(gate.Get(), 1), "gate Wait");
  submit(q.Get(), parent.list.Get());
  const HRESULT reset_after_submit = bundle.list->Reset(bundle.allocator.Get(), solid.Get());
  if (SUCCEEDED(reset_after_submit)) {
    bundle.list->SetGraphicsRoot32BitConstant(0, 233, 2);
    bundle.close();
  }
  // Always open the gate before reporting a failure, to keep teardown bounded.
  check(gate->Signal(1), "open gate");
  drain(device, q.Get());
  expect(reset_after_submit, S_OK, "Reset after submit before completion");
  const auto pixels = readback(result.Get(), static_cast<size_t>(total));
  for (UINT y = 0; y < 16; ++y) {
    for (UINT x = 0; x < 16; ++x) {
      const unsigned char expected[] = {
          static_cast<unsigned char>(y >= 8 ? 90 : x < 8 ? 40 : 180),
          static_cast<unsigned char>(y >= 8 ? 160 : x < 8 ? 70 : 90), 19, 255};
      const auto *actual = pixels.data() + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
      if (std::memcmp(actual, expected, 4)) {
        std::fprintf(stderr, "pixel=(%u,%u) actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n", x, y,
                     actual[0], actual[1], actual[2], actual[3], expected[0], expected[1], expected[2], expected[3]);
        fail("graphics bundle readback");
      }
    }
  }
  passed("graphics_inheritance_arrays_reuse_export_and_reset_inflight");
  passed("failed_reset_preserves_recording_and_pending_bundles");
}

void testCompute(ID3D12Device *device) {
  const char *source = R"(
    cbuffer Args : register(b0) { uint offset; uint value; uint salt; uint unused; };
    RWByteAddressBuffer result : register(u0);
    [numthreads(1,1,1)] void cs() { result.Store(offset, value ^ salt); }
  )";
  auto cs = shader(source, "cs", "cs_5_0");
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 4;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  auto signature = root(device, desc);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
  pipeline.pRootSignature = signature.Get();
  pipeline.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso)), "compute PSO");
  auto target = buffer(device, 40, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto result = buffer(device, 40, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  Recording bundle(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, pso.Get());
  bundle.list->SetComputeRoot32BitConstant(0, 0x4ba8, 2);
  bundle.list->Dispatch(1, 1, 1);
  bundle.close();
  Recording parent(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  parent.list->SetComputeRootSignature(signature.Get());
  parent.list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
  for (UINT i = 0; i < 8; ++i) {
    UINT values[] = {i * 4, 0x1000 + i};
    parent.list->SetComputeRoot32BitConstants(0, 2, values, 0);
    parent.list->ExecuteBundle(bundle.list.Get());
  }
  UINT values[] = {32, 0x1008};
  parent.list->SetComputeRoot32BitConstants(0, 2, values, 0);
  parent.list->Dispatch(1, 1, 1);

  D3D12_INDIRECT_ARGUMENT_DESC arg{};
  arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC sig_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &arg, 0};
  ComPtr<ID3D12CommandSignature> indirect;
  check(device->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&indirect)), "dispatch signature");
  const D3D12_DISPATCH_ARGUMENTS dispatch{1, 1, 1};
  auto args = buffer(device, sizeof(dispatch), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(args.Get(), &dispatch, sizeof(dispatch));
  Recording indirect_bundle(device, D3D12_COMMAND_LIST_TYPE_BUNDLE, pso.Get());
  indirect_bundle.list->ExecuteIndirect(indirect.Get(), 1, args.Get(), 0, nullptr, 0);
  indirect_bundle.close();
  values[0] = 36;
  values[1] = 0x1009;
  parent.list->SetComputeRoot32BitConstants(0, 2, values, 0);
  parent.list->ExecuteBundle(indirect_bundle.list.Get());
  transition(parent.list.Get(), target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  parent.list->CopyResource(result.Get(), target.Get());
  parent.close();
  auto q = queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  submit(q.Get(), parent.list.Get());
  drain(device, q.Get());
  auto bytes = readback(result.Get(), 40);
  for (UINT i = 0; i < 10; ++i) {
    UINT word;
    std::memcpy(&word, bytes.data() + i * sizeof(UINT), sizeof(word));
    if (word != ((0x1000 + i) ^ 0x4ba8))
      fail("compute/indirect bundle readback");
  }
  passed("compute_dispatch_indirect_and_inherited_root_uav");
}

void testImmediate(ID3D12Device *device) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options)), "OPTIONS3");
  for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                    D3D12_COMMAND_LIST_TYPE_COPY, D3D12_COMMAND_LIST_TYPE_BUNDLE}) {
    if (!(options.WriteBufferImmediateSupportFlags & (1u << type)))
      fail("WriteBufferImmediate capability");
    auto result = buffer(device, 32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    const UINT initial[8] = {0x11111111, 0x11111111, 0x11111111, 0x11111111,
                              0x11111111, 0x11111111, 0x11111111, 0x11111111};
    upload(result.Get(), initial, sizeof(initial));
    Recording commands(device, type);
    const auto va = result->GetGPUVirtualAddress();
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[] = {{va + 4, 0xa1b2c3d4}, {va + 8, 0x12345678},
                                                     {va + 12, 0xfedcba98}};
    D3D12_WRITEBUFFERIMMEDIATE_MODE modes[] = {D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT,
                                               D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
                                               D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT};
    commands.list->WriteBufferImmediate(3, params, modes);
    params[0] = {va + 16, 0xdeadbeef};
    commands.list->WriteBufferImmediate(1, params, nullptr);
    commands.list->WriteBufferImmediate(0, nullptr, nullptr);
    commands.close();
    std::memset(params, 0, sizeof(params));
    std::memset(modes, 0xff, sizeof(modes));
    Recording parent(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    if (type == D3D12_COMMAND_LIST_TYPE_BUNDLE)
      parent.list->ExecuteBundle(commands.list.Get());
    parent.close();
    auto q = queue(device, type == D3D12_COMMAND_LIST_TYPE_BUNDLE ? D3D12_COMMAND_LIST_TYPE_DIRECT : type);
    submit(q.Get(), type == D3D12_COMMAND_LIST_TYPE_BUNDLE ? parent.list.Get() : commands.list.Get());
    drain(device, q.Get());
    const UINT expected[8] = {0x11111111, 0xa1b2c3d4, 0x12345678, 0xfedcba98,
                               0xdeadbeef, 0x11111111, 0x11111111, 0x11111111};
    const auto bytes = readback(result.Get(), sizeof(expected));
    if (std::memcmp(bytes.data(), expected, sizeof(expected)))
      fail("WriteBufferImmediate readback/guards");
  }
  passed("immediate_all_four_list_types_modes_and_guard_words");
}

void testContracts(ID3D12Device *device, IDXGIAdapter *adapter) {
  struct InvalidCall { const char *name; std::function<void(ID3D12GraphicsCommandList2 *)> call; };
  const InvalidCall invalid[] = {
    {"viewport", [](auto *l) { l->RSSetViewports(0, nullptr); }},
    {"scissor", [](auto *l) { l->RSSetScissorRects(0, nullptr); }},
    {"clear-state", [](auto *l) { l->ClearState(nullptr); }},
    {"copy-buffer", [](auto *l) { l->CopyBufferRegion(nullptr, 0, nullptr, 0, 0); }},
    {"copy-texture", [](auto *l) { l->CopyTextureRegion(nullptr, 0, 0, 0, nullptr, nullptr); }},
    {"copy-resource", [](auto *l) { l->CopyResource(nullptr, nullptr); }},
    {"copy-tiles", [](auto *l) { l->CopyTiles(nullptr, nullptr, nullptr, nullptr, 0, D3D12_TILE_COPY_FLAG_NONE); }},
    {"barrier", [](auto *l) { l->ResourceBarrier(0, nullptr); }},
    {"targets", [](auto *l) { l->OMSetRenderTargets(0, nullptr, FALSE, nullptr); }},
    {"clear-rtv", [](auto *l) { l->ClearRenderTargetView({}, nullptr, 0, nullptr); }},
    {"clear-dsv", [](auto *l) { l->ClearDepthStencilView({}, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr); }},
    {"clear-uav", [](auto *l) { l->ClearUnorderedAccessViewUint({}, {}, nullptr, nullptr, 0, nullptr); }},
    {"discard", [](auto *l) { l->DiscardResource(nullptr, nullptr); }},
    {"begin-query", [](auto *l) { l->BeginQuery(nullptr, D3D12_QUERY_TYPE_OCCLUSION, 0); }},
    {"end-query", [](auto *l) { l->EndQuery(nullptr, D3D12_QUERY_TYPE_TIMESTAMP, 0); }},
    {"resolve-query", [](auto *l) { l->ResolveQueryData(nullptr, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, nullptr, 0); }},
    {"resolve", [](auto *l) { l->ResolveSubresource(nullptr, 0, nullptr, 0, DXGI_FORMAT_R8G8B8A8_UNORM); }},
    {"predication", [](auto *l) { l->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO); }},
    {"stream-output", [](auto *l) { l->SOSetTargets(0, 0, nullptr); }},
    {"nested-bundle", [](auto *l) { l->ExecuteBundle(l); }},
    {"descriptor-table-without-heap", [](auto *l) { l->SetGraphicsRootDescriptorTable(0, {}); }},
    {"vertex-range", [](auto *l) { l->IASetVertexBuffers(UINT_MAX, 2, nullptr); }},
  };
  for (const auto &c : invalid) {
    Recording b(device, D3D12_COMMAND_LIST_TYPE_BUNDLE);
    c.call(b.list.Get());
    expect(b.list->Close(), E_INVALIDARG, c.name);
    expect(b.list->Reset(b.allocator.Get(), nullptr), E_INVALIDARG, "permanent failed Close");
  }
  {
    Recording unclosed(device, D3D12_COMMAND_LIST_TYPE_BUNDLE);
    unclosed.list.Reset();
    expect(unclosed.allocator->Reset(), S_OK, "released unfinished list frees recording slot");
  }
  {
    Recording b(device, D3D12_COMMAND_LIST_TYPE_BUNDLE);
    Recording p(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    p.list->ExecuteBundle(b.list.Get());
    expect(p.list->Close(), E_INVALIDARG, "open bundle rejected");
    b.close();
  }
  {
    Recording b(device, D3D12_COMMAND_LIST_TYPE_BUNDLE);
    b.close();
    expect(b.allocator->Reset(), S_OK, "reset bundle allocator");
    Recording p(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    p.list->ExecuteBundle(b.list.Get());
    expect(p.list->Close(), E_INVALIDARG, "stale bundle allocator rejected");
  }
  {
    Recording b(device, D3D12_COMMAND_LIST_TYPE_BUNDLE);
    b.close();
    Recording a(device, D3D12_COMMAND_LIST_TYPE_DIRECT), z(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    a.list->ExecuteBundle(b.list.Get());
    z.list->ExecuteBundle(b.list.Get());
    a.close(); z.close();
    expect(b.list->Reset(b.allocator.Get(), nullptr), E_FAIL, "two pending parents");
    expect(a.list->Reset(a.allocator.Get(), nullptr), S_OK, "discard first parent");
    expect(b.list->Reset(b.allocator.Get(), nullptr), E_FAIL, "second parent still pending");
    z.list.Reset();
    expect(b.list->Reset(b.allocator.Get(), nullptr), S_OK, "all pending parents discarded");
    a.close(); b.close();
  }
  {
    auto a = descriptorHeap(device), z = descriptorHeap(device);
    ID3D12DescriptorHeap *ap = a.Get(), *zp = z.Get();
    Recording b(device, D3D12_COMMAND_LIST_TYPE_BUNDLE);
    b.list->SetDescriptorHeaps(1, &ap);
    b.close();
    Recording matching(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    matching.list->SetDescriptorHeaps(1, &ap);
    matching.list->ExecuteBundle(b.list.Get());
    matching.close();
    Recording mismatch(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    mismatch.list->SetDescriptorHeaps(1, &zp);
    mismatch.list->ExecuteBundle(b.list.Get());
    expect(mismatch.list->Close(), E_INVALIDARG, "bundle heaps differ from parent");
    Recording changed(device, D3D12_COMMAND_LIST_TYPE_BUNDLE);
    changed.list->SetDescriptorHeaps(1, &ap);
    changed.list->SetDescriptorHeaps(1, &zp);
    expect(changed.list->Close(), E_INVALIDARG, "bundle changes heap");
  }
  {
    ComPtr<ID3D12Device> other;
    check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other)), "other device");
    Recording b(other.Get(), D3D12_COMMAND_LIST_TYPE_BUNDLE);
    b.close();
    Recording p(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    p.list->ExecuteBundle(b.list.Get());
    expect(p.list->Close(), E_INVALIDARG, "cross device bundle");
  }
  {
    auto target = buffer(device, 32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT variant = 0; variant < 4; ++variant) {
      Recording p(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
      D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param{target->GetGPUVirtualAddress(), 42};
      D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      if (variant == 0) param.Dest += 1;
      if (variant == 1) param.Dest = UINT64_MAX - 3;
      if (variant == 2) mode = static_cast<D3D12_WRITEBUFFERIMMEDIATE_MODE>(99);
      p.list->WriteBufferImmediate(1, variant == 3 ? nullptr : &param, &mode);
      expect(p.list->Close(), E_INVALIDARG, "invalid immediate write");
    }
  }
  passed("invalid_commands_heap_identity_allocator_generation_and_pending_lifetimes");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  try {
    using BackendId = const char *(WINAPI *)();
    auto id = reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
    if (!id || !id() || std::strcmp(id(), "owned-graphics-d3d12"))
      fail("owned backend identity");
    std::printf("OWNED_D3D12_BUNDLES_BACKEND id=%s\n", id());
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    testGraphics(device.Get());
    testCompute(device.Get());
    testImmediate(device.Get());
    testContracts(device.Get(), adapter.Get());
    std::puts("OWNED_D3D12_BUNDLES_PASS");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "OWNED_D3D12_BUNDLES_EXCEPTION %s\n", e.what());
    return 1;
  }
}
