#include "logic_op.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

namespace dxmt::logic_op {
namespace {

Error invalid(const char *message) {
  return createStringError(inconvertibleErrorCode(), "logic op: %s", message);
}

Value *apply(IRBuilder<> &b, uint32_t op, Value *s, Value *d) {
  auto *ones = Constant::getAllOnesValue(s->getType());
  switch (op) {
  case 0: /* CLEAR */ return Constant::getNullValue(s->getType());
  case 1: /* SET */ return ones;
  case 2: /* COPY */ return s;
  case 3: /* COPY_INVERTED */ return b.CreateXor(s, ones);
  case 4: /* NOOP */ return d;
  case 5: /* INVERT */ return b.CreateXor(d, ones);
  case 6: /* AND */ return b.CreateAnd(s, d);
  case 7: /* NAND */ return b.CreateXor(b.CreateAnd(s, d), ones);
  case 8: /* OR */ return b.CreateOr(s, d);
  case 9: /* NOR */ return b.CreateXor(b.CreateOr(s, d), ones);
  case 10: /* XOR */ return b.CreateXor(s, d);
  case 11: /* EQUIV */ return b.CreateXor(b.CreateXor(s, d), ones);
  case 12: /* AND_REVERSE */ return b.CreateAnd(s, b.CreateXor(d, ones));
  case 13: /* AND_INVERTED */ return b.CreateAnd(b.CreateXor(s, ones), d);
  case 14: /* OR_REVERSE */ return b.CreateOr(s, b.CreateXor(d, ones));
  default: return b.CreateOr(b.CreateXor(s, ones), d); // 15: OR_INVERTED
  }
}

} // namespace

Error applyLogicOp(Module &module, const SM50_SHADER_D3D12_LOGIC_OP_DATA &options) {
  if (options.op > 15) return invalid("invalid D3D12_LOGIC_OP");
  auto *fragments = module.getNamedMetadata("air.fragment");
  if (!fragments || fragments->getNumOperands() != 1) return invalid("expected one native fragment entry");
  auto *signature = fragments->getOperand(0);
  auto *original = dyn_cast<Function>(cast<ValueAsMetadata>(signature->getOperand(0))->getValue());
  auto *outputs = dyn_cast<MDTuple>(signature->getOperand(1));
  auto *inputs = dyn_cast<MDTuple>(signature->getOperand(2));
  if (!original || !outputs || !inputs || inputs->getNumOperands() != original->arg_size())
    return invalid("inconsistent AIR fragment signature");
  auto &context = module.getContext();
  auto *returnType = dyn_cast<StructType>(original->getReturnType());
  if (returnType && returnType->getNumElements() != outputs->getNumOperands())
    return invalid("inconsistent AIR fragment outputs");

  struct Target {
    unsigned field;
    unsigned index;
  };
  SmallVector<Target, 8> targets;
  for (unsigned i = 0; returnType && i < outputs->getNumOperands(); ++i) {
    auto *output = cast<MDNode>(outputs->getOperand(i));
    auto *kind = dyn_cast<MDString>(output->getOperand(0));
    if (!kind || kind->getString() != "air.render_target" || output->getNumOperands() < 3) continue;
    const unsigned index = cast<ConstantInt>(cast<ConstantAsMetadata>(output->getOperand(1))->getValue())
                               ->getZExtValue();
    if (index >= 8 || !(options.rt_mask & (1u << index))) continue;
    auto *type = returnType->getElementType(i);
    auto *vector = dyn_cast<FixedVectorType>(type);
    if (!(vector ? vector->getElementType() : type)->isIntegerTy(32) ||
        (vector && vector->getNumElements() > 4))
      return invalid("logic op render target output is not a 32-bit integer scalar/vector");
    targets.push_back({i, index});
  }
  // A PS that writes no logic-op target leaves those targets unchanged: nothing to rewrite.
  if (targets.empty()) return Error::success();

  SmallVector<Type *, 24> parameters(original->getFunctionType()->param_begin(),
                                      original->getFunctionType()->param_end());
  SmallVector<Metadata *, 24> attributes(inputs->op_begin(), inputs->op_end());
  SmallVector<unsigned, 8> fetchArgs;
  for (const auto &target : targets) {
    fetchArgs.push_back(parameters.size());
    parameters.push_back(returnType->getElementType(target.field));
    const std::string name = "logic_op_destination_" + std::to_string(target.index);
    attributes.push_back(MDTuple::get(context, {
        ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), fetchArgs.back())),
        MDString::get(context, "air.render_target"),
        ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(context), target.index)),
        MDString::get(context, "air.arg_type_name"),
        MDString::get(context, isa<FixedVectorType>(returnType->getElementType(target.field))
            ? "uint" + std::to_string(cast<FixedVectorType>(returnType->getElementType(target.field))->getNumElements())
            : "uint"),
        MDString::get(context, "air.arg_name"), MDString::get(context, name)}));
  }
  auto *entry = Function::Create(FunctionType::get(returnType, parameters, false), Function::ExternalLinkage,
                                 "", module);
  entry->takeName(original);
  entry->setCallingConv(original->getCallingConv());
  entry->setAttributes(original->getAttributes());
  for (unsigned i = 0; i < original->arg_size(); ++i) {
    entry->getArg(i)->takeName(original->getArg(i));
    original->getArg(i)->replaceAllUsesWith(entry->getArg(i));
  }
  // Keep the application CFG intact, including discard, derivatives, UAV effects and
  // MSAA replay's publication/coverage gates. Only its actual return epilogues change.
  entry->getBasicBlockList().splice(entry->end(), original->getBasicBlockList());
  SmallVector<Constant *, 8> targetMasks;
  for (const auto &target : targets) {
    auto *vector = dyn_cast<FixedVectorType>(returnType->getElementType(target.field));
    SmallVector<Constant *, 4> masks;
    for (unsigned c = 0; c < (vector ? vector->getNumElements() : 1); ++c) {
      const unsigned bits = options.component_bits[target.index][c];
      masks.push_back(ConstantInt::get(Type::getInt32Ty(context), bits >= 32 ? ~0u : (1u << bits) - 1));
    }
    targetMasks.push_back(vector ? ConstantVector::get(masks) : masks.front());
  }
  for (auto &block : *entry) {
    auto *ret = dyn_cast<ReturnInst>(block.getTerminator());
    if (!ret) continue;
    IRBuilder<> builder(ret);
    Value *result = ret->getReturnValue();
    for (unsigned t = 0; t < targets.size(); ++t) {
      const auto &target = targets[t];
      auto *source = builder.CreateExtractValue(result, {target.field});
      auto *destination = entry->getArg(fetchArgs[t]);
      // Narrow UINT storage keeps low bits, not Metal's saturating conversion.
      auto *value = builder.CreateAnd(apply(builder, options.op, source, destination), targetMasks[t]);
      result = builder.CreateInsertValue(result, value, {target.field});
    }
    ret->setOperand(0, result);
  }

  SmallVector<Metadata *, 8> fields(signature->op_begin(), signature->op_end());
  fields[0] = ValueAsMetadata::get(entry);
  fields[2] = MDTuple::get(context, attributes);
  fragments->setOperand(0, MDTuple::get(context, fields));
  original->eraseFromParent();
  return Error::success();
}

} // namespace dxmt::logic_op
