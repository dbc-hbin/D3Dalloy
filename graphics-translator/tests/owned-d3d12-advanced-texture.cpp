// Public D3D12 readback consumer; shares the existing DXIL fence/buffer/PSO helpers.
#define wmain advanced_texture_base_main
#include "owned-d3d12-dxil.cpp"
#undef wmain
#include <algorithm>
#include <bit>
#include <cwchar>

namespace {
constexpr UINT resultBytes = 576;
void expect(UINT actual, UINT expected, const char *name, UINT row, UINT component) {
  if (actual != expected) {
    std::fprintf(stderr, "OWNED_D3D12_ADVANCED_TEXTURE_FAIL case=%s row=%u component=%u actual=%08x expected=%08x\n",
                 name, row, component, actual, expected);
    ExitProcess(1);
  }
}
void runAdvanced(ID3D12Device *device, UINT shape, UINT bits, const wchar_t *shaderPath, const char *stage,
                 const char *operation = "cmp") {
  const bool raw = bits != 0, cube = shape >= 4;
  const bool array = shape == 1 || shape == 3 || shape == 5;
  Context context(device);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = shape < 2 ? D3D12_RESOURCE_DIMENSION_TEXTURE1D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 8; desc.Height = shape < 2 ? 1 : 8;
  // Rectangular 2D views make an X/Y gradient operand swap change the mip.
  if ((shape == 2 || shape == 3) && std::strcmp(operation, "cmp")) desc.Height = 16;
  desc.DepthOrArraySize = cube ? (array ? 18 : 6) : (array ? 3 : 1);
  desc.MipLevels = 4; desc.SampleDesc.Count = 1;
  desc.Format = !raw ? DXGI_FORMAT_R32_TYPELESS : bits == 16 ? DXGI_FORMAT_R16_UINT :
                bits == 32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32G32_UINT;
  desc.Flags = raw ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  const auto props = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> texture;
  check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&texture)), "advanced texture");
  const UINT count = desc.MipLevels * desc.DepthOrArraySize;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
  std::vector<UINT> rows(count);
  std::vector<UINT64> rowBytes(count);
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), rowBytes.data(), &bytes);
  auto upload = createBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "advanced upload map");
  std::memset(mapped, 0, size_t(bytes));
  // Distinct low/high patterns include sign bits, NaN/Inf-like payloads and asymmetric halves.
  const UINT lows[] = {0x80000001, 0x7fc12345, 0xffffffff, 0x01234567};
  const UINT highs[] = {0xdeadbeef, 0x80000000, 0x76543210, 0xfedcba98};
  const UINT halves[] = {0x8001, 0x7e55, 0xffff, 0x1234};
  for (UINT sub = 0; sub < count; ++sub) {
    const UINT mip = sub % desc.MipLevels, slice = sub / desc.MipLevels;
    for (UINT y = 0; y < rows[sub]; ++y) for (UINT x = 0; x < footprints[sub].Footprint.Width; ++x) {
      auto *dst = static_cast<uint8_t *>(mapped) + footprints[sub].Offset +
                  size_t(y) * footprints[sub].Footprint.RowPitch + size_t(x) * (bits == 64 ? 8 : bits == 16 ? 2 : 4);
      if (raw) {
        const UINT index = (x + 2 * y) & 3;
        // Only resource slice1/mip1 has the intended data; a lost SRV range cannot pass.
        const bool intended = mip == 1 && slice == (array ? 1u : 0u);
        UINT low = intended ? (bits == 16 ? halves[index] : lows[index]) : 0x5555;
        UINT high = intended ? highs[index] : 0xaaaaaaaa;
        std::memcpy(dst, &low, bits == 16 ? 2 : 4);
        if (bits == 64) std::memcpy(dst + 4, &high, 4);
      } else {
        const UINT firstSlice = cube ? (array ? 6 : 0) : (array ? 1 : 0);
        float value = mip == 2 ? .75f : .25f;
        if ((!cube && x == 2) || slice != firstSlice + (cube ? slice % 6 : 0)) value = 1 - value;
        std::memcpy(dst, &value, 4);
      }
    }
  }
  upload->Unmap(0, nullptr);
  for (UINT sub = 0; sub < count; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = texture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = sub;
    src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[sub];
    context.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  transition(context.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  ComPtr<ID3D12DescriptorHeap> views;
  check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views)), "advanced views");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = raw ? desc.Format : DXGI_FORMAT_R32_FLOAT;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = shape == 0 ? D3D12_SRV_DIMENSION_TEXTURE1D : shape == 1 ? D3D12_SRV_DIMENSION_TEXTURE1DARRAY :
                      shape == 2 ? D3D12_SRV_DIMENSION_TEXTURE2D : shape == 3 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY :
                      shape == 4 ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
  float *clamp = nullptr;
  switch (shape) {
  case 0: srv.Texture1D = {1, 2, 0}; clamp = &srv.Texture1D.ResourceMinLODClamp; break;
  case 1: srv.Texture1DArray = {1, 2, 1, 2, 0}; clamp = &srv.Texture1DArray.ResourceMinLODClamp; break;
  case 2: srv.Texture2D = {1, 2, 0, 0}; clamp = &srv.Texture2D.ResourceMinLODClamp; break;
  case 3: srv.Texture2DArray = {1, 2, 1, 2, 0, 0}; clamp = &srv.Texture2DArray.ResourceMinLODClamp; break;
  case 4: srv.TextureCube = {1, 2, 0}; clamp = &srv.TextureCube.ResourceMinLODClamp; break;
  case 5: srv.TextureCubeArray = {1, 2, 6, 2, 0}; clamp = &srv.TextureCubeArray.ResourceMinLODClamp; break;
  }
  auto cpu = views->GetCPUDescriptorHandleForHeapStart();
  const UINT stride = device->GetDescriptorHandleIncrementSize(hd.Type);
  for (UINT i = 0; i < 6; ++i) {
    *clamp = i == 2 ? 2 : i == 3 ? 8 : i >= 4 ? 1.25f : 0;
    if (i == 5) {
      if (shape == 0) srv.Texture1D.MipLevels = 1;
      if (shape == 1) srv.Texture1DArray.MipLevels = 1;
      if (shape == 2) srv.Texture2D.MipLevels = 1;
      if (shape == 3) srv.Texture2DArray.MipLevels = 1;
      if (shape == 4) srv.TextureCube.MipLevels = 1;
      if (shape == 5) srv.TextureCubeArray.MipLevels = 1;
    }
    device->CreateShaderResourceView(i == 1 ? nullptr : texture.Get(), &srv, cpu);
    cpu.ptr += stride;
  }
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0,
      D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
  D3D12_ROOT_PARAMETER1 params[2]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = {1, &range};
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[1].Descriptor.ShaderRegister = 0;
  StaticSamplerDesc1 samplers[6]{};
  for (UINT i = 0; i < 6; ++i) {
    auto &s = samplers[i].base; s.Filter = i ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
    s.AddressU = s.AddressV = s.AddressW = i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_BORDER : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    if (raw && !i) {
      s.BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(3);
      samplers[i].Flags = 1; // D3D12_SAMPLER_FLAG_UINT_BORDER_COLOR
    }
    s.MaxAnisotropy = 1; s.ShaderRegister = i; s.MaxLOD = i == 3 ? 0 : D3D12_FLOAT32_MAX; s.MinLOD = i == 4 ? 1 : 0;
    s.MipLODBias = i == 5 ? 1 : 0;
  }
  RootSignatureDesc2 rootDesc{2, params, 6, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
  auto root = createRootSignature(device, rootDesc);
  auto output = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_COPY_DEST);
  auto zero = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
  check(zero->Map(0, nullptr, &mapped), "advanced sentinel map"); std::memset(mapped, 0xcd, resultBytes); zero->Unmap(0, nullptr);
  context.list->CopyBufferRegion(output.Get(), 0, zero.Get(), 0, resultBytes);
  transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto shader = readFile(shaderPath);
  ComPtr<ID3D12PipelineState> pso;
  std::vector<uint8_t> other;
  ComPtr<ID3D12Resource> target;
  ComPtr<ID3D12DescriptorHeap> rtvs;
  ID3D12DescriptorHeap *heaps[] = {views.Get()}; context.list->SetDescriptorHeaps(1, heaps);
  if (!std::strcmp(stage, "cs")) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get(); p.CS = {shader.data(), shader.size()};
    check(device->CreateComputePipelineState(&p, IID_PPV_ARGS(&pso)), "advanced compute PSO");
    context.list->SetComputeRootSignature(root.Get()); context.list->SetPipelineState(pso.Get());
    context.list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    context.list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress()); context.list->Dispatch(1, 1, 1);
  } else {
    const bool vertex = !std::strcmp(stage, "vs");
    // No-op companion stages are separately produced; no second stage can overwrite the tested buffer.
    other = readFile(vertex ? L"advanced-texture-null-ps.dxil" : L"advanced-texture-null-vs.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get();
    p.VS = vertex ? D3D12_SHADER_BYTECODE{shader.data(), shader.size()} : D3D12_SHADER_BYTECODE{other.data(), other.size()};
    p.PS = vertex ? D3D12_SHADER_BYTECODE{other.data(), other.size()} : D3D12_SHADER_BYTECODE{shader.data(), shader.size()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = D3D12_DEFAULT_SAMPLE_MASK; p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT; p.SampleDesc.Count = 1;
    check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)), "advanced graphics PSO");
    auto td = desc; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = td.Height = td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
          nullptr, IID_PPV_ARGS(&target)), "advanced render target");
    D3D12_DESCRIPTOR_HEAP_DESC rd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    check(device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtvs)), "advanced RTV heap");
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    context.list->SetGraphicsRootSignature(root.Get()); context.list->SetPipelineState(pso.Get());
    context.list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    context.list->SetGraphicsRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    context.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1}; D3D12_RECT scissor{0, 0, 1, 1};
    context.list->RSSetViewports(1, &viewport); context.list->RSSetScissorRects(1, &scissor);
    context.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); context.list->DrawInstanced(3, 1, 0, 0);
  }
  transition(context.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  context.list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, resultBytes);
  context.submitAndWait(); check(device->GetDeviceRemovedReason(), "advanced device status");
  check(readback->Map(0, nullptr, &mapped), "advanced readback map");
  const auto *words = static_cast<const UINT *>(mapped);
  char name[64];
  if (raw) std::snprintf(name, sizeof(name), "raw%u_%s", bits, stage);
  else std::snprintf(name, sizeof(name), "%s%u_%s", operation, shape, stage);
  if (!raw && std::strcmp(operation, "cmp")) {
    // Exact point comparison results: mip0 depth .25, mip1 depth .75.
    // Intrinsic/resource/sampler clamps and bias must select the right mip;
    // reference .875 must still fail at mip1, and null/empty SRVs return zero.
    for (UINT i = 0; i < 22; ++i) {
      const bool pass = i == 1 || i == 2 || i == 4 || i == 6 || i == 7 || i == 13 || i == 14 || i == 21 ||
                        (!cube && i == 16) || (array && i == 17);
      expect(words[i * 2], std::bit_cast<UINT>(pass ? 1.f : 0.f), name, i, 0);
      expect(words[i * 2 + 1], 1, name, i, 1);
    }
    for (UINT i = 44; i < resultBytes / 4; ++i) expect(words[i], 0xcdcdcdcd, name, i / 2, i % 2);
  } else if (!raw) {
    for (UINT i = 0; i < 22; ++i) {
      const bool pass = i == 1 || i == 2 || i == 7 || i == 12 || i == 14 || i == 16 ||
                        (array && (i == 17 || i == 18)) || (!cube && (i == 9 || i == 10 || i == 15));
      expect(words[i * 2], std::bit_cast<UINT>(pass ? 1.f : 0.f), name, i, 0);
      expect(words[i * 2 + 1], 1, name, i, 1);
    }
  } else {
    // Gather order is lower-left, lower-right, upper-right, upper-left.
    const int xs[] = {1, 2, 2, 1}, ys[] = {2, 2, 1, 1};
    for (UINT row = 0; row < 12; ++row) {
      for (UINT lane = 0; lane < 4; ++lane) {
        int x = row == 6 ? 0 : std::clamp(xs[lane] + (row == 2 ? 1 : row == 7 ? -1 : 0), 0, 3);
        const UINT index = (x + 2 * ys[lane]) & 3;
        const bool empty = row == 3 || row == 4 || row == 5 || row == 9;
        const bool secondSlice = array && row == 10;
        expect(words[row * 12 + lane], empty ? 0 : secondSlice ? 0x5555 : bits == 16 ? halves[index] : lows[index], name, row, lane);
        expect(words[row * 12 + 4 + lane], empty || bits != 64 ? 0 : secondSlice ? 0xaaaaaaaa : highs[index], name, row, lane + 4);
      }
      expect(words[row * 12 + 8], 1, name, row, 8); expect(words[row * 12 + 9], 0x13579bdf, name, row, 9);
      expect(words[row * 12 + 10], row, name, row, 10); expect(words[row * 12 + 11], bits, name, row, 11);
    }
  }
  readback->Unmap(0, nullptr);
  std::printf("OWNED_D3D12_ADVANCED_TEXTURE_CASE_PASS %s_shape%u\n", name, shape);
  std::fflush(stdout);
}
}
int wmain() {
  using Backend = const char *(WINAPI *)();
  auto backend = reinterpret_cast<Backend>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "OwnedGraphicsBackendId"));
  if (!backend || !backend() || std::strcmp(backend(), kExpectedBackendId)) return fail("advanced backend");
  std::printf("OWNED_D3D12_ADVANCED_TEXTURE_BACKEND id=%s\n", backend());
  std::fflush(stdout);
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "advanced device");
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
  D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14{};
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)), "advanced OPTIONS12");
  check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &options14, sizeof(options14)), "advanced OPTIONS14");
  // OPTIONS21 ABI: WorkGraphsTier, ExecuteIndirectTier, Grad/Bias, ExtendedCommandInfo.
  UINT options21[4]{};
  check(device->CheckFeatureSupport(static_cast<D3D12_FEATURE>(53), options21, sizeof(options21)), "advanced OPTIONS21");
  std::printf("OWNED_D3D12_ADVANCED_TEXTURE_CAPS relaxed_format_casting=%u advanced_texture_ops=%u comparison_gradient_bias=%u\n",
              unsigned(options12.RelaxedFormatCastingSupported), unsigned(options14.AdvancedTextureOpsSupported), options21[2]);
  std::fflush(stdout);
  if (!options12.RelaxedFormatCastingSupported || !options14.AdvancedTextureOpsSupported || options21[2] != 1) return fail("advanced capabilities");
  wchar_t path[96];
  for (UINT shape = 0; shape < 6; ++shape) for (const char *stage : {"cs", "vs", "ps"}) {
    std::swprintf(path, 96, L"advanced-texture-cmp%u-%hs.dxil", shape, stage);
    runAdvanced(device.Get(), shape, 0, path, stage);
  }
  for (UINT bits : {16u, 32u, 64u}) for (UINT shape : {2u, 3u}) {
    std::swprintf(path, 96, L"advanced-texture-raw%u-%u.dxil", bits, shape);
    runAdvanced(device.Get(), shape, bits, path, "cs");
  }
  for (UINT shape = 0; shape < 6; ++shape) {
    for (const char *stage : {"cs", "vs", "ps"}) {
      std::swprintf(path, 96, L"advanced-texture-cmpgrad%u-%hs.dxil", shape, stage);
      runAdvanced(device.Get(), shape, 0, path, stage, "cmpgrad");
    }
    for (const char *stage : {"cs", "ps"}) {
      std::swprintf(path, 96, L"advanced-texture-cmpbias%u-%hs.dxil", shape, stage);
      runAdvanced(device.Get(), shape, 0, path, stage, "cmpbias");
    }
  }
  std::puts("OWNED_D3D12_ADVANCED_TEXTURE_PASS cases=54");
  return 0;
}
