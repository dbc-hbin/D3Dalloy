/*
 * DXIL (Shader Model 6.x) stage lowering: signature, system values, threadgroup
 * storage, entry point transplant and the AIR prologue/epilogue.
 *
 * The lowering mirrors dxbc_signature.cpp / dxbc_converter.cpp one AIR entry at a
 * time - same AIR signature elements, same prologue helpers, same packed vec4
 * register file - so a DXIL stage produces the AIR the DXBC stages already
 * produce for the same signature.
 */

#include "dxil_signature.hpp"
#include "dxil_quad_layout.hpp"

#include "air_signature.hpp"
#include "airconv_context.hpp"
#include "airconv_public.h"
#include "msaa_pixel.hpp"
#include "nt/air_builder.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"

#include <bit>
#include <cassert>
#include <cstring>
#include <string>

namespace dxmt::dxil {

using llvm::cast;
using llvm::dyn_cast;

namespace {

/* D3D_NAME (d3dcommon.h) system value ids, as stored in the ISG1/OSG1 chunks.
 * The bundled d3dcommon.h stops at D3D_NAME_DEPTH_LESS_EQUAL. */
constexpr uint8_t NAME_UNDEFINED = 0;
constexpr uint8_t NAME_POSITION = 1;
constexpr uint8_t NAME_CLIP_DISTANCE = 2;
constexpr uint8_t NAME_CULL_DISTANCE = 3;
constexpr uint8_t NAME_RENDER_TARGET_ARRAY_INDEX = 4;
constexpr uint8_t NAME_VIEWPORT_ARRAY_INDEX = 5;
constexpr uint8_t NAME_VERTEX_ID = 6;
constexpr uint8_t NAME_PRIMITIVE_ID = 7;
constexpr uint8_t NAME_INSTANCE_ID = 8;
constexpr uint8_t NAME_IS_FRONT_FACE = 9;
constexpr uint8_t NAME_SAMPLE_INDEX = 10;
/* D3D_NAME_BARYCENTRICS (SM 6.1): the signature chunks carry D3D_NAME values, not
 * DXIL::SemanticKind (where Barycentrics is 28). */
constexpr uint8_t NAME_BARYCENTRICS = 23;
constexpr uint8_t NAME_SHADINGRATE = 24;
constexpr uint8_t NAME_TARGET = 64;
constexpr uint8_t NAME_DEPTH = 65;
constexpr uint8_t NAME_COVERAGE = 66;
constexpr uint8_t NAME_DEPTH_GREATER_EQUAL = 67;
constexpr uint8_t NAME_DEPTH_LESS_EQUAL = 68;
constexpr uint8_t NAME_STENCIL_REF = 69;
constexpr uint8_t NAME_INNER_COVERAGE = 70;

/* DXIL::SemanticKind of the compute shader values, which have no D3D_NAME. */
constexpr uint8_t KIND_DISPATCH_THREAD_ID = 21;
constexpr uint8_t KIND_GROUP_ID = 22;
constexpr uint8_t KIND_GROUP_INDEX = 23;
constexpr uint8_t KIND_GROUP_THREAD_ID = 24;

/* DXIL::InterpolationMode, as stored in the signature metadata. */
constexpr uint8_t INTERP_CONSTANT = 1;
constexpr uint8_t INTERP_LINEAR = 2;
constexpr uint8_t INTERP_LINEAR_CENTROID = 3;
constexpr uint8_t INTERP_LINEAR_NOPERSPECTIVE = 4;
constexpr uint8_t INTERP_LINEAR_NOPERSPECTIVE_CENTROID = 5;
constexpr uint8_t INTERP_LINEAR_SAMPLE = 6;
constexpr uint8_t INTERP_LINEAR_NOPERSPECTIVE_SAMPLE = 7;

air::Interpolation to_air_interpolation(uint8_t mode) {
  switch (mode) {
  case INTERP_CONSTANT:
    return air::Interpolation::flat;
  case INTERP_LINEAR:
    return air::Interpolation::center_perspective;
  case INTERP_LINEAR_CENTROID:
    return air::Interpolation::centroid_perspective;
  case INTERP_LINEAR_NOPERSPECTIVE:
    return air::Interpolation::center_no_perspective;
  case INTERP_LINEAR_NOPERSPECTIVE_CENTROID:
    return air::Interpolation::centroid_no_perspective;
  case INTERP_LINEAR_SAMPLE:
    return air::Interpolation::sample_perspective;
  case INTERP_LINEAR_NOPERSPECTIVE_SAMPLE:
    return air::Interpolation::sample_no_perspective;
  default:
    return air::Interpolation::center_perspective;
  }
}

bool is_sample_interpolation(uint8_t mode) {
  return mode == INTERP_LINEAR_SAMPLE || mode == INTERP_LINEAR_NOPERSPECTIVE_SAMPLE;
}

/* D3D_REGISTER_COMPONENT_{UINT16,SINT16,FLOAT16} (SM 6.2 native 16 bit stage I/O). Each
 * 16 bit component travels in a full 32 bit register lane, widened on storeOutput and
 * narrowed on loadInput, so the varying is interpolated at no less than half precision. */
constexpr uint8_t kComponentUint16 = 4;
constexpr uint8_t kComponentSint16 = 5;
constexpr uint8_t kComponentFloat16 = 6;

uint8_t widen_component_type(uint8_t component_type) {
  switch (component_type) {
  case kComponentUint16:
    return (uint8_t)dxbc::RegisterComponentType::Uint;
  case kComponentSint16:
    return (uint8_t)dxbc::RegisterComponentType::Int;
  case kComponentFloat16:
    return (uint8_t)dxbc::RegisterComponentType::Float;
  default:
    return component_type;
  }
}

dxbc::RegisterComponentType to_register_component_type(uint8_t component_type) {
  component_type = widen_component_type(component_type);
  if (component_type < (uint8_t)dxbc::RegisterComponentType::Uint ||
      component_type > (uint8_t)dxbc::RegisterComponentType::Float)
    return dxbc::RegisterComponentType::Unknown;
  return (dxbc::RegisterComponentType)component_type;
}

air::InputAttributeComponentType to_attribute_component_type(uint8_t component_type) {
  component_type = widen_component_type(component_type);
  if (component_type < (uint8_t)dxbc::RegisterComponentType::Uint ||
      component_type > (uint8_t)dxbc::RegisterComponentType::Float)
    return air::InputAttributeComponentType::Unknown;
  return (air::InputAttributeComponentType)component_type;
}

/* A register-file lane (i32 or float) converted to the dx.op I/O value type. */
llvm::Value *narrow_io_value(llvm::IRBuilder<> &builder, llvm::Value *value, llvm::Type *type) {
  if (value->getType() == type)
    return value;
  if (type->isHalfTy())
    return builder.CreateFPTrunc(value->getType()->isFloatTy() ? value : builder.CreateBitCast(value, builder.getFloatTy()),
                                 type);
  if (type->isIntegerTy(16) || type->isIntegerTy(1))
    return builder.CreateTrunc(value->getType()->isIntegerTy() ? value : builder.CreateBitCast(value, builder.getInt32Ty()),
                               type);
  return builder.CreateBitCast(value, type);
}

/* A dx.op I/O value widened to a 32 bit register lane of the element's component type. */
llvm::Value *widen_io_value(llvm::IRBuilder<> &builder, llvm::Value *value, uint8_t component_type) {
  if (value->getType()->isHalfTy())
    return builder.CreateFPExt(value, builder.getFloatTy());
  if (value->getType()->isIntegerTy(16))
    return component_type == kComponentSint16 ? builder.CreateSExt(value, builder.getInt32Ty())
                                              : builder.CreateZExt(value, builder.getInt32Ty());
  return value;
}

/** Stage-local vertex attribute name; varyings use their D3D semantic instead. */
std::string consistent_attribute_name(uint32_t reg, uint32_t mask) {
  return "reg" + std::to_string(reg) + "_" + std::to_string(std::countr_zero(mask & 0xFu));
}

uint32_t element_mask(const SigElement &element) {
  if (element.mask)
    return element.mask & 0xFu;
  return (((1u << element.cols) - 1u) << element.start_col) & 0xFu;
}

uint32_t element_start_col(const SigElement &element) {
  if (element.mask)
    return (uint32_t)std::countr_zero((uint32_t)element.mask);
  return element.start_col;
}

SignatureSlot make_slot(const SigElement &element, uint32_t air_index, bool pull_mode = false) {
  SignatureSlot slot;
  slot.element_id = element.id;
  slot.element_row = element.element_row;
  slot.reg = element.start_row - element.element_row;
  slot.rows = element.rows;
  slot.start_col = element_start_col(element);
  slot.air_index = air_index;
  slot.pull_mode = pull_mode;
  slot.interpolation = element.interp;
  slot.comp_type = element.comp_type;
  slot.semantic_kind = element.semantic_kind;
  return slot;
}

} // namespace

/*
 * Relocate the module-scope storage of `module` into the AIR address spaces, and
 * only those: the relocation is a property of the module, so a module that has
 * already been prepared has nothing left to match and a second call is a no-op.
 * That is what keeps a caller from paying for the walk once per entry point.
 *
 * DXIL declares groupshared variables as external globals in address space 3; AIR
 * expresses threadgroup storage as an internal global in that address space. DXIL
 * immutable globals live in the default address space (0); AIR constant storage is
 * address space 2, exactly like the DXBC immediate-constant-buffer path.
 *
 * The type of a relocated global is kept, so every GEP/load/store the shader
 * already built stays valid. An immutable global is not rewritten in place: its
 * uses are redirected through an addrspacecast to its old AS0 type, which is what
 * the DXIL front end built its flat addressing around. That cast is deliberate -
 * dereferencing it would read constant storage as device memory - and folding it
 * back onto the storage's own address space is exactly what
 * InferAddressSpacesPass(0) in runOptimizationPasses() does. Every path that
 * lowers a module therefore has to run the optimization pipeline (or at least that
 * pass) before it serializes AIR.
 */
void PrepareModuleStorage(llvm::Module &module) {
  llvm::SmallVector<llvm::GlobalVariable *, 4> shared;
  llvm::SmallVector<llvm::GlobalVariable *, 4> constants;
  for (auto &global : module.globals()) {
    if (global.getAddressSpace() == 3 && global.isDeclaration())
      shared.push_back(&global);
    if (global.getAddressSpace() == 0 && global.isConstant() && global.hasInitializer())
      constants.push_back(&global);
  }
  // DXIL immutable globals use the default address space; AIR constant storage
  // uses address space 2, just like the DXBC immediate-constant-buffer path.
  for (auto *global : constants) {
    auto name = global->getName().str();
    auto *storage = new llvm::GlobalVariable(
      module, global->getValueType(), true, llvm::GlobalValue::InternalLinkage,
      global->getInitializer(), name, nullptr, llvm::GlobalValue::NotThreadLocal, 2
    );
    storage->setAlignment(global->getAlign());
    storage->setUnnamedAddr(global->getUnnamedAddr());
    global->replaceAllUsesWith(llvm::ConstantExpr::getAddrSpaceCast(storage, global->getType()));
    global->eraseFromParent();
    storage->setName(name);
  }
  for (auto *declaration : shared) {
    auto name = declaration->getName().str();
    auto *storage = new llvm::GlobalVariable(
      module, declaration->getValueType(), false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(declaration->getValueType()), name, nullptr,
      llvm::GlobalValue::NotThreadLocal, 3
    );
    storage->setAlignment(declaration->getAlign());
    declaration->replaceAllUsesWith(storage);
    declaration->eraseFromParent();
    storage->setName(name);
  }
}

const SignatureSlot *StageSignature::find_input(uint32_t element_id, uint32_t row) const {
  for (auto &slot : inputs) {
    if (slot.element_id == element_id && row >= slot.element_row && row - slot.element_row < slot.rows)
      return &slot;
  }
  return nullptr;
}

const SignatureSlot *StageSignature::find_output(uint32_t element_id, uint32_t row) const {
  for (auto &slot : outputs) {
    if (slot.element_id == element_id && row >= slot.element_row && row - slot.element_row < slot.rows)
      return &slot;
  }
  return nullptr;
}

/* -------------------------------------------------------------------------- */
/* Stage signature declaration                                               */
/* -------------------------------------------------------------------------- */

namespace {

void declare_vertex_signature(
  const ModuleInfo &module, dxbc::SM50ShaderInternal &shader, dxbc::SignatureContext &sig_ctx,
  StageSignature &out_signature
) {
  auto &func_signature = sig_ctx.func_signature;
  auto &prologue = sig_ctx.prologue;
  auto &epilogue = sig_ctx.epilogue;
  auto *ia_layout = sig_ctx.ia_layout;
  auto &inputs = out_signature.inputs;
  auto &outputs = out_signature.outputs;

  for (auto &element : module.input_sig) {
    uint32_t reg = element.start_row;
    uint32_t mask = element_mask(element);
    switch (element.semantic_kind) {
    case NAME_UNDEFINED: {
      if (ia_layout) {
        for (unsigned i = 0; i < ia_layout->num_elements; i++) {
          auto element_info = ia_layout->elements[i];
          if (element_info.reg != reg)
            continue;
          prologue << dxbc::pull_vertex_input(
            func_signature, reg, mask, element_info, ia_layout->slot_mask, true
          );
          break;
        }
        inputs.push_back(make_slot(element, ~0u));
        break;
      }
      uint32_t assigned_index = func_signature.DefineInput(air::InputVertexStageIn{
        .attribute = reg,
        .type = to_attribute_component_type(element.comp_type),
        .name = consistent_attribute_name(reg, mask),
      });
      prologue << dxbc::init_input_reg(assigned_index, reg, mask);
      inputs.push_back(make_slot(element, assigned_index));
      break;
    }
    case NAME_VERTEX_ID: {
      prologue << dxbc::make_effect_bind([=](dxbc::context ctx) {
        return dxbc::store_at_vec4_array_masked(
          ctx.resource.input.ptr_int4, ctx.builder.getInt32(reg), ctx.resource.vertex_id, mask
        );
      });
      inputs.push_back(make_slot(element, ~0u));
      break;
    }
    case NAME_INSTANCE_ID: {
      prologue << dxbc::make_effect_bind([=](dxbc::context ctx) {
        return dxbc::store_at_vec4_array_masked(
          ctx.resource.input.ptr_int4, ctx.builder.getInt32(reg), ctx.resource.instance_id, mask
        );
      });
      inputs.push_back(make_slot(element, ~0u));
      break;
    }
    default:
      return;
    }
  }

  for (auto &element : module.output_sig) {
    uint32_t reg = element.start_row;
    uint32_t mask = element_mask(element);
    uint32_t air_index;
    std::function<dxbc::IRValue(dxbc::pvalue)> pop;
    switch (element.semantic_kind) {
    case NAME_POSITION: {
      air_index = func_signature.DefineOutput(air::OutputPosition{.type = air::msl_float4});
      pop = dxbc::pop_output_reg_sanitize_pos(reg, mask, air_index);
      break;
    }
    case NAME_CLIP_DISTANCE:
    case NAME_CULL_DISTANCE: {
      bool cull = element.semantic_kind == NAME_CULL_DISTANCE;
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1u << i))
          (cull ? shader.cull_distance_scalars : shader.clip_distance_scalars)
            .push_back({.component = (uint8_t)i, .reg = (uint8_t)reg});
      }
      air_index = func_signature.DefineOutput(air::OutputVertex{
        .user = dxbc::varyingAttributeName(element.name, element.semantic_index),
        .type = air::msl_float4,
      });
      pop = dxbc::pop_output_reg_semantic(reg, mask, air_index, true);
      break;
    }
    case NAME_RENDER_TARGET_ARRAY_INDEX: {
      air_index = func_signature.DefineOutput(air::OutputRenderTargetArrayIndex{});
      pop = dxbc::pop_output_reg(reg, mask, air_index);
      break;
    }
    case NAME_SHADINGRATE: {
      air_index = func_signature.DefineOutput(air::OutputVertex{
        .user = "dxmt_shading_rate", .type = air::msl_uint,
      });
      pop = dxbc::pop_output_reg(reg, mask, air_index);
      break;
    }
    case NAME_VIEWPORT_ARRAY_INDEX: {
      air_index = func_signature.DefineOutput(air::OutputViewportArrayIndex{});
      pop = dxbc::pop_output_reg(reg, mask, air_index);
      break;
    }
    default: {
      air_index = func_signature.DefineOutput(air::OutputVertex{
        .user = dxbc::varyingAttributeName(element.name, element.semantic_index),
        .type = dxbc::to_msl_type(to_register_component_type(element.comp_type)),
      });
      pop = dxbc::pop_output_reg_semantic(reg, mask, air_index);
      break;
    }
    }
    /* Without rasterization the AIR vertex function returns void (Metal rejects
     * any output); the outputs are still declared so storeOutput resolves. */
    if (!sig_ctx.skip_vertex_output)
      epilogue >> std::move(pop);
    outputs.push_back(make_slot(element, air_index));
  }
}

