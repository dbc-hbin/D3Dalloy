// SPDX-License-Identifier: LGPL-2.1-or-later
// Same driver against two frozen backends. Hash the full caller texture, including margins/guards.
#define main sr_existing_fixture_main
#include "owned-metalfx-optimization-native.mm"
#undef main
#include <CommonCrypto/CommonDigest.h>

namespace {
using namespace probe;
id<MTLTexture> makeTex(GPU& gpu, MTLPixelFormat format, unsigned w, unsigned h, MTLTextureUsage usage) {
    auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModePrivate; d.usage=usage;
    auto texture=[gpu.device newTextureWithDescriptor:d]; require(texture!=nil,"scene texture"); return texture;
}
struct Segments {
    id<MTLDevice> device;
    id<MTL4CommandAllocator> allocator;
    id<MTLResidencySet> residency;
    std::vector<id<MTL4CommandBuffer>> commands;
    id<MTL4CommandBuffer> next() {
        if (!commands.empty()) [commands.back() endCommandBuffer];
        auto cmd=[device newCommandBuffer]; [cmd beginCommandBufferWithAllocator:allocator];
        [cmd useResidencySet:residency]; commands.push_back(cmd); return cmd;
    }
    ~Segments() { for (auto cmd:commands) [cmd release]; }
};
void scene(GPU& gpu,id<MTL4Compiler> compiler,const char* name,uint32_t iw,uint32_t ih,uint32_t ow,uint32_t oh,
           MTLPixelFormat format,bool capped,bool sharpen,bool padded,unsigned frames) {
    const unsigned x0=capped?3:0,y0=capped?5:0;
    const uint32_t bw=((ow+x0+63)/64)*64, bh=oh+y0+(capped?3:0);
    const uint32_t dx=padded?4:0,dy=padded?2:0,tw=iw+(padded?16:0),th=ih+(padded?8:0);
    CreateInfo info{{iw,ih},{ow,oh},74,true,true};
    Error error;
    auto f=Feature::create({(void*)gpu.device,(void*)compiler},info,&error);require(bool(f),error.message.c_str());
    auto color=makeTex(gpu,MTLPixelFormatRGBA8Unorm,tw,th,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    auto depth=makeTex(gpu,MTLPixelFormatDepth32Float,tw,th,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    auto mv=makeTex(gpu,MTLPixelFormatRG16Float,tw,th,MTLTextureUsageShaderRead);
    auto out=makeTex(gpu,format,bw,bh,MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget);
    const uint32_t row=((tw+63)/64)*64;
    std::vector<uint32_t> rgba(row*th,0xefe1d377u),motion(row*th,0);
    std::vector<float> z(row*th,0.125f);
    for(unsigned y=0;y<ih;++y) for(unsigned x=0;x<iw;++x) {
        const unsigned r=(x*7+y*3)&255,g=(x/3+y*13)&255,b=(x/11+y/5)&255;
        rgba[(y+dy)*row+x+dx]=r|(g<<8)|(b<<16)|0xff000000u;
        z[(y+dy)*row+x+dx]=((x/13+y/11)&1)?0.75f:0.375f;
    }
    std::vector<uint32_t> sentinel(bw*bh,0x7359bd37u);
    gpu.upload(color,rgba.data(),row*4);gpu.upload(depth,z.data(),row*4);
    gpu.upload(mv,motion.data(),row*4);gpu.upload(out,sentinel.data(),bw*4);
    id<MTLFence> fence=[gpu.device newFence];
    auto desc=[MTLResidencySetDescriptor new];
    auto residency=[gpu.device newResidencySetWithDescriptor:desc error:nil];[desc release];
    for(id t:{color,depth,mv,out}) [residency addAllocation:t];
    [residency commit];[residency requestResidency];
    const auto limit=[MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:gpu.device];
    const double scale=std::min({double(limit),double(ow)/iw,double(oh)/ih});
    const unsigned cw=capped?unsigned(std::floor(iw*scale)):ow,ch=capped?unsigned(std::floor(ih*scale)):oh;
    if(capped) require(cw<ow||ch<oh,"expected cap");
    const unsigned cx=x0+(ow-cw)/2,cy=y0+(oh-ch)/2;
    auto counters=[MTL4CounterHeapDescriptor new];counters.type=MTL4CounterHeapTypeTimestamp;counters.count=2;
    auto interval=[gpu.device newCounterHeapWithDescriptor:counters error:nil];[counters release];
    const uint64_t frequency=[gpu.device queryTimestampFrequency];
    require(interval && frequency && [gpu.device sizeOfCounterHeapEntry:MTL4CounterHeapTypeTimestamp]==sizeof(MTL4TimestampHeapEntry),"timestamp counter support");
    for(unsigned k=0;k<frames;++k) @autoreleasepool {
        FrameInfo p{};
        p.inputContent={iw,ih};p.colorRect=p.depthRect=p.motionRect={dx,dy,iw,ih};p.outputRect={x0,y0,ow,oh};
        p.preExposure=1;p.exposureMode=ExposureMode::Automatic;p.resetHistory=k==0||k==3;
        p.jitterOffsetX=(k&1)?.25f:-.25f;p.jitterOffsetY=(k&1)?-.125f:.125f;
        FrameOperations ops;ops.capOutputToTemporalMaxScale=capped;ops.sharpening=sharpen;ops.sharpness=.5f;
        auto frame=f->prepare(p,{color,depth,mv,out},&error,ops);require(bool(frame),error.message.c_str());
        id<MTL4CommandAllocator> alloc=[gpu.device newCommandAllocator];
        Segments seg{gpu.device,alloc,residency,{}};
        std::shared_ptr<const ExecutionLease> lease;
        [interval invalidateCounterRange:NSMakeRange(0,2)];
        auto first=seg.next();[first writeTimestampIntoHeap:interval atIndex:0];
        require(frame->encode(first,fence,lease,&error,false,&seg,[](void* s)->void* {
            return static_cast<Segments*>(s)->next();
        }),error.message.c_str());
        [seg.commands.back() writeTimestampIntoHeap:interval atIndex:1];
        [seg.commands.back() endCommandBuffer];
        auto done=dispatch_semaphore_create(0);__block bool ok=false;
        auto opts=[MTL4CommitOptions new];[opts addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
            ok=!fb.error;dispatch_semaphore_signal(done);
        }];
        [gpu.queue4 commit:seg.commands.data() count:seg.commands.size() options:opts];[opts release];
        [gpu.queue4 signalEvent:gpu.completed value:++gpu.event];
        require([gpu.completed waitUntilSignaledValue:gpu.event timeoutMS:15000],"scene GPU completion");
        require(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC))==0&&ok,"scene GPU feedback");
        dispatch_release(done);lease.reset();
        // Split-list Metal4 feedback can report a zero GPUStartTime. Measure the
        // same submitted command sequence with an explicit counter at each end.
        auto samples=[interval resolveCounterRange:NSMakeRange(0,2)];
        MTL4TimestampHeapEntry ticks[2]{};
        require(samples.length==sizeof(ticks),"timestamp pair");std::memcpy(ticks,samples.bytes,sizeof(ticks));
        require(ticks[0].timestamp && ticks[1].timestamp>ticks[0].timestamp,"valid GPU interval");
        const double us=double(ticks[1].timestamp-ticks[0].timestamp)*1e6/double(frequency);
        auto bytes=readRaw(gpu,out,4);
        unsigned guard=0,black=0,content=0;const uint32_t blackCode=format==MTLPixelFormatRGB10A2Unorm?0xc0000000u:0xff000000u;
        for(unsigned y=0;y<bh;++y) for(unsigned x=0;x<bw;++x) {
            uint32_t value;std::memcpy(&value,&bytes[4*(size_t(y)*bw+x)],4);
            if(x<x0||y<y0||x>=x0+ow||y>=y0+oh){require(value==0x7359bd37u,"outer guard");++guard;}
            else if(x<cx||y<cy||x>=cx+cw||y>=cy+ch){require(value==blackCode,"opaque black margin");++black;}
            else content+=value!=blackCode&&value!=0x7359bd37u;
        }
        require(content>cw*ch/4,"nontrivial center");
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.data(),CC_LONG(bytes.size()),digest);
        char hex[65];for(unsigned i=0;i<32;++i) std::snprintf(hex+2*i,3,"%02x",digest[i]);
        std::printf("SR_SCENE case=%s frame=%u gpu_us=%.3f width=%u height=%u center_pixels=%u margin_pixels=%u guard_pixels=%u bytes=%zu sha=%s\n",
            name,k,us,ow,oh,cw*ch,black,guard,bytes.size(),hex);
        [alloc release];
    }
    [interval release];[residency endResidency];[residency release];[fence release];
    for(id t:{color,depth,mv,out}) [t release];
}
}
int main(int argc,char** argv) { @autoreleasepool {
    const char* selected=argc>1?argv[1]:"all";unsigned frames=argc>2?unsigned(std::atoi(argv[2])):4;
    probe::require(frames>=1&&frames<=32,"frame count");
    GPU gpu;auto cd=[MTL4CompilerDescriptor new];auto compiler=[gpu.device newCompilerWithDescriptor:cd error:nil];[cd release];
    auto run=[&](const char* n,unsigned w,unsigned h,unsigned ow,unsigned oh,MTLPixelFormat fmt,bool cap,bool sharp,bool pad){
        if(!std::strcmp(selected,"all")||!std::strcmp(selected,n))scene(gpu,compiler,n,w,h,ow,oh,fmt,cap,sharp,pad,frames);
    };
    run("city-contract",624,360,1280,720,MTLPixelFormatRGBA8Unorm,false,false,false);
    run("fsr-1080",960,540,1920,1080,MTLPixelFormatRGBA8Unorm,false,true,true);
    run("fsr-4k",1920,1080,3840,2160,MTLPixelFormatRGBA8Unorm,false,true,false);
    run("cap-1440",640,360,2560,1440,MTLPixelFormatRGBA8Unorm,true,false,false);
    run("cap-bgra",65,67,263,255,MTLPixelFormatBGRA8Unorm,true,false,true);
    run("cap-rgb10",83,65,313,271,MTLPixelFormatRGB10A2Unorm,true,false,false);
    [compiler release];return 0;
} }
