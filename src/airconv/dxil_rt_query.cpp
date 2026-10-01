/*
 * DXIL inline raytracing (RayQuery, DXR Tier 1.1) -> AIR intersection_query.
 *
 * LowerRayQueries(module) rewrites every `dx.op.allocateRayQuery` / `rayQuery_*`
 * call of a module into `air.*_intersection_query.instancing.triangle_data`
 * intrinsics. It is stage independent - a RayQuery may appear in any shader stage -
 * and runs at the start of LowerOperations, after the resource pass, which is where
 * the acceleration structure a TraceRayInline names becomes known:
 *
 *   - the resource pass (any stage, root SRV / descriptor table / heap) and the
 *     raytracing pass (library exports) resolve the structure's handle to the D3D12
 *     GPU virtual address its descriptor holds, and call RewriteTraceRayInline, which
 *     replaces `dx.op.rayQuery_TraceRayInline` by `dxmt.rayquery.trace_inline` with
 *     that address in place of the handle;
 *   - this pass then lowers the query object itself.
 *
 * Contract provenance. Every `air.` name and operand below was read off the public
 * Metal front end (`xcrun metal -std=metal3.1 -S -emit-llvm`) for
 * `intersection_query<triangle_data, instancing>` in kernel, vertex, fragment and
 * visible functions (build/local/p3/DxrRayQuery/probes/rq.metal, rq2.metal), and the
 * semantics below were measured on the GPU under Metal API + shader validation
 * (probes/rqprobe.m, acc2.m, acc3.m, nullq.m):
 *
 *   reset(q, origin, direction, tmin, tmax, AS*, mask, winding, triangle_cull,
 *         geometry_cull, opacity_cull, forced_opacity, geometry_type=3, -1, -1, 0,
 *         i1 assume_identity_transforms, i1 accept_any_intersection)
 *     The configuration operands are the same as `air.intersect`'s, so the D3D12
 *     ray flags map exactly as they do for TraceRay (dxil_rt_operations.cpp).
 *   - Opaque triangles are committed by the traversal itself; only non-opaque
 *     triangles and every bounding box (opaque or not) are returned as candidates:
 *     D3D12's CANDIDATE_NON_OPAQUE_TRIANGLE / CANDIDATE_PROCEDURAL_PRIMITIVE.
 *   - accept_any_intersection ends the search at the first commit (opaque or by the
 *     shader) and only then, which is RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH.
 *   - Committing twice is harmless; abort() makes the next next() return false and
 *     keeps the committed hit.
 *   - Candidate/committed ray origin/direction are object space; the transform
 *     getters return four <3 x float> columns; get_committed_distance() is the ray's
 *     TMax while nothing is committed; committed types none/triangle/bounding_box
 *     are exactly COMMITTED_NOTHING / TRIANGLE_HIT / PROCEDURAL_PRIMITIVE_HIT.
 *   - Resetting against the null instance structure is reported by shader validation
 *     ("Invalid Acceleration Structure type Unknown"), so D3D12's null structure
 *     (GPU VA 0) is not traced at all: the query records it, Proceed() returns false,
 *     CommittedStatus() is COMMITTED_NOTHING and CommittedRayT() is TMax.
 *
 * WorldRayOrigin/Direction, RayTMin and RayFlags (template flags OR'd with the
 * dynamic ones) are what the shader passed, so they are kept in per-query
 * allocations instead of asking the query. InstanceContributionToHitGroupIndex has
 * no Metal instance field: it is read out of the structure's header exactly like a
 * TraceRay does (qword 1 = the uint32 contribution array the AS prepass filled).
 */

#include "dxil_converter.hpp"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace dxmt::dxil {

