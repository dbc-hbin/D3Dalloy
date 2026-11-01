#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {

[[noreturn]] void fail(const char *step, HRESULT hr = E_FAIL) {
  std::fprintf(stderr,"OWNED_D3D12_PIPELINE_CACHE_FAIL step=%s hr=0x%08lx\n",step,static_cast<unsigned long>(hr));
  throw std::runtime_error(step);
}
void check(HRESULT hr,const char *step) { if(FAILED(hr))fail(step,hr); }

std::vector<uint8_t> readFile(const wchar_t *path) {
  HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
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

struct DeveloperMode {
  HKEY key=nullptr;DWORD previous=0;bool existed=false;
  DeveloperMode() {
    char isolated[2]{};
    if(GetEnvironmentVariableA("OWNED_SHADER_CACHE_CONTROL_ISOLATED",isolated,2)!=1||isolated[0]!='1')
      fail("automatic cache requires isolated prefix");
    if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",0,
       nullptr,0,KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr))fail("developer registry open");
    DWORD type=0,size=sizeof(previous);
    existed=RegQueryValueExW(key,L"AllowDevelopmentWithoutDevLicense",nullptr,&type,
                           reinterpret_cast<BYTE*>(&previous),&size)==ERROR_SUCCESS;
    if(existed&&(type!=REG_DWORD||size!=sizeof(previous)))fail("developer registry type");
    const DWORD enabled=1;
    if(RegSetValueExW(key,L"AllowDevelopmentWithoutDevLicense",0,REG_DWORD,
                     reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled)))fail("developer registry enable");
  }
  ~DeveloperMode() {
    if(existed)RegSetValueExW(key,L"AllowDevelopmentWithoutDevLicense",0,REG_DWORD,
                            reinterpret_cast<const BYTE*>(&previous),sizeof(previous));
    else RegDeleteValueW(key,L"AllowDevelopmentWithoutDevLicense");
    RegCloseKey(key);
  }
};

std::wstring cacheFile(ID3D12PipelineState *pso,const char *label) {
  ComPtr<ID3DBlob> blob;check(pso->GetCachedBlob(&blob),"automatic export");
  wchar_t temp[32768];if(!GetTempPathW(32768,temp))fail("cache temp path");
  const std::wstring base=std::wstring(temp)+L"d3dalloy-shader-sessions\\";
  constexpr wchar_t prefix[]=L"d3dalloy-pso-";
  // PSO completion publishes to RAM; a separate worker atomically renames the disk blob.
  // Arm before scanning so publication between the scan and wait cannot be missed.
  const ULONGLONG started=GetTickCount64();
  for(;;) {
    std::vector<HANDLE> changes;
    auto watch=[&](const std::wstring &path) {
      HANDLE event=FindFirstChangeNotificationW(path.c_str(),FALSE,
          FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE);
      if(event==INVALID_HANDLE_VALUE)fail("automatic directory event");
      changes.push_back(event);
    };
    auto closeChanges=[&] {for(HANDLE event:changes)FindCloseChangeNotification(event);};
    watch(base);
    WIN32_FIND_DATAW directory{};HANDLE dirs=FindFirstFileW((base+L"*").c_str(),&directory);
    if(dirs==INVALID_HANDLE_VALUE){closeChanges();fail("automatic directory missing");}
    std::wstring result;
    do {
      if(!(directory.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||directory.cFileName[0]==L'.')continue;
      const auto path=base+directory.cFileName+L"\\";
      // Subscribe to each session before scanning its files, without relying on
      // recursive directory notification support.
      watch(path);
      WIN32_FIND_DATAW entry{};HANDLE files=FindFirstFileW((path+prefix+L"*.bin").c_str(),&entry);
      if(files==INVALID_HANDLE_VALUE)continue;
      do {
        if(entry.nFileSizeHigh||entry.nFileSizeLow!=blob->GetBufferSize())continue;
        const auto bytes=readFile((path+entry.cFileName).c_str());
        if(!memcmp(bytes.data(),blob->GetBufferPointer(),bytes.size())) {
          if(!result.empty())fail("ambiguous automatic cache entry");
          result=path+entry.cFileName;
          std::printf("OWNED_D3D12_PIPELINE_CACHE_KEY case=%s key=%ls\n",label,
                      std::wstring(entry.cFileName+std::size(prefix)-1,40).c_str());
        }
      } while(FindNextFileW(files,&entry));
      FindClose(files);
    } while(FindNextFileW(dirs,&directory));
    FindClose(dirs);
    if(!result.empty()){closeChanges();return result;}
    const ULONGLONG elapsed=GetTickCount64()-started;
    if(changes.size()>MAXIMUM_WAIT_OBJECTS){closeChanges();fail("too many automatic cache directories");}
    const DWORD waited=elapsed>=15000?WAIT_TIMEOUT:
        WaitForMultipleObjects(DWORD(changes.size()),changes.data(),FALSE,DWORD(15000-elapsed));
    closeChanges();
    if(waited>=WAIT_OBJECT_0+changes.size())fail("automatic blob not persisted");
  }
}

void writeFile(const std::wstring &path,const std::vector<uint8_t> &bytes) {
  HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file==INVALID_HANDLE_VALUE)fail("cache mutation open");
  DWORD written=0;const bool ok=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
  CloseHandle(file);if(!ok||written!=bytes.size())fail("cache mutation write");
}

