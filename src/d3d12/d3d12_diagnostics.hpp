// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "d3d12.h"
#include "d3d12sdklayers.h"
#include <memory>
#include <string>
#include <vector>
#include <utility>
namespace dxmt {
class MTLD3D12Device;
// Device-creation snapshot; selected Core identity and lease follow the device.
struct RuntimeConfiguration {
  bool debug = false, experimental_shader_models = false;
  D3D12_DRED_ENABLEMENT breadcrumbs = D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED;
  D3D12_DRED_ENABLEMENT contexts = D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED;
  UINT sdk_version = 0;
  UINT factory_flags = 0;
  std::shared_ptr<void> sdk_module_lifetime;
  bool operator==(const RuntimeConfiguration &) const = default;
};
RuntimeConfiguration GetGlobalRuntimeConfiguration();
HRESULT ApplyGlobalRuntimeConfiguration(const RuntimeConfiguration &configuration);
HRESULT ValidateExperimentalFeatures(UINT count, const IID *ids, void *configuration, UINT *sizes);
void EnableGlobalDebugLayer();
void SetGlobalDredEnablement(bool contexts, D3D12_DRED_ENABLEMENT enablement);
HRESULT CreateConfiguredDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **out,
                               const RuntimeConfiguration &configuration, bool independent);
struct DiagnosticBreadcrumb {
  D3D12_AUTO_BREADCRUMB_NODE node{};
  D3D12_AUTO_BREADCRUMB_NODE1 node1{};
  std::vector<D3D12_AUTO_BREADCRUMB_OP> history;
  std::wstring list_name, queue_name;
  std::vector<std::wstring> context_strings;
  std::vector<D3D12_DRED_BREADCRUMB_CONTEXT> contexts;
  UINT completed = 0;
};
class DeviceDiagnostics {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  explicit DeviceDiagnostics(MTLD3D12Device *device);
  void Configure(const RuntimeConfiguration &configuration);
  ~DeviceDiagnostics();
  HRESULT QueryInterface(REFIID iid, void **object);
  void Validation(D3D12_MESSAGE_ID id, const char *description);
  void Removed(HRESULT reason);
  std::shared_ptr<DiagnosticBreadcrumb> BeginSubmission(ID3D12CommandList *list, ID3D12CommandQueue *queue,
      std::vector<D3D12_AUTO_BREADCRUMB_OP> history, const std::vector<std::pair<UINT, std::string>> &contexts = {});
  void Complete(const std::shared_ptr<DiagnosticBreadcrumb> &breadcrumb);
};
HRESULT GetDiagnosticsInterface(REFCLSID clsid, REFIID iid, void **object, std::shared_ptr<void> core_lifetime = {});
HRESULT GetDebugInterface(REFIID iid, void **object, std::shared_ptr<void> core_lifetime = {});
void RemoveDevicesForDebugLayer();
bool HasGlobalDevices();
// Annotation payload is retained for native diagnostics only, never written to owned_trace.
struct DiagnosticAnnotation {
  UINT action = 0, metadata = 0;
  std::vector<unsigned char> payload;
  std::string label;
};
DiagnosticAnnotation CaptureAnnotation(UINT action, UINT metadata, const void *data, UINT size);
}

