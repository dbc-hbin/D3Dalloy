#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_output.hpp"
#include "dxgi_refresh_timing.hpp"
#include "dxgi_options.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "wsi_monitor.hpp"
#include "thread.hpp"

namespace dxmt {

namespace {
struct OutputStatistics {
  const void *owner;
  WMT::Reference<WMT::PresentationObserver> observer;
  std::shared_ptr<std::atomic<bool>> fault;
  WMTPresentationEvent event{};
  RefreshClockBasis clock{};
  bool disjoint = true;
};
dxmt::mutex output_statistics_mutex;
std::unordered_map<HMONITOR, OutputStatistics> output_statistics;
}

__declspec(dllexport) HRESULT RegisterOutputStatistics(HMONITOR monitor, const void *owner, WMT::PresentationObserver observer,
                                 const std::shared_ptr<std::atomic<bool>> &fault) {
  std::lock_guard<dxmt::mutex> lock(output_statistics_mutex);
  auto existing = output_statistics.find(monitor);
  if (existing != output_statistics.end() && existing->second.owner != owner)
    return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  try {
    output_statistics.insert_or_assign(monitor, OutputStatistics{owner, WMT::Reference(observer), fault});
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  return S_OK;
}

__declspec(dllexport) void UnregisterOutputStatistics(HMONITOR monitor, const void *owner) {
  std::lock_guard<dxmt::mutex> lock(output_statistics_mutex);
  auto entry = output_statistics.find(monitor);
  if (entry != output_statistics.end() && entry->second.owner == owner)
    output_statistics.erase(entry);
}

__declspec(dllexport) void PublishOutputStatistics(HMONITOR monitor, const void *owner, const WMTPresentationEvent &event) {
  std::lock_guard<dxmt::mutex> lock(output_statistics_mutex);
  auto entry = output_statistics.find(monitor);
  if (entry != output_statistics.end() && entry->second.owner == owner)
    entry->second.event = event;
}

__declspec(dllexport) void InvalidateOutputStatistics(HMONITOR monitor, const void *owner) {
  std::lock_guard<dxmt::mutex> lock(output_statistics_mutex);
  auto entry = output_statistics.find(monitor);
  if (entry != output_statistics.end() && entry->second.owner == owner) {
    entry->second.event = {};
    entry->second.clock = {};
    entry->second.disjoint = true;
  }
}

__declspec(dllexport) HRESULT ComputeFrameStatistics(const WMTPresentationEvent &event, const WMTDisplayRefreshSample &sample,
                               bool windowed, RefreshClockBasis &clock, bool &disjoint, DXGI_FRAME_STATISTICS *stats) {
  constexpr uint64_t kCVValidTiming = 0x1b;
  if ((sample.cv_flags & kCVValidTiming) != kCVValidTiming || !sample.host_frequency ||
      !sample.output_host_ticks || sample.video_time <= 0 || sample.video_refresh_period <= 0 ||
      sample.video_time_scale <= 0 || !std::isfinite(sample.rate_scalar) || sample.rate_scalar <= 0 ||
      event.outcome != WMTPresentationPresented || !event.refresh_sequence ||
      !std::isfinite(event.presented_host_seconds) || event.presented_host_seconds <= 0 ||
      !event.refresh_output_host_ticks || event.refresh_video_time <= 0 ||
      event.refresh_generation != sample.refresh_generation || sample.refresh_sequence < event.refresh_sequence)
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
  if (clock.SetEpoch(sample.refresh_generation, sample.display_id, sample.host_frequency, sample.video_time_scale))
    disjoint = true;
  double sampled_host_seconds;
  if (!clock.qpc_frequency) {
    LARGE_INTEGER before{}, after{}, frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || !QueryPerformanceCounter(&before))
      return E_FAIL;
    sampled_host_seconds = WMTClockSampleHostSeconds();
    if (!QueryPerformanceCounter(&after) || after.QuadPart < before.QuadPart ||
        !std::isfinite(sampled_host_seconds) || sampled_host_seconds <= 0)
      return E_FAIL;
    clock.calibration_host_seconds = sampled_host_seconds;
    clock.calibration_qpc = before.QuadPart + (after.QuadPart - before.QuadPart) / 2;
    clock.qpc_frequency = frequency.QuadPart;
  } else {
    sampled_host_seconds = WMTClockSampleHostSeconds();
  }
  if (!std::isfinite(sampled_host_seconds) || sampled_host_seconds <= 0)
    return E_FAIL;
  const double sync_anchor_seconds = static_cast<double>(sample.output_host_ticks) / sample.host_frequency;
  const double present_anchor_seconds = static_cast<double>(event.refresh_output_host_ticks) / sample.host_frequency;
  const double period_seconds = static_cast<double>(sample.video_refresh_period) /
                                sample.video_time_scale / sample.rate_scalar;
  if (!std::isfinite(period_seconds) || period_seconds <= 0 ||
      std::abs(sync_anchor_seconds - sampled_host_seconds) > 0.25 ||
      std::abs(present_anchor_seconds - event.presented_host_seconds) > 0.25 ||
      event.presented_host_seconds > sampled_host_seconds + 0.05)
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
  const int64_t present_tick_delta = RefreshTickOffset(event.presented_host_seconds, present_anchor_seconds, period_seconds);
  const int64_t sync_tick_delta = RefreshTickOffset(sampled_host_seconds, sync_anchor_seconds, period_seconds);
  const double sync_host_seconds = sync_anchor_seconds + sync_tick_delta * period_seconds;
  const int64_t present_refresh = (windowed ? static_cast<int64_t>(event.refresh_sequence)
      : event.refresh_video_time / sample.video_refresh_period) + present_tick_delta;
  const int64_t sync_refresh = (windowed ? static_cast<int64_t>(sample.refresh_sequence)
      : sample.video_time / sample.video_refresh_period) + sync_tick_delta;
  if (present_refresh <= 0 || present_refresh > sync_refresh)
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
  if ((sync_tick_delta > 0 && sync_tick_delta >
       (std::numeric_limits<int64_t>::max() - sample.video_time) / sample.video_refresh_period) ||
      (sync_tick_delta < 0 && sync_tick_delta < -(sample.video_time / sample.video_refresh_period)))
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
  const int64_t reached_video_time = sample.video_time + sync_tick_delta * sample.video_refresh_period;
  int64_t sync_qpc;
  if (!clock.Project(reached_video_time, sync_host_seconds, sync_qpc))
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
  *stats = {};
  stats->PresentCount = static_cast<UINT>(event.present_id);
  stats->PresentRefreshCount = static_cast<UINT>(present_refresh);
  stats->SyncRefreshCount = static_cast<UINT>(sync_refresh);
  stats->SyncQPCTime.QuadPart = sync_qpc;
  return S_OK;
}

/*
 * \see
 * https://github.com/microsoft/DirectXTex/blob/main/DirectXTex/DirectXTexUtil.cpp
 */
uint32_t GetMonitorFormatBpp(DXGI_FORMAT Format) {
  switch (Format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    return 32;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return 64;

  default:
    Logger::warn(str::format("GetMonitorFormatBpp: Unknown format: ", Format));
    return 32;
  }
}

DXGI_MODE_DESC1 ConvertDisplayMode(const wsi::WsiMode &WsiMode) {
  DXGI_MODE_DESC1 dxgiMode = {};
  dxgiMode.Width = WsiMode.width;
  dxgiMode.Height = WsiMode.height;
  dxgiMode.RefreshRate = DXGI_RATIONAL{WsiMode.refreshRate.numerator,
                                       WsiMode.refreshRate.denominator};
  dxgiMode.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // FIXME
  dxgiMode.ScanlineOrdering = WsiMode.interlaced
                                  ? DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST
                                  : DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
  dxgiMode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  dxgiMode.Stereo = FALSE;
  return dxgiMode;
}

void FilterModesByDesc(std::vector<DXGI_MODE_DESC1> &Modes,
                       const DXGI_MODE_DESC1 &TargetMode) {
  // Filter modes based on format properties
  bool testScanlineOrder = false;
  bool testScaling = false;
  bool testFormat = false;

  for (const auto &mode : Modes) {
    testScanlineOrder |=
        TargetMode.ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED &&
        TargetMode.ScanlineOrdering == mode.ScanlineOrdering;
    testScaling |= TargetMode.Scaling != DXGI_MODE_SCALING_UNSPECIFIED &&
                   TargetMode.Scaling == mode.Scaling;
    testFormat |= TargetMode.Format != DXGI_FORMAT_UNKNOWN &&
                  TargetMode.Format == mode.Format;
  }

  std::erase_if(Modes, [&](const DXGI_MODE_DESC1 &mode) {
    bool skipMode = mode.Stereo != TargetMode.Stereo;

    if (testScanlineOrder)
      skipMode |= mode.ScanlineOrdering != TargetMode.ScanlineOrdering;

    if (testScaling)
      skipMode |= mode.Scaling != TargetMode.Scaling;

    if (testFormat)
      skipMode |= mode.Format != TargetMode.Format;

    return skipMode;
  });

  // Filter by closest resolution
  uint32_t minDiffResolution = 0;

  if (TargetMode.Width) {
    minDiffResolution = std::accumulate(
        Modes.begin(), Modes.end(), std::numeric_limits<uint32_t>::max(),
        [&TargetMode](uint32_t current, const DXGI_MODE_DESC1 &mode) {
          uint32_t diff = std::abs(int32_t(TargetMode.Width - mode.Width)) +
                          std::abs(int32_t(TargetMode.Height - mode.Height));
          return std::min(current, diff);
        });

    std::erase_if(Modes, [&](const DXGI_MODE_DESC1 &mode) {
      uint32_t diff = std::abs(int32_t(TargetMode.Width - mode.Width)) +
                      std::abs(int32_t(TargetMode.Height - mode.Height));
      return diff != minDiffResolution;
    });
  }

  // Filter by closest refresh rate
  uint32_t minDiffRefreshRate = 0;

  if (TargetMode.RefreshRate.Numerator && TargetMode.RefreshRate.Denominator) {
    minDiffRefreshRate = std::accumulate(
        Modes.begin(), Modes.end(), std::numeric_limits<uint64_t>::max(),
        [&TargetMode](uint64_t current, const DXGI_MODE_DESC1 &mode) {
          uint64_t rate = uint64_t(mode.RefreshRate.Numerator) *
                          uint64_t(TargetMode.RefreshRate.Denominator) /
                          uint64_t(mode.RefreshRate.Denominator);
          uint64_t diff = std::abs(
              int64_t(rate - uint64_t(TargetMode.RefreshRate.Numerator)));
          return std::min(current, diff);
        });

    std::erase_if(Modes, [&](const DXGI_MODE_DESC1 &mode) {
      uint64_t rate = uint64_t(mode.RefreshRate.Numerator) *
                      uint64_t(TargetMode.RefreshRate.Denominator) /
                      uint64_t(mode.RefreshRate.Denominator);
      uint64_t diff =
          std::abs(int64_t(rate - uint64_t(TargetMode.RefreshRate.Numerator)));

      return diff != minDiffRefreshRate;
    });
  }
}

class MTLDXGIOutputImpl : public MTLDXGIOutput {
public:
  MTLDXGIOutputImpl(IMTLDXGIAdapter *adapter, HMONITOR monitor, DxgiOptions &options)
      : adapter_(adapter), monitor_(monitor), options_(options) {
    WMTGetDisplayDescription(monitor_ == wsi::getDefaultMonitor()
                                 ? WMTGetPrimaryDisplayId()
                                 : WMTGetSecondaryDisplayId(),
                             &native_desc_);
    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_ramp_.red[i] = gamma_ramp_.green[i] = gamma_ramp_.blue[i] =
          float(i) / float(DXMT_GAMMA_CP_COUNT - 1);
    }
    gamma_ramp_.version = 0;
  }

