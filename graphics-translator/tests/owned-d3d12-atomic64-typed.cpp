// Public Typed64 consumer, independent CPU operation/permutation/alias oracles.
#define wmain sm6_fixture_wmain
#include "owned-d3d12-sm6.cpp"
#undef wmain

namespace {
ComPtr<ID3D12RootSignature> typedRoot(ID3D12Device *device) {
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0,
    D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable = {1, &range};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 1;
  desc.Desc_1_1.pParameters = &parameter;
  return rootSignature(device, desc);
}

ComPtr<ID3D12Resource> typedResource(Gpu &gpu, unsigned shape) {
  if (!shape) return gpu.uavBuffer(filled(32 * 8, 0));
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = shape == 1 || shape == 4 ? D3D12_RESOURCE_DIMENSION_TEXTURE1D :
                   shape == 3 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 32;
  desc.Height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 2;
  desc.DepthOrArraySize = shape >= 3 ? 4 : 1;
  desc.MipLevels = 2;
  desc.Format = DXGI_FORMAT_R32G32_UINT;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> resource;
  check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource)), "typed texture");
  return resource;
}

void typedView(Gpu &gpu, ID3D12Resource *resource, unsigned shape, unsigned slot, DXGI_FORMAT format) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
  view.Format = format;
  switch (shape) {
  case 0: view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; view.Buffer.NumElements =
      format == DXGI_FORMAT_R32_UINT || format == DXGI_FORMAT_R32_SINT || format == DXGI_FORMAT_R32_FLOAT ? 64 : 32; break;
  case 1: view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; view.Texture1D.MipSlice = 1; break;
  case 2: view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; view.Texture2D.MipSlice = 1; break;
  case 3: view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; view.Texture3D.MipSlice = 1; view.Texture3D.WSize = 2; break;
  case 4: view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY; view.Texture1DArray = {1, 1, 2}; break;
  case 5: view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray = {1, 1, 2, 0}; break;
  }
  gpu.device->CreateUnorderedAccessView(resource, nullptr, &view, gpu.cpu(slot));
}

void uavBarrier(Gpu &gpu) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  gpu.list->ResourceBarrier(1, &barrier);
}

