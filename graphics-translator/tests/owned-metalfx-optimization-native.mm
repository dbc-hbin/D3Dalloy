// SPDX-License-Identifier: LGPL-2.1-or-later
// Root builds with -I<build>/src (generated fsr-kernels.inc).
// clang++ -std=c++20 -O2 -fblocks -framework Foundation -framework Metal \
//   -framework MetalFX -I<build>/src \
//   graphics-translator/tests/owned-metalfx-optimization-native.mm -o <output>
// Includes the owned implementation to inspect native selection and generation IDs.
#include "../../src/winemetal/unix/metalfx-backend.mm"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <objc/runtime.h>

namespace probe {
using namespace yaagl::pso::metalfx;
using TextureCopy = void (*)(id, SEL, id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin, MTLSize,
                            id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin);
Method copyMethod = nullptr;
TextureCopy originalCopy = nullptr;
std::atomic<id<MTLTexture>> observedInputs[3]{};
std::atomic<unsigned> inputCopies[3]{};
std::atomic<uint64_t> inputBytes[3]{};
void observeCopy(id encoder, SEL selector, id<MTLTexture> source, NSUInteger slice, NSUInteger level,
                 MTLOrigin origin, MTLSize size, id<MTLTexture> destination, NSUInteger targetSlice,
                 NSUInteger targetLevel, MTLOrigin targetOrigin) {
    for (unsigned i=0; i<3; ++i) if (source == observedInputs[i].load()) {
        ++inputCopies[i]; inputBytes[i] += size.width * size.height * size.depth * (i == 0 ? 8 : 4);
    }
    originalCopy(encoder,selector,source,slice,level,origin,size,destination,targetSlice,targetLevel,targetOrigin);
}
void require(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "OWNED_METALFX_OPT_FAIL reason=%s\n", reason); std::exit(1); }
}
struct GPU {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTL4CommandQueue> queue4 = [device newMTL4CommandQueue];
    id<MTLSharedEvent> completed = [device newSharedEvent];
    uint64_t event = 0;
    GPU() {
        require(device && queue && queue4 && completed, "native-device-queues");
        auto allocator=[device newCommandAllocator]; auto command=[device newCommandBuffer];
        [command beginCommandBufferWithAllocator:allocator]; auto encoder=[command computeCommandEncoder];
        SEL selector=@selector(copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:);
        copyMethod=class_getInstanceMethod(object_getClass(encoder),selector);
        require(copyMethod != nullptr,"native-texture-copy-method");
        originalCopy=reinterpret_cast<TextureCopy>(method_setImplementation(copyMethod,reinterpret_cast<IMP>(observeCopy)));
        [encoder endEncoding]; [command endCommandBuffer]; [command release]; [allocator release];
    }
    ~GPU() { method_setImplementation(copyMethod,reinterpret_cast<IMP>(originalCopy));
        [completed release]; [queue4 release]; [queue release]; [device release]; }
    void commit(const std::vector<id<MTL4CommandBuffer>>& commands) {
        [queue4 commit:commands.data() count:commands.size()];
        [queue4 signalEvent:completed value:++event];
        require([completed waitUntilSignaledValue:event timeoutMS:15000], "metal4-completion");
    }
    void finish(id<MTLCommandBuffer> command) {
        auto done=dispatch_semaphore_create(0);
        [command addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(done); }];
        [command commit];
        require(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC))==0 &&
                command.status==MTLCommandBufferStatusCompleted,"legacy-command-completion");
        dispatch_release(done);
    }
    void upload(id<MTLTexture> texture, const void* data, size_t row) {
        auto buffer = [device newBufferWithBytes:data length:row * texture.height options:MTLResourceStorageModeShared];
        auto command = [queue commandBuffer]; auto blit = [command blitCommandEncoder];
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * texture.height
            sourceSize:MTLSizeMake(texture.width, texture.height, 1) toTexture:texture
            destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [blit endEncoding]; finish(command);
        [buffer release];
    }
    std::vector<float> read(id<MTLTexture> texture) {
        const size_t row = texture.width * 8, bytes = row * texture.height;
        auto buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        auto command = [queue commandBuffer]; auto blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:buffer destinationOffset:0
            destinationBytesPerRow:row destinationBytesPerImage:bytes];
        [blit endEncoding]; finish(command);
        const auto* halves = static_cast<const _Float16*>(buffer.contents);
        std::vector<float> result(bytes / 2);
        for (size_t i = 0; i < result.size(); ++i) { result[i] = halves[i]; require(std::isfinite(result[i]), "finite-output"); }
        [buffer release]; return result;
    }
};
id<MTLTexture> texture(GPU& gpu, MTLPixelFormat format, unsigned width, MTLTextureUsage usage) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:width mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = usage | MTLTextureUsageShaderRead;
    auto result = [gpu.device newTextureWithDescriptor:d]; require(result != nil, "texture"); return result;
}
bool compare(const std::vector<float>& a, const std::vector<float>& b, const char* name) {
    require(a.size() == b.size(), "comparison-size");
    double sum = 0; float maximum = 0; size_t worst = 0;
    for (size_t i = 0; i < a.size(); ++i) { float delta = std::fabs(a[i] - b[i]); sum += delta; if (delta > maximum) { maximum = delta; worst = i; } }
    std::printf("OWNED_METALFX_OPT_COMPARE case=%s components=%zu mean_abs=%.9g max_abs=%.9g index=%zu direct=%.9g staged=%.9g\n", name,a.size(),sum/a.size(),maximum,worst,a[worst],b[worst]);
    return maximum <= 2.0f / 255.0f;
}
std::vector<float> runSR(GPU& gpu, const std::shared_ptr<Feature::Impl>& core,
                         const std::shared_ptr<ScalerGeneration>& generation, unsigned mask, unsigned iteration,
                         const std::shared_ptr<Feature>& publicFeature = {}) {
    PreparedFrame::Impl frame;
    frame.feature = core; frame.generation = generation;
    auto& p = frame.frame;
    p.inputContent = {120,120}; p.colorRect = {4,3,120,120};
    p.depthRect = {3,5,120,120}; p.motionRect = {6,2,120,120};
    p.outputRect = {4,4,256,256}; p.resetHistory = iteration % 2 == 0;
    frame.scalerMotionRect = p.motionRect; frame.temporalOutputWidth = frame.temporalOutputHeight = 256;
    frame.needsOutputShadow = true;
    frame.color = texture(gpu,MTLPixelFormatRGBA16Float,128,generation->colorUsage);
    frame.depth = texture(gpu,MTLPixelFormatR32Float,128,generation->depthUsage);
    frame.motion = texture(gpu,MTLPixelFormatRG16Float,128,generation->motionUsage);
    frame.output = texture(gpu,MTLPixelFormatRGBA16Float,264,generation->outputUsage);
    frame.textures = {(void*)frame.color,(void*)frame.depth,(void*)frame.motion,(void*)frame.output};
    p.color = frame.textures.color; p.depth = frame.textures.depth; p.motionVectors = frame.textures.motion; p.output = frame.textures.output;
    require(exactDescriptorInput(frame.color,p.colorRect,120,120,128,128,generation->colorUsage) &&
            exactDescriptorInput(frame.depth,p.depthRect,120,120,128,128,generation->depthUsage) &&
            exactDescriptorInput(frame.motion,p.motionRect,120,120,128,128,generation->motionUsage), "sr-offset-eligibility");
    const char* rejection = nullptr;
    require(!exactDescriptorInput(frame.color,p.colorRect,120,120,120,120,generation->colorUsage,&rejection) &&
            std::strcmp(rejection,"descriptor_capacity")==0, "capacity-rejection");
    require(!exactDescriptorInput(frame.color,p.colorRect,120,120,128,128,MTLTextureUsageRenderTarget,&rejection) &&
            std::strcmp(rejection,"native_usage")==0, "usage-rejection");
    std::vector<_Float16> colors(128*128*4, _Float16(8));
    std::vector<float> depths(128*128, .05f);
    std::vector<_Float16> motions(128*128*2, _Float16(64));
    for (unsigned y=0; y<120; ++y) for (unsigned x=0; x<120; ++x) {
        size_t c=((y+p.colorRect.y)*128+x+p.colorRect.x)*4;
        colors[c]=colors[c+1]=colors[c+2]=_Float16(.25f+.5f*float(x)/119); colors[c+3]=_Float16(1);
        depths[(y+p.depthRect.y)*128+x+p.depthRect.x]=.75f;
        size_t m=((y+p.motionRect.y)*128+x+p.motionRect.x)*2; motions[m]=motions[m+1]=0;
    }
    std::vector<_Float16> sentinel(264*264*4, _Float16(.125));
    gpu.upload(frame.color,colors.data(),128*8); gpu.upload(frame.depth,depths.data(),128*4);
    gpu.upload(frame.motion,motions.data(),128*4); gpu.upload(frame.output,sentinel.data(),264*8);
    frame.needsColorStaging = mask & 1; frame.needsDepthStaging = mask & 2; frame.needsMotionStaging = mask & 4;
    Error error; std::shared_ptr<ExecutionLease::Impl> lease;
    std::shared_ptr<const PreparedFrame> prepared; std::shared_ptr<const ExecutionLease> publicLease;
    if (publicFeature) { prepared=publicFeature->prepare(p,frame.textures,&error); require(bool(prepared),error.message.c_str()); }
    else { lease=makeLease(frame,&error); require(bool(lease),error.message.c_str()); }
    observedInputs[0]=frame.color; observedInputs[1]=frame.depth; observedInputs[2]=frame.motion;
    for (unsigned i=0; i<3; ++i) { inputCopies[i]=0; inputBytes[i]=0; }
    auto allocator = [gpu.device newCommandAllocator]; auto fence = [gpu.device newFence];
    std::vector<id<MTL4CommandBuffer>> commands;
    auto begin = [&] { auto command=[gpu.device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
        if (lease) [command useResidencySet:(id<MTLResidencySet>)lease->residency]; commands.push_back(command); return command; };
    auto command = begin();
    if (prepared) {
        struct Segments { GPU* gpu; id<MTL4CommandAllocator> allocator; std::vector<id<MTL4CommandBuffer>>* commands; };
        Segments segments{&gpu,allocator,&commands};
        require(prepared->encode((void*)command,(void*)fence,publicLease,&error,false,&segments,[](void* context) -> void* {
            auto& s=*static_cast<Segments*>(context); [s.commands->back() endCommandBuffer];
            auto next=[s.gpu->device newCommandBuffer]; [next beginCommandBufferWithAllocator:s.allocator];
            s.commands->push_back(next); return (void*)next;
        }),error.message.c_str());
        [commands.back() endCommandBuffer];
    } else {
    require(encodeInputCopiesMetal4(frame,*lease,command,fence), "sr-staging-copies");
    auto boundary=[command computeCommandEncoder];
    [boundary barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [boundary endEncoding]; [command endCommandBuffer]; command=begin();
    configureScalerForFrame(*core,*generation,frame,*lease,nil,p.resetHistory);
    [generation->scaler encodeToCommandBuffer:command]; [command endCommandBuffer]; command=begin();
    boundary=[command computeCommandEncoder];
    [boundary barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [boundary endEncoding]; require(copyOutputMetal4(frame,*lease,command,fence), "sr-output-subrect");
    [command endCommandBuffer];
    }
    // Source resources, generation and lease survive until the queue completion event.
    gpu.commit(commands); auto result=gpu.read(frame.output);
    double mean=0; unsigned outside=0;
    for(unsigned y=0;y<264;++y) for(unsigned x=0;x<264;++x) {
        size_t i=(y*264+x)*4;
        if(x<4||y<4||x>=260||y>=260) { for(unsigned c=0;c<4;++c) require(result[i+c]==.125f,"outside-output-sentinel"); ++outside; }
        else mean+=result[i];
    }
    mean/=256*256; require(std::fabs(mean-.5)<.02,"offset-region-value");
    unsigned copies=0; uint64_t bytes=0;
    for (unsigned i=0; i<3; ++i) {
        require(inputCopies[i].load() == unsigned(bool(mask & (1u << i))),"actual-native-input-copy-count");
        copies+=inputCopies[i]; bytes+=inputBytes[i]; observedInputs[i]=nil;
    }
    std::printf("OWNED_METALFX_OPT_SR iteration=%u staging_mask=%u generation=%llu mean=%.9g outside_pixels=%u primary_input_copies=%u primary_input_copy_bytes=%llu public_prepare=%u\n",
        iteration,mask,(unsigned long long)(prepared ? prepared->generationId() : generation->serial),mean,outside,copies,
        (unsigned long long)bytes,unsigned(bool(prepared)));
    ScalerBindings clear(generation->scaler);
    for(auto c:commands) [c release]; [fence release]; [allocator release];
    return result;
}
std::vector<std::vector<float>> runFG(GPU& gpu, id<MTL4Compiler> compiler, bool staged) {
    auto descriptor=[MTLFXFrameInterpolatorDescriptor new];
    descriptor.inputWidth=descriptor.inputHeight=descriptor.outputWidth=descriptor.outputHeight=128;
    descriptor.colorTextureFormat=descriptor.outputTextureFormat=MTLPixelFormatRGBA16Float;
    descriptor.depthTextureFormat=MTLPixelFormatR32Float; descriptor.motionTextureFormat=MTLPixelFormatRG16Float;
    descriptor.requiresPrevColorTexture=NO;
    auto scaler=[descriptor newFrameInterpolatorWithDevice:gpu.device compiler:compiler];
    require(scaler!=nil,"native-fg-effect");
    auto color=texture(gpu,MTLPixelFormatRGBA16Float,128,scaler.colorTextureUsage);
    auto depth=texture(gpu,MTLPixelFormatR32Float,128,scaler.depthTextureUsage);
    auto motion=texture(gpu,MTLPixelFormatRG16Float,128,scaler.motionTextureUsage);
    auto output=texture(gpu,MTLPixelFormatRGBA16Float,128,scaler.outputTextureUsage);
    auto rd=[MTLResidencySetDescriptor new]; auto residency=[gpu.device newResidencySetWithDescriptor:rd error:nil]; [rd release];
    for (id<MTLTexture> t : {color,depth,motion,output}) [residency addAllocation:t]; [residency commit];
    auto allocator=[gpu.device newCommandAllocator];
    std::vector<std::vector<float>> results; unsigned interpolated=0;
    for(unsigned frame=0;frame<3;++frame) {
        std::vector<_Float16> colors(128*128*4,_Float16(.125));
        // Native FG writes opaque alpha; exercise opaque game color, including its border.
        for (size_t i=3; i<colors.size(); i+=4) colors[i]=1;
        std::vector<float> depths(128*128,.05f); std::vector<_Float16> motions(128*128*2,_Float16(64));
        const unsigned dx=staged?0:3, dy=staged?0:5, mx=staged?0:6, my=staged?0:2;
        for(unsigned y=0;y<120;++y) for(unsigned x=0;x<120;++x) {
            size_t i=(y*128+x)*4;
            _Float16 value=((x+frame*4)/16+y/24)%2 ? _Float16(.75) : _Float16(.25);
            colors[i]=colors[i+1]=colors[i+2]=value; colors[i+3]=1;
            depths[(y+dy)*128+x+dx]=.75f; size_t m=((y+my)*128+x+mx)*2; motions[m]=4; motions[m+1]=0;
        }
        gpu.upload(color,colors.data(),128*8); gpu.upload(depth,depths.data(),128*4); gpu.upload(motion,motions.data(),128*4);
        scaler.colorTexture=color; scaler.depthTexture=depth; scaler.motionTexture=motion; scaler.outputTexture=output;
        scaler.contentWidth=scaler.contentHeight=120;
        scaler.depthContentOffsetX=dx; scaler.depthContentOffsetY=dy;
        scaler.motionContentOffsetX=mx; scaler.motionContentOffsetY=my;
        scaler.outputOffsetX=scaler.outputOffsetY=0;
        scaler.motionVectorScaleX=scaler.motionVectorScaleY=1; scaler.depthReversed=YES; scaler.shouldResetHistory=frame==0;
        scaler.deltaTime=1.f/30; scaler.nearPlane=.1f; scaler.farPlane=100; scaler.fieldOfView=57.2957795f; scaler.aspectRatio=1;
        auto command=[gpu.device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
        [command useResidencySet:residency]; [scaler encodeToCommandBuffer:command]; [command endCommandBuffer];
        gpu.commit({command}); results.push_back(gpu.read(output));
        unsigned changed=0;
        for(unsigned y=0;y<128;++y) for(unsigned x=0;x<128;++x) {
            size_t i=(y*128+x)*4;
            if(x>=120||y>=120) for(unsigned component=0;component<4;++component) {
                if (results.back()[i+component]!=float(colors[i+component]))
                    std::fprintf(stderr,"OWNED_METALFX_FG_OUTSIDE frame=%u x=%u y=%u component=%u actual=%.9g expected=%.9g\n",
                        frame,x,y,component,results.back()[i+component],float(colors[i+component]));
                require(results.back()[i+component]==float(colors[i+component]),"fg-outside-content-sentinel");
            }
            else if(std::fabs(results.back()[i]-float(colors[i]))>1.f/1024) ++changed;
        }
        interpolated+=frame>0 && changed>0;
        std::printf("OWNED_METALFX_OPT_FG staged=%u frame=%u changed_pixels=%u depth_offset=%u,%u motion_offset=%u,%u\n",
            staged,frame,changed,dx,dy,mx,my);
        [command release]; [allocator reset];
    }
    require(interpolated>0,"fg-real-interpolation");
    scaler.colorTexture=scaler.depthTexture=scaler.motionTexture=scaler.outputTexture=nil;
    [allocator release]; [residency release]; [color release]; [depth release]; [motion release]; [output release];
    [scaler release]; [descriptor release]; return results;
}
}
int main() { @autoreleasepool {
    using namespace probe;
    GPU gpu;
    if (@available(macOS 27.0,*)) {} else require(false,"macos27-offset-contract");
    auto core=std::make_shared<Feature::Impl>(); core->device=retainObject(gpu.device);
    auto d=[MTL4CompilerDescriptor new]; core->compiler=[gpu.device newCompilerWithDescriptor:d error:nil]; [d release];
    require(core->compiler!=nil,"metal4-compiler");
    core->create={{128,128},{256,256},FeatureFlagMVLowRes|FeatureFlagIsHDR,true,true};
    core->minScale=[MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:gpu.device];
    core->maxScale=[MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:gpu.device];
    auto color=texture(gpu,MTLPixelFormatRGBA16Float,128,MTLTextureUsageShaderRead);
    auto depth=texture(gpu,MTLPixelFormatR32Float,128,MTLTextureUsageShaderRead);
    auto motion=texture(gpu,MTLPixelFormatRG16Float,128,MTLTextureUsageShaderRead);
    auto output=texture(gpu,MTLPixelFormatRGBA16Float,256,MTLTextureUsageShaderWrite);
    TextureSet textures{(void*)color,(void*)depth,(void*)motion,(void*)output};
    FrameInfo p{}; p.inputContent={128,128}; p.outputRect={0,0,256,256};
    Error error; const uint64_t start=ScalerGeneration::nextId.load();
    auto a=ensureScaler(*core,p,textures,{},&error); require(bool(a),error.message.c_str()); const uint64_t activation=a->activation;
    p.outputRect={0,0,192,192}; auto b=ensureScaler(*core,p,textures,{},&error); require(bool(b),error.message.c_str());
    p.outputRect={0,0,128,128}; auto c=ensureScaler(*core,p,textures,{},&error); require(bool(c),error.message.c_str());
    p.outputRect={0,0,256,256}; auto again=ensureScaler(*core,p,textures,{},&error);
    require(again==a && a!=b && a!=c && b!=c && a->activation==activation+1,"A-B-C-A-reuse-and-activation");
    require(ScalerGeneration::nextId.load()-start==3,"three-real-generation-creations");
    for(unsigned variant=0;variant<9;++variant) {
        p.outputRect={0,0,129+variant*7,129+variant*7};
        require(bool(ensureScaler(*core,p,textures,{},&error)),error.message.c_str());
    }
    unsigned retained=0; bool aCached=false;
    for(const auto& entry:core->generations) { retained+=bool(entry); aCached|=entry==a; }
    require(retained==8 && !aCached && a->scaler!=nil,"bounded-cache-retains-evicted-generation-lease");
    require(ScalerGeneration::nextId.load()-start==12,"nine-additional-native-creations");
    auto publicFeature=Feature::create({(void*)gpu.device,(void*)core->compiler},core->create,&error);
    require(bool(publicFeature),error.message.c_str());
    p.inputContent={128,128}; p.colorRect=p.depthRect=p.motionRect={0,0,128,128};
    p.outputRect={0,0,256,256}; p.color=(void*)color; p.depth=(void*)depth;
    p.motionVectors=(void*)motion; p.output=(void*)output;
    auto prime=publicFeature->prepare(p,textures,&error); require(bool(prime),error.message.c_str());
    auto direct=runSR(gpu,core,a,6,0,publicFeature); auto warm=runSR(gpu,core,a,6,1,publicFeature);
    auto staged=runSR(gpu,core,a,7,2); auto stagedWarm=runSR(gpu,core,a,7,3);
    bool parity=compare(direct,staged,"cold-offset-forced-staging");
    parity &= compare(warm,stagedWarm,"warm-offset-forced-staging");
    auto repeatCold=runSR(gpu,core,a,6,4,publicFeature), repeatWarm=runSR(gpu,core,a,6,5,publicFeature);
    parity &= compare(repeatCold,staged,"repeat-offset-cold");
    parity &= compare(repeatWarm,stagedWarm,"repeat-offset-history");
    require(parity,"direct-staged-numeric-parity");
    std::printf("OWNED_METALFX_OPT_CACHE sequence=A,B,C,A creations=3 reused=1 activation_reset=1 capacity=8\n");
    std::printf("OWNED_METALFX_OPT_EVICTION retained=%u additional_creations=9 evicted_generation_executed=1\n",retained);
    auto fgDirect=runFG(gpu,(id<MTL4Compiler>)core->compiler,false);
    auto fgStaged=runFG(gpu,(id<MTL4Compiler>)core->compiler,true);
    for(unsigned frame=0;frame<3;++frame) require(compare(fgDirect[frame],fgStaged[frame],"fg-offset-staging"),"fg-direct-staged-numeric-parity");
    [color release]; [depth release]; [motion release]; [output release];
    std::puts("OWNED_METALFX_OPT_PASS sr_offset_cases=6 fg_offset_cases=6 async_valid_outputs=1");
} }
