/* DXIL binary64 is represented by its IEEE bits in i64 throughout AIR.
 * Stages/resources resolve their ABI first; this pass then remaps every LLVM
 * type, constant, aggregate, function signature and memory operation together.
 * Arithmetic calls the exact integer SoftFloat subset, never native float32.
 */
#include "dxil_converter.hpp"
#include "airconv_context.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;
namespace d3dalloy::dxil {
namespace {
class DoubleTypes final : public ValueMapTypeRemapper {
  DenseMap<Type *, Type *> mapped;
public:
  bool containsDouble(Type *type) {
    SmallPtrSet<Type *, 16> visited;
    SmallVector<Type *, 16> pending{type};
    while (!pending.empty()) {
      Type *next = pending.pop_back_val();
      if (!visited.insert(next).second) continue;
      if (next->isDoubleTy()) return true;
      for (Type *child : next->subtypes()) pending.push_back(child);
    }
    return false;
  }
  Type *remapType(Type *type) override {
    if (auto found = mapped.find(type); found != mapped.end()) return found->second;
    if (!containsDouble(type)) return type;
    Type *result = nullptr;
    if (type->isDoubleTy()) result = Type::getInt64Ty(type->getContext());
    else if (auto *vector = dyn_cast<FixedVectorType>(type))
      result = FixedVectorType::get(remapType(vector->getElementType()), vector->getNumElements());
    else if (auto *array = dyn_cast<ArrayType>(type))
      result = ArrayType::get(remapType(array->getElementType()), array->getNumElements());
    else if (auto *structure = dyn_cast<StructType>(type)) {
      auto *replacement = StructType::create(type->getContext(), structure->hasName() ? (structure->getName() + ".fp64").str() : "");
      mapped[type] = replacement;
      SmallVector<Type *, 8> fields;
      for (Type *field : structure->elements()) fields.push_back(remapType(field));
      replacement->setBody(fields, structure->isPacked());
      result = replacement;
    } else if (auto *pointer = dyn_cast<PointerType>(type))
      result = PointerType::get(remapType(pointer->getNonOpaquePointerElementType()), pointer->getAddressSpace());
    else if (auto *function = dyn_cast<FunctionType>(type)) {
      SmallVector<Type *, 8> arguments;
      for (Type *argument : function->params()) arguments.push_back(remapType(argument));
      result = FunctionType::get(remapType(function->getReturnType()), arguments, function->isVarArg());
    }
    assert(result && "DXIL has only fixed vectors and ordinary binary64 aggregate types");
    mapped[type] = result;
    return result;
  }
};

class DoubleLowerer {
  Module &module;
  DoubleTypes types;
  ValueToValueMapTy values;
  bool helpers = false;

  AttributeList remapAttributes(AttributeList attributes, unsigned count) {
    for (unsigned parameter = 0; parameter < count; ++parameter)
      for (Attribute attribute : attributes.getParamAttrs(parameter))
        if (attribute.isTypeAttribute())
          attributes = attributes.replaceAttributeTypeAtIndex(module.getContext(),
              parameter + AttributeList::FirstArgIndex, attribute.getKindAsEnum(), types.remapType(attribute.getValueAsType()));
    return attributes;
  }

  void mapConstant(Constant *constant) {
    if (values.count(constant) || isa<GlobalValue>(constant)) return;
    Type *type = types.remapType(constant->getType());
    if (auto *number = dyn_cast<ConstantFP>(constant); number && number->getType()->isDoubleTy()) {
      values[constant] = ConstantInt::get(module.getContext(), number->getValueAPF().bitcastToAPInt());
      return;
    }
    if (isa<PoisonValue>(constant)) { values[constant] = PoisonValue::get(type); return; }
    if (isa<UndefValue>(constant)) { values[constant] = UndefValue::get(type); return; }
    if (isa<ConstantAggregateZero>(constant)) { values[constant] = Constant::getNullValue(type); return; }
    if (type != constant->getType() && (type->isArrayTy() || type->isStructTy() || type->isVectorTy())) {
      unsigned count = type->isArrayTy() ? cast<ArrayType>(type)->getNumElements() :
          type->isStructTy() ? cast<StructType>(type)->getNumElements() : cast<FixedVectorType>(type)->getNumElements();
      SmallVector<Constant *, 16> fields;
      for (unsigned i = 0; i < count; ++i) {
        Constant *field = constant->getAggregateElement(i);
        mapConstant(field);
        fields.push_back(MapValue(field, values, RF_None, &types));
      }
      if (auto *array = dyn_cast<ArrayType>(type)) values[constant] = ConstantArray::get(array, fields);
      else if (auto *structure = dyn_cast<StructType>(type)) values[constant] = ConstantStruct::get(structure, fields);
      else values[constant] = ConstantVector::get(fields);
      return;
    }
    for (Value *operand : constant->operands()) if (auto *child = dyn_cast<Constant>(operand)) mapConstant(child);
  }

