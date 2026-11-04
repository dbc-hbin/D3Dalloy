#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#import <objc/runtime.h>
#define WINEMETAL_API
#include "../winemetal_metal4.h"
#include "rt_acceleration_structure.h"
#include "airconv_public.h"

extern void winemetal_composition_present(id drawable, double duration,
    const struct WMTCompositionFrame *frame, uint32_t sync_interval, bool allow_tearing);

#define M4_SUCCESS 0
#define M4_INVALID ((int32_t)0xc000000d)
#define M4_FAILED ((int32_t)0xc0000001)
#define M4_UNSUPPORTED ((int32_t)0xc0000002)

MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return (MTLPixelFormat)ORIGINAL_FORMAT(format);
}

@class D3DALLOYM4CommandBuffer;
static char m4_view_pool_key;
static char m4_device_residency_key;
/* One residency set per MTLDevice shared by every recording (D3DMetal 4.0b3
 * DoExecute commits a single device set under one lock). Each allocation is
 * counted once per live recording that uses it and leaves the set only when
 * the last such recording retires or is dropped unsubmitted. The lock also
 * spans native queue commit, whose preCommit traverses the committed set. */
@interface D3DALLOYM4DeviceResidency : NSObject {
@public
  NSRecursiveLock *lock;
  id<MTLResidencySet> set;
  NSCountedSet *uses;
  BOOL dirty;
}
@end
@implementation D3DALLOYM4DeviceResidency
- (void)dealloc { [lock release]; [set release]; [uses release]; [super dealloc]; }
@end
static D3DALLOYM4DeviceResidency *m4_device_residency(id<MTLDevice> device) {
  @synchronized(device) {
    D3DALLOYM4DeviceResidency *shared=objc_getAssociatedObject(device,&m4_device_residency_key);
    if (shared) return shared;
    MTLResidencySetDescriptor *descriptor=[MTLResidencySetDescriptor new]; descriptor.initialCapacity=256;
    id<MTLResidencySet> set=[device newResidencySetWithDescriptor:descriptor error:nil]; [descriptor release];
    if (!set) return nil;
    shared=[D3DALLOYM4DeviceResidency new]; shared->lock=[NSRecursiveLock new]; shared->set=set; shared->uses=[NSCountedSet new];
    objc_setAssociatedObject(device,&m4_device_residency_key,shared,OBJC_ASSOCIATION_RETAIN);
    [shared release];
    return shared;
  }
}
/* Immutable unique member set of one view pool generation. It is one counted
 * residency user: its members enter the device set when its first user
 * acquires it and leave when its last user releases it. */
@interface D3DALLOYM4ViewSnapshot : NSObject {
@public
  NSArray *members;
}
@end
@implementation D3DALLOYM4ViewSnapshot
- (void)dealloc { [members release]; [super dealloc]; }
@end
static NSUInteger m4_snapshot_acquire(D3DALLOYM4DeviceResidency *shared, D3DALLOYM4ViewSnapshot *snapshot) {
  NSUInteger added=0;
  if (![shared->uses countForObject:snapshot]) {
    for (id allocation in snapshot->members) {
      if (![shared->uses countForObject:allocation]) { [shared->set addAllocation:allocation]; ++added; }
      [shared->uses addObject:allocation];
    }
  }
  [shared->uses addObject:snapshot];
  return added;
}
static NSUInteger m4_snapshot_release(D3DALLOYM4DeviceResidency *shared, D3DALLOYM4ViewSnapshot *snapshot) {
  NSUInteger removed=0;
  [shared->uses removeObject:snapshot];
  if ([shared->uses countForObject:snapshot]) return 0;
  for (id allocation in snapshot->members) {
    [shared->uses removeObject:allocation];
    if (![shared->uses countForObject:allocation]) { [shared->set removeAllocation:allocation]; ++removed; }
  }
  return removed;
}
@interface D3DALLOYM4ViewPool : NSObject {
@public
  id<MTLTextureViewPool> pool;
  MTLTextureViewDescriptor *descriptor;
  NSMutableArray *textures;
  NSCountedSet *parents;
  NSMutableDictionary *ordinaryViews;
  NSCountedSet *ordinaryOwners;
  /* Bumped only when the unique member set changes; recordings lease the
   * cached snapshot of an unchanged generation without walking members. */
  uint64_t generation, snapshotGeneration;
  D3DALLOYM4ViewSnapshot *snapshot;
  BOOL trace;
  uint64_t setCount, copyCount, clearCount, residencyVisits, ordinaryNew, ordinaryReuse;
}
- (void)replaceTexture:(id)texture atIndex:(NSUInteger)index;
- (void)replaceOrdinaryView:(id)view atIndex:(NSUInteger)index;
- (uint64_t)setTexture:(id<MTLTexture>)texture info:(const struct WMTTextureViewPoolInfo *)info atIndex:(NSUInteger)index;
@end
@implementation D3DALLOYM4ViewPool
- (void)replaceTexture:(id)texture atIndex:(NSUInteger)index {
  id previous=textures[index];
  if (previous == texture) return;
  if (previous != [NSNull null]) {
    [parents removeObject:previous];
    if (![parents countForObject:previous]) ++generation;
  }
  textures[index]=texture;
  if (texture != [NSNull null]) {
    if (![parents countForObject:texture]) ++generation;
    [parents addObject:texture];
  }
}
- (void)replaceOrdinaryView:(id)view atIndex:(NSUInteger)index {
  NSNumber *key=@(index);
  id previous=ordinaryViews[key];
  if (previous == view) return;
  if (previous) {
    [ordinaryOwners removeObject:previous];
    if (![ordinaryOwners countForObject:previous]) ++generation;
  }
  if (view) {
    if (![ordinaryOwners countForObject:view]) ++generation;
    ordinaryViews[key]=view; [ordinaryOwners addObject:view];
  }
  else [ordinaryViews removeObjectForKey:key];
}
- (uint64_t)setTexture:(id<MTLTexture>)texture info:(const struct WMTTextureViewPoolInfo *)info atIndex:(NSUInteger)index {
  descriptor.pixelFormat=to_metal_pixel_format((enum WMTPixelFormat)info->format);
  descriptor.textureType=(MTLTextureType)info->type;
  descriptor.levelRange=NSMakeRange(info->first_mip,info->mip_count);
  descriptor.sliceRange=NSMakeRange(info->first_slice,info->slice_count);
  descriptor.swizzle=MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(info->format,0,MTLTextureSwizzleRed),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(info->format,1,MTLTextureSwizzleGreen),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(info->format,2,MTLTextureSwizzleBlue),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(info->format,3,MTLTextureSwizzleAlpha));
  uint64_t resource;
  if (pool) {
    resource=[pool setTextureView:texture descriptor:descriptor atIndex:index]._impl;
    if (trace) {
      ++setCount;
      fprintf(stderr,"OWNED_GRAPHICS_NATIVE_VIEW_POOL_SET slot_set=1 wrapper=%llu slot=%lu\n",(unsigned long long)(uintptr_t)self,(unsigned long)index);
    }
  } else {
    id<MTLTexture> view=[texture newTextureViewWithPixelFormat:descriptor.pixelFormat textureType:descriptor.textureType
        levels:descriptor.levelRange slices:descriptor.sliceRange swizzle:descriptor.swizzle];
    if (!view) return 0;
    resource=view.gpuResourceID._impl;
    [self replaceOrdinaryView:view atIndex:index];
    if (trace) {
      ++ordinaryNew;
      fprintf(stderr,"OWNED_GRAPHICS_NATIVE_ORDINARY_VIEW texture=%llu view=%llu format=%u type=%u first_mip=%u mip_count=%u first_slice=%u slice_count=%u reason=pool_factory_nil\n",
          (unsigned long long)(uintptr_t)texture,(unsigned long long)(uintptr_t)view,info->format,info->type,info->first_mip,info->mip_count,info->first_slice,info->slice_count);
    }
    [view release];
  }
  [self replaceTexture:texture atIndex:index];
  return resource;
}
- (void)dealloc {
  if (trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_VIEW_POOL pool_new=%u pool_fallback=%u slot_set=%llu slot_copy=%llu slot_clear=%llu slots=%lu unique_parent_visits=%llu ordinary_view_new=%llu ordinary_view_reuse=%llu\n",
      pool?1:0,pool?0:1,(unsigned long long)setCount,(unsigned long long)copyCount,(unsigned long long)clearCount,(unsigned long)textures.count,(unsigned long long)residencyVisits,(unsigned long long)ordinaryNew,(unsigned long long)ordinaryReuse);
  [pool release]; [descriptor release]; [textures release]; [parents release]; [snapshot release];
  [ordinaryViews release]; [ordinaryOwners release]; [super dealloc];
}
@end
typedef struct {
  id<MTL4ArgumentTable> table;
  void (*address)(id, SEL, MTLGPUAddress, NSUInteger);
  void (*resource)(id, SEL, MTLResourceID, NSUInteger);
  void (*texture)(id, SEL, MTLResourceID, NSUInteger);
  struct { uint64_t value, offset; id object; BOOL resource; } buffers[31];
  uint64_t textures[128];
  // Slots written since the last reset; every other entry is zero.
  uint32_t bufferMask;
  uint64_t textureMask[2];
} M4TableState;
typedef struct {
  uint64_t tableNew, tableSet, tableBind, residencyNew, residencyAdd, residencyRemove, residencyCommit;
  uint64_t blitHazardBarrier, blitCapacityBarrier;
} M4BindingCounters;
@interface D3DALLOYM4Recording : NSObject {
@public
  D3DALLOYM4DeviceResidency *residency;
  NSMutableSet *objects;
  NSMutableSet *allocations;
  NSMutableArray *pending, *published;
  BOOL released, trace;
  M4BindingCounters counters;
  NSMutableArray *encoders;
  NSMutableArray<id<MTL4CommandBuffer>> *segments;
  NSMutableArray *presentTickets;
  NSError *error;
}
@end
static void m4_release_residency(D3DALLOYM4Recording *recording);
@implementation D3DALLOYM4Recording
- (void)dealloc {
  // A recording dropped without submission still holds published uses.
  m4_release_residency(self);
  [residency release]; [objects release]; [allocations release]; [pending release]; [published release]; [encoders release]; [segments release];
  [presentTickets release]; [error release]; [super dealloc];
}
@end

@interface D3DALLOYM4PresentTicket : NSObject {
@public
  id drawable, surface;
  struct WMTCompositionFrame frame;
  double duration;
  uint32_t sync_interval;
  BOOL allow_tearing;
}
- (id)initWithCall:(struct WMTMetal4Call *)call;
- (void)present;
@end

@implementation D3DALLOYM4PresentTicket
- (id)initWithCall:(struct WMTMetal4Call *)call {
  self = [super init];
  if (!self) return nil;
  drawable = [(id)(uintptr_t)call->args[1] retain];
  if (!drawable) { [self release]; return nil; }
  memcpy(&duration, &call->args[2], sizeof(duration));
  if (call->args[3]) {
    frame = *(const struct WMTCompositionFrame *)(uintptr_t)call->args[3];
    surface = [(id)(uintptr_t)frame.surface retain];
  }
  sync_interval = (uint32_t)call->args[4];
  allow_tearing = call->args[5] != 0;
  return self;
}
/* Called right after commit and signalDrawable, as D3DMetal 4.0b3 DoPresent
 * does: Core Animation waits for the signaled GPU work, so no CPU completion
 * wait sits between the GPU finishing the frame and its presentation. */
- (void)present {
  winemetal_composition_present(drawable, duration, surface ? &frame : NULL, sync_interval, allow_tearing);
}
- (void)dealloc {
  [drawable release]; [surface release]; [super dealloc];
}
@end

