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

#include "air_signature.hpp"
#include "airconv_context.hpp"
#include "airconv_public.h"
#include "nt/air_builder.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

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
constexpr uint8_t NAME_TARGET = 64;
constexpr uint8_t NAME_DEPTH = 65;
constexpr uint8_t NAME_COVERAGE = 66;
constexpr uint8_t NAME_DEPTH_GREATER_EQUAL = 67;
constexpr uint8_t NAME_DEPTH_LESS_EQUAL = 68;
constexpr uint8_t NAME_STENCIL_REF = 69;

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

dxbc::RegisterComponentType to_register_component_type(uint8_t component_type) {
  if (component_type < (uint8_t)dxbc::RegisterComponentType::Uint ||
      component_type > (uint8_t)dxbc::RegisterComponentType::Float)
    return dxbc::RegisterComponentType::Unknown;
  return (dxbc::RegisterComponentType)component_type;
}

air::InputAttributeComponentType to_attribute_component_type(uint8_t component_type) {
  if (component_type < (uint8_t)dxbc::RegisterComponentType::Uint ||
      component_type > (uint8_t)dxbc::RegisterComponentType::Float)
    return air::InputAttributeComponentType::Unknown;
  return (air::InputAttributeComponentType)component_type;
}

/** Cross stage varying name, identical to dxbc::Signature::consistentAttributeName. */
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

SignatureSlot make_slot(const SigElement &element, uint32_t air_index) {
  SignatureSlot slot;
  slot.element_id = element.id;
  slot.semantic_index = element.semantic_index;
  slot.reg = element.start_row;
  slot.rows = element.rows;
  slot.start_col = element_start_col(element);
  slot.mask = element_mask(element);
  slot.air_index = air_index;
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

const SignatureSlot *StageSignature::find_input(uint32_t element_id) const {
  for (auto &slot : inputs) {
    if (slot.element_id == element_id)
      return &slot;
  }
  return nullptr;
}

const SignatureSlot *StageSignature::find_output(uint32_t element_id) const {
  for (auto &slot : outputs) {
    if (slot.element_id == element_id)
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
            func_signature, reg, mask, element_info, ia_layout->slot_mask
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
    switch (element.semantic_kind) {
    case NAME_POSITION: {
      air_index = func_signature.DefineOutput(air::OutputPosition{.type = air::msl_float4});
      epilogue >> dxbc::pop_output_reg_sanitize_pos(reg, mask, air_index);
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
        .user = std::string(cull ? "SV_CullDistance" : "SV_ClipDistance") + std::to_string(reg),
        .type = air::msl_float4,
      });
      epilogue >> dxbc::pop_output_reg(reg, mask, air_index);
      break;
    }
    case NAME_RENDER_TARGET_ARRAY_INDEX: {
      air_index = func_signature.DefineOutput(air::OutputRenderTargetArrayIndex{});
      epilogue >> dxbc::pop_output_reg(reg, mask, air_index);
      break;
    }
    case NAME_VIEWPORT_ARRAY_INDEX: {
      air_index = func_signature.DefineOutput(air::OutputViewportArrayIndex{});
      epilogue >> dxbc::pop_output_reg(reg, mask, air_index);
      break;
    }
    default: {
      air_index = func_signature.DefineOutput(air::OutputVertex{
        .user = consistent_attribute_name(reg, mask),
        .type = dxbc::to_msl_type(to_register_component_type(element.comp_type)),
      });
      epilogue >> dxbc::pop_output_reg(reg, mask, air_index);
      break;
    }
    }
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

  for (auto &element : module.input_sig) {
    uint32_t reg = element.start_row;
    uint32_t mask = element_mask(element);
    uint32_t air_index;
    bool init_plain = true;
    bool fix_w_component = false;
    switch (element.semantic_kind) {
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
        .user = std::string(
                  element.semantic_kind == NAME_CULL_DISTANCE ? "SV_CullDistance" : "SV_ClipDistance"
                ) +
                std::to_string(reg),
        .type = air::msl_float4,
        .interpolation = to_air_interpolation(element.interp),
        .pull_mode = false,
      });
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
    default: {
      auto interpolation = to_air_interpolation(element.interp);
      bool pull_mode = bool(sig_ctx.pull_mode_reg_mask & (1u << reg)) &&
                       interpolation != air::Interpolation::flat;
      air_index = func_signature.DefineInput(air::InputFragmentStageIn{
        .user = consistent_attribute_name(reg, mask),
        .type = dxbc::to_msl_type(to_register_component_type(element.comp_type)),
        .interpolation = interpolation,
        .pull_mode = pull_mode,
      });
      init_plain = false;
      if (!pull_mode) {
        prologue << dxbc::init_input_reg(air_index, reg, mask);
        break;
      }
      resource.interpolant_map[reg] = dxbc::interpolant_descriptor{
        [=](auto) {
          return dxbc::make_irvalue([=](dxbc::context ctx) {
            return ctx.function->getArg(air_index);
          });
        },
        interpolation == air::Interpolation::sample_perspective ||
          interpolation == air::Interpolation::center_perspective ||
          interpolation == air::Interpolation::centroid_perspective,
      };
      uint32_t sampleidx_at = ~0u;
      if (is_sample_interpolation(element.interp))
        sampleidx_at = func_signature.DefineInput(air::InputSampleIndex{});
      prologue << dxbc::init_input_reg_with_interpolation(
        air_index, reg, mask, interpolation, sampleidx_at
      );
      break;
    }
    }
    if (init_plain)
      prologue << dxbc::init_input_reg(air_index, reg, mask, fix_w_component);
    inputs.push_back(make_slot(element, air_index));
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
              ? ctx.builder.CreateAnd(mask, ctx.builder.getInt32(ctx.pso_sample_mask))
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

