// Reuse upload, fence and sentinel readback helpers. Upscaling is invoked only
// through the official C ABI DLL exports, never through the private COM API.
#define main ownedTemporalFixtureMain
#include "owned-d3d12-temporal.cpp"
#undef main

#include "upscalers/include/ffx_upscale.h"
#include "api/include/dx12/ffx_api_dx12.h"
#include <cstdlib>

namespace {
constexpr uint64_t kFsrProvider = 0x4d46580000000001ull;
constexpr uint32_t kFsrFlags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
    FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
static_assert(FFX_UPSCALER_VERSION == 0x01000003);
static_assert(uint32_t(FFX_API_RESOURCE_STATE_COPY_SRC) != uint32_t(D3D12_RESOURCE_STATE_COPY_SOURCE));

void fsrResult(ffxReturnCode_t actual, ffxReturnCode_t expected, const char* name) {
    if (actual != expected) {
        std::fprintf(stderr, "OWNED_D3D12_FSR_FAIL case=%s expected=%u actual=%u\n",
                     name, expected, actual);
        ExitProcess(1);
    }
    std::printf("OWNED_D3D12_FSR_CASE_PASS case=%s result=%u\n", name, actual);
}

struct FsrApi {
    HMODULE module;
    PfnFfxConfigure configure;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    PfnFfxQuery query;

    FsrApi() {
        module = LoadLibraryW(L"C:\\windows\\system32\\amd_fidelityfx_upscaler_dx12.dll");
        if (!module) {
            std::fprintf(stderr, "OWNED_D3D12_FSR_UNAVAILABLE stage=LoadLibrary error=%lu gpu_attempted=0\n",
                         GetLastError());
            ExitProcess(2);
        }
        const char* names[] = {"ffxConfigure", "ffxCreateContext", "ffxDestroyContext",
                               "ffxDispatch", "ffxQuery"};
        FARPROC functions[5]{};
        for (WORD i = 0; i < 5; ++i) {
            functions[i] = GetProcAddress(module, names[i]);
            if (!functions[i] || functions[i] != GetProcAddress(module, MAKEINTRESOURCEA(i + 1)))
                fail("FSR names/ordinals mismatch");
            std::printf("OWNED_D3D12_FSR_EXPORT name=%s ordinal=%u\n", names[i], i + 1);
        }
        configure = reinterpret_cast<PfnFfxConfigure>(functions[0]);
        create = reinterpret_cast<PfnFfxCreateContext>(functions[1]);
        destroy = reinterpret_cast<PfnFfxDestroyContext>(functions[2]);
        dispatch = reinterpret_cast<PfnFfxDispatch>(functions[3]);
        query = reinterpret_cast<PfnFfxQuery>(functions[4]);
        printModulePath(L"amd_fidelityfx_upscaler_dx12.dll", "fsr");
    }
    ~FsrApi() { FreeLibrary(module); }
};

struct AllocationStats { UINT allocated = 0, freed = 0; bool reject = false; };
void* fsrAllocate(void* user, uint64_t size) {
    auto& stats = *static_cast<AllocationStats*>(user);
    if (stats.reject || size > SIZE_MAX)
        return nullptr;
    void* memory = std::malloc(static_cast<size_t>(size));
    if (memory)
        ++stats.allocated;
    return memory;
}
void fsrDeallocate(void* user, void* memory) {
    if (memory) {
        ++static_cast<AllocationStats*>(user)->freed;
        std::free(memory);
    }
}

// Official create descriptor pointers stay live until context destruction.
struct FsrCreation {
    ffxCreateBackendDX12Desc backend{};
    ffxCreateContextDescUpscaleVersion version{};
    ffxOverrideVersion provider{};
    ffxCreateContextDescUpscale desc{};
    explicit FsrCreation(ID3D12Device* device) {
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        version.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &backend.header};
        version.version = FFX_UPSCALER_VERSION;
        provider.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &version.header};
        provider.versionId = kFsrProvider;
        desc.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &provider.header};
        desc.flags = kFsrFlags;
        desc.maxRenderSize = {kInputWidth, kInputHeight};
        desc.maxUpscaleSize = {kOutputWidth, kOutputHeight};
    }
};

struct FsrProxyRefs { LONG added = 0, released = 0, destroyed = 0; };

// Interception layers keep their own base-device identity, but expose the
// underlying implementation for extensions and return its unwrapped children.
class FsrDeviceProxy final : public ID3D12Device {
    ComPtr<ID3D12Device> device;
    LONG refs = 1;
    FsrProxyRefs& counts;
public:
    FsrDeviceProxy(ID3D12Device* underlying, FsrProxyRefs& counts) : device(underlying), counts(counts) {}
    ~FsrDeviceProxy() { ++counts.destroyed; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12Device)) {
            *out = static_cast<ID3D12Device*>(this);
            AddRef();
            return S_OK;
        }
        return device->QueryInterface(iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        InterlockedIncrement(&counts.added);
        return InterlockedIncrement(&refs);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        InterlockedIncrement(&counts.released);
        const ULONG remaining = InterlockedDecrement(&refs);
        if (!remaining)
            delete this;
        return remaining;
    }

#define FSR_FORWARD(type, name, parameters, arguments) \
    type STDMETHODCALLTYPE name parameters override { return device->name arguments; }
    FSR_FORWARD(HRESULT, GetPrivateData, (REFGUID guid, UINT* size, void* data), (guid, size, data))
    FSR_FORWARD(HRESULT, SetPrivateData, (REFGUID guid, UINT size, const void* data), (guid, size, data))
    FSR_FORWARD(HRESULT, SetPrivateDataInterface, (REFGUID guid, const IUnknown* data), (guid, data))
    FSR_FORWARD(HRESULT, SetName, (const WCHAR* name), (name))
    FSR_FORWARD(UINT, GetNodeCount, (), ())
    FSR_FORWARD(HRESULT, CreateCommandQueue, (const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** out), (desc, iid, out))
    FSR_FORWARD(HRESULT, CreateCommandAllocator, (D3D12_COMMAND_LIST_TYPE type, REFIID iid, void** out), (type, iid, out))
    FSR_FORWARD(HRESULT, CreateGraphicsPipelineState, (const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** out), (desc, iid, out))
    FSR_FORWARD(HRESULT, CreateComputePipelineState, (const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** out), (desc, iid, out))
    FSR_FORWARD(HRESULT, CreateCommandList, (UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator, ID3D12PipelineState* state, REFIID iid, void** out), (node, type, allocator, state, iid, out))
    FSR_FORWARD(HRESULT, CheckFeatureSupport, (D3D12_FEATURE feature, void* data, UINT size), (feature, data, size))
    FSR_FORWARD(HRESULT, CreateDescriptorHeap, (const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** out), (desc, iid, out))
    FSR_FORWARD(UINT, GetDescriptorHandleIncrementSize, (D3D12_DESCRIPTOR_HEAP_TYPE type), (type))
    FSR_FORWARD(HRESULT, CreateRootSignature, (UINT node, const void* bytes, SIZE_T size, REFIID iid, void** out), (node, bytes, size, iid, out))
    FSR_FORWARD(void, CreateConstantBufferView, (const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle), (desc, handle))
    FSR_FORWARD(void, CreateShaderResourceView, (ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle), (resource, desc, handle))
    FSR_FORWARD(void, CreateUnorderedAccessView, (ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle), (resource, counter, desc, handle))
    FSR_FORWARD(void, CreateRenderTargetView, (ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle), (resource, desc, handle))
    FSR_FORWARD(void, CreateDepthStencilView, (ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle), (resource, desc, handle))
    FSR_FORWARD(void, CreateSampler, (const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle), (desc, handle))
    FSR_FORWARD(void, CopyDescriptors, (UINT dstCount, const D3D12_CPU_DESCRIPTOR_HANDLE* dst, const UINT* dstSizes, UINT srcCount, const D3D12_CPU_DESCRIPTOR_HANDLE* src, const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type), (dstCount, dst, dstSizes, srcCount, src, srcSizes, type))
    FSR_FORWARD(void, CopyDescriptorsSimple, (UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type), (count, dst, src, type))
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    FSR_FORWARD(D3D12_RESOURCE_ALLOCATION_INFO*, GetResourceAllocationInfo, (D3D12_RESOURCE_ALLOCATION_INFO* out, UINT mask, UINT count, const D3D12_RESOURCE_DESC* desc), (out, mask, count, desc))
    FSR_FORWARD(D3D12_HEAP_PROPERTIES*, GetCustomHeapProperties, (D3D12_HEAP_PROPERTIES* out, UINT node, D3D12_HEAP_TYPE type), (out, node, type))
    FSR_FORWARD(LUID*, GetAdapterLuid, (LUID* out), (out))
