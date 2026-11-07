// SPDX-License-Identifier: LGPL-2.1-or-later
// NGX ISP has no history input. The feature timeline orders reuse of its
// native scaler and scratch storage across recorded lists and queues.
#include "metalfx_texture_access.hpp"
// MetalFX V1 binds an initializer-created constant buffer that is missing
// from its own residency set. Observe this factory invocation only.
@interface D3DalloySpatialDevice : NSObject {
@public
  id<MTLDevice> device;
  NSMutableArray<id<MTLBuffer>> *constants;
}
@end
@implementation D3DalloySpatialDevice
- (id)forwardingTargetForSelector:(SEL)selector { return device; }
- (BOOL)conformsToProtocol:(Protocol *)protocol { return [device conformsToProtocol:protocol]; }
- (BOOL)respondsToSelector:(SEL)selector {
  return [super respondsToSelector:selector] || [device respondsToSelector:selector];
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
  id<MTLBuffer> buffer = [device newBufferWithBytes:bytes length:length options:options];
  if (buffer) [constants addObject:buffer];
  return buffer;
}
- (void)dealloc { [device release]; [super dealloc]; }
@end

// Keep every encoder on the caller's real command buffer. Import each MetalFX
// declaration into the recording's existing set; Metal caps sets per buffer.
@interface D3DalloySpatialCommand : NSObject {
@public
  id<MTL4CommandBuffer> command;
  obj_handle_t recording;
}
@end
@implementation D3DalloySpatialCommand
- (id)forwardingTargetForSelector:(SEL)selector { return command; }
- (BOOL)conformsToProtocol:(Protocol *)protocol { return [command conformsToProtocol:protocol]; }
- (BOOL)respondsToSelector:(SEL)selector {
  return [super respondsToSelector:selector] || [command respondsToSelector:selector];
}
- (void)useResidencySet:(id<MTLResidencySet>)set {
  for (id<MTLAllocation> allocation in set.allAllocations)
    winemetal_metal4_use_allocation(recording, allocation);
}
- (void)useResidencySets:(const id<MTLResidencySet> *)sets count:(NSUInteger)count {
  for (NSUInteger i = 0; i < count; ++i) [self useResidencySet:sets[i]];
}
@end

@interface D3DalloySpatialGeneration : NSObject {
@public
  id<MTLFXSpatialScalerBase> scaler;
  NSArray<id<MTLBuffer>> *constants;
  id<MTLTexture> inputScratch, outputScratch;
  NSUInteger activeWidth, activeHeight;
  bool initializedInput, inlineFence;
}
@end
@implementation D3DalloySpatialGeneration
- (void)dealloc {
  [scaler release]; [constants release]; [inputScratch release]; [outputScratch release]; [super dealloc];
}
@end

@interface D3DalloySpatialFeature : NSObject {
@public
  id<MTLDevice> device;
  id<MTL4Compiler> compiler;
  id<MTLCommandQueue> queue;
  NSUInteger width, height;
  NSMutableArray<D3DalloySpatialGeneration *> *formats;
}
@end
@implementation D3DalloySpatialFeature
- (void)dealloc { [device release]; [compiler release]; [queue release]; [formats release]; [super dealloc]; }
@end

@interface D3DalloySpatialFrame : NSObject {
@public
  D3DalloySpatialGeneration *generation;
  id<MTLTexture> color, output;
  WMTMetal4FXRect rect;
  bool copyInput, copyOutput;
  id<MTLCommandQueue> queue;
}
@end
@implementation D3DalloySpatialFrame
- (void)dealloc { [generation release]; [color release]; [output release]; [queue release]; [super dealloc]; }
@end

static D3DalloySpatialGeneration *spatialFormat(D3DalloySpatialFeature *f, MTLPixelFormat input, MTLPixelFormat output,
                                                 NSUInteger width, NSUInteger height) {
  for (D3DalloySpatialGeneration *entry in f->formats)
    if (entry->activeWidth == width && entry->activeHeight == height &&
        entry->scaler.colorTextureFormat == input && entry->scaler.outputTextureFormat == output) return entry;
  auto desc = [[[MTLFXSpatialScalerDescriptor alloc] init] autorelease];
  desc.colorTextureFormat = input;
  desc.outputTextureFormat = output;
  desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
  desc.inputWidth = desc.outputWidth = f->width;
  desc.inputHeight = desc.outputHeight = f->height;
  auto entry = [[[D3DalloySpatialGeneration alloc] init] autorelease];
  if (f->compiler) {
    auto capture = [[[D3DalloySpatialDevice alloc] init] autorelease];
    capture->device = [f->device retain];
    capture->constants = [NSMutableArray array];
    @try {
      entry->scaler = [desc newSpatialScalerWithDevice:(id<MTLDevice>)capture compiler:f->compiler];
      if (entry->scaler) entry->constants = [capture->constants copy];
    } @catch (NSException *) {
      [entry->scaler release]; entry->scaler = nil;
    } @finally { capture->constants = nil; }
  }
  if (!entry->scaler) {
    if (!f->queue) f->queue = [f->device newCommandQueue];
    if (!f->queue) return nil;
    entry->scaler = [desc newSpatialScalerWithDevice:f->device];
  }
  if (!entry->scaler) return nil;
  if (@available(macOS 27.0, *))
    entry->inlineFence = entry->constants && [f->device supportsFamily:MTLGPUFamilyApple10];
  // Dynamic extents reuse MetalFX's CPU-written constants ring even while a
  // previous command buffer holds the slot. Keep each generation's values fixed.
  entry->scaler.inputContentWidth = entry->activeWidth = width;
  entry->scaler.inputContentHeight = entry->activeHeight = height;
  if (f->formats.count == 4) [f->formats removeObjectAtIndex:0];
  [f->formats addObject:entry];
  return entry;
}