@interface D3DALLOYM4CommandBuffer : NSObject {
@public
  id<MTLDevice> device;
  id<MTL4CommandBuffer> command;
  id<MTL4CommandAllocator> allocator;
  D3DALLOYM4Recording *recording;
  D3DALLOYM4DeviceResidency *residency;
  M4TableState tables[5]; // Render stages 0-3; compute has a separate table.
  NSMutableArray<id<MTLBuffer>> *uploads;
  NSMutableArray<NSString *> *debugGroups;
  NSUInteger uploadIndex, uploadOffset;
  _Atomic(uint32_t) inFlight;
  BOOL ended;
}
@end
@implementation D3DALLOYM4CommandBuffer
- (void)dealloc {
  [device release]; [command release]; [allocator release]; [recording release]; [residency release];
  for (unsigned i=0;i<5;++i) [tables[i].table release];
  [uploads release]; [debugGroups release]; [super dealloc];
}
@end

@interface D3DALLOYM4Encoder : NSObject {
@public
  D3DALLOYM4CommandBuffer *owner;
  D3DALLOYM4Recording *recording;
  id<MTL4CommandEncoder> encoder;
  BOOL tableReady[4], tableBound[4], compute;
  id<MTLBuffer> buffers[4][31];
  MTLStages orderedFor[3];
  MTLStages fenceWaitStages;
  uint64_t tracePSO; // Borrowed identity only; recording already retains the native PSO.
}
@end
@implementation D3DALLOYM4Encoder
- (void)dealloc {
  [encoder release]; [super dealloc];
}
@end
/* One blit footprint: a byte range of a buffer (or buffer-backed texture), or
 * a level/slice/region box of a root texture. */
typedef struct {
  uint64_t resource, begin, end;
  uint32_t level_begin, level_end, slice_begin, slice_end;
  uint64_t x0, x1, y0, y1, z0, z1;
  BOOL write, texture;
} M4BlitAccess;
@interface D3DALLOYM4ComputeEncoder : D3DALLOYM4Encoder {
@public
  MTLSize threadgroupSize;
  BOOL concurrent;
  MTLStages pendingStages;
  M4BlitAccess blitAccesses[32];
  unsigned blitAccessCount;
}
@end
@implementation D3DALLOYM4ComputeEncoder
@end
@interface D3DALLOYM4RenderEncoder : D3DALLOYM4Encoder {
@public
  /* Last indexed-draw buffer of this encoder, already leased by its recording.
   * Repeated draws from one index buffer skip the lease and ObjC queries. */
  uint64_t indexHandle;
  MTLGPUAddress indexAddress;
  NSUInteger indexLength;
}
@end
@implementation D3DALLOYM4RenderEncoder
@end

@interface D3DALLOYM4Submission : NSObject {
@public
  NSCondition *condition;
  NSMutableArray *leases;
  NSMutableArray<D3DALLOYM4CommandBuffer *> *buffers;
  NSMutableArray *presentTickets;
  NSError *error;
  uint64_t gpuStart, gpuEnd;
  BOOL completed, commitReturned, feedbackReceived;
}
@end
@implementation D3DALLOYM4Submission
- (void)dealloc {
  [condition release]; [leases release]; [buffers release]; [presentTickets release]; [error release]; [super dealloc];
}
@end

@interface D3DALLOYM4ASDescriptor : NSObject {
@public
  MTL4AccelerationStructureDescriptor *descriptor;
  NSMutableArray *geometries;
  NSMutableSet *inputs;
  id<MTLDevice> device;
  id<MTLBuffer> placeholder;
  NSMutableIndexSet *unresolvedGeometries;
  BOOL unresolvedInstances;
}
@end
@implementation D3DALLOYM4ASDescriptor
- (void)dealloc {
  [descriptor release]; [geometries release]; [inputs release]; [placeholder release]; [device release]; [unresolvedGeometries release]; [super dealloc];
}
@end

static inline id m4obj(uint64_t handle) { return (id)(uintptr_t)handle; }
static inline D3DALLOYM4CommandBuffer *m4cb(uint64_t handle) { return (D3DALLOYM4CommandBuffer *)m4obj(handle); }
static void m4_error(D3DALLOYM4Recording *recording, NSString *description) {
  if (!recording->error)
    recording->error=[[NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorInternal
      userInfo:@{NSLocalizedDescriptionKey:description}] retain];
}
static void m4_retain(D3DALLOYM4Recording *recording, id object) {
  if (object) [recording->objects addObject:object];
}
static void m4_allocation(D3DALLOYM4Recording *recording, id<MTLAllocation> allocation) {
  if (!allocation) return;
  D3DALLOYM4ViewPool *views=objc_getAssociatedObject(allocation,&m4_view_pool_key);
  if (views && ![recording->objects containsObject:views]) {
    // The recording, not the mutable descriptor heap, owns this GPU-use lease:
    // the immutable snapshot of the heap's current members.
    m4_retain(recording,views);
    D3DALLOYM4ViewSnapshot *snapshot;
    @synchronized(views) {
      if (!views->snapshot || views->snapshotGeneration!=views->generation) {
        NSMutableArray *members=[[NSMutableArray alloc] initWithCapacity:views->parents.count+views->ordinaryOwners.count];
        for (id texture in views->parents) {
          if (views->trace) ++views->residencyVisits;
          [members addObject:texture];
        }
        for (id view in views->ordinaryOwners) [members addObject:view];
        [views->snapshot release];
        views->snapshot=[D3DALLOYM4ViewSnapshot new]; views->snapshot->members=members;
        views->snapshotGeneration=views->generation;
      }
      snapshot=views->snapshot;
      m4_retain(recording,snapshot);
    }
    if (![recording->allocations containsObject:snapshot]) {
      [recording->allocations addObject:snapshot]; [recording->pending addObject:snapshot];
    }
  }
  [recording->objects addObject:allocation];
  if (![recording->allocations containsObject:allocation]) {
    [recording->allocations addObject:allocation]; [recording->pending addObject:allocation];
  }
}
/* Publish this recording's new uses before its command buffer ends. Only an
 * allocation no live recording holds changes the set, so steady-state frames
 * commit nothing. */
static void m4_commit_residency(D3DALLOYM4Recording *recording) {
  if (!recording->pending.count) return;
  D3DALLOYM4DeviceResidency *shared=recording->residency;
  BOOL dirty=NO;
  [shared->lock lock];
  for (id allocation in recording->pending) {
    if ([allocation isKindOfClass:[D3DALLOYM4ViewSnapshot class]]) {
      NSUInteger added=m4_snapshot_acquire(shared,allocation);
      if (added) {
        dirty=YES;
        if (recording->trace) recording->counters.residencyAdd+=added;
      }
      continue;
    }
    if (![shared->uses countForObject:allocation]) {
      [shared->set addAllocation:allocation]; dirty=YES;
      if (recording->trace) ++recording->counters.residencyAdd;
    }
    [shared->uses addObject:allocation];
  }
  if (dirty) {
    [shared->set commit];
    if (recording->trace) ++recording->counters.residencyCommit;
  }
  [shared->lock unlock];
  [recording->published addObjectsFromArray:recording->pending];
  [recording->pending removeAllObjects];
}
/* Drop this recording's uses after native commit return and GPU retirement, or
 * when the recording dies unsubmitted. Removal is never visible to work that
 * still uses the allocation: another live user keeps its count above zero. */
static void m4_release_residency(D3DALLOYM4Recording *recording) {
  if (recording->released || !recording->residency) return;
  recording->released=YES;
  if (!recording->published.count) return;
  D3DALLOYM4DeviceResidency *shared=recording->residency;
  BOOL dirty=NO;
  [shared->lock lock];
  for (id allocation in recording->published) {
    if ([allocation isKindOfClass:[D3DALLOYM4ViewSnapshot class]]) {
      NSUInteger removed=m4_snapshot_release(shared,allocation);
      if (removed) {
        dirty=YES;
        if (recording->trace) recording->counters.residencyRemove+=removed;
      }
      continue;
    }
    [shared->uses removeObject:allocation];
    if (![shared->uses countForObject:allocation]) {
      [shared->set removeAllocation:allocation]; dirty=YES;
      if (recording->trace) ++recording->counters.residencyRemove;
    }
  }
  if (dirty) {
    [shared->set commit];
    if (recording->trace) ++recording->counters.residencyCommit;
  }
  if (recording->trace)
    fprintf(stderr,"OWNED_GRAPHICS_NATIVE_RESIDENCY live_allocations=%lu\n",(unsigned long)shared->uses.count);
  [shared->lock unlock];
  [recording->published removeAllObjects];
}

id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t handle) { return handle ? m4cb(handle)->command : nil; }
int32_t winemetal_metal4_reserve_present(struct WMTMetal4Call *call) {
  if (!call || !call->args[0]) return M4_INVALID;
  D3DALLOYM4CommandBuffer *context = m4cb(call->args[0]);
  if (!context || !context->recording || context->ended) return M4_INVALID;
  D3DALLOYM4PresentTicket *ticket = [[D3DALLOYM4PresentTicket alloc] initWithCall:call];
  if (!ticket) return M4_FAILED;
  [context->recording->presentTickets addObject:ticket];
  [ticket release];
  call->ret[0] = 1;
  return M4_SUCCESS;
}
void *winemetal_metal4_split_command_buffer(void *opaque) {
  D3DALLOYM4CommandBuffer *context = opaque;
  if (!context || !context->recording || context->ended) return NULL;
  id<MTL4CommandBuffer> next = [context->device newCommandBuffer];
  if (!next) return NULL;
  // All segments share the existing recording, allocator and retirement owner.
  if (!context->recording->segments) context->recording->segments = [NSMutableArray new];
  m4_commit_residency(context->recording);
  [context->command useResidencySet:context->recording->residency->set];
  for (NSUInteger i=0;i<context->debugGroups.count;++i) [context->command popDebugGroup];
  [context->command endCommandBuffer];
  [context->recording->segments addObject:context->command];
  [context->command release]; context->command = next;
  [next beginCommandBufferWithAllocator:context->allocator];
  next.label=context->recording->segments.lastObject.label;
  for (NSString *group in context->debugGroups) [next pushDebugGroup:group];
  return next;
}
id<MTL4ComputeCommandEncoder> winemetal_metal4_compute_encoder(obj_handle_t handle) {
  return handle ? (id<MTL4ComputeCommandEncoder>)((D3DALLOYM4ComputeEncoder *)m4obj(handle))->encoder : nil;
}
id<MTL4RenderCommandEncoder> winemetal_metal4_render_encoder(obj_handle_t handle) {
  return handle ? (id<MTL4RenderCommandEncoder>)((D3DALLOYM4RenderEncoder *)m4obj(handle))->encoder : nil;
}
void winemetal_metal4_retain_resource(obj_handle_t handle, id resource) { m4_retain(m4cb(handle)->recording,resource); }
void winemetal_metal4_use_allocation(obj_handle_t handle, id<MTLAllocation> resource) { m4_allocation(m4cb(handle)->recording,resource); }

