// Public API proof for Close-time shape vs Execute-time values, sparse-negative
// admission, placed aliases, ICB retirement reuse and visibility fanout batches.
#define main retained_render_fixture_main
#include "owned-d3d12-render-passes.cpp"
#undef main

namespace {
constexpr char foundation[]="OWNED_D3D12_SUBMISSION_FOUNDATION";
void phase(const char* name,bool start) {
  char marker[256];
  const int length = std::snprintf(marker, sizeof(marker), "%s_PHASE %s=%s\n", foundation, start ? "begin" : "end", name);
  DWORD written = 0;
  if (length <= 0 || size_t(length) >= sizeof(marker) ||
      !WriteFile(GetStdHandle(STD_ERROR_HANDLE), marker, DWORD(length), &written, nullptr) || written != DWORD(length))
    fail("foundation phase marker");
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
  ComPtr<ID3D12DescriptorHeap> descriptors;
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,65,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
  check(c.dev->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&descriptors)),"replay descriptor heap");
  auto cpu=descriptors->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_TYPELESS;srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Buffer.NumElements=64;srv.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
  c.dev->CreateShaderResourceView(a.Get(),&srv,cpu);
  auto control=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(256),D3D12_RESOURCE_STATE_GENERIC_READ);
  uploadWord(control.Get(),1);
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[1].Descriptor.ShaderRegister=0;
  params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[2].Descriptor.ShaderRegister=0;
  auto root=rootSignature(c.dev.Get(),{3,params});
  // The null-slot phase tests cache invalidation without performing a native
  // null load. The same sealed list reads its branch control from GPU-visible data.
  auto code=compile("ByteAddressBuffer Source:register(t0);RWByteAddressBuffer Dest:register(u0);cbuffer Control:register(b0){uint ReadSource;}[numthreads(1,1,1)]void cs(){uint v=0;[branch]if(ReadSource)v=Source.Load(0);Dest.Store(0,v);}","cs","cs_5_1");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline;check(c.dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"foundation compute");
  auto record=[&](ID3D12GraphicsCommandList* list,ID3D12CommandSignature* signature=nullptr,ID3D12Resource* arguments=nullptr) {
    ID3D12DescriptorHeap* heaps[]={descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
    list->SetPipelineState(pipeline.Get());list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0,descriptors->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
    list->SetComputeRootConstantBufferView(2,control->GetGPUVirtualAddress());
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
  // Reuse metadata only: payload changes must still reach the GPU. Mutations of
  // a used slot (including CopyDescriptors/null) must rebuild the access plan.
  ComPtr<ID3D12DescriptorHeap> copyHeap;
  auto copySource=descriptor(c.dev.Get(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,copyHeap);
  c.dev->CreateShaderResourceView(b.Get(),&srv,copySource);
  auto cacheStep=[&](const char* name,ID3D12Resource* source,uint32_t value) {
    if(source)uploadWord(source,value);
    uploadWord(control.Get(),source?1:0);
    phase(name,true);c.executeSealed({sealed.list.Get()});exactWord(readback.Get(),source?value:0);phase(name,false);
  };
  c.dev->CreateShaderResourceView(a.Get(),&srv,cpu);
  cacheStep("descriptor_cache_cold",a.Get(),0x73100001);
  cacheStep("descriptor_cache_payload",a.Get(),0x73100002);
  auto unused=cpu;unused.ptr+=64*c.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  c.dev->CreateShaderResourceView(b.Get(),&srv,unused);
  cacheStep("descriptor_cache_unrelated",a.Get(),0x73100003);
  c.dev->CopyDescriptorsSimple(1,cpu,copySource,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  cacheStep("descriptor_cache_copy",b.Get(),0x73100004);
  cacheStep("descriptor_cache_copy_payload",b.Get(),0x73100005);
  c.dev->CreateShaderResourceView(nullptr,&srv,cpu);
  cacheStep("descriptor_cache_null",nullptr,0);
  auto unrelated=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(256),D3D12_RESOURCE_STATE_GENERIC_READ);
  cacheStep("descriptor_cache_null_replay",nullptr,0);
  c.dev->CreateShaderResourceView(a.Get(),&srv,cpu);
  cacheStep("descriptor_cache_restore",a.Get(),0x73100008);
  auto vaChange=committed(c.dev.Get(),D3D12_HEAP_TYPE_UPLOAD,bufDesc(256),D3D12_RESOURCE_STATE_GENERIC_READ);
  cacheStep("descriptor_cache_va_epoch",a.Get(),0x73100009);
  cacheStep("descriptor_cache_va_replay",a.Get(),0x7310000a);
  std::printf("%s_CASE_PASS case=descriptor_cache_reuse executions=10 cold_and_invalidation=5 stable_hits=5 null_read_disabled=1 values=exact\n",foundation);
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
void placedDisjointRanges() {
  Context c;
  const D3D12_RESOURCE_DESC descriptions[] = {
      texDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
      texDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 17, 9, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), bufDesc(256)};
  D3D12_RESOURCE_ALLOCATION_INFO1 placements[3]{};
  const auto allocation = c.dev->GetResourceAllocationInfo1(0, 3, descriptions, placements);
  if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX || !allocation.Alignment)
    fail("disjoint placement allocation");
  D3D12_HEAP_DESC hd{};
  hd.SizeInBytes = allocation.SizeInBytes; hd.Alignment = allocation.Alignment;
  hd.Properties = heap(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Heap> physical;
  check(c.dev->CreateHeap(&hd, IID_PPV_ARGS(&physical)), "disjoint mixed heap");
  ComPtr<ID3D12Resource> placed[3];
  ComPtr<ID3D12DescriptorHeap> views[2];
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
  for (unsigned i = 0; i < 3; ++i) {
    if (!placements[i].SizeInBytes || !placements[i].Alignment ||
        placements[i].Offset % placements[i].Alignment ||
        (i && placements[i].Offset < placements[i - 1].Offset + placements[i - 1].SizeInBytes))
      fail("disjoint placement intervals");
    check(c.dev->CreatePlacedResource(physical.Get(), placements[i].Offset, &descriptions[i],
        i < 2 ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&placed[i])), "disjoint placed resource");
    if (i < 2) {
      rtvs[i] = descriptor(c.dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, views[i]);
      c.dev->CreateRenderTargetView(placed[i].Get(), nullptr, rtvs[i]);
    }
  }
  auto upload = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(256), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto words = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(256), D3D12_RESOURCE_STATE_COPY_DEST);
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "disjoint upload map");
  for (unsigned i = 0; i < 64; ++i) static_cast<uint32_t *>(mapped)[i] = 0x5a170000u + i * 17;
  upload->Unmap(0, nullptr);
  const float colors[2][4] = {{1, 0, 0, 1}, {0, 1, 1, 1}};
  const uint32_t expected[] = {0xff0000ff, 0xffffff00};
  auto commands = c.list();
  commands.list->ClearRenderTargetView(rtvs[0], colors[0], 0, nullptr);
  commands.list->ClearRenderTargetView(rtvs[1], colors[1], 0, nullptr);
  commands.list->CopyBufferRegion(placed[2].Get(), 0, upload.Get(), 0, 256);
  Readback pixels[2];
  for (unsigned i = 0; i < 2; ++i) {
    transition(commands.list.Get(), placed[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    pixels[i] = copyOut(c.dev.Get(), commands.list.Get(), placed[i].Get());
  }
  transition(commands.list.Get(), placed[2].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyBufferRegion(words.Get(), 0, placed[2].Get(), 0, 256);
  phase("placed_disjoint_ranges", true);
  c.execute({commands.list.Get()});
  phase("placed_disjoint_ranges", false);
  for (unsigned i = 0; i < 2; ++i)
    for (UINT y = 0; y < descriptions[i].Height; ++y)
      for (UINT x = 0; x < descriptions[i].Width; ++x)
        if (pixels[i].at<uint32_t>(x, y) != expected[i]) fail("disjoint placed full pixels");
  check(words->Map(0, nullptr, &mapped), "disjoint words map");
  for (unsigned i = 0; i < 64; ++i)
    if (static_cast<const uint32_t *>(mapped)[i] != 0x5a170000u + i * 17) fail("disjoint placed buffer words");
  words->Unmap(0, nullptr);
  std::printf("%s_CASE_PASS case=placed_disjoint_ranges textures=2 buffer=1 pixels=185 words=64 exact=1\n", foundation);
}
void placedSparseRangeReplay() {
  Context c;
  constexpr UINT tileBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  const auto textureDesc = texDesc(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, D3D12_RESOURCE_FLAG_NONE);
  const auto allocation = c.dev->GetResourceAllocationInfo(0, 1, &textureDesc);
  if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX || !allocation.Alignment ||
      allocation.Alignment % tileBytes) fail("sparse range allocation");
  const UINT64 textureOffset = ((2 * tileBytes + allocation.Alignment - 1) / allocation.Alignment) * allocation.Alignment;
  const UINT64 bufferOffset = textureOffset + ((allocation.SizeInBytes + tileBytes - 1) / tileBytes) * tileBytes;
  D3D12_HEAP_DESC hd{};
  hd.SizeInBytes = bufferOffset + 2 * tileBytes; hd.Alignment = allocation.Alignment;
  hd.Properties = heap(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Heap> physical;
  check(c.dev->CreateHeap(&hd, IID_PPV_ARGS(&physical)), "sparse range mixed heap");
  ComPtr<ID3D12Resource> texture, buffer, sparse;
  const auto bufferDesc = bufDesc(256), sparseDesc = bufDesc(tileBytes);
  check(c.dev->CreatePlacedResource(physical.Get(), textureOffset, &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&texture)), "sparse range placed texture");
  check(c.dev->CreatePlacedResource(physical.Get(), bufferOffset, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
      nullptr, IID_PPV_ARGS(&buffer)), "sparse range placed buffer");
  check(c.dev->CreateReservedResource(&sparseDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
      IID_PPV_ARGS(&sparse)), "sparse range reserved buffer");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 uploadBytes = 0;
  c.dev->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBytes);
  auto upload = committed(c.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, bufDesc(uploadBytes + 256), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto words = committed(c.dev.Get(), D3D12_HEAP_TYPE_READBACK, bufDesc(256), D3D12_RESOURCE_STATE_COPY_DEST);
  auto commands = c.list();
  D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  source.PlacedFootprint = footprint;
  D3D12_TEXTURE_COPY_LOCATION target{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  commands.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
  commands.list->CopyBufferRegion(buffer.Get(), 0, upload.Get(), uploadBytes, 256);
  transition(commands.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  auto pixels = copyOut(c.dev.Get(), commands.list.Get(), texture.Get());
  transition(commands.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands.list->CopyBufferRegion(words.Get(), 0, buffer.Get(), 0, 256);
  transition(commands.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  transition(commands.list.Get(), buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  check(commands.list->Close(), "sparse range single close");
  const char *names[] = {"placed_sparse_unmapped", "placed_sparse_disjoint", "placed_sparse_overlap", "placed_sparse_null"};
  D3D12_TILED_RESOURCE_COORDINATE start{};
  D3D12_TILE_REGION_SIZE region{1, FALSE, 0, 0, 0};
  const UINT count = 1;
  for (unsigned step = 0; step < 4; ++step) {
    // Only the mapping changes; the same sealed list and placement owners are
    // reused. The sparse resource is deliberately not a command-list operand.
    if (step) {
      const D3D12_TILE_RANGE_FLAGS flags = step == 3 ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE;
      const UINT offset = step == 2 ? UINT(textureOffset / tileBytes) : 0;
      c.queue->UpdateTileMappings(sparse.Get(), 1, &start, &region, step == 3 ? nullptr : physical.Get(),
          1, &flags, step == 3 ? nullptr : &offset, &count, D3D12_TILE_MAPPING_FLAG_NONE);
      // This case checks negative heap-range proofs after mapping retirement.
      // Confirm public fence completion before entering the proof phase; the
      // tiled timeline fixture separately checks asynchronous mapping execution.
      c.executeSealed({});
    }
    const uint32_t expected = 0xff325100u + step;
    void *mapped = nullptr;
    check(upload->Map(0, nullptr, &mapped), "sparse range upload map");
    auto *bytes = static_cast<uint8_t *>(mapped);
    std::memset(bytes, 0xa5, size_t(uploadBytes + 256));
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x)
        std::memcpy(bytes + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4, &expected, 4);
    for (unsigned i = 0; i < 64; ++i) {
      const uint32_t value = 0x6b280000u + step * 0x100 + i;
      std::memcpy(bytes + uploadBytes + i * 4, &value, 4);
    }
    upload->Unmap(0, nullptr);
    phase(names[step], true);
    c.executeSealed({commands.list.Get()});
    phase(names[step], false);
    for (UINT y = 0; y < 4; ++y)
      for (UINT x = 0; x < 4; ++x)
        if (pixels.at<uint32_t>(x, y) != expected) fail("sparse range replay full pixels");
    check(words->Map(0, nullptr, &mapped), "sparse range replay words map");
    for (unsigned i = 0; i < 64; ++i)
      if (static_cast<const uint32_t *>(mapped)[i] != 0x6b280000u + step * 0x100 + i)
        fail("sparse range replay words");
    words->Unmap(0, nullptr);
  }
  std::printf("%s_CASE_PASS case=placed_sparse_range_replay executions=4 close_count=1 mapping=unmapped,disjoint,overlap,null pixels=64 words=256 exact=1\n", foundation);
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
#include "owned-d3d12-dispatch-independence.inc"
int main() {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  auto backend=reinterpret_cast<const char*(WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  if(!backend || std::strcmp(backend(),"owned-graphics-d3d12"))return 3;
  std::printf("%s_BACKEND id=%s\n",foundation,backend());
  volatileReplayAndICB();placedAliases();placedDisjointRanges();placedSparseRangeReplay();
  visibilityBatch();sparseMappingsRemainOrdered();cpuStripTransfers();
  dispatchIndependence();
  std::printf("%s_PASS cases=10\n",foundation);return 0;
}
