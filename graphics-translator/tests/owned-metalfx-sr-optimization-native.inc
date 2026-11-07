// Included inside namespace probe by owned-metalfx-optimization-native.mm.
// Focused exact-extent generation and accepted-encode history oracle.

std::shared_ptr<Feature::Impl> srCore(GPU& gpu) {
    auto core = std::make_shared<Feature::Impl>(); core->device = retainObject(gpu.device);
    auto descriptor = [MTL4CompilerDescriptor new];
    core->compiler = [gpu.device newCompilerWithDescriptor:descriptor error:nil]; [descriptor release];
    require(core->compiler != nil, "sr-proof-compiler");
    core->create = {{128,128},{256,256},FeatureFlagMVLowRes|FeatureFlagIsHDR,true,true};
    core->minScale = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:gpu.device];
    core->maxScale = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:gpu.device];
    return core;
}

using SetReset = void (*)(id, SEL, BOOL);
SetReset originalSRReset = nullptr;
std::vector<bool> srResetValues;
unsigned srEncodedResetAssignments=0;
void observeSRReset(id scaler, SEL selector, BOOL value) {
    srResetValues.push_back(value != NO); originalSRReset(scaler, selector, value);
}

struct SRInputs {
    GPU& gpu; unsigned size;
    id<MTLTexture> color, depth, motion, output;
    SRInputs(GPU& gpu, unsigned size) : gpu(gpu), size(size),
        color(texture(gpu,MTLPixelFormatRGBA16Float,size,MTLTextureUsageShaderRead)),
        depth(texture(gpu,MTLPixelFormatR32Float,size,MTLTextureUsageShaderRead)),
        motion(texture(gpu,MTLPixelFormatRG16Float,size,MTLTextureUsageShaderRead)),
        output(texture(gpu,MTLPixelFormatRGBA16Float,256,MTLTextureUsageShaderWrite)) { upload(0); }
    ~SRInputs() { [output release]; [motion release]; [depth release]; [color release]; }
    void upload(unsigned phase) {
        std::vector<_Float16> colors(size*size*4), motions(size*size*2, _Float16(0));
        std::vector<float> depths(size*size,.75f);
        for (unsigned y=0;y<size;++y) for (unsigned x=0;x<size;++x) {
            const size_t p=(y*size+x)*4;
            colors[p]=_Float16(.125f+.5f*float(x)/float(size-1)+.015625f*phase);
            colors[p+1]=_Float16(.25f+.25f*float(y)/float(size-1));
            colors[p+2]=_Float16(.375f+.03125f*phase); colors[p+3]=_Float16(1);
        }
        gpu.upload(color,colors.data(),size*8); gpu.upload(depth,depths.data(),size*4);
        gpu.upload(motion,motions.data(),size*4);
    }
    TextureSet textures() const { return {(void*)color,(void*)depth,(void*)motion,(void*)output}; }
    FrameInfo info() const {
        FrameInfo p{}; p.inputContent={size,size};
        p.colorRect=p.depthRect=p.motionRect={0,0,size,size}; p.outputRect={0,0,256,256};
        return p;
    }
};

