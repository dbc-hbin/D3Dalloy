#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t dxmt_mtl_handle;

enum dxmt_as_geometry_type {
  DXMT_AS_GEOMETRY_TRIANGLES = 0,
  DXMT_AS_GEOMETRY_AABBS = 1,
};

enum dxmt_as_copy_mode {
  DXMT_AS_COPY_CLONE = 0,
  DXMT_AS_COPY_COMPACT = 1,
};

struct dxmt_as_sizes {
  uint64_t acceleration_structure_size;
  uint64_t build_scratch_size;
  uint64_t refit_scratch_size;
};

struct dxmt_as_prepass_params {
  uint64_t as_resource_id;
  uint64_t contributions_address;
  uint32_t src_instance_offset;
  uint32_t src_instance_stride;
  uint32_t dst_instance_stride;
  uint32_t instance_count;
  /* 0: header only; 1: D3D12_ELEMENTS_LAYOUT_ARRAY; 2: D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS. */
  uint32_t convert_instances;
  /* 32-bit version token this build publishes into live_mirror[live_mirror_slot]. */
  uint32_t version_token;
  uint32_t live_mirror_slot;
};

/* Returned objects have +1 ownership. All input Metal handles are borrowed; descriptors retain
 * their geometry buffers. A sidecar owns the native AS and TLAS conversion buffers. */
dxmt_mtl_handle dxmt_as_descriptor_new(dxmt_mtl_handle device, uint32_t top_level, uint32_t geometry_count);
void dxmt_as_descriptor_set_usage(dxmt_mtl_handle descriptor, uint32_t usage);
int dxmt_as_descriptor_set_triangles(dxmt_mtl_handle descriptor, uint32_t index,
    dxmt_mtl_handle vertices, uint64_t vertex_offset, uint64_t vertex_stride,
    dxmt_mtl_handle indices, uint64_t index_offset, uint32_t index_type,
    dxmt_mtl_handle transform, uint64_t transform_offset, uint64_t triangle_count,
    uint32_t intersection_function_table_offset, uint32_t geometry_flags, uint32_t vertex_format);
int dxmt_as_descriptor_set_aabbs(dxmt_mtl_handle descriptor, uint32_t index,
    dxmt_mtl_handle boxes, uint64_t box_offset, uint64_t box_stride,
    uint64_t box_count, uint32_t intersection_function_table_offset, uint32_t geometry_flags);
int dxmt_as_descriptor_set_instances(dxmt_mtl_handle descriptor,
    dxmt_mtl_handle converted_instances, uint64_t converted_offset, uint64_t instance_count);

struct dxmt_as_sizes dxmt_as_get_sizes(dxmt_mtl_handle device, dxmt_mtl_handle descriptor);
dxmt_mtl_handle dxmt_as_sidecar_new(dxmt_mtl_handle device, uint64_t native_size,
    uint64_t instance_count);
dxmt_mtl_handle dxmt_as_sidecar_acceleration_structure(dxmt_mtl_handle sidecar);
dxmt_mtl_handle dxmt_as_sidecar_instance_buffer(dxmt_mtl_handle sidecar);
dxmt_mtl_handle dxmt_as_sidecar_contribution_buffer(dxmt_mtl_handle sidecar);
uint64_t dxmt_as_sidecar_resource_id(dxmt_mtl_handle sidecar);
uint64_t dxmt_as_sidecar_contribution_gpu_address(dxmt_mtl_handle sidecar);
uint64_t dxmt_as_sidecar_size(dxmt_mtl_handle sidecar);
int dxmt_as_sidecar_allocate(dxmt_mtl_handle sidecar, dxmt_mtl_handle device, uint64_t native_size, uint64_t instance_count);
uint64_t dxmt_as_sidecar_instance_count(dxmt_mtl_handle sidecar);
dxmt_mtl_handle dxmt_as_sidecar_generations(dxmt_mtl_handle sidecar);

/* Compute helpers use an already-open MTLComputeCommandEncoder and the named pipeline state
 * compiled from rt_as_prepass.metal. All handles/offsets are borrowed. */
int dxmt_as_encode_prepass(dxmt_mtl_handle encoder, dxmt_mtl_handle pipeline,
    dxmt_mtl_handle source_instances, uint64_t source_offset,
    dxmt_mtl_handle destination_instances, uint64_t destination_offset,
    dxmt_mtl_handle header_buffer, uint64_t header_offset,
    dxmt_mtl_handle contribution_buffer, uint64_t contribution_offset,
    dxmt_mtl_handle live_mirror_buffer, uint64_t live_mirror_offset,
    const struct dxmt_as_prepass_params *params);


/* These encode into an already-open acceleration-structure command encoder. The caller must encode
 * the prepass compute pass first and end it, giving GPU-command ordering for header/instances. */

int dxmt_as_encode_build(dxmt_mtl_handle encoder, dxmt_mtl_handle descriptor,
    dxmt_mtl_handle destination_sidecar, dxmt_mtl_handle scratch, uint64_t scratch_offset);
int dxmt_as_encode_refit(dxmt_mtl_handle encoder, dxmt_mtl_handle descriptor,
    dxmt_mtl_handle source_sidecar, dxmt_mtl_handle destination_sidecar,
    dxmt_mtl_handle scratch, uint64_t scratch_offset);
int dxmt_as_encode_copy(dxmt_mtl_handle encoder, dxmt_mtl_handle source_sidecar,
    dxmt_mtl_handle destination_sidecar, uint32_t mode);
void dxmt_as_encode_compacted_size(dxmt_mtl_handle encoder, dxmt_mtl_handle source_sidecar,
    dxmt_mtl_handle destination_buffer, uint64_t destination_offset);

#ifdef __cplusplus
}
#endif
