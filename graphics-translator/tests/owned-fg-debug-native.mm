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

struct RecordingOwner { id<MTL4CommandBuffer> command; NSMutableArray* objects; };
extern "C" MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) { return (MTLPixelFormat)ORIGINAL_FORMAT(format); }
extern "C" id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t handle) { return reinterpret_cast<RecordingOwner*>(handle)->command; }
extern "C" void winemetal_metal4_retain_resource(obj_handle_t handle, id object) { [reinterpret_cast<RecordingOwner*>(handle)->objects addObject:object]; }

namespace {
using namespace dxmt::framegeneration;
void require(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "OWNED_FG_DEBUG_NATIVE_FAIL reason=%s\n", reason); std::exit(1); }
}
// Observe public native calls; only the explicit retry proof injects failures.
struct Operations { unsigned depth=0, motion=0, snapshots=0, fused=0; };
struct CompileCounts { unsigned libraries=0, pipelines=0; } compileCounts;
using NewLibrary = id<MTLLibrary> (*)(id,SEL,NSString*,MTLCompileOptions*,NSError**);
using NewPipeline = id<MTLComputePipelineState> (*)(id,SEL,id<MTLFunction>,NSError**);
using SetPipeline = void (*)(id,SEL,id<MTLComputePipelineState>);
using Dispatch = void (*)(id,SEL,MTLSize,MTLSize);
using TextureCopy = void (*)(id,SEL,id<MTLTexture>,NSUInteger,NSUInteger,MTLOrigin,MTLSize,id<MTLTexture>,NSUInteger,NSUInteger,MTLOrigin);
NewLibrary originalLibrary; NewPipeline originalNew; SetPipeline originalSet; Dispatch originalDispatch; TextureCopy originalCopy;
std::mutex observationMutex;
std::unordered_map<id,unsigned> ownedPipelines, encoderPipelines;
Operations operations; id<MTLTexture> snapshotSources[2]{};
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
        [function.name isEqualToString:@"yaagl_fg_normalize_depth_motion"]?3:0;
    std::lock_guard lock(observationMutex); if(pipeline) ownedPipelines[pipeline]=kernel; return pipeline;
}
void observeSet(id encoder,SEL selector,id<MTLComputePipelineState> pipeline) {
    { std::lock_guard lock(observationMutex); auto found=ownedPipelines.find(pipeline); encoderPipelines[encoder]=found==ownedPipelines.end()?0:found->second; }
    originalSet(encoder,selector,pipeline);
}
void observeDispatch(id encoder,SEL selector,MTLSize threads,MTLSize group) {
    { std::lock_guard lock(observationMutex); unsigned kernel=encoderPipelines[encoder]; if(kernel==1)++operations.depth; if(kernel==2)++operations.motion; if(kernel==3)++operations.fused; }
    originalDispatch(encoder,selector,threads,group);
}
void observeCopy(id encoder,SEL selector,id<MTLTexture> source,NSUInteger slice,NSUInteger level,MTLOrigin origin,MTLSize size,id<MTLTexture> target,NSUInteger targetSlice,NSUInteger targetLevel,MTLOrigin targetOrigin) {
    { std::lock_guard lock(observationMutex); for(auto input:snapshotSources) if(input && input==source) { require(target!=source && target.storageMode==MTLStorageModePrivate,"private-snapshot-copy"); ++operations.snapshots; } }
    originalCopy(encoder,selector,source,slice,level,origin,size,target,targetSlice,targetLevel,targetOrigin);
}
struct Observer {
    Method methods[5]; IMP originals[5];
    Observer(id<MTLDevice> device) {
        compileCounts = {};
        auto allocator=[device newCommandAllocator]; auto command=[device newCommandBuffer];
        [command beginCommandBufferWithAllocator:allocator]; auto encoder=[command computeCommandEncoder];
        methods[0]=class_getInstanceMethod(object_getClass(device),@selector(newComputePipelineStateWithFunction:error:));
        methods[1]=class_getInstanceMethod(object_getClass(encoder),@selector(setComputePipelineState:));
        methods[2]=class_getInstanceMethod(object_getClass(encoder),@selector(dispatchThreads:threadsPerThreadgroup:));
        methods[3]=class_getInstanceMethod(object_getClass(encoder),@selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:));
        methods[4]=class_getInstanceMethod(object_getClass(device),@selector(newLibraryWithSource:options:error:));
        for(auto method:methods)require(method!=nullptr,"observer-method");
        originals[0]=method_setImplementation(methods[0],reinterpret_cast<IMP>(observeNew)); originalNew=reinterpret_cast<NewPipeline>(originals[0]);
        originals[1]=method_setImplementation(methods[1],reinterpret_cast<IMP>(observeSet)); originalSet=reinterpret_cast<SetPipeline>(originals[1]);
        originals[2]=method_setImplementation(methods[2],reinterpret_cast<IMP>(observeDispatch)); originalDispatch=reinterpret_cast<Dispatch>(originals[2]);
        originals[3]=method_setImplementation(methods[3],reinterpret_cast<IMP>(observeCopy)); originalCopy=reinterpret_cast<TextureCopy>(originals[3]);
        originals[4]=method_setImplementation(methods[4],reinterpret_cast<IMP>(observeLibrary)); originalLibrary=reinterpret_cast<NewLibrary>(originals[4]);
        [encoder endEncoding]; [command endCommandBuffer]; [command release]; [allocator release];
    }
    ~Observer(){for(unsigned i=0;i<5;++i)method_setImplementation(methods[i],originals[i]);}
};
template<class T> void header(T& p, unsigned operation, uint64_t context = 0) { p.header = {sizeof(p), WMTFG_BRIDGE_VERSION, operation, 0, context}; }
id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format) {
    auto* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:128 height:128 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
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
    uint64_t create() {
        WMTFGcreate_packet p{}; header(p,WMTFG_CREATE); p.device=(uint64_t)device;
        p.display_width=p.display_height=p.max_render_width=p.max_render_height=128;
        p.backbuffer_format=WMTPixelFormatRGBA16Float; p.flags=WMTFG_DEPTH_INVERTED;
        require(api(WMTFG_CREATE,&p,nullptr)==0,"create"); return p.header.context;
    }
    void destroy(uint64_t context) { WMTFGdestroy_packet p{}; header(p,WMTFG_DESTROY,context); require(api(WMTFG_DESTROY,&p,nullptr)==0,"destroy"); }
    void execute(const std::vector<std::shared_ptr<const PreparedFrame>>& commands) {
        auto allocator = [device newCommandAllocator]; auto command = [device newCommandBuffer];
        RecordingOwner owner{command,[NSMutableArray new]}; auto fence = [device newFence];
        [command beginCommandBufferWithAllocator:allocator];
        std::vector<std::shared_ptr<const ExecutionLease>> leases;
        for (const auto& recorded : commands) { std::shared_ptr<const ExecutionLease> lease; require(recorded->encode(command,fence,lease,(uint64_t)&owner),"encode"); leases.push_back(std::move(lease)); }
        [command endCommandBuffer]; [queue4 commit:&command count:1]; [queue4 signalEvent:completed value:++sequence];
        require([completed waitUntilSignaledValue:sequence timeoutMS:15000],"completion");
        [owner.objects release]; [fence release]; [command release]; [allocator release];
    }
    std::vector<std::shared_ptr<const PreparedFrame>> record(uint64_t context, unsigned frame, id<MTLTexture> color,id<MTLTexture> depth,id<MTLTexture> motion,id<MTLTexture> output,unsigned flags,unsigned render=128,float scale=1,bool mutate=false) {
        WMTFGprepare_packet p{}; header(p,WMTFG_PREPARE,context); p.command_list=(uint64_t)&native; p.frame_id=frame;
        p.depth=(uint64_t)depth; p.motion_vectors=(uint64_t)motion; p.render_width=p.render_height=render;
        p.motion_width=p.motion_height=render;
        p.motion_scale_x=p.motion_scale_y=scale; p.frame_time_delta_ms=33.3333f; p.camera_near=.1f; p.camera_far=100;
        p.camera_fov_vertical_radians=1; p.view_space_to_meters=1;
        std::shared_ptr<const PreparedFrame> prepare; require(api(WMTFG_PREPARE,&p,&prepare)==0,"prepare");
        if(mutate) {
            execute({prepare});
            std::vector<float> poisonDepth(128*128,.125f); std::vector<uint16_t> poisonMotion(128*128*2,0x4900);
            upload(device,queue,depth,poisonDepth.data(),128*4); upload(device,queue,motion,poisonMotion.data(),128*4);
        }
        WMTFGdispatch_packet g{}; header(g,WMTFG_DISPATCH,context); g.command_list=(uint64_t)&native; g.frame_id=frame;
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
    require(counts.libraries==1 && counts.pipelines==4, "one-library-for-four-pipeline-attempts");
    failedPipeline = @"yaagl_fg_decode_crop"; throwPipeline = true;
    require(api(WMTFG_PREPARE, &p, &prepared)==3 && !prepared, "configuration-objc-exception-contained");
    counts = compilations();
    require(counts.libraries==2 && counts.pipelines==5, "failed-configuration-not-cached");
    require(api(WMTFG_PREPARE, &p, &prepared)==0 && bool(prepared), "configuration-retry-succeeds");
    counts = compilations();
    require(counts.libraries==3 && counts.pipelines==9, "retry-creates-one-complete-library-batch");
    p.frame_id=2;
    require(api(WMTFG_PREPARE, &p, &prepared)==0 && bool(prepared), "cached-configuration-prepare");
    counts = compilations();
    require(counts.libraries==3 && counts.pipelines==9, "cached-configuration-does-not-recompile");
    h.destroy(context); [depth release]; [motion release];
    std::puts("OWNED_FG_PIPELINES_PASS complete_batch_libraries=1 pipelines=4 partial_retry=complete objc_exception=contained cache_hit_libraries=0");
}
void normalizationProof(Harness& h) {
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
                // Shader uses nearest floor((tid+.5)*source/extent). For 64->128 this is tid/2.
                unsigned sx=mode==1?x:x/2,sy=mode==1?y:y/2;
                z[y*128+x]=((sx/8+sy/8+frame)%2)? .75f:.25f;
                // Conversion: (.5,-.25)*scale2*output128/render64 == (2,-1).
                mv[(y*128+x)*2]=mode==1?0x3800:0x4000; mv[(y*128+x)*2+1]=mode==1?0xb400:0xbc00;
            }
            upload(h.device,h.queue,depth,z.data(),128*4); upload(h.device,h.queue,motion,mv.data(),128*4);
            operations={}; snapshotSources[0]=depth; snapshotSources[1]=motion;
            const auto beforeCompile = compilations();
            h.execute(h.record(contexts[mode],frame,color,depth,motion,output,frame==6?4:0,mode==1?64:128,mode==1?2:1,mode==2));
            const auto afterCompile = compilations();
            const unsigned batches = frame==1 ? (mode==1 ? 2 : 1) : 0;
            require(afterCompile.libraries-beforeCompile.libraries==batches &&
                    afterCompile.pipelines-beforeCompile.pipelines==4*batches, "configuration-batch-library-count-and-reuse");
            snapshotSources[0]=snapshotSources[1]=nil; auto counts=operations;
            require(counts.snapshots==2,"prepare-two-private-snapshots");
            // Converted depth and motion share one fused normalization dispatch.
            require(mode==1?(counts.fused>0 && !counts.depth && !counts.motion):(!counts.depth && !counts.motion && !counts.fused),"actual-normalization-dispatch-count");
            totals[mode].depth+=counts.depth; totals[mode].motion+=counts.motion; totals[mode].snapshots+=counts.snapshots; totals[mode].fused+=counts.fused;
            auto pixels=read(h.device,h.queue,output);
            if(const char* directory=std::getenv("OWNED_FG_PROOF_OUTPUT")) {
                char path[1024]; require(std::snprintf(path,sizeof(path),"%s/frame-%u-mode-%u.rgba16f",directory,frame,mode)<int(sizeof(path)),"artifact-path");
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
    require(counts.libraries==4 && counts.pipelines==16, "independent-configurations-have-no-global-library-cache");
    for(unsigned mode=0;mode<3;++mode) {h.destroy(contexts[mode]);std::printf("OWNED_FG_NORMALIZATION_COUNTS mode=%u depth_dispatches=%u motion_dispatches=%u fused_dispatches=%u private_snapshot_copies=%u\n",mode,totals[mode].depth,totals[mode].motion,totals[mode].fused,totals[mode].snapshots);}
    std::printf("OWNED_FG_NORMALIZATION_PASS interpolated_frames=%u all_pixels=1 guards=1 snapshots_mutated_after_completion=1\n",interpolated);
}
}
int main() { @autoreleasepool {
    Harness h; auto depth=texture(h.device,MTLPixelFormatR32Float), motion=texture(h.device,MTLPixelFormatRG16Float);
    std::vector<float> z(128*128,.75f); std::vector<uint16_t> mv(128*128*2,0);
    upload(h.device,h.queue,depth,z.data(),128*4); upload(h.device,h.queue,motion,mv.data(),128*4);
    uint64_t contexts[] = {h.create(),h.create(),h.create(),h.create()};
    for (unsigned frame=1;frame<=2;++frame) {
        auto scene=fgScene(frame); auto color=texture(h.device,MTLPixelFormatRGBA16Float); upload(h.device,h.queue,color,scene.data(),128*8);
        std::vector<uint16_t> baseline;
        for (unsigned index=0;index<4;++index) {
            auto output=texture(h.device,MTLPixelFormatRGBA16Float); h.execute(h.record(contexts[index],frame,color,depth,motion,output,index?1u<<(index-1):0u)); auto pixels=read(h.device,h.queue,output);
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
    for (unsigned index : {0u,3u}) { auto target=texture(h.device,MTLPixelFormatRGBA16Float); h.execute(h.record(contexts[index],3,color3,depth,motion,target,0)); auto pixels=read(h.device,h.queue,target); if (!index) cleanHistory=pixels; else require(pixels==cleanHistory,"debug-history-unmodified"); [target release]; }
    std::fill(z.begin(),z.end(),.25f); for (unsigned i=0;i<mv.size();i+=2)mv[i]=0x4100; // 2.5 display pixels.
    upload(h.device,h.queue,depth,z.data(),128*4); upload(h.device,h.queue,motion,mv.data(),128*4);
    auto changedOutput=texture(h.device,MTLPixelFormatRGBA16Float); h.execute(h.record(contexts[3],4,color3,depth,motion,changedOutput,4)); auto changed=read(h.device,h.queue,changedOutput);
    const uint16_t changedMv[]={0x3a00,0x3800,0x3800,0x3c00},changedZ[]={0x3400,0x3400,0x3400,0x3c00};
    require(!std::memcmp(changed.data()+(64*128+64)*4,changedMv,8),"motion-input-sensitivity"); require(!std::memcmp(changed.data()+(64*128+110)*4,changedZ,8),"depth-input-sensitivity"); [changedOutput release]; [color3 release];
    auto replayContext=h.create(); auto color=texture(h.device,MTLPixelFormatRGBA16Float),output=texture(h.device,MTLPixelFormatRGBA16Float);
    auto scene=fgScene(10); upload(h.device,h.queue,color,scene.data(),128*8); auto recorded=h.record(replayContext,10,color,depth,motion,output,7); h.destroy(replayContext); [color release];
    h.execute(recorded); auto first=read(h.device,h.queue,output); h.execute(recorded); require(first==read(h.device,h.queue,output),"destroyed-context-replay-exact");
    for(auto context:contexts)h.destroy(context); [output release]; [depth release]; [motion release];
    std::puts("OWNED_FG_DEBUG_NATIVE_PASS tear=exact reset=first_and_history view=input_sensitive history=unmodified replay=exact destroy_before_execute=1");
    pipelineBatchProof(h);
    normalizationProof(h);
} }
