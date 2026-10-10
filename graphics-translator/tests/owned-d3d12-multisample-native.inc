
// Distinct committed CBV owners, one fixed OM binding, and observable draws past the pass budget.
void nativeBindingBudgetCase(Fixture &f) {
  constexpr UINT width=68, height=4, samples=2, owners=65, draws=66;
  constexpr char program[] = R"(
cbuffer Args : register(b0) { float4 bias; };
struct Output { float4 color : SV_Target0; float depth : SV_Depth; };
Output ordinaryCBV() { Output o; o.color=bias; o.depth=bias.x+.125; return o; }
Texture2DMS<float4> colorSource : register(t0);
Texture2DMS<float> depthSource : register(t0);
RWStructuredBuffer<float4> output : register(u0);
[numthreads(4,4,1)] void gatherColor(uint3 p : SV_DispatchThreadID) {
  for(uint s=0;s<2;s++) output[(p.y*68+p.x)*2+s]=colorSource.Load(p.xy,s);
}
[numthreads(4,4,1)] void gatherDepth(uint3 p : SV_DispatchThreadID) {
  for(uint s=0;s<2;s++) output[(p.y*68+p.x)*2+s]=float4(depthSource.Load(p.xy,s),0,0,0);
}
)";
  D3D12_ROOT_PARAMETER1 param{};
  param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
  param.Descriptor.Flags=D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
  param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rd{}; rd.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;
  rd.Desc_1_1.NumParameters=1; rd.Desc_1_1.pParameters=&param;
  ComPtr<ID3DBlob> serialized;
  check(D3D12SerializeVersionedRootSignature(&rd,&serialized,nullptr),"budget root serialize");
  ComPtr<ID3D12RootSignature> root;
  check(f.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
                                     IID_PPV_ARGS(&root)),"budget root");
  GPUSurface color(f,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,samples,false);
  GPUSurface depth(f,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT,samples,true);
  for(auto *surface : {&color,&depth}) {
    auto desc=surface->image->GetDesc(); desc.Width=width; desc.Height=height;
    surface->image=validTexture(f.device.Get(),desc,surface->state);
    if(surface->depth) {
      D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=surface->view;
      view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DMS;
      f.device->CreateDepthStencilView(surface->image.Get(),&view,surface->target);
    } else {
      D3D12_RENDER_TARGET_VIEW_DESC view{}; view.Format=surface->view;
      view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DMS;
      f.device->CreateRenderTargetView(surface->image.Get(),&view,surface->target);
    }
  }
  auto ps=compile(program,"ordinaryCBV","ps_5_0");
  auto pd=graphicsDesc(f,color.view,samples,0,false); pd.pRootSignature=root.Get();
  pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()}; pd.DSVFormat=depth.view;
  pd.DepthStencilState.DepthEnable=TRUE; pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
  ComPtr<ID3D12PipelineState> pso;
  check(f.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"budget native PSO");
  std::array<ComPtr<ID3D12Resource>,owners> constants;
  auto uploadHeap=heap(D3D12_HEAP_TYPE_UPLOAD); auto cbDesc=buffer(256);
  D3D12_RANGE empty{0,0};
  for(UINT i=0;i<owners;i++) {
    check(f.device->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&constants[i])),"budget distinct CBV");
    void *mapped=nullptr; check(constants[i]->Map(0,&empty,&mapped),"budget CBV map");
    const float bias[4]{(i+1)/256.f,2*(i+1)/256.f,3*(i+1)/256.f,4*(i+1)/256.f};
    std::memcpy(mapped,bias,sizeof(bias)); constants[i]->Unmap(0,nullptr);
  }
  std::fprintf(stderr,"%s_NATIVE_BINDING_BUDGET_RECORDING_BEGIN draws=66 committed_cbvs=65 repeated_bindings=1 om_binds=1\n",marker);
  GPUList commands(f);
  const float clear[4]{}; commands.list->ClearRenderTargetView(color.target,clear,0,nullptr);
  commands.list->ClearDepthStencilView(depth.target,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
  commands.list->OMSetRenderTargets(1,&color.target,FALSE,&depth.target);
  commands.list->SetGraphicsRootSignature(root.Get()); commands.list->SetPipelineState(pso.Get());
  commands.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1}; commands.list->RSSetViewports(1,&viewport);
  for(UINT i=0;i<draws;i++) {
    D3D12_RECT rect{LONG(i+1),0,LONG(i+2),LONG(height)}; commands.list->RSSetScissorRects(1,&rect);
    commands.list->SetGraphicsRootConstantBufferView(0,constants[i<owners ? i : owners-1]->GetGPUVirtualAddress());
    commands.list->DrawInstanced(3,1,0,0);
  }
  check(commands.list->Close(),"budget close 65 distinct committed CBVs");
  for(auto &owner : constants) owner.Reset(); root.Reset(); pso.Reset();
  std::fprintf(stderr,"%s_NATIVE_BINDING_BUDGET_RECORDING_CLOSED draws=66 committed_cbvs=65 repeated_bindings=1 owners_released=1\n",marker);
  // Re-submit the same closed recording after readback to prove retained replay ownership.
  for(UINT replay=0;replay<2;replay++) {
    f.submit(commands.list.Get(),replay ? "native_binding_budget_replay" : "native_binding_budget_first",true);
    for(auto *surface : {&color,&depth}) {
      GPUList gather(f); surface->move(gather.list.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      hd.NumDescriptors=1; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      ComPtr<ID3D12DescriptorHeap> views;
      check(f.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"budget gather heap");
      D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=surface->depth ? DXGI_FORMAT_R32_FLOAT : surface->view;
      sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
      f.device->CreateShaderResourceView(surface->image.Get(),&sd,views->GetCPUDescriptorHandleForHeapStart());
      auto code=compile(program,surface->depth ? "gatherDepth" : "gatherColor","cs_5_0");
      D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{}; cpd.pRootSignature=f.readRoot.Get();
      cpd.CS={code->GetBufferPointer(),code->GetBufferSize()}; ComPtr<ID3D12PipelineState> cpso;
      check(f.device->CreateComputePipelineState(&cpd,IID_PPV_ARGS(&cpso)),"budget gather PSO");
      constexpr UINT bytes=width*height*samples*sizeof(std::array<float,4>);
      auto bd=buffer(bytes); bd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      auto dh=heap(D3D12_HEAP_TYPE_DEFAULT), rh=heap(D3D12_HEAP_TYPE_READBACK);
      ComPtr<ID3D12Resource> output,readback;
      check(f.device->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&bd,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)),"budget gather output");
      bd.Flags=D3D12_RESOURCE_FLAG_NONE;
      check(f.device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,
          D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"budget gather readback");
      gather.list->SetPipelineState(cpso.Get()); gather.list->SetComputeRootSignature(f.readRoot.Get());
      ID3D12DescriptorHeap *heaps[]{views.Get()}; gather.list->SetDescriptorHeaps(1,heaps);
      gather.list->SetComputeRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
      gather.list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
      gather.list->Dispatch(width/4,1,1);
      transition(gather.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
      gather.list->CopyResource(readback.Get(),output.Get());
      surface->move(gather.list.Get(),surface->depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
      f.submit(gather.list.Get(),surface->depth ? "native_binding_budget_depth_observe" : "native_binding_budget_color_observe");
      void *mapped=nullptr; D3D12_RANGE range{0,bytes};
      check(readback->Map(0,&range,&mapped),"budget result map");
      const auto *values=static_cast<const std::array<float,4> *>(mapped);
      for(UINT y=0;y<height;y++) for(UINT x=0;x<width;x++) for(UINT s=0;s<samples;s++) {
        const bool written=x>=1 && x<=draws;
        for(UINT c=0;c<(surface->depth ? 1u : 4u);c++) {
          const UINT value=x<=owners ? x : owners;
          const float expected=surface->depth ? written ? value/256.f+.125f : 1.f : written ? (c+1)*value/256.f : 0.f;
          require(std::bit_cast<UINT>(values[(y*width+x)*samples+s][c])==std::bit_cast<UINT>(expected),
                  surface->depth ? "budget exact depth and guards" : "budget exact color and guards");
        }
      }
      readback->Unmap(0,&empty);
    }
    std::printf("%s_NATIVE_BINDING_BUDGET_PASS replay=%u draws=66 committed_cbvs=65 repeated_bindings=1 om_binds=1 count=2 quality=0 pixels=272 channels=4 depth=1 guard_pixels=8 owners_released=1 route=native_static\n",marker,replay);
  }
}

