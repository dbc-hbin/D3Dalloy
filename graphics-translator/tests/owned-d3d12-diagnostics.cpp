// Public Debug/InfoQueue1/DRED2 consumer, including FIFO native annotation retirement.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
constexpr char prefix[] = "OWNED_D3D12_DIAGNOSTICS";
unsigned cases = 0;
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx\n", prefix, step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool condition, const char *step) { if (!condition) fail(step); }
void pass(const char *name) { ++cases; std::printf("%s_CASE_PASS case=%s\n", prefix, name); }
std::vector<unsigned char> message(ID3D12InfoQueue *queue, UINT64 index) {
  SIZE_T length = 0; check(queue->GetMessage(index, nullptr, &length), "message-size");
  std::vector<unsigned char> bytes(length); check(queue->GetMessage(index, reinterpret_cast<D3D12_MESSAGE *>(bytes.data()), &length), "message-get");
  return bytes;
}
void wait(ID3D12Fence *fence, UINT64 value) {
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "event-create");
  check(fence->SetEventOnCompletion(value, event), "fence-event");
  auto result = WaitForSingleObject(event, 15000); CloseHandle(event); require(result == WAIT_OBJECT_0, "fence-wait");
}
struct CallbackContext {
  DWORD cookie = 0;
  unsigned calls = 0;
};
void CALLBACK callback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY, D3D12_MESSAGE_ID, const char *text, void *opaque) {
  auto &context = *static_cast<CallbackContext *>(opaque); ++context.calls;
  require(text && *text, "callback-text");
  // Official callback contract forbids calling D3D APIs from the callback.
}
using GetInterface = HRESULT (WINAPI *)(REFCLSID, REFIID, void **);
void agility_installed_factory(GetInterface get_interface) {
  // Public DirectX-Headers CLSID; the selected MinGW header omits this CLSID.
  constexpr GUID installed_factory = {0x114863bf, 0xc386, 0x4aee, {0xb3,0x9d,0x8f,0x0b,0xbb,0x06,0x29,0x55}};
  ComPtr<ID3D12SDKConfiguration> sdk;
  check(get_interface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdk)), "agility-sdk-base");
  void *out = reinterpret_cast<void *>(1);
  ComPtr<ID3D12SDKConfiguration1> sdk1, queried_sdk1;
  check(get_interface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdk1)), "agility-sdk1-loader");
  check(sdk.As(&queried_sdk1), "agility-sdk1-query");
  HRESULT sdk_hr = sdk1->CreateDeviceFactory(619, ".\\missing-sdk", __uuidof(ID3D12DeviceFactory), &out);
  require(FAILED(sdk_hr) && !out, "agility-foreign-path-rejected");
  std::printf("%s_SDK_REJECT case=wrong_version hr=0x%08lx output=null\n", prefix, static_cast<unsigned long>(sdk_hr));
  out = reinterpret_cast<void *>(1);
  sdk_hr = sdk1->CreateDeviceFactory(602, ".\\", __uuidof(ID3D12DeviceFactory), &out);
  require(FAILED(sdk_hr) && !out, "agility-header-not-runtime-version");
  std::printf("%s_SDK_REJECT case=header_version hr=0x%08lx output=null\n", prefix, static_cast<unsigned long>(sdk_hr));
  sdk_hr = sdk->SetSDKVersion(0, ".\\");
  require(FAILED(sdk_hr), "agility-no-invented-inbox-rule");
  std::printf("%s_SDK_REJECT case=base_zero_version hr=0x%08lx\n", prefix, static_cast<unsigned long>(sdk_hr));
  ComPtr<ID3D12DeviceFactory> a, b;
  check(get_interface(installed_factory, IID_PPV_ARGS(&a)), "agility-installed-factory-a");
  check(get_interface(installed_factory, IID_PPV_ARGS(&b)), "agility-installed-factory-b");
  require(get_interface(installed_factory, __uuidof(ID3D12DeviceFactory), nullptr) == E_POINTER, "agility-null-factory-output");
  require(a->GetConfigurationInterface(CLSID_D3D12Debug, __uuidof(ID3D12Debug), nullptr) == E_POINTER, "agility-null-config-output");
  require(get_interface(__uuidof(ID3D12DeviceFactory), __uuidof(ID3D12DeviceFactory), &out) == E_NOINTERFACE && !out, "agility-unknown-clsid");
  require(a->GetFlags() == D3D12_DEVICE_FACTORY_FLAG_NONE, "agility-default-flags");
  require(a->SetFlags(D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE) == E_INVALIDARG, "agility-incompatible-flags");
  require(a->SetFlags(static_cast<D3D12_DEVICE_FACTORY_FLAGS>(0x80)) == E_INVALIDARG && a->GetFlags() == D3D12_DEVICE_FACTORY_FLAG_NONE, "agility-unknown-flags");
  check(a->SetFlags(static_cast<D3D12_DEVICE_FACTORY_FLAGS>(7)), "agility-valid-flags");
  require(a->GetFlags() == static_cast<D3D12_DEVICE_FACTORY_FLAGS>(7), "agility-flags-roundtrip");
  require(a->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr) == S_FALSE, "agility-null-device-probe");
  require(a->CreateDevice(nullptr, D3D_FEATURE_LEVEL_10_0, __uuidof(ID3D12Device), &out) == E_INVALIDARG && !out, "agility-low-fl");
  require(a->CreateDevice(sdk.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), &out) == E_INVALIDARG && !out, "agility-invalid-adapter");
  require(a->GetConfigurationInterface(CLSID_D3D12Tools, __uuidof(ID3D12Tools), &out) == E_NOINTERFACE && !out, "agility-no-fake-tools");
  require(a->GetConfigurationInterface(installed_factory, __uuidof(ID3D12DeviceFactory), &out) == E_NOINTERFACE && !out, "agility-unknown-config-clsid");
  require(get_interface(installed_factory, __uuidof(ID3D12Device), &out) == E_NOINTERFACE && !out, "agility-unknown-installed-iid");
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
  check(a->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(&settings)), "agility-local-dred");
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);
  ComPtr<ID3D12Device5> before, after, other, global;
  check(a->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&before)), "agility-before-config");
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_11_0};
  D3D12_FEATURE_DATA_FEATURE_LEVELS support{2, levels, D3D_FEATURE_LEVEL_11_0};
  check(before->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &support, sizeof(support)), "agility-feature-levels");
  ComPtr<ID3D12Device> high;
  HRESULT high_hr = a->CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&high));
  require(support.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_12_2 ? high_hr == S_OK && high
      : high_hr == DXGI_ERROR_UNSUPPORTED && !high, "agility-high-fl");
  ComPtr<ID3D12Debug> debug; check(a->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&debug)), "agility-local-debug");
  debug->EnableDebugLayer();
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  const IID experimental = D3D12ExperimentalShaderModels;
  require(a->EnableExperimentalFeatures(1, nullptr, nullptr, nullptr) == E_INVALIDARG, "agility-experimental-validation");
  check(a->EnableExperimentalFeatures(1, &experimental, nullptr, nullptr), "agility-local-experimental");
  const IID invalid_experimental = __uuidof(ID3D12Device);
  require(a->EnableExperimentalFeatures(1, &invalid_experimental, nullptr, nullptr) == E_NOINTERFACE, "agility-unknown-experimental");
  check(a->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&after)), "agility-after-config");
  check(b->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&other)), "agility-other-factory");
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&global)), "agility-global-device");
  require(before.Get() != after.Get() && after.Get() != other.Get(), "agility-independent-device-identities");
  ID3D12Device5 *devices[] = {before.Get(), after.Get(), other.Get(), global.Get()};
  for (unsigned i = 0; i < 4; ++i) {
    ComPtr<ID3D12InfoQueue> info; check(devices[i]->QueryInterface(IID_PPV_ARGS(&info)), "agility-info");
    ComPtr<ID3D12CommandQueue> queue;
    require(devices[i]->CreateCommandQueue(nullptr, IID_PPV_ARGS(&queue)) == E_INVALIDARG, "agility-validation-input");
    require(info->GetNumStoredMessages() == (i == 1 ? 1u : 0u), "agility-debug-snapshot-isolation");
  }
  // Keep the local configuration interfaces alive after releasing the factory.
  check(a->ApplyToGlobalState(), "agility-apply-global");
  require(global->GetDeviceRemovedReason() == DXGI_ERROR_DEVICE_REMOVED &&
      before->GetDeviceRemovedReason() == S_OK && after->GetDeviceRemovedReason() == S_OK && other->GetDeviceRemovedReason() == S_OK, "agility-global-removal-isolation");
  check(b->InitializeFromGlobalState(), "agility-initialize-global");
  ComPtr<ID3D12Device5> inherited; check(b->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&inherited)), "agility-inherited-device");
  ComPtr<ID3D12InfoQueue> inherited_info; check(inherited.As(&inherited_info), "agility-inherited-info");
  ComPtr<ID3D12CommandQueue> invalid_queue;
  require(inherited->CreateCommandQueue(nullptr, IID_PPV_ARGS(&invalid_queue)) == E_INVALIDARG && inherited_info->GetNumStoredMessages() == 1, "agility-inherited-debug");
  a.Reset(); b.Reset(); debug->EnableDebugLayer();
  check(D3D12EnableExperimentalFeatures(0, nullptr, nullptr, nullptr), "agility-global-experimental-reset");
  require(before->GetDeviceRemovedReason() == S_OK && after->GetDeviceRemovedReason() == S_OK &&
      other->GetDeviceRemovedReason() == S_OK && inherited->GetDeviceRemovedReason() == S_OK, "agility-experimental-removal-isolation");
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);
  for (unsigned i = 0; i < 2; ++i) {
    auto *device = devices[i];
    D3D12_COMMAND_QUEUE_DESC qdesc{}; qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "agility-gpu-queue");
    ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(qdesc.Type, IID_PPV_ARGS(&allocator)), "agility-gpu-allocator");
    ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0, qdesc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "agility-gpu-list");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 16;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> upload, readback;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "agility-gpu-upload");
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "agility-gpu-readback");
    const uint32_t expected = 0xa6111700 + i; void *data = nullptr; D3D12_RANGE empty{0,0};
    check(upload->Map(0, &empty, &data), "agility-gpu-upload-map"); memcpy(data, &expected, sizeof(expected)); upload->Unmap(0, nullptr);
    list->SetMarker(1, "agility-snapshot", 16); list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, sizeof(expected));
    check(list->Close(), "agility-gpu-close"); ID3D12CommandList *lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "agility-gpu-fence");
    check(queue->Signal(fence.Get(), 1), "agility-gpu-signal"); wait(fence.Get(), 1);
    check(readback->Map(0, nullptr, &data), "agility-gpu-map"); uint32_t actual; memcpy(&actual, data, sizeof(actual)); readback->Unmap(0, &empty);
    require(actual == expected, "agility-real-gpu-readback");
    device->RemoveDevice(); ComPtr<ID3D12DeviceRemovedExtendedData2> dred; check(device->QueryInterface(IID_PPV_ARGS(&dred)), "agility-dred");
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    require(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs) == (i == 1 ? S_OK : DXGI_ERROR_UNSUPPORTED), "agility-dred-output");
    require((breadcrumbs.pHeadAutoBreadcrumbNode != nullptr) == (i == 1), "agility-dred-snapshot-isolation");
    if (i == 1) require(breadcrumbs.pHeadAutoBreadcrumbNode->BreadcrumbContextsCount == 1, "agility-dred-context-snapshot");
  }
  pass("agility_installed_factory_gpu_isolation");
}
}
int main() {
  auto dll = GetModuleHandleW(L"d3d12.dll"); require(dll != nullptr, "module");
  using Identity = const char *(WINAPI *)();
  auto identity = reinterpret_cast<Identity>(GetProcAddress(dll, "OwnedGraphicsBackendId"));
  require(identity && !std::strcmp(identity(), "owned-graphics-d3d12"), "backend-identity");
  std::printf("%s_BACKEND id=owned-graphics-d3d12\n", prefix);
  auto get_interface = reinterpret_cast<GetInterface>(GetProcAddress(dll, "D3D12GetInterface"));
  require(get_interface != nullptr, "agility-getinterface-export"); agility_installed_factory(get_interface);
  ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug-interface"); debug->EnableDebugLayer();
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
  check(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)), "dred-settings");
  settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  ComPtr<ID3D12Device5> device; check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  ComPtr<ID3D12InfoQueue1> info; check(device.As(&info), "infoqueue1"); info->SetMuteDebugOutput(TRUE);
  ComPtr<IUnknown> device_identity, info_identity; check(device.As(&device_identity), "device-unknown"); check(info.As(&info_identity), "info-unknown");
  require(device_identity.Get() == info_identity.Get(), "aggregated-com-identity");
  ComPtr<ID3D12CommandQueue> queue;
  require(device->CreateCommandQueue(nullptr, IID_PPV_ARGS(&queue)) == E_INVALIDARG, "invalid-create-queue");
  require(info->GetNumStoredMessages() == 1, "validation-storage");
  auto bytes = message(info.Get(), 0); auto *validation = reinterpret_cast<const D3D12_MESSAGE *>(bytes.data());
  require(validation->Severity == D3D12_MESSAGE_SEVERITY_ERROR && std::strstr(validation->pDescription, "CreateCommandQueue"), "validation-description");
  pass("validation");

  info->ClearStoredMessages(); check(info->SetMessageCountLimit(2), "message-limit");
  check(info->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "first"), "message-first");
  check(info->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, "second"), "message-second");
  check(info->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "third"), "message-third");
  require(info->GetNumStoredMessages() == 2 && info->GetNumMessagesDiscardedByMessageCountLimit() == 1 &&
          info->GetNumMessagesAllowedByStorageFilter() == 3, "message-counts");
  bytes = message(info.Get(), 0); auto *first = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
  require(!std::strcmp(first->pDescription, "first") && first->DescriptionByteLength == 6, "message-copy");
  D3D12_MESSAGE small{}; SIZE_T length = sizeof(small);
  require(info->GetMessage(0, &small, &length) == DXGI_ERROR_MORE_DATA && length > sizeof(small), "small-buffer");
  pass("storage_retrieval");

  info->ClearStoredMessages(); check(info->SetMessageCountLimit(1024), "reset-limit");
  D3D12_MESSAGE_SEVERITY deny = D3D12_MESSAGE_SEVERITY_INFO;
  D3D12_INFO_QUEUE_FILTER filter{}; filter.DenyList.NumSeverities = 1; filter.DenyList.pSeverityList = &deny;
  check(info->PushStorageFilter(&filter), "push-storage-filter");
  CallbackContext filtered, unfiltered;
  DWORD invalid_cookie = 99;
  require(info->RegisterMessageCallback(nullptr, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &invalid_cookie) == E_INVALIDARG &&
      invalid_cookie == 0, "callback-failure-cookie");
  check(info->RegisterMessageCallback(callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, &filtered, &filtered.cookie), "register-filtered");
  check(info->RegisterMessageCallback(callback, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, &unfiltered, &unfiltered.cookie), "register-unfiltered");
  check(info->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "denied"), "denied-message");
  require(filtered.calls == 0 && unfiltered.calls == 1 && info->GetNumMessagesDeniedByStorageFilter() == 1, "filter-callbacks");
  check(info->UnregisterMessageCallback(unfiltered.cookie), "unregister-unfiltered");
  info->PopStorageFilter(); check(info->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, "accepted"), "accepted-message");
  require(filtered.calls == 1 && unfiltered.calls == 1, "callback-unregistered");
  check(info->UnregisterMessageCallback(filtered.cookie), "unregister-filtered");
  require(info->UnregisterMessageCallback(filtered.cookie) == E_INVALIDARG, "unregister-unknown");
  check(info->PushRetrievalFilter(&filter), "push-retrieval-filter");
  check(info->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "hidden"), "hidden-message");
  require(info->GetNumStoredMessages() == 2 && info->GetNumStoredMessagesAllowedByRetrievalFilter() == 1, "retrieval-counts");
  check(info->PushCopyOfRetrievalFilter(), "copy-filter");
  length = 0; check(info->GetRetrievalFilter(nullptr, &length), "filter-size");
  std::vector<unsigned char> packed(length); check(info->GetRetrievalFilter(reinterpret_cast<D3D12_INFO_QUEUE_FILTER *>(packed.data()), &length), "filter-get");
  auto *copied = reinterpret_cast<D3D12_INFO_QUEUE_FILTER *>(packed.data());
  require(copied->DenyList.NumSeverities == 1 && copied->DenyList.pSeverityList[0] == deny && info->GetRetrievalFilterStackSize() == 3, "filter-copy");
  info->PopRetrievalFilter(); info->PopRetrievalFilter(); info->PopRetrievalFilter();
  require(info->GetRetrievalFilterStackSize() == 1, "filter-base-retained");
  check(info->SetBreakOnID(D3D12_MESSAGE_ID_UNKNOWN, TRUE), "break-enable"); require(info->GetBreakOnID(D3D12_MESSAGE_ID_UNKNOWN), "break-get");
  check(info->SetBreakOnID(D3D12_MESSAGE_ID_UNKNOWN, FALSE), "break-disable");
  pass("filters_callbacks");

  D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue-create"); check(queue->SetName(L"diagnostic-queue"), "queue-name");
  ComPtr<ID3D12CommandAllocator> allocator; check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "allocator");
  ComPtr<ID3D12GraphicsCommandList> list; check(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
  check(list->SetName(L"diagnostic-list"), "list-name");
  char label[] = "private-marker-payload";
  queue->BeginEvent(1, label, sizeof(label)); queue->SetMarker(1, label, sizeof(label));
  list->BeginEvent(1, label, sizeof(label)); list->SetMarker(1, label, sizeof(label)); list->EndEvent();
  D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 16;
  buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap{}; heap.CreationNodeMask = heap.VisibleNodeMask = 1; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  ComPtr<ID3D12Resource> upload, readback;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload");
  heap.Type = D3D12_HEAP_TYPE_READBACK;
  check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
  void *mapped = nullptr; D3D12_RANGE empty{0, 0}; check(upload->Map(0, &empty, &mapped), "upload-map");
  const std::uint32_t expected = 0xc001d1a6; std::memcpy(mapped, &expected, sizeof(expected)); upload->Unmap(0, nullptr);
  list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, sizeof(expected));
  // The caller storage disappears before FIFO replay. Native annotation payload must be copied.
  std::memset(label, 'X', sizeof(label));
  check(list->Close(), "list-close"); ID3D12CommandList *lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists); queue->EndEvent();
  ComPtr<ID3D12Fence> completed; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed)), "completion-fence");
  check(queue->Signal(completed.Get(), 1), "queue-signal"); wait(completed.Get(), 1);
  require(device->GetDeviceRemovedReason() == S_OK, "annotation-healthy");
  check(readback->Map(0, nullptr, &mapped), "readback-map");
  std::uint32_t actual = 0; std::memcpy(&actual, mapped, sizeof(actual)); readback->Unmap(0, &empty);
  require(actual == expected, "annotated-gpu-copy");
  list.Reset(); allocator.Reset();
  ComPtr<ID3D12DeviceRemovedExtendedData2> dred; check(device.As(&dred), "dred2");
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output{};
  require(dred->GetAutoBreadcrumbsOutput1(&output) == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "dred-before-removal");
  ComPtr<ID3D12Fence> blocked; check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocked)), "blocked-fence");
  check(queue->Wait(blocked.Get(), 1), "blocked-wait"); queue->SetMarker(1, "pending", 8);
  device->RemoveDevice();
  require(device->GetDeviceRemovedReason() == DXGI_ERROR_DEVICE_REMOVED && dred->GetDeviceState() == D3D12_DRED_DEVICE_STATE_FAULT, "removed-state");
  check(dred->GetAutoBreadcrumbsOutput1(&output), "dred-after-removal");
  unsigned retired = 0, pending = 0; bool named_list = false;
  for (auto *node = output.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
    require(node->pLastBreadcrumbValue && node->pCommandHistory && node->BreadcrumbCount >= 2, "breadcrumb-data");
    require(node->pCommandHistory[0] == D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION &&
      node->pCommandHistory[node->BreadcrumbCount - 1] == D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION, "breadcrumb-history");
    require(node->pCommandQueueDebugNameW && !std::wcscmp(node->pCommandQueueDebugNameW, L"diagnostic-queue"), "breadcrumb-queue-name");
    if (*node->pLastBreadcrumbValue == node->BreadcrumbCount) ++retired;
    else { require(*node->pLastBreadcrumbValue == 0, "pending-watermark"); ++pending; }
    if (node->pCommandListDebugNameW && !std::wcscmp(node->pCommandListDebugNameW, L"diagnostic-list")) {
      named_list = true; require(node->BreadcrumbCount == 6 &&
        node->pCommandHistory[4] == D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION, "list-gpu-command-history");
      require(node->BreadcrumbContextsCount == 2 && node->pBreadcrumbContexts &&
        !std::wcscmp(node->pBreadcrumbContexts[0].pContextString, L"private-marker-payload") &&
        node->pBreadcrumbContexts[0].BreadcrumbIndex == 1, "retained-native-annotation-context");
    }
  }
  require(retired >= 4 && pending == 1 && named_list, "retired-pending-breadcrumbs");
  D3D12_DRED_PAGE_FAULT_OUTPUT2 faults{};
  require(dred->GetPageFaultAllocationOutput2(&faults) == DXGI_ERROR_UNSUPPORTED && !faults.PageFaultVA, "no-fabricated-fault-address");
  require(blocked->GetCompletedValue() == UINT64_MAX, "removed-fence");
  pass("native_markers_dred_removal");

  using GetInterface = HRESULT (WINAPI *)(REFCLSID, REFIID, void **);
  auto factory = reinterpret_cast<GetInterface>(GetProcAddress(dll, "D3D12GetInterface")); require(factory != nullptr, "getinterface-export");
  ComPtr<ID3D12SDKConfiguration> sdk;
  check(factory(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdk)), "sdk-configuration");
  HRESULT sdk_hr = sdk->SetSDKVersion(0, ".");
  require(FAILED(sdk_hr), "honest-sdk-loader");
  std::printf("%s_SDK_REJECT case=late_base_zero_version hr=0x%08lx\n", prefix, static_cast<unsigned long>(sdk_hr));
  ComPtr<ID3D12SDKConfiguration1> sdk1; check(sdk.As(&sdk1), "sdk1-configuration");
  void *unknown_sdk = reinterpret_cast<void *>(1);
  sdk_hr = sdk1->CreateDeviceFactory(619, ".", __uuidof(ID3D12DeviceFactory), &unknown_sdk);
  require(FAILED(sdk_hr) && !unknown_sdk, "unsupported-sdk1-version");
  std::printf("%s_SDK_REJECT case=late_wrong_version hr=0x%08lx output=null\n", prefix, static_cast<unsigned long>(sdk_hr));
  void *unsupported = reinterpret_cast<void *>(1);
  require(factory(CLSID_D3D12Tools, __uuidof(ID3D12Tools), &unsupported) == E_NOINTERFACE && !unsupported, "no-dummy-tools");
  require(D3D12GetDebugInterface(__uuidof(ID3D12Debug1), &unsupported) == E_NOINTERFACE && !unsupported, "no-dummy-gpu-validation");
  ComPtr<ID3D12Device> late_device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&late_device)), "late-device");
  debug->EnableDebugLayer();
  require(late_device->GetDeviceRemovedReason() == DXGI_ERROR_DEVICE_REMOVED, "late-debug-enable-removes-device");
  pass("sdk_tools_contracts");
  std::printf("%s_PASS cases=%u\n", prefix, cases); return 0;
}
