#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "com/com_guid.hpp"
#include "log/log.hpp"
#include "util_hotpatch.h"
#include "util_string.hpp"
#include "wsi_window.hpp"
#include "Metal.hpp"
#include "../d3d12/d3d12_shared_metadata.hpp"
#include "owned_budget_notification.hpp"
#include <algorithm>
#include <array>
#include <vector>

namespace dxmt {

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config);

class MTLDXGIFactory : public MTLDXGIObject<IDXGIFactory7> {

public:
  MTLDXGIFactory(UINT Flags) : flags_(Flags) {
    WMTDeviceCatalogCall call = {WMTDeviceCatalogCreate, 0, 0, 0};
    WMTDeviceCatalog_call(&call);
    catalog_ = WMT::Reference<WMT::Object>(call.result);
    if (!catalog_)
      throw std::bad_alloc();
    initial_generation_ = ReadGeneration();
    last_generation_ = initial_generation_;
    devices_ = WMT::CopyAllDevices();
    std::vector<uint64_t> hints;
    hints.reserve(devices_.count());
    for (UINT i = 0; i < devices_.count(); ++i) {
      call = {WMTDeviceCatalogHints, 0, devices_.object(i), 0};
      WMTDeviceCatalog_call(&call);
      hints.push_back(call.result);
      for (auto &order : adapter_order_)
        order.push_back(i);
    }
    // Native low-power classification is the power/performance preference;
    // unified memory is not a substitute (Apple discrete/UMA is orthogonal).
    for (UINT preference = 0; preference < adapter_order_.size(); ++preference) {
      auto &order = adapter_order_[preference];
      std::stable_sort(order.begin(), order.end(), [&](UINT a, UINT b) {
        const bool low_a = hints[a] & WMTDeviceLowPower;
        const bool low_b = hints[b] & WMTDeviceLowPower;
        return preference == DXGI_GPU_PREFERENCE_MINIMUM_POWER ? low_a > low_b : low_a < low_b;
      });
    }
  }