void declare_compute_signature(
  const ModuleInfo &module, dxbc::SM50ShaderInternal &shader, dxbc::SignatureContext &sig_ctx,
  StageSignature &out_signature
) {
  auto &func_signature = sig_ctx.func_signature;
  auto &prologue = sig_ctx.prologue;
  bool used[4] = {};
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

  shader.threadgroup_size[0] = module.num_threads[0];
  shader.threadgroup_size[1] = module.num_threads[1];
  shader.threadgroup_size[2] = module.num_threads[2];
  func_signature.UseMaxWorkgroupSize(
    module.num_threads[0] * module.num_threads[1] * module.num_threads[2]
  );
}

} // namespace

llvm::Error DeclareStageSignature(
  const ModuleInfo &module, dxbc::SM50ShaderInternal &shader, dxbc::SignatureContext &sig_ctx,
  StageSignature &out_signature
) {
  for (auto &element : module.input_sig) {
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
    if (module.num_threads[0] * module.num_threads[1] * module.num_threads[2] == 0)
      return createError("DXIL compute shader has no numthreads");
    declare_compute_signature(module, shader, sig_ctx, out_signature);
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
  if (row >= slot->rows || slot->start_col + component > 3)
    return createError(
      "dx.op I/O instruction addresses signature element " + llvm::Twine(slot->element_id) +
      " out of range"
    );
  return slot;
}

llvm::Error lower_load_input(
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

  auto slot = resolve_slot(call, signature.find_input(element_id.get()), component.get(), row.get());
  if (!slot)
    return slot.takeError();
  auto *element = slot.get();

  auto &builder = ctx.builder;
  builder.SetInsertPoint(&call);

  llvm::Value *value;
  if (element->semantic_kind == NAME_COVERAGE) {
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
        {builder.getInt32(0), builder.getInt32(element->reg + row.get()),
         builder.getInt32(element->start_col + component.get())}
      )
    );
  }
  if (value->getType() != call.getType())
    value = builder.CreateBitCast(value, call.getType());

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
    resolve_slot(call, signature.find_output(element_id.get()), component.get(), row.get());
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
  if (value->getType() != call.getType())
    value = builder.CreateBitCast(value, call.getType());
  call.replaceAllUsesWith(value);
  call.eraseFromParent();
  return llvm::Error::success();
}

} // namespace

