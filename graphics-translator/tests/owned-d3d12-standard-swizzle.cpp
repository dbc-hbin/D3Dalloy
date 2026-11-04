#include <d3d12.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

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
static bool batch_mips_arrays(ID3D12Device *device, ID3D12CommandQueue *queue, DXGI_FORMAT format,
                              const char *name) {
  using Microsoft::WRL::ComPtr;
  constexpr UINT mip_count=3, array_count=2, subresource_count=mip_count*array_count;
  constexpr uint8_t guard=0xa7;
  const bool packed=format!=DXGI_FORMAT_R8G8B8A8_UNORM;
  const UINT block_width=packed?2:1, width=packed?2050:1025, height=129;
  const UINT absent_g1_byte=format==DXGI_FORMAT_G8R8_G8B8_UNORM?2:3;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width=width;desc.Height=height;desc.DepthOrArraySize=array_count;desc.MipLevels=mip_count;
  desc.Format=format;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
  D3D12_RESOURCE_ALLOCATION_INFO allocation{};device->GetResourceAllocationInfo(&allocation,0,1,&desc);
  if(!allocation.SizeInBytes || allocation.SizeInBytes==UINT64_MAX || allocation.Alignment!=65536){
    std::printf("SWIZZLE_FAIL batch-allocation case=%s\n",name);return false;
  }
  D3D12_HEAP_PROPERTIES default_heap{};default_heap.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_HEAP_DESC heap_desc{};heap_desc.SizeInBytes=allocation.SizeInBytes;
  heap_desc.Alignment=allocation.Alignment;heap_desc.Properties=default_heap;
  heap_desc.Flags=D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
  ComPtr<ID3D12Heap> heaps[2];ComPtr<ID3D12Resource> textures[2], aliases[2];
  auto canonical_desc=buffer_desc(allocation.SizeInBytes);
  for(UINT i=0;i<2;++i){
    if(!ok(device->CreateHeap(&heap_desc,IID_PPV_ARGS(&heaps[i])),"batch-heap") ||
       !ok(device->CreatePlacedResource(heaps[i].Get(),0,&desc,
           i?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,
           IID_PPV_ARGS(&textures[i])),"batch-texture") ||
       !ok(device->CreatePlacedResource(heaps[i].Get(),0,&canonical_desc,D3D12_RESOURCE_STATE_COPY_DEST,
           nullptr,IID_PPV_ARGS(&aliases[i])),"batch-alias"))return false;
  }
  UINT tiles=0, tiling_count=subresource_count;
  D3D12_TILE_SHAPE shape{};D3D12_PACKED_MIP_INFO packed_mips{};
  D3D12_SUBRESOURCE_TILING tilings[subresource_count]{};
  device->GetResourceTiling(textures[0].Get(),&tiles,&packed_mips,&shape,&tiling_count,0,tilings);
  const UINT expected_tiles=packed?50:48;
  if(tiles!=expected_tiles || tiling_count!=subresource_count ||
     packed_mips.NumStandardMips!=mip_count || packed_mips.NumPackedMips!=0 ||
     shape.WidthInTexels!=128*block_width || shape.HeightInTexels!=128 || shape.DepthInTexels!=1){
    std::printf("SWIZZLE_FAIL batch-tiling case=%s tiles=%u count=%u shape=%ux%ux%u\n",
                name,tiles,tiling_count,shape.WidthInTexels,shape.HeightInTexels,shape.DepthInTexels);
    return false;
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[subresource_count]{};
  UINT rows[subresource_count]{};UINT64 row_bytes[subresource_count]{}, total_bytes=0;
  device->GetCopyableFootprints(&desc,0,subresource_count,512,footprints,rows,row_bytes,&total_bytes);
  UINT64 linear_bytes=512;UINT first_tile=0;
  for(UINT sub=0;sub<subresource_count;++sub){
    const UINT mip=sub%mip_count, w=width>>mip, h=height>>mip;
    const UINT units=(w+block_width-1)/block_width;
    const UINT expected_x=(w+shape.WidthInTexels-1)/shape.WidthInTexels;
    const UINT expected_y=(h+127)/128;
    if(rows[sub]!=h || row_bytes[sub]!=UINT64(units)*4 ||
       tilings[sub].WidthInTiles!=expected_x || tilings[sub].HeightInTiles!=expected_y ||
       tilings[sub].DepthInTiles!=1 || tilings[sub].StartTileIndexInOverallResource!=first_tile){
      std::printf("SWIZZLE_FAIL batch-subresource case=%s sub=%u\n",name,sub);return false;
    }
    first_tile+=expected_x*expected_y;
    linear_bytes=std::max(linear_bytes,footprints[sub].Offset+UINT64(footprints[sub].Footprint.RowPitch)*rows[sub]);
  }
  linear_bytes+=512;
  const UINT64 guard_bytes=std::max(allocation.SizeInBytes,linear_bytes);
  ComPtr<ID3D12Resource> source_upload, guard_upload, linear_readback, canonical_readback;
  D3D12_HEAP_PROPERTIES upload_heap{};upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
  D3D12_HEAP_PROPERTIES readback_heap{};readback_heap.Type=D3D12_HEAP_TYPE_READBACK;
  auto guard_desc=buffer_desc(guard_bytes), linear_desc=buffer_desc(linear_bytes);
  if(!ok(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&canonical_desc,
         D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&source_upload)),"batch-source-upload") ||
     !ok(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&guard_desc,
         D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&guard_upload)),"batch-guard-upload") ||
     !ok(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&linear_desc,
         D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&linear_readback)),"batch-linear-readback") ||
     !ok(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&canonical_desc,
         D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&canonical_readback)),"batch-canonical-readback"))
    return false;
  void *mapped=nullptr;
  if(!ok(guard_upload->Map(0,nullptr,&mapped),"batch-map-guard"))return false;
  std::memset(mapped,guard,size_t(guard_bytes));guard_upload->Unmap(0,nullptr);
  ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  if(!ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"batch-allocator") ||
     !ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
         IID_PPV_ARGS(&list)),"batch-list") ||
     !ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"batch-fence"))return false;
  auto alias=[&](ID3D12Resource *before,ID3D12Resource *after){
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore=before;barrier.Aliasing.pResourceAfter=after;list->ResourceBarrier(1,&barrier);
  };
  auto transition=[&](ID3D12Resource *resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    list->ResourceBarrier(1,&barrier);
  };
  list->CopyBufferRegion(aliases[0].Get(),0,source_upload.Get(),0,allocation.SizeInBytes);
  list->CopyBufferRegion(aliases[1].Get(),0,guard_upload.Get(),0,allocation.SizeInBytes);
  list->CopyBufferRegion(linear_readback.Get(),0,guard_upload.Get(),0,linear_bytes);
  alias(aliases[0].Get(),textures[0].Get());alias(aliases[1].Get(),textures[1].Get());
  // A texture copy imports both canonical resources and exports the destination.
  // Unlike a buffer copy, it must leave every unused canonical tile byte intact.
  list->CopyResource(textures[1].Get(),textures[0].Get());
  transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  for(UINT sub=0;sub<subresource_count;++sub){
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=textures[1].Get();
    source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=sub;
    D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=linear_readback.Get();
    destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprints[sub];
    list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
  }
  transition(textures[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  alias(textures[0].Get(),aliases[0].Get());alias(textures[1].Get(),aliases[1].Get());
  transition(aliases[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(canonical_readback.Get(),0,aliases[1].Get(),0,allocation.SizeInBytes);
  transition(aliases[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  if(!ok(list->Close(),"batch-close"))return false;
  for(UINT replay=0;replay<2;++replay){
    std::vector<uint8_t> source(size_t(allocation.SizeInBytes),0x6d);
    std::vector<uint8_t> canonical(size_t(allocation.SizeInBytes),guard);
    std::vector<uint8_t> linear(size_t(linear_bytes),guard);
    UINT absent_g1=0;
    for(UINT sub=0;sub<subresource_count;++sub){
      const UINT mip=sub%mip_count, w=width>>mip, h=height>>mip;
      const UINT units=(w+block_width-1)/block_width;
      for(UINT y=0;y<h;++y)for(UINT x=0;x<units;++x)for(UINT byte=0;byte<4;++byte){
        const UINT64 tile=tilings[sub].StartTileIndexInOverallResource+
                          UINT64(y/128)*tilings[sub].WidthInTiles+x/128;
        const UINT16 intra=deposit((x%128)*4+byte,0xaa8f)|deposit(y%128,0x5570);
        const UINT64 canonical_offset=tile*65536+intra;
        const UINT64 linear_offset=footprints[sub].Offset+UINT64(y)*footprints[sub].Footprint.RowPitch+x*4+byte;
        const uint8_t value=uint8_t(((x*4+byte)*13+y*29+sub*47+replay*83+7)&255);
        source[size_t(canonical_offset)]=value;
        const bool absent=packed && x*2+1>=w && byte==absent_g1_byte;
        if(!absent)canonical[size_t(canonical_offset)]=value;
        // Public row compaction writes zero for an absent second pixel's G.
        // The canonical STANDARD representation instead preserves that byte.
        linear[size_t(linear_offset)]=absent?0:value;
        absent_g1+=absent;
      }
    }
    if(!ok(source_upload->Map(0,nullptr,&mapped),"batch-map-source"))return false;
    std::memcpy(mapped,source.data(),source.size());source_upload->Unmap(0,nullptr);
    std::fprintf(stderr,"OWNED_D3D12_STANDARD_SWIZZLE_BATCH_PHASE begin=%s replay=%u\n",name,replay);
    std::fflush(stderr);
    ID3D12CommandList *lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
    const UINT64 ticket=replay+1;
    const bool completed=ok(queue->Signal(fence.Get(),ticket),"batch-signal") &&
                         ok(fence->SetEventOnCompletion(ticket,event),"batch-event") &&
                         WaitForSingleObject(event,30000)==WAIT_OBJECT_0;
    CloseHandle(event);
    if(!completed){std::printf("SWIZZLE_FAIL batch-timeout case=%s replay=%u\n",name,replay);return false;}
    for(UINT kind=0;kind<2;++kind){
      auto *readback=kind?canonical_readback.Get():linear_readback.Get();
      const auto &expected=kind?canonical:linear;
      D3D12_RANGE range{0,expected.size()};const uint8_t *actual=nullptr;
      if(!ok(readback->Map(0,&range,&mapped),"batch-map-readback"))return false;
      actual=static_cast<const uint8_t *>(mapped);
      size_t mismatch=0;while(mismatch<expected.size() && actual[mismatch]==expected[mismatch])++mismatch;
      if(mismatch!=expected.size()){
        std::printf("SWIZZLE_FAIL batch-bytes case=%s replay=%u kind=%s byte=%llu got=%02x expected=%02x\n",
                    name,replay,kind?"canonical":"linear",static_cast<unsigned long long>(mismatch),
                    actual[mismatch],expected[mismatch]);
        readback->Unmap(0,nullptr);return false;
      }
      readback->Unmap(0,nullptr);
    }
    std::fprintf(stderr,"OWNED_D3D12_STANDARD_SWIZZLE_BATCH_PHASE end=%s replay=%u\n",name,replay);
    std::fflush(stderr);
    std::printf("OWNED_D3D12_STANDARD_SWIZZLE_BATCH_REPLAY_PASS case=%s replay=%u tiles=%u mips=3 arrays=2 "
                "linear_bytes=%llu canonical_bytes=%llu absent_g1=%u exact=1 guards=1\n",name,replay,tiles,
                static_cast<unsigned long long>(linear_bytes),
                static_cast<unsigned long long>(allocation.SizeInBytes),absent_g1);
  }
  return true;
}
static bool shader_view_selection(ID3D12Device *device, ID3D12CommandQueue *queue) {
  using Microsoft::WRL::ComPtr;
  constexpr UINT width=1025, height=129, mip_count=3, array_count=2, subresource_count=6;
  constexpr uint8_t stale=0x2b;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width=width;desc.Height=height;desc.DepthOrArraySize=array_count;desc.MipLevels=mip_count;
  desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
  desc.Layout=D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
  D3D12_HEAP_PROPERTIES cpu{};cpu.Type=D3D12_HEAP_TYPE_CUSTOM;
  cpu.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
  cpu.MemoryPoolPreference=D3D12_MEMORY_POOL_L0;cpu.CreationNodeMask=cpu.VisibleNodeMask=1;
  ComPtr<ID3D12Resource> source, destination, inactive, alias, upload, seed, readback, prime_readback;
  if(!ok(device->CreateCommittedResource(&cpu,D3D12_HEAP_FLAG_NONE,&desc,
         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&source)),"view-source"))return false;
  const uint32_t colors[subresource_count]={0xff332211,0xff776655,0xffbbaa99,0xff135724,0xff246813,0xffabcdef};
  for(UINT sub=0;sub<subresource_count;++sub){
    const UINT mip=sub%mip_count,w=width>>mip,h=height>>mip;
    std::vector<uint32_t> pixels(size_t(w)*h,colors[sub]);
    if(!ok(source->WriteToSubresource(sub,nullptr,pixels.data(),w*4,0),"view-source-write"))return false;
  }
  desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_RESOURCE_ALLOCATION_INFO allocation{};device->GetResourceAllocationInfo(&allocation,0,1,&desc);
  if(allocation.SizeInBytes!=48*65536 || allocation.Alignment!=65536){
    std::puts("SWIZZLE_FAIL view-allocation");return false;
  }
  D3D12_HEAP_DESC hd{};hd.SizeInBytes=allocation.SizeInBytes;hd.Alignment=allocation.Alignment;
  hd.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;hd.Flags=D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
  ComPtr<ID3D12Heap> heap;
  auto bytes=buffer_desc(allocation.SizeInBytes);
  if(!ok(device->CreateHeap(&hd,IID_PPV_ARGS(&heap)),"view-heap") ||
     !ok(device->CreatePlacedResource(heap.Get(),0,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
         nullptr,IID_PPV_ARGS(&destination)),"view-destination") ||
     !ok(device->CreatePlacedResource(heap.Get(),0,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
         nullptr,IID_PPV_ARGS(&inactive)),"view-inactive") ||
     !ok(device->CreatePlacedResource(heap.Get(),0,&bytes,D3D12_RESOURCE_STATE_COPY_DEST,
         nullptr,IID_PPV_ARGS(&alias)),"view-alias"))return false;
  UINT tile_count=0,tiling_count=subresource_count;D3D12_TILE_SHAPE shape{};D3D12_PACKED_MIP_INFO packed{};
  D3D12_SUBRESOURCE_TILING tilings[subresource_count]{};
  device->GetResourceTiling(destination.Get(),&tile_count,&packed,&shape,&tiling_count,0,tilings);
  if(tile_count!=48 || tiling_count!=subresource_count || packed.NumPackedMips ||
     shape.WidthInTexels!=128 || shape.HeightInTexels!=128){
    std::puts("SWIZZLE_FAIL view-tiling");return false;
  }
  D3D12_HEAP_PROPERTIES upload_heap{};upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
  D3D12_HEAP_PROPERTIES readback_heap{};readback_heap.Type=D3D12_HEAP_TYPE_READBACK;
  auto prime_bytes=buffer_desc(256);
  if(!ok(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&bytes,
         D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"view-upload") ||
     !ok(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&bytes,
         D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&seed)),"view-seed") ||
     !ok(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&bytes,
         D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"view-readback") ||
     !ok(device->CreateCommittedResource(&readback_heap,D3D12_HEAP_FLAG_NONE,&prime_bytes,
         D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&prime_readback)),"view-prime-readback"))return false;
  void *mapped=nullptr;
  if(!ok(seed->Map(0,nullptr,&mapped),"view-seed-map"))return false;
  std::memset(mapped,stale,size_t(allocation.SizeInBytes));seed->Unmap(0,nullptr);
  ComPtr<ID3D12DescriptorHeap> descriptors;
  D3D12_DESCRIPTOR_HEAP_DESC dd{};dd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  dd.NumDescriptors=6;dd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if(!ok(device->CreateDescriptorHeap(&dd,IID_PPV_ARGS(&descriptors)),"view-descriptors"))return false;
  const UINT stride=device->GetDescriptorHandleIncrementSize(dd.Type);
  auto set_views=[&](UINT table,UINT source_sub,UINT source_mips,UINT destination_sub){
    auto handle=descriptors->GetCPUDescriptorHandleForHeapStart();handle.ptr+=table*3*stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=desc.Format;
    srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MostDetailedMip=source_sub%mip_count;srv.Texture2DArray.MipLevels=source_mips;
    srv.Texture2DArray.FirstArraySlice=source_sub/mip_count;srv.Texture2DArray.ArraySize=1;
    device->CreateShaderResourceView(source.Get(),&srv,handle);handle.ptr+=stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=desc.Format;
    uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray.MipSlice=destination_sub%mip_count;
    uav.Texture2DArray.FirstArraySlice=destination_sub/mip_count;uav.Texture2DArray.ArraySize=1;
    device->CreateUnorderedAccessView(destination.Get(),nullptr,&uav,handle);handle.ptr+=stride;
    uav.Texture2DArray.MipSlice=0;uav.Texture2DArray.FirstArraySlice=0;uav.Texture2DArray.ArraySize=array_count;
    device->CreateUnorderedAccessView(inactive.Get(),nullptr,&uav,handle);
  };
  set_views(0,0,1,0);set_views(1,1,1,3);
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;
  ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=2;
  ranges[1].OffsetInDescriptorsFromTableStart=1;
  D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable={2,ranges};
  D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=1;rs.pParameters=&parameter;
  ComPtr<ID3DBlob> serialized,code,errors;ComPtr<ID3D12RootSignature> root;
  if(!ok(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"view-serialize") ||
     !ok(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
         IID_PPV_ARGS(&root)),"view-root"))return false;
  static constexpr char shader[]=R"hlsl(
    Texture2DArray<float4> source : register(t0);
    RWTexture2DArray<float4> destination : register(u0);
    [numthreads(1,1,1)] void main() { destination[uint3(0,0,0)]=source.Load(int4(0,0,0,0)); }
  )hlsl";
  if(!ok(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",
         D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"view-compile"))return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();
  pd.CS={code->GetBufferPointer(),code->GetBufferSize()};ComPtr<ID3D12PipelineState> pso;
  if(!ok(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)),"view-pso"))return false;
  ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
  if(!ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"view-allocator") ||
     !ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pso.Get(),
         IID_PPV_ARGS(&list)),"view-list") ||
     !ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"view-fence"))return false;
  auto activate=[&](ID3D12Resource *before,ID3D12Resource *after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    b.Aliasing={before,after};list->ResourceBarrier(1,&b);
  };
  auto transition=[&](ID3D12Resource *resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
  };
  // Prime every shadow subresource with known stale bytes through a public copy.
  // Then replace canonical bytes without updating those shadows. Exporting any
  // unselected mip/slice after the shader would corrupt the exact-byte oracle.
  list->CopyBufferRegion(alias.Get(),0,seed.Get(),0,allocation.SizeInBytes);
  activate(alias.Get(),destination.Get());
  transition(destination.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=destination.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=prime_readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint.Footprint={desc.Format,1,1,1,256};D3D12_BOX one{0,0,0,1,1,1};
  list->CopyTextureRegion(&to,0,0,0,&from,&one);
  transition(destination.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  activate(destination.Get(),alias.Get());
  list->CopyBufferRegion(alias.Get(),0,upload.Get(),0,allocation.SizeInBytes);
  activate(alias.Get(),destination.Get());
  ID3D12DescriptorHeap *bound[]={descriptors.Get()};list->SetDescriptorHeaps(1,bound);
  list->SetComputeRootSignature(root.Get());
  // The two dispatches write distinct subresources, so no UAV ordering is needed.
  for(UINT table=0;table<2;++table){
    auto handle=descriptors->GetGPUDescriptorHandleForHeapStart();handle.ptr+=table*3*stride;
    list->SetComputeRootDescriptorTable(0,handle);list->Dispatch(1,1,1);
  }
  activate(destination.Get(),alias.Get());
  transition(alias.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(),0,alias.Get(),0,allocation.SizeInBytes);
  transition(alias.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  if(!ok(list->Close(),"view-close"))return false;
  const UINT source_subs[2][2]={{4,5},{0,1}}, destination_subs[2][2]={{2,4},{1,5}};
  for(UINT replay=0;replay<2;++replay){
    // Descriptor contents are intentionally changed only after Close, and again
    // after retirement before replaying the same immutable command recording.
    for(UINT table=0;table<2;++table)set_views(table,source_subs[replay][table],table?1:2,destination_subs[replay][table]);
    std::vector<uint8_t> expected(size_t(allocation.SizeInBytes));
    for(size_t i=0;i<expected.size();++i)expected[i]=uint8_t((i*13+replay*71+9)&255);
    if(!ok(upload->Map(0,nullptr,&mapped),"view-upload-map"))return false;
    std::memcpy(mapped,expected.data(),expected.size());upload->Unmap(0,nullptr);
    for(UINT table=0;table<2;++table){
      const UINT sub=destination_subs[replay][table];
      const size_t offset=size_t(tilings[sub].StartTileIndexInOverallResource)*65536;
      const uint32_t color=colors[source_subs[replay][table]];
      std::memcpy(expected.data()+offset,&color,sizeof(color));
    }
    std::fprintf(stderr,"OWNED_D3D12_STANDARD_SWIZZLE_VIEW_PHASE begin=selected replay=%u\n",replay);std::fflush(stderr);
    ID3D12CommandList *lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
    const UINT64 ticket=replay+1;
    const bool completed=ok(queue->Signal(fence.Get(),ticket),"view-signal") &&
                         ok(fence->SetEventOnCompletion(ticket,event),"view-event") &&
                         WaitForSingleObject(event,30000)==WAIT_OBJECT_0;
    CloseHandle(event);if(!completed){std::puts("SWIZZLE_FAIL view-timeout");return false;}
    D3D12_RANGE range{0,expected.size()};
    if(!ok(readback->Map(0,&range,&mapped),"view-readback-map"))return false;
    const auto *actual=static_cast<const uint8_t *>(mapped);
    size_t mismatch=0;while(mismatch<expected.size() && actual[mismatch]==expected[mismatch])++mismatch;
    if(mismatch!=expected.size()){
      std::printf("SWIZZLE_FAIL view-bytes replay=%u byte=%llu got=%02x expected=%02x\n",replay,
                  static_cast<unsigned long long>(mismatch),actual[mismatch],expected[mismatch]);
      readback->Unmap(0,nullptr);return false;
    }
    readback->Unmap(0,nullptr);
    std::fprintf(stderr,"OWNED_D3D12_STANDARD_SWIZZLE_VIEW_PHASE end=selected replay=%u\n",replay);std::fflush(stderr);
    std::printf("OWNED_D3D12_STANDARD_SWIZZLE_VIEW_REPLAY_PASS replay=%u bytes=%llu writes=2 "
                "after_close=1 table_rebind=1 overlap_srv=1 inactive_uav=1 untouched_stale=1 guards=1\n",replay,
                static_cast<unsigned long long>(allocation.SizeInBytes));
  }
  return true;
}
static bool representation_validity(ID3D12Device *device, ID3D12CommandQueue *queue) {
  using Microsoft::WRL::ComPtr;
  constexpr uint32_t width=130, height=130, subresources=4, words=12;
  constexpr uint32_t sentinel=0xcafebabe, writer_color=0xffc7470d, copy_color=0xffe17729;
  D3D12_RESOURCE_DESC td{};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width=width;td.Height=height;td.DepthOrArraySize=2;td.MipLevels=2;
  td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;
  td.Layout=D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> textures[2], upload, output, output_readback, image_readback;
  for(auto &texture:textures)
    if(!ok(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,
           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&texture)),"validity-texture"))return false;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[subresources]{};
  UINT rows[subresources]{};UINT64 row_bytes[subresources]{},texture_bytes=0;
  device->GetCopyableFootprints(&td,0,subresources,0,footprints,rows,row_bytes,&texture_bytes);
  const uint64_t texture_stride=(texture_bytes+511)&~uint64_t(511);
  const uint64_t image_bytes=texture_stride*2,patch_offset=image_bytes,guard_offset=patch_offset+512;
  auto create_buffer=[&](ComPtr<ID3D12Resource> &resource,uint64_t bytes,D3D12_HEAP_TYPE type,
                         D3D12_RESOURCE_STATES state,bool uav) {
    auto desc=buffer_desc(bytes);desc.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
    return ok(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,
              IID_PPV_ARGS(&resource)),"validity-buffer");
  };
  if(!create_buffer(upload,guard_offset+512,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,false) ||
     !create_buffer(output,words*4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,true) ||
     !create_buffer(output_readback,words*4,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,false) ||
     !create_buffer(image_readback,image_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,false))return false;

  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if(!ok(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"validity-heap"))return false;
  const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
  auto bind_views=[&](unsigned selected) {
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
    for(unsigned i=0;i<2;++i) {
      D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=td.Format;
      view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Texture2DArray.MostDetailedMip=i;view.Texture2DArray.MipLevels=1;
      view.Texture2DArray.FirstArraySlice=i;view.Texture2DArray.ArraySize=1;
      device->CreateShaderResourceView(textures[selected].Get(),&view,cpu);cpu.ptr+=stride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=td.Format;
    view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.ArraySize=1;
    device->CreateUnorderedAccessView(textures[selected].Get(),nullptr,&view,cpu);
  };
  bind_views(0);
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;
  ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;
  D3D12_ROOT_PARAMETER params[4]{};
  params[0].ParameterType=params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable={1,&ranges[0]};params[1].DescriptorTable={1,&ranges[1]};
  params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[2].Descriptor.ShaderRegister=1;
  params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[3].Constants.Num32BitValues=1;
  D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=4;rs.pParameters=params;
  ComPtr<ID3DBlob> serialized,errors;ComPtr<ID3D12RootSignature> root;
  if(!ok(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"validity-serialize") ||
     !ok(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
         IID_PPV_ARGS(&root)),"validity-root"))return false;
  const char read_shader[]=
      "Texture2DArray<float4> source:register(t0);RWStructuredBuffer<uint> output:register(u1);"
      "cbuffer Params:register(b0){uint slot;}"
      "[numthreads(1,1,1)]void main(){uint4 v=uint4(round(source.Load(int4(7,11,0,0))*255));"
      "output[slot]=v.x|(v.y<<8)|(v.z<<16)|(v.w<<24);}";
  const char write_shader[]=
      "RWTexture2DArray<float4> target:register(u0);"
      "[numthreads(1,1,1)]void main(){target[uint3(7,11,0)]=float4(13,71,199,255)/255.0;}";
  ComPtr<ID3D12PipelineState> read_pso,write_pso;
  auto make_pso=[&](const char *shader,ComPtr<ID3D12PipelineState> &pso) {
    ComPtr<ID3DBlob> code;
    if(!ok(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",
           D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors),"validity-compile"))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();
    pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    return ok(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)),"validity-pso");
  };
  if(!make_pso(read_shader,read_pso) || !make_pso(write_shader,write_pso))return false;
  ComPtr<ID3D12CommandAllocator> allocators[3];
  ComPtr<ID3D12GraphicsCommandList> lists[3];
  for(unsigned i=0;i<3;++i)
    if(!ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])),"validity-allocator") ||
       !ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,
           IID_PPV_ARGS(&lists[i])),"validity-list"))return false;
  auto transition=[](ID3D12GraphicsCommandList *list,ID3D12Resource *resource,D3D12_RESOURCE_STATES before,
                     D3D12_RESOURCE_STATES after,UINT subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,subresource,before,after};list->ResourceBarrier(1,&barrier);
  };
  for(unsigned texture=0;texture<2;++texture) {
    transition(lists[0].Get(),textures[texture].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    for(unsigned sub=0;sub<subresources;++sub) {
      D3D12_TEXTURE_COPY_LOCATION src{},dst{};
      src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint=footprints[sub];src.PlacedFootprint.Offset+=texture*texture_stride;
      dst.pResource=textures[texture].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=sub;
      lists[0]->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    transition(lists[0].Get(),textures[texture].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if(!ok(lists[0]->Close(),"validity-init-close"))return false;

  auto *chain=lists[1].Get();chain->SetName(L"standard-swizzle-representation-validity");
  chain->CopyBufferRegion(output.Get(),0,upload.Get(),guard_offset,words*4);
  transition(chain,output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap *heaps[]={heap.Get()};chain->SetDescriptorHeaps(1,heaps);
  chain->SetComputeRootSignature(root.Get());chain->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress()+4);
  auto read=[&](unsigned descriptor,unsigned slot) {
    auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=descriptor*stride;
    chain->SetPipelineState(read_pso.Get());chain->SetComputeRootDescriptorTable(0,gpu);
    chain->SetComputeRoot32BitConstant(3,slot,0);chain->Dispatch(1,1,1);
    // Separate native passes while retaining ordinary resource ordering.
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource=output.Get();chain->ResourceBarrier(1,&barrier);
  };
  read(0,0);read(0,1);read(1,2);read(1,3);
  for(auto &texture:textures)
    transition(chain,texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=2*stride;
  chain->SetPipelineState(write_pso.Get());chain->SetComputeRootDescriptorTable(1,gpu);chain->Dispatch(1,1,1);
  for(auto &texture:textures)
    transition(chain,texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  read(0,4);read(1,5);read(1,6);
  transition(chain,textures[0].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST,3);
  D3D12_TEXTURE_COPY_LOCATION patch_src{},patch_dst{};
  patch_src.pResource=upload.Get();patch_src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  patch_src.PlacedFootprint.Offset=patch_offset;patch_src.PlacedFootprint.Footprint={td.Format,1,1,1,256};
  patch_dst.pResource=textures[0].Get();patch_dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;patch_dst.SubresourceIndex=3;
  chain->CopyTextureRegion(&patch_dst,7,11,0,&patch_src,nullptr);
  transition(chain,textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,3);
  read(1,7);read(1,8);read(0,9);
  transition(chain,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  chain->CopyBufferRegion(output_readback.Get(),0,output.Get(),0,words*4);
  transition(chain,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  if(!ok(chain->Close(),"validity-chain-close"))return false;

  // Preserve row padding and allocation gaps while checking every texel of
  // both resources, including the mip/slice not selected by any shader view.
  lists[2]->CopyBufferRegion(image_readback.Get(),0,upload.Get(),0,image_bytes);
  for(unsigned texture=0;texture<2;++texture) {
    transition(lists[2].Get(),textures[texture].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    for(unsigned sub=0;sub<subresources;++sub) {
      D3D12_TEXTURE_COPY_LOCATION src{},dst{};
      src.pResource=textures[texture].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=sub;
      dst.pResource=image_readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint=footprints[sub];dst.PlacedFootprint.Offset+=texture*texture_stride;
      lists[2]->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    transition(lists[2].Get(),textures[texture].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  if(!ok(lists[2]->Close(),"validity-images-close"))return false;
  ComPtr<ID3D12Fence> fence;
  if(!ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"validity-fence"))return false;
  uint64_t fence_value=0;
  auto submit=[&](ID3D12GraphicsCommandList *list) {
    ID3D12CommandList *submitted[]={list};queue->ExecuteCommandLists(1,submitted);
    if(!ok(queue->Signal(fence.Get(),++fence_value),"validity-signal"))return false;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
    const bool done=ok(fence->SetEventOnCompletion(fence_value,event),"validity-event") &&
                    WaitForSingleObject(event,20000)==WAIT_OBJECT_0;
    CloseHandle(event);if(!done)std::puts("SWIZZLE_FAIL validity-timeout");return done;
  };
  const uint32_t seeds[2][4]={{0xff132537,0xff294b6d,0xff35799b,0xff81a3c5},
                             {0xff17395b,0xff456789,0xff7193b5,0xffa1c3e5}};
  for(unsigned replay=0;replay<3;++replay) {
    const unsigned selected=replay==1?1:0;
    std::vector<uint8_t> bytes(size_t(guard_offset+512),0xa5);
    uint32_t colors[2][4]{};
    for(unsigned texture=0;texture<2;++texture)for(unsigned sub=0;sub<subresources;++sub) {
      colors[texture][sub]=seeds[texture][sub]^(replay*0x00081020u);
      const auto &fp=footprints[sub];
      for(UINT y=0;y<fp.Footprint.Height;++y)for(UINT x=0;x<fp.Footprint.Width;++x)
        std::memcpy(bytes.data()+texture*texture_stride+fp.Offset+uint64_t(y)*fp.Footprint.RowPitch+x*4,&colors[texture][sub],4);
    }
    std::memcpy(bytes.data()+patch_offset,&copy_color,4);
    for(unsigned i=0;i<words;++i)std::memcpy(bytes.data()+guard_offset+i*4,&sentinel,4);
    void *mapped=nullptr;D3D12_RANGE no_read{0,0};
    if(!ok(upload->Map(0,&no_read,&mapped),"validity-upload-map"))return false;
    std::memcpy(mapped,bytes.data(),bytes.size());upload->Unmap(0,nullptr);
    // The same closed chain is replayed; both upload bytes and descriptor
    // resource identities change only after prior work has retired.
    bind_views(selected);
    if(!submit(lists[0].Get()))return false;
    std::fprintf(stderr,"OWNED_D3D12_STANDARD_SWIZZLE_REPRESENTATION_PHASE begin=chain replay=%u selected=%u\n",replay,selected);
    std::fflush(stderr);
    if(!submit(chain))return false;
    std::fprintf(stderr,"OWNED_D3D12_STANDARD_SWIZZLE_REPRESENTATION_PHASE end=chain replay=%u selected=%u\n",replay,selected);
    std::fflush(stderr);
    const uint32_t expected_words[]={sentinel,colors[selected][0],colors[selected][0],
        colors[selected][3],colors[selected][3],writer_color,colors[selected][3],colors[selected][3],
        selected?colors[selected][3]:copy_color,selected?colors[selected][3]:copy_color,writer_color,sentinel};
    D3D12_RANGE output_range{0,words*4};
    if(!ok(output_readback->Map(0,&output_range,&mapped),"validity-output-map"))return false;
    const bool outputs_equal=std::memcmp(mapped,expected_words,sizeof(expected_words))==0;
    if(!outputs_equal) {
      const auto *actual=static_cast<const uint32_t *>(mapped);
      for(unsigned i=0;i<words;++i)if(actual[i]!=expected_words[i])
        std::printf("SWIZZLE_FAIL validity-word replay=%u index=%u got=%08x expected=%08x\n",
                    replay,i,actual[i],expected_words[i]);
    }
    output_readback->Unmap(0,nullptr);if(!outputs_equal)return false;
    if(!submit(lists[2].Get()))return false;
    std::memcpy(bytes.data()+selected*texture_stride+footprints[0].Offset+
        uint64_t(11)*footprints[0].Footprint.RowPitch+7*4,&writer_color,4);
    std::memcpy(bytes.data()+footprints[3].Offset+uint64_t(11)*footprints[3].Footprint.RowPitch+7*4,&copy_color,4);
    D3D12_RANGE image_range{0,size_t(image_bytes)};
    if(!ok(image_readback->Map(0,&image_range,&mapped),"validity-image-map"))return false;
    const bool images_equal=std::memcmp(mapped,bytes.data(),size_t(image_bytes))==0;
    if(!images_equal) {
      const auto *actual=static_cast<const uint8_t *>(mapped);
      for(size_t i=0;i<image_bytes;++i)if(actual[i]!=bytes[i]) {
        std::printf("SWIZZLE_FAIL validity-image replay=%u byte=%zu got=%02x expected=%02x\n",replay,i,actual[i],bytes[i]);break;
      }
    }
    image_readback->Unmap(0,nullptr);if(!images_equal)return false;
    std::printf("OWNED_D3D12_STANDARD_SWIZZLE_REPRESENTATION_ORACLE replay=%u selected=%u words=12 image_bytes=%llu exact=1\n",
                replay,selected,static_cast<unsigned long long>(image_bytes));
  }
  std::puts("OWNED_D3D12_STANDARD_SWIZZLE_REPRESENTATION_PASS replays=3 close_count=1 read_dispatches=30 "
            "native_writers=3 explicit_copies=3 resources=2 subresources=8 guards=exact");

  // Four distinct public Execute calls, with neither a fence nor a readback
  // between them. Each writes a different output slot; reads 0/1 and 2/3
  // sample the same exact subresource. All these recordings stay immutable.
  auto new_list=[&](ComPtr<ID3D12GraphicsCommandList> &list) {
    return ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[1].Get(),
        nullptr,IID_PPV_ARGS(&list)),"cross-execute-list");
  };
  auto execute=[](ID3D12CommandQueue *q,ID3D12GraphicsCommandList *list) {
    ID3D12CommandList *submitted[]={list};q->ExecuteCommandLists(1,submitted);
  };
  const unsigned descriptors[]={0,0,1,1,0,0,1,1};
  ComPtr<ID3D12GraphicsCommandList> readers[8], setup, patches[2], finish;
  for(unsigned i=0;i<8;++i) {
    if(!new_list(readers[i]))return false;
    auto *list=readers[i].Get();
    auto srv=heap->GetGPUDescriptorHandleForHeapStart();srv.ptr+=descriptors[i]*stride;
    list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
    list->SetPipelineState(read_pso.Get());list->SetComputeRootDescriptorTable(0,srv);
    list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstant(3,1+i,0);list->Dispatch(1,1,1);
    if(!ok(list->Close(),"cross-execute-reader-close"))return false;
  }
  if(!new_list(setup))return false;
  setup->CopyBufferRegion(output.Get(),0,upload.Get(),guard_offset,words*4);
  transition(setup.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if(!ok(setup->Close(),"cross-execute-setup-close"))return false;
  for(unsigned i=0;i<2;++i) {
    if(!new_list(patches[i]))return false;
    auto *list=patches[i].Get();
    transition(list,textures[0].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST,0);
    auto src=patch_src,dst=patch_dst;
    src.PlacedFootprint.Offset=i?texture_stride:patch_offset;dst.SubresourceIndex=0;
    list->CopyTextureRegion(&dst,7,11,0,&src,nullptr);
    transition(list,textures[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,0);
    if(!ok(list->Close(),"cross-execute-copy-close"))return false;
  }
  if(!new_list(finish))return false;
  transition(finish.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  finish->CopyBufferRegion(output_readback.Get(),0,output.Get(),0,words*4);
  transition(finish.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  if(!ok(finish->Close(),"cross-execute-finish-close"))return false;
  // Restore the same final upload seeds (replay=2); the prior executions
  // completely retired before reusing a closed list or patching a descriptor.
  bind_views(0);
  if(!submit(lists[0].Get()) || !submit(setup.Get()))return false;
  auto phase=[](const char *edge,const char *name) {
    std::fprintf(stderr,"OWNED_D3D12_STANDARD_SWIZZLE_CROSS_EXECUTE_PHASE %s=%s\n",edge,name);
    std::fflush(stderr);
  };
  phase("begin","adjacent");
  for(unsigned i=0;i<3;++i)execute(queue,readers[i].Get());
  if(!submit(readers[3].Get()))return false;
  phase("end","adjacent");
  phase("begin","local-copy");
  execute(queue,patches[0].Get());
  if(!submit(readers[4].Get()))return false;
  phase("end","local-copy");
  // A write through a different direct queue changes the same canonical
  // pixel. Real public queue fences order the handoff in both directions.
  D3D12_COMMAND_QUEUE_DESC qd{};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> foreign;
  if(!ok(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&foreign)),"cross-execute-foreign-queue"))return false;
  phase("begin","foreign-copy");
  if(!ok(foreign->Wait(fence.Get(),fence_value),"cross-execute-producer-wait"))return false;
  execute(foreign.Get(),patches[1].Get());
  if(!ok(foreign->Signal(fence.Get(),++fence_value),"cross-execute-foreign-signal") ||
     !ok(queue->Wait(fence.Get(),fence_value),"cross-execute-consumer-wait"))return false;
  execute(queue,readers[5].Get());execute(queue,readers[6].Get());
  if(!submit(readers[7].Get()))return false;
  phase("end","foreign-copy");
  // Reuse the exact same closed recording, now selecting the other resource's
  // mip1/slice1. The first seven read results stay visible through this replay.
  phase("begin","rebind-replay");
  bind_views(1);execute(queue,readers[7].Get());
  if(!submit(finish.Get()))return false;
  phase("end","rebind-replay");
  constexpr uint32_t salt=2*0x00081020u;
  const uint32_t cross_expected[words]={sentinel,seeds[0][0]^salt,seeds[0][0]^salt,
      seeds[0][3]^salt,seeds[0][3]^salt,copy_color,seeds[1][0]^salt,seeds[0][3]^salt,
      seeds[1][3]^salt,sentinel,sentinel,sentinel};
  D3D12_RANGE read_range{0,words*4};void *result=nullptr;
  if(!ok(output_readback->Map(0,&read_range,&result),"cross-execute-map"))return false;
  const auto *actual=static_cast<const uint32_t *>(result);
  const bool cross_equal=!std::memcmp(actual,cross_expected,sizeof(cross_expected));
  for(unsigned i=0;i<words;++i)
    if(actual[i]!=cross_expected[i])
      std::printf("SWIZZLE_FAIL cross-execute-word index=%u got=%08x expected=%08x\n",i,actual[i],cross_expected[i]);
  output_readback->Unmap(0,nullptr);
  if(!cross_equal)return false;
  std::puts("OWNED_D3D12_STANDARD_SWIZZLE_CROSS_EXECUTE_PASS adjacent_reads=4 local_copy=1 foreign_copy=1 "
            "rebound_replay=1 words=12 guards=4 exact=1");
  // A and B have different values at mip1/slice1. The third dispatch must
  // recover A after an intervening use of B and a real queue handoff.
  if(!submit(setup.Get()))return false;
  phase("begin","resource-handoff");
  bind_views(0);if(!submit(readers[2].Get()))return false;
  bind_views(1);if(!submit(readers[3].Get()))return false;
  ComPtr<ID3D12GraphicsCommandList> unrelated;
  if (!new_list(unrelated)) return false;
  D3D12_RESOURCE_DESC color = td;
  color.Width = color.Height = 4;
  color.DepthOrArraySize = color.MipLevels = 1;
  color.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  color.SampleDesc.Count = 4;
  ComPtr<ID3D12Resource> msaa, resolved;
  if (!ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &color, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr, IID_PPV_ARGS(&msaa)), "handoff-msaa")) return false;
  color.SampleDesc.Count = 1;
  if (!ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &color, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                          nullptr, IID_PPV_ARGS(&resolved)), "handoff-resolved")) return false;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtv;
  if (!ok(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv)), "handoff-rtv")) return false;
  auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(msaa.Get(), nullptr, handle);
  const float white[4] = {1, 1, 1, 1};
  unrelated->ClearRenderTargetView(handle, white, 0, nullptr);
  transition(unrelated.Get(), msaa.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  unrelated->ResolveSubresource(resolved.Get(), 0, msaa.Get(), 0, color.Format);
  unrelated->CopyBufferRegion(output_readback.Get(), 0, upload.Get(), guard_offset, sizeof(uint32_t));
  if (!ok(unrelated->Close(),"handoff-unrelated-close") || !submit(unrelated.Get())) return false;
  bind_views(0);
  if(!ok(foreign->Wait(fence.Get(),fence_value),"handoff-wait"))return false;
  execute(foreign.Get(),readers[6].Get());
  if(!ok(foreign->Signal(fence.Get(),++fence_value),"handoff-signal") ||
     !ok(queue->Wait(fence.Get(),fence_value),"handoff-return"))return false;
  if(!submit(finish.Get()))return false;
  phase("end","resource-handoff");
  uint32_t handoff_expected[words];std::fill_n(handoff_expected,words,sentinel);
  handoff_expected[3]=handoff_expected[7]=seeds[0][3]^salt;
  handoff_expected[4]=seeds[1][3]^salt;
  if(!ok(output_readback->Map(0,&read_range,&result),"handoff-map"))return false;
  const bool handoff_equal=!std::memcmp(result,handoff_expected,sizeof(handoff_expected));
  output_readback->Unmap(0,nullptr);
  if(!handoff_equal)return false;
  std::puts("OWNED_D3D12_STANDARD_SWIZZLE_RESOURCE_HANDOFF_PASS resources=2 queues=2 words=12 guards=9 exact=1");
  return true;
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
    constexpr uint32_t width=1025,height=129,pitch=4352;
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
    if (total_tiles!=18 || tiling_count!=1 || packed.NumStandardMips!=1 || packed.NumPackedMips!=0 ||
        shape.WidthInTexels!=128 || shape.HeightInTexels!=128 || shape.DepthInTexels!=1 ||
        tiling.WidthInTiles!=9 || tiling.HeightInTiles!=2 || tiling.DepthInTiles!=1) {
      std::printf("SWIZZLE_FAIL tiling total=%u count=%u standard=%u packed=%u shape=%ux%ux%u tiles=%ux%ux%u\n",
                  total_tiles,tiling_count,packed.NumStandardMips,packed.NumPackedMips,
                  shape.WidthInTexels,shape.HeightInTexels,shape.DepthInTexels,
                  tiling.WidthInTiles,tiling.HeightInTiles,tiling.DepthInTiles);
      break;
    }
    void *default_map=nullptr;
    if (SUCCEEDED(texture->Map(0,nullptr,&default_map))) {std::puts("SWIZZLE_FAIL default-map-authorized");break;}
    D3D12_RESOURCE_ALLOCATION_INFO alloc{};device->GetResourceAllocationInfo(&alloc,0,1,&t);
    if (alloc.SizeInBytes < 18*65536 || alloc.Alignment != 65536) {std::printf("SWIZZLE_FAIL allocation size=%llu alignment=%llu\n",(unsigned long long)alloc.SizeInBytes,(unsigned long long)alloc.Alignment);break;}
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
      uint64_t tile=(uint64_t(y/128)*9+x/128)*65536;
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
        if(coord.x<1025 && coord.y<129) destination[coord.xy]=source.Load(int3(coord.xy,0));
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
      D3D12_RESOURCE_DESC paired=t;paired.Width=2050;paired.Height=129;
      paired.Format=variant?DXGI_FORMAT_G8R8_G8B8_UNORM:DXGI_FORMAT_R8G8_B8G8_UNORM;
      if(!ok(device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&paired,
             D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),
             reinterpret_cast<void**>(&paired_texture)),"paired-standard")){pair_success=false;break;}
      UINT paired_tiles=0,paired_tilings=1;D3D12_TILE_SHAPE paired_shape{};
      D3D12_PACKED_MIP_INFO paired_mips{};D3D12_SUBRESOURCE_TILING paired_sub{};
      device->GetResourceTiling(paired_texture,&paired_tiles,&paired_mips,&paired_shape,&paired_tilings,0,&paired_sub);
      if(paired_tiles!=18 || paired_shape.WidthInTexels!=256 || paired_shape.HeightInTexels!=128 ||
         paired_sub.WidthInTiles!=9 || paired_sub.HeightInTiles!=2){
        std::printf("SWIZZLE_FAIL paired-tiling variant=%u tiles=%u shape=%ux%u\n",variant,
                    paired_tiles,paired_shape.WidthInTexels,paired_shape.HeightInTexels);
        pair_success=false;break;
      }
      uint8_t *pairs=nullptr;
      if(!ok(upload->Map(0,nullptr,reinterpret_cast<void**>(&pairs)),"map-paired-upload")){
        pair_success=false;break;
      }
      for(uint32_t y=0;y<129;++y)for(uint32_t pair=0;pair<1025;++pair)
        for(uint32_t byte=0;byte<4;++byte)
          pairs[y*pitch+pair*4+byte]=uint8_t((pair*13+y*17+byte*31+variant*19)&255);
      upload->Unmap(0,nullptr);
      D3D12_TEXTURE_COPY_LOCATION pair_from{};pair_from.pResource=upload;
      pair_from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      pair_from.PlacedFootprint.Footprint.Format=paired.Format;
      pair_from.PlacedFootprint.Footprint.Width=2050;
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
      for(uint32_t y=0;y<129 && equal;++y)for(uint32_t pair=0;pair<1025;++pair){
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
    if (!shader_view_selection(device,queue)) break;
    if (!representation_validity(device,queue)) break;
    if (!batch_mips_arrays(device,queue,DXGI_FORMAT_R8G8B8A8_UNORM,"rgba8") ||
        !batch_mips_arrays(device,queue,DXGI_FORMAT_R8G8_B8G8_UNORM,"r8g8-b8g8") ||
        !batch_mips_arrays(device,queue,DXGI_FORMAT_G8R8_G8B8_UNORM,"g8r8-g8b8")) break;
    std::puts("OWNED_D3D12_STANDARD_SWIZZLE_BATCH_PASS cases=3 replays=6 subresources=36 "
              "canonical_guards=1 packed_odd_mip=1 large_alias=1");
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
