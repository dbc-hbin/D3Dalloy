// Focused production-backend exercise without Wine: the recording owner below
// retains native Metal objects until GPU completion, as the Unix transport does.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include "../../src/winemetal/unix/metalfx_frame_generation.hpp"
#include "../../src/winemetal/winemetal_metal4_fg.h"
#include "owned-fg-pattern.h"
#include "fsr-kernels.inc"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <objc/runtime.h>
#ifndef EXPECT_GUIDE_BAKING
#define EXPECT_GUIDE_BAKING 1
#endif

struct RecordingOwner { id<MTL4CommandBuffer> command; NSMutableArray* objects; };
extern "C" MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) { return (MTLPixelFormat)ORIGINAL_FORMAT(format); }
extern "C" id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t handle) { return reinterpret_cast<RecordingOwner*>(handle)->command; }
extern "C" void winemetal_metal4_retain_resource(obj_handle_t handle, id object) { [reinterpret_cast<RecordingOwner*>(handle)->objects addObject:object]; }

namespace {
using namespace d3dalloy::framegeneration;
void require(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "OWNED_FG_DEBUG_NATIVE_FAIL reason=%s\n", reason); std::exit(1); }
}
// Observe public native calls; only the explicit retry proof injects failures.
struct Operations {
    unsigned depth=0, motion=0, snapshots=0, fused=0, decode=0;
    unsigned copyPrepasses=0, copyDependencies=0;
    uint64_t realPixels=0;
};
struct CompileCounts { unsigned libraries=0, pipelines=0; } compileCounts;
constexpr unsigned fgPipelineCount = 7;
using NewLibrary = id<MTLLibrary> (*)(id,SEL,NSString*,MTLCompileOptions*,NSError**);
using NewPipeline = id<MTLComputePipelineState> (*)(id,SEL,id<MTLFunction>,NSError**);
using SetPipeline = void (*)(id,SEL,id<MTLComputePipelineState>);
using Dispatch = void (*)(id,SEL,MTLSize,MTLSize);
using TextureCopy = void (*)(id,SEL,id<MTLTexture>,NSUInteger,NSUInteger,MTLOrigin,MTLSize,id<MTLTexture>,NSUInteger,NSUInteger,MTLOrigin);
using StageBarrier = void (*)(id,SEL,MTLStages,MTLStages,MTL4VisibilityOptions);
NewLibrary originalLibrary; NewPipeline originalNew; SetPipeline originalSet; Dispatch originalDispatch; TextureCopy originalCopy;
StageBarrier originalBarrier;
std::mutex observationMutex;
std::unordered_map<id,unsigned> ownedPipelines, encoderPipelines;
Operations operations; id<MTLTexture> snapshotSources[2]{};
id<MTLTexture> observedReal = nil;
id observedCopyEncoder = nil;
NSString* failedPipeline = nil;
bool throwPipeline = false;
CompileCounts compilations() { std::lock_guard lock(observationMutex); return compileCounts; }
id<MTLLibrary> observeLibrary(id device,SEL selector,NSString* source,MTLCompileOptions* options,NSError** error) {
    if ([source isEqualToString:[NSString stringWithUTF8String:kFsrKernelsSource]]) {
        std::lock_guard lock(observationMutex); ++compileCounts.libraries;
    }
    return originalLibrary(device,selector,source,options,error);
}
id<MTLComputePipelineState> observeNew(id device,SEL selector,id<MTLFunction> function,NSError** error) {
    {
        std::lock_guard lock(observationMutex);
        if ([function.name hasPrefix:@"yaagl_fg_"]) ++compileCounts.pipelines;
        if ([function.name isEqualToString:failedPipeline]) {
            failedPipeline = nil;
            if (throwPipeline) {
                throwPipeline = false;
                [NSException raise:NSInternalInconsistencyException format:@"injected pipeline failure"];
            }
            return nil;
        }
    }
    auto pipeline=originalNew(device,selector,function,error);
    unsigned kernel=[function.name isEqualToString:@"yaagl_fg_resample_depth"]?1:[function.name isEqualToString:@"yaagl_fg_normalize_motion"]?2:
        [function.name isEqualToString:@"yaagl_fg_normalize_depth_motion"]?3:
        [function.name isEqualToString:@"yaagl_fg_decode_crop"]?4:0;
    std::lock_guard lock(observationMutex); if(pipeline) ownedPipelines[pipeline]=kernel; return pipeline;
}
void observeSet(id encoder,SEL selector,id<MTLComputePipelineState> pipeline) {
    { std::lock_guard lock(observationMutex); auto found=ownedPipelines.find(pipeline); encoderPipelines[encoder]=found==ownedPipelines.end()?0:found->second; }
    originalSet(encoder,selector,pipeline);
}
void observeDispatch(id encoder,SEL selector,MTLSize threads,MTLSize group) {
    {
        std::lock_guard lock(observationMutex);
        unsigned kernel=encoderPipelines[encoder];
        if(kernel==1)++operations.depth; if(kernel==2)++operations.motion;
        if(kernel==3)++operations.fused; if(kernel==4)++operations.decode;
        if (observedReal && observedCopyEncoder==encoder && kernel==4) {
            ++operations.copyPrepasses;
            // Only count ordering between this caller-output copy and its first
            // owned decode, excluding subsequent native MetalFX encoders.
            observedCopyEncoder=nil;
        }
    }
    originalDispatch(encoder,selector,threads,group);
}
void observeCopy(id encoder,SEL selector,id<MTLTexture> source,NSUInteger slice,NSUInteger level,MTLOrigin origin,MTLSize size,id<MTLTexture> target,NSUInteger targetSlice,NSUInteger targetLevel,MTLOrigin targetOrigin) {
    { std::lock_guard lock(observationMutex); for(auto input:snapshotSources) if(input && input==source) { require(target!=source && target.storageMode==MTLStorageModePrivate,"private-snapshot-copy"); ++operations.snapshots; } }
    if (observedReal && target == observedReal) {
        operations.realPixels += size.width * size.height;
        observedCopyEncoder=encoder;
    }
    originalCopy(encoder,selector,source,slice,level,origin,size,target,targetSlice,targetLevel,targetOrigin);
}
void observeBarrier(id encoder,SEL selector,MTLStages after,MTLStages before,MTL4VisibilityOptions visibility) {
    {
        std::lock_guard lock(observationMutex);
        if (observedReal && observedCopyEncoder==encoder && after==MTLStageBlit && before==MTLStageDispatch)
            ++operations.copyDependencies;
    }
    originalBarrier(encoder,selector,after,before,visibility);
}
struct Observer {
    Method methods[6]; IMP originals[6];
    Observer(id<MTLDevice> device) {
        compileCounts = {};
        ownedPipelines.clear(); encoderPipelines.clear();
        auto allocator=[device newCommandAllocator]; auto command=[device newCommandBuffer];
        [command beginCommandBufferWithAllocator:allocator]; auto encoder=[command computeCommandEncoder];
        methods[0]=class_getInstanceMethod(object_getClass(device),@selector(newComputePipelineStateWithFunction:error:));
        methods[1]=class_getInstanceMethod(object_getClass(encoder),@selector(setComputePipelineState:));
        methods[2]=class_getInstanceMethod(object_getClass(encoder),@selector(dispatchThreads:threadsPerThreadgroup:));
        methods[3]=class_getInstanceMethod(object_getClass(encoder),@selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:));
        methods[4]=class_getInstanceMethod(object_getClass(device),@selector(newLibraryWithSource:options:error:));
        methods[5]=class_getInstanceMethod(object_getClass(encoder),@selector(barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:));
        for(auto method:methods)require(method!=nullptr,"observer-method");
        originals[0]=method_setImplementation(methods[0],reinterpret_cast<IMP>(observeNew)); originalNew=reinterpret_cast<NewPipeline>(originals[0]);
        originals[1]=method_setImplementation(methods[1],reinterpret_cast<IMP>(observeSet)); originalSet=reinterpret_cast<SetPipeline>(originals[1]);
        originals[2]=method_setImplementation(methods[2],reinterpret_cast<IMP>(observeDispatch)); originalDispatch=reinterpret_cast<Dispatch>(originals[2]);
        originals[3]=method_setImplementation(methods[3],reinterpret_cast<IMP>(observeCopy)); originalCopy=reinterpret_cast<TextureCopy>(originals[3]);
        originals[4]=method_setImplementation(methods[4],reinterpret_cast<IMP>(observeLibrary)); originalLibrary=reinterpret_cast<NewLibrary>(originals[4]);
        originals[5]=method_setImplementation(methods[5],reinterpret_cast<IMP>(observeBarrier)); originalBarrier=reinterpret_cast<StageBarrier>(originals[5]);
        [encoder endEncoding]; [command endCommandBuffer]; [command release]; [allocator release];
    }
    ~Observer(){for(unsigned i=0;i<6;++i)method_setImplementation(methods[i],originals[i]);}
};
template<class T> void header(T& p, unsigned operation, uint64_t context = 0) { p.header = {sizeof(p), WMTFG_BRIDGE_VERSION, operation, 0, context}; }
id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, MTLStorageMode storage=MTLStorageModePrivate) {
    auto* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:128 height:128 mipmapped:NO];
    d.storageMode = storage; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    auto t = [device newTextureWithDescriptor:d]; require(t != nil, "texture"); return t;
}
void upload(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> target, const void* data, size_t row) {
    auto source = [device newBufferWithBytes:data length:row * 128 options:MTLResourceStorageModeShared];
    auto command = [queue commandBuffer]; auto blit = [command blitCommandEncoder];
    [blit copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * 128 sourceSize:MTLSizeMake(128,128,1) toTexture:target destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted]; require(command.status == MTLCommandBufferStatusCompleted,"upload"); [source release];
}
std::vector<uint16_t> read(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> source) {
    auto buffer = [device newBufferWithLength:128 * 128 * 8 + 256 options:MTLResourceStorageModeShared];
    std::memset(buffer.contents,0xa5,buffer.length);
    auto command = [queue commandBuffer]; auto blit = [command blitCommandEncoder];
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(128,128,1) toBuffer:buffer destinationOffset:128 destinationBytesPerRow:128 * 8 destinationBytesPerImage:128 * 128 * 8];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted]; require(command.status == MTLCommandBufferStatusCompleted,"read");
    auto bytes=static_cast<const unsigned char*>(buffer.contents);
    for(unsigned i=0;i<128;++i)require(bytes[i]==0xa5 && bytes[128+128*128*8+i]==0xa5,"readback-guards");
    std::vector<uint16_t> pixels(128 * 128 * 4); std::memcpy(pixels.data(),bytes+128,pixels.size()*2); [buffer release]; return pixels;
}
struct Harness {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTL4CommandQueue> queue4 = [device newMTL4CommandQueue];
    id<MTL4Compiler> compiler = nil;
    id<MTLSharedEvent> completed = [device newSharedEvent];
    uint64_t sequence = 0;
    WMTFGNativeCommand native{};
    Harness() {
        require(device && [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:device],"supported");
        auto* d = [MTL4CompilerDescriptor new]; NSError* error = nil; compiler = [device newCompilerWithDescriptor:d error:&error]; [d release];
        require(compiler && queue4,"compiler"); native = {(uint64_t)device,(uint64_t)compiler};
    }
    ~Harness() { [completed release]; [compiler release]; [queue4 release]; [queue release]; [device release]; }
    uint64_t create(uint32_t flags = WMTFG_DEPTH_INVERTED) {
        WMTFGcreate_packet p{}; header(p,WMTFG_CREATE); p.device=(uint64_t)device;
        p.display_width=p.display_height=p.max_render_width=p.max_render_height=128;
        p.backbuffer_format=WMTPixelFormatRGBA16Float; p.flags=flags;
        require(api(WMTFG_CREATE,&p,nullptr)==0,"create"); return p.header.context;
    }
    void destroy(uint64_t context) { WMTFGdestroy_packet p{}; header(p,WMTFG_DESTROY,context); require(api(WMTFG_DESTROY,&p,nullptr)==0,"destroy"); }
    void execute(const std::vector<std::shared_ptr<const PreparedFrame>>& commands,
                 std::initializer_list<id<MTLResource>> inputs = {}) {
        auto allocator = [device newCommandAllocator]; auto command = [device newCommandBuffer];
        RecordingOwner owner{command,[NSMutableArray new]}; auto fence = [device newFence];
        [command beginCommandBufferWithAllocator:allocator];
        auto descriptor = [MTLResidencySetDescriptor new];
        auto residency = [device newResidencySetWithDescriptor:descriptor error:nil]; [descriptor release];
        require(residency != nil,"caller-residency");
        for (auto input : inputs) if (input) [residency addAllocation:input];
        [residency commit]; [residency requestResidency]; [command useResidencySet:residency];
        std::vector<std::shared_ptr<const ExecutionLease>> leases;
        for (const auto& recorded : commands) { std::shared_ptr<const ExecutionLease> lease; require(recorded->encode(command,fence,lease,(uint64_t)&owner),"encode"); leases.push_back(std::move(lease)); }
        [command endCommandBuffer]; [queue4 commit:&command count:1]; [queue4 signalEvent:completed value:++sequence];
        require([completed waitUntilSignaledValue:sequence timeoutMS:15000],"completion");
        [residency endResidency]; [residency release];
        [owner.objects release]; [fence release]; [command release]; [allocator release];
    }
    std::vector<std::shared_ptr<const PreparedFrame>> record(uint64_t context, unsigned frame, id<MTLTexture> color,id<MTLTexture> depth,id<MTLTexture> motion,id<MTLTexture> output,unsigned flags,unsigned render=128,float scale=1,bool mutate=false, const WMTFGdispatch_packet* custom=nullptr) {
        WMTFGprepare_packet p{}; header(p,WMTFG_PREPARE,context); p.command_list=(uint64_t)&native; p.frame_id=frame;
        p.depth=(uint64_t)depth; p.motion_vectors=(uint64_t)motion; p.render_width=p.render_height=render;
        p.motion_width=p.motion_height=render;
        p.motion_scale_x=p.motion_scale_y=scale; p.frame_time_delta_ms=33.3333f; p.camera_near=.1f; p.camera_far=100;
        p.camera_fov_vertical_radians=1; p.view_space_to_meters=1;
        std::shared_ptr<const PreparedFrame> prepare; require(api(WMTFG_PREPARE,&p,&prepare)==0,"prepare");
        if(mutate) {
            execute({prepare},{depth,motion});
            std::vector<float> poisonDepth(128*128,.125f); std::vector<uint16_t> poisonMotion(128*128*2,0x4900);
            upload(device,queue,depth,poisonDepth.data(),128*4); upload(device,queue,motion,poisonMotion.data(),128*4);
        }
        WMTFGdispatch_packet g = custom ? *custom : WMTFGdispatch_packet{};
        header(g,WMTFG_DISPATCH,context); g.command_list=(uint64_t)&native; g.frame_id=frame;
        g.present_color=(uint64_t)color; g.output=(uint64_t)output; g.num_generated_frames=1; g.max_luminance=1; g.flags=flags;
        std::shared_ptr<const PreparedFrame> generate; require(api(WMTFG_DISPATCH,&g,&generate)==0,"generate"); if(mutate)return {generate}; return {prepare,generate};
    }
};
void pipelineBatchProof(Harness& h) {
    Observer observer(h.device);
    auto context = h.create();
    auto depth = texture(h.device, MTLPixelFormatR32Float);
    auto motion = texture(h.device, MTLPixelFormatRG16Float);
    WMTFGprepare_packet p{}; header(p, WMTFG_PREPARE, context);
    p.command_list=(uint64_t)&h.native; p.frame_id=1;
    p.depth=(uint64_t)depth; p.motion_vectors=(uint64_t)motion;
    p.render_width=p.render_height=p.motion_width=p.motion_height=128;
    p.motion_scale_x=p.motion_scale_y=1; p.frame_time_delta_ms=33.3333f;
    p.camera_near=.1f; p.camera_far=100; p.camera_fov_vertical_radians=1; p.view_space_to_meters=1;
    std::shared_ptr<const PreparedFrame> prepared;
    failedPipeline = @"yaagl_fg_resample_depth";
    require(api(WMTFG_PREPARE, &p, &prepared)==4 && !prepared, "partial-configuration-failure-reported");
    auto counts = compilations();
    require(counts.libraries==1 && counts.pipelines==fgPipelineCount, "one-library-for-pipeline-batch");
    failedPipeline = @"yaagl_fg_decode_crop"; throwPipeline = true;
    require(api(WMTFG_PREPARE, &p, &prepared)==3 && !prepared, "configuration-objc-exception-contained");
    counts = compilations();
    require(counts.libraries==2 && counts.pipelines==fgPipelineCount+1, "failed-configuration-not-cached");
    require(api(WMTFG_PREPARE, &p, &prepared)==0 && bool(prepared), "configuration-retry-succeeds");
    counts = compilations();
    require(counts.libraries==3 && counts.pipelines==2*fgPipelineCount+1, "retry-creates-one-complete-library-batch");
    p.frame_id=2;
    require(api(WMTFG_PREPARE, &p, &prepared)==0 && bool(prepared), "cached-configuration-prepare");
    counts = compilations();
    require(counts.libraries==3 && counts.pipelines==2*fgPipelineCount+1, "cached-configuration-does-not-recompile");
    h.destroy(context); [depth release]; [motion release];
    std::printf("OWNED_FG_PIPELINES_PASS complete_batch_libraries=1 pipelines=%u partial_retry=complete objc_exception=contained cache_hit_libraries=0\n",fgPipelineCount);
}
void equivalentPassProof(Harness& h) {
    Observer observer(h.device);
    auto depth=texture(h.device,MTLPixelFormatR32Float), motion=texture(h.device,MTLPixelFormatRG16Float);
    std::vector<float> depths(128*128,.75f); std::vector<uint16_t> vectors(128*128*2,0);
    upload(h.device,h.queue,depth,depths.data(),128*4); upload(h.device,h.queue,motion,vectors.data(),128*4);
    const uint64_t contexts[] = {h.create(),h.create()};
    for (unsigned frame=1;frame<=4;++frame) {
        const auto scene=fgScene(frame);
        auto color=texture(h.device,MTLPixelFormatRGBA16Float), duplicate=texture(h.device,MTLPixelFormatRGBA16Float);
        upload(h.device,h.queue,color,scene.data(),128*8); upload(h.device,h.queue,duplicate,scene.data(),128*8);
        std::vector<uint16_t> baseline;
        for (unsigned alias=0;alias<2;++alias) {
            auto output=texture(h.device,MTLPixelFormatRGBA16Float);
            WMTFGdispatch_packet custom{}; custom.hudless_color=(uint64_t)(alias?color:duplicate);
            operations={};
            h.execute(h.record(contexts[alias],frame,color,depth,motion,output,0,128,1,false,&custom),
                {color,duplicate,depth,motion,output});
            require(operations.decode == (alias?1u:2u),"identical-ui-scene-one-decode");
            auto pixels=read(h.device,h.queue,output);
            if (!alias) baseline=pixels; else require(pixels==baseline,"shared-decode-complete-output-bit-exact");
            [output release];
        }
        [color release]; [duplicate release];
    }
    for (auto context:contexts) h.destroy(context);
    unsigned cases=0; uint64_t copied=0, oldCopies=0;
    for (unsigned mode=0;mode<5;++mode) {
        auto context=h.create(); auto scene=fgScene(5+mode);
        auto color=texture(h.device,MTLPixelFormatRGBA16Float), output=texture(h.device,MTLPixelFormatRGBA16Float),
            real=texture(h.device,MTLPixelFormatRGBA16Float);
        std::vector<uint16_t> sentinel(128*128*4,0x3e00);
        upload(h.device,h.queue,color,scene.data(),128*8); upload(h.device,h.queue,real,sentinel.data(),128*8);
        upload(h.device,h.queue,output,sentinel.data(),128*8);
        WMTFGdispatch_packet custom{};
        custom.hudless_color=(uint64_t)color; custom.real_output=(uint64_t)real;
        custom.independent_subrects=1; custom.generation_rect_width=96; custom.generation_rect_height=80;
        custom.present_x=custom.hudless_x=3; custom.present_y=custom.hudless_y=5;
        custom.output_x=2; custom.output_y=4;
        custom.real_x=mode==0?3:mode==2?0:mode==3?32:17;
        custom.real_y=mode==0?5:mode==2?0:mode==3?48:29;
        custom.output_subrect_only=mode==4;
        observedReal=real; operations={};
        h.execute(h.record(context,1,color,depth,motion,output,0,128,1,false,&custom),{color,depth,motion,output,real});
        observedReal=nil;
        auto expected=custom.output_subrect_only?sentinel:scene;
        for (unsigned y=0;y<80;++y)
            std::memcpy(expected.data()+((custom.real_y+y)*128+custom.real_x)*4,
                scene.data()+((custom.present_y+y)*128+custom.present_x)*4,96*8);
        require(read(h.device,h.queue,real)==expected,"real-output-whole-baseline-and-roi-bit-exact");
        const uint64_t expectedPixels=custom.output_subrect_only?96*80:128*128;
        require(operations.realPixels==expectedPixels,"real-output-no-overwritten-baseline-copy");
        copied+=operations.realPixels; oldCopies+=custom.output_subrect_only?96*80:128*128+(mode?96*80:0);
        h.destroy(context); [color release]; [output release]; [real release]; ++cases;
    }
    [depth release]; [motion release];
    std::printf("OWNED_FG_EQUIVALENT_PASSES_PASS history_frames=4 identical_source_decodes=1 separate_source_decodes=2 whole_output=bit_exact real_roi_cases=%u copied_pixels=%llu former_pixels=%llu outside_sentinels=1\n",
        cases,(unsigned long long)copied,(unsigned long long)oldCopies);
}
void prepassDependencyProof(Harness& h) {
    Observer observer(h.device);
    const uint64_t contexts[]={h.create(),h.create()};
    auto depth=texture(h.device,MTLPixelFormatR32Float), motion=texture(h.device,MTLPixelFormatRG16Float);
    std::vector<float> depths(128*128,.75f);
    std::vector<uint16_t> vectors(128*128*2,0);
    for (size_t i=0;i<vectors.size();i+=2) vectors[i]=0x3c00;
    unsigned barriers[2]{}, comparisons=0;
    for (unsigned frame=1;frame<=4;++frame) {
        auto scene=fgScene(frame); auto color=texture(h.device,MTLPixelFormatRGBA16Float);
        upload(h.device,h.queue,color,scene.data(),128*8);
        std::vector<uint16_t> reference;
        for (unsigned shared=0;shared<2;++shared) {
            auto output=texture(h.device,MTLPixelFormatRGBA16Float);
            auto real=texture(h.device,MTLPixelFormatRGBA16Float,
                shared?MTLStorageModeShared:MTLStorageModePrivate);
            upload(h.device,h.queue,depth,depths.data(),128*4);
            upload(h.device,h.queue,motion,vectors.data(),128*4);
            WMTFGdispatch_packet custom{}; custom.real_output=(uint64_t)real;
            operations={}; observedReal=real; observedCopyEncoder=nil;
            snapshotSources[0]=depth; snapshotSources[1]=motion;
            // Execute Prepare before Generate admission so the generate prepass
            // still consumes both immutable raw snapshots for motion normalization.
            h.execute(h.record(contexts[shared],frame,color,depth,motion,output,0,128,2,true,&custom),
                {color,depth,motion,output,real});
            observedReal=nil; observedCopyEncoder=nil;
            snapshotSources[0]=snapshotSources[1]=nil;
            require(operations.copyPrepasses==1 && operations.copyDependencies==shared,
                "prepass-copy-dependency-barrier-count");
            require(operations.snapshots==2 && operations.motion==1 && operations.decode==1 &&
                    !operations.depth && !operations.fused && operations.realPixels==128*128,
                "prepass-copy-and-compute-work-preserved");
            require(read(h.device,h.queue,real)==scene,"prepass-real-copy-bit-exact");
            auto pixels=read(h.device,h.queue,output);
            if (!shared) reference=pixels;
            else { require(pixels==reference,"prepass-independent-vs-conservative-output-bit-exact"); ++comparisons; }
            barriers[shared]+=operations.copyDependencies;
            [real release]; [output release];
        }
        [color release];
    }
    for (auto context:contexts) h.destroy(context);
    [motion release]; [depth release];
    std::printf("OWNED_FG_PREPASS_DEPENDENCY_PASS image_pairs=%u frames=4 private_barriers=%u shared_barriers=%u raw_snapshots=2 motion_dispatches=1 decode_dispatches=1 outputs=bit_exact\n",
        comparisons,barriers[0],barriers[1]);
}
void finalizedPrepareProof(Harness& h) {
    Observer observer(h.device);
    unsigned compared=0,bakedCopies=0,rawCopies=0;
    for (unsigned scenario=0;scenario<6;++scenario) {
        const bool jittered=scenario==3;
        const uint32_t createFlags=WMTFG_DEPTH_INVERTED | (jittered?WMTFG_MOTION_VECTORS_JITTERED:0);
        uint64_t contexts[]={h.create(createFlags),h.create(createFlags)};
        for (unsigned frame=1;frame<=4;++frame) {
            std::vector<uint16_t> reference;
            for (unsigned delayed=0;delayed<2;++delayed) {
                auto color=texture(h.device,MTLPixelFormatRGBA16Float),output=texture(h.device,MTLPixelFormatRGBA16Float);
                auto depth=texture(h.device,MTLPixelFormatR32Float);
                const bool wideMotion=scenario==2;
                auto motion=texture(h.device,wideMotion?MTLPixelFormatRG32Float:MTLPixelFormatRG16Float);
                auto scene=fgScene(frame);upload(h.device,h.queue,color,scene.data(),128*8);
                std::vector<float> z(128*128),v(128*128*2);
                for (unsigned y=0;y<128;++y) for(unsigned x=0;x<128;++x) {
                    z[y*128+x]=((x/7+y/9+frame)%3)?0.75f:0.25f;
                    v[(y*128+x)*2]=float(int(x%13)-6)*0.125f;
                    v[(y*128+x)*2+1]=float(int(y%11)-5)*0.0625f;
                }
                if(wideMotion)v[(17*128+13)*2]=-999;
                upload(h.device,h.queue,depth,z.data(),128*4);
                if(wideMotion)upload(h.device,h.queue,motion,v.data(),128*8);
                else {std::vector<_Float16> half(v.begin(),v.end());upload(h.device,h.queue,motion,half.data(),128*4);}
                WMTFGprepare_packet p{};header(p,WMTFG_PREPARE,contexts[delayed]);
                p.command_list=(uint64_t)&h.native;p.frame_id=frame;p.depth=(uint64_t)depth;p.motion_vectors=(uint64_t)motion;
                p.render_width=p.render_height=p.motion_width=p.motion_height=scenario==0 || jittered?128:64;
                p.depth_subrect_x=scenario==0 || jittered?0:3;p.depth_subrect_y=scenario==0 || jittered?0:5;
                p.motion_subrect_x=scenario==0 || jittered?0:7;p.motion_subrect_y=scenario==0 || jittered?0:9;
                p.motion_scale_x=2;p.motion_scale_y=-0.75f;p.frame_time_delta_ms=33.333f;
                p.camera_near=.1f;p.camera_far=100;p.camera_fov_vertical_radians=1;p.view_space_to_meters=1;
                p.reset=frame==4;p.jitter_x=frame%2?.25f:-.25f;p.jitter_y=.125f;
                p.motion_vectors_invalid_value_present=wideMotion;p.motion_vectors_invalid_value=-999;
                p.reconstruct_camera_motion=wideMotion;
                for(unsigned i=0;i<4;++i)p.clip_to_previous_clip[i*5]=1;
                p.clip_to_previous_clip[12]=.003f;p.clip_to_previous_clip[13]=-.002f;
                std::shared_ptr<const PreparedFrame> prepare,generate;
                require(api(WMTFG_PREPARE,&p,&prepare)==0 && prepare,"finalized-prepare-record");
                WMTFGdispatch_packet d{};header(d,WMTFG_DISPATCH,contexts[delayed]);d.command_list=(uint64_t)&h.native;
                d.frame_id=frame;d.present_color=(uint64_t)color;d.output=(uint64_t)output;d.num_generated_frames=1;d.max_luminance=1;
                if(scenario!=0 && !jittered) {
                    d.independent_subrects=1;d.generation_rect_width=96;d.generation_rect_height=80;
                    d.present_x=3;d.present_y=5;d.output_x=2;d.output_y=4;
                }
                const bool rawFirst=!delayed || scenario==4 || scenario==5;
                if(scenario==4) {
                    auto rejected=d;rejected.generation_rect_width=80;rejected.generation_rect_height=64;
                    require(api(WMTFG_DISPATCH,&rejected,nullptr)!=0,"rejected-generate-no-plan-publication");
                }
                operations={};snapshotSources[0]=depth;snapshotSources[1]=motion;
                if(!rawFirst) require(api(WMTFG_DISPATCH,&d,&generate)==0 && generate,"finalized-generate-record");
                h.execute({prepare},{depth,motion});
                const auto prepareCounts=operations;
                if(rawFirst)require(api(WMTFG_DISPATCH,&d,&generate)==0 && generate,"raw-first-generate-record");
                const bool baked=EXPECT_GUIDE_BAKING && !rawFirst && !jittered;
                require(prepareCounts.snapshots==(baked?(scenario==0?1u:0u):2u),"finalized-guide-copy-count");
                require(baked?(prepareCounts.motion+prepareCounts.fused)==1:(!prepareCounts.motion && !prepareCounts.fused),
                    "normalization-moves-only-after-successful-generate-admission");
                if(delayed)bakedCopies+=prepareCounts.snapshots;else rawCopies+=prepareCounts.snapshots;
                std::fill(z.begin(),z.end(),.125f);upload(h.device,h.queue,depth,z.data(),128*4);
                if(wideMotion) {std::fill(v.begin(),v.end(),99);upload(h.device,h.queue,motion,v.data(),128*8);}
                else {std::vector<uint16_t> poison(128*128*2,0x4900);upload(h.device,h.queue,motion,poison.data(),128*4);}
                operations={};
                if(frame==4)h.destroy(contexts[delayed]);
                h.execute({generate},{color,output});
                if(baked)require(!operations.motion && !operations.depth && !operations.fused,"generate-direct-normalized-snapshots");
                snapshotSources[0]=snapshotSources[1]=nil;
                const auto pixels=read(h.device,h.queue,output);
                if(!delayed)reference=pixels;else {require(reference==pixels,"raw-first-vs-finalized-prepare-bit-exact");++compared;}
                [color release];[output release];[depth release];[motion release];
            }
        }
    }
    std::printf("OWNED_FG_FINALIZED_PREPARE_PASS comparisons=%u frames_per_contract=4 source_offsets=1 motion_fp32=1 camera=1 invalid_vectors=1 reset=1 jitter_fallback=1 rejected_generate=1 already_executed_fallback=1 poison_after_prepare=1 destroy_before_generate=1 outputs=bit_exact optimized_snapshot_copies=%u raw_snapshot_copies=%u\n",
        compared,bakedCopies,rawCopies);
}
void normalizationProof(Harness& h, unsigned render) {
    Observer observer(h.device);
    uint64_t contexts[]={h.create(),h.create(),h.create()};
    Operations totals[3]{}; unsigned interpolated=0;
    for(unsigned frame=1;frame<=6;++frame) {
        auto scene=fgScene(frame); auto color=texture(h.device,MTLPixelFormatRGBA16Float); upload(h.device,h.queue,color,scene.data(),128*8);
        std::vector<uint16_t> baseline;
        for(unsigned mode=0;mode<3;++mode) {
            auto depth=texture(h.device,MTLPixelFormatR32Float),motion=texture(h.device,MTLPixelFormatRG16Float),output=texture(h.device,MTLPixelFormatRGBA16Float);
            std::vector<float> z(128*128); std::vector<uint16_t> mv(128*128*2);
            for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x) {
                // All three contexts must use the same native content grid.
                // Different grid sizes select different MetalFX history, not
                // numerically equivalent denoisers. Test 64 and 128 separately.
                unsigned sx=render==64?x:x/2,sy=render==64?y:y/2;
                z[y*128+x]=((sx/8+sy/8+frame)%2)? .75f:.25f;
                // Scale2 converts to identical motion on the selected input grid.
                mv[(y*128+x)*2]=render==64?(mode==1?0x3800:0x3c00):(mode==1?0x3c00:0x4000);
                mv[(y*128+x)*2+1]=render==64?(mode==1?0xb400:0xb800):(mode==1?0xb800:0xbc00);
            }
            upload(h.device,h.queue,depth,z.data(),128*4); upload(h.device,h.queue,motion,mv.data(),128*4);
            operations={}; snapshotSources[0]=depth; snapshotSources[1]=motion;
            const auto beforeCompile = compilations();
            h.execute(h.record(contexts[mode],frame,color,depth,motion,output,frame==6?4:0,render,mode==1?2:1,mode==2),{color,depth,motion,output});
            const auto afterCompile = compilations();
            const unsigned batches = frame==1 && mode==0 ? 1 : 0;
            require(afterCompile.libraries-beforeCompile.libraries==batches &&
                    afterCompile.pipelines-beforeCompile.pipelines==fgPipelineCount*batches, "device-kernel-library-count-and-reuse");
            snapshotSources[0]=snapshotSources[1]=nil; auto counts=operations;
            require(counts.snapshots==(EXPECT_GUIDE_BAKING && mode==1 ? 1u : 2u),
                "prepare-converts-only-the-finalized-guide-contract");
            // Depth already matches the native input grid. Only scaled motion
            // needs conversion; depth/motion resampling is checked separately.
            const bool expectedDispatches = mode==1 ? counts.motion==1 && !counts.depth && !counts.fused :
                !counts.depth && !counts.motion && !counts.fused;
            if (!expectedDispatches) std::fprintf(stderr,"OWNED_FG_NORMALIZATION_COUNTS_DIAGNOSTIC frame=%u mode=%u depth=%u motion=%u fused=%u decode=%u\n",
                frame,mode,counts.depth,counts.motion,counts.fused,counts.decode);
            require(expectedDispatches,"actual-normalization-dispatch-count");
            totals[mode].depth+=counts.depth; totals[mode].motion+=counts.motion; totals[mode].snapshots+=counts.snapshots; totals[mode].fused+=counts.fused;
            auto pixels=read(h.device,h.queue,output);
            if(const char* directory=std::getenv("OWNED_FG_PROOF_OUTPUT")) {
                char path[1024]; require(std::snprintf(path,sizeof(path),"%s/grid-%u-frame-%u-mode-%u.rgba16f",directory,render,frame,mode)<int(sizeof(path)),"artifact-path");
                auto file=std::fopen(path,"wb"); require(file!=nullptr,"artifact-open");
                require(std::fwrite(pixels.data(),sizeof(uint16_t),pixels.size(),file)==pixels.size(),"artifact-write");require(std::fclose(file)==0,"artifact-close");
            }
            if(!mode) {
                baseline=pixels; unsigned changed=0;
                if(frame>=3 && frame<=5) {
                    for(size_t i=0;i<pixels.size();i+=4) {
                        float maximum=0;
                        for(unsigned c=0;c<4;++c) { _Float16 a,b;std::memcpy(&a,pixels.data()+i+c,2);std::memcpy(&b,scene.data()+i+c,2);require(std::isfinite(float(a)) && std::isfinite(float(b)),"interpolation-finite");maximum=std::max(maximum,std::fabs(float(a)-float(b))); }
                        if(maximum>2.0f/255.0f)++changed;
                    }
                    require(changed>64,"genuine-interpolation-not-source"); ++interpolated;
                }
                std::printf("OWNED_FG_NORMALIZATION_FRAME frame=%u changed_pixels=%u\n",frame,changed);
            } else {
                float maximum=0;
                for(size_t i=0;i<pixels.size();++i) { _Float16 a,b; std::memcpy(&a,baseline.data()+i,2);std::memcpy(&b,pixels.data()+i,2);require(std::isfinite(float(a)) && std::isfinite(float(b)),"normalization-finite"); maximum=std::max(maximum,std::fabs(float(a)-float(b))); }
                std::printf("OWNED_FG_NORMALIZATION_COMPARE frame=%u mode=%u components=%zu max_abs=%.9g depth_dispatches=%u motion_dispatches=%u fused_dispatches=%u private_snapshot_copies=%u\n",frame,mode,pixels.size(),maximum,counts.depth,counts.motion,counts.fused,counts.snapshots);
                require(maximum<=2.0f/255.0f,"normalization-output-history-equivalent");
                if(mode==2)require(pixels==baseline,"prepare-retained-snapshot-exact");
            }
            [output release];[depth release];[motion release];
        }
        [color release];
    }
    require(interpolated>=3,"three-interpolated-frames");
    const auto counts = compilations();
    require(counts.libraries==1 && counts.pipelines==fgPipelineCount, "independent-configurations-share-device-kernel-library");
    for(unsigned mode=0;mode<3;++mode) {h.destroy(contexts[mode]);std::printf("OWNED_FG_NORMALIZATION_COUNTS mode=%u depth_dispatches=%u motion_dispatches=%u fused_dispatches=%u private_snapshot_copies=%u\n",mode,totals[mode].depth,totals[mode].motion,totals[mode].fused,totals[mode].snapshots);}
    std::printf("OWNED_FG_NORMALIZATION_PASS render_grid=%u interpolated_frames=%u all_pixels=1 guards=1 snapshots_mutated_after_completion=1\n",render,interpolated);
}
}
int main(int argc, char** argv) { @autoreleasepool {
    Harness h;
    if (argc==2 && !std::strcmp(argv[1],"--normalization-only")) {
        normalizationProof(h,128); normalizationProof(h,64); return 0;
    }
    if (argc==2 && !std::strcmp(argv[1],"--barriers-only")) {
        prepassDependencyProof(h); return 0;
    }
    require(argc==1,"arguments");
    auto depth=texture(h.device,MTLPixelFormatR32Float), motion=texture(h.device,MTLPixelFormatRG16Float);
    std::vector<float> z(128*128,.75f); std::vector<uint16_t> mv(128*128*2,0);
    upload(h.device,h.queue,depth,z.data(),128*4); upload(h.device,h.queue,motion,mv.data(),128*4);
    uint64_t contexts[] = {h.create(),h.create(),h.create(),h.create()};
    for (unsigned frame=1;frame<=2;++frame) {
        auto scene=fgScene(frame); auto color=texture(h.device,MTLPixelFormatRGBA16Float); upload(h.device,h.queue,color,scene.data(),128*8);
        std::vector<uint16_t> baseline;
        for (unsigned index=0;index<4;++index) {
            auto output=texture(h.device,MTLPixelFormatRGBA16Float); h.execute(h.record(contexts[index],frame,color,depth,motion,output,index?1u<<(index-1):0u),{color,depth,motion,output}); auto pixels=read(h.device,h.queue,output);
            if (!index) baseline=pixels;
            else for (unsigned y=0;y<128;++y) for (unsigned x=0;x<128;++x) {
                uint16_t expected[4]; std::memcpy(expected,baseline.data()+(y*128+x)*4,8);
                if (index==1 && (x<2 || x+2>=128)) { const uint16_t green[]={0,0x3c00,0,0x3c00}; std::memcpy(expected,green,8); }
                if (index==2 && x<16 && y<16) { const uint16_t reset[]={uint16_t(frame==1?0x3c00:0),uint16_t(frame==1?0:0x3c00),0,0x3c00}; std::memcpy(expected,reset,8); }
                if (index==3) { const unsigned panel=x*3/128; if (!panel) std::memcpy(expected,baseline.data()+(y*128+(x*3)%128)*4,8); else { const uint16_t v=panel==1?0x3800:0x3a00; for(unsigned i=0;i<3;++i)expected[i]=v; expected[3]=0x3c00; } }
                require(!std::memcmp(expected,pixels.data()+(y*128+x)*4,8),"diagnostic-exact");
            }
            [output release];
        }
        [color release];
    }
    auto scene3=fgScene(3); auto color3=texture(h.device,MTLPixelFormatRGBA16Float); upload(h.device,h.queue,color3,scene3.data(),128*8);
    std::vector<uint16_t> cleanHistory;
    for (unsigned index : {0u,3u}) { auto target=texture(h.device,MTLPixelFormatRGBA16Float); h.execute(h.record(contexts[index],3,color3,depth,motion,target,0),{color3,depth,motion,target}); auto pixels=read(h.device,h.queue,target); if (!index) cleanHistory=pixels; else require(pixels==cleanHistory,"debug-history-unmodified"); [target release]; }
    std::fill(z.begin(),z.end(),.25f); for (unsigned i=0;i<mv.size();i+=2)mv[i]=0x4100; // 2.5 display pixels.
    upload(h.device,h.queue,depth,z.data(),128*4); upload(h.device,h.queue,motion,mv.data(),128*4);
    auto changedOutput=texture(h.device,MTLPixelFormatRGBA16Float); h.execute(h.record(contexts[3],4,color3,depth,motion,changedOutput,4),{color3,depth,motion,changedOutput}); auto changed=read(h.device,h.queue,changedOutput);
    const uint16_t changedMv[]={0x3a00,0x3800,0x3800,0x3c00},changedZ[]={0x3400,0x3400,0x3400,0x3c00};
    require(!std::memcmp(changed.data()+(64*128+64)*4,changedMv,8),"motion-input-sensitivity"); require(!std::memcmp(changed.data()+(64*128+110)*4,changedZ,8),"depth-input-sensitivity"); [changedOutput release]; [color3 release];
    auto replayContext=h.create(); auto color=texture(h.device,MTLPixelFormatRGBA16Float),output=texture(h.device,MTLPixelFormatRGBA16Float);
    auto scene=fgScene(10); upload(h.device,h.queue,color,scene.data(),128*8); auto recorded=h.record(replayContext,10,color,depth,motion,output,7); h.destroy(replayContext); [color release];
    h.execute(recorded,{color,depth,motion,output}); auto first=read(h.device,h.queue,output); h.execute(recorded,{color,depth,motion,output}); require(first==read(h.device,h.queue,output),"destroyed-context-replay-exact");
    for(auto context:contexts)h.destroy(context); [output release]; [depth release]; [motion release];
    recorded.clear(); // End the explicit replay lifetime before the cold-cache failure proof.
    std::puts("OWNED_FG_DEBUG_NATIVE_PASS tear=exact reset=first_and_history view=input_sensitive history=unmodified replay=exact destroy_before_execute=1");
    pipelineBatchProof(h);
    equivalentPassProof(h);
    finalizedPrepareProof(h);
    normalizationProof(h,128); normalizationProof(h,64);
    prepassDependencyProof(h);
} }
