#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "../../external/nvngx/include/nvsdk_ngx.h"

using Microsoft::WRL::ComPtr;

namespace {
int failures = 0;
void expect(bool condition, const char *name) {
  std::printf("CAPABILITY_DISCOVERY_%s case=%s\n", condition ? "PASS" : "FAIL", name);
  failures += !condition;
}
template <typename T>
HRESULT query(ID3D12Device *device, D3D12_FEATURE feature, T &data) {
  return device->CheckFeatureSupport(feature, &data, sizeof(data));
}
template <typename T>
void invalidSizes(ID3D12Device *device, D3D12_FEATURE feature, T &data) {
  const T before = data;
  expect(device->CheckFeatureSupport(feature, &data, sizeof(data) - 1) == E_INVALIDARG &&
             std::memcmp(&before, &data, sizeof(data)) == 0, "short_size_preserves_data");
  expect(device->CheckFeatureSupport(feature, &data, sizeof(data) + 1) == E_INVALIDARG &&
             std::memcmp(&before, &data, sizeof(data)) == 0, "large_size_preserves_data");
  expect(device->CheckFeatureSupport(feature, nullptr, sizeof(data)) == E_INVALIDARG, "null_data");
}

// A public-only adapter wrapper delegates all DXGI operations to a real adapter,
// but does not expose implementation-private interfaces across the DLL boundary.
class PublicAdapter final : public IDXGIAdapter {
  IDXGIAdapter *adapter_;
  ULONG refs_ = 1;
public:
  explicit PublicAdapter(IDXGIAdapter *adapter) : adapter_(adapter) { adapter_->AddRef(); }
  ~PublicAdapter() { adapter_->Release(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IDXGIObject) && iid != __uuidof(IDXGIAdapter))
      return E_NOINTERFACE;
    *out = static_cast<IDXGIAdapter *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { const auto refs = --refs_; if (!refs) delete this; return refs; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void *p) override { return adapter_->SetPrivateData(g,n,p); }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown *p) override { return adapter_->SetPrivateDataInterface(g,p); }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *n, void *p) override { return adapter_->GetPrivateData(g,n,p); }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID i, void **p) override { return adapter_->GetParent(i,p); }
  HRESULT STDMETHODCALLTYPE EnumOutputs(UINT n, IDXGIOutput **p) override { return adapter_->EnumOutputs(n,p); }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *p) override { return adapter_->GetDesc(p); }
  HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID i, LARGE_INTEGER *v) override { return adapter_->CheckInterfaceSupport(i,v); }
};

