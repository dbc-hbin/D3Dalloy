#pragma once

/*
 * DXIL (Shader Model 6.x) -> AIR lowering contract.
 *
 * Implementations:
 *   dxil_metadata.cpp        container/bitcode metadata parsing (owned-frontend-core)
 *   dxil_converter.cpp       SM 6.x entry points + orchestration  (owned-frontend-core)
 *   dxil_signature.cpp       stage I/O + system values            (owned-stage-lowering)
 *   dxil_resources.cpp       dx.op resource operations            (owned-resource-lowering)
 *   dxil_operations.cpp      dx.op arithmetic/math operations     (owned-operation-lowering)
 *   dxil_rt_operations.cpp   dx.op ray tracing operations         (owned-rt-compiler)
 *   dxil_raytracing.cpp      ray tracing pipeline entry point     (owned-rt-compiler)
 *
 * The parsed module is lowered *in place*: LowerStage adds the AIR function(s) to
 * ModuleInfo::module, so no IR is cloned across LLVMContext instances. ModuleInfo is
 * constructed per compile (never cached on SM50ShaderInternal) so entry points stay
 * independent and thread safe.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include "dxbc_converter.hpp"
#include "shader_common.hpp"

namespace dxmt::dxil {

using llvm::StringRef;

// ---------------------------------------------------------------------------
// Container access. All reads are bounds checked; malformed input yields an
// llvm::Error instead of reading out of bounds.
// ---------------------------------------------------------------------------

/** True when the bytecode is a DXBC container carrying a `DXIL` chunk. */
bool IsDXILContainer(const void *bytecode, size_t size);

struct DXILProgram {
  /** Bitcode without the DXIL program header; points into the caller's bytecode. */
  const uint8_t *bitcode = nullptr;
  size_t bitcode_size = 0;
  /** Raw ProgramVersion dword of the DXIL chunk. */
  uint32_t program_version = 0;
  /** Shader kind stored in bits 16..23 of ProgramVersion (D3D10_SB program type order). */
  uint32_t shader_kind = 0;
};

llvm::Expected<DXILProgram>
FindDXILProgram(const void *bytecode, size_t size);

/**
 * One fourcc chunk of a DXBC container, as a view into the caller's bytecode.
 *
 * The raytracing front end reads the container's RDAT chunk directly - that is
 * where a DXIL library records the public name of each of its entry points - and a
 * second walk of the container in another translation unit would be a second place
 * for the chunk bounds checks to drift from this one.
 */
struct DXBCChunkView {
  const uint8_t *data = nullptr;
  size_t size = 0;
};

/** False when the bytecode is not a DXBC container or holds no such chunk. */
bool FindDXBCChunk(const void *bytecode, size_t size, const char *fourcc, DXBCChunkView &out);

// ---------------------------------------------------------------------------
// DXIL enumerations (values are the on-disk DXIL ABI values).
// ---------------------------------------------------------------------------

/**
 * Entry point stage. 0..5 match microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE; 6..14
 * are the DXIL `ShaderKind` numbers (`DXIL::ShaderKind` in DXC's DxilConstants.h),
 * which is what `!dx.entryPoints` properties tag 8 carries and what the runtime's
 * OwnedRTStage uses.
 */
enum class Stage : uint32_t {
  Pixel = 0,
  Vertex = 1,
  Geometry = 2,
  Hull = 3,
  Domain = 4,
  Compute = 5,
  Library = 6,
  RayGeneration = 7,
  Intersection = 8,
  AnyHit = 9,
  ClosestHit = 10,
  Miss = 11,
  Callable = 12,
  Mesh = 13,
  Amplification = 14,
  Invalid = 0xffffffffu,
};

/** True for the stages that only exist inside a raytracing library. */
inline bool IsRaytracingStage(Stage stage) {
  switch (stage) {
  case Stage::RayGeneration:
  case Stage::Intersection:
  case Stage::AnyHit:
  case Stage::ClosestHit:
  case Stage::Miss:
  case Stage::Callable:
    return true;
  default:
    return false;
  }
}

