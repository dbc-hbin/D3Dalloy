// AIR 2.8 adds a zero coordinate offset to 2D/2D-array/3D texture atomics.
// These signatures follow AIR emitted by the public Metal 4.0 frontend, not
// LLVM's permissive getOrInsertFunction type checks. An omitted offset can pass
// LLVM verification and fail native PSO creation under Shader Validation.
#include "airconv_context.hpp"
#include "dxbc_converter.hpp"
#include "nt/air_builder.hpp"
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <cstdio>
#include <string>

namespace {
using namespace llvm;
using namespace llvm::air;

struct Shape {
  const char *name;
  Texture::ResourceKind kind;
  unsigned dimensions;
  bool array;
  bool modern_offset;
  bool volume_dispatch = false;
};

constexpr Shape shapes[] = {
    {"texture_buffer_1d", Texture::texture_buffer, 1, false, false},
    {"texture_1d", Texture::texture1d, 1, false, false},
    {"texture_1d_array", Texture::texture1d_array, 1, true, false},
    {"texture_2d", Texture::texture2d, 2, false, true},
    {"texture_2d_array", Texture::texture2d_array, 2, true, true},
    {"texture_3d", Texture::texture3d, 3, false, true},
    {"volume_dispatch", Texture::texture3d, 3, false, true, true},
};

enum class Operation { Add, Min, Max, Exchange, CompareExchange };

const char *operationName(Operation operation) {
  switch (operation) {
  case Operation::Add: return "add";
  case Operation::Min: return "min";
  case Operation::Max: return "max";
  case Operation::Exchange: return "exchange";
  case Operation::CompareExchange: return "compare_exchange";
  }
  llvm_unreachable("unknown test operation");
}

bool integerConstant(Value *value, uint64_t expected) {
  auto *constant = dyn_cast<ConstantInt>(value);
  return constant && constant->getZExtValue() == expected;
}

bool checkCall(CallInst &call, const Shape &shape, bool modern, bool signed_type,
               Operation operation, Value *operand) {
  const bool cas = operation == Operation::CompareExchange;
  const bool offset = modern && shape.modern_offset;
  const unsigned expected_count = (cas ? 7u : 5u) + unsigned(shape.array) + unsigned(offset);
  std::string symbol = "air.atomic_";
  switch (operation) {
  case Operation::Add: symbol += "fetch_add_explicit_"; break;
  case Operation::Min: symbol += "fetch_min_explicit_"; break;
  case Operation::Max: symbol += "fetch_max_explicit_"; break;
  case Operation::Exchange: symbol += "exchange_explicit_"; break;
  case Operation::CompareExchange: symbol += "compare_exchange_weak_explicit_"; break;
  }
  symbol += shape.name;
  symbol += signed_type && (operation == Operation::Min || operation == Operation::Max)
      ? ".s.v4i32" : ".u.v4i32";
  bool okay = call.getCalledFunction()->getName() == symbol && call.arg_size() == expected_count;
  if (!okay) return false;

  auto &context = call.getContext();
  auto *word = Type::getInt32Ty(context);
  auto *vector = FixedVectorType::get(word, 4);
  auto *position = shape.dimensions == 1 ? static_cast<Type *>(word)
      : static_cast<Type *>(FixedVectorType::get(word, shape.dimensions));
  auto *handle = dyn_cast<PointerType>(call.getArgOperand(0)->getType());
  auto *opaque = handle && !handle->isOpaque()
      ? dyn_cast<StructType>(handle->getNonOpaquePointerElementType()) : nullptr;
  okay &= handle && handle->getAddressSpace() == 1 && opaque && opaque->isOpaque() &&
      opaque->getName() == "struct._" + std::string(shape.name) + "_t";
  okay &= call.getArgOperand(1)->getType() == position;
  unsigned index = 2;
  if (shape.array) okay &= call.getArgOperand(index++)->getType() == word;
  if (offset) {
    auto *zero = dyn_cast<Constant>(call.getArgOperand(index++));
    okay &= zero && zero->getType() == position && zero->isNullValue();
  }
  if (cas) {
    auto *expected = dyn_cast<PointerType>(call.getArgOperand(index++)->getType());
    okay &= expected && !expected->isOpaque() && expected->getAddressSpace() == 0 &&
        expected->getNonOpaquePointerElementType() == vector;
  }
  // Nonconstant shader values distinguish an omitted offset from a zero operand.
  okay &= call.getArgOperand(index++) == operand;
  okay &= integerConstant(call.getArgOperand(index++), 0); // relaxed order
  if (cas) okay &= integerConstant(call.getArgOperand(index++), 0); // failure order
  okay &= integerConstant(call.getArgOperand(index++), 3); // read_write access
  okay &= index == expected_count;
  okay &= call.getType() == (cas ? static_cast<Type *>(Type::getInt1Ty(context)) : vector);
  return okay;
}

bool runCase(SM50_SHADER_METAL_VERSION target, bool modern, const Shape &shape,
             bool signed_type, Operation operation) {
  LLVMContext context;
  context.setOpaquePointers(false);
  Module module("texture-atomic-abi", context);
  d3dalloy::initializeModule(module);
  d3dalloy::dxbc::setup_metal_version(module, target);
  IRBuilder<> ir(context);
  AIRBuilder air(ir, errs());
  Texture texture{shape.kind, signed_type ? Texture::sample_int : Texture::sample_uint,
                  Texture::acesss_readwrite};
  auto *vector = FixedVectorType::get(ir.getInt32Ty(), 4);
  auto *position = shape.dimensions == 1 ? static_cast<Type *>(ir.getInt32Ty())
      : static_cast<Type *>(FixedVectorType::get(ir.getInt32Ty(), shape.dimensions));
  auto *entry = Function::Create(FunctionType::get(vector,
      {air.getTextureHandleType(texture), position, vector, vector, ir.getInt32Ty(),
       ir.getInt64Ty(), ir.getInt64Ty(), ir.getInt64Ty()}, false),
      GlobalValue::ExternalLinkage, "atomic_probe", module);
  ir.SetInsertPoint(BasicBlock::Create(context, "entry", entry));
  if (shape.volume_dispatch) {
    // Dynamic descriptor words preserve both native-3D and sparse-2D-array arms.
    texture.volume_layout = entry->getArg(5);
    texture.volume_view = entry->getArg(6);
    texture.volume_metadata = entry->getArg(7);
  }
  auto *slice = shape.array ? entry->getArg(4) : nullptr;
  Value *result;
  if (operation == Operation::CompareExchange) {
    result = air.CreateAtomicCmpXchg(texture, entry->getArg(0), entry->getArg(1),
        entry->getArg(3), entry->getArg(2), slice).first;
  } else {
    auto opcode = AtomicRMWInst::Add;
    switch (operation) {
    case Operation::Add: break;
    case Operation::Min: opcode = signed_type ? AtomicRMWInst::Min : AtomicRMWInst::UMin; break;
    case Operation::Max: opcode = signed_type ? AtomicRMWInst::Max : AtomicRMWInst::UMax; break;
    case Operation::Exchange: opcode = AtomicRMWInst::Xchg; break;
    case Operation::CompareExchange: llvm_unreachable("handled above");
    }
    result = air.CreateAtomicRMW(texture, entry->getArg(0), opcode, entry->getArg(1),
        entry->getArg(2), slice);
  }
  if (!result) return false;
  ir.CreateRet(result);
  if (verifyModule(module, &errs())) return false;

  unsigned calls = 0, atlas_calls = 0, native_calls = 0;
  bool okay = true;
  for (auto &instruction : instructions(entry)) {
    auto *call = dyn_cast<CallInst>(&instruction);
    auto *callee = call ? call->getCalledFunction() : nullptr;
    if (!callee || !callee->getName().startswith("air.atomic_")) continue;
    ++calls;
    const auto &physical = shape.volume_dispatch && callee->getName().contains("texture_2d_array")
        ? shapes[4] : shape.volume_dispatch ? shapes[5] : shape;
    atlas_calls += physical.kind == Texture::texture2d_array;
    native_calls += physical.kind == Texture::texture3d;
    const bool call_okay = checkCall(*call, physical, modern, signed_type, operation, entry->getArg(2));
    okay &= call_okay;
    if (!call_okay) {
      errs() << "TEXTURE_ATOMIC_ABI_BAD_CALL target=" << unsigned(target) << " shape=" << shape.name
             << " signed=" << signed_type << " operation=" << operationName(operation) << '\n';
      call->print(errs());
      errs() << '\n';
    }
  }
  okay &= calls == (shape.volume_dispatch ? 2u : 1u);
  if (shape.volume_dispatch) okay &= atlas_calls == 1 && native_calls == 1;
  return okay;
}
} // namespace

int main() {
  struct Target { SM50_SHADER_METAL_VERSION version; bool modern; };
  constexpr Target targets[] = {{SM50_SHADER_METAL_310, false}, {SM50_SHADER_METAL_320, false},
                                {SM50_SHADER_METAL_400, true}, {SM50_SHADER_METAL_410, true}};
  constexpr Operation operations[] = {Operation::Add, Operation::Min, Operation::Max,
                                       Operation::Exchange, Operation::CompareExchange};
  unsigned total = 0, failures = 0;
  for (auto target : targets) {
    unsigned cases = 0, failed = 0;
    for (const auto &shape : shapes) for (bool signed_type : {false, true}) for (auto operation : operations) {
      const bool okay = runCase(target.version, target.modern, shape, signed_type, operation);
      ++cases;
      failed += !okay;
    }
    total += cases;
    failures += failed;
    std::printf("TEXTURE_ATOMIC_ABI target=%u cases=%u failures=%u %s\n",
                unsigned(target.version), cases, failed, failed ? "FAIL" : "PASS");
  }
  std::printf("TEXTURE_ATOMIC_ABI_%s cases=%u failures=%u\n", failures ? "FAIL" : "PASS", total, failures);
  return failures ? 1 : 0;
}
