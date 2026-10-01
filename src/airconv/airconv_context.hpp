#pragma once
#include "llvm/IR/Module.h"
#include "llvm/Passes/OptimizationLevel.h"

namespace dxmt {

void initializeModule(llvm::Module &M);

void runOptimizationPasses(llvm::Module &M);

void linkMSAD(llvm::Module &M);

void linkSamplePos(llvm::Module &M);

void linkTessellation(llvm::Module &M);

/** Links the exact fp32 denorm-preserve helpers (dxmt.denorm.*, air_denorm.metal). */
void linkDenorm(llvm::Module &M);
/** Links the D3D12 sampler feedback writer (dxmt.sampler_feedback.write, air_sampler_feedback.metal). */
void linkSamplerFeedback(llvm::Module &M);
/** Links the conservative rasterization replay corner (dxmt.conservative.corner, air_conservative.metal). */
void linkConservative(llvm::Module &M);

} // namespace dxmt