std::vector<float> executeSRPrepared(GPU& gpu, const std::shared_ptr<const PreparedFrame>& prepared,
                                    const SRInputs& input, uint64_t& lastGeneration, bool expectedReset,
                                    const std::function<void()>& beforeRetirement={}, unsigned scalerCount=1) {
    auto allocator=[gpu.device newCommandAllocator]; auto fence=[gpu.device newFence];
    auto descriptor=[MTLResidencySetDescriptor new];
    auto residency=[gpu.device newResidencySetWithDescriptor:descriptor error:nil]; [descriptor release];
    require(allocator && fence && residency,"sr-proof-native-resources");
    for (id resource : {input.color,input.depth,input.motion,input.output}) [residency addAllocation:resource];
    [residency commit]; [residency requestResidency];
    std::vector<id<MTL4CommandBuffer>> commands;
    auto first=[gpu.device newCommandBuffer]; [first beginCommandBufferWithAllocator:allocator];
    [first useResidencySet:residency]; commands.push_back(first);
    struct Segments { GPU* gpu; id<MTL4CommandAllocator> allocator; id<MTLResidencySet> residency;
                      std::vector<id<MTL4CommandBuffer>>* commands; } segments{&gpu,allocator,residency,&commands};
    Error error; std::shared_ptr<const ExecutionLease> lease;
    const size_t before=srResetValues.size();
    // Exercise native accepted-encode ordering independently of the transport's
    // redundant generation-switch forceReset (all caller reset flags are false).
    const uint64_t previousGeneration=lastGeneration;
    require(prepared->encode((void*)first,(void*)fence,lease,&error,false,&segments,
        [](void* opaque) -> void* {
            auto& s=*static_cast<Segments*>(opaque); [s.commands->back() endCommandBuffer];
            auto command=[s.gpu->device newCommandBuffer]; [command beginCommandBufferWithAllocator:s.allocator];
            [command useResidencySet:s.residency]; s.commands->push_back(command); return (void*)command;
        }),error.message.c_str());
    require(srResetValues.size()==before+scalerCount &&
            std::all_of(srResetValues.begin()+before,srResetValues.end(),
                        [&](bool value){return value==expectedReset;}),"sr-exact-reset-sequence");
    srEncodedResetAssignments+=scalerCount;
    std::printf("OWNED_METALFX_SR_ENCODE generation=%llu previous_generation=%llu reset=%u force_reset=0 pending_eviction=%u\n",
        (unsigned long long)prepared->generationId(),(unsigned long long)previousGeneration,
        unsigned(expectedReset),unsigned(bool(beforeRetirement)));
    lastGeneration=prepared->generationId(); [commands.back() endCommandBuffer];
    if (beforeRetirement) {
        // Fixture-only queue gate: prove cache eviction while the submitted
        // command and its execution lease are still pending GPU completion.
        auto gate=[gpu.device newSharedEvent]; require(gate!=nil,"sr-pending-retirement-gate");
        [gpu.queue4 waitForEvent:gate value:1];
        [gpu.queue4 commit:commands.data() count:commands.size()];
        [gpu.queue4 signalEvent:gpu.completed value:++gpu.event];
        beforeRetirement();
        require(gpu.completed.signaledValue<gpu.event,"sr-eviction-before-gpu-retirement");
        gate.signaledValue=1;
        require([gpu.completed waitUntilSignaledValue:gpu.event timeoutMS:15000],"sr-pending-retirement-completion");
        [gate release];
    } else gpu.commit(commands);
    auto result=gpu.read(input.output);
    for (auto command : commands) [command release];
    [residency endResidency]; [residency release]; [fence release]; [allocator release];
    return result;
}