/** Which `!dx.resources` list a declaration comes from; identical to DxilResourceBase::Class. */
enum class ResourceClass : uint32_t {
  SRV = 0,
  UAV = 1,
  CBV = 2,
  Sampler = 3,
};

enum class ResourceKind : uint32_t {
  Invalid = 0,
  Texture1D = 1,
  Texture2D = 2,
  Texture2DMS = 3,
  Texture3D = 4,
  TextureCube = 5,
  Texture1DArray = 6,
  Texture2DArray = 7,
  Texture2DMSArray = 8,
  TextureCubeArray = 9,
  TypedBuffer = 10,
  RawBuffer = 11,
  StructuredBuffer = 12,
  CBuffer = 13,
  Sampler = 14,
  TBuffer = 15,
  RTAccelerationStructure = 16,
  FeedbackTexture2D = 17,
  FeedbackTexture2DArray = 18,
};

/** `DXIL::ComponentType` (DxilConstants.h): the sampled-format element type. */
enum class ComponentType : uint32_t {
  Invalid = 0,
  I1 = 1,
  I16 = 2,
  U16 = 3,
  I32 = 4,
  U32 = 5,
  I64 = 6,
  U64 = 7,
  F16 = 8,
  F32 = 9,
  F64 = 10,
  SNormF16 = 11,
  UNormF16 = 12,
  SNormF32 = 13,
  UNormF32 = 14,
  SNormF64 = 15,
  UNormF64 = 16,
};

microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE to_program_type(Stage stage);

/** Scalar lane type of a resource's element, as used by the AIR binding tables. */
shader::common::ScalerDataType to_scaler_type(ComponentType type);

// ---------------------------------------------------------------------------
// Parsed metadata
// ---------------------------------------------------------------------------

/** One ISG1/OSG1/PSG1 signature element. */
struct SigElement {
  /** Ordinal inside the signature chunk (0-based). */
  uint32_t id = 0;
  uint32_t semantic_index = 0;
  std::string name;
  /** microsoft::D3D_REGISTER_COMPONENT_TYPE */
  uint8_t comp_type = 0;
  /** microsoft::D3D_NAME system value, 0 = undefined. */
  uint8_t semantic_kind = 0;
  /** microsoft::D3D11_SB_INTERPOLATION_MODE (pixel shader inputs), 0 = undefined. */
  uint8_t interp = 0;
  /** 4 bit write mask. */
  uint32_t mask = 0;
  uint32_t rows = 1;
  uint32_t cols = 1;
  uint32_t start_row = 0;
  uint32_t start_col = 0;
  /** Output signatures only. */
  uint8_t read_write_mask = 0;
  uint32_t stream = 0;

  bool is_system_value() const { return semantic_kind != 0; }
};

/** One `!dx.resources` declaration (a single range). */
struct ResourceBinding {
  ResourceClass klass = ResourceClass::SRV;
  /**
   * `dx.op` declaration id (metadata operand 0). This is what `dx.op.createHandle`
   * receives as its range id; it is NOT the register and NOT the array ordinal.
   */
  uint32_t declaration_id = 0;
  /** Register space (metadata operand 3). */
  uint32_t space = 0;
  /**
   * Register / lower bound of the range (metadata operand 4). This is the key used
   * by the existing DXBC binding tables, i.e. `ResourceRange::range_id`.
   */
  uint32_t lower_bound = 0;
  /** Number of descriptors in the range (metadata operand 5). */
  uint32_t range_size = 1;
  /** Constant buffer size in bytes (CBV declarations only, metadata operand 6). */
  uint32_t size_in_bytes = 0;
  /** Byte stride of a structured buffer (0 for everything else). */
  uint32_t structure_stride = 0;
  /** SRV/UAV only; CBV/Sampler declarations get CBuffer/Sampler. */
  ResourceKind kind = ResourceKind::Invalid;
  /** Element scalar type; for typed buffers/textures from the sampled format tag. */
  ComponentType component_type = ComponentType::Invalid;
  bool globally_coherent = false;
  bool has_counter = false;
  bool rasterizer_order = false;
  /**
   * Usage, recovered by scanning `dx.op` call sites. DXIL carries no static
   * read/write/sampled marker the way DXBC operands do, so core derives these and
   * the stage lowering copies them into `dxbc::ShaderInfo`'s reflection maps.
   */
  bool read = false;
  bool written = false;
  bool sampled = false;
  bool compared = false;
  bool with_counter = false;
  /** Global variable from `!dx.resources` (may be null for library declarations). */
  llvm::Value *symbol = nullptr;
  std::string name;

