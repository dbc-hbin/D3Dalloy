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
  if (FAILED(hr)) { std::fprintf(stderr, "OWNED_D3D12_ATOMIC64_WORKGRAPH_FAIL step=%s hr=%08lx\n", step, (unsigned long)hr); throw std::runtime_error(step); }
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

int main() {
  try {
    constexpr UINT count=16, stride=96, bytes=count*stride+16;
    constexpr uint64_t base=0xfffffffafffff000ULL, delta=0x000000040000000fULL;
    constexpr uint64_t mask=0xa5c35a3cf00d1234ULL, mask2=0x13579bdf2468ace0ULL, mask3=0x55aa55aaaa55aa55ULL;
    struct Record { uint32_t index,guard; uint64_t value; };
    static_assert(sizeof(Record)==16 && offsetof(Record,value)==8 && alignof(Record)==8);
    std::array<Record,count> records{};
    for(UINT i=0;i<count;++i)records[i]={i,0xf00d1234u^i,base+uint64_t(i)*0x100000001ULL};
    ComPtr<ID3D12Device5> device;Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_8};
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11{};
    // An older runtime rejects unknown enum values; negotiate as the public API requires.
    HRESULT modelResult = E_INVALIDARG;
    for (UINT requested = D3D_SHADER_MODEL_6_8; requested >= D3D_SHADER_MODEL_6_0; --requested) {
      model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(requested);
      modelResult = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model));
      if (modelResult != E_INVALIDARG) break;
    }
    Check(modelResult,"advertised shader model");
    Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9,&options9,sizeof(options9)),"atomic64 typed capability");
    Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11,&options11,sizeof(options11)),"atomic64 heap capability");
    std::printf("OWNED_D3D12_ATOMIC64_WORKGRAPH_CAP library_profile=lib_6_8 advertised_shader_model=%x typed_cap=%u heap_cap=%u\n",
      UINT(model.HighestShaderModel),UINT(options9.AtomicInt64OnTypedResourceSupported),UINT(options11.AtomicInt64OnDescriptorHeapResourceSupported));
    Require(options9.AtomicInt64OnTypedResourceSupported && options11.AtomicInt64OnDescriptorHeapResourceSupported,"atomic64 typed/heap capabilities");
    std::puts("OWNED_D3D12_ATOMIC64_WORKGRAPH_BACKEND id=owned-graphics-d3d12");
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameter.Descriptor.ShaderRegister=0;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> rootBlob;Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,nullptr),"serialize root");
    ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,rootBlob->GetBufferPointer(),rootBlob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
    auto shader=Shader("atomic64-workgraph-records.dxil");D3D12_DXIL_LIBRARY_DESC library{{shader.data(),shader.size()},0,nullptr};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};D3D12_WORK_GRAPH_DESC graph{};
    graph.ProgramName=L"atomic_records";graph.Flags=D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    std::array<D3D12_STATE_SUBOBJECT,3> objects{{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global},{D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH,&graph}}};
    D3D12_STATE_OBJECT_DESC stateDesc{D3D12_STATE_OBJECT_TYPE_EXECUTABLE,UINT(objects.size()),objects.data()};
    ComPtr<ID3D12StateObject> state;Check(device->CreateStateObject(&stateDesc,IID_PPV_ARGS(&state)),"real record64 executable");
    ComPtr<ID3D12WorkGraphProperties> properties;Check(state.As(&properties),"graph properties");
    UINT graphIndex=properties->GetWorkGraphIndex(L"atomic_records");Require(graphIndex!=UINT_MAX,"graph index");
    Require(properties->GetNumNodes(graphIndex)==3 && properties->GetNumEntrypoints(graphIndex)==1,"seed child terminal topology");
    UINT entry=properties->GetEntrypointIndex(graphIndex,{L"seed",0});Require(entry!=UINT_MAX,"seed entry");
    Require(properties->GetEntrypointRecordSizeInBytes(graphIndex,entry)==sizeof(Record) &&
      properties->GetEntrypointRecordAlignmentInBytes(graphIndex,entry)==alignof(Record),"exact public record layout");
    ComPtr<ID3D12StateObjectProperties1> identifiers;Check(state.As(&identifiers),"program identifier interface");
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS requirements{};properties->GetWorkGraphMemoryRequirements(graphIndex,&requirements);
    Require(requirements.MinSizeInBytes!=0 && requirements.SizeGranularityInBytes!=0,"real backing requirements");
    auto backing=Buffer(device.Get(),requirements.MinSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto result=Buffer(device.Get(),bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto initial=Buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    std::array<uint32_t,bytes/4> zero{};zero.back()=0xa11c0ded;Upload(initial.Get(),zero.data(),sizeof(zero));
    auto readback=Buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto gpuSeeds=Buffer(device.Get(),sizeof(records),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto seedUpload=Buffer(device.Get(),sizeof(records),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);Upload(seedUpload.Get(),records.data(),sizeof(records));
    auto gpuArguments=Buffer(device.Get(),sizeof(D3D12_NODE_GPU_INPUT),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_NODE_GPU_INPUT gpuInput{entry,count,{gpuSeeds->GetGPUVirtualAddress(),sizeof(Record)}};Upload(gpuArguments.Get(),&gpuInput,sizeof(gpuInput));
    D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"queue");
    ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"completion event");
    for(UINT mode=0;mode<2;++mode){
      ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
      ComPtr<ID3D12GraphicsCommandList10> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list10");
      D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition={result.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
      if(mode)list->ResourceBarrier(1,&barrier);
      list->CopyBufferRegion(result.Get(),0,initial.Get(),0,bytes);
      barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;list->ResourceBarrier(1,&barrier);
      if(mode){
        list->CopyBufferRegion(gpuSeeds.Get(),0,seedUpload.Get(),0,sizeof(records));
        D3D12_RESOURCE_BARRIER seedBarrier{};seedBarrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        seedBarrier.Transition={gpuSeeds.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};list->ResourceBarrier(1,&seedBarrier);
        ComPtr<ID3D12GraphicsCommandListPreview> preview;Check(list.As(&preview),"public GPU record limit interface");preview->SetWorkGraphMaximumGPUInputRecords(count,1);
      }
      list->SetComputeRootSignature(root.Get());list->SetComputeRootUnorderedAccessView(0,result->GetGPUVirtualAddress());
      D3D12_SET_PROGRAM_DESC program{};program.Type=D3D12_PROGRAM_TYPE_WORK_GRAPH;
      identifiers->GetProgramIdentifier(&program.WorkGraph.ProgramIdentifier,L"atomic_records");
      program.WorkGraph.Flags=mode?D3D12_SET_WORK_GRAPH_FLAG_NONE:D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;
      program.WorkGraph.BackingMemory={backing->GetGPUVirtualAddress(),requirements.MinSizeInBytes};list->SetProgram(&program);
      D3D12_DISPATCH_GRAPH_DESC dispatch{};
      if(mode){dispatch.Mode=D3D12_DISPATCH_MODE_NODE_GPU_INPUT;dispatch.NodeGPUInput=gpuArguments->GetGPUVirtualAddress();}
      else{dispatch.Mode=D3D12_DISPATCH_MODE_NODE_CPU_INPUT;dispatch.NodeCPUInput={entry,count,records.data(),sizeof(Record)};}
      list->DispatchGraph(&dispatch);
      if(!mode)for(auto &record:records)record={0,0,0}; // CPU records must already have been copied.
      barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&barrier);
      list->CopyBufferRegion(readback.Get(),0,result.Get(),0,bytes);Check(list->Close(),"Close");
      ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),mode+1),"Signal");
      Check(fence->SetEventOnCompletion(mode+1,event),"SetEventOnCompletion");Require(WaitForSingleObject(event,180000)==WAIT_OBJECT_0,"GPU completion");Check(device->GetDeviceRemovedReason(),"device alive");
      void *mapping=nullptr;D3D12_RANGE range{0,bytes};Check(readback->Map(0,&range,&mapping),"Map result");
      for(UINT i=0;i<count;++i){
        const uint64_t initialValue=base+uint64_t(i)*0x100000001ULL, added=initialValue+delta;
        const uint64_t beforeXor=added^mask, afterXor=beforeXor^mask2, afterCas=afterXor+delta;
        const uint32_t guard=0xf00d1234u^i, guardNew=guard+3;
        std::array<uint64_t,12> expected{{initialValue,added,beforeXor,afterXor,afterXor,afterCas,
          uint64_t(guard)|(uint64_t(guardNew)<<32),i,afterCas^mask3,0x8000000000000005ULL+i,
          uint64_t(i)|(uint64_t(guardNew)<<32),0xdeadbeefcafef00dULL}};
        std::array<uint64_t,12> actual{};std::memcpy(actual.data(),static_cast<const uint8_t*>(mapping)+i*stride,sizeof(actual));
        std::printf("OWNED_D3D12_ATOMIC64_WORKGRAPH_RECORD mode=%s index=%u words=(",mode?"GPU":"CPU",i);
        for(UINT word=0;word<actual.size();++word)std::printf("%s0x%016llx",word?",":"",(unsigned long long)actual[word]);
        std::puts(")");
        for(UINT word=0;word<expected.size();++word){
          if(actual[word]!=expected[word])std::fprintf(stderr,"RECORD64_MISMATCH mode=%u record=%u word=%u actual=%016llx expected=%016llx\n",mode,i,word,(unsigned long long)actual[word],(unsigned long long)expected[word]);
          Require(actual[word]==expected[word],"exact full64 old/final input output descendant and guard alias");
        }
      }
      std::array<uint32_t,4> counters{};std::memcpy(counters.data(),static_cast<const uint8_t*>(mapping)+count*stride,sizeof(counters));readback->Unmap(0,nullptr);
      std::printf("OWNED_D3D12_ATOMIC64_WORKGRAPH_COUNTS mode=%s seed=%u child=%u terminal=%u guard=%08x\n",mode?"GPU":"CPU",counters[0],counters[1],counters[2],counters[3]);
      Require(counters==std::array<uint32_t,4>{{count,count,count,0xa11c0ded}},"queue counts and adjacent guard intact");
      std::printf("OWNED_D3D12_ATOMIC64_WORKGRAPH_PASS mode=%s records=16 fields=192 seed_count=16 child_count=16 terminal_count=16 input_bytes=16 input_align=8 queue_guard=PASS old_returns=PASS final_values=PASS\n",mode?"GPU":"CPU");
    }
    CloseHandle(event);std::puts("OWNED_D3D12_ATOMIC64_WORKGRAPH_TOTAL_PASS modes=2 fields=384");return 0;
  }catch(...){return 1;}
}