void declare_pixel_signature(
  const ModuleInfo &module, dxbc::SM50ShaderInternal &shader, dxbc::SignatureContext &sig_ctx,
  StageSignature &out_signature
) {
  auto &func_signature = sig_ctx.func_signature;
  auto &prologue = sig_ctx.prologue;
  auto &epilogue = sig_ctx.epilogue;
  auto &resource = sig_ctx.resource;
  auto &inputs = out_signature.inputs;
  auto &outputs = out_signature.outputs;

  /* Elements read per vertex through dx.op.attributeAtVertex (SM 6.1 GetAttributeAtVertex). */
  llvm::DenseSet<uint32_t> per_vertex_elements;
  for (auto &instruction : llvm::instructions(*module.entry)) {
    auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
    if (!call || !call->getCalledFunction() || call->arg_size() < 2 ||
        !call->getCalledFunction()->getName().startswith("dx.op.attributeAtVertex."))
      continue;
    if (auto *id = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1)))
      per_vertex_elements.insert(static_cast<uint32_t>(id->getZExtValue()));
  }

  for (auto &element : module.input_sig) {
    if (element.semantic_kind == NAME_SAMPLE_INDEX && element.start_row == ~0u) {
      // Intrinsic-only signature elements have no packed register or column.
      func_signature.DefineInput(air::InputSampleIndex{});
      continue;
    }
    if (element.semantic_kind == NAME_INNER_COVERAGE && element.start_row == ~0u) {
      if (sig_ctx.captured_pixel) func_signature.DefineInput(air::InputFragmentStageIn{
        .user = "dxmt_inner_coverage", .type = air::msl_uint,
        .interpolation = air::Interpolation::flat, .pull_mode = false,
      });
      continue;
    }
    if (element.semantic_kind == NAME_BARYCENTRICS) {
      /* SV_Barycentrics has no packed register: loadInput reads the AIR argument. */
      const bool perspective = element.interp == INTERP_LINEAR || element.interp == INTERP_LINEAR_CENTROID ||
                               element.interp == INTERP_LINEAR_SAMPLE;
      inputs.push_back(make_slot(element, func_signature.DefineInput(air::InputBarycentric{perspective})));
      continue;
    }
    if (per_vertex_elements.contains(element.id)) {
      /* A per-vertex attribute is bound as `vertex_value<T>`; plain reads of it take the
       * provoking (first) vertex, which is the value D3D's nointerpolation delivers. */
      auto slot = make_slot(element, ~0u);
      slot.vertex_value_index = func_signature.DefineInput(air::InputVertexValue{
        .user = dxbc::varyingAttributeName(element.name, element.semantic_index),
        .type = dxbc::to_msl_type(to_register_component_type(element.comp_type)),
      });
      inputs.push_back(slot);
      continue;
    }
    uint32_t reg = element.start_row;
    uint32_t mask = element_mask(element);
    uint32_t air_index;
    bool init_plain = true;
    bool fix_w_component = false;
    bool element_pull_mode = false;
    switch (element.semantic_kind) {
    case NAME_SHADINGRATE: {
      air_index = sig_ctx.captured_pixel ? func_signature.DefineInput(air::InputFragmentStageIn{
        .user = "dxmt_resolved_shading_rate", .type = air::msl_uint,
        .interpolation = air::Interpolation::flat, .pull_mode = false,
      }) : ~0u;
      init_plain = false;
      break;
    }
    case NAME_POSITION: {
      air_index = func_signature.DefineInput(air::InputPosition{
        .interpolation = to_air_interpolation(element.interp),
      });
      fix_w_component = true;
      break;
    }
    case NAME_RENDER_TARGET_ARRAY_INDEX: {
      air_index = func_signature.DefineInput(air::InputRenderTargetArrayIndex{});
      break;
    }
    case NAME_VIEWPORT_ARRAY_INDEX: {
      air_index = func_signature.DefineInput(air::InputViewportArrayIndex{});
      break;
    }
    case NAME_CLIP_DISTANCE:
    case NAME_CULL_DISTANCE: {
      air_index = func_signature.DefineInput(air::InputFragmentStageIn{
        .user = dxbc::varyingAttributeName(element.name, element.semantic_index),
        .type = air::msl_float4,
        .interpolation = to_air_interpolation(element.interp),
        .pull_mode = false,
      });
      init_plain = false;
      prologue << dxbc::init_input_reg_semantic(air_index, reg, mask);
      break;
    }
    case NAME_IS_FRONT_FACE: {
      air_index = func_signature.DefineInput(air::InputFrontFacing{});
      break;
    }
    case NAME_SAMPLE_INDEX: {
      air_index = func_signature.DefineInput(air::InputSampleIndex{});
      break;
    }
    case NAME_PRIMITIVE_ID: {
      air_index = func_signature.DefineInput(air::InputPrimitiveID{});
      break;
    }
    case NAME_COVERAGE: {
      air_index = func_signature.DefineInput(air::InputInputCoverage{});
      prologue << dxbc::make_effect([=](dxbc::context ctx) {
        ctx.resource.coverage_mask_arg = ctx.function->getArg(air_index);
        return std::monostate{};
      });
      init_plain = false;
      break;
    }
    case NAME_INNER_COVERAGE: {
      air_index = sig_ctx.captured_pixel ? func_signature.DefineInput(air::InputFragmentStageIn{
        .user = "dxmt_inner_coverage", .type = air::msl_uint,
        .interpolation = air::Interpolation::flat, .pull_mode = false,
      }) : ~0u;
      init_plain = false;
      break;
    }
    default: {
      auto interpolation = to_air_interpolation(element.interp);
      bool pull_mode = bool(sig_ctx.pull_mode_reg_mask & (1u << reg)) &&
                       interpolation != air::Interpolation::flat;
      element_pull_mode = pull_mode;
      air_index = func_signature.DefineInput(air::InputFragmentStageIn{
        .user = dxbc::varyingAttributeName(element.name, element.semantic_index),
        .type = dxbc::to_msl_type(to_register_component_type(element.comp_type)),
        .interpolation = interpolation,
        .pull_mode = pull_mode,
      });
      init_plain = false;
      if (!pull_mode) {
        prologue << dxbc::init_input_reg_semantic(air_index, reg, mask);
        break;
      }
      resource.interpolant_map[reg].push_back(dxbc::interpolant_descriptor{
        [=](auto) {
          return dxbc::make_irvalue([=](dxbc::context ctx) {
            return ctx.function->getArg(air_index);
          });
        },
        interpolation == air::Interpolation::sample_perspective ||
          interpolation == air::Interpolation::center_perspective ||
          interpolation == air::Interpolation::centroid_perspective,
        static_cast<uint8_t>(mask),
      });
      uint32_t sampleidx_at = ~0u;
      if (is_sample_interpolation(element.interp))
        sampleidx_at = func_signature.DefineInput(air::InputSampleIndex{});
      prologue << dxbc::init_input_reg_with_interpolation_semantic(
        air_index, reg, mask, interpolation, sampleidx_at
      );
      break;
    }
    }
    if (init_plain)
      prologue << dxbc::init_input_reg(air_index, reg, mask, fix_w_component);
    inputs.push_back(make_slot(element, air_index, element_pull_mode));
  }

  for (auto &element : module.output_sig) {
    uint32_t reg = element.start_row;
    uint32_t mask = element_mask(element);
    uint32_t air_index = ~0u;
    switch (element.semantic_kind) {
    case NAME_TARGET:
    case NAME_UNDEFINED: {
      /* A pixel shader output is a render target unless it is one of the
       * explicitly handled system values below. */
      auto type = reg < 8 ? dxbc::component_type_from_pixel_format(sig_ctx.pixel_formats[reg])
                          : dxbc::RegisterComponentType::Unknown;
      if (sig_ctx.dual_source_blending || type == dxbc::RegisterComponentType::Unknown)
        type = to_register_component_type(element.comp_type);
      air_index = func_signature.DefineOutput(air::OutputRenderTarget{
        .dual_source_blending = sig_ctx.dual_source_blending,
        .index = reg,
        .type = dxbc::to_msl_type(type),
      });
      if (type == dxbc::RegisterComponentType::Float &&
          (sig_ctx.unorm_output_reg_mask & (1u << reg)))
        epilogue >> dxbc::pop_output_reg_fix_unorm(reg, mask, air_index);
      else
        epilogue >> dxbc::pop_output_reg(reg, mask, air_index);
      shader.pso_valid_output_reg_mask |= (1u << reg);
      break;
    }
    case NAME_DEPTH:
    case NAME_DEPTH_GREATER_EQUAL:
    case NAME_DEPTH_LESS_EQUAL: {
      prologue << dxbc::make_effect([](dxbc::context ctx) -> std::monostate {
        assert(ctx.resource.depth_output_reg == nullptr && "otherwise oDepth is defined twice");
        ctx.resource.depth_output_reg = ctx.builder.CreateAlloca(ctx.types._float);
        return {};
      });
      if (!sig_ctx.disable_depth_output) {
        air_index = func_signature.DefineOutput(air::OutputDepth{
          .depth_argument = element.semantic_kind == NAME_DEPTH_GREATER_EQUAL
                              ? air::DepthArgument::greater
                          : element.semantic_kind == NAME_DEPTH_LESS_EQUAL
                              ? air::DepthArgument::less
                              : air::DepthArgument::any,
        });
        epilogue >> [=](dxbc::pvalue value) {
          return dxbc::make_irvalue([=](dxbc::context ctx) {
            return ctx.builder.CreateInsertValue(
              value,
              ctx.builder.CreateLoad(
                ctx.types._float,
                ctx.builder.CreateConstInBoundsGEP1_32(
                  ctx.types._float, ctx.resource.depth_output_reg, 0
                )
              ),
              {air_index}
            );
          });
        };
      }
      break;
    }
    case NAME_COVERAGE: {
      prologue << dxbc::make_effect([](dxbc::context ctx) -> std::monostate {
        assert(ctx.resource.coverage_mask_reg == nullptr && "otherwise oMask is defined twice");
        ctx.resource.coverage_mask_reg = ctx.builder.CreateAlloca(ctx.types._int);
        return {};
      });
      air_index = func_signature.DefineOutput(air::OutputCoverageMask{});
      epilogue >> [=](dxbc::pvalue value) {
        return dxbc::make_irvalue([=](dxbc::context ctx) {
          auto *mask = ctx.builder.CreateLoad(
            ctx.types._int,
            ctx.builder.CreateConstInBoundsGEP1_32(
              ctx.types._int, ctx.resource.coverage_mask_reg, 0
            )
          );
          return ctx.builder.CreateInsertValue(
            value,
            ctx.pso_sample_mask != 0xffffffff
              ? ctx.builder.CreateAnd(mask, dxbc::pixel_sample_mask(ctx))
              : mask,
            {air_index}
          );
        });
      };
      shader.ps_has_coverage_output = 1;
      break;
    }
    case NAME_STENCIL_REF: {
      prologue << dxbc::make_effect([](dxbc::context ctx) -> std::monostate {
        assert(ctx.resource.stencil_ref_reg == nullptr && "otherwise oStencil is defined twice");
        ctx.resource.stencil_ref_reg = ctx.builder.CreateAlloca(ctx.types._int);
        return {};
      });
      air_index = func_signature.DefineOutput(air::OutputStencilRef{});
      epilogue >> [=](dxbc::pvalue value) {
        return dxbc::make_irvalue([=](dxbc::context ctx) {
          return ctx.builder.CreateInsertValue(
            value,
            ctx.builder.CreateLoad(
              ctx.types._int,
              ctx.builder.CreateConstInBoundsGEP1_32(
                ctx.types._int, ctx.resource.stencil_ref_reg, 0
              )
            ),
            {air_index}
          );
        });
      };
      break;
    }
    default:
      break;
    }
    outputs.push_back(make_slot(element, air_index));
  }
}