  bool is_buffer() const {
    return kind == ResourceKind::TypedBuffer || kind == ResourceKind::RawBuffer ||
           kind == ResourceKind::StructuredBuffer || kind == ResourceKind::CBuffer ||
           kind == ResourceKind::TBuffer;
  }
};

struct ModuleInfo {
  /** Owned context: typed pointers, DXIL bitcode constraints preserved. */
  std::unique_ptr<llvm::LLVMContext> context;
  /** Owned parsed DXIL module. Lowering happens in place. */
  std::unique_ptr<llvm::Module> module;

  llvm::Function *entry = nullptr;
  Stage stage = Stage::Invalid;
  std::string entry_name;

  uint32_t shader_model_major = 0;
  uint32_t shader_model_minor = 0;
  uint32_t dxil_major = 0;
  uint32_t dxil_minor = 0;
  uint32_t valver_major = 0;
  uint32_t valver_minor = 0;

  uint32_t num_threads[3] = {0, 0, 0};

  /**
   * Output registers written through `dx.op.storeOutput`, mirroring the SM 5.x
   * reflection which counts actual output writes rather than declarations.
   */
  uint32_t output_register_mask = 0;
  uint32_t max_output_register = 0;
  bool has_coverage_output = false;

  std::vector<SigElement> input_sig;
  std::vector<SigElement> output_sig;
  std::vector<SigElement> patch_constant_sig;
  std::vector<ResourceBinding> resources;

  const ResourceBinding *find_resource(ResourceClass klass, uint32_t declaration_id) const;
  const ResourceBinding *
  find_resource_at(ResourceClass klass, uint32_t space, uint32_t lower_bound) const;

  /**
   * Parse bytecode into IR + metadata. `entry_name` (may be null) selects one
   * `!dx.entryPoints` entry; otherwise the first entry wins.
   */
  static llvm::Expected<std::unique_ptr<ModuleInfo>> Parse(
    const void *bytecode, size_t size, const char *entry_name = nullptr
  );
};

/** Reflection-only view; the IR is parsed, read and discarded immediately. */
struct ShaderMetadata {
  Stage stage = Stage::Invalid;
  std::string entry_name;
  uint32_t shader_model_major = 0;
  uint32_t shader_model_minor = 0;
  uint32_t num_threads[3] = {0, 0, 0};
  uint32_t output_register_mask = 0;
  uint32_t max_output_register = 0;
  bool has_coverage_output = false;
  std::vector<SigElement> input_sig;
  std::vector<SigElement> output_sig;
  std::vector<SigElement> patch_constant_sig;
  std::vector<ResourceBinding> resources;
};

llvm::Expected<ShaderMetadata>
ParseMetadata(const void *bytecode, size_t size, const char *entry_name = nullptr);

// ---------------------------------------------------------------------------
// Lowering
// ---------------------------------------------------------------------------

/**
 * Lower function I/O, system values and the entry signature, and build the binding
 * map for the shader. Runs on a freshly parsed ModuleInfo, mutates `module.module`
 * and `shader`.
 */
llvm::Expected<std::unique_ptr<dxbc::BindingMap>> LowerStage(
  ModuleInfo &module, dxbc::SM50ShaderInternal &shader, const char *function_name,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
);

