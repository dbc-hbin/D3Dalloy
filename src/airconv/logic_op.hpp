#pragma once

#include "airconv_public.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace dxmt::logic_op {
/* Rewrites the single native fragment entry so every render target in options.rt_mask returns
 * op(shader output, framebuffer value) masked to the format's component widths (D3D11.3 FS 17.7). */
llvm::Error applyLogicOp(llvm::Module &module, const SM50_SHADER_D3D12_LOGIC_OP_DATA &options);
}
