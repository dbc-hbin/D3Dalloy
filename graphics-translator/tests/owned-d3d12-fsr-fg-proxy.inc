#include <mutex>
#include <thread>
#include "owned-fg-present.h"
namespace {
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
    HANDLE recorded = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~FgProxyCallbacks() { CloseHandle(recorded); }
};
ffxReturnCode_t fgGenerationCallback(ffxDispatchDescFrameGeneration* desc, void* user) {
    auto& s = *static_cast<FgProxyCallbacks*>(user);
    ++s.dispatches;
    if (s.rejectGeneration) { desc->numGeneratedFrames = 0; return FFX_API_RETURN_ERROR; }
    desc->numGeneratedFrames = desc->frameID == 3 ? 2 : desc->frameID == 4 ? 3 : desc->frameID == 8 ? 4 : 1;
    desc->reset |= desc->frameID == 12;
    for (UINT i = 0; i < desc->numGeneratedFrames; ++i) {
        fgRequire(desc->outputs[i].resource, "callback-multi-output-resource");
        for (UINT j = 0; j < i; ++j)
            fgRequire(desc->outputs[i].resource != desc->outputs[j].resource, "callback-multi-output-nonalias");
    }
    const auto result = s.api->dispatch(s.context, &desc->header);
    if (desc->frameID == 4 && result == FFX_API_RETURN_OK) {
        // The recorded batch owns the accepted identities. The application's
        // descriptor storage is not a completion/result channel.
        desc->outputs[0] = desc->outputs[2];
        desc->outputs[1] = desc->outputs[2];
    }
    return result;
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
    void* untouched = nullptr;
    check(readback.buffer->Map(0, nullptr, &untouched), "callback-pending-map");
    std::memset(untouched, 0xa7, static_cast<size_t>(readback.totalBytes));
    readback.buffer->Unmap(0, nullptr);
    transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(list, output, readback);
    transition(list, output, D3D12_RESOURCE_STATE_COPY_SOURCE, outputState);
    transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.captures.push_back({std::move(readback), desc->frameID, desc->isGeneratedFrame, static_cast<UINT>(d.Width), d.Height});
    s.retainedMarkers.push_back(std::move(marker));
    SetEvent(s.recorded);
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
void fgPresentedStatistics(IDXGISwapChain4* chain, UINT frame, DXGI_FRAME_STATISTICS& previous, const char* stream = nullptr, bool resizedEpoch = false) {
    if (stream) {
        ComPtr<IDXGIOutput> output;
        check(chain->GetContainingOutput(&output), "fullstream-containing-output");
        for (UINT vblank = 0; vblank < 3; ++vblank)
            check(output->WaitForVBlank(), "fullstream-output-vblank");
    }
    const auto observe = [&](bool epoch) {
        const FgActualPresentationObservation observation = fgActualPresentation(chain);
        LARGE_INTEGER observedEnd{}; QueryPerformanceCounter(&observedEnd);
        const auto& statistics = observation.statistics;
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        if (stream)
            std::printf("OWNED_D3D12_FSR_FG_%s stream=%s output_vblanks=3%s",
                        epoch ? "EPOCH_STATISTICS" : "FINAL_STATISTICS", stream, epoch ? " boundary=resize" : "");
        else
            std::printf("OWNED_D3D12_FSR_FG_STATISTICS frame=%u", frame);
        std::printf(" statistics=%s result=0x%08lx submitted_present=%u native_present=%u present_refresh=%u sync_refresh=%u sync_qpc=%lld qpc_end=%lld frequency=%lld source=public_dxgi_actual_presentation\n",
                    observation.result == S_OK ? "actual" : "disjoint", static_cast<unsigned long>(observation.result),
                    observation.submitted, statistics.PresentCount, statistics.PresentRefreshCount, statistics.SyncRefreshCount,
                    statistics.SyncQPCTime.QuadPart, observedEnd.QuadPart, frequency.QuadPart);
        if (observation.result == S_OK) {
            const bool nondecreasing = statistics.PresentCount >= previous.PresentCount &&
                statistics.SyncQPCTime.QuadPart >= previous.SyncQPCTime.QuadPart &&
                statistics.PresentRefreshCount >= previous.PresentRefreshCount &&
                statistics.SyncRefreshCount >= previous.SyncRefreshCount;
            if (!nondecreasing)
                std::fprintf(stderr, "OWNED_D3D12_FSR_FG_STATISTICS_REGRESSION frame=%u previous_native_present=%u previous_present_refresh=%u previous_sync_refresh=%u previous_sync_qpc=%lld\n",
                             frame, previous.PresentCount, previous.PresentRefreshCount, previous.SyncRefreshCount, previous.SyncQPCTime.QuadPart);
            fgRequire(nondecreasing, "actual-native-observations-nondecreasing");
            previous = statistics;
        }
        return observation.result;
    };
    // Resize can defer its one disjoint notification until actual data exists.
    // Observe that epoch once, then require a measured final sample, never poll.
    if (resizedEpoch) observe(true);
    const HRESULT result = observe(false);
    if (stream) fgRequire(result == S_OK, "fullstream-actual-positive-presentation-clock");
}
struct FgQueueGate {
    IDXGISwapChain4* chain;
    ComPtr<ID3D12Fence> gate, reached, progress;
    HANDLE reachedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE progressEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 serial = 0;
    UINT previousSubmittedCount = 0;
    FgQueueGate(Gpu& gpu, IDXGISwapChain4* chain) : chain(chain) {
        check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "queue-gate-fence");
        check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&reached)), "queue-blocked-fence");
        check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&progress)), "queue-idle-fence");
        fgRequire(reachedEvent && progressEvent, "queue-proof-events");
    }
    ~FgQueueGate() { CloseHandle(reachedEvent); CloseHandle(progressEvent); }
    void begin(UINT frame, ID3D12CommandQueue* selected, ID3D12CommandQueue* idle) {
        ++serial;
        check(chain->GetLastPresentCount(&previousSubmittedCount), "queue-before-native-count");
        check(selected->Wait(gate.Get(), serial), "queue-block-selected");
        check(selected->Signal(reached.Get(), serial), "queue-blocked-progress");
        check(idle->Signal(progress.Get(), serial), "queue-idle-progress");
        check(reached->SetEventOnCompletion(serial, reachedEvent), "queue-blocked-event");
        check(progress->SetEventOnCompletion(serial, progressEvent), "queue-idle-event");
        std::printf("OWNED_D3D12_FSR_FG_QUEUE_GATE frame=%u serial=%llu\n", frame, static_cast<unsigned long long>(serial));
    }
    void release(UINT frame, FgProxyCallbacks* callbacks = nullptr, bool realOnly = false) {
        fgRequire(WaitForSingleObject(progressEvent, 10000) == WAIT_OBJECT_0, "queue-idle-worker-not-blocked");
        if (callbacks)
            fgRequire(WaitForSingleObject(callbacks->recorded, 10000) == WAIT_OBJECT_0, "queue-callback-recorded-while-gated");
        fgRequire(WaitForSingleObject(reachedEvent, 100) == WAIT_TIMEOUT, "queue-selected-remains-blocked");
        if (callbacks) {
            std::lock_guard<std::mutex> lock(callbacks->mutex);
            const auto capture = std::find_if(callbacks->captures.begin(), callbacks->captures.end(),
                [frame](const auto& entry) { return entry.frame == frame; });
            fgRequire(capture != callbacks->captures.end(), "queue-gated-callback-found");
            if (realOnly)
                fgRequire(!capture->generated && capture + 1 == callbacks->captures.end(), "reject-generation-real-only-recorded");
            uint8_t* bytes = nullptr;
            check(capture->readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&bytes)), "queue-gated-readback-map");
            fgRequire(bytes[capture->readback.footprint.Offset] == 0xa7,
                      realOnly ? "reject-generation-real-gpu-output-waits-selected-presentation-queue" : "queue-composition-not-on-substitute-queue");
            capture->readback.buffer->Unmap(0, nullptr);
        }
        UINT submitted = 0;
        check(chain->GetLastPresentCount(&submitted), "queue-native-submitted-before-release");
        // Queue Wait blocks GPU execution, not CPU admission of Present calls.
        fgRequire(submitted >= previousSubmittedCount, "queue-public-present-call-count-nonregression");
        DXGI_FRAME_STATISTICS statistics{};
        const HRESULT statsResult = chain->GetFrameStatistics(&statistics);
        fgRequire(statsResult == DXGI_ERROR_FRAME_STATISTICS_DISJOINT ||
                  (SUCCEEDED(statsResult) && statistics.PresentCount <= previousSubmittedCount), "queue-actual-display-bounded-by-prior-calls");
        check(gate->Signal(serial), "queue-release-selected");
        fgRequire(WaitForSingleObject(reachedEvent, 10000) == WAIT_OBJECT_0, "queue-selected-progress-after-release");
        std::printf("OWNED_D3D12_FSR_FG_QUEUE_PROOF frame=%u blocked=selected idle=progress composition=%s submitted_before=%u submitted_during=%u counter_domain=public_present_calls\n",
                    frame, callbacks ? "pending_marker" : "builtin_ui", previousSubmittedCount, submitted);
    }
};
void fgProxy(const FgApi& api, Gpu& gpu) {
    FgPresentWindow window(fgWidth, fgHeight, L"public FFX FG content oracle");
    // Separate paired ABI consumers without changing focus/z-order. Native
    // acquisition/presented metadata, not placement alone, proves visibility.
    RECT work{}, outer{}, client{};
    fgRequire(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && GetWindowRect(window.hwnd, &outer), "proxy-window-workarea");
    const LONG outerWidth = outer.right - outer.left, outerHeight = outer.bottom - outer.top;
    fgRequire(work.right - work.left >= outerWidth + 8 && work.bottom - work.top >= 2 * outerHeight + 16,
              "proxy-workarea-fits-disjoint-abi-windows");
    const LONG x = work.right - outerWidth - 8;
    const LONG y = std::clamp(work.top + 96, work.top, work.bottom - 2 * outerHeight - 16) +
                   (sizeof(void*) == 4 ? outerHeight + 16 : 0);
    fgRequire(x >= work.left && y >= work.top && x + outerWidth <= work.right && y + outerHeight <= work.bottom,
              "proxy-window-fully-within-workarea");
    fgRequire(SetWindowPos(window.hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) &&
              GetWindowRect(window.hwnd, &outer) && GetClientRect(window.hwnd, &client), "proxy-window-controlled-position");
    POINT origin{client.left, client.top};
    fgRequire(ClientToScreen(window.hwnd, &origin) && IsWindowVisible(window.hwnd) && !IsIconic(window.hwnd), "proxy-window-visible-client");
    std::printf("OWNED_D3D12_FSR_FG_WINDOW abi=%s window=%ld,%ld,%ld,%ld client=%ld,%ld,%ld,%ld workarea=%ld,%ld,%ld,%ld visible=1 iconic=0\n",
                sizeof(void*) == 8 ? "x64" : "x86", outer.left, outer.top, outer.right, outer.bottom,
                origin.x, origin.y, origin.x + client.right - client.left, origin.y + client.bottom - client.top,
                work.left, work.top, work.right, work.bottom);
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
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> queueOwnerA, queueOwnerB, computeQueue;
    check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queueOwnerA)), "proxy-queue-a");
    check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queueOwnerB)), "proxy-queue-b");
    auto* queueA = queueOwnerA.Get(); auto* queueB = queueOwnerB.Get();
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&computeQueue)), "proxy-invalid-compute");
    Gpu foreign;
    UINT masks[3] = {0, 1, 0};
    IUnknown* sameQueues[2] = {gpu.queue.Get(), gpu.queue.Get()};
    check(chain->ResizeBuffers1(2, fgWidth, fgHeight, DXGI_FORMAT_UNKNOWN, desc.Flags, masks, sameQueues), "proxy-same-queue-resize1");
    check(chain->ResizeBuffers1(0, fgWidth, fgHeight, DXGI_FORMAT_UNKNOWN, desc.Flags, masks, sameQueues), "proxy-count-zero-resize1");
    IUnknown* queues[3] = {queueA, queueB, queueA};
    unsigned rejected = 0;
    auto rejectResize = [&](UINT count, DXGI_FORMAT format, const UINT* nodes, IUnknown* const* supplied, const char* name) {
        const UINT oldIndex = chain->GetCurrentBackBufferIndex();
        DXGI_SWAP_CHAIN_DESC1 oldDesc{}, after{};
        check(chain->GetDesc1(&oldDesc), "proxy-before-rejected-resize-desc");
        fgRequire(chain->ResizeBuffers1(count, 96, 80, format, desc.Flags, nodes, supplied) == DXGI_ERROR_INVALID_CALL, name);
        check(chain->GetDesc1(&after), "proxy-after-rejected-resize-desc");
        fgRequire(after.Width == oldDesc.Width && after.Height == oldDesc.Height && after.Format == oldDesc.Format &&
                  after.BufferCount == oldDesc.BufferCount && chain->GetCurrentBackBufferIndex() == oldIndex, "proxy-atomic-rejected-resize");
        ++rejected;
    };
    rejectResize(2, DXGI_FORMAT_UNKNOWN, nullptr, sameQueues, "proxy-null-masks");
    rejectResize(2, DXGI_FORMAT_UNKNOWN, masks, nullptr, "proxy-null-queues");
    IUnknown* badQueues[2] = {queueA, computeQueue.Get()};
    rejectResize(2, DXGI_FORMAT_UNKNOWN, masks, badQueues, "proxy-compute-queue");
    badQueues[1] = foreign.queue.Get();
    rejectResize(2, DXGI_FORMAT_UNKNOWN, masks, badQueues, "proxy-foreign-device");
    badQueues[1] = gpu.device.Get();
    rejectResize(2, DXGI_FORMAT_UNKNOWN, masks, badQueues, "proxy-not-queue");
    badQueues[1] = nullptr;
    rejectResize(2, DXGI_FORMAT_UNKNOWN, masks, badQueues, "proxy-null-queue-entry");
    UINT badMasks[2] = {0, 2};
    rejectResize(2, DXGI_FORMAT_UNKNOWN, badMasks, sameQueues, "proxy-node-mask");
    rejectResize(1, DXGI_FORMAT_UNKNOWN, masks, sameQueues, "proxy-single-buffer");
    ComPtr<ID3D12Resource> heldBuffer;
    check(chain->GetBuffer(0, IID_PPV_ARGS(&heldBuffer)), "proxy-held-buffer");
    auto* oldBuffer = heldBuffer.Get();
    rejectResize(2, DXGI_FORMAT_UNKNOWN, masks, sameQueues, "proxy-held-reference");
    heldBuffer.Reset();
    rejectResize(2, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, masks, sameQueues, "proxy-backend-format-rollback");
    check(chain->GetBuffer(0, IID_PPV_ARGS(&heldBuffer)), "proxy-retained-buffer-after-failure");
    fgRequire(heldBuffer.Get() == oldBuffer, "proxy-oldframe-identity-after-rollback");
    heldBuffer.Reset();
    check(chain->ResizeBuffers1(3, fgWidth, fgHeight, DXGI_FORMAT_UNKNOWN, desc.Flags, masks, queues), "proxy-multipqueue-resize1");
    check(chain->ResizeBuffers1(0, fgWidth, fgHeight, DXGI_FORMAT_UNKNOWN, desc.Flags, masks, queues), "proxy-multipqueue-count-zero");
    queueOwnerA.Reset(); queueOwnerB.Reset(); // Only backend/proxy COM owners remain.
    FgQueueGate queueGate(gpu, chain.Get());
    ComPtr<ID3D12Fence> rejectedGenerationGate, rejectedGameReached;
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rejectedGenerationGate)), "reject-generation-game-gate");
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rejectedGameReached)), "reject-generation-game-progress");
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
    DXGI_FRAME_STATISTICS previousStatistics{};
    LARGE_INTEGER frequency{}, start{}, stop{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
    for (unsigned frame = 1; frame <= 12; ++frame) {
        config.frameID = frame;
        config.frameGenerationEnabled = frame != 5 && frame != 6;
        config.onlyPresentGenerated = frame == 8 || frame == 12;
        callbacks.rejectGeneration = frame == 9;
        callbacks.rejectPresentation = frame == 10;
        fgResult(api.configure(&context.value, &config.header), FFX_API_RETURN_OK, "proxy-configure");
        if (frame == 3) {
            auto noNotify = config;
            noNotify.flags |= FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
            fgResult(api.configure(&context.value, &noNotify.header), FFX_API_RETURN_OK, "proxy-attached-no-notify-retains-result-consumer");
        }
        ComPtr<ID3D12Resource> backbuffer;
        check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "proxy-buffer");
        auto r = fgRecording(gpu);
        transition(r.list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "proxy-upload-transition"); gpu.executeAndWait(r.list.Get());
        std::vector<uint8_t> pixels(fgWidth * fgHeight * 4);
        for (UINT y = 0; y < fgHeight; ++y) for (UINT x = 0; x < fgWidth; ++x) {
            const UINT i = (y * fgWidth + x) * 4;
            pixels[i] = frame == 11 ? 255 : 32;
            pixels[i + 1] = ((x + frame * 4) / 16 ^ y / 16) & 1 ? 180 : 30;
            pixels[i + 2] = frame == 11 ? 255 : 8; pixels[i + 3] = 255;
        }
        fgUpload(gpu, backbuffer.Get(), pixels.data(), fgWidth * 4, D3D12_RESOURCE_STATE_PRESENT);
        r = fgRecording(gpu);
        auto prepare = fgPrepare(r.list.Get(), frame, depth.Get(), motion.Get(), frame == 1 || frame == 7 || frame == 12);
        fgResult(api.dispatch(&context.value, &prepare.header), FFX_API_RETURN_OK, "proxy-prepare");
        check(r.list->Close(), "proxy-prepare-close"); gpu.executeAndWait(r.list.Get());
        if (frame == 2) {
            void* publicRecording = nullptr;
            ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12 command{};
            command.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12;
            command.pOutCommandList = &publicRecording;
            fgResult(api.query(&swapContext, &command.header), FFX_API_RETURN_OK, "proxy-public-interpolation-recording");
            fgRequire(publicRecording != nullptr, "proxy-public-recording-present");
            FfxApiResource output{};
            ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12 texture{};
            texture.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12;
            texture.pOutTexture = &output;
            fgResult(api.query(&swapContext, &texture.header), FFX_API_RETURN_OK, "proxy-public-interpolation-output");
            auto* list = static_cast<ID3D12GraphicsCommandList*>(publicRecording);
            auto generation = fgGenerate(list, frame, backbuffer.Get(), static_cast<ID3D12Resource*>(output.resource), false);
            generation.presentColor.state = FFX_API_RESOURCE_STATE_PRESENT;
            generation.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            fgResult(api.dispatch(&context.value, &generation.header), FFX_API_RETURN_OK, "proxy-record-generation-before-rejected-resize");
            backbuffer.Reset();
            rejectResize(3, DXGI_FORMAT_UNKNOWN, masks, queues, "proxy-pending-recording-resize-rejected");
            std::puts("OWNED_D3D12_FSR_FG_RECORDING_ROLLBACK frame=2 preserved=1 consumed_by=public_present_gpu_output");
        }
        LARGE_INTEGER before{}, after{}; QueryPerformanceCounter(&before);
        const bool gated = frame == 1 || frame == 3 || frame == 5 || frame == 9;
        if (gated) ResetEvent(callbacks.recorded);
        if (frame == 9) {
            check(gpu.queue->Wait(rejectedGenerationGate.Get(), 1), "reject-generation-block-current-game-work");
            check(gpu.queue->Signal(rejectedGameReached.Get(), 1), "reject-generation-queued-game-progress");
        }
        if (gated) {
            UINT priorOutputs;
            {
                std::lock_guard<std::mutex> lock(callbacks.mutex);
                priorOutputs = static_cast<UINT>(callbacks.captures.size());
            }
            // Previous batches were drained. Each accepted composition issues
            // one native Present, so generated phases rotate the physical ring
            // independently of the public game index or OS displayed count.
            const UINT physicalSlot = priorOutputs % std::size(queues);
            auto* selected = static_cast<ID3D12CommandQueue*>(queues[physicalSlot]);
            if (frame == 9)
                fgRequire(selected != gpu.queue.Get() && rejectedGenerationGate.Get() != queueGate.gate.Get(),
                          "reject-generation-independent-game-and-presentation-gates");
            std::printf("OWNED_D3D12_FSR_FG_QUEUE_SELECTION frame=%u physical_slot=%u prior_callback_outputs=%u queue=%c\n",
                        frame, physicalSlot, priorOutputs, selected == queueA ? 'A' : 'B');
            queueGate.begin(frame, selected, selected == queueA ? queueB : queueA);
        }
        const HRESULT result = chain->Present(1, 0);
        QueryPerformanceCounter(&after);
        if (frame == 9) {
            fgRequire(double(after.QuadPart - before.QuadPart) / frequency.QuadPart < 15,
                      "reject-generation-present-api-bounded");
            fgRequire(rejectedGameReached->GetCompletedValue() == 0 && queueGate.reached->GetCompletedValue() < queueGate.serial,
                      "reject-generation-game-and-presentation-gpu-still-pending");
            // The SDK retires interpolation before recording its REAL callback.
            // Complete game work without opening the independent presentation gate.
            check(rejectedGenerationGate->Signal(1), "reject-generation-release-game-batch");
            fgRequire(queueGate.gate->GetCompletedValue() < queueGate.serial,
                      "reject-generation-game-release-preserves-presentation-gate");
            queueGate.release(frame, &callbacks, true);
            fgRequire(rejectedGameReached->GetCompletedValue() == 1, "reject-generation-game-batch-retired");
            fgRequire(WaitForSingleObject(callbacks.recorded, 100) == WAIT_TIMEOUT,
                      "reject-generation-no-second-callback");
            std::puts("OWNED_D3D12_FSR_FG_REJECTED_GENERATION_ORDER frame=9 real_gpu_output=after_current_game_batch generated=absent");
        }
        if (frame == 1) {
            backbuffer.Reset();
            HANDLE entered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HANDLE finished = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HRESULT resizeResult = S_OK;
            std::thread resizing([&] {
                SetEvent(entered);
                resizeResult = chain->ResizeBuffers1(3, 96, 80, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, desc.Flags, masks, queues);
                SetEvent(finished);
            });
            fgRequire(WaitForSingleObject(entered, 10000) == WAIT_OBJECT_0, "queue-resize-entered");
            fgRequire(WaitForSingleObject(finished, 100) == WAIT_TIMEOUT, "queue-resize-drains-old-generated-frame");
            queueGate.release(frame, &callbacks);
            fgRequire(WaitForSingleObject(finished, 15000) == WAIT_OBJECT_0, "queue-resize-unblocks-after-retirement");
            resizing.join(); CloseHandle(entered); CloseHandle(finished);
            fgRequire(resizeResult == DXGI_ERROR_INVALID_CALL, "queue-active-resize-backend-rollback");
            DXGI_SWAP_CHAIN_DESC1 retained{}; check(chain->GetDesc1(&retained), "queue-active-rollback-desc");
            fgRequire(retained.Width == fgWidth && retained.Height == fgHeight && retained.BufferCount == 3,
                      "queue-active-rollback-preserves-native-ring");
            ++rejected;
        }
        else if (gated && frame != 9) queueGate.release(frame, &callbacks);
        if (frame != 10) check(result, "proxy-present");
        fgDrain(api, swapContext, frame == 10 && SUCCEEDED(result));
        if (frame != 10) fgPresentedStatistics(chain.Get(), frame, previousStatistics);
        std::printf("OWNED_D3D12_FSR_FG_PRESENT_CALL frame=%u enabled=%u only_generated=%u generation_error=%u present_error=%u qpc_start=%lld qpc_end=%lld frequency=%lld\n",
                    frame, config.frameGenerationEnabled, config.onlyPresentGenerated, frame == 9, frame == 10,
                    before.QuadPart, after.QuadPart, frequency.QuadPart);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    QueryPerformanceCounter(&stop);
    fgRequire(double(stop.QuadPart - start.QuadPart) / frequency.QuadPart < 60, "proxy-ordered-bounded-progress");
    {
        std::lock_guard<std::mutex> lock(callbacks.mutex);
        uint64_t previousFrame = 0; bool previousGenerated = false;
        UINT generatedCounts[13] = {}, realCounts[13] = {};
        std::vector<uint64_t> phaseHashes[13];
        for (const auto& capture : callbacks.captures) {
            auto pixels = fgCallbackPixels(capture);
            const UINT index = capture.generated ? ++generatedCounts[capture.frame] : ++realCounts[capture.frame];
            char path[96]; std::snprintf(path, sizeof(path), "fg-callback-%llu-%u-%u.bin",
                static_cast<unsigned long long>(capture.frame), capture.generated, index);
            fgDump(pixels, path);
            fgRequire(pixels[0] == capture.frame + 32 && pixels[1] == (capture.generated ? 190 : 70) && pixels[2] == 220,
                       "callback-gpu-frame-marker");
            fgRequire(capture.frame >= previousFrame && (capture.frame != previousFrame || previousGenerated),
                       "generated-before-real-callback-pixel-order");
            if (!capture.generated) {
                const UINT center = (64 * fgWidth + 64) * 4;
                fgRequire(pixels[center] == (capture.frame == 11 ? 255 : 32) && pixels[center + 2] == (capture.frame == 11 ? 255 : 8),
                           "actual-render-color-readback");
            }
            fgRequire(capture.frame != 10 && (capture.frame != 8 || capture.generated) &&
                       (capture.frame != 9 || !capture.generated) &&
                       ((capture.frame != 5 && capture.frame != 6) || !capture.generated), "negative-output-never-callback-presented");
            std::printf("OWNED_D3D12_FSR_FG_CALLBACK_PIXELS frame=%llu generated=%u output_index=%u width=%u height=%u hash=%016llx marker=%u,%u,%u\n",
                        static_cast<unsigned long long>(capture.frame), capture.generated, capture.generated ? index : 0, capture.width, capture.height,
                        static_cast<unsigned long long>(fgHash(pixels)), pixels[0], pixels[1], pixels[2]);
            previousFrame = capture.frame; previousGenerated = capture.generated;
            // Ignore the application UI marker: compare generated GPU content,
            // not a callback-provided phase label.
            for (UINT y = 0; y < 8; ++y)
                std::fill_n(pixels.data() + y * capture.width * 4, 8 * 4, 0);
            const auto hash = fgHash(pixels);
            for (auto prior : phaseHashes[capture.frame])
                fgRequire(prior != hash, "genuine-phase-output-not-duplicated-current-or-midpoint");
            phaseHashes[capture.frame].push_back(hash);
        }
        const UINT expectedGenerated[13] = {0, 0, 1, 2, 3, 0, 0, 0, 4, 0, 0, 0, 0};
        for (UINT frame = 1; frame <= 12; ++frame) {
            fgRequire(generatedCounts[frame] == expectedGenerated[frame], "gpu-actual-count-controls-callbacks");
            fgRequire(realCounts[frame] == ((frame == 8 || frame == 10 || frame == 12) ? 0u : 1u), "real-once-or-generated-only-suppressed");
        }
        std::puts("OWNED_D3D12_FSR_FG_MULTI_PACING_PASS counts=1,2,3,4 distinct_gpu_outputs=1 generated_before_real=1 bootstrap_reset_scenechange_disabled=zero only_generated_zero=no_present");
        std::puts("OWNED_D3D12_FSR_FG_NO_NOTIFY_PROXY_PASS frame=3 generated=2 result_observer=retained");
    }
    fgPresentedStatistics(chain.Get(), 12, previousStatistics, "callback");
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
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queueOwnerA)), "ui-present-queue-a");
    check(gpu.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queueOwnerB)), "ui-present-queue-b");
    queueA = queueOwnerA.Get(); queueB = queueOwnerB.Get();
    IUnknown* uiQueues[2] = {queueA, queueB};
    check(chain->ResizeBuffers1(2, fgWidth, fgHeight, DXGI_FORMAT_UNKNOWN, desc.Flags, masks, uiQueues), "ui-present-queues-resize1");
    queueOwnerA.Reset(); queueOwnerB.Reset();
    FgQueueGate uiGate(gpu, chain.Get());
    previousStatistics = {};
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
        if (frame == 11 || frame == 12) uiGate.begin(frame, frame == 11 ? queueA : queueB, frame == 11 ? queueB : queueA);
        check(chain->Present(1, 0), "ui-builtin-present");
        if (frame == 11 || frame == 12) uiGate.release(frame);
        r = fgRecording(gpu); transition(r.list.Get(), ui.Get(), fgRead, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.list->Close(), "ui-mutate-transition"); gpu.executeAndWait(r.list.Get());
        std::vector<uint8_t> mutant(fgWidth * fgHeight * 4);
        for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
            const UINT i = (y * fgWidth + x) * 4; mutant[i + 1] = 128; mutant[i + 3] = 128;
        }
        fgUpload(gpu, ui.Get(), mutant.data(), fgWidth * 4, fgRead);
        fgDrain(api, swapContext);
        fgPresentedStatistics(chain.Get(), frame, previousStatistics);
        std::printf("OWNED_D3D12_FSR_FG_UI_FRAME frame=%u width=%u height=%u source=%u,60,100 ui=64,0,0,128 premul=1 doublebuffer=1 overwritten_after_present=1\n", frame, width, height, frame * 8);
    }
    fgPresentedStatistics(chain.Get(), 14, previousStatistics, "builtin_ui", true);
    registration.uiResource = {};
    fgResult(api.configure(&swapContext, &registration.header), FFX_API_RETURN_OK, "ui-unregister");
    ui.Reset(); fgDrain(api, swapContext); uiContext.destroy();
    fgResult(api.destroy(&swapContext, nullptr), FFX_API_RETURN_OK, "wrapped-proxy-destroy"); chain.Reset();
    std::puts("OWNED_D3D12_FSR_FG_UI_PASS wrap=real_hwnd ui=builtin_shader premultiplied=1 doublebuffer=gpu_snapshot resize=96x80 lifetime=drained");
    fgRequire(rejected == 12, "queue-rejection-cases-complete");
    std::puts("OWNED_D3D12_FSR_FG_QUEUES_PASS samequeue=1 nullarray=rejected count0=preserved multipqueue=3 device_type_mask=rejected rollback=atomic drain=oldframe owners=retained generated_real_ui=selected worker_idle=progress");
    std::puts("OWNED_D3D12_FSR_FG_PROXY_PASS create=hwnd callbacks=gpu-readback generated_before_real=1 only_generated=1 disabled=1 callback_error=1 drain=1 destroy=1");
    std::puts("OWNED_D3D12_FSR_FG_PACING_PASS ordered=1 bounded_progress=1 qpc=observed native_timestamps=external_observer_required");
}
}
