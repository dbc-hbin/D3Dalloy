// SPDX-License-Identifier: LGPL-2.1-or-later
// Public AMD driver ABI provenance (packet layouts and SDK call sequence):
// https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK/blob/main/ffx_antilag2_dx12.h
// No private D3Dalloy COM interfaces. OFF/forced never call AMD/NVAPI enable APIs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
MIDL_INTERFACE("44085fbe-e839-40c5-bf38-0ebc5ab4d0a6") IAmdExtAntiLagApi : public IUnknown {
  virtual HRESULT UpdateAntiLagState(void *data) = 0;
};
__CRT_UUID_DECL(IAmdExtAntiLagApi, 0x44085fbe, 0xe839, 0x40c5, 0xbf, 0x38, 0x0e, 0xbc, 0x5a, 0xb4, 0xd0, 0xa6);
namespace {
constexpr UINT kWidth = 640, kHeight = 360, kSlots = 8, kDraws = 64;
constexpr char kPrefix[] = "OWNED_D3D12_LOW_LATENCY";
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx\n", kPrefix, step, static_cast<unsigned long>(hr));
  std::exit(1);
}
void require(bool value, const char *step) { if (!value) fail(step); }
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void expect(HRESULT actual, HRESULT expected, const char *step) { if (actual != expected) fail(step, actual); }
UINT64 qpc() {
  LARGE_INTEGER value{};
  require(QueryPerformanceCounter(&value), "QPC");
  return static_cast<UINT64>(value.QuadPart);
}
struct Options {
  UINT size = sizeof(Options), version = 1, mode = 2;
  const char *control = nullptr;
  UINT control_length = 0, max_fps = 0;
};
struct alignas(8) FramePacket {
  UINT size = sizeof(FramePacket), version = 2, flags = 0;
  UINT64 index = 0, reserved[19]{};
};
static_assert(sizeof(Options) == (sizeof(void *) == 8 ? 32 : 24));
static_assert(sizeof(FramePacket) == 176 && offsetof(FramePacket, index) == 16);
using CreateAntiLag = HRESULT (__cdecl *)(IUnknown *, REFIID, void **);
const GUID kUnknown = {0x37373737, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};

