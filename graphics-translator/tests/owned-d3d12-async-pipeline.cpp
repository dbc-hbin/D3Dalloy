// SPDX-License-Identifier: LGPL-2.1-or-later
// d3d12.asyncPipelineCompile public D3D12 contract with GPU readback.
// --hold: the runner sets OWNED_ASYNC_PIPELINE_HOLD=1, so no worker ever links a PSO and every
// pending link completes only through a synchronous consumer. Omission is then deterministic.
// Without --hold, real workers race the first draw; that draw may legally be omitted or rendered.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr char kPrefix[] = "OWNED_D3D12_ASYNC_PIPELINE";
constexpr char kExpectedBackendId[] = "owned-graphics-d3d12";
constexpr UINT kWidth = 64, kHeight = 64;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// RenderTargetHistory::kRecurringEpochs; one more frame proves the streak, not an off-by-one.
constexpr unsigned kWarmFrames = 6;

[[noreturn]] void fail(const char *what, HRESULT hr = E_FAIL) {
  std::printf("%s_FAIL step=%s hr=0x%08lx\n", kPrefix, what, static_cast<unsigned long>(hr));
  std::exit(1);
}
void check(HRESULT hr, const char *what) {
  if (FAILED(hr)) fail(what, hr);
}
void require(bool value, const char *what) {
  if (!value) fail(what);
}

