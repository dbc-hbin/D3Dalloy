#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/InferAddressSpaces.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/JumpThreading.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO/DeadArgumentElimination.h"
#include "llvm/Transforms/IPO/GlobalOpt.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/IPO/Annotation2Metadata.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/InferFunctionAttrs.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/InstIterator.h"

#include "airconv_context.hpp"

#include "air_msad.h"
#include "air_samplepos.h"
#include "air_tessellation.h"
#include "air_denorm.h"
#include "air_fp64.h"
#include "air_atomic64.h"
#include "air_sampler_feedback.h"
#include "air_conservative.h"

#include "transforms/lower_16bit_texread.hpp"
#include "transforms/simdgroup_implicit_membarrier.hpp"

using namespace llvm;

namespace d3dalloy {

void initializeModule(llvm::Module &M) {
  M.setSourceFileName("airconv_generated.metal");
  M.setTargetTriple("air64-apple-macosx14.0.0");
  M.setDataLayout(
    "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:"
    "64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-"
    "v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:"
    "16:32"
  );
  M.setSDKVersion(VersionTuple(15, 0));
  M.addModuleFlag(Module::ModFlagBehavior::Error, "wchar_size", 4);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "frame-pointer", 2);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_device_buffers", 31);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_constant_buffers", 31);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_threadgroup_buffers", 31
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_textures", 128);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_read_write_textures", 8
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_samplers", 16);

};

static std::atomic_flag llvm_overwrite = false;

Value *createTextureIsNull(IRBuilderBase &builder, Value *texture) {
  auto &M = *builder.GetInsertBlock()->getModule();
  auto *type = cast<StructType>(cast<PointerType>(texture->getType())->getNonOpaquePointerElementType());
  // Native texture arguments are opaque handles, not ordinary pointers.
  // Use the same null query as Metal's is_null_texture, including without shader validation.
  auto is_null = M.getOrInsertFunction(
      ("air.is_null_" + type->getName().drop_front(StringRef("struct._").size()).drop_back(2)).str(),
      FunctionType::get(builder.getInt1Ty(), {texture->getType()}, false));
  auto *query = cast<Function>(is_null.getCallee());
  query->setOnlyReadsMemory();
  query->setOnlyAccessesArgMemory();
  query->setDoesNotThrow();
  query->setWillReturn();
  query->addFnAttr(Attribute::NoFree);
  query->addFnAttr(Attribute::MustProgress);
  query->addParamAttr(0, Attribute::NoCapture);
  query->addParamAttr(0, Attribute::ReadOnly);
  return builder.CreateCall(is_null, {texture});
}

// An immutable handle keeps its identity across writes to its texels. Reuse
// only an already executed query, never inspect an unselected descriptor.
static bool reuseTextureNullQueries(Function &function) {
  DominatorTree tree(function);
  DenseMap<std::pair<Function *, Value *>, CallInst *> previous;
  bool changed = false;
  for (auto *node : depth_first(tree.getRootNode())) {
    for (auto &inst : make_early_inc_range(*node->getBlock())) {
      auto *call = dyn_cast<CallInst>(&inst);
      auto *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || !(callee->getName().startswith("air.is_null_texture") ||
                       callee->getName().startswith("air.is_null_depth")))
        continue;
      auto &prior = previous[{callee, call->getArgOperand(0)}];
      if (prior && tree.dominates(prior, call)) {
        call->replaceAllUsesWith(prior);
        call->eraseFromParent();
        changed = true;
      } else {
        prior = call;
      }
    }
  }
  for (auto &block : function) {
    // InstCombine spells the descriptor guard either as present && sampler or
    // as null || !sampler. Both forms prove the query on the successful edge.
    auto *branch = dyn_cast<BranchInst>(block.getTerminator());
    if (!branch || !branch->isConditional() || branch->getSuccessor(0) == branch->getSuccessor(1))
      continue;
    Value *lhs, *rhs;
    using namespace PatternMatch;
    const bool absent = match(branch->getCondition(), m_Or(m_Value(lhs), m_Value(rhs)));
    if (!absent && !match(branch->getCondition(), m_And(m_Value(lhs), m_Value(rhs)))) continue;
    for (auto *term : {lhs, rhs}) {
      Value *query = term;
      if (!absent && !match(term, m_Not(m_Value(query)))) continue;
      auto *call = dyn_cast<CallInst>(query);
      auto *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || !(callee->getName().startswith("air.is_null_texture") ||
                       callee->getName().startswith("air.is_null_depth"))) continue;
      changed |= replaceDominatedUsesWith(query, ConstantInt::getFalse(function.getContext()), tree,
          BasicBlockEdge(branch->getParent(), branch->getSuccessor(absent ? 1 : 0))) != 0;
    }
  }
  return changed;
}

