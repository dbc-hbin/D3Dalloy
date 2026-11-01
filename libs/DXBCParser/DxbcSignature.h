// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// DXBC signature chunk payloads (the bytes after the chunk header). The caller keeps
// the payload readable and immutable for each call; name offsets refer to it.
enum {
    D3DALLOY_DXBC_SIGNATURE_SUCCESS = 0,
    D3DALLOY_DXBC_SIGNATURE_INVALID_ARGUMENT = 1,
    D3DALLOY_DXBC_SIGNATURE_HEADER = 2,
    D3DALLOY_DXBC_SIGNATURE_RECORDS = 3,
    D3DALLOY_DXBC_SIGNATURE_NAME = 4,
    D3DALLOY_DXBC_SIGNATURE_REGISTER_ORDER = 5,
    D3DALLOY_DXBC_SIGNATURE_STREAM_ORDER = 6,
};

enum {
    // ISGN, OSGN, PCSG: 24-byte records without stream or precision.
    D3DALLOY_DXBC_SIGNATURE_LEGACY = 0,
    // OSG5: 28-byte records with a stream.
    D3DALLOY_DXBC_SIGNATURE_STREAM = 1,
    // ISG1, OSG1, PSG1: 32-byte records with stream and minimum precision.
    D3DALLOY_DXBC_SIGNATURE_11_1 = 2,
};

typedef struct D3DalloyDxbcSignatureInfo {
    uint32_t parameter_count;
    uint32_t records_offset;
    uint32_t format;
    // Grouped parses only: records are partitioned into stream_count leading
    // streams, stream_parameters[s] records each. Ungrouped parses report all
    // records in stream 0.
    uint32_t stream_count;
    uint32_t stream_parameters[4];
    // Sum over records of name length plus one terminator.
    uint64_t string_bytes;
} D3DalloyDxbcSignatureInfo;

typedef struct D3DalloyDxbcSignatureParameter {
    uint32_t stream;
    uint32_t semantic_index;
    uint32_t system_value;
    uint32_t component_type;
    uint32_t register_index;
    uint32_t mask;
    uint32_t never_writes_mask;
    // Zero (D3D_MIN_PRECISION_DEFAULT) for formats without precision.
    uint32_t min_precision;
    // NUL-terminated name at name_offset; name_size excludes the terminator.
    uint32_t name_offset;
    uint32_t name_size;
} D3DalloyDxbcSignatureParameter;

#ifdef __cplusplus
static_assert(sizeof(D3DalloyDxbcSignatureInfo) == 40, "DXBC signature info ABI");
static_assert(sizeof(D3DalloyDxbcSignatureParameter) == 40, "DXBC signature parameter ABI");
extern "C" {
#endif

// Validates the header, the complete record table, every name and register
// order (nondecreasing per stream when grouped, else across the table). Grouped
// parses reject records whose streams are not nondecreasing in 0..3. An empty
// signature succeeds without a table. Output remains untouched on failure.
uint32_t d3dalloy_dxbc_signature_parse(const void *data, uint64_t length, uint32_t format, uint32_t grouped,
                                   D3DalloyDxbcSignatureInfo *out);
// Requires unmodified info from a successful parse of the same immutable input.
uint32_t d3dalloy_dxbc_signature_parameter(const void *data, uint64_t length, const D3DalloyDxbcSignatureInfo *info,
                                       uint32_t index, D3DalloyDxbcSignatureParameter *out);

#ifdef __cplusplus
}
#endif
