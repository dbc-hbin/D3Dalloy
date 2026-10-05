#pragma once
// Test-only HWND and actual DXGI presentation observation shared by NGX/FFX consumers.
namespace {
struct FgPresentWindow {
    HWND hwnd;
    FgPresentWindow(UINT width, UINT height, const wchar_t* title) {
        WNDCLASSW c{}; c.hInstance = GetModuleHandleW(nullptr); c.lpfnWndProc = DefWindowProcW;
        c.lpszClassName = L"OwnedFgPresentWindow";
        if (!RegisterClassW(&c)) fail("register-fg-window", HRESULT_FROM_WIN32(GetLastError()));
        RECT rect{0, 0, LONG(width), LONG(height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowW(c.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                             60, 60, rect.right - rect.left, rect.bottom - rect.top,
                             nullptr, nullptr, c.hInstance, nullptr);
        if (!hwnd) fail("fg-hwnd", HRESULT_FROM_WIN32(GetLastError()));
        ShowWindow(hwnd, SW_SHOWNA); UpdateWindow(hwnd);
    }
    ~FgPresentWindow() {
        DestroyWindow(hwnd);
        UnregisterClassW(L"OwnedFgPresentWindow", GetModuleHandleW(nullptr));
    }
};
struct FgActualPresentationObservation {
    UINT submitted;
    HRESULT result;
    DXGI_FRAME_STATISTICS statistics;
};
inline FgActualPresentationObservation fgActualPresentation(IDXGISwapChain4* chain) {
    FgActualPresentationObservation observation{};
    check(chain->GetLastPresentCount(&observation.submitted), "native-last-present-count");
    observation.result = chain->GetFrameStatistics(&observation.statistics);
    if (observation.result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        observation.statistics = {};
        return observation;
    }
    check(observation.result, "actual-public-presentation-statistics");
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    const auto& statistics = observation.statistics;
    // GetLastPresentCount counts Present calls; GetFrameStatistics counts
    // displayed frames. A legally skipped MTLDrawable need never advance it.
    // https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getlastpresentcount
    // https://learn.microsoft.com/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics
    if (!statistics.PresentCount || statistics.PresentCount > observation.submitted ||
        !statistics.PresentRefreshCount || statistics.SyncRefreshCount < statistics.PresentRefreshCount ||
        statistics.SyncQPCTime.QuadPart <= 0 || statistics.SyncQPCTime.QuadPart > now.QuadPart)
        fail("actual-presentation-refresh-clock-contract");
    return observation;
}
}
