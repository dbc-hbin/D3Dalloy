// Reuse the already CPU-validated D3D12 resource/upload/readback helpers. The
// temporal feature itself is exercised only through nvngx.dll exports below.
#define main ownedTemporalFixtureMain
#include "owned-d3d12-temporal.cpp"
#undef main

#include <cwctype>
#include <string>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

extern "C" {
unsigned int ngx_fixture_result_success();
unsigned int ngx_fixture_result_feature_not_supported();
unsigned int ngx_fixture_result_feature_not_found();
unsigned int ngx_fixture_result_invalid_parameter();
unsigned int ngx_fixture_result_not_initialized();
unsigned int ngx_fixture_result_missing_input();
unsigned int ngx_fixture_result_unsupported_parameter();
unsigned int ngx_fixture_sdk_version();
unsigned int ngx_fixture_feature_supersampling();
unsigned int ngx_fixture_flags74();
void ngx_fixture_reset(NVSDK_NGX_Parameter*);
void ngx_fixture_set_create_flags74(
    NVSDK_NGX_Parameter*, unsigned int, unsigned int, unsigned int, unsigned int);
void ngx_fixture_set_frame_flags74(
    NVSDK_NGX_Parameter*, ID3D12Resource*, ID3D12Resource*,
    ID3D12Resource*, ID3D12Resource*, int);
void ngx_fixture_set_frame_output_offset(NVSDK_NGX_Parameter*, unsigned int);
void ngx_fixture_set_frame_nan_jitter(NVSDK_NGX_Parameter*);
}

namespace {

using NgxResult = unsigned int;
using NgxInit = NgxResult (*)(
    unsigned long long, const wchar_t*, ID3D12Device*, const void*, unsigned int);
using NgxShutdown1 = NgxResult (*)(ID3D12Device*);
using NgxAllocateParameters = NgxResult (*)(NVSDK_NGX_Parameter**);
using NgxDestroyParameters = NgxResult (*)(NVSDK_NGX_Parameter*);
using NgxCreateFeature = NgxResult (*)(
    ID3D12GraphicsCommandList*, unsigned int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using NgxEvaluateFeature = NgxResult (*)(
    ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    NVSDK_NGX_Parameter*, void*);
using NgxReleaseFeature = NgxResult (*)(NVSDK_NGX_Handle*);

constexpr wchar_t kOwnedNgxSystemPath[] = L"C:\\windows\\system32\\nvngx.dll";
constexpr char kOwnedNgxMarker[] = "own-ngxd3d12";
constexpr UINT kNgxFrames = 8;

void checkNgxResult(NgxResult actual, NgxResult expected, const char* step) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "OWNED_D3D12_NGX_RESULT_MISMATCH step=%s expected=0x%08x actual=0x%08x\n",
                     step, expected, actual);
        fail(step, static_cast<HRESULT>(actual));
    }
}

template <typename T>
T loadExport(HMODULE module, const char* name) {
    FARPROC address = GetProcAddress(module, name);
    if (!address) {
        std::fprintf(stderr, "OWNED_D3D12_NGX_EXPORT_MISSING name=%s error=%lu\n",
                     name, static_cast<unsigned long>(GetLastError()));
        fail("GetProcAddress(nvngx)", HRESULT_FROM_WIN32(GetLastError()));
    }
    T result{};
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

std::wstring normalizedPath(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/')
            ch = L'\\';
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return path;
}

bool endsWith(const std::wstring& value, const std::wstring& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::wstring modulePath(HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const DWORD length =
        GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        fail("GetModuleFileNameW(nvngx)", HRESULT_FROM_WIN32(GetLastError()));
    return std::wstring(path.data(), length);
}

bool fileContainsMarker(const std::wstring& path, const char* marker) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        fail("CreateFileW(nvngx marker scan)", HRESULT_FROM_WIN32(GetLastError()));

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 64ll * 1024ll * 1024ll) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        fail("GetFileSizeEx(nvngx)", error ? HRESULT_FROM_WIN32(error) : E_FAIL);
    }

    std::vector<char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok =
        ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok || read != bytes.size())
        fail("ReadFile(nvngx)", HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT));

    const std::size_t markerLength = std::strlen(marker);
    return std::search(bytes.begin(), bytes.end(), marker, marker + markerLength) != bytes.end();
}

