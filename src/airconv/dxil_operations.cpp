/*
 * DXIL `dx.op` arithmetic / math lowering (Shader Model 6.x -> AIR).
 *
 * Scope of this translation unit (owned-operation-lowering):
 *
 *   LowerOperations(llvm::Module &) rewrites every `dx.op` call this file owns into
 *   the equivalent AIR intrinsic call, in place, and erases the call. Everything it
 *   does not own is left byte identical so that the resource pass (createHandle,
 *   buffer/texture/sampler access, atomics) and the stage pass (loadInput /
 *   storeOutput, system values) can run independently. `dx.op` calls that nobody
 *   lowers survive this pass and are rejected by VerifyLoweredModule(), which is
 *   core owned.
 *
 * Dispatch rule (hard requirement): a `dx.op` call is identified by the callee
 *   *declaration* name prefix (`dx.op.`) and its first i32 argument, which is the
 *   opcode. The overload suffix (`f32`, `i32`, `v2f32`, part of the type name, ...)
 *   is only ever used to recover signedness/width hints; it is never the dispatch
 *   key. dx.op semantics are the on-disk ABI (DXIL.rst / DXC DxilOperations.h):
 *   Exp == 2^x, Log == base 2 logarithm, Round_ne == round-to-nearest-even.
 *
 * AIR mapping provenance: every `air.` name below was verified against Metal's own
 *   compiled output (`xcrun metal -std=metal3.1 --target=air64-apple-macos14.0`,
 *   disassembled), by compiling the equivalent MSL construct and reading back the
 *   emitted AIR intrinsics and signatures. Nothing here is guessed. Where MSL has no
 *   construct (coarse derivatives, non-execution barriers, several wave ops) the
 *   closest semantics-preserving AIR primitive is used and the deviation is called
 *   out in the comment above the lowering.
 *
 * Precision policy: the plain (`air.exp2`, `air.sqrt`, ...) AIR forms are the precise
 *   variants, the `air.fast_` forms the relaxed ones, exactly as Metal's front end
 *   selects them via `precise::`. DXIL carries a per-call `!dx.precise` attribute for
 *   values that must not be reassociated; we honour it. Half precision has no `fast_`
 *   form (Metal always emits `air.<op>.f16` for f16), so f16 always uses the plain
 *   name. The f32 default is the fast form, matching the existing SM5 lowering
 *   (`Converter::operator()(const InstFloatUnaryOp &)` uses AIRBuilder with
 *   FastVariant = true) and D3D's relaxed precision contract for these ops.
 *
 * FP64: DXIL double precision is *not* lowered here. The only semantics-exact FP64
 *   rewrite in the product is the UNORM24 pattern performed on the container by
 *   dxil_fp64_prepass.{h,cpp}; if a `dx.op` that inherently produces or consumes an
 *   f64 value survives into this pass (makeDouble/splitDouble, the i64/f64 bitcasts
 *   and the legacy double conversions) the compile fails with a precise diagnostic
 *   instead of emitting a silently wrong approximation.
 */

#include "dxil_converter.hpp"

#include "nt/air_builder.hpp"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace dxmt::dxil {

