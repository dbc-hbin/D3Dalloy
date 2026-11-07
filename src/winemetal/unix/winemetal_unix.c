#include <stdatomic.h>
#include <stdbool.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <sys/sysctl.h>
#import <Cocoa/Cocoa.h>
#import <ColorSync/ColorSync.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreFoundation/CFRunLoop.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#include "objc/objc-runtime.h"
#include <bootstrap.h>
#include <mach/mach_port.h>
#include <CommonCrypto/CommonDigest.h>
#import <IOSurface/IOSurface.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define WINEMETAL_API
#include "../winemetal_thunks.h"
#include "../winemetal_metal4.h"
#include "../airconv_thunks.h"
#include "winemetal_rt.h"
#include "rt_acceleration_structure.h"

typedef int NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL 0xC0000001
#define STATUS_NOT_IMPLEMENTED 0xC0000002

static BOOL composition_release(NSObject *object);

static NTSTATUS _WMTGetGuestWindowBase(void *result) {
  *(uint64_t *)result = 0;
#if defined(__aarch64__) && !defined(D3DALLOY_NATIVE)
  extern NTSTATUS NtQueryInformationProcess(void *, uint32_t, void *, uint32_t, uint32_t *);
  /* Owned Wine ProcessWineWow64GuestBase returns a native ULONG_PTR. */
  return NtQueryInformationProcess((void *)~(uintptr_t)0, 1010, result, sizeof(uint64_t), NULL);
#else
  return STATUS_SUCCESS;
#endif
}
static void composition_layer_resize(CAMetalLayer *layer);
static NSView *composition_view_for_layer(CAMetalLayer *layer);
static id composition_next_drawable(CAMetalLayer *layer);

void
execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

static NTSTATUS
_NSObject_retain(NSObject **obj) {
  [*obj retain];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_release(NSObject **obj) {
  if (!composition_release(*obj))
    [*obj release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_object(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSArray *)params->handle objectAtIndex:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSArray_count(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(NSArray *)params->handle count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCopyAllDevices(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)MTLCopyAllDevices();
  return STATUS_SUCCESS;
}

@interface WMTDeviceCatalogState : NSObject {
@public
  atomic_uint_fast64_t generation;
}
@end
@implementation WMTDeviceCatalogState
- (instancetype)init {
  if ((self = [super init]))
    atomic_init(&generation, 1);
  return self;
}
@end

@interface WMTDeviceCatalogObserver : NSObject {
@public
  WMTDeviceCatalogState *state;
  id token;
}
@end
@implementation WMTDeviceCatalogObserver
- (instancetype)init {
  if ((self = [super init])) {
    state = [[WMTDeviceCatalogState alloc] init];
    if (!state) {
      [self release];
      return nil;
    }
    WMTDeviceCatalogState *callback_state = state;
    NSArray *devices = MTLCopyAllDevicesWithObserver(&token,
        ^(id<MTLDevice> device, MTLDeviceNotificationName name) {
          /* A removal request is not an enumeration change yet. */
          if ([name isEqualToString:MTLDeviceWasAddedNotification] ||
              [name isEqualToString:MTLDeviceWasRemovedNotification])
            atomic_fetch_add_explicit(&callback_state->generation, 1, memory_order_release);
        });
    [devices release];
    /* SDK: observer out parameter already carries +1 in addition to
     * Metal's retain. Release exactly that +1 after removing it. */
    if (!token) {
      [self release];
      return nil;
    }
  }
  return self;
}
- (void)dealloc {
  if (token)
    MTLRemoveDeviceObserver(token);
  [token release];
  /* The copied callback retains state independently, including in-flight
   * callbacks after removal. It does not capture this observer. */
  [state release];
  [super dealloc];
}
@end

static NTSTATUS
_WMTDeviceCatalog_call(void *obj) {
  struct WMTDeviceCatalogCall *params = obj;
  params->result = 0;
  switch (params->operation) {
  case WMTDeviceCatalogCreate:
    params->result = (obj_handle_t)[[WMTDeviceCatalogObserver alloc] init];
    break;
  case WMTDeviceCatalogGeneration: {
    WMTDeviceCatalogObserver *observer = (id)params->object;
    params->result = atomic_load_explicit(&observer->state->generation, memory_order_acquire);
    break;
  }
  case WMTDeviceCatalogHints: {
    id<MTLDevice> device = (id)params->object;
    params->result = ([device isLowPower] ? WMTDeviceLowPower : 0) |
                     ([device isRemovable] ? WMTDeviceRemovable : 0) |
                     ([device hasUnifiedMemory] ? WMTDeviceUnifiedMemory : 0);
    break;
  }
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_recommendedMaxWorkingSetSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle recommendedMaxWorkingSetSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_currentAllocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle currentAllocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_name(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle name];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_getCString(void *obj) {
  struct unixcall_nsstring_getcstring *params = obj;
  params->ret = (uint32_t)[(NSString *)params->str getCString:(char *)params->buffer_ptr
                                                    maxLength:params->max_length
                                                     encoding:params->encoding];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newCommandQueue(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newCommandQueueWithMaxCommandBufferCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSAutoreleasePool_alloc_init(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[NSAutoreleasePool alloc] init];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_commandBuffer(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandQueue>)params->handle commandBuffer];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle commit];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_waitUntilCompleted(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle waitUntilCompleted];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_status(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLCommandBuffer>)params->handle status];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newSharedEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signaledValue(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLSharedEvent>)params->handle signaledValue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSignalEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeSignalEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBuffer(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  if (info->memory.ptr) {
    buffer = [device newBufferWithBytesNoCopy:info->memory.ptr
                                       length:info->length
                                      options:(enum MTLResourceOptions)info->options
                                  deallocator:NULL];
  } else {
    buffer = [device newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
    info->memory.ptr = [buffer storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];
  }
  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
  return STATUS_SUCCESS;
}

#ifndef D3DALLOY_NATIVE
extern NTSTATUS NtAllocateVirtualMemory(void *process, void **address, uintptr_t zero_bits, size_t *size,
                                        uint32_t type, uint32_t protect);
extern NTSTATUS NtFreeVirtualMemory(void *process, void **address, size_t *size, uint32_t type);

/*
 * Storage released by Metal deallocators, which can run on threads Wine does not own. Blocks are
 * pushed here (the first word links them) and returned to Wine by the next WoW64 allocation, which
 * always runs on a Wine thread inside a unix call.
 */
static _Atomic(void *) wow64_buffer_free_list;

static void
wow64_buffer_drain_frees(void) {
  void *block = atomic_exchange(&wow64_buffer_free_list, NULL);
  while (block) {
    void *next = *(void **)block;
    size_t size = 0;
    NtFreeVirtualMemory((void *)~(uintptr_t)0, &block, &size, 0x8000 /* MEM_RELEASE */);
    block = next;
  }
}

/*
 * WoW64 twin of _MTLDevice_newBuffer. A CPU-visible Metal allocation lives above 4 GiB, where a
 * 32-bit PE cannot address it. When the caller asks for an addressable mapping
 * (WMT_RESOURCE_CPU_ADDRESSABLE) and supplies no storage, the buffer is created over committed
 * Wine memory below 2 GiB; Metal's deallocator returns it once the buffer is gone. The caller then
 * reads a mapping it can dereference, exactly as a 64-bit caller reads [buffer contents]. Every
 * other request is the 64-bit behavior, so GPU-only buffers do not consume the 32-bit address space.
 */
static NTSTATUS
_MTLDevice_newBuffer32(void *obj) {
  struct unixcall_mtldevice_newbuffer *params = obj;
  struct WMTBufferInfo *info = params->info.ptr;
  const bool addressable = info->options & WMT_RESOURCE_CPU_ADDRESSABLE_BIT;
  info->options &= ~WMT_RESOURCE_CPU_ADDRESSABLE_BIT;
  const enum MTLResourceOptions options = (enum MTLResourceOptions)info->options;
  if (!addressable || info->memory.ptr ||
      (options & MTLResourceStorageModeMask) == MTLResourceStorageModePrivate)
    return _MTLDevice_newBuffer(obj);
  wow64_buffer_drain_frees();
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  params->ret = 0;
  info->gpu_address = 0;
  /* Metal requires page-aligned no-copy storage; reserve whole 16 KiB host pages. */
  const uint64_t length = info->length ? info->length : 1;
  if (length > UINT32_MAX)
    return STATUS_SUCCESS;
  void *block = NULL;
  size_t size = (size_t)((length + 0x3fff) & ~(uint64_t)0x3fff);
  if (NtAllocateVirtualMemory((void *)~(uintptr_t)0, &block, 0x7fffffff, &size,
                              0x3000 /* MEM_COMMIT | MEM_RESERVE */, 0x04 /* PAGE_READWRITE */) || !block)
    return STATUS_SUCCESS;
  id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:block
                                                   length:size
                                                  options:options
                                              deallocator:^(void *pointer, NSUInteger unused_length) {
                                                void *head = atomic_load(&wow64_buffer_free_list);
                                                do {
                                                  *(void **)pointer = head;
                                                } while (!atomic_compare_exchange_weak(&wow64_buffer_free_list,
                                                                                       &head, pointer));
                                              }];
  if (!buffer) {
    size = 0;
    NtFreeVirtualMemory((void *)~(uintptr_t)0, &block, &size, 0x8000 /* MEM_RELEASE */);
    return STATUS_SUCCESS;
  }
  info->memory.ptr = block;
  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
  return STATUS_SUCCESS;
}
#endif

static MTLSamplerDescriptor *
new_sampler_descriptor(const struct WMTSamplerInfo *info) {
  MTLSamplerDescriptor *sampler_desc = [[MTLSamplerDescriptor alloc] init];
  sampler_desc.borderColor = (MTLSamplerBorderColor)info->border_color;
  sampler_desc.rAddressMode = (MTLSamplerAddressMode)info->r_address_mode;
  sampler_desc.sAddressMode = (MTLSamplerAddressMode)info->s_address_mode;
  sampler_desc.tAddressMode = (MTLSamplerAddressMode)info->t_address_mode;
  sampler_desc.magFilter = (MTLSamplerMinMagFilter)info->mag_filter;
  sampler_desc.minFilter = (MTLSamplerMinMagFilter)info->min_filter;
  sampler_desc.mipFilter = (MTLSamplerMipFilter)info->mip_filter;
  sampler_desc.compareFunction = (MTLCompareFunction)info->compare_function;
  sampler_desc.lodMaxClamp = info->lod_max_clamp;
  sampler_desc.lodMinClamp = info->lod_min_clamp;
  sampler_desc.maxAnisotropy = info->max_anisotroy;
  sampler_desc.lodAverage = info->lod_average;
  sampler_desc.normalizedCoordinates = info->normalized_coords;
  sampler_desc.supportArgumentBuffers = info->support_argument_buffers;
  return sampler_desc;
}

static NTSTATUS
_MTLDevice_newSamplerState(void *obj) {
  struct unixcall_mtldevice_newsamplerstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTSamplerInfo *info = params->info.ptr;
  MTLSamplerDescriptor *sampler_desc = new_sampler_descriptor(info);

  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];
  info->gpu_resource_id = info->support_argument_buffers ? [sampler gpuResourceID]._impl : 0;
  params->ret = (obj_handle_t)sampler;
  [sampler_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newDepthStencilState(void *obj) {
  struct unixcall_mtldevice_newdepthstencilstate *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTDepthStencilInfo *info = params->info.ptr;

  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)info->depth_compare_function;
  desc.depthWriteEnabled = info->depth_write_enabled;

  if (info->front_stencil.enabled) {
    desc.frontFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->front_stencil.depth_stencil_pass_op;
    desc.frontFaceStencil.depthFailureOperation = (MTLStencilOperation)info->front_stencil.depth_fail_op;
    desc.frontFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->front_stencil.stencil_fail_op;
    desc.frontFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->front_stencil.stencil_compare_function;
    desc.frontFaceStencil.writeMask = info->front_stencil.write_mask;
    desc.frontFaceStencil.readMask = info->front_stencil.read_mask;
  }

  if (info->back_stencil.enabled) {
    desc.backFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->back_stencil.depth_stencil_pass_op;
    desc.backFaceStencil.depthFailureOperation = (MTLStencilOperation)info->back_stencil.depth_fail_op;
    desc.backFaceStencil.stencilFailureOperation = (MTLStencilOperation)info->back_stencil.stencil_fail_op;
    desc.backFaceStencil.stencilCompareFunction = (MTLCompareFunction)info->back_stencil.stencil_compare_function;
    desc.backFaceStencil.writeMask = info->back_stencil.write_mask;
    desc.backFaceStencil.readMask = info->back_stencil.read_mask;
  }

  params->ret = (obj_handle_t)[device newDepthStencilStateWithDescriptor:desc];
  [desc release];
  return STATUS_SUCCESS;
}

void
fill_texture_descriptor(MTLTextureDescriptor *desc, struct WMTTextureInfo *info) {
  desc.textureType = (MTLTextureType)info->type;
  desc.pixelFormat = to_metal_pixel_format(info->pixel_format);
  desc.width = info->width;
  desc.height = info->height;
  desc.depth = info->depth;
  desc.arrayLength = info->array_length;
  desc.mipmapLevelCount = info->mipmap_level_count;
  desc.sampleCount = info->sample_count;
  desc.usage = (MTLTextureUsage)info->usage;
  desc.resourceOptions = (MTLResourceOptions)info->options;
};

void
extract_texture_descriptor(id<MTLTexture> desc, struct WMTTextureInfo *info) {
  info->type = desc.textureType;
  info->pixel_format = desc.pixelFormat;
  info->width = desc.width;
  info->height = desc.height;
  info->depth = desc.depth;
  info->array_length = desc.arrayLength;
  info->mipmap_level_count = desc.mipmapLevelCount;
  info->sample_count = desc.sampleCount;
  info->usage = desc.usage;
  info->options = (enum WMTResourceOptions)desc.resourceOptions;
  info->reserved = 0;
};

static NTSTATUS
_MTLDevice_newTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [device newTextureWithDescriptor:desc];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_newTexture(void *obj) {
  struct unixcall_mtlbuffer_newtexture *params = obj;
  id<MTLBuffer> buffer = (id<MTLBuffer>)params->buffer;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret = [buffer newTextureWithDescriptor:desc offset:params->offset bytesPerRow:params->bytes_per_row];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static inline MTLTextureSwizzleChannels
to_metal_swizzle(struct WMTTextureSwizzleChannels swizzle, enum WMTPixelFormat format) {
  return MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 0, swizzle.r),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 1, swizzle.g),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 2, swizzle.b),
      (MTLTextureSwizzle)DECODE_FORMAT_COMPONENT_SWIZZLE(format, 3, swizzle.a)
  );
}

static NTSTATUS
_MTLTexture_newTextureView(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;

  id<MTLTexture> ret = [texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(params->format)
                        textureType:(MTLTextureType)params->texture_type
                             levels:NSMakeRange(params->level_start, params->level_count)
                             slices:NSMakeRange(params->slice_start, params->slice_count)
                            swizzle:to_metal_swizzle(params->swizzle, params->format)];
  params->ret = (obj_handle_t)ret;
  params->gpu_resource_id = [ret gpuResourceID]._impl;
  const char *trace=getenv("OWNED_GRAPHICS_TRACE");
  if (trace && !strcmp(trace,"1"))
    fprintf(stderr,"OWNED_GRAPHICS_NATIVE_ORDINARY_VIEW texture=%llu view=%llu format=%u type=%u first_mip=%u mip_count=%u first_slice=%u slice_count=%u\n",
        (unsigned long long)params->texture,(unsigned long long)params->ret,(unsigned)params->format,
        (unsigned)params->texture_type,(unsigned)params->level_start,(unsigned)params->level_count,
        (unsigned)params->slice_start,(unsigned)params->slice_count);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_minimumLinearTextureAlignmentForPixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle minimumLinearTextureAlignmentForPixelFormat:to_metal_pixel_format(params->arg)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newLibrary(void *obj) {
  struct unixcall_mtldevice_newlibrary *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSError *err = NULL;
  params->ret_library = (obj_handle_t)[device newLibraryWithData:(dispatch_data_t)params->data error:&err];
  params->ret_error = (obj_handle_t)err;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunction(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->handle;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->arg encoding:NSUTF8StringEncoding];
  params->ret = (obj_handle_t)[library newFunctionWithName:name];
  [name release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_lengthOfBytesUsingEncoding(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = (uint64_t)[(NSString *)params->handle lengthOfBytesUsingEncoding:(NSStringEncoding)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSObject_description(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(NSObject *)params->handle description];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newComputePipelineState(void *obj) {
  struct unixcall_mtldevice_newcomputepso *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTComputePipelineInfo *info = params->info.ptr;
  MTLComputePipelineDescriptor *descriptor = [[MTLComputePipelineDescriptor alloc] init];
  NSError *err = NULL;
  descriptor.computeFunction = (id<MTLFunction>)info->compute_function;
  descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers;
  if (info->max_call_stack_depth > 0)
    descriptor.maxCallStackDepth = info->max_call_stack_depth;
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_buffers & (1 << i))
      descriptor.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso =
      (obj_handle_t)[device newComputePipelineStateWithDescriptor:descriptor options:options reflection:nil error:&err];
  params->ret_error = (obj_handle_t)err;
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addComputePipelineFunctionsWithDescriptor:descriptor
                                                                                                      error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoder(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle blitCommandEncoder];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_computeCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle
      computeCommandEncoderWithDispatchType:params->arg ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_renderCommandEncoder(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  struct WMTRenderPassInfo *info = (struct WMTRenderPassInfo *)params->arg;
  MTLRenderPassDescriptor *descriptor = [[MTLRenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].clearColor = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g, info->colors[i].clear_color.b,
        info->colors[i].clear_color.a
    );
    descriptor.colorAttachments[i].level = info->colors[i].level;
    descriptor.colorAttachments[i].slice = info->colors[i].slice;
    descriptor.colorAttachments[i].depthPlane = info->colors[i].depth_plane;
    descriptor.colorAttachments[i].texture = (id<MTLTexture>)info->colors[i].texture;
    descriptor.colorAttachments[i].loadAction = (MTLLoadAction)info->colors[i].load_action;
    descriptor.colorAttachments[i].storeAction = (MTLStoreAction)info->colors[i].store_action;
    descriptor.colorAttachments[i].resolveTexture = (id<MTLTexture>)info->colors[i].resolve_texture;
    descriptor.colorAttachments[i].resolveLevel = info->colors[i].resolve_level;
    descriptor.colorAttachments[i].resolveSlice = info->colors[i].resolve_slice;
    descriptor.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }

  if (info->depth.texture) {
    descriptor.depthAttachment.clearDepth = info->depth.clear_depth;
    descriptor.depthAttachment.depthPlane = info->depth.depth_plane;
    descriptor.depthAttachment.level = info->depth.level;
    descriptor.depthAttachment.slice = info->depth.slice;
    descriptor.depthAttachment.texture = (id<MTLTexture>)info->depth.texture;
    descriptor.depthAttachment.loadAction = (MTLLoadAction)info->depth.load_action;
    descriptor.depthAttachment.storeAction = (MTLStoreAction)info->depth.store_action;
  }

  if (info->stencil.texture) {
    descriptor.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    descriptor.stencilAttachment.depthPlane = info->stencil.depth_plane;
    descriptor.stencilAttachment.level = info->stencil.level;
    descriptor.stencilAttachment.slice = info->stencil.slice;
    descriptor.stencilAttachment.texture = (id<MTLTexture>)info->stencil.texture;
    descriptor.stencilAttachment.loadAction = (MTLLoadAction)info->stencil.load_action;
    descriptor.stencilAttachment.storeAction = (MTLStoreAction)info->stencil.store_action;
  }

  descriptor.defaultRasterSampleCount = info->default_raster_sample_count;
  descriptor.renderTargetArrayLength =
      info->render_target_array_length | ((NSUInteger)info->render_target_array_length_high << 8);
  descriptor.renderTargetHeight = info->render_target_height;
  descriptor.renderTargetWidth = info->render_target_width;
  descriptor.visibilityResultBuffer = (id<MTLBuffer>)info->visibility_buffer;
  if (info->center_sample_pattern || info->sample_positions.count) {
    NSUInteger sample_count = info->default_raster_sample_count;
    if (!sample_count) {
      for (unsigned i = 0; i < 8 && !sample_count; i++)
        sample_count = [(id<MTLTexture>)info->colors[i].texture sampleCount];
      if (!sample_count && info->depth.texture)
        sample_count = [(id<MTLTexture>)info->depth.texture sampleCount];
      if (!sample_count && info->stencil.texture)
        sample_count = [(id<MTLTexture>)info->stencil.texture sampleCount];
    }
    // The D3D12 validation layer only records center passes with 2/4/8
    // raster samples. Never silently substitute the standard pattern.
    if (sample_count != 2 && sample_count != 4 && sample_count != 8) {
      [descriptor release];
      return STATUS_UNSUCCESSFUL;
    }
    MTLSamplePosition positions[8];
    if (info->sample_positions.count && info->sample_positions.count != sample_count) {
      [descriptor release];
      return STATUS_UNSUCCESSFUL;
    }
    for (NSUInteger i = 0; i < sample_count; i++)
      positions[i] = info->sample_positions.count
          ? MTLSamplePositionMake(0.5f + info->sample_positions.positions[i][0] / 16.f,
                                  0.5f + info->sample_positions.positions[i][1] / 16.f)
          : MTLSamplePositionMake(0.5f, 0.5f);
    [descriptor setSamplePositions:positions count:sample_count];
    for (unsigned i = 0; i < 8; ++i)
      if (descriptor.colorAttachments[i].texture)
        descriptor.colorAttachments[i].storeActionOptions = MTLStoreActionOptionCustomSamplePositions;
    if (descriptor.depthAttachment.texture)
      descriptor.depthAttachment.storeActionOptions = MTLStoreActionOptionCustomSamplePositions;
  }
  if (info->forced16_half) {
    // D3D 16x standard pattern (1/16 pixel from center); probe pos16.m proves the two programmed 8x halves give
    // bit-exact D3D 16x coverage.
    static const int x16[16] = {1, -1, -3, 4, -5, 2, 5, 3, -2, 0, -4, -6, -8, 7, 6, -7};
    static const int y16[16] = {1, -3, 2, -1, -2, 5, 3, -5, 6, -7, -6, 4, 0, -4, 7, -8};
    if (info->forced16_half > 2 || info->default_raster_sample_count != 8 || info->center_sample_pattern ||
        info->sample_positions.count) {
      [descriptor release];
      return STATUS_UNSUCCESSFUL;
    }
    const unsigned base = (info->forced16_half - 1) * 8;
    MTLSamplePosition positions[8];
    for (unsigned i = 0; i < 8; i++)
      positions[i] = MTLSamplePositionMake(0.5f + x16[base + i] / 16.0f, 0.5f + y16[base + i] / 16.0f);
    [descriptor setSamplePositions:positions count:8];
  }

  if (info->tile_height && info->tile_width) {
    descriptor.tileWidth = info->tile_width;
    descriptor.tileHeight = info->tile_height;
  }

  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle renderCommandEncoderWithDescriptor:descriptor];

  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_endEncoding(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(id<MTLCommandEncoder>)params->handle endEncoding];
  return STATUS_SUCCESS;
}

#ifndef D3DALLOY_NO_PRIVATE_API

typedef NS_ENUM(NSUInteger, MTLLogicOperation) {
  MTLLogicOperationClear,
  MTLLogicOperationSet,
  MTLLogicOperationCopy,
  MTLLogicOperationCopyInverted,
  MTLLogicOperationNoop,
  MTLLogicOperationInvert,
  MTLLogicOperationAnd,
  MTLLogicOperationNand,
  MTLLogicOperationOr,
  MTLLogicOperationNor,
  MTLLogicOperationXor,
  MTLLogicOperationEquivalence,
  MTLLogicOperationAndReverse,
  MTLLogicOperationAndInverted,
  MTLLogicOperationOrReverse,
  MTLLogicOperationOrInverted,
};

@interface
MTLRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

@interface
MTLMeshRenderPipelineDescriptor ()

- (void)setLogicOperationEnabled:(BOOL)enable;
- (void)setLogicOperation:(MTLLogicOperation)op;

@end

#endif

static NTSTATUS
_MTLDevice_newRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTRenderPipelineInfo *info = params->info.ptr;
  MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_vertex_buffers & (1 << i))
      descriptor.vertexBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef D3DALLOY_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  descriptor.tessellationPartitionMode = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  descriptor.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  descriptor.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  descriptor.maxTessellationFactor = info->max_tessellation_factor;

  descriptor.vertexFunction = (id<MTLFunction>)info->vertex_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;
  descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers;
  if (info->max_vertex_amplification_count)
    descriptor.maxVertexAmplificationCount = info->max_vertex_amplification_count;

  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                    count:info->num_binary_archives_for_lookup];
  NSError *err = NULL;
  MTLPipelineOption options =
      info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                              options:options
                                                                                           reflection:nil
                                                                                                error:&err];
  params->ret_error = (obj_handle_t)err;
  if (!err && info->binary_archive_for_serialization) {
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization addRenderPipelineFunctionsWithDescriptor:descriptor
                                                                                                     error:&err];
  }
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newMeshRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *params = obj;
  const struct WMTMeshRenderPipelineInfo *info = params->info.ptr;
  MTLMeshRenderPipelineDescriptor *descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->colors[i].pixel_format);
    descriptor.colorAttachments[i].blendingEnabled = info->colors[i].blending_enabled;
    descriptor.colorAttachments[i].writeMask = (MTLColorWriteMask)info->colors[i].write_mask;

    descriptor.colorAttachments[i].alphaBlendOperation = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    descriptor.colorAttachments[i].rgbBlendOperation = (MTLBlendOperation)info->colors[i].rgb_blend_operation;

    descriptor.colorAttachments[i].sourceRGBBlendFactor = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    descriptor.colorAttachments[i].sourceAlphaBlendFactor = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    descriptor.colorAttachments[i].destinationRGBBlendFactor = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    descriptor.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_fragment_buffers & (1 << i))
      descriptor.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_mesh_buffers & (1 << i))
      descriptor.meshBuffers[i].mutability = MTLMutabilityImmutable;
    if (info->immutable_object_buffers & (1 << i))
      descriptor.objectBuffers[i].mutability = MTLMutabilityImmutable;
  }

#ifndef D3DALLOY_NO_PRIVATE_API
  [descriptor setLogicOperationEnabled:info->logic_operation_enabled];
  [descriptor setLogicOperation:(MTLLogicOperation)info->logic_operation];