llvm::Error declare_compute_signature(
  const ModuleInfo &module, dxbc::SM50ShaderInternal &shader, dxbc::SignatureContext &sig_ctx,
  StageSignature &out_signature
) {
  auto &func_signature = sig_ctx.func_signature;
  auto &prologue = sig_ctx.prologue;
  bool used[4] = {};
  bool quad_semantics = false;
  bool requires_derivatives = false;
  for (auto &function : *module.module)
    for (auto &block : function)
      for (auto &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call || !call->getCalledFunction() ||
            !call->getCalledFunction()->getName().startswith("dx.op.") || call->arg_empty())
          continue;
        auto *opcode = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
        if (opcode && opcode->getZExtValue() >= 93 && opcode->getZExtValue() <= 96)
          used[opcode->getZExtValue() - 93] = true;
        if (opcode) {
          quad_semantics |= UsesQuadOperations(opcode->getZExtValue());
          requires_derivatives |= RequiresQuadDerivatives(opcode->getZExtValue());
        }
      }

  /*
   * SM 6.6 compute/mesh/amplification quads: with numthreads X and Y even (and not the 1D X%4, Y=Z=1
   * layout), a quad is (x, y), (x+1, y), (x, y+1), (x+1, y+1). Metal assigns simdgroup
   * lanes in linear thread index order, which already satisfies the 1D rule; for the
   * 2D rule the thread IDs are re-derived from the linear index so that every 4
   * consecutive lanes form one 2x2 quad in Z order. D3D does not define any other
   * mapping between thread IDs and lanes, so the remap is invisible otherwise.
   */
  const uint32_t X = module.num_threads[0], Y = module.num_threads[1], Z = module.num_threads[2];
  const auto layout = ClassifyQuadLayout(X, Y, Z);
  if (requires_derivatives && layout == QuadLayout::Unavailable)
    return createError("DXIL quad derivatives require numthreads X divisible by 4 with Y=Z=1, "
                       "or both X and Y divisible by 2");
  const bool remap = quad_semantics && layout == QuadLayout::Tiled2D;
  if (remap) {
    used[1] = true;
    used[3] = true;
  }

  for (unsigned index = 0; index < 4; ++index) {
    if (!used[index])
      continue;
    uint32_t air_index;
    llvm::Value *dxbc::io_binding_map::*binding;
    switch (index) {
    case 0:
      air_index = func_signature.DefineInput(air::InputThreadPositionInGrid{});
      binding = &dxbc::io_binding_map::thread_id_arg;
      break;
    case 1:
      air_index = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
      binding = &dxbc::io_binding_map::thread_group_id_arg;
      break;
    case 2:
      air_index = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
      binding = &dxbc::io_binding_map::thread_id_in_group_arg;
      break;
    case 3:
      air_index = func_signature.DefineInput(air::InputThreadIndexInThreadgroup{});
      binding = &dxbc::io_binding_map::thread_id_in_group_flat_arg;
      break;
    default:
      continue;
    }
    prologue << dxbc::make_effect([=](dxbc::context ctx) {
      ctx.resource.*binding = ctx.function->getArg(air_index);
      return std::monostate{};
    });
  }
  if (remap) {
    prologue << dxbc::make_effect([=](dxbc::context ctx) {
      auto &b = ctx.builder;
      auto *linear = ctx.resource.thread_id_in_group_flat_arg;
      auto *quad = b.CreateLShr(linear, 2);
      auto *within = b.CreateAnd(linear, 3);
      auto *qx = b.CreateURem(quad, b.getInt32(X / 2));
      auto *qy = b.CreateURem(b.CreateUDiv(quad, b.getInt32(X / 2)), b.getInt32(Y / 2));
      auto *z = b.CreateUDiv(quad, b.getInt32(X / 2 * Y / 2));
      auto *x = b.CreateOr(b.CreateShl(qx, 1), b.CreateAnd(within, 1));
      auto *y = b.CreateOr(b.CreateShl(qy, 1), b.CreateLShr(within, 1));
      llvm::Value *local = llvm::UndefValue::get(llvm::FixedVectorType::get(b.getInt32Ty(), 3));
      local = b.CreateInsertElement(local, x, uint64_t(0));
      local = b.CreateInsertElement(local, y, 1);
      local = b.CreateInsertElement(local, z, 2);
      ctx.resource.thread_id_in_group_arg = local;
      ctx.resource.thread_id_in_group_flat_arg =
        b.CreateAdd(b.CreateMul(z, b.getInt32(X * Y)), b.CreateAdd(b.CreateMul(y, b.getInt32(X)), x));
      auto *size = llvm::ConstantVector::get({b.getInt32(X), b.getInt32(Y), b.getInt32(Z)});
      ctx.resource.thread_id_arg = b.CreateAdd(b.CreateMul(ctx.resource.thread_group_id_arg, size), local);
      return std::monostate{};
    });
  }

  shader.threadgroup_size[0] = module.num_threads[0];
  shader.threadgroup_size[1] = module.num_threads[1];
  shader.threadgroup_size[2] = module.num_threads[2];
  func_signature.UseMaxWorkgroupSize(
    module.num_threads[0] * module.num_threads[1] * module.num_threads[2]
  );
  return llvm::Error::success();
}

} // namespace

