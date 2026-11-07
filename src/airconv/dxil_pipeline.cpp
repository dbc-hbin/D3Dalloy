/*
 * Shader Model 6.x stages of the multi-stage mesh pipelines.
 *
 * The four SM50CompileGeometryPipeline* / SM50CompileTessellationPipeline* entry points
 * build the Metal object/mesh functions through the DXBC converters in
 * dxbc_converter_gs.cpp / dxbc_converter_ts.cpp. Those converters own the ABI (payload
 * layout, dispatch shape, statistics, stream output capture), so a DXIL stage reuses them
 * unchanged: CompilePipelineStage hands each converter a per-compile SM50ShaderInternal
 * view of the DXIL stage whose `body_emitter` transplants the lowered DXIL entry body into
 * the converter's function, in place of the DXBC control flow.
 *
 * The first DXIL stage whose body is emitted provides the module the converter builds
 * into (its IR is lowered in place, exactly like LowerShader); a second one (the hull
 * shader of a VS+HS object function) is parsed into the same context and linked in.
 */

#include "dxil_pipeline.hpp"

#include <algorithm>

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/TypeFinder.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/raw_ostream.h"

#include "air_signature.hpp"
#include "airconv_context.hpp"
#include "dxbc_converter.hpp"
#include "msaa_mesh.hpp"

namespace d3dalloy::dxil {

namespace {

/* DXIL::SemanticKind */
constexpr uint8_t KIND_ARBITRARY = 0;
constexpr uint8_t KIND_POSITION = 3;
constexpr uint8_t KIND_RENDER_TARGET_ARRAY_INDEX = 4;
constexpr uint8_t KIND_VIEWPORT_ARRAY_INDEX = 5;
constexpr uint8_t KIND_CLIP_DISTANCE = 6;
constexpr uint8_t KIND_CULL_DISTANCE = 7;
constexpr uint8_t KIND_SHADING_RATE = 29;

/* DXIL::ComponentType */
constexpr uint8_t COMPONENT_I16 = 2;
constexpr uint8_t COMPONENT_I32 = 4;
constexpr uint8_t COMPONENT_F16 = 8;
constexpr uint8_t COMPONENT_F32 = 9;

/* dx.op opcodes of the geometry stage. */
constexpr uint32_t OP_EMIT_STREAM = 97;
constexpr uint32_t OP_CUT_STREAM = 98;
constexpr uint32_t OP_EMIT_THEN_CUT_STREAM = 99;

llvm::MDNode *node_at(const llvm::MDNode *node, unsigned index) {
  if (!node || index >= node->getNumOperands())
    return nullptr;
  return llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(index));
}

llvm::ConstantInt *int_at(const llvm::MDNode *node, unsigned index) {
  if (!node || index >= node->getNumOperands())
    return nullptr;
  auto *constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(node->getOperand(index));
  return constant ? llvm::dyn_cast<llvm::ConstantInt>(constant->getValue()) : nullptr;
}

uint32_t u32_at(const llvm::MDNode *node, unsigned index, uint32_t fallback = 0) {
  auto *value = int_at(node, index);
  return value ? (uint32_t)value->getZExtValue() : fallback;
}

int32_t s32_at(const llvm::MDNode *node, unsigned index, int32_t fallback = 0) {
  auto *value = int_at(node, index);
  return value ? (int32_t)value->getSExtValue() : fallback;
}

std::vector<IOElement> read_elements(const llvm::MDNode *list) {
  std::vector<IOElement> elements;
  if (!list)
    return elements;
  for (unsigned i = 0; i < list->getNumOperands(); i++) {
    const llvm::MDNode *node = node_at(list, i);
    if (!node)
      continue;
    IOElement element;
    element.id = u32_at(node, 0);
    if (auto *name = llvm::dyn_cast_or_null<llvm::MDString>(node->getNumOperands() > 1 ? node->getOperand(1).get() : nullptr))
      element.name = name->getString().str();
    element.component_type = (uint8_t)u32_at(node, 2);
    element.semantic_kind = (uint8_t)u32_at(node, 3);
    if (const llvm::MDNode *indices = node_at(node, 4))
      for (unsigned k = 0; k < indices->getNumOperands(); k++)
        element.semantic_indices.push_back(u32_at(indices, k));
    element.interpolation = (uint8_t)u32_at(node, 5);
    element.rows = std::max(1u, u32_at(node, 6, 1));
    element.cols = std::max(1u, u32_at(node, 7, 1));
    element.start_row = s32_at(node, 8, -1);
    element.start_col = s32_at(node, 9, -1);
    /* Extended properties are (tag, value) pairs; tag 0 is the GS output stream. */
    if (const llvm::MDNode *properties = node_at(node, 10))
      for (unsigned k = 0; k + 1 < properties->getNumOperands(); k += 2)
        if (u32_at(properties, k, ~0u) == 0)
          element.stream = u32_at(properties, k + 1);
    elements.push_back(std::move(element));
  }
  std::stable_sort(elements.begin(), elements.end(), [](const IOElement &a, const IOElement &b) {
    return a.id < b.id;
  });
  return elements;
}

uint32_t register_mask(const IOElement &element) {
  return (((1u << element.cols) - 1u) << std::max(element.start_col, 0)) & 0xfu;
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

/* Register rows spanned by the given signature chunk elements (one entry per row). */
uint32_t register_count(const std::vector<SigElement> &elements) {
  uint32_t count = 0;
  for (auto &element : elements) {
    if (element.start_row == ~0u)
      continue;
    count = std::max(count, element.start_row + element.rows);
  }
  return count;
}

/* The reflection state a pipeline converter reads, copied from the cached shader. */
void copy_state(dxbc::SM50ShaderInternal &view, const dxbc::SM50ShaderInternal &source) {
  view.shader_info = source.shader_info;
  view.func_signature = source.func_signature;
  view.output_signature = source.output_signature;
  view.shader_type = source.shader_type;
  view.max_input_register = source.max_input_register;
  view.max_output_register = source.max_output_register;
  view.pso_valid_output_reg_mask = source.pso_valid_output_reg_mask;
  view.max_patch_constant_output_register = source.max_patch_constant_output_register;
  std::copy(std::begin(source.threadgroup_size), std::end(source.threadgroup_size), std::begin(view.threadgroup_size));
  view.input_control_point_count = source.input_control_point_count;
  view.output_control_point_count = source.output_control_point_count;
  view.tessellation_partition = source.tessellation_partition;
  view.max_tesselation_factor = source.max_tesselation_factor;
  view.tessellator_output_primitive = source.tessellator_output_primitive;
  view.tessellation_domain = source.tessellation_domain;
  view.patch_constant_scalars = source.patch_constant_scalars;
  view.hull_maximum_threads_per_patch = source.hull_maximum_threads_per_patch;
  view.clip_distance_scalars = source.clip_distance_scalars;
  view.cull_distance_scalars = source.cull_distance_scalars;
  view.gs_input_primitive = source.gs_input_primitive;
  view.gs_output_topology = source.gs_output_topology;
  view.gs_max_vertex_output = source.gs_max_vertex_output;
  view.gs_instance_count = source.gs_instance_count;
  view.ps_has_coverage_output = source.ps_has_coverage_output;
  view.ps_sample_frequency = source.ps_sample_frequency;
}

/*
 * A second DXIL module parsed into a context that already holds the first one gets its
 * `dx.types.*` struct types renamed with a numeric suffix, and the linker keeps that name
 * when the destination no longer uses the original. The resource and operation lowering
 * recognize these types by name, so the used ones get their canonical names back.
 */
void canonicalize_dx_type_names(llvm::Module &module) {
  llvm::TypeFinder types;
  types.run(module, true);
  for (llvm::StructType *type : types) {
    if (!type->hasName())
      continue;
    llvm::StringRef name = type->getName();
    if (!name.startswith("dx.types."))
      continue;
    auto dot = name.rfind('.');
    llvm::StringRef suffix = name.substr(dot + 1);
    if (dot <= strlen("dx.types.") || suffix.empty() ||
        !std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; }))
      continue;
    std::string canonical = name.substr(0, dot).str();
    if (auto *existing = llvm::StructType::getTypeByName(module.getContext(), canonical)) {
      if (std::find(types.begin(), types.end(), existing) != types.end())
        continue; /* both are in use: the lowering sees the canonical one. */
      existing->setName(canonical + ".stale");
    }
    type->setName(canonical);
  }
}

} // namespace

