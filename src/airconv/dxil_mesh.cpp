/*
 * Shader Model 6.5 mesh and amplification shaders (D3D12 MeshShaderTier 1).
 *
 * A mesh shader becomes an AIR `[[mesh]]` function and an amplification shader an AIR
 * `[[object]]` function; Metal's mesh pipeline matches the D3D model one to one (grid
 * limits, 16 KB payload, the vertex/primitive output budget and the x-fastest launch
 * order were probed natively), so nothing is emulated beyond the output store shape:
 *
 * - `dx.op.storeVertexOutput` / `storePrimitiveOutput` write one component, Metal writes
 *   one whole output field. Every element row whose stores come in runs that write the
 *   same component set (one run per vertex/primitive index, which is what DXC emits for
 *   `verts[i] = v`) turns each run into one `air.set_*_mesh` call: the last run executed
 *   writes every component it covers, exactly like the per-component D3D stores. Any
 *   other store pattern (dynamically indexed rows, partial writes spread over several
 *   places) is staged in threadgroup memory and published after a final barrier.
 * - The amplification payload is groupshared in DXIL (it counts against the 32 KB
 *   amplification groupshared budget there too); `dx.op.dispatchMesh` copies it into the
 *   Metal object payload, sets the mesh grid from the first thread and ends the group.
 * - The mesh payload is the Metal object payload, reached through an address space cast
 *   that `InferAddressSpacesPass(0)` folds (as for immutable globals).
 */

#include "dxil_converter.hpp"
#include "dxil_pipeline.hpp"
#include "dxil_signature.hpp"

#include "air_signature.hpp"
#include "airconv_context.hpp"
#include "airconv_public.h"
#include "nt/air_builder.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <map>

