// Public API proof for Close-time shape vs Execute-time values, sparse-negative
// admission, placed aliases, ICB retirement reuse and visibility fanout batches.
#define main retained_render_fixture_main
#include "owned-d3d12-render-passes.cpp"
#undef main

namespace {
constexpr char foundation[]="OWNED_D3D12_SUBMISSION_FOUNDATION";
void phase(const char* name,bool start) {
  std::fprintf(stderr,"%s_PHASE %s=%s\n",foundation,start?"begin":"end",name);std::fflush(stderr);
}
void exactWord(ID3D12Resource* resource,uint32_t expected) {
  void* mapped=nullptr;check(resource->Map(0,nullptr,&mapped),"foundation readback");
  const auto actual=*static_cast<const uint32_t*>(mapped);resource->Unmap(0,nullptr);
  if(actual!=expected)fail("foundation exact word");
}
void uploadWord(ID3D12Resource* resource,uint32_t value) {
  void* mapped=nullptr;check(resource->Map(0,nullptr,&mapped),"foundation upload");
  std::memcpy(mapped,&value,4);resource->Unmap(0,nullptr);
}
void volatileReplayAndICB() {
  Context c;
  ComPtr<ID3D12Resource> unrelatedSparse;auto sparseDesc=bufDesc(65536);
  check(c.dev->CreateReservedResource(&sparseDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,
      IID_PPV_ARGS(&unrelatedSparse)),"unrelated live reserved buffer");
  auto a=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(256),D3D12_RESOURCE_STATE_GENERIC_READ);
  auto b=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(256),D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output=committed(c.dev.Get(),D3D12_HEAP_TYPE_DEFAULT,bufDesc(256,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback=committed(c.dev.Get(),D3D12_HEAP_TYPE_READBACK,bufDesc(256),D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12DescriptorHeap> descriptors;auto cpu=descriptor(c.dev.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,descriptors,true);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_TYPELESS;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=64;srv.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
  c.dev->CreateShaderResourceView(a.Get(),&srv,cpu);
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[1].Descriptor.ShaderRegister=0;
  auto root=rootSignature(c.dev.Get(),{2,params});
  auto code=compile("ByteAddressBuffer Source:register(t0);RWByteAddressBuffer Dest:register(u0);[numthreads(1,1,1)]void cs(){Dest.Store(0,Source.Load(0));}","cs","cs_5_1");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline;check(c.dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"foundation compute");
  auto record=[&](ID3D12GraphicsCommandList* list,ID3D12CommandSignature* signature=nullptr,ID3D12Resource* arguments=nullptr) {
    ID3D12DescriptorHeap* heaps[]={descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetPipelineState(pipeline.Get());list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0,descriptors->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
    if(signature)list->ExecuteIndirect(signature,1,arguments,0,nullptr,0);else list->Dispatch(1,1,1);
    transition(list,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(),0,output.Get(),0,4);
    transition(list,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    check(list->Close(),"foundation close");
  };
  auto sealed=c.list();record(sealed.list.Get());
  phase("volatile_replay_sparse_unrelated",true);
  for(unsigned i=0;i<4;++i) {
    auto* selected=i%2?b.Get():a.Get();const uint32_t value=0x13570000u+i;
    uploadWord(selected,value);c.dev->CreateShaderResourceView(selected,&srv,cpu);
    c.executeSealed({sealed.list.Get()});exactWord(readback.Get(),value);
  }
  phase("volatile_replay_sparse_unrelated",false);
  std::printf("%s_CASE_PASS case=volatile_replay executions=4 close_count=1 latest_slot_and_value=exact unrelated_sparse=live\n",foundation);
  // Root updates force the actual native ICB resolver, not the already fast
  // one-dispatch-indirect shortcut, so retirement reuse is observed directly.
  D3D12_INDIRECT_ARGUMENT_DESC argumentsDesc[2]{};
  argumentsDesc[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
  argumentsDesc[0].UnorderedAccessView.RootParameterIndex=1;
  argumentsDesc[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC signatureDesc{24,2,argumentsDesc};ComPtr<ID3D12CommandSignature> signature;
  check(c.dev->CreateCommandSignature(&signatureDesc,root.Get(),IID_PPV_ARGS(&signature)),"foundation indirect signature");
  auto arguments=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(256),D3D12_RESOURCE_STATE_GENERIC_READ);
  void* mapped=nullptr;check(arguments->Map(0,nullptr,&mapped),"arguments map");
  const uint64_t address=output->GetGPUVirtualAddress();const uint32_t dispatch[]={1,1,1};
  std::memcpy(mapped,&address,8);std::memcpy(static_cast<char*>(mapped)+8,dispatch,12);arguments->Unmap(0,nullptr);
  auto indirect=c.list();phase("retired_icb_reuse",true);
  for(unsigned i=0;i<3;++i) {
    if(i){check(indirect.allocator->Reset(),"retired allocator reset");check(indirect.list->Reset(indirect.allocator.Get(),nullptr),"indirect list reset");}
    uploadWord(a.Get(),0x24680000u+i);c.dev->CreateShaderResourceView(a.Get(),&srv,cpu);
    record(indirect.list.Get(),signature.Get(),arguments.Get());c.executeSealed({indirect.list.Get()});exactWord(readback.Get(),0x24680000u+i);
  }
  phase("retired_icb_reuse",false);
  std::printf("%s_CASE_PASS case=retired_icb_reuse epochs=3 stale_commands=0 output=exact\n",foundation);
}
void placedAliases() {
  Context c;auto desc=texDesc(DXGI_FORMAT_R8G8B8A8_UNORM,4,4,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  auto allocation=c.dev->GetResourceAllocationInfo(0,1,&desc);D3D12_HEAP_DESC hd{};
  hd.SizeInBytes=allocation.SizeInBytes;hd.Alignment=allocation.Alignment;hd.Properties=heap(D3D12_HEAP_TYPE_DEFAULT);
  hd.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;ComPtr<ID3D12Heap> shared;
  check(c.dev->CreateHeap(&hd,IID_PPV_ARGS(&shared)),"placed heap");
  ComPtr<ID3D12Resource> textures[2];ComPtr<ID3D12DescriptorHeap> views[2];D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
  for(unsigned i=0;i<2;++i) {
    check(c.dev->CreatePlacedResource(shared.Get(),0,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&textures[i])),"placed aliases");
    rtvs[i]=descriptor(c.dev.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_RTV,views[i]);c.dev->CreateRenderTargetView(textures[i].Get(),nullptr,rtvs[i]);
  }
  const float colors[2][4]={{1,0,0,1},{0,0,1,1}};const uint32_t words[]={0xff0000ff,0xffff0000};
  auto list=c.list();Readback out[2];
  for(unsigned i=0;i<2;++i) {
    D3D12_RESOURCE_BARRIER alias{};alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    alias.Aliasing={i?textures[0].Get():nullptr,textures[i].Get()};list.list->ResourceBarrier(1,&alias);
    list.list->ClearRenderTargetView(rtvs[i],colors[i],0,nullptr);
    transition(list.list.Get(),textures[i].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    out[i]=copyOut(c.dev.Get(),list.list.Get(),textures[i].Get());
    transition(list.list.Get(),textures[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  phase("placed_alias_clears",true);c.execute({list.list.Get()});phase("placed_alias_clears",false);
  for(unsigned i=0;i<2;++i)for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)if(out[i].at<uint32_t>(x,y)!=words[i])fail("placed clear alias ordering");
  std::printf("%s_CASE_PASS case=placed_alias_clears views=2 full_pixels=exact native_passes=separate\n",foundation);
}
void visibilityBatch() {
  Context c;auto color=committed(c.dev.Get(),D3D12_HEAP_TYPE_DEFAULT,texDesc(DXGI_FORMAT_R8G8B8A8_UNORM,4,4,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),D3D12_RESOURCE_STATE_RENDER_TARGET);
  ComPtr<ID3D12DescriptorHeap> views;auto rtv=descriptor(c.dev.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_RTV,views);c.dev->CreateRenderTargetView(color.Get(),nullptr,rtv);
  Draw draw(c.dev.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_UNKNOWN,1,"psMain",false);
  D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION,2,0};ComPtr<ID3D12QueryHeap> queries;check(c.dev->CreateQueryHeap(&qd,IID_PPV_ARGS(&queries)),"foundation queries");
  auto result=committed(c.dev.Get(),D3D12_HEAP_TYPE_READBACK,bufDesc(16),D3D12_RESOURCE_STATE_COPY_DEST);auto list=c.list();const float green[]={0,1,0,1};
  list.list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
  list.list->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,0);list.list->BeginQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,1);
  draw.draw(list.list.Get(),4,4,green);list.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,1);
  draw.draw(list.list.Get(),4,4,green);list.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,0);
  list.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_OCCLUSION,0,2,result.Get(),0);
  phase("query_fanout",true);c.execute({list.list.Get()});phase("query_fanout",false);
  void* mapped=nullptr;check(result->Map(0,nullptr,&mapped),"query result");const auto* values=static_cast<uint64_t*>(mapped);
  if(values[0]!=32 || values[1]!=16)fail("nested query counters exact");result->Unmap(0,nullptr);
  std::printf("%s_CASE_PASS case=query_fanout outer=32 inner=16 resets_and_repeated_counters=ordered\n",foundation);
}
void sparseMappingsRemainOrdered() {
  Context c;auto desc=bufDesc(65536);ComPtr<ID3D12Resource> sparse,alias;
  check(c.dev->CreateReservedResource(&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&sparse)),"mapped reserved buffer");
  check(c.dev->CreateReservedResource(&desc,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&alias)),"reserved alias buffer");
  D3D12_HEAP_DESC hd{};hd.SizeInBytes=2*65536;hd.Properties=heap(D3D12_HEAP_TYPE_DEFAULT);hd.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
  ComPtr<ID3D12Heap> physical;check(c.dev->CreateHeap(&hd,IID_PPV_ARGS(&physical)),"mapping heap");
  auto upload=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(256),D3D12_RESOURCE_STATE_GENERIC_READ);
  auto result=committed(c.dev.Get(),D3D12_HEAP_TYPE_READBACK,bufDesc(256),D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TILED_RESOURCE_COORDINATE start{};D3D12_TILE_REGION_SIZE region{1,FALSE,0,0,0};
  D3D12_TILE_RANGE_FLAGS flags=D3D12_TILE_RANGE_FLAG_NONE;const UINT count=1;
  phase("sparse_mapping_positive",true);
  for(UINT tile=0;tile<2;++tile) {
    c.queue->UpdateTileMappings(sparse.Get(),1,&start,&region,physical.Get(),1,&flags,&tile,&count,D3D12_TILE_MAPPING_FLAG_NONE);
    c.queue->UpdateTileMappings(alias.Get(),1,&start,&region,physical.Get(),1,&flags,&tile,&count,D3D12_TILE_MAPPING_FLAG_NONE);
    uploadWord(upload.Get(),0x31410000u+tile);
    auto writer=c.list();writer.list->CopyBufferRegion(sparse.Get(),0,upload.Get(),0,4);c.execute({writer.list.Get()});
    auto reader=c.list();D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing={sparse.Get(),alias.Get()};reader.list->ResourceBarrier(1,&barrier);
    reader.list->CopyBufferRegion(result.Get(),0,alias.Get(),0,4);c.execute({reader.list.Get()});
    exactWord(result.Get(),0x31410000u+tile);
  }
  phase("sparse_mapping_positive",false);
  std::printf("%s_CASE_PASS case=sparse_mapping_positive remaps=2 alias_reads=exact conservative_publication=retained\n",foundation);
}
void cpuStripTransfers() {
  Context c;unsigned cases=0;
  for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32G32B32_FLOAT}) {
    const UINT width=129,height=format==DXGI_FORMAT_R8G8B8A8_UNORM?1025:257;
    const UINT pixel=format==DXGI_FORMAT_R8G8B8A8_UNORM?4:12,row=width*pixel,pitch=row+3;
    auto desc=texDesc(format,width,height,D3D12_RESOURCE_FLAG_NONE);
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_CUSTOM;props.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    props.MemoryPoolPreference=D3D12_MEMORY_POOL_L0;props.CreationNodeMask=props.VisibleNodeMask=1;
    ComPtr<ID3D12Resource> texture;check(c.dev->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"strip CPU texture");
    std::vector<uint8_t> source(size_t(pitch)*height+32,0x35),destination(source.size(),0xcd);
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
      auto* target=source.data()+7+size_t(y)*pitch+x*pixel;
      if(pixel==4){target[0]=uint8_t(x*3+y);target[1]=uint8_t(y*5+x);target[2]=uint8_t(x^y);target[3]=255;}
      else {const float value[]={float(x)/128,float(y)/256,float((x+y)%31)/32};std::memcpy(target,value,12);}
    }
    check(texture->WriteToSubresource(0,nullptr,source.data()+7,pitch,pitch*height),"strip write");
    check(texture->ReadFromSubresource(destination.data()+7,pitch,pitch*height,0,nullptr),"strip read");
    for(UINT y=0;y<height;++y) {
      const size_t offset=7+size_t(y)*pitch;
      if(std::memcmp(source.data()+offset,destination.data()+offset,row))fail("strip values");
      for(UINT b=row;b<pitch;++b)if(destination[offset+b]!=0xcd)fail("strip row padding");
    }
    for(size_t i=0;i<7;++i)if(destination[i]!=0xcd)fail("strip prefix guard");
    for(size_t i=7+size_t(pitch)*height;i<destination.size();++i)if(destination[i]!=0xcd)fail("strip suffix guard");
    ++cases;
  }
  std::printf("%s_CASE_PASS case=cpu_strip_transfer formats=%u crosses_256KiB=1 unaligned_pitch_and_pointer=1 all_values_and_padding=exact\n",foundation,cases);
}
}
int main() {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  auto backend=reinterpret_cast<const char*(WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  if(!backend || std::strcmp(backend(),"owned-graphics-d3d12"))return 3;
  std::printf("%s_BACKEND id=%s\n",foundation,backend());
  volatileReplayAndICB();placedAliases();visibilityBatch();sparseMappingsRemainOrdered();cpuStripTransfers();
  std::printf("%s_PASS cases=6\n",foundation);return 0;
}
