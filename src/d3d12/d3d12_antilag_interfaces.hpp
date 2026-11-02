// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "com/com_guid.hpp"
#include "d3d12.h"

// Private bridge: the AMD SDK facade and NVAPI share the device's frame scheduler.
DEFINE_COM_INTERFACE("8c0169ab-731a-4eaa-b1de-1282799e2ce6", IMTLD3D12AntiLag) : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetAntiLagMode(BOOL low_latency, UINT interval_us) = 0;
  virtual HRESULT STDMETHODCALLTYPE AntiLagSleep() = 0;
  virtual HRESULT STDMETHODCALLTYPE AntiLagEndFrame() = 0;
  virtual HRESULT STDMETHODCALLTYPE AntiLagFrameType(BOOL interpolated) = 0;
};
