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
 * FP64: LowerFP64 integerizes binary64 types/storage and lowers all double
 *   arithmetic through exact software helpers before this arithmetic pass.
 *
 * Waves: DXIL waves of 4/8/16 partition a physical Apple SIMD32; width32 uses
 *   native reductions where possible. Ballots, indices and lane loops use the
 *   selected virtual width, with no cross-SIMD barriers. Metal's
 *   simdgroup reductions include pixel shader helper threads, D3D wave operations do
 *   not (unless the entry carries `waveops-include-helper-lanes`), so in pixel
 *   shaders helper lanes are replaced by the reduction identity and masked out of
 *   votes and ballots. Quad operations include helper lanes in both APIs.
 */

#include "dxil_converter.hpp"

#include "airconv_context.hpp"
#include "nt/air_builder.hpp"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace d3dalloy::dxil {

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
  OpLegacyF32ToF16 = 130,
  OpLegacyF16ToF32 = 131,
  OpWaveAllBitCount = 135,
  OpWavePrefixBitCount = 136,
  OpDot2AddHalf = 162,
  OpDot4AddI8Packed = 163,
  OpDot4AddU8Packed = 164,
  OpWaveMatch = 165,
  OpWaveMultiPrefixOp = 166,
  OpWaveMultiPrefixBitCount = 167,
  OpUnpack4x8 = 219,
  OpPack4x8 = 220,
  OpIsHelperLane = 221,
  OpQuadVote = 222,
  OpBarrierByMemoryType = 244,
  OpVectorReduceAnd = 309,
  OpVectorReduceOr = 310,
  OpFDot = 311,
};

/** Physical Apple simdgroups; smaller DXIL waves partition these lanes. */
constexpr unsigned kNativeWaveLanes = 32;

/** DXIL::MemoryTypeFlag and DXIL::BarrierSemanticFlag (SM 6.8 barriers). */
enum DxilMemoryTypeBit : uint32_t {
  kMemoryTypeUAV = 1u << 0,
  kMemoryTypeGroupShared = 1u << 1,
};
enum DxilBarrierSemanticBit : uint32_t {
  kSemanticGroupSync = 1u << 0,
  kSemanticGroupScope = 1u << 1,
  kSemanticDeviceScope = 1u << 2,
};

/** Reduction kinds shared by the active, prefix and multi-prefix wave operations. */
enum class WaveReduce { Sum, Product, Min, Max, And, Or, Xor };

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
  /** The module's AIR entry is a fragment function (helper lanes exist). */
  bool pixel_stage = false;
  /** The DXIL entry carries `waveops-include-helper-lanes` (SM 6.7). */
  bool waves_include_helpers = false;
  unsigned wave_lanes = kNativeWaveLanes;
  unsigned wide_threads = 0;
  Value *wide_group_index = nullptr;
  GlobalVariable *wide_scratch = nullptr;
  Expected<Value *> lowerWideWave(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air);
  /** Compute, mesh and object functions form derivatives with explicit quad shuffles. */
  bool quad_derivative_stage = false;
  /** The AIR entry is a vertex function, where the lane-index and simdgroup-size intrinsics
   * are illegal ("vertex function uses illegal functions"). */
  bool vertex_stage = false;
  /** Vertex stage: the lane index of each function, computed once at its entry. */
  DenseMap<Function *, Value *> vertex_lanes;
  Value *laneIndex(IRBuilder<> &builder);

  bool owns(uint32_t opcode) const;
  Error lowerCall(CallInst &call, uint32_t opcode);
  /** Rewrites the fp32 arithmetic and comparisons of "preserve" functions (see run()). */
  void preserveDenormals();
  /** The function declares "fp32-denorm-mode"="preserve" (SM 6.2). */
  static bool preserves(const Function &function);
  /** fp32 add/sub/mul/div through the exact d3dalloy.denorm.* helpers. */
  Value *exactBinary(IRBuilder<> &builder, Instruction::BinaryOps op, Value *a, Value *b);
  /** fcmp evaluated on the IEEE ordering of the bit patterns: a denormal is not zero. */
  Value *exactCompare(IRBuilder<> &builder, CmpInst::Predicate predicate, Value *a, Value *b);
  /** Set once a d3dalloy.denorm.* helper is referenced; run() links air_denorm.metal. */
  bool uses_denorm_helpers = false;

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
  Expected<Value *> lowerPacked(CallInst &call, uint32_t opcode, IRBuilder<> &builder);
  Error lowerBarrierByMemoryType(CallInst &call, air::AIRBuilder &air);
  Error emitBarrier(bool sync, bool group_shared, bool uav, bool device_scope, air::AIRBuilder &air);
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
  case OpVectorReduceAnd:
  case OpVectorReduceOr:
  case OpFDot:
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
  case OpWaveMatch:
  case OpWaveMultiPrefixOp:
  case OpWaveMultiPrefixBitCount:
  case OpIsHelperLane:
  case OpQuadVote:
  /* SM 6.4 / 6.6 packed math */
  case OpDot2AddHalf:
  case OpDot4AddI8Packed:
  case OpDot4AddU8Packed:
  case OpUnpack4x8:
  case OpPack4x8:
  /* SM 6.8 barriers */
  case OpBarrierByMemoryType:
  /* bitcasts and half conversions */
  case OpBitcastI16toF16:
  case OpBitcastF16toI16:
  case OpBitcastI32toF32:
  case OpBitcastF32toI32:
  case OpLegacyF32ToF16:
  case OpLegacyF16ToF32:
    return true;
  default:
    return false;
  }
}

/*
 * The simdgroup lane index. Kernels and fragments use air.get_thread_index_in_simdgroup.
 * Vertex functions may not; there the active lanes at function entry are the contiguous
 * lanes 0..n-1 (M5 Pro probe: every partial draw of 1/7/33/45/100 vertices), so the lane
 * index is the exclusive prefix count taken once in the entry block.
 */
Value *
OperationLowerer::laneIndex(IRBuilder<> &builder) {
  Type *i32 = builder.getInt32Ty();
  if (!vertex_stage)
    return emitAIR(builder, "air.get_thread_index_in_simdgroup.i32", {}, i32, true);
  Function *function = builder.GetInsertBlock()->getParent();
  Value *&lane = vertex_lanes[function];
  if (!lane) {
    IRBuilder<> entry(&*function->getEntryBlock().getFirstInsertionPt());
    lane = emitAIR(entry, "air.simd_prefix_exclusive_sum.u.i32", {entry.getInt32(1)}, i32, true);
  }
  return lane;
}

