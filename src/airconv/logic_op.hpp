#pragma once

#include "airconv_public.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace dxmt::logic_op {
/* Rewrites the native fragment's existing epilogues in place; it never reinvokes the application shader.
 * UINT targets in options.rt_mask return op(output, framebuffer) masked to their storage component widths.
 * MSAA callers must use the execution-frequency contract in SM50_SHADER_D3D12_LOGIC_OP_DATA. */
llvm::Error applyLogicOp(llvm::Module &module, const SM50_SHADER_D3D12_LOGIC_OP_DATA &options);
}
