#include "dxil_converter.hpp"
#include "airconv_context.hpp"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

namespace dxmt::dxil {
using namespace llvm;
namespace {
constexpr const char *helperAttribute = "dxmt.atomic64.helper";

FunctionCallee nativeHelper(Module &module, StringRef name, ArrayRef<Type *> arguments) {
  return module.getOrInsertFunction(name, FunctionType::get(Type::getInt64Ty(module.getContext()), arguments, false));
}

// Elect around the complete operation, not its individual read/CAS calls. If
// every lane first reads the same root, serializing only CAS still makes the
// remaining lanes repeat an increasingly stale update. No pending lane holds
// a protected node while the elected lane executes its full retry sequence.
void cooperateAtomic64(Function &function, ReturnInst &returned) {
  auto &ctx = function.getContext();
  auto &module = *function.getParent();
  IRBuilder<> builder(ctx);
  llvm::air::AIRBuilder air(builder, llvm::nulls());
  auto *i1 = builder.getInt1Ty();
  auto *i32 = builder.getInt32Ty();
  auto *i64 = builder.getInt64Ty();
  auto attributes = AttributeList::get(ctx,
      {{~0U, Attribute::get(ctx, Attribute::Convergent)},
       {~0U, Attribute::get(ctx, Attribute::NoUnwind)},
       {~0U, Attribute::get(ctx, Attribute::WillReturn)}});
  auto ballot = module.getOrInsertFunction("air.simd_ballot.i64",
      FunctionType::get(i64, {i1}, false), attributes);
  auto lane_index = module.getOrInsertFunction("air.get_thread_index_in_simdgroup.i32",
      FunctionType::get(i32, false), attributes);
  auto *body = &function.getEntryBlock();
  auto *head = BasicBlock::Create(ctx, "atomic64.cooperative", &function, body);
  auto *vote = BasicBlock::Create(ctx, "atomic64.vote", &function, body);
  auto *elected = BasicBlock::Create(ctx, "atomic64.elected", &function, body);
  auto *next = BasicBlock::Create(ctx, "atomic64.next", &function);
  auto *done = BasicBlock::Create(ctx, "atomic64.done", &function);
  function.addFnAttr(Attribute::Convergent);
  builder.SetInsertPoint(head);
  auto *lane = builder.CreateCall(lane_index);
  builder.CreateBr(vote);
  builder.SetInsertPoint(vote);
  auto *pending = builder.CreatePHI(i1, 2);
  auto *result = builder.CreatePHI(i64, 2);
  pending->addIncoming(builder.getTrue(), head);
  result->addIncoming(builder.getInt64(0), head);
  auto *mask = builder.CreateTrunc(builder.CreateCall(ballot, {pending}), i32);
  auto *leader = air.CreateCountZero(mask, true);
  auto *mine = builder.CreateAnd(pending, builder.CreateICmpEQ(lane, leader));
  builder.CreateCondBr(builder.CreateICmpNE(mask, builder.getInt32(0)), elected, done);
  builder.SetInsertPoint(elected);
  builder.CreateCondBr(mine, body, next);
  auto *completed = returned.getParent();
  auto *value = returned.getReturnValue();
  builder.SetInsertPoint(&returned);
  builder.CreateBr(next);
  returned.eraseFromParent();
  builder.SetInsertPoint(next);
  auto *still_pending = builder.CreatePHI(i1, 2);
  auto *updated = builder.CreatePHI(i64, 2);
  still_pending->addIncoming(builder.getFalse(), completed);
  still_pending->addIncoming(pending, elected);
  updated->addIncoming(value, completed);
  updated->addIncoming(result, elected);
  pending->addIncoming(still_pending, next);
  result->addIncoming(updated, next);
  builder.CreateBr(vote);
  builder.SetInsertPoint(done);
  builder.CreateRet(result);
}

// Expose application AIR32 buffer atomics to the same legalization as LLVM32
// atomics. Removing the old declarations also avoids mixing the Metal3.1 and
// Metal4.1 signatures when the SC32 lifetime helpers are linked.
Error exposeBufferAtomics(Module &module) {
  SmallVector<Function *, 16> declarations;
  for (auto &function : module)
    if (function.isDeclaration() && function.getName().startswith("air.atomic.global.") &&
        function.arg_size() == 5)
      declarations.push_back(&function);
  for (auto *function : declarations) {
    auto name = function->getName();
    AtomicRMWInst::BinOp operation;
    if (name.startswith("air.atomic.global.add.")) operation = AtomicRMWInst::Add;
    else if (name.startswith("air.atomic.global.sub.")) operation = AtomicRMWInst::Sub;
    else if (name.startswith("air.atomic.global.and.")) operation = AtomicRMWInst::And;
    else if (name.startswith("air.atomic.global.or.")) operation = AtomicRMWInst::Or;
    else if (name.startswith("air.atomic.global.xor.")) operation = AtomicRMWInst::Xor;
    else if (name.startswith("air.atomic.global.xchg.")) operation = AtomicRMWInst::Xchg;
    else if (name.startswith("air.atomic.global.min.s.")) operation = AtomicRMWInst::Min;
    else if (name.startswith("air.atomic.global.min.u.")) operation = AtomicRMWInst::UMin;
    else if (name.startswith("air.atomic.global.max.s.")) operation = AtomicRMWInst::Max;
    else if (name.startswith("air.atomic.global.max.u.")) operation = AtomicRMWInst::UMax;
    else return createError("atomic64 phase: unknown application AIR buffer atomic " + name);
    SmallVector<CallInst *, 16> calls;
    for (auto *user : function->users())
      if (auto *call = dyn_cast<CallInst>(user)) calls.push_back(call);
    for (auto *call : calls) {
      IRBuilder<> builder(call);
      auto *replacement = builder.CreateAtomicRMW(operation, call->getArgOperand(0), call->getArgOperand(1),
        MaybeAlign(), AtomicOrdering::Monotonic);
      replacement->copyMetadata(*call);
      if (auto *isVolatile = dyn_cast<ConstantInt>(call->getArgOperand(4)))
        replacement->setVolatile(!isVolatile->isZero());
      call->replaceAllUsesWith(replacement);
      call->eraseFromParent();
    }
    if (function->use_empty()) function->eraseFromParent();
  }
  return Error::success();
}

Value *scalarBits(IRBuilderBase &builder, Value *value) {
  auto *type = value->getType();
  return type->isIntegerTy() ? value : builder.CreateBitCast(value, IntegerType::get(builder.getContext(), type->getPrimitiveSizeInBits()));
}

Value *bitsScalar(IRBuilderBase &builder, Value *value, Type *type) {
  return type->isIntegerTy() ? value : builder.CreateBitCast(value, type);
}

// A binary search is unnecessary for the small dynamic resource set. Unmapped
// scratch/counter resources retain their normal native operations; a mapped sparse
// hole yields a null root and therefore D3D12 zero-read/drop-write semantics.
Value *findRawRoot(IRBuilderBase &builder, Value *pointer) {
  auto *module = builder.GetInsertBlock()->getModule();
  auto *context = EmitAtomic64Context(builder);
  auto *i32 = builder.getInt32Ty();
  auto *i64 = builder.getInt64Ty();
  auto *rootType = PointerType::get(i32, 1);
  auto loadField = [&](uint32_t offset, Type *type) {
    auto *address = builder.CreateInBoundsGEP(builder.getInt8Ty(), context, builder.getInt64(offset));
    return builder.CreateLoad(type, builder.CreateBitCast(address, PointerType::get(type, 1)));
  };
  auto *ranges = builder.CreateIntToPtr(loadField(0, i64), PointerType::get(builder.getInt8Ty(), 1));
  auto *roots = builder.CreateIntToPtr(loadField(16, i64), rootType);
  auto *count = loadField(40, i32);
  auto *address = builder.CreatePtrToInt(pointer, i64);
  auto *start = builder.GetInsertBlock();
  auto *tail = start->splitBasicBlock(builder.GetInsertPoint(), "atomic64.raw.tail");
  start->getTerminator()->eraseFromParent();
  auto *loop = BasicBlock::Create(module->getContext(), "atomic64.raw.range", start->getParent(), tail);
  auto *test = BasicBlock::Create(module->getContext(), "atomic64.raw.test", start->getParent(), tail);
  auto *found = BasicBlock::Create(module->getContext(), "atomic64.raw.found", start->getParent(), tail);
  auto *next = BasicBlock::Create(module->getContext(), "atomic64.raw.next", start->getParent(), tail);
  builder.SetInsertPoint(start);
  builder.CreateBr(loop);
  builder.SetInsertPoint(loop);
  auto *index = builder.CreatePHI(i32, 2);
  index->addIncoming(builder.getInt32(0), start);
  builder.CreateCondBr(builder.CreateICmpULT(index, count), test, tail);
  builder.SetInsertPoint(test);
  auto *range = builder.CreateInBoundsGEP(builder.getInt8Ty(), ranges, builder.CreateMul(builder.CreateZExt(index, i64), builder.getInt64(24)));
  auto readRange = [&](unsigned offset) {
    auto *at = builder.CreateInBoundsGEP(builder.getInt8Ty(), range, builder.getInt64(offset));
    return builder.CreateLoad(i64, builder.CreateBitCast(at, PointerType::get(i64, 1)));
  };
  auto *begin = readRange(0);
  auto *length = readRange(8);
  auto *relative = builder.CreateSub(address, begin);
  builder.CreateCondBr(builder.CreateAnd(builder.CreateICmpUGE(address, begin), builder.CreateICmpULT(relative, length)), found, next);
  builder.SetInsertPoint(found);
  auto *map = builder.CreateIntToPtr(readRange(16), rootType);
  auto *cell = builder.CreateLoad(i32, builder.CreateInBoundsGEP(i32, map, builder.CreateLShr(relative, builder.getInt64(3))));
  auto *mapped = builder.CreateSelect(builder.CreateICmpEQ(cell, builder.getInt32(SM50_ATOMIC64_UNMAPPED_CELL)),
    ConstantPointerNull::get(rootType), builder.CreateInBoundsGEP(i32, roots, cell));
  builder.CreateBr(tail);
  builder.SetInsertPoint(next);
  auto *following = builder.CreateAdd(index, builder.getInt32(1));
  index->addIncoming(following, next);
  builder.CreateBr(loop);
  builder.SetInsertPoint(tail, tail->getFirstInsertionPt());
  // UINT64_MAX sentinel distinguishes no phase mapping from a mapped sparse hole.
  auto *result = builder.CreatePHI(rootType, 2);
  result->addIncoming(ConstantExpr::getIntToPtr(ConstantInt::get(i64, ~uint64_t(0)), rootType), loop);
  result->addIncoming(mapped, found);
  return result;
}

Value *nativeOrCell(IRBuilderBase &builder, Value *pointer, function_ref<Value *()> native,
                    function_ref<Value *(Value *)> cell) {
  auto *root = findRawRoot(builder, pointer);
  auto *start = builder.GetInsertBlock();
  auto *tail = start->splitBasicBlock(builder.GetInsertPoint(), "atomic64.access.tail");
  start->getTerminator()->eraseFromParent();
  auto *physical = BasicBlock::Create(builder.getContext(), "atomic64.access.native", start->getParent(), tail);
  auto *canonical = BasicBlock::Create(builder.getContext(), "atomic64.access.cell", start->getParent(), tail);
  builder.SetInsertPoint(start);
  auto *notMapped = builder.CreateICmpEQ(builder.CreatePtrToInt(root, builder.getInt64Ty()), builder.getInt64(~uint64_t(0)));
  builder.CreateCondBr(notMapped, physical, canonical);
  builder.SetInsertPoint(physical);
  auto *nativeValue = native();
  auto *physicalEnd = builder.GetInsertBlock();
  builder.CreateBr(tail);
  builder.SetInsertPoint(canonical);
  auto *cellValue = cell(root);
  auto *canonicalEnd = builder.GetInsertBlock();
  builder.CreateBr(tail);
  builder.SetInsertPoint(tail, tail->getFirstInsertionPt());
  if (!nativeValue) return nullptr;
  auto *value = builder.CreatePHI(nativeValue->getType(), 2);
  value->addIncoming(nativeValue, physicalEnd);
  value->addIncoming(cellValue, canonicalEnd);
  return value;
}
}

Value *EmitAtomic64Context(IRBuilderBase &builder) {
  auto *module = builder.GetInsertBlock()->getModule();
  auto *global = module->getNamedGlobal(kAtomic64ContextGlobal);
  if (!global) global = new GlobalVariable(*module, builder.getInt64Ty(), true,
    GlobalValue::LinkOnceODRLinkage, builder.getInt64(0), kAtomic64ContextGlobal, nullptr,
    GlobalValue::NotThreadLocal, 2);
  // Every linked application stage shares the same exclusive phase context.
  // Internal linkage would suffix duplicate slots and lose their relocation.
  global->setLinkage(GlobalValue::LinkOnceODRLinkage);
  // Exactly as for the existing lock-table relocation: optimization must not
  // replace the runtime-patched address by its zero initializer.
  global->setExternallyInitialized(true);
  global->setAlignment(Align(8));
  return builder.CreateIntToPtr(builder.CreateLoad(builder.getInt64Ty(), global), PointerType::get(builder.getInt8Ty(), 1));
}

void InitializeAtomic64Phase(Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *arguments) {
  for (auto *argument = arguments; argument;
       argument = static_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(argument->next))
    if (argument->type == SM50_SHADER_D3D12_ATOMIC64_PHASE) {
      if (!module.getModuleFlag(kAtomic64PhaseFlag)) module.addModuleFlag(Module::Warning, kAtomic64PhaseFlag, 1);
      break;
    }
}

bool HasDeviceAtomic64(const Module &module) {
  for (const auto &function : module)
    for (const auto &instruction : instructions(function)) {
      if (const auto *call = dyn_cast<CallInst>(&instruction))
        if (const auto *callee = call->getCalledFunction(); callee && call->getType()->isIntegerTy(64))
          if (callee->getName().startswith("dx.op.atomicBinOp.") ||
              callee->getName().startswith("dx.op.atomicCompareExchange.")) return true;
      if (const auto *atomic = dyn_cast<AtomicRMWInst>(&instruction))
        if ((atomic->getPointerAddressSpace() == 1 || atomic->getPointerAddressSpace() == 6) &&
            atomic->getValOperand()->getType()->isIntegerTy(64)) return true;
      if (const auto *atomic = dyn_cast<AtomicCmpXchgInst>(&instruction))
        if ((atomic->getPointerAddressSpace() == 1 || atomic->getPointerAddressSpace() == 6) &&
            atomic->getCompareOperand()->getType()->isIntegerTy(64)) return true;
    }
  return false;
}

Value *EmitAtomic64CellRead(IRBuilderBase &builder, Value *root) {
  auto *context = EmitAtomic64Context(builder);
  auto &module = *builder.GetInsertBlock()->getModule();
  auto *function = module.getFunction("dxmt.atomic64.read");
  if (!function) {
    function = Function::Create(FunctionType::get(builder.getInt64Ty(),
        {context->getType(), root->getType()}, false), GlobalValue::InternalLinkage,
        "dxmt.atomic64.read", module);
    auto *entry = BasicBlock::Create(builder.getContext(), "entry", function);
    IRBuilder<> body(entry);
    auto reader = nativeHelper(module, "dxmt_atomic64_read", {context->getType(), root->getType()});
    auto *value = body.CreateCall(reader, {function->getArg(0), function->getArg(1)});
    body.CreateRet(value);
    // Reads remain available to helper lanes; only updates carry helperAttribute.
  }
  auto *call = builder.CreateCall(function, {context, root});
  call->setMetadata(kAtomic64Metadata, MDNode::get(builder.getContext(), {}));
  return call;
}

Value *EmitAtomic64CellUpdate(IRBuilderBase &builder, Value *root,
                            function_ref<Value *(Value *)> update) {
  auto *module = builder.GetInsertBlock()->getModule();
  auto *context = EmitAtomic64Context(builder);
  auto callerPoint = builder.saveIP();
  auto *temporary = Function::Create(FunctionType::get(builder.getInt64Ty(),
    {context->getType(), root->getType()}, false), GlobalValue::InternalLinkage, "dxmt.atomic64.update.tmp", *module);
  temporary->addFnAttr(helperAttribute);
  auto *entry = BasicBlock::Create(builder.getContext(), "entry", temporary);
  auto *loop = BasicBlock::Create(builder.getContext(), "retry", temporary);
  auto *done = BasicBlock::Create(builder.getContext(), "done", temporary);
  builder.SetInsertPoint(entry);
  auto reader = nativeHelper(*module, "dxmt_atomic64_read", {context->getType(), root->getType()});
  auto cas = nativeHelper(*module, "dxmt_atomic64_compare_exchange", {context->getType(), root->getType(), builder.getInt64Ty(), builder.getInt64Ty()});
  auto *initial = builder.CreateCall(reader, {temporary->getArg(0), temporary->getArg(1)});
  builder.CreateBr(loop);
  builder.SetInsertPoint(loop);
  auto *old = builder.CreatePHI(builder.getInt64Ty(), 2);
  old->addIncoming(initial, entry);
  auto *desired = update(old);
  auto *observed = builder.CreateCall(cas, {temporary->getArg(0), temporary->getArg(1), old, desired});
  old->addIncoming(observed, builder.GetInsertBlock());
  builder.CreateCondBr(builder.CreateICmpEQ(observed, old), done, loop);
  builder.SetInsertPoint(done);
  builder.CreateRet(old);

  // Capture only external SSA operands of the callback. The resulting one call
  // is an application effect, allowing sample emulation to guard the entire retry
  // operation instead of disabling publication inside an otherwise live loop.
  SmallVector<Value *, 8> captures;
  SmallPtrSet<Value *, 8> seen;
  for (auto &instruction : instructions(temporary))
    for (auto &operand : instruction.operands()) {
      auto *value = operand.get();
      bool external = false;
      if (auto *argument = dyn_cast<Argument>(value)) external = argument->getParent() != temporary;
      if (auto *definition = dyn_cast<Instruction>(value)) external = definition->getFunction() != temporary;
      if (external && seen.insert(value).second) captures.push_back(value);
    }
  SmallVector<Type *, 10> parameters{context->getType(), root->getType()};
  for (auto *value : captures) parameters.push_back(value->getType());
  auto *function = Function::Create(FunctionType::get(builder.getInt64Ty(), parameters, false),
    GlobalValue::InternalLinkage, "dxmt.atomic64.update", *module);
  function->addFnAttr(helperAttribute);
  function->getBasicBlockList().splice(function->end(), temporary->getBasicBlockList());
  temporary->getArg(0)->replaceAllUsesWith(function->getArg(0));
  temporary->getArg(1)->replaceAllUsesWith(function->getArg(1));
  for (auto &instruction : instructions(function))
    for (auto &operand : instruction.operands())
      for (unsigned i = 0; i < captures.size(); ++i)
        if (operand.get() == captures[i]) { operand.set(function->getArg(i + 2)); break; }
  temporary->eraseFromParent();
  builder.restoreIP(callerPoint);
  SmallVector<Value *, 10> arguments{context, root};
  arguments.append(captures.begin(), captures.end());
  return builder.CreateCall(function, arguments);
}

Error LowerAtomic64Phase(Module &module) {
  if (!module.getModuleFlag(kAtomic64PhaseFlag)) return Error::success();
  if (auto error = exposeBufferAtomics(module)) return error;
  SmallVector<Instruction *, 64> accesses;
  for (auto &function : module) {
    if (function.hasFnAttribute(helperAttribute) || function.getName().startswith("dxmt_atomic64_")) continue;
    for (auto &instruction : instructions(function)) {
      if (instruction.getMetadata(kAtomic64Metadata)) continue;
      if (auto *load = dyn_cast<LoadInst>(&instruction); load && load->getPointerAddressSpace() == 1)
        accesses.push_back(load);
      else if (auto *store = dyn_cast<StoreInst>(&instruction); store && store->getPointerAddressSpace() == 1)
        accesses.push_back(store);
      else if (auto *atomic = dyn_cast<AtomicRMWInst>(&instruction); atomic && atomic->getPointerAddressSpace() == 1)
        accesses.push_back(atomic);
      else if (auto *atomic = dyn_cast<AtomicCmpXchgInst>(&instruction); atomic && atomic->getPointerAddressSpace() == 1)
        accesses.push_back(atomic);
    }
  }
  for (auto *instruction : accesses) {
    IRBuilder<> builder(instruction);
    auto *load = dyn_cast<LoadInst>(instruction);
    auto *store = dyn_cast<StoreInst>(instruction);
    auto *rmw = dyn_cast<AtomicRMWInst>(instruction);
    auto *cas = dyn_cast<AtomicCmpXchgInst>(instruction);
    auto *pointer = load ? load->getPointerOperand() : store ? store->getPointerOperand() :
      rmw ? rmw->getPointerOperand() : cas->getPointerOperand();
    auto *type = load ? load->getType() : store ? store->getValueOperand()->getType() :
      rmw ? rmw->getValOperand()->getType() : cas->getCompareOperand()->getType();
    auto *vector = dyn_cast<FixedVectorType>(type);
    auto *scalar = vector ? vector->getElementType() : type;
    const unsigned scalarWidth = scalar->getPrimitiveSizeInBits();
    if (!scalarWidth || scalarWidth > 64 || (scalarWidth % 8)) {
      std::string text;
      raw_string_ostream stream(text);
      instruction->print(stream);
      return createError("atomic64 phase: buffer scalar width must be 8, 16, 32 or 64 bits in " +
        instruction->getFunction()->getName() + ": " + stream.str());
    }
    if ((rmw || cas) && vector) return createError("atomic64 phase: vector atomic cannot be lowered");
    Value *result = vector && !store ? UndefValue::get(type) : nullptr;
    const unsigned components = vector ? vector->getNumElements() : 1;
    for (unsigned component = 0; component < components; ++component) {
      // Ordinary DXIL raw accesses can be less aligned than their value width.
      // Split only at the declared alignment, never truncate a value which spans
      // two canonical cells. Returning atomics retain one naturally aligned cell.
      const unsigned alignmentBits = load ? load->getAlign().value() * 8 :
        store ? store->getAlign().value() * 8 : scalarWidth;
      const unsigned width = std::min(scalarWidth, alignmentBits);
      auto *fragmentType = width == scalarWidth ? scalar : IntegerType::get(builder.getContext(), width);
      Value *assembled = builder.getInt64(0);
      for (unsigned fragment = 0; fragment < scalarWidth / width; ++fragment) {
      auto *bytes = builder.CreateBitCast(pointer, PointerType::get(builder.getInt8Ty(), 1));
      auto *at = builder.CreateInBoundsGEP(builder.getInt8Ty(), bytes,
        builder.getInt64(component * (scalarWidth / 8) + fragment * (width / 8)));
      auto *address = builder.CreatePtrToInt(at, builder.getInt64Ty());
      auto *aligned = builder.CreateIntToPtr(builder.CreateAnd(address, builder.getInt64(~uint64_t(7))), PointerType::get(builder.getInt64Ty(), 1));
      auto *shift = builder.CreateTrunc(builder.CreateMul(builder.CreateAnd(address, builder.getInt64(7)), builder.getInt64(8)), builder.getInt64Ty());
      auto *scalarPointer = builder.CreateBitCast(at, PointerType::get(fragmentType, 1));
      Value *input = store ? store->getValueOperand() : rmw ? rmw->getValOperand() : cas ? cas->getNewValOperand() : nullptr;
      if (input && vector) input = builder.CreateExtractElement(input, component);
      if (input && width != scalarWidth)
        input = builder.CreateTrunc(builder.CreateLShr(scalarBits(builder, input), fragment * width), fragmentType);
      auto extract = [&](Value *cell) {
        return bitsScalar(builder, builder.CreateTrunc(builder.CreateLShr(cell, shift), IntegerType::get(builder.getContext(), width)), fragmentType);
      };
      auto update = [&](Value *cell) -> Value * {
        auto *old = extract(cell);
        Value *desired = input;
        if (cas) desired = builder.CreateSelect(builder.CreateICmpEQ(old, cas->getCompareOperand()), input, old);
        if (rmw) switch (rmw->getOperation()) {
          case AtomicRMWInst::Xchg: break;
          case AtomicRMWInst::Add: desired = builder.CreateAdd(old, input); break;
          case AtomicRMWInst::Sub: desired = builder.CreateSub(old, input); break;
          case AtomicRMWInst::And: desired = builder.CreateAnd(old, input); break;
          case AtomicRMWInst::Or: desired = builder.CreateOr(old, input); break;
          case AtomicRMWInst::Xor: desired = builder.CreateXor(old, input); break;
          case AtomicRMWInst::Min: desired = builder.CreateSelect(builder.CreateICmpSLT(old, input), old, input); break;
          case AtomicRMWInst::Max: desired = builder.CreateSelect(builder.CreateICmpSGT(old, input), old, input); break;
          case AtomicRMWInst::UMin: desired = builder.CreateSelect(builder.CreateICmpULT(old, input), old, input); break;
          case AtomicRMWInst::UMax: desired = builder.CreateSelect(builder.CreateICmpUGT(old, input), old, input); break;
          default: llvm_unreachable("unexpected buffer atomic operation");
        }
        auto *mask = builder.CreateShl(builder.getInt64(width == 64 ? ~uint64_t(0) : ((uint64_t(1) << width) - 1)), shift);
        auto *bits = builder.CreateShl(builder.CreateZExtOrTrunc(scalarBits(builder, desired), builder.getInt64Ty()), shift);
        return builder.CreateOr(builder.CreateAnd(cell, builder.CreateNot(mask)), builder.CreateAnd(bits, mask));
      };
      auto *value = nativeOrCell(builder, aligned, [&]() -> Value * {
        if (load) {
          auto *operation = builder.CreateLoad(fragmentType, scalarPointer, load->isVolatile());
          operation->setAlignment(Align(width / 8));
          if (load->isAtomic()) operation->setAtomic(load->getOrdering(), load->getSyncScopeID());
          return operation;
        }
        if (store) {
          auto *operation = builder.CreateStore(input, scalarPointer, store->isVolatile());
          operation->setAlignment(Align(width / 8));
          if (store->isAtomic()) operation->setAtomic(store->getOrdering(), store->getSyncScopeID());
          return nullptr;
        }
        // Every valid device resource is registered by the exclusive phase
        // importer. A missing i64 atomic mapping can only be an invalid/null
        // descriptor; never emit an unsupported native i64 instruction.
        if ((rmw || cas) && width == 64) return Constant::getNullValue(instruction->getType());
        if (rmw) {
          auto *operation = builder.CreateAtomicRMW(rmw->getOperation(), scalarPointer, input, rmw->getAlign(), rmw->getOrdering(), rmw->getSyncScopeID());
          operation->setVolatile(rmw->isVolatile());
          return operation;
        }
        auto *operation = builder.CreateAtomicCmpXchg(scalarPointer, cas->getCompareOperand(), input, cas->getAlign(), cas->getSuccessOrdering(), cas->getFailureOrdering(), cas->getSyncScopeID());
        operation->setWeak(cas->isWeak());
        operation->setVolatile(cas->isVolatile());
        return operation;
      }, [&](Value *root) -> Value * {
        auto *cell = load ? EmitAtomic64CellRead(builder, root) : EmitAtomic64CellUpdate(builder, root, update);
        if (store) return nullptr;
        auto *old = extract(cell);
        if (!cas) return old;
        auto *aggregate = builder.CreateInsertValue(UndefValue::get(cas->getType()), old, 0);
        return builder.CreateInsertValue(aggregate, builder.CreateICmpEQ(old, cas->getCompareOperand()), 1);
      });
      if (value) {
        if (width == scalarWidth) result = vector ? builder.CreateInsertElement(result, value, component) : value;
        else assembled = builder.CreateOr(assembled,
          builder.CreateShl(builder.CreateZExt(scalarBits(builder, value), builder.getInt64Ty()), fragment * width));
      }
      }
      if (load && width != scalarWidth) {
        auto *value = bitsScalar(builder, builder.CreateTrunc(assembled, IntegerType::get(builder.getContext(), scalarWidth)), scalar);
        result = vector ? builder.CreateInsertElement(result, value, component) : value;
      }
    }
    if (result) instruction->replaceAllUsesWith(result);
    instruction->eraseFromParent();
  }
  // Native intersection callbacks cannot use physical SIMD intrinsics. Their
  // metadata and helper callgraph are complete only after stage/export merging,
  // so choose the legal execution policy here rather than while emitting a cell.
  SmallPtrSet<Function *, 32> intersections;
  SmallVector<Function *, 16> pending;
  if (auto *entries = module.getNamedMetadata("air.intersection"))
    for (auto *entry : entries->operands())
      if (auto *value = dyn_cast<ValueAsMetadata>(entry->getOperand(0)))
        if (auto *function = dyn_cast<Function>(value->getValue()))
          if (intersections.insert(function).second) pending.push_back(function);
  while (!pending.empty()) {
    auto *function = pending.pop_back_val();
    for (auto &instruction : instructions(function))
      if (auto *call = dyn_cast<CallBase>(&instruction))
        if (auto *callee = dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts()))
          if (intersections.insert(callee).second) pending.push_back(callee);
  }
  for (auto &function : module) {
    if (function.isDeclaration() || intersections.contains(&function) ||
        function.hasFnAttribute(Attribute::Convergent)) continue;
    if (function.hasFnAttribute(helperAttribute) || function.getName().startswith("dxmt.atomic64.read"))
      cooperateAtomic64(function, *cast<ReturnInst>(function.back().getTerminator()));
  }
  dxmt::linkAtomic64(module);
  return Error::success();
}
} // namespace dxmt::dxil
