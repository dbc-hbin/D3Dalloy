#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

namespace {
constexpr char kPrefix[] = "OWNED_D3D12_COLORSPACE";
constexpr UINT kWidth = 64, kHeight = 48;
constexpr DWORD kTimeoutMs = 10000;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx\n", kPrefix, step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool ok, const char *step) { if (!ok) fail(step); }
void expect(HRESULT got, HRESULT wanted, const char *step, UINT value = 0xffffffffu) {
  if (got != wanted) {
    std::fprintf(stderr, "%s_EXPECT step=%s value=%u wanted=0x%08lx got=0x%08lx\n",
                 kPrefix, step, value, static_cast<unsigned long>(wanted), static_cast<unsigned long>(got));
    fail(step, got);
  }
}

struct Window {
  HWND hwnd;
  Window() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"OwnedD3D12ColorSpaceWindow";
    require(RegisterClassW(&cls) != 0, "register-class");
    hwnd = CreateWindowExW(0, cls.lpszClassName, L"D3D12 color-space contract", WS_OVERLAPPEDWINDOW,
                           80, 80, kWidth, kHeight, nullptr, nullptr, cls.hInstance, nullptr);
    require(hwnd != nullptr, "create-window");
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
  }
  ~Window() {
    DestroyWindow(hwnd);
    UnregisterClassW(L"OwnedD3D12ColorSpaceWindow", GetModuleHandleW(nullptr));
  }
};

void waitFence(ID3D12Fence *fence, UINT64 value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "create-event");
  check(fence->SetEventOnCompletion(value, event), "set-fence-event");
  DWORD status = WaitForSingleObject(event, kTimeoutMs);
  CloseHandle(event);
  require(status == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value, "fence-complete");
}

const DXGI_COLOR_SPACE_TYPE supported[] = {
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020,
};

struct Context {
  Window window;
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue, queueB;
  ComPtr<IDXGISwapChain4> chain;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12DescriptorHeap> rtv;
  UINT64 nextFence = 0;
  DXGI_FORMAT format;
  UINT frameNumber = 0;
  bool rotatedQueues = false;

  explicit Context(DXGI_FORMAT requested) : format(requested) {
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumAdapters1(0, &adapter), "adapter");
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    auto module = GetModuleHandleW(L"d3d12.dll");
    require(module != nullptr, "d3d12-module");
    using BackendId = const char *(WINAPI *)();
    auto backendId = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
    require(backendId != nullptr && backendId() &&
            std::strcmp(backendId(), "owned-graphics-d3d12") == 0, "owned-backend");
    std::printf("%s_BACKEND id=owned-graphics-d3d12\n", kPrefix);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queueB)), "queue-b");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> base;
    check(factory->CreateSwapChainForHwnd(queue.Get(), window.hwnd, &desc, nullptr, nullptr, &base), "swapchain");
    check(base.As(&chain), "swapchain4");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&list)), "list");
    check(list->Close(), "initial-close");
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtv)), "rtv-heap");
  }

  ID3D12CommandQueue *currentQueue(UINT index) const {
    return rotatedQueues && index == 1 ? queueB.Get() : queue.Get();
  }
  void frame() {
    const UINT index = chain->GetCurrentBackBufferIndex();
    require(index < 2, "buffer-index");
    ComPtr<ID3D12Resource> source, readback;
    check(chain->GetBuffer(index, IID_PPV_ARGS(&source)), "get-buffer");
    auto desc = source->GetDesc();
    require(desc.Format == format && desc.Width == kWidth && desc.Height == kHeight, "buffer-format-size");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &bytes);
    const UINT pixelBytes = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    require(rows == kHeight && rowBytes == kWidth * pixelBytes && bytes >= rowBytes * rows,
            "readback-footprint");
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
                                          IID_PPV_ARGS(&readback)), "readback-resource");
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
    const bool odd = frameNumber & 1;
    const float color[] = {odd ? 0.75f : 0.25f, 0.0f, 0.0f, 1.0f};
    list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = source.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    check(list->Close(), "list-close");
    ID3D12CommandList *lists[] = {list.Get()};
    auto *active = currentQueue(index);
    active->ExecuteCommandLists(1, lists);
    check(active->Signal(fence.Get(), ++nextFence), "frame-signal");
    waitFence(fence.Get(), nextFence);
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    void *mapped = nullptr;
    check(readback->Map(0, &range, &mapped), "readback-map");
    const UINT32 expected32 = format == DXGI_FORMAT_R10G10B10A2_UNORM
        ? (odd ? 767u : 256u) | (3u << 30)
        : (odd ? 191u : 64u) | (255u << 24);
    const UINT64 expected64 = static_cast<UINT64>(odd ? 0x3a00u : 0x3400u) | (static_cast<UINT64>(0x3c00) << 48);
    for (UINT y = 0; y < kHeight; ++y) {
      auto row = static_cast<const unsigned char *>(mapped) + y * footprint.Footprint.RowPitch;
      for (UINT x = 0; x < kWidth; ++x) {
        if (pixelBytes == 8) {
          UINT64 actual;
          std::memcpy(&actual, row + x * 8, 8);
          require(actual == expected64, "float-source-readback");
        } else {
          UINT32 actual;
          std::memcpy(&actual, row + x * 4, 4);
          require(actual == expected32, "unorm-source-readback");
        }
      }
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    source.Reset();
    readback.Reset();
    check(chain->Present(0, 0), "present");
    require(chain->GetCurrentBackBufferIndex() == (index + 1) % 2, "present-rotation");
    check(active->Signal(fence.Get(), ++nextFence), "present-signal");
    waitFence(fence.Get(), nextFence);
    std::printf("%s_FRAME format=%u frame=%u source_pixels=%u exact=1 fence=complete present=ok\n",
                kPrefix, static_cast<UINT>(format), frameNumber++, kWidth * kHeight);
  }

  void resize(DXGI_FORMAT next, bool same) {
    check(chain->ResizeBuffers(2, kWidth, kHeight, next, 0), "resize-buffers");
    format = next;
    rotatedQueues = false;
    require(chain->GetCurrentBackBufferIndex() == 0, "resize-index");
    frame();
    std::printf("%s_RESIZE same=%u format=%u frame=ok\n", kPrefix, same ? 1u : 0u,
                static_cast<UINT>(format));
  }
};

