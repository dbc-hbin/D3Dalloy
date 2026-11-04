// Ordinary placed attachments: exact pixels and immutable replay first; native
// work counts are checked independently from the bounded phase trace.
void placedRenderPhase(const char *name, UINT replay, bool begin) {
  graphPhaseLine("%s_PLACED_PHASE %s=%s replay=%u\n", kPrefix, begin ? "begin" : "end", name, replay);
}

void placedRenderGroups(UINT control) {
  Context c;
  constexpr auto colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr auto depthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  constexpr const char *names[] = {"same-list-union", "committed-leader", "cross-list-boundary"};
  constexpr const char *cases[] = {"placed-render-same-list-union", "placed-render-committed-leader",
      "placed-render-cross-list-boundary"};
  const D3D12_RESOURCE_DESC descriptions[] = {
      texDesc(colorFormat, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
      texDesc(colorFormat, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
      texDesc(depthFormat, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)};
  D3D12_RESOURCE_ALLOCATION_INFO1 placements[3]{};
  const auto allocation = c.dev->GetResourceAllocationInfo1(0, 3, descriptions, placements);
  if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX || !allocation.Alignment)
    fail("placed render allocation");
  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = allocation.SizeInBytes;
  heapDesc.Alignment = allocation.Alignment;
  heapDesc.Properties = heap(D3D12_HEAP_TYPE_DEFAULT);
  heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
  ComPtr<ID3D12Heap> physical;
  check(c.dev->CreateHeap(&heapDesc, IID_PPV_ARGS(&physical)), "placed render heap");
  ComPtr<ID3D12Resource> surfaces[3];
  for (UINT i = 0; i < 3; ++i) {
    if (!placements[i].SizeInBytes || !placements[i].Alignment ||
        placements[i].Offset % placements[i].Alignment ||
        (i && placements[i].Offset < placements[i - 1].Offset + placements[i - 1].SizeInBytes))
      fail("placed render disjoint intervals");
    const auto state = i == 2 ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (control == 1 && i != 1)
      surfaces[i] = committed(c.dev.Get(), D3D12_HEAP_TYPE_DEFAULT, descriptions[i], state);
    else
      check(c.dev->CreatePlacedResource(physical.Get(), placements[i].Offset, &descriptions[i], state, nullptr,
          IID_PPV_ARGS(&surfaces[i])), "placed render surface");
  }
  D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
  check(c.dev->CreateDescriptorHeap(&views, IID_PPV_ARGS(&rtvHeap)), "placed render RTV heap");
  const auto start = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = c.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = {start, {start.ptr + stride}};
  for (UINT i = 0; i < 2; ++i) c.dev->CreateRenderTargetView(surfaces[i].Get(), nullptr, rtvs[i]);
  const auto dsv = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, dsvHeap);
  c.dev->CreateDepthStencilView(surfaces[2].Get(), nullptr, dsv);
  Draw red(c.dev.Get(), colorFormat, depthFormat, 1, "psMain", true, true);
  Draw green(c.dev.Get(), colorFormat, depthFormat, 1, "psGreen", true, true, 8);
  auto initialize = c.list(), first = c.list(), second = c.list(), read = c.list();
  const float black[] = {0, 0, 0, 1};
  for (const auto rtv : rtvs) initialize.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  initialize.list->ClearDepthStencilView(dsv,
      D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 1.0f, 0x5a, 0, nullptr);
  const float colors[][4] = {{32 / 255.0f, 0, 0, 0}, {0, 0, 64 / 255.0f, 0},
      {16 / 255.0f, 0, 0, 0}, {0, 0, 32 / 255.0f, 0}, {128 / 255.0f, 0, 0, 0}};
  const float depths[] = {0.75f, 0.5f, 0.25f, 0.125f, 0.875f};
  for (UINT draw = 0; draw < 5; ++draw) {
    auto *list = control == 2 && draw >= 3 ? second.list.Get() : first.list.Get();
    list->OMSetRenderTargets(1, &rtvs[draw & 1], FALSE, &dsv);
    (draw & 1 ? green : red).draw(list, 4, 4, colors[draw], depths[draw]);
  }
  Readback color[2], depth, stencil;
  for (UINT i = 0; i < 3; ++i) {
    const auto state = i == 2 ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    transition(read.list.Get(), surfaces[i].Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (i < 2) color[i] = copyOut(c.dev.Get(), read.list.Get(), surfaces[i].Get());
    else {
      depth = copyOut(c.dev.Get(), read.list.Get(), surfaces[i].Get(), 0);
      stencil = copyOut(c.dev.Get(), read.list.Get(), surfaces[i].Get(), 1);
    }
    transition(read.list.Get(), surfaces[i].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  }
  for (auto *list : {initialize.list.Get(), first.list.Get(), second.list.Get(), read.list.Get()})
    check(list->Close(), "placed render close");
  for (UINT replay = 0; replay < 2; ++replay) {
    std::vector<ID3D12CommandList *> batch{initialize.list.Get(), first.list.Get()};
    if (control == 2) batch.push_back(second.list.Get());
    placedRenderPhase(names[control], replay, true);
    const bool lifetime = control == 0 && replay == 1;
    if (lifetime) {
      ComPtr<ID3D12Fence> gate;
      check(c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "placed render gate");
      check(c.queue->Wait(gate.Get(), 1), "placed render gate wait");
      c.queue->ExecuteCommandLists(UINT(batch.size()), batch.data());
      ComPtr<ID3D12CommandAllocator> replacement;
      check(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)),
          "placed render replacement allocator");
      expect(first.allocator->Reset(), E_FAIL, "placed render live allocator reset");
      check(first.list->Reset(replacement.Get(), nullptr), "placed render reset list");
      check(first.list->Close(), "placed render reset close");
      // RTVs are captured by value at recording. Mutating and releasing their
      // CPU heap after submission must not retarget the frozen attachment map.
      c.dev->CreateRenderTargetView(surfaces[1].Get(), nullptr, rtvs[0]);
      c.dev->CreateRenderTargetView(surfaces[0].Get(), nullptr, rtvs[1]);
      rtvHeap.Reset(); dsvHeap.Reset();
      first.list.Reset();
      // A command list may be reset/released after submission. Keep the PSOs,
      // root signatures, physical heap and original allocator owned through
      // completion and the following readback; public GPU lifetime is ours.
      check(gate->Signal(1), "placed render gate signal");
      c.executeSealed({});
    } else {
      c.executeSealed(batch);
    }
    placedRenderPhase(names[control], replay, false);
    c.executeSealed({read.list.Get()});
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x)
        if (color[0].at<uint32_t>(x, y) != 0xff000030u || color[1].at<uint32_t>(x, y) != 0xff006000u ||
            depth.at<uint32_t>(x, y) != 0x3e000000u || stencil.at<uint8_t>(x, y) != 0x5a)
          fail(cases[control]);
    std::printf("%s_PLACED_GROUP case=%s replay=%u first=%08x second=%08x depth=%08x stencil=%02x "
        "draws=5 sealed=1 lifetime_reset=%u exact=1\n", kPrefix, names[control], replay,
        color[0].at<uint32_t>(0), color[1].at<uint32_t>(0), depth.at<uint32_t>(0), stencil.at<uint8_t>(0),
        unsigned(lifetime));
  }
  pass(cases[control]);
}