namespace {

constexpr const char kTraceInlineFunction[] = "dxmt.rayquery.trace_inline";
constexpr const char kQuerySuffix[] = "_intersection_query.instancing.triangle_data";

/* dx.op opcodes of the inline raytracing block (DXC DxilOperations.h). */
enum RayQueryOpcode : uint32_t {
  kAllocate = 178,
  kTraceRayInline = 179,
  kProceed = 180,
  kAbort = 181,
  kCommitNonOpaqueTriangleHit = 182,
  kCommitProceduralPrimitiveHit = 183,
  kCommittedStatus = 184,
  kCandidateType = 185,
  kCandidateObjectToWorld3x4 = 186,
  kCandidateWorldToObject3x4 = 187,
  kCommittedObjectToWorld3x4 = 188,
  kCommittedWorldToObject3x4 = 189,
  kCandidateProceduralPrimitiveNonOpaque = 190,
  kCandidateTriangleFrontFace = 191,
  kCommittedTriangleFrontFace = 192,
  kCandidateTriangleBarycentrics = 193,
  kCommittedTriangleBarycentrics = 194,
  kRayFlags = 195,
  kWorldRayOrigin = 196,
  kWorldRayDirection = 197,
  kRayTMin = 198,
  kCandidateTriangleRayT = 199,
  kCommittedRayT = 200,
  kCandidateInstanceIndex = 201,
  kCandidateInstanceID = 202,
  kCandidateGeometryIndex = 203,
  kCandidatePrimitiveIndex = 204,
  kCandidateObjectRayOrigin = 205,
  kCandidateObjectRayDirection = 206,
  kCommittedInstanceIndex = 207,
  kCommittedInstanceID = 208,
  kCommittedGeometryIndex = 209,
  kCommittedPrimitiveIndex = 210,
  kCommittedObjectRayOrigin = 211,
  kCommittedObjectRayDirection = 212,
  /* 213 is GeometryIndex(), a hit shader system value (dxil_rt_operations.cpp). */
  kCandidateInstanceContribution = 214,
  kCommittedInstanceContribution = 215,
  kAllocate2 = 258,
};

/** Metal intersection_type of a candidate: triangle (1) or bounding_box (2). */
constexpr uint32_t kMetalBoundingBox = 2;
/** D3D12 CANDIDATE_TYPE. */
constexpr uint32_t kCandidateNonOpaqueTriangle = 0;
constexpr uint32_t kCandidateProcedural = 1;
/** D3D12 RAY_FLAG bits with no plain two-bit field. */
constexpr uint32_t kRayFlagAcceptFirstHit = 0x4;

bool dxilOpcode(const CallInst &call, uint32_t &opcode) {
  const Function *callee = call.getCalledFunction();
  if (!callee || !callee->isDeclaration() || call.arg_size() < 1)
    return false;
  if (callee->getName() == kTraceInlineFunction) {
    opcode = kTraceRayInline;
    return true;
  }
  if (!callee->getName().startswith("dx.op."))
    return false;
  auto *constant = dyn_cast<ConstantInt>(call.getArgOperand(0));
  if (!constant)
    return false;
  opcode = static_cast<uint32_t>(constant->getZExtValue());
  return true;
}

bool isRayQueryOpcode(uint32_t opcode) {
  return (opcode >= kAllocate && opcode <= kCommittedInstanceContribution && opcode != 213) ||
         opcode == kAllocate2;
}

/** Per-query state: the AIR query and what the shader traced it with. */
struct Query {
  Value *object = nullptr;
  AllocaInst *origin = nullptr, *direction = nullptr;
  AllocaInst *tmin = nullptr, *tmax = nullptr;
  AllocaInst *flags = nullptr;
  /** i64 D3D12 GPU VA of the traced structure; 0 = the null structure (or not traced). */
  AllocaInst *address = nullptr;
  uint32_t template_flags = 0;
};

class RayQueryLowerer {
public:
  explicit RayQueryLowerer(Module &module)
      : module(module), context(module.getContext()), f32(Type::getFloatTy(context)),
        i32(Type::getInt32Ty(context)), i64(Type::getInt64Ty(context)), i1(Type::getInt1Ty(context)),
        f32x2(FixedVectorType::get(f32, 2)), f32x3(FixedVectorType::get(f32, 3)) {
    StructType *query = StructType::getTypeByName(context, "struct._intersection_query_t");
    if (!query)
      query = StructType::create(context, "struct._intersection_query_t");
    query_ptr = query->getPointerTo();
    StructType *as = StructType::getTypeByName(context, "struct._instance_acceleration_structure_t");
    if (!as)
      as = StructType::create(context, "struct._instance_acceleration_structure_t");
    as_ptr = as->getPointerTo(1);
    transform = StructType::get(context, {f32x3, f32x3, f32x3, f32x3});
  }

