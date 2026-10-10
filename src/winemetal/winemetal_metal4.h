#pragma once
#include "winemetal.h"
#include <stddef.h>

/* Append-only Unix ABI 314. All addresses are zero-extended caller addresses;
 * object handles are native 64-bit identities, never truncated by WoW64. */
#pragma pack(push, 8)
struct WMTMetal4Call {
  uint32_t operation;
  uint32_t reserved;
  uint64_t args[16];
  uint64_t ret[8];
};
#ifdef __cplusplus
static_assert(sizeof(WMTMetal4Call) == 200);
static_assert(offsetof(WMTMetal4Call, args) == 8);
static_assert(sizeof(WMTCompositionFrame) == 40);
#else
_Static_assert(sizeof(struct WMTMetal4Call) == 200, "PE32/PE64/native Metal4 call ABI");
_Static_assert(offsetof(struct WMTMetal4Call, args) == 8, "Metal4 caller pointer slots ABI");
_Static_assert(sizeof(struct WMTCompositionFrame) == 40, "PE32 queued composition frame ABI");
#endif
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
  WMTMetal4UseAllocation,
  WMTMetal4Diagnostic,
  // args[0]: recording command buffer; args[1]: acquired ordinary/paired drawable.
  WMTMetal4UseDrawable = 44,
  // args[1]: exactly eight physical indices 0..7. Disabled outputs use a genuinely
  // unbound physical slot chosen by the caller; the SDK rejects 0xff.
  WMTMetal4SetColorAttachmentMap = 45,
  WMTMetal4NewTextureViewPool = 46,
  WMTMetal4SetTextureView = 47,
  WMTMetal4CopyTextureViews = 48,
  WMTMetal4ClearTextureView = 49,
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

/* Pool descriptors have no host-sized fields. */
struct WMTTextureViewPoolInfo {
  uint32_t format, type;
  uint32_t first_mip, mip_count;
  uint32_t first_slice, slice_count;
};

#ifdef __cplusplus
static_assert(sizeof(WMTTextureViewPoolInfo) == 24);
static_assert(sizeof(WMTRenderPassInfo) == 688);
static_assert(offsetof(WMTRenderPassInfo, sample_positions) == 667);
static_assert(offsetof(WMTRenderPassInfo, support_color_attachment_mapping) == 684);
static_assert(sizeof(WMTRenderPipelineInfo) == 168);
static_assert(offsetof(WMTRenderPipelineInfo, inherit_color_attachment_mapping) == 164);
#else
_Static_assert(sizeof(struct WMTTextureViewPoolInfo) == 24, "texture pool descriptor ABI");
_Static_assert(sizeof(struct WMTRenderPassInfo) == 688, "render pass ABI");
_Static_assert(offsetof(struct WMTRenderPassInfo, sample_positions) == 667, "sample positions ABI");
_Static_assert(offsetof(struct WMTRenderPassInfo, support_color_attachment_mapping) == 684, "mapping flag ABI");
_Static_assert(sizeof(struct WMTRenderPipelineInfo) == 168, "render pipeline ABI");
_Static_assert(offsetof(struct WMTRenderPipelineInfo, inherit_color_attachment_mapping) == 164, "pipeline mapping ABI");
#endif

WINEMETAL_API int32_t WMTMetal4Call(struct WMTMetal4Call *params);

#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#ifdef __cplusplus
extern "C" {
#endif
int32_t winemetal_metal4_call(void *params);
int32_t winemetal_metal4_pipeline_call(struct WMTMetal4Call *params);
int32_t winemetal_metal4_query_call(struct WMTMetal4Call *params);
int32_t winemetal_metal4_fx_call(struct WMTMetal4Call *params);
id<MTL4CommandBuffer> winemetal_metal4_command_buffer(obj_handle_t handle);
void winemetal_composition_drawables(id drawable, id<CAMetalDrawable> *primary, id<CAMetalDrawable> *residual);
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
