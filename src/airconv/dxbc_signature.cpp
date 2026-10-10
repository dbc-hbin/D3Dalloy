/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "dxbc_converter.hpp"

using namespace microsoft;
using namespace dxmt::air;
using namespace dxmt::shader::common;

namespace dxmt::dxbc {

inline std::optional<air::Interpolation>
to_air_interpolation(microsoft::D3D10_SB_INTERPOLATION_MODE mode) {
  using namespace microsoft;
  switch (mode) {
  case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE:
    return air::Interpolation::sample_no_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_SAMPLE:
    return air::Interpolation::sample_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_CENTROID:
    return air::Interpolation::centroid_no_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_CENTROID:
    return air::Interpolation::centroid_perspective;
  case D3D10_SB_INTERPOLATION_CONSTANT:
    return air::Interpolation::flat;
  case D3D10_SB_INTERPOLATION_LINEAR:
    return air::Interpolation::center_perspective;
  case D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE:
    return air::Interpolation::center_no_perspective;
  default:
    return std::nullopt;
  }
}

void handle_signature_vs(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  auto &signature_handlers = sm50_shader->signature_handlers;
  auto &func_signature = sm50_shader->func_signature;

  auto findInputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    inputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < inputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };
  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    // auto MinPrecision = Inst.m_Operands[0].m_MinPrecision; // not used
    auto sgv = Inst.m_InputDeclSGV.Name;
    switch (sgv) {
    case D3D10_SB_NAME_VERTEX_ID: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect_bind([=](struct context ctx) {
          return store_at_vec4_array_masked(
            ctx.resource.input.ptr_int4, ctx.builder.getInt32(reg),
            ctx.resource.vertex_id, mask
          );
        });
      });
      break;
    }
    case D3D10_SB_NAME_INSTANCE_ID: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect_bind([=](struct context ctx) {
          return store_at_vec4_array_masked(
            ctx.resource.input.ptr_int4, ctx.builder.getInt32(reg),
            ctx.resource.instance_id, mask
          );
        });
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC vertex shader system-value input";
      return;
    }
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_INPUT: {
      unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      assert(Inst.m_Operands[0].m_IndexDimension == D3D10_SB_OPERAND_INDEX_1D);

      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findInputElement([=](Signature &sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_input_register = std::max(reg + 1, max_input_register);
      if (sig.mask() == 0) break;
      signature_handlers.push_back(
        [=, type = (InputAttributeComponentType)sig.componentType(),
         name = sig.consistentAttributeName()](SignatureContext &ctx) {
          if (ctx.ia_layout) {
            for (unsigned i = 0; i < ctx.ia_layout->num_elements; i++) {
              if (ctx.ia_layout->elements[i].reg == reg) {
                ctx.prologue << pull_vertex_input(
                  ctx.func_signature, reg, mask, ctx.ia_layout->elements[i],
                  ctx.ia_layout->slot_mask, ctx.dense_vertex_table
                );
                break;
              }
            }
          } else {
            auto assigned_index = ctx.func_signature.DefineInput(
              InputVertexStageIn{.attribute = reg, .type = type, .name = name}
            );
            ctx.prologue << init_input_reg(assigned_index, reg, mask);
          }
        }
      );
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC vertex shader input declaration";
      return;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D10_SB_NAME_CLIP_DISTANCE: {
      auto sig = findOutputElement([=](const Signature &element) {
        return element.reg() == reg && (element.mask() & mask) != 0;
      });
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->clip_distance_scalars.push_back(
            {.component = (uint8_t)(i), .reg = (uint8_t)reg}
          );
        }
      }
      uint32_t assigned_index = func_signature.DefineOutput(OutputVertex{
        .user = varyingAttributeName("SV_ClipDistance", sig.semanticIndex()),
        .type = msl_float4,
      });
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg_semantic(reg, mask, assigned_index, true);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_CULL_DISTANCE: {
      auto sig = findOutputElement([=](const Signature &element) {
        return element.reg() == reg && (element.mask() & mask) != 0;
      });
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->cull_distance_scalars.push_back(
            {.component = (uint8_t)(i), .reg = (uint8_t)reg}
          );
        }
      }
      uint32_t assigned_index = func_signature.DefineOutput(OutputVertex{
        .user = varyingAttributeName("SV_CullDistance", sig.semanticIndex()),
        .type = msl_float4,
      });
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg_semantic(reg, mask, assigned_index, true);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_POSITION: {
      auto assigned_index =
        func_signature.DefineOutput(OutputPosition{.type = msl_float4});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg_sanitize_pos(reg, mask, assigned_index);
      });
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      auto assigned_index =
        func_signature.DefineOutput(OutputRenderTargetArrayIndex{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg(reg, mask, assigned_index);
      });
      break;
    }
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: {
      auto assigned_index =
        func_signature.DefineOutput(OutputViewportArrayIndex{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg(reg, mask, assigned_index);
      });
      break;
    }
    default:
      assert(0 && "Unexpected/unhandled input system value");
      break;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      if (sig.mask() == 0) break;
      uint32_t assigned_index = func_signature.DefineOutput(OutputVertex{
        .user = sig.varyingAttributeName(),
        .type = to_msl_type(sig.componentType()),
      });
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.skip_vertex_output)
          return;
        ctx.epilogue >> pop_output_reg_semantic(reg, mask, assigned_index);
      });
      break;
    }
    default:
      assert(0 && "unhandled vertex shader output");
      break;
    }
    break;
  }
  default:
    assert(0);
    break;
  }
};

