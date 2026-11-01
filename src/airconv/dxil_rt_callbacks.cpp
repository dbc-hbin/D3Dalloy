/*
 * Per-candidate ray tracing callbacks: dx.op.ignoreHit / dx.op.acceptHitAndEndSearch
 * and dx.op.reportHit -> AIR (dxil_rt_callbacks.hpp).
 *
 * Contract provenance. Every `air.` name and every operand shape below was read out
 * of the public Metal toolchain's own output:
 *
 *   probes/rt-compiler/rt-public-contract.ll   i1 (@isect(<2 x float>, i32 ..., AS5*))
 *                                              with !air.accept_intersection
 *   /tmp/rtfix/c1.ll                           <{ i1, i1 }> (@isect(...)) with both
 *                                              !air.accept_intersection and
 *                                              !air.continue_search
 *   /tmp/cbprobe/i1.ll                         a [[visible]] function called through
 *                                              the visible function table from an
 *                                              [[intersection]] function, its payload
 *                                              parameter in the ray-data space
 *   /tmp/cbprobe/g1.ll                         the same call from an intersection
 *                                              function that also takes the table
 *
 * Nothing here is a vendor ABI.
 *
 * The two DXIL operations are the shader-side halves of the per-candidate contract:
 *
 *   - An any-hit shader decides one candidate. `dx.op.ignoreHit` rejects it,
 *     `dx.op.acceptHitAndEndSearch` accepts it and ends the search; both are
 *     `noreturn` in DXIL, so the decision is recorded and the any-hit body ends
 *     there. What invoked the any-hit - the shared per-candidate callback of a
 *     triangle hit group, or `dx.op.reportHit` of a procedural one - reads the
 *     decision and turns it into the intersection function's accept /
 *     continue_search result.
 *
 *   - An intersection shader reports candidates with `dx.op.reportHit`. D3D12 gives
 *     it a value result: true when the hit was reported (the any-hit shader accepted
 *     it), false when the reported distance is outside the active ray interval or
 *     the any-hit shader rejected it. It is an ordinary call - it may run any number
 *     of times in one invocation and the shader keeps going afterwards - so the
 *     lowering replaces the call with its value and never terminates the body.
 */

#include "dxil_rt_callbacks.hpp"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

using namespace llvm;

