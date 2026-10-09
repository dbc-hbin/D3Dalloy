#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "DXBCParser/DXBCUtils.h"
#include "air_operations.hpp"
#include "air_signature.hpp"
#include "dxbc_constants.hpp"
#include "dxbc_instructions.hpp"
#include "nt/air_builder.hpp"
#include "nt/dxbc_binding_map.hpp"
#include "shader_common.hpp"

#include "airconv_public.h"

namespace dxmt::dxbc {

struct ResourceRange {
  uint32_t range_id;
  uint32_t lower_bound;
  uint32_t size;
  uint32_t space;
};

struct ShaderResourceViewInfo {
  ResourceRange range;
  shader::common::ScalerDataType scaler_type;
  shader::common::ResourceType resource_type;
  bool read = false;
  bool sampled = false;
  bool compared = false; // therefore we use depth texture!

  uint32_t structure_stride = 0;
  uint32_t arg_index;
  uint32_t arg_metadata_index;
};
struct UnorderedAccessViewInfo {
  ResourceRange range;
  shader::common::ScalerDataType scaler_type;
  shader::common::ResourceType resource_type;
  bool read = false;
  bool written = false;
  bool global_coherent = false;
  bool rasterizer_order = false;
  bool with_counter = false;

  uint32_t structure_stride = 0;
  uint32_t arg_index;
  uint32_t arg_metadata_index;
  uint32_t arg_counter_index;
};
struct ConstantBufferInfo {
  ResourceRange range;
  uint32_t size_in_vec4;
  uint32_t arg_index;
};
struct SamplerInfo {
  ResourceRange range;
  uint32_t arg_index;
  uint32_t arg_cube_index;
  uint32_t arg_metadata_index;
};

struct ThreadgroupBufferInfo {
  uint32_t size_in_uint;
  uint32_t size;
  uint32_t stride;
  bool structured;
};

struct PhaseInfo {
  uint32_t tempRegisterCount = 0;
  std::map<
    uint32_t, std::pair<uint32_t /* count */, uint32_t /* mask */>>
    indexableTempRegisterCounts;
};

class ShaderInfo {
public:
  std::vector<std::array<uint32_t, 4>> immConstantBufferData;
  std::map<uint32_t, ShaderResourceViewInfo> srvMap;
  std::map<uint32_t, UnorderedAccessViewInfo> uavMap;
  std::map<uint32_t, ConstantBufferInfo> cbufferMap;
  std::map<uint32_t, SamplerInfo> samplerMap;
  std::map<uint32_t, ThreadgroupBufferInfo> tgsmMap;
  uint32_t tempRegisterCount = 0;
  std::map<
    uint32_t, std::pair<uint32_t /* count */, uint32_t /* mask */>>
    indexableTempRegisterCounts;
  air::ArgumentBufferBuilder binding_table_cbuffer;
  air::ArgumentBufferBuilder binding_table;
  bool skipOptimization = false;
  bool refactoringAllowed = false;
  bool no_control_point_phase_passthrough = false;
  bool output_control_point_read = false;
  bool use_msad = false;
  bool use_samplepos = false;
  std::vector<PhaseInfo> phases;
  uint32_t pull_mode_reg_mask = 0;
};

Instruction readInstruction(
  const microsoft::D3D10ShaderBinary::CInstruction &Inst,
  ShaderInfo &shader_info, uint32_t phase
);

using pvalue = dxmt::air::pvalue;
using epvalue = llvm::Expected<pvalue>;
using dxbc::Swizzle;
using dxbc::swizzle_identity;

struct context;
using IRValue = ReaderIO<context, pvalue>;
using IREffect = ReaderIO<context, std::monostate>;
using IndexedIRValue = std::function<IRValue(pvalue)>;

struct register_file {
  llvm::Value *ptr_int4 = nullptr;
  llvm::Value *ptr_float4 = nullptr;
};

struct indexable_register_file {
  llvm::Value *ptr_int_vec = nullptr;
  llvm::Value *ptr_float_vec = nullptr;
  uint32_t vec_size = 0;
};

struct phase_temp {
  register_file temp{};
  std::unordered_map<uint32_t, indexable_register_file> indexable_temp_map{};
};

struct interpolant_descriptor {
  IndexedIRValue interpolant;
  bool perspective;
  uint8_t mask;
};

struct io_binding_map {
  llvm::GlobalVariable *icb = nullptr;
  llvm::Value *icb_float = nullptr;
  std::unordered_map<uint32_t, std::pair<uint32_t, llvm::GlobalVariable *>>
    tgsm_map{};
  std::unordered_map<uint32_t, std::vector<interpolant_descriptor>> interpolant_map{};

