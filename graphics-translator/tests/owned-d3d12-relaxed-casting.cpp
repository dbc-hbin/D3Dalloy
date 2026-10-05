// Independent public D3D12 consumer for VulkanOn12 FormatListCasting.
// Compile with the same modern DirectX-Headers include path as resource-interfaces.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char *step) {
  if (!value) { std::fprintf(stderr, "RELAXED_CASTING_FAIL step=%s\n", step); std::exit(1); }
}
void check(HRESULT hr, const char *step) {
  if (FAILED(hr)) { std::fprintf(stderr, "RELAXED_CASTING_FAIL step=%s hr=%08lx\n", step, (unsigned long)hr); std::exit(1); }
}
struct Context {
  ComPtr<ID3D12Device10> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  UINT64 serial = 0;
  Context() {
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device10");
    D3D12_COMMAND_QUEUE_DESC d{};
    check(device->CreateCommandQueue(&d, IID_PPV_ARGS(&queue)), "queue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
  }
  ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES p{}; p.Type = heap;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
  }
  void submit(ID3D12GraphicsCommandList *list) {
    ID3D12CommandList *lists[] = {list}; queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++serial), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); require(event != nullptr, "event");
    check(fence->SetEventOnCompletion(serial, event), "completion");
    DWORD status = WaitForSingleObject(event, 20000); CloseHandle(event);
    require(status == WAIT_OBJECT_0, "bounded GPU completion");
    check(device->GetDeviceRemovedReason(), "GPU device status");
  }
};
void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1, &b);
}
void capabilities(Context &c) {
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
  check(c.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&options,sizeof(options)),"OPTIONS12");
  std::printf("RELAXED_CASTING_OPTIONS12 relaxed=%u\n",options.RelaxedFormatCastingSupported!=FALSE);
  require(options.RelaxedFormatCastingSupported!=FALSE,"RelaxedFormatCastingSupported");
}
void sharedMetadata(ID3D12Device10 *recipient,HANDLE handle,DXGI_FORMAT base,DXGI_FORMAT target,
                    DXGI_FORMAT alias=DXGI_FORMAT_UNKNOWN) {
  auto bytes=static_cast<BYTE *>(MapViewOfFile(handle,FILE_MAP_WRITE,0,0,65536));
  require(bytes!=nullptr,"shared cast metadata mapping");
  auto version=reinterpret_cast<uint32_t *>(bytes+8);
  auto marker=reinterpret_cast<uint32_t *>(bytes+4852);
  auto formats=reinterpret_cast<uint64_t *>(bytes+4864);
  uint64_t expected[4]{};
  for (DXGI_FORMAT format:{base,target,alias})
    if (format!=DXGI_FORMAT_UNKNOWN) expected[UINT(format)/64] |= uint64_t(1)<<(UINT(format)%64);
  require(*version==3 && *marker==1,"shared explicit casting ABI v3");
  require(!std::memcmp(formats,expected,sizeof(expected)),"shared canonical format membership");
  ComPtr<ID3D12Resource> rejected;
  *version=2;
  require(recipient->OpenSharedHandle(handle,IID_PPV_ARGS(&rejected))==E_INVALIDARG,"shared v2 ABI rejected");
  *version=3; *marker=2;
  require(recipient->OpenSharedHandle(handle,IID_PPV_ARGS(&rejected))==E_INVALIDARG,"shared explicit marker rejected");
  *marker=1; formats[3] |= uint64_t(1)<<63;
  require(recipient->OpenSharedHandle(handle,IID_PPV_ARGS(&rejected))==E_INVALIDARG,"shared unknown format rejected");
  formats[3] &= ~(uint64_t(1)<<63);
  UnmapViewOfFile(bytes);
}
void run(Context &c, DXGI_FORMAT base, DXGI_FORMAT target, UINT unit, UINT channels,
         const char *scalar, bool bc, bool placed, bool legacy_api = false, bool writable = false, bool sparse = false,
         Context *shared_creator = nullptr) {
  D3D12_HEAP_PROPERTIES p{}; p.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC1 d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  const bool compressed_resource=bc || base==DXGI_FORMAT_BC4_TYPELESS;
  d.Width = compressed_resource ? 16 : 4; d.Height = compressed_resource ? 8 : 2; d.DepthOrArraySize = 2;
  d.MipLevels = 2; d.SampleDesc.Count = 1; d.Format = base;
  if (writable) d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const bool alias_oracle = bc && writable;
  DXGI_FORMAT formats[] = {base, target, DXGI_FORMAT_R16G16B16A16_UINT};
  const UINT format_count=alias_oracle ? 3 : 2;
  ComPtr<ID3D12Resource> texture;
  ComPtr<ID3D12Resource> shared_source;
  ComPtr<ID3D12Heap> heap;
  D3D12_RESOURCE_DESC creation_desc{};
  creation_desc.Dimension=d.Dimension; creation_desc.Width=d.Width; creation_desc.Height=d.Height;
  creation_desc.DepthOrArraySize=d.DepthOrArraySize; creation_desc.MipLevels=d.MipLevels;
  creation_desc.Format=d.Format; creation_desc.SampleDesc=d.SampleDesc;
  creation_desc.Flags=d.Flags;
  if (sparse) {
    creation_desc.Layout=D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    check(c.device->CreateReservedResource2(&creation_desc,D3D12_BARRIER_LAYOUT_COMMON,nullptr,nullptr,
        format_count,formats,IID_PPV_ARGS(&texture)),"reserved cast list");
    UINT tiles=0; c.device->GetResourceTiling(texture.Get(),&tiles,nullptr,nullptr,nullptr,0,nullptr);
    require(tiles != 0,"reserved cast tiles");
    D3D12_HEAP_DESC heap_desc{UINT64(tiles)*D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,p,
        D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,D3D12_HEAP_FLAG_NONE};
    check(c.device->CreateHeap(&heap_desc,IID_PPV_ARGS(&heap)),"reserved cast heap");
    D3D12_TILED_RESOURCE_COORDINATE coordinate{}; D3D12_TILE_REGION_SIZE region{tiles,FALSE,0,0,0};
    D3D12_TILE_RANGE_FLAGS flag=D3D12_TILE_RANGE_FLAG_NONE; UINT offset=0;
    c.queue->UpdateTileMappings(texture.Get(),1,&coordinate,&region,heap.Get(),1,&flag,&offset,&tiles,D3D12_TILE_MAPPING_FLAG_NONE);
    heap.Reset();
  } else if (placed) {
    auto allocation = c.device->GetResourceAllocationInfo(0, 1, &creation_desc);
    require(allocation.SizeInBytes != UINT64_MAX, "placed allocation info");
    D3D12_HEAP_DESC hd{allocation.SizeInBytes, p, allocation.Alignment, D3D12_HEAP_FLAG_NONE};
    check(c.device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "placed heap");
    check(legacy_api ? c.device->CreatePlacedResource(heap.Get(),0,&creation_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture))
                 : c.device->CreatePlacedResource2(heap.Get(), 0, &d, D3D12_BARRIER_LAYOUT_COMMON,
                                         nullptr, format_count, formats, IID_PPV_ARGS(&texture)), "placed cast list");
    heap.Reset(); // The resource, descriptor and replay must retain the backing.
  } else {
    auto device=shared_creator ? shared_creator->device.Get() : c.device.Get();
    check(legacy_api ? c.device->CreateCommittedResource(&p,D3D12_HEAP_FLAG_NONE,&creation_desc,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture))
        : device->CreateCommittedResource3(&p, shared_creator ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COMMON,
        nullptr, nullptr, format_count, formats, IID_PPV_ARGS(&texture)), "committed cast list");
  }
  formats[1] = DXGI_FORMAT_UNKNOWN; // Caller list storage is not owned by the resource.
  formats[2] = DXGI_FORMAT_UNKNOWN;
  if (shared_creator) {
    shared_source=texture;
    HANDLE shared=nullptr;
    check(shared_creator->device->CreateSharedHandle(texture.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"cast shared handle");
    sharedMetadata(c.device.Get(),shared,base,target,alias_oracle ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_UNKNOWN);
    texture.Reset();
    const HRESULT opened=c.device->OpenSharedHandle(shared,IID_PPV_ARGS(&texture));
    CloseHandle(shared);
    check(opened,"cast import preserves explicit format list");
  }
  const UINT sub = 3; // slice1,mip1; both native and cast views must address the same bytes.
  D3D12_RESOURCE_DESC legacy = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows=0; UINT64 row_bytes=0, bytes=0;
  c.device->GetCopyableFootprints(&legacy, sub, 1, 0, &fp, &rows, &row_bytes, &bytes);
  const UINT columns = UINT(row_bytes / unit), count = columns * rows;
  auto upload = c.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  const UINT result_count = alias_oracle ? count*3 : writable ? count*2 : count + (bc ? 1 : 0);
  auto output = c.buffer(UINT64(result_count)*16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = c.buffer(UINT64(result_count)*16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> native_readback;
  const UINT64 native_bytes=bytes;
  if (writable) native_readback=c.buffer(native_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  std::vector<std::array<UINT,4>> expected(result_count);
  const bool snorm_oracle = legacy_api && base == DXGI_FORMAT_R16G16B16A16_SNORM;
  const bool bc_signed = base == DXGI_FORMAT_BC4_TYPELESS;
  void *mapped=nullptr; D3D12_RANGE none{};
  check(upload->Map(0, &none, &mapped), "map upload");
  std::memset(mapped, 0, size_t(bytes));
  for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
    auto &words=expected[y*columns+x];
    for (UINT k=0;k<channels;++k) words[k]=0x3e800000u+(y*columns+x)*0x10000u+k*0x100u;
    if (bc) words = base == DXGI_FORMAT_BC1_UNORM
        ? std::array<UINT,4>{0x07e0f800u,0,0,0} : std::array<UINT,4>{0x40u,0,0,0};
    if (!std::strcmp(scalar,"int")) words[1] |= 0x80000000u;
    auto destination = static_cast<unsigned char *>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+size_t(x)*unit;
    if (bc_signed) {
      words={UINT(-32),0,0,0};
      const UINT raw[2]={0x000020e0u,0};
      std::memcpy(destination,raw,sizeof(raw));
    } else if (snorm_oracle) {
      const int16_t raw[] = {1024,2048,-1024,-2048};
      for (UINT k=0;k<4;++k) words[k]=UINT(int(raw[k]));
      std::memcpy(destination,raw,sizeof(raw));
    } else std::memcpy(destination,words.data(),unit);
  }
  upload->Unmap(0, nullptr);
  if (bc) expected[count] = base == DXGI_FORMAT_BC1_UNORM
      ? std::array<UINT,4>{255,0,0,255} : std::array<UINT,4>{0,0,0,0};
  if (writable) for (UINT i=0;i<count;++i) {
    expected[i]={bc ? 0x07e0f800u : 12345678u,0,0,0};
    expected[count+i]={bc ? 0xf80007e0u : 0xdeadbeefu,0,0,0};
    if (alias_oracle) expected[count*2+i]={0x4321u,0x8765u,0xba98u,0xfedcu};
  }
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  hd.NumDescriptors=alias_oracle ? 3 : (bc || writable) ? 2 : 1; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> descriptors;
  check(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)), "descriptors");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=target;
  srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2DArray.MostDetailedMip=1; srv.Texture2DArray.MipLevels=1;
  srv.Texture2DArray.FirstArraySlice=1; srv.Texture2DArray.ArraySize=1;
  c.device->CreateShaderResourceView(texture.Get(), &srv, descriptors->GetCPUDescriptorHandleForHeapStart());
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu{}; D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu{};
  if (writable) {
    D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors=1;
    check(c.device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&rtv_heap)),"RTV heap");
    D3D12_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=target; rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv.Texture2DArray={1,1,1,0};
    c.device->CreateRenderTargetView(texture.Get(),&rtv,rtv_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=target; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uav.Texture2DArray={1,1,1,0};
    uav_cpu=descriptors->GetCPUDescriptorHandleForHeapStart(); uav_gpu=descriptors->GetGPUDescriptorHandleForHeapStart();
    UINT stride=c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    uav_cpu.ptr += stride; uav_gpu.ptr += stride;
    c.device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav,uav_cpu);
    if (alias_oracle) {
      uav.Format=DXGI_FORMAT_R16G16B16A16_UINT;
      auto alias_cpu=uav_cpu; alias_cpu.ptr += stride;
      c.device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav,alias_cpu);
    }
  }
  if (bc && !writable) {
    srv.Format = base;
    auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    c.device->CreateShaderResourceView(texture.Get(), &srv, handle);
  }
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,bc ? 2u : 1u,0,0,0};
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={1,&range};
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[2].Constants={0,0,1};
  D3D12_ROOT_SIGNATURE_DESC root_desc{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> root_blob,error;
  check(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error),"serialize root");
  ComPtr<ID3D12RootSignature> root;
  check(c.device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  char shader[1024];
  std::snprintf(shader,sizeof(shader),
    "Texture2DArray<%s%u> t:register(t0); RWByteAddressBuffer o:register(u0); cbuffer C:register(b0){uint width;} "
    "[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){uint4 v=uint4(0,0,0,0); "
    "v%s=%s(t.Load(int4(p.xy,0,0)));o.Store4((p.y*width+p.x)*16,v);}",
    scalar,channels,channels==1?".x":channels==2?".xy":"",std::strcmp(scalar,"uint")?"asuint":"uint");
  // uint2/uint4 constructors preserve bits; float and sint loads are explicitly bitcast.
  if (!std::strcmp(scalar,"uint")) {
    std::snprintf(shader,sizeof(shader),
      "Texture2DArray<uint%u> t:register(t0); RWByteAddressBuffer o:register(u0); cbuffer C:register(b0){uint width;} "
      "[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){uint4 v=uint4(0,0,0,0); "
      "v%s=t.Load(int4(p.xy,0,0));o.Store4((p.y*width+p.x)*16,v);}", channels,channels==1?".x":channels==2?".xy":"");
  }
  if (bc && !writable) {
    std::snprintf(shader,sizeof(shader),
      "Texture2DArray<uint%u> t:register(t0); Texture2DArray<float4> n:register(t1); "
      "RWByteAddressBuffer o:register(u0); cbuffer C:register(b0){uint width;} "
      "[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){uint4 v=uint4(0,0,0,0); "
      "v%s=t.Load(int4(p.xy,0,0));o.Store4((p.y*width+p.x)*16,v); "
      "if(p.x==0&&p.y==0)o.Store4(%u,uint4(round(n.Load(int4(0,0,0,0))*255)));}",
      channels,channels==2?".xy":"",count*16);
  }
  if (snorm_oracle) {
    std::snprintf(shader,sizeof(shader),
      "Texture2DArray<float4> t:register(t0); RWByteAddressBuffer o:register(u0); cbuffer C:register(b0){uint width;} "
      "[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){o.Store4((p.y*width+p.x)*16,"
      "uint4(int4(round(t.Load(int4(p.xy,0,0))*32767))));}");
  }
  if (bc_signed) {
    std::snprintf(shader,sizeof(shader),
      "Texture2DArray<float> t:register(t0); RWByteAddressBuffer o:register(u0); cbuffer C:register(b0){uint width;} "
      "[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){o.Store4((p.y*width+p.x)*16,"
      "uint4(uint(int(round(t.Load(int4(p.xy,0,0))*127))),0,0,0));}");
  }
  ComPtr<ID3DBlob> code;
  HRESULT compiled=D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&error);
  if (FAILED(compiled) && error) std::fwrite(error->GetBufferPointer(),1,error->GetBufferSize(),stderr);
  check(compiled,"independent HLSL compile");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get(); pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline; check(c.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"pipeline");
  ComPtr<ID3D12RootSignature> alias_root; ComPtr<ID3D12PipelineState> alias_pipeline;
  if (alias_oracle) {
    D3D12_DESCRIPTOR_RANGE alias_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0};
    params[0].DescriptorTable={1,&alias_range}; params[1].Descriptor.ShaderRegister=2;
    check(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error),"alias root serialize");
    check(c.device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&alias_root)),"alias root");
    const char alias_shader[]=
      "RWTexture2DArray<uint2> a:register(u0); RWTexture2DArray<uint4> b:register(u1); "
      "RWByteAddressBuffer o:register(u2); cbuffer C:register(b0){uint width;} "
      "[numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){"
      "a[int3(p.xy,0)]=uint2(0x87654321u,0xfedcba98u); DeviceMemoryBarrier();"
      "o.Store4((p.y*width+p.x)*16,b[int3(p.xy,0)]);}";
    compiled=D3DCompile(alias_shader,sizeof(alias_shader)-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&error);
    if (FAILED(compiled) && error) std::fwrite(error->GetBufferPointer(),1,error->GetBufferSize(),stderr);
    check(compiled,"alias HLSL compile");
    pd.pRootSignature=alias_root.Get(); pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    check(c.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&alias_pipeline)),"alias pipeline");
  }
  ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
  check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
  check(c.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)),"list");
  D3D12_TEXTURE_COPY_LOCATION dst{},src{}; dst.pResource=texture.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=sub;
  src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
  list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  if (writable) {
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const FLOAT value[4]={FLOAT(expected[0][0]),0,0,0};
    list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(),value,0,nullptr);
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  } else barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  ID3D12DescriptorHeap *heaps[]={descriptors.Get()}; list->SetDescriptorHeaps(1,heaps);
  list->SetComputeRootSignature(root.Get()); list->SetComputeRootDescriptorTable(0,descriptors->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()); list->SetComputeRoot32BitConstant(2,columns,0);
  list->Dispatch(columns,rows,1);
  if (writable) {
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT value[4]={expected[count][0],0,0,0};
    list->ClearUnorderedAccessViewUint(uav_gpu,uav_cpu,texture.Get(),value,0,nullptr);
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()+UINT64(count)*16);
    list->Dispatch(columns,rows,1);
  }
  if (alias_oracle) {
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(alias_pipeline.Get()); list->SetComputeRootSignature(alias_root.Get());
    list->SetComputeRootDescriptorTable(0,uav_gpu);
    list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress()+UINT64(count)*32);
    list->SetComputeRoot32BitConstant(2,columns,0);
    list->Dispatch(columns,rows,1);
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(),0,output.Get(),0,UINT64(result_count)*16);
  // Return to initial states so the closed recording is genuinely replayable.
  barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (writable) {
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION copy{}; copy.pResource=native_readback.Get();
    copy.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; copy.PlacedFootprint=fp;
    list->CopyTextureRegion(&copy,0,0,0,&dst,nullptr);
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  } else barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
  check(list->Close(),"close");
  const UINT replays=sparse && !writable ? 3 : 2;
  for (UINT replay=0;replay<replays;++replay) {
    if (replay) {
      check(upload->Map(0,&none,&mapped),"update upload between replays");
      for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
        auto &words = expected[y*columns+x];
        if (bc && !writable) words[0] = base == DXGI_FORMAT_BC1_UNORM ? 0xf80007e0u : 0x40u;
        else if (!snorm_oracle && !writable && !bc_signed) for (UINT k=0;k<channels;++k) words[k] ^= 0x00008000u;
        auto destination = static_cast<unsigned char *>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+size_t(x)*unit;
        if (bc_signed) {
          words[0]=UINT(-64);
          const UINT raw[2]={0x000020c0u,0};
          std::memcpy(destination,raw,sizeof(raw));
        } else if (snorm_oracle) {
          int16_t raw[4];
          for (UINT k=0;k<4;++k) { words[k] += 512; raw[k]=int16_t(words[k]); }
          std::memcpy(destination,raw,sizeof(raw));
        } else std::memcpy(destination,words.data(),unit);
      }
      upload->Unmap(0,nullptr);
      if (base == DXGI_FORMAT_BC1_UNORM && !writable) expected[count]={0,255,0,255};
      if (sparse && !writable) {
        UINT tiles=0; c.device->GetResourceTiling(texture.Get(),&tiles,nullptr,nullptr,nullptr,0,nullptr);
        D3D12_TILED_RESOURCE_COORDINATE coordinate{}; D3D12_TILE_REGION_SIZE region{tiles,FALSE,0,0,0};
        D3D12_TILE_RANGE_FLAGS flag=replay==1 ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE;
        UINT offset=0;
        if (replay==2) {
          D3D12_HEAP_DESC desc{UINT64(tiles)*D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,p,
              D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,D3D12_HEAP_FLAG_NONE};
          check(c.device->CreateHeap(&desc,IID_PPV_ARGS(&heap)),"reserved cast remap heap");
        }
        c.queue->UpdateTileMappings(texture.Get(),1,&coordinate,&region,heap.Get(),1,&flag,&offset,&tiles,D3D12_TILE_MAPPING_FLAG_NONE);
        heap.Reset();
        if (replay==1) {
          for (auto &words : expected) words={0,0,0,0};
        }
      }
    }
    c.submit(list.Get()); D3D12_RANGE read{0,size_t(result_count)*16};
    check(readback->Map(0,&read,&mapped),"map result");
    if (std::memcmp(mapped,expected.data(),size_t(result_count)*16)) {
      const auto *actual=static_cast<const UINT *>(mapped);
      for (UINT index=0;index<result_count;++index) for (UINT word=0;word<4;++word)
        if (actual[index*4+word]!=expected[index][word]) {
          const char *phase=!writable ? "SRV_upload" : index/count==0 ? "RTV_clear_then_SRV"
              : index/count==1 ? "UAV_clear_then_SRV" : "UAV_alias_same_dispatch";
          std::fprintf(stderr,"RELAXED_CASTING_MISMATCH base=%u target=%u placed=%u writable=%u sparse=%u "
              "replay=%u element=%u segment=%u phase=%s word=%u actual=%08x expected=%08x\n",
              base,target,placed,writable,sparse,replay,index,index/count,phase,word,
              actual[index*4+word],expected[index][word]);
          require(false,"shader exact reinterpretation words and native BC sample");
        }
    }
    readback->Unmap(0,&none);
    if (writable) {
      D3D12_RANGE native_range{0,SIZE_T(native_bytes)};
      check(native_readback->Map(0,&native_range,&mapped),"map native cast copy");
      const std::array<UINT,4> alias_written{0x87654321u,0xfedcba98u,0,0};
      for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
        const auto raw=static_cast<const unsigned char *>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+size_t(x)*unit;
        const auto &native_expected=alias_oracle ? alias_written : expected[count+y*columns+x];
        require(!std::memcmp(raw,native_expected.data(),unit),"cast write native GPU copy");
      }
      native_readback->Unmap(0,&none);
    }
  }
  if (shared_creator) {
    auto &creator=*shared_creator;
    auto shared_readback=creator.buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12CommandAllocator> shared_allocator; ComPtr<ID3D12GraphicsCommandList> shared_list;
    check(creator.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&shared_allocator)),"shared producer allocator");
    check(creator.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,shared_allocator.Get(),nullptr,IID_PPV_ARGS(&shared_list)),"shared producer list");
    barrier(shared_list.Get(),shared_source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{},destination{};
    source.pResource=shared_source.Get(); source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; source.SubresourceIndex=sub;
    destination.pResource=shared_readback.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=fp;
    shared_list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    barrier(shared_list.Get(),shared_source.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    check(shared_list->Close(),"shared producer close"); creator.submit(shared_list.Get());
    D3D12_RANGE range{0,SIZE_T(bytes)}; check(shared_readback->Map(0,&range,&mapped),"shared producer GPU copy readback");
    const std::array<UINT,4> written{0x87654321u,0xfedcba98u,0,0};
    for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
      const auto raw=static_cast<const unsigned char *>(mapped)+fp.Offset+size_t(y)*fp.Footprint.RowPitch+size_t(x)*unit;
      const auto &words=alias_oracle ? written : expected[(writable ? count : 0)+y*columns+x];
      require(!std::memcmp(raw,words.data(),unit),"shared creator receives imported view GPU writes");
    }
    shared_readback->Unmap(0,&none);
  }
  std::printf("RELAXED_CASTING_CASE base=%u target=%u bc=%u placed=%u legacy=%u writable=%u sparse=%u shared=%u mip=1 slice=1 words=exact replay=%u\n",base,target,bc,placed,legacy_api,writable,sparse,shared_creator!=nullptr,replays);
}
void volume(Context &c, bool placed, Context *shared_creator=nullptr,
            D3D12_TEXTURE_LAYOUT layout=D3D12_TEXTURE_LAYOUT_UNKNOWN,bool writable=false,bool wide=false) {
  D3D12_HEAP_PROPERTIES properties{}; properties.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  desc.Width=256; desc.Height=128; desc.DepthOrArraySize=64;
  desc.MipLevels=2; desc.Format=wide ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_BC1_UNORM; desc.SampleDesc.Count=1;
  desc.Layout=layout;
  if (writable) desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_RESOURCE_DESC1 extended{}; extended.Dimension=desc.Dimension; extended.Width=desc.Width;
  extended.Height=desc.Height; extended.DepthOrArraySize=desc.DepthOrArraySize; extended.MipLevels=desc.MipLevels;
  extended.Format=desc.Format; extended.SampleDesc=desc.SampleDesc;
  extended.Layout=desc.Layout; extended.Flags=desc.Flags;
  const DXGI_FORMAT target=wide ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32_UINT;
  const DXGI_FORMAT alias_format=wide ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R16G16B16A16_UINT;
  const UINT unit=wide ? 16 : 8;
  DXGI_FORMAT formats[]{desc.Format,target,alias_format};
  const UINT format_count=writable ? 3 : 2;
  auto small=desc; small.Alignment=D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  require(c.device->GetResourceAllocationInfo(0,1,&small).SizeInBytes==UINT64_MAX,"BC volume rejects 4KB allocation alignment");
  auto small_extended=extended; small_extended.Alignment=D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
  ComPtr<ID3D12Resource> invalid;
  require(c.device->CreateCommittedResource3(&properties,D3D12_HEAP_FLAG_NONE,&small_extended,
      D3D12_BARRIER_LAYOUT_COMMON,nullptr,nullptr,format_count,formats,IID_PPV_ARGS(&invalid))==E_INVALIDARG,
      "BC volume rejects 4KB creation alignment");
  ComPtr<ID3D12Resource> texture,shared_source; ComPtr<ID3D12Heap> heap;
  if (placed) {
    auto allocation=c.device->GetResourceAllocationInfo(0,1,&desc);
    require(allocation.SizeInBytes!=UINT64_MAX,"ordinary volume allocation");
    D3D12_HEAP_DESC hd{allocation.SizeInBytes,properties,allocation.Alignment,D3D12_HEAP_FLAG_NONE};
    check(c.device->CreateHeap(&hd,IID_PPV_ARGS(&heap)),"ordinary volume heap");
    check(c.device->CreatePlacedResource2(heap.Get(),0,&extended,D3D12_BARRIER_LAYOUT_COMMON,nullptr,
        format_count,formats,IID_PPV_ARGS(&texture)),"placed BC volume cast list");
    heap.Reset();
  } else check((shared_creator ? shared_creator->device.Get() : c.device.Get())->CreateCommittedResource3(
      &properties,shared_creator ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE,&extended,
      D3D12_BARRIER_LAYOUT_COMMON,nullptr,nullptr,format_count,formats,IID_PPV_ARGS(&texture)),"committed BC volume cast list");
  formats[1]=DXGI_FORMAT_UNKNOWN;
  if (shared_creator) {
    shared_source=texture; HANDLE shared=nullptr;
    check(shared_creator->device->CreateSharedHandle(texture.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"volume shared handle");
    sharedMetadata(c.device.Get(),shared,desc.Format,target,writable ? alias_format : DXGI_FORMAT_UNKNOWN);
    texture.Reset();
    const HRESULT opened=c.device->OpenSharedHandle(shared,IID_PPV_ARGS(&texture));
    CloseHandle(shared); check(opened,"volume imported list");
  }
  require(texture->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
      texture->GetDesc().Layout==layout,"ordinary volume public descriptor");
  D3D12_HEAP_PROPERTIES actual{}; D3D12_HEAP_FLAGS flags{};
  check(texture->GetHeapProperties(&actual,&flags),"ordinary volume public heap properties");
  require(actual.Type==D3D12_HEAP_TYPE_DEFAULT,"ordinary volume retains heap contract");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows=0; UINT64 row_bytes=0,bytes=0;
  c.device->GetCopyableFootprints(&desc,1,1,0,&fp,&rows,&row_bytes,&bytes);
  const UINT columns=UINT(row_bytes/unit),depth=fp.Footprint.Depth,count=columns*rows*depth;
  const UINT64 texel_result_bytes=UINT64(count)*(writable ? 48 : 32);
  const UINT64 result_bytes=texel_result_bytes+32;
  auto upload=c.buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output=c.buffer(result_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback=c.buffer(result_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  auto copied=c.buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=writable ? 4 : 2;
  hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> descriptors; check(c.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&descriptors)),"volume descriptors");
  auto handle=descriptors->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=target;
  srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture3D.MostDetailedMip=1; srv.Texture3D.MipLevels=1;
  c.device->CreateShaderResourceView(texture.Get(),&srv,handle);
  handle.ptr+=c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  srv.Format=desc.Format; c.device->CreateShaderResourceView(texture.Get(),&srv,handle);
  auto write_gpu=descriptors->GetGPUDescriptorHandleForHeapStart();
  if (writable) {
    const UINT stride=c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    handle.ptr+=stride; write_gpu.ptr+=2*stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=target;
    uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D; uav.Texture3D={1,0,depth};
    c.device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav,handle);
    handle.ptr+=stride; uav.Format=alias_format;
    c.device->CreateUnorderedAccessView(texture.Get(),nullptr,&uav,handle);
  }
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
  D3D12_ROOT_PARAMETER params[3]{}; params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable={1,&range}; params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[2].Constants={0,0,3};
  D3D12_ROOT_SIGNATURE_DESC rd{3,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob,error; check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"volume root blob");
  ComPtr<ID3D12RootSignature> root; check(c.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"volume root");
  char shader[1024]; std::snprintf(shader,sizeof(shader),
    "Texture3D<uint%u> b:register(t0); Texture3D<float4> n:register(t1); RWByteAddressBuffer o:register(u0); "
    "cbuffer C:register(b0){uint w;uint h;} [numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){"
    "uint i=(p.z*h+p.y)*w+p.x; o.Store4(i*32,uint4(b.Load(int4(p,0))%s));"
    "o.Store4(i*32+16,uint4(round(n.Load(int4(p.x*4,p.y*4,p.z,0))*255)));"
    "if(i==0){uint a,c,d,e; b.GetDimensions(0,a,c,d,e);o.Store4(%llu,uint4(a,c,d,e));"
    "n.GetDimensions(0,a,c,d,e);o.Store4(%llu,uint4(a,c,d,e));}}",
    wide ? 4 : 2,wide ? "" : ",0,0",
    static_cast<unsigned long long>(texel_result_bytes),static_cast<unsigned long long>(texel_result_bytes+16));
  HRESULT compiled=D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&blob,&error);
  if (FAILED(compiled) && error) std::fwrite(error->GetBufferPointer(),1,error->GetBufferSize(),stderr);
  check(compiled,"independent volume HLSL compile");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature=root.Get(); pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pipeline; check(c.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"volume pipeline");
  ComPtr<ID3D12RootSignature> write_root; ComPtr<ID3D12PipelineState> write_pipeline;
  if (writable) {
    D3D12_DESCRIPTOR_RANGE write_range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0};
    params[0].DescriptorTable={1,&write_range}; params[1].Descriptor.ShaderRegister=2;
    check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"volume write root blob");
    check(c.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&write_root)),"volume write root");
    char write_shader[1024]; std::snprintf(write_shader,sizeof(write_shader),
      "RWTexture3D<uint%u> a:register(u0); RWTexture3D<%s4> b:register(u1); RWByteAddressBuffer o:register(u2); "
      "cbuffer C:register(b0){uint w;uint h;uint d;} [numthreads(1,1,1)] void main(uint3 p:SV_DispatchThreadID){"
      "a[p]=%s; DeviceMemoryBarrier();"
      "o.Store4(w*h*d*32+((p.z*h+p.y)*w+p.x)*16,asuint(b[p]));}",wide ? 4 : 2,wide ? "int" : "uint",
      wide ? "uint4(0x40u,0,0,0x87654321u)" : "uint2(0xf80007e0u,0)");
    compiled=D3DCompile(write_shader,std::strlen(write_shader),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&blob,&error);
    if (FAILED(compiled) && error) std::fwrite(error->GetBufferPointer(),1,error->GetBufferSize(),stderr);
    check(compiled,"independent volume dual UAV HLSL compile");
    pd.pRootSignature=write_root.Get(); pd.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
    check(c.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&write_pipeline)),"volume dual UAV pipeline");
  }
  ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
  check(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"volume allocator");
  check(c.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)),"volume list");
  barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
  dst.pResource=texture.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=1;
  list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  ID3D12DescriptorHeap *heaps[]{descriptors.Get()}; list->SetDescriptorHeaps(1,heaps);
  const UINT sizes[]{columns,rows,depth};
  if (writable) {
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(write_pipeline.Get()); list->SetComputeRootSignature(write_root.Get());
    list->SetComputeRootDescriptorTable(0,write_gpu); list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
    list->SetComputeRoot32BitConstants(2,3,sizes,0); list->Dispatch(columns,rows,depth);
    barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetPipelineState(pipeline.Get());
  } else barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->SetComputeRootSignature(root.Get()); list->SetComputeRootDescriptorTable(0,descriptors->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
  list->SetComputeRoot32BitConstants(2,3,sizes,0); list->Dispatch(columns,rows,depth);
  barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(),0,output.Get(),0,result_bytes);
  barrier(list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION copy{}; copy.pResource=copied.Get(); copy.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; copy.PlacedFootprint=fp;
  list->CopyTextureRegion(&copy,0,0,0,&dst,nullptr);
  barrier(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
  check(list->Close(),"volume close");
  for (UINT replay=0;replay<2;++replay) {
    void *mapped=nullptr; D3D12_RANGE none{}; check(upload->Map(0,&none,&mapped),"volume upload map");
    for (UINT z=0;z<depth;++z) for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
      // BC7 mode6 zero endpoints decode black regardless of these independent selector bits.
      const UINT raw[4]{wide ? 0x40u : ((x+y+z+replay)&1) ? 0xf80007e0u : 0x07e0f800u,0,0,
          wide ? (z*rows+y)*columns+x+replay : 0};
      std::memcpy(static_cast<BYTE *>(mapped)+fp.Offset+(size_t(z)*rows+y)*fp.Footprint.RowPitch+size_t(x)*unit,raw,unit);
    }
    upload->Unmap(0,nullptr); c.submit(list.Get());
    D3D12_RANGE range{0,SIZE_T(result_bytes)}; check(readback->Map(0,&range,&mapped),"volume shader readback");
    const auto *words=static_cast<const UINT *>(mapped);
    const auto *dimensions=words+texel_result_bytes/4;
    const UINT expected_dimensions[]{columns,rows,depth,1,columns*4,rows*4,depth,1};
    for (UINT word=0;word<8;++word) if (dimensions[word]!=expected_dimensions[word]) {
      std::fprintf(stderr,"RELAXED_CASTING_VOLUME_DIMENSIONS word=%u actual=%u expected=%u\n",
          word,dimensions[word],expected_dimensions[word]);
      require(false,"ordinary volume native and blockcast dimensions");
    }
    for (UINT z=0;z<depth;++z) for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
      const bool green=writable || ((x+y+z+replay)&1)!=0;
      const UINT expected[]{wide ? 0x40u : green ? 0xf80007e0u : 0x07e0f800u,0,0,
          wide ? writable ? 0x87654321u : (z*rows+y)*columns+x+replay : 0,
          wide ? 0u : green ? 0u : 255u,wide ? 0u : green ? 255u : 0u,0,wide ? 0u : 255u};
      const auto *actual=words+((size_t(z)*rows+y)*columns+x)*8;
      for (UINT word=0;word<8;++word) if (actual[word]!=expected[word]) {
        std::fprintf(stderr,"RELAXED_CASTING_VOLUME_MISMATCH base=%u unit=%u placed=%u shared=%u layout=%u writable=%u "
            "replay=%u x=%u y=%u z=%u phase=%s word=%u actual=%08x expected=%08x\n",
            desc.Format,unit,placed,shared_creator!=nullptr,layout,writable,replay,x,y,z,
            word<4 ? "blockcast_SRV" : "native_BC_SRV",word,actual[word],expected[word]);
        void *copy_mapped=nullptr; D3D12_RANGE copy_range{0,SIZE_T(bytes)};
        check(copied->Map(0,&copy_range,&copy_mapped),"volume mismatch copy readback");
        UINT raw[4]{};
        std::memcpy(raw,static_cast<const BYTE *>(copy_mapped)+fp.Offset+
            (size_t(z)*rows+y)*fp.Footprint.RowPitch+size_t(x)*unit,unit);
        std::fprintf(stderr,"RELAXED_CASTING_VOLUME_READBACK blockcast=%08x,%08x,%08x,%08x "
            "nativeBC=%08x,%08x,%08x,%08x nativecopy=%08x,%08x,%08x,%08x\n",
            actual[0],actual[1],actual[2],actual[3],actual[4],actual[5],actual[6],actual[7],
            raw[0],raw[1],raw[2],raw[3]);
        copied->Unmap(0,&none);
        require(false,"ordinary volume native and blockcast exact");
      }
      if (writable) {
        const UINT alias[]{wide ? 0x40u : 0x07e0u,wide ? 0u : 0xf800u,0,wide ? 0x87654321u : 0};
        const auto *actual=words+size_t(count)*8+((size_t(z)*rows+y)*columns+x)*4;
        for (UINT word=0;word<4;++word) if (actual[word]!=alias[word]) {
          std::fprintf(stderr,"RELAXED_CASTING_VOLUME_MISMATCH base=%u unit=%u placed=%u shared=%u layout=%u writable=1 "
              "replay=%u x=%u y=%u z=%u phase=UAV_alias_same_dispatch word=%u actual=%08x expected=%08x\n",
              desc.Format,unit,placed,shared_creator!=nullptr,layout,replay,x,y,z,word,actual[word],alias[word]);
          require(false,"ordinary volume dual UAV same-dispatch exact");
        }
      }
    }
    readback->Unmap(0,&none); range.End=SIZE_T(bytes); check(copied->Map(0,&range,&mapped),"volume native copy readback");
    for (UINT z=0;z<depth;++z) for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
      const UINT expected[]{wide ? 0x40u : (writable || ((x+y+z+replay)&1)) ? 0xf80007e0u : 0x07e0f800u,0,0,
          wide ? writable ? 0x87654321u : (z*rows+y)*columns+x+replay : 0};
      require(!std::memcmp(static_cast<const BYTE *>(mapped)+fp.Offset+(size_t(z)*rows+y)*fp.Footprint.RowPitch+size_t(x)*unit,
          expected,unit),"ordinary BC volume native copy exact");
    }
    copied->Unmap(0,&none);
  }
  if (shared_creator) {
    auto &creator=*shared_creator; auto read=creator.buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> command;
    check(creator.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"volume shared creator allocator");
    check(creator.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command)),"volume shared creator list");
    barrier(command.Get(),shared_source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{},destination{}; source.pResource=shared_source.Get();
    source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; source.SubresourceIndex=1;
    destination.pResource=read.Get(); destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=fp;
    command->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    barrier(command.Get(),shared_source.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    check(command->Close(),"volume shared creator close"); creator.submit(command.Get());
    void *mapped=nullptr; D3D12_RANGE range{0,SIZE_T(bytes)},none{}; check(read->Map(0,&range,&mapped),"volume shared creator readback");
    for (UINT z=0;z<depth;++z) for (UINT y=0;y<rows;++y) for (UINT x=0;x<columns;++x) {
      const UINT expected[]{wide ? 0x40u : (writable || ((x+y+z+1)&1)) ? 0xf80007e0u : 0x07e0f800u,0,0,
          wide ? writable ? 0x87654321u : (z*rows+y)*columns+x+1 : 0};
      require(!std::memcmp(static_cast<const BYTE *>(mapped)+fp.Offset+(size_t(z)*rows+y)*fp.Footprint.RowPitch+size_t(x)*unit,
          expected,unit),"shared volume canonical GPU publication");
    }
    read->Unmap(0,&none);
  }
  std::printf("RELAXED_CASTING_VOLUME_CASE base=%u unit=%u placed=%u shared=%u layout=%u writable=%u alias=%s mip=1 depth=32 words=exact nativeBC=exact nativecopy=exact replay=2\n",
      desc.Format,unit,placed,shared_creator!=nullptr,layout,writable,writable ? "same-dispatch-exact" : "none");
}