void dispatchTyped(Gpu &gpu, ID3D12RootSignature *root, unsigned shape, unsigned sign,
                   const char *entry, unsigned groups) {
  wchar_t name[128];
  if (std::strcmp(entry, "formats") == 0)
    std::swprintf(name, 128, L"atomic64-typed-format-%u.dxil", sign);
  else if (std::strcmp(entry, "float_atomics") == 0)
    std::swprintf(name, 128, L"atomic64-typed-float.dxil");
  else
    std::swprintf(name, 128, L"atomic64-typed-%u-%u-%hs.dxil", shape, sign, entry);
  auto bytes = readFile(name);
  ComPtr<ID3D12PipelineState> pipeline;
  if (gpu.submissionDiagnostics) std::printf("OWNED_D3D12_ATOMIC64_TYPED_BOUNDARY pso=begin\n");
  const HRESULT pipelineResult = computePso(gpu.device.Get(), root, bytes, pipeline);
  if (gpu.submissionDiagnostics) std::printf("OWNED_D3D12_ATOMIC64_TYPED_BOUNDARY pso=returned hr=%08lx\n", static_cast<unsigned long>(pipelineResult));
  check(pipelineResult, "Typed64 CreateComputePipelineState");
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetComputeRootSignature(root);
  gpu.list->SetComputeRootDescriptorTable(0, gpu.gpu(0));
  gpu.list->SetPipelineState(pipeline.Get());
  if (gpu.submissionDiagnostics) std::printf("OWNED_D3D12_ATOMIC64_TYPED_BOUNDARY dispatch=begin\n");
  gpu.list->Dispatch(groups, 1, 1);
  if (gpu.submissionDiagnostics) std::printf("OWNED_D3D12_ATOMIC64_TYPED_BOUNDARY dispatch=recorded\n");
  // Command recording retains the PSO through execution.
}
void formatCases(Gpu &gpu, ID3D12RootSignature *root) {
  struct Format { DXGI_FORMAT format; unsigned channels, bits, encoding; };
  const Format formats[] = {
    {DXGI_FORMAT_A8_UNORM,4,8,3},
    {DXGI_FORMAT_R8_UNORM,1,8,3},{DXGI_FORMAT_R8_SNORM,1,8,4},{DXGI_FORMAT_R8_UINT,1,8,0},{DXGI_FORMAT_R8_SINT,1,8,1},
    {DXGI_FORMAT_R16_UNORM,1,16,3},{DXGI_FORMAT_R16_SNORM,1,16,4},{DXGI_FORMAT_R16_UINT,1,16,0},{DXGI_FORMAT_R16_SINT,1,16,1},{DXGI_FORMAT_R16_FLOAT,1,16,5},
    {DXGI_FORMAT_R8G8_UNORM,2,8,3},{DXGI_FORMAT_R8G8_SNORM,2,8,4},{DXGI_FORMAT_R8G8_UINT,2,8,0},{DXGI_FORMAT_R8G8_SINT,2,8,1},
    {DXGI_FORMAT_R32_UINT,1,32,0},{DXGI_FORMAT_R32_SINT,1,32,1},{DXGI_FORMAT_R32_FLOAT,1,32,2},
    {DXGI_FORMAT_R16G16_UNORM,2,16,3},{DXGI_FORMAT_R16G16_SNORM,2,16,4},{DXGI_FORMAT_R16G16_UINT,2,16,0},{DXGI_FORMAT_R16G16_SINT,2,16,1},{DXGI_FORMAT_R16G16_FLOAT,2,16,5},
    {DXGI_FORMAT_R8G8B8A8_UNORM,4,8,3},{DXGI_FORMAT_R8G8B8A8_SNORM,4,8,4},{DXGI_FORMAT_R8G8B8A8_UINT,4,8,0},{DXGI_FORMAT_R8G8B8A8_SINT,4,8,1},
    {DXGI_FORMAT_R10G10B10A2_UNORM,4,10,3},{DXGI_FORMAT_R10G10B10A2_UINT,4,10,0},{DXGI_FORMAT_R11G11B10_FLOAT,3,11,6},
    {DXGI_FORMAT_R32G32_UINT,2,32,0},{DXGI_FORMAT_R32G32_SINT,2,32,1},{DXGI_FORMAT_R32G32_FLOAT,2,32,2},
    {DXGI_FORMAT_R16G16B16A16_UNORM,4,16,3},{DXGI_FORMAT_R16G16B16A16_SNORM,4,16,4},{DXGI_FORMAT_R16G16B16A16_UINT,4,16,0},{DXGI_FORMAT_R16G16B16A16_SINT,4,16,1},{DXGI_FORMAT_R16G16B16A16_FLOAT,4,16,5},
    {DXGI_FORMAT_R32G32B32A32_UINT,4,32,0},{DXGI_FORMAT_R32G32B32A32_SINT,4,32,1},{DXGI_FORMAT_R32G32B32A32_FLOAT,4,32,2}
  };
  for (auto f : formats) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = f.format;
    check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)), "typed format support");
    const auto required = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if ((support.Support2 & required) != required) {
      // Optional formats are outside this device's reported legal UAV contract.
      std::printf("OWNED_D3D12_ATOMIC64_TYPED_FORMAT_UNSUPPORTED format=%u support2=%u\n", unsigned(f.format), unsigned(support.Support2));
      continue;
    }
    unsigned unit = f.format == DXGI_FORMAT_A8_UNORM ? 1 :
                    f.format == DXGI_FORMAT_R10G10B10A2_UNORM || f.format == DXGI_FORMAT_R10G10B10A2_UINT ||
                    f.format == DXGI_FORMAT_R11G11B10_FLOAT ? 4 : f.channels * f.bits / 8;
    auto cells = gpu.uavBuffer(filled(256, 0));
    auto out = gpu.uavBuffer(filled(48));
    gpu.rawUav(0, out.Get());
    typedView(gpu, cells.Get(), 0, 1, DXGI_FORMAT_R32G32_UINT);
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = f.format;
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = 256 / unit;
    gpu.device->CreateUnorderedAccessView(cells.Get(), nullptr, &view, gpu.cpu(2));
    unsigned kind = f.encoding == 0 ? 0 : f.encoding == 1 ? 1 : 2;
    dispatchTyped(gpu, root, 0, kind, "formats", 1);
    auto result = gpu.finish({out.Get()}, nullptr)[0];
    begin();
    for (unsigned pass = 0; pass < 2; ++pass) {
      std::array<uint8_t, 16> packed{};
      uint32_t decoded[4]{};
      decoded[3] = kind == 2 ? floatBits(1.0f) : 1;
      unsigned bit = 0;
      for (unsigned c = 0; c < f.channels; ++c) {
        unsigned bits = f.format == DXGI_FORMAT_R10G10B10A2_UNORM || f.format == DXGI_FORMAT_R10G10B10A2_UINT ?
                        (c == 3 ? 2 : 10) : f.format == DXGI_FORMAT_R11G11B10_FLOAT ? (c == 2 ? 10 : 11) :
                        f.format == DXGI_FORMAT_A8_UNORM && c < 3 ? 0 : f.bits;
        if (!bits) continue;
        uint64_t mask = (uint64_t(1) << bits) - 1;
        const uint32_t integers[] = {0x12345678u,0x87654321u,0xabcd1234u,0xfedc9876u};
        const int32_t signedIntegers[] = {-12,34,-56,78};
        const float floats[] = {.25f,.5f,.75f,1.0f};
        float x = pass && c == 0 ? .125f : floats[c];
        uint32_t encoded = 0;
        if (f.encoding == 0) encoded = uint32_t((pass && c == 0 ? 0x23456789u : integers[c]) & mask);
        else if (f.encoding == 1) encoded = uint32_t((pass && c == 0 ? -23 : signedIntegers[c]) & mask);
        else if (f.encoding == 2) encoded = floatBits(x);
        else if (f.encoding == 3) encoded = uint32_t(std::nearbyint(double(x) * mask));
        else if (f.encoding == 4) encoded = uint32_t(std::nearbyint(double(x) * (mask >> 1))) & uint32_t(mask);
        else if (f.encoding == 5) encoded = floatToHalf(x);
        else {
          // The independent dyadic oracle uses the specified 5-bit exponent,
          // bias15 and 5/6-bit mantissa, not production float bit operations.
          int exponent;
          float fraction = std::frexp(x, &exponent);
          unsigned mantissa = bits - 5;
          encoded = uint32_t(exponent - 1 + 15) << mantissa;
          encoded |= uint32_t((fraction * 2 - 1) * float(1u << mantissa));
        }
        for (unsigned b = 0; b < bits; ++b)
          if (encoded & (1u << b)) packed[(bit + b) / 8] |= uint8_t(1u << ((bit + b) % 8));
        if (f.encoding == 0) decoded[c] = encoded;
        else if (f.encoding == 1) decoded[c] = uint32_t(int32_t(encoded << (32 - bits)) >> (32 - bits));
        else if (f.encoding == 3) decoded[c] = floatBits(float(double(encoded) / mask));
        else if (f.encoding == 4) decoded[c] = floatBits(float(double(encoded) / (mask >> 1)));
        else if (f.encoding == 5) decoded[c] = floatBits(halfToFloat(uint16_t(encoded)));
        else decoded[c] = floatBits(x);
        bit += bits;
      }
      uint64_t expected;
      std::memcpy(&expected, packed.data(), sizeof(expected));
      EXPECT_EQ("typed64_format", "encoded", pass, load<uint64_t>(result, pass * 24), expected);
      for (unsigned c = 0; c < 4; ++c) {
        auto observed = load<uint32_t>(result, pass * 24 + 8 + c * 4);
        if (kind != 2) EXPECT_EQ("typed64_format", "decoded", c, observed, decoded[c]);
        else {
          float actual, wanted;
          std::memcpy(&actual, &observed, 4);
          std::memcpy(&wanted, &decoded[c], 4);
          if (std::fabs(actual - wanted) > 0.000001f || !std::isfinite(actual))
            mismatch("typed64_format", "decoded", c, observed, decoded[c]);
        }
      }
    }
    end("atomic64_typed_format");
    std::printf("OWNED_D3D12_ATOMIC64_TYPED_FORMAT format=%u unit=%u\n", unsigned(f.format), unit);
  }
}
}