static id<MTLTexture> spatialScratch(D3DalloySpatialFeature *f, MTLPixelFormat format, MTLTextureUsage usage) {
  auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
      width:f->width height:f->height mipmapped:NO];
  desc.storageMode = MTLStorageModePrivate;
  desc.usage = usage;
  return [f->device newTextureWithDescriptor:desc];
}

static uint32_t prepareSpatial(D3DalloySpatialFeature *feature, const WMTMetal4FXTemporalFrame &p, uint64_t &result) {
  id<MTLTexture> color = (id<MTLTexture>)p.color, output = (id<MTLTexture>)p.output;
  const auto &r = p.color_rect;
  if (!color || !output || color == output || color.device != feature->device || output.device != feature->device ||
      !r.width || !r.height || r.width > feature->width || r.height > feature->height ||
      uint64_t(r.x) + r.width > color.width || uint64_t(r.y) + r.height > color.height ||
      uint64_t(r.x) + feature->width > output.width || uint64_t(r.y) + feature->height > output.height)
    return 0x80070057;
  @synchronized (feature) {
    auto entry = spatialFormat(feature, color.pixelFormat, output.pixelFormat, r.width, r.height);
    if (!entry) return 0x887a0004;
    auto scaler = entry->scaler;
    const bool copyInput = r.x || r.y || color.width != feature->width || color.height != feature->height ||
        (color.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage ||
        !d3dalloy::metalfx::texturesDisjoint(color, output);
    const bool copyOutput = r.x || r.y || output.width != feature->width || output.height != feature->height ||
        output.storageMode != MTLStorageModePrivate || (output.usage & scaler.outputTextureUsage) != scaler.outputTextureUsage;
    if (copyInput && !entry->inputScratch)
      entry->inputScratch = spatialScratch(feature, color.pixelFormat, scaler.colorTextureUsage | MTLTextureUsageRenderTarget);
    if (copyOutput && !entry->outputScratch)
      entry->outputScratch = spatialScratch(feature, output.pixelFormat, scaler.outputTextureUsage);
    if ((copyInput && !entry->inputScratch) || (copyOutput && !entry->outputScratch)) return 0x8007000e;
    auto frame = [D3DalloySpatialFrame new];
    frame->queue = entry->constants ? nil : [feature->queue retain]; frame->generation = [entry retain]; frame->color = [color retain]; frame->output = [output retain];
    frame->rect = r; frame->copyInput = copyInput; frame->copyOutput = copyOutput;
    result = reinterpret_cast<uintptr_t>(frame);
    return 0;
  }
}

template<class Command>
static BOOL encodeSpatial(D3DalloySpatialFrame *frame, Command command, id<MTLFence> fence = nil,
                          obj_handle_t recording = 0, uint32_t handoff = 0) {
  constexpr bool metal4 = std::is_same_v<Command, id<MTL4CommandBuffer>>;
  auto entry = frame->generation;
  auto scaler = entry->scaler;
  const auto &r = frame->rect;
  const auto origin = MTLOriginMake(r.x, r.y, 0), zero = MTLOriginMake(0, 0, 0);
  if (frame->copyInput && !entry->initializedInput && (r.width != scaler.inputWidth || r.height != scaler.inputHeight)) {
    auto clear = [] {
      if constexpr (metal4) return [[[MTL4RenderPassDescriptor alloc] init] autorelease];
      else return [MTLRenderPassDescriptor renderPassDescriptor];
    }();
    auto attachment = clear.colorAttachments[0];
    attachment.texture = entry->inputScratch;
    attachment.loadAction = MTLLoadActionClear;
    attachment.storeAction = MTLStoreActionStore;
    attachment.clearColor = MTLClearColorMake(0, 0, 0, 0);
    auto encoder = [command renderCommandEncoderWithDescriptor:clear];
    if (!encoder) return NO;
    if constexpr (metal4)
      [encoder barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [encoder endEncoding];
    m4_hud_count(WMTMetal4HUDRender);
  }
  auto copyEncoder = [&] {
    if constexpr (metal4) return [command computeCommandEncoder];
    else return [command blitCommandEncoder];
  };
  if (frame->copyInput || (metal4 && !(handoff & WMTMetal4FXProducer))) {
    auto blit = copyEncoder();
    if (!blit) return NO;
    const auto stage = frame->copyInput ? MTLStageBlit : MTLStageDispatch;
    if constexpr (metal4)
      [blit barrierAfterQueueStages:MTLStageAll beforeStages:stage visibilityOptions:MTL4VisibilityOptionDevice];
    if (frame->copyInput)
      [blit copyFromTexture:frame->color sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:MTLSizeMake(r.width,r.height,1)
                toTexture:entry->inputScratch destinationSlice:0 destinationLevel:0 destinationOrigin:zero];
    if constexpr (metal4) [blit updateFence:fence afterEncoderStages:stage];
    [blit endEncoding];
    m4_hud_count(frame->copyInput ? WMTMetal4HUDBlit : WMTMetal4HUDCompute);
  }
  scaler.colorTexture = frame->copyInput ? entry->inputScratch : frame->color;
  scaler.outputTexture = frame->copyOutput ? entry->outputScratch : frame->output;
  @try {
    if constexpr (metal4) {
      scaler.fence = fence;
      auto scope = [[[D3DalloySpatialCommand alloc] init] autorelease];
      scope->command = command; scope->recording = recording;
      [(id<MTL4FXSpatialScaler>)scaler encodeToCommandBuffer:(id<MTL4CommandBuffer>)scope];
    } else [(id<MTLFXSpatialScaler>)scaler encodeToCommandBuffer:command];
  }
  @finally { scaler.fence = nil; scaler.colorTexture = nil; scaler.outputTexture = nil; }
  if (frame->copyOutput || (metal4 && !(handoff & WMTMetal4FXConsumer))) {
    auto blit = copyEncoder();
    if (!blit) return NO;
    const auto stage = frame->copyOutput ? MTLStageBlit : MTLStageDispatch;
    if constexpr (metal4) [blit waitForFence:fence beforeEncoderStages:stage];
    if (frame->copyOutput)
      [blit copyFromTexture:entry->outputScratch sourceSlice:0 sourceLevel:0 sourceOrigin:zero
               sourceSize:MTLSizeMake(scaler.outputWidth,scaler.outputHeight,1)
                toTexture:frame->output destinationSlice:0 destinationLevel:0 destinationOrigin:origin];
    if constexpr (metal4) {
      [blit barrierAfterStages:stage beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
      if (frame->copyOutput) [blit updateFence:fence afterEncoderStages:stage];
    }
    [blit endEncoding];
    m4_hud_count(frame->copyOutput ? WMTMetal4HUDBlit : WMTMetal4HUDCompute);
  }
  if (frame->copyInput) entry->initializedInput = true;
  return YES;
}

static void spatialCall(struct WMTMetal4Call *call) {
  if (call->operation == WMTMetal4FXCreateSpatial) {
    const auto &p = *reinterpret_cast<const WMTMetal4FXTemporalInfo *>(call->args[2]);
    auto feature = [D3DalloySpatialFeature new];
    feature->device = [(id<MTLDevice>)call->args[0] retain];
    if ([MTLFXSpatialScalerDescriptor supportsMetal4FX:feature->device])
      feature->compiler = [(id<MTL4Compiler>)call->args[1] retain];
    feature->width = p.output_width; feature->height = p.output_height;
    feature->formats = [NSMutableArray new];
    call->ret[0] = reinterpret_cast<uintptr_t>(feature);
  } else if (call->operation == WMTMetal4FXPrepareSpatial) {
    call->ret[1] = prepareSpatial((D3DalloySpatialFeature *)call->args[0],
        *reinterpret_cast<const WMTMetal4FXTemporalFrame *>(call->args[1]), call->ret[0]);
  } else {
    auto frame = (D3DalloySpatialFrame *)call->args[1];
    auto entry = frame->generation;
    if (entry->constants) {
      auto handle = call->args[0];
      auto command = winemetal_metal4_command_buffer(handle);
      auto fence = (id<MTLFence>)call->args[2];
      winemetal_metal4_retain_resource(handle, frame);
      winemetal_metal4_retain_resource(handle, fence);
      winemetal_metal4_use_allocation(handle, frame->color);
      winemetal_metal4_use_allocation(handle, frame->output);
      if (frame->copyInput) winemetal_metal4_use_allocation(handle, entry->inputScratch);
      if (frame->copyOutput) winemetal_metal4_use_allocation(handle, entry->outputScratch);
      for (id<MTLBuffer> buffer in entry->constants) winemetal_metal4_use_allocation(handle, buffer);
      const uint32_t handoff = entry->inlineFence ? uint32_t(call->args[4]) &
          ((frame->copyInput ? 0u : WMTMetal4FXProducer) | (frame->copyOutput ? 0u : WMTMetal4FXConsumer)) : 0;
      call->ret[1] = encodeSpatial(frame, command, fence, handle, handoff) ? 0 : 0x887a0020;
      call->ret[3] = handoff;
    } else {
      // The classic scaler runs after the preceding Metal 4 segment signals
      // its shared event. That orders all queue stages, including feature history.
      call->ret[1] = winemetal_metal4_add_classic_segment(call->args[0], frame->queue,
          ^BOOL(id<MTLCommandBuffer> command) { return encodeSpatial(frame, command); }) ? 0 : 0x887a0020;
    }
  }
}