void placedRenderAlias(bool depthAlias) {
  Context c;
  constexpr auto colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  constexpr auto depthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  const char *name = depthAlias ? "depth-color-alias" : "color-color-alias";
  const char *caseName = depthAlias ? "placed-render-depth-color-alias" : "placed-render-color-color-alias";
  const D3D12_RESOURCE_DESC descriptions[] = {
      texDesc(depthAlias ? depthFormat : colorFormat, 4, 4,
          depthAlias ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
      texDesc(colorFormat, 4, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)};
  const auto allocation = c.dev->GetResourceAllocationInfo(0, 2, descriptions);
  if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX || !allocation.Alignment)
    fail("placed alias allocation");
  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = allocation.SizeInBytes; heapDesc.Alignment = allocation.Alignment;
  heapDesc.Properties = heap(D3D12_HEAP_TYPE_DEFAULT); heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
  ComPtr<ID3D12Heap> physical;
  check(c.dev->CreateHeap(&heapDesc, IID_PPV_ARGS(&physical)), "placed alias heap");
  ComPtr<ID3D12Resource> surfaces[2];
  ComPtr<ID3D12DescriptorHeap> viewHeaps[2];
  D3D12_CPU_DESCRIPTOR_HANDLE handles[2];
  const auto firstState = depthAlias ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  for (UINT i = 0; i < 2; ++i) {
    check(c.dev->CreatePlacedResource(physical.Get(), 0, &descriptions[i], i ? D3D12_RESOURCE_STATE_RENDER_TARGET : firstState,
        nullptr, IID_PPV_ARGS(&surfaces[i])), "placed overlapping surface");
    handles[i] = descriptor(c.dev.Get(), !i && depthAlias ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV :
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, viewHeaps[i]);
    if (!i && depthAlias) c.dev->CreateDepthStencilView(surfaces[i].Get(), nullptr, handles[i]);
    else c.dev->CreateRenderTargetView(surfaces[i].Get(), nullptr, handles[i]);
  }
  Draw first(c.dev.Get(), depthAlias ? DXGI_FORMAT_UNKNOWN : colorFormat,
      depthAlias ? depthFormat : DXGI_FORMAT_UNKNOWN, 1, "psMain", true, depthAlias);
  Draw second(c.dev.Get(), colorFormat, DXGI_FORMAT_UNKNOWN, 1, "psGreen", true, false, 8);
  auto commands = c.list(), read = c.list();
  auto activate = [&](ID3D12Resource *before, ID3D12Resource *after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing = {before, after}; commands.list->ResourceBarrier(1, &barrier);
  };
  const float black[] = {0, 0, 0, 1}, blue[] = {0, 0, 32 / 255.0f, 1};
  const float red[] = {32 / 255.0f, 0, 0, 0}, green[] = {0, 0, 64 / 255.0f, 0};
  activate(nullptr, surfaces[0].Get());
  if (depthAlias) {
    commands.list->ClearDepthStencilView(handles[0],
        D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 1.0f, 0x5a, 0, nullptr);
    commands.list->OMSetRenderTargets(0, nullptr, FALSE, &handles[0]);
  } else {
    commands.list->ClearRenderTargetView(handles[0], black, 0, nullptr);
    commands.list->OMSetRenderTargets(1, &handles[0], FALSE, nullptr);
  }
  first.draw(commands.list.Get(), 4, 4, red, 0.25f);
  transition(commands.list.Get(), surfaces[0].Get(), firstState, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto beforeAlias = copyOut(c.dev.Get(), commands.list.Get(), surfaces[0].Get(), 0);
  Readback preservedStencil;
  if (depthAlias) preservedStencil = copyOut(c.dev.Get(), commands.list.Get(), surfaces[0].Get(), 1);
  transition(commands.list.Get(), surfaces[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, firstState);
  activate(surfaces[0].Get(), surfaces[1].Get());
  commands.list->ClearRenderTargetView(handles[1], blue, 0, nullptr);
  commands.list->OMSetRenderTargets(1, &handles[1], FALSE, nullptr);
  second.draw(commands.list.Get(), 4, 4, green);
  transition(read.list.Get(), surfaces[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto afterAlias = copyOut(c.dev.Get(), read.list.Get(), surfaces[1].Get());
  transition(read.list.Get(), surfaces[1].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  check(commands.list->Close(), "placed alias close"); check(read.list->Close(), "placed alias read close");
  for (UINT replay = 0; replay < 2; ++replay) {
    placedRenderPhase(name, replay, true);
    c.executeSealed({commands.list.Get()});
    placedRenderPhase(name, replay, false);
    c.executeSealed({read.list.Get()});
    const uint32_t expectedFirst = depthAlias ? 0x3e800000u : 0xff000020u;
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x)
        if (beforeAlias.at<uint32_t>(x, y) != expectedFirst || afterAlias.at<uint32_t>(x, y) != 0xff204000u ||
            (depthAlias && preservedStencil.at<uint8_t>(x, y) != 0x5a)) fail(caseName);
    std::printf("%s_PLACED_ALIAS case=%s replay=%u before=%08x after=%08x stencil=%02x "
        "draws=2 sealed=1 exact=1\n", kPrefix, name, replay, beforeAlias.at<uint32_t>(0),
        afterAlias.at<uint32_t>(0), depthAlias ? preservedStencil.at<uint8_t>(0) : 0);
  }
  pass(caseName);
}