#else
    FSR_FORWARD(D3D12_RESOURCE_ALLOCATION_INFO, GetResourceAllocationInfo, (UINT mask, UINT count, const D3D12_RESOURCE_DESC* desc), (mask, count, desc))
    FSR_FORWARD(D3D12_HEAP_PROPERTIES, GetCustomHeapProperties, (UINT node, D3D12_HEAP_TYPE type), (node, type))
    FSR_FORWARD(LUID, GetAdapterLuid, (), ())
#endif
    FSR_FORWARD(HRESULT, CreateCommittedResource, (const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out), (heap, flags, desc, state, clear, iid, out))
    FSR_FORWARD(HRESULT, CreateHeap, (const D3D12_HEAP_DESC* desc, REFIID iid, void** out), (desc, iid, out))
    FSR_FORWARD(HRESULT, CreatePlacedResource, (ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out), (heap, offset, desc, state, clear, iid, out))
    FSR_FORWARD(HRESULT, CreateReservedResource, (const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out), (desc, state, clear, iid, out))
    FSR_FORWARD(HRESULT, CreateSharedHandle, (ID3D12DeviceChild* object, const SECURITY_ATTRIBUTES* attributes, DWORD access, const WCHAR* name, HANDLE* handle), (object, attributes, access, name, handle))
    FSR_FORWARD(HRESULT, OpenSharedHandle, (HANDLE handle, REFIID iid, void** out), (handle, iid, out))
    FSR_FORWARD(HRESULT, OpenSharedHandleByName, (const WCHAR* name, DWORD access, HANDLE* handle), (name, access, handle))
    FSR_FORWARD(HRESULT, MakeResident, (UINT count, ID3D12Pageable* const* objects), (count, objects))
    FSR_FORWARD(HRESULT, Evict, (UINT count, ID3D12Pageable* const* objects), (count, objects))
    FSR_FORWARD(HRESULT, CreateFence, (UINT64 value, D3D12_FENCE_FLAGS flags, REFIID iid, void** out), (value, flags, iid, out))
    FSR_FORWARD(HRESULT, GetDeviceRemovedReason, (), ())
    FSR_FORWARD(void, GetCopyableFootprints, (const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* rowSize, UINT64* size), (desc, first, count, base, layouts, rows, rowSize, size))
    FSR_FORWARD(HRESULT, CreateQueryHeap, (const D3D12_QUERY_HEAP_DESC* desc, REFIID iid, void** out), (desc, iid, out))
    FSR_FORWARD(HRESULT, SetStablePowerState, (WINBOOL enable), (enable))
    FSR_FORWARD(HRESULT, CreateCommandSignature, (const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* root, REFIID iid, void** out), (desc, root, iid, out))
    FSR_FORWARD(void, GetResourceTiling, (ID3D12Resource* resource, UINT* count, D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape, UINT* subCount, UINT first, D3D12_SUBRESOURCE_TILING* tilings), (resource, count, packed, shape, subCount, first, tilings))
#undef FSR_FORWARD
};

ffxDispatchDescUpscale fsrFrame(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
                                ID3D12Resource* depth, ID3D12Resource* motion,
                                ID3D12Resource* output) {
    ffxDispatchDescUpscale desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = list;
    desc.color = ffxApiGetResourceDX12(color);
    desc.depth = ffxApiGetResourceDX12(depth);
    desc.motionVectors = ffxApiGetResourceDX12(motion);
    desc.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_COPY_SRC);
    desc.motionVectorScale = {64.0f, 64.0f};
    desc.renderSize = {kInputWidth, kInputHeight};
    desc.frameTimeDelta = 16.0f;
    desc.preExposure = 1.0f;
    desc.cameraNear = 0.1f;
    desc.cameraFar = 1000.0f;
    desc.cameraFovAngleVertical = 1.0f;
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.reset = true;
    return desc;
}