  Error run();

private:
  Module &module;
  LLVMContext &context;
  Type *f32, *i32, *i64, *i1, *f32x2, *f32x3;
  PointerType *query_ptr = nullptr, *as_ptr = nullptr;
  StructType *transform = nullptr;
  SmallVector<std::unique_ptr<Query>, 4> queries;
  DenseMap<Value *, Query *> by_handle;

  FunctionCallee intrinsic(StringRef base, Type *result, ArrayRef<Type *> params, bool reads_only) {
    AttributeList attributes = AttributeList::get(
      context, {{~0U, Attribute::get(context, Attribute::NoUnwind)},
                {~0U, Attribute::get(context, Attribute::WillReturn)}}
    );
    if (reads_only)
      attributes = attributes.addFnAttribute(context, Attribute::ReadOnly);
    return module.getOrInsertFunction(
      (Twine("air.") + base + kQuerySuffix).str(), FunctionType::get(result, params, false), attributes
    );
  }
  Value *getter(IRBuilder<> &b, StringRef base, Type *result, Query &q) {
    return b.CreateCall(intrinsic(base, result, {query_ptr}, true), {q.object});
  }

  Query *queryOf(Value *handle);
  Error lowerFunction(Function &function);
  Error lowerCall(CallInst &call, uint32_t opcode);
  Error lowerTrace(CallInst &call, Query &q);
  Value *guarded(Instruction *at, Value *condition, Value *otherwise, function_ref<Value *(IRBuilder<> &)> body);
  Value *contribution(IRBuilder<> &b, Instruction *at, Query &q, Value *instance_index);
};

Query *RayQueryLowerer::queryOf(Value *handle) {
  auto found = by_handle.find(handle);
  if (found != by_handle.end())
    return found->second;
  /* A query handle merged by a phi or select is accepted only when every input names the same query. */
  SmallVector<Value *, 4> inputs;
  if (auto *phi = dyn_cast<PHINode>(handle))
    inputs.append(phi->incoming_values().begin(), phi->incoming_values().end());
  else if (auto *select = dyn_cast<SelectInst>(handle))
    inputs = {select->getTrueValue(), select->getFalseValue()};
  else
    return nullptr;
  by_handle[handle] = nullptr; /* breaks phi cycles */
  Query *common = nullptr;
  for (Value *input : inputs) {
    if (input == handle)
      continue;
    Query *q = queryOf(input);
    if (!q || (common && common != q)) {
      by_handle.erase(handle);
      return nullptr;
    }
    common = q;
  }
  by_handle[handle] = common;
  return common;
}

/**
 * Evaluate `body` only when `condition` holds, splitting the block in front of `at`;
 * the merged value (or nothing for a void body) is available at `at`.
 */
Value *RayQueryLowerer::guarded(
  Instruction *at, Value *condition, Value *otherwise, function_ref<Value *(IRBuilder<> &)> body
) {
  BasicBlock *head = at->getParent();
  Instruction *terminator = SplitBlockAndInsertIfThen(condition, at, /*Unreachable=*/false);
  IRBuilder<> b(terminator);
  Value *value = body(b);
  if (!otherwise)
    return nullptr;
  IRBuilder<> merge(at);
  PHINode *phi = merge.CreatePHI(otherwise->getType(), 2);
  phi->addIncoming(value, terminator->getParent());
  phi->addIncoming(otherwise, head);
  return phi;
}

Value *RayQueryLowerer::contribution(IRBuilder<> &b, Instruction *at, Query &q, Value *instance_index) {
  Value *address = b.CreateLoad(i64, q.address);
  Value *array = guarded(at, b.CreateICmpNE(address, b.getInt64(0)), b.getInt64(0), [&](IRBuilder<> &g) -> Value * {
    return g.CreateLoad(i64, g.CreateGEP(i64, g.CreateIntToPtr(address, i64->getPointerTo(1)), g.getInt32(1)));
  });
  IRBuilder<> after(at);
  return guarded(at, after.CreateICmpNE(array, after.getInt64(0)), after.getInt32(0), [&](IRBuilder<> &g) -> Value * {
    return g.CreateLoad(i32, g.CreateGEP(i32, g.CreateIntToPtr(array, i32->getPointerTo(1)), instance_index));
  });
}

Error RayQueryLowerer::lowerTrace(CallInst &call, Query &q) {
  /* dxmt.rayquery.trace_inline(i32 op, i32 query, i64 address, i32 flags, i32 mask,
   *   float ox, oy, oz, tmin, dx, dy, dz, tmax) */
  if (call.arg_size() != 13 || !call.getArgOperand(2)->getType()->isIntegerTy(64))
    return createError("dxil: rayQuery_TraceRayInline's acceleration structure was not resolved");
  IRBuilder<> b(&call);
  Value *address = call.getArgOperand(2);
  Value *flags = b.CreateOr(call.getArgOperand(3), b.getInt32(q.template_flags));
  Value *mask = b.CreateAnd(call.getArgOperand(4), b.getInt32(0xff));
  auto vector = [&](unsigned first) {
    Value *v = PoisonValue::get(f32x3);
    for (unsigned i = 0; i < 3; ++i)
      v = b.CreateInsertElement(v, call.getArgOperand(first + i), uint64_t(i));
    return v;
  };
  Value *origin = vector(5), *direction = vector(9);
  Value *tmin = call.getArgOperand(8), *tmax = call.getArgOperand(12);
  b.CreateStore(origin, q.origin);
  b.CreateStore(direction, q.direction);
  b.CreateStore(tmin, q.tmin);
  b.CreateStore(tmax, q.tmax);
  b.CreateStore(flags, q.flags);
  b.CreateStore(address, q.address);

  auto field = [&](unsigned shift) { return b.CreateAnd(b.CreateLShr(flags, b.getInt32(shift)), b.getInt32(3)); };
  Value *forced_opacity = field(0);
  Value *opacity_cull = field(6);
  Value *geometry_cull = field(8);
  /* D3D12 CULL_BACK is bit 4 and CULL_FRONT bit 5; AIR's triangle_cull_mode is 1 = front, 2 = back. */
  Value *back = b.CreateAnd(b.CreateLShr(flags, b.getInt32(4)), b.getInt32(1));
  Value *front = b.CreateAnd(b.CreateLShr(flags, b.getInt32(5)), b.getInt32(1));
  Value *triangle_cull = b.CreateOr(b.CreateShl(back, b.getInt32(1)), front);
  Value *accept_first = b.CreateICmpNE(b.CreateAnd(flags, b.getInt32(kRayFlagAcceptFirstHit)), b.getInt32(0));

  FunctionCallee reset = intrinsic(
    "reset", Type::getVoidTy(context),
    {query_ptr, f32x3, f32x3, f32, f32, as_ptr, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1}, false
  );
  guarded(&call, b.CreateICmpNE(address, b.getInt64(0)), nullptr, [&](IRBuilder<> &g) -> Value * {
    /* The structure's 64 byte header starts with its Metal resource id. */
    Value *id = g.CreateLoad(i64, g.CreateIntToPtr(address, i64->getPointerTo(1)));
    g.CreateCall(
      reset, {q.object, origin, direction, tmin, tmax, g.CreateIntToPtr(id, as_ptr), mask,
              g.getInt32(0) /* clockwise: D3D12's default facing */, triangle_cull, geometry_cull, opacity_cull,
              forced_opacity, g.getInt32(3), g.getInt32(-1), g.getInt32(-1), g.getInt32(0), g.getFalse(), accept_first}
    );
    return nullptr;
  });
  call.eraseFromParent();
  return Error::success();
}

Error RayQueryLowerer::lowerCall(CallInst &call, uint32_t opcode) {
  if (call.arg_size() < 2)
    return createError("dxil: RayQuery operation without a query operand");
  Query *query = queryOf(call.getArgOperand(1));
  if (!query)
    return unsupported("DXIL RayQuery handle that does not name a single allocateRayQuery");
  Query &q = *query;
  if (opcode == kTraceRayInline)
    return lowerTrace(call, q);

  IRBuilder<> b(&call);
  Value *traced = b.CreateICmpNE(b.CreateLoad(i64, q.address), b.getInt64(0));
  auto component = [&](unsigned index, uint64_t limit, uint64_t &out) -> bool {
    auto *constant = index < call.arg_size() ? dyn_cast<ConstantInt>(call.getArgOperand(index)) : nullptr;
    if (!constant || constant->getZExtValue() > limit)
      return false;
    out = constant->getZExtValue();
    return true;
  };
  auto vectorGetter = [&](StringRef base, Type *type, uint64_t limit) -> Value * {
    uint64_t c = 0;
    if (!component(2, limit, c))
      return nullptr;
    return b.CreateExtractElement(getter(b, base, type, q), c);
  };
  auto matrixGetter = [&](StringRef base) -> Value * {
    uint64_t row = 0, column = 0;
    if (!component(2, 2, row) || !component(3, 3, column))
      return nullptr;
    Value *columns = getter(b, base, transform, q);
    return b.CreateExtractElement(b.CreateExtractValue(columns, unsigned(column)), row);
  };
  auto storedVector = [&](AllocaInst *slot) -> Value * {
    uint64_t c = 0;
    if (!component(2, 2, c))
      return nullptr;
    return b.CreateExtractElement(b.CreateLoad(f32x3, slot), c);
  };

  Value *value = nullptr;
  switch (opcode) {
  case kProceed:
    value = guarded(&call, traced, b.getFalse(), [&](IRBuilder<> &g) -> Value * {
      return g.CreateCall(intrinsic("next", i1, {query_ptr}, false), {q.object});
    });
    break;
  case kAbort:
    guarded(&call, traced, nullptr, [&](IRBuilder<> &g) -> Value * {
      g.CreateCall(intrinsic("abort", Type::getVoidTy(context), {query_ptr}, false), {q.object});
      return nullptr;
    });
    break;
  case kCommitNonOpaqueTriangleHit:
    b.CreateCall(intrinsic("commit_triangle_intersection", Type::getVoidTy(context), {query_ptr}, false), {q.object});
    break;
  case kCommitProceduralPrimitiveHit:
    if (call.arg_size() != 3)
      return createError("dxil: rayQuery_CommitProceduralPrimitiveHit expects a distance");
    b.CreateCall(
      intrinsic("commit_bounding_box_intersection", Type::getVoidTy(context), {query_ptr, f32}, false),
      {q.object, call.getArgOperand(2)}
    );
    break;
  case kCommittedStatus:
    value = b.CreateSelect(traced, getter(b, "get_committed_intersection_type", i32, q), b.getInt32(0));
    break;
  case kCandidateType:
    value = b.CreateSelect(
      b.CreateICmpEQ(getter(b, "get_candidate_intersection_type", i32, q), b.getInt32(kMetalBoundingBox)),
      b.getInt32(kCandidateProcedural), b.getInt32(kCandidateNonOpaqueTriangle)
    );
    break;
  case kCandidateObjectToWorld3x4:
    value = matrixGetter("get_candidate_object_to_world_transform");
    break;
  case kCandidateWorldToObject3x4:
    value = matrixGetter("get_candidate_world_to_object_transform");
    break;
  case kCommittedObjectToWorld3x4:
    value = matrixGetter("get_committed_object_to_world_transform");
    break;
  case kCommittedWorldToObject3x4:
    value = matrixGetter("get_committed_world_to_object_transform");
    break;
  case kCandidateProceduralPrimitiveNonOpaque:
    value = getter(b, "is_candidate_non_opaque_bounding_box", i1, q);
    break;
  case kCandidateTriangleFrontFace:
    value = getter(b, "is_candidate_triangle_front_facing", i1, q);
    break;
  case kCommittedTriangleFrontFace:
    value = getter(b, "is_committed_triangle_front_facing", i1, q);
    break;
  case kCandidateTriangleBarycentrics:
    value = vectorGetter("get_candidate_triangle_barycentric_coord", f32x2, 1);
    break;
  case kCommittedTriangleBarycentrics:
    value = vectorGetter("get_committed_triangle_barycentric_coord", f32x2, 1);
    break;
  case kRayFlags:
    value = b.CreateLoad(i32, q.flags);
    break;
  case kWorldRayOrigin:
    value = storedVector(q.origin);
    break;
  case kWorldRayDirection:
    value = storedVector(q.direction);
    break;
  case kRayTMin:
    value = b.CreateLoad(f32, q.tmin);
    break;
  case kCandidateTriangleRayT:
    value = getter(b, "get_candidate_triangle_distance", f32, q);
    break;
  case kCommittedRayT:
    value = b.CreateSelect(traced, getter(b, "get_committed_distance", f32, q), b.CreateLoad(f32, q.tmax));
    break;
  /* Metal's instance_id is the instance's index in the structure (D3D12 InstanceIndex);
   * user_instance_id is the application's InstanceID. */
  case kCandidateInstanceIndex:
    value = getter(b, "get_candidate_instance_id", i32, q);
    break;
  case kCandidateInstanceID:
    value = getter(b, "get_candidate_user_instance_id", i32, q);
    break;
  case kCandidateGeometryIndex:
    value = getter(b, "get_candidate_geometry_id", i32, q);
    break;
  case kCandidatePrimitiveIndex:
    value = getter(b, "get_candidate_primitive_id", i32, q);
    break;
  case kCandidateObjectRayOrigin:
    value = vectorGetter("get_candidate_ray_origin", f32x3, 2);
    break;
  case kCandidateObjectRayDirection:
    value = vectorGetter("get_candidate_ray_direction", f32x3, 2);
    break;
  case kCommittedInstanceIndex:
    value = getter(b, "get_committed_instance_id", i32, q);
    break;
  case kCommittedInstanceID:
    value = getter(b, "get_committed_user_instance_id", i32, q);
    break;
  case kCommittedGeometryIndex:
    value = getter(b, "get_committed_geometry_id", i32, q);
    break;
  case kCommittedPrimitiveIndex:
    value = getter(b, "get_committed_primitive_id", i32, q);
    break;
  case kCommittedObjectRayOrigin:
    value = vectorGetter("get_committed_ray_origin", f32x3, 2);
    break;
  case kCommittedObjectRayDirection:
    value = vectorGetter("get_committed_ray_direction", f32x3, 2);
    break;
  case kCandidateInstanceContribution:
    value = contribution(b, &call, q, getter(b, "get_candidate_instance_id", i32, q));
    break;
  case kCommittedInstanceContribution:
    value = contribution(b, &call, q, getter(b, "get_committed_instance_id", i32, q));
    break;
  default:
    return unsupported(("DXIL RayQuery opcode " + Twine(opcode) + " is not a RayQuery method").str());
  }
  if (!call.getType()->isVoidTy()) {
    if (!value)
      return createError("dxil: RayQuery operation " + Twine(opcode) + " has a non-constant or out of range component");
    if (value->getType() != call.getType())
      return createError("dxil: RayQuery operation " + Twine(opcode) + " result type mismatch");
    call.replaceAllUsesWith(value);
  }
  call.eraseFromParent();
  return Error::success();
}

Error RayQueryLowerer::lowerFunction(Function &function) {
  SmallVector<CallInst *, 4> allocations;
  SmallVector<std::pair<CallInst *, uint32_t>, 32> operations;
  for (BasicBlock &block : function)
    for (Instruction &instruction : block) {
      auto *call = dyn_cast<CallInst>(&instruction);
      uint32_t opcode = 0;
      if (!call || !dxilOpcode(*call, opcode) || !isRayQueryOpcode(opcode))
        continue;
      if (opcode == kAllocate || opcode == kAllocate2)
        allocations.push_back(call);
      else
        operations.emplace_back(call, opcode);
    }
  if (allocations.empty() && operations.empty())
    return Error::success();

  /* Every query lives for the whole invocation: its state is allocated in the entry block. */
  BasicBlock &entry = function.getEntryBlock();
  BasicBlock::iterator point = entry.getFirstInsertionPt();
  while (point != entry.end() && isa<AllocaInst>(*point))
    ++point;
  IRBuilder<> b(&entry, point);
  FunctionCallee allocate = intrinsic("allocate", query_ptr, {}, false);
  for (CallInst *call : allocations) {
    auto *flags = dyn_cast<ConstantInt>(call->getArgOperand(1));
    if (!flags)
      return createError("dxil: allocateRayQuery's template ray flags are not a constant");
    auto q = std::make_unique<Query>();
    q->template_flags = uint32_t(flags->getZExtValue());
    q->origin = b.CreateAlloca(f32x3);
    q->direction = b.CreateAlloca(f32x3);
    q->tmin = b.CreateAlloca(f32);
    q->tmax = b.CreateAlloca(f32);
    q->flags = b.CreateAlloca(i32);
    q->address = b.CreateAlloca(i64);
    q->object = b.CreateCall(allocate, {});
    b.CreateStore(ConstantAggregateZero::get(f32x3), q->origin);
    b.CreateStore(ConstantAggregateZero::get(f32x3), q->direction);
    b.CreateStore(ConstantFP::get(f32, 0.0), q->tmin);
    b.CreateStore(ConstantFP::get(f32, 0.0), q->tmax);
    b.CreateStore(b.getInt32(q->template_flags), q->flags);
    b.CreateStore(b.getInt64(0), q->address);
    by_handle[call] = q.get();
    queries.push_back(std::move(q));
  }
  for (auto &[call, opcode] : operations)
    if (Error error = lowerCall(*call, opcode))
      return error;
  for (CallInst *call : allocations) {
    call->replaceAllUsesWith(PoisonValue::get(call->getType()));
    call->eraseFromParent();
  }
  /* Release every query of this invocation where it ends, as the Metal front end does. */
  FunctionCallee deallocate = intrinsic("deallocate", Type::getVoidTy(context), {query_ptr}, false);
  for (BasicBlock &block : function)
    if (auto *ret = dyn_cast<ReturnInst>(block.getTerminator())) {
      IRBuilder<> r(ret);
      for (auto &q : queries)
        if (q->object && cast<Instruction>(q->object)->getFunction() == &function)
          r.CreateCall(deallocate, {q->object});
    }
  return Error::success();
}

Error RayQueryLowerer::run() {
  for (Function &function : module)
    if (!function.isDeclaration())
      if (Error error = lowerFunction(function))
        return error;
  SmallVector<Function *, 8> dead;
  for (Function &function : module)
    if (function.isDeclaration() && function.use_empty() &&
        (function.getName() == kTraceInlineFunction || function.getName().startswith("dx.op.rayQuery_") ||
         function.getName().startswith("dx.op.allocateRayQuery")))
      dead.push_back(&function);
  for (Function *function : dead)
    function->eraseFromParent();
  return Error::success();
}

} // namespace

llvm::Error RewriteTraceRayInline(llvm::CallInst &call, llvm::Value *gpu_address) {
  /* dx.op.rayQuery_TraceRayInline(i32 179, i32 query, %dx.types.Handle, i32 flags, i32 mask,
   *   float ox, oy, oz, tmin, dx, dy, dz, tmax) */
  if (call.arg_size() != 13 || !gpu_address->getType()->isIntegerTy(64))
    return createError("dxil: dx.op.rayQuery_TraceRayInline expects 12 operands");
  Module &module = *call.getModule();
  SmallVector<Value *, 13> operands(call.args());
  operands[2] = gpu_address;
  SmallVector<Type *, 13> types;
  for (Value *operand : operands)
    types.push_back(operand->getType());
  FunctionCallee trace = module.getOrInsertFunction(
    kTraceInlineFunction, FunctionType::get(Type::getVoidTy(module.getContext()), types, false)
  );
  IRBuilder<> b(&call);
  b.CreateCall(trace, operands);
  call.eraseFromParent();
  return llvm::Error::success();
}

llvm::Error LowerRayQueries(llvm::Module &module) {
  RayQueryLowerer lowerer(module);
  return lowerer.run();
}

} // namespace dxmt::dxil
