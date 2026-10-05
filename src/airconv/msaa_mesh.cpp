#include "msaa_mesh.hpp"
#include "msaa_vertex.hpp"
#include "air_signature.hpp"
#include "dxil_converter.hpp"
#include "nt/air_builder.hpp"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace dxmt::msaa {
namespace {

Error invalid(const Twine &message) {
  return createStringError(inconvertibleErrorCode(), "MSAA mesh: %s", message.str().c_str());
}

struct Field {
  MDNode *metadata;
  Type *type;
  bool primitive;
  StringRef kind;
  unsigned data_index = ~0u;
  unsigned offset = 0;
};

StringRef tag(MDNode *node, unsigned index) {
  auto *value = index < node->getNumOperands() ? dyn_cast<MDString>(node->getOperand(index)) : nullptr;
  return value ? value->getString() : StringRef{};
}

unsigned number(MDNode *node, unsigned index) {
  auto *value = index < node->getNumOperands() ? dyn_cast<ConstantAsMetadata>(node->getOperand(index)) : nullptr;
  return value && isa<ConstantInt>(value->getValue()) ? cast<ConstantInt>(value->getValue())->getZExtValue() : 0;
}

Type *fieldType(MDNode *node, LLVMContext &context) {
  StringRef name;
  for (unsigned i = 0; i + 1 < node->getNumOperands(); ++i)
    if (tag(node, i) == "air.arg_type_name") name = tag(node, i + 1);
  unsigned lanes = 1;
  if (!name.empty() && name.back() >= '2' && name.back() <= '4') {
    lanes = name.back() - '0';
    name = name.drop_back();
  }
  Type *scalar = name == "float" ? Type::getFloatTy(context)
      : name == "half" ? Type::getHalfTy(context)
      : name == "bool" ? Type::getInt1Ty(context)
      : name == "int" || name == "uint" ? Type::getInt32Ty(context) : nullptr;
  if (!scalar) return nullptr;
  if (tag(node, 0) == "air.clip_distance")
    return ArrayType::get(scalar, number(node, 2));
  return lanes == 1 ? scalar : FixedVectorType::get(scalar, lanes);
}

MDNode *vertexMetadata(const Field &field, LLVMContext &context) {
  SmallVector<Metadata *, 12> values(field.metadata->op_begin(), field.metadata->op_end());
  if (field.kind == "air.mesh_vertex_data" || field.kind == "air.mesh_primitive_data") {
    values[0] = MDString::get(context, "air.vertex_output");
    values.erase(values.begin() + 1);
  } else if (field.kind == "air.primitive_id") {
    values[0] = MDString::get(context, "air.vertex_output");
    values.insert(values.begin() + 1, MDString::get(context, "user(dxmt_primitive_id)"));
  }
  return MDTuple::get(context, values);
}

air::ArgumentBindingBuffer binding(unsigned slot, bool constant, const char *name) {
  return {.buffer_size = {}, .location_index = slot, .array_size = 0,
    .memory_access = constant ? air::MemoryAccess::read : air::MemoryAccess::read_write,
    .address_space = constant ? air::AddressSpace::constant : air::AddressSpace::device,
    .type = air::MSLUint{}, .arg_name = name, .raster_order_group = {}};
}

Error transformObject(Module &module, const SM50_SHADER_D3D12_MSAA_MESH_DATA &options) {
  auto *objects = module.getNamedMetadata("air.object");
  if (!objects || objects->getNumOperands() != 1) return invalid("expected one object entry");
  auto *metadata = objects->getOperand(0);
  auto *original = cast<Function>(cast<ValueAsMetadata>(metadata->getOperand(0))->getValue());
  auto *inputs = cast<MDTuple>(metadata->getOperand(2));
  unsigned payloadArg = ~0u, groupArg = ~0u, threadArg = ~0u, propertiesArg = ~0u;
  SM50_MSAA_OBJECT_LAYOUT layout{};
  for (unsigned i = 0; i < inputs->getNumOperands(); ++i) {
    auto *input = cast<MDNode>(inputs->getOperand(i));
    auto kind = tag(input, 1);
    if (kind == "air.payload") {
      payloadArg = i;
      for (unsigned j = 2; j + 1 < input->getNumOperands(); ++j)
        if (tag(input, j) == "air.arg_type_size") layout.payload_bytes = number(input, j + 1);
    }
    if (kind == "air.threadgroup_position_in_grid") groupArg = i;
    if (kind == "air.thread_index_in_threadgroup") threadArg = i;
    if (kind == "air.mesh_grid_properties") propertiesArg = i;
  }
  layout.record_dwords = 4 + (layout.payload_bytes + 3) / 4;
  if (options.object_layout_out) *options.object_layout_out = layout;
  if (threadArg == ~0u || propertiesArg == ~0u) return invalid("missing object builtins");
  SmallVector<Type *, 24> parameters(original->getFunctionType()->param_begin(), original->getFunctionType()->param_end());
  const unsigned recordsArg = parameters.size(); parameters.push_back(Type::getInt32Ty(module.getContext())->getPointerTo(1));
  const unsigned controlArg = parameters.size(); parameters.push_back(Type::getInt32Ty(module.getContext())->getPointerTo(2));
  const bool addGroup = groupArg == ~0u;
  if (addGroup) { groupArg = parameters.size(); parameters.push_back(FixedVectorType::get(Type::getInt32Ty(module.getContext()), 3)); }
  const std::string name = original->getName().str();
  original->setName(name + ".original");
  auto *entry = Function::Create(FunctionType::get(Type::getVoidTy(module.getContext()), parameters, false), Function::ExternalLinkage, name, module);
  SmallVector<Metadata *, 24> inputMetadata(inputs->op_begin(), inputs->op_end());
  inputMetadata.push_back(air::BuildBufferArgumentMetadata(recordsArg, binding(SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS, false, "msaa_object_records"), module.getContext(), module.getDataLayout()));
  inputMetadata.push_back(air::BuildBufferArgumentMetadata(controlArg, binding(SM50_BINDING_INDEX_MSAA_MESH_CONTROL, true, "msaa_object_control"), module.getContext(), module.getDataLayout()));
  if (addGroup) {
    air::FunctionSignatureBuilder signature; signature.DefineInput(air::InputThreadgroupPositionInGrid{});
    auto [dummy, md] = signature.CreateFunction("msaa_object_builtin", module.getContext(), module, 0, true);
    auto *input = cast<MDNode>(cast<MDTuple>(md->getOperand(2))->getOperand(0));
    SmallVector<Metadata *, 12> values(input->op_begin(), input->op_end());
    values[0] = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(module.getContext()), groupArg));
    inputMetadata.push_back(MDTuple::get(module.getContext(), values)); dummy->eraseFromParent();
  }
  SmallVector<Metadata *, 8> updated(metadata->op_begin(), metadata->op_end());
  updated[0] = ValueAsMetadata::get(entry); updated[2] = MDTuple::get(module.getContext(), inputMetadata);
  objects->setOperand(0, MDTuple::get(module.getContext(), updated));
  auto *records = entry->getArg(recordsArg);
  if (options.variant == SM50_MSAA_OBJECT_REPLAY) {
    IRBuilder<> b(BasicBlock::Create(module.getContext(), "entry", entry));
    llvm::raw_null_ostream debug; llvm::air::AIRBuilder air(b, debug);
    if (layout.payload_bytes) {
      unsigned threads = 32;
      for (unsigned i = 3; i < metadata->getNumOperands(); ++i)
        if (auto *limit = dyn_cast<MDNode>(metadata->getOperand(i)); limit && tag(limit, 0) == "air.max_work_group_size") threads = number(limit, 1);
      auto *source = b.CreateBitCast(b.CreateConstGEP1_32(b.getInt32Ty(), records, 4), b.getInt8Ty()->getPointerTo(1));
      auto *payload = b.CreateBitCast(entry->getArg(payloadArg), b.getInt8Ty()->getPointerTo(6));
      auto *head = b.GetInsertBlock(), *loop = BasicBlock::Create(module.getContext(), "payload_loop", entry);
      auto *body = BasicBlock::Create(module.getContext(), "payload_copy", entry), *done = BasicBlock::Create(module.getContext(), "payload_done", entry);
      b.CreateBr(loop); b.SetInsertPoint(loop);
      auto *index = b.CreatePHI(b.getInt32Ty(), 2); index->addIncoming(entry->getArg(threadArg), head);
      b.CreateCondBr(b.CreateICmpULT(index, b.getInt32(layout.payload_bytes)), body, done);
      b.SetInsertPoint(body);
      b.CreateStore(b.CreateLoad(b.getInt8Ty(), b.CreateGEP(b.getInt8Ty(), source, index)), b.CreateGEP(b.getInt8Ty(), payload, index));
      auto *next = b.CreateAdd(index, b.getInt32(threads)); b.CreateBr(loop); index->addIncoming(next, body); b.SetInsertPoint(done);
      air.CreateBarrier(llvm::air::MemFlags::ObjectData);
    }
    auto *first = BasicBlock::Create(module.getContext(), "publish", entry), *done = BasicBlock::Create(module.getContext(), "done", entry);
    b.CreateCondBr(b.CreateICmpEQ(entry->getArg(threadArg), b.getInt32(0)), first, done); b.SetInsertPoint(first);
    Value *grid = UndefValue::get(FixedVectorType::get(b.getInt32Ty(), 3));
    for (unsigned axis = 0; axis < 3; ++axis) grid = b.CreateInsertElement(grid, b.CreateLoad(b.getInt32Ty(), b.CreateConstGEP1_32(b.getInt32Ty(), records, axis)), axis);
    // Child-group order within a parent is unconstrained; parent records are replayed serially.
    air.CreateSetMeshProperties(grid); b.CreateBr(done); b.SetInsertPoint(done); b.CreateRetVoid();
  } else if (options.variant == SM50_MSAA_OBJECT_CAPTURE) {
    ValueToValueMapTy mapping;
    for (unsigned i = 0; i < original->arg_size(); ++i) mapping[original->getArg(i)] = entry->getArg(i);
    SmallVector<ReturnInst *, 8> returns;
    CloneFunctionInto(entry, original, mapping, CloneFunctionChangeType::LocalChangesOnly, returns);
    SmallVector<CallInst *, 4> dispatches;
    for (auto &block : *entry) for (auto &instruction : block)
      if (auto *call = dyn_cast<CallInst>(&instruction); call && call->getCalledFunction() && call->getCalledFunction()->getName() == "air.set_threadgroups_per_grid_mesh_properties") dispatches.push_back(call);
    for (auto *call : dispatches) {
      if (layout.payload_bytes) {
        // The original mesh launch publishes object_data after all AS lanes retire.
        // Capture reads it in lane zero before retirement, so publish explicitly
        // in the uniform predecessor, never inside the lane-zero branch.
        auto *publish = call->getParent()->getSinglePredecessor();
        if (!publish) return invalid("object dispatch has no uniform payload publication block");
        IRBuilder<> barrier(publish->getTerminator()); llvm::raw_null_ostream debug;
        llvm::air::AIRBuilder air(barrier, debug); air.CreateBarrier(llvm::air::MemFlags::ObjectData);
      }
      IRBuilder<> b(call);
      auto load = [&](unsigned word) { return b.CreateLoad(b.getInt32Ty(), b.CreateConstGEP1_32(b.getInt32Ty(), entry->getArg(controlArg), word)); };
      auto *position = entry->getArg(groupArg);
      Value *group = b.CreateAdd(b.CreateExtractElement(position, uint64_t(0)), b.CreateMul(load(0), b.CreateAdd(b.CreateExtractElement(position, 1), b.CreateMul(load(1), b.CreateExtractElement(position, 2)))));
      group = b.CreateAdd(group, load(2));
      auto *write = SplitBlockAndInsertIfThen(b.CreateICmpULT(group, load(3)), call, false); b.SetInsertPoint(write);
      auto *base = b.CreateMul(group, b.getInt32(layout.record_dwords));
      for (unsigned axis = 0; axis < 3; ++axis) b.CreateStore(b.CreateExtractElement(call->getArgOperand(1), axis), b.CreateGEP(b.getInt32Ty(), records, b.CreateAdd(base, b.getInt32(axis))));
      if (layout.payload_bytes) {
        auto *source = b.CreateBitCast(entry->getArg(payloadArg), b.getInt8Ty()->getPointerTo(6));
        auto *destination = b.CreateBitCast(b.CreateGEP(b.getInt32Ty(), records, b.CreateAdd(base, b.getInt32(4))), b.getInt8Ty()->getPointerTo(1));
        b.CreateMemCpy(destination, Align(4), source, Align(4), layout.payload_bytes);
      }
      call->eraseFromParent();
    }
  } else return invalid("unknown object variant");
  original->dropAllReferences(); original->eraseFromParent();
  return Error::success();
}

} // namespace

