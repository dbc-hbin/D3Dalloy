#define main ownedTemporalContractSupportMain
#include "owned-d3d12-temporal.cpp"
#undef main

#include <cwctype>
#include <string>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

extern "C" {
unsigned int ngx_contract_result_success();
unsigned int ngx_contract_result_invalid_parameter();
unsigned int ngx_contract_result_feature_not_found();
unsigned int ngx_contract_result_not_initialized();
unsigned int ngx_contract_result_unsupported_parameter();
unsigned int ngx_contract_sdk_version();
unsigned int ngx_contract_feature_supersampling();
unsigned int ngx_contract_get_output_subrect_default(NVSDK_NGX_Parameter*, int*);
void ngx_contract_set_create(
    NVSDK_NGX_Parameter*, unsigned int, unsigned int, unsigned int, unsigned int, int);
void ngx_contract_set_frame(
    NVSDK_NGX_Parameter*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
    ID3D12Resource*, unsigned int, unsigned int);
}

namespace {

using NgxResult = unsigned int;
using NgxInit = NgxResult (*)(
    unsigned long long, const wchar_t*, ID3D12Device*, const void*, unsigned int);
using NgxShutdown1 = NgxResult (*)(ID3D12Device*);
using NgxAllocateParameters = NgxResult (*)(NVSDK_NGX_Parameter**);
using NgxGetCapabilityParameters = NgxResult (*)(NVSDK_NGX_Parameter**);
using NgxGetParameters = NgxResult (*)(NVSDK_NGX_Parameter**);
using NgxDestroyParameters = NgxResult (*)(NVSDK_NGX_Parameter*);
using NgxCreateFeature = NgxResult (*)(
    ID3D12GraphicsCommandList*, unsigned int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using NgxEvaluateFeature = NgxResult (*)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
using NgxReleaseFeature = NgxResult (*)(NVSDK_NGX_Handle*);

constexpr wchar_t kNvngxPath[] = L"C:\\windows\\system32\\nvngx.dll";
constexpr char kNvngxMarker[] = "own-ngxd3d12";

void expectNgx(NgxResult actual, NgxResult expected, const char* step) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "OWNED_D3D12_NGX_CONTRACT_RESULT_MISMATCH "
                     "step=%s expected=0x%08x actual=0x%08x\n",
                     step, expected, actual);
        fail(step, static_cast<HRESULT>(actual));
    }
}

template <typename T>
T loadNgxExport(HMODULE module, const char* name) {
    FARPROC address = GetProcAddress(module, name);
    if (!address) {
        std::fprintf(stderr, "OWNED_D3D12_NGX_CONTRACT_EXPORT_MISSING name=%s\n", name);
        fail("GetProcAddress(nvngx)", HRESULT_FROM_WIN32(GetLastError()));
    }
    T result{};
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

std::wstring normalizePath(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/')
            ch = L'\\';
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return path;
}

bool pathEndsWith(const std::wstring& value, const std::wstring& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool moduleHasMarker(HMODULE module, const char* marker) {
    std::array<wchar_t, 32768> path{};
    const DWORD path_length =
        GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!path_length || path_length >= path.size())
        fail("GetModuleFileNameW(nvngx)", HRESULT_FROM_WIN32(GetLastError()));
    if (!pathEndsWith(normalizePath(std::wstring(path.data(), path_length)),
                      L"\\windows\\system32\\nvngx.dll"))
        fail("nvngx loaded outside system32");

    HANDLE file = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        fail("CreateFileW(nvngx)", HRESULT_FROM_WIN32(GetLastError()));
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 64ll * 1024ll * 1024ll) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        fail("GetFileSizeEx(nvngx)", error ? HRESULT_FROM_WIN32(error) : E_FAIL);
    }
    std::vector<char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD bytes_read = 0;
    const BOOL read_ok =
        ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytes_read, nullptr);
    const DWORD read_error = read_ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!read_ok || bytes_read != bytes.size())
        fail("ReadFile(nvngx)",
             HRESULT_FROM_WIN32(read_error ? read_error : ERROR_READ_FAULT));
    const std::size_t marker_length = std::strlen(marker);
    return std::search(bytes.begin(), bytes.end(), marker, marker + marker_length) != bytes.end();
}