void fsrQueries(const FsrApi& api, Gpu& gpu, ffxContext* context) {
    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = gpu.device.Get();
    versions.outputCount = &count;
    fsrResult(api.query(nullptr, &versions.header), FFX_API_RETURN_OK, "provider_count");
    if (count != 1)
        fail("FSR provider count");
    std::array<uint64_t, 3> ids{0xfeed, 0, 0xbeef};
    std::array<const char*, 3> names{nullptr, nullptr, nullptr};
    versions.versionIds = &ids[1];
    versions.versionNames = &names[1];
    count = 0;
    fsrResult(api.query(nullptr, &versions.header), FFX_API_RETURN_OK, "provider_zero_capacity");
    if (count != 1 || ids[1] || names[1])
        fail("FSR zero capacity overwrote output");
    count = 1;
    fsrResult(api.query(nullptr, &versions.header), FFX_API_RETURN_OK, "provider_bounded_array");
    if (count != 1 || ids[0] != 0xfeed || ids[2] != 0xbeef || ids[1] != kFsrProvider || names[0] || names[2] ||
        !names[1] || !std::strstr(names[1], "MetalFX"))
        fail("FSR honest provider identity/bounds");
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    fsrResult(api.query(context, &provider.header), FFX_API_RETURN_OK, "context_provider");
    if (provider.versionId != ids[1] || !provider.versionName ||
        !std::strstr(provider.versionName, "MetalFX"))
        fail("FSR context provider mismatch");
    ffxQueryDescUpscaleGetResourceRequirements resources{};
    resources.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    fsrResult(api.query(context, &resources.header), FFX_API_RETURN_OK, "resource_requirements");
    constexpr uint64_t required = FFX_API_QUERY_RESOURCE_INPUT_COLOR |
        FFX_API_QUERY_RESOURCE_INPUT_DEPTH | FFX_API_QUERY_RESOURCE_INPUT_MV;
    if (resources.required_resources != required ||
        (resources.optional_resources & ~uint64_t(FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE |
             FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK | FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION)))
        fail("FSR advertised unsupported resource consumption");
    std::printf("OWNED_D3D12_FSR_PROVIDER id=%llx name=%s api=%08x required=%llx optional=%llx\n",
                static_cast<unsigned long long>(provider.versionId), provider.versionName,
                FFX_UPSCALER_VERSION, static_cast<unsigned long long>(resources.required_resources),
                static_cast<unsigned long long>(resources.optional_resources));
    constexpr float ratios[] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    for (uint32_t quality = 0; quality < 5; ++quality) {
        float ratio = -1;
        ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode ratioQuery{};
        ratioQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE;
        ratioQuery.qualityMode = quality;
        ratioQuery.pOutUpscaleRatio = &ratio;
        fsrResult(api.query(nullptr, &ratioQuery.header), FFX_API_RETURN_OK, "quality_ratio");
        if (std::fabs(ratio - ratios[quality]) > 1e-6f)
            fail("FSR quality ratio");
        uint32_t width = 0, height = 0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode resolution{};
        resolution.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        resolution.displayWidth = 1920;
        resolution.displayHeight = 1080;
        resolution.qualityMode = quality;
        resolution.pOutRenderWidth = &width;
        resolution.pOutRenderHeight = &height;
        fsrResult(api.query(nullptr, &resolution.header), FFX_API_RETURN_OK, "quality_dimensions");
        if (width != uint32_t(1920.0f / ratios[quality]) || height != uint32_t(1080.0f / ratios[quality]))
            fail("FSR quality dimensions");
        std::printf("OWNED_D3D12_FSR_QUALITY mode=%u ratio=%.9g render=%ux%u\n", quality, ratio, width, height);
    }
    float invalidRatio = -1;
    ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode invalidQuality{};
    invalidQuality.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE;
    invalidQuality.qualityMode = 5;
    invalidQuality.pOutUpscaleRatio = &invalidRatio;
    fsrResult(api.query(nullptr, &invalidQuality.header), FFX_API_RETURN_ERROR_PARAMETER, "quality_out_of_bounds");
    if (invalidRatio != -1)
        fail("FSR failed query mutated output");
    int32_t phases = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount phase{};
    phase.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phase.renderWidth = 64;
    phase.displayWidth = 128;
    phase.pOutPhaseCount = &phases;
    fsrResult(api.query(nullptr, &phase.header), FFX_API_RETURN_OK, "jitter_phases");
    if (phases != 32)
        fail("FSR jitter phase count");
    float x = 9, y = 9;
    ffxQueryDescUpscaleGetJitterOffset jitter{};
    jitter.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jitter.phaseCount = phases;
    jitter.pOutX = &x;
    jitter.pOutY = &y;
    fsrResult(api.query(nullptr, &jitter.header), FFX_API_RETURN_OK, "jitter_halton");
    if (std::fabs(x) > 1e-6f || std::fabs(y + 1.0f / 6.0f) > 1e-6f)
        fail("FSR jitter Halton values");
    jitter.phaseCount = 0;
    fsrResult(api.query(nullptr, &jitter.header), FFX_API_RETURN_ERROR_PARAMETER, "jitter_zero_phases");
    phase.renderWidth = 0;
    fsrResult(api.query(nullptr, &phase.header), FFX_API_RETURN_ERROR_PARAMETER, "jitter_zero_width");
    ffxQueryDescUpscaleGetGPUMemoryUsage memory{};
    FfxApiEffectMemoryUsage usage{};
    memory.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    memory.gpuMemoryUsageUpscaler = &usage;
    fsrResult(api.query(context, &memory.header), FFX_API_RETURN_NO_PROVIDER, "memory_unavailable_truthful");
    fsrResult(api.query(context, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "null_query");
    versions.outputCount = nullptr;
    fsrResult(api.query(nullptr, &versions.header), FFX_API_RETURN_ERROR_PARAMETER, "null_query_count");
}

void fsrErrorCases(const FsrApi& api, Gpu& gpu, FsrCreation& creation, ffxContext* context,
                   const ffxAllocationCallbacks& callbacks, ID3D12Resource* depth,
                   ID3D12Resource* motion) {
    ffxContext unexpected = nullptr;
    fsrResult(api.create(nullptr, &creation.desc.header, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "null_context_out");
    fsrResult(api.create(&unexpected, nullptr, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "null_create_desc");
    const auto savedSize = creation.desc.maxRenderSize;
    creation.desc.maxRenderSize.width = 0;
    fsrResult(api.create(&unexpected, &creation.desc.header, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "create_zero_size");
    creation.desc.maxRenderSize = savedSize;
    uint32_t boundaryWidth = 0, boundaryHeight = 0;
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode boundary{};
    boundary.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    boundary.displayWidth = 2560;
    boundary.displayHeight = 1440;
    boundary.qualityMode = FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
    boundary.pOutRenderWidth = &boundaryWidth;
    boundary.pOutRenderHeight = &boundaryHeight;
    fsrResult(api.query(nullptr, &boundary.header), FFX_API_RETURN_OK, "ultra_performance_floor_query");
    if (boundaryWidth != 853 || boundaryHeight != 480)
        fail("FSR ultra-performance official floor query");
    const auto savedOutput = creation.desc.maxUpscaleSize;
    creation.desc.maxRenderSize = {boundaryWidth, boundaryHeight};
    creation.desc.maxUpscaleSize = {2560, 1440};
    fsrResult(api.create(&unexpected, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "floor_uses_device_scale_cap");
    fsrResult(api.destroy(&unexpected, nullptr), FFX_API_RETURN_OK, "floor_cap_destroy");
    creation.desc.maxRenderSize = savedSize;
    creation.desc.maxUpscaleSize = savedOutput;
    std::puts("OWNED_D3D12_FSR_RATIO_BOUNDARY query_render=853x480 output=2560x1440 create_result=0 cap=device");
    creation.version.version = FFX_UPSCALER_VERSION + 1;
    fsrResult(api.create(&unexpected, &creation.desc.header, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "foreign_api_version");
    creation.version.version = FFX_UPSCALER_VERSION;
    creation.desc.flags = kFsrFlags | (1u << 31);
    fsrResult(api.create(&unexpected, &creation.desc.header, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "unknown_create_option");
    creation.desc.flags = kFsrFlags;
    creation.provider.versionId = 123;
    fsrResult(api.create(&unexpected, &creation.desc.header, nullptr), FFX_API_RETURN_NO_PROVIDER, "foreign_provider");
    creation.provider.versionId = kFsrProvider;
    AllocationStats denied{0, 0, true};
    ffxAllocationCallbacks deniedCallbacks{&denied, fsrAllocate, fsrDeallocate};
    fsrResult(api.create(&unexpected, &creation.desc.header, &deniedCallbacks), FFX_API_RETURN_ERROR_MEMORY, "allocator_failure");
    if (unexpected)
        fail("FSR failed create returned context");
    fsrResult(api.destroy(nullptr, &callbacks), FFX_API_RETURN_ERROR_PARAMETER, "null_destroy");
    fsrResult(api.destroy(context, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "mismatched_destroy_allocator");
    if (!*context)
        fail("FSR rejected destroy consumed context");
    fsrResult(api.dispatch(nullptr, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "null_dispatch");
    fsrResult(api.configure(context, nullptr), FFX_API_RETURN_ERROR_PARAMETER, "null_configure");
    ffxConfigureDescUpscaleKeyValue config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
    fsrResult(api.configure(context, &config.header), FFX_API_RETURN_NO_PROVIDER, "unsupported_configure_truthful");

    auto color = makeColor(gpu, 100);
    auto output = makeOutput(gpu, 777);
    auto recording = gpu.makeRecording();
    const auto valid = fsrFrame(recording.list.Get(), color.Get(), depth, motion, output.resource.Get());
    auto reject = [&](ffxDispatchDescUpscale desc, ffxReturnCode_t result, const char* name) {
        fsrResult(api.dispatch(context, &desc.header), result, name);
    };
    auto bad = valid;
    bad.commandList = nullptr;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "null_command_list");
    bad = valid; bad.color.resource = nullptr;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "missing_color");
    bad = valid; bad.renderSize.width = 0;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "render_zero");
    bad = valid; bad.renderSize.width = 65;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "render_above_max");
    bad = valid; bad.upscaleSize = {0, 128};
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "mixed_zero_output_extent");
    bad = valid; bad.upscaleSize = {129, 128};
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "output_above_max");
    bad = valid; bad.jitterOffset.x = std::numeric_limits<float>::quiet_NaN();
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "nan_jitter");
    bad = valid; bad.cameraFar = std::numeric_limits<float>::infinity();
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "infinite_far_without_infinite_depth");
    bad = valid; bad.cameraFar = std::numeric_limits<float>::quiet_NaN();
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "nan_far");
    bad = valid; bad.viewSpaceToMetersFactor = std::numeric_limits<float>::quiet_NaN();
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "nan_world_scale");
    bad = valid; bad.preExposure = 0;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "zero_pre_exposure");
    bad = valid; bad.color.description.width++;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "resource_metadata_mismatch");
    bad = valid; bad.output.state = 0xffffffffu;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "unknown_ffx_state");
    bad = valid; bad.enableSharpening = true; bad.sharpness = std::numeric_limits<float>::quiet_NaN();
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "nan_sharpness");

    bad = valid; bad.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB | FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "conflicting_transfer_functions");
    Gpu other;
    auto foreign = makeColor(other, 100);
    bad = valid; bad.color = ffxApiGetResourceDX12(foreign.Get());
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "foreign_resource_device");
    auto foreignRecording = other.makeRecording();
    bad = valid; bad.commandList = foreignRecording.list.Get();
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "foreign_list_device");
    check(foreignRecording.list->Close(), "foreign list Close");
    ComPtr<ID3D12CommandAllocator> copyAllocator;
    ComPtr<ID3D12GraphicsCommandList> copyList;
    check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&copyAllocator)), "copy allocator");
    check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, copyAllocator.Get(), nullptr,
                                       IID_PPV_ARGS(&copyList)), "copy list");
    bad = valid; bad.commandList = copyList.Get();
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "wrong_list_type");
    check(copyList->Close(), "copy list Close");
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "negative dispatch list Close");
    bad = valid;
    reject(bad, FFX_API_RETURN_ERROR_PARAMETER, "closed_list");
    gpu.executeAndWait(recording.list.Get());
    D3D12_RANGE range{0, static_cast<SIZE_T>(output.readback.totalBytes)};
    uint8_t* bytes = nullptr;
    check(output.readback.buffer->Map(0, &range, reinterpret_cast<void**>(&bytes)), "negative readback Map");
    for (UINT y = 0; y < kOutputBacking; ++y)
        for (UINT x = 0; x < kOutputBacking; ++x)
            if (!std::equal(output.sentinel.begin(), output.sentinel.end(), bytes + output.readback.footprint.Offset +
                size_t(y) * output.readback.footprint.Footprint.RowPitch + size_t(x) * 4))
                fail("FSR failed dispatch mutated output");
    output.readback.buffer->Unmap(0, nullptr);
    std::puts("OWNED_D3D12_FSR_CASE_PASS case=rejected_dispatch_output_unchanged result=0");
}

