// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12_core_bridge.hpp"
#include "d3d12_core_generation.h"
#include <cstring>
#include <mutex>
#include <new>

extern "C" const UINT D3D12SDKVersion = 618;
extern "C" const char OwnedD3D12CoreGeneration[33] = OWNED_D3D12_CORE_GENERATION;
// Distinct implementation names keep i686 .def exports from binding Main import thunks.
extern "C" const char *WINAPI OwnedCoreGraphicsBackendId() { return "owned-graphics-d3d12core"; }

namespace {
struct Factory {
  std::mutex mutex;
  OwnedCoreConfiguration1 configuration{};
  OwnedD3D12MainBridge1 main;
  explicit Factory(const OwnedD3D12MainBridge1 &bridge) : main(bridge) {}
};
HRESULT WINAPI CreateFactory(const OwnedD3D12MainBridge1 *main, void **out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (!main || main->size != sizeof(*main) || main->version != 1 ||
      main->configuration_size != sizeof(OwnedCoreConfiguration1) || main->pointer_size != sizeof(void *) ||
      !main->generation || std::strcmp(main->generation, OwnedD3D12CoreGeneration) ||
      !main->get_global || !main->apply_global || !main->create_device || !main->get_interface)
    return E_NOINTERFACE;
  try { *out = new Factory(*main); return S_OK; }
  catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
void WINAPI DestroyFactory(void *state) { delete static_cast<Factory *>(state); }
HRESULT WINAPI Initialize(void *state, void *lease) {
  auto &f = *static_cast<Factory *>(state);
  OwnedCoreConfiguration1 value;
  HRESULT hr = f.main.get_global(lease, &value); if (FAILED(hr)) return hr;
  std::lock_guard lock(f.mutex); f.configuration = value;
  return S_OK;
}
HRESULT WINAPI Apply(void *state, void *lease) {
  auto &f = *static_cast<Factory *>(state);
  OwnedCoreConfiguration1 value;
  { std::lock_guard lock(f.mutex); value = f.configuration; }
  return f.main.apply_global(lease, &value);
}
void WINAPI GetConfiguration(void *state, OwnedCoreConfiguration1 *out) {
  auto &f = *static_cast<Factory *>(state);
  std::lock_guard lock(f.mutex); *out = f.configuration;
}
HRESULT WINAPI SetFlags(void *state, UINT flags) {
  // These flags permit reuse; they do not require singleton devices.
  if ((flags & ~7u) || ((flags & 2u) && !(flags & 1u))) return E_INVALIDARG;
  auto &f = *static_cast<Factory *>(state);
  std::lock_guard lock(f.mutex); f.configuration.factory_flags = flags; return S_OK;
}
UINT WINAPI GetFlags(void *state) {
  auto &f = *static_cast<Factory *>(state);
  std::lock_guard lock(f.mutex); return f.configuration.factory_flags;
}
void WINAPI EnableDebug(void *state) {
  auto &f = *static_cast<Factory *>(state);
  std::lock_guard lock(f.mutex); f.configuration.debug = 1;
}
void WINAPI SetDred(void *state, BOOL contexts, UINT enablement) {
  if (enablement > 2) return;
  auto &f = *static_cast<Factory *>(state);
  std::lock_guard lock(f.mutex);
  (contexts ? f.configuration.contexts : f.configuration.breadcrumbs) = enablement;
}
HRESULT WINAPI EnableExperimental(void *state, UINT count, const IID *ids, void *configuration, UINT *sizes) {
  if (count && !ids) return E_INVALIDARG;
  for (UINT i = 0; i < count; ++i)
    if (ids[i] != D3D12ExperimentalShaderModels) return E_NOINTERFACE;
  if (configuration) return E_INVALIDARG;
  if (sizes) for (UINT i = 0; i < count; ++i) if (sizes[i]) return E_INVALIDARG;
  auto &f = *static_cast<Factory *>(state);
  std::lock_guard lock(f.mutex); f.configuration.experimental_shader_models = count != 0;
  return S_OK;
}
HRESULT WINAPI CreateDevice(void *state, void *lease, IUnknown *adapter, D3D_FEATURE_LEVEL level,
                          REFIID iid, void **out, BOOL independent) {
  if (out) *out = nullptr;
  auto &f = *static_cast<Factory *>(state);
  OwnedCoreConfiguration1 value;
  { std::lock_guard lock(f.mutex); value = f.configuration; }
  return f.main.create_device(lease, adapter, level, iid, out, &value, independent);
}
const OwnedD3D12CoreBridge1 bridge = {
  sizeof(OwnedD3D12CoreBridge1), 1, sizeof(OwnedCoreConfiguration1), sizeof(void *), 618,
  OwnedD3D12CoreGeneration, CreateFactory, DestroyFactory, Initialize, Apply, GetConfiguration, SetFlags, GetFlags,
  EnableDebug, SetDred, EnableExperimental, CreateDevice
};
}
extern "C" const OwnedD3D12CoreBridge1 *WINAPI OwnedD3D12CoreGetBridge1(UINT version, UINT size) {
  return version == 1 && size == sizeof(bridge) ? &bridge : nullptr;
}
extern "C" HRESULT WINAPI OwnedCoreD3D12GetInterface(REFCLSID clsid, REFIID iid, void **out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  const auto *main = OwnedD3D12MainGetBridge1(1, sizeof(OwnedD3D12MainBridge1));
  if (!main || std::strcmp(main->generation, OwnedD3D12CoreGeneration)) return E_NOINTERFACE;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&OwnedCoreD3D12GetInterface), &self)) return HRESULT_FROM_WIN32(GetLastError());
  return main->get_interface(self, clsid, iid, out);
}
