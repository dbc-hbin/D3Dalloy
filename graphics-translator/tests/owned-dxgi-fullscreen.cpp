#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kWidth = 80, kHeight = 60;
constexpr DWORD kWaitMs = 10000;
const char *api = "setup";

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "OWNED_DXGI_FULLSCREEN_FAIL api=%s step=%s hr=0x%08lx\n", api, step,
               static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool ok, const char *step) { if (!ok) fail(step); }
void expect(HRESULT got, HRESULT wanted, const char *step) {
  if (got != wanted) {
    std::fprintf(stderr, "OWNED_DXGI_FULLSCREEN_EXPECT api=%s step=%s wanted=0x%08lx got=0x%08lx\n",
                 api, step, static_cast<unsigned long>(wanted), static_cast<unsigned long>(got));
    fail(step, got);
  }
}

// This is a transparent output proxy except for one selected fault. Other methods
// really reach the adapter's output; no swapchain or output state is simulated.
class OutputFault final : public IDXGIOutput1 {
public:
  enum Fault { None, QueryOutput1, Description, ClosestMode };
  explicit OutputFault(IDXGIOutput1 *real) : real_(real) {}
  Fault fault = None;
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG refs = InterlockedDecrement(&refs_);
    if (!refs) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (fault == QueryOutput1 && IsEqualIID(iid, __uuidof(IDXGIOutput1))) return E_NOINTERFACE;
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IDXGIObject)) ||
        IsEqualIID(iid, __uuidof(IDXGIOutput)) || IsEqualIID(iid, __uuidof(IDXGIOutput1))) {
      *out = static_cast<IDXGIOutput1 *>(this);
      AddRef();
      return S_OK;
    }
    return real_->QueryInterface(iid, out);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a, UINT b, const void *c) override { return real_->SetPrivateData(a,b,c); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a, const IUnknown *b) override { return real_->SetPrivateDataInterface(a,b); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a, UINT *b, void *c) override { return real_->GetPrivateData(a,b,c); }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID a, void **b) override { return real_->GetParent(a,b); }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC *a) override {
    if (fault == Description) return E_FAIL;
    return real_->GetDesc(a);
  }
  HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT a, UINT b, UINT *c, DXGI_MODE_DESC *d) override {
    return real_->GetDisplayModeList(a,b,c,d);
  }
  HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC *a, DXGI_MODE_DESC *b, IUnknown *c) override {
    return real_->FindClosestMatchingMode(a,b,c);
  }
  HRESULT STDMETHODCALLTYPE WaitForVBlank() override { return real_->WaitForVBlank(); }
  HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown *a, BOOL b) override { return real_->TakeOwnership(a,b); }
  void STDMETHODCALLTYPE ReleaseOwnership() override { real_->ReleaseOwnership(); }
  HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES *a) override {
    return real_->GetGammaControlCapabilities(a);
  }
  HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL *a) override { return real_->SetGammaControl(a); }
  HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL *a) override { return real_->GetGammaControl(a); }
  HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface *a) override { return real_->SetDisplaySurface(a); }
  HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface *a) override { return real_->GetDisplaySurfaceData(a); }
  HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *a) override { return real_->GetFrameStatistics(a); }
  HRESULT STDMETHODCALLTYPE GetDisplayModeList1(DXGI_FORMAT a, UINT b, UINT *c, DXGI_MODE_DESC1 *d) override {
    return real_->GetDisplayModeList1(a,b,c,d);
  }
  HRESULT STDMETHODCALLTYPE FindClosestMatchingMode1(const DXGI_MODE_DESC1 *a, DXGI_MODE_DESC1 *b, IUnknown *c) override {
    if (fault == ClosestMode) return E_FAIL;
    return real_->FindClosestMatchingMode1(a,b,c);
  }
  HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData1(IDXGIResource *a) override {
    return real_->GetDisplaySurfaceData1(a);
  }
  HRESULT STDMETHODCALLTYPE DuplicateOutput(IUnknown *a, IDXGIOutputDuplication **b) override {
    return real_->DuplicateOutput(a,b);
  }
