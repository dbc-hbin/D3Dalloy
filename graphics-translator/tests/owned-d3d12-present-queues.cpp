#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kCompletionTimeoutMs = 10000;
// A present stuck behind a blocked queue must stay pending at least this long.
constexpr DWORD kBlockedProbeMs = 500;
constexpr UINT kWidth = 64;
constexpr UINT kHeight = 48;
constexpr UINT kFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_D3D12_PRESENT_QUEUES_FAIL step=%s hr=0x%08lx\n", step,
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

void expectHr(HRESULT actual, HRESULT expected, const char *step) {
  if (actual != expected) {
    std::fprintf(stderr, "OWNED_D3D12_PRESENT_QUEUES_FAIL step=%s expected=0x%08lx actual=0x%08lx\n", step,
                 static_cast<unsigned long>(expected), static_cast<unsigned long>(actual));
    ExitProcess(1);
  }
}

void requireOwnedBackend() {
  HMODULE module = GetModuleHandleW(L"d3d12.dll");
  require(module != nullptr, "d3d12-module");
  using BackendId = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(backendId != nullptr, "owned-backend-id-export");
  const char *id = backendId();
  require(id && std::strcmp(id, "owned-graphics-d3d12") == 0, "owned-backend-id");
  std::printf("OWNED_D3D12_PRESENT_QUEUES_BACKEND id=%s\n", id);
}

class Window final {
public:
  Window() {
    instance_ = GetModuleHandleW(nullptr);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = instance_;
    cls.lpszClassName = kClassName;
    atom_ = RegisterClassExW(&cls);
    require(atom_ != 0, "register-class");
    window_ = CreateWindowExW(0, kClassName, L"owned-d3d12-present-queues", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 64, 64, nullptr, nullptr, instance_, nullptr);
    require(window_ != nullptr, "create-window");
    ShowWindow(window_, SW_SHOWNA);
    UpdateWindow(window_);
  }

  ~Window() {
    if (window_)
      DestroyWindow(window_);
    if (atom_)
      UnregisterClassW(kClassName, instance_);
  }

  HWND get() const { return window_; }

private:
  static constexpr wchar_t kClassName[] = L"OwnedD3D12PresentQueuesWindow";
  HINSTANCE instance_ = nullptr;
  ATOM atom_ = 0;
  HWND window_ = nullptr;
};

ComPtr<ID3D12CommandQueue> createQueue(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
  D3D12_COMMAND_QUEUE_DESC desc{};
  desc.Type = type;
  ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "create-queue");
  return queue;
}

void waitIdle(ID3D12Device *device, ID3D12CommandQueue *queue) {
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "idle-fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "idle-event");
  check(queue->Signal(fence.Get(), 1), "idle-signal");
  check(fence->SetEventOnCompletion(1, event), "idle-set-event");
  require(WaitForSingleObject(event, kCompletionTimeoutMs) == WAIT_OBJECT_0, "idle-wait");
  CloseHandle(event);
}

void requireDesc(IDXGISwapChain3 *swapchain, UINT count, UINT width, UINT height, const char *step) {
  DXGI_SWAP_CHAIN_DESC1 desc{};
  check(swapchain->GetDesc1(&desc), "get-desc1");
  if (desc.BufferCount != count || desc.Width != width || desc.Height != height) {
    std::fprintf(stderr, "OWNED_D3D12_PRESENT_QUEUES_DESC step=%s count=%u width=%u height=%u\n", step,
                 desc.BufferCount, desc.Width, desc.Height);
    fail(step);
  }
}

// Presents one frame and reports whether the present completed (the waitable
// object is released by the present's GPU completion) within `timeout`.
bool presentCompletes(IDXGISwapChain3 *swapchain, HANDLE waitable, DWORD timeout) {
  check(swapchain->Present(0, 0), "present");
  return WaitForSingleObject(waitable, timeout) == WAIT_OBJECT_0;
}

struct PendingPresent {
  IDXGISwapChain3 *chain;
  HANDLE started;
  HANDLE finished;
  HRESULT result;
};

DWORD WINAPI runPendingPresent(void *value) {
  auto &attempt = *static_cast<PendingPresent *>(value);
  SetEvent(attempt.started);
  attempt.result = attempt.chain->Present(0, 0);
  SetEvent(attempt.finished);
  return 0;
}

struct PendingWait {
  HANDLE waitable;
  HANDLE started;
  HANDLE finished;
  DWORD result;
};

DWORD WINAPI runPendingWait(void *value) {
  auto &attempt = *static_cast<PendingWait *>(value);
  SetEvent(attempt.started);
  attempt.result = WaitForSingleObject(attempt.waitable, INFINITE);
  SetEvent(attempt.finished);
  return 0;
}