struct NgxApi {
    HMODULE module = nullptr;
    NgxInit init = nullptr;
    NgxShutdown1 shutdown1 = nullptr;
    NgxAllocateParameters allocate = nullptr;
    NgxGetCapabilityParameters get_capability = nullptr;
    NgxGetParameters get_parameters = nullptr;
    NgxDestroyParameters destroy = nullptr;
    NgxCreateFeature create = nullptr;
    NgxEvaluateFeature evaluate = nullptr;
    NgxReleaseFeature release = nullptr;

    NgxApi() {
        module = LoadLibraryW(kNvngxPath);
        if (!module)
            fail("LoadLibraryW(system32 nvngx.dll)", HRESULT_FROM_WIN32(GetLastError()));
        if (!moduleHasMarker(module, kNvngxMarker))
            fail("nvngx owned marker missing");
        init = loadNgxExport<NgxInit>(module, "NVSDK_NGX_D3D12_Init");
        shutdown1 = loadNgxExport<NgxShutdown1>(module, "NVSDK_NGX_D3D12_Shutdown1");
        allocate = loadNgxExport<NgxAllocateParameters>(
            module, "NVSDK_NGX_D3D12_AllocateParameters");
        get_capability = loadNgxExport<NgxGetCapabilityParameters>(
            module, "NVSDK_NGX_D3D12_GetCapabilityParameters");
        get_parameters = loadNgxExport<NgxGetParameters>(
            module, "NVSDK_NGX_D3D12_GetParameters");
        destroy = loadNgxExport<NgxDestroyParameters>(
            module, "NVSDK_NGX_D3D12_DestroyParameters");
        create = loadNgxExport<NgxCreateFeature>(module, "NVSDK_NGX_D3D12_CreateFeature");
        evaluate = loadNgxExport<NgxEvaluateFeature>(
            module, "NVSDK_NGX_D3D12_EvaluateFeature");
        release = loadNgxExport<NgxReleaseFeature>(module, "NVSDK_NGX_D3D12_ReleaseFeature");
    }

    ~NgxApi() {
        if (module)
            FreeLibrary(module);
    }
};

NVSDK_NGX_Parameter* allocateParams(const NgxApi& api) {
    NVSDK_NGX_Parameter* params = nullptr;
    expectNgx(api.allocate(&params), ngx_contract_result_success(), "AllocateParameters");
    if (!params)
        fail("AllocateParameters returned null");
    return params;
}

void destroyParams(const NgxApi& api, NVSDK_NGX_Parameter*& params) {
    if (!params)
        return;
    expectNgx(api.destroy(params), ngx_contract_result_success(), "DestroyParameters");
    params = nullptr;
}

NVSDK_NGX_Handle* createFeature(
    const NgxApi& api, Gpu& gpu, bool enable_output_subrects) {
    NVSDK_NGX_Parameter* params = allocateParams(api);
    ngx_contract_set_create(
        params, kInputWidth, kInputHeight, kOutputWidth, kOutputHeight,
        enable_output_subrects ? 1 : 0);
    Recording recording = gpu.makeRecording();
    NVSDK_NGX_Handle* handle = nullptr;
    expectNgx(api.create(recording.list.Get(), ngx_contract_feature_supersampling(),
                         params, &handle),
              ngx_contract_result_success(), "CreateFeature");
    if (!handle)
        fail("CreateFeature returned null handle");
    check(recording.list->Close(), "CreateFeature Close");
    destroyParams(api, params);
    return handle;
}

struct FrameResources {
    ComPtr<ID3D12Resource> color;
    OutputResource output;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12Resource> motion;
};

FrameResources makeFrameResources(Gpu& gpu) {
    FrameResources resources;
    resources.color = makeColor(gpu, 112);
    resources.output = makeOutput(gpu, 2000);
    resources.depth = makeDepth(gpu);
    resources.motion = makeMotion(gpu);
    return resources;
}

