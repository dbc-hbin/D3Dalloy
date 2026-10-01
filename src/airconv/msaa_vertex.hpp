#pragma once

#include "airconv_public.h"
#include "llvm/Support/Error.h"

namespace llvm {
class Module;
}

namespace dxmt::msaa {

// Rewrites one independently lowered AIR vertex entry. Capture calls the
// original stage once per IA ordinal; replay reads only its native output.
// `conservative` (SM50_SHADER_D3D12_CONSERVATIVE_RASTER) selects the conservative
// triangle replay geometry for a replay variant without a line mode.
llvm::Error transformVertex(llvm::Module &module, const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options,
                            bool conservative);

} // namespace dxmt::msaa