  ~MTLDXGIOutputImpl() {
    if (vblank_observer_)
      vblank_observer_.close();
  }

  const DXMTGammaRamp *
  STDMETHODCALLTYPE
  GetGammaRamp() override {
    return &gamma_ramp_;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {

    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIOutput) || riid == __uuidof(IDXGIOutput1) ||
        riid == __uuidof(IDXGIOutput2) || riid == __uuidof(IDXGIOutput3) ||
        riid == __uuidof(IDXGIOutput4) || riid == __uuidof(IDXGIOutput5) ||
        riid == __uuidof(IDXGIOutput6)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIOutput), riid)) {
      WARN("DXGIOutput: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetParent(REFIID riid, void **ppParent) final {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDesc(DXGI_OUTPUT_DESC *pDesc) final {
    if (pDesc == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::getDesktopCoordinates(monitor_, &pDesc->DesktopCoordinates)) {
      ERR("Failed to query monitor coords");
      return E_FAIL;
    }

    if (!wsi::getDisplayName(monitor_, pDesc->DeviceName)) {
      ERR("Failed to query monitor name");
      return E_FAIL;
    }

    pDesc->AttachedToDesktop = 1;
    pDesc->Rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    pDesc->Monitor = monitor_;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT *pNumModes,
                     DXGI_MODE_DESC *pDesc) final {
    if (pNumModes == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    const UINT capacity = *pNumModes;
    std::vector<DXGI_MODE_DESC1> modes;

    if (pDesc)
      modes.resize(std::max(1u, capacity));

    UINT modeCount = capacity;
    HRESULT hr = GetDisplayModeList1(EnumFormat, Flags, &modeCount,
                                     pDesc ? modes.data() : nullptr);

    if (pDesc) {
      for (uint32_t i = 0; i < modeCount && i < capacity && i < modes.size(); i++) {
        pDesc[i].Width = modes[i].Width;
        pDesc[i].Height = modes[i].Height;
        pDesc[i].RefreshRate = modes[i].RefreshRate;
        pDesc[i].Format = modes[i].Format;
        pDesc[i].ScanlineOrdering = modes[i].ScanlineOrdering;
        pDesc[i].Scaling = modes[i].Scaling;
      }
    }

    *pNumModes = modeCount;
    return hr;
  }

  HRESULT
  STDMETHODCALLTYPE
  FindClosestMatchingMode(const DXGI_MODE_DESC *pModeToMatch,
                          DXGI_MODE_DESC *pClosestMatch,
                          IUnknown *pConcernedDevice) final {
    if (!pModeToMatch || !pClosestMatch)
      return DXGI_ERROR_INVALID_CALL;

    DXGI_MODE_DESC1 modeToMatch;
    modeToMatch.Width = pModeToMatch->Width;
    modeToMatch.Height = pModeToMatch->Height;
    modeToMatch.RefreshRate = pModeToMatch->RefreshRate;
    modeToMatch.Format = pModeToMatch->Format;
    modeToMatch.ScanlineOrdering = pModeToMatch->ScanlineOrdering;
    modeToMatch.Scaling = pModeToMatch->Scaling;
    modeToMatch.Stereo = FALSE;

    DXGI_MODE_DESC1 closestMatch = {};

    HRESULT hr =
        FindClosestMatchingMode1(&modeToMatch, &closestMatch, pConcernedDevice);

    if (FAILED(hr))
      return hr;

    pClosestMatch->Width = closestMatch.Width;
    pClosestMatch->Height = closestMatch.Height;
    pClosestMatch->RefreshRate = closestMatch.RefreshRate;
    pClosestMatch->Format = closestMatch.Format;
    pClosestMatch->ScanlineOrdering = closestMatch.ScanlineOrdering;
    pClosestMatch->Scaling = closestMatch.Scaling;
    return hr;
  }

  HRESULT
  STDMETHODCALLTYPE
  WaitForVBlank() final {
    std::lock_guard<dxmt::mutex> lock(vblank_mutex_);
    RECT bounds{};
    if (!wsi::getDesktopCoordinates(monitor_, &bounds))
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    WMTMonitorTimingCall timing{};
    timing.operation = WMTMonitorTimingCreate;
    timing.left = bounds.left;
    timing.top = bounds.top;
    timing.right = bounds.right;
    timing.bottom = bounds.bottom;
    timing.primary = monitor_ == wsi::getDefaultMonitor();
    if (!vblank_observer_) {
      WMTMonitorTiming_call(&timing);
      if (!timing.object)
        return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
      vblank_observer_ = WMT::Reference<WMT::PresentationObserver>(timing.object);
    }
    timing.object = vblank_observer_.handle;
    timing.operation = WMTMonitorTimingWait;
    WMTMonitorTiming_call(&timing);
    if (!timing.result) {
      vblank_observer_.close();
      vblank_observer_ = nullptr;
    }
    return timing.result ? S_OK : DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  }

  HRESULT
  STDMETHODCALLTYPE
  TakeOwnership(IUnknown *device, BOOL exclusive) final {
    WARN("TakeOwnership: Stub");
    return S_OK;
  }

  void STDMETHODCALLTYPE ReleaseOwnership() final {
    WARN("ReleaseOwnership: Stub");
  }

  HRESULT
  STDMETHODCALLTYPE
  GetGammaControlCapabilities(
      DXGI_GAMMA_CONTROL_CAPABILITIES *gamma_caps) final {
    if (gamma_caps == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    gamma_caps->ScaleAndOffsetSupported = false;
    gamma_caps->MaxConvertedValue = 1.0f;
    gamma_caps->MinConvertedValue = 0.0f;
    gamma_caps->NumGammaControlPoints = DXMT_GAMMA_CP_COUNT;
    for (uint32_t i = 0; i < gamma_caps->NumGammaControlPoints; i++)
      gamma_caps->ControlPointPositions[i] = float(i) / float(DXMT_GAMMA_CP_COUNT - 1);
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  SetGammaControl(const DXGI_GAMMA_CONTROL *gamma_control) final {
    if (gamma_control == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_ramp_.red[i] = gamma_control->GammaCurve[i].Red;
      gamma_ramp_.green[i] = gamma_control->GammaCurve[i].Green;
      gamma_ramp_.blue[i] = gamma_control->GammaCurve[i].Blue;
    }
    gamma_ramp_.version++;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetGammaControl(DXGI_GAMMA_CONTROL *gamma_control) final {
    if (gamma_control == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    gamma_control->Scale = { 1.0f, 1.0f, 1.0f };
    gamma_control->Offset = { 0.0f, 0.0f, 0.0f };
    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_control->GammaCurve[i].Red = gamma_ramp_.red[i];
      gamma_control->GammaCurve[i].Green = gamma_ramp_.green[i];
      gamma_control->GammaCurve[i].Blue = gamma_ramp_.blue[i];
    }
  
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  SetDisplaySurface(IDXGISurface *surface) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplaySurfaceData(IDXGISurface *surface) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) final {
    if (!stats)
      return E_INVALIDARG;
    std::lock_guard<dxmt::mutex> lock(output_statistics_mutex);
    auto entry = output_statistics.find(monitor_);
    if (entry == output_statistics.end())
      return DXGI_ERROR_INVALID_CALL; // Output statistics are fullscreen-only.
    auto &state = entry->second;
    if (state.fault && state.fault->load(std::memory_order_acquire))
      return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    WMTDisplayRefreshSample sample{};
    if (!state.observer.getRefreshSample(sample))
      return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    DXGI_FRAME_STATISTICS measured{};
    HRESULT hr = ComputeFrameStatistics(state.event, sample, false, state.clock, state.disjoint, &measured);
    if (FAILED(hr))
      return hr;
    if (state.disjoint) {
      state.disjoint = false;
      return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    }
    *stats = measured;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags, UINT *pNumModes,
                      DXGI_MODE_DESC1 *pDesc) final {
    if (pNumModes == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    // Special case, just return zero modes
    if (EnumFormat == DXGI_FORMAT_UNKNOWN) {
      *pNumModes = 0;
      return S_OK;
    }
    MTL_DXGI_FORMAT_DESC formatDesc;
    if (FAILED(MTLQueryDXGIFormat(this->adapter_->GetMTLDevice(), EnumFormat, formatDesc))
      || !(formatDesc.Flag & MTL_DXGI_FORMAT_BACKBUFFER)) {
      *pNumModes = 0;
      return S_OK;
    }

    // Walk over all modes that the display supports and
    // return those that match the requested format etc.
    wsi::WsiMode devMode = {};

    uint32_t srcModeId = 0;
    uint32_t dstModeId = 0;

    std::vector<DXGI_MODE_DESC1> modeList;

    while (wsi::getDisplayMode(monitor_, srcModeId++, &devMode)) {
      // Only enumerate interlaced modes if requested.
      if (devMode.interlaced && !(Flags & DXGI_ENUM_MODES_INTERLACED))
        continue;

      // Skip modes with incompatible formats
      if (devMode.bitsPerPixel != GetMonitorFormatBpp(EnumFormat))
        continue;

      if (pDesc != nullptr) {
        DXGI_MODE_DESC1 mode = ConvertDisplayMode(devMode);
        // Fix up the DXGI_FORMAT to match what we were enumerating.
        mode.Format = EnumFormat;

        modeList.push_back(mode);
      }

      dstModeId += 1;
    }

    // Sort display modes by width, height and refresh rate,
    // in that order. Some games rely on correct ordering.
    std::sort(modeList.begin(), modeList.end(),
              [](const DXGI_MODE_DESC1 &a, const DXGI_MODE_DESC1 &b) {
                if (a.Width < b.Width)
                  return true;
                if (a.Width > b.Width)
                  return false;

                if (a.Height < b.Height)
                  return true;
                if (a.Height > b.Height)
                  return false;

                return (a.RefreshRate.Numerator / a.RefreshRate.Denominator) <
                       (b.RefreshRate.Numerator / b.RefreshRate.Denominator);
              });

    // If requested, write out the first set of display
    // modes to the destination array.
    if (pDesc != nullptr) {
      for (uint32_t i = 0; i < *pNumModes && i < dstModeId; i++)
        pDesc[i] = modeList[i];

      if (dstModeId > *pNumModes)
        return DXGI_ERROR_MORE_DATA;
    }

    *pNumModes = dstModeId;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  FindClosestMatchingMode1(const DXGI_MODE_DESC1 *pModeToMatch,
                           DXGI_MODE_DESC1 *pClosestMatch,
                           IUnknown *pConcernedDevice) final {
    if (!pModeToMatch || !pClosestMatch)
      return DXGI_ERROR_INVALID_CALL;

    if (pModeToMatch->Format == DXGI_FORMAT_UNKNOWN && !pConcernedDevice)
      return DXGI_ERROR_INVALID_CALL;

    // Both or neither must be zero
    if ((pModeToMatch->Width == 0) ^ (pModeToMatch->Height == 0))
      return DXGI_ERROR_INVALID_CALL;

    wsi::WsiMode activeWsiMode = {};
    wsi::getCurrentDisplayMode(monitor_, &activeWsiMode);

    DXGI_MODE_DESC1 activeMode = ConvertDisplayMode(activeWsiMode);

    DXGI_MODE_DESC1 defaultMode;
    defaultMode.Width = 0;
    defaultMode.Height = 0;
    defaultMode.RefreshRate = {0, 0};
    defaultMode.Format = DXGI_FORMAT_UNKNOWN;
    defaultMode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    defaultMode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    defaultMode.Stereo = pModeToMatch->Stereo;

    if (pModeToMatch->ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED)
      defaultMode.ScanlineOrdering = activeMode.ScanlineOrdering;

    if (pModeToMatch->Scaling == DXGI_MODE_SCALING_UNSPECIFIED)
      defaultMode.Scaling = activeMode.Scaling;

    DXGI_FORMAT targetFormat = pModeToMatch->Format;

    if (pModeToMatch->Format == DXGI_FORMAT_UNKNOWN) {
      defaultMode.Format = activeMode.Format;
      targetFormat = activeMode.Format;
    }

    if (!pModeToMatch->Width) {
      defaultMode.Width = activeMode.Width;
      defaultMode.Height = activeMode.Height;
    }

    if (!pModeToMatch->RefreshRate.Numerator ||
        !pModeToMatch->RefreshRate.Denominator) {
      defaultMode.RefreshRate.Numerator = activeMode.RefreshRate.Numerator;
      defaultMode.RefreshRate.Denominator = activeMode.RefreshRate.Denominator;
    }

    UINT modeCount = 0;
    GetDisplayModeList1(targetFormat, DXGI_ENUM_MODES_SCALING, &modeCount,
                        nullptr);

    if (modeCount == 0) {
      ERR("No modes found");
      return DXGI_ERROR_NOT_FOUND;
    }

    std::vector<DXGI_MODE_DESC1> modes(modeCount);
    GetDisplayModeList1(targetFormat, DXGI_ENUM_MODES_SCALING, &modeCount,
                        modes.data());

    FilterModesByDesc(modes, *pModeToMatch);
    FilterModesByDesc(modes, defaultMode);

    if (modes.empty())
      return DXGI_ERROR_NOT_FOUND;

    *pClosestMatch = modes[0];

    Logger::debug(str::format("DXGI: For mode ", pModeToMatch->Width, "x",
                              pModeToMatch->Height, "@",
                              pModeToMatch->RefreshRate.Denominator
                                  ? (pModeToMatch->RefreshRate.Numerator /
                                     pModeToMatch->RefreshRate.Denominator)
                                  : 0,
                              " found closest mode ", pClosestMatch->Width, "x",
                              pClosestMatch->Height, "@",
                              pClosestMatch->RefreshRate.Denominator
                                  ? (pClosestMatch->RefreshRate.Numerator /
                                     pClosestMatch->RefreshRate.Denominator)
                                  : 0));
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplaySurfaceData1(IDXGIResource *destination) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT
  STDMETHODCALLTYPE
  DuplicateOutput(IUnknown *pDevice,
                  IDXGIOutputDuplication **ppOutputDuplication) final {
    InitReturnPtr(ppOutputDuplication);

    if (!pDevice)
      return E_INVALIDARG;

    ERR("Not implemented");

    // At least return a valid error code
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT
  STDMETHODCALLTYPE
  CheckOverlaySupport(DXGI_FORMAT enum_format,
                              IUnknown *concerned_device,
                              UINT *flags) override {
    if (flags) {
      *flags = 0;
    }
    return S_OK;
  }

  WINBOOL STDMETHODCALLTYPE SupportsOverlays() override { return FALSE; }

  HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT format,
                                        DXGI_COLOR_SPACE_TYPE colour_space,
                                        IUnknown *device,
                                        UINT *flags) override {
    if (flags) {
      *flags = 0;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1 *pDesc) override {
    if (pDesc == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::getDesktopCoordinates(monitor_, &pDesc->DesktopCoordinates)) {
      ERR("Failed to query monitor coords");
      return E_FAIL;
    }

    if (!wsi::getDisplayName(monitor_, pDesc->DeviceName)) {
      ERR("Failed to query monitor name");
      return E_FAIL;
    }

    pDesc->AttachedToDesktop = 1;
    pDesc->Rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    pDesc->Monitor = monitor_;
    if (options_.forceSDR) {
      pDesc->BitsPerColor = 8;
      pDesc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    } else {
      pDesc->BitsPerColor =
          native_desc_.maximum_potential_edr_color_component_value > 1 ? 10 : 8;
      pDesc->ColorSpace =
          native_desc_.maximum_potential_edr_color_component_value > 1
              ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
              : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    memcpy(pDesc->RedPrimary, native_desc_.red_primaries, 8);
    memcpy(pDesc->GreenPrimary, native_desc_.green_primaries, 8);
    memcpy(pDesc->BluePrimary, native_desc_.blue_primaries, 8);
    memcpy(pDesc->WhitePoint, native_desc_.white_points, 8);
    pDesc->MinLuminance = 0.0f;
    pDesc->MaxLuminance = native_desc_.maximum_potential_edr_color_component_value * 100.0f;
    pDesc->MaxFullFrameLuminance = pDesc->MaxLuminance;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  CheckHardwareCompositionSupport(UINT *flags) override {
    if (flags) {
      *flags = 0;
    }
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  DuplicateOutput1(IUnknown *pDevice, UINT flags, UINT format_count,
                   const DXGI_FORMAT *formats,
                   IDXGIOutputDuplication **ppOutputDuplication) override {
    InitReturnPtr(ppOutputDuplication);
    if (!pDevice)
      return E_INVALIDARG;

    ERR("Not implemented");

    return DXGI_ERROR_UNSUPPORTED;
  }

private:
  dxmt::mutex vblank_mutex_;
  WMT::Reference<WMT::PresentationObserver> vblank_observer_;
  Com<IMTLDXGIAdapter> adapter_ = nullptr;
  HMONITOR monitor_ = nullptr;
  WMTDisplayDescription native_desc_;
  DxgiOptions &options_;
  DXMTGammaRamp gamma_ramp_;
};

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options) {
  return Com<IDXGIOutput>::transfer(new MTLDXGIOutputImpl(pAadapter, monitor, options));
};

} // namespace dxmt