struct SetRequest {
  IDXGISwapChain4 *chain;
  DXGI_COLOR_SPACE_TYPE colorSpace;
  HRESULT result = E_FAIL;
};
DWORD WINAPI setOnThread(void *opaque) {
  auto *request = static_cast<SetRequest *>(opaque);
  request->result = request->chain->SetColorSpace1(request->colorSpace);
  return 0;
}

void verifyContract(Context &c) {
  expect(c.chain->CheckColorSpaceSupport(supported[0], nullptr), E_INVALIDARG, "null-support");
  for (const auto space : supported) {
    UINT flags = 0xdeadbeef;
    check(c.chain->CheckColorSpaceSupport(space, &flags), "mapped-support");
    require(flags & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT, "mapped-present-flag");
    check(c.chain->SetColorSpace1(space), "mapped-set");
    flags = 0;
    check(c.chain->CheckColorSpaceSupport(space, &flags), "selected-support");
    require(flags & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT, "selected-present-flag");
    c.frame();
  }
  UINT rejected = 0;
  // The SDK defines values 0..24, of which only 0,1,12,17 map to native RGB.
  // Include RESERVED and all YCbCr/studio/HLG/G24 types, plus CUSTOM and unknown.
  for (UINT value = 0; value <= 24; ++value) {
    if (value == 0 || value == 1 || value == 12 || value == 17) continue;
    auto space = static_cast<DXGI_COLOR_SPACE_TYPE>(value);
    UINT flags = 0xdeadbeef;
    check(c.chain->CheckColorSpaceSupport(space, &flags), "unmapped-support");
    require(flags == 0, "unmapped-support-zero");
    expect(c.chain->SetColorSpace1(space), DXGI_ERROR_INVALID_CALL, "unmapped-set", value);
    ++rejected;
  }
  for (const auto value : {DXGI_COLOR_SPACE_CUSTOM, static_cast<DXGI_COLOR_SPACE_TYPE>(0x5eed)}) {
    UINT flags = 0xdeadbeef;
    check(c.chain->CheckColorSpaceSupport(value, &flags), "custom-unknown-support");
    require(flags == 0, "custom-unknown-support-zero");
    expect(c.chain->SetColorSpace1(value), DXGI_ERROR_INVALID_CALL, "custom-unknown-set",
           static_cast<UINT>(value));
    ++rejected;
  }
  c.frame(); // The rejected setters must not poison the already selected space or frame rotation.
  std::printf("%s_CONTRACT mapped=4 rejected=%u null=invalidarg post_reject_frame=ok\n", kPrefix, rejected);
}

