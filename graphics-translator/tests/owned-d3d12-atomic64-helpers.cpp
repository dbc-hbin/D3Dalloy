// Public D3D12 helper-lane oracle. Build atomic64-helpers DXIL beside this executable.
#define wmain sm6_fixture_wmain
#include "owned-d3d12-sm6.cpp"
#undef wmain

int wmain() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId) != 0)
    die("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_ATOMIC64_HELPERS_BACKEND id=%s\n", backend());
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  Gpu gpu(device.Get());
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0,
      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 4};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[1].DescriptorTable = {1, &range};
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[2].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 3;
  root_desc.Desc_1_1.pParameters = parameters;
  auto root = rootSignature(device.Get(), root_desc);
  uint32_t source_data[64];
  for (uint32_t i = 0; i < 64; ++i) source_data[i] = i % 8 + 10 * (i / 8);
  auto source = gpu.constants(source_data, sizeof(source_data));
  constexpr uint64_t initial = 0x1234567800000000ULL;
  constexpr uint32_t record_bytes = 96;
  constexpr int8_t sample_x4[] = {-2, 6, -6, 2}, sample_y4[] = {-6, -2, 2, 6};
  constexpr int8_t sample_x8[] = {1, -1, 5, -3, -5, -7, 3, 7}, sample_y8[] = {-3, 3, 1, -5, 5, -1, 7, -7};
  for (uint32_t mode = 0; mode < 11; ++mode) {
    const bool sparse = mode == 2 || mode == 6 || mode == 8 || mode == 10;
    const bool early = mode == 3, discard = mode == 4 || mode == 9;
    const bool inclusive = mode == 5 || mode == 6;
    const uint32_t samples = mode == 1 || mode == 4 || mode == 10 ? 4 : mode >= 7 && mode <= 9 ? 8 : 1;
    begin();
    const auto vs = readFile(L"atomic64-helpers-vs.dxil");
    const auto ps = readFile(inclusive ? L"atomic64-helpers-inclusive.dxil" : early ? L"atomic64-helpers-early.dxil" : samples > 1 ? L"atomic64-helpers-sample.dxil" : L"atomic64-helpers-ps.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.VS = {vs.data(), vs.size()}; pso_desc.PS = {ps.data(), ps.size()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT32_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.MultisampleEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = samples;
    if (early) {
      pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
      pso_desc.DepthStencilState.DepthEnable = TRUE;
      pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
      pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    }
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState(helpers)");
    D3D12_RESOURCE_DESC rt{};
    rt.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt.Width = 7; rt.Height = 5; rt.DepthOrArraySize = rt.MipLevels = 1;
    rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rt.SampleDesc.Count = samples;
    rt.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> target, depth;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &rt,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)), "CreateCommittedResource(7x5)");
    const auto rtv = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (early) {
      rt.Format = DXGI_FORMAT_D32_FLOAT; rt.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
      check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &rt,
          D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth)), "CreateCommittedResource(depth)");
      D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
      check(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsv_heap)), "CreateDescriptorHeap(DSV)");
      dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
      device->CreateDepthStencilView(depth.Get(), nullptr, dsv);
      gpu.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
    }
    auto output = gpu.uavBuffer(filled(64 * samples * record_bytes));
    std::vector<uint8_t> cell_data(64 * samples * 8);
    for (uint32_t i = 0; i < 64 * samples; ++i) std::memcpy(cell_data.data() + i * 8, &initial, 8);
    auto cells = gpu.uavBuffer(cell_data);
    gpu.rawUav(0, output.Get()); gpu.structuredUav(1, cells.Get(), 8);
    ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
    gpu.list->SetDescriptorHeaps(1, heaps);
    gpu.list->SetPipelineState(pso.Get()); gpu.list->SetGraphicsRootSignature(root.Get());
    const uint32_t constants[] = {samples, discard ? 6u : UINT32_MAX, 0, 0};
    gpu.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    gpu.list->SetGraphicsRootDescriptorTable(1, gpu.gpu(0));
    gpu.list->SetGraphicsRootShaderResourceView(2, source->GetGPUVirtualAddress());
    gpu.list->OMSetRenderTargets(1, &rtv, FALSE, early ? &dsv : nullptr);
    const D3D12_VIEWPORT viewport{0, 0, 7, 5, 0, 1};
    const D3D12_RECT scissor = sparse ? D3D12_RECT{6, 4, 7, 5} : D3D12_RECT{0, 0, 7, 5};
    gpu.list->RSSetViewports(1, &viewport); gpu.list->RSSetScissorRects(1, &scissor);
    gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.list->DrawInstanced(3, 1, 0, 0);
    const auto result = gpu.finish({output.Get(), cells.Get()}, nullptr);
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x)
      for (uint32_t sample = 0; sample < samples; ++sample) {
        const uint32_t record = (y * 8 + x) * samples + sample;
        const bool active = x < 7 && y < 5 && !early && (!sparse || (x == 6 && y == 4)) && (!discard || x != 6);
        EXPECT_EQ("helpers", "cell old+exactly once", record, load<uint64_t>(result[1], record * 8), initial + active);
        const uint32_t base = record * record_bytes;
        if (!active) {
          for (uint32_t field = 0; field < record_bytes; field += 4)
            EXPECT_EQ("helpers", "logical helper/discard no UAV write", record, load<uint32_t>(result[0], base + field), kSentinel);
          continue;
        }
        EXPECT_EQ("helpers", "UAV old", record, load<uint64_t>(result[0], base), initial);
        EXPECT_EQ("helpers", "fine ddx protected SRV", record, load<uint32_t>(result[0], base + 8), floatBits(1));
        EXPECT_EQ("helpers", "fine ddy protected SRV", record, load<uint32_t>(result[0], base + 12), floatBits(10));
        EXPECT_EQ("helpers", "coarse ddx", record, load<uint32_t>(result[0], base + 16), floatBits(1));
        EXPECT_EQ("helpers", "coarse ddy", record, load<uint32_t>(result[0], base + 20), floatBits(10));
        EXPECT_EQ("helpers", "quad SRV helper value", record, load<uint32_t>(result[0], base + 24), floatBits(float((x ^ 1) + 10 * y)));
        const bool helper_peer = sparse || (x ^ 1) >= 7;
        EXPECT_EQ("helpers", "logical IsHelperLane peer", record, load<uint32_t>(result[0], base + 28), helper_peer);
        const auto lanes = load<uint32_t>(result[0], base + 32);
        if (!lanes || lanes > 32) mismatch("helpers", "logical wave active lanes", record, lanes, 32);
        EXPECT_EQ("helpers", "active IsHelperLane", record, load<uint32_t>(result[0], base + 36), 0);
        EXPECT_EQ("helpers", "virtual SV_Position x", record, load<uint32_t>(result[0], base + 40), floatBits(float(x) + .5f));
        EXPECT_EQ("helpers", "virtual SV_Position y", record, load<uint32_t>(result[0], base + 44), floatBits(float(y) + .5f));
        EXPECT_EQ("helpers", "coverage", record, load<uint32_t>(result[0], base + 48), (1u << samples) - 1);
        EXPECT_EQ("helpers", "sample index", record, load<uint32_t>(result[0], base + 52), sample);
        EXPECT_EQ("helpers", "captured center interpolant x", record, load<uint32_t>(result[0], base + 56), floatBits(float(x) + .5f));
        EXPECT_EQ("helpers", "original sample count", record, load<uint32_t>(result[0], base + 64), samples);
        const float offset_x = samples == 1 ? 0.f : float(samples == 4 ? sample_x4[sample] : sample_x8[sample]) / 16.f;
        const float offset_y = samples == 1 ? 0.f : float(samples == 4 ? sample_y4[sample] : sample_y8[sample]) / 16.f;
        EXPECT_EQ("helpers", "original sample position x", record, load<uint32_t>(result[0], base + 68), floatBits(offset_x));
        EXPECT_EQ("helpers", "original sample position y", record, load<uint32_t>(result[0], base + 72), floatBits(offset_y));
        EXPECT_EQ("helpers", "captured center interpolant y", record, load<uint32_t>(result[0], base + 76), floatBits(float(y) + .5f));
        EXPECT_EQ("helpers", "EvaluateAttributeAtSample x", record, load<uint32_t>(result[0], base + 80), floatBits(float(x) + .5f + offset_x));
        EXPECT_EQ("helpers", "EvaluateAttributeAtSample y", record, load<uint32_t>(result[0], base + 84), floatBits(float(y) + .5f + offset_y));
        EXPECT_EQ("helpers", "quad SRV vertical helper value", record, load<uint32_t>(result[0], base + 88), floatBits(float(x + 10 * (y ^ 1))));
        EXPECT_EQ("helpers", "quad SRV diagonal helper value", record, load<uint32_t>(result[0], base + 92), floatBits(float((x ^ 1) + 10 * (y ^ 1))));
        const auto helper_lanes = load<uint32_t>(result[0], base + 60);
        if (!inclusive) EXPECT_EQ("helpers", "default wave excludes logical helpers", record, helper_lanes, 0);
        else if (sparse ? helper_lanes < 3 || helper_lanes > 31 : (x == 6 || y == 4) && (!helper_lanes || helper_lanes > 31))
          mismatch("helpers", "WaveOpsIncludeHelperLanes", record, helper_lanes, sparse ? 3 : 1);
      }
    end("atomic64_physical_helper_7x5_sample_derivative_early_discard");
    std::printf("OWNED_D3D12_ATOMIC64_HELPERS_CASE mode=%u samples=%u status=%s\n",
                mode, samples, g_case_mismatches ? "FAIL" : "PASS");
  }
  std::printf("OWNED_D3D12_ATOMIC64_HELPERS_%s cases=11 failures=%d\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}
