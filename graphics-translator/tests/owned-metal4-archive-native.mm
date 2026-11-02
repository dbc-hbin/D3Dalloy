// Exercise the actual native bridge and optional archive lifecycle. A warm run
// rejects compiler calls, so success proves archive lookup rather than a fast
// driver compile. Each mode runs in a fresh process with the same binary UUID.
#include "../../src/winemetal/unix/winemetal_metal4_pipeline.c"
#include <atomic>
#include <thread>
#include <vector>

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return format==WMTPixelFormatRGBA8Unorm ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatInvalid;
}
namespace {
void require(bool condition,const char* what) {
  if(!condition){std::fprintf(stderr,"METAL4_ARCHIVE_FAIL reason=%s\n",what);std::exit(1);}
}
using Compute=id(*)(id,SEL,id,id,id,NSError**);
using Render=id(*)(id,SEL,id,id,NSError**);
using Serialize=BOOL(*)(id,SEL,NSURL*,NSError**);
Compute originalCompute;Render originalRender;Serialize originalSerialize;
std::atomic_uint computeCalls{0},renderCalls{0};
std::atomic_bool denyCompile{false},failCaptureOnce{false},pauseSerialization{false};
dispatch_semaphore_t entered,proceed;
id computeObserved(id self,SEL selector,id descriptor,id dynamic,id options,NSError** error) {
  ++computeCalls;
  if(denyCompile || (((id<MTL4Compiler>)self).pipelineDataSetSerializer && failCaptureOnce.exchange(false))) {
    if(error)*error=[NSError errorWithDomain:@"ArchiveTest" code:1 userInfo:nil];return nil;
  }
  return originalCompute(self,selector,descriptor,dynamic,options,error);
}
id renderObserved(id self,SEL selector,id descriptor,id options,NSError** error) {
  ++renderCalls;
  if(denyCompile){if(error)*error=[NSError errorWithDomain:@"ArchiveTest" code:1 userInfo:nil];return nil;}
  return originalRender(self,selector,descriptor,options,error);
}
BOOL serializeObserved(id self,SEL selector,NSURL* url,NSError** error) {
  if(pauseSerialization.exchange(false)) {
    dispatch_semaphore_signal(entered);
    require(dispatch_semaphore_wait(proceed,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC))==0,"writer release");
  }
  return originalSerialize(self,selector,url,error);
}
id call(uint32_t operation,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
  struct WMTMetal4Call value{};value.operation=operation;value.args[0]=a;value.args[1]=b;value.args[2]=c;value.args[3]=d;
  require(winemetal_metal4_pipeline_call(&value)==0,"bridge status");
  NSError* error=(NSError*)value.ret[1];
  if(error)std::fprintf(stderr,"ARCHIVE_TEST_ERROR %s\n",error.localizedDescription.UTF8String);
  [error release];return (id)value.ret[0];
}
id function(id<MTLLibrary> library,const char* name) {
  id result=call(WMTMetal4PipelineLibraryFunction,(uint64_t)library,(uint64_t)name);require(result!=nil,"function");return result;
}
struct Fixture {
  id<MTLDevice> device=MTLCreateSystemDefaultDevice();
  id<MTL4Compiler> compiler=nil;
  id<MTLLibrary> library=nil;
  Method computeMethod=nullptr,renderMethod=nullptr;
  Fixture() {
    require(device!=nil,"device");compiler=call(WMTMetal4PipelineNewCompiler,(uint64_t)device);require(compiler!=nil,"compiler");
    static NSString* source=@R"(
#include <metal_stdlib>
using namespace metal;
constant uint archive_bias [[function_constant(0)]];
kernel void plain(device uint* out [[buffer(0)]],uint i [[thread_position_in_grid]]) {out[i]=i*7+3;}
kernel void special(device uint* out [[buffer(0)]],uint i [[thread_position_in_grid]]) {out[i]=i*7+archive_bias;}
[[visible]] uint helper(uint i) {return i*13+9;}
kernel void linked(device uint* out [[buffer(0)]],visible_function_table<uint(uint)> functions [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {out[i]=functions[0](i);}
vertex float4 vertex_main(uint i [[vertex_id]]) {return float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1);}
fragment float4 fragment_main() {return float4(.25,.5,.75,1);}
struct MeshVertex {float4 position [[position]];};
[[mesh,max_total_threads_per_threadgroup(1)]]
void mesh_main(metal::mesh<MeshVertex,void,3,1,topology::triangle> outputMesh) {
  for(uint i=0;i<3;++i) {MeshVertex v;v.position=float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1);
    outputMesh.set_vertex(i,v);outputMesh.set_index(i,i);}
  outputMesh.set_primitive_count(1);
}
)";
    MTLCompileOptions* options=[[MTLCompileOptions alloc] init];options.languageVersion=MTLLanguageVersion4_0;
    MTL4LibraryDescriptor* sourceDescriptor=[[MTL4LibraryDescriptor alloc] init];sourceDescriptor.source=source;sourceDescriptor.options=options;
    NSError* error=nil;library=[compiler newLibraryWithDescriptor:sourceDescriptor error:&error];
    [sourceDescriptor release];[options release];
    if(error)std::fprintf(stderr,"MSL: %s\n",error.localizedDescription.UTF8String);
    require(library!=nil,"library");
    computeMethod=class_getInstanceMethod(object_getClass(compiler),@selector(newComputePipelineStateWithDescriptor:dynamicLinkingDescriptor:compilerTaskOptions:error:));
    renderMethod=class_getInstanceMethod(object_getClass(compiler),@selector(newRenderPipelineStateWithDescriptor:compilerTaskOptions:error:));
    originalCompute=(Compute)method_setImplementation(computeMethod,(IMP)computeObserved);
    originalRender=(Render)method_setImplementation(renderMethod,(IMP)renderObserved);
  }
  void compute(unsigned variant=0,uint32_t bias=19) {
    id entry=nil,helper=nil;
    if(variant==1) {
      WMTFunctionConstant constant{};constant.type=WMTDataTypeUInt;constant.index=0;constant.data.ptr=&bias;
      entry=call(WMTMetal4PipelineSpecializedFunction,(uint64_t)library,(uint64_t)"special",(uint64_t)&constant,1);
    } else entry=function(library,variant==2?"linked":"plain");
    require(entry!=nil,"entry");
    WMTComputePipelineInfo info{};info.compute_function=(uint64_t)entry;info.max_call_stack_depth=variant==2?4:0;
    if(variant==2)helper=function(library,"helper");
    uint64_t helperHandle=(uint64_t)helper;
    auto pipeline=(id<MTLComputePipelineState>)call(variant==2?WMTMetal4PipelineLinkedCompute:WMTMetal4PipelineCompute,
        (uint64_t)compiler,(uint64_t)&info,(uint64_t)&helperHandle,helper?1:0);
    require(pipeline!=nil,"compute PSO");
    id<MTLVisibleFunctionTable> table=nil;
    if(helper) {
      auto handle=(id<MTLFunctionHandle>)call(WMTMetal4PipelineFunctionHandle,(uint64_t)pipeline,(uint64_t)helper);
      require(handle!=nil,"archive-linked handle");
      auto desc=[MTLVisibleFunctionTableDescriptor visibleFunctionTableDescriptor];desc.functionCount=1;
      table=[pipeline newVisibleFunctionTableWithDescriptor:desc];require(table!=nil,"visible table");
      [table setFunction:handle atIndex:0];[handle release];
    }
    auto out=[device newBufferWithLength:128 options:MTLResourceStorageModeShared];std::memset(out.contents,0xff,128);
    auto queue=[device newCommandQueue];auto command=[queue commandBuffer];auto encoder=[command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];[encoder setBuffer:out offset:0 atIndex:0];
    if(table)[encoder setVisibleFunctionTable:table atBufferIndex:1];
    [encoder dispatchThreads:MTLSizeMake(32,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];
    [encoder endEncoding];[command commit];[command waitUntilCompleted];
    require(command.status==MTLCommandBufferStatusCompleted,"GPU compute completion");
    auto values=(const uint32_t*)out.contents;
    for(unsigned i=0;i<32;++i)require(values[i]==(variant==2?i*13+9:i*7+(variant==1?bias:3)),"compute exact output");
    [queue release];[out release];[table release];[pipeline release];[entry release];[helper release];
  }
  void render(bool mesh=false) {
    id vertex=function(library,mesh?"mesh_main":"vertex_main"),fragment=function(library,"fragment_main");
    WMTRenderPipelineInfo info{};info.vertex_function=(uint64_t)vertex;info.fragment_function=(uint64_t)fragment;
    info.colors[0].pixel_format=WMTPixelFormatRGBA8Unorm;info.colors[0].write_mask=15;
    info.rasterization_enabled=1;info.raster_sample_count=1;info.input_primitive_topology=WMTPrimitiveTopologyClassTriangle;
    WMTMeshRenderPipelineInfo meshInfo{};meshInfo.mesh_function=(uint64_t)vertex;meshInfo.fragment_function=(uint64_t)fragment;
    meshInfo.colors[0]=info.colors[0];meshInfo.rasterization_enabled=1;meshInfo.raster_sample_count=1;
    auto pipeline=(id<MTLRenderPipelineState>)call(mesh?WMTMetal4PipelineMesh:WMTMetal4PipelineRender,
        (uint64_t)compiler,mesh?(uint64_t)&meshInfo:(uint64_t)&info);require(pipeline!=nil,"render PSO");
    auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageRenderTarget;
    auto texture=[device newTextureWithDescriptor:desc];auto queue=[device newCommandQueue];auto command=[queue commandBuffer];
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=texture;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    auto encoder=[command renderCommandEncoderWithDescriptor:pass];[encoder setRenderPipelineState:pipeline];
    if(mesh)[encoder drawMeshThreadgroups:MTLSizeMake(1,1,1) threadsPerObjectThreadgroup:MTLSizeMake(1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(1,1,1)];
    else [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit];[command waitUntilCompleted];require(command.status==MTLCommandBufferStatusCompleted,"GPU render completion");
    uint8_t pixels[64];[texture getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    for(unsigned i=0;i<16;++i)require(pixels[i*4]==64&&pixels[i*4+1]==128&&pixels[i*4+2]==191&&pixels[i*4+3]==255,"render exact output");
    [texture release];[queue release];[pipeline release];[vertex release];[fragment release];
  }
  void flush(){call(WMTMetal4PipelineArchiveFlush,(uint64_t)compiler);}
  ~Fixture(){flush();method_setImplementation(computeMethod,(IMP)originalCompute);method_setImplementation(renderMethod,(IMP)originalRender);[compiler release];[library release];[device release];}
};
}
int main(int argc,char** argv) { @autoreleasepool {
  require(argc==2,"mode");Fixture f;const bool warm=!std::strcmp(argv[1],"warm");
  denyCompile=warm;
  f.compute();f.compute(1);f.compute(2);f.render();f.render(true);
  if(std::strcmp(argv[1],"controls"))
    require(warm?(computeCalls==0&&renderCalls==0):(computeCalls>=3&&renderCalls>=2),"cold vs actual archive-hit counts");
  if(!std::strcmp(argv[1],"validation-fallback")) {
    f.flush();auto state=pipeline_archive(f.compiler);require(state!=nil,"validation fallback state");
    auto archives=[state copyArchives];const bool compatible=archives.count!=0;[archives release];
    auto before=computeCalls.load();auto renderBefore=renderCalls.load();
    if(compatible) {
      denyCompile=true;f.compute();f.compute(1);f.compute(2);f.render();f.render(true);denyCompile=false;
      require(computeCalls==before && renderCalls==renderBefore,"validated archive factories bypass compiler");
    } else {f.compute(1,61);f.flush();require(computeCalls==before+1,"validated ordinary fallback");}
    std::printf("METAL4_ARCHIVE_VALIDATION_PASS validation_kept=1 archive_supported=%u invalid_capture_not_published=1 later_pipelines=exact\n",compatible);
  }
  if(!std::strcmp(argv[1],"controls")) {
    denyCompile=false;call(WMTMetal4PipelineArchiveControl,5); // clear+disable
    auto state=pipeline_archive(f.compiler);require(state!=nil,"control state");
    auto before=computeCalls.load();f.compute(1,37);f.flush();require(computeCalls==before+1,"disabled native fallback");
    auto archives=[state copyArchives];require(!archives.count,"disabled archives");[archives release];
    call(WMTMetal4PipelineArchiveControl,2);f.compute(1,41);
    uint64_t generation=0;auto capture=[state beginCapture:&generation];require(capture!=nil,"capture owner");
    Method method=class_getInstanceMethod(object_getClass(capture.pipelineDataSetSerializer),@selector(serializeAsArchiveAndFlushToURL:error:));
    originalSerialize=(Serialize)method_setImplementation(method,(IMP)serializeObserved);[state endCapture:generation success:NO];[capture release];
    entered=dispatch_semaphore_create(0);proceed=dispatch_semaphore_create(0);pauseSerialization=true;
    std::thread writer([&]{@autoreleasepool {f.flush();}});
    require(dispatch_semaphore_wait(entered,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC))==0,"writer enters");
    call(WMTMetal4PipelineArchiveControl,4);
    before=computeCalls.load();f.compute(1,43);require(computeCalls==before+1,"compile proceeds while serializer paused");
    dispatch_semaphore_signal(proceed);writer.join();method_setImplementation(method,(IMP)originalSerialize);
    archives=[state copyArchives];require(!archives.count,"clear prevents late publication");[archives release];
    dispatch_release(entered);dispatch_release(proceed);
    before=computeCalls.load();failCaptureOnce=true;f.compute(1,47);
    require(computeCalls==before+2,"capture failure falls back to ordinary compiler");
    f.compute(1,53);f.flush();archives=[state copyArchives];require(archives.count>0,"enable retries capture after clear");[archives release];
    std::puts("METAL4_ARCHIVE_CONTROL_PASS disable_fallback=1 clear_late_publish=0 compile_during_flush=1 capture_failure_retry=1 recapture=1");
  }
  f.flush();
  std::printf("METAL4_ARCHIVE_PASS mode=%s compute_compiler_calls=%u render_compiler_calls=%u compute_specialized_linked_render_mesh_gpu=exact\n",
      argv[1],computeCalls.load(),renderCalls.load());
} }