void evaluateOffset(
    const NgxApi& api, Gpu& gpu, NVSDK_NGX_Handle* feature,
    const FrameResources& resources, UINT output_base_x, UINT output_base_y,
    NgxResult expected, const char* step) {
    NVSDK_NGX_Parameter* params = allocateParams(api);
    ngx_contract_set_frame(
        params, resources.color.Get(), resources.output.resource.Get(), resources.depth.Get(),
        resources.motion.Get(), output_base_x, output_base_y);
    Recording recording = gpu.makeRecording();
    transition(recording.list.Get(), resources.output.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    expectNgx(api.evaluate(recording.list.Get(), feature, params, nullptr), expected, step);
    check(recording.list->Close(), "offset Evaluate Close");
    destroyParams(api, params);
}

void testCapabilityDefault(const NgxApi& api) {
    NVSDK_NGX_Parameter* params = nullptr;
    expectNgx(api.get_capability(&params), ngx_contract_result_success(),
              "GetCapabilityParameters");
    if (!params)
        fail("GetCapabilityParameters returned null");
    int output_subrects = -1;
    expectNgx(ngx_contract_get_output_subrect_default(params, &output_subrects),
              ngx_contract_result_success(), "Get DLSS.Enable.Output.Subrects");
    if (output_subrects != 0)
        fail("capability output subrect default is not disabled");
    destroyParams(api, params);

    NVSDK_NGX_Parameter* managed = nullptr;
    expectNgx(api.get_parameters(&managed), ngx_contract_result_success(),
              "GetParameters");
    if (!managed)
        fail("GetParameters returned null");
    output_subrects = -1;
    expectNgx(ngx_contract_get_output_subrect_default(managed, &output_subrects),
              ngx_contract_result_success(), "GetParameters DLSS.Enable.Output.Subrects");
    if (output_subrects != 0)
        fail("managed parameter output subrect default is not disabled");

    std::puts("OWNED_D3D12_NGX_CONTRACT_CASE_PASS case=capability_parameter_output_subrect_default value=0");
}

void testHandleLifetime(const NgxApi& api, Gpu& gpu) {
    NVSDK_NGX_Handle* stale = createFeature(api, gpu, false);
    expectNgx(api.release(stale), ngx_contract_result_success(), "ReleaseFeature(stale seed)");

    std::array<NVSDK_NGX_Handle*, 65> released{};
    released[0] = stale;
    for (unsigned int iteration = 0; iteration < 64; ++iteration) {
        NVSDK_NGX_Handle* current = createFeature(api, gpu, false);
        for (unsigned int prior = 0; prior <= iteration; ++prior) {
            if (current == released[prior])
                fail("feature handle token reused after release");
        }
        expectNgx(api.release(current), ngx_contract_result_success(), "ReleaseFeature(loop)");
        released[iteration + 1] = current;
    }
    expectNgx(api.release(stale), ngx_contract_result_feature_not_found(),
              "ReleaseFeature(stale handle)");
    std::puts("OWNED_D3D12_NGX_CONTRACT_CASE_PASS case=opaque_handle_lifetime iterations=64 stale_not_found=1");
}

void testOutputSubrectContract(const NgxApi& api, Gpu& gpu) {
    const FrameResources resources = makeFrameResources(gpu);

    NVSDK_NGX_Handle* disabled = createFeature(api, gpu, false);
    evaluateOffset(api, gpu, disabled, resources, 1, 0,
                   ngx_contract_result_unsupported_parameter(),
                   "disabled output subrect offset");
    expectNgx(api.release(disabled), ngx_contract_result_success(),
              "ReleaseFeature(disabled subrect)");

    NVSDK_NGX_Handle* enabled = createFeature(api, gpu, true);
    evaluateOffset(api, gpu, enabled, resources, 8, 0,
                   ngx_contract_result_success(), "enabled valid output subrect offset");
    evaluateOffset(api, gpu, enabled, resources, 9, 0,
                   ngx_contract_result_invalid_parameter(),
                   "enabled out-of-bounds output subrect offset");
    expectNgx(api.release(enabled), ngx_contract_result_success(),
              "ReleaseFeature(enabled subrect)");
    std::puts("OWNED_D3D12_NGX_CONTRACT_CASE_PASS case=output_subrect disabled_rejected=1 enabled_valid=1 backend_bounds=1");
}

void initDevice(const NgxApi& api, Gpu& gpu, const wchar_t* data_path) {
    expectNgx(api.init(0x4f574e45444e4758ull, data_path, gpu.device.Get(), nullptr,
                       ngx_contract_sdk_version()),
              ngx_contract_result_success(), "NVSDK_NGX_D3D12_Init");
}

int runContractFixture() {
    requireOwnedBackend();
    NgxApi api;

    NVSDK_NGX_Parameter* before_init = reinterpret_cast<NVSDK_NGX_Parameter*>(1);
    expectNgx(api.allocate(&before_init), ngx_contract_result_not_initialized(),
              "AllocateParameters before Init");
    if (before_init)
        fail("pre-init AllocateParameters did not clear output");
    NVSDK_NGX_Parameter* capability_before_init =
        reinterpret_cast<NVSDK_NGX_Parameter*>(1);
    expectNgx(api.get_capability(&capability_before_init),
              ngx_contract_result_not_initialized(),
              "GetCapabilityParameters before Init");
    if (capability_before_init)
        fail("pre-init GetCapabilityParameters did not clear output");
    NVSDK_NGX_Parameter* managed_before_init =
        reinterpret_cast<NVSDK_NGX_Parameter*>(1);
    expectNgx(api.get_parameters(&managed_before_init),
              ngx_contract_result_not_initialized(),
              "GetParameters before Init");
    if (managed_before_init)
        fail("pre-init GetParameters did not clear output");

    wchar_t data_path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, data_path))
        fail("GetTempPathW", HRESULT_FROM_WIN32(GetLastError()));

    Gpu first;
    Gpu second;
    ComPtr<IUnknown> first_identity;
    ComPtr<IUnknown> second_identity;
    check(first.device.As(&first_identity), "first device IUnknown identity");
    check(second.device.As(&second_identity), "second device IUnknown identity");
    if (first_identity.Get() == second_identity.Get())
        fail("Shutdown1 nullptr fixture requires two distinct D3D12 devices");
    initDevice(api, first, data_path);
    initDevice(api, second, data_path);

    testCapabilityDefault(api);
    testHandleLifetime(api, first);
    testOutputSubrectContract(api, first);
    NVSDK_NGX_Handle* live_first = createFeature(api, first, false);
    NVSDK_NGX_Handle* live_second = createFeature(api, second, false);

    expectNgx(api.shutdown1(nullptr), ngx_contract_result_success(),
              "Shutdown1(nullptr)");
    NVSDK_NGX_Parameter* after_shutdown = reinterpret_cast<NVSDK_NGX_Parameter*>(1);
    expectNgx(api.allocate(&after_shutdown), ngx_contract_result_not_initialized(),
              "AllocateParameters after Shutdown1(nullptr)");
    if (after_shutdown)
        fail("post-shutdown AllocateParameters did not clear output");
    NVSDK_NGX_Parameter* capability_after_shutdown =
        reinterpret_cast<NVSDK_NGX_Parameter*>(1);
    expectNgx(api.get_capability(&capability_after_shutdown),
              ngx_contract_result_not_initialized(),
              "GetCapabilityParameters after Shutdown1(nullptr)");
    if (capability_after_shutdown)
        fail("post-shutdown GetCapabilityParameters did not clear output");
    NVSDK_NGX_Parameter* managed_after_shutdown =
        reinterpret_cast<NVSDK_NGX_Parameter*>(1);
    expectNgx(api.get_parameters(&managed_after_shutdown),
              ngx_contract_result_not_initialized(),
              "GetParameters after Shutdown1(nullptr)");
    if (managed_after_shutdown)
        fail("post-shutdown GetParameters did not clear output");
    expectNgx(api.release(live_first), ngx_contract_result_feature_not_found(),
              "first-device feature after Shutdown1(nullptr)");
    expectNgx(api.release(live_second), ngx_contract_result_feature_not_found(),
              "second-device feature after Shutdown1(nullptr)");

    initDevice(api, first, data_path);
    NVSDK_NGX_Parameter* after_reinit = allocateParams(api);
    ngx_contract_set_create(
        after_reinit, kInputWidth, kInputHeight, kOutputWidth, kOutputHeight, 0);
    Recording still_shutdown_second = second.makeRecording();
    NVSDK_NGX_Handle* unexpected_second = nullptr;
    expectNgx(api.create(still_shutdown_second.list.Get(),
                         ngx_contract_feature_supersampling(), after_reinit,
                         &unexpected_second),
              ngx_contract_result_not_initialized(),
              "second device remains shutdown after first-device re-init");
    if (unexpected_second)
        fail("CreateFeature returned handle for second shutdown device");
    check(still_shutdown_second.list->Close(), "second shutdown CreateFeature Close");
    destroyParams(api, after_reinit);
    expectNgx(api.shutdown1(first.device.Get()), ngx_contract_result_success(),
              "Shutdown1(device) after re-init");

    std::puts(
        "OWNED_D3D12_NGX_CONTRACTS_PASS shutdown_null_all_devices=1 "
        "reinit=1 opaque_handle_lifetime=1 capability_defaults=1 "
        "output_subrect_gate=1 backend_bounds=1");
    return 0;
}

} // namespace

int main() {
    return runContractFixture();
}