void handle_signature_ps(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  uint32_t &pso_valid_output_reg_mask = sm50_shader->pso_valid_output_reg_mask;
  auto &signature_handlers = sm50_shader->signature_handlers;
  auto &func_signature = sm50_shader->func_signature;

  auto findInputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    inputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < inputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };
  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SIV:
  case D3D10_SB_OPCODE_DCL_INPUT_SGV:
    sm50_shader->initialization_error = "Unsupported DXBC pixel shader input declaration";
    return;
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK: {
      if (sm50_shader->ps_has_inner_coverage_input) {
        sm50_shader->initialization_error = "SV_Coverage and SV_InnerCoverage inputs are mutually exclusive";
        return;
      }
      sm50_shader->ps_has_coverage_input = true;
      auto assigned_index = func_signature.DefineInput(InputInputCoverage{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.coverage_mask_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INNER_COVERAGE: {
      if (sm50_shader->ps_has_coverage_input || sm50_shader->ps_has_inner_coverage_input) {
        sm50_shader->initialization_error = "Invalid or mutually exclusive SV_InnerCoverage declaration";
        return;
      }
      sm50_shader->ps_has_inner_coverage_input = true;
      signature_handlers.push_back([](SignatureContext &ctx) {
        // Ordinary user metadata is only a compiler placeholder. transformPixel removes it
        // from all native entries and supplies the gathered geometric value to the body.
        auto index = ctx.captured_pixel ? ctx.func_signature.DefineInput(InputFragmentStageIn{
          .user = "dxmt_inner_coverage", .type = msl_uint, .interpolation = Interpolation::flat,
          .pull_mode = false,
        }) : ~0u;
        ctx.prologue << make_effect([=](struct context ctx) {
          ctx.resource.inner_coverage_arg = index == ~0u ? static_cast<llvm::Value *>(ctx.builder.getInt32(0))
                                                        : ctx.function->getArg(index);
          return std::monostate{};
        });
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC pixel shader input declaration";
      return;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_InputPSDeclSIV.Name;
    auto interpolation_result = to_air_interpolation(Inst.m_InputPSDeclSIV.InterpolationMode);
    if (!interpolation_result) {
      sm50_shader->initialization_error = "Invalid DXBC pixel shader input interpolation mode";
      return;
    }
    auto interpolation = *interpolation_result;
    if (interpolation == Interpolation::sample_perspective ||
        interpolation == Interpolation::sample_no_perspective)
      sm50_shader->ps_sample_frequency = 1;
    uint32_t assigned_index;
    switch (siv) {
    case D3D10_SB_NAME_POSITION:
      // assert(
      //   interpolation == Interpolation::center_no_perspective ||
      //   // in case it's per-sample, FIXME: will this cause problem?
      //   interpolation == Interpolation::sample_no_perspective
      // );
      assigned_index = func_signature.DefineInput(
        // the only supported interpolation for [[position]]
        InputPosition{.interpolation = interpolation}
      );
      break;
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
      if (interpolation != Interpolation::flat) {
        sm50_shader->initialization_error = "Invalid interpolation for DXBC render-target-array input";
        return;
      }
      assigned_index =
        func_signature.DefineInput(InputRenderTargetArrayIndex{});
      break;
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX:
      if (interpolation != Interpolation::flat) {
        sm50_shader->initialization_error = "Invalid interpolation for DXBC viewport-array input";
        return;
      }
      assigned_index = func_signature.DefineInput(InputViewportArrayIndex{});
      break;
    case D3D10_SB_NAME_CLIP_DISTANCE: {
      auto sig = findInputElement([=](const Signature &element) {
        return element.reg() == reg && (element.mask() & mask) != 0;
      });
      assigned_index = func_signature.DefineInput(InputFragmentStageIn{
        .user = varyingAttributeName("SV_ClipDistance", sig.semanticIndex()),
        .type = msl_float4,
        .interpolation = interpolation,
        .pull_mode = false
      });
      break;
    }
    case D3D10_SB_NAME_CULL_DISTANCE: {
      auto sig = findInputElement([=](const Signature &element) {
        return element.reg() == reg && (element.mask() & mask) != 0;
      });
      assigned_index = func_signature.DefineInput(InputFragmentStageIn{
        .user = varyingAttributeName("SV_CullDistance", sig.semanticIndex()),
        .type = msl_float4,
        .interpolation = interpolation,
        .pull_mode = false
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC pixel shader system-value input";
      return;
    }
    signature_handlers.push_back([=](SignatureContext &ctx) {
      if (siv == D3D10_SB_NAME_CLIP_DISTANCE || siv == D3D10_SB_NAME_CULL_DISTANCE)
        ctx.prologue << init_input_reg_semantic(assigned_index, reg, mask);
      else
        ctx.prologue << init_input_reg(assigned_index, reg, mask, siv == D3D10_SB_NAME_POSITION);
    });
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_InputPSDeclSIV.Name;
    const auto interpolation_mode = Inst.m_InputPSDeclSGV.InterpolationMode;
    auto interpolation_result = interpolation_mode == D3D10_SB_INTERPOLATION_UNDEFINED
      ? std::optional<air::Interpolation>(Interpolation::flat)
      : to_air_interpolation(interpolation_mode);
    if (!interpolation_result) {
      sm50_shader->initialization_error = "Invalid DXBC pixel shader input interpolation mode";
      return;
    }
    auto interpolation = *interpolation_result;
    if (interpolation == Interpolation::sample_perspective ||
        interpolation == Interpolation::sample_no_perspective)
      sm50_shader->ps_sample_frequency = 1;
    uint32_t assigned_index;
    switch (siv) {
    case microsoft::D3D10_SB_NAME_IS_FRONT_FACE:
      if (interpolation != Interpolation::flat) {
        sm50_shader->initialization_error = "Invalid interpolation for DXBC front-face input";
        return;
      }
      assigned_index = func_signature.DefineInput(InputFrontFacing{});
      break;
    case microsoft::D3D10_SB_NAME_SAMPLE_INDEX:
      if (interpolation != Interpolation::flat) {
        sm50_shader->initialization_error = "Invalid interpolation for DXBC sample-index input";
        return;
      }
      assigned_index = func_signature.DefineInput(InputSampleIndex{});
      sm50_shader->ps_sample_frequency = 1;
      break;
    case microsoft::D3D10_SB_NAME_PRIMITIVE_ID:
      if (interpolation != Interpolation::flat) {
        sm50_shader->initialization_error = "Invalid interpolation for DXBC primitive-id input";
        return;
      }
      assigned_index = func_signature.DefineInput(InputPrimitiveID{});
      break;
    default:
      sm50_shader->initialization_error = "Unsupported DXBC pixel shader generated system-value input";
      return;
    }
    signature_handlers.push_back([=](SignatureContext &ctx) {
      ctx.prologue << init_input_reg(assigned_index, reg, mask);
    });
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_PS: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto interpolation_result = to_air_interpolation(Inst.m_InputPSDecl.InterpolationMode);
    if (!interpolation_result) {
      sm50_shader->initialization_error = "Invalid DXBC pixel shader input interpolation mode";
      return;
    }
    auto interpolation = *interpolation_result;
    if (interpolation == Interpolation::sample_perspective ||
        interpolation == Interpolation::sample_no_perspective)
      sm50_shader->ps_sample_frequency = 1;
    auto sig = findInputElement([=](Signature sig) {
      return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
    });
    max_input_register = std::max(reg + 1, max_input_register);
    if (sig.mask() == 0) break;
    signature_handlers.push_back([=, type = sig.componentType(), name = sig.varyingAttributeName()]
    (SignatureContext &ctx) {
      bool pull_mode = bool(ctx.pull_mode_reg_mask & (1 << reg)) && interpolation != air::Interpolation::flat;
      auto assigned_index = ctx.func_signature.DefineInput(InputFragmentStageIn{
        .user = name, .type = to_msl_type(type), .interpolation = interpolation, .pull_mode = pull_mode
      });
      if (pull_mode) {
        assert(type == RegisterComponentType::Float && "otherwise the input register contains mixed data type");
        ctx.resource.interpolant_map[reg].push_back(interpolant_descriptor{
          [=](auto) {
            return make_irvalue([=](struct context ctx) {
              return ctx.function->getArg(assigned_index);
            });
          },
          (interpolation == air::Interpolation::sample_perspective ||
           interpolation == air::Interpolation::center_perspective ||
           interpolation == air::Interpolation::centroid_perspective),
          static_cast<uint8_t>(mask)
        });
        uint32_t sampleidx_at = ~0u;
        switch (interpolation) {
        case Interpolation::sample_perspective:
        case Interpolation::sample_no_perspective:
          sampleidx_at = ctx.func_signature.DefineInput(InputSampleIndex{});
          break;
        default:
          break;
        }
        ctx.prologue << init_input_reg_with_interpolation_semantic(assigned_index, reg, mask, interpolation, sampleidx_at);
      } else {
        ctx.prologue << init_input_reg_semantic(assigned_index, reg, mask);
      }
    });
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
    assert(0 && "dcl_output_sgv should not happen for now");
    // only GS PrimitiveID uses this, but we don't support GS for now
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    // auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    default:
      assert(0 && "Unexpected/unhandled input system value");
      break;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH:
    case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
    case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([](struct context ctx) -> std::monostate {
          assert(
            ctx.resource.depth_output_reg == nullptr &&
            "otherwise oDepth is defined twice"
          );
          ctx.resource.depth_output_reg =
            ctx.builder.CreateAlloca(ctx.types._float);
          return {};
        });
      });
      signature_handlers.push_back([=](SignatureContext &ctx) {
        if (ctx.disable_depth_output)
          return;
        auto assigned_index = ctx.func_signature.DefineOutput(OutputDepth{
          .depth_argument =
            RegType == D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL
              ? DepthArgument::greater
            : RegType == D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL
              ? DepthArgument::less
              : DepthArgument::any
        });
        ctx.epilogue >> [=](pvalue v) {
          return make_irvalue([=](struct context ctx) {
            return ctx.builder.CreateInsertValue(
              v,
              ctx.builder.CreateLoad(
                ctx.types._float,
                ctx.builder.CreateConstInBoundsGEP1_32(
                  ctx.types._float, ctx.resource.depth_output_reg, 0
                )
              ),
              {assigned_index}
            );
          });
        };
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_OUTPUT_STENCIL_REF: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([](struct context ctx) -> std::monostate {
          assert(ctx.resource.stencil_ref_reg == nullptr && "otherwise oStencil is defined twice");
          ctx.resource.stencil_ref_reg = ctx.builder.CreateAlloca(ctx.types._int);
          return {};
        });
      });
      auto assigned_index = func_signature.DefineOutput(OutputStencilRef{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.epilogue >> [=](pvalue v) {
          return make_irvalue([=](struct context ctx) {
            auto ostencil = ctx.builder.CreateLoad(
                ctx.types._int, ctx.builder.CreateConstInBoundsGEP1_32(ctx.types._int, ctx.resource.stencil_ref_reg, 0)
            );
            return ctx.builder.CreateInsertValue(v, ostencil, {assigned_index});
          });
        };
      });
      break;
    }
    case D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK: {
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([](struct context ctx) -> std::monostate {
          assert(
            ctx.resource.coverage_mask_reg == nullptr &&
            "otherwise oMask is defined twice"
          );
          ctx.resource.coverage_mask_reg =
            ctx.builder.CreateAlloca(ctx.types._int);
          return {};
        });
      });
      auto assigned_index = func_signature.DefineOutput(OutputCoverageMask{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.epilogue >> [=](pvalue v) {
          return make_irvalue([=](struct context ctx) {
            auto odepth = ctx.builder.CreateLoad(
              ctx.types._int,
              ctx.builder.CreateConstInBoundsGEP1_32(
                ctx.types._int, ctx.resource.coverage_mask_reg, 0
              )
            );
            return ctx.builder.CreateInsertValue(
              v,
              ctx.pso_sample_mask != 0xffffffff
                ? ctx.builder.CreateAnd(odepth, pixel_sample_mask(ctx))
                : odepth,
              {assigned_index}
            );
          });
        };
      });
      sm50_shader->ps_has_coverage_output = 1;
      break;
    }

    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      pso_valid_output_reg_mask |= (1 << reg);
      if (sig.mask() == 0) break;
      auto sig_type = sig.componentType();
      signature_handlers.push_back([=](SignatureContext &ctx) {
        auto type = component_type_from_pixel_format(ctx.pixel_formats[reg]);
        if (ctx.dual_source_blending || type == RegisterComponentType::Unknown)
          type = sig_type;
        uint32_t assigned_index;
        if (ctx.dual_source_blending) {
          if (reg > 1 || reg < 0)
            return;
          assigned_index = ctx.func_signature.DefineOutput(OutputRenderTarget{
            .dual_source_blending = true,
            .index = reg,
            .type = to_msl_type(type),
          });
        } else {
          assigned_index = ctx.func_signature.DefineOutput(OutputRenderTarget{
            .dual_source_blending = false,
            .index = reg,
            .type = to_msl_type(type),
          });
        }
        if (type == RegisterComponentType::Float && ctx.unorm_output_reg_mask & (1 << reg))
          ctx.epilogue >> pop_output_reg_fix_unorm(reg, mask, assigned_index);
        else
          ctx.epilogue >> pop_output_reg(reg, mask, assigned_index);
      });
      break;
    }
    default:
      assert(0 && "unhandled pixel shader output");
      break;
    }
    break;
  }
  default:
    assert(0 && "unhandled pixel shader output");
    break;
  }
};

