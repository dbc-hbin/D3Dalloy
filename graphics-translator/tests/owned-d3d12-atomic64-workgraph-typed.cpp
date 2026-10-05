#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef DXMT_MATRIX_PREVIEW_SDK_HEADER
#error Compile with the pinned public preview SDK in DXMT_MATRIX_PREVIEW_SDK_HEADER.
#endif
#include <rpc.h>
#include <rpcndr.h>
#if defined(__clang__) && defined(__MINGW32__)
#undef __uuidof
#undef DECLSPEC_UUID
#define DECLSPEC_UUID(value) __declspec(uuid(value))
#undef MIDL_INTERFACE
#define MIDL_INTERFACE(value) struct __declspec(uuid(value))
#endif
#include DXMT_MATRIX_PREVIEW_SDK_HEADER
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

namespace {
void Check(HRESULT hr, const char *step) {
  if (FAILED(hr)) { std::fprintf(stderr, "OWNED_D3D12_ATOMIC64_WG_TYPED_FAIL step=%s hr=%08lx\n", step, (unsigned long)hr); throw std::runtime_error(step); }
}
void Require(bool condition, const char *step) { Check(condition ? S_OK : E_FAIL, step); }
std::vector<uint8_t> Shader(const char *name) {
  FILE *file = std::fopen(name, "rb"); Require(file != nullptr, "open shader");
  std::fseek(file, 0, SEEK_END); long size = std::ftell(file); std::rewind(file);
  std::vector<uint8_t> bytes(size); Require(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size(), "read shader");
  std::fclose(file); return bytes;
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device *device, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                             D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
  D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
  desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
  ComPtr<ID3D12Resource> resource; Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state,
      nullptr, IID_PPV_ARGS(&resource)), "CreateCommittedResource"); return resource;
}
void Upload(ID3D12Resource *buffer, const void *bytes, size_t size, size_t offset = 0) {
  void *mapping = nullptr; D3D12_RANGE empty{0, 0}; Check(buffer->Map(0, &empty, &mapping), "Map upload");
  std::memcpy(static_cast<uint8_t *>(mapping) + offset, bytes, size); buffer->Unmap(0, nullptr);
}
}

