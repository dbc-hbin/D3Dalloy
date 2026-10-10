// SPDX-License-Identifier: LGPL-2.1-or-later
// Native production Unix-plugin oracle. Build/run commands are in local://port-native-binding.md.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <sys/sysctl.h>
#include <unistd.h>
#include "../../src/winemetal/winemetal_metal4.h"

using UnixCall = int32_t (*)(void *);
static UnixCall bridge;
static void require(bool ok, const char *reason) {
  if (!ok) { std::fprintf(stderr, "METAL4_BINDING_FAIL %s\n", reason); std::exit(1); }
}
static uint64_t handle(id object) { return reinterpret_cast<uintptr_t>(object); }
static uint64_t call(uint32_t operation, std::initializer_list<uint64_t> args) {
  struct WMTMetal4Call c{}; c.operation=operation;
  unsigned i=0; for (uint64_t arg : args) c.args[i++]=arg;
  int32_t status=bridge(&c);
  if (status) { std::fprintf(stderr,"METAL4_BINDING_FAIL operation=%u status=%08x\n",operation,uint32_t(status)); std::exit(1); }
  return c.ret[0];
}
template<class T> static void command(uint64_t encoder, uint32_t operation, T &c) {
  call(operation,{encoder,reinterpret_cast<uintptr_t>(&c)});
}
static id member(id object, const char *name) {
  Ivar ivar=class_getInstanceVariable(object_getClass(object),name);
  require(ivar!=nullptr,"missing production object ivar");
  return object_getIvar(object,ivar);
}
// This is a private test oracle, not a bridge ABI. Resolve the ivar by name and
// copy only the machine counters; changing their layout requires updating this oracle.
struct Counters { uint64_t tableNew,tableSet,tableBind,residencyNew,residencyAdd,residencyRemove,residencyCommit; };
static Counters counters(uint64_t buffer) {
  id recording=member(reinterpret_cast<id>(buffer),"recording");
  Ivar ivar=class_getInstanceVariable(object_getClass(recording),"counters");
  require(ivar!=nullptr,"missing production counters");
  Counters result{};
  std::memcpy(&result,reinterpret_cast<const char *>(recording)+ivar_getOffset(ivar),sizeof(result));
  return result;
}
static void computeBuffer(uint64_t encoder, id<MTLBuffer> buffer, uint64_t offset, uint8_t index) {
  wmtcmd_compute_setbuffer c{}; c.type=WMTComputeCommandSetBuffer; c.buffer=handle(buffer); c.offset=offset; c.index=index;
  command(encoder,WMTMetal4ComputeCommands,c);
}
static void dispatch(uint64_t encoder) {
  wmtcmd_compute_dispatch c{}; c.type=WMTComputeCommandDispatchThreads; c.size={1,1,1};
  command(encoder,WMTMetal4ComputeCommands,c);
}
static uint64_t compute(uint64_t buffer,id<MTLComputePipelineState> pso) {
  uint64_t encoder=call(WMTMetal4ComputeEncoder,{buffer,0}); require(encoder!=0,"compute encoder");
  wmtcmd_compute_setpso c{}; c.type=WMTComputeCommandSetPSO; c.pso=handle(pso); c.threadgroup_size={1,1,1};
  command(encoder,WMTMetal4ComputeCommands,c); return encoder;
}
static void render(uint64_t buffer,id<MTLRenderPipelineState> pso,id<MTLTexture> target,id<MTLBuffer> source,uint64_t offset) {
  WMTRenderPassInfo pass{}; pass.colors[0].texture=handle(target);
  pass.colors[0].load_action=WMTLoadActionClear; pass.colors[0].store_action=WMTStoreActionStore;
  pass.default_raster_sample_count=1; pass.render_target_array_length=1;
  pass.render_target_width=1; pass.render_target_height=1;
  uint64_t encoder=call(WMTMetal4RenderEncoder,{buffer,reinterpret_cast<uintptr_t>(&pass)});
  require(encoder!=0,"render encoder");
  wmtcmd_render_setpso p{}; p.type=WMTRenderCommandSetPSO; p.pso=handle(pso); command(encoder,WMTMetal4RenderCommands,p);
  wmtcmd_render_setviewport v{}; v.type=WMTRenderCommandSetViewport; v.viewport={0,0,1,1,0,1}; command(encoder,WMTMetal4RenderCommands,v);
  wmtcmd_render_setbuffer b{}; b.type=WMTRenderCommandSetFragmentBuffer; b.buffer=handle(source); b.offset=offset; b.index=0;
  command(encoder,WMTMetal4RenderCommands,b); command(encoder,WMTMetal4RenderCommands,b);
  wmtcmd_render_draw d{}; d.type=WMTRenderCommandDraw; d.primitive_type=WMTPrimitiveTypeTriangle; d.vertex_count=3; d.instance_count=1;
  command(encoder,WMTMetal4RenderCommands,d);
  call(WMTMetal4EndEncoding,{encoder});
}
static void finish(uint64_t buffer,id<MTL4CommandQueue> queue) {
  call(WMTMetal4EndCommandBuffer,{buffer});
  uint64_t submission=call(WMTMetal4Commit,{handle(queue),reinterpret_cast<uintptr_t>(&buffer),1});
  require(submission!=0,"submission owner");
  call(WMTMetal4WaitSubmission,{submission});
  WMTMetal4SubmissionInfo info{}; call(WMTMetal4GetSubmissionInfo,{submission,reinterpret_cast<uintptr_t>(&info)});
  require(info.completed && !info.error_handle,"native GPU completion without error");
  require([(id<MTLResidencySet>)member(reinterpret_cast<id>(buffer),"residency") allocationCount]==0,"retired residency is empty");
  [reinterpret_cast<id>(submission) release];
}
static void word(const char *name,uint32_t actual,uint32_t expected) {
  std::printf("METAL4_BINDING_WORD name=%s actual=%08x expected=%08x\n",name,actual,expected);
  require(actual==expected,name);
}