private:
  ~OutputFault() = default;
  LONG refs_ = 1;
  ComPtr<IDXGIOutput1> real_;
};

struct Window {
  HWND hwnd = nullptr;
  LONG_PTR style = 0;
  RECT rect{};
  IDXGISwapChain *reentrant = nullptr;
  HRESULT reentrantResult = E_FAIL;
  bool triggerReentry = false;
  bool destroyOnStyle = false;
  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto *self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == WM_STYLECHANGING) {
      if (self->triggerReentry && self->reentrant) {
        self->triggerReentry = false;
        self->reentrantResult = self->reentrant->SetFullscreenState(FALSE, nullptr);
      }
      if (self->destroyOnStyle) {
        self->destroyOnStyle = false;
        DestroyWindow(hwnd);
        self->hwnd = nullptr;
      }
    }
    return DefWindowProcW(hwnd, msg, w, l);
  }
  Window() {
    static bool registered = false;
    if (!registered) {
      WNDCLASSW cls{};
      cls.lpfnWndProc = proc;
      cls.hInstance = GetModuleHandleW(nullptr);
      cls.lpszClassName = L"OwnedDxgiFullscreenWindow";
      require(RegisterClassW(&cls) != 0, "RegisterClassW");
      registered = true;
    }
    hwnd = CreateWindowExW(0, L"OwnedDxgiFullscreenWindow", L"fullscreen-contract",
                           WS_OVERLAPPEDWINDOW, 90, 80, kWidth, kHeight, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    require(hwnd != nullptr, "CreateWindowExW");
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    require(GetWindowRect(hwnd, &rect), "initial-rect");
  }
  ~Window() { if (hwnd) DestroyWindow(hwnd); }
  void restored() const {
    require(hwnd && GetWindowLongPtrW(hwnd, GWL_STYLE) == style, "window-style-restored");
    RECT now{};
    require(GetWindowRect(hwnd, &now) && EqualRect(&now, &rect), "window-rect-restored");
  }
};

struct Snapshot {
  BOOL fullscreen = FALSE;
  ComPtr<IDXGIOutput> target;
  DXGI_SWAP_CHAIN_DESC desc{};
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
  ComPtr<IDXGIOutput> containing;
};
Snapshot snapshot(IDXGISwapChain1 *chain) {
  Snapshot s;
  check(chain->GetFullscreenState(&s.fullscreen, &s.target), "GetFullscreenState");
  check(chain->GetDesc(&s.desc), "GetDesc");
  check(chain->GetFullscreenDesc(&s.fullscreenDesc), "GetFullscreenDesc");
  check(chain->GetContainingOutput(&s.containing), "GetContainingOutput");
  return s;
}
void same(IDXGISwapChain1 *chain, const Snapshot &s, const char *step) {
  auto now = snapshot(chain);
  DXGI_OUTPUT_DESC beforeOutput{}, afterOutput{};
  check(s.containing->GetDesc(&beforeOutput), "original-containing-desc");
  check(now.containing->GetDesc(&afterOutput), "current-containing-desc");
  require(now.fullscreen == s.fullscreen && now.target.Get() == s.target.Get() &&
          now.desc.Windowed == s.desc.Windowed &&
          now.desc.BufferDesc.Width == s.desc.BufferDesc.Width &&
          now.desc.BufferDesc.Height == s.desc.BufferDesc.Height &&
          now.desc.BufferDesc.RefreshRate.Numerator == s.desc.BufferDesc.RefreshRate.Numerator &&
          now.desc.BufferDesc.RefreshRate.Denominator == s.desc.BufferDesc.RefreshRate.Denominator &&
          now.desc.BufferDesc.Format == s.desc.BufferDesc.Format &&
          now.desc.BufferDesc.ScanlineOrdering == s.desc.BufferDesc.ScanlineOrdering &&
          now.desc.BufferDesc.Scaling == s.desc.BufferDesc.Scaling &&
          now.desc.BufferCount == s.desc.BufferCount &&
          now.desc.BufferUsage == s.desc.BufferUsage &&
          now.desc.SwapEffect == s.desc.SwapEffect &&
          now.desc.Flags == s.desc.Flags && now.desc.OutputWindow == s.desc.OutputWindow &&
          now.fullscreenDesc.Windowed == s.fullscreenDesc.Windowed &&
          now.fullscreenDesc.RefreshRate.Numerator == s.fullscreenDesc.RefreshRate.Numerator &&
          now.fullscreenDesc.RefreshRate.Denominator == s.fullscreenDesc.RefreshRate.Denominator &&
          now.fullscreenDesc.ScanlineOrdering == s.fullscreenDesc.ScanlineOrdering &&
          now.fullscreenDesc.Scaling == s.fullscreenDesc.Scaling &&
          beforeOutput.Monitor == afterOutput.Monitor, step);
}

