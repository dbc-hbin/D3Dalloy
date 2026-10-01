#pragma once

/*
 * DXIL stage signature lowering: the `!dx.entryPoints` signature elements of a
 * vertex / pixel / compute entry point are turned into AIR function arguments,
 * a prologue that fills the packed 4x32 input register file and an epilogue that
 * assembles the AIR return struct from the packed output register file.
 *
 * The table below is what `dx.op.loadInput` (4) and `dx.op.storeOutput` (5) are
 * resolved against; both instructions address a signature element by its
 * metadata id and its component, never by a register.
 */

#include <cstdint>
#include <vector>

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include "dxbc_converter.hpp"
#include "dxil_converter.hpp"

namespace dxmt::dxil {

/**
 * Where one signature element lives in `dxbc::io_binding_map::input` /
 * `output` (the packed vec4 register file shared with the DXBC path).
 */
struct SignatureSlot {
  /** `!dx.entryPoints` element id, i.e. `dx.op` operand 1. */
  uint32_t element_id = 0;
  uint32_t element_row = 0;
  uint32_t semantic_index = 0;
  /** Register of the element (signature start row). */
  uint32_t reg = 0;
  /** Number of vec4 registers the element spans (signature rows). */
  uint32_t rows = 1;
  /** First component inside `reg`. */
  uint32_t start_col = 0;
  /** Component mask inside the element's register. */
  uint32_t mask = 0;
  /** Index of the AIR function argument the element is bound to, ~0u if none. */
  uint32_t air_index = ~0u;
  /** AIR argument is a pull-mode interpolant rather than an eagerly interpolated value. */
  bool pull_mode = false;
  /** AIR `vertex_value` argument for GetAttributeAtVertex (SM 6.1), ~0u if not read per vertex. */
  uint32_t vertex_value_index = ~0u;
  /** DXIL interpolation mode from the signature metadata. */
  uint8_t interpolation = 0;
  /** microsoft::D3D_REGISTER_COMPONENT_TYPE */
  uint8_t comp_type = 0;
  /** microsoft::D3D_NAME */
  uint8_t semantic_kind = 0;
};

/** Element lookup for `dx.op` I/O instructions, indexed by element id. */
struct StageSignature {
  std::vector<SignatureSlot> inputs;
  std::vector<SignatureSlot> outputs;

  const SignatureSlot *find_input(uint32_t element_id, uint32_t row) const;
  const SignatureSlot *find_output(uint32_t element_id, uint32_t row) const;
};

/**
 * Define the AIR stage I/O of `module` on `shader.func_signature` and register the
 * matching prologue/epilogue work on `sig_ctx`. Fills `out_signature`.
 */
llvm::Error DeclareStageSignature(
  const ModuleInfo &module, dxbc::SM50ShaderInternal &shader, dxbc::SignatureContext &sig_ctx,
  StageSignature &out_signature
);

/** SV_ViewID of the current invocation: the root view-id qword, or 0 without root arguments. */
llvm::Value *ViewInstanceId(dxbc::context &ctx);

/** Lower every `dx.op.viewID` of the AIR function (stages whose I/O is not lowered by LowerStageIO). */
llvm::Error LowerViewInstanceId(dxbc::context &ctx);

/** Lower every `dx.op.loadInput` / `dx.op.storeOutput` of the AIR function. */
llvm::Error LowerStageIO(dxbc::context &ctx, const StageSignature &signature);

/**
 * Move the body of the DXIL entry point into `function`, replacing each of its
 * `ret` instructions with a branch to `exit`. Returns the first transplanted
 * block, which `function`'s entry block has to branch to. Static entry allocas
 * move into the AIR entry so they remain static after transplant and inlining.
 */
llvm::Expected<llvm::BasicBlock *>
TransplantEntryBody(llvm::Function &entry, llvm::Function &function, llvm::BasicBlock &exit);

/**
 * Relocate the module-scope storage of `module` into the AIR address spaces.
 *
 * Every path that lowers a DXIL module to AIR needs this, and it needs it on the
 * module, not per entry point: a module-scope global is shared by every entry
 * point of a library, and the relocation is idempotent, so a second call after
 * the globals have been moved finds nothing to move. Callers therefore call it
 * once per module, before lowering the entry point(s) that address that storage.
 *
 * The relocation redirects the uses of a relocated immutable global through an
 * addrspacecast into its own address space. Folding that cast (and the flat
 * GEP/load chain behind it) back onto the storage's address space is what
 * `dxmt::runOptimizationPasses` does through `InferAddressSpacesPass(0)`; a path
 * that prepares storage therefore also has to run the optimization pipeline
 * before it serializes AIR, or the shader keeps dereferencing constant storage
 * as if it were device memory.
 */
void PrepareModuleStorage(llvm::Module &module);

} // namespace dxmt::dxil
