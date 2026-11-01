// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// All offsets refer to the original caller-owned byte array. The caller keeps it
// readable and immutable for each call and while any returned view is in use.
// No Rust allocation, pointer-bearing record, or Rust enum crosses this ABI.
enum {
    D3DALLOY_SHADER_BYTECODE_SUCCESS = 0,
    D3DALLOY_SHADER_BYTECODE_INVALID_ARGUMENT = 1,
    D3DALLOY_SHADER_BYTECODE_TRUNCATED_CONTAINER = 2,
    D3DALLOY_SHADER_BYTECODE_CONTAINER_MAGIC = 3,
    D3DALLOY_SHADER_BYTECODE_CONTAINER_VERSION = 4,
    D3DALLOY_SHADER_BYTECODE_CONTAINER_SIZE = 5,
    D3DALLOY_SHADER_BYTECODE_CHUNK_TABLE = 6,
    D3DALLOY_SHADER_BYTECODE_CHUNK_OFFSET = 7,
    D3DALLOY_SHADER_BYTECODE_CHUNK_SIZE = 8,
    D3DALLOY_SHADER_BYTECODE_CHUNK_LAYOUT = 9,
    D3DALLOY_SHADER_BYTECODE_NOT_FOUND = 10,
    D3DALLOY_SHADER_BYTECODE_TRUNCATED_DXIL = 11,
    D3DALLOY_SHADER_BYTECODE_DXIL_ENCODING = 12,
    D3DALLOY_SHADER_BYTECODE_BITCODE_OFFSET = 13,
    D3DALLOY_SHADER_BYTECODE_BITCODE_SIZE = 14,
    D3DALLOY_SHADER_BYTECODE_BITCODE_MAGIC = 15,
    D3DALLOY_SHADER_BYTECODE_RDAT_LAYOUT = 16,
    D3DALLOY_SHADER_BYTECODE_RDAT_STRING = 17,
    D3DALLOY_SHADER_BYTECODE_RDAT_KIND = 18,
};

enum {
    // Legacy CDXBCParser requires the caller's exact declared extent.
    D3DALLOY_SHADER_CONTAINER_LEGACY = 0,
    // Root extraction allows caller padding, retaining the contiguous layout.
    D3DALLOY_SHADER_CONTAINER_ROOT_SIGNATURE = 1,
    // DXIL permits indexed gaps, reordered chunks, and caller padding.
    D3DALLOY_SHADER_CONTAINER_DXIL = 2,
};

typedef struct D3DalloyShaderContainerInfo {
    uint32_t size;
    uint32_t chunk_count;
    uint32_t version_major;
    uint32_t version_minor;
    uint8_t hash[16];
} D3DalloyShaderContainerInfo;

typedef struct D3DalloyShaderChunk {
    uint32_t index;
    uint32_t fourcc;
    uint32_t offset;
    uint32_t size;
} D3DalloyShaderChunk;

typedef struct D3DalloyDxilProgram {
    uint32_t program_version;
    uint32_t shader_kind;
    uint32_t bitcode_offset;
    uint32_t bitcode_size;
} D3DalloyDxilProgram;

typedef struct D3DalloyRdatInfo {
    uint32_t strings_offset;
    uint32_t strings_size;
    uint32_t records_offset;
    uint32_t record_count;
    uint32_t record_stride;
} D3DalloyRdatInfo;

typedef struct D3DalloyRdatFunction {
    uint32_t name_offset;
    uint32_t name_size;
    uint32_t public_name_offset;
    uint32_t public_name_size;
} D3DalloyRdatFunction;

typedef struct D3DalloyShaderSpan {
    uint32_t offset;
    uint32_t size;
} D3DalloyShaderSpan;

enum {
    D3DALLOY_SHADER_RDAT_STRINGS = 1,
    D3DALLOY_SHADER_RDAT_INDEX_ARRAYS = 2,
    D3DALLOY_SHADER_RDAT_RAW_BYTES = 4,
};

typedef struct D3DalloyRdatSubobjects {
    // Zero when the container has no RDAT chunk or no subobject table part.
    uint32_t present;
    uint32_t record_count;
    uint32_t record_stride;
    uint32_t records_offset;
    // D3DALLOY_SHADER_RDAT_* bits of the parts that exist; the last of each type wins.
    uint32_t parts;
    D3DalloyShaderSpan strings;
    D3DalloyShaderSpan indices;
    D3DalloyShaderSpan raw;
} D3DalloyRdatSubobjects;