/* -------------------------------------------------------------------------- */
/* Shared helpers                                                             */
/* -------------------------------------------------------------------------- */

bool IsDXILShader(const void *shader) {
  return shader && static_cast<const dxbc::SM50ShaderInternal *>(shader)->dxil_bytecode.has_value();
}

llvm::Expected<EntrySignature> ReadEntrySignature(const ModuleInfo &info) {
  llvm::NamedMDNode *entries = info.module ? info.module->getNamedMetadata("dx.entryPoints") : nullptr;
  if (!entries)
    return createError("DXIL module has no entry point metadata");
  for (unsigned i = 0; i < entries->getNumOperands(); i++) {
    llvm::MDNode *entry = entries->getOperand(i);
    auto *value = entry && entry->getNumOperands() ? llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(entry->getOperand(0).get()) : nullptr;
    if (!value || value->getValue() != info.entry)
      continue;
    EntrySignature signature;
    llvm::MDNode *lists = node_at(entry, 2);
    signature.inputs = read_elements(node_at(lists, 0));
    signature.outputs = read_elements(node_at(lists, 1));
    signature.patch_constants = read_elements(node_at(lists, 2));
    return signature;
  }
  return createError("DXIL entry point metadata does not name the entry function");
}

const IOElement *FindElement(const std::vector<IOElement> &elements, uint32_t id) {
  auto it = std::lower_bound(elements.begin(), elements.end(), id, [](const IOElement &element, uint32_t id) {
    return element.id < id;
  });
  return it != elements.end() && it->id == id ? &*it : nullptr;
}

