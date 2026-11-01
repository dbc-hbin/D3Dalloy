#pragma once
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "d3dalloy_presenter.hpp"
#include "dxgi_refresh_timing.hpp"
#include <atomic>
#include <memory>

namespace d3dalloy {

HRESULT ComputeFrameStatistics(const WMTPresentationEvent &event, const WMTDisplayRefreshSample &sample,
                               bool windowed, RefreshClockBasis &clock, bool &disjoint, DXGI_FRAME_STATISTICS *stats);
HRESULT RegisterOutputStatistics(HMONITOR monitor, const void *owner, WMT::PresentationObserver observer,
                                 const std::shared_ptr<std::atomic<bool>> &fault);
void UnregisterOutputStatistics(HMONITOR monitor, const void *owner);
void PublishOutputStatistics(HMONITOR monitor, const void *owner, const WMTPresentationEvent &event);
void InvalidateOutputStatistics(HMONITOR monitor, const void *owner);

struct MTLDXGIOutput : public MTLDXGIObject<IDXGIOutput6> {
  virtual const D3DalloyGammaRamp *STDMETHODCALLTYPE GetGammaRamp() = 0;
};

} // namespace d3dalloy