Error transformMesh(Module &module, const SM50_SHADER_D3D12_MSAA_MESH_DATA &options, bool conservative) {
  if (module.getNamedMetadata("air.object")) return transformObject(module, options);
  auto *meshes = module.getNamedMetadata("air.mesh");
  if (!meshes || meshes->getNumOperands() != 1) return invalid("expected one mesh entry");
  auto *metadata = meshes->getOperand(0);
  auto *original = dyn_cast<Function>(cast<ValueAsMetadata>(metadata->getOperand(0))->getValue());
  auto *inputs = dyn_cast<MDTuple>(metadata->getOperand(2));
  if (!original || !inputs) return invalid("missing entry arguments");
  // Tessellation publishes its primitive count/indices inside linked helpers.
  // Materialize them before collecting output stores, otherwise the device
  // capture has vertex data but no live topology and replays an empty mesh.
  SmallVector<Function *, 8> domainOutputs;
  for (auto &instruction : instructions(*original))
    if (auto *call = dyn_cast<CallBase>(&instruction); call && call->getCalledFunction() &&
        call->getCalledFunction()->getName().startswith("dxmt.domain_generate_primitives."))
      domainOutputs.push_back(call->getCalledFunction());
  // The exported helper delegates to templated connectivity/sink functions.
  // Materialize the complete reachable helper closure, not just its wrapper.
  for (unsigned i = 0; i < domainOutputs.size(); ++i)
    for (auto &instruction : instructions(*domainOutputs[i]))
      if (auto *call = dyn_cast<CallBase>(&instruction); call && call->getCalledFunction() &&
          !call->getCalledFunction()->isDeclaration() &&
          !llvm::is_contained(domainOutputs, call->getCalledFunction()))
        domainOutputs.push_back(call->getCalledFunction());
  if (!domainOutputs.empty()) for (;;) {
    SmallVector<CallBase *, 8> calls;
    for (auto &instruction : instructions(*original))
      if (auto *call = dyn_cast<CallBase>(&instruction); call &&
          llvm::is_contained(domainOutputs, call->getCalledFunction()))
        calls.push_back(call);
    if (calls.empty()) break;
    for (auto *call : calls) {
      InlineFunctionInfo info;
      if (!InlineFunction(*call, info).isSuccess())
        return invalid("cannot materialize domain primitive output helper");
    }
  }

  MDNode *meshInfo = nullptr;
  unsigned threadArg = ~0u, groupArg = ~0u;
  for (unsigned i = 0; i < inputs->getNumOperands(); ++i) {
    auto *input = cast<MDNode>(inputs->getOperand(i));
    if (tag(input, 1) == "air.mesh") meshInfo = cast<MDNode>(input->getOperand(2));
    if (tag(input, 1) == "air.thread_index_in_threadgroup") threadArg = i;
    if (tag(input, 1) == "air.threadgroup_position_in_grid") groupArg = i;
  }
  if (!meshInfo) return invalid("missing mesh type info");
  SM50_MSAA_MESH_LAYOUT meshLayout = options.mesh_layout_out ? *options.mesh_layout_out : SM50_MSAA_MESH_LAYOUT{};
  meshLayout.max_vertices = number(meshInfo, 3);
  meshLayout.max_primitives = number(meshInfo, 4);
  meshLayout.vertices_per_primitive = tag(meshInfo, 5) == "air.triangle" ? 3
      : tag(meshInfo, 5) == "air.line" ? 2 : 1;
  if (!meshLayout.threads_per_group) meshLayout.threads_per_group = 32;
  for (unsigned i = 3; i < metadata->getNumOperands(); ++i)
    if (auto *limit = dyn_cast<MDNode>(metadata->getOperand(i)); limit && tag(limit, 0) == "air.max_work_group_size")
      meshLayout.threads_per_group = number(limit, 1);
  if (!meshLayout.max_vertices || !meshLayout.max_primitives || !meshLayout.threads_per_group)
    return invalid("invalid output or thread limits");

  SmallVector<Field, 24> fields;
  unsigned position = ~0u;
  for (unsigned primitive = 0; primitive < 2; ++primitive) {
    auto *outputs = cast<MDTuple>(meshInfo->getOperand(1 + primitive));
    for (auto &operand : outputs->operands()) {
      auto *row = cast<MDNode>(operand);
      auto kind = tag(row, 0);
      if (kind == "air.primitive_culled") continue;
      auto *type = fieldType(row, module.getContext());
      if (!type) return invalid("unsupported output type " + kind);
      if (kind == "air.position") position = fields.size();
      fields.push_back({row, type, bool(primitive), kind,
        kind == "air.mesh_vertex_data" || kind == "air.mesh_primitive_data" ? number(row, 1) : ~0u});
    }
  }
  if (position == ~0u) return invalid("missing position");
  SmallVector<Type *, 24> types;
  SmallVector<Metadata *, 24> outputs;
  for (const auto &field : fields) {
    types.push_back(field.type);
    outputs.push_back(vertexMetadata(field, module.getContext()));
  }
  auto *native = StructType::get(module.getContext(), types, true);
  auto *structure = module.getDataLayout().getStructLayout(native);
  const unsigned stride = structure->getSizeInBytes() / 4;
  if (!stride || structure->getSizeInBytes() % 4) return invalid("output is not dword aligned");
  for (unsigned i = 0; i < fields.size(); ++i) fields[i].offset = structure->getElementOffset(i) / 4;
  const unsigned outputWords = meshLayout.max_primitives * meshLayout.vertices_per_primitive * stride;
  const unsigned vertexWords = meshLayout.max_vertices * stride;
  const unsigned primitiveWords = meshLayout.max_primitives * stride;
  const unsigned indexWords = meshLayout.max_primitives * meshLayout.vertices_per_primitive;
  unsigned scratchWords = vertexWords + primitiveWords + indexWords + 1 + meshLayout.max_primitives;
  SmallVector<std::pair<GlobalVariable *, unsigned>, 8> stagedOutputs;
  for (auto &global : module.globals()) {
    if (global.getAddressSpace() != 3 ||
        !(global.getName().startswith("dxmt.mesh.vertex.") ||
          global.getName().startswith("dxmt.mesh.primitive."))) continue;
    const uint64_t bytes = module.getDataLayout().getTypeAllocSize(global.getValueType()).getFixedValue();
    if ((bytes & 3) || bytes / 4 > UINT32_MAX - scratchWords)
      return invalid("staged mesh output exceeds capture record format");
    stagedOutputs.push_back({&global, scratchWords});
    scratchWords += bytes / 4;
  }
  meshLayout.capture_dwords_per_group = outputWords + scratchWords;
  if (options.mesh_layout_out) *options.mesh_layout_out = meshLayout;

  // Construct an ordinary packed vertex signature; transformVertex supplies captured replay and layout.
  auto buildReplay = [&]() -> Error {
    air::FunctionSignatureBuilder signature;
    signature.DefineInput(air::InputVertexID{});
    signature.DefineInput(air::InputBaseVertex{});
    signature.DefineInput(air::InputInstanceID{});
    signature.DefineInput(air::InputBaseInstance{});
    auto [dummy, dummyMetadata] = signature.CreateFunction("msaa_mesh_layout", module.getContext(), module, 0, true);
    auto *entry = Function::Create(FunctionType::get(native, dummy->getFunctionType()->params(), false),
                                  Function::ExternalLinkage, original->getName() + ".replay", module);
    IRBuilder<> b(BasicBlock::Create(module.getContext(), "entry", entry));
    b.CreateRet(UndefValue::get(native));
    SmallVector<Metadata *, 3> vertex{ValueAsMetadata::get(entry), MDTuple::get(module.getContext(), outputs),
                                    dummyMetadata->getOperand(2)};
    module.getOrInsertNamedMetadata("air.vertex")->addOperand(MDTuple::get(module.getContext(), vertex));
    dummy->eraseFromParent();
    SM50_SHADER_D3D12_MSAA_VERTEX_DATA replay{};
    replay.variant = SM50_MSAA_VERTEX_REPLAY;
    replay.layout_out = options.layout_out;
    replay.line_mode = options.line_mode;
    replay.interpolation = options.interpolation;
    replay.depth_clip_enable = options.depth_clip_enable;
    replay.front_counter_clockwise = options.front_counter_clockwise;
    return transformVertex(module, replay, conservative);
  };
  if (auto error = buildReplay()) return error;
  if (options.layout_out) {
    for (unsigned field = 0; field < fields.size(); ++field) {
      if (!fields[field].primitive) continue;
      for (auto &operand : cast<MDNode>(outputs[field])->operands()) {
        auto *name = dyn_cast<MDString>(operand);
        if (!name || !name->getString().startswith("user(")) continue;
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char character : name->getString()) { hash ^= character; hash *= 1099511628211ull; }
        for (unsigned user = 0; user < options.layout_out->user_count; ++user)
          if (options.layout_out->users[user].hash == hash) options.layout_out->users[user].flags |= 2;
      }
    }
  }
  if (options.variant == SM50_MSAA_VERTEX_REPLAY) {
    // The independently compiled replay retains no application stage or object payload.
    auto name = original->getName().str();
    auto *vertices = module.getNamedMetadata("air.vertex");
    auto *replay = cast<Function>(cast<ValueAsMetadata>(vertices->getOperand(0)->getOperand(0))->getValue());
    meshes->eraseFromParent();
    original->dropAllReferences();
    original->eraseFromParent();
    replay->setName(name);
    return Error::success();
  }
  if (options.variant != SM50_MSAA_VERTEX_CAPTURE) return invalid("unknown variant");
  auto *vertices = module.getNamedMetadata("air.vertex");
  auto *replay = cast<Function>(cast<ValueAsMetadata>(vertices->getOperand(0)->getOperand(0))->getValue());
  vertices->eraseFromParent();
  replay->eraseFromParent();

  SmallVector<Type *, 24> parameters(original->getFunctionType()->param_begin(), original->getFunctionType()->param_end());
  const unsigned recordsArg = parameters.size();
  parameters.push_back(Type::getInt32Ty(module.getContext())->getPointerTo(1));
  unsigned controlArg = ~0u;
  for (unsigned i = 0; i < inputs->getNumOperands(); ++i) {
    auto *input = cast<MDNode>(inputs->getOperand(i));
    if (tag(input, 1) != "air.buffer") continue;
    for (unsigned j = 2; j + 1 < input->getNumOperands(); ++j)
      if (tag(input, j) == "air.location_index" && number(input, j + 1) == 13) controlArg = i;
  }
  const bool addControl = controlArg == ~0u;
  if (addControl) {
    controlArg = parameters.size();
    parameters.push_back(Type::getInt32Ty(module.getContext())->getPointerTo(2));
  }
  air::FunctionSignatureBuilder builtins;
  const bool addThread = threadArg == ~0u, addGroup = groupArg == ~0u;
  if (addThread) {
    threadArg = parameters.size();
    parameters.push_back(Type::getInt32Ty(module.getContext()));
    builtins.DefineInput(air::InputThreadIndexInThreadgroup{});
  }
  if (addGroup) {
    groupArg = parameters.size();
    parameters.push_back(FixedVectorType::get(Type::getInt32Ty(module.getContext()), 3));
    builtins.DefineInput(air::InputThreadgroupPositionInGrid{});
  }
  auto oldName = original->getName().str();
  original->setName(oldName + ".original");
  auto *entry = Function::Create(FunctionType::get(Type::getVoidTy(module.getContext()), parameters, false),
                                Function::ExternalLinkage, oldName, module);
  ValueToValueMapTy mapping;
  for (unsigned i = 0; i < original->arg_size(); ++i) mapping[original->getArg(i)] = entry->getArg(i);
  SmallVector<ReturnInst *, 8> returns;
  CloneFunctionInto(entry, original, mapping, CloneFunctionChangeType::LocalChangesOnly, returns);
  SmallVector<Metadata *, 24> inputMetadata(inputs->op_begin(), inputs->op_end());
  inputMetadata.push_back(air::BuildBufferArgumentMetadata(recordsArg, binding(SM50_BINDING_INDEX_MSAA_VERTEX_RECORDS, false, "msaa_mesh_records"),
                                                          module.getContext(), module.getDataLayout()));
  // The capture function emits no native mesh. Keep only a legal position schema;
  // application user outputs live in device records, not Metal's 124-scalar mesh interface.
  for (unsigned i = 0; i < inputs->getNumOperands(); ++i) {
    auto *input = cast<MDNode>(inputs->getOperand(i));
    if (tag(input, 1) != "air.mesh") continue;
    SmallVector<Metadata *, 8> minimal(meshInfo->op_begin(), meshInfo->op_end());
    minimal[1] = MDTuple::get(module.getContext(), {fields[position].metadata});
    minimal[2] = MDTuple::get(module.getContext(), {});
    SmallVector<Metadata *, 8> argument(input->op_begin(), input->op_end());
    argument[2] = MDTuple::get(module.getContext(), minimal);
    inputMetadata[i] = MDTuple::get(module.getContext(), argument);
  }
  if (addControl)
    inputMetadata.push_back(air::BuildBufferArgumentMetadata(controlArg, binding(13, true, "msaa_mesh_control"),
                                                            module.getContext(), module.getDataLayout()));
  if (addThread || addGroup) {
    auto [dummy, md] = builtins.CreateFunction("msaa_mesh_builtins", module.getContext(), module, 0, true);
    auto *values = cast<MDTuple>(md->getOperand(2));
    for (auto &operand : values->operands()) {
      auto *node = cast<MDNode>(operand);
      SmallVector<Metadata *, 12> data(node->op_begin(), node->op_end());
      data[0] = ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(module.getContext()), inputMetadata.size()));
      inputMetadata.push_back(MDTuple::get(module.getContext(), data));
    }
    dummy->eraseFromParent();
  }
  SmallVector<Metadata *, 8> updated(metadata->op_begin(), metadata->op_end());
  updated[0] = ValueAsMetadata::get(entry);
  updated[2] = MDTuple::get(module.getContext(), inputMetadata);
  meshes->setOperand(0, MDTuple::get(module.getContext(), updated));

  CallInst *slotMarker = nullptr;
  SmallVector<CallInst *, 64> stores;
  for (auto &block : *entry)
    for (auto &instruction : block)
      if (auto *call = dyn_cast<CallInst>(&instruction); call && call->getCalledFunction()) {
        auto name = call->getCalledFunction()->getName();
        if (name == "dxmt.msaa.mesh.slot") slotMarker = call;
        if (name.startswith("air.set_") && name.contains("_mesh")) stores.push_back(call);
      }
  IRBuilder<> b(slotMarker ? slotMarker : &*entry->getEntryBlock().getFirstInsertionPt());
  auto loadControl = [&](unsigned word) {
    return b.CreateLoad(b.getInt32Ty(), b.CreateConstGEP1_32(b.getInt32Ty(), entry->getArg(controlArg), word));
  };
  auto *groupsX = loadControl(0), *groupsY = loadControl(1), *base = loadControl(2), *total = loadControl(3);
  Value *group;
  if (slotMarker) {
    group = slotMarker->getArgOperand(0);
  } else {
    auto *positionValue = entry->getArg(groupArg);
    group = b.CreateAdd(b.CreateExtractElement(positionValue, uint64_t(0)), b.CreateMul(groupsX,
      b.CreateAdd(b.CreateExtractElement(positionValue, 1), b.CreateMul(groupsY, b.CreateExtractElement(positionValue, 2)))));
    group = b.CreateAdd(group, base);
  }
  auto *records = entry->getArg(recordsArg);
  auto wordPointer = [&](Value *word) { return b.CreateGEP(b.getInt32Ty(), records, b.CreateZExt(word, b.getInt64Ty())); };
  auto *outputBase = b.CreateMul(group, b.getInt32(outputWords));
  auto *scratchBase = b.CreateAdd(b.CreateMul(total, b.getInt32(outputWords)), b.CreateMul(group, b.getInt32(scratchWords)));
  auto *countWord = b.CreateAdd(scratchBase, b.getInt32(vertexWords + primitiveWords + indexWords));
  // Generated scattered output stores belong in per-group device scratch, not
  // the application's threadgroup budget. Keep app TG globals.
  for (const auto &[global, offset] : stagedOutputs) {
    auto *device = b.CreateBitCast(wordPointer(b.CreateAdd(scratchBase, b.getInt32(offset))),
      global->getValueType()->getPointerTo(1));
    SmallVector<GetElementPtrInst *, 16> addresses;
    for (auto &instruction : instructions(*entry))
      if (auto *gep = dyn_cast<GetElementPtrInst>(&instruction); gep && gep->getPointerOperand() == global)
        addresses.push_back(gep);
    for (auto *old : addresses) {
      IRBuilder<> replace(old);
      SmallVector<Value *, 4> indices(old->idx_begin(), old->idx_end());
      auto *address = replace.CreateGEP(old->getSourceElementType(), device, indices);
      for (auto &use : make_early_inc_range(old->uses())) {
        auto *load = dyn_cast<LoadInst>(use.getUser());
        auto *store = dyn_cast<StoreInst>(use.getUser());
        if ((!load || use.getOperandNo() != 0) && (!store || use.getOperandNo() != 1))
          return invalid("generated mesh staging pointer escapes scalar load/store");
        use.set(address);
        if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
          cast<Instruction>(use.getUser())->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
      }
      old->eraseFromParent();
    }
  }
  Value *thread = entry->getArg(threadArg);
  if (!stagedOutputs.empty())
    for (auto &instruction : instructions(*entry))
      if (auto *call = dyn_cast<CallInst>(&instruction); call && call->getMetadata("dxmt.mesh.output_barrier")) {
        auto *flags = cast<ConstantInt>(call->getArgOperand(0));
        call->setArgOperand(0, b.getInt32(flags->getZExtValue() | unsigned(llvm::air::MemFlags::Device)));
        call->setMetadata("dxmt.mesh.output_barrier", nullptr);
      }
  if (slotMarker) { b.ClearInsertionPoint(); slotMarker->eraseFromParent(); }

  // Runtime clears scratch before launch; untouched count/cull flags remain zero.
  for (auto *call : stores) {
    b.SetInsertPoint(call);
    auto name = call->getCalledFunction()->getName();
    Value *offset = nullptr, *value = nullptr;
    if (name == "air.set_primitive_count_mesh") {
      // SetMeshOutputCounts is group-uniform, while GS/DS may publish from one lane.
      offset = countWord;
      value = call->getArgOperand(1);
    } else if (name == "air.set_index_mesh") {
      offset = b.CreateAdd(scratchBase, b.CreateAdd(b.getInt32(vertexWords + primitiveWords), call->getArgOperand(1)));
      value = b.CreateZExt(call->getArgOperand(2), b.getInt32Ty());
    } else if (name == "air.set_primitive_culled_mesh") {
      offset = b.CreateAdd(countWord, b.CreateAdd(b.getInt32(1), call->getArgOperand(1)));
      value = b.CreateZExt(call->getArgOperand(2), b.getInt32Ty());
    } else {
      const bool data = name.startswith("air.set_vertex_data_mesh") || name.startswith("air.set_primitive_data_mesh");
      const bool primitive = name.startswith("air.set_primitive_data_mesh") || name.contains("array_index_mesh") ||
                             name == "air.set_primitive_id_mesh";
      StringRef kind = name == "air.set_position_mesh" ? "air.position"
          : name == "air.set_clip_distance_mesh" ? "air.clip_distance"
          : name == "air.set_point_size_mesh" ? "air.point_size"
          : name.startswith("air.set_render_target_array_index_mesh") ? "air.render_target_array_index"
          : name.startswith("air.set_viewport_array_index_mesh") ? "air.viewport_array_index"
          : name == "air.set_primitive_id_mesh" ? "air.primitive_id"
          : primitive ? "air.mesh_primitive_data" : "air.mesh_vertex_data";
      unsigned dataIndex = data ? cast<ConstantInt>(call->getArgOperand(1))->getZExtValue() : ~0u;
      auto found = llvm::find_if(fields, [&](const Field &f) { return f.kind == kind && f.primitive == primitive && f.data_index == dataIndex; });
      if (found == fields.end()) return invalid("unmatched output intrinsic " + name);
      auto *index = call->getArgOperand(data || kind == "air.clip_distance" ? 2 : 1);
      offset = b.CreateAdd(scratchBase, b.CreateAdd(b.getInt32((primitive ? vertexWords : 0) + found->offset),
                                                  b.CreateMul(index, b.getInt32(stride))));
      if (kind == "air.clip_distance") offset = b.CreateAdd(offset, call->getArgOperand(1));
      value = call->getArgOperand(call->arg_size() - 1);
    }
    Value *active = b.CreateICmpULT(group, total);
    if (name == "air.set_primitive_count_mesh")
      active = b.CreateAnd(active, b.CreateICmpEQ(thread, b.getInt32(0)));
    auto *write = SplitBlockAndInsertIfThen(active, call, false);
    b.SetInsertPoint(write);
    auto *pointer = b.CreateBitCast(wordPointer(offset), value->getType()->getPointerTo(1));
    auto *store = b.CreateAlignedStore(value, pointer, Align(4));
    if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
      store->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
    call->eraseFromParent();
  }
  for (auto *ret : returns) {
    b.SetInsertPoint(ret);
    llvm::raw_null_ostream debug;
    llvm::air::AIRBuilder air({}, b, debug);
    air.CreateBarrier(static_cast<llvm::air::MemFlags>(3));
    auto *head = ret->getParent();
    auto *loop = BasicBlock::Create(module.getContext(), "capture_primitives", entry);
    auto *body = BasicBlock::Create(module.getContext(), "capture_primitive", entry);
    auto *done = BasicBlock::Create(module.getContext(), "capture_done", entry);
    b.CreateCondBr(b.CreateICmpULT(group, total), loop, done);
    ret->eraseFromParent();
    b.SetInsertPoint(loop);
    auto *primitive = b.CreatePHI(b.getInt32Ty(), 2);
    primitive->addIncoming(thread, head);
    b.CreateCondBr(b.CreateICmpULT(primitive, b.getInt32(meshLayout.max_primitives)), body, done);
    b.SetInsertPoint(body);
    auto *count = b.CreateLoad(b.getInt32Ty(), wordPointer(countWord));
    auto *culled = b.CreateLoad(b.getInt32Ty(), wordPointer(b.CreateAdd(countWord, b.CreateAdd(primitive, b.getInt32(1)))));
    Value *live = b.CreateAnd(b.CreateICmpULT(primitive, count), b.CreateICmpEQ(culled, b.getInt32(0)));
    SmallVector<Value *, 3> results;
    for (unsigned v = 0; v < meshLayout.vertices_per_primitive; ++v) {
      auto *ordinal = b.CreateAdd(b.CreateMul(primitive, b.getInt32(meshLayout.vertices_per_primitive)), b.getInt32(v));
      auto *vertex = b.CreateLoad(b.getInt32Ty(), wordPointer(b.CreateAdd(scratchBase,
        b.CreateAdd(b.getInt32(vertexWords + primitiveWords), ordinal))));
      Value *result = UndefValue::get(native);
      for (unsigned f = 0; f < fields.size(); ++f) {
        const auto &field = fields[f];
        auto *word = b.CreateAdd(scratchBase, b.CreateAdd(b.getInt32((field.primitive ? vertexWords : 0) + field.offset),
          b.CreateMul(field.primitive ? static_cast<Value *>(primitive) : vertex, b.getInt32(stride))));
        auto *value = b.CreateAlignedLoad(field.type, b.CreateBitCast(wordPointer(word), field.type->getPointerTo(1)), Align(4));
        if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
          value->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
        result = b.CreateInsertValue(result, value, f);
      }
      results.push_back(result);
    }
    for (unsigned f = 0; f < fields.size(); ++f) {
      const auto &field = fields[f];
      bool cullDistance = false;
      for (auto &operand : field.metadata->operands())
        if (auto *name = dyn_cast<MDString>(operand); name &&
            name->getString().startswith("user(semantic_sv_culldistance_")) cullDistance = true;
      if (!cullDistance) continue;
      auto *vector = dyn_cast<FixedVectorType>(field.type);
      if (!vector || !vector->getElementType()->isFloatTy()) return invalid("cull distance must be float vector");
      for (unsigned component = 0; component < vector->getNumElements(); ++component) {
        Value *outside = b.getTrue();
        for (auto *result : results) {
          auto *distance = b.CreateExtractElement(b.CreateExtractValue(result, f), component);
          outside = b.CreateAnd(outside, b.CreateFCmpOLT(distance, ConstantFP::get(distance->getType(), 0)));
        }
        live = b.CreateAnd(live, b.CreateNot(outside));
      }
    }
    for (unsigned v = 0; v < meshLayout.vertices_per_primitive; ++v) {
      auto *ordinal = b.CreateAdd(b.CreateMul(primitive, b.getInt32(meshLayout.vertices_per_primitive)), b.getInt32(v));
      auto *result = results[v];
      // Missing or culled primitives have zero W and are discarded before bounds/rasterization.
      auto *positionValue = b.CreateExtractValue(result, position);
      result = b.CreateInsertValue(result, b.CreateSelect(live, positionValue, Constant::getNullValue(positionValue->getType())), position);
      auto *out = b.CreateAdd(outputBase, b.CreateMul(ordinal, b.getInt32(stride)));
      auto *store = b.CreateAlignedStore(result, b.CreateBitCast(wordPointer(out), native->getPointerTo(1)), Align(4));
      if (module.getModuleFlag(dxil::kAtomic64PhaseFlag))
        store->setMetadata(dxil::kAtomic64Metadata, MDNode::get(module.getContext(), {}));
    }
    auto *next = b.CreateAdd(primitive, b.getInt32(meshLayout.threads_per_group));
    b.CreateBr(loop);
    primitive->addIncoming(next, body);
    b.SetInsertPoint(done);
    b.CreateRetVoid();
  }
  original->dropAllReferences();
  original->eraseFromParent();
  return Error::success();
}
} // namespace dxmt::msaa
