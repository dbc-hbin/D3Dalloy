#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
static void fail(const char* at,HRESULT hr=E_FAIL) { std::fprintf(stderr,"OWNED_D3D12_PACKED_CUBE_FAIL %s hr=%08lx\n",at,static_cast<unsigned long>(hr)); ExitProcess(1); }
static void check(HRESULT hr,const char* at) { if(FAILED(hr)) fail(at,hr); }
static void require(bool value,const char* at) { if(!value) fail(at); }
static D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) { D3D12_HEAP_PROPERTIES h{}; h.Type=type; h.CreationNodeMask=h.VisibleNodeMask=1; return h; }
static D3D12_RESOURCE_DESC buffer(UINT64 bytes,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) { D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=d.DepthOrArraySize=d.MipLevels=1; d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags=flags; return d; }
static void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b); }
int main() {
  HMODULE dll=GetModuleHandleW(L"d3d12.dll"); auto backend=reinterpret_cast<const char*(WINAPI*)()>(GetProcAddress(dll,"OwnedGraphicsBackendId"));
  require(backend&&!std::strcmp(backend(),"owned-graphics-d3d12"),"backend"); std::printf("OWNED_D3D12_PACKED_CUBE_BACKEND id=%s\n",backend());
  const char* hlsl=R"(
    TextureCube<float4> first : register(t0);
    TextureCubeArray<float4> both : register(t1);
    SamplerState pointSampler : register(s0);
    RWByteAddressBuffer result : register(u0);
    [numthreads(1,1,1)] void main() {
      float4 a=first.SampleLevel(pointSampler,float3(0,0,1),0);
      float4 b=both.SampleLevel(pointSampler,float4(0,0,1,1),0);
      float4 clamped=both.SampleLevel(pointSampler,float4(0,0,1,11),0);
      result.Store4(0,uint4(round(a.r*255),round(b.g*255),round(clamped.g*255),round(b.a*255)));
    }
  )";
  ComPtr<ID3DBlob> shader,errors;
  HRESULT compiled=D3DCompile(hlsl,std::strlen(hlsl),"packed-cubearray",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS,0,&shader,&errors);
  if(FAILED(compiled)&&errors) std::fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr);
  check(compiled,"compile shader");
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> dev;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory"); check(factory->EnumAdapters1(0,&adapter),"adapter");
  check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)),"device");
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{}; support.Format=DXGI_FORMAT_R8G8_B8G8_UNORM;
  check(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)),"format support");
  require((support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURECUBE) && (support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE),"cube and sample caps");
  auto dh=heap(D3D12_HEAP_TYPE_DEFAULT); auto uh=heap(D3D12_HEAP_TYPE_UPLOAD); auto rh=heap(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=4; td.Height=4; td.DepthOrArraySize=12; td.MipLevels=1; td.Format=DXGI_FORMAT_R8G8_B8G8_UNORM; td.SampleDesc.Count=1;
  ComPtr<ID3D12Resource> texture; check(dev->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)),"texture array");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[12]{}; UINT rowcounts[12]{}; UINT64 rowbytes[12]{},total=0;
  dev->GetCopyableFootprints(&td,0,12,0,footprints,rowcounts,rowbytes,&total);
  require(total && rowcounts[11]==4 && total>=footprints[11].Offset+footprints[11].Footprint.RowPitch*3+rowbytes[11],"footprints");
  auto ub=buffer(total); ComPtr<ID3D12Resource> upload;
  check(dev->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&ub,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"upload");
  BYTE* data=nullptr; check(upload->Map(0,nullptr,reinterpret_cast<void**>(&data)),"map upload");
  for (UINT slice=0;slice<12;++slice) {
    require(rowcounts[slice]==4 && rowbytes[slice]==8 && footprints[slice].Footprint.RowPitch>=8,"packed footprint");
    BYTE* base=data+footprints[slice].Offset;
    // Pair of pixels: R,G0,B,G1. Each face of cube 0 has R=77, cube 1 G0=120.
    const BYTE r=slice<6?77:13, g=slice<6?20:120;
    for (UINT y=0;y<4;++y) for (UINT pair=0;pair<2;++pair) {
      BYTE* pixel=base+y*footprints[slice].Footprint.RowPitch+pair*4;
      pixel[0]=r; pixel[1]=g; pixel[2]=60; pixel[3]=slice<6?40:200;
    }
  }
  upload->Unmap(0,nullptr);
  D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=2; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> views; check(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"srv heap");
  auto first=views->GetCPUDescriptorHandleForHeapStart(); D3D12_CPU_DESCRIPTOR_HANDLE second{first.ptr+dev->GetDescriptorHandleIncrementSize(hd.Type)};
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=td.Format; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE; srv.TextureCube.MipLevels=1;
  dev->CreateShaderResourceView(texture.Get(),&srv,first);
  srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; srv.TextureCubeArray.MipLevels=1; srv.TextureCubeArray.NumCubes=2;
  dev->CreateShaderResourceView(texture.Get(),&srv,second);
  hd={}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; hd.NumDescriptors=1; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> samplers; check(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&samplers)),"sampler heap");
  D3D12_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT; sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP; sampler.MaxLOD=D3D12_FLOAT32_MAX;
  dev->CreateSampler(&sampler,samplers->GetCPUDescriptorHandleForHeapStart());
  D3D12_DESCRIPTOR_RANGE ranges[2]{}; ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors=2; ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; ranges[1].NumDescriptors=1;
  D3D12_ROOT_PARAMETER rp[3]{}; rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable={1,&ranges[0]}; rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
  rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable={1,&ranges[1]}; rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
  rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; rp[2].Descriptor.ShaderRegister=0; rp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd{3,rp,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE}; ComPtr<ID3DBlob> rootbytes,error;
  check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&rootbytes,&error),"root serialize");
  ComPtr<ID3D12RootSignature> root; check(dev->CreateRootSignature(0,rootbytes->GetBufferPointer(),rootbytes->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  D3D12_COMPUTE_PIPELINE_STATE_DESC ps{}; ps.pRootSignature=root.Get(); ps.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
  ComPtr<ID3D12PipelineState> pso; check(dev->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pso)),"pso");
  ComPtr<ID3D12Resource> output,readback; auto ob=buffer(16,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  check(dev->CreateCommittedResource(&dh,D3D12_HEAP_FLAG_NONE,&ob,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)),"output");
  ob.Flags=D3D12_RESOURCE_FLAG_NONE;
  check(dev->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&ob,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
  D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
  check(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue"); check(dev->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)),"allocator");
  check(dev->CreateCommandList(0,qd.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&list)),"list"); check(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
  for (UINT slice=0;slice<12;++slice) {
    D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=footprints[slice];
    dst.pResource=texture.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=slice;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  }
  transition(list.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pso.Get()); ID3D12DescriptorHeap* active[]={views.Get(),samplers.Get()}; list->SetDescriptorHeaps(2,active);
  list->SetComputeRootDescriptorTable(0,views->GetGPUDescriptorHandleForHeapStart()); list->SetComputeRootDescriptorTable(1,samplers->GetGPUDescriptorHandleForHeapStart());
  list->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress()); list->Dispatch(1,1,1);
  transition(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE); list->CopyResource(readback.Get(),output.Get());
  check(list->Close(),"close"); ID3D12CommandList* submit[]={list.Get()}; queue->ExecuteCommandLists(1,submit);
  check(queue->Signal(fence.Get(),1),"signal"); HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); require(event!=nullptr,"event");
  check(fence->SetEventOnCompletion(1,event),"completion"); require(WaitForSingleObject(event,15000)==WAIT_OBJECT_0,"wait"); CloseHandle(event);
  check(dev->GetDeviceRemovedReason(),"device removed"); UINT32* values=nullptr; check(readback->Map(0,nullptr,reinterpret_cast<void**>(&values)),"map output");
  std::printf("OWNED_D3D12_PACKED_CUBE_RESULT %u %u %u %u\n",values[0],values[1],values[2],values[3]);
  require(values[0]==77 && values[1]==120 && values[2]==120 && values[3]==255,"cube samples / clamp / alpha");
  readback->Unmap(0,nullptr); std::printf("OWNED_D3D12_PACKED_CUBE_PASS\n");
}
