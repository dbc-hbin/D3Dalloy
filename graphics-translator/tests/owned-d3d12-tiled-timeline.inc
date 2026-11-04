// One public API oracle for asynchronous mapping publication. Every payload is
// independently initialized; expected bytes do not use runtime mapping metadata.
void runMappingTimeline(Context &c) {
  constexpr UINT rounds = 36, split = 18;
  constexpr UINT64 rowBytes = 2ull * kTile;
  std::puts("OWNED_D3D12_TILED_TIMELINE_BEGIN waves=2 rounds=36 closed_list_count=36 replay=72 handoff=18");
  std::fflush(stdout);
  D3D12_HEAP_DESC heapDesc{};
  heapDesc.SizeInBytes = 2ull * kTile;
  heapDesc.Properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ComPtr<ID3D12Heap> heap;
  check(c.device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap)), "timeline physical heap");
  ComPtr<ID3D12Resource> backing[2], target, snapshot;
  const auto pageDesc = bufferDesc(kTile);
  const auto targetDesc = bufferDesc(2ull * kTile);
  for (UINT page = 0; page < 2; ++page)
    check(c.device->CreatePlacedResource(heap.Get(), UINT64(page) * kTile, &pageDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&backing[page])), "timeline physical page");
  check(c.device->CreateReservedResource(&targetDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
      IID_PPV_ARGS(&target)), "timeline target");
  check(c.device->CreateReservedResource(&pageDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
      IID_PPV_ARGS(&snapshot)), "timeline mapping snapshot");
  const auto upload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, rowBytes, "timeline upload");
  const auto staging = c.committed(D3D12_HEAP_TYPE_DEFAULT, bufferDesc(rowBytes),
      D3D12_RESOURCE_STATE_COPY_DEST, "timeline staging");
  const auto readback = c.buffer(D3D12_HEAP_TYPE_READBACK, rounds * rowBytes, "timeline readback");
  void *data = nullptr;
  check(upload->Map(0, nullptr, &data), "timeline initialize");
  std::memset(data, 0x31, kTile);
  std::memset(static_cast<BYTE *>(data) + kTile, 0x72, kTile);
  upload->Unmap(0, nullptr);
  c.reset();
  for (UINT page = 0; page < 2; ++page) {
    c.list->CopyBufferRegion(backing[page].Get(), 0, upload.Get(), UINT64(page) * kTile, kTile);
    transition(c.list.Get(), backing[page].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
  }
  D3D12_RESOURCE_BARRIER alias{};
  alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  c.list->ResourceBarrier(1, &alias);
  c.execute("timeline backing close");
  c.finish("timeline backing ready");

  // Each recording executes once per wave. Re-executing a command list before
  // its previous GPU execution retires violates ExecuteCommandLists; the second
  // wave replays these same 36 closed recordings only after public completion.
  // All allocators and public heap/resource owners survive both waves.
  std::vector<ComPtr<ID3D12CommandAllocator>> allocators(rounds);
  std::vector<ComPtr<ID3D12GraphicsCommandList>> copies(rounds);
  for (UINT round = 0; round < rounds; ++round) {
    check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[round])),
          "timeline snapshot allocator");
    check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[round].Get(), nullptr,
        IID_PPV_ARGS(&copies[round])), "timeline snapshot list");
    copies[round]->CopyBufferRegion(staging.Get(), 0, snapshot.Get(), 0, kTile);
    copies[round]->CopyBufferRegion(staging.Get(), kTile, target.Get(), 0, kTile);
    transition(copies[round].Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copies[round]->CopyBufferRegion(readback.Get(), UINT64(round) * rowBytes, staging.Get(), 0, rowBytes);
    transition(copies[round].Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    check(copies[round]->Close(), "timeline snapshot close");
  }
  D3D12_COMMAND_QUEUE_DESC queueDesc{};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> second;
  check(c.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&second)), "timeline second direct queue");
  const D3D12_TILED_RESOURCE_COORDINATE first{}, unused{1, 0, 0, 0};
  const D3D12_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
  auto map = [&](ID3D12CommandQueue *queue, const D3D12_TILED_RESOURCE_COORDINATE &tile,
                 UINT page, D3D12_TILE_MAPPING_FLAGS flags) {
    const D3D12_TILE_RANGE_FLAGS range = page == 2 ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE;
    queue->UpdateTileMappings(target.Get(), 1, &tile, &one, page == 2 ? nullptr : heap.Get(),
                              1, &range, &page, nullptr, flags);
  };
  for (UINT wave = 0; wave < 2; ++wave) {
    for (UINT round = 0; round < rounds; ++round) {
      if (round == split) {
        check(c.queue->Signal(c.fence.Get(), ++c.value), "timeline first queue signal");
        check(second->Wait(c.fence.Get(), c.value), "timeline second queue wait");
      }
      auto *queue = round < split ? c.queue.Get() : second.Get();
      const UINT source = (round + wave) % 3;
      map(queue, first, source, D3D12_TILE_MAPPING_FLAG_NONE);
      queue->CopyTileMappings(snapshot.Get(), &first, target.Get(), &first, &one, D3D12_TILE_MAPPING_FLAG_NONE);
      map(queue, first, source == 0 ? 1 : 0, D3D12_TILE_MAPPING_FLAG_NONE);
      // Only tile 0 is read. Changing tile 1 is a valid, real NO_HAZARD
      // update even while a preceding distinct recording still reads tile 0.
      map(queue, unused, round % 2, D3D12_TILE_MAPPING_FLAG_NO_HAZARD);
      ID3D12CommandList *lists[] = {copies[round].Get()};
      queue->ExecuteCommandLists(1, lists);
    }
    check(second->Signal(c.fence.Get(), ++c.value), "timeline second queue signal");
    check(c.queue->Wait(c.fence.Get(), c.value), "timeline first queue wait");
    c.finish("timeline wave public retirement");
    Words observed(readback);
    for (UINT round = 0; round < rounds; ++round) {
      const UINT source = (round + wave) % 3;
      const UINT expected[] = {source == 2 ? 0u : source == 0 ? 0x31313131u : 0x72727272u,
                              source == 0 ? 0x72727272u : 0x31313131u};
      for (UINT view = 0; view < 2; ++view)
        for (UINT word = 0; word < kTile / sizeof(UINT); ++word)
          expect("timeline immutable snapshot / current target", observed[(UINT64(round) * rowBytes +
                 UINT64(view) * kTile) / sizeof(UINT) + word], expected[view]);
    }
    std::printf("OWNED_D3D12_TILED_TIMELINE_WAVE_PASS wave=%u shift=%u bytes=4718592\n", wave, wave);
    std::fflush(stdout);
  }
  std::puts("OWNED_D3D12_TILED_TIMELINE_PASS waves=2 rounds=36 bytes=9437184 closed_list_count=36 replay=72 null_rounds=24 no_hazard=72 cross_queue=2");
  pass("mapping-gpu-timeline-closed-replay");
}

