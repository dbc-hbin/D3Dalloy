// Compare public NGX feature 9 readback with a standalone native spatial scaler.
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstdio>
#include <cstring>
#include <vector>

static NSData *image(NSString *directory, NSString *name, size_t bytes) {
  NSData *data = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:name]];
  return data.length == bytes ? data : nil;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc != 2) return 2;
    auto directory = [NSString stringWithUTF8String:argv[1]];
    auto device = MTLCreateSystemDefaultDevice();
    auto blitQueue = [device newCommandQueue];
    auto desc = [MTLFXSpatialScalerDescriptor new];
    constexpr unsigned width = 96, height = 80;
    desc.inputWidth = desc.outputWidth = width;
    desc.inputHeight = desc.outputHeight = height;
    desc.colorTextureFormat = desc.outputTextureFormat = MTLPixelFormatRGBA8Unorm;
    desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
    auto scaler = [desc newSpatialScalerWithDevice:device];
    if (!scaler || !blitQueue) return 2;
    auto td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:width height:height mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = scaler.colorTextureUsage;
    auto input = [device newTextureWithDescriptor:td];
    td.usage = scaler.outputTextureUsage;
    auto output = [device newTextureWithDescriptor:td];
    for (unsigned atlas = 0; atlas < 2; ++atlas) {
      const unsigned x = atlas ? 5 : 0, y = atlas ? 3 : 0, stride = width + 2 * x;
      const size_t bytes = stride * (height + 2 * y) * 4;
      auto source = image(directory, atlas ? @"isp-input-atlas.bin" : @"isp-input-direct.bin", bytes);
      auto actual = image(directory, atlas ? @"isp-output-atlas.bin" : @"isp-output-direct.bin", bytes);
      if (!source || !actual) return 2;
      const unsigned w = atlas ? 48 : 64, h = atlas ? 40 : 48;
      constexpr unsigned pitch = 512;
      auto buffer = [device newBufferWithLength:pitch * height options:MTLResourceStorageModeShared];
      memset(buffer.contents, 0, pitch * height);
      for (unsigned row = 0; row < (atlas ? h : height); ++row)
        memcpy((uint8_t *)buffer.contents + row * pitch,
               (const uint8_t *)source.bytes + ((y + row) * stride + x) * 4, (atlas ? w : width) * 4);
      auto upload = [blitQueue commandBuffer];
      auto blit = [upload blitCommandEncoder];
      [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch * height
          sourceSize:MTLSizeMake(width,height,1) toTexture:input destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0,0,0)];
      [blit endEncoding]; [upload commit]; [upload waitUntilCompleted];
      if (upload.status != MTLCommandBufferStatusCompleted) return 2;
      scaler.colorTexture = input; scaler.outputTexture = output;
      scaler.inputContentWidth = w; scaler.inputContentHeight = h;
      auto cmd = [blitQueue commandBuffer];
      [scaler encodeToCommandBuffer:cmd];
      [cmd commit]; [cmd waitUntilCompleted];
      if (cmd.status != MTLCommandBufferStatusCompleted) return 2;
      auto read = [blitQueue commandBuffer];
      blit = [read blitCommandEncoder];
      [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
          sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0
          destinationBytesPerRow:pitch destinationBytesPerImage:pitch * height];
      [blit endEncoding]; [read commit]; [read waitUntilCompleted];
      if (read.status != MTLCommandBufferStatusCompleted) return 2;
      std::vector<uint8_t> expected(bytes, 19);
      for (unsigned row = 0; row < height; ++row)
        memcpy(expected.data() + ((row + y) * stride + x) * 4,
               (const uint8_t *)buffer.contents + row * pitch, width * 4);
      size_t different = 0;
      for (size_t i = 0; i < bytes; ++i) different += expected[i] != ((const uint8_t *)actual.bytes)[i];
      std::printf("ISP_NATIVE atlas=%u bytes=%zu mismatches=%zu\n", atlas, bytes, different);
      if (different) return 1;
    }
  }
}
