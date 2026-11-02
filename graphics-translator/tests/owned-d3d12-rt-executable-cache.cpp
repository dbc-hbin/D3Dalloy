// Reuse the full public DXR geometry/table oracle. No private API exposes or
// mutates native executables; actual creation counts come from owned trace.
#define wmain retained_raytracing_fixture_main
#include "owned-d3d12-raytracing.cpp"
#undef wmain
#include <barrier>
#include <thread>

namespace {
void rtCacheRequire(bool value,const char* reason) {
  if(!value){std::fprintf(stderr,"OWNED_D3D12_RT_EXECUTABLE_FAIL reason=%s\n",reason);ExitProcess(1);}
}
void rtCachePhase(const char* name,bool begin) {
  char line[256];const int count=std::snprintf(line,sizeof(line),"OWNED_D3D12_RT_EXECUTABLE_PHASE %s=%s\n",begin?"begin":"end",name);
  DWORD written=0;rtCacheRequire(count>0 && count<int(sizeof(line)) &&
      WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(count),&written,nullptr) && written==DWORD(count),"atomic phase marker");
}
struct DeveloperCacheControl {
  HKEY key=nullptr;DWORD previous=0,size=sizeof(previous),type=0;bool existed=false;
  DeveloperCacheControl() {
    rtCacheRequire(RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",0,nullptr,0,
        KEY_QUERY_VALUE|KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS,"private-prefix developer key");
    existed=RegQueryValueExW(key,L"AllowDevelopmentWithoutDevLicense",nullptr,&type,reinterpret_cast<BYTE*>(&previous),&size)==ERROR_SUCCESS;
    rtCacheRequire(!existed || (type==REG_DWORD&&size==sizeof(previous)),"existing developer value type");
    DWORD enabled=1;rtCacheRequire(RegSetValueExW(key,L"AllowDevelopmentWithoutDevLicense",0,REG_DWORD,
        reinterpret_cast<const BYTE*>(&enabled),sizeof(enabled))==ERROR_SUCCESS,"enable private-prefix cache control");
  }
  ~DeveloperCacheControl(){if(key){if(existed)RegSetValueExW(key,L"AllowDevelopmentWithoutDevLicense",0,REG_DWORD,
      reinterpret_cast<const BYTE*>(&previous),sizeof(previous));else RegDeleteValueW(key,L"AllowDevelopmentWithoutDevLicense");RegCloseKey(key);}}
};
int exactTrace(stateobj::Runner& run,const stateobj::Scene& scene,ID3D12StateObject* object,uint32_t tag,bool localOverride=false) {
  stateobj::writeTable(run,object);
  run.hitGroup(0,object,L"SoHitGroup",tag);
  run.hitGroup(1,object,L"SoHitGroup",tag+1);
  run.hitGroup(2,object,L"SoProcGroup",tag+2);
  run.miss(0,object,L"SoMiss",tag+3);
  run.miss(2,object,L"SoMissCbv",tag+4,!localOverride);
  std::vector<stateobj::LaneRay> rays={stateobj::atGeometry(0),stateobj::atGeometry(1),stateobj::atGeometry(2),
      stateobj::ray(-10,0,-1,0,0,1),stateobj::ray(-10,0,-1,0,0,1,0,0,1,2)};
  auto words=run.trace("native-executable-cache",object,scene.tlas->GetGPUVirtualAddress(),rays);
  int bad=stateobj::expect("native-executable-cache",words,0,stateobj::hit(0,tag));
  bad+=stateobj::expect("native-executable-cache",words,1,stateobj::hit(1,tag+1,0,1));
  bad+=stateobj::expect("native-executable-cache",words,2,stateobj::hit(2,tag+2));
  bad+=stateobj::expect("native-executable-cache",words,3,stateobj::missOf(rays[3],tag+3));
  bad+=stateobj::expect("native-executable-cache",words,4,stateobj::missOf(rays[4],tag+4));
  return bad;
}
}
int wmain(int argc,wchar_t** argv) {
  rtCacheRequire(argc==3,"frozen contract and state-object DXIL paths");std::setvbuf(stdout,nullptr,_IONBF,0);
  auto backend=reinterpret_cast<const char*(WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"),"OwnedGraphicsBackendId"));
  rtCacheRequire(backend && !std::strcmp(backend(),"owned-graphics-d3d12"),"owned backend");
  std::printf("OWNED_D3D12_RT_EXECUTABLE_BACKEND id=%s\n",backend());
  ComPtr<ID3D12Device5> device;check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"cache device");
  Context context(device.Get());auto contractDxil=readFile(argv[1]);auto stateDxil=readFile(argv[2]);
  contract::Runner geometry(device.Get(),context,contractDxil);stateobj::Runner run(device.Get(),context,stateDxil,geometry);
  auto scene=stateobj::buildScene(run);auto library=run.library();
  std::array<ComPtr<ID3D12StateObject>,4> objects;std::array<HRESULT,4> results{};std::array<std::thread,4> threads;
  std::barrier start(4);
  rtCachePhase("concurrent-cold",true);
  for(unsigned i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{start.arrive_and_wait();
    results[i]=run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library}},objects[i]);});
  for(auto& thread:threads)thread.join();rtCachePhase("concurrent-cold",false);
  std::array<std::array<uint8_t,32>,4> identifiers;
  for(unsigned i=0;i<objects.size();++i) {
    check(results[i],"concurrent state-object create");const void* id=stateobj::Runner::identifier(objects[i].Get(),L"SoRayGen");
    rtCacheRequire(id!=nullptr,"per-state shader identifier");std::memcpy(identifiers[i].data(),id,32);
    rtCacheRequire(!exactTrace(run,scene,objects[i].Get(),0x3100+i*16),"shared native immutable executable GPU output");
  }
  std::puts("OWNED_D3D12_RT_EXECUTABLE_CASE case=concurrent-cold objects=4 gpu_dispatches=4 triangle_procedural_miss_localroots=exact");
  rtCachePhase("warm",true);ComPtr<ID3D12StateObject> warm;
  check(run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library}},warm),"warm create");rtCachePhase("warm",false);
  rtCacheRequire(!exactTrace(run,scene,warm.Get(),0x5100),"warm mutable table isolation");
  for(unsigned i=0;i<objects.size();++i)rtCacheRequire(!std::memcmp(identifiers[i].data(),stateobj::Runner::identifier(objects[i].Get(),L"SoRayGen"),32),"state identifiers unmodified");
  std::puts("OWNED_D3D12_RT_EXECUTABLE_CASE case=warm objects=1 gpu_dispatches=1 identifiers=preserved");
  const WCHAR* miss[]={L"SoMissCbv"};D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION association{L"SoLocalRS",1,miss};
  std::array<ComPtr<ID3D12StateObject>,2> variants;rtCachePhase("local-root-variant",true);
  for(auto& variant:variants)check(run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION,&association}},variant),"root-layout variant");
  rtCachePhase("local-root-variant",false);
  for(unsigned i=0;i<variants.size();++i)rtCacheRequire(!exactTrace(run,scene,variants[i].Get(),0x6100+i*16,true),"different linkage/local root key");
  std::puts("OWNED_D3D12_RT_EXECUTABLE_CASE case=local-root-variant objects=2 gpu_dispatches=2 different_local_layout=exact");
  DeveloperCacheControl permission;ComPtr<ID3D12Device9> device9;check(device.As(&device9),"cache control interface");
  rtCachePhase("clear-live",true);check(device9->ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED,
      D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR),"clear native cache");
  ComPtr<ID3D12StateObject> afterClear;check(run.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library}},afterClear),"recreate after clear");
  rtCachePhase("clear-live",false);
  rtCacheRequire(!exactTrace(run,scene,objects[0].Get(),0x7100),"old executable survives membership retirement");
  rtCacheRequire(!exactTrace(run,scene,afterClear.Get(),0x7200),"fresh generation output");
  variants={};warm.Reset();for(unsigned i=1;i<objects.size();++i)objects[i].Reset();
  stateobj::writeTable(run,objects[0].Get());
  auto words=run.run("release-before-submit",scene.tlas->GetGPUVirtualAddress(),{stateobj::atGeometry(2)},
      [&](ID3D12GraphicsCommandList4* list,const D3D12_DISPATCH_RAYS_DESC& desc){
        list->SetPipelineState1(objects[0].Get());list->DispatchRays(&desc);objects[0].Reset();});
  rtCacheRequire(!stateobj::expect("released-old-executable",words,0,stateobj::hit(2,0x1002)),"released caller retained through GPU completion");
  std::puts("OWNED_D3D12_RT_EXECUTABLE_CASE case=clear-live objects=1 gpu_dispatches=3 old_and_new=exact released_before_submit=1");
  check(device->GetDeviceRemovedReason(),"device alive");
  std::puts("OWNED_D3D12_RT_EXECUTABLE_PASS cases=4 gpu_dispatches=10 mutable_tables_and_localroots=isolated");return 0;
}