struct NgxApi {
    HMODULE module = nullptr;
    NgxInit init = nullptr;
    NgxShutdown1 shutdown1 = nullptr;
    NgxAllocateParameters allocate = nullptr;
    NgxDestroyParameters destroy = nullptr;
    NgxCreateFeature create = nullptr;
    NgxEvaluateFeature evaluate = nullptr;
    NgxReleaseFeature release = nullptr;
    std::wstring path;

    NgxApi() {
        module = LoadLibraryW(kOwnedNgxSystemPath);
        if (!module)
            fail("LoadLibraryW(system32 nvngx.dll)", HRESULT_FROM_WIN32(GetLastError()));

        path = modulePath(module);
        const std::wstring normalized = normalizedPath(path);
        if (!endsWith(normalized, L"\\windows\\system32\\nvngx.dll")) {
            std::fwprintf(stderr, L"OWNED_D3D12_NGX_PATH_MISMATCH path=%ls\n", path.c_str());
            fail("nvngx loaded outside system32");
        }
        if (!fileContainsMarker(path, kOwnedNgxMarker))
            fail("nvngx owned marker missing");

        const char* requiredExports[] = {
            "NVSDK_NGX_D3D12_Init",
            "NVSDK_NGX_D3D12_Init_Ext",
            "NVSDK_NGX_D3D12_Init_ProjectID",
            "NVSDK_NGX_D3D12_Init_with_ProjectID",
            "NVSDK_NGX_D3D12_AllocateParameters",
            "NVSDK_NGX_D3D12_DestroyParameters",
            "NVSDK_NGX_D3D12_CreateFeature",
            "NVSDK_NGX_D3D12_EvaluateFeature",
            "NVSDK_NGX_D3D12_EvaluateFeature_C",
            "NVSDK_NGX_D3D12_ReleaseFeature",
            "NVSDK_NGX_D3D12_Shutdown1",
        };
        for (const char* name : requiredExports) {
            if (!GetProcAddress(module, name)) {
                std::fprintf(stderr, "OWNED_D3D12_NGX_EXPORT_MISSING name=%s\n", name);
                fail("required nvngx D3D12 export missing");
            }
        }

        init = loadExport<NgxInit>(module, "NVSDK_NGX_D3D12_Init");
        shutdown1 = loadExport<NgxShutdown1>(module, "NVSDK_NGX_D3D12_Shutdown1");
        allocate =
            loadExport<NgxAllocateParameters>(module, "NVSDK_NGX_D3D12_AllocateParameters");
        destroy =
            loadExport<NgxDestroyParameters>(module, "NVSDK_NGX_D3D12_DestroyParameters");
        create = loadExport<NgxCreateFeature>(module, "NVSDK_NGX_D3D12_CreateFeature");
        evaluate =
            loadExport<NgxEvaluateFeature>(module, "NVSDK_NGX_D3D12_EvaluateFeature");
        release = loadExport<NgxReleaseFeature>(module, "NVSDK_NGX_D3D12_ReleaseFeature");

        std::printf("OWNED_D3D12_NGX_MODULE path=%ls marker=%s exports=ok\n",
                    path.c_str(), kOwnedNgxMarker);
    }

    ~NgxApi() {
        if (module)
            FreeLibrary(module);
    }
};

NVSDK_NGX_Parameter* allocateParams(const NgxApi& api) {
    NVSDK_NGX_Parameter* params = nullptr;
    checkNgxResult(api.allocate(&params), ngx_fixture_result_success(),
                   "NVSDK_NGX_D3D12_AllocateParameters");
    if (!params)
        fail("AllocateParameters returned null");
    return params;
}

void resetAndDestroyParams(const NgxApi& api, NVSDK_NGX_Parameter*& params) {
    if (!params)
        return;
    ngx_fixture_reset(params);
    checkNgxResult(api.destroy(params), ngx_fixture_result_success(),
                   "NVSDK_NGX_D3D12_DestroyParameters");
    params = nullptr;
}