static id<MTL4ArgumentTable> m4_table(D3DALLOYM4Encoder *context, unsigned stage) {
  M4TableState *state=&context->owner->tables[context->compute ? 4 : stage];
  if (!state->table) {
    MTL4ArgumentTableDescriptor *descriptor=[MTL4ArgumentTableDescriptor new];
    descriptor.maxBufferBindCount=31; descriptor.maxTextureBindCount=128; descriptor.maxSamplerStateBindCount=16;
    descriptor.initializeBindings=YES;
    state->table=[context->owner->device newArgumentTableWithDescriptor:descriptor error:nil];
    [descriptor release];
    if (!state->table) { m4_error(context->recording,@"Metal4 argument table allocation failed"); return nil; }
    // Cache only the implementations of this actual table class, including validation wrappers.
    Class cls=object_getClass(state->table);
    state->address=(void (*)(id,SEL,MTLGPUAddress,NSUInteger))class_getMethodImplementation(cls,@selector(setAddress:atIndex:));
    state->resource=(void (*)(id,SEL,MTLResourceID,NSUInteger))class_getMethodImplementation(cls,@selector(setResource:atBufferIndex:));
    state->texture=(void (*)(id,SEL,MTLResourceID,NSUInteger))class_getMethodImplementation(cls,@selector(setTexture:atIndex:));
    if (context->recording->trace) ++context->recording->counters.tableNew;
  }
  if (!context->tableReady[stage]) {
    // New encoders formerly received nil-initialized tables. Restore that contract
    // before publishing any new binding; prior draws/dispatches own native snapshots.
    for (uint32_t mask=state->bufferMask;mask;mask&=mask-1) {
      const unsigned i=__builtin_ctz(mask);
      if (state->buffers[i].value) {
        state->address(state->table,@selector(setAddress:atIndex:),0,i);
        if (context->recording->trace) ++context->recording->counters.tableSet;
      }
      state->buffers[i]=(typeof(state->buffers[0])){0};
    }
    for (unsigned word=0;word<2;++word)
      for (uint64_t mask=state->textureMask[word];mask;mask&=mask-1) {
        const unsigned i=word*64+__builtin_ctzll(mask);
        if (state->textures[i]) {
          state->texture(state->table,@selector(setTexture:atIndex:),(MTLResourceID){0},i);
          if (context->recording->trace) ++context->recording->counters.tableSet;
        }
        state->textures[i]=0;
      }
    state->bufferMask=0; state->textureMask[0]=state->textureMask[1]=0;
    context->tableReady[stage]=YES; m4_retain(context->recording,state->table);
  }
  return state->table;
}
static const MTLRenderStages m4_render_stages[4]={MTLRenderStageVertex,MTLRenderStageFragment,MTLRenderStageMesh,MTLRenderStageObject};
static void m4_render_table(D3DALLOYM4RenderEncoder *context, unsigned stage) {
  if (!context->tableBound[stage]) {
    [(id<MTL4RenderCommandEncoder>)context->encoder setArgumentTable:m4_table(context,stage) atStages:m4_render_stages[stage]];
    context->tableBound[stage]=YES;
    if (context->recording->trace) ++context->recording->counters.tableBind;
  }
}
static void m4_compute_table(D3DALLOYM4ComputeEncoder *context) {
  if (!context->tableBound[0]) {
    [(id<MTL4ComputeCommandEncoder>)context->encoder setArgumentTable:m4_table(context,0)];
    context->tableBound[0]=YES;
    if (context->recording->trace) ++context->recording->counters.tableBind;
  }
}
static void m4_bind_resource(D3DALLOYM4Encoder *context, unsigned stage, MTLResourceID resource, unsigned index, BOOL texture) {
  id<MTL4ArgumentTable> table=m4_table(context,stage); if (!table) return;
  M4TableState *state=&context->owner->tables[context->compute ? 4 : stage];
  if (texture) {
    if (state->textures[index]==resource._impl) return;
    state->texture(table,@selector(setTexture:atIndex:),resource,index); state->textures[index]=resource._impl;
    state->textureMask[index/64]|=1ull<<(index%64);
  } else {
    if (state->buffers[index].resource && state->buffers[index].value==resource._impl) return;
    state->resource(table,@selector(setResource:atBufferIndex:),resource,index);
    state->buffers[index]=(typeof(state->buffers[0])){.value=resource._impl,.resource=YES};
    state->bufferMask|=1u<<index;
    context->buffers[stage][index]=nil;
  }
  if (context->recording->trace) ++context->recording->counters.tableSet;
}
static void m4_record_barrier(D3DALLOYM4Encoder *context, MTLStages after, MTLStages before) {
  if (before & MTLStageDispatch) context->orderedFor[0] |= after;
  if (before & MTLStageBlit) context->orderedFor[1] |= after;
  if (before & MTLStageAccelerationStructure) context->orderedFor[2] |= after;
}
static void m4_stage_barrier(D3DALLOYM4ComputeEncoder *context, MTLStages stage) {
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
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_STAGE_BARRIER encoder=%llu after=%llu before=%llu implicit=1\n",
          (unsigned long long)(uintptr_t)context->encoder,(unsigned long long)after,(unsigned long long)stage);
      m4_record_barrier(context,after,stage);
      if (stage==MTLStageBlit) context->blitAccessCount=0;
    }
  }
  context->pendingStages |= stage;
  for (unsigned i=0;i<3;++i) context->orderedFor[i] &= ~stage;
}
static uint64_t m4_add_clamped(uint64_t a, uint64_t b) { return b>UINT64_MAX-a ? UINT64_MAX : a+b; }
static uint64_t m4_mul_clamped(uint64_t a, uint64_t b) { return a && b>UINT64_MAX/a ? UINT64_MAX : a*b; }
static M4BlitAccess m4_buffer_footprint(uint64_t buffer, uint64_t offset, uint64_t length, BOOL write) {
  return (M4BlitAccess){.resource=buffer,.begin=offset,.end=m4_add_clamped(offset,length),.write=write};
}
/* Upper bound of the bytes a buffer<->texture copy touches: every image but
 * the last spans bytesPerImage, and an image spans at most bytesPerRow per
 * pixel row (block rows are never more than pixel rows). Unknown pitches
 * cover the rest of the buffer. */
static M4BlitAccess m4_linear_footprint(uint64_t buffer, uint64_t offset, uint64_t bytesPerRow, uint64_t bytesPerImage,
                                        struct WMTSize size, BOOL write) {
  uint64_t image=bytesPerRow ? m4_mul_clamped(bytesPerRow,size.height) : UINT64_MAX;
  uint64_t length=size.depth>1 ? m4_add_clamped(m4_mul_clamped(bytesPerImage ? bytesPerImage : UINT64_MAX,size.depth-1),image) : image;
  if (!size.width || !size.height || !size.depth) length=0;
  return m4_buffer_footprint(buffer,offset,length,write);
}
/* A texture view's level/slice/region in its root texture. Buffer-backed
 * textures and views whose format may change texel units fall back to the
 * whole buffer or the whole level/slice. */
static M4BlitAccess m4_texture_footprint(id<MTLTexture> texture, uint32_t level, uint32_t levels, uint32_t slice,
                                         uint32_t slices, struct WMTOrigin origin, struct WMTSize size, BOOL write) {
  BOOL region=YES;
  id<MTLTexture> root=texture;
  while (root.parentTexture) {
    if (root.pixelFormat!=root.parentTexture.pixelFormat) region=NO;
    level=level+(uint32_t)root.parentRelativeLevel;
    slice=slice+(uint32_t)root.parentRelativeSlice;
    root=root.parentTexture;
  }
  if (root.buffer) return m4_buffer_footprint((uint64_t)root.buffer,0,UINT64_MAX,write);
  M4BlitAccess access={.resource=(uint64_t)root,.level_begin=level,.level_end=levels==UINT32_MAX ? UINT32_MAX : level+levels,
    .slice_begin=slice,.slice_end=slices==UINT32_MAX ? UINT32_MAX : slice+slices,
    .x1=UINT64_MAX,.y1=UINT64_MAX,.z1=UINT64_MAX,.write=write,.texture=YES};
  if (region && levels==1) {
    access.x0=origin.x; access.x1=m4_add_clamped(origin.x,size.width);
    access.y0=origin.y; access.y1=m4_add_clamped(origin.y,size.height);
    access.z0=origin.z; access.z1=m4_add_clamped(origin.z,size.depth);
  }
  if (!size.width || !size.height || !size.depth) access.slice_end=access.slice_begin;
  return access;
}
static M4BlitAccess m4_whole_texture(id<MTLTexture> texture, BOOL write) {
  return m4_texture_footprint(texture,0,UINT32_MAX,0,UINT32_MAX,(struct WMTOrigin){0,0,0},
                              (struct WMTSize){UINT64_MAX,UINT64_MAX,UINT64_MAX},write);
}
static BOOL m4_blit_empty(const M4BlitAccess *a) {
  return !a->resource || (a->texture ? a->level_begin>=a->level_end || a->slice_begin>=a->slice_end ||
      a->x0>=a->x1 || a->y0>=a->y1 || a->z0>=a->z1 : a->begin>=a->end);
}
static BOOL m4_blit_overlaps(const M4BlitAccess *a, const M4BlitAccess *b) {
  if (a->resource!=b->resource) return NO;
  if (a->texture!=b->texture) return YES;
  if (!a->texture) return a->begin<b->end && b->begin<a->end;
  return a->level_begin<b->level_end && b->level_begin<a->level_end && a->slice_begin<b->slice_end &&
      b->slice_begin<a->slice_end && a->x0<b->x1 && b->x0<a->x1 && a->y0<b->y1 && b->y0<a->y1 && a->z0<b->z1 && b->z0<a->z1;
}
static BOOL m4_blit_covers(const M4BlitAccess *a, const M4BlitAccess *b) {
  if (a->resource!=b->resource || a->texture!=b->texture || (b->write && !a->write)) return NO;
  if (!a->texture) return a->begin<=b->begin && b->end<=a->end;
  return a->level_begin<=b->level_begin && b->level_end<=a->level_end && a->slice_begin<=b->slice_begin &&
      b->slice_end<=a->slice_end && a->x0<=b->x0 && b->x1<=a->x1 && a->y0<=b->y0 && b->y1<=a->y1 &&
      a->z0<=b->z0 && b->z1<=a->z1;
}
/* The bounding footprint of two same-kind accesses of one resource. A larger
 * footprint only adds conflicts, so replacing both by it stays ordered. */
static M4BlitAccess m4_blit_bound(const M4BlitAccess *a, const M4BlitAccess *b) {
  M4BlitAccess u=*a;
  u.write=a->write || b->write;
  #define M4_LO(f) if (b->f<u.f) u.f=b->f
  #define M4_HI(f) if (b->f>u.f) u.f=b->f
  M4_LO(begin); M4_HI(end); M4_LO(level_begin); M4_HI(level_end); M4_LO(slice_begin); M4_HI(slice_end);
  M4_LO(x0); M4_HI(x1); M4_LO(y0); M4_HI(y1); M4_LO(z0); M4_HI(z1);
  #undef M4_LO
  #undef M4_HI
  return u;
}
static BOOL m4_blit_adjacent(const M4BlitAccess *a, const M4BlitAccess *b) {
  if (a->resource!=b->resource || a->texture!=b->texture || a->write!=b->write) return NO;
  if (!a->texture) return a->begin<=b->end && b->begin<=a->end;
  /* Consecutive slices of one level and region. */
  return a->level_begin==b->level_begin && a->level_end==b->level_end && a->x0==b->x0 && a->x1==b->x1 &&
      a->y0==b->y0 && a->y1==b->y1 && a->z0==b->z0 && a->z1==b->z1 &&
      a->slice_begin<=b->slice_end && b->slice_begin<=a->slice_end;
}
/* Absorbs an access into an entry that covers it, or widens a covered or
 * adjacent entry. Returns NO when it needs its own slot. */