ComPtr<IAmdExtAntiLagApi> antiLag(ID3D12Device *device) {
  // The SDK discovers an already loaded driver module after device creation.
  const auto module = GetModuleHandleW(L"amdxc64.dll");
  require(module != nullptr, "AMD-preloaded-after-device");
  using BackendId = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedAntiLagBackendId"));
  require(backend && !std::strcmp(backend(), "d3dalloy-owned-antilag2"), "AMD-backend");
  auto create = reinterpret_cast<CreateAntiLag>(GetProcAddress(module, "AmdExtD3DCreateInterface"));
  require(create != nullptr, "AMD-factory-export");
  expect(create(device, __uuidof(IAmdExtAntiLagApi), nullptr), E_POINTER, "AMD-null-output");
  void *invalid = device;
  expect(create(nullptr, __uuidof(IAmdExtAntiLagApi), &invalid), E_INVALIDARG, "AMD-null-outer");
  require(!invalid, "AMD-clear-null-outer-output");
  invalid = device;
  expect(create(device, kUnknown, &invalid), E_NOINTERFACE, "AMD-unknown-interface");
  require(!invalid, "AMD-clear-unknown-interface-output");
  ComPtr<IAmdExtAntiLagApi> api;
  check(create(device, __uuidof(IAmdExtAntiLagApi), reinterpret_cast<void **>(api.GetAddressOf())), "AMD-initialize");
  expect(api->QueryInterface(__uuidof(IUnknown), nullptr), E_POINTER, "AMD-QI-null");
  ComPtr<IUnknown> identity;
  check(api.As(&identity), "AMD-IUnknown");
  ComPtr<IAmdExtAntiLagApi> roundtrip;
  check(identity.As(&roundtrip), "AMD-QI-roundtrip");
  require(roundtrip.Get() == api.Get(), "AMD-COM-identity");
  invalid = device;
  expect(api->QueryInterface(kUnknown, &invalid), E_NOINTERFACE, "AMD-QI-unknown");
  require(!invalid, "AMD-QI-clears-output");
  const ULONG retained = api->AddRef();
  require(api->Release() + 1 == retained, "AMD-balanced-reference");
  identity.Reset(); roundtrip.Reset();
  Options options;
  expect(api->UpdateAntiLagState(&options), S_OK, "AMD-init-disabled");
  const auto badOptions = [&](Options bad, HRESULT error, const char *step) {
    expect(api->UpdateAntiLagState(&bad), error, step);
  };
  options.size = 4; badOptions(options, E_INVALIDARG, "AMD-short-header");
  options = {}; options.size -= 1; badOptions(options, E_INVALIDARG, "AMD-bad-v1-size");
  options = {}; options.version = 3; badOptions(options, E_NOTIMPL, "AMD-unknown-version");
  options = {}; options.mode = 0; badOptions(options, E_INVALIDARG, "AMD-invalid-mode");
  options = {}; options.control = "x"; badOptions(options, E_NOTIMPL, "AMD-control-string");
  options = {}; options.control_length = 1; badOptions(options, E_NOTIMPL, "AMD-control-length");
  FramePacket packet;
  const auto badPacket = [&](FramePacket bad, HRESULT error, const char *step) {
    expect(api->UpdateAntiLagState(&bad), error, step);
  };
  packet.flags = 1u << 5; packet.size -= 1; badPacket(packet, E_INVALIDARG, "AMD-bad-v2-size");
  packet = {}; packet.flags = 1u << 4; badPacket(packet, E_NOTIMPL, "AMD-unsupported-input-index");
  packet = {}; packet.flags = 1u << 31; badPacket(packet, E_NOTIMPL, "AMD-unknown-flag");
  packet = {}; packet.flags = 1u << 3; badPacket(packet, E_INVALIDARG, "AMD-interpolated-without-type");
  packet = {}; badPacket(packet, E_INVALIDARG, "AMD-empty-v2");
  packet.flags = 1u << 5; packet.index = 1; badPacket(packet, E_INVALIDARG, "AMD-reserved-index");
  packet.index = 0; packet.reserved[18] = 1; badPacket(packet, E_INVALIDARG, "AMD-reserved-array");
  // Exercise option transitions without admitting a dummy CPU frame before rendering.
  options = {}; options.mode = 1; options.max_fps = 1000;
  expect(api->UpdateAntiLagState(&options), S_OK, "AMD-enable-cap");
  options.max_fps = 0;
  expect(api->UpdateAntiLagState(&options), S_OK, "AMD-disable-cap");
  std::printf("%s_AMD contracts=init,options,identity,lifetime,version,errors,input,fg,end-render backend=d3dalloy-owned-antilag2\n", kPrefix);
  return api;
}
void packet(IAmdExtAntiLagApi *api, UINT flags) {
  FramePacket data; data.flags = flags;
  expect(api->UpdateAntiLagState(&data), S_OK, "AMD-frame-packet");
}
ComPtr<ID3DBlob> compile(const char *source, const char *target) {
  ComPtr<ID3DBlob> bytes, errors;
  const auto hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target,
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytes, &errors);
  if (FAILED(hr) && errors) std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
  check(hr, "D3DCompile"); return bytes;
}
void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &barrier);
}
ComPtr<ID3D12Resource> readback(ID3D12Device *device, UINT64 size) {
  D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> result;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&result)), "readback-resource");
  return result;
}
// Raw QPC ticks preserve measurement precision. Divide CPU differences by cpu_frequency:
// input_end-input_begin = cooperative input wait; submit_end-submit_begin = Execute return;
// submit_end-input_end = CPU recording+submission; present_end-present_begin = Present return.
// GPU query differences use gpu_frequency, not the CPU clock. They exclude display scanout.
struct Measurement { UINT64 input_begin, input_end, submit_begin, submit_end, present_begin, present_end, gpu_begin, gpu_end; };
struct Slot {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Resource> pixels, timestamps;
  UINT64 fence = 0;
  UINT frame = 0;
};
} // namespace

