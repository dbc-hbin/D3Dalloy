#pragma once

#include "airconv_public.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace dxmt::msaa {
/* `coarse`: SM50_SHADER_D3D12_COARSE_SHADING is present (variable-rate coarse pixel shading, msaa_coarse.hpp). */
/* `conservative`: SM50_SHADER_D3D12_CONSERVATIVE_RASTER; geometric coverage comes from the gather pass. */
llvm::Error transformPixel(llvm::Module &module, const SM50_SHADER_D3D12_MSAA_PIXEL_DATA &options, bool coarse,
                           bool conservative);
llvm::Error maskSampleFrequency(llvm::Module &module, uint32_t sampleMask);
}
