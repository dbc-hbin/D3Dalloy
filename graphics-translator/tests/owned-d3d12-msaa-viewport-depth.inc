// Clip-disabled triangles expose out-of-range viewport Z to the pixel shader,
// while their actual D32 sample writes are clamped by the output merger.
void msaaViewportDepth(Fixture &f, const MSReader &reader,
                       const std::array<D3D12_SHADER_BYTECODE, 30> &code,
                       UINT count, float clipZ) {
  const float interpolatedZ = .2f + .6f * clipZ;
  const float storedZ = clipZ < 0 ? .2f : .8f;
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
        "viewport out-of-range color");
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtvHeap;
  check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "viewport RTV heap");
  auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
  f.device->CreateRenderTargetView(color.Get(), nullptr, rtv);
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = DXGI_FORMAT_D32_FLOAT;
  clear.DepthStencil.Depth = clipZ < 0 ? 1 : 0;
  ComPtr<ID3D12Resource> depth;
  check(f.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth)),
        "viewport DSV");
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  ComPtr<ID3D12DescriptorHeap> dsvHeap;
  check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)), "viewport DSV heap");
  auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
  dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
  dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
  f.device->CreateDepthStencilView(depth.Get(), &dsvDesc, dsv);
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors = 3;
  hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  check(f.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)), "viewport SRV heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  auto handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
  srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  f.device->CreateShaderResourceView(color.Get(), &srv, handle);
  const UINT increment = f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  handle.ptr += increment;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  f.device->CreateShaderResourceView(depth.Get(), &srv, handle);
  handle.ptr += increment;
  srv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  f.device->CreateShaderResourceView(color.Get(), &srv, handle);
  auto image = buffer(f.device.Get(), 3072, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(f.device.Get(), 3072, D3D12_HEAP_TYPE_READBACK,
                         D3D12_RESOURCE_STATE_COPY_DEST);
  const StageTriangle triangle{{{{-1,-1,clipZ,1}, {0,0,0,0}},
                                {{-1,3,clipZ,1}, {0,0,0,0}},
                                {{3,-1,clipZ,1}, {0,0,0,0}}}};
  auto vb = buffer(f.device.Get(), sizeof(triangle), D3D12_HEAP_TYPE_UPLOAD,
                   D3D12_RESOURCE_STATE_GENERIC_READ);
  void *mapped = nullptr;
  check(vb->Map(0, nullptr, &mapped), "viewport geometry map");
  std::memcpy(mapped, triangle.data(), sizeof(triangle)); vb->Unmap(0, nullptr);
  D3D12_INPUT_ELEMENT_DESC input[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = f.shader.root.Get(); pso.InputLayout = {input, 2};
  pso.VS = code[6]; pso.PS = code[23];
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = FALSE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
  pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  pso.SampleDesc.Count = count; pso.SampleMask = UINT_MAX;
  pso.DepthStencilState.DepthEnable = TRUE;
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  ComPtr<ID3D12PipelineState> pipeline;
  check(f.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)),
        "out-of-range viewport PSO");
  auto statistics = queryHeap(f.device.Get(), 1);
  auto result = buffer(f.device.Get(), 128, D3D12_HEAP_TYPE_READBACK,
                       D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_QUERY_HEAP_DESC occlusionDesc{};
  occlusionDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  occlusionDesc.Count = occlusionDesc.NodeMask = 1;
  ComPtr<ID3D12QueryHeap> occlusion;
  check(f.device->CreateQueryHeap(&occlusionDesc, IID_PPV_ARGS(&occlusion)),
        "viewport occlusion heap");
  auto visible = buffer(f.device.Get(), 8, D3D12_HEAP_TYPE_READBACK,
                        D3D12_RESOURCE_STATE_COPY_DEST);
  auto r = recording(f.device.Get()); auto *l = r.list.Get(); f.prepare(l);
  const float empty[]{0, 0, 0, 0};
  l->ClearRenderTargetView(rtv, empty, 0, nullptr);
  l->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, clear.DepthStencil.Depth,
                           0, 0, nullptr);
  l->SetGraphicsRootSignature(f.shader.root.Get()); l->SetPipelineState(pipeline.Get());
  l->SetGraphicsRootUnorderedAccessView(0, f.entries->GetGPUVirtualAddress());
  l->SetGraphicsRootUnorderedAccessView(1, f.entries->GetGPUVirtualAddress() + 16);
  D3D12_VERTEX_BUFFER_VIEW view{vb->GetGPUVirtualAddress(), sizeof(triangle), sizeof(StageVertex)};
  l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  l->IASetVertexBuffers(0, 1, &view);
  D3D12_VIEWPORT viewport{0, 0, 4, 4, .2f, .8f};
  D3D12_RECT scissor{0, 0, 4, 4};
  l->RSSetViewports(1, &viewport); l->RSSetScissorRects(1, &scissor);
  l->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  l->BeginQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->BeginQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  l->DrawInstanced(3, 1, 0, 0);
  l->EndQuery(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
  l->EndQuery(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
  l->ResolveQueryData(statistics.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                      0, 1, result.Get(), 0);
  l->ResolveQueryData(occlusion.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                      0, 1, visible.Get(), 0);
  transition(l, color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  transition(l, depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  l->SetComputeRootSignature(reader.root.Get()); l->SetPipelineState(reader.pipeline.Get());
  ID3D12DescriptorHeap *heaps[]{srvHeap.Get()}; l->SetDescriptorHeaps(1, heaps);
  l->SetComputeRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
  l->SetComputeRootUnorderedAccessView(1, image->GetGPUVirtualAddress());
  l->SetComputeRoot32BitConstant(2, count, 0);
  l->SetComputeRoot32BitConstant(2, 1, 1);
  l->SetComputeRoot32BitConstant(2, 0, 2);
  l->Dispatch(1, 1, 1);
  transition(l, image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
             D3D12_RESOURCE_STATE_COPY_SOURCE);
  l->CopyBufferRegion(readback.Get(), 0, image.Get(), 0, 3072);
  f.finish(l); f.submit(r);
  const Stats stats = readStats(result.Get(), 0);
  require(stats[0] == 3 && stats[2] == 3 && stats[7] == 16,
          "unclipped viewport pixel shader did not execute once per pixel");
  D3D12_RANGE visibleRange{0, 8};
  check(visible->Map(0, &visibleRange, &mapped), "viewport occlusion map");
  require(*static_cast<const UINT64 *>(mapped) == 16u * count,
          "unclipped viewport DSV lost sample occlusion");
  D3D12_RANGE emptyRange{0, 0}; visible->Unmap(0, &emptyRange);
  D3D12_RANGE imageRange{0, 3072};
  check(readback->Map(0, &imageRange, &mapped), "viewport depth samples map");
  const float *values = static_cast<const float *>(mapped);
  for (UINT pix = 0; pix < 16; ++pix)
    for (UINT sid = 0; sid < count; ++sid) {
      const UINT at = (pix * 4 + sid) * 4;
      require(std::fabs(values[at] - interpolatedZ) < .0001f &&
              std::fabs(values[256 + at] - storedZ) < .0001f &&
              std::fabs(values[at + 3] - 1) < .001f,
              "PS Position Z was clamped or output DSV depth was not clamped");
    }
  readback->Unmap(0, &emptyRange);
  std::printf("%s_GPU_CASE_PASS case=msaa_%ux_viewport_unclamped_%s ps=16 depth=%g\n",
              marker, count, clipZ < 0 ? "near" : "far", storedZ);
}