namespace d3dalloy::dxil {

namespace {

/* DXIL::SemanticKind */
constexpr uint8_t KIND_ARBITRARY = 0;
constexpr uint8_t KIND_POSITION = 3;
constexpr uint8_t KIND_RENDER_TARGET_ARRAY_INDEX = 4;
constexpr uint8_t KIND_VIEWPORT_ARRAY_INDEX = 5;
constexpr uint8_t KIND_CLIP_DISTANCE = 6;
constexpr uint8_t KIND_CULL_DISTANCE = 7;
constexpr uint8_t KIND_PRIMITIVE_ID = 10;
constexpr uint8_t KIND_CULL_PRIMITIVE = 30;
constexpr uint8_t KIND_SHADING_RATE = 29;

/* DXIL::ComponentType */
constexpr uint8_t COMPONENT_I16 = 2;
constexpr uint8_t COMPONENT_I32 = 4;
constexpr uint8_t COMPONENT_F16 = 8;
constexpr uint8_t COMPONENT_F32 = 9;

/* DXIL::MeshOutputTopology */
constexpr uint32_t TOPOLOGY_LINE = 1;
constexpr uint32_t TOPOLOGY_TRIANGLE = 2;

/* dx.op opcodes */
constexpr uint32_t OP_SET_MESH_OUTPUT_COUNTS = 168;
constexpr uint32_t OP_EMIT_INDICES = 169;
constexpr uint32_t OP_GET_MESH_PAYLOAD = 170;
constexpr uint32_t OP_STORE_VERTEX_OUTPUT = 171;
constexpr uint32_t OP_STORE_PRIMITIVE_OUTPUT = 172;
constexpr uint32_t OP_DISPATCH_MESH = 173;

/** How one signature row reaches the Metal mesh. */
struct OutputRow {
  enum class Kind { Position, Data, RenderTargetArrayIndex, ViewportArrayIndex, PrimitiveID, Culled } kind;
  /** `air.mesh_vertex_data` / `air.mesh_primitive_data` index (Data, and the varying copy of a clip distance). */
  uint32_t data_index = ~0u;
  /** First `[[clip_distance]]` slot of the row, ~0u unless a clip distance. */
  uint32_t clip_base = ~0u;
  air::MSLScalerOrVectorType type = air::msl_float4;
  bool is_float = true;
  /** Semantic lanes are compacted into the same float4 ABI as ordinary VS outputs. */
  bool packed_user = false;
};

/** One vertex or primitive output element and how its stores are lowered. */
struct OutputElement {
  const IOElement *element = nullptr;
  bool primitive = false;
  std::vector<OutputRow> rows;
  std::vector<llvm::CallInst *> stores;
  /** `[N x [rows x <4 x i32>]]` threadgroup copy when the stores cannot be coalesced. */
  llvm::GlobalVariable *staging = nullptr;
  /** Pipeline statistics: `[words x i32]` threadgroup bit set of the primitives SV_CullPrimitive culls. */
  llvm::GlobalVariable *culled_bits = nullptr;
};

air::MSLScalerOrVectorType data_type(uint8_t component_type, uint32_t cols, bool &is_float) {
  air::MSLScalerType scalar = air::msl_uint;
  is_float = component_type == COMPONENT_F16 || component_type == COMPONENT_F32;
  if (is_float)
    scalar = air::msl_float;
  else if (component_type == COMPONENT_I16 || component_type == COMPONENT_I32)
    scalar = air::msl_int;
  if (cols == 1)
    return std::visit([](auto s) -> air::MSLScalerOrVectorType { return s; }, scalar);
  return air::MSLVector{cols, scalar};
}

/** The stored value as the 32 bit lane the output (or its staging copy) holds. */
llvm::Value *to_lane_bits(llvm::IRBuilder<> &builder, llvm::Value *value, uint8_t component_type) {
  value = WidenLane(builder, value, component_type == COMPONENT_I16);
  if (value->getType()->isFloatTy())
    return builder.CreateBitCast(value, builder.getInt32Ty());
  return builder.CreateZExtOrTrunc(value, builder.getInt32Ty());
}

/** Publish one row of an output element; `lanes` are i32 bits, a null lane was never written. */
void emit_row(
  dxbc::context &ctx, const OutputElement &output, const OutputRow &row, llvm::Value *index,
  llvm::ArrayRef<llvm::Value *> lanes
) {
  auto &b = ctx.builder;
  auto &air = ctx.air;
  auto lane_or_zero = [&](unsigned c) -> llvm::Value * {
    return c < lanes.size() && lanes[c] ? lanes[c] : b.getInt32(0);
  };
  auto as_float = [&](llvm::Value *bits) { return b.CreateBitCast(bits, b.getFloatTy()); };
  switch (row.kind) {
  case OutputRow::Kind::Position: {
    llvm::Value *position = llvm::UndefValue::get(llvm::FixedVectorType::get(b.getFloatTy(), 4));
    for (unsigned c = 0; c < 4; c++)
      position = b.CreateInsertElement(position, as_float(lane_or_zero(c)), c);
    air.CreateSetMeshPosition(index, position);
    return;
  }
  case OutputRow::Kind::RenderTargetArrayIndex:
    air.CreateSetMeshRenderTargetArrayIndex(index, lane_or_zero(0));
    return;
  case OutputRow::Kind::ViewportArrayIndex:
    air.CreateSetMeshViewportArrayIndex(index, lane_or_zero(0));
    return;
  case OutputRow::Kind::PrimitiveID:
    air.CreateSetMeshPrimitiveID(index, lane_or_zero(0));
    return;
  case OutputRow::Kind::Culled: {
    auto *culled = b.CreateICmpNE(lane_or_zero(0), b.getInt32(0));
    air.CreateSetMeshPrimitiveCulled(index, culled);
    if (output.culled_bits) {
      // The last write of the flag wins, as for the Metal output itself.
      auto *word = b.CreateGEP(output.culled_bits->getValueType(), output.culled_bits,
                               {b.getInt32(0), b.CreateLShr(index, b.getInt32(5))});
      auto *bit = b.CreateShl(b.getInt32(1), b.CreateAnd(index, b.getInt32(31)));
      air.CreateAtomicRMW(llvm::AtomicRMWInst::Or, word, b.CreateSelect(culled, bit, b.getInt32(0)));
      air.CreateAtomicRMW(llvm::AtomicRMWInst::And, word, b.CreateSelect(culled, b.getInt32(~0u), b.CreateNot(bit)));
    }
    return;
  }
  case OutputRow::Kind::Data:
    break;
  }
  const uint32_t cols = row.packed_user ? 4 : output.element->cols;
  if (row.clip_base != ~0u)
    for (unsigned c = 0; c < cols; c++)
      if (c < lanes.size() && lanes[c])
        air.CreateSetMeshClipDistance(index, b.getInt32(row.clip_base + c), as_float(lanes[c]));
  llvm::SmallVector<llvm::Value *, 4> values;
  for (unsigned c = 0; c < cols; c++) {
    // AIR semantics are compact, independent of D3D packed register columns.
    // The PS prologue scatters these leading lanes into its own packed mask.
    llvm::Value *lane = lane_or_zero(c);
    if (row.clip_base != ~0u) {
      /* The varying copy of a distance: Metal rejects non-finite interpolants, the VS path
       * publishes the same sanitized value. */
      auto *nonfinite = b.CreateICmpUGE(b.CreateAnd(lane, b.getInt32(0x7fffffffu)), b.getInt32(0x7f800000u));
      lane = b.CreateSelect(nonfinite, b.getInt32(0), lane);
    }
    values.push_back(row.is_float ? as_float(lane) : lane);
  }
  llvm::Value *value = values[0];
  if (cols > 1) {
    value = llvm::UndefValue::get(llvm::FixedVectorType::get(values[0]->getType(), cols));
    for (unsigned c = 0; c < cols; c++)
      value = b.CreateInsertElement(value, values[c], c);
  }
  if (output.primitive)
    air.CreateSetMeshPrimitiveData(index, b.getInt32(row.data_index), value);
  else
    air.CreateSetMeshVertexData(index, b.getInt32(row.data_index), value);
}

/** One run of consecutive stores of an element row for one vertex/primitive index. */
struct StoreRun {
  uint32_t row;
  llvm::Value *index;
  llvm::CallInst *last;
  llvm::Value *lanes[4] = {};
  uint32_t mask = 0;
};

/*
 * Coalesce the stores of `output` into whole-row writes when that is exact, otherwise
 * stage them. Returns whether the element was staged.
 */
llvm::Error lower_output_stores(
  dxbc::context &ctx, OutputElement &output, uint32_t max_count
) {
  auto &b = ctx.builder;
  const IOElement &element = *output.element;
  std::vector<StoreRun> runs;
  bool coalescible = true;
  StoreRun *open = nullptr;
  for (auto *store : output.stores) {
    auto *row = llvm::dyn_cast<llvm::ConstantInt>(store->getArgOperand(2));
    auto col = ConstantOperand(*store, 3);
    if (!col)
      return col.takeError();
    if (*col >= element.cols || *col >= 4)
      return createError("DXIL mesh output store to column " + llvm::Twine(*col) + " of " + element.name);
    if (!row) {
      coalescible = false;
      continue;
    }
    if (row->getZExtValue() >= element.rows)
      return createError("DXIL mesh output store to row " + llvm::Twine(row->getZExtValue()) + " of " + element.name);
    llvm::Value *index = store->getArgOperand(5);
    const uint32_t r = (uint32_t)row->getZExtValue();
    if (!open || open->last->getParent() != store->getParent() || open->row != r || open->index != index ||
        (open->mask & (1u << *col))) {
      runs.push_back({.row = r, .index = index, .last = store});
      open = &runs.back();
    }
    open->lanes[*col] = store->getArgOperand(4);
    open->mask |= 1u << *col;
    open->last = store;
  }
  if (coalescible) {
    std::map<uint32_t, uint32_t> row_mask;
    for (auto &run : runs) {
      auto [it, inserted] = row_mask.emplace(run.row, run.mask);
      if (!inserted && it->second != run.mask)
        coalescible = false;
    }
  }

  if (coalescible) {
    for (auto &run : runs) {
      b.SetInsertPoint(run.last->getNextNode());
      llvm::Value *lanes[4] = {};
      for (unsigned c = 0; c < 4; c++)
        if (run.lanes[c])
          lanes[c] = to_lane_bits(b, run.lanes[c], element.component_type);
      emit_row(ctx, output, output.rows[run.row], b.CreateZExtOrTrunc(run.index, b.getInt32Ty()),
               llvm::ArrayRef<llvm::Value *>(lanes, element.cols));
    }
  } else {
    auto *row_type = llvm::ArrayType::get(llvm::FixedVectorType::get(b.getInt32Ty(), 4), element.rows);
    auto *type = llvm::ArrayType::get(row_type, max_count);
    output.staging = new llvm::GlobalVariable(
      ctx.module, type, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(type),
      "d3dalloy.mesh." + std::string(output.primitive ? "primitive." : "vertex.") + element.name, nullptr,
      llvm::GlobalValue::NotThreadLocal, 3
    );
    output.staging->setAlignment(llvm::Align(16));
    for (auto *store : output.stores) {
      b.SetInsertPoint(store);
      auto *index = b.CreateZExtOrTrunc(store->getArgOperand(5), b.getInt32Ty());
      auto *row = b.CreateZExtOrTrunc(store->getArgOperand(2), b.getInt32Ty());
      auto col = ConstantOperand(*store, 3);
      if (!col)
        return col.takeError();
      auto *lane = b.CreateGEP(type, output.staging, {b.getInt32(0), index, row, b.getInt32(*col)});
      b.CreateStore(to_lane_bits(b, store->getArgOperand(4), element.component_type), lane);
    }
  }
  for (auto *store : output.stores)
    store->eraseFromParent();
  output.stores.clear();
  return llvm::Error::success();
}

/** Define the AIR mesh outputs of one signature list; fills `outputs`. */
llvm::Error define_mesh_outputs(
  air::FunctionSignatureBuilder &signature, const std::vector<IOElement> &elements, bool primitive,
  std::vector<OutputElement> &outputs, uint32_t &data_count, uint32_t &clip_count
) {
  for (auto &element : elements) {
    OutputElement output{.element = &element, .primitive = primitive};
    if (element.cols == 0 || element.cols > 4)
      return createError("DXIL mesh output " + element.name + " has an invalid column count");
    for (uint32_t r = 0; r < element.rows; r++) {
      const uint32_t semantic_index = r < element.semantic_indices.size() ? element.semantic_indices[r] : r;
      OutputRow row{};
      auto define_data = [&](air::MSLScalerOrVectorType type) {
        row.data_index = data_count++;
        row.type = type;
        air::OutputMeshData data{
          .user = dxbc::varyingAttributeName(element.name, semantic_index), .type = type, .index = row.data_index
        };
        if (primitive)
          signature.DefineMeshPrimitiveOutput(data);
        else
          signature.DefineMeshVertexOutput(data);
      };
      switch (element.semantic_kind) {
      case KIND_SHADING_RATE: {
        if (!primitive || element.cols != 1)
          return createError("SV_ShadingRate must be a scalar primitive output");
        row.kind = OutputRow::Kind::Data;
        row.data_index = data_count++;
        row.type = air::msl_uint;
        row.is_float = false;
        signature.DefineMeshPrimitiveOutput(air::OutputMeshData{
          .user = "d3dalloy_shading_rate", .type = air::msl_uint, .index = row.data_index
        });
        break;
      }
      case KIND_ARBITRARY:
        row.kind = OutputRow::Kind::Data;
        row.packed_user = true;
        define_data(data_type(element.component_type, 4, row.is_float));
        break;
      case KIND_POSITION:
        if (primitive)
          return createError("DXIL mesh shader declares SV_Position as a primitive output");
        row.kind = OutputRow::Kind::Position;
        signature.DefineMeshVertexOutput(air::OutputPosition{.type = air::msl_float4});
        break;
      case KIND_CLIP_DISTANCE:
        if (primitive)
          return createError("DXIL mesh shader declares SV_ClipDistance as a primitive output");
        row.kind = OutputRow::Kind::Data;
        row.clip_base = clip_count;
        clip_count += element.cols;
        row.is_float = true;
        row.packed_user = true;
        define_data(air::msl_float4);
        break;
      case KIND_CULL_DISTANCE:
        if (primitive)
          return createError("DXIL mesh shader declares SV_CullDistance as a primitive output");
        // Mesh capture retains these lanes without interpolation. Its final primitive export
        // rejects a primitive only when every referenced vertex is negative on the same plane.
        // The float4 varying also preserves the existing PS-readable distance ABI.
        row.kind = OutputRow::Kind::Data;
        row.is_float = true;
        row.packed_user = true;
        define_data(air::msl_float4);
        break;
      case KIND_RENDER_TARGET_ARRAY_INDEX:
      case KIND_VIEWPORT_ARRAY_INDEX:
      case KIND_PRIMITIVE_ID:
      case KIND_CULL_PRIMITIVE:
        if (!primitive)
          return createError("DXIL mesh shader declares " + element.name + " as a vertex output");
        if (element.semantic_kind == KIND_RENDER_TARGET_ARRAY_INDEX) {
          row.kind = OutputRow::Kind::RenderTargetArrayIndex;
          signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
        } else if (element.semantic_kind == KIND_VIEWPORT_ARRAY_INDEX) {
          row.kind = OutputRow::Kind::ViewportArrayIndex;
          signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
        } else if (element.semantic_kind == KIND_PRIMITIVE_ID) {
          row.kind = OutputRow::Kind::PrimitiveID;
          signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveID{});
        } else {
          row.kind = OutputRow::Kind::Culled;
          signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveCulled{});
        }
        break;
      default:
        return unsupported("DXIL mesh shader output " + element.name);
      }
      output.rows.push_back(row);
    }
    outputs.push_back(std::move(output));
  }
  return llvm::Error::success();
}