Error
OperationLowerer::run() {
  if (Metadata *flag = module.getModuleFlag("d3dalloy.wave-size")) {
    auto *width = dyn_cast<ConstantAsMetadata>(flag);
    auto *constant = width ? dyn_cast<ConstantInt>(width->getValue()) : nullptr;
    if (!constant || !constant->getType()->isIntegerTy(32))
      return createError("DXIL selected wave width is not an i32 constant");
    wave_lanes = constant->getZExtValue();
    if (wave_lanes < 4 || wave_lanes > 128 || (wave_lanes & (wave_lanes - 1)))
      return createError("DXIL selected wave width is outside the supported set");
  }
  if (wave_lanes > kNativeWaveLanes) {
    auto *threads = mdconst::dyn_extract_or_null<ConstantInt>(module.getModuleFlag("d3dalloy.wide-wave-threads"));
    auto *kernels = module.getNamedMetadata("air.kernel");
    if (!threads || !kernels || kernels->getNumOperands() != 1)
      return createError("DXIL wide wave lowering requires the unconditional compute admission proof");
    wide_threads = threads->getZExtValue();
    auto *entry = kernels->getOperand(0);
    auto *function = cast<Function>(cast<ValueAsMetadata>(entry->getOperand(0))->getValue());
    auto *inputs = cast<MDNode>(entry->getOperand(2));
    for (unsigned i = 0; i < inputs->getNumOperands(); ++i) {
      auto *input = cast<MDNode>(inputs->getOperand(i));
      auto *kind = dyn_cast<MDString>(input->getOperand(1));
      if (kind && kind->getString() == "air.thread_index_in_threadgroup")
        wide_group_index = function->getArg(i);
    }
    if (!wide_group_index || !wide_threads || wide_threads > 1024)
      return createError("DXIL wide wave lowering is missing its group index or thread count");
    auto *array = ArrayType::get(Type::getInt64Ty(module.getContext()), wide_threads);
    wide_scratch = new GlobalVariable(module, array, false, GlobalValue::InternalLinkage,
      UndefValue::get(array), "d3dalloy.wide-wave-scratch", nullptr, GlobalValue::NotThreadLocal, 3);
    wide_scratch->setAlignment(Align(8));
  }
  if (auto *fragments = module.getNamedMetadata("air.fragment"))
    pixel_stage = fragments->getNumOperands() != 0;
  for (const char *stage : {"air.kernel", "air.mesh", "air.object"})
    if (auto *entries = module.getNamedMetadata(stage))
      quad_derivative_stage |= entries->getNumOperands() != 0;
  if (auto *vertices = module.getNamedMetadata("air.vertex"))
    vertex_stage = vertices->getNumOperands() != 0;
  for (Function &function : module)
    if (function.hasFnAttribute("waveops-include-helper-lanes"))
      waves_include_helpers = true;

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

  preserveDenormals();
  if (uses_denorm_helpers)
    d3dalloy::linkDenorm(module);
  /* The mode is fully applied; it is a DXIL attribute, not an AIR one. */
  for (Function &function : module)
    function.removeFnAttr("fp32-denorm-mode");
  SmallVector<Instruction *, 16> shared_atomics;
  for (Function &function : module) {
    for (BasicBlock &block : function) {
      for (Instruction &instruction : block) {
        Type *atomic_type = nullptr;
        Value *pointer = nullptr;
        if (auto *rmw = dyn_cast<AtomicRMWInst>(&instruction)) {
          atomic_type = rmw->getValOperand()->getType();
          pointer = rmw->getPointerOperand();
        } else if (auto *exchange = dyn_cast<AtomicCmpXchgInst>(&instruction)) {
          atomic_type = exchange->getNewValOperand()->getType();
          pointer = exchange->getPointerOperand();
        }
        if (atomic_type && atomic_type->isIntegerTy(64)) {
          if (pointer->getType()->getPointerAddressSpace() == 1 && module.getModuleFlag(kAtomic64PhaseFlag))
            continue; // The whole-operation phase pass legalizes device i64 atomics after sample guards.
          if (pointer->getType()->getPointerAddressSpace() != 3)
            return unsupported("unlowered 64 bit atomic outside groupshared storage");
          shared_atomics.push_back(&instruction);
        }
      }
    }
  }
  for (Instruction *instruction : shared_atomics) {
    auto *rmw = dyn_cast<AtomicRMWInst>(instruction);
    auto *exchange = dyn_cast<AtomicCmpXchgInst>(instruction);
    if (rmw) {
      switch (rmw->getOperation()) {
      case AtomicRMWInst::Add: case AtomicRMWInst::And: case AtomicRMWInst::Or:
      case AtomicRMWInst::Xor: case AtomicRMWInst::Min: case AtomicRMWInst::Max:
      case AtomicRMWInst::UMin: case AtomicRMWInst::UMax: case AtomicRMWInst::Xchg:
        break;
      default:
        return unsupported("unknown groupshared 64 bit atomic operation");
      }
    }
    IRBuilder<> builder(instruction);
    Value *pointer = rmw ? rmw->getPointerOperand() : exchange->getPointerOperand();
    Value *value = rmw ? rmw->getValOperand() : exchange->getNewValOperand();
    Value *old = EmitLockedAtomic64(builder, pointer, [&](Value *previous) -> Value * {
      if (exchange)
        return builder.CreateSelect(builder.CreateICmpEQ(previous, exchange->getCompareOperand()), value, previous);
      switch (rmw->getOperation()) {
      case AtomicRMWInst::Add: return builder.CreateAdd(previous, value);
      case AtomicRMWInst::And: return builder.CreateAnd(previous, value);
      case AtomicRMWInst::Or: return builder.CreateOr(previous, value);
      case AtomicRMWInst::Xor: return builder.CreateXor(previous, value);
      case AtomicRMWInst::Min: return builder.CreateSelect(builder.CreateICmpSLT(value, previous), value, previous);
      case AtomicRMWInst::Max: return builder.CreateSelect(builder.CreateICmpSGT(value, previous), value, previous);
      case AtomicRMWInst::UMin: return builder.CreateSelect(builder.CreateICmpULT(value, previous), value, previous);
      case AtomicRMWInst::UMax: return builder.CreateSelect(builder.CreateICmpUGT(value, previous), value, previous);
      default: return value;
      }
    });
    Value *result = old;
    if (exchange) {
      result = builder.CreateInsertValue(UndefValue::get(exchange->getType()), old, 0);
      result = builder.CreateInsertValue(result, builder.CreateICmpEQ(old, exchange->getCompareOperand()), 1);
    }
    instruction->replaceAllUsesWith(result);
    instruction->eraseFromParent();
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

/*
 * SM 6.2 "fp32-denorm-mode" (DXIL.rst "DXIL 1.2 Changes"; DXC wiki Denorm-Mode).
 * Apple GPUs flush fp32 denormals on input and output of every fp32 ALU operation,
 * comparisons included, and Metal offers no mode that preserves them
 * (air.compile.denorms_enable / `-fdenorms` change nothing on the GPU); fp16 keeps
 * its denormals natively. So:
 *   "any", "ftz": native arithmetic (flush to zero, sign preserved).
 *   "preserve": fp32 fadd/fsub/fmul/fdiv call the exact d3dalloy.denorm.* helpers
 *     (shaders/air_denorm.metal), fcmp compares IEEE bit orderings; FMad, dot and
 *     float wave sums/products were already expanded into those operations by their
 *     lowerings. Everything else is exact on bit patterns (fneg, fabs, loads/stores,
 *     bitcasts, conversions, IsNaN/IsInf/IsFinite/IsNormal) or explicitly allowed to
 *     flush by DXIL.rst (FMin/FMax "denorms are flushed before comparison", sqrt,
 *     rsqrt, the rounding ops, frc, log and the transcendental tables map ±denorm
 *     to the ±0 result).
 */
bool
OperationLowerer::preserves(const Function &function) {
  Attribute mode = function.getFnAttribute("fp32-denorm-mode");
  return mode.isStringAttribute() && mode.getValueAsString() == "preserve";
}

Value *
OperationLowerer::exactBinary(IRBuilder<> &builder, Instruction::BinaryOps op, Value *a, Value *b) {
  StringRef name;
  switch (op) {
  case Instruction::FAdd:
    name = "d3dalloy.denorm.fadd.f32";
    break;
  case Instruction::FSub:
    name = "d3dalloy.denorm.fsub.f32";
    break;
  case Instruction::FMul:
    name = "d3dalloy.denorm.fmul.f32";
    break;
  case Instruction::FDiv:
    name = "d3dalloy.denorm.fdiv.f32";
    break;
  default:
    return builder.CreateBinOp(op, a, b);
  }
  uses_denorm_helpers = true;
  Type *f32 = builder.getFloatTy();
  LLVMContext &context = builder.getContext();
  AttributeList attributes = AttributeList::get(context, {{~0U, Attribute::get(context, Attribute::ReadNone)},
                                                          {~0U, Attribute::get(context, Attribute::NoUnwind)},
                                                          {~0U, Attribute::get(context, Attribute::WillReturn)}});
  FunctionCallee helper = module.getOrInsertFunction(name, FunctionType::get(f32, {f32, f32}, false), attributes);
  return builder.CreateCall(helper, {a, b});
}

Value *
OperationLowerer::exactCompare(IRBuilder<> &builder, CmpInst::Predicate predicate, Value *a, Value *b) {
  Type *i32 = builder.getInt32Ty();
  if (predicate == CmpInst::FCMP_FALSE)
    return builder.getFalse();
  if (predicate == CmpInst::FCMP_TRUE)
    return builder.getTrue();
  auto magnitude = [&](Value *x) { return builder.CreateAnd(builder.CreateBitCast(x, i32), 0x7fffffffu); };
  /* The total order key: -|x| for negative x, |x| otherwise (so -0 == +0). */
  auto key = [&](Value *x, Value *m) {
    Value *negative = builder.CreateICmpSLT(builder.CreateBitCast(x, i32), builder.getInt32(0));
    return builder.CreateSelect(negative, builder.CreateNeg(m), m);
  };
  Value *ma = magnitude(a), *mb = magnitude(b);
  Value *unordered = builder.CreateOr(builder.CreateICmpUGT(ma, builder.getInt32(0x7f800000u)),
                                      builder.CreateICmpUGT(mb, builder.getInt32(0x7f800000u)));
  Value *ka = key(a, ma), *kb = key(b, mb);
  Value *relation = nullptr;
  switch (predicate) {
  case CmpInst::FCMP_OEQ:
  case CmpInst::FCMP_UEQ:
    relation = builder.CreateICmpEQ(ka, kb);
    break;
  case CmpInst::FCMP_ONE:
  case CmpInst::FCMP_UNE:
    relation = builder.CreateICmpNE(ka, kb);
    break;
  case CmpInst::FCMP_OLT:
  case CmpInst::FCMP_ULT:
    relation = builder.CreateICmpSLT(ka, kb);
    break;
  case CmpInst::FCMP_OLE:
  case CmpInst::FCMP_ULE:
    relation = builder.CreateICmpSLE(ka, kb);
    break;
  case CmpInst::FCMP_OGT:
  case CmpInst::FCMP_UGT:
    relation = builder.CreateICmpSGT(ka, kb);
    break;
  case CmpInst::FCMP_OGE:
  case CmpInst::FCMP_UGE:
    relation = builder.CreateICmpSGE(ka, kb);
    break;
  case CmpInst::FCMP_ORD:
    return builder.CreateNot(unordered);
  case CmpInst::FCMP_UNO:
    return unordered;
  default:
    return nullptr;
  }
  return CmpInst::isUnordered(predicate) ? builder.CreateOr(unordered, relation)
                                         : builder.CreateAnd(builder.CreateNot(unordered), relation);
}

void
OperationLowerer::preserveDenormals() {
  SmallVector<Instruction *, 64> rewrite;
  for (Function &function : module) {
    if (function.isDeclaration() || !preserves(function))
      continue;
    for (Instruction &instruction : instructions(function)) {
      if (auto *binary = dyn_cast<BinaryOperator>(&instruction)) {
        if (binary->getType()->isFloatTy() &&
            (binary->getOpcode() == Instruction::FAdd || binary->getOpcode() == Instruction::FSub ||
             binary->getOpcode() == Instruction::FMul || binary->getOpcode() == Instruction::FDiv))
          rewrite.push_back(binary);
      } else if (auto *compare = dyn_cast<FCmpInst>(&instruction)) {
        if (compare->getOperand(0)->getType()->isFloatTy())
          rewrite.push_back(compare);
      }
    }
  }
  for (Instruction *instruction : rewrite) {
    IRBuilder<> builder(instruction);
    Value *replacement = nullptr;
    if (auto *compare = dyn_cast<FCmpInst>(instruction)) {
      replacement = exactCompare(builder, compare->getPredicate(), compare->getOperand(0), compare->getOperand(1));
    } else {
      auto *binary = cast<BinaryOperator>(instruction);
      Value *a = binary->getOperand(0), *b = binary->getOperand(1);
      /* DXC spells negation `fsub -0.0, x`: a sign flip, exact on the bits. */
      auto *zero = dyn_cast<ConstantFP>(a);
      if (binary->getOpcode() == Instruction::FSub && zero && zero->isZero() && zero->isNegative())
        replacement = builder.CreateFNeg(b);
      else
        replacement = exactBinary(builder, binary->getOpcode(), a, b);
    }
    if (!replacement)
      continue;
    replacement->takeName(instruction);
    instruction->replaceAllUsesWith(replacement);
    instruction->eraseFromParent();
  }
}

Error
OperationLowerer::lowerCall(CallInst &call, uint32_t opcode) {
  IRBuilder<> builder(&call);
  air::AIRBuilder air_builder(builder, debug);

  // The SM6.9 elementwise overloads have exactly the scalar contract per component.
  // Reuse the scalar lowering, retaining precise metadata and IR fast-math flags.
  FixedVectorType *vector_type = nullptr;
  for (unsigned i = 1; i < call.arg_size(); ++i)
    if (auto *type = dyn_cast<FixedVectorType>(call.getArgOperand(i)->getType())) {
      vector_type = type;
      break;
    }
  if (vector_type && opcode != OpVectorReduceAnd && opcode != OpVectorReduceOr && opcode != OpFDot &&
      (opcode != OpWaveActiveAllEqual || call.getType()->isVectorTy()) && opcode != OpWaveMatch) {
    const bool elementwise = (opcode >= OpFAbs && opcode <= OpUMin) ||
        (opcode >= OpFMad && opcode <= OpUMad) ||
        (opcode >= OpDerivCoarseX && opcode <= OpDerivFineY) ||
        (opcode >= OpWaveReadLaneAt && opcode <= OpQuadOp) || opcode == OpWaveMultiPrefixOp ||
        opcode == OpWaveActiveAllEqual;
    auto *result_type = dyn_cast<FixedVectorType>(call.getType());
    if (!elementwise || !result_type || result_type->getNumElements() != vector_type->getNumElements() ||
        vector_type->getNumElements() < 2 || vector_type->getNumElements() > 1024)
      return createError("dx.op does not have a legal SM6.9 elementwise vector overload");
    SmallVector<Type *, 8> types;
    for (Value *argument : call.args()) {
      auto *type = dyn_cast<FixedVectorType>(argument->getType());
      if (type && type->getNumElements() != vector_type->getNumElements())
        return createError("dx.op vector operand lengths do not agree");
      types.push_back(type ? type->getElementType() : argument->getType());
    }
    // Never mutate a shared vector declaration: scalar calls have their own ABI.
    auto *function_type = FunctionType::get(result_type->getElementType(), types, false);
    auto scalar_callee = module.getOrInsertFunction(
        (call.getCalledFunction()->getName() + ".scalar").str(), function_type);
    Value *result = PoisonValue::get(result_type);
    for (unsigned component = 0; component < vector_type->getNumElements(); ++component) {
      SmallVector<Value *, 8> arguments;
      for (Value *argument : call.args())
        arguments.push_back(argument->getType()->isVectorTy()
            ? builder.CreateExtractElement(argument, component) : argument);
      auto *scalar = builder.CreateCall(scalar_callee, arguments);
      scalar->setAttributes(call.getAttributes());
      scalar->copyMetadata(call);
      scalar->copyIRFlags(&call);
      auto *next = builder.CreateInsertElement(result, scalar, component);
      if (Error error = lowerCall(*scalar, opcode))
        return std::move(error);
      result = next;
    }
    call.replaceAllUsesWith(result);
    call.eraseFromParent();
    return Error::success();
  }

  Expected<Value *> lowered = [&]() -> Expected<Value *> {
    switch (opcode) {
    case OpVectorReduceAnd:
    case OpVectorReduceOr:
    case OpFDot: {
      if (Error error = expectArgs(call, opcode == OpFDot ? 2 : 1, "dx.op vector reduction"))
        return std::move(error);
      auto *type = dyn_cast<FixedVectorType>(call.getArgOperand(1)->getType());
      if (!type || type->getNumElements() < 2 || type->getNumElements() > 1024 ||
          call.getType() != type->getElementType())
        return createError("dx.op vector reduction has invalid vector or result type");
      if (opcode == OpFDot) {
        if ((!call.getType()->isFloatTy() && !call.getType()->isHalfTy()) ||
            call.getArgOperand(2)->getType() != type)
          return createError("dx.op FDot requires two matching floating point vectors");
      } else if (!call.getType()->isIntegerTy()) {
        return createError("dx.op bitwise vector reduction requires integer components");
      }
      if (isa<FPMathOperator>(call))
        builder.setFastMathFlags(call.getFastMathFlags());
      if (isPreciseCall(call) || preserves(*call.getFunction()))
        builder.clearFastMathFlags();
      auto component = [&](unsigned i) -> Value * {
        Value *a = builder.CreateExtractElement(call.getArgOperand(1), i);
        return opcode == OpFDot ? builder.CreateFMul(a,
            builder.CreateExtractElement(call.getArgOperand(2), i)) : a;
      };
      Value *value = component(0);
      for (unsigned i = 1; i < type->getNumElements(); ++i)
        value = opcode == OpFDot ? builder.CreateFAdd(value, component(i))
            : (opcode == OpVectorReduceAnd ? builder.CreateAnd(value, component(i))
                                          : builder.CreateOr(value, component(i)));
      return value;
    }
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
    case OpWaveMatch:
    case OpWaveMultiPrefixOp:
    case OpWaveMultiPrefixBitCount:
    case OpIsHelperLane:
    case OpQuadVote:
      return lowerWave(call, opcode, builder, air_builder);
    case OpDot2AddHalf:
    case OpDot4AddI8Packed:
    case OpDot4AddU8Packed:
    case OpUnpack4x8:
    case OpPack4x8:
      return lowerPacked(call, opcode, builder);
    case OpBarrierByMemoryType:
      if (Error error = lowerBarrierByMemoryType(call, air_builder))
        return std::move(error);
      return nullptr;
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
  if (!type->isIntegerTy() || (type->getIntegerBitWidth() != 16 && type->getIntegerBitWidth() != 32 &&
                              type->getIntegerBitWidth() != 64))
    return unsupported("dx.op bit operation on an unsupported integer width");
  /* Bfrev is `unary` (result has the operand type); Countbits/Firstbit* are `unaryBits`,
   * whose result is i32 for every operand width. */
  Type *i32 = builder.getInt32Ty();
  if (call.getType() != (opcode == OpBfrev ? type : i32))
    return createError("dx.op bit operation result type does not match DXIL");

  const unsigned width = type->getIntegerBitWidth();
  /* 16 and 64 bit values are processed with the 32 bit AIR primitives: a 16 bit
   * value is widened, a 64 bit value is handled as two halves. */
  Value *value = width == 16 ? builder.CreateZExt(operand, i32) : operand;
  auto low_half = [&](Value *v) { return width == 64 ? builder.CreateTrunc(v, i32) : v; };
  auto high_half = [&](Value *v) { return builder.CreateTrunc(builder.CreateLShr(v, 32), i32); };
  auto if_zero = [&](Value *v, Value *count) {
    return builder.CreateSelect(builder.CreateICmpEQ(v, ConstantInt::get(v->getType(), 0)), builder.getInt32(-1),
                                count);
  };
  /* Leading zeros of `v` within the operand width; v is never zero where it is used. */
  auto leading_zeros = [&](Value *v) -> Value * {
    if (width == 64) {
      Value *high = high_half(v);
      Value *high_clz = air_builder.CreateCountZero(high, /*TrailingZero=*/false);
      Value *low_clz = builder.CreateAdd(air_builder.CreateCountZero(low_half(v), false), builder.getInt32(32));
      return builder.CreateSelect(builder.CreateICmpEQ(high, builder.getInt32(0)), low_clz, high_clz);
    }
    Value *clz = air_builder.CreateCountZero(v, false);
    return width == 16 ? builder.CreateSub(clz, builder.getInt32(16)) : clz;
  };

  switch (opcode) {
  case OpBfrev: {
    if (width == 64) {
      /* Reversing 64 bits reverses both 32 bit halves and swaps them. */
      Value *low = air_builder.CreateIntUnOp(air::AIRBuilder::reverse_bits, low_half(value));
      Value *high = air_builder.CreateIntUnOp(air::AIRBuilder::reverse_bits, high_half(value));
      return builder.CreateOr(builder.CreateShl(builder.CreateZExt(low, type), 32), builder.CreateZExt(high, type));
    }
    Value *reversed = air_builder.CreateIntUnOp(air::AIRBuilder::reverse_bits, value);
    return width == 16 ? builder.CreateTrunc(builder.CreateLShr(reversed, 16), type) : reversed;
  }
  case OpCountbits:
    if (width == 64)
      return builder.CreateAdd(air_builder.CreateIntUnOp(air::AIRBuilder::popcount, low_half(value)),
                               air_builder.CreateIntUnOp(air::AIRBuilder::popcount, high_half(value)));
    return air_builder.CreateIntUnOp(air::AIRBuilder::popcount, value);
  case OpFirstbitLo: {
    /* Index of the lowest set bit from the LSB, -1 for zero (AIR's ctz of zero is 32). */
    Value *count;
    if (width == 64) {
      Value *low = low_half(value);
      Value *high_index = builder.CreateAdd(air_builder.CreateCountZero(high_half(value), true), builder.getInt32(32));
      count = builder.CreateSelect(builder.CreateICmpEQ(low, builder.getInt32(0)), high_index,
                                   air_builder.CreateCountZero(low, true));
    } else {
      count = air_builder.CreateCountZero(value, true);
    }
    return if_zero(operand, count);
  }
  case OpFirstbitHi:
    /* DXIL counts from the MSB (the HLSL intrinsic is `width - 1 - result`), -1 for zero. */
    return if_zero(operand, leading_zeros(value));
  case OpFirstbitSHi: {
    /* The first bit that differs from the sign bit, counted from the MSB; -1 for 0 and -1. */
    Value *negative = builder.CreateICmpSLT(operand, ConstantInt::get(type, 0));
    Value *magnitude = builder.CreateSelect(negative, builder.CreateNot(operand), operand);
    Value *widened = width == 16 ? builder.CreateZExt(magnitude, i32) : magnitude;
    return if_zero(magnitude, leading_zeros(widened));
  }
  default:
    return unsupported(("unhandled dx.op bit operation opcode " + Twine(opcode)).str());
  }
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
  if (opcode == OpUAddc || opcode == OpUSubb) {
    auto *result = dyn_cast<StructType>(call.getType());
    if (!result || result->getNumElements() != 2 || result->getElementType(0) != type ||
        !result->getElementType(1)->isIntegerTy(1))
      return unsupported("dx.op carry arithmetic requires {i32, i1}");
  } else if (Error error = expectAggregateResult(call, 2, type, "dx.op integer arithmetic")) {
    return std::move(error);
  }

  Value *first = nullptr;
  Value *second = nullptr;
  switch (opcode) {
  case OpIMul: {
    Value *product = builder.CreateMul(builder.CreateSExt(lhs, builder.getInt64Ty()),
                                      builder.CreateSExt(rhs, builder.getInt64Ty()));
    first = builder.CreateTrunc(builder.CreateAShr(product, 32), type);
    second = builder.CreateTrunc(product, type);
    break;
  }
  case OpUMul: {
    Value *product = builder.CreateMul(builder.CreateZExt(lhs, builder.getInt64Ty()),
                                      builder.CreateZExt(rhs, builder.getInt64Ty()));
    first = builder.CreateTrunc(builder.CreateLShr(product, 32), type);
    second = builder.CreateTrunc(product, type);
    break;
  }
  case OpUDiv: {
    /* DXIL division by zero is undefined; LLVM's udiv poisons, which keeps that
     * contract without inventing a value. */
    first = builder.CreateUDiv(lhs, rhs);
    second = builder.CreateURem(lhs, rhs);
    break;
  }
  case OpUAddc: {
    first = builder.CreateAdd(lhs, rhs);
    second = builder.CreateICmpULT(first, lhs);
    break;
  }
  case OpUSubb: {
    first = builder.CreateSub(lhs, rhs);
    second = builder.CreateICmpULT(lhs, rhs);
    break;
  }
  default:
    return unsupported(("unhandled dx.op integer arithmetic opcode " + Twine(opcode)).str());
  }

  return buildAggregate(builder, call.getType(), {first, second});
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
    /* "fp32-denorm-mode"="preserve": FMad may be evaluated unfused, and the separate
     * multiply and add become exact denormal-preserving helpers in preserveDenormals(). */
    if (type->isFloatTy() && preserves(*call.getFunction()))
      return builder.CreateFAdd(builder.CreateFMul(a, b), c);
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

  // DXIL masks width/offset to five bits; AIR takes value/offset/width and
  // requires a range contained in the 32-bit value.
  Value *width = builder.CreateAnd(call.getArgOperand(1), builder.getInt32(31));
  Value *offset = builder.CreateAnd(call.getArgOperand(2), builder.getInt32(31));
  Value *available = builder.CreateSub(builder.getInt32(32), offset);
  width = builder.CreateSelect(builder.CreateICmpULT(width, available), width, available);
  switch (opcode) {
  case OpUbfe:
    return emitAIR(
      builder, airSymbol(air_builder, "air.extract_bits", type, air::Signedness::Unsigned),
      {call.getArgOperand(3), offset, width}, type, false
    );
  case OpIbfe:
    return emitAIR(
      builder, airSymbol(air_builder, "air.extract_bits", type, air::Signedness::Signed),
      {call.getArgOperand(3), offset, width}, type, false
    );
  case OpBfi:
    return emitAIR(
      builder, airSymbol(air_builder, "air.insert_bits", type, air::Signedness::Unsigned),
      {call.getArgOperand(4), call.getArgOperand(3), offset, width}, type, false
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

  /* "preserve": a multiply/add chain that preserveDenormals() makes exact. */
  if (type->isFloatTy() && preserves(*call.getFunction())) {
    Value *sum = builder.CreateFMul(lhs[0], rhs[0]);
    for (unsigned i = 1; i < dimension; ++i)
      sum = builder.CreateFAdd(sum, builder.CreateFMul(lhs[i], rhs[i]));
    return sum;
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
  if (quad_derivative_stage) {
    /*
     * SM 6.6 compute/mesh/amplification derivatives: air.dfdx/dfdy is fragment-only,
     * so the quad difference is formed explicitly. Quad lane q = 2*y + x
     * in Z order (the thread IDs were laid out that way by the stage signature).
     * Fine: right - left in the lane's own row (x) or bottom - top in its own column
     * (y). Coarse: the same difference taken from the quad's top-left row/column.
     */
    Type *type = operand->getType();
    auto shuffle = [&](Value *value, unsigned lane_xor) {
      return emitAIR(builder, airSymbol(air_builder, "air.quad_shuffle_xor", type), {value, builder.getInt16(lane_xor)},
                     type, true);
    };
    auto read = [&](Value *value, unsigned lane) {
      return emitAIR(builder, airSymbol(air_builder, "air.quad_shuffle", type), {value, builder.getInt16(lane)}, type,
                     true);
    };
    const bool coarse = opcode == OpDerivCoarseX || opcode == OpDerivCoarseY;
    if (coarse)
      return y_axis ? builder.CreateFSub(read(operand, 2), read(operand, 0))
                    : builder.CreateFSub(read(operand, 1), read(operand, 0));
    Value *lane = laneIndex(builder);
    Value *bit = builder.CreateICmpNE(builder.CreateAnd(lane, builder.getInt32(y_axis ? 2 : 1)), builder.getInt32(0));
    Value *other = shuffle(operand, y_axis ? 2 : 1);
    return builder.CreateSelect(bit, builder.CreateFSub(operand, other), builder.CreateFSub(other, operand));
  }
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

  if (mode & ~(kBarrierSyncThreadGroup | kBarrierAccessUAVGlobal | kBarrierAccessUAVThreadGroup |
               kBarrierAccessGroupShared))
    return createError("dx.op barrier mode has unknown bits");
  return emitBarrier(mode & kBarrierSyncThreadGroup, mode & kBarrierAccessGroupShared,
                     mode & (kBarrierAccessUAVGlobal | kBarrierAccessUAVThreadGroup), mode & kBarrierAccessUAVGlobal,
                     air_builder);
}

/*
 * The DXIL modes are execution sync plus the memory spaces to order and the scope
 * of that ordering. AIR expresses the same thing with `air.wg.barrier(<flags>, 1)`
 * for the execution barrier and `air.atomic.fence(<flags>, 5, <scope>)` for pure
 * memory fences, which is exactly how the SM5 path lowers `sync`
 * (Converter::operator()(const InstSync &) in nt/dxbc_converter_base.hpp). The
 * memory spaces requested are never dropped, and no execution barrier is invented
 * where DXIL did not ask for one.
 */
Error
OperationLowerer::emitBarrier(bool sync, bool group_shared, bool uav, bool device_scope, air::AIRBuilder &air_builder) {
  uint32_t memory_bits = static_cast<uint32_t>(air::MemFlags::None);
  uint32_t scope = static_cast<uint32_t>(air::ThreadScope::Threadgroup);
  const uint32_t device_memory =
    static_cast<uint32_t>(air::MemFlags::Device) | static_cast<uint32_t>(air::MemFlags::Texture);
  if (group_shared)
    memory_bits |= static_cast<uint32_t>(air::MemFlags::Threadgroup);
  if (uav)
    memory_bits |= device_memory;
  if (uav && device_scope)
    scope |= static_cast<uint32_t>(air::ThreadScope::Device);
  const auto memory = static_cast<air::MemFlags>(memory_bits);

  if (!sync && !memory_bits)
    return Error::success(); /* neither execution nor memory ordering requested */

  if (sync) {
    /* Device scoped ordering is not expressible with the thread group barrier
     * flag alone; emit the fence first and then the execution barrier, mirroring
     * the SM5 lowering when non execution barriers are unavailable. */
    if (uav && device_scope)
      air_builder.CreateAtomicFence(memory, air::ThreadScope(scope));
    air_builder.CreateBarrier(memory, /*SimdGroup=*/false);
    return Error::success();
  }

  air_builder.CreateAtomicFence(memory, air::ThreadScope(scope));
  return Error::success();
}

/*
 * SM 6.8 barrierByMemoryType (MemoryTypeFlags, SemanticFlags). Memory types: 1 UAV,
 * 2 group shared, 4 node input, 8 node output (work graphs; nothing to order
 * outside a node shader). Semantics: 1 group sync, 2 group scope, 4 device scope,
 * 8 reorder scope (hit-object reordering, which has no memory effect here).
 */
Error
OperationLowerer::lowerBarrierByMemoryType(CallInst &call, air::AIRBuilder &air_builder) {
  if (Error error = expectArgs(call, 2, "dx.op barrierByMemoryType"))
    return error;
  const auto *types = dyn_cast<ConstantInt>(call.getArgOperand(1));
  const auto *semantics = dyn_cast<ConstantInt>(call.getArgOperand(2));
  if (!types || !semantics)
    return createError("dx.op barrierByMemoryType flags must be constant");
  const uint32_t type_bits = static_cast<uint32_t>(types->getZExtValue());
  const uint32_t semantic_bits = static_cast<uint32_t>(semantics->getZExtValue());
  if (type_bits & ~0xfu || semantic_bits & ~0xfu)
    return createError("dx.op barrierByMemoryType has unknown flag bits");
  return emitBarrier(semantic_bits & kSemanticGroupSync, type_bits & kMemoryTypeGroupShared,
                     type_bits & kMemoryTypeUAV, semantic_bits & kSemanticDeviceScope, air_builder);
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

namespace {

/** Wave reduction identity; the value a non-participating lane contributes. */
Value *
waveIdentity(IRBuilder<> &builder, WaveReduce kind, Type *type, bool is_signed) {
  if (type->isFloatingPointTy()) {
    const fltSemantics &semantics = type->getFltSemantics();
    switch (kind) {
    case WaveReduce::Sum:
      return ConstantFP::get(type, 0.0);
    case WaveReduce::Product:
      return ConstantFP::get(type, 1.0);
    case WaveReduce::Min:
      return ConstantFP::get(type->getContext(), APFloat::getInf(semantics, false));
    case WaveReduce::Max:
      return ConstantFP::get(type->getContext(), APFloat::getInf(semantics, true));
    default:
      return nullptr;
    }
  }
  const unsigned width = type->getIntegerBitWidth();
  switch (kind) {
  case WaveReduce::Sum:
  case WaveReduce::Or:
  case WaveReduce::Xor:
    return ConstantInt::get(type, 0);
  case WaveReduce::Product:
    return ConstantInt::get(type, 1);
  case WaveReduce::And:
    return ConstantInt::get(type, APInt::getAllOnes(width));
  case WaveReduce::Min:
    return ConstantInt::get(type, is_signed ? APInt::getSignedMaxValue(width) : APInt::getMaxValue(width));
  case WaveReduce::Max:
    return ConstantInt::get(type, is_signed ? APInt::getSignedMinValue(width) : APInt::getMinValue(width));
  }
  return nullptr;
}

/** Scalar combine step of a wave reduction, bit exact with the native AIR reductions. */
Value *
waveCombine(IRBuilder<> &builder, air::AIRBuilder &air_builder, WaveReduce kind, Value *a, Value *b, bool is_signed) {
  const bool is_float = a->getType()->isFloatingPointTy();
  switch (kind) {
  case WaveReduce::Sum:
    return is_float ? builder.CreateFAdd(a, b) : builder.CreateAdd(a, b);
  case WaveReduce::Product:
    return is_float ? builder.CreateFMul(a, b) : builder.CreateMul(a, b);
  case WaveReduce::Min:
    if (is_float)
      return air_builder.CreateFPBinOp(air::AIRBuilder::fmin, a, b, false);
    return builder.CreateSelect(is_signed ? builder.CreateICmpSLT(a, b) : builder.CreateICmpULT(a, b), a, b);
  case WaveReduce::Max:
    if (is_float)
      return air_builder.CreateFPBinOp(air::AIRBuilder::fmax, a, b, false);
    return builder.CreateSelect(is_signed ? builder.CreateICmpSGT(a, b) : builder.CreateICmpUGT(a, b), a, b);
  case WaveReduce::And:
    return builder.CreateAnd(a, b);
  case WaveReduce::Or:
    return builder.CreateOr(a, b);
  case WaveReduce::Xor:
    return builder.CreateXor(a, b);
  }
  return nullptr;
}

/** AIR base name of a native simdgroup reduction or exclusive scan. */
StringRef
waveNativeName(WaveReduce kind, bool prefix) {
  switch (kind) {
  case WaveReduce::Sum:
    return prefix ? "air.simd_prefix_exclusive_sum" : "air.simd_sum";
  case WaveReduce::Product:
    return prefix ? "air.simd_prefix_exclusive_product" : "air.simd_product";
  case WaveReduce::Min:
    return prefix ? "" : "air.simd_min";
  case WaveReduce::Max:
    return prefix ? "" : "air.simd_max";
  case WaveReduce::And:
    return prefix ? "" : "air.simd_and";
  case WaveReduce::Or:
    return prefix ? "" : "air.simd_or";
  case WaveReduce::Xor:
    return prefix ? "" : "air.simd_xor";
  }
  return "";
}

} // namespace

Expected<Value *>
OperationLowerer::lowerWideWave(CallInst &call, uint32_t opcode, IRBuilder<> &b, air::AIRBuilder &air) {
  Value *lane = b.CreateAnd(wide_group_index, b.getInt32(wave_lanes - 1));
  Value *base = b.CreateAnd(wide_group_index, b.getInt32(~(wave_lanes - 1)));
  Value *remaining = b.CreateSub(b.getInt32(wide_threads), base);
  Value *active = b.CreateSelect(b.CreateICmpULT(remaining, b.getInt32(wave_lanes)), remaining, b.getInt32(wave_lanes));
  if (opcode == OpWaveGetLaneCount || opcode == OpWaveGetLaneIndex || opcode == OpWaveIsFirstLane) {
    if (auto error = expectArgs(call, 0, "dx.op wide wave lane query"))
      return std::move(error);
    if (opcode == OpWaveIsFirstLane) {
      if (!call.getType()->isIntegerTy(1)) return createError("DXIL wide first-lane result must be i1");
      return b.CreateICmpEQ(lane, b.getInt32(0));
    }
    if (!call.getType()->isIntegerTy(32)) return createError("DXIL wide lane query result must be i32");
    return opcode == OpWaveGetLaneCount ? b.getInt32(wave_lanes) : lane;
  }
  const unsigned args = opcode == OpWaveReadLaneAt || opcode == OpWaveActiveBit ? 2
      : opcode == OpWaveActiveOp || opcode == OpWavePrefixOp ? 3
      : opcode == OpWaveMultiPrefixOp ? 7 : opcode == OpWaveMultiPrefixBitCount ? 5 : 1;
  if (auto error = expectArgs(call, args, "dx.op wide collective")) return std::move(error);
  Value *value = call.getArgOperand(1);
  Type *type = value->getType();
  if (!(type->isIntegerTy() || type->isFloatTy() || type->isHalfTy()) ||
      (type->isIntegerTy() && type->getIntegerBitWidth() > 64) || call.getMetadata("d3dalloy.fp64"))
    return createError("DXIL wide collective supports scalar integers, half and float only");
  const bool vote = opcode == OpWaveAnyTrue || opcode == OpWaveAllTrue || opcode == OpWaveActiveBallot ||
      opcode == OpWaveAllBitCount || opcode == OpWavePrefixBitCount || opcode == OpWaveMultiPrefixBitCount;
  const bool mask_result = opcode == OpWaveActiveBallot || opcode == OpWaveMatch;
  const bool bool_result = opcode == OpWaveAnyTrue || opcode == OpWaveAllTrue || opcode == OpWaveActiveAllEqual;
  const bool count_result = opcode == OpWaveAllBitCount || opcode == OpWavePrefixBitCount || opcode == OpWaveMultiPrefixBitCount;
  if (vote && !type->isIntegerTy(1)) return createError("DXIL wide vote operand must be i1");
  if (mask_result) {
    if (auto error = expectAggregateResult(call, 4, b.getInt32Ty(), "dx.op wide mask")) return std::move(error);
  } else if (call.getType() != (bool_result ? b.getInt1Ty() : count_result ? b.getInt32Ty() : type)) {
    return createError("DXIL wide collective result type is invalid");
  }
  auto kind_at = [&](unsigned operand) -> Expected<unsigned> {
    auto *kind = dyn_cast<ConstantInt>(call.getArgOperand(operand));
    if (!kind) return createError("DXIL wide reduction kind must be constant");
    return unsigned(kind->getZExtValue());
  };
  WaveReduce kind = WaveReduce::Sum;
  bool is_signed = false;
  const bool reduction = opcode == OpWaveActiveOp || opcode == OpWavePrefixOp || opcode == OpWaveActiveBit || opcode == OpWaveMultiPrefixOp;
  const bool prefix = opcode == OpWavePrefixOp || opcode == OpWaveMultiPrefixOp || opcode == OpWavePrefixBitCount || opcode == OpWaveMultiPrefixBitCount;
  const bool multi = opcode == OpWaveMultiPrefixOp || opcode == OpWaveMultiPrefixBitCount;
  if (reduction) {
    auto operation = kind_at(opcode == OpWaveMultiPrefixOp ? 6 : 2);
    if (!operation) return operation.takeError();
    if (opcode == OpWaveActiveBit) {
      if (*operation > 2 || !type->isIntegerTy()) return createError("DXIL wide bitwise reduction is invalid");
      constexpr WaveReduce kinds[] = {WaveReduce::And, WaveReduce::Or, WaveReduce::Xor};
      kind = kinds[*operation];
    } else {
      auto sign = kind_at(opcode == OpWaveMultiPrefixOp ? 7 : 3);
      if (!sign) return sign.takeError();
      if (*sign > 1) return createError("DXIL wide reduction signedness is invalid");
      is_signed = *sign == 0;
      if (opcode == OpWaveMultiPrefixOp) {
        if (*operation > 4) return createError("DXIL wide multi-prefix kind is invalid");
        constexpr WaveReduce kinds[] = {WaveReduce::Sum, WaveReduce::And, WaveReduce::Or, WaveReduce::Xor, WaveReduce::Product};
        kind = kinds[*operation];
      } else {
        if (*operation > (opcode == OpWavePrefixOp ? 1u : 3u)) return createError("DXIL wide reduction kind is invalid");
        constexpr WaveReduce kinds[] = {WaveReduce::Sum, WaveReduce::Product, WaveReduce::Min, WaveReduce::Max};
        kind = kinds[*operation];
      }
    }
    if ((kind == WaveReduce::And || kind == WaveReduce::Or || kind == WaveReduce::Xor) && !type->isIntegerTy())
      return createError("DXIL wide bitwise reduction requires integers");
  }
  if (multi)
    for (unsigned i = 2; i <= 5; ++i)
      if (!call.getArgOperand(i)->getType()->isIntegerTy(32)) return createError("DXIL wide prefix mask words must be i32");
  if (opcode == OpWaveReadLaneAt && !call.getArgOperand(2)->getType()->isIntegerTy(32))
    return createError("DXIL wide lane read index must be i32");

  auto pointer = [&](Value *index) { return b.CreateInBoundsGEP(wide_scratch->getValueType(), wide_scratch, {b.getInt32(0), index}); };
  const auto nativeLane = [&] { return b.CreateAnd(lane, b.getInt32(kNativeWaveLanes - 1)); };
  const auto partialAt = [&](unsigned group, Type *result_type, Value *identity, unsigned slot = 0) -> Value * {
    auto *offset = b.getInt32(group * kNativeWaveLanes + slot);
    Value *present = b.CreateICmpULT(offset, active);
    Value *index = b.CreateSelect(present, offset, b.getInt32(0));
    auto *load = b.CreateLoad(b.getInt64Ty(), pointer(b.CreateAdd(base, index)));
    load->setVolatile(true);
    return b.CreateSelect(present, b.CreateZExtOrTrunc(load, result_type), identity);
  };
  const unsigned source_lanes = std::min(wide_threads, wave_lanes);
  const bool one_simd = source_lanes <= kNativeWaveLanes;
  if (opcode == OpWaveActiveAllEqual) {
    // The wide-wave admission proof guarantees contiguous active compute lanes.
    auto *bitsType = b.getIntNTy(type->getPrimitiveSizeInBits());
    auto asValue = [&](Value *bits) { return b.CreateBitCast(b.CreateTruncOrBitCast(bits, bitsType), type); };
    auto equals = [&](Value *other) {
      return type->isFloatingPointTy() ? b.CreateFCmpOEQ(value, other) : b.CreateICmpEQ(value, other);
    };
    const bool two_words = bitsType->getBitWidth() > 32;
    auto *integer = two_words ? b.getInt64Ty() : b.getInt32Ty();
    Type *exchange = two_words ? static_cast<Type *>(FixedVectorType::get(b.getInt32Ty(), 2)) : integer;
    auto *bits = b.CreateZExtOrTrunc(b.CreateBitCast(value, bitsType), integer);
    auto *shuffled = emitAIR(b, two_words ? "air.simd_shuffle.u.v2i32" : "air.simd_shuffle.u.i32",
        {b.CreateBitCast(bits, exchange), b.getInt16(0)}, exchange, true);
    auto *first = b.CreateBitCast(shuffled, integer);
    auto *equal = emitAIR(b, "air.simd_all", {equals(asValue(first))}, b.getInt1Ty(), true);
    if (one_simd) return equal;
    // Short values pack the verdict above the representative. A full-width
    // representative occupies lane 0's word; lane 1 stores its SIMD verdict.
    // A partial SIMD with only lane 0 is uniform by definition.
    Value *word = two_words ? b.CreateSelect(b.CreateICmpEQ(nativeLane(), b.getInt32(0)), first,
                                             b.CreateZExt(equal, b.getInt64Ty()))
                            : b.CreateOr(b.CreateZExt(first, b.getInt64Ty()),
                                  b.CreateSelect(equal, b.getInt64(0), b.getInt64(uint64_t(1) << 32)));
    b.CreateStore(word, pointer(wide_group_index))->setVolatile(true);
    air.CreateBarrier(air::MemFlags::Threadgroup, false);
    Value *result = equal;
    for (unsigned group = 0; group * kNativeWaveLanes < source_lanes; ++group) {
      Value *peer = partialAt(group, b.getInt64Ty(), two_words ? first : word);
      Value *uniform = two_words ? partialAt(group, b.getInt1Ty(), b.getTrue(), 1)
                                 : b.CreateICmpULT(peer, b.getInt64(uint64_t(1) << 32));
      result = b.CreateAnd(result, b.CreateAnd(uniform, equals(asValue(peer))));
    }
    air.CreateBarrier(air::MemFlags::Threadgroup, false);
    return result;
  }
  if (!multi && reduction && type->isIntegerTy()) {
    // Exact associative integer arithmetic: one physical-SIMD partial followed
    // by only 2/4 shared-memory partials, not 64/128 lane reads per thread.
    // Floating-point and arbitrary per-lane multi-prefix masks keep the ordered
    // reference path below, including precise/denormal handling.
    Value *identity = waveIdentity(b, kind, type, is_signed);
    Value *partial = nullptr, *localPrefix = nullptr;
    if (type->isIntegerTy(64)) {
      auto exchange = [&](Value *v, Value *index) {
        auto *pair = FixedVectorType::get(b.getInt32Ty(), 2);
        return b.CreateBitCast(emitAIR(b, "air.simd_shuffle.u.v2i32",
            {b.CreateBitCast(v, pair), b.CreateTrunc(index, b.getInt16Ty())}, pair, true), type);
      };
      Value *local = nativeLane(), *inclusive = value;
      for (unsigned stride = 1; stride < kNativeWaveLanes; stride *= 2) {
        Value *hasPrevious = b.CreateICmpUGE(local, b.getInt32(stride));
        Value *index = b.CreateSelect(hasPrevious, b.CreateSub(local,b.getInt32(stride)), b.getInt32(0));
        Value *previous = exchange(inclusive, index);
        inclusive = b.CreateSelect(hasPrevious, waveCombine(b,air,kind,previous,inclusive,is_signed), inclusive);
      }
      Value *remainingLocal = b.CreateSub(active, b.CreateAnd(lane,b.getInt32(~(kNativeWaveLanes - 1))));
      Value *last = b.CreateSub(b.CreateSelect(b.CreateICmpULT(remainingLocal,b.getInt32(kNativeWaveLanes)),
          remainingLocal,b.getInt32(kNativeWaveLanes)), b.getInt32(1));
      partial = exchange(inclusive,last);
      if (prefix) {
        Value *first = b.CreateICmpEQ(local,b.getInt32(0));
        localPrefix = b.CreateSelect(first,identity,exchange(inclusive,
            b.CreateSelect(first,b.getInt32(0),b.CreateSub(local,b.getInt32(1)))));
      }
    } else {
      Type *nativeType = type->isIntegerTy(16) ? type : b.getInt32Ty();
      Value *nativeValue = is_signed ? b.CreateSExtOrTrunc(value,nativeType) : b.CreateZExtOrTrunc(value,nativeType);
      const bool bitwise = kind == WaveReduce::And || kind == WaveReduce::Or || kind == WaveReduce::Xor;
      const auto sign = airSign(nativeType,bitwise || !is_signed);
      partial = b.CreateZExtOrTrunc(emitAIR(b,airSymbol(air,waveNativeName(kind,false),nativeType,sign),
          {nativeValue},nativeType,true),type);
      if (prefix) localPrefix = b.CreateZExtOrTrunc(emitAIR(b,
          airSymbol(air,waveNativeName(kind,true),nativeType,sign),{nativeValue},nativeType,true),type);
    }
    if (one_simd) return prefix ? localPrefix : partial;
    b.CreateStore(b.CreateZExtOrTrunc(partial,b.getInt64Ty()),pointer(wide_group_index))->setVolatile(true);
    air.CreateBarrier(air::MemFlags::Threadgroup,false);
    Value *result = identity;
    for (unsigned group = 0; group * kNativeWaveLanes < source_lanes; ++group) {
      Value *next = waveCombine(b,air,kind,result,partialAt(group,type,identity),is_signed);
      result = prefix ? b.CreateSelect(b.CreateICmpULT(b.getInt32(group * kNativeWaveLanes),
          b.CreateAnd(lane,b.getInt32(~(kNativeWaveLanes-1)))),next,result) : next;
    }
    if (prefix) result = waveCombine(b,air,kind,result,localPrefix,is_signed);
    air.CreateBarrier(air::MemFlags::Threadgroup,false);
    return result;
  }
  if (vote) {
    Value *condition = opcode == OpWaveAllTrue ? b.CreateNot(value) : value;
    Value *ballot = b.CreateTrunc(emitAIR(b,"air.simd_ballot.i64",{condition},b.getInt64Ty(),true),b.getInt32Ty());
    if (!one_simd) {
      b.CreateStore(b.CreateZExt(ballot,b.getInt64Ty()),pointer(wide_group_index))->setVolatile(true);
      air.CreateBarrier(air::MemFlags::Threadgroup,false);
    }
    SmallVector<Value *,4> words(4,b.getInt32(0));
    Value *result = b.getInt32(0);
    for (unsigned group = 0; group * kNativeWaveLanes < source_lanes; ++group) {
      Value *word = one_simd ? ballot : partialAt(group,b.getInt32Ty(),b.getInt32(0));
      if (multi) word = b.CreateAnd(word,call.getArgOperand(2+group));
      if (prefix) {
        Value *groupIndex = b.CreateLShr(lane,b.getInt32(5));
        Value *below = b.CreateSub(b.CreateShl(b.getInt32(1),nativeLane()),b.getInt32(1));
        word = b.CreateAnd(word,b.CreateSelect(b.CreateICmpULT(b.getInt32(group),groupIndex),
            b.getInt32(~0u),b.CreateSelect(b.CreateICmpEQ(b.getInt32(group),groupIndex),below,b.getInt32(0))));
      }
      words[group] = word;
      result = count_result ? b.CreateAdd(result,emitAIR(b,"air.popcount.i32",{word},b.getInt32Ty(),false))
          : b.CreateOr(result,word);
    }
    if (!one_simd) air.CreateBarrier(air::MemFlags::Threadgroup,false);
    if (opcode == OpWaveActiveBallot) return buildAggregate(b,call.getType(),words);
    if (opcode == OpWaveAnyTrue) return b.CreateICmpNE(result,b.getInt32(0));
    if (opcode == OpWaveAllTrue) return b.CreateICmpEQ(result,b.getInt32(0));
    return result;
  }

  // One snapshot per group, not per physical SIMD. Both barriers are unconditional:
  // admission ran on the original DXIL before any stage/resource lowering.
  Type *bits_type = type->isFloatingPointTy() ? b.getIntNTy(type->getPrimitiveSizeInBits()) : type;
  Value *bits = type->isFloatingPointTy() ? b.CreateBitCast(value, bits_type) : value;
  b.CreateStore(b.CreateZExtOrTrunc(bits, b.getInt64Ty()), pointer(wide_group_index))->setVolatile(true);
  air.CreateBarrier(air::MemFlags::Threadgroup, false);
  auto read = [&](Value *index) -> Value * {
    // Inactive source lanes never address outside the original numthreads array.
    Value *safe = b.CreateSelect(b.CreateICmpULT(index, active), index, b.getInt32(0));
    auto *load = b.CreateLoad(b.getInt64Ty(), pointer(b.CreateAdd(base, safe)));
    load->setVolatile(true);
    Value *other = b.CreateTruncOrBitCast(load, bits_type);
    return type->isFloatingPointTy() ? b.CreateBitCast(other, type) : other;
  };
  Value *result = nullptr;
  if (opcode == OpWaveReadLaneAt || opcode == OpWaveReadLaneFirst) {
    result = read(opcode == OpWaveReadLaneAt ? call.getArgOperand(2) : b.getInt32(0));
  } else {
    Value *acc = reduction ? waveIdentity(b, kind, type, is_signed)
        : bool_result ? (opcode == OpWaveAnyTrue ? b.getFalse() : b.getTrue()) : b.getInt32(0);
    SmallVector<Value *, 4> words(4, b.getInt32(0));
    for (unsigned source = 0; source < source_lanes; ++source) {
      Value *other = read(b.getInt32(source));
      Value *take = b.CreateICmpULT(b.getInt32(source), active);
      if (prefix) take = b.CreateAnd(take, b.CreateICmpULT(b.getInt32(source), lane));
      if (multi) take = b.CreateAnd(take, b.CreateICmpNE(b.CreateAnd(call.getArgOperand(2 + source / 32), b.getInt32(1u << (source % 32))), b.getInt32(0)));
      if (reduction) {
        acc = b.CreateSelect(take, waveCombine(b, air, kind, acc, other, is_signed), acc);
      } else {
        Value *condition = other;
        if (opcode == OpWaveMatch) {
          Value *other_bits = type->isFloatingPointTy() ? b.CreateBitCast(other, bits_type) : other;
          condition = b.CreateICmpEQ(other_bits, bits);
        }
        Value *selected = b.CreateAnd(take, condition);
        if (mask_result) words[source / 32] = b.CreateOr(words[source / 32], b.CreateShl(b.CreateZExt(selected, b.getInt32Ty()), source % 32));
        else if (opcode == OpWaveAnyTrue) acc = b.CreateOr(acc, selected);
        else if (opcode == OpWaveAllTrue) acc = b.CreateAnd(acc, b.CreateOr(b.CreateNot(take), condition));
        else if (count_result) acc = b.CreateAdd(acc, b.CreateZExt(selected, b.getInt32Ty()));
        else return createError("DXIL wide collective opcode is unsupported");
      }
    }
    result = mask_result ? buildAggregate(b, call.getType(), words) : acc;
  }
  // Complete every reader before the next collective reuses the same scratch.
  air.CreateBarrier(air::MemFlags::Threadgroup, false);
  return result;
}

Expected<Value *>
OperationLowerer::lowerWave(CallInst &call, uint32_t opcode, IRBuilder<> &builder, air::AIRBuilder &air_builder) {
  if (wave_lanes > kNativeWaveLanes)
    return lowerWideWave(call, opcode, builder, air_builder);
  /* Wave operations are convergent: they read lane state, so they must not be
   * moved or merged across control flow. */
  const bool convergent = true;
  Type *i1 = builder.getInt1Ty();
  Type *i16 = builder.getInt16Ty();
  Type *i32 = builder.getInt32Ty();
  Type *i64 = builder.getInt64Ty();

  /* LowerFP64 preserves the original overload while values become i64 bits. */
  const bool fp64_wave = call.getMetadata("d3dalloy.fp64") || call.getCalledFunction()->getName().contains("f64");
  if (fp64_wave && opcode == OpWaveActiveBit)
    return createError("DXIL FP64 wave bitwise reduction is invalid");

  /* D3D excludes pixel shader helper lanes from every wave operation except the
   * quad family and explicit lane reads; Metal's simdgroup functions include them. */
  const bool exclude_helpers = pixel_stage && !waves_include_helpers;
  Value *helper = nullptr;
  auto is_helper = [&]() -> Value * {
    if (!exclude_helpers)
      return nullptr;
    if (!helper)
      helper = emitAIR(builder, "air.simd_is_helper_thread", {}, i1, convergent);
    return helper;
  };
  auto participates = [&](Value *condition) -> Value * {
    if (Value *h = is_helper())
      return builder.CreateAnd(condition, builder.CreateNot(h));
    return condition;
  };
  auto lane_id = [&]() {
    Value *lane = laneIndex(builder);
    return wave_lanes == kNativeWaveLanes ? lane : builder.CreateAnd(lane, builder.getInt32(wave_lanes - 1));
  };
  auto lane_base = [&]() { return builder.CreateAnd(laneIndex(builder), builder.getInt32(~(wave_lanes - 1))); };
  auto ballot = [&](Value *condition) {
    Value *mask = builder.CreateTrunc(emitAIR(builder, "air.simd_ballot.i64", {condition}, i64, convergent), i32);
    if (wave_lanes != kNativeWaveLanes)
      mask = builder.CreateAnd(builder.CreateLShr(mask, lane_base()),
                               builder.getInt32((1u << wave_lanes) - 1));
    return mask;
  };
  auto participating_mask = [&]() { return ballot(participates(builder.getTrue())); };
  auto first_lane = [&](Value *mask) {
    return emitAIR(builder, "air.ctz.i32", {mask, builder.getFalse()}, i32, false);
  };
  auto lanes_below = [&](Value *lane) { return builder.CreateSub(builder.CreateShl(builder.getInt32(1), lane), builder.getInt32(1)); };
  auto popcount = [&](Value *mask) { return emitAIR(builder, "air.popcount.i32", {mask}, i32, false); };

  auto supported_type = [](Type *type) {
    return type->isFloatTy() || type->isHalfTy() || type->isIntegerTy(1) || type->isIntegerTy(8) || type->isIntegerTy(16) ||
           type->isIntegerTy(32) || type->isIntegerTy(64);
  };
  /* Lane exchange: i64 travels as <2 x i32>, i1/i8 as i32. */
  auto exchange = [&](StringRef base, Value *value, Value *lane16) -> Value * {
    Type *type = value->getType();
    if (type->isIntegerTy(64)) {
      Type *pair = FixedVectorType::get(i32, 2);
      Value *moved = emitAIR(builder, (base + ".u.v2i32").str(), {builder.CreateBitCast(value, pair), lane16}, pair,
                             convergent);
      return builder.CreateBitCast(moved, type);
    }
    if (type->isIntegerTy(1) || type->isIntegerTy(8)) {
      Value *moved = emitAIR(builder, (base + ".u.i32").str(), {builder.CreateZExt(value, i32), lane16}, i32,
                             convergent);
      return builder.CreateTrunc(moved, type);
    }
    return emitAIR(builder, airSymbol(air_builder, base, type, airSign(type, true)), {value, lane16}, type, convergent);
  };
  auto shuffle = [&](Value *value, Value *lane) {
    if (wave_lanes != kNativeWaveLanes)
      lane = builder.CreateAdd(lane_base(), lane);
    return exchange("air.simd_shuffle", value, builder.CreateTrunc(lane, i16));
  };
  auto bits_of = [&](Value *value) -> Value * {
    Type *type = value->getType();
    if (type->isFloatingPointTy())
      return builder.CreateBitCast(value, builder.getIntNTy(type->getPrimitiveSizeInBits()));
    return value;
  };

  /*
   * Reduction over the lanes selected per source lane by `take(lane)`. Used where
   * AIR has no native form (64 bit values, multi-prefix scans): every lane reads
   * every lane with a uniform shuffle, which is exact because a lane that is not
   * taken contributes nothing.
   */
  auto lane_loop = [&](WaveReduce kind, Value *value, bool is_signed, function_ref<Value *(unsigned)> take) {
    Value *accumulator = waveIdentity(builder, kind, value->getType(), is_signed);
    StringRef fp64_helper;
    if (fp64_wave) {
      uint64_t identity = 0;
      switch (kind) {
      case WaveReduce::Sum: fp64_helper = "d3dalloy.fp64.add"; break;
      case WaveReduce::Product: fp64_helper = "d3dalloy.fp64.mul"; identity = 0x3ff0000000000000ull; break;
      case WaveReduce::Min: fp64_helper = "d3dalloy.fp64.min"; identity = 0x7ff0000000000000ull; break;
      case WaveReduce::Max: fp64_helper = "d3dalloy.fp64.max"; identity = 0xfff0000000000000ull; break;
      case WaveReduce::And:
      case WaveReduce::Or:
      case WaveReduce::Xor: llvm_unreachable("FP64 wave bitwise reduction is invalid");
      }
      accumulator = builder.getInt64(identity);
    }
    for (unsigned lane = 0; lane < wave_lanes; ++lane) {
      Value *other = shuffle(value, builder.getInt32(lane));
      Value *combined = fp64_wave
          ? emitAIR(builder, fp64_helper, {accumulator, other}, i64, false)
          : waveCombine(builder, air_builder, kind, accumulator, other, is_signed);
      accumulator = builder.CreateSelect(take(lane), combined, accumulator);
    }
    return accumulator;
  };
  auto mask_has = [&](Value *mask, unsigned lane) {
    return builder.CreateICmpNE(builder.CreateAnd(mask, builder.getInt32(1u << lane)), builder.getInt32(0));
  };

  auto reduce = [&](WaveReduce kind, Value *value, bool is_signed, bool prefix) -> Expected<Value *> {
    Type *type = value->getType();
    StringRef native = waveNativeName(kind, prefix);
    /* The native float sum/product flushes denormals; under "preserve" the lane loop's
     * fadd/fmul become exact helpers (preserveDenormals). */
    const bool exact_float = type->isFloatTy() && (kind == WaveReduce::Sum || kind == WaveReduce::Product) &&
                             preserves(*call.getFunction());
    if (wave_lanes != kNativeWaveLanes || type->isIntegerTy(1) || type->isIntegerTy(8) || type->isIntegerTy(64) || native.empty() || exact_float) {
      Value *mask = participating_mask();
      const bool use_integer_tree = wave_lanes >= 8 && type->isIntegerTy() && !fp64_wave;
      Value *lane = prefix || use_integer_tree ? lane_id() : nullptr;
      if (use_integer_tree) {
        // Each stage combines two aligned half-blocks. Every participating lane
        // already has its own half's aggregate; the first active lane in the
        // opposite half supplies its aggregate even when the ballot has holes.
        // Keep every shuffle inside the logical wave and select self for an
        // empty peer half before discarding that value with the identity.
        Value *identity = waveIdentity(builder, kind, type, is_signed);
        Value *aggregate = value;
        if (Value *h = is_helper())
          aggregate = builder.CreateSelect(h, identity, aggregate);
        Value *exclusive = identity;
        for (unsigned stride = 1; stride < wave_lanes; stride *= 2) {
          Value *peer_base = builder.CreateXor(
              builder.CreateAnd(lane, builder.getInt32(~(stride - 1))), builder.getInt32(stride));
          Value *peer_mask = builder.CreateAnd(mask,
              builder.CreateShl(builder.getInt32((1u << stride) - 1), peer_base));
          Value *has_peer = builder.CreateICmpNE(peer_mask, builder.getInt32(0));
          Value *source = builder.CreateSelect(has_peer, first_lane(peer_mask), lane);
          Value *other = builder.CreateSelect(has_peer, shuffle(aggregate, source), identity);
          if (prefix) {
            Value *upper = builder.CreateICmpNE(
                builder.CreateAnd(lane, builder.getInt32(stride)), builder.getInt32(0));
            exclusive = builder.CreateSelect(upper,
                waveCombine(builder, air_builder, kind, other, exclusive, is_signed), exclusive);
          }
          aggregate = waveCombine(builder, air_builder, kind, aggregate, other, is_signed);
        }
        return prefix ? exclusive : aggregate;
      }
      return lane_loop(kind, value, is_signed, [&](unsigned source) -> Value * {
        Value *taken = mask_has(mask, source);
        if (prefix)
          taken = builder.CreateAnd(taken, builder.CreateICmpULT(builder.getInt32(source), lane));
        return taken;
      });
    }
    if (Value *h = is_helper())
      value = builder.CreateSelect(h, waveIdentity(builder, kind, type, is_signed), value);
    const bool bitwise = kind == WaveReduce::And || kind == WaveReduce::Or || kind == WaveReduce::Xor;
    air::Signedness sign = airSign(type, bitwise ? true : !is_signed);
    return emitAIR(builder, airSymbol(air_builder, native, type, sign), {value}, type, convergent);
  };

  auto constant_kind = [&](unsigned operand, StringRef what) -> Expected<uint32_t> {
    const auto *kind = dyn_cast<ConstantInt>(call.getArgOperand(operand));
    if (!kind)
      return createError(what + " kind must be a constant");
    return static_cast<uint32_t>(kind->getZExtValue());
  };

  switch (opcode) {
  case OpWaveIsFirstLane: {
    if (Error error = expectArgs(call, 0, "dx.op waveIsFirstLane"))
      return std::move(error);
    if (!call.getType()->isIntegerTy(1))
      return createError("dx.op waveIsFirstLane result is not i1");
    if (!exclude_helpers && wave_lanes == kNativeWaveLanes)
      return emitAIR(builder, "air.simd_is_first", {}, call.getType(), convergent);
    Value *first = builder.CreateICmpEQ(lane_id(), first_lane(participating_mask()));
    return exclude_helpers ? builder.CreateAnd(first, builder.CreateNot(is_helper())) : first;
  }
  case OpWaveGetLaneIndex: {
    if (Error error = expectArgs(call, 0, "dx.op waveGetLaneIndex"))
      return std::move(error);
    if (!call.getType()->isIntegerTy(32))
      return createError("dx.op waveGetLaneIndex result is not i32");
    return lane_id();
  }
  case OpWaveGetLaneCount: {
    if (Error error = expectArgs(call, 0, "dx.op waveGetLaneCount"))
      return std::move(error);
    if (!call.getType()->isIntegerTy(32))
      return createError("dx.op waveGetLaneCount result is not i32");
    /* The selected virtual width, not Metal's physical SIMD width. */
    return builder.getInt32(wave_lanes);
  }
  case OpIsHelperLane: {
    if (Error error = expectArgs(call, 0, "dx.op isHelperLane"))
      return std::move(error);
    if (!call.getType()->isIntegerTy(1))
      return createError("dx.op isHelperLane result is not i1");
    /* Only pixel shaders have helper lanes. */
    if (!pixel_stage)
      return builder.getFalse();
    return emitAIR(builder, "air.simd_is_helper_thread", {}, i1, convergent);
  }
  case OpWaveAnyTrue:
  case OpWaveAllTrue: {
    if (Error error = expectArgs(call, 1, "dx.op wave vote"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    if (!condition->getType()->isIntegerTy(1) || !call.getType()->isIntegerTy(1))
      return createError("dx.op wave vote operates on i1");
    if (wave_lanes != kNativeWaveLanes)
      return opcode == OpWaveAnyTrue
        ? builder.CreateICmpNE(ballot(participates(condition)), builder.getInt32(0))
        : builder.CreateICmpEQ(ballot(participates(builder.CreateNot(condition))), builder.getInt32(0));
    if (opcode == OpWaveAnyTrue)
      return emitAIR(builder, "air.simd_any", {participates(condition)}, i1, convergent);
    if (Value *h = is_helper())
      condition = builder.CreateOr(condition, h);
    return emitAIR(builder, "air.simd_all", {condition}, i1, convergent);
  }
  case OpQuadVote: {
    if (Error error = expectArgs(call, 2, "dx.op quadVote"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    auto kind = constant_kind(2, "dx.op quadVote");
    if (!kind)
      return kind.takeError();
    if (!condition->getType()->isIntegerTy(1) || !call.getType()->isIntegerTy(1))
      return createError("dx.op quadVote operates on i1");
    if (*kind > 1)
      return createError("dx.op quadVote has an unknown vote kind");
    /* Quad operations include helper lanes in D3D as well. */
    return emitAIR(builder, *kind == 0 ? "air.quad_any" : "air.quad_all", {condition}, i1, convergent);
  }
  case OpWaveActiveAllEqual: {
    if (Error error = expectArgs(call, 1, "dx.op waveActiveAllEqual"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    if (!call.getType()->isIntegerTy(1))
      return createError("dx.op waveActiveAllEqual result is not i1");
    if (!supported_type(value->getType()->getScalarType()))
      return unsupported("dx.op waveActiveAllEqual on an unsupported type");
    /* All lanes equal == every participating lane matches the first one. */
    Value *first_index = first_lane(participating_mask());
    Value *equal = builder.getTrue();
    auto *vector = dyn_cast<FixedVectorType>(value->getType());
    for (unsigned i = 0; i < (vector ? vector->getNumElements() : 1u); ++i) {
      Value *component = vector ? builder.CreateExtractElement(value, i) : value;
      Value *first = shuffle(component, first_index);
      Value *matches = fp64_wave ? emitAIR(builder, "d3dalloy.fp64.eq", {component, first}, i1, false)
          : component->getType()->isFloatingPointTy() ? builder.CreateFCmpOEQ(component, first)
                                                     : builder.CreateICmpEQ(component, first);
      equal = builder.CreateAnd(equal, matches);
    }
    return builder.CreateICmpEQ(ballot(participates(builder.CreateNot(equal))), builder.getInt32(0));
  }
  case OpWaveActiveBallot: {
    if (Error error = expectArgs(call, 1, "dx.op waveActiveBallot"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    if (!condition->getType()->isIntegerTy(1))
      return createError("dx.op waveActiveBallot condition is not i1");
    if (Error error = expectAggregateResult(call, 4, i32, "dx.op waveActiveBallot"))
      return std::move(error);
    Value *zero = builder.getInt32(0);
    return buildAggregate(builder, call.getType(), {ballot(participates(condition)), zero, zero, zero});
  }
  case OpWaveMatch: {
    if (Error error = expectArgs(call, 1, "dx.op waveMatch"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    if (!supported_type(value->getType()->getScalarType()))
      return unsupported("dx.op waveMatch on an unsupported type");
    if (Error error = expectAggregateResult(call, 4, i32, "dx.op waveMatch"))
      return std::move(error);
    /* Lanes are matched on their bit pattern, which is what DXC's reference
     * implementation compares; -0.0 and +0.0 are therefore distinct. */
    Value *mask = participating_mask();
    Value *matched = builder.getInt32(0);
    for (unsigned lane = 0; lane < wave_lanes; ++lane) {
      Value *equal = mask_has(mask, lane);
      auto *vector = dyn_cast<FixedVectorType>(value->getType());
      for (unsigned i = 0; i < (vector ? vector->getNumElements() : 1u); ++i) {
        Value *component = vector ? builder.CreateExtractElement(value, i) : value;
        equal = builder.CreateAnd(equal, builder.CreateICmpEQ(
            bits_of(shuffle(component, builder.getInt32(lane))), bits_of(component)));
      }
      matched = builder.CreateOr(matched, builder.CreateShl(builder.CreateZExt(equal, i32), lane));
    }
    Value *zero = builder.getInt32(0);
    return buildAggregate(builder, call.getType(), {matched, zero, zero, zero});
  }
  case OpWaveReadLaneAt: {
    if (Error error = expectArgs(call, 2, "dx.op waveReadLaneAt"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    Value *index = call.getArgOperand(2);
    if (call.getType() != value->getType() || !index->getType()->isIntegerTy(32))
      return createError("dx.op waveReadLaneAt operand types are invalid");
    if (!supported_type(value->getType()))
      return unsupported("dx.op waveReadLaneAt on an unsupported type");
    return shuffle(value, index);
  }
  case OpQuadReadLaneAt: {
    if (Error error = expectArgs(call, 2, "dx.op quadReadLaneAt"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    Value *index = call.getArgOperand(2);
    if (call.getType() != value->getType() || !index->getType()->isIntegerTy(32))
      return createError("dx.op quadReadLaneAt operand types are invalid");
    if (!supported_type(value->getType()))
      return unsupported("dx.op quadReadLaneAt on an unsupported type");
    return exchange("air.quad_shuffle", value, builder.CreateTrunc(builder.CreateAnd(index, builder.getInt32(3)), i16));
  }
  case OpWaveReadLaneFirst: {
    if (Error error = expectArgs(call, 1, "dx.op waveReadLaneFirst"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    if (call.getType() != value->getType())
      return createError("dx.op waveReadLaneFirst result type does not match its operand");
    if (!supported_type(value->getType()))
      return unsupported("dx.op waveReadLaneFirst on an unsupported type");
    return shuffle(value, first_lane(participating_mask()));
  }
  case OpWaveActiveOp:
  case OpWavePrefixOp: {
    /* waveActiveOp / wavePrefixOp (value, op, sop): op 0 sum, 1 product, 2 min,
     * 3 max (prefix: sum and product only); sop 0 signed, 1 unsigned. */
    if (Error error = expectArgs(call, 3, "dx.op wave reduction"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    auto kind = constant_kind(2, "dx.op wave reduction");
    if (!kind)
      return kind.takeError();
    auto sign = constant_kind(3, "dx.op wave reduction signedness");
    if (!sign)
      return sign.takeError();
    if (call.getType() != value->getType())
      return createError("dx.op wave reduction result type does not match its operand");
    if (!supported_type(value->getType()))
      return unsupported("dx.op wave reduction on an unsupported type");
    static constexpr WaveReduce kinds[] = {WaveReduce::Sum, WaveReduce::Product, WaveReduce::Min, WaveReduce::Max};
    if (*kind > 3 || (opcode == OpWavePrefixOp && *kind > 1))
      return createError("dx.op wave reduction has an unknown kind");
    return reduce(kinds[*kind], value, *sign == 0, opcode == OpWavePrefixOp);
  }
  case OpWaveActiveBit: {
    /* waveActiveBit (value, op): 0 and, 1 or, 2 xor. */
    if (Error error = expectArgs(call, 2, "dx.op waveActiveBit"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    auto kind = constant_kind(2, "dx.op waveActiveBit");
    if (!kind)
      return kind.takeError();
    if (call.getType() != value->getType())
      return createError("dx.op waveActiveBit result type does not match its operand");
    if (!value->getType()->isIntegerTy(8) && !value->getType()->isIntegerTy(16) && !value->getType()->isIntegerTy(32) &&
        !value->getType()->isIntegerTy(64))
      return unsupported("dx.op waveActiveBit on an unsupported type");
    static constexpr WaveReduce kinds[] = {WaveReduce::And, WaveReduce::Or, WaveReduce::Xor};
    if (*kind > 2)
      return createError("dx.op waveActiveBit has an unknown kind");
    return reduce(kinds[*kind], value, false, false);
  }
  case OpWaveMultiPrefixOp: {
    /* waveMultiPrefixOp (value, mask0..3, op, sop): op 0 sum, 1 and, 2 or, 3 xor,
     * 4 product. Only mask0 can name a lane of a 32 lane wave. */
    if (Error error = expectArgs(call, 7, "dx.op waveMultiPrefixOp"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    Value *mask = call.getArgOperand(2);
    auto kind = constant_kind(6, "dx.op waveMultiPrefixOp");
    if (!kind)
      return kind.takeError();
    auto sign = constant_kind(7, "dx.op waveMultiPrefixOp signedness");
    if (!sign)
      return sign.takeError();
    if (call.getType() != value->getType() || !mask->getType()->isIntegerTy(32))
      return createError("dx.op waveMultiPrefixOp operand types are invalid");
    if (!supported_type(value->getType()))
      return unsupported("dx.op waveMultiPrefixOp on an unsupported type");
    static constexpr WaveReduce kinds[] = {WaveReduce::Sum, WaveReduce::And, WaveReduce::Or, WaveReduce::Xor,
                                           WaveReduce::Product};
    if (*kind > 4)
      return createError("dx.op waveMultiPrefixOp has an unknown kind");
    if ((value->getType()->isFloatingPointTy() || fp64_wave) && *kind >= 1 && *kind <= 3)
      return createError("dx.op waveMultiPrefixOp bitwise kind on a floating point value");
    Value *members = builder.CreateAnd(mask, participating_mask());
    Value *lane = lane_id();
    return lane_loop(kinds[*kind], value, *sign == 0, [&](unsigned source) -> Value * {
      return builder.CreateAnd(mask_has(members, source), builder.CreateICmpULT(builder.getInt32(source), lane));
    });
  }
  case OpWaveMultiPrefixBitCount: {
    if (Error error = expectArgs(call, 5, "dx.op waveMultiPrefixBitCount"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    Value *mask = call.getArgOperand(2);
    if (!condition->getType()->isIntegerTy(1) || !mask->getType()->isIntegerTy(32) || !call.getType()->isIntegerTy(32))
      return createError("dx.op waveMultiPrefixBitCount operand types are invalid");
    Value *votes = builder.CreateAnd(ballot(participates(condition)), mask);
    return popcount(builder.CreateAnd(votes, lanes_below(lane_id())));
  }
  case OpQuadOp: {
    /* quadOp (value, op): 0 across x, 1 across y, 2 diagonal, a quad lane XOR of
     * 1, 2 and 3 in the Z ordered quad. */
    if (Error error = expectArgs(call, 2, "dx.op quadOp"))
      return std::move(error);
    Value *value = call.getArgOperand(1);
    auto kind = constant_kind(2, "dx.op quadOp");
    if (!kind)
      return kind.takeError();
    if (call.getType() != value->getType())
      return createError("dx.op quadOp result type does not match its operand");
    if (!supported_type(value->getType()))
      return unsupported("dx.op quadOp on an unsupported type");
    if (*kind > 2)
      return createError("dx.op quadOp has an unknown swap kind");
    return exchange("air.quad_shuffle_xor", value, builder.getInt16(*kind + 1));
  }
  case OpWaveAllBitCount:
  case OpWavePrefixBitCount: {
    if (Error error = expectArgs(call, 1, "dx.op wave bit count"))
      return std::move(error);
    Value *condition = call.getArgOperand(1);
    if (!condition->getType()->isIntegerTy(1) || !call.getType()->isIntegerTy(32))
      return createError("dx.op wave bit count operates on i1 and returns i32");
    Value *votes = ballot(participates(condition));
    if (opcode == OpWavePrefixBitCount)
      votes = builder.CreateAnd(votes, lanes_below(lane_id()));
    return popcount(votes);
  }
  default:
    return unsupported(("unhandled dx.op wave opcode " + Twine(opcode)).str());
  }
}

/* -------------------------------------------------------------------------- */
/* SM 6.4 / 6.6 packed math                                                   */
/* -------------------------------------------------------------------------- */

Expected<Value *>
OperationLowerer::lowerPacked(CallInst &call, uint32_t opcode, IRBuilder<> &builder) {
  Type *i32 = builder.getInt32Ty();
  auto byte_of = [&](Value *packed, unsigned index, bool is_signed, Type *type) {
    Value *byte = builder.CreateTrunc(builder.CreateLShr(packed, index * 8), builder.getInt8Ty());
    return is_signed ? builder.CreateSExt(byte, type) : builder.CreateZExt(byte, type);
  };
  switch (opcode) {
  case OpDot2AddHalf: {
    /* dot2AddHalf (acc, ax, ay, bx, by): acc + ax*bx + ay*by, with the half
     * products formed exactly in float (a half product is exact in float). */
    if (Error error = expectArgs(call, 5, "dx.op dot2AddHalf"))
      return std::move(error);
    Type *f32 = builder.getFloatTy();
    if (!call.getType()->isFloatTy() || !call.getArgOperand(1)->getType()->isFloatTy())
      return createError("dx.op dot2AddHalf accumulator must be f32");
    for (unsigned i = 2; i <= 5; ++i)
      if (!call.getArgOperand(i)->getType()->isHalfTy())
        return createError("dx.op dot2AddHalf operands must be f16");
    Value *x = builder.CreateFMul(builder.CreateFPExt(call.getArgOperand(2), f32),
                                  builder.CreateFPExt(call.getArgOperand(4), f32));
    Value *y = builder.CreateFMul(builder.CreateFPExt(call.getArgOperand(3), f32),
                                  builder.CreateFPExt(call.getArgOperand(5), f32));
    return builder.CreateFAdd(builder.CreateFAdd(call.getArgOperand(1), x), y);
  }
  case OpDot4AddI8Packed:
  case OpDot4AddU8Packed: {
    /* dot4AddPacked (acc, a, b): acc + sum of the four byte products, wrapping. */
    if (Error error = expectArgs(call, 3, "dx.op dot4AddPacked"))
      return std::move(error);
    for (unsigned i = 1; i <= 3; ++i)
      if (!call.getArgOperand(i)->getType()->isIntegerTy(32))
        return createError("dx.op dot4AddPacked operands must be i32");
    const bool is_signed = opcode == OpDot4AddI8Packed;
    Value *sum = call.getArgOperand(1);
    for (unsigned i = 0; i < 4; ++i)
      sum = builder.CreateAdd(sum, builder.CreateMul(byte_of(call.getArgOperand(2), i, is_signed, i32),
                                                     byte_of(call.getArgOperand(3), i, is_signed, i32)));
    return sum;
  }
  case OpUnpack4x8: {
    /* unpack4x8 (mode, packed): mode 0 zero extends, 1 sign extends each byte. */
    if (Error error = expectArgs(call, 2, "dx.op unpack4x8"))
      return std::move(error);
    const auto *mode = dyn_cast<ConstantInt>(call.getArgOperand(1));
    if (!mode || mode->getZExtValue() > 1)
      return createError("dx.op unpack4x8 mode must be a constant 0 or 1");
    auto *result = dyn_cast<StructType>(call.getType());
    if (!result || result->getNumElements() != 4 || !result->getElementType(0)->isIntegerTy() ||
        !call.getArgOperand(2)->getType()->isIntegerTy(32))
      return createError("dx.op unpack4x8 has unexpected types");
    SmallVector<Value *, 4> lanes;
    for (unsigned i = 0; i < 4; ++i)
      lanes.push_back(byte_of(call.getArgOperand(2), i, mode->isOne(), result->getElementType(0)));
    return buildAggregate(builder, call.getType(), lanes);
  }
  case OpPack4x8: {
    /* pack4x8 (mode, x, y, z, w): 0 truncates, 1 clamps to [0, 255], 2 clamps to
     * [-128, 127]; the inputs are signed. */
    if (Error error = expectArgs(call, 5, "dx.op pack4x8"))
      return std::move(error);
    const auto *mode = dyn_cast<ConstantInt>(call.getArgOperand(1));
    if (!mode || mode->getZExtValue() > 2)
      return createError("dx.op pack4x8 mode must be a constant 0, 1 or 2");
    Type *type = call.getArgOperand(2)->getType();
    if (!call.getType()->isIntegerTy(32) || !type->isIntegerTy() || type->getIntegerBitWidth() < 8)
      return createError("dx.op pack4x8 has unexpected types");
    Value *packed = builder.getInt32(0);
    for (unsigned i = 0; i < 4; ++i) {
      Value *lane = call.getArgOperand(2 + i);
      if (lane->getType() != type)
        return createError("dx.op pack4x8 operand types do not agree");
      if (!mode->isZero()) {
        const int64_t low = mode->getZExtValue() == 1 ? 0 : -128;
        const int64_t high = mode->getZExtValue() == 1 ? 255 : 127;
        lane = builder.CreateSelect(builder.CreateICmpSLT(lane, ConstantInt::getSigned(type, low)),
                                    ConstantInt::getSigned(type, low), lane);
        lane = builder.CreateSelect(builder.CreateICmpSGT(lane, ConstantInt::getSigned(type, high)),
                                    ConstantInt::getSigned(type, high), lane);
      }
      Value *byte = builder.CreateZExt(builder.CreateTrunc(lane, builder.getInt8Ty()), i32);
      packed = builder.CreateOr(packed, builder.CreateShl(byte, i * 8));
    }
    return packed;
  }
  default:
    return unsupported(("unhandled dx.op packed opcode " + Twine(opcode)).str());
  }
}

} // namespace

llvm::Error
LowerOperations(llvm::Module &module) {
  /* Inline RayQuery objects (any stage); their structures were resolved by the resource pass. */
  if (Error error = LowerRayQueries(module))
    return error;
  if (Error error = LowerVectors(module, false))
    return error;
  if (Error error = LowerFP64(module))
    return error;
  OperationLowerer lowerer(module);
  if (Error error = lowerer.run())
    return error;
  // Wave reductions introduce FP64 helper calls after the binary64 prepass.
  for (Function &function : module)
    if (function.isDeclaration() && !function.use_empty() && function.getName().startswith("d3dalloy.fp64.")) {
      d3dalloy::linkFP64(module);
      break;
    }
  return LowerVectors(module, true);
}

} // namespace d3dalloy::dxil
