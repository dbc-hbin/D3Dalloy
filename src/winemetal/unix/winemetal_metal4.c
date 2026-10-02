#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <string.h>
#define WINEMETAL_API
#include "../winemetal_metal4.h"
#include "rt_acceleration_structure.h"
#include "airconv_public.h"

#define M4_SUCCESS 0
#define M4_INVALID ((int32_t)0xc000000d)
#define M4_FAILED ((int32_t)0xc0000001)
#define M4_UNSUPPORTED ((int32_t)0xc0000002)

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return (MTLPixelFormat)ORIGINAL_FORMAT(format);
}

@class DXMTM4CommandBuffer;
@interface DXMTM4Recording : NSObject {
@public
  id<MTLResidencySet> residency;
  NSMutableSet *objects;
  NSMutableArray *encoders;
  NSMutableArray<id<MTL4CommandBuffer>> *segments;
  NSError *error;
}
@end
@implementation DXMTM4Recording
- (void)dealloc {
  [residency release]; [objects release]; [encoders release]; [segments release]; [error release]; [super dealloc];
}
@end

@interface DXMTM4CommandBuffer : NSObject {
@public
  id<MTLDevice> device;
  id<MTL4CommandBuffer> command;
  id<MTL4CommandAllocator> allocator;
  DXMTM4Recording *recording;
  NSMutableArray<id<MTLBuffer>> *uploads;
  NSUInteger uploadIndex, uploadOffset;
  _Atomic(uint32_t) inFlight;
  BOOL ended;
}
@end
@implementation DXMTM4CommandBuffer
- (void)dealloc {
  [device release]; [command release]; [allocator release]; [recording release]; [uploads release]; [super dealloc];
}
@end

@interface DXMTM4Encoder : NSObject {
@public
  DXMTM4CommandBuffer *owner;
  DXMTM4Recording *recording;
  id<MTL4CommandEncoder> encoder;
  id<MTL4ArgumentTable> tables[4];
  id<MTLBuffer> buffers[4][31];
  uint64_t offsets[4][31];
  MTLStages orderedFor[3];
  MTLStages fenceWaitStages;
}
@end
@implementation DXMTM4Encoder
- (void)dealloc {
  [encoder release]; for (unsigned i=0;i<4;++i) [tables[i] release]; [super dealloc];
}
@end
@interface DXMTM4ComputeEncoder : DXMTM4Encoder {
@public
  MTLSize threadgroupSize;
  BOOL concurrent;
  MTLStages pendingStages;
  struct { uint64_t resource, begin, end; BOOL write; } blitAccesses[32];
  unsigned blitAccessCount;
}
@end
@implementation DXMTM4ComputeEncoder
@end
@interface DXMTM4RenderEncoder : DXMTM4Encoder
@end
@implementation DXMTM4RenderEncoder
@end

@interface DXMTM4Submission : NSObject {
@public
  NSCondition *condition;
  NSMutableArray *leases;
  NSMutableArray<DXMTM4CommandBuffer *> *buffers;
  NSError *error;
  uint64_t gpuStart, gpuEnd;
  BOOL completed, commitReturned, feedbackReceived;
}
@end
@implementation DXMTM4Submission
- (void)dealloc {
  [condition release]; [leases release]; [buffers release]; [error release]; [super dealloc];
}
@end

@interface DXMTM4ASDescriptor : NSObject {
@public
  MTL4AccelerationStructureDescriptor *descriptor;
  NSMutableArray *geometries;
  NSMutableSet *inputs;
  id<MTLBuffer> placeholder;
  NSMutableIndexSet *unresolvedGeometries;
  BOOL unresolvedInstances;
}
@end
@implementation DXMTM4ASDescriptor
- (void)dealloc {
  [descriptor release]; [geometries release]; [inputs release]; [placeholder release]; [unresolvedGeometries release]; [super dealloc];
}
@end

static inline id m4obj(uint64_t handle) { return (id)(uintptr_t)handle; }
static inline DXMTM4CommandBuffer *m4cb(uint64_t handle) { return (DXMTM4CommandBuffer *)m4obj(handle); }
static void m4_error(DXMTM4Recording *recording, NSString *description) {
  if (!recording->error)
    recording->error=[[NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorInternal
      userInfo:@{NSLocalizedDescriptionKey:description}] retain];
}
static void m4_retain(DXMTM4Recording *recording, id object) {
  if (object) [recording->objects addObject:object];
}
static void m4_allocation(DXMTM4Recording *recording, id<MTLAllocation> allocation) {
  if (allocation) { [recording->objects addObject:allocation]; [recording->residency addAllocation:allocation]; }
}

id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t handle) { return handle ? m4cb(handle)->command : nil; }
void *winemetal_metal4_split_command_buffer(void *opaque) {
  DXMTM4CommandBuffer *context = opaque;
  if (!context || !context->recording || context->ended) return NULL;
  id<MTL4CommandBuffer> next = [context->device newCommandBuffer];
  if (!next) return NULL;
  // All segments share the existing recording, allocator and retirement owner.
  if (!context->recording->segments) context->recording->segments = [NSMutableArray new];
  [context->command useResidencySet:context->recording->residency];
  [context->command endCommandBuffer];
  [context->recording->segments addObject:context->command];
  [context->command release]; context->command = next;
  [next beginCommandBufferWithAllocator:context->allocator];
  return next;
}
id<MTL4ComputeCommandEncoder> winemetal_metal4_compute_encoder(obj_handle_t handle) {
  return handle ? (id<MTL4ComputeCommandEncoder>)((DXMTM4ComputeEncoder *)m4obj(handle))->encoder : nil;
}
id<MTL4RenderCommandEncoder> winemetal_metal4_render_encoder(obj_handle_t handle) {
  return handle ? (id<MTL4RenderCommandEncoder>)((DXMTM4RenderEncoder *)m4obj(handle))->encoder : nil;
}
void winemetal_metal4_retain_resource(obj_handle_t handle, id resource) { m4_retain(m4cb(handle)->recording,resource); }
void winemetal_metal4_use_allocation(obj_handle_t handle, id<MTLAllocation> resource) { m4_allocation(m4cb(handle)->recording,resource); }