typedef struct D3DalloyRdatSubobject {
    uint32_t kind;
    // Kind-specific payload words after { Kind, Name }.
    uint32_t words[4];
    D3DalloyShaderSpan name;
    // SubobjectToExportsAssociation: [0] the associated subobject.
    // HitGroup: any-hit, closest-hit and intersection names; empty means absent.
    D3DalloyShaderSpan strings[3];
    // Global/LocalRootSignature: the RTS0 payload.
    D3DalloyShaderSpan root_signature;
    uint32_t export_count;
    uint32_t exports_offset;
} D3DalloyRdatSubobject;

#ifdef __cplusplus
static_assert(sizeof(D3DalloyShaderSpan) == 8, "shader span ABI");
static_assert(sizeof(D3DalloyRdatSubobjects) == 44, "RDAT subobject table ABI");
static_assert(sizeof(D3DalloyRdatSubobject) == 68, "RDAT subobject ABI");
static_assert(sizeof(D3DalloyShaderContainerInfo) == 32, "shader container ABI");
static_assert(sizeof(D3DalloyShaderChunk) == 16, "shader chunk ABI");
static_assert(sizeof(D3DalloyDxilProgram) == 16, "DXIL program ABI");
static_assert(sizeof(D3DalloyRdatInfo) == 20, "RDAT table ABI");
static_assert(sizeof(D3DalloyRdatFunction) == 16, "RDAT function ABI");
extern "C" {
#endif

// Fixed-width lengths are checked against the platform's addressable slice size.
// Outputs remain untouched on failure. Parse, find and DXIL program extraction
// validate every chunk before returning, even when an earlier chunk matches.
uint32_t d3dalloy_shader_container_parse(const void *data, uint64_t length, uint32_t policy,
                                     D3DalloyShaderContainerInfo *out);
// O(1) access after a successful parse of the same immutable buffer. This still
// bounds-checks the header, index and selected chunk, without rescanning siblings.
uint32_t d3dalloy_shader_container_chunk(const void *data, uint64_t length, uint32_t policy,
                                     uint32_t index, D3DalloyShaderChunk *out);
uint32_t d3dalloy_shader_container_find(const void *data, uint64_t length, uint32_t policy,
                                    uint32_t fourcc, uint32_t start_index, D3DalloyShaderChunk *out);
uint32_t d3dalloy_shader_dxil_program(const void *data, uint64_t length, D3DalloyDxilProgram *out);

// Parse the container and every RDAT part extent before publishing table metadata.
// Function access requires unmodified info from a successful parse of the same
// immutable input. It checks the selected record and both NUL-terminated names;
// invalid/empty names return RDAT_STRING so consumers can skip that record.
// Returned name spans exclude NUL and use offsets into the original container.
uint32_t d3dalloy_shader_rdat_parse(const void *data, uint64_t length, D3DalloyRdatInfo *out);
uint32_t d3dalloy_shader_rdat_function(const void *data, uint64_t length, const D3DalloyRdatInfo *info,
                                  uint32_t index, D3DalloyRdatFunction *out);

// Subobject tables of a legacy-layout container. A missing RDAT chunk or subobject
// part succeeds with present = 0. Records are { Kind, Name, four words } with a
// stride of at least 24 bytes. Record access validates the nonempty name first,
// then the kind payload: root-signature raw spans (nonempty), association subobject
// name and every nonempty export name, hit-group type and names. Unknown kinds
// return RDAT_KIND; invalid names return RDAT_STRING; other layout faults RDAT_LAYOUT.
uint32_t d3dalloy_shader_rdat_subobjects(const void *data, uint64_t length, D3DalloyRdatSubobjects *out);
uint32_t d3dalloy_shader_rdat_subobject(const void *data, uint64_t length, const D3DalloyRdatSubobjects *table,
                                    uint32_t index, D3DalloyRdatSubobject *out);
// Name span of one export of an association read from the same input and table.
uint32_t d3dalloy_shader_rdat_export(const void *data, uint64_t length, const D3DalloyRdatSubobjects *table,
                                 const D3DalloyRdatSubobject *subobject, uint32_t index, D3DalloyShaderSpan *out);

// Only reads the declared-size field. Pointer-only legacy callers must already
// guarantee a readable header and an allocation covering that declared size.
uint32_t d3dalloy_shader_container_declared_size(const void *data, uint64_t length, uint32_t *out);
int32_t d3dalloy_validate_root_signature(const void *data, uint64_t length);

#ifdef __cplusplus
}
#endif