/** `for (i = tid; i < count; i += step) body(i)` at the builder; continues after the loop. */
template <typename Body>
void emit_strided_loop(
  llvm::IRBuilder<> &b, llvm::Function *function, llvm::Value *tid, uint32_t count, uint32_t step, Body body
) {
  auto *pre = b.GetInsertBlock();
  auto *header = llvm::BasicBlock::Create(b.getContext(), "mesh_loop", function);
  auto *loop_body = llvm::BasicBlock::Create(b.getContext(), "mesh_loop_body", function);
  auto *exit = llvm::BasicBlock::Create(b.getContext(), "mesh_loop_exit", function);
  b.CreateBr(header);
  b.SetInsertPoint(header);
  auto *i = b.CreatePHI(b.getInt32Ty(), 2);
  i->addIncoming(tid, pre);
  b.CreateCondBr(b.CreateICmpULT(i, b.getInt32(count)), loop_body, exit);
  b.SetInsertPoint(loop_body);
  body(i);
  auto *next = b.CreateAdd(i, b.getInt32(step));
  i->addIncoming(next, b.GetInsertBlock());
  b.CreateBr(header);
  b.SetInsertPoint(exit);
}

} // namespace

llvm::Expected<std::unique_ptr<dxbc::BindingMap>> LowerMeshStage(
  ModuleInfo &module, dxbc::SM50ShaderInternal &shader, const char *function_name,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
) {
  const bool mesh = module.stage == Stage::Mesh;
  auto &module_ir = *module.module;
  auto &llvm_context = *module.context;
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

  const uint32_t threads = module.num_threads[0] * module.num_threads[1] * module.num_threads[2];
  if (!threads || threads > 128)
    return createError("DXIL mesh/amplification shader has an invalid numthreads");

  auto entry_signature = ReadEntrySignature(module);
  if (!entry_signature)
    return entry_signature.takeError();
  if (mesh) {
    uint64_t output_scalars = 0;
    bool position = false;
    for (const auto *elements : {&entry_signature->outputs, &entry_signature->patch_constants})
      for (const auto &element : *elements) {
        if (element.semantic_kind == KIND_CULL_PRIMITIVE) continue;
        output_scalars += uint64_t(element.rows) * element.cols;
        if (element.start_row < 0 || uint64_t(element.start_row) + element.rows > 32)
          return createError("DXIL mesh output exceeds the mandatory 32-row signature");
        if (element.semantic_kind == KIND_POSITION) {
          if (elements != &entry_signature->outputs || element.rows != 1 || element.cols != 4 ||
              element.component_type != COMPONENT_F32)
            return createError("DXIL mesh SV_Position must be a vertex float4");
          position = true;
        }
      }
    if (!position || output_scalars > 128)
      return createError("DXIL mesh output requires SV_Position and at most 128 scalars");
  }

  auto shader_type = to_program_type(module.stage);
  shader.shader_type = shader_type;
  auto &func_signature = shader.func_signature;
  dxbc::IREffect prologue([](auto) { return std::monostate(); });
  dxbc::IRValue epilogue([](dxbc::context) -> dxbc::pvalue { return nullptr; });
  dxbc::io_binding_map resource_map;
  air::AirType types(llvm_context);
  StageSignature signature;
  {
    dxbc::SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    if (auto err = DeclareStageSignature(module, shader, sig_ctx, signature))
      return std::move(err);
  }
  const uint32_t thread_index_arg = func_signature.DefineInput(air::InputThreadIndexInThreadgroup{});
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *stats = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(args, &stats);
  const uint32_t stats_arg = stats ? dxbc::define_pipeline_statistics(func_signature) : ~0u;
  /* SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM: every MS threadgroup takes a draw-unique number from the
   * claim header's mesh_groups counter (threadgroup positions repeat across AS groups); primitive p's key is
   * (number, p + 1, 1). */
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *claim_arg = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(
    args, &claim_arg
  );
  const bool statistics_key = mesh && stats && claim_arg;
  uint32_t statistics_claim_arg = ~0u, primitive_key_index = 0;

  /* Payload: the mesh reads what the object stage wrote, the object writes its own. */
  auto has_calls = [&](llvm::StringRef name) { return !CollectCalls(*module.entry, name).empty(); };
  uint32_t payload_size = module.mesh.payload_size;
  const bool payload_used = mesh ? has_calls("getMeshPayload") : has_calls("dispatchMesh");
  if (payload_used && !payload_size) {
    llvm::Type *payload_type = nullptr;
    for (auto *call : CollectCalls(*module.entry, mesh ? "getMeshPayload" : "dispatchMesh")) {
      llvm::Type *pointer = mesh ? call->getType() : call->getArgOperand(4)->getType();
      payload_type = pointer->getNonOpaquePointerElementType();
    }
    if (payload_type)
      payload_size = (uint32_t)module_ir.getDataLayout().getTypeAllocSize(payload_type);
  }
  if (payload_size > 16384)
    return createError("DXIL mesh payload exceeds 16384 bytes");
  const uint32_t payload_arg =
    payload_used && payload_size ? func_signature.DefineInput(air::InputPayload{.size = payload_size}) : ~0u;

  std::vector<OutputElement> vertex_outputs, primitive_outputs;
  uint32_t max_vertices = 0, max_primitives = 0, primitive_vertices = 0;
  if (mesh) {
    max_vertices = module.mesh.max_vertices;
    max_primitives = module.mesh.max_primitives;
    if (max_vertices > 256 || max_primitives > 256)
      return createError("DXIL mesh shader exceeds 256 output vertices or primitives");
    air::MeshOutputTopology topology;
    switch (module.mesh.output_topology) {
    case TOPOLOGY_LINE:
      topology = air::MeshOutputTopology::Line, primitive_vertices = 2;
      break;
    case TOPOLOGY_TRIANGLE:
      topology = air::MeshOutputTopology::Triangle, primitive_vertices = 3;
      break;
    default:
      return createError("DXIL mesh shader has no output topology");
    }
    uint32_t vertex_data = 0, primitive_data = 0, clip_count = 0, unused_clip = 0;
    if (auto err = define_mesh_outputs(
          func_signature, entry_signature->outputs, false, vertex_outputs, vertex_data, clip_count
        ))
      return std::move(err);
    if (auto err = define_mesh_outputs(
          func_signature, entry_signature->patch_constants, true, primitive_outputs, primitive_data, unused_clip
        ))
      return std::move(err);
    if (clip_count)
      func_signature.DefineMeshVertexOutput(air::OutputClipDistance{clip_count});
    if (statistics_key) {
      primitive_key_index = primitive_data;
      dxbc::define_mesh_statistics_key(func_signature, primitive_data);
      statistics_claim_arg = func_signature.DefineInput(air::ArgumentBindingBuffer{
        .buffer_size = {},
        .location_index = SM50_BINDING_INDEX_STATISTICS_CLAIM,
        .array_size = 0,
        .memory_access = air::MemoryAccess::read_write,
        .address_space = air::AddressSpace::device,
        .type = air::MSLUint{},
        .arg_name = "statistics_claim",
        .raster_order_group = {}
      });
    }
    /* Metal's mesh type needs at least one vertex/primitive slot; D3D allows declaring 0. */
    func_signature.DefineInput(air::InputMesh{std::max(max_vertices, 1u), std::max(max_primitives, 1u), topology});
  } else {
    func_signature.DefineInput(air::InputMeshGridProperties{});
  }

  auto binding_map = dxbc::setup_binding_rootsig(
    &shader.shader_info, func_signature, module_ir, shader_type, rootsig->bytecode, rootsig->bytecode_length
  );
  if (!binding_map)
    return createError("DXIL shader has an invalid root signature");

  module.entry->setName(llvm::Twine(function_name) + ".dxil");
  auto [function, function_metadata] = func_signature.CreateFunction(function_name, llvm_context, module_ir, 0, true);
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
    .pso_sample_mask = 0xffffffff,
    .shader_type = shader_type,
    .metal_version = metal_version,
  };
  if (auto err = prologue.build(ctx).takeError())
    return std::move(err);
  llvm::Value *thread_index = function->getArg(thread_index_arg);

  /*
   * Pipeline statistics (D3D12_QUERY_DATA_PIPELINE_STATISTICS1): AS/MSInvocations count threads, one atomic add
   * per group. MSPrimitives counts the primitives SetMeshOutputCounts declares minus those SV_CullPrimitive culls
   * (MSPrimitivesPipelineStatisticIncludesCulledPrimitives = FALSE); exactly those primitives reach the clipper
   * stage, so they are also CInvocations and CPrimitives (a primitive split by a clip plane counts once, as on
   * the GS mesh path).
   */
  llvm::GlobalVariable *stats_count = nullptr, *stats_culled = nullptr;
  const uint32_t culled_words = (std::max(max_primitives, 1u) + 31) / 32;
  if (stats_arg != ~0u && mesh) {
    auto *i32 = builder.getInt32Ty();
    stats_count = new llvm::GlobalVariable(
      module_ir, i32, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(i32),
      "d3dalloy.mesh.statistics.count", nullptr, llvm::GlobalValue::NotThreadLocal, 3
    );
    for (auto &output : primitive_outputs)
      for (auto &row : output.rows)
        if (row.kind == OutputRow::Kind::Culled) {
          if (!stats_culled) {
            auto *type = llvm::ArrayType::get(i32, culled_words);
            stats_culled = new llvm::GlobalVariable(
              module_ir, type, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(type),
              "d3dalloy.mesh.statistics.culled", nullptr, llvm::GlobalValue::NotThreadLocal, 3
            );
          }
          output.culled_bits = stats_culled;
        }
  }
  if (stats_arg != ~0u) {
    auto *count_bb = llvm::BasicBlock::Create(llvm_context, "statistics_invocations", function);
    auto *done_bb = llvm::BasicBlock::Create(llvm_context, "statistics_invocations_done", function);
    builder.CreateCondBr(builder.CreateICmpEQ(thread_index, builder.getInt32(0)), count_bb, done_bb);
    builder.SetInsertPoint(count_bb);
    dxbc::add_pipeline_statistics(builder, air, function, stats_arg, mesh ? 24 : 22, builder.getInt32(threads));
    if (stats_count)
      builder.CreateStore(builder.getInt32(0), stats_count);
    if (stats_culled)
      for (uint32_t w = 0; w < culled_words; w++)
        builder.CreateStore(
          builder.getInt32(0),
          builder.CreateConstInBoundsGEP2_32(stats_culled->getValueType(), stats_culled, 0, w)
        );
    builder.CreateBr(done_bb);
    builder.SetInsertPoint(done_bb);
    if (stats_count)
      air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
  }
  auto body_entry = TransplantEntryBody(*module.entry, *function, *epilogue_bb);
  if (!body_entry)
    return body_entry.takeError();
  builder.CreateBr(*body_entry);
  if (auto err = LowerStageIO(ctx, signature))
    return std::move(err);

  llvm::Value *payload = payload_arg != ~0u ? function->getArg(payload_arg) : nullptr;

  /* Collect the mesh operations in instruction order (store runs depend on it). */
  llvm::SmallVector<llvm::CallInst *, 32> calls;
  for (auto &block : *function)
    for (auto &instruction : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (auto *callee = call->getCalledFunction(); callee && callee->getName().startswith("dx.op.") &&
                                                    call->arg_size())
          if (auto *op = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
              op && op->getZExtValue() >= OP_SET_MESH_OUTPUT_COUNTS && op->getZExtValue() <= OP_DISPATCH_MESH)
            calls.push_back(call);

  bool dispatched = false;
  for (auto *call : calls) {
    const uint32_t op = (uint32_t)llvm::cast<llvm::ConstantInt>(call->getArgOperand(0))->getZExtValue();
    const bool mesh_op = op != OP_DISPATCH_MESH;
    if (mesh_op != mesh)
      return createError("dx.op " + llvm::Twine(op) + " is not available in this stage");
    switch (op) {
    case OP_SET_MESH_OUTPUT_COUNTS:
      builder.SetInsertPoint(call);
      /* The vertex count is implicit in Metal: vertices past it are never referenced. */
      air.CreateSetMeshPrimitiveCount(builder.CreateZExtOrTrunc(call->getArgOperand(2), builder.getInt32Ty()));
      if (stats_count)
        builder.CreateStore(builder.CreateZExtOrTrunc(call->getArgOperand(2), builder.getInt32Ty()), stats_count);
      call->eraseFromParent();
      break;
    case OP_EMIT_INDICES: {
      builder.SetInsertPoint(call);
      auto *base = builder.CreateMul(
        builder.CreateZExtOrTrunc(call->getArgOperand(1), builder.getInt32Ty()), builder.getInt32(primitive_vertices)
      );
      for (unsigned k = 0; k < primitive_vertices; k++)
        air.CreateSetMeshIndex(builder.CreateAdd(base, builder.getInt32(k)), call->getArgOperand(2 + k));
      call->eraseFromParent();
      break;
    }
    case OP_GET_MESH_PAYLOAD: {
      if (!payload)
        return createError("DXIL mesh shader reads a payload of size 0");
      builder.SetInsertPoint(call);
      auto *pointer_type = llvm::cast<llvm::PointerType>(call->getType());
      auto *typed = builder.CreateBitCast(
        payload, pointer_type->getNonOpaquePointerElementType()->getPointerTo((uint32_t)air::AddressSpace::object_data)
      );
      call->replaceAllUsesWith(builder.CreateAddrSpaceCast(typed, pointer_type));
      call->eraseFromParent();
      break;
    }
    case OP_STORE_VERTEX_OUTPUT:
    case OP_STORE_PRIMITIVE_OUTPUT: {
      auto id = ConstantOperand(*call, 1);
      if (!id)
        return id.takeError();
      auto &outputs = op == OP_STORE_VERTEX_OUTPUT ? vertex_outputs : primitive_outputs;
      auto found = std::find_if(outputs.begin(), outputs.end(), [&](auto &o) { return o.element->id == *id; });
      if (found == outputs.end())
        return createError("DXIL mesh shader stores undeclared output element " + llvm::Twine(*id));
      found->stores.push_back(call);
      break;
    }
    case OP_DISPATCH_MESH: {
      if (dispatched)
        return createError("DXIL amplification shader calls DispatchMesh more than once");
      dispatched = true;
      llvm::Value *grid[3] = {call->getArgOperand(1), call->getArgOperand(2), call->getArgOperand(3)};
      llvm::Value *source = call->getArgOperand(4);
      auto *head = call->getParent();
      head->splitBasicBlock(call->getNextNode(), "dispatch_mesh_dead");
      head->getTerminator()->eraseFromParent();
      builder.SetInsertPoint(head);
      /* DispatchMesh implies GroupMemoryBarrierWithGroupSync. */
      air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
      if (payload) {
        auto *src = builder.CreateBitCast(source, builder.getInt32Ty()->getPointerTo(3));
        auto *dst = builder.CreateBitCast(payload, builder.getInt32Ty()->getPointerTo((uint32_t)air::AddressSpace::object_data));
        emit_strided_loop(builder, function, thread_index, payload_size / 4, threads, [&](llvm::Value *i) {
          builder.CreateStore(builder.CreateLoad(builder.getInt32Ty(), builder.CreateGEP(builder.getInt32Ty(), src, i)),
                              builder.CreateGEP(builder.getInt32Ty(), dst, i));
        });
        if (payload_size % 4) {
          auto *src8 = builder.CreateBitCast(source, builder.getInt8Ty()->getPointerTo(3));
          auto *dst8 = builder.CreateBitCast(payload, builder.getInt8Ty()->getPointerTo((uint32_t)air::AddressSpace::object_data));
          emit_strided_loop(builder, function, builder.CreateAdd(thread_index, builder.getInt32(payload_size & ~3u)),
                            payload_size, threads, [&](llvm::Value *i) {
            builder.CreateStore(builder.CreateLoad(builder.getInt8Ty(), builder.CreateGEP(builder.getInt8Ty(), src8, i)),
                                builder.CreateGEP(builder.getInt8Ty(), dst8, i));
          });
        }
      }
      /* The arguments are read from the first thread when they are not group uniform. */
      auto *first = llvm::BasicBlock::Create(llvm_context, "dispatch_mesh_first", function);
      builder.CreateCondBr(builder.CreateICmpEQ(thread_index, builder.getInt32(0)), first, epilogue_bb);
      builder.SetInsertPoint(first);
      llvm::Value *size = llvm::UndefValue::get(llvm::FixedVectorType::get(builder.getInt32Ty(), 3));
      for (unsigned c = 0; c < 3; c++)
        size = builder.CreateInsertElement(size, builder.CreateZExtOrTrunc(grid[c], builder.getInt32Ty()), c);
      air.CreateSetMeshProperties(size);
      builder.CreateBr(epilogue_bb);
      call->eraseFromParent();
      break;
    }
    }
  }

  for (auto &output : vertex_outputs)
    if (auto err = lower_output_stores(ctx, output, std::max(max_vertices, 1u)))
      return std::move(err);
  for (auto &output : primitive_outputs)
    if (auto err = lower_output_stores(ctx, output, std::max(max_primitives, 1u)))
      return std::move(err);
  llvm::EliminateUnreachableBlocks(*function);

  builder.SetInsertPoint(epilogue_bb);
  auto flush = [&](std::vector<OutputElement> &outputs, uint32_t count) {
    bool staged = std::any_of(outputs.begin(), outputs.end(), [](auto &o) { return o.staging; });
    if (!staged)
      return;
    auto *barrier = air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    barrier->setMetadata("d3dalloy.mesh.output_barrier", llvm::MDTuple::get(llvm_context, {}));
    emit_strided_loop(builder, function, thread_index, count, threads, [&](llvm::Value *i) {
      for (auto &output : outputs) {
        if (!output.staging)
          continue;
        for (uint32_t r = 0; r < output.element->rows; r++) {
          auto *row = builder.CreateLoad(
            llvm::FixedVectorType::get(builder.getInt32Ty(), 4),
            builder.CreateGEP(output.staging->getValueType(), output.staging, {builder.getInt32(0), i, builder.getInt32(r)})
          );
          llvm::Value *lanes[4];
          for (unsigned c = 0; c < 4; c++)
            lanes[c] = builder.CreateExtractElement(row, c);
          emit_row(ctx, output, output.rows[r], i, llvm::ArrayRef<llvm::Value *>(lanes, output.element->cols));
        }
      }
    });
  };
  flush(vertex_outputs, std::max(max_vertices, 1u));
  flush(primitive_outputs, std::max(max_primitives, 1u));
  if (statistics_key) {
    auto *i32 = builder.getInt32Ty();
    auto *group = new llvm::GlobalVariable(
      module_ir, i32, false, llvm::GlobalValue::InternalLinkage, llvm::UndefValue::get(i32),
      "d3dalloy.mesh.statistics.group", nullptr, llvm::GlobalValue::NotThreadLocal, 3
    );
    auto *take_bb = llvm::BasicBlock::Create(llvm_context, "statistics_group", function);
    auto *taken_bb = llvm::BasicBlock::Create(llvm_context, "statistics_group_done", function);
    builder.CreateCondBr(builder.CreateICmpEQ(thread_index, builder.getInt32(0)), take_bb, taken_bb);
    builder.SetInsertPoint(take_bb);
    auto *counter = builder.CreateConstInBoundsGEP1_32(
      i32, function->getArg(statistics_claim_arg), offsetof(SM50_STATISTICS_CLAIM_HEADER, mesh_groups) / 4
    );
    builder.CreateStore(air.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counter, builder.getInt32(1)), group);
    builder.CreateBr(taken_bb);
    builder.SetInsertPoint(taken_bb);
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    auto *number = builder.CreateLoad(i32, group);
    emit_strided_loop(builder, function, thread_index, std::max(max_primitives, 1u), threads, [&](llvm::Value *p) {
      dxbc::set_mesh_statistics_key(builder, air, p, number, builder.CreateAdd(p, builder.getInt32(1)),
                                    builder.getInt32(1), primitive_key_index);
    });
  }
  if (stats_count) {
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    auto *count_bb = llvm::BasicBlock::Create(llvm_context, "statistics_primitives", function);
    auto *done_bb = llvm::BasicBlock::Create(llvm_context, "statistics_primitives_done", function);
    builder.CreateCondBr(builder.CreateICmpEQ(thread_index, builder.getInt32(0)), count_bb, done_bb);
    builder.SetInsertPoint(count_bb);
    llvm::Value *count = builder.CreateLoad(builder.getInt32Ty(), stats_count);
    /* A count above the declared maximum is undefined in D3D; Metal clamps it, so the statistic does too. */
    count = builder.CreateSelect(builder.CreateICmpUGT(count, builder.getInt32(max_primitives)),
                                 builder.getInt32(max_primitives), count);
    llvm::Value *visible = count;
    if (stats_culled) {
      for (uint32_t w = 0; w < culled_words; w++) {
        auto *bits = builder.CreateLoad(
          builder.getInt32Ty(), builder.CreateConstInBoundsGEP2_32(stats_culled->getValueType(), stats_culled, 0, w)
        );
        /* Bits of primitives at or past the output count are not output primitives. */
        auto *live = builder.CreateSub(count, builder.getInt32(w * 32));
        auto *mask = builder.CreateSelect(
          builder.CreateICmpSGE(live, builder.getInt32(32)), builder.getInt32(~0u),
          builder.CreateSelect(builder.CreateICmpSLE(live, builder.getInt32(0)), builder.getInt32(0),
                               builder.CreateSub(builder.CreateShl(builder.getInt32(1), live), builder.getInt32(1)))
        );
        visible = builder.CreateSub(
          visible, air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, builder.CreateAnd(bits, mask))
        );
      }
    }
    dxbc::add_pipeline_statistics(builder, air, function, stats_arg, 26, visible); // MSPrimitives
    dxbc::add_pipeline_statistics(builder, air, function, stats_arg, 10, visible); // CInvocations
    dxbc::add_pipeline_statistics(builder, air, function, stats_arg, 12, visible); // CPrimitives
    builder.CreateBr(done_bb);
    builder.SetInsertPoint(done_bb);
  }
  builder.CreateRetVoid();

  module_ir.getOrInsertNamedMetadata(mesh ? "air.mesh" : "air.object")->addOperand(function_metadata);
  return std::move(binding_map);
}

} // namespace d3dalloy::dxil
