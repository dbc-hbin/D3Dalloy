#include "simdgroup_implicit_membarrier.hpp"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/raw_ostream.h"
#include "../nt/air_builder.hpp"

namespace llvm::air {

static unsigned maxThreadIndex(const Value *value, const Function &f) {
  auto *arg = dyn_cast<Argument>(value);
  if (!arg || arg->getParent() != &f) return UINT_MAX;
  for (auto *entry : f.getParent()->getNamedMetadata("air.kernel")->operands()) {
    if (entry->getNumOperands() < 4 || mdconst::dyn_extract_or_null<Function>(entry->getOperand(0)) != &f)
      continue;
    auto *inputs = dyn_cast<MDNode>(entry->getOperand(2));
    if (!inputs || arg->getArgNo() >= inputs->getNumOperands()) continue;
    auto *input = dyn_cast<MDNode>(inputs->getOperand(arg->getArgNo()));
    if (!input || input->getNumOperands() < 2 ||
        input->getOperand(1) != MDString::get(f.getContext(), "air.thread_index_in_threadgroup")) continue;
    for (unsigned i = 3; i < entry->getNumOperands(); ++i) {
      auto *attr = dyn_cast<MDNode>(entry->getOperand(i));
      if (!attr || attr->getNumOperands() != 2 ||
          attr->getOperand(0) != MDString::get(f.getContext(), "air.max_work_group_size")) continue;
      auto *size = mdconst::dyn_extract_or_null<ConstantInt>(attr->getOperand(1));
      if (size && size->getLimitedValue() && size->getLimitedValue() <= 1024)
        return unsigned(size->getZExtValue()) - 1;
    }
  }
  return UINT_MAX;
}

// For a store-only counted loop, offsets of the form row * stride + iteration
// never alias across different iterations when the row holds the complete loop.
// Preserve the first and last barriers: they also order effects outside the loop.
static unsigned
disjointWriteIterations(StoreInst &store, LoopInfo &loops, ScalarEvolution &se) {
  using namespace PatternMatch;
  BasicBlock *body = store.getParent();
  auto *loop = loops.getLoopFor(body);
  if (!loop || loop->getNumBlocks() != 1 || !loop->getCanonicalInductionVariable())
    return 0;
  const auto count = se.getSmallConstantTripCount(loop);
  if (count <= 2)
    return 0;
  // The add is non-wrapping in the signed GEP index domain. The byte address
  // calculation must not truncate or wrap either (direct, in-bounds array index).
  auto *iv = loop->getCanonicalInductionVariable();
  if (!iv->getType()->isIntegerTy(32) ||
      body->getModule()->getDataLayout().getIndexSizeInBits(3) != 64) return 0;
  Value *rowBase = nullptr;
  SmallPtrSet<Value *, 4> written;
  for (auto &inst : *body) {
    if (!inst.mayReadOrWriteMemory()) continue;
    auto *write = dyn_cast<StoreInst>(&inst);
    if (!write || !write->isSimple() || write->getPointerAddressSpace() != 3) return 0;
    auto *gep = dyn_cast<GetElementPtrInst>(write->getPointerOperand());
    if (!gep || gep->getNumIndices() != 2 || !match(gep->getOperand(1), m_Zero())) return 0;
    auto *array = dyn_cast<ArrayType>(gep->getSourceElementType());
    auto *global = dyn_cast<GlobalVariable>(gep->getPointerOperand());
    if (!array || !global || global->getValueType() != array || !global->hasLocalLinkage() ||
        (!array->getElementType()->isIntegerTy() && !array->getElementType()->isFloatingPointTy()) ||
        array->getElementType()->getPrimitiveSizeInBits() > 128 ||
        write->getValueOperand()->getType() != array->getElementType() ||
        !written.insert(global).second) return 0;
    Value *index = gep->getOperand(2);
    if (auto *extend = dyn_cast<SExtInst>(index)) index = extend->getOperand(0);
    Value *base;
    if (!match(index, m_c_Add(m_Specific(iv), m_Value(base))) ||
        !cast<BinaryOperator>(index)->hasNoSignedWrap() || (rowBase && rowBase != base)) return 0;
    rowBase = base;
  }
  ConstantInt *stride;
  Value *row;
  if (!rowBase || !match(rowBase, m_c_Mul(m_Value(row), m_ConstantInt(stride))) ||
      !loop->isLoopInvariant(rowBase) || stride->isNegative() || stride->getLimitedValue() < count)
    return 0;
  // Modular integer multiplication only preserves divisibility for power-of-two
  // strides. For other strides, prove the multiply itself cannot wrap.
  if (!stride->getValue().isPowerOf2() && !cast<BinaryOperator>(rowBase)->hasNoUnsignedWrap()) {
    auto maxRow = se.getUnsignedRangeMax(se.getSCEV(row));
    Value *tid;
    ConstantInt *shift;
    if (row->getType()->isIntegerTy(32) && match(row, m_LShr(m_Value(tid), m_ConstantInt(shift))) &&
        shift->getLimitedValue() < 32)
      maxRow = APIntOps::umin(maxRow, APInt(32, maxThreadIndex(tid, *body->getParent()) >> shift->getZExtValue()));
    if (maxRow.ugt(APInt::getMaxValue(maxRow.getBitWidth()).udiv(stride->getValue()))) return 0;
  }
  return count;
}

PreservedAnalyses
SimdgroupImplicitMemBarrierPass::run(Function &F, FunctionAnalysisManager &AM) {
  if (!F.getParent()->getNamedMetadata("air.kernel"))
    return PreservedAnalyses::all();

  bool Changed = false;

  SmallVector<LoadInst *, 4> ToCheck;
  SmallDenseSet<Instruction *, 4> Fenced;

  for (auto &BB : F) {
    for (auto &Inst : BB) {
      if (auto Load = llvm::dyn_cast<LoadInst>(&Inst)) {
        if (Load->getPointerAddressSpace() == 3)
          ToCheck.push_back(Load);
      }
    }
  }

  if (ToCheck.empty())
    return PreservedAnalyses::all();

  auto &MemSSA = AM.getResult<MemorySSAAnalysis>(F).getMSSA();
  struct Insert { StoreInst *store; PHINode *iv; unsigned count; };
  SmallVector<Insert, 4> insertions;

  for (auto &Load : ToCheck) {
    auto MA = MemSSA.getMemoryAccess(Load);
    auto DefAccess = MA->getDefiningAccess();
    /**
    * We are only checking the simplest case: MemoryDef is a store to threadgroup memory
    * It's not complete, but good enough for all games I known requiring such warp-coherency behavior
    * FIXME: uniformity can be a issue because we are using `simdgroup_barrier`
    */
    if (auto Def = llvm::dyn_cast<MemoryDef>(DefAccess)) {
      auto DefInst = Def->getMemoryInst();
      if (Fenced.contains(DefInst))
        continue;
      if (auto Store = llvm::dyn_cast<StoreInst>(DefInst)) {
        if (Store->getPointerAddressSpace() == 3) {
          Fenced.insert(DefInst);
          auto &loops = AM.getResult<LoopAnalysis>(F);
          auto count = disjointWriteIterations(*Store, loops,
                                               AM.getResult<ScalarEvolutionAnalysis>(F));
          insertions.push_back({Store, count ? loops.getLoopFor(Store->getParent())->getCanonicalInductionVariable()
                                             : nullptr, count});
          Changed = true;
        }
      }
    }
  }

  for (auto [store, iv, count] : insertions) {
    auto *afterStore = store->getNextNode();
    IRBuilder<> builder(afterStore);
    if (count) {
      auto *type = cast<IntegerType>(iv->getType());
      auto *boundary = builder.CreateOr(builder.CreateICmpEQ(iv, ConstantInt::get(type, 0)),
                                        builder.CreateICmpEQ(iv, ConstantInt::get(type, count - 1)));
      builder.SetInsertPoint(SplitBlockAndInsertIfThen(boundary, afterStore, false));
    }
    AIRBuilder air(builder, nulls());
    air.CreateBarrier(MemFlags::Threadgroup, true);
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
} // namespace llvm::air