namespace d3dalloy::dxil {

/* -------------------------------------------------------------------------- */
/* Invocation block layout                                                    */
/* -------------------------------------------------------------------------- */

RtInvocationLayout rtInvocationLayout(const RaytracingExportContext &context) {
  RtInvocationLayout layout{};
  layout.frame_size = context.frame_size;
  layout.payload_size = context.payload_size;
  layout.attribute_size = context.attribute_size;
  layout.hit_state = context.hitStateOffset();
  layout.attribute = context.attributeOffset();
  layout.candidate_control = context.candidateOffset();
  /*
   * The candidate's own attributes follow the control block, which is the order the
   * traversal lowering addresses the control fields in (candidateOffset() + 0x00 is
   * Decision, +0x10 HitGroupBase, ...): the block has to start exactly where the
   * region does, so the attributes cannot sit in front of it.
   */
  layout.candidate_attribute = layout.candidate_control + kRtCandidateControlSize;
  layout.block_size = layout.candidate_attribute + layout.attribute_size;

  return layout;
}

namespace {

/**
 * Reject a layout the callback layer cannot address.
 *
 * The candidate region has to hold the control block *and* one attribute region,
 * and the hit state has to be at least the ABI struct: a callback addresses all
 * three by fixed offsets, so a smaller region would read or write memory that
 * belongs to something else. Refusing keeps that a compile error instead of a
 * silently corrupted payload.
 */
llvm::Error checkInvocationLayout(const RaytracingExportContext &context) {
  const uint32_t required = kRtCandidateRegionSize(context.attribute_size);
  if (context.candidate_size < required)
    return createError(
      "dxil: the per-candidate region of this export is " + Twine(context.candidate_size) +
      " bytes, but the candidate control block and the candidate attribute region "
      "need " + Twine(required) +
      " (the stage builder must publish kRtCandidateRegionSize(attribute_size))"
    );
  if (context.hit_state_size && context.hit_state_size < sizeof(SM50_RT_HIT_STATE))
    return createError(
      "dxil: the hit state region of this export is " + Twine(context.hit_state_size) +
      " bytes, but SM50_RT_HIT_STATE needs " + Twine(sizeof(SM50_RT_HIT_STATE))
    );
  return llvm::Error::success();
}

/** The AIR visible-function-table type. */
Type *visibleFunctionTableType(LLVMContext &context) {
  StructType *type = StructType::getTypeByName(context, "struct._visible_function_table_t");
  if (!type)
    type = StructType::create(context, "struct._visible_function_table_t");
  return type->getPointerTo(1);
}

/** The i8* form of `value` in `value`'s own address space. */
Value *bytePointer(IRBuilder<> &builder, Value *value) {
  LLVMContext &context = value->getContext();
  Type *type = Type::getInt8PtrTy(
    context, cast<PointerType>(value->getType())->getAddressSpace()
  );
  return value->getType() == type ? value : builder.CreateBitCast(value, type);
}

/**
 * `base + byte_offset` as a `type*`, in `base`'s own address space.
 *
 * The address space is preserved rather than assumed to be the thread space: a
 * callback runs inside the traversal and is handed the invocation block as a
 * ray-data (address space 5) pointer, so every region of the block is addressed
 * through that pointer.
 */
Value *fieldPointer(IRBuilder<> &builder, Value *base, uint32_t byte_offset, Type *type) {
  LLVMContext &context = base->getContext();
  unsigned address_space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *pointer = builder.CreateGEP(
    Type::getInt8Ty(context), bytePointer(builder, base), builder.getInt32(byte_offset)
  );
  return builder.CreateBitCast(pointer, type->getPointerTo(address_space));
}

Value *loadField(IRBuilder<> &builder, Value *base, uint32_t byte_offset, Type *type) {
  return builder.CreateAlignedLoad(type, fieldPointer(builder, base, byte_offset, type), Align(1));
}

void storeField(IRBuilder<> &builder, Value *base, uint32_t byte_offset, Value *value) {
  builder.CreateAlignedStore(value, fieldPointer(builder, base, byte_offset, value->getType()), Align(1));
}

/** Convert an integer value to i32, so a stored field has one shape. */
Value *asWord(IRBuilder<> &builder, Value *value) {
  if (value->getType()->isIntegerTy(32))
    return value;
  if (value->getType()->isIntegerTy())
    return builder.CreateZExtOrTrunc(value, builder.getInt32Ty());
  return builder.CreateBitCast(value, builder.getInt32Ty());
}

/**
 * Run `body` in a block entered only when `condition` holds, continuing in a join
 * block afterwards.
 *
 * The block is split at the insertion point first, so the instructions that follow
 * it stay reachable and the branch never lands in the middle of a block: this is
 * what makes a guard usable while a call is still sitting at the insertion point
 * (the call moves into the join block, where the caller erases it).
 *
 * A condition that folds to false emits nothing at all, so guarded code that the
 * compiler can already prove unreachable is never emitted.
 */
void emitConditional(
  IRBuilder<> &builder, Value *condition, StringRef name,
  function_ref<void(IRBuilder<> &)> body
) {
  if (auto *constant = dyn_cast<ConstantInt>(condition)) {
    if (constant->isOne())
      body(builder);
    return;
  }
  LLVMContext &context = builder.getContext();
  Function *function = builder.GetInsertBlock()->getParent();
  BasicBlock *head = builder.GetInsertBlock();
  BasicBlock *continuation = nullptr;
  if (builder.GetInsertPoint() != head->end())
    continuation = head->splitBasicBlock(builder.GetInsertPoint(), name + ".cont");
  if (head->getTerminator())
    head->getTerminator()->eraseFromParent();
  if (!continuation)
    continuation = BasicBlock::Create(context, name + ".cont", function);

  BasicBlock *guarded = BasicBlock::Create(context, name, function, continuation);
  builder.SetInsertPoint(head);
  builder.CreateCondBr(condition, guarded, continuation);
  builder.SetInsertPoint(guarded);
  body(builder);
  builder.CreateBr(continuation);
  builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
}

/* -------------------------------------------------------------------------- */
/* Intersection function arguments                                            */
/* -------------------------------------------------------------------------- */

/**
 * One declared input of the lowered `air.intersection` function, by its AIR kind.
 *
 * An intersection shader's primitive / geometry / instance ids are arguments of the
 * `[[intersection]]` function, not fields of the traversal result (the traversal has
 * not finished when it runs), so the callback layer reads them from the signature
 * the stage builder declared. The AIR metadata is the only place they are recorded,
 * and the argument is matched by kind rather than by position.
 */
Value *intersectionInput(Module &module, Function *function, StringRef kind) {
  if (!function)
    return nullptr;
  NamedMDNode *named = module.getNamedMetadata("air.intersection");
  if (!named)
    return nullptr;
  MDNode *inputs = nullptr;
  for (unsigned i = 0; i < named->getNumOperands() && !inputs; ++i) {
    MDNode *node = named->getOperand(i);
    if (!node || node->getNumOperands() < 3)
      continue;
    auto *value = dyn_cast<ValueAsMetadata>(node->getOperand(0));
    if (!value || value->getValue() != function)
      continue;
    inputs = dyn_cast<MDNode>(node->getOperand(2));
  }
  if (!inputs)
    return nullptr;
  for (const MDOperand &operand : inputs->operands()) {
    MDNode *argument = dyn_cast_or_null<MDNode>(operand.get());
    if (!argument || argument->getNumOperands() < 2)
      continue;
    auto *declared = dyn_cast<MDString>(argument->getOperand(1));
    if (!declared || declared->getString() != kind)
      continue;
    auto *index = dyn_cast<ConstantAsMetadata>(argument->getOperand(0));
    if (!index)
      continue;
    auto *position = dyn_cast<ConstantInt>(index->getValue());
    if (!position)
      continue;
    unsigned slot = static_cast<unsigned>(position->getZExtValue());
    if (slot < function->arg_size())
      return function->getArg(slot);
  }
  return nullptr;
}

/** The candidate's own ids, as the intersection function declares them. */
using CandidateIds = RtCandidateIdentity;

/**
 * Resolve the candidate ids of an intersection export.
 *
 * D3D12 defines PrimitiveIndex / GeometryIndex / InstanceIndex / InstanceID inside
 * an intersection shader and inside the any-hit shader it reports to, so a hit group
 * whose any-hit shader reads them cannot be lowered without them. The stage builder
 * declares them (the public front end emits exactly these AIR kinds for the MSL
 * attributes, see /tmp/rtfix/c1.ll), and a missing one is reported instead of being
 * answered with whatever the block happens to contain.
 */
llvm::Error resolveCandidateIds(
  Module &module, const RaytracingEntryArguments &entry, CandidateIds &out
) {
  out.primitive_id = intersectionInput(module, entry.function, "air.primitive_id");
  out.geometry_id = intersectionInput(module, entry.function, "air.geometry_id");
  /*
   * D3D12's InstanceIndex is the autogenerated index of the instance in the top-level
   * structure (the traversal's instance id), while InstanceID is the application's
   * user defined per-instance id (the traversal's user instance id). The AIR kinds are
   * named after the MSL attributes, so the two are crossed over here and nowhere else.
   */
  out.instance_index = intersectionInput(module, entry.function, "air.instance_id");
  out.instance_id = intersectionInput(module, entry.function, "air.user_instance_id");

  StringRef missing;
  if (!out.primitive_id)
    missing = "air.primitive_id ([[primitive_id]])";
  else if (!out.geometry_id)
    missing = "air.geometry_id ([[geometry_id]])";
  else if (!out.instance_index)
    missing = "air.instance_id ([[instance_id]])";
  else if (!out.instance_id)
    missing = "air.user_instance_id ([[user_instance_id]])";
  if (missing.empty())
    return llvm::Error::success();
  return createError(
    "dxil: dx.op.reportHit needs the intersection shader's candidate id input " +
    Twine(missing) +
    ", which the stage builder must declare: an any-hit shader runs for the candidate "
    "the intersection shader reports, and PrimitiveIndex / GeometryIndex / "
    "InstanceIndex / InstanceID are the candidate's values"
  );
}

/** Publish the candidate's system values into the hit state the any-hit reads. */
void publishCandidateHitState(
  const RaytracingExportContext &context, const CandidateIds &ids, Value *block,
  Value *distance, Value *hit_kind, IRBuilder<> &builder
) {
  const RtInvocationLayout layout = rtInvocationLayout(context);
  const uint32_t base = layout.hit_state;
  storeField(
    builder, block, base + offsetof(SM50_RT_HIT_STATE, PrimitiveIndex),
    asWord(builder, ids.primitive_id)
  );
  storeField(
    builder, block, base + offsetof(SM50_RT_HIT_STATE, GeometryIndex),
    asWord(builder, ids.geometry_id)
  );
  storeField(
    builder, block, base + offsetof(SM50_RT_HIT_STATE, InstanceID),
    asWord(builder, ids.instance_id)
  );
  storeField(
    builder, block, base + offsetof(SM50_RT_HIT_STATE, InstanceIndex),
    asWord(builder, ids.instance_index)
  );
  /* The reported hit's own distance and kind: RayTCurrent and HitKind. */
  storeField(builder, block, base + offsetof(SM50_RT_HIT_STATE, RayTCurrent), distance);
  storeField(builder, block, base + offsetof(SM50_RT_HIT_STATE, HitKind), hit_kind);
}

/** The recursion depth of the invocation `block` belongs to. */
Value *invocationDepth(IRBuilder<> &builder, Value *block) {
  return loadField(
    builder, block, offsetof(SM50_RT_FRAME, Depth), builder.getInt32Ty()
  );
}

} // namespace

/* -------------------------------------------------------------------------- */
/* Shared callback primitives                                                 */
/* -------------------------------------------------------------------------- */

llvm::Value *EmitVisibleFunctionCall(
  Module &module, Value *visible_function_table, Value *record_index, Value *depth,
  Value *payload, Value *hit_state, IRBuilder<> &builder
) {
  if (!visible_function_table || !payload || !hit_state)
    return nullptr;
  LLVMContext &context = module.getContext();
  AttributeList attributes = AttributeList::get(
    context,
    {{~0U, Attribute::get(context, Attribute::Convergent)},
     {~0U, Attribute::get(context, Attribute::NoUnwind)},
     {~0U, Attribute::get(context, Attribute::WillReturn)}}
  );
  /* Slot 0 is always the null entry, so export index N lives in slot N + 1. */
  FunctionCallee getter = module.getOrInsertFunction(
    "air.get_function_pointer_visible_function_table",
    FunctionType::get(
      Type::getInt8PtrTy(context),
      {visibleFunctionTableType(context), builder.getInt32Ty()}, false
    ),
    attributes
  );
  Value *slot = builder.CreateAdd(record_index, builder.getInt32(1));
  Value *pointer = builder.CreateCall(getter, {visible_function_table, slot});

  /*
   * The callee's payload and hit-state parameters keep the address spaces of the
   * pointers passed in, which is exactly what the public front end emits for a call
   * through the table: an intersection function's payload is a ray-data pointer and
   * the visible function it reaches declares its payload the same way
   * (/tmp/cbprobe/i1.ll). Nothing is cast between address spaces, and the pointer
   * the hardware handed the traversal is the pointer the callee reads.
   */
  auto *function_type = FunctionType::get(
    Type::getVoidTy(context),
    {payload->getType(), hit_state->getType(), builder.getInt32Ty(), builder.getInt32Ty()},
    false
  );
  Value *callee = builder.CreateBitCast(pointer, function_type->getPointerTo());
  return builder.CreateCall(
    function_type, callee, {payload, hit_state, record_index, depth}
  );
}

llvm::Value *EmitAnyHitCandidate(
  Module &module, const RaytracingEntryArguments &entry,
  const RaytracingExportContext &context, Value *block, IRBuilder<> &builder
) {
  if (llvm::Error error = checkInvocationLayout(context))
    return nullptr;
  if (!block)
    return nullptr;
  const RtInvocationLayout layout = rtInvocationLayout(context);
  Value *control = builder.CreateGEP(
    Type::getInt8Ty(module.getContext()), bytePointer(builder, block),
    builder.getInt32(layout.candidate_control)
  );
  /*
   * The decision belongs to this candidate alone: clearing it first is what makes
   * the value read back below the answer of the any-hit shader rather than of the
   * previous candidate of the same invocation.
   */
  storeField(
    builder, control, kRtCandidateDecision, builder.getInt32(kRtCandidateNone)
  );
  if (context.any_hit_index == SM50_RT_INDEX_NONE)
    return builder.getInt1(true);

  Value *visible_function_table = entry.visible_function_table;
  if (!visible_function_table && context.query_callbacks) {
    Value *address = loadField(builder, block, offsetof(SM50_RT_FRAME, VisibleFunctionTable), builder.getInt64Ty());
    visible_function_table = builder.CreateIntToPtr(address, visibleFunctionTableType(builder.getContext()));
  }
  if (!visible_function_table)
    return nullptr;
  /*
   * The any-hit shader is invoked through the visible function table, exactly like a
   * closest-hit shader: D3D12 gives a hit group's members the same payload, and the
   * record index and depth it runs as are its own. The candidate's attributes are
   * already in the candidate attribute region, so the hit state the any-hit reads is
   * the candidate's.
   */
  /*
   * The callee's payload and hit-state parameters are declared in the ray-data space
   * (see kRtRayDataAddressSpace), which is the space an intersection function's own
   * payload argument already lives in, so the region pointers are cast into it.
   */
  LLVMContext &llvm_context = module.getContext();
  auto rayDataPointer = [&](uint32_t byte_offset) -> Value * {
    Value *field = builder.CreateGEP(
      Type::getInt8Ty(llvm_context), bytePointer(builder, block), builder.getInt32(byte_offset)
    );
    if (context.query_callbacks)
      return field;
    return builder.CreateAddrSpaceCast(field, Type::getInt8PtrTy(llvm_context, kRtRayDataAddressSpace));
  };
  Value *payload = rayDataPointer(layout.frame_size);
  Value *hit_state = rayDataPointer(layout.hit_state);
  Value *skip = context.query_callbacks
    ? builder.CreateICmpNE(loadField(builder, control, kRtCandidateSkipAnyHit, builder.getInt32Ty()), builder.getInt32(0))
    : builder.getFalse();
  bool failed = false;
  emitConditional(builder, builder.CreateNot(skip), "rt.anyhit", [&](IRBuilder<> &b) {
    failed = !EmitVisibleFunctionCall(module, visible_function_table, b.getInt32(context.any_hit_index),
                                     invocationDepth(b, block), payload, hit_state, b);
  });
  if (failed)
    return nullptr;

  Value *decision = loadField(
    builder, control, kRtCandidateDecision, builder.getInt32Ty()
  );
  return builder.CreateOr(skip, builder.CreateICmpNE(decision, builder.getInt32(kRtCandidateReject)));
}

void EmitCandidateCommit(
  Module &module, const RaytracingExportContext &context, Value *block, Value *distance,
  Value *hit_kind, uint32_t attribute_size, const RtCandidateIdentity &identity,
  Value *accepted, IRBuilder<> &builder
) {
  if (!block || !accepted)
    return;
  const RtInvocationLayout layout = rtInvocationLayout(context);
  Value *control = builder.CreateGEP(
    Type::getInt8Ty(module.getContext()), bytePointer(builder, block),
    builder.getInt32(layout.candidate_control)
  );
  Value *committed = loadField(
    builder, control, kRtCandidateCommitted, builder.getInt32Ty()
  );
  Value *previous = loadField(
    builder, control, kRtCandidateDistance, builder.getFloatTy()
  );
  /*
   * A hit is committed when the any-hit shader accepted it and it is closer than
   * everything committed before it. Without the second half the last accepted
   * candidate of a traversal would win instead of the closest one, and the committed
   * attributes would no longer describe the committed distance.
   */
  Value *closer = builder.CreateFCmpOLT(distance, previous);
  Value *first = builder.CreateICmpEQ(committed, builder.getInt32(0));
  Value *wins = builder.CreateAnd(
    accepted, builder.CreateOr(first, closer)
  );

  /*
   * The committed tuple is written *only* here, inside the win guard: Distance,
   * HitKind and AttributeSize are committed state, and a candidate that loses must
   * leave them - and the committed attribute region - exactly as the winner left
   * them. The report body deliberately writes none of them.
   */
  emitConditional(builder, wins, "rt.commit", [&](IRBuilder<> &b) {
    if (attribute_size) {
      Value *destination = b.CreateGEP(
        Type::getInt8Ty(module.getContext()), bytePointer(b, block),
        b.getInt32(layout.attribute)
      );
      Value *source = b.CreateGEP(
        Type::getInt8Ty(module.getContext()), bytePointer(b, block),
        b.getInt32(layout.candidate_attribute)
      );
      b.CreateMemCpy(
        destination, MaybeAlign(4), source, MaybeAlign(4),
        b.getInt64(attribute_size)
      );
    }
    storeField(b, control, kRtCandidateDistance, distance);
    storeField(b, control, kRtCandidateHitKind, hit_kind);
    storeField(
      b, control, kRtCandidateAttributeSize, b.getInt32(attribute_size)
    );
    /*
     * The winning candidate's own system values, so the committed hit can be
     * republished after the candidate is decided: the hit state holds the candidate
     * under consideration while its any-hit shader runs, and the closest-hit shader
     * that runs after the traversal must read the committed hit instead.
     */
    storeField(
      b, control, kRtCandidateCommittedPrimitiveIndex, asWord(b, identity.primitive_id)
    );
    storeField(
      b, control, kRtCandidateCommittedGeometryIndex, asWord(b, identity.geometry_id)
    );
    storeField(
      b, control, kRtCandidateCommittedInstanceIndex, asWord(b, identity.instance_index)
    );
    storeField(
      b, control, kRtCandidateCommittedInstanceID, asWord(b, identity.instance_id)
    );
    storeField(b, control, kRtCandidateCommitted, b.getInt32(1));
  });
}

void EmitCommittedHitStateRestore(
  Module &module, const RaytracingExportContext &context, Value *block, IRBuilder<> &builder
) {
  if (!block)
    return;
  LLVMContext &llvm_context = module.getContext();
  const RtInvocationLayout layout = rtInvocationLayout(context);
  Value *control = CandidateControlPointer(module, context, block, builder);
  Value *committed = loadField(builder, control, kRtCandidateCommitted, builder.getInt32Ty());
  Value *have_committed = builder.CreateICmpNE(committed, builder.getInt32(0));

  /*
   * Nothing is emitted until a hit has actually been committed: the first report of
   * an invocation has to see the ray's own interval, not a tightened one.
   */
  emitConditional(builder, have_committed, "rt.committed", [&](IRBuilder<> &b) {
    const uint32_t base = layout.hit_state;
    auto restore = [&](uint32_t byte_offset, uint32_t field_offset) {
      Value *value = loadField(
        b, control, byte_offset, Type::getInt32Ty(llvm_context)
      );
      storeField(b, block, base + field_offset, value);
    };
    restore(kRtCandidateCommittedPrimitiveIndex, offsetof(SM50_RT_HIT_STATE, PrimitiveIndex));
    restore(kRtCandidateCommittedGeometryIndex, offsetof(SM50_RT_HIT_STATE, GeometryIndex));
    restore(kRtCandidateCommittedInstanceIndex, offsetof(SM50_RT_HIT_STATE, InstanceIndex));
    restore(kRtCandidateCommittedInstanceID, offsetof(SM50_RT_HIT_STATE, InstanceID));

    Value *distance = loadField(
      b, control, kRtCandidateDistance, Type::getFloatTy(llvm_context)
    );
    Value *kind = loadField(b, control, kRtCandidateHitKind, Type::getInt32Ty(llvm_context));
    storeField(b, block, base + offsetof(SM50_RT_HIT_STATE, RayTCurrent), distance);
    storeField(b, block, base + offsetof(SM50_RT_HIT_STATE, HitKind), kind);

    /*
     * A committed hit ends the search for anything farther away, so the ray's upper
     * bound shrinks to it. This is what makes a later `dx.op.reportHit` beyond the
     * committed distance return false (and, like the interval test itself, it is the
     * only place the bound is derived from committed state).
     */
    Value *t_max = loadField(b, control, kRtCandidateTMax, Type::getFloatTy(llvm_context));
    storeField(
      b, control, kRtCandidateTMax,
      b.CreateSelect(b.CreateFCmpOLT(distance, t_max), distance, t_max)
    );
  });
}

/* -------------------------------------------------------------------------- */
/* Candidate state access                                                     */
/* -------------------------------------------------------------------------- */

llvm::Value *CandidateControlPointer(
  Module &module, const RaytracingExportContext &context, Value *block, IRBuilder<> &builder
) {
  if (!block)
    return nullptr;
  const RtInvocationLayout layout = rtInvocationLayout(context);
  return builder.CreateGEP(
    Type::getInt8Ty(module.getContext()), bytePointer(builder, block),
    builder.getInt32(layout.candidate_control)
  );
}

llvm::Value *EmitIntersectionResult(
  Module &module, const RaytracingExportContext &context, Value *block, Value *accept,
  IRBuilder<> &builder
) {
  LLVMContext &llvm_context = module.getContext();
  Value *continue_search = builder.getInt1(false);
  if (block) {
    Value *control = CandidateControlPointer(module, context, block, builder);
    Value *ended = loadField(builder, control, kRtCandidateEndSearch, builder.getInt32Ty());
    continue_search = builder.CreateICmpEQ(ended, builder.getInt32(0));
  }
  StructType *result = StructType::get(
    llvm_context, {Type::getInt1Ty(llvm_context), Type::getInt1Ty(llvm_context)}, true
  );
  Value *packed = PoisonValue::get(result);
  packed = builder.CreateInsertValue(packed, accept, 0);
  return builder.CreateInsertValue(packed, continue_search, 1);
}

/* -------------------------------------------------------------------------- */
/* dx.op.ignoreHit / dx.op.acceptHitAndEndSearch                              */
/* -------------------------------------------------------------------------- */

llvm::Error LowerAnyHitControlCall(
  Module &module, const RaytracingEntryArguments &entry, Value *block,
  const RaytracingExportContext &context, CallInst &call, uint32_t opcode
) {
  if (llvm::Error error = checkInvocationLayout(context))
    return error;
  if (!block)
    return createError(
      "dxil: an any-hit shader needs the per-invocation block its candidate control "
      "region lives in"
    );
  if (!call.getFunction()->getReturnType()->isVoidTy())
    return createError("dxil: an any-hit shader must return void");

  const RtInvocationLayout layout = rtInvocationLayout(context);
  IRBuilder<> builder(&call);
  Value *control = fieldPointer(
    builder, block, layout.candidate_control, Type::getInt8Ty(module.getContext())
  );
  Value *decision = nullptr;
  switch (opcode) {
  case kRtOpIgnoreHit:
    decision = ConstantInt::get(call.getContext(), APInt(32, kRtCandidateReject));
    break;
  case kRtOpAcceptHitAndEndSearch:
    decision = ConstantInt::get(
      call.getContext(), APInt(32, kRtCandidateAcceptAndEndSearch)
    );
    break;
  default:
    return createError(
      "dxil: dxil_rt_callbacks owns only dx.op.ignoreHit and "
      "dx.op.acceptHitAndEndSearch, got opcode " + Twine(opcode)
    );
  }

  storeField(builder, control, kRtCandidateDecision, decision);
  if (opcode == kRtOpAcceptHitAndEndSearch)
    /* Sticky: Decision is cleared for every candidate, the end-search is not. */
    storeField(builder, control, kRtCandidateEndSearch, builder.getInt32(1));

  /*
   * Both operations are `noreturn` in DXIL: the candidate is decided and the any-hit
   * body ends there, so nothing the shader placed after the call runs. The rest of
   * the block is unreachable - including the branch the entry transplant put in place
   * of the shader's own `ret` - so it is dropped and the body ends with the
   * `ret void` the AIR visible function an any-hit export is lowered to returns.
   *
   * An instruction that something outside the block still uses is left where it is
   * (unreachable but valid) rather than erased out from under its user: a phi in a
   * successor block can legitimately name a value from here.
   */
  BasicBlock *head = call.getParent();
  head->getTerminator()->eraseFromParent();
  /*
   * The instructions that followed the call are unreachable now, so they are dropped
   * (in reverse, so a value is erased after its users). One that something outside
   * the block still names - a phi in a successor block can legitimately reference it -
   * is left in place: unreachable code is valid, an erased value with a user is not.
   */
  SmallVector<Instruction *, 8> trailing;
  for (Instruction *instruction = call.getNextNode(); instruction != nullptr;
       instruction = instruction->getNextNode())
    trailing.push_back(instruction);
  for (Instruction *instruction : llvm::reverse(trailing))
    if (instruction->use_empty())
      instruction->eraseFromParent();

  IRBuilder<> terminator_builder(head->getContext());
  terminator_builder.SetInsertPoint(head);
  terminator_builder.CreateRetVoid();
  call.eraseFromParent();
  /*
   * `entry` is part of the frozen call contract (the pass dispatches every
   * raytracing opcode with the same arguments); this lowering needs nothing from it,
   * because the invocation block the caller hands over carries the whole candidate
   * state.
   */
  (void)entry;
  return llvm::Error::success();
}

/* -------------------------------------------------------------------------- */
/* dx.op.reportHit                                                            */
/* -------------------------------------------------------------------------- */

llvm::Error LowerReportHitCall(
  Module &module, const RaytracingEntryArguments &entry, Value *block,
  const RaytracingExportContext &context, CallInst &call
) {
  /* reportHit(opcode, float t, i32 hitKind, <attribute>*) */
  if (call.arg_size() != 4)
    return createError(
      "dx.op.reportHit expects 3 operands, got " + Twine(call.arg_size() - 1)
    );
  if (!call.getType()->isIntegerTy())
    return createError("dxil: dx.op.reportHit must return an integer");
  if (llvm::Error error = checkInvocationLayout(context))
    return error;
  if (!block)
    return createError(
      "dxil: dx.op.reportHit needs the per-invocation block its candidate control "
      "region lives in"
    );

  auto *attribute_pointer = dyn_cast<PointerType>(call.getArgOperand(3)->getType());
  if (!attribute_pointer)
    return createError("dxil: dx.op.reportHit's attribute operand is not a pointer");
  const uint64_t attribute_size =
    module.getDataLayout().getTypeStoreSize(attribute_pointer->getNonOpaquePointerElementType());
  if (attribute_size == 0)
    return createError("dxil: dx.op.reportHit reports an empty attribute struct");
  if (attribute_size > context.attribute_size)
    /*
     * D3D12 rejects a report whose attributes exceed the pipeline's maximum
     * attribute size, so the report can never be accepted: refusing at compile time
     * says so once instead of silently dropping every candidate at run time.
     */
    return createError(
      "dxil: dx.op.reportHit reports " + Twine(attribute_size) +
      " attribute bytes, which exceeds the pipeline's maximum attribute size of " +
      Twine(context.attribute_size)
    );

  /* ------------------------------------------------------------------------ */
  /* Everything that can fail is resolved before a single instruction is emitted */
  /* ------------------------------------------------------------------------ */
  CandidateIds ids;
  if (context.query_callbacks) {
    IRBuilder<> inputs(&call);
    Value *state = inputs.CreateGEP(inputs.getInt8Ty(), bytePointer(inputs, block), inputs.getInt32(context.hitStateOffset()));
    ids.primitive_id = loadField(inputs, state, offsetof(SM50_RT_HIT_STATE, PrimitiveIndex), inputs.getInt32Ty());
    ids.geometry_id = loadField(inputs, state, offsetof(SM50_RT_HIT_STATE, GeometryIndex), inputs.getInt32Ty());
    ids.instance_index = loadField(inputs, state, offsetof(SM50_RT_HIT_STATE, InstanceIndex), inputs.getInt32Ty());
    ids.instance_id = loadField(inputs, state, offsetof(SM50_RT_HIT_STATE, InstanceID), inputs.getInt32Ty());
  } else if (llvm::Error error = resolveCandidateIds(module, entry, ids)) {
    return error;
  }
  if (context.any_hit_index != SM50_RT_INDEX_NONE && !entry.visible_function_table && !context.query_callbacks)
    return createError(
      "dxil: dx.op.reportHit must run the hit group's any-hit shader for every "
      "candidate, but this intersection export has no visible function table argument"
    );

  LLVMContext &llvm_context = module.getContext();
  const RtInvocationLayout layout = rtInvocationLayout(context);
  IRBuilder<> builder(&call);
  Value *t = call.getArgOperand(1);
  if (t->getType() != Type::getFloatTy(llvm_context))
    return createError("dxil: dx.op.reportHit's distance operand is not a float");
  Value *hit_kind = asWord(builder, call.getArgOperand(2));

  Value *control = fieldPointer(
    builder, block, layout.candidate_control, Type::getInt8Ty(llvm_context)
  );
  Value *candidate_attributes = fieldPointer(
    builder, block, layout.candidate_attribute, Type::getInt8Ty(llvm_context)
  );
  Value *t_min = loadField(builder, control, kRtCandidateTMin, Type::getFloatTy(llvm_context));
  Value *t_max = loadField(builder, control, kRtCandidateTMax, Type::getFloatTy(llvm_context));

  /* No candidate is decided yet: an unbound report must not read a stale decision. */
  storeField(builder, control, kRtCandidateDecision, builder.getInt32(kRtCandidateNone));

  /*
   * D3D12 reports a hit only inside the ray's own interval, and the interval is not
   * the traversal's `tmin` / `tmax` as the ray generation shader declared it but the
   * one the *active* trace used: the traversal lowering publishes both into the
   * candidate control before issuing the trace, because RayTCurrent does not carry
   * them until the traversal is over. A NaN distance compares false and is therefore
   * not reported, which is also what D3D12 does.
   */
  Value *within_interval = builder.CreateAnd(
    builder.CreateFCmpOLE(t_min, t), builder.CreateFCmpOLE(t, t_max)
  );

  emitConditional(builder, within_interval, "rt.report", [&](IRBuilder<> &inner) {
    inner.CreateMemCpy(
      candidate_attributes, MaybeAlign(4), call.getArgOperand(3), MaybeAlign(4),
      inner.getInt64(attribute_size)
    );
    /*
     * The candidate's system values go into the hit state, because the any-hit shader
     * runs for *this* candidate and must read this candidate's HitKind, ids and
     * distance. Nothing committed is written here: Distance / HitKind /
     * AttributeSize and the committed attribute region are written by
     * EmitCandidateCommit alone, so a rejected candidate cannot leave them behind.
     */
    publishCandidateHitState(context, ids, block, t, hit_kind, inner);

    Value *accepted = EmitAnyHitCandidate(module, entry, context, block, inner);
    EmitCandidateCommit(
      module, context, block, t, hit_kind, static_cast<uint32_t>(attribute_size), ids,
      accepted, inner
    );
    /*
     * The any-hit shader's own verdict is left in place: it wrote `reject` or
     * `accept and end search`, and the end-search has to survive into the traversal's
     * continue_search result. Only a hit group with no any-hit shader - the decision is
     * still `none` - is recorded as a plain accept here.
     */
    Value *decision = loadField(inner, control, kRtCandidateDecision, inner.getInt32Ty());
    Value *undecided = inner.CreateICmpEQ(decision, inner.getInt32(kRtCandidateNone));
    storeField(
      inner, control, kRtCandidateDecision,
      inner.CreateSelect(
        inner.CreateAnd(accepted, undecided), inner.getInt32(kRtCandidateAccept), decision
      )
    );
  });

  /*
   * The candidate has been decided, so the hit state goes back to describing the
   * committed hit: a candidate the any-hit shader rejected has published its own
   * system values there, and the closest-hit shader that runs after the traversal
   * must read the hit that actually won - never the last candidate the intersection
   * shader happened to report. The committed hit also tightens the ray's upper bound
   * to its distance, which is what makes a later, farther `reportHit` return false.
   */
  EmitCommittedHitStateRestore(module, context, block, builder);

  /*
   * The call's result is the any-hit verdict for the candidate it reported, and it
   * is false when the report never happened at all because the distance was outside
   * the ray's interval.
   */
  Value *decision = loadField(builder, control, kRtCandidateDecision, builder.getInt32Ty());
  Value *reported = builder.CreateAnd(
    within_interval, builder.CreateICmpNE(decision, builder.getInt32(kRtCandidateReject))
  );
  Value *result = reported;
  if (call.getType() != Type::getInt1Ty(llvm_context))
    result = builder.CreateZExt(reported, call.getType());
  call.replaceAllUsesWith(result);
  call.eraseFromParent();
  return llvm::Error::success();
}

RtShaderRecord EmitShaderRecordLookup(
  IRBuilder<> &builder, Value *dispatch, RtShaderTable table, Value *index
) {
  LLVMContext &context = builder.getContext();
  Type *i32 = builder.getInt32Ty();
  Type *i64 = builder.getInt64Ty();
  struct Fields {
    uint32_t base, stride, count;
  };
  /* The ray generation record is a single record (stride 0, count 1). */
  constexpr uint32_t kNone = ~0u;
  Fields fields{};
  switch (table) {
  case RtShaderTable::RayGeneration:
    fields = {offsetof(SM50_RT_DISPATCH_ARGUMENTS, RayGenerationRecord), kNone, kNone};
    break;
  case RtShaderTable::Miss:
    fields = {offsetof(SM50_RT_DISPATCH_ARGUMENTS, MissRecordBase),
              offsetof(SM50_RT_DISPATCH_ARGUMENTS, MissRecordStride),
              offsetof(SM50_RT_DISPATCH_ARGUMENTS, MissRecordCount)};
    break;
  case RtShaderTable::HitGroup:
    fields = {offsetof(SM50_RT_DISPATCH_ARGUMENTS, HitGroupRecordBase),
              offsetof(SM50_RT_DISPATCH_ARGUMENTS, HitGroupRecordStride),
              offsetof(SM50_RT_DISPATCH_ARGUMENTS, HitGroupRecordCount)};
    break;
  case RtShaderTable::Callable:
    fields = {offsetof(SM50_RT_DISPATCH_ARGUMENTS, CallableRecordBase),
              offsetof(SM50_RT_DISPATCH_ARGUMENTS, CallableRecordStride),
              offsetof(SM50_RT_DISPATCH_ARGUMENTS, CallableRecordCount)};
    break;
  }
  Value *base = loadField(builder, dispatch, fields.base, i64);
  Value *stride = fields.stride == kNone ? builder.getInt32(0) : loadField(builder, dispatch, fields.stride, i32);
  Value *count = fields.count == kNone ? builder.getInt32(1) : loadField(builder, dispatch, fields.count, i32);
  Value *export_count = loadField(builder, dispatch, offsetof(SM50_RT_DISPATCH_ARGUMENTS, ExportCount), i32);
  Value *token = loadField(builder, dispatch, offsetof(SM50_RT_DISPATCH_ARGUMENTS, PipelineToken), i32);
  /* A stride of 0 resolves every index to the table's single record. */
  Value *in_table = builder.CreateAnd(
    builder.CreateICmpNE(base, builder.getInt64(0)),
    builder.CreateOr(builder.CreateICmpEQ(stride, builder.getInt32(0)), builder.CreateICmpULT(index, count))
  );
  Value *record = builder.CreateAdd(
    base, builder.CreateMul(builder.CreateZExt(index, i64), builder.CreateZExt(stride, i64))
  );

  /* OwnedShaderIdentifier (d3d12_state_object.hpp): magic, version | kind << 16, token, export. */
  BasicBlock *head = builder.GetInsertBlock();
  Function *function = head->getParent();
  BasicBlock *continuation = nullptr;
  if (builder.GetInsertPoint() != head->end())
    continuation = head->splitBasicBlock(builder.GetInsertPoint(), "rt.record.cont");
  if (head->getTerminator())
    head->getTerminator()->eraseFromParent();
  if (!continuation)
    continuation = BasicBlock::Create(context, "rt.record.cont", function);
  BasicBlock *read = BasicBlock::Create(context, "rt.record", function, continuation);
  builder.SetInsertPoint(head);
  builder.CreateCondBr(in_table, read, continuation);
  builder.SetInsertPoint(read);
  Value *words = builder.CreateIntToPtr(record, Type::getInt32PtrTy(context, 1));
  auto word = [&](uint32_t i) {
    return builder.CreateAlignedLoad(i32, builder.CreateGEP(i32, words, builder.getInt32(i)), Align(4));
  };
  Value *ours = builder.CreateAnd(
    builder.CreateAnd(builder.CreateICmpEQ(word(0), builder.getInt32(0x4c413344u)),
                      builder.CreateICmpEQ(word(1), builder.getInt32(1u | (1u << 16)))),
    builder.CreateICmpEQ(word(2), token)
  );
  Value *candidate = word(3);
  Value *valid = builder.CreateAnd(ours, builder.CreateICmpULT(candidate, export_count));
  Value *read_export = builder.CreateSelect(valid, candidate, builder.getInt32(SM50_RT_INDEX_NONE));
  Value *read_entry = builder.CreateSelect(
    valid, builder.CreateAdd(record, builder.getInt64(32)), builder.getInt64(0)
  );
  BasicBlock *read_end = builder.GetInsertBlock();
  builder.CreateBr(continuation);
  builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
  PHINode *export_index = builder.CreatePHI(i32, 2, "rt.record.export");
  export_index->addIncoming(builder.getInt32(SM50_RT_INDEX_NONE), head);
  export_index->addIncoming(read_export, read_end);
  PHINode *local_entry = builder.CreatePHI(i64, 2, "rt.record.entry");
  local_entry->addIncoming(builder.getInt64(0), head);
  local_entry->addIncoming(read_entry, read_end);
  return {export_index, local_entry};
}

} // namespace d3dalloy::dxil