static bool uniformDescriptorWord(Value *value, const MDNode &inputs) {
  // The D3D12 root table is an immutable, draw-uniform constant-buffer argument.
  for (;;) {
    if (auto *load = dyn_cast<LoadInst>(value)) {
      if (!load->isSimple() || load->getPointerAddressSpace() != 2) return false;
      value = load->getPointerOperand();
    } else if (auto *gep = dyn_cast<GetElementPtrInst>(value)) {
      if (!gep->hasAllConstantIndices()) return false;
      value = gep->getPointerOperand();
    } else if (auto *cast = dyn_cast<BitCastInst>(value)) {
      value = cast->getOperand(0);
    } else {
      auto *arg = dyn_cast<Argument>(value);
      if (!arg || arg->getArgNo() >= inputs.getNumOperands()) return false;
      auto *desc = dyn_cast<MDNode>(inputs.getOperand(arg->getArgNo()));
      auto *kind = desc && desc->getNumOperands() > 1 ? dyn_cast<MDString>(desc->getOperand(1)) : nullptr;
      if (!kind) return false;
      if (kind->getString() == "air.indirect_buffer") return true;
      if (kind->getString() != "air.buffer") return false;
      for (unsigned i = 2; i + 1 < desc->getNumOperands(); ++i) {
        auto *key = dyn_cast<MDString>(desc->getOperand(i));
        auto *name = dyn_cast<MDString>(desc->getOperand(i + 1));
        if (key && key->getString() == "air.arg_name" && name && name->getString() == "static_samplers")
          return true;
      }
      return false;
    }
  }
}

static MDNode *stageInputs(Function &f, StringRef stage) {
  if (f.empty()) return nullptr;
  auto *entries = f.getParent()->getNamedMetadata(stage);
  if (!entries || entries->getNumOperands() != 1) return nullptr;
  auto *signature = entries->getOperand(0);
  if (signature->getNumOperands() < 3 || mdconst::dyn_extract<Function>(signature->getOperand(0)) != &f)
    return nullptr;
  return dyn_cast<MDNode>(signature->getOperand(2));
}

static Value *queriedTexture(Instruction &inst) {
  auto *call = dyn_cast<CallInst>(&inst);
  auto *callee = call ? call->getCalledFunction() : nullptr;
  if (!callee || !(callee->getName().startswith("air.is_null_texture") ||
                   callee->getName().startswith("air.is_null_depth"))) return nullptr;
  return call->getArgOperand(0);
}

static LoadInst *fixedEntryDescriptor(Value *value, BasicBlock &entry, const MDNode &inputs) {
  auto *load = dyn_cast_or_null<LoadInst>(value);
  return load && load->getParent() == &entry && uniformDescriptorWord(load, inputs) ? load : nullptr;
}

// D3D12 owns these fixed table slots and writes a zero 64-bit ID for null.
// Inspect only an already loaded slot. Never speculate a native texture access.
static Value *descriptorId(IRBuilder<> &b, LoadInst *handle) {
  auto *slot = b.CreateBitCast(handle->getPointerOperand(), b.getInt64Ty()->getPointerTo(2));
  return b.CreateFreeze(b.CreateAlignedLoad(b.getInt64Ty(), slot, handle->getAlign()));
}

static bool lowerFixedTextureNulls(Function &f, const MDNode &inputs) {
  DenseMap<LoadInst *, Value *> nulls;
  for (auto &bb : f) {
    for (auto &inst : make_early_inc_range(bb)) {
      auto *handle = fixedEntryDescriptor(queriedTexture(inst), f.getEntryBlock(), inputs);
      if (!handle) continue;
      auto &absent = nulls[handle];
      if (!absent) {
        IRBuilder<> b(handle->getNextNode());
        absent = b.CreateICmpEQ(descriptorId(b, handle), b.getInt64(0));
      }
      inst.replaceAllUsesWith(absent);
      inst.eraseFromParent();
    }
  }
  return !nulls.empty();
}