NVSDK_NGX_Handle* createFeature(const NgxApi& api, Gpu& gpu) {
    NVSDK_NGX_Parameter* params = allocateParams(api);
    ngx_fixture_set_create_flags74(
        params, kInputWidth, kInputHeight, kOutputWidth, kOutputHeight);

    Recording recording = gpu.makeRecording();
    NVSDK_NGX_Handle* handle = nullptr;
    checkNgxResult(
        api.create(recording.list.Get(), ngx_fixture_feature_supersampling(),
                   params, &handle),
        ngx_fixture_result_success(), "NVSDK_NGX_D3D12_CreateFeature(flags74)");
    if (!handle)
        fail("CreateFeature returned null handle");
    check(recording.list->Close(), "NGX CreateFeature Close");
    resetAndDestroyParams(api, params);
    return handle;
}

void runOfficialErrorCases(const NgxApi& api, Gpu& gpu,
                           ID3D12Resource* depth, ID3D12Resource* motion,
                           NVSDK_NGX_Handle* validHandle) {
    {
        NVSDK_NGX_Parameter* params = allocateParams(api);
        ngx_fixture_set_create_flags74(
            params, kInputWidth, kInputHeight, kOutputWidth, kOutputHeight);
        Recording closed = gpu.makeRecording();
        check(closed.list->Close(), "closed CreateFeature Close");
        NVSDK_NGX_Handle* unexpected = nullptr;
        checkNgxResult(
            api.create(closed.list.Get(), ngx_fixture_feature_supersampling(),
                       params, &unexpected),
            ngx_fixture_result_invalid_parameter(), "closed_list_create_feature");
        if (unexpected)
            fail("closed CreateFeature returned handle");
        resetAndDestroyParams(api, params);
        std::printf("OWNED_D3D12_NGX_CASE_PASS case=closed_create result=0x%08x\n",
                    ngx_fixture_result_invalid_parameter());
    }

    {
        NVSDK_NGX_Parameter* params = allocateParams(api);
        ngx_fixture_set_create_flags74(
            params, kInputWidth, kInputHeight, kOutputWidth, kOutputHeight);
        Recording open = gpu.makeRecording();
        NVSDK_NGX_Handle* unexpected = nullptr;
        checkNgxResult(
            api.create(open.list.Get(), 0u, params, &unexpected),
            ngx_fixture_result_feature_not_supported(), "unsupported_feature_create");
        check(open.list->Close(), "unsupported CreateFeature Close");
        resetAndDestroyParams(api, params);
        std::printf(
            "OWNED_D3D12_NGX_CASE_PASS case=unsupported_feature result=0x%08x\n",
            ngx_fixture_result_feature_not_supported());
    }

    ComPtr<ID3D12Resource> color = makeColor(gpu, 112);
    OutputResource output = makeOutput(gpu, 900);

    {
        NVSDK_NGX_Parameter* params = allocateParams(api);
        ngx_fixture_set_frame_flags74(
            params, color.Get(), output.resource.Get(), depth, motion, 0);
        Recording closed = gpu.makeRecording();
        check(closed.list->Close(), "closed Evaluate Close");
        checkNgxResult(
            api.evaluate(closed.list.Get(), validHandle, params, nullptr),
            ngx_fixture_result_invalid_parameter(), "closed_list_evaluate");
        resetAndDestroyParams(api, params);
        std::printf("OWNED_D3D12_NGX_CASE_PASS case=closed_evaluate result=0x%08x\n",
                    ngx_fixture_result_invalid_parameter());
    }

    {
        NVSDK_NGX_Parameter* params = allocateParams(api);
        ngx_fixture_set_frame_flags74(
            params, color.Get(), output.resource.Get(), depth, motion, 0);
        ngx_fixture_set_frame_output_offset(params, 1u);
        Recording open = gpu.makeRecording();
        checkNgxResult(
            api.evaluate(open.list.Get(), validHandle, params, nullptr),
            ngx_fixture_result_unsupported_parameter(), "nonzero_output_offset");
        check(open.list->Close(), "offset negative Close");
        resetAndDestroyParams(api, params);
        std::printf("OWNED_D3D12_NGX_CASE_PASS case=output_offset result=0x%08x\n",
                    ngx_fixture_result_unsupported_parameter());
    }

    {
        NVSDK_NGX_Parameter* params = allocateParams(api);
        ngx_fixture_set_frame_flags74(
            params, color.Get(), output.resource.Get(), depth, motion, 0);
        ngx_fixture_set_frame_nan_jitter(params);
        Recording open = gpu.makeRecording();
        checkNgxResult(
            api.evaluate(open.list.Get(), validHandle, params, nullptr),
            ngx_fixture_result_invalid_parameter(), "nan_jitter");
        check(open.list->Close(), "jitter negative Close");
        resetAndDestroyParams(api, params);
        std::printf("OWNED_D3D12_NGX_CASE_PASS case=nan_jitter result=0x%08x\n",
                    ngx_fixture_result_invalid_parameter());
    }
}