static BOOL m4_blit_absorb(D3DALLOYM4ComputeEncoder *context, const M4BlitAccess *access) {
  for (unsigned i=0;i<context->blitAccessCount;++i) {
    M4BlitAccess *prior=&context->blitAccesses[i];
    if (m4_blit_covers(prior,access)) return YES;
    if (m4_blit_covers(access,prior) || m4_blit_adjacent(prior,access)) {
      *prior=m4_blit_bound(prior,access);
      /* The widened entry may now cover later ones. */
      for (unsigned j=i+1;j<context->blitAccessCount;)
        if (m4_blit_covers(prior,&context->blitAccesses[j]))
          context->blitAccesses[j]=context->blitAccesses[--context->blitAccessCount];
        else ++j;
      return YES;
    }
  }
  return NO;
}
/* Frees two slots before a copy records its footprints. Two same-kind
 * entries of one resource merge into their bound first; only when nothing
 * merges does one barrier order every earlier copy before this one. */
static void m4_blit_reserve(D3DALLOYM4ComputeEncoder *context) {
  const unsigned capacity=sizeof(context->blitAccesses)/sizeof(context->blitAccesses[0]);
  for (unsigned i=0;i<context->blitAccessCount && context->blitAccessCount>capacity-2;++i)
    for (unsigned j=i+1;j<context->blitAccessCount && context->blitAccessCount>capacity-2;) {
      M4BlitAccess *a=&context->blitAccesses[i], *b=&context->blitAccesses[j];
      if (a->resource!=b->resource || a->texture!=b->texture || a->write!=b->write) { ++j; continue; }
      *a=m4_blit_bound(a,b);
      *b=context->blitAccesses[--context->blitAccessCount];
    }
  if (context->blitAccessCount>capacity-2) {
    [context->encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    if (context->recording->trace) ++context->recording->counters.blitCapacityBarrier;
    context->blitAccessCount=0;
  }
}
static void m4_blit_access(D3DALLOYM4ComputeEncoder *context, M4BlitAccess source, M4BlitAccess destination) {
  m4_stage_barrier(context,MTLStageBlit);
  const M4BlitAccess accesses[2]={source,destination};
  BOOL conflict=NO;
  for (unsigned j=0;j<2 && !conflict;++j)
    for (unsigned i=0;i<context->blitAccessCount && !conflict;++i)
      conflict=!m4_blit_empty(&accesses[j]) && (accesses[j].write || context->blitAccesses[i].write) &&
        m4_blit_overlaps(&accesses[j],&context->blitAccesses[i]);
  if (conflict) {
    [context->encoder barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    if (context->recording->trace) ++context->recording->counters.blitHazardBarrier;
    context->blitAccessCount=0;
  }
  m4_blit_reserve(context);
  for (unsigned j=0;j<2;++j)
    if (!m4_blit_empty(&accesses[j]) && !m4_blit_absorb(context,&accesses[j]))
      context->blitAccesses[context->blitAccessCount++]=accesses[j];
}
static void m4_bind_buffer(D3DALLOYM4Encoder *context, unsigned stage, id<MTLBuffer> buffer, uint64_t offset, unsigned index) {
  if (index>=31 || (buffer && offset>buffer.length)) { m4_error(context->recording,@"Metal4 buffer binding is out of range"); return; }
  context->buffers[stage][index]=buffer;
  M4TableState *state=&context->owner->tables[context->compute ? 4 : stage];
  /* A ready table was reset when this encoder (and so this recording) first used it, so a matching
   * entry was bound, and its allocation leased, by this recording. */
  if (buffer && context->tableReady[stage] && state->buffers[index].object==buffer &&
      !state->buffers[index].resource && state->buffers[index].offset==offset) return;
  m4_allocation(context->recording,buffer);
  id<MTL4ArgumentTable> table=m4_table(context,stage); if (!table) return;
  uint64_t address=buffer ? buffer.gpuAddress+offset : 0;
  if (!state->buffers[index].resource && state->buffers[index].value==address &&
      state->buffers[index].object==buffer && state->buffers[index].offset==offset) return;
  state->address(table,@selector(setAddress:atIndex:),address,index);
  state->buffers[index]=(typeof(state->buffers[0])){.value=address,.offset=offset,.object=buffer};
  state->bufferMask|=1u<<index;
  if (context->recording->trace) ++context->recording->counters.tableSet;
}
static void m4_render_buffer(D3DALLOYM4RenderEncoder *context, unsigned stage, id<MTLBuffer> buffer, uint64_t offset, unsigned index) {
  m4_bind_buffer(context,stage,buffer,offset,index); m4_render_table(context,stage);
}
static void m4_render_offset(D3DALLOYM4RenderEncoder *context, unsigned stage, uint64_t offset, unsigned index) {
  if (index>=31) { m4_error(context->recording,@"Metal4 buffer offset index is out of range"); return; }
  m4_render_buffer(context,stage,context->buffers[stage][index],offset,index);
}
static void m4_compute_buffer(D3DALLOYM4ComputeEncoder *context, id<MTLBuffer> buffer, uint64_t offset, unsigned index) {
  m4_bind_buffer(context,0,buffer,offset,index); m4_compute_table(context);
}
static id<MTLBuffer> m4_upload(D3DALLOYM4CommandBuffer *context, const void *bytes, uint64_t length, NSUInteger *offset) {
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
static void m4_render_bytes(D3DALLOYM4RenderEncoder *context, const void *bytes, uint64_t length, unsigned index) {
  NSUInteger offset=0; id<MTLBuffer> buffer=m4_upload(context->owner,bytes,length,&offset);
  if (buffer) m4_render_buffer(context,1,buffer,offset,index);
}
static MTLGPUAddress m4_address(D3DALLOYM4Recording *recording, uint64_t handle, uint64_t offset) {
  id<MTLBuffer> buffer=m4obj(handle); m4_allocation(recording,buffer);
  if (!buffer || offset>buffer.length) { m4_error(recording,@"Metal4 indirect/index buffer is out of range"); return 0; }
  return buffer.gpuAddress+offset;
}

/* Resolves an indexed-draw buffer range for one render encoder. An encoder
 * belongs to exactly one recording, so a cached buffer remains leased by it. */
static BOOL m4_index_range(D3DALLOYM4RenderEncoder *context, uint64_t handle, uint64_t offset,
                           MTLGPUAddress *address, NSUInteger *length) {
  if (!handle || handle != context->indexHandle) {
    id<MTLBuffer> buffer=m4obj(handle); m4_allocation(context->recording,buffer);
    if (!buffer) {
      m4_error(context->recording,@"Metal4 indirect/index buffer is out of range"); *address=0; *length=0;
      return NO;
    }
    context->indexHandle=handle; context->indexAddress=buffer.gpuAddress; context->indexLength=buffer.length;
  }
  if (offset>context->indexLength) {
    m4_error(context->recording,@"Metal4 indirect/index buffer is out of range"); *address=0; *length=0;
    return NO;
  }
  *address=context->indexAddress+offset; *length=context->indexLength-offset;
  return YES;
}

static MTL4RenderPassDescriptor *m4_render_pass(D3DALLOYM4CommandBuffer *context, const struct WMTRenderPassInfo *info) {
  MTL4RenderPassDescriptor *descriptor=[MTL4RenderPassDescriptor new];
  descriptor.supportColorAttachmentMapping=info->support_color_attachment_mapping != 0;
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
  if (info->center_sample_pattern || info->sample_positions.count) {
    NSUInteger count=info->default_raster_sample_count;
    for (unsigned i=0;i<8 && !count;++i) count=[descriptor.colorAttachments[i].texture sampleCount];
    if (!count) count=[descriptor.depthAttachment.texture sampleCount];
    if (!count) count=[descriptor.stencilAttachment.texture sampleCount];
    if (count!=2 && count!=4 && count!=8) { [descriptor release]; m4_error(context->recording,@"Invalid center sample count"); return nil; }
    if (info->sample_positions.count && info->sample_positions.count!=count) {
      [descriptor release]; m4_error(context->recording,@"Invalid programmable sample count"); return nil;
    }
    MTLSamplePosition positions[8];
    for (NSUInteger i=0;i<count;++i)
      positions[i]=info->sample_positions.count
          ? MTLSamplePositionMake(.5f+info->sample_positions.positions[i][0]/16.f,
                                  .5f+info->sample_positions.positions[i][1]/16.f)
          : MTLSamplePositionMake(.5f,.5f);
    [descriptor setSamplePositions:positions count:count];
    // Metal accepts the option only for store actions that keep multisample data.
    for (unsigned i=0;i<8;++i)
      if (descriptor.colorAttachments[i].texture &&
          (descriptor.colorAttachments[i].storeAction==MTLStoreActionStore ||
           descriptor.colorAttachments[i].storeAction==MTLStoreActionStoreAndMultisampleResolve))
        descriptor.colorAttachments[i].storeActionOptions=MTLStoreActionOptionCustomSamplePositions;
    if (descriptor.depthAttachment.texture &&
        (descriptor.depthAttachment.storeAction==MTLStoreActionStore ||
         descriptor.depthAttachment.storeAction==MTLStoreActionStoreAndMultisampleResolve))
      descriptor.depthAttachment.storeActionOptions=MTLStoreActionOptionCustomSamplePositions;
  }
  if (info->forced16_half) {
    if (info->forced16_half>2 || info->default_raster_sample_count!=8 || info->center_sample_pattern || info->sample_positions.count) {
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


static int32_t m4_compute_commands(D3DALLOYM4ComputeEncoder *context, const struct wmtcmd_base *next);
static int32_t m4_render_commands(D3DALLOYM4RenderEncoder *context, const struct wmtcmd_base *next);
static int32_t m4_blit_commands(D3DALLOYM4ComputeEncoder *context, const struct wmtcmd_base *next);

static MTL4BufferRange m4_buffer_range(id<MTLBuffer> buffer, uint64_t offset) {
  return buffer && offset<=buffer.length ? MTL4BufferRangeMake(buffer.gpuAddress+offset,buffer.length-offset) : MTL4BufferRangeMake(0,0);
}
static id<MTLBuffer> m4_as_placeholder(D3DALLOYM4ASDescriptor *owner) {
  if (!owner->placeholder) {
    owner->placeholder=[owner->device newBufferWithLength:256 options:MTLResourceStorageModePrivate];
    if (owner->placeholder) [owner->inputs addObject:owner->placeholder];
  }
  return owner->placeholder;
}
static D3DALLOYM4ASDescriptor *m4_as_new(id<MTLDevice> device, BOOL topLevel, uint32_t geometryCount) {
  D3DALLOYM4ASDescriptor *owner=[D3DALLOYM4ASDescriptor new];
  owner->inputs=[NSMutableSet new];
  owner->device=[device retain];
  if (topLevel) owner->descriptor=[MTL4InstanceAccelerationStructureDescriptor new];
  else {
    owner->descriptor=[MTL4PrimitiveAccelerationStructureDescriptor new];
    owner->geometries=[[NSMutableArray alloc] initWithCapacity:MAX(geometryCount,1u)];
    for (uint32_t i=0;i<geometryCount;++i) [owner->geometries addObject:[NSNull null]];
    if (!geometryCount) {
      if (!m4_as_placeholder(owner)) { [owner release]; return nil; }
      MTL4AccelerationStructureTriangleGeometryDescriptor *empty=[MTL4AccelerationStructureTriangleGeometryDescriptor new];
      empty.vertexBuffer=m4_buffer_range(owner->placeholder,0); empty.vertexStride=12; empty.triangleCount=0;
      [owner->geometries addObject:empty]; [empty release];
    }
    ((MTL4PrimitiveAccelerationStructureDescriptor *)owner->descriptor).geometryDescriptors=owner->geometries;
  }
  return owner;
}
static void m4_as_input(D3DALLOYM4ASDescriptor *owner, id buffer) { if (buffer) [owner->inputs addObject:buffer]; }
static void m4_as_unresolved(D3DALLOYM4ASDescriptor *owner, NSUInteger geometry, BOOL unresolved) {
  if (unresolved) {
    if (!owner->unresolvedGeometries) owner->unresolvedGeometries=[NSMutableIndexSet new];
    [owner->unresolvedGeometries addIndex:geometry];
  } else [owner->unresolvedGeometries removeIndex:geometry];
}
static void m4_as_resources(D3DALLOYM4Recording *recording, D3DALLOYM4ASDescriptor *descriptor, uint64_t sidecar) {
  if (descriptor) {
    m4_retain(recording,descriptor);
    for (id<MTLAllocation> allocation in descriptor->inputs) m4_allocation(recording,allocation);
  }
  if (sidecar) {
    m4_retain(recording,m4obj(sidecar));
    NSArray *generations=m4obj(d3dalloy_as_sidecar_generations(sidecar));
    for (id<MTLAllocation> allocation in generations) m4_allocation(recording,allocation);
    m4_allocation(recording,m4obj(d3dalloy_as_sidecar_acceleration_structure(sidecar)));
    m4_allocation(recording,m4obj(d3dalloy_as_sidecar_instance_buffer(sidecar)));
    m4_allocation(recording,m4obj(d3dalloy_as_sidecar_contribution_buffer(sidecar)));
  }
}
static int32_t m4_as_call(struct WMTMetal4Call *call) {
  uint64_t *a=call->args;
  D3DALLOYM4ASDescriptor *descriptor=(D3DALLOYM4ASDescriptor *)m4obj(a[0]);
  switch (call->operation) {
  case WMTMetal4NewASDescriptor:
    call->ret[0]=(uint64_t)m4_as_new(m4obj(a[0]),a[1]!=0,(uint32_t)a[2]); return M4_SUCCESS;
  case WMTMetal4ASUsage:
    descriptor->descriptor.usage=(MTLAccelerationStructureUsage)a[1]; return M4_SUCCESS;
  case WMTMetal4ASTriangles: {
    if (!descriptor->geometries || a[1]>=descriptor->geometries.count) return M4_INVALID;
    id<MTLBuffer> vertices=m4obj(a[2]),indices=m4obj(a[5]),transform=m4obj(a[8]);
    if ((vertices && a[3]>vertices.length) || (indices && a[6]>indices.length) || (transform && a[9]>transform.length)) return M4_INVALID;
    id<MTLBuffer> vertexInput=vertices?:m4_as_placeholder(descriptor);
    if (!vertexInput) return M4_FAILED;
    MTL4AccelerationStructureTriangleGeometryDescriptor *geometry=[MTL4AccelerationStructureTriangleGeometryDescriptor new];
    geometry.vertexBuffer=m4_buffer_range(vertexInput,vertices?a[3]:0); geometry.vertexStride=a[4];
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
    id<MTLBuffer> boxInput=buffer?:m4_as_placeholder(descriptor);
    if (!boxInput) return M4_FAILED;
    MTL4AccelerationStructureBoundingBoxGeometryDescriptor *geometry=[MTL4AccelerationStructureBoundingBoxGeometryDescriptor new];
    geometry.boundingBoxBuffer=m4_buffer_range(boxInput,buffer?a[3]:0); geometry.boundingBoxStride=a[4]; geometry.boundingBoxCount=a[5];
    geometry.intersectionFunctionTableOffset=a[6]; geometry.opaque=(a[7]&1)!=0; geometry.allowDuplicateIntersectionFunctionInvocation=(a[7]&2)==0;
    descriptor->geometries[a[1]]=geometry; [geometry release]; m4_as_unresolved(descriptor,a[1],!buffer && a[5]);
    m4_as_input(descriptor,buffer); call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASInstances: {
    if (descriptor->geometries) return M4_INVALID;
    id<MTLBuffer> buffer=m4obj(a[1]); if (buffer && a[2]>buffer.length) return M4_INVALID;
    id<MTLBuffer> instanceInput=buffer?:m4_as_placeholder(descriptor);
    if (!instanceInput) return M4_FAILED;
    MTL4InstanceAccelerationStructureDescriptor *instances=(MTL4InstanceAccelerationStructureDescriptor *)descriptor->descriptor;
    instances.instanceDescriptorBuffer=m4_buffer_range(instanceInput,buffer?a[2]:0);
    instances.instanceDescriptorStride=sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
    instances.instanceDescriptorType=MTLAccelerationStructureInstanceDescriptorTypeIndirect; instances.instanceCount=a[3];
    descriptor->unresolvedInstances=!buffer && a[3];
    m4_as_input(descriptor,buffer); call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASSizes: {
    descriptor=(D3DALLOYM4ASDescriptor *)m4obj(a[1]);
    MTLAccelerationStructureSizes sizes=[(id<MTLDevice>)m4obj(a[0]) accelerationStructureSizesWithDescriptor:descriptor->descriptor];
    call->ret[0]=sizes.accelerationStructureSize; call->ret[1]=sizes.buildScratchBufferSize; call->ret[2]=sizes.refitScratchBufferSize; return M4_SUCCESS;
  }
  case WMTMetal4ASBuild:
  case WMTMetal4ASRefit: {
    D3DALLOYM4ComputeEncoder *context=(D3DALLOYM4ComputeEncoder *)m4obj(a[0]); descriptor=(D3DALLOYM4ASDescriptor *)m4obj(a[1]);
    BOOL refit=call->operation==WMTMetal4ASRefit; uint64_t destination=refit?a[3]:a[2];
    id<MTLBuffer> scratch=m4obj(a[refit?4:3]); uint64_t offset=a[refit?5:4];
    if (!descriptor || !scratch || offset>scratch.length || !destination) return M4_INVALID;
    /* Size queries intentionally have no resolved GPU buffers. Their real
     * count/format descriptor is valid for sizing, never for executing work. */
    if (descriptor->unresolvedGeometries.count || descriptor->unresolvedInstances) return M4_INVALID;
    m4_as_resources(context->recording,descriptor,destination); m4_allocation(context->recording,scratch);
    id<MTLAccelerationStructure> target=m4obj(d3dalloy_as_sidecar_acceleration_structure(destination));
    id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
    m4_stage_barrier(context,MTLStageAccelerationStructure);
    if (refit) {
      m4_as_resources(context->recording,nil,a[2]);
      [encoder refitAccelerationStructure:m4obj(d3dalloy_as_sidecar_acceleration_structure(a[2])) descriptor:descriptor->descriptor destination:target scratchBuffer:m4_buffer_range(scratch,offset)];
    } else [encoder buildAccelerationStructure:target descriptor:descriptor->descriptor scratchBuffer:m4_buffer_range(scratch,offset)];
    call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASCopy: {
    D3DALLOYM4ComputeEncoder *context=(D3DALLOYM4ComputeEncoder *)m4obj(a[0]); if (a[3]>1) return M4_INVALID;
    m4_as_resources(context->recording,nil,a[1]); m4_as_resources(context->recording,nil,a[2]);
    id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
    id<MTLAccelerationStructure> source=m4obj(d3dalloy_as_sidecar_acceleration_structure(a[1])),destination=m4obj(d3dalloy_as_sidecar_acceleration_structure(a[2]));
    m4_stage_barrier(context,MTLStageAccelerationStructure);
    if (a[3]) [encoder copyAndCompactAccelerationStructure:source toAccelerationStructure:destination];
    else [encoder copyAccelerationStructure:source toAccelerationStructure:destination];
    call->ret[0]=1; return M4_SUCCESS;
  }
  case WMTMetal4ASCompactedSize: {
    D3DALLOYM4ComputeEncoder *context=(D3DALLOYM4ComputeEncoder *)m4obj(a[0]); id<MTLBuffer> buffer=m4obj(a[2]);
    if (!buffer || a[3]>buffer.length || buffer.length-a[3]<sizeof(uint64_t)) return M4_INVALID;
    m4_as_resources(context->recording,nil,a[1]); m4_allocation(context->recording,buffer);
    m4_stage_barrier(context,MTLStageAccelerationStructure);
    [(id<MTL4ComputeCommandEncoder>)context->encoder writeCompactedAccelerationStructureSize:m4obj(d3dalloy_as_sidecar_acceleration_structure(a[1])) toBuffer:MTL4BufferRangeMake(buffer.gpuAddress+a[3],sizeof(uint64_t))];
    return M4_SUCCESS;
  }
  case WMTMetal4ASPrepass: {
    D3DALLOYM4ComputeEncoder *context=(D3DALLOYM4ComputeEncoder *)m4obj(a[0]); id<MTLComputePipelineState> state=m4obj(a[1]);
    const struct WMTASPrepassInfo *info=(const void *)(uintptr_t)a[12];
    if (!state || !info || !a[6] || !a[10]) return M4_INVALID;
    id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
    m4_allocation(context->recording,state); [encoder setComputePipelineState:state];
    if (context->recording->trace) context->tracePSO=a[1];
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
static void m4_finish_submission(D3DALLOYM4Submission *submission) {
  if (!submission->commitReturned || !submission->feedbackReceived) return;
  // Removal waits for native commit return AND GPU retirement. The device set
  // keeps an allocation only while another live recording still uses it.
  for (id lease in submission->leases) {
    if (![lease isKindOfClass:[D3DALLOYM4Recording class]]) continue;
    D3DALLOYM4Recording *recording=lease;
    m4_release_residency(recording);
    if (recording->trace) {
      M4BindingCounters c=recording->counters;
      fprintf(stderr,"OWNED_GRAPHICS_NATIVE_BINDING table_new=%llu table_set=%llu table_bind=%llu residency_new=%llu residency_add=%llu residency_remove=%llu residency_commit=%llu\n",
        (unsigned long long)c.tableNew,(unsigned long long)c.tableSet,(unsigned long long)c.tableBind,
        (unsigned long long)c.residencyNew,(unsigned long long)c.residencyAdd,
        (unsigned long long)c.residencyRemove,(unsigned long long)c.residencyCommit);
      if (c.blitHazardBarrier || c.blitCapacityBarrier)
        fprintf(stderr,"OWNED_GRAPHICS_BLIT_HAZARD hazard_barrier=%llu capacity_barrier=%llu\n",
          (unsigned long long)c.blitHazardBarrier,(unsigned long long)c.blitCapacityBarrier);
    }
  }
  [submission->leases removeAllObjects];
  for (D3DALLOYM4CommandBuffer *context in submission->buffers)
    atomic_fetch_sub_explicit(&context->inFlight,1,memory_order_release);
  submission->completed=YES; [submission->condition broadcast];
}

static int32_t m4_commit(struct WMTMetal4Call *call) {
  id<MTL4CommandQueue> queue=m4obj(call->args[0]);
  const uint64_t *handles=(const void *)(uintptr_t)call->args[1]; uint64_t count=call->args[2];
  if (!queue || !handles || !count || count>NSUIntegerMax/sizeof(id)) return M4_INVALID;
  uint64_t command_count = count;
  for (uint64_t i=0;i<count;++i) {
    D3DALLOYM4CommandBuffer *context=m4cb(handles[i]);
    if (!context || !context->recording || !context->ended || atomic_load_explicit(&context->inFlight,memory_order_acquire))
      return M4_INVALID;
    if (context->recording->segments.count > NSUIntegerMax/sizeof(id) - command_count) return M4_INVALID;
    command_count += context->recording->segments.count;
  }
  id<MTL4CommandBuffer> local[32]; id<MTL4CommandBuffer> *commands=command_count<=32?local:malloc(command_count*sizeof(*commands));
  if (!commands) return M4_FAILED;
  D3DALLOYM4Submission *submission=[D3DALLOYM4Submission new]; submission->condition=[NSCondition new];
  submission->leases=[[NSMutableArray alloc] initWithCapacity:count*3]; submission->buffers=[[NSMutableArray alloc] initWithCapacity:count];
  submission->presentTickets=[NSMutableArray new];
  NSUInteger command_index = 0;
  for (uint64_t i=0;i<count;++i) {
    D3DALLOYM4CommandBuffer *context=m4cb(handles[i]);
    for (id<MTL4CommandBuffer> segment in context->recording->segments) commands[command_index++]=segment;
    commands[command_index++]=context->command; [submission->buffers addObject:context];
    [submission->leases addObject:context->recording]; [submission->leases addObject:context->command]; [submission->leases addObject:context->allocator];
    [submission->presentTickets addObjectsFromArray:context->recording->presentTickets];
    if (context->recording->error && !submission->error) submission->error=[context->recording->error retain];
  }
  for (D3DALLOYM4CommandBuffer *context in submission->buffers) {
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
    // preCommit snapshots the shared device set; no retirement may remove an
    // allocation while it traverses that set (the per-device lock, 4.0b3 +0xc4).
    D3DALLOYM4DeviceResidency *shared=m4cb(handles[0])->residency;
    [shared->lock lock];
    [queue commit:commands count:command_count options:options]; [options release];
    [shared->lock unlock];
    // Signal each drawable after its commit and present it immediately; the
    // display waits for the signal, not the CPU (D3DMetal 4.0b3 DoPresent).
    for (D3DALLOYM4PresentTicket *ticket in submission->presentTickets) {
      id<CAMetalDrawable> primary, residual;
      winemetal_composition_drawables(ticket->drawable, &primary, &residual);
      [queue signalDrawable:primary];
      if (residual) [queue signalDrawable:residual];
      [ticket present];
    }
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
    case WMTMetal4NewTextureViewPool: {
      if (!a[1]) return M4_INVALID;
      D3DALLOYM4ViewPool *views=[D3DALLOYM4ViewPool new];
      MTLResourceViewPoolDescriptor *descriptor=[MTLResourceViewPoolDescriptor new];
      descriptor.resourceViewCount=a[1];
      views->pool=[(id<MTLDevice>)m4obj(a[0]) newTextureViewPoolWithDescriptor:descriptor error:nil];
      [descriptor release];
      if (!views->pool) {
        views->ordinaryViews=[NSMutableDictionary new];
        views->ordinaryOwners=[NSCountedSet new];
      }
      views->descriptor=[MTLTextureViewDescriptor new];
      views->textures=[[NSMutableArray alloc] initWithCapacity:a[1]];
      views->parents=[NSCountedSet new];
      for (NSUInteger i=0;i<a[1];++i) [views->textures addObject:[NSNull null]];
      const char *trace=getenv("OWNED_GRAPHICS_TRACE"); views->trace=trace && !strcmp(trace,"1");
      if (views->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_VIEW_POOL_CREATE pool_new=%u pool_fallback=%u wrapper=%llu slots=%llu\n",views->pool?1:0,views->pool?0:1,(unsigned long long)(uintptr_t)views,(unsigned long long)a[1]);
      if (a[2]) objc_setAssociatedObject(m4obj(a[2]),&m4_view_pool_key,views,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      call->ret[0]=(uint64_t)views; return M4_SUCCESS;
    }
    case WMTMetal4SetTextureView: {
      D3DALLOYM4ViewPool *views=m4obj(a[0]);
      const struct WMTTextureViewPoolInfo *info=(const struct WMTTextureViewPoolInfo *)(uintptr_t)a[2];
      if (!a[1] || !info || a[3]>=views->textures.count) return M4_INVALID;
      @synchronized(views) {
        call->ret[0]=[views setTexture:m4obj(a[1]) info:info atIndex:a[3]];
      }
      return call->ret[0] ? M4_SUCCESS : M4_FAILED;
    }
    case WMTMetal4CopyTextureViews: {
      D3DALLOYM4ViewPool *views=m4obj(a[0]), *source=m4obj(a[1]);
      const struct WMTTextureViewPoolInfo *info=(const struct WMTTextureViewPoolInfo *)(uintptr_t)a[5];
      if (!source || !a[4] || !info || a[2]>=source->textures.count || a[3]>=views->textures.count) return M4_INVALID;
      // D3D12 serializes writes/copies to the same slots; copy one slot so overlap has CPU memmove order.
      id first=(uintptr_t)views<(uintptr_t)source ? views : source;
      id second=first==views ? source : views;
      @synchronized(first) { @synchronized(second) {
        if (views->pool && source->pool) {
          call->ret[0]=[views->pool copyResourceViewsFromPool:source->pool sourceRange:NSMakeRange(a[2],1) destinationIndex:a[3]]._impl;
          [views replaceTexture:m4obj(a[4]) atIndex:a[3]];
          if (views->trace) {
            ++views->copyCount;
            fprintf(stderr,"OWNED_GRAPHICS_NATIVE_VIEW_POOL_COPY slot_copy=1 wrapper=%llu from=%llu to=%llu\n",(unsigned long long)(uintptr_t)views,(unsigned long long)a[2],(unsigned long long)a[3]);
          }
        } else if (!views->pool && !source->pool) {
          id<MTLTexture> view=source->ordinaryViews[@(a[2])];
          [views replaceOrdinaryView:view atIndex:a[3]];
          [views replaceTexture:m4obj(a[4]) atIndex:a[3]];
          call->ret[0]=view.gpuResourceID._impl;
          if (views->trace) ++views->ordinaryReuse;
        } else {
          call->ret[0]=[views setTexture:m4obj(a[4]) info:info atIndex:a[3]];
        }
      } }
      return call->ret[0] ? M4_SUCCESS : M4_FAILED;
    }
    case WMTMetal4CopyTextureViewsRange: {
      D3DALLOYM4ViewPool *views=m4obj(a[0]), *source=m4obj(a[1]);
      NSUInteger from=a[2], to=a[3], count=a[4];
      if (!views || !source || !views->pool || !source->pool || !count ||
          from > source->textures.count || count > source->textures.count-from ||
          to > views->textures.count || count > views->textures.count-to)
        return M4_INVALID;
      if (views==source && ((to < from + count && from < to + count))) return M4_INVALID;
      id first=(uintptr_t)views<(uintptr_t)source ? views : source;
      id second=first==views ? source : views;
      @synchronized(first) { @synchronized(second) {
        MTLResourceID resource=[views->pool copyResourceViewsFromPool:source->pool
            sourceRange:NSMakeRange(from,count) destinationIndex:to];
        if (!resource._impl) return M4_FAILED;
        for (NSUInteger i=0; i<count; ++i) {
          [views replaceTexture:source->textures[from+i] atIndex:to+i];
          if (views->trace) ++views->copyCount;
        }
        call->ret[0]=resource._impl;
      } }
      return M4_SUCCESS;
    }
    case WMTMetal4ClearTextureView: {
      D3DALLOYM4ViewPool *views=m4obj(a[0]); if (a[1]>=views->textures.count) return M4_INVALID;
      @synchronized(views) {
        [views replaceTexture:[NSNull null] atIndex:a[1]];
        if (views->ordinaryViews) [views replaceOrdinaryView:nil atIndex:a[1]];
        if (views->trace) ++views->clearCount;
      }
      // The GPU descriptor is zeroed by the heap. No SDK nil-texture setter is required.
      return M4_SUCCESS;
    }
    case WMTMetal4SetColorAttachmentMap: {
      const uint8_t *indices=(const void *)(uintptr_t)a[1]; if (!indices) return M4_INVALID;
      MTLLogicalToPhysicalColorAttachmentMap *mapping=[MTLLogicalToPhysicalColorAttachmentMap new];
      for (unsigned i=0;i<8;++i) {
        if (indices[i]>=8) { [mapping release]; return M4_INVALID; }
        [mapping setPhysicalIndex:indices[i] forLogicalIndex:i];
      }
      D3DALLOYM4RenderEncoder *context=m4obj(a[0]);
      [(id<MTL4RenderCommandEncoder>)context->encoder setColorAttachmentMap:mapping];
      [mapping release]; return M4_SUCCESS;
    }
    case WMTMetal4NewAllocator: call->ret[0]=(uint64_t)[(id<MTLDevice>)m4obj(a[0]) newCommandAllocator]; return M4_SUCCESS;
    case WMTMetal4ResetAllocator: [(id<MTL4CommandAllocator>)m4obj(a[0]) reset]; return M4_SUCCESS;
    case WMTMetal4AllocatorSize: call->ret[0]=[(id<MTL4CommandAllocator>)m4obj(a[0]) allocatedSize]; return M4_SUCCESS;
    case WMTMetal4NewCommandBuffer: {
      D3DALLOYM4CommandBuffer *context=[D3DALLOYM4CommandBuffer new]; context->device=[m4obj(a[0]) retain];
      context->command=[context->device newCommandBuffer]; context->uploads=[NSMutableArray new]; atomic_init(&context->inFlight,0);
      if (!context->command) { [context release]; return M4_FAILED; }
      call->ret[0]=(uint64_t)context; return M4_SUCCESS;
    }
    case WMTMetal4BeginCommandBuffer: {
      D3DALLOYM4CommandBuffer *context=m4cb(a[0]); if (!a[1] || context->recording || atomic_load_explicit(&context->inFlight,memory_order_acquire)) return M4_INVALID;
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
      context->recording=[D3DALLOYM4Recording new]; context->recording->objects=[NSMutableSet new]; context->recording->encoders=[NSMutableArray new];
      context->recording->presentTickets=[NSMutableArray new];
      context->recording->allocations=[NSMutableSet new];
      context->recording->pending=[NSMutableArray new]; context->recording->published=[NSMutableArray new];
      const char *trace=getenv("OWNED_GRAPHICS_TRACE"); context->recording->trace=trace && trace[0]=='1' && !trace[1];
      if (!context->residency) {
        context->residency=[m4_device_residency(context->device) retain];
        if (context->recording->trace) ++context->recording->counters.residencyNew;
      }
      context->recording->residency=[context->residency retain];
      if (!context->recording->residency) { [context->recording release]; context->recording=nil; return M4_FAILED; }
      context->uploadIndex=0; context->uploadOffset=0; context->ended=NO;
      [context->debugGroups removeAllObjects];
      [context->command beginCommandBufferWithAllocator:context->allocator]; return M4_SUCCESS;
    }
    case WMTMetal4EndCommandBuffer: {
      D3DALLOYM4CommandBuffer *context=m4cb(a[0]); if (!context->recording || context->ended) return M4_INVALID;
      m4_commit_residency(context->recording);
      [context->command useResidencySet:context->recording->residency->set];
      for (NSUInteger i=0;i<context->debugGroups.count;++i) [context->command popDebugGroup];
      [context->command endCommandBuffer]; context->ended=YES; return M4_SUCCESS;
    }
    case WMTMetal4RenderEncoder: {
      D3DALLOYM4CommandBuffer *owner=m4cb(a[0]); if (!owner->recording || owner->ended || !a[1]) return M4_INVALID;
      MTL4RenderPassDescriptor *descriptor=m4_render_pass(owner,(const void *)(uintptr_t)a[1]); if (!descriptor) return M4_FAILED;
      D3DALLOYM4RenderEncoder *context=[D3DALLOYM4RenderEncoder new]; context->owner=owner; context->recording=owner->recording;
      /* Registering a visibility mode makes Reset write the scratch buffer
       * before rasterization, even if the mode is disabled before drawing.
       * A Fragment-only wait therefore cannot protect a preceding GPU read. */
      if (descriptor.visibilityResultBuffer && descriptor.visibilityResultType==MTLVisibilityResultTypeReset)
        context->fenceWaitStages=MTLStageVertex|MTLStageObject|MTLStageMesh;
      context->encoder=[[owner->command renderCommandEncoderWithDescriptor:descriptor] retain];
      if (!context->encoder) { [descriptor release]; [context release]; return M4_FAILED; }
      if (owner->recording->trace) {
        for (unsigned i=0;i<10;++i) {
          MTLRenderPassAttachmentDescriptor *attachment=i<8 ? descriptor.colorAttachments[i] :
              i==8 ? descriptor.depthAttachment : descriptor.stencilAttachment;
          if (!attachment.texture) continue;
          fprintf(stderr,"OWNED_GRAPHICS_NATIVE_RENDER_ATTACHMENT encoder=%llu aspect=%u slot=%u texture=%llu load=%u store=%u\n",
              (unsigned long long)(uintptr_t)context->encoder,i<8?4u:i==8?1u:2u,i<8?i:0u,
              (unsigned long long)(uintptr_t)attachment.texture,(unsigned)attachment.loadAction,(unsigned)attachment.storeAction);
        }
      }
      [descriptor release];
      [owner->recording->encoders addObject:context]; call->ret[0]=(uint64_t)context; [context release]; return M4_SUCCESS;
    }
    case WMTMetal4ComputeEncoder: {
      D3DALLOYM4CommandBuffer *owner=m4cb(a[0]); if (!owner->recording || owner->ended) return M4_INVALID;
      D3DALLOYM4ComputeEncoder *context=[D3DALLOYM4ComputeEncoder new]; context->owner=owner; context->recording=owner->recording;
      context->compute=YES;
      context->concurrent=a[1]!=0;
      context->encoder=[[owner->command computeCommandEncoder] retain];
      if (!context->encoder) { [context release]; return M4_FAILED; }
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_COMPUTE_BEGIN context=%llu encoder=%llu concurrent=%u\n",
          (unsigned long long)(uintptr_t)context,(unsigned long long)(uintptr_t)context->encoder,context->concurrent?1:0);
      [owner->recording->encoders addObject:context]; call->ret[0]=(uint64_t)context; [context release]; return M4_SUCCESS;
    }
    case WMTMetal4EndEncoding: [((D3DALLOYM4Encoder *)m4obj(a[0]))->encoder endEncoding]; return M4_SUCCESS;
    case WMTMetal4SetEncoderLabel: ((D3DALLOYM4Encoder *)m4obj(a[0]))->encoder.label=m4obj(a[1]); return M4_SUCCESS;
    case WMTMetal4Diagnostic: {
      D3DALLOYM4CommandBuffer *context=m4cb(a[0]);
      if (!context->recording || context->ended || a[1]>3 || (a[3] && !a[2])) return M4_INVALID;
      NSString *label=[[[NSString alloc] initWithBytes:(const void *)(uintptr_t)a[2]
        length:a[3] encoding:NSUTF8StringEncoding] autorelease];
      if (a[1]!=2 && !label) return M4_INVALID;
      switch (a[1]) {
      case 0: {
        id<MTL4ComputeCommandEncoder> encoder=[context->command computeCommandEncoder];
        if (!encoder) return M4_FAILED;
        encoder.label=label; [encoder insertDebugSignpost:label]; [encoder endEncoding];
        [context->recording->encoders addObject:encoder]; break;
      }
      case 1:
        if (!context->debugGroups) context->debugGroups=[NSMutableArray new];
        [context->command pushDebugGroup:label]; [context->debugGroups addObject:label]; break;
      case 2:
        if (!context->debugGroups.count) return M4_INVALID;
        [context->command popDebugGroup]; [context->debugGroups removeLastObject]; break;
      case 3: context->command.label=label; break;
      }
      return M4_SUCCESS;
    }
    case WMTMetal4RenderCommands: return m4_render_commands((D3DALLOYM4RenderEncoder *)m4obj(a[0]),(const void *)(uintptr_t)a[1]);
    case WMTMetal4ComputeCommands: return m4_compute_commands((D3DALLOYM4ComputeEncoder *)m4obj(a[0]),(const void *)(uintptr_t)a[1]);
    case WMTMetal4BlitCommands: return m4_blit_commands((D3DALLOYM4ComputeEncoder *)m4obj(a[0]),(const void *)(uintptr_t)a[1]);
    case WMTMetal4Commit: return m4_commit(call);
    case WMTMetal4WaitSubmission: {
      D3DALLOYM4Submission *submission=(D3DALLOYM4Submission *)m4obj(a[0]); [submission->condition lock];
      while (!submission->completed) [submission->condition wait]; [submission->condition unlock]; return M4_SUCCESS;
    }
    case WMTMetal4GetSubmissionInfo: {
      if (!a[1]) return M4_INVALID; D3DALLOYM4Submission *submission=(D3DALLOYM4Submission *)m4obj(a[0]);
      struct WMTMetal4SubmissionInfo *info=(void *)(uintptr_t)a[1]; [submission->condition lock];
      *info=(struct WMTMetal4SubmissionInfo){submission->completed,(uint64_t)submission->error,submission->gpuStart,submission->gpuEnd};
      [submission->condition unlock]; return M4_SUCCESS;
    }
    case WMTMetal4WaitEvent: [(id<MTL4CommandQueue>)m4obj(a[0]) waitForEvent:m4obj(a[1]) value:a[2]]; return M4_SUCCESS;
    case WMTMetal4WaitDrawable: {
      id<MTL4CommandQueue> queue = m4obj(a[0]);
      id drawable = m4obj(a[1]);
      id<CAMetalDrawable> primary, residual;
      winemetal_composition_drawables(drawable, &primary, &residual);
      [queue waitForDrawable:primary];
      if (residual) [queue waitForDrawable:residual];
      return M4_SUCCESS;
    }
    case WMTMetal4SignalDrawable: {
      id<MTL4CommandQueue> queue = m4obj(a[0]);
      id drawable = m4obj(a[1]);
      id<CAMetalDrawable> primary, residual;
      winemetal_composition_drawables(drawable, &primary, &residual);
      [queue signalDrawable:primary];
      if (residual) [queue signalDrawable:residual];
      return M4_SUCCESS;
    }
    case WMTMetal4UseResidencySet: {
      D3DALLOYM4CommandBuffer *context=m4cb(a[0]); if (!context->recording) return M4_INVALID;
      m4_retain(context->recording,m4obj(a[1])); [context->command useResidencySet:m4obj(a[1])]; return M4_SUCCESS;
    }
    case WMTMetal4UseAllocation: {
      D3DALLOYM4CommandBuffer *context=m4cb(a[0]); if (!context->recording || context->ended) return M4_INVALID;
      m4_allocation(context->recording,m4obj(a[1])); return M4_SUCCESS;
    }
    case WMTMetal4UseDrawable: {
      D3DALLOYM4CommandBuffer *context=m4cb(a[0]);
      if (!context->recording || context->ended || !a[1]) return M4_INVALID;
      id<CAMetalDrawable> primary, residual;
      winemetal_composition_drawables(m4obj(a[1]), &primary, &residual);
      id<MTLResidencySet> primary_set = primary.layer.residencySet;
      id<MTLResidencySet> residual_set = residual.layer.residencySet;
      if (!primary_set || (residual && !residual_set)) return M4_FAILED;
      // CAMetalLayer owns and updates these sets. Keep them through recording
      // retirement, but never modify them or accumulate them on the queue.
      m4_retain(context->recording, primary_set);
      [context->command useResidencySet:primary_set];
      if (residual_set && residual_set != primary_set) {
        m4_retain(context->recording, residual_set);
        [context->command useResidencySet:residual_set];
      }
      return M4_SUCCESS;
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
      D3DALLOYM4ComputeEncoder *context=(D3DALLOYM4ComputeEncoder *)m4obj(a[0]); m4_retain(context->recording,m4obj(a[1]));
      context->tableBound[0]=NO;
      [(id<MTL4ComputeCommandEncoder>)context->encoder setArgumentTable:m4obj(a[1])]; return M4_SUCCESS;
    }
    case WMTMetal4RenderArgumentTable: {
      D3DALLOYM4RenderEncoder *context=(D3DALLOYM4RenderEncoder *)m4obj(a[0]); m4_retain(context->recording,m4obj(a[1]));
      for (unsigned stage=0;stage<4;++stage) if (a[2]&m4_render_stages[stage]) context->tableBound[stage]=NO;
      [(id<MTL4RenderCommandEncoder>)context->encoder setArgumentTable:m4obj(a[1]) atStages:(MTLRenderStages)a[2]]; return M4_SUCCESS;
    }
    case WMTMetal4Barrier: {
      D3DALLOYM4Encoder *context=(D3DALLOYM4Encoder *)m4obj(a[0]); id<MTL4CommandEncoder> encoder=context->encoder;
      if (a[1]==0) [encoder barrierAfterQueueStages:(MTLStages)a[2] beforeStages:(MTLStages)a[3] visibilityOptions:(MTL4VisibilityOptions)a[4]];
      else if (a[1]==1) [encoder barrierAfterStages:(MTLStages)a[2] beforeQueueStages:(MTLStages)a[3] visibilityOptions:(MTL4VisibilityOptions)a[4]];
      else if (a[1]==2) [encoder barrierAfterEncoderStages:(MTLStages)a[2] beforeEncoderStages:(MTLStages)a[3] visibilityOptions:(MTL4VisibilityOptions)a[4]];
      else return M4_INVALID;
      if (a[1]==2 && (a[4]&MTL4VisibilityOptionDevice)) m4_record_barrier(context,a[2],a[3]);
      return M4_SUCCESS;
    }
    case WMTMetal4Fence: {
      D3DALLOYM4Encoder *context=(D3DALLOYM4Encoder *)m4obj(a[0]); m4_retain(context->recording,m4obj(a[1]));
      if (a[2]) [context->encoder updateFence:m4obj(a[1]) afterEncoderStages:(MTLStages)a[3]];
      else [context->encoder waitForFence:m4obj(a[1]) beforeEncoderStages:(MTLStages)a[3]|context->fenceWaitStages]; return M4_SUCCESS;
    }
    default: return M4_INVALID;
    }
  }
  return M4_UNSUPPORTED;
}


static int32_t m4_compute_commands(D3DALLOYM4ComputeEncoder *context, const struct wmtcmd_base *next) {
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
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_DISPATCH encoder=%llu pso=%llu mode=%s grid_x=%lu grid_y=%lu grid_z=%lu threads_x=%lu threads_y=%lu threads_z=%lu\n",
          (unsigned long long)(uintptr_t)encoder,(unsigned long long)context->tracePSO,
          next->type==WMTComputeCommandDispatch?"threadgroups":"threads",
          (unsigned long)size.width,(unsigned long)size.height,(unsigned long)size.depth,
          (unsigned long)context->threadgroupSize.width,(unsigned long)context->threadgroupSize.height,(unsigned long)context->threadgroupSize.depth);
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      const struct wmtcmd_compute_dispatch_indirect *body=(const void *)next;
      m4_stage_barrier(context,MTLStageDispatch);
      [encoder dispatchThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->indirect_args_buffer,body->indirect_args_offset)
                              threadsPerThreadgroup:context->threadgroupSize];
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_DISPATCH_INDIRECT encoder=%llu pso=%llu argument_buffer=%llu argument_offset=%llu threads_x=%lu threads_y=%lu threads_z=%lu\n",
          (unsigned long long)(uintptr_t)encoder,(unsigned long long)context->tracePSO,
          (unsigned long long)body->indirect_args_buffer,(unsigned long long)body->indirect_args_offset,
          (unsigned long)context->threadgroupSize.width,(unsigned long)context->threadgroupSize.height,(unsigned long)context->threadgroupSize.depth);
      break;
    }
    case WMTComputeCommandSetPSO: {
      const struct wmtcmd_compute_setpso *body=(const void *)next;
      id<MTLComputePipelineState> state=m4obj(body->pso); m4_allocation(context->recording,state);
      [encoder setComputePipelineState:state];
      if (context->recording->trace) context->tracePSO=body->pso;
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
      m4_bind_resource(context,0,texture.gpuResourceID,body->index,YES); m4_compute_table(context); break;
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
      [encoder executeCommandsInBuffer:buffer withRange:NSMakeRange(body->location,body->length)];
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_EXECUTE_ICB stage=compute encoder=%llu buffer=%llu location=%u length=%u\n",
          (unsigned long long)(uintptr_t)encoder,(unsigned long long)body->indirect_command_buffer,body->location,body->length);
      break;
    }
    case WMTComputeCommandSetVisibleFunctionTable: {
      const struct wmtcmd_compute_setfunctiontable *body=(const void *)next;
      id<MTLVisibleFunctionTable> table=m4obj(body->table); m4_allocation(context->recording,table);
      if (body->index>=31) return M4_INVALID;
      m4_bind_resource(context,0,table.gpuResourceID,body->index,NO); m4_compute_table(context); break;
    }
    case WMTComputeCommandSetIntersectionFunctionTable: {
      const struct wmtcmd_compute_setfunctiontable *body=(const void *)next;
      id<MTLIntersectionFunctionTable> table=m4obj(body->table); m4_allocation(context->recording,table);
      if (body->index>=31) return M4_INVALID;
      m4_bind_resource(context,0,table.gpuResourceID,body->index,NO); m4_compute_table(context); break;
    }
    case WMTComputeCommandSetAccelerationStructure: {
      const struct wmtcmd_compute_setaccelerationstructure *body=(const void *)next;
      id<MTLAccelerationStructure> structure=m4obj(body->acceleration_structure); m4_allocation(context->recording,structure);
      if (body->index>=31) return M4_INVALID;
      m4_bind_resource(context,0,structure.gpuResourceID,body->index,NO); m4_compute_table(context); break;
    }
    default: m4_error(context->recording,@"Unknown Metal4 compute command"); return M4_INVALID;
    }
    next=next->next.ptr;
  }
  return context->recording->error ? M4_FAILED : M4_SUCCESS;
}
static int32_t m4_render_commands(D3DALLOYM4RenderEncoder *context, const struct wmtcmd_base *next) {
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
      m4_bind_resource(context,1,texture.gpuResourceID,body->index,YES); m4_render_table(context,1);
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
    case WMTRenderCommandSetDepthBounds: {
      const struct wmtcmd_render_setdepthbounds *body = (const struct wmtcmd_render_setdepthbounds *)next;
      [encoder setDepthTestMinBound:body->min_depth maxBound:body->max_depth];
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
      MTLGPUAddress index_address; NSUInteger index_length;
      m4_index_range(context,body->index_buffer,body->index_buffer_offset,&index_address,&index_length);
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                          indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:index_address
                   indexBufferLength:index_length
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                indirectBuffer:m4_address(context->recording,body->indirect_args_buffer,body->indirect_args_offset)];
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_DRAW_INDIRECT encoder=%llu argument_buffer=%llu argument_offset=%llu primitive_type=%u\n",
          (unsigned long long)(uintptr_t)encoder,(unsigned long long)body->indirect_args_buffer,
          (unsigned long long)body->indirect_args_offset,body->primitive_type);
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      MTLGPUAddress index_address; NSUInteger index_length;
      m4_index_range(context,body->index_buffer,body->index_buffer_offset,&index_address,&index_length);
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                           indexType:(MTLIndexType)body->index_type
                         indexBuffer:index_address
                   indexBufferLength:index_length
                      indirectBuffer:m4_address(context->recording,body->indirect_args_buffer,body->indirect_args_offset)];
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_DRAW_INDEXED_INDIRECT encoder=%llu argument_buffer=%llu argument_offset=%llu primitive_type=%u index_type=%u index_buffer=%llu index_offset=%llu\n",
          (unsigned long long)(uintptr_t)encoder,(unsigned long long)body->indirect_args_buffer,
          (unsigned long long)body->indirect_args_offset,body->primitive_type,body->index_type,
          (unsigned long long)body->index_buffer,(unsigned long long)body->index_buffer_offset);
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
    case WMTRenderCommandD3DalloyGeometryDraw: {
      struct wmtcmd_render_d3dalloy_geometry_draw *body = (struct wmtcmd_render_d3dalloy_geometry_draw *)next;
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandD3DalloyGeometryDrawIndexed: {
      struct wmtcmd_render_d3dalloy_geometry_draw_indexed *body = (struct wmtcmd_render_d3dalloy_geometry_draw_indexed *)next;
      m4_render_buffer(context,3,m4obj(body->index_buffer),body->index_buffer_offset,SM50_BINDING_INDEX_INDEX_BUFFER);
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandD3DalloyGeometryDrawIndirect: {
      struct wmtcmd_render_d3dalloy_geometry_draw_indirect *body = (struct wmtcmd_render_d3dalloy_geometry_draw_indirect *)next;
      m4_render_buffer(context,3,m4obj(body->indirect_args_buffer),body->indirect_args_offset,SM50_BINDING_INDEX_INDIRECT_ARGUMENTS);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->dispatch_args_buffer,body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      m4_render_buffer(context,3,m4obj(body->imm_draw_arguments),0,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandD3DalloyGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_d3dalloy_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_d3dalloy_geometry_draw_indexed_indirect *)next;
      m4_render_buffer(context,3,m4obj(body->index_buffer),body->index_buffer_offset,SM50_BINDING_INDEX_INDEX_BUFFER);
      m4_render_buffer(context,3,m4obj(body->indirect_args_buffer),body->indirect_args_offset,SM50_BINDING_INDEX_INDIRECT_ARGUMENTS);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->dispatch_args_buffer,body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      m4_render_buffer(context,3,m4obj(body->imm_draw_arguments),0,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandD3DalloyTessellationMeshDraw: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw *body = (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw *)next;
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandD3DalloyTessellationMeshDrawIndexed: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed *body =
          (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed *)next;
      m4_render_buffer(context,3,m4obj(body->index_buffer),body->index_buffer_offset,SM50_BINDING_INDEX_INDEX_BUFFER);
      m4_render_offset(context,3,body->draw_arguments_offset,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandD3DalloyTessellationMeshDrawIndirect: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indirect *)next;
      m4_render_buffer(context,3,m4obj(body->indirect_args_buffer),body->indirect_args_offset,SM50_BINDING_INDEX_INDIRECT_ARGUMENTS);
      [encoder drawMeshThreadgroupsWithIndirectBuffer:m4_address(context->recording,body->dispatch_args_buffer,body->dispatch_args_offset)
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      m4_render_buffer(context,3,m4obj(body->imm_draw_arguments),0,SM50_BINDING_INDEX_DRAW_ARGUMENTS);
      break;
    }
    case WMTRenderCommandD3DalloyTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed_indirect *)next;
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
      if (context->recording->trace) fprintf(stderr,"OWNED_GRAPHICS_NATIVE_EXECUTE_ICB stage=render encoder=%llu buffer=%llu location=%u length=%u\n",
          (unsigned long long)(uintptr_t)encoder,(unsigned long long)body->indirect_command_buffer,body->location,body->length);
      break;
    }
    }
    next = next->next.ptr;
  }
  return context->recording->error ? M4_FAILED : M4_SUCCESS;
}



static int32_t m4_blit_commands(D3DALLOYM4ComputeEncoder *context, const struct wmtcmd_base *next) {
  id<MTL4ComputeCommandEncoder> encoder=(id<MTL4ComputeCommandEncoder>)context->encoder;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      if (next->type) { m4_error(context->recording,@"Unknown Metal4 copy command"); return M4_INVALID; }
      break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,m4_buffer_footprint(body->src,body->src_offset,body->copy_length,NO),
        m4_buffer_footprint(body->dst,body->dst_offset,body->copy_length,YES));
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
      m4_blit_access(context,m4_linear_footprint(body->src,body->src_offset,body->bytes_per_row,body->bytes_per_image,body->size,NO),
        m4_texture_footprint((id<MTLTexture>)body->dst,body->level,1,body->slice,1,body->origin,body->size,YES));
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
      m4_blit_access(context,m4_linear_footprint(body->src,body->src_offset,body->bytes_per_row,body->bytes_per_image,body->size,NO),
        m4_texture_footprint((id<MTLTexture>)body->dst,body->level,1,body->slice,1,body->origin,body->size,YES));
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
      m4_blit_access(context,m4_texture_footprint((id<MTLTexture>)body->src,body->level,1,body->slice,1,body->origin,body->size,NO),
        m4_linear_footprint(body->dst,body->offset,body->bytes_per_row,body->bytes_per_image,body->size,YES));
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
      m4_blit_access(context,m4_texture_footprint((id<MTLTexture>)body->src,body->level,1,body->slice,1,body->origin,body->size,NO),
        m4_linear_footprint(body->dst,body->offset,body->bytes_per_row,body->bytes_per_image,body->size,YES));
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
      m4_blit_access(context,m4_texture_footprint((id<MTLTexture>)body->src,body->src_level,1,body->src_slice,1,body->src_origin,body->src_size,NO),
        /* Source texels of a format-reinterpreting copy are not destination texels. */
        m4_texture_footprint((id<MTLTexture>)body->dst,body->dst_level,1,body->dst_slice,1,body->dst_origin,
                             ((id<MTLTexture>)body->src).pixelFormat==((id<MTLTexture>)body->dst).pixelFormat ? body->src_size :
                             (struct WMTSize){UINT64_MAX,UINT64_MAX,UINT64_MAX},YES));
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
      m4_blit_access(context,m4_whole_texture((id<MTLTexture>)body->texture,NO),m4_whole_texture((id<MTLTexture>)body->texture,YES));
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
      m4_blit_access(context,(M4BlitAccess){0},m4_buffer_footprint(body->buffer,body->offset,body->length,YES));
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResolveCounters:
      m4_error(context->recording,@"Classic counter sample buffers cannot be encoded in Metal4; use the counter-heap query API");
      return M4_INVALID;
    case WMTBlitCommandResetCommandsInBuffer: {
      struct wmtcmd_blit_resetcommands *body = (struct wmtcmd_blit_resetcommands *)next;
      m4_allocation(context->recording,m4obj(body->indirect_command_buffer));
      m4_blit_access(context,(M4BlitAccess){0},m4_buffer_footprint(body->indirect_command_buffer,body->location,body->length,YES));
      [encoder resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                           withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTBlitCommandCopyTexture: {
      struct wmtcmd_blit_copy_texture *body = (struct wmtcmd_blit_copy_texture *)next;
      m4_allocation(context->recording,m4obj(body->src)); m4_allocation(context->recording,m4obj(body->dst));
      m4_blit_access(context,m4_whole_texture((id<MTLTexture>)body->src,NO),m4_whole_texture((id<MTLTexture>)body->dst,YES));
      [encoder copyFromTexture:(id<MTLTexture>)body->src toTexture:(id<MTLTexture>)body->dst];
      break;
    }
    }

    next = next->next.ptr;
  }
  return context->recording->error ? M4_FAILED : M4_SUCCESS;
}