namespace {

namespace air = llvm::air;

/*
 * dx.op opcode ids. Names and numbers follow DXIL.rst and DXC's
 * DXIL/DxilOperations.h `enum class OpCode`; the numbers used by the corpus were
 * cross-checked against build/owned-direct-20260919/dxil-corpus-inventory.json.
 */
enum DxilOpcode : uint32_t {
  OpFAbs = 6,
  OpSaturate = 7,
  OpIsNan = 8,
  OpIsInf = 9,
  OpIsFinite = 10,
  OpIsNormal = 11,
  OpCos = 12,
  OpSin = 13,
  OpTan = 14,
  OpAcos = 15,
  OpAsin = 16,
  OpAtan = 17,
  OpHcos = 18,
  OpHsin = 19,
  OpHtan = 20,
  OpExp = 21, // 2^x, not e^x
  OpFrc = 22,
  OpLog = 23, // base 2, not natural
  OpSqrt = 24,
  OpRsqrt = 25,
  OpRound_ne = 26,
  OpRound_ni = 27,
  OpRound_pi = 28,
  OpRound_z = 29,
  OpBfrev = 30,
  OpCountbits = 31,
  OpFirstbitLo = 32,
  OpFirstbitHi = 33,
  OpFirstbitSHi = 34,
  OpFMax = 35,
  OpFMin = 36,
  OpIMax = 37,
  OpIMin = 38,
  OpUMax = 39,
  OpUMin = 40,
  OpIMul = 41,
  OpUMul = 42,
  OpUDiv = 43,
  OpUAddc = 44,
  OpUSubb = 45,
  OpFMad = 46,
  OpFma = 47,
  OpIMad = 48,
  OpUMad = 49,
  OpIbfe = 51,
  OpUbfe = 52,
  OpBfi = 53,
  OpDot2 = 54,
  OpDot3 = 55,
  OpDot4 = 56,
  OpBarrier = 80,
  OpDiscard = 82,
  OpDerivCoarseX = 83,
  OpDerivCoarseY = 84,
  OpDerivFineX = 85,
  OpDerivFineY = 86,
  OpMakeDouble = 101,
  OpSplitDouble = 102,
  OpWaveIsFirstLane = 110,
  OpWaveGetLaneIndex = 111,
  OpWaveGetLaneCount = 112,
  OpWaveAnyTrue = 113,
  OpWaveAllTrue = 114,
  OpWaveActiveAllEqual = 115,
  OpWaveActiveBallot = 116,
  OpWaveReadLaneAt = 117,
  OpWaveReadLaneFirst = 118,
  OpWaveActiveOp = 119,
  OpWaveActiveBit = 120,
  OpWavePrefixOp = 121,
  OpQuadReadLaneAt = 122,
  OpQuadOp = 123,
  OpBitcastI16toF16 = 124,
  OpBitcastF16toI16 = 125,
  OpBitcastI32toF32 = 126,
  OpBitcastF32toI32 = 127,
  OpBitcastI64toF64 = 128,
  OpBitcastF64toI64 = 129,
  OpLegacyF32ToF16 = 130,
  OpLegacyF16ToF32 = 131,
  OpLegacyDoubleToFloat = 132,
  OpLegacyDoubleToSInt32 = 133,
  OpLegacyDoubleToUInt32 = 134,
  OpWaveAllBitCount = 135,
  OpWavePrefixBitCount = 136,
};

/** DXIL barrier mode bits (`dxil::BarrierMode`); the operand is a bitmask. */
enum DxilBarrierBit : uint32_t {
  kBarrierSyncThreadGroup = 1u << 0,
  kBarrierAccessUAVGlobal = 1u << 1,
  kBarrierAccessUAVThreadGroup = 1u << 2,
  kBarrierAccessGroupShared = 1u << 3,
};

/* -------------------------------------------------------------------------- */
/* helpers                                                                    */
/* -------------------------------------------------------------------------- */

/** True for a call to a declared `dx.op.*` function; yields the leading i32 opcode. */
bool
getDxilOpcode(const CallInst &call, uint32_t &opcode) {
  if (call.arg_size() < 1)
    return false;
  const Function *callee = call.getCalledFunction();
  if (!callee || !callee->isDeclaration())
    return false;
  if (!callee->getName().startswith("dx.op."))
    return false;
  const auto *constant = dyn_cast<ConstantInt>(call.getArgOperand(0));
  if (!constant || constant->getBitWidth() != 32)
    return false;
  opcode = static_cast<uint32_t>(constant->getZExtValue());
  return true;
}

/** DXIL marks values that must keep their exact evaluation with `!dx.precise`. */
bool
isPreciseCall(const CallInst &call) {
  return call.getMetadata("dx.precise") != nullptr;
}

/** Last component of a dx.op symbol, e.g. "u32" in `dx.op.waveActiveOp.u32`. */
StringRef
overloadName(const Function &callee) {
  StringRef name = callee.getName();
  size_t dot = name.rfind('.');
  return dot == StringRef::npos ? name : name.substr(dot + 1);
}

/** Unsigned overloads carry a `u` prefix in the dx.op symbol ("u32", "u16", "u64"). */
bool
isUnsignedOverload(StringRef overload) {
  return overload.startswith("u");
}

/**
 * Signedness to encode in an AIR overload suffix. Integer AIR intrinsics are
 * emitted with an explicit `s`/`u` component by Metal whenever the source type was
 * signed/unsigned ("air.simd_sum.u.i32" vs "air.simd_sum.s.i32"), floating point
 * ones never are ("air.simd_shuffle.f32").
 */
air::Signedness
airSign(Type *type, bool is_unsigned) {
  if (type->isFloatingPointTy())
    return air::Signedness::DontCare;
  return is_unsigned ? air::Signedness::Unsigned : air::Signedness::Signed;
}

/** Full AIR symbol for a base name and its overload type. */
std::string
airSymbol(air::AIRBuilder &air_builder, StringRef base, Type *type, air::Signedness sign = air::Signedness::DontCare) {
  return (base + air_builder.getTypeOverloadSuffix(type, sign)).str();
}

/**
 * Emits one AIR intrinsic call. Attribute sets mirror AIRBuilder: pure math is
 * readnone, everything that depends on lane/quad state or orders memory is
 * convergent. The Metal front end emits the same two attribute groups.
 */
Value *
emitAIR(IRBuilder<> &builder, StringRef name, ArrayRef<Value *> args, Type *result, bool convergent) {
  LLVMContext &context = builder.getContext();
  AttributeList attributes =
    convergent
      ? AttributeList::get(
          context,
          {{~0U, Attribute::get(context, Attribute::Convergent)},
           {~0U, Attribute::get(context, Attribute::NoUnwind)},
           {~0U, Attribute::get(context, Attribute::WillReturn)}}
        )
      : AttributeList::get(
          context,
          {{~0U, Attribute::get(context, Attribute::ReadNone)},
           {~0U, Attribute::get(context, Attribute::NoUnwind)},
           {~0U, Attribute::get(context, Attribute::WillReturn)}}
        );

  SmallVector<Type *, 4> parameter_types;
  for (Value *arg : args)
    parameter_types.push_back(arg->getType());

  Module *module = builder.GetInsertBlock()->getModule();
  FunctionCallee callee =
    module->getOrInsertFunction(name, FunctionType::get(result, parameter_types, false), attributes);
  return builder.CreateCall(callee, args);
}

/** True when `ty` is a vector/struct/array of exactly `count` `element`. */
bool
aggregateMatches(Type *ty, unsigned count, Type *element) {
  if (auto *vector = dyn_cast<FixedVectorType>(ty))
    return vector->getNumElements() == count && vector->getElementType() == element;
  if (auto *structure = dyn_cast<StructType>(ty)) {
    if (structure->getNumElements() != count)
      return false;
    for (Type *field : structure->elements())
      if (field != element)
        return false;
    return true;
  }
  if (auto *array = dyn_cast<ArrayType>(ty))
    return array->getNumElements() == count && array->getElementType() == element;
  return false;
}

/** Builds a vector/struct/array value out of its components. */
Value *
buildAggregate(IRBuilder<> &builder, Type *ty, ArrayRef<Value *> components) {
  if (auto *vector = dyn_cast<FixedVectorType>(ty)) {
    Value *aggregate = PoisonValue::get(vector);
    for (unsigned i = 0; i < components.size(); ++i)
      aggregate = builder.CreateInsertElement(aggregate, components[i], builder.getInt64(i));
    return aggregate;
  }
  Value *aggregate = PoisonValue::get(ty);
  for (unsigned i = 0; i < components.size(); ++i)
    aggregate = builder.CreateInsertValue(aggregate, components[i], i);
  return aggregate;
}

/** `<N x T>` built from scalars of that same type. */
Value *
buildVector(IRBuilder<> &builder, Type *scalar, ArrayRef<Value *> components) {
  Value *vector = PoisonValue::get(FixedVectorType::get(scalar, components.size()));
  for (unsigned i = 0; i < components.size(); ++i)
    vector = builder.CreateInsertElement(vector, components[i], builder.getInt64(i));
  return vector;
}

/* -------------------------------------------------------------------------- */
/* lowering                                                                   */
/* -------------------------------------------------------------------------- */

class OperationLowerer {
public:
  explicit OperationLowerer(Module &module) : module(module), debug(nulls()) {}

  Error run();

private:
  Module &module;
  raw_ostream &debug;

  bool owns(uint32_t opcode) const;
  Error lowerCall(CallInst &call, uint32_t opcode);

  Error expectArgs(const CallInst &call, unsigned expected, StringRef what) const;
  Error expectAggregateResult(const CallInst &call, unsigned count, Type *element, StringRef what) const;