int main(){try{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
  using BackendIdFn = const char *(WINAPI *)();
  auto backend = d3d12 ? reinterpret_cast<BackendIdFn>(GetProcAddress(d3d12, "OwnedGraphicsBackendId")) : nullptr;
  Require(backend && backend() && std::strcmp(backend(), "owned-graphics-d3d12") == 0,
          "OwnedGraphicsBackendId mismatch");
  std::printf("OWNED_D3D12_ATOMIC64_WG_TYPED_BACKEND id=%s\n", backend());
  constexpr UINT n=16;constexpr uint64_t base=0xfffffffafffff000ULL,delta=0x40000000fULL;
  ComPtr<ID3D12Device5> device;Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 options{};Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9,&options,sizeof(options)),"typed atomic64 capability");
  Require(options.AtomicInt64OnTypedResourceSupported,"AtomicInt64OnTypedResourceSupported");
  std::printf("OWNED_D3D12_ATOMIC64_WG_TYPED_CAP library_profile=lib_6_8 typed_cap=%u\n",UINT(options.AtomicInt64OnTypedResourceSupported));
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0};D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable={1,&range};
  D3D12_ROOT_SIGNATURE_DESC rootDesc{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob;
  Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"serialize root");
  ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  auto shader=Shader("atomic64-workgraph-typed.dxil");D3D12_DXIL_LIBRARY_DESC library{{shader.data(),shader.size()},0,nullptr};
  D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};D3D12_WORK_GRAPH_DESC graph{};graph.ProgramName=L"typed_publisher";graph.Flags=D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
  std::array<D3D12_STATE_SUBOBJECT,3> objects{{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},{D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global},{D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH,&graph}}};
  D3D12_STATE_OBJECT_DESC stateDesc{D3D12_STATE_OBJECT_TYPE_EXECUTABLE,UINT(objects.size()),objects.data()};ComPtr<ID3D12StateObject> state;
  Check(device->CreateStateObject(&stateDesc,IID_PPV_ARGS(&state)),"typed node executable");
  ComPtr<ID3D12WorkGraphProperties> properties;Check(state.As(&properties),"graph properties");UINT g=properties->GetWorkGraphIndex(L"typed_publisher");Require(g!=UINT_MAX,"graph index");
  UINT entry=properties->GetEntrypointIndex(g,{L"typed_publish",0});Require(entry!=UINT_MAX && properties->GetNumNodes(g)==1,"one legal typed node");
  D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS requirements{};properties->GetWorkGraphMemoryRequirements(g,&requirements);
  auto backing=Buffer(device.Get(),requirements.MinSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=4;desc.Height=4;desc.DepthOrArraySize=1;desc.MipLevels=1;
  desc.Format=DXGI_FORMAT_R32G32_UINT;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES heapProperties{};heapProperties.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> texture;
  Check(device->CreateCommittedResource(&heapProperties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture)),"R32G32 UINT texture");
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 rowBytes=0,total=0;
  device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&total);Require(rows==4 && rowBytes==32,"typed texture footprint");
  auto upload=Buffer(device.Get(),total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);std::vector<uint8_t> seedBytes(total);
  for(UINT i=0;i<n;++i){uint64_t value=base+uint64_t(i)*0x100000001ULL;std::memcpy(seedBytes.data()+footprint.Offset+(i/4)*footprint.Footprint.RowPitch+(i%4)*8,&value,8);}Upload(upload.Get(),seedBytes.data(),seedBytes.size());
  auto textureReadback=Buffer(device.Get(),total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  auto output=Buffer(device.Get(),n*16+8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto outputUpload=Buffer(device.Get(),n*16+8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);std::array<uint32_t,n*4+2> zeros{};zeros.back()=0xa11c0ded;Upload(outputUpload.Get(),zeros.data(),sizeof(zeros));
  auto outputReadback=Buffer(device.Get(),n*16+8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heapDesc.NumDescriptors=2;heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  ComPtr<ID3D12DescriptorHeap> heap;Check(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)),"two nonaliasing UAV descriptors");
  D3D12_CPU_DESCRIPTOR_HANDLE cpu{};D3D12_GPU_DESCRIPTOR_HANDLE gpu{};heap->GetCPUDescriptorHandleForHeapStart(&cpu);heap->GetGPUDescriptorHandleForHeapStart(&gpu);
  D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32_UINT;view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(texture.Get(),nullptr,&view,cpu);
  cpu.ptr+=device->GetDescriptorHandleIncrementSize(heapDesc.Type);view={};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=n*4+2;view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(output.Get(),nullptr,&view,cpu);
  D3D12_COMMAND_QUEUE_DESC queueDesc{};ComPtr<ID3D12CommandQueue> queue;Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"queue");
  ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
  ComPtr<ID3D12GraphicsCommandList10> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list10");
  D3D12_TEXTURE_COPY_LOCATION tex{},source{},destination{};tex.pResource=texture.Get();tex.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.pResource=upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprint;list->CopyTextureRegion(&tex,0,0,0,&source,nullptr);
  list->CopyBufferRegion(output.Get(),0,outputUpload.Get(),0,n*16+8);
  std::array<D3D12_RESOURCE_BARRIER,2> barriers{};for(auto &b:barriers)b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition={texture.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
  barriers[1].Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};list->ResourceBarrier(UINT(barriers.size()),barriers.data());
  ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetComputeRootDescriptorTable(0,gpu);
  ComPtr<ID3D12StateObjectProperties1> identifiers;Check(state.As(&identifiers),"program identifier");D3D12_SET_PROGRAM_DESC program{};program.Type=D3D12_PROGRAM_TYPE_WORK_GRAPH;
  identifiers->GetProgramIdentifier(&program.WorkGraph.ProgramIdentifier,L"typed_publisher");program.WorkGraph.Flags=D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;program.WorkGraph.BackingMemory={backing->GetGPUVirtualAddress(),requirements.MinSizeInBytes};list->SetProgram(&program);
  std::array<uint32_t,n> inputs{};for(UINT i=0;i<n;++i)inputs[i]=i;D3D12_DISPATCH_GRAPH_DESC dispatch{};dispatch.Mode=D3D12_DISPATCH_MODE_NODE_CPU_INPUT;dispatch.NodeCPUInput={entry,n,inputs.data(),sizeof(UINT)};list->DispatchGraph(&dispatch);
  D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;uav.UAV.pResource=texture.Get();list->ResourceBarrier(1,&uav);
  for(auto &b:barriers){b.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;}list->ResourceBarrier(UINT(barriers.size()),barriers.data());
  destination.pResource=textureReadback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprint;list->CopyTextureRegion(&destination,0,0,0,&tex,nullptr);list->CopyBufferRegion(outputReadback.Get(),0,output.Get(),0,n*16+8);Check(list->Close(),"Close");
  ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"event");
  ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),1),"Signal");Check(fence->SetEventOnCompletion(1,event),"completion");Require(WaitForSingleObject(event,180000)==WAIT_OBJECT_0,"GPU completion");Check(device->GetDeviceRemovedReason(),"device alive");CloseHandle(event);
  void *pixels=nullptr,*results=nullptr;D3D12_RANGE pixelRange{0,size_t(total)},resultRange{0,n*16+8};Check(textureReadback->Map(0,&pixelRange,&pixels),"Map footprint");Check(outputReadback->Map(0,&resultRange,&results),"Map old final returns");
  for(UINT i=0;i<n;++i){std::array<uint64_t,2> actual{};uint64_t copied=0;std::memcpy(actual.data(),static_cast<const uint8_t*>(results)+i*16,16);std::memcpy(&copied,static_cast<const uint8_t*>(pixels)+footprint.Offset+(i/4)*footprint.Footprint.RowPitch+(i%4)*8,8);
    const uint64_t initial=base+uint64_t(i)*0x100000001ULL,final=initial+delta;
    std::printf("OWNED_D3D12_ATOMIC64_WG_TYPED_TEXEL index=%u old=0x%016llx final=0x%016llx copy=0x%016llx\n",i,(unsigned long long)actual[0],(unsigned long long)actual[1],(unsigned long long)copied);
    Require(actual[0]==initial && actual[1]==final && copied==final,"returning full64 typed and published physical texture values");}
  std::array<uint32_t,2> counts{};std::memcpy(counts.data(),static_cast<const uint8_t*>(results)+n*16,8);textureReadback->Unmap(0,nullptr);outputReadback->Unmap(0,nullptr);Require(counts==std::array<uint32_t,2>{{16,0xa11c0ded}},"typed node count and guard");
  std::printf("OWNED_D3D12_ATOMIC64_WG_TYPED_COUNTS count=%u guard=%08x\n",counts[0],counts[1]);
  std::puts("OWNED_D3D12_ATOMIC64_WG_TYPED_PASS texels=16 full64_words=48 nonaliasing_views=1 API_barrier=1 physical_copy=PASS count=16 guard=PASS");return 0;
}catch(...){return 1;}}