void srExtentProof(GPU& gpu) {
    auto core=srCore(gpu); SRInputs large(gpu,128), small(gpu,112), referenceLarge(gpu,128), referenceSmall(gpu,112);
    Error error; FrameInfo p=large.info();
    auto a=ensureScaler(*core,p,large.textures(),{},&error); require(bool(a),error.message.c_str());
    p=small.info(); auto b=ensureScaler(*core,p,small.textures(),{},&error); require(bool(b),error.message.c_str());
    require(a!=b && b->inputCapacityWidth==112 && b->inputCapacityHeight==112,"uncached-shrink-exact-generation");
    require(exactDescriptorInput(small.color,p.colorRect,112,112,112,112,b->colorUsage) &&
            exactDescriptorInput(small.depth,p.depthRect,112,112,112,112,b->depthUsage) &&
            exactDescriptorInput(small.motion,p.motionRect,112,112,112,112,b->motionUsage),"exact-shrink-direct-inputs");
    require(ensureScaler(*core,p,small.textures(),{},&error)==b && core->lastEncodedGeneration==0,"steady-extent-no-activation");
    p.resetHistory=true;
    require(ensureScaler(*core,p,small.textures(),{},&error)==b && core->lastEncodedGeneration==0,"caller-reset-keeps-exact-generation");
    for (unsigned width=113;width<=121;++width) {
        p=large.info(); p.inputContent={width,width};
        auto next=ensureScaler(*core,p,large.textures(),{},&error); require(bool(next),error.message.c_str());
        require(next->inputCapacityWidth==width && next->inputCapacityHeight==width,"each-transition-exact-capacity");
    }
    unsigned cached=0; bool oldCached=false;
    for (const auto& generation : core->generations) { cached+=bool(generation); oldCached|=generation==b; }
    require(cached==8 && !oldCached && b->scaler!=nil,"eight-generation-bound-retains-old-owner");
    auto method=class_getInstanceMethod(object_getClass(a->scaler),@selector(setReset:));
    require(method!=nullptr,"sr-native-reset-observer");
    originalSRReset=reinterpret_cast<SetReset>(method_setImplementation(method,reinterpret_cast<IMP>(observeSRReset)));
    srResetValues.clear(); srEncodedResetAssignments=0;
    const auto create=[&] { auto feature=Feature::create({(void*)gpu.device,(void*)core->compiler},core->create,&error);
        require(bool(feature),error.message.c_str()); return feature; };
    const auto prepare=[&](const std::shared_ptr<Feature>& feature,const SRInputs& inputs) {
        auto result=feature->prepare(inputs.info(),inputs.textures(),&error); require(bool(result),error.message.c_str()); return result; };
    auto feature=create(), reference=create();
    auto largeFrame=prepare(feature,large), referenceFrame=prepare(reference,referenceLarge);
    uint64_t executed=0, referenceExecuted=0; unsigned comparisons=0;
    const auto check=[&](const std::shared_ptr<const PreparedFrame>& frame,const SRInputs& input,bool reset,
                         const SRInputs& referenceInput,const char* name,const std::function<void()>& beforeRetirement={}) {
        auto actual=executeSRPrepared(gpu,frame,input,executed,reset,beforeRetirement);
        auto expected=executeSRPrepared(gpu,referenceFrame,referenceInput,referenceExecuted,reset);
        require(compare(actual,expected,name),"sr-fresh-descriptor-history-parity"); ++comparisons;
    };
    check(largeFrame,large,true,referenceLarge,"extent-initial");
    auto smallFrame=prepare(feature,small);
    require(smallFrame->generationId()!=largeFrame->generationId(),"public-uncached-shrink-generation");
    auto freshLargeFrame=prepare(feature,large);
    require(freshLargeFrame->generationId()==largeFrame->generationId(),"prepare-only-return-cached-generation");
    large.upload(1); referenceLarge.upload(1);
    check(freshLargeFrame,large,false,referenceLarge,"extent-prepare-only-B-fresh-A");
    large.upload(2); referenceLarge.upload(2);
    check(largeFrame,large,false,referenceLarge,"extent-prepare-only-B-old-A-replay");
    small.upload(1); referenceSmall.upload(1);
    reference=create(); referenceExecuted=0; referenceFrame=prepare(reference,referenceSmall);
    check(smallFrame,small,true,referenceSmall,"extent-uncached-shrink");
    auto warmFrame=prepare(feature,small);
    require(warmFrame->generationId()==smallFrame->generationId(),"public-stable-generation");
    small.upload(2); referenceSmall.upload(2);
    check(warmFrame,small,false,referenceSmall,"extent-warm-history");
    auto callerResetInfo=small.info(); callerResetInfo.resetHistory=true;
    auto callerResetFrame=feature->prepare(callerResetInfo,small.textures(),&error);
    auto referenceResetInfo=referenceSmall.info(); referenceResetInfo.resetHistory=true;
    auto referenceWarmFrame=referenceFrame;
    referenceFrame=reference->prepare(referenceResetInfo,referenceSmall.textures(),&error);
    require(callerResetFrame && referenceFrame && callerResetFrame->generationId()==smallFrame->generationId(),
            "caller-reset-without-generation-switch");
    check(callerResetFrame,small,true,referenceSmall,"extent-explicit-caller-reset");
    referenceFrame=referenceWarmFrame;
    small.upload(3); referenceSmall.upload(3);
    check(smallFrame,small,false,referenceSmall,"extent-closed-recording-replay");
    large.upload(4); referenceLarge.upload(4);
    reference=create(); referenceExecuted=0; referenceFrame=prepare(reference,referenceLarge);
    check(largeFrame,large,true,referenceLarge,"extent-late-large-prepared");
    small.upload(5); referenceSmall.upload(5);
    reference=create(); referenceExecuted=0; referenceFrame=prepare(reference,referenceSmall);
    check(smallFrame,small,true,referenceSmall,"extent-return-old-small");
    small.upload(6); referenceSmall.upload(6);
    check(smallFrame,small,false,referenceSmall,"extent-pending-lease-cache-eviction",[&] {
        // Nine exact descriptors evict B from the eight-entry cache, while its
        // submitted frame/lease still own the scaler and scratch allocations.
        for (unsigned width=113;width<=121;++width) { SRInputs other(gpu,width); (void)prepare(feature,other); }
    });
    feature->markDormant(); small.upload(7); referenceSmall.upload(7);
    check(smallFrame,small,false,referenceSmall,"extent-dormant-prepared-replay");
    method_setImplementation(method,reinterpret_cast<IMP>(originalSRReset));
    require(comparisons==11 && srEncodedResetAssignments==22,"sr-extent-case-count");
    std::printf("OWNED_METALFX_SR_EXTENT_PASS image_pairs=%u reset_calls=%u exact_shrink=1 prepare_only_fresh_no_reset=1 prepare_only_old_no_reset=1 actual_generation_switch_reset=1 caller_reset=1 stable_history=1 late_replay=1 pending_retirement_eviction=1 dormant_replay=1 cache_bound=%u\n",
        comparisons,srEncodedResetAssignments,cached);
}