llvm::Error DeclareStageSignature(
  const ModuleInfo &module, dxbc::SM50ShaderInternal &shader, dxbc::SignatureContext &sig_ctx,
  StageSignature &out_signature
) {
  for (auto &element : module.input_sig) {
    if (element.semantic_kind == NAME_INNER_COVERAGE) {
      if (module.stage != Stage::Pixel || element.rows != 1 || element.cols != 1 ||
          (element.interp != INTERP_CONSTANT && element.start_row != ~0u) ||
          (element.comp_type != 1 && element.comp_type != 2))
        return createError("SV_InnerCoverage must be a constant scalar integer pixel input");
      if (std::any_of(module.input_sig.begin(), module.input_sig.end(), [](const SigElement &input) {
            return input.semantic_kind == NAME_COVERAGE;
          })) return createError("SV_Coverage and SV_InnerCoverage inputs are mutually exclusive");
    }
    if (element.start_row + element.rows > shader.max_input_register)
      shader.max_input_register = element.start_row + element.rows;
  }
  for (auto &element : module.output_sig) {
    if (element.start_row + element.rows > shader.max_output_register)
      shader.max_output_register = element.start_row + element.rows;
  }

  switch (module.stage) {
  case Stage::Vertex:
    declare_vertex_signature(module, shader, sig_ctx, out_signature);
    break;
  case Stage::Pixel:
    declare_pixel_signature(module, shader, sig_ctx, out_signature);
    break;
  case Stage::Compute:
  case Stage::Mesh:
  case Stage::Amplification:
    if (module.num_threads[0] * module.num_threads[1] * module.num_threads[2] == 0)
      return createError("DXIL compute shader has no numthreads");
    if (auto error = declare_compute_signature(module, shader, sig_ctx, out_signature))
      return error;
    break;
  default:
    return unsupported("DXIL stage");
  }

  /* A pixel shader input that carries a user varying has to say how it is
   * interpolated; without the interpolation mode from the signature metadata
   * the stage would be linked against the wrong AIR qualifier. */
  if (module.stage == Stage::Pixel) {
    for (auto &element : module.input_sig) {
      if (element.semantic_kind == NAME_UNDEFINED && element.interp == 0)
        return createError(
          "DXIL pixel shader input '" + element.name +
          "' has no interpolation mode in its signature metadata"
        );
    }
  }

  return llvm::Error::success();
}

/* -------------------------------------------------------------------------- */
/* dx.op loadInput / storeOutput                                              */
/* -------------------------------------------------------------------------- */

namespace {

constexpr llvm::StringLiteral LOAD_INPUT_PREFIX = "dx.op.loadInput.";
constexpr llvm::StringLiteral STORE_OUTPUT_PREFIX = "dx.op.storeOutput.";
constexpr llvm::StringLiteral THREAD_ID_PREFIX = "dx.op.threadId.";
constexpr llvm::StringLiteral GROUP_ID_PREFIX = "dx.op.groupId.";
constexpr llvm::StringLiteral THREAD_ID_IN_GROUP_PREFIX = "dx.op.threadIdInGroup.";
constexpr llvm::StringLiteral FLATTENED_THREAD_ID_IN_GROUP_PREFIX =
  "dx.op.flattenedThreadIdInGroup.";

std::string callee_name(const llvm::CallInst &call) {
  if (auto *callee = call.getCalledFunction())
    return callee->getName().str();
  return {};
}

llvm::Expected<uint32_t> constant_operand(const llvm::CallInst &call, unsigned index) {
  if (auto *constant = dyn_cast<llvm::ConstantInt>(call.getArgOperand(index)))
    return (uint32_t)constant->getZExtValue();
  return createError(
    "dx.op I/O instruction of '" + call.getFunction()->getName() + "' has a non constant operand " +
    llvm::Twine(index)
  );
}

/* Every component of a signature element is addressed relative to its start
 * column, but the register file is addressed by absolute component. */
llvm::Expected<SignatureSlot const *>
resolve_slot(
  const llvm::CallInst &call, const SignatureSlot *slot, uint32_t component, uint32_t row
) {
  if (!slot)
    return createError(
      "dx.op I/O instruction references unknown signature element " +
      llvm::Twine(cast<llvm::ConstantInt>(call.getArgOperand(1))->getZExtValue())
    );
  if (row < slot->element_row || row - slot->element_row >= slot->rows || slot->start_col + component > 3)
    return createError(
      "dx.op I/O instruction addresses signature element " + llvm::Twine(slot->element_id) +
      " out of range"
    );
  return slot;
}

/* Component `component` of per-vertex attribute `element` at triangle vertex `vertex`
 * (air.get_vertex_value; the argument's type is the element's full register type). */
llvm::Value *load_vertex_value(dxbc::context &ctx, const SignatureSlot &element, uint32_t component,
                               llvm::Value *vertex) {
  auto &builder = ctx.builder;
  const bool is_float = to_register_component_type(element.comp_type) == dxbc::RegisterComponentType::Float;
  const bool is_signed = to_register_component_type(element.comp_type) == dxbc::RegisterComponentType::Int;
  auto *lane = is_float ? builder.getFloatTy() : builder.getInt32Ty();
  auto *type = llvm::FixedVectorType::get(lane, 4);
  auto *argument = ctx.function->getArg(element.vertex_value_index);
  std::string name = "air.get_vertex_value.";
  name += is_float ? "v4f32" : is_signed ? "s.v4i32" : "u.v4i32";
  auto attributes = llvm::AttributeList::get(
    ctx.llvm, {{~0U, llvm::Attribute::get(ctx.llvm, llvm::Attribute::ReadOnly)},
               {~0U, llvm::Attribute::get(ctx.llvm, llvm::Attribute::NoUnwind)},
               {~0U, llvm::Attribute::get(ctx.llvm, llvm::Attribute::WillReturn)}});
  auto callee = ctx.module.getOrInsertFunction(
    name, llvm::FunctionType::get(type, {argument->getType(), builder.getInt32Ty()}, false), attributes);
  auto *values = builder.CreateCall(callee, {argument, vertex});
  return builder.CreateExtractElement(values, element.start_col + component);
}

/* dx.op.attributeAtVertex (137): (element id, row, column, vertex). */
llvm::Error lower_attribute_at_vertex(dxbc::context &ctx, const StageSignature &signature, llvm::CallInst &call) {
  auto id = constant_operand(call, 1);
  auto row = constant_operand(call, 2);
  auto component = constant_operand(call, 3);
  if (!id) return id.takeError();
  if (!row) return row.takeError();
  if (!component) return component.takeError();
  auto slot = resolve_slot(call, signature.find_input(*id, *row), *component, *row);
  if (!slot) return slot.takeError();
  if ((*slot)->vertex_value_index == ~0u)
    return createError("dx.op.attributeAtVertex reads an element that is not bound per vertex");
  auto &builder = ctx.builder;
  builder.SetInsertPoint(&call);
  auto *vertex = builder.CreateZExt(call.getArgOperand(4), builder.getInt32Ty());
  /* D3D names the vertices in the order the primitive was submitted, which is also
   * Metal's vertex_index order; out-of-range indices are undefined in D3D. */
  vertex = builder.CreateSelect(builder.CreateICmpULT(vertex, builder.getInt32(3)), vertex, builder.getInt32(0));
  llvm::Value *value = narrow_io_value(builder, load_vertex_value(ctx, **slot, *component, vertex), call.getType());
  call.replaceAllUsesWith(value);
  call.eraseFromParent();
  return llvm::Error::success();
}

llvm::Error lower_load_input(
  dxbc::context &ctx, const StageSignature &signature, llvm::CallInst &call
) {
  auto element_id = constant_operand(call, 1);
  if (!element_id)
    return element_id.takeError();
  auto *constant_row = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(2));
  const uint32_t row = constant_row ? uint32_t(constant_row->getZExtValue()) : 0;
  auto component = constant_operand(call, 3);
  if (!component)
    return component.takeError();

