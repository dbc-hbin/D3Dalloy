#pragma once

#include "./winemetal.h"
#include <cstddef>
#include <string>
#include <cstring>
#include <vector>

namespace WMT {

class String;

class Object {
public:
  obj_handle_t handle;

  Object() {
    handle = NULL_OBJECT_HANDLE;
  }

  Object(obj_handle_t h) {
    handle = h;
  }

  void
  retain() {
    NSObject_retain(handle);
  }

  void
  release() {
    NSObject_release(handle);
  }

  String description();

  bool
  operator==(std::nullptr_t) const {
    return handle == 0;
  }
  bool
  operator!=(std::nullptr_t) const {
    return handle != 0;
  }
  bool
  operator==(Object obj) const {
    return handle == obj.handle;
  }
  bool
  operator!=(Object obj) const {
    return handle != obj.handle;
  }

  operator bool() const {
    return handle != 0;
  }

  operator obj_handle_t() const {
    return handle;
  }
};

template <typename Class> class Reference : public Class {
public:
  Reference() {
    this->handle = NULL_OBJECT_HANDLE;
  }
  Reference(const Reference &copy) {
    this->handle = copy.handle;
    this->retain();
  }
  Reference(Reference &&move) {
    this->handle = move.handle;
    move.handle = NULL_OBJECT_HANDLE;
  }
  Reference(Class non_retained) {
    this->handle = non_retained.handle;
    this->retain();
  }
  Reference(obj_handle_t retained) {
    this->handle = retained;
  }

  Reference &
  operator=(const Reference &copy) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = copy.handle;
    this->retain();
    return *this;
  }
  Reference &
  operator=(Class non_retained) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = non_retained.handle;
    this->retain();
    return *this;
  }
  Reference &
  operator=(Reference &&move) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = move.handle;
    move.handle = NULL_OBJECT_HANDLE;
    return *this;
  }
  Reference &
  operator=(std::nullptr_t) {
    if (this->handle != NULL_OBJECT_HANDLE)
      this->release();
    this->handle = NULL_OBJECT_HANDLE;
    return *this;
  }

  ~Reference() {
    if (this->handle == NULL_OBJECT_HANDLE)
      return;
    this->release();
    this->handle = NULL_OBJECT_HANDLE;
  }
};

template <typename Class> class Array : public Object {
public:
  uint64_t
  count() const {
    return NSArray_count(handle);
  }

  Class
  object(uint64_t index) const {
    return Class{NSArray_object(handle, index)};
  }
};

class String : public Object {
public:
  static String
  string(const char *data, WMTStringEncoding encoding) {
    return String{NSString_string(data, encoding)};
  }

  bool
  getCString(char *buffer, uint64_t maxLength, WMTStringEncoding encoding) {
    return NSString_getCString(handle, buffer, maxLength, encoding);
  }

  uint64_t
  lengthOfBytesUsingEncoding(WMTStringEncoding encoding) {
    return NSString_lengthOfBytesUsingEncoding(handle, encoding);
  }

  std::string
  getUTF8String() {
    auto len = lengthOfBytesUsingEncoding(WMTUTF8StringEncoding);
    std::vector<char> str(len + 1);
    getCString(str.data(), len + 1, WMTUTF8StringEncoding);
    return std::string(str.data());
  }
};

inline String
Object::description() {
  return String{NSObject_description(handle)};
}

class Error : public Object {
public:
};

class DispatchData : public Object {
public:
};

class Event : public Object {
public:
};

class SharedEvent : public Event {
public:
  uint64_t
  signaledValue() {
    return MTLSharedEvent_signaledValue(handle);
  }

  void
  signalValue(uint64_t value) {
    MTLSharedEvent_signalValue(handle, value);
  }

  mach_port_t
  createMachPort() {
    return MTLSharedEvent_createMachPort(handle);
  }

  bool
  waitUntilSignaledValue(uint64_t value, uint64_t timeout) {
    return MTLSharedEvent_waitUntilSignaledValue(handle, value, timeout);
  }
};

class Fence : public Object {
public:
};

class CounterSampleBuffer : public Object {
public:
  void
  resolveCounterRange(uint32_t start, uint32_t len, void *data_out, uint64_t data_length) {
    MTLCounterSampleBuffer_resolveCounterRange(handle, start, len, data_out, data_length);
  }
};

class Allocation : public Object {
public:
  uint64_t
  allocatedSize() const {
    return MTLAllocation_allocatedSize(handle);
  }
};

class Resource : public Allocation {
public:
};

class Texture : public Resource {
public:
  WMTSparseTextureTail
  sparseTail() const {
    return MTLTexture_sparseTail(handle);
  }

  Reference<Texture>
  newTextureView(
      WMTPixelFormat format, WMTTextureType texture_type, uint16_t level_start, uint16_t level_count,
      uint16_t slice_start, uint16_t slice_count, struct WMTTextureSwizzleChannels swizzle,
      uint64_t &out_gpu_resource_id
  ) {
    return Reference<Texture>(MTLTexture_newTextureView(
        handle, format, texture_type, level_start, level_count, slice_start, slice_count, swizzle, &out_gpu_resource_id
    ));
  }

  WMTPixelFormat
  pixelFormat() {
    return MTLTexture_pixelFormat(handle);
  }

  uint64_t
  width() {
    return MTLTexture_width(handle);
  }

  uint64_t
  height() {
    return MTLTexture_height(handle);
  }

  uint64_t
  depth() {
    return MTLTexture_depth(handle);
  }

  uint64_t
  arrayLength() {
    return MTLTexture_arrayLength(handle);
  }

  uint64_t
  mipmapLevelCount() {
    return MTLTexture_mipmapLevelCount(handle);
  }

  void
  replaceRegion(
      WMTOrigin origin, WMTSize size, uint64_t level, uint64_t slice, const void *pixelBytes, uint64_t bytesPerRow,
      uint64_t bytesPerImage
  ) {
    WMTMemoryPointer data;
    data.set((void *)pixelBytes);
    return MTLTexture_replaceRegion(handle, origin, size, level, slice, data, bytesPerRow, bytesPerImage);
  }

