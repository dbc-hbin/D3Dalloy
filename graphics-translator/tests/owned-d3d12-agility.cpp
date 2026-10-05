// Public Agility 618 loader consumer. Inputs are copied from an immutable staged runtime by the runner.
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
#include <string>
#include <vector>
#include <barrier>
#include <thread>
using Microsoft::WRL::ComPtr;
#ifdef OWNED_AGILITY_EXECUTABLE_SELECTION
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = OWNED_AGILITY_EXECUTABLE_SELECTION;
#if OWNED_AGILITY_EXECUTABLE_SELECTION == 617
__declspec(dllexport) extern const char *const D3D12SDKPath = ".\\sdk-missing\\";
#elif OWNED_AGILITY_EXECUTABLE_SELECTION == 618
__declspec(dllexport) extern const char *const D3D12SDKPath = ":invalid-sdk-path:";
#elif OWNED_AGILITY_EXECUTABLE_SELECTION == 619
__declspec(dllexport) extern const char *const D3D12SDKPath = ".\\sdk-foreign\\";
#else
#error Build immutable EXE selection variants for SDK617, SDK618 or SDK619
#endif
}
#endif
namespace {
constexpr char prefix[] = "OWNED_D3D12_AGILITY";
// Independent Devices ApplyToGlobalState specifies INVALID_REDIST. This MinGW
// header omits the constant; Microsoft's Agility SDK guide identifies 887e0003:
// https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
constexpr HRESULT invalid_redist = static_cast<HRESULT>(0x887e0003u);
constexpr GUID installed_factory = {0x114863bf, 0xc386, 0x4aee, {0xb3,0x9d,0x8f,0x0b,0xbb,0x06,0x29,0x55}};
using GetInterface = HRESULT (WINAPI *)(REFCLSID, REFIID, void **);
unsigned cases = 0;
std::wstring exe_directory;
[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx\n", prefix, step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void check(HRESULT hr, const char *step) { if (FAILED(hr)) fail(step, hr); }
void require(bool condition, const char *step) { if (!condition) fail(step); }
void pass(const char *name) { ++cases; std::printf("%s_CASE_PASS case=%s\n", prefix, name); }
std::wstring core_path(const wchar_t *directory = L"sdk-owned") {
  return exe_directory + L"\\" + directory + L"\\d3d12core.dll";
}
HMODULE selected_core(const std::wstring &path = core_path()) {
  auto module = GetModuleHandleW(path.c_str());
  require(module != nullptr, "selected-core-loaded");
  wchar_t actual[MAX_PATH];
  require(GetModuleFileNameW(module, actual, MAX_PATH) != 0 && !_wcsicmp(actual, path.c_str()), "selected-core-actual-path");
  auto version = reinterpret_cast<const UINT *>(GetProcAddress(module, "D3D12SDKVersion"));
  require(version && *version == 618, "selected-core-actual-data-version");
  auto get_interface = GetProcAddress(module, "D3D12GetInterface");
  HMODULE export_module = nullptr;
  require(get_interface && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(get_interface), &export_module) && export_module == module, "selected-core-concrete-public-export");
  using Identity = const char *(WINAPI *)();
  auto identity = reinterpret_cast<Identity>(GetProcAddress(module, "OwnedGraphicsBackendId"));
  const auto identity_error = identity ? ERROR_SUCCESS : GetLastError();
  const char *backend_id = identity ? identity() : nullptr;
  if (!backend_id || std::strcmp(backend_id, "owned-graphics-d3d12core")) {
    std::fprintf(stderr, "%s_CORE_IDENTITY module=%p path=%ls proc=%s error=%lu actual=%s\n", prefix,
        static_cast<void *>(module), path.c_str(), identity ? "present" : "missing",
        static_cast<unsigned long>(identity_error), backend_id ? backend_id : "<null>");
    fail("selected-core-owned-identity");
  }
  return module;
}
std::wstring inbox_core(const char *operation, UINT requested) {
  auto module = GetModuleHandleW(L"d3d12core.dll");
  wchar_t path[MAX_PATH];
  require(module && GetModuleFileNameW(module, path, MAX_PATH), "inbox-core-module-path");
  require(selected_core(path) == module && GetModuleHandleW(core_path().c_str()) == nullptr &&
      GetModuleHandleW(core_path(L"sdk-alternate").c_str()) == nullptr &&
      GetModuleHandleW(core_path(L"sdk-foreign").c_str()) == nullptr, "inbox-not-app-local-core");
  auto generation = reinterpret_cast<const char *>(GetProcAddress(module, "OwnedD3D12CoreGeneration"));
  auto exe_sdk = reinterpret_cast<const UINT *>(GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion"));
  require(generation && std::strlen(generation) == 32 && exe_sdk, "inbox-generation-and-immutable-exe-version");
  std::printf("%s_INBOX_SELECTION operation=%s requested=%u exe_sdk=%u sdk=618 generation=%s path=%ls app_loaded=0 cwd_base=exe\n",
      prefix, operation, requested, *exe_sdk, generation, path);
  return path;
}
void device_configuration(ID3D12Device *device, bool debug_enabled, UINT experimental_count = 0) {
  ComPtr<ID3D12DeviceConfiguration> configuration;
  check(device->QueryInterface(IID_PPV_ARGS(&configuration)), "device-public-configuration");
  auto desc = configuration->GetDesc();
  require(desc.SDKVersion == 618, "device-configuration-sdk618");
  require(((desc.Flags & D3D12_DEVICE_FLAG_DEBUG_LAYER_ENABLED) != 0) == debug_enabled, "device-debug-snapshot");
  require(desc.NumEnabledExperimentalFeatures == experimental_count, "device-experimental-snapshot");
  GUID experimental{};
  check(configuration->GetEnabledExperimentalFeatures(experimental_count ? &experimental : nullptr, experimental_count), "configuration-experimental-features");
  if (experimental_count) require(experimental == D3D12ExperimentalShaderModels, "configuration-experimental-guid");
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root{}; root.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
  ComPtr<ID3DBlob> blob, errors;
  check(configuration->SerializeVersionedRootSignature(&root, &blob, &errors), "configuration-serialize-public-root");
  require(blob && blob->GetBufferSize() != 0, "configuration-root-blob");
  ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
  check(configuration->CreateVersionedRootSignatureDeserializer(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&deserializer)), "configuration-public-deserializer");
  require(deserializer->GetUnconvertedRootSignatureDesc()->Version == root.Version, "configuration-root-roundtrip");
}
struct Copy {
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Resource> upload, readback;
  ComPtr<ID3D12Fence> fence;
  explicit Copy(ID3D12Device *device) {
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "copy-queue");
    check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "copy-allocator");
    check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "copy-list");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 16;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "copy-upload");
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "copy-readback");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "copy-fence");
  }
  void run(const char *phase, uint32_t expected, UINT64 completion, bool reset = false) {
    if (reset) {
      check(allocator->Reset(), "copy-allocator-reset"); check(list->Reset(allocator.Get(), nullptr), "copy-list-reset");
    }
    void *data = nullptr; D3D12_RANGE empty{0, 0};
    check(upload->Map(0, &empty, &data), "upload-map"); std::memcpy(data, &expected, sizeof(expected)); upload->Unmap(0, nullptr);
    list->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, sizeof(expected)); check(list->Close(), "copy-close");
    ID3D12CommandList *lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists); check(queue->Signal(fence.Get(), completion), "copy-signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "copy-event");
    check(fence->SetEventOnCompletion(completion, event), "copy-completion");
    DWORD result = WaitForSingleObject(event, 15000); CloseHandle(event); require(result == WAIT_OBJECT_0, "copy-wait");
    uint32_t actual = 0; check(readback->Map(0, nullptr, &data), "readback-map"); std::memcpy(&actual, data, sizeof(actual)); readback->Unmap(0, &empty);
    require(actual == expected, "gpu-copy-exact");
    std::printf("%s_GPU phase=%s expected=%08x actual=%08x sdk=618\n", prefix, phase, expected, actual);
  }
};
void rejected(ID3D12SDKConfiguration1 *sdk, UINT version, const char *path, const wchar_t *directory, const char *name) {
  void *out = reinterpret_cast<void *>(1);
  HRESULT hr = sdk->CreateDeviceFactory(version, path, __uuidof(ID3D12DeviceFactory), &out);
  require(FAILED(hr) && !out, name);
  require(GetModuleHandleW(core_path(directory).c_str()) == nullptr, "rejected-core-not-loaded");
  std::printf("%s_REJECT case=%s output=null loaded=0 hr=0x%08lx\n", prefix, name, static_cast<unsigned long>(hr));
}
void concurrent_free_unused(ID3D12SDKConfiguration1 *sdk) {
  constexpr unsigned rounds = 32;
  const auto requested_path = core_path();
  std::barrier gate(2);
  std::thread freeing([&] {
    for (unsigned round = 0; round < rounds; ++round) {
      gate.arrive_and_wait();
      sdk->FreeUnusedSDKs();
      gate.arrive_and_wait();
    }
  });
  for (unsigned round = 0; round < rounds; ++round) {
    require(GetModuleHandleW(requested_path.c_str()) == nullptr, "concurrent-cold-core-start");
    gate.arrive_and_wait();
    ComPtr<ID3D12DeviceFactory> factory;
    check(sdk->CreateDeviceFactory(618, ".\\sdk-owned\\", IID_PPV_ARGS(&factory)), "concurrent-core-factory-create");
    ComPtr<ID3D12Debug> settings;
    check(factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&settings)), "concurrent-core-configuration");
    settings->EnableDebugLayer();
    const GUID feature = D3D12ExperimentalShaderModels;
    check(factory->EnableExperimentalFeatures(1, &feature, nullptr, nullptr), "concurrent-core-experimental");
    ComPtr<ID3D12DeviceConfiguration> configuration;
    check(factory.As(&configuration), "concurrent-core-public-configuration");
    const auto desc = configuration->GetDesc();
    require(desc.SDKVersion == 618 && desc.NumEnabledExperimentalFeatures == 1 &&
        (desc.Flags & D3D12_DEVICE_FLAG_DEBUG_LAYER_ENABLED), "concurrent-core-configuration-values");
    GUID actual_feature{};
    check(configuration->GetEnabledExperimentalFeatures(&actual_feature, 1), "concurrent-core-feature-get");
    require(actual_feature == feature, "concurrent-core-feature-value");
    const auto module = selected_core();
    gate.arrive_and_wait();
    require(selected_core() == module && factory->GetFlags() == D3D12_DEVICE_FACTORY_FLAG_NONE,
        "concurrent-free-retains-live-core-and-factory");
    configuration.Reset(); settings.Reset(); factory.Reset();
    sdk->FreeUnusedSDKs();
    require(GetModuleHandleW(requested_path.c_str()) == nullptr, "concurrent-core-final-unload");
  }
  freeing.join();
  std::printf("%s_CONCURRENT_FREE rounds=32 factories=32 configurations=32 sdk=618 cold_start=1 module=stable final_unload=1\n", prefix);
}
void set_sdk_child(ID3D12SDKConfiguration1 *sdk, bool retail_apply, UINT requested = 618) {
  constexpr wchar_t key_path[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock";
  constexpr wchar_t value_name[] = L"AllowDevelopmentWithoutDevLicense";
  HKEY key = nullptr; DWORD disposition = 0;
  require(RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, &disposition) == ERROR_SUCCESS, "developer-mode-open-os-state");
  DWORD previous_type = 0, previous_size = 0;
  LONG status = RegQueryValueExW(key, value_name, nullptr, &previous_type, nullptr, &previous_size);
  const bool existed = status == ERROR_SUCCESS;
  require(existed || status == ERROR_FILE_NOT_FOUND, "developer-mode-save-os-state");
  std::vector<BYTE> previous(previous_size);
  if (existed) require(RegQueryValueExW(key, value_name, nullptr, &previous_type, previous.data(), &previous_size) == ERROR_SUCCESS, "developer-mode-read-os-state");
  DWORD enabled = 0;
  require(RegSetValueExW(key, value_name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&enabled), sizeof(enabled)) == ERROR_SUCCESS, "ordinary-os-mode");
  const HRESULT ordinary_hr = sdk->SetSDKVersion(618, ".\\sdk-owned\\");
  require(FAILED(ordinary_hr), "ordinary-set-sdk-denied");
  std::printf("%s_HRESULT operation=ordinary_set hr=0x%08lx\n", prefix, static_cast<unsigned long>(ordinary_hr));
  require(GetModuleHandleW(core_path().c_str()) == nullptr, "ordinary-set-sdk-no-load");
  ComPtr<ID3D12DeviceFactory> ordinary;
  check(sdk->CreateDeviceFactory(618, ".\\sdk-owned\\", IID_PPV_ARGS(&ordinary)), "ordinary-sdk1-factory-supported");
  ordinary.Reset(); sdk->FreeUnusedSDKs();
  require(GetModuleHandleW(core_path().c_str()) == nullptr, "ordinary-factory-unused-unload");
  std::printf("%s_DEVELOPER mode=%s ordinary_set=DENIED ordinary_sdk1=AVAILABLE source=os-registry\n", prefix, retail_apply ? "apply_global" : requested == 617 ? "set_sdk_lower" : "set_sdk");
  if (retail_apply) {
    ComPtr<ID3D12DeviceFactory> factory, initialized;
    check(sdk->CreateDeviceFactory(618, ".\\sdk-owned\\", IID_PPV_ARGS(&factory)), "retail-apply-factory");
    ComPtr<ID3D12Debug> settings;
    check(factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&settings)), "retail-apply-settings");
    settings->EnableDebugLayer();
    const GUID experimental = D3D12ExperimentalShaderModels;
    check(factory->EnableExperimentalFeatures(1, &experimental, nullptr, nullptr), "retail-apply-experimental");
    check(factory->ApplyToGlobalState(), "retail-apply-selected-core");
    factory.Reset(); settings.Reset(); sdk->FreeUnusedSDKs(); selected_core();
    ComPtr<ID3D12Device> global, independent;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&global)), "retail-apply-global-device");
    device_configuration(global.Get(), true, 1);
    check(sdk->CreateDeviceFactory(618, ".\\sdk-alternate\\", IID_PPV_ARGS(&initialized)), "retail-initialize-factory");
    const auto alternate = GetModuleHandleW(core_path(L"sdk-alternate").c_str());
    require(alternate != nullptr && alternate != selected_core(), "alternate-real-module-instance");
    check(initialized->InitializeFromGlobalState(), "retail-initialize-selected-global-state");
    check(initialized->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&independent)), "retail-initialized-device");
    device_configuration(independent.Get(), true, 1);
    const HRESULT apply_hr = initialized->ApplyToGlobalState();
    require(apply_hr == invalid_redist, "alternate-live-global-apply-rejected");
    std::printf("%s_HRESULT operation=alternate_apply hr=0x%08lx\n", prefix, static_cast<unsigned long>(apply_hr));
    require(global->GetDeviceRemovedReason() == S_OK && independent->GetDeviceRemovedReason() == S_OK, "rejected-apply-keeps-global-and-independent");
    selected_core();
    ComPtr<ID3D12Device> unchanged;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&unchanged)), "global-after-rejected-apply");
    device_configuration(unchanged.Get(), true, 1);
    { Copy copy(global.Get()); copy.run("apply_global_child", 0xa6180006, 1); }
    { Copy copy(independent.Get()); copy.run("initialized_alternate_child", 0xa6180007, 1); }
    initialized.Reset(); independent.Reset(); sdk->FreeUnusedSDKs(); selected_core();
    require(GetModuleHandleW(core_path(L"sdk-alternate").c_str()) == nullptr, "alternate-independent-unused-unload");
  } else {
    enabled = 1;
    require(RegSetValueExW(key, value_name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&enabled), sizeof(enabled)) == ERROR_SUCCESS, "developer-os-mode");
    auto exe_sdk = reinterpret_cast<const UINT *>(GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion"));
    require(exe_sdk && *exe_sdk == 619, "setter-overrides-newer-immutable-exe");
    require(sdk->SetSDKVersion(requested, ":invalid-sdk-path:") == E_INVALIDARG, "setter-relative-path-syntax");
    check(sdk->SetSDKVersion(requested, ".\\sdk-missing\\"), "set-sdk-before-device");
    auto installed_path = inbox_core("set_sdk", requested);
    const HRESULT failed_hr = sdk->SetSDKVersion(619, ".\\sdk-foreign\\");
    require(FAILED(failed_hr) && selected_core(installed_path) &&
        GetModuleHandleW(core_path(L"sdk-foreign").c_str()) == nullptr, "failed-set-preserves-inbox-selection");
    std::printf("%s_SET_SDK_REJECT requested=619 selection=UNCHANGED app_loaded=0 hr=0x%08lx\n", prefix, static_cast<unsigned long>(failed_hr));
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "selected-global-device");
    device_configuration(device.Get(), false);
    { Copy copy(device.Get()); copy.run(requested == 617 ? "set_sdk_lower_child" : "set_sdk_before_late", requested == 617 ? 0xa6180009 : 0xa6180004, 1); }
    if (requested == 618) {
      ComPtr<ID3D12DeviceFactory> factory;
      check(sdk->CreateDeviceFactory(618, ".\\sdk-owned\\", IID_PPV_ARGS(&factory)), "child-independent-factory");
      ComPtr<ID3D12Device> independent;
      check(factory->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&independent)), "child-independent-device");
      const HRESULT late_hr = sdk->SetSDKVersion(618, ".\\sdk-alternate\\");
      require(FAILED(late_hr), "set-sdk-late-failure");
      std::printf("%s_HRESULT operation=late_set hr=0x%08lx\n", prefix, static_cast<unsigned long>(late_hr));
      const HRESULT removed_hr = device->GetDeviceRemovedReason();
      require(FAILED(removed_hr) && independent->GetDeviceRemovedReason() == S_OK, "late-set-global-removal-isolation");
      std::printf("%s_HRESULT operation=late_global_removal hr=0x%08lx\n", prefix, static_cast<unsigned long>(removed_hr));
      require(GetModuleHandleW(core_path(L"sdk-alternate").c_str()) == nullptr, "late-set-no-selection-mutation");
      ComPtr<ID3D12Device> future;
      check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&future)), "global-after-late-call");
      device_configuration(future.Get(), false); selected_core(installed_path);
      require(future->GetDeviceRemovedReason() == S_OK && GetModuleHandleW(core_path(L"sdk-alternate").c_str()) == nullptr, "late-set-preserves-selected-sdk");
      { Copy copy(independent.Get()); copy.run("set_sdk_child", 0xa6180005, 1); }
    }
  }
  if (existed) require(RegSetValueExW(key, value_name, 0, previous_type, previous.data(), previous_size) == ERROR_SUCCESS, "restore-developer-os-value");
  else require(RegDeleteValueW(key, value_name) == ERROR_SUCCESS, "restore-absent-developer-os-value");
  RegCloseKey(key);
  if (disposition == REG_CREATED_NEW_KEY) require(RegDeleteKeyW(HKEY_LOCAL_MACHINE, key_path) == ERROR_SUCCESS, "restore-absent-developer-os-key");
  if (retail_apply) std::printf("%s_APPLY_GLOBAL ordinary_mode=PASS sdk=618 selected_core=RETAINED initialized_factory=PASS alternate_apply=REJECTED global_device=LIVE independent_device=LIVE os_state=RESTORED\n", prefix);
  else if (requested == 617) std::printf("%s_SET_SDK_LOWER before_device=PASS exe_sdk=619 os_state=RESTORED sdk=618\n", prefix);
  else std::printf("%s_SET_SDK before_device=PASS late_call=FAIL existing_global=REMOVED independent_device=LIVE selection=UNCHANGED os_state=RESTORED sdk=618\n", prefix);
}
}
int main(int argc, char **argv) {
  wchar_t executable[MAX_PATH]; require(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0, "exe-path");
  exe_directory = executable; exe_directory.resize(exe_directory.find_last_of(L"\\/"));
  auto cwd = exe_directory + L"\\other-cwd";
  require(SetCurrentDirectoryW(cwd.c_str()) != FALSE, "change-cwd-away-from-exe");
  wchar_t actual_cwd[MAX_PATH]; require(GetCurrentDirectoryW(MAX_PATH, actual_cwd) != 0 && !_wcsicmp(actual_cwd, cwd.c_str()), "cwd-readback");
  auto main_module = GetModuleHandleW(L"d3d12.dll"); require(main_module != nullptr, "main-module");
  using Identity = const char *(WINAPI *)();
  auto identity = reinterpret_cast<Identity>(GetProcAddress(main_module, "OwnedGraphicsBackendId"));
  require(identity && !std::strcmp(identity(), "owned-graphics-d3d12"), "main-owned-identity");
  std::printf("%s_BACKEND id=owned-graphics-d3d12\n", prefix);
  auto get_interface = reinterpret_cast<GetInterface>(GetProcAddress(main_module, "D3D12GetInterface")); require(get_interface != nullptr, "get-interface");
  ComPtr<ID3D12SDKConfiguration1> sdk; check(get_interface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdk)), "sdk1-public-interface");
  if (argc == 2 && (!std::strcmp(argv[1], "--set-sdk-child") || !std::strcmp(argv[1], "--set-sdk-lower-child") || !std::strcmp(argv[1], "--apply-global-child"))) {
    set_sdk_child(sdk.Get(), !std::strcmp(argv[1], "--apply-global-child"), !std::strcmp(argv[1], "--set-sdk-lower-child") ? 617 : 618); return 0;
  }
