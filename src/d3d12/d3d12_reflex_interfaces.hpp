// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "com/com_guid.hpp"
#include "d3d12.h"

// NVAPI V1 report layout, deliberately independent of the vendor header.
struct DXMT_REFLEX_REPORT {
  UINT64 frameID, inputSampleTime, simStartTime, simEndTime;
  UINT64 renderSubmitStartTime, renderSubmitEndTime, presentStartTime, presentEndTime;
  UINT64 driverStartTime, driverEndTime, osRenderQueueStartTime, osRenderQueueEndTime;
  UINT64 gpuRenderStartTime, gpuRenderEndTime;
  UINT gpuActiveRenderTimeUs, gpuFrameTimeUs;
  BYTE reserved[120];
};

DEFINE_COM_INTERFACE("c176e297-6e21-4b32-8c77-a8d51f05d602", IMTLD3D12Reflex)
    : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetReflexMode(BOOL low_latency, UINT interval_us, BOOL use_markers) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetReflexStatus(BOOL *low_latency, UINT *sleep_us, BOOL *game_sleep) = 0;
  virtual HRESULT STDMETHODCALLTYPE ReflexSleep() = 0;
  virtual HRESULT STDMETHODCALLTYPE ReflexMarker(UINT64 frame, UINT marker) = 0;
  // S_FALSE: fewer than 64 complete, measured frames; NVAPI returns zero warmup reports.
  virtual HRESULT STDMETHODCALLTYPE GetReflexReports(DXMT_REFLEX_REPORT *reports) = 0;
};
