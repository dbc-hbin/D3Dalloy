/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#pragma once

#include "d3d12_acceleration_structure.hpp"
#include "dxmt_texture.hpp"
#include "util_svector.hpp"
#include "owned_query_timestamps.hpp"
#include <cstdint>

namespace dxmt {

enum class EncoderType {
  Null,
  Clear,
  Render,
  Blit,
  Compute,
  Resolve,
  Temporal,
  Query,
  Raytracing,
};

struct EncoderData {
  EncoderType type;
  EncoderData *next = nullptr;
  uint64_t id;
};

struct ClearEncoderData : EncoderData {
  union {
    WMTClearColor color;
    std::pair<float, uint8_t> depth_stencil;
  };
  TextureViewRef attachment;
  unsigned clear_dsv;
  unsigned array_length;
  unsigned width;
  unsigned height;
  unsigned depth_plane;

  ClearEncoderData() {}
};

struct RenderEncoderColorAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  struct WMTClearColor clear_color;
  TextureViewRef resolve_attachment;
  uint16_t resolve_level;
  uint16_t resolve_slice;
  uint32_t resolve_depth_plane;
};

struct RenderEncoderDepthAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  float clear_depth;
};

struct RenderEncoderStencilAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  uint8_t clear_stencil;
};

struct QueryVisibilityTarget {
  WMT::Reference<WMT::Buffer> destination;
  WMT::Reference<WMT::ComputePipelineState> accumulate_pipeline;
  uint64_t heap_id = 0;
  uint32_t index = 0;
};

struct RenderEncoderData : EncoderData {
  std::array<RenderEncoderColorAttachmentData, 8> colors;
  RenderEncoderDepthAttachmentData depth;
  RenderEncoderStencilAttachmentData stencil;
  uint8_t default_raster_sample_count;
  uint16_t render_target_array_length;
  uint32_t render_target_height;
  uint32_t render_target_width;
  wmtcmd_render_nop cmd_head;
  wmtcmd_base *cmd_tail;
  uint8_t dsv_planar_flags;
  uint8_t dsv_readonly_flags;
  uint8_t render_target_count;
  // Metal exposes one visibility-result stream per render pass. Snapshot all
  // D3D12 queries active for this pass, count once into a private 8-byte
  // scratch slot, then fan that count into each query heap slot on the queue.
  WMT::Reference<WMT::Buffer> visibility_scratch;
  small_vector<QueryVisibilityTarget, 4> visibility_targets;
  // Recorded handles must survive PSO switches and deferred queue encoding.
  small_vector<WMT::Reference<WMT::Buffer>, 4> immutable_constant_buffers;
  bool use_tessellation = 0;
  bool use_geometry = 0;
};

struct BlitEncoderData : EncoderData {
  wmtcmd_blit_nop cmd_head;
  wmtcmd_base *cmd_tail;
};

struct ComputeEncoderData : EncoderData {
  wmtcmd_compute_nop cmd_head;
  wmtcmd_base *cmd_tail;
  small_vector<WMT::Reference<WMT::Buffer>, 4> immutable_constant_buffers;
  /*
   * A ray dispatch binds the compiled pipeline and the intersection function
   * table, and reads the application's shader table buffers. The encoded commands
   * only carry Metal handles, so the encoder keeps the objects alive until the
   * queue worker has encoded them.
   */
  WMT::Reference<WMT::ComputePipelineState> rt_pipeline;
  WMT::Reference<WMT::IntersectionFunctionTable> rt_intersection_function_table;
  /**
   * The visible function table is a distinct Metal object from the intersection
   * function table, so the pass has to retain it too: the encoded command only
   * carries a handle, and this reference is what keeps the table alive until the
   * queue worker encodes it.
   */
  WMT::Reference<WMT::VisibleFunctionTable> rt_visible_function_table;
  small_vector<WMT::Reference<WMT::Buffer>, 4> rt_buffers;
};
struct ResolveEncoderData : EncoderData {
  TextureViewRef src;
  TextureViewRef dst;
};

enum class QueryEncoderOp : uint8_t {
  ResetOcclusion,
  SampleTimestamp,
  ResolveOcclusion,
  ResolveBinaryOcclusion,
  ResolveTimestamp,
};

struct QueryEncoderData : EncoderData {
  QueryEncoderOp op;
  WMT::Reference<WMT::CounterSampleBuffer> timestamp_samples;
  std::shared_ptr<owned_graphics::QueryTimestampProducers> timestamp_producers;
  WMT::Reference<WMT::Buffer> source;
  WMT::Reference<WMT::Buffer> destination;
  WMT::Reference<WMT::Buffer> timestamp_dummy;
  WMT::Reference<WMT::ComputePipelineState> compute_pipeline;
  uint32_t start_index = 0;
  uint32_t query_count = 0;
  uint64_t destination_offset = 0;
};

enum class RTASOperation : uint8_t {
  Build,
  Refit,
  Clone,
  Compact,
  Emit,
};

struct RTASPostbuildInfo {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE type{};
  WMT::Reference<WMT::Buffer> destination;
  uint64_t offset = 0;
};

/* One acceleration-structure command. It carries everything the queue needs to run the GPU
 * prepass and then encode the native build/refit/copy on the application's own command buffer. */
struct RTASEncoderData : EncoderData {
  RTASOperation operation{};
  WMT::Reference<WMT::AccelerationStructureDescriptor> descriptor;
  WMT::Reference<WMT::AccelerationStructureSidecar> source;
  WMT::Reference<WMT::AccelerationStructureSidecar> destination;
  WMT::Reference<WMT::Buffer> sourceInstances;
  WMT::Reference<WMT::Buffer> header;
  WMT::Reference<WMT::Buffer> scratch;
  WMT::Reference<WMT::ComputePipelineState> prepassPipeline;
  WMT::Reference<WMT::ComputePipelineState> writeU64Pipeline;
  WMT::Reference<WMT::ComputePipelineState> finalizeCompactedSizePipeline;
  uint64_t sourceInstancesOffset = 0;
  uint64_t destinationOffset = 0;
  uint64_t scratchOffset = 0;
  uint32_t instanceCount = 0;
  /*
   * The version this build publishes, and where the prepass publishes it. The queue reports the
   * mirror value to the device once this submission completes, which is what tells the device
   * which native structure the D3D12 address holds from then on (see owned_rtas_lifetime.hpp).
   * Zero means the build is not version tracked, which only happens for Emit.
   */
  raytracing::ArenaKey destinationKey{};
  WMT::Reference<WMT::Buffer> liveMirror;
  uint64_t liveMirrorOffset = 0;
  uint32_t versionToken = 0;
  std::vector<RTASPostbuildInfo> postbuild;
};

}; // namespace dxmt
