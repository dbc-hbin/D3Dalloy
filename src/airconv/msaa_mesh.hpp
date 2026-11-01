#pragma once

#include "airconv_public.h"
#include "llvm/Support/Error.h"

namespace llvm { class Module; }

namespace d3dalloy::msaa {
// Capture runs the application mesh stage once. Replay contains no application code.
llvm::Error transformMesh(llvm::Module &module, const SM50_SHADER_D3D12_MSAA_MESH_DATA &options,
                          bool conservative);
} // namespace d3dalloy::msaa