  register_file input{};
  register_file output{};
  register_file temp{};
  std::unordered_map<uint32_t, indexable_register_file> indexable_temp_map{};
  std::vector<phase_temp> phases;
  register_file patch_constant_output{};
  uint32_t input_element_count = 0;
  uint32_t output_element_count = 0;

  // special registers (input)
  llvm::Value *thread_id_arg = nullptr;
  llvm::Value *thread_group_id_arg = nullptr;
  llvm::Value *thread_id_in_group_arg = nullptr;
  llvm::Value *thread_id_in_group_flat_arg = nullptr;
  llvm::Value *coverage_mask_arg = nullptr;
  llvm::Value *inner_coverage_arg = nullptr;

  llvm::Value *domain = nullptr;
  llvm::Value *patch_id = nullptr;

  llvm::Value *thread_id_in_patch = nullptr;

  llvm::Value *gs_instance_id = nullptr;

  // special registers (output)
  llvm::AllocaInst *depth_output_reg = nullptr;
  llvm::AllocaInst *stencil_ref_reg = nullptr;
  llvm::AllocaInst *coverage_mask_reg = nullptr;

  // special buffers for tessellation
  llvm::Type *hull_cp_passthrough_type = nullptr;
  llvm::Value *hull_cp_passthrough_src = nullptr;
  llvm::Value *hull_cp_passthrough_dst = nullptr;
  llvm::Value *tess_factor_buffer;    // half*

  // temp for fast look-up
  llvm::Value *vertex_id = nullptr;
  llvm::Value *vertex_id_with_base = nullptr;
  llvm::Value *instance_id = nullptr;
  llvm::Value *instance_id_with_base = nullptr;
  llvm::Value *base_vertex_id = nullptr;
  llvm::Value *base_instance_id = nullptr;
  llvm::Value *vertex_buffer_table = nullptr;
  /* D3D12 stream-output capture: IA position of this invocation (record key). */
  llvm::Value *so_capture_key = nullptr;

