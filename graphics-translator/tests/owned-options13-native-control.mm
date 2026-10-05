// Independent Metal control for the native viewport/texel-copy contract.
// Run with MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1; no Wine or owned compiler.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstring>

int main(int argc, const char **argv) {
  @autoreleasepool {
    const char *mode = argc > 1 ? argv[1] : "normal";
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 2;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) return 2;
    const bool flip_y = !std::strcmp(mode, "flip-y");
    const bool flip_z = !std::strcmp(mode, "flip-z");
    const bool copy = !std::strcmp(mode, "copy");
    std::printf("NATIVE_OPTIONS13 device=%s mode=%s\n", device.name.UTF8String, mode);
    std::fflush(stdout);
    if (copy) {
      unsigned char input[64], output[64];
      for (unsigned i = 0; i < sizeof(input); ++i) input[i] = i + 1;
      std::memset(output, 0xcc, sizeof(output));
      id<MTLBuffer> src = [device newBufferWithBytes:input length:sizeof(input) options:MTLResourceStorageModeShared];
      id<MTLBuffer> dst = [device newBufferWithBytes:output length:sizeof(output) options:MTLResourceStorageModeShared];
      auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint
                                                                   width:3 height:2 mipmapped:NO];
      desc.storageMode = MTLStorageModePrivate;
      id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
      [blit copyFromBuffer:src sourceOffset:1 sourceBytesPerRow:5 sourceBytesPerImage:0
               sourceSize:MTLSizeMake(3, 2, 1) toTexture:texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(3, 2, 1) toBuffer:dst destinationOffset:3
         destinationBytesPerRow:7 destinationBytesPerImage:0];
      [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
      if (cb.status != MTLCommandBufferStatusCompleted) {
        std::fprintf(stderr, "NATIVE_OPTIONS13_FAIL %s\n", cb.error.description.UTF8String); return 1;
      }
      for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 3; ++x) output[3 + y * 7 + x] = input[1 + y * 5 + x];
      if (std::memcmp(dst.contents, output, sizeof(output))) return 1;
    } else {
      NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 t=float2((id<<1)&2,id&2); "
        "return {float4(t*float2(2,-2)+float2(-1,1),.25,1),t}; }\n"
        "fragment float4 ps(V i [[stage_in]]) { return float4(i.uv,i.p.z,1); }\n";
      NSError *error = nil;
      id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
      if (!library) { std::fprintf(stderr, "%s\n", error.description.UTF8String); return 2; }
      auto pipeline_desc = [MTLRenderPipelineDescriptor new];
      pipeline_desc.vertexFunction = [library newFunctionWithName:@"vs"];
      pipeline_desc.fragmentFunction = [library newFunctionWithName:@"ps"];
      pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
      id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
      if (!pipeline) { std::fprintf(stderr, "%s\n", error.description.UTF8String); return 2; }
      auto texture_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                           width:8 height:8 mipmapped:NO];
      texture_desc.storageMode = MTLStorageModeShared;
      texture_desc.usage = MTLTextureUsageRenderTarget;
      id<MTLTexture> texture = [device newTextureWithDescriptor:texture_desc];
      auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
      [encoder setRenderPipelineState:pipeline];
      [encoder setViewport:MTLViewport{0, flip_y ? 8.0 : 0.0, 8, flip_y ? -8.0 : 8.0,
                                      flip_z ? 1.0 : 0.0, flip_z ? 0.0 : 1.0}];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
      if (cb.status != MTLCommandBufferStatusCompleted) {
        std::fprintf(stderr, "NATIVE_OPTIONS13_FAIL %s\n", cb.error.description.UTF8String); return 1;
      }
      float pixels[8 * 8 * 4];
      [texture getBytes:pixels bytesPerRow:8 * 16 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
      for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
        const float expected[] = {(x + .5f) / 8, flip_y ? 1 - (y + .5f) / 8 : (y + .5f) / 8,
                                  flip_z ? .75f : .25f, 1};
        for (unsigned c = 0; c < 4; ++c) if (std::fabs(pixels[(y * 8 + x) * 4 + c] - expected[c]) > .00001f) {
          std::fprintf(stderr, "NATIVE_OPTIONS13_FAIL x=%u y=%u c=%u got=%g expected=%g\n",
                       x, y, c, pixels[(y * 8 + x) * 4 + c], expected[c]); return 1;
        }
      }
    }
    std::printf("NATIVE_OPTIONS13_PASS mode=%s\n", mode);
    return 0;
  }
}