struct Backend {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  ComPtr<IDXGIOutput1> output1;
  ComPtr<ID3D12Device> device12;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context;
  Backend(bool d3d12) {
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
    check(adapter->EnumOutputs(0, &output), "EnumOutputs");
    check(output.As(&output1), "IDXGIOutput1");
    if (d3d12) {
      check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)), "D3D12CreateDevice");
      D3D12_COMMAND_QUEUE_DESC desc{};
      desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
      check(device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    } else {
      D3D_FEATURE_LEVEL level{};
      check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                              D3D11_SDK_VERSION, &device11, &level, &context), "D3D11CreateDevice");
      std::printf("OWNED_DXGI_FULLSCREEN_D3D11_DEVICE feature_level=0x%x\n", static_cast<unsigned>(level));
    }
  }
  ComPtr<IDXGISwapChain3> create12(Window &window, bool fullscreen = false) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = kWidth; desc.Height = kHeight;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    fullscreenDesc.Windowed = fullscreen ? FALSE : TRUE;
    ComPtr<IDXGISwapChain1> chain;
    check(factory->CreateSwapChainForHwnd(queue.Get(), window.hwnd, &desc,
                                          fullscreen ? &fullscreenDesc : nullptr, nullptr, &chain),
          "CreateSwapChainForHwnd12");
    ComPtr<IDXGISwapChain3> result;
    check(chain.As(&result), "IDXGISwapChain3");
    return result;
  }
  ComPtr<IDXGISwapChain1> create11(Window &window, bool fullscreen = false) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = kWidth; desc.Height = kHeight;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    fullscreenDesc.Windowed = fullscreen ? FALSE : TRUE;
    ComPtr<IDXGISwapChain1> chain;
    check(factory->CreateSwapChainForHwnd(device11.Get(), window.hwnd, &desc,
                                          fullscreen ? &fullscreenDesc : nullptr, nullptr, &chain),
          "CreateSwapChainForHwnd11");
    return chain;
  }
};