// Wire mirror of production CacheHeader/StageRecord, not a runtime-version heuristic.
struct CacheMutationStage {
  uint64_t library_size, constant_size, constant_alignment, relocation_count;
};
struct CacheMutationHeader {
  uint64_t magic;
  uint32_t version, stages;
  uint64_t size, adapter;
  uint8_t identity[48], key[20], digest[20];
  CacheMutationStage records[3];
};
static_assert(sizeof(CacheMutationHeader)==216 && sizeof(CacheMutationStage)==32);
static_assert(offsetof(CacheMutationHeader,identity)==32 && offsetof(CacheMutationHeader,digest)==100 &&
              offsetof(CacheMutationHeader,records)==120);
constexpr uint32_t kMutationCacheVersion=7;

size_t cacheLibrarySize(const std::vector<uint8_t> &bytes) {
  // Mutate only a validated single-stage export; unknown wire revisions fail closed.
  if(bytes.size()<sizeof(CacheMutationHeader)||memcmp(bytes.data(),"D3ALPSO",8))fail("mutation cache magic/header");
  CacheMutationHeader header{};memcpy(&header,bytes.data(),sizeof(header));
  const uint32_t stages=header.stages;
  if(header.version!=kMutationCacheVersion||header.size!=bytes.size()||!stages||(stages&(stages-1))||(stages&~7u))
    fail("mutation cache revision/layout");
  size_t end=sizeof(header),library=0;
  for(unsigned i=0;i<3;++i){
    const auto &record=header.records[i];
    if(!(stages&(1u<<i))){
      for(auto field:{record.library_size,record.constant_size,record.constant_alignment,record.relocation_count})
        if(field)fail("mutation inactive stage");
      continue;
    }
    if(record.library_size<88||!record.constant_alignment||(record.constant_alignment&(record.constant_alignment-1))||
       record.relocation_count>record.constant_size/8)fail("mutation stage record");
    library=static_cast<size_t>(record.library_size);
    for(const uint64_t length:{record.library_size,record.constant_size,record.relocation_count*16}){
      if(length>bytes.size()-end)fail("mutation stage bounds");end+=static_cast<size_t>(length);
      const size_t padding=(-end)&7;if(padding>bytes.size()-end)fail("mutation stage padding");end+=padding;
    }
  }
  if(end!=bytes.size()||memcmp(bytes.data()+sizeof(header),"MTLB",4))fail("mutation library layout");
  return library;
}

void rehashCache(std::vector<uint8_t> &bytes) {
  cacheLibrarySize(bytes);
  constexpr size_t digest=offsetof(CacheMutationHeader,digest),
                   afterDigest=digest+sizeof(CacheMutationHeader::digest);
  HCRYPTPROV provider=0;HCRYPTHASH hash=0;DWORD size=20;
  const bool acquired=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT);
  const bool ok=acquired&&CryptCreateHash(provider,CALG_SHA1,0,0,&hash)&&
      CryptHashData(hash,bytes.data(),static_cast<DWORD>(digest),0)&&
      CryptHashData(hash,bytes.data()+afterDigest,static_cast<DWORD>(bytes.size()-afterDigest),0)&&
      CryptGetHashParam(hash,HP_HASHVAL,bytes.data()+digest,&size,0)&&size==20;
  if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);
  if(!ok)fail("mutation SHA1");
}

