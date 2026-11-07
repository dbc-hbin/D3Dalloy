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
static void scratchPoolProof() {
  auto core = std::make_shared<d3dalloy_rr::Core>();
  core->idleByteLimit = 300;
  auto make = [](uint32_t width, uint64_t bytes) {
    auto set = std::make_unique<d3dalloy_rr::Resources>();
    set->width = set->height = set->outputWidth = set->outputHeight = width;
    set->bytes = bytes; set->ready = true; return set;
  };
  auto a = make(128,100), b = make(256,100), c = make(512,100);
  const auto *bAddress = b.get();
  core->recycle(a); core->recycle(b); core->recycle(c);
  require(!a && !b && !c && core->idleBytes == 300,"pool-caches-retired-sets");
  d3dalloy_rr::Generation g{0}; g.width = g.height = g.outputWidth = g.outputHeight = 256;
  auto exact = core->takeResources(g);
  require(exact.get() == bAddress && core->idleBytes == 200,"pool-prefers-exact-extent");
  core->recycle(exact);
  auto fourth = make(1024,100); core->recycle(fourth);
  unsigned count = 0; for (const auto& entry : core->pool) count += bool(entry);
  require(!fourth && count == 3 && core->idleBytes == 300,"pool-bounded-at-three");
  core->idleByteLimit = 100; core->trimPool();
  require(core->idleBytes <= 100,"pool-trims-resize-high-water");
  auto large = make(2048,200); core->recycle(large);
  require(bool(large) && core->idleBytes <= 100,"pool-rejects-oversized-idle-set");
  auto failed = make(128,50); failed->ready = false; core->recycle(failed);
  require(bool(failed),"pool-does-not-cache-partial-allocation");
  auto feature = [D3DALLOYRRFeature new]; feature->core = core;
  auto execution = [D3DALLOYRRExecution new]; execution->frame = [D3DALLOYRRFrame new];
  execution->frame->core = core;
  execution->resources = make(128,100);
  struct WMTMetal4Call dormant{}; dormant.operation = WMTMetal4FXDormantTemporal;
  dormant.args[0] = uintptr_t(feature);
  require(d3dalloyRRCall(&dormant) && core->dormant && core->idleBytes == 0,"pool-dormant-drops-idle");
  require(bool(execution->resources),"pool-dormant-preserves-inflight");
  [execution release]; [feature release];
  for (const auto& entry : core->pool) require(!entry,"late-retirement-does-not-repopulate-dormant");
  auto replay = core->takeResources(g); replay->ready = true; replay->bytes = 100;
  core->recycle(replay);
  require(bool(replay) && core->idleBytes == 0,"dormant-replay-owns-unpooled-resources");
  std::puts("OWNED_RR_POOL_PASS max_idle_sets=3 byte_limit=working_set_based exact_extent=1 partial_failure=1 dormant_late_retirement=1 replay=1");
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
static void normalRoughnessProof(id<MTLDevice> device) {
  using namespace d3dalloy_rr;
  auto pipelines = devicePipelines(device); require(bool(pipelines),"packed-pipelines");
  auto queue = [device newMTL4CommandQueue]; auto event = [device newSharedEvent];
  unsigned cases = 0;
  for (auto inputFormat : {MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA16Float})
  for (unsigned variant=0;variant<3;++variant) {
    const unsigned width=variant==2?1:37, height=variant==1?1:29;
    auto make = [&](MTLPixelFormat format, unsigned w, unsigned h) {
      auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
      desc.storageMode=MTLStorageModeShared; desc.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
      auto result=[device newTextureWithDescriptor:desc]; require(result != nil,"packed-texture"); return result;
    };
    auto source=make(inputFormat,64,48);
    std::vector<float> values(64*48*4);
    for (size_t i=0;i<values.size();++i) values[i]=float(int(i%197)-98)/97.f;
    values[(5*64+3)*4+3]=1.0008f; values[(6*64+4)*4+3]=INFINITY; values[(7*64+5)*4+3]=NAN;
    if (inputFormat==MTLPixelFormatRGBA32Float)
      [source replaceRegion:MTLRegionMake2D(0,0,64,48) mipmapLevel:0 withBytes:values.data() bytesPerRow:64*16];
    else {
      std::vector<_Float16> half(values.begin(),values.end());
      [source replaceRegion:MTLRegionMake2D(0,0,64,48) mipmapLevel:0 withBytes:half.data() bytesPerRow:64*8];
    }
    id<MTLTexture> outputs[4] = {make(MTLPixelFormatRGBA16Float,width,height),make(MTLPixelFormatR32Float,width,height),
      make(MTLPixelFormatRGBA16Float,width,height),make(MTLPixelFormatR32Float,width,height)};
    Params p{}; p.x=3; p.y=5; p.width=width; p.height=height;
    p.sourceWidth=31; p.sourceHeight=23;
    auto buffer=[device newBufferWithLength:512 options:MTLResourceStorageModeShared];
    std::memcpy(buffer.contents,&p,sizeof(p)); p.mode=1;
    std::memcpy(static_cast<char*>(buffer.contents)+256,&p,sizeof(p));
    auto td=[MTL4ArgumentTableDescriptor new];td.maxBufferBindCount=1;td.maxTextureBindCount=3;td.initializeBindings=YES;
    auto table=[device newArgumentTableWithDescriptor:td error:nil];[td release];
    auto rd=[MTLResidencySetDescriptor new]; auto residency=[device newResidencySetWithDescriptor:rd error:nil];[rd release];
    require(buffer && table && residency,"packed-encoding-resources");
    [residency addAllocation:source];[residency addAllocation:buffer];
    for(auto output:outputs)[residency addAllocation:output];
    [residency commit];[residency requestResidency];
    auto allocator=[device newCommandAllocator]; auto command=[device newCommandBuffer];
    [command beginCommandBufferWithAllocator:allocator];[command useResidencySet:residency];
    auto encoder=[command computeCommandEncoder];
    for (unsigned pass=0;pass<3;++pass) {
      [table setAddress:buffer.gpuAddress+(pass==1?256:0) atIndex:0];
      [table setTexture:source.gpuResourceID atIndex:0];
      [table setTexture:outputs[pass==2?2:pass].gpuResourceID atIndex:1];
      [table setTexture:outputs[3].gpuResourceID atIndex:2];
      [encoder setComputePipelineState:pass==2?pipelines->normalRoughness:pipelines->conversion];
      [encoder setArgumentTable:table];
      [encoder dispatchThreads:MTLSizeMake(width,height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
    }
    [encoder endEncoding];[command endCommandBuffer];[queue commit:&command count:1];[queue signalEvent:event value:cases+1];
    require([event waitUntilSignaledValue:cases+1 timeoutMS:15000],"packed-completion");
    for (unsigned channel=0;channel<2;++channel) {
      const size_t pixel=channel?4:8;std::vector<uint8_t> separate(width*height*pixel),fused(separate.size());
      [outputs[channel] getBytes:separate.data() bytesPerRow:width*pixel fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
      [outputs[channel+2] getBytes:fused.data() bytesPerRow:width*pixel fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
      require(separate==fused,"packed-original-source-bit-exact");
    }
    auto frame=[D3DALLOYRRFrame new]; frame->props.roughness_packed=1;
    frame->inputs[9]=[source retain];frame->inputs[10]=[source retain];
    frame->rects[9]=frame->rects[10]={3,5,31,23};
    std::array<id<MTLTexture>,Count> bound{};bound[9]=outputs[2];bound[10]=outputs[3];
    require(d3dalloyRRFusesNormalRoughness(frame,bound),"packed-production-selection");
    frame->rects[10].x++;require(!d3dalloyRRFusesNormalRoughness(frame,bound),"packed-different-rect-fallback");
    frame->rects[10]=frame->rects[9];bound[9]=source;
    require(!d3dalloyRRFusesNormalRoughness(frame,bound),"packed-direct-normal-fallback");
    bound[9]=outputs[2];frame->props.roughness_packed=0;
    require(!d3dalloyRRFusesNormalRoughness(frame,bound),"unpacked-fallback");
    [frame release];[command release];[allocator release];[residency endResidency];[residency release];
    [table release];[buffer release];[source release];for(auto output:outputs)[output release];++cases;
  }
  [event release];[queue release];
  std::printf("OWNED_RR_PACKED_FUSION_PASS cases=%u original_precision=1 resampling_offsets=1 thin_extents=1 nonfinite_values=preserved outputs=bit_exact direct_normal_fallback=1\n",cases);
}
static void descriptorReuse(const std::shared_ptr<d3dalloy_rr::Core> &owner) {
  auto core = std::make_shared<d3dalloy_rr::Core>();
  core->device = [owner->device retain]; core->compiler = [owner->compiler retain];
  core->info.feature_flags = d3dalloy_rr::Flag | 2 | 8;
  WMTMetal4FXTemporalFrame p{}; p.input_width = p.input_height = 128; p.output_rect = {0,0,128,128};
  auto a = d3dalloy_rr::generation(*core,p); require(bool(a),"generation-A");
  p.specular_hit_distance = 1; auto b = d3dalloy_rr::generation(*core,p); require(bool(b),"generation-B");
  p.reactive = 1; auto c = d3dalloy_rr::generation(*core,p); require(bool(c),"generation-C");
  p.specular_hit_distance = p.reactive = 0; auto restored = d3dalloy_rr::generation(*core,p);
  require(restored == a && a != b && b != c && a != c,"A-B-C-A-descriptor-reuse");
  require(b->serial == a->serial + 1 && c->serial == b->serial + 1,"three-native-generation-creations");
  auto queue = [core->device newCommandQueue]; auto queue4 = [core->device newMTL4CommandQueue];
  auto event = [core->device newSharedEvent]; uint64_t value = 0;
  require(queue && queue4 && event,"native-queues");
  std::array<std::shared_ptr<d3dalloy_rr::Generation>,4> generations{a,b,c,restored};
  std::vector<float> firstA;
  for (unsigned mode = 0; mode < generations.size(); ++mode) {
    auto frame = [D3DALLOYRRFrame new]; frame->core = core; frame->generation = generations[mode];
    frame->props = p; frame->props.motion_scale_x = frame->props.motion_scale_y = 1;
    auto scaler = frame->generation->scaler;
    const MTLTextureUsage usage[d3dalloy_rr::Count] = {scaler.colorTextureUsage,scaler.depthTextureUsage,
      scaler.motionTextureUsage,scaler.outputTextureUsage,0,scaler.reactiveMaskTextureUsage,0,
      scaler.diffuseAlbedoTextureUsage,scaler.specularAlbedoTextureUsage,scaler.normalTextureUsage,
      scaler.roughnessTextureUsage,scaler.specularHitDistanceTextureUsage};
    for (size_t i : {0u,1u,2u,3u,7u,8u,9u,10u}) {
      auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:d3dalloy_rr::format(i) width:128 height:128 mipmapped:NO];
      d.storageMode = MTLStorageModePrivate; d.usage = usage[i] | MTLTextureUsageShaderRead;
      frame->inputs[i] = [core->device newTextureWithDescriptor:d]; require(frame->inputs[i] != nil,"required-input");
      frame->rects[i] = {0,0,128,128};
    }
    if (mode == 1 || mode == 2) {
      auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:d3dalloy_rr::format(11) width:128 height:128 mipmapped:NO];
      d.storageMode = MTLStorageModePrivate; d.usage = usage[11] | MTLTextureUsageShaderRead;
      frame->inputs[11] = [core->device newTextureWithDescriptor:d]; frame->rects[11] = {0,0,128,128};
    }
    if (mode == 2) {
      auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:d3dalloy_rr::format(5) width:128 height:128 mipmapped:NO];
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
    d3dalloy_rr::Resources resources; std::array<id<MTLTexture>,d3dalloy_rr::Count> bound{};
    require(d3dalloyRRResources(frame,resources,bound),"native-resource-selection");
    auto allocator = [core->device newCommandAllocator];
    auto readback = [core->device newBufferWithLength:128*128*8 options:MTLResourceStorageModeShared];
    require(!resources.residency && !resources.table && !resources.params,"direct-frame-no-helper-allocations");
    auto rd = [MTLResidencySetDescriptor new];
    auto appResidency = [core->device newResidencySetWithDescriptor:rd error:nil]; [rd release];
    require(appResidency != nil,"native-app-residency");
    for (auto texture : bound) if (texture) [appResidency addAllocation:texture];
    [appResidency commit];
    for (unsigned warm = 0; warm < 2; ++warm) {
      scaler.colorTexture=bound[0]; scaler.depthTexture=bound[1]; scaler.motionTexture=bound[2]; scaler.outputTexture=bound[3];
      scaler.diffuseAlbedoTexture=bound[7]; scaler.specularAlbedoTexture=bound[8]; scaler.normalTexture=bound[9];
      scaler.roughnessTexture=bound[10]; scaler.specularHitDistanceTexture=bound[11]; scaler.reactiveMaskTexture=bound[5];
      scaler.preExposure=1; scaler.depthReversed=YES; scaler.shouldResetHistory=!warm;
      scaler.motionVectorScaleX=scaler.motionVectorScaleY=1;
      scaler.jitterOffsetX=scene.jitterX; scaler.jitterOffsetY=scene.jitterY;
      simd_float4x4 world, projection; d3dalloy_rr::matrix(world,scene.world); d3dalloy_rr::matrix(projection,scene.projection);
      scaler.worldToViewMatrix=world; scaler.viewToClipMatrix=projection;
      auto command=[core->device newCommandBuffer]; [command beginCommandBufferWithAllocator:allocator];
      [command useResidencySet:appResidency]; [scaler encodeToCommandBuffer:command]; [command endCommandBuffer];
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
    [appResidency release]; [readback release]; [allocator release]; [frame release];
  }
  [event release]; [queue4 release]; [queue release];
  auto feature = [D3DALLOYRRFeature new]; feature->core = core;
  auto prepared = [D3DALLOYRRFrame new]; prepared->core = core; prepared->generation = a;
  std::weak_ptr<d3dalloy_rr::Generation> retained = a, idleB = b, idleC = c;
  a.reset(); b.reset(); c.reset(); restored.reset(); generations.fill(nullptr);
  struct WMTMetal4Call dormant{}; dormant.operation = WMTMetal4FXDormantTemporal;
  dormant.args[0] = uintptr_t(feature);
  require(d3dalloyRRCall(&dormant),"dormant-dispatch");
  for (const auto &cached : core->generations) require(!cached,"dormant-cache-empty");
  require(idleB.expired() && idleC.expired() && !retained.expired() &&
          prepared->generation->scaler != nil,"dormant-releases-only-idle-generations");
  auto execution = [D3DALLOYRRExecution new]; execution->frame = [prepared retain];
  [prepared release]; [feature release];
  require(!retained.expired() && execution->frame->generation->scaler != nil,"execution-retains-generation");
  [execution release];
  require(retained.expired(),"retired-generation-released");
  std::puts("OWNED_RR_DORMANT_PASS cache_empty=1 idle_released=2 prepared_retained=1 execution_retained=1 retired_released=1");
  std::puts("OWNED_RR_REUSE_PASS sequence=A,B,C,A creations=3 frames=8 version=65536");
}

int main() { @autoreleasepool {
  scratchPoolProof();
  auto frame = [D3DALLOYRRFrame new];
  frame->core = std::make_shared<d3dalloy_rr::Core>();
  auto &core = *frame->core;
  core.device = MTLCreateSystemDefaultDevice();
  auto descriptor = [MTL4CompilerDescriptor new];
  core.compiler = [core.device newCompilerWithDescriptor:descriptor error:nil]; [descriptor release];
  if (!core.device || !core.compiler) return 1;
  normalRoughnessProof(core.device);
  auto &p = frame->props;
  p.input_width = p.input_height = 128; p.output_rect = {0,0,128,128};
  p.motion_scale_x = p.motion_scale_y = 1;
  core.info.feature_flags = d3dalloy_rr::Flag | 2 | 8;
  frame->generation = d3dalloy_rr::generation(core,p);
  if (!frame->generation) return 1;
  const MTLTextureUsage usage = frame->generation->scaler.motionTextureUsage | MTLTextureUsageShaderRead;
  unsigned cases = 0;
  // Reuse the same retired set across conversion/direct mode switches.
  d3dalloy_rr::Resources resources;
  for (unsigned variant = 0; variant < 9; ++variant) {
    core.info.feature_flags = d3dalloy_rr::Flag | 2 | 8;
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
    auto formats = d3dalloy_rr::fixedFormats();
    formats[2] = d3dalloy_rr::descriptorFormat(core,p,2,frame->inputs[2],frame->rects[2]);
    frame->generation = d3dalloy_rr::generation(core,p,formats);
    if (!frame->generation) return 1;
    std::array<id<MTLTexture>,d3dalloy_rr::Count> bound{};
    const bool direct = variant == 0 || variant == 5 || variant == 8;
    if (!d3dalloyRRResources(frame,resources,bound) || (bound[2] == frame->inputs[2]) != direct ||
        (resources.textures[2] == nil) != direct || (resources.residency == nil) != direct ||
        (resources.params == nil) != direct || (resources.table == nil) != direct) {
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