  // geometry shader ops
  llvm::Value *mesh = nullptr;
  /* GS emit/cut on an output stream (0..3). */
  std::function<IREffect(uint32_t)> call_emit;
  std::function<IREffect(uint32_t)> call_cut;
};

struct context {
  llvm::IRBuilder<> &builder;
  llvm::air::AIRBuilder &air;
  BindingMap &binding;
  llvm::LLVMContext &llvm;
  llvm::Module &module;
  llvm::Function *function;
  io_binding_map &resource;
  air::AirType &types; // hmmm
  uint32_t pso_sample_mask;
  microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type;
  SM50_SHADER_METAL_VERSION metal_version;
  bool capture_raw_distance_output = false;
};

template <typename S> IRValue make_irvalue(S &&fs) {
  return IRValue(std::forward<S>(fs));
}

template <typename S> IRValue make_irvalue_bind(S &&fs) {
  return IRValue([fs = std::forward<S>(fs)](auto ctx) {
    return fs(ctx).build(ctx);
  });
}

template <typename S> IREffect make_effect(S &&fs) {
  return IREffect(std::forward<S>(fs));
}

template <typename S> IREffect make_effect_bind(S &&fs) {
  return IREffect([fs = std::forward<S>(fs)](auto ctx) mutable {
    return fs(ctx).build(ctx);
  });
}

IREffect store_at_vec4_array_masked(
  llvm::Value *array, pvalue index, pvalue maybe_vec4, uint32_t mask
);

IREffect init_input_reg(
  uint32_t with_fnarg_at, uint32_t to_reg, uint32_t mask,
  bool fix_w_component = false
);

IREffect init_input_reg_semantic(uint32_t with_fnarg_at, uint32_t to_reg, uint32_t mask);

IREffect init_input_reg_with_interpolation(
  uint32_t with_fnarg_at, uint32_t to_reg, uint32_t mask,
  air::Interpolation interpolation, uint32_t sampleidx_at
);

IREffect init_input_reg_with_interpolation_semantic(
  uint32_t with_fnarg_at, uint32_t to_reg, uint32_t mask,
  air::Interpolation interpolation, uint32_t sampleidx_at
);

std::function<IRValue(pvalue)>
pop_output_reg(uint32_t from_reg, uint32_t mask, uint32_t to_element);

std::function<IRValue(pvalue)>
pop_output_reg_semantic(uint32_t from_reg, uint32_t mask, uint32_t to_element,
                        bool sanitize_distance = false);

std::function<IRValue(pvalue)>
pop_output_reg_fix_unorm(uint32_t from_reg, uint32_t mask, uint32_t to_element);

std::function<IRValue(pvalue)>
pop_output_reg_sanitize_pos(uint32_t from_reg, uint32_t mask, uint32_t to_element);

IREffect pull_vertex_input(
  air::FunctionSignatureBuilder &func_signature, uint32_t to_reg, uint32_t mask,
  SM50_IA_INPUT_ELEMENT element_info, uint32_t slot_mask, bool dense_vertex_table
);

IREffect pop_mesh_output_render_target_array_index(uint32_t from_reg, uint32_t mask, pvalue primitive_id);
IREffect pop_mesh_output_viewport_array_index(uint32_t from_reg, uint32_t mask, pvalue primitive_id);
IREffect pop_mesh_output_position(uint32_t from_reg, uint32_t mask, pvalue vertex_id);
IREffect
pop_mesh_output_vertex_data(uint32_t from_reg, uint32_t mask, uint32_t idx, pvalue vertex_id,
                            air::MSLScalerOrVectorType desired_type, bool sanitize_distance = false);

/**
 * One SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE element resolved to the output
 * register file: `count` components of register `reg` starting at absolute
 * component `component`, copied to `dword_offset` inside the record.
 */
struct StreamOutputCaptureRun {
  uint32_t reg;
  uint32_t component;
  uint32_t count;
  uint32_t dword_offset;
};

/**
 * Resolve `element` against the output element occupying register `reg` with
 * write mask `mask`. `start_component` is relative to the element's first
 * component (the D3D SO declaration contract). A range outside the mask or the
 * record fails compilation.
 */
llvm::Expected<StreamOutputCaptureRun> resolve_stream_output_capture(
  const SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA &capture, const SM50_SO_CAPTURE_ELEMENT &element,
  uint32_t reg, uint32_t mask
);

/** Declare and increment the D3D12 pipeline-statistics counter for this stage. */
uint32_t define_pipeline_statistics(air::FunctionSignatureBuilder &func_signature);
void increment_pipeline_statistics(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function,
  uint32_t argument, uint32_t low_word
);
/** SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL: [[sample_id]] and [[sample_mask]] argument indices. */
struct PixelStatisticsDedupe {
  uint32_t sample_index;
  uint32_t coverage;
};
PixelStatisticsDedupe define_pixel_statistics_dedupe(air::FunctionSignatureBuilder &func_signature);
/** PSInvocations: unconditionally, or (with `dedupe`) only in the lowest covered sample's invocation. */
void increment_pixel_invocations(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function, uint32_t argument,
  const PixelStatisticsDedupe *dedupe
);
/** SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM: the PS's key, position, layer and claim-buffer arguments. */
struct PixelStatisticsClaim {
  uint32_t key;
  uint32_t position;
  uint32_t layer;
  uint32_t coverage;
  uint32_t claim;
};
PixelStatisticsClaim define_pixel_statistics_claim(air::FunctionSignatureBuilder &func_signature);
/** PSInvocations once per (pixel, primitive): swap the primitive key into the pixel's claim entry. */
void claim_pixel_invocation(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function, uint32_t argument,
  const PixelStatisticsClaim &claim
);
/** The mesh primitive data a PRIMITIVE_CLAIM mesh function writes: declare it (at `data_index`, the first free
 * primitive data index) and set it. */
void define_mesh_statistics_key(air::FunctionSignatureBuilder &func_signature, uint32_t data_index = 0);
void set_mesh_statistics_key(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Value *primitive, llvm::Value *key0,
  llvm::Value *key1, llvm::Value *key2, uint32_t data_index = 0
);
/* Adds a (runtime, possibly zero) 32-bit amount to one 64-bit pipeline-statistics counter. */
void add_pipeline_statistics(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function,
  uint32_t argument, uint32_t low_word, llvm::Value *amount
);

/** Function argument indices shared by index pulling and SO capture. */
struct VertexIndexPullArgs {
  uint32_t control = ~0u;
  uint32_t indices = ~0u;
};

VertexIndexPullArgs define_vertex_index_pull(air::FunctionSignatureBuilder &func_signature);

/**
 * Declare the capture control/scratch/index buffers and append the record
 * writes to `epilogue`. The epilogue value passes through unchanged; the writes
 * read the output register file, so they must run after the shader body joined
 * the epilogue block.
 */
VertexIndexPullArgs define_stream_output_capture(
  air::FunctionSignatureBuilder &func_signature, IRValue &epilogue, std::vector<StreamOutputCaptureRun> runs,
  uint32_t record_dwords
);

/**
 * Must run in the entry block right after `resource.vertex_id`/`vertex_id_with_base`
 * are set and before the prologue. For indexed draws (control.index_size != 0),
 * replaces the vertex ids with the
 * application's index value (+ BaseVertexLocation for the vertex fetch).
 * control_dword_offset selects the common three words within the SO or draw control.
 */
void apply_vertex_index_pull(
  llvm::IRBuilder<> &builder, llvm::Function *function, const VertexIndexPullArgs &args, io_binding_map &resource,
  uint32_t control_dword_offset
);

llvm::Expected<llvm::BasicBlock *> convert_basicblocks(
  BasicBlock *entry, context &ctx, llvm::BasicBlock *return_bb
);

constexpr air::MSLScalerOrVectorType to_msl_type(RegisterComponentType type) {
  switch (type) {
  default:
  case RegisterComponentType::Unknown: {
    assert(0 && "unknown component type");
    break;
  }
  case RegisterComponentType::Uint:
    return air::msl_uint4;
  case RegisterComponentType::Int:
    return air::msl_int4;
  case RegisterComponentType::Float:
    return air::msl_float4;
    break;
  }
}

struct ScalarInfo {
  uint8_t component : 2;
  uint8_t reg : 6;
};

struct PatchConstantScalarInfo {
  uint8_t component : 2;
  uint8_t reg : 6;
  int8_t tess_factor_index;
};

struct SignatureContext {
  IREffect &prologue;
  IRValue &epilogue;
  air::FunctionSignatureBuilder &func_signature;
  io_binding_map &resource;
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout;
  bool dense_vertex_table;
  bool dual_source_blending;
  bool disable_depth_output;
  bool skip_vertex_output;
  bool captured_pixel = false;
  uint32_t pull_mode_reg_mask;
  uint32_t unorm_output_reg_mask;
  // it is considered optional (as a hint)
  air::MTLPixelFormat pixel_formats[8];