  void
  getBytes(
      WMTOrigin origin, WMTSize size, uint64_t level, uint64_t slice, void *pixelBytes, uint64_t bytesPerRow,
      uint64_t bytesPerImage
  ) {
    WMTMemoryPointer data;
    data.set((void *)pixelBytes);
    return MTLTexture_getBytes(handle, origin, size, level, slice, data, bytesPerRow, bytesPerImage);
  }
};

class Buffer : public Resource {
public:
  Reference<Texture>
  newTexture(WMTTextureInfo &info, uint64_t offset, uint64_t bytes_per_row) {
    return Reference<Texture>(MTLBuffer_newTexture(handle, &info, offset, bytes_per_row));
  }

  Reference<Texture>
  newPlacementSparseTextureView(
      WMTTextureInfo &info, uint64_t offset, uint64_t bytes_per_row, WMTSparsePageSize page_size
  ) {
    return Reference<Texture>(MTLBuffer_newPlacementSparseTextureView(handle, &info, offset, bytes_per_row, page_size));
  }

  void
  didModifyRange(uint64_t start, uint64_t length) {
    MTLBuffer_didModifyRange(handle, start, length);
  }

  void
  updateContents(uint64_t offset, const void *data, uint64_t length) {
    WMTConstMemoryPointer ptr;
    ptr.set(data);
    MTLBuffer_updateContents(handle, offset, ptr, length);
  }
};

class AccelerationStructureDescriptor : public Object {
public:
  void
  setUsage(uint32_t usage) {
    WMTASDescriptor_setUsage(handle, usage);
  }

  bool
  setTriangles(
      uint32_t index, Buffer vertices, uint64_t vertex_offset, uint64_t vertex_stride, Buffer indices,
      uint64_t index_offset, uint32_t index_type, Buffer transform, uint64_t transform_offset,
      uint64_t triangle_count, uint32_t intersection_function_table_offset, uint32_t geometry_flags
  ) {
    return WMTASDescriptor_setTriangles(
        handle, index, vertices, vertex_offset, vertex_stride, indices, index_offset, index_type,
        transform, transform_offset, triangle_count, intersection_function_table_offset, geometry_flags
    );
  }

  bool
  setAABBs(
      uint32_t index, Buffer boxes, uint64_t box_offset, uint64_t box_stride, uint64_t box_count,
      uint32_t intersection_function_table_offset, uint32_t geometry_flags
  ) {
    return WMTASDescriptor_setAABBs(
        handle, index, boxes, box_offset, box_stride, box_count, intersection_function_table_offset,
        geometry_flags
    );
  }

  bool
  setInstances(Buffer instances, uint64_t offset, uint64_t count) {
    return WMTASDescriptor_setInstances(handle, instances, offset, count);
  }
};

class AccelerationStructureSidecar : public Object {
public:
  /**
   * The MTLAccelerationStructure this sidecar owns, as an allocation so callers can
   * register it in a residency set: Metal acceleration structures are resources but
   * not buffers, so a buffer residency registration does not cover them.
   */
  Allocation
  accelerationStructure() const {
    return Allocation{WMTASSidecar_accelerationStructure(handle)};
  }

  Buffer
  instanceBuffer() const {
    return Buffer{WMTASSidecar_instanceBuffer(handle)};
  }

  Buffer
  contributionBuffer() const {
    return Buffer{WMTASSidecar_contributionBuffer(handle)};
  }

  uint64_t
  resourceID() const {
    return WMTASSidecar_resourceID(handle);
  }

  uint64_t
  contributionGPUAddress() {
    return WMTASSidecar_contributionGPUAddress(handle);
  }

  uint64_t
  size() {
    return WMTASSidecar_size(handle);
  }
};

class Heap : public Allocation {
public:
  Reference<Buffer>
  newBuffer(WMTBufferInfo &info, uint64_t offset = ~0ull) {
    return Reference<Buffer>(MTLHeap_newBuffer(handle, &info, offset));
  }

  Reference<Texture>
  newTexture(WMTTextureInfo &info, uint64_t offset = ~0ull) {
    return Reference<Texture>(MTLHeap_newTexture(handle, &info, offset));
  }
};

class SamplerState : public Object {
public:
};

class DepthStencilState : public Object {
public:
};

class CommandEncoder : public Object {
public:
  void
  endEncoding() {
    MTLCommandEncoder_endEncoding(handle);
  }

  void
  setLabel(String label) {
    MTLCommandEncoder_setLabel(handle, label);
  }
};

class FunctionHandle : public Object {};

class VisibleFunctionTable : public Object {
public:
  void
  setFunction(FunctionHandle function, uint64_t index) {
    MTLVisibleFunctionTable_setFunction(handle, function, index);
  }

  uint64_t
  gpuResourceID() {
    return MTLResource_gpuResourceID(handle);
  }
};

class IntersectionFunctionTable : public Object {
public:
  void
  setFunction(FunctionHandle function, uint64_t index) {
    MTLIntersectionFunctionTable_setFunction(handle, function, index);
  }

  /**
   * One slot of the table's *own* buffer argument table.
   *
   * An intersection function installed in the table reads `[[buffer(N)]]` from this table, not
   * from the compute encoder's argument table: the encoder's bindings are not visible to it. So a
   * callback that needs the dispatch constants, the root arguments or the local root table has to
   * have them bound here.
   */
  void
  setBuffer(Buffer buffer, uint64_t offset, uint64_t index) {
    MTLIntersectionFunctionTable_setBuffer(handle, buffer, offset, index);
  }

  /** A visible function table in that same argument table, for a callback that calls through one. */
  void
  setVisibleFunctionTable(VisibleFunctionTable table, uint64_t index) {
    MTLIntersectionFunctionTable_setVisibleFunctionTable(handle, table, index);
  }

  uint64_t
  gpuResourceID() {
    return MTLResource_gpuResourceID(handle);
  }
};

class ComputePipelineState : public Object {
public:
  Reference<VisibleFunctionTable>
  newVisibleFunctionTable(uint32_t count) {
    return Reference<VisibleFunctionTable>(MTLComputePipelineState_newVisibleFunctionTable(handle, count));
  }

