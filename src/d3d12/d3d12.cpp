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

#include "d3d12.h"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_sdk.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <mutex>
#include <vector>

namespace dxmt {

Logger Logger::s_instance("d3d12.log");

struct ExperimentalDeviceRegistration {
  std::mutex mutex;
  MTLD3D12Device *device;
  const bool independent;
  explicit ExperimentalDeviceRegistration(MTLD3D12Device *value) : device(value), independent(value->independent_device) {}
};

namespace {
std::mutex experimental_mutex;
RuntimeConfiguration global_configuration;
std::vector<std::shared_ptr<ExperimentalDeviceRegistration>> experimental_devices;
}

HRESULT
RegisterExperimentalDevice(MTLD3D12Device *device, std::shared_ptr<ExperimentalDeviceRegistration> &registration) {
  try {
    auto entry = std::make_shared<ExperimentalDeviceRegistration>(device);
    std::lock_guard lock(experimental_mutex);
    experimental_devices.push_back(entry);
    registration = std::move(entry);
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

void
UnregisterExperimentalDevice(const std::shared_ptr<ExperimentalDeviceRegistration> &registration) {
  if (!registration)
    return;
  {
    std::lock_guard lock(experimental_mutex);
    std::erase(experimental_devices, registration);
  }
  // Snapshots own the registration, never the COM device. Wait for a bounded
  // removal callback before the device's destructor can free its storage.
  std::lock_guard lock(registration->mutex);
  registration->device = nullptr;
}

bool HasGlobalDevices() {
  std::lock_guard lock(experimental_mutex);
  return std::any_of(experimental_devices.begin(), experimental_devices.end(),
                     [](const auto &entry) { return !entry->independent; });
}
void RemoveDevicesForDebugLayer() {
  // Reuse the existing bounded device registration/teardown synchronization.
  // The public Debug contract removes devices created before enablement.
  std::vector<std::shared_ptr<ExperimentalDeviceRegistration>> devices;
  try {
    std::lock_guard lock(experimental_mutex); devices = experimental_devices;
  } catch (const std::bad_alloc &) { ERR("EnableDebugLayer: unable to snapshot existing devices"); return; }
  for (const auto &entry : devices) {
    std::lock_guard lock(entry->mutex);
    if (entry->device && !entry->device->independent_device) entry->device->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
  }
}

RuntimeConfiguration GetGlobalRuntimeConfiguration() {
  std::lock_guard lock(experimental_mutex);
  return global_configuration;
}
void EnableGlobalDebugLayer() {
  { std::lock_guard lock(experimental_mutex); global_configuration.debug = true; }
  RemoveDevicesForDebugLayer();
}
void SetGlobalDredEnablement(bool contexts, D3D12_DRED_ENABLEMENT enablement) {
  std::lock_guard lock(experimental_mutex);
  (contexts ? global_configuration.contexts : global_configuration.breadcrumbs) = enablement;
}
HRESULT ValidateExperimentalFeatures(UINT count, const IID *ids, void *configuration, UINT *sizes) {
  if (count && !ids)
    return E_INVALIDARG;
  for (UINT i = 0; i < count; ++i)
    if (ids[i] != D3D12ExperimentalShaderModels)
      return E_NOINTERFACE;
  if (configuration)
    return E_INVALIDARG;
  if (sizes)
    for (UINT i = 0; i < count; ++i)
      if (sizes[i])
        return E_INVALIDARG;
  return S_OK;
}
HRESULT ApplyGlobalRuntimeConfiguration(const RuntimeConfiguration &configuration) {
  std::vector<std::shared_ptr<ExperimentalDeviceRegistration>> devices;
  try {
    std::lock_guard lock(experimental_mutex);
    if (configuration == global_configuration) return S_OK;
    devices = experimental_devices;
    global_configuration = configuration;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  for (const auto &entry : devices) {
    std::lock_guard lock(entry->mutex);
    if (entry->device && !entry->device->independent_device)
      entry->device->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
  }
  return S_OK;
}
extern "C" HRESULT WINAPI
D3D12EnableExperimentalFeatures(UINT count, const IID *ids, void *configuration, UINT *sizes) {
  const auto hr = ValidateExperimentalFeatures(count, ids, configuration, sizes);
  if (FAILED(hr)) return hr;

  // Owned-backend policy: the public GUID opts in directly; this macOS
  // implementation does not inspect or modify Windows Developer Mode.
  const bool enabled = count != 0;
  std::vector<std::shared_ptr<ExperimentalDeviceRegistration>> devices;
  try {
    std::lock_guard lock(experimental_mutex);
    if (enabled == global_configuration.experimental_shader_models)
      return S_OK;
    devices = experimental_devices;
    global_configuration.experimental_shader_models = enabled;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  // Never call device/fence removal while holding the process registry lock.
  for (const auto &entry : devices) {
    std::lock_guard lock(entry->mutex);
    if (entry->device && !entry->device->independent_device)
      entry->device->ReportRemoval(DXGI_ERROR_DEVICE_REMOVED);
  }
  return S_OK;
}

// This identifies this source-built derivative, not API conformance. Tests
// must reject a silently selected Wine/GPTK/D3DMetal fallback implementation.
extern "C" const char *WINAPI
OwnedGraphicsBackendId() {
  return "owned-graphics-d3d12";
}

HRESULT
CreateConfiguredDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice,
                       const RuntimeConfiguration &configuration, bool independent) {
  if (ppDevice)
    *ppDevice = nullptr;
  if (MinimumFeatureLevel > kOwnedMaxFeatureLevel)
    return DXGI_ERROR_UNSUPPORTED;

  Com<IDXGIAdapter> dxgi_adapter = nullptr;
  Com<IDXGIFactory> dxgi_factory = nullptr;
  Com<IMTLDXGIAdapter> dxgi_adapter_mtl = nullptr;

  if (MinimumFeatureLevel < D3D_FEATURE_LEVEL_11_0)
    return E_INVALIDARG;

  HRESULT hr;

  if (!pAdapter) {
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));

    if (FAILED(hr)) {
      ERR("D3D12CreateDevice: Failed to create a DXGI factory");
      return hr;
    }

    if (FAILED(hr = dxgi_factory->EnumAdapters(0, &dxgi_adapter))) {
      ERR("D3D12CreateDevice: No default adapter available");
      return hr;
    }
  } else {
    dxgi_adapter = com_cast<IDXGIAdapter>(pAdapter);
    if (!dxgi_adapter)
      return E_INVALIDARG;
  }

  if (FAILED(hr = dxgi_adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter_mtl)))) {
    ERR("D3D12CreateDevice: Not a DXMT adapter");
    return hr;
  }

  if (!ppDevice)
    return S_FALSE;

  try {
    return dxmt::CreateD3D12Device(dxgi_adapter_mtl.ptr(), riid, ppDevice, configuration, independent);
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
extern "C" HRESULT WINAPI
D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **out) {
  return CreateGlobalSDKDevice(adapter, level, iid, out);
}

extern "C" HRESULT WINAPI
D3D12GetInterface(REFCLSID rcslid, REFIID iid, void **debug) {
  if (rcslid == CLSID_D3D12SDKConfiguration || rcslid == CLSID_D3D12DeviceFactory)
    return GetSDKInterface(rcslid, iid, debug);
  return GetDiagnosticsInterface(rcslid, iid, debug);
}

extern "C" HRESULT WINAPI
D3D12GetDebugInterface(REFIID iid, void **debug) {
  return GetDebugInterface(iid, debug);
}

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  DisableThreadLibraryCalls(instance);
  return TRUE;
}

} // namespace dxmt