  SignatureContext(
      IREffect &prologue, IRValue &epilogue, air::FunctionSignatureBuilder &func_signature, io_binding_map &resource
  ) :
      prologue(prologue),
      epilogue(epilogue),
      func_signature(func_signature),
      resource(resource),
      ia_layout(nullptr),
      dense_vertex_table(false),
      dual_source_blending(false),
      disable_depth_output(false),
      skip_vertex_output(false),
      pull_mode_reg_mask(0),
      unorm_output_reg_mask(0) {
    memset(pixel_formats, 0, sizeof(pixel_formats));
  };
};

RegisterComponentType component_type_from_pixel_format(air::MTLPixelFormat format);

struct MeshOutputContext {
  llvm::Value *vertex_id;
  llvm::Value *primitive_id;
};

/* The mesh-output state one GS stream's output declarations define (SM50ShaderInternal::gs_stream_declarations). */
struct GSStreamOutputs {
  air::FunctionSignatureBuilder &func_signature;
  std::vector<std::function<IREffect(MeshOutputContext &)>> &mesh_output_handlers;
  uint32_t &num_mesh_vertex_data;
  std::vector<ScalarInfo> &clip_distance_scalars;
  std::vector<ScalarInfo> &cull_distance_scalars;
};

// VS outputs and PS inputs have independent register allocation. Link AIR user
// varyings by the D3D signature semantic instead of the stage-local register.
inline std::string varyingAttributeName(std::string_view semantic, uint32_t index) {
  std::string name = "semantic_";
  name.reserve(name.size() + semantic.size() + 12);
  for (char c : semantic)
    name += c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
  name += '_';
  name += std::to_string(index);
  return name;
}

class Signature {

public:
  Signature(const microsoft::D3D11_SIGNATURE_PARAMETER &parameter) :
      semantic_name_(parameter.SemanticName),
      semantic_index_(parameter.SemanticIndex),
      stream_(parameter.Stream),
      mask_(parameter.Mask),
      register_(parameter.Register),
      system_value_(parameter.SystemValue),
      component_type_((RegisterComponentType)parameter.ComponentType) {
    std::transform(semantic_name_.begin(), semantic_name_.end(), semantic_name_.begin(), [](auto c) {
      return std::tolower(c);
    });
  }