// Consecutive lane guards all succeed when the final byte is in the UAV.
// Keep the original per-lane path for partial ranges and null descriptors.
static void specializeStoreSpans(Function &f) {
  const auto &dl = f.getParent()->getDataLayout();
  SmallVector<BasicBlock *, 32> starts;
  for (auto &bb : f) starts.push_back(&bb);
  SmallPtrSet<BasicBlock *, 32> consumed;
  for (auto *head : starts) {
    if (consumed.count(head)) continue;
    SmallVector<BasicBlock *, 16> guards, stores;
    auto *guard = head;
    Value *length = nullptr, *base = nullptr;
    uint64_t last = 0;
    int64_t lastOffset = 0;
    while (stores.size() < 16) {
      auto *br = dyn_cast<BranchInst>(guard->getTerminator());
      if (!br || !br->isConditional()) break;
      auto *cmp = dyn_cast<ICmpInst>(br->getCondition());
      if (!cmp || cmp->getPredicate() != ICmpInst::ICMP_UGT) break;
      auto *limit = dyn_cast<ConstantInt>(cmp->getOperand(1));
      if (!limit || limit->getBitWidth() > 64) break;
      auto *arm = br->getSuccessor(0), *next = br->getSuccessor(1);
      if (arm == next || arm->getSinglePredecessor() != guard || arm->getSingleSuccessor() != next ||
          isa<PHINode>(next->front())) break;
      if (guard != head && (pred_size(guard) != 2 || llvm::any_of(*guard, [](Instruction &i) {
        return !i.isTerminator() && (!isa<ICmpInst>(i) || !i.hasOneUse() || !isa<BranchInst>(*i.user_begin()));
      }))) break;
      auto *store = dyn_cast_or_null<StoreInst>(arm->getTerminator()->getPrevNode());
      if (!store || !store->isSimple() || store->getPointerAddressSpace() != 1) break;
      if (llvm::any_of(*arm, [&](Instruction &i) {
        return &i != store && !i.isTerminator() && !isa<GetElementPtrInst>(i) && !isa<BitCastInst>(i);
      })) break;
      int64_t offset;
      auto *ptr = GetPointerBaseWithConstantOffset(store->getPointerOperand(), offset, dl);
      uint64_t end = limit->getZExtValue(), width = dl.getTypeStoreSize(store->getValueOperand()->getType());
      if (!length) { length = cmp->getOperand(0); base = ptr; }
      else if (cmp->getOperand(0) != length || ptr != base || end <= last || end-last != width ||
               offset <= lastOffset || uint64_t(offset)-uint64_t(lastOffset) != width) break;
      guards.push_back(guard); stores.push_back(arm); last = end; lastOffset = offset; guard = next;
    }
    if (stores.size() < 4) continue;
    // The only values bypassed by the fast path are branch predicates and
    // arm-local addresses. Require all store operands to be available there.
    DominatorTree dt(f);
    bool available = true;
    for (auto *bb : guards) if (bb != head) for (auto &i : *bb) for (auto *user : i.users()) {
      auto *use = dyn_cast<Instruction>(user);
      if (!use || !llvm::is_contained(guards, use->getParent())) available = false;
    }
    for (auto *arm : stores) for (auto &i : *arm) for (Value *operand : i.operands()) {
      auto *def = dyn_cast<Instruction>(operand);
      if (def && def->getParent() != arm && !dt.dominates(def, head->getTerminator())) available = false;
    }
    if (!available) continue;
    consumed.insert(guards.begin(), guards.end());
    auto *partial = head->splitBasicBlock(head->getTerminator(), "uav.span.partial");
    auto *full = BasicBlock::Create(f.getContext(), "uav.span.full", &f, partial);
    IRBuilder<> fast(full);
    ValueToValueMapTy map;
    for (auto *arm : stores) for (auto &inst : *arm) {
      if (inst.isTerminator()) continue;
      auto *copy = inst.clone();
      RemapInstruction(copy, map, RF_IgnoreMissingLocals);
      fast.Insert(copy); map[&inst] = copy;
    }
    fast.CreateBr(guard);
    IRBuilder<> b(head->getTerminator());
    b.CreateCondBr(b.CreateICmpUGT(length, ConstantInt::get(length->getType(), last)), full, partial);
    head->getTerminator()->eraseFromParent();
  }
}

