// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// D3D12_PIPELINE_STATE_STREAM_DESC subobject walk. The stream is borrowed and
// immutable for each call; payloads are reinterpreted by the caller.
enum {
    DXMT_PSO_STREAM_SUCCESS = 0,
    DXMT_PSO_STREAM_INVALID_ARGUMENT = 1,
    DXMT_PSO_STREAM_TRUNCATED = 2,
    DXMT_PSO_STREAM_UNKNOWN_TYPE = 3,
    DXMT_PSO_STREAM_DUPLICATE = 4,
};

#define DXMT_PSO_STREAM_TYPES 64u

// sizeof/alignof of the payload of each subobject type; size 0 = unhandled.
typedef struct DxmtPsoStreamLayout {
    uint32_t size;
    uint32_t alignment;
} DxmtPsoStreamLayout;

typedef struct DxmtPsoStreamSubobject {
    uint64_t payload_offset;
    // The following subobject; at or past the stream end after the last one.
    uint64_t next_offset;
    // `defined` with this type's bit added.
    uint64_t defined;
    uint32_t type;
    uint32_t reserved;
} DxmtPsoStreamSubobject;

#ifdef __cplusplus
static_assert(sizeof(DxmtPsoStreamSubobject) == 32, "pso stream subobject ABI");
extern "C" {
#endif

// The subobject at offset (< length) after the types in the `defined` bit mask.
// Errors, in order: TRUNCATED type word, UNKNOWN_TYPE >= 64, DUPLICATE type or
// revision group, UNKNOWN_TYPE unhandled, TRUNCATED subobject.
uint32_t dxmt_pso_stream_next(const void *data, uint64_t length, const DxmtPsoStreamLayout layouts[64],
                              uint64_t offset, uint64_t defined, DxmtPsoStreamSubobject *out);

#ifdef __cplusplus
}
#endif