void ngxDiscovery(IDXGIAdapter *adapter, ID3D12Device *device) {
  HMODULE module = LoadLibraryW(L"nvngx.dll");
  expect(module != nullptr, "ngx_load");
  if (!module) return;
  using Discover = decltype(&NVSDK_NGX_D3D12_GetFeatureRequirements);
  using Init = decltype(&NVSDK_NGX_D3D12_Init);
  using Shutdown = decltype(&NVSDK_NGX_D3D12_Shutdown1);
  auto discover = reinterpret_cast<Discover>(GetProcAddress(module, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
  auto init = reinterpret_cast<Init>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init"));
  auto shutdown = reinterpret_cast<Shutdown>(GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown1"));
  expect(discover && init && shutdown, "ngx_exports");
  if (!discover || !init || !shutdown) return;
  NVSDK_NGX_FeatureDiscoveryInfo info{};
  info.SDKVersion = NVSDK_NGX_Version_API;
  info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  info.ApplicationDataPath = L".";
  NVSDK_NGX_FeatureRequirement requirement{};
  std::memset(&requirement, 0xa5, sizeof(requirement));
  expect(discover(adapter, &info, &requirement) == NVSDK_NGX_Result_Success, "ngx_before_init");
  const auto initialized = init(1, L".", device, nullptr, NVSDK_NGX_Version_API);
  expect((initialized == NVSDK_NGX_Result_Success && requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported) ||
             (initialized == NVSDK_NGX_Result_FAIL_FeatureNotSupported && requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_AdapterUnsupported),
         "ngx_discovery_matches_init");
  expect(requirement.MinHWArchitecture == 0, "ngx_no_fake_nvidia_architecture");
  if (initialized == NVSDK_NGX_Result_Success) {
    expect(std::strcmp(requirement.MinOSVersion, "10.0.16299.0") == 0, "ngx_supported_os_requirement");
    expect(shutdown(device) == NVSDK_NGX_Result_Success, "ngx_shutdown");
  }
  ComPtr<IDXGIAdapter> public_adapter; public_adapter.Attach(new PublicAdapter(adapter));
  std::memset(&requirement, 0xa5, sizeof(requirement));
  expect(discover(public_adapter.Get(), &info, &requirement) == NVSDK_NGX_Result_Success &&
             requirement.FeatureSupported == NVSDK_NGX_FeatureSupportResult_AdapterUnsupported &&
             requirement.MinHWArchitecture == 0 && requirement.MinOSVersion[0] == 0,
         "ngx_non_owned_adapter_unsupported");
  expect(discover(nullptr, &info, &requirement) == NVSDK_NGX_Result_FAIL_InvalidParameter, "ngx_null_adapter");
  expect(discover(adapter, nullptr, &requirement) == NVSDK_NGX_Result_FAIL_InvalidParameter, "ngx_null_info");
  expect(discover(adapter, &info, nullptr) == NVSDK_NGX_Result_FAIL_InvalidParameter, "ngx_null_output");
  info.FeatureID = NVSDK_NGX_Feature_ImageSuperResolution;
  expect(discover(adapter, &info, &requirement) == NVSDK_NGX_Result_FAIL_FeatureNotSupported, "ngx_unknown_feature_error");
  FreeLibrary(module);
}

void coreDiscovery(ID3D12Device *device) {
  std::printf("CAPABILITY_DISCOVERY_ABI pointer=%zu options10=%zu queue_priority=%zu existing_heaps=%zu "
              "cross_node=%zu displayable=%zu protected_support=%zu protected_count=%zu protected_types=%zu meta=%zu\n",
              sizeof(void *), sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS10),
              sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY), sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS),
              sizeof(D3D12_FEATURE_DATA_CROSS_NODE), sizeof(D3D12_FEATURE_DATA_DISPLAYABLE),
              sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT),
              sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT),
              sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES), sizeof(D3D12_FEATURE_DATA_QUERY_META_COMMAND));
  D3D12_FEATURE_DATA_D3D12_OPTIONS10 options10{FALSE, FALSE};
  expect(query(device, D3D12_FEATURE_D3D12_OPTIONS10, options10) == S_OK &&
             options10.VariableRateShadingSumCombinerSupported && options10.MeshShaderPerPrimitiveShadingRateSupported, "options10_existing_vrs");
  invalidSizes(device, D3D12_FEATURE_D3D12_OPTIONS10, options10);
  D3D12_FEATURE_DATA_EXISTING_HEAPS existing{};
  expect(query(device, D3D12_FEATURE_EXISTING_HEAPS, existing) == S_OK && existing.Supported, "existing_heaps_available");
  invalidSizes(device, D3D12_FEATURE_EXISTING_HEAPS, existing);
  D3D12_FEATURE_DATA_CROSS_NODE cross{};
  expect(query(device, D3D12_FEATURE_CROSS_NODE, cross) == S_OK &&
             cross.SharingTier == D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED && !cross.AtomicShaderInstructions, "cross_node_negative");
  invalidSizes(device, D3D12_FEATURE_CROSS_NODE, cross);
  D3D12_FEATURE_DATA_DISPLAYABLE displayable{};
  expect(query(device, D3D12_FEATURE_DISPLAYABLE, displayable) == S_OK && !displayable.DisplayableTexture &&
             displayable.SharedResourceCompatibilityTier == D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0, "displayable_negative");
  invalidSizes(device, D3D12_FEATURE_DISPLAYABLE, displayable);
  for (auto type : {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY,
                   D3D12_COMMAND_LIST_TYPE_BUNDLE, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                   D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE}) {
    for (UINT priority : {UINT(D3D12_COMMAND_QUEUE_PRIORITY_NORMAL), UINT(D3D12_COMMAND_QUEUE_PRIORITY_HIGH),
                          UINT(D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME), UINT(0xffffffff)}) {
      D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY queue{type, priority, TRUE};
      const bool supported = priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL &&
          (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE || type == D3D12_COMMAND_LIST_TYPE_COPY);
      expect(query(device, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, queue) == S_OK && queue.CommandListType == type &&
                 queue.Priority == priority && bool(queue.PriorityForTypeIsSupported) == supported, "queue_priority_contract");
    }
  }
  D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY queue{static_cast<D3D12_COMMAND_LIST_TYPE>(99), 0, FALSE};
  expect(query(device, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, queue) == E_INVALIDARG, "queue_unknown_type");
  invalidSizes(device, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, queue);
  D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT support{0, D3D12_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_SUPPORTED};
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT, support) == S_OK &&
             support.NodeIndex == 0 && support.Support == D3D12_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_NONE, "protected_negative");
  invalidSizes(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT, support);
  support.NodeIndex = 1;
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT, support) == E_INVALIDARG, "protected_bad_node");
  D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPE_COUNT count{0, 99};
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT, count) == S_OK && count.Count == 0 && !count.NodeIndex, "protected_count_zero");
  invalidSizes(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT, count);
  count.NodeIndex = 1;
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPE_COUNT, count) == E_INVALIDARG, "protected_count_bad_node");
  GUID sentinel; std::memset(&sentinel, 0xa5, sizeof(sentinel)); const GUID before = sentinel;
  D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_TYPES types{0, 0, &sentinel};
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES, types) == S_OK && !types.Count &&
             types.pTypes == &sentinel && std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0, "protected_empty_types_no_write");
  invalidSizes(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES, types);
  types.pTypes = nullptr;
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES, types) == S_OK, "protected_empty_null_types");
  types.Count = 1;
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES, types) == E_INVALIDARG && types.Count == 1, "protected_count_must_match");
  types.Count = 0; types.NodeIndex = 1;
  expect(query(device, D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_TYPES, types) == E_INVALIDARG, "protected_types_bad_node");
  UINT opaque = 0x12345678;
  D3D12_FEATURE_DATA_QUERY_META_COMMAND meta{};
  meta.pQueryOutputData = &opaque; meta.QueryOutputDataSizeInBytes = sizeof(opaque);
  expect(query(device, D3D12_FEATURE_QUERY_META_COMMAND, meta) == E_INVALIDARG && opaque == 0x12345678, "meta_unknown_guid_no_fake_output");
  invalidSizes(device, D3D12_FEATURE_QUERY_META_COMMAND, meta);
  for (UINT id : {9u, 11u, 48u, 49u, 50u, 51u, 52u, 54u, 0x7fffu}) {
    UINT unknown = 0x12345678;
    expect(query(device, static_cast<D3D12_FEATURE>(id), unknown) == E_INVALIDARG && unknown == 0x12345678, "unknown_revision_boundary");
  }
  D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_HIGHEST_SHADER_MODEL};
  expect(query(device, D3D12_FEATURE_SHADER_MODEL, model) == S_OK && model.HighestShaderModel <= D3D_SHADER_MODEL_6_5, "preserved_shader_cap");
  model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0x68);
  expect(query(device, D3D12_FEATURE_SHADER_MODEL, model) == E_INVALIDARG && UINT(model.HighestShaderModel) == 0x68, "unknown_shader_revision");
}
}

int main(int argc, char **argv) {
  ComPtr<IDXGIFactory4> factory;
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) { std::printf("factory hr=%08lx\n", (unsigned long)hr); return 2; }
  ComPtr<IDXGIAdapter1> adapter;
  hr = factory->EnumAdapters1(0, &adapter);
  if (FAILED(hr)) return 2;
  ComPtr<ID3D12Device> device;
  hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr)) { std::printf("device hr=%08lx\n", (unsigned long)hr); return 2; }
  if (argc > 1 && !std::strcmp(argv[1], "--ngx")) ngxDiscovery(adapter.Get(), device.Get());
  else coreDiscovery(device.Get());
  std::printf("CAPABILITY_DISCOVERY_RESULT failures=%d\n", failures);
  return failures ? 1 : 0;
}