void frame12(Backend &b, IDXGISwapChain3 *chain) {
  UINT index = chain->GetCurrentBackBufferIndex();
  ComPtr<ID3D12Resource> buffer;
  check(chain->GetBuffer(index, IID_PPV_ARGS(&buffer)), "GetBuffer12");
  auto desc = buffer->GetDesc();
  require(desc.Width == 72 && desc.Height == 54, "resized-backbuffer12");
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(b.device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
  auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
  b.device12->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
  UINT64 total = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
  b.device12->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &total);
  D3D12_HEAP_PROPERTIES props{};
  props.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC readDesc{};
  readDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  readDesc.Width = total;
  readDesc.Height = 1;
  readDesc.DepthOrArraySize = 1;
  readDesc.MipLevels = 1;
  readDesc.SampleDesc.Count = 1;
  readDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  check(b.device12->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &readDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "CreateReadback12");
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  check(b.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateAllocator");
  check(b.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&list)), "CreateCommandList");
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = buffer.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  list->ResourceBarrier(1, &barrier);
  const FLOAT color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  list->ClearRenderTargetView(rtv, color, 0, nullptr);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
  src.pResource = buffer.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = layout;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  list->ResourceBarrier(1, &barrier);
  check(list->Close(), "CloseList");
  ID3D12CommandList *lists[] = {list.Get()};
  b.queue->ExecuteCommandLists(1, lists);
  ComPtr<ID3D12Fence> fence;
  check(b.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
  check(b.queue->Signal(fence.Get(), 1), "Signal");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "CreateEvent");
  check(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
  require(WaitForSingleObject(event, kWaitMs) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1,
          "GPU-fence12");
  CloseHandle(event);
  void *mapped = nullptr;
  D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
  check(readback->Map(0, &range, &mapped), "MapReadback12");
  const auto *p = static_cast<const unsigned char *>(mapped) + layout.Offset;
  for (UINT y = 0; y < 54; ++y)
    for (UINT x = 0; x < 72; ++x) {
      const auto *pixel = p + y * layout.Footprint.RowPitch + x * 4;
      require(pixel[0] == 64 && pixel[1] == 128 && pixel[2] == 191 && pixel[3] == 255,
              "backbuffer-data12");
    }
  D3D12_RANGE empty{0,0}; readback->Unmap(0, &empty);
  buffer.Reset();
  check(chain->Present(0, 0), "Present12");
  check(b.queue->Signal(fence.Get(), 2), "PresentSignal12");
  event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  require(event != nullptr, "PresentEvent12");
  check(fence->SetEventOnCompletion(2, event), "PresentCompletion12");
  require(WaitForSingleObject(event, kWaitMs) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 2,
          "PresentQueue12");
  CloseHandle(event);
}
void frame11(Backend &b, IDXGISwapChain1 *chain) {
  ComPtr<ID3D11Texture2D> buffer;
  check(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GetBuffer11");
  D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
  require(desc.Width == 72 && desc.Height == 54, "resized-backbuffer11");
  ComPtr<ID3D11RenderTargetView> rtv;
  check(b.device11->CreateRenderTargetView(buffer.Get(), nullptr, &rtv), "CreateRTV11");
  const FLOAT color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  b.context->ClearRenderTargetView(rtv.Get(), color);
  D3D11_TEXTURE2D_DESC staging = desc;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> copy;
  check(b.device11->CreateTexture2D(&staging, nullptr, &copy), "CreateStaging11");
  b.context->CopyResource(copy.Get(), buffer.Get());
  ComPtr<ID3D11Query> query;
  D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT, 0};
  check(b.device11->CreateQuery(&queryDesc, &query), "CreateQuery11");
  b.context->End(query.Get());
  b.context->Flush();
  // Query completion is the GPU oracle; bound any stalled driver.
  const ULONGLONG deadline = GetTickCount64() + kWaitMs;
  HRESULT queryResult = S_FALSE;
  while ((queryResult = b.context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
    require(GetTickCount64() < deadline, "GPU-query-timeout11");
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Sleep(0);
  }
  check(queryResult, "GPU-query11");
  D3D11_MAPPED_SUBRESOURCE mapped{};
  check(b.context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped), "MapReadback11");
  const auto *p = static_cast<const unsigned char *>(mapped.pData);
  for (UINT y = 0; y < 54; ++y)
    for (UINT x = 0; x < 72; ++x) {
      const auto *pixel = p + y * mapped.RowPitch + x * 4;
      require(pixel[0] == 64 && pixel[1] == 128 && pixel[2] == 191 && pixel[3] == 255,
              "backbuffer-data11");
    }
  b.context->Unmap(copy.Get(), 0);
  rtv.Reset(); buffer.Reset();
  check(chain->Present(0, 0), "Present11");
  std::puts("OWNED_DXGI_FULLSCREEN_FRAME api=d3d11 pixels=3888 exact=1 query=complete present=ok");
}

