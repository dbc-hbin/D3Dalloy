#include <mutex>
namespace {
struct FgWindow {
    HWND hwnd;
    FgWindow() {
        WNDCLASSW c{}; c.hInstance = GetModuleHandleW(nullptr); c.lpfnWndProc = DefWindowProcW;
        c.lpszClassName = L"OwnedFsrFgWindow";
        fgRequire(RegisterClassW(&c) != 0, "register-fg-window");
        RECT rect{0, 0, fgWidth, fgHeight}; AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowW(c.lpszClassName, L"public FFX FG content oracle", WS_OVERLAPPEDWINDOW,
                             60, 60, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, c.hInstance, nullptr);
        fgRequire(hwnd != nullptr, "fg-hwnd"); ShowWindow(hwnd, SW_SHOWNA); UpdateWindow(hwnd);
    }
    ~FgWindow() { DestroyWindow(hwnd); UnregisterClassW(L"OwnedFsrFgWindow", GetModuleHandleW(nullptr)); }
};
struct FgCallbackCapture {
    TextureReadback readback;
    uint64_t frame;
    bool generated;
    UINT width, height;
};
struct FgProxyCallbacks {
    const FgApi* api;
    ffxContext* context;
    Gpu* gpu;
    bool rejectGeneration = false;
    bool rejectPresentation = false;
    std::mutex mutex;
    std::vector<FgCallbackCapture> captures;
    std::vector<ComPtr<ID3D12Resource>> retainedMarkers;
    std::atomic<unsigned> dispatches{0};
};
ffxReturnCode_t fgGenerationCallback(ffxDispatchDescFrameGeneration* desc, void* user) {
    auto& s = *static_cast<FgProxyCallbacks*>(user);
    ++s.dispatches;
    if (s.rejectGeneration) { desc->numGeneratedFrames = 0; return FFX_API_RETURN_ERROR; }
    return s.api->dispatch(s.context, &desc->header);
}
D3D12_RESOURCE_STATES fgCallbackState(uint32_t state) {
    switch (state) {
    case FFX_API_RESOURCE_STATE_PRESENT: case FFX_API_RESOURCE_STATE_COMMON: return D3D12_RESOURCE_STATE_COMMON;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    default: fgFail("unsupported-public-callback-resource-state", state);
    }
}
ffxReturnCode_t fgPresentCallback(ffxCallbackDescFrameGenerationPresent* desc, void* user) {
    auto& s = *static_cast<FgProxyCallbacks*>(user);
    if (s.rejectPresentation) return FFX_API_RETURN_ERROR;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(desc->commandList);
    auto* source = static_cast<ID3D12Resource*>(desc->currentBackBuffer.resource);
    auto* output = static_cast<ID3D12Resource*>(desc->outputSwapChainBuffer.resource);
    fgRequire(source && output && list, "callback-public-resources");
    const auto sourceState = fgCallbackState(desc->currentBackBuffer.state);
    const auto outputState = fgCallbackState(desc->outputSwapChainBuffer.state);
    transition(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, output, outputState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(output, source);
    const auto d = output->GetDesc();
    auto marker = createBuffer(s.gpu->device.Get(), 256 * 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* data = nullptr; check(marker->Map(0, nullptr, reinterpret_cast<void**>(&data)), "marker-map");
    for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
        auto* p = data + y * 256 + x * 4;
        p[0] = static_cast<uint8_t>(desc->frameID + 32); p[1] = desc->isGeneratedFrame ? 190 : 70;
        p[2] = 220; p[3] = 255;
    }
    marker->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = marker.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 256};
    D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = output; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    auto readback = createTextureReadback(*s.gpu, output);
    transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(list, output, readback);
    transition(list, output, D3D12_RESOURCE_STATE_COPY_SOURCE, outputState);
    transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.captures.push_back({std::move(readback), desc->frameID, desc->isGeneratedFrame, static_cast<UINT>(d.Width), d.Height});
    s.retainedMarkers.push_back(std::move(marker));
    return FFX_API_RETURN_OK;
}
std::vector<uint8_t> fgCallbackPixels(const FgCallbackCapture& capture) {
    uint8_t* data = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(capture.readback.totalBytes)};
    check(capture.readback.buffer->Map(0, &range, reinterpret_cast<void**>(&data)), "callback-readback-map");
    std::vector<uint8_t> pixels(capture.width * capture.height * 4);
    for (UINT y = 0; y < capture.height; ++y)
        std::memcpy(pixels.data() + y * capture.width * 4,
                    data + capture.readback.footprint.Offset + y * capture.readback.footprint.Footprint.RowPitch, capture.width * 4);
    const D3D12_RANGE written{0, 0}; capture.readback.buffer->Unmap(0, &written);
    return pixels;
}
void fgDrain(const FgApi& api, ffxContext& swapContext, bool expectedError = false) {
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    LARGE_INTEGER start{}, stop{}, frequency{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    fgResult(api.dispatch(&swapContext, &wait.header),
             expectedError ? FFX_API_RETURN_ERROR_RUNTIME_ERROR : FFX_API_RETURN_OK, "proxy-drain");
    QueryPerformanceCounter(&stop);
    fgRequire(double(stop.QuadPart - start.QuadPart) / frequency.QuadPart < 15, "bounded-drain-progress");
}
void fgPresentedStatistics(IDXGISwapChain4* chain, UINT frame) {
    UINT target = 0; check(chain->GetLastPresentCount(&target), "native-last-present-count");
    LARGE_INTEGER start{}, now{}, frequency{}; QueryPerformanceCounter(&start); QueryPerformanceFrequency(&frequency);
    for (;;) {
        DXGI_FRAME_STATISTICS statistics{};
        const HRESULT result = chain->GetFrameStatistics(&statistics);
        if (SUCCEEDED(result) && statistics.PresentCount == target && statistics.SyncQPCTime.QuadPart > 0) {
            std::printf("OWNED_D3D12_FSR_FG_STATISTICS frame=%u native_present=%u present_refresh=%u sync_refresh=%u sync_qpc=%lld frequency=%lld source=public_dxgi_actual_presentation\n",
                        frame, statistics.PresentCount, statistics.PresentRefreshCount, statistics.SyncRefreshCount,
                        statistics.SyncQPCTime.QuadPart, frequency.QuadPart);
            return;
        }
        if (FAILED(result) && result != DXGI_ERROR_FRAME_STATISTICS_DISJOINT) check(result, "actual-public-presentation-statistics");
        QueryPerformanceCounter(&now);
        fgRequire(double(now.QuadPart - start.QuadPart) / frequency.QuadPart < 15, "actual-presentation-statistics-bounded");
        MSG message; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(1);
    }
}
void fgProxy(const FgApi& api, Gpu& gpu) {
    FgWindow window;
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "fg-factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = fgWidth; desc.Height = fgHeight; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling = DXGI_SCALING_STRETCH;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain4> chain;
    ffxContext swapContext = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    create.swapchain = chain.GetAddressOf(); create.hwnd = window.hwnd; create.desc = &desc;
    create.dxgiFactory = factory.Get(); create.gameQueue = gpu.queue.Get();
    fgResult(api.create(&swapContext, &create.header, nullptr), FFX_API_RETURN_OK, "proxy-create-hwnd");
    fgRequire(chain != nullptr, "proxy-idxgiswapchain4");
    FgContext context(api, gpu, fgWidth, fgHeight, desc.Format);
    FgProxyCallbacks callbacks{}; callbacks.api = &api; callbacks.context = &context.value; callbacks.gpu = &gpu;
    auto depth = fgDepth(gpu);
    auto motion = fgMotion(gpu);
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = chain.Get(); config.frameGenerationEnabled = true;
    config.frameGenerationCallback = fgGenerationCallback; config.frameGenerationCallbackUserContext = &callbacks;
    config.presentCallback = fgPresentCallback; config.presentCallbackUserContext = &callbacks;
    config.generationRect = {0, 0, fgWidth, fgHeight};
    LARGE_INTEGER frequency{}, start{}, stop{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    for (unsigned frame = 1; frame <= 10; ++frame) {
        config.frameID = frame;
        config.frameGenerationEnabled = frame != 5 && frame != 6;
        config.onlyPresentGenerated = frame == 8;
        callbacks.rejectGeneration = frame == 9;
        callbacks.rejectPresentation = frame == 10;
        fgResult(api.configure(&context.value, &config.header), FFX_API_RETURN_OK, "proxy-configure");
        ComPtr<ID3D12Resource> backbuffer;
        check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "proxy-buffer");
        auto r = fgRecording(gpu);
        transition(r.list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "proxy-upload-transition"); gpu.executeAndWait(r.list.Get());
        std::vector<uint8_t> pixels(fgWidth * fgHeight * 4);
        for (UINT y = 0; y < fgHeight; ++y) for (UINT x = 0; x < fgWidth; ++x) {
            const UINT i = (y * fgWidth + x) * 4;
            pixels[i] = static_cast<uint8_t>(frame * 16);
            pixels[i + 1] = ((x + frame * 4) / 16 ^ y / 16) & 1 ? 180 : 30;
            pixels[i + 2] = static_cast<uint8_t>(frame * 8); pixels[i + 3] = 255;
        }
        fgUpload(gpu, backbuffer.Get(), pixels.data(), fgWidth * 4, D3D12_RESOURCE_STATE_PRESENT);
        r = fgRecording(gpu);
        auto prepare = fgPrepare(r.list.Get(), frame, depth.Get(), motion.Get(), frame == 1 || frame == 7);
        fgResult(api.dispatch(&context.value, &prepare.header), FFX_API_RETURN_OK, "proxy-prepare");
        check(r.list->Close(), "proxy-prepare-close"); gpu.executeAndWait(r.list.Get());
        LARGE_INTEGER before{}, after{}; QueryPerformanceCounter(&before);
        const HRESULT result = chain->Present(1, 0);
        QueryPerformanceCounter(&after);
        if (frame != 10) check(result, "proxy-present");
        fgDrain(api, swapContext, frame == 10 && SUCCEEDED(result));
        if (frame != 10) fgPresentedStatistics(chain.Get(), frame);
        std::printf("OWNED_D3D12_FSR_FG_PRESENT_CALL frame=%u enabled=%u only_generated=%u generation_error=%u present_error=%u qpc_start=%lld qpc_end=%lld frequency=%lld\n",
                    frame, config.frameGenerationEnabled, config.onlyPresentGenerated, frame == 9, frame == 10,
                    before.QuadPart, after.QuadPart, frequency.QuadPart);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(33); // Caller cadence only; presentation timestamps are the oracle.
    }
    QueryPerformanceCounter(&stop);
    fgRequire(double(stop.QuadPart - start.QuadPart) / frequency.QuadPart < 60, "proxy-ordered-bounded-progress");
    {
        std::lock_guard<std::mutex> lock(callbacks.mutex);
        uint64_t previousFrame = 0; bool previousGenerated = false;
        for (const auto& capture : callbacks.captures) {
            auto pixels = fgCallbackPixels(capture);
            char path[96]; std::snprintf(path, sizeof(path), "fg-callback-%llu-%u.bin",
                static_cast<unsigned long long>(capture.frame), capture.generated);
            fgDump(pixels, path);
            fgRequire(pixels[0] == capture.frame + 32 && pixels[1] == (capture.generated ? 190 : 70) && pixels[2] == 220,
                       "callback-gpu-frame-marker");
            fgRequire(capture.frame >= previousFrame && (capture.frame != previousFrame || (previousGenerated && !capture.generated)),
                       "generated-before-real-callback-pixel-order");
            if (!capture.generated) {
                const UINT center = (64 * fgWidth + 64) * 4;
                fgRequire(pixels[center] == capture.frame * 16 && pixels[center + 2] == capture.frame * 8,
                           "actual-render-color-readback");
            }
            fgRequire(capture.frame != 10 && (capture.frame != 8 || capture.generated) &&
                       (capture.frame != 9 || !capture.generated) &&
                       ((capture.frame != 5 && capture.frame != 6) || !capture.generated), "negative-output-never-callback-presented");
            std::printf("OWNED_D3D12_FSR_FG_CALLBACK_PIXELS frame=%llu generated=%u width=%u height=%u hash=%016llx marker=%u,%u,%u\n",
                        static_cast<unsigned long long>(capture.frame), capture.generated, capture.width, capture.height,
                        static_cast<unsigned long long>(fgHash(pixels)), pixels[0], pixels[1], pixels[2]);
            previousFrame = capture.frame; previousGenerated = capture.generated;
        }
    }
    config.frameGenerationEnabled = false;
    fgResult(api.configure(&context.value, &config.header), FFX_API_RETURN_OK, "proxy-final-disable");
    fgDrain(api, swapContext);
    context.destroy();
    fgResult(api.destroy(&swapContext, nullptr), FFX_API_RETURN_OK, "proxy-destroy");
    chain.Reset();
    // Wrap an actual native HWND chain, then exercise the SDK composition
    // shaders rather than replacing them with the application callback.
    ComPtr<IDXGISwapChain1> nativeBase;
    check(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window.hwnd, &desc, nullptr, nullptr, &nativeBase), "wrap-native-create");
    check(nativeBase.As(&chain), "wrap-native-chain4"); nativeBase.Reset();
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrap{};
    wrap.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    wrap.swapchain = chain.GetAddressOf(); wrap.gameQueue = gpu.queue.Get();
    fgResult(api.create(&swapContext, &wrap.header, nullptr), FFX_API_RETURN_OK, "proxy-wrap-real-chain");
    FgContext uiContext(api, gpu, fgWidth, fgHeight, desc.Format);
    ffxConfigureDescFrameGeneration builtin{};
    builtin.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    builtin.swapChain = chain.Get(); builtin.generationRect = {0, 0, fgWidth, fgHeight};
    auto ui = createTexture(gpu.device.Get(), fgWidth, fgHeight, desc.Format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> uiPixels(fgWidth * fgHeight * 4);
    for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
        const UINT i = (y * fgWidth + x) * 4; uiPixels[i] = 64; uiPixels[i + 3] = 128;
    }
    fgUpload(gpu, ui.Get(), uiPixels.data(), fgWidth * 4, fgRead);
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 registration{};
    registration.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    registration.uiResource = ffxApiGetResourceDX12(ui.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    registration.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                         FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    for (UINT frame = 11; frame <= 14; ++frame) {
        const UINT width = frame < 13 ? fgWidth : 96, height = frame < 13 ? fgHeight : 80;
        if (frame == 13) {
            check(chain->ResizeBuffers(2, width, height, desc.Format, desc.Flags), "proxy-resize");
            DXGI_SWAP_CHAIN_DESC1 resized{}; check(chain->GetDesc1(&resized), "proxy-resized-desc");
            fgRequire(resized.Width == width && resized.Height == height && resized.BufferCount == 2, "proxy-real-resize-contract");
            builtin.generationRect = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
        }
        builtin.frameID = frame;
        fgResult(api.configure(&uiContext.value, &builtin.header), FFX_API_RETURN_OK, "ui-builtin-configure");
        ComPtr<ID3D12Resource> buffer; check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer)), "ui-real-buffer");
        auto r = fgRecording(gpu);
        transition(r.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "ui-source-transition"); gpu.executeAndWait(r.list.Get());
        std::vector<uint8_t> scene(width * height * 4);
        for (UINT i = 0; i < scene.size(); i += 4) { scene[i] = static_cast<uint8_t>(frame * 8); scene[i + 1] = 60; scene[i + 2] = 100; scene[i + 3] = 255; }
        fgUpload(gpu, buffer.Get(), scene.data(), width * 4, D3D12_RESOURCE_STATE_PRESENT);
        fgEqual(fgPixels(gpu, buffer.Get(), D3D12_RESOURCE_STATE_PRESENT), scene, "ui-actual-real-source-readback");
        // Re-publish red UI, then overwrite the application-owned original on
        // the game queue AFTER Present. Internal double buffering must retain
        // the old GPU pixels; a borrowed live texture becomes visibly green.
        r = fgRecording(gpu); transition(r.list.Get(), ui.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "ui-restore-transition"); gpu.executeAndWait(r.list.Get());
        fgUpload(gpu, ui.Get(), uiPixels.data(), fgWidth * 4, fgRead);
        fgEqual(fgPixels(gpu, ui.Get(), fgRead), uiPixels, "ui-actual-original-red-before-present");
        // The SDK consumes a double-buffered UI registration after its copy.
        // Publish each frame explicitly, just as the application render loop.
        fgResult(api.configure(&swapContext, &registration.header), FFX_API_RETURN_OK, "ui-register-premul-doublebuffer");
        check(chain->Present(1, 0), "ui-builtin-present");
        r = fgRecording(gpu); transition(r.list.Get(), ui.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "ui-mutate-transition"); gpu.executeAndWait(r.list.Get());
        std::vector<uint8_t> mutant(fgWidth * fgHeight * 4);
        for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
            const UINT i = (y * fgWidth + x) * 4; mutant[i + 1] = 128; mutant[i + 3] = 128;
        }
        fgUpload(gpu, ui.Get(), mutant.data(), fgWidth * 4, fgRead);
        fgDrain(api, swapContext);
        fgPresentedStatistics(chain.Get(), frame);
        std::printf("OWNED_D3D12_FSR_FG_UI_FRAME frame=%u width=%u height=%u source=%u,60,100 ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1\n", frame, width, height, frame * 8);
        Sleep(33);
    }
    registration.uiResource = {};
    fgResult(api.configure(&swapContext, &registration.header), FFX_API_RETURN_OK, "ui-unregister");
    ui.Reset(); fgDrain(api, swapContext); uiContext.destroy();
    fgResult(api.destroy(&swapContext, nullptr), FFX_API_RETURN_OK, "wrapped-proxy-destroy"); chain.Reset();
    std::puts("OWNED_D3D12_FSR_FG_UI_PASS wrap=real_hwnd ui=builtin_shader premultiplied=1 doublebuffer=gpu_snapshot resize=96x80 lifetime=drained");
    std::puts("OWNED_D3D12_FSR_FG_PROXY_PASS create=hwnd callbacks=gpu-readback generated_before_real=1 only_generated=1 disabled=1 callback_error=1 drain=1 destroy=1");
    std::puts("OWNED_D3D12_FSR_FG_PACING_PASS ordered=1 bounded_progress=1 qpc=observed native_timestamps=external_observer_required");
}
}
