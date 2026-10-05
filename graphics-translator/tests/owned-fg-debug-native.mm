// Focused production-backend exercise without Wine: the recording owner below
// retains native Metal objects until GPU completion, as the Unix transport does.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include "../../src/winemetal/unix/metalfx_frame_generation.hpp"
#include "../../src/winemetal/winemetal_metal4_fg.h"
#include "owned-fg-pattern.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct RecordingOwner { id<MTL4CommandBuffer> command; NSMutableArray* objects; };
extern "C" MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) { return (MTLPixelFormat)ORIGINAL_FORMAT(format); }
extern "C" id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t handle) { return reinterpret_cast<RecordingOwner*>(handle)->command; }
extern "C" void winemetal_metal4_retain_resource(obj_handle_t handle, id object) { [reinterpret_cast<RecordingOwner*>(handle)->objects addObject:object]; }

namespace {
using namespace dxmt::framegeneration;
void require(bool ok, const char* reason) {
    if (!ok) { std::fprintf(stderr, "OWNED_FG_DEBUG_NATIVE_FAIL reason=%s\n", reason); std::exit(1); }
}
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
    auto buffer = [device newBufferWithLength:128 * 128 * 8 options:MTLResourceStorageModeShared];
    auto command = [queue commandBuffer]; auto blit = [command blitCommandEncoder];
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(128,128,1) toBuffer:buffer destinationOffset:0 destinationBytesPerRow:128 * 8 destinationBytesPerImage:128 * 128 * 8];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted]; require(command.status == MTLCommandBufferStatusCompleted,"read");
    std::vector<uint16_t> pixels(128 * 128 * 4); std::memcpy(pixels.data(),buffer.contents,pixels.size()*2); [buffer release]; return pixels;
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
    std::vector<std::shared_ptr<const PreparedFrame>> record(uint64_t context, unsigned frame, id<MTLTexture> color,id<MTLTexture> depth,id<MTLTexture> motion,id<MTLTexture> output,unsigned flags) {
        WMTFGprepare_packet p{}; header(p,WMTFG_PREPARE,context); p.command_list=(uint64_t)&native; p.frame_id=frame;
        p.depth=(uint64_t)depth; p.motion_vectors=(uint64_t)motion; p.render_width=p.render_height=128;
        p.motion_scale_x=p.motion_scale_y=1; p.frame_time_delta_ms=33.3333f; p.camera_near=.1f; p.camera_far=100;
        p.camera_fov_vertical_radians=1; p.view_space_to_meters=1;
        std::shared_ptr<const PreparedFrame> prepare; require(api(WMTFG_PREPARE,&p,&prepare)==0,"prepare");
        WMTFGdispatch_packet g{}; header(g,WMTFG_DISPATCH,context); g.command_list=(uint64_t)&native; g.frame_id=frame;
        g.present_color=(uint64_t)color; g.output=(uint64_t)output; g.num_generated_frames=1; g.max_luminance=1; g.flags=flags;
        std::shared_ptr<const PreparedFrame> generate; require(api(WMTFG_DISPATCH,&g,&generate)==0,"generate"); return {prepare,generate};
    }
};
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
} }