void fsrGpuFrames(const FsrApi& api, Gpu& gpu, ffxContext* context,
                   ID3D12Resource* depth, ID3D12Resource* motion, const char* variant) {
    double resetMean = 0;
    for (UINT frame = 0; frame < 4; ++frame) {
        const uint8_t tag = frame == 1 ? 160 : 96;
        auto color = makeColor(gpu, tag);
        auto output = makeOutput(gpu, frame);
        auto recording = gpu.makeRecording();
        auto desc = fsrFrame(recording.list.Get(), color.Get(), depth, motion, output.resource.Get());
        desc.reset = frame != 1;
        if (frame == 0)
            desc.exposure = desc.color; // Auto-exposure must ignore this unused, wrong-format texture.
        if (frame == 2)
            desc.upscaleSize = {kOutputWidth, kOutputHeight};
        // Different content extent within the creation maximum, same fixed output.
        if (frame == 3)
            desc.renderSize = {48, 48};
        fsrResult(api.dispatch(context, &desc.header), FFX_API_RETURN_OK, "gpu_dispatch");
        // The API has returned: later caller descriptor changes must not affect recording.
        desc.color = {}; desc.output = {}; desc.preExposure = -1;
        // Exercise the declared post-dispatch states, rather than merely copying.
        transition(recording.list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        transition(recording.list.Get(), output.resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(recording.list.Get(), color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(recording.list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "FSR frame Close");
        gpu.executeAndWait(recording.list.Get());
        const auto stats = validateOutput(output, tag, "fsr_constant", frame);
        if (frame == 0)
            resetMean = stats.mean;
        if (frame == 1 && stats.mean - resetMean <= 8.0 / 255.0)
            fail("FSR changed color did not increase output");
        if (frame == 2 && std::fabs(stats.mean - resetMean) > 1.0 / 255.0)
            fail("FSR history reset did not recover first-frame constant");
        std::printf("OWNED_D3D12_FSR_FRAME_PASS frame=%u variant=%s input_tag=%u output_mean=%.9g reset=%u render=%u output=128 sentinel=1 state_restored=1\n",
                    frame, variant, tag, stats.mean, frame != 1, frame == 3 ? 48 : 64);
    }
}

void fsrDeviceProxy(const FsrApi& api, Gpu& gpu, const ffxAllocationCallbacks& callbacks,
                    ID3D12Resource* depth, ID3D12Resource* motion) {
    FsrProxyRefs refs;
    {
        ComPtr<ID3D12Device> proxy;
        proxy.Attach(new FsrDeviceProxy(gpu.device.Get(), refs));
        {
            ComPtr<IUnknown> proxyIdentity, deviceIdentity;
            ComPtr<ID3D12Device> proxyDevice;
            check(proxy.As(&proxyIdentity), "FSR proxy IUnknown");
            check(proxy.As(&proxyDevice), "FSR proxy ID3D12Device");
            check(gpu.device.As(&deviceIdentity), "FSR underlying IUnknown");
            if (proxyIdentity.Get() == deviceIdentity.Get() || proxyDevice.Get() != proxy.Get() ||
                proxyDevice.Get() == gpu.device.Get())
                fail("FSR proxy identity must differ from underlying device");
        }
        FsrCreation creation(proxy.Get());
        ffxContext context = nullptr;
        fsrResult(api.create(&context, &creation.desc.header, &callbacks), FFX_API_RETURN_OK, "proxy_create");
        if (!context)
            fail("FSR proxy create returned no context");
        {
            auto color = makeColor(gpu, 100);
            auto output = makeOutput(gpu, 777);
            auto recording = gpu.makeRecording();
            const auto valid = fsrFrame(recording.list.Get(), color.Get(), depth, motion, output.resource.Get());
            Gpu other;
            auto foreign = makeColor(other, 100);
            auto bad = valid;
            bad.color = ffxApiGetResourceDX12(foreign.Get());
            fsrResult(api.dispatch(&context, &bad.header), FFX_API_RETURN_ERROR_PARAMETER, "proxy_foreign_resource_device");
            auto foreignRecording = other.makeRecording();
            bad = valid;
            bad.commandList = foreignRecording.list.Get();
            fsrResult(api.dispatch(&context, &bad.header), FFX_API_RETURN_ERROR_PARAMETER, "proxy_foreign_list_device");
            check(foreignRecording.list->Close(), "FSR proxy foreign list Close");
            check(recording.list->Close(), "FSR proxy negative list Close");
        }
        // Lists and resources deliberately come from gpu, not from the proxy.
        fsrGpuFrames(api, gpu, &context, depth, motion, "device_proxy");
        fsrResult(api.destroy(&context, &callbacks), FFX_API_RETURN_OK, "proxy_destroy");
        if (context)
            fail("FSR proxy destroy retained context");
    }
    if (refs.destroyed != 1 || refs.released != refs.added + 1)
        fail("FSR proxy COM references unbalanced");
    std::puts("OWNED_D3D12_FSR_CASE_PASS case=device_proxy_underlying_gpu result=0");
}

void fsrDestroyReplay(const FsrApi& api, Gpu& gpu, FsrCreation& creation,
                       const ffxAllocationCallbacks& callbacks, ID3D12Resource* depth,
                       ID3D12Resource* motion) {
    ffxContext context = nullptr;
    fsrResult(api.create(&context, &creation.desc.header, &callbacks), FFX_API_RETURN_OK, "replay_create");
    auto color = makeColor(gpu, 116);
    auto output = makeOutput(gpu, 1000);
    auto recording = gpu.makeRecording();
    auto desc = fsrFrame(recording.list.Get(), color.Get(), depth, motion, output.resource.Get());
    fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_OK, "replay_record");
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "FSR replay Close");
    ffxContext stale = context;
    ffxContext staleDispatch = context;
    fsrResult(api.destroy(&context, &callbacks), FFX_API_RETURN_OK, "destroy_before_submit");
    if (context)
        fail("FSR destroy did not null context");
    fsrResult(api.destroy(&stale, &callbacks), FFX_API_RETURN_ERROR_PARAMETER, "stale_destroy");
    fsrResult(api.dispatch(&staleDispatch, &desc.header), FFX_API_RETURN_ERROR_PARAMETER, "stale_dispatch");
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    fsrResult(api.query(&staleDispatch, &provider.header), FFX_API_RETURN_ERROR_PARAMETER, "stale_query");
    double firstMean = 0;
    for (UINT replay = 0; replay < 3; ++replay) {
        const uint8_t inputTag = replay == 2 ? 140 : 116;
        if (replay == 2) {
            auto update = gpu.makeRecording();
            transition(update.list.Get(), color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            check(update.list->Close(), "FSR input update Close");
            gpu.executeAndWait(update.list.Get());
            std::vector<uint8_t> pixels(size_t(kInputWidth) * kInputHeight * 4, inputTag);
            for (size_t i = 3; i < pixels.size(); i += 4)
                pixels[i] = 255;
            uploadTexture(gpu, color.Get(), pixels.data(), kInputWidth * 4,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        gpu.executeAndWait(recording.list.Get());
        const auto stats = validateOutput(output, inputTag, "fsr_destroy_replay", replay);
        if (!replay)
            firstMean = stats.mean;
        else if (replay == 1 && stats.mean != firstMean)
            fail("FSR replay changed constant output");
        if (replay == 2 && stats.mean - firstMean < 0.05)
            fail("FSR replay failed to consume changed GPU input");
        std::printf("OWNED_D3D12_FSR_REPLAY_PASS replay=%u input_tag=%u output_mean=%.9g destroyed_before_submit=1 sentinel=1 changed_gpu_input=%u\n",
                    replay, inputTag, stats.mean, replay == 2);
    }
}
void fsrManualExposure(const FsrApi& api, Gpu& gpu, FsrCreation& creation,
                       const ffxAllocationCallbacks& callbacks, ID3D12Resource* depth,
                       ID3D12Resource* motion) {
    creation.desc.flags = kFsrFlags & ~FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    ffxContext context = nullptr;
    fsrResult(api.create(&context, &creation.desc.header, &callbacks), FFX_API_RETURN_OK, "manual_exposure_create");
    ffxQueryDescUpscaleGetResourceRequirements requirements{};
    requirements.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    fsrResult(api.query(&context, &requirements.header), FFX_API_RETURN_OK, "manual_exposure_requirements");
    if ((requirements.required_resources & FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE) ||
        requirements.optional_resources != (FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE |
            FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK | FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION))
        fail("FSR manual exposure optional-resource contract");
    auto color = makeColor(gpu, 116);
    std::vector<uint8_t> referenceImage;
    for (DXGI_FORMAT format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32_FLOAT}) {
        ComPtr<ID3D12Resource> exposure;
        const uint16_t half = 0x3c00;
        const float full = 1.0f;
        if (format != DXGI_FORMAT_UNKNOWN) {
            exposure = createTexture(gpu.device.Get(), 1, 1, format, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);
            uploadTexture(gpu, exposure.Get(), format == DXGI_FORMAT_R16_FLOAT ?
                          static_cast<const void*>(&half) : static_cast<const void*>(&full),
                          format == DXGI_FORMAT_R16_FLOAT ? sizeof(half) : sizeof(full),
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        auto output = makeOutput(gpu, 1200);
        auto recording = gpu.makeRecording();
        auto desc = fsrFrame(recording.list.Get(), color.Get(), depth, motion, output.resource.Get());
        if (exposure)
            desc.exposure = ffxApiGetResourceDX12(exposure.Get());
        fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_OK, "manual_exposure_dispatch");
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "FSR manual exposure Close");
        gpu.executeAndWait(recording.list.Get());
        const auto stats = validateOutput(output, 116, "fsr_manual_exposure", format);
        D3D12_RANGE range{0, static_cast<SIZE_T>(output.readback.totalBytes)};
        uint8_t* bytes = nullptr;
        check(output.readback.buffer->Map(0, &range, reinterpret_cast<void**>(&bytes)), "FSR exposure oracle Map");
        const size_t logicalRowBytes = size_t(kOutputWidth) * 4;
        if (format == DXGI_FORMAT_UNKNOWN)
            referenceImage.resize(logicalRowBytes * kOutputHeight);
        for (UINT row = 0; row < kOutputHeight; ++row) {
            const auto* actual = bytes + output.readback.footprint.Offset +
                size_t(row) * output.readback.footprint.Footprint.RowPitch;
            auto* expected = referenceImage.data() + size_t(row) * logicalRowBytes;
            if (format == DXGI_FORMAT_UNKNOWN)
                std::memcpy(expected, actual, logicalRowBytes);
            else if (std::memcmp(expected, actual, logicalRowBytes))
                fail("FSR explicit unit exposure differs from implicit unit exposure");
        }
        output.readback.buffer->Unmap(0, nullptr);
        std::printf("OWNED_D3D12_FSR_EXPOSURE_PASS format=%u value=1 output_mean=%.9g sentinel=1\n",
                    format, stats.mean);
    }
    fsrResult(api.destroy(&context, &callbacks), FFX_API_RETURN_OK, "manual_exposure_destroy");
    creation.desc.flags = kFsrFlags;
}
// Read logical pixels and independently protect all backing padding. This also
// handles changing logical output sizes, unlike the fixed temporal smoke helper.
std::vector<uint8_t> fsrPixels(const OutputResource& output, UINT width, UINT height) {
    D3D12_RANGE range{0, static_cast<SIZE_T>(output.readback.totalBytes)};
    uint8_t* bytes = nullptr;
    check(output.readback.buffer->Map(0, &range, reinterpret_cast<void**>(&bytes)), "FSR pixels Map");
    std::vector<uint8_t> image(size_t(width) * height * 4);
    for (UINT y = 0; y < output.readback.footprint.Footprint.Height; ++y)
        for (UINT x = 0; x < output.readback.footprint.Footprint.Width; ++x) {
            const auto* pixel = bytes + output.readback.footprint.Offset +
                size_t(y) * output.readback.footprint.Footprint.RowPitch + size_t(x) * 4;
            if (x < width && y < height)
                std::copy_n(pixel, 4, image.data() + (size_t(y) * width + x) * 4);
            else if (!std::equal(output.sentinel.begin(), output.sentinel.end(), pixel))
                fail("FSR variable output padding changed");
        }
    output.readback.buffer->Unmap(0, nullptr);
    return image;
}

void fsrEqualPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                    const char* name, int tolerance = 1) {
    if (a.size() != b.size()) fail("FSR image dimensions differ");
    int maximum = 0;
    for (size_t i = 0; i < a.size(); ++i)
        maximum = std::max(maximum, std::abs(int(a[i]) - int(b[i])));
    if (maximum > tolerance) {
        std::fprintf(stderr, "OWNED_D3D12_FSR_FAIL case=%s max_byte_delta=%d tolerance=%d\n", name, maximum, tolerance);
        ExitProcess(1);
    }
    std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=%s max_byte_delta=%d sentinel=1\n", name, maximum);
}

void fsrScaleCap(const FsrApi& api, Gpu& gpu) {
    const char* scaleText = std::getenv("OWNED_FSR_NATIVE_MAX_SCALE");
    char* end = nullptr;
    const double scale = scaleText ? std::strtod(scaleText, &end) : 0;
    if (!scaleText || *end || !std::isfinite(scale) || scale < 1 || scale >= 3.5)
        fail("set OWNED_FSR_NATIVE_MAX_SCALE from same-device public MetalFX descriptor probe (1<=max<3.5)");
    const UINT nativeExtent = static_cast<UINT>(std::floor(64 * scale));
    FsrCreation creation(gpu.device.Get());
    creation.desc.maxUpscaleSize = {256, 224};
    creation.desc.flags &= ~FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    ffxContext context = nullptr;
    fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "scale_cap_create");
    auto color = makeColor(gpu, 112);
    auto motion = makeMotion(gpu);
    auto depth = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R32_FLOAT,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> depths(64 * 64, 0.75f);
    uploadTexture(gpu, depth.Get(), depths.data(), 64 * sizeof(float), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    std::vector<uint8_t> reference;
    for (UINT mode = 0; mode < 2; ++mode) {
        const UINT width = mode ? 256 : nativeExtent, height = mode ? 224 : nativeExtent;
        OutputResource output;
        output.resource = createTexture(gpu.device.Get(), 264, 232, DXGI_FORMAT_R8G8B8A8_UNORM,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        output.sentinel = {17, 37, 59, 83};
        std::vector<uint8_t> backing(264 * 232 * 4);
        for (size_t i = 0; i < backing.size(); ++i) backing[i] = output.sentinel[i % 4];
        uploadTexture(gpu, output.resource.Get(), backing.data(), 264 * 4, D3D12_RESOURCE_STATE_COPY_SOURCE);
        output.readback = createTextureReadback(gpu, output.resource.Get());
        auto recording = gpu.makeRecording();
        auto desc = fsrFrame(recording.list.Get(), color.Get(), depth.Get(), motion.Get(), output.resource.Get());
        desc.upscaleSize = {width, height};
        fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_OK, "scale_cap_dispatch");
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "scale cap Close");
        gpu.executeAndWait(recording.list.Get());
        auto image = fsrPixels(output, width, height);
        if (!mode) reference = image;
        else {
            const UINT baseX = (width - nativeExtent) / 2, baseY = (height - nativeExtent) / 2;
            std::vector<uint8_t> center(reference.size());
            for (UINT y = 0; y < height; ++y)
                for (UINT x = 0; x < width; ++x) {
                    const auto* p = image.data() + (size_t(y) * width + x) * 4;
                    if (x >= baseX && x < baseX + nativeExtent && y >= baseY && y < baseY + nativeExtent)
                        std::copy_n(p, 4, center.data() + (size_t(y - baseY) * nativeExtent + x - baseX) * 4);
                    else if (p[0] || p[1] || p[2] || p[3] != 255)
                        fail("FSR centered cap margin not opaque black");
                }
            fsrEqualPixels(reference, center, "cap_center_matches_native_scale_control");
            if (reference[(size_t(nativeExtent / 2) * nativeExtent + nativeExtent / 2) * 4] < 64)
                fail("FSR centered cap produced black content");
            std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=cap_scale_geometry native_max=%.9g content=%ux%u caller=256x224 center=%u,%u black_opaque=1 sentinel=1\n",
                        scale, nativeExtent, nativeExtent, baseX, baseY);
        }
    }
    fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "scale_cap_destroy");
}

