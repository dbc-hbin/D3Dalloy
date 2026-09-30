#pragma once

#include "airconv_public.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace dxmt::msaa {
llvm::Error transformPixel(llvm::Module &module, const SM50_SHADER_D3D12_MSAA_PIXEL_DATA &options);
llvm::Error maskSampleFrequency(llvm::Module &module, uint32_t sampleMask);
}
