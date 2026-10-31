// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// ID3D12PipelineLibrary serialized container. Inputs are borrowed, readable and
// immutable for each call; writable outputs must not overlap inputs. Native
// identity, SHA1, names, PSO blobs and their order remain caller-owned.
enum {
    DXMT_PIPELINE_LIBRARY_SUCCESS = 0,
    DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT = 1,
    DXMT_PIPELINE_LIBRARY_VERSION_MISMATCH = 2,
    DXMT_PIPELINE_LIBRARY_OVERFLOW = 3,
};

typedef struct DxmtPipelineLibraryHeader {
    uint64_t adapter;
    uint64_t size;
    uint32_t count;
    uint32_t reserved;
    uint8_t identity[48];
    uint8_t digest[20];
    uint32_t reserved_tail;
} DxmtPipelineLibraryHeader;

// Offsets into the serialized library. name_offset/blob_offset are 8-aligned
// and next_offset is the following record (or the end).
typedef struct DxmtPipelineLibraryEntry {
    uint64_t name_offset;
    uint64_t blob_offset;
    uint64_t blob_size;
    uint64_t next_offset;
    uint32_t name_bytes;
    uint32_t kind;
    uint8_t key[20];
    uint32_t reserved;
} DxmtPipelineLibraryEntry;

// Running serialized size and entry count; start with { 104, 0 }.
typedef struct DxmtPipelineLibraryExtent {
    uint64_t size;
    uint64_t count;
} DxmtPipelineLibraryExtent;

typedef struct DxmtPipelineLibraryRecord {
    uint64_t name_bytes;
    uint64_t blob_size;
    uint32_t kind;
    uint32_t reserved;
    uint8_t key[20];
    uint32_t reserved_tail;
} DxmtPipelineLibraryRecord;

#define DXMT_PIPELINE_LIBRARY_HEADER_SIZE 104u
// SHA1 covers [0, 80) followed by [100, size).
#define DXMT_PIPELINE_LIBRARY_DIGEST_BEGIN 80u
#define DXMT_PIPELINE_LIBRARY_DIGEST_END 100u

#ifdef __cplusplus
static_assert(sizeof(DxmtPipelineLibraryHeader) == 96, "pipeline library header ABI");
static_assert(sizeof(DxmtPipelineLibraryEntry) == 64, "pipeline library entry ABI");
static_assert(sizeof(DxmtPipelineLibraryExtent) == 16, "pipeline library extent ABI");
static_assert(sizeof(DxmtPipelineLibraryRecord) == 48, "pipeline library record ABI");
extern "C" {
#endif

// Adds one entry: rejects odd or >u32 name byte counts, empty blobs, a count
// past u32 and a total past isize (OVERFLOW). Output untouched on failure.
uint32_t dxmt_pipeline_library_extend(const DxmtPipelineLibraryExtent *extent, uint64_t name_bytes,
                                      uint64_t blob_size, DxmtPipelineLibraryExtent *out);
// Magic is checked before version, and version before size and reserved fields.
// Adapter, identity and digest are returned for the caller's policy checks.
uint32_t dxmt_pipeline_library_header(const void *data, uint64_t length, DxmtPipelineLibraryHeader *out);
// Bounds the record at offset with its padded name and blob inside the input,
// and validates name parity, reserved, kind and nonempty blob.
uint32_t dxmt_pipeline_library_entry(const void *data, uint64_t length, uint64_t offset,
                                     DxmtPipelineLibraryEntry *out);
// Zeroes the complete destination of description->size bytes and writes the
// header without digest. adapter, size, count and identity are used.
uint32_t dxmt_pipeline_library_begin(const DxmtPipelineLibraryHeader *description, void *data, uint64_t length);
// Writes the record at offset and returns where the caller copies name and blob.
uint32_t dxmt_pipeline_library_put_entry(void *data, uint64_t length, uint64_t offset,
                                         const DxmtPipelineLibraryRecord *record, DxmtPipelineLibraryEntry *out);
uint32_t dxmt_pipeline_library_set_digest(void *data, uint64_t length, const uint8_t digest[20]);

#ifdef __cplusplus
}
#endif