static bool specializeBoundNormalizedSamples(Function &f, const MDNode &inputs, bool kernel) {
  using namespace PatternMatch;
  constexpr uint64_t pixelMask = uint64_t(1) << 47;
  DenseMap<Value *, SmallVector<BranchInst *, 8>> sites;
  Value *best = nullptr;
  for (auto &bb : f) {
    auto *br = dyn_cast<BranchInst>(bb.getTerminator());
    if (!br || !br->isConditional()) continue;
    // Track the coordinate split emitted by AIRBuilder. If CFG folding has
    // removed both named successors there is no candidate at this site.
    if (!br->getSuccessor(0)->getName().startswith("sample.normalized") &&
        !br->getSuccessor(1)->getName().startswith("sample.normalized")) continue;
    auto *eq = dyn_cast<ICmpInst>(br->getCondition());
    if (!eq || !eq->isEquality() || !match(eq->getOperand(1), m_Zero())) continue;
    Value *word;
    if (!match(eq->getOperand(0), m_And(m_Value(word), m_SpecificInt(pixelMask)))) continue;
    if (!fixedEntryDescriptor(word, f.getEntryBlock(), inputs)) continue;
    sites[word].push_back(br);
    if (!best || sites[word].size() > sites[best].size()) best = word;
  }
  // Bound code growth to fragments with a dense run of samples using one
  // uniform descriptor. Other samplers retain their own per-access mode.
  if (!kernel && (!best || sites[best].size() < 8 || sites[best].size() * 128 < f.getInstructionCount())) return false;
  auto &entry = f.getEntryBlock();
  SmallVector<LoadInst *, 8> handles;
  SmallVector<std::pair<Instruction *, bool>, 16> checks;
  // Limit the extra normalized fallback to long runs. Smaller fragments
  // split by coordinate mode and share each fixed-ID comparison.
  const bool longRun = !kernel && sites[best].size() >= 24;
  unsigned maxUses = 0;
  {
    for (auto &inst : instructions(f)) {
      Value *handle = queriedTexture(inst);
      bool boundResult = false;
      if (auto *cmp = dyn_cast<ICmpInst>(&inst); cmp && cmp->isEquality() &&
          isa<ConstantPointerNull>(cmp->getOperand(1))) {
        auto *ptr = cast<PointerType>(cmp->getOperand(0)->getType());
        auto *type = !ptr->isOpaque() && ptr->getAddressSpace() == 2
            ? dyn_cast<StructType>(ptr->getNonOpaquePointerElementType()) : nullptr;
        if (type && type->hasName() && type->getName() == "struct._sampler_t") handle = cmp->getOperand(0);
        boundResult = cmp->getPredicate() == ICmpInst::ICMP_NE;
      }
      if (auto *load = fixedEntryDescriptor(handle, entry, inputs)) {
        if (!llvm::is_contained(handles, load)) handles.push_back(load);
        checks.emplace_back(&inst, boundResult);
        maxUses = std::max(maxUses, unsigned(inst.getNumUses()));
      }
    }
  }
  // A small number of repeatedly tested, draw/dispatch-uniform descriptors
  // justifies a fast body. Share the original full semantics as one fallback.
  // Cloning a long ALU chain to remove a few guards increases register/code
  // pressure. Require one repeated test per 64 PS or 160 kernel instructions.
  const bool sharedFallback = !longRun && maxUses >= 8 && handles.size() <= 8 &&
      (kernel || maxUses >= 4 * handles.size()) && maxUses * (kernel ? 160u : 64u) >= f.getInstructionCount();
  if (kernel && !sharedFallback) return false;
  if (!longRun && !sharedFallback) { handles.clear(); checks.clear(); }
  auto *lastLoad = kernel ? handles.front() : cast<Instruction>(best);
  for (auto *handle : handles)
    if (lastLoad->comesBefore(handle)) lastLoad = handle;
  Instruction *split = entry.getTerminator();
  for (auto [check, ignored] : checks)
    if (check->getParent() == &entry && lastLoad->comesBefore(check) && check->comesBefore(split)) split = check;
  auto *start = entry.splitBasicBlock(split, "coordinate.normalized");
  SmallVector<BasicBlock *, 32> original;
  for (auto &bb : f) if (&bb != &entry) original.push_back(&bb);
  auto cloneBody = [&](ValueToValueMapTy &map, StringRef suffix) {
    for (auto *bb : original) map[bb] = CloneBasicBlock(bb, map, suffix, &f);
    for (auto *bb : original)
      for (auto &inst : *cast<BasicBlock>(map[bb])) RemapInstruction(&inst, map, RF_IgnoreMissingLocals);
  };
  ValueToValueMapTy checked, pixel, empty;
  if (!kernel && sharedFallback) cloneBody(empty, ".empty");
  if (!handles.empty()) cloneBody(checked, ".dynamic");
  if (!sharedFallback) cloneBody(pixel, ".pixel");
  if (!kernel) for (auto *br : sites[best]) {
    auto *cmp = cast<ICmpInst>(br->getCondition());
    bool takesNormalized = cmp->getPredicate() == ICmpInst::ICMP_EQ;
    if (!handles.empty() && !sharedFallback)
      cast<BranchInst>(checked[br])->setCondition(ConstantInt::get(cmp->getType(), takesNormalized));
    if (!sharedFallback) cast<BranchInst>(pixel[br])->setCondition(ConstantInt::get(cmp->getType(), !takesNormalized));
    br->setCondition(ConstantInt::get(cmp->getType(), takesNormalized));
  }
  for (auto [check, boundResult] : checks) {
    if (!empty.empty() && queriedTexture(*check) && check->getParent() != &entry)
      cast<Instruction>(empty[check])->replaceAllUsesWith(ConstantInt::getTrue(f.getContext()));
    if (check->getParent() != &entry)
      check->replaceAllUsesWith(ConstantInt::get(check->getType(), boundResult));
  }
  auto *oldTerminator = entry.getTerminator();
  IRBuilder<> b(oldTerminator);
  Value *normalized = kernel ? b.getTrue() :
      b.CreateICmpEQ(b.CreateAnd(b.CreateFreeze(best), b.getInt64(pixelMask)), b.getInt64(0));
  auto *fallback = cast<BasicBlock>(sharedFallback ? checked[start] : pixel[start]);
  Value *fast = normalized;
  if (!handles.empty()) {
    // Freeze each ID independently so an unused undefined slot cannot mask a known null.
    for (auto *handle : handles)
      fast = b.CreateAnd(fast, b.CreateICmpNE(descriptorId(b, handle), b.getInt64(0)));
    if (!sharedFallback) {
      fallback = BasicBlock::Create(f.getContext(), "coordinate.fallback", &f);
      IRBuilder<>(fallback).CreateCondBr(normalized, cast<BasicBlock>(checked[start]), cast<BasicBlock>(pixel[start]));
    }
  }
  // The generic fallback retains partial bindings. If every fixed texture is
  // absent, its native sample operations and their individual guards fold away.
  if (!empty.empty()) {
    Value *allAbsent = b.getTrue();
    for (auto *handle : handles) {
      auto *ptr = cast<PointerType>(handle->getType());
      if (ptr->getAddressSpace() != 1) continue;
      allAbsent = b.CreateAnd(allAbsent, b.CreateICmpEQ(descriptorId(b, handle), b.getInt64(0)));
    }
    auto *partial = BasicBlock::Create(f.getContext(), "sample.null.fallback", &f);
    IRBuilder<>(partial).CreateCondBr(allAbsent, cast<BasicBlock>(empty[start]), fallback);
    fallback = partial;
  }
  b.CreateCondBr(fast, start, fallback);
  oldTerminator->eraseFromParent();
  return true;
}