int main(int argc,char **argv) {
  require(argc==2 || (argc==3 && (!std::strcmp(argv[2],"explicit") || !std::strcmp(argv[2],"encoder") ||
                                 !std::strcmp(argv[2],"retired"))),
          "usage: owned-metal4-binding-reuse <fresh-native-winemetal.so> [explicit|encoder|retired]");
  const char *negative=argc==3 ? argv[2] : "";
  auto requireUnbound = [&](uint64_t encoder, const char *phase) {
    if (std::strcmp(negative,phase)) return;
    // Metal API validation must reject the required slot. A leaked nonnil
    // binding would let this dispatch through and fail this negative oracle.
    std::fprintf(stderr,"METAL4_BINDING_EXPECT_UNBOUND phase=%s index=2\n",phase); std::fflush(stderr);
    dispatch(encoder);
    require(false,"required null binding unexpectedly accepted");
  };
  alarm(60); // Bound the exact production completion wait; no sleeps or polling.
#if !defined(__arm64__)
  require(false,"probe must be built ARM64");
#endif
  int translated=0; size_t size=sizeof(translated);
  if (sysctlbyname("sysctl.proc_translated",&translated,&size,nullptr,0)==0) require(translated==0,"probe must not be translated");
  require(setenv("OWNED_GRAPHICS_TRACE","1",1)==0,"enable counters");
  void *plugin=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
  if (!plugin) { std::fprintf(stderr,"METAL4_BINDING_FAIL dlopen=%s\n",dlerror()); return 1; }
  auto calls=reinterpret_cast<UnixCall *>(dlsym(plugin,"__wine_unix_call_funcs"));
  require(calls!=nullptr,"real Unix-plugin dispatch table"); bridge=calls[314];
  @autoreleasepool {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    require(device && [device supportsFamily:MTLGPUFamilyMetal4],"Metal4 device");
    std::printf("METAL4_BINDING_NATIVE arch=arm64 translated=%d plugin=%s device=%s\n",translated,argv[1],device.name.UTF8String);
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
      "kernel void copy_word(device uint *out [[buffer(0)]], device const uint *in [[buffer(1)]], device const uint *optional [[buffer(2)]]) { out[0]=in[0]; out[1]=optional ? optional[0] : 0; }\n"
      "vertex float4 triangle(uint i [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
      "fragment uint color(device const uint *in [[buffer(0)]]) { return in[0]; }\n";
    id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
    if (!library) { std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 1; }
    MTL4CompilerDescriptor *cd=[MTL4CompilerDescriptor new];
    id<MTL4Compiler> compiler=[device newCompilerWithDescriptor:cd error:&error]; [cd release]; require(compiler!=nil,"native compiler");
    MTL4LibraryFunctionDescriptor *cf=[MTL4LibraryFunctionDescriptor new]; cf.library=library; cf.name=@"copy_word";
    MTL4ComputePipelineDescriptor *cp=[MTL4ComputePipelineDescriptor new]; cp.computeFunctionDescriptor=cf;
    id<MTLComputePipelineState> computePSO=[compiler newComputePipelineStateWithDescriptor:cp compilerTaskOptions:nil error:&error];
    MTL4LibraryFunctionDescriptor *vf=[MTL4LibraryFunctionDescriptor new]; vf.library=library; vf.name=@"triangle";
    MTL4LibraryFunctionDescriptor *ff=[MTL4LibraryFunctionDescriptor new]; ff.library=library; ff.name=@"color";
    MTL4RenderPipelineDescriptor *rp=[MTL4RenderPipelineDescriptor new]; rp.vertexFunctionDescriptor=vf; rp.fragmentFunctionDescriptor=ff;
    rp.colorAttachments[0].pixelFormat=MTLPixelFormatR32Uint; rp.rasterSampleCount=1;
    id<MTLRenderPipelineState> renderPSO=[compiler newRenderPipelineStateWithDescriptor:rp compilerTaskOptions:nil error:&error];
    if (!computePSO || !renderPSO) { std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String); return 1; }
    id<MTL4CommandQueue> queue=[device newMTL4CommandQueue]; require(queue!=nil,"native queue");
    uint64_t allocator=call(WMTMetal4NewAllocator,{handle(device)}), buffer=call(WMTMetal4NewCommandBuffer,{handle(device)});
    require(allocator && buffer,"native-slot owners");
    uint32_t values[2]={0x11223344,0xa5a55a5a}, optionalValue=0x87654321;
    id<MTLBuffer> input=[device newBufferWithBytes:values length:sizeof(values) options:MTLResourceStorageModeShared];
    id<MTLBuffer> optional=[device newBufferWithBytes:&optionalValue length:4 options:MTLResourceStorageModeShared];
    const uint32_t zeroValue=0;
    id<MTLBuffer> zero=[device newBufferWithBytes:&zeroValue length:4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:32 options:MTLResourceStorageModeShared]; std::memset(output.contents,0xcc,32);
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:1 height:1 mipmapped:NO];
    td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> a=[device newTextureWithDescriptor:td], b=[device newTextureWithDescriptor:td]; require(input && optional && zero && output && a && b,"resources");
    call(WMTMetal4BeginCommandBuffer,{buffer,allocator});
    // Retained-object membership alone must not suppress residency registration.
    [(NSMutableSet *)member(member(reinterpret_cast<id>(buffer),"recording"),"objects") addObject:input];
    uint64_t e=compute(buffer,computePSO);
    computeBuffer(e,output,0,0); computeBuffer(e,input,0,1); computeBuffer(e,optional,0,2);
    wmtcmd_compute_settexture texture{}; texture.type=WMTComputeCommandSetTexture; texture.texture=handle(a); texture.index=7;
    Counters textureBefore=counters(buffer);
    command(e,WMTMetal4ComputeCommands,texture); command(e,WMTMetal4ComputeCommands,texture);
    texture.texture=0; command(e,WMTMetal4ComputeCommands,texture); command(e,WMTMetal4ComputeCommands,texture);
    require(counters(buffer).tableSet==textureBefore.tableSet+2,"texture ID and nil setter equality");
    wmtcmd_compute_setaccelerationstructure resource{}; resource.type=WMTComputeCommandSetAccelerationStructure; resource.index=8;
    Counters resourceBefore=counters(buffer);
    command(e,WMTMetal4ComputeCommands,resource); command(e,WMTMetal4ComputeCommands,resource);
    computeBuffer(e,nil,0,8); computeBuffer(e,nil,0,8);
    require(counters(buffer).tableSet==resourceBefore.tableSet+2,"zero resource/address tags remain distinct");
    Counters before=counters(buffer);
    for (unsigned i=0;i<64;++i) { computeBuffer(e,input,0,1); call(WMTMetal4UseAllocation,{buffer,handle(input)}); }
    Counters after=counters(buffer);
    require(after.tableSet==before.tableSet && after.tableBind==before.tableBind && after.residencyAdd==before.residencyAdd,"identical setters/binds/residency suppressed");
    dispatch(e); // Snapshot A must survive all following table mutations before commit.
    wmtcmd_compute_setbufferoffset offset{}; offset.type=WMTComputeCommandSetBufferOffset; offset.index=1; offset.offset=4;
    command(e,WMTMetal4ComputeCommands,offset); computeBuffer(e,output,8,0); computeBuffer(e,nil,0,2);
    requireUnbound(e,"explicit"); computeBuffer(e,zero,0,2); dispatch(e);
    computeBuffer(e,optional,0,2); // The next encoder must clear an actually nonnil stale slot.
    call(WMTMetal4EndEncoding,{e});
    render(buffer,renderPSO,a,input,0); render(buffer,renderPSO,b,input,4);
    e=compute(buffer,computePSO); computeBuffer(e,output,16,0); computeBuffer(e,input,0,1);
    // The negative arm omits slot 2; the positive arm binds a valid zero resource.
    requireUnbound(e,"encoder"); computeBuffer(e,zero,0,2);
    dispatch(e); computeBuffer(e,optional,0,2); call(WMTMetal4EndEncoding,{e});
    after=counters(buffer); require(after.tableNew==2 && after.tableBind==4 && after.residencyNew==1,"one compute/fragment table and one residency set per native slot");
    require(after.tableSet==22 && after.residencyAdd==8,"exact ordinary-path operation counts");
    require(after.residencyAdd==[(NSSet *)member(member(reinterpret_cast<id>(buffer),"recording"),"allocations") count],"separate allocation membership");
    [input release]; input=nil; [optional release]; optional=nil; // Recording must retain native resources through retirement.
    finish(buffer,queue);
    auto words=static_cast<const uint32_t *>(output.contents);
    word("dispatch_A",words[0],values[0]); word("dispatch_A_optional",words[1],optionalValue);
    word("changed_offset",words[2],values[1]); word("explicit_zero_resource",words[3],0);
    word("compute_render_compute",words[4],values[0]); word("new_encoder_zero_resource",words[5],0);
    word("guard_6",words[6],0xcccccccc); word("guard_7",words[7],0xcccccccc);
    uint32_t pixel=0; [a getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0]; word("draw_A",pixel,values[0]);
    [b getBytes:&pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0]; word("draw_B",pixel,values[1]);
    // Rebegin after retirement: tables/set persist, while resource membership and stale slots reset.
    call(WMTMetal4ResetAllocator,{allocator}); call(WMTMetal4BeginCommandBuffer,{buffer,allocator});
    input=[device newBufferWithBytes:&optionalValue length:4 options:MTLResourceStorageModeShared];
    e=compute(buffer,computePSO); computeBuffer(e,output,0,0); computeBuffer(e,input,0,1);
    requireUnbound(e,"retired"); computeBuffer(e,zero,0,2); dispatch(e); call(WMTMetal4EndEncoding,{e});
    after=counters(buffer); require(after.tableNew==0 && after.residencyNew==0 && after.tableBind==1,"retired native slot reuses table and residency");
    require(after.tableSet==6 && after.residencyAdd==4,"exact retired-reuse operation counts");
    [input release]; finish(buffer,queue); word("retired_reuse",words[0],optionalValue); word("retired_zero_resource",words[1],0);
    [reinterpret_cast<id>(buffer) release]; [reinterpret_cast<id>(allocator) release];
    [output release]; [zero release]; [a release]; [b release]; [queue release]; [computePSO release]; [renderPSO release];
    [cp release]; [rp release]; [cf release]; [vf release]; [ff release]; [compiler release]; [library release]; [device release];
    std::puts("METAL4_BINDING_PASS numeric_cases=12 snapshots=draw,dispatch reuse=compute-render-compute,retired-slot");
  }
  // Keep the plugin loaded until process exit; Objective-C class registrations outlive dlclose.
  return 0;
}