void child(const wchar_t *first,const wchar_t *second,const wchar_t *mode) {
  wchar_t executable[32768];if(!GetModuleFileNameW(nullptr,executable,32768))fail("self path");
  // Do not copy/rename the executable: its absolute path is part of the disk namespace.
  std::wstring command=L"\""+std::wstring(executable)+L"\" \""+first+L"\" \""+second+L"\" "+mode;
  STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
  std::fflush(stdout);std::fflush(stderr);
  char logPath[32768];const DWORD length=GetEnvironmentVariableA("D3DALLOY_LOG_PATH",logPath,sizeof(logPath));
  if(!length||length>=sizeof(logPath))fail("child log path");
  const std::string childLog=std::string(logPath)+"/pipeline-cache-"+std::string(mode+2,mode+wcslen(mode));
  // Set the inherited environment before DLL static initialization opens the log.
  if(!SetEnvironmentVariableA("D3DALLOY_LOG_PATH",childLog.c_str()))fail("child log environment");
  const bool spawned=CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&process);
  const DWORD spawnError=GetLastError();
  if(!SetEnvironmentVariableA("D3DALLOY_LOG_PATH",logPath))fail("restore parent log environment");
  if(!spawned)
    fail("cache child create",HRESULT_FROM_WIN32(spawnError));
  const DWORD waited=WaitForSingleObject(process.hProcess,120000);DWORD code=1;
  if(waited==WAIT_TIMEOUT){TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,INFINITE);}
  GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
  if(waited!=WAIT_OBJECT_0||code)fail("cache child result");
}

ComPtr<ID3D12PipelineState> ordinary(ID3D12Device *device,D3D12_COMPUTE_PIPELINE_STATE_DESC desc) {
  desc.CachedPSO={};ComPtr<ID3D12PipelineState> pso;
  check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pso)),"ordinary create");return pso;
}