void run(bool d3d12) {
  api = d3d12 ? "d3d12" : "d3d11";
  Backend b(d3d12);
  Window window;
  ComPtr<IDXGISwapChain3> chain12;
  ComPtr<IDXGISwapChain1> chain11;
  if (d3d12) chain12 = b.create12(window);
  else chain11 = b.create11(window);
  IDXGISwapChain1 *chain = d3d12 ? static_cast<IDXGISwapChain1 *>(chain12.Get()) : chain11.Get();
  auto initial = snapshot(chain);
  require(!initial.fullscreen && initial.desc.Windowed && initial.fullscreenDesc.Windowed, "initial-windowed");
  if (d3d12) {
    DXGI_FRAME_STATISTICS stats{};
    expect(b.output->GetFrameStatistics(nullptr), E_INVALIDARG, "output-stats-null");
    expect(b.output->GetFrameStatistics(&stats), DXGI_ERROR_INVALID_CALL, "output-stats-windowed");
    // A real monitor wait must advance the caller clock; a success-only stub cannot.
    check(b.output->WaitForVBlank(), "vblank-prime");
    LARGE_INTEGER start{}, finish{}, frequency{};
    require(QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&start), "vblank-clock-start");
    for (UINT i = 0; i < 3; ++i)
      check(b.output->WaitForVBlank(), "vblank-measured");
    require(QueryPerformanceCounter(&finish) && finish.QuadPart - start.QuadPart > frequency.QuadPart / 1000,
            "vblank-not-stub");
    std::printf("OWNED_DXGI_OUTPUT_VBLANK waits=3 elapsed_qpc=%lld frequency=%lld\n",
                finish.QuadPart - start.QuadPart, frequency.QuadPart);
  }
  auto *proxy = new OutputFault(b.output1.Get());
  expect(chain->SetFullscreenState(FALSE, proxy), DXGI_ERROR_INVALID_CALL, "false-nonnull");
  same(chain, initial, "false-nonnull-preserved");
  proxy->fault = OutputFault::QueryOutput1;
  expect(chain->SetFullscreenState(TRUE, proxy), E_NOINTERFACE, "output-QI-error");
  same(chain, initial, "QI-preserved");
  proxy->fault = OutputFault::Description;
  expect(chain->SetFullscreenState(TRUE, proxy), E_FAIL, "output-GetDesc-error");
  same(chain, initial, "GetDesc-preserved");
  proxy->fault = OutputFault::ClosestMode;
  expect(chain->SetFullscreenState(TRUE, proxy), E_FAIL, "mode-selection-error");
  same(chain, initial, "mode-preserved");
  proxy->fault = OutputFault::None;
  DXGI_MODE_DESC mode{};
  mode.Width = 72; mode.Height = 54;
  mode.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  mode.RefreshRate.Numerator = 60; mode.RefreshRate.Denominator = 1;
  window.reentrant = chain;
  window.triggerReentry = true;
  check(chain->SetFullscreenState(TRUE, proxy), "enter-fullscreen");
  require(!window.triggerReentry, "style-change-reentered");
  expect(window.reentrantResult, DXGI_STATUS_MODE_CHANGE_IN_PROGRESS, "reentrant-status");
  auto entered = snapshot(chain);
  require(entered.fullscreen && !entered.desc.Windowed && !entered.fullscreenDesc.Windowed &&
          entered.target.Get() == proxy, "entered-target");
  RECT fullscreenRect{};
  require(GetWindowRect(window.hwnd, &fullscreenRect), "fullscreen-rect");
  const LONG_PTR fullscreenStyle = GetWindowLongPtrW(window.hwnd, GWL_STYLE);
  proxy->fault = OutputFault::ClosestMode;
  expect(chain->ResizeTarget(&mode), E_FAIL, "ResizeTarget-mode-error");
  same(chain, entered, "ResizeTarget-preserved");
  RECT failedRect{};
  require(GetWindowRect(window.hwnd, &failedRect) && EqualRect(&failedRect, &fullscreenRect) &&
          GetWindowLongPtrW(window.hwnd, GWL_STYLE) == fullscreenStyle,
          "ResizeTarget-native-preserved");
  proxy->fault = OutputFault::None;
  check(chain->ResizeTarget(&mode), "ResizeTarget-fullscreen");
  check(chain->ResizeBuffers(2, 72, 54, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers-fullscreen");
  if (d3d12) {
    frame12(b, chain12.Get());
    std::puts("OWNED_DXGI_FULLSCREEN_FRAME api=d3d12 pixels=3888 exact=1 fence=complete present=ok");
  } else {
    frame11(b, chain);
  }
  if (d3d12) {
    DXGI_FRAME_STATISTICS measured{};
    const ULONGLONG deadline = GetTickCount64() + kWaitMs;
    HRESULT hr;
    do {
      check(b.output->WaitForVBlank(), "stats-vblank");
      hr = b.output->GetFrameStatistics(&measured);
      require(hr == S_OK || hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT, "output-stats-result");
      require(GetTickCount64() < deadline, "output-stats-timeout");
    } while (hr != S_OK);
    require(measured.PresentCount && measured.PresentRefreshCount &&
            measured.SyncRefreshCount >= measured.PresentRefreshCount && measured.SyncQPCTime.QuadPart > 0 &&
            measured.SyncGPUTime.QuadPart == 0, "output-stats-real-presentation");
    const auto first = measured;
    check(b.output->WaitForVBlank(), "stats-next-vblank");
    check(b.output->WaitForVBlank(), "stats-next-vblank2");
    check(b.output->GetFrameStatistics(&measured), "stats-next");
    require(measured.PresentCount == first.PresentCount &&
            measured.PresentRefreshCount == first.PresentRefreshCount &&
            measured.SyncRefreshCount > first.SyncRefreshCount && measured.SyncQPCTime.QuadPart > first.SyncQPCTime.QuadPart,
            "output-stats-monitor-progress");
    std::printf("OWNED_DXGI_OUTPUT_STATISTICS present=%u present_refresh=%u sync_refresh=%u qpc=%lld\n",
                measured.PresentCount, measured.PresentRefreshCount, measured.SyncRefreshCount, measured.SyncQPCTime.QuadPart);
    check(chain->ResizeBuffers(2, 72, 54, DXGI_FORMAT_UNKNOWN, 0), "stats-resize");
    expect(b.output->GetFrameStatistics(&measured), DXGI_ERROR_FRAME_STATISTICS_DISJOINT, "stats-resize-disjoint");
  }
  check(chain->SetFullscreenState(FALSE, nullptr), "leave-fullscreen");
  if (d3d12) {
    DXGI_FRAME_STATISTICS stats{};
    expect(b.output->GetFrameStatistics(&stats), DXGI_ERROR_INVALID_CALL, "stats-leave-unregistered");
    std::puts("OWNED_DXGI_OUTPUT_STATISTICS_PASS native=1 resize_disjoint=1 leave_unregistered=1");
  }
  auto left = snapshot(chain);
  require(!left.fullscreen && left.desc.Windowed && left.fullscreenDesc.Windowed && !left.target,
          "left-windowed");
  window.restored();
  std::printf("OWNED_DXGI_FULLSCREEN_TRANSITION api=%s enter=1 resize=1 frame=1 leave=1 rect=restored\n", api);
  // A second cycle exercises saved window placement, not merely first-use setup.
  check(chain->SetFullscreenState(TRUE, proxy), "repeat-enter");
  check(chain->SetFullscreenState(FALSE, nullptr), "repeat-leave");
  window.restored();
  std::printf("OWNED_DXGI_FULLSCREEN_REPEAT api=%s cycles=2 restored=1\n", api);
  proxy->Release();

  // Losing the native HWND during the style mutation is a genuine WSI failure,
  // unlike a COM output fault. The swapchain must not publish fullscreen state.
  // GetContainingOutput requires a live HWND by contract; the other three
  // getters remain usable even after destruction.
  {
    Window doomed;
    ComPtr<IDXGISwapChain1> broken;
    if (d3d12) broken = b.create12(doomed);
    else broken = b.create11(doomed);
    const auto before = snapshot(broken.Get());
    doomed.destroyOnStyle = true;
    expect(broken->SetFullscreenState(TRUE, b.output.Get()),
           DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "native-enter-error");
    require(doomed.hwnd == nullptr, "native-window-destroyed");
    BOOL fullscreen = TRUE;
    ComPtr<IDXGIOutput> target;
    DXGI_SWAP_CHAIN_DESC desc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    check(broken->GetFullscreenState(&fullscreen, &target), "native-state-getter");
    check(broken->GetDesc(&desc), "native-desc-getter");
    check(broken->GetFullscreenDesc(&fullscreenDesc), "native-fullscreen-desc-getter");
    require(!fullscreen && !target && desc.Windowed == before.desc.Windowed &&
            fullscreenDesc.Windowed == before.fullscreenDesc.Windowed,
            "native-failure-preserved");
  }
  {
    Window doomed;
    ComPtr<IDXGISwapChain1> broken;
    if (d3d12) broken = b.create12(doomed);
    else broken = b.create11(doomed);
    check(broken->SetFullscreenState(TRUE, b.output.Get()), "native-leave-setup");
    const auto before = snapshot(broken.Get());
    require(before.fullscreen && before.target && !before.desc.Windowed &&
            !before.fullscreenDesc.Windowed, "native-leave-setup-state");
    doomed.destroyOnStyle = true;
    expect(broken->SetFullscreenState(FALSE, nullptr),
           DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "native-leave-error");
    require(doomed.hwnd == nullptr, "native-leave-window-destroyed");
    BOOL fullscreen = FALSE;
    ComPtr<IDXGIOutput> target;
    DXGI_SWAP_CHAIN_DESC desc{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    check(broken->GetFullscreenState(&fullscreen, &target), "native-leave-state-getter");
    check(broken->GetDesc(&desc), "native-leave-desc-getter");
    check(broken->GetFullscreenDesc(&fullscreenDesc), "native-leave-fullscreen-desc-getter");
    require(fullscreen && target.Get() == before.target.Get() &&
            desc.Windowed == before.desc.Windowed &&
            fullscreenDesc.Windowed == before.fullscreenDesc.Windowed &&
            fullscreenDesc.RefreshRate.Numerator == before.fullscreenDesc.RefreshRate.Numerator &&
            fullscreenDesc.RefreshRate.Denominator == before.fullscreenDesc.RefreshRate.Denominator,
            "native-leave-preserved");
  }
  std::printf("OWNED_DXGI_FULLSCREEN_ERRORS api=%s invalid=1 qi=1 desc=1 mode=1 "
              "resize=1 reentrant=1 native_enter=1 native_leave=1 preserved=1\n", api);

  {
    Window created;
    ComPtr<IDXGISwapChain1> fullscreen;
    if (d3d12) fullscreen = b.create12(created, true);
    else fullscreen = b.create11(created, true);
    auto active = snapshot(fullscreen.Get());
    require(active.fullscreen && !active.desc.Windowed && !active.fullscreenDesc.Windowed && active.target,
            "creation-fullscreen-state");
    require((GetWindowLongPtrW(created.hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) != WS_OVERLAPPEDWINDOW,
            "creation-native-style");
    check(fullscreen->SetFullscreenState(FALSE, nullptr), "creation-leave");
    created.restored();
  }
  std::printf("OWNED_DXGI_FULLSCREEN_CREATION api=%s native=1 windowed=1 rect=restored\n", api);

  Window invalid;
  HWND stale = invalid.hwnd;
  require(DestroyWindow(stale), "destroy-before-creation");
  invalid.hwnd = nullptr;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = kWidth; desc.Height = kHeight;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = d3d12 ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
  desc.Scaling = DXGI_SCALING_STRETCH;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
  fullscreenDesc.Windowed = FALSE;
  ComPtr<IDXGISwapChain1> rejected;
  HRESULT invalidHr = b.factory->CreateSwapChainForHwnd(
      d3d12 ? static_cast<IUnknown *>(b.queue.Get()) : static_cast<IUnknown *>(b.device11.Get()),
      stale, &desc, &fullscreenDesc, nullptr, &rejected);
  require(FAILED(invalidHr) && !rejected, "invalid-window-creation-rejected");
  std::printf("OWNED_DXGI_FULLSCREEN_INVALID_CREATION api=%s rejected=1 null_output=1\n", api);
}

void removedStatistics() {
  api = "d3d12";
  Backend b(true);
  Window window;
  auto chain = b.create12(window, true);
  check(chain->ResizeBuffers(2, 72, 54, DXGI_FORMAT_UNKNOWN, 0), "removal-stats-resize");
  frame12(b, chain.Get());
  DXGI_FRAME_STATISTICS chainStats{}, outputStats{};
  const ULONGLONG deadline = GetTickCount64() + kWaitMs;
  HRESULT chainResult, outputResult;
  do {
    check(b.output->WaitForVBlank(), "removal-stats-vblank");
    chainResult = chain->GetFrameStatistics(&chainStats);
    outputResult = b.output->GetFrameStatistics(&outputStats);
    require(chainResult == S_OK || chainResult == DXGI_ERROR_FRAME_STATISTICS_DISJOINT,
            "removal-stats-chain-result");
    require(outputResult == S_OK || outputResult == DXGI_ERROR_FRAME_STATISTICS_DISJOINT,
            "removal-stats-output-result");
    require(GetTickCount64() < deadline, "removal-stats-timeout");
  } while (chainResult != S_OK || outputResult != S_OK);
  require(chainStats.PresentCount && outputStats.PresentCount, "removal-stats-presented");
  // frame12 retired the queue and both observers saw its presentation. No
  // pending Present can invalidate statistics on the removal callback's behalf.
  ComPtr<ID3D12Device5> device5;
  check(b.device12.As(&device5), "removal-stats-device5");
  device5->RemoveDevice();
  chainResult = chain->GetFrameStatistics(&chainStats);
  outputResult = b.output->GetFrameStatistics(&outputStats);
  check(chain->SetFullscreenState(FALSE, nullptr), "removal-stats-leave");
  window.restored();
  expect(chainResult, DXGI_ERROR_FRAME_STATISTICS_DISJOINT, "removed-chain-stats-disjoint");
  expect(outputResult, DXGI_ERROR_FRAME_STATISTICS_DISJOINT, "removed-output-stats-disjoint");
  std::puts("OWNED_DXGI_REMOVAL_STATISTICS_PASS presented=1 pending=0 swapchain=disjoint output=disjoint");
}

void backend() {
  HMODULE module = LoadLibraryW(L"d3d12.dll");
  require(module != nullptr, "d3d12-module");
  using Id = const char *(WINAPI *)();
  auto id = reinterpret_cast<Id>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  require(id && id() && std::strcmp(id(), "owned-graphics-d3d12") == 0, "owned-backend");
  std::printf("OWNED_DXGI_FULLSCREEN_BACKEND id=%s\n", id());
}
}
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool d3d12Only = argc == 2 && std::strcmp(argv[1], "--d3d12-only") == 0;
  const bool removalOnly = argc == 2 && std::strcmp(argv[1], "--removal-statistics-only") == 0;
  require(argc == 1 || d3d12Only || removalOnly, "arguments");
  backend();
  if (removalOnly) {
    removedStatistics();
    return 0;
  }
  run(true);
  removedStatistics();
  if (!d3d12Only)
    run(false);
  std::puts("OWNED_DXGI_FULLSCREEN_PASS");
  return 0;
}
