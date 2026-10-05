// Public ABI consumer: uses an actual WinRT CoreWindow, never a fake COM object.
// A desktop thread normally has no CoreWindow. A genuine CoreApplication-created
// UI thread is a separate prerequisite, not inferred from desktop activation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

using Microsoft::WRL::ComPtr;

// Public windows.ui.core.h ABI: IID 4d239005-3c2a-41b1-9022-536bb9cf93b1,
// IInspectable slots followed by GetForCurrentThread(ICoreWindow **). The full
// generated UI projection is not needed (mingw boolean/BYTE specializations clash).
// ICoreWindow inherits IInspectable; only its inherited identity methods are used.
namespace ABI::Windows::UI::Core {
struct ICoreWindow;
struct ICoreWindowStatic : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetForCurrentThread(ICoreWindow **window) = 0;
};
}
constexpr IID kCoreWindowStaticIID{0x4d239005, 0x3c2a, 0x41b1,
                                  {0x90, 0x22, 0x53, 0x6b, 0xb9, 0xcf, 0x93, 0xb1}};
namespace {
constexpr char kPrefix[] = "OWNED_D3D12_COREWINDOW_COMPOSITION";
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx\n", kPrefix, step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool value, const char *step) { if (!value) fail(step); }
void expect(HRESULT hr, HRESULT wanted, const char *step) { if (hr != wanted) fail(step, hr); }
void report(const char *step, HRESULT hr) {
  std::printf("%s_CONTRACT step=%s hr=0x%08lx\n", kPrefix, step, static_cast<unsigned long>(hr));
}

void present(ID3D12Device *device, ID3D12CommandQueue *queue, IDXGISwapChain4 *chain) {
  ComPtr<ID3D12Resource> buffer;
  check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)), "get-corewindow-buffer");
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12DescriptorHeap> rtv;
  ComPtr<ID3D12Fence> fence;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&list)), "list");
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = 1;
  check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv)), "rtv");
  device->CreateRenderTargetView(buffer.Get(), nullptr, rtv->GetCPUDescriptorHandleForHeapStart());
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = buffer.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  list->ResourceBarrier(1, &barrier);
  const float green[] = {0, 1, 0, 1};
  list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(), green, 0, nullptr);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  list->ResourceBarrier(1, &barrier);
  check(list->Close(), "list-close");
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  check(chain->Present(1, 0), "corewindow-present");
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  check(queue->Signal(fence.Get(), 1), "signal");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "fence-event");
  check(fence->SetEventOnCompletion(1, event), "fence-wait");
  const DWORD result = WaitForSingleObject(event, 10000);
  CloseHandle(event);
  require(result == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1, "gpu-complete");
}
} // namespace

