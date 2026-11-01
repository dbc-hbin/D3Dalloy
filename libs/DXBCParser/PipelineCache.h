// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// Inputs are borrowed, readable and immutable for each call. Struct pointers use
// their native alignment; byte buffers may be unaligned. Writable outputs must
// not overlap any input or each other. Nothing is retained or allocated by Rust.
// Native identity and SHA1 remain caller-owned opaque byte sequences.
enum {
    D3DALLOY_PIPELINE_CACHE_SUCCESS = 0,
    D3DALLOY_PIPELINE_CACHE_INVALID_ARGUMENT = 1,
    D3DALLOY_PIPELINE_CACHE_VERSION_MISMATCH = 2,
    D3DALLOY_PIPELINE_CACHE_STAGE_COUNT = 3,
};

typedef struct D3DalloyPipelineCacheDigest {
    uint8_t bytes[20];
} D3DalloyPipelineCacheDigest;

typedef struct D3DalloyPipelineCacheStageRecord {
    uint64_t library_size;
    uint64_t constant_size;
    uint64_t constant_alignment;
    uint64_t relocation_count;
} D3DalloyPipelineCacheStageRecord;

typedef struct D3DalloyPipelineCacheDescription {
    uint64_t adapter;
    uint32_t stages;
    uint32_t reserved;
    uint8_t identity[48];
    D3DalloyPipelineCacheDigest key;
    uint32_t reserved_tail;
    D3DalloyPipelineCacheStageRecord records[D3DALLOY_PIPELINE_CACHE_STAGE_COUNT];
} D3DalloyPipelineCacheDescription;

typedef struct D3DalloyPipelineCacheStageOffsets {
    uint64_t library;
    uint64_t constants;
    uint64_t relocations;
} D3DalloyPipelineCacheStageOffsets;

typedef struct D3DalloyPipelineCacheLayout {
    uint64_t size;
    // SHA1 covers [0, digest_begin) followed by [digest_end, size).
    uint64_t digest_begin;
    uint64_t digest_end;
    D3DalloyPipelineCacheStageOffsets offsets[D3DALLOY_PIPELINE_CACHE_STAGE_COUNT];
} D3DalloyPipelineCacheLayout;

typedef struct D3DalloyPipelineCacheInfo {
    D3DalloyPipelineCacheDescription description;
    D3DalloyPipelineCacheLayout layout;
    D3DalloyPipelineCacheDigest digest;
    uint32_t reserved;
} D3DalloyPipelineCacheInfo;

#ifdef __cplusplus
static_assert(sizeof(D3DalloyPipelineCacheDigest) == 20, "pipeline digest ABI");
static_assert(sizeof(D3DalloyPipelineCacheStageRecord) == 32, "pipeline stage ABI");
static_assert(sizeof(D3DalloyPipelineCacheDescription) == 184, "pipeline description ABI");
static_assert(sizeof(D3DalloyPipelineCacheStageOffsets) == 24, "pipeline offsets ABI");
static_assert(sizeof(D3DalloyPipelineCacheLayout) == 96, "pipeline layout ABI");
static_assert(sizeof(D3DalloyPipelineCacheInfo) == 304, "pipeline info ABI");
extern "C" {
#endif

// Header-only lookup deliberately does not inspect stage records or payload.
// Magic is checked before version, and version before the declared total size.
// All record outputs remain untouched on failure.
uint32_t d3dalloy_pipeline_cache_key(const void *data, uint64_t length, D3DalloyPipelineCacheDigest *out);
// Validates the header, stage lengths and exact aligned extent, not SHA1,
// adapter/native identity or payload. Those policy checks remain in the caller.
uint32_t d3dalloy_pipeline_cache_decode(const void *data, uint64_t length, D3DalloyPipelineCacheInfo *out);
uint32_t d3dalloy_pipeline_cache_layout(const D3DalloyPipelineCacheDescription *description, D3DalloyPipelineCacheLayout *out);
// Requires unmodified info from decode of the same immutable input. Call after
// SHA1, adapter and identity checks to preserve their precedence over payload.
uint32_t d3dalloy_pipeline_cache_validate_payload(const void *data, uint64_t length, const D3DalloyPipelineCacheInfo *info);
// Creates the exact v7 header and zeroes the complete planned destination.
// The caller then fills payload spans using its owner-aware compiler read API;
// Rust must never dereference compiler-owned, potentially inaccessible pointers.
uint32_t d3dalloy_pipeline_cache_begin(const D3DalloyPipelineCacheDescription *description, void *data, uint64_t length);
// Patches SHA1 bytes after the caller has hashed the layout's exclusion spans.
uint32_t d3dalloy_pipeline_cache_set_digest(void *data, uint64_t length, const D3DalloyPipelineCacheDigest *digest);

#ifdef __cplusplus
}
#endif
