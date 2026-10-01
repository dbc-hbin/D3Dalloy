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
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  const auto rejectPso = [&](const char *name) {
    void *output = reinterpret_cast<void *>(uintptr_t(1));
    expect(device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), &output),
           E_INVALIDARG, name);
    if (output)
      fail("invalid PSO retained output");
  };
  input.InputSlot = 32;
  rejectPso("input slot 32");
  input.InputSlot = UINT_MAX;
  rejectPso("input slot UINT_MAX");
  input.InputSlot = 0;
  desc.InputLayout.NumElements = 33;
  rejectPso("input element count");
  desc.InputLayout.NumElements = 1;
  desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(2);
  rejectPso("graphics PSO flags");
  desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  input.InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(99);
  rejectPso("input classification");
  input.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  input.AlignedByteOffset = UINT_MAX - 2;
  rejectPso("input append offset overflow");
  input.AlignedByteOffset = 0;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0x10;
  rejectPso("render target write mask");
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
  desc.BlendState.RenderTarget[0].LogicOp = static_cast<D3D12_LOGIC_OP>(16);
  rejectPso("logic op out of range");
  desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
  desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
  desc.BlendState.RenderTarget[0].SrcBlend = static_cast<D3D12_BLEND>(UINT_MAX);
  rejectPso("blend factor");
  desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
  desc.BlendState.RenderTarget[0].BlendOp = static_cast<D3D12_BLEND_OP>(UINT_MAX);
  rejectPso("blend operation");
  desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
  desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  desc.RasterizerState.FillMode = static_cast<D3D12_FILL_MODE>(UINT_MAX);
  rejectPso("fill mode");
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = static_cast<D3D12_CULL_MODE>(UINT_MAX);
  rejectPso("cull mode");
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.DepthStencilState.DepthEnable = TRUE;
  desc.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(UINT_MAX);
  rejectPso("depth compare");
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.StencilEnable = TRUE;
  desc.DepthStencilState.FrontFace.StencilFailOp = static_cast<D3D12_STENCIL_OP>(UINT_MAX);
  rejectPso("stencil operation");
  desc.DepthStencilState.StencilEnable = FALSE;
  desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
  desc.RasterizerState.ForcedSampleCount = 3;
  rejectPso("invalid forced sample count");
  desc.RasterizerState.ForcedSampleCount = 4;
  desc.DepthStencilState.DepthEnable = TRUE;
  rejectPso("forced samples with depth test");
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  rejectPso("forced samples with depth attachment format");
  desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
  desc.RasterizerState.ForcedSampleCount = 0;
  ComPtr<ID3D12PipelineState> slot16, slot31;
  input.InputSlot = 16;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&slot16)), "input slot 16 PSO");
  input.InputSlot = 31;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&slot31)), "input slot 31 PSO");
  input.InputSlot = 0;
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
  D3D12_RECT bottom_left{0, 8, 8, 16};
  parent.list->RSSetScissorRects(1, &bottom_left);
  parent.list->SetGraphicsRoot32BitConstants(0, 2, rg, 0);
  const D3D12_VERTEX_BUFFER_VIEW input_view{vb->GetGPUVirtualAddress(), sizeof(vertices), 2 * sizeof(float)};
  parent.list->SetPipelineState(slot16.Get());
  parent.list->IASetVertexBuffers(16, 1, &input_view);
  parent.list->DrawIndexedInstanced(3, 1, 0, 0, 0);
  D3D12_RECT bottom_right{8, 8, 16, 16};
  parent.list->RSSetScissorRects(1, &bottom_right);
  parent.list->SetPipelineState(slot31.Get());
  parent.list->IASetVertexBuffers(31, 1, &input_view);
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
  passed("pso_input_slots_16_31_and_invalid_state");
}