static id<MTL4ArgumentTable> m4_table(DXMTM4Encoder *context, unsigned stage) {
  if (!context->tables[stage]) {
    MTL4ArgumentTableDescriptor *descriptor=[MTL4ArgumentTableDescriptor new];
    descriptor.maxBufferBindCount=31; descriptor.maxTextureBindCount=128; descriptor.maxSamplerStateBindCount=16;
    descriptor.initializeBindings=YES;
    context->tables[stage]=[context->owner->device newArgumentTableWithDescriptor:descriptor error:nil];
    [descriptor release];
    if (!context->tables[stage]) m4_error(context->recording,@"Metal4 argument table allocation failed");
  }
  return context->tables[stage];
}
static const MTLRenderStages m4_render_stages[4]={MTLRenderStageVertex,MTLRenderStageFragment,MTLRenderStageMesh,MTLRenderStageObject};
static void m4_render_table(DXMTM4RenderEncoder *context, unsigned stage) {
  [(id<MTL4RenderCommandEncoder>)context->encoder setArgumentTable:m4_table(context,stage) atStages:m4_render_stages[stage]];
}
static void m4_compute_table(DXMTM4ComputeEncoder *context) {
  [(id<MTL4ComputeCommandEncoder>)context->encoder setArgumentTable:m4_table(context,0)];
}
static void m4_record_barrier(DXMTM4Encoder *context, MTLStages after, MTLStages before) {
  if (before & MTLStageDispatch) context->orderedFor[0] |= after;
  if (before & MTLStageBlit) context->orderedFor[1] |= after;
  if (before & MTLStageAccelerationStructure) context->orderedFor[2] |= after;
}
static void m4_stage_barrier(DXMTM4ComputeEncoder *context, MTLStages stage) {
  /* The SDK's combined encoder has no serial selector. Dispatch/AS accesses
   * may be bindless, so a serial pass orders all prior memory stages. Copy
   * accesses are known; independent copies keep Metal4's native concurrency. */
  if (!context->concurrent) {
    unsigned target=stage==MTLStageDispatch ? 0 : stage==MTLStageBlit ? 1 : 2;
    MTLStages after=context->pendingStages & ~context->orderedFor[target];
    if (stage==MTLStageBlit) {
      if (context->orderedFor[1] & MTLStageBlit) context->blitAccessCount=0;
      after &= ~MTLStageBlit;
    }
    if (after) {
      [context->encoder barrierAfterEncoderStages:after beforeEncoderStages:stage visibilityOptions:MTL4VisibilityOptionDevice];
      m4_record_barrier(context,after,stage);
      if (stage==MTLStageBlit) context->blitAccessCount=0;
    }
  }
  context->pendingStages |= stage;
  for (unsigned i=0;i<3;++i) context->orderedFor[i] &= ~stage;
}
static uint64_t m4_texture_key(id<MTLTexture> texture) {
  while (texture.parentTexture) texture=texture.parentTexture;
  return texture.buffer ? (uint64_t)texture.buffer : (uint64_t)texture;
}
static void m4_blit_access(DXMTM4ComputeEncoder *context, uint64_t source, uint64_t sourceOffset, uint64_t sourceLength,
                          uint64_t destination, uint64_t destinationOffset, uint64_t destinationLength) {
  m4_stage_barrier(context,MTLStageBlit);
  typeof(context->blitAccesses[0]) accesses[2]={
    {source,sourceOffset,sourceLength>UINT64_MAX-sourceOffset ? UINT64_MAX : sourceOffset+sourceLength,NO},
    {destination,destinationOffset,destinationLength>UINT64_MAX-destinationOffset ? UINT64_MAX : destinationOffset+destinationLength,YES}
  };
  BOOL conflict=context->blitAccessCount>30;
  for (unsigned j=0;j<2 && !conflict;++j)
    for (unsigned i=0;i<context->blitAccessCount && !conflict;++i)
      conflict=accesses[j].resource && accesses[j].resource==context->blitAccesses[i].resource &&
        (accesses[j].write || context->blitAccesses[i].write) && accesses[j].begin<context->blitAccesses[i].end && context->blitAccesses[i].begin<accesses[j].end;
  if (conflict) {
    [context->encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    context->blitAccessCount=0;
  }
  for (unsigned j=0;j<2;++j)
    if (accesses[j].resource && accesses[j].begin<accesses[j].end) context->blitAccesses[context->blitAccessCount++]=accesses[j];
}
static void m4_bind_buffer(DXMTM4Encoder *context, unsigned stage, id<MTLBuffer> buffer, uint64_t offset, unsigned index) {
  if (index>=31 || (buffer && offset>buffer.length)) { m4_error(context->recording,@"Metal4 buffer binding is out of range"); return; }
  context->buffers[stage][index]=buffer; context->offsets[stage][index]=offset;
  m4_allocation(context->recording,buffer);
  [m4_table(context,stage) setAddress:buffer ? buffer.gpuAddress+offset : 0 atIndex:index];
}
static void m4_render_buffer(DXMTM4RenderEncoder *context, unsigned stage, id<MTLBuffer> buffer, uint64_t offset, unsigned index) {
  m4_bind_buffer(context,stage,buffer,offset,index); m4_render_table(context,stage);
}
static void m4_render_offset(DXMTM4RenderEncoder *context, unsigned stage, uint64_t offset, unsigned index) {
  if (index>=31) { m4_error(context->recording,@"Metal4 buffer offset index is out of range"); return; }
  m4_render_buffer(context,stage,context->buffers[stage][index],offset,index);
}
static void m4_compute_buffer(DXMTM4ComputeEncoder *context, id<MTLBuffer> buffer, uint64_t offset, unsigned index) {
  m4_bind_buffer(context,0,buffer,offset,index); m4_compute_table(context);
}
static id<MTLBuffer> m4_upload(DXMTM4CommandBuffer *context, const void *bytes, uint64_t length, NSUInteger *offset) {
  if (!length || !bytes || length>NSUIntegerMax-255) { m4_error(context->recording,@"Invalid Metal4 inline constant data"); return nil; }
  const NSUInteger aligned=(length+255)&~(NSUInteger)255;
  for (;;) {
    if (context->uploadIndex>=context->uploads.count) {
      id<MTLBuffer> buffer=[context->device newBufferWithLength:MAX(aligned,65536) options:MTLResourceStorageModeShared];
      if (!buffer) { m4_error(context->recording,@"Metal4 inline constant upload allocation failed"); return nil; }
      [context->uploads addObject:buffer]; [buffer release];
    }
    id<MTLBuffer> buffer=context->uploads[context->uploadIndex];
    if (aligned<=buffer.length-context->uploadOffset) {
      *offset=context->uploadOffset; memcpy((char *)buffer.contents+*offset,bytes,length);
      context->uploadOffset+=aligned; m4_allocation(context->recording,buffer); return buffer;
    }
    ++context->uploadIndex; context->uploadOffset=0;
  }
}
static void m4_render_bytes(DXMTM4RenderEncoder *context, const void *bytes, uint64_t length, unsigned index) {
  NSUInteger offset=0; id<MTLBuffer> buffer=m4_upload(context->owner,bytes,length,&offset);
  if (buffer) m4_render_buffer(context,1,buffer,offset,index);
}
static MTLGPUAddress m4_address(DXMTM4Recording *recording, uint64_t handle, uint64_t offset) {
  id<MTLBuffer> buffer=m4obj(handle); m4_allocation(recording,buffer);
  if (!buffer || offset>buffer.length) { m4_error(recording,@"Metal4 indirect/index buffer is out of range"); return 0; }
  return buffer.gpuAddress+offset;
}
static NSUInteger m4_index_length(DXMTM4Recording *recording, uint64_t handle, uint64_t offset) {
  id<MTLBuffer> buffer=m4obj(handle);
  if (!buffer || offset>buffer.length) { m4_error(recording,@"Metal4 index buffer offset is out of range"); return 0; }
  return buffer.length-offset;
}

static MTL4RenderPassDescriptor *m4_render_pass(DXMTM4CommandBuffer *context, const struct WMTRenderPassInfo *info) {
  MTL4RenderPassDescriptor *descriptor=[MTL4RenderPassDescriptor new];
  for (unsigned i=0;i<8;++i) {
    const struct WMTColorAttachmentInfo *src=&info->colors[i];
    MTLRenderPassColorAttachmentDescriptor *dst=descriptor.colorAttachments[i];
    dst.texture=m4obj(src->texture); dst.level=src->level; dst.slice=src->slice; dst.depthPlane=src->depth_plane;
    dst.clearColor=MTLClearColorMake(src->clear_color.r,src->clear_color.g,src->clear_color.b,src->clear_color.a);
    dst.loadAction=(MTLLoadAction)src->load_action; dst.storeAction=(MTLStoreAction)src->store_action;
    dst.resolveTexture=m4obj(src->resolve_texture); dst.resolveLevel=src->resolve_level; dst.resolveSlice=src->resolve_slice;
    dst.resolveDepthPlane=src->resolve_depth_plane;
    m4_allocation(context->recording,dst.texture); m4_allocation(context->recording,dst.resolveTexture);
  }
  descriptor.depthAttachment.texture=m4obj(info->depth.texture); descriptor.depthAttachment.level=info->depth.level;
  descriptor.depthAttachment.slice=info->depth.slice; descriptor.depthAttachment.depthPlane=info->depth.depth_plane;
  descriptor.depthAttachment.clearDepth=info->depth.clear_depth; descriptor.depthAttachment.loadAction=(MTLLoadAction)info->depth.load_action;
  descriptor.depthAttachment.storeAction=(MTLStoreAction)info->depth.store_action;
  descriptor.stencilAttachment.texture=m4obj(info->stencil.texture); descriptor.stencilAttachment.level=info->stencil.level;
  descriptor.stencilAttachment.slice=info->stencil.slice; descriptor.stencilAttachment.depthPlane=info->stencil.depth_plane;
  descriptor.stencilAttachment.clearStencil=info->stencil.clear_stencil; descriptor.stencilAttachment.loadAction=(MTLLoadAction)info->stencil.load_action;
  descriptor.stencilAttachment.storeAction=(MTLStoreAction)info->stencil.store_action;
  descriptor.defaultRasterSampleCount=info->default_raster_sample_count;
  descriptor.renderTargetArrayLength=info->render_target_array_length|((NSUInteger)info->render_target_array_length_high<<8);
  descriptor.renderTargetWidth=info->render_target_width; descriptor.renderTargetHeight=info->render_target_height;
  descriptor.visibilityResultBuffer=m4obj(info->visibility_buffer);
  descriptor.visibilityResultType=MTLVisibilityResultTypeReset;
  m4_allocation(context->recording,descriptor.depthAttachment.texture); m4_allocation(context->recording,descriptor.stencilAttachment.texture);
  m4_allocation(context->recording,descriptor.visibilityResultBuffer);
  if (info->tile_width && info->tile_height) { descriptor.tileWidth=info->tile_width; descriptor.tileHeight=info->tile_height; }
  if (info->center_sample_pattern) {
    NSUInteger count=info->default_raster_sample_count;
    for (unsigned i=0;i<8 && !count;++i) count=[descriptor.colorAttachments[i].texture sampleCount];
    if (!count) count=[descriptor.depthAttachment.texture sampleCount];
    if (!count) count=[descriptor.stencilAttachment.texture sampleCount];
    if (count!=2 && count!=4 && count!=8) { [descriptor release]; m4_error(context->recording,@"Invalid center sample count"); return nil; }
    MTLSamplePosition positions[8]; for (NSUInteger i=0;i<count;++i) positions[i]=MTLSamplePositionMake(.5f,.5f);
    [descriptor setSamplePositions:positions count:count];
  }
  if (info->forced16_half) {
    if (info->forced16_half>2 || info->default_raster_sample_count!=8 || info->center_sample_pattern) {
      [descriptor release]; m4_error(context->recording,@"Invalid forced-16 sample half"); return nil;
    }
    static const int x[16]={1,-1,-3,4,-5,2,5,3,-2,0,-4,-6,-8,7,6,-7};
    static const int y[16]={1,-3,2,-1,-2,5,3,-5,6,-7,-6,4,0,-4,7,-8};
    MTLSamplePosition positions[8]; const unsigned base=(info->forced16_half-1)*8;
    for (unsigned i=0;i<8;++i) positions[i]=MTLSamplePositionMake(.5f+x[base+i]/16.f,.5f+y[base+i]/16.f);
    [descriptor setSamplePositions:positions count:8];
  }
  return descriptor;
}


static int32_t m4_compute_commands(DXMTM4ComputeEncoder *context, const struct wmtcmd_base *next);
static int32_t m4_render_commands(DXMTM4RenderEncoder *context, const struct wmtcmd_base *next);
static int32_t m4_blit_commands(DXMTM4ComputeEncoder *context, const struct wmtcmd_base *next);

static MTL4BufferRange m4_buffer_range(id<MTLBuffer> buffer, uint64_t offset) {
  return buffer && offset<=buffer.length ? MTL4BufferRangeMake(buffer.gpuAddress+offset,buffer.length-offset) : MTL4BufferRangeMake(0,0);
}
static DXMTM4ASDescriptor *m4_as_new(id<MTLDevice> device, BOOL topLevel, uint32_t geometryCount) {
  DXMTM4ASDescriptor *owner=[DXMTM4ASDescriptor new];
  owner->inputs=[NSMutableSet new];
  owner->placeholder=[device newBufferWithLength:256 options:MTLResourceStorageModePrivate];
  if (!owner->placeholder) { [owner release]; return nil; }
  [owner->inputs addObject:owner->placeholder];
  if (topLevel) owner->descriptor=[MTL4InstanceAccelerationStructureDescriptor new];
  else {
    owner->descriptor=[MTL4PrimitiveAccelerationStructureDescriptor new];
    owner->geometries=[[NSMutableArray alloc] initWithCapacity:MAX(geometryCount,1u)];
    for (uint32_t i=0;i<geometryCount;++i) [owner->geometries addObject:[NSNull null]];
    if (!geometryCount) {
      MTL4AccelerationStructureTriangleGeometryDescriptor *empty=[MTL4AccelerationStructureTriangleGeometryDescriptor new];
      empty.vertexBuffer=m4_buffer_range(owner->placeholder,0); empty.vertexStride=12; empty.triangleCount=0;
      [owner->geometries addObject:empty]; [empty release];
    }
    ((MTL4PrimitiveAccelerationStructureDescriptor *)owner->descriptor).geometryDescriptors=owner->geometries;
  }
  return owner;
}
static void m4_as_input(DXMTM4ASDescriptor *owner, id buffer) { if (buffer) [owner->inputs addObject:buffer]; }
static void m4_as_unresolved(DXMTM4ASDescriptor *owner, NSUInteger geometry, BOOL unresolved) {
  if (unresolved) {
    if (!owner->unresolvedGeometries) owner->unresolvedGeometries=[NSMutableIndexSet new];
    [owner->unresolvedGeometries addIndex:geometry];
  } else [owner->unresolvedGeometries removeIndex:geometry];
}
static void m4_as_resources(DXMTM4Recording *recording, DXMTM4ASDescriptor *descriptor, uint64_t sidecar) {
  if (descriptor) {
    m4_retain(recording,descriptor);
    for (id<MTLAllocation> allocation in descriptor->inputs) m4_allocation(recording,allocation);
  }
  if (sidecar) {
    m4_retain(recording,m4obj(sidecar));
    NSArray *generations=m4obj(dxmt_as_sidecar_generations(sidecar));
    for (id<MTLAllocation> allocation in generations) m4_allocation(recording,allocation);
    m4_allocation(recording,m4obj(dxmt_as_sidecar_acceleration_structure(sidecar)));
    m4_allocation(recording,m4obj(dxmt_as_sidecar_instance_buffer(sidecar)));
    m4_allocation(recording,m4obj(dxmt_as_sidecar_contribution_buffer(sidecar)));
  }
}
static int32_t m4_as_call(struct WMTMetal4Call *call) {
  uint64_t *a=call->args;
  DXMTM4ASDescriptor *descriptor=(DXMTM4ASDescriptor *)m4obj(a[0]);
  switch (call->operation) {
  case WMTMetal4NewASDescriptor:
    call->ret[0]=(uint64_t)m4_as_new(m4obj(a[0]),a[1]!=0,(uint32_t)a[2]); return M4_SUCCESS;
  case WMTMetal4ASUsage:
    descriptor->descriptor.usage=(MTLAccelerationStructureUsage)a[1]; return M4_SUCCESS;
  case WMTMetal4ASTriangles: {
    if (!descriptor->geometries || a[1]>=descriptor->geometries.count) return M4_INVALID;
    id<MTLBuffer> vertices=m4obj(a[2]),indices=m4obj(a[5]),transform=m4obj(a[8]);
    if ((vertices && a[3]>vertices.length) || (indices && a[6]>indices.length) || (transform && a[9]>transform.length)) return M4_INVALID;
    MTL4AccelerationStructureTriangleGeometryDescriptor *geometry=[MTL4AccelerationStructureTriangleGeometryDescriptor new];
    geometry.vertexBuffer=m4_buffer_range(vertices?:descriptor->placeholder,vertices?a[3]:0); geometry.vertexStride=a[4];
    geometry.vertexFormat=(MTLAttributeFormat)a[13];
    if (a[7]!=UINT32_MAX && a[10]) { geometry.indexBuffer=m4_buffer_range(indices,a[6]); geometry.indexType=(MTLIndexType)a[7]; }
    geometry.transformationMatrixBuffer=m4_buffer_range(transform,a[9]); geometry.triangleCount=a[10];
    geometry.transformationMatrixLayout=MTLMatrixLayoutRowMajor;
    geometry.intersectionFunctionTableOffset=a[11]; geometry.opaque=(a[12]&1)!=0; geometry.allowDuplicateIntersectionFunctionInvocation=(a[12]&2)==0;
    descriptor->geometries[a[1]]=geometry; [geometry release];
    m4_as_unresolved(descriptor,a[1],!vertices && a[10]);
    m4_as_input(descriptor,vertices); m4_as_input(descriptor,indices); m4_as_input(descriptor,transform);
    call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASBoxes: {
    if (!descriptor->geometries || a[1]>=descriptor->geometries.count) return M4_INVALID;
    id<MTLBuffer> buffer=m4obj(a[2]); if (buffer && a[3]>buffer.length) return M4_INVALID;
    MTL4AccelerationStructureBoundingBoxGeometryDescriptor *geometry=[MTL4AccelerationStructureBoundingBoxGeometryDescriptor new];
    geometry.boundingBoxBuffer=m4_buffer_range(buffer?:descriptor->placeholder,buffer?a[3]:0); geometry.boundingBoxStride=a[4]; geometry.boundingBoxCount=a[5];
    geometry.intersectionFunctionTableOffset=a[6]; geometry.opaque=(a[7]&1)!=0; geometry.allowDuplicateIntersectionFunctionInvocation=(a[7]&2)==0;
    descriptor->geometries[a[1]]=geometry; [geometry release]; m4_as_unresolved(descriptor,a[1],!buffer && a[5]);
    m4_as_input(descriptor,buffer); call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASInstances: {
    if (descriptor->geometries) return M4_INVALID;
    id<MTLBuffer> buffer=m4obj(a[1]); if (buffer && a[2]>buffer.length) return M4_INVALID;
    MTL4InstanceAccelerationStructureDescriptor *instances=(MTL4InstanceAccelerationStructureDescriptor *)descriptor->descriptor;
    instances.instanceDescriptorBuffer=m4_buffer_range(buffer?:descriptor->placeholder,buffer?a[2]:0);
    instances.instanceDescriptorStride=sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
    instances.instanceDescriptorType=MTLAccelerationStructureInstanceDescriptorTypeIndirect; instances.instanceCount=a[3];
    descriptor->unresolvedInstances=!buffer && a[3];
    m4_as_input(descriptor,buffer); call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASSizes: {
    descriptor=(DXMTM4ASDescriptor *)m4obj(a[1]);
    MTLAccelerationStructureSizes sizes=[(id<MTLDevice>)m4obj(a[0]) accelerationStructureSizesWithDescriptor:descriptor->descriptor];
    call->ret[0]=sizes.accelerationStructureSize; call->ret[1]=sizes.buildScratchBufferSize; call->ret[2]=sizes.refitScratchBufferSize; return M4_SUCCESS;
  }
  case WMTMetal4ASBuild:
  case WMTMetal4ASRefit: {
    DXMTM4ComputeEncoder *context=(DXMTM4ComputeEncoder *)m4obj(a[0]); descriptor=(DXMTM4ASDescriptor *)m4obj(a[1]);
    BOOL refit=call->operation==WMTMetal4ASRefit; uint64_t destination=refit?a[3]:a[2];
    id<MTLBuffer> scratch=m4obj(a[refit?4:3]); uint64_t offset=a[refit?5:4];
    if (!descriptor || !scratch || offset>scratch.length || !destination) return M4_INVALID;
    /* Size queries intentionally have no resolved GPU buffers. Their real
     * count/format descriptor is valid for sizing, never for executing work. */
    if (descriptor->unresolvedGeometries.count || descriptor->unresolvedInstances) return M4_INVALID;
    m4_as_resources(context->recording,descriptor,destination); m4_allocation(context->recording,scratch);
    id<MTLAccelerationStructure> target=m4obj(dxmt_as_sidecar_acceleration_structure(destination));
    id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
    m4_stage_barrier(context,MTLStageAccelerationStructure);
    if (refit) {
      m4_as_resources(context->recording,nil,a[2]);
      [encoder refitAccelerationStructure:m4obj(dxmt_as_sidecar_acceleration_structure(a[2])) descriptor:descriptor->descriptor destination:target scratchBuffer:m4_buffer_range(scratch,offset)];
    } else [encoder buildAccelerationStructure:target descriptor:descriptor->descriptor scratchBuffer:m4_buffer_range(scratch,offset)];
    call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASCopy: {
    DXMTM4ComputeEncoder *context=(DXMTM4ComputeEncoder *)m4obj(a[0]); if (a[3]>1) return M4_INVALID;
    m4_as_resources(context->recording,nil,a[1]); m4_as_resources(context->recording,nil,a[2]);
    id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
    id<MTLAccelerationStructure> source=m4obj(dxmt_as_sidecar_acceleration_structure(a[1])),destination=m4obj(dxmt_as_sidecar_acceleration_structure(a[2]));
    m4_stage_barrier(context,MTLStageAccelerationStructure);
    if (a[3]) [encoder copyAndCompactAccelerationStructure:source toAccelerationStructure:destination];
    else [encoder copyAccelerationStructure:source toAccelerationStructure:destination];
    call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASCompactedSize: {
    DXMTM4ComputeEncoder *context=(DXMTM4ComputeEncoder *)m4obj(a[0]); id<MTLBuffer> buffer=m4obj(a[2]);
    if (!buffer || a[3]>buffer.length || buffer.length-a[3]<sizeof(uint64_t)) return M4_INVALID;
    m4_as_resources(context->recording,nil,a[1]); m4_allocation(context->recording,buffer);
    m4_stage_barrier(context,MTLStageAccelerationStructure);
    [(id<MTL4ComputeCommandEncoder>)context->encoder writeCompactedAccelerationStructureSize:m4obj(dxmt_as_sidecar_acceleration_structure(a[1])) toBuffer:MTL4BufferRangeMake(buffer.gpuAddress+a[3],sizeof(uint64_t))];
    return M4_SUCCESS;
  }
  case WMTMetal4ASPrepass: {
    DXMTM4ComputeEncoder *context=(DXMTM4ComputeEncoder *)m4obj(a[0]); id<MTLComputePipelineState> state=m4obj(a[1]);
    const struct WMTASPrepassInfo *info=(const void *)(uintptr_t)a[12];
    if (!state || !info || !a[6] || !a[10]) return M4_INVALID;
    id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
    m4_allocation(context->recording,state); [encoder setComputePipelineState:state];
    m4_compute_buffer(context,m4obj(a[2]?:a[6]),a[2]?a[3]:a[7],0);
    m4_compute_buffer(context,m4obj(a[4]?:a[6]),a[4]?a[5]:a[7],1);
    m4_compute_buffer(context,m4obj(a[6]),a[7],2);
    m4_compute_buffer(context,m4obj(a[8]?:a[6]),a[8]?a[9]:a[7],3);
    NSUInteger offset=0; id<MTLBuffer> constants=m4_upload(context->owner,info,sizeof(*info),&offset);
    if (!constants) return M4_FAILED;
    m4_compute_buffer(context,constants,offset,4); m4_compute_buffer(context,m4obj(a[10]),a[11],5);
    NSUInteger threads=info->convert_instances?MAX(info->instance_count,1u):1u;
    m4_stage_barrier(context,MTLStageDispatch);
    [encoder dispatchThreads:MTLSizeMake(threads,1,1) threadsPerThreadgroup:MTLSizeMake(MIN(threads,state.maxTotalThreadsPerThreadgroup),1,1)];
    call->ret[0]=1; return M4_SUCCESS;
  }
  default: return M4_INVALID;
  }
}

/* Called with the submission condition locked. Native commit can still be
 * using its exact command/allocator/resource identities when feedback arrives. */
static void m4_finish_submission(DXMTM4Submission *submission) {
  if (!submission->commitReturned || !submission->feedbackReceived) return;
  [submission->leases removeAllObjects];
  for (DXMTM4CommandBuffer *context in submission->buffers)
    atomic_fetch_sub_explicit(&context->inFlight,1,memory_order_release);
  submission->completed=YES; [submission->condition broadcast];
}

static int32_t m4_commit(struct WMTMetal4Call *call) {
  id<MTL4CommandQueue> queue=m4obj(call->args[0]);
  const uint64_t *handles=(const void *)(uintptr_t)call->args[1]; uint64_t count=call->args[2];
  if (!queue || !handles || !count || count>NSUIntegerMax/sizeof(id)) return M4_INVALID;
  uint64_t command_count = count;
  for (uint64_t i=0;i<count;++i) {
    DXMTM4CommandBuffer *context=m4cb(handles[i]);
    if (!context || !context->recording || !context->ended || atomic_load_explicit(&context->inFlight,memory_order_acquire))
      return M4_INVALID;
    if (context->recording->segments.count > NSUIntegerMax/sizeof(id) - command_count) return M4_INVALID;
    command_count += context->recording->segments.count;
  }
  id<MTL4CommandBuffer> local[32]; id<MTL4CommandBuffer> *commands=command_count<=32?local:malloc(command_count*sizeof(*commands));
  if (!commands) return M4_FAILED;
  DXMTM4Submission *submission=[DXMTM4Submission new]; submission->condition=[NSCondition new];
  submission->leases=[[NSMutableArray alloc] initWithCapacity:count*3]; submission->buffers=[[NSMutableArray alloc] initWithCapacity:count];
  NSUInteger command_index = 0;
  for (uint64_t i=0;i<count;++i) {
    DXMTM4CommandBuffer *context=m4cb(handles[i]);
    for (id<MTL4CommandBuffer> segment in context->recording->segments) commands[command_index++]=segment;
    commands[command_index++]=context->command; [submission->buffers addObject:context];
    [submission->leases addObject:context->recording]; [submission->leases addObject:context->command]; [submission->leases addObject:context->allocator];
    if (context->recording->error && !submission->error) submission->error=[context->recording->error retain];
  }
  for (DXMTM4CommandBuffer *context in submission->buffers) {
    atomic_fetch_add_explicit(&context->inFlight,1,memory_order_release);
    [context->recording release]; context->recording=nil;
  }
  if (submission->error) {
    [submission->condition lock]; submission->commitReturned=YES; submission->feedbackReceived=YES;
    m4_finish_submission(submission); [submission->condition unlock];
  } else {
    MTL4CommitOptions *options=[MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
      [submission->condition lock]; submission->error=[feedback.error retain];
      submission->gpuStart=(uint64_t)(feedback.GPUStartTime*1000000000.0);
      submission->gpuEnd=(uint64_t)(feedback.GPUEndTime*1000000000.0);
      submission->feedbackReceived=YES; m4_finish_submission(submission); [submission->condition unlock];
    }];
    [queue commit:commands count:command_count options:options]; [options release];
    [submission->condition lock]; submission->commitReturned=YES;
    m4_finish_submission(submission); [submission->condition unlock];
  }
  if (commands!=local) free(commands); call->ret[0]=(uint64_t)submission; return M4_SUCCESS;
}

int32_t winemetal_metal4_call(void *params) {
  struct WMTMetal4Call *call=params;
  if (!call || call->reserved) return M4_INVALID;
  memset(call->ret,0,sizeof(call->ret));
  if (@available(macOS 26.0,*)) {
    if (call->operation>=128 && call->operation<192) return winemetal_metal4_pipeline_call(call);
    if (call->operation>=192 && call->operation<224) return winemetal_metal4_query_call(call);
    if (call->operation>=224 && call->operation<288) return winemetal_metal4_fx_call(call);
    uint64_t *a=call->args; if (!a[0]) return M4_INVALID;
    if (call->operation>=WMTMetal4NewASDescriptor && call->operation<=WMTMetal4ASPrepass) return m4_as_call(call);
    switch (call->operation) {
    case WMTMetal4Supports: call->ret[0]=[(id<MTLDevice>)m4obj(a[0]) supportsFamily:MTLGPUFamilyMetal4]; return M4_SUCCESS;
    case WMTMetal4NewAllocator: call->ret[0]=(uint64_t)[(id<MTLDevice>)m4obj(a[0]) newCommandAllocator]; return M4_SUCCESS;
    case WMTMetal4ResetAllocator: [(id<MTL4CommandAllocator>)m4obj(a[0]) reset]; return M4_SUCCESS;
    case WMTMetal4AllocatorSize: call->ret[0]=[(id<MTL4CommandAllocator>)m4obj(a[0]) allocatedSize]; return M4_SUCCESS;
    case WMTMetal4NewCommandBuffer: {
      DXMTM4CommandBuffer *context=[DXMTM4CommandBuffer new]; context->device=[m4obj(a[0]) retain];
      context->command=[context->device newCommandBuffer]; context->uploads=[NSMutableArray new]; atomic_init(&context->inFlight,0);
      if (!context->command) { [context release]; return M4_FAILED; }
      call->ret[0]=(uint64_t)context; return M4_SUCCESS;
    }
    case WMTMetal4BeginCommandBuffer: {
      DXMTM4CommandBuffer *context=m4cb(a[0]); if (!a[1] || context->recording || atomic_load_explicit(&context->inFlight,memory_order_acquire)) return M4_INVALID;
      if (context->ended) {
        /* macOS 27 MetalTools keeps retired heap-usage entries across begin.
         * A subsequent preCommit dereferences a freed GPU debug heap, even
         * after completed submissions and committed residency removals. Keep
         * allocator/upload reuse, but give each recording fresh native metadata.
         * The factory-provided command buffer already serves the first recording. */
        id<MTL4CommandBuffer> command=[context->device newCommandBuffer];
        if (!command) return M4_FAILED;
        [context->command release]; context->command=command;
      }
      [context->allocator release]; context->allocator=[m4obj(a[1]) retain];
      context->recording=[DXMTM4Recording new]; context->recording->objects=[NSMutableSet new]; context->recording->encoders=[NSMutableArray new];
      MTLResidencySetDescriptor *descriptor=[MTLResidencySetDescriptor new]; descriptor.initialCapacity=64;
      context->recording->residency=[context->device newResidencySetWithDescriptor:descriptor error:nil]; [descriptor release];
      if (!context->recording->residency) { [context->recording release]; context->recording=nil; return M4_FAILED; }
      context->uploadIndex=0; context->uploadOffset=0; context->ended=NO;
      [context->command beginCommandBufferWithAllocator:context->allocator]; return M4_SUCCESS;
    }
    case WMTMetal4EndCommandBuffer: {
      DXMTM4CommandBuffer *context=m4cb(a[0]); if (!context->recording || context->ended) return M4_INVALID;
      [context->recording->residency commit];
      [context->command useResidencySet:context->recording->residency];
      [context->command endCommandBuffer]; context->ended=YES; return M4_SUCCESS;
    }
    case WMTMetal4RenderEncoder: {
      DXMTM4CommandBuffer *owner=m4cb(a[0]); if (!owner->recording || owner->ended || !a[1]) return M4_INVALID;
      MTL4RenderPassDescriptor *descriptor=m4_render_pass(owner,(const void *)(uintptr_t)a[1]); if (!descriptor) return M4_FAILED;
      DXMTM4RenderEncoder *context=[DXMTM4RenderEncoder new]; context->owner=owner; context->recording=owner->recording;
      /* Registering a visibility mode makes Reset write the scratch buffer
       * before rasterization, even if the mode is disabled before drawing.
       * A Fragment-only wait therefore cannot protect a preceding GPU read. */
      if (descriptor.visibilityResultBuffer && descriptor.visibilityResultType==MTLVisibilityResultTypeReset)
        context->fenceWaitStages=MTLStageVertex|MTLStageObject|MTLStageMesh;
      context->encoder=[[owner->command renderCommandEncoderWithDescriptor:descriptor] retain]; [descriptor release];
      if (!context->encoder) { [context release]; return M4_FAILED; }
      [owner->recording->encoders addObject:context]; call->ret[0]=(uint64_t)context; [context release]; return M4_SUCCESS;
    }
    case WMTMetal4ComputeEncoder: {
      DXMTM4CommandBuffer *owner=m4cb(a[0]); if (!owner->recording || owner->ended) return M4_INVALID;
      DXMTM4ComputeEncoder *context=[DXMTM4ComputeEncoder new]; context->owner=owner; context->recording=owner->recording;
      context->concurrent=a[1]!=0;
      context->encoder=[[owner->command computeCommandEncoder] retain];
      if (!context->encoder) { [context release]; return M4_FAILED; }
      [owner->recording->encoders addObject:context]; call->ret[0]=(uint64_t)context; [context release]; return M4_SUCCESS;
    }
    case WMTMetal4EndEncoding: [((DXMTM4Encoder *)m4obj(a[0]))->encoder endEncoding]; return M4_SUCCESS;
    case WMTMetal4SetEncoderLabel: ((DXMTM4Encoder *)m4obj(a[0]))->encoder.label=m4obj(a[1]); return M4_SUCCESS;
    case WMTMetal4RenderCommands: return m4_render_commands((DXMTM4RenderEncoder *)m4obj(a[0]),(const void *)(uintptr_t)a[1]);
    case WMTMetal4ComputeCommands: return m4_compute_commands((DXMTM4ComputeEncoder *)m4obj(a[0]),(const void *)(uintptr_t)a[1]);
    case WMTMetal4BlitCommands: return m4_blit_commands((DXMTM4ComputeEncoder *)m4obj(a[0]),(const void *)(uintptr_t)a[1]);
    case WMTMetal4Commit: return m4_commit(call);
    case WMTMetal4WaitSubmission: {
      DXMTM4Submission *submission=(DXMTM4Submission *)m4obj(a[0]); [submission->condition lock];
      while (!submission->completed) [submission->condition wait]; [submission->condition unlock]; return M4_SUCCESS;
    }
    case WMTMetal4GetSubmissionInfo: {
      if (!a[1]) return M4_INVALID; DXMTM4Submission *submission=(DXMTM4Submission *)m4obj(a[0]);
      struct WMTMetal4SubmissionInfo *info=(void *)(uintptr_t)a[1]; [submission->condition lock];
      *info=(struct WMTMetal4SubmissionInfo){submission->completed,(uint64_t)submission->error,submission->gpuStart,submission->gpuEnd};
      [submission->condition unlock]; return M4_SUCCESS;
    }
    case WMTMetal4WaitEvent: [(id<MTL4CommandQueue>)m4obj(a[0]) waitForEvent:m4obj(a[1]) value:a[2]]; return M4_SUCCESS;
    case WMTMetal4WaitDrawable: [(id<MTL4CommandQueue>)m4obj(a[0]) waitForDrawable:m4obj(a[1])]; return M4_SUCCESS;
    case WMTMetal4SignalDrawable: [(id<MTL4CommandQueue>)m4obj(a[0]) signalDrawable:m4obj(a[1])]; return M4_SUCCESS;
    case WMTMetal4UseResidencySet: {
      DXMTM4CommandBuffer *context=m4cb(a[0]); if (!context->recording) return M4_INVALID;
      m4_retain(context->recording,m4obj(a[1])); [context->command useResidencySet:m4obj(a[1])]; return M4_SUCCESS;
    }
    case WMTMetal4NewArgumentTable: {
      MTL4ArgumentTableDescriptor *descriptor=[MTL4ArgumentTableDescriptor new]; descriptor.maxBufferBindCount=a[1];
      descriptor.maxTextureBindCount=a[2]; descriptor.maxSamplerStateBindCount=a[3]; descriptor.supportAttributeStrides=a[4]!=0; descriptor.initializeBindings=YES;
      call->ret[0]=(uint64_t)[(id<MTLDevice>)m4obj(a[0]) newArgumentTableWithDescriptor:descriptor error:nil]; [descriptor release]; return M4_SUCCESS;
    }
    case WMTMetal4TableAddress: [(id<MTL4ArgumentTable>)m4obj(a[0]) setAddress:a[1] atIndex:a[2]]; return M4_SUCCESS;
    case WMTMetal4TableResource: [(id<MTL4ArgumentTable>)m4obj(a[0]) setResource:(MTLResourceID){a[1]} atBufferIndex:a[2]]; return M4_SUCCESS;
    case WMTMetal4TableTexture: [(id<MTL4ArgumentTable>)m4obj(a[0]) setTexture:(MTLResourceID){a[1]} atIndex:a[2]]; return M4_SUCCESS;
    case WMTMetal4TableSampler: [(id<MTL4ArgumentTable>)m4obj(a[0]) setSamplerState:(MTLResourceID){a[1]} atIndex:a[2]]; return M4_SUCCESS;
    case WMTMetal4TableAddressStride: [(id<MTL4ArgumentTable>)m4obj(a[0]) setAddress:a[1] attributeStride:a[2] atIndex:a[3]]; return M4_SUCCESS;
    case WMTMetal4ComputeArgumentTable: {
      DXMTM4ComputeEncoder *context=(DXMTM4ComputeEncoder *)m4obj(a[0]); m4_retain(context->recording,m4obj(a[1]));
      [(id<MTL4ComputeCommandEncoder>)context->encoder setArgumentTable:m4obj(a[1])]; return M4_SUCCESS;
    }
    case WMTMetal4RenderArgumentTable: {
      DXMTM4RenderEncoder *context=(DXMTM4RenderEncoder *)m4obj(a[0]); m4_retain(context->recording,m4obj(a[1]));
      [(id<MTL4RenderCommandEncoder>)context->encoder setArgumentTable:m4obj(a[1]) atStages:(MTLRenderStages)a[2]]; return M4_SUCCESS;
    }
    case WMTMetal4Barrier: {
      DXMTM4Encoder *context=(DXMTM4Encoder *)m4obj(a[0]); id<MTL4CommandEncoder> encoder=context->encoder;
      if (a[1]==0) [encoder barrierAfterQueueStages:(MTLStages)a[2] beforeStages:(MTLStages)a[3] visibilityOptions:(MTL4VisibilityOptions)a[4]];
      else if (a[1]==1) [encoder barrierAfterStages:(MTLStages)a[2] beforeQueueStages:(MTLStages)a[3] visibilityOptions:(MTL4VisibilityOptions)a[4]];
      else if (a[1]==2) [encoder barrierAfterEncoderStages:(MTLStages)a[2] beforeEncoderStages:(MTLStages)a[3] visibilityOptions:(MTL4VisibilityOptions)a[4]];
      else return M4_INVALID;
      if (a[1]==2 && (a[4]&MTL4VisibilityOptionDevice)) m4_record_barrier(context,a[2],a[3]);
      return M4_SUCCESS;
    }
    case WMTMetal4Fence: {
      DXMTM4Encoder *context=(DXMTM4Encoder *)m4obj(a[0]); m4_retain(context->recording,m4obj(a[1]));
      if (a[2]) [context->encoder updateFence:m4obj(a[1]) afterEncoderStages:(MTLStages)a[3]];
      else [context->encoder waitForFence:m4obj(a[1]) beforeEncoderStages:(MTLStages)a[3]|context->fenceWaitStages]; return M4_SUCCESS;
    }
    default: return M4_INVALID;
    }
  }
  return M4_UNSUPPORTED;
}


static int32_t m4_compute_commands(DXMTM4ComputeEncoder *context, const struct wmtcmd_base *next) {
  id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    case WMTComputeCommandNop: break;
    case WMTComputeCommandDispatch:
    case WMTComputeCommandDispatchThreads: {
      const struct wmtcmd_compute_dispatch *body=(const void *)next;
      MTLSize size=MTLSizeMake(body->size.width,body->size.height,body->size.depth);
      m4_stage_barrier(context,MTLStageDispatch);
      if (next->type==WMTComputeCommandDispatch) [encoder dispatchThreadgroups:size threadsPerThreadgroup:context->threadgroupSize];
      else [encoder dispatchThreads:size threadsPerThreadgroup:context->threadgroupSize];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      const struct wmtcmd_compute_dispatch_indirect *body=(const void *)next;
      m4_stage_barrier(context,MTLStageDispatch);
      [encoder dispatchThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->indirect_args_buffer,body->indirect_args_offset)
                              threadsPerThreadgroup:context->threadgroupSize]; break;
    }
    case WMTComputeCommandSetPSO: {
      const struct wmtcmd_compute_setpso *body=(const void *)next;
      id<MTLComputePipelineState> state=m4obj(body->pso); m4_allocation(context->recording,state);
      [encoder setComputePipelineState:state];
      context->threadgroupSize=MTLSizeMake(body->threadgroup_size.width,body->threadgroup_size.height,body->threadgroup_size.depth); break;
    }
    case WMTComputeCommandSetBuffer: {
      const struct wmtcmd_compute_setbuffer *body=(const void *)next;
      m4_compute_buffer(context,m4obj(body->buffer),body->offset,body->index); break;
    }
    case WMTComputeCommandSetBufferOffset: {
      const struct wmtcmd_compute_setbufferoffset *body=(const void *)next;
      if (body->index>=31) return M4_INVALID;
      m4_compute_buffer(context,context->buffers[0][body->index],body->offset,body->index); break;
    }
    case WMTComputeCommandUseResource: {
      const struct wmtcmd_compute_useresource *body=(const void *)next;
      m4_allocation(context->recording,m4obj(body->resource)); break;
    }
    case WMTComputeCommandSetBytes: {
      const struct wmtcmd_compute_setbytes *body=(const void *)next;
      NSUInteger offset=0; id<MTLBuffer> buffer=m4_upload(context->owner,body->bytes.ptr,body->length,&offset);
      if (buffer) m4_compute_buffer(context,buffer,offset,body->index); break;
    }
    case WMTComputeCommandSetTexture: {
      const struct wmtcmd_compute_settexture *body=(const void *)next;
      id<MTLTexture> texture=m4obj(body->texture); m4_allocation(context->recording,texture);
      if (body->index>=128) return M4_INVALID;
      [m4_table(context,0) setTexture:texture.gpuResourceID atIndex:body->index]; m4_compute_table(context); break;
    }
    case WMTComputeCommandUpdateFence:
    case WMTComputeCommandWaitForFence: {
      const struct wmtcmd_compute_fence_op *body=(const void *)next;
      id<MTLFence> fence=m4obj(body->fence); m4_retain(context->recording,fence);
      if (next->type==WMTComputeCommandUpdateFence) [encoder updateFence:fence afterEncoderStages:MTLStageDispatch];
      else [encoder waitForFence:fence beforeEncoderStages:MTLStageDispatch]; break;
    }
    case WMTComputeCommandMemoryBarrier:
      [encoder barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
      m4_record_barrier(context,MTLStageDispatch,MTLStageDispatch); break;
    case WMTComputeCommandExecuteCommandsInBuffer: {
      const struct wmtcmd_compute_executecommands *body=(const void *)next;
      id<MTLIndirectCommandBuffer> buffer=m4obj(body->indirect_command_buffer); m4_allocation(context->recording,buffer);
      m4_stage_barrier(context,MTLStageDispatch);
      [encoder executeCommandsInBuffer:buffer withRange:NSMakeRange(body->location,body->length)]; break;
    }
    case WMTComputeCommandSetVisibleFunctionTable: {
      const struct wmtcmd_compute_setfunctiontable *body=(const void *)next;
      id<MTLVisibleFunctionTable> table=m4obj(body->table); m4_allocation(context->recording,table);
      if (body->index>=31) return M4_INVALID;
      [m4_table(context,0) setResource:table.gpuResourceID atBufferIndex:body->index]; m4_compute_table(context); break;
    }
    case WMTComputeCommandSetIntersectionFunctionTable: {
      const struct wmtcmd_compute_setfunctiontable *body=(const void *)next;
      id<MTLIntersectionFunctionTable> table=m4obj(body->table); m4_allocation(context->recording,table);
      if (body->index>=31) return M4_INVALID;
      [m4_table(context,0) setResource:table.gpuResourceID atBufferIndex:body->index]; m4_compute_table(context); break;
    }
    case WMTComputeCommandSetAccelerationStructure: {
      const struct wmtcmd_compute_setaccelerationstructure *body=(const void *)next;
      id<MTLAccelerationStructure> structure=m4obj(body->acceleration_structure); m4_allocation(context->recording,structure);
      if (body->index>=31) return M4_INVALID;
      [m4_table(context,0) setResource:structure.gpuResourceID atBufferIndex:body->index]; m4_compute_table(context); break;
    }
    default: m4_error(context->recording,@"Unknown Metal4 compute command"); return M4_INVALID;
    }
    next=next->next.ptr;
  }
  return context->recording->error ? M4_FAILED : M4_SUCCESS;
}
static int32_t m4_render_commands(DXMTM4RenderEncoder *context, const struct wmtcmd_base *next) {
  id<MTL4RenderCommandEncoder> encoder=(id<MTL4RenderCommandEncoder>)context->encoder;
  while (next) {
    switch ((enum WMTRenderCommandType)next->type) {
    default:
      m4_error(context->recording,@"Unknown Metal4 render command"); return M4_INVALID;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      struct wmtcmd_render_useresource *body = (struct wmtcmd_render_useresource *)next;
      m4_allocation(context->recording,m4obj(body->resource));
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      m4_render_buffer(context,0,m4obj(body->buffer),body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      m4_render_offset(context,0,body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      m4_render_buffer(context,1,m4obj(body->buffer),body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      m4_render_offset(context,1,body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      m4_render_buffer(context,2,m4obj(body->buffer),body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      m4_render_offset(context,2,body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      m4_render_buffer(context,3,m4obj(body->buffer),body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      m4_render_offset(context,3,body->offset,body->index);
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      m4_render_bytes(context,body->bytes.ptr,body->length,body->index);
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      id<MTLTexture> texture=m4obj(body->texture); m4_allocation(context->recording,texture);
      if (body->index>=128) return M4_INVALID;
      [m4_table(context,1) setTexture:texture.gpuResourceID atIndex:body->index]; m4_render_table(context,1);
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      struct wmtcmd_render_setrasterizerstate *body = (struct wmtcmd_render_setrasterizerstate *)next;
      [encoder setTriangleFillMode:(MTLTriangleFillMode)body->fill_mode];
      [encoder setCullMode:(MTLCullMode)body->cull_mode];
      [encoder setDepthClipMode:(MTLDepthClipMode)body->depth_clip_mode];
      [encoder setDepthBias:body->depth_bias slopeScale:body->scole_scale clamp:body->depth_bias_clamp];
      [encoder setFrontFacingWinding:(MTLWinding)body->winding];
      break;
    }
    case WMTRenderCommandSetViewports: {
      struct wmtcmd_render_setviewports *body = (struct wmtcmd_render_setviewports *)next;
      [encoder setViewports:(const MTLViewport *)body->viewports.ptr count:body->viewport_count];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      struct wmtcmd_render_setscissorrects *body = (struct wmtcmd_render_setscissorrects *)next;
      [encoder setScissorRects:(const MTLScissorRect *)body->scissor_rects.ptr count:body->rect_count];
      break;
    }
    case WMTRenderCommandSetPSO: {
      struct wmtcmd_render_setpso *body = (struct wmtcmd_render_setpso *)next;
      m4_allocation(context->recording,m4obj(body->pso));
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      m4_retain(context->recording,m4obj(body->dsso));
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetDepthStencilState: {
      struct wmtcmd_render_setdepthstencilstate *body = (struct wmtcmd_render_setdepthstencilstate *)next;
      m4_retain(context->recording,m4obj(body->depth_stencil_state));
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->depth_stencil_state];
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetBlendFactor: {
      struct wmtcmd_render_setblendcolor *body = (struct wmtcmd_render_setblendcolor *)next;
      [encoder setBlendColorRed:body->red green:body->green blue:body->blue alpha:body->alpha];
      break;
    }
    case WMTRenderCommandSetStencilRef: {
      struct wmtcmd_render_setstencilref *body = (struct wmtcmd_render_setstencilref *)next;
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetStencilRefFrontBack: {
      struct wmtcmd_render_setstencilref_front_back *body = (struct wmtcmd_render_setstencilref_front_back *)next;
      [encoder setStencilFrontReferenceValue:body->front_stencil_ref backReferenceValue:body->back_stencil_ref];
      break;
    }
    case WMTRenderCommandSetVertexAmplification: {
      struct wmtcmd_render_set_vertex_amplification *body = (struct wmtcmd_render_set_vertex_amplification *)next;
      MTLVertexAmplificationViewMapping mapping = {body->viewport_offset, body->render_target_array_offset};
      [encoder setVertexAmplificationCount:1 viewMappings:&mapping];
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      struct wmtcmd_render_setvisibilitymode *body = (struct wmtcmd_render_setvisibilitymode *)next;
      [encoder setVisibilityResultMode:(MTLVisibilityResultMode)body->mode offset:body->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      struct wmtcmd_render_draw *body = (struct wmtcmd_render_draw *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  vertexStart:body->vertex_start
                  vertexCount:body->vertex_count
                instanceCount:body->instance_count
                 baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      struct wmtcmd_render_draw_indexed *body = (struct wmtcmd_render_draw_indexed *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:m4_address(context->recording,body->index_buffer,body->index_buffer_offset)
                   indexBufferLength:m4_index_length(context->recording,body->index_buffer,body->index_buffer_offset)
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:m4_address(context->recording,body->indirect_args_buffer,body->indirect_args_offset)];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:m4_address(context->recording,body->index_buffer,body->index_buffer_offset)
                   indexBufferLength:m4_index_length(context->recording,body->index_buffer,body->index_buffer_offset)
                      indirectBuffer:m4_address(context->recording,body->indirect_args_buffer,body->indirect_args_offset)];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      struct wmtcmd_render_draw_meshthreadgroups *body = (struct wmtcmd_render_draw_meshthreadgroups *)next;
      [encoder drawMeshThreadgroups:MTLSizeMake(
                                        body->threadgroup_per_grid.width, body->threadgroup_per_grid.height,
                                        body->threadgroup_per_grid.depth
                                    )
          threadsPerObjectThreadgroup:MTLSizeMake(
                                          body->object_threadgroup_size.width, body->object_threadgroup_size.height,
                                          body->object_threadgroup_size.depth
                                      )
            threadsPerMeshThreadgroup:MTLSizeMake(
                                          body->mesh_threadgroup_size.width, body->mesh_threadgroup_size.height,
                                          body->mesh_threadgroup_size.depth
                                      )];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      struct wmtcmd_render_draw_meshthreadgroups_indirect *body =
          (struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->indirect_args_buffer,body->indirect_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(
                                                          body->object_threadgroup_size.width,
                                                          body->object_threadgroup_size.height,
                                                          body->object_threadgroup_size.depth
                                                      )
                            threadsPerMeshThreadgroup:MTLSizeMake(
                                                          body->mesh_threadgroup_size.width,
                                                          body->mesh_threadgroup_size.height,
                                                          body->mesh_threadgroup_size.depth
                                                      )];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      struct wmtcmd_render_memory_barrier *body = (struct wmtcmd_render_memory_barrier *)next;
      [encoder barrierAfterEncoderStages:(MTLStages)body->stages_after beforeEncoderStages:(MTLStages)body->stages_before visibilityOptions:MTL4VisibilityOptionDevice];
      break;
    }
    case WMTRenderCommandDXMTGeometryDraw: {
      struct wmtcmd_render_dxmt_geometry_draw *body = (struct wmtcmd_render_dxmt_geometry_draw *)next;
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexed: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed *body = (struct wmtcmd_render_dxmt_geometry_draw_indexed *)next;
      m4_render_buffer(context,3,m4obj(body->index_buffer),body->index_buffer_offset,SM50_BINDING_INDEX_INDEX_BUFFER);
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indirect *body = (struct wmtcmd_render_dxmt_geometry_draw_indirect *)next;
      m4_render_buffer(context,3,m4obj(body->indirect_args_buffer),body->indirect_args_offset,SM50_BINDING_INDEX_INDIRECT_ARGUMENTS);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->dispatch_args_buffer,body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      m4_render_buffer(context,3,m4obj(body->imm_draw_arguments),0,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)next;
      m4_render_buffer(context,3,m4obj(body->index_buffer),body->index_buffer_offset,SM50_BINDING_INDEX_INDEX_BUFFER);
      m4_render_buffer(context,3,m4obj(body->indirect_args_buffer),body->indirect_args_offset,SM50_BINDING_INDEX_INDIRECT_ARGUMENTS);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->dispatch_args_buffer,body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      m4_render_buffer(context,3,m4obj(body->imm_draw_arguments),0,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDraw: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw *)next;
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)next;
      m4_render_buffer(context,3,m4obj(body->index_buffer),body->index_buffer_offset,SM50_BINDING_INDEX_INDEX_BUFFER);
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)next;
      m4_render_buffer(context,3,m4obj(body->indirect_args_buffer),body->indirect_args_offset,SM50_BINDING_INDEX_INDIRECT_ARGUMENTS);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->dispatch_args_buffer,body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      m4_render_buffer(context,3,m4obj(body->imm_draw_arguments),0,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)next;
      m4_render_buffer(context,3,m4obj(body->index_buffer),body->index_buffer_offset,SM50_BINDING_INDEX_INDEX_BUFFER);
      m4_render_buffer(context,3,m4obj(body->indirect_args_buffer),body->indirect_args_offset,SM50_BINDING_INDEX_INDIRECT_ARGUMENTS);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->dispatch_args_buffer,body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      m4_render_buffer(context,3,m4obj(body->imm_draw_arguments),0,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      m4_retain(context->recording,m4obj(body->fence));
      [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:(MTLStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      m4_retain(context->recording,m4obj(body->fence));
      [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:(MTLStages)body->stages|context->fenceWaitStages];
      break;
    }
    case WMTRenderCommandSetViewport: {
      struct wmtcmd_render_setviewport *body = (struct wmtcmd_render_setviewport *)next;
      union {
        struct WMTViewport src;
        MTLViewport dst;
      } u = {.src = body->viewport};
      [encoder setViewport:u.dst];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      struct wmtcmd_render_setscissorrect *body = (struct wmtcmd_render_setscissorrect *)next;
      union {
        struct WMTScissorRect src;
        MTLScissorRect dst;
      } u = {.src = body->scissor_rect};
      [encoder setScissorRect:u.dst];
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      struct wmtcmd_render_dispatch_threads_per_tile *body = (struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [encoder dispatchThreadsPerTile:MTLSizeMake(body->width, body->height, 1)];
      break;
    }
    case WMTRenderCommandExecuteCommandsInBuffer: {
      struct wmtcmd_render_executecommands *body = (struct wmtcmd_render_executecommands *)next;
      m4_allocation(context->recording,m4obj(body->indirect_command_buffer));
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    }
    next = next->next.ptr;
  }
  return context->recording->error ? M4_FAILED : M4_SUCCESS;
}



static int32_t m4_blit_commands(DXMTM4ComputeEncoder *context, const struct wmtcmd_base *next) {
  id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      if (next->type) { m4_error(context->recording,@"Unknown Metal4 copy command"); return M4_INVALID; }
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,body->src,body->src_offset,body->copy_length,body->dst,body->dst_offset,body->copy_length);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,body->src,body->src_offset,UINT64_MAX,m4_texture_key(m4obj(body->dst)),0,UINT64_MAX);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTextureWithBlitOption: {
      struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *body =
          (struct wmtcmd_blit_copy_from_buffer_to_texture_withblitoption *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,body->src,body->src_offset,UINT64_MAX,m4_texture_key(m4obj(body->dst)),0,UINT64_MAX);
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
            sourceBytesPerRow:body->bytes_per_row
          sourceBytesPerImage:body->bytes_per_image
                   sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                    toTexture:(id<MTLTexture>)body->dst
             destinationSlice:body->slice
             destinationLevel:body->level
            destinationOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                      options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      struct wmtcmd_blit_copy_from_texture_to_buffer *body = (struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,m4_texture_key(m4obj(body->src)),0,UINT64_MAX,body->dst,body->offset,UINT64_MAX);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBufferWithBlitOption: {
      struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *body =
          (struct wmtcmd_blit_copy_from_texture_to_buffer_withblitoption *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,m4_texture_key(m4obj(body->src)),0,UINT64_MAX,body->dst,body->offset,UINT64_MAX);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                       sourceSlice:body->slice
                       sourceLevel:body->level
                      sourceOrigin:MTLOriginMake(body->origin.x, body->origin.y, body->origin.z)
                        sourceSize:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
                          toBuffer:(id<MTLBuffer>)body->dst
                 destinationOffset:body->offset
            destinationBytesPerRow:body->bytes_per_row
          destinationBytesPerImage:body->bytes_per_image
                           options:(MTLBlitOption)body->options];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      struct wmtcmd_blit_copy_from_texture_to_texture *body = (struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,m4_texture_key(m4obj(body->src)),0,UINT64_MAX,m4_texture_key(m4obj(body->dst)),0,UINT64_MAX);
      [encoder copyFromTexture:(id<MTLTexture>)body->src
                   sourceSlice:body->src_slice
                   sourceLevel:body->src_level
                  sourceOrigin:MTLOriginMake(body->src_origin.x, body->src_origin.y, body->src_origin.z)
                    sourceSize:MTLSizeMake(body->src_size.width, body->src_size.height, body->src_size.depth)
                     toTexture:(id<MTLTexture>)body->dst
              destinationSlice:body->dst_slice
              destinationLevel:body->dst_level
             destinationOrigin:MTLOriginMake(body->dst_origin.x, body->dst_origin.y, body->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      struct wmtcmd_blit_generate_mipmaps *body = (struct wmtcmd_blit_generate_mipmaps *)next;
      m4_allocation(context->recording,m4obj(body->texture));
      m4_blit_access(context,m4_texture_key(m4obj(body->texture)),0,UINT64_MAX,m4_texture_key(m4obj(body->texture)),0,UINT64_MAX);
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      m4_retain(context->recording,m4obj(body->fence));
      [encoder updateFence:(id<MTLFence>)body->fence afterEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      m4_retain(context->recording,m4obj(body->fence));
      [encoder waitForFence:(id<MTLFence>)body->fence beforeEncoderStages:MTLStageBlit];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      m4_allocation(context->recording,m4obj(body->buffer));
      m4_blit_access(context,0,0,0,body->buffer,body->offset,body->length);
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResolveCounters:
      m4_error(context->recording,@"Classic counter sample buffers cannot be encoded in Metal4; use the counter-heap query API");
      return M4_INVALID;
    case WMTBlitCommandResetCommandsInBuffer: {
      struct wmtcmd_blit_resetcommands *body = (struct wmtcmd_blit_resetcommands *)next;
      m4_allocation(context->recording,m4obj(body->indirect_command_buffer));
      m4_blit_access(context,0,0,0,body->indirect_command_buffer,body->location,body->length);
      [encoder resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                           withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTBlitCommandCopyTexture: {
      struct wmtcmd_blit_copy_texture *body = (struct wmtcmd_blit_copy_texture *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,m4_texture_key(m4obj(body->src)),0,UINT64_MAX,m4_texture_key(m4obj(body->dst)),0,UINT64_MAX);
      [encoder copyFromTexture:(id<MTLTexture>)body->src toTexture:(id<MTLTexture>)body->dst];
      break;
    }
    }

    next = next->next.ptr;
  }
  return context->recording->error ? M4_FAILED : M4_SUCCESS;
}



