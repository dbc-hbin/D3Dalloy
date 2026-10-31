// Public AMD RR ABI -> owned D3D12 recording -> native MetalFX RR replay -> GPU readback.
// Ported from zzz-wine-yaagl-pr-runtime e9d0e50db84 scripts/rr-public-smoke.cpp.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <vector>
#include "denoisers/include/ffx_denoiser.h"
#include "legacy.h"
using Microsoft::WRL::ComPtr;
static_assert(sizeof(FfxApiResource)==48);
static_assert(sizeof(ffxDispatchDescDenoiser)==448);
static_assert(offsetof(ffxDispatchDescDenoiser,view)==296);
static_assert(offsetof(ffxDispatchDescDenoiser,projection)==360);
static_assert(sizeof(rr_legacy_settings)==128);
static_assert(sizeof(rr_legacy_create)==48);
static_assert(sizeof(rr_frame_10)==368 && sizeof(rr_frame_11)==368);
static_assert(offsetof(rr_frame_10,cameraPositionDelta)==280);
static_assert(offsetof(rr_frame_11,cameraPositionDelta)==284);
static_assert(offsetof(rr_legacy_create,fpMessage)==32 && offsetof(rr_legacy_create,mode)==40);
static_assert(offsetof(rr_frame_10,flags)==360 && offsetof(rr_frame_11,flags)==364);
static_assert(sizeof(rr_legacy_signal)==104 && sizeof(rr_legacy_light)==144);
static_assert(sizeof(rr_legacy_four)==432 && sizeof(rr_legacy_two)==224 && sizeof(rr_legacy_one)==168);
static_assert(sizeof(rr_configure_10)==144 && sizeof(rr_defaults_10)==32);
static_assert(sizeof(rr_legacy_memory)==48 && sizeof(rr_legacy_version)==48);
// The SDK DX12 helper emits an unrelated incomplete-switch warning. Use its
// documented two-field backend descriptor and populate the test resources.
struct BackendDX12 { ffxCreateContextDescHeader header; ID3D12Device* device; };
static FfxApiResource resource(ID3D12Resource* object,
                              uint32_t state=FFX_API_RESOURCE_STATE_COMPUTE_READ) {
    auto desc=object->GetDesc();
    FfxApiResource result{};result.resource=object;result.state=state;
    result.description.type=FFX_API_RESOURCE_TYPE_TEXTURE2D;
    result.description.format=desc.Format==DXGI_FORMAT_R32_FLOAT ?
        FFX_API_SURFACE_FORMAT_R32_FLOAT : FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    result.description.width=UINT(desc.Width);result.description.height=desc.Height;
    result.description.depth=1;result.description.mipCount=1;
    result.description.usage=(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)?FFX_API_RESOURCE_USAGE_UAV:FFX_API_RESOURCE_USAGE_READ_ONLY;
    return result;
}
static void check(bool value,const char* what){if(!value){printf("OWNED_D3D12_FSR_RR_FAIL %s\n",what);exit(1);}}
static void hr(HRESULT value,const char* what){if(FAILED(value)){printf("OWNED_D3D12_FSR_RR_FAIL %s HRESULT %08lx\n",what,(unsigned long)value);exit(1);}}
static void fx(ffxReturnCode_t value,const char* what){if(value){printf("OWNED_D3D12_FSR_RR_FAIL %s FFX %u\n",what,unsigned(value));exit(1);}}
struct Allocations { unsigned allocated=0,freed=0; };
static void* allocate(void* user,uint64_t size){++static_cast<Allocations*>(user)->allocated;return std::malloc(size);}
static void deallocate(void* user,void* pointer){++static_cast<Allocations*>(user)->freed;std::free(pointer);}
static unsigned messages;
static void message(uint32_t,const wchar_t*){++messages;}
static unsigned contextMessages;
static PfnFfxQuery callbackQuery;
static ffxContext* callbackContext;
static bool callbackActive;
static void contextMessage(uint32_t,const wchar_t*){
    ++contextMessages;
    if(callbackContext&&!callbackActive){
        callbackActive=true;
        uint32_t major=0,minor=0,patch=0;rr_legacy_version q{};q.header.type=RR_TYPE(0x0a);
        q.major=&major;q.minor=&minor;q.patch=&patch;
        fx(callbackQuery(callbackContext,&q.header),"reentrant callback query");
        check(major==1,"callback context remains live");
        callbackActive=false;
    }
}
static void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
static float run(uint32_t version, uint32_t mode=0, bool temporal=false, bool wrongDelta=false){
    constexpr unsigned w=32,h=16;
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"device");
    auto module=LoadLibraryW(L"amd_fidelityfx_denoiser_dx12.dll");check(module,"load RR builtin");
    auto create=(PfnFfxCreateContext)GetProcAddress(module,"ffxCreateContext");
    auto destroy=(PfnFfxDestroyContext)GetProcAddress(module,"ffxDestroyContext");
    auto dispatch=(PfnFfxDispatch)GetProcAddress(module,"ffxDispatch");
    auto configure=(PfnFfxConfigure)GetProcAddress(module,"ffxConfigure");
    auto query=(PfnFfxQuery)GetProcAddress(module,"ffxQuery");
    check(create&&destroy&&dispatch&&configure&&query,"exports");
    ffxConfigureDescGlobalDebug debug{};debug.header.type=FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    debug.effectId=FFX_API_EFFECT_ID_DENOISER;debug.fpMessage=message;debug.debugLevel=FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
    fx(configure(nullptr,&debug.header),"debug callback");
    uint64_t versionCount=0;
    ffxQueryDescGetVersions versions{};versions.header.type=FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType=FFX_API_EFFECT_ID_DENOISER;versions.device=device.Get();versions.outputCount=&versionCount;
    fx(query(nullptr,&versions.header),"versions");check(versionCount==1,"one RR provider");
    BackendDX12 backend{};backend.header.type=FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12,0x02);backend.device=device.Get();
    ffxCreateContextDescDenoiser info{};info.header.type=FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;info.header.pNext=&backend.header;
    info.version=FFX_DENOISER_VERSION;info.maxRenderSize={w,h};info.signalFlags=0x7f;
    rr_legacy_create legacy{};legacy.header=info.header;legacy.version=version;legacy.maxRenderSize={w,h};
    legacy.mode=mode;legacy.flags=2;legacy.fpMessage=contextMessage;
    bool old=version!=FFX_DENOISER_VERSION;
    uint32_t mask=old?((mode==0?0x36:mode==1?0x30:0x20)|8):0x7f;
    auto createDesc=old?&legacy.header:&info.header;
    ffxContext context=nullptr;
    if(old){unsigned before=messages;legacy.version=0x00403000;check(create(&context,createDesc,nullptr)==FFX_API_RETURN_ERROR_PARAMETER,"invalid API version");legacy.version=version;check(messages==before+1,"error callback");}
    Allocations allocations;ffxAllocationCallbacks callbacks{&allocations,allocate,deallocate};
    fx(create(&context,createDesc,&callbacks),"RR create");check(allocations.allocated==1,"allocation callback");
    if(old){callbackQuery=query;callbackContext=&context;}
    float defaultBias=-1;
    ffxQueryDescDenoiserGetDefaultKeyValue defaults{};defaults.header.type=FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
    defaults.key=FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS;defaults.count=1;defaults.data=&defaultBias;
    if(old)defaults.header.type=RR_TYPE(0x0b);
    float bias=.5f;ffxConfigureDescDenoiserKeyValue config{};config.header.type=FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
    config.key=defaults.key;config.count=1;config.data=&bias;
    if(old)config.header.type=RR_TYPE(0x08);
    if(version==RR_10){
        rr_legacy_settings settings{};rr_defaults_10 d{};d.header.type=RR_TYPE(0x0b);d.device=device.Get();d.defaultSettings=&settings;
        check(query(nullptr,&d.header)==FFX_API_RETURN_NO_PROVIDER,"ambiguous contextless defaults rejected");
        fx(query(&context,&d.header),"1.0 defaults");check(settings.historyRejectionStrength==0,"provider neutral history default");
        defaultBias=settings.stabilityBias;settings.stabilityBias=bias;
        rr_configure_10 c{};c.header.type=RR_TYPE(0x08);c.settings=settings;
        fx(configure(&context,&c.header),"1.0 settings");
        c.settings.historyRejectionStrength=2;check(configure(&context,&c.header)==FFX_API_RETURN_ERROR_PARAMETER,"1.0 invalid strength");
    }else{
        fx(query(&context,&defaults.header),"default setting");
        fx(configure(&context,&config.header),"configure");
    }
    check(std::isfinite(defaultBias)&&defaultBias>=0,"valid temporal default");
    FfxApiFloatBounds debugBounds{0,100};
    config.key=FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS;config.data=&debugBounds;
    if(!old)check(configure(&context,&config.header)==FFX_API_RETURN_NO_PROVIDER,"unsupported debug setting rejected");
    if(old){
        uint32_t major=0,minor=0,patch=99;rr_legacy_version q{};q.header.type=RR_TYPE(0x0a);q.device=device.Get();q.major=&major;q.minor=&minor;q.patch=&patch;
        fx(query(nullptr,&q.header),"provider version");check(major==1&&minor==2&&patch==0,"highest provider API");
        fx(query(&context,&q.header),"legacy context version");check(major==1&&minor==((version>>12)&0x3ff)&&patch==0,"selected contract version");
        ffxConfigureDescDenoiserKeyValue mismatch=config;mismatch.header.type=FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
        unsigned before=contextMessages;
        check(configure(&context,&mismatch.header)==FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE,"configure type version mismatch");
        check(contextMessages==before+1,"legacy per-context message callback");
    }
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"queue");
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    D3D12_ROOT_SIGNATURE_DESC rd{};ComPtr<ID3DBlob> root,shader,errors;
    hr(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root,&errors),"root bytes");
    ComPtr<ID3D12RootSignature> signature;hr(device->CreateRootSignature(0,root->GetBufferPointer(),root->GetBufferSize(),IID_PPV_ARGS(&signature)),"root");
    const char code[]="[numthreads(1,1,1)] void main() {}";hr(D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&shader,&errors),"caller shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=signature.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;hr(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"pipeline");
    list->SetComputeRootSignature(signature.Get());list->SetPipelineState(pipeline.Get());
    ComPtr<ID3D12Resource> inputs[7],outputs[7];
    FfxApiDenoiserSignal signals[7]{};
    ComPtr<ID3D12DescriptorHeap> rtv;D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,UINT(5+2*std::size(inputs)),D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"rtv heap");
    auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    unsigned rtvIndex=0;
    auto make=[&](DXGI_FORMAT format,const float* value,bool writable=false){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;
        d.Format=format;d.Flags=value?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(writable)d.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> result;
        hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,value?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&result)),"texture");
        if(value){auto handle=rtv->GetCPUDescriptorHandleForHeapStart();handle.ptr+=rtvIndex++*stride;device->CreateRenderTargetView(result.Get(),nullptr,handle);
            list->ClearRenderTargetView(handle,value,0,nullptr);transition(list.Get(),result.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,
                writable?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        return result;
    };
    const float depthValue[]{1,0,0,0},motionValue[]{0,0,0,0},normalValue[]{old?1.f:.5f,.5,.5,0},albedoValue[]{1,1,1,1};
    auto depth=make(DXGI_FORMAT_R32_FLOAT,depthValue);auto motion=make(DXGI_FORMAT_R32G32B32A32_FLOAT,motionValue);
    auto normal=make(DXGI_FORMAT_R32G32B32A32_FLOAT,normalValue);auto specular=make(DXGI_FORMAT_R32G32B32A32_FLOAT,albedoValue);
    const float diffuseValue[]{1,1,1,version==0x00400000?0.f:1.f};
    auto diffuse=make(DXGI_FORMAT_R32G32B32A32_FLOAT,diffuseValue);
    for(unsigned s=0;s<7;++s){float v=s==3?65504.f:float(s+1)/8;float value[]{v,v,v,float(s+1)};
        inputs[s]=make(DXGI_FORMAT_R32G32B32A32_FLOAT,value);signals[s].input=resource(inputs[s].Get());}
    for(unsigned s=0;s<7;++s){float value[]{0,0,0,float(13+s)};
        outputs[s]=make(DXGI_FORMAT_R32G32B32A32_FLOAT,value,true);
        signals[s].output=resource(outputs[s].Get(),FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);}
    ffxDispatchDescDenoiser frame{};frame.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER;frame.commandList=list.Get();
    frame.linearDepth=resource(depth.Get());frame.motionVectors=resource(motion.Get());frame.normals=resource(normal.Get());
    frame.specularAlbedo=resource(specular.Get());frame.diffuseAlbedo=resource(diffuse.Get());
    frame.motionVectorScale={1,1,1};frame.linearDepthBounds={0,100};frame.renderSize={w,h};frame.flags=FFX_DENOISER_DISPATCH_RESET;
    float view[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};float projection[16]{1,0,0,0,0,1,0,0,0,0,100.f/99.9f,1,0,0,-10.f/99.9f,0};
    memcpy(&frame.view,view,sizeof(view));memcpy(&frame.projection,projection,sizeof(projection));
    ffxDispatchDescDenoiserAmbientOcclusion ao{};ao.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION;ao.signal=signals[0];
    ffxDispatchDescDenoiserDirectDiffuse dd{};dd.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;dd.signal=signals[1];
    ffxDispatchDescDenoiserDirectSpecular ds{};ds.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;ds.signal=signals[2];
    ffxDispatchDescDenoiserDominantLight light{};light.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT;light.signal=signals[3];light.direction={0,0,-1};light.emission={1,1,1};light.angularRadius=.01f;
    ffxDispatchDescDenoiserIndirectDiffuse id{};id.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;id.signal=signals[4];
    ffxDispatchDescDenoiserIndirectSpecular is{};is.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;is.signal=signals[5];
    ffxDispatchDescDenoiserSpecularOcclusion so{};so.header.type=FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION;so.signal=signals[6];
    frame.header.pNext=&ao.header;ao.header.pNext=&dd.header;dd.header.pNext=&ds.header;ds.header.pNext=&light.header;light.header.pNext=&id.header;id.header.pNext=&is.header;is.header.pNext=&so.header;
    if(!old){
        check(dispatch(&context,&ao.header)==FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE,"base descriptor must be head");
        so.header.pNext=&frame.header;check(dispatch(&context,&frame.header)==FFX_API_RETURN_ERROR_PARAMETER,"cyclic chain rejected");so.header.pNext=nullptr;
        fx(dispatch(&context,&frame.header),"RR seven-signal record");
    }else{
        rr_frame_10 f10{};rr_frame_11 f11{};
        auto fill=[&](auto& f){
            f.header.type=RR_TYPE(0x02);f.commandList=list.Get();f.linearDepth=frame.linearDepth;f.motionVectors=frame.motionVectors;f.normals=frame.normals;
            f.specularAlbedo=frame.specularAlbedo;f.diffuseAlbedo=frame.diffuseAlbedo;f.motionVectorScale.x=f.motionVectorScale.y=1;
            f.cameraRight={0,0,-1};f.cameraUp={0,1,0};f.cameraForward={version==RR_10?-1.f:1.f,0,0};
            f.cameraAspectRatio=2;f.cameraNear=.1f;f.cameraFar=100;f.cameraFovAngleVertical=1.5707963f;f.renderSize={w,h};f.flags=1;
        };
        fill(f10);fill(f11);f11.motionVectorScale.z=1;
        rr_legacy_four four{};four.header.type=RR_TYPE(0x03);
        rr_legacy_two two{};two.header.type=RR_TYPE(0x04);
        rr_legacy_one one{};one.header.type=RR_TYPE(0x05);one.fusedAlbedo=frame.diffuseAlbedo;
        auto copy=[](rr_legacy_signal& to,const FfxApiDenoiserSignal& from){to.input=from.input;to.output=from.output;};
        copy(four.indirectSpecularRadiance,signals[5]);copy(four.indirectDiffuseRadiance,signals[4]);
        copy(four.directSpecularRadiance,signals[2]);copy(four.directDiffuseRadiance,signals[1]);
        copy(two.specularRadiance,signals[5]);copy(two.diffuseRadiance,signals[4]);copy(one.radiance,signals[5]);
        rr_legacy_light dominant{};dominant.header.type=RR_TYPE(0x07);copy(dominant.dominantLightVisibility,signals[3]);
        dominant.dominantLightDirection={0,0,-1};dominant.dominantLightEmission={1,1,1};
        auto signalDesc=mode==0?&four.header:mode==1?&two.header:&one.header;signalDesc->pNext=&dominant.header;
        auto desc=version==RR_10?&f10.header:&f11.header;desc->pNext=signalDesc;
        check(dispatch(&context,&frame.header)==FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE,"dispatch type version mismatch");
        auto saved=signalDesc->type;signalDesc->type=mode==0?RR_TYPE(0x04):RR_TYPE(0x03);
        check(dispatch(&context,desc)==FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE,"mode descriptor mismatch");signalDesc->type=saved;
        fx(dispatch(&context,desc),"legacy signal record");
        if(temporal){
            auto clear=[&](ID3D12Resource* object,unsigned index,const float* value,const D3D12_RECT* rect=nullptr){
                transition(list.Get(),object,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto target=rtv->GetCPUDescriptorHandleForHeapStart();target.ptr+=index*stride;
                list->ClearRenderTargetView(target,value,rect?1:0,rect);
                transition(list.Get(),object,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            };
            const float nextDepth[]{2,0,0,0};
            float nextMotion[]{0,0,version==RR_10?1.f:-1.f,0};
            if(wrongDelta)nextMotion[2]=-nextMotion[2];
            clear(depth.Get(),0,nextDepth);clear(motion.Get(),1,nextMotion);
            const float dark[]{0,0,0,6},bright[]{1,1,1,6};
            clear(inputs[5].Get(),10,dark);
            for(unsigned x=1;x<w;x+=2){
                D3D12_RECT rect{LONG(x),0,LONG(x+1),LONG(h)};
                clear(inputs[5].Get(),10,bright,&rect);
            }
            f10.flags=f11.flags=0;f10.frameIndex=f11.frameIndex=1;
            fx(dispatch(&context,desc),"legacy temporal record");
        }
    }
    FfxApiEffectMemoryUsage usage{};ffxQueryDescDenoiserGetGPUMemoryUsage memory{};memory.header.type=FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE;memory.gpuMemoryUsage=&usage;
    rr_legacy_memory oldMemory{};oldMemory.header.type=RR_TYPE(0x09);oldMemory.gpuMemoryUsage=&usage;
    fx(query(&context,old?&oldMemory.header:&memory.header),"memory usage");printf("OWNED_D3D12_FSR_RR_MEMORY api=%x mode=%u bytes=%llu\n",version,mode,(unsigned long long)usage.totalUsageInBytes);
    check(destroy(&context,nullptr)==FFX_API_RETURN_ERROR_PARAMETER,"allocator mismatch rejected");
    if(!temporal){
        fx(destroy(&context,&callbacks),"destroy before replay");check(!context,"handle cleared");
        callbackContext=nullptr;callbackQuery=nullptr;
        check(allocations.freed==1,"deallocation callback");
    }
    ComPtr<ID3D12Resource> readbacks[7];D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[7]{};
    for(unsigned s=0;s<7;++s){if(!(mask&(1u<<s)))continue;transition(list.Get(),outputs[s].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto desc=outputs[s]->GetDesc();UINT64 size;device->GetCopyableFootprints(&desc,0,1,0,&footprints[s],nullptr,nullptr,&size);
        D3D12_RESOURCE_DESC rb{};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=size;rb.Height=1;rb.DepthOrArraySize=rb.MipLevels=rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rb,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readbacks[s])),"readback");
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=outputs[s].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readbacks[s].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprints[s];list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
    hr(list->Close(),"close");ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event,"event");hr(fence->SetEventOnCompletion(1,event),"subscribe completion");hr(queue->Signal(fence.Get(),1),"signal");
    check(WaitForSingleObject(event,60000)==WAIT_OBJECT_0,"GPU completion");CloseHandle(event);hr(device->GetDeviceRemovedReason(),"GPU status");
    float specularCenter=0;
    for(unsigned s=0;s<7;++s){if(!(mask&(1u<<s)))continue;void* mapped=nullptr;hr(readbacks[s]->Map(0,nullptr,&mapped),"map");
        auto p=(float*)((unsigned char*)mapped+footprints[s].Offset+(h/2)*footprints[s].Footprint.RowPitch+(w/2)*16);
        printf("OWNED_D3D12_FSR_RR_SIGNAL api=%x mode=%u temporal=%d signal=%u center=%g,%g,%g,%g\n",version,mode,int(temporal),s,p[0],p[1],p[2],p[3]);float expected=s==3?1:float(s+1)/8;
        if(temporal&&s==5)check(std::isfinite(p[0])&&p[0]>0&&p[0]<1,"temporal output range");
        else check(std::isfinite(p[0])&&std::abs(p[0]-expected)<.01,"independent signal output");
        if(s==5)specularCenter=p[0];
        if(s!=0&&s!=3&&s!=6)check(std::abs(p[3]-float(old?s+1:13+s))<.001,"version-correct preserved alpha");D3D12_RANGE none{0,0};readbacks[s]->Unmap(0,&none);}
    if(temporal){
        fx(destroy(&context,&callbacks),"destroy after temporal execution");
        callbackContext=nullptr;callbackQuery=nullptr;
        check(!context&&allocations.freed==1,"temporal context released");
    }
    hr(allocator->Reset(),"retire execution leases");FreeLibrary(module);
    return specularCenter;
}
static void requireOwnedBackend(){
    auto d3d12=GetModuleHandleW(L"d3d12.dll");check(d3d12,"d3d12 loaded");
    using BackendId=const char*(WINAPI*)();
    auto id=reinterpret_cast<BackendId>(GetProcAddress(d3d12,"OwnedGraphicsBackendId"));
    check(id&&!strcmp(id(),"owned-graphics-d3d12"),"owned D3D12 backend");
    printf("OWNED_D3D12_FSR_RR_BACKEND id=%s\n",id());
}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    run(FFX_DENOISER_VERSION);
    requireOwnedBackend();
    for(auto version:{RR_10,RR_11})for(unsigned mode=0;mode<3;++mode)run(version,mode);
    float accepted[2]{};
    for(unsigned i=0;i<2;++i){
        auto version=i?RR_11:RR_10;
        accepted[i]=run(version,2,true);
        auto rejected=run(version,2,true,true);
        check(accepted[i]>rejected+.1f,"public legacy depth delta selects history");
    }
    check(std::abs(accepted[0]-accepted[1])<.0001f,"public legacy temporal versions agree");
    puts("OWNED_D3D12_FSR_RR_LEGACY_TEMPORAL_PASS versions=2 depth_delta=selects_history");
    puts("OWNED_D3D12_FSR_RR_PASS api12=1 legacy_modes=6 signals=7 replay=destroy_before_execute");
}
