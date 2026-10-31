// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// Serialized acceleration structures (CopyRaytracingAccelerationStructure
// DESERIALIZE). All inputs are borrowed copies of application bytes; outputs are
// written only on success. Driver identity, vertex formats, Metal buffer
// locations and ownership stay with the caller.
enum {
    DXMT_RTAS_SERIALIZED_SUCCESS = 0,
    DXMT_RTAS_SERIALIZED_INVALID_ARGUMENT = 1,
    DXMT_RTAS_SERIALIZED_INVALID_DATA = 2,
};

// blocks: the header is followed by one OMM block; header_offset is then known
// only after dxmt_rtas_serialized_block.
typedef struct DxmtRtasSerializedLayout {
    uint64_t serialized_size;
    uint64_t header_offset;
    uint32_t blocks;
    uint32_t reserved;
} DxmtRtasSerializedLayout;

// Offset and size relative to the deserialized acceleration structure.
typedef struct DxmtRtasSpan {
    uint64_t offset;
    uint64_t size;
} DxmtRtasSpan;

typedef struct DxmtRtasSerializedInfo {
    uint64_t header_offset;
    uint64_t shadow_size;
    uint64_t deserialized_size;
    DxmtRtasSpan instances; // top level only
    uint32_t type;
    uint32_t flags;
    uint32_t desc_count;
    uint32_t reserved;
} DxmtRtasSerializedInfo;

enum {
    DXMT_RTAS_GEOMETRY_DATA = 1,
    DXMT_RTAS_GEOMETRY_INDICES = 2,
    DXMT_RTAS_GEOMETRY_TRANSFORM = 4,
    DXMT_RTAS_GEOMETRY_OMM_INDICES = 8,
};

typedef struct DxmtRtasSerializedGeometry {
    DxmtRtasSpan data; // vertices, AABBs or OMM array data
    DxmtRtasSpan indices;
    DxmtRtasSpan transform;
    DxmtRtasSpan omm_indices;
    uint32_t present; // DXMT_RTAS_GEOMETRY_* bits of the valid spans
    uint32_t reserved;
} DxmtRtasSerializedGeometry;

#define DXMT_RTAS_SERIALIZED_HEADER_SIZE 56u
#define DXMT_RTAS_SERIALIZED_BLOCK_SIZE 16u

#ifdef __cplusplus
static_assert(sizeof(DxmtRtasSerializedLayout) == 24, "serialized layout ABI");
static_assert(sizeof(DxmtRtasSerializedInfo) == 56, "serialized info ABI");
static_assert(sizeof(DxmtRtasSerializedGeometry) == 72, "serialized geometry ABI");
extern "C" {
#endif

// The 56-byte D3D12 header against the readable source bytes: size within
// [120, available], and either the OMM block marker or pointers that leave the
// 64-byte dxmt header in range.
uint32_t dxmt_rtas_serialized_prefix(const void *serialized, uint64_t length, uint64_t available,
                                     DxmtRtasSerializedLayout *out);
// The 16-byte block after the header: type 0 and pointers in range.
uint32_t dxmt_rtas_serialized_block(const DxmtRtasSerializedLayout *layout, const void *block, uint64_t length,
                                    DxmtRtasSerializedLayout *out);
// Bytes of geometry records after the dxmt header (0 for a top level), bounded
// by the serialized size.
uint32_t dxmt_rtas_serialized_geometry_bytes(const DxmtRtasSerializedLayout *layout, const void *header,
                                             uint64_t length, uint64_t *out);
// Every structural rule of the serialized header, dxmt header and geometry
// records; destination_available bounds the deserialized size.
uint32_t dxmt_rtas_serialized_parse(const void *serialized, uint64_t serialized_length,
                                    const DxmtRtasSerializedLayout *layout, const void *header, uint64_t header_length,
                                    const void *geometry, uint64_t geometry_length, uint64_t destination_available,
                                    DxmtRtasSerializedInfo *out);
// Spans of geometry record `index` for an info from a successful parse.
uint32_t dxmt_rtas_serialized_geometry(const DxmtRtasSerializedInfo *info, const void *geometry, uint64_t length,
                                       uint32_t index, DxmtRtasSerializedGeometry *out);

#ifdef __cplusplus
}
#endif
