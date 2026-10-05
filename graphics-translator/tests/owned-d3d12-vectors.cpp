// Dedicated SM6.9 readback oracle. Reuse the existing SM6 suite's production
// D3D12 setup/buffer/fence helpers rather than introducing another runner.
// Frozen sm6-*vectors*.dxil / sm6-cs_vector_*.dxil and the heap root signature
// must be beside the exe; scripts/test-owned-graphics.py --suite vectors stages them.
#define wmain sm6_suite_main
#include "owned-d3d12-sm6.cpp"
#undef wmain

int wmain() {
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendIdFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId)) die("vector backend identity");
  std::printf("OWNED_D3D12_VECTORS_BACKEND id=%s\n", backend());
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "vector factory");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "vector adapter");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "vector device");
  Gpu gpu(device.Get());
  Compute compute{gpu, computeRoot(device.Get()), nullptr};
  auto input = filled(32, 0);
  for (uint32_t i = 0; i < 8; ++i) put(input, i * 4, i + 1);
  auto component = [](uint32_t lane, uint32_t i) -> uint32_t {
    if (lane % 3 == 0) return i + 1;
    if (lane % 3 == 1 && i < 4) return i + 5;
    return 0;
  };
  const uint32_t constants[] = {11, 13, 17, 19, 23};
  for (uint32_t at = 0; at < 5; ++at) {
    begin();
    auto results = compute.run("cs_vectors", filled(32 * 128), input, filled(64), {at, 0, 0, 0}, 1);
    if (!results.empty()) {
      const auto &out = results[0];
      for (uint32_t lane = 0; lane < 32; ++lane) {
        const uint32_t base = lane * 128;
        for (uint32_t i = 0; i < 8; ++i) {
          const uint32_t value = component(lane, i);
          EXPECT_EQ("vectors", "raw.partial_oob", lane * 8 + i, load<uint32_t>(out, base + i * 4), value);
          EXPECT_EQ("vectors", "groupshared", lane * 8 + i, load<uint32_t>(out, base + 32 + i * 4), component((lane + 1) & 31, i));
          uint32_t bits = value, count = 0;
          while (bits) { count += bits & 1; bits >>= 1; }
          EXPECT_EQ("vectors", "countbits", lane * 8 + i, load<uint32_t>(out, base + 96 + i * 4), count);
        }
        EXPECT_EQ("vectors", "helper.dynamic", lane, load<uint32_t>(out, base + 64), constants[at] + 7);
        EXPECT_EQ("vectors", "private.insert", lane, load<uint32_t>(out, base + 68), lane + 100);
        EXPECT_EQ("vectors", "precise.dot5", lane, load<uint32_t>(out, base + 72), floatBits(2.0f * (190 - constants[(at + 1) % 5] + lane)));
        EXPECT_EQ("vectors", "reduce_and", lane, load<uint32_t>(out, base + 76), lane % 3 == 0);
        EXPECT_EQ("vectors", "reduce_or", lane, load<uint32_t>(out, base + 80), lane % 3 != 2);
        EXPECT_EQ("vectors", "wave_all_equal.all_components", lane, load<uint32_t>(out, base + 84), 0);
        EXPECT_EQ("vectors", "wave_match.last_component", lane, load<uint32_t>(out, base + 88), lane & 1 ? 0xaaaaaaaau : 0x55555555u);
        EXPECT_EQ("vectors", "committed_status", lane, load<uint32_t>(out, base + 92), 1);
      }
    }
    end("vectors");
  }
  for (uint32_t at : {0u, 1u, 511u, 1023u}) {
    begin();
    auto results = compute.run("cs_vectors1024", filled(16), input, filled(64), {at, 0, 0, 0}, 1);
    if (!results.empty()) {
      EXPECT_EQ("vectors1024", "dynamic_extract", at, load<uint32_t>(results[0], 0), at + 1);
      EXPECT_EQ("vectors1024", "neighbor", at, load<uint32_t>(results[0], 4), 0);
      EXPECT_EQ("vectors1024", "any", at, load<uint32_t>(results[0], 8), 1);
      EXPECT_EQ("vectors1024", "all", at, load<uint32_t>(results[0], 12), 0);
    }
    end("vectors1024");
  }
  begin();
  const uint32_t denorm[] = {1, 2, 3, 0x80000001u, 0x007fffffu};
  const uint32_t doubled[] = {2, 4, 6, 0x80000002u, 0x00fffffeu};
  input = filled(20, 0);
  std::memcpy(input.data(), denorm, sizeof(denorm));
  auto results = compute.run("cs_vector_denorm", filled(24), input, filled(64), {0, 0, 0, 0}, 1);
  if (!results.empty()) {
    for (uint32_t i = 0; i < 5; ++i)
      EXPECT_EQ("vector_denorm", "precise.add", i, load<uint32_t>(results[0], i * 4), doubled[i]);
    EXPECT_EQ("vector_denorm", "precise.dot", 0, load<uint32_t>(results[0], 20), 0x01000004u);
  }
  end("vector_denorm");
  for (uint32_t at = 0; at < 8; ++at) {
    begin();
    input = filled(20, 0);
    uint32_t expected[5];
    for (uint32_t i = 0; i < 5; ++i) { expected[i] = i + 1; put(input, i * 4, expected[i]); }
    for (uint32_t iteration = 0; iteration < at % 4 + 1; ++iteration) {
      for (uint32_t &value : expected) value = at & 1 ? value + iteration + 1 : value * 2;
      expected[(at + iteration) % 5] ^= 0x55;
    }
    auto ssa = compute.run("cs_vector_ssa", filled(20), input, filled(64), {at,0,0,0}, 1);
    if (!ssa.empty())
      for (uint32_t i = 0; i < 5; ++i)
        EXPECT_EQ("vector_ssa", "loop_phi.select.dynamic_insert", i, load<uint32_t>(ssa[0], i * 4), expected[i]);
    end("vector_ssa");
  }
  begin();
  const uint64_t double_input[] = {0x3ff0000000000001ull, 0x8000000000000000ull,
      0x4004000000000000ull, 0xc008000000000000ull, 0x0010000000000000ull};
  const uint64_t double_result[] = {0x4008000000000001ull, 0x3ff0000000000000ull,
      0x4018000000000000ull, 0xc014000000000000ull, 0x3ff0000000000000ull};
  input = filled(sizeof(double_input), 0);
  std::memcpy(input.data(), double_input, sizeof(double_input));
  results = compute.run("cs_vector_double", filled(44), input, filled(64), {0, 0, 0, 0}, 1);
  if (!results.empty()) {
    for (uint32_t i = 0; i < 5; ++i)
      EXPECT_EQ("vector_double", "precise.mul_add.bits", i, load<uint64_t>(results[0], i * 8), double_result[i]);
    EXPECT_EQ("vector_double", "componentwise_wave_equal", 0, load<uint32_t>(results[0], 40), 1);
  }
  end("vector_double");
  begin();
  const uint64_t integer_input[] = {0x0000000100000001ull, ~0ull, 0x8000000000000000ull, 7, 0x0123456789abcdefull};
  input = filled(sizeof(integer_input), 0);
  std::memcpy(input.data(), integer_input, sizeof(integer_input));
  results = compute.run("cs_vector_uint64", filled(44), input, filled(64), {0,0,0,0}, 1);
  if (!results.empty()) {
    for (uint32_t i = 0; i < 5; ++i)
      EXPECT_EQ("vector_uint64", "raw64.shift.add.xor", i, load<uint64_t>(results[0], i * 8), (integer_input[i] << 17) ^ (integer_input[i] + 1));
    EXPECT_EQ("vector_uint64", "componentwise_wave_equal", 0, load<uint32_t>(results[0], 40), 1);
  }
  end("vector_uint64");
  begin();
  const uint16_t half_input[] = {0x3c00,0x4000,0x4200,0x4400,0x4500,0x4600,0x4700,0x4800};
  const uint16_t half_result[] = {0x4000,0x4400,0x4600,0x4800,0x4900,0x4a00,0x4b00,0x4c00};
  input = filled(sizeof(half_input), 0);
  std::memcpy(input.data(), half_input, sizeof(half_input));
  results = compute.run("cs_vector_half", filled(20), input, filled(64), {0,0,0,0}, 1);
  if (!results.empty()) {
    for (uint32_t i = 0; i < 8; ++i)
      EXPECT_EQ("vector_half", "raw16.mul", i, load<uint16_t>(results[0], i * 2), half_result[i]);
    EXPECT_EQ("vector_half", "dot8", 0, load<uint32_t>(results[0], 16), floatBits(36));
  }
  end("vector_half");
  auto heap_signature = readFile(L"sm6-VECTOR_HEAP_ROOT.dxbc");
  ComPtr<ID3D12RootSignature> heap_root;
  check(device->CreateRootSignature(0, heap_signature.data(), heap_signature.size(), IID_PPV_ARGS(&heap_root)), "vector heap root");
  Compute heap_compute{gpu, heap_root, nullptr};
  for (uint32_t at : {0u, 16u, 0xfffffff0u}) {
    begin();
    input = filled(32, 0);
    for (uint32_t i = 0; i < 8; ++i) put(input, i * 4, i + 1);
    results = heap_compute.run("cs_vector_heap", filled(32), input, filled(64), {at,0,0,0}, 1);
    if (!results.empty()) {
      for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t expected = at == 0 ? i + 1 : at == 16 && i < 4 ? i + 5 : 0;
        EXPECT_EQ("vector_heap", "load", i, load<uint32_t>(results[0], i * 4), expected);
        uint32_t stored = i + 1;
        if (at == 0 && i >= 4) stored = i - 4 + 11;
        if (at == 0xfffffff0u) stored = 10;
        EXPECT_EQ("vector_heap", "store.partially_oob", i, load<uint32_t>(results[1], i * 4), stored);
      }
    }
    end("vector_heap");
  }
  for (uint32_t at : {0u, 1u, 0xffffffffu}) {
    begin();
    auto structured = filled(64, 0);
    for (uint32_t i = 0; i < 16; ++i) put(structured, i * 4, i + 1);
    results = compute.run("cs_vector_structured", filled(32), filled(32), structured,
                          {at,0,0,0}, 1, nullptr, 0, nullptr, 32);
    if (!results.empty()) {
      for (uint32_t i = 0; i < 8; ++i)
        EXPECT_EQ("vector_structured", "load", i, load<uint32_t>(results[0], i * 4), at < 2 ? at * 8 + i + 1 : 0);
      for (uint32_t i = 0; i < 16; ++i) {
        uint32_t expected = i + 1;
        if (at == 0 && i >= 8) expected = i - 8 + 11;
        if (at == 0xffffffffu && i < 8) expected = 10;
        EXPECT_EQ("vector_structured", "bounded_store", i, load<uint32_t>(results[2], i * 4), expected);
      }
    }
    end("vector_structured");
  }
  begin();
  Graphics graphics{gpu, graphicsRoot(device.Get())};
  results = graphics.draw("vs_vectors", "ps_vectors", 8, 8, filled(2048), filled(32), {0,0,0,0}, false, 0, 0);
  if (!results.empty()) {
    for (uint32_t pixel = 0; pixel < 64; ++pixel) {
      for (uint32_t i = 0; i < 5; ++i)
        EXPECT_EQ("vector_graphics", "ps.long_helper", pixel * 5 + i, load<uint32_t>(results[0], pixel * 24 + i * 4), constants[i] + (pixel % 5 == i ? 7 : 0));
      EXPECT_EQ("vector_graphics", "ps.reduce", pixel, load<uint32_t>(results[0], pixel * 24 + 20), pixel % 5 == 2);
    }
    for (uint32_t vertex = 0; vertex < 3; ++vertex)
      for (uint32_t i = 0; i < 5; ++i)
        EXPECT_EQ("vector_graphics", "vs.long_helper", vertex * 5 + i, load<uint32_t>(results[0], 1536 + vertex * 32 + i * 4), constants[i] + (vertex == i ? 7 : 0));
  }
  end("vector_graphics");
  D3D12_RESOURCE_DESC sparse_desc{};
  sparse_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  sparse_desc.Width = 2 * 65536;
  sparse_desc.Height = sparse_desc.DepthOrArraySize = sparse_desc.MipLevels = 1;
  sparse_desc.SampleDesc.Count = 1;
  sparse_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  sparse_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> sparse;
  check(device->CreateReservedResource(&sparse_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&sparse)), "vector reserved buffer");
  D3D12_HEAP_DESC tile_desc{};
  tile_desc.SizeInBytes = tile_desc.Alignment = 65536;
  tile_desc.Properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  tile_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ComPtr<ID3D12Heap> tile;
  check(device->CreateHeap(&tile_desc, IID_PPV_ARGS(&tile)), "vector tile heap");
  D3D12_TILED_RESOURCE_COORDINATE origin{};
  D3D12_TILE_REGION_SIZE one_tile{1,FALSE,0,0,0};
  const UINT zero = 0, one = 1;
  gpu.queue->UpdateTileMappings(sparse.Get(), 1, &origin, &one_tile, tile.Get(), 1, nullptr, &zero, &one, D3D12_TILE_MAPPING_FLAG_NONE);
  auto upload = gpu.buffer(65536, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "vector tile upload");
  auto *words = static_cast<uint32_t *>(mapped);
  for (uint32_t i = 0; i < 65536 / 4; ++i) words[i] = 1;
  for (uint32_t i = 0; i < 4; ++i) words[65536 / 4 - 4 + i] = 101 + i;
  upload->Unmap(0, nullptr);
  gpu.list->CopyBufferRegion(sparse.Get(), 0, upload.Get(), 0, 65536);
  gpu.transition(sparse.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComPtr<ID3D12PipelineState> tile_pso;
  check(computePso(device.Get(), compute.root.Get(), shader("cs_vector_tiles"), tile_pso), "vector tile PSO");
  for (uint32_t at : {0u, 65520u, 65536u, 0xfffffff0u}) {
    begin();
    auto out = gpu.uavBuffer(filled(36));
    gpu.rawUav(0, out.Get());
    gpu.rawUav(1, sparse.Get());
    const std::array<uint32_t,4> params{at,0,0,0};
    auto cb = gpu.constants(params.data(), sizeof(params));
    ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
    gpu.list->SetDescriptorHeaps(1, heaps);
    gpu.list->SetPipelineState(tile_pso.Get());
    gpu.list->SetComputeRootSignature(compute.root.Get());
    gpu.list->SetComputeRoot32BitConstants(0, 4, params.data(), 0);
    gpu.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
    gpu.list->SetComputeRootDescriptorTable(2, gpu.gpu(0));
    gpu.list->SetComputeRootDescriptorTable(3, gpu.gpu(3));
    gpu.list->Dispatch(1,1,1);
    results = gpu.finish({out.Get()}, nullptr);
    for (uint32_t i = 0; i < 8; ++i)
      EXPECT_EQ("vector_tiles", "mapped_and_unmapped_components", i, load<uint32_t>(results[0], i * 4), at == 0 ? 1 : at == 65520 && i < 4 ? 101 + i : 0);
    EXPECT_EQ("vector_tiles", "whole_vector_status", 0, load<uint32_t>(results[0], 32), at == 0 || at == 0xfffffff0u);
    end("vector_tiles");
  }
  std::printf("OWNED_D3D12_VECTORS_%s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
