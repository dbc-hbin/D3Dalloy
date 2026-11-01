#pragma once

/*
 * Shader Model 6.x stages of the multi-stage mesh pipelines (VS+GS, VS+HS / HS+DS).
 *
 * The DXBC converters (convert_dxbc_vertex_for_geometry_shader, convert_dxbc_geometry_shader,
 * convert_dxbc_vertex_hull_shader, convert_dxbc_tesselator_domain_shader) own the Metal
 * object/mesh ABI. A DXIL stage plugs into them through a per-compile SM50ShaderInternal
 * view: the cached shader's reflection state plus signature handlers, mesh output handlers
 * and a `body_emitter` that links the parsed DXIL module into the converter's module and
 * transplants the lowered entry body into the converter's function.
 */

#include <memory>
#include <string>
#include <vector>

#include "llvm/IR/IRBuilder.h"

#include "dxil_converter.hpp"
#include "dxil_signature.hpp"

namespace d3dalloy::dxil {

enum class PipelineKind {
  GeometryObject,     // SM50CompileGeometryPipelineVertex (VS body, GS state)
  GeometryMesh,       // SM50CompileGeometryPipelineGeometry (VS state, GS body)
  TessellationObject, // SM50CompileTessellationPipelineHull (VS and HS bodies)
  TessellationMesh,   // SM50CompileTessellationPipelineDomain (HS state, DS body)
};

/**
 * One `!dx.entryPoints` signature element. dx.op I/O instructions address elements by this
 * id; the ISG1/OSG1/PCSG chunks list one entry per row instead, so their ordinals only match
 * the ids when no element spans several rows.
 */
struct IOElement {
  uint32_t id = 0;
  std::string name;
  /** DXIL::ComponentType */
  uint8_t component_type = 0;
  /** DXIL::SemanticKind */
  uint8_t semantic_kind = 0;
  std::vector<uint32_t> semantic_indices;
  uint8_t interpolation = 0;
  uint32_t rows = 1;
  uint32_t cols = 1;
  /** -1 for system values that have no register (SV_PrimitiveID, ...). */
  int32_t start_row = 0;
  int32_t start_col = 0;
  /** GS output stream. */
  uint32_t stream = 0;
};

struct EntrySignature {
  std::vector<IOElement> inputs;
  std::vector<IOElement> outputs;
  std::vector<IOElement> patch_constants;
};

/** The selected entry's signature lists from `!dx.entryPoints`. */
llvm::Expected<EntrySignature> ReadEntrySignature(const ModuleInfo &info);
const IOElement *FindElement(const std::vector<IOElement> &elements, uint32_t id);

/** One stage of a pipeline compile. */
struct PipelineStage {
  /** The cached SM50Initialize shader; never mutated by a compile. */
  const dxbc::SM50ShaderInternal *source = nullptr;
  /** Per-compile state handed to the converter for a DXIL stage. */
  dxbc::SM50ShaderInternal view;
  /**
   * Parsed DXIL module of a stage whose body is emitted, in the pipeline's shared context.
   * LinkStageModule moves the module into the converter's module on first use.
   */
  std::unique_ptr<ModuleInfo> info;
  EntrySignature signature;
  /** Entry point (and HS patch-constant function) inside the converter's module once linked. */
  llvm::Function *entry = nullptr;
  llvm::Function *patch_constant = nullptr;
  /** Element table the VS path's LowerStageIO resolves against (filled by its signature handler). */
  StageSignature vertex_signature;
  /** First error raised from a signature handler (they cannot return one). */
  std::string error;
  /** Unique tag of the stage inside the pipeline module ("vs", "gs", "hs", "ds"). */
  std::string tag;
};

/** True when the SM50Initialize shader carries DXIL. */
bool IsDXILShader(const void *shader);

/** Compile one multi-stage pipeline function where at least one of the stages is DXIL. */
llvm::Expected<std::unique_ptr<SM50CompiledBitcodeInternal>> CompilePipelineStage(
  PipelineKind kind, dxbc::SM50ShaderInternal *first, dxbc::SM50ShaderInternal *second,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *function_name
);

/* ---- SM50Initialize side ------------------------------------------------------------ */

/** Output signature in the DXBC representation the converters and SO capture read. */
std::vector<dxbc::Signature> BuildOutputSignature(const std::vector<SigElement> &elements);

/** GS reflection state (primitive, topology, max vertex count, instance count, registers). */
llvm::Error PrepareGeometryShader(const ShaderMetadata &metadata, dxbc::SM50ShaderInternal &shader);
/** HS/DS reflection state (dxil_pipeline_ts.cpp). Called after the generic field copies. */
llvm::Error PrepareHullShader(const ShaderMetadata &metadata, dxbc::SM50ShaderInternal &shader);
llvm::Error PrepareDomainShader(const ShaderMetadata &metadata, dxbc::SM50ShaderInternal &shader);

/* ---- Stage bindings: fill PipelineStage::view of a stage whose body is emitted -------- */

llvm::Error BindVertexStage(PipelineStage &stage);
llvm::Error BindGeometryStage(PipelineStage &stage);
/** dxil_pipeline_ts.cpp */
llvm::Error BindHullStage(PipelineStage &stage);
llvm::Error BindDomainStage(PipelineStage &stage);

/* ---- Helpers for body emitters -------------------------------------------------------- */

/**
 * Link the stage module into `target` (the converter's module) on first use. The entry
 * point is renamed `d3dalloy.dxil.<tag>` (the HS patch-constant function `d3dalloy.dxil.<tag>.pc`),
 * module storage is relocated to AIR address spaces, every other definition is internalized
 * and the `dx.types.*` names the shared context uniqued are restored, so two stages of one
 * pipeline never collide. Sets `stage.entry` / `stage.patch_constant`.
 */
llvm::Error LinkStageModule(PipelineStage &stage, llvm::Module &target);

/** Lower the stage's dx.op resource operations against the converter's binding map. */
llvm::Error LowerStageResources(PipelineStage &stage, dxbc::context &ctx);

/** Register-file lane (i32 / float) converted to a dx.op I/O value type. */
llvm::Value *NarrowLane(llvm::IRBuilder<> &builder, llvm::Value *lane, llvm::Type *type);
/** dx.op I/O value widened to a 32 bit register lane. */
llvm::Value *WidenLane(llvm::IRBuilder<> &builder, llvm::Value *value, bool is_signed);

/**
 * Pointer to lane (`row`, `col`) of a packed vec4 register file: `registers` points at
 * `[N x <4 x i32|float>]`, `prefix` are the leading GEP indices (for per-vertex arrays).
 * The float view is used when `as_float`.
 */
llvm::Value *RegisterLane(
  llvm::IRBuilder<> &builder, const dxbc::register_file &registers, bool as_float,
  llvm::ArrayRef<llvm::Value *> prefix, llvm::Value *row, uint32_t col
);

/** Constant operand `index` of a dx.op call. */
llvm::Expected<uint32_t> ConstantOperand(const llvm::CallInst &call, unsigned index);

/** Every call to a `dx.op.<name>.*` overload inside `function`. */
llvm::SmallVector<llvm::CallInst *, 16> CollectCalls(llvm::Function &function, llvm::StringRef name);

} // namespace d3dalloy::dxil
