#include <dxgi1_6.h>
#include "owned-fg-present.h"

namespace {
constexpr UINT ngxPresentExtent = 128;
constexpr auto ngxPresentRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

FgActualPresentationObservation ngxPresentObserve(IDXGISwapChain4* chain, DXGI_FRAME_STATISTICS& previous) {
    const auto observation = fgActualPresentation(chain);
    if (SUCCEEDED(observation.result)) {
        const auto& statistics = observation.statistics;
        if (statistics.PresentCount < previous.PresentCount ||
            statistics.PresentRefreshCount < previous.PresentRefreshCount ||
            statistics.SyncRefreshCount < previous.SyncRefreshCount ||
            statistics.SyncQPCTime.QuadPart < previous.SyncQPCTime.QuadPart)
            fail("NGX measured presentation statistics regressed");
        previous = statistics;
    }
    return observation;
}

// Caller-authored translating checkerboard and a current-frame opaque HUD.
// HUDLess carries the scene; Backbuffer carries scene+HUD. No DLSSG.UI keys.
std::vector<uint8_t> ngxPresentScene(UINT frame, bool hud) {
    std::vector<uint8_t> pixels(ngxPresentExtent * ngxPresentExtent * 4);
    for (UINT y = 0; y < ngxPresentExtent; ++y) for (UINT x = 0; x < ngxPresentExtent; ++x) {
        const auto i = (y * ngxPresentExtent + x) * 4;
        const bool checker = (((x + frame * 4) / 16) ^ (y / 16)) & 1;
        pixels[i] = checker ? 128 : 64;
        pixels[i + 1] = x > 40 + frame * 2 && x < 64 + frame * 2 && y > 40 && y < 96 ? 200 : 32;
        pixels[i + 2] = checker ? 32 : 128;
        pixels[i + 3] = 255;
        if (hud && x < 32 && y < 32) {
            pixels[i] = uint8_t(32 + frame * 24); pixels[i + 1] = 220; pixels[i + 2] = 48;
        }
    }
    return pixels;
}
void ngxPresentDump(const std::vector<uint8_t>& pixels, const char* path) {
    FILE* file = std::fopen(path, "wb");
    if (!file || std::fwrite(pixels.data(), pixels.size(), 1, file) != 1 || std::fclose(file))
        fail("NGX present pixel dump");
}
std::vector<uint8_t> ngxPresentPixels(Gpu& gpu, ID3D12Resource* texture, D3D12_RESOURCE_STATES state) {
    auto readback = createTextureReadback(gpu, texture);
    auto record = gpu.makeRecording();
    transition(record.list.Get(), texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyTextureToReadback(record.list.Get(), texture, readback);
    transition(record.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    check(record.list->Close(), "NGX present readback close"); gpu.executeAndWait(record.list.Get());
    uint8_t* bytes = nullptr;
    check(readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&bytes)), "NGX present readback map");
    const auto& footprint = readback.footprint.Footprint;
    std::vector<uint8_t> pixels(size_t(footprint.Width) * footprint.Height * 4);
    for (UINT y = 0; y < footprint.Height; ++y)
        std::memcpy(pixels.data() + size_t(y) * footprint.Width * 4,
                    bytes + readback.footprint.Offset + y * readback.footprint.Footprint.RowPitch,
                    size_t(footprint.Width) * 4);
    readback.buffer->Unmap(0, nullptr);
    return pixels;
}
NVSDK_NGX_Handle* ngxPresentFeature(const NgxApi& api, Gpu& gpu, bool orthographic = false) {
    auto params = allocateParams(api);
    ngx_feature_set_uint(params, "DLSSG.Width", ngxPresentExtent);
    ngx_feature_set_uint(params, "DLSSG.Height", ngxPresentExtent);
    ngx_feature_set_uint(params, "DLSSG.InternalWidth", ngxPresentExtent);
    ngx_feature_set_uint(params, "DLSSG.InternalHeight", ngxPresentExtent);
    ngx_feature_set_uint(params, "DLSSG.BackbufferFormat", DXGI_FORMAT_R8G8B8A8_UNORM);
    ngx_feature_set_uint(params, "DLSSG.DepthInverted", 1);
    ngx_feature_set_uint(params, "DLSSG.OrthoProjection", orthographic);
    auto record = gpu.makeRecording(); NVSDK_NGX_Handle* feature = nullptr;
    checkNgxResult(api.create(record.list.Get(), 11, params, &feature), ngx_fixture_result_success(), "NGX present create11");
    check(record.list->Close(), "NGX present create close"); resetAndDestroyParams(api, params);
    return feature;
}
ComPtr<ID3D12Resource> ngxPresentDisableBuffer(Gpu& gpu, UINT64 size = 4) {
    auto desc = bufferDescription(size);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> result;
    check(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&result)), "NGX disable buffer");
    return result;
}
Recording ngxPresentEvaluate(const NgxApi& api, Gpu& gpu, NVSDK_NGX_Handle* feature,
                             UINT frame, bool reset, ID3D12Resource* color, ID3D12Resource* hudless,
                             ID3D12Resource* output, ID3D12Resource* depth, ID3D12Resource* motion,
                             bool close = true, bool notRendering = false, ID3D12Resource* disable = nullptr,
                             float* clipToPrev = nullptr, bool cameraMotionIncluded = true,
                             float* viewToClip = nullptr) {
    auto params = allocateParams(api);
    ngx_feature_set_resource(params, "DLSSG.Backbuffer", color);
    ngx_feature_set_resource(params, "DLSSG.HUDLess", hudless);
    ngx_feature_set_resource(params, "DLSSG.OutputInterpolated", output);
    ngx_feature_set_resource(params, "DLSSG.Depth", depth);
    ngx_feature_set_resource(params, "DLSSG.MVecs", motion);
    ngx_feature_set_resource(params, "DLSSG.OutputDisableInterpolation", disable);
    ngx_feature_set_uint(params, "DLSSG.CameraMotionIncluded", cameraMotionIncluded);
    if (clipToPrev) ngx_feature_set_pointer(params, "DLSSG.ClipToPrevClip", clipToPrev);
    if (viewToClip) {
        ngx_feature_set_uint(params, "DLSSG.OrthoProjection", 1);
        ngx_feature_set_pointer(params, "DLSSG.CameraViewToClip", viewToClip);
        const char* keys[4][3] = {{"DLSSG.CameraPosX","DLSSG.CameraPosY","DLSSG.CameraPosZ"},
            {"DLSSG.CameraUpX","DLSSG.CameraUpY","DLSSG.CameraUpZ"},
            {"DLSSG.CameraRightX","DLSSG.CameraRightY","DLSSG.CameraRightZ"},
            {"DLSSG.CameraFwdX","DLSSG.CameraFwdY","DLSSG.CameraFwdZ"}};
        const float basis[4][3] = {{0,0,0},{0,1,0},{1,0,0},{0,0,1}};
        for (UINT vector = 0; vector != 4; ++vector) for (UINT axis = 0; axis != 3; ++axis)
            ngx_feature_set_float(params, keys[vector][axis], basis[vector][axis]);
    }
    ngx_feature_set_uint(params, "DLSSG.BackbufferFrameID", frame);
    ngx_feature_set_uint(params, "DLSSG.Reset", reset);
    ngx_feature_set_uint(params, "DLSSG.NotRenderingGameFrames", notRendering);
    ngx_feature_set_float(params, "FrameTimeDeltaInMsec", 1000.f / 30);
    ngx_feature_set_float(params, "DLSSG.MvecScaleX", 1.0f / ngxPresentExtent);
    ngx_feature_set_float(params, "DLSSG.MvecScaleY", 1.0f / ngxPresentExtent);
    ngx_feature_set_float(params, "DLSSG.JitterOffsetX", (frame & 1 ? .25f : -.25f) * 2 / ngxPresentExtent);
    ngx_feature_set_float(params, "DLSSG.JitterOffsetY", (frame & 1 ? -.25f : .25f) * -2 / ngxPresentExtent);
    ngx_feature_set_float(params, "DLSSG.CameraNear", .1f);
    ngx_feature_set_float(params, "DLSSG.CameraFar", 1000);
    ngx_feature_set_float(params, "DLSSG.CameraFOV", 1);
    auto record = gpu.makeRecording();
    checkNgxResult(api.evaluate(record.list.Get(), feature, params, nullptr), ngx_fixture_result_success(), "NGX present evaluate11");
    resetAndDestroyParams(api, params);
    if (close) check(record.list->Close(), "NGX present evaluate close");
    return record;
}
void ngxPresentCopy(Gpu& gpu, IDXGISwapChain4* chain, HANDLE credit,
                    ID3D12Resource* source, D3D12_RESOURCE_STATES state) {
    if (WaitForSingleObject(credit, 15000) != WAIT_OBJECT_0) fail("NGX present latency credit");
    ComPtr<ID3D12Resource> backbuffer;
    check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "NGX present real backbuffer");
    auto record = gpu.makeRecording();
    transition(record.list.Get(), source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(record.list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    record.list->CopyResource(backbuffer.Get(), source);
    transition(record.list.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    transition(record.list.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    check(record.list->Close(), "NGX present copy close"); gpu.executeAndWait(record.list.Get());
}
void ngxPresentBootstrap(Gpu& gpu, IDXGISwapChain4* chain, HANDLE credit, DXGI_FRAME_STATISTICS& previous) {
    // A newly attached Wine/CA window can legally discard its first drawable.
    // A skipped drawable cannot satisfy GetFrameStatistics: submit genuine real
    // frames until actual visibility is established BEFORE numbering NGX pairs.
    LARGE_INTEGER start{}, now{}, frequency{};
    QueryPerformanceCounter(&start); QueryPerformanceFrequency(&frequency);
    auto color = createTexture(gpu.device.Get(), ngxPresentExtent, ngxPresentExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint8_t> pixels(ngxPresentExtent * ngxPresentExtent * 4);
    for (UINT attempt = 1; attempt <= 64; ++attempt) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (attempt != 1) {
            auto record = gpu.makeRecording();
            transition(record.list.Get(), color.Get(), ngxPresentRead, D3D12_RESOURCE_STATE_COPY_DEST);
            check(record.list->Close(), "NGX bootstrap upload transition"); gpu.executeAndWait(record.list.Get());
        }
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = uint8_t(16 + attempt); pixels[i + 1] = 80; pixels[i + 2] = 176; pixels[i + 3] = 255;
        }
        uploadTexture(gpu, color.Get(), pixels.data(), ngxPresentExtent * 4, ngxPresentRead);
        const auto readback = ngxPresentPixels(gpu, color.Get(), ngxPresentRead);
        if (readback != pixels) fail("NGX bootstrap independent real pixels");
        char path[80]; std::snprintf(path, sizeof(path), "fg-ngx-bootstrap-%u.bin", attempt);
        ngxPresentDump(readback, path);
        ngxPresentCopy(gpu, chain, credit, color.Get(), ngxPresentRead);
        check(chain->Present(1, 0), "NGX bootstrap real Present");
        const auto observation = ngxPresentObserve(chain, previous);
        std::printf("OWNED_D3D12_NGX_PRESENT_BOOTSTRAP attempt=%u width=128 height=128 submitted_present=%u rgb=%u,80,176 source=caller_real_gpu_copy_to_dxgi\n",
                    attempt, observation.submitted, 16 + attempt);
        std::fflush(stdout);
        QueryPerformanceCounter(&now);
        if (observation.result == S_OK) {
            const auto& statistics = observation.statistics;
            std::printf("OWNED_D3D12_NGX_PRESENT_BOOTSTRAP_READY attempt=%u submitted_present=%u statistics=actual result=0x%08lx actual_present=%u sync_qpc=%lld present_refresh=%u sync_refresh=%u qpc_end=%lld frequency=%lld source=public_dxgi_actual_presentation\n",
                        attempt, observation.submitted, static_cast<unsigned long>(observation.result), statistics.PresentCount, statistics.SyncQPCTime.QuadPart,
                        statistics.PresentRefreshCount, statistics.SyncRefreshCount, now.QuadPart, frequency.QuadPart);
            return;
        }
        if (double(now.QuadPart - start.QuadPart) / frequency.QuadPart >= 15) fail("NGX bootstrap visibility bounded");
    }
    fail("NGX bootstrap actual display not established");
}
void ngxPresentTexture(Gpu& gpu, IDXGISwapChain4* chain, IDXGIOutput* display, HANDLE credit,
                       ID3D12Resource* source, D3D12_RESOURCE_STATES state, UINT frame, UINT generated,
                       DXGI_FRAME_STATISTICS& previous) {
    // Continue servicing the real HWND after the visibility bootstrap. GPU
    // waits and output-vblank pacing do not dispatch window/driver events.
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    ngxPresentCopy(gpu, chain, credit, source, state);
    LARGE_INTEGER paceBefore{}, before{}, after{}, frequency{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&paceBefore);
    check(display->WaitForVBlank(), "NGX caller-owned output pacing");
    QueryPerformanceCounter(&before);
    check(chain->Present(1, 0), "NGX caller-owned DXGI Present");
    // S_OK means submitted, not scanned out. Frame-latency credit retires GPU
    // work; CA can still legally skip the drawable. Snapshot actual statistics
    // without blocking progress on an id that may never become displayed.
    const auto observation = ngxPresentObserve(chain, previous);
    const auto& stats = observation.statistics;
    QueryPerformanceCounter(&after);
    std::printf("OWNED_D3D12_NGX_PRESENT_FRAME frame=%u generated=%u generation=native_metalfx width=128 height=128 submitted_present=%u statistics=%s result=0x%08lx actual_present=%u sync_qpc=%lld present_refresh=%u sync_refresh=%u pace_qpc_start=%lld qpc_start=%lld qpc_end=%lld frequency=%lld source=ngx11_gpu_copy_to_dxgi\n",
                frame, generated, observation.submitted, SUCCEEDED(observation.result) ? "actual" : "disjoint", static_cast<unsigned long>(observation.result), stats.PresentCount,
                stats.SyncQPCTime.QuadPart, stats.PresentRefreshCount, stats.SyncRefreshCount,
                paceBefore.QuadPart, before.QuadPart, after.QuadPart, frequency.QuadPart);
}
void ngxPresentHold(const char* control, const char* phase, HWND hwnd, IDXGISwapChain4* chain, Gpu& gpu,
                    DXGI_FRAME_STATISTICS& previous) {
    if (!control) return;
    // Only the opt-in investigation adds this public fence AFTER Present.
    // READY must not race the queued GPU/native submission. Window events
    // remain serviced while waiting, exactly as in the ordinary public probe.
    ComPtr<ID3D12Fence> fence;
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "NGX visual retirement fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) fail("NGX visual retirement event", HRESULT_FROM_WIN32(GetLastError()));
    check(gpu.queue->Signal(fence.Get(), 1), "NGX visual retirement queue signal");
    check(fence->SetEventOnCompletion(1, event), "NGX visual retirement fence event");
    const ULONGLONG retirementStart = GetTickCount64();
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        const DWORD result = MsgWaitForMultipleObjects(1, &event, FALSE, DWORD(15000 - std::min<ULONGLONG>(15000, GetTickCount64() - retirementStart)), QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) fail("NGX visual retirement wait", HRESULT_FROM_WIN32(GetLastError()));
        if (GetTickCount64() - retirementStart >= 15000) fail("NGX visual retirement bounded");
    }
    CloseHandle(event);
    check(gpu.device->GetDeviceRemovedReason(), "NGX visual retirement device status");
    if (fence->GetCompletedValue() != 1) fail("NGX visual retirement exact fence");
    char title[160];
    std::snprintf(title, sizeof(title), "Owned NGX feature11 | %s | control-file hold", phase);
    if (!SetWindowTextA(hwnd, title)) fail("NGX visual phase window title", HRESULT_FROM_WIN32(GetLastError()));
    RECT window{}, client{};
    if (!GetWindowRect(hwnd, &window) || !GetClientRect(hwnd, &client))
        fail("NGX visual phase window geometry", HRESULT_FROM_WIN32(GetLastError()));
    const auto observation = ngxPresentObserve(chain, previous);
    const auto& statistics = observation.statistics;
    std::printf("OWNED_D3D12_NGX_PRESENT_VISUAL_READY phase=%s hwnd=%p submitted_present=%u statistics=%s result=0x%08lx actual_present=%u sync_qpc=%lld gpu_retired=1 window=%ld,%ld,%ld,%ld client=%ld,%ld control=%s\n",
                phase, static_cast<void*>(hwnd), observation.submitted, SUCCEEDED(observation.result) ? "actual" : "disjoint", static_cast<unsigned long>(observation.result),
                statistics.PresentCount, statistics.SyncQPCTime.QuadPart,
                window.left, window.top, window.right, window.bottom, client.right, client.bottom, control);
    std::fflush(stdout);
    char fullControl[MAX_PATH];
    if (!GetFullPathNameA(control, MAX_PATH, fullControl, nullptr)) fail("NGX control full path");
    char* separator = std::strrchr(fullControl, '\\');
    if (!separator) fail("NGX control parent directory");
    *separator = 0;
    HANDLE changed = FindFirstChangeNotificationA(fullControl, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (changed == INVALID_HANDLE_VALUE) fail("NGX control directory event");
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        bool advance = false;
        if (FILE* file = std::fopen(control, "r")) {
            char token[128]{};
            advance = std::fscanf(file, "%127s", token) == 1 && std::strcmp(token, phase) == 0;
            std::fclose(file);
        }
        if (advance) break;
        if (GetTickCount64() - started >= 300000) fail("NGX visual control timeout");
        const DWORD result = MsgWaitForMultipleObjects(1, &changed, FALSE, DWORD(300000 - std::min<ULONGLONG>(300000, GetTickCount64() - started)), QS_ALLINPUT);
        if (result == WAIT_OBJECT_0 && !FindNextChangeNotification(changed)) fail("NGX control rearm event");
        if (result == WAIT_FAILED) fail("NGX control event wait");
    }
    FindCloseChangeNotification(changed);
    // A file token only advances the investigation; it is not visual evidence.
    std::printf("OWNED_D3D12_NGX_PRESENT_VISUAL_ADVANCED phase=%s\n", phase);
    std::fflush(stdout);
}
void runPublicFgPresent(const NgxApi& api, Gpu& gpu, const char* control) {
    FgPresentWindow window(ngxPresentExtent, ngxPresentExtent, L"public NGX feature11 caller-owned Present");
    RECT outer{}, work{};
    if (!GetWindowRect(window.hwnd, &outer) || !SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        fail("NGX public presentation work area", HRESULT_FROM_WIN32(GetLastError()));
    const LONG width = outer.right - outer.left, height = outer.bottom - outer.top;
    if (width <= 0 || height <= 0 || work.right - work.left < width + 8 || work.bottom - work.top < 2 * height + 16)
        fail("NGX public presentation work area cannot fit disjoint ABI windows");
    const LONG x = work.right - width - 8;
    const LONG y = std::clamp(work.top + 96, work.top, work.bottom - 2 * height - 16) +
                   (sizeof(void*) == 4 ? height + 16 : 0);
    if (x < work.left || y < work.top || x + width > work.right || y + height > work.bottom)
        fail("NGX public presentation work area too small");
    if (!SetWindowPos(window.hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER) ||
        !GetWindowRect(window.hwnd, &outer))
        fail("NGX public presentation window placement", HRESULT_FROM_WIN32(GetLastError()));
    if (outer.left != x || outer.top != y || outer.right != x + width || outer.bottom != y + height)
        fail("NGX public presentation actual rectangle differs");
    std::printf("OWNED_D3D12_NGX_PRESENT_WINDOW abi=%u rect=%ld,%ld,%ld,%ld work=%ld,%ld,%ld,%ld placement=public_noactivate_nozorder physical_proof=external_observer_required\n",
                unsigned(sizeof(void*) * 8), outer.left, outer.top, outer.right, outer.bottom,
                work.left, work.top, work.right, work.bottom);
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "NGX present factory");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = ngxPresentExtent; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> base; ComPtr<IDXGISwapChain4> chain;
    check(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window.hwnd, &desc, nullptr, nullptr, &base), "NGX caller-owned flip swapchain");
    check(base.As(&chain), "NGX present chain4"); base.Reset();
    check(chain->SetMaximumFrameLatency(1), "NGX present frame latency");
    HANDLE credit = chain->GetFrameLatencyWaitableObject(); if (!credit) fail("NGX present waitable object");
    DXGI_FRAME_STATISTICS previous{};
    ngxPresentBootstrap(gpu, chain.Get(), credit, previous);
    ComPtr<IDXGIOutput> display; check(chain->GetContainingOutput(&display), "NGX actual containing output");
    check(display->WaitForVBlank(), "NGX caller output pacing prime");
    auto depth = createTexture(gpu.device.Get(), ngxPresentExtent, ngxPresentExtent, DXGI_FORMAT_R32_FLOAT,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const std::vector<float> depths(ngxPresentExtent * ngxPresentExtent, .75f);
    uploadTexture(gpu, depth.Get(), depths.data(), ngxPresentExtent * 4, ngxPresentRead);
    auto motion = createTexture(gpu.device.Get(), ngxPresentExtent, ngxPresentExtent, DXGI_FORMAT_R16G16_FLOAT,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<uint16_t> vectors(ngxPresentExtent * ngxPresentExtent * 2);
    for (size_t i = 0; i < vectors.size(); i += 2) vectors[i] = 0x4400; // +4 display pixels.
    uploadTexture(gpu, motion.Get(), vectors.data(), ngxPresentExtent * 4, ngxPresentRead);
    auto feature = ngxPresentFeature(api, gpu);
    const std::vector<uint8_t> sentinel(ngxPresentExtent * ngxPresentExtent * 4, 19);
    for (UINT frame = 1; frame <= 6; ++frame) {
        auto color = createTexture(gpu.device.Get(), ngxPresentExtent, ngxPresentExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto hudless = createTexture(gpu.device.Get(), ngxPresentExtent, ngxPresentExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        const auto real = ngxPresentScene(frame, true), scene = ngxPresentScene(frame, false);
        uploadTexture(gpu, color.Get(), real.data(), ngxPresentExtent * 4, ngxPresentRead);
        uploadTexture(gpu, hudless.Get(), scene.data(), ngxPresentExtent * 4, ngxPresentRead);
        auto output = createTexture(gpu.device.Get(), ngxPresentExtent, ngxPresentExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        uploadTexture(gpu, output.Get(), sentinel.data(), ngxPresentExtent * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool reset = frame == 1 || frame >= 5;
        auto record = ngxPresentEvaluate(api, gpu, feature, frame, reset, color.Get(), hudless.Get(), output.Get(), depth.Get(), motion.Get());
        if (frame == 6) checkNgxResult(api.release(feature), ngx_fixture_result_success(), "NGX present release before execute");
        gpu.executeAndWait(record.list.Get());
        const auto generated = ngxPresentPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (generated == sentinel || ((frame == 3 || frame == 4) && generated == real)) fail("NGX native empty/current generated output");
        char path[80];
        std::snprintf(path, sizeof(path), "fg-ngx-%u-1.bin", frame); ngxPresentDump(generated, path);
        std::snprintf(path, sizeof(path), "fg-ngx-%u-0.bin", frame);
        const auto realReadback = ngxPresentPixels(gpu, color.Get(), ngxPresentRead);
        if (realReadback != real) fail("NGX real texture differs from caller-authored pixels");
        ngxPresentDump(realReadback, path);
        if (frame == 4 || frame == 5) {
            auto fresh = ngxPresentFeature(api, gpu);
            auto freshOutput = createTexture(gpu.device.Get(), ngxPresentExtent, ngxPresentExtent, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            uploadTexture(gpu, freshOutput.Get(), sentinel.data(), ngxPresentExtent * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto control = ngxPresentEvaluate(api, gpu, fresh, frame, true, color.Get(), hudless.Get(), freshOutput.Get(), depth.Get(), motion.Get());
            checkNgxResult(api.release(fresh), ngx_fixture_result_success(), "NGX fresh control release before execute");
            gpu.executeAndWait(control.list.Get());
            const auto expected = ngxPresentPixels(gpu, freshOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (expected == sentinel) fail("NGX fresh bootstrap output unwritten");
            if (frame == 5 && generated != expected) fail("NGX native reset fresh equivalence");
            std::snprintf(path, sizeof(path), "fg-ngx-fresh-%u.bin", frame); ngxPresentDump(expected, path);
        }
        if (frame == 6) {
            gpu.executeAndWait(record.list.Get());
            const auto replay = ngxPresentPixels(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (replay == sentinel) fail("NGX released feature native replay output unwritten");
            checkNgxResult(api.release(feature), ngx_fixture_result_feature_not_found(), "NGX stale present feature");
            ngxPresentDump(replay, "fg-ngx-replay-6.bin");
        }
        if (!reset) {
            ngxPresentTexture(gpu, chain.Get(), display.Get(), credit, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, frame, 1, previous);
            if (control && frame == 2)
                ngxPresentHold(control, "after-generated-2", window.hwnd, chain.Get(), gpu, previous);
        } else
            std::printf("OWNED_D3D12_NGX_BOOTSTRAP_OUTPUT frame=%u reset=1 output_written=1 presented=0 policy=caller_no_history\n", frame);
        ngxPresentTexture(gpu, chain.Get(), display.Get(), credit, color.Get(), ngxPresentRead, frame, 0, previous);
        if (frame == 2) ngxPresentHold(control, "after-real-2", window.hwnd, chain.Get(), gpu, previous);
    }
    if (WaitForSingleObject(credit, 15000) != WAIT_OBJECT_0) fail("NGX final GPU latency retirement");
    // Drain scheduling with real output refreshes, not a per-id scanout promise
    // or resubmitting a skipped/generated replacement frame.
    for (UINT tick = 0; tick < 3; ++tick) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        check(display->WaitForVBlank(), "NGX final native output refresh drain");
    }
    const auto observation = ngxPresentObserve(chain.Get(), previous);
    const auto& finalStats = observation.statistics;
    LARGE_INTEGER now{}, frequency{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    std::printf("OWNED_D3D12_NGX_PRESENT_DRAIN submitted_present=%u statistics=%s result=0x%08lx actual_present=%u sync_qpc=%lld present_refresh=%u sync_refresh=%u qpc_end=%lld frequency=%lld output_vblanks=3\n",
                observation.submitted, SUCCEEDED(observation.result) ? "actual" : "disjoint", static_cast<unsigned long>(observation.result), finalStats.PresentCount,
                finalStats.SyncQPCTime.QuadPart, finalStats.PresentRefreshCount, finalStats.SyncRefreshCount, now.QuadPart, frequency.QuadPart);
    CloseHandle(credit);
    std::puts("OWNED_D3D12_NGX_PRESENT_PASS frames=6 drawables=9 generated=3 bootstrap_not_presented=3 ngx_feature=11 hudless=1 ui=current_backbuffer history=native reset=fresh_equivalent release_before_execute=1 replay=native_encode caller_owned_present=1");
}
}
