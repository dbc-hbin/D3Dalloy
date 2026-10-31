// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// Data-only preflight of a PE image file (no SEC_IMAGE mapping). The file
// bytes are borrowed and immutable for each call; outputs are written only on
// success. File I/O, loading and identity policy stay with the caller.
enum {
    DXMT_PE_SUCCESS = 0,
    DXMT_PE_INVALID_ARGUMENT = 1,
    DXMT_PE_UNSUPPORTED = 2,
    DXMT_PE_NOT_FOUND = 3,
};

typedef struct DxmtPeImage {
    uint64_t nt_offset;
    uint64_t sections_offset;
    uint32_t export_rva;
    uint32_t export_size;
    uint32_t section_count;
    uint32_t machine;
    uint64_t names_offset;
    uint64_t ordinals_offset;
    uint64_t functions_offset;
    uint32_t name_count;
    uint32_t function_count;
} DxmtPeImage;

#ifdef __cplusplus
static_assert(sizeof(DxmtPeImage) == 64, "PE image ABI");
extern "C" {
#endif

// DOS and complete NT headers of `machine` (PE32 for I386, else PE32+), no Wine
// builtin/placeholder DOS-stub marker, 1..96 sections inside the file, an
// export directory with 1..65536 names and functions, and all three export
// tables inside section raw data. Every failure is UNSUPPORTED.
uint32_t dxmt_pe_image(const void *data, uint64_t length, uint32_t machine, DxmtPeImage *out);
// File offset of export_length bytes at the first export named exactly
// name[0..name_length). Forwarders, bad ordinals and ranges outside section raw
// data are NOT_FOUND.
uint32_t dxmt_pe_export(const void *data, uint64_t length, const DxmtPeImage *image, const char *name,
                        uint64_t name_length, uint64_t export_length, uint64_t *out);

#ifdef __cplusplus
}
#endif
