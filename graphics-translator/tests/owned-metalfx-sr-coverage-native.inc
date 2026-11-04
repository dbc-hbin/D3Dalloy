// Included in namespace probe; the fallback is selected by a failed optional
// view allocation, not by changing scaler dimensions, flags or input bytes.
using MakeTextureView = id<MTLTexture>(*)(id,SEL,MTLPixelFormat,MTLTextureType,NSRange,NSRange,MTLTextureSwizzleChannels);
using SetColorTexture = void(*)(id,SEL,id<MTLTexture>);
MakeTextureView originalView = nullptr;
SetColorTexture originalColor = nullptr;
bool refuseCoverageView = false;
unsigned coverageBindings = 0, extractBindings = 0;
bool alphaSwizzle(MTLTextureSwizzleChannels s) {
    return s.red==MTLTextureSwizzleAlpha && s.green==MTLTextureSwizzleAlpha &&
           s.blue==MTLTextureSwizzleAlpha && s.alpha==MTLTextureSwizzleOne;
}
id<MTLTexture> observeView(id t,SEL sel,MTLPixelFormat format,MTLTextureType type,
                          NSRange levels,NSRange slices,MTLTextureSwizzleChannels swizzle) {
    if (refuseCoverageView && alphaSwizzle(swizzle)) return nil;
    return originalView(t,sel,format,type,levels,slices,swizzle);
}
void observeCoverageBinding(id scaler,SEL sel,id<MTLTexture> texture) {
    if (texture && alphaSwizzle(texture.swizzle)) ++coverageBindings;
    if ([texture.label hasSuffix:@".YAAGL.Alpha.Input"]) ++extractBindings;
    originalColor(scaler,sel,texture);
}
void srAlphaViewProof(GPU& gpu) {
    Error error; auto core=srCore(gpu); core->create.featureFlags|=FeatureFlagAlphaUpscaling;
    unsigned pairs=0;
    for(unsigned mode=0;mode<5;++mode) {
        SRInputs in(gpu,128); [in.color release];
        in.color=texture(gpu,MTLPixelFormatRGBA16Float,128,MTLTextureUsageShaderRead |
            (mode==4 ? MTLTextureUsageUnknown : MTLTextureUsagePixelFormatView));
        in.color.label=@"sr-coverage-input"; auto info=in.info(); FrameOperations ops{};
        if(mode==1 || mode==3) {
            info.inputContent={120,120}; info.colorRect=info.depthRect=info.motionRect={3,4,120,120};
        }
        if(mode==2 || mode==3) ops.colorTransfer=mode==2 ? ColorTransfer::SRGB : ColorTransfer::PQ;
        auto gen=ensureScaler(*core,info,in.textures(),ops,&error); require(gen && gen->alphaScaler,"alpha-view-scaler");
        Class klass=object_getClass(gen->alphaScaler);
        Method resetMethod=class_getInstanceMethod(klass,@selector(setReset:));
        Method assignMethod=class_getInstanceMethod(klass,@selector(setColorTexture:));
        Method viewMethod=class_getInstanceMethod(object_getClass(in.color),
            @selector(newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:));
        require(resetMethod && assignMethod && viewMethod,"alpha-view-observer-methods");
        originalSRReset=(SetReset)method_setImplementation(resetMethod,(IMP)observeSRReset);
        originalColor=(SetColorTexture)method_setImplementation(assignMethod,(IMP)observeCoverageBinding);
        originalView=(MakeTextureView)method_setImplementation(viewMethod,(IMP)observeView);
        auto make=[&]{return Feature::create({(void*)gpu.device,(void*)core->compiler},core->create,&error);};
        auto reference=make(),candidate=make(); uint64_t refGeneration=0,candidateGeneration=0;
        require(reference && candidate,"alpha-view-features");
        for(unsigned phase=0;phase<5;++phase) {
            std::vector<_Float16> pixels(128*128*4);
            for(unsigned i=0;i<128*128;++i){
                pixels[i*4]=.6f; pixels[i*4+1]=.15f; pixels[i*4+2]=.03f;
                pixels[i*4+3]=float((i*31u+(i/128)*17+phase*7)%257)/256.f;
            }
            gpu.upload(in.color,pixels.data(),128*8); info.resetHistory=phase==3;
            info.jitterOffsetX=phase%2 ? .25f : -.25f; bool reset=phase==0 || phase==3;
            refuseCoverageView=true; coverageBindings=extractBindings=0;
            auto rf=reference->prepare(info,in.textures(),&error,ops); require(bool(rf),error.message.c_str());
            auto expected=executeSRPrepared(gpu,rf,in,refGeneration,reset,{},2);
            require(coverageBindings==0 && extractBindings==1,"alpha-view-reference-extract");
            refuseCoverageView=false; coverageBindings=extractBindings=0;
            auto cf=candidate->prepare(info,in.textures(),&error,ops); require(bool(cf),error.message.c_str());
            auto actual=executeSRPrepared(gpu,cf,in,candidateGeneration,reset,{},2);
            require(actual==expected,"alpha-view-exact-output");
            require(coverageBindings==(mode!=4) && extractBindings==(mode==4),"alpha-view-selection"); ++pairs;
        }
        method_setImplementation(viewMethod,(IMP)originalView);
        method_setImplementation(assignMethod,(IMP)originalColor);
        method_setImplementation(resetMethod,(IMP)originalSRReset);
    }
    std::printf("OWNED_METALFX_SR_ALPHA_VIEW_PASS pairs=%u components=%u direct=5 staged=5 transfer=10 fallback=5\n",
                pairs,pairs*256*256*4);
}
