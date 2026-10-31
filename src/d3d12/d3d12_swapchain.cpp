/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12_device.hpp"
#include "d3d12_frame_generation_interfaces.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_composition.h"
#include "dxgi_object.hpp"
#include "dxgi_output.hpp"
#include "dxmt_hud_state.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_monitor_format.hpp"
#include "dxmt_queue_order.hpp"
#include "dxmt_info.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "wsi_window.hpp"
#include "util_hotpatch.h"
#include "config/config.hpp"
#include <cassert>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

/**
Ref: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
This value is 1 by default.
*/
constexpr size_t kSwapchainLatency = 1;

/**
TODO: some implementations can be shared with d3d11 swapchain
*/

namespace dxmt {

struct NativeViewUnavailable {};
struct NativeViewAlreadyAttached {};

DXMTPresentLatency::~DXMTPresentLatency() {
  if (wake_semaphore_)
    CloseHandle(wake_semaphore_);
}

HRESULT
DXMTPresentLatency::initializeWakeHandle(HANDLE semaphore) {
  if (!DuplicateHandle(GetCurrentProcess(), semaphore, GetCurrentProcess(), &wake_semaphore_, 0,
                       FALSE, DUPLICATE_SAME_ACCESS))
    return HRESULT_FROM_WIN32(GetLastError());
  return S_OK;
}

bool
DXMTPresentLatency::reserve(bool do_not_wait, UINT replaceable_queued, bool *borrowed) {
  std::unique_lock lock(mutex_, std::defer_lock);
  if (do_not_wait) {
    if (!lock.try_lock() || removed_ || next_ticket_ != serving_ticket_ ||
        outstanding_ >= maximum_ + replaceable_queued)
      return false;
  } else {
    lock.lock();
    const uint64_t ticket = next_ticket_++;
    cv_.wait(lock, [&] {
      return removed_ || (ticket == serving_ticket_ && outstanding_ < maximum_ + replaceable_queued);
    });
    if (removed_)
      return false;
    ++serving_ticket_;
  }
  if (borrowed)
    *borrowed = outstanding_ >= maximum_;
  ++outstanding_;
  cv_.notify_all();
  return true;
}

void
DXMTPresentLatency::cancel() {
  {
    std::lock_guard lock(mutex_);
    assert(outstanding_);
    --outstanding_;
  }
  cv_.notify_all();
}

void
DXMTPresentLatency::complete(HANDLE semaphore) {
  {
    std::lock_guard lock(mutex_);
    assert(outstanding_);
    --outstanding_;
    if (credit_debt_)
      --credit_debt_;
    else
      ReleaseSemaphore(semaphore, 1, nullptr);
  }
  cv_.notify_all();
}

void
DXMTPresentLatency::discardQueuedForRestart(UINT canceled, bool borrowed, HANDLE semaphore) {
  {
    std::lock_guard lock(mutex_);
    assert(canceled <= outstanding_);
    // The replacement may have been admitted one slot above the limit. Its
    // canceled predecessor transfers that slot directly; only additional
    // canceled frames return public waitable credits.
    assert(!borrowed || canceled);
    outstanding_ -= canceled;
    for (UINT i = borrowed ? 1u : 0u; i < canceled; ++i) {
      if (credit_debt_)
        --credit_debt_;
      else
        ReleaseSemaphore(semaphore, 1, nullptr);
    }
  }
  cv_.notify_all();
}

void
DXMTPresentLatency::remove() {
  {
    std::lock_guard lock(mutex_);
    if (removed_)
      return;
    removed_ = true;
    // A caller may already be waiting on its duplicated frame-latency handle,
    // not just on the internal admission condition variable.
    for (UINT i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; ++i)
      if (!ReleaseSemaphore(wake_semaphore_, 1, nullptr))
        break;
  }
  cv_.notify_all();
}

HRESULT
DXMTPresentLatency::setMaximum(UINT maximum, HANDLE semaphore) {
  std::lock_guard lock(mutex_);
  if (removed_)
    return DXGI_ERROR_DEVICE_REMOVED;
  if (maximum < maximum_) {
    for (UINT count = maximum_; count > maximum; --count) {
      const DWORD result = WaitForSingleObject(semaphore, 0);
      if (result == WAIT_TIMEOUT)
        ++credit_debt_;
      else if (result != WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32(GetLastError());
    }
  } else if (maximum > maximum_) {
    UINT added = maximum - maximum_;
    const UINT canceled_debt = std::min(credit_debt_, added);
    credit_debt_ -= canceled_debt;
    added -= canceled_debt;
    while (added && ReleaseSemaphore(semaphore, 1, nullptr))
      --added;
    if (added && GetLastError() != ERROR_TOO_MANY_POSTS)
      return HRESULT_FROM_WIN32(GetLastError());
  }
  maximum_ = maximum;
  cv_.notify_all();
  return S_OK;
}

UINT
DXMTPresentLatency::maximum() const {
  std::lock_guard lock(mutex_);
  return maximum_;
}

WMTPixelFormat
ConvertSwapChainFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTPixelFormatRGBA16Float;
  default:
    return WMTPixelFormatInvalid;
  }
}

WMTColorSpace
ConvertColorSpace(DXGI_COLOR_SPACE_TYPE color_space, bool hdr) {
  switch (color_space) {
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    return WMTColorSpaceSRGB;
  case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    return hdr ? WMTColorSpaceHDR_scRGB : WMTColorSpaceSRGBLinear;
  case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    return WMTColorSpaceHDR_PQ;
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
    return WMTColorSpaceBT2020;
  default:
    return WMTColorSpaceInvalid;
  }
}

class ModeSetGuard {
  std::atomic_flag in_progress_;
public:
  class ModeSetInProgress {
    ModeSetGuard &guard_;
    bool protected_;

  public:
    ModeSetInProgress(ModeSetGuard &guard) : guard_(guard) {
      protected_ = guard_.in_progress_.test_and_set();
    }
    ~ModeSetInProgress() {
      if (!protected_) {
        guard_.in_progress_.clear();
      }
    }
    operator bool() {
      return protected_;
    }
  };
};