void handle_signature_hs(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  uint32_t &max_patch_constant_output_register =
    sm50_shader->max_patch_constant_output_register;

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    // auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    // auto MinPrecision = Inst.m_Operands[0].m_MinPrecision; // not used
    auto sgv = Inst.m_InputDeclSGV.Name;
    switch (sgv) {
    default:
      sm50_shader->initialization_error = "Unsupported DXBC hull shader system-value input";
      return;
    }
    max_input_register = std::max(reg + 1, max_input_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT_ID:
    case D3D11_SB_OPERAND_TYPE_INPUT_FORK_INSTANCE_ID:
    case D3D11_SB_OPERAND_TYPE_INPUT_JOIN_INSTANCE_ID:
    case D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID: {
      break;
    }
    case D3D10_SB_OPERAND_TYPE_INPUT: {
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT: {
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_CONTROL_POINT: {
      break;
    }
    case D3D11_SB_OPERAND_TYPE_OUTPUT_CONTROL_POINT: {
      sm50_shader->shader_info.output_control_point_read = true;
      break;
    }

    default:
      sm50_shader->initialization_error = "Unsupported DXBC hull shader input declaration";
      return;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
    sm50_shader->initialization_error = "Unsupported DXBC hull shader system-value output";
    return;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i),
               .reg = (uint8_t)reg,
               .tess_factor_index = (int8_t)(siv - D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR)}
          );
        }
      }
      break;
    }
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i),
               .reg = (uint8_t)reg,
               .tess_factor_index = (int8_t)(siv - D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR)}
          );
        }
      }
      break;
    }
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR: {
      for (unsigned i = 0; i < 4; i++) {
        if (mask & (1 << i)) {
          sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i),
               .reg = (uint8_t)reg,
               .tess_factor_index = (int8_t)(siv - D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR)}
          );
        }
      }
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC hull shader system-value output";
      return;
    }
    if (phase != ~0u) {
      max_patch_constant_output_register =
        std::max(reg + 1, max_patch_constant_output_register);
    } else {
      max_output_register = std::max(reg + 1, max_output_register);
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      if (phase != ~0u) {
        if (Inst.m_Operands[0].m_IndexDimension != D3D10_SB_OPERAND_INDEX_1D) {
          sm50_shader->initialization_error = "Invalid DXBC hull shader patch-constant output";
          return;
        }
        for (unsigned i = 0; i < 4; i++) {
          if (mask & (1 << i)) {
            sm50_shader->patch_constant_scalars.push_back(
              {.component = (uint8_t)(i), .reg = (uint8_t)reg, .tess_factor_index = -1}
            );
          }
        }
        max_patch_constant_output_register =
          std::max(reg + 1, max_patch_constant_output_register);
      } else {
        max_output_register = std::max(reg + 1, max_output_register);
      }
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC hull shader output declaration";
      return;
    }
    break;
  }
  default:
    sm50_shader->initialization_error = "Unsupported DXBC hull shader signature declaration";
    return;
  }
};

