// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// D3D12_PIPELINE_STATE_STREAM_DESC subobject walk. The stream is borrowed and
// immutable for each call; payloads are reinterpreted by the caller.
enum {
    D3DALLOY_PSO_STREAM_SUCCESS = 0,
    D3DALLOY_PSO_STREAM_INVALID_ARGUMENT = 1,
    D3DALLOY_PSO_STREAM_TRUNCATED = 2,
    D3DALLOY_PSO_STREAM_UNKNOWN_TYPE = 3,
    D3DALLOY_PSO_STREAM_DUPLICATE = 4,
};

#define D3DALLOY_PSO_STREAM_TYPES 64u

// sizeof/alignof of the payload of each subobject type; size 0 = unhandled.
typedef struct D3DalloyPsoStreamLayout {
    uint32_t size;
    uint32_t alignment;
} D3DalloyPsoStreamLayout;

typedef struct D3DalloyPsoStreamSubobject {
    uint64_t payload_offset;
    // The following subobject; at or past the stream end after the last one.
    uint64_t next_offset;
    // `defined` with this type's bit added.
    uint64_t defined;
    uint32_t type;
    uint32_t reserved;
} D3DalloyPsoStreamSubobject;

#ifdef __cplusplus
static_assert(sizeof(D3DalloyPsoStreamSubobject) == 32, "pso stream subobject ABI");
extern "C" {
#endif

// The subobject at offset (< length) after the types in the `defined` bit mask.
// Errors, in order: TRUNCATED type word, UNKNOWN_TYPE >= 64, DUPLICATE type or
// revision group, UNKNOWN_TYPE unhandled, TRUNCATED subobject.
uint32_t d3dalloy_pso_stream_next(const void *data, uint64_t length, const D3DalloyPsoStreamLayout layouts[64],
                              uint64_t offset, uint64_t defined, D3DalloyPsoStreamSubobject *out);

#ifdef __cplusplus
}
#endif
