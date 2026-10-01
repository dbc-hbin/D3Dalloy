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
#include "airconv_public.h"
#include "dxmt_buffer.hpp"
#include "dxmt_texture.hpp"
#include "util_svector.hpp"
#include "owned_query_timestamps.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace dxmt {

enum class EncoderType {
  Null,
  Clear,
  Render,
  Blit,
  Compute,
  Resolve,
  PredicatedTextureCopy,
  Temporal,
  Query,
  Raytracing,
  StandardSwizzle,
  WorkGraph,
};

struct EncoderData {
  EncoderType type;
  EncoderData *next = nullptr;
  uint64_t id;
};

struct WorkGraphDispatch;
struct WorkGraphEncoderData : EncoderData {
  std::shared_ptr<WorkGraphDispatch> dispatch;
};

class MTLD3D12Resource;
class MTLD3D12RootSignature;
class MTLD3D12DescriptorHeap;
struct StandardSwizzleTableUse {
  MTLD3D12RootSignature *rootsig;
  MTLD3D12DescriptorHeap *heap;
  uint64_t base;
  uint32_t parameter;
};
struct StandardSwizzleEncoderData : EncoderData {
  // A private COM reference survives the recording's allocator lease through
  // GPU retirement; the allocator releases it when it destroys this entry.
  MTLD3D12Resource *resource = nullptr;
  bool to_canonical = false;
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
  // D3D12 permits typed buffer RTVs, while Metal render attachments cannot be
  // texture-buffer views. These retain the backing allocation and the private
  // 2D staging texture used for an exact copy-in/render/copy-out pass.
  WMT::Reference<WMT::Buffer> buffer_backing;
  WMT::Reference<WMT::Texture> buffer_attachment;
  uint64_t buffer_offset = 0;
  uint32_t buffer_bytes_per_row = 0;
  uint32_t buffer_width = 0;
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

/* Mirrors DXMTSOAssembly in dxmt_command.metal (so_index_range / so_assemble). */
struct StreamOutputAssemblyParams {
  struct Element {
    uint32_t slot;
    uint32_t byte_offset;
    uint32_t dword_offset;
    uint32_t component_count;
  };
  uint32_t topology; // 0 point list, 1 line list, 2 line strip, 3 triangle list, 4 triangle strip
  uint32_t indexed;
  uint32_t index_size;
  uint32_t cut_enabled;
  uint32_t cut_value;
  uint32_t vertex_count;
  uint32_t instance_count;
  uint32_t record_dwords;
  uint32_t element_count;
  uint32_t referenced_mask;
  uint32_t bound_mask;
  uint32_t predicate_enabled;
  uint32_t predicate_not_equal;
  uint32_t index_skip; // bytes from the 4-aligned index binding to the first index
  uint32_t padding[2];
  uint64_t size[4];
  uint32_t stride[4];
  Element elements[128];
  uint32_t statistics_capture_offset;
  uint32_t clip_mask;
  uint32_t cull_mask;
  uint32_t statistics_enabled;
  uint32_t rasterization_enabled;
  uint32_t depth_clip_enabled;
  uint32_t stream_output_enabled;
  uint32_t statistics_padding;
};
static_assert(sizeof(StreamOutputAssemblyParams) == 2192);

/* GPU primitive assembly for pixel-frequency MSAA replay. One indirect draw and three
 * uint32 replay ordinals are reserved per possible IA primitive. A zero index count
 * suppresses a slot removed by a strip cut or GPU predication without a CPU readback. */
struct MSAAPrimitiveAssemblyParams {
  uint32_t topology;
  uint32_t indexed;
  uint32_t index_size;
  uint32_t cut_enabled;
  uint32_t cut_value;
  uint32_t vertex_count;
  uint32_t instance_count;
  uint32_t first_index;
  uint32_t positions_per_instance;
  uint32_t first_position;
  uint32_t max_primitives;
  uint32_t predicate_enabled;
  uint32_t predicate_not_equal;
  uint32_t index_skip;
  uint32_t line_mode;
  /* Conservative triangles: 24 replay words per primitive (18 fan corners, 3 original ordinals, 3 unused). */
  uint32_t conservative;
};
static_assert(sizeof(MSAAPrimitiveAssemblyParams) == 64);

struct MSAAPrimitiveIndirectArguments {
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t index_start;
  int32_t base_vertex;
  uint32_t base_instance;
  uint32_t primitive_id;
};
static_assert(sizeof(MSAAPrimitiveIndirectArguments) == 24);

struct MSAAPrimitiveBoundsParams {
  uint32_t record_dwords;
  uint32_t position_offset;
  uint32_t viewport_index_offset;
  uint32_t target_width;
  uint32_t target_height;
  uint32_t viewport_count;
  uint32_t scissor_count;
  uint32_t max_primitives;
  uint32_t render_target_array_index_offset;
  uint32_t target_array_length;
  uint32_t line_mode;
  uint32_t conservative;
  float viewports[16][4]; // x, y, width, height
  uint32_t scissors[16][4]; // left, top, right, bottom
  /* View instancing: the replayed view's location. When enabled, the effective viewport / render target array
   * index is the shader's value plus the offset, and an out-of-range sum selects 0 (the D3D rule, which is also
   * what the native amplification mapping applies to the replay pipelines). */
  uint32_t view_viewport_offset;
  uint32_t view_layer_offset;
  uint32_t view_offsets_enabled;
  uint32_t primitive_id_offset = ~0u;
};
static_assert(sizeof(MSAAPrimitiveBoundsParams) == 576);

struct MSAAPrimitiveTileBounds {
  uint32_t left;
  uint32_t top;
  uint32_t right;
  uint32_t bottom;
  uint32_t viewport_index;
  uint32_t target_layer;
  uint32_t primitive_id;
};
static_assert(sizeof(MSAAPrimitiveTileBounds) == 28);

/* Mirrors DXMTSOIndirectPreflightParams/Record in dxmt_command.metal. The preflight
 * runs after preceding GPU argument producers and before allocating SO capture memory. */
struct SOIndirectPreflightParams {
  uint64_t argument_buffer;
  uint64_t count_buffer;
  uint64_t predicate;
  uint64_t index_buffer;
  uint32_t max_count;
  uint32_t stride;
  uint32_t draw_offset;
  uint32_t ib_offset; // UINT32_MAX: use the recorded index buffer
  uint32_t predicate_op;
  uint32_t indexed;
  uint32_t index_format;
  uint32_t count_only;
};
static_assert(sizeof(SOIndirectPreflightParams) == 64);

struct SOIndirectPreflightResult {
  uint32_t effective_count;
  uint32_t padding[3];
};
static_assert(sizeof(SOIndirectPreflightResult) == 16);

struct SOIndirectPreflightRecord {
  uint32_t vertex_count;
  uint32_t instance_count;
  uint32_t start_location;
  uint32_t start_instance;
  int32_t base_vertex;
  uint32_t index_format;
  uint64_t index_address;
};
static_assert(sizeof(SOIndirectPreflightRecord) == 32);

/* GPU pointers bound by the signature resolver for one SO ICB command. */
struct SOIndirectCaptureBinding {
  uint64_t indices;
  uint64_t control;
  uint64_t scratch;
  uint64_t positions;
  SOIndirectPreflightRecord arguments;
};
static_assert(sizeof(SOIndirectCaptureBinding) == 64);

/* Mirrors dxmt_render_command_data in the command-signature Metal source. */
struct IndirectRenderCommandData {
  uint64_t cmd_buf;
  uint64_t max_count;
  uint64_t max_count_buffer;
  uint64_t argument_buffer;
  uint64_t predicate;
  uint32_t predicate_op;
  uint32_t predicate_padding;
  uint64_t static_samplers;
  uint64_t rootsig_qwords;
  uint64_t vertex_constants;
  uint64_t fragment_constants;
  uint32_t rootsig_qwords_stride;
  uint32_t primitive_type;
  uint64_t vertex_buffer;
  uint64_t index_buffer;
  DXGI_FORMAT index_buffer_format;
  uint32_t vertex_argbuf_stride;
  uint64_t index_pull_controls;
  uint32_t index_pull_mode;
  uint32_t index_pull_padding;
  uint64_t so_draws;
  uint64_t pipeline_statistics;
};

/*
 * One draw with a stream-output PSO (vertex shader only, stream 0).
 *
 * The VS captures its declared outputs into `scratch` during the application's own draw
 * (SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE, vertex buffers 26-28), keyed by IA position, so
 * the scratch size (VertexCountPerInstance * InstanceCount records plus one claim word each) is
 * known and allocated at record time for direct draws, or after GPU argument preflight for indirect
 * draws. Indexed draws are issued over `positions`, a uint32 buffer
 * the queue regenerates before every execution with k, or 0xFFFFFFFF where a D3D12 strip cut
 * restarts a strip, and the VS pulls the application's index itself. So Metal's restart and the
 * assembly's cut rule are the same rule, and any index value (sparse or 0xFFFF) is captured.
 *
 * Per execution, before the render pass: the queue zeroes the claim words and (indexed) fills
 * `positions`. After the pass: `assemble_pipeline` runs once per draw in recording order, each in
 * its own fenced compute encoder, so a later draw appends after the BufferFilledSize an earlier
 * draw wrote. Offsets are into `heap`, which is the allocator heap for direct draws and an
 * execution-owned buffer for indirect draws. The SO counters and predicate remain in the
 * recording heap, while indirect `heap`, scratch, positions and status remain retained on the
 * in-flight submission; `status_cpu` counts primitives the assembly could not
 * copy because a record was never claimed, a translator invariant the completion thread reports.
 */
struct StreamOutputDrawData {
  WMT::Reference<WMT::Buffer> scratch;
  WMT::Reference<WMT::Buffer> positions;
  WMT::Reference<WMT::ComputePipelineState> assemble_pipeline;
  WMT::Reference<WMT::ComputePipelineState> positions_pipeline;
  WMT::Buffer heap;
  WMT::Buffer counters_heap;
  WMT::Buffer predicate_heap;
  WMT::Buffer statistics_heap;
  WMT::Buffer index_buffer;
  Rc<BufferAllocation> index_owner;
  uint64_t index_offset = 0;
  uint64_t params_offset = 0;
  uint64_t control_offset = 0;
  uint64_t counters_offset = 0;
  uint64_t statistics_offset = 0;
  uint64_t predicate_offset = 0;
  uint64_t status_offset = 0;
  uint64_t claims_offset = 0;
  uint64_t claims_bytes = 0;
  std::array<Rc<BufferAllocation>, 4> targets;
  std::array<uint64_t, 4> target_offsets{};
  std::array<Rc<BufferAllocation>, 4> filled_sizes;
  std::array<uint64_t, 4> filled_size_offsets{};
  bool indexed = false;
  uint32_t vertex_count = 0;
  const uint32_t *status_cpu = nullptr;
  /* GS stream output: `scratch` is shared by every captured stream of the draw. This draw assembles
   * stream `stream`, whose capture region starts `scratch_offset` bytes into `scratch`, i.e.
   * keys * scratch_prefix_dwords dwords (SM50_SHADER_D3D12_GS_STREAMS_DATA layout). `next_stream`
   * is the next captured stream of the same draw (assembled after this one); null otherwise. */
  uint32_t stream = 0;
  uint64_t scratch_offset = 0;
  uint64_t scratch_prefix_dwords = 0;
  std::shared_ptr<StreamOutputDrawData> next_stream;
};

/* The recorded SO ExecuteIndirect is immutable; the submission worker produces
 * separate capture storage and resolver data for every execution. */
struct SOIndirectEncoderData {
  IndirectRenderCommandData command{};
  SOIndirectPreflightParams preflight{};
  std::shared_ptr<StreamOutputDrawData> draw_template;
  const StreamOutputAssemblyParams *assembly_template = nullptr;
  const void *root_arguments = nullptr;
  const void *vertex_buffers = nullptr;
  WMT::Reference<WMT::ComputePipelineState> preflight_pipeline;
  WMT::Reference<WMT::RenderPipelineState> resolver_pipeline;
  WMT::Reference<WMT::ComputePipelineState> argument_resolver;
  WMT::Reference<WMT::RenderPipelineState> draw_pipeline;
  uint64_t static_sampler_offset = 0;
  uint32_t strip_cut = 0;
  bool msaa = false;
};

/*
 * ExecuteIndirect whose captures are sized from the draw arguments: a VS+HS+DS+GS PSO (tsgs-design) or a VS+GS PSO
 * with stream output. The recording holds render passes that carry only the pipeline state. Per execution the
 * queue runs the GPU argument preflight, waits for it, sizes every command's captures exactly from its GPU-written
 * vertex and instance counts (the same sizes a direct draw of those arguments gets), then injects the per-command
 * draws. TS+GS: pass 1 (VS+HS / DS capture, never rasterized), the compaction dispatches, pass 2 (capture fetch /
 * GS) in API order. GS: one pass (`geometry`). Stream output is assembled per command in API order.
 */
struct MeshCaptureIndirectData {
  bool domain_only = false;
  SM50_MSAA_MESH_LAYOUT msaa_layout{};
  /* Mirrors DXMTMeshStripCutControl (dxmt_mesh_strip_cut, d3d12_command_list.cpp). */
  struct StripCutControl {
    uint64_t max_threadgroups;
    uint32_t topology, index_size, cut_value, count, instances;
    int32_t base_vertex;
    uint32_t start_instance, index_skip, predicate_enabled, predicate_not_equal, statistics_enabled, per_group;
    uint32_t cut_enabled;
  };
  SOIndirectPreflightParams preflight{};
  WMT::Reference<WMT::ComputePipelineState> preflight_pipeline;
  WMT::Reference<WMT::ComputePipelineState> compact_pipeline;
  /* Stream output after the GS: the recorded draw (chained per captured stream) whose assembly params live in
   * `so_template->heap`. Every command copies it with its own scratch, params, control and status; the SO
   * counters, predicate snapshot and targets stay shared, so commands append in API order. */
  std::shared_ptr<StreamOutputDrawData> so_template;
  std::vector<StreamOutputAssemblyParams> so_params;  // per chained stream, copied from the recording heap
  WMT::Buffer index_buffer;
  uint64_t index_offset = 0;
  WMT::Buffer binding_heap;
  uint64_t root_offset = 0, root_stride = 0, vertex_offset = 0, vertex_stride = 0;
  const uint8_t *binding_memory = nullptr;
  std::vector<uint32_t> root_view_offsets;
  uint32_t vertex_mask = 0;
  bool update_index = false;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  struct IndexVariant {
    WMT::Reference<WMT::RenderPipelineState> pipeline;
    WMT::Reference<WMT::Buffer> object_constants, mesh_constants;
    uint64_t object_offset = 0, mesh_offset = 0;
  };
  std::array<IndexVariant, 2> index_variants;
  SM50_TESS_CAPTURE_LAYOUT layout{};
  uint32_t control_points = 0;
  uint32_t threads_per_patch = 0;
  uint32_t per_group = 0;  // GS input primitives per pass-2 object threadgroup
  uint32_t gs_instances = 0;
  // GS (`geometry`): D3D_PRIMITIVE_TOPOLOGY of the draw and its object-threadgroup shape (GeometryWarpShape).
  uint32_t topology = 0;
  uint32_t vertex_per_warp = 0;
  uint32_t warp_increment = 0;
  bool geometry = false;
  bool indexed = false;
  /* GS over an indexed strip with an active cut, or over a triangle strip with adjacency: before the pass every
   * command is expanded on the GPU into the equivalent list (dxmt_mesh_strip_cut, as a direct draw of the same
   * arguments), drawn with the list variant; `topology` is then the strip topology, `vertex_per_warp` and
   * `warp_increment` the list's. */
  bool strip_expand = false;