std::vector<dxbc::Signature> BuildOutputSignature(const std::vector<SigElement> &elements) {
  std::vector<dxbc::Signature> signature;
  signature.reserve(elements.size());
  for (auto &element : elements) {
    if (element.start_row == ~0u)
      continue;
    microsoft::D3D11_SIGNATURE_PARAMETER parameter{};
    parameter.Stream = element.stream;
    parameter.SemanticName = const_cast<char *>(element.name.c_str());
    parameter.SemanticIndex = element.semantic_index;
    parameter.SystemValue = (microsoft::D3D10_SB_NAME)element.semantic_kind;
    /* 16 bit component types (4..6) travel in 32 bit lanes of the matching type. */
    uint8_t component = element.comp_type;
    if (component >= 4 && component <= 6)
      component -= 3;
    parameter.ComponentType = (microsoft::D3D10_SB_REGISTER_COMPONENT_TYPE)component;
    parameter.Register = element.start_row;
    parameter.Mask = (uint8_t)element.mask;
    signature.emplace_back(parameter);
  }
  return signature;
}

llvm::Value *NarrowLane(llvm::IRBuilder<> &builder, llvm::Value *lane, llvm::Type *type) {
  if (lane->getType() == type)
    return lane;
  if (type->isHalfTy())
    return builder.CreateFPTrunc(
      lane->getType()->isFloatTy() ? lane : builder.CreateBitCast(lane, builder.getFloatTy()), type
    );
  if (type->isIntegerTy() && type->getIntegerBitWidth() < 32)
    return builder.CreateTrunc(
      lane->getType()->isIntegerTy() ? lane : builder.CreateBitCast(lane, builder.getInt32Ty()), type
    );
  return builder.CreateBitCast(lane, type);
}

llvm::Value *WidenLane(llvm::IRBuilder<> &builder, llvm::Value *value, bool is_signed) {
  if (value->getType()->isHalfTy())
    return builder.CreateFPExt(value, builder.getFloatTy());
  if (value->getType()->isIntegerTy() && value->getType()->getIntegerBitWidth() < 32)
    return is_signed && !value->getType()->isIntegerTy(1) ? builder.CreateSExt(value, builder.getInt32Ty())
                                                           : builder.CreateZExt(value, builder.getInt32Ty());
  return value;
}

llvm::Value *RegisterLane(
  llvm::IRBuilder<> &builder, const dxbc::register_file &registers, bool as_float,
  llvm::ArrayRef<llvm::Value *> prefix, llvm::Value *row, uint32_t col
) {
  llvm::Value *base = as_float ? registers.ptr_float4 : registers.ptr_int4;
  auto *type = llvm::cast<llvm::PointerType>(base->getType())->getNonOpaquePointerElementType();
  llvm::SmallVector<llvm::Value *, 5> indices{builder.getInt32(0)};
  indices.append(prefix.begin(), prefix.end());
  indices.push_back(row);
  indices.push_back(builder.getInt32(col));
  return builder.CreateGEP(type, base, indices);
}

llvm::Expected<uint32_t> ConstantOperand(const llvm::CallInst &call, unsigned index) {
  if (index < call.arg_size())
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(index)))
      return (uint32_t)constant->getZExtValue();
  return createError(
    "dx.op instruction " + (call.getCalledFunction() ? call.getCalledFunction()->getName() : llvm::StringRef("?")) +
    " has a non constant operand " + llvm::Twine(index)
  );
}