void automatic(ID3D12Device9 *device,D3D12_COMPUTE_PIPELINE_STATE_DESC desc,
               const std::vector<uint8_t> &shaderB,const wchar_t *first,const wchar_t *second,const wchar_t *mode) {
  constexpr auto driver=D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED;
  if(mode) {
    if(!wcscmp(mode,L"--failure")) {
      // Isolated invalid-input proof, never a normal production shader fixture.
      auto malformed=readFile(first);malformed.resize(malformed.size()-1);desc.CS={malformed.data(),malformed.size()};
      std::barrier start(8);std::vector<std::thread> threads;HRESULT results[8]{};void *outputs[8]{};
      for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{
        outputs[i]=reinterpret_cast<void*>(uintptr_t(1));start.arrive_and_wait();
        results[i]=device->CreateComputePipelineState(&desc,__uuidof(ID3D12PipelineState),&outputs[i]);
      });
      for(auto &thread:threads)thread.join();
      for(unsigned i=0;i<8;++i)if(SUCCEEDED(results[i])||outputs[i]||results[i]!=results[0])fail("failure completion");
      void *output=reinterpret_cast<void*>(uintptr_t(1));
      const HRESULT retry=device->CreateComputePipelineState(&desc,__uuidof(ID3D12PipelineState),&output);
      if(retry!=results[0]||output)fail("failure later retry",retry);
      desc.CS={nullptr,0};auto valid=readFile(first);desc.CS={valid.data(),valid.size()};
      auto pso=ordinary(device,desc);gpuValue(device,pso.Get(),desc.pRootSignature,0x31415926u+0x135u,"failure_valid_control");
      std::puts("OWNED_D3D12_PIPELINE_CACHE_CASE_PASS failure_completion_retry");return;
    }
    if(!wcscmp(mode,L"--control")) {
      D3D12_SHADER_CACHE_SESSION_DESC sessionDesc{};
      sessionDesc.Identifier={0xa07cca31,1,2,{3,4,5,6,7,8,9,10}};
      sessionDesc.Mode=D3D12_SHADER_CACHE_MODE_DISK;sessionDesc.Flags=D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR;
      ComPtr<ID3D12ShaderCacheSession> session;
      check(device->CreateShaderCacheSession(&sessionDesc,IID_PPV_ARGS(&session)),"independent app session");
      const UINT key=71,value=92;check(session->StoreValue(&key,sizeof(key),&value,sizeof(value)),"app session seed");
      check(device->ShaderCacheControl(driver,D3D12_SHADER_CACHE_CONTROL_FLAGS(
            D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE|D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR)),"driver clear");
      auto root=rootSignature(device,6);desc.pRootSignature=root.Get();
      auto seeded=ordinary(device,desc);const auto path=cacheFile(seeded.Get(),"control");
      check(device->ShaderCacheControl(driver,D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE),"driver disable");
      for(unsigned i=0;i<2;++i){auto pso=ordinary(device,desc);gpuValue(device,pso.Get(),root.Get(),
                                    0x31415926u+0x135u,i?"disabled_second":"disabled_first");}
      check(device->ShaderCacheControl(driver,D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE),"driver enable");
      auto enabled=ordinary(device,desc);gpuValue(device,enabled.Get(),root.Get(),0x31415926u+0x135u,"enabled");
      check(device->ShaderCacheControl(driver,D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR),"driver clear active");
      if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)fail("driver clear left entry");
      gpuValue(device,seeded.Get(),root.Get(),0x31415926u+0x135u,"retained_after_stage_library_clear");
      UINT actual=0,size=sizeof(actual);check(session->FindValue(&key,sizeof(key),&actual,&size),"app session independent");
      if(actual!=value||size!=sizeof(value))fail("app session changed by driver control");
      auto rebuilt=ordinary(device,desc);gpuValue(device,rebuilt.Get(),root.Get(),0x31415926u+0x135u,"clear_rebuild");
      cacheFile(rebuilt.Get(),"control_rebuilt");session->SetDeleteOnDestroy();
      std::puts("OWNED_D3D12_PIPELINE_CACHE_CASE_PASS driver_control_app_independence");return;
    }
    auto pso=ordinary(device,desc);cacheFile(pso.Get(),"child");
    const std::string label=std::string("child_")+std::string(mode+2,mode+wcslen(mode));
    gpuValue(device,pso.Get(),desc.pRootSignature,0x31415926u+0x135u,label.c_str());
    std::printf("OWNED_D3D12_PIPELINE_CACHE_CHILD_PASS mode=%ls\n",mode+2);return;
  }
  DeveloperMode developer;
  check(device->ShaderCacheControl(driver,D3D12_SHADER_CACHE_CONTROL_FLAGS(
        D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE|D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR)),"initial driver clear");
  D3D12_FEATURE_DATA_SHADER_CACHE support{};check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE,&support,sizeof(support)),"cache feature");
  constexpr UINT required=D3D12_SHADER_CACHE_SUPPORT_SINGLE_PSO|D3D12_SHADER_CACHE_SUPPORT_DRIVER_MANAGED_CACHE;
  if((support.SupportFlags&required)!=required)fail("automatic cache required bits");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_CACHE_ARTIFACT_PHASE begin=distinct_compute\n");std::fflush(stderr);
  auto a=ordinary(device,desc);const auto path=cacheFile(a.Get(),"a");gpuValue(device,a.Get(),desc.pRootSignature,0x31415926u+0x135u,"ordinary_a");
  auto aliasDesc=desc;aliasDesc.NodeMask=1;auto alias=ordinary(device,aliasDesc);
  if(cacheFile(alias.Get(),"same_stage_node_one")==path)fail("distinct PSO node key");
  gpuValue(device,alias.Get(),desc.pRootSignature,0x31415926u+0x135u,"distinct_compute_same_stage");
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_CACHE_ARTIFACT_PHASE end=distinct_compute air=1 libraries=1\n");std::fflush(stderr);
  auto bDesc=desc;bDesc.CS={shaderB.data(),shaderB.size()};auto b=ordinary(device,bDesc);
  const auto bPath=cacheFile(b.Get(),"b");gpuValue(device,b.Get(),desc.pRootSignature,0x31415926u^0x135u,"ordinary_b");
  auto otherRoot=rootSignature(device,3);auto rootDesc=desc;rootDesc.pRootSignature=otherRoot.Get();
  auto r=ordinary(device,rootDesc);const auto rootPath=cacheFile(r.Get(),"root");
  gpuValue(device,r.Get(),otherRoot.Get(),0x31415926u+0x135u,"ordinary_root");
  if(path==bPath||path==rootPath||bPath==rootPath)fail("automatic key separation");
  for(auto entry:{desc,bDesc,rootDesc}){auto pso=ordinary(device,entry);gpuValue(device,pso.Get(),entry.pRootSignature,
                                entry.CS.pShaderBytecode==shaderB.data()?0x31415926u^0x135u:0x31415926u+0x135u,
                                entry.pRootSignature==otherRoot.Get()?"memory_root":entry.CS.pShaderBytecode==shaderB.data()?"memory_b":"memory_a");}
  std::puts("OWNED_D3D12_PIPELINE_CACHE_CASE_PASS ordinary_key_memory");
  child(first,second,L"--disk");
  auto good=readFile(path.c_str());
  for(const auto *mutation:{L"--corrupt",L"--truncated",L"--identity",L"--native"}) {
    auto changed=good;
    if(!wcscmp(mutation,L"--corrupt"))changed.back()^=1;
    else if(!wcscmp(mutation,L"--truncated"))changed.pop_back();
    else if(!wcscmp(mutation,L"--identity")){
      cacheLibrarySize(changed);changed[offsetof(CacheMutationHeader,identity)]^=1;rehashCache(changed);
    }else{
      const size_t library=cacheLibrarySize(changed), offset=sizeof(CacheMutationHeader);
      uint64_t fileSize=0;uint16_t major=0,minor=0;
      memcpy(&fileSize,changed.data()+offset+16,8);
      memcpy(&major,changed.data()+offset+6,2);memcpy(&minor,changed.data()+offset+8,2);
      if(fileSize!=library||major!=2||minor!=7)fail("mutation metallib version layout");
      // Native container version is consumed before AIR function HASH/cache reuse.
      // Keep MTLB magic, offsets, byte sizes, function data and PSO relocations intact.
      const uint16_t unsupported=0xffff;memcpy(changed.data()+offset+6,&unsupported,2);
      if(!memcmp(changed.data()+offset+6,good.data()+offset+6,2))fail("native fault did not change bytes");
      rehashCache(changed);
      std::puts("OWNED_D3D12_PIPELINE_CACHE_NATIVE_FAULT field=metallib_version_major before=2 after=65535");
    }
    writeFile(path,changed);child(first,second,mutation);
    if(readFile(path.c_str())==changed)fail("ignored entry not rebuilt");
  }
  std::puts("OWNED_D3D12_PIPELINE_CACHE_CASE_PASS disk_restore_invalid_rebuild");
  // Keep the device-local native artifact cache cold without clearing earlier automatic keys.
  ComPtr<ID3D12Device> concurrentDevice;
  check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&concurrentDevice)),"concurrent device");
  auto concurrentRoot=rootSignature(concurrentDevice.Get(),4);auto shared=desc;shared.pRootSignature=concurrentRoot.Get();
  std::barrier start(8);std::vector<std::thread> threads;ComPtr<ID3D12PipelineState> psos[8];HRESULT results[8]{};
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_CACHE_ARTIFACT_PHASE begin=concurrent_distinct_compute\n");std::fflush(stderr);
  for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{auto entry=shared;entry.NodeMask=i&1u;start.arrive_and_wait();
    results[i]=concurrentDevice->CreateComputePipelineState(&entry,IID_PPV_ARGS(&psos[i]));});
  for(auto &thread:threads)thread.join();
  std::fprintf(stderr, "OWNED_D3D12_PIPELINE_CACHE_ARTIFACT_PHASE end=concurrent_distinct_compute air=1 libraries_max=1\n");std::fflush(stderr);
  for(unsigned i=0;i<8;++i){check(results[i],"concurrent ordinary create");if(!psos[i])fail("concurrent output");
    char label[32];std::snprintf(label,sizeof(label),"concurrent_%u",i);
    gpuValue(concurrentDevice.Get(),psos[i].Get(),concurrentRoot.Get(),0x31415926u+0x135u,label);}
  cacheFile(psos[0].Get(),"concurrent");
  std::puts("OWNED_D3D12_PIPELINE_CACHE_CASE_PASS concurrent_single_producer");
  child(first,second,L"--failure");child(first,second,L"--control");
}

int run(const wchar_t *firstPath,const wchar_t *secondPath,const wchar_t *mode=nullptr) {
  using BackendId=const char*(WINAPI*)();auto identity=reinterpret_cast<BackendId>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  if(!identity||strcmp(identity(),"owned-graphics-d3d12"))fail("backend identity");
  std::printf("OWNED_D3D12_PIPELINE_CACHE_BACKEND id=%s\n",identity());
  ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"create factory");
  ComPtr<IDXGIAdapter1> adapter;check(factory->EnumAdapters1(0,&adapter),"get adapter");
  ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"create device");
  auto shaderA=readFile(firstPath),shaderB=readFile(secondPath);auto root=rootSignature(device.Get(),2);
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();desc.CS={shaderA.data(),shaderA.size()};
  ComPtr<ID3D12Device9> device9;check(device.As(&device9),"device9");
  automatic(device9.Get(),desc,shaderB,firstPath,secondPath,mode);if(mode)return 0;
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
int wmain(int argc,wchar_t **argv){
  try {
    if(argc!=3&&argc!=4)return 2;
    return run(argv[1],argv[2],argc==4?argv[3]:nullptr);
  }catch(const std::exception &){return 1;}
}
