/*
 * Shader Model 6.x hull and domain stages of the tessellation mesh pipeline.
 *
 * convert_dxbc_vertex_hull_shader / convert_dxbc_tesselator_domain_shader own the Metal
 * object/mesh ABI; these bindings only supply the stage state (PrepareHullShader /
 * PrepareDomainShader at SM50Initialize) and the stage bodies (BindHullStage /
 * BindDomainStage). The register files the bodies address are the converters' own:
 *   HS: input  = [input CP][VS output register] of this patch (threadgroup),
 *       output = [output CP][HS output register] of this patch (payload, or threadgroup when
 *                the patch-constant function reads output control points),
 *       patch_constant_output = [patch-constant register] of this patch (threadgroup);
 *   DS: input  = [output CP][HS output register] of this patch (payload),
 *       patch_constant_output = [patch-constant register], output = [DS output register].
 */

#include "dxil_pipeline.hpp"

#include <algorithm>

#include "llvm/IR/Instructions.h"

#include "air_signature.hpp"
#include "dxbc_converter.hpp"
#include "nt/air_builder.hpp"

namespace dxmt::dxil {

namespace {

/* D3D_NAME of the patch-constant system values, as the PCSG chunk stores them. */
constexpr uint8_t NAME_FINAL_QUAD_EDGE_TESSFACTOR = 11;
constexpr uint8_t NAME_FINAL_QUAD_INSIDE_TESSFACTOR = 12;
constexpr uint8_t NAME_FINAL_TRI_EDGE_TESSFACTOR = 13;
constexpr uint8_t NAME_FINAL_TRI_INSIDE_TESSFACTOR = 14;
constexpr uint8_t NAME_FINAL_LINE_DETAIL_TESSFACTOR = 15;
constexpr uint8_t NAME_FINAL_LINE_DENSITY_TESSFACTOR = 16;

/* DXIL::SemanticKind of the domain shader outputs. */
constexpr uint8_t KIND_ARBITRARY = 0;
constexpr uint8_t KIND_POSITION = 3;
constexpr uint8_t KIND_RENDER_TARGET_ARRAY_INDEX = 4;
constexpr uint8_t KIND_VIEWPORT_ARRAY_INDEX = 5;
constexpr uint8_t KIND_CLIP_DISTANCE = 6;
constexpr uint8_t KIND_CULL_DISTANCE = 7;

/* DXIL::ComponentType */
constexpr uint8_t COMPONENT_I16 = 2;
constexpr uint8_t COMPONENT_I32 = 4;
constexpr uint8_t COMPONENT_F16 = 8;
constexpr uint8_t COMPONENT_F32 = 9;

/* Register rows spanned by signature chunk elements (the chunks list one entry per row). */
uint32_t register_count(const std::vector<SigElement> &elements) {
  uint32_t count = 0;
  for (auto &element : elements)
    if (element.start_row != ~0u)
      count = std::max(count, element.start_row + element.rows);
  return count;
}

uint32_t register_mask(const std::vector<SigElement> &elements) {
  uint32_t mask = 0;
  for (auto &element : elements)
    if (element.start_row != ~0u)
      for (uint32_t r = 0; r < element.rows && element.start_row + r < 32; r++)
        mask |= 1u << (element.start_row + r);
  return mask;
}

dxbc::RegisterComponentType register_type(uint8_t component_type) {
  switch (component_type) {
  case COMPONENT_F16:
  case COMPONENT_F32:
    return dxbc::RegisterComponentType::Float;
  case COMPONENT_I16:
  case COMPONENT_I32:
    return dxbc::RegisterComponentType::Int;
  default:
    return dxbc::RegisterComponentType::Uint;
  }
}

/* Slot of a tessellation factor inside the converter's six-entry factor array. */
int8_t tess_factor_index(const SigElement &element) {
  switch (element.semantic_kind) {
  case NAME_FINAL_QUAD_EDGE_TESSFACTOR:
  case NAME_FINAL_TRI_EDGE_TESSFACTOR:
    return (int8_t)element.semantic_index;
  case NAME_FINAL_QUAD_INSIDE_TESSFACTOR:
    return (int8_t)(4 + element.semantic_index);
  case NAME_FINAL_TRI_INSIDE_TESSFACTOR:
    return 3;
  case NAME_FINAL_LINE_DETAIL_TESSFACTOR:
    return 0;
  case NAME_FINAL_LINE_DENSITY_TESSFACTOR:
    return 1;
  default:
    return -1;
  }
}

/* `start_row + row` of a dx.op I/O call whose row operand is `row_operand`. */
llvm::Value *element_row(llvm::IRBuilder<> &builder, const IOElement &element, llvm::Value *row_operand) {
  return builder.CreateAdd(
    builder.getInt32(element.start_row), builder.CreateZExtOrTrunc(row_operand, builder.getInt32Ty())
  );
}

/*
 * Replace every `dx.op.<name>` read of an element in `elements` with a load of its lane in
 * `registers`. `vertex_operand` is the operand holding the per-vertex (control point) index,
 * or 0 when the register file is not per vertex.
 */
llvm::Error lower_register_loads(
  dxbc::context &ctx, llvm::StringRef name, const std::vector<IOElement> &elements,
  const dxbc::register_file &registers, unsigned vertex_operand, llvm::StringRef stage
) {
  auto &builder = ctx.builder;
  for (auto *call : CollectCalls(*ctx.function, name)) {
    auto id = ConstantOperand(*call, 1);
    if (!id)
      return id.takeError();
    auto col = ConstantOperand(*call, 3);
    if (!col)
      return col.takeError();
    const IOElement *element = FindElement(elements, *id);
    if (!element || element->start_row < 0)
      return createError(
        "DXIL " + stage + " shader dx.op." + name + " reads element " + llvm::Twine(*id) + " that has no register"
      );
    if (!registers.ptr_int4)
      return createError("DXIL " + stage + " shader dx.op." + name + " has no register file in this stage");
    builder.SetInsertPoint(call);
    const bool as_float = call->getType()->isFloatingPointTy();
    llvm::SmallVector<llvm::Value *, 1> prefix;
    if (vertex_operand)
      prefix.push_back(builder.CreateZExtOrTrunc(call->getArgOperand(vertex_operand), builder.getInt32Ty()));
    auto *lane_ptr = RegisterLane(
      builder, registers, as_float, prefix, element_row(builder, *element, call->getArgOperand(2)),
      element->start_col + *col
    );
    auto *lane = builder.CreateLoad(as_float ? builder.getFloatTy() : builder.getInt32Ty(), lane_ptr);
    call->replaceAllUsesWith(NarrowLane(builder, lane, call->getType()));
    call->eraseFromParent();
  }
  return llvm::Error::success();
}

/* Replace every `dx.op.<name>` write (value at operand 4) with a store to its lane in `registers`. */
llvm::Error lower_register_stores(
  dxbc::context &ctx, llvm::StringRef name, const std::vector<IOElement> &elements,
  const dxbc::register_file &registers, llvm::Value *vertex, llvm::StringRef stage
) {
  auto &builder = ctx.builder;
  for (auto *call : CollectCalls(*ctx.function, name)) {
    auto id = ConstantOperand(*call, 1);
    if (!id)
      return id.takeError();
    auto col = ConstantOperand(*call, 3);
    if (!col)
      return col.takeError();
    const IOElement *element = FindElement(elements, *id);
    if (!element || element->start_row < 0)
      return createError(
        "DXIL " + stage + " shader dx.op." + name + " writes element " + llvm::Twine(*id) + " that has no register"
      );
    if (!registers.ptr_int4)
      return createError("DXIL " + stage + " shader dx.op." + name + " has no register file in this stage");
    builder.SetInsertPoint(call);
    auto *value = WidenLane(builder, call->getArgOperand(4), element->component_type == COMPONENT_I16);
    const bool as_float = value->getType()->isFloatTy();
    llvm::SmallVector<llvm::Value *, 1> prefix;
    if (vertex)
      prefix.push_back(vertex);
    builder.CreateStore(
      value, RegisterLane(
               builder, registers, as_float, prefix, element_row(builder, *element, call->getArgOperand(2)),
               element->start_col + *col
             )
    );
    call->eraseFromParent();
  }
  return llvm::Error::success();
}

/* Replace every `dx.op.<name>` call with `value` (a system value the converter provides). */
llvm::Error lower_system_value(dxbc::context &ctx, llvm::StringRef name, llvm::Value *value, llvm::StringRef stage) {
  auto &builder = ctx.builder;
  for (auto *call : CollectCalls(*ctx.function, name)) {
    if (!value)
      return createError("DXIL " + stage + " shader dx.op." + name + " is not available in this stage");
    builder.SetInsertPoint(call);
    call->replaceAllUsesWith(builder.CreateZExtOrTrunc(value, call->getType()));
    call->eraseFromParent();
  }
  return llvm::Error::success();
}

/* dx.op I/O of the hull shader phases, once both are transplanted into the object function. */
llvm::Error lower_hull_body(PipelineStage &stage, dxbc::context &ctx) {
  auto &signature = stage.signature;
  auto &resource = ctx.resource;
  if (auto error = lower_register_loads(ctx, "loadInput", signature.inputs, resource.input, 4, "hull"))
    return error;
  if (auto error = lower_register_loads(
        ctx, "loadOutputControlPoint", signature.outputs, resource.output, 4, "hull"
      ))
    return error;
  if (auto error = lower_register_stores(
        ctx, "storeOutput", signature.outputs, resource.output, resource.thread_id_in_patch, "hull"
      ))
    return error;
  if (auto error = lower_register_stores(
        ctx, "storePatchConstant", signature.patch_constants, resource.patch_constant_output, nullptr, "hull"
      ))
    return error;
  if (auto error = lower_system_value(ctx, "outputControlPointID", resource.thread_id_in_patch, "hull"))
    return error;
  return lower_system_value(ctx, "primitiveID", resource.patch_id, "hull");
}

/* dx.op I/O of the domain shader body. */
llvm::Error lower_domain_body(PipelineStage &stage, dxbc::context &ctx) {
  auto &signature = stage.signature;
  auto &resource = ctx.resource;
  auto &builder = ctx.builder;
  for (auto *call : CollectCalls(*ctx.function, "domainLocation")) {
    auto component = ConstantOperand(*call, 1);
    if (!component)
      return component.takeError();
    if (*component > 2 || !resource.domain)
      return createError("DXIL domain shader dx.op.domainLocation component is out of range");
    builder.SetInsertPoint(call);
    call->replaceAllUsesWith(NarrowLane(builder, builder.CreateExtractElement(resource.domain, *component), call->getType()));
    call->eraseFromParent();
  }
  if (auto error = lower_register_loads(ctx, "loadInput", signature.inputs, resource.input, 4, "domain"))
    return error;
  if (auto error = lower_register_loads(
        ctx, "loadPatchConstant", signature.patch_constants, resource.patch_constant_output, 0, "domain"
      ))
    return error;
  if (auto error = lower_register_stores(ctx, "storeOutput", signature.outputs, resource.output, nullptr, "domain"))
    return error;
  return lower_system_value(ctx, "primitiveID", resource.patch_id, "domain");
}

} // namespace

/* -------------------------------------------------------------------------- */
/* SM50Initialize                                                             */
/* -------------------------------------------------------------------------- */

llvm::Error PrepareHullShader(const ShaderMetadata &metadata, dxbc::SM50ShaderInternal &shader) {
  const auto &state = metadata.hull;
  if (state.input_control_points < 1 || state.input_control_points > 32 || state.output_control_points > 32)
    return createError(
      "DXIL hull shader control point counts " + std::to_string(state.input_control_points) + "/" +
      std::to_string(state.output_control_points) + " are out of range"
    );
  switch (state.domain) {
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_TRI:
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
    break;
  default:
    return createError("DXIL hull shader has no tessellator domain");
  }
  switch (state.partitioning) {
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_INTEGER:
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_POW2:
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD:
  case microsoft::D3D11_SB_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN:
    break;
  default:
    return createError("DXIL hull shader has no tessellator partitioning");
  }
  switch (state.output_primitive) {
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_POINT:
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_LINE:
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CW:
  case microsoft::D3D11_SB_TESSELLATOR_OUTPUT_TRIANGLE_CCW:
    break;
  default:
    return createError("DXIL hull shader has no tessellator output primitive");
  }

  shader.input_control_point_count = state.input_control_points;
  shader.output_control_point_count = state.output_control_points;
  shader.hull_maximum_threads_per_patch = std::max(state.input_control_points, state.output_control_points);
  shader.tessellation_domain = (microsoft::D3D11_SB_TESSELLATOR_DOMAIN)state.domain;
  shader.tessellation_partition = (microsoft::D3D11_SB_TESSELLATOR_PARTITIONING)state.partitioning;
  shader.tessellator_output_primitive = (microsoft::D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE)state.output_primitive;
  shader.max_tesselation_factor = std::clamp(state.max_tess_factor, 1.0f, 64.0f);
  /* A DXIL hull shader always has a control point function. */
  shader.shader_info.no_control_point_phase_passthrough = true;

  shader.max_input_register = register_count(metadata.input_sig);
  shader.max_output_register = register_count(metadata.output_sig);
  shader.pso_valid_output_reg_mask = register_mask(metadata.output_sig);
  shader.max_patch_constant_output_register = register_count(metadata.patch_constant_sig);

  /* One scalar per written (register, component) of the patch-constant signature; the
   * chunk lists each row of an array element (SV_TessFactor[i]) as its own entry. */
  shader.patch_constant_scalars.clear();
  for (auto &element : metadata.patch_constant_sig) {
    if (element.start_row == ~0u)
      continue;
    for (uint32_t c = 0; c < 4; c++)
      if (element.mask & (1u << c))
        shader.patch_constant_scalars.push_back(
          {.component = (uint8_t)c, .reg = (uint8_t)element.start_row, .tess_factor_index = tess_factor_index(element)}
        );
  }
  return llvm::Error::success();
}

llvm::Error PrepareDomainShader(const ShaderMetadata &metadata, dxbc::SM50ShaderInternal &shader) {
  const auto &state = metadata.domain;
  switch (state.domain) {
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_ISOLINE:
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_TRI:
  case microsoft::D3D11_SB_TESSELLATOR_DOMAIN_QUAD:
    break;
  default:
    return createError("DXIL domain shader has no tessellator domain");
  }
  if (state.input_control_points > 32)
    return createError("DXIL domain shader input control point count is out of range");
  shader.tessellation_domain = (microsoft::D3D11_SB_TESSELLATOR_DOMAIN)state.domain;
  shader.input_control_point_count = state.input_control_points;
  /* For the domain stage the input registers are the patch constants (control points are
   * read from the hull stage's payload). */
  shader.max_input_register = register_count(metadata.patch_constant_sig);
  shader.max_output_register = register_count(metadata.output_sig);
  shader.pso_valid_output_reg_mask = register_mask(metadata.output_sig);
  shader.clip_distance_scalars.clear();
  shader.cull_distance_scalars.clear();
  for (auto &element : metadata.output_sig) {
    if (element.start_row == ~0u)
      continue;
    const bool clip = element.semantic_kind == microsoft::D3D10_SB_NAME_CLIP_DISTANCE;
    const bool cull = element.semantic_kind == microsoft::D3D10_SB_NAME_CULL_DISTANCE;
    if (!clip && !cull)
      continue;
    for (uint32_t c = 0; c < 4; c++)
      if (element.mask & (1u << c))
        (clip ? shader.clip_distance_scalars : shader.cull_distance_scalars)
          .push_back({.component = (uint8_t)c, .reg = (uint8_t)element.start_row});
  }
  return llvm::Error::success();
}

/* -------------------------------------------------------------------------- */
/* Hull stage (VS+HS object function)                                         */
/* -------------------------------------------------------------------------- */

llvm::Error BindHullStage(PipelineStage &stage) {
  auto &info = *stage.info;
  if (!info.patch_constant_function || info.patch_constant_function->isDeclaration())
    return createError("DXIL hull shader has no patch constant function");
  auto &view = stage.view;
  view.signature_handlers.clear();
  view.shader_info.no_control_point_phase_passthrough = true;
  /* The control points the patch-constant phase reads live in threadgroup memory and are
   * copied to the payload afterwards (the DXBC BasicBlockHullShaderWriteOutput step). */
  view.shader_info.output_control_point_read =
    !CollectCalls(*info.patch_constant_function, "loadOutputControlPoint").empty();

  view.body_emitter = [&stage](dxbc::context &ctx, llvm::BasicBlock *return_bb)
    -> llvm::Expected<llvm::BasicBlock *> {
    auto &builder = ctx.builder;
    auto &llvm_context = ctx.llvm;
    auto *function = ctx.function;
    auto insert_point = builder.saveIP();
    if (auto error = LinkStageModule(stage, ctx.module))
      return std::move(error);
    if (!stage.patch_constant)
      return createError("DXIL hull shader patch constant function was lost while linking");

    const uint32_t output_points = stage.view.output_control_point_count;
    auto *thread = ctx.resource.thread_id_in_patch;
    auto *entry = llvm::BasicBlock::Create(llvm_context, "hull_control_point_phase", function);
    auto *control_point_end = llvm::BasicBlock::Create(llvm_context, "hull_control_point_end", function);
    auto *patch_constant_end = llvm::BasicBlock::Create(llvm_context, "hull_patch_constant_end", function);

    /* 1. Control point phase: one thread per output control point. */
    auto control_point_body = TransplantEntryBody(*stage.entry, *function, *control_point_end);
    if (!control_point_body)
      return control_point_body.takeError();
    builder.SetInsertPoint(entry);
    builder.CreateCondBr(
      builder.CreateICmpULT(thread, builder.getInt32(output_points)), *control_point_body, control_point_end
    );

    /* 2. Patch constant phase: thread 0 of the patch. */
    auto patch_constant_body = TransplantEntryBody(*stage.patch_constant, *function, *patch_constant_end);
    if (!patch_constant_body)
      return patch_constant_body.takeError();
    builder.SetInsertPoint(control_point_end);
    ctx.air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    builder.CreateCondBr(
      builder.CreateICmpEQ(thread, builder.getInt32(0)), *patch_constant_body, patch_constant_end
    );

    builder.SetInsertPoint(patch_constant_end);
    ctx.air.CreateBarrier(llvm::air::MemFlags::Threadgroup);

    /* 3. Control points read by the patch-constant phase: threadgroup copy -> payload. */
    if (stage.view.shader_info.output_control_point_read) {
      auto *write = llvm::BasicBlock::Create(llvm_context, "write_control_point", function);
      auto *write_end = llvm::BasicBlock::Create(llvm_context, "write_control_point_end", function);
      builder.CreateCondBr(builder.CreateICmpULT(thread, builder.getInt32(output_points)), write, write_end);
      builder.SetInsertPoint(write);
      auto *type = ctx.resource.hull_cp_passthrough_type;
      auto *source = ctx.resource.hull_cp_passthrough_src;
      if (source->getType()->getNonOpaquePointerElementType() != type)
        source = builder.CreateBitCast(source, type->getPointerTo(source->getType()->getPointerAddressSpace()));
      builder.CreateStore(builder.CreateLoad(type, source), ctx.resource.hull_cp_passthrough_dst);
      builder.CreateBr(write_end);
      builder.SetInsertPoint(write_end);
      ctx.air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    }
    builder.CreateBr(return_bb);

    if (auto error = lower_hull_body(stage, ctx))
      return std::move(error);
    if (auto error = LowerStageResources(stage, ctx))
      return std::move(error);
    builder.restoreIP(insert_point);
    return entry;
  };
  return llvm::Error::success();
}

/* -------------------------------------------------------------------------- */
/* Domain stage (mesh function)                                               */
/* -------------------------------------------------------------------------- */

llvm::Error BindDomainStage(PipelineStage &stage) {
  auto &view = stage.view;
  view.signature_handlers.clear();
  view.mesh_output_handlers.clear();
  view.num_mesh_vertex_data = 0;

  /* Mesh vertex/primitive outputs, as handle_signature_ds declares them. In the TS+GS
   * capture mode the converter drops these and captures the output register file instead. */
  for (auto &element : stage.signature.outputs) {
    if (element.start_row < 0)
      return unsupported("DXIL domain shader output " + element.name + " has no register");
    const uint32_t mask = (((1u << element.cols) - 1u) << std::max(element.start_col, 0)) & 0xfu;
    for (uint32_t r = 0; r < element.rows; r++) {
      const uint32_t reg = element.start_row + r;
      const uint32_t semantic_index = r < element.semantic_indices.size() ? element.semantic_indices[r] : r;
      auto define_varying = [&](air::MSLScalerOrVectorType type) {
        const uint32_t index = view.num_mesh_vertex_data++;
        view.func_signature.DefineMeshVertexOutput(air::OutputMeshData{
          .user = dxbc::varyingAttributeName(element.name, semantic_index),
          .type = type,
          .index = index,
        });
        view.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
          return dxbc::pop_mesh_output_vertex_data(reg, mask, index, output.vertex_id, type);
        });
      };
      switch (element.semantic_kind) {
      case KIND_POSITION:
        view.func_signature.DefineMeshVertexOutput(air::OutputPosition{.type = air::msl_float4});
        view.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
          return dxbc::pop_mesh_output_position(reg, mask, output.vertex_id);
        });
        break;
      case KIND_RENDER_TARGET_ARRAY_INDEX:
        view.func_signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
        view.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
          return dxbc::pop_mesh_output_render_target_array_index(reg, mask, output.primitive_id);
        });
        break;
      case KIND_VIEWPORT_ARRAY_INDEX:
        view.func_signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
        view.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
          return dxbc::pop_mesh_output_viewport_array_index(reg, mask, output.primitive_id);
        });
        break;
      case KIND_CLIP_DISTANCE:
      case KIND_CULL_DISTANCE:
        /* clip_distance_scalars (PrepareDomainShader) feed [[clip_distance]]; the varying
         * lets a pixel shader that reads the distance link, as on the DXIL vertex path. */
        define_varying(air::msl_float4);
        break;
      case KIND_ARBITRARY:
        define_varying(dxbc::to_msl_type(register_type(element.component_type)));
        break;
      default:
        return unsupported("DXIL domain shader system-value output " + element.name);
      }
    }
  }

  view.body_emitter = [&stage](dxbc::context &ctx, llvm::BasicBlock *return_bb)
    -> llvm::Expected<llvm::BasicBlock *> {
    auto insert_point = ctx.builder.saveIP();
    if (auto error = LinkStageModule(stage, ctx.module))
      return std::move(error);
    auto body = TransplantEntryBody(*stage.entry, *ctx.function, *return_bb);
    if (!body)
      return body.takeError();
    if (auto error = lower_domain_body(stage, ctx))
      return std::move(error);
    if (auto error = LowerStageResources(stage, ctx))
      return std::move(error);
    ctx.builder.restoreIP(insert_point);
    return *body;
  };
  return llvm::Error::success();
}

} // namespace dxmt::dxil
