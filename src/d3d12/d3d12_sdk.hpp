// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12_core_bridge.hpp"
#include <memory>
extern "C" const GUID CLSID_D3D12DeviceFactory;
namespace d3dalloy {
struct OwnedCoreModule;
// COM objects live in Main: last Release never executes in an unloadable Core.
// Core owns the factory state, locking and behavior behind this private POD ABI.
struct CoreFactoryConfiguration {
  const std::shared_ptr<OwnedCoreModule> module;
  const OwnedD3D12CoreBridge1 *const bridge;
  void *state = nullptr;
  explicit CoreFactoryConfiguration(std::shared_ptr<OwnedCoreModule>);
  ~CoreFactoryConfiguration();
};
HRESULT SelectCoreFactory(UINT version, const char *directory, std::shared_ptr<CoreFactoryConfiguration> &out);
HRESULT CreateCoreFactoryInterface(const std::shared_ptr<CoreFactoryConfiguration> &, REFIID, void **);
HRESULT GetSDKInterface(REFCLSID, REFIID, void **);
HRESULT CreateGlobalSDKDevice(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
}