void run() {
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12Device> otherDevice;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&otherDevice)), "other-device");

  auto queueA = createQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto queueB = createQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto computeQueue = createQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE);
  auto foreignQueue = createQueue(otherDevice.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);

  Window window;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Flags = kFlags;
  ComPtr<IDXGISwapChain1> base;
  DXGI_SWAP_CHAIN_DESC1 badDesc = desc;
  badDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  expectHr(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &badDesc, nullptr, nullptr, &base),
           DXGI_ERROR_INVALID_CALL, "d3d12-requires-flip");
  badDesc = desc;
  badDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  expectHr(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &badDesc, nullptr, nullptr, &base),
           DXGI_ERROR_INVALID_CALL, "flip-format-must-be-linear");
  badDesc = desc;
  badDesc.SampleDesc.Count = 4;
  expectHr(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &badDesc, nullptr, nullptr, &base),
           DXGI_ERROR_INVALID_CALL, "flip-must-be-single-sample");
  badDesc = desc;
  badDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED;
  expectHr(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &badDesc, nullptr, nullptr, &base),
           DXGI_ERROR_UNSUPPORTED, "unprotected-metal-layer");
  badDesc = desc;
  badDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  expectHr(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &badDesc, nullptr, nullptr, &base),
           DXGI_ERROR_UNSUPPORTED, "opaque-hwnd-no-premultiplied-alpha");
  badDesc = desc;
  badDesc.Scaling = static_cast<DXGI_SCALING>(~UINT{0});
  expectHr(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &badDesc, nullptr, nullptr, &base),
           DXGI_ERROR_INVALID_CALL, "invalid-scaling-enum");
  require(!base, "rejected-creation-has-no-swapchain");
  check(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &desc, nullptr, nullptr, &base), "swapchain");
  ComPtr<IDXGISwapChain3> swapchain;
  check(base.As(&swapchain), "swapchain3");
  expectHr(swapchain->GetFrameStatistics(nullptr), E_INVALIDARG, "null-frame-statistics");
  expectHr(swapchain->GetContainingOutput(nullptr), E_INVALIDARG, "null-containing-output");
  IDXGIOutput *restricted = reinterpret_cast<IDXGIOutput *>(uintptr_t(1));
  check(swapchain->GetRestrictToOutput(&restricted), "unrestricted-output");
  require(restricted == nullptr, "unrestricted-output-null");
  expectHr(swapchain->GetRestrictToOutput(nullptr), E_INVALIDARG, "null-output-pointer");
  check(swapchain->SetMaximumFrameLatency(1), "max-latency");
  HANDLE waitable = swapchain->GetFrameLatencyWaitableObject();
  require(waitable != nullptr, "waitable");
  require(WaitForSingleObject(waitable, kCompletionTimeoutMs) == WAIT_OBJECT_0, "waitable-initial");
  expectHr(swapchain->Present(0, DXGI_PRESENT_RESTRICT_TO_OUTPUT), DXGI_ERROR_INVALID_CALL,
           "restricted-flag-without-output");
  UINT rejectedPresentCount = 1;
  check(swapchain->GetLastPresentCount(&rejectedPresentCount), "restricted-rejection-count");
  require(rejectedPresentCount == 0 && swapchain->GetCurrentBackBufferIndex() == 0 &&
          WaitForSingleObject(waitable, 0) == WAIT_TIMEOUT, "restricted-rejection-preserves-state");

  // Rejected calls must leave the swapchain untouched.
  ComPtr<ID3D12Resource> original0;
  check(swapchain->GetBuffer(0, IID_PPV_ARGS(&original0)), "get-buffer-0");
  check(swapchain->SetSourceSize(24, 18), "source-before-failed-resize");
  const UINT masks[3] = {1, 1, 1};
  IUnknown *directQueues[2] = {queueA.Get(), queueB.Get()};
  IUnknown *withCompute[2] = {queueA.Get(), computeQueue.Get()};
  IUnknown *withForeign[2] = {foreignQueue.Get(), queueB.Get()};
  IUnknown *withNull[2] = {queueA.Get(), nullptr};
  const UINT badMask[2] = {1, 2};
  int rejected = 0;
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, masks, directQueues),
           DXGI_ERROR_INVALID_CALL, "outstanding-reference");
  ++rejected;
  original0.Reset();
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, nullptr, directQueues),
           DXGI_ERROR_INVALID_CALL, "null-node-masks");
  ++rejected;
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, masks, nullptr),
           DXGI_ERROR_INVALID_CALL, "null-queues");
  ++rejected;
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, masks, withCompute),
           DXGI_ERROR_INVALID_CALL, "compute-queue");
  ++rejected;
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, masks, withForeign),
           DXGI_ERROR_INVALID_CALL, "foreign-queue");
  ++rejected;
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, masks, withNull),
           DXGI_ERROR_INVALID_CALL, "null-queue-entry");
  ++rejected;
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, badMask, directQueues),
           DXGI_ERROR_INVALID_CALL, "node-mask-2");
  ++rejected;
  expectHr(swapchain->ResizeBuffers1(1, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags, masks, directQueues),
           DXGI_ERROR_INVALID_CALL, "flip-single-buffer");
  ++rejected;
  expectHr(swapchain->ResizeBuffers(DXGI_MAX_SWAP_CHAIN_BUFFERS + 1, 32, 32, DXGI_FORMAT_UNKNOWN, kFlags),
           DXGI_ERROR_INVALID_CALL, "too-many-buffers");
  ++rejected;
  expectHr(swapchain->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, 0, masks, directQueues),
           DXGI_ERROR_INVALID_CALL, "drop-waitable-flag");
  ++rejected;
  expectHr(swapchain->ResizeBuffers(2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kFlags),
           DXGI_ERROR_INVALID_CALL, "flip-resize-srgb-format");
  ++rejected;
  expectHr(swapchain->ResizeBuffers(2, 32, 32, DXGI_FORMAT_UNKNOWN,
                                   kFlags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING),
           DXGI_ERROR_INVALID_CALL, "cannot-add-tearing-after-creation");
  ++rejected;
  requireDesc(swapchain.Get(), 2, kWidth, kHeight, "rejected-preserves-desc");
  require(swapchain->GetCurrentBackBufferIndex() == 0, "rejected-preserves-index");
  UINT preservedWidth = 0, preservedHeight = 0;
  check(swapchain->GetSourceSize(&preservedWidth, &preservedHeight), "failed-resize-source-size");
  require(preservedWidth == 24 && preservedHeight == 18, "failed-resize-preserves-source-size");
  std::printf("OWNED_D3D12_PRESENT_QUEUES_VALIDATION_PASS rejected=%d preserved=1\n", rejected);

  // Advance the index so the resize must reset it.
  require(presentCompletes(swapchain.Get(), waitable, kCompletionTimeoutMs), "pre-resize-present");
  require(swapchain->GetCurrentBackBufferIndex() == 1, "pre-resize-index");
  check(swapchain->ResizeBuffers1(2, 32, 24, DXGI_FORMAT_UNKNOWN, kFlags, masks, directQueues), "resize1");
  requireDesc(swapchain.Get(), 2, 32, 24, "resize1-desc");
  require(swapchain->GetCurrentBackBufferIndex() == 0, "resize1-index");
  check(swapchain->GetSourceSize(&preservedWidth, &preservedHeight), "successful-resize-source-size");
  require(preservedWidth == 32 && preservedHeight == 24, "successful-resize-resets-source-size");
  ComPtr<ID3D12Resource> resized;
  check(swapchain->GetBuffer(1, IID_PPV_ARGS(&resized)), "resize1-buffer");
  const auto resizedDesc = resized->GetDesc();
  require(resizedDesc.Width == 32 && resizedDesc.Height == 24, "resize1-buffer-size");
  resized.Reset();

  // Block queue B only. Buffer 0 presents on A and must complete; buffer 1
  // presents on B and must stay pending until B's wait is released.
  ComPtr<ID3D12Fence> gate;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
  check(queueB->Wait(gate.Get(), 1), "block-b");
  require(presentCompletes(swapchain.Get(), waitable, kCompletionTimeoutMs), "buffer0-on-queue-a");
  require(swapchain->GetCurrentBackBufferIndex() == 1, "rotate-to-1");
  check(swapchain->SetMaximumFrameLatency(2), "two-queued-frames");
  require(WaitForSingleObject(waitable, 0) == WAIT_OBJECT_0, "second-frame-credit");
  require(!presentCompletes(swapchain.Get(), waitable, kBlockedProbeMs), "buffer1-blocked-on-queue-b");
  require(swapchain->GetCurrentBackBufferIndex() == 0, "rotate-to-0");
  // The next frame targets buffer 0 on the unblocked queue A, but it must not
  // overtake the earlier frame still waiting on B. Both fit the explicit
  // two-frame queue limit; neither may complete before B is released.
  check(swapchain->Present(0, 0), "present-behind-blocked");
  require(WaitForSingleObject(waitable, kBlockedProbeMs) == WAIT_TIMEOUT, "present-order-not-overtaken");
  check(swapchain->SetMaximumFrameLatency(1), "reduce-latency-while-two-pending");
  UINT pendingCount = 0, maximumLatency = 0;
  check(swapchain->GetLastPresentCount(&pendingCount), "pending-count-before-do-not-wait");
  check(swapchain->GetMaximumFrameLatency(&maximumLatency), "reduced-maximum-latency");
  const UINT pendingIndex = swapchain->GetCurrentBackBufferIndex();
  expectHr(swapchain->Present(0, DXGI_PRESENT_DO_NOT_WAIT), DXGI_ERROR_WAS_STILL_DRAWING,
           "do-not-wait-at-capacity");
  UINT rejectedCount = 0;
  check(swapchain->GetLastPresentCount(&rejectedCount), "pending-count-after-do-not-wait");
  require(maximumLatency == 1 && rejectedCount == pendingCount &&
          swapchain->GetCurrentBackBufferIndex() == pendingIndex, "capacity-rejection-preserves-state");
  check(gate->Signal(1), "release-b");
  require(WaitForSingleObject(waitable, kCompletionTimeoutMs) == WAIT_OBJECT_0, "queued-frames-after-release");
  require(WaitForSingleObject(waitable, 0) == WAIT_TIMEOUT, "reduced-latency-retracts-extra-credit");
  std::printf("OWNED_D3D12_PRESENT_QUEUES_ROTATION_PASS buffer0=queue_a buffer1=queue_b blocked=1 released=1 "
              "ordered=1\n");

  // Plain ResizeBuffers returns every slot to the creation queue, so blocking
  // B no longer affects presentation. Three buffers also exercise count change.
  waitIdle(device.Get(), queueB.Get());
  check(swapchain->ResizeBuffers(3, 0, 0, DXGI_FORMAT_UNKNOWN, kFlags), "resize-plain");
  DXGI_SWAP_CHAIN_DESC1 plain{};
  check(swapchain->GetDesc1(&plain), "resize-plain-desc");
  require(plain.BufferCount == 3, "resize-plain-count");
  check(queueB->Wait(gate.Get(), 2), "block-b-again");
  for (UINT frame = 0; frame < 3; ++frame)
    require(presentCompletes(swapchain.Get(), waitable, kCompletionTimeoutMs), "plain-present-on-queue-a");
  require(swapchain->GetCurrentBackBufferIndex() == 0, "plain-rotation");
  check(gate->Signal(2), "release-b-again");
  waitIdle(device.Get(), queueB.Get());
  std::printf("OWNED_D3D12_PRESENT_QUEUES_PLAIN_RESIZE_PASS buffers=3 creation_queue=1\n");

  require(SetWindowPos(window.get(), nullptr, 0, 0, 200, 160,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "enlarge-for-axis-resize");
  RECT client{};
  require(GetClientRect(window.get(), &client), "current-client-size");
  require(client.right > client.left && client.bottom > client.top, "positive-client-axes");
  check(swapchain->ResizeBuffers(3, 40, 0, DXGI_FORMAT_UNKNOWN, kFlags), "resize-height-from-client");
  check(swapchain->GetDesc1(&plain), "width-only-resize-desc");
  require(plain.Width == 40 && plain.Height == UINT(client.bottom - client.top),
          "zero-height-preserves-explicit-width");
  check(swapchain->ResizeBuffers(3, 0, 28, DXGI_FORMAT_UNKNOWN, kFlags), "resize-width-from-client");
  check(swapchain->GetDesc1(&plain), "height-only-resize-desc");
  require(plain.Width == UINT(client.right - client.left) && plain.Height == 28,
          "zero-width-preserves-explicit-height");
  UINT sourceWidth = 0, sourceHeight = 0;
  check(swapchain->GetSourceSize(&sourceWidth, &sourceHeight), "default-source-size");
  require(sourceWidth == plain.Width && sourceHeight == plain.Height, "source-reset-after-resize");
  expectHr(swapchain->SetSourceSize(plain.Width + 1, plain.Height), E_INVALIDARG, "source-too-wide");
  expectHr(swapchain->SetSourceSize(0, plain.Height), E_INVALIDARG, "source-zero-width");
  check(swapchain->GetSourceSize(&sourceWidth, &sourceHeight), "source-after-reject");
  require(sourceWidth == plain.Width && sourceHeight == plain.Height, "source-rejection-preserves-state");
  const UINT cropWidth = plain.Width > 1 ? plain.Width / 2 : 1;
  const UINT cropHeight = plain.Height > 1 ? plain.Height / 2 : 1;
  check(swapchain->SetSourceSize(cropWidth, cropHeight), "source-crop");
  check(swapchain->GetSourceSize(&sourceWidth, &sourceHeight), "cropped-source-size");
  require(sourceWidth == cropWidth && sourceHeight == cropHeight, "source-crop-state");
  DXGI_RGBA background{.125f, .375f, .625f, 1.f}, queried{};
  check(swapchain->SetBackgroundColor(&background), "background-color");
  check(swapchain->GetBackgroundColor(&queried), "get-background-color");
  require(queried.r == background.r && queried.g == background.g && queried.b == background.b,
          "background-state");
  DXGI_RGBA invalidBackground{2.f, 0, 0, 1};
  expectHr(swapchain->SetBackgroundColor(&invalidBackground), E_INVALIDARG, "background-out-of-range");
  check(swapchain->GetBackgroundColor(&queried), "background-after-reject");
  require(queried.r == background.r, "background-rejection-preserves-state");
  check(swapchain->SetRotation(DXGI_MODE_ROTATION_ROTATE90), "rotation-90");
  DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
  check(swapchain->GetRotation(&rotation), "get-rotation-90");
  require(rotation == DXGI_MODE_ROTATION_ROTATE90, "rotation-state");
  expectHr(swapchain->SetRotation(DXGI_MODE_ROTATION_UNSPECIFIED), DXGI_ERROR_INVALID_CALL,
           "rotation-invalid");
  require(presentCompletes(swapchain.Get(), waitable, kCompletionTimeoutMs), "cropped-rotated-present");
  check(swapchain->SetRotation(DXGI_MODE_ROTATION_IDENTITY), "rotation-identity");
  check(swapchain->SetSourceSize(plain.Width, plain.Height), "source-full");
  std::puts("OWNED_D3D12_PRESENT_QUEUES_GEOMETRY_PASS getters=1 invalid_preserved=1 present=1");

  waitIdle(device.Get(), queueA.Get());
  CloseHandle(waitable);
  swapchain.Reset();
  base.Reset();

  ComPtr<IDXGIOutput> output;
  check(adapter->EnumOutputs(0, &output), "enumerate-restricted-output");
  check(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &desc, nullptr, output.Get(), &base),
        "restricted-swapchain");
  check(base.As(&swapchain), "restricted-swapchain3");
  restricted = nullptr;
  check(swapchain->GetRestrictToOutput(&restricted), "get-restricted-output");
  require(restricted == output.Get(), "restricted-output-identity");
  restricted->Release();
  output.Reset();
  restricted = nullptr;
  check(swapchain->GetRestrictToOutput(&restricted), "retained-restricted-output");
  require(restricted != nullptr, "restricted-output-retained");
  restricted->Release();
  HANDLE restrictedWaitable = swapchain->GetFrameLatencyWaitableObject();
  require(restrictedWaitable != nullptr, "restricted-waitable");
  require(WaitForSingleObject(restrictedWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "restricted-waitable-initial");
  require(presentCompletes(swapchain.Get(), restrictedWaitable, kCompletionTimeoutMs), "restricted-present");
  require(swapchain->GetCurrentBackBufferIndex() == 1, "restricted-rotation");
  check(swapchain->Present(0, DXGI_PRESENT_RESTRICT_TO_OUTPUT), "restricted-flag-present");
  require(WaitForSingleObject(restrictedWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          swapchain->GetCurrentBackBufferIndex() == 0, "restricted-flag-completion");
  UINT beforeNoSequence = 0, afterNoSequence = 0;
  check(swapchain->GetLastPresentCount(&beforeNoSequence), "before-no-sequence");
  check(swapchain->Present(0, DXGI_PRESENT_DO_NOT_SEQUENCE), "present-without-sequencing");
  require(WaitForSingleObject(restrictedWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "no-sequence-completion");
  check(swapchain->GetLastPresentCount(&afterNoSequence), "after-no-sequence");
  require(afterNoSequence == beforeNoSequence + 1 && swapchain->GetCurrentBackBufferIndex() == 0,
          "no-sequence-preserves-current-buffer");
  waitIdle(device.Get(), queueA.Get());
  CloseHandle(restrictedWaitable);
  std::puts("OWNED_D3D12_PRESENT_QUEUES_RESTRICT_PASS null=1 retained=1 present=1 clipped_present=1");
  swapchain.Reset();
  base.Reset();
  require(SetWindowPos(window.get(), nullptr, 0, 0, 160, 120, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE),
          "enlarge-window-client");

  check(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &desc, nullptr, nullptr, &base),
        "restart-swapchain");
  check(base.As(&swapchain), "restart-swapchain3");
  const LONG windowedStyle = GetWindowLongW(window.get(), GWL_STYLE);
  RECT windowedRect{};
  require(GetWindowRect(window.get(), &windowedRect), "restart-windowed-rect");
  DEVMODEW windowedMode{};
  windowedMode.dmSize = sizeof(windowedMode);
  require(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &windowedMode), "restart-windowed-mode");
  check(swapchain->SetFullscreenState(TRUE, nullptr), "restart-enter-fullscreen");
  check(swapchain->SetMaximumFrameLatency(2), "restart-two-frame-slots");
  HANDLE restartWaitable = swapchain->GetFrameLatencyWaitableObject();
  require(restartWaitable && WaitForSingleObject(restartWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          WaitForSingleObject(restartWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "restart-initial-credits");
  ComPtr<ID3D12Fence> restartStall;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&restartStall)), "restart-fence");
  check(queueA->Wait(restartStall.Get(), 1), "restart-block-queue");
  check(swapchain->Present(0, 0), "restart-queue-old-frame");
  check(swapchain->Present(0, DXGI_PRESENT_RESTART), "restart-discard-queued-frame");
  UINT restartCount = 0;
  check(swapchain->GetLastPresentCount(&restartCount), "restart-present-count");
  require(restartCount == 2 && swapchain->GetCurrentBackBufferIndex() == 0,
          "restart-accepts-new-frame-after-queue-discard");
  check(restartStall->Signal(1), "restart-release-queue");
  require(WaitForSingleObject(restartWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          WaitForSingleObject(restartWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "restart-canceled-and-new-frame-credits");
  waitIdle(device.Get(), queueA.Get());
  CloseHandle(restartWaitable);
  swapchain.Reset();
  base.Reset();
  RECT releasedRect{};
  DEVMODEW releasedMode{};
  releasedMode.dmSize = sizeof(releasedMode);
  const bool rectRestored = GetWindowRect(window.get(), &releasedRect) && EqualRect(&releasedRect, &windowedRect);
  const bool styleRestored = GetWindowLongW(window.get(), GWL_STYLE) == windowedStyle;
  const bool modeRestored = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &releasedMode) &&
                            releasedMode.dmPelsWidth == windowedMode.dmPelsWidth &&
                            releasedMode.dmPelsHeight == windowedMode.dmPelsHeight &&
                            releasedMode.dmDisplayFrequency == windowedMode.dmDisplayFrequency;
  if (!styleRestored || !rectRestored) {
    SetWindowLongW(window.get(), GWL_STYLE, windowedStyle);
    SetWindowPos(window.get(), nullptr, windowedRect.left, windowedRect.top,
                 windowedRect.right - windowedRect.left, windowedRect.bottom - windowedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
  if (!modeRestored)
    ChangeDisplaySettingsW(&windowedMode, 0);
  require(styleRestored && rectRestored && modeRestored, "fullscreen-release-restores-window-and-display");
  std::puts("OWNED_D3D12_PRESENT_QUEUES_RESTART_PASS queued_discard=1 credit=1 release_restores=1");

  // Fresh swapchain so the prior case's frames cannot confound the
  // queued-only max-one restart case.
  check(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &desc, nullptr, nullptr, &base),
        "restart-one-slot-swapchain");
  check(base.As(&swapchain), "restart-one-slot-swapchain3");
  check(swapchain->SetFullscreenState(TRUE, nullptr), "restart-one-slot-fullscreen");
  HANDLE oneSlotWaitable = swapchain->GetFrameLatencyWaitableObject();
  require(oneSlotWaitable && WaitForSingleObject(oneSlotWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "restart-one-slot-initial-credit");
  ComPtr<ID3D12Fence> oneSlotStall;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&oneSlotStall)), "restart-one-slot-fence");
  check(queueA->Wait(oneSlotStall.Get(), 1), "restart-one-slot-block-queue");
  check(swapchain->Present(0, 0), "restart-one-slot-old-frame");
  LARGE_INTEGER restartStart{}, restartStop{}, restartFrequency{};
  require(QueryPerformanceFrequency(&restartFrequency) && QueryPerformanceCounter(&restartStart),
          "restart-one-slot-timing-start");
  const HRESULT oneSlotRestart = swapchain->Present(0, DXGI_PRESENT_RESTART);
  require(QueryPerformanceCounter(&restartStop), "restart-one-slot-timing-stop");
  const auto restartMs = (restartStop.QuadPart - restartStart.QuadPart) * 1000 / restartFrequency.QuadPart;
  const DWORD beforeReleaseCredit = WaitForSingleObject(oneSlotWaitable, 0);
  std::fprintf(stderr, "RESTART_ONE_SLOT_RESULT hr=%08lx elapsed_ms=%lld credit=%lu\n",
               static_cast<unsigned long>(oneSlotRestart), static_cast<long long>(restartMs),
               static_cast<unsigned long>(beforeReleaseCredit));
  UINT oneSlotCount = 0;
  check(swapchain->GetLastPresentCount(&oneSlotCount), "restart-one-slot-count");
  const UINT oneSlotIndex = swapchain->GetCurrentBackBufferIndex();
  check(oneSlotStall->Signal(1), "restart-one-slot-release-queue");
  waitIdle(device.Get(), queueA.Get());
  const DWORD afterReleaseCredit = WaitForSingleObject(oneSlotWaitable, kCompletionTimeoutMs);
  const DWORD extraCredit = WaitForSingleObject(oneSlotWaitable, 0);
  CloseHandle(oneSlotWaitable);
  swapchain.Reset();
  base.Reset();
  RECT oneSlotReleasedRect{};
  DEVMODEW oneSlotReleasedMode{};
  oneSlotReleasedMode.dmSize = sizeof(oneSlotReleasedMode);
  const bool oneSlotRectRestored = GetWindowRect(window.get(), &oneSlotReleasedRect) &&
                                   EqualRect(&oneSlotReleasedRect, &windowedRect);
  const bool oneSlotStyleRestored = GetWindowLongW(window.get(), GWL_STYLE) == windowedStyle;
  const bool oneSlotModeRestored = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &oneSlotReleasedMode) &&
                                   oneSlotReleasedMode.dmPelsWidth == windowedMode.dmPelsWidth &&
                                   oneSlotReleasedMode.dmPelsHeight == windowedMode.dmPelsHeight &&
                                   oneSlotReleasedMode.dmDisplayFrequency == windowedMode.dmDisplayFrequency;
  if (!oneSlotStyleRestored || !oneSlotRectRestored) {
    SetWindowLongW(window.get(), GWL_STYLE, windowedStyle);
    SetWindowPos(window.get(), nullptr, windowedRect.left, windowedRect.top,
                 windowedRect.right - windowedRect.left, windowedRect.bottom - windowedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
  if (!oneSlotModeRestored)
    ChangeDisplaySettingsW(&windowedMode, 0);
  require(oneSlotRestart == S_OK && restartMs < kBlockedProbeMs && beforeReleaseCredit == WAIT_TIMEOUT &&
          oneSlotCount == 2 && oneSlotIndex == 0 && afterReleaseCredit == WAIT_OBJECT_0 &&
          extraCredit == WAIT_TIMEOUT && oneSlotStyleRestored && oneSlotRectRestored && oneSlotModeRestored,
          "restart-one-slot-discard-without-block-or-extra-credit");
  std::printf("OWNED_D3D12_PRESENT_QUEUES_RESTART_ONE_SLOT_PASS elapsed_ms=%lld credit=1\n",
              static_cast<long long>(restartMs));

  // Restart discards every frame not yet committed to Metal. Native present happens at commit
  // (as in D3DMetal 4.0b3), so a frame on another queue cannot commit before its blocked
  // predecessor: buffer 0 presents on blocked queue B, buffer 1 on idle queue A waits behind it,
  // and both are still queue-owned when the restart frame targets buffer 0 on B again.
  check(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &desc, nullptr, nullptr, &base),
        "restart-native-swapchain");
  check(base.As(&swapchain), "restart-native-swapchain3");
  check(swapchain->SetFullscreenState(TRUE, nullptr), "restart-native-fullscreen");
  IUnknown *restartQueues[2] = {queueB.Get(), queueA.Get()};
  check(swapchain->ResizeBuffers1(2, kWidth, kHeight, DXGI_FORMAT_UNKNOWN, kFlags, masks, restartQueues),
        "restart-native-resize1");
  check(swapchain->SetMaximumFrameLatency(2), "restart-native-two-frame-slots");
  HANDLE nativeWaitable = swapchain->GetFrameLatencyWaitableObject();
  require(nativeWaitable && WaitForSingleObject(nativeWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          WaitForSingleObject(nativeWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "restart-native-initial-credits");
  ComPtr<ID3D12Fence> nativeStall;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nativeStall)), "restart-native-fence");
  // Present ids restart at 1 per swapchain, so native OWNED_GRAPHICS_NATIVE_PRESENT_DECISION lines
  // for this case (ids 1..3) are only meaningful between these stderr markers.
  std::fputs("RESTART_NATIVE_BEGIN\n", stderr);
  std::fflush(stderr);
  check(queueB->Wait(nativeStall.Get(), 1), "restart-native-block-queue-b");
  check(swapchain->Present(0, 0), "restart-native-queued-frame");
  check(swapchain->Present(0, 0), "restart-native-encoded-frame");
  // Also gives queue A's worker time to encode and commit frame 2 before the restart.
  const DWORD nativeEarlyCredit = WaitForSingleObject(nativeWaitable, kBlockedProbeMs);
  LARGE_INTEGER nativeStart{}, nativeStop{}, nativeFrequency{};
  require(QueryPerformanceFrequency(&nativeFrequency) && QueryPerformanceCounter(&nativeStart),
          "restart-native-timing-start");
  const HRESULT nativeRestart = swapchain->Present(0, DXGI_PRESENT_RESTART);
  require(QueryPerformanceCounter(&nativeStop), "restart-native-timing-stop");
  const auto nativeMs = (nativeStop.QuadPart - nativeStart.QuadPart) * 1000 / nativeFrequency.QuadPart;
  const DWORD nativeBeforeRelease = WaitForSingleObject(nativeWaitable, 0);
  std::fprintf(stderr, "RESTART_NATIVE_RESULT hr=%08lx elapsed_ms=%lld credit=%lu\n",
               static_cast<unsigned long>(nativeRestart), static_cast<long long>(nativeMs),
               static_cast<unsigned long>(nativeBeforeRelease));
  UINT nativeCount = 0;
  check(swapchain->GetLastPresentCount(&nativeCount), "restart-native-count");
  const UINT nativeIndex = swapchain->GetCurrentBackBufferIndex();
  // Releasing B skips discarded frame 1; frame 2 then sees the advanced epoch and is skipped
  // too, so only the restart frame reaches the display.
  check(nativeStall->Signal(1), "restart-native-release-queue-b");
  waitIdle(device.Get(), queueB.Get());
  waitIdle(device.Get(), queueA.Get());
  // Frame 1's credit was transferred to the restart frame and frame 2's returned at restart;
  // only the restart frame returns one on GPU completion, and nothing else.
  const DWORD nativeRestartCredit = WaitForSingleObject(nativeWaitable, kCompletionTimeoutMs);
  // Give the native completion handlers (traced as OWNED_GRAPHICS_NATIVE_PRESENT_DECISION) time
  // to log before the swapchain and its layer are torn down.
  const ULONGLONG nativePresentedDeadline = GetTickCount64() + kBlockedProbeMs;
  while (GetTickCount64() < nativePresentedDeadline) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Sleep(0);
  }
  std::fputs("RESTART_NATIVE_END\n", stderr);
  std::fflush(stderr);
  const DWORD nativeExtraCredit = WaitForSingleObject(nativeWaitable, 0);
  CloseHandle(nativeWaitable);
  swapchain.Reset();
  base.Reset();
  RECT nativeReleasedRect{};
  DEVMODEW nativeReleasedMode{};
  nativeReleasedMode.dmSize = sizeof(nativeReleasedMode);
  const bool nativeRectRestored = GetWindowRect(window.get(), &nativeReleasedRect) &&
                                  EqualRect(&nativeReleasedRect, &windowedRect);
  const bool nativeStyleRestored = GetWindowLongW(window.get(), GWL_STYLE) == windowedStyle;
  const bool nativeModeRestored = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &nativeReleasedMode) &&
                                  nativeReleasedMode.dmPelsWidth == windowedMode.dmPelsWidth &&
                                  nativeReleasedMode.dmPelsHeight == windowedMode.dmPelsHeight &&
                                  nativeReleasedMode.dmDisplayFrequency == windowedMode.dmDisplayFrequency;
  if (!nativeStyleRestored || !nativeRectRestored) {
    SetWindowLongW(window.get(), GWL_STYLE, windowedStyle);
    SetWindowPos(window.get(), nullptr, windowedRect.left, windowedRect.top,
                 windowedRect.right - windowedRect.left, windowedRect.bottom - windowedRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
  if (!nativeModeRestored)
    ChangeDisplaySettingsW(&windowedMode, 0);
  require(nativeEarlyCredit == WAIT_TIMEOUT, "restart-native-encoded-frame-waits-on-queued-frame");
  require(nativeRestart == S_OK && nativeMs < kBlockedProbeMs && nativeBeforeRelease == WAIT_OBJECT_0 &&
          nativeCount == 3 && nativeIndex == 1 && nativeRestartCredit == WAIT_OBJECT_0 &&
          nativeExtraCredit == WAIT_TIMEOUT,
          "restart-native-discard-without-block-or-extra-credit");
  require(nativeStyleRestored && nativeRectRestored && nativeModeRestored,
          "restart-native-release-restores-window-and-display");
  std::puts("OWNED_D3D12_PRESENT_QUEUES_RESTART_NATIVE_PASS discarded=2 credit=1 presented=restart_only");

  desc.Width = 32;
  desc.Height = 24;
  desc.Scaling = DXGI_SCALING_NONE;
  check(factory->CreateSwapChainForHwnd(queueA.Get(), window.get(), &desc, nullptr, nullptr, &base),
        "unscaled-swapchain");
  check(base.As(&swapchain), "unscaled-swapchain3");
  UINT initialCount = ~UINT{0};
  check(swapchain->Present(0, DXGI_PRESENT_DO_NOT_SEQUENCE), "first-no-sequence-is-ignored");
  check(swapchain->GetLastPresentCount(&initialCount), "first-no-sequence-count");
  require(initialCount == 0 && swapchain->GetCurrentBackBufferIndex() == 0,
          "first-no-sequence-does-not-submit");
  expectHr(swapchain->Present(1, DXGI_PRESENT_ALLOW_TEARING), DXGI_ERROR_INVALID_CALL,
           "tearing-requires-creation-flag-and-sync-zero");
  expectHr(swapchain->Present(0, DXGI_PRESENT_RESTART), DXGI_ERROR_INVALID_CALL,
           "restart-requires-fullscreen");
  check(swapchain->SetSourceSize(16, 12), "unscaled-source-crop");
  check(swapchain->SetBackgroundColor(&background), "unscaled-background");
  check(swapchain->SetRotation(DXGI_MODE_ROTATION_ROTATE180), "unscaled-rotation");
  HANDLE unscaledWaitable = swapchain->GetFrameLatencyWaitableObject();
  require(unscaledWaitable && WaitForSingleObject(unscaledWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "unscaled-initial-credit");
  require(presentCompletes(swapchain.Get(), unscaledWaitable, kCompletionTimeoutMs), "unscaled-present");
  require(swapchain->GetCurrentBackBufferIndex() == 1, "unscaled-present-rotation");
  UINT beforeOcclusion = 0, afterOcclusion = 0;
  check(swapchain->GetLastPresentCount(&beforeOcclusion), "count-before-minimize");
  ShowWindow(window.get(), SW_MINIMIZE);
  expectHr(swapchain->Present(0, DXGI_PRESENT_TEST), DXGI_STATUS_OCCLUDED, "minimized-test-status");
  require(WaitForSingleObject(unscaledWaitable, 0) == WAIT_TIMEOUT,
          "present-test-does-not-credit-unsent-frame");
  expectHr(swapchain->Present(0, 0), DXGI_STATUS_OCCLUDED, "minimized-present-status");
  check(swapchain->GetLastPresentCount(&afterOcclusion), "count-after-minimize");
  require(afterOcclusion == beforeOcclusion && swapchain->GetCurrentBackBufferIndex() == 1,
          "occlusion-does-not-accept-frame");
  require(WaitForSingleObject(unscaledWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "occluded-frame-restores-waitable-credit");
  ShowWindow(window.get(), SW_SHOWNOACTIVATE);
  check(swapchain->Present(0, DXGI_PRESENT_TEST), "restored-window-test-status");
  require(presentCompletes(swapchain.Get(), unscaledWaitable, kCompletionTimeoutMs),
          "restored-window-presents");
  waitIdle(device.Get(), queueA.Get());
  std::puts("OWNED_D3D12_PRESENT_QUEUES_UNSCALED_PASS background=1 crop=1 rotation=1 present=1");

  // One slot remains free, but a layer resize would drain the prior queued
  // frame. DO_NOT_WAIT must reject without entering that GPU wait.
  ComPtr<ID3D12Fence> layerStall;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&layerStall)),
        "layer-resize-stall-fence");
  check(swapchain->SetMaximumFrameLatency(2), "layer-resize-two-slots");
  require(WaitForSingleObject(unscaledWaitable, 0) == WAIT_OBJECT_0, "layer-resize-extra-credit");
  check(queueA->Wait(layerStall.Get(), 1), "layer-resize-stall");
  check(swapchain->Present(0, 0), "layer-resize-first-present");
  UINT beforeNonblocking = 0, afterNonblocking = 0;
  check(swapchain->GetLastPresentCount(&beforeNonblocking), "before-nonblocking-resize");
  const UINT beforeNonblockingIndex = swapchain->GetCurrentBackBufferIndex();
  require(SetWindowPos(window.get(), nullptr, 0, 0, 180, 140,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "change-layer-size-while-blocked");
  LARGE_INTEGER start{}, stop{}, frequency{};
  require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start), "nonblocking-start");
  const HRESULT nonblocking = swapchain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
  require(QueryPerformanceCounter(&stop), "nonblocking-stop");
  check(swapchain->GetLastPresentCount(&afterNonblocking), "after-nonblocking-resize");
  require(nonblocking == DXGI_ERROR_WAS_STILL_DRAWING && afterNonblocking == beforeNonblocking &&
          swapchain->GetCurrentBackBufferIndex() == beforeNonblockingIndex &&
          (stop.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart < kBlockedProbeMs,
          "nonblocking-layer-transition-does-not-drain-gpu");
  check(layerStall->Signal(1), "release-layer-stall");
  require(WaitForSingleObject(unscaledWaitable, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "layer-resize-first-completion");
  check(swapchain->SetMaximumFrameLatency(1), "restore-layer-latency");
  std::puts("OWNED_D3D12_PRESENT_QUEUES_DO_NOT_WAIT_PASS free_slot=1 resize=1 nonblocking=1");
  // A removed device must wake both a Present blocked at the queue limit and
  // a client already waiting on the duplicated frame-latency handle.
  ComPtr<ID3D12Fence> stalled;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&stalled)), "removal-stall-fence");
  check(queueA->Wait(stalled.Get(), 1), "stall-present-for-removal");
  check(swapchain->Present(0, 0), "accept-stalled-present");
  PendingPresent pending{swapchain.Get(), CreateEventW(nullptr, FALSE, FALSE, nullptr),
            CreateEventW(nullptr, FALSE, FALSE, nullptr), E_FAIL};
  require(pending.started && pending.finished, "removal-thread-events");
  HANDLE presentThread = CreateThread(nullptr, 0, runPendingPresent, &pending, 0, nullptr);
  require(presentThread != nullptr, "removal-present-thread");
  require(WaitForSingleObject(pending.started, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          WaitForSingleObject(pending.finished, kBlockedProbeMs) == WAIT_TIMEOUT,
          "second-present-blocks-at-latency-limit");
  PendingWait publicWait{unscaledWaitable, CreateEventW(nullptr, FALSE, FALSE, nullptr),
               CreateEventW(nullptr, FALSE, FALSE, nullptr), WAIT_FAILED};
  require(publicWait.started && publicWait.finished, "public-wait-thread-events");
  HANDLE waitThread = CreateThread(nullptr, 0, runPendingWait, &publicWait, 0, nullptr);
  require(waitThread != nullptr, "public-wait-thread");
  require(WaitForSingleObject(publicWait.started, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          WaitForSingleObject(publicWait.finished, kBlockedProbeMs) == WAIT_TIMEOUT,
          "public-handle-stays-unsignaled-until-removal");
  ComPtr<ID3D12CommandAllocator> staleAllocator;
  ComPtr<ID3D12GraphicsCommandList> staleList;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&staleAllocator)),
        "removal-stale-allocator");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, staleAllocator.Get(), nullptr,
                                  IID_PPV_ARGS(&staleList)), "removal-stale-list");
  check(staleList->Close(), "removal-stale-close");
  check(staleAllocator->Reset(), "removal-stale-reset");
  ID3D12CommandList *invalid[] = {staleList.Get()};
  queueB->ExecuteCommandLists(1, invalid);
  require(device->GetDeviceRemovedReason() == DXGI_ERROR_DEVICE_REMOVED,
          "invalid-recording-removes-device");
  require(WaitForSingleObject(pending.finished, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          pending.result == DXGI_ERROR_DEVICE_REMOVED, "blocked-present-wakes-on-removal");
  require(WaitForSingleObject(publicWait.finished, kCompletionTimeoutMs) == WAIT_OBJECT_0 &&
          publicWait.result == WAIT_OBJECT_0, "public-latency-wait-wakes-on-removal");
  stalled->Signal(1); // A removed device may reject CPU signaling; the wait is canceled separately.
  require(WaitForSingleObject(presentThread, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "removal-thread-joined");
  require(WaitForSingleObject(waitThread, kCompletionTimeoutMs) == WAIT_OBJECT_0,
          "public-wait-thread-joined");
  CloseHandle(presentThread); CloseHandle(pending.started); CloseHandle(pending.finished);
  CloseHandle(waitThread); CloseHandle(publicWait.started); CloseHandle(publicWait.finished);
  std::puts("OWNED_D3D12_PRESENT_QUEUES_REMOVAL_PASS internal=1 public=1");
  CloseHandle(unscaledWaitable);
}

#include "owned-d3d12-present-history.inc"

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool history_only = argc == 2 && std::strcmp(argv[1], "--history-only") == 0;
  if (argc != 1 && !history_only)
    fail("arguments");
  requireOwnedBackend();
  if (history_only) {
    std::puts("OWNED_D3D12_PRESENT_HISTORY_BACKEND id=owned-graphics-d3d12");
    runPresentHistory();
    return 0;
  }
  run();
  std::puts("OWNED_D3D12_PRESENT_QUEUES_PASS");
  return 0;
}
