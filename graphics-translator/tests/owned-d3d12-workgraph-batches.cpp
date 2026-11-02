#define main retained_typed_fixture_main
#include "owned-d3d12-atomic64-workgraph-typed.cpp"
#undef main
#include <algorithm>

int main() { try {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  auto module=GetModuleHandleW(L"d3d12.dll");
  auto backend=reinterpret_cast<const char*(WINAPI*)()>(GetProcAddress(module,"OwnedGraphicsBackendId"));
  Require(backend && !std::strcmp(backend(),"owned-graphics-d3d12"),"backend");
  std::printf("OWNED_D3D12_WG_BATCH_BACKEND id=%s\n",backend());
  ComPtr<ID3D12Device5> device;Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
  D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC rootDesc{1,&parameter};ComPtr<ID3DBlob> blob;
  Check(D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"root blob");
  ComPtr<ID3D12RootSignature> root;Check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  auto shader=Shader("workgraph-batches.dxil");D3D12_DXIL_LIBRARY_DESC library{{shader.data(),shader.size()},0,nullptr};
  D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};D3D12_WORK_GRAPH_DESC graph{};
  graph.ProgramName=L"batches";graph.Flags=D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
  std::array<D3D12_STATE_SUBOBJECT,3> sub{{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global},{D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH,&graph}}};
  D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_EXECUTABLE,UINT(sub.size()),sub.data()};
  ComPtr<ID3D12StateObject> state;Check(device->CreateStateObject(&desc,IID_PPV_ARGS(&state)),"leaf program");
  ComPtr<ID3D12WorkGraphProperties> properties;Check(state.As(&properties),"properties");
  ComPtr<ID3D12StateObjectProperties1> ids;Check(state.As(&ids),"program ID");
  const UINT g=properties->GetWorkGraphIndex(L"batches");Require(g!=UINT_MAX,"graph index");
  D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{};properties->GetWorkGraphMemoryRequirements(g,&memory);
  auto backing=Buffer(device.Get(),memory.MinSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  struct Seed {UINT x,y,z,tag;};
  unsigned cases=0;
  for (bool atomic : {false,true}) for (bool gpu : {false,true}) {
    const Seed seed{atomic?65u:257u,atomic?1u:257u,1,0x13579bdf};
    const UINT groups=seed.x*seed.y;const size_t bytes=8+size_t(groups)*8+8;
    auto output=Buffer(device.Get(),bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto upload=Buffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    std::vector<uint8_t> zeros(bytes,0);const uint64_t guard=0xfeedabcddcba4321ull;
    std::memcpy(zeros.data()+bytes-8,&guard,8);Upload(upload.Get(),zeros.data(),zeros.size());
    auto readback=Buffer(device.Get(),bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto seedBuffer=Buffer(device.Get(),256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    Upload(seedBuffer.Get(),&seed,sizeof(seed));
    const UINT entry=properties->GetEntrypointIndex(g,{atomic?L"atomic_leaf":L"leaf",0});Require(entry!=UINT_MAX,"entry");
    D3D12_NODE_GPU_INPUT input{entry,1,{seedBuffer->GetGPUVirtualAddress(),sizeof(seed)}};
    Upload(seedBuffer.Get(),&input,sizeof(input),64);
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
    ComPtr<ID3D12CommandAllocator> allocator;Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    ComPtr<ID3D12GraphicsCommandList10> list;Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    list->CopyBufferRegion(output.Get(),0,upload.Get(),0,bytes);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};list->ResourceBarrier(1,&barrier);
    list->SetComputeRootSignature(root.Get());list->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());
    D3D12_SET_PROGRAM_DESC program{};program.Type=D3D12_PROGRAM_TYPE_WORK_GRAPH;
    ids->GetProgramIdentifier(&program.WorkGraph.ProgramIdentifier,L"batches");
    program.WorkGraph.Flags=D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE;program.WorkGraph.BackingMemory={backing->GetGPUVirtualAddress(),memory.MinSizeInBytes};
    list->SetProgram(&program);D3D12_DISPATCH_GRAPH_DESC dispatch{};
    dispatch.Mode=gpu?D3D12_DISPATCH_MODE_NODE_GPU_INPUT:D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    if(gpu)dispatch.NodeGPUInput=seedBuffer->GetGPUVirtualAddress()+64;else dispatch.NodeCPUInput={entry,1,&seed,sizeof(seed)};
    list->DispatchGraph(&dispatch);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;list->ResourceBarrier(1,&barrier);
    list->CopyBufferRegion(readback.Get(),0,output.Get(),0,bytes);Check(list->Close(),"close");
    ComPtr<ID3D12Fence> fence;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    auto event=CreateEvent(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"event");
    ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);Check(queue->Signal(fence.Get(),1),"signal");
    Check(fence->SetEventOnCompletion(1,event),"completion");Require(WaitForSingleObject(event,180000)==WAIT_OBJECT_0,"GPU complete");CloseHandle(event);
    Check(device->GetDeviceRemovedReason(),"device alive");void* mapped=nullptr;D3D12_RANGE range{0,bytes};Check(readback->Map(0,&range,&mapped),"readback");
    uint64_t count=0,actualGuard=0;std::memcpy(&count,mapped,8);std::memcpy(&actualGuard,static_cast<uint8_t*>(mapped)+bytes-8,8);
    Require(count==groups && actualGuard==guard,"all groups and sentinel");std::vector<uint64_t> returns;
    for(UINT i=0;i<groups;++i) {
      uint64_t actual=0;std::memcpy(&actual,static_cast<uint8_t*>(mapped)+8+size_t(i)*8,8);
      if(atomic)returns.push_back(actual);else Require(actual==(uint64_t((i%seed.x)+(i/seed.x)*65536)<<32 | (i^seed.tag)),"original grid coordinates and record");
    }
    if(atomic){std::sort(returns.begin(),returns.end());for(UINT i=0;i<groups;++i)Require(returns[i]==i,"full64 old values across batches");}
    readback->Unmap(0,nullptr);++cases;
    std::printf("OWNED_D3D12_WG_BATCH_CASE atomic=%u gpu_input=%u groups=%u count=%llu guard=ok coordinates_or_returns=exact\n",atomic,gpu,groups,(unsigned long long)count);
  }
  std::printf("OWNED_D3D12_WG_BATCH_PASS cases=%u dynamic_grid=1 ordinary_split=65535,514 atomic_split=32,32,1 full_records=validated\n",cases);
  return 0;
} catch(...) {return 1;} }
