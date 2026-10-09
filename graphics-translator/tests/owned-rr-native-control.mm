// SPDX-License-Identifier: LGPL-2.1-or-later
// Independent native MetalFX denoiser control: no production bridge, NGX,
// Wine, adapter shaders or proprietary implementation is linked.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include "owned-rr-pattern.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

@interface MTLFXTemporalDenoisedScalerDescriptor (OwnedRRBackend)
- (void)setVersion:(NSUInteger)version;
@end

namespace {
void require(bool condition, const char *reason) {
  if (!condition) { std::fprintf(stderr,"OWNED_RR_NATIVE_FAIL reason=%s\n",reason); std::exit(1); }
}
id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, MTLTextureUsage usage) {
  auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
    width:owned_rr::Width height:owned_rr::Height mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate; descriptor.usage = usage;
  auto result = [device newTextureWithDescriptor:descriptor]; require(result != nil,"texture"); return result;
}
void upload(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> destination,
            const void *data, size_t row) {
  auto source = [device newBufferWithBytes:data length:row * owned_rr::Height options:MTLResourceStorageModeShared];
  require(source != nil,"upload-buffer");
  auto command = [queue commandBuffer]; auto encoder = [command blitCommandEncoder];
  [encoder copyFromBuffer:source sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * owned_rr::Height
    sourceSize:MTLSizeMake(owned_rr::Width,owned_rr::Height,1) toTexture:destination
    destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
  [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
  require(command.status == MTLCommandBufferStatusCompleted,"upload-completed"); [source release];
}
void save(const char *directory, const char *prefix, unsigned frame, const std::vector<uint16_t> &data) {
  char path[4096]; std::snprintf(path,sizeof(path),"%s/%s-%u.bin",directory,prefix,frame);
  FILE *file = std::fopen(path,"wb"); require(file != nullptr,"output-open");
  require(std::fwrite(data.data(),data.size() * sizeof(uint16_t),1,file) == 1,"output-write");
  require(std::fclose(file) == 0,"output-close");
}
std::vector<std::vector<uint16_t>> run(id<MTLDevice> device, id<MTL4Compiler> compiler,
  id<MTLCommandQueue> queue, id<MTL4CommandQueue> queue4, const char *directory, bool transpose,
  bool mirror = false, bool replay = false, bool rotate = false) {
  auto descriptor = [MTLFXTemporalDenoisedScalerDescriptor new];
  descriptor.inputWidth = descriptor.outputWidth = owned_rr::Width;
  descriptor.inputHeight = descriptor.outputHeight = owned_rr::Height;
  descriptor.colorTextureFormat = descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
  descriptor.depthTextureFormat = descriptor.roughnessTextureFormat = descriptor.specularHitDistanceTextureFormat = MTLPixelFormatR32Float;
  descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
  descriptor.diffuseAlbedoTextureFormat = descriptor.specularAlbedoTextureFormat = descriptor.normalTextureFormat = MTLPixelFormatRGBA16Float;
  descriptor.specularHitDistanceTextureEnabled = YES; descriptor.requiresSynchronousInitialization = YES;
  // Keep the public factory and native math; select Apple's non-fused GPU implementation.
  require([descriptor respondsToSelector:@selector(setVersion:)],"native-non-fused-backend");
  [descriptor setVersion:0x10000];
  std::puts("OWNED_RR_NATIVE_BACKEND requested_private_version=65536");
  auto scaler = [descriptor newTemporalDenoisedScalerWithDevice:device compiler:compiler];
  require(scaler != nil,"native-denoised-scaler");
  const float minScale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:device];
  const float maxScale = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:device];
  require(minScale <= 1 && maxScale >= 1,"native-supported-1x-scale");
  std::array<id<MTLTexture>,9> textures = {
    texture(device,MTLPixelFormatRGBA16Float,scaler.colorTextureUsage),
    texture(device,MTLPixelFormatR32Float,scaler.depthTextureUsage),
    texture(device,MTLPixelFormatRG16Float,scaler.motionTextureUsage),
    texture(device,MTLPixelFormatRGBA16Float,scaler.outputTextureUsage),
    texture(device,MTLPixelFormatRGBA16Float,scaler.diffuseAlbedoTextureUsage),
    texture(device,MTLPixelFormatRGBA16Float,scaler.specularAlbedoTextureUsage),
    texture(device,MTLPixelFormatRGBA16Float,scaler.normalTextureUsage),
    texture(device,MTLPixelFormatR32Float,scaler.roughnessTextureUsage),
    texture(device,MTLPixelFormatR32Float,scaler.specularHitDistanceTextureUsage)};
  const MTLPixelFormat formats[] = {descriptor.colorTextureFormat,descriptor.depthTextureFormat,
    descriptor.motionTextureFormat,descriptor.outputTextureFormat,descriptor.diffuseAlbedoTextureFormat,
    descriptor.specularAlbedoTextureFormat,descriptor.normalTextureFormat,descriptor.roughnessTextureFormat,
    descriptor.specularHitDistanceTextureFormat};
  const MTLTextureUsage usages[] = {scaler.colorTextureUsage,scaler.depthTextureUsage,scaler.motionTextureUsage,
    scaler.outputTextureUsage,scaler.diffuseAlbedoTextureUsage,scaler.specularAlbedoTextureUsage,
    scaler.normalTextureUsage,scaler.roughnessTextureUsage,scaler.specularHitDistanceTextureUsage};
  for (size_t i = 0; i < textures.size(); ++i) {
    auto input = textures[i];
    require(input.device == device && input.pixelFormat == formats[i] && (input.usage & usages[i]) == usages[i] &&
      input.width == owned_rr::Width && input.height == owned_rr::Height && input.textureType == MTLTextureType2D &&
      input.depth == 1 && input.arrayLength == 1 && input.sampleCount == 1 && input.storageMode == MTLStorageModePrivate,
      "native-texture-descriptor-preconditions");
  }
  std::printf("OWNED_RR_NATIVE_PRECONDITIONS width=%u height=%u scale=1 min_scale=%g max_scale=%g textures=9 descriptor_usage_exact=1 private=1 device=%s\n",
    owned_rr::Width,owned_rr::Height,minScale,maxScale,device.name.UTF8String);
  auto completed = [device newSharedEvent];
  auto allocator = [device newCommandAllocator];
  const size_t bytes = owned_rr::Width * owned_rr::Height * 8;
  auto readback = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
  NSError *error = nil;
  auto residencyDescriptor = [MTLResidencySetDescriptor new];
  auto residency = [device newResidencySetWithDescriptor:residencyDescriptor error:&error];
  require(completed && allocator && readback && residency,"native-execution-resources");
  for (auto input : textures) [residency addAllocation:input]; [residency commit];
  std::vector<std::vector<uint16_t>> results;
  std::vector<id<MTLTexture>> priorTextures;
  unsigned denoised = 0;
  for (unsigned execution = 1; execution <= owned_rr::Frames + unsigned(replay); ++execution) {
    const unsigned frame = execution > owned_rr::Frames ? owned_rr::Frames : execution;
    if (rotate && execution > 1 && execution <= owned_rr::Frames) {
      // Public D3D12 changes texture identities per frame; retain old versions to isolate identity from lifetime.
      for (auto &input : textures) {
        priorTextures.push_back(input); input = texture(device,input.pixelFormat,input.usage);
        [residency addAllocation:input];
      }
      [residency commit];
    }
    auto scene = owned_rr::makeFrame(frame);
    if (mirror) {
      for (auto &value : scene.specular) value = 0x3c00;
      for (auto &value : scene.roughness) value = 0;
    }
    upload(device,queue,textures[0],scene.color.data(),owned_rr::Width * 8);
    upload(device,queue,textures[1],scene.depth.data(),owned_rr::Width * 4);
    upload(device,queue,textures[2],scene.motion.data(),owned_rr::Width * 4);
    upload(device,queue,textures[4],scene.diffuse.data(),owned_rr::Width * 8);
    upload(device,queue,textures[5],scene.specular.data(),owned_rr::Width * 8);
    upload(device,queue,textures[6],scene.normals.data(),owned_rr::Width * 8);
    upload(device,queue,textures[7],scene.roughness.data(),owned_rr::Width * 4);
    upload(device,queue,textures[8],scene.hit.data(),owned_rr::Width * 4);
    scaler.colorTexture = textures[0]; scaler.depthTexture = textures[1]; scaler.motionTexture = textures[2];
    scaler.outputTexture = textures[3]; scaler.diffuseAlbedoTexture = textures[4]; scaler.specularAlbedoTexture = textures[5];
    scaler.normalTexture = textures[6]; scaler.roughnessTexture = textures[7]; scaler.specularHitDistanceTexture = textures[8];
    // Re-executing an immutable frame advances native history; release is not a reset.
    scaler.depthReversed = YES; scaler.shouldResetHistory = scene.reset;
    scaler.preExposure = 1; scaler.motionVectorScaleX = scaler.motionVectorScaleY = 1;
    scaler.jitterOffsetX = scene.jitterX; scaler.jitterOffsetY = scene.jitterY;
    // Independently authored literal SIMD columns, NOT the bridge's matrix converter.
    simd_float4x4 world = {{simd_make_float4(.8f,0,.6f,0),simd_make_float4(0,1,0,0),
      simd_make_float4(-.6f,0,.8f,0),simd_make_float4(.125f + frame * .125f,-.25f,.5f,1)}};
    simd_float4x4 projection = {{simd_make_float4(1.5f,0,0,0),simd_make_float4(0,1.25f,0,0),
      simd_make_float4(.125f + frame * .0625f,-.0625f,0,1),simd_make_float4(0,0,.1f,0)}};
    if (transpose) { world = simd_transpose(world); projection = simd_transpose(projection); }
    scaler.worldToViewMatrix = world; scaler.viewToClipMatrix = projection;
    require(owned_rr::saveInputs(directory,mirror ? (transpose ? "native-rr-mirror-input-transposed" : "native-rr-mirror-input") :
      (transpose ? "native-rr-input-transposed" : "native-rr-input"),frame,scene,&world,&projection),"native-input-parity-artifact");
    auto command4 = [device newCommandBuffer]; [command4 beginCommandBufferWithAllocator:allocator];
    [command4 useResidencySet:residency]; [scaler encodeToCommandBuffer:command4]; [command4 endCommandBuffer];
    [queue4 commit:&command4 count:1]; [queue4 signalEvent:completed value:execution];
    require([completed waitUntilSignaledValue:execution timeoutMS:15000],"native-metal4-completed");
    auto command = [queue commandBuffer]; auto encoder = [command blitCommandEncoder];
    [encoder copyFromTexture:textures[3] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
      sourceSize:MTLSizeMake(owned_rr::Width,owned_rr::Height,1) toBuffer:readback destinationOffset:0
      destinationBytesPerRow:owned_rr::Width * 8 destinationBytesPerImage:bytes];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted,"native-readback-completed");
    results.emplace_back(scene.color.size()); std::memcpy(results.back().data(),readback.contents,bytes);
    unsigned different = 0;
    for (size_t i = 0; i < scene.color.size(); ++i) {
      require((results.back()[i] & 0x7c00) != 0x7c00,"finite-output");
      if (i % 4 != 3 && results.back()[i] != scene.color[i]) ++different;
    }
    denoised += different != 0;
    save(directory,execution > owned_rr::Frames ? (mirror ? "native-rr-mirror-replay" : "native-rr-replay") :
      mirror ? (transpose ? "native-rr-mirror-transposed" : "native-rr-mirror") :
      (transpose ? "native-rr-transposed" : "native-rr-frame"),frame,results.back());
    // MetalFX consumes the reset request during encode; log the submitted flag, not only its post-encode getter.
    std::printf("OWNED_RR_NATIVE_FRAME frame=%u transpose=%u mirror=%u changed_components=%u reset=%u reset_after_encode=%u bytes=%zu\n",
      frame,transpose,mirror,different,unsigned(scene.reset),unsigned(bool(scaler.shouldResetHistory)),bytes);
    [command4 release]; [allocator reset];
  }
  require(denoised == owned_rr::Frames + unsigned(replay),"denoiser-is-not-source-copy");
  scaler.colorTexture = scaler.depthTexture = scaler.motionTexture = scaler.outputTexture = nil;
  scaler.diffuseAlbedoTexture = scaler.specularAlbedoTexture = scaler.normalTexture = nil;
  scaler.roughnessTexture = scaler.specularHitDistanceTexture = nil;
  for (auto input : textures) [input release];
  for (auto input : priorTextures) [input release];
  [completed release]; [allocator release]; [readback release]; [residency release]; [residencyDescriptor release];
  [scaler release]; [descriptor release]; return results;
}
}
int main(int argc, char **argv) {
  require(argc == 2 || (argc == 3 && (std::strcmp(argv[2],"replay") == 0 ||
    std::strcmp(argv[2],"rotate") == 0 || std::strcmp(argv[2],"rotate-replay") == 0)),
    "usage-output-directory-[replay|rotate|rotate-replay]");
  const bool rotate = argc == 3 && std::strncmp(argv[2],"rotate",6) == 0;
  const bool replay = argc == 3 && (std::strcmp(argv[2],"replay") == 0 || std::strcmp(argv[2],"rotate-replay") == 0);
  @autoreleasepool {
    auto device = MTLCreateSystemDefaultDevice();
    require(device && [MTLFXTemporalDenoisedScalerDescriptor supportsMetal4FX:device],"native-supported");
    auto queue = [device newCommandQueue]; auto queue4 = [device newMTL4CommandQueue];
    auto compilerDescriptor = [MTL4CompilerDescriptor new]; NSError *error = nil;
    auto compiler = [device newCompilerWithDescriptor:compilerDescriptor error:&error];
    require(queue && queue4 && compiler,"native-device-context");
    if (replay) {
      run(device,compiler,queue,queue4,argv[1],false,false,true,rotate);
      run(device,compiler,queue,queue4,argv[1],false,true,true,rotate);
      puts("OWNED_RR_NATIVE_REPLAY_PASS frames=18 history_frames=8 replay_frame=8 replay_reset=0 source=public-metalfx-denoiser");
      [compiler release]; [compilerDescriptor release]; [queue release]; [queue4 release]; [device release];
      return 0;
    }
    auto reference = run(device,compiler,queue,queue4,argv[1],false,false,false,rotate);
    auto transposed = run(device,compiler,queue,queue4,argv[1],true,false,false,rotate);
    unsigned sensitive = 0;
    for (size_t frame = 0; frame < reference.size(); ++frame) sensitive += reference[frame] != transposed[frame];
    auto mirror = run(device,compiler,queue,queue4,argv[1],false,true,false,rotate);
    auto mirrorTransposed = run(device,compiler,queue,queue4,argv[1],true,true,false,rotate);
    for (size_t frame = 0; frame < mirror.size(); ++frame) sensitive += mirror[frame] != mirrorTransposed[frame];
    require(sensitive != 0,"asymmetric-matrix-oracle-sensitive");
    std::printf("OWNED_RR_NATIVE_PASS frames=%u matrix_sensitive_frames=%u source=public-metalfx-denoiser\n",owned_rr::Frames,sensitive);
    [compiler release]; [compilerDescriptor release]; [queue release]; [queue4 release]; [device release];
  }
}