llvm::StringMap<llvm::SmallVector<llvm::CallInst *, 16>>
CollectCalls(llvm::Function &function, llvm::ArrayRef<llvm::StringRef> names) {
  llvm::StringMap<llvm::SmallVector<llvm::CallInst *, 16>> calls;
  for (auto name : names)
    calls[name];
  for (auto &block : function)
    for (auto &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (auto *callee = call->getCalledFunction()) {
          auto name = callee->getName();
          if (!name.consume_front("dx.op."))
            continue;
          auto found = calls.find(name.split('.').first);
          if (found != calls.end())
            found->second.push_back(call);
        }
  return calls;
}

llvm::Error LinkStageModule(PipelineStage &stage, llvm::Module &target) {
  if (stage.entry)
    return llvm::Error::success();
  auto &info = *stage.info;
  if (!info.entry)
    return createError("DXIL pipeline stage has no entry point");
  info.entry->setName("d3dalloy.dxil." + stage.tag);
  if (info.patch_constant_function)
    info.patch_constant_function->setName("d3dalloy.dxil." + stage.tag + ".pc");
  std::string entry_name = info.entry->getName().str();
  std::string patch_constant_name = info.patch_constant_function ? info.patch_constant_function->getName().str() : "";

  if (info.module.get() != &target) {
    if (!info.module)
      return createError("DXIL pipeline stage module was already linked");
    llvm::Module &source = *info.module;
    initializeModule(source);
    /* Stage-unique global names; resource declarations are re-resolved after the link. */
    llvm::SmallVector<std::pair<ResourceBinding *, std::string>, 8> symbols;
    for (auto &global : source.globals()) {
      if (!global.hasName() || global.getName().startswith("llvm."))
        continue;
      global.setName("d3dalloy." + stage.tag + "." + global.getName());
    }
    for (auto &resource : info.resources)
      if (auto *global = llvm::dyn_cast_or_null<llvm::GlobalValue>(resource.symbol ? resource.symbol->stripPointerCasts() : nullptr))
        symbols.emplace_back(&resource, global->getName().str());
    for (auto &function : source)
      if (!function.isDeclaration() && function.getName() != entry_name && function.getName() != patch_constant_name)
        function.setLinkage(llvm::GlobalValue::InternalLinkage);
    if (llvm::Linker::linkModules(target, std::move(info.module)))
      return createError("failed to link DXIL " + stage.tag + " stage into the pipeline module");
    canonicalize_dx_type_names(target);
    for (auto &[resource, name] : symbols)
      resource->symbol = target.getNamedValue(name);
    for (auto &resource : info.resources)
      if (resource.symbol && !llvm::isa<llvm::Constant>(resource.symbol))
        resource.symbol = nullptr;
  } else if (target.getDataLayout().getPointerSizeInBits() != 64) {
    initializeModule(target);
  }

  stage.entry = target.getFunction(entry_name);
  stage.patch_constant = patch_constant_name.empty() ? nullptr : target.getFunction(patch_constant_name);
  info.entry = stage.entry;
  info.patch_constant_function = stage.patch_constant;
  if (!stage.entry)
    return createError("DXIL " + stage.tag + " entry point was lost while linking");
  PrepareModuleStorage(target);
  return llvm::Error::success();
}

llvm::Error LowerStageResources(PipelineStage &stage, dxbc::context &ctx) {
  auto &info = *stage.info;
  if (info.module && info.module.get() != &ctx.module)
    return createError("DXIL " + stage.tag + " stage is lowered before it is linked");
  /* SV_ViewID of GS/HS/DS bodies (the VS body lowers it with its stage I/O). */
  if (auto error = LowerViewInstanceId(ctx))
    return error;
  /* ModuleInfo owns its module; a linked stage lowers the pipeline module without owning it. */
  const bool borrowed = !info.module;
  if (borrowed)
    info.module.reset(&ctx.module);
  auto error = LowerResources(info, ctx.binding);
  if (borrowed)
    (void)info.module.release();
  return error;
}

/* -------------------------------------------------------------------------- */
/* Vertex stage inside an object function                                    */
/* -------------------------------------------------------------------------- */

llvm::Error BindVertexStage(PipelineStage &stage) {
  auto &info = *stage.info;
  stage.view.max_input_register = register_count(info.input_sig);
  stage.view.signature_handlers.clear();
  stage.view.signature_handlers.push_back([&stage](dxbc::SignatureContext &sig_ctx) {
    /* Registers and distances were settled at SM50Initialize; the declaration only has to
     * define the AIR inputs and the prologue on the converter's signature. */
    dxbc::SM50ShaderInternal scratch;
    scratch.shader_type = stage.view.shader_type;
    if (auto error = DeclareStageSignature(*stage.info, scratch, sig_ctx, stage.vertex_signature))
      stage.error = llvm::toString(std::move(error));
  });
  stage.view.body_emitter = [&stage](dxbc::context &ctx, llvm::BasicBlock *return_bb)
    -> llvm::Expected<llvm::BasicBlock *> {
    if (!stage.error.empty())
      return createError(stage.error);
    auto insert_point = ctx.builder.saveIP();
    if (auto error = LinkStageModule(stage, ctx.module))
      return std::move(error);
    auto body = TransplantEntryBody(*stage.entry, *ctx.function, *return_bb);
    if (!body)
      return body.takeError();
    if (auto error = LowerStageIO(ctx, stage.vertex_signature))
      return std::move(error);
    if (auto error = LowerStageResources(stage, ctx))
      return std::move(error);
    ctx.builder.restoreIP(insert_point);
    return *body;
  };
  return llvm::Error::success();
}

/* -------------------------------------------------------------------------- */
/* Geometry stage                                                             */
/* -------------------------------------------------------------------------- */

llvm::Error PrepareGeometryShader(const ShaderMetadata &metadata, dxbc::SM50ShaderInternal &shader) {
  const auto &state = metadata.geometry;
  switch (state.input_primitive) {
  case microsoft::D3D10_SB_PRIMITIVE_POINT:
  case microsoft::D3D10_SB_PRIMITIVE_LINE:
  case microsoft::D3D10_SB_PRIMITIVE_TRIANGLE:
  case microsoft::D3D10_SB_PRIMITIVE_LINE_ADJ:
  case microsoft::D3D10_SB_PRIMITIVE_TRIANGLE_ADJ:
    break;
  default:
    return unsupported("DXIL geometry shader input primitive " + std::to_string(state.input_primitive));
  }
  switch (state.output_topology) {
  case microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST:
  case microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_LINESTRIP:
  case microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    break;
  default:
    return unsupported("DXIL geometry shader output topology " + std::to_string(state.output_topology));
  }
  if (state.stream_mask & ~0xfu)
    return createError("DXIL geometry shader has an invalid stream mask");
  /* D3D11.3 13.5.2: a GS writing more than one stream outputs point lists only. */
  if ((state.stream_mask & (state.stream_mask - 1)) &&
      state.output_topology != microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST)
    return createError("DXIL geometry shader writes multiple streams with non-point output");
  if (state.max_vertex_count == 0 || state.instance_count == 0 || state.instance_count > 32)
    return createError("DXIL geometry shader has an invalid maxvertexcount or instance count");
  shader.gs_input_primitive = (microsoft::D3D10_SB_PRIMITIVE)state.input_primitive;
  shader.gs_output_topology = (microsoft::D3D10_SB_PRIMITIVE_TOPOLOGY)state.output_topology;
  shader.gs_max_vertex_output = state.max_vertex_count;
  shader.gs_instance_count = state.instance_count;
  shader.max_input_register = register_count(metadata.input_sig);
  /* Every stream writes the one output register file. */
  shader.max_output_register = register_count(metadata.output_sig);
  return llvm::Error::success();
}

namespace {

/* dx.op.loadInput (4) / storeOutput (5) / emit / cut / GSInstanceID / PrimitiveID of a GS body. */
llvm::Error lower_geometry_body(PipelineStage &stage, dxbc::context &ctx) {
  auto &builder = ctx.builder;
  auto &function = *ctx.function;
  auto &signature = stage.signature;
  auto calls = CollectCalls(function, {
    "loadInput", "storeOutput", "emitStream", "cutStream", "emitThenCutStream", "gsInstanceID", "primitiveID"
  });

  for (auto *call : calls["loadInput"]) {
    auto id = ConstantOperand(*call, 1);
    auto col = ConstantOperand(*call, 3);
    if (!id) return id.takeError();
    if (!col) return col.takeError();
    const IOElement *element = FindElement(signature.inputs, *id);
    if (!element || element->start_row < 0)
      return createError("DXIL geometry shader loads input element " + llvm::Twine(*id) + " that has no register");
    builder.SetInsertPoint(call);
    const bool as_float = call->getType()->isFloatingPointTy();
    auto *row = builder.CreateAdd(builder.getInt32(element->start_row),
                                  builder.CreateZExtOrTrunc(call->getArgOperand(2), builder.getInt32Ty()));
    auto *vertex = builder.CreateZExtOrTrunc(call->getArgOperand(4), builder.getInt32Ty());
    auto *lane_ptr = RegisterLane(builder, ctx.resource.input, as_float, {vertex}, row, element->start_col + *col);
    auto *lane = builder.CreateLoad(as_float ? builder.getFloatTy() : builder.getInt32Ty(), lane_ptr);
    call->replaceAllUsesWith(NarrowLane(builder, lane, call->getType()));
    call->eraseFromParent();
  }

  for (auto *call : calls["storeOutput"]) {
    auto id = ConstantOperand(*call, 1);
    auto col = ConstantOperand(*call, 3);
    if (!id) return id.takeError();
    if (!col) return col.takeError();
    const IOElement *element = FindElement(signature.outputs, *id);
    if (!element || element->start_row < 0)
      return createError("DXIL geometry shader stores output element " + llvm::Twine(*id) + " that has no register");
    builder.SetInsertPoint(call);
    auto *value = WidenLane(builder, call->getArgOperand(4), element->component_type == COMPONENT_I16);
    const bool as_float = value->getType()->isFloatTy();
    auto *row = builder.CreateAdd(builder.getInt32(element->start_row),
                                  builder.CreateZExtOrTrunc(call->getArgOperand(2), builder.getInt32Ty()));
    builder.CreateStore(value, RegisterLane(builder, ctx.resource.output, as_float, {}, row, element->start_col + *col));
    call->eraseFromParent();
  }

  for (llvm::StringRef name : {"emitStream", "cutStream", "emitThenCutStream"}) {
    for (auto *call : calls[name]) {
      auto opcode = ConstantOperand(*call, 0);
      auto stream = ConstantOperand(*call, 1);
      if (!opcode) return opcode.takeError();
      if (!stream) return stream.takeError();
      if (*stream >= 4)
        return createError("DXIL geometry shader emits to stream " + std::to_string(*stream));
      /* The converter's emit/cut may build control flow of its own (stream output capture), so
       * it runs at the end of a block of its own, which then continues with the rest. */
      llvm::BasicBlock *head = call->getParent();
      llvm::BasicBlock *tail = head->splitBasicBlock(call, "gs_emit_continue");
      head->getTerminator()->eraseFromParent();
      builder.SetInsertPoint(head);
      if (*opcode == OP_EMIT_STREAM || *opcode == OP_EMIT_THEN_CUT_STREAM)
        if (auto error = ctx.resource.call_emit(uint32_t(*stream)).build(ctx).takeError())
          return error;
      if (*opcode == OP_CUT_STREAM || *opcode == OP_EMIT_THEN_CUT_STREAM)
        if (auto error = ctx.resource.call_cut(uint32_t(*stream)).build(ctx).takeError())
          return error;
      builder.CreateBr(tail);
      call->eraseFromParent();
    }
  }

  for (auto [name, value] : {std::pair<llvm::StringRef, llvm::Value *>{"gsInstanceID", ctx.resource.gs_instance_id},
                             {"primitiveID", ctx.resource.patch_id}}) {
    for (auto *call : calls[name]) {
      if (!value)
        return createError("dx.op." + name + " is not available in this stage");
      builder.SetInsertPoint(call);
      call->replaceAllUsesWith(builder.CreateZExtOrTrunc(value, call->getType()));
      call->eraseFromParent();
    }
  }
  return llvm::Error::success();
}

} // namespace

llvm::Error BindGeometryStage(PipelineStage &stage) {
  auto &view = stage.view;
  view.signature_handlers.clear();
  view.mesh_output_handlers.clear();
  view.clip_distance_scalars.clear();
  view.cull_distance_scalars.clear();
  view.num_mesh_vertex_data = 0;
  for (auto &declarations : view.gs_stream_declarations)
    declarations.clear();

  /* The mesh outputs of each stream, exactly as handle_signature_gs declares them; the GS converter defines
   * those of the stream that drives the mesh output. */
  for (auto &element : stage.signature.outputs) {
    if (element.stream >= 4)
      return createError("DXIL geometry shader output " + element.name + " has an invalid stream");
    if (element.start_row < 0)
      return unsupported("DXIL geometry shader output " + element.name + " has no register");
    auto &declarations = view.gs_stream_declarations[element.stream];
    for (uint32_t r = 0; r < element.rows; r++) {
      const uint32_t reg = element.start_row + r;
      const uint32_t mask = register_mask(element);
      const uint32_t semantic_index = r < element.semantic_indices.size() ? element.semantic_indices[r] : r;
      const std::string user = dxbc::varyingAttributeName(element.name, semantic_index);
      auto define_varying = [&](air::MSLScalerOrVectorType type) {
        declarations.push_back([=](dxbc::GSStreamOutputs &out) {
          const uint32_t index = out.num_mesh_vertex_data++;
          out.func_signature.DefineMeshVertexOutput(air::OutputMeshData{.user = user, .type = type, .index = index});
          out.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
            return dxbc::pop_mesh_output_vertex_data(reg, mask, index, output.vertex_id, type);
          });
        });
      };
      switch (element.semantic_kind) {
      case KIND_POSITION:
        declarations.push_back([=](dxbc::GSStreamOutputs &out) {
          out.func_signature.DefineMeshVertexOutput(air::OutputPosition{.type = air::msl_float4});
          out.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
            return dxbc::pop_mesh_output_position(reg, mask, output.vertex_id);
          });
        });
        break;
      case KIND_RENDER_TARGET_ARRAY_INDEX:
        declarations.push_back([=](dxbc::GSStreamOutputs &out) {
          out.func_signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
          out.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
            return dxbc::pop_mesh_output_render_target_array_index(reg, mask, output.primitive_id);
          });
        });
        break;
      case KIND_VIEWPORT_ARRAY_INDEX:
        declarations.push_back([=](dxbc::GSStreamOutputs &out) {
          out.func_signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
          out.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
            return dxbc::pop_mesh_output_viewport_array_index(reg, mask, output.primitive_id);
          });
        });
        break;
      case KIND_CLIP_DISTANCE:
      case KIND_CULL_DISTANCE: {
        const bool clip = element.semantic_kind == KIND_CLIP_DISTANCE;
        declarations.push_back([=](dxbc::GSStreamOutputs &out) {
          for (unsigned c = 0; c < 4; c++)
            if (mask & (1u << c))
              (clip ? out.clip_distance_scalars : out.cull_distance_scalars)
                .push_back({.component = (uint8_t)c, .reg = (uint8_t)reg});
        });
        /* Also a varying, so a pixel shader that reads the distance links (the DXIL vertex
         * path exports it the same way). */
        define_varying(air::msl_float4);
        break;
      }
      case KIND_ARBITRARY:
        define_varying(dxbc::to_msl_type(register_type(element.component_type)));
        break;
      case KIND_SHADING_RATE:
        declarations.push_back([=](dxbc::GSStreamOutputs &out) {
          const uint32_t index = out.num_mesh_vertex_data++;
          out.func_signature.DefineMeshVertexOutput(air::OutputMeshData{
            .user = "d3dalloy_shading_rate", .type = air::msl_uint, .index = index
          });
          out.mesh_output_handlers.push_back([=](dxbc::MeshOutputContext &output) -> dxbc::IREffect {
            return dxbc::pop_mesh_output_vertex_data(reg, mask, index, output.vertex_id, air::msl_uint);
          });
        });
        break;
      default:
        return unsupported("DXIL geometry shader system-value output " + element.name);
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
    if (auto error = lower_geometry_body(stage, ctx))
      return std::move(error);
    if (auto error = LowerStageResources(stage, ctx))
      return std::move(error);
    ctx.builder.restoreIP(insert_point);
    return *body;
  };
  return llvm::Error::success();
}

