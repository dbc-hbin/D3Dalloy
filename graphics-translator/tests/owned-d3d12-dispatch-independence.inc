// Included by submission-foundation. All possible-overlap controls access
// disjoint words in the shader; they are legal without a D3D12 UAV barrier.
// Their descriptor/root footprints must nevertheless keep our serial fallback.
void dispatchIndependence() {
  Context c;
  constexpr UINT count=8, words=512, bytes=words*4;
  constexpr uint32_t guard=0xcda51377;
  auto source=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);
  auto initial=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output=committed(c.dev.Get(),D3D12_HEAP_TYPE_DEFAULT,bufDesc(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto result=committed(c.dev.Get(),D3D12_HEAP_TYPE_DEFAULT,bufDesc(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto readback=committed(c.dev.Get(),D3D12_HEAP_TYPE_READBACK,bufDesc(bytes*2),D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped=nullptr;
  check(source->Map(0,nullptr,&mapped),"dispatch source map");
  for(UINT i=0;i<words;++i)static_cast<uint32_t*>(mapped)[i]=0x13570000+i;
  source->Unmap(0,nullptr);
  check(initial->Map(0,nullptr,&mapped),"dispatch initial map");
  std::fill_n(static_cast<uint32_t*>(mapped),words,guard);initial->Unmap(0,nullptr);
  ComPtr<ID3D12DescriptorHeap> descriptors;
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,count,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  check(c.dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&descriptors)),"dispatch table heap");
  const auto cpu=descriptors->GetCPUDescriptorHandleForHeapStart();
  const auto gpu=descriptors->GetGPUDescriptorHandleForHeapStart();
  const UINT increment=c.dev->GetDescriptorHandleIncrementSize(hd.Type);
  auto setView=[&](UINT slot,UINT first,UINT length) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;view.Buffer.FirstElement=first;view.Buffer.NumElements=length;
    view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
    c.dev->CreateUnorderedAccessView(output.Get(),nullptr,&view,{cpu.ptr+slot*increment});
  };
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
  D3D12_ROOT_PARAMETER parameters[4]{};
  parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[0].Descriptor.ShaderRegister=0;
  parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[1].DescriptorTable={1,&range};
  parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[2].Constants={0,0,1};
  parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor.ShaderRegister=1;
  D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=4;rd.pParameters=parameters;
  auto root=rootSignature(c.dev.Get(),rd);
  auto makePipeline=[&](const char *text,ID3D12RootSignature *signature) {
    auto code=compile(text,"cs","cs_5_1");D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature=signature;pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;check(c.dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"dispatch pipeline");
    return pipeline;
  };
  const char *writeCode="ByteAddressBuffer Source:register(t0);RWByteAddressBuffer Dest:register(u0);cbuffer C:register(b0){uint Index;}[numthreads(1,1,1)]void cs(){Dest.Store(Index*4,Source.Load(Index*4));}";
  auto writer=makePipeline(writeCode,root.Get());
  auto controlWriter=makePipeline("ByteAddressBuffer Source:register(t0);RWStructuredBuffer<uint> Dest:register(u0);cbuffer C:register(b0){uint Index;}[numthreads(1,1,1)]void cs(){Dest[Index]=Source.Load(Index*4);}",root.Get());
  auto reader=makePipeline("RWStructuredBuffer<uint> Dest:register(u0);RWByteAddressBuffer Result:register(u1);cbuffer C:register(b0){uint Index;}[numthreads(1,1,1)]void cs(){Result.Store(Index*4,Dest[Index]);}",root.Get());
  auto initialize=[&](ID3D12GraphicsCommandList *list,ID3D12Resource *target) {
    transition(list,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(target,0,initial.Get(),0,bytes);
    transition(list,target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  };
  auto bind=[&](ID3D12GraphicsCommandList *list) {
    ID3D12DescriptorHeap *heaps[]={descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(root.Get());list->SetComputeRootShaderResourceView(0,source->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(3,result->GetGPUVirtualAddress());
  };
  auto copy=[&](ID3D12GraphicsCommandList *list,ID3D12Resource *target,UINT offset) {
    transition(list,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(),offset,target,0,bytes);
    transition(list,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  };
  auto verify=[&](const std::vector<uint32_t> &expected,UINT offset=0) {
    void *data=nullptr;check(readback->Map(0,nullptr,&data),"dispatch readback map");
    const bool exact=!std::memcmp(static_cast<const char*>(data)+offset,expected.data(),bytes);
    readback->Unmap(0,nullptr);if(!exact)fail("dispatch all values and guards");
  };
  auto sealed=c.list();initialize(sealed.list.Get(),output.Get());bind(sealed.list.Get());
  sealed.list->SetPipelineState(writer.Get());
  for(UINT i=0;i<count;++i) {
    sealed.list->SetComputeRootDescriptorTable(1,{gpu.ptr+i*increment});
    sealed.list->SetComputeRoot32BitConstant(2,i,0);sealed.list->Dispatch(1,1,1);
  }
  copy(sealed.list.Get(),output.Get(),0);check(sealed.list->Close(),"dispatch sealed close");
  const char *names[]={"dispatch_tables_disjoint","dispatch_tables_overlap","dispatch_tables_restore","dispatch_tables_mixed"};
  // Dispatches 3 and 6 conflict with a nonadjacent earlier view. All
  // other dispatches in this replay are independent since the last barrier.
  const UINT mixed[count]={0,1,2,0,3,4,3,5};
  for(UINT replay=0;replay<4;++replay) {
    std::vector<uint32_t> expected(words,guard);
    for(UINT i=0;i<count;++i) {
      const UINT row=replay==1?0:replay==2?(i+3)%count:replay==3?mixed[i]:i;
      setView(i,row*64,64);expected[row*64+i]=0x13570000+i;
    }
    phase(names[replay],true);c.executeSealed({sealed.list.Get()});verify(expected);phase(names[replay],false);
  }
  // A shader-readonly UAV and a writer have potential RAW/WAR footprint edges,
  // while their actual words (one and zero) are disjoint and order-independent.
  D3D12_UNORDERED_ACCESS_VIEW_DESC structured{};structured.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
  structured.Buffer.NumElements=words;structured.Buffer.StructureByteStride=4;
  c.dev->CreateUnorderedAccessView(output.Get(),nullptr,&structured,cpu);
  for(UINT order=0;order<2;++order) {
    auto list=c.list();initialize(list.list.Get(),output.Get());initialize(list.list.Get(),result.Get());bind(list.list.Get());
    list.list->SetComputeRootDescriptorTable(1,gpu);
    for(UINT step=0;step<2;++step) {
      const bool read=(step^order)!=0;
      list.list->SetPipelineState(read?reader.Get():controlWriter.Get());
      list.list->SetComputeRoot32BitConstant(2,read?1:0,0);list.list->Dispatch(1,1,1);
    }
    copy(list.list.Get(),output.Get(),0);copy(list.list.Get(),result.Get(),bytes);
    const char *name=order?"dispatch_potential_war":"dispatch_potential_raw";
    phase(name,true);c.execute({list.list.Get()});
    std::vector<uint32_t> expected(words,guard);expected[0]=0x13570000;verify(expected);
    expected[0]=guard;verify(expected,bytes);phase(name,false);
  }
  setView(0,0,words);
  auto bounded=c.list();initialize(bounded.list.Get(),output.Get());bind(bounded.list.Get());
  bounded.list->SetPipelineState(writer.Get());bounded.list->SetComputeRootDescriptorTable(1,gpu);
  for(UINT i=0;i<65;++i){bounded.list->SetComputeRoot32BitConstant(2,i,0);bounded.list->Dispatch(1,1,1);}
  copy(bounded.list.Get(),output.Get(),0);
  phase("dispatch_budget_fallback",true);c.execute({bounded.list.Get()});
  std::vector<uint32_t> expected(words,guard);for(UINT i=0;i<65;++i)expected[i]=0x13570000+i;
  verify(expected);phase("dispatch_budget_fallback",false);
  // Unknown root-UAV lengths cover their whole backing. Distinct allocations
  // remain provably independent, sharing only a readonly input allocation.
  parameters[1]={};parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[1].Descriptor.ShaderRegister=0;
  rd.NumParameters=3;
  auto rootViews=rootSignature(c.dev.Get(),rd);auto rootWriter=makePipeline(writeCode,rootViews.Get());
  std::array<ComPtr<ID3D12Resource>,count> targets;
  auto roots=c.list();roots.list->SetComputeRootSignature(rootViews.Get());roots.list->SetPipelineState(rootWriter.Get());
  roots.list->SetComputeRootShaderResourceView(0,source->GetGPUVirtualAddress());
  for(UINT i=0;i<count;++i)targets[i]=committed(c.dev.Get(),D3D12_HEAP_TYPE_DEFAULT,bufDesc(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  for(auto &target:targets)initialize(roots.list.Get(),target.Get());
  for(UINT i=0;i<count;++i) {
    roots.list->SetComputeRootUnorderedAccessView(1,targets[i]->GetGPUVirtualAddress());
    roots.list->SetComputeRoot32BitConstant(2,i,0);roots.list->Dispatch(1,1,1);
  }
  auto rootReadback=committed(c.dev.Get(),D3D12_HEAP_TYPE_READBACK,bufDesc(bytes*count),D3D12_RESOURCE_STATE_COPY_DEST);
  for(UINT i=0;i<count;++i){transition(roots.list.Get(),targets[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);roots.list->CopyBufferRegion(rootReadback.Get(),i*bytes,targets[i].Get(),0,bytes);}
  phase("dispatch_roots_disjoint",true);c.execute({roots.list.Get()});
  check(rootReadback->Map(0,nullptr,&mapped),"dispatch root readback");
  for(UINT i=0;i<count;++i)for(UINT j=0;j<words;++j)
    if(static_cast<const uint32_t*>(mapped)[i*words+j]!=(j==i?0x13570000+i:guard))fail("dispatch root values and guards");
  rootReadback->Unmap(0,nullptr);phase("dispatch_roots_disjoint",false);
  const auto properties=heap(D3D12_HEAP_TYPE_DEFAULT);
  const auto desc=bufDesc(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ComPtr<ID3D12Resource> shared,opened;
  check(c.dev->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_SHARED,&desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&shared)),"shared dispatch buffer");
  HANDLE handle=nullptr;
  check(c.dev->CreateSharedHandle(shared.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"share dispatch buffer");
  check(c.dev->OpenSharedHandle(handle,IID_PPV_ARGS(&opened)),"reopen dispatch buffer");
  CloseHandle(handle);
  auto aliases=c.list();initialize(aliases.list.Get(),shared.Get());
  aliases.list->SetComputeRootSignature(rootViews.Get());aliases.list->SetPipelineState(rootWriter.Get());
  aliases.list->SetComputeRootShaderResourceView(0,source->GetGPUVirtualAddress());
  // Each dispatch writes a distinct word. A and its reopened alias B have
  // overlapping root footprints despite their separate native buffers.
  for(UINT i=0;i<3;++i) {
    aliases.list->SetComputeRootUnorderedAccessView(1,(i==2?opened:shared)->GetGPUVirtualAddress());
    aliases.list->SetComputeRoot32BitConstant(2,i,0);aliases.list->Dispatch(1,1,1);
  }
  copy(aliases.list.Get(),shared.Get(),0);
  phase("dispatch_external_backing",true);c.execute({aliases.list.Get()});
  expected.assign(words,guard);for(UINT i=0;i<3;++i)expected[i]=0x13570000+i;
  verify(expected);phase("dispatch_external_backing",false);
  std::printf("%s_CASE_PASS case=dispatch_independence phases=9 table_replays=4 potential_raw_war_waw=serial budget=65 roots=8 external_alias=serial all_words_and_guards=exact\n",foundation);
}