  ~MTLDXGIFactory() {
    {
      std::lock_guard<dxmt::mutex> lock(notification_mutex_);
      stopping_ = true;
      notification_wake_.notify_all();
    }
    if (notification_worker_.joinable())
      notification_worker_.join();
    for (const auto &[cookie, registration] : registrations_)
      if (registration.event)
        CloseHandle(registration.event);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
        riid == __uuidof(IDXGIFactory2) ||
        riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
        riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
        riid == __uuidof(IDXGIFactory7)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIFactory2), riid)) {
      WARN("DXGIFactory: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    InitReturnPtr(ppParent);

    WARN("DXGIFactory::GetParent: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() final {
    // We don't support Stereo 3D at the moment
    return FALSE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    ERR("Software adapters not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  DXMT_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
                  IDXGISwapChain **ppSwapChain) final {
    if (ppSwapChain == nullptr || pDesc == nullptr || pDevice == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width = pDesc->BufferDesc.Width;
    desc.Height = pDesc->BufferDesc.Height;
    desc.Format = pDesc->BufferDesc.Format;
    desc.Stereo = FALSE;
    desc.SampleDesc = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = pDesc->SwapEffect;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = pDesc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling = pDesc->BufferDesc.Scaling;
    descFs.Windowed = pDesc->Windowed;

    IDXGISwapChain1 *swapChain = nullptr;
    HRESULT hr = CreateSwapChainForHwnd(pDevice, pDesc->OutputWindow, &desc,
                                        &descFs, nullptr, &swapChain);

    *ppSwapChain = swapChain;
    return hr;
  }

  DXMT_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
      IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    if (!ppSwapChain || !pDesc || !hWnd || !pDevice)
      return DXGI_ERROR_INVALID_CALL;
    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;

    Com<IMTLSwapChainFactory> swapchain_factory;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&swapchain_factory)))) {
      ERR("Unsupported device type");
      return DXGI_ERROR_UNSUPPORTED;
    }

    // Make sure the back buffer size is not zero
    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;

    wsi::getWindowSize(hWnd, desc.Width ? nullptr : &desc.Width,
                       desc.Height ? nullptr : &desc.Height);

    // If necessary, set up a default set of
    // fullscreen parameters for the swap chain
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc;

    if (pFullscreenDesc) {
      fsDesc = *pFullscreenDesc;
    } else {
      fsDesc.RefreshRate = {0, 0};
      fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
      fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
      fsDesc.Windowed = TRUE;
    }

    Com<IMTLD3D12SwapChainFactory> d3d12_factory;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&d3d12_factory))))
      return d3d12_factory->CreateSwapChainWithOutput(this, hWnd, &desc, &fsDesc,
                                                       pRestrictToOutput, ppSwapChain, nullptr, DXMTSwapChainKind::Hwnd);
    return swapchain_factory->CreateSwapChain(this, hWnd, &desc, &fsDesc, ppSwapChain);
  }

  DXMT_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
      IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);
    if (!pDevice || !pWindow || !pDesc || !ppSwapChain)
      return DXGI_ERROR_INVALID_CALL;
    Com<IMTLD3D12SwapChainFactory> swapchain_factory;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&swapchain_factory))))
      return DXGI_ERROR_UNSUPPORTED;
    Com<IUnknown> core_window;
    Com<DXMTCoreWindowInterop> interop;
    if (FAILED(pWindow->QueryInterface(DXMT_CORE_WINDOW_IID, reinterpret_cast<void **>(&core_window))) ||
        FAILED(pWindow->QueryInterface(IID_PPV_ARGS(&interop))))
      return DXGI_ERROR_INVALID_CALL;
    HWND window = nullptr;
    HRESULT hr = interop->get_WindowHandle(&window);
    if (FAILED(hr))
      return hr;
    if (!window || !wsi::isWindow(window) || pDesc->Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH ||
        (pDesc->Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH))
      return DXGI_ERROR_INVALID_CALL;
    // Attach to the CoreWindow's own native window. Never manufacture a hidden HWND.
    return swapchain_factory->CreateSwapChainWithOutput(this, window, pDesc, nullptr,
                                                       pRestrictToOutput, ppSwapChain, pWindow, DXMTSwapChainKind::CoreWindow);
  }

  DXMT_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
      IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);
    if (!pDevice || !pDesc || !ppSwapChain)
      return DXGI_ERROR_INVALID_CALL;
    Com<IMTLD3D12SwapChainFactory> swapchain_factory;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&swapchain_factory))))
      return DXGI_ERROR_UNSUPPORTED;
    return swapchain_factory->CreateSwapChainWithOutput(this, nullptr, pDesc, nullptr,
        pRestrictToOutput, ppSwapChain, nullptr, DXMTSwapChainKind::Composition);
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter,
                                         IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    IDXGIAdapter1 *handle = nullptr;
    HRESULT hr = this->EnumAdapters1(Adapter, &handle);
    *ppAdapter = handle;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter,
                                          IDXGIAdapter1 **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (!ppAdapter)
      return DXGI_ERROR_INVALID_CALL;
    if (Adapter >= adapter_order_[0].size())
      return DXGI_ERROR_NOT_FOUND;
    *ppAdapter = CreateAdapter(devices_.object(adapter_order_[0][Adapter]), this, Config::getInstance());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) final {
    if (pWindowHandle == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pWindowHandle = associated_window_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource,
                                                         LUID *pLuid) final {
    try {
      return sharing::GetSharedAdapterLuid(hResource, pLuid);
    } catch (const std::bad_alloc &) {
      if (pLuid)
        *pLuid = {};
      return E_OUTOFMEMORY;
    }
  }

  HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle,
                                                  UINT Flags) final {
    if (Flags) {
      WARN("MakeWindowAssociation: Ignoring flags ", Flags);
    }
    associated_window_ = WindowHandle;
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE IsCurrent() final { return ReadGeneration() == initial_generation_; }

  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(
      HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) final {
    return RegisterNotification(NotificationKind::Occlusion, nullptr, WindowHandle, wMsg, pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent,
                                                      DWORD *pdwCookie) final {
    return RegisterNotification(NotificationKind::Stereo, hEvent, nullptr, 0, pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle,
                                                       UINT wMsg,
                                                       DWORD *pdwCookie) final {
    return RegisterNotification(NotificationKind::Stereo, nullptr, WindowHandle, wMsg, pdwCookie);
  }

  HRESULT STDMETHODCALLTYPE
  RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) final {
    return RegisterNotification(NotificationKind::Occlusion, hEvent, nullptr, 0, pdwCookie);
  }

  void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) final {
    UnregisterNotification(NotificationKind::Stereo, dwCookie);
  }

  void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) final {
    UnregisterNotification(NotificationKind::Occlusion, dwCookie);
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

  HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid,
                                              void **ppAdapter) override {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    for (UINT index = 0;; index++) {
      Com<IDXGIAdapter1> adapter;
      HRESULT hr = this->EnumAdapters1(index, &adapter);
      if (FAILED(hr))
        return hr;

      DXGI_ADAPTER_DESC desc;
      hr = adapter->GetDesc(&desc);
      if (FAILED(hr))
        return hr;
      if (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart)
        return adapter->QueryInterface(riid, ppAdapter);
    }
  }

  HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid,
                                            void **adapter) override {
    InitReturnPtr(adapter);
    if (!adapter)
      return DXGI_ERROR_INVALID_CALL;
    // No software renderer is installed. Hardware is never advertised as WARP.
    return DXGI_ERROR_NOT_FOUND;
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    case DXGI_FEATURE_PRESENT_ALLOW_TEARING: {
      auto info = static_cast<BOOL *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return E_INVALIDARG;

      *info = TRUE;
      return S_OK;
    }
    default: {
      ERR("DXGIFactory::CheckFeatureSupport: unknown feature ", Feature);
      return E_INVALIDARG;
    }
    }
  };

  HRESULT STDMETHODCALLTYPE
  EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                             REFIID riid, void **ppvAdapter) override {
    InitReturnPtr(ppvAdapter);
    if (!ppvAdapter || GpuPreference < DXGI_GPU_PREFERENCE_UNSPECIFIED ||
        GpuPreference > DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE)
      return DXGI_ERROR_INVALID_CALL;
    const auto &order = adapter_order_[GpuPreference];
    if (Adapter >= order.size())
      return DXGI_ERROR_NOT_FOUND;
    auto adapter = CreateAdapter(devices_.object(order[Adapter]), this, Config::getInstance());
    return adapter->QueryInterface(riid, ppvAdapter);
  };

  HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE event, DWORD *cookie) override {
    return RegisterNotification(NotificationKind::Adapters, event, nullptr, 0, cookie);
  }

  HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override {
    UnregisterNotification(NotificationKind::Adapters, cookie);
    return S_OK;
  }

