#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static bool ok(HRESULT hr, const char *what) {
  if (SUCCEEDED(hr)) return true;
  std::printf("SWIZZLE_FAIL %s hr=%08x\n", what, unsigned(hr));
  return false;
}
static D3D12_RESOURCE_DESC buffer_desc(uint64_t bytes) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;
  d.Format=DXGI_FORMAT_UNKNOWN;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
static uint16_t deposit(uint32_t v, uint16_t mask) {
  uint16_t result=0;
  for(uint16_t bit=1;bit;bit<<=1) if(mask&bit){if(v&1)result|=bit;v>>=1;}
  return result;
}
static bool cpu_visible_standard(ID3D12Device *device, ID3D12CommandQueue *queue) {
  using Microsoft::WRL::ComPtr;
  constexpr uint32_t width=130, height=130, pitch=768;
  D3D12_RESOURCE_DESC t{};
  t.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  t.Width=width;t.Height=height;t.DepthOrArraySize=1;t.MipLevels=1;
  t.Format=DXGI_FORMAT_R8G8B8A8_UNORM;t.SampleDesc.Count=1;
  t.Layout=D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
  D3D12_HEAP_PROPERTIES cpu{};
  cpu.Type=D3D12_HEAP_TYPE_CUSTOM;
  cpu.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  cpu.MemoryPoolPreference=D3D12_MEMORY_POOL_L0;
  cpu.CreationNodeMask=cpu.VisibleNodeMask=1;
  ComPtr<ID3D12Resource> texture, readback;
  if (!ok(device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&t,D3D12_RESOURCE_STATE_COPY_SOURCE,
          nullptr,IID_PPV_ARGS(&texture)),"cpu-standard-create")) return false;
  const uint32_t bytes=pitch*height;
  auto b=buffer_desc(bytes);
  D3D12_HEAP_PROPERTIES readback_heap{};readback_heap.Type=D3D12_HEAP_TYPE_READBACK;
  if (!ok(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&b,
          D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"cpu-standard-readback")) return false;
  uint8_t source[height*pitch]={}, recovered[height*pitch]={};
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width*4;++x)
    source[y*pitch+x]=uint8_t((x*7+y*19+91)&255);
  if (!ok(texture->WriteToSubresource(0,nullptr,source,pitch,0),"cpu-standard-write")) return false;
  if (!ok(texture->ReadFromSubresource(recovered,pitch,0,0,nullptr),"cpu-standard-read")) return false;
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width*4;++x)
    if(recovered[y*pitch+x]!=source[y*pitch+x]) {
      std::printf("SWIZZLE_FAIL cpu-transfer x=%u y=%u\n",x,y);return false;
    }
  void *mapped=nullptr;
  if (!ok(texture->Map(0,nullptr,&mapped),"cpu-standard-map") || !mapped) return false;
  const auto *tiles=static_cast<const uint8_t *>(mapped);
  for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width*4;++x) {
    uint64_t tile=(uint64_t(y/128)*2+(x/4)/128)*65536;
    uint16_t intra=deposit(((x/4)%128)*4+(x%4),0xaa8f)|deposit(y%128,0x5570);
    if(tiles[tile+intra]!=source[y*pitch+x]) {
      std::printf("SWIZZLE_FAIL cpu-canonical x=%u y=%u\n",x,y);texture->Unmap(0,nullptr);return false;
    }
  }
  texture->Unmap(0,nullptr);
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if (!ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"cpu-allocator") ||
      !ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
          IID_PPV_ARGS(&list)),"cpu-list") ||
      !ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"cpu-fence")) return false;
  D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=texture.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint.Format=t.Format;dst.PlacedFootprint.Footprint.Width=width;
  dst.PlacedFootprint.Footprint.Height=height;dst.PlacedFootprint.Footprint.Depth=1;
  dst.PlacedFootprint.Footprint.RowPitch=pitch;
  list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  if (!ok(list->Close(),"cpu-close")) return false;
  ID3D12CommandList *lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
  if (!ok(queue->Signal(fence.Get(),1),"cpu-signal")) return false;
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  if (!event) return false;
  const bool completed=ok(fence->SetEventOnCompletion(1,event),"cpu-fence-event") &&
                       WaitForSingleObject(event,20000)==WAIT_OBJECT_0;
  CloseHandle(event);
  if (!completed) {std::puts("SWIZZLE_FAIL cpu-timeout");return false;}
  D3D12_RANGE range{0,bytes};
  uint8_t *bytes_out=nullptr;
  if (!ok(readback->Map(0,&range,reinterpret_cast<void**>(&bytes_out)),"cpu-gpu-map")) return false;
  bool equal=true;
  for(uint32_t y=0;y<height && equal;++y)for(uint32_t x=0;x<width*4;++x)
    if(bytes_out[y*pitch+x]!=source[y*pitch+x]) {equal=false;break;}
  readback->Unmap(0,nullptr);
  if (!equal) {std::puts("SWIZZLE_FAIL cpu-gpu-transfer");return false;}
  std::puts("OWNED_D3D12_STANDARD_SWIZZLE_CPU_GPU_PASS canonical_map=1 transfers=1");
  return true;
}
static bool descriptor_capture(ID3D12Device *device, ID3D12CommandQueue *queue) {
  using Microsoft::WRL::ComPtr;
  // Every dispatch writes a distinct word, so the unchanged-table sequence needs no UAV ordering.
  ComPtr<ID3D12Resource> textures[6], output, readback;
  D3D12_RESOURCE_DESC t{};t.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  t.Width=128;t.Height=128;t.DepthOrArraySize=1;t.MipLevels=1;
  t.Format=DXGI_FORMAT_R8G8B8A8_UNORM;t.SampleDesc.Count=1;
  t.Layout=D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
  D3D12_HEAP_PROPERTIES cpu{};cpu.Type=D3D12_HEAP_TYPE_CUSTOM;
  cpu.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  cpu.MemoryPoolPreference=D3D12_MEMORY_POOL_L0;cpu.CreationNodeMask=cpu.VisibleNodeMask=1;
  const uint32_t colors[]={0xff332211,0xff776655,0xffbbaa99,0xff135724,0xff246813,0xffabcdef};
  uint32_t pixels[128*128];
  for(unsigned i=0;i<6;++i){
    if(!ok(device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&t,
           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&textures[i])),
           "capture-texture")) return false;
    for(auto &pixel:pixels)pixel=colors[i];
    if(!ok(textures[i]->WriteToSubresource(0,nullptr,pixels,512,0),"capture-write")) return false;
  }
  auto b=buffer_desc(24);b.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
  if(!ok(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,
         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)),"capture-output"))return false;
  b.Flags=D3D12_RESOURCE_FLAG_NONE;hp.Type=D3D12_HEAP_TYPE_READBACK;
  if(!ok(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,
         D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"capture-readback"))return false;
  ComPtr<ID3D12DescriptorHeap> heaps[2];
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors=4;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
  for(unsigned heap=0;heap<2;++heap){
    auto &h=heaps[heap];
    if(!ok(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&h)),"capture-heap"))return false;
    auto handle=h->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<4;++i){
      device->CreateShaderResourceView(textures[heap?4+i%2:i].Get(),nullptr,handle);
      handle.ptr+=stride;
    }
  }
  D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors=2; // Preserve t1 even while the first PSOs only use t0.
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable={1,&range};params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=params;
  ComPtr<ID3DBlob> serialized,errors;ComPtr<ID3D12RootSignature> root;
  if(!ok(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"capture-serialize") ||
     !ok(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
         IID_PPV_ARGS(&root)),"capture-root"))return false;
  ComPtr<ID3D12PipelineState> psos[6];
  for(unsigned i=0;i<6;++i){
    char shader[512];
    std::snprintf(shader,sizeof(shader),
      "Texture2D<float4> source:register(t%u); RWStructuredBuffer<uint> output:register(u0);"
      "[numthreads(1,1,1)] void main(){uint4 v=uint4(round(source.Load(int3(37,83,0))*255));"
      "output[%u]=v.x|(v.y<<8)|(v.z<<16)|(v.w<<24);}",i==2?1:0,i);
    ComPtr<ID3DBlob> code;
    if(!ok(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",
           D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"capture-compile"))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();
    pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if(!ok(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&psos[i])),"capture-pso"))return false;
  }
  ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if(!ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"capture-allocator") ||
     !ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
         IID_PPV_ARGS(&list)),"capture-list") ||
     !ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"capture-fence"))return false;
  if(!ok(list->SetName(L"standard-swizzle-descriptor-capture"),"capture-name"))return false;
  std::printf("OWNED_D3D12_STANDARD_SWIZZLE_CASE_BEGIN case=descriptor-capture list=%p dispatches=6\n",
              static_cast<void *>(list.Get()));std::fflush(stdout);
  ID3D12DescriptorHeap *bound[]={heaps[0].Get()};list->SetDescriptorHeaps(1,bound);
  list->SetComputeRootSignature(root.Get());
  auto table=heaps[0]->GetGPUDescriptorHandleForHeapStart();
  list->SetComputeRootDescriptorTable(0,table);
  list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
  for(unsigned i=0;i<6;++i){
    if(i==3){table.ptr+=2*stride;list->SetComputeRootDescriptorTable(0,table);}
    if(i==4){
      bound[0]=heaps[1].Get();list->SetDescriptorHeaps(1,bound);
      table=heaps[1]->GetGPUDescriptorHandleForHeapStart();
      list->SetComputeRootDescriptorTable(0,table);
    }
    if(i==5){
      D3D12_RESOURCE_BARRIER boundary{};boundary.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
      boundary.UAV.pResource=output.Get();list->ResourceBarrier(1,&boundary);
    }
    list->SetPipelineState(psos[i].Get());list->Dispatch(1,1,1);
  }
  D3D12_RESOURCE_BARRIER end{};end.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  end.Transition.pResource=output.Get();end.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  end.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  end.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&end);
  list->CopyBufferRegion(readback.Get(),0,output.Get(),0,24);
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
  const bool ready=ok(fence->SetEventOnCompletion(1,event),"capture-event") && ok(list->Close(),"capture-close");
  if(ready){ID3D12CommandList *lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);}
  const bool completed=ready && ok(queue->Signal(fence.Get(),1),"capture-signal") &&
                       WaitForSingleObject(event,20000)==WAIT_OBJECT_0;
  CloseHandle(event);if(!completed){std::puts("SWIZZLE_FAIL capture-completion");return false;}
  uint32_t *values=nullptr;D3D12_RANGE bytes{0,24};
  if(!ok(readback->Map(0,&bytes,reinterpret_cast<void **>(&values)),"capture-map"))return false;
  bool equal=true;
  const unsigned sources[]={0,0,1,2,4,4};
  for(unsigned i=0;i<6;++i){
    const uint32_t expected=colors[sources[i]];
    if(values[i]!=expected){
      std::printf("SWIZZLE_FAIL capture index=%u got=%08x expected=%08x\n",i,values[i],expected);equal=false;
    }
  }
  readback->Unmap(0,nullptr);
  if(equal)std::puts("OWNED_D3D12_STANDARD_SWIZZLE_DESCRIPTOR_CAPTURE_PASS unchanged=2 pso_other_slot=1 table_rebind=1 heap_rebind=1 new_pass=1");
  return equal;
}
int main() {
  const auto module = GetModuleHandleW(L"d3d12.dll");
  using BackendId = const char *(WINAPI *)();
  const auto backend = module ? reinterpret_cast<BackendId>(GetProcAddress(module, "OwnedGraphicsBackendId")) : nullptr;
  if (!backend || std::strcmp(backend(), "owned-graphics-d3d12") != 0) {
    std::puts("OWNED_D3D12_STANDARD_SWIZZLE_FAIL backend-id");
    return 1;
  }
  std::puts("OWNED_D3D12_STANDARD_SWIZZLE_BACKEND id=owned-graphics-d3d12");
  ID3D12Device *device=nullptr;
  ID3D12CommandQueue *queue=nullptr;
  ID3D12CommandAllocator *allocator=nullptr;
  ID3D12GraphicsCommandList *list=nullptr;
  ID3D12Resource *texture=nullptr,*upload=nullptr,*readback=nullptr;
  ID3D12Resource *placed_texture=nullptr,*placed_alias=nullptr,*canonical_upload=nullptr;
  ID3D12Resource *shader_output=nullptr;
  ID3D12Resource *paired_texture=nullptr;
  ID3D12Resource *alias_uav_a=nullptr,*alias_uav_b=nullptr;
  ID3D12Resource *alias_uav_buffer=nullptr;
  ID3D12Heap *alias_heap=nullptr;
  ID3D12DescriptorHeap *descriptor_heap=nullptr;
  ID3D12DescriptorHeap *rtv_heap=nullptr,*uav_cpu_heap=nullptr;
  ID3D12RootSignature *root=nullptr;
  ID3D12PipelineState *pipeline=nullptr;
  ID3D12PipelineState *buffer_pipeline=nullptr;
  ID3DBlob *code=nullptr,*root_bytes=nullptr,*errors=nullptr;
  ID3DBlob *buffer_code=nullptr;
  ID3D12Fence *fence=nullptr;
  HANDLE event=nullptr;
  int rc=1;
  do {
    if (!ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),reinterpret_cast<void**>(&device)),"device")) break;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!ok(device->CreateCommandQueue(&q,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(&queue)),"queue")) break;
    if (!ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),reinterpret_cast<void**>(&allocator)),"allocator")) break;
    if (!ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,nullptr,__uuidof(ID3D12GraphicsCommandList),reinterpret_cast<void**>(&list)),"list")) break;
    constexpr uint32_t width=130,height=130,pitch=768;
    D3D12_RESOURCE_DESC t{};t.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width=width;t.Height=height;t.DepthOrArraySize=1;t.MipLevels=1;
    t.Format=DXGI_FORMAT_R8G8B8A8_UNORM;t.SampleDesc.Count=1;
    t.Layout=D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
    D3D12_HEAP_PROPERTIES default_heap{};default_heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    if (!ok(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&t,
             D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&texture)),"standard-creation")) break;
    UINT total_tiles=0,tiling_count=1;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_TILE_SHAPE shape{};
    D3D12_SUBRESOURCE_TILING tiling{};
    device->GetResourceTiling(texture,&total_tiles,&packed,&shape,&tiling_count,0,&tiling);
    if (total_tiles!=4 || tiling_count!=1 || packed.NumStandardMips!=1 || packed.NumPackedMips!=0 ||
        shape.WidthInTexels!=128 || shape.HeightInTexels!=128 || shape.DepthInTexels!=1 ||
        tiling.WidthInTiles!=2 || tiling.HeightInTiles!=2 || tiling.DepthInTiles!=1) {
      std::printf("SWIZZLE_FAIL tiling total=%u count=%u standard=%u packed=%u shape=%ux%ux%u tiles=%ux%ux%u\n",
                  total_tiles,tiling_count,packed.NumStandardMips,packed.NumPackedMips,
                  shape.WidthInTexels,shape.HeightInTexels,shape.DepthInTexels,
                  tiling.WidthInTiles,tiling.HeightInTiles,tiling.DepthInTiles);
      break;
    }
    void *default_map=nullptr;
    if (SUCCEEDED(texture->Map(0,nullptr,&default_map))) {std::puts("SWIZZLE_FAIL default-map-authorized");break;}
    D3D12_RESOURCE_ALLOCATION_INFO alloc{};device->GetResourceAllocationInfo(&alloc,0,1,&t);
    if (alloc.SizeInBytes < 4*65536 || alloc.Alignment != 65536) {std::printf("SWIZZLE_FAIL allocation size=%llu alignment=%llu\n",(unsigned long long)alloc.SizeInBytes,(unsigned long long)alloc.Alignment);break;}
    D3D12_HEAP_PROPERTIES upload_heap{};upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    auto b=buffer_desc(uint64_t(pitch)*height);
    if (!ok(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&b,
             D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&upload)),"upload")) break;
    D3D12_HEAP_PROPERTIES readback_heap{};readback_heap.Type=D3D12_HEAP_TYPE_READBACK;
    if (!ok(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&b,
             D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&readback)),"readback")) break;
    uint8_t *src=nullptr;
    if (!ok(upload->Map(0,nullptr,reinterpret_cast<void**>(&src)),"map-upload")) break;
    for (uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width*4;++x)
      src[y*pitch+x]=uint8_t((x*13+y*29+7)&255);
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=upload;from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint.Format=t.Format;from.PlacedFootprint.Footprint.Width=width;
    from.PlacedFootprint.Footprint.Height=height;from.PlacedFootprint.Footprint.Depth=1;
    from.PlacedFootprint.Footprint.RowPitch=pitch;
    D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=texture;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=0;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=texture;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION back{};back.pResource=readback;back.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    back.PlacedFootprint=from.PlacedFootprint;
    list->CopyTextureRegion(&back,0,0,0,&to,nullptr);
    if (!ok(list->Close(),"close")) break;
    ID3D12CommandList *lists[]={list};queue->ExecuteCommandLists(1,lists);
    if (!ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),reinterpret_cast<void**>(&fence)),"fence")) break;
    if (!ok(queue->Signal(fence,1),"signal")) break;
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event || !ok(fence->SetEventOnCompletion(1,event),"fence-event") || WaitForSingleObject(event,20000)!=WAIT_OBJECT_0) {std::puts("SWIZZLE_FAIL timeout");break;}
    D3D12_RANGE read_range{0,uint64_t(pitch)*height};
    uint8_t *dst=nullptr;
    if (!ok(readback->Map(0,&read_range,reinterpret_cast<void**>(&dst)),"map-readback")) break;
    bool equal=true;uint32_t badx=0,bady=0;
    for (uint32_t y=0;y<height && equal;++y)for(uint32_t x=0;x<width*4;++x)
      if(dst[y*pitch+x]!=uint8_t((x*13+y*29+7)&255)){equal=false;badx=x;bady=y;break;}
    readback->Unmap(0,nullptr);
    if (!equal){std::printf("SWIZZLE_FAIL mismatch byteX=%u y=%u\n",badx,bady);break;}
    // A placed buffer and STANDARD texture at the same heap offset must share
    // the same canonical tile bytes, not a separately owned linear shadow.
    D3D12_HEAP_DESC hd{};hd.SizeInBytes=alloc.SizeInBytes;hd.Properties=default_heap;
    hd.Alignment=65536;hd.Flags=D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    if (!ok(device->CreateHeap(&hd,__uuidof(ID3D12Heap),reinterpret_cast<void**>(&alias_heap)),"alias-heap")) break;
    if (!ok(device->CreatePlacedResource(alias_heap,0,&t,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,
             __uuidof(ID3D12Resource),reinterpret_cast<void**>(&placed_texture)),"placed-standard")) break;
    auto canonical_desc=buffer_desc(alloc.SizeInBytes);
    if (!ok(device->CreatePlacedResource(alias_heap,0,&canonical_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,
             __uuidof(ID3D12Resource),reinterpret_cast<void**>(&placed_alias)),"placed-buffer-alias")) break;
    ID3D12Pageable *resident[]={alias_heap};
    if (!ok(device->Evict(1,resident),"evict-placed-standard") ||
        !ok(device->MakeResident(1,resident),"make-resident-placed-standard")) break;
    if (!ok(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&canonical_desc,
             D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,__uuidof(ID3D12Resource),
             reinterpret_cast<void**>(&canonical_upload)),"canonical-upload")) break;
    uint8_t *canonical=nullptr;
    if (!ok(canonical_upload->Map(0,nullptr,reinterpret_cast<void**>(&canonical)),"map-canonical")) break;
    std::memset(canonical,0,size_t(alloc.SizeInBytes));
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)for(uint32_t byte=0;byte<4;++byte){
      uint64_t tile=(uint64_t(y/128)*2+x/128)*65536;
      uint16_t intra=deposit((x%128)*4+byte,0xaa8f)|deposit(y%128,0x5570);
      canonical[tile+intra]=uint8_t(((x*4+byte)*17+y*31+123)&255);
    }
    canonical_upload->Unmap(0,nullptr);
    if (!ok(allocator->Reset(),"reset-allocator") || !ok(list->Reset(allocator,nullptr),"reset-list")) break;
    list->CopyBufferRegion(placed_alias,0,canonical_upload,0,alloc.SizeInBytes);
    D3D12_RESOURCE_BARRIER alias{};alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    alias.Aliasing.pResourceBefore=placed_alias;alias.Aliasing.pResourceAfter=placed_texture;
    list->ResourceBarrier(1,&alias);
    D3D12_TEXTURE_COPY_LOCATION placed_from{};placed_from.pResource=placed_texture;
    placed_from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;placed_from.SubresourceIndex=0;
    list->CopyTextureRegion(&back,0,0,0,&placed_from,nullptr);
    if (!ok(list->Close(),"close-alias")) break;
    ID3D12CommandList *alias_lists[]={list};queue->ExecuteCommandLists(1,alias_lists);
    if (!ok(queue->Signal(fence,2),"signal-alias") ||
        !ok(fence->SetEventOnCompletion(2,event),"fence-event-alias") ||
        WaitForSingleObject(event,20000)!=WAIT_OBJECT_0){std::puts("SWIZZLE_FAIL alias-timeout");break;}
    dst=nullptr;
    if (!ok(readback->Map(0,&read_range,reinterpret_cast<void**>(&dst)),"map-alias-readback")) break;
    equal=true;
    for(uint32_t y=0;y<height && equal;++y)for(uint32_t x=0;x<width*4;++x)
      if(dst[y*pitch+x]!=uint8_t((x*17+y*31+123)&255)){equal=false;badx=x;bady=y;break;}
    readback->Unmap(0,nullptr);
    if (!equal){std::printf("SWIZZLE_FAIL alias-mismatch byteX=%u y=%u\n",badx,bady);break;}
    std::puts("OWNED_D3D12_STANDARD_SWIZZLE_RESIDENCY_PASS placed_evict=1 placed_make_resident=1 gpu_alias_readback=1");
    // Exercise descriptor-table SRV import and UAV export in a real shader pass.
    D3D12_RESOURCE_DESC output_desc=t;
    output_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!ok(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&output_desc,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),
             reinterpret_cast<void**>(&shader_output)),"shader-output")) break;
    D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dh.NumDescriptors=3;dh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!ok(device->CreateDescriptorHeap(&dh,__uuidof(ID3D12DescriptorHeap),
             reinterpret_cast<void**>(&descriptor_heap)),"shader-descriptor-heap")) break;
    const UINT stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu=descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(texture,nullptr,cpu);
    cpu.ptr+=stride;
    device->CreateUnorderedAccessView(shader_output,nullptr,nullptr,cpu);
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    for(auto &range:ranges){range.NumDescriptors=1;range.BaseShaderRegister=0;
      range.OffsetInDescriptorsFromTableStart=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;}
    ranges[1].NumDescriptors=2; // inactive u1 can reference the same placed bytes as the written u0
    D3D12_ROOT_PARAMETER parameters[2]{};
    for(unsigned i=0;i<2;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[i].DescriptorTable.NumDescriptorRanges=1;
      parameters[i].DescriptorTable.pDescriptorRanges=&ranges[i];
      parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;
    if (!ok(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&root_bytes,&errors),
            "serialize-root")) break;
    if (!ok(device->CreateRootSignature(0,root_bytes->GetBufferPointer(),root_bytes->GetBufferSize(),
             __uuidof(ID3D12RootSignature),reinterpret_cast<void**>(&root)),"root")) break;
    static constexpr char shader[]=R"hlsl(
      Texture2D<float4> source : register(t0);
      RWTexture2D<float4> destination : register(u0);
      [numthreads(8,8,1)] void main(uint3 coord : SV_DispatchThreadID) {
        if(coord.x<130 && coord.y<130) destination[coord.xy]=source.Load(int3(coord.xy,0));
      }
    )hlsl";
    if (!ok(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",
                       D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"compile-shader")) {
      if(errors) std::fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr);
      break;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};cp.pRootSignature=root;
    cp.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if (!ok(device->CreateComputePipelineState(&cp,__uuidof(ID3D12PipelineState),
             reinterpret_cast<void**>(&pipeline)),"compute-pipeline")) break;
    if (!ok(allocator->Reset(),"reset-allocator-shader") ||
        !ok(list->Reset(allocator,pipeline),"reset-list-shader")) break;
    D3D12_RESOURCE_BARRIER source_barrier=barrier;
    source_barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    source_barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1,&source_barrier);
    ID3D12DescriptorHeap *heaps[]={descriptor_heap};list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(root);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu=descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(0,gpu);
    gpu.ptr+=stride;
    list->SetComputeRootDescriptorTable(1,gpu);
    list->Dispatch((width+7)/8,(height+7)/8,1);
    D3D12_RESOURCE_BARRIER output_barrier{};output_barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.pResource=shader_output;
    output_barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    output_barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&output_barrier);
    D3D12_TEXTURE_COPY_LOCATION shader_from{};shader_from.pResource=shader_output;
    shader_from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&back,0,0,0,&shader_from,nullptr);
    if (!ok(list->Close(),"close-shader")) break;
    ID3D12CommandList *shader_lists[]={list};queue->ExecuteCommandLists(1,shader_lists);
    if (!ok(queue->Signal(fence,3),"signal-shader") ||
        !ok(fence->SetEventOnCompletion(3,event),"fence-event-shader") ||
        WaitForSingleObject(event,20000)!=WAIT_OBJECT_0){std::puts("SWIZZLE_FAIL shader-timeout");break;}
    dst=nullptr;
    if (!ok(readback->Map(0,&read_range,reinterpret_cast<void**>(&dst)),"map-shader-readback")) break;
    equal=true;
    for(uint32_t y=0;y<height && equal;++y)for(uint32_t x=0;x<width*4;++x)
      if(dst[y*pitch+x]!=uint8_t((x*13+y*29+7)&255)){equal=false;badx=x;bady=y;break;}
    readback->Unmap(0,nullptr);
    if (!equal){std::printf("SWIZZLE_FAIL shader-mismatch byteX=%u y=%u\n",badx,bady);break;}
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=1;
    if (!ok(device->CreateDescriptorHeap(&rh,__uuidof(ID3D12DescriptorHeap),
             reinterpret_cast<void**>(&rtv_heap)),"rtv-heap")) break;
    device->CreateRenderTargetView(shader_output,nullptr,rtv_heap->GetCPUDescriptorHandleForHeapStart());
    if (!ok(allocator->Reset(),"reset-allocator-rtv") ||
        !ok(list->Reset(allocator,nullptr),"reset-list-rtv")) break;
    D3D12_RESOURCE_BARRIER rt{};rt.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rt.Transition.pResource=shader_output;rt.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rt.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1,&rt);
    D3D12_RECT rtv_rect{1,1,3,3};const float black[4]={0,0,0,1};
    list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(),black,1,&rtv_rect);
    D3D12_RESOURCE_BARRIER rt_end=rt;
    rt_end.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_end.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&rt_end);
    list->CopyTextureRegion(&back,0,0,0,&shader_from,nullptr);
    if (!ok(list->Close(),"close-rtv")) break;
    ID3D12CommandList *rtv_lists[]={list};queue->ExecuteCommandLists(1,rtv_lists);
    if (!ok(queue->Signal(fence,4),"signal-rtv") ||
        !ok(fence->SetEventOnCompletion(4,event),"fence-event-rtv") ||
        WaitForSingleObject(event,20000)!=WAIT_OBJECT_0){std::puts("SWIZZLE_FAIL rtv-timeout");break;}
    dst=nullptr;
    if (!ok(readback->Map(0,&read_range,reinterpret_cast<void**>(&dst)),"map-rtv-readback")) break;
    equal=true;
    for(uint32_t y=0;y<height && equal;++y)for(uint32_t x=0;x<width*4;++x){
      const uint32_t px=x/4,component=x%4;
      const uint8_t expected=(px>=1 && px<3 && y>=1 && y<3)
          ? uint8_t(component==3 ? 255 : 0) : uint8_t((x*13+y*29+7)&255);
      if(dst[y*pitch+x]!=expected){equal=false;badx=x;bady=y;break;}
    }
    readback->Unmap(0,nullptr);
    if (!equal){std::printf("SWIZZLE_FAIL rtv-mismatch byteX=%u y=%u\n",badx,bady);break;}
    D3D12_DESCRIPTOR_HEAP_DESC uh{};uh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;uh.NumDescriptors=1;
    if (!ok(device->CreateDescriptorHeap(&uh,__uuidof(ID3D12DescriptorHeap),
             reinterpret_cast<void**>(&uav_cpu_heap)),"uav-cpu-heap")) break;
    auto uav_cpu=uav_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(shader_output,nullptr,nullptr,uav_cpu);
    if (!ok(allocator->Reset(),"reset-allocator-uav") ||
        !ok(list->Reset(allocator,nullptr),"reset-list-uav")) break;
    D3D12_RESOURCE_BARRIER uav=rt;
    uav.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    uav.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->ResourceBarrier(1,&uav);
    list->SetDescriptorHeaps(1,heaps);
    const float red[4]={1,0,0,1};const D3D12_RECT uav_rect{5,5,8,8};
    list->ClearUnorderedAccessViewFloat(gpu,uav_cpu,shader_output,red,1,&uav_rect);
    D3D12_RESOURCE_BARRIER uav_end=uav;
    uav_end.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    uav_end.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&uav_end);
    list->CopyTextureRegion(&back,0,0,0,&shader_from,nullptr);
    if (!ok(list->Close(),"close-uav")) break;
    ID3D12CommandList *uav_lists[]={list};queue->ExecuteCommandLists(1,uav_lists);
    if (!ok(queue->Signal(fence,5),"signal-uav") ||
        !ok(fence->SetEventOnCompletion(5,event),"fence-event-uav") ||
        WaitForSingleObject(event,20000)!=WAIT_OBJECT_0){std::puts("SWIZZLE_FAIL uav-timeout");break;}
    dst=nullptr;
    if (!ok(readback->Map(0,&read_range,reinterpret_cast<void**>(&dst)),"map-uav-readback")) break;
    equal=true;
    for(uint32_t y=0;y<height && equal;++y)for(uint32_t x=0;x<width*4;++x){
      const uint32_t px=x/4,component=x%4;
      uint8_t expected=uint8_t((x*13+y*29+7)&255);
      if(px>=1 && px<3 && y>=1 && y<3) expected=uint8_t(component==3?255:0);
      if(px>=5 && px<8 && y>=5 && y<8) expected=uint8_t(component==0 || component==3 ? 255 : 0);
      if(dst[y*pitch+x]!=expected){equal=false;badx=x;bady=y;break;}
    }
    readback->Unmap(0,nullptr);
    if (!equal){std::printf("SWIZZLE_FAIL uav-mismatch byteX=%u y=%u\n",badx,bady);break;}
    bool pair_success=true;
    for(unsigned variant=0;variant<2;++variant){
      D3D12_RESOURCE_DESC paired=t;paired.Width=260;paired.Height=129;
      paired.Format=variant?DXGI_FORMAT_G8R8_G8B8_UNORM:DXGI_FORMAT_R8G8_B8G8_UNORM;
      if(!ok(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&paired,
             D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),
             reinterpret_cast<void**>(&paired_texture)),"paired-standard")){pair_success=false;break;}
      UINT paired_tiles=0,paired_tilings=1;D3D12_TILE_SHAPE paired_shape{};
      D3D12_PACKED_MIP_INFO paired_mips{};D3D12_SUBRESOURCE_TILING paired_sub{};
      device->GetResourceTiling(paired_texture,&paired_tiles,&paired_mips,&paired_shape,&paired_tilings,0,&paired_sub);
      if(paired_tiles!=4 || paired_shape.WidthInTexels!=256 || paired_shape.HeightInTexels!=128 ||
         paired_sub.WidthInTiles!=2 || paired_sub.HeightInTiles!=2){
        std::printf("SWIZZLE_FAIL paired-tiling variant=%u tiles=%u shape=%ux%u\n",variant,
                    paired_tiles,paired_shape.WidthInTexels,paired_shape.HeightInTexels);
        pair_success=false;break;
      }
      uint8_t *pairs=nullptr;
      if(!ok(upload->Map(0,nullptr,reinterpret_cast<void**>(&pairs)),"map-paired-upload")){
        pair_success=false;break;
      }
      for(uint32_t y=0;y<129;++y)for(uint32_t pair=0;pair<130;++pair)
        for(uint32_t byte=0;byte<4;++byte)
          pairs[y*pitch+pair*4+byte]=uint8_t((pair*13+y*17+byte*31+variant*19)&255);
      upload->Unmap(0,nullptr);
      D3D12_TEXTURE_COPY_LOCATION pair_from{};pair_from.pResource=upload;
      pair_from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      pair_from.PlacedFootprint.Footprint.Format=paired.Format;
      pair_from.PlacedFootprint.Footprint.Width=260;
      pair_from.PlacedFootprint.Footprint.Height=129;
      pair_from.PlacedFootprint.Footprint.Depth=1;
      pair_from.PlacedFootprint.Footprint.RowPitch=pitch;
      D3D12_TEXTURE_COPY_LOCATION pair_to{};pair_to.pResource=paired_texture;
      pair_to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_TEXTURE_COPY_LOCATION pair_back{};pair_back.pResource=readback;
      pair_back.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      pair_back.PlacedFootprint=pair_from.PlacedFootprint;
      if(!ok(allocator->Reset(),"reset-paired-allocator") ||
         !ok(list->Reset(allocator,nullptr),"reset-paired-list")){pair_success=false;break;}
      list->CopyTextureRegion(&pair_to,0,0,0,&pair_from,nullptr);
      D3D12_RESOURCE_BARRIER transition{};transition.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      transition.Transition.pResource=paired_texture;
      transition.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      transition.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
      transition.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
      list->ResourceBarrier(1,&transition);
      list->CopyTextureRegion(&pair_back,0,0,0,&pair_to,nullptr);
      if(!ok(list->Close(),"close-paired")){pair_success=false;break;}
      ID3D12CommandList *pair_lists[]={list};queue->ExecuteCommandLists(1,pair_lists);
      const UINT64 ticket=6+variant;
      if(!ok(queue->Signal(fence,ticket),"signal-paired") ||
         !ok(fence->SetEventOnCompletion(ticket,event),"paired-event") ||
         WaitForSingleObject(event,20000)!=WAIT_OBJECT_0){std::puts("SWIZZLE_FAIL paired-timeout");pair_success=false;break;}
      dst=nullptr;
      if(!ok(readback->Map(0,&read_range,reinterpret_cast<void**>(&dst)),"map-paired-readback")){
        pair_success=false;break;
      }
      equal=true;
      for(uint32_t y=0;y<129 && equal;++y)for(uint32_t pair=0;pair<130;++pair){
        for(uint32_t byte=0;byte<4;++byte){
          const uint8_t expected=uint8_t((pair*13+y*17+byte*31+variant*19)&255);
          if(dst[y*pitch+pair*4+byte]!=expected){equal=false;badx=pair*4+byte;bady=y;break;}
        }
        if(!equal)break;
      }
      readback->Unmap(0,nullptr);
      if(!equal){std::printf("SWIZZLE_FAIL paired-mismatch variant=%u byteX=%u y=%u\n",variant,badx,bady);
        pair_success=false;break;}
      paired_texture->Release();paired_texture=nullptr;
    }
    if(!pair_success)break;
    // The shader writes only u0. An inactive u1 descriptor may still name an
    // overlapping placed alias, and can be populated after list recording.
    if(!ok(device->CreatePlacedResource(alias_heap,0,&output_desc,
           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),
           reinterpret_cast<void**>(&alias_uav_a)),"alias-uav-a") ||
       !ok(device->CreatePlacedResource(alias_heap,0,&output_desc,
           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),
           reinterpret_cast<void**>(&alias_uav_b)),"alias-uav-b"))break;
    cpu=descriptor_heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=stride;
    device->CreateUnorderedAccessView(alias_uav_a,nullptr,nullptr,cpu);
    if(!ok(allocator->Reset(),"reset-alias-uav-allocator") ||
       !ok(list->Reset(allocator,pipeline),"reset-alias-uav-list"))break;
    D3D12_RESOURCE_BARRIER activate{};activate.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    activate.Aliasing.pResourceBefore=placed_texture;
    activate.Aliasing.pResourceAfter=alias_uav_a;
    list->ResourceBarrier(1,&activate);
    list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(root);
    gpu=descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(0,gpu);
    gpu.ptr+=stride;list->SetComputeRootDescriptorTable(1,gpu);
    list->Dispatch((width+7)/8,(height+7)/8,1);
    D3D12_RESOURCE_BARRIER alias_uav_end{};alias_uav_end.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    alias_uav_end.Transition.pResource=alias_uav_a;
    alias_uav_end.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    alias_uav_end.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    alias_uav_end.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&alias_uav_end);
    D3D12_TEXTURE_COPY_LOCATION alias_from{};alias_from.pResource=alias_uav_a;
    alias_from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&back,0,0,0,&alias_from,nullptr);
    if(!ok(list->Close(),"close-alias-uav"))break;
    cpu.ptr+=stride;
    device->CreateUnorderedAccessView(alias_uav_b,nullptr,nullptr,cpu);
    ID3D12CommandList *alias_uav_lists[]={list};queue->ExecuteCommandLists(1,alias_uav_lists);
    if(!ok(queue->Signal(fence,8),"signal-alias-uav") ||
       !ok(fence->SetEventOnCompletion(8,event),"alias-uav-event") ||
       WaitForSingleObject(event,20000)!=WAIT_OBJECT_0){std::puts("SWIZZLE_FAIL alias-uav-timeout");break;}
    dst=nullptr;
    if(!ok(readback->Map(0,&read_range,reinterpret_cast<void**>(&dst)),"map-alias-uav-readback"))break;
    equal=true;
    for(uint32_t y=0;y<height && equal;++y)for(uint32_t x=0;x<width*4;++x)
      if(dst[y*pitch+x]!=uint8_t((x*13+y*29+7)&255)){equal=false;badx=x;bady=y;break;}
    readback->Unmap(0,nullptr);
    if(!equal){std::printf("SWIZZLE_FAIL alias-uav-mismatch byteX=%u y=%u\n",badx,bady);break;}
    // An inactive STANDARD texture UAV in u1 must not overwrite an aliased
    // buffer UAV in u0, even though the buffer directly updates canonical bytes.
    auto uav_buffer_desc=buffer_desc(alloc.SizeInBytes);
    uav_buffer_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(!ok(device->CreatePlacedResource(alias_heap,0,&uav_buffer_desc,
           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,__uuidof(ID3D12Resource),
           reinterpret_cast<void**>(&alias_uav_buffer)),"alias-uav-buffer"))break;
    static constexpr char buffer_shader[]=R"hlsl(
      RWByteAddressBuffer destination : register(u0);
      [numthreads(1,1,1)] void main() { destination.Store(0,0x78563412); }
    )hlsl";
    if(!ok(D3DCompile(buffer_shader,sizeof(buffer_shader)-1,nullptr,nullptr,nullptr,
           "main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&buffer_code,&errors),
           "compile-buffer-shader"))break;
    D3D12_COMPUTE_PIPELINE_STATE_DESC buffer_cp{};buffer_cp.pRootSignature=root;
    buffer_cp.CS={buffer_code->GetBufferPointer(),buffer_code->GetBufferSize()};
    if(!ok(device->CreateComputePipelineState(&buffer_cp,__uuidof(ID3D12PipelineState),
           reinterpret_cast<void**>(&buffer_pipeline)),"buffer-pipeline"))break;
    cpu=descriptor_heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
    raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements=UINT(alloc.SizeInBytes/4);raw.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(alias_uav_buffer,nullptr,&raw,cpu);
    if(!ok(allocator->Reset(),"reset-alias-buffer-allocator") ||
       !ok(list->Reset(allocator,buffer_pipeline),"reset-alias-buffer-list"))break;
    D3D12_RESOURCE_BARRIER activate_buffer{};activate_buffer.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    activate_buffer.Aliasing.pResourceBefore=alias_uav_a;
    activate_buffer.Aliasing.pResourceAfter=alias_uav_buffer;
    list->ResourceBarrier(1,&activate_buffer);
    list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root);
    gpu=descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=stride;
    list->SetComputeRootDescriptorTable(1,gpu);
    list->Dispatch(1,1,1);
    D3D12_RESOURCE_BARRIER buffer_end{};buffer_end.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    buffer_end.Transition.pResource=alias_uav_buffer;
    buffer_end.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    buffer_end.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    buffer_end.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1,&buffer_end);
    list->CopyBufferRegion(readback,0,alias_uav_buffer,0,4);
    if(!ok(list->Close(),"close-alias-buffer"))break;
    ID3D12CommandList *buffer_lists[]={list};queue->ExecuteCommandLists(1,buffer_lists);
    if(!ok(queue->Signal(fence,9),"signal-alias-buffer") ||
       !ok(fence->SetEventOnCompletion(9,event),"alias-buffer-event") ||
       WaitForSingleObject(event,20000)!=WAIT_OBJECT_0){std::puts("SWIZZLE_FAIL alias-buffer-timeout");break;}
    D3D12_RANGE four_bytes{0,4};dst=nullptr;
    if(!ok(readback->Map(0,&four_bytes,reinterpret_cast<void**>(&dst)),"map-alias-buffer"))break;
    uint8_t observed[4];std::memcpy(observed,dst,4);readback->Unmap(0,nullptr);
    if(observed[0]!=0x12 || observed[1]!=0x34 || observed[2]!=0x56 || observed[3]!=0x78){
      std::printf("SWIZZLE_FAIL alias-buffer-result %02x %02x %02x %02x\n",
                  observed[0],observed[1],observed[2],observed[3]);break;
    }
    if (!cpu_visible_standard(device,queue)) break;
    if (!descriptor_capture(device,queue)) break;
    std::printf("OWNED_D3D12_STANDARD_SWIZZLE_PASS width=%u height=%u allocation=%llu\n",width,height,(unsigned long long)alloc.SizeInBytes);
    rc=0;
  } while(false);
  if(event) CloseHandle(event);
  if(errors) errors->Release();if(code) code->Release();if(root_bytes) root_bytes->Release();
  if(buffer_code) buffer_code->Release();if(buffer_pipeline) buffer_pipeline->Release();
  if(pipeline) pipeline->Release();if(root) root->Release();
  if(rtv_heap) rtv_heap->Release();if(uav_cpu_heap) uav_cpu_heap->Release();
  if(descriptor_heap) descriptor_heap->Release();if(shader_output) shader_output->Release();
  if(paired_texture) paired_texture->Release();
  if(alias_uav_buffer) alias_uav_buffer->Release();
  if(alias_uav_b) alias_uav_b->Release();if(alias_uav_a) alias_uav_a->Release();
  if(canonical_upload) canonical_upload->Release();
  if(placed_alias) placed_alias->Release();if(placed_texture) placed_texture->Release();
  if(alias_heap) alias_heap->Release();
  if(fence) fence->Release();if(readback) readback->Release();if(upload) upload->Release();
  if(texture) texture->Release();if(list) list->Release();if(allocator) allocator->Release();
  if(queue) queue->Release();if(device) device->Release();
  return rc;
}