void fsrInflightAndDiscard(const FsrApi& api, Gpu& gpu, ID3D12Resource* depth, ID3D12Resource* motion) {
    FsrCreation creation(gpu.device.Get());
    ffxContext context = nullptr;
    fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "inflight_create");
    auto color = makeColor(gpu, 124);
    auto output = makeOutput(gpu, 1800);
    auto recording = gpu.makeRecording();
    auto desc = fsrFrame(recording.list.Get(), color.Get(), depth, motion, output.resource.Get());
    fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_OK, "inflight_record");
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "inflight Close");
    ComPtr<ID3D12Fence> gate, completed;
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "inflight gate");
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completed)), "inflight completion");
    check(gpu.queue->Wait(gate.Get(), 1), "inflight gate Wait");
    ID3D12CommandList* lists[] = {recording.list.Get()};
    gpu.queue->ExecuteCommandLists(1, lists);
    check(gpu.queue->Signal(completed.Get(), 1), "inflight completion Signal");
    fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "destroy_inflight");
    color.Reset(); output.resource.Reset(); recording.extension.Reset(); recording.list.Reset();
    check(gate->Signal(1), "inflight gate release");
    waitForFenceValue(completed.Get(), 1, "inflight bounded completion");
    validateOutput(output, 124, "fsr_inflight_release", 0);
    std::puts("OWNED_D3D12_FSR_BEHAVIOR_PASS case=inflight_release fence_gate=1 context_destroyed=1 caller_resources_released=1 sentinel=1");

    fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "discard_create");
    color = makeColor(gpu, 124); output = makeOutput(gpu, 1801);
    recording = gpu.makeRecording();
    desc = fsrFrame(recording.list.Get(), color.Get(), depth, motion, output.resource.Get());
    fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_OK, "discard_record");
    check(recording.list->Close(), "discard Close");
    ComPtr<ID3D12CommandAllocator> replacement;
    check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)), "discard replacement allocator");
    check(recording.list->Reset(replacement.Get(), nullptr), "discard list Reset");
    copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
    check(recording.list->Close(), "discard readback Close");
    fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "discard_destroy");
    gpu.executeAndWait(recording.list.Get());
    auto pixels = fsrPixels(output, 128, 128);
    for (size_t i = 0; i < pixels.size(); ++i)
        if (pixels[i] != output.sentinel[i % 4]) fail("FSR discarded recording executed");
    std::puts("OWNED_D3D12_FSR_BEHAVIOR_PASS case=command_list_reset_discards_work output_unchanged=1 sentinel=1");
}