  Signature()
      : semantic_name_("INVALID"), semantic_index_(0), stream_(0), mask_(0),
        register_(0), system_value_(microsoft::D3D10_SB_NAME_UNDEFINED),
        component_type_(RegisterComponentType::Unknown) {}

  std::string_view semanticName() const { return semantic_name_; }

  uint32_t semanticIndex() const { return semantic_index_; }

  std::string fullSemanticString() const {
    return semantic_name_ + std::to_string(semantic_index_);
  }

  std::string consistentAttributeName() const {
    return "reg" + std::to_string(register_) + "_" + std::to_string(std::countr_zero(mask_ & 0xFu));
  }

  std::string varyingAttributeName() const {
    return dxmt::dxbc::varyingAttributeName(semantic_name_, semantic_index_);
  }

  uint32_t stream() const { return stream_; }

  uint32_t mask() const { return mask_; }

  uint32_t reg() const { return register_; }

  bool isSystemValue() const {
    return system_value_ != microsoft::D3D10_SB_NAME_UNDEFINED;
  }

  RegisterComponentType componentType() const {
    return (RegisterComponentType)component_type_;
  }

private:
  std::string semantic_name_;
  uint32_t semantic_index_;
  uint8_t stream_;
  uint8_t mask_;
  uint8_t register_;
  microsoft::D3D10_SB_NAME system_value_;
  RegisterComponentType component_type_;
};

class SM50ShaderInternal {
public:
  /**
   * Set only for Shader Model 6.x shaders: an owned copy of the DXIL container.
   * `SM50Compile` dispatches to the DXIL pipeline on this tag, everything else
   * follows the SM 5.x DXBC path.
   */
  std::optional<std::vector<uint8_t>> dxil_bytecode;

  dxmt::dxbc::ShaderInfo shader_info;
  dxmt::air::FunctionSignatureBuilder func_signature;
  std::vector<Signature> output_signature;
  std::vector<std::unique_ptr<BasicBlock>> bbs;
  std::vector<std::function<void(SignatureContext &)>> signature_handlers;
  std::string initialization_error;
  microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type;
  /* for domain shader, it refers to patch constant input count */
  uint32_t max_input_register = 0;
  uint32_t max_output_register = 0;
  uint32_t pso_valid_output_reg_mask = 0;
  uint32_t max_patch_constant_output_register = 0;
  std::vector<MTL_SM50_SHADER_ARGUMENT> args_reflection_cbuffer;
  std::vector<MTL_SM50_SHADER_ARGUMENT> args_reflection;
  uint32_t threadgroup_size[3] = {0};
  uint32_t input_control_point_count = ~0u;
  uint32_t output_control_point_count = ~0u;
  microsoft::D3D11_SB_TESSELLATOR_PARTITIONING tessellation_partition = {};
  float max_tesselation_factor = 64.0f;
  microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE tessellator_output_primitive = {};
  microsoft::D3D11_SB_TESSELLATOR_DOMAIN tessellation_domain = {};
  std::vector<PatchConstantScalarInfo> patch_constant_scalars;
  uint32_t hull_maximum_threads_per_patch = 0;
  std::vector<ScalarInfo> clip_distance_scalars;
  std::vector<ScalarInfo> cull_distance_scalars;
  microsoft::D3D10_SB_PRIMITIVE gs_input_primitive = {};
  std::vector<std::function<IREffect(MeshOutputContext &)>> mesh_output_handlers;
  uint32_t num_mesh_vertex_data = 0;
  /* DXBC GS: the output declarations of each stream, replayed by the GS converter for the stream that drives
   * the mesh output (the rasterized stream). A declaration belongs to every stream whose output signature has its
   * register; `gs_declaring_stream` is the stream it is being recorded for. */
  std::array<std::vector<std::function<void(GSStreamOutputs &)>>, 4> gs_stream_declarations;
  uint32_t gs_declaring_stream = 0;
  /* [s][r]: components of output register r already declared for stream s. */
  std::array<std::array<uint8_t, 32>, 4> gs_declared_components{};
  microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY gs_output_topology = {};
  uint32_t gs_max_vertex_output = 0;
  uint32_t gs_instance_count = 1;
  uint32_t ps_has_coverage_output = 0;
  bool ps_has_coverage_input = false;
  bool ps_has_inner_coverage_input = false;
  uint32_t ps_sample_frequency = 0;
  /* Pixel shader declares ForceEarlyDepthStencil. */
  uint32_t ps_force_early_depth_stencil = 0;
  /**
   * Stage body of a Shader Model 6.x stage inside a multi-stage (mesh) pipeline. DXIL stages
   * carry no DXBC basic blocks; the DXIL front end emits the lowered entry body into the
   * converter's function instead and returns the block the converter branches to.
   */
  std::function<llvm::Expected<llvm::BasicBlock *>(context &, llvm::BasicBlock *)> body_emitter;

