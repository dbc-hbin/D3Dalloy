// Included by owned-d3d12-pipeline-statistics.cpp after its fixture helpers.
// A sample-frequency default-depth draw establishes the DSV. Independent
// pixel-frequency, sample-frequency, and depth-only EQUAL draws then exercise
// that exact sample depth across each execution route.
void msaaDepthInterop(Fixture &f, const MSReader &reader,
                      const std::array<D3D12_SHADER_BYTECODE, 30> &code,
                      UINT count, DXGI_FORMAT format, INT bias) {
  const bool d16 = format == DXGI_FORMAT_D16_UNORM;
  auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = desc.Height = 4;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = count;
  desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> color;
  check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&color)),
        "depth interop color");
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)),
        "depth interop RTV heap");
  const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  f.device->CreateRenderTargetView(color.Get(), nullptr, rtv);
  desc.Format = d16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = format;
  clear.DepthStencil.Depth = 1;
  ComPtr<ID3D12Resource> depth;
  check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)),
        "depth interop DSV");
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)),
        "depth interop DSV heap");
  const auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
  dsvDesc.Format = format;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
  f.device->CreateDepthStencilView(depth.Get(), &dsvDesc, dsv);
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 3;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)),
        "depth interop SRV heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  auto handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
  f.device->CreateShaderResourceView(color.Get(), &srv, handle);
  handle.ptr += f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  srv.Format = d16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT;
  f.device->CreateShaderResourceView(depth.Get(), &srv, handle);
  handle.ptr += f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  f.device->CreateShaderResourceView(color.Get(), &srv, handle);
  auto image = buffer(f.device.Get(), 3072, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(f.device.Get(), 3072, D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COPY_DEST);
  // W is constant, making the default depth a screen-affine plane.
  const StageTriangle triangle{{{{-1,-1,.25f,1}, {0,1,1,1}},
                                {{-1,3,.75f,1}, {0,-1,1,1}},
                                {{3,-1,.5f,1}, {2,1,1,1}}}};
  auto vb = buffer(f.device.Get(), sizeof(triangle), D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(vb->Map(0, nullptr, &mapped), "depth interop VB map");
  std::memcpy(mapped, triangle.data(), sizeof(triangle)); vb->Unmap(0, nullptr);
  D3D12_INPUT_ELEMENT_DESC input[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = f.shader.root.Get();
  pso.InputLayout = {input, 2};
  pso.VS = code[6];
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.RasterizerState.DepthBias = bias;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso.DSVFormat = format;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  pso.SampleDesc.Count = count;
  // Establish default DSV depth per sample without any SV_Depth override.
  pso.PS = code[24];
  ComPtr<ID3D12PipelineState> prepass, pixel, sample, depthOnly, depthWrite,
                               centroid, sampleInterpolated;
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&prepass)),
        "depth interop sample prepass");
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.PS = code[23];
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pixel)),
        "depth interop pixel EQUAL");
  pso.PS = code[24];
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sample)),
        "depth interop sample EQUAL");
  pso.PS = {};
  pso.NumRenderTargets = 0;
  pso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&depthOnly)),
        "depth interop depth-only EQUAL");
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  pso.SampleMask = UINT_MAX;
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&depthWrite)),
        "depth interop depth-only default write");
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.PS = code[26];
  pso.SampleMask = 1;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  if (!bias) check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&centroid)),
                   "centroid output depth masked sample");
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  pso.SampleMask = UINT_MAX;
  pso.PS = code[25];
  if (!bias && code[25].pShaderBytecode) check(f.device->CreateGraphicsPipelineState(&pso,
                   IID_PPV_ARGS(&sampleInterpolated)), "explicitly sample-interpolated depth PSO");
  ID3D12PipelineState *passes[]{prepass.Get(), pixel.Get(), sample.Get(),
                                depthOnly.Get(), centroid.Get(), depthWrite.Get(),
                                sampleInterpolated.Get()};
  const char *passNames[]{"sample_prepass", "pixel_equal", "sample_equal",
                          "depth_only_equal", "centroid_output_depth_write",
                          "depth_only_default_write", "explicit_sample_position_equal"};
  auto statistics = queryHeap(f.device.Get(), 1);
  auto result = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK,
                       D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_QUERY_HEAP_DESC occlusionDesc{};
  occlusionDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  occlusionDesc.Count = occlusionDesc.NodeMask = 1;
  ComPtr<ID3D12QueryHeap> occlusion;
  check(f.device->CreateQueryHeap(&occlusionDesc, IID_PPV_ARGS(&occlusion)),
        "depth interop occlusion heap");
  auto visible = buffer(f.device.Get(), 8, D3D12_HEAP_TYPE_READBACK,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  std::array<float, 64> baseline{};
  std::array<UINT, 16> baselineCoverage{};
  for (UINT pass = 0; pass < (bias ? 4u : code[25].pShaderBytecode ? 7u : 6u); ++pass) {
    auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
    const float empty[]{0, 0, 0, 0};
    l->ClearRenderTargetView(rtv, empty, 0, nullptr);
    if (!pass || pass == 5) l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    l->SetGraphicsRootSignature(f.shader.root.Get());
    l->SetPipelineState(passes[pass]);
    l->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
    l->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 16);
    D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), sizeof(triangle), sizeof(StageVertex)};
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->IASetVertexBuffers(0, 1, &view);
    D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    D3D12_RECT scissor{0, 0, 4, 4};
    l->RSSetViewports(1, &viewport); l->RSSetScissorRects(1, &scissor);
    l->OMSetRenderTargets(pass == 3 || pass == 5 ? 0 : 1,
                          pass == 3 || pass == 5 ? nullptr : &rtv,
                          FALSE, &dsv);
    l->BeginQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    l->BeginQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    l->DrawInstanced(3, 1, 0, 0);
    l->EndQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    l->EndQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    l->ResolveQueryData(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
                        result.Get(), 0);
    l->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0, 1,
                        visible.Get(), 0);
    transition(l, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(l, depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    l->SetComputeRootSignature(reader.root.Get());
    l->SetPipelineState(reader.pipeline.Get());
    ID3D12DescriptorHeap *heaps[]{srvHeap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    l->SetComputeRootUnorderedAccessView(1, image->GetGPUVirtualAddress());
    l->SetComputeRoot32BitConstant(2, count, 0);
    l->SetComputeRoot32BitConstant(2, 1, 1);
    l->SetComputeRoot32BitConstant(2, 0, 2);
    l->Dispatch(1, 1, 1);
    transition(l, image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyBufferRegion(readback.Get(), 0, image.Get(), 0, 3072);
    transition(l, image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(l, color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(l, depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    f.finish(l); f.submit(r);
    const Stats stats = readStats(result.Get(), 0);
    require(stats[0] == 3 && stats[2] == 3 && stats[5] == 1 && stats[6] == 1 &&
            stats[7] == (pass == 0 || pass == 2 || pass == 6 ? 16 * count :
                         pass == 1 || pass == 4 ? 16 : 0),
            passNames[pass]);
    D3D12_RANGE visibleRange{0, 8};
    check(visible->Map(0, &visibleRange, &mapped), "depth interop occlusion map");
    require(*static_cast<const UINT64 *>(mapped) == (pass == 4 ? 16u : 16u * count),
            "depth interop occlusion lost samples across pixel/sample/noPS routes");
    D3D12_RANGE emptyRange{0, 0}; visible->Unmap(0, &emptyRange);
    D3D12_RANGE range{0, 3072};
    check(readback->Map(0, &range, &mapped), "depth interop samples map");
    const float *values = static_cast<const float *>(mapped);
    for (UINT pix = 0; pix < 16; ++pix)
      for (UINT sid = 0; sid < count; ++sid) {
        const UINT at = (pix * 4 + sid) * 4;
        const float z = values[256 + at];
        if (!pass) {
          baseline[pix * 4 + sid] = z;
          if (values[at + 3] > .99f) baselineCoverage[pix] |= 1u << sid;
        }
        else if (pass == 5) require(z == baseline[pix * 4 + sid],
                     "depth-only write diverged from sample-frequency default depth");
        else if (pass != 4 || sid != 0) require(z == baseline[pix * 4 + sid],
                     "depth EQUAL route unexpectedly modified the DSV sample");
        const float center = .5f - .0625f * (float(pix / 4) + .5f) +
                             .03125f * (float(pix % 4) + .5f);
        if (pass == 0 || pass == 2 || pass == 6) {
          if (pass == 6) require(std::fabs(values[at] - z) < (d16 ? .00004f : .000002f),
                                 "explicit sample-interpolated Position Z differs from DSV sample depth");
          else if (!bias) require(std::fabs(values[at] - center) < .0001f,
                             "ordinary sample-frequency Position Z differs from pixel center");
          if (pass == 2 || pass == 6) require(std::fabs(values[at + 3] - 1) < .001f,
                  "sample-frequency depth EQUAL did not shade surviving sample");
        } else if (pass == 1)
          require((bias != 0 || std::fabs(values[at] - center) < .0001f) &&
                  std::fabs(values[at + 3] - 1) < .001f,
                  "pixel default depth EQUAL or center Position Z changed");
        else if (pass == 3 || pass == 5)
          require(values[at + 3] == 0,
                  "depth-only EQUAL unexpectedly wrote a color sample");
        else if (sid == 0) {
          require(std::fabs(values[at] - center) < .0001f &&
                  std::fabs(z - (values[at] + .0625f)) < (d16 ? .00004f : .000002f) &&
                  std::fabs(values[at + 3] - 1) < .001f,
                  "fully covered centroid SV_Depth must use center and update only masked sample");
        }
        else require(values[at + 3] == 0,
                     "centroid output depth escaped PSO sample mask");
      }
    readback->Unmap(0, &emptyRange);
    if (!pass)
      for (UINT pix = 0; pix < 16; ++pix)
        require(baselineCoverage[pix] == (1u << count) - 1,
                "depth interop geometry must cover every sample independent of PSO mask");
    D3D12_RANGE counterRange{0, 16 + 16 * sizeof(UINT)};
    check(f.entriesReadback->Map(0, &counterRange, &mapped), "depth interop PS counters map");
    const UINT *words = static_cast<const UINT *>(mapped);
    require(words[0] == 3, "depth interop original VS reran");
    if (pass == 1 || pass == 4)
      for (UINT pix = 0; pix < 16; ++pix)
        require(words[4 + pix] == 1, "depth EQUAL PS not exactly once per pixel");
    D3D12_RANGE emptyCounters{0, 0}; f.entriesReadback->Unmap(0, &emptyCounters);
    std::printf("%s_GPU_CASE_PASS case=msaa_depth_%ux_%s_bias%d_%s ps=%llu\n",
                marker, count, d16 ? "d16" : "d32", bias, passNames[pass],
                static_cast<unsigned long long>(stats[7]));
  }
}
