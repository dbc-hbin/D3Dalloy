// Public consumer: only official FFX exports are used for Prepare/Generate and
// swapchain control. Existing temporal helpers supply uploads/fences/readback.
#define main ownedTemporalFixtureMain
#include "owned-d3d12-temporal.cpp"
#undef main
#include <dxgi1_6.h>
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "owned-fg-pattern.h"
#include <cstdlib>
#include <atomic>

namespace {
constexpr UINT fgWidth = 128, fgHeight = 128;
constexpr D3D12_RESOURCE_STATES fgRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

[[noreturn]] void fgFail(const char* name, unsigned actual = 0) {
    std::fprintf(stderr, "OWNED_D3D12_FSR_FG_FAIL case=%s actual=%u\n", name, actual);
    ExitProcess(1);
}
void fgRequire(bool ok, const char* name) { if (!ok) fgFail(name); }
void fgResult(ffxReturnCode_t actual, ffxReturnCode_t expected, const char* name) {
    if (actual != expected) fgFail(name, actual);
}
struct FgApi {
    HMODULE module = nullptr;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    FgApi() {
        module = LoadLibraryW(L"C:\\windows\\system32\\amd_fidelityfx_framegeneration_dx12.dll");
        if (!module) {
            std::printf("OWNED_D3D12_FSR_FG_UNAVAILABLE stage=LoadLibrary error=%lu gpu_attempted=0\n", GetLastError());
            ExitProcess(2);
        }
        const char* names[] = {"ffxCreateContext", "ffxDestroyContext", "ffxConfigure", "ffxQuery", "ffxDispatch"};
        const WORD ordinals[] = {2, 3, 1, 5, 4}; // Public reference DLL ordinals.
        FARPROC functions[5]{};
        for (WORD i = 0; i < 5; ++i) {
            functions[i] = GetProcAddress(module, names[i]);
            fgRequire(functions[i] && functions[i] == GetProcAddress(module, MAKEINTRESOURCEA(ordinals[i])), "export-name-ordinal");
        }
        create = reinterpret_cast<PfnFfxCreateContext>(functions[0]);
        destroy = reinterpret_cast<PfnFfxDestroyContext>(functions[1]);
        configure = reinterpret_cast<PfnFfxConfigure>(functions[2]);
        query = reinterpret_cast<PfnFfxQuery>(functions[3]);
        dispatch = reinterpret_cast<PfnFfxDispatch>(functions[4]);
        printModulePath(L"amd_fidelityfx_framegeneration_dx12.dll", "fg");
    }
    ~FgApi() { FreeLibrary(module); }
};
struct FgAlloc {
    unsigned allocated = 0, freed = 0;
    static void* allocate(void* user, uint64_t size) {
        auto& self = *static_cast<FgAlloc*>(user);
        if (size > SIZE_MAX) return nullptr;
        void* memory = std::malloc(static_cast<size_t>(size));
        if (memory) ++self.allocated;
        return memory;
    }
    static void release(void* user, void* memory) {
        if (memory) ++static_cast<FgAlloc*>(user)->freed;
        std::free(memory);
    }
};
struct FgContext {
    const FgApi& api;
    ffxContext value = nullptr;
    FgAlloc allocations;
    ffxAllocationCallbacks callbacks{};
    ffxCreateBackendDX12Desc backend{};
    ffxCreateContextDescFrameGeneration desc{};
    FgContext(const FgApi& api, Gpu& gpu, UINT width = fgWidth, UINT height = fgHeight,
              DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT) : api(api) {
        callbacks = {&allocations, FgAlloc::allocate, FgAlloc::release};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = gpu.device.Get();
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        desc.displaySize = {width, height};
        desc.maxRenderSize = {kInputWidth, kInputHeight};
        desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(format);
        const auto result = api.create(&value, &desc.header, &callbacks);
        if (result != FFX_API_RETURN_OK) {
            std::printf("OWNED_D3D12_FSR_FG_UNAVAILABLE stage=create result=%u gpu_attempted=0\n", result);
            ExitProcess(2);
        }
    }
    void destroy() {
        fgResult(api.destroy(&value, &callbacks), FFX_API_RETURN_OK, "destroy-context");
        fgRequire(!value && allocations.allocated == allocations.freed, "allocator-balanced");
    }
    ~FgContext() { if (value) destroy(); }
};
Recording fgRecording(Gpu& gpu) {
    Recording r;
    check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.allocator)), "fg-allocator");
    check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.allocator.Get(), nullptr, IID_PPV_ARGS(&r.list)), "fg-list");
    return r;
}
// Public-only recording: private COM validation is not a prerequisite for FFX.
void fgUpload(Gpu& gpu, ID3D12Resource* texture, const void* pixels, size_t row, D3D12_RESOURCE_STATES state) {
    const auto desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows; UINT64 rowBytes, bytes;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &bytes);
    fgRequire(row >= rowBytes, "public-upload-row");
    auto source = createBuffer(gpu.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr; check(source->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "public-upload-map");
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch, static_cast<const uint8_t*>(pixels) + y * row, static_cast<size_t>(rowBytes));
    source->Unmap(0, nullptr);
    auto r = fgRecording(gpu);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = source.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = footprint;
    to.pResource = texture; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    r.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(r.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, state);
    check(r.list->Close(), "public-upload-close"); gpu.executeAndWait(r.list.Get());
}
ComPtr<ID3D12Resource> fgDepth(Gpu& gpu) {
    auto depth = createTexture(gpu.device.Get(), kInputWidth, kInputHeight, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> pixels(kInputWidth * kInputHeight, .75f);
    fgUpload(gpu, depth.Get(), pixels.data(), kInputWidth * 4, fgRead);
    return depth;
}
ComPtr<ID3D12Resource> fgMotion(Gpu& gpu) {
    auto motion = createTexture(gpu.device.Get(), kInputWidth, kInputHeight, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> pixels(kInputWidth * kInputHeight * 2);
    fgUpload(gpu, motion.Get(), pixels.data(), kInputWidth * 4, fgRead);
    return motion;
}
std::vector<uint8_t> fgPixels(Gpu& gpu, ID3D12Resource* texture, D3D12_RESOURCE_STATES state) {
    auto readback = createTextureReadback(gpu, texture);
    auto r = fgRecording(gpu);
    transition(r.list.Get(), texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(r.list.Get(), texture, readback);
    transition(r.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    check(r.list->Close(), "fg-readback-close");
    gpu.executeAndWait(r.list.Get());
    uint8_t* data = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(readback.totalBytes)};
    check(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&data)), "fg-readback-map");
    std::vector<uint8_t> pixels(readback.rows * readback.rowBytes);
    for (UINT y = 0; y < readback.rows; ++y)
        std::memcpy(pixels.data() + y * readback.rowBytes,
                    data + readback.footprint.Offset + y * readback.footprint.Footprint.RowPitch,
                    static_cast<size_t>(readback.rowBytes));
    const D3D12_RANGE written{0, 0};
    readback.buffer->Unmap(0, &written);
    return pixels;
}
uint64_t fgHash(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t value : bytes) hash = (hash ^ value) * 1099511628211ull;
    return hash;
}
ComPtr<ID3D12Resource> fgColor(Gpu& gpu, unsigned frame) {
    auto color = createTexture(gpu.device.Get(), fgWidth, fgHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto pixels = fgScene(frame);
    fgUpload(gpu, color.Get(), pixels.data(), fgWidth * 8, fgRead);
    return color;
}
ComPtr<ID3D12Resource> fgOutput(Gpu& gpu) {
    auto output = createTexture(gpu.device.Get(), fgWidth, fgHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> guard(fgWidth * fgHeight * 4, 0x3200);
    fgUpload(gpu, output.Get(), guard.data(), fgWidth * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return output;
}
ffxDispatchDescFrameGenerationPrepare fgPrepare(ID3D12GraphicsCommandList* list, uint64_t frame,
                                                ID3D12Resource* depth, ID3D12Resource* motion, bool reset) {
    ffxDispatchDescFrameGenerationPrepare p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    p.commandList = list;
    p.frameID = frame;
    p.renderSize = {kInputWidth, kInputHeight};
    p.motionVectorScale = {1.f, 1.f};
    p.frameTimeDelta = 1000.f / 30;
    p.unused_reset = reset;
    p.cameraNear = 0.1f; p.cameraFar = 100.f; p.cameraFovAngleVertical = 1.f;
    p.viewSpaceToMetersFactor = 1.f;
    p.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    return p;
}
ffxDispatchDescFrameGeneration fgGenerate(ID3D12GraphicsCommandList* list, uint64_t frame,
                                         ID3D12Resource* color, ID3D12Resource* output, bool reset,
                                         FfxApiRect2D rect = {0, 0, fgWidth, fgHeight}) {
    ffxDispatchDescFrameGeneration g{};
    g.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    g.commandList = list; g.frameID = frame; g.numGeneratedFrames = 1; g.reset = reset;
    g.presentColor = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    g.outputs[0] = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    g.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    g.minMaxLuminance[1] = 80.f; // scRGB 1.0 = 80 nits: identity native RGB domain.
    g.generationRect = rect;
    return g;
}
std::vector<uint8_t> fgFrame(FgContext& context, Gpu& gpu, unsigned frame,
                            ID3D12Resource* depth, ID3D12Resource* motion, bool reset = false,
                            bool mutateAfterPrepare = false,
                            FfxApiRect2D rect = {0, 0, fgWidth, fgHeight}) {
    auto color = fgColor(gpu, frame);
    auto output = fgOutput(gpu);
    auto r = fgRecording(gpu);
    auto p = fgPrepare(r.list.Get(), frame, depth, motion, reset);
    fgResult(context.api.dispatch(&context.value, &p.header), FFX_API_RETURN_OK, "prepare");
    check(r.list->Close(), "fg-prepare-close");
    gpu.executeAndWait(r.list.Get());
    if (mutateAfterPrepare) {
        r = fgRecording(gpu);
        transition(r.list.Get(), motion, fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(r.list.Get(), depth, fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "mutation-transition"); gpu.executeAndWait(r.list.Get());
        std::vector<float> depths(kInputWidth * kInputHeight, 0.03125f);
        std::vector<uint16_t> vectors(kInputWidth * kInputHeight * 2, 0x6000); // Finite +512-pixel vectors.
        fgUpload(gpu, depth, depths.data(), kInputWidth * 4, fgRead);
        fgUpload(gpu, motion, vectors.data(), kInputWidth * 4, fgRead);
    }
    r = fgRecording(gpu);
    auto g = fgGenerate(r.list.Get(), frame, color.Get(), output.Get(), reset, rect);
    fgResult(context.api.dispatch(&context.value, &g.header), FFX_API_RETURN_OK, "generate");
    check(r.list->Close(), "fg-generate-close"); gpu.executeAndWait(r.list.Get());
    auto pixels = fgPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::printf("OWNED_D3D12_FSR_FG_FRAME frame=%u reset=%u hash=%016llx source=gpu-readback\n",
                frame, reset, static_cast<unsigned long long>(fgHash(pixels)));
    return pixels;
}
void fgEqual(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const char* name) {
    fgRequire(a == b, name);
}
void fgDump(const std::vector<uint8_t>& pixels, const char* path) {
    FILE* file = std::fopen(path, "wb");
    fgRequire(file != nullptr, "pixel-output-open");
    fgRequire(std::fwrite(pixels.data(), pixels.size(), 1, file) == 1, "pixel-output-write");
    fgRequire(std::fclose(file) == 0, "pixel-output-close");
}
void fgDirect(const FgApi& api, Gpu& gpu) {
    auto depth = createTexture(gpu.device.Get(), kInputWidth, kInputHeight, DXGI_FORMAT_R32_FLOAT,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> depths(kInputWidth * kInputHeight, .75f);
    fgUpload(gpu, depth.Get(), depths.data(), kInputWidth * 4, fgRead);
    auto motion = fgMotion(gpu);
    std::vector<uint16_t> pan(kInputWidth * kInputHeight * 2);
    for (UINT i = 0; i < pan.size(); i += 2) pan[i] = 0x4000; // +2 render pixels, +4 display pixels.
    auto initializeMotion = fgRecording(gpu);
    transition(initializeMotion.list.Get(), motion.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
    check(initializeMotion.list->Close(), "initial-pan-transition"); gpu.executeAndWait(initializeMotion.list.Get());
    fgUpload(gpu, motion.Get(), pan.data(), kInputWidth * 4, fgRead);
    FgContext clean(api, gpu), snapshot(api, gpu);
    for (unsigned frame = 1; frame <= 4; ++frame) {
        const auto expected = fgFrame(clean, gpu, frame, depth.Get(), motion.Get());
        char path[64]; std::snprintf(path, sizeof(path), "fg-direct-frame-%u.bin", frame);
        fgDump(expected, path);
        if (frame == 4) {
            FgContext noHistory(api, gpu);
            fgRequire(expected != fgFrame(noHistory, gpu, frame, depth.Get(), motion.Get(), true), "history-affects-generated-pixels");
        }
        const auto actual = fgFrame(snapshot, gpu, frame, depth.Get(), motion.Get(), false, true);
        fgEqual(actual, expected, "prepare-gpu-snapshot-exact");
        auto r = fgRecording(gpu);
        transition(r.list.Get(), depth.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(r.list.Get(), motion.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "restore-inputs"); gpu.executeAndWait(r.list.Get());
        fgUpload(gpu, depth.Get(), depths.data(), kInputWidth * 4, fgRead);
        fgUpload(gpu, motion.Get(), pan.data(), kInputWidth * 4, fgRead);
    }
    const FfxApiRect2D rect{7, 11, 96, 88};
    auto pixels = fgFrame(snapshot, gpu, 5, depth.Get(), motion.Get(), true, false, rect);
    const auto preserved = fgScene(5);
    for (UINT y = 0; y < fgHeight; ++y)
        for (UINT x = 0; x < fgWidth; ++x)
            if (x < 7 || x >= 103 || y < 11 || y >= 99)
                for (UINT c = 0; c < 4; ++c) {
                    uint16_t value; std::memcpy(&value, pixels.data() + ((y * fgWidth + x) * 4 + c) * 2, 2);
                    fgRequire(value == preserved[(y * fgWidth + x) * 4 + c], "generation-rect-outside-preserved");
                }
    std::puts("OWNED_D3D12_FSR_FG_DIRECT_PASS snapshot=exact rect_guard=exact frames=4");
    FgContext reset(api, gpu), fresh(api, gpu), gap(api, gpu);
    fgFrame(reset, gpu, 1, depth.Get(), motion.Get());
    fgFrame(gap, gpu, 1, depth.Get(), motion.Get());
    const auto resetPixels = fgFrame(reset, gpu, 8, depth.Get(), motion.Get(), true);
    const auto freshPixels = fgFrame(fresh, gpu, 8, depth.Get(), motion.Get(), true);
    fgEqual(resetPixels, freshPixels, "explicit-reset-fresh-native-equivalence");
    fgEqual(fgFrame(gap, gpu, 8, depth.Get(), motion.Get()), freshPixels, "frame-gap-reset-equivalence");
    FgContext epoch(api, gpu), epochControl(api, gpu);
    fgFrame(epoch, gpu, 1, depth.Get(), motion.Get());
    auto oldColor = fgColor(gpu, 2); auto oldOutput = fgOutput(gpu);
    auto stale = fgRecording(gpu);
    auto oldPrepare = fgPrepare(stale.list.Get(), 2, depth.Get(), motion.Get(), false);
    fgResult(api.dispatch(&epoch.value, &oldPrepare.header), FFX_API_RETURN_OK, "epoch-prepare");
    auto oldGenerate = fgGenerate(stale.list.Get(), 2, oldColor.Get(), oldOutput.Get(), false);
    fgResult(api.dispatch(&epoch.value, &oldGenerate.header), FFX_API_RETURN_OK, "epoch-record-generate");
    check(stale.list->Close(), "epoch-stale-close");
    ffxConfigureDescFrameGeneration off{};
    off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    off.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    off.generationRect = {0, 0, fgWidth, fgHeight};
    fgResult(api.configure(&epoch.value, &off.header), FFX_API_RETURN_OK, "epoch-off");
    off.frameGenerationEnabled = true;
    fgResult(api.configure(&epoch.value, &off.header), FFX_API_RETURN_OK, "epoch-on");
    gpu.executeAndWait(stale.list.Get());
    fgEqual(fgFrame(epoch, gpu, 3, depth.Get(), motion.Get()),
            fgFrame(epochControl, gpu, 3, depth.Get(), motion.Get(), true), "stale-record-cannot-republish-new-epoch-history");
    auto negativeOutput = fgOutput(gpu); auto negativeColor = fgColor(gpu, 12);
    const auto negativeBefore = fgPixels(gpu, negativeOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto negative = fgRecording(gpu);
    auto missing = fgGenerate(negative.list.Get(), 12, negativeColor.Get(), negativeOutput.Get(), false);
    fgResult(api.dispatch(&epoch.value, &missing.header), FFX_API_RETURN_ERROR_PARAMETER, "generate-without-prepare");
    check(negative.list->Close(), "negative-close"); gpu.executeAndWait(negative.list.Get());
    fgEqual(fgPixels(gpu, negativeOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS), negativeBefore, "negative-output-unchanged");
    // The output is a real retained resource even if the public context and all
    // input COM references disappear before the recorded work executes.
    FgContext dying(api, gpu);
    auto dyingDepth = fgDepth(gpu);
    auto dyingMotion = fgMotion(gpu);
    auto dyingInit = fgRecording(gpu);
    transition(dyingInit.list.Get(), dyingMotion.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
    check(dyingInit.list->Close(), "lifetime-pan-transition"); gpu.executeAndWait(dyingInit.list.Get());
    fgUpload(gpu, dyingMotion.Get(), pan.data(), kInputWidth * 4, fgRead);
    auto color = fgColor(gpu, 1); auto output = fgOutput(gpu);
    auto r = fgRecording(gpu);
    auto p = fgPrepare(r.list.Get(), 1, dyingDepth.Get(), dyingMotion.Get(), true);
    fgResult(api.dispatch(&dying.value, &p.header), FFX_API_RETURN_OK, "lifetime-prepare");
    auto g = fgGenerate(r.list.Get(), 1, color.Get(), output.Get(), true);
    fgResult(api.dispatch(&dying.value, &g.header), FFX_API_RETURN_OK, "lifetime-generate");
    check(r.list->Close(), "lifetime-close");
    dying.destroy(); color.Reset(); dyingDepth.Reset(); dyingMotion.Reset();
    gpu.executeAndWait(r.list.Get());
    FgContext control(api, gpu);
    fgEqual(fgPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            fgFrame(control, gpu, 1, depth.Get(), motion.Get(), true), "destroy-recorded-resource-lifetime");
    std::puts("OWNED_D3D12_FSR_FG_HISTORY_PASS reset=fresh gap=reset off_on=fresh stale_record=no_history_publication negative_output=unchanged");
    FgContext bounded(api, gpu);
    ffxConfigureDescFrameGeneration configured{};
    configured.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configured.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    configured.frameGenerationEnabled = true; configured.generationRect = {0, 0, fgWidth, fgHeight};
    for (unsigned replacement = 0; replacement < 65; ++replacement)
        fgResult(api.configure(&bounded.value, &configured.header), FFX_API_RETURN_OK, "repeat-config-replaces-frame-snapshot");
    for (configured.frameID = 1; configured.frameID < 64; ++configured.frameID)
        fgResult(api.configure(&bounded.value, &configured.header), FFX_API_RETURN_OK, "configure-64-distinct-unretired-frames");
    fgResult(api.configure(&bounded.value, &configured.header), FFX_API_RETURN_ERROR_RUNTIME_ERROR, "reject-65th-distinct-config");
    configured.frameID = 0;
    fgResult(api.configure(&bounded.value, &configured.header), FFX_API_RETURN_OK, "replace-config-at-full-capacity");
    configured.frameGenerationEnabled = false;
    fgResult(api.configure(&bounded.value, &configured.header), FFX_API_RETURN_OK, "off-clears-config-snapshots");
    configured.frameGenerationEnabled = true; configured.frameID = 64;
    fgResult(api.configure(&bounded.value, &configured.header), FFX_API_RETURN_OK, "configure-after-off-clears-capacity");
    std::puts("OWNED_D3D12_FSR_FG_CONFIG_PASS repeated_id=replace retained_distinct_limit=64 overflow=runtime_error off_clears=1");
    std::puts("OWNED_D3D12_FSR_FG_LIFETIME_PASS context_destroy_before_execute=1 input_release=1 allocation_balance=1");
}
} // namespace

#include "owned-d3d12-fsr-fg-proxy.inc"

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    requireOwnedBackend();
    std::puts("OWNED_D3D12_FSR_FG_BACKEND id=owned-graphics-d3d12");
    FgApi api;
    Gpu gpu;
    fgDirect(api, gpu);
    fgProxy(api, gpu);
    std::puts("OWNED_D3D12_FSR_FG_PASS");
    return 0;
}