void handle_signature_ds(
  CSignatureParser &inputParser, const CSignatureParser &outputParser,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  uint32_t &max_input_register = sm50_shader->max_input_register;
  uint32_t &max_output_register = sm50_shader->max_output_register;
  auto &func_signature = sm50_shader->func_signature;
  auto &mesh_output_handlers = sm50_shader->mesh_output_handlers;
  uint32_t &num_mesh_vertex_data = sm50_shader->num_mesh_vertex_data;

  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR:
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR: {
      max_input_register = std::max(reg + 1, max_input_register);
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC domain shader system-value input";
      return;
    }

    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    sm50_shader->initialization_error = "Unsupported DXBC domain shader system-value input";
    return;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_INPUT_DOMAIN_POINT: {
      // should be defined in convert_dxbc_tessellator_domain_shader()
      break;
    }
    case D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID: {
      // `convert_dxbc_domain_shader()` will always define [[patch_id]] and initialize `io_binding_map.patch_id` field
      break;
    }
    case D3D10_SB_OPERAND_TYPE_INPUT:
      sm50_shader->initialization_error = "Unsupported DXBC domain shader input declaration";
      return;
    case D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT: {
      unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      if (Inst.m_Operands[0].m_IndexDimension != D3D10_SB_OPERAND_INDEX_1D) {
        sm50_shader->initialization_error = "Invalid DXBC domain shader patch-constant input";
        return;
      }
      max_input_register = std::max(reg + 1, max_input_register);
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_CONTROL_POINT:
      break;
    default:
      sm50_shader->initialization_error = "Unsupported DXBC domain shader input declaration";
      return;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
    sm50_shader->initialization_error = "Unsupported DXBC domain shader system-value output";
    return;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    switch (siv) {
    case D3D10_SB_NAME_CLIP_DISTANCE:
    case D3D10_SB_NAME_CULL_DISTANCE: {
      const bool clip = siv == D3D10_SB_NAME_CLIP_DISTANCE;
      for (unsigned i = 0; i < 4; ++i)
        if (mask & (1 << i))
          (clip ? sm50_shader->clip_distance_scalars : sm50_shader->cull_distance_scalars)
              .push_back({.component = (uint8_t)i, .reg = (uint8_t)reg});
      const auto sig = findOutputElement([=](const Signature &element) {
        return element.reg() == reg && (element.mask() & mask) != 0;
      });
      const auto user = varyingAttributeName(clip ? "SV_ClipDistance" : "SV_CullDistance", sig.semanticIndex());
      const auto index = num_mesh_vertex_data++;
      func_signature.DefineMeshVertexOutput(OutputMeshData{.user = user, .type = msl_float4, .index = index});
      mesh_output_handlers.push_back([=](MeshOutputContext &output) -> IREffect {
        return pop_mesh_output_vertex_data(reg, mask, index, output.vertex_id, msl_float4, true);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      break;
    }
    case D3D10_SB_NAME_POSITION: {
      func_signature.DefineMeshVertexOutput(OutputPosition{.type = msl_float4});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_position(reg, mask, output.vertex_id);
      });
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      func_signature.DefineMeshPrimitiveOutput(OutputRenderTargetArrayIndex{});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_render_target_array_index(reg, mask, output.primitive_id);
      });
      break;
    }
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: {
      func_signature.DefineMeshPrimitiveOutput(OutputViewportArrayIndex{});
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_viewport_array_index(reg, mask, output.primitive_id);
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC domain shader system-value output";
      return;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0);
      });
      max_output_register = std::max(reg + 1, max_output_register);
      if (sig.mask() == 0) break;
      auto const mesh_vertex_data_index = num_mesh_vertex_data++;
      auto const type = to_msl_type(sig.componentType());
      func_signature.DefineMeshVertexOutput(OutputMeshData{
        .user = sig.varyingAttributeName(),
        .type = type,
        .index = mesh_vertex_data_index
      });
      mesh_output_handlers.push_back([=](MeshOutputContext& output) -> IREffect {
        return pop_mesh_output_vertex_data(reg, mask, mesh_vertex_data_index, output.vertex_id, type);
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC domain shader output declaration";
      return;
    }
    break;
  }
  default:
    sm50_shader->initialization_error = "Unsupported DXBC domain shader signature declaration";
    return;
  }
};

