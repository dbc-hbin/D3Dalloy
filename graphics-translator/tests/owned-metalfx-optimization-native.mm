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
#include <thread>
#include <objc/runtime.h>

namespace probe {
using namespace yaagl::pso::metalfx;
namespace metalfx = yaagl::pso::metalfx;
using TextureCopy = void (*)(id, SEL, id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin, MTLSize,
                            id<MTLTexture>, NSUInteger, NSUInteger, MTLOrigin);
Method copyMethod = nullptr;
TextureCopy originalCopy = nullptr;
std::atomic<id<MTLTexture>> observedInputs[3]{};
std::atomic<unsigned> inputCopies[3]{};
std::atomic<uint64_t> inputBytes[3]{};
std::atomic<id<MTLTexture>> observedOutput{};
std::atomic<unsigned> outputCopies{};
void observeCopy(id encoder, SEL selector, id<MTLTexture> source, NSUInteger slice, NSUInteger level,
                 MTLOrigin origin, MTLSize size, id<MTLTexture> destination, NSUInteger targetSlice,
                 NSUInteger targetLevel, MTLOrigin targetOrigin) {
    for (unsigned i=0; i<3; ++i) if (source == observedInputs[i].load()) {
        ++inputCopies[i]; inputBytes[i] += size.width * size.height * size.depth * (i == 0 ? 8 : 4);
    }
    if (destination == observedOutput.load()) ++outputCopies;
    originalCopy(encoder,selector,source,slice,level,origin,size,destination,targetSlice,targetLevel,targetOrigin);
}
void require(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "OWNED_METALFX_OPT_FAIL reason=%s\n", reason); std::exit(1); }
}
using NewLibrary = id<MTLLibrary> (*)(id, SEL, NSString*, MTLCompileOptions*, NSError**);
using NewPipeline = id<MTLComputePipelineState> (*)(id, SEL, id<MTLFunction>, NSError**);
NewLibrary originalLibrary = nullptr;
NewPipeline originalPipeline = nullptr;
unsigned fsrLibraries = 0, exposureLibraries = 0, pipelineAttempts = 0;
NSString* failedPipeline = nil;
bool throwLibrary = false, throwPipeline = false;
id<MTLLibrary> observeLibrary(id device, SEL selector, NSString* source, MTLCompileOptions* options, NSError** error) {
    if ([source isEqualToString:[NSString stringWithUTF8String:kFsrKernelsSource]]) {
        ++fsrLibraries;
        if (throwLibrary) {
            throwLibrary = false;
            [NSException raise:NSInternalInconsistencyException format:@"injected library failure"];
        }
    }
    if ([source isEqualToString:[NSString stringWithUTF8String:kExposureKernel]]) ++exposureLibraries;
    return originalLibrary(device, selector, source, options, error);
}
id<MTLComputePipelineState> observePipeline(id device, SEL selector, id<MTLFunction> function, NSError** error) {
    if ([function.name hasPrefix:@"yaagl_"]) ++pipelineAttempts;
    if ([function.name isEqualToString:failedPipeline]) {
        failedPipeline = nil;
        if (throwPipeline) {
            throwPipeline = false;
            [NSException raise:NSInternalInconsistencyException format:@"injected pipeline failure"];
        }
        return nil;
    }
    return originalPipeline(device, selector, function, error);
}
void pipelineBatchProof(id<MTLDevice> device) {
    Method libraryMethod = class_getInstanceMethod(object_getClass(device), @selector(newLibraryWithSource:options:error:));
    Method pipelineMethod = class_getInstanceMethod(object_getClass(device), @selector(newComputePipelineStateWithFunction:error:));
    require(libraryMethod && pipelineMethod, "pipeline-observer-methods");
    originalLibrary = reinterpret_cast<NewLibrary>(method_setImplementation(libraryMethod, reinterpret_cast<IMP>(observeLibrary)));
    originalPipeline = reinterpret_cast<NewPipeline>(method_setImplementation(pipelineMethod, reinterpret_cast<IMP>(observePipeline)));
    const CreateInfo create{{128,128},{256,256},FeatureFlagMVLowRes|FeatureFlagIsHDR|FeatureFlagAlphaUpscaling,true,true};
    auto core = std::make_shared<Feature::Impl>();
    core->device = retainObject(device); core->create = create;
    Error error;
    FsrPipelineNeeds linearOnly; linearOnly.linearize = true;
    require(ensureFsrPipelines(*core, linearOnly, &error) && fsrLibraries == 1 && pipelineAttempts == 1 &&
            core->linearizePipeline && !core->combineMaskPipeline && !core->finishPipeline,
            "lazy-compiles-only-needed-pipeline");
    FsrPipelineNeeds all; all.linearize = all.combineMasks = all.finish = all.alpha = all.debug = all.padding = true;
    failedPipeline = @"yaagl_fsr_combine_masks";
    require(!ensureFsrPipelines(*core, all, &error) && error.code == ErrorCode::ResourceCreationFailed,
            "partial-pipeline-creation-reported");
    require(fsrLibraries == 1 && pipelineAttempts == 6 && !core->combineMaskPipeline,
            "failure-attempts-every-needed-pipeline-from-one-library");
    std::array<id<MTLComputePipelineState>, 5> retained = {core->linearizePipeline, core->finishPipeline,
        core->alphaPipeline, core->debugPipeline, core->paddingPipeline};
    for (auto pipeline : retained) require(pipeline != nil, "successful-partial-pipeline-retained");
    clearError(&error);
    require(ensureFsrPipelines(*core, all, &error) && !error && fsrLibraries == 1 && pipelineAttempts == 7,
            "retry-compiles-only-missing-pipeline");
    require(retained == std::array<id<MTLComputePipelineState>, 5>{core->linearizePipeline, core->finishPipeline,
        core->alphaPipeline, core->debugPipeline, core->paddingPipeline}, "retry-preserves-existing-pipelines");
    require(ensureFsrPipelines(*core, all, nullptr) && fsrLibraries == 1 && pipelineAttempts == 7,
            "complete-feature-does-not-recompile");
    require(ensureExposurePipeline(*core, &error) && exposureLibraries == 1 && fsrLibraries == 1,
            "exposure-uses-separate-source-library");
    require(ensureExposurePipeline(*core, nullptr) && exposureLibraries == 1, "exposure-reuses-pipeline");
    {
        auto shared = std::make_shared<Feature::Impl>();
        shared->device = retainObject(device); shared->create = create;
        const unsigned attempts = pipelineAttempts;
        require(ensureFsrPipelines(*shared, linearOnly, &error) && fsrLibraries == 1 &&
                shared->fsrLibrary == core->fsrLibrary && shared->linearizePipeline == core->linearizePipeline &&
                pipelineAttempts == attempts, "same-device-features-share-library-and-pso");
        require(ensureExposurePipeline(*shared, &error) && shared->exposurePipeline == core->exposurePipeline &&
                exposureLibraries == 1 && pipelineAttempts == attempts, "same-device-exposure-pso");
        std::array<std::thread, 6> threads;
        std::array<id<MTLComputePipelineState>, 6> firstPipelines{};
        FsrPipelineNeeds concurrent = all; concurrent.debugDepth = true;
        for (size_t i = 0; i < threads.size(); ++i) threads[i] = std::thread([&, i] { @autoreleasepool {
            auto parallel = std::make_shared<Feature::Impl>();
            parallel->device = retainObject(device); parallel->create = create;
            Error local;
            require(ensureFsrPipelines(*parallel, concurrent, &local) && ensureExposurePipeline(*parallel, &local) &&
                    parallel->finishPipeline == core->finishPipeline &&
                    parallel->exposurePipeline == core->exposurePipeline, "concurrent-feature-pso-reuse");
            firstPipelines[i] = parallel->debugDepthPipeline;
        } });
        for (auto& thread : threads) thread.join();
        for (auto pipeline : firstPipelines)
            require(pipeline && pipeline == firstPipelines[0], "concurrent-first-use-single-pso");
        require(pipelineAttempts == attempts + 1, "concurrent-first-use-one-compilation");
    }
    core.reset();
    auto independent = std::make_shared<Feature::Impl>();
    independent->device = retainObject(device); independent->create = create;
    throwLibrary = true;
    require(!ensureFsrPipelines(*independent, all, &error) &&
            fsrLibraries == 2 && independent->fsrLibrary && !independent->fsrLibrary->library,
            "library-objc-exception-contained-and-not-cached");
    failedPipeline = @"yaagl_fsr_combine_masks"; throwPipeline = true;
    require(!ensureFsrPipelines(*independent, all, &error) && fsrLibraries == 3,
            "pipeline-objc-exception-contained");
    require(ensureFsrPipelines(*independent, all, &error) && fsrLibraries == 3,
            "retry-after-exception-reuses-library");
    method_setImplementation(pipelineMethod, reinterpret_cast<IMP>(originalPipeline));
    method_setImplementation(libraryMethod, reinterpret_cast<IMP>(originalLibrary));
    std::puts("OWNED_METALFX_OPT_PIPELINES_PASS lazy=1 device_shared_library=1 device_shared_pso=1 concurrent_features=6 partial_retry_missing=1 exposure_shared=1 objc_exceptions=contained");
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
                         const std::shared_ptr<Feature>& publicFeature = {},
                         MTLPixelFormat depthFormat = MTLPixelFormatR32Float) {
    PreparedFrame::Impl frame;
    frame.feature = core; frame.generation = generation;
    auto& p = frame.frame;
    p.inputContent = {120,120}; p.colorRect = {4,3,120,120};
    p.depthRect = {3,5,120,120}; p.motionRect = {6,2,120,120};
    p.outputRect = {4,4,256,256}; p.resetHistory = iteration % 2 == 0;
    frame.scalerMotionRect = p.motionRect; frame.temporalOutputWidth = frame.temporalOutputHeight = 256;
    frame.needsOutputShadow = true;
    frame.color = texture(gpu,MTLPixelFormatRGBA16Float,128,generation->colorUsage);
    frame.depth = texture(gpu,depthFormat,128,generation->depthUsage);
    frame.motion = texture(gpu,MTLPixelFormatRG16Float,128,generation->motionUsage);
    frame.output = texture(gpu,MTLPixelFormatRGBA16Float,264,generation->outputUsage);
    frame.textures = {(void*)frame.color,(void*)frame.depth,(void*)frame.motion,(void*)frame.output};
    p.color = frame.textures.color; p.depth = frame.textures.depth; p.motionVectors = frame.textures.motion; p.output = frame.textures.output;
    require(exactDescriptorInput(frame.color,p.colorRect,120,120,128,128,generation->colorUsage) &&
            exactDescriptorInput(frame.depth,p.depthRect,120,120,128,128,generation->depthUsage) &&
            exactDescriptorInput(frame.motion,p.motionRect,120,120,128,128,generation->motionUsage), "sr-offset-eligibility");
    require(!exactDescriptorInput(frame.color,p.colorRect,120,120,120,120,generation->colorUsage),
            "capacity-rejection");
    require(!exactDescriptorInput(frame.color,p.colorRect,120,120,128,128,MTLTextureUsageRenderTarget),
            "usage-rejection");
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
    observedOutput=frame.output; outputCopies=0;
    auto allocator = [gpu.device newCommandAllocator]; auto fence = [gpu.device newFence];
    // D3D12 attaches its device residency set to every recording. This standalone
    // fixture must provide the same ownership/residency for its caller textures.
    auto residencyDescriptor = [MTLResidencySetDescriptor new];
    auto callerResidency = [gpu.device newResidencySetWithDescriptor:residencyDescriptor error:nil];
    [residencyDescriptor release]; require(callerResidency != nil, "sr-caller-residency");
    for (id texture : {frame.color,frame.depth,frame.motion,frame.output}) [callerResidency addAllocation:texture];
    [callerResidency commit]; [callerResidency requestResidency];
    std::vector<id<MTL4CommandBuffer>> commands;
    auto begin = [&] { auto command=[gpu.device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
        [command useResidencySet:callerResidency];
        if (lease) [command useResidencySet:(id<MTLResidencySet>)lease->residency]; commands.push_back(command); return command; };
    auto command = begin();
    if (prepared) {
        struct Segments { GPU* gpu; id<MTL4CommandAllocator> allocator; std::vector<id<MTL4CommandBuffer>>* commands; id<MTLResidencySet> residency; };
        Segments segments{&gpu,allocator,&commands,callerResidency};
        require(prepared->encode((void*)command,(void*)fence,publicLease,&error,false,&segments,[](void* context) -> void* {
            auto& s=*static_cast<Segments*>(context); [s.commands->back() endCommandBuffer];
            auto next=[s.gpu->device newCommandBuffer]; [next beginCommandBufferWithAllocator:s.allocator];
            [next useResidencySet:s.residency];
            s.commands->push_back(next); return (void*)next;
        }),error.message.c_str());
        [commands.back() endCommandBuffer];
    } else {
    require(encodePrepassMetal4(frame,*lease,command,fence), "sr-staging-copies");
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
    mean/=256*256;
    if (std::fabs(mean-.5)>=.02)
        std::fprintf(stderr,"OWNED_METALFX_OFFSET_DIAGNOSTIC iteration=%u mask=%u public=%u mean=%.9g first=%.9g center=%.9g last=%.9g\n",
            iteration, mask, unsigned(bool(prepared)), mean, result[(4*264+4)*4],
            result[(132*264+132)*4], result[(259*264+259)*4]);
    require(std::fabs(mean-.5)<.02,"offset-region-value");
    unsigned copies=0; uint64_t bytes=0;
    for (unsigned i=0; i<3; ++i) {
        // Public prepare pads readable RGBA16F/R32F/RG16F inputs by compute;
        // depth formats keep the blit copy and edge-replication fallback.
        const bool blitDepth = i == 1 && depthFormat != MTLPixelFormatR32Float;
        require(inputCopies[i].load() == (prepared ? unsigned(blitDepth) : unsigned(bool(mask & (1u << i)))),
                "actual-native-input-copy-count");
        copies+=inputCopies[i]; bytes+=inputBytes[i]; observedInputs[i]=nil;
    }
    // Public prepare writes the offset Private output in place; the manual path forces the shadow.
    const unsigned shadowCopies=outputCopies.load(); observedOutput=nil;
    require(shadowCopies == (prepared ? 0u : 1u), "actual-native-output-shadow-copy-count");
    std::printf("OWNED_METALFX_OPT_SR iteration=%u staging_mask=%u generation=%llu mean=%.9g outside_pixels=%u primary_input_copies=%u primary_input_copy_bytes=%llu output_shadow_copies=%u public_prepare=%u\n",
        iteration,mask,(unsigned long long)(prepared ? prepared->generationId() : generation->serial),mean,outside,copies,
        (unsigned long long)bytes,shadowCopies,unsigned(bool(prepared)));
    ScalerBindings clear(generation->scaler);
    for(auto c:commands) [c release]; [fence release]; [allocator release];
    [callerResidency endResidency]; [callerResidency release];
    return result;
}
// Byte-exact padding oracle: every scratch texel equals the source texel at the
// clamped active position (compute copies and the blit fallback alike); the
// generated linear color and combined mask replicate their own active edges.
std::vector<uint8_t> readRaw(GPU& gpu, id<MTLTexture> texture, size_t bpp) {
    const size_t row = texture.width * bpp, bytes = row * texture.height;
    auto buffer = [gpu.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    auto command = [gpu.queue commandBuffer]; auto blit = [command blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:bytes];
    [blit endEncoding]; gpu.finish(command);
    std::vector<uint8_t> result(static_cast<const uint8_t*>(buffer.contents),
                                static_cast<const uint8_t*>(buffer.contents) + bytes);
    [buffer release]; return result;
}
id<MTLTexture> filled(GPU& gpu, MTLPixelFormat format, unsigned size, size_t bpp, unsigned seed,
                      std::vector<uint8_t>& bytes) {
    auto result = texture(gpu, format, size, MTLTextureUsageUnknown);
    bytes.resize(size * size * bpp);
    uint32_t state = seed * 2654435761u + 1;
    for (size_t i = 0; i < bytes.size(); ++i) {
        state = state * 1664525u + 1013904223u; bytes[i] = uint8_t(state >> 24);
    }
    if (format == MTLPixelFormatRGBA16Float || format == MTLPixelFormatRG16Float)
        for (size_t i = 0; i < bytes.size(); i += 2) { // finite halves in [-8, 8)
            _Float16 value = _Float16(float(int(bytes[i] | (bytes[i + 1] << 8)) - 32768) / 4096.f);
            std::memcpy(&bytes[i], &value, 2);
        }
    if (format == MTLPixelFormatR32Float)
        for (size_t i = 0; i < bytes.size(); i += 4) {
            uint32_t bits; std::memcpy(&bits, &bytes[i], 4);
            float value = float(bits % 1000003u) / 1000003.f; std::memcpy(&bytes[i], &value, 4);
        }
    gpu.upload(result, bytes.data(), size * bpp);
    return result;
}
using DispatchThreads = void (*)(id, SEL, MTLSize, MTLSize);
DispatchThreads originalDispatch = nullptr;
unsigned observedDispatches = 0;
void observeDispatch(id encoder, SEL selector, MTLSize extent, MTLSize group) {
    ++observedDispatches; originalDispatch(encoder, selector, extent, group);
}
void fusedColorAlphaProof(GPU& gpu) {
    unsigned cases = 0;
    for (auto format : {MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB})
    for (auto transfer : {ColorTransfer::SRGB, ColorTransfer::PQ}) {
        auto core = std::make_shared<Feature::Impl>(); core->device = retainObject(gpu.device);
        auto descriptor = [MTL4CompilerDescriptor new];
        core->compiler = [gpu.device newCompilerWithDescriptor:descriptor error:nil]; [descriptor release];
        require(core->compiler != nil, "prepass-compiler");
        core->create = {{128,128},{256,256},FeatureFlagMVLowRes|FeatureFlagIsHDR|FeatureFlagAlphaUpscaling,true,true};
        core->minScale = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:gpu.device];
        core->maxScale = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:gpu.device];
        PreparedFrame::Impl frame; frame.feature = core;
        std::vector<uint8_t> input;
        frame.color = filled(gpu, format, 128, format == MTLPixelFormatRGBA16Float ? 8 : 4, 7, input);
        frame.depth = texture(gpu,MTLPixelFormatR32Float,128,MTLTextureUsageShaderRead);
        frame.motion = texture(gpu,MTLPixelFormatRG16Float,128,MTLTextureUsageShaderRead);
        frame.output = texture(gpu,MTLPixelFormatRGBA16Float,256,MTLTextureUsageShaderWrite);
        frame.textures = {(void*)frame.color,(void*)frame.depth,(void*)frame.motion,(void*)frame.output};
        frame.frame.inputContent = {128,128};
        frame.frame.colorRect = frame.frame.depthRect = frame.frame.motionRect = {0,0,128,128};
        frame.frame.outputRect = {0,0,256,256};
        frame.temporalOutputWidth = frame.temporalOutputHeight = 256;
        frame.needsOutputShadow = true; frame.operations.colorTransfer = transfer;
        Error error;
        frame.generation = ensureScaler(*core,frame.frame,frame.textures,frame.operations,&error);
        require(frame.generation && frame.generation->alphaScaler,"prepass-alpha-generation");
        FsrPipelineNeeds needs;
        needs.linearize = needs.alpha = needs.padding = needs.linearizeAlpha = true;
        require(ensureFsrPipelines(*core,needs,&error),"prepass-pipelines");
        auto residencyDesc = [MTLResidencySetDescriptor new];
        auto inputs = [gpu.device newResidencySetWithDescriptor:residencyDesc error:nil]; [residencyDesc release];
        require(inputs != nil,"prepass-input-residency");
        [inputs addAllocation:frame.color]; [inputs commit]; [inputs requestResidency];
        for (metalfx::Rect rect : {metalfx::Rect{0,0,128,128}, {3,5,96,72}, {0,0,127,1}, {1,0,1,128}}) {
            frame.frame.inputContent = {rect.width,rect.height}; frame.frame.colorRect = rect;
            std::array<std::vector<uint8_t>,2> reference; unsigned standaloneDispatches = 0;
            for (bool fused : {false,true}) {
                auto lease = makeLease(frame,&error); require(bool(lease),error.message.c_str());
                require(lease->parameterArena != nil, "single-parameter-arena");
                std::vector<std::pair<uint64_t,uint64_t>> spans;
                for (const auto& slice : {lease->linearizeParams,lease->maskParams,lease->finishParams,
                                         lease->alphaParams,lease->debugParams,lease->copyParams}) {
                    if (!slice) continue;
                    const uint64_t offset = slice.gpuAddress - lease->parameterArena.gpuAddress;
                    require(slice.buffer == lease->parameterArena && offset % 256 == 0 &&
                            offset + slice.length <= lease->parameterArena.length, "parameter-arena-aligned-slices");
                    spans.emplace_back(offset, offset + slice.length);
                }
                std::sort(spans.begin(),spans.end());
                for (size_t i=1;i<spans.size();++i) require(spans[i-1].second <= spans[i].first,
                                                        "parameter-arena-disjoint-slices");
                const auto* constants = static_cast<const uint8_t*>(lease->parameterArena.contents);
                std::vector<uint8_t> frozen(constants,constants+lease->parameterArena.length);
                auto concurrentLease = makeLease(frame,&error);
                require(concurrentLease && concurrentLease->parameterArena != lease->parameterArena &&
                        !std::memcmp(frozen.data(),lease->parameterArena.contents,frozen.size()),
                        "active-executions-have-private-immutable-arenas");
                concurrentLease.reset();
                auto saved = core->linearizeAlphaPipeline;
                if (!fused) core->linearizeAlphaPipeline = nil;
                auto allocator = [gpu.device newCommandAllocator]; auto fence = [gpu.device newFence];
                auto command = [gpu.device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
                [command useResidencySet:(id<MTLResidencySet>)lease->residency]; [command useResidencySet:inputs];
                auto initial = [command computeCommandEncoder];
                [initial updateFence:fence afterEncoderStages:MTLStageDispatch];
                auto method = class_getInstanceMethod(object_getClass(initial),@selector(dispatchThreads:threadsPerThreadgroup:));
                require(method != nullptr,"dispatch-observer");
                [initial endEncoding]; observedDispatches = 0;
                originalDispatch = reinterpret_cast<DispatchThreads>(method_setImplementation(method,reinterpret_cast<IMP>(observeDispatch)));
                const bool accepted = encodePrepassMetal4(frame,*lease,command,fence);
                method_setImplementation(method,reinterpret_cast<IMP>(originalDispatch));
                core->linearizeAlphaPipeline = saved;
                require(accepted,"prepass-encode");
                [command endCommandBuffer]; gpu.commit({command});
                std::array<std::vector<uint8_t>,2> actual{readRaw(gpu,lease->linearColor,8),readRaw(gpu,lease->alphaColor,8)};
                if (!fused) { reference = actual; standaloneDispatches = observedDispatches; }
                else {
                    require(actual == reference,"fused-color-alpha-bit-exact-including-padding");
                    require(observedDispatches + 1 == standaloneDispatches,"one-fewer-native-dispatch");
                }
                [command release]; [fence release]; [allocator release];
            }
            ++cases;
        }
        [inputs endResidency]; [inputs release];
    }
    std::printf("OWNED_METALFX_FUSED_PREPASS_PASS cases=%u color_bit_exact=1 alpha_bit_exact=1 padded_origins=1 srgb_hardware_decode=1 one_fewer_dispatch=1 parameter_buffers_per_execution=1 aligned_immutable_slices=1\n", cases);
}
unsigned paddingOracle(GPU& gpu, const std::shared_ptr<Feature::Impl>& core,
                       const std::shared_ptr<ScalerGeneration>& generation, bool compute,
                       ColorTransfer transfer, MTLPixelFormat colorFormat, metalfx::Rect active) {
    PreparedFrame::Impl frame;
    frame.feature = core; frame.generation = generation;
    frame.operations.colorTransfer = transfer;
    frame.operations.combineCompositionMask = transfer != ColorTransfer::Linear;
    auto& p = frame.frame;
    const unsigned cap = unsigned(generation->inputCapacityWidth);
    p.inputContent = {active.width, active.height};
    p.colorRect = {active.x, active.y, active.width, active.height};
    p.depthRect = {std::min(active.y, cap - active.width), std::min(active.x, cap - active.height),
                   active.width, active.height};
    p.motionRect = {cap - active.width, cap - active.height, active.width, active.height};
    p.reactiveRect = {(cap - active.width) / 2, (cap - active.height) / 2, active.width, active.height};
    p.compositionRect = {cap - active.width, 0, active.width, active.height};
    frame.scalerMotionRect = p.motionRect;
    const size_t colorBpp = colorFormat == MTLPixelFormatRGBA16Float ? 8 : 4;
    std::vector<uint8_t> colors, depths, motions, reactives, compositions;
    frame.color = filled(gpu, colorFormat, cap, colorBpp, 1, colors);
    frame.depth = filled(gpu, MTLPixelFormatR32Float, cap, 4, 2, depths);
    frame.motion = filled(gpu, MTLPixelFormatRG16Float, cap, 4, 3, motions);
    frame.reactive = filled(gpu, MTLPixelFormatR8Unorm, cap, 1, 4, reactives);
    frame.composition = transfer != ColorTransfer::Linear
        ? filled(gpu, MTLPixelFormatR8Unorm, cap, 1, 5, compositions) : nil;
    frame.textures = {(void*)frame.color,(void*)frame.depth,(void*)frame.motion,nullptr,nullptr,
                      (void*)frame.reactive,(void*)frame.composition};
    frame.needsColorStaging = transfer == ColorTransfer::Linear;
    frame.needsDepthStaging = frame.needsMotionStaging = true;
    frame.needsReactiveStaging = transfer == ColorTransfer::Linear;
    frame.computeCopyMask = compute ? (frame.needsColorStaging ? 1u : 0u) | 2u | 4u |
                                      (frame.needsReactiveStaging ? 8u : 0u) : 0u;
    Error error; auto lease = makeLease(frame, &error); require(bool(lease), error.message.c_str());
    require(!lease->stagedDepth.allowGPUOptimizedContents &&
            (!lease->stagedColor || !lease->stagedColor.allowGPUOptimizedContents) &&
            (!lease->linearColor || !lease->linearColor.allowGPUOptimizedContents),
            "padded-scratch-not-gpu-optimized");
    auto allocator = [gpu.device newCommandAllocator]; auto fence = [gpu.device newFence];
    auto command = [gpu.device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
    [command useResidencySet:(id<MTLResidencySet>)lease->residency];
    auto callerDescriptor = [MTLResidencySetDescriptor new];
    auto callerResidency = [gpu.device newResidencySetWithDescriptor:callerDescriptor error:nil];
    [callerDescriptor release]; require(callerResidency != nil, "padding-caller-residency");
    for (id texture : {frame.color,frame.depth,frame.motion,frame.reactive,frame.composition})
        if (texture) [callerResidency addAllocation:texture];
    [callerResidency commit]; [callerResidency requestResidency]; [command useResidencySet:callerResidency];
    require(encodePrepassMetal4(frame, *lease, command, fence), "oracle-encode");
    [command endCommandBuffer]; gpu.commit({command});
    [callerResidency endResidency]; [callerResidency release];
    unsigned checked = 0;
    const auto expectCopy = [&](id<MTLTexture> staged, const std::vector<uint8_t>& source,
                                size_t bpp, const metalfx::Rect& rect, const char* name) {
        if (!staged) return;
        auto actual = readRaw(gpu, staged, bpp);
        for (unsigned y = 0; y < cap; ++y) for (unsigned x = 0; x < cap; ++x) {
            const unsigned sx = rect.x + std::min(x, rect.width - 1), sy = rect.y + std::min(y, rect.height - 1);
            if (std::memcmp(&actual[(y * cap + x) * bpp], &source[(sy * cap + sx) * bpp], bpp)) {
                std::fprintf(stderr, "OWNED_METALFX_PAD_MISMATCH input=%s compute=%u x=%u y=%u\n", name, compute, x, y);
                require(false, "copied-padding-exact");
            }
        }
        ++checked;
    };
    expectCopy(lease->stagedColor, colors, colorBpp, p.colorRect, "color");
    expectCopy(lease->stagedDepth, depths, 4, p.depthRect, "depth");
    expectCopy(lease->stagedMotion, motions, 4, p.motionRect, "motion");
    expectCopy(lease->stagedReactive, reactives, 1, p.reactiveRect, "reactive");
    const auto expectSelfPadded = [&](id<MTLTexture> texture, size_t bpp, const char* name) {
        if (!texture) return;
        auto actual = readRaw(gpu, texture, bpp);
        for (unsigned y = 0; y < cap; ++y) for (unsigned x = 0; x < cap; ++x) {
            const unsigned sx = std::min(x, active.width - 1), sy = std::min(y, active.height - 1);
            if (std::memcmp(&actual[(y * cap + x) * bpp], &actual[(sy * cap + sx) * bpp], bpp)) {
                std::fprintf(stderr, "OWNED_METALFX_PAD_MISMATCH input=%s x=%u y=%u\n", name, x, y);
                require(false, "generated-padding-exact");
            }
        }
        ++checked;
    };
    expectSelfPadded(lease->linearColor, 8, "linear-color");
    if (lease->combinedMask) {
        auto actual = readRaw(gpu, lease->combinedMask, 1);
        for (unsigned y = 0; y < cap; ++y) for (unsigned x = 0; x < cap; ++x) {
            const unsigned ox = std::min(x, active.width - 1), oy = std::min(y, active.height - 1);
            const uint8_t r = reactives[(p.reactiveRect.y + oy) * cap + p.reactiveRect.x + ox];
            const uint8_t c = compositions[(p.compositionRect.y + oy) * cap + p.compositionRect.x + ox];
            require(actual[y * cap + x] == std::max(r, c), "combined-mask-clamped-exact");
        }
        ++checked;
    }
    for (id<MTLTexture> t : {frame.color, frame.depth, frame.motion, frame.reactive, frame.composition}) [t release];
    frame.color = frame.depth = frame.motion = frame.reactive = frame.composition = nil;
    [command release]; [fence release]; [allocator release];
    return checked;
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
int main(int argc, char** argv) { @autoreleasepool {
    using namespace probe;
    const bool pipelinesOnly = argc == 2 && std::strcmp(argv[1], "--pipelines-only") == 0;
    const bool prepassesOnly = argc == 2 && std::strcmp(argv[1], "--prepasses-only") == 0;
    require(argc == 1 || pipelinesOnly || prepassesOnly, "arguments");
    GPU gpu;
    if (@available(macOS 27.0,*)) {} else require(false,"macos27-offset-contract");
    pipelineBatchProof(gpu.device);
    if (pipelinesOnly) return 0;
    fusedColorAlphaProof(gpu);
    if (prepassesOnly) return 0;
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
    p.outputRect={0,0,256,256}; auto again=ensureScaler(*core,p,textures,{},&error);
    require(again==a && a!=b && a->activation==activation+1,"A-B-A-reuse-and-activation");
    again=ensureScaler(*core,p,textures,{},&error);
    require(again==a && a->activation==activation+1,"unchanged-selection-keeps-activation");
    p.outputRect={0,0,128,128}; auto c=ensureScaler(*core,p,textures,{},&error); require(bool(c),error.message.c_str());
    require(c!=a && c!=b && ScalerGeneration::nextId.load()-start==3,"three-real-generation-creations");
    // Dynamic output keeps only the new and the previously current output extents.
    for(unsigned variant=0;variant<9;++variant) {
        p.outputRect={0,0,129+variant*7,129+variant*7};
        require(bool(ensureScaler(*core,p,textures,{},&error)),error.message.c_str());
    }
    unsigned retained=0; bool aCached=false;
    for(const auto& entry:core->generations) { retained+=bool(entry); aCached|=entry==a; }
    require(retained==2 && !aCached && a->scaler!=nil,"two-output-eviction-retains-evicted-generation-lease");
    require(ScalerGeneration::nextId.load()-start==12,"nine-additional-native-creations");
    {
        // Exact-extent preference: a fitting high-water generation yields to a
        // cached exact one; the activation bumps only when the selection changes.
        auto exact=std::make_shared<Feature::Impl>(); exact->device=retainObject(gpu.device);
        exact->compiler=retainObject(core->compiler); exact->create=core->create;
        exact->minScale=core->minScale; exact->maxScale=core->maxScale;
        FrameInfo q{}; q.outputRect={0,0,256,256};
        q.inputContent={112,112}; auto small=ensureScaler(*exact,q,textures,{},&error); require(bool(small),error.message.c_str());
        q.inputContent={128,128}; auto large=ensureScaler(*exact,q,textures,{},&error); require(bool(large),error.message.c_str());
        require(large!=small && large->inputCapacityWidth==128,"high-water-growth");
        const uint64_t smallActivation=small->activation, largeActivation=large->activation;
        q.inputContent={112,112}; auto back=ensureScaler(*exact,q,textures,{},&error);
        require(back==small && small->activation==smallActivation+1,"prefers-cached-exact-generation");
        back=ensureScaler(*exact,q,textures,{},&error);
        require(back==small && small->activation==smallActivation+1,"exact-reselection-keeps-activation");
        q.inputContent={120,120}; back=ensureScaler(*exact,q,textures,{},&error);
        require(back==large && large->activation==largeActivation+1,"fitting-high-water-fallback");
        std::printf("OWNED_METALFX_OPT_EXACT_EXTENT exact_preferred=1 fallback_high_water=1 activation_on_change_only=1\n");
    }
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
    auto depthBlit=runSR(gpu,core,a,6,6,publicFeature,MTLPixelFormatDepth32Float);
    parity &= compare(depthBlit,staged,"depth32-blit-fallback-cold");
    require(parity,"direct-staged-numeric-parity");
    {
        FsrPipelineNeeds needs; needs.linearize = needs.combineMasks = needs.padding = true;
        require(ensureFsrPipelines(*core,needs,&error) &&
                ensureFsrPipeline(*core,true,core->copyHalfPipeline,@"yaagl_sr_copy_pad_half") &&
                ensureFsrPipeline(*core,true,core->copyFloatPipeline,@"yaagl_sr_copy_pad_float"),
                "oracle-pipelines");
        unsigned checked = 0;
        for (metalfx::Rect active : {metalfx::Rect{3,5,96,72}, metalfx::Rect{0,0,127,1}, metalfx::Rect{1,0,1,128}, metalfx::Rect{2,2,120,120}})
            for (bool compute : {false, true}) {
                checked += paddingOracle(gpu,core,a,compute,ColorTransfer::Linear,MTLPixelFormatRGBA16Float,active);
                checked += paddingOracle(gpu,core,a,compute,ColorTransfer::Linear,MTLPixelFormatRGBA8Unorm,active);
                checked += paddingOracle(gpu,core,a,compute,ColorTransfer::SRGB,MTLPixelFormatRGBA8Unorm,active);
            }
        std::printf("OWNED_METALFX_OPT_PADDING_ORACLE textures=%u exact=1\n",checked);
    }
    {
        // Dormant drops cache ownership; a frame's own generation still executes.
        std::weak_ptr<ScalerGeneration> cached;
        for(const auto& entry:core->generations) if(entry && entry!=a) cached=entry;
        require(!cached.expired(),"cached-generation-before-dormant");
        core->markDormant();
        unsigned live=0; for(const auto& entry:core->generations) live+=bool(entry);
        require(live==0 && !core->currentGeneration && cached.expired(),"dormant-releases-cached-generations");
        auto afterDormant=runSR(gpu,core,a,7,8);
        require(compare(afterDormant,staged,"dormant-retained-generation-executes"),"dormant-retained-generation-output");
        std::printf("OWNED_METALFX_OPT_DORMANT cached_released=1 retained_generation_executed=1\n");
    }
    std::printf("OWNED_METALFX_OPT_CACHE sequence=A,B,A,C creations=3 reused=1 activation_reset=1\n");
    std::printf("OWNED_METALFX_OPT_EVICTION retained=%u additional_creations=9 evicted_generation_executed=1\n",retained);
    auto fgDirect=runFG(gpu,(id<MTL4Compiler>)core->compiler,false);
    auto fgStaged=runFG(gpu,(id<MTL4Compiler>)core->compiler,true);
    for(unsigned frame=0;frame<3;++frame) require(compare(fgDirect[frame],fgStaged[frame],"fg-offset-staging"),"fg-direct-staged-numeric-parity");
    [color release]; [depth release]; [motion release]; [output release];
    std::puts("OWNED_METALFX_OPT_PASS sr_offset_cases=6 fg_offset_cases=6 async_valid_outputs=1");
} }
