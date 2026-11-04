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
  // These signatures mutate only tag, preserving enabled at DWORD 0. The third
  // also changes the IBV at a four-byte-aligned (not eight-byte-aligned) offset.
  // Their unaligned 64-bit fields and nonzero draw offset also exercise the signature decoder.
  std::array<ComPtr<ID3D12CommandSignature>, 3> gpu_signatures;
  for (uint32_t mode = 0; mode < 3; ++mode) {
    D3D12_INDIRECT_ARGUMENT_DESC gpu_arguments[7]{};
    std::copy_n(arguments, 5, gpu_arguments);
    gpu_arguments[0].Constant = {0, 1, 1};
    if (mode == 2) gpu_arguments[5].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    gpu_arguments[mode == 2 ? 6 : 5].Type =
        mode == 1 ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc{mode == 2 ? 80u : mode == 1 ? 60u : 64u,
                                     mode == 2 ? 7u : 6u, gpu_arguments, 0};
    check(c.device->CreateCommandSignature(&desc, root.Get(), IID_PPV_ARGS(&gpu_signatures[mode])), "gpu-mutation-signature");
  }
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
      for (uint32_t variant = 0; variant < (!stream_output && kind == 0 ? 4u : 2u); ++variant) {
      const uint32_t mode = !stream_output && kind == 0 ? variant : variant ? 4 : 0;
      const bool capture_bounds = mode == 4;
      const bool gpu_mutation = mode > 0 && mode < 4, nonindexed = mode == 2, dynamic_index = mode == 3;
      const uint32_t command_stride = gpu_mutation ? dynamic_index ? 80u : nonindexed ? 60u : 64u : sizeof(Command);
      auto *active_signature = gpu_mutation ? gpu_signatures[mode-1].Get() : signature.Get();
      const std::string name=std::string("indirect-mutation-")+(tessellation ? with_gs?"tsgs":"ts" : kind==0?"gs":kind==1?"stripcut":"adjacency")+(stream_output?"-so":"")+(no_raster?"-no-raster":"")+(truncated?"-overflow":"")+(mode==1?"-fixed-ib":mode==2?"-nonindexed":mode==3?"-dynamic-ib":capture_bounds?"-index-bounds":"");
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
      if (gpu_mutation) {
        commands[1].draw.InstanceCount = commands[2].draw.InstanceCount = 2;
        commands[3].draw.InstanceCount = UINT_MAX;
        commands[3].cbv = commands[3].srv = commands[3].uav = UINT64_MAX;
        commands[3].vertex.BufferLocation = UINT64_MAX;
        if (dynamic_index) {
          commands[0].index.BufferLocation = commands[3].index.BufferLocation = UINT64_MAX;
          commands[0].index.SizeInBytes = commands[3].index.SizeInBytes = UINT_MAX;
        }
      }
      if (capture_bounds) {
        // The direct oracle fetches index 1 plus base 1. NULL/OOB views must
        // supply index 0 plus base 2, preserving the same full capture/output.
        // Keep the u16 strip's real cut; the u32 strip has no matching cut.
        if (tessellation || kind != 1) {
          commands[1].index = {0, 0, DXGI_FORMAT_UNKNOWN};
          commands[1].draw.BaseVertexLocation = 2;
        } else commands[2].index = {0, 0, DXGI_FORMAT_UNKNOWN};
        commands[2].index.SizeInBytes = 4;
        commands[2].draw.BaseVertexLocation = 2;
      }
      auto upload_commands = [&](uint32_t count) {
        if (!gpu_mutation) put(args_upload.Get(), commands, sizeof(commands), 16);
        else {
          std::vector<uint8_t> bytes(command_stride * 5, 0);
          for (uint32_t i = 0; i < 5; ++i) {
            auto *at = bytes.data() + command_stride * i;
            const auto &command = commands[i];
            std::memcpy(at, &command.constants[1], 4);
            std::memcpy(at+4, &command.cbv, 8); std::memcpy(at+12, &command.srv, 8);
            std::memcpy(at+20, &command.uav, 8); std::memcpy(at+28, &command.vertex, 16);
            if (dynamic_index) {
              std::memcpy(at+44, &command.index, sizeof(command.index));
              std::memcpy(at+60, &command.draw, sizeof(command.draw));
            } else if (nonindexed) {
              const D3D12_DRAW_ARGUMENTS draw{command.draw.IndexCountPerInstance, command.draw.InstanceCount,
                                             2, command.draw.StartInstanceLocation};
              std::memcpy(at+44, &draw, sizeof(draw));
            } else std::memcpy(at+44, &command.draw, sizeof(command.draw));
          }
          put(args_upload.Get(), bytes.data(), bytes.size(), 16);
        }
        const uint32_t counts[2] = {count, 0}; put(args_upload.Get(), counts, sizeof(counts), 768);
      };
      upload_commands(3);
      auto predicate = buffer(c.device.Get(), 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
      const uint64_t pred = 0; put(predicate.Get(), &pred, 8);
      std::array<std::vector<uint8_t>, 2> image, so_bytes;
      std::array<std::array<uint32_t, 2>, 2> counters{};
      ComPtr<ID3D12QueryHeap> so_queries, pipeline_queries;
      ComPtr<ID3D12Resource> query_readback;
      if (domain_so || gpu_mutation) {
        query_readback = buffer(c.device.Get(), 128, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 1, 0};
        if (domain_so) check(c.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&so_queries)), "domain-so-query-heap");
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        check(c.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&pipeline_queries)), "domain-so-pipeline-query-heap");
      }
      std::array<D3D12_QUERY_DATA_SO_STATISTICS, 2> so_stats{};
      std::array<D3D12_QUERY_DATA_PIPELINE_STATISTICS, 2> pipe_stats{};
      for (uint32_t indirect = 0; indirect < 2; ++indirect) {
        if (gpu_mutation && indirect) std::fprintf(stderr, "%s_GPU_MUTATION_PHASE shader=%s case=%s replay=0 begin\n", kPrefix, useDxil?"dxil":"dxbc", name.c_str());
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
        auto draw = [&](const Command &command) {
          const uint32_t instances = gpu_mutation ? 2u : command.draw.InstanceCount;
          if (nonindexed) c.list->DrawInstanced(command.draw.IndexCountPerInstance, instances,
                                               2, command.draw.StartInstanceLocation);
          else c.list->DrawIndexedInstanced(command.draw.IndexCountPerInstance, instances,
                                             command.draw.StartIndexLocation, command.draw.BaseVertexLocation,
                                             command.draw.StartInstanceLocation);
        };
        if (domain_so || gpu_mutation) {
          if (domain_so) c.list->BeginQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
          c.list->BeginQuery(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        }
        if (indirect) {
          // Poison initial state: both active commands must replace every binding.
          bind(slots[1]);
          if (dynamic_index) c.list->IASetIndexBuffer(nullptr);
          else if (gpu_mutation) c.list->IASetIndexBuffer(&slots[0].index);
          c.list->ExecuteIndirect(active_signature,5,args.Get(),16,args.Get(),768);
          // Whole-root mutation suppresses the draw. Partial mutation preserves enabled and resets only tag,
          // so the GPU variants must draw the same RGB with alpha zero.
          c.list->SetGraphicsRootConstantBufferView(1,slots[0].cbv); c.list->SetGraphicsRootShaderResourceView(2,slots[0].srv); c.list->SetGraphicsRootUnorderedAccessView(3,slots[0].uav);
          c.list->IASetVertexBuffers(0,1,&slots[0].vertex); c.list->IASetIndexBuffer(&slots[0].index);
          draw(slots[0]);
          c.list->SetPredication(predicate.Get(),0,D3D12_PREDICATION_OP_EQUAL_ZERO);
          c.list->ExecuteIndirect(active_signature,gpu_mutation?2:5,args.Get(),gpu_mutation?16+3*command_stride:16,
                                  gpu_mutation?nullptr:args.Get(),gpu_mutation?0:768);
          c.list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
          if (gpu_mutation) c.list->ExecuteIndirect(active_signature,2,args.Get(),16+3*command_stride,args.Get(),772);
        } else {
          for (const auto &slot : slots) { bind(slot); draw(slot); }
          if (gpu_mutation) { bind(slots[0]); c.list->SetGraphicsRoot32BitConstant(0,0,1); draw(slots[0]); }
        }
        if (domain_so || gpu_mutation) {
          if (domain_so) {
            c.list->EndQuery(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
            c.list->ResolveQueryData(so_queries.Get(), D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, query_readback.Get(), 0);
          }
          c.list->EndQuery(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
          c.list->ResolveQueryData(pipeline_queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, query_readback.Get(), 16);
        }
        if (stream_output) { c.list->SOSetTargets(0,0,nullptr); auto barrier=transition(so.Get(),D3D12_RESOURCE_STATE_STREAM_OUT,D3D12_RESOURCE_STATE_COPY_SOURCE); c.list->ResourceBarrier(1,&barrier); c.list->CopyBufferRegion(so_readback.Get(),0,so.Get(),0,4096); barrier=transition(so.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST); c.list->ResourceBarrier(1,&barrier); }
        for (uint32_t i=0;i<2;i++) { auto barrier=transition(uav[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE); c.list->ResourceBarrier(1,&barrier); c.list->CopyBufferRegion(uav_readback[i].Get(),0,uav[i].Get(),0,64); barrier=transition(uav[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST); c.list->ResourceBarrier(1,&barrier); }
        auto barrier=transition(c.target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE); c.list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=c.readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,32,32,1,256}; D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=c.target.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; c.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier=transition(c.target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET); c.list->ResourceBarrier(1,&barrier);
        barrier=transition(args.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_COPY_DEST); c.list->ResourceBarrier(1,&barrier); c.submit();
        if (gpu_mutation && indirect) std::fprintf(stderr, "%s_GPU_MUTATION_PHASE shader=%s case=%s replay=0 end\n", kPrefix, useDxil?"dxil":"dxbc", name.c_str());
        image[indirect]=read(c.readback.Get(),8192); if(stream_output) so_bytes[indirect]=read(so_readback.Get(),4096);
        for(uint32_t i=0;i<2;i++) { const auto bytes=read(uav_readback[i].Get(),64); std::memcpy(&counters[indirect][i],bytes.data()+20,4); }
        if (domain_so || gpu_mutation) {
          const auto bytes = read(query_readback.Get(), 128);
          std::memcpy(&so_stats[indirect], bytes.data(), sizeof(so_stats[indirect]));
          std::memcpy(&pipe_stats[indirect], bytes.data()+16, sizeof(pipe_stats[indirect]));
        }
      }
      std::printf("%s_MUTATION shader=%s case=%s direct=%u,%u indirect=%u,%u\n",kPrefix,useDxil?"dxil":"dxbc",name.c_str(),counters[0][0],counters[0][1],counters[1][0],counters[1][1]);
      require(image[0]==image[1],name.c_str()); require(counters[0]==counters[1],"mutation-uav-slots");
      const uint32_t invocations=tessellation?3:kind==1?2:1; require(counters[1][0]==invocations*(gpu_mutation?4u:1u) && counters[1][1]==(!tessellation && kind==1 ? 5u : invocations*(gpu_mutation?2u:1u)),"mutation-uav-exact");
      bool colors[2]{}; for(size_t y=0;y<32;y++) for(size_t x=0;x<32;x++) { uint32_t pixel; std::memcpy(&pixel,image[1].data()+y*256+x*4,4); colors[0]|=pixel==(gpu_mutation?0x00bfbf40u:0x40bfbf40u); colors[1]|=pixel==0x804040bf; if (no_raster) require(pixel == 0, "domain-so-no-raster-image"); }
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
      if (gpu_mutation) {
        require(std::memcmp(&pipe_stats[0], &pipe_stats[1], sizeof(pipe_stats[0])) == 0, "gpu-mutation-statistics");
        // Replay the same closed list with a smaller GPU count and a changed root tag/VB. The list's copy
        // produces arguments in the same submission; every consumer must observe this execution's values.
        commands[1].constants[1] = 32; commands[1].vertex = slots[1].vertex;
        // Created after Close, never bound directly: residency must come from
        // this execution's VA-owner snapshot, not a recording-time cache.
        ComPtr<ID3D12Resource> replay_ib;
        if (dynamic_index) {
          replay_ib = buffer(c.device.Get(), 128, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
          const uint32_t replay_indices[2] = {0, 3};
          put(replay_ib.Get(), replay_indices, sizeof(replay_indices), 16);
          commands[1].index = {replay_ib->GetGPUVirtualAddress() + 16, 8, DXGI_FORMAT_R32_UINT};
          commands[1].draw.BaseVertexLocation = -1;
        }
        upload_commands(2);
        std::fprintf(stderr, "%s_GPU_MUTATION_PHASE shader=%s case=%s replay=1 begin\n", kPrefix, useDxil?"dxil":"dxbc", name.c_str());
        ID3D12CommandList *lists[] = {c.list.Get()}; c.queue->ExecuteCommandLists(1, lists);
        const uint64_t value = ++c.fenceValue; check(c.queue->Signal(c.fence.Get(), value), "gpu-mutation-replay-signal");
        if (c.fence->GetCompletedValue() < value) {
          check(c.fence->SetEventOnCompletion(value, c.event), "gpu-mutation-replay-event");
          require(WaitForSingleObject(c.event, 60000) == WAIT_OBJECT_0, "gpu-mutation-replay-wait");
        }
        check(c.device->GetDeviceRemovedReason(), "gpu-mutation-replay-device");
        std::fprintf(stderr, "%s_GPU_MUTATION_PHASE shader=%s case=%s replay=1 end\n", kPrefix, useDxil?"dxil":"dxbc", name.c_str());
        auto expected = image[0];
        for (size_t y=0;y<32;y++) for (size_t x=0;x<32;x++) {
          auto *at = expected.data()+y*256+x*4; uint32_t pixel; std::memcpy(&pixel,at,4);
          if (pixel == 0x804040bfu) { pixel=0x20bfbf40u; std::memcpy(at,&pixel,4); }
        }
        require(read(c.readback.Get(),8192) == expected, "gpu-mutation-replay-image");
        for (uint32_t i=0;i<2;i++) {
          const auto bytes=read(uav_readback[i].Get(),64); uint32_t count; std::memcpy(&count,bytes.data()+20,4);
          require(count == (i?0:4*invocations), "gpu-mutation-replay-uav");
        }
        const auto bytes=read(query_readback.Get(),128); D3D12_QUERY_DATA_PIPELINE_STATISTICS replay{};
        std::memcpy(&replay,bytes.data()+16,sizeof(replay));
        require(replay.IAVertices == 4 && replay.IAPrimitives == 4 &&
                (tessellation ? replay.HSInvocations == 4 && replay.DSInvocations == 12 && !replay.GSInvocations
                              : replay.GSInvocations == 4), "gpu-mutation-replay-statistics");
        if (dynamic_index) {
          const D3D12_INDEX_BUFFER_VIEW valid_view = commands[1].index;
          const char *bounds_cases[] = {"null", "empty-view", "view-oob", "partial-u32", "byte-wrap", "u16-max", "u32-max"};
          for (uint32_t probe = 0; probe < 7; ++probe) {
            commands[1].index = valid_view;
            commands[1].draw.StartIndexLocation = 1;
            commands[1].draw.BaseVertexLocation = 2;
            if (probe == 0) commands[1].index = {0, 0, DXGI_FORMAT_UNKNOWN};
            if (probe == 1) commands[1].index.SizeInBytes = 0;
            if (probe == 2) commands[1].index.SizeInBytes = 4;
            if (probe == 3) commands[1].index.SizeInBytes = 7;
            if (probe == 4) {
              commands[1].draw.StartIndexLocation = 0x40000001u;
              commands[1].draw.BaseVertexLocation = -1;
            }
            if (probe == 5) {
              const uint16_t indices[2] = {0, 0xffffu};
              put(replay_ib.Get(), indices, sizeof(indices), 16);
              commands[1].index.SizeInBytes = 4;
              commands[1].index.Format = DXGI_FORMAT_R16_UINT;
              commands[1].draw.BaseVertexLocation = 2 - 65535;
            }
            if (probe == 6) {
              const uint32_t indices[2] = {0, UINT_MAX};
              put(replay_ib.Get(), indices, sizeof(indices), 16);
              commands[1].draw.BaseVertexLocation = 3;
            }
            upload_commands(2);
            std::fprintf(stderr, "%s_GPU_INDEX_VIEW_PHASE shader=%s case=%s probe=%s begin\n",
                kPrefix, useDxil?"dxil":"dxbc", name.c_str(), bounds_cases[probe]);
            c.queue->ExecuteCommandLists(1, lists);
            const uint64_t probe_fence = ++c.fenceValue;
            check(c.queue->Signal(c.fence.Get(), probe_fence), "gpu-index-view-signal");
            if (c.fence->GetCompletedValue() < probe_fence) {
              check(c.fence->SetEventOnCompletion(probe_fence, c.event), "gpu-index-view-event");
              require(WaitForSingleObject(c.event, 60000) == WAIT_OBJECT_0, "gpu-index-view-wait");
            }
            check(c.device->GetDeviceRemovedReason(), "gpu-index-view-device");
            require(read(c.readback.Get(),8192) == expected, "gpu-index-view-image");
            for (uint32_t i = 0; i < 2; ++i) {
              const auto result = read(uav_readback[i].Get(), 64); uint32_t count;
              std::memcpy(&count, result.data()+20, 4);
              require(count == (i ? 0 : 4*invocations), "gpu-index-view-uav");
            }
            const auto result = read(query_readback.Get(), 128);
            require(!std::memcmp(result.data()+16, &replay, sizeof(replay)), "gpu-index-view-statistics");
            std::fprintf(stderr, "%s_GPU_INDEX_VIEW_PHASE shader=%s case=%s probe=%s end\n",
                kPrefix, useDxil?"dxil":"dxbc", name.c_str(), bounds_cases[probe]);
          }
          std::printf("%s_GPU_INDEX_VIEW_PASS shader=%s case=%s probes=7 null=1 bounds=3 byte_wrap=1 max_indices=2 new_owner=1 negative_base=1 raster=exact\n",
              kPrefix, useDxil?"dxil":"dxbc", name.c_str());
        }
        std::printf("%s_GPU_MUTATION_PASS shader=%s case=%s executions=2 instances=2 partial_constant=1 fixed_index=%u dynamic_index=%u count=3,2 root_reset=1 toxic_skip=1 raster=exact\n", kPrefix, useDxil?"dxil":"dxbc", name.c_str(), nonindexed||dynamic_index?0u:1u, dynamic_index?1u:0u);
      }
      if (capture_bounds)
        std::printf("%s_CAPTURE_INDEX_VIEW_PASS shader=%s case=%s null=1 view_oob=1 raster=exact so=%u\n",
            kPrefix, useDxil?"dxil":"dxbc", name.c_str(), stream_output?1u:0u);
      if (capture_bounds && !tessellation && kind == 1) {
        // The R32 command does not match the PSO's 0xffff cut. Its maximum
        // index must remain an ordinary index after expansion to a uint32 list.
        auto maximum_ib = buffer(c.device.Get(), 128, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint32_t maximum_indices[16]; std::fill(std::begin(maximum_indices), std::end(maximum_indices), UINT_MAX);
        put(maximum_ib.Get(), maximum_indices, sizeof(maximum_indices), 16);
        commands[2].index = {maximum_ib->GetGPUVirtualAddress() + 16, 64, DXGI_FORMAT_R32_UINT};
        commands[2].draw.BaseVertexLocation = 3; // 0xffffffff + 3 wraps to vertex 2
        upload_commands(3);
        ID3D12CommandList *lists[] = {c.list.Get()}; c.queue->ExecuteCommandLists(1, lists);
        const uint64_t maximum_fence = ++c.fenceValue;
        check(c.queue->Signal(c.fence.Get(), maximum_fence), "gpu-index-strip-max-signal");
        if (c.fence->GetCompletedValue() < maximum_fence) {
          check(c.fence->SetEventOnCompletion(maximum_fence, c.event), "gpu-index-strip-max-event");
          require(WaitForSingleObject(c.event, 60000) == WAIT_OBJECT_0, "gpu-index-strip-max-wait");
        }
        check(c.device->GetDeviceRemovedReason(), "gpu-index-strip-max-device");
        require(read(c.readback.Get(), 8192) == image[0], "gpu-index-strip-max-image");
        if (stream_output)
          require(read(so_readback.Get(), 4096) == so_bytes[0], "gpu-index-strip-max-so");
        for (uint32_t i = 0; i < 2; ++i) {
          const auto bytes = read(uav_readback[i].Get(), 64); uint32_t count;
          std::memcpy(&count, bytes.data()+20, 4);
          require(count == counters[0][i], "gpu-index-strip-max-uav");
        }
        std::printf("%s_CAPTURE_INDEX_MAX_PASS shader=%s case=%s index=0xffffffff cut=0xffff ordinary=1 so=%u\n",
            kPrefix, useDxil?"dxil":"dxbc", name.c_str(), stream_output?1u:0u);
      }
      std::printf("%s_CASE_PASS shader=%s case=%s\n",kPrefix,useDxil?"dxil":"dxbc",name.c_str()); ++cases;
      }
      }
    }
  }
  return cases;
}
