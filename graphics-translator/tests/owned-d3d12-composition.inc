// Public D3D12/DXGI/DComposition consumer. Readback proves source pixels only;
// the held windows are the independent on-screen alpha/matrix oracle.
namespace composition_test {
constexpr char prefix[] = "OWNED_D3D12_COMPOSITION";
unsigned advances = 0;
const char *control = nullptr;
bool visual = false;
[[noreturn]] void error(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr, "%s_FAIL step=%s hr=0x%08lx\n", prefix, step, static_cast<unsigned long>(hr));
  ExitProcess(1);
}
void ok(HRESULT hr, const char *step) { if (FAILED(hr)) error(step, hr); }
void need(bool value, const char *step) { if (!value) error(step); }
void invalid(HRESULT hr, const char *step, HRESULT wanted) { if (hr != wanted) error(step, hr); }
void passed(const char *name) { std::printf("%s_CASE_PASS case=%s\n", prefix, name); }
LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLOSE && visual) { ++advances; return 0; }
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
void hold(const char *phase, HWND left, HWND right) {
  if (!visual) return;
  char title[160];
  std::snprintf(title, sizeof(title), "Owned Composition premul | %s | close advances", phase);
  SetWindowTextA(left, title);
  std::snprintf(title, sizeof(title), "Owned Composition straight | %s | close advances", phase);
  SetWindowTextA(right, title);
  std::printf("%s_VISUAL_READY phase=%s left=%p right=%p control=%s\n", prefix, phase,
              static_cast<void *>(left), static_cast<void *>(right), control ? control : "WM_CLOSE");
  unsigned before = advances;
  ULONGLONG started = GetTickCount64();
  while (advances == before) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (control) {
      FILE *file = std::fopen(control, "r");
      if (file) {
        char token[128]{};
        if (std::fscanf(file, "%127s", token) == 1 && std::strcmp(token, phase) == 0) ++advances;
        std::fclose(file);
      }
    }
    need(GetTickCount64() - started < 300000, "visual-control-timeout");
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
  }
  // Advancing is not a visual PASS: only Main's independent capture can prove it.
  std::printf("%s_VISUAL_ADVANCED phase=%s\n", prefix, phase);
}
void wait(ID3D12Device *device, ID3D12CommandQueue *queue) {
  ComPtr<ID3D12Fence> fence;
  ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "completion-fence");
  ok(queue->Signal(fence.Get(), 1), "completion-signal");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  need(event != nullptr, "completion-event");
  ok(fence->SetEventOnCompletion(1, event), "completion-arm");
  DWORD result = WaitForSingleObject(event, 10000);
  CloseHandle(event);
  need(result == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1, "completion-wait");
}
// Retains command resources while queued work is blocked by the caller's fence.
struct Frame {
  ComPtr<ID3D12Resource> buffer, readback;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT width = 0, height = 0;
  bool straight = false;
  void record(ID3D12Device *device, ID3D12CommandQueue *queue, IDXGISwapChain4 *chain, bool isStraight, UINT index = ~0u) {
    straight = isStraight;
    ok(chain->GetBuffer(index == ~0u ? chain->GetCurrentBackBufferIndex() : index, IID_PPV_ARGS(&buffer)), "buffer");
    auto desc = buffer->GetDesc(); width = static_cast<UINT>(desc.Width); height = desc.Height;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = bytes; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                      nullptr, IID_PPV_ARGS(&readback)), "readback");
    ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    ok(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ok(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "rtv-heap");
    auto rtv = heap->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);
    const float blue[] = {0, 0, straight ? 1.0f : 0.5f, 0.5f};
    list->ClearRenderTargetView(rtv, blue, 0, nullptr);
    // Asymmetric opaque fiducials: red upper-left, white lower-right, yellow narrow top stripe.
    const float red[] = {1, 0, 0, 1}, white[] = {1, 1, 1, 1}, yellow[] = {1, 1, 0, 1};
    D3D12_RECT r{0, 0, 16, 16}, w{48, 40, 64, 56}, y{32, 0, 40, 8}, additive{64, 48, 80, 64};
    list->ClearRenderTargetView(rtv, red, 1, &r); list->ClearRenderTargetView(rtv, white, 1, &w);
    list->ClearRenderTargetView(rtv, yellow, 1, &y);
    // Premultiplied RGB>alpha is additive light, even at alpha=0; straight
    // RGB at alpha=0 contributes nothing. Native composition must preserve it.
    const float zeroAlphaRed[] = {straight ? 1.0f : .25f, 0, 0, 0};
    list->ClearRenderTargetView(rtv, zeroAlphaRed, 1, &additive);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = buffer.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    ok(list->Close(), "close"); ID3D12CommandList *lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
  }
  void verify(const char *name) {
    unsigned char *data = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(footprint.Offset + footprint.Footprint.RowPitch * height)};
    ok(readback->Map(0, &range, reinterpret_cast<void **>(&data)), "map");
    const UINT xs[] = {8, 24, 52, 36, 72}, ys[] = {8, 24, 44, 4, 56};
    const unsigned expected[][4] = {{0, 0, 255, 255}, {straight ? 255u : 128u, 0, 0, 128},
                                  {255, 255, 255, 255}, {0, 255, 255, 255},
                                  {0, 0, straight ? 255u : 64u, 0}};
    for (unsigned i = 0; i != 5; ++i) {
      auto pixel = data + footprint.Offset + ys[i] * footprint.Footprint.RowPitch + xs[i] * 4;
      for (unsigned channel = 0; channel != 4; ++channel)
        need(std::abs(static_cast<int>(pixel[channel]) - static_cast<int>(expected[i][channel])) <= 1, "source-pixel-mismatch");
    }
    D3D12_RANGE noWrite{0, 0}; readback->Unmap(0, &noWrite);
    std::printf("%s_GPU case=%s size=%ux%u samples=5 readback=exact\n", prefix, name, width, height);
  }
};
void render(ID3D12Device *device, ID3D12CommandQueue *queue, IDXGISwapChain4 *chain, bool straight, const char *name) {
  Frame frame; frame.record(device, queue, chain, straight);
  ok(chain->Present(1, 0), "present"); wait(device, queue); frame.verify(name);
}
void matrix_equals(IDXGISwapChain4 *chain, const DXGI_MATRIX_3X2_F &wanted) {
  DXGI_MATRIX_3X2_F actual{}; ok(chain->GetMatrixTransform(&actual), "matrix-get");
  need(std::memcmp(&actual, &wanted, sizeof(actual)) == 0, "matrix-value");
}
} // namespace composition_test