  BasicBlock *entry() const {
    return bbs.front().get();
  }
};

/** The body of a pipeline stage: the DXIL emitter when present, the DXBC control flow otherwise. */
inline llvm::Expected<llvm::BasicBlock *>
convert_stage_body(const SM50ShaderInternal &shader, context &ctx, llvm::BasicBlock *return_bb) {
  if (shader.body_emitter)
    return shader.body_emitter(ctx, return_bb);
  return convert_basicblocks(shader.entry(), ctx, return_bb);
}

void handle_signature(
  microsoft::CSignatureParser &inputParser,
  microsoft::CSignatureParser5 &outputParser,
  microsoft::D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase
);

std::vector<std::unique_ptr<BasicBlock>> read_control_flow(
    microsoft::D3D10ShaderBinary::CShaderCodeParser &Parser, SM50ShaderInternal *sm50_shader,
    microsoft::CSignatureParser &inputParser, microsoft::CSignatureParser5 &outputParser
);

uint32_t next_pow2(uint32_t x);

/** Binding slot masks of the shader's argument tables. */
struct ArgumentTableMasks {
  uint16_t constant_buffer = 0;
  uint16_t sampler = 0;
  uint64_t srv_lo = 0;
  uint64_t srv_hi = 0;
  uint64_t uav = 0;
};

/**
 * Builds the AIR argument tables (and the SM50 argument reflection) from
 * `shader->shader_info`. Shared by the SM 5.x DXBC and the SM 6.x DXIL paths so
 * both derive the same layout from one place.
 */
ArgumentTableMasks build_argument_tables(SM50ShaderInternal *shader);

/**
 * Fills `pRefl` from the shader's reflection state. The DXBC signature parsers are
 * only needed for the geometry stage and may be null. Returns false when the
 * reflection is inconsistent; the reason is written to `error`.
 */
bool fill_shader_reflection(
  SM50ShaderInternal *shader, const ArgumentTableMasks &masks,
  MTL_SHADER_REFLECTION *pRefl, microsoft::CSignatureParser *inputParser,
  microsoft::CSignatureParser5 *outputParser, llvm::raw_ostream &error
);

size_t estimate_payload_size(SM50ShaderInternal *pHullStage, float factor, uint32_t patch_per_group);

size_t estimate_mesh_size(SM50ShaderInternal *pDomainStage, uint32_t max_potential_factor_int);

std::unique_ptr<BindingMap> setup_binding_table2(
    const ShaderInfo *shader_info, air::FunctionSignatureBuilder &func_signature, llvm::Module &module,
    uint32_t argbuffer_constant_slot = SM50_BINDING_INDEX_CONSTANT_BUFFER, uint32_t argbuffer_slot = SM50_BINDING_INDEX_ARGUMENT_TABLE
);

/**
 * Build the binding map of a shader that resolves its resources through a root
 * signature.
 *
 * `bytecode`/`bytecode_length` are the shader's root signature: the pipeline's
 * global root signature for a graphics or compute pipeline, and for a raytracing
 * export the pipeline's dispatch root signature - the global root signature, or the
 * signature the pipeline built from it to carry the union of the static samplers the
 * dispatch's root signatures define (`MTLD3D12StateObject::GetDispatchRootSignature`).
 *
 * `local_bytecode`/`local_bytecode_length` are a raytracing export's own local
 * root signature, or null. The ranges it defines are read from the local root
 * arguments of the shader record the invocation runs as, which the caller hands
 * in with `BindingMap::SetLocalRootArgumentPointer` before the resource lowering
 * runs. D3D12 forbids the register bindings of a local root signature from
 * overlapping the global root signature's for the same shader, so a range both
 * define fails the build.
 *
 * A static sampler either signature defines is pipeline state and resolves through
 * the dispatch's one static sampler table, which is encoded from `bytecode`: every
 * root signature a dispatch can use has to define a static sampler of a given register
 * identically, and the runtime binds that table for the whole dispatch. So a static
 * sampler only the local signature defines is served as long as the pipeline's
 * dispatch signature carries it - which it does, because the pipeline builds it as the
 * union of both.
 *
 * `bind_signature_arguments` false declares neither the root argument argument nor
 * the static sampler argument on the function signature, which is what a visible
 * function needs: it is called through the visible function table with the fixed
 * ABI payload, hit state, record, depth (see `EmitVisibleFunctionCall`), so extra
 * arguments would make the indirect call type mismatch the callee. Such a stage
 * hands the addresses of the same two entries in with
 * `BindingMap::SetGlobalRootArgumentPointer` / `SetStaticSamplerArgumentPointer`
 * before the resource lowering runs, and a range of either entry is reported
 * unbound until it does. Raster and compute shaders take the default, which is
 * every existing caller's configuration.
 */
std::unique_ptr<BindingMap> setup_binding_rootsig(
    const ShaderInfo *shader_info, air::FunctionSignatureBuilder &func_signature, llvm::Module &module,
    microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type, const void *bytecode, size_t bytecode_length,
    uint32_t root_sig_slot = SM50_BINDING_INDEX_ROOT_ARGUMENTS,
    uint32_t static_sampler_slot = SM50_BINDING_INDEX_STATIC_SAMPLERS,
    const void *local_bytecode = nullptr, size_t local_bytecode_length = 0,
    bool bind_signature_arguments = true
);

void setup_metal_version(llvm::Module &module, SM50_SHADER_METAL_VERSION metal_version);

void setup_temp_register(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
);

void setup_immediate_constant_buffer(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
);

llvm::Error convert_dxbc_geometry_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  SM50ShaderInternal *pVertexStage, llvm::LLVMContext &context,
  llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

llvm::Error convert_dxbc_vertex_for_geometry_shader(
  const SM50ShaderInternal *pShaderInternal, const char *name,
  const SM50ShaderInternal *pGeometryStage,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

// TS+GS pass 2 object: fetches the pass-1 capture (SM50_SHADER_GS_TESSELLATION_INPUT required).
llvm::Error convert_dxbc_domain_for_geometry_shader(
  const SM50ShaderInternal *pDomainStage, const char *name,
  const SM50ShaderInternal *pGeometryStage,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

llvm::Error convert_dxbc_vertex_hull_shader(
    SM50ShaderInternal *pVertexStage, SM50ShaderInternal *pHullStage, const char *name, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

llvm::Error convert_dxbc_tesselator_domain_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, SM50ShaderInternal *pHullStage, llvm::LLVMContext &context,
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
);

template <SM50_SHADER_COMPILATION_ARGUMENT_TYPE data_e, typename data_t>
bool
args_get_data(const struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *data, data_t **out) {
  const SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg = data;
  while (arg) {
    switch (arg->type) {
    case data_e:
      *out = (data_t *)arg;
      return true;
    default:
      break;
    }
    arg = (const SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next;
  }
  *out = NULL;
  return false;
}

} // namespace dxmt::dxbc
