// Shared exact consumer oracle for the geometry and tessellation suites.
int casesIndirectMutation(Context &c, bool tessellation) {
  auto source = readFile("gsindirect.hlsl");
  auto load = [&](const char *entry, const char *profile) {
    if (useDxil) return readFile(std::string("gsindirect-") + entry + ".dxil");
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "gsindirect.hlsl", nullptr, nullptr, entry, profile,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) { if (errors) std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr); fail(entry, hr); }
    auto *bytes = static_cast<const uint8_t *>(code->GetBufferPointer());
    return std::vector<uint8_t>(bytes, bytes + code->GetBufferSize());
  };
  auto put = [&](ID3D12Resource *resource, const void *data, size_t bytes, size_t offset = 0) {
    void *mapped; D3D12_RANGE none{};
    check(resource->Map(0, &none, &mapped), "mutation-upload");
    std::memcpy(static_cast<uint8_t *>(mapped) + offset, data, bytes); resource->Unmap(0, &none);
  };
  auto read = [&](ID3D12Resource *resource, size_t bytes) {
    void *mapped; D3D12_RANGE range{0, bytes}, none{};
    check(resource->Map(0, &range, &mapped), "mutation-read");
    std::vector<uint8_t> result(static_cast<uint8_t *>(mapped), static_cast<uint8_t *>(mapped) + bytes);
    resource->Unmap(0, &none); return result;
  };
  D3D12_ROOT_PARAMETER parameters[4]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 2};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[1].Descriptor.ShaderRegister = 1;
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC root_desc{4, parameters, 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT};
  ComPtr<ID3DBlob> code, errors; ComPtr<ID3D12RootSignature> root;
  check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &errors), "mutation-root-code");
  check(c.device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root)), "mutation-root");
  struct Command {
    uint32_t constants[2]; uint64_t cbv, srv, uav;
    D3D12_VERTEX_BUFFER_VIEW vertex; D3D12_INDEX_BUFFER_VIEW index;
    D3D12_DRAW_INDEXED_ARGUMENTS draw;
  };
  static_assert(offsetof(Command, draw) == 64);
  D3D12_INDIRECT_ARGUMENT_DESC arguments[7]{};
  arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT; arguments[0].Constant = {0, 0, 2};
  arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW; arguments[1].ConstantBufferView.RootParameterIndex = 1;
  arguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW; arguments[2].ShaderResourceView.RootParameterIndex = 2;
  arguments[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW; arguments[3].UnorderedAccessView.RootParameterIndex = 3;
  arguments[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
  arguments[5].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
  arguments[6].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(Command), 7, arguments, 0};
  ComPtr<ID3D12CommandSignature> signature;
  check(c.device->CreateCommandSignature(&signature_desc, root.Get(), IID_PPV_ARGS(&signature)), "mutation-signature");
  auto vs = load("vs_main", "vs_5_0"), ps = load("ps_main", "ps_5_0");
  std::vector<uint8_t> hs, ds;
  if (tessellation) { hs = load("hs_main", "hs_5_0"); ds = load("ds_main", "ds_5_0"); }
  auto zero = buffer(c.device.Get(), 4096, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  const uint8_t zeros[4096]{}; put(zero.Get(), zeros, sizeof(zeros));
  auto so = buffer(c.device.Get(), 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  auto so_readback = buffer(c.device.Get(), 4096, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  std::array<ComPtr<ID3D12Resource>, 2> vb, ib, cbv, srv, uav, uav_readback;
  std::array<Command, 2> slots{};
  for (uint32_t i = 0; i < 2; ++i) {
    vb[i] = buffer(c.device.Get(), 128, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ib[i] = buffer(c.device.Get(), 128, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    cbv[i] = buffer(c.device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    srv[i] = buffer(c.device.Get(), 64, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uav[i] = buffer(c.device.Get(), 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    uav_readback[i] = buffer(c.device.Get(), 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    const float vertices[][2] = {{4,4},{4,4},{-0.75f + float(i)*0.75f, 0.5f},{4,4}};
    put(vb[i].Get(), vertices, sizeof(vertices), 16);
    const float red = i ? 0.75f : 0.25f, green = i ? 0.25f : 0.75f;
    put(cbv[i].Get(), &red, 4, 256); put(srv[i].Get(), &green, 4, 16);
    slots[i].constants[0] = 1; slots[i].constants[1] = 64 + 64*i;
    slots[i].cbv = cbv[i]->GetGPUVirtualAddress() + 256; slots[i].srv = srv[i]->GetGPUVirtualAddress() + 16;
    slots[i].uav = uav[i]->GetGPUVirtualAddress() + 16;
    slots[i].vertex = {vb[i]->GetGPUVirtualAddress() + 16, sizeof(vertices), 8};
    slots[i].index = {ib[i]->GetGPUVirtualAddress() + 16, 64, i ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
  }
  int cases = 0;
  for (uint32_t kind = 0; kind < (tessellation ? 2u : 3u); ++kind) {
    const bool with_gs = !tessellation || kind == 1;
    const char *entry = tessellation ? "gs_tri" : kind == 0 ? "gs_point" : kind == 1 ? "gs_strip" : "gs_adj";
    auto gs = with_gs ? load(entry, "gs_5_0") : std::vector<uint8_t>{};
    const auto topology = tessellation ? D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST :
        kind == 0 ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : kind == 1 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    for (uint32_t i = 0; i < 2; ++i) {
      uint32_t indices[16]{}; std::fill(std::begin(indices), std::end(indices), 1);
      if (!tessellation && kind == 1 && !i) indices[4] = 0xffffu;
      if (i) put(ib[i].Get(), indices, sizeof(indices), 16);
      else { uint16_t small[16]; for (size_t j = 0; j < 16; ++j) small[j] = uint16_t(indices[j]); put(ib[i].Get(), small, sizeof(small), 16); }
      slots[i].draw = {tessellation || kind == 0 ? 1u : kind == 1 ? 7u : 6u, 1, 1, 1, i+1};
    }
    for (bool stream_output : {false, true}) {
      const bool domain_so = tessellation && !with_gs && stream_output;
      for (uint32_t so_mode = 0; so_mode < (domain_so ? 4u : 1u); ++so_mode) {
      const bool no_raster = domain_so && (so_mode & 1);
      const bool truncated = domain_so && (so_mode & 2);
      D3D12_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
      D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get();
      p.VS = {vs.data(), vs.size()}; p.PS = {ps.data(), ps.size()}; p.GS = {gs.data(), gs.size()};
      p.HS = {hs.data(), hs.size()}; p.DS = {ds.data(), ds.size()};
      p.InputLayout = {&element, 1}; p.SampleMask = UINT_MAX; p.SampleDesc.Count = 1;
      p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; p.RasterizerState.DepthClipEnable = TRUE;
      p.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
      p.PrimitiveTopologyType = tessellation ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : kind == 0 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      p.IBStripCutValue = kind == 1 && !tessellation ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
      p.NumRenderTargets = 1; p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      D3D12_SO_DECLARATION_ENTRY declaration[] = {{0,"SV_Position",0,0,4,0},{0,"COLOR",0,0,4,0}}; UINT stride = 32;
      if (stream_output) p.StreamOutput = {declaration, 2, &stride, 1, 0};
      if (no_raster) p.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
      if (no_raster) p.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
      ComPtr<ID3D12PipelineState> pipeline;
      check(c.device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline)), "mutation-pipeline");
      auto args_upload = buffer(c.device.Get(), 1024, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      auto args = buffer(c.device.Get(), 1024, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
      Command commands[5]{}; commands[0] = slots[0]; commands[0].draw.IndexCountPerInstance = 0;
      commands[1] = slots[0]; commands[2] = slots[1]; commands[3] = slots[0]; commands[3].draw.IndexCountPerInstance = UINT_MAX;
      put(args_upload.Get(), commands, sizeof(commands), 16); const uint32_t count = 3; put(args_upload.Get(), &count, 4, 768);
      auto predicate = buffer(c.device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      const uint64_t pred = 0; put(predicate.Get(), &pred, 8);
      std::array<std::vector<uint8_t>, 2> image, so_bytes;
      std::array<std::array<uint32_t, 2>, 2> counters{};
      ComPtr<ID3D12QueryHeap> so_queries, pipeline_queries;
      ComPtr<ID3D12Resource> query_readback;
      if (domain_so) {
        query_readback = buffer(c.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 1, 0};
        check(c.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&so_queries)), "domain-so-query-heap");
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        check(c.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&pipeline_queries)), "domain-so-pipeline-query-heap");
      }
      std::array<D3D12_QUERY_DATA_SO_STATISTICS, 2> so_stats{};
      std::array<D3D12_QUERY_DATA_PIPELINE_STATISTICS, 2> pipe_stats{};
      for (uint32_t indirect = 0; indirect < 2; ++indirect) {
        c.begin();
        c.list->CopyBufferRegion(args.Get(),0,args_upload.Get(),0,1024);
        auto args_barrier = transition(args.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); c.list->ResourceBarrier(1,&args_barrier);
        for (uint32_t i=0;i<2;i++) {
          c.list->CopyBufferRegion(uav[i].Get(),0,zero.Get(),0,64);
          c.list->CopyBufferRegion(uav[i].Get(),16,srv[i].Get(),16,4);
          auto barrier=transition(uav[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); c.list->ResourceBarrier(1,&barrier);
        }
        if (stream_output) {
          c.list->CopyBufferRegion(so.Get(),0,zero.Get(),0,4096);
          auto barrier=transition(so.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_STREAM_OUT); c.list->ResourceBarrier(1,&barrier);
          D3D12_STREAM_OUTPUT_BUFFER_VIEW view{so->GetGPUVirtualAddress(), truncated ? 144u : 3072u, so->GetGPUVirtualAddress()+3072}; c.list->SOSetTargets(0,1,&view);
        }
        const float clear[4]{}; auto rtv=c.rtvs->GetCPUDescriptorHandleForHeapStart(); c.list->ClearRenderTargetView(rtv,clear,0,nullptr); c.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        D3D12_VIEWPORT viewport{0,0,32,32,0,1}; D3D12_RECT scissor{0,0,32,32}; c.list->RSSetViewports(1,&viewport); c.list->RSSetScissorRects(1,&scissor);
        c.list->SetGraphicsRootSignature(root.Get()); c.list->SetPipelineState(pipeline.Get()); c.list->IASetPrimitiveTopology(topology);
        if (domain_so) {
          ComPtr<ID3D12GraphicsCommandList5> vrs;
          check(c.list.As(&vrs), "domain-so-vrs-list");
          vrs->RSSetShadingRate(no_raster ? D3D12_SHADING_RATE_2X2 : D3D12_SHADING_RATE_1X1, nullptr);
        }
        auto bind = [&](const Command &command) {
          c.list->SetGraphicsRoot32BitConstants(0,2,command.constants,0); c.list->SetGraphicsRootConstantBufferView(1,command.cbv);
          c.list->SetGraphicsRootShaderResourceView(2,command.srv); c.list->SetGraphicsRootUnorderedAccessView(3,command.uav);
          c.list->IASetVertexBuffers(0,1,&command.vertex); c.list->IASetIndexBuffer(&command.index);
        };
        if (domain_so) {
          c.list->BeginQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
          c.list->BeginQuery(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        }
        if (indirect) {
          // Poison initial state: both active commands must replace every binding.
          bind(slots[1]); c.list->ExecuteIndirect(signature.Get(),5,args.Get(),16,args.Get(),768);
          // Rebind only the views/IA: the reset constants must suppress this draw.
          c.list->SetGraphicsRootConstantBufferView(1,slots[0].cbv); c.list->SetGraphicsRootShaderResourceView(2,slots[0].srv); c.list->SetGraphicsRootUnorderedAccessView(3,slots[0].uav);
          c.list->IASetVertexBuffers(0,1,&slots[0].vertex); c.list->IASetIndexBuffer(&slots[0].index);
          c.list->DrawIndexedInstanced(slots[0].draw.IndexCountPerInstance,1,1,1,1);
          c.list->SetPredication(predicate.Get(),0,D3D12_PREDICATION_OP_EQUAL_ZERO); c.list->ExecuteIndirect(signature.Get(),5,args.Get(),16,args.Get(),768); c.list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
        } else for (const auto &slot : slots) { bind(slot); c.list->DrawIndexedInstanced(slot.draw.IndexCountPerInstance,1,1,1,slot.draw.StartInstanceLocation); }
        if (domain_so) {
          c.list->EndQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
          c.list->ResolveQueryData(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, query_readback.Get(), 0);
          c.list->EndQuery(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
          c.list->ResolveQueryData(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, query_readback.Get(), 16);
        }
        if (stream_output) { c.list->SOSetTargets(0,0,nullptr); auto barrier=transition(so.Get(),D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE); c.list->ResourceBarrier(1,&barrier); c.list->CopyBufferRegion(so_readback.Get(),0,so.Get(),0,4096); barrier=transition(so.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST); c.list->ResourceBarrier(1,&barrier); }
        for (uint32_t i=0;i<2;i++) { auto barrier=transition(uav[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE); c.list->ResourceBarrier(1,&barrier); c.list->CopyBufferRegion(uav_readback[i].Get(),0,uav[i].Get(),0,64); barrier=transition(uav[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST); c.list->ResourceBarrier(1,&barrier); }
        auto barrier=transition(c.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE); c.list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=c.readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,32,32,1,256}; D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=c.target.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; c.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier=transition(c.target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET); c.list->ResourceBarrier(1,&barrier);
        barrier=transition(args.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_COPY_DEST); c.list->ResourceBarrier(1,&barrier); c.submit();
        image[indirect]=read(c.readback.Get(),8192); if(stream_output) so_bytes[indirect]=read(so_readback.Get(),4096);
        for(uint32_t i=0;i<2;i++) { const auto bytes=read(uav_readback[i].Get(),64); std::memcpy(&counters[indirect][i],bytes.data()+20,4); }
        if (domain_so) {
          const auto bytes = read(query_readback.Get(), 128);
          std::memcpy(&so_stats[indirect], bytes.data(), sizeof(so_stats[indirect]));
          std::memcpy(&pipe_stats[indirect], bytes.data()+16, sizeof(pipe_stats[indirect]));
        }
      }
      const std::string name=std::string("indirect-mutation-")+(tessellation ? with_gs?"tsgs":"ts" : kind==0?"gs":kind==1?"stripcut":"adjacency")+(stream_output?"-so":"")+(no_raster?"-no-raster":"")+(truncated?"-overflow":"");
      std::printf("%s_MUTATION shader=%s case=%s direct=%u,%u indirect=%u,%u\n",kPrefix,useDxil?"dxil":"dxbc",name.c_str(),counters[0][0],counters[0][1],counters[1][0],counters[1][1]);
      require(image[0]==image[1],name.c_str()); require(counters[0]==counters[1],"mutation-uav-slots");
      const uint32_t invocations=tessellation?3:kind==1?2:1; require(counters[1][0]==invocations && counters[1][1]==(!tessellation && kind==1 ? 5u : invocations),"mutation-uav-exact");
      bool colors[2]{}; for(size_t y=0;y<32;y++) for(size_t x=0;x<32;x++) { uint32_t pixel; std::memcpy(&pixel,image[1].data()+y*256+x*4,4); colors[0]|=pixel==0x40bfbf40; colors[1]|=pixel==0x804040bf; if (no_raster) require(pixel == 0, "domain-so-no-raster-image"); }
      if (no_raster) require(!colors[0] && !colors[1], "domain-so-no-raster-colors");
      else require(colors[0] && colors[1],"mutation-distinct-colors");
      if(stream_output) { require(so_bytes[0]==so_bytes[1],"mutation-so-exact"); uint64_t filled; std::memcpy(&filled,so_bytes[1].data()+3072,8); require(filled==(truncated?96u:tessellation?192u:kind==1?1344u:384u),"mutation-so-filled"); }
      if (domain_so) {
        // In the 32x32 viewport each factor-one triangle covers a 2x2 box.
        // The diagonal is a bottom/right edge, so only the top-left pixel center is included.
        for (uint32_t y = 0; y < 32; ++y)
          for (uint32_t x = 0; x < 32; ++x) {
            uint32_t pixel; std::memcpy(&pixel, image[1].data()+y*256+x*4, 4);
            const uint32_t expected_pixel = no_raster || y != 7 ? 0 :
                x == 3 ? 0x40bfbf40u : x == 15 ? 0x804040bfu : 0;
            require(pixel == expected_pixel, "domain-so-top-left-coverage");
          }
        // Factor-one CW connectivity is (_001, _100, _010); each command owns one complete triangle.
        std::vector<uint8_t> expected(4096, 0);
        const float corners[3][2] = {{-1,-1}, {-1,1}, {1,1}};
        for (uint32_t slot = 0; slot < (truncated ? 1u : 2u); ++slot)
          for (uint32_t v = 0; v < 3; ++v) {
            const float r = slot ? 0.75f : 0.25f, g = slot ? 0.25f : 0.75f;
            // SV_InstanceID is zero-based; StartInstanceLocation only offsets per-instance IA data.
            const float record[8] = {-0.75f + float(slot) * 0.75f + corners[v][0]/16.0f,
                                     0.5f + corners[v][1]/16.0f, 0, 1, r, g, g, float(64 + slot*64)/255.0f};
            std::memcpy(expected.data() + (slot*3 + v)*32, record, sizeof(record));
          }
        const uint32_t expected_filled = truncated ? 96 : 192;
        std::memcpy(expected.data() + 3072, &expected_filled, 4);
        require(so_bytes[0] == expected && so_bytes[1] == expected, "domain-so-independent-bytes-order-guards");
        for (uint32_t i = 0; i < 2; ++i) {
          require(so_stats[i].NumPrimitivesWritten == (truncated ? 1u : 2u) &&
                  so_stats[i].PrimitivesStorageNeeded == 2, "domain-so-written-needed");
          require(pipe_stats[i].DSInvocations == 6 && pipe_stats[i].GSInvocations == 0 &&
                  pipe_stats[i].CInvocations == (no_raster ? 0u : 2u) &&
                  pipe_stats[i].CPrimitives == (no_raster ? 0u : 2u) &&
                  pipe_stats[i].PSInvocations == (no_raster ? 0u : 2u), "domain-so-pipeline-statistics");
        }
        std::printf("%s_DOMAIN_SO shader=%s case=%s bytes=%u written=%llu needed=%llu ds=%llu c=%llu ps=%llu\n",
            kPrefix, useDxil ? "dxil" : "dxbc", name.c_str(), truncated ? 96u : 192u,
            (unsigned long long)so_stats[1].NumPrimitivesWritten,
            (unsigned long long)so_stats[1].PrimitivesStorageNeeded,
            (unsigned long long)pipe_stats[1].DSInvocations,
            (unsigned long long)pipe_stats[1].CPrimitives, (unsigned long long)pipe_stats[1].PSInvocations);
      }
      std::printf("%s_CASE_PASS shader=%s case=%s\n",kPrefix,useDxil?"dxil":"dxbc",name.c_str()); ++cases;
      }
    }
  }
  return cases;
}
