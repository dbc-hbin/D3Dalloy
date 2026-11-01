// CPU-only full-module LLVM contracts; arithmetic bits are checked by test-fp64.sh and public SM6 consumers.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../src/airconv/dxil_converter.hpp"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/SourceMgr.h"
#include <cassert>
#include <cstdio>

int main() {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseAssemblyString(R"(
%Pair = type { double, [2 x double] }
@state = addrspace(3) global [2 x %Pair] [%Pair { double 1.0, [2 x double] [double 0x0000000000000001, double -0.0] }, %Pair zeroinitializer]
@vector = constant <2 x double> <double 1.0, double 0x7FF8000000000123>
declare double @llvm.sqrt.f64(double)
declare double @llvm.fma.f64(double, double, double)
declare double @llvm.minimum.f64(double, double)
declare double @llvm.maxnum.f64(double, double)
define double @choose(double %a, double %b, i1 %pick) #0 {
entry:
  %slot = alloca %Pair
  %ptr = getelementptr %Pair, %Pair* %slot, i32 0, i32 0
  br i1 %pick, label %left, label %right
left:
  %add = fadd double %a, 0x3FF0000000000001
  %sub = fsub double %add, %b
  %mul = fmul double %sub, %a
  %div = fdiv double %mul, %b
  %rem = frem double %div, %a
  br label %merge
right:
  %fma = call double @llvm.fma.f64(double %a, double %b, double -0.0)
  %sqrt = call double @llvm.sqrt.f64(double %fma)
  %minimum = call double @llvm.minimum.f64(double %sqrt, double %a)
  %maximum = call double @llvm.maxnum.f64(double %minimum, double %b)
  br label %merge
merge:
  %phi = phi double [ %rem, %left ], [ %maximum, %right ]
  store double %phi, double* %ptr
  %loaded = load double, double* %ptr
  %cmp = fcmp olt double %loaded, %a
  %select = select i1 %cmp, double %loaded, double %b
  %neg = fneg double %select
  %tofloat = fptrunc double %neg to float
  %fromfloat = fpext float %tofloat to double
  %tohalf = fptrunc double %fromfloat to half
  %fromhalf = fpext half %tohalf to double
  %toi = fptosi double %fromhalf to i64
  %tou = fptoui double %fromhalf to i32
  %fromi = sitofp i64 %toi to double
  %fromu = uitofp i32 %tou to double
  %pair = insertvalue %Pair undef, double %fromu, 0
  %extracted = extractvalue %Pair %pair, 0
  ret double %extracted
}
define double @byvalue(%Pair* byval(%Pair) %p) {
  %ptr = getelementptr %Pair, %Pair* %p, i32 0, i32 0
  %value = load double, double* %ptr
  ret double %value
}
define double @invoke(double %x, %Pair* %p) {
  %a = call double @byvalue(%Pair* byval(%Pair) %p)
  %b = call double @choose(double %a, double %x, i1 true)
  ret double %b
}
define <2 x double> @vectoradd(<2 x double> %a, <2 x double> %b) {
  %sum = fadd <2 x double> %a, %b
  ret <2 x double> %sum
}
define double @poisonvalue() { ret double poison }
attributes #0 = { "fp32-denorm-mode"="preserve" }
)", diagnostic, context);
  assert(module && "FP64 LLVM fixture must parse");
  llvm::Function *old = module->getFunction("choose");
  auto *allocation = llvm::cast<llvm::AllocaInst>(&old->getEntryBlock().front());
  // The same transient LocalAsMetadata shape used by raytracing stage storage.
  module->getOrInsertNamedMetadata("test.locals")->addOperand(llvm::MDNode::get(context,
      {llvm::ValueAsMetadata::get(old), llvm::ValueAsMetadata::get(allocation)}));
  if (llvm::Error error = d3dalloy::dxil::LowerFP64(*module)) {
    llvm::logAllUnhandledErrors(std::move(error), llvm::errs());
    return 1;
  }
  auto *locals = module->getNamedMetadata("test.locals");
  auto *function = llvm::cast<llvm::ValueAsMetadata>(locals->getOperand(0)->getOperand(0))->getValue();
  auto *storage = llvm::cast<llvm::ValueAsMetadata>(locals->getOperand(0)->getOperand(1))->getValue();
  assert(function == module->getFunction("choose"));
  assert(llvm::cast<llvm::Instruction>(storage)->getFunction() == function);
  module->eraseNamedMetadata(locals); // Transient function-local metadata is intentionally not serializable.
  assert(!llvm::verifyModule(*module, &llvm::errs()));
  auto *poison = llvm::cast<llvm::ReturnInst>(module->getFunction("poisonvalue")->getEntryBlock().getTerminator());
  assert(llvm::isa<llvm::PoisonValue>(poison->getReturnValue()));
  unsigned checks = 0;
  auto checkType = [&](llvm::Type *type) {
    llvm::SmallPtrSet<llvm::Type *, 16> seen;
    llvm::SmallVector<llvm::Type *, 16> pending{type};
    while (!pending.empty()) {
      llvm::Type *next = pending.pop_back_val();
      if (!seen.insert(next).second) continue;
      assert(!next->isDoubleTy());
      for (llvm::Type *child : next->subtypes()) pending.push_back(child);
      ++checks;
    }
  };
  for (llvm::GlobalVariable &global : module->globals()) checkType(global.getType());
  for (llvm::Function &function : *module) {
    checkType(function.getFunctionType());
    for (llvm::Instruction &instruction : llvm::instructions(function)) {
      checkType(instruction.getType());
      for (llvm::Value *operand : instruction.operands()) checkType(operand->getType());
    }
  }
  // Independently lowered exports are linked into the same native pipeline.
  for (const char *entry : {"export_a", "export_b"}) {
    auto stage = llvm::parseAssemblyString(R"(
define float @entry(double %value) {
  %converted = fptrunc double %value to float
  ret float %converted
}
)", diagnostic, context);
    assert(stage);
    stage->getFunction("entry")->setName(entry);
    if (llvm::Error error = d3dalloy::dxil::LowerFP64(*stage)) {
      llvm::logAllUnhandledErrors(std::move(error), llvm::errs());
      return 1;
    }
    assert(!llvm::Linker::linkModules(*module, std::move(stage)));
  }
  assert(!llvm::verifyModule(*module, &llvm::errs()));
  std::printf("FP64_LOWERING_PASS type_checks=%u locals=remapped poison=preserved exports=linked\n", checks);
}
