#define wmain owned_d3d12_dxil_base_wmain
#include "owned-d3d12-dxil.cpp"
#undef wmain
#include <bit>

namespace {

D3D12_RESOURCE_DESC texture2DDesc(UINT width, UINT height, UINT16 layers, UINT samples,
                                  DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = layers;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = samples;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;
  return desc;
}

ComPtr<ID3D12Resource> createTexture(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                     D3D12_RESOURCE_STATES state) {
  const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                        IID_PPV_ARGS(&texture)),
        "CreateCommittedResource(texture)");
  return texture;
}

ComPtr<ID3D12Resource> uploadFloatTexture(ID3D12Device *device, ID3D12GraphicsCommandList *list,
                                         ID3D12Resource *texture, UINT layers,
                                         const std::array<float, 32> &values) {
  const auto desc = texture->GetDesc();
  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
  std::array<UINT, 2> rows{};
  std::array<UINT64, 2> rowBytes{};
  UINT64 total = 0;
  device->GetCopyableFootprints(&desc, 0, layers, 0, footprints.data(), rows.data(), rowBytes.data(), &total);
  auto upload = createBuffer(device, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "Map(texture upload)");
  for (UINT layer = 0; layer < layers; ++layer) {
    auto *dst = static_cast<uint8_t *>(mapped) + footprints[layer].Offset;
    const float *src = values.data() + layer * 16;
    for (UINT y = 0; y < 4; ++y)
      std::memcpy(dst + y * footprints[layer].Footprint.RowPitch, src + y * 4, 4 * sizeof(float));
  }
  upload->Unmap(0, nullptr);
  for (UINT layer = 0; layer < layers; ++layer) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = layer;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprints[layer];
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  transition(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  return upload;
}

bool closeFloat(float actual, float expected, float tolerance = 0.001f) {
  return std::fabs(actual - expected) <= tolerance;
}

int runProgrammableOffsets(ID3D12Device *device, wchar_t **argv) {
  constexpr UINT lanes = 16, batches = 2, rowsPerLane = 22;
  constexpr UINT bytes = lanes * batches * rowsPerLane * 16;
  const std::array<int32_t, lanes> values = {
      -9, -8, 7, 8, 15, 16, INT32_MIN, INT32_MAX,
      -33, -32, 31, 32, 63, 64, 0x12345678, -0x12345679};
  std::array<std::array<int32_t, 4>, lanes * batches> offsets{};
  for (UINT batch = 0; batch < batches; ++batch)
    for (UINT lane = 0; lane < lanes; ++lane) {
      const UINT i = (lane + batch * 5) % lanes;
      offsets[batch * lanes + lane] = {values[i], values[(i + 7) % lanes],
                                      values[(i + 3) % lanes], values[(i + 11) % lanes]};
    }
  Context context(device);
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0};
  ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 8};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {2, ranges};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 1};
  D3D12_STATIC_SAMPLER_DESC samplers[6]{};
  samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[0].MaxAnisotropy = 1;
  samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  samplers[1] = samplers[0];
  samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  samplers[1].ShaderRegister = 1;
  for (UINT i = 2; i < 6; ++i) {
    samplers[i] = samplers[0];
    samplers[i].ShaderRegister = i;
    samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW =
        i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP :
        i == 3 ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR :
        i == 4 ? D3D12_TEXTURE_ADDRESS_MODE_BORDER : D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
  }
  D3D12_ROOT_SIGNATURE_DESC rootDesc{2, parameters, 6, samplers,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  auto root = createRootSignature(device, rootDesc);
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 9,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "programmable offset heap");
  const UINT stride = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  std::array<ComPtr<ID3D12Resource>, 6> textures;
  std::array<ComPtr<ID3D12Resource>, 6> uploads;
  auto floatTexel = [](int x, int y, int layer) {
    return float(1 + x + 128 * y + 16384 * layer) / 65536.0f;
  };
  for (UINT textureIndex = 0; textureIndex < textures.size(); ++textureIndex) {
    auto desc = texture2DDesc(127, 113, textureIndex == 1 || textureIndex == 2 ? 2 : 1, 1,
        textureIndex == 3 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    if (textureIndex == 2) {
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
      desc.Height = 1;
    }
    const UINT firstSubresource = textureIndex == 4 ? 1 : 0;
    if (textureIndex == 4) {
      desc.Width = 254;
      desc.Height = 226;
      desc.MipLevels = 2;
    } else if (textureIndex == 5) {
      desc.Width = desc.Height = 1;
    }
    textures[textureIndex] = createTexture(device, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
    std::array<UINT, 2> rowCounts{};
    std::array<UINT64, 2> rowBytes{};
    UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&desc, firstSubresource, desc.DepthOrArraySize, 0, footprints.data(),
                                  rowCounts.data(), rowBytes.data(), &uploadBytes);
    uploads[textureIndex] = createBuffer(device, uploadBytes, D3D12_HEAP_TYPE_UPLOAD,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = nullptr;
    check(uploads[textureIndex]->Map(0, nullptr, &mapped), "programmable texture upload");
    for (UINT layer = 0; layer < desc.DepthOrArraySize; ++layer) {
      for (UINT y = 0; y < footprints[layer].Footprint.Height; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(mapped) +
            footprints[layer].Offset + y * footprints[layer].Footprint.RowPitch);
        for (UINT x = 0; x < footprints[layer].Footprint.Width; ++x)
          row[x] = textureIndex == 3 ? 0x80000000u + x + 128 * y :
                   std::bit_cast<uint32_t>(floatTexel(x, y, textureIndex >= 4 ? textureIndex - 2 : layer));
      }
    }
    uploads[textureIndex]->Unmap(0, nullptr);
    for (UINT layer = 0; layer < desc.DepthOrArraySize; ++layer) {
      D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
      source.pResource = uploads[textureIndex].Get();
      source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      source.PlacedFootprint = footprints[layer];
      destination.pResource = textures[textureIndex].Get();
      destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      destination.SubresourceIndex = firstSubresource + layer;
      context.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    transition(context.list.Get(), textures[textureIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = desc.Format;
    if (textureIndex == 1) {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv.Texture2DArray.MipLevels = 1;
      srv.Texture2DArray.ArraySize = 2;
    } else if (textureIndex == 2) {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
      srv.Texture1DArray.MipLevels = 1;
      srv.Texture1DArray.ArraySize = 2;
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = 1;
      srv.Texture2D.MostDetailedMip = firstSubresource;
    }
    device->CreateShaderResourceView(textures[textureIndex].Get(), &srv, cpu);
    cpu.ptr += stride;
  }
  auto msTexture = createTexture(device, texture2DDesc(127, 113, 1, 4, DXGI_FORMAT_R32_FLOAT,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_SHADER_RESOURCE_VIEW_DESC msSRV{};
  msSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  msSRV.Format = DXGI_FORMAT_R32_FLOAT;
  msSRV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  device->CreateShaderResourceView(msTexture.Get(), &msSRV, cpu);
  cpu.ptr += stride;
  auto inputs = createBuffer(device, sizeof(offsets), D3D12_HEAP_TYPE_UPLOAD,
                             D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(inputs->Map(0, nullptr, &mapped), "programmable offset input map");
  std::memcpy(mapped, offsets.data(), sizeof(offsets));
  inputs->Unmap(0, nullptr);
  D3D12_SHADER_RESOURCE_VIEW_DESC inputSRV{};
  inputSRV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  inputSRV.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  inputSRV.Buffer.NumElements = lanes * batches;
  inputSRV.Buffer.StructureByteStride = 16;
  device->CreateShaderResourceView(inputs.Get(), &inputSRV, cpu);
  cpu.ptr += stride;
  auto output = createBuffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = createBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
  check(zero->Map(0, nullptr, &mapped), "programmable output initialize");
  std::memset(mapped, 0xcd, bytes);
  zero->Unmap(0, nullptr);
  context.list->CopyBufferRegion(output.Get(), 0, zero.Get(), 0, bytes);
  transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = createBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = lanes * batches * rowsPerLane;
  uav.Buffer.StructureByteStride = 16;
  device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpu);
  auto target = createTexture(device, texture2DDesc(lanes, 1, 1, 1, DXGI_FORMAT_R32_FLOAT,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "programmable offset RTV heap");
  const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  auto msRTV = rtv;
  msRTV.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  device->CreateRenderTargetView(msTexture.Get(), nullptr, msRTV);
  const float clear[4] = {.75f, 0, 0, 0};
  context.list->ClearRenderTargetView(msRTV, clear, 0, nullptr);
  transition(context.list.Get(), msTexture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const auto vs = readFile(argv[3]), ps = readFile(argv[4]), cs = readFile(argv[5]);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
  graphics.pRootSignature = root.Get();
  graphics.VS = {vs.data(), vs.size()};
  graphics.PS = {ps.data(), ps.size()};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
  graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  graphics.RasterizerState.DepthClipEnable = TRUE;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
  graphics.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pixelPSO, computePSO;
  check(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&pixelPSO)),
        "programmable offset graphics PSO");
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
  compute.pRootSignature = root.Get();
  compute.CS = {cs.data(), cs.size()};
  check(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&computePSO)),
        "programmable offset compute PSO");
  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  context.list->SetDescriptorHeaps(1, heaps);
  context.list->SetGraphicsRootSignature(root.Get());
  context.list->SetComputeRootSignature(root.Get());
  context.list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  context.list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  context.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0, 0, float(lanes), 1, 0, 1};
  const D3D12_RECT scissor{0, 0, lanes, 1};
  context.list->RSSetViewports(1, &viewport);
  context.list->RSSetScissorRects(1, &scissor);
  context.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  for (UINT batch = 0; batch < batches; ++batch) {
    context.list->SetGraphicsRoot32BitConstant(1, batch, 0);
    context.list->SetPipelineState(pixelPSO.Get());
    context.list->DrawInstanced(3, 1, 0, 0);
    context.list->SetComputeRoot32BitConstant(1, batch, 0);
    context.list->SetPipelineState(computePSO.Get());
    context.list->Dispatch(1, 1, 1);
  }
  transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  context.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, bytes);
  context.submitAndWait();
  const D3D12_RANGE readRange{0, bytes};
  check(readback->Map(0, &readRange, &mapped), "programmable offset readback");
  const auto *actual = static_cast<const uint32_t *>(mapped);
  auto signedLow = [](int32_t value, UINT bits) {
    const int low = uint32_t(value) & ((1u << bits) - 1);
    return low >= (1 << (bits - 1)) ? low - (1 << bits) : low;
  };
  auto address = [](int coord, int size, UINT mode) {
    if (mode == 4) {
      coord = coord < 0 ? -coord - 1 : coord;
      return coord >= size ? size - 1 : coord;
    }
    if (mode == 0)
      return coord < 0 ? 0 : coord >= size ? size - 1 : coord;
    if (mode == 3)
      return coord < 0 || coord >= size ? -1 : coord;
    const int period = mode == 2 ? size * 2 : size;
    int reduced = coord % period;
    if (reduced < 0)
      reduced += period;
    return mode == 2 && reduced >= size ? period - reduced - 1 : reduced;
  };
  for (UINT index = 0; index < lanes * batches; ++index) {
    const auto &o = offsets[index];
    const UINT batch = index / lanes;
    const int x = 64 + signedLow(o[0], 4), y = 64 + signedLow(o[1], 4);
    const float scalar = floatTexel(x, y, 0), array = floatTexel(x, y, 1);
    const float line = floatTexel(x, 0, 1);
    const float cmp = scalar >= 0.125f ? 1.f : 0.f;
    std::array<std::array<uint32_t, 4>, rowsPerLane> expected{};
    const std::array<std::array<float, 4>, 4> sampleRows = {{
      {scalar, scalar, cmp, cmp}, {array, array, array, array >= .375f ? 1.f : 0.f},
      {line, line, line, line >= 16448.f / 65536 ? 1.f : 0.f},
      {scalar, scalar, floatTexel(65, 63, 0), scalar}}};
    for (UINT row = 0; row < 4; ++row)
      for (UINT component = 0; component < 4; ++component)
        expected[row][component] = std::bit_cast<uint32_t>(sampleRows[row][component]);
    const int ox = signedLow(o[2], 6), oy = signedLow(o[3], 6);
    // D3D gather returns lower-left, lower-right, upper-right, upper-left.
    // Quarter-texel interior/edge and +/-1/64 boundary inputs stay outside
    // the specification's 0.6/256-texel fixed-point rounding ambiguity.
    const std::array<std::array<int, 2>, 4> corners = {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    for (UINT row : {4u, 5u, 6u, 7u, 8u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 19u, 20u}) {
      int bx = batch == 0 ? 64 : 1, by = bx;
      UINT mode = row == 11 || row == 14 ? 1 : row == 12 || row == 15 ? 2 : row == 13 ? 3 : 0;
      if (row == 19) {
        bx = -3;
        by = 1;
        mode = 4;
      }
      if (row == 14) {
        bx = by = -1; // +/-2^24 wraps to 0 before the texel offsets.
      } else if (row == 15) {
        bx = batch == 0 ? -1 : 126;
        by = batch == 0 ? -1 : 112; // Huge odd mirror periods reach endpoint 1.
      } else if (row == 16) {
        bx = 1000000;
        by = -1000000; // Huge clamp coordinates are provably beyond every offset.
      } else if (row == 17) {
        bx = by = batch == 0 ? 63 : 64;
      }
      for (UINT component = 0; component < 4; ++component) {
        const int gx = address(bx + ox + corners[component][0], 127, mode);
        const int gy = address(by + oy + corners[component][1], 113, mode);
        const bool border = gx < 0 || gy < 0;
        const float value = border ? 0.f : floatTexel(gx, gy, row == 20 ? 2 : row == 7 || row == 8 ? 1 : 0);
        if (row == 6)
          expected[row][component] = 0x80000000u + gx + 128 * gy;
        else if (row == 5 || row == 8)
          expected[row][component] = std::bit_cast<uint32_t>(value >= (row == 5 ? .125f : .375f) ? 1.f : 0.f);
        else
          expected[row][component] = std::bit_cast<uint32_t>(value);
      }
    }
    const std::array<float, 4> last = {cmp, array, line, floatTexel(65, 0, 1)};
    for (UINT component = 0; component < 4; ++component)
      expected[9][component] = std::bit_cast<uint32_t>(last[component]);
    // Load(coord + offset) must keep all 32 coordinate bits, unlike Load(coord, offset).
    const int32_t fullX = std::bit_cast<int32_t>(uint32_t(o[0]) + 64u);
    const int32_t fullY = std::bit_cast<int32_t>(uint32_t(o[1]) + 64u);
    const bool fullInBounds = fullX >= 0 && fullX < 127 && fullY >= 0 && fullY < 113;
    expected[10] = {std::bit_cast<uint32_t>(scalar), std::bit_cast<uint32_t>(scalar),
                    std::bit_cast<uint32_t>(cmp),
                    std::bit_cast<uint32_t>(fullInBounds ? floatTexel(fullX, fullY, 0) : 0.f)};
    expected[18] = {std::bit_cast<uint32_t>(signedLow(o[0], 4) >= 0 && signedLow(o[1], 4) >= 0 ? .75f : 0.f),
                    std::bit_cast<uint32_t>(fullInBounds ? .75f : 0.f),
                    0x80000000u + uint32_t(x) + 128u * uint32_t(y),
                    fullInBounds ? 0x80000000u + uint32_t(fullX) + 128u * uint32_t(fullY) : 0u};
    expected[21].fill(std::bit_cast<uint32_t>(floatTexel(0, 0, 3)));
    for (UINT row = 0; row < rowsPerLane; ++row)
      for (UINT component = 0; component < 4; ++component) {
        const uint32_t observed = actual[(index * rowsPerLane + row) * 4 + component];
        if (observed != expected[row][component]) {
          std::fprintf(stderr, "OWNED_D3D12_DXIL_TEXTURE_OFFSETS_FAIL batch=%u lane=%u family=%u component=%u "
                       "offsets=(%d,%d,%d,%d) actual=0x%08x expected=0x%08x\n",
                       index / lanes, index % lanes, row, component, o[0], o[1], o[2], o[3],
                       observed, expected[row][component]);
          readback->Unmap(0, nullptr);
          return 1;
        }
      }
  }
  readback->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_DXIL_TEXTURE_OFFSETS_PASS batches=%u lanes=%u families=%u components=%u\n",
              batches, lanes, rowsPerLane, bytes / 4);
  return 0;
}

int runTextureOps(int argc, wchar_t **argv) {
  if (argc != 6) {
    std::fprintf(stderr, "usage: owned-d3d12-dxil-texture.exe <basic-texture-vs.dxil> <basic-texture-ps.dxil> <programmable-offsets-vs.dxil> <programmable-offsets-ps.dxil> <programmable-offsets-cs.dxil>\n");
    return 2;
  }

  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  if (!d3d12)
    return fail("GetModuleHandleW(d3d12.dll)", HRESULT_FROM_WIN32(GetLastError()));
  using BackendIdFn = const char *(WINAPI *)();
  auto backendId = reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId"));
  if (!backendId || !backendId() || std::strcmp(backendId(), kExpectedBackendId) != 0)
    return fail("OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_DXIL_TEXTURE_BACKEND id=%s\n", backendId());

  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
  ComPtr<IDXGIAdapter1> adapter;
  check(factory->EnumAdapters1(0, &adapter), "EnumAdapters1");
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
  Context context(device.Get());

  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 3;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 3;
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  parameter.DescriptorTable.NumDescriptorRanges = 2;
  parameter.DescriptorTable.pDescriptorRanges = ranges;

  D3D12_STATIC_SAMPLER_DESC samplers[2]{};
  samplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
  samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  samplers[0].MaxAnisotropy = 1;
  samplers[0].ShaderRegister = 0;
  samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
  samplers[1] = samplers[0];
  samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  samplers[1].ShaderRegister = 1;

  D3D12_ROOT_SIGNATURE_DESC rootDesc{};
  rootDesc.NumParameters = 1;
  rootDesc.pParameters = &parameter;
  rootDesc.NumStaticSamplers = 2;
  rootDesc.pStaticSamplers = samplers;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  const auto root = createRootSignature(device.Get(), rootDesc);

  auto scalar = createTexture(device.Get(), texture2DDesc(4, 4, 1, 1, DXGI_FORMAT_R32_FLOAT,
                                                         D3D12_RESOURCE_FLAG_NONE),
                              D3D12_RESOURCE_STATE_COPY_DEST);
  auto array = createTexture(device.Get(), texture2DDesc(4, 4, 2, 1, DXGI_FORMAT_R32_FLOAT,
                                                        D3D12_RESOURCE_FLAG_NONE),
                             D3D12_RESOURCE_STATE_COPY_DEST);
  auto multisample = createTexture(device.Get(), texture2DDesc(4, 4, 1, 4, DXGI_FORMAT_R32_FLOAT,
                                                              D3D12_RESOURCE_FLAG_NONE),
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  auto target = createTexture(device.Get(), texture2DDesc(8, 8, 1, 4, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
  auto output = createBuffer(device.Get(), 5 * sizeof(float) * 4, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = createBuffer(device.Get(), 5 * sizeof(float) * 4, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

  const std::array<float, 32> textureValues = {
      0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f, 0.25f,
      0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f, 0.25f,
      0.75f, 0.25f, 0.75f, 0.25f, 0.25f, 0.75f, 0.25f, 0.75f,
      0.75f, 0.25f, 0.75f, 0.25f, 0.25f, 0.75f, 0.25f, 0.75f,
  };
  const auto scalarUpload = uploadFloatTexture(device.Get(), context.list.Get(), scalar.Get(), 1, textureValues);
  const auto arrayUpload = uploadFloatTexture(device.Get(), context.list.Get(), array.Get(), 2, textureValues);

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
  heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = 4;
  heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;
  check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap(resources)");
  const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(scalar.Get(), &srv, cpu);
  cpu.ptr += stride;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  device->CreateShaderResourceView(multisample.Get(), &srv, cpu);
  cpu.ptr += stride;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Texture2DArray.MipLevels = 1;
  srv.Texture2DArray.ArraySize = 2;
  device->CreateShaderResourceView(array.Get(), &srv, cpu);
  cpu.ptr += stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Format = DXGI_FORMAT_UNKNOWN;
  uav.Buffer.NumElements = 5;
  uav.Buffer.StructureByteStride = 4 * sizeof(float);
  device->CreateUnorderedAccessView(output.Get(), nullptr, &uav, cpu);

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap(RTV)");
  D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
  rtvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
  const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), &rtvDesc, rtv);

  const auto vs = readFile(argv[1]);
  const auto ps = readFile(argv[2]);
  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
  psoDesc.pRootSignature = root.Get();
  psoDesc.VS = {vs.data(), vs.size()};
  psoDesc.PS = {ps.data(), ps.size()};
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.RasterizerState.DepthClipEnable = TRUE;
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  psoDesc.SampleDesc.Count = 4;
  ComPtr<ID3D12PipelineState> pso;
  check(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState(texture ops)");

  ID3D12DescriptorHeap *heaps[] = {heap.Get()};
  context.list->SetDescriptorHeaps(1, heaps);
  context.list->SetPipelineState(pso.Get());
  context.list->SetGraphicsRootSignature(root.Get());
  context.list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
  context.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
  D3D12_RECT scissor{0, 0, 8, 8};
  context.list->RSSetViewports(1, &viewport);
  context.list->RSSetScissorRects(1, &scissor);
  context.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context.list->DrawInstanced(3, 1, 0, 0);
  transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  context.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, 5 * sizeof(float) * 4);
  context.submitAndWait();

  void *mapped = nullptr;
  D3D12_RANGE mapRange{0, 5 * sizeof(float) * 4};
  check(readback->Map(0, &mapRange, &mapped), "Map(texture-op readback)");
  const float *v = static_cast<const float *>(mapped);
  std::printf("OWNED_D3D12_DXIL_TEXTURE_GATHER lanes=(%.3f,%.3f,%.3f,%.3f) array=(%.3f,%.3f,%.3f,%.3f)\n",
              v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
  std::printf("OWNED_D3D12_DXIL_TEXTURE_LOD clamped=%.3f unclamped=%.3f\n", v[8], v[9]);
  std::printf("OWNED_D3D12_DXIL_TEXTURE_SAMPLE_POS texture=(%.3f,%.3f) target=(%.3f,%.3f) count=%.0f\n",
              v[12], v[13], v[14], v[15], v[16]);
  // The (0.375, 0.625) coordinate has a (1, 2) upper-left footprint texel. D3D
  // returns lower-left, lower-right, upper-right, upper-left. After (+1, -1),
  // scalar texels are (.25, .75, .25, .75). On array layer 1, (-1, +1) and
  // clamp address both rows to y=3, producing (.25, .75, .75, .25). The .5
  // reference is the comparison source, so LESS_EQUAL passes only .75 texels.
  const std::array<float, 4> expectedGather = {0, 1, 0, 1};
  const std::array<float, 4> expectedArrayGather = {0, 1, 1, 0};
  bool ok = true;
  for (UINT i = 0; i < 4; ++i) {
    ok &= closeFloat(v[i], expectedGather[i]);
    ok &= closeFloat(v[4 + i], expectedArrayGather[i]);
  }
  ok &= closeFloat(v[8], 0.0f, 0.05f) && closeFloat(v[9], -4.0f, 0.05f);
  ok &= closeFloat(v[12], -0.375f) && closeFloat(v[13], 0.125f);
  ok &= closeFloat(v[14], 0.375f) && closeFloat(v[15], -0.125f) && closeFloat(v[16], 4.0f);
  readback->Unmap(0, nullptr);
  if (!ok)
    return fail("texture opcode readback mismatch");
  // Resource GetSamplePosition follows the SRV's quality, while the target
  // intrinsic follows the active PSO. Cross the two qualities in both
  // directions rather than only comparing matching source and target pairs.
  auto patternCase = [&](UINT count, UINT targetQuality, ID3D12Resource *source,
                         ID3D12Resource *renderTarget, bool centerSource) {
    auto patternOutput = createBuffer(device.Get(), 5 * sizeof(float) * 4, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto patternReadback = createBuffer(device.Get(), 5 * sizeof(float) * 4, D3D12_HEAP_TYPE_READBACK,
                                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto sourceSrv = heap->GetCPUDescriptorHandleForHeapStart();
    sourceSrv.ptr += stride;
    srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    device->CreateShaderResourceView(source, &srv, sourceSrv);
    device->CreateUnorderedAccessView(patternOutput.Get(), nullptr, &uav, cpu);
    device->CreateRenderTargetView(renderTarget, &rtvDesc, rtv);
    psoDesc.SampleDesc = {count, targetQuality};
    ComPtr<ID3D12PipelineState> patternPSO;
    check(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&patternPSO)), "pattern DXIL PSO");
    Context patternContext(device.Get());
    patternContext.list->SetDescriptorHeaps(1, heaps);
    patternContext.list->SetPipelineState(patternPSO.Get());
    patternContext.list->SetGraphicsRootSignature(root.Get());
    patternContext.list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    patternContext.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    patternContext.list->RSSetViewports(1, &viewport);
    patternContext.list->RSSetScissorRects(1, &scissor);
    patternContext.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    patternContext.list->DrawInstanced(3, 1, 0, 0);
    transition(patternContext.list.Get(), patternOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    patternContext.list->CopyBufferRegion(patternReadback.Get(), 0, patternOutput.Get(), 0,
                                          5 * sizeof(float) * 4);
    patternContext.submitAndWait();
    void *patternMapped = nullptr;
    check(patternReadback->Map(0, &mapRange, &patternMapped), "pattern DXIL readback");
    const float *values = static_cast<const float *>(patternMapped);
    const bool centerTarget = targetQuality == 0xfffffffeu;
    const bool positionsOK = closeFloat(values[12], centerSource ? 0.f : -0.375f) &&
                             closeFloat(values[13], centerSource ? 0.f : 0.125f) &&
                             closeFloat(values[14], centerTarget ? 0.f : 0.375f) &&
                             closeFloat(values[15], centerTarget ? 0.f : -0.125f) &&
                             closeFloat(values[16], float(count));
    patternReadback->Unmap(0, nullptr);
    if (!positionsOK)
      return fail("DXIL resource/target sample pattern independence");
    std::printf("OWNED_D3D12_DXIL_TEXTURE_PATTERN_PASS count=%u target=%u source=%u\n",
                count, targetQuality, centerSource ? 0xfffffffeu : 0u);
    return 0;
  };
  for (UINT count : {2u, 4u, 8u}) {
    // The DXIL SRV is declared Texture2DMS<float, 4>; only the target varies.
    auto centerDesc = texture2DDesc(4, 4, 1, 4, DXGI_FORMAT_R32_FLOAT,
                                    D3D12_RESOURCE_FLAG_NONE);
    centerDesc.SampleDesc.Quality = 0xfffffffeu;
    auto centerTexture = createTexture(device.Get(), centerDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto centerTargetDesc = texture2DDesc(8, 8, 1, count, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    centerTargetDesc.SampleDesc.Quality = 0xfffffffeu;
    auto centerTarget = createTexture(device.Get(), centerTargetDesc, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (patternCase(count, 0xfffffffeu, centerTexture.Get(), centerTarget.Get(), true) ||
        patternCase(count, 0xfffffffeu, multisample.Get(), centerTarget.Get(), false))
      return 1;
    if (count == 4 && patternCase(count, 0u, centerTexture.Get(), target.Get(), true))
      return 1;
  }
  if (runProgrammableOffsets(device.Get(), argv))
    return 1;
  std::puts("OWNED_D3D12_DXIL_TEXTURE_PASS");
  return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  return runTextureOps(argc, argv);
}