  uint32_t strip_vertices_per_primitive = 0;
  WMT::Reference<WMT::ComputePipelineState> strip_cut_pipeline;
  WMT::Buffer statistics_buffer;  // IAVertices accounting of the expansion (statistics queries only)
  uint64_t statistics_offset = 0;
  uint64_t max_threadgroups = 0;
  /* PRIMITIVE_CLAIM statistics: per-command claim headers (command i at statistics_claim_offset + 32 * i), bound
   * to the fragment stage before each command's rasterizing draw. Empty: none. */
  WMT::Buffer statistics_claim;
  uint64_t statistics_claim_offset = 0;
};

/* One recorded MSAA draw. The queue creates its own capture,
 * tile and primitive buffers for every execution; only immutable recorded state
 * and native pipeline handles live here. A bundle replays into a fresh record. */
struct MSAADrawData {
  WMT::Buffer statistics_claim;
  uint64_t statistics_claim_offset = 0;
  bool mesh_capture = false;
  SM50_MSAA_MESH_LAYOUT mesh_layout{};
  SM50_MSAA_MESH_CONTROL mesh_control{};
  SM50_MSAA_OBJECT_LAYOUT object_layout{};
  WMT::Reference<WMT::RenderPipelineState> object_replay_pipeline;
  WMT::Reference<WMT::Buffer> object_records;
  uint64_t object_record_offset = 0;
  WMT::Reference<WMT::Buffer> mesh_grid_buffer;
  uint64_t mesh_grid_offset = 0;
  WMT::Reference<WMT::ComputePipelineState> capture_pipeline;
  WMT::Reference<WMT::ComputePipelineState> assembly_pipeline;
  WMT::Reference<WMT::ComputePipelineState> bounds_pipeline;
  WMT::Reference<WMT::RenderPipelineState> coverage_pipeline;
  WMT::Reference<WMT::RenderPipelineState> coverage_with_depth_pipeline;
  WMT::Reference<WMT::RenderPipelineState> clear_coverage_pipeline;
  WMT::Reference<WMT::RenderPipelineState> gather_depth_pipeline;
  WMT::Reference<WMT::RenderPipelineState> early_depth_pipeline;
  WMT::Reference<WMT::RenderPipelineState> late_depth_pipeline;
  WMT::Reference<WMT::RenderPipelineState> sample_pixel_pipeline;
  WMT::Reference<WMT::DepthStencilState> capture_depth_state;
  WMT::Reference<WMT::RenderPipelineState> pixel_pipeline;
  WMT::Reference<WMT::RenderPipelineState> replay_pipeline;
  WMT::Reference<WMT::RenderPipelineState> replay_layer_pipeline;
  WMT::Reference<WMT::RenderPipelineState> query_coverage_pipeline;
  std::array<WMT::Reference<WMT::Buffer>, 6> constants;
  std::array<uint64_t, 6> constant_offsets{};
  WMT::Reference<WMT::DepthStencilState> original_depth_stencil;
  WMT::Reference<WMT::DepthStencilState> no_depth_stencil;
  WMT::Reference<WMT::Buffer> index_buffer;
  WMT::Reference<WMT::Buffer> predicate_buffer;
  WMT::Reference<WMT::Buffer> root_buffer;
  WMT::Reference<WMT::Buffer> vertex_buffer_table;
  WMT::Reference<WMT::Buffer> static_sampler_buffer;
  std::shared_ptr<StreamOutputDrawData> stream_output;
  SM50_MSAA_PIXEL_LAYOUT pixel_layout{};
  MSAAPrimitiveAssemblyParams assembly{};
  MSAAPrimitiveBoundsParams bounds{};
  float viewport_depths[16][2]{};
  uint64_t index_offset = 0;
  uint64_t predicate_offset = 0;
  uint64_t root_offset = 0;
  uint64_t vertex_buffer_offset = 0;
  uint64_t static_sampler_offset = 0;
  SM50_MSAA_VERTEX_LAYOUT vertex_layout{};
  uint32_t sample_count = 0;
  uint32_t rov_marker_sample_count = 0;
  uint32_t sample_mask = 0;
  /* PSO SampleDesc.Count: the RTV (output) samples; differs from sample_count under a forced raster pattern. */
  uint32_t output_sample_count = 1;
  SM50_MSAA_LINE_MODE line_mode = SM50_MSAA_LINE_NONE;
  bool depth_clip_enabled = true;
  uint32_t start_vertex = 0;
  uint32_t start_instance = 0;
  int32_t base_vertex = 0;
  bool indexed = false;
  bool indirect = false;
  bool needs_stream_assembly = false;
  bool single_sample_replay = false;
  bool center_sample_pattern = false;
  /* Variable-rate shading state for coarse variants (SM50_MSAA_TILE_CONTROL.shading_rate, msaa_coarse.hpp
   * kRate* encoding); 0 for fine-rate variants. */
  uint32_t shading_rate = 0;
  WMT::Reference<WMT::Texture> shading_rate_image;
  uint32_t shading_rate_image_width = 0, shading_rate_image_height = 0;
  /* D3D12 conservative rasterization of a triangle draw (SM50_SHADER_D3D12_CONSERVATIVE_RASTER variants). */
  bool conservative = false;
};

/* Replay index words per primitive slot: line quads 20, conservative triangles 24, otherwise 3. */
inline uint32_t MSAAIndexStride(SM50_MSAA_LINE_MODE line_mode, bool conservative) {
  return line_mode != SM50_MSAA_LINE_NONE ? 20 : conservative ? 24 : 3;
}

struct RenderEncoderData : EncoderData {
  std::array<RenderEncoderColorAttachmentData, 8> colors;
  RenderEncoderDepthAttachmentData depth;
  RenderEncoderStencilAttachmentData stencil;
  uint8_t default_raster_sample_count;
  bool center_sample_pattern = false;
  uint16_t render_target_array_length;
  uint32_t render_target_height;
  uint32_t render_target_width;
  uint32_t rov_marker_width = 0;
  uint32_t rov_marker_height = 0;
  uint8_t rov_marker_sample_count = 0;
  bool rov_marker_attachmentless = false;
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
  /* Stream-output draws of this pass, assembled in order after the pass ends. */
  std::vector<std::shared_ptr<StreamOutputDrawData>> so_draws;
  std::unique_ptr<SOIndirectEncoderData> so_indirect;
  std::unique_ptr<MSAADrawData> msaa_draw;
  std::unique_ptr<std::vector<StandardSwizzleTableUse>> swizzle_tables;
  /* Capture-sized ExecuteIndirect: pass 1 or 2 of a TS+GS `capture_indirect`, or 1 (its only pass) of a GS one;
   * 0: none. The queue appends the per-command draws. */
  std::shared_ptr<const MeshCaptureIndirectData> capture_indirect;
  uint8_t capture_indirect_pass = 0;
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
  std::unique_ptr<std::vector<StandardSwizzleTableUse>> swizzle_tables;
  /*
   * A ray dispatch binds the compiled pipeline and the intersection function
   * table, and reads the application's shader table buffers. The encoded commands
   * only carry Metal handles, so the encoder keeps the objects alive until the
   * queue worker has encoded them.
   */
  WMT::Reference<WMT::ComputePipelineState> rt_pipeline;
  /* Translator-internal kernel (predication, atomic copy) bound by this pass;
   * the owning list may be released before the queue worker encodes it. */
  WMT::Reference<WMT::ComputePipelineState> internal_pipeline;
  WMT::Reference<WMT::IntersectionFunctionTable> rt_intersection_function_table;
  /**
   * The visible function table is a distinct Metal object from the intersection
   * function table, so the pass has to retain it too: the encoded command only
   * carries a handle, and this reference is what keeps the table alive until the
   * queue worker encodes it.
   */
  WMT::Reference<WMT::VisibleFunctionTable> rt_visible_function_table;
};
struct PredicatedTextureCopyEncoderData : EncoderData {
  WMT::Reference<WMT::Texture> source;
  WMT::Reference<WMT::Texture> destination;
  WMT::Reference<WMT::Buffer> predicate;
  const uint64_t *predicate_value = nullptr;
  uint64_t predicate_offset = 0;
  WMTOrigin source_origin{};
  WMTOrigin destination_origin{};
  WMTSize size{};
  uint32_t source_level = 0;
  uint32_t source_slice = 0;
  uint32_t destination_level = 0;
  uint32_t destination_slice = 0;
  bool predicate_not_equal = false;
};

struct ResolveEncoderData : EncoderData {
  TextureViewRef src;
  TextureViewRef dst;
  WMT::Reference<WMT::RenderPipelineState> pipeline;
  WMT::Reference<WMT::DepthStencilState> depth_stencil_state;
  uint8_t dsv_flags = 0;
  struct {
    int32_t source_delta_x = 0;
    int32_t source_delta_y = 0;
    uint32_t sample_count = 0;
    uint32_t mode = 0;
    uint32_t predicate_enabled = 0;
    uint32_t predicate_not_equal = 0;
  } parameters;
  WMT::Reference<WMT::Buffer> predicate;
  uint64_t predicate_offset = 0;
  uint32_t destination_x = 0;
  uint32_t destination_y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

enum class QueryEncoderOp : uint8_t {
  ResetOcclusion,
  SampleTimestamp,
  ResolveOcclusion,
  ResolveBinaryOcclusion,
  ResolveTimestamp,
  /* SO statistics: copy the stream counter pair at `counters`+`counters_offset` into the
   * heap's Begin snapshot at `source`+`snapshot_offset`. */
  BeginSOStatistics,
  /* Write (counter - snapshot) into the heap result at `source`+`start_index`*16. */
  EndSOStatistics,
  ResolveSOStatistics,
  ResetPipelineStatistics,
  BeginPipelineStatistics,
  EndPipelineStatistics,
  ResolvePipelineStatistics,
  /* No query: an in-command-buffer event signal/wait that makes writes through one GPU virtual address
   * visible through every other address aliasing the same memory (sparse tiles sharing a heap page).
   * Metal fences and memory barriers do not; see d3d12_reserved_resource.hpp. */
  AliasFlush,
};

struct QueryEncoderData : EncoderData {
  QueryEncoderOp op;
  WMT::Reference<WMT::Buffer> counters;
  uint64_t counters_offset = 0;
  uint64_t snapshot_offset = 0;
  WMT::Reference<WMT::CounterSampleBuffer> timestamp_samples;
  std::shared_ptr<owned_graphics::QueryTimestampProducers> timestamp_producers;
  WMT::Reference<WMT::Buffer> source;
  WMT::Reference<WMT::Buffer> destination;
  WMT::Reference<WMT::Buffer> timestamp_dummy;
  WMT::Reference<WMT::ComputePipelineState> compute_pipeline;
  uint32_t start_index = 0;
  uint32_t query_count = 0;
  uint64_t destination_offset = 0;
  /* Pipeline-statistics ops: bytes per query record (88 for PIPELINE_STATISTICS, 112 for STATISTICS1). */
  uint32_t record_bytes = 0;
};

enum class RTASOperation : uint8_t {
  Build,
  Refit,
  Clone,
  Compact,
  Emit,
  Serialize,
  Deserialize,
  Visualize,
};

struct RTASPostbuildInfo {
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE type{};
  WMT::Reference<WMT::Buffer> destination;
  uint64_t offset = 0;
  uint64_t address = 0;
};

/* One acceleration-structure command. It carries everything the queue needs to run the GPU
 * prepass and then encode the native build/refit/copy on the application's own command buffer. */
struct RTASEncoderData : EncoderData {
  RTASComputePipelines pipelines;
  raytracing::ShadowPlan shadowPlan;
  raytracing::ShadowMetadata metadata{};
  bool deferredUpdate = false;
  uint64_t nativeCapacity = 0;
  raytracing::BuildInputs deferredInputs;
  raytracing::BufferLocation sourceLocation{}, destinationLocation{};
  RTASOperation operation{};
  WMT::Reference<WMT::AccelerationStructureDescriptor> descriptor;
  WMT::Reference<WMT::AccelerationStructureSidecar> source;
  WMT::Reference<WMT::AccelerationStructureSidecar> destination;
  WMT::Reference<WMT::Buffer> sourceInstances;
  WMT::Reference<WMT::Buffer> header;
  WMT::Reference<WMT::Buffer> scratch;

  uint64_t sourceInstancesOffset = 0;
  uint64_t destinationOffset = 0;
  uint64_t scratchOffset = 0;
  uint32_t instanceCount = 0;
  /* raytracing::InstanceLayout of sourceInstances (Build/Refit of a top-level structure). */
  uint32_t instanceLayout = 1;
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