int corewindow_main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool reference = argc == 2 && std::strcmp(argv[1], "--reference") == 0;
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory");
  check(factory->EnumAdapters1(0, &adapter), "adapter");
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  if (!reference) {
    using BackendId = const char *(WINAPI *)();
    auto backend = reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
    require(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned-backend");
    std::printf("%s_BACKEND id=owned-graphics-d3d12\n", kPrefix);
  }
  D3D12_COMMAND_QUEUE_DESC q{};
  q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = 64; desc.Height = 48;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  ComPtr<IDXGISwapChain1> base;
  HRESULT nullCore = factory->CreateSwapChainForCoreWindow(queue.Get(), nullptr, &desc, nullptr, &base);
  report("null-corewindow", nullCore);
  if (!reference) expect(nullCore, DXGI_ERROR_INVALID_CALL, "null-corewindow");
  HRESULT notCore = factory->CreateSwapChainForCoreWindow(queue.Get(), queue.Get(), &desc, nullptr, &base);
  report("non-corewindow-object", notCore);
  if (!reference) expect(notCore, DXGI_ERROR_INVALID_CALL, "non-corewindow-object");
  require(!base, "no-invalid-object-retained");

  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"OwnedCoreWindowCompositionContract";
  require(RegisterClassW(&cls) != 0, "window-class");
  HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"CoreWindow/Composition consumer", WS_OVERLAPPEDWINDOW,
                              40, 40, 160, 120, nullptr, nullptr, cls.hInstance, nullptr);
  require(hwnd != nullptr, "window");
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  check(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc, nullptr, nullptr, &base), "hwnd-chain");
  ComPtr<IDXGISwapChain4> chain;
  check(base.As(&chain), "hwnd-chain4");
  if (!reference) {
    ComPtr<IDXGISwapChain1> duplicate;
    expect(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc, nullptr, nullptr, &duplicate),
           DXGI_ERROR_INVALID_CALL, "single-native-layer-owner");
    require(!duplicate, "no-duplicate-native-owner");
  }
  ComPtr<IUnknown> identity;
  HRESULT noCore = chain->GetCoreWindow(IID_PPV_ARGS(&identity));
  report("hwnd-no-corewindow", noCore);
  if (!reference) expect(noCore, DXGI_ERROR_INVALID_CALL, "hwnd-no-corewindow");
  require(!identity, "no-corewindow-retained");
  DXGI_MATRIX_3X2_F matrix{2, 0, 0, 3, 5, 7}, unchanged = matrix;
  HRESULT setMatrix = chain->SetMatrixTransform(&matrix), getMatrix = chain->GetMatrixTransform(&matrix);
  report("hwnd-no-matrix", setMatrix);
  report("hwnd-no-matrix-get", getMatrix);
  if (!reference) {
    expect(setMatrix, DXGI_ERROR_INVALID_CALL, "hwnd-no-matrix");
    expect(getMatrix, DXGI_ERROR_INVALID_CALL, "hwnd-no-matrix-get");
  }
  require(std::memcmp(&matrix, &unchanged, sizeof(matrix)) == 0, "matrix-output-untouched");
  DXGI_HDR_METADATA_HDR10PLUS dynamic{};
  report("hdr10plus", chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10PLUS, sizeof(dynamic), &dynamic));
  if (!reference)
    expect(chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10PLUS, sizeof(dynamic), &dynamic),
           DXGI_ERROR_UNSUPPORTED, "reject-hdr10plus");
  ComPtr<IDXGIOutput6> output;
  ComPtr<IDXGIOutput> outputBase;
  check(chain->GetContainingOutput(&outputBase), "output");
  check(outputBase.As(&output), "output6");
  UINT hardware = ~0u, overlay = ~0u;
  check(output->CheckHardwareCompositionSupport(&hardware), "hardware-composition-query");
  check(output->CheckOverlaySupport(desc.Format, device.Get(), &overlay), "overlay-query");
  std::printf("%s_CAPS hardware=%u overlay=%u overlays=%u stereo=%u\n", kPrefix,
              hardware, overlay, output->SupportsOverlays(), factory->IsWindowedStereoEnabled());
  if (!reference)
    require(hardware == 0 && overlay == 0 && !output->SupportsOverlays() &&
            !factory->IsWindowedStereoEnabled(), "no-invented-plane-or-stereo-capability");
  chain.Reset(); base.Reset();
  desc.Stereo = TRUE;
  HRESULT stereo = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc, nullptr, nullptr, &base);
  report("stereo-swapchain", stereo);
  if (!reference) require(FAILED(stereo) && !base, "reject-stereo");
  base.Reset(); desc.Stereo = FALSE;

  HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
  ComPtr<ABI::Windows::UI::Core::ICoreWindowStatic> statics;
  ComPtr<IInspectable> core;
  HSTRING className = nullptr;
  constexpr wchar_t name[] = L"Windows.UI.Core.CoreWindow";
  HRESULT runtime = FAILED(initialized) ? initialized : WindowsCreateString(name, ARRAYSIZE(name) - 1, &className);
  if (SUCCEEDED(runtime))
    runtime = RoGetActivationFactory(className, kCoreWindowStaticIID,
                                     reinterpret_cast<void **>(statics.GetAddressOf()));
  if (SUCCEEDED(runtime)) {
    ABI::Windows::UI::Core::ICoreWindow *current = nullptr;
    runtime = statics->GetForCurrentThread(&current);
    core.Attach(reinterpret_cast<IInspectable *>(current));
  }
  WindowsDeleteString(className);
  report("actual-corewindow-runtime", runtime);
  if (SUCCEEDED(runtime) && core) {
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    check(factory->CreateSwapChainForCoreWindow(queue.Get(), core.Get(), &desc, nullptr, &base), "actual-corewindow-chain");
    check(base.As(&chain), "actual-corewindow-chain4");
    ComPtr<IUnknown> expected;
    check(core.As(&expected), "corewindow-identity");
    check(chain->GetCoreWindow(IID_PPV_ARGS(&identity)), "get-corewindow-identity");
    require(identity.Get() == expected.Get(), "same-corewindow-identity");
    HWND returned = reinterpret_cast<HWND>(1);
    expect(chain->GetHwnd(&returned), DXGI_ERROR_INVALID_CALL, "corewindow-not-hwnd-chain");
    require(returned == nullptr, "no-invented-hwnd");
    expect(chain->SetFullscreenState(TRUE, nullptr), DXGI_ERROR_INVALID_CALL, "corewindow-no-exclusive-mode");
    expect(chain->SetMatrixTransform(&matrix), DXGI_ERROR_INVALID_CALL, "corewindow-no-composition-matrix");
    HANDLE latency = chain->GetFrameLatencyWaitableObject();
    require(latency && WaitForSingleObject(latency, 10000) == WAIT_OBJECT_0, "corewindow-initial-pacing-credit");
    present(device.Get(), queue.Get(), chain.Get());
    require(WaitForSingleObject(latency, 10000) == WAIT_OBJECT_0, "corewindow-completed-pacing-credit");
    check(chain->ResizeBuffers(3, 32, 24, DXGI_FORMAT_UNKNOWN, desc.Flags), "corewindow-resize");
    DXGI_SWAP_CHAIN_DESC1 resized{};
    check(chain->GetDesc1(&resized), "corewindow-resized-desc");
    require(resized.Width == 32 && resized.Height == 24 && resized.BufferCount == 3, "corewindow-resized-buffers");
    present(device.Get(), queue.Get(), chain.Get());
    require(WaitForSingleObject(latency, 10000) == WAIT_OBJECT_0, "corewindow-resized-pacing-credit");
    CloseHandle(latency);
    chain.Reset(); base.Reset(); identity.Reset();
    std::printf("%s_COREWINDOW actual=1 gpu_present=1\n", kPrefix);
  } else {
    std::printf("%s_COREWINDOW actual=0 gpu_present=0\n", kPrefix);
    std::printf("%s_PREREQUISITE corewindow=coreapplication-ui-thread-window-required\n", kPrefix);
  }
  core.Reset(); statics.Reset();
  if (SUCCEEDED(initialized)) RoUninitialize();
  DestroyWindow(hwnd);
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
  std::printf("%s_REACHABLE_OK corewindow=separate-prerequisite\n", kPrefix);
  return 0;
}

#include "owned-d3d12-composition.inc"

int main(int argc, char **argv) {
  if (argc > 1 && std::strcmp(argv[1], "--composition") == 0)
    return composition_main(argc, argv);
  return corewindow_main(argc, argv);
}
