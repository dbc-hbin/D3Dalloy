// Independent public MetalFX control. No adapter kernels, FFX code, Wine or
// production implementation are linked. Constant normalized depth/MV inputs
// make the expected transport mapping unambiguous.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include "owned-fg-pattern.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void require(bool ok, const char* reason) {
    if (!ok) { fprintf(stderr, "OWNED_D3D12_FSR_FG_NATIVE_FAIL reason=%s\n", reason); exit(1); }
}
id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, MTLTextureUsage usage) {
    auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:128 height:128 mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate; desc.usage = usage;
    id<MTLTexture> result = [device newTextureWithDescriptor:desc]; require(result != nil, "texture"); return result;
}
void upload(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture, const void* data, size_t row) {
    id<MTLBuffer> source = [device newBufferWithBytes:data length:row * 128 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * 128
             sourceSize:MTLSizeMake(128, 128, 1) toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "upload-completed"); [source release];
}
}
int main(int argc, char** argv) {
    require(argc == 2, "usage-output-directory");
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device && [MTLFXFrameInterpolatorDescriptor supportsMetal4FX:device], "native-supported");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTL4CommandQueue> queue4 = [device newMTL4CommandQueue];
        NSError* error = nil;
        auto* compilerDesc = [MTL4CompilerDescriptor new];
        id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:compilerDesc error:&error];
        require(compiler && queue4, "native-metal4-compiler-queue");
        id<MTLSharedEvent> completed = [device newSharedEvent];
        auto* desc = [MTLFXFrameInterpolatorDescriptor new];
        desc.colorTextureFormat = desc.outputTextureFormat = MTLPixelFormatRGBA16Float;
        desc.depthTextureFormat = MTLPixelFormatR32Float; desc.motionTextureFormat = MTLPixelFormatRG16Float;
        desc.uiTextureFormat = MTLPixelFormatRGBA16Float;
        desc.inputWidth = desc.inputHeight = desc.outputWidth = desc.outputHeight = 128;
        desc.requiresPrevColorTexture = YES;
        id<MTL4FXFrameInterpolator> effect = [desc newFrameInterpolatorWithDevice:device compiler:compiler];
        require(effect != nil, "public-native-effect");
        id<MTLTexture> previous = nil;
        unsigned interpolated = 0;
        for (unsigned frame = 1; frame <= 8; ++frame) {
            auto color = texture(device, MTLPixelFormatRGBA16Float, effect.colorTextureUsage);
            auto depth = texture(device, MTLPixelFormatR32Float, effect.depthTextureUsage);
            auto motion = texture(device, MTLPixelFormatRG16Float, effect.motionTextureUsage);
            auto output = texture(device, MTLPixelFormatRGBA16Float, effect.outputTextureUsage);
            auto scene = fgScene(frame);
            std::vector<float> depths(128 * 128, .75f);
            std::vector<uint16_t> motions(128 * 128 * 2, 0);
            for (unsigned i = 0; i < motions.size(); i += 2) motions[i] = 0x4400; // +4 display pixels (backward pan flow).
            upload(device, queue, color, scene.data(), 128 * 8);
            upload(device, queue, depth, depths.data(), 128 * 4);
            upload(device, queue, motion, motions.data(), 128 * 4);
            effect.colorTexture = color; effect.prevColorTexture = previous ?: color;
            effect.depthTexture = depth; effect.motionTexture = motion; effect.outputTexture = output;
            effect.motionVectorScaleX = effect.motionVectorScaleY = 1;
            effect.deltaTime = 1.f / 30.f; effect.nearPlane = .1f; effect.farPlane = 100.f;
            effect.fieldOfView = 57.295779513082320876f; effect.aspectRatio = 1;
            effect.depthReversed = YES; effect.shouldResetHistory = frame == 1;
            effect.contentWidth = effect.contentHeight = 128;
            id<MTLBuffer> readback = [device newBufferWithLength:128 * 128 * 8 options:MTLResourceStorageModeShared];
            auto* residencyDesc = [MTLResidencySetDescriptor new];
            id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:residencyDesc error:&error];
            id<MTLAllocation> inputs[] = {color, previous ?: color, depth, motion, output};
            [residency addAllocations:inputs count:5]; [residency commit];
            id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
            id<MTL4CommandBuffer> command4 = [device newCommandBuffer];
            [command4 beginCommandBufferWithAllocator:allocator]; [command4 useResidencySet:residency];
            [effect encodeToCommandBuffer:command4]; [command4 endCommandBuffer];
            [queue4 commit:&command4 count:1]; [queue4 signalEvent:completed value:frame];
            require([completed waitUntilSignaledValue:frame timeoutMS:15000], "native-metal4-gpu-completion");
            id<MTLCommandBuffer> command = [queue commandBuffer];
            auto blit = [command blitCommandEncoder];
            [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(128, 128, 1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:128 * 8 destinationBytesPerImage:128 * 128 * 8];
            [blit endEncoding]; [command commit]; [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted, "native-gpu-completion");
            const bool changed = memcmp(readback.contents, scene.data(), 128 * 128 * 8) != 0;
            if (frame > 1 && changed) ++interpolated;
            printf("OWNED_D3D12_FSR_FG_NATIVE_HISTORY frame=%u differs_from_current=%u\n", frame, changed);
            char path[4096]; snprintf(path, sizeof(path), "%s/native-frame-%u.bin", argv[1], frame);
            FILE* file = fopen(path, "wb"); require(file != nullptr, "native-output-open");
            require(fwrite(readback.contents, 128 * 128 * 8, 1, file) == 1, "native-output-write"); fclose(file);
            printf("OWNED_D3D12_FSR_FG_NATIVE_FRAME frame=%u bytes=131072 source=public-metalfx-gpu path=%s\n", frame, path);
            [previous release]; previous = color;
            [depth release]; [motion release]; [output release]; [readback release];
            [allocator release]; [command4 release]; [residency release]; [residencyDesc release];
        }
        require(interpolated != 0, "native-generated-is-not-current-copy");
        [previous release]; [effect release]; [desc release]; [queue release]; [device release];
        [queue4 release]; [compiler release]; [compilerDesc release]; [completed release];
        puts("OWNED_D3D12_FSR_FG_NATIVE_PASS frames=8");
    }
}