  auto slot = resolve_slot(call, signature.find_input(element_id.get(), row), component.get(), row);
  if (!slot)
    return slot.takeError();
  auto *element = slot.get();
  if (!constant_row && (element->semantic_kind != NAME_UNDEFINED || element->vertex_value_index != ~0u))
    return createError("DXIL dynamic input row requires a packed user varying");

  auto &builder = ctx.builder;
  builder.SetInsertPoint(&call);

  llvm::Value *value;
  if (element->semantic_kind == NAME_SHADINGRATE) {
    value = element->air_index == ~0u ? static_cast<llvm::Value *>(builder.getInt32(0))
                                    : ctx.function->getArg(element->air_index);
  } else if (element->semantic_kind == NAME_INNER_COVERAGE) {
    value = element->air_index == ~0u ? static_cast<llvm::Value *>(builder.getInt32(0))
                                    : ctx.function->getArg(element->air_index);
  } else if (element->semantic_kind == NAME_BARYCENTRICS) {
    value = builder.CreateExtractElement(ctx.function->getArg(element->air_index), element->start_col + *component);
  } else if (element->vertex_value_index != ~0u) {
    /* A plain read of a per-vertex (nointerpolation) attribute is the provoking vertex. */
    value = load_vertex_value(ctx, *element, *component, builder.getInt32(0));
  } else if (element->semantic_kind == NAME_COVERAGE) {
    /* Coverage arrives as a scalar argument; the DX 5.x path keeps it in
     * io_binding_map instead of the input register file. */
    if (!ctx.resource.coverage_mask_arg)
      return createError("SV_Coverage is read without being declared");
    value = ctx.resource.coverage_mask_arg;
  } else {
    auto *registers = to_register_component_type(element->comp_type) ==
                          dxbc::RegisterComponentType::Float
                        ? ctx.resource.input.ptr_float4
                        : ctx.resource.input.ptr_int4;
    auto *registers_type =
      cast<llvm::PointerType>(registers->getType())->getNonOpaquePointerElementType();
    auto *lane_type = cast<llvm::FixedVectorType>(
                        cast<llvm::ArrayType>(registers_type)->getArrayElementType()
                      )
                        ->getElementType();
    value = builder.CreateLoad(
      lane_type,
      builder.CreateGEP(
        registers_type, registers,
        {builder.getInt32(0), builder.CreateAdd(builder.getInt32(element->reg), call.getArgOperand(2)),
         builder.getInt32(element->start_col + component.get())}
      )
    );
  }
  value = narrow_io_value(builder, value, call.getType());

  call.replaceAllUsesWith(value);
  call.eraseFromParent();
  return llvm::Error::success();
}

llvm::Error lower_store_output(
  dxbc::context &ctx, const StageSignature &signature, llvm::CallInst &call
) {
  auto element_id = constant_operand(call, 1);
  if (!element_id)
    return element_id.takeError();
  auto row = constant_operand(call, 2);
  if (!row)
    return row.takeError();
  auto component = constant_operand(call, 3);
  if (!component)
    return component.takeError();

  auto slot =
    resolve_slot(call, signature.find_output(element_id.get(), row.get()), component.get(), row.get());
  if (!slot)
    return slot.takeError();
  auto *element = slot.get();

  auto &builder = ctx.builder;
  auto *value = call.getArgOperand(4);
  builder.SetInsertPoint(&call);

  /* The DX 5.x path keeps these three system values in dedicated registers
   * rather than in the packed output register file. */
  llvm::AllocaInst *dedicated = nullptr;
  bool needs_float = false;
  switch (element->semantic_kind) {
  case NAME_DEPTH:
  case NAME_DEPTH_GREATER_EQUAL:
  case NAME_DEPTH_LESS_EQUAL:
    dedicated = ctx.resource.depth_output_reg;
    needs_float = true;
    break;
  case NAME_COVERAGE:
    dedicated = ctx.resource.coverage_mask_reg;
    break;
  case NAME_STENCIL_REF:
    dedicated = ctx.resource.stencil_ref_reg;
    break;
  default:
    break;
  }

  if (dedicated) {
    auto *wanted = needs_float ? (llvm::Type *)builder.getFloatTy() : builder.getInt32Ty();
    if (value->getType() != wanted) {
      if (value->getType()->isIntegerTy() && value->getType()->getIntegerBitWidth() < 32)
        value = builder.CreateZExt(value, wanted);
      else if (value->getType()->getPrimitiveSizeInBits() == 32)
        value = builder.CreateBitCast(value, wanted);
      else
        return createError(
          "dx.op.storeOutput writes signature element " + llvm::Twine(element->element_id) +
          " with a type that does not fit its system value"
        );
    }
    builder.CreateStore(
      value, builder.CreateConstInBoundsGEP1_32(wanted, dedicated, 0)
    );
    call.eraseFromParent();
    return llvm::Error::success();
  }

  value = widen_io_value(builder, value, element->comp_type);
  auto *registers =
    value->getType()->isFloatTy() ? ctx.resource.output.ptr_float4 : ctx.resource.output.ptr_int4;
  auto *registers_type =
    cast<llvm::PointerType>(registers->getType())->getNonOpaquePointerElementType();
  builder.CreateStore(
    value,
    builder.CreateGEP(
      registers_type, registers,
      {builder.getInt32(0), builder.getInt32(element->reg + row.get()),
       builder.getInt32(element->start_col + component.get())}
    )
  );
  call.eraseFromParent();
  return llvm::Error::success();
}

llvm::Error lower_evaluate_input(dxbc::context &ctx, const StageSignature &signature,
                                 llvm::CallInst &call) {
  auto id = constant_operand(call, 1);
  auto row = constant_operand(call, 2);
  auto component = constant_operand(call, 3);
  if (!id) return id.takeError();
  if (!row) return row.takeError();
  if (!component) return component.takeError();
  auto slot = resolve_slot(call, signature.find_input(*id, *row), *component, *row);
  if (!slot) return slot.takeError();
  auto *element = *slot;
  auto &builder = ctx.builder;
  builder.SetInsertPoint(&call);
  ctx.air.builder.SetInsertPoint(&call);
  if (!element->pull_mode) {
    // Constant interpolation is independent of sample and snapped offset.
    if (element->air_index == ~0u) return createError("DXIL evaluate input is not interpolatable");
    auto *registers = ctx.resource.input.ptr_float4;
    auto *type = cast<llvm::PointerType>(registers->getType())->getNonOpaquePointerElementType();
    auto *value = builder.CreateLoad(builder.getFloatTy(), builder.CreateGEP(type, registers,
      {builder.getInt32(0), builder.getInt32(element->reg + *row),
       builder.getInt32(element->start_col + *component)}));
    call.replaceAllUsesWith(value);
    call.eraseFromParent();
    return llvm::Error::success();
  }
  // The input register can contain several independently packed semantics;
  // the dx.op element id, not its register number, identifies the interpolant.
  auto *interpolant = ctx.function->getArg(element->air_index);
  bool perspective = element->interpolation == INTERP_LINEAR ||
                     element->interpolation == INTERP_LINEAR_CENTROID ||
                     element->interpolation == INTERP_LINEAR_SAMPLE;
  auto *callee = call.getCalledFunction();
  if (!callee) return createError("DXIL evaluate input is an indirect call");
  llvm::Value *value = nullptr;
  if (callee->getName().startswith("dx.op.evalSampleIndex.")) {
    value = ctx.air.CreateInterpolateAtSample(interpolant, call.getArgOperand(4), perspective);
  } else if (callee->getName().startswith("dx.op.evalCentroid.")) {
    value = ctx.air.CreateInterpolateAtCentroid(interpolant, perspective);
  } else if (callee->getName().startswith("dx.op.evalSnapped.")) {
    llvm::Value *offset = llvm::UndefValue::get(llvm::FixedVectorType::get(builder.getFloatTy(), 2));
    for (unsigned i = 0; i < 2; ++i) {
      llvm::Value *snapped = builder.CreateAdd(call.getArgOperand(4 + i), builder.getInt32(8));
      const bool wide = ctx.function->getParent()->getModuleFlag("dxmt.coarse.snapped") != nullptr;
      if (!wide) snapped = builder.CreateAnd(snapped, builder.getInt32(15));

      // D3D offsets are relative to the pixel center; AIR/Metal offsets are
      // measured from its upper-left corner.
      auto *floatOffset = builder.CreateFMul(builder.CreateSIToFP(snapped, builder.getFloatTy()),
                                             llvm::ConstantFP::get(builder.getFloatTy(), 1.0 / 16.0));
      offset = builder.CreateInsertElement(offset, floatOffset, builder.getInt32(i));
    }
    value = ctx.air.CreateInterpolateAtOffset(interpolant, offset, perspective);
  } else return createError("unknown DXIL evaluate opcode");
  auto *lane = builder.CreateExtractElement(value, builder.getInt32(*component));
  if (lane->getType() != call.getType()) lane = builder.CreateBitCast(lane, call.getType());
  call.replaceAllUsesWith(lane);
  call.eraseFromParent();
  return llvm::Error::success();
}

/* dx.op.threadId (93), groupId (94) and threadIdInGroup (95) return one component
 * of the matching uint3 AIR argument; flattenedThreadIdInGroup (96) is the whole
 * uint argument. */
