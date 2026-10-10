#pragma once

#include "airconv_public.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

namespace dxmt::msaa {
void preparePixelTarget(llvm::Module &module, const SM50_SHADER_D3D12_MSAA_PIXEL_DATA &options);
uint64_t userHash(llvm::StringRef semantic);
llvm::Value *lineBlend(llvm::IRBuilder<> &builder, llvm::Value *a, llvm::Value *z, llvm::Value *fraction);
/* Private capture/replay transport only; never cloned application SRV/UAV loads. */
llvm::LoadInst *capturedLoad(llvm::IRBuilder<> &builder, llvm::Type *type, llvm::Value *pointer,
                           llvm::MaybeAlign alignment = {});
/* `coarse`: SM50_SHADER_D3D12_COARSE_SHADING is present (variable-rate coarse pixel shading, msaa_coarse.hpp). */
/* `conservative`: SM50_SHADER_D3D12_CONSERVATIVE_RASTER; geometric coverage comes from the gather pass. */
llvm::Error transformPixel(llvm::Module &module, const SM50_SHADER_D3D12_MSAA_PIXEL_DATA &options, bool coarse,
                           bool conservative);
llvm::Error maskSampleFrequency(llvm::Module &module, uint32_t sampleMask);
}
