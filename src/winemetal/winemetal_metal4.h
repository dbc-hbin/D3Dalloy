#pragma once
#include "winemetal.h"

/* Append-only Unix ABI 314. All addresses are zero-extended caller addresses;
 * object handles are native 64-bit identities, never truncated by WoW64. */
#pragma pack(push, 8)
struct WMTMetal4Call {
  uint32_t operation;
  uint32_t reserved;
  uint64_t args[16];
  uint64_t ret[8];
};
struct WMTMetal4SubmissionInfo {
  uint64_t completed;
  obj_handle_t error_handle;
  uint64_t gpu_start_ns;
  uint64_t gpu_end_ns;
};
enum WMTMetal4Operation : uint32_t {
  WMTMetal4Supports = 0,
  WMTMetal4NewAllocator,
  WMTMetal4ResetAllocator,
  WMTMetal4AllocatorSize,
  WMTMetal4NewCommandBuffer,
  WMTMetal4BeginCommandBuffer,
  WMTMetal4EndCommandBuffer,
  WMTMetal4RenderEncoder,
  WMTMetal4ComputeEncoder,
  WMTMetal4EndEncoding,
  WMTMetal4SetEncoderLabel,
  WMTMetal4RenderCommands,
  WMTMetal4ComputeCommands,
  WMTMetal4BlitCommands,
  WMTMetal4Commit,
  WMTMetal4WaitSubmission,
  WMTMetal4GetSubmissionInfo,
  WMTMetal4WaitEvent,
  WMTMetal4WaitDrawable,
  WMTMetal4SignalDrawable,
  WMTMetal4UseResidencySet,
  WMTMetal4NewArgumentTable,
  WMTMetal4TableAddress,
  WMTMetal4TableResource,
  WMTMetal4TableTexture,
  WMTMetal4TableSampler,
  WMTMetal4TableAddressStride,
  WMTMetal4ComputeArgumentTable,
  WMTMetal4RenderArgumentTable,
  WMTMetal4Barrier,
  WMTMetal4Fence,
  WMTMetal4NewASDescriptor,
  WMTMetal4ASUsage,
  WMTMetal4ASTriangles,
  WMTMetal4ASBoxes,
  WMTMetal4ASInstances,
  WMTMetal4ASSizes,
  WMTMetal4ASBuild,
  WMTMetal4ASRefit,
  WMTMetal4ASCopy,
  WMTMetal4ASCompactedSize,
  WMTMetal4ASPrepass,
};
enum WMTMetal4Stages : uint64_t {
  WMTMetal4StageVertex = 1ull << 0,
  WMTMetal4StageFragment = 1ull << 1,
  WMTMetal4StageTile = 1ull << 2,
  WMTMetal4StageObject = 1ull << 3,
  WMTMetal4StageMesh = 1ull << 4,
  WMTMetal4StageResourceState = 1ull << 26,
  WMTMetal4StageDispatch = 1ull << 27,
  WMTMetal4StageBlit = 1ull << 28,
  WMTMetal4StageAccelerationStructure = 1ull << 29,
  WMTMetal4StageMachineLearning = 1ull << 30,
  WMTMetal4StageAll = 0x7fffffffffffffffull,
};
enum WMTMetal4Visibility : uint64_t {
  WMTMetal4VisibilityNone = 0,
  WMTMetal4VisibilityDevice = 1,
  WMTMetal4VisibilityResourceAlias = 2,
};
#pragma pack(pop)

WINEMETAL_API int32_t WMTMetal4Call(struct WMTMetal4Call *params);

#ifdef __OBJC__
#import <Metal/Metal.h>
#ifdef __cplusplus
extern "C" {
#endif
int32_t winemetal_metal4_call(void *params);
int32_t winemetal_metal4_pipeline_call(struct WMTMetal4Call *params);
int32_t winemetal_metal4_query_call(struct WMTMetal4Call *params);
int32_t winemetal_metal4_fx_call(struct WMTMetal4Call *params);
id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t handle);
void *winemetal_metal4_split_command_buffer(void *context);
id<MTL4ComputeCommandEncoder> winemetal_metal4_compute_encoder(obj_handle_t handle);
id<MTL4RenderCommandEncoder> winemetal_metal4_render_encoder(obj_handle_t handle);
void winemetal_metal4_retain_resource(obj_handle_t command_buffer, id resource);
void winemetal_metal4_use_allocation(obj_handle_t command_buffer, id<MTLAllocation> allocation);
MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format);
#ifdef __cplusplus
}
#endif
#endif