int main(int argc, char **argv) {
  const char *mode = "off";
  UINT frames = 120;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) {
      char *end = nullptr;
      const auto count = std::strtoul(argv[++i], &end, 10);
      require(end && !*end && count >= 2 && count <= 512, "frames-range");
      frames = static_cast<UINT>(count);
    } else fail("arguments");
  }
  require(!std::strcmp(mode, "off") || !std::strcmp(mode, "forced") || !std::strcmp(mode, "antilag2"), "mode");
  const bool cooperative = !std::strcmp(mode, "antilag2");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
  using BackendId = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  require(backend && !std::strcmp(backend(), "owned-graphics-d3d12"), "backend");
  std::printf("%s_BACKEND id=owned-graphics-d3d12\n", kPrefix);
  ComPtr<IAmdExtAntiLagApi> amd;
  if (cooperative) amd = antiLag(device.Get());
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
  LARGE_INTEGER cpu_frequency{};
  require(QueryPerformanceFrequency(&cpu_frequency) && cpu_frequency.QuadPart > 0, "QPC-frequency");
  UINT64 gpu_frequency = 0;
  const HRESULT timing_hr = queue->GetTimestampFrequency(&gpu_frequency);
  const bool timing = SUCCEEDED(timing_hr);
  if (!timing) require(timing_hr == E_NOTIMPL || timing_hr == DXGI_ERROR_UNSUPPORTED, "unexpected-timestamp-error");
  else require(gpu_frequency != 0, "timestamp-frequency");
  ComPtr<ID3D12QueryHeap> queries;
  if (timing) {
    D3D12_QUERY_HEAP_DESC desc{}; desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count = frames * 2;
    check(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&queries)), "timestamp-query-heap");
  }
  WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"OwnedLowLatency";
  require(RegisterClassW(&wc), "window-class");
  HWND window = CreateWindowW(wc.lpszClassName, L"owned low latency", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr, wc.hInstance, nullptr);
  require(window != nullptr, "window"); ShowWindow(window, SW_SHOWNOACTIVATE);
  ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  DXGI_SWAP_CHAIN_DESC1 swap_desc{}; swap_desc.Width = kWidth; swap_desc.Height = kHeight;
  swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; swap_desc.SampleDesc.Count = 1;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; swap_desc.BufferCount = 3;
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> base;
  check(factory->CreateSwapChainForHwnd(queue.Get(), window, &swap_desc, nullptr, nullptr, &base), "swapchain");
  ComPtr<IDXGISwapChain3> swap; check(base.As(&swap), "swapchain3");
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{}; heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap_desc.NumDescriptors = 3;
  ComPtr<ID3D12DescriptorHeap> heap; check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "RTV-heap");
  std::array<ComPtr<ID3D12Resource>, 3> buffers;
  const auto rtv_base = heap->GetCPUDescriptorHandleForHeapStart();
  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (UINT i = 0; i < buffers.size(); ++i) {
    check(swap->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "backbuffer");
    device->CreateRenderTargetView(buffers[i].Get(), nullptr, {rtv_base.ptr + i * stride});
  }
  D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants = {0, 0, 1}; parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_desc{}; root_desc.NumParameters = 1; root_desc.pParameters = &parameter;
  ComPtr<ID3DBlob> serialized, errors;
  check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), "serialize-root");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
  auto vs = compile("float4 main(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}", "vs_5_0");
  auto ps = compile("cbuffer C:register(b0){uint frame;}float4 main(float4 p:SV_Position):SV_Target{"
    "uint x=(uint)p.x,y=(uint)p.y,h=(x>>5)^((y>>5)<<16)^(frame&1);"
    "[loop]for(uint i=0;i<32768;++i){h=(h^(h>>16))*1664525u+1013904223u;}"
    "return float4(h&1,(frame>>1)&1,(x>>3)&1,1);}", "ps_5_0");
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{}; pso_desc.pRootSignature = root.Get();
  pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  auto &blend = pso_desc.BlendState.RenderTarget[0]; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
  blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  pso_desc.SampleMask = ~UINT{0}; pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pso_desc.RasterizerState.DepthClipEnable = TRUE;
  pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = swap_desc.Format; pso_desc.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso; check(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "pipeline");
  // Independent integer oracle, computed before timing rather than stalling
  // every CPU frame with the shader's workload during readback verification.
  std::array<std::vector<unsigned char>, 2> red;
  for (UINT parity = 0; parity < red.size(); ++parity) {
    red[parity].resize(kWidth * kHeight);
    for (UINT y = 0; y < kHeight; y += 32) for (UINT x = 0; x < kWidth; x += 32) {
      UINT hash = (x >> 5) ^ ((y >> 5) << 16) ^ parity;
      for (UINT iteration = 0; iteration < 32768; ++iteration)
        hash = (hash ^ (hash >> 16)) * 1664525u + 1013904223u;
      for (UINT py = y; py < y + 32 && py < kHeight; ++py)
        for (UINT px = x; px < x + 32 && px < kWidth; ++px)
          red[parity][py * kWidth + px] = static_cast<unsigned char>((hash & 1) * 255);
    }
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 pixel_bytes = 0;
  auto texture_desc = buffers[0]->GetDesc();
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &pixel_bytes);
  std::array<Slot, kSlots> slots;
  for (auto &slot : slots) {
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr, IID_PPV_ARGS(&slot.list)), "list");
    check(slot.list->Close(), "initial-close");
    slot.pixels = readback(device.Get(), pixel_bytes);
    if (timing) slot.timestamps = readback(device.Get(), 2 * sizeof(UINT64));
  }
  ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(completed != nullptr, "completion-event");
  std::vector<Measurement> measurements(frames);
  UINT verified = 0;
  auto retire = [&](Slot &slot) {
    if (!slot.fence) return;
    check(fence->SetEventOnCompletion(slot.fence, completed), "register-completion");
    require(WaitForSingleObject(completed, 30000) == WAIT_OBJECT_0, "GPU-completion");
    require(fence->GetCompletedValue() != ~UINT64{0}, "not-device-removed");
    unsigned char *pixels = nullptr; D3D12_RANGE range{0, static_cast<SIZE_T>(pixel_bytes)};
    check(slot.pixels->Map(0, &range, reinterpret_cast<void **>(&pixels)), "pixel-map");
    for (UINT y = 0; y < kHeight; ++y) for (UINT x = 0; x < kWidth; ++x) {
      const auto *pixel = pixels + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
      const unsigned char expected[] = {red[slot.frame & 1][y * kWidth + x],
        static_cast<unsigned char>(((slot.frame >> 1) & 1) * 255), static_cast<unsigned char>(((x >> 3) & 1) * 255), 255};
      if (std::memcmp(pixel, expected, 4)) fail("exact-pixel-oracle");
    }
    D3D12_RANGE written{0, 0}; slot.pixels->Unmap(0, &written);
    if (timing) {
      UINT64 *ticks = nullptr; D3D12_RANGE timestamps{0, 2 * sizeof(UINT64)};
      check(slot.timestamps->Map(0, &timestamps, reinterpret_cast<void **>(&ticks)), "timestamp-map");
      require(ticks[0] && ticks[1] >= ticks[0] && ticks[1] != ~UINT64{0}, "real-GPU-timestamps");
      measurements[slot.frame].gpu_begin = ticks[0]; measurements[slot.frame].gpu_end = ticks[1];
      slot.timestamps->Unmap(0, &written);
    }
    ++verified; slot.fence = 0;
  };
  for (UINT frame = 0; frame < frames; ++frame) {
    auto &slot = slots[frame % kSlots]; retire(slot);
    MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    auto &m = measurements[frame]; m.input_begin = qpc();
    if (amd) expect(amd->UpdateAntiLagState(nullptr), S_OK, "AMD-input-sleep");
    m.input_end = qpc();
    check(slot.allocator->Reset(), "allocator-reset"); check(slot.list->Reset(slot.allocator.Get(), pso.Get()), "list-reset");
    auto *list = slot.list.Get(); const UINT index = swap->GetCurrentBackBufferIndex();
    if (timing) list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2);
    transition(list, buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_base.ptr + index * stride}; list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1}; const D3D12_RECT scissor{0, 0, kWidth, kHeight};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor); list->SetGraphicsRootSignature(root.Get());
    list->SetGraphicsRoot32BitConstant(0, frame, 0); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT draw = 0; draw < kDraws; ++draw) list->DrawInstanced(3, 1, 0, 0);
    transition(list, buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = slot.pixels.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = buffers[index].Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(list, buffers[index].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    if (timing) {
      list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2 + 1);
      list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2, 2, slot.timestamps.Get(), 0);
    }
    check(list->Close(), "frame-close");
    ID3D12CommandList *lists[] = {list}; m.submit_begin = qpc(); queue->ExecuteCommandLists(1, lists); m.submit_end = qpc();
    if (amd) {
      packet(amd.Get(), 1u << 5); // SDK MarkEndOfFrameRendering after Execute, before Present.
      packet(amd.Get(), 1u << 2); // This workload renders real frames; never label them interpolated.
    }
    m.present_begin = qpc(); check(swap->Present(0, 0), "present"); m.present_end = qpc();
    slot.frame = frame; slot.fence = frame + 1; check(queue->Signal(fence.Get(), slot.fence), "queue-signal");
  }
  for (auto &slot : slots) retire(slot);
  if (timing) {
    UINT64 gpu_calibration = 0, cpu_calibration = 0;
    const UINT64 before = qpc();
    check(queue->GetClockCalibration(&gpu_calibration, &cpu_calibration), "clock-calibration");
    const UINT64 after = qpc();
    require(gpu_calibration && cpu_calibration >= before && cpu_calibration <= after, "paired-clock-calibration");
    std::printf("%s_CALIBRATION gpu=%llu cpu=%llu before=%llu after=%llu\n", kPrefix,
      gpu_calibration, cpu_calibration, before, after);
  }
  if (amd) {
    // Validate the public interpolated packet without mislabelling a real Present.
    packet(amd.Get(), (1u << 2) | (1u << 3)); packet(amd.Get(), 1u << 2);
    Options disabled; expect(amd->UpdateAntiLagState(&disabled), S_OK, "AMD-disable");
    auto *released = amd.Detach(); require(released->Release() == 0, "AMD-final-release");
    check(device->GetDeviceRemovedReason(), "device-live-after-AMD-release");
    ComPtr<IAmdExtAntiLagApi> recreated;
    const auto create = reinterpret_cast<CreateAntiLag>(GetProcAddress(GetModuleHandleW(L"amdxc64.dll"), "AmdExtD3DCreateInterface"));
    check(create(device.Get(), __uuidof(IAmdExtAntiLagApi), reinterpret_cast<void **>(recreated.GetAddressOf())), "AMD-reinitialize");
    expect(recreated->UpdateAntiLagState(&disabled), S_OK, "AMD-reinitialize-disabled");
  }
  require(verified == frames, "all-frames-verified");
  std::printf("%s_CLOCK cpu_frequency=%llu gpu_supported=%u gpu_frequency=%llu gpu_status=0x%08lx\n", kPrefix,
    static_cast<UINT64>(cpu_frequency.QuadPart), UINT(timing), gpu_frequency, static_cast<unsigned long>(timing_hr));
  for (UINT frame = 0; frame < frames; ++frame) {
    const auto &m = measurements[frame];
    std::printf("%s_FRAME frame=%u input_begin=%llu input_end=%llu submit_begin=%llu submit_end=%llu present_begin=%llu present_end=%llu gpu_begin=%llu gpu_end=%llu pixels=%u readback=exact\n", kPrefix,
      frame, m.input_begin, m.input_end, m.submit_begin, m.submit_end, m.present_begin, m.present_end,
      m.gpu_begin, m.gpu_end, kWidth * kHeight);
  }
  std::printf("%s_PASS mode=%s frames=%u verified=%u width=%u height=%u draws=%u slots=%u amd_enable=%u reflex_enable=0\n", kPrefix,
    mode, frames, verified, kWidth, kHeight, kDraws, kSlots, UINT(cooperative));
  CloseHandle(completed); swap.Reset(); base.Reset(); buffers = {}; DestroyWindow(window); UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}
