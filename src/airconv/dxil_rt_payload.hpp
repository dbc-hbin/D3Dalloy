#pragma once

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"

namespace d3dalloy::dxil {

// DXC encodes read=1/write=2 in four bits per role: caller, closesthit, miss, anyhit.
llvm::Error ValidateRayPayloadAnnotations(llvm::Module &module);
llvm::MDNode *RayPayloadAnnotation(llvm::Module &module, llvm::Type *type);
void CopyQualifiedRayPayload(
  llvm::IRBuilder<> &builder, llvm::Value *destination, llvm::Value *source,
  llvm::StructType *type, llvm::MDNode *fields, unsigned access_bit
);

} // namespace d3dalloy::dxil
