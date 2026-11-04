#pragma once
#include "Metal.hpp"
#include "winemetal_metal4.h"
#include <cassert>

namespace WMT {

/* CPU addresses alone use the guest window; object handles and GPU addresses do not. */
inline uint64_t Metal4CPUPointer(const void *pointer) {
  return pointer ? WMTGetGuestWindowBase() + uint64_t(reinterpret_cast<uintptr_t>(pointer)) : 0;
}

/* Typed operations never pass a classic command buffer or encoder handle. */
template <typename... Args>
inline struct WMTMetal4Call Metal4Invoke(uint32_t operation, Args... args) {
  struct WMTMetal4Call call{operation, 0, {uint64_t(args)...}, {}};
  const int32_t status = ::WMTMetal4Call(&call);
  assert(status == 0 && "Metal4 bridge operation failed");
  (void)status;
  return call;
}

class MTL4CommandAllocator : public Object {
public:
  void reset() { Metal4Invoke(WMTMetal4ResetAllocator, handle); }
  uint64_t allocatedSize() const { return Metal4Invoke(WMTMetal4AllocatorSize, handle).ret[0]; }
};

class MTL4ArgumentTable : public Object {
public:
  void setAddress(uint64_t address, uint64_t index) { Metal4Invoke(WMTMetal4TableAddress, handle, address, index); }
  void setAddress(uint64_t address, uint64_t stride, uint64_t index) { Metal4Invoke(WMTMetal4TableAddressStride, handle, address, stride, index); }
  void setResource(uint64_t resource_id, uint64_t index) { Metal4Invoke(WMTMetal4TableResource, handle, resource_id, index); }
  void setTexture(uint64_t resource_id, uint64_t index) { Metal4Invoke(WMTMetal4TableTexture, handle, resource_id, index); }
  void setSamplerState(uint64_t resource_id, uint64_t index) { Metal4Invoke(WMTMetal4TableSampler, handle, resource_id, index); }
};

class TextureViewPool : public Object {
public:
  uint64_t setTextureView(Texture texture, const WMTTextureViewPoolInfo &info, uint32_t index) {
    return Metal4Invoke(WMTMetal4SetTextureView, handle, texture.handle, Metal4CPUPointer(&info), index).ret[0];
  }
  uint64_t copyTextureView(TextureViewPool source, uint32_t from, uint32_t to, Texture parent, const WMTTextureViewPoolInfo &info) {
    return Metal4Invoke(WMTMetal4CopyTextureViews, handle, source.handle, from, to, parent.handle, Metal4CPUPointer(&info)).ret[0];
  }
  uint64_t copyTextureViews(TextureViewPool source, uint32_t from, uint32_t to, uint32_t count) {
    struct WMTMetal4Call call{WMTMetal4CopyTextureViewsRange, 0,
                              {handle, source.handle, from, to, count}, {}};
    return ::WMTMetal4Call(&call) == 0 ? call.ret[0] : 0;
  }
  void clear(uint32_t index) { Metal4Invoke(WMTMetal4ClearTextureView, handle, index); }
};

class MTL4Submission : public Object {
public:
  void waitUntilCompleted() { Metal4Invoke(WMTMetal4WaitSubmission, handle); }
  void getInfo(WMTMetal4SubmissionInfo &info) const {
    Metal4Invoke(WMTMetal4GetSubmissionInfo, handle, WMTGetGuestWindowBase() + uintptr_t(&info));
  }
  Error error() const {
    WMTMetal4SubmissionInfo info{}; getInfo(info); return Error{info.error_handle};
  }
  uint64_t gpuEndTime() const {
    WMTMetal4SubmissionInfo info{}; getInfo(info); return info.gpu_end_ns;
  }
};

class MTL4AccelerationStructureDescriptor : public Object {
public:
  void setUsage(uint32_t usage) { Metal4Invoke(WMTMetal4ASUsage, handle, usage); }
  bool setTriangles(uint32_t index, Buffer vertices, uint64_t vertex_offset, uint64_t vertex_stride,
                    Buffer indices, uint64_t index_offset, uint32_t index_type,
                    Buffer transform, uint64_t transform_offset, uint64_t triangle_count,
                    uint32_t table_offset, uint32_t flags, WMTAttributeFormat vertex_format) {
    return Metal4Invoke(WMTMetal4ASTriangles, handle, index, vertices.handle, vertex_offset, vertex_stride,
                       indices.handle, index_offset, index_type, transform.handle, transform_offset,
                       triangle_count, table_offset, flags, uint64_t(vertex_format)).ret[0] != 0;
  }
  bool setAABBs(uint32_t index, Buffer boxes, uint64_t offset, uint64_t stride, uint64_t count,
               uint32_t table_offset, uint32_t flags) {
    return Metal4Invoke(WMTMetal4ASBoxes, handle, index, boxes.handle, offset, stride, count, table_offset, flags).ret[0] != 0;
  }
  bool setInstances(Buffer instances, uint64_t offset, uint64_t count) {
    return Metal4Invoke(WMTMetal4ASInstances, handle, instances.handle, offset, count).ret[0] != 0;
  }
};

class MTL4CommandEncoder : public Object {
public:
  void endEncoding() { Metal4Invoke(WMTMetal4EndEncoding, handle); }
  void setLabel(String label) { Metal4Invoke(WMTMetal4SetEncoderLabel, handle, label.handle); }
  void barrierAfterQueueStages(uint64_t after, uint64_t before, uint64_t visibility) {
    Metal4Invoke(WMTMetal4Barrier, handle, 0, after, before, visibility);
  }
  void barrierAfterStages(uint64_t after, uint64_t before_queue, uint64_t visibility) {
    Metal4Invoke(WMTMetal4Barrier, handle, 1, after, before_queue, visibility);
  }
  void barrierAfterEncoderStages(uint64_t after, uint64_t before, uint64_t visibility) {
    Metal4Invoke(WMTMetal4Barrier, handle, 2, after, before, visibility);
  }
};

class MTL4ComputeCommandEncoder : public MTL4CommandEncoder {
public:
  void encodeCommands(const wmtcmd_compute_nop *commands, uint64_t independent_dispatch_mask = 0) {
    Metal4Invoke(WMTMetal4ComputeCommands, handle, Metal4CPUPointer(commands), independent_dispatch_mask);
  }
  void encodeCommands(const wmtcmd_blit_nop *commands) {
    Metal4Invoke(WMTMetal4BlitCommands, handle, commands ? WMTGetGuestWindowBase() + uintptr_t(commands) : 0);
  }
  void setArgumentTable(MTL4ArgumentTable table) { Metal4Invoke(WMTMetal4ComputeArgumentTable, handle, table.handle); }
  void waitForFence(Fence fence) { Metal4Invoke(WMTMetal4Fence, handle, fence.handle, 0, WMTMetal4StageDispatch | WMTMetal4StageBlit | WMTMetal4StageAccelerationStructure); }
  void updateFence(Fence fence) { Metal4Invoke(WMTMetal4Fence, handle, fence.handle, 1, WMTMetal4StageDispatch | WMTMetal4StageBlit | WMTMetal4StageAccelerationStructure); }
  void setVisibleFunctionTable(VisibleFunctionTable table, uint64_t index) {
    wmtcmd_compute_setfunctiontable cmd{}; cmd.type=WMTComputeCommandSetVisibleFunctionTable;
    cmd.table=table.handle; cmd.index=index; encodeCommands(reinterpret_cast<const wmtcmd_compute_nop*>(&cmd));
  }
  void setIntersectionFunctionTable(IntersectionFunctionTable table, uint64_t index) {
    wmtcmd_compute_setfunctiontable cmd{}; cmd.type=WMTComputeCommandSetIntersectionFunctionTable;
    cmd.table=table.handle; cmd.index=index; encodeCommands(reinterpret_cast<const wmtcmd_compute_nop*>(&cmd));
  }
  void setAccelerationStructure(Object structure, uint64_t index) {
    wmtcmd_compute_setaccelerationstructure cmd{}; cmd.type=WMTComputeCommandSetAccelerationStructure;
    cmd.acceleration_structure=structure.handle; cmd.index=index; encodeCommands(reinterpret_cast<const wmtcmd_compute_nop*>(&cmd));
  }
  bool encodeASPrepass(ComputePipelineState pipeline, Buffer source_instances, uint64_t source_offset,
                       Buffer destination_instances, uint64_t destination_offset, Buffer header, uint64_t header_offset,
                       Buffer contributions, uint64_t contribution_offset, Buffer live_mirror, uint64_t live_mirror_offset,
                       const WMTASPrepassInfo &info) {
    return Metal4Invoke(WMTMetal4ASPrepass, handle, pipeline.handle, source_instances.handle, source_offset,
                       destination_instances.handle, destination_offset, header.handle, header_offset,
                       contributions.handle, contribution_offset, live_mirror.handle, live_mirror_offset,
                       WMTGetGuestWindowBase() + uintptr_t(&info)).ret[0] != 0;
  }
  bool build(MTL4AccelerationStructureDescriptor descriptor, AccelerationStructureSidecar destination, Buffer scratch, uint64_t offset) {
    return Metal4Invoke(WMTMetal4ASBuild, handle, descriptor.handle, destination.handle, scratch.handle, offset).ret[0] != 0;
  }
  bool refit(MTL4AccelerationStructureDescriptor descriptor, AccelerationStructureSidecar source,
             AccelerationStructureSidecar destination, Buffer scratch, uint64_t offset) {
    return Metal4Invoke(WMTMetal4ASRefit, handle, descriptor.handle, source.handle, destination.handle, scratch.handle, offset).ret[0] != 0;
  }
  bool copy(AccelerationStructureSidecar source, AccelerationStructureSidecar destination, uint32_t mode) {
    return Metal4Invoke(WMTMetal4ASCopy, handle, source.handle, destination.handle, mode).ret[0] != 0;
  }
  void compactedSize(AccelerationStructureSidecar source, Buffer destination, uint64_t offset) {
    Metal4Invoke(WMTMetal4ASCompactedSize, handle, source.handle, destination.handle, offset);
  }
};

class MTL4RenderCommandEncoder : public MTL4CommandEncoder {
public:
  void setColorAttachmentMap(const uint8_t *indices) {
    Metal4Invoke(WMTMetal4SetColorAttachmentMap, handle, Metal4CPUPointer(indices));
  }
  void encodeCommands(const wmtcmd_render_nop *commands) {
    Metal4Invoke(WMTMetal4RenderCommands, handle, commands ? WMTGetGuestWindowBase() + uintptr_t(commands) : 0);
  }
  void setArgumentTable(MTL4ArgumentTable table, WMTRenderStages stages) { Metal4Invoke(WMTMetal4RenderArgumentTable, handle, table.handle, uint64_t(stages)); }
  void setVertexBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    wmtcmd_render_setbuffer cmd{}; cmd.type=WMTRenderCommandSetVertexBuffer;
    cmd.buffer=buffer.handle; cmd.offset=offset; cmd.index=index;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setFragmentBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    wmtcmd_render_setbuffer cmd{}; cmd.type=WMTRenderCommandSetFragmentBuffer;
    cmd.buffer=buffer.handle; cmd.offset=offset; cmd.index=index;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setMeshBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    wmtcmd_render_setbuffer cmd{}; cmd.type=WMTRenderCommandSetMeshBuffer;
    cmd.buffer=buffer.handle; cmd.offset=offset; cmd.index=index;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setObjectBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    wmtcmd_render_setbuffer cmd{}; cmd.type=WMTRenderCommandSetObjectBuffer;
    cmd.buffer=buffer.handle; cmd.offset=offset; cmd.index=index;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void memoryBarrier(WMTBarrierScope scope, WMTRenderStages before, WMTRenderStages after) {
    wmtcmd_render_memory_barrier cmd{}; cmd.type=WMTRenderCommandMemoryBarrier;
    cmd.scope=scope; cmd.stages_before=before; cmd.stages_after=after;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void useResource(Resource resource, WMTResourceUsage usage, WMTRenderStages stages) {
    wmtcmd_render_useresource cmd{}; cmd.type=WMTRenderCommandUseResource;
    cmd.resource=resource.handle; cmd.usage=usage; cmd.stages=stages;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setRenderPipelineState(RenderPipelineState state) {
    wmtcmd_render_setpso cmd{}; cmd.type=WMTRenderCommandSetPSO; cmd.pso=state.handle;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setDepthStencilState(DepthStencilState state) {
    wmtcmd_render_setdepthstencilstate cmd{}; cmd.type=WMTRenderCommandSetDepthStencilState; cmd.depth_stencil_state=state.handle;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void drawPrimitives(WMTPrimitiveType primitive, uint32_t start, uint32_t count) {
    wmtcmd_render_draw cmd{}; cmd.type=WMTRenderCommandDraw; cmd.primitive_type=primitive;
    cmd.vertex_start=start; cmd.vertex_count=count; cmd.instance_count=1;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setFragmentTexture(Texture texture, uint8_t index) {
    wmtcmd_render_settexture cmd{}; cmd.type=WMTRenderCommandSetFragmentTexture; cmd.texture=texture.handle; cmd.index=index;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setFragmentBytes(const void *bytes, uint64_t length, uint8_t index) {
    wmtcmd_render_setbytes cmd{}; cmd.type=WMTRenderCommandSetFragmentBytes; cmd.bytes.set(const_cast<void*>(bytes)); cmd.length=length; cmd.index=index;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void setViewport(WMTViewport viewport) {
    wmtcmd_render_setviewport cmd{}; cmd.type=WMTRenderCommandSetViewport; cmd.viewport=viewport;
    encodeCommands(reinterpret_cast<const wmtcmd_render_nop*>(&cmd));
  }
  void waitForFence(Fence fence, WMTRenderStages before) { Metal4Invoke(WMTMetal4Fence, handle, fence.handle, 0, uint64_t(before)); }
  void updateFence(Fence fence, WMTRenderStages after) { Metal4Invoke(WMTMetal4Fence, handle, fence.handle, 1, uint64_t(after)); }
};

class MTL4CommandBuffer : public Object {
public:
  void begin(MTL4CommandAllocator allocator) { Metal4Invoke(WMTMetal4BeginCommandBuffer, handle, allocator.handle); }
  void end() { Metal4Invoke(WMTMetal4EndCommandBuffer, handle); }
  int32_t diagnostic(uint32_t action, const char *label, uint64_t length) {
    struct WMTMetal4Call call{}; call.operation = WMTMetal4Diagnostic;
    call.args[0] = handle; call.args[1] = action;
    call.args[2] = label ? WMTGetGuestWindowBase() + uintptr_t(label) : 0; call.args[3] = length;
    return WMTMetal4Call(&call);
  }
  void useResidencySet(ResidencySet set) { Metal4Invoke(WMTMetal4UseResidencySet, handle, set.handle); }
  // Sparse resources do not imply residency of the heaps backing their mapped pages.
  void useAllocation(Allocation allocation) { Metal4Invoke(WMTMetal4UseAllocation, handle, allocation.handle); }
  bool useDrawable(MetalDrawable drawable) {
    struct WMTMetal4Call call{}; call.operation = WMTMetal4UseDrawable;
    call.args[0] = handle; call.args[1] = drawable.handle;
    return ::WMTMetal4Call(&call) == 0;
  }
  MTL4RenderCommandEncoder renderCommandEncoder(WMTRenderPassInfo &info) {
    return MTL4RenderCommandEncoder{
      Metal4Invoke(WMTMetal4RenderEncoder, handle, WMTGetGuestWindowBase() + uintptr_t(&info)).ret[0]};
  }
  MTL4ComputeCommandEncoder computeCommandEncoder(bool concurrent = false, bool external_backing = false) {
    return MTL4ComputeCommandEncoder{Metal4Invoke(WMTMetal4ComputeEncoder, handle, uint64_t(concurrent),
                                                  uint64_t(external_backing)).ret[0]};
  }
  MTL4ComputeCommandEncoder blitCommandEncoder() { return computeCommandEncoder(); }
  MTL4ComputeCommandEncoder accelerationStructureCommandEncoder() { return computeCommandEncoder(); }
};

inline Reference<MTL4Submission> MTL4CommandQueue::commit(const MTL4CommandBuffer *buffers, uint64_t count) {
  return Reference<MTL4Submission>(
    Metal4Invoke(WMTMetal4Commit, handle, buffers ? WMTGetGuestWindowBase() + uintptr_t(buffers) : 0, count).ret[0]);
}
inline void MTL4CommandQueue::waitForEvent(Event event, uint64_t value) { Metal4Invoke(WMTMetal4WaitEvent, handle, event.handle, value); }
inline void MTL4CommandQueue::waitForDrawable(MetalDrawable drawable) { Metal4Invoke(WMTMetal4WaitDrawable, handle, drawable.handle); }
inline void MTL4CommandQueue::signalDrawable(MetalDrawable drawable) { Metal4Invoke(WMTMetal4SignalDrawable, handle, drawable.handle); }
inline bool Device::supportsMetal4() { return Metal4Invoke(WMTMetal4Supports, handle).ret[0] != 0; }
inline Reference<TextureViewPool> Device::newTextureViewPool(uint32_t count, Buffer argument_buffer) {
  return Reference<TextureViewPool>(Metal4Invoke(WMTMetal4NewTextureViewPool, handle, count, argument_buffer.handle).ret[0]);
}
inline Reference<MTL4CommandAllocator> Device::newMTL4CommandAllocator() { return Reference<MTL4CommandAllocator>(Metal4Invoke(WMTMetal4NewAllocator, handle).ret[0]); }
inline Reference<MTL4CommandBuffer> Device::newMTL4CommandBuffer() { return Reference<MTL4CommandBuffer>(Metal4Invoke(WMTMetal4NewCommandBuffer, handle).ret[0]); }
inline Reference<MTL4ArgumentTable> Device::newMTL4ArgumentTable(uint32_t buffers, uint32_t textures, uint32_t samplers, bool strides) {
  return Reference<MTL4ArgumentTable>(Metal4Invoke(WMTMetal4NewArgumentTable, handle, buffers, textures, samplers, uint64_t(strides)).ret[0]);
}
inline Reference<MTL4AccelerationStructureDescriptor> Device::newMTL4AccelerationStructureDescriptor(bool top_level, uint32_t geometry_count) {
  return Reference<MTL4AccelerationStructureDescriptor>(Metal4Invoke(WMTMetal4NewASDescriptor, handle, uint64_t(top_level), geometry_count).ret[0]);
}
inline WMTAccelerationStructureSizes Device::accelerationStructureSizes(MTL4AccelerationStructureDescriptor descriptor) {
  const auto call=Metal4Invoke(WMTMetal4ASSizes, handle, descriptor.handle);
  return WMTAccelerationStructureSizes{call.ret[0],call.ret[1],call.ret[2]};
}

} // namespace WMT