int composition_main(int argc, char **argv) {
  using namespace composition_test;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--visual-proof") == 0) visual = true;
    else if (std::strcmp(argv[i], "--control") == 0 && i + 1 < argc) control = argv[++i];
    else error("argument");
  }
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
  ok(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "factory"); ok(factory->EnumAdapters1(0, &adapter), "adapter");
  ok(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
  using Backend = const char *(WINAPI *)();
  auto backend = reinterpret_cast<Backend>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  need(backend && std::strcmp(backend(), "owned-graphics-d3d12") == 0, "owned-backend");
  std::printf("%s_BACKEND id=owned-graphics-d3d12\n", prefix);
  D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ok(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
  WNDCLASSW cls{}; cls.lpfnWndProc = window_proc; cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"OwnedCompositionPublicConsumer"; cls.hbrBackground = CreateSolidBrush(RGB(0, 255, 0));
  need(RegisterClassW(&cls) != 0, "register-class");
  HWND windows[2]{};
  for (unsigned i = 0; i != 2; ++i) {
    RECT bounds{0, 0, 200, 160}; AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_NOACTIVATE);
    windows[i] = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName, i ? L"Owned Composition straight" : L"Owned Composition premul",
        WS_OVERLAPPEDWINDOW, 60 + i * 260, 80, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, cls.hInstance, nullptr);
    need(windows[i] != nullptr, "create-window"); ShowWindow(windows[i], SW_SHOWNOACTIVATE); UpdateWindow(windows[i]);
  }
  ComPtr<IDCompositionDevice> dcomp, other;
  ok(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&dcomp)), "dcomp-device-null");
  ok(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&other)), "dcomp-other-device");
  // Owned native hosting currently supports opaque destination windows only.
  // A real layered HWND must be rejected, never accepted with opaque output.
  HWND destination = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED, cls.lpszClassName,
      L"Owned Composition destination boundary", WS_OVERLAPPEDWINDOW,
      60, 80, 200, 160, nullptr, nullptr, cls.hInstance, nullptr);
  need(destination != nullptr, "layered-destination-window");
  ComPtr<IDCompositionTarget> destinationTarget;
  invalid(dcomp->CreateTargetForHwnd(destination, TRUE, &destinationTarget), "layered-destination-not-supported", E_NOTIMPL);
  need(!destinationTarget, "layered-destination-output-cleared");
  // WS_EX_TRANSPARENT controls paint/input ordering, not destination opacity.
  need(SetWindowLongPtrW(destination, GWL_EXSTYLE, WS_EX_NOACTIVATE | WS_EX_TRANSPARENT) != 0,
       "ordinary-transparent-destination-style");
  ok(dcomp->CreateTargetForHwnd(destination, TRUE, &destinationTarget), "ordinary-transparent-destination-target");
  destinationTarget.Reset();
  need(DestroyWindow(destination) != FALSE, "destination-boundary-window-release");
  ComPtr<IDCompositionTarget> targets[2]; ComPtr<IDCompositionVisual> visuals[2]; ComPtr<IDXGISwapChain4> chains[2];
  DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = 80; desc.Height = 64; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling = DXGI_SCALING_STRETCH;
  for (unsigned i = 0; i != 2; ++i) {
    desc.AlphaMode = i ? DXGI_ALPHA_MODE_STRAIGHT : DXGI_ALPHA_MODE_PREMULTIPLIED;
    ComPtr<IDXGISwapChain1> base; ok(factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr, &base), "composition-chain");
    ok(base.As(&chains[i]), "composition-chain4");
    DXGI_SWAP_CHAIN_DESC1 returned{}; ok(chains[i]->GetDesc1(&returned), "composition-desc");
    need(returned.AlphaMode == desc.AlphaMode && returned.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD && returned.Width == 80 && returned.Height == 64, "godot-descriptor");
    ok(dcomp->CreateTargetForHwnd(windows[i], TRUE, &targets[i]), "target");
    ok(dcomp->CreateVisual(&visuals[i]), "visual"); ok(visuals[i]->SetContent(chains[i].Get()), "content");
    ok(targets[i]->SetRoot(visuals[i].Get()), "root");
  }
  // Premultiplied two-plane hosting is an SDR contract; reject HDR inputs
  // honestly instead of silently losing extended RGB or alpha semantics.
  auto floatDesc = desc;
  floatDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  floatDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  ComPtr<IDXGISwapChain1> rejectedFloat;
  invalid(factory->CreateSwapChainForComposition(queue.Get(), &floatDesc, nullptr, &rejectedFloat),
          "premul-float-creation-unsupported", DXGI_ERROR_UNSUPPORTED);
  need(!rejectedFloat, "premul-float-no-chain");
  invalid(chains[0]->ResizeBuffers(3, 96, 80, DXGI_FORMAT_R16G16B16A16_FLOAT, 0),
          "premul-float-resize-unsupported", DXGI_ERROR_UNSUPPORTED);
  DXGI_SWAP_CHAIN_DESC1 preserved{};
  ok(chains[0]->GetDesc1(&preserved), "premul-float-resize-preserved-desc");
  need(preserved.Width == 80 && preserved.Height == 64 && preserved.BufferCount == 2 &&
       preserved.Format == DXGI_FORMAT_B8G8R8A8_UNORM && preserved.AlphaMode == DXGI_ALPHA_MODE_PREMULTIPLIED &&
       chains[0]->GetCurrentBackBufferIndex() == 0, "premul-float-resize-preserves-chain");
  UINT preservedWidth = 0, preservedHeight = 0;
  ok(chains[0]->GetSourceSize(&preservedWidth, &preservedHeight), "premul-float-resize-preserved-source");
  need(preservedWidth == 80 && preservedHeight == 64, "premul-float-resize-preserves-source");
  UINT colorSupport = 0;
  ok(chains[0]->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &colorSupport), "premul-sdr-color-support");
  need(colorSupport == DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT, "premul-sdr-present-support");
  ok(chains[0]->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709), "premul-sdr-color-space");
  const DXGI_COLOR_SPACE_TYPE unsupportedColors[] = {DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
      DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020};
  for (auto space : unsupportedColors) {
    colorSupport = ~0u;
    ok(chains[0]->CheckColorSpaceSupport(space, &colorSupport), "premul-hdr-color-support-query");
    need(colorSupport == 0, "premul-hdr-color-unadvertised");
    invalid(chains[0]->SetColorSpace1(space), "premul-hdr-color-unsupported", DXGI_ERROR_UNSUPPORTED);
  }
  // Same valid HDR10 data as the existing owned-d3d12-hdr-metadata consumer.
  DXGI_HDR_METADATA_HDR10 hdr{};
  hdr.RedPrimary[0] = 34000; hdr.RedPrimary[1] = 16000;
  hdr.GreenPrimary[0] = 13250; hdr.GreenPrimary[1] = 34500;
  hdr.BluePrimary[0] = 7500; hdr.BluePrimary[1] = 3000;
  hdr.WhitePoint[0] = 15635; hdr.WhitePoint[1] = 16450;
  hdr.MaxMasteringLuminance = 1000; hdr.MinMasteringLuminance = 1;
  hdr.MaxContentLightLevel = 900; hdr.MaxFrameAverageLightLevel = 450;
  invalid(chains[0]->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr) - 1, &hdr),
          "premul-hdr10-malformed-size", E_INVALIDARG);
  invalid(chains[0]->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(hdr), &hdr),
          "premul-hdr10-unsupported", DXGI_ERROR_UNSUPPORTED);
  ok(chains[0]->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr), "premul-hdr-none");
  render(device.Get(), queue.Get(), chains[0].Get(), false, "premul");
  render(device.Get(), queue.Get(), chains[1].Get(), true, "straight");
  ok(dcomp->Commit(), "initial-commit-after-unbound-present");
  hold("identity", windows[0], windows[1]); passed("godot_alpha_present");
  DXGI_MATRIX_3X2_F identity{1, 0, 0, 1, 0, 0}, transformed{1.25f, 0, 0, .75f, 20, 15};
  for (auto &chain : chains) {
    matrix_equals(chain.Get(), identity); ok(chain->SetMatrixTransform(&transformed), "matrix-set"); matrix_equals(chain.Get(), transformed);
    invalid(chain->SetMatrixTransform(nullptr), "matrix-set-null", E_INVALIDARG);
    invalid(chain->GetMatrixTransform(nullptr), "matrix-get-null", E_INVALIDARG);
    const float nonfinite[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (unsigned field = 0; field != 6; ++field) {
      for (float bad : nonfinite) {
        DXGI_MATRIX_3X2_F rejected = transformed;
        switch (field) { case 0: rejected._11 = bad; break; case 1: rejected._12 = bad; break; case 2: rejected._21 = bad; break;
                         case 3: rejected._22 = bad; break; case 4: rejected._31 = bad; break; case 5: rejected._32 = bad; break; }
        invalid(chain->SetMatrixTransform(&rejected), "matrix-nonfinite", E_INVALIDARG); matrix_equals(chain.Get(), transformed);
      }
    }
    auto skew = transformed; skew._12 = .25f; invalid(chain->SetMatrixTransform(&skew), "matrix-skew12", E_INVALIDARG);
    skew = transformed; skew._21 = -.25f; invalid(chain->SetMatrixTransform(&skew), "matrix-skew21", E_INVALIDARG); matrix_equals(chain.Get(), transformed);
    auto finite = identity; finite._11 = -1; finite._22 = 0;
    ok(chain->SetMatrixTransform(&finite), "finite-negative-zero-scale"); matrix_equals(chain.Get(), finite);
    ok(chain->SetMatrixTransform(&transformed), "restore-requested-matrix");
  }
  // Getter is new while the displayed frame must still occupy [0,80)x[0,64).
  hold("matrix-pending", windows[0], windows[1]);
  render(device.Get(), queue.Get(), chains[0].Get(), false, "matrix_premul");
  render(device.Get(), queue.Get(), chains[1].Get(), true, "matrix_straight");
  hold("matrix-presented", windows[0], windows[1]); passed("matrix_next_present_validation");
  for (auto &chain : chains) {
    HWND output = reinterpret_cast<HWND>(1); invalid(chain->GetHwnd(&output), "composition-no-hwnd", DXGI_ERROR_INVALID_CALL); need(!output, "no-fake-hwnd");
    ComPtr<IUnknown> core; invalid(chain->GetCoreWindow(IID_PPV_ARGS(&core)), "composition-no-corewindow", DXGI_ERROR_INVALID_CALL); need(!core, "no-fake-corewindow");
    invalid(chain->SetFullscreenState(TRUE, nullptr), "composition-no-exclusive", DXGI_ERROR_INVALID_CALL);
    DXGI_MODE_DESC mode{}; invalid(chain->ResizeTarget(&mode), "composition-no-resizetarget", DXGI_ERROR_INVALID_CALL);
  }
  passed("no_hwnd_restrictions");
  for (auto &chain : chains) {
    ok(chain->SetSourceSize(64, 48), "source-size"); UINT width = 0, height = 0;
    ok(chain->GetSourceSize(&width, &height), "source-size-get"); need(width == 64 && height == 48, "source-size-value");
    invalid(chain->SetSourceSize(81, 48), "source-size-overflow", E_INVALIDARG);
    invalid(chain->SetSourceSize(0, 48), "source-size-zero", E_INVALIDARG);
    ok(chain->GetSourceSize(&width, &height), "source-size-preserved"); need(width == 64 && height == 48, "source-size-preserved-value");
  }
  render(device.Get(), queue.Get(), chains[0].Get(), false, "source_premul"); render(device.Get(), queue.Get(), chains[1].Get(), true, "source_straight");
  hold("source-cropped", windows[0], windows[1]);
  for (auto &chain : chains) {
    ok(chain->ResizeBuffers(3, 96, 80, DXGI_FORMAT_UNKNOWN, 0), "resize-buffers");
    DXGI_SWAP_CHAIN_DESC1 resized{}; ok(chain->GetDesc1(&resized), "resized-desc");
    need(resized.Width == 96 && resized.Height == 80 && resized.BufferCount == 3, "resized-value"); matrix_equals(chain.Get(), transformed);
    UINT width = 0, height = 0; ok(chain->GetSourceSize(&width, &height), "resized-source"); need(width == 96 && height == 80, "resized-source-reset");
  }
  render(device.Get(), queue.Get(), chains[0].Get(), false, "resize_premul"); render(device.Get(), queue.Get(), chains[1].Get(), true, "resize_straight");
  hold("resized", windows[0], windows[1]); passed("source_resize");
  ComPtr<IDCompositionTarget> duplicate, below;
  need(FAILED(other->CreateTargetForHwnd(windows[0], TRUE, &duplicate)) && !duplicate, "target-unique-across-devices");
  ok(other->CreateTargetForHwnd(windows[0], FALSE, &below), "distinct-topmost-slot");
  need(FAILED(below->SetRoot(visuals[0].Get())), "foreign-device-root");
  ComPtr<IDCompositionTarget> sameDeviceBelow; below.Reset();
  ok(dcomp->CreateTargetForHwnd(windows[0], FALSE, &sameDeviceBelow), "same-device-below");
  need(FAILED(sameDeviceBelow->SetRoot(visuals[0].Get())), "root-single-owner");
  ComPtr<IDCompositionVisual> parent; ok(dcomp->CreateVisual(&parent), "parent-visual");
  need(FAILED(parent->AddVisual(visuals[0].Get(), FALSE, nullptr)), "root-not-child");
  sameDeviceBelow.Reset(); parent.Reset();
  for (auto &v : visuals) ok(v->SetContent(nullptr), "null-content-pending");
  hold("content-pending", windows[0], windows[1]); ok(dcomp->Commit(), "null-content-commit"); ok(dcomp->Commit(), "empty-repeat-commit");
  hold("content-detached", windows[0], windows[1]);
  for (unsigned i = 0; i != 2; ++i) ok(visuals[i]->SetContent(chains[i].Get()), "restore-content");
  ok(dcomp->Commit(), "content-reattach");
  for (auto &target : targets) ok(target->SetRoot(nullptr), "null-root-pending");
  hold("root-pending", windows[0], windows[1]); ok(dcomp->Commit(), "null-root-commit");
  hold("root-detached", windows[0], windows[1]);
  for (unsigned i = 0; i != 2; ++i) ok(targets[i]->SetRoot(visuals[i].Get()), "root-reattach");
  ok(dcomp->Commit(), "root-reattach-commit");
  targets[0].Reset(); ok(dcomp->Commit(), "released-target-commit");
  ok(dcomp->CreateTargetForHwnd(windows[0], TRUE, &targets[0]), "target-release-slot");
  ok(targets[0]->SetRoot(visuals[0].Get()), "root-after-owner-release"); ok(dcomp->Commit(), "released-target-reattach");
  hold("reattached", windows[0], windows[1]); passed("commit_detach_ownership");
  ComPtr<ID3D12Fence> gate; ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
  ComPtr<ID3D12CommandQueue> queueA, queueB;
  ok(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queueA)), "ordered-queue-a");
  ok(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queueB)), "ordered-queue-b");
  UINT nodeMasks[] = {1, 1}; IUnknown *presentQueues[] = {queueA.Get(), queueB.Get()};
  Frame queued[2][2];
  for (unsigned i = 0; i != 2; ++i) {
    ok(chains[i]->ResizeBuffers1(2, 96, 80, DXGI_FORMAT_UNKNOWN, 0, nodeMasks, presentQueues), "per-buffer-present-queues");
    need(chains[i]->GetCurrentBackBufferIndex() == 0, "resized-first-buffer");
    // Pre-render both buffers before blocking A. B's pending completion then
    // exercises Present ordering, not an unrelated unfinished render dependency.
    queued[i][0].record(device.Get(), queue.Get(), chains[i].Get(), i != 0, 0);
    queued[i][1].record(device.Get(), queue.Get(), chains[i].Get(), i != 0, 1);
  }
  wait(device.Get(), queue.Get());
  ok(queueA->Wait(gate.Get(), 1), "queue-a-block");
  ComPtr<ID3D12Fence> completedA, completedB;
  ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completedA)), "queue-a-retirement-fence");
  ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completedB)), "queue-b-retirement-fence");
  for (unsigned i = 0; i != 2; ++i) {
    ok(chains[i]->SetMatrixTransform(&transformed), "queued-a-captured-transform");
    ok(chains[i]->Present(1, 0), "queued-a-present");
    need(chains[i]->GetCurrentBackBufferIndex() == 1, "next-buffer-uses-queue-b");
    ok(chains[i]->SetMatrixTransform(&identity), "queued-b-captured-identity");
    ok(chains[i]->Present(1, 0), "queued-b-present");
    ok(chains[i]->SetMatrixTransform(&transformed), "post-present-request"); matrix_equals(chains[i].Get(), transformed);
  }
  ok(queueA->Signal(completedA.Get(), 1), "queue-a-logical-completion");
  ok(queueB->Signal(completedB.Get(), 1), "queue-b-logical-completion");
  HANDLE completedBEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  need(completedBEvent != nullptr, "queue-b-completion-event");
  ok(completedB->SetEventOnCompletion(1, completedBEvent), "queue-b-completion-arm");
  // Same 500ms bounded blocked probe as owned-d3d12-present-queues.cpp.
  // Present B must not overtake A even though B's source buffer is already ready.
  need(WaitForSingleObject(completedBEvent, 500) == WAIT_TIMEOUT &&
       completedB->GetCompletedValue() == 0 && completedA->GetCompletedValue() == 0,
       "queue-b-cannot-overtake-blocked-a");
  std::printf("%s_QUEUE_ORDER phase=before_release completed_b=0 completed_a=0\n", prefix);
  hold("queued-before-release", windows[0], windows[1]);
  // Pending queued Present and visual content jointly protect the native surface.
  for (auto &chain : chains) chain.Reset();
  ok(gate->Signal(1), "release-gate");
  need(WaitForSingleObject(completedBEvent, 10000) == WAIT_OBJECT_0 && completedB->GetCompletedValue() >= 1,
       "queue-b-completed-after-release");
  CloseHandle(completedBEvent);
  wait(device.Get(), queueA.Get());
  need(completedA->GetCompletedValue() >= 1, "queue-a-completed-after-release");
  std::printf("%s_QUEUE_ORDER phase=after_release completed_b=1 completed_a=1\n", prefix);
  queued[0][0].verify("queued_a_premul"); queued[0][1].verify("queued_b_premul");
  queued[1][0].verify("queued_a_straight"); queued[1][1].verify("queued_b_straight");
  hold("queued-captured-identity", windows[0], windows[1]);
  // Visual owns content after all application swapchain references disappear.
  ok(dcomp->Commit(), "content-retained-commit");
  hold("content-lifetime", windows[0], windows[1]);
  // Destroy a real target window and drop all content references with a Present
  // already captured behind a blocked queue. Only queued work owns its surface.
  ComPtr<IDXGISwapChain1> destructionBase; ComPtr<IDXGISwapChain4> destructionChain;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  ok(factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr, &destructionBase), "destroy-chain");
  ok(destructionBase.As(&destructionChain), "destroy-chain4");
  ok(visuals[0]->SetContent(destructionChain.Get()), "destroy-chain-content"); ok(dcomp->Commit(), "destroy-chain-attach");
  ok(queue->Wait(gate.Get(), 2), "destroy-queue-block");
  Frame destroyed; destroyed.record(device.Get(), queue.Get(), destructionChain.Get(), false);
  ok(destructionChain->Present(1, 0), "destroy-queued-present");
  destructionChain.Reset(); destructionBase.Reset();
  need(DestroyWindow(windows[0]) != FALSE, "destroy-while-queued");
  ok(visuals[0]->SetContent(nullptr), "destroy-pending-content"); ok(targets[0]->SetRoot(nullptr), "destroy-pending-root");
  ok(dcomp->Commit(), "destroy-detach-before-present-completion");
  ok(gate->Signal(2), "destroy-gate-release"); wait(device.Get(), queue.Get()); destroyed.verify("destroyed_window");
  for (unsigned i = 0; i != 2; ++i) {
    if (i != 0) DestroyWindow(windows[i]);
    ok(visuals[i]->SetContent(nullptr), "destroyed-window-detach");
    ok(targets[i]->SetRoot(nullptr), "destroyed-window-root-detach");
  }
  ok(dcomp->Commit(), "destroyed-window-commit");
  for (auto &v : visuals) v.Reset(); for (auto &t : targets) t.Reset();
  passed("queued_present_content_window_lifetime");
  // Ordinary HWND path still draws; composition matrix must never leak into it.
  HWND ordinary = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName, L"Owned Composition HWND regression",
      WS_OVERLAPPEDWINDOW, 60, 80, 240, 200, nullptr, nullptr, cls.hInstance, nullptr);
  need(ordinary != nullptr, "ordinary-window"); ShowWindow(ordinary, SW_SHOWNOACTIVATE);
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  ComPtr<IDXGISwapChain1> base; ComPtr<IDXGISwapChain4> chain;
  ok(factory->CreateSwapChainForHwnd(queue.Get(), ordinary, &desc, nullptr, nullptr, &base), "ordinary-chain");
  ok(base.As(&chain), "ordinary-chain4"); invalid(chain->SetMatrixTransform(&transformed), "ordinary-no-matrix", DXGI_ERROR_INVALID_CALL);
  auto untouched = transformed; invalid(chain->GetMatrixTransform(&untouched), "ordinary-no-get-matrix", DXGI_ERROR_INVALID_CALL);
  need(std::memcmp(&untouched, &transformed, sizeof(untouched)) == 0, "ordinary-get-preserves");
  render(device.Get(), queue.Get(), chain.Get(), false, "ordinary_hwnd");
  chain.Reset(); base.Reset(); DestroyWindow(ordinary); passed("ordinary_hwnd_regression");
  dcomp.Reset(); other.Reset(); UnregisterClassW(cls.lpszClassName, cls.hInstance); DeleteObject(cls.hbrBackground);
  std::printf("%s_PASS cases=7 gpu=14 visual=%s corewindow=separate-prerequisite\n", prefix, visual ? "external-capture-required" : "not-observed");
  return 0;
}