void negatives(Context &c) {
  D3D12_RESOURCE_DESC1 d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=d.Height=16;
  d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Format=DXGI_FORMAT_R32G32_UINT;
  D3D12_HEAP_PROPERTIES p{}; p.Type=D3D12_HEAP_TYPE_DEFAULT;
  for (auto forbidden:{DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_NV12,DXGI_FORMAT_R32_UINT}) {
    ComPtr<ID3D12Resource> r;
    require(c.device->CreateCommittedResource3(&p,D3D12_HEAP_FLAG_NONE,&d,D3D12_BARRIER_LAYOUT_COPY_DEST,
        nullptr,nullptr,1,&forbidden,IID_PPV_ARGS(&r))==E_INVALIDARG,"forbidden cast list");
  }
  std::puts("RELAXED_CASTING_NEGATIVES nonBC_to_BC=reject planar=reject unequal_unit=reject");
}
}
int main(int argc, char **argv) {
  const auto backend=reinterpret_cast<const char*(WINAPI*)()>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  require(backend && !std::strcmp(backend(),"owned-graphics-d3d12"),"actual owned backend identity");
  std::printf("RELAXED_CASTING_BACKEND id=%s\n",backend());
  if (argc==2 && !std::strcmp(argv[1],"--shared")) {
    Context creator,consumer;
    capabilities(consumer);
    run(consumer,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_UINT,4,1,"uint",false,false,false,false,false,&creator);
    run(consumer,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_UINT,4,1,"uint",false,false,false,true,false,&creator);
    run(consumer,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_R32G32_UINT,8,2,"uint",true,false,false,true,false,&creator);
    volume(consumer,false,&creator,D3D12_TEXTURE_LAYOUT_UNKNOWN,true);
    volume(consumer,false,&creator,D3D12_TEXTURE_LAYOUT_UNKNOWN,true,true);
    std::puts("RELAXED_CASTING_SHARED_PASS cases=5 GPU=exact");
    return 0;
  }
  if (argc==2 && !std::strcmp(argv[1],"--volume")) {
    Context c; capabilities(c); volume(c,false); volume(c,true);
    volume(c,false,nullptr,D3D12_TEXTURE_LAYOUT_UNKNOWN,true); volume(c,true,nullptr,D3D12_TEXTURE_LAYOUT_UNKNOWN,true);
    volume(c,false,nullptr,D3D12_TEXTURE_LAYOUT_UNKNOWN,false,true);
    volume(c,true,nullptr,D3D12_TEXTURE_LAYOUT_UNKNOWN,false,true);
    volume(c,false,nullptr,D3D12_TEXTURE_LAYOUT_UNKNOWN,true,true);
    volume(c,true,nullptr,D3D12_TEXTURE_LAYOUT_UNKNOWN,true,true);
    std::puts("RELAXED_CASTING_VOLUME_PASS cases=8 GPU=exact");
    return 0;
  }
  if (argc==2 && !std::strcmp(argv[1],"--volume-standard")) {
    Context c; capabilities(c); volume(c,false,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE);
    volume(c,true,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE);
    volume(c,false,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE,true);
    volume(c,true,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE,true);
    volume(c,false,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE,false,true);
    volume(c,true,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE,false,true);
    volume(c,false,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE,true,true);
    volume(c,true,nullptr,D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE,true,true);
    std::puts("RELAXED_CASTING_VOLUME_STANDARD_PASS cases=8 GPU=exact");
    return 0;
  }
  require(argc==1,"known fixture mode");
  Context c; capabilities(c); negatives(c);
  for (bool placed:{false,true}) {
    run(c,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_UINT,4,1,"uint",false,placed);
    run(c,DXGI_FORMAT_R16G16B16A16_UNORM,DXGI_FORMAT_R32G32_SINT,8,2,"int",false,placed);
    run(c,DXGI_FORMAT_R32G32B32A32_UINT,DXGI_FORMAT_R32G32B32A32_FLOAT,16,4,"float",false,placed);
    run(c,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_R32G32_UINT,8,2,"uint",true,placed);
    run(c,DXGI_FORMAT_BC7_UNORM,DXGI_FORMAT_R32G32B32A32_UINT,16,4,"uint",true,placed);
    run(c,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32_FLOAT,4,1,"float",false,placed,true);
    run(c,DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R32_UINT,4,1,"uint",false,placed,true);
    run(c,DXGI_FORMAT_R16G16B16A16_SNORM,DXGI_FORMAT_R16G16B16A16_SNORM,8,4,"float",false,placed,true);
    run(c,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_UINT,4,1,"uint",false,placed,false,true);
    run(c,DXGI_FORMAT_BC4_TYPELESS,DXGI_FORMAT_BC4_SNORM,8,1,"float",false,placed,true);
    run(c,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_R32G32_UINT,8,2,"uint",true,placed,false,true);
  }
  run(c,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_R32G32_UINT,8,2,"uint",true,false,false,false,true);
  run(c,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_R32G32_UINT,8,2,"uint",true,false,false,true,true);
  std::puts("RELAXED_CASTING_PASS cases=24 GPU=exact");
}