void
runOptimizationPasses(llvm::Module &M) {
  // Only D3D12 root-bound shaders define null-descriptor reads as zero. D3D11
  // modules never enter this path. Guard native AIR accesses, not their results:
  // selecting zero after a query/read/sample still touches a null Metal texture.
  if (M.getModuleFlag("d3dalloy.d3d12.root-signature")) {
    // Derivatives read neighboring fragment lanes, including helper lanes.
    // Preserve their convergence when optimizing the surrounding control flow.
    for (auto &function : M)
      if (function.getName().startswith("air.dfdx.") || function.getName().startswith("air.dfdy."))
        function.addFnAttr(Attribute::Convergent);
    SmallVector<CallInst *, 16> accesses;
    for (auto &function : M) for (auto &instruction : instructions(function)) {
      auto *call = dyn_cast<CallInst>(&instruction);
      auto *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || !callee->isDeclaration() || !callee->getName().startswith("air.") ||
          callee->getName().startswith("air.atomic_") || callee->getName().startswith("air.is_null_") ||
          call->arg_empty())
        continue;
      auto *pointer = dyn_cast<PointerType>(call->getArgOperand(0)->getType());
      auto *type = pointer && !pointer->isOpaque() && pointer->getAddressSpace() == 1
          ? dyn_cast<StructType>(pointer->getNonOpaquePointerElementType()) : nullptr;
      // AIRBuilder::getTextureHandleType names the opaque native texture/depth
      // types this way. Raw buffers, phase metadata and workgraph pointers are not textures.
      if (type && type->isOpaque() && type->hasName() && type->getName().endswith("_t") &&
          (type->getName().startswith("struct._texture_") || type->getName().startswith("struct._depth_")))
        accesses.push_back(call);
    }
    for (auto *call : accesses) {
      IRBuilder<> builder(call);
      auto *texture = call->getArgOperand(0);
      Value *present = builder.CreateNot(createTextureIsNull(builder, texture));
      for (auto &argument : call->args()) {
        auto *pointer = dyn_cast<PointerType>(argument->getType());
        auto *type = pointer && !pointer->isOpaque() && pointer->getAddressSpace() == 2
            ? dyn_cast<StructType>(pointer->getNonOpaquePointerElementType()) : nullptr;
        // AIR 2.8 reads use an implicit sampler, not a nullable D3D12 descriptor.
        if (type && type->hasName() && type->getName() == "struct._sampler_t" &&
            !call->getCalledFunction()->getName().startswith("air.read_"))
          present = builder.CreateAnd(present, builder.CreateIsNotNull(argument));
      }
      auto *head = call->getParent();
      auto *then_term = SplitBlockAndInsertIfThen(present, call, false);
      auto *continuation = call->getParent();
      call->moveBefore(then_term);
      if (!call->getType()->isVoidTy()) {
        builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
        auto *merged = builder.CreatePHI(call->getType(), 2);
        call->replaceUsesWithIf(merged, [&](Use &use) { return use.getUser() != merged; });
        merged->addIncoming(call, then_term->getParent());
        // AIR residency bit0 denotes a fault; skipped access has no fault.
        merged->addIncoming(Constant::getNullValue(call->getType()), head);
      }
    }
  }
  for (auto &function : M) {
    if (function.isDeclaration() && function.getName().startswith("d3dalloy.fp64.")) {
      linkFP64(M);
      break;
    }
  }

  if (!llvm_overwrite.test_and_set()) {
    auto Map = cl::getRegisteredOptions();
    auto InfiniteLoopThreshold = Map["instcombine-infinite-loop-threshold"];
    if (InfiniteLoopThreshold) {
      reinterpret_cast<cl::opt<unsigned> *>(InfiniteLoopThreshold)->setValue(1000);
    }
  }

  // Following optimization passes are picked from default LLVM pipelines
  // An unoptimized shader can make PSO compilation take a very long time
  // But a full optimization pipeline is also too heavy
  // So we choose some passes that really matter to run

  // Create the analysis managers.
  // These must be declared in this order so that they are destroyed in the
  // correct order due to inter-analysis-manager references.
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Create the new pass manager builder.
  // Take a look at the PassBuilder constructor parameters for more
  // customization, e.g. specifying a TargetMachine or various debugging
  // options.
  llvm::PassInstrumentationCallbacks PIC;
  PassBuilder PB(nullptr, PipelineTuningOptions(), {}, &PIC);

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // llvm::StandardInstrumentations SI(true);
  // SI.registerCallbacks(PIC, &FAM);

  ModulePassManager MPM;

  // Convert @llvm.global.annotations to !annotation metadata.
  MPM.addPass(Annotation2MetadataPass());

  // Force any function attributes we want the rest of the pipeline to observe.
  MPM.addPass(ForceFunctionAttrsPass());
  // Do basic inference of function attributes from known properties of system
  // libraries and other oracles.
  MPM.addPass(InferFunctionAttrsPass());

  /*
   * Recover the address space of every flat (AS0) address expression that
   * reaches storage in a specific space.
   *
   * The DXIL front end relocates a module-scope immutable global into AIR
   * constant storage (AS2, see dxil::PrepareModuleStorage), but the GEP/load
   * chain the shader already built stays in AS0 and only sees the storage
   * through an addrspacecast. Dereferencing that cast would read constant
   * storage as device memory, so the flat chain has to be folded back onto the
   * storage's own address space - which is exactly what this pass infers: the
   * cast's operand is in AS2, so the cast, the GEPs and the loads that follow
   * are all rewritten into AS2. The DXBC immediate-constant path already emits
   * the AS2 form directly, so both front ends then produce the same addressing.
   *
   * The flat address space is stated explicitly: the pass otherwise asks the
   * target for it, and this pipeline has no target machine, so it would infer
   * nothing and the AS0 chain would stay behind a cast. AS0 is the right flat
   * space here because it is the default address space the DXIL front end
   * builds flat pointers in, and it also holds the private alloca-derived
   * addressing, which the pass leaves alone (join(flat, AS0) = AS0).
   *
   * It has to run before any pass can reshape the chain - InstCombine folds a
   * GEP of an addrspacecast into an addrspacecast of a GEP, which moves the
   * cast to the load instead of removing it - so it is the first thing the
   * pipeline does to the module.
   */
  MPM.addPass(createModuleToFunctionPassAdaptor(InferAddressSpacesPass(0)));

  {
    // Create an early function pass manager to cleanup the output of the
    // frontend.
    FunctionPassManager EarlyFPM;
    // Lower llvm.expect to metadata before attempting transforms.
    // Compare/branch metadata may alter the behavior of passes like SimplifyCFG.
    EarlyFPM.addPass(LowerExpectIntrinsicPass());
    EarlyFPM.addPass(SimplifyCFGPass());
    EarlyFPM.addPass(SROAPass());
    EarlyFPM.addPass(EarlyCSEPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(EarlyFPM), true /* ? */));
  }

  MPM.addPass(AlwaysInlinerPass());
  MPM.addPass(IPSCCPPass());
  MPM.addPass(GlobalOptPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
  MPM.addPass(DeadArgumentEliminationPass());

  {
    // Create a small function pass pipeline to cleanup after all the global
    // optimizations.
    FunctionPassManager GlobalCleanupPM;
    GlobalCleanupPM.addPass(InstCombinePass());

    GlobalCleanupPM.addPass(SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

    GlobalCleanupPM.addPass(air::Lower16BitTexReadPass());
    GlobalCleanupPM.addPass(air::SimdgroupImplicitMemBarrierPass());

    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(GlobalCleanupPM), true /* ? */));
  }

  // MPM.addPass(VerifierPass());

  // Optimize the IR!
  MPM.run(M, MAM);
  if (M.getModuleFlag("d3dalloy.d3d12.root-signature")) {
    SmallPtrSet<Function *, 8> candidates;
    for (auto &query : M) {
      if (!query.getName().startswith("air.is_null_texture") && !query.getName().startswith("air.is_null_depth"))
        continue;
      if (!query.hasNUsesOrMore(2)) continue;
      for (auto *user : query.users())
        if (auto *call = dyn_cast<CallInst>(user)) candidates.insert(call->getFunction());
    }
    for (auto *function : candidates) {
      if (!reuseTextureNullQueries(*function)) continue;
      FAM.invalidate(*function, PreservedAnalyses::none());
      FunctionPassManager cleanup;
      cleanup.addPass(InstCombinePass());
      cleanup.addPass(SimplifyCFGPass());
      cleanup.run(*function, FAM);
      auto *inputs = stageInputs(*function, "air.fragment");
      const bool kernel = !inputs;
      if (kernel) inputs = stageInputs(*function, "air.kernel");
      if (inputs && specializeBoundNormalizedSamples(*function, *inputs, kernel)) {
        FAM.invalidate(*function, PreservedAnalyses::none());
        FunctionPassManager modes;
        modes.addPass(SimplifyCFGPass());
        modes.addPass(EarlyCSEPass());
        modes.addPass(InstCombinePass());
        modes.addPass(SimplifyCFGPass());
        // Adjacent samples on one descriptor share its validity predicate.
        // Thread that known outcome across short intervening arithmetic blocks.
        modes.addPass(JumpThreadingPass(64));
        modes.addPass(EarlyCSEPass());
        modes.addPass(InstCombinePass());
        modes.addPass(SimplifyCFGPass(SimplifyCFGOptions().sinkCommonInsts(true)));
        modes.run(*function, FAM);
      }
    }
    for (auto &f : M) {
      auto *inputs = stageInputs(f, "air.fragment");
      if (!inputs) inputs = stageInputs(f, "air.kernel");
      if (!inputs || !lowerFixedTextureNulls(f, *inputs)) continue;
      FAM.invalidate(f, PreservedAnalyses::none());
      FunctionPassManager cleanup;
      cleanup.addPass(EarlyCSEPass());
      cleanup.addPass(JumpThreadingPass(64));
      cleanup.addPass(EarlyCSEPass());
      cleanup.addPass(InstCombinePass());
      cleanup.addPass(SimplifyCFGPass(SimplifyCFGOptions().sinkCommonInsts(true)));
      cleanup.run(f, FAM);
    }
  }
  lowerBoundedLoads(M);
  for (auto &f : M) {
    if (f.isDeclaration()) continue;
    if (stageInputs(f, "air.kernel")) specializeStoreSpans(f);
    if (!stageInputs(f, "air.fragment")) continue;
    SmallPtrSet<Value *, 16> seen;
    for (auto &bb : f) {
      auto *br = dyn_cast<BranchInst>(bb.getTerminator());
      if (!br || !br->isConditional() ||
          (!br->getSuccessor(0)->getName().startswith("sample.normalized") &&
           !br->getSuccessor(1)->getName().startswith("sample.normalized"))) continue;
      if (seen.insert(br->getCondition()).second) continue;
      FAM.invalidate(f, PreservedAnalyses::none());
      FunctionPassManager share;
      share.addPass(JumpThreadingPass(12));
      share.addPass(EarlyCSEPass());
      share.run(f, FAM);
      break;
    }
  }
}

