// SPDX-License-Identifier: LGPL-2.1-or-later
#include "d3d12_diagnostics.hpp"
#include "d3d12_device.hpp"
#include "d3d12_sdk.hpp"
#include "com/com_object.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>

// The selected MinGW import libraries declare these public CLSIDs but do not
// define them. Own only the factory GUIDs referenced by this translation unit.
extern "C" const GUID CLSID_D3D12Debug =
    {0xf2352aeb, 0xdd84, 0x49fe, {0xb9, 0x7b, 0xa9, 0xdc, 0xfd, 0xcc, 0x1b, 0x4f}};
extern "C" const GUID CLSID_D3D12DeviceRemovedExtendedData =
    {0x4a75bbc4, 0x9ff4, 0x4ad8, {0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2}};
extern "C" const GUID CLSID_D3D12SDKConfiguration =
    {0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
extern "C" const GUID CLSID_D3D12DeviceFactory =
    {0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};

namespace dxmt {
namespace {
using FactoryConfiguration = CoreFactoryConfiguration;

struct FilterDesc {
  std::vector<D3D12_MESSAGE_CATEGORY> categories;
  std::vector<D3D12_MESSAGE_SEVERITY> severities;
  std::vector<D3D12_MESSAGE_ID> ids;
  void Append(const D3D12_INFO_QUEUE_FILTER_DESC &d) {
    if ((d.NumCategories && !d.pCategoryList) || (d.NumSeverities && !d.pSeverityList) || (d.NumIDs && !d.pIDList))
      throw std::invalid_argument("filter");
    if (d.NumCategories) categories.insert(categories.end(), d.pCategoryList, d.pCategoryList + d.NumCategories);
    if (d.NumSeverities) severities.insert(severities.end(), d.pSeverityList, d.pSeverityList + d.NumSeverities);
    if (d.NumIDs) ids.insert(ids.end(), d.pIDList, d.pIDList + d.NumIDs);
  }
  bool Empty() const { return categories.empty() && severities.empty() && ids.empty(); }
  bool Matches(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id) const {
    return std::find(categories.begin(), categories.end(), c) != categories.end() ||
           std::find(severities.begin(), severities.end(), s) != severities.end() ||
           std::find(ids.begin(), ids.end(), id) != ids.end();
  }
};
struct Filter {
  FilterDesc allow, deny;
  void Append(const D3D12_INFO_QUEUE_FILTER &f) { allow.Append(f.AllowList); deny.Append(f.DenyList); }
  bool Allows(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id) const {
    return (allow.Empty() || allow.Matches(c, s, id)) && !deny.Matches(c, s, id);
  }
};
struct StoredMessage {
  D3D12_MESSAGE_CATEGORY category;
  D3D12_MESSAGE_SEVERITY severity;
  D3D12_MESSAGE_ID id;
  std::string description;
};
class DebugLayer final : public ComObject<ID3D12Debug> {
  std::shared_ptr<FactoryConfiguration> configuration_;
  std::shared_ptr<void> core_lifetime_;
public:
  explicit DebugLayer(std::shared_ptr<FactoryConfiguration> configuration = {}, std::shared_ptr<void> core_lifetime = {})
    : configuration_(std::move(configuration)), core_lifetime_(std::move(core_lifetime)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Debug)) return E_NOINTERFACE;
    *out = static_cast<ID3D12Debug *>(this); AddRef(); return S_OK;
  }
  void STDMETHODCALLTYPE EnableDebugLayer() override {
    if (configuration_) {
      configuration_->bridge->enable_debug(configuration_->state);
    } else EnableGlobalDebugLayer();
  }
};
class DredSettings final : public ComObject<ID3D12DeviceRemovedExtendedDataSettings1> {
  std::shared_ptr<FactoryConfiguration> configuration_;
  std::shared_ptr<void> core_lifetime_;
public:
  explicit DredSettings(std::shared_ptr<FactoryConfiguration> configuration = {}, std::shared_ptr<void> core_lifetime = {})
    : configuration_(std::move(configuration)), core_lifetime_(std::move(core_lifetime)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12DeviceRemovedExtendedDataSettings) &&
        iid != __uuidof(ID3D12DeviceRemovedExtendedDataSettings1)) return E_NOINTERFACE;
    *out = static_cast<ID3D12DeviceRemovedExtendedDataSettings1 *>(this); AddRef(); return S_OK;
  }
  void STDMETHODCALLTYPE SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT e) override {
    if (e >= D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED && e <= D3D12_DRED_ENABLEMENT_FORCED_ON) {
      if (configuration_) configuration_->bridge->set_dred(configuration_->state, FALSE, UINT(e));
      else SetGlobalDredEnablement(false, e);
    }
  }
  void STDMETHODCALLTYPE SetPageFaultEnablement(D3D12_DRED_ENABLEMENT e) override {
    if (e == D3D12_DRED_ENABLEMENT_FORCED_ON) WARN("DRED: Metal does not expose a fault GPU virtual address; allocation queries return DXGI_ERROR_UNSUPPORTED");
  }
  void STDMETHODCALLTYPE SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT e) override {
    if (e == D3D12_DRED_ENABLEMENT_FORCED_ON) WARN("DRED: Windows Watson dump service is unavailable in this owned runtime");
  }
  void STDMETHODCALLTYPE SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT e) override {
    if (e >= D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED && e <= D3D12_DRED_ENABLEMENT_FORCED_ON) {
      if (configuration_) configuration_->bridge->set_dred(configuration_->state, TRUE, UINT(e));
      else SetGlobalDredEnablement(true, e);
    }
  }
};
class DeviceFactory final : public ComObject<ID3D12DeviceFactory, ID3D12DeviceConfiguration> {
  std::shared_ptr<FactoryConfiguration> configuration_;
public:
  explicit DeviceFactory(std::shared_ptr<FactoryConfiguration> configuration) : configuration_(std::move(configuration)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12DeviceFactory))
      *out = static_cast<ID3D12DeviceFactory *>(this);
    else if (iid == __uuidof(ID3D12DeviceConfiguration))
      *out = static_cast<ID3D12DeviceConfiguration *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_DEVICE_CONFIGURATION_DESC *STDMETHODCALLTYPE GetDesc(D3D12_DEVICE_CONFIGURATION_DESC *out) override {
#else
  D3D12_DEVICE_CONFIGURATION_DESC STDMETHODCALLTYPE GetDesc() override {
    D3D12_DEVICE_CONFIGURATION_DESC value; auto *out = &value;
#endif
    OwnedCoreConfiguration1 c; configuration_->bridge->get_configuration(configuration_->state, &c);
    UINT flags = c.debug ? D3D12_DEVICE_FLAG_DEBUG_LAYER_ENABLED : 0;
    if (c.breadcrumbs != D3D12_DRED_ENABLEMENT_FORCED_OFF) flags |= D3D12_DEVICE_FLAG_DRED_AUTO_BREADCRUMBS_ENABLED;
    if (c.contexts == D3D12_DRED_ENABLEMENT_FORCED_ON) flags |= D3D12_DEVICE_FLAG_DRED_BREADCRUMB_CONTEXT_ENABLED;
    *out = {D3D12_DEVICE_FLAGS(flags), 0, configuration_->bridge->sdk_version, UINT(c.experimental_shader_models != 0)};
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    return out;
#else
    return value;
#endif
  }
  HRESULT STDMETHODCALLTYPE GetEnabledExperimentalFeatures(GUID *guids, UINT count) override {
    OwnedCoreConfiguration1 c; configuration_->bridge->get_configuration(configuration_->state, &c);
    if (count < UINT(c.experimental_shader_models != 0)) return E_INVALIDARG;
    if (c.experimental_shader_models) {
      if (!guids) return E_POINTER;
      guids[0] = D3D12ExperimentalShaderModels;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc,
                                                           ID3DBlob **result, ID3DBlob **error) override {
    return ::D3D12SerializeVersionedRootSignature(desc, result, error);
  }
  HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(const void *blob, SIZE_T size, REFIID iid, void **out) override {
    return ::D3D12CreateVersionedRootSignatureDeserializer(blob, size, iid, out);
  }
  HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() override {
    return configuration_->bridge->initialize(configuration_->state, configuration_.get());
  }
  HRESULT STDMETHODCALLTYPE ApplyToGlobalState() override {
    return configuration_->bridge->apply(configuration_->state, configuration_.get());
  }
  HRESULT STDMETHODCALLTYPE SetFlags(D3D12_DEVICE_FACTORY_FLAGS flags) override {
    return configuration_->bridge->set_flags(configuration_->state, UINT(flags));
  }
  D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE GetFlags() override {
    return D3D12_DEVICE_FACTORY_FLAGS(configuration_->bridge->get_flags(configuration_->state));
  }
  HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid, REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    try {
      if (clsid == CLSID_D3D12Debug) {
        auto object = std::make_unique<DebugLayer>(configuration_);
        auto hr = object->QueryInterface(iid, out); if (SUCCEEDED(hr)) object.release(); return hr;
      }
      if (clsid == CLSID_D3D12DeviceRemovedExtendedData) {
        auto object = std::make_unique<DredSettings>(configuration_);
        auto hr = object->QueryInterface(iid, out); if (SUCCEEDED(hr)) object.release(); return hr;
      }
      return E_NOINTERFACE;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(UINT count, const IID *ids, void *configuration, UINT *sizes) override {
    return configuration_->bridge->enable_experimental(configuration_->state, count, ids, configuration, sizes);
  }
  HRESULT STDMETHODCALLTYPE CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **out) override {
    return configuration_->bridge->create_device(configuration_->state, configuration_.get(), adapter, level, iid, out, TRUE);
  }
};
}
HRESULT CreateCoreFactoryInterface(const std::shared_ptr<CoreFactoryConfiguration> &configuration, REFIID iid, void **out) {
  if (!out) return E_POINTER; *out = nullptr;
  try {
    auto object = std::make_unique<DeviceFactory>(configuration);
    HRESULT hr = object->QueryInterface(iid, out); if (SUCCEEDED(hr)) object.release(); return hr;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

struct DeviceDiagnostics::Impl final : ID3D12InfoQueue1, ID3D12DeviceRemovedExtendedData2 {
  MTLD3D12Device *device;
  bool validation_enabled, capture_enabled, context_enabled;
  std::recursive_mutex mutex;
  std::recursive_mutex callback_dispatch;
  std::deque<StoredMessage> messages;
  std::vector<Filter> storage{1}, retrieval{1};
  UINT64 limit = 1024, allowed = 0, denied = 0, discarded = 0;
  bool mute = false;
  std::set<D3D12_MESSAGE_CATEGORY> break_categories;
  std::set<D3D12_MESSAGE_SEVERITY> break_severities;
  std::set<D3D12_MESSAGE_ID> break_ids;
  struct Callback {
    bool active = true;
    D3D12MessageFunc function;
    D3D12_MESSAGE_CALLBACK_FLAGS flags;
    void *context;
  };
  std::map<DWORD, std::shared_ptr<Callback>> callbacks;
  DWORD next_cookie = 1;
  std::deque<std::shared_ptr<DiagnosticBreadcrumb>> breadcrumbs;
  explicit Impl(MTLD3D12Device *d) : device(d), validation_enabled(false), capture_enabled(false), context_enabled(false) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override { return device->QueryInterface(iid, out); }
  ULONG STDMETHODCALLTYPE AddRef() override { return device->AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return device->Release(); }
  HRESULT Query(REFIID iid, void **out) {
    if (iid == __uuidof(ID3D12InfoQueue) || iid == __uuidof(ID3D12InfoQueue1)) *out = static_cast<ID3D12InfoQueue1 *>(this);
    else if (iid == __uuidof(ID3D12DeviceRemovedExtendedData) || iid == __uuidof(ID3D12DeviceRemovedExtendedData1) ||
             iid == __uuidof(ID3D12DeviceRemovedExtendedData2)) *out = static_cast<ID3D12DeviceRemovedExtendedData2 *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetMessageCountLimit(UINT64 value) override {
    std::lock_guard lock(mutex); limit = value;
    while (messages.size() > limit) { messages.pop_back(); ++discarded; }
    return S_OK;
  }
  void STDMETHODCALLTYPE ClearStoredMessages() override {
    std::lock_guard lock(mutex); messages.clear(); allowed = denied = discarded = 0;
  }
  HRESULT STDMETHODCALLTYPE GetMessage(UINT64 index, D3D12_MESSAGE *out, SIZE_T *length) override {
    if (!length) return E_INVALIDARG;
    std::lock_guard lock(mutex);
    for (const auto &message : messages) {
      if (!retrieval.back().Allows(message.category, message.severity, message.id)) continue;
      if (index--) continue;
      const SIZE_T required = sizeof(D3D12_MESSAGE) + message.description.size() + 1;
      const auto available = *length; *length = required;
      if (!out) return S_OK;
      if (available < required) return DXGI_ERROR_MORE_DATA;
      auto *description = reinterpret_cast<char *>(out + 1);
      memcpy(description, message.description.c_str(), message.description.size() + 1);
      *out = {message.category, message.severity, message.id, description, message.description.size() + 1};
      return S_OK;
    }
    return E_INVALIDARG;
  }
  UINT64 STDMETHODCALLTYPE GetNumMessagesAllowedByStorageFilter() override { std::lock_guard lock(mutex); return allowed; }
  UINT64 STDMETHODCALLTYPE GetNumMessagesDeniedByStorageFilter() override { std::lock_guard lock(mutex); return denied; }
  UINT64 STDMETHODCALLTYPE GetNumStoredMessages() override { std::lock_guard lock(mutex); return messages.size(); }
  UINT64 STDMETHODCALLTYPE GetNumStoredMessagesAllowedByRetrievalFilter() override {
    std::lock_guard lock(mutex);
    return std::count_if(messages.begin(), messages.end(), [&](const auto &m) { return retrieval.back().Allows(m.category, m.severity, m.id); });
  }
  UINT64 STDMETHODCALLTYPE GetNumMessagesDiscardedByMessageCountLimit() override { std::lock_guard lock(mutex); return discarded; }
  UINT64 STDMETHODCALLTYPE GetMessageCountLimit() override { std::lock_guard lock(mutex); return limit; }
  HRESULT AddFilter(std::vector<Filter> &stack, D3D12_INFO_QUEUE_FILTER *filter, bool push) {
    if (!filter) return E_INVALIDARG;
    try {
      std::lock_guard lock(mutex);
      Filter next = push ? Filter{} : stack.back(); next.Append(*filter);
      if (push) stack.push_back(std::move(next)); else stack.back() = std::move(next);
      return S_OK;
    } catch (const std::invalid_argument &) { return E_INVALIDARG; }
      catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  HRESULT GetFilter(const std::vector<Filter> &stack, D3D12_INFO_QUEUE_FILTER *out, SIZE_T *length) {
    if (!length) return E_INVALIDARG;
    std::lock_guard lock(mutex); const auto &f = stack.back();
    const SIZE_T required = sizeof(*out) + (f.allow.categories.size() + f.deny.categories.size()) * sizeof(D3D12_MESSAGE_CATEGORY) +
      (f.allow.severities.size() + f.deny.severities.size()) * sizeof(D3D12_MESSAGE_SEVERITY) +
      (f.allow.ids.size() + f.deny.ids.size()) * sizeof(D3D12_MESSAGE_ID);
    auto available = *length; *length = required;
    if (!out) return S_OK;
    if (available < required) return DXGI_ERROR_MORE_DATA;
    auto *cursor = reinterpret_cast<unsigned char *>(out + 1);
    auto copy = [&]<typename T>(const std::vector<T> &values, UINT &count, T *&pointer) {
      count = values.size(); pointer = values.empty() ? nullptr : reinterpret_cast<T *>(cursor);
      if (!values.empty()) memcpy(cursor, values.data(), values.size() * sizeof(T));
      cursor += values.size() * sizeof(T);
    };
    *out = {};
    for (auto pair : {std::pair{&f.allow, &out->AllowList}, std::pair{&f.deny, &out->DenyList}}) {
      copy(pair.first->categories, pair.second->NumCategories, pair.second->pCategoryList);
      copy(pair.first->severities, pair.second->NumSeverities, pair.second->pSeverityList);
      copy(pair.first->ids, pair.second->NumIDs, pair.second->pIDList);
    }
    return S_OK;
  }
#define FILTER_METHODS(Name, stack) \
  HRESULT STDMETHODCALLTYPE Add##Name##FilterEntries(D3D12_INFO_QUEUE_FILTER *f) override { return AddFilter(stack, f, false); } \
  HRESULT STDMETHODCALLTYPE Get##Name##Filter(D3D12_INFO_QUEUE_FILTER *f, SIZE_T *n) override { return GetFilter(stack, f, n); } \
  void STDMETHODCALLTYPE Clear##Name##Filter() override { std::lock_guard lock(mutex); stack.back() = {}; } \
  HRESULT STDMETHODCALLTYPE PushEmpty##Name##Filter() override { try { std::lock_guard lock(mutex); stack.emplace_back(); return S_OK; } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; } } \
  HRESULT STDMETHODCALLTYPE PushCopyOf##Name##Filter() override { try { std::lock_guard lock(mutex); Filter copy = stack.back(); stack.push_back(std::move(copy)); return S_OK; } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; } } \
  HRESULT STDMETHODCALLTYPE Push##Name##Filter(D3D12_INFO_QUEUE_FILTER *f) override { return AddFilter(stack, f, true); } \
  void STDMETHODCALLTYPE Pop##Name##Filter() override { std::lock_guard lock(mutex); if (stack.size() > 1) stack.pop_back(); } \
  UINT STDMETHODCALLTYPE Get##Name##FilterStackSize() override { std::lock_guard lock(mutex); return stack.size(); }
  FILTER_METHODS(Storage, storage)
  FILTER_METHODS(Retrieval, retrieval)
#undef FILTER_METHODS
  HRESULT STDMETHODCALLTYPE AddMessage(D3D12_MESSAGE_CATEGORY c, D3D12_MESSAGE_SEVERITY s, D3D12_MESSAGE_ID id, const char *text) override {
    if (!text || c < D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED || c > D3D12_MESSAGE_CATEGORY_SHADER ||
        s < D3D12_MESSAGE_SEVERITY_CORRUPTION || s > D3D12_MESSAGE_SEVERITY_MESSAGE) return E_INVALIDARG;
    try {
      StoredMessage message{c, s, id, text};
      std::vector<std::shared_ptr<Callback>> notify;
      bool passes, output, breakpoint;
      {
        std::lock_guard lock(mutex); passes = storage.back().Allows(c, s, id); output = !mute;
        breakpoint = passes && (break_categories.contains(c) || break_severities.contains(s) || break_ids.contains(id));
        if (passes) { ++allowed; if (messages.size() < limit) messages.push_back(message); else ++discarded; }
        else ++denied;
        for (auto &[cookie, callback] : callbacks)
          if (passes || (callback->flags & D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS)) notify.push_back(callback);
      }
      // No message lock is held through user code. Serialize dispatch and
      // unregister; recursive locking allows callback reentry/self-unregister.
      for (const auto &callback : notify) {
        std::lock_guard lock(callback_dispatch);
        if (callback->active) callback->function(c, s, id, message.description.c_str(), callback->context);
      }
      if (output && passes) OutputDebugStringA(message.description.c_str());
      if (breakpoint) DebugBreak();
      return S_OK;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  HRESULT STDMETHODCALLTYPE AddApplicationMessage(D3D12_MESSAGE_SEVERITY s, const char *text) override {
    return AddMessage(D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED, s, D3D12_MESSAGE_ID_STRING_FROM_APPLICATION, text);
  }
#define BREAK_METHODS(Name, Type, values) \
  HRESULT STDMETHODCALLTYPE SetBreakOn##Name(Type key, BOOL enabled) override { try { std::lock_guard lock(mutex); if (enabled) values.insert(key); else values.erase(key); return S_OK; } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; } } \
  BOOL STDMETHODCALLTYPE GetBreakOn##Name(Type key) override { std::lock_guard lock(mutex); return values.contains(key); }
  BREAK_METHODS(Category, D3D12_MESSAGE_CATEGORY, break_categories)
  BREAK_METHODS(Severity, D3D12_MESSAGE_SEVERITY, break_severities)
  BREAK_METHODS(ID, D3D12_MESSAGE_ID, break_ids)
#undef BREAK_METHODS
  void STDMETHODCALLTYPE SetMuteDebugOutput(BOOL value) override { std::lock_guard lock(mutex); mute = value; }
  BOOL STDMETHODCALLTYPE GetMuteDebugOutput() override { std::lock_guard lock(mutex); return mute; }
  HRESULT STDMETHODCALLTYPE RegisterMessageCallback(D3D12MessageFunc function, D3D12_MESSAGE_CALLBACK_FLAGS flags, void *context, DWORD *cookie) override {
    if (cookie) *cookie = 0;
    if (!function || !cookie || (flags & ~D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS)) return E_INVALIDARG;
    try {
      auto callback = std::make_shared<Callback>(); callback->function = function; callback->flags = flags; callback->context = context;
      std::lock_guard lock(mutex);
      if (!next_cookie) return E_OUTOFMEMORY;
      auto next = next_cookie; callbacks.emplace(next, std::move(callback));
      ++next_cookie; *cookie = next; return S_OK;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  HRESULT STDMETHODCALLTYPE UnregisterMessageCallback(DWORD cookie) override {
    std::lock_guard dispatch_lock(callback_dispatch);
    std::shared_ptr<Callback> callback;
    { std::lock_guard lock(mutex); auto it = callbacks.find(cookie); if (it == callbacks.end()) return E_INVALIDARG;
      callback = it->second; callbacks.erase(it); }
    callback->active = false; return S_OK;
  }
  HRESULT BreadcrumbOutput(D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT *out, D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 *out1) {
    std::lock_guard lock(mutex);
    if (out) *out = {}; if (out1) *out1 = {};
    if (device->RemovedReason() == S_OK) return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    if (!capture_enabled) return DXGI_ERROR_UNSUPPORTED;
    const D3D12_AUTO_BREADCRUMB_NODE *next = nullptr;
    const D3D12_AUTO_BREADCRUMB_NODE1 *next1 = nullptr;
    for (auto &b : breadcrumbs) {
      b->node.pNext = next; b->node1.pNext = next1; next = &b->node; next1 = &b->node1;
    }
    if (out) out->pHeadAutoBreadcrumbNode = next;
    if (out1) out1->pHeadAutoBreadcrumbNode = next1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput(D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT *out) override {
    return out ? BreadcrumbOutput(out, nullptr) : E_INVALIDARG;
  }
  HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput1(D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 *out) override {
    return out ? BreadcrumbOutput(nullptr, out) : E_INVALIDARG;
  }
  HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput(D3D12_DRED_PAGE_FAULT_OUTPUT *out) override {
    if (!out) return E_INVALIDARG; *out = {}; return DXGI_ERROR_UNSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput1(D3D12_DRED_PAGE_FAULT_OUTPUT1 *out) override {
    if (!out) return E_INVALIDARG; *out = {}; return DXGI_ERROR_UNSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput2(D3D12_DRED_PAGE_FAULT_OUTPUT2 *out) override {
    if (!out) return E_INVALIDARG; *out = {}; return DXGI_ERROR_UNSUPPORTED;
  }
  D3D12_DRED_DEVICE_STATE STDMETHODCALLTYPE GetDeviceState() override {
    auto reason = device->RemovedReason();
    return reason == S_OK ? D3D12_DRED_DEVICE_STATE_UNKNOWN :
      reason == DXGI_ERROR_DEVICE_HUNG ? D3D12_DRED_DEVICE_STATE_HUNG : D3D12_DRED_DEVICE_STATE_FAULT;
  }
};
DeviceDiagnostics::DeviceDiagnostics(MTLD3D12Device *device) : impl_(std::make_unique<Impl>(device)) {}
void DeviceDiagnostics::Configure(const RuntimeConfiguration &configuration) {
  impl_->validation_enabled = configuration.debug;
  impl_->capture_enabled = configuration.breadcrumbs != D3D12_DRED_ENABLEMENT_FORCED_OFF;
  impl_->context_enabled = configuration.contexts == D3D12_DRED_ENABLEMENT_FORCED_ON;
}
DeviceDiagnostics::~DeviceDiagnostics() = default;
HRESULT DeviceDiagnostics::QueryInterface(REFIID iid, void **out) { return impl_->Query(iid, out); }
void DeviceDiagnostics::Validation(D3D12_MESSAGE_ID id, const char *description) {
  if (impl_->validation_enabled)
    impl_->AddMessage(D3D12_MESSAGE_CATEGORY_EXECUTION, D3D12_MESSAGE_SEVERITY_ERROR, id, description);
}
void DeviceDiagnostics::Removed(HRESULT reason) {
  // Freeze breadcrumb completion at the same first-removal boundary as fences.
  owned_graphics::Trace("device_removed", ",\"reason\":", uint32_t(reason));
  impl_->AddMessage(D3D12_MESSAGE_CATEGORY_EXECUTION, D3D12_MESSAGE_SEVERITY_ERROR,
                   D3D12_MESSAGE_ID_DEVICE_REMOVAL_PROCESS_AT_FAULT, "Owned D3D12 device removed; DRED contains retired/submitted native work observations");
}
std::shared_ptr<DiagnosticBreadcrumb> DeviceDiagnostics::BeginSubmission(ID3D12CommandList *list, ID3D12CommandQueue *queue,
    std::vector<D3D12_AUTO_BREADCRUMB_OP> history, const std::vector<std::pair<UINT, std::string>> &contexts) {
  if (!impl_->capture_enabled) return {};
  std::lock_guard lock(impl_->mutex);
  if (impl_->device->RemovedReason() != S_OK) return {};
  auto b = std::make_shared<DiagnosticBreadcrumb>(); b->history = std::move(history);
  auto name = [](ID3D12Object *object, std::wstring &target) {
    if (!object) return;
    UINT size = 0;
    const GUID guid = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
    if (SUCCEEDED(object->GetPrivateData(guid, &size, nullptr)) && size >= sizeof(WCHAR) && !(size % sizeof(WCHAR))) {
      target.resize(size / sizeof(WCHAR));
      if (FAILED(object->GetPrivateData(guid, &size, target.data()))) target.clear();
      else target.back() = 0;
    }
  };
  name(list, b->list_name); name(queue, b->queue_name);
  b->node.pCommandListDebugNameW = b->list_name.empty() ? nullptr : b->list_name.c_str();
  b->node.pCommandQueueDebugNameW = b->queue_name.empty() ? nullptr : b->queue_name.c_str();
  // Do not keep COM references: that would form device -> DRED -> queue -> device.
  // Stable copied names identify released application objects; object pointers remain null.
  b->node.BreadcrumbCount = b->history.size(); b->node.pLastBreadcrumbValue = &b->completed;
  b->node.pCommandHistory = b->history.data();
  b->node1.pCommandListDebugNameW = b->node.pCommandListDebugNameW;
  b->node1.pCommandQueueDebugNameW = b->node.pCommandQueueDebugNameW;
  b->node1.BreadcrumbCount = b->node.BreadcrumbCount; b->node1.pLastBreadcrumbValue = &b->completed;
  b->node1.pCommandHistory = b->history.data();
  if (impl_->context_enabled) {
    b->context_strings.reserve(contexts.size()); b->contexts.reserve(contexts.size());
    for (const auto &[index, label] : contexts) {
      const int count = MultiByteToWideChar(CP_UTF8, 0, label.data(), int(label.size()), nullptr, 0);
      if (count <= 0) continue;
      auto &text = b->context_strings.emplace_back(size_t(count), WCHAR(0));
      MultiByteToWideChar(CP_UTF8, 0, label.data(), int(label.size()), text.data(), count);
      b->contexts.push_back({index, text.c_str()});
    }
    b->node1.BreadcrumbContextsCount = b->contexts.size();
    b->node1.pBreadcrumbContexts = b->contexts.data();
  }
  // ponytail: retain 128 retired submissions plus all outstanding work;
  // raise the retired-history bound if fault diagnosis needs a longer window.
  if (impl_->breadcrumbs.size() >= 128) {
    auto retired = std::find_if(impl_->breadcrumbs.begin(), impl_->breadcrumbs.end(),
        [](const auto &entry) { return entry->completed == entry->history.size(); });
    if (retired != impl_->breadcrumbs.end()) impl_->breadcrumbs.erase(retired);
  }
  impl_->breadcrumbs.push_back(b); return b;
}
void DeviceDiagnostics::Complete(const std::shared_ptr<DiagnosticBreadcrumb> &b) {
  if (!b) return;
  std::lock_guard lock(impl_->mutex);
  if (impl_->device->RemovedReason() == S_OK) b->completed = b->history.size();
}
HRESULT GetDebugInterface(REFIID iid, void **out, std::shared_ptr<void> core_lifetime) {
  if (!out) return E_POINTER; *out = nullptr;
  try {
    if (iid == __uuidof(ID3D12Debug) || iid == __uuidof(IUnknown)) {
      auto object = std::make_unique<DebugLayer>(nullptr, core_lifetime); auto hr = object->QueryInterface(iid, out);
      if (SUCCEEDED(hr)) object.release(); return hr;
    }
    if (iid == __uuidof(ID3D12DeviceRemovedExtendedDataSettings) || iid == __uuidof(ID3D12DeviceRemovedExtendedDataSettings1)) {
      auto object = std::make_unique<DredSettings>(nullptr, core_lifetime); auto hr = object->QueryInterface(iid, out);
      if (SUCCEEDED(hr)) object.release(); return hr;
    }
    return E_NOINTERFACE;
  } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}
HRESULT GetDiagnosticsInterface(REFCLSID clsid, REFIID iid, void **out, std::shared_ptr<void> core_lifetime) {
  if (!out) return E_POINTER; *out = nullptr;
  if (clsid == CLSID_D3D12DeviceFactory) {
    return GetSDKInterface(clsid, iid, out);
  }
  if (clsid == CLSID_D3D12Debug) {
    if (iid != __uuidof(ID3D12Debug) && iid != __uuidof(IUnknown)) return E_NOINTERFACE;
    return GetDebugInterface(iid, out, core_lifetime);
  }
  if (clsid == CLSID_D3D12DeviceRemovedExtendedData) {
    try {
      auto object = std::make_unique<DredSettings>(nullptr, core_lifetime); auto hr = object->QueryInterface(iid, out);
      if (SUCCEEDED(hr)) object.release(); return hr;
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
  }
  if (clsid == CLSID_D3D12SDKConfiguration) {
    return GetSDKInterface(clsid, iid, out);
  }
  // ID3D12Tools requires shader instrumentation. No shader instrumentation
  // implementation exists in this runtime; returning an inert Tools vtable
  // would falsely advertise support.
  return E_NOINTERFACE;
}
DiagnosticAnnotation CaptureAnnotation(UINT action, UINT metadata, const void *data, UINT size) {
  if (size && !data) throw std::invalid_argument("annotation");
  DiagnosticAnnotation annotation; annotation.action = action; annotation.metadata = metadata;
  if (size) annotation.payload.assign(static_cast<const unsigned char *>(data), static_cast<const unsigned char *>(data) + size);
  if (metadata == 1 && size) {
    auto *bytes = reinterpret_cast<const char *>(annotation.payload.data());
    const auto length = strnlen(bytes, size);
    if (length <= size_t(std::numeric_limits<int>::max())) {
      const int wide_count = MultiByteToWideChar(CP_ACP, 0, bytes, int(length), nullptr, 0);
      if (wide_count > 0) {
        std::vector<WCHAR> wide(wide_count);
        MultiByteToWideChar(CP_ACP, 0, bytes, int(length), wide.data(), wide_count);
        const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_count, nullptr, 0, nullptr, nullptr);
        if (count > 0) { annotation.label.resize(count); WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_count, annotation.label.data(), count, nullptr, nullptr); }
      }
    }
  } else if (metadata == 0 && size >= sizeof(WCHAR) && !(size % sizeof(WCHAR))) {
    // Copy to an aligned UTF-16 buffer before conversion (callers may pass an unaligned blob).
    std::vector<WCHAR> text(size / sizeof(WCHAR)); memcpy(text.data(), annotation.payload.data(), size);
    auto end = std::find(text.begin(), text.end(), WCHAR(0));
    int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(end - text.begin()), nullptr, 0, nullptr, nullptr);
    if (count > 0) { annotation.label.resize(count); WideCharToMultiByte(CP_UTF8, 0, text.data(), int(end - text.begin()), annotation.label.data(), count, nullptr, nullptr); }
  }
  if (annotation.label.empty()) annotation.label = "D3D12 annotation metadata=" + std::to_string(metadata) + " bytes=" + std::to_string(size);
  return annotation;
}
} // namespace dxmt
