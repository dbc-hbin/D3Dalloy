#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char *what, NSError *error = nil) {
  if (!condition) {
    std::fprintf(stderr, "OWNED_METAL4_COLOR_MAP_FAIL %s %s\n", what,
                 error ? error.localizedDescription.UTF8String : "");
    std::exit(1);
  }
}

int main(int argc, char **argv) {
  const bool support = argc < 2 || std::strtoul(argv[1], nullptr, 0) != 0;
  const bool identity_only = argc > 3;
  const bool disabled = argc > 2 && !identity_only;
  const NSUInteger disabled_index = disabled ? std::strtoull(argv[2], nullptr, 0) : 0;
  std::printf("OWNED_METAL4_COLOR_MAP_CONTRACT support=%u disabled=%u index=%llu\n", support, disabled,
              (unsigned long long)disabled_index);
  std::fflush(stdout);
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    require(device != nil, "device");
    NSError *error = nil;
    auto compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
    require(compiler != nil, "compiler", error);
    NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 vertex_main(uint id [[vertex_id]]) {
  const float2 positions[] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
  return float4(positions[id], 0, 1);
}
struct Colors { float4 a [[color(0)]]; float4 b [[color(1)]]; };
fragment Colors mrt() {
  return {float4(17, 34, 51, 255) / 255.0f, float4(68, 85, 102, 255) / 255.0f};
}
fragment float4 single() { return float4(1, 2, 3, 0) / 255.0f; }
)";
    auto library = [device newLibraryWithSource:source options:nil error:&error];
    require(library != nil, "library", error);
    NSMutableArray<id<MTLRenderPipelineState>> *pipelines = [NSMutableArray new];
    for (unsigned variant = 0; variant < 3; ++variant) {
      auto descriptor = [MTL4RenderPipelineDescriptor new];
      auto vertex = [MTL4LibraryFunctionDescriptor new];
      vertex.library = library;
      vertex.name = @"vertex_main";
      descriptor.vertexFunctionDescriptor = vertex;
      auto fragment = [MTL4LibraryFunctionDescriptor new];
      fragment.library = library;
      fragment.name = variant == 1 ? @"single" : @"mrt";
      descriptor.fragmentFunctionDescriptor = fragment;
      descriptor.rasterSampleCount = 1;
      descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
      descriptor.colorAttachmentMappingState = variant == 2
          ? MTL4LogicalToPhysicalColorAttachmentMappingStateIdentity
          : MTL4LogicalToPhysicalColorAttachmentMappingStateInherited;
      for (unsigned slot = 0; slot < (variant == 1 ? 1u : 2u); ++slot)
        descriptor.colorAttachments[slot].pixelFormat = MTLPixelFormatRGBA8Unorm;
      if (variant == 1) {
        auto attachment = descriptor.colorAttachments[0];
        attachment.blendingState = MTL4BlendStateEnabled;
        attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
        attachment.destinationRGBBlendFactor = MTLBlendFactorOne;
        attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
        attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
      }
      auto pipeline = [compiler newRenderPipelineStateWithDescriptor:descriptor compilerTaskOptions:nil error:&error];
      require(pipeline != nil, "pipeline", error);
      [pipelines addObject:pipeline];
    }
    auto queue = [device newMTL4CommandQueue];
    auto allocator = [device newCommandAllocator];
    auto residency = [device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
    require(queue && allocator && residency, "execution-resources", error);
    NSMutableArray<id<MTLTexture>> *targets = [NSMutableArray new];
    for (unsigned slot = 0; slot < 3; ++slot) {
      auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                          width:4 height:2 mipmapped:NO];
      descriptor.storageMode = MTLStorageModeShared;
      descriptor.usage = MTLTextureUsageRenderTarget;
      auto texture = [device newTextureWithDescriptor:descriptor];
      require(texture != nil, "texture");
      [targets addObject:texture];
      [residency addAllocation:texture];
    }
    [residency commit];
    for (unsigned execution = 0; execution < 2; ++execution) {
      auto command = [device newCommandBuffer];
      require(command != nil, "command-buffer");
      [command beginCommandBufferWithAllocator:allocator];
      [command useResidencySet:residency];
      auto pass = [MTL4RenderPassDescriptor new];
      pass.supportColorAttachmentMapping = support;
      pass.renderTargetWidth = 4;
      pass.renderTargetHeight = 2;
      pass.renderTargetArrayLength = 1;
      for (unsigned slot = 0; slot < 3; ++slot) {
        if (identity_only && slot == 2) continue;
        auto attachment = pass.colorAttachments[slot];
        attachment.texture = targets[slot];
        attachment.loadAction = MTLLoadActionClear;
        attachment.storeAction = MTLStoreActionStore;
        attachment.clearColor = MTLClearColorMake(16.0 / 255, 32.0 / 255, 48.0 / 255, 64.0 / 255);
      }
      auto encoder = [command renderCommandEncoderWithDescriptor:pass];
      require(encoder != nil, "render-encoder");
      [encoder setViewport:MTLViewport{0, 0, 4, 2, 0, 1}];
      auto mapping = [MTLLogicalToPhysicalColorAttachmentMap new];
      if (disabled) [mapping setPhysicalIndex:disabled_index forLogicalIndex:1];
      if (support || argc > 4) [encoder setColorAttachmentMap:mapping];
      [encoder setRenderPipelineState:pipelines[0]];
      [encoder setScissorRect:MTLScissorRect{0, 0, 2, 2}];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

      if (!identity_only) {
      [mapping setPhysicalIndex:1 forLogicalIndex:0];
      [mapping setPhysicalIndex:0 forLogicalIndex:1];
      [encoder setColorAttachmentMap:mapping];
      [encoder setScissorRect:MTLScissorRect{2, 0, 2, 2}];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

      // The blend state belongs to logical output 0, even when it writes physical slot 2.
      [mapping reset];
      [mapping setPhysicalIndex:2 forLogicalIndex:0];
      [mapping setPhysicalIndex:0 forLogicalIndex:2];
      [encoder setColorAttachmentMap:mapping];
      [encoder setRenderPipelineState:pipelines[1]];
      [encoder setScissorRect:MTLScissorRect{0, 0, 2, 2}];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

      [encoder setColorAttachmentMap:nil];
      [encoder setRenderPipelineState:pipelines[0]];
      [encoder setScissorRect:MTLScissorRect{3, 1, 1, 1}];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];

      // An Identity PSO must ignore the encoder's nonidentity map.
      [mapping reset];
      [mapping setPhysicalIndex:1 forLogicalIndex:0];
      [mapping setPhysicalIndex:0 forLogicalIndex:1];
      [encoder setColorAttachmentMap:mapping];
      [encoder setRenderPipelineState:pipelines[2]];
      [encoder setScissorRect:MTLScissorRect{2, 0, 1, 1}];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      }
      [encoder endEncoding];
      [command endCommandBuffer];

      auto completed = dispatch_semaphore_create(0);
      __block NSError *gpu_error = nil;
      auto options = [MTL4CommitOptions new];
      [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
        gpu_error = feedback.error;
        dispatch_semaphore_signal(completed);
      }];
      id<MTL4CommandBuffer> commands[] = {command};
      [queue commit:commands count:1 options:options];
      require(dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) == 0,
              "completion-timeout");
      require(gpu_error == nil, "gpu-completion", gpu_error);
      for (unsigned slot = 0; slot < 3; ++slot) {
        if (identity_only && slot == 2) continue;
        std::array<uint32_t, 8> pixels{};
        [targets[slot] getBytes:pixels.data() bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
        std::printf("OWNED_METAL4_COLOR_MAP_ATTACHMENT execution=%u slot=%u pixels=", execution, slot);
        for (unsigned i = 0; i < pixels.size(); ++i) {
          const unsigned x = i % 4, y = i / 4;
          const bool identity = x < 2 || (x == 3 && y == 1) || (x == 2 && y == 0);
          const uint32_t expected = identity_only ? (x < 2 ? (slot ? 0xff665544u : 0xff332211u) : 0x40302010u)
              : disabled && slot == 1 && x < 2 ? 0x40302010u
              : slot == 2 ? (x < 2 ? 0x40332211u : 0x40302010u)
              : ((slot == 0) == identity ? 0xff332211u : 0xff665544u);
          std::printf("%s%08x", i ? "," : "", pixels[i]);
          if (pixels[i] != expected) {
            std::fprintf(stderr, "\nOWNED_METAL4_COLOR_MAP_FAIL execution=%u slot=%u pixel=%u actual=%08x expected=%08x\n",
                         execution, slot, i, pixels[i], expected);
            return 1;
          }
        }
        std::printf("\n");
      }
      [allocator reset];
    }
    if (identity_only)
      std::printf("OWNED_METAL4_COLOR_MAP_INHERITED_PASS executions=2 pixels=32 support=%u explicit_identity=%u\n", support, argc > 4 || support);
    else
      std::printf("OWNED_METAL4_COLOR_MAP_PASS executions=2 pixels=48 mrt_swap=1 logical_blend=1 unused_outputs=1 nil_reset=1 identity_pso=1\n");
  }
  return 0;
}
