// Eight-sample boundary: exact pixel-frequency atomic return, coverage, and independent sample-frequency control.
static constexpr char eightShaderSource[] = R"(
RWStructuredBuffer<uint> vertexEntries : register(u0);
RWStructuredBuffer<uint> pixelEntries : register(u1);
struct VI { float4 position : POSITION; };
struct VO { float4 position : SV_Position; };
VO vsMain(VI input) {
  uint old;
  InterlockedAdd(vertexEntries[0], 1, old);
  VO result;
  result.position = input.position;
  return result;
}
float4 psSample(VO input, uint sample : SV_SampleIndex) : SV_Target0 {
  uint pixel = (uint)input.position.y * 8 + (uint)input.position.x;
  uint old;
  InterlockedOr(pixelEntries[pixel], 1u << sample, old);
  InterlockedAdd(pixelEntries[64 + sample], 1, old);
  return float4(.125, .25, .375, 1);
}
float4 psPixel(VO input) : SV_Target0 {
  uint pixel = (uint)input.position.y * 8 + (uint)input.position.x;
  uint old;
  InterlockedAdd(pixelEntries[pixel], 1, old);
  return float4((old + 1) * .25, .5, .75, 1);
}
)";
static constexpr char eightReaderSource[] = R"(
Texture2DMS<float4> image : register(t0);
RWStructuredBuffer<float4> output : register(u0);
[numthreads(4,4,1)] void main(uint3 id : SV_DispatchThreadID) {
  for (uint sample = 0; sample < 8; ++sample)
    output[(id.y * 8 + id.x) * 8 + sample] = image.Load(int2(id.xy), sample);
}
)";