void fsrExposureReplay(const FsrApi& api, Gpu& gpu, ID3D12Resource* depth, ID3D12Resource* motion) {
    FsrCreation creation(gpu.device.Get());
    creation.desc.flags = (kFsrFlags & ~FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    auto color = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> colors(64 * 64 * 4);
    // Discriminating HDR asset independently exercised by the direct public
    // Metal4 native control; constant-reset brightness is not an exposure oracle.
    for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x) {
            auto* p = colors.data() + (y * 64 + x) * 4;
            p[0] = ((x ^ y) & 1) ? 0x4400 : 0x3400; // 4 / 0.25
            p[1] = x % 7 ? 0x3c00 : 0x4800; // 1 / 8
            p[2] = y % 5 ? 0x3800 : 0x4000; // 0.5 / 2
            p[3] = 0x3c00;
        }
    uploadTexture(gpu, color.Get(), colors.data(), 64 * 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto exposure16 = createTexture(gpu.device.Get(), 1, 1, DXGI_FORMAT_R16_FLOAT,
                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto exposure32 = createTexture(gpu.device.Get(), 1, 1, DXGI_FORMAT_R32_FLOAT,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    check(gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "FSR exposure RTV heap");
    const auto rtv = heap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(exposure32.Get(), nullptr, rtv);
    const uint16_t initial = 0x3555;
    uploadTexture(gpu, exposure16.Get(), &initial, sizeof(initial), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Record with exposure32 in its declared eventual shader-read state; actual
    // GPU producer runs before either consumer on every replay.
    OutputResource outputs[2];
    Recording recordings[2];
    for (UINT index = 0; index < 2; ++index) {
        outputs[index].resource = createTexture(gpu.device.Get(), 128, 128, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        outputs[index].readback = createTextureReadback(gpu, outputs[index].resource.Get());
        recordings[index] = gpu.makeRecording();
        ffxContext context = nullptr;
        fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "exposure_replay_create");
        auto desc = fsrFrame(recordings[index].list.Get(), color.Get(), depth, motion, outputs[index].resource.Get());
        desc.exposure = ffxApiGetResourceDX12(index ? exposure32.Get() : exposure16.Get());
        fsrResult(api.dispatch(&context, &desc.header), FFX_API_RETURN_OK, "exposure_replay_record");
        copyTextureToReadback(recordings[index].list.Get(), outputs[index].resource.Get(), outputs[index].readback);
        check(recordings[index].list->Close(), "FSR exposure replay Close");
        fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "exposure_replay_destroy_before_submit");
    }
    constexpr float values[] = {0.3333f, 1.234567f, 3.14159f};
    constexpr uint16_t rounded[] = {0x3555, 0x3cf0, 0x4248}; // IEEE nearest-even oracle.
    std::vector<uint16_t> previous;
    UINT changed = 0;
    for (UINT replay = 0; replay < 3; ++replay) {
        if (replay) {
            auto update = gpu.makeRecording();
            transition(update.list.Get(), exposure16.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            check(update.list->Close(), "FSR half exposure update Close");
            gpu.executeAndWait(update.list.Get());
            uploadTexture(gpu, exposure16.Get(), &rounded[replay], sizeof(uint16_t), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        auto producer = gpu.makeRecording();
        if (replay) transition(producer.list.Get(), exposure32.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[] = {values[replay], 0, 0, 1};
        producer.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        transition(producer.list.Get(), exposure32.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        check(producer.list->Close(), "FSR GPU exposure producer Close");
        gpu.executeAndWait(producer.list.Get());
        std::vector<uint16_t> image[2];
        for (UINT index = 0; index < 2; ++index) {
            gpu.executeAndWait(recordings[index].list.Get());
            uint8_t* bytes = nullptr;
            check(outputs[index].readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&bytes)), "FSR exposure half pixels Map");
            image[index].resize(128 * 128 * 4);
            for (UINT y = 0; y < 128; ++y)
                std::memcpy(image[index].data() + y * 128 * 4, bytes + outputs[index].readback.footprint.Offset +
                            size_t(y) * outputs[index].readback.footprint.Footprint.RowPitch, 128 * 8);
            outputs[index].readback.buffer->Unmap(0, nullptr);
        }
        UINT maxUlp = 0;
        for (size_t i = 0; i < image[0].size(); ++i) {
            if ((image[0][i] & 0x7c00) == 0x7c00 || (image[1][i] & 0x7c00) == 0x7c00) fail("FSR exposure nonfinite half output");
            maxUlp = std::max(maxUlp, UINT(std::abs(int(image[0][i]) - int(image[1][i]))));
        }
        if (maxUlp > 1) fail("FSR GPU R32 exposure differs from independently rounded R16 control");
        if (!previous.empty() && previous != image[0]) ++changed;
        previous = image[0];
        std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=exposure_gpu_replay replay=%u value=%.9g half_bits=%04x max_ulp=%u contexts_destroyed=1 gpu_produced_r32=1\n",
                    replay, values[replay], rounded[replay], maxUlp);
    }
    if (changed != 2) fail("FSR exposure replay did not consume changing GPU scalar");
}

void fsrFullBehavior(const FsrApi& api, Gpu& gpu, ID3D12Resource* depth, ID3D12Resource* motion) {
    FsrCreation creation(gpu.device.Get());
    float cameraFar = 1000.0f;
    auto color = makeColor(gpu, 112);
    auto highMotion = makeMotion(gpu, DXGI_FORMAT_R16G16_FLOAT, 128, 128);
    auto mask = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R8_UNORM,
                              D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> masks(64 * 64, 192);
    uploadTexture(gpu, mask.Get(), masks.data(), 64, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto run = [&](ffxContext* context, UINT width, UINT height, ID3D12Resource* mv,
                   uint32_t transfer, bool sharpen, bool reset, bool reactive, bool composition,
                   ID3D12Resource* input = nullptr, float mvScale = 64.0f, bool jitter = false,
                   bool poisonExposure = false, ID3D12Resource* suppliedMask = nullptr,
                   ID3D12Resource* compositionMask = nullptr) {
        auto output = makeOutput(gpu, 1400);
        auto recording = gpu.makeRecording();
        auto desc = fsrFrame(recording.list.Get(), input ? input : color.Get(), depth, mv, output.resource.Get());
        desc.cameraFar = cameraFar;
        desc.upscaleSize = {width, height};
        desc.reset = reset;
        desc.flags = transfer;
        desc.enableSharpening = sharpen;
        desc.sharpness = 1;
        desc.motionVectorScale = {mvScale, mvScale};
        if (jitter) desc.jitterOffset = reset ? FfxApiFloatCoords2D{0.25f, -0.125f} : FfxApiFloatCoords2D{-0.25f, 0.125f};
        if (poisonExposure) desc.exposure = desc.color;
        if (reactive) desc.reactive = ffxApiGetResourceDX12(suppliedMask ? suppliedMask : mask.Get());
        if (composition) desc.transparencyAndComposition = ffxApiGetResourceDX12(compositionMask ? compositionMask : suppliedMask ? suppliedMask : mask.Get());
        fsrResult(api.dispatch(context, &desc.header), FFX_API_RETURN_OK, "full_behavior_dispatch");
        copyTextureToReadback(recording.list.Get(), output.resource.Get(), output.readback);
        check(recording.list->Close(), "FSR full behavior Close");
        gpu.executeAndWait(recording.list.Get());
        return fsrPixels(output, width, height);
    };
    std::vector<uint8_t> low;
    // Formerly unsupported public options are real GPU paths, not success-only probes.
    for (uint32_t option : {uint32_t(FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS),
                            uint32_t(FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION), uint32_t(0)}) {
        creation.desc.flags = kFsrFlags | option;
        ffxContext context = nullptr;
        fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "full_motion_create");
        auto image = run(&context, 128, 128, option == FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS ?
                         highMotion.Get() : motion, 0, false, true, false, false);
        if (low.empty()) low = image;
        else fsrEqualPixels(low, image, "zero_motion_resolution_jitter_equivalence");
        auto poisoned = run(&context, 128, 128, option == FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS ?
                            highMotion.Get() : motion, 0, false, true, false, false, nullptr, 64, false, true);
        fsrEqualPixels(image, poisoned, "auto_exposure_ignores_poison_texture");
        fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "full_motion_destroy");
    }
    for (uint32_t reversed : {uint32_t(0), uint32_t(FFX_UPSCALE_ENABLE_DEPTH_INVERTED)}) {
        creation.desc.flags = (kFsrFlags & ~FFX_UPSCALE_ENABLE_DEPTH_INVERTED) |
            FFX_UPSCALE_ENABLE_DEPTH_INFINITE | reversed;
        ffxContext infiniteContext = nullptr;
        fsrResult(api.create(&infiniteContext, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "infinite_depth_create");
        auto finiteFar = run(&infiniteContext, 128, 128, motion, 0, false, true, false, false);
        cameraFar = std::numeric_limits<float>::infinity();
        auto infiniteFar = run(&infiniteContext, 128, 128, motion, 0, false, true, false, false);
        fsrEqualPixels(finiteFar, infiniteFar, "infinite_depth_far_equivalence");
        cameraFar = 1000.0f;
        fsrResult(api.destroy(&infiniteContext, nullptr), FFX_API_RETURN_OK, "infinite_depth_destroy");
    }
    creation.desc.flags = (kFsrFlags & ~FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    ffxContext context = nullptr;
    fsrResult(api.create(&context, &creation.desc.header, nullptr), FFX_API_RETURN_OK, "full_manual_create");
    auto linear = run(&context, 128, 128, motion, 0, false, true, false, false);
    auto sharpened = run(&context, 128, 128, motion, 0, true, true, false, false);
    fsrEqualPixels(linear, sharpened, "sharpen_constant_invariance", 2);
    auto edge = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> edgePixels(64 * 64 * 4);
    for (UINT y = 0; y < 64; ++y)
        for (UINT x = 0; x < 64; ++x) {
            auto* p = edgePixels.data() + (y * 64 + x) * 4;
            p[0] = p[1] = p[2] = x < 32 ? 64 : 160;
            p[3] = y < 32 ? 64 : 192;
        }
    uploadTexture(gpu, edge.Get(), edgePixels.data(), 64 * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (uint32_t option : {uint32_t(0), uint32_t(FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS),
                            uint32_t(FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)}) {
        FsrCreation motionCreation(gpu.device.Get());
        motionCreation.desc.flags = (kFsrFlags & ~FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) | option;
        ffxContext motionContext = nullptr;
        fsrResult(api.create(&motionContext, &motionCreation.desc.header, nullptr), FFX_API_RETURN_OK, "nonzero_motion_create");
        const UINT extent = option == FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS ? 128 : 64;
        std::vector<uint8_t> reference;
        for (UINT variant = 0; variant < 3; ++variant) {
            auto vectors = createTexture(gpu.device.Get(), extent, extent, DXGI_FORMAT_R16G16_FLOAT,
                                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
            std::vector<uint16_t> values(size_t(extent) * extent * 2, 0);
            // Exact IEEE binary16 1/64 and 1/32; equal field after caller scale.
            for (size_t i = 0; i < values.size(); i += 2) values[i] = variant == 0 ? 0x2400 : variant == 1 ? 0x2800 : 0;
            uploadTexture(gpu, vectors.Get(), values.data(), extent * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const bool jitter = option == FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
            run(&motionContext, 128, 128, vectors.Get(), 0, false, true, false, false, edge.Get(), variant == 1 ? 32 : 64, jitter);
            auto image = run(&motionContext, 128, 128, vectors.Get(), 0, false, false, false, false,
                             edge.Get(), variant == 1 ? 32 : 64, jitter);
            if (!variant) reference = image;
            else if (variant == 1) fsrEqualPixels(reference, image, "nonzero_motion_scale_equivalence", 2);
            else {
                uint64_t delta = 0;
                for (size_t i = 0; i < image.size(); ++i)
                    if (i % 4 != 3) delta += std::abs(int(image[i]) - int(reference[i]));
                if (!delta) fail("FSR motion stimulus did not demonstrate nonzero motion consumption");
                std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=nonzero_motion_effect option=%u rgb_byte_delta_sum=%llu jitter_nonzero=%u\n",
                            option, static_cast<unsigned long long>(delta), jitter);
            }
        }
        fsrResult(api.destroy(&motionContext, nullptr), FFX_API_RETURN_OK, "nonzero_motion_destroy");
    }
    auto plainEdge = run(&context, 128, 128, motion, 0, false, true, false, false, edge.Get());
    auto sharpEdge = run(&context, 128, 128, motion, 0, true, true, false, false, edge.Get());
    uint64_t plainVariation = 0, sharpVariation = 0;
    bool rgbChanged = false;
    for (UINT y = 8; y < 120; ++y)
        for (UINT x = 8; x < 120; ++x) {
            const size_t i = (size_t(y) * 128 + x) * 4;
            plainVariation += std::abs(int(plainEdge[i]) - int(plainEdge[i - 4]));
            sharpVariation += std::abs(int(sharpEdge[i]) - int(sharpEdge[i - 4]));
            rgbChanged |= plainEdge[i] != sharpEdge[i];
            if (plainEdge[i + 3] != sharpEdge[i + 3]) fail("FSR sharpening modified alpha");
        }
    if (!rgbChanged || sharpVariation < plainVariation) fail("FSR sharpening did not increase controlled edge contrast");
    std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=sharpen_edge_alpha plain_variation=%llu sharp_variation=%llu alpha_unchanged=1\n",
                static_cast<unsigned long long>(plainVariation), static_cast<unsigned long long>(sharpVariation));
    for (uint32_t transfer : {uint32_t(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB), uint32_t(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ)}) {
        auto image = run(&context, 128, 128, motion, transfer, false, true, false, false);
        // IEC 61966-2-1 / SMPTE ST 2084, independently applied to the fixture
        // scene. Never duplicate the production shader/RCAS implementation.
        auto encode = [transfer](double value) {
            if (transfer == FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB)
                return value <= 0.0031308 ? 12.92 * value : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
            const double power = std::pow(value, 2610.0 / 16384.0);
            return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * power) /
                            (1.0 + 2392.0 / 128.0 * power), 2523.0 / 32.0);
        };
        auto decode = [transfer](double value) {
            if (transfer == FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB)
                return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            const double power = std::pow(value, 32.0 / 2523.0);
            return std::pow(std::max(power - 3424.0 / 4096.0, 0.0) /
                            (2413.0 / 128.0 - 2392.0 / 128.0 * power), 16384.0 / 2610.0);
        };
        // Native UNORM output quantizes LINEAR light before the finishing
        // transfer. PQ can amplify one linear quantization step into many
        // encoded bytes, so bound the independent decoded scene, not bytes.
        double constantError = 0;
        for (size_t i = 0; i < image.size(); ++i) {
            if (i % 4 == 3) {
                if (image[i] != linear[i]) fail("FSR constant transfer changed native alpha");
            } else constantError = std::max(constantError, std::fabs(decode(image[i] / 255.0) - decode(112.0 / 255.0)));
        }
        if (constantError > 1.5 / 255.0) fail("FSR constant transfer exceeded linear UNORM quantization bound");
        std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=constant_transfer_roundtrip transfer=%u linear_max_error=%.9g alpha_unchanged=1 sentinel=1\n",
                    transfer, constantError);
        auto encodedPixels = edgePixels;
        for (size_t i = 0; i < encodedPixels.size(); ++i)
            if (i % 4 != 3) encodedPixels[i] = static_cast<uint8_t>(std::lround(255 * encode(edgePixels[i] / 255.0)));
        auto encodedInput = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        uploadTexture(gpu, encodedInput.Get(), encodedPixels.data(), 64 * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto correct = run(&context, 128, 128, motion, transfer, false, true, false, false, encodedInput.Get());
        auto wrong = run(&context, 128, 128, motion, 0, false, true, false, false, encodedInput.Get());
        double error = 0;
        uint64_t separation = 0;
        size_t count = 0;
        for (UINT y = 8; y < 120; ++y)
            for (UINT x = 8; x < 120; ++x) {
                const size_t i = (size_t(y) * 128 + x) * 4;
                separation += std::abs(int(correct[i]) - int(wrong[i]));
                if (correct[i + 3] != plainEdge[i + 3]) fail("FSR transfer changed alpha");
                if (x < 56 || x >= 72) { error += std::fabs(decode(correct[i] / 255.0) - plainEdge[i] / 255.0); ++count; }
            }
        if (error / count > 0.02 || !separation) fail("FSR encoded-scene control failed or transfer was a no-op");
        std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=transfer_independent_scene transfer=%u linear_mean_abs_error=%.9g wrong_flag_byte_delta_sum=%llu alpha_unchanged=1\n",
                    transfer, error / count, static_cast<unsigned long long>(separation));
    }
    auto reactive = run(&context, 128, 128, motion, 0, false, true, true, false);
    auto composition = run(&context, 128, 128, motion, 0, false, true, false, true);
    auto combined = run(&context, 128, 128, motion, 0, false, true, true, true);
    fsrEqualPixels(reactive, composition, "reactive_composition_equivalence");
    fsrEqualPixels(reactive, combined, "mask_idempotence");
    // Independent history-consuming pair: same previous/current images and mask,
    // supplied through each public channel.
    std::vector<uint8_t> historyReference;
    for (UINT mode = 0; mode < 3; ++mode) {
        run(&context, 128, 128, motion, 0, false, true, mode != 1, mode != 0);
        auto image = run(&context, 128, 128, motion, 0, false, false, mode != 1, mode != 0, edge.Get());
        if (!mode) historyReference = image;
        else fsrEqualPixels(historyReference, image, "history_mask_channel_equivalence");
    }
    auto zeroMask = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R8_UNORM,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> zeros(64 * 64, 0);
    uploadTexture(gpu, zeroMask.Get(), zeros.data(), 64, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Same mask-enabled generation for both frames; changing ONLY texel values
    // must not be confused with the reset caused by enabling a new mask channel.
    run(&context, 128, 128, motion, 0, false, true, true, false, nullptr, 64, false, false, zeroMask.Get());
    auto unmasked = run(&context, 128, 128, motion, 0, false, false, true, false,
                        edge.Get(), 64, false, false, zeroMask.Get());
    uint64_t maskDifference = 0;
    for (size_t i = 0; i < unmasked.size(); ++i)
        if (i % 4 != 3) maskDifference += std::abs(int(unmasked[i]) - int(historyReference[i]));
    if (!maskDifference) fail("FSR history stimulus did not demonstrate reactive consumption");
    std::printf("OWNED_D3D12_FSR_BEHAVIOR_PASS case=reactive_history_effect rgb_byte_delta_sum=%llu\n",
                static_cast<unsigned long long>(maskDifference));
    auto lowerMask = createTexture(gpu.device.Get(), 64, 64, DXGI_FORMAT_R8_UNORM,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> lower(64 * 64, 64);
    uploadTexture(gpu, lowerMask.Get(), lower.data(), 64, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    run(&context, 128, 128, motion, 0, false, true, true, true, nullptr, 64, false, false, nullptr, lowerMask.Get());
    auto combinedLower = run(&context, 128, 128, motion, 0, false, false, true, true,
                             edge.Get(), 64, false, false, nullptr, lowerMask.Get());
    fsrEqualPixels(historyReference, combinedLower, "composition_below_reactive_no_change");
    // Output-generation switch without caller reset must behave like fresh history.
    run(&context, 96, 112, motion, 0, false, false, false, false);
    auto switched = run(&context, 128, 128, motion, 0, false, false, false, false);
    fsrEqualPixels(linear, switched, "output_generation_switch_resets_history");
    auto reset = run(&context, 128, 128, motion, 0, false, true, false, false);
    fsrEqualPixels(linear, reset, "history_reset_matches_fresh");
    fsrResult(api.destroy(&context, nullptr), FFX_API_RETURN_OK, "full_manual_destroy");
    std::puts("OWNED_D3D12_FSR_FULL_SR_PASS motion=1 masks=1 transfer=1 sharpening_constant=1 output_generation=1");
}
} // namespace

#ifndef OWNED_FSR_FIXTURE_ENTRY
#define OWNED_FSR_FIXTURE_ENTRY main
#endif
int OWNED_FSR_FIXTURE_ENTRY() {
    FsrApi api; // Missing DLL is an availability failure, not a GPU failure.
    ffxConfigureDescGlobalDebug1 debug{};
    debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
    debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_SILENCE;
    fsrResult(api.configure(nullptr, &debug.header), FFX_API_RETURN_OK, "configure_global_debug");
    Gpu gpu;
    requireOwnedBackend();
    printModulePath(L"d3d12.dll", "d3d12");
    AllocationStats allocations;
    const ffxAllocationCallbacks callbacks{&allocations, fsrAllocate, fsrDeallocate};
    FsrCreation creation(gpu.device.Get());
    creation.desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    ffxContext context = nullptr;
    fsrResult(api.create(&context, &creation.desc.header, &callbacks), FFX_API_RETURN_OK, "create");
    if (!context || !allocations.allocated)
        fail("FSR context/custom allocator unused");
    fsrQueries(api, gpu, &context);
    // Depth is uploaded explicitly alongside color and zero motion vectors.
    auto depth = createTexture(gpu.device.Get(), kInputWidth, kInputHeight, DXGI_FORMAT_R32_FLOAT,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> depths(size_t(kInputWidth) * kInputHeight, 0.75f);
    uploadTexture(gpu, depth.Get(), depths.data(), kInputWidth * sizeof(float),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto motion = makeMotion(gpu);
    fsrFullBehavior(api, gpu, depth.Get(), motion.Get());
    fsrErrorCases(api, gpu, creation, &context, callbacks, depth.Get(), motion.Get());
    fsrGpuFrames(api, gpu, &context, depth.Get(), motion.Get(), "direct");
    fsrResult(api.destroy(&context, &callbacks), FFX_API_RETURN_OK, "destroy");
    fsrResult(api.destroy(&context, &callbacks), FFX_API_RETURN_ERROR_PARAMETER, "destroy_twice");
    fsrDeviceProxy(api, gpu, callbacks, depth.Get(), motion.Get());
    fsrDestroyReplay(api, gpu, creation, callbacks, depth.Get(), motion.Get());
    fsrManualExposure(api, gpu, creation, callbacks, depth.Get(), motion.Get());
    fsrScaleCap(api, gpu);
    fsrInflightAndDiscard(api, gpu, depth.Get(), motion.Get());
    fsrExposureReplay(api, gpu, depth.Get(), motion.Get());
    if (allocations.allocated != allocations.freed)
        fail("FSR custom allocator leak/mismatch");
    std::printf("OWNED_D3D12_FSR_ALLOCATIONS allocated=%u freed=%u\n", allocations.allocated, allocations.freed);
    std::puts("OWNED_D3D12_FSR_PASS provider=MetalFX image_quality_equivalence=not_claimed");
    return 0;
}
