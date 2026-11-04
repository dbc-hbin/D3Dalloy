// Public GPU observations for the captured MSAA tile continuation path.
// Fourteen overlapping primitives cover six 8x8 tiles each.  The 84 tasks
// cross a 64-task quantum, including partial right/bottom tiles.  Root UAV
// atomic32 side effects keep this on the general captured route.
void msaaTileTaskCase(Fixture &f) {
  constexpr UINT width=19, height=13, samples=4, primitives=14, sampleMask=5;
  constexpr UINT sampleValues=width*height*samples, guardWords=4;
  constexpr UINT counterWords=guardWords+sampleValues+guardWords;
  constexpr UINT outputWords=guardWords+sampleValues*5+guardWords;
  constexpr UINT counterBytes=counterWords*sizeof(UINT), outputBytes=outputWords*sizeof(UINT);
  constexpr UINT guard=0xdeadc0deu, activeSamples=(width-3)*(height-2)*2;
  constexpr float clear[4]{-2.f,-4.f,-8.f,.25f};
  constexpr char program[] = R"(
float4 tileVS(uint id : SV_VertexID) : SV_Position {
  id %= 3;
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, .375, 1);
}
RWStructuredBuffer<uint> counter : register(u1);
struct TileOutput { float4 color : SV_Target0; float depth : SV_Depth; };
TileOutput tilePS(float4 p : SV_Position, uint primitive : SV_PrimitiveID,
                  uint sample : SV_SampleIndex) {
  uint x=uint(p.x), y=uint(p.y), previous;
  InterlockedAdd(counter[4+(y*19+x)*4+sample],1,previous);
  TileOutput o;
  o.color=float4((primitive+1)/32.0,(sample+1)/16.0,(y*19+x+1)/1024.0,.5);
  o.depth=.25+(primitive+1)/64.0+sample/256.0;
  return o;
}
Texture2DMS<float4> colorSource : register(t0);
Texture2DMS<float> depthSource : register(t1);
RWByteAddressBuffer gathered : register(u1);
[numthreads(4,4,1)] void tileGather(uint3 p : SV_DispatchThreadID) {
  if(p.x>=19 || p.y>=13) return;
  for(uint sample=0;sample<4;sample++) {
    uint address=16+((p.y*19+p.x)*4+sample)*20;
    gathered.Store4(address,asuint(colorSource.Load(p.xy,sample)));
    gathered.Store(address+16,asuint(depthSource.Load(p.xy,sample)));
  }
}
)";
  D3D12_ROOT_PARAMETER drawParameter{};
  drawParameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  drawParameter.Descriptor.ShaderRegister=1;
  drawParameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC drawDesc{};
  drawDesc.NumParameters=1; drawDesc.pParameters=&drawParameter;
  ComPtr<ID3DBlob> serialized;
  check(D3D12SerializeRootSignature(&drawDesc,D3D_ROOT_SIGNATURE_VERSION_1,
                                   &serialized,nullptr),"tile draw root serialize");
  ComPtr<ID3D12RootSignature> drawRoot;
  check(f.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
                                     IID_PPV_ARGS(&drawRoot)),"tile draw root");
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=2;
  D3D12_ROOT_PARAMETER gatherParameters[2]{};
  gatherParameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  gatherParameters[0].DescriptorTable={1,&range};
  gatherParameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  gatherParameters[1].Descriptor.ShaderRegister=1;
  D3D12_ROOT_SIGNATURE_DESC gatherDesc{};
  gatherDesc.NumParameters=2; gatherDesc.pParameters=gatherParameters;
  check(D3D12SerializeRootSignature(&gatherDesc,D3D_ROOT_SIGNATURE_VERSION_1,
                                   &serialized,nullptr),"tile gather root serialize");
  ComPtr<ID3D12RootSignature> gatherRoot;
  check(f.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
                                     IID_PPV_ARGS(&gatherRoot)),"tile gather root");
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
  auto vs=compile(program,"tileVS","vs_5_0"), ps=compile(program,"tilePS","ps_5_0");
  auto pd=graphicsDesc(f,color.view,samples,0,false);
  pd.pRootSignature=drawRoot.Get();
  pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
  pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
  pd.DSVFormat=depth.view; pd.DepthStencilState.DepthEnable=TRUE;
  pd.SampleMask=sampleMask;
  auto &blend=pd.BlendState.RenderTarget[0];
  blend.BlendEnable=TRUE; blend.SrcBlend=D3D12_BLEND_ONE;
  blend.DestBlend=D3D12_BLEND_SRC_ALPHA; blend.BlendOp=D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_ZERO;
  blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
  ComPtr<ID3D12PipelineState> drawPSO;
  check(f.device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&drawPSO)),"tile draw PSO");
  auto cs=compile(program,"tileGather","cs_5_0");
  D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{}; cpd.pRootSignature=gatherRoot.Get();
  cpd.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
  ComPtr<ID3D12PipelineState> gatherPSO;
  check(f.device->CreateComputePipelineState(&cpd,IID_PPV_ARGS(&gatherPSO)),"tile gather PSO");
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=2;
  hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> views;
  check(f.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"tile gather views");
  auto viewHandle=views->GetCPUDescriptorHandleForHeapStart();
  for(auto *surface : {&color,&depth}) {
    D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format=surface->depth ? DXGI_FORMAT_R32_FLOAT : surface->view;
    vd.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vd.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMS;
    f.device->CreateShaderResourceView(surface->image.Get(),&vd,viewHandle);
    viewHandle.ptr+=f.device->GetDescriptorHandleIncrementSize(hd.Type);
  }
  auto makeBuffer=[&](UINT bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
    auto desc=buffer(bytes); auto properties=heap(type);
    if(type==D3D12_HEAP_TYPE_DEFAULT) desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;
    check(f.device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,
                                            IID_PPV_ARGS(&resource)),"tile buffer");
    return resource;
  };
  auto counters=makeBuffer(counterBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
  auto counterInit=makeBuffer(counterBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto counterRead=makeBuffer(counterBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  auto output=makeBuffer(outputBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
  auto outputInit=makeBuffer(outputBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto outputRead=makeBuffer(outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_RANGE empty{0,0}; void *mapped=nullptr;
  check(counterInit->Map(0,&empty,&mapped),"tile counter init map");
  auto *words=static_cast<UINT *>(mapped);
  for(UINT i=0;i<counterWords;i++)
    words[i]=i<guardWords || i>=guardWords+sampleValues ? guard : 0;
  counterInit->Unmap(0,nullptr);
  check(outputInit->Map(0,&empty,&mapped),"tile output init map");
  words=static_cast<UINT *>(mapped);
  for(UINT i=0;i<outputWords;i++) words[i]=guard;
  outputInit->Unmap(0,nullptr);

  // Initial copies, observation, and state restoration are part of the same
  // recording: replay cannot accidentally reuse an earlier result/counter.
  GPUList commands(f); auto *list=commands.list.Get();
  list->CopyBufferRegion(counters.Get(),0,counterInit.Get(),0,counterBytes);
  transition(list,counters.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->CopyBufferRegion(output.Get(),0,outputInit.Get(),0,outputBytes);
  transition(list,output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->ClearRenderTargetView(color.target,clear,0,nullptr);
  list->ClearDepthStencilView(depth.target,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
  list->OMSetRenderTargets(1,&color.target,FALSE,&depth.target);
  list->SetGraphicsRootSignature(drawRoot.Get()); list->SetPipelineState(drawPSO.Get());
  list->SetGraphicsRootUnorderedAccessView(0,counters->GetGPUVirtualAddress());
  D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
  D3D12_RECT scissor{3,2,LONG(width),LONG(height)};
  list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(primitives*3,1,0,0);
  transition(list,counters.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(counterRead.Get(),0,counters.Get(),0,counterBytes);
  transition(list,counters.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  color.move(list,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  depth.move(list,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->SetComputeRootSignature(gatherRoot.Get()); list->SetPipelineState(gatherPSO.Get());
  ID3D12DescriptorHeap *heaps[]{views.Get()}; list->SetDescriptorHeaps(1,heaps);
  list->SetComputeRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
  list->Dispatch((width+3)/4,(height+3)/4,1);
  transition(list,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(outputRead.Get(),0,output.Get(),0,outputBytes);
  transition(list,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  color.move(list,D3D12_RESOURCE_STATE_RENDER_TARGET);
  depth.move(list,D3D12_RESOURCE_STATE_DEPTH_WRITE);
  check(list->Close(),"tile closed recording");
  for(UINT replay=0;replay<2;replay++) {
    f.submit(list,replay ? "msaa_tile_tasks_replay" : "msaa_tile_tasks_first",true);
    auto exact=[&](const char *what,UINT index,UINT actual,UINT expected) {
      if(actual!=expected) {
        std::fprintf(stderr,"%s_TILE_TASKS_MISMATCH replay=%u kind=%s index=%u actual=%08x expected=%08x\n",
                     marker,replay,what,index,actual,expected);
        fail("tile exact GPU oracle");
      }
    };
    D3D12_RANGE counterRange{0,counterBytes}, outputRange{0,outputBytes};
    check(counterRead->Map(0,&counterRange,&mapped),"tile counter read map");
    const auto *counts=static_cast<const UINT *>(mapped);
    for(UINT i=0;i<guardWords;i++) {
      exact("counter_head",i,counts[i],guard);
      exact("counter_tail",i,counts[guardWords+sampleValues+i],guard);
    }
    UINT invocations=0;
    for(UINT y=0;y<height;y++) for(UINT x=0;x<width;x++) for(UINT sample=0;sample<samples;sample++) {
      const UINT index=(y*width+x)*samples+sample;
      const bool active=x>=3 && y>=2 && ((sampleMask>>sample)&1);
      exact("counter",index,counts[guardWords+index],active ? primitives : 0);
      invocations+=counts[guardWords+index];
    }
    counterRead->Unmap(0,&empty);
    exact("invocations",0,invocations,activeSamples*primitives);
    check(outputRead->Map(0,&outputRange,&mapped),"tile output read map");
    const auto *values=static_cast<const UINT *>(mapped);
    for(UINT i=0;i<guardWords;i++) {
      exact("output_head",i,values[i],guard);
      exact("output_tail",i,values[guardWords+sampleValues*5+i],guard);
    }
    for(UINT y=0;y<height;y++) for(UINT x=0;x<width;x++) for(UINT sample=0;sample<samples;sample++) {
      const UINT index=(y*width+x)*samples+sample;
      const bool active=x>=3 && y>=2 && ((sampleMask>>sample)&1);
      float wanted[5]{clear[0],clear[1],clear[2],clear[3],1.f};
      if(active) {
        // Dyadic inputs remain exactly representable through all 14 ordered
        // src + .5*dst steps, so no floating-point tolerance hides reordering.
        for(UINT primitive=0;primitive<primitives;primitive++) {
          wanted[0]=(primitive+1)/32.f+.5f*wanted[0];
          wanted[1]=(sample+1)/16.f+.5f*wanted[1];
          wanted[2]=(y*width+x+1)/1024.f+.5f*wanted[2];
        }
        wanted[3]=.5f; wanted[4]=.25f+primitives/64.f+sample/256.f;
      }
      for(UINT component=0;component<5;component++)
        exact(component==4 ? "depth" : "color",index*5+component,
              values[guardWords+index*5+component],std::bit_cast<UINT>(wanted[component]));
    }
    outputRead->Unmap(0,&empty);
    std::printf("%s_TILE_TASKS_PASS replay=%u width=%u height=%u samples=%u mask=%08x primitives=%u expected_tiles=84 pixels=%u sample_values=%u ps_invocations=%u guard_pixels=71 counter_guards=8 output_guards=8 ordered_blend=1 exact_depth=1 same_recording=1\n",
                marker,replay,width,height,samples,sampleMask,primitives,width*height,sampleValues,invocations);
  }
  GPUList clipped(f);
  list=clipped.list.Get();
  list->CopyBufferRegion(counters.Get(),0,counterInit.Get(),0,counterBytes);
  transition(list,counters.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->OMSetRenderTargets(1,&color.target,FALSE,&depth.target);
  list->SetGraphicsRootSignature(drawRoot.Get()); list->SetPipelineState(drawPSO.Get());
  list->SetGraphicsRootUnorderedAccessView(0,counters->GetGPUVirtualAddress());
  const D3D12_RECT outside{LONG(width),0,LONG(width+4),LONG(height)};
  list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&outside);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3,1,0,0);
  transition(list,counters.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(counterRead.Get(),0,counters.Get(),0,counterBytes);
  check(list->Close(),"clipped tile close");
  f.submit(list,"msaa_tile_tasks_clipped",true);
  D3D12_RANGE readRange{0,counterBytes};
  check(counterRead->Map(0,&readRange,&mapped),"clipped tile counters");
  for (UINT i=0;i<counterWords;++i)
    if (static_cast<const UINT *>(mapped)[i]!=(i<guardWords || i>=guardWords+sampleValues ? guard : 0u))
      fail("fully clipped tile invoked pixel shader or touched guards");
  counterRead->Unmap(0,&empty);

}
