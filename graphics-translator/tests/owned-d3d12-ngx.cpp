// Reuse the already CPU-validated D3D12 resource/upload/readback helpers. The
// temporal feature itself is exercised only through nvngx.dll exports below.
#pragma push_macro("main")
#undef main
#define main ownedTemporalFixtureMain
#include "owned-d3d12-temporal.cpp"
#undef main
#pragma pop_macro("main")

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
void ngx_fixture_set_create_options(NVSDK_NGX_Parameter*, unsigned int, int);
void ngx_fixture_set_frame_offsets(NVSDK_NGX_Parameter*, unsigned int, unsigned int, unsigned int, unsigned int);
void ngx_fixture_set_frame_mask(NVSDK_NGX_Parameter*, ID3D12Resource*, unsigned int, unsigned int);
void ngx_fixture_set_frame_scalars(NVSDK_NGX_Parameter*, float, float);
void ngx_fixture_set_compatibility_scalars(NVSDK_NGX_Parameter*, float, float, float);
void ngx_fixture_set_exposure(NVSDK_NGX_Parameter*, ID3D12Resource*, float);
void ngx_fixture_set_motion_scalars(NVSDK_NGX_Parameter*, float, float, float, float);
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

void runRejectedOutputPreservation(const NgxApi& api, Gpu& gpu,
                                    NVSDK_NGX_Handle* feature, ID3D12Resource* depth, ID3D12Resource* motion) {
    auto color = makeColor(gpu, 112);
    auto output = makeOutput(gpu, 1900);
    auto recording = gpu.makeRecording();
    auto* params = allocateParams(api);
    ngx_fixture_set_frame_flags74(params, color.Get(), output.resource.Get(), depth, motion, 0);
    ngx_fixture_set_frame_offsets(params, 1, 0, 0, 0);
    checkNgxResult(api.evaluate(recording.list.Get(), feature, params, nullptr), ngx_fixture_result_invalid_parameter(),
                    "input color subrect exceeds backing");
    ngx_fixture_set_frame_flags74(params, color.Get(), output.resource.Get(), depth, motion, 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const float invalid[][3] = {{-1, 0, 0}, {nan, 0, 0}, {infinity, 0, 0},
                                {1, nan, 0}, {1, 0, -infinity}};
    for (const auto& values : invalid) {
        ngx_fixture_set_compatibility_scalars(params, values[0], values[1], values[2]);
        checkNgxResult(api.evaluate(recording.list.Get(), feature, params, nullptr),
                       ngx_fixture_result_invalid_parameter(), "invalid compatibility scalar");
    }
    resetAndDestroyParams(api, params);
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "NGX negative preservation Close");
    gpu.executeAndWait(recording.list.Get());
    D3D12_RANGE range{0, static_cast<SIZE_T>(output.readback.totalBytes)};
    uint8_t* bytes = nullptr;
    check(output.readback.buffer->Map(0, &range, reinterpret_cast<void**>(&bytes)), "NGX negative pixels Map");
    for (UINT y = 0; y < kOutputBacking; ++y)
        for (UINT x = 0; x < kOutputBacking; ++x) {
            const auto* p = bytes + output.readback.footprint.Offset + size_t(y) * output.readback.footprint.Footprint.RowPitch + x * 4;
            if (!std::equal(output.sentinel.begin(), output.sentinel.end(), p)) fail("NGX rejected evaluation modified output");
        }
    output.readback.buffer->Unmap(0, nullptr);
    std::puts("OWNED_D3D12_NGX_BEHAVIOR_PASS case=rejected_output_preserved invalid_input_bounds=1 invalid_compatibility_scalars=5 sentinel=1");
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
    runRejectedOutputPreservation(api, gpu, feature, depth, motion);

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
            "raw_pre_exposure_zero_normalized=1 exposure_scale_ignored=1\n",
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

void runOffsetBackingPairs(const NgxApi& api, Gpu& gpu) {
    std::vector<uint8_t> reference;
    for (UINT variant = 0; variant < 4; ++variant) {
        const UINT backing = variant ? 80 : 64;
        const UINT baseX = variant ? 7 : 0, baseY = variant ? 9 : 0;
        auto color = createTexture(gpu.device.Get(), backing, backing, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint8_t> pixels(size_t(backing) * backing * 4, 255);
        for (UINT y = 0; y < 64; ++y)
            for (UINT x = 0; x < 64; ++x) {
                auto* p = pixels.data() + (size_t(y + baseY) * backing + x + baseX) * 4;
                p[0] = x < 32 ? 64 : 160; p[1] = 96; p[2] = y < 32 ? 80 : 128; p[3] = 255;
            }
        uploadTexture(gpu, color.Get(), pixels.data(), backing * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto depth = createTexture(gpu.device.Get(), backing, backing, DXGI_FORMAT_R32_FLOAT,
                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<float> depths(size_t(backing) * backing, 0);
        for (UINT y = 0; y < 64; ++y)
            std::fill_n(depths.data() + size_t(y + baseY) * backing + baseX, 64, 0.75f);
        uploadTexture(gpu, depth.Get(), depths.data(), backing * sizeof(float), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const UINT motionBacking = variant == 2 ? 144 : backing;
        const UINT motionActive = variant == 2 ? 128 : 64;
        auto motion = createTexture(gpu.device.Get(), motionBacking, motionBacking, DXGI_FORMAT_R16G16_FLOAT,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint16_t> vectors(size_t(motionBacking) * motionBacking * 2, 0x3c00);
        for (UINT y = 0; y < motionActive; ++y)
            std::fill_n(vectors.data() + (size_t(y + baseY) * motionBacking + baseX) * 2, motionActive * 2, 0);
        uploadTexture(gpu, motion.Get(), vectors.data(), motionBacking * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto mask = createTexture(gpu.device.Get(), backing, backing, DXGI_FORMAT_R8_UNORM,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint8_t> masks(size_t(backing) * backing, 255);
        for (UINT y = 0; y < 64; ++y)
            std::fill_n(masks.data() + size_t(y + baseY) * backing + baseX, 64, 0);
        uploadTexture(gpu, mask.Get(), masks.data(), backing, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto* params = allocateParams(api);
        ngx_fixture_set_create_flags74(params, 64, 64, 128, 128);
        const UINT flags = variant == 2 ? (ngx_fixture_flags74() & ~2u) | 4u :
                           variant == 3 ? ngx_fixture_flags74() | 32u : ngx_fixture_flags74();
        ngx_fixture_set_create_options(params, flags, 1);
        auto recording = gpu.makeRecording();
        NVSDK_NGX_Handle* feature = nullptr;
        checkNgxResult(api.create(recording.list.Get(), ngx_fixture_feature_supersampling(), params, &feature),
                        ngx_fixture_result_success(), "offset pair create");
        auto output = makeOutput(gpu, 1600);
        ngx_fixture_set_frame_flags74(params, color.Get(), output.resource.Get(), depth.Get(), motion.Get(), 1);
        ngx_fixture_set_frame_offsets(params, baseX, baseY, variant ? 3 : 0, variant ? 5 : 0);
        ngx_fixture_set_frame_mask(params, mask.Get(), baseX, baseY);
        if (variant == 3) ngx_fixture_set_frame_scalars(params, 1.0f, 1.0f);
        transition(recording.list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        checkNgxResult(api.evaluate(recording.list.Get(), feature, params, nullptr), ngx_fixture_result_success(), "offset pair evaluate");
        resetAndDestroyParams(api, params);
        transition(recording.list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "offset pair Close");
        checkNgxResult(api.release(feature), ngx_fixture_result_success(), "offset feature release before submit");
        gpu.executeAndWait(recording.list.Get());
        D3D12_RANGE range{0, static_cast<SIZE_T>(output.readback.totalBytes)};
        uint8_t* bytes = nullptr;
        check(output.readback.buffer->Map(0, &range, reinterpret_cast<void**>(&bytes)), "offset pair Map");
        std::vector<uint8_t> image(128 * 128 * 4);
        const UINT ox = variant ? 3 : 0, oy = variant ? 5 : 0;
        for (UINT y = 0; y < kOutputBacking; ++y)
            for (UINT x = 0; x < kOutputBacking; ++x) {
                const auto* p = bytes + output.readback.footprint.Offset + size_t(y) * output.readback.footprint.Footprint.RowPitch + x * 4;
                if (x >= ox && x < ox + 128 && y >= oy && y < oy + 128)
                    std::copy_n(p, 4, image.data() + (size_t(y - oy) * 128 + x - ox) * 4);
                else if (!std::equal(output.sentinel.begin(), output.sentinel.end(), p))
                    fail("NGX offset output padding modified");
            }
        output.readback.buffer->Unmap(0, nullptr);
        if (!variant) reference = image;
        else if (variant < 3) {
            int maxDelta = 0;
            for (size_t i = 0; i < image.size(); ++i)
                maxDelta = std::max(maxDelta, std::abs(int(image[i]) - int(reference[i])));
            if (maxDelta > 1) fail("NGX poison-backed offsets differ from tight input");
            std::printf("OWNED_D3D12_NGX_BEHAVIOR_PASS case=offset_backing_pair max_byte_delta=%d sentinel=1 released_before_submit=1 highres_jittered=%u\n", maxDelta, variant == 2);
        } else {
            uint64_t rgbDifference = 0;
            for (size_t i = 0; i < image.size(); ++i) {
                if (i % 4 == 3 && image[i] != reference[i]) fail("NGX sharpening changed native-scaled alpha");
                if (i % 4 != 3) rgbDifference += std::abs(int(image[i]) - int(reference[i]));
            }
            if (!rgbDifference) fail("NGX enabled sharpening was a no-op");
            std::printf("OWNED_D3D12_NGX_BEHAVIOR_PASS case=sharpen_edge_alpha rgb_byte_delta_sum=%llu alpha_unchanged=1 sentinel=1\n",
                        static_cast<unsigned long long>(rgbDifference));
        }
    }
}

void runIgnoredCompatibilityPairs(const NgxApi& api, Gpu& gpu) {
    struct Case {
        const char* name;
        UINT flags;
        float exposureScale, offsetX, offsetY, preExposure;
        bool exposureTexture;
    };
    const Case cases[] = {
        {"exposure_scale_auto", 75, 2.0f, 0, 0, 1, true},
        {"exposure_scale_texture", 11, 2.0f, 0, 0, 2, true},
        {"exposure_scale_no_texture", 11, 0.375f, 0, 0, 2, false},
        {"mv_value_offsets_lowres", 75, 1, -1.25f, 2.5f, 1, false},
        {"mv_value_offsets_highres_jittered", 77, 1, -0.375f, 0.625f, 1, false},
    };
    for (const auto& item : cases) {
        constexpr UINT backing = 80, baseX = 7, baseY = 9, outputX = 3, outputY = 5;
        auto color = createTexture(gpu.device.Get(), backing, backing, DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint8_t> pixels(size_t(backing) * backing * 4, 255);
        const auto writeColor = [&](UINT frame) {
            for (UINT y = 0; y < 64; ++y)
                for (UINT x = 0; x < 64; ++x) {
                    auto* p = pixels.data() + (size_t(y + baseY) * backing + x + baseX) * 4;
                    p[0] = x < 32 + frame * 4 ? 64 : 160;
                    p[1] = 96; p[2] = y < 32 - frame * 3 ? 80 : 144; p[3] = 255;
                }
            uploadTexture(gpu, color.Get(), pixels.data(), backing * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        writeColor(0);
        auto depth = makeDepth(gpu, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
                              DXGI_FORMAT_D32_FLOAT_S8X24_UINT, backing, backing);
        const bool highres = !(item.flags & 2);
        const UINT motionBacking = highres ? 144 : backing;
        const UINT motionActive = highres ? 128 : 64;
        auto motion = createTexture(gpu.device.Get(), motionBacking, motionBacking, DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        std::vector<uint16_t> vectors(size_t(motionBacking) * motionBacking * 2, 0x3c00);
        const auto writeMotion = [&](UINT frame) {
            for (UINT y = 0; y < motionActive; ++y)
                for (UINT x = 0; x < motionActive; ++x) {
                    auto* p = vectors.data() + (size_t(y + baseY) * motionBacking + x + baseX) * 2;
                    p[0] = frame & 1 ? 0x3800 : 0x3400; // 0.5 / 0.25
                    p[1] = frame & 1 ? 0xb400 : 0xb800; // -0.25 / -0.5
                }
            uploadTexture(gpu, motion.Get(), vectors.data(), motionBacking * 4,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        writeMotion(0);
        ComPtr<ID3D12Resource> exposure;
        ComPtr<ID3D12DescriptorHeap> exposureHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE exposureRtv{};
        if (item.exposureTexture) {
            exposure = createTexture(gpu.device.Get(), 1, 1, DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 1;
            check(gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&exposureHeap)), "compatibility exposure heap");
            exposureRtv = exposureHeap->GetCPUDescriptorHandleForHeapStart();
            gpu.device->CreateRenderTargetView(exposure.Get(), nullptr, exposureRtv);
            auto initialize = gpu.makeRecording();
            const float initial[] = {1, 0, 0, 1};
            initialize.list->ClearRenderTargetView(exposureRtv, initial, 0, nullptr);
            transition(initialize.list.Get(), exposure.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            check(initialize.list->Close(), "compatibility exposure initialize");
            gpu.executeAndWait(initialize.list.Get());
        }
        std::array<OutputResource, 2> outputs;
        std::array<Recording, 2> recordings;
        for (UINT side = 0; side < 2; ++side) {
            outputs[side] = makeOutput(gpu, 1700 + side);
            recordings[side] = gpu.makeRecording();
            auto* params = allocateParams(api);
            ngx_fixture_set_create_flags74(params, 64, 64, 128, 128);
            ngx_fixture_set_create_options(params, item.flags, 1);
            NVSDK_NGX_Handle* feature = nullptr;
            checkNgxResult(api.create(recordings[side].list.Get(), ngx_fixture_feature_supersampling(), params, &feature),
                           ngx_fixture_result_success(), "compatibility create");
            ngx_fixture_set_frame_flags74(params, color.Get(), outputs[side].resource.Get(), depth.Get(), motion.Get(), 0);
            ngx_fixture_set_frame_offsets(params, baseX, baseY, outputX, outputY);
            ngx_fixture_set_exposure(params, exposure.Get(), item.preExposure);
            ngx_fixture_set_motion_scalars(params, -2.0f, 0.5f, 0.125f, -0.25f);
            ngx_fixture_set_compatibility_scalars(params, side ? item.exposureScale : 1.0f,
                                                 side ? item.offsetX : 0.0f, side ? item.offsetY : 0.0f);
            transition(recordings[side].list.Get(), outputs[side].resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            checkNgxResult(api.evaluate(recordings[side].list.Get(), feature, params, nullptr),
                           ngx_fixture_result_success(), item.name);
            // Caller maps become invalid before submission; replay must never revisit them.
            ngx_fixture_set_compatibility_scalars(params, -1, std::numeric_limits<float>::quiet_NaN(), 0);
            resetAndDestroyParams(api, params);
            checkNgxResult(api.release(feature), ngx_fixture_result_success(), "compatibility release before submit");
            transition(recordings[side].list.Get(), outputs[side].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            copyTextureToReadback(recordings[side].list.Get(), outputs[side].resource.Get(), outputs[side].readback);
            check(recordings[side].list->Close(), "compatibility Close");
        }
        for (UINT replay = 0; replay < 3; ++replay) {
            auto producer = gpu.makeRecording();
            if (replay) {
                transition(producer.list.Get(), color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                transition(producer.list.Get(), motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            if (exposure) {
                const float values[] = {0.3333f, 1.234567f, 3.14159f};
                const float clear[] = {values[replay], 0, 0, 1};
                transition(producer.list.Get(), exposure.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                producer.list->ClearRenderTargetView(exposureRtv, clear, 0, nullptr);
                transition(producer.list.Get(), exposure.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            check(producer.list->Close(), "compatibility live producer Close");
            gpu.executeAndWait(producer.list.Get());
            if (replay) { writeColor(replay); writeMotion(replay); }
            std::vector<uint8_t> reference;
            for (UINT side = 0; side < 2; ++side) {
                gpu.executeAndWait(recordings[side].list.Get());
                const auto& output = outputs[side];
                uint8_t* bytes = nullptr;
                check(output.readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&bytes)), "compatibility output Map");
                std::vector<uint8_t> image(128 * 128 * 4);
                for (UINT y = 0; y < kOutputBacking; ++y)
                    for (UINT x = 0; x < kOutputBacking; ++x) {
                        const auto* p = bytes + output.readback.footprint.Offset + size_t(y) * output.readback.footprint.Footprint.RowPitch + x * 4;
                        if (x >= outputX && x < outputX + 128 && y >= outputY && y < outputY + 128)
                            std::copy_n(p, 4, image.data() + (size_t(y - outputY) * 128 + x - outputX) * 4);
                        else if (!std::equal(output.sentinel.begin(), output.sentinel.end(), p)) fail("compatibility output sentinel");
                    }
                output.readback.buffer->Unmap(0, nullptr);
                if (!side) {
                    const auto* topLeft = image.data() + (16 * 128 + 16) * 4;
                    const auto* bottomRight = image.data() + (112 * 128 + 112) * 4;
                    if (bottomRight[0] <= topLeft[0] + 16 || bottomRight[2] <= topLeft[2] + 16)
                        fail("compatibility GPU output did not reconstruct the input contrasts");
                    reference = std::move(image);
                } else {
                    int maxDelta = 0;
                    for (size_t i = 0; i < image.size(); ++i)
                        maxDelta = std::max(maxDelta, std::abs(int(image[i]) - int(reference[i])));
                    if (maxDelta > 1) fail("ignored NGX compatibility scalars changed GPU output");
                    std::printf("OWNED_D3D12_NGX_BEHAVIOR_PASS case=%s replay=%u exposure_scale=%.9g mv_offset_x=%.9g mv_offset_y=%.9g max_byte_delta=%d sentinel=1 live_inputs=1 params_destroyed=1 released_before_submit=1 highres_jittered=%u\n",
                                item.name, replay, item.exposureScale, item.offsetX, item.offsetY, maxDelta, highres);
                }
            }
        }
    }
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

    runIgnoredCompatibilityPairs(api, gpu);
    runOffsetBackingPairs(api, gpu);
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
