// Public SM6.7 consumer; reuse the SM6 suite's real device/buffer/fence helpers.
// Modules produced by build-writable-msaa.sh must be beside the executable.
#define wmain sm6_suite_main
#include "owned-d3d12-sm6.cpp"
#undef wmain
#include <bit>

namespace {
ComPtr<ID3D12RootSignature> msaaRoot(ID3D12Device *device, bool direct) {
  D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 parameters[3]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 4};
  for (UINT i = 1; i < 3; ++i) {
    parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[i].DescriptorTable = {1, &ranges[i - 1]};
  }
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = direct ? 1 : 3;
  desc.Desc_1_1.pParameters = parameters;
  desc.Desc_1_1.Flags = direct ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
                             : D3D12_ROOT_SIGNATURE_FLAG_NONE;
  return rootSignature(device, desc);
}
ComPtr<ID3D12Resource> msaaTexture(Gpu &gpu, UINT samples, UINT layers,
    DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_UINT) {
  auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 4;
  desc.DepthOrArraySize = layers;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = samples;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> texture;
  check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture)), "writable MSAA texture");
  return texture;
}
void views(Gpu &gpu, ID3D12Resource *texture, bool array, UINT first = 0, UINT layers = 1,
           DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = format != DXGI_FORMAT_UNKNOWN ? format : texture ? texture->GetDesc().Format : DXGI_FORMAT_R32G32B32A32_UINT;
  uav.ViewDimension = array ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
  if (array) { uav.Texture2DArray.FirstArraySlice = first; uav.Texture2DArray.ArraySize = layers; }
  gpu.device->CreateUnorderedAccessView(texture, nullptr, &uav, gpu.cpu(0));
  gpu.device->CreateUnorderedAccessView(texture, nullptr, &uav, gpu.cpu(2));
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = uav.Format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
  if (array) { srv.Texture2DMSArray.FirstArraySlice = first; srv.Texture2DMSArray.ArraySize = layers; }
  gpu.device->CreateShaderResourceView(texture, &srv, gpu.cpu(3));
}
std::array<uint32_t, 4> value(UINT base, UINT pixel, UINT layer, UINT sample, bool floating = false) {
  UINT x = base + pixel + 16 * layer + 256 * sample;
  if (floating)
    return {std::bit_cast<uint32_t>(float(x) / 8), std::bit_cast<uint32_t>(float(x + 5) / 8),
            std::bit_cast<uint32_t>(float(x + 17) / 8), std::bit_cast<uint32_t>(float(x + 19) / 8)};
  return {x, x ^ 0x80000000u, x + 17, 0x12345678u + sample};
}
void expectSamples(const char *name, const std::vector<uint8_t> &bytes, UINT samples, UINT layers,
                   UINT base, UINT layer_offset = 0, bool null = false, bool cleared = false, bool floating = false) {
  const uint32_t clear[] = {7, 11, 13, 17};
  for (UINT layer = 0; layer < layers; ++layer)
    for (UINT pixel = 0; pixel < 16; ++pixel)
      for (UINT sample = 0; sample < samples; ++sample)
        for (UINT alias = 0; alias < 2; ++alias)
          for (UINT component = 0; component < 4; ++component) {
            const UINT offset = ((layer * 16 + pixel) * samples + sample) * 32 + alias * 16 + component * 4;
            auto expected = value(base, pixel, layer + layer_offset, sample, floating)[component];
            if (null) expected = 0;
            if (cleared && pixel % 4 >= 1 && pixel % 4 < 3 && pixel / 4 >= 1 && pixel / 4 < 3)
              expected = clear[component];
            EXPECT_EQ(name, alias ? "alias samples" : "UAV samples", offset / 4, load<uint32_t>(bytes, offset), expected);
          }
}
std::vector<uint8_t> dispatch(Gpu &gpu, ID3D12RootSignature *root, const char *entry,
                              UINT samples, UINT layers, UINT base, bool direct = false) {
  ComPtr<ID3D12PipelineState> pso;
  check(computePso(gpu.device.Get(), root, shader(entry), pso), "MSAA compute PSO");
  auto output = gpu.uavBuffer(filled(layers * 16 * samples * 32 + 16));
  gpu.rawUav(1, output.Get());
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetPipelineState(pso.Get());
  gpu.list->SetComputeRootSignature(root);
  const UINT constants[] = {base, 0, layers, 0};
  gpu.list->SetComputeRoot32BitConstants(0, 4, constants, 0);
  if (!direct) {
    gpu.list->SetComputeRootDescriptorTable(1, gpu.gpu(0));
    gpu.list->SetComputeRootDescriptorTable(2, gpu.gpu(3));
  }
  gpu.list->Dispatch(1, 1, layers);
  return gpu.finish({output.Get()}, nullptr)[0];
}
void raster(Gpu &gpu, ID3D12RootSignature *root, ID3D12Resource *texture, UINT samples, UINT layers, UINT base,
            const char *pixel_entry = "ps_msaa") {
  auto vs = shader("vs_msaa"), ps = shader(pixel_entry);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.VS = {vs.data(), vs.size()}; desc.PS = {ps.data(), ps.size()};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.RasterizerState.MultisampleEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1; desc.RTVFormats[0] = texture->GetDesc().Format;
  desc.SampleDesc.Count = samples;
  ComPtr<ID3D12PipelineState> pso;
  check(gpu.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "MSAA graphics PSO");
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetPipelineState(pso.Get()); gpu.list->SetGraphicsRootSignature(root);
  gpu.list->SetGraphicsRootDescriptorTable(1, gpu.gpu(0));
  gpu.list->SetGraphicsRootDescriptorTable(2, gpu.gpu(3));
  gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
  const D3D12_RECT rect{0, 0, 4, 4};
  gpu.list->RSSetViewports(1, &viewport); gpu.list->RSSetScissorRects(1, &rect);
  for (UINT layer = 0; layer < layers; ++layer) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = desc.RTVFormats[0];
    rtv.ViewDimension = layers > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DMS;
    if (layers > 1) { rtv.Texture2DMSArray.FirstArraySlice = layer; rtv.Texture2DMSArray.ArraySize = 1; }
    auto handle = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    gpu.device->CreateRenderTargetView(texture, &rtv, handle);
    gpu.list->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    const UINT constants[] = {base, layer, layers, 0};
    gpu.list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    gpu.list->DrawInstanced(3, 1, 0, 0);
  }
}
void resolveSamples(Gpu &gpu, ID3D12Resource *texture, UINT samples, UINT layers, UINT base, bool floating = false) {
  ComPtr<ID3D12GraphicsCommandList1> list;
  check(gpu.list.As(&list), "resolve list1");
  gpu.transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  for (auto mode : {D3D12_RESOLVE_MODE_MIN, D3D12_RESOLVE_MODE_MAX}) {
    auto desc = texture->GetDesc(); desc.SampleDesc.Count = 1; desc.Alignment = 0;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resolved;
    check(gpu.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved)), "resolve destination");
    for (UINT layer = 0; layer < layers; ++layer)
      list->ResolveSubresourceRegion(resolved.Get(), layer, 0, 0, texture, layer, nullptr, desc.Format, mode);
    gpu.transition(resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    std::vector<ComPtr<ID3D12Resource>> readbacks;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    for (UINT layer = 0; layer < layers; ++layer) {
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes = 0;
      gpu.device->GetCopyableFootprints(&desc, layer, 1, 0, &footprint, nullptr, nullptr, &bytes);
      auto readback = gpu.buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
      source.pResource = resolved.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; source.SubresourceIndex = layer;
      destination.pResource = readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      destination.PlacedFootprint = footprint;
      list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
      readbacks.push_back(readback); footprints.push_back(footprint);
    }
    gpu.finish({}, nullptr);
    const UINT sample = mode == D3D12_RESOLVE_MODE_MIN ? 0 : samples - 1;
    for (UINT layer = 0; layer < layers; ++layer) {
      void *mapped = nullptr; check(readbacks[layer]->Map(0, nullptr, &mapped), "resolve readback");
      for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) for (UINT c = 0; c < 4; ++c) {
        uint32_t actual;
        std::memcpy(&actual, static_cast<uint8_t *>(mapped) + footprints[layer].Offset +
            y * footprints[layer].Footprint.RowPitch + x * 16 + c * 4, 4);
        EXPECT_EQ("resolve", "MIN/MAX samples", layer * 64 + y * 16 + x * 4 + c,
            actual, value(base, y * 4 + x, layer, sample, floating)[c]);
      }
      readbacks[layer]->Unmap(0, nullptr);
    }
  }
  gpu.transition(texture, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void run(Gpu &gpu, UINT samples, bool array) {
  const UINT layers = array ? 2 : 1;
  auto root = msaaRoot(gpu.device.Get(), false), direct = msaaRoot(gpu.device.Get(), true);
  auto texture = msaaTexture(gpu, samples, layers);
  views(gpu, texture.Get(), array, 0, layers);
  const std::string suffix = std::to_string(samples) + (array ? "array" : "single");
  const auto store = "cs_msaa_store" + suffix, read = "cs_msaa_read" + suffix, heap = "cs_msaa_heap" + suffix;
  const auto srv = "cs_msaa_srv" + suffix, oob = "cs_msaa_oob" + suffix;
  begin();
  auto bytes = dispatch(gpu, root.Get(), store.c_str(), samples, layers, 100);
  expectSamples("same_dispatch", bytes, samples, layers, 100);
  end("same_dispatch");
  begin();
  bytes = dispatch(gpu, root.Get(), read.c_str(), samples, layers, 100);
  expectSamples("next_dispatch", bytes, samples, layers, 100);
  const UINT dimensions[] = {4, 4, layers, samples};
  for (UINT i = 0; i < 4; ++i)
    EXPECT_EQ("dimensions", "UAV dimensions", i, load<UINT>(bytes, layers * 16 * samples * 32 + i * 4), dimensions[i]);
  end("next_dispatch_dimensions");
  begin();
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  bytes = dispatch(gpu, root.Get(), srv.c_str(), samples, layers, 100);
  expectSamples("SRV_after_transition", bytes, samples, layers, 100);
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  end("SRV_after_transition");
  begin();
  bytes = dispatch(gpu, direct.Get(), heap.c_str(), samples, layers, 200, true);
  expectSamples("direct_heap", bytes, samples, layers, 200);
  end("direct_heap");
  begin();
  bytes = dispatch(gpu, root.Get(), oob.c_str(), samples, layers, 200);
  expectSamples("out_of_bounds", bytes, samples, layers, 200);
  end("out_of_bounds_sample_xy_layer");
  begin();
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()}; gpu.list->SetDescriptorHeaps(1, heaps);
  const UINT clear[] = {7, 11, 13, 17}; const D3D12_RECT rect{1, 1, 3, 3};
  D3D12_DESCRIPTOR_HEAP_DESC cpuDesc{};
  cpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; cpuDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> cpuHeap;
  check(gpu.device->CreateDescriptorHeap(&cpuDesc, IID_PPV_ARGS(&cpuHeap)), "clear CPU heap");
  auto cpuHandle = cpuHeap->GetCPUDescriptorHandleForHeapStart();
  gpu.device->CopyDescriptorsSimple(1, cpuHandle, gpu.cpu(0), cpuDesc.Type);
  gpu.list->ClearUnorderedAccessViewUint(gpu.gpu(0), cpuHandle, texture.Get(), clear, 1, &rect);
  D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = texture.Get();
  gpu.list->ResourceBarrier(1, &uav);
  bytes = dispatch(gpu, root.Get(), read.c_str(), samples, layers, 200);
  expectSamples("rect_clear", bytes, samples, layers, 200, 0, false, true);
  end("UAV_clear_barrier");
  begin();
  auto copied = msaaTexture(gpu, samples, layers);
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  gpu.list->CopyResource(copied.Get(), texture.Get());
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  views(gpu, copied.Get(), array, 0, layers);
  bytes = dispatch(gpu, root.Get(), read.c_str(), samples, layers, 200);
  expectSamples("copy", bytes, samples, layers, 200, 0, false, true);
  end("native_copy");
  begin();
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
  raster(gpu, root.Get(), copied.Get(), samples, layers, 300);
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  bytes = dispatch(gpu, root.Get(), read.c_str(), samples, layers, 300);
  expectSamples("raster", bytes, samples, layers, 300);
  end("native_raster");
  begin(); resolveSamples(gpu, copied.Get(), samples, layers, 300); end("native_resolve_min_max");
  if (array) {
    begin(); views(gpu, copied.Get(), true, 1, 1);
    bytes = dispatch(gpu, root.Get(), read.c_str(), samples, 1, 300);
    expectSamples("subview", bytes, samples, 1, 300, 1);
    end("array_subview");
  }
  begin(); views(gpu, nullptr, array, 0, layers);
  bytes = dispatch(gpu, root.Get(), store.c_str(), samples, layers, 400);
  expectSamples("null", bytes, samples, layers, 0, 0, true);
  end("null_store_load");
  std::printf("OWNED_D3D12_WRITABLE_MSAA_SHAPE samples=%u array=%u\n", samples, array);
}
void runScalar(Gpu &gpu, bool floating) {
  const std::string name = floating ? "float" : "sint";
  const auto format = floating ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32A32_SINT;
  auto root = msaaRoot(gpu.device.Get(), false);
  auto texture = msaaTexture(gpu, 4, 2, format);
  views(gpu, texture.Get(), true, 0, 2);
  const auto store = "cs_msaa_store" + name, read = "cs_msaa_read" + name, srv = "cs_msaa_srv" + name;
  begin();
  auto bytes = dispatch(gpu, root.Get(), store.c_str(), 4, 2, 100);
  expectSamples(name.c_str(), bytes, 4, 2, 100, 0, false, false, floating);
  end((name + "_same_dispatch").c_str());
  begin();
  bytes = dispatch(gpu, root.Get(), read.c_str(), 4, 2, 100);
  expectSamples(name.c_str(), bytes, 4, 2, 100, 0, false, false, floating);
  const UINT dimensions[] = {4, 4, 2, 4};
  for (UINT i = 0; i < 4; ++i)
    EXPECT_EQ(name.c_str(), "UAV dimensions", i, load<UINT>(bytes, 2 * 16 * 4 * 32 + i * 4), dimensions[i]);
  end((name + "_next_dispatch_dimensions").c_str());
  begin();
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  bytes = dispatch(gpu, root.Get(), srv.c_str(), 4, 2, 100);
  expectSamples(name.c_str(), bytes, 4, 2, 100, 0, false, false, floating);
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  end((name + "_SRV_after_transition").c_str());
  begin();
  auto copied = msaaTexture(gpu, 4, 2, format);
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  gpu.list->CopyResource(copied.Get(), texture.Get());
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  views(gpu, copied.Get(), true, 0, 2);
  bytes = dispatch(gpu, root.Get(), read.c_str(), 4, 2, 100);
  expectSamples(name.c_str(), bytes, 4, 2, 100, 0, false, false, floating);
  end((name + "_native_copy").c_str());
  begin();
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RENDER_TARGET);
  raster(gpu, root.Get(), copied.Get(), 4, 2, 300, ("ps_msaa" + name).c_str());
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  bytes = dispatch(gpu, root.Get(), read.c_str(), 4, 2, 300);
  expectSamples(name.c_str(), bytes, 4, 2, 300, 0, false, false, floating);
  end((name + "_native_raster").c_str());
  begin(); resolveSamples(gpu, copied.Get(), 4, 2, 300, floating); end((name + "_native_resolve_min_max").c_str());
  begin();
  ID3D12DescriptorHeap *heaps[] = {gpu.heap.Get()}; gpu.list->SetDescriptorHeaps(1, heaps);
  D3D12_DESCRIPTOR_HEAP_DESC cpuDesc{};
  cpuDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; cpuDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> cpuHeap;
  check(gpu.device->CreateDescriptorHeap(&cpuDesc, IID_PPV_ARGS(&cpuHeap)), "scalar clear CPU heap");
  auto cpuHandle = cpuHeap->GetCPUDescriptorHandleForHeapStart();
  gpu.device->CopyDescriptorsSimple(1, cpuHandle, gpu.cpu(0), cpuDesc.Type);
  const float clearFloat[] = {0.5f, -2.0f, 4.0f, 8.0f};
  const UINT clearUint[] = {7, 11, 13, 17};
  if (floating)
    gpu.list->ClearUnorderedAccessViewFloat(gpu.gpu(0), cpuHandle, copied.Get(), clearFloat, 0, nullptr);
  else
    gpu.list->ClearUnorderedAccessViewUint(gpu.gpu(0), cpuHandle, copied.Get(), clearUint, 0, nullptr);
  D3D12_RESOURCE_BARRIER uav{}; uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = copied.Get();
  gpu.list->ResourceBarrier(1, &uav);
  bytes = dispatch(gpu, root.Get(), read.c_str(), 4, 2, 0);
  for (UINT i = 0; i < 2 * 16 * 4 * 8; ++i) {
    const auto expected = floating ? std::bit_cast<UINT>(clearFloat[i % 4]) : clearUint[i % 4];
    EXPECT_EQ(name.c_str(), "all-sample clear", i, load<UINT>(bytes, i * 4), expected);
  }
  end((name + "_UAV_clear").c_str());
  std::printf("OWNED_D3D12_WRITABLE_MSAA_SCALAR type=%s\n", name.c_str());
}
void runRaw16(Gpu &gpu) {
  auto root = msaaRoot(gpu.device.Get(), false);
  auto texture = msaaTexture(gpu, 4, 2, DXGI_FORMAT_R16G16B16A16_TYPELESS);
  views(gpu, texture.Get(), true, 0, 2, DXGI_FORMAT_R16G16B16A16_UINT);
  for (const char *entry : {"cs_msaa_storeraw16", "cs_msaa_readraw16"}) {
    begin();
    auto bytes = dispatch(gpu, root.Get(), entry, 4, 2, 100);
    for (UINT layer = 0; layer < 2; ++layer) for (UINT pixel = 0; pixel < 16; ++pixel)
      for (UINT sample = 0; sample < 4; ++sample) {
        UINT x = 100 + pixel + 16 * layer + 256 * sample;
        const UINT expected[] = {x, x + 1, 0x7c01 + sample, 0x8000};
        for (UINT alias = 0; alias < 2; ++alias) for (UINT c = 0; c < 4; ++c) {
          const UINT offset = ((layer * 16 + pixel) * 4 + sample) * 32 + alias * 16 + c * 4;
          EXPECT_EQ("raw16", "half bit payload", offset / 4, load<UINT>(bytes, offset), expected[c]);
        }
      }
    end(std::strcmp(entry, "cs_msaa_storeraw16") == 0 ? "typeless16_same_dispatch" : "typeless16_next_dispatch");
  }
  begin();
  auto copied = msaaTexture(gpu, 4, 2, DXGI_FORMAT_R16G16B16A16_TYPELESS);
  gpu.transition(texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  gpu.list->CopyResource(copied.Get(), texture.Get());
  gpu.transition(copied.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2DMSArray.ArraySize = 2;
  gpu.device->CreateShaderResourceView(copied.Get(), &srv, gpu.cpu(3));
  auto bytes = dispatch(gpu, root.Get(), "cs_msaa_srvfloat", 4, 2, 100);
  for (UINT layer = 0; layer < 2; ++layer) for (UINT pixel = 0; pixel < 16; ++pixel)
    for (UINT sample = 0; sample < 4; ++sample) for (UINT alias = 0; alias < 2; ++alias) {
      const UINT offset = ((layer * 16 + pixel) * 4 + sample) * 32 + alias * 16;
      const UINT x = 100 + pixel + 16 * layer + 256 * sample;
      EXPECT_EQ("raw16", "half subnormal", offset / 4, load<UINT>(bytes, offset), std::bit_cast<UINT>(float(x) / 16777216));
      EXPECT_EQ("raw16", "half subnormal", offset / 4 + 1, load<UINT>(bytes, offset + 4), std::bit_cast<UINT>(float(x + 1) / 16777216));
      const UINT nan = load<UINT>(bytes, offset + 8);
      EXPECT_EQ("raw16", "half NaN class", offset / 4 + 2, (nan & 0x7f800000u) == 0x7f800000u && (nan & 0x7fffffu) != 0, true);
      EXPECT_EQ("raw16", "negative zero", offset / 4 + 3, load<UINT>(bytes, offset + 12), 0x80000000u);
    }
  end("typeless16_native_copy_float_srv");
}
}
int wmain() {
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = reinterpret_cast<BackendIdFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId)) die("writable MSAA backend identity");
  std::printf("OWNED_D3D12_WRITABLE_MSAA_BACKEND id=%s\n", backend());
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "MSAA device");
  D3D12_FEATURE_DATA_D3D12_OPTIONS14 options{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options, sizeof(options)), "MSAA options14");
  EXPECT_EQ("caps", "writable MSAA", 0, options.WriteableMSAATexturesSupported, TRUE);
  std::printf("OWNED_D3D12_WRITABLE_MSAA_CAPS writable=%u\n", options.WriteableMSAATexturesSupported);
  Gpu gpu(device.Get());
  for (UINT samples : {2u, 4u, 8u}) for (bool array : {false, true}) run(gpu, samples, array);
  for (bool floating : {false, true}) runScalar(gpu, floating);
  runRaw16(gpu);
  std::printf("OWNED_D3D12_WRITABLE_MSAA_%s\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