#endif
  descriptor.depthAttachmentPixelFormat = to_metal_pixel_format(info->depth_pixel_format);
  descriptor.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  descriptor.alphaToCoverageEnabled = info->alpha_to_coverage_enabled;
  descriptor.rasterizationEnabled = info->rasterization_enabled;
  descriptor.rasterSampleCount = info->raster_sample_count;

  descriptor.objectFunction = (id<MTLFunction>)info->object_function;
  descriptor.meshFunction = (id<MTLFunction>)info->mesh_function;
  descriptor.fragmentFunction = (id<MTLFunction>)info->fragment_function;
  descriptor.payloadMemoryLength = info->payload_memory_length;

  descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->mesh_tgsize_is_multiple_of_sgwidth;
  descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth = info->object_tgsize_is_multiple_of_sgwidth;
  descriptor.supportIndirectCommandBuffers = info->support_indirect_command_buffers;
  if (info->max_vertex_amplification_count)
    descriptor.maxVertexAmplificationCount = info->max_vertex_amplification_count;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithMeshDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)err;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (!err && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addMeshRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBlitCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLBlitCommandEncoder> encoder = (id<MTLBlitCommandEncoder>)params->encoder;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default:
      assert(!next->type && "unhandled blit command type");
      break;
    case WMTBlitCommandSnapshotPredicate:
    case WMTBlitCommandCopyFromBufferToBuffer: {
      struct wmtcmd_blit_copy_from_buffer_to_buffer *body = (struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      [encoder copyFromBuffer:(id<MTLBuffer>)body->src
                 sourceOffset:body->src_offset
                     toBuffer:(id<MTLBuffer>)body->dst
            destinationOffset:body->dst_offset
                         size:body->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      struct wmtcmd_blit_copy_from_buffer_to_texture *body = (struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
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
      [encoder generateMipmapsForTexture:(id<MTLTexture>)body->texture];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      struct wmtcmd_blit_fence_op *body = (struct wmtcmd_blit_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      struct wmtcmd_blit_fillbuffer *body = (struct wmtcmd_blit_fillbuffer *)next;
      [encoder fillBuffer:(id<MTLBuffer>)body->buffer range:NSMakeRange(body->offset, body->length) value:body->value];
      break;
    }
    case WMTBlitCommandResolveCounters: {
      struct wmtcmd_blit_resolvecounters *body = (struct wmtcmd_blit_resolvecounters *)next;
      [encoder resolveCounters:(id<MTLCounterSampleBuffer>)body->sample_buffer
                       inRange:NSMakeRange(body->start, body->len)
             destinationBuffer:(id<MTLBuffer>)body->dst_buffer
             destinationOffset:body->dst_offset];
      break;
    }
    case WMTBlitCommandResetCommandsInBuffer: {
      struct wmtcmd_blit_resetcommands *body = (struct wmtcmd_blit_resetcommands *)next;
      [encoder resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                           withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTBlitCommandCopyTexture: {
      struct wmtcmd_blit_copy_texture *body = (struct wmtcmd_blit_copy_texture *)next;
      [encoder copyFromTexture:(id<MTLTexture>)body->src toTexture:(id<MTLTexture>)body->dst];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputeCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLComputeCommandEncoder> encoder = (id<MTLComputeCommandEncoder>)params->encoder;
  MTLSize threadgroup_size = {0, 0, 0};
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default:
      assert(!next->type && "unhandled compute command type");
      break;
    case WMTComputeCommandDispatchCompact: {
      const struct wmtcmd_compute_dispatch_compact *body = (const void *)next;
      [encoder dispatchThreadgroups:MTLSizeMake(body->x, body->y, body->z) threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatch: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreadgroups:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
              threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      struct wmtcmd_compute_dispatch *body = (struct wmtcmd_compute_dispatch *)next;
      [encoder dispatchThreads:MTLSizeMake(body->size.width, body->size.height, body->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchIndirectRange:
    case WMTComputeCommandDispatchIndirect: {
      struct wmtcmd_compute_dispatch_indirect *body = (struct wmtcmd_compute_dispatch_indirect *)next;
      const struct wmtcmd_compute_dispatch_indirect_range *range = next->type == WMTComputeCommandDispatchIndirectRange ? (const void *)next : NULL;
      uint64_t offset = body->indirect_args_offset;
      const uint32_t slots = range ? range->count : 1, stride = range ? range->stride : 0;
      for (uint32_t i = 0; i < slots; ++i, offset += stride) {
        if (i) [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
        [encoder dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                   indirectBufferOffset:offset
                                  threadsPerThreadgroup:threadgroup_size];
      }
      break;
    }
    case WMTComputeCommandSetPSO: {
      struct wmtcmd_compute_setpso *body = (struct wmtcmd_compute_setpso *)next;
      [encoder setComputePipelineState:(id<MTLComputePipelineState>)body->pso];
      threadgroup_size.width = body->threadgroup_size.width;
      threadgroup_size.height = body->threadgroup_size.height;
      threadgroup_size.depth = body->threadgroup_size.depth;
      break;
    }
    case WMTComputeCommandSetBuffer: {
      struct wmtcmd_compute_setbuffer *body = (struct wmtcmd_compute_setbuffer *)next;
      [encoder setBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      struct wmtcmd_compute_setbufferoffset *body = (struct wmtcmd_compute_setbufferoffset *)next;
      [encoder setBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTComputeCommandUseResource: {
      struct wmtcmd_compute_useresource *body = (struct wmtcmd_compute_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource usage:(MTLResourceUsage)body->usage];
      break;
    }
    case WMTComputeCommandSetBytes: {
      struct wmtcmd_compute_setbytes *body = (struct wmtcmd_compute_setbytes *)next;
      [encoder setBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTComputeCommandSetTexture: {
      struct wmtcmd_compute_settexture *body = (struct wmtcmd_compute_settexture *)next;
      [encoder setTexture:(id<MTLTexture>)body->texture atIndex:body->index];
      break;
    }
    case WMTComputeCommandUpdateFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      struct wmtcmd_compute_fence_op *body = (struct wmtcmd_compute_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence];
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      struct wmtcmd_compute_memory_barrier *body = (struct wmtcmd_compute_memory_barrier *)next;
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope];
      break;
    }    
    case WMTComputeCommandExecuteCommandsInBuffer: {
      struct wmtcmd_compute_executecommands *body = (struct wmtcmd_compute_executecommands *)next;
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    case WMTComputeCommandSetVisibleFunctionTable: {
      struct wmtcmd_compute_setfunctiontable *body = (struct wmtcmd_compute_setfunctiontable *)next;
      [encoder setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)body->table atBufferIndex:body->index];
      break;
    }
    case WMTComputeCommandSetIntersectionFunctionTable: {
      struct wmtcmd_compute_setfunctiontable *body = (struct wmtcmd_compute_setfunctiontable *)next;
      [encoder setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)body->table
                              atBufferIndex:body->index];
      break;
    }
    case WMTComputeCommandSetAccelerationStructure: {
      struct wmtcmd_compute_setaccelerationstructure *body =
          (struct wmtcmd_compute_setaccelerationstructure *)next;
      [encoder setAccelerationStructure:(id<MTLAccelerationStructure>)body->acceleration_structure
                          atBufferIndex:body->index];
      break;
    }
    }

    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLRenderCommandEncoder_encodeCommands(void *obj) {
  struct unixcall_generic_obj_cmd_noret *params = obj;
  const struct wmtcmd_base *next = params->cmd_head.ptr;
  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)params->encoder;
  typedef void (*IndirectDrawIMP)(id, SEL, MTLPrimitiveType, id<MTLBuffer>, NSUInteger);
  typedef void (*IndexedIndirectDrawIMP)(id, SEL, MTLPrimitiveType, MTLIndexType, id<MTLBuffer>, NSUInteger, id<MTLBuffer>, NSUInteger);
  IndirectDrawIMP indirect_draw = NULL;
  IndexedIndirectDrawIMP indexed_indirect_draw = NULL;
  while (next) {
    switch ((enum WMTRenderCommandType)next->type) {
    default:
      assert(!next->type && "unhandled render command type");
      break;
    case WMTRenderCommandNop:
      break;
    case WMTRenderCommandUseResource: {
      struct wmtcmd_render_useresource *body = (struct wmtcmd_render_useresource *)next;
      [encoder useResource:(id<MTLResource>)body->resource
                     usage:(MTLResourceUsage)body->usage
                    stages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setVertexBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setVertexBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setFragmentBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setFragmentBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setMeshBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setMeshBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      struct wmtcmd_render_setbuffer *body = (struct wmtcmd_render_setbuffer *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->buffer offset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      struct wmtcmd_render_setbufferoffset *body = (struct wmtcmd_render_setbufferoffset *)next;
      [encoder setObjectBufferOffset:body->offset atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      struct wmtcmd_render_setbytes *body = (struct wmtcmd_render_setbytes *)next;
      [encoder setFragmentBytes:body->bytes.ptr length:body->length atIndex:body->index];
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      struct wmtcmd_render_settexture *body = (struct wmtcmd_render_settexture *)next;
      [encoder setFragmentTexture:(id<MTLTexture>)body->texture atIndex:body->index];
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
    case WMTRenderCommandSetViewportScissors: {
      const struct wmtcmd_render_set_viewport_scissors *body = (const struct wmtcmd_render_set_viewport_scissors *)next;
      const MTLViewport *views = (const MTLViewport *)(body + 1);
      if (body->viewport_count)
        [encoder setViewports:views count:body->viewport_count];
      if (body->scissor_count)
        [encoder setScissorRects:(const MTLScissorRect *)(views + body->viewport_count) count:body->scissor_count];
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
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)body->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      struct wmtcmd_render_setdsso *body = (struct wmtcmd_render_setdsso *)next;
      [encoder setDepthStencilState:(id<MTLDepthStencilState>)body->dsso];
      [encoder setStencilReferenceValue:body->stencil_ref];
      break;
    }
    case WMTRenderCommandSetDepthStencilState: {
      struct wmtcmd_render_setdepthstencilstate *body = (struct wmtcmd_render_setdepthstencilstate *)next;
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
    case WMTRenderCommandDrawRun: {
      const struct wmtcmd_render_draw_run *run = (const void *)next;
      const MTLPrimitiveType primitive = (MTLPrimitiveType)run->primitive_type;
      WMT_REPLAY_DRAW_RUN(run, wmtdraw_parameters, draw,
        [encoder drawPrimitives:primitive vertexStart:draw.first vertexCount:draw.count
                  instanceCount:draw.instances baseInstance:draw.base_instance];
      );
      break;
    }
    case WMTRenderCommandDrawIndexedRun: {
      const struct wmtcmd_render_draw_run *run = (const void *)next;
      const struct wmtdraw_index_binding *binding = wmt_draw_run_binding(run);
      const MTLPrimitiveType primitive = (MTLPrimitiveType)run->primitive_type;
      const MTLIndexType index_type = (run->flags & WMTDrawRunIndex32) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
      const id<MTLBuffer> index_buffer = (id<MTLBuffer>)binding->index_buffer;
      const uint64_t index_offset = binding->index_offset;
      const uint64_t width = index_type == MTLIndexTypeUInt32 ? 4 : 2;
      WMT_REPLAY_DRAW_RUN(run, wmtdraw_indexed_parameters, draw,
        const uint64_t offset = width * draw.first;
        [encoder drawIndexedPrimitives:primitive indexCount:draw.count
                             indexType:index_type indexBuffer:index_buffer
                     indexBufferOffset:index_offset + offset
                         instanceCount:draw.instances baseVertex:draw.base_vertex baseInstance:draw.base_instance];
      );
      break;
    }
    case WMTRenderCommandDrawCompact: {
      const struct wmtcmd_render_draw_compact *body = (const void *)next;
      [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type vertexStart:body->vertex_start
                  vertexCount:body->vertex_count instanceCount:1 baseInstance:0];
      break;
    }
    case WMTRenderCommandDrawIndexedSmall: {
      const struct wmtcmd_render_draw_indexed_small *body = (const void *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type indexBuffer:(id<MTLBuffer>)body->index_buffer indexBufferOffset:body->index_buffer_offset
                       instanceCount:1 baseVertex:0 baseInstance:0];
      break;
    }
    case WMTRenderCommandDrawIndexedCompact: {
      const struct wmtcmd_render_draw_indexed_compact *body = (const void *)next;
      [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type indexCount:body->index_count
                           indexType:(MTLIndexType)body->index_type indexBuffer:(id<MTLBuffer>)body->index_buffer indexBufferOffset:body->index_buffer_offset
                       instanceCount:1 baseVertex:body->base_vertex baseInstance:0];
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
                         indexBuffer:(id<MTLBuffer>)body->index_buffer
                   indexBufferOffset:body->index_buffer_offset
                       instanceCount:body->instance_count
                          baseVertex:body->base_vertex
                        baseInstance:body->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirectRange:
    case WMTRenderCommandDrawIndirect: {
      struct wmtcmd_render_draw_indirect *body = (struct wmtcmd_render_draw_indirect *)next;
      const struct wmtcmd_render_draw_indirect_range *range = next->type == WMTRenderCommandDrawIndirectRange ? (const void *)next : NULL;
      uint64_t offset = body->indirect_args_offset;
      const uint32_t slots = range ? range->count : 1, stride = range ? range->stride : 0;
      if (slots >= WMTDrawIndirectCachedIMPMinCount && encoder) {
        const SEL sel = @selector(drawPrimitives:indirectBuffer:indirectBufferOffset:);
        const MTLPrimitiveType primitive = (MTLPrimitiveType)body->primitive_type;
        id<MTLBuffer> arguments = (id<MTLBuffer>)body->indirect_args_buffer;
        if (!indirect_draw)
          indirect_draw = (IndirectDrawIMP)class_getMethodImplementation(object_getClass(encoder), sel);
        for (uint32_t i = 0; i < slots; ++i, offset += stride)
          indirect_draw(encoder, sel, primitive, arguments, offset);
        break;
      }
      for (uint32_t i = 0; i < slots; ++i, offset += stride) {
        [encoder drawPrimitives:(MTLPrimitiveType)body->primitive_type
                  indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
            indirectBufferOffset:offset];
      }
      break;
    }
    case WMTRenderCommandDrawIndexedIndirectRange:
    case WMTRenderCommandDrawIndexedIndirect: {
      struct wmtcmd_render_draw_indexed_indirect *body = (struct wmtcmd_render_draw_indexed_indirect *)next;
      const struct wmtcmd_render_draw_indexed_indirect_range *range = next->type == WMTRenderCommandDrawIndexedIndirectRange ? (const void *)next : NULL;
      uint64_t offset = body->indirect_args_offset;
      const uint32_t slots = range ? range->count : 1, stride = range ? range->stride : 0;
      if (slots >= WMTDrawIndirectCachedIMPMinCount && encoder) {
        const SEL sel = @selector(drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:);
        const MTLPrimitiveType primitive = (MTLPrimitiveType)body->primitive_type;
        const MTLIndexType index_type = (MTLIndexType)body->index_type;
        id<MTLBuffer> indices = (id<MTLBuffer>)body->index_buffer, arguments = (id<MTLBuffer>)body->indirect_args_buffer;
        const NSUInteger index_offset = body->index_buffer_offset;
        if (!indexed_indirect_draw)
          indexed_indirect_draw = (IndexedIndirectDrawIMP)class_getMethodImplementation(object_getClass(encoder), sel);
        for (uint32_t i = 0; i < slots; ++i, offset += stride)
          indexed_indirect_draw(encoder, sel, primitive, index_type, indices, index_offset, arguments, offset);
        break;
      }
      for (uint32_t i = 0; i < slots; ++i, offset += stride) {
        [encoder drawIndexedPrimitives:(MTLPrimitiveType)body->primitive_type
                             indexType:(MTLIndexType)body->index_type
                           indexBuffer:(id<MTLBuffer>)body->index_buffer
                     indexBufferOffset:body->index_buffer_offset
                        indirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                  indirectBufferOffset:offset];
      }
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
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                                 indirectBufferOffset:body->indirect_args_offset
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
      [encoder memoryBarrierWithScope:(MTLBarrierScope)body->scope
                          afterStages:(MTLRenderStages)body->stages_after
                         beforeStages:(MTLRenderStages)body->stages_before];
      break;
    }
    case WMTRenderCommandD3DalloyGeometryDraw: {
      struct wmtcmd_render_d3dalloy_geometry_draw *body = (struct wmtcmd_render_d3dalloy_geometry_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandD3DalloyGeometryDrawIndexed: {
      struct wmtcmd_render_d3dalloy_geometry_draw_indexed *body = (struct wmtcmd_render_d3dalloy_geometry_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->warp_count, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandD3DalloyGeometryDrawIndirect: {
      struct wmtcmd_render_d3dalloy_geometry_draw_indirect *body = (struct wmtcmd_render_d3dalloy_geometry_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandD3DalloyGeometryDrawIndexedIndirect: {
      struct wmtcmd_render_d3dalloy_geometry_draw_indexed_indirect *body =
          (struct wmtcmd_render_d3dalloy_geometry_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->vertex_per_warp, 1, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandD3DalloyTessellationMeshDraw: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw *body = (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw *)next;
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandD3DalloyTessellationMeshDrawIndexed: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed *body =
          (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBufferOffset:body->draw_arguments_offset atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      [encoder drawMeshThreadgroups:MTLSizeMake(body->patch_per_mesh_instance, body->instance_count, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }

    case WMTRenderCommandD3DalloyTessellationMeshDrawIndirect: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indirect *body = (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandD3DalloyTessellationMeshDrawIndexedIndirect: {
      struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed_indirect *body =
          (struct wmtcmd_render_d3dalloy_tessellation_mesh_draw_indexed_indirect *)next;
      [encoder setObjectBuffer:(id<MTLBuffer>)body->index_buffer
                        offset:body->index_buffer_offset
                       atIndex:SM50_BINDING_INDEX_INDEX_BUFFER];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->indirect_args_buffer
                        offset:body->indirect_args_offset
                       atIndex:SM50_BINDING_INDEX_INDIRECT_ARGUMENTS];
      [encoder drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)body->dispatch_args_buffer
                                 indirectBufferOffset:body->dispatch_args_offset
                          threadsPerObjectThreadgroup:MTLSizeMake(body->threads_per_patch, body->patch_per_group, 1)
                            threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder setObjectBuffer:(id<MTLBuffer>)body->imm_draw_arguments
                        offset:0
                       atIndex:SM50_BINDING_INDEX_DRAW_ARGUMENTS];
      break;
    }
    case WMTRenderCommandUpdateFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder updateFence:(id<MTLFence>)body->fence afterStages:(MTLRenderStages)body->stages];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      struct wmtcmd_render_fence_op *body = (struct wmtcmd_render_fence_op *)next;
      [encoder waitForFence:(id<MTLFence>)body->fence beforeStages:(MTLRenderStages)body->stages];
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
      [encoder executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)body->indirect_command_buffer
                             withRange:NSMakeRange(body->location, body->length)];
      break;
    }
    }
    next = next->next.ptr;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_pixelFormat(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle pixelFormat];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_width(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle width];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_height(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle height];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_depth(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle depth];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_arrayLength(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle arrayLength];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_mipmapLevelCount(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLTexture>)params->handle mipmapLevelCount];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_replaceRegion(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  [(id<MTLTexture>)params->texture replaceRegion:MTLRegionMake3D(
                                                     params->origin.x, params->origin.y, params->origin.z,
                                                     params->size.width, params->size.height, params->size.depth
                                                 )
                                     mipmapLevel:params->level
                                           slice:params->slice
                                       withBytes:params->data.ptr
                                     bytesPerRow:params->bytes_per_row
                                   bytesPerImage:params->bytes_per_image];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_didModifyRange(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  [(id<MTLBuffer>)params->handle didModifyRange:NSMakeRange(params->arg, params->ret)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawable(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawableAfterMinimumDuration(void *obj) {
  struct unixcall_generic_obj_obj_double_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle presentDrawable:(id<MTLDrawable>)params->arg0
                                   afterMinimumDuration:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFamily(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsFamily:(MTLGPUFamily)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsBCTextureCompression(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsBCTextureCompression];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsTextureSampleCount(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle supportsTextureSampleCount:params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_programmableSamplePositionsSupported(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle areProgrammableSamplePositionsSupported];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_gpuCoreCount(void *obj) {
  /* Metal does not expose the core count; read "gpu-core-count" from the IOKit entry of
   * this exact device (registryID), or from the AGXAccelerator above it. */
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = 0;
  io_registry_entry_t entry = IOServiceGetMatchingService(
      kIOMainPortDefault, IORegistryEntryIDMatching([(id<MTLDevice>)params->handle registryID]));
  if (!entry)
    return STATUS_SUCCESS;
  CFTypeRef value = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, CFSTR("gpu-core-count"),
                                                    kCFAllocatorDefault,
                                                    kIORegistryIterateRecursively | kIORegistryIterateParents);
  uint32_t count = 0;
  if (value && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberSInt32Type, &count))
    params->ret = count;
  if (value)
    CFRelease(value);
  IOObjectRelease(entry);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_hasUnifiedMemory(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle hasUnifiedMemory];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_sharedCaptureManager(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[MTLCaptureManager sharedCaptureManager];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_startCapture(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *params = obj;
  MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
  const struct WMTCaptureInfo *info = params->info.ptr;
  desc.destination = (MTLCaptureDestination)info->destination;
  desc.captureObject = (id)info->capture_object;
  NSString *path_str = [[NSString alloc] initWithCString:info->output_url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  desc.outputURL = url;
  [(MTLCaptureManager *)params->capture_manager startCaptureWithDescriptor:desc error:nil];
  [url release];
  [path_str release];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCaptureManager_stopCapture(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  [(MTLCaptureManager *)params->handle stopCapture];
  return STATUS_SUCCESS;
}

#include <signal.h>

void
temp_handler(int signum) {
  fprintf(stderr, "received signal %d in temp_handler(), and it may cause problem!\n", signum);
}

static const int SIGNALS[] = {
    SIGHUP,
    SIGINT,
    SIGTERM,
    SIGUSR2,
    SIGILL,
    SIGTRAP,
    SIGABRT,
    SIGFPE,
    SIGBUS,
    SIGSEGV,
    SIGQUIT
#ifdef SIGSYS
    ,
    SIGSYS
#endif
#ifdef SIGXCPU
    ,
    SIGXCPU
#endif
#ifdef SIGXFSZ
    ,
    SIGXFSZ
#endif
#ifdef SIGEMT
    ,
    SIGEMT
#endif
    ,
    SIGUSR1
#ifdef SIGINFO
    ,
    SIGINFO
#endif
};

static NTSTATUS
_MTLDevice_newTemporalScaler(void *obj) {
  struct unixcall_mtldevice_newfxtemporalscaler *params = obj;
  MTLFXTemporalScalerDescriptor *desc = [[MTLFXTemporalScalerDescriptor alloc] init];
  const struct WMTFXTemporalScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.depthTextureFormat = to_metal_pixel_format(info->depth_format);
  desc.motionTextureFormat = to_metal_pixel_format(info->motion_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  desc.inputContentMaxScale = info->input_content_max_scale;
  desc.inputContentMinScale = info->input_content_min_scale;
  desc.inputContentPropertiesEnabled = info->input_content_properties_enabled;
  #if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    desc.requiresSynchronousInitialization = info->requires_synchronous_initialization;
  }
  #endif
  desc.autoExposureEnabled = info->auto_exposure;

  struct sigaction old_action[sizeof(SIGNALS) / sizeof(int)], new_action;
  if (@available(macOS 16, *)) {} else {
    new_action.sa_handler = temp_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &new_action, &old_action[i]);
  }

  params->ret = (obj_handle_t)[desc newTemporalScalerWithDevice:(id<MTLDevice>)params->device];

  if (@available(macOS 16, *)) {} else {
    for (unsigned int i = 0; i < sizeof(SIGNALS) / sizeof(int); i++)
      sigaction(SIGNALS[i], &old_action[i], NULL);
  }

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSpatialScaler(void *obj) {
  struct unixcall_mtldevice_newfxspatialscaler *params = obj;
  MTLFXSpatialScalerDescriptor *desc = [[MTLFXSpatialScalerDescriptor alloc] init];
  const struct WMTFXSpatialScalerInfo *info = params->info.ptr;
  desc.colorTextureFormat = to_metal_pixel_format(info->color_format);
  desc.outputTextureFormat = to_metal_pixel_format(info->output_format);
  desc.inputWidth = info->input_width;
  desc.inputHeight = info->input_height;
  desc.outputWidth = info->output_width;
  desc.outputHeight = info->output_height;
  params->ret = (obj_handle_t)[desc newSpatialScalerWithDevice:(id<MTLDevice>)params->device];
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeTemporalScale(void *obj) {
  struct unixcall_mtlcommandbuffer_temporal_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXTemporalScaler> scaler = (id<MTLFXTemporalScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.depthTexture = (id<MTLTexture>)params->depth;
  scaler.motionTexture = (id<MTLTexture>)params->motion;
  scaler.exposureTexture = (id<MTLTexture>)params->exposure;
  scaler.fence = (id<MTLFence>)params->fence;
  const struct WMTFXTemporalScalerProps *props = params->props.ptr;
  scaler.inputContentWidth = props->input_content_width;
  scaler.inputContentHeight = props->input_content_height;
  scaler.reset = props->reset;
  scaler.depthReversed = props->depth_reversed;
  scaler.motionVectorScaleX = props->motion_vector_scale_x;
  scaler.motionVectorScaleY = props->motion_vector_scale_y;
  scaler.jitterOffsetX = props->jitter_offset_x;
  scaler.jitterOffsetY = props->jitter_offset_y;
  scaler.preExposure = props->pre_exposure;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeSpatialScale(void *obj) {
  struct unixcall_mtlcommandbuffer_spatial_scale *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  id<MTLFXSpatialScaler> scaler = (id<MTLFXSpatialScaler>)params->scaler;
  scaler.colorTexture = (id<MTLTexture>)params->color;
  scaler.outputTexture = (id<MTLTexture>)params->output;
  scaler.fence = (id<MTLFence>)params->fence;
  [scaler encodeToCommandBuffer:cmdbuf];
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_string(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [NSString stringWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static NTSTATUS
_NSString_alloc_init(void *obj) {
  struct unixcall_nsstring_string *params = obj;
  NSString *str = [[NSString alloc] initWithCString:params->buffer_ptr.ptr encoding:(NSStringEncoding)params->encoding];
  params->ret = (obj_handle_t)str;
  return STATUS_SUCCESS;
}

static bool
developer_hud_uses_metric_service(void) {
  Class cls = objc_lookUpClass("MTLHUDService");
  id instance = ((id (*)(id, SEL))objc_msgSend)((id)cls, @selector(instance));
  return [instance respondsToSelector:@selector(graphicsAPI)];
}

static NTSTATUS
_DeveloperHUDProperties_instance(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  Class cls = objc_lookUpClass(
      developer_hud_uses_metric_service()
          ? "MTLHUDService"
          : "_CADeveloperHUDProperties"
  );
  params->ret =
      (obj_handle_t)((id(*)(id, SEL))objc_msgSend)((id)cls, @selector(instance));
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_addLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_uint64_ret *params = obj;
  if (developer_hud_uses_metric_service()) {
    params->ret =
        ((bool (*)(id, SEL, id, id, id, id, uint32_t, uint32_t, uint32_t, uint64_t))objc_msgSend)(
            (id)params->handle,
            @selector(insertMetric:after:name:unit:nameColor:valueColor:visualType:options:),
            (id)params->arg0,
            (id)params->arg1,
            @"",
            @"",
            UINT32_MAX,
            UINT32_MAX,
            1u,
            0u
        );
  } else {
    params->ret =
        ((bool (*)(id, SEL, id, id))objc_msgSend)(
            (id)params->handle,
            @selector(addLabel:after:),
            (id)params->arg0,
            (id)params->arg1
        );
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_updateLabel(void *obj) {
  struct unixcall_generic_obj_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id, id)
  )objc_msgSend)((id)params->handle, @selector(updateLabel:value:), (id)params->arg0, (id)params->arg1);
  return STATUS_SUCCESS;
}

static NTSTATUS
_DeveloperHUDProperties_remove(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  ((void (*)(id, SEL, id))objc_msgSend)((id)params->handle, @selector(remove:), (id)params->arg);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalDrawable_texture(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<CAMetalDrawable>)params->handle texture];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_nextDrawable(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)composition_next_drawable((CAMetalLayer *)params->handle);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_acquirePresentDrawable(void *obj) {
  struct WMTPresentDrawable *present = obj;
  id<CAMetalDrawable> drawable = composition_next_drawable((CAMetalLayer *)present->layer);
  present->drawable = (obj_handle_t)drawable;
  if (drawable) {
    id<MTLTexture> source = (id<MTLTexture>)present->source;
    id<MTLTexture> destination = drawable.texture;
    present->texture = (obj_handle_t)destination;
    present->source_width = (uint32_t)source.width;
    present->source_height = (uint32_t)source.height;
    present->destination_width = (uint32_t)destination.width;
    present->destination_height = (uint32_t)destination.height;
    present->source_format = (enum WMTPixelFormat)source.pixelFormat;
    present->destination_format = (enum WMTPixelFormat)destination.pixelFormat;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXSpatialScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsFXTemporalScaler(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [MTLFXTemporalScalerDescriptor supportsDevice:(id<MTLDevice>)params->handle];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setProps(void *obj) {
  struct unixcall_generic_obj_constptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  const struct WMTLayerProps *props = params->arg.ptr;
  execute_on_main(^{
    layer.device = (id<MTLDevice>)props->device;
    layer.opaque = props->opaque;
    layer.framebufferOnly = props->framebuffer_only;
    layer.contentsScale = props->contents_scale;
    // Composition sources own their display synchronization: it must stay
    // enabled from creation so every published drawable reports a real time.
    if (![layer isKindOfClass:objc_lookUpClass("WMTCompositionMetalLayer")])
      layer.displaySyncEnabled = props->display_sync_enabled;
    layer.drawableSize = CGSizeMake(props->drawable_width, props->drawable_height);
    layer.pixelFormat = to_metal_pixel_format(props->pixel_format);
    composition_layer_resize(layer);
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_getProps(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTLayerProps *props = params->arg.ptr;
  props->device = (obj_handle_t)layer.device;
  props->opaque = layer.opaque;
  props->framebuffer_only = layer.framebufferOnly;
  props->contents_scale = layer.contentsScale;
  props->display_sync_enabled = layer.displaySyncEnabled;
  props->drawable_height = layer.drawableSize.height;
  props->drawable_width = layer.drawableSize.width;
  props->pixel_format = layer.pixelFormat;
  return STATUS_SUCCESS;
}

typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct macdrv_opaque_window *macdrv_window;
typedef struct macdrv_opaque_window_data *macdrv_window_data;
typedef struct opaque_window_surface *window_surface;
typedef struct opaque_HWND *HWND;
struct macdrv_win_data {
  HWND hwnd; /* hwnd that this private data belongs to */
  macdrv_window cocoa_window;
  macdrv_view cocoa_view;
  macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(BOOL);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};
_Static_assert(sizeof(struct macdrv_win_data) == 32, "macdrv window-data prefix");
_Static_assert(offsetof(struct macdrv_win_data, client_cocoa_view) == 24, "macdrv client-view prefix");
_Static_assert(sizeof(struct macdrv_functions_t) == 80, "macdrv first ten function slots");


/* Composition state is AppKit-main-thread owned. The source owns its exported
 * context/layers; targets own a distinct host and source; the reverse set is weak. */
@class WMTCompositionTarget;
@interface WMTCompositionMetalLayer : CAMetalLayer {
@public
  CAMetalLayer *paired_layer; /* Borrowed; protected by the owning surface. */
}
@end
@implementation WMTCompositionMetalLayer
@end
/* The opaque WMT handle owns an immutable pair. Never pass this envelope to
 * Metal: queue operations unwrap both real native CAMetalDrawables. */
@interface WMTCompositionDrawablePair : NSObject {
@public
  id<CAMetalDrawable> primary;
  id<CAMetalDrawable> residual;
}
@end
@implementation WMTCompositionDrawablePair
- (id<MTLTexture>)texture { return [primary texture]; }
- (void)dealloc {
  [primary release];
  [residual release];
  [super dealloc];
}
@end

void winemetal_composition_drawables(id drawable, id<CAMetalDrawable> *primary, id<CAMetalDrawable> *residual) {
  if ([drawable isKindOfClass:WMTCompositionDrawablePair.class]) {
    WMTCompositionDrawablePair *pair = drawable;
    *primary = pair->primary;
    *residual = pair->residual;
  } else { *primary = drawable; *residual = nil; }
}
static id composition_next_drawable(CAMetalLayer *layer) {
  if (![layer isKindOfClass:WMTCompositionMetalLayer.class] ||
      !((WMTCompositionMetalLayer *)layer)->paired_layer)
    return [layer nextDrawable];
  id<CAMetalDrawable> primary = [layer nextDrawable];
  if (!primary) return nil;
  id<CAMetalDrawable> residual = [((WMTCompositionMetalLayer *)layer)->paired_layer nextDrawable];
  if (!residual) return nil;
  WMTCompositionDrawablePair *pair = [[WMTCompositionDrawablePair alloc] init];
  if (!pair) return nil;
  pair->primary = [primary retain];
  pair->residual = [residual retain];
  return [pair autorelease];
}
@interface WMTCompositionSource : NSObject {
@public
  CAMetalLayer *layer;
  CALayer *root;
  id context;
  uint32_t context_id;
  NSHashTable *targets;
  id observer;
  float transform[6];
  uint32_t restrict_output;
  uint32_t restricted_display_id;
  BOOL premultiplied;
  CAMetalLayer *residual_layer;
  CALayer *residual_root;
  id residual_context;
  uint32_t residual_context_id;
  id residual_filter;
  id latest_drawable;
  BOOL deferred_present;
  double deferred_duration;
  uint32_t deferred_sync_interval;
  bool deferred_allow_tearing;
  uint64_t deferred_present_id;
}
- (void)updateGeometry;
- (void)updateObserver;
- (void)publishDeferredWithPeriod:(double)period;
- (void)cancelDeferred;
@end
@interface WMTCompositionTarget : NSObject {
@public
  uint64_t hwnd;
  BOOL topmost;
  BOOL dead;
  BOOL observing_view;
  NSWindow *lifetime_window;
  NSView *view;
  CALayer *host;
  CALayer *container;
  CALayer *residual_host;
  CALayer *residual_container;
  float offset_x;
  float offset_y;
  WMTCompositionSource *source;
}
- (void)layout;
- (void)detach;
- (void)bindView:(NSView *)new_view;
- (void)stopObservingView;
- (void)changed:(NSNotification *)notification;
@end

static NSMapTable *composition_layers;
static NSMapTable *composition_views;
static void composition_bind_observer(id observer, NSView *view);
static double composition_refresh_period(id observer);
static void composition_fail_observer(id observer);
static uint64_t composition_observed_present_id(id observer, id drawable);
static void composition_skip_deferred(id observer, id drawable, uint64_t present_id);
NSScreen *GetNSScreenForDisplayID(CGDirectDisplayID display_id);

static void composition_on_main(dispatch_block_t block) {
  struct macdrv_functions_t *functions = dlsym(RTLD_DEFAULT, "macdrv_functions");
  if ([NSThread isMainThread]) block();
  else if (functions && functions->on_main_thread) functions->on_main_thread(block);
  else execute_on_main(block);
}

static BOOL composition_is_residual_filter(id filter) {
  Class cls = objc_lookUpClass("CAFilter");
  if (![filter isKindOfClass:cls] || ![filter respondsToSelector:@selector(type)] ||
      ![filter respondsToSelector:@selector(isEnabled)]) return NO;
  id type = ((id (*)(id, SEL))objc_msgSend)(filter, @selector(type));
  return [type isKindOfClass:NSString.class] && [(NSString *)type isEqualToString:@"plusLIgnoreAlpha"] &&
      ((BOOL (*)(id, SEL))objc_msgSend)(filter, @selector(isEnabled));
}

static id composition_create_residual_filter(void) {
  Class cls = objc_lookUpClass("CAFilter");
  if (![cls respondsToSelector:@selector(filterWithType:)]) return nil;
  id filter = nil;
  @try {
    // Add residual RGB without plusL's extended-range clamp or changing base coverage.
    filter = [((id (*)(id, SEL, id))objc_msgSend)((id)cls, @selector(filterWithType:), @"plusLIgnoreAlpha") retain];
  } @catch (...) { return nil; }
  if (!composition_is_residual_filter(filter)) { [filter release]; return nil; }
  return filter;
}

@implementation WMTCompositionSource
- (id)init {
  if ((self = [super init])) {
    targets = [[NSHashTable weakObjectsHashTable] retain];
    transform[0] = transform[3] = 1;
  }
  return self;
}
- (void)updateGeometry {
  const CGSize size = layer.drawableSize;
  layer.bounds = CGRectMake(0, 0, size.width, size.height);
  layer.position = CGPointZero;
  const CGAffineTransform matrix = CGAffineTransformMake(
      transform[0], transform[1], transform[2], transform[3], transform[4], transform[5]);
  layer.affineTransform = matrix;
  // Export every transformed pixel, including negative scales/translations.
  root.bounds = CGRectUnion(layer.bounds, CGRectApplyAffineTransform(layer.bounds, matrix));
  if (residual_layer) {
    residual_layer.bounds = layer.bounds;
    residual_layer.position = layer.position;
    residual_layer.affineTransform = matrix;
    residual_root.bounds = root.bounds;
  }
  for (WMTCompositionTarget *target in targets) [target layout];
}
- (void)updateObserver {
  NSView *selected = nil;
  for (WMTCompositionTarget *target in targets) {
    if (!target->dead && target->view.window == target->lifetime_window && !target->view.hidden) {
      selected = target->view; break;
    }
  }
  composition_bind_observer(observer, selected);
}
- (void)publishDeferredWithPeriod:(double)period {
  if (!deferred_present || !targets.count) return;
  double duration = deferred_duration;
  if (deferred_sync_interval) {
    if (period > 0) duration = fmax(duration, deferred_sync_interval * period);
    else if (deferred_sync_interval > 1) {
      // Never turn an unavailable multiple-refresh deadline into Present(0).
      composition_fail_observer(observer);
      return;
    }
  }
  // SyncInterval 0 cancels duration, not synchronization permission. The
  // layers stay display-synced so every published drawable reports a time.
  deferred_duration = duration;
  deferred_present = NO;
  // Observation retirement is not scene retirement. Explicit source teardown
  // and replacement cancel deferred publication through cancelDeferred.
  id<CAMetalDrawable> primary, residual;
  winemetal_composition_drawables(latest_drawable, &primary, &residual);
  const double present_time = deferred_duration > 0 ? 0 : CACurrentMediaTime();
  if (deferred_duration > 0) [primary presentAfterMinimumDuration:deferred_duration]; else [primary presentAtTime:present_time];
  if (deferred_duration > 0) [residual presentAfterMinimumDuration:deferred_duration]; else [residual presentAtTime:present_time];
  deferred_present_id = 0;
}
- (void)cancelDeferred {
  if (!deferred_present) return;
  deferred_present = NO;
  // Source owns observer; replacement cancels this frame before releasing it.
  composition_skip_deferred(observer, latest_drawable, deferred_present_id);
  deferred_present_id = 0;
  deferred_allow_tearing = false;
  [latest_drawable release]; latest_drawable = nil;
}
- (void)dealloc {
  assert([NSThread isMainThread]);
  if (layer) [composition_layers removeObjectForKey:layer];
  [self cancelDeferred];
  composition_bind_observer(observer, nil);
  [observer release];
  [latest_drawable release];
  ((void (*)(id, SEL, id))objc_msgSend)(context, @selector(setLayer:), nil);
  [context release];
  if (residual_context)
    ((void (*)(id, SEL, id))objc_msgSend)(residual_context, @selector(setLayer:), nil);
  [residual_context release];
  [residual_filter release];
  [residual_layer removeFromSuperlayer];
  [residual_layer release];
  [residual_root release];
  [layer removeFromSuperlayer];
  [layer release];
  [root release];
  [targets release];
  [super dealloc];
}
@end

@implementation WMTCompositionTarget
- (void)stopObservingView {
  if (!observing_view) return;
  observing_view = NO;
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [view removeObserver:self forKeyPath:@"layer"];
  [view removeObserver:self forKeyPath:@"layer.contentsScale"];
  [view removeObserver:self forKeyPath:@"layer.geometryFlipped"];
  [view removeObserver:self forKeyPath:@"layer.superlayer"];
}
- (void)bindView:(NSView *)new_view {
  assert([NSThread isMainThread]);
  if (view == new_view && observing_view) return;
  [self stopObservingView];
  if (view != new_view) {
    [new_view retain];
    [view release];
    view = new_view;
  }
  if (!view.wantsLayer) view.wantsLayer = YES;
  [view addObserver:self forKeyPath:@"layer" options:0 context:NULL];
  [view addObserver:self forKeyPath:@"layer.contentsScale" options:0 context:NULL];
  [view addObserver:self forKeyPath:@"layer.geometryFlipped" options:0 context:NULL];
  [view addObserver:self forKeyPath:@"layer.superlayer" options:0 context:NULL];
  view.postsFrameChangedNotifications = YES;
  view.postsBoundsChangedNotifications = YES;
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  [center addObserver:self selector:@selector(changed:) name:NSViewFrameDidChangeNotification object:view];
  [center addObserver:self selector:@selector(changed:) name:NSViewBoundsDidChangeNotification object:view];
  [center addObserver:self selector:@selector(changed:) name:NSViewGlobalFrameDidChangeNotification object:view];
  [center addObserver:self selector:@selector(changed:) name:NSWindowDidChangeScreenNotification object:view.window];
  [center addObserver:self selector:@selector(changed:) name:NSWindowDidChangeBackingPropertiesNotification object:view.window];
  [center addObserver:self selector:@selector(changed:) name:NSWindowDidMoveNotification object:view.window];
  [center addObserver:self selector:@selector(changed:) name:NSApplicationDidChangeScreenParametersNotification object:nil];
  [center addObserver:self selector:@selector(changed:) name:NSWindowWillCloseNotification object:lifetime_window];
  observing_view = YES;
}
- (void)layout {
  if (!host || !source || dead) return;
  if (container.superlayer != view.layer || (residual_container && residual_container.superlayer != view.layer)) {
    [container removeFromSuperlayer];
    [residual_container removeFromSuperlayer];
    // Both bands composite above the window's ordinary swapchain content; the
    // zPosition band keeps FALSE below TRUE.
    [view.layer addSublayer:container];
    // Nested filtered groups cannot see the backing-layer backdrop. These
    // adjacent siblings blend base source-over, then only the opaque residual.
    if (residual_container) [view.layer insertSublayer:residual_container above:container];
  }
  const CGRect client_bounds = view.layer.bounds;
  CGRect clip = client_bounds;
  if (source->restrict_output) {
    NSScreen *screen = CGDisplayIsActive(source->restricted_display_id)
        ? GetNSScreenForDisplayID(source->restricted_display_id) : nil;
    if (!screen || !view.window) {
      container.hidden = YES; residual_container.hidden = YES; return;
    }
    const NSRect window_rect = [view.window convertRectFromScreen:screen.frame];
    const NSRect view_rect = [view convertRect:window_rect fromView:nil];
    CGRect display_rect = NSRectToCGRect(view_rect);
    if (!view.isFlipped)
      display_rect.origin.y = NSMinY(view.bounds) + NSMaxY(view.bounds) - CGRectGetMaxY(display_rect);
    clip = CGRectIntersection(clip, display_rect);
    if (CGRectIsNull(clip) || CGRectIsEmpty(clip)) {
      container.hidden = YES; residual_container.hidden = YES; return;
    }
  }
  container.hidden = NO;
  residual_container.hidden = NO;
  container.anchorPoint = CGPointZero;
  container.bounds = clip;
  container.position = clip.origin;
  container.geometryFlipped = YES;
  container.masksToBounds = YES;
  container.zPosition = topmost ? 1 : 0;
  const CGFloat pixels_per_point = view.layer.contentsScale > 0 ? view.layer.contentsScale : 1;
  // CALayerHost conversion includes inherited flips that frame/flip getters do
  // not describe. Convert the DXGI client center through the actual hierarchy.
  host.anchorPoint = CGPointMake(0.5, 0.5);
  host.bounds = source->root.bounds;
  const CGFloat center_x = CGRectGetMidX(host.bounds) / pixels_per_point;
  const CGFloat center_y = CGRectGetMidY(host.bounds) / pixels_per_point;
  host.affineTransform = CGAffineTransformMakeScale(1 / pixels_per_point, 1 / pixels_per_point);
  host.contentsScale = pixels_per_point;
  host.geometryFlipped = source->root.geometryFlipped;
  host.position = [view.layer convertPoint:CGPointMake(CGRectGetMinX(client_bounds) + center_x + offset_x,
                                                      CGRectGetMinY(client_bounds) + center_y + offset_y)
                                  toLayer:container];
  if (residual_host) {
    residual_container.anchorPoint = CGPointZero;
    residual_container.bounds = container.bounds;
    residual_container.position = container.position;
    residual_container.geometryFlipped = container.geometryFlipped;
    residual_container.masksToBounds = container.masksToBounds;
    residual_container.zPosition = container.zPosition;
    residual_host.anchorPoint = host.anchorPoint;
    residual_host.bounds = source->residual_root.bounds;
    residual_host.affineTransform = host.affineTransform;
    residual_host.contentsScale = host.contentsScale;
    residual_host.geometryFlipped = source->residual_root.geometryFlipped;
    residual_host.position = [container convertPoint:host.position toLayer:residual_container];
  }
}
- (void)detach {
  [self stopObservingView];
  [container removeFromSuperlayer];
  [residual_host removeFromSuperlayer];
  [residual_container removeFromSuperlayer];
  [residual_host release]; residual_host = nil;
  [residual_container release]; residual_container = nil;
  [host removeFromSuperlayer];
  [host release]; host = nil;
  [container release]; container = nil;
  WMTCompositionSource *old = source;
  source = nil;
  if (old) {
    [old->targets removeObject:self];
    [old updateObserver];
    [old release];
  }
}
- (void)changed:(NSNotification *)notification {
  assert([NSThread isMainThread]);
  [CATransaction begin]; [CATransaction setDisableActions:YES];
  if ([notification.name isEqualToString:NSWindowWillCloseNotification]) {
    dead = YES;
    [self detach];
  } else {
    [self layout];
    [source updateObserver];
    if (source && source->deferred_present)
      [source publishDeferredWithPeriod:source->deferred_sync_interval ? composition_refresh_period(source->observer) : 0];
  }
  [CATransaction commit];
}
- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object
                       change:(NSDictionary *)change context:(void *)context {
  [self changed:nil];
}
- (void)dealloc {
  assert([NSThread isMainThread]);
  if (source) {
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    [self detach];
    [CATransaction commit];
  }
  [view release];
  [lifetime_window release];
  [super dealloc];
}
@end

static BOOL composition_release(NSObject *object) {
  if (![object isKindOfClass:WMTCompositionSource.class] &&
      ![object isKindOfClass:WMTCompositionTarget.class]) return NO;
  // Capture an address, not self: block ownership must not resurrect a final release.
  const uint64_t handle = (uint64_t)object;
  composition_on_main(^{ [(id)handle release]; });
  return YES;
}

static NSView *composition_view_for_layer(CAMetalLayer *layer) {
  if (![layer isKindOfClass:WMTCompositionMetalLayer.class])
    return [layer.delegate isKindOfClass:NSView.class] ? (NSView *)layer.delegate : nil;
  assert([NSThread isMainThread]);
  WMTCompositionSource *source = [composition_layers objectForKey:layer];
  if (source) {
    [source updateObserver];
    for (WMTCompositionTarget *target in source->targets)
      if (!target->dead && target->view.window == target->lifetime_window && !target->view.hidden)
        return target->view;
    return nil;
  }
  return [layer.delegate isKindOfClass:NSView.class] ? (NSView *)layer.delegate : nil;
}

static void composition_query(CAMetalLayer *layer, dispatch_block_t block) {
  if ([layer isKindOfClass:WMTCompositionMetalLayer.class]) composition_on_main(block);
  else block(); // Ordinary WineMetalView presentation keeps its existing path.
}

static void composition_layer_resize(CAMetalLayer *layer) {
  if (![layer isKindOfClass:WMTCompositionMetalLayer.class]) return;
  WMTCompositionSource *source = [composition_layers objectForKey:layer];
  if (!source) return;
  if (source->residual_layer) {
    CAMetalLayer *residual = source->residual_layer;
    residual.device = layer.device;
    residual.framebufferOnly = layer.framebufferOnly;
    residual.contentsScale = layer.contentsScale;
    residual.displaySyncEnabled = layer.displaySyncEnabled;
    residual.drawableSize = layer.drawableSize;
    residual.pixelFormat = layer.pixelFormat;
  }
  [CATransaction begin]; [CATransaction setDisableActions:YES];
  [source updateGeometry];
  [CATransaction commit];
}

static NTSTATUS _WMTComposition_call(void *obj) {
  struct WMTCompositionCall *call = obj;
  call->result = 0;
  call->status = (int32_t)0x80070057;
  if (call->operation == 0) {
    call->argument = 0;
    if (!call->object || call->count > 1) return STATUS_SUCCESS;
    composition_on_main(^{
      Class context_class = objc_lookUpClass("CAContext");
      int (*main_connection)(void) = dlsym(RTLD_DEFAULT, "CGSMainConnectionID");
      if (!context_class || !main_connection ||
          ![context_class respondsToSelector:@selector(contextWithCGSConnection:options:)]) return;
      WMTCompositionSource *source = [[WMTCompositionSource alloc] init];
      if (!source || !source->targets) { [source release]; return; }
      source->premultiplied = call->count != 0;
      source->layer = [[WMTCompositionMetalLayer alloc] init];
      source->root = [[CALayer alloc] init];
      if (source->premultiplied) {
        source->residual_filter = composition_create_residual_filter();
        if (!source->residual_filter) {
          [source release]; call->status = (int32_t)0x80004001; return;
        }
        source->residual_layer = [[CAMetalLayer alloc] init];
        source->residual_root = [[CALayer alloc] init];
      }
      if (!source->layer || !source->root ||
          (source->premultiplied && (!source->residual_layer || !source->residual_root))) {
        [source release]; call->status = (int32_t)0x8007000e; return;
      }
      const unsigned plane_count = source->premultiplied ? 2 : 1;
      for (unsigned plane = 0; plane < plane_count; ++plane) {
        CAMetalLayer *metal = plane ? source->residual_layer : source->layer;
        CALayer *root = plane ? source->residual_root : source->root;
        metal.device = (id<MTLDevice>)call->object;
        metal.framebufferOnly = NO;
        metal.presentsWithTransaction = YES;
        // Composition presentation timing requires a real display time for every
        // published drawable. Keep both planes display-synced from creation and
        // never toggle per present: SyncInterval 0 only cancels the minimum
        // duration, and a torn/unsynced present reports no presented time.
        metal.displaySyncEnabled = YES;
        metal.opaque = plane != 0;
        metal.contentsScale = 1;
        metal.anchorPoint = CGPointZero;
        root.anchorPoint = CGPointZero;
        root.geometryFlipped = YES;
        root.masksToBounds = NO;
        [root addSublayer:metal];
        id context = [((id (*)(id, SEL, int, id))objc_msgSend)(
            (id)context_class, @selector(contextWithCGSConnection:options:),
            main_connection(), @{}) retain];
        uint32_t context_id = 0;
        if (context) {
          ((void (*)(id, SEL, id))objc_msgSend)(context, @selector(setLayer:), root);
          context_id = ((uint32_t (*)(id, SEL))objc_msgSend)(context, @selector(contextId));
        }
        if (plane) { source->residual_context = context; source->residual_context_id = context_id; }
        else { source->context = context; source->context_id = context_id; }
        if (!context_id) { [source release]; call->status = (int32_t)0x80004001; return; }
      }
      ((WMTCompositionMetalLayer *)source->layer)->paired_layer = source->residual_layer;
      if (!composition_layers) composition_layers = [[NSMapTable weakToWeakObjectsMapTable] retain];
      [composition_layers setObject:source forKey:source->layer];
      call->result = (uint64_t)source;
      call->argument = (uint64_t)source->layer;
      call->status = 0;
    });
  } else if (call->operation == 1) {
    struct macdrv_functions_t *functions = dlsym(RTLD_DEFAULT, "macdrv_functions");
    if (!functions || !functions->macdrv_get_cocoa_window) return STATUS_SUCCESS;
    // The exported get_win_data creates a D3DMetal client surface and binds
    // its view. Creating an empty target must not replace another target's
    // committed client view. Pin only the existing native window here; acquire
    // the actual client view when a nonnull source is committed.
    NSWindow *lifetime_window = [(NSWindow *)functions->macdrv_get_cocoa_window(
        (HWND)(uintptr_t)call->object, FALSE) retain];
    composition_on_main(^{
      if (!lifetime_window || !objc_lookUpClass("CALayerHost")) {
        [lifetime_window release]; return;
      }
      WMTCompositionTarget *target = [[WMTCompositionTarget alloc] init];
      if (!target) { [lifetime_window release]; return; }
      target->hwnd = call->object; target->topmost = call->count != 0;
      target->lifetime_window = lifetime_window; // Transfer the retained identity token.
      call->result = (uint64_t)target; call->status = 0;
    });
  } else if (call->operation == 2) {
    const struct WMTCompositionTargetState *states = (const void *)(uintptr_t)call->argument;
    if (call->count && !states) return STATUS_SUCCESS;
    struct macdrv_functions_t *functions = dlsym(RTLD_DEFAULT, "macdrv_functions");
    // Validate every attachment HWND before touching any target. Detach needs
    // only its retained graph, including after the real window is destroyed.
    // No Wine window-data lock crosses the main-thread hop. A live HWND may
    // recreate its client view; the exported getter materializes a client surface
    // and allocates a snapshot per call. The retained native window, not snapshot
    // or client-view addresses, guards HWND reuse.
    NSMutableArray *views = nil;
    for (uint32_t i = 0; i < call->count; ++i) {
      WMTCompositionTarget *target = (id)states[i].target;
      if (![target isKindOfClass:WMTCompositionTarget.class] ||
          (states[i].surface && ![(id)states[i].surface isKindOfClass:WMTCompositionSource.class])) {
        composition_on_main(^{ [views release]; });
        return STATUS_SUCCESS;
      }
      if (!states[i].surface) {
        if (views) [views addObject:target->view ?: (id)[NSNull null]];
        continue;
      }
      if (!functions || !functions->macdrv_get_cocoa_window) {
        composition_on_main(^{ [views release]; });
        return STATUS_SUCCESS;
      }
      if ((NSWindow *)functions->macdrv_get_cocoa_window((HWND)(uintptr_t)target->hwnd, FALSE) !=
          target->lifetime_window) {
        composition_on_main(^{ [views release]; });
        call->status = (int32_t)0x80070578; return STATUS_SUCCESS;
      }
      __block NSView *current = nil;
      __block BOOL dead = NO;
      composition_on_main(^{
        dead = target->dead;
        if (dead) return;
        NSView *candidate = target->view;
        if (!candidate || candidate.window != target->lifetime_window || candidate.hidden) {
          candidate = [composition_views objectForKey:target->lifetime_window];
          if (candidate && (candidate.window != target->lifetime_window || candidate.hidden)) {
            [composition_views removeObjectForKey:target->lifetime_window];
            candidate = nil;
          }
        }
        if (candidate.window == target->lifetime_window && !candidate.hidden) {
          current = candidate;
          if (current != target->view) [current retain];
        }
      });
      if (dead) {
        composition_on_main(^{ [views release]; });
        call->status = (int32_t)0x80070578; return STATUS_SUCCESS;
      }
      if (!current) {
        if (!functions->get_win_data || !functions->release_win_data) {
          composition_on_main(^{ [views release]; });
          return STATUS_SUCCESS;
        }
        // This is real first attachment, or Wine replaced the committed view.
        // Never invoke this materializing getter just to validate a live host.
        struct macdrv_win_data *data = functions->get_win_data((HWND)(uintptr_t)target->hwnd);
        BOOL valid = data && data->hwnd == (HWND)(uintptr_t)target->hwnd &&
            (NSWindow *)data->cocoa_window == target->lifetime_window && data->client_cocoa_view;
        current = valid ? (NSView *)data->client_cocoa_view : nil;
        if (current && current != target->view) [current retain];
        if (data) functions->release_win_data(data);
        if (!valid) {
          composition_on_main(^{ [views release]; });
          call->status = (int32_t)0x80070578; return STATUS_SUCCESS;
        }
        composition_on_main(^{
          // TRUE/FALSE targets, including different devices, share the actual
          // Wine consumer view. Weak entries own no window/view or scene state.
          if (!composition_views) composition_views = [[NSMapTable weakToWeakObjectsMapTable] retain];
          [composition_views setObject:current forKey:target->lifetime_window];
        });
      }
      BOOL replaced = current && current != target->view;
      if (replaced && !views) {
        views = [[NSMutableArray alloc] initWithCapacity:call->count];
        for (uint32_t previous = 0; previous < i; ++previous)
          [views addObject:((WMTCompositionTarget *)states[previous].target)->view ?: (id)[NSNull null]];
      }
      if (views) [views addObject:current];
      if (replaced) composition_on_main(^{ [current release]; });
    }
    composition_on_main(^{
      for (uint32_t i = 0; i < call->count; ++i) {
        if (!states[i].surface) continue;
        WMTCompositionTarget *target = (id)states[i].target;
        NSView *current = views ? [views objectAtIndex:i] : target->view;
        if (target->dead || current.window != target->lifetime_window || current.hidden) {
          call->status = (int32_t)0x80070578; return;
        }
        if (!current.window.opaque) { call->status = (int32_t)0x80004001; return; }
      }
      BOOL changed = NO;
      for (uint32_t i = 0; i < call->count; ++i) {
        WMTCompositionTarget *probe = (WMTCompositionTarget *)states[i].target;
        changed |= probe->source != (id)states[i].surface || views != nil ||
            probe->offset_x != states[i].offset_x || probe->offset_y != states[i].offset_y;
      }
      if (!changed) {
        call->status = 0; return;
      }
      // Prepare every host before mutating the committed graph. Each target owns
      // its host, not Wine's context-ID dictionary (the same source may be reused).
      NSMutableArray *hosts = [NSMutableArray arrayWithCapacity:call->count];
      NSMutableArray *containers = [NSMutableArray arrayWithCapacity:call->count];
      BOOL needs_residual = NO;
      for (uint32_t i = 0; i < call->count; ++i) {
        WMTCompositionSource *source = (id)states[i].surface;
        needs_residual |= source && source->premultiplied &&
            ((WMTCompositionTarget *)states[i].target)->source != source;
      }
      NSMutableArray *residual_hosts = needs_residual ? [NSMutableArray arrayWithCapacity:call->count] : nil;
      NSMutableArray *residual_containers = needs_residual ? [NSMutableArray arrayWithCapacity:call->count] : nil;
      for (uint32_t i = 0; i < call->count; ++i) {
        WMTCompositionSource *source = (id)states[i].surface;
        WMTCompositionTarget *target = (id)states[i].target;
        if (target->source == source) {
          [hosts addObject:[NSNull null]]; [containers addObject:[NSNull null]];
          [residual_hosts addObject:[NSNull null]]; [residual_containers addObject:[NSNull null]];
          continue;
        }
        CALayer *host = source ? [(CALayer *)[objc_lookUpClass("CALayerHost") alloc] init] : nil;
        CALayer *container = source ? [[CALayer alloc] init] : nil;
        CALayer *residual_host = source && source->premultiplied
            ? [(CALayer *)[objc_lookUpClass("CALayerHost") alloc] init] : nil;
        CALayer *residual_container = source && source->premultiplied ? [[CALayer alloc] init] : nil;
        if (source && (!host || !container || (source->premultiplied && (!residual_host || !residual_container)))) {
          [host release]; [container release]; [residual_host release]; [residual_container release];
          call->status = (int32_t)0x8007000e; return;
        }
        if (residual_container) {
          // Blend only the opaque residual plane. The base retains ordinary
          // source-over; putting the additive filter on the entire image would be incorrect.
          @try { residual_container.compositingFilter = source->residual_filter; }
          @catch (...) {
            [host release]; [container release]; [residual_host release]; [residual_container release];
            call->status = (int32_t)0x80004001; return;
          }
          if (!composition_is_residual_filter(residual_container.compositingFilter)) {
            [host release]; [container release]; [residual_host release]; [residual_container release];
            call->status = (int32_t)0x80004001; return;
          }
          ((void (*)(id, SEL, uint32_t))objc_msgSend)(residual_host, @selector(setContextId:), source->residual_context_id);
          [residual_container addSublayer:residual_host];
        }
        if (source) ((void (*)(id, SEL, uint32_t))objc_msgSend)(host, @selector(setContextId:), source->context_id);
        [hosts addObject:host ?: (id)[NSNull null]];
        [containers addObject:container ?: (id)[NSNull null]];
        [residual_hosts addObject:residual_host ?: (id)[NSNull null]];
        [residual_containers addObject:residual_container ?: (id)[NSNull null]];
        [host release]; [container release]; [residual_host release]; [residual_container release];
      }
      // Reject unsupported clock-dependent publication before changing any
      // committed target graph. Binding uses the real prepared HWND screen.
      for (uint32_t i = 0; i < call->count; ++i) {
        WMTCompositionSource *source = (id)states[i].surface;
        if (!source || !source->deferred_present || source->deferred_sync_interval <= 1) continue;
        WMTCompositionTarget *target = (id)states[i].target;
        composition_bind_observer(source->observer, views ? [views objectAtIndex:i] : target->view);
        if (composition_refresh_period(source->observer) <= 0) {
          for (uint32_t j = 0; j <= i; ++j) {
            WMTCompositionSource *bound = (id)states[j].surface;
            if (bound && bound->deferred_present && bound->deferred_sync_interval > 1)
              [bound updateObserver]; // Restore the unchanged committed graph's display.
          }
          call->status = (int32_t)0x887a0004; return;
        }
      }
      [CATransaction begin]; [CATransaction setDisableActions:YES];
      for (uint32_t i = 0; i < call->count; ++i) {
        WMTCompositionTarget *target = (id)states[i].target;
        if (target->source == (id)states[i].surface) {
          target->offset_x = states[i].offset_x;
          target->offset_y = states[i].offset_y;
          if (states[i].surface) [target bindView:views ? [views objectAtIndex:i] : target->view];
          [target layout];
          [target->source updateObserver];
          continue;
        }
        WMTCompositionSource *source = [(id)states[i].surface retain];
        [target detach];
        target->source = source;
        target->offset_x = states[i].offset_x;
        target->offset_y = states[i].offset_y;
        if (source) {
          target->host = [[hosts objectAtIndex:i] retain];
          target->container = [[containers objectAtIndex:i] retain];
          [target->container addSublayer:target->host];
          if (source->premultiplied) {
            target->residual_host = [[residual_hosts objectAtIndex:i] retain];
            target->residual_container = [[residual_containers objectAtIndex:i] retain];
          }
          [source->targets addObject:target];
          [target bindView:views ? [views objectAtIndex:i] : target->view];
          [target layout];
          [source updateObserver];
        }
      }
      for (uint32_t i = 0; i < call->count; ++i)
        if (states[i].surface) {
          // Every target snapshot is installed before the first publication.
          WMTCompositionSource *source = (id)states[i].surface;
          if (source->deferred_present)
            [source publishDeferredWithPeriod:source->deferred_sync_interval ? composition_refresh_period(source->observer) : 0];
        }
      [CATransaction commit];
      for (uint32_t i = 0; i < call->count; ++i)
        if (states[i].surface) {
          // Match WineContentView's native host-installation readiness hook.
          // Empty targets and pending frontend changes must remain inert.
          WMTCompositionTarget *target = (id)states[i].target;
          NSWindow *window = target->view.window;
          if ([window respondsToSelector:@selector(windowDidDrawContent)])
            ((void (*)(id, SEL))objc_msgSend)(window, @selector(windowDidDrawContent));
        }
      call->status = 0;
    });
    composition_on_main(^{ [views release]; });
  } else if (call->operation == 3) {
    WMTCompositionSource *source = (id)call->object;
    if (![source isKindOfClass:WMTCompositionSource.class]) return STATUS_SUCCESS;
    composition_on_main(^{
      id observer = [(id)call->argument retain];
      if (source->observer != observer) {
        [source cancelDeferred];
        source->deferred_allow_tearing = false;
        composition_bind_observer(source->observer, nil);
      }
      [source->observer release]; source->observer = observer;
      [source updateObserver];
      call->status = 0;
    });
  } else if (call->operation == 4) {
    WMTCompositionDrawablePair *pair = (id)call->object;
    if ([pair isKindOfClass:WMTCompositionDrawablePair.class]) {
      call->result = (uint64_t)pair->residual.texture; call->status = 0;
    }
  }
  return STATUS_SUCCESS;
}

void winemetal_composition_present(id<MTLDrawable> drawable, double duration,
                                  const struct WMTCompositionFrame *frame, uint32_t sync_interval, bool allow_tearing) {
  id<CAMetalDrawable> primary, residual;
  winemetal_composition_drawables(drawable, &primary, &residual);
  WMTCompositionSource *source = frame ? (id)frame->surface : nil;
  if (!source && !residual) {
    if (duration > 0) [primary presentAfterMinimumDuration:duration]; else [primary present];
    return;
  }
  if (source) for (unsigned i = 0; i < 6; ++i) if (!isfinite(frame->transform[i])) return;
  const struct WMTCompositionFrame captured = frame ? *frame : (struct WMTCompositionFrame){0};
  composition_on_main(^{
    const double period = source && source->targets.count && sync_interval ? composition_refresh_period(source->observer) : 0;
    if (source && source->targets.count && sync_interval > 1 && period <= 0) {
      // A lost display must not change the old scene or submit a zero deadline.
      composition_fail_observer(source->observer);
      return;
    }
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    if (source && (memcmp(source->transform, captured.transform, sizeof(source->transform)) ||
        source->restrict_output != captured.restrict_output ||
        source->restricted_display_id != captured.restricted_display_id)) {
      memcpy(source->transform, captured.transform, sizeof(source->transform));
      source->restrict_output = captured.restrict_output;
      source->restricted_display_id = captured.restricted_display_id;
      [source updateGeometry];
    }
    if (source && source->deferred_present) [source cancelDeferred];
    if (source && source->latest_drawable != (id)drawable) {
      // Keep the latest real buffers alive across GPU retirement and the
      // first target attachment. Commit must not require another Present.
      id latest = [(id)drawable retain];
      [source->latest_drawable release];
      source->latest_drawable = latest;
    }
    if (source) {
      // Capture the interval and validated tearing permission even without a target/cadence.
      // A bound source publishes below; an unbound source waits for Commit.
      if (source->observer) {
        source->deferred_present = YES;
        source->deferred_duration = duration;
        source->deferred_sync_interval = sync_interval;
        source->deferred_allow_tearing = allow_tearing;
        source->deferred_present_id = composition_observed_present_id(source->observer, drawable);
        [source publishDeferredWithPeriod:period];
      } else {
        // Swapchain teardown cleared/closed the observer before this accepted
        // task reached AppKit. Do not recache an already-cancelled publication.
        [source->latest_drawable release]; source->latest_drawable = nil;
      }
    } else if (duration > 0) {
      [primary presentAfterMinimumDuration:duration];
      [residual presentAfterMinimumDuration:duration];
    } else { [primary present]; [residual present]; }
    [CATransaction commit];
  });
}


static NTSTATUS
_CreateMetalViewFromHWND(void *obj) {
  struct unixcall_create_metal_view_from_hwnd *params = obj;

  struct macdrv_win_data *(*pfn_get_win_data)(HWND hwnd) = NULL;
  void (*pfn_release_win_data)(struct macdrv_win_data *data) = NULL;
  macdrv_metal_view (*pfn_macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d) = NULL;
  macdrv_metal_layer (*pfn_macdrv_view_get_metal_layer)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_get_win_data = macdrv_functions->get_win_data;
    pfn_release_win_data = macdrv_functions->release_win_data;
    pfn_macdrv_view_create_metal_view = macdrv_functions->macdrv_view_create_metal_view;
    pfn_macdrv_view_get_metal_layer = macdrv_functions->macdrv_view_get_metal_layer;
  } else {
    pfn_get_win_data = dlsym(RTLD_DEFAULT, "get_win_data");
    pfn_release_win_data = dlsym(RTLD_DEFAULT, "release_win_data");
    pfn_macdrv_view_create_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_create_metal_view");
    pfn_macdrv_view_get_metal_layer = dlsym(RTLD_DEFAULT, "macdrv_view_get_metal_layer");
  }

  if (pfn_get_win_data && pfn_release_win_data && pfn_macdrv_view_create_metal_view &&
      pfn_macdrv_view_get_metal_layer) {
    struct macdrv_win_data *win_data = pfn_get_win_data((HWND)params->hwnd);
    if (!win_data)
      return STATUS_SUCCESS;
    macdrv_metal_view view =
        pfn_macdrv_view_create_metal_view(win_data->client_cocoa_view, (macdrv_metal_device)params->device);
    params->ret_view = (obj_handle_t)view;
    NSWindow *window = nil;
    NSView *client_view = nil;
    if (view) {
      params->ret_layer = (obj_handle_t)pfn_macdrv_view_get_metal_layer(view);
      window = [(NSWindow *)win_data->cocoa_window retain];
      client_view = [(NSView *)win_data->client_cocoa_view retain];
    }
    pfn_release_win_data(win_data);
    if (view) composition_on_main(^{
      // Record the actual ordinary consumer, not another materializing lookup.
      // Composition overlays must reuse it rather than hide the game's view.
      if (window && client_view.window == window && !client_view.hidden) {
        if (!composition_views) composition_views = [[NSMapTable weakToWeakObjectsMapTable] retain];
        [composition_views setObject:client_view forKey:window];
      } else if (window) {
        [composition_views removeObjectForKey:window];
      }
      [client_view release]; [window release];
    });
  }

  return STATUS_SUCCESS;
}

static NTSTATUS
_ReleaseMetalView(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;

  void (*pfn_macdrv_view_release_metal_view)(macdrv_metal_view v) = NULL;

  struct macdrv_functions_t *macdrv_functions;
  if ((macdrv_functions = dlsym(RTLD_DEFAULT, "macdrv_functions"))) {
    pfn_macdrv_view_release_metal_view = macdrv_functions->macdrv_view_release_metal_view;
  } else {
    pfn_macdrv_view_release_metal_view = dlsym(RTLD_DEFAULT, "macdrv_view_release_metal_view");
  }

  if (pfn_macdrv_view_release_metal_view)
    pfn_macdrv_view_release_metal_view((macdrv_metal_view)params->handle);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Initialize(void *args) {
  struct sm50_initialize_params *params = args;

  params->ret =
      SM50Initialize(params->bytecode, params->bytecode_size, params->shader, params->reflection, params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Destroy(void *args) {
  struct sm50_destroy_params *params = args;

  SM50Destroy(params->shader);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50Compile(void *args) {
  struct sm50_compile_params *params = args;

  params->ret = SM50Compile(params->shader, params->args, params->func_name, params->bitcode, params->error);

  return STATUS_SUCCESS;
}

/* Same handler for both tables: the param layout is fixed-width and the destination is a caller-image address. */
static NTSTATUS
thunk_SM50CopyCompiledBitcode(void *args) {
  struct sm50_copy_compiled_bitcode_params *params = args;

  params->ret = SM50CopyCompiledBitcode(
      params->bitcode, (enum SM50_COMPILED_BITCODE_PART)params->part, (void *)params->destination, params->size
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params *params = args;

  SM50GetCompiledBitcode(params->bitcode, params->data_out);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50DestroyBitcode(void *args) {
  struct sm50_destroy_bitcode_params *params = args;

  SM50DestroyBitcode(params->bitcode);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, params->buffer, params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50FreeError(void *args) {
  struct sm50_free_error_params *params = args;

  SM50FreeError(params->error);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetErrorKind(void *args) {
  struct sm50_get_error_kind_params *params = args;
  params->kind = SM50GetErrorKind(params->error);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CloneError(void *args) {
  struct sm50_clone_error_params *params = args;
  params->ret = SM50CloneError(params->error, &params->clone);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params *params = args;

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, params->hull_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params *params = args;

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, params->domain_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params *params = args;

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, params->vertex_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params *params = args;

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, params->geometry_args, params->func_name, params->bitcode, params->error
  );

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandEncoder_setLabel(void *args) {
  struct unixcall_generic_obj_obj_noret *params = args;
  [(id<MTLCommandEncoder>)params->handle setLabel:(NSString *)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_setShouldMaximizeConcurrentCompilation(void *args) {
  struct unixcall_generic_obj_uint64_noret *params = args;
  [(id<MTLDevice>)params->handle setShouldMaximizeConcurrentCompilation:(BOOL)params->arg];
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params *params = args;
  SM50GetArgumentsInfo(params->shader, params->constant_buffers, params->arguments);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50NeedsRasterOrderedBufferMarker(void *args) {
  struct sm50_needs_raster_ordered_buffer_marker_params *params = args;
  params->ret = SM50NeedsRasterOrderedBufferMarker(
      params->shader, params->root_signature, params->root_signature_length);
  return STATUS_SUCCESS;
}

/*
 * Whole-pipeline raytracing compilation. Both thunks forward the description the
 * PE side built against dxil_raytracing.h verbatim; the compiler owns the meaning
 * of every field, so nothing here interprets the ABI. They must be registered at
 * their absolute table indices, because a NULL entry is an indirect call to
 * address 0 rather than a failure.
 */
static NTSTATUS
thunk_SM50RTEnumerateLibraryExports(void *args) {
  struct sm50_rt_enumerate_exports_params *params = args;
  uint64_t names_size = 0;

  params->ret = SM50RTEnumerateLibraryExports(
      params->bytecode, params->bytecode_size, params->exports, params->export_capacity, params->names,
      params->names_size, params->export_count, &names_size, params->error
  );
  if (params->names_size_out)
    *params->names_size_out = names_size;

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50RTCompileRaytracingPipeline(void *args) {
  struct sm50_rt_compile_pipeline_params *params = args;

  params->ret = SM50CompileRaytracingPipeline(
      params->desc, params->bitcode, params->metallib, params->functions, params->function_capacity,
      params->function_count, params->error
  );

  return STATUS_SUCCESS;
}

/* D3D12 work graph node shaders (dxil_workgraph.h): forwarded verbatim. */
static NTSTATUS
thunk_SM50WGReflectLibrary(void *args) {
  struct sm50_wg_reflect_library_params *params = args;
  params->ret = SM50WGReflectLibrary(
      params->bytecode, params->bytecode_size, params->nodes, params->node_capacity, params->outputs,
      params->output_capacity, params->names, params->names_size, params->node_count, params->output_count,
      params->names_size_out, params->error
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk_SM50WGCompileNode(void *args) {
  struct sm50_wg_compile_node_params *params = args;
  params->ret = SM50WGCompileNode(params->desc, params->bitcode, params->metallib, params->info, params->error);
  return STATUS_SUCCESS;
}

static inline void *
UInt32ToPtr(uint32_t v) {
  uint64_t base = 0;
  if (!v) return NULL;
  NTSTATUS status = _WMTGetGuestWindowBase(&base);
  assert(!status);
  return (void *)(base + v);
}

#ifndef D3DALLOY_NATIVE

static NTSTATUS
thunk32_SM50Initialize(void *args) {
  struct sm50_initialize_params32 *params = args;

  params->ret = SM50Initialize(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->shader),
      UInt32ToPtr(params->reflection), UInt32ToPtr(params->error)
  );

  return STATUS_SUCCESS;
}

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  uint32_t elements;
};

struct SM50_SHADER_COMMON_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flag;
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
  /** MTLPixelFormat */
  uint32_t pixel_formats[8];
  uint32_t native_sample_count;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

struct SM50_SHADER_ROOT_SIGNATURE_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t bytecode;
  uint32_t bytecode_length;
};

struct SM50_SO_CAPTURE_ELEMENT32 {
  uint32_t semantic_name;
  uint32_t semantic_index;
  uint32_t start_component;
  uint32_t component_count;
  uint32_t dword_offset;
};

struct SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t record_dwords;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_GS_STREAM_CAPTURE32 {
  uint32_t record_dwords;
  uint32_t num_elements;
  uint32_t elements;
};

struct SM50_SHADER_D3D12_GS_STREAMS_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t rasterized_stream;
  struct SM50_GS_STREAM_CAPTURE32 streams[4];
};
_Static_assert(sizeof(struct SM50_SHADER_D3D12_GS_STREAMS_DATA32) == 60, "Wine32 GS streams argument size");

struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t variant;
  uint32_t sample_count;
  uint32_t layout_out;
  uint8_t depth_attachment;
  uint8_t single_sample_target;
  uint32_t line_mode;
  uint32_t interpolation_out;
  uint32_t line_vertex_layout;
  uint8_t center_pattern;
  uint32_t distance_vertex_layout;
  uint32_t distance_vertex_count;
  uint32_t coarse_vertex_layout;
  uint8_t depth_clip_enable;
  uint8_t depth_bounds_emulated;
};
_Static_assert(offsetof(struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA32, distance_vertex_layout) == 40,
               "Wine32 MSAA distance layout pointer offset");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA32, distance_vertex_count) == 44,
               "Wine32 MSAA distance vertex count offset");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA32, depth_clip_enable) == 52,
               "Wine32 MSAA pixel depth clip flag offset");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA32, depth_bounds_emulated) == 53,
               "Wine32 MSAA pixel depth bounds flag offset");
_Static_assert(sizeof(struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA32) == 56,
               "Wine32 MSAA pixel argument size");
_Static_assert(sizeof(struct SM50_MSAA_PIXEL_LAYOUT) == 76 &&
               offsetof(struct SM50_MSAA_PIXEL_LAYOUT, depth_bounds_mask_dword) == 72,
               "Pointer-free MSAA pixel output layout shared with PE32");

struct SM50_SHADER_D3D12_LOGIC_OP_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t op;
  uint32_t rt_mask;
  uint8_t component_bits[8][4];
};
_Static_assert(sizeof(struct SM50_SHADER_D3D12_LOGIC_OP_DATA32) == 48, "Wine32 logic op argument size");

struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t view_count;
  uint32_t viewport_offsets[4];
  uint32_t render_target_offsets[4];
};
_Static_assert(sizeof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA32) == 44,
               "Wine32 indirect view locations argument size");
_Static_assert(_Alignof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA32) == 4,
               "Wine32 indirect view locations argument alignment");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA32, view_count) == 8,
               "Wine32 indirect view count offset");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA32, viewport_offsets) == 12,
               "Wine32 indirect viewport offsets offset");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA32, render_target_offsets) == 28,
               "Wine32 indirect render target offsets offset");
_Static_assert(sizeof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA) == 48,
               "Native indirect view locations argument size");
_Static_assert(_Alignof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA) == 8,
               "Native indirect view locations argument alignment");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA, view_count) == 12,
               "Native indirect view count offset");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA, viewport_offsets) == 16,
               "Native indirect viewport offsets offset");
_Static_assert(offsetof(struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA, render_target_offsets) == 32,
               "Native indirect render target offsets offset");

struct SM50_SHADER_D3D12_MSAA_VERTEX_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t variant;
  uint32_t sample_count;
  uint32_t layout_out;
  uint32_t line_mode;
  uint32_t interpolation;
  uint8_t depth_clip_enable;
  uint8_t front_counter_clockwise;
};
_Static_assert(offsetof(struct SM50_SHADER_D3D12_MSAA_VERTEX_DATA32, front_counter_clockwise) == 29,
               "Wine32 MSAA vertex winding flag offset");
_Static_assert(sizeof(struct SM50_SHADER_D3D12_MSAA_VERTEX_DATA32) == 32, "Wine32 MSAA vertex argument size");

struct SM50_SHADER_D3D12_MSAA_MESH_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t variant;
  uint32_t layout_out;
  uint32_t mesh_layout_out;
  uint32_t line_mode;
  uint32_t interpolation;
  uint8_t depth_clip_enable;
  uint32_t object_layout_out;
  uint8_t front_counter_clockwise;
};
_Static_assert(offsetof(struct SM50_SHADER_D3D12_MSAA_MESH_DATA32, front_counter_clockwise) == 36,
               "Wine32 MSAA mesh winding flag offset");
_Static_assert(sizeof(struct SM50_SHADER_D3D12_MSAA_MESH_DATA32) == 40, "Wine32 MSAA mesh argument size");
_Static_assert(sizeof(struct SM50_MSAA_MESH_CONTROL) == 16, "MSAA mesh capture control size");

struct SM50_SHADER_D3D12_TESS_CAPTURE_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t layout_out;
};

struct SM50_SHADER_GS_TESSELLATION_INPUT_DATA32 {
  uint32_t next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  SM50_TESS_CAPTURE_LAYOUT layout;
};

static bool
sm50_compilation_argument32_convert(
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32
) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;

  first_arg->type = SM50_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  while (args32) {
    switch (args32->type) {
    case SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT: {
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *data =
          malloc(sizeof(struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->num_output_slots = src->num_output_slots;
      data->num_elements = src->num_elements;
      data->strides[0] = src->strides[0];
      data->strides[1] = src->strides[1];
      data->strides[2] = src->strides[2];
      data->strides[3] = src->strides[3];
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_COMMON: {
      struct SM50_SHADER_COMMON_DATA32 *src = (void *)args32;
      struct SM50_SHADER_COMMON_DATA *data = malloc(sizeof(struct SM50_SHADER_COMMON_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->metal_version = src->metal_version;
      data->flags = src->flag;
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_PIXEL_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_PIXEL_SHADER_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->unorm_output_reg_mask = src->unorm_output_reg_mask;
      data->disable_depth_output = src->disable_depth_output;
      data->sample_mask = src->sample_mask;
      data->native_sample_count = src->native_sample_count;
      data->dual_source_blending = src->dual_source_blending;
      memcpy(data->pixel_formats, src->pixel_formats, sizeof(data->pixel_formats));
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_IA_INPUT_LAYOUT_DATA *data = malloc(sizeof(struct SM50_SHADER_IA_INPUT_LAYOUT_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->slot_mask = src->slot_mask;
      data->index_buffer_format = src->index_buffer_format;
      data->num_elements = src->num_elements;
      data->elements = UInt32ToPtr(src->elements);
      break;
    }
    case SM50_SHADER_GS_PASS_THROUGH: {
      struct SM50_SHADER_GS_PASS_THROUGH_DATA32 *src = (void *)args32;
      struct SM50_SHADER_GS_PASS_THROUGH_DATA *data = malloc(sizeof(struct SM50_SHADER_GS_PASS_THROUGH_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->Data = src->Data;
      data->RasterizationDisabled = src->RasterizationDisabled;
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->strip_topology = src->strip_topology;
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      struct SM50_SHADER_PSO_TESSELLATOR_DATA32 *src = (void *)args32;
      struct SM50_SHADER_PSO_TESSELLATOR_DATA *data = malloc(sizeof(struct SM50_SHADER_PSO_TESSELLATOR_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->max_potential_tess_factor = src->max_potential_tess_factor;
      break;
    }
    case SM50_SHADER_ROOT_SIGNATURE:
    case SM50_SHADER_ROOT_SIGNATURE2: {
      struct SM50_SHADER_ROOT_SIGNATURE_DATA32 *src = (void *)args32;
      struct SM50_SHADER_ROOT_SIGNATURE_DATA *data = malloc(sizeof(struct SM50_SHADER_ROOT_SIGNATURE_DATA));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      last_arg->next = NULL;
      data->type = src->type;
      data->bytecode = UInt32ToPtr(src->bytecode);
      data->bytecode_length = src->bytecode_length;
      break;
    }
    case SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE: {
      const struct SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA32 *src = (void *)args32;
      const struct SM50_SO_CAPTURE_ELEMENT32 *src_elements = UInt32ToPtr(src->elements);
      struct SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->record_dwords = src->record_dwords;
      data->num_elements = src->num_elements;
      if (src->num_elements) {
        struct SM50_SO_CAPTURE_ELEMENT *elements = calloc(src->num_elements, sizeof(*elements));
        if (!elements)
          return false;
        data->elements = elements;
        for (uint32_t i = 0; i < src->num_elements; ++i) {
          const char *name = UInt32ToPtr(src_elements[i].semantic_name);
          if (name && !(elements[i].semantic_name = strdup(name)))
            return false;
          elements[i].semantic_index = src_elements[i].semantic_index;
          elements[i].start_component = src_elements[i].start_component;
          elements[i].component_count = src_elements[i].component_count;
          elements[i].dword_offset = src_elements[i].dword_offset;
        }
      }
      break;
    }
    case SM50_SHADER_D3D12_GS_STREAMS: {
      const struct SM50_SHADER_D3D12_GS_STREAMS_DATA32 *src = (void *)args32;
      struct SM50_SHADER_D3D12_GS_STREAMS_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->rasterized_stream = src->rasterized_stream;
      for (uint32_t s = 0; s < 4; ++s) {
        const struct SM50_SO_CAPTURE_ELEMENT32 *src_elements = UInt32ToPtr(src->streams[s].elements);
        data->streams[s].record_dwords = src->streams[s].record_dwords;
        data->streams[s].num_elements = src->streams[s].num_elements;
        if (!src->streams[s].num_elements)
          continue;
        struct SM50_SO_CAPTURE_ELEMENT *elements = calloc(src->streams[s].num_elements, sizeof(*elements));
        if (!elements)
          return false;
        data->streams[s].elements = elements;
        for (uint32_t i = 0; i < src->streams[s].num_elements; ++i) {
          const char *name = UInt32ToPtr(src_elements[i].semantic_name);
          if (name && !(elements[i].semantic_name = strdup(name)))
            return false;
          elements[i].semantic_index = src_elements[i].semantic_index;
          elements[i].start_component = src_elements[i].start_component;
          elements[i].component_count = src_elements[i].component_count;
          elements[i].dword_offset = src_elements[i].dword_offset;
        }
      }
      break;
    }
    case SM50_SHADER_D3D12_LOGIC_OP: {
      const struct SM50_SHADER_D3D12_LOGIC_OP_DATA32 *src = (void *)args32;
      struct SM50_SHADER_D3D12_LOGIC_OP_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->op = src->op;
      data->rt_mask = src->rt_mask;
      memcpy(data->component_bits, src->component_bits, sizeof(data->component_bits));
      break;
    }
    case SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS: {
      const struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA32 *src = (void *)args32;
      struct SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->view_count = src->view_count;
      memcpy(data->viewport_offsets, src->viewport_offsets, sizeof(data->viewport_offsets));
      memcpy(data->render_target_offsets, src->render_target_offsets, sizeof(data->render_target_offsets));
      break;
    }
    case SM50_SHADER_D3D12_INDEX_PULL:
    case SM50_SHADER_D3D12_PIPELINE_STATS:
    case SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL:
    case SM50_SHADER_D3D12_COARSE_SHADING:
    case SM50_SHADER_D3D12_CONSERVATIVE_RASTER:
    case SM50_SHADER_D3D12_ATOMIC64_PHASE:
    case SM50_SHADER_D3D12_SAMPLE_MASK_CONSTANT:
    case SM50_SHADER_D3D12_CAPTURE_FRONTIER:
    case SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM: {
      struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *data = malloc(sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = data;
      data->next = NULL;
      data->type = args32->type;
      break;
    }
    case SM50_SHADER_D3D12_MSAA_PIXEL: {
      const struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA32 *src = (void *)args32;
      struct SM50_SHADER_D3D12_MSAA_PIXEL_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->variant = src->variant;
      data->sample_count = src->sample_count;
      data->layout_out = UInt32ToPtr(src->layout_out);
      data->depth_attachment = src->depth_attachment;
      data->single_sample_target = src->single_sample_target;
      data->line_mode = src->line_mode;
      data->interpolation_out = UInt32ToPtr(src->interpolation_out);
      data->line_vertex_layout = UInt32ToPtr(src->line_vertex_layout);
      data->center_pattern = src->center_pattern;
      data->distance_vertex_layout = UInt32ToPtr(src->distance_vertex_layout);
      data->distance_vertex_count = src->distance_vertex_count;
      data->coarse_vertex_layout = UInt32ToPtr(src->coarse_vertex_layout);
      data->depth_clip_enable = src->depth_clip_enable;
      data->depth_bounds_emulated = src->depth_bounds_emulated;
      break;
    }
    case SM50_SHADER_D3D12_MSAA_VERTEX: {
      const struct SM50_SHADER_D3D12_MSAA_VERTEX_DATA32 *src = (void *)args32;
      struct SM50_SHADER_D3D12_MSAA_VERTEX_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->variant = src->variant;
      data->sample_count = src->sample_count;
      data->layout_out = UInt32ToPtr(src->layout_out);
      data->line_mode = src->line_mode;
      data->interpolation = UInt32ToPtr(src->interpolation);
      data->depth_clip_enable = src->depth_clip_enable;
      data->front_counter_clockwise = src->front_counter_clockwise;
      break;
    }
    case SM50_SHADER_D3D12_TESS_CAPTURE: {
      const struct SM50_SHADER_D3D12_TESS_CAPTURE_DATA32 *src = (void *)args32;
      struct SM50_SHADER_D3D12_TESS_CAPTURE_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->layout_out = UInt32ToPtr(src->layout_out);
      break;
    }
    case SM50_SHADER_D3D12_MSAA_MESH: {
      const struct SM50_SHADER_D3D12_MSAA_MESH_DATA32 *src = (void *)args32;
      struct SM50_SHADER_D3D12_MSAA_MESH_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->variant = src->variant;
      data->layout_out = UInt32ToPtr(src->layout_out);
      data->mesh_layout_out = UInt32ToPtr(src->mesh_layout_out);
      data->line_mode = src->line_mode;
      data->interpolation = UInt32ToPtr(src->interpolation);
      data->depth_clip_enable = src->depth_clip_enable;
      data->object_layout_out = UInt32ToPtr(src->object_layout_out);
      data->front_counter_clockwise = src->front_counter_clockwise;
      break;
    }

    case SM50_SHADER_GS_TESSELLATION_INPUT: {
      const struct SM50_SHADER_GS_TESSELLATION_INPUT_DATA32 *src = (void *)args32;
      struct SM50_SHADER_GS_TESSELLATION_INPUT_DATA *data = calloc(1, sizeof(*data));
      if (!data)
        return false;
      last_arg->next = data;
      last_arg = (void *)data;
      data->type = src->type;
      data->layout = src->layout;
      break;
    }
    case SM50_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
    args32 = UInt32ToPtr(args32->next);
  }
  return true;
}

static void
sm50_compilation_argument32_free(struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = first_arg->next;

  while (arg) {
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *next = arg->next;
    if (arg->type == SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE) {
      struct SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA *capture = (void *)arg;
      for (uint32_t i = 0; capture->elements && i < capture->num_elements; ++i)
        free((void *)capture->elements[i].semantic_name);
      free((void *)capture->elements);
    }
    if (arg->type == SM50_SHADER_D3D12_GS_STREAMS) {
      struct SM50_SHADER_D3D12_GS_STREAMS_DATA *streams = (void *)arg;
      for (uint32_t s = 0; s < 4; ++s) {
        for (uint32_t i = 0; streams->streams[s].elements && i < streams->streams[s].num_elements; ++i)
          free((void *)streams->streams[s].elements[i].semantic_name);
        free((void *)streams->streams[s].elements);
      }
    }
    free(arg);
    arg = next;
  }
}

static NTSTATUS
sm50_compilation_argument32_oom(
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, uint32_t bitcode, uint32_t error, int *ret
) {
  sm50_compilation_argument32_free(first_arg);
  if (bitcode)
    *(sm50_bitcode_t *)UInt32ToPtr(bitcode) = (sm50_bitcode_t){0};
  if (error)
    *(sm50_error_t *)UInt32ToPtr(error) = (sm50_error_t){0};
  *ret = 1; // SM50Compile's failure result; the null error means allocation failed in the thunk.
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50Compile(void *args) {
  struct sm50_compile_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->args);
  if (!sm50_compilation_argument32_convert(&first_arg, args32))
    return sm50_compilation_argument32_oom(&first_arg, params->bitcode, params->error, &params->ret);

  params->ret = SM50Compile(
      params->shader, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetCompiledBitcode(void *args) {
  struct sm50_get_compiled_bitcode_params32 *params = args;

  SM50GetCompiledBitcode(params->bitcode, UInt32ToPtr(params->data_out));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetErrorMessage(void *args) {
  struct sm50_get_error_message_params32 *params = args;

  params->ret_size = SM50GetErrorMessage(params->error, UInt32ToPtr(params->buffer), params->buffer_size);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineHull(void *args) {
  struct sm50_compile_tessellation_pipeline_hull_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->hull_args);
  if (!sm50_compilation_argument32_convert(&first_arg, args32))
    return sm50_compilation_argument32_oom(&first_arg, params->bitcode, params->error, &params->ret);

  params->ret = SM50CompileTessellationPipelineHull(
      params->vertex, params->hull, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileTessellationPipelineDomain(void *args) {
  struct sm50_compile_tessellation_pipeline_domain_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->domain_args);
  if (!sm50_compilation_argument32_convert(&first_arg, args32))
    return sm50_compilation_argument32_oom(&first_arg, params->bitcode, params->error, &params->ret);

  params->ret = SM50CompileTessellationPipelineDomain(
      params->hull, params->domain, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineVertex(void *args) {
  struct sm50_compile_geometry_pipeline_vertex_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->vertex_args);
  if (!sm50_compilation_argument32_convert(&first_arg, args32))
    return sm50_compilation_argument32_oom(&first_arg, params->bitcode, params->error, &params->ret);

  params->ret = SM50CompileGeometryPipelineVertex(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50CompileGeometryPipelineGeometry(void *args) {
  struct sm50_compile_geometry_pipeline_geometry_params32 *params = args;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA first_arg;
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 = UInt32ToPtr(params->geometry_args);
  if (!sm50_compilation_argument32_convert(&first_arg, args32))
    return sm50_compilation_argument32_oom(&first_arg, params->bitcode, params->error, &params->ret);

  params->ret = SM50CompileGeometryPipelineGeometry(
      params->vertex, params->geometry, &first_arg, UInt32ToPtr(params->func_name), UInt32ToPtr(params->bitcode),
      UInt32ToPtr(params->error)
  );

  sm50_compilation_argument32_free(&first_arg);

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50GetArgumentsInfo(void *args) {
  struct sm50_get_arguments_info_params32 *params = args;

  SM50GetArgumentsInfo(params->shader, UInt32ToPtr(params->constant_buffers), UInt32ToPtr(params->arguments));

  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50NeedsRasterOrderedBufferMarker(void *args) {
  struct sm50_needs_raster_ordered_buffer_marker_params32 *params = args;
  params->ret = SM50NeedsRasterOrderedBufferMarker(
      params->shader, UInt32ToPtr(params->root_signature), params->root_signature_length);
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50RTEnumerateLibraryExports(void *args) {
  struct sm50_rt_enumerate_exports_params32 *params = args;
  /*
   * The compiler writes a uint64_t name byte count, while a 32 bit caller only
   * has a 32 bit slot for it; widen into a local so the call can never write past
   * the caller's variable.
   */
  uint64_t names_size = 0;

  params->ret = SM50RTEnumerateLibraryExports(
      UInt32ToPtr(params->bytecode), params->bytecode_size, UInt32ToPtr(params->exports), params->export_capacity,
      UInt32ToPtr(params->names), params->names_size, UInt32ToPtr(params->export_count), &names_size,
      UInt32ToPtr(params->error)
  );
  if (names_size > UINT32_MAX)
    params->ret = 0;
  else if (params->names_size_out)
    *(uint32_t *)UInt32ToPtr(params->names_size_out) = (uint32_t)names_size;
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50RTCompileRaytracingPipeline(void *args) {
  struct sm50_rt_compile_pipeline_params32 *params = args;

  params->ret = 0;
  if (params->bitcode)
    *(sm50_bitcode_t *)UInt32ToPtr(params->bitcode) = 0;
  if (params->error)
    *(sm50_error_t *)UInt32ToPtr(params->error) = 0;
  if (params->function_count)
    *(uint32_t *)UInt32ToPtr(params->function_count) = 0;
  const struct sm50_rt_pipeline_desc32 *source = UInt32ToPtr(params->desc);
  if (!source || !params->functions || !params->bitcode || !params->metallib || !params->function_count ||
      (source->LibraryCount && !source->Libraries) || (source->ExportCount && !source->Exports) ||
      (source->LinkExportCount && !source->LinkExports))
    return STATUS_SUCCESS;
  struct SM50_RT_LIBRARY *libraries = calloc(source->LibraryCount, sizeof(*libraries));
  struct SM50_RT_EXPORT *exports = calloc(source->ExportCount, sizeof(*exports));
  struct SM50_WG_EXPORT *link_exports = calloc(source->LinkExportCount, sizeof(*link_exports));
  struct SM50_RT_FUNCTION_INFO *functions = calloc(params->function_capacity, sizeof(*functions));
  if ((source->LibraryCount && !libraries) || (source->ExportCount && !exports) ||
      (source->LinkExportCount && !link_exports) || (params->function_capacity && !functions))
    goto done;
  const struct sm50_rt_library32 *libraries32 = UInt32ToPtr(source->Libraries);
  const struct sm50_rt_export32 *exports32 = UInt32ToPtr(source->Exports);
  const struct sm50_wg_export32 *link_exports32 = UInt32ToPtr(source->LinkExports);
  for (uint32_t i = 0; i < source->LibraryCount; ++i)
    libraries[i] = (struct SM50_RT_LIBRARY){UInt32ToPtr(libraries32[i].Bytecode), libraries32[i].Size};
  for (uint32_t i = 0; i < source->ExportCount; ++i) {
    const struct sm50_rt_export32 *entry = &exports32[i];
    exports[i] = (struct SM50_RT_EXPORT){
      .Stage = entry->Stage, .LibraryIndex = entry->LibraryIndex, .HitGroupType = entry->HitGroupType,
      .ClosestHitIndex = entry->ClosestHitIndex, .AnyHitIndex = entry->AnyHitIndex,
      .IntersectionIndex = entry->IntersectionIndex, .EntryPoint = UInt32ToPtr(entry->EntryPoint),
      .ContainerEntryPoint = UInt32ToPtr(entry->ContainerEntryPoint),
      .LocalRootSignature = UInt32ToPtr(entry->LocalRootSignature), .LocalRootSignatureSize = entry->LocalRootSignatureSize};
  }
  for (uint32_t i = 0; i < source->LinkExportCount; ++i) {
    const struct sm50_wg_export32 *entry = &link_exports32[i];
    link_exports[i] = (struct SM50_WG_EXPORT){UInt32ToPtr(entry->Name), UInt32ToPtr(entry->EntryPoint),
      entry->LibraryIndex, entry->Scope, entry->Flags, entry->Reserved,
      UInt32ToPtr(entry->GlobalRootSignature), entry->GlobalRootSignatureSize,
      UInt32ToPtr(entry->LocalRootSignature), entry->LocalRootSignatureSize};
  }
  struct SM50_RT_PIPELINE_DESC desc = {
    .Libraries = libraries, .LibraryCount = source->LibraryCount, .Exports = exports, .ExportCount = source->ExportCount,
    .GlobalRootSignature = UInt32ToPtr(source->GlobalRootSignature), .GlobalRootSignatureSize = source->GlobalRootSignatureSize,
    .MaxRecursionDepth = source->MaxRecursionDepth, .MaxPayloadSize = source->MaxPayloadSize,
    .MaxAttributeSize = source->MaxAttributeSize, .Flags = source->Flags, .MetalVersion = source->MetalVersion,
    .LinkExports = link_exports, .LinkExportCount = source->LinkExportCount,
    .FunctionNames = UInt32ToPtr(source->FunctionNames), .FunctionNamesSize = source->FunctionNamesSize,
    .Incremental = UInt32ToPtr(source->Incremental)};
  uint32_t count = 0;
  params->ret = SM50CompileRaytracingPipeline(&desc, UInt32ToPtr(params->bitcode), UInt32ToPtr(params->metallib),
      functions, params->function_capacity, &count, UInt32ToPtr(params->error));
  if (params->ret) {
    struct sm50_rt_function_info32 *out = UInt32ToPtr(params->functions);
    for (uint32_t i = 0; i < count; ++i)
      out[i] = (struct sm50_rt_function_info32){(uint32_t)(uintptr_t)functions[i].Name, (uint32_t)(uintptr_t)functions[i].ExportName,
          functions[i].Kind, functions[i].ExportIndex, functions[i].TableIndex, functions[i].Flags};
    *(uint32_t *)UInt32ToPtr(params->function_count) = count;
  }
done:
  free(functions);
  free(link_exports);
  free(exports);
  free(libraries);
  return STATUS_SUCCESS;
}
static NTSTATUS
thunk32_SM50WGReflectLibrary(void *args) {
  struct sm50_wg_reflect_library_params32 *params = args;
  params->ret = SM50WGReflectLibrary(UInt32ToPtr(params->bytecode), params->bytecode_size,
      UInt32ToPtr(params->nodes), params->node_capacity, UInt32ToPtr(params->outputs), params->output_capacity,
      UInt32ToPtr(params->names), params->names_size, UInt32ToPtr(params->node_count),
      UInt32ToPtr(params->output_count), UInt32ToPtr(params->names_size_out), UInt32ToPtr(params->error));
  return STATUS_SUCCESS;
}

static NTSTATUS
thunk32_SM50WGCompileNode(void *args) {
  struct sm50_wg_compile_node_params32 *params = args;
  struct sm50_wg_node_compile_desc32 *source = UInt32ToPtr(params->desc);
  if (!source) { params->ret = 0; return STATUS_SUCCESS; }
  struct SM50_WG_LIBRARY *libraries = calloc(source->LibraryCount, sizeof(*libraries));
  struct SM50_WG_EXPORT *exports = calloc(source->ExportCount, sizeof(*exports));
  if ((source->LibraryCount && (!source->Libraries || !libraries)) || (source->ExportCount && (!source->Exports || !exports))) {
    free(libraries); free(exports); params->ret = 0;
    if (params->error) *(uint64_t *)UInt32ToPtr(params->error) = 0;
    return STATUS_SUCCESS;
  }
  const struct sm50_wg_library32 *source_libraries = UInt32ToPtr(source->Libraries);
  const struct sm50_wg_export32 *source_exports = UInt32ToPtr(source->Exports);
  for (uint32_t i = 0; i < source->LibraryCount; ++i)
    libraries[i] = (struct SM50_WG_LIBRARY){UInt32ToPtr(source_libraries[i].Bytecode), source_libraries[i].Size};
  for (uint32_t i = 0; i < source->ExportCount; ++i) {
    const struct sm50_wg_export32 *exp = &source_exports[i];
    exports[i] = (struct SM50_WG_EXPORT){UInt32ToPtr(exp->Name), UInt32ToPtr(exp->EntryPoint), exp->LibraryIndex, exp->Scope, exp->Flags, exp->Reserved,
        UInt32ToPtr(exp->GlobalRootSignature), exp->GlobalRootSignatureSize, UInt32ToPtr(exp->LocalRootSignature), exp->LocalRootSignatureSize};
  }
  struct SM50_WG_NODE_COMPILE_DESC desc = {
    .Library = UInt32ToPtr(source->Library), .LibrarySize = source->LibrarySize,
    .EntryPoint = UInt32ToPtr(source->EntryPoint), .FunctionName = UInt32ToPtr(source->FunctionName),
    .GlobalRootSignature = UInt32ToPtr(source->GlobalRootSignature), .GlobalRootSignatureSize = source->GlobalRootSignatureSize,
    .LocalRootSignature = UInt32ToPtr(source->LocalRootSignature), .LocalRootSignatureSize = source->LocalRootSignatureSize,
    .MetalVersion = source->MetalVersion, .Launch = source->Launch, .OutputCount = source->OutputCount,
    .Reserved = source->Reserved,
    .OutputElementBase = UInt32ToPtr(source->OutputElementBase), .OutputElementCount = UInt32ToPtr(source->OutputElementCount),
    .Libraries = libraries, .Exports = exports, .LibraryCount = source->LibraryCount, .ExportCount = source->ExportCount,
    .EntryLibraryIndex = source->EntryLibraryIndex, .AllowUnresolved = source->AllowUnresolved, .EntryExportIndex = source->EntryExportIndex,
  };
  params->ret = SM50WGCompileNode(&desc, UInt32ToPtr(params->bitcode), UInt32ToPtr(params->metallib),
                                UInt32ToPtr(params->info), UInt32ToPtr(params->error));
  free(exports); free(libraries);
  return STATUS_SUCCESS;
}
#endif /* D3DALLOY_NATIVE */

static NTSTATUS
_MTLCommandBuffer_error(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle error];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_logs(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLCommandBuffer>)params->handle logs];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLogContainer_enumerate(void *obj) {
  struct unixcall_enumerate *params = obj;
  uint64_t count = 0;
  uint64_t read = 0;
  id *buffer = params->buffer.ptr;
  for (id _ in (id<MTLLogContainer>)params->enumerable) {
    if (count >= params->start) {
      if (count < params->start + params->buffer_size) {
        buffer[count - params->start] = _;
        read++;
      } else {
        break;
      }
    }
    count++;
  }
  params->ret_read = read;
  return STATUS_SUCCESS;
}

CFStringRef
GetColorSpaceName(enum WMTColorSpace colorspace) {
  switch (colorspace) {
  case WMTColorSpaceSRGB:
    return kCGColorSpaceSRGB;
  case WMTColorSpaceSRGBLinear:
  case WMTColorSpaceHDR_scRGB:
    return kCGColorSpaceExtendedLinearSRGB;
  case WMTColorSpaceBT2020:
    return kCGColorSpaceITUR_2020_sRGBGamma;
  case WMTColorSpaceHDR_PQ:
    return kCGColorSpaceITUR_2100_PQ;
  default:
    return nil;
  }
}

static NTSTATUS
_CGColorSpace_checkColorSpaceSupported(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = false;
  CFStringRef name = GetColorSpaceName((enum WMTColorSpace)params->handle);
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  CGColorSpaceRelease(ref);
  params->ret = true;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MetalLayer_setColorSpace(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  enum WMTColorSpace colorspace = params->arg;
  CFStringRef name = GetColorSpaceName(colorspace);
  params->ret = false;
  if (!name)
    return STATUS_SUCCESS;
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  if (!ref)
    return STATUS_SUCCESS;
  execute_on_main(^{
    WMTCompositionSource *source = [layer isKindOfClass:WMTCompositionMetalLayer.class]
        ? [composition_layers objectForKey:layer] : nil;
    if (source && source->premultiplied && colorspace != WMTColorSpaceSRGB &&
        colorspace != WMTColorSpaceSRGBLinear && colorspace != WMTColorSpaceHDR_scRGB) {
      CGColorSpaceRelease(ref);
      return;
    }
    layer.colorspace = ref;
    layer.wantsExtendedDynamicRangeContent = WMT_COLORSPACE_IS_HDR(colorspace);
    if (source && source->residual_layer) {
      source->residual_layer.colorspace = ref;
      source->residual_layer.wantsExtendedDynamicRangeContent = WMT_COLORSPACE_IS_HDR(colorspace);
    }
    CGColorSpaceRelease(ref);
    params->ret = true;
  });
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetPrimaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = CGMainDisplayID();
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetSecondaryDisplayId(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = kCGNullDirectDisplay;

  uint32_t count = 0;
  CGGetOnlineDisplayList(0, NULL, &count);

  if (count == 0)
    return STATUS_SUCCESS;

  CGDirectDisplayID main_display = CGMainDisplayID();
  CGDirectDisplayID displays[count];
  CGGetOnlineDisplayList(count, displays, &count);

  for (uint32_t i = 0; i < count; i++) {
    CGDirectDisplayID id = displays[i];
    if (id == main_display)
      continue;
    if (CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
      continue;
    params->ret = id;
    break;
  }

  return STATUS_SUCCESS;
}

typedef struct icc_XYZ_t {
  uint32_t sig;      // 0x205a5958
  uint32_t reserved; // 0
  int32_t x;
  int32_t y;
  int32_t z;
} icc_XYZ_t;

bool
GetChromaticity_xy(ColorSyncProfileRef profile, CFStringRef tag, float *out_x, float *out_y) {
  CFDataRef tag_data = ColorSyncProfileCopyTag(profile, tag);
  if (!tag_data)
    return false;
  const icc_XYZ_t *data = (const icc_XYZ_t *)CFDataGetBytePtr(tag_data);
  if (CFDataGetLength(tag_data) != sizeof(icc_XYZ_t) || data->sig != 0x205a5958) {
    CFRelease(tag_data);
    return false;
  }
  double X = (int32_t)__builtin_bswap32(data->x) / 65536.0;
  double Y = (int32_t)__builtin_bswap32(data->y) / 65536.0;
  double Z = (int32_t)__builtin_bswap32(data->z) / 65536.0;
  CFRelease(tag_data);
  *out_x = X / (X + Y + Z);
  *out_y = Y / (X + Y + Z);
  return true;
}

bool
GetDisplayColorGamut(ColorSyncProfileRef profile, struct WMTDisplayDescription *desc_out) {
  return GetChromaticity_xy(
             profile, kColorSyncSigMediaWhitePointTag, &desc_out->white_points[0], &desc_out->white_points[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigRedColorantTag, &desc_out->red_primaries[0], &desc_out->red_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigGreenColorantTag, &desc_out->green_primaries[0], &desc_out->green_primaries[1]
         ) &&
         GetChromaticity_xy(
             profile, kColorSyncSigBlueColorantTag, &desc_out->blue_primaries[0], &desc_out->blue_primaries[1]
         );
}

NSScreen *
GetNSScreenForDisplayID(CGDirectDisplayID display_id) {
  for (NSScreen *screen in [NSScreen screens]) {
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    if (id == display_id) {
      return screen;
    }
  }
  return nil;
}

static NTSTATUS
_WMTGetDisplayDescription(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CGDirectDisplayID display_id = params->handle;
  struct WMTDisplayDescription *desc_out = params->arg.ptr;
  ColorSyncProfileRef profile = ColorSyncProfileCreateWithDisplayID(display_id);
  if (!profile || !GetDisplayColorGamut(profile, desc_out)) {
    ColorSyncProfileRef generic = ColorSyncProfileCreateWithName(kColorSyncGenericRGBProfile);
    GetDisplayColorGamut(generic, desc_out);
    if (generic)
      CFRelease(generic);
  }
  if (profile)
    CFRelease(profile);
  NSScreen *screen = GetNSScreenForDisplayID(display_id);
  if (screen) {
    desc_out->maximum_edr_color_component_value = [screen maximumExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_reference_edr_color_component_value =
        [screen maximumReferenceExtendedDynamicRangeColorComponentValue];
    desc_out->maximum_potential_edr_color_component_value =
        [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
  } else {
    desc_out->maximum_edr_color_component_value = 1.0;
    desc_out->maximum_reference_edr_color_component_value = 0.0;
    desc_out->maximum_potential_edr_color_component_value = 1.0;
  }
  return STATUS_SUCCESS;
}

struct DisplaySetting {
  uint64_t version;
  enum WMTColorSpace colorspace;
  struct WMTHDRMetadata hdr_metadata;
};

struct DisplaySetting g_display_settings[2] = {{0, 0, {}}, {0, 0, {}}};

static NTSTATUS
_MetalLayer_getEDRValue(void *obj) {
  struct unixcall_generic_obj_ptr_noret *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->handle;
  struct WMTEDRValue *value = params->arg.ptr;
  value->maximum_edr_color_component_value = 1.0;
  value->maximum_potential_edr_color_component_value = 1.0;

  composition_query(layer, ^{
    NSView *view = composition_view_for_layer(layer);
    NSScreen *screen = view.window.screen;
    if (!screen) return;
    value->maximum_edr_color_component_value =
        layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
    value->maximum_potential_edr_color_component_value = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
  });

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLLibrary_newFunctionWithConstants(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *params = obj;
  id<MTLLibrary> library = (id<MTLLibrary>)params->library;
  NSString *name = [[NSString alloc] initWithCString:(char *)params->name.ptr encoding:NSUTF8StringEncoding];
  struct WMTFunctionConstant *constants = (struct WMTFunctionConstant *)params->constants.ptr;
  NSError *err = NULL;
  MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
  for (uint64_t i = 0; i < params->num_constants; i++)
    [values setConstantValue:constants[i].data.ptr type:(MTLDataType)constants[i].type atIndex:constants[i].index];

  params->ret = (obj_handle_t)[library newFunctionWithName:name constantValues:values error:&err];
  params->ret_error = (obj_handle_t)err;
  [name release];
  [values release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySetting(void *obj) {
  struct unixcall_query_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  params->ret = false;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (setting->version) {
    *value = setting->hdr_metadata;
    params->colorspace = setting->colorspace;
    params->ret = true;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTUpdateDisplaySetting(void *obj) {
  struct unixcall_update_display_setting *params = obj;
  CGDirectDisplayID display_id = params->display_id;
  const struct WMTHDRMetadata *value = params->hdr_metadata.ptr;
  struct DisplaySetting *setting = &g_display_settings[display_id == CGMainDisplayID()];
  if (value) {
    setting->hdr_metadata = *value;
    setting->colorspace = params->colorspace;
    setting->version++;
  } else {
    setting->version = 0;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTQueryDisplaySettingForLayer(void *obj) {
  struct unixcall_query_display_setting_for_layer *params = obj;
  CAMetalLayer *layer = (CAMetalLayer *)params->layer;
  struct WMTHDRMetadata *hdr_metadata_out = params->hdr_metadata.ptr;

  params->version = 0;
  params->colorspace = WMTColorSpaceSRGB;
  *hdr_metadata_out = (struct WMTHDRMetadata){};
  params->edr_value.maximum_edr_color_component_value = 1.0;
  params->edr_value.maximum_potential_edr_color_component_value = 1.0;
  composition_query(layer, ^{
    NSView *view = composition_view_for_layer(layer);
    NSScreen *screen = view.window.screen;
    if (!screen) return;
    CGDirectDisplayID id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    struct DisplaySetting *setting = &g_display_settings[id == CGMainDisplayID()];
    *hdr_metadata_out = setting->hdr_metadata;
    params->version = setting->version;
    params->colorspace = setting->colorspace;
    params->edr_value.maximum_edr_color_component_value =
        layer.wantsExtendedDynamicRangeContent ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0;
    params->edr_value.maximum_potential_edr_color_component_value =
        screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
  });

  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_encodeWaitForEvent(void *obj) {
  struct unixcall_generic_obj_obj_uint64_noret *params = obj;
  [(id<MTLCommandBuffer>)params->handle encodeWaitForEvent:(id<MTLSharedEvent>)params->arg0 value:params->arg1];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_signalValue(void *obj) {
  struct unixcall_generic_obj_uint64_noret *params = obj;
  [(id<MTLSharedEvent>)params->handle setSignaledValue:params->arg];
  return STATUS_SUCCESS;
}

#ifndef D3DALLOY_NATIVE

// Callback blocks retain this state independently of the device and of the
// Wine notification thread. Destroy marks it closed; callbacks which arrive
// afterwards cannot touch a freed run loop or signal a reused Win32 handle.
@interface OwnedSharedEventListener : NSObject {
@public
  NSCondition *condition;
  CFRunLoopRef runloop;
  BOOL closing;
  MTLSharedEventListener *listener;
}
@end

@implementation OwnedSharedEventListener
- (id)init {
  if ((self = [super init])) {
    condition = [[NSCondition alloc] init];
    listener = [[MTLSharedEventListener alloc] init];
    if (!condition || !listener) {
      [self release];
      return nil;
    }
  }
  return self;
}
- (void)dealloc {
  [condition release];
  [listener release];
  [super dealloc];
}
@end

@interface OwnedFenceCompletion : NSObject {
@public
  OwnedSharedEventListener *owner;
  uint32_t remaining;
  void *event;
}
@end

@implementation OwnedFenceCompletion
- (void)dealloc {
  [owner release];
  [super dealloc];
}
@end

extern NTSTATUS NtSetEvent(void *handle, void *prev_state);

static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  struct unixcall_mtlsharedevent_setevent *params = obj;
  OwnedSharedEventListener *q = (OwnedSharedEventListener *)params->shared_event_listener;
  const struct WMTSharedEventValue *values = params->values.ptr;
  params->ret = 0;
  if (!q || !values || !params->count || !params->event_handle)
    return STATUS_SUCCESS;
  OwnedFenceCompletion *completion = [[OwnedFenceCompletion alloc] init];
  if (!completion)
    return STATUS_SUCCESS;
  completion->owner = [q retain];
  completion->remaining = params->wait_all ? params->count : 1;
  completion->event = (void *)params->event_handle;
  for (uint32_t i = 0; i < params->count; ++i) {
    [(id<MTLSharedEvent>)values[i].event
        notifyListener:q->listener
               atValue:values[i].value
                 block:^(id<MTLSharedEvent> _e, uint64_t _v) {
                   OwnedSharedEventListener *owner = completion->owner;
                   [owner->condition lock];
                   while (!owner->runloop && !owner->closing)
                     [owner->condition wait];
                   if (!owner->closing && completion->remaining && --completion->remaining == 0) {
                     CFRunLoopPerformBlock(owner->runloop, kCFRunLoopCommonModes, ^{
                       [owner->condition lock];
                       if (!owner->closing)
                         NtSetEvent(completion->event, NULL);
                       [owner->condition unlock];
                     });
                     CFRunLoopWakeUp(owner->runloop);
                   }
                   [owner->condition unlock];
                 }];
  }
  [completion release];
  params->ret = 1;
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  OwnedSharedEventListener *q = (OwnedSharedEventListener *)params->handle;
  if (!q)
    return STATUS_SUCCESS;
  [q->condition lock];
  if (!q->closing) {
    q->runloop = CFRunLoopGetCurrent();
    CFRunLoopSourceContext source_context = {0};
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(NULL, 0, &source_context);
    CFRunLoopAddSource(q->runloop, source, kCFRunLoopCommonModes);
    [q->condition broadcast];
    [q->condition unlock];
    CFRunLoopRun();
    [q->condition lock];
    CFRunLoopRemoveSource(q->runloop, source, kCFRunLoopCommonModes);
    CFRelease(source);
    q->runloop = NULL;
  }
  [q->condition broadcast];
  [q->condition unlock];
  // create() reserves this reference before the Wine thread is scheduled.
  [q release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  OwnedSharedEventListener *q = [[OwnedSharedEventListener alloc] init];
  [q retain]; // The start() reference, also when destroy() runs before start().
  params->ret = (obj_handle_t)q;
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  OwnedSharedEventListener *q = (OwnedSharedEventListener *)params->handle;
  if (q) {
    [q->condition lock];
    q->closing = YES;
    [q->condition broadcast];
    if (q->runloop) {
      // Stop *inside* Run, even if destroy races between publishing the loop
      // and entering CFRunLoopRun. CFRunLoopStop before Run loses that wakeup.
      CFRunLoopPerformBlock(q->runloop, kCFRunLoopCommonModes, ^{
        CFRunLoopStop(CFRunLoopGetCurrent());
      });
      CFRunLoopWakeUp(q->runloop);
    }
    [q->condition unlock];
    [q release];
  }
  return STATUS_SUCCESS;
}

#else
static NTSTATUS
_MTLSharedEvent_setWin32EventAtValue(void *obj) {
  // nop
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_start(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_create(void *obj) {
  return STATUS_SUCCESS;
}

static NTSTATUS
_SharedEventListener_destroy(void *obj) {
  return STATUS_SUCCESS;
}

#endif

static NTSTATUS
_MTLDevice_newFence(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newFence];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newEvent(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newEvent];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_updateContents(void *obj) {
  struct unixcall_mtlbuffer_updatecontents *params = obj;
  memcpy((void *)((char *)[(id<MTLBuffer>)params->buffer contents] + params->offset), params->data.ptr, params->length);
  if ([(id<MTLBuffer>)params->buffer storageMode] == MTLStorageModeManaged)
    [(id<MTLBuffer>)params->buffer didModifyRange:NSMakeRange(params->offset, params->length)];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTGetOSVersion(void *obj) {
  struct unixcall_get_os_version *params = obj;
  NSOperatingSystemVersion version = [NSProcessInfo processInfo].operatingSystemVersion;
  params->ret_major = version.majorVersion;
  params->ret_minor = version.minorVersion;
  params->ret_patch = version.patchVersion;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newBinaryArchive(void *obj) {
  struct unixcall_mtldevice_newbinaryarchive *params = obj;
  NSString *path_str = NULL;
  NSURL *url = NULL;
  MTLBinaryArchiveDescriptor *desc = [[MTLBinaryArchiveDescriptor alloc] init];
  if (params->url.ptr != NULL) {
    path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
    url = [[NSURL alloc] initFileURLWithPath:path_str];
    desc.url = url;
  }
  NSError *err = NULL;
  params->ret_archive = (obj_handle_t)[(id<MTLDevice>)params->device newBinaryArchiveWithDescriptor:desc error:&err];
  params->ret_error = (obj_handle_t)err;
  [desc release];
  if (url)
    [url release];
  if (path_str)
    [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBinaryArchive_serialize(void *obj) {
  struct unixcall_mtlbinaryarchive_serialize *params = obj;
  NSString *path_str = [[NSString alloc] initWithCString:params->url.ptr encoding:NSUTF8StringEncoding];
  NSURL *url = [[NSURL alloc] initFileURLWithPath:path_str];
  NSError *err = NULL;
  [(id<MTLBinaryArchive>)params->archive serializeToURL:url error:&err];
  params->ret_error = (obj_handle_t)err;
  [url release];
  [path_str release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_DispatchData_alloc_init(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *params = obj;
  params->ret = (obj_handle_t)dispatch_data_create((void *)params->handle, params->arg, NULL, NULL);
  return STATUS_SUCCESS;
}

@interface MTLSharedTextureHandle ()

- (MTLSharedTextureHandle *)initWithMachPort:(mach_port_t)port;
- (mach_port_t)createMachPort;

@end

static NTSTATUS
_MTLDevice_newSharedTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;

  if (info->mach_port) {
    MTLSharedTextureHandle *handle = [[MTLSharedTextureHandle alloc] initWithMachPort:info->mach_port];
    id<MTLTexture> ret = [device newSharedTextureWithHandle:handle];
    extract_texture_descriptor(ret, info);
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    [handle release];
  } else {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    id<MTLTexture> ret = [device newSharedTextureWithDescriptor:desc];
    MTLSharedTextureHandle *handle = [ret newSharedTextureHandle];
    params->ret = (obj_handle_t)ret;
    info->gpu_resource_id = [ret gpuResourceID]._impl;
    info->mach_port = [handle createMachPort]; // implicitly add ref to underlying IOSurface
    [handle release];
    [desc release];
  }

  return STATUS_SUCCESS;
}

/* Private API to register a mach port with the bootstrap server */
extern kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp, int flags);

static NTSTATUS
_WMTBootstrapRegister(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = params->mach_port;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_register2(bp, params->name, rp, 0) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  return ret;
}

static NTSTATUS
_WMTBootstrapLookUp(void *obj) {
  struct unixcall_bootstrap *params = obj;
  mach_port_t rp = 0;
  mach_port_t bp;

  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  NTSTATUS ret = bootstrap_look_up(bp, params->name, &rp) != KERN_SUCCESS ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
  mach_port_deallocate(mach_task_self(), bp);
  params->mach_port = rp;
  return ret;
}

@protocol MTLDeviceSPI <MTLDevice>

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;

@end

@interface MTLSharedEventHandle ()

- (mach_port_t)eventPort;

@end

static NTSTATUS
_MTLSharedEvent_createMachPort(void *obj) {
  struct unixcall_mtlsharedevent_createmachport *params = obj;
  id<MTLSharedEvent> event = (id<MTLSharedEvent>)params->event;
  MTLSharedEventHandle *handle = [event newSharedEventHandle];
  mach_port_t port = [handle eventPort];
  
  // The eventPort method returns a send right that's owned by the handle.
  // We need to add our own send right since we're keeping the port but releasing the handle.
  // This increments the send right count so the port remains valid.
  mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1);
  
  params->ret_mach_port = port;
  [handle release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSharedEventWithMachPort(void *obj) {
  struct unixcall_mtldevice_newsharedeventwithmachport *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  id<MTLDeviceSPI> deviceSPI = (id<MTLDeviceSPI>)device;
  params->ret_event = (obj_handle_t)[deviceSPI newSharedEventWithMachPort:params->mach_port];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_registryID(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle registryID];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLSharedEvent_waitUntilSignaledValue(void *obj) {
  struct unixcall_mtlsharedevent_waituntilsignaledvalue *params = obj;
  bool timeout = [(id<MTLSharedEvent>)params->event waitUntilSignaledValue:params->value timeoutMS:params->timeout_ms];
  params->ret_timeout = timeout;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_newTimestampBuffer(void *obj) {
  struct unixcall_mtlcountersamplebuffer_newtimestampbuffer *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;

  MTLCounterSampleBufferDescriptor *desc = [[MTLCounterSampleBufferDescriptor alloc] init];
  NSArray<id<MTLCounterSet>> *counter_sets = [device counterSets];
  id<MTLCounterSet> timestamp_counter_set = nil;
  for (id<MTLCounterSet> counterSet in counter_sets) {
    if ([counterSet.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestamp_counter_set = counterSet;
      break;
    }
  }
  desc.counterSet = timestamp_counter_set;
  desc.sampleCount = params->sample_count;
  desc.storageMode = params->shared ? MTLStorageModeShared : MTLStorageModePrivate;

  NSError *error = nil;
  params->ret = (obj_handle_t)[device newCounterSampleBufferWithDescriptor:desc error:&error];

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCounterSampleBuffer_resolveCounterRange(void *obj) {
  struct unixcall_mtlcountersamplebuffer_resolvecounterrange *params = obj;
  id<MTLCounterSampleBuffer> sample_buffer = (id<MTLCounterSampleBuffer>)params->sample_buffer;

  NSData *data = [sample_buffer resolveCounterRange:NSMakeRange(params->start, params->len)];
  if (data && params->data_out.ptr) {
    [data getBytes:params->data_out.ptr length:params->data_length];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(void *obj) {
  struct unixcall_mtlcommandbuffer_blitcommandencoderwithsamplebuffers *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->cmdbuf;
  struct WMTSampleBufferAttachmentInfo *attachments = params->attachments.ptr;

  MTLBlitPassDescriptor *blit_desc = [[MTLBlitPassDescriptor alloc] init];
  for (uint64_t i = 0; i < params->num_attachments; i++) {
    MTLBlitPassSampleBufferAttachmentDescriptor *desc = blit_desc.sampleBufferAttachments[i];
    desc.sampleBuffer = (id<MTLCounterSampleBuffer>)attachments[i].sample_buffer;
    desc.startOfEncoderSampleIndex = attachments[i].start_of_encoder_sample_index;
    desc.endOfEncoderSampleIndex = attachments[i].end_of_encoder_sample_index;
  }

  params->ret = (obj_handle_t)[cmdbuf blitCommandEncoderWithDescriptor:blit_desc];

  [blit_desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_property(void *obj) {
  struct unixcall_generic_obj_uint64_uint64_ret *params = obj;
  id<MTLCommandBuffer> cmdbuf = (id<MTLCommandBuffer>)params->handle;
  double ns = 1000000000;
  switch (params->arg) {
  case WMTCommandBufferPropertyKernelStartTime:
    params->ret = (uint64_t)([cmdbuf kernelStartTime] * ns);
    break;
  case WMTCommandBufferPropertyKernelEndTime:
    params->ret = (uint64_t)([cmdbuf kernelEndTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUStartTime:
    params->ret = (uint64_t)([cmdbuf GPUStartTime] * ns);
    break;
  case WMTCommandBufferPropertyGPUEndTime:
    params->ret = (uint64_t)([cmdbuf GPUEndTime] * ns);
    break;
  default:
    params->ret = 0;
    break;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newTileRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *params = obj;
  const struct WMTTileRenderPipelineInfo *info = params->info.ptr;
  MTLTileRenderPipelineDescriptor *descriptor = [[MTLTileRenderPipelineDescriptor alloc] init];

  for (unsigned i = 0; i < 8; i++) {
    descriptor.colorAttachments[i].pixelFormat = to_metal_pixel_format(info->color_formats[i]);
  }

  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_tile_buffers & (1 << i))
      descriptor.tileBuffers[i].mutability = MTLMutabilityImmutable;
  }

  descriptor.rasterSampleCount = info->raster_sample_count;
  descriptor.threadgroupSizeMatchesTileSize = info->tgsize_matches_tile_size;


  descriptor.tileFunction = (id<MTLFunction>)info->tile_function;

  MTLPipelineOption options = MTLPipelineOptionNone;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
      descriptor.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                                      count:info->num_binary_archives_for_lookup];
    options = info->fail_on_binary_archive_miss ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone;
  }
#endif
  NSError *err = NULL;
  params->ret_pso = (obj_handle_t)[(id<MTLDevice>)params->device newRenderPipelineStateWithTileDescriptor:descriptor
                                                                                                  options:options
                                                                                               reflection:nil
                                                                                                    error:&err];
  params->ret_error = (obj_handle_t)err;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15, *)) {
    if (!err && info->binary_archive_for_serialization) {
      [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
          addTileRenderPipelineFunctionsWithDescriptor:descriptor
                                                 error:&err];
    }
  }
#endif
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newResidencySet(void *obj) {
  struct unixcall_mtldevice_newresidencyset *params = obj;
  NSError *err = NULL;
  MTLResidencySetDescriptor *descriptor = [[MTLResidencySetDescriptor alloc] init];
  descriptor.initialCapacity = params->init_capacity;
  params->ret_set = (obj_handle_t)[(id<MTLDevice>)params->device newResidencySetWithDescriptor:descriptor error:&err];
  params->ret_error = (obj_handle_t)err;
  [descriptor release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_addAllocations(void *obj) {
  struct unixcall_mtlresidencyset_addallocations *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->set;
  [set addAllocations:(id<MTLAllocation> const *)params->allocations.ptr count:params->count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_removeAllocations(void *obj) {
  struct unixcall_mtlresidencyset_addallocations *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->set;
  [set removeAllocations:(id<MTLAllocation> const *)params->allocations.ptr count:params->count];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_removeAllAllocations(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->handle;
  [set removeAllAllocations];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_commit(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->handle;
  [set commit];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandQueue_addResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>)params->handle;
  id<MTLResidencySet> set = (id<MTLResidencySet>)params->arg;
  [queue addResidencySet:set];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newHeap(void *obj) {
  struct unixcall_mtldevice_newheap *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTHeapInfo const *info = params->info.ptr;
  MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
  desc.resourceOptions = (MTLResourceOptions)info->options;
  desc.sparsePageSize = (MTLSparsePageSize)info->sparse_page_size;
  desc.type = (MTLHeapType)info->type;
  desc.size = info->size;

  params->ret = (obj_handle_t)[device newHeapWithDescriptor:desc];

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_heapBufferSizeAndAlign(void *obj) {
  struct unixcall_mtldevice_heapbuffersizeandalign *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  MTLSizeAndAlign ret =
      [device heapBufferSizeAndAlignWithLength:params->length options:(MTLResourceOptions)params->options];
  params->ret_size = ret.size;
  params->ret_align = ret.align;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_heapTextureSizeAndAlign(void *obj) {
  struct unixcall_mtldevice_heaptexturesizeandalign *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);
  MTLSizeAndAlign ret = [device heapTextureSizeAndAlignWithDescriptor:desc];
  params->ret_size = ret.size;
  params->ret_align = ret.align;
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLHeap_newBuffer(void *obj) {
  struct unixcall_mtlheap_newbuffer *params = obj;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  struct WMTBufferInfo *info = params->info.ptr;
  id<MTLBuffer> buffer;
  if (~params->offset == 0)
    buffer = [heap newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options];
  else
    buffer =
        [heap newBufferWithLength:info->length options:(enum MTLResourceOptions)info->options offset:params->offset];
  info->memory.ptr = [heap storageMode] == MTLStorageModePrivate ? NULL : [buffer contents];

  params->ret = (obj_handle_t)buffer;
  info->gpu_address = [buffer gpuAddress];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLHeap_newTexture(void *obj) {
  struct unixcall_mtlheap_newtexture *params = obj;
  id<MTLHeap> heap = (id<MTLHeap>)params->heap;
  struct WMTTextureInfo *info = params->info.ptr;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);

  id<MTLTexture> ret;
  if (~params->offset == 0)
    ret = [heap newTextureWithDescriptor:desc];
  else
    ret = [heap newTextureWithDescriptor:desc offset:params->offset];
  params->ret = (obj_handle_t)ret;
  info->gpu_resource_id = [ret gpuResourceID]._impl;
  info->mach_port = 0;

  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newIndirectCommandBuffer(void *obj) {
  struct unixcall_mtldevice_newicb *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;

  struct WMTIndirectCommandBufferInfo *info = params->info.ptr;

  MTLIndirectCommandBufferDescriptor *desc = [[MTLIndirectCommandBufferDescriptor alloc] init];

  desc.commandTypes = (MTLIndirectCommandType)info->type;

  desc.maxFragmentBufferBindCount = info->max_fragment_buffer_binding;
  desc.maxKernelBufferBindCount = info->max_kernel_buffer_binding;
  desc.maxKernelThreadgroupMemoryBindCount = info->max_kernel_threadgroup_memory_binding;
  desc.maxMeshBufferBindCount = info->max_mesh_buffer_binding;
  desc.maxObjectBufferBindCount = info->max_object_buffer_binding;
  desc.maxObjectThreadgroupMemoryBindCount = info->max_object_threadgroup_memory_binding;
  desc.maxVertexBufferBindCount = info->max_vertex_buffer_binding;

  desc.inheritBuffers = info->inherit_buffers;
  desc.inheritPipelineState = info->inherit_pso;
  desc.supportDynamicAttributeStride = info->support_dynamic_attribute_stride;
  desc.supportRayTracing = info->support_ray_tracing;

#if 0 // needs macOS 26 SDK, and we don't really use it at the moment
  desc.inheritCullMode = info->inherit_cull_mode;
  desc.inheritDepthBias = info->inherit_depth_bias;
  desc.inheritDepthClipMode = info->inherit_depth_clip_mode;
  desc.inheritDepthStencilState = info->inherit_depth_stencil_state;
  desc.inheritFrontFacingWinding = info->inherit_front_facing;
  desc.inheritTriangleFillMode = info->inherit_fill_mode;
  desc.supportColorAttachmentMapping = info->support_color_attachment_mapping;
#endif

  id<MTLIndirectCommandBuffer> icb =
      [device newIndirectCommandBufferWithDescriptor:desc
                                     maxCommandCount:params->max_count
                                             options:(MTLResourceOptions)params->options];
  params->ret = (obj_handle_t)icb;
  info->gpu_resource_id = [icb gpuResourceID]._impl;
  [desc release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newLibraryWithSource(void *obj) {
  struct unixcall_mtldevice_newlibrarywithsource *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  NSString *source =
      [[NSString alloc] initWithBytes:params->source.ptr length:params->length encoding:NSASCIIStringEncoding];
  NSError *err = NULL;
  params->ret_library = (obj_handle_t)[device newLibraryWithSource:source options:nil error:&err];
  params->ret_error = (obj_handle_t)err;
  [source release];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_getBytes(void *obj) {
  struct unixcall_mtltexture_replaceregion *params = obj;
  [(id<MTLTexture>)params->texture getBytes:params->data.ptr
                                bytesPerRow:params->bytes_per_row
                              bytesPerImage:params->bytes_per_image
                                 fromRegion:MTLRegionMake3D(
                                                params->origin.x, params->origin.y, params->origin.z,
                                                params->size.width, params->size.height, params->size.depth
                                            )
                                mipmapLevel:params->level
                                      slice:params->slice];
  return STATUS_SUCCESS;
}

/*
 * Raytracing primitives (winemetal_rt.c) and acceleration-structure runtime
 * (rt_acceleration_structure.c). Unixcall codes 165..195.
 */

static NTSTATUS
_MTLDevice_newComputePipelineStateWithLinkedFunctions(void *obj) {
  struct unixcall_mtldevice_newcomputepipeline_linked *params = obj;
  params->ret = winemetal_rt_new_compute_pipeline(
      params->device, params->info.ptr, params->functions.ptr, params->count, &params->error
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputePipelineState_newVisibleFunctionTable(void *obj) {
  struct unixcall_mtlpipeline_newfunctiontable *params = obj;
  params->ret = winemetal_rt_new_visible_function_table(params->pipeline, params->count);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputePipelineState_newIntersectionFunctionTable(void *obj) {
  struct unixcall_mtlpipeline_newfunctiontable *params = obj;
  params->ret = winemetal_rt_new_intersection_function_table(params->pipeline, params->count);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputePipelineState_functionHandle(void *obj) {
  struct unixcall_mtlpipeline_functionhandle *params = obj;
  params->ret = winemetal_rt_function_handle(params->pipeline, params->function);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputePipelineState_maxTotalThreadsPerThreadgroup(void *obj) {
  struct unixcall_mtlpipeline_maxthreads *params = obj;
  params->ret = winemetal_rt_max_total_threads_per_threadgroup(params->pipeline);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLVisibleFunctionTable_setFunction(void *obj) {
  struct unixcall_mtlfunctiontable_set *params = obj;
  winemetal_rt_visible_function_table_set(params->table, params->function, params->index);
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLIntersectionFunctionTable_setFunction(void *obj) {
  struct unixcall_mtlfunctiontable_set *params = obj;
  winemetal_rt_intersection_function_table_set(params->table, params->function, params->index);
  return STATUS_SUCCESS;
}

/*
 * The intersection function table's own buffer argument table. These are the only bindings a
 * callback installed in the table can see: the compute encoder's argument table belongs to the
 * dispatch's kernel, not to the functions the traversal calls out of the table.
 */
static NTSTATUS
_MTLIntersectionFunctionTable_setBuffer(void *obj) {
  struct unixcall_mtlintersectiontable_setbuffer *params = obj;
  if (params->table)
    [(id<MTLIntersectionFunctionTable>)params->table
        setBuffer:(id<MTLBuffer>)params->buffer
           offset:(NSUInteger)params->offset
          atIndex:(NSUInteger)params->index];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLIntersectionFunctionTable_setVisibleFunctionTable(void *obj) {
  struct unixcall_mtlintersectiontable_setbuffer *params = obj;
  if (params->table)
    [(id<MTLIntersectionFunctionTable>)params->table
        setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)params->buffer
                   atBufferIndex:(NSUInteger)params->index];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASDescriptor_new(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_descriptor_new(params->args[2], (uint32_t)params->args[0], (uint32_t)params->args[1]);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASDescriptor_setUsage(void *obj) {
  struct unixcall_wmt_as *params = obj;
  d3dalloy_as_descriptor_set_usage(params->args[0], (uint32_t)params->args[1]);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASDescriptor_setTriangles(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_descriptor_set_triangles(
      params->args[0], (uint32_t)params->args[1], params->args[2], params->args[3], params->args[4],
      params->args[5], params->args[6], (uint32_t)params->args[7], params->args[8], params->args[9],
      params->args[10], (uint32_t)params->args[11], (uint32_t)params->args[12], (uint32_t)params->args[13]
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASDescriptor_setAABBs(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_descriptor_set_aabbs(
      params->args[0], (uint32_t)params->args[1], params->args[2], params->args[3], params->args[4],
      params->args[5], (uint32_t)params->args[6], (uint32_t)params->args[7]
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASDescriptor_setInstances(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_descriptor_set_instances(
      params->args[0], params->args[1], params->args[2], params->args[3]
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTDevice_getASSizes(void *obj) {
  struct unixcall_wmt_as *params = obj;
  struct d3dalloy_as_sizes sizes = d3dalloy_as_get_sizes(params->args[0], params->args[1]);
  params->ret[0] = sizes.acceleration_structure_size;
  params->ret[1] = sizes.build_scratch_size;
  params->ret[2] = sizes.refit_scratch_size;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASSidecar_new(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_sidecar_new(params->args[0], params->args[1], params->args[2]);
  return STATUS_SUCCESS;
}

#define WMT_AS_SIDECAR_HANDLE(name, call)                                                          \
  static NTSTATUS name(void *obj) {                                                                \
    struct unixcall_wmt_as *params = obj;                                                          \
    params->ret[0] = call(params->args[0]);                                                        \
    return STATUS_SUCCESS;                                                                         \
  }

WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_accelerationStructure, d3dalloy_as_sidecar_acceleration_structure)
WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_instanceBuffer, d3dalloy_as_sidecar_instance_buffer)
WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_contributionBuffer, d3dalloy_as_sidecar_contribution_buffer)
WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_resourceID, d3dalloy_as_sidecar_resource_id)
WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_contributionGPUAddress, d3dalloy_as_sidecar_contribution_gpu_address)
WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_size, d3dalloy_as_sidecar_size)
WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_instanceCount, d3dalloy_as_sidecar_instance_count)
WMT_AS_SIDECAR_HANDLE(_WMTASSidecar_generations, d3dalloy_as_sidecar_generations)

static NTSTATUS
_WMTASSidecar_allocate(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_sidecar_allocate(params->args[0], params->args[1], params->args[2], params->args[3]);
  return STATUS_SUCCESS;
}

#undef WMT_AS_SIDECAR_HANDLE

static NTSTATUS
_WMTCommandBuffer_accelerationStructureCommandEncoder(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] =
      (obj_handle_t)[(id<MTLCommandBuffer>)params->args[0] accelerationStructureCommandEncoder];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASEncoder_build(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_encode_build(
      params->args[0], params->args[1], params->args[2], params->args[3], params->args[4]
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASEncoder_refit(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_encode_refit(
      params->args[0], params->args[1], params->args[2], params->args[3], params->args[4], params->args[5]
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASEncoder_copy(void *obj) {
  struct unixcall_wmt_as *params = obj;
  params->ret[0] = d3dalloy_as_encode_copy(
      params->args[0], params->args[1], params->args[2], (uint32_t)params->args[3]
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASEncoder_compactedSize(void *obj) {
  struct unixcall_wmt_as *params = obj;
  d3dalloy_as_encode_compacted_size(params->args[0], params->args[1], params->args[2], params->args[3]);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTComputeEncoder_encodeASPrepass(void *obj) {
  _Static_assert(sizeof(struct d3dalloy_as_prepass_params) == sizeof(struct WMTASPrepassInfo), "AS prepass size ABI");
  _Static_assert(offsetof(struct d3dalloy_as_prepass_params, shadow_metadata) == 48, "AS prepass metadata ABI");
  struct unixcall_wmt_as *params = obj;
  struct d3dalloy_as_prepass_params info = {
      params->args[12], params->args[13], (uint32_t)params->args[14], (uint32_t)params->args[15],
      (uint32_t)params->args[16], (uint32_t)params->args[17], (uint32_t)params->args[18],
      (uint32_t)params->args[19], (uint32_t)params->args[20], (uint32_t)params->args[21], {0}
  };
  memcpy(info.shadow_metadata, (const void *)(uintptr_t)params->args[22], sizeof(info.shadow_metadata));
  params->ret[0] = d3dalloy_as_encode_prepass(
      params->args[0], params->args[1], params->args[2], params->args[3], params->args[4],
      params->args[5], params->args[6], params->args[7], params->args[8], params->args[9],
      params->args[10], params->args[11], &info
  );
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTASEncoder_fence(void *obj) {
  struct unixcall_wmt_as *params = obj;
  id<MTLAccelerationStructureCommandEncoder> encoder = (id)params->args[0];
  id<MTLFence> fence = (id)params->args[1];
  if (params->args[2])
    [encoder updateFence:fence];
  else
    [encoder waitForFence:fence];
  return STATUS_SUCCESS;
}



/* gpuResourceID is declared by the concrete Metal resource protocols (function tables,
 * acceleration structures, ...), not by MTLResource itself, so name the requirement here and
 * let Objective-C dispatch resolve the receiver. */
@protocol D3DALLOYGPUResourceIdentity <NSObject>
@property (readonly) MTLResourceID gpuResourceID;
@end

static NTSTATUS
_MTLResource_gpuResourceID(void *obj) {
  struct unixcall_wmt_resource_id *params = obj;
  params->ret = [(id<D3DALLOYGPUResourceIdentity>)params->resource gpuResourceID]._impl;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLAllocation_allocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLAllocation>)params->handle allocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLResidencySet_allocatedSize(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLResidencySet>)params->handle allocatedSize];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sampleTimestamps(void *obj) {
  struct unixcall_mtldevice_sampletimestamps *params = obj;
  [(id<MTLDevice>)params->device sampleTimestamps:&params->cpu_timestamp gpuTimestamp:&params->gpu_timestamp];
  return STATUS_SUCCESS;
}

/* Callback blocks own their observer even if a drawable outlives the swapchain.
 * The Wine worker blocks in wait(); no callback invokes PE code. */
struct WMTPresentationActive {
  uint64_t present_id;
  uint64_t drawable; // Borrowed identity only; no observer-to-drawable ownership.
};

@interface OwnedPresentationObserver : NSObject {
@public
  NSCondition *condition;
  BOOL closing;
  BOOL failed;
  struct WMTPresentationActive *active;
  size_t active_count;
  size_t active_capacity;
  struct WMTPresentationEvent *events;
  size_t event_head;
  size_t event_count;
  size_t event_capacity;
  CVDisplayLinkRef display_link;
  uint32_t display_id;
  uint64_t refresh_sequence;
  uint64_t refresh_generation;
  uint64_t output_host_ticks;
  uint64_t host_frequency;
  int64_t previous_video_time;
  int64_t video_refresh_period;
  int32_t video_time_scale;
  double rate_scalar;
  uint64_t cv_flags;
  BOOL has_video_anchor;
}
@end

@implementation OwnedPresentationObserver
- (id)init {
  if ((self = [super init])) {
    condition = [[NSCondition alloc] init];
    active_capacity = 16;
    event_capacity = 16;
    active = malloc(active_capacity * sizeof(*active));
    events = malloc(event_capacity * sizeof(*events));
    if (!condition || !active || !events) {
      [self release];
      return nil;
    }
  }
  return self;
}
- (void)dealloc {
  [condition release];
  free(active);
  free(events);
  [super dealloc];
}
@end

static CVReturn
presentation_refresh_callback(CVDisplayLinkRef link, const CVTimeStamp *now,
                              const CVTimeStamp *output, CVOptionFlags in_flags,
                              CVOptionFlags *out_flags, void *context) {
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)context;
  [owner->condition lock];
  if (!owner->closing && owner->display_link == link) {
    const uint64_t required = kCVTimeStampVideoTimeValid | kCVTimeStampHostTimeValid |
        kCVTimeStampVideoRefreshPeriodValid | kCVTimeStampRateScalarValid;
    const BOOL valid = (output->flags & required) == required && output->hostTime &&
        output->videoRefreshPeriod > 0 && output->videoTimeScale > 0 &&
        output->rateScalar > 0;
    if (valid) {
      if (owner->has_video_anchor) {
        const int64_t elapsed = output->videoTime - owner->previous_video_time;
        if (owner->video_refresh_period == output->videoRefreshPeriod &&
            owner->video_time_scale == output->videoTimeScale && elapsed > 0 &&
            elapsed % output->videoRefreshPeriod == 0) {
          owner->refresh_sequence += (uint64_t)(elapsed / output->videoRefreshPeriod);
          if (elapsed > output->videoRefreshPeriod)
            ++owner->refresh_generation;
        } else {
          ++owner->refresh_generation;
          ++owner->refresh_sequence;
        }
      } else {
        ++owner->refresh_sequence;
      }
      owner->previous_video_time = output->videoTime;
      owner->output_host_ticks = output->hostTime;
      owner->video_refresh_period = output->videoRefreshPeriod;
      owner->video_time_scale = output->videoTimeScale;
      owner->rate_scalar = output->rateScalar;
      owner->has_video_anchor = YES;
    } else {
      if (owner->has_video_anchor)
        ++owner->refresh_generation;
      owner->has_video_anchor = NO;
    }
    owner->cv_flags = output->flags;
    [owner->condition broadcast];
  }
  [owner->condition unlock];
  return kCVReturnSuccess;
}

static NTSTATUS
_WMTPresentationObserver_create(void *obj) {
  struct unixcall_generic_obj_ret *params = obj;
  params->ret = (obj_handle_t)[[OwnedPresentationObserver alloc] init];
  return STATUS_SUCCESS;
}

static void presentation_record_event_locked(OwnedPresentationObserver *owner, uint64_t present_id,
                                             uint64_t drawable_id, double time) {
  size_t i = 0;
  while (i < owner->active_count && owner->active[i].present_id != present_id)
    ++i;
  if (i < owner->active_count) {
    owner->active[i] = owner->active[--owner->active_count];
    if (!owner->closing && !owner->failed) {
      if (owner->event_count == owner->event_capacity) {
        const size_t capacity = owner->event_capacity * 2;
        struct WMTPresentationEvent *expanded = malloc(capacity * sizeof(*expanded));
        if (expanded) {
          for (size_t n = 0; n < owner->event_count; ++n)
            expanded[n] = owner->events[(owner->event_head + n) % owner->event_capacity];
          free(owner->events);
          owner->events = expanded;
          owner->event_capacity = capacity;
          owner->event_head = 0;
        } else {
          owner->failed = YES;
        }
      }
      if (!owner->failed) {
        owner->events[(owner->event_head + owner->event_count++) % owner->event_capacity] =
            (struct WMTPresentationEvent){present_id, drawable_id, time,
                                          owner->refresh_sequence, owner->refresh_generation,
                                          time > 0 ? WMTPresentationPresented : WMTPresentationSkipped, 0,
                                          owner->has_video_anchor ? owner->output_host_ticks : 0,
                                          owner->has_video_anchor ? owner->previous_video_time : 0};
      }
      [owner->condition signal];
    }
  }
}

struct WMTCompositionPresentationProof {
  unsigned mask;
  double times[2];
  uint64_t drawable_id;
};

static uint64_t composition_observed_present_id(id observer, id drawable) {
  OwnedPresentationObserver *owner = observer;
  if (!owner) return 0;
  uint64_t present_id = 0;
  [owner->condition lock];
  for (size_t i = 0; i < owner->active_count; ++i) {
    if (owner->active[i].drawable == (uint64_t)drawable) {
      present_id = owner->active[i].present_id; break;
    }
  }
  [owner->condition unlock];
  return present_id;
}

static void composition_skip_deferred(id observer, id drawable, uint64_t present_id) {
  OwnedPresentationObserver *owner = observer;
  if (!owner || !present_id) return;
  id<CAMetalDrawable> primary, residual;
  winemetal_composition_drawables(drawable, &primary, &residual);
  [owner->condition lock];
  // This request was never published, so zero denotes a real skipped outcome.
  presentation_record_event_locked(owner, present_id, primary.drawableID, 0);
  [owner->condition unlock];
}

static void composition_record_presentation_locked(OwnedPresentationObserver *owner, uint64_t present_id,
    struct WMTCompositionPresentationProof *proof, id<MTLDrawable> presented, unsigned plane) {
  proof->mask |= 1u << plane;
  proof->times[plane] = presented.presentedTime;
  if (!plane) proof->drawable_id = presented.drawableID;
  if (proof->mask == 3) {
    const double time = proof->times[0] > 0 && proof->times[1] > 0
        ? fmax(proof->times[0], proof->times[1]) : 0;
    presentation_record_event_locked(owner, present_id, proof->drawable_id, time);
  }
}

static NTSTATUS
_MetalDrawable_observePresentation(void *obj) {
  struct unixcall_wmt_presentation_observe *params = obj;
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)params->observer;
  id<CAMetalDrawable> drawable, residual;
  winemetal_composition_drawables((id)params->drawable, &drawable, &residual);
  params->ret = 0;
  if (!owner || !drawable || !params->present_id ||
      ![drawable respondsToSelector:@selector(addPresentedHandler:)] ||
      (residual && ![residual respondsToSelector:@selector(addPresentedHandler:)]))
    return STATUS_SUCCESS;

  [owner->condition lock];
  if (!owner->closing && !owner->failed) {
    size_t i = 0;
    while (i < owner->active_count && owner->active[i].present_id != params->present_id)
      ++i;
    if (i == owner->active_count) {
      if (owner->active_count == owner->active_capacity) {
        const size_t capacity = owner->active_capacity * 2;
        void *expanded = realloc(owner->active, capacity * sizeof(*owner->active));
        if (expanded) {
          owner->active = expanded;
          owner->active_capacity = capacity;
        } else {
          owner->failed = YES;
          [owner->condition broadcast];
        }
      }
      if (!owner->failed) {
        owner->active[owner->active_count++] = (struct WMTPresentationActive){params->present_id, params->drawable};
        params->ret = 1;
      }
    }
  }
  [owner->condition unlock];
  if (!params->ret)
    return STATUS_SUCCESS;

  const uint64_t present_id = params->present_id;
  if (!residual) {
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
      [owner->condition lock];
      presentation_record_event_locked(owner, present_id, presented.drawableID, presented.presentedTime);
      [owner->condition unlock];
    }];
  } else {
    // Callbacks retain only this scalar proof and observer, never the envelope
    // or either drawable. Cancellation cannot create a drawable/block cycle.
    __block struct WMTCompositionPresentationProof proof = {0};
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
      [owner->condition lock];
      composition_record_presentation_locked(owner, present_id, &proof, presented, 0);
      [owner->condition unlock];
    }];
    [residual addPresentedHandler:^(id<MTLDrawable> presented) {
      [owner->condition lock];
      composition_record_presentation_locked(owner, present_id, &proof, presented, 1);
      [owner->condition unlock];
    }];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTPresentationObserver_wait(void *obj) {
  struct unixcall_wmt_presentation_wait *params = obj;
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)params->observer;
  params->ret = 0;
  if (!owner)
    return STATUS_SUCCESS;
  [owner->condition lock];
  for (;;) {
    if (owner->closing || owner->failed) {
      params->ret = owner->failed ? -1 : 0;
      break;
    }
    if (owner->event_count) {
      struct WMTPresentationEvent event = owner->events[owner->event_head];
      owner->event_head = (owner->event_head + 1) % owner->event_capacity;
      --owner->event_count;
      if (event.present_id) {
        params->event = event;
        params->ret = 1;
        break;
      }
      continue;
    }
    [owner->condition wait];
  }
  [owner->condition unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTPresentationObserver_cancel(void *obj) {
  struct unixcall_wmt_presentation_cancel *params = obj;
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)params->observer;
  if (!owner)
    return STATUS_SUCCESS;
  [owner->condition lock];
  for (size_t i = 0; i < owner->active_count; ++i)
    if (owner->active[i].present_id == params->present_id) {
      owner->active[i] = owner->active[--owner->active_count];
      break;
    }
  for (size_t i = 0; i < owner->event_count; ++i) {
    struct WMTPresentationEvent *event = &owner->events[(owner->event_head + i) % owner->event_capacity];
    if (event->present_id == params->present_id)
      event->present_id = 0;
  }
  [owner->condition unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLCommandBuffer_presentDrawableIfCurrent(void *obj) {
  struct unixcall_mtlcommandbuffer_present_if_current *params = obj;
  id<MTLDrawable> drawable = (id<MTLDrawable>)params->drawable;
  id<MTLSharedEvent> token = (id<MTLSharedEvent>)params->token;
  OwnedPresentationObserver *observer = (OwnedPresentationObserver *)params->observer;
  const uint64_t epoch = params->epoch;
  const double after = params->after;
  const uint64_t present_id = params->present_id;
  // Diagnostic record of the native decision: whether this drawable was ever handed to Core
  // Animation. present_id is the swapchain's DXGI present count.
  static int trace_decision = -1;
  if (trace_decision < 0) {
    const char *trace = getenv("OWNED_GRAPHICS_TRACE");
    trace_decision = trace && trace[0] == '1' && !trace[1];
  }
  const int trace = trace_decision;
  // Hand the drawable to Core Animation only once the GPU finished rendering it, and only if no
  // DXGI_PRESENT_RESTART advanced the token meanwhile. A discarded drawable is released
  // unpresented with the block, and its presentation observation is withdrawn so it can never
  // report a presented time. Block_copy retains the captured drawable, token, and observer (MRC).
  [(id<MTLCommandBuffer>)params->cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> cmdbuf) {
    const char *reason = "current";
    if (token.signaledValue != epoch) {
      if (observer && present_id) {
        struct unixcall_wmt_presentation_cancel cancel = {(obj_handle_t)observer, present_id};
        _WMTPresentationObserver_cancel(&cancel);
      }
      reason = "epoch";
    } else if (cmdbuf.status != MTLCommandBufferStatusCompleted) {
      reason = "status";
    } else if (after > 0) {
      [drawable presentAfterMinimumDuration:after];
    } else {
      [drawable present];
    }
    if (trace)
      fprintf(stderr, "OWNED_GRAPHICS_NATIVE_PRESENT_DECISION present_id=%llu presented=%d reason=%s\n",
              (unsigned long long)present_id, reason[0] == 'c', reason);
  }];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTPresentationObserver_close(void *obj) {
  struct unixcall_generic_obj_noret *params = obj;
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)params->handle;
  if (owner) {
    [owner->condition lock];
    owner->closing = YES;
    owner->active_count = 0;
    CVDisplayLinkRef link = owner->display_link;
    owner->display_link = NULL;
    [owner->condition broadcast];
    [owner->condition unlock];
    if (link) {
      CVDisplayLinkStop(link);
      CVDisplayLinkRelease(link);
      [owner release]; // The link's callback-context reference.
    }
  }
  return STATUS_SUCCESS;
}

static BOOL
presentation_observer_set_display(OwnedPresentationObserver *owner, CGDirectDisplayID display_id) {
  if (!owner || !CGDisplayIsActive(display_id))
    return NO;
  [owner->condition lock];
  BOOL same_display = !owner->closing && owner->display_link && owner->display_id == display_id;
  [owner->condition unlock];
  if (same_display)
    return YES;

  CVDisplayLinkRef new_link = NULL;
  if (CVDisplayLinkCreateWithCGDisplay(display_id, &new_link) != kCVReturnSuccess || !new_link)
    return NO;
  [owner retain]; // Keep the callback context alive until Stop returns.
  CVReturn status = CVDisplayLinkSetOutputCallback(new_link, presentation_refresh_callback, owner);
  if (status == kCVReturnSuccess)
    status = CVDisplayLinkStart(new_link);
  if (status != kCVReturnSuccess) {
    CVDisplayLinkRelease(new_link);
    [owner release];
    return NO;
  }

  [owner->condition lock];
  if (owner->closing) {
    [owner->condition unlock];
    CVDisplayLinkStop(new_link);
    CVDisplayLinkRelease(new_link);
    [owner release];
    return NO;
  }
  CVDisplayLinkRef old_link = owner->display_link;
  owner->display_link = new_link;
  owner->display_id = display_id;
  owner->refresh_sequence = 0;
  ++owner->refresh_generation;
  owner->output_host_ticks = 0;
  owner->host_frequency = CVGetHostClockFrequency();
  owner->previous_video_time = 0;
  owner->video_refresh_period = 0;
  owner->video_time_scale = 0;
  owner->rate_scalar = 0;
  owner->cv_flags = 0;
  owner->has_video_anchor = NO;
  [owner->condition unlock];
  if (old_link) {
    CVDisplayLinkStop(old_link);
    CVDisplayLinkRelease(old_link);
    [owner release];
  }
  return YES;
}

static void composition_bind_observer(id observer, NSView *view) {
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)observer;
  if (!owner) return;
  assert([NSThread isMainThread]);
  [owner->condition lock];
  BOOL closing = owner->closing;
  [owner->condition unlock];
  if (closing) return;
  NSScreen *screen = view.window.screen;
  CGDirectDisplayID display_id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
  if (screen && display_id != kCGNullDirectDisplay && CGDisplayIsActive(display_id)) {
    presentation_observer_set_display(owner, display_id);
    return;
  }
  [owner->condition lock];
  CVDisplayLinkRef link = owner->display_link;
  owner->display_link = NULL;
  owner->display_id = kCGNullDirectDisplay;
  owner->has_video_anchor = NO;
  if (link) ++owner->refresh_generation;
  [owner->condition unlock];
  if (link) {
    CVDisplayLinkStop(link);
    CVDisplayLinkRelease(link);
    [owner release];
  }
}

static double composition_refresh_period(id observer) {
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)observer;
  if (!owner) return 0;
  double period = 0;
  [owner->condition lock];
  if (!owner->closing && !owner->failed && owner->display_link && CGDisplayIsActive(owner->display_id)) {
    if (owner->has_video_anchor && owner->video_refresh_period > 0 && owner->video_time_scale > 0 &&
        isfinite(owner->rate_scalar) && owner->rate_scalar > 0)
      period = (double)owner->video_refresh_period / owner->video_time_scale / owner->rate_scalar;
    else {
      // This is the actual bound device's nominal period, not a fabricated
      // timestamp, VBlank sequence, or observer refresh anchor.
      const CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(owner->display_link);
      if (!(nominal.flags & kCVTimeIsIndefinite) && nominal.timeValue > 0 && nominal.timeScale > 0)
        period = (double)nominal.timeValue / nominal.timeScale;
    }
  }
  [owner->condition unlock];
  return isfinite(period) && period > 0 ? period : 0;
}

static void composition_fail_observer(id observer) {
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)observer;
  if (!owner) return;
  [owner->condition lock];
  owner->failed = YES;
  [owner->condition broadcast];
  [owner->condition unlock];
}

static NTSTATUS
_WMTPresentationObserver_setView(void *obj) {
  struct unixcall_wmt_observer_view *params = obj;
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)params->observer;
  NSView *view = (NSView *)params->view;
  params->ret = 0;
  if (!owner || !view)
    return STATUS_SUCCESS;
  __block CGDirectDisplayID display_id = kCGNullDirectDisplay;
  execute_on_main(^{
    if ([view isKindOfClass:NSView.class]) {
      NSScreen *screen = view.window.screen;
      display_id = [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
    }
  });
  if (display_id != kCGNullDirectDisplay)
    params->ret = presentation_observer_set_display(owner, display_id);
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTMonitorTiming_call(void *obj) {
  struct WMTMonitorTimingCall *params = obj;
  params->result = 0;
  if (params->operation == WMTMonitorTimingCreate || params->operation == WMTMonitorTimingResolveDisplay) {
    params->object = 0;
    params->display_id = 0;
    CGDirectDisplayID selected = kCGNullDirectDisplay;
    const BOOL resolving = params->operation == WMTMonitorTimingResolveDisplay;
    uint32_t count = 0, matches = 0;
    if (!resolving && params->primary) {
      selected = CGMainDisplayID(); // Preserve the existing timing-Create contract.
    } else {
      if (CGGetActiveDisplayList(0, NULL, &count) != kCGErrorSuccess || !count)
        return STATUS_SUCCESS;
      CGDirectDisplayID displays[count];
      if (CGGetActiveDisplayList(count, displays, &count) != kCGErrorSuccess)
        return STATUS_SUCCESS;
      for (uint32_t i = 0; i < count; ++i) {
        CGRect bounds = CGDisplayBounds(displays[i]);
        if (!resolving) {
          if (bounds.origin.x == params->left && bounds.origin.y == params->top &&
              CGRectGetMaxX(bounds) == params->right && CGRectGetMaxY(bounds) == params->bottom) {
            selected = displays[i];
            break;
          }
          continue;
        }
        if ((CGDisplayIsMain(displays[i]) != 0) != (params->primary != 0)) continue;
        // Wine's cgrect_win_from_mac has exactly two modes: native points or
        // every coordinate doubled. Match the entire rectangle and require a
        // unique (display, mode), never select a primary/secondary approximation.
        // ponytail: ambiguous multi-display layouts fail closed; explicit Wine
        // coordinate-mode identity is the upgrade if those layouts need support.
        for (unsigned scale = 1; scale <= 2; ++scale) {
          if (bounds.origin.x * scale == params->left && bounds.origin.y * scale == params->top &&
              CGRectGetMaxX(bounds) * scale == params->right &&
              CGRectGetMaxY(bounds) * scale == params->bottom) {
            selected = displays[i];
            ++matches;
          }
        }
      }
    }
    if ((resolving && matches != 1) || selected == kCGNullDirectDisplay)
      return STATUS_SUCCESS;
    if (params->operation == WMTMonitorTimingResolveDisplay) {
      if (!CGDisplayIsActive(selected)) return STATUS_SUCCESS;
      params->display_id = selected;
      params->result = 1;
      return STATUS_SUCCESS;
    }
    OwnedPresentationObserver *owner = [[OwnedPresentationObserver alloc] init];
    if (presentation_observer_set_display(owner, selected)) {
      params->object = (obj_handle_t)owner;
      params->result = 1;
    } else {
      [owner release];
    }
  } else if (params->operation == WMTMonitorTimingWait) {
    OwnedPresentationObserver *owner = (id)params->object;
    if (!owner)
      return STATUS_SUCCESS;
    [owner->condition lock];
    const uint64_t sequence = owner->refresh_sequence;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:1.0];
    while (!owner->closing && CGDisplayIsActive(owner->display_id) &&
           (!owner->has_video_anchor || owner->refresh_sequence == sequence)) {
      if (![owner->condition waitUntilDate:deadline])
        break;
    }
    if (!owner->closing && CGDisplayIsActive(owner->display_id) && owner->has_video_anchor &&
        owner->refresh_sequence != sequence) {
      /* CoreVideo calls ahead of scan-out: do not return at callback delivery. */
      const uint64_t target = owner->output_host_ticks;
      while (!owner->closing && CVGetCurrentHostTime() < target) {
        const uint64_t now = CVGetCurrentHostTime();
        if (now >= target)
          break;
        double remaining = (double)(target - now) / owner->host_frequency;
        if (remaining > 0 && remaining < 1.0)
          [owner->condition waitUntilDate:[NSDate dateWithTimeIntervalSinceNow:remaining]];
        else
          break;
      }
      params->result = !owner->closing && CGDisplayIsActive(owner->display_id) &&
          CVGetCurrentHostTime() >= target;
    }
    [owner->condition unlock];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTPresentationObserver_getRefreshSample(void *obj) {
  struct unixcall_wmt_refresh_sample *params = obj;
  OwnedPresentationObserver *owner = (OwnedPresentationObserver *)params->observer;
  params->ret = 0;
  if (!owner)
    return STATUS_SUCCESS;
  [owner->condition lock];
  if (!owner->closing && owner->has_video_anchor && CGDisplayIsActive(owner->display_id)) {
    params->sample = (struct WMTDisplayRefreshSample){owner->refresh_sequence, owner->refresh_generation,
        owner->output_host_ticks, owner->host_frequency, owner->previous_video_time,
        owner->video_refresh_period, owner->rate_scalar, owner->cv_flags,
        owner->display_id, owner->video_time_scale};
    params->ret = 1;
  }
  [owner->condition unlock];
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTClockSampleHostSeconds(void *obj) {
  struct unixcall_wmt_clock_sample *params = obj;
  params->host_seconds = CACurrentMediaTime();
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputeCommandEncoder_setVisibleFunctionTable(void *obj) {
  struct unixcall_mtlcomputeencoder_functiontable *params = obj;
  [(id<MTLComputeCommandEncoder>)params->encoder
      setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)params->table
                              atBufferIndex:params->index];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLComputeCommandEncoder_setIntersectionFunctionTable(void *obj) {
  struct unixcall_mtlcomputeencoder_functiontable *params = obj;
  [(id<MTLComputeCommandEncoder>)params->encoder
      setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)params->table
                                   atBufferIndex:params->index];
  return STATUS_SUCCESS;
}

/*
 * Definition from cache.c
 */

NTSTATUS _CacheReader_alloc_init(void *obj);
NTSTATUS _CacheReader_get(void *obj);
NTSTATUS _CacheWriter_alloc_init(void *obj);
NTSTATUS _CacheWriter_set(void *obj);
NTSTATUS _WMTSetMetalShaderCachePath(void *obj);

static NTSTATUS
_WMTGetPipelineCacheIdentity(void *obj) {
  struct unixcall_get_pipeline_cache_identity *params = obj;
  memset(&params->identity, 0, sizeof(params->identity));
  params->ret_success = 0;
  Dl_info image;
  if (!dladdr((const void *)&_WMTGetPipelineCacheIdentity, &image))
    return STATUS_SUCCESS;
  const struct mach_header_64 *header = image.dli_fbase;
  if (header->magic != MH_MAGIC_64)
    return STATUS_SUCCESS;
  const uint8_t *cursor = (const uint8_t *)(header + 1);
  const uint8_t *end = cursor + header->sizeofcmds;
  bool found_uuid = false;
  for (uint32_t i = 0; i < header->ncmds; ++i) {
    if ((size_t)(end - cursor) < sizeof(struct load_command))
      return STATUS_SUCCESS;
    const struct load_command *command = (const struct load_command *)cursor;
    if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor))
      return STATUS_SUCCESS;
    if (command->cmd == LC_UUID && command->cmdsize >= sizeof(struct uuid_command)) {
      memcpy(params->identity.compiler_uuid, ((const struct uuid_command *)command)->uuid, 16);
      found_uuid = true;
      break;
    }
    cursor += command->cmdsize;
  }
  size_t size = sizeof(params->identity.driver_build);
  if (found_uuid && sysctlbyname("kern.osversion", params->identity.driver_build, &size, NULL, 0) == 0 &&
      size > 1 && size <= sizeof(params->identity.driver_build))
    params->ret_success = 1;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_maxBufferLength(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  params->ret = [(id<MTLDevice>)params->handle maxBufferLength];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_supportsPlacementSparse(void *obj) {
  struct unixcall_generic_obj_uint64_ret *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->handle;
  params->ret = 0;
  if (@available(macOS 26.4, *))
    params->ret = [device supportsPlacementSparse] ? 1 : 0;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newPlacementHeap(void *obj) {
  struct unixcall_mtldevice_newplacementheap *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  const struct WMTHeapInfo *info = params->info.ptr;
  params->ret = 0;
  if (@available(macOS 26.0, *)) {
    MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
    desc.resourceOptions = (MTLResourceOptions)info->options;
    desc.type = MTLHeapTypePlacement;
    desc.size = info->size;
    desc.maxCompatiblePlacementSparsePageSize = (MTLSparsePageSize)params->max_sparse_page_size;
    params->ret = (obj_handle_t)[device newHeapWithDescriptor:desc];
    [desc release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newPlacementSparseBuffer(void *obj) {
  struct unixcall_mtldevice_newplacementsparseresource *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTBufferInfo *info = params->info.ptr;
  params->ret = 0;
  info->memory.ptr = NULL;
  info->gpu_address = 0;
  if (@available(macOS 26.0, *)) {
    /* Placement sparse buffers only exist in private storage. */
    MTLResourceOptions options = ((MTLResourceOptions)info->options & ~MTLResourceStorageModeMask) |
                                 MTLResourceStorageModePrivate;
    if (info->length > [device maxBufferLength])
      return STATUS_SUCCESS;
    id<MTLBuffer> buffer = [device newBufferWithLength:info->length
                                               options:options
                               placementSparsePageSize:(MTLSparsePageSize)params->page_size];
    params->ret = (obj_handle_t)buffer;
    if (buffer)
      info->gpu_address = [buffer gpuAddress];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newPlacementSparseTexture(void *obj) {
  struct unixcall_mtldevice_newplacementsparseresource *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTTextureInfo *info = params->info.ptr;
  params->ret = 0;
  info->gpu_resource_id = 0;
  info->mach_port = 0;
  if (@available(macOS 26.0, *)) {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    desc.storageMode = MTLStorageModePrivate;
    desc.placementSparsePageSize = (MTLSparsePageSize)params->page_size;
    id<MTLTexture> ret = [device newTextureWithDescriptor:desc];
    params->ret = (obj_handle_t)ret;
    if (ret)
      info->gpu_resource_id = [ret gpuResourceID]._impl;
    [desc release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLBuffer_newPlacementSparseTextureView(void *obj) {
  struct unixcall_mtlbuffer_newplacementsparsetextureview *params = obj;
  id<MTLBuffer> buffer = (id<MTLBuffer>)params->buffer;
  struct WMTTextureInfo *info = params->info.ptr;
  params->ret = 0;
  info->gpu_resource_id = 0;
  info->mach_port = 0;
  if (@available(macOS 26.0, *)) {
    MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
    fill_texture_descriptor(desc, info);
    desc.storageMode = MTLStorageModePrivate;
    desc.placementSparsePageSize = (MTLSparsePageSize)params->page_size;
    id<MTLTexture> ret = [buffer newTextureWithDescriptor:desc offset:params->offset bytesPerRow:params->bytes_per_row];
    params->ret = (obj_handle_t)ret;
    if (ret)
      info->gpu_resource_id = [ret gpuResourceID]._impl;
    [desc release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_sparseTileSize(void *obj) {
  struct unixcall_mtldevice_sparsetilesize *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  MTLSize size = [device sparseTileSizeWithTextureType:(MTLTextureType)params->type
                                           pixelFormat:to_metal_pixel_format(params->format)
                                           sampleCount:params->sample_count
                                        sparsePageSize:(MTLSparsePageSize)params->page_size];
  params->ret.width = size.width;
  params->ret.height = size.height;
  params->ret.depth = size.depth;
  params->ret.reserved = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLTexture_sparseTail(void *obj) {
  struct unixcall_mtltexture_sparsetail *params = obj;
  id<MTLTexture> texture = (id<MTLTexture>)params->texture;
  params->ret.first_mipmap_in_tail = [texture firstMipmapInTail];
  params->ret.tail_size_in_bytes = [texture tailSizeInBytes];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newMTL4CommandQueue(void *obj) {
  struct unixcall_generic_obj_obj_ret *params = obj;
  params->ret = 0;
  if (@available(macOS 26.0, *))
    params->ret = (obj_handle_t)[(id<MTLDevice>)params->handle newMTL4CommandQueue];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_addResidencySet(void *obj) {
  struct unixcall_generic_obj_obj_noret *params = obj;
  if (@available(macOS 26.0, *))
    [(id<MTL4CommandQueue>)params->handle addResidencySet:(id<MTLResidencySet>)params->arg];
  return STATUS_SUCCESS;
}

#define WMT_SPARSE_MAPPING_BATCH 64

static NTSTATUS
_MTL4CommandQueue_updateBufferMappings(void *obj) {
  struct unixcall_mtl4commandqueue_updatemappings *params = obj;
  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)params->queue;
    const struct WMTSparseBufferMapping *mappings = params->mappings.ptr;
    MTL4UpdateSparseBufferMappingOperation operations[WMT_SPARSE_MAPPING_BATCH];
    for (uint64_t done = 0; done < params->count;) {
      NSUInteger count = 0;
      for (; count < WMT_SPARSE_MAPPING_BATCH && done + count < params->count; count++) {
        const struct WMTSparseBufferMapping *mapping = &mappings[done + count];
        operations[count].mode = (MTLSparseTextureMappingMode)mapping->mode;
        operations[count].bufferRange = NSMakeRange(mapping->first_tile, mapping->tile_count);
        operations[count].heapOffset = mapping->heap_offset;
      }
      [queue updateBufferMappings:(id<MTLBuffer>)params->resource
                             heap:(id<MTLHeap>)params->heap
                       operations:operations
                            count:count];
      done += count;
    }
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_updateTextureMappings(void *obj) {
  struct unixcall_mtl4commandqueue_updatemappings *params = obj;
  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = (id<MTL4CommandQueue>)params->queue;
    const struct WMTSparseTextureMapping *mappings = params->mappings.ptr;
    MTL4UpdateSparseTextureMappingOperation operations[WMT_SPARSE_MAPPING_BATCH];
    for (uint64_t done = 0; done < params->count;) {
      NSUInteger count = 0;
      for (; count < WMT_SPARSE_MAPPING_BATCH && done + count < params->count; count++) {
        const struct WMTSparseTextureMapping *mapping = &mappings[done + count];
        operations[count].mode = (MTLSparseTextureMappingMode)mapping->mode;
        operations[count].textureRegion = MTLRegionMake3D(
            mapping->origin.x, mapping->origin.y, mapping->origin.z, mapping->size.width, mapping->size.height,
            mapping->size.depth
        );
        operations[count].textureLevel = mapping->level;
        operations[count].textureSlice = mapping->slice;
        operations[count].heapOffset = mapping->heap_offset;
      }
      [queue updateTextureMappings:(id<MTLTexture>)params->resource
                              heap:(id<MTLHeap>)params->heap
                        operations:operations
                             count:count];
      done += count;
    }
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTL4CommandQueue_signalEvent(void *obj) {
  struct unixcall_mtl4commandqueue_signalevent *params = obj;
  if (@available(macOS 26.0, *))
    [(id<MTL4CommandQueue>)params->queue signalEvent:(id<MTLEvent>)params->event value:params->value];
  return STATUS_SUCCESS;
}

static NTSTATUS
_MTLDevice_newSamplerStateWithReduction(void *obj) {
  struct unixcall_mtldevice_newsamplerstatewithreduction *params = obj;
  id<MTLDevice> device = (id<MTLDevice>)params->device;
  struct WMTSamplerInfo *info = params->info.ptr;
  params->ret = 0;
  info->gpu_resource_id = 0;
  if (@available(macOS 26.0, *)) {
    MTLSamplerDescriptor *sampler_desc = new_sampler_descriptor(info);
    sampler_desc.reductionMode = (MTLSamplerReductionMode)params->reduction;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];
    info->gpu_resource_id = sampler && info->support_argument_buffers ? [sampler gpuResourceID]._impl : 0;
    params->ret = (obj_handle_t)sampler;
    [sampler_desc release];
  }
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTUnimplemented(void *obj) {
  return STATUS_NOT_IMPLEMENTED;
}

/*
 * D3D12 shared-handle rendezvous (232..237).
 *
 * A Metal object port (IOSurface texture port or MTLSharedEvent port) cannot be
 * registered with bootstrap directly, because bootstrap_register2(name, NULL)
 * does not remove a registration: only the death of the registered receive
 * right does. Each process that holds a shared object therefore owns one
 * receive right R registered as d3dalloy.share.<pid>.<128-bit random>. A lookup
 * sends a request to R; the dispatch source replies with a copied send right of
 * the object port. Destroying the rendezvous destroys R, which unregisters the
 * name immediately.
 */
struct share_rendezvous {
  dispatch_source_t source;
  mach_port_t recv;
  mach_port_t object;
};

struct share_reply_msg {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_port_descriptor_t port;
};

struct share_request_msg {
  mach_msg_header_t header;
  mach_msg_trailer_t trailer;
};

static NTSTATUS
_WMTShareRendezvousCreate(void *obj) {
  struct unixcall_share_rendezvous *params = obj;
  params->handle = 0;
  params->name[0] = '\0';
  if (!MACH_PORT_VALID(params->port))
    return STATUS_UNSUCCESSFUL;
  struct share_rendezvous *rv = calloc(1, sizeof(*rv));
  if (!rv)
    return STATUS_UNSUCCESSFUL;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &rv->recv) != KERN_SUCCESS) {
    free(rv);
    return STATUS_UNSUCCESSFUL;
  }
  if (mach_port_insert_right(mach_task_self(), rv->recv, rv->recv, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS ||
      mach_port_mod_refs(mach_task_self(), params->port, MACH_PORT_RIGHT_SEND, 1) != KERN_SUCCESS) {
    mach_port_mod_refs(mach_task_self(), rv->recv, MACH_PORT_RIGHT_RECEIVE, -1);
    free(rv);
    return STATUS_UNSUCCESSFUL;
  }
  rv->object = params->port;

  uint8_t random[16];
  arc4random_buf(random, sizeof(random));
  int len = snprintf(params->name, sizeof(params->name), "d3dalloy.share.%d.", (int)getpid());
  for (unsigned i = 0; i < sizeof(random); ++i)
    len += snprintf(params->name + len, sizeof(params->name) - len, "%02x", random[i]);

  mach_port_t bp;
  kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &bp);
  if (kr == KERN_SUCCESS) {
    kr = bootstrap_register2(bp, params->name, rv->recv, 0);
    mach_port_deallocate(mach_task_self(), bp);
  }
  if (kr != KERN_SUCCESS) {
    mach_port_mod_refs(mach_task_self(), rv->recv, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), rv->recv);
    mach_port_deallocate(mach_task_self(), rv->object);
    free(rv);
    params->name[0] = '\0';
    return STATUS_UNSUCCESSFUL;
  }

  rv->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, rv->recv, 0,
                                      dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
  dispatch_source_set_event_handler(rv->source, ^{
    struct {
      struct share_request_msg msg;
      uint8_t slack[256];
    } request;
    memset(&request, 0, sizeof(request));
    if (mach_msg(&request.msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(request), rv->recv, 0,
                 MACH_PORT_NULL) != MACH_MSG_SUCCESS)
      return;
    mach_port_t reply_to = request.msg.header.msgh_remote_port;
    if (!MACH_PORT_VALID(reply_to)) {
      mach_msg_destroy(&request.msg.header);
      return;
    }
    struct share_reply_msg reply;
    memset(&reply, 0, sizeof(reply));
    reply.header.msgh_bits =
        MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.msg.header.msgh_bits), 0) | MACH_MSGH_BITS_COMPLEX;
    reply.header.msgh_size = sizeof(reply);
    reply.header.msgh_remote_port = reply_to;
    reply.header.msgh_id = request.msg.header.msgh_id + 100;
    reply.body.msgh_descriptor_count = 1;
    reply.port.name = rv->object;
    reply.port.disposition = MACH_MSG_TYPE_COPY_SEND;
    reply.port.type = MACH_MSG_PORT_DESCRIPTOR;
    if (mach_msg(&reply.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(reply), 0, MACH_PORT_NULL, 1000,
                 MACH_PORT_NULL) != MACH_MSG_SUCCESS)
      mach_port_deallocate(mach_task_self(), reply_to);
  });
  dispatch_source_set_cancel_handler(rv->source, ^{
    mach_port_mod_refs(mach_task_self(), rv->recv, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), rv->recv);
    mach_port_deallocate(mach_task_self(), rv->object);
    dispatch_release(rv->source);
    free(rv);
  });
  dispatch_resume(rv->source);
  params->handle = (uint64_t)(uintptr_t)rv;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTShareRendezvousDestroy(void *obj) {
  struct unixcall_share_rendezvous *params = obj;
  struct share_rendezvous *rv = (struct share_rendezvous *)(uintptr_t)params->handle;
  if (rv)
    dispatch_source_cancel(rv->source);
  params->handle = 0;
  return STATUS_SUCCESS;
}

static NTSTATUS
_WMTShareRendezvousLookUp(void *obj) {
  struct unixcall_share_rendezvous *params = obj;
  params->port = 0;
  params->handle = 0;
  params->name[sizeof(params->name) - 1] = '\0';
  if (strncmp(params->name, "d3dalloy.share.", sizeof("d3dalloy.share.") - 1))
    return STATUS_UNSUCCESSFUL;

  mach_port_t bp, service = MACH_PORT_NULL, reply_port = MACH_PORT_NULL;
  if (task_get_bootstrap_port(mach_task_self(), &bp) != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  kern_return_t kr = bootstrap_look_up(bp, params->name, &service);
  mach_port_deallocate(mach_task_self(), bp);
  if (kr != KERN_SUCCESS)
    return STATUS_UNSUCCESSFUL;
  if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply_port) != KERN_SUCCESS) {
    mach_port_deallocate(mach_task_self(), service);
    return STATUS_UNSUCCESSFUL;
  }

  mach_msg_header_t request;
  memset(&request, 0, sizeof(request));
  request.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
  request.msgh_size = sizeof(request);
  request.msgh_remote_port = service;
  request.msgh_local_port = reply_port;
  request.msgh_id = 0x4433414c; /* 'D3AL' */
  kr = mach_msg(&request, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(request), 0, MACH_PORT_NULL, 2000,
                MACH_PORT_NULL);
  mach_port_deallocate(mach_task_self(), service);

  NTSTATUS status = STATUS_UNSUCCESSFUL;
  if (kr == MACH_MSG_SUCCESS) {
    struct {
      struct share_reply_msg msg;
      mach_msg_trailer_t trailer;
      uint8_t slack[64];
    } reply;
    memset(&reply, 0, sizeof(reply));
    kr = mach_msg(&reply.msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(reply), reply_port, 2000,
                  MACH_PORT_NULL);
    if (kr == MACH_MSG_SUCCESS) {
      if ((reply.msg.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) && reply.msg.body.msgh_descriptor_count == 1 &&
          reply.msg.port.type == MACH_MSG_PORT_DESCRIPTOR && MACH_PORT_VALID(reply.msg.port.name) &&
          reply.msg.header.msgh_id == request.msgh_id + 100) {
        params->port = reply.msg.port.name;
        status = STATUS_SUCCESS;
      } else {
        mach_msg_destroy(&reply.msg.header);
      }
    }
  }
  mach_port_mod_refs(mach_task_self(), reply_port, MACH_PORT_RIGHT_RECEIVE, -1);
  return status;
}

@interface MTLSharedTextureHandle ()
- (IOSurfaceRef)ioSurface;
@end

@interface NSObject (D3DalloyShareIdentity)
- (uint64_t)globalTraceObjectID;
- (id)baseObject;
@end

static NTSTATUS
_WMTShareObjectIdentity(void *obj) {
  struct unixcall_share_identity *params = obj;
  params->ret = 0;
  if (!params->object)
    return STATUS_UNSUCCESSFUL;
  if (params->kind == 0) {
    NSObject *event = (NSObject *)params->object;
    // Metal API validation wraps the event; the trace id lives on the base object.
    for (int depth = 0; depth < 4 && ![event respondsToSelector:@selector(globalTraceObjectID)] &&
                        [event respondsToSelector:@selector(baseObject)];
         ++depth)
      event = [(id)event baseObject];
    if (![event conformsToProtocol:@protocol(MTLSharedEvent)] ||
        ![event respondsToSelector:@selector(globalTraceObjectID)])
      return STATUS_UNSUCCESSFUL;
    params->ret = [event globalTraceObjectID];
  } else if (params->kind == 1) {
    id<MTLTexture> texture = (id<MTLTexture>)params->object;
    if (![texture isShareable])
      return STATUS_UNSUCCESSFUL;
    MTLSharedTextureHandle *handle = [texture newSharedTextureHandle];
    if (!handle)
      return STATUS_UNSUCCESSFUL;
    if ([handle respondsToSelector:@selector(ioSurface)]) {
      IOSurfaceRef surface = [handle ioSurface];
      if (surface)
        params->ret = IOSurfaceGetID(surface);
    }
    [handle release];
  } else {
    return STATUS_UNSUCCESSFUL;
  }
  return params->ret ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS
_WMTMachPortDeallocate(void *obj) {
  struct unixcall_share_rendezvous *params = obj;
  if (MACH_PORT_VALID(params->port))
    mach_port_deallocate(mach_task_self(), params->port);
  params->port = 0;
  return STATUS_SUCCESS;
}

/*
 * The per-user session salt lives in the private per-user temporary directory
 * (confstr _CS_DARWIN_USER_TEMP_DIR, mode 0700), created 0600 and published
 * atomically with link(2), so every process of this user derives the same
 * bootstrap-free hash for a D3D object name and no other user can predict it.
 */
static bool
share_session_salt(uint8_t salt[16]) {
  char dir[PATH_MAX];
  size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof(dir));
  if (!n || n >= sizeof(dir) - 32)
    return false;
  strlcat(dir, "d3dalloy-share", sizeof(dir));
  if (mkdir(dir, 0700) && errno != EEXIST)
    return false;
  struct stat st;
  if (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077))
    return false;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/salt", dir);
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/salt.XXXXXX", dir);
    int tfd = mkstemp(tmp);
    if (tfd < 0)
      return false;
    uint8_t fresh[16];
    arc4random_buf(fresh, sizeof(fresh));
    bool wrote = fchmod(tfd, 0600) == 0 && write(tfd, fresh, sizeof(fresh)) == sizeof(fresh) && fsync(tfd) == 0;
    close(tfd);
    if (wrote)
      link(tmp, path); /* loses a race harmlessly: the winner's salt is read below */
    unlink(tmp);
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
      return false;
  }
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 077) &&
            st.st_size == 16 && read(fd, salt, 16) == 16;
  close(fd);
  return ok;
}

static NTSTATUS
_WMTShareHashName(void *obj) {
  struct unixcall_share_hashname *params = obj;
  params->hash[0] = '\0';
  if (!params->length || params->length > sizeof(params->name) / sizeof(params->name[0]))
    return STATUS_UNSUCCESSFUL;
  uint8_t salt[16];
  if (!share_session_salt(salt))
    return STATUS_UNSUCCESSFUL;
  CC_SHA256_CTX ctx;
  CC_SHA256_Init(&ctx);
  CC_SHA256_Update(&ctx, "d3dalloy.d3d12.name", sizeof("d3dalloy.d3d12.name"));
  CC_SHA256_Update(&ctx, salt, sizeof(salt));
  CC_SHA256_Update(&ctx, params->name, params->length * sizeof(params->name[0]));
  uint8_t digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(digest, &ctx);
  for (unsigned i = 0; i < 16; ++i)
    snprintf(params->hash + i * 2, 3, "%02x", digest[i]);
  return STATUS_SUCCESS;
}


const void *__wine_unix_call_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer,
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk_SM50Compile,
    &thunk_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk_SM50CompileGeometryPipelineVertex,
    &thunk_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk_SM50CompileTessellationPipelineHull,
    &thunk_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk_SM50GetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_MTLDevice_newResidencySet,
    &_MTLResidencySet_addAllocations,
    &_MTLResidencySet_removeAllocations,
    &_MTLResidencySet_removeAllAllocations,
    &_MTLResidencySet_commit,
    &_MTLCommandQueue_addResidencySet,
    &_MTLDevice_newHeap,
    &_MTLDevice_heapBufferSizeAndAlign,
    &_MTLDevice_heapTextureSizeAndAlign,
    &_MTLHeap_newBuffer,
    &_MTLHeap_newTexture,
    &_MTLDevice_newIndirectCommandBuffer,
    &_MTLDevice_newLibraryWithSource,
    &_MTLTexture_getBytes,
    /* 146..164: DXIL / DXIL ray-tracing compiler entries (reserved, owned by the DXIL
     * compiler work; the table must stay index-stable for the PE-side thunks). */
    &thunk_SM50RTEnumerateLibraryExports,
    &thunk_SM50RTCompileRaytracingPipeline,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 165..171: raytracing primitives. */
    &_MTLDevice_newComputePipelineStateWithLinkedFunctions,
    &_MTLComputePipelineState_newVisibleFunctionTable,
    &_MTLComputePipelineState_newIntersectionFunctionTable,
    &_MTLComputePipelineState_functionHandle,
    &_MTLComputePipelineState_maxTotalThreadsPerThreadgroup,
    &_MTLVisibleFunctionTable_setFunction,
    &_MTLIntersectionFunctionTable_setFunction,
    /* 172..195: acceleration structures. */
    &_WMTASDescriptor_new,
    &_WMTASDescriptor_setUsage,
    &_WMTASDescriptor_setTriangles,
    &_WMTASDescriptor_setAABBs,
    &_WMTASDescriptor_setInstances,
    &_WMTDevice_getASSizes,
    &_WMTASSidecar_new,
    &_WMTASSidecar_accelerationStructure,
    &_WMTASSidecar_instanceBuffer,
    &_WMTASSidecar_contributionBuffer,
    &_WMTASSidecar_resourceID,
    &_WMTASSidecar_contributionGPUAddress,
    &_WMTASSidecar_size,
    &_WMTCommandBuffer_accelerationStructureCommandEncoder,
    &_WMTASEncoder_build,
    &_WMTASEncoder_refit,
    &_WMTASEncoder_copy,
    &_WMTASEncoder_compactedSize,
    &_WMTComputeEncoder_encodeASPrepass,
    &_WMTUnimplemented, /* 191: retired DXR scalar postbuild helper */
    &_WMTUnimplemented, /* 192: retired DXR scalar compaction helper */
    &_MTLResource_gpuResourceID,
    &_MTLComputeCommandEncoder_setVisibleFunctionTable,
    &_MTLComputeCommandEncoder_setIntersectionFunctionTable,
    /* 196..197: the intersection function table's own buffer argument table. */
    &_MTLIntersectionFunctionTable_setBuffer,
    &_MTLIntersectionFunctionTable_setVisibleFunctionTable,
    &_WMTASEncoder_fence,
    &_WMTGetPipelineCacheIdentity, /* 199: native compiler image and Metal driver build. */
    &_MTLDevice_maxBufferLength, /* 200: validate cached constant allocations. */
    &_MTLDevice_sampleTimestamps, /* 201: paired CPU/GPU clock calibration sample. */
    &_MTLAllocation_allocatedSize, /* 202: pageable allocation byte accounting. */
    &_MTLResidencySet_allocatedSize, /* 203: committed residency-set byte total. */
    &_WMTPresentationObserver_create, /* 204 */
    &_MetalDrawable_observePresentation, /* 205 */
    &_WMTPresentationObserver_wait, /* 206 */
    &_WMTPresentationObserver_cancel, /* 207 */
    &_WMTPresentationObserver_close, /* 208 */
    &_WMTClockSampleHostSeconds, /* 209 */
    &_WMTPresentationObserver_setView, /* 210 */
    &_WMTPresentationObserver_getRefreshSample, /* 211 */
    &thunk_SM50NeedsRasterOrderedBufferMarker, /* 212 */
    &_MTLDevice_programmableSamplePositionsSupported, /* 213 */
    &_MTLCommandBuffer_presentDrawableIfCurrent, /* 214: restart-epoch-gated completion present. */
    &thunk_SM50CopyCompiledBitcode, /* 215: copy compiled bitcode parts into caller memory. */
    &_MTLDevice_supportsPlacementSparse, /* 216 */
    &_MTLDevice_newPlacementHeap, /* 217 */
    &_MTLDevice_newPlacementSparseBuffer, /* 218 */
    &_MTLDevice_newPlacementSparseTexture, /* 219 */
    &_MTLBuffer_newPlacementSparseTextureView, /* 220 */
    &_MTLDevice_sparseTileSize, /* 221 */
    &_MTLTexture_sparseTail, /* 222 */
    &_MTLDevice_newMTL4CommandQueue, /* 223 */
    &_MTL4CommandQueue_addResidencySet, /* 224 */
    &_MTL4CommandQueue_updateBufferMappings, /* 225 */
    &_MTL4CommandQueue_updateTextureMappings, /* 226 */
    &_MTL4CommandQueue_signalEvent, /* 227 */
    &_MTLDevice_newSamplerStateWithReduction, /* 228 */
    &_WMTUnimplemented, /* 229: reserved (tiled) */
    &_WMTUnimplemented, /* 230: reserved (tiled) */
    &_WMTUnimplemented, /* 231: reserved (tiled) */
    &_WMTShareRendezvousCreate, /* 232: D3D12 sharing */
    &_WMTShareRendezvousDestroy, /* 233: D3D12 sharing */
    &_WMTShareRendezvousLookUp, /* 234: D3D12 sharing */
    &_WMTShareObjectIdentity, /* 235: D3D12 sharing */
    &_WMTMachPortDeallocate, /* 236: D3D12 sharing */
    &_WMTShareHashName, /* 237: D3D12 sharing */
    &_WMTUnimplemented, /* 238: reserved (sharing) */
    &_WMTUnimplemented, /* 239: reserved (sharing) */
    &_WMTUnimplemented, /* 240: reserved (sharing) */
    &_WMTUnimplemented, /* 241: reserved (sharing) */
    &_WMTUnimplemented, /* 242: reserved (sharing) */
    &_WMTUnimplemented, /* 243: reserved (sharing) */
    &_WMTUnimplemented, /* 244: reserved (sharing) */
    &_WMTUnimplemented, /* 245: reserved (sharing) */
    &_WMTUnimplemented, /* 246: reserved (sharing) */
    &_WMTUnimplemented, /* 247: reserved (sharing) */
    &_WMTUnimplemented, /* 248: reserved (gsts) */
    &_WMTUnimplemented, /* 249: reserved (gsts) */
    &_WMTUnimplemented, /* 250: reserved (gsts) */
    &_WMTUnimplemented, /* 251: reserved (gsts) */
    &_WMTUnimplemented, /* 252: reserved (gsts) */
    &_WMTUnimplemented, /* 253: reserved (gsts) */
    &_WMTUnimplemented, /* 254: reserved (gsts) */
    &_WMTUnimplemented, /* 255: reserved (gsts) */
    &_MTLDevice_gpuCoreCount, /* 256: IOKit gpu-core-count of the device's accelerator (0 if unknown). */
    &_WMTUnimplemented, /* 257: reserved (FL) */
    &_WMTUnimplemented, /* 258: reserved (FL) */
    &_WMTUnimplemented, /* 259: reserved (FL) */
    &_WMTUnimplemented, /* 260: reserved (FL) */
    &_WMTUnimplemented, /* 261: reserved (FL) */
    &_WMTUnimplemented, /* 262: reserved (FL) */
    &_WMTUnimplemented, /* 263: reserved (FL) */
    &_WMTUnimplemented, /* 264: reserved (DXR) */
    &_WMTUnimplemented, /* 265: reserved (DXR) */
    &_WMTUnimplemented, /* 266: reserved (DXR) */
    &_WMTUnimplemented, /* 267: reserved (DXR) */
    &_WMTASSidecar_allocate, /* 268: DXR deferred sidecar allocation (fixed-width) */
    &_WMTASSidecar_instanceCount, /* 269: DXR sidecar instance count (fixed-width) */
    &_WMTASSidecar_generations, /* 270: DXR retained sidecar generations (fixed-width) */
    &_WMTUnimplemented, /* 271: reserved (DXR) */
    &_WMTUnimplemented, /* 272: reserved (DXR) */
    &_WMTUnimplemented, /* 273: reserved (DXR) */
    &_WMTUnimplemented, /* 274: reserved (DXR) */
    &_WMTUnimplemented, /* 275: reserved (DXR) */
    &_WMTUnimplemented, /* 276: reserved (DXR) */
    &_WMTUnimplemented, /* 277: reserved (DXR) */
    &_WMTUnimplemented, /* 278: reserved (DXR) */
    &_WMTUnimplemented, /* 279: reserved (DXR) */
    &_WMTUnimplemented, /* 280: reserved (MESH) */
    &_WMTUnimplemented, /* 281: reserved (MESH) */
    &_WMTUnimplemented, /* 282: reserved (MESH) */
    &_WMTUnimplemented, /* 283: reserved (MESH) */
    &_WMTUnimplemented, /* 284: reserved (MESH) */
    &_WMTUnimplemented, /* 285: reserved (MESH) */
    &_WMTUnimplemented, /* 286: reserved (MESH) */
    &_WMTUnimplemented, /* 287: reserved (MESH) */
    &_WMTUnimplemented, /* 288: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 289: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 290: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 291: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 292: reserved (VRS) */
    &_WMTUnimplemented, /* 293: reserved (VRS) */
    &_WMTUnimplemented, /* 294: reserved (VRS) */
    &_WMTUnimplemented, /* 295: reserved (VRS) */
    &_WMTUnimplemented, /* 296: reserved (VRS) */
    &_WMTUnimplemented, /* 297: reserved (VRS) */
    &_WMTUnimplemented, /* 298: reserved (VRS) */
    &_WMTUnimplemented, /* 299: reserved (VRS) */
    &_WMTUnimplemented, /* 300: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 301: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 302: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 303: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 304: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 305: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 306: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 307: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 308: reserved (VIEWINST) */
    &_WMTUnimplemented, /* 309: reserved (VIEWINST) */
    &_WMTUnimplemented, /* 310: reserved (VIEWINST) */
    &_WMTUnimplemented, /* 311: reserved (VIEWINST) */
    &thunk_SM50WGReflectLibrary, /* 312: work graph library reflection */
    &thunk_SM50WGCompileNode, /* 313: work graph node compilation */
    &winemetal_metal4_call, /* 314: fixed-width typed Metal4 operation ABI */
    &_WMTDeviceCatalog_call, /* 315: fixed-width device hints and observer */
    &_WMTMonitorTiming_call, /* 316: fixed-width monitor timing */
    &_WMTComposition_call, /* 317: fixed-width composition, including nested PE32 pointers */
    &_WMTGetGuestWindowBase, /* 318: native typed guest-window query */
    &thunk_SM50GetErrorKind, /* 319: fixed-width error kind */
    &thunk_SM50CloneError, /* 320: fixed-width independent error handle */
    &_MetalLayer_acquirePresentDrawable, /* 321: inline drawable and texture metadata */
};
_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == 322,
               "PE64/native composition dispatch index");

#ifndef D3DALLOY_NATIVE
const void *__wine_unix_call_wow64_funcs[] = {
    &_NSObject_retain,
    &_NSObject_release,
    &_NSArray_object,
    &_NSArray_count,
    &_MTLCopyAllDevices,
    &_MTLDevice_recommendedMaxWorkingSetSize,
    &_MTLDevice_currentAllocatedSize,
    &_MTLDevice_name,
    &_NSString_getCString,
    &_MTLDevice_newCommandQueue,
    &_NSAutoreleasePool_alloc_init,
    &_MTLCommandQueue_commandBuffer,
    &_MTLCommandBuffer_commit,
    &_MTLCommandBuffer_waitUntilCompleted,
    &_MTLCommandBuffer_status,
    &_MTLDevice_newSharedEvent,
    &_MTLSharedEvent_signaledValue,
    &_MTLCommandBuffer_encodeSignalEvent,
    &_MTLDevice_newBuffer32, /* 18: CPU-visible storage the 32-bit caller can address. */
    &_MTLDevice_newSamplerState,
    &_MTLDevice_newDepthStencilState,
    &_MTLDevice_newTexture,
    &_MTLBuffer_newTexture,
    &_MTLTexture_newTextureView,
    &_MTLDevice_minimumLinearTextureAlignmentForPixelFormat,
    &_MTLDevice_newLibrary,
    &_MTLLibrary_newFunction,
    &_NSString_lengthOfBytesUsingEncoding,
    &_NSObject_description,
    &_MTLDevice_newComputePipelineState,
    &_MTLCommandBuffer_blitCommandEncoder,
    &_MTLCommandBuffer_computeCommandEncoder,
    &_MTLCommandBuffer_renderCommandEncoder,
    &_MTLCommandEncoder_endEncoding,
    &_MTLDevice_newRenderPipelineState,
    &_MTLDevice_newMeshRenderPipelineState,
    &_MTLBlitCommandEncoder_encodeCommands,
    &_MTLComputeCommandEncoder_encodeCommands,
    &_MTLRenderCommandEncoder_encodeCommands,
    &_MTLTexture_pixelFormat,
    &_MTLTexture_width,
    &_MTLTexture_height,
    &_MTLTexture_depth,
    &_MTLTexture_arrayLength,
    &_MTLTexture_mipmapLevelCount,
    &_MTLTexture_replaceRegion,
    &_MTLBuffer_didModifyRange,
    &_MTLCommandBuffer_presentDrawable,
    &_MTLCommandBuffer_presentDrawableAfterMinimumDuration,
    &_MTLDevice_supportsFamily,
    &_MTLDevice_supportsBCTextureCompression,
    &_MTLDevice_supportsTextureSampleCount,
    &_MTLDevice_hasUnifiedMemory,
    &_MTLCaptureManager_sharedCaptureManager,
    &_MTLCaptureManager_startCapture,
    &_MTLCaptureManager_stopCapture,
    &_MTLDevice_newTemporalScaler,
    &_MTLDevice_newSpatialScaler,
    &_MTLCommandBuffer_encodeTemporalScale,
    &_MTLCommandBuffer_encodeSpatialScale,
    &_NSString_string,
    &_NSString_alloc_init,
    &_DeveloperHUDProperties_instance,
    &_DeveloperHUDProperties_addLabel,
    &_DeveloperHUDProperties_updateLabel,
    &_DeveloperHUDProperties_remove,
    &_MetalDrawable_texture,
    &_MetalLayer_nextDrawable,
    &_MTLDevice_supportsFXSpatialScaler,
    &_MTLDevice_supportsFXTemporalScaler,
    &_MetalLayer_setProps,
    &_MetalLayer_getProps,
    &_CreateMetalViewFromHWND,
    &_ReleaseMetalView,
    &thunk32_SM50Initialize,
    &thunk_SM50Destroy,
    &thunk32_SM50Compile,
    &thunk32_SM50GetCompiledBitcode,
    &thunk_SM50DestroyBitcode,
    &thunk32_SM50GetErrorMessage,
    &thunk_SM50FreeError,
    &thunk32_SM50CompileGeometryPipelineVertex,
    &thunk32_SM50CompileGeometryPipelineGeometry,
    NULL,
    &thunk32_SM50CompileTessellationPipelineHull,
    &thunk32_SM50CompileTessellationPipelineDomain,
    &_MTLCommandEncoder_setLabel,
    &_MTLDevice_setShouldMaximizeConcurrentCompilation,
    &thunk32_SM50GetArgumentsInfo,
    &_MTLCommandBuffer_error,
    &_MTLCommandBuffer_logs,
    &_MTLLogContainer_enumerate,
    &_CGColorSpace_checkColorSpaceSupported,
    &_MetalLayer_setColorSpace,
    &_WMTGetPrimaryDisplayId,
    &_WMTGetSecondaryDisplayId,
    &_WMTGetDisplayDescription,
    &_MetalLayer_getEDRValue,
    &_MTLLibrary_newFunctionWithConstants,
    &_WMTQueryDisplaySetting,
    &_WMTUpdateDisplaySetting,
    &_WMTQueryDisplaySettingForLayer,
    &_MTLCommandBuffer_encodeWaitForEvent,
    &_MTLSharedEvent_signalValue,
    &_MTLSharedEvent_setWin32EventAtValue,
    &_MTLDevice_newFence,
    &_MTLDevice_newEvent,
    &_MTLBuffer_updateContents,
    &_SharedEventListener_create,
    &_SharedEventListener_start,
    &_SharedEventListener_destroy,
    &_WMTGetOSVersion,
    &_MTLDevice_newBinaryArchive,
    &_MTLBinaryArchive_serialize,
    &_DispatchData_alloc_init,
    &_CacheReader_alloc_init,
    &_CacheReader_get,
    &_CacheWriter_alloc_init,
    &_CacheWriter_set,
    &_WMTSetMetalShaderCachePath,
    &_MTLDevice_newSharedTexture,
    &_WMTBootstrapRegister,
    &_WMTBootstrapLookUp,
    &_MTLSharedEvent_createMachPort,
    &_MTLDevice_newSharedEventWithMachPort,
    &_MTLDevice_registryID,
    &_MTLSharedEvent_waitUntilSignaledValue,
    &_MTLCounterSampleBuffer_newTimestampBuffer,
    &_MTLCounterSampleBuffer_resolveCounterRange,
    &_MTLCommandBuffer_blitCommandEncoderWithSampleBuffers,
    &_MTLCommandBuffer_property,
    &_MTLDevice_newTileRenderPipelineState,
    &_MTLDevice_newResidencySet,
    &_MTLResidencySet_addAllocations,
    &_MTLResidencySet_removeAllocations,
    &_MTLResidencySet_removeAllAllocations,
    &_MTLResidencySet_commit,
    &_MTLCommandQueue_addResidencySet,
    &_MTLDevice_newHeap,
    &_MTLDevice_heapBufferSizeAndAlign,
    &_MTLDevice_heapTextureSizeAndAlign,
    &_MTLHeap_newBuffer,
    &_MTLHeap_newTexture,
    &_MTLDevice_newIndirectCommandBuffer,
    &_MTLDevice_newLibraryWithSource,
    &_MTLTexture_getBytes,
    /* 146..164: DXIL / DXIL ray-tracing compiler entries (reserved, owned by the DXIL
     * compiler work; the table must stay index-stable for the PE-side thunks). */
    &thunk32_SM50RTEnumerateLibraryExports,
    &thunk32_SM50RTCompileRaytracingPipeline,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    /* 165..171: raytracing primitives. */
    &_MTLDevice_newComputePipelineStateWithLinkedFunctions,
    &_MTLComputePipelineState_newVisibleFunctionTable,
    &_MTLComputePipelineState_newIntersectionFunctionTable,
    &_MTLComputePipelineState_functionHandle,
    &_MTLComputePipelineState_maxTotalThreadsPerThreadgroup,
    &_MTLVisibleFunctionTable_setFunction,
    &_MTLIntersectionFunctionTable_setFunction,
    /* 172..195: acceleration structures. */
    &_WMTASDescriptor_new,
    &_WMTASDescriptor_setUsage,
    &_WMTASDescriptor_setTriangles,
    &_WMTASDescriptor_setAABBs,
    &_WMTASDescriptor_setInstances,
    &_WMTDevice_getASSizes,
    &_WMTASSidecar_new,
    &_WMTASSidecar_accelerationStructure,
    &_WMTASSidecar_instanceBuffer,
    &_WMTASSidecar_contributionBuffer,
    &_WMTASSidecar_resourceID,
    &_WMTASSidecar_contributionGPUAddress,
    &_WMTASSidecar_size,
    &_WMTCommandBuffer_accelerationStructureCommandEncoder,
    &_WMTASEncoder_build,
    &_WMTASEncoder_refit,
    &_WMTASEncoder_copy,
    &_WMTASEncoder_compactedSize,
    &_WMTComputeEncoder_encodeASPrepass,
    &_WMTUnimplemented, /* 191: retired DXR scalar postbuild helper */
    &_WMTUnimplemented, /* 192: retired DXR scalar compaction helper */
    &_MTLResource_gpuResourceID,
    &_MTLComputeCommandEncoder_setVisibleFunctionTable,
    &_MTLComputeCommandEncoder_setIntersectionFunctionTable,
    /* 196..197: the intersection function table's own buffer argument table. */
    &_MTLIntersectionFunctionTable_setBuffer,
    &_MTLIntersectionFunctionTable_setVisibleFunctionTable,
    &_WMTASEncoder_fence,
    &_WMTGetPipelineCacheIdentity,
    &_MTLDevice_maxBufferLength,
    &_MTLDevice_sampleTimestamps, /* 201: timestamps return in the shared fixed-width POD. */
    &_MTLAllocation_allocatedSize,
    &_MTLResidencySet_allocatedSize,
    &_WMTPresentationObserver_create,
    &_MetalDrawable_observePresentation,
    &_WMTPresentationObserver_wait,
    &_WMTPresentationObserver_cancel,
    &_WMTPresentationObserver_close,
    &_WMTClockSampleHostSeconds,
    &_WMTPresentationObserver_setView,
    &_WMTPresentationObserver_getRefreshSample,
    &thunk32_SM50NeedsRasterOrderedBufferMarker, /* 212 */
    &_MTLDevice_programmableSamplePositionsSupported, /* 213 */
    &_MTLCommandBuffer_presentDrawableIfCurrent, /* 214 */
    &thunk_SM50CopyCompiledBitcode, /* 215: fixed-width params, shared with the 64-bit table. */
    /* 216..231: fixed-width params (pointers travel as 64-bit WMTMemoryPointer). */
    &_MTLDevice_supportsPlacementSparse, /* 216 */
    &_MTLDevice_newPlacementHeap, /* 217 */
    &_MTLDevice_newPlacementSparseBuffer, /* 218 */
    &_MTLDevice_newPlacementSparseTexture, /* 219 */
    &_MTLBuffer_newPlacementSparseTextureView, /* 220 */
    &_MTLDevice_sparseTileSize, /* 221 */
    &_MTLTexture_sparseTail, /* 222 */
    &_MTLDevice_newMTL4CommandQueue, /* 223 */
    &_MTL4CommandQueue_addResidencySet, /* 224 */
    &_MTL4CommandQueue_updateBufferMappings, /* 225 */
    &_MTL4CommandQueue_updateTextureMappings, /* 226 */
    &_MTL4CommandQueue_signalEvent, /* 227 */
    &_MTLDevice_newSamplerStateWithReduction, /* 228 */
    &_WMTUnimplemented, /* 229: reserved (tiled) */
    &_WMTUnimplemented, /* 230: reserved (tiled) */
    &_WMTUnimplemented, /* 231: reserved (tiled) */
    &_WMTShareRendezvousCreate, /* 232: D3D12 sharing */
    &_WMTShareRendezvousDestroy, /* 233: D3D12 sharing */
    &_WMTShareRendezvousLookUp, /* 234: D3D12 sharing */
    &_WMTShareObjectIdentity, /* 235: D3D12 sharing */
    &_WMTMachPortDeallocate, /* 236: D3D12 sharing */
    &_WMTShareHashName, /* 237: D3D12 sharing */
    &_WMTUnimplemented, /* 238: reserved (sharing) */
    &_WMTUnimplemented, /* 239: reserved (sharing) */
    &_WMTUnimplemented, /* 240: reserved (sharing) */
    &_WMTUnimplemented, /* 241: reserved (sharing) */
    &_WMTUnimplemented, /* 242: reserved (sharing) */
    &_WMTUnimplemented, /* 243: reserved (sharing) */
    &_WMTUnimplemented, /* 244: reserved (sharing) */
    &_WMTUnimplemented, /* 245: reserved (sharing) */
    &_WMTUnimplemented, /* 246: reserved (sharing) */
    &_WMTUnimplemented, /* 247: reserved (sharing) */
    &_WMTUnimplemented, /* 248: reserved (gsts) */
    &_WMTUnimplemented, /* 249: reserved (gsts) */
    &_WMTUnimplemented, /* 250: reserved (gsts) */
    &_WMTUnimplemented, /* 251: reserved (gsts) */
    &_WMTUnimplemented, /* 252: reserved (gsts) */
    &_WMTUnimplemented, /* 253: reserved (gsts) */
    &_WMTUnimplemented, /* 254: reserved (gsts) */
    &_WMTUnimplemented, /* 255: reserved (gsts) */
    &_MTLDevice_gpuCoreCount, /* 256: IOKit gpu-core-count of the device's accelerator (0 if unknown). */
    &_WMTUnimplemented, /* 257: reserved (FL) */
    &_WMTUnimplemented, /* 258: reserved (FL) */
    &_WMTUnimplemented, /* 259: reserved (FL) */
    &_WMTUnimplemented, /* 260: reserved (FL) */
    &_WMTUnimplemented, /* 261: reserved (FL) */
    &_WMTUnimplemented, /* 262: reserved (FL) */
    &_WMTUnimplemented, /* 263: reserved (FL) */
    &_WMTUnimplemented, /* 264: reserved (DXR) */
    &_WMTUnimplemented, /* 265: reserved (DXR) */
    &_WMTUnimplemented, /* 266: reserved (DXR) */
    &_WMTUnimplemented, /* 267: reserved (DXR) */
    &_WMTASSidecar_allocate, /* 268: DXR deferred sidecar allocation (fixed-width) */
    &_WMTASSidecar_instanceCount, /* 269: DXR sidecar instance count (fixed-width) */
    &_WMTASSidecar_generations, /* 270: DXR retained sidecar generations (fixed-width) */
    &_WMTUnimplemented, /* 271: reserved (DXR) */
    &_WMTUnimplemented, /* 272: reserved (DXR) */
    &_WMTUnimplemented, /* 273: reserved (DXR) */
    &_WMTUnimplemented, /* 274: reserved (DXR) */
    &_WMTUnimplemented, /* 275: reserved (DXR) */
    &_WMTUnimplemented, /* 276: reserved (DXR) */
    &_WMTUnimplemented, /* 277: reserved (DXR) */
    &_WMTUnimplemented, /* 278: reserved (DXR) */
    &_WMTUnimplemented, /* 279: reserved (DXR) */
    &_WMTUnimplemented, /* 280: reserved (MESH) */
    &_WMTUnimplemented, /* 281: reserved (MESH) */
    &_WMTUnimplemented, /* 282: reserved (MESH) */
    &_WMTUnimplemented, /* 283: reserved (MESH) */
    &_WMTUnimplemented, /* 284: reserved (MESH) */
    &_WMTUnimplemented, /* 285: reserved (MESH) */
    &_WMTUnimplemented, /* 286: reserved (MESH) */
    &_WMTUnimplemented, /* 287: reserved (MESH) */
    &_WMTUnimplemented, /* 288: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 289: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 290: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 291: reserved (CONSRAST) */
    &_WMTUnimplemented, /* 292: reserved (VRS) */
    &_WMTUnimplemented, /* 293: reserved (VRS) */
    &_WMTUnimplemented, /* 294: reserved (VRS) */
    &_WMTUnimplemented, /* 295: reserved (VRS) */
    &_WMTUnimplemented, /* 296: reserved (VRS) */
    &_WMTUnimplemented, /* 297: reserved (VRS) */
    &_WMTUnimplemented, /* 298: reserved (VRS) */
    &_WMTUnimplemented, /* 299: reserved (VRS) */
    &_WMTUnimplemented, /* 300: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 301: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 302: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 303: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 304: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 305: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 306: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 307: reserved (FEEDBACK) */
    &_WMTUnimplemented, /* 308: reserved (VIEWINST) */
    &_WMTUnimplemented, /* 309: reserved (VIEWINST) */
    &_WMTUnimplemented, /* 310: reserved (VIEWINST) */
    &_WMTUnimplemented, /* 311: reserved (VIEWINST) */
    &thunk32_SM50WGReflectLibrary, /* 312: work graph library reflection */
    &thunk32_SM50WGCompileNode, /* 313: work graph node compilation */
    &winemetal_metal4_call, /* 314: identical POD layout; nested caller pointers are zero-extended */
    &_WMTDeviceCatalog_call, /* 315: identical fixed-width POD layout */
    &_WMTMonitorTiming_call, /* 316: identical fixed-width POD layout */
    &_WMTComposition_call, /* 317: identical 40-byte native/PE32 layout */
    &_WMTGetGuestWindowBase, /* 318: native typed guest-window query */
    &thunk_SM50GetErrorKind, /* 319: identical fixed-width layout on PE32 */
    &thunk_SM50CloneError, /* 320: inline handle needs no pointer conversion */
    &_MetalLayer_acquirePresentDrawable, /* 321: inline drawable and texture metadata */
};
_Static_assert(sizeof(__wine_unix_call_wow64_funcs) / sizeof(__wine_unix_call_wow64_funcs[0]) == 322,
               "PE32/native composition dispatch index");
#endif