void runContinuousFrames(const NgxApi& api, Gpu& gpu,
                         ID3D12Resource* depth, ID3D12Resource* motion) {
    NVSDK_NGX_Handle* feature = createFeature(api, gpu);
    std::printf(
        "OWNED_D3D12_NGX_CREATE flags=%u input=64x64 logical_output=128x128 "
        "output_backing=136x136 raw_pre_exposure=0 raw_exposure_scale=0 "
        "expected_semantic_pre_exposure=1 expected_semantic_exposure_scale=1\n",
        ngx_fixture_flags74());

    runOfficialErrorCases(api, gpu, depth, motion, feature);

    for (UINT frame = 0; frame < kNgxFrames; ++frame) {
        const std::uint8_t inputTag = static_cast<std::uint8_t>(92u + frame * 8u);
        ComPtr<ID3D12Resource> color = makeColor(gpu, inputTag);
        OutputResource output = makeOutput(gpu, frame);
        NVSDK_NGX_Parameter* params = allocateParams(api);
        ngx_fixture_set_frame_flags74(
            params, color.Get(), output.resource.Get(), depth, motion, 0);

        Recording recording = gpu.makeRecording();
        transition(recording.list.Get(), output.resource.Get(),
                   D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        checkNgxResult(
            api.evaluate(recording.list.Get(), feature, params, nullptr),
            ngx_fixture_result_success(), "NVSDK_NGX_D3D12_EvaluateFeature(flags74)");

        // The frontend must have snapshotted every resource/scalar before return.
        resetAndDestroyParams(api, params);
        transition(recording.list.Get(), output.resource.Get(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "NGX continuous Close");
        gpu.executeAndWait(recording.list.Get());

        OutputStats stats = validateOutput(output, inputTag, "ngx_continuous", frame);
        const double inputMean = static_cast<double>(inputTag) / 255.0;
        std::printf(
            "OWNED_D3D12_NGX_FRAME_PASS frame=%u caller_reset=0 expected_effective_reset=%u "
            "input_tag=%u input_mean=%.9g output_tag=%u output_mean=%.9g "
            "sameframe_delta=%.9g outside_sentinel=1 params_destroyed_before_execute=1 "
            "raw_exposure_zero_normalized=1\n",
            frame, frame == 0 ? 1u : 0u, static_cast<unsigned>(inputTag),
            inputMean, stats.outputTag, stats.mean, std::fabs(stats.mean - inputMean));
    }

    checkNgxResult(api.release(feature), ngx_fixture_result_success(),
                   "NVSDK_NGX_D3D12_ReleaseFeature(continuous)");
    std::puts("OWNED_D3D12_NGX_CASE_PASS case=continuous_8_frames");
}

void runReleaseBeforeExecuteReplay(const NgxApi& api, Gpu& gpu,
                                   ID3D12Resource* depth,
                                   ID3D12Resource* motion) {
    constexpr std::uint8_t inputTag = 116;
    NVSDK_NGX_Handle* feature = createFeature(api, gpu);
    ComPtr<ID3D12Resource> color = makeColor(gpu, inputTag);
    OutputResource output = makeOutput(gpu, 1000);
    NVSDK_NGX_Parameter* params = allocateParams(api);
    ngx_fixture_set_frame_flags74(
        params, color.Get(), output.resource.Get(), depth, motion, 0);

    Recording recording = gpu.makeRecording();
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    checkNgxResult(
        api.evaluate(recording.list.Get(), feature, params, nullptr),
        ngx_fixture_result_success(), "release-before-execute EvaluateFeature");
    resetAndDestroyParams(api, params);
    transition(recording.list.Get(), output.resource.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "release-before-execute Close");

    checkNgxResult(api.release(feature), ngx_fixture_result_success(),
                   "ReleaseFeature before Execute");
    std::puts(
        "OWNED_D3D12_NGX_CASE case=release_before_execute "
        "params_reset_destroyed=1 feature_released=1 caller_reset=0");

    gpu.executeAndWait(recording.list.Get());
    OutputStats first =
        validateOutput(output, inputTag, "ngx_release_before_execute", 0);
    std::printf(
        "OWNED_D3D12_NGX_CASE_REPLAY case=release_before_execute execution=1 "
        "expected_effective_reset=1 input_tag=%u output_tag=%u mean=%.9g\n",
        static_cast<unsigned>(inputTag), first.outputTag, first.mean);

    gpu.executeAndWait(recording.list.Get());
    OutputStats second =
        validateOutput(output, inputTag, "ngx_release_before_execute", 1);
    std::printf(
        "OWNED_D3D12_NGX_CASE_REPLAY case=release_before_execute execution=2 "
        "expected_effective_reset=0 input_tag=%u output_tag=%u mean=%.9g\n",
        static_cast<unsigned>(inputTag), second.outputTag, second.mean);

    NVSDK_NGX_Parameter* invalidParams = allocateParams(api);
    ngx_fixture_set_frame_flags74(
        invalidParams, color.Get(), output.resource.Get(), depth, motion, 0);
    Recording open = gpu.makeRecording();
    checkNgxResult(
        api.evaluate(open.list.Get(), feature, invalidParams, nullptr),
        ngx_fixture_result_feature_not_found(), "Evaluate after ReleaseFeature");
    check(open.list->Close(), "feature-not-found Close");
    resetAndDestroyParams(api, invalidParams);
    std::printf(
        "OWNED_D3D12_NGX_CASE_PASS case=release_before_execute_replay "
        "feature_not_found_after_release=0x%08x\n",
        ngx_fixture_result_feature_not_found());
}

int runNgxFixture() {
    printModulePath(L"d3d12.dll", "d3d12");
    requireOwnedBackend();
    NgxApi api;

    // Fresh process must not accept parameter allocation before D3D12 Init.
    NVSDK_NGX_Parameter* beforeInit = nullptr;
    checkNgxResult(api.allocate(&beforeInit), ngx_fixture_result_not_initialized(),
                   "AllocateParameters before Init");
    if (beforeInit)
        fail("AllocateParameters before Init returned object");
    std::printf("OWNED_D3D12_NGX_CASE_PASS case=preinit_allocate result=0x%08x\n",
                ngx_fixture_result_not_initialized());

    Gpu gpu;
    wchar_t dataPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, dataPath))
        fail("GetTempPathW", HRESULT_FROM_WIN32(GetLastError()));
    checkNgxResult(
        api.init(0x594141474c4e4758ull, dataPath, gpu.device.Get(), nullptr,
                 ngx_fixture_sdk_version()),
        ngx_fixture_result_success(), "NVSDK_NGX_D3D12_Init");
    std::printf("OWNED_D3D12_NGX_INIT sdk_version=0x%08x result=0x%08x\n",
                ngx_fixture_sdk_version(), ngx_fixture_result_success());

    ComPtr<ID3D12Resource> depth = makeDepth(gpu);
    ComPtr<ID3D12Resource> motion = makeMotion(gpu);
    std::puts("OWNED_D3D12_NGX_FORMAT role=color dxgi=28");
    std::puts("OWNED_D3D12_NGX_FORMAT role=output dxgi=28 backing=136 logical=128");
    std::puts("OWNED_D3D12_NGX_FORMAT role=motion dxgi=34 zeros=1");
    std::puts("OWNED_D3D12_NGX_FORMAT role=depth dxgi=20 dsv_clear=1 reverse_depth=1");

    runContinuousFrames(api, gpu, depth.Get(), motion.Get());
    runReleaseBeforeExecuteReplay(api, gpu, depth.Get(), motion.Get());

    checkNgxResult(api.shutdown1(gpu.device.Get()), ngx_fixture_result_success(),
                   "NVSDK_NGX_D3D12_Shutdown1");
    std::puts(
        "OWNED_D3D12_NGX_PASS init_allocate_create_evaluate_release=1 "
        "flags74=1 continuous_frames=8 params_destroy_before_execute=1 "
        "release_before_execute=1 closed_list_replay=1 owned_nvngx_marker=1");
    return 0;
}

} // namespace

int main() {
    return runNgxFixture();
}
