#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr,"OWNED_D3D12_PIPELINE_CACHE_FAIL step=%s hr=0x%08lx\n",step,static_cast<unsigned long>(hr));
  std::exit(1);
}
void check(HRESULT hr,const char *step) { if(FAILED(hr))fail(step,hr); }

std::vector<uint8_t> readFile(const wchar_t *path) {
  HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)fail("open shader",HRESULT_FROM_WIN32(GetLastError()));
  LARGE_INTEGER size{};
  if(!GetFileSizeEx(file,&size)||size.QuadPart<=0||size.QuadPart>16*1024*1024)fail("shader size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));DWORD count=0;
  if(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size())fail("read shader");
  CloseHandle(file);return bytes;
}

ComPtr<ID3D12RootSignature> rootSignature(ID3D12Device *device,UINT constants) {
  D3D12_ROOT_PARAMETER params[2]{};
  params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues=constants;
  params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=params;
  ComPtr<ID3DBlob> bytes,error;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error),"serialize root");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&root)),"create root");
  return root;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device *device,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{};heap.Type=type;heap.CreationNodeMask=heap.VisibleNodeMask=1;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=4;
  desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if(type==D3D12_HEAP_TYPE_DEFAULT)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> resource;
  check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"create buffer");
  return resource;
}

void gpuValue(ID3D12Device *device,ID3D12PipelineState *pso,ID3D12RootSignature *root,UINT expected,const char *label) {
  D3D12_COMMAND_QUEUE_DESC queueDesc{};ComPtr<ID3D12CommandQueue> queue;
  check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"create queue");
  ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"create allocator");
  check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"create list");
  auto upload=buffer(device,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output=buffer(device,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
  auto readback=buffer(device,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  void *memory=nullptr;D3D12_RANGE noRead{0,0};check(upload->Map(0,&noRead,&memory),"map sentinel");
  const UINT sentinel=0xcdcdcdcd;memcpy(memory,&sentinel,4);upload->Unmap(0,nullptr);
  list->CopyBufferRegion(output.Get(),0,upload.Get(),0,4);
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource=output.Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  list->ResourceBarrier(1,&barrier);
  const UINT values[2]{3,0x135};list->SetPipelineState(pso);list->SetComputeRootSignature(root);
  list->SetComputeRoot32BitConstants(0,2,values,0);list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());
  list->Dispatch(1,1,1);
  barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1,&barrier);list->CopyBufferRegion(readback.Get(),0,output.Get(),0,4);check(list->Close(),"close list");
  ID3D12CommandList *lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
  ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"create fence");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)fail("create event");
  check(queue->Signal(fence.Get(),1),"signal completion");check(fence->SetEventOnCompletion(1,event),"register completion");
  const DWORD waited=WaitForSingleObject(event,15000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)fail("GPU timeout");
  D3D12_RANGE read{0,4};check(readback->Map(0,&read,&memory),"map result");UINT actual;memcpy(&actual,memory,4);readback->Unmap(0,&noRead);
  if(actual!=expected){std::fprintf(stderr,"actual=%08x expected=%08x\n",actual,expected);fail(label);}
  std::printf("OWNED_D3D12_PIPELINE_CACHE_GPU case=%s actual=%08x expected=%08x\n",label,actual,expected);
}

void reject(ID3D12Device *device,D3D12_COMPUTE_PIPELINE_STATE_DESC desc,const void *bytes,size_t size,const char *label,
            HRESULT expected=E_INVALIDARG) {
  desc.CachedPSO={bytes,size};void *output=reinterpret_cast<void*>(uintptr_t(1));
  const HRESULT hr=device->CreateComputePipelineState(&desc,__uuidof(ID3D12PipelineState),&output);
  if(hr!=expected||output)fail(label,hr);
  std::printf("OWNED_D3D12_PIPELINE_CACHE_REJECT case=%s hr=%08lx\n",label,static_cast<unsigned long>(hr));
}