void handle_signature_cs(
  CSignatureParser &inputParser_unused, const CSignatureParser &outputParser_unused,
  D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *sm50_shader,
  uint32_t phase_unused
) {
  auto &signature_handlers = sm50_shader->signature_handlers;
  auto &func_signature = sm50_shader->func_signature;

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV:
    sm50_shader->initialization_error = "Unsupported DXBC compute shader system-value input";
    return;
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID: {
      auto assigned_index =
        func_signature.DefineInput(InputThreadPositionInGrid{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_id_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID: {
      auto assigned_index =
        func_signature.DefineInput(InputThreadgroupPositionInGrid{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_group_id_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP: {
      auto assigned_index =
        func_signature.DefineInput(InputThreadPositionInThreadgroup{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_id_in_group_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED: {
      auto assigned_index =
        func_signature.DefineInput(InputThreadIndexInThreadgroup{});
      signature_handlers.push_back([=](SignatureContext &ctx) {
        ctx.prologue << make_effect([=](struct context ctx) {
          auto attr = ctx.function->getArg(assigned_index);
          ctx.resource.thread_id_in_group_flat_arg = attr;
          return std::monostate{};
        });
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC compute shader input declaration";
      return;
    }
    break;
  }
  default:
    sm50_shader->initialization_error = "Unsupported DXBC compute shader signature declaration";
    return;
  }
};

void
handle_signature_gs(
    CSignatureParser &inputParser, const CSignatureParser &outputParser, D3D10ShaderBinary::CInstruction &Inst,
    SM50ShaderInternal *sm50_shader, uint32_t phase
) {
  uint32_t &max_output_register = sm50_shader->max_output_register;

  auto findOutputElement = [&](auto matcher) -> Signature {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    outputParser.GetParameters(&parameters);
    for (unsigned i = 0; i < outputParser.GetNumParameters(); i++) {
      auto sig = Signature(parameters[i]);
      if (matcher(sig)) {
        return sig;
      }
    }
    return Signature();
  };
  // See the output declaration cases below.
  auto claim_output = [&](uint32_t reg, uint32_t mask, D3D10_SB_NAME system_value) {
    const D3D11_SIGNATURE_PARAMETER *parameters;
    const UINT count = outputParser.GetParameters(&parameters);
    uint32_t stream_mask = 0;
    for (UINT i = 0; i < count; i++)
      if (parameters[i].Register == reg && parameters[i].SystemValue == system_value)
        stream_mask |= parameters[i].Mask & 0xf;
    if (reg >= 32 || !mask)
      return false;
    auto &declared = sm50_shader->gs_declared_components[sm50_shader->gs_declaring_stream][reg];
    if ((mask & ~stream_mask) || (declared & mask))
      return false;
    declared |= mask;
    return true;
  };

  switch (Inst.m_OpCode) {
  case D3D10_SB_OPCODE_DCL_INPUT_SGV: {
    // auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    // auto MinPrecision = Inst.m_Operands[0].m_MinPrecision; // not used
    auto sgv = Inst.m_InputDeclSGV.Name;
    switch (sgv) {
    default:
      sm50_shader->initialization_error = "Unsupported DXBC geometry shader system-value input";
      return;
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT_SIV: {
    D3D10_SB_NAME siv = Inst.m_InputDeclSIV.Name;

    switch (siv) {
    case D3D10_SB_NAME_POSITION:
    case D3D10_SB_NAME_CULL_DISTANCE:
    case D3D10_SB_NAME_CLIP_DISTANCE:
      break;
    default:
      sm50_shader->initialization_error = "Unsupported DXBC geometry shader system-value input";
      return;
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_INPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;

    switch (RegType) {
    case microsoft::D3D10_SB_OPERAND_TYPE_INPUT: {
      if (Inst.m_Operands[0].m_IndexDimension != microsoft::D3D10_SB_OPERAND_INDEX_2D) {
        sm50_shader->initialization_error = "Invalid DXBC geometry shader input declaration";
        return;
      }
      break;
    }
    case microsoft::D3D10_SB_OPERAND_TYPE_INPUT_PRIMITIVEID:
    case microsoft::D3D11_SB_OPERAND_TYPE_INPUT_GS_INSTANCE_ID:
      break;
    default:
      sm50_shader->initialization_error = "Unsupported DXBC geometry shader input declaration";
      return;
      break;
    }
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT_SGV: {
    sm50_shader->initialization_error = "Unsupported DXBC geometry shader system-value output";
    return;
    break;
  }
  /* Output declarations: every stream writes the same output register file, so the register count is shared.
   * The mesh outputs a declaration defines are recorded for each stream whose signature has the declared components
   * of that register and has not declared them yet, and are defined by the GS converter for the stream that drives
   * the mesh output. */
  case D3D10_SB_OPCODE_DCL_OUTPUT_SIV: {
    unsigned reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
    auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
    auto siv = Inst.m_OutputDeclSIV.Name;
    auto &declarations = sm50_shader->gs_stream_declarations[sm50_shader->gs_declaring_stream];
    max_output_register = std::max(reg + 1, max_output_register);
    if (!claim_output(reg, mask, siv))
      break;
    switch (siv) {
    case D3D10_SB_NAME_CLIP_DISTANCE:
    case D3D10_SB_NAME_CULL_DISTANCE: {
      const bool clip = siv == D3D10_SB_NAME_CLIP_DISTANCE;
      auto sig = findOutputElement([=](const Signature &element) {
        return element.reg() == reg && (element.mask() & mask) != 0;
      });
      const auto user = varyingAttributeName(clip ? "SV_ClipDistance" : "SV_CullDistance", sig.semanticIndex());
      declarations.push_back([=](GSStreamOutputs &out) {
        for (unsigned i = 0; i < 4; i++)
          if (mask & (1 << i))
            (clip ? out.clip_distance_scalars : out.cull_distance_scalars)
                .push_back({.component = (uint8_t)(i), .reg = (uint8_t)reg});
        const auto index = out.num_mesh_vertex_data++;
        out.func_signature.DefineMeshVertexOutput(OutputMeshData{.user = user, .type = msl_float4, .index = index});
        out.mesh_output_handlers.push_back([=](MeshOutputContext &output) -> IREffect {
          return pop_mesh_output_vertex_data(reg, mask, index, output.vertex_id, msl_float4, true);
        });
      });
      break;
    }
    case D3D10_SB_NAME_POSITION: {
      declarations.push_back([=](GSStreamOutputs &out) {
        out.func_signature.DefineMeshVertexOutput(OutputPosition{.type = msl_float4});
        out.mesh_output_handlers.push_back([=](MeshOutputContext &output) -> IREffect {
          return pop_mesh_output_position(reg, mask, output.vertex_id);
        });
      });
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      declarations.push_back([=](GSStreamOutputs &out) {
        out.func_signature.DefineMeshPrimitiveOutput(OutputRenderTargetArrayIndex{});
        out.mesh_output_handlers.push_back([=](MeshOutputContext &output) -> IREffect {
          return pop_mesh_output_render_target_array_index(reg, mask, output.primitive_id);
        });
      });
      break;
    }
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: {
      declarations.push_back([=](GSStreamOutputs &out) {
        out.func_signature.DefineMeshPrimitiveOutput(OutputViewportArrayIndex{});
        out.mesh_output_handlers.push_back([=](MeshOutputContext &output) -> IREffect {
          return pop_mesh_output_viewport_array_index(reg, mask, output.primitive_id);
        });
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC geometry shader system-value output";
      return;
      break;
    }
    max_output_register = std::max(reg + 1, max_output_register);
    break;
  }
  case D3D10_SB_OPCODE_DCL_OUTPUT: {
    D3D10_SB_OPERAND_TYPE RegType = Inst.m_Operands[0].m_Type;
    switch (RegType) {
    case D3D10_SB_OPERAND_TYPE_OUTPUT: {
      // normal output register
      auto reg = Inst.m_Operands[0].m_Index[0].m_RegIndex;
      auto mask = Inst.m_Operands[0].m_WriteMask >> 4;
      auto sig = findOutputElement([=](Signature sig) {
        return (sig.reg() == reg) && ((sig.mask() & mask) != 0) && !sig.isSystemValue();
      });
      max_output_register = std::max(reg + 1, max_output_register);
      if (sig.mask() == 0 || !claim_output(reg, mask, D3D10_SB_NAME_UNDEFINED)) break;
      auto const type = to_msl_type(sig.componentType());
      auto const user = sig.varyingAttributeName();
      sm50_shader->gs_stream_declarations[sm50_shader->gs_declaring_stream].push_back([=](GSStreamOutputs &out) {
        auto const mesh_vertex_data_index = out.num_mesh_vertex_data++;
        out.func_signature.DefineMeshVertexOutput(OutputMeshData{
          .user = user,
          .type = type,
          .index = mesh_vertex_data_index
        });
        out.mesh_output_handlers.push_back([=](MeshOutputContext &output) -> IREffect {
          return pop_mesh_output_vertex_data(reg, mask, mesh_vertex_data_index, output.vertex_id, type);
        });
      });
      break;
    }
    default:
      sm50_shader->initialization_error = "Unsupported DXBC geometry shader output declaration";
      return;
      break;
    }
    break;
  }
  default:
    sm50_shader->initialization_error = "Unsupported DXBC geometry shader signature declaration";
    return;
  }
};

void handle_signature(
  microsoft::CSignatureParser &inputParser,
  microsoft::CSignatureParser5 &outputParser,
  microsoft::D3D10ShaderBinary::CInstruction &Inst, SM50ShaderInternal *shader,
  uint32_t phase
) {
  switch (shader->shader_type) {
  case D3D10_SB_PIXEL_SHADER:
    return handle_signature_ps(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D10_SB_VERTEX_SHADER:
    return handle_signature_vs(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D11_SB_HULL_SHADER:
    return handle_signature_hs(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D11_SB_DOMAIN_SHADER:
    return handle_signature_ds(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D11_SB_COMPUTE_SHADER:
    return handle_signature_cs(inputParser, *outputParser.Signature(0), Inst, shader, phase);
  case D3D10_SB_GEOMETRY_SHADER: {
    /* A declaration belongs to every stream whose output signature has the declared register (vkd3d emits all
     * dcl_stream before the output declarations, so declaration order does not identify the stream). Inputs and
     * other declarations are handled once. */
    const bool output = Inst.m_OpCode == D3D10_SB_OPCODE_DCL_OUTPUT || Inst.m_OpCode == D3D10_SB_OPCODE_DCL_OUTPUT_SIV ||
                        Inst.m_OpCode == D3D10_SB_OPCODE_DCL_OUTPUT_SGV;
    const UINT streams = output ? std::max(outputParser.NumStreams(), 1u) : 1;
    for (UINT s = 0; s < streams && shader->initialization_error.empty(); s++) {
      shader->gs_declaring_stream = s;
      handle_signature_gs(inputParser, *outputParser.Signature(s), Inst, shader, phase);
    }
    shader->gs_declaring_stream = 0;
    return;
  }
  default:
    assert(0);
    break;
  }
}

}; // namespace dxmt::dxbc