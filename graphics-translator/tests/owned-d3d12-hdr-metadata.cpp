#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kWidth = 64, kHeight = 48;
constexpr DWORD kWaitMs = 10000;
constexpr char kPrefix[] = "OWNED_D3D12_HDR_METADATA";

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_HDR_METADATA_FAIL step=%s hr=0x%08lx\n", step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool ok, const char *step) { if (!ok) fail(step); }
void expect(HRESULT got, HRESULT wanted, const char *step) {
  if (got != wanted) {
    std::fprintf(stderr, "OWNED_D3D12_HDR_METADATA_EXPECT step=%s wanted=0x%08lx got=0x%08lx\n",
                 step, static_cast<unsigned long>(wanted), static_cast<unsigned long>(got));
    fail(step, got);
  }
}

struct Window {
  HWND hwnd;
  Window() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"OwnedD3D12HDRMetadataWindow";
    require(RegisterClassW(&cls) != 0, "register-window-class");
    hwnd = CreateWindowExW(0, cls.lpszClassName, L"HDR metadata contract", WS_OVERLAPPEDWINDOW,
                           80, 80, kWidth, kHeight, nullptr, nullptr, cls.hInstance, nullptr);
    require(hwnd != nullptr, "create-window");
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
  }
  ~Window() {
    DestroyWindow(hwnd);
    UnregisterClassW(L"OwnedD3D12HDRMetadataWindow", GetModuleHandleW(nullptr));
  }
};

void waitFence(ID3D12Fence *fence, UINT64 value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "create-fence-event");
  check(fence->SetEventOnCompletion(value, event), "set-fence-event");
  DWORD result = WaitForSingleObject(event, kWaitMs);
  CloseHandle(event);
  require(result == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value, "gpu-fence-completed");
}

struct Context {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain4> chain;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12DescriptorHeap> rtv;
  UINT64 nextFence = 0;
  Window window;

  Context() {
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    auto module = GetModuleHandleW(L"d3d12.dll");
    require(module != nullptr, "d3d12-module");
    using BackendId = const char *(WINAPI *)();
    auto backendId = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
    require(backendId != nullptr, "owned-backend-export");
    const char *id = backendId();
    require(id && std::strcmp(id, "owned-graphics-d3d12") == 0, "owned-backend");
    std::printf("%s_BACKEND id=owned-graphics-d3d12\n", kPrefix);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> base;
    check(factory->CreateSwapChainForHwnd(queue.Get(), window.hwnd, &desc, nullptr, nullptr, &base), "swapchain");
    check(base.As(&chain), "swapchain4");
    UINT support = 0;
    constexpr auto pq = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    check(chain->CheckColorSpaceSupport(pq, &support), "check-pq-color-space");
    require((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0, "pq-present-supported");
    check(chain->SetColorSpace1(pq), "set-pq-color-space");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&list)), "command-list");
    check(list->Close(), "initial-list-close");
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtv)), "rtv-heap");
  }

  // GPU readback proves a real D3D12 source frame and queue completion, NOT
  // the privately composited Presenter output or the HDR tone-map effect.
  void frame(UINT number) {
    const UINT index = chain->GetCurrentBackBufferIndex();
    require(index < 2, "backbuffer-index");
    ComPtr<ID3D12Resource> source, readback;
    check(chain->GetBuffer(index, IID_PPV_ARGS(&source)), "get-buffer");
    const auto desc = source->GetDesc();
    require(desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && desc.Width == kWidth && desc.Height == kHeight,
            "source-format-and-size");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &bytes);
    require(rows == kHeight && rowBytes == kWidth * 4 && bytes >= rowBytes * rows, "readback-footprint");
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &buffer,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&readback)), "readback-buffer");
    check(allocator->Reset(), "allocator-reset");
    check(list->Reset(allocator.Get(), nullptr), "list-reset");
    device->CreateRenderTargetView(source.Get(), nullptr, rtv->GetCPUDescriptorHandleForHeapStart());
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = source.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);
    const float color[4] = {static_cast<float>(number + 1) / 8.0f, 0.5f, 0.75f, 1.0f};
    list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = source.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    check(list->Close(), "close-list");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++nextFence), "signal-frame");
    waitFence(fence.Get(), nextFence);
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    void *mapped = nullptr;
    check(readback->Map(0, &range, &mapped), "map-source");
    constexpr UINT red[7] = {128, 256, 384, 512, 639, 767, 895};
    require(number < 7, "frame-number");
    const UINT expected = red[number] | (512u << 10) | (767u << 20) | (3u << 30);
    for (UINT y = 0; y < kHeight; ++y) {
      auto row = reinterpret_cast<const UINT *>(static_cast<const unsigned char *>(mapped) +
                                                y * footprint.Footprint.RowPitch);
      for (UINT x = 0; x < kWidth; ++x)
        if (row[x] != expected) fail("source-readback-pixels");
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    source.Reset();
    readback.Reset();
    check(chain->Present(0, 0), "present");
    require(chain->GetCurrentBackBufferIndex() == (index + 1) % 2, "present-rotation");
    check(queue->Signal(fence.Get(), ++nextFence), "signal-present");
    waitFence(fence.Get(), nextFence);
    std::printf("%s_FRAME frame=%u source_pixels=%u exact=1 fence=complete present=ok\n",
                kPrefix, number, kWidth * kHeight);
  }
};