#ifdef OWNED_AGILITY_EXECUTABLE_SELECTION
  require(argc == 2 && !std::strcmp(argv[1], "--executable-selection-child"), "executable-selection-arguments");
#if OWNED_AGILITY_EXECUTABLE_SELECTION == 619
  void *out = reinterpret_cast<void *>(1);
  const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), &out);
  require(FAILED(hr) && !out && GetModuleHandleW(core_path(L"sdk-foreign").c_str()) == nullptr &&
      GetModuleHandleW(core_path().c_str()) == nullptr, "newer-executable-no-valid-redist");
  std::printf("%s_EXECUTABLE_REJECT requested=619 output=null app_loaded=0 hr=0x%08lx\n", prefix, static_cast<unsigned long>(hr));
#else
  ComPtr<ID3D12Device> selected;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&selected)), "executable-exports-device");
  inbox_core("exe_exports", D3D12SDKVersion); device_configuration(selected.Get(), false);
  { Copy copy(selected.Get()); copy.run(D3D12SDKVersion == 617 ? "executable_lower_child" : "executable_exports_child", D3D12SDKVersion == 617 ? 0xa618000a : 0xa6180008, 1); }
#endif
  return 0;
#else
  require(argc == 1, "arguments");
  rejected(sdk.Get(), 619, ".\\sdk-owned\\", L"sdk-owned", "wrong_version");
  rejected(sdk.Get(), 618, ".\\sdk-missing\\", L"sdk-missing", "missing_path");
  rejected(sdk.Get(), 618, ".\\sdk-empty\\", L"sdk-empty", "missing_file");
  rejected(sdk.Get(), 618, ".\\sdk-foreign\\", L"sdk-foreign", "foreign_module");
  rejected(sdk.Get(), 618, ".\\sdk-wrong-abi\\", L"sdk-wrong-abi", "wrong_abi");
  rejected(sdk.Get(), 618, ".\\sdk-other-generation\\", L"sdk-other-generation", "different_generation");
  void *out = reinterpret_cast<void *>(1);
  require(sdk->CreateDeviceFactory(618, ".\\sdk-owned\\", __uuidof(ID3D12DeviceFactory), nullptr) == E_POINTER, "factory-null-output");
  require(FAILED(sdk->CreateDeviceFactory(618, ".\\sdk-owned\\", __uuidof(ID3D12Device), &out)) && !out, "factory-unknown-iid");
  sdk->FreeUnusedSDKs(); require(GetModuleHandleW(core_path().c_str()) == nullptr, "failed-query-no-live-core");
  pass("rejected_modules");
  ComPtr<ID3D12DeviceFactory> factory, other;
  check(sdk->CreateDeviceFactory(618, ".\\sdk-owned\\", IID_PPV_ARGS(&factory)), "selected-factory");
  auto module = selected_core();
  auto version = reinterpret_cast<const UINT *>(GetProcAddress(module, "D3D12SDKVersion"));
  auto generation = reinterpret_cast<const char *>(GetProcAddress(module, "OwnedD3D12CoreGeneration"));
  require(generation && std::strlen(generation) == 32, "core-generation-data");
  std::printf("%s_CORE sdk=%u generation=%s path=%ls cwd_base=exe source=staged-copy\n", prefix, *version, generation, core_path().c_str());
  auto core_get_interface = reinterpret_cast<GetInterface>(GetProcAddress(module, "D3D12GetInterface"));
  check(core_get_interface(installed_factory, IID_PPV_ARGS(&other)), "selected-core-public-factory");
  sdk->FreeUnusedSDKs(); selected_core(); require(factory->GetFlags() == D3D12_DEVICE_FACTORY_FLAG_NONE, "live-factory-after-free-unused");
  pass("selected_core_path_version");
  ComPtr<ID3D12DeviceConfiguration> factory_configuration;
  check(factory.As(&factory_configuration), "factory-public-configuration");
  auto factory_desc = factory_configuration->GetDesc();
  require(factory_desc.SDKVersion == 618, "factory-public-sdk618");
  ComPtr<ID3D12Device> before, after, independent;
  check(factory->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&before)), "before-local-settings");
  ComPtr<ID3D12Debug> settings; check(factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(&settings)), "factory-local-settings");
  settings->EnableDebugLayer();
  const GUID experimental = D3D12ExperimentalShaderModels;
  check(factory->EnableExperimentalFeatures(1, &experimental, nullptr, nullptr), "factory-local-experimental");
  factory_desc = factory_configuration->GetDesc();
  require(factory_desc.SDKVersion == 618 && factory_desc.NumEnabledExperimentalFeatures == 1, "factory-configuration-live-state");
  check(factory->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&after)), "after-local-settings");
  check(other->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&independent)), "independent-factory");
  device_configuration(before.Get(), false); device_configuration(after.Get(), true, 1); device_configuration(independent.Get(), false);
  { Copy copy(after.Get()); copy.run("configured", 0xa6180001, 1); }
  factory.Reset(); other.Reset(); before.Reset(); independent.Reset(); sdk->FreeUnusedSDKs(); selected_core();
  settings->EnableDebugLayer(); device_configuration(after.Get(), true, 1); require(after->GetDeviceRemovedReason() == S_OK, "retained-device-not-removed");
  pass("factory_configuration_gpu");
  {
    Copy copy(after.Get()); copy.run("retained", 0xa6180002, 1);
    after.Reset(); sdk->FreeUnusedSDKs(); selected_core();
    ComPtr<ID3D12Device> child_device; check(copy.queue->GetDevice(IID_PPV_ARGS(&child_device)), "child-retained-device");
    device_configuration(child_device.Get(), true, 1); child_device.Reset();
    copy.run("children", 0xa6180003, 2, true);
  }
  sdk->FreeUnusedSDKs(); selected_core(); settings->EnableDebugLayer(); settings.Reset();
  sdk->FreeUnusedSDKs(); selected_core();
  factory_desc = factory_configuration->GetDesc();
  require(factory_desc.SDKVersion == 618 && factory_desc.NumEnabledExperimentalFeatures == 1, "retained-factory-configuration-after-all-children");
  check(core_get_interface(installed_factory, IID_PPV_ARGS(&other)), "direct-core-retained-public-factory");
  {
    ComPtr<ID3D12DeviceConfiguration> direct_configuration;
    check(other.As(&direct_configuration), "direct-core-public-configuration");
    require(direct_configuration->GetDesc().SDKVersion == 618, "direct-core-public-sdk618");
  }
  factory_configuration.Reset();
  sdk->FreeUnusedSDKs();
  require(GetModuleHandleW(core_path().c_str()) == module, "direct-core-only-factory-retains-selected-module");
  require(other->GetFlags() == D3D12_DEVICE_FACTORY_FLAG_NONE, "direct-core-retained-factory-operation");
  {
    ComPtr<ID3D12DeviceConfiguration> direct_configuration;
    check(other.As(&direct_configuration), "direct-core-only-owner-configuration");
    require(direct_configuration->GetDesc().SDKVersion == 618, "direct-core-only-owner-sdk618");
  }
  other.Reset(); sdk->FreeUnusedSDKs();
  require(GetModuleHandleW(core_path().c_str()) == nullptr, "direct-core-final-factory-releases-selected-module");
  std::printf("%s_DIRECT_CORE_FACTORY sdk=618 selected_module=retained owners=1 final_unload=1\n", prefix);
  pass("retained_objects_gpu");
  sdk->FreeUnusedSDKs(); require(GetModuleHandleW(core_path().c_str()) == nullptr, "unused-core-actually-unloaded");
  concurrent_free_unused(sdk.Get());
  std::printf("%s_UNLOAD path=%ls live_objects=0 loaded=0\n", prefix, core_path().c_str());
  pass("unused_core_unload");
  const struct { const wchar_t *mode; const wchar_t *suffix; } children[] = {
    {L"--set-sdk-lower-child", L"-executable-619.exe"}, {L"--set-sdk-child", L"-executable-619.exe"},
    {L"--apply-global-child", L".exe"}, {L"--executable-selection-child", L"-executable-617.exe"},
    {L"--executable-selection-child", L"-executable-618.exe"}, {L"--executable-selection-child", L"-executable-619.exe"}};
  for (const auto &child : children) {
    std::wstring child_executable = executable;
    child_executable.resize(child_executable.find_last_of(L'.'));
    child_executable += child.suffix;
    std::wstring command = L"\"" + child_executable + L"\" " + child.mode;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    std::fflush(stdout);
    require(CreateProcessW(child_executable.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, cwd.c_str(), &startup, &process) != FALSE, "fresh-global-configuration-process");
    DWORD status = WaitForSingleObject(process.hProcess, 30000), code = 1;
    require(GetExitCodeProcess(process.hProcess, &code) != FALSE, "global-configuration-child-exit-code");
    CloseHandle(process.hThread); CloseHandle(process.hProcess); require(status == WAIT_OBJECT_0 && code == 0, "global-configuration-child-result");
  }
  pass("set_sdk_version_fresh_process");
  std::printf("%s_PASS cases=%u\n", prefix, cases); return 0;
#endif
}
