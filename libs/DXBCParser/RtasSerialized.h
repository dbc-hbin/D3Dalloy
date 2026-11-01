// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// Serialized acceleration structures (CopyRaytracingAccelerationStructure
// DESERIALIZE). All inputs are borrowed copies of application bytes; outputs are
// written only on success. Driver identity, vertex formats, Metal buffer
// locations and ownership stay with the caller.
enum {
    D3DALLOY_RTAS_SERIALIZED_SUCCESS = 0,
    D3DALLOY_RTAS_SERIALIZED_INVALID_ARGUMENT = 1,
    D3DALLOY_RTAS_SERIALIZED_INVALID_DATA = 2,
};

// blocks: the header is followed by one OMM block; header_offset is then known
// only after d3dalloy_rtas_serialized_block.
typedef struct D3DalloyRtasSerializedLayout {
    uint64_t serialized_size;
    uint64_t header_offset;
    uint32_t blocks;
    uint32_t reserved;
} D3DalloyRtasSerializedLayout;

// Offset and size relative to the deserialized acceleration structure.
typedef struct D3DalloyRtasSpan {
    uint64_t offset;
    uint64_t size;
} D3DalloyRtasSpan;

typedef struct D3DalloyRtasSerializedInfo {
    uint64_t header_offset;
    uint64_t shadow_size;
    uint64_t deserialized_size;
    D3DalloyRtasSpan instances; // top level only
    uint32_t type;
    uint32_t flags;
    uint32_t desc_count;
    uint32_t reserved;
} D3DalloyRtasSerializedInfo;

enum {
    D3DALLOY_RTAS_GEOMETRY_DATA = 1,
    D3DALLOY_RTAS_GEOMETRY_INDICES = 2,
    D3DALLOY_RTAS_GEOMETRY_TRANSFORM = 4,
    D3DALLOY_RTAS_GEOMETRY_OMM_INDICES = 8,
};

typedef struct D3DalloyRtasSerializedGeometry {
    D3DalloyRtasSpan data; // vertices, AABBs or OMM array data
    D3DalloyRtasSpan indices;
    D3DalloyRtasSpan transform;
    D3DalloyRtasSpan omm_indices;
    uint32_t present; // D3DALLOY_RTAS_GEOMETRY_* bits of the valid spans
    uint32_t reserved;
} D3DalloyRtasSerializedGeometry;

#define D3DALLOY_RTAS_SERIALIZED_HEADER_SIZE 56u
#define D3DALLOY_RTAS_SERIALIZED_BLOCK_SIZE 16u

#ifdef __cplusplus
static_assert(sizeof(D3DalloyRtasSerializedLayout) == 24, "serialized layout ABI");
static_assert(sizeof(D3DalloyRtasSerializedInfo) == 56, "serialized info ABI");
static_assert(sizeof(D3DalloyRtasSerializedGeometry) == 72, "serialized geometry ABI");
extern "C" {
#endif

// The 56-byte D3D12 header against the readable source bytes: size within
// [120, available], and either the OMM block marker or pointers that leave the
// 64-byte d3dalloy header in range.
uint32_t d3dalloy_rtas_serialized_prefix(const void *serialized, uint64_t length, uint64_t available,
                                     D3DalloyRtasSerializedLayout *out);
// The 16-byte block after the header: type 0 and pointers in range.
uint32_t d3dalloy_rtas_serialized_block(const D3DalloyRtasSerializedLayout *layout, const void *block, uint64_t length,
                                    D3DalloyRtasSerializedLayout *out);
// Bytes of geometry records after the d3dalloy header (0 for a top level), bounded
// by the serialized size.
uint32_t d3dalloy_rtas_serialized_geometry_bytes(const D3DalloyRtasSerializedLayout *layout, const void *header,
                                             uint64_t length, uint64_t *out);
// Every structural rule of the serialized header, d3dalloy header and geometry
// records; destination_available bounds the deserialized size.
uint32_t d3dalloy_rtas_serialized_parse(const void *serialized, uint64_t serialized_length,
                                    const D3DalloyRtasSerializedLayout *layout, const void *header, uint64_t header_length,
                                    const void *geometry, uint64_t geometry_length, uint64_t destination_available,
                                    D3DalloyRtasSerializedInfo *out);
// Spans of geometry record `index` for an info from a successful parse.
uint32_t d3dalloy_rtas_serialized_geometry(const D3DalloyRtasSerializedInfo *info, const void *geometry, uint64_t length,
                                       uint32_t index, D3DalloyRtasSerializedGeometry *out);

#ifdef __cplusplus
}
#endif