  Reference<IntersectionFunctionTable>
  newIntersectionFunctionTable(uint32_t count) {
    return Reference<IntersectionFunctionTable>(
        MTLComputePipelineState_newIntersectionFunctionTable(handle, count)
    );
  }

  Reference<FunctionHandle>
  functionHandle(obj_handle_t function) {
    return Reference<FunctionHandle>(MTLComputePipelineState_functionHandle(handle, function));
  }

  uint64_t
  maxTotalThreadsPerThreadgroup() {
    return MTLComputePipelineState_maxTotalThreadsPerThreadgroup(handle);
  }
};

class RenderPipelineState : public Object {
public:
};

class RenderCommandEncoder : public CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_render_nop *cmd_head) {
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)cmd_head);
  }

  void
  setVertexBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetVertexBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setFragmentBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetFragmentBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setMeshBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetMeshBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setObjectBuffer(Buffer buffer, uint64_t offset, uint8_t index) {
    struct wmtcmd_render_setbuffer cmd;
    cmd.type = WMTRenderCommandSetObjectBuffer;
    cmd.next.set(nullptr);
    cmd.buffer = buffer;
    cmd.offset = offset;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  memoryBarrier(WMTBarrierScope scope, WMTRenderStages stages_after, WMTRenderStages stages_before) {
    struct wmtcmd_render_memory_barrier cmd;
    cmd.type = WMTRenderCommandMemoryBarrier;
    cmd.next.set(nullptr);
    cmd.scope = scope;
    cmd.stages_before = stages_before;
    cmd.stages_after = stages_after;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  useResource(Resource resource, WMTResourceUsage usage, WMTRenderStages stages) {
    struct wmtcmd_render_useresource cmd;
    cmd.type = WMTRenderCommandUseResource;
    cmd.next.set(nullptr);
    cmd.resource = resource;
    cmd.usage = usage;
    cmd.stages = stages;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  };

  void
  setRenderPipelineState(RenderPipelineState pso) {
    struct wmtcmd_render_setpso cmd;
    cmd.type = WMTRenderCommandSetPSO;
    cmd.next.set(nullptr);
    cmd.pso = pso;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setDepthStencilState(DepthStencilState state) {
    struct wmtcmd_render_setdepthstencilstate cmd;
    cmd.type = WMTRenderCommandSetDepthStencilState;
    cmd.next.set(nullptr);
    cmd.depth_stencil_state = state;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  drawPrimitives(WMTPrimitiveType primitive_type, uint32_t vertex_start, uint32_t vertex_count) {
    struct wmtcmd_render_draw cmd;
    cmd.type = WMTRenderCommandDraw;
    cmd.next.set(nullptr);
    cmd.primitive_type = primitive_type;
    cmd.vertex_start = vertex_start;
    cmd.vertex_count = vertex_count;
    cmd.base_instance = 0;
    cmd.instance_count = 1;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setFragmentTexture(Texture texture, uint8_t index) {
    struct wmtcmd_render_settexture cmd;
    cmd.type = WMTRenderCommandSetFragmentTexture;
    cmd.next.set(nullptr);
    cmd.texture = texture;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  };

  void
  setFragmentBytes(const void *buf, uint64_t length, uint8_t index) {
    struct wmtcmd_render_setbytes cmd;
    cmd.type = WMTRenderCommandSetFragmentBytes;
    cmd.next.set(nullptr);
    cmd.bytes.set((void *)buf);
    cmd.length = length;
    cmd.index = index;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setViewport(WMTViewport viewport) {
    struct wmtcmd_render_setviewports cmd;
    cmd.type = WMTRenderCommandSetViewports;
    cmd.next.set(nullptr);
    cmd.viewports.set(&viewport);
    cmd.viewport_count = 1;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  };

  void
  waitForFence(Fence fence, WMTRenderStages before) {
    struct wmtcmd_render_fence_op cmd;
    cmd.type = WMTRenderCommandWaitForFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    cmd.stages = before;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  updateFence(Fence fence, WMTRenderStages after) {
    struct wmtcmd_render_fence_op cmd;
    cmd.type = WMTRenderCommandUpdateFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    cmd.stages = after;
    MTLRenderCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }
};

class BlitCommandEncoder : public CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_blit_nop *cmd_head) {
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)cmd_head);
  }

  void
  waitForFence(Fence fence) {
    struct wmtcmd_blit_fence_op cmd;
    cmd.type = WMTBlitCommandWaitForFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  updateFence(Fence fence) {
    struct wmtcmd_blit_fence_op cmd;
    cmd.type = WMTBlitCommandUpdateFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  resolveCounters(CounterSampleBuffer sample_buffer, uint32_t start, uint32_t len, Buffer dst, uint64_t dst_offset) {
    struct wmtcmd_blit_resolvecounters cmd;
    cmd.type = WMTBlitCommandResolveCounters;
    cmd.next.set(nullptr);
    cmd.sample_buffer = sample_buffer.handle;
    cmd.start = start;
    cmd.len = len;
    cmd.dst_buffer = dst.handle;
    cmd.dst_offset = dst_offset;
    MTLBlitCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }
};

class ComputeCommandEncoder : public CommandEncoder {
public:
  void
  encodeCommands(const wmtcmd_compute_nop *cmd_head) {
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)cmd_head);
  }

  void
  waitForFence(Fence fence) {
    struct wmtcmd_compute_fence_op cmd;
    cmd.type = WMTComputeCommandWaitForFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  updateFence(Fence fence) {
    struct wmtcmd_compute_fence_op cmd;
    cmd.type = WMTComputeCommandUpdateFence;
    cmd.next.set(nullptr);
    cmd.fence = fence.handle;
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setVisibleFunctionTable(VisibleFunctionTable table, uint64_t index) {
    struct wmtcmd_compute_setfunctiontable cmd;
    cmd.type = WMTComputeCommandSetVisibleFunctionTable;
    cmd.next.set(nullptr);
    cmd.table = table.handle;
    cmd.index = index;
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setIntersectionFunctionTable(IntersectionFunctionTable table, uint64_t index) {
    struct wmtcmd_compute_setfunctiontable cmd;
    cmd.type = WMTComputeCommandSetIntersectionFunctionTable;
    cmd.next.set(nullptr);
    cmd.table = table.handle;
    cmd.index = index;
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  void
  setAccelerationStructure(Object acceleration_structure, uint64_t index) {
    struct wmtcmd_compute_setaccelerationstructure cmd;
    cmd.type = WMTComputeCommandSetAccelerationStructure;
    cmd.next.set(nullptr);
    cmd.acceleration_structure = acceleration_structure.handle;
    cmd.index = index;
    MTLComputeCommandEncoder_encodeCommands(handle, (const wmtcmd_base *)&cmd);
  }

  /* Acceleration-structure helpers run on the application's own command buffer, so they are
   * encoded directly instead of travelling through the command ring. */
  bool
  encodeASPrepass(
      ComputePipelineState pipeline, Buffer source_instances, uint64_t source_offset,
      Buffer destination_instances, uint64_t destination_offset, Buffer header, uint64_t header_offset,
      Buffer contributions, uint64_t contribution_offset, Buffer live_mirror, uint64_t live_mirror_offset,
      const WMTASPrepassInfo &info
  ) {
    return WMTComputeEncoder_encodeASPrepass(
        handle, pipeline, source_instances, source_offset, destination_instances, destination_offset,
        header, header_offset, contributions, contribution_offset, live_mirror, live_mirror_offset, &info
    );
  }

  bool
  encodeASWriteU64(ComputePipelineState pipeline, Buffer destination, uint64_t offset, uint64_t value) {
    return WMTComputeEncoder_encodeASWriteU64(handle, pipeline, destination, offset, value);
  }

  bool
  encodeASFinalizeCompactedSize(ComputePipelineState pipeline, Buffer destination, uint64_t offset) {
    return WMTComputeEncoder_encodeASFinalizeCompactedSize(handle, pipeline, destination, offset);
  }
};

class AccelerationStructureCommandEncoder : public CommandEncoder {
public:
  void waitForFence(Fence fence) {
    WMTASEncoder_fence(handle, fence, false);
  }

  void updateFence(Fence fence) {
    WMTASEncoder_fence(handle, fence, true);
  }

  bool
  build(
      AccelerationStructureDescriptor descriptor, AccelerationStructureSidecar destination, Buffer scratch,
      uint64_t scratch_offset
  ) {
    return WMTASEncoder_build(handle, descriptor, destination, scratch, scratch_offset);
  }

  bool
  refit(
      AccelerationStructureDescriptor descriptor, AccelerationStructureSidecar source,
      AccelerationStructureSidecar destination, Buffer scratch, uint64_t scratch_offset
  ) {
    return WMTASEncoder_refit(handle, descriptor, source, destination, scratch, scratch_offset);
  }

  bool
  copy(AccelerationStructureSidecar source, AccelerationStructureSidecar destination, uint32_t mode) {
    return WMTASEncoder_copy(handle, source, destination, mode);
  }

  void
  compactedSize(AccelerationStructureSidecar source, Buffer destination, uint64_t offset) {
    WMTASEncoder_compactedSize(handle, source, destination, offset);
  }
};

class PresentationObserver : public Object {
public:
  int32_t wait(WMTPresentationEvent &event) const {
    return WMTPresentationObserver_wait(handle, &event);
  }

  void cancel(uint64_t present_id) const {
    WMTPresentationObserver_cancel(handle, present_id);
  }

  void close() const {
    WMTPresentationObserver_close(handle);
  }

  bool setView(obj_handle_t view) const {
    return WMTPresentationObserver_setView(handle, view);
  }

  bool getRefreshSample(WMTDisplayRefreshSample &sample) const {
    return WMTPresentationObserver_getRefreshSample(handle, &sample);
  }
};

class MetalDrawable : public Object {
public:
  bool observePresentation(PresentationObserver observer, uint64_t present_id) const {
    return MetalDrawable_observePresentation(handle, observer, present_id);
  }

  Texture
  texture() {
    return Texture{MetalDrawable_texture(handle)};
  };
};

class MetalLayer : public Object {
public:
  MetalDrawable
  nextDrawable() {
    return MetalDrawable{MetalLayer_nextDrawable(handle)};
  };

  void
  setProps(const WMTLayerProps &props) {
    MetalLayer_setProps(handle, &props);
  };

  void
  getProps(WMTLayerProps &props) {
    MetalLayer_getProps(handle, &props);
  };

  bool
  setColorSpace(WMTColorSpace colorspace) {
    return MetalLayer_setColorSpace(handle, colorspace);
  };
};

class FXTemporalScaler : public Object {
public:
};

class FXSpatialScaler : public Object {
public:
};

class LogContainer : public Object {
public:
  std::vector<Object>
  elements() {
    std::vector<Object> ret;
    uint64_t read = 0;
    uint64_t start;
    do {
      start = ret.size();
      ret.resize(start + 4);
      read = MTLLogContainer_enumerate(handle, start, 4, (obj_handle_t *)ret.data() + start);
    } while (read == 4);
    ret.resize(start + read);
    return ret;
  };
};

class ResidencySet : public Object {
public:
  void
  addAllocations(const Allocation *allocations, uint64_t count) {
    MTLResidencySet_addAllocations(handle, reinterpret_cast<const obj_handle_t *>(allocations), count);
  }

  void
  removeAllocations(const Allocation *allocations, uint64_t count) {
    MTLResidencySet_removeAllocations(handle, reinterpret_cast<const obj_handle_t *>(allocations), count);
  }

  void
  removeAllAllocations() {
    MTLResidencySet_removeAllAllocations(handle);
  }

  void
  commit() {
    MTLResidencySet_commit(handle);
  }

  uint64_t
  allocatedSize() const {
    return MTLResidencySet_allocatedSize(handle);
  }
};

class CommandBuffer : public Object {
public:
  void
  commit() {
    return MTLCommandBuffer_commit(handle);
  }

  void
  waitUntilCompleted() {
    return MTLCommandBuffer_waitUntilCompleted(handle);
  }

  WMTCommandBufferStatus
  status() {
    return MTLCommandBuffer_status(handle);
  }

  Error
  error() {
    return Error{MTLCommandBuffer_error(handle)};
  }

  LogContainer
  logs() {
    return LogContainer{MTLCommandBuffer_logs(handle)};
  }

  uint64_t
  gpuEndTime() {
    return MTLCommandBuffer_property(handle, WMTCommandBufferPropertyGPUEndTime);
  }

  void
  encodeSignalEvent(Event event, uint64_t value) {
    return MTLCommandBuffer_encodeSignalEvent(handle, event.handle, value);
  }

  void
  encodeWaitForEvent(Event event, uint64_t value) {
    return MTLCommandBuffer_encodeWaitForEvent(handle, event.handle, value);
  }

  RenderCommandEncoder
  renderCommandEncoder(WMTRenderPassInfo &info) {
    return RenderCommandEncoder{MTLCommandBuffer_renderCommandEncoder(handle, &info)};
  }

  BlitCommandEncoder
  blitCommandEncoder() {
    return BlitCommandEncoder{MTLCommandBuffer_blitCommandEncoder(handle)};
  }

  BlitCommandEncoder
  blitCommandEncoderWithSampleBuffers(WMTSampleBufferAttachmentInfo *attachments, uint64_t num_attachments) {
    return BlitCommandEncoder{MTLCommandBuffer_blitCommandEncoderWithSampleBuffers(handle, attachments, num_attachments)};
  }

  ComputeCommandEncoder
  computeCommandEncoder(bool concurrent) {
    return ComputeCommandEncoder{MTLCommandBuffer_computeCommandEncoder(handle, concurrent)};
  }

  AccelerationStructureCommandEncoder
  accelerationStructureCommandEncoder() {
    return AccelerationStructureCommandEncoder{WMTCommandBuffer_accelerationStructureCommandEncoder(handle)};
  }

  void
  presentDrawable(MetalDrawable drawable) {
    MTLCommandBuffer_presentDrawable(handle, drawable);
  }

  void
  presentDrawableAfterMinimumDuration(MetalDrawable drawable, double after) {
    MTLCommandBuffer_presentDrawableAfterMinimumDuration(handle, drawable, after);
  }

  void
  presentDrawableIfCurrent(MetalDrawable drawable, SharedEvent token, uint64_t epoch, double after,
                           PresentationObserver observer, uint64_t present_id) {
    MTLCommandBuffer_presentDrawableIfCurrent(handle, drawable, token, epoch, after, observer, present_id);
  }

  void
  encodeSpatialScale(FXSpatialScaler scaler, Texture color, Texture output, Fence fence) {
    MTLCommandBuffer_encodeSpatialScale(handle, scaler, color, output, fence);
  }

  void
  encodeTemporalScale(
      FXTemporalScaler scaler, Texture color, Texture output, Texture depth, Texture motion, Texture exposure,
      Fence fence, const WMTFXTemporalScalerProps &props
  ) {
    MTLCommandBuffer_encodeTemporalScale(handle, scaler, color, output, depth, motion, exposure, fence, &props);
  }
};

/* A Metal 4 command queue, used here only to apply sparse mapping updates. */
class MTL4CommandQueue : public Object {
public:
  void
  addResidencySet(ResidencySet residency_set) {
    MTL4CommandQueue_addResidencySet(handle, residency_set.handle);
  }

  void
  updateBufferMappings(Buffer buffer, Heap heap, const WMTSparseBufferMapping *mappings, uint64_t count) {
    MTL4CommandQueue_updateBufferMappings(handle, buffer.handle, heap.handle, mappings, count);
  }

  void
  updateTextureMappings(Texture texture, Heap heap, const WMTSparseTextureMapping *mappings, uint64_t count) {
    MTL4CommandQueue_updateTextureMappings(handle, texture.handle, heap.handle, mappings, count);
  }

  void
  signalEvent(Event event, uint64_t value) {
    MTL4CommandQueue_signalEvent(handle, event.handle, value);
  }
};

class CommandQueue : public Object {
public:
  CommandBuffer
  commandBuffer() {
    return CommandBuffer{MTLCommandQueue_commandBuffer(handle)};
  }

  void
  addResidencySet(ResidencySet residency_set) {
    MTLCommandQueue_addResidencySet(handle, residency_set.handle);
  }
};

class Function : public Object {};

class Library : public Object {
public:
  Reference<Function>
  newFunction(const char *name) {
    return Reference<Function>(MTLLibrary_newFunction(handle, name));
  }

  Reference<Function>
  newFunctionWithConstants(
      const char *name, const WMTFunctionConstant *constants, uint32_t num_constants, Error &error
  ) {
    return Reference<Function>(
        MTLLibrary_newFunctionWithConstants(handle, name, constants, num_constants, &error.handle)
    );
  }
};

class BinaryArchive : public Object {
public:
  void
  serialize(const char *url, Error &error) {
    MTLBinaryArchive_serialize(handle, url, &error.handle);
  }
};

class IndirectCommandBuffer : public Resource {
public:
};

class Device : public Object {
public:
  uint64_t
  recommendedMaxWorkingSetSize() const {
    return MTLDevice_recommendedMaxWorkingSetSize(handle);
  }

  uint64_t
  maxBufferLength() const {
    return MTLDevice_maxBufferLength(handle);
  }

  void
  sampleTimestamps(uint64_t *cpu_timestamp, uint64_t *gpu_timestamp) const {
    MTLDevice_sampleTimestamps(handle, cpu_timestamp, gpu_timestamp);
  }

  uint64_t
  currentAllocatedSize() const {
    return MTLDevice_currentAllocatedSize(handle);
  }

  String
  name() const {
    return String{MTLDevice_name(handle)};
  }

  uint64_t
  registryID() const {
    return MTLDevice_registryID(handle);
  }

  Reference<CommandQueue>
  newCommandQueue(uint64_t maxCommandBufferCount) {
    return Reference<CommandQueue>(MTLDevice_newCommandQueue(handle, maxCommandBufferCount));
  }

  Reference<SharedEvent>
  newSharedEvent() {
    return Reference<SharedEvent>(MTLDevice_newSharedEvent(handle));
  }

  Reference<SharedEvent>
  newSharedEventWithMachPort(mach_port_t mach_port) {
    return Reference<SharedEvent>(MTLDevice_newSharedEventWithMachPort(handle, mach_port));
  }

  Reference<Buffer>
  newBuffer(WMTBufferInfo &info) {
    return Reference<Buffer>(MTLDevice_newBuffer(handle, &info));
  }

  Reference<SamplerState>
  newSamplerState(WMTSamplerInfo &info) {
    return Reference<SamplerState>(MTLDevice_newSamplerState(handle, &info));
  }

  Reference<DepthStencilState>
  newDepthStencilState(const WMTDepthStencilInfo &info) {
    return Reference<DepthStencilState>(MTLDevice_newDepthStencilState(handle, &info));
  }

  Reference<Texture>
  newTexture(WMTTextureInfo &info) {
    return Reference<Texture>(MTLDevice_newTexture(handle, &info));
  }

  Reference<Library>
  newLibrary(const void *bytecode, uint64_t bytecode_length, Error &error) {
    auto data = DispatchData_alloc_init((uint64_t)bytecode, bytecode_length);
    auto ret = Reference<Library>(MTLDevice_newLibrary(handle, data, &error.handle));
    NSObject_release(data);
    return ret;
  }

  Reference<Library>
  newLibraryFromNativeBuffer(uint64_t bytecode, uint64_t bytecode_length, Error &error) {
    auto data = DispatchData_alloc_init(bytecode, bytecode_length);
    auto ret = Reference<Library>(MTLDevice_newLibrary(handle, data, &error.handle));
    NSObject_release(data);
    return ret;
  }

  Reference<Library>
  newLibrary(DispatchData data, Error &error) {
    return Reference<Library>(MTLDevice_newLibrary(handle, data, &error.handle));
  }

  Reference<Library>
  newLibraryWithSource(std::string_view view, Error &error) {
    return Reference<Library>(MTLDevice_newLibraryWithSource(handle, view.data(), view.length(), &error.handle));
  }

  Reference<ComputePipelineState>
  newComputePipelineState(const Function &compute_function, Error &error) {
    WMTComputePipelineInfo info{};
    info.compute_function = compute_function;
    return Reference<ComputePipelineState>(MTLDevice_newComputePipelineState(handle, &info, &error.handle));
  }

  Reference<ComputePipelineState>
  newComputePipelineState(const Function &compute_function, bool tgsize_is_multiple_of_sgwidth, Error &error) {
    WMTComputePipelineInfo info{};
    info.compute_function = compute_function;
    info.tgsize_is_multiple_of_sgwidth = tgsize_is_multiple_of_sgwidth;
    return Reference<ComputePipelineState>(MTLDevice_newComputePipelineState(handle, &info, &error.handle));
  }

  Reference<RenderPipelineState>
  newRenderPipelineState(const WMTRenderPipelineInfo &info, Error &error) {
    return Reference<RenderPipelineState>(MTLDevice_newRenderPipelineState(handle, &info, &error.handle));
  }

  Reference<ComputePipelineState>
  newComputePipelineState(const WMTComputePipelineInfo &info, Error &error) {
    return Reference<ComputePipelineState>(MTLDevice_newComputePipelineState(handle, &info, &error.handle));
  }

  Reference<ComputePipelineState>
  newComputePipelineStateWithLinkedFunctions(
      const WMTComputePipelineInfo &info, const obj_handle_t *linked_functions,
      uint32_t linked_function_count, Error &error
  ) {
    return Reference<ComputePipelineState>(MTLDevice_newComputePipelineStateWithLinkedFunctions(
        handle, &info, linked_functions, linked_function_count, &error.handle
    ));
  }

  Reference<AccelerationStructureDescriptor>
  newAccelerationStructureDescriptor(bool top_level, uint32_t geometry_count) {
    return Reference<AccelerationStructureDescriptor>(WMTASDescriptor_new(top_level, geometry_count));
  }

  WMTAccelerationStructureSizes
  accelerationStructureSizes(AccelerationStructureDescriptor descriptor) {
    WMTAccelerationStructureSizes sizes{};
    WMTDevice_getASSizes(handle, descriptor, &sizes);
    return sizes;
  }

  Reference<AccelerationStructureSidecar>
  newAccelerationStructureSidecar(uint64_t native_size, uint64_t instance_count) {
    return Reference<AccelerationStructureSidecar>(WMTASSidecar_new(handle, native_size, instance_count));
  }

  Reference<RenderPipelineState>
  newRenderPipelineState(const WMTMeshRenderPipelineInfo &info, Error &error) {
    return Reference<RenderPipelineState>(MTLDevice_newMeshRenderPipelineState(handle, &info, &error.handle));
  }

  Reference<RenderPipelineState>
  newRenderPipelineState(const WMTTileRenderPipelineInfo &info, Error &error) {
    return Reference<RenderPipelineState>(MTLDevice_newTileRenderPipelineState(handle, &info, &error.handle));
  }

  Reference<Fence>
  newFence() {
    return Reference<Fence>(MTLDevice_newFence(handle));
  }

  Reference<Event>
  newEvent() {
    return Reference<Event>(MTLDevice_newEvent(handle));
  }

  Reference<ResidencySet>
  newResidencySet(uint64_t init_capacity, Error &error) {
    return Reference<ResidencySet>(MTLDevice_newResidencySet(handle, init_capacity, &error.handle));
  }

  Reference<Heap>
  newHeap(const WMTHeapInfo &info) {
    return Reference<Heap>(MTLDevice_newHeap(handle, &info));
  }

  bool
  supportsPlacementSparse() const {
    return MTLDevice_supportsPlacementSparse(handle);
  }

  Reference<Heap>
  newPlacementHeap(const WMTHeapInfo &info, WMTSparsePageSize max_sparse_page_size) {
    return Reference<Heap>(MTLDevice_newPlacementHeap(handle, &info, max_sparse_page_size));
  }

  Reference<Buffer>
  newPlacementSparseBuffer(WMTBufferInfo &info, WMTSparsePageSize page_size) {
    return Reference<Buffer>(MTLDevice_newPlacementSparseBuffer(handle, &info, page_size));
  }

  Reference<Texture>
  newPlacementSparseTexture(WMTTextureInfo &info, WMTSparsePageSize page_size) {
    return Reference<Texture>(MTLDevice_newPlacementSparseTexture(handle, &info, page_size));
  }

  WMTSparseTileShape
  sparseTileSize(WMTTextureType type, WMTPixelFormat format, uint32_t sample_count, WMTSparsePageSize page_size) {
    return MTLDevice_sparseTileSize(handle, type, format, sample_count, page_size);
  }

  Reference<MTL4CommandQueue>
  newMTL4CommandQueue() {
    return Reference<MTL4CommandQueue>(MTLDevice_newMTL4CommandQueue(handle));
  }

  Reference<SamplerState>
  newSamplerStateWithReduction(WMTSamplerInfo &info, uint32_t reduction) {
    return Reference<SamplerState>(MTLDevice_newSamplerStateWithReduction(handle, &info, reduction));
  }

  WMTSizeAndAlign
  heapBufferSizeAndAlign(uint64_t length, WMTResourceOptions options) {
    return MTLDevice_heapBufferSizeAndAlign(handle, length, options);
  }

  WMTSizeAndAlign
  heapTextureSizeAndAlign(WMTTextureInfo &info) {
    return MTLDevice_heapTextureSizeAndAlign(handle, &info);
  }

  uint64_t
  minimumLinearTextureAlignmentForPixelFormat(WMTPixelFormat format) {
    return MTLDevice_minimumLinearTextureAlignmentForPixelFormat(handle, format);
  }

  bool
  supportsFamily(WMTGPUFamily gpu_family) {
    return MTLDevice_supportsFamily(handle, gpu_family);
  }

  bool
  supportsBCTextureCompression() {
    return MTLDevice_supportsBCTextureCompression(handle);
  }

  bool
  supportsTextureSampleCount(uint8_t sample_count) {
    return MTLDevice_supportsTextureSampleCount(handle, sample_count);
  }

  bool
  programmableSamplePositionsSupported() {
    return MTLDevice_programmableSamplePositionsSupported(handle);
  }

  uint32_t
  gpuCoreCount() {
    return MTLDevice_gpuCoreCount(handle);
  }

  bool
  hasUnifiedMemory() {
    return MTLDevice_hasUnifiedMemory(handle);
  }

  Reference<FXTemporalScaler>
  newTemporalScaler(const WMTFXTemporalScalerInfo &info) {
    return Reference<FXTemporalScaler>(MTLDevice_newTemporalScaler(handle, &info));
  }

  Reference<FXSpatialScaler>
  newSpatialScaler(const WMTFXSpatialScalerInfo &info) {
    return Reference<FXSpatialScaler>(MTLDevice_newSpatialScaler(handle, &info));
  }

  Reference<BinaryArchive>
  newBinaryArchive(const char *url, Error &error) {
    return Reference<BinaryArchive>(MTLDevice_newBinaryArchive(handle, url, &error.handle));
  }

  bool
  supportsFXSpatialScaler() {
    return MTLDevice_supportsFXSpatialScaler(handle);
  }

  bool
  supportsFXTemporalScaler() {
    return MTLDevice_supportsFXTemporalScaler(handle);
  }

  void
  setShouldMaximizeConcurrentCompilation(bool value) {
    MTLDevice_setShouldMaximizeConcurrentCompilation(handle, value);
  }

  Reference<Texture>
  newSharedTexture(WMTTextureInfo &info) {
    return Reference<Texture>(MTLDevice_newSharedTexture(handle, &info));
  }

  Reference<CounterSampleBuffer>
  newCounterSampleBuffer(uint32_t sample_count, bool shared = true) {
    return Reference<CounterSampleBuffer>(MTLCounterSampleBuffer_newTimestampBuffer(handle, sample_count, shared));
  }

  Reference<IndirectCommandBuffer>
  newIndirectCommandBuffer(WMTIndirectCommandBufferInfo &info, uint64_t max_count, WMTResourceOptions options) {
    return Reference<IndirectCommandBuffer>(MTLDevice_newIndirectCommandBuffer(handle, &info, max_count, options));
  }
};

inline Reference<Array<Device>>
CopyAllDevices() {
  return Reference<Array<Device>>(WMTCopyAllDevices());
}

class CaptureManager : public Object {
public:
  static CaptureManager
  sharedCaptureManager() {
    return CaptureManager{MTLCaptureManager_sharedCaptureManager()};
  };

  bool
  startCapture(WMTCaptureInfo &info) {
    return MTLCaptureManager_startCapture(handle, &info);
  };

  void
  stopCapture() {
    MTLCaptureManager_stopCapture(handle);
  };
};

class DeveloperHUDProperties : public Object {
public:
  static DeveloperHUDProperties
  instance() {
    return DeveloperHUDProperties{DeveloperHUDProperties_instance()};
  };

  bool
  addLabel(String label, String after) {
    return DeveloperHUDProperties_addLabel(handle, label, after);
  }

  void
  updateLabel(String label, String value) {
    DeveloperHUDProperties_updateLabel(handle, label, value);
  }

  void
  remove(String label) {
    DeveloperHUDProperties_remove(handle, label);
  }
};

class CacheReader : public Object {
public:
  static Reference<CacheReader>
  alloc_init(const char *path, uint64_t version) {
    return Reference<CacheReader>(CacheReader_alloc_init(path, version));
  };

  Reference<DispatchData>
  get(const void *key, uint64_t length) {
    return Reference<DispatchData>(CacheReader_get(handle, key, length));
  };

  template <typename K>
  Reference<DispatchData>
  get(const K &key) {
    return Reference<DispatchData>(CacheReader_get(handle, reinterpret_cast<const void *>(&key), sizeof(key)));
  };
};

class CacheWriter : public Object {
public:
  static Reference<CacheWriter>
  alloc_init(const char *path, uint64_t version) {
    return Reference<CacheWriter>(CacheWriter_alloc_init(path, version));
  };

  void
  set(const void *key, uint64_t key_length, DispatchData value) {
    CacheWriter_set(handle, key, key_length, value);
  };

  template <typename K>
  void
  set(const K &key, DispatchData value) {
    set(reinterpret_cast<const void *>(&key), sizeof(key), value);
  };
};

inline Reference<Object>
MakeAutoreleasePool() {
  return Reference<Object>(NSAutoreleasePool_alloc_init());
}

inline Reference<String>
MakeString(const char *data, WMTStringEncoding encoding) {
  return Reference<String>(NSString_alloc_init(data, encoding));
}

inline Reference<DispatchData>
MakeDispatchData(uint64_t native_ptr, uint64_t length) {
  return Reference<DispatchData>(DispatchData_alloc_init(native_ptr, length));
}

inline Reference<DispatchData>
MakeDispatchData(void *native_ptr, uint64_t length) {
  return Reference<DispatchData>(DispatchData_alloc_init((uint64_t)native_ptr, length));
}

inline Object
CreateMetalViewFromHWND(intptr_t hwnd, Device device, MetalLayer &layer) {
  return {::CreateMetalViewFromHWND(hwnd, device.handle, &layer.handle)};
}

inline void
ReleaseMetalView(Object view) {
  return ::ReleaseMetalView(view.handle);
}

inline void
InitializeRenderPassInfo(WMTRenderPassInfo &info) {
  std::memset(&info, 0, sizeof(WMTRenderPassInfo));
  for (unsigned i = 0; i < 8; i++) {
    info.colors[i].store_action = WMTStoreActionStore;
    info.colors[i].clear_color.a = 1.0f;
  }
  info.stencil.load_action = WMTLoadActionClear;
  info.depth.load_action = WMTLoadActionClear;
  info.depth.clear_depth = 1.0f;
};

inline void
InitializeRenderPipelineInfo(WMTRenderPipelineInfo &info) {
  for (unsigned i = 0; i < 8; i++) {
    info.colors[i].pixel_format = WMTPixelFormatInvalid;
    info.colors[i].write_mask = WMTColorWriteMaskAll;
    info.colors[i].blending_enabled = false;
    info.colors[i].alpha_blend_operation = WMTBlendOperationAdd;
    info.colors[i].rgb_blend_operation = WMTBlendOperationAdd;
    info.colors[i].dst_alpha_blend_factor = WMTBlendFactorZero;
    info.colors[i].dst_rgb_blend_factor = WMTBlendFactorZero;
    info.colors[i].src_alpha_blend_factor = WMTBlendFactorOne;
    info.colors[i].src_rgb_blend_factor = WMTBlendFactorOne;
  }
  info.alpha_to_coverage_enabled = false;
  info.rasterization_enabled = true;
  info.raster_sample_count = 1;
  info.depth_pixel_format = WMTPixelFormatInvalid;
  info.stencil_pixel_format = WMTPixelFormatInvalid;
  info.logic_operation_enabled = false;
  info.logic_operation = WMTLogicOperationClear;

  info.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
  info.max_tessellation_factor = 16.0f;
  info.tessellation_factor_step = WMTTessellationFactorStepFunctionConstant;
  info.tessellation_partition_mode = WMTTessellationPartitionModePow2;
  info.tessellation_output_winding_order = WMTWindingClockwise;

  info.vertex_function = NULL_OBJECT_HANDLE;
  info.fragment_function = NULL_OBJECT_HANDLE;
  info.immutable_vertex_buffers = 0;
  info.immutable_fragment_buffers = 0;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
  info.support_indirect_command_buffers = false;
}

inline void
InitializeComputePipelineInfo(WMTComputePipelineInfo &info) {
  info.compute_function = NULL_OBJECT_HANDLE;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
  info.tgsize_is_multiple_of_sgwidth = false;
  info.immutable_buffers = 0;
  info.support_indirect_command_buffers = false;
  info.max_call_stack_depth = 0;
}

inline void
InitializeMeshRenderPipelineInfo(WMTMeshRenderPipelineInfo &info) {
  for (unsigned i = 0; i < 8; i++) {
    info.colors[i].pixel_format = WMTPixelFormatInvalid;
    info.colors[i].write_mask = WMTColorWriteMaskAll;
    info.colors[i].blending_enabled = false;
    info.colors[i].alpha_blend_operation = WMTBlendOperationAdd;
    info.colors[i].rgb_blend_operation = WMTBlendOperationAdd;
    info.colors[i].dst_alpha_blend_factor = WMTBlendFactorZero;
    info.colors[i].dst_rgb_blend_factor = WMTBlendFactorZero;
    info.colors[i].src_alpha_blend_factor = WMTBlendFactorOne;
    info.colors[i].src_rgb_blend_factor = WMTBlendFactorOne;
  }
  info.alpha_to_coverage_enabled = false;
  info.rasterization_enabled = true;
  info.raster_sample_count = 1;
  info.depth_pixel_format = WMTPixelFormatInvalid;
  info.stencil_pixel_format = WMTPixelFormatInvalid;
  info.logic_operation_enabled = false;
  info.logic_operation = WMTLogicOperationClear;

  info.object_function = NULL_OBJECT_HANDLE;
  info.mesh_function = NULL_OBJECT_HANDLE;
  info.fragment_function = NULL_OBJECT_HANDLE;
  info.immutable_object_buffers = 0;
  info.immutable_mesh_buffers = 0;
  info.immutable_fragment_buffers = 0;
  info.payload_memory_length = 0;
  info.mesh_tgsize_is_multiple_of_sgwidth = 0;
  info.object_tgsize_is_multiple_of_sgwidth = 0;
  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
  info.support_indirect_command_buffers = false;
}

inline void
InitializeTileRenderPipelineInfo(WMTTileRenderPipelineInfo &info) {
  for (unsigned i = 0; i < 8; i++) {
    info.color_formats[i] = WMTPixelFormatInvalid;
  }
  info.tile_function = NULL_OBJECT_HANDLE;
  info.immutable_tile_buffers = 0;
  info.raster_sample_count = 1;
  info.tgsize_matches_tile_size = 0;

  info.binary_archive_for_serialization = NULL_OBJECT_HANDLE;
  info.binary_archives_for_lookup.set(nullptr);
  info.num_binary_archives_for_lookup = 0;
  info.fail_on_binary_archive_miss = false;
}

} // namespace WMT