class MTLD3D12SwapChain final : public MTLDXGISubObject<IDXGISwapChain4, MTLD3D12Device>,
                              public IMTLDXGICompositionSurface,
                              public IMTLD3D12GeneratedPresent {

  Com<IDXGIFactory1> factory_;
  Com<MTLD3D12CommandQueue> queue_;
  const uint64_t owned_id_ = owned_graphics::NewObjectId();
  HWND hWnd;
  const DXMTSwapChainKind kind_;
  WMT::Reference<WMT::Object> composition_surface_;
  DXGI_MATRIX_3X2_F matrix_ = {1, 0, 0, 1, 0, 0};
  static inline dxmt::mutex view_owners_mutex_;
  static inline MTLD3D12SwapChain *view_owners_ = nullptr;
  MTLD3D12SwapChain *next_view_owner_ = nullptr;
  HMONITOR monitor_;
  Com<IDXGIOutput1> target_;
  Com<IDXGIOutput> restrict_output_;
  Com<IUnknown> core_window_;
  wsi::DXMTWindowState window_state_;
  ULONG presentation_count_ = 0;
  uint64_t next_presentation_id_ = 0;
  WMT::Reference<WMT::PresentationObserver> presentation_observer_;
  std::shared_ptr<std::atomic<bool>> presentation_observer_fault_;
  std::thread presentation_observer_thread_;
  WMTPresentationEvent last_presentation_event_{};
  bool has_presentation_event_ = false;
  bool statistics_disjoint_ = true;
  uint64_t earliest_presentation_id_ = 1;
  dxmt::RefreshClockBasis statistics_clock_{};
  DXGI_SWAP_CHAIN_DESC1 desc_;
  UINT source_width_ = 0, source_height_ = 0;
  UINT drawable_width_ = 0, drawable_height_ = 0;
  DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
  DXGI_RGBA background_ = {0, 0, 0, 1};
  D3D12_RESOURCE_DESC backbuffer_desc_;
  DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc_{};
  HANDLE present_semaphore_ = nullptr;
  std::shared_ptr<DXMTPresentLatency> latency_ = std::make_shared<DXMTPresentLatency>();
  std::shared_ptr<owned_graphics::FenceOrderEpoch> removal_epoch_;
  WMT::Object native_view_;
  WMT::MetalLayer layer_weak_;
  Rc<Presenter> presenter;
  DXGI_COLOR_SPACE_TYPE colorspace_;
  WMTColorSpace pending_colorspace_ = WMTColorSpaceInvalid;
  float scale_factor = 1.0;
  HUDState hud;
  double init_refresh_rate_ = 0; // Unknown until an actual display cadence is available.
  int preferred_max_frame_rate = 0;
  ModeSetGuard modeset_guard_;
  dxmt::mutex mutex_;

  std::vector<Com<MTLD3D12Resource>> backbuffers_;
  std::shared_ptr<DXMTPresentHistory> present_history_;
  uint64_t history_presents_ = 0;
  uint64_t resize_generation_ = 0;
  // One present queue per backbuffer; Present rotates both with the same index.
  std::vector<Com<MTLD3D12CommandQueue>> present_queues_;
  UINT current_buffer_ = 0;

  void
  ResetFrameStatisticsLocked() {
    statistics_disjoint_ = true;
    has_presentation_event_ = false;
    earliest_presentation_id_ = next_presentation_id_ + 1;
    statistics_clock_ = {};
  }

  bool
  LayerSupportEDR() {
    WMTEDRValue edr_value;
    MetalLayer_getEDRValue(layer_weak_, &edr_value);
    return edr_value.maximum_potential_edr_color_component_value > 1.0f;
  };

  InternalCommandLibrary lib;

public:
  MTLD3D12SwapChain(
      IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
      const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGIOutput *pRestrictToOutput, IUnknown *core_window, DXMTSwapChainKind kind
  ) :
      MTLDXGISubObject(pDevice),
      factory_(pFactory),
      queue_(pQueue),
      hWnd(hWnd),
      kind_(kind),
      monitor_(kind == DXMTSwapChainKind::Composition ? nullptr : wsi::getWindowMonitor(hWnd)),
      restrict_output_(pRestrictToOutput),
      core_window_(core_window),
      desc_(*pDesc),
      colorspace_(pDesc->Format == DXGI_FORMAT_R16G16B16A16_FLOAT
                      ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                      : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709),
      hud(WMT::DeveloperHUDProperties::instance()),
      lib(pDevice->GetMTLDevice()) {
    // Wine returns the same unretained Metal view for an already-attached HWND.
    // One flip chain per native layer avoids sharing and double-releasing it.
    // ponytail: linear active-chain scan; add indexing only if creation scale warrants it.
    std::unique_lock view_owner_lock(view_owners_mutex_, std::defer_lock);
    if (kind_ != DXMTSwapChainKind::Composition) {
      view_owner_lock.lock();
      for (auto *owner = view_owners_; owner; owner = owner->next_view_owner_)
        if (owner->hWnd == hWnd)
          throw NativeViewAlreadyAttached{};
    }

    if (kind_ == DXMTSwapChainKind::Composition) {
      obj_handle_t layer = 0;
      composition_surface_ = WMT::Reference<WMT::Object>(
          WMTCompositionCreateSurface(pDevice->GetMTLDevice().handle,
                                      desc_.AlphaMode == DXGI_ALPHA_MODE_PREMULTIPLIED, &layer));
      layer_weak_ = WMT::MetalLayer(layer);
    } else {
      native_view_ = WMT::CreateMetalViewFromHWND((intptr_t)hWnd, pDevice->GetMTLDevice(), layer_weak_);
    }
    struct NativeGuard {
      WMT::Object view;
      HANDLE &semaphore;
      bool active = true;
      ~NativeGuard() {
        if (active) {
          if (semaphore) CloseHandle(semaphore);
          if (view) WMT::ReleaseMetalView(view);
        }
      }
    } native_guard{native_view_, present_semaphore_};

    if ((!native_view_ && !composition_surface_) || !layer_weak_.handle) {
      ERR("Failed to create metal view, it seems like your Wine has no exported symbols needed by DXMT.");
      throw NativeViewUnavailable{};
    }

    presenter = Rc(new Presenter(pDevice->GetMTLDevice(), layer_weak_, lib, scale_factor,
                                 desc_.SampleDesc.Count, pDevice->GetMetal4Compiler(),
                                 kind_ == DXMTSwapChainKind::Composition));

    present_semaphore_ = CreateSemaphore(nullptr, kSwapchainLatency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      latency_->setMaximum(3, present_semaphore_);

    if (desc_.Width == 0 || desc_.Height == 0) {
      wsi::getWindowSize(hWnd, &desc_.Width, &desc_.Height);
    }
    source_width_ = desc_.Width;
    source_height_ = desc_.Height;

    if (pFullscreenDesc) {
      fullscreen_desc_ = *pFullscreenDesc;
      fullscreen_desc_.Windowed = TRUE;
    } else {
      fullscreen_desc_.Windowed = true;
    }

    preferred_max_frame_rate = Config::getInstance().getOption<int>("d3d12.preferredMaxFrameRate", 0);
    wsi::WsiMode current_mode;
    if (monitor_ && wsi::getCurrentDisplayMode(monitor_, &current_mode) && current_mode.refreshRate.denominator != 0 &&
        current_mode.refreshRate.numerator != 0) {
      init_refresh_rate_ = (double)current_mode.refreshRate.numerator / (double)current_mode.refreshRate.denominator;
    }

    hud.initialize(GetVersionDescriptionText(12, device_->GetFeatureLevel()));

    backbuffer_desc_ = D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
        .Width = desc_.Width,
        .Height = desc_.Height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = desc_.Format,
        .SampleDesc =
            {
                .Count = 1,
                .Quality = 0,
            },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    };
    if (desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      backbuffer_desc_.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    native_guard.active = false;
    if (kind_ != DXMTSwapChainKind::Composition) {
      next_view_owner_ = view_owners_;
      view_owners_ = this;
    }
  }

  ~MTLD3D12SwapChain() {
    // Composition has no HWND-owned layer to dismantle. Accepted presents
    // independently own their surface, presenter, resource, latency state and
    // duplicated completion handle; Release must not wait on a caller's queue gate.
    if (kind_ != DXMTSwapChainKind::Composition) {
      presenter->waitPresentsCompleted();
      dxmt::UnregisterOutputStatistics(monitor_, this);
    }
    if (presentation_observer_) {
      if (composition_surface_)
        WMTCompositionSetObserver(composition_surface_.handle, 0);
      presentation_observer_.close();
      if (presentation_observer_thread_.joinable())
        presentation_observer_thread_.join();
    }
    if (!fullscreen_desc_.Windowed && FAILED(LeaveFullscreenMode()))
      ERR("DXGI: Failed to restore fullscreen window and display mode on swapchain release");
    if (kind_ != DXMTSwapChainKind::Composition) {
      std::lock_guard view_owner_lock(view_owners_mutex_);
      auto **owner = &view_owners_;
      while (*owner != this)
        owner = &(*owner)->next_view_owner_;
      if (native_view_) WMT::ReleaseMetalView(native_view_);
      *owner = next_view_owner_;
    }
    native_view_ = {};
    CloseHandle(present_semaphore_);
  };

  HRESULT InitializePresentationObserver() {
    if (!present_semaphore_)
      return E_OUTOFMEMORY;
    presentation_observer_ = WMT::Reference<WMT::PresentationObserver>(WMTPresentationObserver_create());
    if (!presentation_observer_)
      return E_OUTOFMEMORY;
    try {
      presentation_observer_fault_ = std::make_shared<std::atomic<bool>>(false);
      auto observer = presentation_observer_;
      presentation_observer_thread_ = std::thread([this, observer = std::move(observer)] {
        WMTPresentationEvent event{};
        int32_t status;
        while ((status = observer.wait(event)) == 1) {
          if (event.outcome != WMTPresentationPresented || event.presented_host_seconds <= 0)
            continue;
          std::lock_guard<dxmt::mutex> lock(mutex_);
          if (event.present_id >= earliest_presentation_id_ &&
              (!has_presentation_event_ || event.present_id > last_presentation_event_.present_id)) {
            last_presentation_event_ = event;
            has_presentation_event_ = true;
            if (!fullscreen_desc_.Windowed)
              dxmt::PublishOutputStatistics(monitor_, this, event);
          }
        }
        if (status < 0) {
          presentation_observer_fault_->store(true, std::memory_order_release);
          std::lock_guard<dxmt::mutex> lock(mutex_);
          dxmt::InvalidateOutputStatistics(monitor_, this);
        }
      });
    } catch (const std::system_error &) {
      presentation_observer_.close();
      presentation_observer_ = nullptr;
      return E_OUTOFMEMORY;
    } catch (const std::bad_alloc &) {
      presentation_observer_.close();
      presentation_observer_ = nullptr;
      return E_OUTOFMEMORY;
    }
    if (composition_surface_)
      WMTCompositionSetObserver(composition_surface_.handle, presentation_observer_.handle);
    else
      presentation_observer_.setView(native_view_.handle);
    return S_OK;
  }

  HRESULT InitializeLatencyRemoval() {
    HRESULT hr = latency_->initializeWakeHandle(present_semaphore_);
    if (FAILED(hr))
      return hr;
    try {
      removal_epoch_ = std::make_shared<owned_graphics::FenceOrderEpoch>(0);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    if (!removal_epoch_->addCompletionWaiter(UINT64_MAX,
        [weak = std::weak_ptr(latency_), fault = std::weak_ptr(presentation_observer_fault_)] {
          if (auto latency = weak.lock())
            latency->remove();
          if (auto state = fault.lock())
            state->store(true, std::memory_order_release);
        }) || !device_->RegisterFenceOrder(removal_epoch_))
      return E_OUTOFMEMORY;
    return device_->RemovedReason() == S_OK ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
        riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4)) {
      *ppvObject = static_cast<IDXGISwapChain4 *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == IID_IMTLDXGICompositionSurface && composition_surface_) {
      *ppvObject = static_cast<IMTLDXGICompositionSurface *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IMTLD3D12GeneratedPresent)) {
      *ppvObject = static_cast<IMTLD3D12GeneratedPresent *>(this);
      AddRef();
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGISwapChain1), riid)) {
      WARN("DXGISwapChain: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  ULONG STDMETHODCALLTYPE AddRef() final {
    return MTLDXGISubObject<IDXGISwapChain4, MTLD3D12Device>::AddRef();
  }

  ULONG STDMETHODCALLTYPE Release() final {
    return MTLDXGISubObject<IDXGISwapChain4, MTLD3D12Device>::Release();
  }

  HRESULT STDMETHODCALLTYPE GetCompositionSurface(uint64_t *surface) final {
    if (!surface) return E_POINTER;
    *surface = composition_surface_.handle;
    return composition_surface_ ? S_OK : E_NOINTERFACE;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetParent(REFIID riid, void **parent) final {
    return factory_->QueryInterface(riid, parent);
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  Present(UINT sync_interval, UINT flags) final {
    return Present1(sync_interval, flags, nullptr);
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) final {
    InitReturnPtr(ppSurface);
    if (!ppSurface)
      return E_POINTER;
    if (Buffer >= backbuffers_.size())
      return DXGI_ERROR_NOT_FOUND;
    return backbuffers_[Buffer]->QueryInterface(riid, ppSurface);
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) final {
    if (kind_ != DXMTSwapChainKind::Hwnd)
      return DXGI_ERROR_INVALID_CALL;
    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    if (!Fullscreen && pTarget)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIOutput1> target;

    if (pTarget) {
      DXGI_OUTPUT_DESC desc;

      HRESULT hr = pTarget->QueryInterface(IID_PPV_ARGS(&target));
      if (FAILED(hr))
        return hr;
      hr = target->GetDesc(&desc);
      if (FAILED(hr))
        return hr;

      if (!fullscreen_desc_.Windowed && Fullscreen && monitor_ != desc.Monitor) {
        const auto old_target = target_;
        const auto old_monitor = monitor_;
        const auto old_window_state = window_state_;
        const auto old_refresh_rate = init_refresh_rate_;
        wsi::WsiMode old_mode = {};
        if (!wsi::getCurrentDisplayMode(old_monitor, &old_mode))
          return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

        hr = LeaveFullscreenMode();
        if (FAILED(hr))
          return hr;
        hr = EnterFullscreenMode(target.ptr());
        if (FAILED(hr)) {
          if (FAILED(EnterFullscreenMode(old_target.ptr())))
            ERR("DXGI: Failed to restore previous fullscreen output");
          if (!wsi::setWindowMode(old_monitor, hWnd, old_mode))
            ERR("DXGI: Failed to restore previous display mode");
          window_state_ = old_window_state;
          init_refresh_rate_ = old_refresh_rate;
          monitor_ = old_monitor;
          target_ = old_target;
          fullscreen_desc_.Windowed = FALSE;
          dxmt::RegisterOutputStatistics(monitor_, this, presentation_observer_, presentation_observer_fault_);
        }
        return hr;
      }
    }

    if (fullscreen_desc_.Windowed && Fullscreen)
      return EnterFullscreenMode(target.ptr());
    else if (!fullscreen_desc_.Windowed && !Fullscreen)
      return LeaveFullscreenMode();

    return S_OK;
  };

  HRESULT
  EnterFullscreenMode(IDXGIOutput1 *pTarget) {
    Com<IDXGIOutput1> output = pTarget;

    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    if (output == nullptr) {
      if (FAILED(GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output))) {
        ERR("DXGI: EnterFullscreenMode: Cannot query containing output");
        return E_FAIL;
      }
    }

    DXGI_OUTPUT_DESC output_desc = {};
    HRESULT hr = output->GetDesc(&output_desc);
    if (FAILED(hr))
      return hr;
    wsi::WsiMode old_mode = {};
    if (!wsi::getCurrentDisplayMode(output_desc.Monitor, &old_mode))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    hr = dxmt::RegisterOutputStatistics(output_desc.Monitor, this, presentation_observer_, presentation_observer_fault_);
    if (FAILED(hr))
      return hr;
    const double old_refresh_rate = init_refresh_rate_;

    DXGI_MODE_DESC1 preferred_display_mode = {
        desc_.Width,
        desc_.Height,
        fullscreen_desc_.RefreshRate,
        desc_.Format,
        DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED,
        DXGI_MODE_SCALING_UNSPECIFIED
    };
    hr = ChangeDisplayMode(output.ptr(), &preferred_display_mode);
    if (FAILED(hr)) {
      ERR("DXGI: EnterFullscreenMode: Failed to change display mode");
      dxmt::UnregisterOutputStatistics(output_desc.Monitor, this);
      if (!wsi::setWindowMode(output_desc.Monitor, hWnd, old_mode))
        ERR("DXGI: EnterFullscreenMode: Failed to restore display mode");
      init_refresh_rate_ = old_refresh_rate;
      return hr;
    }

    bool modeSwitch = (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0u;
    if (!wsi::enterFullscreenMode(output_desc.Monitor, hWnd, &window_state_, modeSwitch)) {
      ERR("DXGI: EnterFullscreenMode: Failed to enter fullscreen mode");
      dxmt::UnregisterOutputStatistics(output_desc.Monitor, this);
      if (!wsi::setWindowMode(output_desc.Monitor, hWnd, old_mode))
        ERR("DXGI: EnterFullscreenMode: Failed to restore display mode");
      init_refresh_rate_ = old_refresh_rate;
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    presentation_observer_.setView(native_view_.handle);
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      monitor_ = output_desc.Monitor;
      target_ = std::move(output);
      fullscreen_desc_.Windowed = FALSE;
      dxmt::InvalidateOutputStatistics(monitor_, this);
      ResetFrameStatisticsLocked();
    }
    return S_OK;
  }

  HRESULT
  LeaveFullscreenMode() {
    wsi::WsiMode old_mode = {};
    if (!wsi::getCurrentDisplayMode(monitor_, &old_mode))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    const double old_refresh_rate = init_refresh_rate_;
    HRESULT hr = RestoreDisplayMode(monitor_);
    if (FAILED(hr))
      return hr;

    if (wsi::isWindow(hWnd) && !wsi::leaveFullscreenMode(hWnd, &window_state_, true)) {
      ERR("DXGI: LeaveFullscreenMode: Failed to exit fullscreen mode");
      if (!wsi::setWindowMode(monitor_, hWnd, old_mode))
        ERR("DXGI: LeaveFullscreenMode: Failed to restore fullscreen display mode");
      init_refresh_rate_ = old_refresh_rate;
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }

    presenter->changeGammaRamp(nullptr);
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      dxmt::UnregisterOutputStatistics(monitor_, this);
      fullscreen_desc_.Windowed = TRUE;
      target_ = nullptr;
      monitor_ = wsi::getWindowMonitor(hWnd);
      ResetFrameStatisticsLocked();
    }

    return S_OK;
  }

  HRESULT
  ChangeDisplayMode(IDXGIOutput1 *pOutput, DXGI_MODE_DESC1 *pDisplayMode) {
    if (!pOutput)
      return DXGI_ERROR_INVALID_CALL;

    // Find a mode that the output supports

    DXGI_MODE_DESC1 preferred_mode = *pDisplayMode;
    DXGI_MODE_DESC1 selected_mode = {};

    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)) {
      preferred_mode.Width = 0;
      preferred_mode.Height = 0;
    }

    if (preferred_mode.Format == DXGI_FORMAT_UNKNOWN)
      preferred_mode.Format = desc_.Format;

    HRESULT hr = pOutput->FindClosestMatchingMode1(&preferred_mode, &selected_mode, nullptr);

    if (FAILED(hr)) {
      ERR("DXGI: Failed to query closest mode:"
          "\n"
          "  Format: ",
          preferred_mode.Format,
          "\n"
          "  Mode:   ",
          preferred_mode.Width, "x", preferred_mode.Height, "@",
          preferred_mode.RefreshRate.Numerator / std::max(preferred_mode.RefreshRate.Denominator, 1u));
      return hr;
    }

    if (!selected_mode.RefreshRate.Denominator)
      selected_mode.RefreshRate.Denominator = 1;

    DXGI_OUTPUT_DESC output_desc;
    hr = pOutput->GetDesc(&output_desc);
    if (FAILED(hr))
      return hr;
    wsi::WsiMode wsi_mode{
        selected_mode.Width,
        selected_mode.Height,
        {selected_mode.RefreshRate.Numerator, selected_mode.RefreshRate.Denominator},
        GetMonitorFormatBpp(selected_mode.Format),
        selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST ||
            selected_mode.ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST
    };
    if (!wsi::setWindowMode(output_desc.Monitor, hWnd, wsi_mode))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    *pDisplayMode = selected_mode;
    init_refresh_rate_ = double(selected_mode.RefreshRate.Numerator) / double(selected_mode.RefreshRate.Denominator);
    return S_OK;
  }

  HRESULT
  RestoreDisplayMode(HMONITOR hMonitor) {
    if (!hMonitor)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::restoreDisplayMode(hMonitor))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;

    init_refresh_rate_ = 0;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) final {
    HRESULT hr = S_OK;

    if (pFullscreen != nullptr)
      *pFullscreen = !fullscreen_desc_.Windowed;

    if (ppTarget != nullptr)
      *ppTarget = target_.ref();

    return hr;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) final {
    if (!pDesc)
      return E_INVALIDARG;

    pDesc->BufferDesc.Width = desc_.Width;
    pDesc->BufferDesc.Height = desc_.Height;
    pDesc->BufferDesc.RefreshRate = fullscreen_desc_.RefreshRate;
    pDesc->BufferDesc.Format = desc_.Format;
    pDesc->BufferDesc.ScanlineOrdering = fullscreen_desc_.ScanlineOrdering;
    pDesc->BufferDesc.Scaling = fullscreen_desc_.Scaling;
    pDesc->SampleDesc = desc_.SampleDesc;
    pDesc->BufferUsage = desc_.BufferUsage;
    pDesc->BufferCount = desc_.BufferCount;
    pDesc->OutputWindow = kind_ == DXMTSwapChainKind::Hwnd ? hWnd : nullptr;
    pDesc->Windowed = fullscreen_desc_.Windowed;
    pDesc->SwapEffect = desc_.SwapEffect;
    pDesc->Flags = desc_.Flags;
    return S_OK;
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT flags) final {
    return ResizeBuffersImpl(BufferCount, Width, Height, Format, flags, nullptr, nullptr);
  };

  // Single-node adapter: node mask 0 and 1 both name node 0.
  static bool
  IsNodeZeroMask(UINT mask) {
    return mask <= 1;
  }

  HRESULT
  CollectPresentQueue(UINT node_mask, IUnknown *queue, Com<MTLD3D12CommandQueue> &out) {
    if (!IsNodeZeroMask(node_mask) || !queue)
      return DXGI_ERROR_INVALID_CALL;
    Com<ID3D12CommandQueue> d3d12_queue;
    if (FAILED(queue->QueryInterface(IID_PPV_ARGS(&d3d12_queue))))
      return DXGI_ERROR_INVALID_CALL;
    Com<ID3D12Device> owner;
    if (FAILED(d3d12_queue->GetDevice(IID_PPV_ARGS(&owner))) || owner.ptr() != static_cast<ID3D12Device *>(device_))
      return DXGI_ERROR_INVALID_CALL;
    const auto queue_desc = d3d12_queue->GetDesc();
    if (queue_desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT || !IsNodeZeroMask(queue_desc.NodeMask))
      return DXGI_ERROR_INVALID_CALL;
    out = static_cast<MTLD3D12CommandQueue *>(d3d12_queue.ptr());
    return S_OK;
  }

  // Validates and builds the complete new state before touching the current
  // one, so any failure leaves the swapchain exactly as it was.
  HRESULT
  ResizeBuffersImpl(
      UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT flags, const UINT *node_masks,
      IUnknown *const *queues
  ) {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    // Waitable-object and tearing support are fixed at creation.
    if ((flags ^ desc_.Flags) &
        (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
      return DXGI_ERROR_INVALID_CALL;
    constexpr UINT supported_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
                                      DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                                      DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (flags & ~supported_flags)
      return DXGI_ERROR_UNSUPPORTED;
    if (kind_ == DXMTSwapChainKind::Composition &&
        (!Width || !Height || (flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)))
      return DXGI_ERROR_INVALID_CALL;
    const UINT count = BufferCount ? BufferCount : desc_.BufferCount;
    const bool flip = desc_.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                      desc_.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if (!count || count > DXGI_MAX_SWAP_CHAIN_BUFFERS || (flip && count < 2))
      return DXGI_ERROR_INVALID_CALL;
    if (Format != DXGI_FORMAT_UNKNOWN && ConvertSwapChainFormat(Format) == WMTPixelFormatInvalid)
      return DXGI_ERROR_INVALID_CALL;
    // Every application reference to the current backbuffers must be released.
    // Internal holders (pending presents, temporal frames) use private refs.
    for (auto &backbuffer : backbuffers_) {
      backbuffer->AddRef();
      if (backbuffer->Release() > 1)
        return DXGI_ERROR_INVALID_CALL;
    }

    std::vector<Com<MTLD3D12CommandQueue>> new_queues;
    std::vector<Com<MTLD3D12Resource>> new_backbuffers;
    try {
      new_queues.resize(count);
      new_backbuffers.reserve(count);
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    for (UINT i = 0; i < count; i++) {
      if (!queues) {
        new_queues[i] = queue_;
        continue;
      }
      HRESULT hr = CollectPresentQueue(node_masks[i], queues[i], new_queues[i]);
      if (FAILED(hr))
        return hr;
    }

    DXGI_SWAP_CHAIN_DESC1 new_desc = desc_;
    new_desc.Width = Width;
    new_desc.Height = Height;
    if (!Width || !Height)
      wsi::getWindowSize(hWnd, Width ? nullptr : &new_desc.Width, Height ? nullptr : &new_desc.Height);
    if (Format != DXGI_FORMAT_UNKNOWN)
      new_desc.Format = Format;
    new_desc.BufferCount = count;
    new_desc.Flags = flags;

    D3D12_RESOURCE_DESC new_backbuffer_desc = backbuffer_desc_;
    const bool empty = !new_desc.Width || !new_desc.Height;
    new_backbuffer_desc.Width = empty ? 1 : new_desc.Width;
    new_backbuffer_desc.Height = empty ? 1 : new_desc.Height;
    new_backbuffer_desc.Format = new_desc.Format;

    D3D12_HEAP_PROPERTIES heap_props = device_->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    for (UINT i = 0; i < count; i++) {
      Com<ID3D12Resource> backbuffer;
      HRESULT hr = dxmt::CreateCommittedTexture(
          device_, &heap_props, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES, &new_backbuffer_desc,
          D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&backbuffer)
      );
      if (FAILED(hr))
        return hr;
      new_backbuffers.push_back(static_cast<MTLD3D12Resource *>(backbuffer.ptr()));
    }

    std::shared_ptr<DXMTPresentHistory> new_history;
    if (new_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) {
      auto source = new_backbuffers[0]->texture->view(new_backbuffers[0]->texture->fullView).texture;
      WMTTextureInfo info{};
      info.width = new_backbuffer_desc.Width;
      info.height = new_backbuffer_desc.Height;
      info.depth = 1;
      info.array_length = 1;
      info.mipmap_level_count = 1;
      info.pixel_format = source.pixelFormat();
      info.sample_count = 1;
      info.type = WMTTextureType2D;
      info.usage = WMTTextureUsageShaderRead;
      info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
      try {
        new_history = std::make_shared<DXMTPresentHistory>();
      } catch (const std::bad_alloc &) {
        return E_OUTOFMEMORY;
      }
      new_history->device = device_;
      for (auto &texture : new_history->textures) {
        texture = device_->GetMTLDevice().newTexture(info);
        if (!texture)
          return E_OUTOFMEMORY;
        HRESULT hr = device_->RegisterResidency(texture);
        if (FAILED(hr))
          return hr;
        new_history->resident[&texture - new_history->textures.data()] = true;
      }
    }

    // Old drawables must finish using the previous format and color space
    // before the shared layer is reconfigured.
    presenter->waitPresentsCompleted();
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    desc_ = new_desc;
    backbuffer_desc_ = new_backbuffer_desc;
    backbuffers_.swap(new_backbuffers);
    present_history_ = std::move(new_history);
    history_presents_ = 0;
    ++resize_generation_;
    present_queues_.swap(new_queues);
    current_buffer_ = 0;
    source_width_ = new_desc.Width;
    source_height_ = new_desc.Height;
    dxmt::InvalidateOutputStatistics(monitor_, this);
    ResetFrameStatisticsLocked();
    ApplyLayerProps();
    return S_OK;
  }

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  ResizeTarget(const DXGI_MODE_DESC *pDesc) final {
    if (kind_ != DXMTSwapChainKind::Hwnd || !pDesc)
      return DXGI_ERROR_INVALID_CALL;

    ModeSetGuard::ModeSetInProgress modeset_inprogress(modeset_guard_);
    if (modeset_inprogress)
      return DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;
    if (!wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;

    // Promote display mode
    DXGI_MODE_DESC1 newDisplayMode = {};
    newDisplayMode.Width = pDesc->Width;
    newDisplayMode.Height = pDesc->Height;
    newDisplayMode.RefreshRate = pDesc->RefreshRate;
    newDisplayMode.Format = pDesc->Format;
    newDisplayMode.ScanlineOrdering = pDesc->ScanlineOrdering;
    newDisplayMode.Scaling = pDesc->Scaling;

    if (fullscreen_desc_.Windowed) {
      if (!wsi::resizeWindow(hWnd, &window_state_, newDisplayMode.Width, newDisplayMode.Height))
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    } else {
      wsi::WsiMode old_mode = {};
      if (!wsi::getCurrentDisplayMode(monitor_, &old_mode))
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
      const double old_refresh_rate = init_refresh_rate_;
      HRESULT hr = ChangeDisplayMode(target_.ptr(), &newDisplayMode);
      if (FAILED(hr)) {
        if (!wsi::setWindowMode(monitor_, hWnd, old_mode))
          ERR("DXGI: ResizeTarget: Failed to restore display mode");
        init_refresh_rate_ = old_refresh_rate;
        return hr;
      }
      if (!wsi::updateFullscreenWindow(monitor_, hWnd, false)) {
        if (!wsi::setWindowMode(monitor_, hWnd, old_mode))
          ERR("DXGI: ResizeTarget: Failed to restore display mode");
        init_refresh_rate_ = old_refresh_rate;
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
      }
    }

    if (newDisplayMode.RefreshRate.Numerator != 0)
      fullscreen_desc_.RefreshRate = newDisplayMode.RefreshRate;
    fullscreen_desc_.ScanlineOrdering = newDisplayMode.ScanlineOrdering;
    fullscreen_desc_.Scaling = newDisplayMode.Scaling;
    {
      std::lock_guard<dxmt::mutex> lock(mutex_);
      dxmt::InvalidateOutputStatistics(monitor_, this);
      ResetFrameStatisticsLocked();
    }
    return S_OK;
  };

  void
  ApplyLayerProps() {
    UINT width = 0, height = 0;
    if (kind_ == DXMTSwapChainKind::Composition) {
      width = source_width_;
      height = source_height_;
    } else {
      wsi::getWindowSize(hWnd, &width, &height);
    }
    drawable_width_ = width ? width : desc_.Width;
    drawable_height_ = height ? height : desc_.Height;
    presenter->changeLayerProperties(
        ConvertSwapChainFormat(desc_.Format), ConvertColorSpace(colorspace_, kind_ == DXMTSwapChainKind::Composition || LayerSupportEDR()),
        drawable_width_ * scale_factor, drawable_height_ * scale_factor, desc_.SampleDesc.Count,
        desc_.AlphaMode == DXGI_ALPHA_MODE_PREMULTIPLIED ? DXMTPresentAlpha::Premultiplied :
        desc_.AlphaMode == DXGI_ALPHA_MODE_STRAIGHT ? DXMTPresentAlpha::Straight : DXMTPresentAlpha::Opaque
    );
    pending_colorspace_ = WMTColorSpaceInvalid;
  };

  HRESULT GetOutputFromMonitor(
          HMONITOR                  Monitor,
          IDXGIOutput1**            ppOutput) {
    if (!ppOutput)
      return DXGI_ERROR_INVALID_CALL;

    Com<IDXGIAdapter> adapter;
    Com<IDXGIOutput> output;

    if (FAILED(device_->GetAdapter(IID_PPV_ARGS(&adapter))))
      return E_FAIL;

    for (uint32_t i = 0; SUCCEEDED(adapter->EnumOutputs(i, &output)); i++) {
      DXGI_OUTPUT_DESC outputDesc;
      HRESULT hr = output->GetDesc(&outputDesc);
      if (FAILED(hr))
        return hr;
      
      if (outputDesc.Monitor == Monitor)
        return output->QueryInterface(IID_PPV_ARGS(ppOutput));
      
      output = nullptr;
    }
    
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetContainingOutput(IDXGIOutput **ppOutput) final {
    InitReturnPtr(ppOutput);
    if (!ppOutput)
      return E_INVALIDARG;
    
    if (kind_ == DXMTSwapChainKind::Composition || !wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;
    
    Com<IDXGIOutput1> output;

    if (target_ == nullptr) {
      HRESULT hr = GetOutputFromMonitor(wsi::getWindowMonitor(hWnd), &output);

      if (FAILED(hr))
        return hr;
    } else {
      output = target_;
    }

    *ppOutput = output.ref();
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) final {
    if (!stats)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (fullscreen_desc_.Windowed && desc_.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        desc_.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
      return DXGI_ERROR_UNSUPPORTED;
    if (presentation_observer_fault_->load(std::memory_order_acquire))
      return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    if (!composition_surface_ && !presentation_observer_.setView(native_view_.handle))
      return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    WMTDisplayRefreshSample sample{};
    if (!presentation_observer_.getRefreshSample(sample) || !has_presentation_event_)
      return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    DXGI_FRAME_STATISTICS measured{};
    HRESULT hr = dxmt::ComputeFrameStatistics(last_presentation_event_, sample, fullscreen_desc_.Windowed,
                                            statistics_clock_, statistics_disjoint_, &measured);
    if (FAILED(hr))
      return hr;
    if (statistics_disjoint_) {
      statistics_disjoint_ = false;
      return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    }
    *stats = measured;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetLastPresentCount(UINT *last_present_count) final {
    if (last_present_count == NULL) {
      return E_POINTER;
    }
    std::lock_guard<dxmt::mutex> lock(mutex_);
    *last_present_count = presentation_count_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) final {
    if (pDesc == NULL) {
      return E_POINTER;
    }
    std::lock_guard<dxmt::mutex> lock(mutex_);
    *pDesc = desc_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) final {
    if (pDesc == NULL) {
      return E_POINTER;
    }
    *pDesc = fullscreen_desc_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetHwnd(HWND *pHwnd) final {
    if (pHwnd == NULL) {
      return E_POINTER;
    }
    *pHwnd = kind_ == DXMTSwapChainKind::Hwnd ? hWnd : nullptr;
    return kind_ == DXMTSwapChainKind::Hwnd ? S_OK : DXGI_ERROR_INVALID_CALL;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetCoreWindow(REFIID refiid, void **ppUnk) final {
    if (!ppUnk)
      return DXGI_ERROR_INVALID_CALL;
    *ppUnk = nullptr;
    return core_window_ ? core_window_->QueryInterface(refiid, ppUnk) : DXGI_ERROR_INVALID_CALL;
  };

  DXMT_HOTPATCHABLE
  HRESULT
  STDMETHODCALLTYPE
  Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters) final {
    return PresentImpl(SyncInterval, PresentFlags, pPresentParameters, true);
  }

  HRESULT STDMETHODCALLTYPE PresentGenerated(UINT SyncInterval, UINT Flags) final {
    return PresentImpl(SyncInterval, Flags, nullptr, false);
  }

  // closes_frame is false only for an interpolated Present followed by its real member.
  HRESULT PresentImpl(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters,
                      bool closes_frame) {
    const bool do_not_wait = (PresentFlags & DXGI_PRESENT_DO_NOT_WAIT) != 0;
    std::unique_lock<dxmt::mutex> lock(mutex_, std::defer_lock);
    if (do_not_wait) {
      if (!lock.try_lock())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    } else {
      lock.lock();
    }
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (kind_ != DXMTSwapChainKind::Composition && !wsi::isWindow(hWnd))
      return DXGI_ERROR_INVALID_CALL;
    if (SyncInterval > 4)
      return DXGI_ERROR_INVALID_CALL;
    constexpr UINT supported_present_flags = DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE |
                                             DXGI_PRESENT_RESTART | DXGI_PRESENT_DO_NOT_WAIT |
                                             DXGI_PRESENT_RESTRICT_TO_OUTPUT | DXGI_PRESENT_ALLOW_TEARING;
    if (PresentFlags & ~(supported_present_flags | DXGI_PRESENT_STEREO_PREFER_RIGHT |
                         DXGI_PRESENT_STEREO_TEMPORARY_MONO | DXGI_PRESENT_USE_DURATION))
      return DXGI_ERROR_INVALID_CALL;
    if (PresentFlags & (DXGI_PRESENT_STEREO_PREFER_RIGHT | DXGI_PRESENT_STEREO_TEMPORARY_MONO |
                        DXGI_PRESENT_USE_DURATION))
      return DXGI_ERROR_UNSUPPORTED;
    if ((PresentFlags & DXGI_PRESENT_RESTART) && fullscreen_desc_.Windowed &&
        kind_ != DXMTSwapChainKind::Composition)
      return DXGI_ERROR_INVALID_CALL;
    if ((PresentFlags & DXGI_PRESENT_ALLOW_TEARING) &&
        (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) || SyncInterval || !fullscreen_desc_.Windowed))
      return DXGI_ERROR_INVALID_CALL;
    if (!fullscreen_desc_.Windowed && rotation_ != DXGI_MODE_ROTATION_IDENTITY)
      return DXGI_ERROR_INVALID_CALL;
    if ((PresentFlags & DXGI_PRESENT_RESTRICT_TO_OUTPUT) && !restrict_output_)
      return DXGI_ERROR_INVALID_CALL;
    DXMTPresentComposition composition;
    if (pPresentParameters) {
      const bool has_scroll = pPresentParameters->pScrollRect || pPresentParameters->pScrollOffset;
      if ((pPresentParameters->DirtyRectsCount && !pPresentParameters->pDirtyRects) ||
          bool(pPresentParameters->pScrollRect) != bool(pPresentParameters->pScrollOffset) ||
          (desc_.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD &&
           (pPresentParameters->DirtyRectsCount || has_scroll)))
        return DXGI_ERROR_INVALID_CALL;
      const auto valid_rect = [&](const RECT &rect) {
        return rect.left >= 0 && rect.top >= 0 && rect.right > rect.left && rect.bottom > rect.top &&
               uint64_t(rect.right) <= desc_.Width && uint64_t(rect.bottom) <= desc_.Height;
      };
      for (UINT i = 0; i < pPresentParameters->DirtyRectsCount; ++i)
        if (!valid_rect(pPresentParameters->pDirtyRects[i]))
          return DXGI_ERROR_INVALID_CALL;
      if (has_scroll && !valid_rect(*pPresentParameters->pScrollRect))
        return DXGI_ERROR_INVALID_CALL;
      if (pPresentParameters->DirtyRectsCount) {
        try {
          composition.dirty_rects.assign(pPresentParameters->pDirtyRects,
                                         pPresentParameters->pDirtyRects + pPresentParameters->DirtyRectsCount);
        } catch (const std::bad_alloc &) {
          return E_OUTOFMEMORY;
        }
        composition.full = false;
        if (has_scroll) {
          composition.scroll = true;
          composition.scroll_rect = *pPresentParameters->pScrollRect;
          composition.scroll_offset = *pPresentParameters->pScrollOffset;
        }
      }
    }
    HRESULT hr = S_OK;
    if (desc_.Width == 0 || desc_.Height == 0 ||
        (kind_ != DXMTSwapChainKind::Composition && (!::IsWindowVisible(hWnd) || ::IsIconic(hWnd))))
      hr = DXGI_STATUS_OCCLUDED;
    if (PresentFlags & DXGI_PRESENT_TEST)
      return hr;
    // A successful or occluded non-TEST Present ends an application frame, except
    // the interpolated member of a generated/real pair. Failed Presents do not.
    const auto end_frame = [&] {
      if (closes_frame)
        device_->async_pipelines.AdvanceEpoch();
    };
    if (hr == DXGI_STATUS_OCCLUDED) {
      end_frame();
      // A client that waited before rendering must not lose its frame credit
      // when no presentation is submitted.
      if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        ReleaseSemaphore(present_semaphore_, 1, nullptr);
      return hr;
    }
    if ((PresentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE) && !presentation_count_ &&
        !(PresentFlags & DXGI_PRESENT_RESTART)) {
      end_frame();
      return S_OK;
    }

    // Waiting for a free queued-frame slot cannot hold the swapchain state
    // mutex: another thread may increase the limit or resize while we wait.
    const uint64_t resize_generation = resize_generation_;
    bool borrowed_credit = false;
    lock.unlock();
    if (!latency_->reserve(do_not_wait,
                           PresentFlags & DXGI_PRESENT_RESTART ? presenter->restartableQueuedCredits() : 0,
                           &borrowed_credit))
      return device_->RemovedReason() == S_OK ? DXGI_ERROR_WAS_STILL_DRAWING : DXGI_ERROR_DEVICE_REMOVED;
    struct Admission {
      std::shared_ptr<DXMTPresentLatency> latency;
      ~Admission() { if (latency) latency->cancel(); }
    } admission{latency_};
    if (do_not_wait) {
      if (!lock.try_lock())
        return DXGI_ERROR_WAS_STILL_DRAWING;
    } else {
      lock.lock();
    }
    if (device_->RemovedReason() != S_OK)
      return DXGI_ERROR_DEVICE_REMOVED;
    if (resize_generation != resize_generation_ ||
        (!fullscreen_desc_.Windowed && rotation_ != DXGI_MODE_ROTATION_IDENTITY))
      return DXGI_ERROR_INVALID_CALL;

    UINT client_width = 0, client_height = 0;
    if (composition_surface_) {
      client_width = source_width_;
      client_height = source_height_;
      composition.surface = composition_surface_;
      composition.sync_interval = SyncInterval;
      composition.allow_tearing = (PresentFlags & DXGI_PRESENT_ALLOW_TEARING) != 0;
      composition.frame = {composition_surface_.handle,
          {matrix_._11, matrix_._12, matrix_._21, matrix_._22, matrix_._31, matrix_._32}, 0, 0};
    } else {
      wsi::getWindowSize(hWnd, &client_width, &client_height);
    }
    if (client_width && client_height &&
        (client_width != drawable_width_ || client_height != drawable_height_))
      ApplyLayerProps();

    DXMTPresentGeometry geometry;
    geometry.source_width = source_width_;
    geometry.source_height = source_height_;
    geometry.destination_width = drawable_width_ * scale_factor;
    geometry.destination_height = drawable_height_ * scale_factor;
    geometry.rotation = rotation_;
    geometry.scaling_none = desc_.Scaling == DXGI_SCALING_NONE;
    geometry.right_to_left = hWnd && (::GetWindowLongW(hWnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    geometry.background[0] = background_.r;
    geometry.background[1] = background_.g;
    geometry.background[2] = background_.b;
    geometry.background[3] = desc_.AlphaMode == DXGI_ALPHA_MODE_IGNORE ? 1.0f : background_.a;
    if (PresentFlags & DXGI_PRESENT_RESTRICT_TO_OUTPUT) {
      DXGI_OUTPUT_DESC output_desc{};
      hr = restrict_output_->GetDesc(&output_desc);
      if (FAILED(hr))
        return hr;
      if (composition_surface_) {
        WMTMonitorTimingCall timing{};
        timing.operation = WMTMonitorTimingResolveDisplay;
        timing.left = output_desc.DesktopCoordinates.left;
        timing.top = output_desc.DesktopCoordinates.top;
        timing.right = output_desc.DesktopCoordinates.right;
        timing.bottom = output_desc.DesktopCoordinates.bottom;
        timing.primary = output_desc.Monitor == wsi::getDefaultMonitor();
        WMTMonitorTiming_call(&timing);
        if (!timing.result || !timing.display_id)
          return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
        composition.frame.restrict_output = 1;
        composition.frame.restricted_display_id = timing.display_id;
      } else {
        POINT client_origin{};
        if (!::ClientToScreen(hWnd, &client_origin))
          return DXGI_ERROR_INVALID_CALL;
        geometry.restrict_output = 1;
        geometry.output_clip[0] = (output_desc.DesktopCoordinates.left - client_origin.x) * scale_factor;
        geometry.output_clip[1] = (output_desc.DesktopCoordinates.top - client_origin.y) * scale_factor;
        geometry.output_clip[2] = (output_desc.DesktopCoordinates.right - client_origin.x) * scale_factor;
        geometry.output_clip[3] = (output_desc.DesktopCoordinates.bottom - client_origin.y) * scale_factor;
      }
    }

    double refresh_rate = init_refresh_rate_;
    if (composition_surface_) {
      WMTDisplayRefreshSample sample{};
      if (presentation_observer_.getRefreshSample(sample) && sample.video_refresh_period > 0 &&
          sample.video_time_scale > 0 && std::isfinite(sample.rate_scalar) && sample.rate_scalar > 0)
        refresh_rate = double(sample.video_time_scale) * sample.rate_scalar / sample.video_refresh_period;
    }
    const double cadence = preferred_max_frame_rate > 0 ? preferred_max_frame_rate : refresh_rate;
    double vsync_duration = 0;
    // An unattached composition surface has no display cadence. Use the normal
    // native Present path until attachment provides one; never divide a sentinel.
    if (!(PresentFlags & DXGI_PRESENT_ALLOW_TEARING) && std::isfinite(cadence) && cadence > 0)
      vsync_duration = std::max(SyncInterval / cadence, preferred_max_frame_rate > 0 ? 1.0 / cadence : 0);

    auto &backbuffer = backbuffers_[current_buffer_];
    auto &present_queue = present_queues_[current_buffer_];
    if (present_history_) {
      composition.history = present_history_;
      composition.write_index = PresentFlags & DXGI_PRESENT_RESTART ? 0 : history_presents_ & 1;
      composition.has_previous = !(PresentFlags & DXGI_PRESENT_RESTART) && history_presents_ != 0;
      composition.width = backbuffer_desc_.Width;
      composition.height = backbuffer_desc_.Height;
    }
    if (pending_colorspace_ != WMTColorSpaceInvalid) {
      presenter->changeLayerColorSpace(pending_colorspace_);
      pending_colorspace_ = WMTColorSpaceInvalid;
    }
    owned_graphics::Trace("present_request", ",\"swapchain\":", owned_id_,
                          ",\"frame\":", presentation_count_,
                          ",\"index\":", current_buffer_,
                          ",\"resource\":", backbuffer->owned_id,
                          ",\"sync_interval\":", SyncInterval);
    hr = present_queue->Present(this->presenter.ptr(), backbuffer.ptr(), present_semaphore_, vsync_duration,
                                geometry, presentation_observer_, next_presentation_id_ + 1,
                                presentation_observer_fault_, std::move(composition), do_not_wait, latency_,
                                (PresentFlags & DXGI_PRESENT_RESTART) != 0, borrowed_credit, closes_frame);

    if (SUCCEEDED(hr)) {
      end_frame();
      if (PresentFlags & DXGI_PRESENT_RESTART) {
        history_presents_ = 0;
        dxmt::InvalidateOutputStatistics(monitor_, this);
        ResetFrameStatisticsLocked();
      }
      admission.latency.reset();
      next_presentation_id_ += 1;
      presentation_count_ += 1;
      if (present_history_)
        history_presents_ += 1;
      if (!(PresentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE))
        current_buffer_ = (current_buffer_ + 1) % backbuffers_.size();
    }

    return hr;
  };

  BOOL STDMETHODCALLTYPE
  IsTemporaryMonoSupported() final {
    return FALSE;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) final {
    if (!ppRestrictToOutput)
      return E_INVALIDARG;
    *ppRestrictToOutput = restrict_output_.ref();
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetBackgroundColor(const DXGI_RGBA *pColor) final {
    if (!pColor || !(pColor->r >= 0 && pColor->r <= 1 && pColor->g >= 0 && pColor->g <= 1 &&
                     pColor->b >= 0 && pColor->b <= 1 && pColor->a >= 0 && pColor->a <= 1))
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    background_ = *pColor;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetBackgroundColor(DXGI_RGBA *pColor) final {
    if (!pColor)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    *pColor = background_;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  SetRotation(DXGI_MODE_ROTATION Rotation) final {
    if (Rotation < DXGI_MODE_ROTATION_IDENTITY || Rotation > DXGI_MODE_ROTATION_ROTATE270)
      return DXGI_ERROR_INVALID_CALL;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (desc_.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && desc_.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
      return DXGI_ERROR_INVALID_CALL;
    rotation_ = Rotation;
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  GetRotation(DXGI_MODE_ROTATION *pRotation) final {
    if (!pRotation)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    *pRotation = rotation_;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  SetSourceSize(UINT width, UINT height) override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (!width || !height || width > desc_.Width || height > desc_.Height)
      return E_INVALIDARG;
    source_width_ = width;
    source_height_ = height;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetSourceSize(UINT *width, UINT *height) override {
    if (!width || !height)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    *width = source_width_;
    *height = source_height_;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  SetMaximumFrameLatency(UINT max_latency) override {
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return DXGI_ERROR_INVALID_CALL;
    if (!max_latency || max_latency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return E_INVALIDARG;
    return latency_->setMaximum(max_latency, present_semaphore_);
  };

  HRESULT STDMETHODCALLTYPE
  GetMaximumFrameLatency(UINT *max_latency) override {
    if (!max_latency)
      return E_INVALIDARG;
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      return DXGI_ERROR_INVALID_CALL;
    *max_latency = latency_->maximum();
    return S_OK;
  };

  HANDLE STDMETHODCALLTYPE
  GetFrameLatencyWaitableObject() override {
    if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
      return nullptr;
    }

    HANDLE result = nullptr;
    HANDLE processHandle = GetCurrentProcess();

    if (!DuplicateHandle(processHandle, present_semaphore_, processHandle, &result, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      return nullptr;
    }

    return result;
  };

  HRESULT STDMETHODCALLTYPE
  SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {
    if (kind_ != DXMTSwapChainKind::Composition) return DXGI_ERROR_INVALID_CALL;
    if (!matrix || !std::isfinite(matrix->_11) || !std::isfinite(matrix->_12) ||
        !std::isfinite(matrix->_21) || !std::isfinite(matrix->_22) ||
        !std::isfinite(matrix->_31) || !std::isfinite(matrix->_32) || matrix->_12 != 0 || matrix->_21 != 0)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    matrix_ = *matrix;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {
    if (kind_ != DXMTSwapChainKind::Composition) return DXGI_ERROR_INVALID_CALL;
    if (!matrix) return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    *matrix = matrix_;
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE
  CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override {
    if (!pColorSpaceSupport)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    const auto native = ConvertColorSpace(ColorSpace, kind_ == DXMTSwapChainKind::Composition || LayerSupportEDR());
    *pColorSpaceSupport = native != WMTColorSpaceInvalid &&
                                  (ColorSpace == colorspace_ || CGColorSpace_checkColorSpaceSupported(native))
                              ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
                              : 0;
    return S_OK;
  };

  UINT STDMETHODCALLTYPE
  GetCurrentBackBufferIndex() override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return current_buffer_;
  }

  DXMT_HOTPATCHABLE
  HRESULT STDMETHODCALLTYPE
  ResizeBuffers1(
      UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT *pCreationNodeMask,
      IUnknown *const *ppPresentQueue
  ) override {
    if (!pCreationNodeMask || !ppPresentQueue)
      return DXGI_ERROR_INVALID_CALL;
    return ResizeBuffersImpl(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
  }

  HRESULT STDMETHODCALLTYPE
  SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {
    std::lock_guard<dxmt::mutex> lock(mutex_);
    const auto native = ConvertColorSpace(ColorSpace, kind_ == DXMTSwapChainKind::Composition || LayerSupportEDR());
    if (native == WMTColorSpaceInvalid)
      return DXGI_ERROR_INVALID_CALL;
    if (ColorSpace != colorspace_ && !CGColorSpace_checkColorSpaceSupported(native))
      return DXGI_ERROR_INVALID_CALL;
    colorspace_ = ColorSpace;
    pending_colorspace_ = native;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override {
    if (Size && !pMetaData)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(mutex_);
    if (Type == DXGI_HDR_METADATA_TYPE_NONE) {
      presenter->changeHDRMetadata(nullptr);
      return S_OK;
    }
    if (Type == DXGI_HDR_METADATA_TYPE_HDR10) {
      static_assert(sizeof(DXGI_HDR_METADATA_HDR10) == sizeof(WMTHDRMetadata));
      if (Size != sizeof(DXGI_HDR_METADATA_HDR10) || !pMetaData)
        return E_INVALIDARG;
      WMTHDRMetadata metadata;
      std::memcpy(&metadata, pMetaData, sizeof(metadata));
      presenter->changeHDRMetadata(&metadata);
      return S_OK;
    }
    return DXGI_ERROR_UNSUPPORTED;
  }
};

HRESULT
CreateSwapChain(
    IDXGIFactory1 *pFactory, MTLD3D12Device *pDevice, MTLD3D12CommandQueue *pQueue, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain, IDXGIOutput *pRestrictToOutput, IUnknown *core_window, DXMTSwapChainKind kind
) {
  InitReturnPtr(ppSwapChain);
  if (!pFactory || !pDevice || !pQueue || !pDesc || !ppSwapChain ||
      pQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
      (kind == DXMTSwapChainKind::Composition ? (hWnd || core_window || pFullscreenDesc) : !hWnd))
    return DXGI_ERROR_INVALID_CALL;
  if (kind == DXMTSwapChainKind::Composition &&
      (!pDesc->Width || !pDesc->Height || pDesc->Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH ||
       (pDesc->Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH)))
    return DXGI_ERROR_INVALID_CALL;
  // D3D12 presents only flip-model, single-sample, non-stereo buffers. Reject
  // unsupported descriptors before creating a native view or changing modes.
  if ((pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL &&
       pDesc->SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD) ||
      pDesc->Stereo || pDesc->SampleDesc.Count != 1 || pDesc->SampleDesc.Quality != 0 ||
      pDesc->BufferCount < 2 || pDesc->BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS ||
      (pDesc->Scaling != DXGI_SCALING_STRETCH && pDesc->Scaling != DXGI_SCALING_NONE &&
       pDesc->Scaling != DXGI_SCALING_ASPECT_RATIO_STRETCH) ||
      (pDesc->AlphaMode != DXGI_ALPHA_MODE_UNSPECIFIED && pDesc->AlphaMode != DXGI_ALPHA_MODE_IGNORE &&
       pDesc->AlphaMode != DXGI_ALPHA_MODE_PREMULTIPLIED &&
       pDesc->AlphaMode != DXGI_ALPHA_MODE_STRAIGHT) ||
      ConvertSwapChainFormat(pDesc->Format) == WMTPixelFormatInvalid)
    return DXGI_ERROR_INVALID_CALL;
  if (kind != DXMTSwapChainKind::Composition &&
      (pDesc->AlphaMode == DXGI_ALPHA_MODE_PREMULTIPLIED ||
       pDesc->AlphaMode == DXGI_ALPHA_MODE_STRAIGHT))
    return DXGI_ERROR_UNSUPPORTED; // The HWND-backed Metal layer is opaque.
  constexpr UINT supported_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
                                    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                                    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  if (pDesc->Flags & ~supported_flags)
    return DXGI_ERROR_UNSUPPORTED;
  if (pRestrictToOutput) {
    Com<IDXGIAdapter> parent, adapter;
    DXGI_ADAPTER_DESC parent_desc = {}, adapter_desc = {};
    DXGI_OUTPUT_DESC output_desc = {};
    if (FAILED(pRestrictToOutput->GetParent(IID_PPV_ARGS(&parent))) ||
        FAILED(pDevice->GetAdapter(IID_PPV_ARGS(&adapter))) ||
        FAILED(parent->GetDesc(&parent_desc)) || FAILED(adapter->GetDesc(&adapter_desc)) ||
        std::memcmp(&parent_desc.AdapterLuid, &adapter_desc.AdapterLuid, sizeof(LUID)) != 0 ||
        FAILED(pRestrictToOutput->GetDesc(&output_desc)))
      return DXGI_ERROR_INVALID_CALL;
    bool found = false;
    for (UINT i = 0; !found; ++i) {
      Com<IDXGIOutput> candidate;
      if (FAILED(adapter->EnumOutputs(i, &candidate)))
        break;
      DXGI_OUTPUT_DESC candidate_desc = {};
      found = SUCCEEDED(candidate->GetDesc(&candidate_desc)) && candidate_desc.Monitor == output_desc.Monitor;
    }
    if (!found)
      return DXGI_ERROR_INVALID_CALL;
  }
  Com<MTLD3D12SwapChain> swapchain;
  try {
    swapchain = Com(new MTLD3D12SwapChain(pFactory, pDevice, pQueue, hWnd, pDesc, pFullscreenDesc,
                                          pRestrictToOutput, core_window, kind));
    // MTLDXGIObject starts with one public reference; Com acquired its own.
    // Without releasing the constructor's reference the last application
    // Release never destroys this swapchain, leaving its fullscreen HWND set.
    swapchain->Release();
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  } catch (const NativeViewUnavailable &) {
    return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  } catch (const NativeViewAlreadyAttached &) {
    return DXGI_ERROR_INVALID_CALL;
  }
  HRESULT hr = swapchain->InitializeLatencyRemoval();
  if (FAILED(hr))
    return hr;
  hr = swapchain->InitializePresentationObserver();
  if (FAILED(hr))
    return hr;
  hr = swapchain->ResizeBuffers(0, pDesc->Width, pDesc->Height, DXGI_FORMAT_UNKNOWN, pDesc->Flags);
  if (FAILED(hr))
    return hr;
  if (pFullscreenDesc && !pFullscreenDesc->Windowed) {
    hr = swapchain->SetFullscreenState(TRUE, nullptr);
    if (FAILED(hr))
      return hr;
  }
  return swapchain->QueryInterface(IID_PPV_ARGS(ppSwapChain));
}

}; // namespace dxmt
