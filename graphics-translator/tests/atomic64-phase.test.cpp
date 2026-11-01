// CPU-only full-module phase lowering contracts; actual GPU values are checked by public atomic64 consumers.
#include "../../src/airconv/dxil_converter.hpp"
#include "../../src/airconv/airconv_context.hpp"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include <cstdio>

int main() {
  using namespace llvm;
  LLVMContext context;
  context.setOpaquePointers(false);
  Module module("atomic64-phase-semantics", context);
  d3dalloy::initializeModule(module);
  SM50_SHADER_COMPILATION_ARGUMENT_DATA phase{nullptr, SM50_SHADER_D3D12_ATOMIC64_PHASE};
  d3dalloy::dxil::InitializeAtomic64Phase(module, &phase);
  auto *i32 = Type::getInt32Ty(context);
  auto *vector = FixedVectorType::get(i32, 4);
  auto *pointer = PointerType::get(i32, 1);
  auto *function = Function::Create(FunctionType::get(Type::getVoidTy(context),
    {pointer, PointerType::get(vector, 1)}, false), GlobalValue::ExternalLinkage, "consumer", module);
  IRBuilder<> builder(BasicBlock::Create(context, "entry", function));
  auto *data = function->getArg(0);
  auto *load = builder.CreateLoad(i32, data, true);
  load->setAlignment(Align(4));
  load->setAtomic(AtomicOrdering::Acquire, SyncScope::SingleThread);
  auto *store = builder.CreateStore(builder.getInt32(9), data, true);
  store->setAlignment(Align(4));
  store->setAtomic(AtomicOrdering::Release, SyncScope::SingleThread);
  auto *rmw = builder.CreateAtomicRMW(AtomicRMWInst::Add, data, builder.getInt32(1),
    Align(4), AtomicOrdering::Monotonic, SyncScope::SingleThread);
  rmw->setVolatile(true);
  auto *cas = builder.CreateAtomicCmpXchg(data, builder.getInt32(9), builder.getInt32(10),
    Align(4), AtomicOrdering::AcquireRelease, AtomicOrdering::Acquire, SyncScope::SingleThread);
  cas->setWeak(true);
  cas->setVolatile(true);
  builder.CreateStore(ConstantVector::getSplat(ElementCount::getFixed(4), builder.getInt32(7)), function->getArg(1), true);
  auto legacy = module.getOrInsertFunction("air.atomic.global.add.u.i32", FunctionType::get(i32,
    {pointer, i32, i32, i32, builder.getInt1Ty()}, false));
  auto *metadata = builder.CreateCall(legacy, {data, builder.getInt32(1), builder.getInt32(0), builder.getInt32(3), builder.getTrue()});
  metadata->setMetadata(d3dalloy::dxil::kAtomic64Metadata, MDNode::get(context, {}));
  builder.CreateRetVoid();
  if (auto error = d3dalloy::dxil::LowerAtomic64Phase(module)) {
    logAllUnhandledErrors(std::move(error), errs()); return 1;
  }
  if (verifyModule(module, &errs())) return 1;

  unsigned loads = 0, stores = 0, atomics = 0, exchanges = 0, nativeMetadata = 0;
  for (auto &instruction : instructions(function)) {
    if (auto *operation = dyn_cast<AtomicRMWInst>(&instruction);
        operation && operation->getMetadata(d3dalloy::dxil::kAtomic64Metadata)) {
      if (!operation->isVolatile() || operation->getOrdering() != AtomicOrdering::Monotonic) return 1;
      ++nativeMetadata;
    }
    if (!instruction.getParent()->getName().startswith("atomic64.access.native")) continue;
    if (auto *operation = dyn_cast<LoadInst>(&instruction)) {
      if (!operation->isVolatile() || operation->getOrdering() != AtomicOrdering::Acquire ||
          operation->getSyncScopeID() != SyncScope::SingleThread) return 1;
      ++loads;
    } else if (auto *operation = dyn_cast<StoreInst>(&instruction)) {
      if (!operation->isVolatile()) return 1;
      if (operation->isAtomic() && (operation->getOrdering() != AtomicOrdering::Release ||
          operation->getSyncScopeID() != SyncScope::SingleThread)) return 1;
      ++stores;
    } else if (auto *operation = dyn_cast<AtomicRMWInst>(&instruction)) {
      if (!operation->isVolatile() || operation->getOrdering() != AtomicOrdering::Monotonic ||
          operation->getSyncScopeID() != SyncScope::SingleThread) return 1;
      ++atomics;
    } else if (auto *operation = dyn_cast<AtomicCmpXchgInst>(&instruction)) {
      if (!operation->isVolatile() || !operation->isWeak() ||
          operation->getSuccessOrdering() != AtomicOrdering::AcquireRelease ||
          operation->getFailureOrdering() != AtomicOrdering::Acquire ||
          operation->getSyncScopeID() != SyncScope::SingleThread) return 1;
      ++exchanges;
    }
  }
  std::printf("PHASE_IR_SEMANTICS loads=%u stores=%u rmw=%u cmpxchg=%u metadata=%u llvm_verify=PASS\n", loads, stores, atomics, exchanges, nativeMetadata);
  auto first = std::make_unique<Module>("stage_a", context);
  auto second = std::make_unique<Module>("stage_b", context);
  for (auto *stage : {first.get(), second.get()}) {
    d3dalloy::initializeModule(*stage);
    d3dalloy::dxil::InitializeAtomic64Phase(*stage, &phase);
    auto *entry = Function::Create(FunctionType::get(Type::getInt8PtrTy(context, 1), false),
      GlobalValue::ExternalLinkage, stage->getName(), *stage);
    IRBuilder<> body(BasicBlock::Create(context, "entry", entry));
    body.CreateRet(d3dalloy::dxil::EmitAtomic64Context(body));
  }
  if (Linker::linkModules(*first, std::move(second)) || verifyModule(*first, &errs())) return 1;
  unsigned contexts = 0;
  for (const auto &global : first->globals()) {
    if (!global.getName().startswith(d3dalloy::dxil::kAtomic64ContextGlobal)) continue;
    if (global.getName() != d3dalloy::dxil::kAtomic64ContextGlobal || !global.isExternallyInitialized()) return 1;
    ++contexts;
  }
  std::printf("PHASE_LINKED_CONTEXT count=%u llvm_verify=PASS\n", contexts);
  if (contexts != 1) return 1;
  for (unsigned addressSpace : {1u, 3u, 6u})
    for (bool exchange : {false, true}) {
      Module classifier("atomic64-address-space", context);
      auto *i64 = Type::getInt64Ty(context);
      auto *entry = Function::Create(FunctionType::get(Type::getVoidTy(context),
        {PointerType::get(i64, addressSpace)}, false), GlobalValue::ExternalLinkage, "classifier", classifier);
      IRBuilder<> body(BasicBlock::Create(context, "entry", entry));
      if (exchange)
        body.CreateAtomicCmpXchg(entry->getArg(0), body.getInt64(0), body.getInt64(1), Align(8),
          AtomicOrdering::SequentiallyConsistent, AtomicOrdering::SequentiallyConsistent);
      else
        body.CreateAtomicRMW(AtomicRMWInst::Add, entry->getArg(0), body.getInt64(1), Align(8),
          AtomicOrdering::SequentiallyConsistent);
      body.CreateRetVoid();
      if (verifyModule(classifier, &errs()) ||
          d3dalloy::dxil::HasDeviceAtomic64(classifier) != (addressSpace != 3)) return 1;
    }
  std::puts("PHASE_CLASSIFIER_AS1_AS6_DEVICE_AS3_GROUP_ONLY cases=6 llvm_verify=PASS");
  return loads == 1 && stores == 5 && atomics == 1 && exchanges == 1 && nativeMetadata == 1 ? 0 : 1;
}
