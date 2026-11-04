#pragma once

#include "airconv_public.h"
#include "llvm/Support/Error.h"

namespace llvm {
class Module;
}

namespace d3dalloy::msaa {

// Rewrites one independently lowered AIR vertex entry. Capture calls the
// original stage once per IA ordinal; replay reads only its native output.
// `conservative` (SM50_SHADER_D3D12_CONSERVATIVE_RASTER) selects the conservative
// triangle replay geometry for a replay variant without a line mode.
// SM50_MSAA_GPU_TILE_TASKS preserves the legacy replay geometry and clip-distance signature exactly.
// GPU-generated task descriptors select a native scissor; capture layout, interpolation and wireframe edges
// remain unchanged. Shader-side eligibility is reflected in layout.flags.
llvm::Error transformVertex(llvm::Module &module, const SM50_SHADER_D3D12_MSAA_VERTEX_DATA &options,
                            bool conservative);

} // namespace d3dalloy::msaa