void pendingPresent(Context &c) {
  // Rotate per-buffer queues, then block B. A previously accepted Present on B
  // must not make a concurrent color-space setter wait for GPU completion.
  const UINT masks[] = {1, 1};
  IUnknown *queues[] = {c.queue.Get(), c.queueB.Get()};
  check(c.chain->ResizeBuffers1(2, kWidth, kHeight, DXGI_FORMAT_UNKNOWN, 0, masks, queues),
        "rotate-queues-resize");
  c.rotatedQueues = true;
  require(c.chain->GetCurrentBackBufferIndex() == 0, "rotation-start");
  c.frame(); // Queue A -> index 1.
  ComPtr<ID3D12Fence> gate;
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
  check(c.queueB->Wait(gate.Get(), 1), "block-b");
  check(c.chain->Present(0, 0), "accepted-pending-present");
  require(c.chain->GetCurrentBackBufferIndex() == 0, "pending-rotation");
  // Previous accepted frame is PQ; the setter must transition to linear RGB.
  SetRequest request{c.chain.Get(), DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709};
  HANDLE thread = CreateThread(nullptr, 0, setOnThread, &request, 0, nullptr);
  require(thread != nullptr, "setter-thread");
  const bool returnedBeforeRelease = WaitForSingleObject(thread, 750) == WAIT_OBJECT_0;
  check(gate->Signal(1), "release-b"); // Release even on failure; never strand the GPU queue.
  require(WaitForSingleObject(thread, kTimeoutMs) == WAIT_OBJECT_0, "setter-complete");
  CloseHandle(thread);
  require(returnedBeforeRelease, "setter-waited-on-pending-present");
  check(request.result, "pending-setter-result");
  check(c.queueB->Signal(c.fence.Get(), ++c.nextFence), "pending-signal");
  waitFence(c.fence.Get(), c.nextFence);
  c.frame();
  std::printf("%s_PENDING accepted=1 setter_before_release=1 queue_rotation=1 next_frame=ok\n", kPrefix);
}

void run() {
  Context c(DXGI_FORMAT_R8G8B8A8_UNORM);
  verifyContract(c);
  check(c.chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020), "explicit-before-resize");
  ComPtr<ID3D12Resource> heldBuffer;
  check(c.chain->GetBuffer(0, IID_PPV_ARGS(&heldBuffer)), "held-buffer");
  expect(c.chain->ResizeBuffers(2, kWidth, kHeight, DXGI_FORMAT_R10G10B10A2_UNORM, 0),
         DXGI_ERROR_INVALID_CALL, "reject-resize-with-live-buffer");
  DXGI_SWAP_CHAIN_DESC1 preserved{};
  check(c.chain->GetDesc1(&preserved), "failed-resize-desc");
  require(preserved.Format == DXGI_FORMAT_R8G8B8A8_UNORM && preserved.BufferCount == 2,
          "failed-resize-preserved-desc");
  heldBuffer.Reset();
  c.frame();
  std::printf("%s_FAILED_RESIZE rejected=1 desc_preserved=1 next_frame=ok\n", kPrefix);
  c.resize(DXGI_FORMAT_R8G8B8A8_UNORM, true);
  c.resize(DXGI_FORMAT_R10G10B10A2_UNORM, false);
  check(c.chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020), "pq-rgb10");
  c.frame();
  pendingPresent(c);
  check(c.chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020), "restore-pq-before-float-resize");
  c.resize(DXGI_FORMAT_R16G16B16A16_FLOAT, false);
  check(c.chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709), "linear-float");
  c.frame();
  // Source readback proves per-format rendering and copy/Present, not the
  // private composited output's converted pixels or retained renderer state.
  std::printf("%s_PASS frames=%u formats=3 source_readback=1 present=1\n", kPrefix, c.frameNumber);
}
} // namespace

int main() { run(); return 0; }
