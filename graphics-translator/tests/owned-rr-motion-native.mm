// SPDX-License-Identifier: LGPL-2.1-or-later
// clang++ -std=c++20 -O2 -fblocks -framework Foundation -framework Metal -framework MetalFX \
//   graphics-translator/tests/owned-rr-motion-native.mm -o build/owned-rr-motion-native
// Resource selection uses the production RR code and real native MetalFX usages.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "owned-rr-pattern.h"
#define WINEMETAL_API
#include "../../src/winemetal/winemetal_metal4_fx.h"
#include "../../src/winemetal/unix/metalfx_rr.inc"

// This probe uses native queues; Dormant must never enter the Wine recording transport.
extern "C" id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t) { std::abort(); }
extern "C" void *winemetal_metal4_split_command_buffer(void *) { std::abort(); }
extern "C" void winemetal_metal4_retain_resource(obj_handle_t, id) { std::abort(); }

static void require(bool ok, const char *reason) {
  if (!ok) { std::fprintf(stderr,"OWNED_RR_REUSE_FAIL reason=%s\n",reason); std::exit(1); }
}
static void finish(id<MTLCommandBuffer> command, id<MTLSharedEvent> event, uint64_t &value) {
  auto done=dispatch_semaphore_create(0);
  [command addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(done); }];
  [command encodeSignalEvent:event value:++value]; [command commit];
  require(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC))==0 &&
    command.status==MTLCommandBufferStatusCompleted,"legacy-command-completion");
  dispatch_release(done);
}
static void upload(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLSharedEvent> event,
                   uint64_t &value, id<MTLTexture> destination, const void *data, size_t row) {
  auto buffer = [device newBufferWithBytes:data length:row * 128 options:MTLResourceStorageModeShared];
  auto command = [queue commandBuffer]; auto blit = [command blitCommandEncoder];
  [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row * 128
    sourceSize:MTLSizeMake(128,128,1) toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
  [blit endEncoding]; finish(command,event,value);
  [buffer release];
}
static void descriptorReuse(const std::shared_ptr<dxmt_rr::Core> &owner) {
  auto core = std::make_shared<dxmt_rr::Core>();
  core->device = [owner->device retain]; core->compiler = [owner->compiler retain];
  core->info.feature_flags = dxmt_rr::Flag | 2 | 8;
  WMTMetal4FXTemporalFrame p{}; p.input_width = p.input_height = 128; p.output_rect = {0,0,128,128};
  auto a = dxmt_rr::generation(*core,p); require(bool(a),"generation-A");
  p.specular_hit_distance = 1; auto b = dxmt_rr::generation(*core,p); require(bool(b),"generation-B");
  p.reactive = 1; auto c = dxmt_rr::generation(*core,p); require(bool(c),"generation-C");
  p.specular_hit_distance = p.reactive = 0; auto restored = dxmt_rr::generation(*core,p);
  require(restored == a && a != b && b != c && a != c,"A-B-C-A-descriptor-reuse");
  require(b->serial == a->serial + 1 && c->serial == b->serial + 1,"three-native-generation-creations");
  auto queue = [core->device newCommandQueue]; auto queue4 = [core->device newMTL4CommandQueue];
  auto event = [core->device newSharedEvent]; uint64_t value = 0;
  require(queue && queue4 && event,"native-queues");
  std::array<std::shared_ptr<dxmt_rr::Generation>,4> generations{a,b,c,restored};
  std::vector<float> firstA;
  for (unsigned mode = 0; mode < generations.size(); ++mode) {
    auto frame = [DXMTRRFrame new]; frame->core = core; frame->generation = generations[mode];
    frame->props = p; frame->props.motion_scale_x = frame->props.motion_scale_y = 1;
    auto scaler = frame->generation->scaler;
    const MTLTextureUsage usage[dxmt_rr::Count] = {scaler.colorTextureUsage,scaler.depthTextureUsage,
      scaler.motionTextureUsage,scaler.outputTextureUsage,0,scaler.reactiveMaskTextureUsage,0,
      scaler.diffuseAlbedoTextureUsage,scaler.specularAlbedoTextureUsage,scaler.normalTextureUsage,
      scaler.roughnessTextureUsage,scaler.specularHitDistanceTextureUsage};
    for (size_t i : {0u,1u,2u,3u,7u,8u,9u,10u}) {
      auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dxmt_rr::format(i) width:128 height:128 mipmapped:NO];
      d.storageMode = MTLStorageModePrivate; d.usage = usage[i] | MTLTextureUsageShaderRead;
      frame->inputs[i] = [core->device newTextureWithDescriptor:d]; require(frame->inputs[i] != nil,"required-input");
      frame->rects[i] = {0,0,128,128};
    }
    if (mode == 1 || mode == 2) {
      auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dxmt_rr::format(11) width:128 height:128 mipmapped:NO];
      d.storageMode = MTLStorageModePrivate; d.usage = usage[11] | MTLTextureUsageShaderRead;
      frame->inputs[11] = [core->device newTextureWithDescriptor:d]; frame->rects[11] = {0,0,128,128};
    }
    if (mode == 2) {
      auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dxmt_rr::format(5) width:128 height:128 mipmapped:NO];
      d.storageMode = MTLStorageModePrivate; d.usage = usage[5] | MTLTextureUsageShaderRead;
      frame->inputs[5] = [core->device newTextureWithDescriptor:d]; frame->rects[5] = {0,0,128,128};
    }
    auto scene = owned_rr::makeFrame(1);
    upload(core->device,queue,event,value,frame->inputs[0],scene.color.data(),128*8);
    upload(core->device,queue,event,value,frame->inputs[1],scene.depth.data(),128*4);
    upload(core->device,queue,event,value,frame->inputs[2],scene.motion.data(),128*4);
    upload(core->device,queue,event,value,frame->inputs[7],scene.diffuse.data(),128*8);
    upload(core->device,queue,event,value,frame->inputs[8],scene.specular.data(),128*8);
    upload(core->device,queue,event,value,frame->inputs[9],scene.normals.data(),128*8);
    upload(core->device,queue,event,value,frame->inputs[10],scene.roughness.data(),128*4);
    if (frame->inputs[11]) upload(core->device,queue,event,value,frame->inputs[11],scene.hit.data(),128*4);
    std::vector<uint16_t> reactive(128*128,0);
    if (frame->inputs[5]) upload(core->device,queue,event,value,frame->inputs[5],reactive.data(),128*2);
    dxmt_rr::Resources resources; std::array<id<MTLTexture>,dxmt_rr::Count> bound{};
    require(dxmtRRResources(frame,resources,bound),"native-resource-selection");
    auto allocator = [core->device newCommandAllocator];
    auto readback = [core->device newBufferWithLength:128*128*8 options:MTLResourceStorageModeShared];
    for (auto texture : bound) if (texture) [resources.residency addAllocation:texture];
    [resources.residency commit];
    for (unsigned warm = 0; warm < 2; ++warm) {
      scaler.colorTexture=bound[0]; scaler.depthTexture=bound[1]; scaler.motionTexture=bound[2]; scaler.outputTexture=bound[3];
      scaler.diffuseAlbedoTexture=bound[7]; scaler.specularAlbedoTexture=bound[8]; scaler.normalTexture=bound[9];
      scaler.roughnessTexture=bound[10]; scaler.specularHitDistanceTexture=bound[11]; scaler.reactiveMaskTexture=bound[5];
      scaler.preExposure=1; scaler.depthReversed=YES; scaler.shouldResetHistory=!warm;
      scaler.motionVectorScaleX=scaler.motionVectorScaleY=1;
      scaler.jitterOffsetX=scene.jitterX; scaler.jitterOffsetY=scene.jitterY;
      simd_float4x4 world, projection; dxmt_rr::matrix(world,scene.world); dxmt_rr::matrix(projection,scene.projection);
      scaler.worldToViewMatrix=world; scaler.viewToClipMatrix=projection;
      auto command=[core->device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
      [command useResidencySet:resources.residency]; [scaler encodeToCommandBuffer:command]; [command endCommandBuffer];
      [queue4 commit:&command count:1]; [queue4 signalEvent:event value:++value];
      require([event waitUntilSignaledValue:value timeoutMS:15000],"rr-native-completion");
      auto copy=[queue commandBuffer]; auto blit=[copy blitCommandEncoder];
      [blit copyFromTexture:bound[3] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(128,128,1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:128*8 destinationBytesPerImage:128*128*8];
      [blit endEncoding]; finish(copy,event,value);
      const auto *pixels=static_cast<const _Float16 *>(readback.contents); double mean=0; float maxDelta=0;
      if (mode==0 && !warm) firstA.resize(128*128*4);
      for (size_t i=0;i<128*128*4;++i) {
        float pixel=pixels[i]; require(std::isfinite(pixel),"rr-finite-output");
        if (i%4!=3) mean+=pixel;
        if (mode==0 && !warm) firstA[i]=pixel;
        if (mode==3 && !warm) maxDelta=std::max(maxDelta,std::fabs(pixel-firstA[i]));
      }
      mean/=128*128*3; require(mean>.05 && mean<1,"rr-valid-output-mean");
      if (mode==3 && !warm) require(maxDelta<=2.f/255,"rr-restored-reset-parity");
      std::printf("OWNED_RR_REUSE_FRAME mode=%u warm=%u generation=%llu reset=%u mean=%.9g max_restored_delta=%.9g\n",
        mode,warm,(unsigned long long)frame->generation->serial,unsigned(!warm),mean,maxDelta);
      [command release]; [allocator reset];
    }
    scaler.colorTexture=scaler.depthTexture=scaler.motionTexture=scaler.outputTexture=nil;
    scaler.diffuseAlbedoTexture=scaler.specularAlbedoTexture=scaler.normalTexture=nil;
    scaler.roughnessTexture=scaler.specularHitDistanceTexture=scaler.reactiveMaskTexture=nil;
    [readback release]; [allocator release]; [frame release];
  }
  [event release]; [queue4 release]; [queue release];
  auto feature = [DXMTRRFeature new]; feature->core = core;
  auto prepared = [DXMTRRFrame new]; prepared->core = core; prepared->generation = a;
  std::weak_ptr<dxmt_rr::Generation> retained = a, idleB = b, idleC = c;
  a.reset(); b.reset(); c.reset(); restored.reset(); generations.fill(nullptr);
  struct WMTMetal4Call dormant{}; dormant.operation = WMTMetal4FXDormantTemporal;
  dormant.args[0] = uintptr_t(feature);
  require(dxmtRRCall(&dormant),"dormant-dispatch");
  for (const auto &cached : core->generations) require(!cached,"dormant-cache-empty");
  require(idleB.expired() && idleC.expired() && !retained.expired() &&
          prepared->generation->scaler != nil,"dormant-releases-only-idle-generations");
  auto execution = [DXMTRRExecution new]; execution->core = core;
  execution->generation = prepared->generation;
  [prepared release]; [feature release];
  require(!retained.expired() && execution->generation->scaler != nil,"execution-retains-generation");
  [execution release];
  require(retained.expired(),"retired-generation-released");
  std::puts("OWNED_RR_DORMANT_PASS cache_empty=1 idle_released=2 prepared_retained=1 execution_retained=1 retired_released=1");
  std::puts("OWNED_RR_REUSE_PASS sequence=A,B,C,A creations=3 frames=8 version=65536");
}

