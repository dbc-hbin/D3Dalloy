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
        // Direct dispatch has no proxy to enable the generation context.
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        config.frameGenerationEnabled = true;
        config.generationRect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
        fgResult(api.configure(&value, &config.header), FFX_API_RETURN_OK, "enable-direct-generation-context");
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
                                                ID3D12Resource* depth, ID3D12Resource* motion, bool reset,
                                                float frameTimeDelta = 1000.f / 30) {
    ffxDispatchDescFrameGenerationPrepare p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    p.commandList = list;
    p.frameID = frame;
    p.renderSize = {kInputWidth, kInputHeight};
    p.motionVectorScale = {1.f, 1.f};
    p.frameTimeDelta = frameTimeDelta;
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
                            FfxApiRect2D rect = {0, 0, fgWidth, fgHeight}, UINT flags = 0,
                            float frameTimeDelta = 1000.f / 30) {
    auto color = fgColor(gpu, frame);
    auto output = fgOutput(gpu);
    auto r = fgRecording(gpu);
    auto p = fgPrepare(r.list.Get(), frame, depth, motion, reset, frameTimeDelta);
    p.flags = flags;
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
    std::vector<uint8_t> secondFrame;
    for (unsigned frame = 1; frame <= 4; ++frame) {
        auto expected = fgFrame(clean, gpu, frame, depth.Get(), motion.Get());
        if (frame == 1) {
            const auto current = fgScene(frame);
            fgRequire(expected.size() == current.size() * sizeof(uint16_t) &&
                      !std::memcmp(expected.data(), current.data(), expected.size()), "native-bootstrap-current-pixels");
        }
        char path[64]; std::snprintf(path, sizeof(path), "fg-direct-frame-%u.bin", frame);
        fgDump(expected, path);
        if (frame == 4) {
            FgContext noHistory(api, gpu);
            fgRequire(expected != fgFrame(noHistory, gpu, frame, depth.Get(), motion.Get(), true), "history-affects-generated-pixels");
        }
        const auto actual = fgFrame(snapshot, gpu, frame, depth.Get(), motion.Get(), false, true);
        fgEqual(actual, expected, "prepare-gpu-snapshot-exact");
        if (frame == 2) secondFrame = std::move(expected);
        auto r = fgRecording(gpu);
        transition(r.list.Get(), depth.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(r.list.Get(), motion.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "restore-inputs"); gpu.executeAndWait(r.list.Get());
        fgUpload(gpu, depth.Get(), depths.data(), kInputWidth * 4, fgRead);
        fgUpload(gpu, motion.Get(), pan.data(), kInputWidth * 4, fgRead);
    }
    FgContext ordered(api, gpu);
    fgFrame(ordered, gpu, 1, depth.Get(), motion.Get());
    for (UINT frame : {2u, 3u}) {
        auto r = fgRecording(gpu);
        auto prepare = fgPrepare(r.list.Get(), frame, depth.Get(), motion.Get(), false);
        fgResult(api.dispatch(&ordered.value, &prepare.header), FFX_API_RETURN_OK, "prepare-ahead-of-matching-generation");
        check(r.list->Close(), "prepare-ahead-close"); gpu.executeAndWait(r.list.Get());
    }
    auto orderedColor = fgColor(gpu, 2), orderedOutput = fgOutput(gpu);
    auto orderedList = fgRecording(gpu);
    auto orderedGeneration = fgGenerate(orderedList.list.Get(), 2, orderedColor.Get(), orderedOutput.Get(), false);
    fgResult(api.dispatch(&ordered.value, &orderedGeneration.header), FFX_API_RETURN_OK, "generate-matching-older-prepare");
    check(orderedList.list->Close(), "generate-older-prepare-close"); gpu.executeAndWait(orderedList.list.Get());
    fgEqual(secondFrame, fgPixels(gpu, orderedOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS), "newer-prepare-does-not-reset-matching-real-pair");
    std::puts("OWNED_D3D12_FSR_FG_PREPARE_ORDER_PASS matching_frame=2 newest_prepare=3 output=exact");
    const FfxApiRect2D rect{7, 11, 96, 88};
    auto pixels = fgFrame(snapshot, gpu, 5, depth.Get(), motion.Get(), false, false, rect);
    const auto preserved = fgScene(5);
    for (UINT y = 0; y < fgHeight; ++y)
        for (UINT x = 0; x < fgWidth; ++x)
            if (x < 7 || x >= 103 || y < 11 || y >= 99)
                for (UINT c = 0; c < 4; ++c) {
                    uint16_t value; std::memcpy(&value, pixels.data() + ((y * fgWidth + x) * 4 + c) * 2, 2);
                    fgRequire(value == preserved[(y * fgWidth + x) * 4 + c], "generation-rect-outside-preserved");
                }
    std::puts("OWNED_D3D12_FSR_FG_DIRECT_PASS snapshot=exact rect_guard=exact frames=4");
    FgContext zeroDelta(api, gpu);
    const auto zeroFrame = fgFrame(zeroDelta, gpu, 6, depth.Get(), motion.Get(), false, false,
                                    {0, 0, fgWidth, fgHeight}, 0, 0.f);
    fgRequire(!zeroFrame.empty(), "zero-frame-time-delta");
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
    auto countPrepare = fgPrepare(negative.list.Get(), 12, depth.Get(), motion.Get(), true);
    fgResult(api.dispatch(&epoch.value, &countPrepare.header), FFX_API_RETURN_OK, "count-prepare");
    for (UINT count : {0u, 5u}) {
        missing.numGeneratedFrames = count;
        fgResult(api.dispatch(&epoch.value, &missing.header), FFX_API_RETURN_ERROR_PARAMETER, "count-outside-public-range");
    }
    for (UINT count : {2u, 3u, 4u}) {
        missing.numGeneratedFrames = count;
        fgResult(api.dispatch(&epoch.value, &missing.header), FFX_API_RETURN_NO_PROVIDER, "native-single-output-capacity");
    }
    check(negative.list->Close(), "negative-close"); gpu.executeAndWait(negative.list.Get());
    fgEqual(fgPixels(gpu, negativeOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS), negativeBefore, "negative-output-unchanged");
    auto retry = fgRecording(gpu); missing.commandList = retry.list.Get(); missing.numGeneratedFrames = 1;
    fgResult(api.dispatch(&epoch.value, &missing.header), FFX_API_RETURN_OK, "count-rejection-preserves-prepare");
    check(retry.list->Close(), "count-retry-close"); gpu.executeAndWait(retry.list.Get());
    std::puts("OWNED_D3D12_FSR_FG_COUNT_REJECTION_PASS invalid=0,5 unsupported=2,3,4 sentinel=exact prepare=retained");
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
void fgGrouped(const FgApi& api, Gpu& gpu) {
    auto depth = fgDepth(gpu), motion = fgMotion(gpu);
    FgContext context(api, gpu);
    fgFrame(context, gpu, 1, depth.Get(), motion.Get(), false, false, {7, 11, 96, 88});
    // Native MetalFX first writes the current image twice before interpolation.
    fgFrame(context, gpu, 2, depth.Get(), motion.Get(), false, false, {7, 11, 96, 88});
    auto color = fgColor(gpu, 3), output = fgOutput(gpu);
    const auto current = fgPixels(gpu, color.Get(), fgRead);
    const auto before = fgPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto r = fgRecording(gpu);
    auto prepare = fgPrepare(r.list.Get(), 3, depth.Get(), motion.Get(), false);
    fgResult(api.dispatch(&context.value, &prepare.header), FFX_API_RETURN_OK, "single-prepare");
    auto generation = fgGenerate(r.list.Get(), 3, color.Get(), output.Get(), false, {7, 11, 96, 88});
    auto invalid = generation; invalid.outputs[0] = generation.presentColor;
    fgResult(api.dispatch(&context.value, &invalid.header), FFX_API_RETURN_ERROR_PARAMETER, "output-present-alias");
    invalid = generation; invalid.outputs[0].resource = nullptr;
    fgResult(api.dispatch(&context.value, &invalid.header), FFX_API_RETURN_ERROR_PARAMETER, "missing-output");
    invalid = generation; invalid.outputs[0].state = FFX_API_RESOURCE_STATE_COPY_SRC | FFX_API_RESOURCE_STATE_COPY_DEST;
    fgResult(api.dispatch(&context.value, &invalid.header), FFX_API_RETURN_ERROR_PARAMETER, "invalid-output-state");
    auto wrongSize = createTexture(gpu.device.Get(), 96, 80, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    invalid = generation; invalid.outputs[0] = ffxApiGetResourceDX12(wrongSize.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    fgResult(api.dispatch(&context.value, &invalid.header), FFX_API_RETURN_ERROR_PARAMETER, "invalid-output-dimensions");
    auto wrongFormat = createTexture(gpu.device.Get(), fgWidth, fgHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    invalid = generation; invalid.outputs[0] = ffxApiGetResourceDX12(wrongFormat.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    fgResult(api.dispatch(&context.value, &invalid.header), FFX_API_RETURN_ERROR_PARAMETER, "invalid-output-format");
    fgResult(api.dispatch(&context.value, &generation.header), FFX_API_RETURN_OK, "single-generate");
    check(r.list->Close(), "single-close"); gpu.executeAndWait(r.list.Get());
    const auto pixels = fgPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    fgRequire(pixels != before && pixels != current, "native-generated-not-sentinel-or-current");
    for (UINT y = 0; y < fgHeight; ++y) for (UINT x = 0; x < fgWidth; ++x)
        if (x < 7 || x >= 103 || y < 11 || y >= 99)
            fgRequire(!std::memcmp(pixels.data() + (y * fgWidth + x) * 8,
                current.data() + (y * fgWidth + x) * 8, 8), "native-cropped-border-real-pixels");
    fgDump(pixels, "fg-grouped-1-1.bin");
    r = fgRecording(gpu);
    prepare = fgPrepare(r.list.Get(), 4, depth.Get(), motion.Get(), false);
    fgResult(api.dispatch(&context.value, &prepare.header), FFX_API_RETURN_OK, "single-reset-prepare");
    generation.commandList = r.list.Get(); generation.frameID = 4; generation.reset = true;
    fgResult(api.dispatch(&context.value, &generation.header), FFX_API_RETURN_OK, "single-reset-generate");
    check(r.list->Close(), "single-reset-close"); gpu.executeAndWait(r.list.Get());
    fgEqual(current, fgPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS), "native-reset-current-output");
    std::puts("OWNED_D3D12_FSR_FG_NATIVE_PASS count=1 distinct_gpu_outputs=1 rect_guard=exact reset_output=current");
}
void fgDebug(const FgApi& api, Gpu& gpu) {
    auto depth = fgDepth(gpu), motion = fgMotion(gpu);
    FgContext control(api, gpu), tear(api, gpu), indicator(api, gpu), view(api, gpu);
    std::vector<uint8_t> debugBaseline;
    for (UINT frame = 1; frame <= 2; ++frame) {
        const auto baseline = fgFrame(control, gpu, frame, depth.Get(), motion.Get());
        const auto lines = fgFrame(tear, gpu, frame, depth.Get(), motion.Get(), false, false,
            {0, 0, fgWidth, fgHeight}, FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES);
        const auto reset = fgFrame(indicator, gpu, frame, depth.Get(), motion.Get(), false, false,
            {0, 0, fgWidth, fgHeight}, FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS);
        // Configure-only flags must use the same immutable per-frame snapshot as HUDLessColor.
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
        config.frameGenerationEnabled = true; config.frameID = frame;
        config.generationRect = {0, 0, fgWidth, fgHeight};
        fgResult(api.configure(&view.value, &config.header), FFX_API_RETURN_OK, "debug-view-configure");
        auto debug = fgFrame(view, gpu, frame, depth.Get(), motion.Get());
        if (frame == 1) {
            fgRequire(lines != baseline, "native-bootstrap-debug-tear-overlay");
            fgRequire(reset != baseline, "native-bootstrap-debug-reset-overlay");
            fgRequire(debug != baseline, "native-bootstrap-debug-view-overlay");
            continue;
        }
        // Diagnostics are full-display GPU outputs; their vendor pixel layout
        // is not a permanent public consumer contract.
        fgRequire(lines != baseline, "debug-tear-affects-valid-generated-pixels");
        fgRequire(reset != baseline, "native-valid-pair-history-indicator-overlay");
        fgRequire(debug != baseline, "debug-view-affects-valid-generated-pixels");
        for (const auto* pixels : std::array<const std::vector<uint8_t>*, 3>{&lines, &reset, &debug})
            for (size_t offset = 6; offset < pixels->size(); offset += 8) {
                uint16_t alpha; std::memcpy(&alpha, pixels->data() + offset, sizeof(alpha));
                fgRequire(alpha == 0x3c00, "debug-generated-opaque-alpha");
            }
        if (frame == 2) debugBaseline = std::move(debug);
    }
    // Independent GPU contexts establish flag-capture baselines without
    // pinning a vendor panel layout or reading production-private wiring.
    FgContext preparedFlags(api, gpu), configuredFlags(api, gpu), cropControl(api, gpu);
    fgFrame(preparedFlags, gpu, 1, depth.Get(), motion.Get());
    fgFrame(configuredFlags, gpu, 1, depth.Get(), motion.Get());
    fgFrame(cropControl, gpu, 1, depth.Get(), motion.Get());
    for (UINT frame : {2u, 3u}) {
        auto r = fgRecording(gpu);
        auto p = fgPrepare(r.list.Get(), frame, depth.Get(), motion.Get(), false);
        p.flags = frame == 2 ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0;
        fgResult(api.dispatch(&preparedFlags.value, &p.header), FFX_API_RETURN_OK, "debug-prepare-per-frame-flags");
        p.flags = 0;
        check(r.list->Close(), "debug-prepare-flags-close"); gpu.executeAndWait(r.list.Get());
    }
    auto preparedColor = fgColor(gpu, 2), preparedOutput = fgOutput(gpu);
    auto preparedList = fgRecording(gpu);
    auto preparedGeneration = fgGenerate(preparedList.list.Get(), 2, preparedColor.Get(), preparedOutput.Get(), false);
    fgResult(api.dispatch(&preparedFlags.value, &preparedGeneration.header), FFX_API_RETURN_OK, "debug-generate-matching-prepare-flags");
    check(preparedList.list->Close(), "debug-prepare-capture-close"); gpu.executeAndWait(preparedList.list.Get());
    fgEqual(debugBaseline, fgPixels(gpu, preparedOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS), "debug-prepare-flags-independent-gpu-exact");
    const FfxApiRect2D capturedRect{7, 11, 96, 88};
    const auto cropBaseline = fgFrame(cropControl, gpu, 2, depth.Get(), motion.Get(), false, false,
        capturedRect, FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW);
    ffxConfigureDescFrameGeneration captured{};
    captured.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    captured.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    captured.frameGenerationEnabled = true; captured.frameID = 2; captured.generationRect = capturedRect;
    fgResult(api.configure(&configuredFlags.value, &captured.header), FFX_API_RETURN_OK, "debug-configure-recorded-flags");
    auto capturedColor = fgColor(gpu, 2), capturedOutput = fgOutput(gpu);
    auto capturedList = fgRecording(gpu);
    auto capturedPrepare = fgPrepare(capturedList.list.Get(), 2, depth.Get(), motion.Get(), false);
    fgResult(api.dispatch(&configuredFlags.value, &capturedPrepare.header), FFX_API_RETURN_OK, "debug-configure-capture-prepare");
    auto capturedGeneration = fgGenerate(capturedList.list.Get(), 2, capturedColor.Get(), capturedOutput.Get(), false, capturedRect);
    fgResult(api.dispatch(&configuredFlags.value, &capturedGeneration.header), FFX_API_RETURN_OK, "debug-configure-capture-generate");
    capturedGeneration.generationRect = {0, 0, fgWidth, fgHeight};
    captured.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    captured.generationRect = capturedGeneration.generationRect;
    fgResult(api.configure(&configuredFlags.value, &captured.header), FFX_API_RETURN_OK, "debug-configure-flags-region-cutover-before-execute");
    check(capturedList.list->Close(), "debug-configure-capture-close"); gpu.executeAndWait(capturedList.list.Get());
    fgEqual(cropBaseline, fgPixels(gpu, capturedOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS), "debug-configure-flags-region-independent-gpu-exact");
    std::puts("OWNED_D3D12_FSR_FG_DEBUG_CAPTURE_PASS prepare=per_frame configure=recorded rect=recorded output=independent_gpu_exact");
    // Flagged outputs never contaminate real-pair history or survive a configure cutover.
    ffxConfigureDescFrameGeneration clear{};
    clear.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    clear.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    clear.frameGenerationEnabled = true; clear.frameID = 3; clear.generationRect = {0, 0, fgWidth, fgHeight};
    fgResult(api.configure(&view.value, &clear.header), FFX_API_RETURN_OK, "debug-view-clear");
    fgEqual(fgFrame(view, gpu, 3, depth.Get(), motion.Get()), fgFrame(control, gpu, 3, depth.Get(), motion.Get()), "debug-history-unmodified");
    auto originalDepth = fgDepth(gpu), originalMotion = fgMotion(gpu);
    const auto unchangedGuides = fgFrame(control, gpu, 4, originalDepth.Get(), originalMotion.Get(), false, false,
        {0, 0, fgWidth, fgHeight}, FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW);
    auto mutate = fgRecording(gpu);
    transition(mutate.list.Get(), depth.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(mutate.list.Get(), motion.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
    check(mutate.list->Close(), "debug-input-change"); gpu.executeAndWait(mutate.list.Get());
    std::vector<float> changedDepth(kInputWidth * kInputHeight, .25f);
    std::vector<uint16_t> changedMotion(kInputWidth * kInputHeight * 2);
    for (UINT i = 0; i < changedMotion.size(); i += 2) changedMotion[i] = 0x3d00; // 1.25 render pixels -> 2.5 display pixels.
    fgUpload(gpu, depth.Get(), changedDepth.data(), kInputWidth * 4, fgRead);
    fgUpload(gpu, motion.Get(), changedMotion.data(), kInputWidth * 4, fgRead);
    const auto changed = fgFrame(view, gpu, 4, depth.Get(), motion.Get(), false, false,
        {0, 0, fgWidth, fgHeight}, FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW);
    fgRequire(changed != unchangedGuides, "debug-guides-actual-gpu-input-dependent");
    FgContext replay(api, gpu);
    fgFrame(replay, gpu, 9, depth.Get(), motion.Get());
    clear.frameID = 10; clear.flags |= 7u;
    fgResult(api.configure(&replay.value, &clear.header), FFX_API_RETURN_OK, "debug-replay-configure");
    auto color = fgColor(gpu, 10), output = fgOutput(gpu);
    auto recorded = fgRecording(gpu);
    auto p = fgPrepare(recorded.list.Get(), 10, depth.Get(), motion.Get(), false);
    fgResult(api.dispatch(&replay.value, &p.header), FFX_API_RETURN_OK, "debug-replay-prepare");
    auto g = fgGenerate(recorded.list.Get(), 10, color.Get(), output.Get(), false, {7, 11, 96, 88});
    fgResult(api.dispatch(&replay.value, &g.header), FFX_API_RETURN_OK, "debug-replay-generate");
    check(recorded.list->Close(), "debug-replay-close");
    clear.frameID = 11; clear.flags &= ~7u;
    fgResult(api.configure(&replay.value, &clear.header), FFX_API_RETURN_OK, "debug-replay-cutover");
    gpu.executeAndWait(recorded.list.Get());
    FgContext replayControl(api, gpu);
    const auto freshReplay = fgFrame(replayControl, gpu, 10, depth.Get(), motion.Get(), true, false, {7, 11, 96, 88}, 7u);
    // Native reset history is authoritative; retained commands still own their inputs.
    replay.destroy(); color.Reset();
    gpu.executeAndWait(recorded.list.Get());
    fgEqual(freshReplay, fgPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS), "debug-recording-replay-fresh-native");
    // Debug diagnostics can override the full display, including outside ROI.
    // Normal crop guards are checked separately without debug flags.
    std::puts("OWNED_D3D12_FSR_FG_DEBUG_PASS tear=gpu_effect reset=history debug=real_inputs configure=frame_snapshot history=unmodified alpha=opaque");
    std::puts("OWNED_D3D12_FSR_FG_DEBUG_REPLAY_PASS combined=1 debug_overlay=full_display replay=fresh_native context_destroy_before_replay=1");
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
    fgGrouped(api, gpu);
    fgDebug(api, gpu);
    fgProxy(api, gpu);
    std::puts("OWNED_D3D12_FSR_FG_PASS");
    return 0;
}
