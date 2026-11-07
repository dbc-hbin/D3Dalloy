#pragma once
#include "llvm/IR/Module.h"
#include "llvm/Passes/OptimizationLevel.h"

namespace llvm { class IRBuilderBase; }

namespace d3dalloy {

void initializeModule(llvm::Module &M);

llvm::Value *createTextureIsNull(llvm::IRBuilderBase &builder, llvm::Value *texture);

void runOptimizationPasses(llvm::Module &M);
void lowerBoundedLoads(llvm::Module &M);

void linkMSAD(llvm::Module &M);

void linkSamplePos(llvm::Module &M);

void linkTessellation(llvm::Module &M);

/** Links the exact fp32 denorm-preserve helpers (d3dalloy.denorm.*, air_denorm.metal). */
void linkDenorm(llvm::Module &M);
/** Links the integer-only exact binary64 helpers (d3dalloy.fp64.*, air_fp64.cpp). */
void linkFP64(llvm::Module &M);
/** Links only SC32 metadata operations; application i64 CAS is fully emulated. */
void linkAtomic64(llvm::Module &M);
/** Links the D3D12 sampler feedback writer (d3dalloy.sampler_feedback.write, air_sampler_feedback.metal). */
void linkSamplerFeedback(llvm::Module &M);
/** Links the conservative rasterization replay corner (d3dalloy.conservative.corner, air_conservative.metal). */
void linkConservative(llvm::Module &M);

} // namespace d3dalloy
