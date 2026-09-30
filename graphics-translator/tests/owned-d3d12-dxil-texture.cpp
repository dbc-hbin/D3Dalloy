#define wmain owned_d3d12_dxil_base_wmain
#include "owned-d3d12-dxil.cpp"
#undef wmain

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

int runTextureOps(int argc, wchar_t **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: owned-d3d12-dxil-texture.exe <basic-texture-vs.dxil> <basic-texture-ps.dxil>\n");
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
  samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
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
  std::puts("OWNED_D3D12_DXIL_TEXTURE_PASS");
  return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  return runTextureOps(argc, argv);
}