/** Lower `dx.op` resource operations (createHandle, loads, stores, samples, atomics). */
llvm::Error LowerResources(ModuleInfo &module, dxbc::BindingMap &binding);

/**
 * Context of one raytracing export being lowered: which record it runs as, how to
 * reach the pipeline-wide tables, and the AIR slot numbers they are bound at.
 * Filled by dxil_raytracing.cpp from SM50_RT_PIPELINE_DESC + SM50_RT_EXPORT.
 */
struct RaytracingExportContext {
  /** SM50_RT_STAGE of this export. */
  uint32_t stage = 0;
  /** Export index, i.e. the record index the runtime publishes for this shader. */
  uint32_t export_index = 0;
  /**
   * Export indices of the hit-group members this export runs as, taken straight
   * from SM50_RT_EXPORT. `0xffffffff` (SM50_RT_INDEX_NONE) means "this hit group
   * has no shader of that kind"; a triangle hit group has no intersection shader
   * and a procedural one has no closest-hit-is-self. The per-candidate callback
   * layer resolves the group's other members through these.
   */
  uint32_t closest_hit_index = 0xffffffffu;
  uint32_t any_hit_index = 0xffffffffu;
  uint32_t intersection_index = 0xffffffffu;
  /** Group export -> visible closest-hit slot, or NONE for a group without one. */
  const uint32_t *hit_group_continuations = nullptr;
  uint32_t hit_group_continuation_count = 0;
  /** Local root table slot/stride the record's local root arguments are read from. */
  uint32_t local_root_table_slot = 22;
  /** Binding slots of the pipeline-wide tables (dxil_raytracing.h). */
  uint32_t dispatch_arguments_slot = 20;
  uint32_t intersection_function_table_slot = 21;
  uint32_t visible_function_table_slot = 23;
  /** Byte size of SM50_RT_FRAME and of the user payload area that follows it. */
  uint32_t frame_size = 0;
  uint32_t payload_size = 0;
  uint32_t attribute_size = 0;
  /** Byte size of SM50_RT_HIT_STATE, published by the stage builder. */
  uint32_t hit_state_size = 0;
  /** Byte size of the per-candidate region (kRtCandidateRegionSize). */
  uint32_t candidate_size = 0;
  /**
   * The per-invocation block layout, as offsets inside the block.
   *
   * `rtInvocationLayout` (dxil_rt_callbacks.hpp) is the function every party calls to
   * get these - the stage builder sizes the ray generation shader's block with it, the
   * traversal lowering allocates the per-callsite blocks with it, and the callback
   * layer addresses every region from the block base - but it is computed from these
   * accessors, so the sizes above stay the only inputs and the order is stated once.
   *
   *   +0                 SM50_RT_FRAME (frame_size bytes)
   *   +frame_size        user payload (payload_size)
   *   +hitStateOffset()  SM50_RT_HIT_STATE
   *   +attributeOffset() committed hit's attributes (attribute_size)
   *   +candidateOffset() per-candidate control block, then the candidate's own
   *                      attributes (kRtCandidateRegionSize)
   */
  uint32_t hitStateOffset() const { return frame_size + payload_size; }
  uint32_t attributeOffset() const { return hitStateOffset() + hit_state_size; }
  uint32_t candidateOffset() const { return attributeOffset() + attribute_size; }
  uint32_t invocationBlockSize() const { return candidateOffset() + candidate_size; }
  uint32_t max_recursion_depth = 1;
  /** SM50_RT_PIPELINE_FLAG, constant for the whole pipeline. */
  uint32_t pipeline_flags = 0;
  /** Global root signature blob, shared by every export. */
  const void *global_root_signature = nullptr;
  uint64_t global_root_signature_size = 0;
  /** Local root signature blob of this export, or null. */
  const void *local_root_signature = nullptr;
  uint64_t local_root_signature_size = 0;
  /** The export is the pipeline's dispatch entry (the single ray generation shader). */
  bool is_dispatch_entry = false;
  /**
   * The export's binding map, as `LowerRaytracingStage` built it.
   *
   * A raytracing export resolves its resources through the root signature it was
   * associated with, and an acceleration structure is one of those resources: its
   * descriptor holds the D3D12 GPU virtual address the traversal lowering loads the
   * structure's header from. The map therefore has to reach
   * `LowerRaytracingOperations`, which runs after the stage builder.
   */
  dxbc::BindingMap *binding = nullptr;
};