void msaaEightBoundary(Fixture &f, bool dxil) {
  constexpr UINT width = 8, pixels = 64, samples = 8;
  constexpr UINT counterBytes = 352, imageBytes = pixels * samples * 4 * sizeof(float);
  ID3D12Device *device = f.device.Get();
  struct Snapshot {
    std::array<UINT, counterBytes / sizeof(UINT)> counters{};
    std::array<float, imageBytes / sizeof(float)> image{};
  };
  D3D12_ROOT_PARAMETER graphicsParameters[2]{};
  for (UINT i = 0; i < 2; ++i) {
    graphicsParameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    graphicsParameters[i].Descriptor.ShaderRegister = i;
    graphicsParameters[i].ShaderVisibility = i ? D3D12_SHADER_VISIBILITY_PIXEL :
                                                D3D12_SHADER_VISIBILITY_VERTEX;
  }
  D3D12_ROOT_SIGNATURE_DESC rootDesc{2, graphicsParameters};
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob;
  check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &blob, nullptr), "graphics root serialize");
  ComPtr<ID3D12RootSignature> graphicsRoot;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                    IID_PPV_ARGS(&graphicsRoot)), "graphics root");
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER readParameters[2]{};
  readParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  readParameters[0].DescriptorTable = {1, &range};
  readParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  readParameters[1].Descriptor.ShaderRegister = 0;
  D3D12_ROOT_SIGNATURE_DESC readRootDesc{2, readParameters};
  check(D3D12SerializeRootSignature(&readRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &blob, nullptr), "reader root serialize");
  ComPtr<ID3D12RootSignature> readRoot;
  check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                    IID_PPV_ARGS(&readRoot)), "reader root");
  auto vs = compile(eightShaderSource, "vsMain", "vs_5_0");
  auto samplePS = compile(eightShaderSource, "psSample", "ps_5_0");
  auto pixelPS = compile(eightShaderSource, "psPixel", "ps_5_0");
  auto readCS = compile(eightReaderSource, "main", "cs_5_0");
  D3D12_INPUT_ELEMENT_DESC input{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                  0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
  pipelineDesc.pRootSignature = graphicsRoot.Get();
  pipelineDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pipelineDesc.PS = {samplePS->GetBufferPointer(), samplePS->GetBufferSize()};
  pipelineDesc.InputLayout = {&input, 1};
  pipelineDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pipelineDesc.SampleMask = UINT_MAX;
  pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
  pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipelineDesc.NumRenderTargets = 1;
  pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pipelineDesc.SampleDesc.Count = samples;
  ComPtr<ID3D12PipelineState> samplePipeline, pixelPipeline, positionsPipeline;
  check(device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&samplePipeline)),
        "sample PSO");
  pipelineDesc.PS = {pixelPS->GetBufferPointer(), pixelPS->GetBufferSize()};
  check(device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pixelPipeline)),
        "pixel PSO");
  if (dxil) {
    const auto samplePositions = shaderFile("pipeline-statistics-psMSPosition8.dxil");
    pipelineDesc.PS = {samplePositions.data(), samplePositions.size()};
    check(device->CreateGraphicsPipelineState(&pipelineDesc,
          IID_PPV_ARGS(&positionsPipeline)), "8x DXIL sample position PSO");
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC readDesc{};
  readDesc.pRootSignature = readRoot.Get();
  readDesc.CS = {readCS->GetBufferPointer(), readCS->GetBufferSize()};
  ComPtr<ID3D12PipelineState> readPipeline;
  check(device->CreateComputePipelineState(&readDesc, IID_PPV_ARGS(&readPipeline)),
        "sample reader PSO");
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC colorDesc{};
  colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  colorDesc.Width = colorDesc.Height = width;
  colorDesc.DepthOrArraySize = colorDesc.MipLevels = 1;
  colorDesc.SampleDesc.Count = samples;
  colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> color;
  check(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &colorDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)), "MSAA target");
  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "RTV heap");
  auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(color.Get(), nullptr, rtv);
  D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = 1;
  srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap)), "SRV heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = colorDesc.Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  device->CreateShaderResourceView(color.Get(), &srv,
                                   srvHeap->GetCPUDescriptorHandleForHeapStart());
  auto entries = buffer(device, counterBytes, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto zero = buffer(device, counterBytes, D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
  auto entriesReadback = buffer(device, counterBytes, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_STATE_COPY_DEST);
  auto image = buffer(device, imageBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto imageReadback = buffer(device, imageBytes, D3D12_HEAP_TYPE_READBACK,
                              D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped = nullptr;
  check(zero->Map(0, nullptr, &mapped), "zero map");
  std::memset(mapped, 0, counterBytes);
  zero->Unmap(0, nullptr);
  // Same 8x8 contained triangle as the original native 98-sample repro.
  const std::array<float, 12> triangle{-.875f,.875f,.5f,1, .875f,.875f,.5f,1,
                                       -.875f,-.875f,.5f,1};
  auto vertices = buffer(device, sizeof(triangle), D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
  check(vertices->Map(0, nullptr, &mapped), "vertex map");
  std::memcpy(mapped, triangle.data(), sizeof(triangle));
  vertices->Unmap(0, nullptr);
  auto draw = [&](UINT mode) {
    auto r = recording(device);
    auto *list = r.list.Get();
    list->CopyBufferRegion(entries.Get(), 0, zero.Get(), 0, counterBytes);
    transition(list, entries.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float clear[]{0, 0, 0, 1};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->SetGraphicsRootSignature(graphicsRoot.Get());
    list->SetPipelineState(mode == 0 ? samplePipeline.Get() :
                           mode == 1 ? pixelPipeline.Get() : positionsPipeline.Get());
    list->SetGraphicsRootUnorderedAccessView(0, entries->GetGPUVirtualAddress());
    list->SetGraphicsRootUnorderedAccessView(1, entries->GetGPUVirtualAddress() + 16);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vb{vertices->GetGPUVirtualAddress(), sizeof(triangle), 16};
    list->IASetVertexBuffers(0, 1, &vb);
    D3D12_VIEWPORT viewport{0, 0, 8, 8, 0, 1};
    D3D12_RECT scissor{0, 0, 8, 8};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetComputeRootSignature(readRoot.Get());
    list->SetPipelineState(readPipeline.Get());
    ID3D12DescriptorHeap *heaps[]{srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, image->GetGPUVirtualAddress());
    list->Dispatch(2, 2, 1);
    transition(list, image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(imageReadback.Get(), 0, image.Get(), 0, imageBytes);
    transition(list, image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(list, entries.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(entriesReadback.Get(), 0, entries.Get(), 0, counterBytes);
    transition(list, entries.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    f.submit(r);
    Snapshot result;
    D3D12_RANGE counterRange{0, counterBytes};
    check(entriesReadback->Map(0, &counterRange, &mapped), "counter map");
    std::memcpy(result.counters.data(), mapped, counterBytes);
    D3D12_RANGE empty{0, 0};
    entriesReadback->Unmap(0, &empty);
    D3D12_RANGE imageRange{0, imageBytes};
    check(imageReadback->Map(0, &imageRange, &mapped), "image map");
    std::memcpy(result.image.data(), mapped, imageBytes);
    imageReadback->Unmap(0, &empty);
    return result;
  };
  const Snapshot reference = draw(0);
  const Snapshot actual = draw(1);
  require(reference.counters[0] == 3 && actual.counters[0] == 3,
          "original VS executed more than once per vertex");
  UINT coveredPixels = 0, coveredSamples = 0;
  for (UINT pixel = 0; pixel < pixels; ++pixel) {
    const UINT mask = reference.counters[4 + pixel];
    if (mask) ++coveredPixels;
    require(actual.counters[4 + pixel] == (mask ? 1u : 0u),
            "pixel-frequency atomic was not exactly once per covered pixel");
    for (UINT sample = 0; sample < samples; ++sample) {
      const UINT at = (pixel * samples + sample) * 4;
      const bool covered = (mask & (1u << sample)) != 0;
      if (covered) ++coveredSamples;
      require(std::fabs(reference.image[at] - (covered ? .125f : 0.f)) < .01f,
              "sample-frequency control disagrees with per-sample coverage");
      require(std::fabs(actual.image[at] - (covered ? .25f : 0.f)) < .01f &&
              std::fabs(actual.image[at + 1] - (covered ? .5f : 0.f)) < .01f &&
              std::fabs(actual.image[at + 2] - (covered ? .75f : 0.f)) < .01f &&
              std::fabs(actual.image[at + 3] - 1) < .01f,
              "atomic-return color not broadcast to exactly covered samples");
    }
  }
  for (UINT sample = 0; sample < samples; ++sample) {
    UINT expected = 0;
    for (UINT pixel = 0; pixel < pixels; ++pixel)
      if (reference.counters[4 + pixel] & (1u << sample)) ++expected;
    require(reference.counters[4 + 64 + sample] == expected,
            "sample-frequency control bucket disagrees with per-sample coverage");
  }
  require(coveredPixels == 35 && coveredSamples == 189,
          "8x contained triangle lost expected 35-pixel/189-sample coverage");
  if (dxil) {
    const Snapshot positions = draw(2);
    constexpr std::array<std::array<float, 2>, 8> standardPositions{{
      {{.5625f, .3125f}}, {{.4375f, .6875f}}, {{.8125f, .5625f}},
      {{.3125f, .1875f}}, {{.1875f, .8125f}}, {{.0625f, .4375f}},
      {{.6875f, .9375f}}, {{.9375f, .0625f}}
    }};
    require(positions.counters[0] == 3, "8x position path reran VS");
    for (UINT pixel = 0; pixel < pixels; ++pixel) {
      const UINT mask = reference.counters[4 + pixel];
      require(positions.counters[4 + pixel] == mask,
              "DXIL eight-sample position path differs from coverage reference");
      for (UINT sample = 0; sample < samples; ++sample) {
        const UINT at = (pixel * samples + sample) * 4;
        if (mask & (1u << sample))
          require(std::fabs(positions.image[at] - standardPositions[sample][0]) < .005f &&
                  std::fabs(positions.image[at + 1] - standardPositions[sample][1]) < .005f &&
                  std::fabs(positions.image[at + 2] - (sample + .5f) / 8) < .005f,
                  "8x render-target sample position does not match raster sample index");
        else require(positions.image[at] == 0 && positions.image[at + 1] == 0,
                     "8x sample-position invocation escaped geometric coverage");
      }
    }
    for (UINT sample = 0; sample < samples; ++sample)
      require(positions.counters[4 + 64 + sample] == reference.counters[4 + 64 + sample],
              "eight-sample DXIL position path emitted unexpected sample invocations");
  }
  std::printf("%s_GPU_CASE_PASS case=msaa_8x_boundary pixels=%u covered_samples=%u atomics=%u\n",
              marker, coveredPixels, coveredSamples, coveredPixels);
}