  Expected<Value *> lowerUnaryFloat(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  Expected<Value *> lowerFloatPredicate(CallInst &call, uint32_t opcode, IRBuilder<> &builder);
  Expected<Value *> lowerBitUnary(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  Expected<Value *> lowerMinMax(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  Expected<Value *> lowerWideArith(CallInst &call, uint32_t opcode, IRBuilder<> &builder);
  Expected<Value *> lowerMultiplyAdd(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  Expected<Value *> lowerBitField(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  Expected<Value *> lowerDot(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  Error lowerBarrier(CallInst &call, air::AIRBuilder &air);
  Error lowerDiscard(CallInst &call, IRBuilder<> &builder, air::AIRBuilder &air);
  Expected<Value *> lowerDerivative(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  Expected<Value *> lowerBitcast(CallInst &call, uint32_t opcode, IRBuilder<> &builder);
  Expected<Value *> lowerWave(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
};

Error
OperationLowerer::expectArgs(const CallInst &call, unsigned expected, StringRef what) const {
  if (call.arg_size() != expected + 1)
    return createError(what + " expects " + Twine(expected) + " operands, got " + Twine(call.arg_size() - 1));
  return Error::success();
}

Error
OperationLowerer::expectAggregateResult(const CallInst &call, unsigned count, Type *element, StringRef what) const {
  if (!aggregateMatches(call.getType(), count, element))
    return unsupported((what + " does not have the expected aggregate result type").str());
  return Error::success();
}

bool
OperationLowerer::owns(uint32_t opcode) const {
  switch (opcode) {
  /* unary float */
  case OpFAbs:
  case OpSaturate:
  case OpCos:
  case OpSin:
  case OpTan:
  case OpAcos:
  case OpAsin:
  case OpAtan:
  case OpHcos:
  case OpHsin:
  case OpHtan:
  case OpExp:
  case OpFrc:
  case OpLog:
  case OpSqrt:
  case OpRsqrt:
  case OpRound_ne:
  case OpRound_ni:
  case OpRound_pi:
  case OpRound_z:
  /* float predicates */
  case OpIsNan:
  case OpIsInf:
  case OpIsFinite:
  case OpIsNormal:
  /* integer bit ops */
  case OpBfrev:
  case OpCountbits:
  case OpFirstbitLo:
  case OpFirstbitHi:
  case OpFirstbitSHi:
  /* min/max */
  case OpFMax:
  case OpFMin:
  case OpIMax:
  case OpIMin:
  case OpUMax:
  case OpUMin:
  /* multiplies / division / carry */
  case OpIMul:
  case OpUMul:
  case OpUDiv:
  case OpUAddc:
  case OpUSubb:
  /* multiply-add */
  case OpFMad:
  case OpFma:
  case OpIMad:
  case OpUMad:
  /* bit field */
  case OpIbfe:
  case OpUbfe:
  case OpBfi:
  /* dot products */
  case OpDot2:
  case OpDot3:
  case OpDot4:
  /* sync / fragment */
  case OpBarrier:
  case OpDiscard:
  case OpDerivCoarseX:
  case OpDerivCoarseY:
  case OpDerivFineX:
  case OpDerivFineY:
  /* wave / quad */
  case OpWaveIsFirstLane:
  case OpWaveGetLaneIndex:
  case OpWaveGetLaneCount:
  case OpWaveAnyTrue:
  case OpWaveAllTrue:
  case OpWaveActiveAllEqual:
  case OpWaveActiveBallot:
  case OpWaveReadLaneAt:
  case OpWaveReadLaneFirst:
  case OpWaveActiveOp:
  case OpWaveActiveBit:
  case OpWavePrefixOp:
  case OpQuadReadLaneAt:
  case OpQuadOp:
  case OpWaveAllBitCount:
  case OpWavePrefixBitCount:
  /* bitcasts and half conversions */
  case OpBitcastI16toF16:
  case OpBitcastF16toI16:
  case OpBitcastI32toF32:
  case OpBitcastF32toI32:
  case OpLegacyF32ToF16:
  case OpLegacyF16ToF32:
  /* FP64: owned so that it is rejected with a precise message instead of being
   * approximated; see the file header. */
  case OpMakeDouble:
  case OpSplitDouble:
  case OpBitcastI64toF64:
  case OpBitcastF64toI64:
  case OpLegacyDoubleToFloat:
  case OpLegacyDoubleToSInt32:
  case OpLegacyDoubleToUInt32:
    return true;
  default:
    return false;
  }
}

Error
OperationLowerer::run() {
  SmallVector<std::pair<CallInst *, uint32_t>, 64> worklist;
  for (Function &function : module) {
    if (function.isDeclaration())
      continue;
    for (BasicBlock &block : function) {
      for (Instruction &instruction : block) {
        auto *call = dyn_cast<CallInst>(&instruction);
        uint32_t opcode = 0;
        if (call && getDxilOpcode(*call, opcode) && owns(opcode))
          worklist.emplace_back(call, opcode);
      }
    }
  }

  for (auto &[call, opcode] : worklist) {
    if (Error error = lowerCall(*call, opcode))
      return error;
  }

  /* Declarations of everything we lowered are dead now; drop them so that the
   * final module contains no `dx.op.` symbol at all. Declarations still in use
   * belong to other passes and are left alone. */
  SmallVector<Function *, 32> dead;
  for (Function &function : module) {
    if (function.isDeclaration() && function.getName().startswith("dx.op.") && function.use_empty())
      dead.push_back(&function);
  }
  for (Function *function : dead)
    function->eraseFromParent();

  return Error::success();
}

Error
OperationLowerer::lowerCall(CallInst &call, uint32_t opcode) {
  IRBuilder<> builder(&call);
  air::AIRBuilder air_builder(builder, debug);

  /* FP64 is rejected up front: the UNORM24 rewrite runs on the container before
   * the module is parsed, so any double that reaches this point is genuine
   * unsupported precision. */
  switch (opcode) {
  case OpMakeDouble:
  case OpSplitDouble:
  case OpBitcastI64toF64:
  case OpBitcastF64toI64:
  case OpLegacyDoubleToFloat:
  case OpLegacyDoubleToSInt32:
  case OpLegacyDoubleToUInt32:
    return unsupported(
      "dx.op FP64 operation; only the semantics-exact UNORM24 rewrite of "
      "dxil_fp64_prepass is supported"
    );
  default:
    break;
  }

  Expected<Value *> lowered = [&]() -> Expected<Value *> {
    switch (opcode) {
    case OpFAbs:
    case OpSaturate:
    case OpCos:
    case OpSin:
    case OpTan:
    case OpAcos:
    case OpAsin:
    case OpAtan:
    case OpHcos:
    case OpHsin:
    case OpHtan:
    case OpExp:
    case OpFrc:
    case OpLog:
    case OpSqrt:
    case OpRsqrt:
    case OpRound_ne:
    case OpRound_ni:
    case OpRound_pi:
    case OpRound_z:
      return lowerUnaryFloat(call, opcode, builder, air_builder);
    case OpIsNan:
    case OpIsInf:
    case OpIsFinite:
    case OpIsNormal:
      return lowerFloatPredicate(call, opcode, builder);
    case OpBfrev:
    case OpCountbits:
    case OpFirstbitLo:
    case OpFirstbitHi:
    case OpFirstbitSHi:
      return lowerBitUnary(call, opcode, builder, air_builder);
    case OpFMax:
    case OpFMin:
    case OpIMax:
    case OpIMin:
    case OpUMax:
    case OpUMin:
      return lowerMinMax(call, opcode, builder, air_builder);
    case OpIMul:
    case OpUMul:
    case OpUDiv:
    case OpUAddc:
    case OpUSubb:
      return lowerWideArith(call, opcode, builder);
    case OpFMad:
    case OpFma:
    case OpIMad:
    case OpUMad:
      return lowerMultiplyAdd(call, opcode, builder, air_builder);
    case OpIbfe:
    case OpUbfe:
    case OpBfi:
      return lowerBitField(call, opcode, builder, air_builder);
    case OpDot2:
    case OpDot3:
    case OpDot4:
      return lowerDot(call, opcode, builder, air_builder);
    case OpDerivCoarseX:
    case OpDerivCoarseY:
    case OpDerivFineX:
    case OpDerivFineY:
      return lowerDerivative(call, opcode, builder, air_builder);
    case OpBitcastI16toF16:
    case OpBitcastF16toI16:
    case OpBitcastI32toF32:
    case OpBitcastF32toI32:
    case OpLegacyF32ToF16:
    case OpLegacyF16ToF32:
      return lowerBitcast(call, opcode, builder);
    case OpWaveIsFirstLane:
    case OpWaveGetLaneIndex:
    case OpWaveGetLaneCount:
    case OpWaveAnyTrue:
    case OpWaveAllTrue:
    case OpWaveActiveAllEqual:
    case OpWaveActiveBallot:
    case OpWaveReadLaneAt:
    case OpWaveReadLaneFirst:
    case OpWaveActiveOp:
    case OpWaveActiveBit:
    case OpWavePrefixOp:
    case OpQuadReadLaneAt:
    case OpQuadOp:
    case OpWaveAllBitCount:
    case OpWavePrefixBitCount:
      return lowerWave(call, opcode, builder, air_builder);
    case OpBarrier:
      if (Error error = lowerBarrier(call, air_builder))
        return std::move(error);
      return nullptr;
    case OpDiscard:
      if (Error error = lowerDiscard(call, builder, air_builder))
        return std::move(error);
      return nullptr;
    default:
      return unsupported(("unhandled dx.op opcode " + Twine(opcode)).str());
    }
  }();

  if (!lowered)
    return lowered.takeError();
  Value *result = *lowered;

  if (result) {
    if (result->getType() != call.getType())
      return createError("dx.op lowering produced a mismatched result type");
    call.replaceAllUsesWith(result);
  } else if (!call.getType()->isVoidTy()) {
    return createError("dx.op operation produced no value but is declared to return one");
  } else if (!call.use_empty()) {
    return createError("dx.op operation without a result is used as a value");
  }

  call.eraseFromParent();
  return Error::success();
}

/* -------------------------------------------------------------------------- */
/* unary float                                                                */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerUnaryFloat(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  if (Error error = expectArgs(call, 1, "dx.op unary operation"))
    return std::move(error);

  Value *operand = call.getArgOperand(1);
  Type *type = operand->getType();
  const bool is_f32 = type->isFloatTy();
  if (!is_f32 && !type->isHalfTy())
    return unsupported("dx.op unary operation on a non floating point type");
  if (call.getType() != type)
    return createError("dx.op unary operation result type does not match its operand");

  /* Metal emits `air.fast_<op>.f32` for relaxed math and `air.<op>.f32` for
   * `precise::`; the half variants only exist without the `fast_` prefix. */
  const bool fast = is_f32 && !isPreciseCall(call);

  switch (opcode) {
  case OpFAbs:
    return air_builder.CreateFPUnOp(air::AIRBuilder::fabs, operand, fast);
  case OpSaturate:
    return air_builder.CreateFPUnOp(air::AIRBuilder::saturate, operand, fast);
  case OpCos:
    return air_builder.CreateFPUnOp(air::AIRBuilder::cos, operand, fast);
  case OpSin:
    return air_builder.CreateFPUnOp(air::AIRBuilder::sin, operand, fast);
  case OpExp:
    /* DXIL Exp is 2^x (`air.fast_exp2`), never the natural exponential. */
    return air_builder.CreateFPUnOp(air::AIRBuilder::exp2, operand, fast);
  case OpFrc:
    return air_builder.CreateFPUnOp(air::AIRBuilder::fract, operand, fast);
  case OpLog:
    /* DXIL Log is the base 2 logarithm (`air.fast_log2`). */
    return air_builder.CreateFPUnOp(air::AIRBuilder::log2, operand, fast);
  case OpSqrt:
    return air_builder.CreateFPUnOp(air::AIRBuilder::sqrt, operand, fast);
  case OpRsqrt:
    return air_builder.CreateFPUnOp(air::AIRBuilder::rsqrt, operand, fast);
  case OpRound_ne:
    /* Round half to even is IEEE rint; `air.fast_round` is half-away-from-zero
     * and would be wrong here. */
    return air_builder.CreateFPUnOp(air::AIRBuilder::rint, operand, fast);
  case OpRound_ni:
    return air_builder.CreateFPUnOp(air::AIRBuilder::floor, operand, fast);
  case OpRound_pi:
    return air_builder.CreateFPUnOp(air::AIRBuilder::ceil, operand, fast);
  case OpRound_z:
    return air_builder.CreateFPUnOp(air::AIRBuilder::trunc, operand, fast);
  /* AIRBuilder has no entry for the remaining unary ops. */
  case OpTan:
    return emitAIR(builder, airSymbol(air_builder, fast ? "air.fast_tan" : "air.tan", type), {operand}, type, false);
  case OpAcos:
    return emitAIR(builder, airSymbol(air_builder, fast ? "air.fast_acos" : "air.acos", type), {operand}, type, false);
  case OpAsin:
    return emitAIR(builder, airSymbol(air_builder, fast ? "air.fast_asin" : "air.asin", type), {operand}, type, false);
  case OpAtan:
    return emitAIR(builder, airSymbol(air_builder, fast ? "air.fast_atan" : "air.atan", type), {operand}, type, false);
  case OpHcos:
    return emitAIR(builder, airSymbol(air_builder, fast ? "air.fast_cosh" : "air.cosh", type), {operand}, type, false);
  case OpHsin:
    return emitAIR(builder, airSymbol(air_builder, fast ? "air.fast_sinh" : "air.sinh", type), {operand}, type, false);
  case OpHtan:
    return emitAIR(builder, airSymbol(air_builder, fast ? "air.fast_tanh" : "air.tanh", type), {operand}, type, false);
  default:
    return unsupported(("unhandled dx.op unary opcode " + Twine(opcode)).str());
  }
}

/* -------------------------------------------------------------------------- */
/* float predicates                                                           */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerFloatPredicate(CallInst &call, uint32_t opcode, IRBuilder<> &builder) {
  if (Error error = expectArgs(call, 1, "dx.op float predicate"))
    return std::move(error);

  Value *operand = call.getArgOperand(1);
  Type *type = operand->getType();
  if (!call.getType()->isIntegerTy(1))
    return createError("dx.op float predicate result is not i1");

  /* Bit exact implementations: AIR has no IsFinite/IsNormal intrinsic, and
   * comparing the raw exponent field is exactly what D3D specifies. */
  IntegerType *bits_type = nullptr;
  uint64_t magnitude_mask = 0;
  uint64_t infinity = 0;
  uint64_t smallest_normal = 0;
  if (type->isFloatTy()) {
    bits_type = builder.getInt32Ty();
    magnitude_mask = 0x7fffffffu;
    infinity = 0x7f800000u;
    smallest_normal = 0x00800000u;
  } else if (type->isHalfTy()) {
    bits_type = builder.getInt16Ty();
    magnitude_mask = 0x7fffu;
    infinity = 0x7c00u;
    smallest_normal = 0x0400u;
  } else {
    return unsupported("dx.op float predicate on a non floating point type");
  }

  Value *bits =
    builder.CreateAnd(builder.CreateBitCast(operand, bits_type), ConstantInt::get(bits_type, magnitude_mask));

  switch (opcode) {
  case OpIsNan:
    return builder.CreateICmpUGT(bits, ConstantInt::get(bits_type, infinity));
  case OpIsInf:
    return builder.CreateICmpEQ(bits, ConstantInt::get(bits_type, infinity));
  case OpIsFinite:
    return builder.CreateICmpULT(bits, ConstantInt::get(bits_type, infinity));
  case OpIsNormal:
    return builder.CreateAnd(
      builder.CreateICmpULT(bits, ConstantInt::get(bits_type, infinity)),
      builder.CreateICmpUGE(bits, ConstantInt::get(bits_type, smallest_normal))
    );
  default:
    return unsupported(("unhandled dx.op float predicate opcode " + Twine(opcode)).str());
  }
}

/* -------------------------------------------------------------------------- */
/* integer bit operations                                                     */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerBitUnary(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  if (Error error = expectArgs(call, 1, "dx.op bit operation"))
    return std::move(error);

  Value *operand = call.getArgOperand(1);
  Type *type = operand->getType();
  if (call.getType() != type)
    return createError("dx.op bit operation result type does not match its operand");
  if (!type->isIntegerTy() || (type->getIntegerBitWidth() != 16 && type->getIntegerBitWidth() != 32 &&
                              type->getIntegerBitWidth() != 64))
    return unsupported("dx.op bit operation on an unsupported integer width");

  const unsigned width = type->getIntegerBitWidth();
  /* 16 and 64 bit values are processed with the verified 32 bit AIR primitives:
   * widening a 16 bit value keeps the counting arithmetic valid, and a 64 bit
   * value is handled as two halves. This avoids depending on unverified 16/64 bit
   * AIR spellings. */
  Type *compute_type = width == 64 ? builder.getInt64Ty() : builder.getInt32Ty();
  Value *value = operand;
  if (width == 16)
    value = builder.CreateZExt(operand, builder.getInt32Ty());

  auto low_half = [&]() {
    return width == 64 ? builder.CreateTrunc(value, builder.getInt32Ty()) : value;
  };
  auto high_half = [&]() {
    return builder.CreateTrunc(builder.CreateLShr(value, 32), builder.getInt32Ty());
  };
  auto zero_result = [&](Value *count) {
    return builder.CreateSelect(
      builder.CreateICmpEQ(operand, ConstantInt::get(type, 0)), ConstantInt::getSigned(compute_type, -1), count
    );
  };

  Value *result = nullptr;
  switch (opcode) {
  case OpBfrev: {
    if (width == 64) {
      /* Bit reversal of 64 bits == reverse both 32 bit halves and swap them. */
      Value *low = air_builder.CreateIntUnOp(air::AIRBuilder::reverse_bits, low_half());
      Value *high = air_builder.CreateIntUnOp(air::AIRBuilder::reverse_bits, high_half());
      Value *shifted = builder.CreateShl(builder.CreateZExt(high, builder.getInt64Ty()), 32);
      result = builder.CreateOr(shifted, builder.CreateZExt(low, builder.getInt64Ty()));
    } else {
      result = air_builder.CreateIntUnOp(air::AIRBuilder::reverse_bits, value);
    }
    break;
  }
  case OpCountbits: {
    if (width == 64) {
      Value *low = air_builder.CreateIntUnOp(air::AIRBuilder::popcount, low_half());
      Value *high = air_builder.CreateIntUnOp(air::AIRBuilder::popcount, high_half());
      result = builder.CreateZExt(builder.CreateAdd(low, high), builder.getInt64Ty());
    } else {
      result = air_builder.CreateIntUnOp(air::AIRBuilder::popcount, value);
    }
    break;
  }
  case OpFirstbitLo: {
    /* AIR's ctz of zero is defined (32), so only the zero case needs fixing up:
     * DXIL returns -1 when no bit is set. */
    Value *count = nullptr;
    if (width == 64) {
      Value *low = air_builder.CreateCountZero(low_half(), /*TrailingZero=*/true);
      Value *high = air_builder.CreateCountZero(high_half(), /*TrailingZero=*/true);
      Value *high_index = builder.CreateAdd(high, builder.getInt32(32));
      Value *low_is_zero = builder.CreateICmpEQ(low_half(), builder.getInt32(0));
      count = builder.CreateZExt(builder.CreateSelect(low_is_zero, high_index, low), builder.getInt64Ty());
    } else {
      count = air_builder.CreateCountZero(value, true);
    }
    result = zero_result(count);
    break;
  }
  case OpFirstbitHi:
  case OpFirstbitSHi: {
    /* Index of the highest set bit, -1 when the value is zero. A negative two's
     * complement value has its sign bit set, so `width - 1 - clz` already yields
     * width - 1 there and both DXIL flavours share one implementation. */
    Value *count = nullptr;
    if (width == 64) {
      Value *low = air_builder.CreateCountZero(low_half(), /*TrailingZero=*/false);
      Value *high = air_builder.CreateCountZero(high_half(), /*TrailingZero=*/false);
      Value *high_is_zero = builder.CreateICmpEQ(high_half(), builder.getInt32(0));
      Value *low_index = builder.CreateSub(builder.getInt32(31), low);
      Value *high_index = builder.CreateSub(builder.getInt32(63), high);
      count = builder.CreateZExt(builder.CreateSelect(high_is_zero, low_index, high_index), builder.getInt64Ty());
    } else {
      count = builder.CreateSub(builder.getInt32(31), air_builder.CreateCountZero(value, false));
    }
    result = zero_result(count);
    break;
  }
  default:
    return unsupported(("unhandled dx.op bit operation opcode " + Twine(opcode)).str());
  }

  if (result->getType() != type)
    result = builder.CreateTrunc(result, type);
  return result;
}

/* -------------------------------------------------------------------------- */
/* min / max                                                                  */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerMinMax(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  if (Error error = expectArgs(call, 2, "dx.op min/max"))
    return std::move(error);

  Value *lhs = call.getArgOperand(1);
  Value *rhs = call.getArgOperand(2);
  Type *type = lhs->getType();
  if (type != rhs->getType() || type != call.getType())
    return createError("dx.op min/max operand types do not agree");

  switch (opcode) {
  case OpFMax:
  case OpFMin: {
    if (!type->isFloatTy() && !type->isHalfTy())
      return unsupported("dx.op float min/max on a non floating point type");
    const bool fast = type->isFloatTy() && !isPreciseCall(call);
    return air_builder.CreateFPBinOp(opcode == OpFMax ? air::AIRBuilder::fmax : air::AIRBuilder::fmin, lhs, rhs, fast);
  }
  case OpIMax:
  case OpIMin:
  case OpUMax:
  case OpUMin: {
    if (!type->isIntegerTy() || (type->getIntegerBitWidth() != 16 && type->getIntegerBitWidth() != 32 &&
                                 type->getIntegerBitWidth() != 64))
      return unsupported("dx.op integer min/max on an unsupported integer width");
    const bool is_signed = opcode == OpIMax || opcode == OpIMin;
    const bool is_max = opcode == OpIMax || opcode == OpUMax;
    return air_builder.CreateIntBinOp(is_max ? air::AIRBuilder::max : air::AIRBuilder::min, lhs, rhs, is_signed);
  }
  default:
    return unsupported(("unhandled dx.op min/max opcode " + Twine(opcode)).str());
  }
}

/* -------------------------------------------------------------------------- */
/* widening integer arithmetic                                                */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerWideArith(CallInst &call, uint32_t opcode, IRBuilder<> &builder) {
  if (Error error = expectArgs(call, 2, "dx.op integer arithmetic"))
    return std::move(error);

  Value *lhs = call.getArgOperand(1);
  Value *rhs = call.getArgOperand(2);
  Type *type = lhs->getType();
  if (!type->isIntegerTy(32) || rhs->getType() != type)
    return unsupported("dx.op integer arithmetic is only supported for the 32 bit overload");
  if (Error error = expectAggregateResult(call, 2, type, "dx.op integer arithmetic"))
    return std::move(error);

  Value *low = nullptr;
  Value *high = nullptr;
  switch (opcode) {
  case OpIMul: {
    Value *product = builder.CreateMul(builder.CreateSExt(lhs, builder.getInt64Ty()),
                                      builder.CreateSExt(rhs, builder.getInt64Ty()));
    low = builder.CreateTrunc(product, type);
    high = builder.CreateTrunc(builder.CreateAShr(product, 32), type);
    break;
  }
  case OpUMul: {
    Value *product = builder.CreateMul(builder.CreateZExt(lhs, builder.getInt64Ty()),
                                      builder.CreateZExt(rhs, builder.getInt64Ty()));
    low = builder.CreateTrunc(product, type);
    high = builder.CreateTrunc(builder.CreateLShr(product, 32), type);
    break;
  }
  case OpUDiv: {
    /* DXIL division by zero is undefined; LLVM's udiv poisons, which keeps that
     * contract without inventing a value. */
    low = builder.CreateUDiv(lhs, rhs);
    high = builder.CreateURem(lhs, rhs);
    break;
  }
  case OpUAddc: {
    low = builder.CreateAdd(lhs, rhs);
    high = builder.CreateZExt(builder.CreateICmpULT(low, lhs), type);
    break;
  }
  case OpUSubb: {
    low = builder.CreateSub(lhs, rhs);
    high = builder.CreateZExt(builder.CreateICmpUGT(rhs, lhs), type);
    break;
  }
  default:
    return unsupported(("unhandled dx.op integer arithmetic opcode " + Twine(opcode)).str());
  }

  return buildAggregate(builder, call.getType(), {low, high});
}

/* -------------------------------------------------------------------------- */
/* multiply-add                                                               */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerMultiplyAdd(
  CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder
) {
  if (Error error = expectArgs(call, 3, "dx.op multiply-add"))
    return std::move(error);

  Value *a = call.getArgOperand(1);
  Value *b = call.getArgOperand(2);
  Value *c = call.getArgOperand(3);
  Type *type = a->getType();
  if (b->getType() != type || c->getType() != type || call.getType() != type)
    return createError("dx.op multiply-add operand types do not agree");

  switch (opcode) {
  case OpFMad:
  case OpFma: {
    if (!type->isFloatTy() && !type->isHalfTy())
      return unsupported("dx.op fma on a non floating point type");
    /* `air.fma` is used unconditionally, like the SM5 lowering: DXIL FMad allows
     * a split multiply/add, Fma requires the fused form, and the fused form
     * satisfies both (defuseFma is a compilation argument, see
     * SM50_SHADER_FLAG_DEFUSE_FMA, that this signature does not receive). */
    Value *result = air_builder.CreateFMA(a, b, c);
    if (!result)
      return createError("failed to build air.fma");
    return result;
  }
  case OpIMad:
  case OpUMad:
    if (!type->isIntegerTy())
      return unsupported("dx.op imad on a non integer type");
    return builder.CreateAdd(builder.CreateMul(a, b), c);
  default:
    return unsupported(("unhandled dx.op multiply-add opcode " + Twine(opcode)).str());
  }
}

/* -------------------------------------------------------------------------- */
/* bit field extract / insert                                                 */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerBitField(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  const unsigned operands = opcode == OpBfi ? 4 : 3;
  if (Error error = expectArgs(call, operands, "dx.op bit field operation"))
    return std::move(error);

  Type *type = builder.getInt32Ty();
  for (unsigned i = 1; i <= operands; ++i) {
    if (call.getArgOperand(i)->getType() != type)
      return unsupported("dx.op bit field operation is only supported for the 32 bit overload");
  }
  if (call.getType() != type)
    return createError("dx.op bit field operation result is not i32");

  /* MSL's extract_bits/insert_bits (and therefore AIR's) define the same
   * out-of-range clamping as D3D's ubfe/ibfe/bfi. */
  switch (opcode) {
  case OpUbfe:
    return emitAIR(
      builder, airSymbol(air_builder, "air.extract_bits", type, air::Signedness::Unsigned),
      {call.getArgOperand(1), call.getArgOperand(2), call.getArgOperand(3)}, type, false
    );
  case OpIbfe:
    return emitAIR(
      builder, airSymbol(air_builder, "air.extract_bits", type, air::Signedness::Signed),
      {call.getArgOperand(1), call.getArgOperand(2), call.getArgOperand(3)}, type, false
    );
  case OpBfi:
    return emitAIR(
      builder, airSymbol(air_builder, "air.insert_bits", type, air::Signedness::Unsigned),
      {call.getArgOperand(1), call.getArgOperand(2), call.getArgOperand(3), call.getArgOperand(4)}, type, false
    );
  default:
    return unsupported(("unhandled dx.op bit field opcode " + Twine(opcode)).str());
  }
}

/* -------------------------------------------------------------------------- */
/* dot products                                                               */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerDot(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  const unsigned dimension = opcode == OpDot2 ? 2 : (opcode == OpDot3 ? 3 : 4);
  if (Error error = expectArgs(call, 2 * dimension, "dx.op dot product"))
    return std::move(error);

  /* DXIL passes the dot components as scalars, not as vectors. */
  Type *type = call.getArgOperand(1)->getType();
  if (!type->isFloatTy() && !type->isHalfTy())
    return unsupported("dx.op dot product on a non floating point type");
  for (unsigned i = 1; i <= 2 * dimension; ++i) {
    if (call.getArgOperand(i)->getType() != type)
      return createError("dx.op dot product operand types do not agree");
  }
  if (call.getType() != type)
    return createError("dx.op dot product result type does not match its operands");

  SmallVector<Value *, 4> lhs;
  SmallVector<Value *, 4> rhs;
  for (unsigned i = 0; i < dimension; ++i) {
    lhs.push_back(call.getArgOperand(1 + i));
    rhs.push_back(call.getArgOperand(1 + dimension + i));
  }

  Value *result = air_builder.CreateDotProduct(buildVector(builder, type, lhs), buildVector(builder, type, rhs));
  if (!result)
    return createError("failed to build air.dot");
  return result;
}

/* -------------------------------------------------------------------------- */
/* derivatives                                                                */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerDerivative(
  CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder
) {
  if (Error error = expectArgs(call, 1, "dx.op derivative"))
    return std::move(error);

  Value *operand = call.getArgOperand(1);
  if (!operand->getType()->isFloatTy() && !operand->getType()->isHalfTy())
    return unsupported("dx.op derivative on a non floating point type");
  if (call.getType() != operand->getType())
    return createError("dx.op derivative result type does not match its operand");

  /* AIR exposes a single derivative primitive per axis; the coarse DXIL flavours
   * (one value shared by the quad) fold onto it, because Apple's hardware
   * evaluates derivatives per 2x2 quad and air.dfdx/air.dfdy is the only
   * derivative intrinsic the driver accepts. */
  const bool y_axis = opcode == OpDerivCoarseY || opcode == OpDerivFineY;
  Value *result = air_builder.CreateDerivative(operand, y_axis);
  if (!result)
    return createError("failed to build air.dfd");
  return result;
}

/* -------------------------------------------------------------------------- */
/* barrier / discard                                                          */
/* -------------------------------------------------------------------------- */

Error
OperationLowerer::lowerBarrier(CallInst &call, air::AIRBuilder &air_builder) {
  if (Error error = expectArgs(call, 1, "dx.op barrier"))
    return error;
  if (!call.getType()->isVoidTy())
    return createError("dx.op barrier must not produce a value");

  const auto *mode_constant = dyn_cast<ConstantInt>(call.getArgOperand(1));
  if (!mode_constant || mode_constant->getBitWidth() != 32)
    return createError("dx.op barrier mode must be a constant i32");
  const uint32_t mode = static_cast<uint32_t>(mode_constant->getZExtValue());

  /* The DXIL mode is a bitmask: execution sync, device scoped UAV visibility,
   * thread group scoped UAV visibility and group shared memory ordering. AIR
   * expresses the same thing with `air.wg.barrier(<flags>, 1)` for the execution
   * barrier and `air.atomic.fence(<flags>, 5, <scope>)` for pure memory fences,
   * which is exactly how the SM5 path lowers `sync`
   * (Converter::operator()(const InstSync &) in nt/dxbc_converter_base.hpp).
   * The memory spaces a mode requests are never dropped, and no execution
   * barrier is invented where DXIL did not ask for one. */
  uint32_t memory_bits = static_cast<uint32_t>(air::MemFlags::None);
  uint32_t scope = static_cast<uint32_t>(air::ThreadScope::Threadgroup);
  const uint32_t device_memory =
    static_cast<uint32_t>(air::MemFlags::Device) | static_cast<uint32_t>(air::MemFlags::Texture);
  if (mode & kBarrierAccessGroupShared)
    memory_bits |= static_cast<uint32_t>(air::MemFlags::Threadgroup);
  if (mode & kBarrierAccessUAVThreadGroup)
    memory_bits |= device_memory;
  if (mode & kBarrierAccessUAVGlobal) {
    memory_bits |= device_memory;
    scope |= static_cast<uint32_t>(air::ThreadScope::Device);
  }
  const auto memory = static_cast<air::MemFlags>(memory_bits);

  if (mode == 0)
    return Error::success(); /* neither execution nor memory ordering requested */

  if (mode & kBarrierSyncThreadGroup) {
    /* Device scoped ordering is not expressible with the thread group barrier
     * flag alone; emit the fence first and then the execution barrier, mirroring
     * the SM5 lowering when non execution barriers are unavailable. */
    if (mode & kBarrierAccessUAVGlobal)
      air_builder.CreateAtomicFence(memory, air::ThreadScope(scope));
    air_builder.CreateBarrier(memory, /*SimdGroup=*/false);
    return Error::success();
  }

  air_builder.CreateAtomicFence(memory, air::ThreadScope(scope));
  return Error::success();
}

Error
OperationLowerer::lowerDiscard(CallInst &call, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  if (Error error = expectArgs(call, 1, "dx.op discard"))
    return error;
  if (!call.getType()->isVoidTy())
    return createError("dx.op discard must not produce a value");
  Value *condition = call.getArgOperand(1);
  if (!condition->getType()->isIntegerTy(1))
    return createError("dx.op discard condition must be i1");
  if (auto *constant = dyn_cast<ConstantInt>(condition)) {
    if (constant->isZero())
      return Error::success();
  } else {
    builder.SetInsertPoint(SplitBlockAndInsertIfThen(condition, &call, false));
  }
  air_builder.CreateDiscard();
  return Error::success();
}

/* -------------------------------------------------------------------------- */
/* bitcasts and half conversions                                              */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerBitcast(CallInst &call, uint32_t opcode, IRBuilder<> &builder) {
  if (Error error = expectArgs(call, 1, "dx.op conversion"))
    return std::move(error);

  Value *operand = call.getArgOperand(1);
  Type *result_type = call.getType();
  LLVMContext &context = builder.getContext();

  switch (opcode) {
  case OpBitcastI16toF16:
    if (!operand->getType()->isIntegerTy(16) || !result_type->isHalfTy())
      return unsupported("dx.op bitcast i16 to f16 with unexpected types");
    return builder.CreateBitCast(operand, result_type);
  case OpBitcastF16toI16:
    if (!operand->getType()->isHalfTy() || !result_type->isIntegerTy(16))
      return unsupported("dx.op bitcast f16 to i16 with unexpected types");
    return builder.CreateBitCast(operand, result_type);
  case OpBitcastI32toF32:
    if (!operand->getType()->isIntegerTy(32) || !result_type->isFloatTy())
      return unsupported("dx.op bitcast i32 to f32 with unexpected types");
    return builder.CreateBitCast(operand, result_type);
  case OpBitcastF32toI32:
    if (!operand->getType()->isFloatTy() || !result_type->isIntegerTy(32))
      return unsupported("dx.op bitcast f32 to i32 with unexpected types");
    return builder.CreateBitCast(operand, result_type);
  case OpLegacyF32ToF16: {
    /* The DXBC style conversion returns the half bit pattern in the low 16 bits
     * of an i32; fptrunc plus bitcast is what Metal itself emits for
     * float -> half. */
    if (!operand->getType()->isFloatTy() || !result_type->isIntegerTy(32))
      return unsupported("dx.op legacy f32 to f16 with unexpected types");
    Value *half = builder.CreateFPTrunc(operand, Type::getHalfTy(context));
    return builder.CreateZExt(builder.CreateBitCast(half, Type::getInt16Ty(context)), result_type);
  }
  case OpLegacyF16ToF32: {
    if (!operand->getType()->isIntegerTy(32) || !result_type->isFloatTy())
      return unsupported("dx.op legacy f16 to f32 with unexpected types");
    Value *half =
      builder.CreateBitCast(builder.CreateTrunc(operand, Type::getInt16Ty(context)), Type::getHalfTy(context));
    return builder.CreateFPExt(half, result_type);
  }
  default:
    return unsupported(("unhandled dx.op conversion opcode " + Twine(opcode)).str());
  }
}

/* -------------------------------------------------------------------------- */
/* wave / quad operations                                                     */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerWave(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  const Function *callee = call.getCalledFunction();
  const bool is_unsigned = callee && isUnsignedOverload(overloadName(*callee));

  /* Wave operations are convergent: they read lane state, so they must not be
   * moved or merged across control flow. */
  const bool convergent = true;

  auto lane_index = [&](Value *index) {
    /* AIR lane indices are 16 bit wide; DXIL passes them as i32. */
    return builder.CreateTrunc(index, builder.getInt16Ty());
  };
  auto supported_wave_type = [](Type *type) {
    return type->isFloatTy() || type->isHalfTy() || type->isIntegerTy(32) || type->isIntegerTy(64);
  };

  switch (opcode) {
  case OpWaveIsFirstLane: {
    if (Error error = expectArgs(call, 0, "dx.op waveIsFirstLane"))
      return std::move(error);
    if (!call.getType()->isIntegerTy(1))
      return createError("dx.op waveIsFirstLane result is not i1");
    return emitAIR(builder, "air.simd_is_first", {}, call.getType(), convergent);
  }
  case OpWaveGetLaneIndex: {
    if (Error error = expectArgs(call, 0, "dx.op waveGetLaneIndex"))
      return std::move(error);
    if (!call.getType()->isIntegerTy(32))
      return createError("dx.op waveGetLaneIndex result is not i32");
    return emitAIR(builder, "air.get_thread_index_in_simdgroup", {}, call.getType(), convergent);
  }
  case OpWaveGetLaneCount: {
    if (Error error = expectArgs(call, 0, "dx.op waveGetLaneCount"))
      return std::move(error);
    if (!call.getType()->isIntegerTy(32))
      return createError("dx.op waveGetLaneCount result is not i32");
    return emitAIR(builder, "air.get_simdgroup_size", {}, call.getType(), convergent);
  }
  case OpWaveAnyTrue:
  case OpWaveAllTrue: {
    if (Error error = expectArgs(call, 1, "dx.op wave vote"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    if (!condition->getType()->isIntegerTy(1) || !call.getType()->isIntegerTy(1))
      return createError("dx.op wave vote operates on i1");
    return emitAIR(
      builder, opcode == OpWaveAnyTrue ? "air.simd_any" : "air.simd_all", {condition}, call.getType(), convergent
    );
  }
  case OpWaveActiveAllEqual: {
    if (Error error = expectArgs(call, 1, "dx.op waveActiveAllEqual"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    Type *type = value->getType();
    if (!call.getType()->isIntegerTy(1))
      return createError("dx.op waveActiveAllEqual result is not i1");
    if (!supported_wave_type(type))
      return unsupported("dx.op waveActiveAllEqual on an unsupported type");
    /* All lanes equal == every lane matches the value held by the first lane. */
    Value *first =
      emitAIR(builder, airSymbol(air_builder, "air.simd_broadcast_first", type, airSign(type, is_unsigned)), {value},
              type, convergent);
    Value *equal =
      type->isFloatingPointTy() ? builder.CreateFCmpOEQ(value, first) : builder.CreateICmpEQ(value, first);
    return emitAIR(builder, "air.simd_all", {equal}, call.getType(), convergent);
  }
  case OpWaveActiveBallot: {
    if (Error error = expectArgs(call, 1, "dx.op waveActiveBallot"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    if (!condition->getType()->isIntegerTy(1))
      return createError("dx.op waveActiveBallot condition is not i1");
    if (Error error = expectAggregateResult(call, 4, builder.getInt32Ty(), "dx.op waveActiveBallot"))
      return std::move(error);
    /* AIR's ballot is a 64 bit mask, one bit per lane; DXIL returns the mask as
     * four 32 bit words with the unused high words zero. */
    Value *ballot = emitAIR(builder, "air.simd_ballot", {condition}, builder.getInt64Ty(), convergent);
    Value *low = builder.CreateTrunc(ballot, builder.getInt32Ty());
    Value *high = builder.CreateTrunc(builder.CreateLShr(ballot, 32), builder.getInt32Ty());
    Value *zero = builder.getInt32(0);
    return buildAggregate(builder, call.getType(), {low, high, zero, zero});
  }
  case OpWaveReadLaneAt:
  case OpQuadReadLaneAt: {
    if (Error error = expectArgs(call, 2, "dx.op wave lane read"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    Value *index = call.getArgOperand(2);
    Type *type = value->getType();
    if (call.getType() != type || !index->getType()->isIntegerTy(32))
      return createError("dx.op wave lane read operand types are invalid");
    if (!supported_wave_type(type))
      return unsupported("dx.op wave lane read on an unsupported type");
    StringRef base = opcode == OpQuadReadLaneAt ? "air.quad_shuffle" : "air.simd_shuffle";
    return emitAIR(
      builder, airSymbol(air_builder, base, type, airSign(type, is_unsigned)), {value, lane_index(index)}, type,
      convergent
    );
  }
  case OpWaveReadLaneFirst: {
    if (Error error = expectArgs(call, 1, "dx.op waveReadLaneFirst"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    Type *type = value->getType();
    if (call.getType() != type)
      return createError("dx.op waveReadLaneFirst result type does not match its operand");
    if (!supported_wave_type(type))
      return unsupported("dx.op waveReadLaneFirst on an unsupported type");
    return emitAIR(
      builder, airSymbol(air_builder, "air.simd_broadcast_first", type, airSign(type, is_unsigned)), {value}, type,
      convergent
    );
  }
  case OpWaveActiveOp:
  case OpWavePrefixOp: {
    if (Error error = expectArgs(call, 2, "dx.op wave reduction"))
      return std::move(error);
    const auto *kind = dyn_cast<ConstantInt>(call.getArgOperand(1));
    if (!kind)
      return createError("dx.op wave reduction kind must be a constant");
    Value *value = call.getArgOperand(2);
    Type *type = value->getType();
    if (call.getType() != type)
      return createError("dx.op wave reduction result type does not match its operand");
    const bool is_float = type->isFloatTy() || type->isHalfTy();
    if (!is_float && !type->isIntegerTy(32))
      return unsupported("dx.op wave reduction on an unsupported type");

    StringRef base;
    switch (static_cast<uint32_t>(kind->getZExtValue())) {
    case 0:
      base = opcode == OpWaveActiveOp ? "air.simd_sum" : "air.simd_prefix_exclusive_sum";
      break;
    case 1:
      base = opcode == OpWaveActiveOp ? "air.simd_product" : "air.simd_prefix_exclusive_product";
      break;
    case 2:
    case 3:
      /* AIR has no prefix min/max; DXIL's exclusive scan flavours are left for a
       * later pass instead of being approximated. */
      if (opcode == OpWavePrefixOp)
        return unsupported("dx.op wavePrefixOp min/max has no AIR equivalent");
      base = static_cast<uint32_t>(kind->getZExtValue()) == 2 ? "air.simd_min" : "air.simd_max";
      break;
    default:
      return createError("dx.op wave reduction has an unknown kind");
    }
    return emitAIR(builder, airSymbol(air_builder, base, type, airSign(type, is_unsigned)), {value}, type, convergent);
  }
  case OpWaveActiveBit: {
    if (Error error = expectArgs(call, 2, "dx.op waveActiveBit"))
      return std::move(error);
    const auto *kind = dyn_cast<ConstantInt>(call.getArgOperand(1));
    if (!kind)
      return createError("dx.op waveActiveBit kind must be a constant");
    Value *value = call.getArgOperand(2);
    Type *type = value->getType();
    if (call.getType() != type)
      return createError("dx.op waveActiveBit result type does not match its operand");
    if (!type->isIntegerTy(32) && !type->isIntegerTy(64))
      return unsupported("dx.op waveActiveBit on an unsupported type");
    StringRef base;
    switch (static_cast<uint32_t>(kind->getZExtValue())) {
    case 0:
      base = "air.simd_and";
      break;
    case 1:
      base = "air.simd_or";
      break;
    case 2:
      base = "air.simd_xor";
      break;
    default:
      return createError("dx.op waveActiveBit has an unknown kind");
    }
    /* Bitwise reductions are sign agnostic; the unsigned spelling is the one
     * Metal's front end emits. */
    return emitAIR(builder, airSymbol(air_builder, base, type, air::Signedness::Unsigned), {value}, type, convergent);
  }
  case OpQuadOp: {
    if (Error error = expectArgs(call, 2, "dx.op quadOp"))
      return std::move(error);
    const auto *kind = dyn_cast<ConstantInt>(call.getArgOperand(1));
    if (!kind)
      return createError("dx.op quadOp kind must be a constant");
    Value *value = call.getArgOperand(2);
    Type *type = value->getType();
    if (call.getType() != type)
      return createError("dx.op quadOp result type does not match its operand");
    if (!supported_wave_type(type))
      return unsupported("dx.op quadOp on an unsupported type");
    /* DXIL swaps across the 2x2 quad: 0 = horizontal (x), 1 = vertical (y),
     * 2 = diagonal, which in the quad lane layout is a lane XOR by 1, 2 or 3. */
    uint64_t lane_xor = 0;
    switch (static_cast<uint32_t>(kind->getZExtValue())) {
    case 0:
      lane_xor = 1;
      break;
    case 1:
      lane_xor = 2;
      break;
    case 2:
      lane_xor = 3;
      break;
    default:
      return createError("dx.op quadOp has an unknown swap kind");
    }
    return emitAIR(
      builder, airSymbol(air_builder, "air.quad_shuffle_xor", type, air::Signedness::Unsigned),
      {value, builder.getInt16(lane_xor)}, type, convergent
    );
  }
  case OpWaveAllBitCount:
  case OpWavePrefixBitCount: {
    if (Error error = expectArgs(call, 1, "dx.op wave bit count"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    if (!condition->getType()->isIntegerTy(1) || !call.getType()->isIntegerTy(32))
      return createError("dx.op wave bit count operates on i1 and returns i32");
    /* Counting true lanes is a sum of 0/1 across the wave, so the verified
     * sum/scan reductions implement it exactly without a ballot or lane index. */
    Value *counted = builder.CreateZExt(condition, builder.getInt32Ty());
    return emitAIR(
      builder, opcode == OpWaveAllBitCount ? "air.simd_sum.u.i32" : "air.simd_prefix_exclusive_sum.u.i32", {counted},
      call.getType(), convergent
    );
  }
  default:
    return unsupported(("unhandled dx.op wave opcode " + Twine(opcode)).str());
  }
}

} // namespace

llvm::Error
LowerOperations(llvm::Module &module) {
  OperationLowerer lowerer(module);
  return lowerer.run();
}

} // namespace dxmt::dxil