  Value *helper(IRBuilder<> &builder, StringRef operation, Type *result, ArrayRef<Value *> arguments) {
    SmallVector<Type *, 4> parameters;
    for (Value *argument : arguments) parameters.push_back(argument->getType());
    auto function = module.getOrInsertFunction(("d3dalloy.fp64." + operation).str(), FunctionType::get(result, parameters, false));
    helpers = true;
    return builder.CreateCall(function, arguments);
  }

  Value *compare(IRBuilder<> &builder, CmpInst::Predicate predicate, Value *a, Value *b) {
    auto *i64 = builder.getInt64Ty();
    auto magnitude = [&](Value *v) { return builder.CreateAnd(v, ConstantInt::get(i64, 0x7fffffffffffffffULL)); };
    Value *ma = magnitude(a), *mb = magnitude(b);
    Value *unordered = builder.CreateOr(builder.CreateICmpUGT(ma, ConstantInt::get(i64, 0x7ff0000000000000ULL)),
                                       builder.CreateICmpUGT(mb, ConstantInt::get(i64, 0x7ff0000000000000ULL)));
    Value *equal = builder.CreateOr(builder.CreateICmpEQ(a, b), builder.CreateICmpEQ(builder.CreateOr(ma, mb), builder.getInt64(0)));
    Value *sa = builder.CreateICmpSLT(a, builder.getInt64(0)), *sb = builder.CreateICmpSLT(b, builder.getInt64(0));
    Value *less = builder.CreateAnd(builder.CreateNot(equal), builder.CreateSelect(builder.CreateICmpNE(sa, sb), sa,
        builder.CreateSelect(sa, builder.CreateICmpUGT(ma, mb), builder.CreateICmpULT(ma, mb))));
    Value *relation = nullptr;
    switch (predicate) {
    case CmpInst::FCMP_FALSE: return builder.getFalse();
    case CmpInst::FCMP_TRUE: return builder.getTrue();
    case CmpInst::FCMP_ORD: return builder.CreateNot(unordered);
    case CmpInst::FCMP_UNO: return unordered;
    case CmpInst::FCMP_OEQ: case CmpInst::FCMP_UEQ: relation = equal; break;
    case CmpInst::FCMP_ONE: case CmpInst::FCMP_UNE: relation = builder.CreateNot(equal); break;
    case CmpInst::FCMP_OLT: case CmpInst::FCMP_ULT: relation = less; break;
    case CmpInst::FCMP_OLE: case CmpInst::FCMP_ULE: relation = builder.CreateOr(less, equal); break;
    case CmpInst::FCMP_OGT: case CmpInst::FCMP_UGT: relation = builder.CreateNot(builder.CreateOr(less, equal)); break;
    case CmpInst::FCMP_OGE: case CmpInst::FCMP_UGE: relation = builder.CreateNot(less); break;
    default: llvm_unreachable("not a floating predicate");
    }
    return CmpInst::isUnordered(predicate) ? builder.CreateOr(unordered, relation) : builder.CreateAnd(builder.CreateNot(unordered), relation);
  }