llvm::Error lower_thread_id(
  dxbc::context &ctx, llvm::CallInst &call, llvm::Value *argument, bool component_indexed,
  llvm::StringRef what
) {
  if (!argument)
    return createError(
      llvm::Twine(what) + " is used without being declared in the entry point signature"
    );
  auto &builder = ctx.builder;
  builder.SetInsertPoint(&call);
  llvm::Value *value = argument;
  if (component_indexed) {
    auto component = constant_operand(call, 1);
    if (!component)
      return component.takeError();
    auto *vector_type = dyn_cast<llvm::FixedVectorType>(value->getType());
    if (!vector_type || component.get() >= vector_type->getNumElements())
      return createError(llvm::Twine(what) + " component is out of range");
    value = builder.CreateExtractElement(value, component.get());
  }
  value = narrow_io_value(builder, value, call.getType());
  call.replaceAllUsesWith(value);
  call.eraseFromParent();
  return llvm::Error::success();
}

} // namespace

llvm::Value *ViewInstanceId(dxbc::context &ctx) {
  /* The view the command list replays this draw for (view instancing, Tier 1 draw looping); it writes 0 for a
   * PSO without view instancing, which is the value SV_ViewID must then read. */
  if (auto *view = ctx.binding.ViewInstanceId(ctx.air))
    return view;
  return ctx.builder.getInt32(0);
}

llvm::Error LowerViewInstanceId(dxbc::context &ctx) {
  llvm::SmallVector<llvm::CallInst *, 4> calls;
  for (auto &instruction : llvm::instructions(*ctx.function))
    if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        call && llvm::StringRef(callee_name(*call)).startswith("dx.op.viewID."))
      calls.push_back(call);
  for (auto *call : calls) {
    ctx.builder.SetInsertPoint(call);
    auto *value = ctx.builder.CreateZExtOrTrunc(ViewInstanceId(ctx), call->getType());
    call->replaceAllUsesWith(value);
    call->eraseFromParent();
  }
  return llvm::Error::success();
}

llvm::Error LowerStageIO(dxbc::context &ctx, const StageSignature &signature) {
  llvm::SmallVector<llvm::CallInst *, 16> load_inputs;
  llvm::SmallVector<llvm::CallInst *, 16> eval_inputs;
  llvm::SmallVector<llvm::CallInst *, 16> store_outputs;
  llvm::SmallVector<llvm::CallInst *, 16> thread_ids;
  llvm::SmallVector<llvm::CallInst *, 16> group_ids;
  llvm::SmallVector<llvm::CallInst *, 16> thread_id_in_groups;
  llvm::SmallVector<llvm::CallInst *, 16> flattened_thread_ids;
  llvm::SmallVector<llvm::CallInst *, 16> attribute_at_vertex;
  llvm::SmallVector<llvm::CallInst *, 4> command_values;

  for (auto &block : *ctx.function) {
    for (auto &instruction : block) {
      auto *call = dyn_cast<llvm::CallInst>(&instruction);
      if (!call)
        continue;
      auto name = callee_name(*call);
      llvm::StringRef name_ref(name);
      if (name_ref.startswith(LOAD_INPUT_PREFIX))
        load_inputs.push_back(call);
      else if (name_ref.startswith("dx.op.evalSnapped.") ||
               name_ref.startswith("dx.op.evalSampleIndex.") ||
               name_ref.startswith("dx.op.evalCentroid."))
        eval_inputs.push_back(call);
      else if (name_ref.startswith(STORE_OUTPUT_PREFIX))
        store_outputs.push_back(call);
      else if (name_ref.startswith(THREAD_ID_PREFIX))
        thread_ids.push_back(call);
      else if (name_ref.startswith(GROUP_ID_PREFIX))
        group_ids.push_back(call);
      else if (name_ref.startswith(THREAD_ID_IN_GROUP_PREFIX))
        thread_id_in_groups.push_back(call);
      else if (name_ref.startswith(FLATTENED_THREAD_ID_IN_GROUP_PREFIX))
        flattened_thread_ids.push_back(call);
      else if (name_ref.startswith("dx.op.attributeAtVertex."))
        attribute_at_vertex.push_back(call);
      else if (name_ref.startswith("dx.op.startVertexLocation.") ||
               name_ref.startswith("dx.op.startInstanceLocation.") || name_ref.startswith("dx.op.viewID."))
        command_values.push_back(call);
    }
  }

  for (auto *call : attribute_at_vertex) {
    if (auto err = lower_attribute_at_vertex(ctx, signature, *call))
      return err;
  }
  for (auto *call : command_values) {
    auto &builder = ctx.builder;
    builder.SetInsertPoint(call);
    llvm::Value *value = nullptr;
    auto name = callee_name(*call);
    llvm::StringRef name_ref(name);
    if (name_ref.startswith("dx.op.viewID.")) {
      value = ViewInstanceId(ctx);
    } else {
      /* SM 6.8 SV_StartVertexLocation is BaseVertexLocation (indexed) or
       * StartVertexLocation (non-indexed), exactly Metal's [[base_vertex]];
       * SV_StartInstanceLocation is Metal's [[base_instance]]. */
      const bool vertex = name_ref.startswith("dx.op.startVertexLocation.");
      value = vertex ? ctx.resource.base_vertex_id : ctx.resource.base_instance_id;
      if (!value)
        return createError(llvm::Twine(name_ref) + " outside a vertex shader");
    }
    if (value->getType() != call->getType())
      value = builder.CreateZExtOrTrunc(value, call->getType());
    call->replaceAllUsesWith(value);
    call->eraseFromParent();
  }

  for (auto *call : load_inputs) {
    if (auto err = lower_load_input(ctx, signature, *call))
      return err;
  }
  for (auto *call : eval_inputs) {
    if (auto err = lower_evaluate_input(ctx, signature, *call))
      return err;
  }
  for (auto *call : store_outputs) {
    if (auto err = lower_store_output(ctx, signature, *call))
      return err;
  }
  for (auto *call : thread_ids) {
    if (auto err = lower_thread_id(
          ctx, *call, ctx.resource.thread_id_arg, true, "dx.op.threadId"
        ))
      return err;
  }
  for (auto *call : group_ids) {
    if (auto err = lower_thread_id(
          ctx, *call, ctx.resource.thread_group_id_arg, true, "dx.op.groupId"
        ))
      return err;
  }
  for (auto *call : thread_id_in_groups) {
    if (auto err = lower_thread_id(
          ctx, *call, ctx.resource.thread_id_in_group_arg, true, "dx.op.threadIdInGroup"
        ))
      return err;
  }
  for (auto *call : flattened_thread_ids) {
    if (auto err = lower_thread_id(
          ctx, *call, ctx.resource.thread_id_in_group_flat_arg, false,
          "dx.op.flattenedThreadIdInGroup"
        ))
      return err;
  }
  return llvm::Error::success();
}

/* -------------------------------------------------------------------------- */
/* Entry point transplant                                                    */
/* -------------------------------------------------------------------------- */

llvm::Expected<llvm::BasicBlock *>
TransplantEntryBody(llvm::Function &entry, llvm::Function &function, llvm::BasicBlock &exit) {
  if (entry.isDeclaration())
    return createError("DXIL entry point '" + entry.getName() + "' has no body");
  if (!entry.getReturnType()->isVoidTy())
    return createError("DXIL entry point '" + entry.getName() + "' does not return void");
  for (auto &block : entry) {
    auto *ret = dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (ret && ret->getNumOperands())
      return createError("DXIL entry point '" + entry.getName() + "' returns a value");
  }

  /* SM 6.2 "fp32-denorm-mode" is an attribute of the DXIL entry; the body now runs in
   * `function`, where LowerOperations reads it. Bodies that share one Metal function
   * (VS+HS) must agree, except that "any" admits either mode. */
  if (auto mode = entry.getFnAttribute("fp32-denorm-mode"); mode.isStringAttribute()) {
    const llvm::StringRef incoming = mode.getValueAsString();
    auto existing = function.getFnAttribute("fp32-denorm-mode");
    if (!existing.isStringAttribute() || existing.getValueAsString() == "any")
      function.addFnAttr("fp32-denorm-mode", incoming);
    else if (incoming != "any" && incoming != existing.getValueAsString())
      return createError("DXIL stages sharing one Metal function declare different fp32 denorm modes (" +
                         existing.getValueAsString() + ", " + incoming + ")");
  }

  auto *body_entry = &entry.front();
  llvm::SmallVector<llvm::AllocaInst *, 8> static_allocations;
  for (auto &instruction : *body_entry) {
    if (auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
      if (allocation->isStaticAlloca())
        static_allocations.push_back(allocation);
    }
  }
  function.getBasicBlockList().splice(function.end(), entry.getBasicBlockList());
  /* Preserve static storage when introducing an AIR prologue. Leaving these in
   * the old entry makes LLVM treat them as dynamic allocations during inlining,
   * emitting stacksave/stackrestore intrinsics that AIR cannot execute. */
  for (auto *allocation : static_allocations)
    allocation->moveBefore(&*function.getEntryBlock().getFirstInsertionPt());

  for (auto &block : function) {
    /* The AIR epilogue block is still empty at this point. */
    auto *ret = dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (!ret)
      continue;
    llvm::IRBuilder<> builder(ret);
    builder.CreateBr(&exit);
    ret->eraseFromParent();
  }
  return body_entry;
}

/* -------------------------------------------------------------------------- */
/* LowerStage                                                                */
/* -------------------------------------------------------------------------- */