llvm::Error LowerStageIO(dxbc::context &ctx, const StageSignature &signature) {
  llvm::SmallVector<llvm::CallInst *, 16> load_inputs;
  llvm::SmallVector<llvm::CallInst *, 16> store_outputs;
  llvm::SmallVector<llvm::CallInst *, 16> thread_ids;
  llvm::SmallVector<llvm::CallInst *, 16> group_ids;
  llvm::SmallVector<llvm::CallInst *, 16> thread_id_in_groups;
  llvm::SmallVector<llvm::CallInst *, 16> flattened_thread_ids;

  for (auto &block : *ctx.function) {
    for (auto &instruction : block) {
      auto *call = dyn_cast<llvm::CallInst>(&instruction);
      if (!call)
        continue;
      auto name = callee_name(*call);
      llvm::StringRef name_ref(name);
      if (name_ref.startswith(LOAD_INPUT_PREFIX))
        load_inputs.push_back(call);
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
    }
  }

  for (auto *call : load_inputs) {
    if (auto err = lower_load_input(ctx, signature, *call))
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

  auto shader_type = to_program_type(module.stage);
  shader.shader_type = shader_type;
  shader.max_input_register = 0;
  shader.max_output_register = 0;
  shader.pso_valid_output_reg_mask = 0;

  auto &func_signature = shader.func_signature;

  dxbc::IREffect prologue([](auto) { return std::monostate(); });
  dxbc::IRValue epilogue([](dxbc::context ctx) -> dxbc::pvalue {
    auto *return_type = ctx.function->getReturnType();
    if (return_type->isVoidTy())
      return nullptr;
    return llvm::ConstantAggregateZero::get(return_type);
  });

  dxbc::io_binding_map resource_map;
  air::AirType types(llvm_context);
  StageSignature signature;

  {
    dxbc::SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.dual_source_blending = pso_dual_source_blending;
    sig_ctx.disable_depth_output = pso_disable_depth_output;
    sig_ctx.pull_mode_reg_mask = shader.shader_info.pull_mode_reg_mask;
    sig_ctx.unorm_output_reg_mask = pso_unorm_output_reg_mask;
    if (pso_pixel_formats)
      memcpy(sig_ctx.pixel_formats, pso_pixel_formats, sizeof(sig_ctx.pixel_formats));
    if (auto err = DeclareStageSignature(module, shader, sig_ctx, signature))
      return std::move(err);
  }

  /* Vertex stages always declare the four IA values: they are what the AIR
   * stage_in metadata carries for [[vertex_id]] / [[instance_id]] and what the
   * input assembly pull needs to index the vertex buffers. */
  uint32_t vertex_idx = 0, base_vertex_idx = 0, instance_idx = 0, base_instance_idx = 0;
  uint32_t clip_distance_out_idx = ~0u;
  if (module.stage == Stage::Vertex) {
    vertex_idx = func_signature.DefineInput(air::InputVertexID{});
    base_vertex_idx = func_signature.DefineInput(air::InputBaseVertex{});
    instance_idx = func_signature.DefineInput(air::InputInstanceID{});
    base_instance_idx = func_signature.DefineInput(air::InputBaseInstance{});
    if (!shader.clip_distance_scalars.empty())
      clip_distance_out_idx = func_signature.DefineOutput(
        air::OutputClipDistance{shader.clip_distance_scalars.size()}
      );
  }

  if (module.stage == Stage::Pixel && pso_sample_mask != 0xffffffff) {
    auto assigned_index = func_signature.DefineOutput(air::OutputCoverageMask{});
    epilogue >> [=](dxbc::pvalue value) -> dxbc::IRValue {
      return dxbc::make_irvalue([=](dxbc::context ctx) {
        if (ctx.resource.coverage_mask_reg)
          return value;
        return ctx.builder.CreateInsertValue(
          value, ctx.builder.getInt32(ctx.pso_sample_mask), {assigned_index}
        );
      });
    };
  }

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
    func_signature.CreateFunction(function_name, llvm_context, module_ir, 0, false);

  auto *entry_bb = llvm::BasicBlock::Create(llvm_context, "entry", function);
  auto *epilogue_bb = llvm::BasicBlock::Create(llvm_context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
    {
      .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
      .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
    },
    builder, nulldbg
  );

  dxbc::setup_metal_version(module_ir, metal_version);

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

  if (auto err = prologue.build(ctx).takeError())
    return std::move(err);

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
