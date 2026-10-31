#define wmain owned_d3d12_dxil_base_wmain
#include "owned-d3d12-dxil.cpp"
#undef wmain

namespace {
constexpr char kReviewPrefix[] = "OWNED_D3D12_DXIL_RESOURCE_REVIEW";

bool expectWord(const char *name, unsigned index, uint64_t actual, uint64_t expected) {
  std::printf("%s_WORD case=%s index=%u actual=0x%016llx expected=0x%016llx\n", kReviewPrefix, name, index,
              static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
  if (actual == expected)
    return true;
  std::fprintf(stderr, "%s_FAIL case=%s index=%u\n", kReviewPrefix, name, index);
  return false;
}

ComPtr<ID3D12Resource> reviewTexture(ID3D12Device *device, bool image, UINT16 slices, UINT16 mips,
                                    DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = image ? D3D12_RESOURCE_DIMENSION_TEXTURE2D : D3D12_RESOURCE_DIMENSION_TEXTURE1D;
  desc.Width = 8;
  desc.Height = image ? 4 : 1;
  desc.DepthOrArraySize = slices;
  desc.MipLevels = mips;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
        flags ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&resource)), "Create review texture");
  return resource;
}

int runReview(int argc, wchar_t **argv) {
  if (argc != 5 && argc != 6) {
    std::fprintf(stderr, "usage: dxil-resource-review.exe <bounds> <gradients> <array-atomics> <bounds64> [case 0..3]\n");
    return 2;
  }
  const auto backend = reinterpret_cast<const char *(WINAPI *)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || std::strcmp(backend(), kExpectedBackendId))
    return fail("resource review backend");
  std::printf("%s_BACKEND id=%s\n", kReviewPrefix, backend());
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "review factory");
  check(factory->EnumAdapters1(0, &adapter), "review adapter");
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "review device");

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0};
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0, 0, 5};
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable = {2, ranges};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants = {0, 0, 4};
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = params;
  rootDesc.NumStaticSamplers = 1;
  rootDesc.pStaticSamplers = &sampler;
  // Version 1.0 descriptors are volatile: OOB accesses must remain bound checked.
  const auto root = createRootSignature(device.Get(), rootDesc);
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 10;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "review heap");
  const UINT stride = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
  auto cpuHandle = [&](UINT slot) {
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += slot * stride;
    return handle;
  };
  auto bufferView = [&](ID3D12Resource *resource, UINT slot, UINT structure, bool uav, UINT elements = 1) {
    if (uav) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
      view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      view.Format = structure ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
      view.Buffer.NumElements = elements;
      view.Buffer.StructureByteStride = structure;
      view.Buffer.Flags = structure ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW;
      device->CreateUnorderedAccessView(resource, nullptr, &view, cpuHandle(slot));
    } else {
      D3D12_SHADER_RESOURCE_VIEW_DESC view{};
      view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Format = structure ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
      view.Buffer.NumElements = elements;
      view.Buffer.StructureByteStride = structure;
      view.Buffer.Flags = structure ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW;
      device->CreateShaderResourceView(resource, &view, cpuHandle(slot));
    }
  };
  const char *names[] = {"bounds", "gradients", "array-atomics", "bounds64"};
  const int selected = argc == 6 ? _wtoi(argv[5]) : -1;
  if (selected < -1 || selected > 3)
    return 2;
  for (int test = 0; test < 4; ++test) {
    if (selected != -1 && selected != test)
      continue;
    Context ctx(device.Get());
    const auto code = readFile(argv[test + 1]);
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = root.Get();
    psoDesc.CS = {code.data(), code.size()};
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), names[test]);
    auto output = createBuffer(device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto readback = createBuffer(device.Get(), 384, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputView{};
    outputView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    outputView.Format = DXGI_FORMAT_R32_TYPELESS;
    outputView.Buffer.NumElements = 64;
    outputView.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output.Get(), nullptr, &outputView, cpuHandle(5));
    std::vector<ComPtr<ID3D12Resource>> retained;
    ComPtr<ID3D12Resource> mutableBuffer, rawMutable;
    if (test == 0 || test == 3) {
      const UINT structure = test == 3 ? 8 : 4;
      auto upload = createBuffer(device.Get(), 64, D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
      void *mapped = nullptr;
      check(upload->Map(0, nullptr, &mapped), "review buffer upload map");
      std::memset(mapped, 0, 64);
      const uint64_t value = 11;
      std::memcpy(mapped, &value, structure);
      if (test == 3) {
        const uint64_t second = 0x1122334455667788ull;
        std::memcpy(static_cast<uint8_t *>(mapped) + 8, &second, 8);
      } else {
        const uint32_t rest[] = {12, 13, 14};
        std::memcpy(static_cast<uint8_t *>(mapped) + 4, rest, sizeof(rest));
      }
      const uint32_t raw = 77;
      std::memcpy(static_cast<uint8_t *>(mapped) + 16, &raw, 4);
      upload->Unmap(0, nullptr);
      bufferView(upload.Get(), 0, structure, false);
      bufferView(upload.Get(), 1, 16, false);
      bufferView(upload.Get(), 2, 0, false, test == 3 ? 3 : 1);
      mutableBuffer = createBuffer(device.Get(), 64, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      rawMutable = createBuffer(device.Get(), 64, D3D12_HEAP_TYPE_DEFAULT,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
      ctx.list->CopyBufferRegion(mutableBuffer.Get(), 0, upload.Get(), 0, 64);
      ctx.list->CopyBufferRegion(rawMutable.Get(), 0, upload.Get(), 16, 4);
      transition(ctx.list.Get(), mutableBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(ctx.list.Get(), rawMutable.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      bufferView(mutableBuffer.Get(), 6, structure, true);
      bufferView(rawMutable.Get(), 7, 0, true);
      retained.push_back(upload);
    } else if (test == 1) {
      for (UINT array = 0; array < 2; ++array) {
        auto texture = reviewTexture(device.Get(), false, array ? 2 : 1, 4, DXGI_FORMAT_R32_FLOAT,
                                     D3D12_RESOURCE_FLAG_NONE);
        const auto desc = texture->GetDesc();
        const UINT count = array ? 8 : 4;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 8> footprints{};
        UINT64 size = 0;
        device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), nullptr, nullptr, &size);
        auto upload = createBuffer(device.Get(), size, D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        void *mapped = nullptr;
        check(upload->Map(0, nullptr, &mapped), "review texture upload map");
        std::memset(mapped, 0, static_cast<size_t>(size));
        for (UINT sub = 0; sub < count; ++sub) {
          auto *dst = reinterpret_cast<float *>(static_cast<uint8_t *>(mapped) + footprints[sub].Offset);
          for (UINT x = 0; x < footprints[sub].Footprint.Width; ++x)
            dst[x] = (array ? 200.f : 100.f) + 10.f * (sub / 4) + float(sub % 4);
        }
        upload->Unmap(0, nullptr);
        for (UINT sub = 0; sub < count; ++sub) {
          D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
          dst.pResource = texture.Get();
          dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dst.SubresourceIndex = sub;
          src.pResource = upload.Get();
          src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
          src.PlacedFootprint = footprints[sub];
          ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        transition(ctx.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY : D3D12_SRV_DIMENSION_TEXTURE1D;
        if (array) {
          view.Texture1DArray.MipLevels = 4;
          view.Texture1DArray.ArraySize = 2;
        } else {
          view.Texture1D.MipLevels = 4;
        }
        device->CreateShaderResourceView(texture.Get(), &view, cpuHandle(3 + array));
        retained.push_back(texture);
        retained.push_back(upload);
      }
    } else {
      for (UINT image = 0; image < 2; ++image) {
        auto texture = reviewTexture(device.Get(), image != 0, 2, 1, DXGI_FORMAT_R32_UINT,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_UINT;
        view.ViewDimension = image ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        if (image)
          view.Texture2DArray.ArraySize = 2;
        else
          view.Texture1DArray.ArraySize = 2;
        device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, cpuHandle(8 + image));
        retained.push_back(texture);
      }
    }
    const UINT indices[] = {0, 1, test == 3 ? 0x20000000u : 0x40000000u, 0xffffffffu};
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    ctx.list->SetDescriptorHeaps(1, heaps);
    ctx.list->SetComputeRootSignature(root.Get());
    ctx.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    ctx.list->SetComputeRoot32BitConstants(1, 4, indices, 0);
    ctx.list->SetPipelineState(pso.Get());
    ctx.list->Dispatch(1, 1, 1);
    transition(ctx.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 256);
    if (mutableBuffer) {
      transition(ctx.list.Get(), mutableBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
      transition(ctx.list.Get(), rawMutable.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
      ctx.list->CopyBufferRegion(readback.Get(), 256, mutableBuffer.Get(), 0, 8);
      ctx.list->CopyBufferRegion(readback.Get(), 264, rawMutable.Get(), 0, 4);
    }
    ctx.submitAndWait();
    void *mapped = nullptr;
    D3D12_RANGE range{0, 268};
    check(readback->Map(0, &range, &mapped), "review readback map");
    const auto *words = static_cast<const uint32_t *>(mapped);
    bool ok = true;
    unsigned checks = 0;
    auto compare = [&](uint64_t actual, uint64_t expected) {
      ok = expectWord(names[test], checks++, actual, expected) && ok;
    };
    if (test == 0) {
      const uint32_t expected[] = {11, 20, 23, 29};
      for (unsigned i = 0; i < 24; ++i)
        compare(words[i], i < 4 ? expected[i] : 0);
      const uint32_t vectorExpected[] = {11, 0, 0, 0, 11, 12, 13, 14};
      for (unsigned i = 0; i < 8; ++i)
        compare(words[24 + i], vectorExpected[i]);
      const auto *halves = static_cast<const uint16_t *>(mapped);
      for (unsigned i = 0; i < 6; ++i)
        compare(halves[64 + i], i == 0 || i == 4 ? 11 : 0);
      const uint32_t sparseExpected[] = {11, 0, 12, 14};
      for (unsigned i = 0; i < 4; ++i)
        compare(words[36 + i], sparseExpected[i]);
      compare(words[64], 29);
      compare(words[66], 77);
    } else if (test == 1) {
      const float expected[] = {101, 102, 211, 212, 102, 212};
      for (unsigned i = 0; i < 6; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &expected[i], 4);
        compare(words[i], bits);
      }
    } else if (test == 2) {
      const uint32_t expected[] = {10, 13, 40, 40, 60, 60, 20, 27, 80, 80, 90, 90,
                                   110, 113, 140, 140, 160, 160, 120, 127, 180, 180, 190, 190};
      for (unsigned i = 0; i < 24; ++i)
        compare(words[i], expected[i]);
    } else {
      const auto *wide = static_cast<const uint64_t *>(mapped);
      const uint64_t expected[] = {11, 31, 38, 41};
      for (unsigned i = 0; i < 16; ++i)
        compare(wide[i], i < 4 ? expected[i] : 0);
      const uint64_t vectorExpected[] = {11, 0x1122334455667788ull, 11, 0, 0, 0, 11, 0};
      for (unsigned i = 0; i < 8; ++i)
        compare(wide[16 + i], vectorExpected[i]);
      compare(wide[32], 41);
    }
    readback->Unmap(0, nullptr);
    if (!ok)
      return 1;
    std::printf("%s_CASE_PASS case=%s checks=%u\n", kReviewPrefix, names[test], checks);
  }
  std::printf("%s_PASS\n", kReviewPrefix);
  return 0;
}
} // namespace

int wmain(int argc, wchar_t **argv) { return runReview(argc, argv); }