// Public ordinary VS/PS work, exact per-sample color/depth, and mixed-route recording.
constexpr char nativeOrdinaryShaders[] = R"(
struct Output { float4 color : SV_Target0; float depth : SV_Depth; };
Output ordinary() {
  Output o; o.color=float4(.0625,.25,.5,1); o.depth=.375; return o;
}
Output sampleFallback(uint s : SV_SampleIndex) {
  Output o; o.color=float4((s+1)/16.0,.25,.5,1); o.depth=.375+s/32.0; return o;
}
)";

void nativeOrdinaryCases(Fixture &f) {
  auto ordinary=compile(nativeOrdinaryShaders,"ordinary","ps_5_0");
  auto sample=compile(nativeOrdinaryShaders,"sampleFallback","ps_5_0");
  const auto coverage=readFile(L"native-msaa-coverage.dxil");
  for(UINT count : {2u,4u,8u}) for(UINT quality : {0u,standardPattern,centerPattern}) {
    for(UINT mask : {UINT_MAX,0x55555555u,0u}) {
      GPUSurface color(f,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,count,false,true,quality);
      GPUSurface depth(f,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT,count,true,true,quality);
      auto pd=graphicsDesc(f,color.view,count,quality,false);
      auto &vs=quality==centerPattern ? f.vsCenterEdge : f.vsEdge;
      pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
      pd.PS={ordinary->GetBufferPointer(),ordinary->GetBufferSize()};
      pd.SampleMask=mask; pd.DSVFormat=depth.view;
      pd.DepthStencilState.DepthEnable=TRUE;
      pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
      pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
      ComPtr<ID3D12PipelineState> pso;
      check(f.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"native ordinary PSO");
      GPUList commands(f);
      clearSurface(commands.list.Get(),color);
      clearSurface(commands.list.Get(),depth,4,1);
      bindDraw(f,commands.list.Get(),color,pso.Get());
      commands.list->OMSetRenderTargets(1,&color.target,FALSE,&depth.target);
      commands.list->DrawInstanced(6,1,0,0);
      char label[128];
      std::snprintf(label,sizeof(label),"native_ordinary_count%u_quality%u_mask%08x",count,quality,mask);
      f.submit(commands.list.Get(),label);
      auto colors=observe(f,color), depths=observe(f,depth);
      const UINT edge=quality==centerPattern ? (1u<<count)-1 : count==2 ? 2 : count==4 ? 5 : 0x3a;
      for(UINT y=0;y<4;y++) for(UINT x=0;x<4;x++) for(UINT s=0;s<count;s++) {
        const bool written=(mask&(1u<<s)) && (x==0 || (x==1 && (edge&(1u<<s))));
        const auto &c=colors[(y*4+x)*count+s];
        const float expected[4]{.0625f,.25f,.5f,1.f};
        for(UINT channel=0;channel<4;channel++)
          require(std::bit_cast<UINT>(c[channel])==std::bit_cast<UINT>(written ? expected[channel] : 0.f),
                  "ordinary exact color sample");
        require(std::bit_cast<UINT>(depths[(y*4+x)*count+s][0])==
                std::bit_cast<UINT>(written ? .375f : 1.f),"ordinary exact depth sample");
      }
      std::printf("%s_NATIVE_ORDINARY_PASS count=%u quality=%u mask=%08x edge=%u pixels=16 channels=4 depth=1\n",
                  marker,count,quality,mask,edge&mask);
    }
    // One immutable recording switches native -> sample-frequency -> native -> coverage-input.
    // Each row is independently observable; a stale PSO or constants fails the exact oracle.
    GPUSurface color(f,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,count,false,true,quality);
    GPUSurface depth(f,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT,count,true,true,quality);
    ComPtr<ID3D12PipelineState> psos[3];
    for(UINT i=0;i<3;i++) {
      // Sample-frequency center is invalid, so use coverage input for both fallback rows there.
      auto pd=graphicsDesc(f,color.view,count,quality,false);
      if(i==0 || (i==1 && quality!=centerPattern)) {
        auto &ps=i==0 ? ordinary : sample;
        pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
      } else pd.PS={coverage.data(),coverage.size()};
      pd.DSVFormat=depth.view; pd.DepthStencilState.DepthEnable=TRUE;
      pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
      pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
      check(f.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&psos[i])),"native fallback transition PSO");
    }
    GPUList commands(f);
    clearSurface(commands.list.Get(),color); clearSurface(commands.list.Get(),depth,4,1);
    auto indexDesc=buffer(3*sizeof(UINT16)); auto upload=heap(D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> indices;
    check(f.device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&indexDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&indices)),"transition index buffer");
    void *mapped=nullptr; D3D12_RANGE empty{0,0};
    check(indices->Map(0,&empty,&mapped),"transition index map");
    const UINT16 indexValues[3]{0,1,2}; std::memcpy(mapped,indexValues,sizeof(indexValues));
    indices->Unmap(0,nullptr);
    D3D12_INDEX_BUFFER_VIEW indexView{indices->GetGPUVirtualAddress(),sizeof(indexValues),DXGI_FORMAT_R16_UINT};
    commands.list->IASetIndexBuffer(&indexView);
    for(UINT row=0;row<4;row++) {
      bindDraw(f,commands.list.Get(),color,psos[row==1 ? 1 : row==3 ? 2 : 0].Get(),row);
      commands.list->OMSetRenderTargets(1,&color.target,FALSE,&depth.target);
      if(row==0 || row==2) {
        if(row==2) commands.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        commands.list->DrawIndexedInstanced(3,1,0,0,0);
      } else commands.list->DrawInstanced(3,1,0,0);
    }
    char label[96]; std::snprintf(label,sizeof(label),"native_fallback_transitions_count%u_quality%u",count,quality);
    f.submit(commands.list.Get(),label);
    auto colors=observe(f,color), depths=observe(f,depth);
    for(UINT row=0;row<4;row++) for(UINT x=0;x<4;x++) for(UINT s=0;s<count;s++) {
      const bool frequency=row==1 && quality!=centerPattern;
      const float expected[4]{frequency ? (s+1)/16.f : .0625f,.25f,.5f,1.f};
      for(UINT channel=0;channel<4;channel++)
        require(std::bit_cast<UINT>(colors[(row*4+x)*count+s][channel])==std::bit_cast<UINT>(expected[channel]),
                "transition exact color sample");
      require(std::bit_cast<UINT>(depths[(row*4+x)*count+s][0])==
              std::bit_cast<UINT>(frequency ? .375f+s/32.f : .375f),"transition exact depth sample");
    }
    std::printf("%s_NATIVE_FALLBACK_TRANSITIONS_PASS count=%u quality=%u draws=4 native_candidates=2 fallback=2 indexed=1 index_pull=1\n",
                marker,count,quality);
  }
}


