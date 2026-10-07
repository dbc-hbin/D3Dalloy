// Link against the production libairconv.a with the native Meson test flags.
#include "nt/air_builder.hpp"
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>

int main() {
  using namespace llvm;
  using namespace llvm::air;
  LLVMContext context;
  context.setOpaquePointers(false);
  Module module("volume-sample-regression", context);
  IRBuilder<> ir(context);
  AIRBuilder air(ir, errs());
  Texture texture{Texture::texture3d, Texture::sample_float, Texture::access_sample};
  auto *f3 = FixedVectorType::get(ir.getFloatTy(), 3);
  auto *f4 = FixedVectorType::get(ir.getFloatTy(), 4);
  auto *i3 = FixedVectorType::get(ir.getInt32Ty(), 3);
  auto *result_type = StructType::get(f4, ir.getInt8Ty());
  auto *entry = Function::Create(FunctionType::get(result_type,
      {air.getTextureHandleType(texture), air.getSamplerHandleType(), f3, i3, ir.getFloatTy(),
       ir.getInt64Ty(), ir.getInt64Ty(), ir.getInt64Ty(), ir.getInt64Ty(), ir.getInt64Ty(), f4}, false),
      GlobalValue::ExternalLinkage, "sample_volume", module);
  ir.SetInsertPoint(BasicBlock::Create(context, "entry", entry));
  // Descriptor words and sampler state use the runtime qword/vector contracts;
  // no constants hide atlas branches or remove sampled values and status.
  texture.volume_layout = entry->getArg(5);
  texture.volume_view = entry->getArg(6);
  texture.volume_metadata = entry->getArg(7);
  texture.sampler_metadata = entry->getArg(8);
  texture.sampler_lod_range = entry->getArg(9);
  texture.sampler_float_border = entry->getArg(10);
  constexpr unsigned samples = 8;
  Value *value = Constant::getNullValue(f4);
  Value *status = ir.getInt8(0);
  for (unsigned i = 0; i < samples; ++i) {
    auto *coord = ir.CreateFAdd(entry->getArg(2), ConstantFP::get(f3, float(i) / samples));
    auto sampled = air.CreateSample(texture, entry->getArg(0), entry->getArg(1), coord, nullptr,
        entry->getArg(3), sample_level{entry->getArg(4)});
    value = ir.CreateFAdd(value, sampled.first);
    status = ir.CreateOr(status, sampled.second);
  }
  auto *packed = ir.CreateInsertValue(UndefValue::get(result_type), value, 0);
  ir.CreateRet(ir.CreateInsertValue(packed, status, 1));
  if (verifyModule(module, &errs())) return 1;

  unsigned helpers = 0, calls = 0, entry_loads = 0, helper_loads = 0;
  bool attributes = true;
  for (auto &function : module) {
    if (&function == entry || function.isDeclaration()) continue;
    ++helpers;
    attributes &= function.hasInternalLinkage() && function.hasFnAttribute(Attribute::NoInline);
    for (auto &instruction : instructions(function)) helper_loads += isa<LoadInst>(instruction);
  }
  for (auto &instruction : instructions(entry)) {
    entry_loads += isa<LoadInst>(instruction);
    if (auto *call = dyn_cast<CallInst>(&instruction)) {
      auto *callee = call->getCalledFunction();
      calls += callee && !callee->isDeclaration() && callee != entry;
    }
  }
  bool okay = helpers == 1 && calls == samples && attributes && !entry_loads && helper_loads;
  std::printf("VOLUME_SAMPLE_%s valid=1 samples=%u helpers=%u calls=%u noinline=%u entry_loads=%u helper_loads=%u\n",
      okay ? "PASS" : "FAIL", samples, helpers, calls, unsigned(attributes && helpers), entry_loads, helper_loads);
  return okay ? 0 : 1;
}