int main() { @autoreleasepool {
  auto frame = [DXMTRRFrame new];
  frame->core = std::make_shared<dxmt_rr::Core>();
  auto &core = *frame->core;
  core.device = MTLCreateSystemDefaultDevice();
  auto descriptor = [MTL4CompilerDescriptor new];
  core.compiler = [core.device newCompilerWithDescriptor:descriptor error:nil]; [descriptor release];
  if (!core.device || !core.compiler) return 1;
  auto &p = frame->props;
  p.input_width = p.input_height = 128; p.output_rect = {0,0,128,128};
  p.motion_scale_x = p.motion_scale_y = 1;
  core.info.feature_flags = dxmt_rr::Flag | 2 | 8;
  frame->generation = dxmt_rr::generation(core,p);
  if (!frame->generation) return 1;
  const MTLTextureUsage usage = frame->generation->scaler.motionTextureUsage | MTLTextureUsageShaderRead;
  unsigned cases = 0;
  for (unsigned variant = 0; variant < 9; ++variant) {
    core.info.feature_flags = dxmt_rr::Flag | 2 | 8;
    p.motion_scale_x = p.motion_scale_y = 1;
    frame->rects[2] = {0,0,128,128};
    p.output_rect = {0,0,128,128};
    if (variant == 1) p.motion_scale_x = 2;
    if (variant == 2) p.motion_scale_y = -.5f;
    if (variant == 3) core.info.feature_flags |= 4; // Jitter removal depends on execution history, not prepare order.
    if (variant == 4) { core.info.feature_flags &= ~2u; frame->rects[2] = p.output_rect = {0,0,256,256}; }
    if (variant == 6) frame->rects[2].x = 4;
    if (variant == 8) p.output_rect = {0,0,256,256}; // Upscaling still accepts input-resolution MV.
    const unsigned width = variant == 4 ? 256 : variant == 6 || variant == 7 ? 132 : 128;
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
      variant == 5 ? MTLPixelFormatRG32Float : MTLPixelFormatRG16Float
      width:width height:variant == 4 ? 256 : 128 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = usage;
    [frame->inputs[2] release]; frame->inputs[2] = [core.device newTextureWithDescriptor:d];
    if (!frame->inputs[2]) return 1;
    auto formats = dxmt_rr::fixedFormats();
    formats[2] = dxmt_rr::descriptorFormat(core,p,2,frame->inputs[2],frame->rects[2]);
    frame->generation = dxmt_rr::generation(core,p,formats);
    if (!frame->generation) return 1;
    dxmt_rr::Resources resources;
    std::array<id<MTLTexture>,dxmt_rr::Count> bound{};
    const bool direct = variant == 0 || variant == 5 || variant == 8;
    if (!dxmtRRResources(frame,resources,bound) || (bound[2] == frame->inputs[2]) != direct ||
        (resources.textures[2] == nil) != direct) {
      std::fprintf(stderr,"OWNED_RR_MOTION_FAIL case=%u expected_direct=%u\n",variant,unsigned(direct));
      return 1;
    }
    std::printf("OWNED_RR_MOTION_CASE case=%u direct=%u scratch=%u\n",variant,unsigned(direct),unsigned(!direct));
    ++cases;
  }
  descriptorReuse(frame->core);
  [frame release];
  std::printf("OWNED_RR_MOTION_PASS cases=%u direct=3 conversion_required=6\n",cases);
} }