int wmain(int argc, wchar_t **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  bool first_case = false;
  for (int i = 1; i < argc; ++i) {
    if (std::wcscmp(argv[i], L"--first-case") == 0) first_case = true;
    else die("usage: atomic64-typed [--first-case]");
  }
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId) != 0)
    die("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_ATOMIC64_TYPED_BACKEND id=%s\n", backend());
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 caps{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &caps, sizeof(caps)), "OPTIONS9");
  std::printf("OWNED_D3D12_ATOMIC64_TYPED_CAP typed=%u\n", unsigned(caps.AtomicInt64OnTypedResourceSupported));
  if (!caps.AtomicInt64OnTypedResourceSupported) die("AtomicInt64OnTypedResourceSupported");
  Gpu gpu(device.Get());
  gpu.submissionDiagnostics = first_case;
  auto root = typedRoot(device.Get());
  constexpr uint64_t initial = 0xfffffff000000002ull, value = 0x100000001ull;
  for (unsigned shape = 0; shape < 6; ++shape) {
    for (unsigned sign = 0; sign < 2; ++sign) {
      auto cells = typedResource(gpu, shape);
      auto out = gpu.uavBuffer(filled(8200));
      gpu.rawUav(0, out.Get());
      typedView(gpu, cells.Get(), shape, 1, DXGI_FORMAT_R32G32_UINT);
      typedView(gpu, cells.Get(), shape, 2, DXGI_FORMAT_R32G32_UINT);
      // Descriptor-copy aliases must share the cell map, not acquire a second root set.
      device->CopyDescriptorsSimple(1, gpu.cpu(2), gpu.cpu(1), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      if (!shape) { typedView(gpu, cells.Get(), 0, 3, DXGI_FORMAT_R32_UINT); gpu.rawUav(4, cells.Get()); }
      ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
      gpu.list->SetDescriptorHeaps(1, heaps);
      const UINT zeros[4]{};
      gpu.list->ClearUnorderedAccessViewUint(gpu.gpu(1), gpu.cpu(1), cells.Get(), zeros, 0, nullptr);
      uavBarrier(gpu);
      std::printf("OWNED_D3D12_ATOMIC64_TYPED_BEGIN shape=%u signed=%u first=%u\n", shape, sign, unsigned(first_case));
      dispatchTyped(gpu, root.Get(), shape, sign, "operations", 1);
      auto result = gpu.finish({out.Get()}, nullptr)[0];
      begin();
      for (unsigned op = 0; op < 9; ++op) {
        uint64_t expected = value;
        switch (op) {
        case 0: expected = initial + value; break;
        case 1: expected = initial & value; break;
        case 2: expected = initial | value; break;
        case 3: expected = initial ^ value; break;
        case 4: expected = sign ? uint64_t(std::min(int64_t(initial), int64_t(value))) : std::min(initial, value); break;
        case 5: expected = sign ? uint64_t(std::max(int64_t(initial), int64_t(value))) : std::max(initial, value); break;
        }
        EXPECT_EQ("typed64", "old", op, load<uint64_t>(result, op * 16), initial);
        EXPECT_EQ("typed64", "value", op, load<uint64_t>(result, op * 16 + 8), expected);
      }
      const uint64_t tail[] = {value, value, UINT64_MAX, 0, 0x9abcdef087654321ull,
                              0x7654321087654321ull, 0, 0, 0x7654321087654321ull};
      for (unsigned i = 0; i < 9; ++i) EXPECT_EQ("typed64", "CAS/wrap/alias/OOB", i, load<uint64_t>(result, 144 + i * 8), tail[i]);
      end("atomic64_typed_operations");
      if (first_case) {
        std::printf("OWNED_D3D12_ATOMIC64_TYPED_FIRST_RESULT failures=%d\n", g_failures);
        return g_failures ? 1 : 0;
      }
      gpu.transition(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      dispatchTyped(gpu, root.Get(), shape, sign, "contention", 32);
      uavBarrier(gpu);
      dispatchTyped(gpu, root.Get(), shape, sign, "snapshot", 1);
      result = gpu.finish({out.Get()}, nullptr)[0];
      begin();
      std::vector<uint64_t> olds(1024);
      for (unsigned i = 0; i < olds.size(); ++i) olds[i] = load<uint64_t>(result, i * 8);
      std::sort(olds.begin(), olds.end());
      for (unsigned i = 0; i < olds.size(); ++i) EXPECT_EQ("typed64", "permutation", i, olds[i], uint64_t(i) * value);
      EXPECT_EQ("typed64", "final", 0, load<uint64_t>(result, 8192), 1024ull * value);
      end("atomic64_typed_contention");
      if (!shape && !sign) {
        gpu.transition(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatchTyped(gpu, root.Get(), 0, 0, "aliases", 1);
        result = gpu.finish({out.Get()}, nullptr)[0];
        begin();
        EXPECT_EQ("typed64_alias", "old32", 0, load<uint32_t>(result, 0), 0xffffffffu);
        const uint64_t values[] = {0x1234567800000000ull, 0x1234567800000000ull,
          0x1234567900000001ull, 0x8765432100000001ull};
        for (unsigned i = 0; i < 4; ++i) EXPECT_EQ("typed64_alias", "ordinary", i, load<uint64_t>(result, 8 + i * 8), values[i]);
        EXPECT_EQ("typed64_alias", "CASold32", 0, load<uint32_t>(result, 40), 1u);
        EXPECT_EQ("typed64_alias", "final", 0, load<uint64_t>(result, 48), 0x87654321fedcba98ull);
        end("atomic64_typed_raw32_aliases");
      }
      std::printf("OWNED_D3D12_ATOMIC64_TYPED_CASE shape=%u signed=%u\n", shape, sign);
    }
  }
  formatCases(gpu, root.Get());
  {
    auto cells = gpu.uavBuffer(filled(256, 0));
    auto out = gpu.uavBuffer(filled(40));
    gpu.rawUav(0, out.Get());
    typedView(gpu, cells.Get(), 0, 1, DXGI_FORMAT_R32G32_UINT);
    typedView(gpu, cells.Get(), 0, 3, DXGI_FORMAT_R32_FLOAT);
    dispatchTyped(gpu, root.Get(), 0, 0, "float_atomics", 1);
    auto result = gpu.finish({out.Get()}, nullptr)[0];
    begin();
    EXPECT_EQ("typed64_float32", "signedzero", 0, load<uint64_t>(result, 0), 0x80000000ull);
    EXPECT_EQ("typed64_float32", "exchangeold", 0, load<uint32_t>(result, 8), 0x80000000u);
    EXPECT_EQ("typed64_float32", "NaNpayload", 0, load<uint64_t>(result, 16), 0x7fc00123ull);
    EXPECT_EQ("typed64_float32", "CASold", 0, load<uint32_t>(result, 24), 0x7fc00123u);
    EXPECT_EQ("typed64_float32", "failedCASold", 0, load<uint32_t>(result, 28), 0x3fc00000u);
    EXPECT_EQ("typed64_float32", "comparestore", 0, load<uint64_t>(result, 32), 0x7f8000003fc00000ull);
    end("atomic64_typed_float32_aliases");
  }
  std::printf("OWNED_D3D12_ATOMIC64_TYPED_%s shapes=6 signs=2 failures=%d\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}