void
lowerBoundedLoads(llvm::Module &M) {
  /* d3dalloy.bounded.load.*(valid, pointer) is a device or constant load that reads zero
   * when `valid` is false. The fallback address is a zero-filled constant: a load
   * through null also reads zero on Apple GPUs, but Shader Validation rejects it and
   * Metal does not define it. Its 16 bytes cover the widest bounded load (a CBV row). */
  SmallVector<Function *, 4> helpers;
  for (auto &function : M)
    if (function.isDeclaration() && function.getName().startswith("d3dalloy.bounded.load."))
      helpers.push_back(&function);
  if (helpers.empty())
    return;
  auto *zero_type = ArrayType::get(Type::getInt32Ty(M.getContext()), 4);
  auto *zero = new GlobalVariable(M, zero_type, true, GlobalValue::InternalLinkage,
                                  ConstantAggregateZero::get(zero_type), "d3dalloy.bounded.zero", nullptr,
                                  GlobalValue::NotThreadLocal, 2);
  zero->setAlignment(Align(16));
  for (auto *helper : helpers) {
    auto *type = cast<PointerType>(helper->getFunctionType()->getParamType(1));
    /* Device pointers reach the constant by its address, as Metal's own
     * reinterpret_cast of a program-scope constant does. */
    auto *fallback = type->getAddressSpace() == 2
        ? ConstantExpr::getBitCast(zero, type)
        : ConstantExpr::getIntToPtr(ConstantExpr::getPtrToInt(zero, Type::getInt64Ty(M.getContext())), type);
    for (auto *user : make_early_inc_range(helper->users())) {
      auto *call = cast<CallInst>(user);
      IRBuilder<> builder(call);
      auto *pointer = call->getArgOperand(1);
      auto *bounded = builder.CreateSelect(call->getArgOperand(0), pointer, fallback);
      auto *load = builder.CreateLoad(call->getType(), bounded);
      call->replaceAllUsesWith(load);
      call->eraseFromParent();
    }
    helper->eraseFromParent();
  }
}

