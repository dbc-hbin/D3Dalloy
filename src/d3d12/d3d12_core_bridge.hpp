// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12.h"
#include <type_traits>

// Private, same-build ABI. No C++ ownership, STL objects or exceptions cross DLLs.
struct OwnedCoreConfiguration1 {
  UINT debug, experimental_shader_models, breadcrumbs, contexts, factory_flags;
};
struct OwnedD3D12MainBridge1 {
  UINT size, version, configuration_size, pointer_size;
  const char *generation;
  HRESULT (WINAPI *get_global)(void *lease, OwnedCoreConfiguration1 *);
  HRESULT (WINAPI *apply_global)(void *lease, const OwnedCoreConfiguration1 *);
  HRESULT (WINAPI *create_device)(void *lease, IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **,
                                const OwnedCoreConfiguration1 *, BOOL independent);
  HRESULT (WINAPI *get_interface)(HMODULE core, REFCLSID, REFIID, void **);
};
struct OwnedD3D12CoreBridge1 {
  UINT size, version, configuration_size, pointer_size, sdk_version;
  const char *generation;
  HRESULT (WINAPI *create_factory)(const OwnedD3D12MainBridge1 *, void **);
  void (WINAPI *destroy_factory)(void *);
  HRESULT (WINAPI *initialize)(void *, void *lease);
  HRESULT (WINAPI *apply)(void *, void *lease);
  void (WINAPI *get_configuration)(void *, OwnedCoreConfiguration1 *);
  HRESULT (WINAPI *set_flags)(void *, UINT);
  UINT (WINAPI *get_flags)(void *);
  void (WINAPI *enable_debug)(void *);
  void (WINAPI *set_dred)(void *, BOOL contexts, UINT);
  HRESULT (WINAPI *enable_experimental)(void *, UINT, const IID *, void *, UINT *);
  HRESULT (WINAPI *create_device)(void *, void *lease, IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **, BOOL independent);
};
using OwnedCoreGetBridge1 = const OwnedD3D12CoreBridge1 *(WINAPI *)(UINT, UINT);
extern "C" const OwnedD3D12MainBridge1 *WINAPI OwnedD3D12MainGetBridge1(UINT, UINT);
static_assert(std::is_standard_layout_v<OwnedCoreConfiguration1> && std::is_trivial_v<OwnedCoreConfiguration1>);
static_assert(std::is_standard_layout_v<OwnedD3D12CoreBridge1> && std::is_trivial_v<OwnedD3D12CoreBridge1>);
static_assert(std::is_standard_layout_v<OwnedD3D12MainBridge1> && std::is_trivial_v<OwnedD3D12MainBridge1>);
