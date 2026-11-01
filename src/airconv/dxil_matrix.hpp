#pragma once
#include <cstdint>
#include "llvm/Support/Error.h"
namespace llvm { class Module; class CallInst; }
namespace d3dalloy::dxil {
struct ModuleInfo;
// Pinned DXC v1.8.2306-preview WaveMatrix and v1.10.2605.37 LinAlg are
// distinct contracts. Final LinAlg uses the published 322..342 signatures.
llvm::Expected<bool> ValidateMatrixContract(llvm::Module &module, uint32_t major, uint32_t minor);
llvm::Expected<bool> IsMatrixOperation(const llvm::CallInst &call);
llvm::Error LowerMatrices(ModuleInfo &info);
}
