// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdint.h>

// DXBC SHDR/SHEX program token bounds. data/length is the chunk payload; the
// caller keeps it readable and immutable for each call. Offsets count tokens.
enum {
    D3DALLOY_DXBC_PROGRAM_SUCCESS = 0,
    D3DALLOY_DXBC_PROGRAM_INVALID_ARGUMENT = 1,
    D3DALLOY_DXBC_PROGRAM_HEADER = 2,
    D3DALLOY_DXBC_PROGRAM_LENGTH = 3,
    D3DALLOY_DXBC_PROGRAM_INSTRUCTION_LENGTH = 4,
    D3DALLOY_DXBC_PROGRAM_OPCODE = 5,
    D3DALLOY_DXBC_PROGRAM_TRUNCATED = 6,
    D3DALLOY_DXBC_PROGRAM_OPERAND = 7,
};

typedef struct D3DalloyDxbcProgram {
    uint32_t version_token;
    // Declared program length, at least 2 and inside the payload.
    uint32_t length_tokens;
} D3DalloyDxbcProgram;

typedef struct D3DalloyDxbcInstruction {
    uint32_t opcode;
    // Nonzero; [offset, offset + length_tokens) covers every token the decoder
    // reads for this instruction and lies inside the declared program.
    uint32_t length_tokens;
} D3DalloyDxbcInstruction;

#ifdef __cplusplus
static_assert(sizeof(D3DalloyDxbcProgram) == 8, "DXBC program ABI");
static_assert(sizeof(D3DalloyDxbcInstruction) == 8, "DXBC instruction ABI");
extern "C" {
#endif

uint32_t d3dalloy_dxbc_program_parse(const void *data, uint64_t length, D3DalloyDxbcProgram *out);
// operand_counts holds the generic operand count of each of opcode_count
// opcodes (D3D10_SB_NUM_OPCODES). Rejects opcodes outside that table, extended
// custom data, invalid component selection and relative-index nesting past 4.
uint32_t d3dalloy_dxbc_instruction_parse(const void *data, uint64_t length, const uint8_t *operand_counts,
                                     uint32_t opcode_count, uint32_t token_offset, D3DalloyDxbcInstruction *out);

#ifdef __cplusplus
}
#endif