private:
  enum class NotificationKind { Occlusion, Stereo, Adapters };
  struct Registration {
    NotificationKind kind;
    HANDLE event;
    HWND window;
    UINT message;
  };

  uint64_t ReadGeneration() const {
    WMTDeviceCatalogCall call = {WMTDeviceCatalogGeneration, 0, catalog_, 0};
    WMTDeviceCatalog_call(&call);
    return call.result;
  }

  static BOOL CALLBACK FindVisibleWindow(HWND window, LPARAM context) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    RECT area = {};
    if (process == GetCurrentProcessId() && IsWindowVisible(window) && !IsIconic(window) &&
        GetClientRect(window, &area) && area.right > area.left && area.bottom > area.top) {
      *reinterpret_cast<bool *>(context) = true;
      return FALSE;
    }
    return TRUE;
  }

  void ObserveNotifications() {
    // Match the visibility/minimized contract used by Present(TEST). A
    // notification is process-wide, not tied to the recipient HWND.
    bool visible = false;
    EnumWindows(FindVisibleWindow, reinterpret_cast<LPARAM>(&visible));
    const bool occluded = !visible;
    const uint64_t generation = ReadGeneration();
    for (const auto &[cookie, registration] : registrations_) {
      const bool changed = registration.kind == NotificationKind::Adapters
                               ? generation != last_generation_
                               : registration.kind == NotificationKind::Occlusion &&
                                     have_occlusion_ && occluded != last_occluded_;
      if (!changed)
        continue;
      if (registration.event)
        SetEvent(registration.event);
      else
        PostMessageW(registration.window, registration.message, 0, 0);
    }
    last_generation_ = generation;
    last_occluded_ = occluded;
    have_occlusion_ = true;
  }

  HRESULT RegisterNotification(NotificationKind kind, HANDLE event, HWND window, UINT message, DWORD *cookie) {
    if (!cookie)
      return DXGI_ERROR_INVALID_CALL;
    *cookie = 0;
    struct OwnedEvent {
      HANDLE handle = nullptr;
      ~OwnedEvent() { if (handle) CloseHandle(handle); }
    } duplicate;
    using CompareObjects = LONG(NTAPI *)(HANDLE, HANDLE);
    static const auto compare_objects = reinterpret_cast<CompareObjects>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCompareObjects")));
    if (window) {
      DWORD process = 0;
      if (!IsWindow(window) || !message || !GetWindowThreadProcessId(window, &process) ||
          process != GetCurrentProcessId())
        return DXGI_ERROR_INVALID_CALL;
    } else {
      if (!event || event == INVALID_HANDLE_VALUE ||
          !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &duplicate.handle,
                           0, FALSE, DUPLICATE_SAME_ACCESS))
        return DXGI_ERROR_INVALID_CALL;
      if (FAILED(DxgiBudgetNotifications::ValidateEvent(duplicate.handle)))
        return DXGI_ERROR_INVALID_CALL;
      if (!compare_objects)
        return E_FAIL;
    }
    std::lock_guard<dxmt::mutex> lock(notification_mutex_);
    for (const auto &[key, registration] : registrations_)
      if (registration.kind == kind &&
          ((event && registration.event && compare_objects(registration.event, duplicate.handle) == 0) ||
           (window && registration.window == window && registration.message == message)))
        return DXGI_ERROR_INVALID_CALL;
    if (registrations_.size() >= (std::numeric_limits<DWORD>::max)())
      return E_OUTOFMEMORY;
    DWORD candidate = next_cookie_;
    while (!candidate || registrations_.contains(candidate))
      ++candidate;
    ObserveNotifications();
    try {
      // Compare retained kernel objects, not original numeric handle values:
      // the caller may close its handle and Windows may recycle that value.
      registrations_.emplace(candidate, Registration{kind, duplicate.handle, window, message});
      if (kind != NotificationKind::Stereo && !notification_worker_.joinable())
        notification_worker_ = dxmt::thread([this] {
          std::unique_lock<dxmt::mutex> worker_lock(notification_mutex_);
          while (!stopping_) {
            if (registrations_.empty())
              notification_wake_.wait(worker_lock);
            else
              // ponytail: bounded 100ms visibility/generation sampling; use
              // native wake delivery if hotplug latency becomes observable.
              notification_wake_.wait_for(worker_lock, std::chrono::milliseconds(100));
            if (!stopping_)
              ObserveNotifications();
          }
        });
    } catch (const std::bad_alloc &) {
      registrations_.erase(candidate);
      return E_OUTOFMEMORY;
    } catch (const MTLD3DError &) {
      registrations_.erase(candidate);
      return E_FAIL;
    }
    duplicate.handle = nullptr;
    next_cookie_ = candidate + 1;
    *cookie = candidate;
    notification_wake_.notify_all();
    return S_OK;
  }

  bool UnregisterNotification(NotificationKind kind, DWORD cookie) {
    // Dispatch holds this lock through SetEvent/PostMessage, so unregister
    // returns only after any in-flight delivery. Already posted messages
    // remain in the caller queue, as on Windows.
    std::lock_guard<dxmt::mutex> lock(notification_mutex_);
    auto found = registrations_.find(cookie);
    if (found == registrations_.end() || found->second.kind != kind)
      return false;
    if (found->second.event)
      CloseHandle(found->second.event);
    registrations_.erase(found);
    notification_wake_.notify_all();
    return true;
  }

  WMT::Reference<WMT::Object> catalog_;
  WMT::Reference<WMT::Array<WMT::Device>> devices_;
  std::array<std::vector<UINT>, 3> adapter_order_;
  uint64_t initial_generation_ = 0;
  uint64_t last_generation_ = 0;
  dxmt::mutex notification_mutex_;
  dxmt::condition_variable notification_wake_;
  dxmt::thread notification_worker_;
  std::map<DWORD, Registration> registrations_;
  DWORD next_cookie_ = 1;
  bool stopping_ = false;
  bool have_occlusion_ = false;
  bool last_occluded_ = true;
  UINT flags_;

  HWND associated_window_ = nullptr;
};

extern "C" HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid,
                                                void **ppFactory) {
  try {
    MTLDXGIFactory* factory = new MTLDXGIFactory(Flags);
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    factory->Release();

    if (FAILED(hr))
      return hr;

    return S_OK;
  } catch (const MTLD3DError &e) {
    Logger::err(e.message());
    return E_FAIL;
  } catch (const std::bad_alloc &) {
    if (ppFactory)
      *ppFactory = nullptr;
    return E_OUTOFMEMORY;
  }
}

extern "C" HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  return CreateDXGIFactory2(0, riid, ppFactory);
}

extern "C" HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **factory) {
  return CreateDXGIFactory2(0, riid, factory);
}

} // namespace dxmt