// Compare the actual prepared FSR mask path with two distinct, byte-identical
// masks, which forces the existing combine shader without changing its output.
void srMaskCropProof(GPU& gpu) {
    Error error;
    uint64_t components=0; unsigned pairs=0;
    for (const auto extent : {Extent{624,360},Extent{1920,1080}}) { @autoreleasepool {
        const uint32_t w=extent.width, h=extent.height, sw=w+16, sh=h+8;
        const size_t pixels=size_t(w)*h;
        auto core=srCore(gpu); core->create.input=core->create.output=extent;
        auto makeTex=[&](unsigned x,unsigned y,MTLPixelFormat fmt,MTLTextureUsage usage) {
            auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:x height:y mipmapped:NO];
            d.usage=usage; d.storageMode=MTLStorageModePrivate;
            auto t=[gpu.device newTextureWithDescriptor:d]; require(bool(t),"crop-proof-texture"); return t;
        };
        SRInputs in(gpu,128); [in.color release]; [in.depth release]; [in.motion release]; [in.output release];
        in.color=makeTex(w,h,MTLPixelFormatRGBA16Float,MTLTextureUsageShaderRead);
        in.depth=makeTex(w,h,MTLPixelFormatR32Float,MTLTextureUsageShaderRead);
        in.motion=makeTex(w,h,MTLPixelFormatRG16Float,MTLTextureUsageShaderRead);
        in.output=makeTex(w,h,MTLPixelFormatRGBA16Float,MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite);
        auto a=makeTex(sw,sh,MTLPixelFormatR8Unorm,MTLTextureUsageShaderRead);
        auto b=makeTex(sw,sh,MTLPixelFormatR8Unorm,MTLTextureUsageShaderRead);
        auto desc=[MTLResidencySetDescriptor new];
        auto resident=[gpu.device newResidencySetWithDescriptor:desc error:nil]; [desc release];
        [resident addAllocation:a]; [resident addAllocation:b]; [resident commit];
        [resident requestResidency]; [gpu.queue4 addResidencySet:resident];
        std::vector<_Float16> colors(pixels*4), motions(pixels*2,0);
        std::vector<float> depths(pixels,.65f); std::vector<uint8_t> masks(size_t(sw)*sh);
        for(unsigned y=0;y<sh;++y) for(unsigned x=0;x<sw;++x) masks[y*sw+x]=uint8_t(17*x+31*y);
        gpu.upload(in.depth,depths.data(),w*4); gpu.upload(in.motion,motions.data(),w*4);
        gpu.upload(a,masks.data(),sw); gpu.upload(b,masks.data(),sw);
        auto info=in.info(); info.inputContent=extent;
        info.colorRect=info.motionRect=info.depthRect=info.outputRect={0,0,w,h};
        info.compositionRect=info.reactiveRect={3,5,w,h};
        FrameOperations ops{}; ops.combineCompositionMask=true;
        auto t=in.textures(); t.reactive=(void*)b; t.composition=(void*)a;
        auto generation=ensureScaler(*core,info,t,ops,&error); require(bool(generation),error.message.c_str());
        auto m=class_getInstanceMethod(object_getClass(generation->scaler),@selector(setReset:));
        require(m,"crop-proof-reset-observer"); originalSRReset=(SetReset)method_setImplementation(m,(IMP)observeSRReset);
        auto make=[&]{return Feature::create({(void*)gpu.device,(void*)core->compiler},core->create,&error);};
        auto reference=make(), candidate=make(); require(reference && candidate,"crop-proof-features");
        uint64_t rg=0,cg=0;
        // Mode 0: composition alone. Mode 1: aliased masks. Mode 2: a
        // shorter composition rect that the combine backend has historically
        // accepted; only safe with its original full-input compute behavior.
        for(unsigned mode=0;mode<3;++mode) {
            for(size_t p=0;p<pixels;++p) {
                colors[p*4]=_Float16(((p+mode*11)%241)/255.f);
                colors[p*4+1]=_Float16(((p/w+mode*7)%199)/255.f);
                colors[p*4+2]=.125f; colors[p*4+3]=1;
            }
            gpu.upload(in.color,colors.data(),w*8);
            info.jitterOffsetX=mode==1 ?.25f:-.25f;
            info.resetHistory=mode==2; info.compositionRect.width=w-(mode==2 ? 1:0);
            t.reactive=(void*)b;
            observedInputs[0]=a; inputCopies[0]=0;
            auto rf=reference->prepare(info,t,&error,ops); require(bool(rf),error.message.c_str());
            auto expected=executeSRPrepared(gpu,rf,in,rg,mode!=1);
            require(inputCopies[0]==0,"reference-combines-both-masks");
            t.reactive=mode==1 ? (void*)a : nullptr;
            auto cf=candidate->prepare(info,t,&error,ops); require(bool(cf),error.message.c_str());
            auto actual=executeSRPrepared(gpu,cf,in,cg,mode!=1);
            require(expected==actual,"sr-mask-crop-exact-fp16-output");
            require(inputCopies[0]==(w==1920 && mode!=2 ? 1u:0u),"sr-mask-crop-selected-shape");
            observedInputs[0]=nil; ++pairs; components+=actual.size();
        }
        method_setImplementation(m,(IMP)originalSRReset);
        [gpu.queue4 removeResidencySet:resident]; [resident endResidency]; [resident release];
        [a release]; [b release];
    }}
    std::printf("OWNED_METALFX_SR_MASK_CROP_PASS pairs=%u components=%llu native_crops=2 small_fallback=3 short_rect_fallback=1\n",
                pairs,(unsigned long long)components);
}