int run(const wchar_t *firstPath,const wchar_t *secondPath) {
  using BackendId=const char*(WINAPI*)();auto identity=reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  if(!identity||strcmp(identity(),"owned-graphics-d3d12"))fail("backend identity");
  std::printf("OWNED_D3D12_PIPELINE_CACHE_BACKEND id=%s\n",identity());
  ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"create factory");
  ComPtr<IDXGIAdapter1> adapter;check(factory->EnumAdapters1(0,&adapter),"get adapter");
  ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"create device");
  auto shaderA=readFile(firstPath),shaderB=readFile(secondPath);auto root=rootSignature(device.Get(),2);
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();desc.CS={shaderA.data(),shaderA.size()};
  ComPtr<ID3D12PipelineState> source;check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&source)),"create source PSO");
  ComPtr<ID3DBlob> blob;check(source->GetCachedBlob(&blob),"export cache");
  const auto *data=static_cast<const uint8_t*>(blob->GetBufferPointer());std::vector<uint8_t> good(data,data+blob->GetBufferSize());
  if(source->GetCachedBlob(nullptr)!=E_POINTER)fail("null cache output");
  // Version 3 predates the P3 hidden-resource/signature ABI. Applications must
  // discard those persisted blobs even when shader/root/PSO inputs are unchanged.
  auto prior=good;const uint32_t priorVersion=3;memcpy(prior.data()+8,&priorVersion,sizeof(priorVersion));
  reject(device.Get(),desc,prior.data(),prior.size(),"pre_p3_version",D3D12_ERROR_DRIVER_VERSION_MISMATCH);
  reject(device.Get(),desc,nullptr,good.size(),"null_nonempty");
  reject(device.Get(),desc,good.data(),good.size()-1,"truncated");
  auto damaged=good;damaged.back()^=1;reject(device.Get(),desc,damaged.data(),damaged.size(),"corrupt_payload");
  std::fill(damaged.begin(),damaged.end(),0xa5);reject(device.Get(),desc,damaged.data(),damaged.size(),"foreign_blob");
  auto otherRoot=rootSignature(device.Get(),3);auto different=desc;different.pRootSignature=otherRoot.Get();
  reject(device.Get(),different,good.data(),good.size(),"root_mismatch");
  different=desc;different.CS={shaderB.data(),shaderB.size()};
  reject(device.Get(),different,good.data(),good.size(),"shader_mismatch");
  ComPtr<ID3D12PipelineState> other;
  check(device->CreateComputePipelineState(&different,IID_PPV_ARGS(&other)),"valid changed shader without old cache");
  gpuValue(device.Get(),other.Get(),root.Get(),0x31415926u^0x135u,"changed_shader_control");
  // The same valid cache must still work after all rejected requests. Unaligned
  // caller storage is legal and may be destroyed as soon as creation returns.
  std::vector<uint8_t> unaligned(good.size()+1);memcpy(unaligned.data()+1,good.data(),good.size());
  desc.CachedPSO={unaligned.data()+1,good.size()};ComPtr<ID3D12PipelineState> restored;
  check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&restored)),"restore unaligned cache");
  std::fill(unaligned.begin(),unaligned.end(),0xda);unaligned.clear();unaligned.shrink_to_fit();source.Reset();blob.Reset();
  gpuValue(device.Get(),restored.Get(),root.Get(),0x31415926u+0x135u,"unaligned_input_lifetime");
  check(restored->GetCachedBlob(&blob),"export restored cache");memset(blob->GetBufferPointer(),0x5a,blob->GetBufferSize());blob.Reset();
  check(restored->GetCachedBlob(&blob),"export after client mutation");desc.CachedPSO={blob->GetBufferPointer(),blob->GetBufferSize()};
  ComPtr<ID3D12PipelineState> final;check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&final)),"restore independent returned blob");
  restored.Reset();blob.Reset();gpuValue(device.Get(),final.Get(),root.Get(),0x31415926u+0x135u,"returned_blob_isolation");
  std::puts("OWNED_D3D12_PIPELINE_CACHE_PASS");return 0;
}
} // namespace
int wmain(int argc,wchar_t **argv){return argc==3?run(argv[1],argv[2]):2;}