/**
 * Lower one raytracing export of a DXIL library into an AIR function.
 *
 * The DXIL entry point body is transplanted into the AIR function built here, the
 * raytracing `dx.op` operations are lowered, and the function is registered under
 * `air.kernel` (ray generation), `air.intersection` (intersection) or `air.visible`
 * (miss / closest-hit / any-hit / callable). Returns the binding map of the export,
 * exactly like LowerStage.
 */
llvm::Expected<std::unique_ptr<dxbc::BindingMap>> LowerRaytracingStage(
  ModuleInfo &module, const RaytracingExportContext &context,
  dxbc::SM50ShaderInternal &shader, const char *function_name,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
);

/**
 * Link one lowered export into the pipeline module. The export is serialized to
 * bitcode and parsed into `target`'s context first, because every ModuleInfo owns
 * its own LLVMContext (DXIL needs typed pointers) while the metallib writer takes a
 * single module.
 */
llvm::Error MergeRaytracingExport(
  llvm::Module &target, llvm::Module &source, llvm::StringRef entry_name
);

/** Lower the `dx.op` raytracing operations of one export on an already staged module. */
llvm::Error LowerRaytracingOperations(
  ModuleInfo &info, const RaytracingExportContext &context, llvm::StringRef entry_name
);

/**
 * Resolved arguments of a lowered raytracing export, recovered from the AIR entry
 * metadata the stage builder wrote plus the `dxmt.rt.locals` node that names the
 * per-invocation allocations.
 */
struct RaytracingEntryArguments {
  llvm::Function *function = nullptr;
  llvm::Value *dispatch_arguments = nullptr;
  llvm::Value *intersection_function_table = nullptr;
  llvm::Value *visible_function_table = nullptr;
  llvm::Value *thread_position = nullptr;
  /**
   * The two entries every root-signature shader of the dispatch shares: the
   * pipeline's global root arguments (the root signature's qword entry) and the
   * static sampler table.
   *
   * Only a stage that declares them as arguments has them here - the ray
   * generation shader above all. A visible function cannot declare them (the call
   * through the visible function table has a fixed argument list), so it takes
   * their addresses out of its invocation frame instead; they are null for it.
   */
  llvm::Value *root_arguments = nullptr;
  llvm::Value *static_samplers = nullptr;
  /** i8* per-invocation block (frame + user payload). */
  llvm::Value *payload = nullptr;
  /**
   * i32 export index of this invocation and its current recursion depth.
   *
   * A visible function receives both as AIR arguments (they are the record and
   * depth the traversal passed), so the resolver prefers the argument over the
   * constant the stage builder publishes for the ray generation shader.
   */
  llvm::Value *record_index = nullptr;
  llvm::Value *depth = nullptr;
};

/** True when the module holds a lowered raytracing export with this name. */
llvm::Error ResolveRaytracingEntryArguments(
  llvm::Module &module, llvm::StringRef entry_name, RaytracingEntryArguments &out
);

/** DXIL `ShaderKind` (SM50_RT_STAGE) -> Stage. Returns Stage::Invalid when unknown. */
Stage stageFromShaderKind(uint32_t shader_kind);

/**
 * `!dx.entryPoints` properties tag carrying the entry's `DXIL::ShaderKind`
 * (DXC's `kDxilShaderKindTag`). It is the only place a library entry's stage is
 * recorded: `!dx.shaderModel` is "lib" for every entry of a container.
 */
inline constexpr uint32_t kDxilShaderKindTag = 8;