void testAttachmentlessSampleSwitch(ID3D12Device *device) {
  const char *source = R"(
    RWStructuredBuffer<uint> output : register(u0);
    float4 vs(uint id : SV_VertexID) : SV_Position {
      return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
    }
    void ps(float4 position : SV_Position) {
      InterlockedAdd(output[uint(position.y) * 8 + uint(position.x)], 1);
    }
  )";
  auto vs = shader(source, "vs", "vs_5_0");
  auto ps = shader(source, "ps", "ps_5_0");
  D3D12_ROOT_PARAMETER uav{};
  uav.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC root_desc{1, &uav, 0, nullptr,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  auto signature = root(device, root_desc);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = signature.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.SampleMask = UINT_MAX;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  std::array<ComPtr<ID3D12PipelineState>, 3> pipelines;
  for (UINT i = 0; i < pipelines.size(); ++i) {
    desc.SampleDesc.Count = i == 1 ? 4 : 1;
    desc.RasterizerState.ForcedSampleCount = i == 2 ? 4 : 0;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[i])),
          "attachmentless sample-switch PSO");
  }
  std::array<UINT, 8 * 8> zeros{};
  auto init = buffer(device, sizeof(zeros), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(init.Get(), zeros.data(), sizeof(zeros));
  auto output = buffer(device, sizeof(zeros), D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto result = buffer(device, sizeof(zeros), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  Recording recording(device, D3D12_COMMAND_LIST_TYPE_DIRECT, pipelines[0].Get());
  recording.list->CopyBufferRegion(output.Get(), 0, init.Get(), 0, sizeof(zeros));
  transition(recording.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  recording.list->SetGraphicsRootSignature(signature.Get());
  recording.list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  recording.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
  const D3D12_RECT scissor{0, 0, 8, 8};
  recording.list->RSSetViewports(1, &viewport);
  recording.list->RSSetScissorRects(1, &scissor);
  for (UINT index : {0u, 1u, 0u, 2u}) {
    recording.list->SetPipelineState(pipelines[index].Get());
    recording.list->DrawInstanced(3, 1, 0, 0);
  }
  transition(recording.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  recording.list->CopyBufferRegion(result.Get(), 0, output.Get(), 0, sizeof(zeros));
  recording.close();
  auto q = queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  submit(q.Get(), recording.list.Get());
  drain(device, q.Get());
  const auto bytes = readback(result.Get(), sizeof(zeros));
  for (UINT i = 0; i < zeros.size(); ++i) {
    UINT value;
    std::memcpy(&value, bytes.data() + i * sizeof(value), sizeof(value));
    if (value != 4)
      fail("attachmentless sample-switch pixel invocation count");
  }
  passed("attachmentless_sample1_sample4_forced4_switch");
}

void testForcedSamples(ID3D12Device *device) {
  const char *source = R"(
    RWStructuredBuffer<uint> output : register(u1);
    float4 vs(uint id : SV_VertexID) : SV_Position {
      return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
    }
    float4 vsEdge(uint id : SV_VertexID) : SV_Position {
      float2 p = id == 0 ? float2(-0.9, -0.1) :
                 id == 1 ? float2(-0.56, -0.1) : float2(-0.9, -0.44);
      return float4(p, 0, 1);
    }
    struct LayerVertex { float4 position : SV_Position; uint layer : SV_RenderTargetArrayIndex; };
    LayerVertex vsLayer(uint id : SV_VertexID) {
      LayerVertex result;
      result.position = vsEdge(id);
      result.layer = 1;
      return result;
    }
    void ps(float4 position : SV_Position) {
      InterlockedAdd(output[uint(position.y) * 16 + uint(position.x)], 1);
    }
    float4 psColor(float4 position : SV_Position) : SV_Target {
      InterlockedAdd(output[uint(position.y) * 16 + uint(position.x)], 1);
      return float4(0.25, 0.5, 1, 1);
    }
    float4 psPlainColor() : SV_Target { return float4(0.25, 0.5, 1, 1); }
    struct MaskedColor { float4 color : SV_Target; uint coverage : SV_Coverage; };
    MaskedColor psCoverage(float4 position : SV_Position) {
      InterlockedAdd(output[uint(position.y) * 16 + uint(position.x)], 1);
      MaskedColor result;
      result.color = float4(1, 0.25, 0.5, 1);
      result.coverage = 1;
      return result;
    }
    MaskedColor psCoverageZero(float4 position : SV_Position) {
      MaskedColor result;
      result.color = float4(1, 0, 0, 1);
      result.coverage = 0;
      return result;
    }
    float4 psDiscard() : SV_Target { clip(-1); return float4(1, 0, 0, 1); }
    float psDepth() : SV_Depth { return 0.5; }
  )";
  auto vs = shader(source, "vs", "vs_5_0");
  auto vs_edge = shader(source, "vsEdge", "vs_5_0");
  auto vs_layer = shader(source, "vsLayer", "vs_5_0");
  auto ps = shader(source, "ps", "ps_5_0");
  auto ps_color = shader(source, "psColor", "ps_5_0");
  auto ps_plain_color = shader(source, "psPlainColor", "ps_5_0");
  auto ps_coverage = shader(source, "psCoverage", "ps_5_0");
  auto ps_coverage_zero = shader(source, "psCoverageZero", "ps_5_0");
  auto ps_discard = shader(source, "psDiscard", "ps_5_0");
  auto ps_depth = shader(source, "psDepth", "ps_5_0");
  D3D12_ROOT_PARAMETER uav{};
  uav.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  uav.Descriptor.ShaderRegister = 1;
  D3D12_ROOT_SIGNATURE_DESC root_desc{1, &uav, 0, nullptr,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  auto signature = root(device, root_desc);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = signature.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.ForcedSampleCount = 4;
  desc.SampleMask = UINT_MAX;
  desc.SampleDesc.Count = 1;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "forced sample4 no RTV PSO");
  desc.PS = {ps_depth->GetBufferPointer(), ps_depth->GetBufferSize()};
  void *bad = reinterpret_cast<void *>(uintptr_t(1));
  expect(device->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), &bad),
         E_INVALIDARG, "forced samples with PS depth output");
  if (bad) fail("failed depth PSO output not null");

  std::array<UINT, 16 * 16> zeros{};
  auto init = buffer(device, sizeof(zeros), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(init.Get(), zeros.data(), sizeof(zeros));
  auto output = buffer(device, sizeof(zeros), D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto result = buffer(device, 16 * 16 * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  Recording recording(device, D3D12_COMMAND_LIST_TYPE_DIRECT, pso.Get());
  recording.list->CopyBufferRegion(output.Get(), 0, init.Get(), 0, sizeof(zeros));
  transition(recording.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  recording.list->SetGraphicsRootSignature(signature.Get());
  recording.list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  recording.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport{0, 0, 16, 16, 0, 1};
  D3D12_RECT scissor{0, 0, 16, 16};
  recording.list->RSSetViewports(1, &viewport);
  recording.list->RSSetScissorRects(1, &scissor);
  recording.list->DrawInstanced(3, 1, 0, 0);
  transition(recording.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  recording.list->CopyBufferRegion(result.Get(), 0, output.Get(), 0, 16 * 16 * 4);
  recording.close();
  auto q = queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  submit(q.Get(), recording.list.Get());
  drain(device, q.Get());
  auto bytes = readback(result.Get(), 16 * 16 * 4);
  for (UINT i = 0; i < 16 * 16; ++i) {
    UINT value = 0;
    std::memcpy(&value, bytes.data() + i * 4, 4);
    if (value != 1) {
      std::fprintf(stderr, "forced sample4 pixel[%u]=%u\n", i, value);
      fail("forced sample4 no RTV pixel invocation");
    }
  }
  passed("forced_sample4_attachmentless_uav_and_depth_restrictions");

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
                                        IID_PPV_ARGS(&texture)), "forced single-sample RTV");
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "forced RTV heap");
  const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 image_bytes = 0;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
  auto image_result = buffer(device, image_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = texture_desc.Format;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  struct ForcedCase { UINT count; bool masked; std::array<UINT, 4> color; };
  const ForcedCase cases[] = {
      {1, false, {64, 128, 255, 255}},
      {4, false, {64, 128, 255, 255}},
      {8, false, {64, 128, 255, 255}},
      {4, true, {255, 64, 128, 255}},
  };
  D3D12_QUERY_HEAP_DESC query_desc{};
  query_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  query_desc.Count = 2;
  query_desc.NodeMask = 1;
  ComPtr<ID3D12QueryHeap> visibility;
  check(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&visibility)), "forced visibility heap");
  auto visible_result = buffer(device, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  for (UINT case_index = 0; case_index < std::size(cases); ++case_index) {
    const auto &current = cases[case_index];
    desc.RasterizerState.ForcedSampleCount = current.count;
    desc.SampleMask = current.masked ? 1u : UINT_MAX;
    auto fragment = current.masked ? ps_coverage : ps_color;
    desc.PS = {fragment->GetBufferPointer(), fragment->GetBufferSize()};
    ComPtr<ID3D12PipelineState> color_pso;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&color_pso)), "forced single-sample RTV PSO");
    Recording color(device, D3D12_COMMAND_LIST_TYPE_DIRECT, color_pso.Get());
    if (case_index)
      transition(color.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(color.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    color.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float clear[] = {0, 0, 0, 0};
    color.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    color.list->SetGraphicsRootSignature(signature.Get());
    color.list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    color.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    color.list->RSSetViewports(1, &viewport);
    color.list->RSSetScissorRects(1, &scissor);
    color.list->BeginQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    color.list->BeginQuery(visibility.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    color.list->DrawInstanced(3, 1, 0, 0);
    color.list->EndQuery(visibility.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    color.list->EndQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    color.list->ResolveQueryData(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                 0, 1, visible_result.Get(), 0);
    color.list->ResolveQueryData(visibility.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION,
                                 1, 1, visible_result.Get(), 8);
    transition(color.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    color.list->CopyBufferRegion(result.Get(), 0, output.Get(), 0, sizeof(zeros));
    transition(color.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = image_result.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    color.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    color.close();
    submit(q.Get(), color.list.Get());
    drain(device, q.Get());
    auto counts = readback(result.Get(), sizeof(zeros));
    auto pixels = readback(image_result.Get(), image_bytes);
    auto visible = readback(visible_result.Get(), 16);
    UINT64 samples = 0, binary = 0;
    std::memcpy(&samples, visible.data(), sizeof(samples));
    std::memcpy(&binary, visible.data() + 8, sizeof(binary));
    if (samples != UINT64(16 * 16) * current.count || binary != 1) {
      std::fprintf(stderr, "forced RTV case=%u samples=%llu binary=%llu expected=%u\n",
                   case_index, static_cast<unsigned long long>(samples),
                   static_cast<unsigned long long>(binary), 16 * 16 * current.count);
      fail("forced raster numeric/binary visibility counts virtual samples");
    }
    for (UINT i = 0; i < 16 * 16; ++i) {
      UINT value = 0;
      std::memcpy(&value, counts.data() + i * 4, 4);
      if (value != case_index + 2) {
        std::fprintf(stderr, "forced RTV case=%u pixel[%u]=%u\n", case_index, i, value);
        fail("forced RTV pixel invocation");
      }
      const auto *pixel = pixels.data() + (i / 16) * footprint.Footprint.RowPitch + (i % 16) * 4;
      for (UINT component = 0; component < 4; ++component)
        if (abs(int(pixel[component]) - int(current.color[component])) > 1)
          fail("forced RTV color and coverage");
    }
  }
  passed("forced_samples_1_4_8_single_sample_rtv_pixel_frequency_and_coverage");
  desc.VS = {vs_edge->GetBufferPointer(), vs_edge->GetBufferSize()};
  desc.PS = {ps_coverage->GetBufferPointer(), ps_coverage->GetBufferSize()};
  desc.SampleMask = UINT_MAX;
  ComPtr<ID3D12PipelineState> edge_pso;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&edge_pso)),
        "forced4 edge-only sample PSO");
  Recording edge(device, D3D12_COMMAND_LIST_TYPE_DIRECT, edge_pso.Get());
  transition(edge.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_RENDER_TARGET);
  transition(edge.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  edge.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const float clear[] = {0, 0, 0, 0};
  edge.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  edge.list->SetGraphicsRootSignature(signature.Get());
  edge.list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  edge.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT edge_viewport{0, 0, 1, 1, 0, 1};
  D3D12_RECT edge_scissor{0, 0, 1, 1};
  edge.list->RSSetViewports(1, &edge_viewport);
  edge.list->RSSetScissorRects(1, &edge_scissor);
  edge.list->BeginQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  edge.list->DrawInstanced(3, 1, 0, 0);
  edge.list->EndQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  edge.list->ResolveQueryData(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                               0, 1, visible_result.Get(), 0);
  transition(edge.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  edge.list->CopyBufferRegion(result.Get(), 0, output.Get(), 0, sizeof(UINT));
  transition(edge.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION edge_src{}, edge_dst{};
  edge_src.pResource = texture.Get();
  edge_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  edge_dst.pResource = image_result.Get();
  edge_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  edge_dst.PlacedFootprint = footprint;
  edge.list->CopyTextureRegion(&edge_dst, 0, 0, 0, &edge_src, nullptr);
  edge.close();
  submit(q.Get(), edge.list.Get());
  drain(device, q.Get());
  auto count_bytes = readback(result.Get(), sizeof(UINT));
  UINT count = 0;
  std::memcpy(&count, count_bytes.data(), sizeof(count));
  if (count != std::size(cases) + 2)
    fail("edge-only forced4 PS executes once");
  auto edge_visible = readback(visible_result.Get(), sizeof(UINT64));
  UINT64 edge_samples = 0;
  std::memcpy(&edge_samples, edge_visible.data(), sizeof(edge_samples));
  if (edge_samples != 1) {
    std::fprintf(stderr, "forced edge samples=%llu expected=1\n",
                 static_cast<unsigned long long>(edge_samples));
    fail("edge-only forced4 query counts one covered virtual sample");
  }
  auto edge_pixels = readback(image_result.Get(), image_bytes);
  const unsigned char expected_color[] = {255, 64, 128, 255};
  for (UINT component = 0; component < 4; ++component) {
    if (abs(int(edge_pixels[component]) - int(expected_color[component])) > 1)
      fail("edge-only forced4 sample writes single-sample RTV");
    if (edge_pixels[4 + component]) fail("forced replay respects one-pixel scissor");
  }
  passed("forced4_edge_only_coverage_bit0_output_fullscreen_replay_and_scissor");
  // Output masks operate on the 1x RTV, not on virtual raster samples.
  // A PS can execute while SV_Coverage suppresses all OM writes; the
  // functional spec recommends zero visible samples in this case.
  for (UINT mask_case : {0u, 1u, 2u}) {
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.RasterizerState.ForcedSampleCount = 8;
    desc.SampleMask = mask_case ? UINT_MAX : 2u;
    auto fragment = mask_case == 1 ? ps_coverage_zero :
                    mask_case == 2 ? ps_discard : ps_color;
    desc.PS = {fragment->GetBufferPointer(), fragment->GetBufferSize()};
    ComPtr<ID3D12PipelineState> masked_pso;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&masked_pso)),
          "forced output-mask PSO");
    Recording masked(device, D3D12_COMMAND_LIST_TYPE_DIRECT, masked_pso.Get());
    transition(masked.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    masked.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    masked.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    masked.list->SetGraphicsRootSignature(signature.Get());
    masked.list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    masked.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    masked.list->RSSetViewports(1, &viewport);
    masked.list->RSSetScissorRects(1, &scissor);
    masked.list->BeginQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    masked.list->BeginQuery(visibility.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    masked.list->DrawInstanced(3, 1, 0, 0);
    masked.list->EndQuery(visibility.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    masked.list->EndQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    masked.list->ResolveQueryData(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                  0, 1, visible_result.Get(), 0);
    masked.list->ResolveQueryData(visibility.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION,
                                  1, 1, visible_result.Get(), 8);
    transition(masked.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION masked_src{}, masked_dst{};
    masked_src.pResource = texture.Get();
    masked_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    masked_dst.pResource = image_result.Get();
    masked_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    masked_dst.PlacedFootprint = footprint;
    masked.list->CopyTextureRegion(&masked_dst, 0, 0, 0, &masked_src, nullptr);
    masked.close();
    submit(q.Get(), masked.list.Get());
    drain(device, q.Get());
    auto visible = readback(visible_result.Get(), 16);
    UINT64 samples = ~UINT64(0), binary = ~UINT64(0);
    std::memcpy(&samples, visible.data(), sizeof(samples));
    std::memcpy(&binary, visible.data() + 8, sizeof(binary));
    if (samples || binary) fail("forced output mask suppresses numeric/binary visibility");
    auto pixels = readback(image_result.Get(), image_bytes);
    for (UINT i = 0; i < 16 * 16; ++i) {
      const auto *pixel = pixels.data() + (i / 16) * footprint.Footprint.RowPitch + (i % 16) * 4;
      if (pixel[0] || pixel[1] || pixel[2] || pixel[3])
        fail("forced output mask preserves clear color");
    }
  }
  passed("forced8_output_sample_mask_sv_coverage_discard_zero_visibility");
  D3D12_INDIRECT_ARGUMENT_DESC draw_arg{};
  draw_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  D3D12_COMMAND_SIGNATURE_DESC draw_signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, &draw_arg, 0};
  ComPtr<ID3D12CommandSignature> draw_signature;
  check(device->CreateCommandSignature(&draw_signature_desc, nullptr, IID_PPV_ARGS(&draw_signature)),
        "forced indirect draw signature");
  const D3D12_DRAW_ARGUMENTS draw_args{3, 1, 0, 0};
  auto argument_buffer = buffer(device, sizeof(draw_args), D3D12_HEAP_TYPE_UPLOAD,
                                D3D12_RESOURCE_STATE_GENERIC_READ);
  upload(argument_buffer.Get(), &draw_args, sizeof(draw_args));
  auto predicate_buffer = buffer(device, sizeof(UINT64), D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ);
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps_plain_color->GetBufferPointer(), ps_plain_color->GetBufferSize()};
  desc.RasterizerState.ForcedSampleCount = 8;
  desc.SampleMask = UINT_MAX;
  ComPtr<ID3D12PipelineState> indirect_pso;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&indirect_pso)),
        "forced indirect visibility PSO");
  for (UINT64 predicate : {UINT64(0), UINT64(1)}) {
    upload(predicate_buffer.Get(), &predicate, sizeof(predicate));
    Recording indirect_record(device, D3D12_COMMAND_LIST_TYPE_DIRECT, indirect_pso.Get());
    transition(indirect_record.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    indirect_record.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    indirect_record.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    indirect_record.list->SetGraphicsRootSignature(signature.Get());
    indirect_record.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    indirect_record.list->RSSetViewports(1, &viewport);
    indirect_record.list->RSSetScissorRects(1, &scissor);
    indirect_record.list->SetPredication(predicate_buffer.Get(), 0,
                                         D3D12_PREDICATION_OP_EQUAL_ZERO);
    indirect_record.list->BeginQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    indirect_record.list->ExecuteIndirect(draw_signature.Get(), 1, argument_buffer.Get(), 0, nullptr, 0);
    indirect_record.list->EndQuery(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    indirect_record.list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    indirect_record.list->ResolveQueryData(visibility.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                           0, 1, visible_result.Get(), 0);
    transition(indirect_record.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION indirect_src{}, indirect_dst{};
    indirect_src.pResource = texture.Get();
    indirect_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    indirect_dst.pResource = image_result.Get();
    indirect_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    indirect_dst.PlacedFootprint = footprint;
    indirect_record.list->CopyTextureRegion(&indirect_dst, 0, 0, 0, &indirect_src, nullptr);
    indirect_record.close();
    submit(q.Get(), indirect_record.list.Get());
    drain(device, q.Get());
    auto visible = readback(visible_result.Get(), sizeof(UINT64));
    UINT64 samples = 0;
    std::memcpy(&samples, visible.data(), sizeof(samples));
    // D3D12_PREDICATION_OP_EQUAL_ZERO skips commands when the predicate is zero.
    if (samples != (predicate ? UINT64(16 * 16 * 8) : 0)) {
      std::fprintf(stderr, "forced indirect predicate=%llu samples=%llu expected=%llu\n",
                   static_cast<unsigned long long>(predicate),
                   static_cast<unsigned long long>(samples),
                   static_cast<unsigned long long>(predicate ? UINT64(16 * 16 * 8) : 0));
      fail("forced indirect predicated virtual visibility");
    }
    auto pixels = readback(image_result.Get(), image_bytes);
    for (UINT i = 0; i < 16 * 16; ++i) {
      const auto *pixel = pixels.data() + (i / 16) * footprint.Footprint.RowPitch + (i % 16) * 4;
      const unsigned char expected[] = {64, 128, 255, 255};
      for (UINT channel = 0; channel < 4; ++channel)
        if (abs(int(pixel[channel]) - (predicate ? int(expected[channel]) : 0)) > 1)
          fail("forced indirect predication color");
    }
  }
  passed("forced8_indirect_predication_virtual_visibility");
  auto array_desc = texture_desc;
  array_desc.DepthOrArraySize = 2;
  ComPtr<ID3D12Resource> array_texture;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &array_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&array_texture)), "forced array RTV");
  D3D12_RENDER_TARGET_VIEW_DESC array_rtv{};
  array_rtv.Format = array_desc.Format;
  array_rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
  array_rtv.Texture2DArray.ArraySize = 2;
  device->CreateRenderTargetView(array_texture.Get(), &array_rtv, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT array_footprints[2]{};
  UINT64 array_bytes = 0;
  device->GetCopyableFootprints(&array_desc, 0, 2, 0, array_footprints,
                                nullptr, nullptr, &array_bytes);
  auto array_result = buffer(device, array_bytes, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_STATE_COPY_DEST);
  desc.VS = {vs_layer->GetBufferPointer(), vs_layer->GetBufferSize()};
  desc.PS = {ps_color->GetBufferPointer(), ps_color->GetBufferSize()};
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.ForcedSampleCount = 4;
  ComPtr<ID3D12PipelineState> layer_pso;
  check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&layer_pso)),
        "forced4 layered edge PSO");
  Recording layer(device, D3D12_COMMAND_LIST_TYPE_DIRECT, layer_pso.Get());
  transition(layer.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  layer.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  layer.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
  layer.list->SetGraphicsRootSignature(signature.Get());
  layer.list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
  layer.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  layer.list->RSSetViewports(1, &edge_viewport);
  layer.list->RSSetScissorRects(1, &edge_scissor);
  layer.list->DrawInstanced(3, 1, 0, 0);
  transition(layer.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  layer.list->CopyBufferRegion(result.Get(), 0, output.Get(), 0, sizeof(UINT));
  transition(layer.list.Get(), array_texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  for (UINT slice = 0; slice < 2; ++slice) {
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = array_texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = slice;
    dst.pResource = array_result.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = array_footprints[slice];
    layer.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  layer.close();
  submit(q.Get(), layer.list.Get());
  drain(device, q.Get());
  count_bytes = readback(result.Get(), sizeof(UINT));
  std::memcpy(&count, count_bytes.data(), sizeof(count));
  if (count != std::size(cases) + 3) fail("layered forced PS executes once");
  auto array_pixels = readback(array_result.Get(), array_bytes);
  const unsigned char expected_layer_color[] = {64, 128, 255, 255};
  for (UINT component = 0; component < 4; ++component) {
    if (array_pixels[array_footprints[0].Offset + component])
      fail("forced edge array layer zero unchanged");
    if (abs(int(array_pixels[array_footprints[1].Offset + component]) -
            int(expected_layer_color[component])) > 1)
      fail("forced edge writes captured array layer one");
  }
  passed("forced4_edge_array_layer_fullscreen_replay");
}

void testRGB32Blend(ID3D12Device *device) {
  const char *source = R"(
    float4 vs(uint id : SV_VertexID) : SV_Position {
      return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
    }
    float4 ps() : SV_Target { return float4(0.2, 0.4, 0.6, 0.25); }
  )";
  auto vs = shader(source, "vs", "vs_5_0");
  auto ps = shader(source, "ps", "ps_5_0");
  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  auto signature = root(device, root_desc);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = signature.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.SampleMask = UINT_MAX;
  desc.SampleDesc.Count = 1;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32_FLOAT;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  auto &blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = TRUE;
  blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.DestBlendAlpha = D3D12_BLEND_ZERO;
  D3D12_HEAP_PROPERTIES props{};
  props.Type = D3D12_HEAP_TYPE_DEFAULT;
  props.CreationNodeMask = props.VisibleNodeMask = 1;
  D3D12_RESOURCE_DESC texture_desc{};
  texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture_desc.Width = texture_desc.Height = 1;
  texture_desc.DepthOrArraySize = texture_desc.MipLevels = texture_desc.SampleDesc.Count = 1;
  texture_desc.Format = DXGI_FORMAT_R32G32B32_FLOAT;
  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&texture)), "RGB32 blend texture");
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "RGB32 blend RTV heap");
  auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 image_bytes = 0;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &image_bytes);
  auto pixels = buffer(device, image_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto q = queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
  struct BlendCase { D3D12_BLEND src, dst; std::array<float, 3> expected; };
  const BlendCase cases[] = {
      {D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_ZERO, {0.2f, 0.4f, 0.6f}},
      {D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_ONE, {0.3f, 0.5f, 0.7f}},
      {D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_ONE, {0.3f, 0.5f, 0.7f}},
      {D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_ONE, {0.35f, 0.6f, 0.85f}},
      {D3D12_BLEND_ZERO, D3D12_BLEND_DEST_ALPHA, {0.3f, 0.5f, 0.7f}},
      {D3D12_BLEND_ZERO, D3D12_BLEND_INV_DEST_ALPHA, {0, 0, 0}},
  };
  for (UINT i = 0; i < std::size(cases); ++i) {
    blend.SrcBlend = cases[i].src;
    blend.DestBlend = cases[i].dst;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "RGB32 blend PSO");
    Recording draw(device, D3D12_COMMAND_LIST_TYPE_DIRECT, pso.Get());
    if (i)
      transition(draw.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[] = {0.3f, 0.5f, 0.7f, 0.0f};
    draw.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    draw.list->SetGraphicsRootSignature(signature.Get());
    draw.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
    D3D12_RECT scissor{0, 0, 1, 1};
    draw.list->RSSetViewports(1, &viewport);
    draw.list->RSSetScissorRects(1, &scissor);
    draw.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    draw.list->DrawInstanced(3, 1, 0, 0);
    transition(draw.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = pixels.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    draw.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    draw.close();
    submit(q.Get(), draw.list.Get());
    drain(device, q.Get());
    auto bytes = readback(pixels.Get(), image_bytes);
    for (UINT channel = 0; channel < 3; ++channel) {
      float actual = 0;
      std::memcpy(&actual, bytes.data() + channel * sizeof(float), sizeof(float));
      float delta = actual - cases[i].expected[channel];
      if (delta < 0) delta = -delta;
      if (delta > 0.0001f) {
        std::fprintf(stderr, "RGB32 blend case=%u channel=%u actual=%f expected=%f\n",
                     i, channel, actual, cases[i].expected[channel]);
        fail("RGB32 blend logical destination alpha");
      }
    }
  }
  passed("rgb32_blend_implicit_dest_alpha_one_source_alpha_preserved");
}

void testBindingLimits(ID3D12Device *device) {
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  heap_desc.NumDescriptors = 2048;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "2048 sampler heap");
  ++heap_desc.NumDescriptors;
  void *bad = reinterpret_cast<void *>(uintptr_t(1));
  expect(device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), &bad),
         E_INVALIDARG, "2049 sampler heap");
  if (bad) fail("failed sampler heap output not null");
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  ComPtr<ID3D12DescriptorHeap> cpu_heap;
  check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&cpu_heap)), "CPU-only 2049 sampler heap");
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  heap_desc.NumDescriptors = 1000001;
  bad = reinterpret_cast<void *>(uintptr_t(1));
  expect(device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), &bad),
         E_INVALIDARG, "Tier2 view heap exceeds one million descriptors");
  if (bad) fail("failed over-million view heap output not null");

  std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(2033);
  for (UINT i = 0; i < samplers.size(); ++i) {
    samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[i].MipLODBias = -16.0f + float(i) / 100.0f;
    samplers[i].MaxAnisotropy = 1;
    samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[i].ShaderRegister = i;
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc{0, nullptr, 2032, samplers.data(), D3D12_ROOT_SIGNATURE_FLAG_NONE};
  auto live = root(device, desc);
  desc.NumStaticSamplers = 1;
  auto shared_state = root(device, desc);
  desc.NumStaticSamplers = 1;
  desc.pStaticSamplers = &samplers[2032];
  ComPtr<ID3DBlob> blob, errors;
  check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
        "additional static sampler serialize");
  expect(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     __uuidof(ID3D12RootSignature), &bad),
         E_INVALIDARG, "2033rd live unique static sampler reject");
  if (bad) fail("failed root signature output not null");
  live.Reset();
  shared_state.Reset();
  ComPtr<ID3D12RootSignature> after_release;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                    IID_PPV_ARGS(&after_release)), "released static sampler capacity");

  D3D12_FEATURE_DATA_SHADER_CACHE cache{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache)), "shader cache feature");
  if (cache.SupportFlags != (D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO | D3D12_SHADER_CACHE_SUPPORT_LIBRARY))
    fail("shader cache advertised unsupported automatic modes");
  passed("sampler_heap_static_limits_and_cached_pso_support");
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
  pipeline.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(2);
  void *invalid_pipeline = reinterpret_cast<void *>(uintptr_t(1));
  expect(device->CreateComputePipelineState(&pipeline, __uuidof(ID3D12PipelineState), &invalid_pipeline),
         E_INVALIDARG, "compute PSO flags");
  if (invalid_pipeline)
    fail("invalid compute PSO retained output");
  pipeline.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
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
    testAttachmentlessSampleSwitch(device.Get());
    testForcedSamples(device.Get());
    testRGB32Blend(device.Get());
    testBindingLimits(device.Get());
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
