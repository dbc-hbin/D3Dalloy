// Public Present1 history oracle. The CPU image always applies previous image,
// scroll, and dirty pixels in that order; it does not use the full-dirty shortcut.
void runPresentHistory() {
  constexpr UINT width = 8, height = 6, pixels = width * height;
  struct Frame {
    const char *name;
    UINT dirty_count;
    RECT dirty[2];
    bool scroll;
    RECT scroll_rect;
    POINT scroll_offset;
    bool crop;
  };
  const Frame frames[] = {
      {"seed", 0, {}, false, {}, {}, false},
      {"full-dirty-scroll", 1, {{0, 0, 8, 6}}, true, {0, 0, 8, 6}, {-2, 1}, false},
      {"partial-dirty-scroll", 1, {{0, 0, 2, 2}}, true, {1, 1, 8, 6}, {-2, 1}, false},
      {"full-dirty-list", 2, {{3, 1, 5, 4}, {0, 0, 8, 6}}, false, {}, {}, false},
      {"partial-after-full", 1, {{1, 2, 4, 5}}, false, {}, {}, false},
      {"full-dirty-cropped", 1, {{0, 0, 8, 6}}, false, {}, {}, true},
      {"dirty-crop-only", 1, {{0, 0, 5, 4}}, false, {}, {}, true},
      {"overlap-positive-scroll", 2, {{0, 0, 3, 2}, {2, 1, 5, 3}}, true, {0, 0, 7, 5}, {1, 1}, false},
      {"stationary-scroll", 1, {{4, 3, 7, 5}}, true, {0, 0, 8, 6}, {0, 0}, false},
      {"large-inner-scroll", 1, {{1, 1, 7, 5}}, true, {0, 0, 8, 6}, {-1, 1}, true},
      {"large-edge-dirty", 1, {{0, 0, 8, 5}}, false, {}, {}, false},
      {"adjacent-full", 2, {{0, 0, 3, 6}, {3, 0, 8, 6}}, false, {}, {}, false},
      {"bands-cover-scroll", 2, {{1, 0, 5, 5}, {3, 0, 8, 5}}, true, {0, 0, 7, 4}, {1, 1}, true},
      {"large-dirty-gap", 2, {{0, 0, 5, 6}, {6, 0, 8, 6}}, true, {0, 1, 7, 4}, {1, 0}, false},
      {"corner-gap", 2, {{0, 0, 3, 3}, {3, 3, 8, 6}}, true, {0, 0, 8, 6}, {-2, 1}, false},
      {"staggered-cover-scroll", 2, {{0, 0, 4, 6}, {3, 1, 8, 6}}, true, {0, 0, 7, 4}, {1, 1}, false},
  };
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "history-factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "history-adapter");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "history-device");
  auto queue = createQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  auto alternate_queue = createQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
  ID3D12CommandQueue *submission_queue = queue.Get();
  Window window;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  ComPtr<IDXGISwapChain1> base;
  check(factory->CreateSwapChainForHwnd(queue.Get(), window.get(), &desc, nullptr, nullptr, &base),
        "history-swapchain");
  ComPtr<IDXGISwapChain3> swapchain;
  check(base.As(&swapchain), "history-swapchain3");
  const UINT node_masks[] = {1, 1};
  IUnknown *present_queues[] = {queue.Get(), alternate_queue.Get()};
  check(swapchain->ResizeBuffers1(2, width, height, desc.Format, desc.Flags, node_masks, present_queues),
        "history-present-queues");
  ComPtr<ID3D12Resource> backbuffers[2];
  for (UINT i = 0; i < 2; ++i)
    check(swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i])), "history-backbuffer");

  const auto texture_desc = backbuffers[0]->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows = 0;
  UINT64 row_bytes = 0, total_bytes = 0;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &total_bytes);
  require(rows == height && row_bytes == width * sizeof(UINT), "history-footprint");
  auto createBuffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, state, nullptr,
                                          IID_PPV_ARGS(&result)), "history-buffer");
    return result;
  };
  auto upload = createBuffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto readback = createBuffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12CommandAllocator> allocator;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
        "history-allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                  IID_PPV_ARGS(&list)), "history-command-list");
  check(list->Close(), "history-initial-close");
  auto reset = [&] {
    // Each previous Execute/Present has reached an actual GPU fence before reset.
    check(allocator->Reset(), "history-reset-allocator");
    check(list->Reset(allocator.Get(), nullptr), "history-reset-list");
  };
  auto transition = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
  };
  auto execute = [&] {
    check(list->Close(), "history-close");
    ID3D12CommandList *submitted[] = {list.Get()};
    submission_queue->ExecuteCommandLists(1, submitted);
  };
  auto phase = [](const char *edge, const char *name) {
    char marker[192];
    const int length = std::snprintf(marker, sizeof(marker),
        "OWNED_D3D12_PRESENT_HISTORY_PHASE %s=%s\n", edge, name);
    require(length > 0 && size_t(length) < sizeof(marker), "history-phase-format");
    DWORD written = 0;
    require(WriteFile(GetStdHandle(STD_ERROR_HANDLE), marker, DWORD(length), &written, nullptr) &&
            written == DWORD(length), "history-phase-write");
  };

  UINT previous[pixels]{};
  for (UINT frame_index = 0; frame_index < sizeof(frames) / sizeof(frames[0]); ++frame_index) {
    const auto &frame = frames[frame_index];
    UINT input[pixels], expected[pixels];
    for (UINT y = 0; y < height; ++y)
      for (UINT x = 0; x < width; ++x) {
        const UINT seed = frame_index + 1;
        input[y * width + x] = 0xff000000u | (seed * 17 + x * 3) |
                              ((seed * 11 + y * 5) << 8) | ((seed * 7 + x + y) << 16);
      }
    std::memcpy(expected, previous, sizeof(expected));
    if (frame.scroll)
      for (LONG y = frame.scroll_rect.top; y < frame.scroll_rect.bottom; ++y)
        for (LONG x = frame.scroll_rect.left; x < frame.scroll_rect.right; ++x) {
          const LONG dx = x + frame.scroll_offset.x, dy = y + frame.scroll_offset.y;
          if (dx >= 0 && dx < LONG(width) && dy >= 0 && dy < LONG(height))
            expected[dy * width + dx] = previous[y * width + x];
        }
    if (!frame.dirty_count)
      std::memcpy(expected, input, sizeof(expected));
    else
      for (UINT r = 0; r < frame.dirty_count; ++r)
        for (LONG y = frame.dirty[r].top; y < frame.dirty[r].bottom; ++y)
          for (LONG x = frame.dirty[r].left; x < frame.dirty[r].right; ++x)
            expected[y * width + x] = input[y * width + x];

    check(swapchain->SetSourceSize(frame.crop ? 5 : width, frame.crop ? 4 : height), "history-source-size");
    const UINT buffer_index = swapchain->GetCurrentBackBufferIndex();
    require(buffer_index < 2, "history-current-buffer");
    submission_queue = buffer_index ? alternate_queue.Get() : queue.Get();
    auto *backbuffer = backbuffers[buffer_index].Get();
    ComPtr<ID3D12Fence> queued_full;
    if (frame_index == 3) {
      // Present the old pixels, then overwrite this same public buffer while
      // both full Presents are queued. The following partial needs the new pixels.
      check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queued_full)), "history-full-gate");
      check(submission_queue->Wait(queued_full.Get(), 1), "history-full-wait");
      check(swapchain->Present(0, DXGI_PRESENT_DO_NOT_SEQUENCE), "history-queued-full");
    }
    BYTE *upload_data = nullptr;
    const D3D12_RANGE no_read{0, 0};
    check(upload->Map(0, &no_read, reinterpret_cast<void **>(&upload_data)), "history-upload-map");
    for (UINT y = 0; y < height; ++y)
      std::memcpy(upload_data + footprint.Offset + y * footprint.Footprint.RowPitch,
                  input + y * width, width * sizeof(UINT));
    upload->Unmap(0, nullptr);

    reset();
    transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    destination.pResource = backbuffer;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const RECT whole{0, 0, LONG(width), LONG(height)};
    const UINT copy_count = frame.dirty_count ? frame.dirty_count : 1;
    for (UINT r = 0; r < copy_count; ++r) {
      const auto &rect = frame.dirty_count ? frame.dirty[r] : whole;
      const D3D12_BOX box{UINT(rect.left), UINT(rect.top), 0, UINT(rect.right), UINT(rect.bottom), 1};
      // Partial Presents modify only their reported dirty pixels, including every
      // pixel in each rect. Unchanged/scroll pixels must come from history.
      list->CopyTextureRegion(&destination, UINT(rect.left), UINT(rect.top), 0, &source, &box);
    }
    transition(backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    execute();
    DXGI_PRESENT_PARAMETERS parameters{};
    parameters.DirtyRectsCount = frame.dirty_count;
    parameters.pDirtyRects = frame.dirty_count ? const_cast<RECT *>(frame.dirty) : nullptr;
    parameters.pScrollRect = frame.scroll ? const_cast<RECT *>(&frame.scroll_rect) : nullptr;
    parameters.pScrollOffset = frame.scroll ? const_cast<POINT *>(&frame.scroll_offset) : nullptr;
    phase("begin", frame.name);
    check(swapchain->Present1(0, 0, &parameters), "history-present");
    if (queued_full) check(queued_full->Signal(1), "history-release-full");
    waitIdle(device.Get(), submission_queue);
    phase("end", frame.name);

    // FLIP_SEQUENTIAL preserves the actual backbuffer contents after Present1.
    // Read every pixel, including those outside a cropped presentation region.
    reset();
    transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    source = {};
    source.pResource = backbuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    execute();
    waitIdle(device.Get(), submission_queue);
    BYTE *readback_data = nullptr;
    const D3D12_RANGE read_range{0, SIZE_T(total_bytes)};
    check(readback->Map(0, &read_range, reinterpret_cast<void **>(&readback_data)), "history-readback-map");
    UINT hash = 2166136261u;
    for (UINT y = 0; y < height; ++y)
      for (UINT x = 0; x < width; ++x) {
        UINT actual;
        std::memcpy(&actual, readback_data + footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(UINT),
                    sizeof(actual));
        if (actual != expected[y * width + x]) {
          std::fprintf(stderr,
              "OWNED_D3D12_PRESENT_HISTORY_FAIL case=%s x=%u y=%u expected=%08x actual=%08x\n",
              frame.name, x, y, expected[y * width + x], actual);
          fail("history-pixel");
        }
        hash = (hash ^ actual) * 16777619u;
      }
    readback->Unmap(0, &no_read);
    std::memcpy(previous, expected, sizeof(previous));
    std::printf("OWNED_D3D12_PRESENT_HISTORY_CASE case=%s pixels=48 hash=%08x exact=1\n", frame.name, hash);
  }
  std::puts("OWNED_D3D12_PRESENT_HISTORY_PASS cases=16 pixels=768 full_dirty=4 partial=11 crop=4 queues=2 exact=1");
}