llvm::Expected<std::unique_ptr<dxbc::BindingMap>> LowerStage(
  ModuleInfo &module, dxbc::SM50ShaderInternal &shader, const char *function_name,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
) {
  if (!module.module || !module.entry)
    return createError("DXIL module was not parsed");
  if (module.stage == Stage::Mesh || module.stage == Stage::Amplification)
    return LowerMeshStage(module, shader, function_name, args);
  if (module.stage != Stage::Vertex && module.stage != Stage::Pixel &&
      module.stage != Stage::Compute)
    return unsupported("DXIL entry point stage");

  auto &module_ir = *module.module;
  auto &llvm_context = *module.context;

  /* DXIL containers carry the dxil-ms-dx 32 bit pointer layout; AIR needs the
   * air64 layout and the air.* module flags. */
  if (module_ir.getDataLayout().getPointerSizeInBits() != 64)
    initializeModule(module_ir);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (dxbc::args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(args, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  dxbc::args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(args, &rootsig);
  if (!rootsig)
    return createError("DXIL shader requires a root signature");
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *stats = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(args, &stats);

  uint32_t pso_sample_mask = 0xffffffff;
  bool pso_dual_source_blending = false;
  bool pso_disable_depth_output = false;
  uint32_t pso_unorm_output_reg_mask = 0;
  uint32_t *pso_pixel_formats = nullptr;
  SM50_SHADER_PSO_PIXEL_SHADER_DATA *pso_data = nullptr;
  if (dxbc::args_get_data<SM50_SHADER_PSO_PIXEL_SHADER, SM50_SHADER_PSO_PIXEL_SHADER_DATA>(
        args, &pso_data
      )) {
    pso_dual_source_blending = pso_data->dual_source_blending;
    pso_disable_depth_output = pso_data->disable_depth_output;
    pso_unorm_output_reg_mask = pso_data->unorm_output_reg_mask;
    pso_sample_mask = pso_data->sample_mask;
    pso_pixel_formats = pso_data->pixel_formats;
  }
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *coarse_mask_args = nullptr;
  const bool coarse_mask_output = pso_sample_mask != 0xffffffff &&
    dxbc::args_get_data<SM50_SHADER_D3D12_COARSE_SHADING, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(args, &coarse_mask_args);
  // The captured path applies SampleMask per fine pixel. Applying it to the original
  // body would truncate coarse SV_Coverage to the first fine pixel's samples. Keep
  // the implicit output shape, but let coverage capture/replay apply the actual mask.
  if (coarse_mask_output) pso_sample_mask = 0xffffffff;
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  dxbc::args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(
    args, &ia_layout
  );
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *vertex_so = nullptr;
  if (module.stage == Stage::Vertex &&
      dxbc::args_get_data<
        SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT, SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA>(
        args, &vertex_so
      ))
    return unsupported("stream output for a DXIL vertex shader");
  /* Same contract as the DXBC vertex path: without rasterization the AIR vertex
   * function returns void, which is what a rasterization-disabled Metal
   * pipeline requires. */
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool rasterization_disabled =
    module.stage == Stage::Vertex &&
    dxbc::args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(args, &gs_passthrough) &&
    gs_passthrough->RasterizationDisabled;
  SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA *so_capture = nullptr;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *index_pull = nullptr;
  if (dxbc::args_get_data<SM50_SHADER_D3D12_INDEX_PULL, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(args, &index_pull) &&
      module.stage != Stage::Vertex)
    return unsupported("index pulling outside a vertex shader");
  std::vector<dxbc::StreamOutputCaptureRun> so_capture_runs;
  if (dxbc::args_get_data<
        SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE, SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA>(
        args, &so_capture
      )) {
    if (module.stage != Stage::Vertex)
      return unsupported("stream output capture outside a vertex shader");
    SM50_SHADER_D3D12_MSAA_VERTEX_DATA *msaa_vertex = nullptr;
    dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_VERTEX, SM50_SHADER_D3D12_MSAA_VERTEX_DATA>(args, &msaa_vertex);
    if (index_pull && !(msaa_vertex && msaa_vertex->variant == SM50_MSAA_VERTEX_CAPTURE))
      return unsupported("standalone index pulling and stream output on the same vertex shader");
    for (uint32_t i = 0; i < so_capture->num_elements; i++) {
      auto &element = so_capture->elements[i];
      if (!element.semantic_name)
        return unsupported("stream output capture element has no semantic name");
      auto output = std::find_if(
        module.output_sig.begin(), module.output_sig.end(), [&](const SigElement &sig) {
          return sig.stream == 0 && sig.semantic_index == element.semantic_index &&
                 llvm::StringRef(sig.name).equals_insensitive(element.semantic_name);
        }
      );
      if (output == module.output_sig.end())
        return unsupported(
          (llvm::Twine("stream output capture element ") + element.semantic_name +
           llvm::Twine(element.semantic_index) + " is not a vertex shader output")
            .str()
        );
      /* Matches the storeOutput lowering: element row 0 lives at start_row and
       * its components at the mask's columns of the packed output registers. */
      auto run = dxbc::resolve_stream_output_capture(
        *so_capture, element, output->start_row, element_mask(*output)
      );
      if (!run)
        return run.takeError();
      so_capture_runs.push_back(*run);
    }
  }

  auto shader_type = to_program_type(module.stage);
  shader.shader_type = shader_type;
  shader.max_input_register = 0;
  shader.max_output_register = 0;
  shader.pso_valid_output_reg_mask = 0;

  auto &func_signature = shader.func_signature;

  dxbc::IREffect prologue([](auto) { return std::monostate(); });
  if (module.stage == Stage::Pixel && module.force_early_depth_stencil)
    func_signature.UseEarlyFragmentTests();
  dxbc::IRValue epilogue([](dxbc::context ctx) -> dxbc::pvalue {
    auto *return_type = ctx.function->getReturnType();
    if (return_type->isVoidTy())
      return nullptr;
    return llvm::ConstantAggregateZero::get(return_type);
  });

  dxbc::io_binding_map resource_map;
  air::AirType types(llvm_context);
  StageSignature signature;
  if (module.stage == Stage::Pixel) {
    for (auto &instruction : llvm::instructions(*module.entry)) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (!call || call->arg_size() < 4) continue;
      auto name = callee_name(*call);
      llvm::StringRef name_ref(name);
      if (!name_ref.startswith("dx.op.evalSnapped.") &&
          !name_ref.startswith("dx.op.evalSampleIndex.") &&
          !name_ref.startswith("dx.op.evalCentroid.")) continue;
      auto *id = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
      if (!id) return createError("DXIL evaluated input has a dynamic signature element");
      auto found = std::find_if(module.input_sig.begin(), module.input_sig.end(),
        [&](const SigElement &element) { return element.id == id->getZExtValue(); });
      if (found == module.input_sig.end()) return createError("DXIL evaluated input has no signature element");
      for (unsigned row = 0; row < found->rows; ++row) {
        if (found->start_row + row >= 32) return createError("DXIL pull input register exceeds mask");
        if (found->interp != INTERP_CONSTANT)
          shader.shader_info.pull_mode_reg_mask |= 1u << (found->start_row + row);
      }
    }
  }

  {
    dxbc::SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.skip_vertex_output = rasterization_disabled;
    sig_ctx.dual_source_blending = pso_dual_source_blending;
    sig_ctx.disable_depth_output = pso_disable_depth_output;
    SM50_SHADER_D3D12_MSAA_PIXEL_DATA *msaa_pixel = nullptr;
    dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_PIXEL, SM50_SHADER_D3D12_MSAA_PIXEL_DATA>(args, &msaa_pixel);
    sig_ctx.captured_pixel = msaa_pixel != nullptr;
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *coarse = nullptr;
    dxbc::args_get_data<SM50_SHADER_D3D12_COARSE_SHADING, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(args, &coarse);
    if (coarse) module_ir.addModuleFlag(llvm::Module::Error, "dxmt.coarse.snapped", 1);
    sig_ctx.pull_mode_reg_mask = msaa_pixel ? 0xffffffffu : shader.shader_info.pull_mode_reg_mask;
    sig_ctx.unorm_output_reg_mask = pso_unorm_output_reg_mask;
    if (pso_pixel_formats)
      memcpy(sig_ctx.pixel_formats, pso_pixel_formats, sizeof(sig_ctx.pixel_formats));
    if (auto err = DeclareStageSignature(module, shader, sig_ctx, signature))
      return std::move(err);
  }

  // DXIL carries SV_SampleIndex as a dedicated intrinsic, not a loadInput
  // signature element. Share the AIR argument with sample-interpolated inputs.
  llvm::SmallVector<llvm::CallInst *, 4> sample_indices;
  if (auto *intrinsic = module_ir.getFunction("dx.op.sampleIndex.i32")) {
    for (auto *user : intrinsic->users()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (module.stage != Stage::Pixel || !call || call->getFunction() != module.entry ||
          call->arg_size() != 1 || !call->getType()->isIntegerTy(32))
        return createError("invalid dx.op.sampleIndex use");
      auto opcode = constant_operand(*call, 0);
      if (!opcode)
        return opcode.takeError();
      if (*opcode != 90)
        return createError("invalid dx.op.sampleIndex opcode");
      sample_indices.push_back(call);
    }
  }
  const auto sample_index_arg = sample_indices.empty() ? ~0u : func_signature.DefineInput(air::InputSampleIndex{});
  // SV_Coverage input is the dx.op.coverage intrinsic (opcode 91) in DXIL; like DXBC's vCoverage it reads the
  // native [[sample_mask]] input ANDed with the PSO SampleMask.
  llvm::SmallVector<llvm::CallInst *, 4> coverage_inputs;
  if (auto *intrinsic = module_ir.getFunction("dx.op.coverage.i32")) {
    for (auto *user : intrinsic->users()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (module.stage != Stage::Pixel || !call || call->getFunction() != module.entry ||
          call->arg_size() != 1 || !call->getType()->isIntegerTy(32))
        return createError("invalid dx.op.coverage use");
      auto opcode = constant_operand(*call, 0);
      if (!opcode)
        return opcode.takeError();
      if (*opcode != 91)
        return createError("invalid dx.op.coverage opcode");
      coverage_inputs.push_back(call);
    }
  }
  const auto coverage_input_arg =
    coverage_inputs.empty() ? ~0u : func_signature.DefineInput(air::InputInputCoverage{});
  llvm::SmallVector<llvm::CallInst *, 4> inner_inputs;
  if (auto *intrinsic = module_ir.getFunction("dx.op.innerCoverage.i32")) {
    for (auto *user : intrinsic->users()) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(user);
      if (module.stage != Stage::Pixel || !call || call->getFunction() != module.entry ||
          call->arg_size() != 1 || !call->getType()->isIntegerTy(32))
        return createError("invalid dx.op.innerCoverage use");
      auto opcode = constant_operand(*call, 0);
      if (!opcode) return opcode.takeError();
      if (*opcode != 92) return createError("invalid dx.op.innerCoverage opcode");
      inner_inputs.push_back(call);
    }
  }
  const bool has_inner = !inner_inputs.empty() || std::any_of(module.input_sig.begin(), module.input_sig.end(),
    [](const SigElement &input) { return input.semantic_kind == NAME_INNER_COVERAGE; });
  if (has_inner && (!coverage_inputs.empty() || std::any_of(module.input_sig.begin(), module.input_sig.end(),
      [](const SigElement &input) { return input.semantic_kind == NAME_COVERAGE; })))
    return createError("SV_Coverage and SV_InnerCoverage inputs are mutually exclusive");
  SM50_SHADER_D3D12_MSAA_PIXEL_DATA *inner_pixel = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_PIXEL, SM50_SHADER_D3D12_MSAA_PIXEL_DATA>(args, &inner_pixel);
  const auto inner_input_arg = has_inner && inner_pixel ? func_signature.DefineInput(air::InputFragmentStageIn{
    .user = "dxmt_inner_coverage", .type = air::msl_uint,
    .interpolation = air::Interpolation::flat, .pull_mode = false,
  }) : ~0u;

  /* Vertex stages always declare the four IA values: they are what the AIR
   * stage_in metadata carries for [[vertex_id]] / [[instance_id]] and what the
   * input assembly pull needs to index the vertex buffers. */
  uint32_t vertex_idx = 0, base_vertex_idx = 0, instance_idx = 0, base_instance_idx = 0;
  dxbc::VertexIndexPullArgs vertex_index_args;
  uint32_t clip_distance_out_idx = ~0u;
  if (module.stage == Stage::Vertex) {
    vertex_idx = func_signature.DefineInput(air::InputVertexID{});
    base_vertex_idx = func_signature.DefineInput(air::InputBaseVertex{});
    instance_idx = func_signature.DefineInput(air::InputInstanceID{});
    base_instance_idx = func_signature.DefineInput(air::InputBaseInstance{});
    if (!rasterization_disabled && !shader.clip_distance_scalars.empty())
      clip_distance_out_idx = func_signature.DefineOutput(
        air::OutputClipDistance{shader.clip_distance_scalars.size()}
      );
    if (so_capture)
      vertex_index_args = dxbc::define_stream_output_capture(
        func_signature, epilogue, std::move(so_capture_runs), so_capture->record_dwords
      );
    else if (index_pull)
      vertex_index_args = dxbc::define_vertex_index_pull(func_signature);
  }

  dxbc::initialize_sample_mask_constant(module_ir, args);
  if (module.stage == Stage::Pixel && (pso_sample_mask != 0xffffffff || coarse_mask_output)) {
    auto assigned_index = func_signature.DefineOutput(air::OutputCoverageMask{});
    epilogue >> [=](dxbc::pvalue value) -> dxbc::IRValue {
      return dxbc::make_irvalue([=](dxbc::context ctx) {
        if (ctx.resource.coverage_mask_reg)
          return value;
        return ctx.builder.CreateInsertValue(
          value, dxbc::pixel_sample_mask(ctx), {assigned_index}
        );
      });
    };
  }

  const auto stats_arg = stats ? dxbc::define_pipeline_statistics(func_signature) : ~0u;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *per_pixel = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(
    args, &per_pixel
  );
  std::optional<dxbc::PixelStatisticsDedupe> stats_dedupe;
  if (stats && per_pixel && module.stage == Stage::Pixel)
    stats_dedupe = dxbc::define_pixel_statistics_dedupe(func_signature);
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *claim_arg = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(
    args, &claim_arg
  );
  std::optional<dxbc::PixelStatisticsClaim> stats_claim;
  if (stats && claim_arg && module.stage == Stage::Pixel)
    stats_claim = dxbc::define_pixel_statistics_claim(func_signature);
  auto binding_map = dxbc::setup_binding_rootsig(
    &shader.shader_info, func_signature, module_ir, shader_type, rootsig->bytecode,
    rootsig->bytecode_length
  );
  if (!binding_map)
    return createError("DXIL shader has an invalid root signature");

  /* The runtime looks the AIR function up by name, so the DXIL entry point has
   * to give the name up before the AIR function is created. */
  module.entry->setName(llvm::Twine(function_name) + ".dxil");
  auto [function, function_metadata] =
    func_signature.CreateFunction(function_name, llvm_context, module_ir, 0, rasterization_disabled);
  for (auto *call : sample_indices) {
    call->replaceAllUsesWith(function->getArg(sample_index_arg));
    call->eraseFromParent();
  }
  for (auto *call : coverage_inputs) {
    llvm::Value *coverage = function->getArg(coverage_input_arg);
    if (pso_sample_mask != 0xffffffff) {
      llvm::IRBuilder<> builder(call);
      llvm::Value *mask = builder.getInt32(pso_sample_mask);
      if (auto *constant = module_ir.getGlobalVariable("dxmt_sample_mask.MTL_FC_INIT_0_j"))
        mask = builder.CreateLoad(builder.getInt32Ty(), constant);
      coverage = builder.CreateAnd(coverage, mask, "coverage");
    }
    call->replaceAllUsesWith(coverage);
    call->eraseFromParent();
  }
  for (auto *call : inner_inputs) {
    call->replaceAllUsesWith(inner_input_arg == ~0u ? static_cast<llvm::Value *>(llvm::ConstantInt::get(call->getType(), 0))
                                                   : function->getArg(inner_input_arg));
    call->eraseFromParent();
  }

  auto *entry_bb = llvm::BasicBlock::Create(llvm_context, "entry", function);
  auto *epilogue_bb = llvm::BasicBlock::Create(llvm_context, "epilogue", function);
  dxbc::setup_metal_version(module_ir, metal_version);
  if (inner_pixel) dxmt::msaa::preparePixelTarget(module_ir, *inner_pixel);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
    {
      .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
      .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
    },
    builder, nulldbg
  );

  if (module.stage == Stage::Vertex) {
    resource_map.vertex_id_with_base = function->getArg(vertex_idx);
    resource_map.base_vertex_id = function->getArg(base_vertex_idx);
    resource_map.instance_id_with_base = function->getArg(instance_idx);
    resource_map.base_instance_id = function->getArg(base_instance_idx);
    resource_map.vertex_id =
      builder.CreateSub(resource_map.vertex_id_with_base, resource_map.base_vertex_id);
    resource_map.instance_id =
      builder.CreateSub(resource_map.instance_id_with_base, resource_map.base_instance_id);
  }

  if (shader.max_input_register) {
    resource_map.input.ptr_int4 =
      builder.CreateAlloca(llvm::ArrayType::get(types._int4, shader.max_input_register));
    resource_map.input.ptr_float4 = builder.CreateBitCast(
      resource_map.input.ptr_int4,
      llvm::ArrayType::get(types._float4, shader.max_input_register)->getPointerTo()
    );
    resource_map.input_element_count = shader.max_input_register;
  }
  if (shader.max_output_register) {
    resource_map.output.ptr_int4 =
      builder.CreateAlloca(llvm::ArrayType::get(types._int4, shader.max_output_register));
    resource_map.output.ptr_float4 = builder.CreateBitCast(
      resource_map.output.ptr_int4,
      llvm::ArrayType::get(types._float4, shader.max_output_register)->getPointerTo()
    );
    resource_map.output_element_count = shader.max_output_register;
  }

  dxbc::setup_temp_register(&shader.shader_info, resource_map, types, module_ir, builder);
  dxbc::setup_immediate_constant_buffer(&shader.shader_info, resource_map, types, module_ir, builder);
  PrepareModuleStorage(module_ir);

  dxbc::context ctx{
    .builder = builder,
    .air = air,
    .binding = *binding_map,
    .llvm = llvm_context,
    .module = module_ir,
    .function = function,
    .resource = resource_map,
    .types = types,
    .pso_sample_mask = pso_sample_mask,
    .shader_type = shader_type,
    .metal_version = metal_version,
  };

  if (so_capture)
    resource_map.so_capture_key = resource_map.vertex_id;
  if (so_capture || index_pull)
    dxbc::apply_vertex_index_pull(
      builder, function, vertex_index_args, resource_map,
      so_capture ? offsetof(SM50_SO_CAPTURE_CONTROL, index_size) / 4 : 0
    );
  if (auto err = prologue.build(ctx).takeError())
    return std::move(err);
  if (stats_claim)
    dxbc::claim_pixel_invocation(builder, air, function, stats_arg, *stats_claim);
  else if (stats && module.stage == Stage::Pixel)
    dxbc::increment_pixel_invocations(builder, air, function, stats_arg, stats_dedupe ? &*stats_dedupe : nullptr);
  else if (stats)
    dxbc::increment_pipeline_statistics(
      builder, air, function, stats_arg, module.stage == Stage::Vertex ? 4 : 20
    );

  auto body_entry = TransplantEntryBody(*module.entry, *function, *epilogue_bb);
  if (!body_entry)
    return body_entry.takeError();

  /* An AIR prologue helper may have left the builder in a continuation block of
   * its own (input assembly pull), which is where the body has to be joined. */
  builder.CreateBr(*body_entry);

  if (auto err = LowerStageIO(ctx, signature))
    return std::move(err);

  builder.SetInsertPoint(epilogue_bb);
  auto epilogue_result = epilogue.build(ctx);
  if (!epilogue_result)
    return epilogue_result.takeError();
  auto *return_value = epilogue_result.get();

  if (clip_distance_out_idx != ~0u) {
    auto *clip_distance_type =
      llvm::ArrayType::get(types._float, shader.clip_distance_scalars.size());
    auto *clip_distance_array = (llvm::Value *)llvm::ConstantAggregateZero::get(clip_distance_type);
    unsigned clip_distance_index = 0;
    for (auto &scalar : shader.clip_distance_scalars) {
      auto *source = builder.CreateGEP(
        resource_map.output.ptr_float4->getType()->getNonOpaquePointerElementType(),
        resource_map.output.ptr_float4,
        {builder.getInt32(0), builder.getInt32(scalar.reg), builder.getInt32(scalar.component)}
      );
      clip_distance_array = builder.CreateInsertValue(
        clip_distance_array, builder.CreateLoad(types._float, source),
        {clip_distance_index++}
      );
    }
    return_value =
      builder.CreateInsertValue(return_value, clip_distance_array, {clip_distance_out_idx});
  }

  if (return_value == nullptr)
    builder.CreateRetVoid();
  else
    builder.CreateRet(return_value);

  llvm::StringRef kind = module.stage == Stage::Pixel    ? "air.fragment"
                         : module.stage == Stage::Vertex ? "air.vertex"
                                                         : "air.kernel";
  module_ir.getOrInsertNamedMetadata(kind)->addOperand(function_metadata);

  return std::move(binding_map);
}

} // namespace dxmt::dxil