/* -------------------------------------------------------------------------- */
/* Pipeline compile                                                           */
/* -------------------------------------------------------------------------- */

llvm::Expected<std::unique_ptr<SM50CompiledBitcodeInternal>> CompilePipelineStage(
  PipelineKind kind, dxbc::SM50ShaderInternal *first, dxbc::SM50ShaderInternal *second,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *function_name
) {
  if (!first || !second)
    return createError("pipeline stage shader is null");

  llvm::LLVMContext context;
  context.setOpaquePointers(false);

  PipelineStage stages[2];
  stages[0].source = first;
  stages[1].source = second;
  bool emit_body[2] = {};
  switch (kind) {
  case PipelineKind::GeometryObject:
    /* With tessellation the "vertex" stage is the domain shader, whose body runs in the
     * tessellation mesh function (SM50_SHADER_GS_TESSELLATION_INPUT): only its state is read. */
    stages[0].tag = "vs", stages[1].tag = "gs";
    emit_body[0] = first->shader_type == microsoft::D3D10_SB_VERTEX_SHADER;
    break;
  case PipelineKind::GeometryMesh:
    stages[0].tag = first->shader_type == microsoft::D3D10_SB_VERTEX_SHADER ? "vs" : "ds";
    stages[1].tag = "gs", emit_body[1] = true;
    break;
  case PipelineKind::TessellationObject:
    stages[0].tag = "vs", stages[1].tag = "hs", emit_body[0] = emit_body[1] = true;
    break;
  case PipelineKind::TessellationMesh:
    stages[0].tag = "hs", stages[1].tag = "ds", emit_body[1] = true;
    break;
  }

  std::unique_ptr<llvm::Module> fresh_module;
  llvm::Module *module = nullptr;
  dxbc::SM50ShaderInternal *converter_stage[2] = {first, second};
  for (unsigned i = 0; i < 2; i++) {
    auto &stage = stages[i];
    if (!stage.source->dxil_bytecode)
      continue;
    copy_state(stage.view, *stage.source);
    converter_stage[i] = &stage.view;
    if (!emit_body[i])
      continue;
    auto info = ModuleInfo::Parse(
      stage.source->dxil_bytecode->data(), stage.source->dxil_bytecode->size(), nullptr, &context
    );
    if (!info)
      return info.takeError();
    stage.info = std::move(*info);
    auto signature = ReadEntrySignature(*stage.info);
    if (!signature)
      return signature.takeError();
    stage.signature = std::move(*signature);
    if (!module) {
      module = stage.info->module.get();
      if (module->getDataLayout().getPointerSizeInBits() != 64)
        initializeModule(*module);
    }
  }
  if (kind == PipelineKind::TessellationObject && stages[0].info && stages[1].info) {
    // Match whole elements in both directions: an HS load may index its row dynamically.
    // Unconsumed VS outputs and separately packed neighbours do not affect a matched element.
    const auto &outputs = stages[0].info->output_sig;
    const auto &inputs = stages[1].info->input_sig;
    auto same_row = [](const SigElement &a, const SigElement &b) {
      return a.comp_type == 6 && b.comp_type == 6 && !a.semantic_kind && !b.semantic_kind &&
             a.name == b.name && a.semantic_index == b.semantic_index && a.start_row == b.start_row &&
             a.start_col == b.start_col && a.element_row == b.element_row && a.rows == b.rows && a.mask == b.mask;
    };
    auto covers = [&](const auto &from, uint32_t id, const auto &to, uint32_t other_id) {
      return std::all_of(from.begin(), from.end(), [&](const SigElement &row) {
        return row.id != id || std::any_of(to.begin(), to.end(), [&](const SigElement &other) {
          return other.id == other_id && same_row(row, other);
        });
      });
    };
    for (const auto &out : outputs) {
      if (out.element_row || out.comp_type != 6 || out.semantic_kind) continue;
      auto in = std::find_if(inputs.begin(), inputs.end(), [&](const SigElement &row) { return same_row(out,row); });
      if (in == inputs.end() || !covers(outputs, out.id, inputs, in->id) ||
          !covers(inputs, in->id, outputs, out.id)) continue;
      stages[0].vertex_signature.raw_half_outputs.push_back(out.id);
      stages[1].raw_half_inputs.push_back(in->id);
    }
  }
  for (auto &stage : stages) {
    if (!stage.info)
      continue;
    auto bind = [&]() -> llvm::Error {
      switch (stage.info->stage) {
      case Stage::Vertex:
        return BindVertexStage(stage);
      case Stage::Geometry:
        return BindGeometryStage(stage);
      case Stage::Hull:
        return BindHullStage(stage);
      case Stage::Domain:
        return BindDomainStage(stage);
      default:
        return createError("DXIL shader stage does not belong to this pipeline");
      }
    };
    if (auto error = bind())
      return std::move(error);
  }
  if (!module) {
    fresh_module = std::make_unique<llvm::Module>("shader.air", context);
    initializeModule(*fresh_module);
    module = fresh_module.get();
  }
  // Resource lowering in every linked body uses this per-compilation target module.
  InitializeAtomic64Phase(*module, args);
  // Bodies link lazily, after the converter selects its AIR target. Include every
  // emitted stage now without lowering another stage with the wrong binding map.
  SM50_SHADER_COMMON_DATA common{args, SM50_SHADER_COMMON, SM50_SHADER_METAL_310, {}};
  SM50_SHADER_COMMON_DATA *requested_common = nullptr;
  if (dxbc::args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(args, &requested_common))
    common = *requested_common;
  common.next = args;
  for (const auto &stage : stages)
    if (stage.info)
      common.metal_version = dxbc::required_metal_version(*stage.info->module, common.metal_version);
  args = reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&common);
  // Reserve the wrapper name before creating it, but retain stage-local modules
  // until their bodies lower: resources from another stage must not be visited
  // with this stage's binding map.
  for (auto &stage : stages)
    if (stage.info && stage.info->entry)
      stage.info->entry->setName("d3dalloy.dxil." + stage.tag);

  auto convert = [&]() -> llvm::Error {
    switch (kind) {
    case PipelineKind::GeometryObject: {
      /* TS+GS: the object function fetches the tessellation capture instead of running a VS. */
      SM50_SHADER_GS_TESSELLATION_INPUT_DATA *tessellation_input = nullptr;
      auto convert_object =
        dxbc::args_get_data<SM50_SHADER_GS_TESSELLATION_INPUT, SM50_SHADER_GS_TESSELLATION_INPUT_DATA>(
          args, &tessellation_input
        )
          ? dxbc::convert_dxbc_domain_for_geometry_shader
          : dxbc::convert_dxbc_vertex_for_geometry_shader;
      return convert_object(converter_stage[0], function_name, converter_stage[1], context, *module, args);
    }
    case PipelineKind::GeometryMesh:
      return dxbc::convert_dxbc_geometry_shader(
        converter_stage[1], function_name, converter_stage[0], context, *module, args
      );
    case PipelineKind::TessellationObject:
      return dxbc::convert_dxbc_vertex_hull_shader(
        converter_stage[0], converter_stage[1], function_name, context, *module, args
      );
    case PipelineKind::TessellationMesh:
      return dxbc::convert_dxbc_tesselator_domain_shader(
        converter_stage[1], function_name, converter_stage[0], context, *module, args
      );
    }
    return createError("unknown pipeline stage");
  };
  if (auto error = convert()) {
    for (auto &stage : stages)
      if (!stage.error.empty()) {
        llvm::consumeError(std::move(error));
        return createError(stage.error);
      }
    return std::move(error);
  }
  for (auto &stage : stages)
    if (!stage.error.empty())
      return createError(stage.error);

  if (auto *sample_position = module->getFunction("d3dalloy.sample_pos.i32");
      sample_position && sample_position->isDeclaration())
    linkSamplePos(*module);
  if (auto error = LowerOperations(*module))
    return std::move(error);
  if (auto error = VerifyLoweredModule(*module))
    return std::move(error);
  StripDXILArtifacts(*module);

  for (auto *shader : converter_stage) {
    if (shader->shader_info.use_msad)
      linkMSAD(*module);
    if (shader->shader_info.use_samplepos)
      linkSamplePos(*module);
  }
  if (kind == PipelineKind::TessellationObject || kind == PipelineKind::TessellationMesh)
    linkTessellation(*module);
  SM50_SHADER_D3D12_MSAA_MESH_DATA *msaa_mesh = nullptr;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *conservative = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_CONSERVATIVE_RASTER, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(args, &conservative);
  if (dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_MESH, SM50_SHADER_D3D12_MSAA_MESH_DATA>(args, &msaa_mesh))
    if (auto error = msaa::transformMesh(*module, *msaa_mesh, conservative != nullptr))
      return std::move(error);

  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  if (auto error = FinalizeShader(*module, *compiled, true))
    return std::move(error);
  return std::move(compiled);
}

} // namespace d3dalloy::dxil