struct ReplaceRequest {
  IDXGISwapChain4 *chain;
  DXGI_HDR_METADATA_HDR10 replacement;
  HRESULT result = E_FAIL;
};
DWORD WINAPI replaceOnThread(void *param) {
  auto *request = static_cast<ReplaceRequest *>(param);
  request->result = request->chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
                                                   sizeof(request->replacement), &request->replacement);
  return 0;
}

void run() {
  Context c;
  DXGI_HDR_METADATA_HDR10 hdr{};
  hdr.RedPrimary[0] = 34000; hdr.RedPrimary[1] = 16000;
  hdr.GreenPrimary[0] = 13250; hdr.GreenPrimary[1] = 34500;
  hdr.BluePrimary[0] = 7500; hdr.BluePrimary[1] = 3000;
  hdr.WhitePoint[0] = 15635; hdr.WhitePoint[1] = 16450;
  hdr.MaxMasteringLuminance = 1000;
  hdr.MinMasteringLuminance = 1;
  hdr.MaxContentLightLevel = 900;
  hdr.MaxFrameAverageLightLevel = 450;
  c.frame(0);
  expect(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, 0, &hdr), E_INVALIDARG, "reject-zero-size");
  expect(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr) - 1, &hdr), E_INVALIDARG, "reject-short-size");
  expect(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr) + 1, &hdr), E_INVALIDARG, "reject-long-size");
  expect(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), nullptr), E_INVALIDARG, "reject-null-data");
  expect(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10PLUS, sizeof(hdr), &hdr),
         DXGI_ERROR_UNSUPPORTED, "reject-hdr10-plus");
  expect(c.chain->SetHDRMetaData(static_cast<DXGI_HDR_METADATA_TYPE>(0x5eed), sizeof(hdr), &hdr),
         DXGI_ERROR_UNSUPPORTED, "reject-unknown-type");
  std::printf("%s_VALIDATION_PASS invalid=4 unsupported=2\n", kPrefix);
  check(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr), "set-hdr10");
  hdr.MaxMasteringLuminance = 77; // A setter must not retain caller-owned storage.
  c.frame(1);
  hdr.MaxMasteringLuminance = 4000;
  check(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr), "replace-hdr10");
  c.frame(2);
  check(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, sizeof(hdr), &hdr), "clear-ignored-args");
  c.frame(3);
  std::printf("%s_LIFECYCLE_PASS before=1 set=1 replaced=1 cleared=1 caller_modified=1\n", kPrefix);

  check(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr), "set-before-resize");
  check(c.chain->ResizeBuffers(2, kWidth, kHeight, DXGI_FORMAT_UNKNOWN, 0), "resize-buffers");
  c.frame(4);
  check(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr), "clear-after-resize");
  c.frame(5);
  std::printf("%s_RESIZE_PASS format=r10g10b10a2 frames=2\n", kPrefix);

  // A queued Present is accepted while the queue is blocked. The setter must
  // finish without waiting for GPU completion; always release the gate, even
  // if it fails this invariant, so the test cannot strand a GPU queue.
  ComPtr<ID3D12Fence> gate;
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate-fence");
  check(c.chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr), "set-blocked-frame");
  check(c.queue->Wait(gate.Get(), 1), "block-queue");
  check(c.chain->Present(0, 0), "accepted-blocked-present");
  ReplaceRequest request{c.chain.Get(), hdr};
  request.replacement.MaxMasteringLuminance = 2000;
  HANDLE thread = CreateThread(nullptr, 0, replaceOnThread, &request, 0, nullptr);
  require(thread != nullptr, "replacement-thread");
  const bool completedBeforeRelease = WaitForSingleObject(thread, 750) == WAIT_OBJECT_0;
  check(gate->Signal(1), "release-queue");
  require(WaitForSingleObject(thread, kWaitMs) == WAIT_OBJECT_0, "replacement-thread-completion");
  CloseHandle(thread);
  require(completedBeforeRelease, "setter-blocked-on-pending-present");
  check(request.result, "replace-while-present-pending");
  check(c.queue->Signal(c.fence.Get(), ++c.nextFence), "signal-unblocked-present");
  waitFence(c.fence.Get(), c.nextFence);
  c.frame(6);
  std::printf("%s_PENDING_PASS accepted=1 replaced_before_release=1 next_present=1\n", kPrefix);
  std::printf("%s_PASS frames=7 source_readback=1 gpu_fence=1 present=1\n", kPrefix);
}
} // namespace

int main() { run(); return 0; }