void
removeNamedMetadata(llvm::Module &M, StringRef Name) {
  if (auto MD = M.getNamedMetadata(Name)) {
    M.eraseNamedMetadata(MD);
  }
}

template<size_t N>
void linkShader(llvm::Module &M, const unsigned char (&bitcode)[N]) {
  Expected<std::unique_ptr<Module>> modOrErr =
      parseBitcodeFile(MemoryBufferRef(StringRef((const char *)bitcode, N), ""), M.getContext());

  if (!modOrErr) {
    // not expected to see this unless something really bad happened in compile time
    errs() << "Failed to parse air bitcode\n";
    return;
  }

  auto module = std::move(modOrErr.get());

  removeNamedMetadata(*module, "air.compile_options");
  removeNamedMetadata(*module, "air.version");
  removeNamedMetadata(*module, "air.language_version");
  removeNamedMetadata(*module, "air.source_file_name");
  removeNamedMetadata(*module, "llvm.ident");
  removeNamedMetadata(*module, "llvm.module.flags");

  // Helpers belong to this shader module, not each separately linked AIR export.
  llvm::Linker::linkModules(M, std::move(module), Linker::LinkOnlyNeeded,
    [](Module &linked, const StringSet<> &imported) {
      internalizeModule(linked, [&](const GlobalValue &value) {
        return !imported.contains(value.getName());
      });
    });
};

void
linkMSAD(llvm::Module &M) {
  linkShader(M, air_msad);
}

void
linkSamplePos(llvm::Module &M) {
  linkShader(M, air_samplepos);
}

void
linkTessellation(llvm::Module &M) {
  linkShader(M, air_tessellation);
}

void
linkDenorm(llvm::Module &M) {
  linkShader(M, air_denorm);
}

void
linkFP64(llvm::Module &M) {
  linkShader(M, air_fp64);
}

void
linkAtomic64(llvm::Module &M) {
  linkShader(M, air_atomic64);
  // Keep the allocator/reclamation loop shared: inlining it into every user
  // update makes AGX instruction selection grow pathologically on typed shaders.
  if (auto *cas = M.getFunction("d3dalloy_atomic64_compare_exchange"))
    cas->addFnAttr(llvm::Attribute::NoInline);
}

void
linkSamplerFeedback(llvm::Module &M) {
  linkShader(M, air_sampler_feedback);
}

void
linkConservative(llvm::Module &M) {
  linkShader(M, air_conservative);
}

} // namespace d3dalloy
