// Coupled public D3D12 Atomic64 pixel/sample, ROV and ordinary32-alias oracle.
#define wmain sm6_fixture_wmain
#include "owned-d3d12-sm6.cpp"
#undef wmain
#include <cwchar>

int pixelContention(ID3D12Device *device, Gpu &gpu, ID3D12RootSignature *root) {
  constexpr UINT width = 64, height = 64, fragments = width * height;
  constexpr UINT prefix = 4, capacity = 32, suffix = 4, recordGuardBytes = 32;
  constexpr uint64_t initial = 0xfffffffffffff000ULL, delta = 0x100000001ULL;
  constexpr uint64_t guard = 0xf00dcafea11c0dedULL;
  const auto vs = readFile(L"atomic64-pixel-contention-vs.dxil");
  for (UINT cas = 0; cas < 2; ++cas) for (UINT targets : {1u, 16u}) {
    begin();
    const char *operation = cas ? "cas" : "add";
    std::printf("OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_BEGIN operation=%s targets=%u fragments=4096\n", operation, targets);
    const auto ps = readFile(cas ? L"atomic64-pixel-contention-cas.dxil" : L"atomic64-pixel-contention-add.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root; desc.VS = {vs.data(), vs.size()}; desc.PS = {ps.data(), ps.size()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT32_MAX; desc.SampleDesc.Count = 1;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState(contention)");
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width; texture.Height = height; texture.DepthOrArraySize = texture.MipLevels = 1;
    texture.SampleDesc.Count = 1; texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> target;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr, IID_PPV_ARGS(&target)), "CreateCommittedResource(contention color)");
    auto rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    const float clear[4]{}; gpu.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    std::vector<uint8_t> cellBytes((prefix + capacity + suffix) * 8);
    for (UINT cell = 0; cell < prefix + capacity + suffix; ++cell)
      put(cellBytes, cell * 8, cell >= prefix && cell < prefix + capacity ? initial : guard);
    auto cells = gpu.uavBuffer(cellBytes);
    auto records = gpu.uavBuffer(filled(recordGuardBytes * 2 + fragments * 8));
    gpu.structuredUav(0, cells.Get(), 8);
    gpu.rawUav(1, records.Get()); gpu.rawUav(2, records.Get());
    ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
    gpu.list->SetDescriptorHeaps(1, heaps); gpu.list->SetGraphicsRootSignature(root);
    const uint32_t constants[] = {targets, prefix, 0, 0};
    gpu.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    gpu.list->SetGraphicsRootDescriptorTable(1, gpu.gpu(0));
    gpu.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const D3D12_RECT all{0, 0, LONG(width), LONG(height)};
    gpu.list->RSSetViewports(1, &viewport); gpu.list->RSSetScissorRects(1, &all);
    gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.list->SetPipelineState(pso.Get()); gpu.list->DrawInstanced(3, 1, 0, 0);
    gpu.transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 colorBytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &colorBytes);
    auto colorReadback = gpu.buffer(colorBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = target.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = colorReadback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto values = gpu.finish({cells.Get(), records.Get()}, nullptr);
    const UINT updates = fragments / targets;
    for (UINT cell = 0; cell < prefix + capacity + suffix; ++cell) {
      const uint64_t expected = cell < prefix || cell >= prefix + capacity ? guard :
          initial + (cell - prefix < targets ? uint64_t(updates) * delta : 0);
      EXPECT_EQ("pixel-contention", "full64 final/unused/guard", cell, load<uint64_t>(values[0], cell * 8), expected);
    }
    // No GPU execution order is assumed: every target must return its entire full64 multiset.
    for (UINT cell = 0; cell < targets; ++cell) {
      std::vector<uint64_t> actual, expected; actual.reserve(updates); expected.reserve(updates);
      for (UINT rank = 0; rank < updates; ++rank) {
        actual.push_back(load<uint64_t>(values[1], recordGuardBytes + (rank * targets + cell) * 8));
        expected.push_back(initial + uint64_t(rank) * delta);
      }
      std::sort(actual.begin(), actual.end()); std::sort(expected.begin(), expected.end());
      for (UINT rank = 0; rank < updates; ++rank)
        EXPECT_EQ("pixel-contention", "4096 full64 returned-old multiset", cell * updates + rank, actual[rank], expected[rank]);
    }
    for (UINT word = 0; word < recordGuardBytes / 4; ++word) {
      EXPECT_EQ("pixel-contention", "record prefix guard", word, load<uint32_t>(values[1], word * 4), kSentinel);
      EXPECT_EQ("pixel-contention", "record suffix guard", word,
          load<uint32_t>(values[1], recordGuardBytes + fragments * 8 + word * 4), kSentinel);
    }
    void *mapped = nullptr; const D3D12_RANGE readRange{0, static_cast<SIZE_T>(colorBytes)};
    check(colorReadback->Map(0, &readRange, &mapped), "Map(contention color)");
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
      const auto *rgba = static_cast<const uint8_t *>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
      constexpr uint32_t expected[] = {64, 128, 192, 255};
      for (UINT channel = 0; channel < 4; ++channel)
        EXPECT_EQ("pixel-contention", "every covered pixel", (y * width + x) * 4 + channel, rgba[channel], expected[channel]);
    }
    colorReadback->Unmap(0, nullptr);
    end("atomic64_pixel_contention_full64_multiset_color_guards");
    std::printf("OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_CASE operation=%s targets=%u fragments=4096 old_values=4096 pixels=4096 status=%s\n",
        operation, targets, g_case_mismatches ? "FAIL" : "PASS");
  }
  std::printf("OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION_%s cases=4 fragments=4096 failures=%d\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}

int wmain(int argc, wchar_t **argv) {
  const bool contention = argc == 2 && std::wcscmp(argv[1], L"--contention-only") == 0;
  if (argc != 1 && !contention) die("unknown pixel fixture argument");
  const char *prefix = contention ? "OWNED_D3D12_ATOMIC64_PIXEL_CONTENTION" : "OWNED_D3D12_ATOMIC64_PIXEL";
  setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId) != 0)
    die("OwnedGraphicsBackendId mismatch");
  std::printf("%s_BACKEND id=%s\n", prefix, backend());
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 caps{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &caps, sizeof(caps)), "CheckFeatureSupport(OPTIONS9)");
  std::printf("%s_CAP typed=%u\n", prefix, caps.AtomicInt64OnTypedResourceSupported);
  if (!caps.AtomicInt64OnTypedResourceSupported) die("AtomicInt64OnTypedResourceSupported");
  Gpu gpu(device.Get());
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0,
      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 4};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &range};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  rootDesc.Desc_1_1.NumParameters = 2;
  rootDesc.Desc_1_1.pParameters = parameters;
  auto root = rootSignature(device.Get(), rootDesc);
  if (contention) return pixelContention(device.Get(), gpu, root.Get());
  const auto vs = readFile(L"atomic64-pixel-vs.dxil");
  const auto probe = readFile(L"atomic64-pixel-probe.dxil");
  constexpr uint64_t initial = 0x12345678fffffffcULL, increment = 0x100000008ULL;
  constexpr UINT width = 4, height = 4;
  for (UINT rov = 0; rov < 2; ++rov) for (UINT typed = 0; typed < 2; ++typed)
    for (UINT frequency = 0; frequency < 2; ++frequency) for (UINT mode = 0; mode < 5; ++mode) {
      // Modes: late depth export; late discard; early depth reject; early discard; early stencil reject.
      const bool early = mode >= 2, discarded = mode == 1 || mode == 3, rejected = mode == 2 || mode == 4;
      const UINT samples = frequency ? 4 : 1, count = width * height * samples;
      begin();
      std::printf("OWNED_D3D12_ATOMIC64_PIXEL_BEGIN rov=%u typed=%u sample=%u mode=%u\n", rov, typed, frequency, mode);
      wchar_t name[96];
      std::swprintf(name, 96, L"atomic64-pixel-r%u-t%u-s%u-e%u.dxil", rov, typed, frequency, UINT(early));
      const auto ps = readFile(name);
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = root.Get(); desc.VS = {vs.data(), vs.size()}; desc.PS = {ps.data(), ps.size()};
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = UINT32_MAX;
      desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      desc.RasterizerState.DepthClipEnable = TRUE; desc.RasterizerState.MultisampleEnable = TRUE;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1; desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = samples; desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
      desc.DepthStencilState.DepthEnable = TRUE; desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
      desc.DepthStencilState.StencilEnable = TRUE;
      desc.DepthStencilState.StencilReadMask = desc.DepthStencilState.StencilWriteMask = 0xff;
      desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
          D3D12_STENCIL_OP_REPLACE, mode == 4 ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS};
      desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
      ComPtr<ID3D12PipelineState> pso;
      check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState(pixel)");
      D3D12_RESOURCE_DESC texture{};
      texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      texture.Width = width; texture.Height = height; texture.DepthOrArraySize = texture.MipLevels = 1;
      texture.SampleDesc.Count = samples; texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
      ComPtr<ID3D12Resource> target, depth, resolved;
      check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
          nullptr, IID_PPV_ARGS(&target)), "CreateCommittedResource(color)");
      auto rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
      device->CreateRenderTargetView(target.Get(), nullptr, rtv);
      texture.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_DEPTH_WRITE,
          nullptr, IID_PPV_ARGS(&depth)), "CreateCommittedResource(depth/stencil)");
      D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      ComPtr<ID3D12DescriptorHeap> dsvHeap;
      check(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap)), "CreateDescriptorHeap(DSV)");
      auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
      device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
      const float clearColor[4]{};
      gpu.list->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
      gpu.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
          mode == 2 ? 0.0f : 1.0f, 0, 0, nullptr);
      std::vector<uint8_t> cellBytes(size_t(count) * 2 * 8);
      for (UINT cell = 0; cell < count * 2; ++cell) put(cellBytes, cell * 8, initial);
      auto cells = gpu.uavBuffer(cellBytes), records = gpu.uavBuffer(filled(size_t(count) * 2 * 32));
      if (typed) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32G32_UINT; view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = count * 2;
        device->CreateUnorderedAccessView(cells.Get(), nullptr, &view, gpu.cpu(0));
      } else gpu.structuredUav(0, cells.Get(), 8);
      D3D12_UNORDERED_ACCESS_VIEW_DESC alias{};
      alias.Format = DXGI_FORMAT_R32_UINT; alias.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      alias.Buffer.NumElements = count * 4;
      device->CreateUnorderedAccessView(cells.Get(), nullptr, &alias, gpu.cpu(1));
      gpu.rawUav(2, records.Get());
      ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
      gpu.list->SetDescriptorHeaps(1, heaps); gpu.list->SetGraphicsRootSignature(root.Get());
      uint32_t constants[] = {samples, discarded ? 3u : UINT32_MAX, floatBits(.5f), 0};
      gpu.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
      gpu.list->SetGraphicsRootDescriptorTable(1, gpu.gpu(0));
      gpu.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
      gpu.list->OMSetStencilRef(7);
      const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
      const D3D12_RECT all{0, 0, LONG(width), LONG(height)};
      gpu.list->RSSetViewports(1, &viewport); gpu.list->RSSetScissorRects(1, &all);
      gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ComPtr<ID3D12QueryHeap> queries, statistics;
      D3D12_QUERY_HEAP_DESC qDesc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 5, 0};
      check(device->CreateQueryHeap(&qDesc, IID_PPV_ARGS(&queries)), "CreateQueryHeap(occlusion)");
      qDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; qDesc.Count = 1;
      check(device->CreateQueryHeap(&qDesc, IID_PPV_ARGS(&statistics)), "CreateQueryHeap(statistics)");
      auto qReadback = gpu.buffer(40, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      auto statsReadback = gpu.buffer(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS), D3D12_HEAP_TYPE_READBACK,
          D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.list->SetPipelineState(pso.Get());
      gpu.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
      gpu.list->BeginQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      gpu.list->DrawInstanced(3, 2, 0, 0);
      gpu.list->EndQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
      gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
      gpu.list->ResolveQueryData(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, statsReadback.Get(), 0);
      // Capture coupled color before the independent read-only depth/stencil probes.
      texture = target->GetDesc(); texture.SampleDesc = {1, 0}; texture.Alignment = 0;
      ID3D12Resource *color = target.Get();
      if (samples > 1) {
        check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RESOLVE_DEST,
            nullptr, IID_PPV_ARGS(&resolved)), "CreateCommittedResource(resolve)");
        gpu.transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        gpu.list->ResolveSubresource(resolved.Get(), 0, target.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        gpu.transition(resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.transition(target.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        color = resolved.Get();
      } else gpu.transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 colorBytes = 0;
      device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &colorBytes);
      auto colorReadback = gpu.buffer(colorBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
      src.pResource = color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.pResource = colorReadback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
      gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      if (samples == 1) gpu.transition(target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      desc.PS = {probe.data(), probe.size()};
      desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
      desc.DepthStencilState.StencilWriteMask = 0;
      desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
          D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
      desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
      std::array<ComPtr<ID3D12PipelineState>, 4> probePsos;
      for (UINT probeIndex = 1; probeIndex < 5; ++probeIndex) {
        const bool right = probeIndex == 2;
        const bool hasDepth = !rejected && (early || !discarded || !right);
        const float expectedDepth = hasDepth ? early ? .5f : .25f : mode == 2 ? 0.0f : 1.0f;
        const UINT expectedStencil = hasDepth ? 7 : 0;
        constants[2] = floatBits(probeIndex == 3 ? .75f : expectedDepth);
        gpu.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
        gpu.list->OMSetStencilRef(probeIndex == 4 ? 8 : expectedStencil);
        auto probeDesc = desc;
        if (probeIndex == 3) {
          probeDesc.DepthStencilState.FrontFace.StencilFunc = probeDesc.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        } else if (probeIndex == 4) probeDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        auto &probePso = probePsos[probeIndex - 1];
        check(device->CreateGraphicsPipelineState(&probeDesc, IID_PPV_ARGS(&probePso)), "CreateGraphicsPipelineState(probe)");
        const D3D12_RECT region = probeIndex >= 3 ? all : right ? D3D12_RECT{3, 0, 4, 4} : D3D12_RECT{0, 0, 3, 4};
        gpu.list->RSSetScissorRects(1, &region); gpu.list->SetPipelineState(probePso.Get());
        gpu.list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, probeIndex);
        gpu.list->DrawInstanced(3, 1, 0, 0);
        gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, probeIndex);
      }
      gpu.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 5, qReadback.Get(), 0);
      const auto values = gpu.finish({cells.Get(), records.Get()}, nullptr);
      for (UINT pixel = 0; pixel < width * height; ++pixel) for (UINT sample = 0; sample < samples; ++sample) {
        const UINT cell = pixel * samples + sample;
        const bool active = !rejected && (!discarded || pixel % width != 3);
        for (UINT ordinal = 0; ordinal < 2; ++ordinal) {
          const UINT at = ordinal * count + cell;
          const uint64_t before = initial + (rov ? ordinal * increment : 0);
          const UINT cellIndex = rov ? cell : at;
          const uint64_t expectedCell = initial + (active ? (rov ? 2 : 1) * increment : 0);
          EXPECT_EQ("pixel", "full64 final", cellIndex, load<uint64_t>(values[0], cellIndex * 8), expectedCell);
          if (!active) {
            for (UINT word = 0; word < 8; ++word)
              EXPECT_EQ("pixel", "rejected/discard no side effects", at * 8 + word,
                  load<uint32_t>(values[1], at * 32 + word * 4), kSentinel);
          } else {
            EXPECT_EQ("pixel", "full64 old", at, load<uint64_t>(values[1], at * 32), before);
            EXPECT_EQ("pixel", "full64 alias read", at, load<uint64_t>(values[1], at * 32 + 8), before + increment);
            EXPECT_EQ("pixel", "ordinary32 alias old", at, load<uint32_t>(values[1], at * 32 + 16), uint32_t(before + 0x100000005ULL));
            EXPECT_EQ("pixel", "ordinary32 alias new", at, load<uint32_t>(values[1], at * 32 + 20), uint32_t(before + increment));
            EXPECT_EQ("pixel", "old high word", at, load<uint32_t>(values[1], at * 32 + 24), uint32_t(before >> 32));
            EXPECT_EQ("pixel", "new high word", at, load<uint32_t>(values[1], at * 32 + 28), uint32_t((before + increment) >> 32));
          }
        }
        if (rov) EXPECT_EQ("pixel", "unused primitive cells", cell, load<uint64_t>(values[0], (count + cell) * 8), initial);
      }
      void *mapped = nullptr;
      D3D12_RANGE readRange{0, static_cast<SIZE_T>(colorBytes)};
      check(colorReadback->Map(0, &readRange, &mapped), "Map(color)");
      for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        const bool active = !rejected && (!discarded || x != 3);
        const auto *rgba = static_cast<const uint8_t *>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
        const uint32_t expected[] = {active && rov ? 128u : 0u, active ? 128u : 0u,
            active && frequency ? 96u : 0u, active ? 255u : 0u};
        for (UINT channel = 0; channel < 4; ++channel)
          EXPECT_EQ("pixel", "coupled color", (y * width + x) * 4 + channel, rgba[channel], expected[channel]);
      }
      colorReadback->Unmap(0, nullptr);
      readRange = {0, 40}; check(qReadback->Map(0, &readRange, &mapped), "Map(occlusion)");
      const auto *q = static_cast<const uint64_t *>(mapped);
      EXPECT_EQ("pixel", "application occlusion", 0, q[0], rejected ? 0 : uint64_t(discarded && !early ? 12 : 16) * samples * 2);
      EXPECT_EQ("pixel", "depth/stencil positive left", 1, q[1], 12ull * samples);
      EXPECT_EQ("pixel", "depth/stencil positive right", 2, q[2], 4ull * samples);
      EXPECT_EQ("pixel", "wrong depth negative", 3, q[3], 0);
      EXPECT_EQ("pixel", "wrong stencil negative", 4, q[4], 0);
      qReadback->Unmap(0, nullptr);
      readRange = {0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
      check(statsReadback->Map(0, &readRange, &mapped), "Map(statistics)");
      const auto *stats = static_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS *>(mapped);
      EXPECT_EQ("pixel", "application IA vertices", 0, stats->IAVertices, 6);
      EXPECT_EQ("pixel", "application IA primitives", 0, stats->IAPrimitives, 2);
      EXPECT_EQ("pixel", "application PS invocations", 0, stats->PSInvocations, rejected ? 0 : 32ull * samples);
      statsReadback->Unmap(0, nullptr);
      end("atomic64_pixel_coupled_color_depth_stencil_query");
      std::printf("OWNED_D3D12_ATOMIC64_PIXEL_CASE rov=%u typed=%u sample=%u mode=%u status=%s\n",
          rov, typed, frequency, mode, g_case_mismatches ? "FAIL" : "PASS");
    }
  std::printf("OWNED_D3D12_ATOMIC64_PIXEL_%s cases=40 failures=%d\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}
