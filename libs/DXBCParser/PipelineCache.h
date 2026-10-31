// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// Inputs are borrowed, readable and immutable for each call. Struct pointers use
// their native alignment; byte buffers may be unaligned. Writable outputs must
// not overlap any input or each other. Nothing is retained or allocated by Rust.
// Native identity and SHA1 remain caller-owned opaque byte sequences.
enum {
    DXMT_PIPELINE_CACHE_SUCCESS = 0,
    DXMT_PIPELINE_CACHE_INVALID_ARGUMENT = 1,
    DXMT_PIPELINE_CACHE_VERSION_MISMATCH = 2,
    DXMT_PIPELINE_CACHE_STAGE_COUNT = 3,
};

typedef struct DxmtPipelineCacheDigest {
    uint8_t bytes[20];
} DxmtPipelineCacheDigest;

typedef struct DxmtPipelineCacheStageRecord {
    uint64_t library_size;
    uint64_t constant_size;
    uint64_t constant_alignment;
    uint64_t relocation_count;
} DxmtPipelineCacheStageRecord;

typedef struct DxmtPipelineCacheDescription {
    uint64_t adapter;
    uint32_t stages;
    uint32_t reserved;
    uint8_t identity[48];
    DxmtPipelineCacheDigest key;
    uint32_t reserved_tail;
    DxmtPipelineCacheStageRecord records[DXMT_PIPELINE_CACHE_STAGE_COUNT];
} DxmtPipelineCacheDescription;

typedef struct DxmtPipelineCacheStageOffsets {
    uint64_t library;
    uint64_t constants;
    uint64_t relocations;
} DxmtPipelineCacheStageOffsets;

typedef struct DxmtPipelineCacheLayout {
    uint64_t size;
    // SHA1 covers [0, digest_begin) followed by [digest_end, size).
    uint64_t digest_begin;
    uint64_t digest_end;
    DxmtPipelineCacheStageOffsets offsets[DXMT_PIPELINE_CACHE_STAGE_COUNT];
} DxmtPipelineCacheLayout;

typedef struct DxmtPipelineCacheInfo {
    DxmtPipelineCacheDescription description;
    DxmtPipelineCacheLayout layout;
    DxmtPipelineCacheDigest digest;
    uint32_t reserved;
} DxmtPipelineCacheInfo;

#ifdef __cplusplus
static_assert(sizeof(DxmtPipelineCacheDigest) == 20, "pipeline digest ABI");
static_assert(sizeof(DxmtPipelineCacheStageRecord) == 32, "pipeline stage ABI");
static_assert(sizeof(DxmtPipelineCacheDescription) == 184, "pipeline description ABI");
static_assert(sizeof(DxmtPipelineCacheStageOffsets) == 24, "pipeline offsets ABI");
static_assert(sizeof(DxmtPipelineCacheLayout) == 96, "pipeline layout ABI");
static_assert(sizeof(DxmtPipelineCacheInfo) == 304, "pipeline info ABI");
extern "C" {
#endif

// Header-only lookup deliberately does not inspect stage records or payload.
// Magic is checked before version, and version before the declared total size.
// All record outputs remain untouched on failure.
uint32_t dxmt_pipeline_cache_key(const void *data, uint64_t length, DxmtPipelineCacheDigest *out);
// Validates the header, stage lengths and exact aligned extent, not SHA1,
// adapter/native identity or payload. Those policy checks remain in the caller.
uint32_t dxmt_pipeline_cache_decode(const void *data, uint64_t length, DxmtPipelineCacheInfo *out);
uint32_t dxmt_pipeline_cache_layout(const DxmtPipelineCacheDescription *description, DxmtPipelineCacheLayout *out);
// Requires unmodified info from decode of the same immutable input. Call after
// SHA1, adapter and identity checks to preserve their precedence over payload.
uint32_t dxmt_pipeline_cache_validate_payload(const void *data, uint64_t length, const DxmtPipelineCacheInfo *info);
// Creates the exact v7 header and zeroes the complete planned destination.
// The caller then fills payload spans using its owner-aware compiler read API;
// Rust must never dereference compiler-owned, potentially inaccessible pointers.
uint32_t dxmt_pipeline_cache_begin(const DxmtPipelineCacheDescription *description, void *data, uint64_t length);
// Patches SHA1 bytes after the caller has hashed the layout's exclusion spans.
uint32_t dxmt_pipeline_cache_set_digest(void *data, uint64_t length, const DxmtPipelineCacheDigest *digest);

#ifdef __cplusplus
}
#endif