void runMappingBursts(Context &c, bool scattered) {
  constexpr UINT pages = 25, physical = 4, words = kTile / sizeof(UINT);
  constexpr UINT64 bytes = UINT64(pages) * kTile;
  auto bits = [](UINT h, UINT p, UINT w) { return 0x10000000u | (h << 24) | (p << 18) | w; };
  auto upload = c.buffer(D3D12_HEAP_TYPE_UPLOAD, 2ull * physical * kTile, "burst upload");
  UINT *data = nullptr;
  check(upload->Map(0, nullptr, reinterpret_cast<void **>(&data)), "burst initialize");
  for (UINT h = 0; h < 2; ++h)
    for (UINT p = 0; p < physical; ++p)
      for (UINT w = 0; w < words; ++w) data[(h * physical + p) * words + w] = bits(h, p, w);
  upload->Unmap(0, nullptr);
  ComPtr<ID3D12Heap> heaps[2];
  ComPtr<ID3D12Resource> backing[2], target, companion;
  D3D12_HEAP_DESC hd{};
  hd.SizeInBytes = UINT64(physical) * kTile;
  hd.Properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  auto desc = bufferDesc(hd.SizeInBytes);
  c.reset();
  for (UINT h = 0; h < 2; ++h) {
    check(c.device->CreateHeap(&hd, IID_PPV_ARGS(&heaps[h])), "burst heap");
    check(c.device->CreatePlacedResource(heaps[h].Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&backing[h])), "burst physical contents");
    c.list->CopyBufferRegion(backing[h].Get(), 0, upload.Get(), h * hd.SizeInBytes, hd.SizeInBytes);
    transition(c.list.Get(), backing[h].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
  }
  D3D12_RESOURCE_BARRIER alias{};
  alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  c.list->ResourceBarrier(1, &alias);
  c.execute("burst initialize close");
  c.finish("burst initialize retire");
  desc = bufferDesc(bytes);
  check(c.device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&target)),
        "burst target");
  if (scattered)
    check(c.device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&companion)),
          "burst companion");
  auto staging = c.committed(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST, "burst staging");
  auto readback = c.buffer(D3D12_HEAP_TYPE_READBACK, (scattered ? 4 : 2) * bytes, "burst readback");
  ComPtr<ID3D12CommandAllocator> allocators[2];
  ComPtr<ID3D12GraphicsCommandList> snapshots[2];
  for (UINT i = 0; i < 2; ++i) {
    check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocators[i])),
          "burst snapshot allocator");
    check(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocators[i].Get(), nullptr,
          IID_PPV_ARGS(&snapshots[i])), "burst snapshot list");
    snapshots[i]->CopyBufferRegion(staging.Get(), 0, target.Get(), 0, bytes);
    transition(snapshots[i].Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    snapshots[i]->CopyBufferRegion(readback.Get(), i * bytes, staging.Get(), 0, bytes);
    transition(snapshots[i].Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (scattered) {
      snapshots[i]->CopyBufferRegion(staging.Get(), 0, companion.Get(), 0, bytes);
      transition(snapshots[i].Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
      snapshots[i]->CopyBufferRegion(readback.Get(), (2 + i) * bytes, staging.Get(), 0, bytes);
      transition(snapshots[i].Get(), staging.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    check(snapshots[i]->Close(), "burst snapshot close");
  }
  ComPtr<ID3D12Fence> start, handoff;
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&start)), "burst start");
  check(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&handoff)), "burst handoff");
  check(c.queue->Wait(start.Get(), 1), "burst hold producer");
  auto map = [&](UINT tile, UINT n, UINT h, UINT p, D3D12_TILE_RANGE_FLAGS flag,
                 D3D12_TILE_MAPPING_FLAGS hazard = D3D12_TILE_MAPPING_FLAG_NONE, ID3D12Resource *resource = nullptr) {
    const D3D12_TILED_RESOURCE_COORDINATE at{tile, 0, 0, 0};
    const D3D12_TILE_REGION_SIZE size{n, FALSE, 0, 0, 0};
    c.queue->UpdateTileMappings(resource ? resource : target.Get(), 1, &at, &size, h < 2 ? heaps[h].Get() : nullptr,
                                1, &flag, &p, &n, hazard);
  };
  auto snapshot = [&](UINT i) {
    const UINT64 ready = i * 2 + 1;
    check(c.queue->Signal(handoff.Get(), ready), "burst producer done");
    check(c.queue2->Wait(handoff.Get(), ready), "burst consumer wait");
    ID3D12CommandList *lists[] = {snapshots[i].Get()};
    c.queue2->ExecuteCommandLists(1, lists);
    check(c.queue2->Signal(handoff.Get(), ready + 1), "burst consumer done");
    check(c.queue->Wait(handoff.Get(), ready + 1), "burst producer wait");
  };
  for (UINT i = 0; i < 20; ++i) {
    const UINT t = scattered ? (i * 7) % 20 : i;
    map(t, 1, (t / physical) % 2, t % physical, D3D12_TILE_RANGE_FLAG_NONE);
    if (scattered)
      map(t, 1, 1 - (t / physical) % 2, 3 - t % physical, D3D12_TILE_RANGE_FLAG_NONE,
          D3D12_TILE_MAPPING_FLAG_NONE, companion.Get());
  }
  snapshot(0);
  map(20, 2, 1, 2, D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE);
  map(22, 1, 0, 3, D3D12_TILE_RANGE_FLAG_NONE);
  map(23, 1, 2, 0, D3D12_TILE_RANGE_FLAG_NULL); // already null, in a changed run
  map(22, 1, 1, 1, D3D12_TILE_RANGE_FLAG_NONE); // later overlapping update wins
  const D3D12_TILED_RESOURCE_COORDINATE first{};
  const D3D12_TILE_REGION_SIZE two{2, FALSE, 0, 0, 0};
  const D3D12_TILE_RANGE_FLAGS flags[] = {D3D12_TILE_RANGE_FLAG_SKIP, D3D12_TILE_RANGE_FLAG_NULL};
  const UINT counts[] = {1, 1};
  c.queue->UpdateTileMappings(target.Get(), 1, &first, &two, nullptr, 2, flags, nullptr, counts,
                              D3D12_TILE_MAPPING_FLAG_NONE);
  map(1, 1, 2, 0, D3D12_TILE_RANGE_FLAG_NULL); // start of an entirely unchanged run
  for (UINT t = 2; t <= 5; ++t)
    map(t, 1, (t / physical) % 2, t % physical, D3D12_TILE_RANGE_FLAG_NONE);
  map(24, 1, 0, 2, D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_MAPPING_FLAG_NO_HAZARD);
  if (scattered) {
    // Two calls with holes inside each envelope. All four tiles stay resident
    // while the byte oracle below observes their replacement physical pages.
    for (UINT h = 0; h < 2; ++h) {
      const UINT t = 6 + 4 * h;
      const D3D12_TILED_RESOURCE_COORDINATE at[] = {{t, 0, 0, 0}, {t + 2, 0, 0, 0}};
      const D3D12_TILE_REGION_SIZE sizes[] = {{1, FALSE, 0, 0, 0}, {1, FALSE, 0, 0, 0}};
      const UINT offset = 1 + h, count = 2;
      c.queue->UpdateTileMappings(target.Get(), 2, at, sizes, heaps[h].Get(), 1, nullptr, &offset, &count,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
    }
  }
  snapshot(1);
  for (UINT h = 0; h < 2; ++h) { backing[h].Reset(); heaps[h].Reset(); }
  check(start->Signal(1), "burst release producer");
  c.finish("burst retire");
  Words out(readback);
  for (UINT s = 0; s < 2; ++s) for (UINT t = 0; t < pages; ++t) {
    bool mapped = t < 20;
    UINT h = (t / physical) % 2, p = t % physical;
    if (s == 1) {
      if (t == 1) mapped = false;
      if (t == 20 || t == 21) { mapped = true; h = 1; p = 2; }
      if (t == 22) { mapped = true; h = 1; p = 1; }
      if (t == 24) { mapped = true; h = 0; p = 2; }
      if (scattered && (t == 6 || t == 8)) { h = 0; p = t == 6 ? 1 : 2; }
      if (scattered && (t == 10 || t == 12)) { h = 1; p = t == 10 ? 2 : 3; }
    }
    for (UINT w = 0; w < words; ++w)
      expect("burst page and offset", out[(s * pages + t) * words + w], mapped ? bits(h, p, w) : 0);
  }
  if (scattered) {
    for (UINT s = 2; s < 4; ++s) for (UINT t = 0; t < pages; ++t) for (UINT w = 0; w < words; ++w)
      expect("companion page and offset", out[(s * pages + t) * words + w],
             t < 20 ? bits(1 - (t / physical) % 2, 3 - t % physical, w) : 0);
    pass("mapping-scattered-multiple-resources");
    return;
  }
  std::puts("OWNED_D3D12_TILED_BURST_PASS requests=31 heaps=2 snapshots=2 bytes=3276800");
  pass("mapping-burst-fence-snapshots");
}