/**
 * Drop the DXIL scaffolding (debug info, `dx.*` metadata, the original entry point
 * and dead declarations) from a module whose AIR entry functions are already
 * registered under an `air.*` named metadata kind.
 */
void StripDXILArtifacts(llvm::Module &module);

/** Lower the remaining `dx.op` operations on the (already staged) module. */
llvm::Error LowerOperations(llvm::Module &module);

/** Reject a module that still contains `dx.op` calls or non-lowerable instructions. */
llvm::Error VerifyLoweredModule(llvm::Module &module);

/**
 * Publish a DXIL module's resource declarations into a ShaderInfo.
 *
 * The root-signature binding layer resolves a shader's resources through
 * `ShaderInfo`'s cbuffer/sampler/SRV/UAV maps, keyed by declaration id, so any path
 * that hands a ShaderInfo to `setup_binding_rootsig` has to fill them first. The
 * raytracing pipeline builds its own SM50ShaderInternal and therefore calls this
 * directly (LowerRaytracingStage); PrepareShader calls the same function, so the
 * ordinary and the raytracing DXIL paths cannot drift apart.
 */
void PopulateShaderResourceBindings(
  dxbc::ShaderInfo &shader_info, llvm::ArrayRef<ResourceBinding> resources
);

/**
 * SM50Initialize side of the DXIL path: parses the metadata, fills the shared
 * ShaderInfo/argument reflection, stores the bytecode for the compile step and
 * reports the reflection. Unsupported stages fail here with an explicit error.
 */
llvm::Error PrepareShader(
  dxbc::SM50ShaderInternal &shader, const void *bytecode, size_t size,
  MTL_SHADER_REFLECTION *reflection
);

/**
 * SM50Compile side: parses the stored bytecode, lowers stage, resources and
 * operations, verifies the result and strips the DXIL scaffolding. Returns the
 * ModuleInfo owning the lowered AIR module.
 */
llvm::Expected<std::unique_ptr<ModuleInfo>> LowerShader(
  dxbc::SM50ShaderInternal &shader, const char *function_name,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
);



/** Error helper shared by the DXIL translation units. */
llvm::Error createError(const llvm::Twine &message);
llvm::Error unsupported(StringRef what);

/*
 * Opaque handles behind sm50_bitcode_t and sm50_error_t.
 *
 * Both are owned by the caller-facing entry points in dxbc_converter.cpp
 * (SM50GetCompiledBitcode / SM50DestroyBitcode / SM50GetErrorMessage /
 * SM50FreeError), which cast the handle straight back to these types, so every
 * translation unit that produces a handle has to use the same definitions. They
 * live here so the DXIL raytracing entry points share them instead of duplicating
 * a layout-dependent copy.
 */
class SM50CompiledBitcodeInternal {
public:
  llvm::SmallVector<char, 0> vec;
  llvm::SmallVector<uint8_t, 0> constants;
  llvm::SmallVector<SM50_CONSTANT_RELOCATION, 0> constant_relocations;
  uint64_t constant_alignment = 1;
  /**
   * The function names the raytracing reflection points at.
   *
   * `SM50_RT_FUNCTION_INFO::Name` is a bare `const char *` handed to the caller, so the
   * strings it points at have to outlive the compile call. A raytracing pipeline builds
   * them in a local vector, which dies on return and leaves the reflection naming freed
   * memory - the runtime then sees an unnamed function. The vector is moved into this
   * object, which the caller owns through `sm50_bitcode_t` until `SM50DestroyBitcode`,
   * so the pointers stay valid exactly as long as the metallib they describe.
   */
  std::vector<std::string> function_names;
};

/** Lift normal-stage immutable storage into an explicit constant buffer.
 * Raytracing libraries retain their existing callback ABI. */
llvm::Error LowerImmutableConstants(llvm::Module &module, SM50CompiledBitcodeInternal &compiled);

class SM50ErrorInternal {
public:
  llvm::SmallVector<char, 0> buf;
};

} // namespace dxmt::dxil