void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3DBlob> compile(const std::string &source, const char *target) {
  ComPtr<ID3DBlob> bytes, errors;
  const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", target, 0, 0,
                                &bytes, &errors);
  if (FAILED(hr) && errors)
    std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
  check(hr, "D3DCompile");
  return bytes;
}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool hold = argc > 1 && !std::strcmp(argv[1], "--hold");

  using BackendId = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendId>(GetProcAddress(LoadLibraryW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  require(backend && backend() && !std::strcmp(backend(), kExpectedBackendId), "backend identity");
  std::printf("%s_BACKEND id=%s\n", kPrefix, backend());

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.hInstance = instance;
  wc.lpfnWndProc = DefWindowProcW;
  wc.lpszClassName = L"OwnedAsyncPipeline";
  require(RegisterClassW(&wc) != 0, "RegisterClass");
  HWND window = CreateWindowW(wc.lpszClassName, L"async pipeline", WS_OVERLAPPEDWINDOW, 0, 0, kWidth, kHeight,
                              nullptr, nullptr, instance, nullptr);
  require(window != nullptr, "CreateWindow");
  ShowWindow(window, SW_SHOWNOACTIVATE);

  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
  DXGI_SWAP_CHAIN_DESC1 swap_desc{};
  swap_desc.Width = kWidth;
  swap_desc.Height = kHeight;
  swap_desc.Format = kFormat;
  swap_desc.SampleDesc.Count = 1;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_desc.BufferCount = 2;
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swap_base;
  check(factory->CreateSwapChainForHwnd(queue.Get(), window, &swap_desc, nullptr, nullptr, &swap_base),
        "CreateSwapChainForHwnd");
  ComPtr<IDXGISwapChain3> swap;
  check(swap_base.As(&swap), "IDXGISwapChain3");

  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
        "CreateCommandList");
  check(list->Close(), "initial Close");

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 1};
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].Descriptor.ShaderRegister = 1;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  auto make_root = [&](UINT count) {
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = count;
    desc.pParameters = parameters;
    ComPtr<ID3DBlob> bytes, errors;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors), "SerializeRoot");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(), IID_PPV_ARGS(&root)),
          "CreateRootSignature");
    return root;
  };
  auto root = make_root(1), uav_root = make_root(2);

  auto vertex = compile("float4 main(uint id:SV_VertexID):SV_Position{"
                        "float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}",
                        "vs_5_0");
  // A distinct seed makes a distinct native pipeline; color identifies which PSO rendered.
  auto pipeline = [&](unsigned seed, bool writes_uav = false) {
    const unsigned channel = seed % 3;
    const float level = 0.5f + float(seed % 31) / 64.0f;
    char source[512];
    std::snprintf(source, sizeof(source),
                  "cbuffer C:register(b0){float gain;}RWStructuredBuffer<uint> writes:register(u1);"
                  "float4 main():SV_Target{%sreturn float4(%.8f*gain,%.8f*gain,%.8f*gain,1);}",
                  writes_uav ? "uint previous;InterlockedAdd(writes[0],1,previous);" : "",
                  channel == 0 ? level : 0.f, channel == 1 ? level : 0.f, channel == 2 ? level : 0.f);
    auto pixel = compile(source, "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = writes_uav ? uav_root.Get() : root.Get();
    desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    auto &blend = desc.BlendState.RenderTarget[0];
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = kFormat;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> result;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&result)), "CreateGraphicsPipelineState");
    return result;
  };

  D3D12_HEAP_PROPERTIES default_heap{};
  default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC texture_desc{};
  texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture_desc.Width = kWidth;
  texture_desc.Height = kHeight;
  texture_desc.DepthOrArraySize = 1;
  texture_desc.MipLevels = 1;
  texture_desc.Format = kFormat;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> target;
  auto create_target = [&] {
    target.Reset();
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)),
          "create target");
  };
  create_target();
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)), "RTV heap");
  const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 readback_size = 0;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_size);
  auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)),
          "create buffer");
    return result;
  };
  auto readback = buffer(readback_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto indices = buffer(3 * sizeof(UINT16), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  const D3D12_RANGE no_read{0, 0};
  check(indices->Map(0, &no_read, &mapped), "map indices");
  const UINT16 index_data[] = {0, 1, 2};
  std::memcpy(mapped, index_data, sizeof(index_data));
  indices->Unmap(0, nullptr);
  const D3D12_INDEX_BUFFER_VIEW index_view{indices->GetGPUVirtualAddress(), sizeof(index_data), DXGI_FORMAT_R16_UINT};
  auto counter = buffer(sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto counter_readback = buffer(sizeof(UINT), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = buffer(sizeof(UINT), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  check(zero->Map(0, &no_read, &mapped), "map zero");
  std::memset(mapped, 0, sizeof(UINT));
  zero->Unmap(0, nullptr);
  D3D12_QUERY_HEAP_DESC query_desc{};
  query_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  query_desc.Count = 1;
  ComPtr<ID3D12QueryHeap> query;
  check(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query)), "CreateQueryHeap");
  auto query_readback = buffer(sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "CreateEvent");
  UINT64 serial = 0;

  struct Frame {
    std::array<unsigned, 4> pixel;
    UINT uav_writes;
  };
  enum class Kind { Direct, Indexed };
  // `second`: a later draw in the same unpresented frame. `volume`: render into another target
  // (bound through `rtv`) without readback; its omission is judged from the runner's trace count.
  auto frame = [&](ID3D12PipelineState *pso, Kind kind, bool guarded, bool writes_uav = false,
                   ID3D12PipelineState *second = nullptr, ID3D12Resource *volume = nullptr) {
    check(allocator->Reset(), "allocator Reset");
    check(list->Reset(allocator.Get(), nullptr), "list Reset");
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(counter.Get(), 0, zero.Get(), 0, sizeof(UINT));
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float clear[] = {0, 0, 0, 1};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(kWidth), LONG(kHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(writes_uav ? uav_root.Get() : root.Get());
    const float gain = 1.f;
    UINT bits;
    std::memcpy(&bits, &gain, sizeof(bits));
    list->SetGraphicsRoot32BitConstant(0, bits, 0);
    if (writes_uav)
      list->SetGraphicsRootUnorderedAccessView(1, counter->GetGPUVirtualAddress());
    list->SetPipelineState(pso);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetIndexBuffer(&index_view);
    if (guarded)
      list->BeginQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    if (kind == Kind::Indexed)
      list->DrawIndexedInstanced(3, 1, 0, 0, 0);
    else
      list->DrawInstanced(3, 1, 0, 0);
    if (second) {
      list->SetPipelineState(second);
      list->DrawInstanced(3, 1, 0, 0);
    }
    if (guarded) {
      list->EndQuery(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
      list->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1, query_readback.Get(), 0);
    }
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(counter_readback.Get(), 0, counter.Get(), 0, sizeof(UINT));
    transition(list.Get(), counter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (volume) {
      check(list->Close(), "volume list Close");
      check(fence->SetEventOnCompletion(++serial, event), "SetEventOnCompletion");
      ID3D12CommandList *submitted[] = {list.Get()};
      queue->ExecuteCommandLists(1, submitted);
      check(queue->Signal(fence.Get(), serial), "Signal");
      require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "GPU completion");
      check(device->GetDeviceRemovedReason(), "device removed");
      check(swap->Present(0, 0), "Present");
      return Frame{};
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dest{};
    dest.pResource = readback.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    ComPtr<ID3D12Resource> backbuffer;
    check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "GetBuffer");
    transition(list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer.Get(), target.Get());
    transition(list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    check(list->Close(), "list Close");
    // Subscribe before submission; the event is the completion signal, no timing waits.
    check(fence->SetEventOnCompletion(++serial, event), "SetEventOnCompletion");
    ID3D12CommandList *submitted[] = {list.Get()};
    queue->ExecuteCommandLists(1, submitted);
    check(queue->Signal(fence.Get(), serial), "Signal");
    require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "GPU completion");
    check(device->GetDeviceRemovedReason(), "device removed");
    // Present advances the presentation epoch that target recurrence counts.
    check(swap->Present(0, 0), "Present");
    Frame result{};
    const D3D12_RANGE range{0, SIZE_T(readback_size)};
    check(readback->Map(0, &range, &mapped), "map readback");
    const auto *pixel = static_cast<const unsigned char *>(mapped) + footprint.Offset +
                        (kHeight / 2) * footprint.Footprint.RowPitch + (kWidth / 2) * 4;
    result.pixel = {pixel[0], pixel[1], pixel[2], pixel[3]};
    readback->Unmap(0, &no_read);
    const D3D12_RANGE counter_range{0, sizeof(UINT)};
    check(counter_readback->Map(0, &counter_range, &mapped), "map counter");
    result.uav_writes = *static_cast<const UINT *>(mapped);
    counter_readback->Unmap(0, &no_read);
    if (guarded) {
      const D3D12_RANGE query_range{0, sizeof(UINT64)};
      check(query_readback->Map(0, &query_range, &mapped), "map query");
      const UINT64 samples = *static_cast<const UINT64 *>(mapped);
      query_readback->Unmap(0, &no_read);
      require(samples != 0, "query-guarded draw executed");
    }
    return result;
  };
  // Exact color of `seed`, or the clear color when `omitted`.
  auto rendered = [](const Frame &f, unsigned seed) {
    const unsigned channel = seed % 3;
    const int expected = int((0.5f + float(seed % 31) / 64.0f) * 255.0f + 0.5f);
    for (unsigned c = 0; c < 3; ++c)
      if (std::abs(int(f.pixel[c]) - (c == channel ? expected : 0)) > 1) return false;
    return f.pixel[3] == 255;
  };
  auto cleared = [](const Frame &f) { return !f.pixel[0] && !f.pixel[1] && !f.pixel[2] && f.pixel[3] == 255; };
  auto expect = [&](bool ok, const char *name, const Frame &f) {
    std::printf("%s_%s %s rgba=%u,%u,%u,%u\n", kPrefix, ok ? "CASE_PASS" : "CASE_FAIL", name, f.pixel[0], f.pixel[1],
                f.pixel[2], f.pixel[3]);
    if (!ok) std::exit(1);
  };
  // A pending first draw on a recurring target: held runs must omit it; worker runs may race either way.
  auto pending = [&](const Frame &f, unsigned seed, const char *name) {
    expect(hold ? cleared(f) : cleared(f) || rendered(f, seed), name, f);
  };
  auto warm_up = [&](ID3D12PipelineState *warm, const char *name) {
    for (unsigned i = 0; i < kWarmFrames; ++i) {
      const auto f = frame(warm, Kind::Direct, true);
      if (!rendered(f, 0)) expect(false, name, f);
    }
  };

  // The first warm draw has a first-use target, so it joins its pending link synchronously.
  auto warm = pipeline(0);
  warm_up(warm.Get(), "first_use_target_waits");
  std::printf("%s_CASE_PASS warm_recurring_target frames=%u\n", kPrefix, kWarmFrames);

  auto cold_direct = pipeline(1);
  pending(frame(cold_direct.Get(), Kind::Direct, false), 1, "pending_direct_omitted");
  {
    const auto f = frame(cold_direct.Get(), Kind::Direct, true);
    expect(rendered(f, 1), "query_draw_waits", f);
  }
  {
    const auto f = frame(cold_direct.Get(), Kind::Direct, false);
    expect(rendered(f, 1), "ready_draw_resumes", f);
  }

  warm_up(warm.Get(), "restore_recurring_target");
  auto cold_indexed = pipeline(2);
  pending(frame(cold_indexed.Get(), Kind::Indexed, false), 2, "pending_indexed_omitted");
  // Omitted draws never extend recurrence: one Present later the target has expired, so the
  // same still-pending pipeline is joined and rendered instead of being omitted again.
  {
    const auto f = frame(cold_indexed.Get(), Kind::Indexed, false);
    expect(rendered(f, 2), "omitted_history_expires", f);
  }

  warm_up(warm.Get(), "restore_before_uav");
  auto uav = pipeline(20, true);
  {
    const auto f = frame(uav.Get(), Kind::Direct, false, true);
    expect(rendered(f, 20) && f.uav_writes == kWidth * kHeight, "uav_pipeline_waits", f);
  }

  for (unsigned seed = 10; seed < 18; ++seed)
    pipeline(seed).Reset(); // Released while its link is queued: cancelled, never touches the PSO.
  {
    const auto f = frame(warm.Get(), Kind::Direct, true);
    expect(rendered(f, 0), "release_while_pending", f);
  }

  create_target();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  auto one_shot = pipeline(21);
  {
    const auto f = frame(one_shot.Get(), Kind::Direct, false);
    expect(rendered(f, 21), "new_target_waits", f);
  }

  // Five-Present boundary on a fresh target: frame 1 is first use, so after the warm draws plus an
  // in-frame ready draw only four Presents have completed. The pending draw recorded in that same
  // unpresented frame must still wait; after one more Present it is omittable.
  auto boundary = [&](unsigned presented, unsigned seed, const char *name, bool omit_expected) {
    create_target();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    for (unsigned i = 0; i < presented; ++i) {
      const auto f = frame(warm.Get(), Kind::Direct, true);
      if (!rendered(f, 0)) expect(false, name, f);
    }
    auto cold = pipeline(seed);
    // The ready warm draw first (records this unpresented epoch), then the pending draw.
    const auto f = frame(warm.Get(), Kind::Direct, false, false, cold.Get());
    if (omit_expected)
      expect(hold ? rendered(f, 0) : rendered(f, 0) || rendered(f, seed), name, f);
    else
      expect(rendered(f, seed), name, f);
  };
  boundary(4, 22, "four_presents_wait", false);
  boundary(5, 23, "five_presents_omit", true);

  // A widened 3D RTV is a different target from the warmed one-slice RTV at the same FirstWSlice.
  {
    D3D12_RESOURCE_DESC volume_desc = texture_desc;
    volume_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    volume_desc.DepthOrArraySize = 2;
    ComPtr<ID3D12Resource> volume;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &volume_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&volume)),
          "create volume");
    D3D12_RENDER_TARGET_VIEW_DESC view{};
    view.Format = kFormat;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
    view.Texture3D = {0, 0, 1};
    device->CreateRenderTargetView(volume.Get(), &view, rtv);
    for (unsigned i = 0; i < kWarmFrames; ++i)
      frame(warm.Get(), Kind::Direct, false, false, nullptr, volume.Get());
    view.Texture3D.WSize = 2;
    device->CreateRenderTargetView(volume.Get(), &view, rtv);
    // Pending draw on the widened view: must wait (no omission trace), checked by the runner count.
    auto cold = pipeline(24);
    frame(cold.Get(), Kind::Direct, false, false, nullptr, volume.Get());
    std::printf("%s_CASE_PASS wider_volume_rtv_recorded\n", kPrefix);
    create_target();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  }

  // A pending draw must not hide PreDraw validation: mismatched sample positions fail Close().
  {
    for (unsigned i = 0; i < kWarmFrames; ++i)
      frame(warm.Get(), Kind::Direct, true);
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
    const bool programmable = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2,
                                                                    sizeof(options2))) &&
                              options2.ProgrammableSamplePositionsTier != D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
    if (programmable) {
      D3D12_SAMPLE_POSITION positions[2] = {{4, 4}, {-4, -4}};
      const D3D12_VIEWPORT viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
      const D3D12_RECT scissor{0, 0, LONG(kWidth), LONG(kHeight)};
      // Control: the setter alone is valid, so a failure below comes from the draw's validation.
      {
        ComPtr<ID3D12CommandAllocator> control_allocator;
        ComPtr<ID3D12GraphicsCommandList1> control;
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&control_allocator)),
              "control allocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, control_allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&control)), "control list");
        control->SetSamplePositions(2, 1, positions);
        check(control->Close(), "sample positions setter control");
      }
      auto cold = pipeline(25);
      ComPtr<ID3D12CommandAllocator> probe_allocator;
      ComPtr<ID3D12GraphicsCommandList1> probe;
      check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probe_allocator)),
            "probe allocator");
      check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, probe_allocator.Get(), cold.Get(),
                                      IID_PPV_ARGS(&probe)), "probe list");
      probe->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      probe->RSSetViewports(1, &viewport);
      probe->RSSetScissorRects(1, &scissor);
      probe->SetGraphicsRootSignature(root.Get());
      probe->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      probe->SetSamplePositions(2, 1, positions);
      probe->DrawInstanced(3, 1, 0, 0);
      const HRESULT close = probe->Close();
      std::printf("%s_%s sample_positions_validated close=0x%08lx\n", kPrefix,
                  FAILED(close) ? "CASE_PASS" : "CASE_FAIL", static_cast<unsigned long>(close));
      if (SUCCEEDED(close)) std::exit(1);
    } else {
      std::printf("%s_CASE_PASS sample_positions_validated unsupported\n", kPrefix);
    }
  }

  check(list->Reset(allocator.Get(), nullptr), "final Reset");
  check(list->Close(), "final Close");
  CloseHandle(event);
  swap.Reset();
  swap_base.Reset();
  DestroyWindow(window);
  std::printf("%s_PASS hold=%d\n", kPrefix, hold ? 1 : 0);
  return 0;
}
