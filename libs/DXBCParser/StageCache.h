// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

enum {
    DXMT_STAGE_CACHE_SUCCESS = 0,
    DXMT_STAGE_CACHE_INVALID_ARGUMENT = 1,
    DXMT_STAGE_CACHE_INVALID_DATA = 2,
    DXMT_STAGE_CACHE_OUTPUT_SIZE = 3,
};

typedef struct DxmtStageCacheRange {
    uint64_t offset;
    uint64_t size;
} DxmtStageCacheRange;

// All ranges refer to the caller-owned encoded file, including metadata_sizes.
// The checksum hashes checksum_header followed by checksum_payload; the checksum
// field and the four trailing header padding bytes are excluded.
typedef struct DxmtStageCacheLayout {
    DxmtStageCacheRange library;
    DxmtStageCacheRange metadata_sizes;
    DxmtStageCacheRange metadata;
    DxmtStageCacheRange constants;
    DxmtStageCacheRange relocations;
    uint64_t constant_alignment;
    DxmtStageCacheRange checksum_header;
    DxmtStageCacheRange checksum_payload;
    DxmtStageCacheRange checksum;
} DxmtStageCacheLayout;

// Input byte regions need no alignment. metadata_sizes contains little-endian
// u32 lengths, constants contains u64 words, and relocations contains pairs of
// little-endian u64 offset/target values. identity points to exactly 48 bytes.
typedef struct DxmtStageCacheParts {
    const void *key;
    const void *library;
    const void *metadata_sizes;
    const void *metadata;
    const void *constants;
    const void *relocations;
    const void *identity;
    uint64_t key_size;
    uint64_t library_size;
    uint64_t metadata_sizes_size;
    uint64_t metadata_size;
    uint64_t constants_size;
    uint64_t relocations_size;
    uint64_t constant_alignment;
} DxmtStageCacheParts;

#ifdef __cplusplus
static_assert(sizeof(DxmtStageCacheRange) == 16, "stage cache range ABI");
static_assert(sizeof(DxmtStageCacheLayout) == 136, "stage cache layout ABI");
extern "C" {
#endif

// All pointer-bearing inputs remain readable and immutable for the entire call.
// A null byte-region pointer is permitted only when its size is zero. Record and
// scalar pointers must be nonnull and naturally aligned. Writable outputs must
// not overlap any input or each other; no borrowed view or ownership crosses the
// ABI. Every output, including the destination file, is untouched on failure.
// Check file_size before allocating a whole-file buffer.
uint32_t dxmt_stage_cache_file_size(uint64_t file_size);
uint32_t dxmt_stage_cache_measure(const DxmtStageCacheParts *parts, uint64_t *out_size);
// Encode preserves the existing writer's acceptance rules. Decode additionally
// validates MTLB magic, reader alignment limits and relocation offsets/targets.
// Encode zeroes the checksum and header padding; C++ fills the returned checksum
// range using its existing SHA1 implementation after a successful call.
// Decode validates structure, key and identity, not SHA1. Before publishing an
// artifact, the caller must hash the two returned spans and compare the digest
// with the returned checksum range.
uint32_t dxmt_stage_cache_encode(const DxmtStageCacheParts *parts, void *destination,
                                 uint64_t destination_size, DxmtStageCacheLayout *out);
uint32_t dxmt_stage_cache_decode(const void *data, uint64_t size, const void *key,
                                 uint64_t key_size, const void *identity, DxmtStageCacheLayout *out);

#ifdef __cplusplus
}
#endif