constexpr char nativeResourceShaders[] = R"(
Texture2D<float4> source : register(t0);
Texture2DMS<float4> shadowSource : register(t1);
cbuffer Args : register(b0) { float4 bias; };
struct Output { float4 color : SV_Target0; float depth : SV_Depth; };
Output ordinaryResource(float4 p : SV_Position) {
  float4 texel=source.Load(int3(uint2(p.xy),0));
  Output o; o.color=texel+bias; o.depth=texel.x+bias.x+.125; return o;
}
Output shadowResource(float4 p : SV_Position) {
  float4 texel=source.Load(int3(uint2(p.xy),0));
  float4 shadow=shadowSource.Load(uint2(p.xy),0);
  Output o; o.color=texel+bias+shadow*.5; o.depth=texel.x+bias.x+.125+shadow.x*.5; return o;
}
)";

void nativeResourceCases(Fixture &f, bool staticDescriptors) {
  auto ordinary=compile(nativeResourceShaders,"ordinaryResource","ps_5_0");
  auto shadowPS=compile(nativeResourceShaders,"shadowResource","ps_5_0");
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=2;
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable={1,&range}; params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters=2; rd.pParameters=params;
  ComPtr<ID3DBlob> serialized; ComPtr<ID3D12RootSignature> root;
  if(staticDescriptors) {
    D3D12_DESCRIPTOR_RANGE1 staticRange{};
    staticRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; staticRange.NumDescriptors=2;
    staticRange.Flags=D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_ROOT_PARAMETER1 staticParams[2]{};
    staticParams[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    staticParams[0].DescriptorTable={1,&staticRange}; staticParams[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    staticParams[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    staticParams[1].Descriptor.Flags=D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    staticParams[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{}; versioned.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1.NumParameters=2; versioned.Desc_1_1.pParameters=staticParams;
    check(D3D12SerializeVersionedRootSignature(&versioned,&serialized,nullptr),"static resource root serialize");
  } else check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr),"volatile resource root serialize");
  check(f.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
                                     IID_PPV_ARGS(&root)),"resource root");
  auto td=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,1,0);
  auto input=validTexture(f.device.Get(),td,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT rows=0; UINT64 rowBytes=0,total=0;
  f.device->GetCopyableFootprints(&td,0,1,0,&footprint,&rows,&rowBytes,&total);
  auto uploadHeap=heap(D3D12_HEAP_TYPE_UPLOAD);
  auto uploadDesc=buffer(total); ComPtr<ID3D12Resource> upload,constants;
  check(f.device->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&uploadDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"resource texture upload");
  void *mapped=nullptr; D3D12_RANGE empty{0,0};
  check(upload->Map(0,&empty,&mapped),"resource texture map");
  for(UINT y=0;y<4;y++) for(UINT x=0;x<4;x++) {
    const float texel[4]{(x+1)/32.f,(y+1)/32.f,(x+y+1)/64.f,.5f};
    std::memcpy(static_cast<char *>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*sizeof(texel),
                texel,sizeof(texel));
  }
  upload->Unmap(0,nullptr);
  auto cbDesc=buffer(512);
  check(f.device->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&cbDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&constants)),"resource constant buffer");
  const float biases[2][4]{{.0625f,.125f,.25f,.5f},{.125f,.25f,.0625f,.25f}};
  check(constants->Map(0,&empty,&mapped),"resource constants map");
  std::memcpy(mapped,biases[0],sizeof(biases[0]));
  std::memcpy(static_cast<char *>(mapped)+256,biases[1],sizeof(biases[1]));
  constants->Unmap(0,nullptr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors=2; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> views;
  check(f.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"resource input heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=td.Format;
  sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
  f.device->CreateShaderResourceView(input.Get(),&sd,views->GetCPUDescriptorHandleForHeapStart());
  GPUSurface shadow(f,td.Format,td.Format,2,false);
  auto shadowView=views->GetCPUDescriptorHandleForHeapStart();
  shadowView.ptr+=f.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sd={}; sd.Format=td.Format; sd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
  f.device->CreateShaderResourceView(shadow.image.Get(),&sd,shadowView);
  GPUList initialize(f);
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};
  src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=footprint;
  dst.pResource=input.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  initialize.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  transition(initialize.list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  clearSurface(initialize.list.Get(),shadow,4,.125f);
  shadow.move(initialize.list.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  f.submit(initialize.list.Get(),staticDescriptors ? "native_static_resource_upload" : "native_resource_upload");
  initialize.list.Reset(); initialize.allocator.Reset();
  for(UINT count : {2u,4u,8u}) for(UINT quality : {0u,standardPattern,centerPattern}) {
    // Three ordinary masks plus one recording that preserves attachment contents across native/captured routes.
    for(UINT variant=0;variant<4;variant++) {
      const bool mixed=variant==3;
      const UINT mask=variant==1 ? 0x55555555u : variant==2 ? 0u : UINT_MAX;
      GPUSurface color(f,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,count,false,true,quality);
      GPUSurface depth(f,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_D32_FLOAT,count,true,true,quality);
      ComPtr<ID3D12PipelineState> psos[2];
      for(UINT i=0;i<(mixed ? 2u : 1u);i++) {
        auto pd=graphicsDesc(f,color.view,count,quality,false); pd.pRootSignature=root.Get();
        auto &vs=mixed ? f.vs : quality==centerPattern ? f.vsCenterEdge : f.vsEdge;
        auto &ps=i ? shadowPS : ordinary;
        pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pd.SampleMask=mask; pd.DSVFormat=depth.view; pd.DepthStencilState.DepthEnable=TRUE;
        pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        check(f.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&psos[i])),"resource native/fallback PSO");
      }
      GPUList commands(f); clearSurface(commands.list.Get(),color); clearSurface(commands.list.Get(),depth,4,1);
      ID3D12DescriptorHeap *heaps[]{views.Get()}; commands.list->SetDescriptorHeaps(1,heaps);
      for(UINT draw=0;draw<(mixed ? 4u : 1u);draw++) {
        bindDraw(f,commands.list.Get(),color,psos[mixed && draw==1 ? 1 : 0].Get(),mixed ? draw : 4);
        commands.list->SetGraphicsRootSignature(root.Get());
        commands.list->SetGraphicsRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
        const UINT biasIndex=mixed && (draw==1 || draw==2) ? 1 : 0;
        commands.list->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress()+biasIndex*256);
        commands.list->OMSetRenderTargets(1,&color.target,FALSE,&depth.target);
        commands.list->DrawInstanced(mixed ? 3 : 6,1,0,0);
      }
      char label[128]; std::snprintf(label,sizeof(label),"native_%sresource_%s_count%u_quality%u_mask%08x",
                                    staticDescriptors ? "static_" : "",mixed ? "transitions" : "inputs",count,quality,mask);
      check(commands.list->Close(),"resource closed recording");
      psos[0].Reset();
      // Release native used-input owners before replay; keep fallback-only shadow/PSO owners alive.
      const bool releaseOwners=mixed && count==8 && quality==centerPattern;
      if(releaseOwners) {
        views.Reset(); input.Reset(); constants.Reset(); upload.Reset(); root.Reset();
      }
      f.submit(commands.list.Get(),label,true);
      auto colors=observe(f,color), depths=observe(f,depth);
      const UINT edge=quality==centerPattern ? (1u<<count)-1 : count==2 ? 2 : count==4 ? 5 : 0x3a;
      for(UINT y=0;y<4;y++) for(UINT x=0;x<4;x++) for(UINT s=0;s<count;s++) {
        const bool written=(mask&(1u<<s)) && (mixed || x==0 || (x==1 && (edge&(1u<<s))));
        const UINT biasIndex=mixed && (y==1 || y==2) ? 1 : 0;
        const float shadowValue=mixed && y==1 ? .0625f : 0.f;
        const float texel[4]{(x+1)/32.f,(y+1)/32.f,(x+y+1)/64.f,.5f};
        for(UINT c=0;c<4;c++)
          require(std::bit_cast<UINT>(colors[(y*4+x)*count+s][c])==
                  std::bit_cast<UINT>(written ? texel[c]+biases[biasIndex][c]+shadowValue : 0.f),"resource exact color sample");
        require(std::bit_cast<UINT>(depths[(y*4+x)*count+s][0])==
                std::bit_cast<UINT>(written ? texel[0]+biases[biasIndex][0]+.125f+shadowValue : 1.f),"resource exact depth sample");
      }
      if(mixed)
        std::printf("%s_NATIVE_%sRESOURCE_TRANSITIONS_PASS count=%u quality=%u draws=4 native_candidates=%u fallback=%u srv=1 cbv=1 msaa_srv=1 owners_released=%u\n",
                    marker,staticDescriptors ? "STATIC_" : "",count,quality,staticDescriptors ? 3 : 0,
                    staticDescriptors ? 1 : 4,releaseOwners);
      else
        std::printf("%s_NATIVE_%sRESOURCE_INPUTS_PASS count=%u quality=%u mask=%08x edge=%u pixels=16 channels=4 depth=1 srv=1 cbv=1 route=%s\n",
                    marker,staticDescriptors ? "STATIC_" : "",count,quality,mask,edge&mask,
                    staticDescriptors ? "native_static" : "captured_volatile");
    }
  }
}