  Expected<Value *> scalar(IRBuilder<> &builder, const Instruction &original, ArrayRef<Value *> operands, Type *result) {
    Value *a = operands.empty() ? nullptr : operands[0];
    auto binary = [&](StringRef name) { return helper(builder, name, builder.getInt64Ty(), {a, operands[1]}); };
    auto round = [&](unsigned mode) { return helper(builder, "round", builder.getInt64Ty(), {a, builder.getInt32(mode)}); };
    auto fp32 = [&](Value *bits) -> Value * {
      Attribute mode = original.getFunction()->getFnAttribute("fp32-denorm-mode");
      if (mode.isStringAttribute() && mode.getValueAsString() == "preserve") return bits;
      Value *magnitude = builder.CreateAnd(bits, builder.getInt32(0x7fffffff));
      return builder.CreateSelect(builder.CreateICmpULT(magnitude, builder.getInt32(0x00800000)),
          builder.CreateAnd(bits, builder.getInt32(0x80000000)), bits);
    };
    switch (original.getOpcode()) {
    case Instruction::FAdd: return binary("add");
    case Instruction::FSub: return binary("sub");
    case Instruction::FMul: return binary("mul");
    case Instruction::FDiv: return binary("div");
    case Instruction::FRem: return binary("rem");
    case Instruction::FNeg: return builder.CreateXor(a, builder.getInt64(0x8000000000000000ULL));
    case Instruction::FCmp: return compare(builder, cast<FCmpInst>(original).getPredicate(), a, operands[1]);
    case Instruction::BitCast: return a->getType() == result ? a : builder.CreateBitCast(a, result);
    case Instruction::FPExt: {
      unsigned width = original.getOperand(0)->getType()->getScalarSizeInBits();
      if (width != 16 && width != 32) return unsupported("FP64 extension from a non-DXIL floating type");
      auto *bits = builder.getIntNTy(width);
      Value *input = builder.CreateBitCast(a, bits);
      if (width == 32) input = fp32(input);
      return helper(builder, width == 16 ? "from.f16" : "from.f32", builder.getInt64Ty(), {input});
    }
    case Instruction::FPTrunc: {
      unsigned width = result->getScalarSizeInBits();
      if (width != 16 && width != 32) return unsupported("FP64 truncation to a non-DXIL floating type");
      Value *output = helper(builder, width == 16 ? "to.f16" : "to.f32", builder.getIntNTy(width), {a});
      if (width == 32) output = fp32(output);
      return builder.CreateBitCast(output, result);
    }
    case Instruction::SIToFP: case Instruction::UIToFP: {
      bool sign = original.getOpcode() == Instruction::SIToFP;
      if (a->getType()->getIntegerBitWidth() > 64) return unsupported("FP64 conversion from integers wider than DXIL supports");
      Value *integer = sign ? builder.CreateSExtOrTrunc(a, builder.getInt64Ty()) : builder.CreateZExtOrTrunc(a, builder.getInt64Ty());
      return helper(builder, sign ? "from.i64" : "from.u64", builder.getInt64Ty(), {integer});
    }
    case Instruction::FPToSI: case Instruction::FPToUI: {
      bool sign = original.getOpcode() == Instruction::FPToSI;
      unsigned width = result->getIntegerBitWidth();
      if (width > 64) return unsupported("FP64 conversion to integers wider than DXIL supports");
      Type *integer = width <= 32 ? builder.getInt32Ty() : builder.getInt64Ty();
      StringRef name = width <= 32 ? (sign ? "to.i32" : "to.u32") : (sign ? "to.i64" : "to.u64");
      return builder.CreateTruncOrBitCast(helper(builder, name, integer, {a}), result);
    }
    case Instruction::Call: break;
    default: return static_cast<Value *>(nullptr); // Integer remapping is sufficient for phi/select/storage/aggregate instructions.
    }
    const auto &call = cast<CallInst>(original);
    Function *callee = call.getCalledFunction();
    if (!callee) return static_cast<Value *>(nullptr); // The remapped indirect signature retains the bits ABI.
    StringRef name = callee->getName();
    if (name.startswith("dx.op.")) {
      auto *opcode = dyn_cast<ConstantInt>(call.getArgOperand(0));
      if (!opcode) return createError("dxil: FP64 dx.op requires a constant opcode");
      operands = operands.drop_front();
      a = operands.empty() ? nullptr : operands[0];
      switch (opcode->getZExtValue()) {
      case 6: return builder.CreateAnd(a, builder.getInt64(0x7fffffffffffffffULL));
      case 7: {
        Value *zero = builder.getInt64(0), *one = builder.getInt64(0x3ff0000000000000ULL);
        return builder.CreateSelect(compare(builder, CmpInst::FCMP_OLT, a, one),
            builder.CreateSelect(compare(builder, CmpInst::FCMP_OGT, a, zero), a, zero),
            builder.CreateSelect(compare(builder, CmpInst::FCMP_UNO, a, a), zero, one));
      }
      case 8: return compare(builder, CmpInst::FCMP_UNO, a, a);
      case 9: return builder.CreateICmpEQ(builder.CreateAnd(a, builder.getInt64(0x7fffffffffffffffULL)), builder.getInt64(0x7ff0000000000000ULL));
      case 10: return builder.CreateICmpULT(builder.CreateAnd(a, builder.getInt64(0x7fffffffffffffffULL)), builder.getInt64(0x7ff0000000000000ULL));
      case 11: {
        Value *exponent = builder.CreateAnd(a, builder.getInt64(0x7ff0000000000000ULL));
        return builder.CreateAnd(builder.CreateICmpNE(exponent, builder.getInt64(0)), builder.CreateICmpNE(exponent, builder.getInt64(0x7ff0000000000000ULL)));
      }
      case 22: return helper(builder, "sub", builder.getInt64Ty(), {a, round(2)});
      case 24: return helper(builder, "sqrt", builder.getInt64Ty(), {a});
      case 25: return helper(builder, "div", builder.getInt64Ty(), {builder.getInt64(0x3ff0000000000000ULL), helper(builder, "sqrt", builder.getInt64Ty(), {a})});
      case 26: return round(0);
      case 27: return round(2);
      case 28: return round(3);
      case 29: return round(1);
      case 35: return binary("max");
      case 36: return binary("min");
      case 46: return helper(builder, "add", builder.getInt64Ty(), {binary("mul"), operands[2]});
      case 47: return helper(builder, "fma", builder.getInt64Ty(), {a, operands[1], operands[2]});
      case 101: return builder.CreateOr(builder.CreateZExt(a, builder.getInt64Ty()), builder.CreateShl(builder.CreateZExt(operands[1], builder.getInt64Ty()), 32));
      case 102: {
        Value *split = UndefValue::get(result);
        split = builder.CreateInsertValue(split, builder.CreateTrunc(a, builder.getInt32Ty()), 0);
        return builder.CreateInsertValue(split, builder.CreateTrunc(builder.CreateLShr(a, 32), builder.getInt32Ty()), 1);
      }
      case 132: return builder.CreateBitCast(fp32(helper(builder, "to.f32", builder.getInt32Ty(), {a})), result);
      case 128: case 129: return a;
      case 133: return helper(builder, "to.i32", builder.getInt32Ty(), {a});
      case 134: return helper(builder, "to.u32", builder.getInt32Ty(), {a});
      case 213: return helper(builder, "to.i64", builder.getInt64Ty(), {a});
      case 214: return helper(builder, "to.u64", builder.getInt64Ty(), {a});
      case 215: return helper(builder, "from.i64", builder.getInt64Ty(), {a});
      case 216: return helper(builder, "from.u64", builder.getInt64Ty(), {a});
      default:
        // Wave read/ballot/quad movement is bitwise and may be lowered by the wave pass.
        if (opcode->getZExtValue() == 115 || opcode->getZExtValue() == 117 || opcode->getZExtValue() == 118 ||
            opcode->getZExtValue() == 119 || opcode->getZExtValue() == 121 || opcode->getZExtValue() == 122 ||
            opcode->getZExtValue() == 123 || opcode->getZExtValue() == 165 || opcode->getZExtValue() == 166)
          return static_cast<Value *>(nullptr);
        return unsupported(("FP64 dx.op outside the DXIL double instruction contract: " + Twine(opcode->getZExtValue())).str());
      }
    }
    if (name.startswith("llvm.")) {
      if (name.startswith("llvm.fabs.")) return builder.CreateAnd(a, builder.getInt64(0x7fffffffffffffffULL));
      if (name.startswith("llvm.sqrt.")) return helper(builder, "sqrt", builder.getInt64Ty(), {a});
      if (name.startswith("llvm.fma.") || name.startswith("llvm.fmuladd.")) return helper(builder, "fma", builder.getInt64Ty(), {a, operands[1], operands[2]});
      if (name.startswith("llvm.floor.")) return round(2);
      if (name.startswith("llvm.ceil.")) return round(3);
      if (name.startswith("llvm.trunc.")) return round(1);
      if (name.startswith("llvm.rint.") || name.startswith("llvm.nearbyint.") || name.startswith("llvm.roundeven.")) return round(0);
      if (name.startswith("llvm.round.")) return round(4);
      if (name.startswith("llvm.copysign.")) return builder.CreateOr(builder.CreateAnd(a, builder.getInt64(0x7fffffffffffffffULL)), builder.CreateAnd(operands[1], builder.getInt64(0x8000000000000000ULL)));
      if (name.startswith("llvm.minnum.") || name.startswith("llvm.maxnum."))
        return binary(name.startswith("llvm.minnum.") ? "min" : "max");
      if (name.startswith("llvm.minimum.") || name.startswith("llvm.maximum.")) {
        Value *number = binary(name.startswith("llvm.minimum.") ? "min" : "max");
        Value *quietNaN = builder.CreateOr(builder.CreateSelect(compare(builder, CmpInst::FCMP_UNO, a, a), a, operands[1]),
            builder.getInt64(0x0008000000000000ULL));
        return builder.CreateSelect(compare(builder, CmpInst::FCMP_UNO, a, operands[1]), quietNaN, number);
      }
      return unsupported("FP64 LLVM intrinsic outside the DXIL double instruction contract");
    }
    if (callee->isDeclaration()) return unsupported("FP64 external call without a DXIL lowering contract");
    return static_cast<Value *>(nullptr);
  }

public:
  explicit DoubleLowerer(Module &module) : module(module) {}
  Error run() {
    bool present = false;
    for (GlobalVariable &global : module.globals()) present |= types.containsDouble(global.getValueType());
    for (Function &function : module) {
      present |= types.containsDouble(function.getFunctionType());
      for (Instruction &instruction : instructions(function)) {
        present |= types.containsDouble(instruction.getType());
        for (Value *operand : instruction.operands()) present |= types.containsDouble(operand->getType());
      }
    }
    if (!present) return Error::success();
    SmallVector<Function *, 32> originals;
    SmallVector<GlobalVariable *, 16> globals;
    for (Function &function : module) originals.push_back(&function);
    for (GlobalVariable &global : module.globals()) globals.push_back(&global);
    for (GlobalVariable *global : globals) {
      Type *type = types.remapType(global->getValueType());
      if (type == global->getValueType()) { values[global] = global; continue; }
      auto *replacement = new GlobalVariable(module, type, global->isConstant(), global->getLinkage(), nullptr,
          global->getName() + ".fp64", nullptr, global->getThreadLocalMode(), global->getAddressSpace());
      replacement->copyAttributesFrom(global);
      values[global] = replacement;
    }
    for (Function *function : originals) {
      auto *type = cast<FunctionType>(types.remapType(function->getFunctionType()));
      if (function->isDeclaration() && type == function->getFunctionType()) { values[function] = function; continue; }
      auto *replacement = Function::Create(type, function->getLinkage(), function->getAddressSpace(), function->getName() + ".fp64", &module);
      replacement->copyAttributesFrom(function);
      values[function] = replacement;
      auto argument = replacement->arg_begin();
      for (Argument &old : function->args()) { argument->setName(old.getName()); values[&old] = &*argument++; }
    }
    for (GlobalVariable *global : globals) if (global->hasInitializer()) mapConstant(global->getInitializer());
    for (Function *function : originals) for (Instruction &instruction : instructions(function))
      for (Value *operand : instruction.operands()) if (auto *constant = dyn_cast<Constant>(operand)) mapConstant(constant);
    for (GlobalVariable *global : globals) if (global->hasInitializer())
      cast<GlobalVariable>(values[global])->setInitializer(MapValue(global->getInitializer(), values, RF_None, &types));
    for (Function *function : originals) {
      auto *replacement = cast<Function>(values[function]);
      if (replacement == function || function->isDeclaration()) continue;
      SmallVector<ReturnInst *, 8> returns;
      CloneFunctionInto(replacement, function, values, CloneFunctionChangeType::GlobalChanges, returns, "", nullptr, &types);
    }
    for (Function *function : originals) {
      if (values[function] == function) continue;
      cast<Function>(values[function])->setAttributes(remapAttributes(function->getAttributes(), function->arg_size()));
      for (Instruction &original : instructions(function)) {
        if (auto *call = dyn_cast<CallBase>(values[&original]))
          call->setAttributes(remapAttributes(call->getAttributes(), call->arg_size()));
        bool arithmetic = original.getType()->getScalarType()->isDoubleTy();
        for (Value *operand : original.operands()) arithmetic |= operand->getType()->getScalarType()->isDoubleTy();
        if (!arithmetic) continue;
        auto *instruction = cast<Instruction>(values[&original]);
        if (auto *call = dyn_cast<CallInst>(instruction); call && call->getCalledFunction() &&
            call->getCalledFunction()->getName().startswith("dx.op."))
          call->setMetadata("d3dalloy.fp64", MDNode::get(module.getContext(), {}));
        IRBuilder<> builder(instruction);
        SmallVector<Value *, 8> operands;
        if (auto *call = dyn_cast<CallInst>(instruction)) for (Value *argument : call->args()) operands.push_back(argument);
        else for (Value *operand : instruction->operands()) operands.push_back(operand);
        Value *replacement = nullptr;
        auto *vector = dyn_cast<FixedVectorType>(original.getType());
        if (!vector) for (Value *operand : original.operands())
          if ((vector = dyn_cast<FixedVectorType>(operand->getType()))) break;
        // Bitcast may change lane count; storage/phi/aggregate operations are remapped intact.
        bool elementwise = original.isBinaryOp() || isa<FCmpInst>(original) || original.getOpcode() == Instruction::FNeg ||
            (isa<CastInst>(original) && original.getOpcode() != Instruction::BitCast) || isa<CallInst>(original);
        if (vector && elementwise && instruction->getType()->isVectorTy()) {
          auto *result = cast<FixedVectorType>(instruction->getType());
          replacement = UndefValue::get(result);
          for (unsigned lane = 0; lane < vector->getNumElements(); ++lane) {
            SmallVector<Value *, 8> arguments;
            for (Value *operand : operands) arguments.push_back(operand->getType()->isVectorTy() ? builder.CreateExtractElement(operand, lane) : operand);
            auto scalar_result = scalar(builder, original, arguments, result->getElementType());
            if (!scalar_result) return scalar_result.takeError();
            if (!*scalar_result) { replacement = nullptr; break; }
            replacement = builder.CreateInsertElement(replacement, *scalar_result, lane);
          }
        } else {
          auto lowered = scalar(builder, original, operands, instruction->getType());
          if (!lowered) return lowered.takeError();
          replacement = *lowered;
        }
        if (replacement) {
          replacement->takeName(instruction);
          instruction->replaceAllUsesWith(replacement);
          values[&original] = replacement;
          instruction->eraseFromParent();
        }
      }
    }
    // AIR temporarily names invocation-local allocations in module metadata.
    // LLVM's module mapper does not accept LocalAsMetadata unless already memoized.
    for (const auto &entry : values) {
      auto *metadata = ValueAsMetadata::getIfExists(const_cast<Value *>(entry.first));
      Value *mapped = entry.second;
      if (metadata && isa<LocalAsMetadata>(metadata) && mapped)
        values.MD()[metadata] = TrackingMDRef(ValueAsMetadata::get(mapped));
    }
    for (NamedMDNode &named : module.named_metadata())
      for (unsigned i = 0; i < named.getNumOperands(); ++i)
        named.setOperand(i, cast<MDNode>(MapMetadata(named.getOperand(i), values, RF_None, &types)));
    SmallVector<Function *, 16> unusedDeclarations;
    for (Function *function : originals) if (values[function] != function) {
      auto *replacement = cast<Function>(values[function]);
      replacement->takeName(function);
      function->dropAllReferences();
      if (replacement->isDeclaration() && replacement->use_empty()) unusedDeclarations.push_back(replacement);
    }
    for (Function *function : originals) if (values[function] != function) function->eraseFromParent();
    for (Function *function : unusedDeclarations) function->eraseFromParent();
    for (GlobalVariable *global : globals) if (values[global] != global) {
      cast<GlobalVariable>(values[global])->takeName(global);
      global->dropAllReferences();
      global->eraseFromParent();
    }
    if (helpers) d3dalloy::linkFP64(module);
    return Error::success();
  }
};
} // namespace
Error LowerFP64(Module &module) { return DoubleLowerer(module).run(); }
} // namespace d3dalloy::dxil
