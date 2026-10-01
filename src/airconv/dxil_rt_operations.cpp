/*
 * DXIL `dx.op` ray tracing operations -> AIR (owned-rt-compiler).
 *
 * Scope of this translation unit:
 *
 *   LowerRaytracingOperations(llvm::Module &, const RaytracingExportContext &)
 *   rewrites every ray tracing `dx.op` call of one already staged raytracing
 *   export into AIR intrinsics, in place, and erases the call. Everything the
 *   generic passes own (arithmetic, texture/buffer access, the stage I/O) is left
 *   untouched, so the pass order is
 *       LowerRaytracingStage -> LowerRaytracingOperations -> LowerResources
 *       -> LowerOperations -> VerifyLoweredModule.
 *
 * Contract provenance. Every `air.` name, every operand and every operand *value*
 * below was obtained by compiling the equivalent MSL with the public Metal
 * toolchain (`xcrun metal -std=metal3.1 -S -emit-llvm`) and reading the AIR it
 * emits; the probes and their AIR live under
 * build/owned-direct-20260919/probes/rt-compiler and /tmp/rtflags. Nothing here
 * comes from a vendor SDK.
 *
 *   Traversal (ray generation):
 *     { i32, float, i32, i32, i8*, i32, i32, <2 x float>, i1 }
 *     @air.intersect.instancing.triangle_data(
 *         <3 x float> origin, <3 x float> direction, float tmin, float tmax,
 *         AS* as, i32 instance_mask,
 *         IFT* ift, i8* payload, i64 payload_size,
 *         i32 triangle_front_facing_winding, i32 triangle_cull_mode,
 *         i32 geometry_cull_mode, i32 opacity_cull_mode, i32 forced_opacity,
 *         i32 geometry_type, i32 curve_basis, i32 curve_type,
 *         i32 curve_control_point_count,
 *         i1 assume_identity_transforms, i1 accept_any_intersection)
 *     -> { type, distance, primitive_id, geometry_id, primitive_data,
 *          instance_id, user_instance_id, barycentrics, front_facing }
 *
 *   Exactly twenty operands. The enumeration above is not a reading of the
 *   prototype: each operand was identified by compiling one MSL source per
 *   `intersection_params` / `intersector` setting and diffing the emitted call.
 *   The mapping the front end uses (and the values it uses for a default
 *   `intersector<triangle_data, instancing>`) is
 *
 *     winding               0 = clockwise (the front end's default), 1 = ccw
 *     triangle_cull_mode    0 = none, 1 = front, 2 = back
 *     geometry_cull_mode    0 = none, 1 = triangle, 2 = bounding_box, 3 = both
 *     opacity_cull_mode     0 = none, 1 = opaque, 2 = non_opaque
 *     forced_opacity        0 = none, 1 = opaque, 2 = non_opaque
 *     geometry_type         1 = triangle, 2 = bounding_box, 3 = both (default)
 *     curve_basis/type      -1 when the shader has no curve information
 *     curve_control_points  0
 *
 *   A miss / closest-hit / callable shader is reached through the visible function
 *   table:  %fp = call i8* @air.get_function_pointer_visible_function_table(vft, i32 slot)
 *           call void %fp(i8* payload, i8* hit state, i32 record, i32 depth)
 *   Public Metal has no [[miss]] / [[closest_hit]] / [[any_hit]] / [[callable]]
 *   attribute (the front end warns "unknown attribute" and lowers the function as
 *   an ordinary visible one), so all four are `air.visible` functions and the
 *   record index / depth are ordinary visible-function arguments. They are the
 *   arguments the *caller* computes for the *callee*, never the caller's own
 *   values, and the callee is skipped entirely when its record index is the
 *   runtime's invalid-record sentinel.
 *
 * D3D12 ray flags. TraceRay's second operand is the D3D12_RAY_FLAG dword and every
 * bit that a traversal can express is lowered onto the traversal operands above:
 *
 *   FORCE_OPAQUE / FORCE_NON_OPAQUE      -> forced_opacity        (bits 0..1)
 *   CULL_OPAQUE / CULL_NON_OPAQUE        -> opacity_cull_mode     (bits 6..7)
 *   CULL_BACK / CULL_FRONT_FACING_TRIANGLES -> triangle_cull_mode (bits 4..5)
 *   SKIP_TRIANGLES / SKIP_PROCEDURAL     -> geometry_cull_mode    (bits 8..9)
 *   SKIP_CLOSEST_HIT_SHADER              -> gates the visible-function continuation
 *
 * The pipeline's own SKIP_TRIANGLES / SKIP_PROCEDURAL_PRIMITIVES flags
 * (SM50_RT_PIPELINE_FLAG, the same bit positions) are folded into
 * geometry_cull_mode, so a pipeline that skips a primitive type skips it for every
 * trace, not only for the traces that ask for it.
 *
 * Four of those six are the flag bits shifted into place, which is why the values
 * are derived by shifting rather than by a chain of compares; only
 * triangle_cull_mode needs the front/back value swap (D3D12 puts *back* facing in
 * the lower bit, AIR puts *front* facing at 1). Constant flag operands are folded
 * at compile time, so a shader that passes a literal emits no arithmetic at all.
 *
 *   ACCEPT_FIRST_HIT_AND_END_SEARCH is deliberately NOT mapped onto
 *   accept_any_intersection. D3D12 runs the any-hit shader for every candidate
 *   even with that flag, and only the IgnoreHit exception lets the search
 *   continue; `accept_any_intersection` would instead let the traversal commit the
 *   first candidate *without* running the per-candidate hook at all. The flag is
 *   expressed by the per-candidate callback's `[[continue_search]]` result (see
 *   dxil_rt_callbacks.cpp), so operand 19 is pinned false and the D3D12 exception
 *   is decided where the any-hit shader actually runs.
 *
 *   FORCE_OMM_2_STATE (0x400) has no effect here and is not a gap: the pipeline
 *   never sets D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS, no
 *   geometry this runtime builds carries a micromap link, and the flag's only
 *   defined meaning is how an *encountered* micromap is interpreted.
 *
 * Recursion. D3D12 requires TraceRay to be rejected once the pipeline's
 * D3D12_RAYTRACING_PIPELINE_CONFIG.MaxTraceRecursionDepth would be exceeded (the
 * device is removed). A Metal shader cannot remove a device, so the bound is
 * enforced where the trace is emitted: the depth the traversal would run the next
 * shaders at is compared against the pipeline's configured limit, and a trace that
 * would exceed it is not issued at all. The limit is read from the dispatch
 * constants at run time (SM50_RT_DISPATCH_ARGUMENTS::RecursionDepth), so it
 * follows the pipeline actually being dispatched, and the compile-time
 * MaxRecursionDepth is used as the fallback and as the secondary bound. Metal's
 * own default call stack depth (one additional frame) is never what bounds this.
 *
 * Invocation blocks. One `TraceRay` or `CallShader` is one invocation of the
 * shaders it runs, and each of them gets a block of its own, allocated per call
 * site in the tracing function's entry block (see
 * RaytracingExportContext::invocationBlockSize for the layout: frame, user
 * payload, hit state, attributes, candidate control). The payload object the call
 * names is copied into the block before the traversal and copied back into that
 * object when the shaders have run, which is what gives D3D12's payload semantics:
 * the payload the hit shader wrote is what the tracing shader reads afterwards, two
 * payloads can be live at once, a loop's payload object survives the trace, a
 * nested trace owns its own block, and the tracing shader's own frame / hit state /
 * candidate control are not overwritten by the shaders it invokes. The ray
 * generation shader's own block is the one the stage builder allocated.
 *
 * Rejected explicitly, never mis-lowered:
 *   - a hit shader system value read outside a hit shader;
 *
 * The inline `RayQuery` family (178..215 except GeometryIndex, 258) is lowered in
 * every stage by LowerRayQueries (dxil_rt_query.cpp); this pass only resolves the
 * structure a library's `TraceRayInline` names, exactly as it does for TraceRay.
 *
 * Acceleration structures. `TraceRay`'s structure is the shader's own operand: the
 * handle `dx.op.createHandleForLib` built from its `RaytracingAccelerationStructure`
 * declaration. That declaration is an ordinary root-signature SRV whose descriptor
 * holds the structure's D3D12 GPU virtual address, so the pass resolves the
 * descriptor (root SRV argument or descriptor-table slot, register index and all)
 * and loads the Metal structure out of the 64 byte header the runtime writes at that
 * address; address zero is D3D12's null structure and becomes
 * `air.get_null_instance_acceleration_structure()`. There is no pipeline-wide
 * structure slot, so an export may name several structures and index one
 * dynamically.
 */

#include "dxil_converter.hpp"
#include "dxil_raytracing.h"
/*
 * The any-hit / intersection control operations are lowered by
 * dxil_rt_callbacks.cpp, which needs the same traversal context this pass builds.
 */
#include "dxil_rt_callbacks.hpp"

#include "nt/air_builder.hpp"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Alignment.h"

#include <algorithm>
#include <cstddef>
#include <string>

using namespace llvm;

namespace dxmt::dxil {

/*
 * dx.op opcode ids of the ray tracing block, from DXIL.rst and DXC's
 * DXIL/DxilOperations.h `enum class OpCode`. The numbers were cross-checked
 * against the library container DXC produces for
 * graphics-translator's raytracing fixture (the disassembly shows
 * dispatchRaysIndex=145, traceRay=157, createHandleForLib=160, ignoreHit=155).
 */
enum DxilRtOpcode : uint32_t {
  OpInstanceID = 141,
  OpInstanceIndex = 142,
  OpHitKind = 143,
  OpRayFlags = 144,
  OpDispatchRaysIndex = 145,
  OpDispatchRaysDimensions = 146,
  OpWorldRayOrigin = 147,
  OpWorldRayDirection = 148,
  OpObjectRayOrigin = 149,
  OpObjectRayDirection = 150,
  OpObjectToWorld = 151,
  OpWorldToObject = 152,
  OpRayTMin = 153,
  OpRayTCurrent = 154,
  OpIgnoreHit = 155,
  OpAcceptHitAndEndSearch = 156,
  OpTraceRay = 157,
  OpReportHit = 158,
  OpCallShader = 159,
  OpCreateHandleForLib = 160,
  OpPrimitiveIndex = 161,
  /** `dx.op.rayQuery_TraceRayInline`; lowered by LowerRayQueries (dxil_rt_query.cpp). */
  OpRayQueryTraceRayInline = 179,
  /**
   * `dx.op.annotateHandle`. Not a raytracing operation - the resource pass lowers
   * it - but a raytracing export's acceleration-structure handle is annotated like
   * any other resource handle, so the chain has to be walked to reach the
   * `dx.op.createHandleForLib` that produced it.
   */
  OpAnnotateHandle = 216,
  /**
   * The autogenerated index of the current geometry in the bottom-level
   * structure. It sits inside the RayQuery opcode block numerically but is a hit
   * shader system value, not a RayQuery method, so it is lowered here.
   */
  OpGeometryIndex = 213,
};

/**
 * D3D12_RAY_FLAGS bits the traversal operands are derived from. The full set is
 * listed in the file comment; only the bit this pass tests directly needs a name,
 * because the other five map onto their operand by a plain shift.
 */
enum RayFlag : uint32_t {
  kRayFlagSkipClosestHitShader = 0x8,
};

/** Values of the AIR traversal's configuration operands (see the file comment). */
enum TraversalMode : uint32_t {
  /** D3D12's default facing; a per-instance flag reverses it. */
  kWindingClockwise = 0,
  /** D3D12 lets the acceleration structure decide; both types are assumed. */
  kGeometryTypeAll = 3,
  /** The front end's value for a shader with no curve information. */
  kNoCurveBasis = 0xffffffffu,
};

/**
 * Byte offsets inside the constant buffer the runtime binds at
 * SM50_RT_BINDING_INDEX_DISPATCH.
 *
 * They are derived from SM50_RT_DISPATCH_ARGUMENTS itself rather than written
 * out, so the two can never disagree about a field's position: the layout of that
 * struct *is* the ABI, and a field added to it is addressable here without a
 * second edit.
 */
enum RtDispatchField : uint32_t {
  kRtRecursionDepth = offsetof(SM50_RT_DISPATCH_ARGUMENTS, RecursionDepth),
  kRtDispatchWidth = offsetof(SM50_RT_DISPATCH_ARGUMENTS, DispatchWidth),
  kRtDispatchHeight = offsetof(SM50_RT_DISPATCH_ARGUMENTS, DispatchHeight),
  kRtDispatchDepth = offsetof(SM50_RT_DISPATCH_ARGUMENTS, DispatchDepth),
};

/** A record index the runtime could not resolve to an export. */
constexpr uint32_t kRtRecordIndexNone = 0xffffffffu;

/** D3D12_HIT_KIND_*; what HitKind() reports for a fixed-function triangle hit. */
constexpr uint32_t kHitKindTriangleFrontFace = 0xfe;
constexpr uint32_t kHitKindTriangleBackFace = 0xff;

/** Metal's intersection_type, in the traversal result's first field. */
constexpr uint32_t kIntersectionTypeNone = 0;
constexpr uint32_t kIntersectionTypeTriangle = 1;

/**
 * Bits of SM50_RT_FRAME::Flags.
 *
 * D3D12 removes the device when TraceRay is called past the pipeline's maximum
 * recursion depth. A Metal shader cannot, so the trace is not issued and the
 * violation is recorded on the invocation's frame instead of being absorbed
 * silently; the value is also what the runtime reads to report the condition.
 */
enum RtFrameFlag : uint32_t {
  kRtFrameFlagRecursionExceeded = 0x1,
};

namespace {

bool getDxilOpcode(const CallInst &call, uint32_t &opcode) {
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

bool isRaytracingOpcode(uint32_t opcode) {
  if (opcode >= OpInstanceID && opcode <= OpPrimitiveIndex)
    return true;
  /* GeometryIndex shares the RayQuery numbering but is a hit shader system value. The
   * inline RayQuery family itself is lowered by LowerRayQueries, in every stage. */
  return opcode == OpGeometryIndex;
}

/* -------------------------------------------------------------------------- */
/* AIR types                                                                  */
/* -------------------------------------------------------------------------- */

StructType *namedStructType(LLVMContext &context, StringRef name) {
  StructType *existing = StructType::getTypeByName(context, name);
  return existing ? existing : StructType::create(context, name);
}

Type *accelerationStructureType(LLVMContext &context) {
  return namedStructType(context, "struct._instance_acceleration_structure_t")->getPointerTo(1);
}

Type *intersectionFunctionTableType(LLVMContext &context) {
  return namedStructType(context, "struct._intersection_function_table_t")->getPointerTo(1);
}


/**
 * Byte offsets inside SM50_RT_HIT_STATE (dxil_raytracing.h). The pass reads and
 * writes the block the stage builder allocated, and the two have to agree.
 */
enum RtHitStateField : uint32_t {
  kRtHitWorldOrigin = offsetof(SM50_RT_HIT_STATE, WorldRayOrigin),
  kRtHitWorldDirection = offsetof(SM50_RT_HIT_STATE, WorldRayDirection),
  kRtHitTMin = offsetof(SM50_RT_HIT_STATE, RayTMin),
  kRtHitTCurrent = offsetof(SM50_RT_HIT_STATE, RayTCurrent),
  kRtHitPrimitiveIndex = offsetof(SM50_RT_HIT_STATE, PrimitiveIndex),
  kRtHitInstanceID = offsetof(SM50_RT_HIT_STATE, InstanceID),
  kRtHitInstanceIndex = offsetof(SM50_RT_HIT_STATE, InstanceIndex),
  kRtHitGeometryIndex = offsetof(SM50_RT_HIT_STATE, GeometryIndex),
  kRtHitKind = offsetof(SM50_RT_HIT_STATE, HitKind),
  kRtHitRayFlags = offsetof(SM50_RT_HIT_STATE, RayFlags),
  kRtHitBarycentric = offsetof(SM50_RT_HIT_STATE, Barycentrics),
  kRtHitFrontFace = offsetof(SM50_RT_HIT_STATE, FrontFace),
  kRtHitObjectToWorld = offsetof(SM50_RT_HIT_STATE, ObjectToWorld),
  kRtHitWorldToObject = offsetof(SM50_RT_HIT_STATE, WorldToObject),
  kRtHitObjectOrigin = offsetof(SM50_RT_HIT_STATE, ObjectRayOrigin),
  kRtHitObjectDirection = offsetof(SM50_RT_HIT_STATE, ObjectRayDirection),
};

/**
 * Result of `air.intersect.instancing.triangle_data.world_space_data`, in the exact field
 * order the public Metal front end emits: the nine base fields, then the instance's
 * world-to-object transform as four <3 x float> columns (fields 9..12) and its
 * object-to-world transform (fields 13..16). Read back from
 * `intersector<triangle_data, instancing, world_space_data>` (build/local/p3/dxr/probes/xform.ll:
 * `res.object_to_world_transform` extracts field 13, `res.world_to_object_transform` field 9).
 */
StructType *intersectionResultType(LLVMContext &context) {
  StructType *type = namedStructType(context, "struct.dxmt.rt.intersection_result");
  if (!type->isOpaque())
    return type;
  Type *f32 = Type::getFloatTy(context);
  Type *u32 = Type::getInt32Ty(context);
  type->setBody(
    {
      u32,                                 /* type */
      f32,                                 /* distance */
      u32,                                 /* primitive_id */
      u32,                                 /* geometry_id */
      Type::getInt8PtrTy(context, 1),      /* primitive_data */
      u32,                                 /* instance_id */
      u32,                                 /* user_instance_id */
      FixedVectorType::get(f32, 2),        /* barycentrics */
      Type::getInt1Ty(context),            /* front_facing */
      FixedVectorType::get(f32, 3),        /* world_to_object column 0 */
      FixedVectorType::get(f32, 3),        /* column 1 */
      FixedVectorType::get(f32, 3),        /* column 2 */
      FixedVectorType::get(f32, 3),        /* column 3 (translation) */
      FixedVectorType::get(f32, 3),        /* object_to_world column 0 */
      FixedVectorType::get(f32, 3),        /* column 1 */
      FixedVectorType::get(f32, 3),        /* column 2 */
      FixedVectorType::get(f32, 3),        /* column 3 (translation) */
    },
    false
  );
  return type;
}

FunctionCallee declareRtIntrinsic(
  Module &module, StringRef name, FunctionType *type, bool convergent
) {
  LLVMContext &context = module.getContext();
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
          {{~0U, Attribute::get(context, Attribute::ReadOnly)},
           {~0U, Attribute::get(context, Attribute::NoUnwind)},
           {~0U, Attribute::get(context, Attribute::WillReturn)}}
        );
  return module.getOrInsertFunction(name, type, attributes);
}

/* -------------------------------------------------------------------------- */
/* Ray flag lowering                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Read a two bit field of the D3D12 ray flags. Four of the six traversal
 * configuration operands are exactly a flag pair shifted into place, so no
 * compares are needed; a constant operand folds to a constant.
 */
Value *flagField(IRBuilder<> &builder, Value *flags, uint32_t shift) {
  if (auto *constant = dyn_cast<ConstantInt>(flags))
    return builder.getInt32((constant->getZExtValue() >> shift) & 0x3u);
  Value *shifted = builder.CreateLShr(flags, builder.getInt32(shift));
  return builder.CreateAnd(shifted, builder.getInt32(0x3u));
}

/**
 * D3D12 puts back-facing culling in bit 4 and front-facing in bit 5, while the AIR
 * triangle_cull_mode names front facing at 1 and back facing at 2, so the pair is
 * swapped rather than shifted.
 */
Value *triangleCullMode(IRBuilder<> &builder, Value *flags) {
  if (auto *constant = dyn_cast<ConstantInt>(flags)) {
    uint64_t bits = constant->getZExtValue();
    return builder.getInt32(((bits >> 4) & 1u) * 2u + ((bits >> 5) & 1u));
  }
  Value *back = builder.CreateAnd(builder.CreateLShr(flags, builder.getInt32(4)), builder.getInt32(1));
  Value *front = builder.CreateAnd(builder.CreateLShr(flags, builder.getInt32(5)), builder.getInt32(1));
  return builder.CreateOr(builder.CreateShl(back, builder.getInt32(1)), front);
}

/** True when `flags` is a constant that does / does not contain `bit`. */
bool constantFlag(Value *flags, uint32_t bit, bool &set) {
  auto *constant = dyn_cast<ConstantInt>(flags);
  if (!constant)
    return false;
  set = (constant->getZExtValue() & bit) != 0;
  return true;
}

/** The i8* form of `value` in its own address space. */
Value *bytePointerOf(IRBuilder<> &builder, Value *value) {
  LLVMContext &llvm_context = value->getContext();
  unsigned address_space = cast<PointerType>(value->getType())->getAddressSpace();
  Type *i8ptr = Type::getInt8PtrTy(llvm_context, address_space);
  return value->getType() == i8ptr ? value : builder.CreateBitCast(value, i8ptr);
}

/**
 * Emit `llvm.memcpy` between two pointers, which may live in different address
 * spaces (a payload object is in the thread space, the block it is copied into is
 * too, but the intersection stage's payload arrives in Metal's ray-data space).
 */
void emitMemcpy(Module &module, IRBuilder<> &builder, Value *dest, Value *src, uint64_t size) {
  LLVMContext &llvm_context = module.getContext();
  Value *dest_bytes = bytePointerOf(builder, dest);
  Value *src_bytes = bytePointerOf(builder, src);
  std::string name = "llvm.memcpy.p" +
                     std::to_string(cast<PointerType>(dest_bytes->getType())->getAddressSpace()) +
                     "i8.p" +
                     std::to_string(cast<PointerType>(src_bytes->getType())->getAddressSpace()) +
                     "i8.i64";
  FunctionCallee memcpy = module.getOrInsertFunction(
    name,
    FunctionType::get(
      Type::getVoidTy(llvm_context),
      {dest_bytes->getType(), src_bytes->getType(), builder.getInt64Ty(),
       Type::getInt1Ty(llvm_context)},
      false
    )
  );
  builder.CreateCall(
    memcpy, {dest_bytes, src_bytes, builder.getInt64(size), builder.getInt1(false)}
  );
}

/* -------------------------------------------------------------------------- */
/* The pass                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Resolved shape of the lowered shader.
 *
 * LowerRaytracingStage built the AIR function and registered it under
 * `air.kernel` / `air.visible` / `air.intersection`; this pass recovers the
 * arguments from that signature by their AIR metadata kind (and, where the kind is
 * shared, by their `air.arg_name`), so it never guesses an index.
 */
using RaytracingEntry = RaytracingEntryArguments;

/**
 * Run `body` in a block that is entered only when `condition` holds, continuing in
 * a join block afterwards. A condition that folds to true emits nothing; one that
 * folds to false emits nothing either, so the guarded code never runs.
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
  BasicBlock *head = builder.GetInsertBlock();
  Function *function = head->getParent();
  BasicBlock *join_block;
  if (builder.GetInsertPoint() != head->end()) {
    join_block = head->splitBasicBlock(builder.GetInsertPoint(), name + ".join");
    head->getTerminator()->eraseFromParent();
  } else {
    join_block = BasicBlock::Create(builder.getContext(), name + ".join", function);
  }
  BasicBlock *body_block = BasicBlock::Create(builder.getContext(), name, function, join_block);
  builder.SetInsertPoint(head);
  builder.CreateCondBr(condition, body_block, join_block);
  builder.SetInsertPoint(body_block);
  body(builder);
  builder.CreateBr(join_block);
  builder.SetInsertPoint(join_block, join_block->getFirstInsertionPt());
}

/**
 * Evaluate `body` in a block entered only when `condition` holds, and continue with
 * the merged value in a block that also carries everything that followed the
 * insertion point.
 *
 * The block is split at the insertion point first, so the instructions that come
 * after it stay reachable: the condition is tested where the value is needed, the
 * guarded block computes it, and the continuation - the rest of the original block -
 * receives either value. This is what allows a guard in the middle of a shader
 * without moving the shader's own instructions around.
 *
 * A select cannot be used instead wherever the guarded expression must not be
 * evaluated at all (a load through a null base faults in address space 1).
 */
Value *emitGuardedValue(
  IRBuilder<> &builder, Value *condition, Value *otherwise, StringRef name,
  function_ref<Value *(IRBuilder<> &)> body
) {
  if (auto *constant = dyn_cast<ConstantInt>(condition))
    return constant->isOne() ? body(builder) : otherwise;

  LLVMContext &llvm_context = builder.getContext();
  Function *function = builder.GetInsertBlock()->getParent();
  BasicBlock *head = builder.GetInsertBlock();
  /*
   * Splitting keeps the instructions after the insertion point reachable. When the
   * insertion point is already the end of the block there is nothing to keep, so the
   * continuation is simply a new block.
   *
   * The terminator is erased inside the split case only, because that is the only case
   * that has one. `splitBasicBlock` requires a terminator and appends a branch from
   * `head` to the new block, and that branch is exactly what the conditional below
   * replaces. A block whose insertion point is its end has no terminator - the normal
   * state while a shader body is still being built - so there is nothing to erase
   * there, and `head` still gets the conditional branch below as its only terminator.
   */
  BasicBlock *continuation = nullptr;
  if (builder.GetInsertPoint() != head->end()) {
    continuation = head->splitBasicBlock(builder.GetInsertPoint(), name + ".cont");
    head->getTerminator()->eraseFromParent();
  } else {
    continuation = BasicBlock::Create(llvm_context, name + ".cont", function);
  }

  BasicBlock *guarded = BasicBlock::Create(llvm_context, name, function, continuation);
  builder.SetInsertPoint(head);
  builder.CreateCondBr(condition, guarded, continuation);
  builder.SetInsertPoint(guarded);
  Value *value = body(builder);
  BasicBlock *guarded_end = builder.GetInsertBlock();
  builder.CreateBr(continuation);

  builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
  PHINode *merged = builder.CreatePHI(otherwise->getType(), 2, name + ".value");
  merged->addIncoming(otherwise, head);
  merged->addIncoming(value, guarded_end);
  return merged;
}

class RaytracingLowerer {
public:
  RaytracingLowerer(
    Module &module, const RaytracingExportContext &context, const RaytracingEntry &entry,
    ArrayRef<ResourceBinding> resources
  )
      : module(module), context(context), entry(entry), resources(resources),
        /*
         * The block layout is not a second copy of the sizes: `rtInvocationLayout`
         * (dxil_rt_callbacks.hpp) is the one authority the stage builder, this pass and
         * the callback layer all use, so a region can never be addressed at one offset
         * here and another there.
         */
        layout(rtInvocationLayout(context)) {}

  Error run();

private:
  Module &module;
  const RaytracingExportContext &context;
  const RaytracingEntry &entry;
  /** `!dx.resources` declarations, used to tell an acceleration structure handle
   * from an ordinary resource handle. */
  ArrayRef<ResourceBinding> resources;
  /** Sink for the AIR builder's diagnostics; the pass reports through llvm::Error. */
  raw_null_ostream nulldebug;
  /**
   * The per-invocation block layout, from `rtInvocationLayout` (dxil_rt_callbacks.hpp),
   * which is the single authority for the offsets: the stage builder sizes the ray
   * generation shader's block with it, this pass allocates the per-callsite blocks
   * with it, and the callback layer addresses every region from the block base.
   */
  RtInvocationLayout layout;
  /** i8* per-invocation block of the shader being lowered (the entry function's). */
  Value *entry_block = nullptr;
  /**
   * The instance-contribution array address of each resolved acceleration structure,
   * keyed by the structure operand the shader holds.
   *
   * The header's second qword is that address (`AccelerationStructureHeader::
   * contributionsAddress`), and it is read where the structure is resolved - the
   * traversal itself only carries the Metal resource id, so the address cannot be
   * recovered from the operand later.
   */
  DenseMap<Value *, Value *> contributions_addresses;

  /** Byte offset of SM50_RT_HIT_STATE inside a per-invocation block. */
  uint32_t hitStateOffset() const { return layout.hit_state; }
  uint32_t candidateOffset() const { return layout.candidate_control; }
  uint32_t blockSize() const { return layout.block_size; }

  Value *loadDispatchField(IRBuilder<> &builder, uint32_t byte_offset, Type *type);
  /** The dispatch constants as an i8 pointer, or null when this invocation cannot reach them. */
  Value *dispatchBytes(IRBuilder<> &builder);
  /**
   * Resolve record `index` of `table` (EmitShaderRecordLookup). False when the
   * dispatch constants are not reachable.
   */
  bool shaderRecord(IRBuilder<> &builder, RtShaderTable table, Value *index, RtShaderRecord &record);
  /** Load one field of a block's SM50_RT_FRAME. */
  Value *loadFrameField(
    IRBuilder<> &builder, Value *block, uint32_t byte_offset, Type *type
  );
  /** Store one field of a block's SM50_RT_FRAME. */
  void storeFrameField(
    IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value
  );
  /** The D3D12 GPU virtual address of the dispatch constants (see the definition). */
  Value *dispatchArgumentsAddress(IRBuilder<> &builder);
  /**
   * The two function tables, as *this* invocation reaches them.
   *
   * A stage that declares them (the ray generation shader, and the intersection
   * shader for the visible function table) reads the argument; every other stage
   * takes the address out of its invocation frame, because the traversal's call into
   * an intersection function and the call through the visible function table bind
   * the tables the table itself declares - not the encoder's. Returns null when
   * neither exists, which the callers report rather than substituting anything.
   */
  Value *intersectionFunctionTable(IRBuilder<> &builder);
  Value *visibleFunctionTable(IRBuilder<> &builder);
  /**
   * Publish this invocation's runtime context into a block allocated for a callee:
   * the dispatch constants' address and the pipeline's four bound entries (see
   * SM50_RT_FRAME). Every block of an invocation is filled in by whoever allocates
   * it, so a callee that cannot take them as arguments still reaches them.
   */
  void publishRuntimeContext(IRBuilder<> &builder, Value *block);
  /**
   * Read one field of the hit state of the invocation a block belongs to.
   *
   * The block is passed explicitly rather than taken from the entry, because a
   * `TraceRay` publishes the ray it is about to trace into the block it allocates
   * for the shaders it runs, not into the caller's own.
   */
  Value *readHitState(IRBuilder<> &builder, Value *block, uint32_t byte_offset, Type *type);
  void storeHitState(IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value);
  /** Address one field of a hit state block by byte offset. */
  Value *hitStateFieldPointer(
    IRBuilder<> &builder, Value *hit_state, uint32_t byte_offset, Type *type
  );
  /** Write one word of a block's SM50_RT_FRAME. */
  void storeFrameWord(IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value);
  /** Write one dword of a block's per-candidate control area (RtCandidateField). */
  void storeCandidateWord(IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value);
  /**
   * Write one field of a block's per-candidate control area with the value's own
   * type, for the fields that are not a dword (RtCandidateControl::TMin / TMax are
   * floats): a store has to name a pointer to the stored type, so those fields
   * cannot go through the dword helper.
   */
  void storeCandidateField(
    IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value
  );
  /**
   * Read one dword of a block's per-candidate control area, or null when this layout
   * has no room for it.
   */
  Value *loadCandidateWord(IRBuilder<> &builder, Value *block, uint32_t byte_offset);
  /**
   * Call one export through the visible function table with the given invocation
   * block: its payload area, hit state, candidate control and attribute region all
   * come from that block. The call itself is emitted by the callback layer's shared
   * helper, so the visible-function ABI has one implementation.
   */
  Value *callVisibleFunction(
    IRBuilder<> &builder, Value *block, Value *record_index, Value *depth
  );
  /**
   * Allocate a per-invocation block of this export's layout at the current insertion
   * point and return its base as an i8*.
   */
  Value *allocateInvocationBlock(IRBuilder<> &builder, StringRef name);
  /** i8* user payload area of `block`, passed to visible shaders. */
  Value *payloadPointerOf(IRBuilder<> &builder, Value *block);
  /** i8* hit state of `block`. */
  Value *hitStatePointerOf(IRBuilder<> &builder, Value *block);
  /** i8* per-candidate control area of `block`. */
  Value *candidatePointerOf(IRBuilder<> &builder, Value *block);
  /** The block the shader being lowered runs with (see the definition). */
  Value *invocationBlockOf(IRBuilder<> &builder);
  /** The acceleration-structure declaration a `dx.op.createHandleForLib` names. */
  const ResourceBinding *accelerationStructureOf(const CallInst &call) const;
  /**
   * Resolve a RaytracingAccelerationStructure declaration to the D3D12 GPU virtual
   * address its descriptor holds, through the export's binding map. The address is
   * an i64: the descriptor-table slot's first qword, or the root SRV argument
   * itself. Null when the root signature does not bind the declaration.
   */
  Value *accelerationStructureAddress(const ResourceBinding &declaration, IRBuilder<> &builder);
  /**
   * Turn a D3D12 GPU virtual address of an acceleration structure into the AIR
   * structure operand.
   *
   * The structure's own 64 byte header starts at that address and its first qword
   * is the Metal resource id, which is exactly what the traversal wants; address
   * zero is D3D12's null structure, and AIR has a dedicated null structure for it
   * (binding a zero id instead would be an invalid resource id, not "no
   * structure").
   */
  Value *loadAccelerationStructure(Value *gpu_address, IRBuilder<> &builder);
  /**
   * The application's `InstanceContributionToHitGroupIndex` for the instance a trace
   * hit, read out of the invocation frame (see the definition).
   */
  Value *loadInstanceContribution(
    IRBuilder<> &builder, Value *block, Value *instance_index
  );
  /** The instance-contribution array address of a resolved structure, or null. */
  Value *contributionsArrayAddress(IRBuilder<> &builder, Value *structure);
  /**
   * The D3D12 GPU virtual address a resolved structure operand was loaded from, or
   * null when the operand does not trace back to a `loadAccelerationStructure`.
   */
  Value *accelerationStructureGpuAddress(Value *structure) const;
  /**
   * Copy the payload object a trace names into a child invocation block.
   *
   * The operand is the shader's own payload object (an alloca, or the payload
   * argument of a visible function); its contents are copied into the child block's
   * payload area, which is what the shaders this trace runs will read and write.
   * The caller's object is left alone, so two payloads can be live at once, a loop
   * reuses the object after the trace returns, and a recursive invocation keeps its
   * own. Returns the child's payload area as the address the shaders see, or null
   * when the operand cannot be copied from.
   */
  Value *copyPayloadInto(IRBuilder<> &builder, Value *child_block, Value *payload_operand);
  /** Copy a child block's payload area back to the object a trace named. */
  void copyPayloadBack(IRBuilder<> &builder, Value *child_block, Value *payload_operand);

  Error lowerTraceRay(CallInst &call, IRBuilder<> &builder);
  Value *emitQueryTraversal(IRBuilder<> &builder, Value *child, Value *structure, Value *origin, Value *direction,
                            Value *t_min, Value *t_max, Value *flags, Value *mask, Value *triangle_cull,
                            Value *geometry_cull, Value *opacity_cull, Value *forced_opacity);
  Error lowerCallShader(CallInst &call, IRBuilder<> &builder);
  Error lowerDispatchRaysIndex(CallInst &call, IRBuilder<> &builder);
  Value *linearDispatchIndex(IRBuilder<> &builder);
  Error lowerDispatchRaysDimensions(CallInst &call, IRBuilder<> &builder);
  Error lowerHitStateRead(CallInst &call, uint32_t opcode, IRBuilder<> &builder);
  Error lowerCreateHandleForLib(CallInst &call, IRBuilder<> &builder);
  Error lowerCall(CallInst &call, uint32_t opcode, IRBuilder<> &builder);
};

/**
 * The acceleration structure of a `RaytracingAccelerationStructure` declaration.
 *
 * The declaration is an ordinary root-signature SRV, so its descriptor is resolved
 * exactly like any other resource: the binding map knows whether it is a root SRV
 * argument (whose value *is* the D3D12 GPU virtual address) or a descriptor-table
 * slot (whose first qword holds it), and the register index stays dynamic. This is
 * what keeps a shader with several structures, or with a dynamically indexed one,
 * tracing against the structure it actually named.
 */
Value *RaytracingLowerer::accelerationStructureAddress(
  const ResourceBinding &declaration, IRBuilder<> &builder
) {
  if (!context.binding)
    return nullptr;
  llvm::air::AIRBuilder air{llvm::air::AIRBuilderOptions{}, builder, nulldebug};
  auto descriptor = context.binding->GetAccelerationStructure(
    air, declaration.declaration_id, builder.getInt32(declaration.lower_bound)
  );
  if (!descriptor)
    return nullptr;
  return descriptor->GpuVA;
}

/**
 * Load the Metal structure out of a D3D12 GPU virtual address.
 *
 * Every structure the runtime builds starts with the 64 byte header described by
 * `AccelerationStructureHeader` in the runtime's d3d12_acceleration_structure.hpp,
 * and its first qword is the Metal resource id the build published. Address zero is
 * D3D12's null acceleration structure, which is not a resource id at all: AIR
 * exposes `air.get_null_instance_acceleration_structure()` for exactly that case,
 * and it is used instead so the trace misses rather than reading an invalid id.
 */
Value *RaytracingLowerer::loadAccelerationStructure(Value *gpu_address, IRBuilder<> &builder) {
  LLVMContext &llvm_context = module.getContext();
  Type *i64 = builder.getInt64Ty();
  Type *as_type = accelerationStructureType(llvm_context);
  FunctionCallee null_structure = declareRtIntrinsic(
    module, "air.get_null_instance_acceleration_structure",
    FunctionType::get(as_type, {}, false), /*convergent=*/false
  );
  Value *nothing = builder.CreateCall(null_structure, {});
  Value *bound_address = builder.CreateICmpNE(gpu_address, builder.getInt64(0));
  Value *structure = emitGuardedValue(
    builder, bound_address, nothing, "rt.as", [&](IRBuilder<> &b) -> Value * {
      Value *header = b.CreateIntToPtr(gpu_address, i64->getPointerTo(1));
      Value *resource_id = b.CreateLoad(i64, header);
      return b.CreateIntToPtr(resource_id, as_type);
    }
  );
  /*
   * The header the structure was loaded from is also where its instance-contribution
   * array address lives (qword 1), and the traversal operand only carries the Metal
   * resource id, so the D3D12 address is remembered here for the hit-group selection
   * of every trace that uses this structure.
   */
  contributions_addresses[structure] = gpu_address;
  return structure;
}

/**
 * The instance-contribution array address of a resolved acceleration structure.
 *
 * `structure` is the traversal operand the trace carries; the D3D12 address of the
 * structure's own header was recorded when the structure was resolved
 * (`loadAccelerationStructure` reads that header anyway for the resource id),
 * because the traversal only carries the Metal resource id and the address cannot
 * be recovered from it. The array address is the header's second qword
 * (`AccelerationStructureHeader::contributionsAddress`, written by the runtime's
 * AS prepass), and zero when the structure has no such array - which is what the
 * runtime publishes for a structure that contributes nothing.
 *
 * A null structure has no header at all, so that read is guarded by real control
 * flow: the null case answers zero without ever loading through address zero.
 */
Value *RaytracingLowerer::contributionsArrayAddress(IRBuilder<> &builder, Value *structure) {
  Type *i64 = builder.getInt64Ty();
  Value *address = accelerationStructureGpuAddress(structure);
  if (!address)
    return nullptr;
  return emitGuardedValue(
    builder, builder.CreateICmpNE(address, builder.getInt64(0)), builder.getInt64(0),
    "rt.contributions", [&](IRBuilder<> &b) -> Value * {
      return b.CreateLoad(
        i64, b.CreateGEP(
               i64, b.CreateIntToPtr(address, i64->getPointerTo(1)), b.getInt32(1)
             )
      );
    }
  );
}

Value *RaytracingLowerer::accelerationStructureGpuAddress(Value *structure) const {
  /*
   * The trace holds the structure through the DXIL handle chain (an extractvalue of
   * the handle's insertvalue, then a bitcast), so the address recorded when the
   * structure was resolved is found by walking that chain back to it.
   */
  Value *candidate = structure;
  Value *address = nullptr;
  while (candidate) {
    auto found = contributions_addresses.find(candidate);
    if (found != contributions_addresses.end()) {
      address = found->second;
      break;
    }
    if (auto *cast = dyn_cast<CastInst>(candidate))
      candidate = cast->getOperand(0);
    else if (auto *extract = dyn_cast<ExtractValueInst>(candidate))
      candidate = extract->getAggregateOperand();
    else if (auto *insert = dyn_cast<InsertValueInst>(candidate))
      /* The structure is the *inserted* value of the handle, not the aggregate. */
      candidate = insert->getOperand(1);
    else
      candidate = nullptr;
  }
  return address;
}

/**
 * The application's `InstanceContributionToHitGroupIndex` for one instance, out of
 * the invocation frame.
 *
 * The frame's SM50_RT_FRAME::ContributionArray is the address of the `uint32_t`
 * array the runtime's AS prepass filled for the structure the active trace is
 * tracing, indexed by the instance's position in that structure - which is the
 * traversal result's instance id. D3D12 adds the word to the hit group index
 * (`InstanceContributionToHitGroupIndex + RayContributionToHitGroupIndex +
 * GeometryIndex * MultiplierForGeometryContributionToHitGroupIndex`), and Metal has
 * no per-instance field for it, so it is read from here - by the tracing shader for
 * the hit it committed, by the shared candidate callback for a candidate, and by a
 * procedural intersection shader for the candidate it reports.
 *
 * Zero when the structure has no such array, so a non-instanced structure adds
 * nothing. The address is a device address, so the load is issued only when it is
 * non-zero (address space 1 has no address zero) and the guard is real control
 * flow rather than a select, which would evaluate the load.
 */
Value *RaytracingLowerer::loadInstanceContribution(
  IRBuilder<> &builder, Value *block, Value *instance_index
) {
  Type *i64 = builder.getInt64Ty();
  Type *u32 = builder.getInt32Ty();
  Value *array = loadFrameField(
    builder, block, offsetof(SM50_RT_FRAME, ContributionArray), i64
  );
  return emitGuardedValue(
    builder, builder.CreateICmpNE(array, builder.getInt64(0)), builder.getInt32(0),
    "rt.contribution", [&](IRBuilder<> &b) -> Value * {
      Value *words = b.CreateIntToPtr(array, u32->getPointerTo(1));
      return b.CreateLoad(u32, b.CreateGEP(u32, words, instance_index));
    }
  );
}

Value *RaytracingLowerer::dispatchBytes(IRBuilder<> &builder) {
  /* See loadDispatchField: the bound argument when this stage declares it, else the frame's address. */
  LLVMContext &llvm_context = module.getContext();
  if (entry.dispatch_arguments) {
    auto *block = dyn_cast<PointerType>(entry.dispatch_arguments->getType());
    if (!block)
      return nullptr;
    return builder.CreateBitCast(
      entry.dispatch_arguments, Type::getInt8PtrTy(llvm_context, block->getAddressSpace())
    );
  }
  Value *address = dispatchArgumentsAddress(builder);
  if (!address)
    return nullptr;
  return builder.CreateIntToPtr(address, Type::getInt8PtrTy(llvm_context, 1));
}

Value *RaytracingLowerer::loadDispatchField(
  IRBuilder<> &builder, uint32_t byte_offset, Type *type
) {
  /*
   * The dispatch block is a plain constant buffer the runtime binds at its own
   * argument index (SM50_RT_DISPATCH_ARGUMENTS in dxil_raytracing.h); the AIR type is
   * a dword array, so the field is addressed by byte, which is also what keeps the
   * access independent of how the argument was declared.
   *
   * A stage that does not declare it (an intersection function, or a visible function
   * reached through the function tables, whose argument list is fixed) reads the same
   * buffer through the address its invocation frame carries - the address the runtime
   * published into the block itself, or the one the party that issued the trace
   * published. Both name the same constants, so the field is read from whichever this
   * invocation has.
   */
  Value *bytes = dispatchBytes(builder);
  if (!bytes)
    return nullptr;
  unsigned address_space = cast<PointerType>(bytes->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(builder.getInt8Ty(), bytes, builder.getInt32(byte_offset));
  return builder.CreateLoad(
    type, builder.CreateBitCast(field, type->getPointerTo(address_space))
  );
}

bool RaytracingLowerer::shaderRecord(
  IRBuilder<> &builder, RtShaderTable table, Value *index, RtShaderRecord &record
) {
  /*
   * The record is resolved on the GPU straight out of the application's table: its 32 byte
   * identifier names the export (or none, for a record of another pipeline), and its local
   * root arguments are the bytes that follow it, read in place.
   */
  Value *bytes = dispatchBytes(builder);
  if (!bytes)
    return false;
  record = EmitShaderRecordLookup(builder, bytes, table, index);
  return true;
}

Value *RaytracingLowerer::readHitState(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset, Type *type
) {
  Value *base = hitStatePointerOf(builder, block);
  return builder.CreateAlignedLoad(type, hitStateFieldPointer(builder, base, byte_offset, type), Align(1));
}

void RaytracingLowerer::storeHitState(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value
) {
  Value *base = hitStatePointerOf(builder, block);
  builder.CreateAlignedStore(
    value, hitStateFieldPointer(builder, base, byte_offset, value->getType()), Align(1)
  );
}

/**
 * Address one field of a hit state by byte offset.
 *
 * The block is addressed as bytes and cast to the field's own type: SM50_RT_HIT_STATE
 * mixes scalars and vectors, so a struct-field index would only line up with the byte
 * offsets by accident (and would load a vector out of a scalar field).
 */
Value *RaytracingLowerer::hitStateFieldPointer(
  IRBuilder<> &builder, Value *hit_state, uint32_t byte_offset, Type *type
) {
  LLVMContext &llvm_context = module.getContext();
  Value *bytes = bytePointerOf(builder, hit_state);
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(llvm_context), bytes, builder.getInt32(byte_offset)
  );
  unsigned address_space = cast<PointerType>(bytes->getType())->getAddressSpace();
  return builder.CreateBitCast(field, type->getPointerTo(address_space));
}

/**
 * Write one field of a block's per-candidate control area, with the value's own
 * type.
 *
 * The control area mixes dwords and floats (RtCandidateControl), so a store into it
 * has to name a pointer to the stored type - the dword helper below can only serve
 * the integer fields. No room for the control block in this layout means the callback
 * layer is not part of this pipeline, so there is nothing to publish.
 */
void RaytracingLowerer::storeCandidateField(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value
) {
  if (layout.candidate_control + kRtCandidateControlSize > layout.block_size)
    return;
  Value *base = candidatePointerOf(builder, block);
  unsigned address_space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(module.getContext()), bytePointerOf(builder, base),
    builder.getInt32(byte_offset)
  );
  builder.CreateAlignedStore(
    value, builder.CreateBitCast(field, value->getType()->getPointerTo(address_space)), Align(1)
  );
}

/**
 * Read one dword of a block's per-candidate control area.
 *
 * The mirror of `storeCandidateWord`, for the fields a tracing shader reads back
 * after the traversal: the committed hit's kind, which a procedural hit's
 * `dx.op.reportHit` recorded while the traversal was still running. Null when this
 * layout has no room for the control block, which the caller reports rather than
 * reading something else.
 */
Value *RaytracingLowerer::loadCandidateWord(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset
) {
  if (layout.candidate_control + kRtCandidateControlSize > layout.block_size)
    return nullptr;
  Value *base = candidatePointerOf(builder, block);
  unsigned address_space = cast<PointerType>(base->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(module.getContext()), bytePointerOf(builder, base),
    builder.getInt32(byte_offset)
  );
  return builder.CreateAlignedLoad(
    builder.getInt32Ty(),
    builder.CreateBitCast(field, builder.getInt32Ty()->getPointerTo(address_space)), Align(1)
  );
}

/**
 * Write one dword of a block's per-candidate control area.
 *
 * The area is part of every invocation block at the same offset, so a callback the
 * traversal invokes can reach it from the payload pointer it was handed. The layout
 * is `RtCandidateControl` (dxil_rt_callbacks.hpp), which the callback layer shares.
 */
void RaytracingLowerer::storeCandidateWord(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value
) {
  /*
   * No room for the control block in this layout means the callback layer is not
   * part of this pipeline, so there is nothing to publish.
   */
  if (layout.candidate_control + kRtCandidateControlSize > layout.block_size)
    return;
  LLVMContext &llvm_context = module.getContext();
  Value *base = candidatePointerOf(builder, block);
  Value *words = builder.CreateBitCast(
    base, Type::getInt32PtrTy(
            llvm_context, cast<PointerType>(base->getType())->getAddressSpace()
          )
  );
  builder.CreateAlignedStore(
    value,
    builder.CreateGEP(
      Type::getInt32Ty(llvm_context), words, builder.getInt32(byte_offset / 4)
    ), Align(1)
  );
}

/** Write one word of a block's SM50_RT_FRAME. */
void RaytracingLowerer::storeFrameWord(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value
) {
  LLVMContext &llvm_context = module.getContext();
  Value *bytes = bytePointerOf(builder, block);
  Value *as_words = builder.CreateBitCast(
    bytes, Type::getInt32PtrTy(
             llvm_context, cast<PointerType>(bytes->getType())->getAddressSpace()
           )
  );
  builder.CreateStore(
    value,
    builder.CreateGEP(
      Type::getInt32Ty(llvm_context), as_words, builder.getInt32(byte_offset / 4)
    )
  );
}

/** Read one field of a block's SM50_RT_FRAME, addressed by byte offset. */
Value *RaytracingLowerer::loadFrameField(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset, Type *type
) {
  LLVMContext &llvm_context = module.getContext();
  Value *bytes = bytePointerOf(builder, block);
  unsigned address_space = cast<PointerType>(bytes->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(llvm_context), bytes, builder.getInt32(byte_offset)
  );
  return builder.CreateLoad(type, builder.CreateBitCast(field, type->getPointerTo(address_space)));
}

/** Write one field of a block's SM50_RT_FRAME, addressed by byte offset. */
void RaytracingLowerer::storeFrameField(
  IRBuilder<> &builder, Value *block, uint32_t byte_offset, Value *value
) {
  LLVMContext &llvm_context = module.getContext();
  Value *bytes = bytePointerOf(builder, block);
  unsigned address_space = cast<PointerType>(bytes->getType())->getAddressSpace();
  Value *field = builder.CreateGEP(
    Type::getInt8Ty(llvm_context), bytes, builder.getInt32(byte_offset)
  );
  builder.CreateStore(
    value, builder.CreateBitCast(field, value->getType()->getPointerTo(address_space))
  );
}

/**
 * The D3D12 GPU virtual address of the dispatch constants, as *this* invocation
 * knows it.
 *
 * The ray generation shader, the intersection shader and the shared candidate
 * callback are handed the constants as a bound argument (slot
 * SM50_RT_BINDING_INDEX_DISPATCH), so their address is the argument's own value.
 * A visible function has no such argument: the address travels in the frame of the
 * invocation it runs as, which its caller filled (see publishRuntimeContext). The
 * runtime writes the same address into the block itself
 * (`SM50_RT_DISPATCH_ARGUMENTS::DispatchArgumentsAddress`), which is what the
 * caller reads it from when it is not itself bound to the constants.
 */
Value *RaytracingLowerer::dispatchArgumentsAddress(IRBuilder<> &builder) {
  Type *i64 = builder.getInt64Ty();
  if (entry.dispatch_arguments)
    return builder.CreatePtrToInt(entry.dispatch_arguments, i64);
  return loadFrameField(
    builder, invocationBlockOf(builder), offsetof(SM50_RT_FRAME, DispatchArguments), i64
  );
}

/**
 * The pipeline's function tables, as *this* invocation reaches them.
 *
 * A stage that declares one (the ray generation shader, and the intersection shader
 * for the visible function table) hands the argument itself. Every other stage is
 * reached through a function table - the traversal's call into an intersection
 * function, or the call through the visible function table - and the bindings of such
 * a call are the *table's* own argument table, not the dispatch encoder's, so the
 * tables travel in the invocation frame instead, published by the party that issued
 * the call (see publishRuntimeContext). The frame holds the address the ray
 * generation shader received, which is the pointer value itself.
 */
Value *RaytracingLowerer::intersectionFunctionTable(IRBuilder<> &builder) {
  if (entry.intersection_function_table)
    return entry.intersection_function_table;
  Value *address = loadFrameField(
    builder, invocationBlockOf(builder),
    offsetof(SM50_RT_FRAME, IntersectionFunctionTable), builder.getInt64Ty()
  );
  return builder.CreateIntToPtr(
    address, intersectionFunctionTableType(module.getContext())
  );
}

Value *RaytracingLowerer::visibleFunctionTable(IRBuilder<> &builder) {
  if (entry.visible_function_table)
    return entry.visible_function_table;
  Value *address = loadFrameField(
    builder, invocationBlockOf(builder), offsetof(SM50_RT_FRAME, VisibleFunctionTable),
    builder.getInt64Ty()
  );
  return builder.CreateIntToPtr(
    address,
    namedStructType(module.getContext(), "struct._visible_function_table_t")
      ->getPointerTo(1)
  );
}

/**
 * Publish the runtime context of this invocation into a block allocated for a
 * callee (see SM50_RT_FRAME).
 *
 * The callee is a visible function, so it cannot be handed the dispatch constants,
 * the two function tables or the root arguments as arguments; the caller is the
 * one that knows them, so it writes them into the callee's block before the call.
 * What it knows them *as* is what this invocation itself resolved them to - its own
 * argument where it has one, otherwise the frame it was handed - so a nested trace
 * propagates the same addresses instead of leaving the callee's frame empty. The
 * local root entry is *not* written here: it is the callee's own record, which only
 * the caller of a specific trace knows, so `lowerTraceRay` / `lowerCallShader`
 * publish it with the record they selected.
 */
void RaytracingLowerer::publishRuntimeContext(IRBuilder<> &builder, Value *block) {
  Type *i64 = builder.getInt64Ty();
  /* DispatchRaysIndex() is defined in every raytracing stage: the callee rebuilds it from this. */
  if (Value *linear = linearDispatchIndex(builder))
    storeFrameWord(builder, block, offsetof(SM50_RT_FRAME, LinearIndex), linear);
  Value *dispatch = dispatchArgumentsAddress(builder);
  if (dispatch)
    storeFrameField(
      builder, block, offsetof(SM50_RT_FRAME, DispatchArguments), dispatch
    );
  /*
   * The two function tables are pointers (an address-space-1 pointer each), so their
   * addresses are the pointer values; a stage that has no such argument reads the
   * address its own caller published, which is the same dispatch-wide table.
   */
  storeFrameField(
    builder, block, offsetof(SM50_RT_FRAME, IntersectionFunctionTable),
    builder.CreatePtrToInt(intersectionFunctionTable(builder), i64)
  );
  storeFrameField(
    builder, block, offsetof(SM50_RT_FRAME, VisibleFunctionTable),
    builder.CreatePtrToInt(visibleFunctionTable(builder), i64)
  );
  /*
   * The global root arguments and the static sampler table are bound entries of the
   * dispatch, so their addresses are the values of the arguments the ray generation
   * shader was handed; a stage that has no such arguments (a visible function, or a
   * pipeline whose signature declares none) republishes the address it read out of
   * its own frame. The ray generation shader is the publisher itself, so an entry it
   * has no argument for is genuinely unbound and stays zero, which the readers treat
   * as "not bound" rather than resolving the range somewhere else.
   */
  auto entryAddress = [&](Value *argument, uint32_t byte_offset) -> Value * {
    if (argument)
      return builder.CreatePtrToInt(argument, i64);
    if (context.stage == SM50_RT_STAGE_RAYGEN)
      return builder.getInt64(0);
    return loadFrameField(builder, invocationBlockOf(builder), byte_offset, i64);
  };
  storeFrameField(
    builder, block, offsetof(SM50_RT_FRAME, GlobalRootArguments),
    entryAddress(entry.root_arguments, offsetof(SM50_RT_FRAME, GlobalRootArguments))
  );
  storeFrameField(
    builder, block, offsetof(SM50_RT_FRAME, StaticSamplers),
    entryAddress(entry.static_samplers, offsetof(SM50_RT_FRAME, StaticSamplers))
  );
}

/**
 * Allocate a per-invocation block of this export's layout.
 *
 * A block is allocated per call site, in the entry block of the function that
 * traces, so it is a distinct object for every `TraceRay` and `CallShader` in the
 * shader: two payloads can be live at once, a loop reuses the block after the trace
 * returns, and a recursive invocation owns its own storage. The layout is the one
 * `RaytracingExportContext` publishes (frame, payload, hit state, attributes,
 * candidate control), so a callback can reach every region from the payload
 * pointer.
 */
Value *RaytracingLowerer::allocateInvocationBlock(IRBuilder<> &builder, StringRef name) {
  LLVMContext &llvm_context = module.getContext();
  Function *function = builder.GetInsertBlock()->getParent();
  BasicBlock &entry_block_ir = function->getEntryBlock();
  IRBuilder<> entry_builder(&entry_block_ir, entry_block_ir.getFirstInsertionPt());
  auto *array = ArrayType::get(Type::getInt8Ty(llvm_context), std::max(blockSize(), 1u));
  AllocaInst *alloca = entry_builder.CreateAlloca(array, nullptr, name);
  alloca->setAlignment(Align(16));
  return alloca;
}

/** i8* user payload area of `block`, passed to visible shaders. */
Value *RaytracingLowerer::payloadPointerOf(IRBuilder<> &builder, Value *block) {
  LLVMContext &llvm_context = module.getContext();
  Value *bytes = bytePointerOf(builder, block);
  return builder.CreateGEP(
    Type::getInt8Ty(llvm_context), bytes, builder.getInt32(layout.frame_size)
  );
}

/** i8* SM50_RT_HIT_STATE of `block`. */
Value *RaytracingLowerer::hitStatePointerOf(IRBuilder<> &builder, Value *block) {
  LLVMContext &llvm_context = module.getContext();
  Value *bytes = bytePointerOf(builder, block);
  return builder.CreateGEP(
    Type::getInt8Ty(llvm_context), bytes, builder.getInt32(hitStateOffset())
  );
}

/** i8* per-candidate control area of `block`. */
Value *RaytracingLowerer::candidatePointerOf(IRBuilder<> &builder, Value *block) {
  LLVMContext &llvm_context = module.getContext();
  Value *bytes = bytePointerOf(builder, block);
  return builder.CreateGEP(
    Type::getInt8Ty(llvm_context), bytes, builder.getInt32(candidateOffset())
  );
}

/**
 * Copy the payload object a trace names into a child block's payload area.
 *
 * The operand is the address of the shader's own payload object (a local
 * allocation, or the payload argument of a visible function). Its bytes are copied
 * into the child block, which is what the shaders this trace runs will read and
 * write; the caller's object is not touched, so a nested trace cannot overwrite the
 * outer payload and a loop's object is still intact when the trace returns.
 *
 * Returns the child's user payload area. Hardware traversal instead receives the
 * complete child block, including the frame before that area.
 */
Value *RaytracingLowerer::copyPayloadInto(
  IRBuilder<> &builder, Value *child_block, Value *payload_operand
) {
  Value *child_payload = payloadPointerOf(builder, child_block);
  if (!payload_operand || layout.payload_size == 0)
    return child_payload;
  emitMemcpy(module, builder, child_payload, payload_operand, layout.payload_size);
  return child_payload;
}

/** Copy a child block's payload area back to the object the trace named. */
void RaytracingLowerer::copyPayloadBack(
  IRBuilder<> &builder, Value *child_block, Value *payload_operand
) {
  if (!payload_operand || layout.payload_size == 0)
    return;
  emitMemcpy(
    module, builder, payload_operand, payloadPointerOf(builder, child_block), layout.payload_size
  );
}

/**
 * Call one export through the visible function table.
 *
 * `block` is the invocation block the callee runs with: its payload area, its hit
 * state and its candidate control all come from it, so the callee sees the state of
 * *this* trace rather than the caller's.
 *
 * `record_index` and `depth` are the *callee's* values: the callee receives them as
 * its own AIR arguments, which is what makes a nested trace observe its real
 * recursion depth instead of the depth the enclosing invocation happened to have.
 */
Value *RaytracingLowerer::callVisibleFunction(
  IRBuilder<> &builder, Value *block, Value *record_index, Value *depth
) {
  /*
   * The call goes through the callback layer's shared helper, which owns the
   * visible-function ABI (slot 0 is the null entry, so export index N lives in slot
   * N + 1, and the callee's payload / hit-state parameters keep the address spaces of
   * the pointers passed in). One implementation of that ABI is what keeps a hit
   * shader reached from here indistinguishable from one reached by a candidate
   * callback.
   *
   * A callable has no hit of its own, but it is still an `air.visible` function whose
   * second parameter is the hit state, so it is handed this invocation's hit state -
   * a real address, which the callable's body cannot read because a hit shader system
   * value outside a hit shader is rejected by this pass.
   */
  /*
   * Miss, closest-hit and callable shaders receive this thread-local child block.
   * Ray-data pointers belong only to traversal callbacks and the any-hit shaders
   * they invoke; casting a thread allocation into ray-data space loses its writes.
   */
  Value *payload = payloadPointerOf(builder, block);
  Value *hit_state = hitStatePointerOf(builder, block);
  /*
   * The callee is a visible function: it has no bound arguments at all, so the
   * state every stage of the pipeline shares - the dispatch constants, the two
   * function tables and the root arguments - is written into its block here, by the
   * caller that knows it. Its own record's local root entry is published separately
   * by the caller of the trace, which is the only party that knows which record it
   * selected.
   */
  publishRuntimeContext(builder, block);
  return EmitVisibleFunctionCall(
    module, visibleFunctionTable(builder), record_index, depth, payload, hit_state, builder
  );
}

/**
 * The block the shader being lowered is running with.
 *
 * Ray generation owns the block head; hardware intersection receives that same
 * whole block as ray-data storage. Visible stages instead receive the user payload
 * area, exactly frame_size bytes after the head.
 */
Value *RaytracingLowerer::invocationBlockOf(IRBuilder<> &builder) {
  if (entry_block)
    return entry_block;
  LLVMContext &llvm_context = module.getContext();
  /*
   * The head is derived once, in the entry block, so it dominates every use: the
   * payload pointer is a function argument here, and a GEP built at the first use
   * site would not necessarily dominate a later one (a use inside a loop body, or
   * after a branch). The byte cast is built there too, for the same reason.
   */
  BasicBlock &function_entry = entry.function->getEntryBlock();
  IRBuilder<> entry_builder(&function_entry, function_entry.getFirstInsertionPt());
  Value *payload = bytePointerOf(entry_builder, entry.payload);
  if (context.stage == SM50_RT_STAGE_RAYGEN ||
      (context.stage == SM50_RT_STAGE_INTERSECTION && !context.query_callbacks)) {
    entry_block = payload;
    return entry_block;
  }
  entry_block = entry_builder.CreateGEP(
    Type::getInt8Ty(llvm_context), payload,
    entry_builder.getInt32(-static_cast<int32_t>(layout.frame_size))
  );
  return entry_block;
}

/**
 * The acceleration-structure declaration named by a `dx.op.createHandleForLib`.
 *
 * The operand is the loaded value of the resource global (a library resource
 * object), so it is traced back through the load / GEP / cast chain to the
 * `!dx.resources` declaration the parser recorded; the declaration's kind is what
 * says whether this handle is an acceleration structure.
 */
const ResourceBinding *RaytracingLowerer::accelerationStructureOf(const CallInst &call) const {
  if (call.arg_size() < 2)
    return nullptr;
  Value *operand = call.getArgOperand(1);
  while (true) {
    if (auto *load = dyn_cast<LoadInst>(operand)) {
      operand = load->getPointerOperand();
      continue;
    }
    if (auto *gep = dyn_cast<GetElementPtrInst>(operand)) {
      operand = gep->getPointerOperand();
      continue;
    }
    if (auto *cast = dyn_cast<CastInst>(operand)) {
      operand = cast->getOperand(0);
      continue;
    }
    break;
  }
  auto *global = dyn_cast<GlobalVariable>(operand);
  if (!global)
    return nullptr;

  const ResourceBinding *match = nullptr;
  for (const ResourceBinding &binding : resources) {
    if (binding.kind != ResourceKind::RTAccelerationStructure)
      continue;
    if (binding.symbol == global) {
      match = &binding;
      break;
    }
    /* Library declarations may not carry the symbol; match the mangled name the
     * same way the resource pass does. */
    if (!binding.name.empty() && global->getName().contains(("?" + binding.name + "@@").data())) {
      if (match && match != &binding)
        return nullptr;
      match = &binding;
    }
  }
  return match;
}

/**
 * This invocation's DispatchRaysIndex linearized as x + Width * (y + Height * z), which D3D12's
 * Width * Height * Depth <= 2^30 keeps within 32 bits. The ray generation shader computes it from its
 * thread position; every other stage reads the frame its caller published it into.
 */
Value *RaytracingLowerer::linearDispatchIndex(IRBuilder<> &builder) {
  Type *u32 = builder.getInt32Ty();
  if (!entry.thread_position)
    return loadFrameField(builder, invocationBlockOf(builder), offsetof(SM50_RT_FRAME, LinearIndex), u32);
  Value *width = loadDispatchField(builder, kRtDispatchWidth, u32);
  Value *height = loadDispatchField(builder, kRtDispatchHeight, u32);
  if (!width || !height)
    return nullptr;
  Value *x = builder.CreateExtractElement(entry.thread_position, builder.getInt64(0));
  Value *y = builder.CreateExtractElement(entry.thread_position, builder.getInt64(1));
  Value *z = builder.CreateExtractElement(entry.thread_position, builder.getInt64(2));
  return builder.CreateAdd(x, builder.CreateMul(width, builder.CreateAdd(y, builder.CreateMul(height, z))));
}

Error RaytracingLowerer::lowerDispatchRaysIndex(CallInst &call, IRBuilder<> &builder) {
  if (call.arg_size() != 2)
    return createError("dx.op.dispatchRaysIndex expects one operand");
  auto *component = dyn_cast<ConstantInt>(call.getArgOperand(1));
  if (!component || component->getZExtValue() > 2)
    return createError("dx.op.dispatchRaysIndex component must be a constant 0, 1 or 2");
  Value *lane = nullptr;
  if (entry.thread_position) {
    lane = builder.CreateExtractElement(entry.thread_position, builder.getInt64(component->getZExtValue()));
  } else {
    Type *u32 = builder.getInt32Ty();
    Value *linear = linearDispatchIndex(builder);
    Value *width = loadDispatchField(builder, kRtDispatchWidth, u32);
    Value *height = loadDispatchField(builder, kRtDispatchHeight, u32);
    if (!width || !height)
      return createError("dxil: the dispatch arguments are not bound in this shader");
    switch (component->getZExtValue()) {
    case 0:
      lane = builder.CreateURem(linear, width);
      break;
    case 1:
      lane = builder.CreateURem(builder.CreateUDiv(linear, width), height);
      break;
    default:
      lane = builder.CreateUDiv(linear, builder.CreateMul(width, height));
      break;
    }
  }
  call.replaceAllUsesWith(lane);
  call.eraseFromParent();
  return Error::success();
}

Error RaytracingLowerer::lowerDispatchRaysDimensions(CallInst &call, IRBuilder<> &builder) {
  if (call.arg_size() != 2)
    return createError("dx.op.dispatchRaysDimensions expects one operand");
  auto *component = dyn_cast<ConstantInt>(call.getArgOperand(1));
  if (!component || component->getZExtValue() > 2)
    return createError("dx.op.dispatchRaysDimensions component must be a constant 0, 1 or 2");
  const uint32_t offsets[3] = {kRtDispatchWidth, kRtDispatchHeight, kRtDispatchDepth};
  Value *value =
    loadDispatchField(builder, offsets[component->getZExtValue()], builder.getInt32Ty());
  if (!value)
    return createError("dxil: the dispatch arguments are not bound in this shader");
  call.replaceAllUsesWith(value);
  call.eraseFromParent();
  return Error::success();
}

Error RaytracingLowerer::lowerHitStateRead(
  CallInst &call, uint32_t opcode, IRBuilder<> &builder
) {
  /*
   * Hit shader system values are only defined in a hit shader. A closest-hit or
   * any-hit shader reads the state the traversal published for the hit it is
   * running as. An intersection shader reads the same values, but they
   * arrive as Metal's own `[[intersection]]` arguments (primitive / geometry /
   * instance ids, the ray interval), so the stage builder publishes them into this
   * invocation's hit state in its prologue before the DXIL body runs. A ray
   * generation, miss or callable shader has no hit at all, so a read there is
   * rejected instead of answering from an uninitialized hit state.
   */
  const bool is_hit_stage = context.stage == SM50_RT_STAGE_CLOSEST_HIT ||
                            context.stage == SM50_RT_STAGE_ANY_HIT ||
                            context.stage == SM50_RT_STAGE_INTERSECTION;
  /* D3D12 also defines the ray system values (WorldRayOrigin/Direction, RayTMin, RayTCurrent,
   * RayFlags) in a miss shader; the trace publishes them before the miss runs. */
  const bool is_ray_value = opcode == OpWorldRayOrigin || opcode == OpWorldRayDirection ||
                            opcode == OpRayTMin || opcode == OpRayTCurrent || opcode == OpRayFlags;
  const bool readable = is_hit_stage || (context.stage == SM50_RT_STAGE_MISS && is_ray_value);
  /*
   * The hit state lives inside the invocation block (dxil_converter.hpp layout), so
   * the payload pointer - not a separate argument - is what says the block exists.
   */
  if (!readable || !entry.payload)
    return unsupported("DXIL hit shader system value read outside a hit shader");
  LLVMContext &llvm_context = module.getContext();
  Type *f32 = Type::getFloatTy(llvm_context);
  Type *u32 = Type::getInt32Ty(llvm_context);
  Value *block = invocationBlockOf(builder);
  Value *replacement = nullptr;
  switch (opcode) {
  case OpWorldRayOrigin:
  case OpWorldRayDirection: {
    if (call.arg_size() != 2)
      return createError("DXIL world ray system value requires a component");
    auto *component = dyn_cast<ConstantInt>(call.getArgOperand(1));
    if (!component || component->getZExtValue() > 2)
      return createError("DXIL world ray component must be a constant 0, 1 or 2");
    const uint32_t offset = opcode == OpWorldRayOrigin ? kRtHitWorldOrigin : kRtHitWorldDirection;
    replacement = readHitState(builder, block, offset + uint32_t(component->getZExtValue()) * sizeof(float), f32);
    break;
  }
  case OpRayTMin:
    replacement = readHitState(builder, block, kRtHitTMin, f32);
    break;
  case OpRayTCurrent:
    replacement = readHitState(builder, block, kRtHitTCurrent, f32);
    break;
  case OpPrimitiveIndex:
    replacement = readHitState(builder, block, kRtHitPrimitiveIndex, u32);
    break;
  /*
   * D3D12's InstanceID is the application's user defined per-instance id, which is
   * the traversal's *user* instance id; InstanceIndex is the autogenerated index of
   * the instance in the top-level structure, which is the traversal's instance id.
   * The two are separate result fields and must not be interchanged.
   */
  case OpInstanceID:
    replacement = readHitState(builder, block, kRtHitInstanceID, u32);
    break;
  case OpInstanceIndex:
    replacement = readHitState(builder, block, kRtHitInstanceIndex, u32);
    break;
  case OpGeometryIndex:
    replacement = readHitState(builder, block, kRtHitGeometryIndex, u32);
    break;
  case OpHitKind:
    replacement = readHitState(builder, block, kRtHitKind, u32);
    break;
  case OpRayFlags:
    replacement = readHitState(builder, block, kRtHitRayFlags, u32);
    break;
  case OpObjectRayOrigin:
  case OpObjectRayDirection: {
    /* `dx.op.objectRayOrigin(i32, i8 col)`: the ray in the hit instance's object space. */
    if (call.arg_size() != 2)
      return createError("DXIL object ray system value requires a component");
    auto *component = dyn_cast<ConstantInt>(call.getArgOperand(1));
    if (!component || component->getZExtValue() > 2)
      return createError("DXIL object ray component must be a constant 0, 1 or 2");
    const uint32_t offset = opcode == OpObjectRayOrigin ? kRtHitObjectOrigin : kRtHitObjectDirection;
    replacement = readHitState(builder, block, offset + uint32_t(component->getZExtValue()) * sizeof(float), f32);
    break;
  }
  case OpObjectToWorld:
  case OpWorldToObject: {
    /*
     * `dx.op.objectToWorld(i32, i32 row, i8 col)`: element [row][col] of the 3x4 matrix.
     * DXC emits the 4x3 forms as the transposed reads of the same 3x4 matrix, so (row, col)
     * always addresses the 3x4 form. The hit state stores four columns of three floats.
     */
    if (call.arg_size() != 3)
      return createError("DXIL instance transform system value requires a row and a column");
    auto *row = dyn_cast<ConstantInt>(call.getArgOperand(1));
    auto *column = dyn_cast<ConstantInt>(call.getArgOperand(2));
    if (!row || !column || row->getZExtValue() > 2 || column->getZExtValue() > 3)
      return createError("DXIL instance transform element must be a constant row 0..2 and column 0..3");
    const uint32_t base = opcode == OpObjectToWorld ? kRtHitObjectToWorld : kRtHitWorldToObject;
    const uint32_t element = uint32_t(column->getZExtValue()) * 3 + uint32_t(row->getZExtValue());
    replacement = readHitState(builder, block, base + element * sizeof(float), f32);
    break;
  }
  default:
    return unsupported(("unhandled DXIL hit shader system value " + Twine(opcode)).str());
  }
  if (!replacement)
    return createError("dxil: the hit state block is missing a field");
  if (replacement->getType() != call.getType())
    return createError("dxil: the hit state field does not match the dx.op result type");
  call.replaceAllUsesWith(replacement);
  call.eraseFromParent();
  return Error::success();
}

// A native intersection function may not start a RayQuery. Enumerate candidates in
// the tracing kernel instead; native queries still provide geometry, masks, opacity,
// transforms and pruning, while the same shader-record and candidate-control ABI
// runs the application's hit shaders between Proceed and Commit.
Value *RaytracingLowerer::emitQueryTraversal(
  IRBuilder<> &b, Value *child, Value *structure, Value *origin, Value *direction,
  Value *t_min, Value *t_max, Value *flags, Value *mask, Value *triangle_cull,
  Value *geometry_cull, Value *opacity_cull, Value *forced_opacity
) {
  LLVMContext &c = module.getContext();
  Type *f32 = b.getFloatTy(), *i32 = b.getInt32Ty(), *i1 = b.getInt1Ty();
  Type *v2 = FixedVectorType::get(f32, 2), *v3 = FixedVectorType::get(f32, 3);
  Type *qp = namedStructType(c, "struct._intersection_query_t")->getPointerTo();
  Type *matrix = StructType::get(c, {v3, v3, v3, v3});
  auto intrinsic = [&](StringRef name, Type *result, ArrayRef<Type *> types, bool mutates) {
    return declareRtIntrinsic(module, (Twine("air.") + name + "_intersection_query.instancing.triangle_data").str(),
                              FunctionType::get(result, types, false), mutates);
  };
  IRBuilder<> entry_builder(&b.GetInsertBlock()->getParent()->getEntryBlock(),
                             b.GetInsertBlock()->getParent()->getEntryBlock().getFirstInsertionPt());
  Value *query = entry_builder.CreateCall(intrinsic("allocate", qp, {}, true), {});
  auto get = [&](IRBuilder<> &g, StringRef name, Type *type) {
    return g.CreateCall(intrinsic(name, type, {qp}, false), {query});
  };
  Value *accept_first = b.CreateICmpNE(b.CreateAnd(flags, b.getInt32(4)), b.getInt32(0));
  b.CreateCall(intrinsic("reset", b.getVoidTy(),
    {qp, v3, v3, f32, f32, structure->getType(), i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i1, i1}, true),
    {query, origin, direction, t_min, t_max, structure, b.CreateAnd(mask, b.getInt32(255)), b.getInt32(0),
     triangle_cull, geometry_cull, opacity_cull, forced_opacity, b.getInt32(3), b.getInt32(-1), b.getInt32(-1),
     b.getInt32(0), b.getFalse(), accept_first});
  storeCandidateWord(b, child, kRtCandidateEndSearch, b.getInt32(0));
  Function *function = b.GetInsertBlock()->getParent();
  BasicBlock *test = BasicBlock::Create(c, "rt.query.next", function);
  BasicBlock *candidate = BasicBlock::Create(c, "rt.query.candidate", function);
  BasicBlock *done = BasicBlock::Create(c, "rt.query.done", function);
  b.CreateBr(test);
  b.SetInsertPoint(test);
  b.CreateCondBr(b.CreateCall(intrinsic("next", i1, {qp}, true), {query}), candidate, done);
  b.SetInsertPoint(candidate);
  Value *type = get(b, "get_candidate_intersection_type", i32);
  Value *is_triangle = b.CreateICmpEQ(type, b.getInt32(1));
  Value *instance = get(b, "get_candidate_instance_id", i32);
  Value *geometry = get(b, "get_candidate_geometry_id", i32);
  Value *primitive = get(b, "get_candidate_primitive_id", i32);
  storeHitState(b, child, kRtHitPrimitiveIndex, primitive);
  storeHitState(b, child, kRtHitGeometryIndex, geometry);
  storeHitState(b, child, kRtHitInstanceIndex, instance);
  storeHitState(b, child, kRtHitInstanceID, get(b, "get_candidate_user_instance_id", i32));
  storeHitState(b, child, kRtHitObjectOrigin, get(b, "get_candidate_ray_origin", v3));
  storeHitState(b, child, kRtHitObjectDirection, get(b, "get_candidate_ray_direction", v3));
  Value *o2w = get(b, "get_candidate_object_to_world_transform", matrix);
  Value *w2o = get(b, "get_candidate_world_to_object_transform", matrix);
  for (unsigned col = 0; col < 4; ++col) {
    storeHitState(b, child, kRtHitObjectToWorld + col * 12, b.CreateExtractValue(o2w, col));
    storeHitState(b, child, kRtHitWorldToObject + col * 12, b.CreateExtractValue(w2o, col));
  }
  Value *upper = get(b, "get_committed_distance", f32);
  storeHitState(b, child, kRtHitTCurrent, upper);
  storeCandidateField(b, child, kRtCandidateTMax, upper);
  storeCandidateField(b, child, kRtCandidateDistance, upper);
  storeCandidateWord(b, child, kRtCandidateCommitted, b.getInt32(0));
  storeCandidateWord(b, child, kRtCandidateDecision, b.getInt32(kRtCandidateNone));
  Value *group_index = b.CreateAdd(loadInstanceContribution(b, child, instance),
    b.CreateAdd(loadCandidateWord(b, child, kRtCandidateHitGroupBase),
      b.CreateMul(geometry, loadCandidateWord(b, child, kRtCandidateGeometryMultiplier))));
  RtShaderRecord lookup;
  shaderRecord(b, RtShaderTable::HitGroup, group_index, lookup);
  storeFrameField(b, child, offsetof(SM50_RT_FRAME, LocalRootEntry), lookup.local_entry);
  // Constant member maps are per compilation, never shared mutable LLVM IR.
  auto member = [&](IRBuilder<> &g, const uint32_t *indices, StringRef name) -> Value * {
    const uint32_t size = std::max(8u, context.hit_group_continuation_count);
    auto *array_type = ArrayType::get(i32, size);
    GlobalVariable *table = module.getGlobalVariable(name, true);
    if (!table) {
      std::vector<uint32_t> words(indices, indices + context.hit_group_continuation_count);
      words.resize(size, kRtRecordIndexNone);
      table = new GlobalVariable(module, array_type, true, GlobalValue::PrivateLinkage,
        ConstantDataArray::get(c, words), name, nullptr, GlobalValue::NotThreadLocal, 2);
    }
    return emitGuardedValue(g, g.CreateICmpULT(lookup.export_index, g.getInt32(context.hit_group_continuation_count)),
      g.getInt32(kRtRecordIndexNone), "rt.query.member", [&](IRBuilder<> &load) -> Value * {
        return load.CreateLoad(i32, load.CreateGEP(array_type, table, {load.getInt32(0), lookup.export_index}));
      });
  };
  BasicBlock *triangle = BasicBlock::Create(c, "rt.query.triangle", function);
  BasicBlock *box = BasicBlock::Create(c, "rt.query.box", function);
  BasicBlock *advance = BasicBlock::Create(c, "rt.query.advance", function);
  b.CreateCondBr(is_triangle, triangle, box);
  b.SetInsertPoint(triangle);
  Value *distance = get(b, "get_candidate_triangle_distance", f32);
  Value *front = get(b, "is_candidate_triangle_front_facing", i1);
  Value *bary = get(b, "get_candidate_triangle_barycentric_coord", v2);
  storeHitState(b, child, kRtHitTCurrent, distance);
  storeHitState(b, child, kRtHitKind, b.CreateSelect(front, b.getInt32(0xfe), b.getInt32(0xff)));
  storeHitState(b, child, kRtHitFrontFace, b.CreateZExt(front, i32));
  storeHitState(b, child, kRtHitBarycentric, bary);
  Value *attributes = b.CreateGEP(b.getInt8Ty(), bytePointerOf(b, child), b.getInt32(layout.candidate_attribute));
  b.CreateAlignedStore(bary, b.CreateBitCast(attributes, v2->getPointerTo()), Align(1));
  Value *any_hit = member(b, context.hit_group_any_hits, "dxmt.rt.query.any_hits");
  emitConditional(b, b.CreateICmpNE(any_hit, b.getInt32(kRtRecordIndexNone)), "rt.query.anyhit", [&](IRBuilder<> &g) {
    callVisibleFunction(g, child, any_hit, loadFrameField(g, child, offsetof(SM50_RT_FRAME, Depth), i32));
  });
  emitConditional(b, b.CreateICmpNE(loadCandidateWord(b, child, kRtCandidateDecision), b.getInt32(kRtCandidateReject)),
    "rt.query.commit.triangle", [&](IRBuilder<> &g) {
      g.CreateCall(intrinsic("commit_triangle_intersection", g.getVoidTy(), {qp}, true), {query});
    });
  b.CreateBr(advance);
  b.SetInsertPoint(box);
  storeCandidateWord(b, child, kRtCandidateSkipAnyHit,
    b.CreateZExt(b.CreateNot(get(b, "is_candidate_non_opaque_bounding_box", i1)), i32));
  Value *intersection = member(b, context.hit_group_intersections, "dxmt.rt.query.intersections");
  emitConditional(b, b.CreateICmpNE(intersection, b.getInt32(kRtRecordIndexNone)), "rt.query.intersection", [&](IRBuilder<> &g) {
    callVisibleFunction(g, child, intersection, loadFrameField(g, child, offsetof(SM50_RT_FRAME, Depth), i32));
  });
  emitConditional(b, b.CreateICmpNE(loadCandidateWord(b, child, kRtCandidateCommitted), b.getInt32(0)),
    "rt.query.commit.box", [&](IRBuilder<> &g) {
      g.CreateCall(intrinsic("commit_bounding_box_intersection", g.getVoidTy(), {qp, f32}, true),
        {query, g.CreateBitCast(loadCandidateWord(g, child, kRtCandidateDistance), f32)});
    });
  b.CreateBr(advance);
  b.SetInsertPoint(advance);
  emitConditional(b, b.CreateICmpNE(loadCandidateWord(b, child, kRtCandidateEndSearch), b.getInt32(0)),
    "rt.query.abort", [&](IRBuilder<> &g) {
      g.CreateCall(intrinsic("abort", g.getVoidTy(), {qp}, true), {query});
    });
  b.CreateBr(test);
  b.SetInsertPoint(done);
  Value *committed_type = get(b, "get_committed_intersection_type", i32);
  Value *result = emitGuardedValue(b, b.CreateICmpNE(committed_type, b.getInt32(0)),
    Constant::getNullValue(intersectionResultType(c)), "rt.query.result", [&](IRBuilder<> &g) -> Value * {
      Value *out = PoisonValue::get(intersectionResultType(c));
      out = g.CreateInsertValue(out, committed_type, 0);
      out = g.CreateInsertValue(out, get(g, "get_committed_distance", f32), 1);
      out = g.CreateInsertValue(out, get(g, "get_committed_primitive_id", i32), 2);
      out = g.CreateInsertValue(out, get(g, "get_committed_geometry_id", i32), 3);
      out = g.CreateInsertValue(out, ConstantPointerNull::get(g.getInt8PtrTy(1)), 4);
      out = g.CreateInsertValue(out, get(g, "get_committed_instance_id", i32), 5);
      out = g.CreateInsertValue(out, get(g, "get_committed_user_instance_id", i32), 6);
      Value *is_tri = g.CreateICmpEQ(committed_type, g.getInt32(1));
      Value *committed_bary = emitGuardedValue(g, is_tri, Constant::getNullValue(v2), "rt.query.bary",
        [&](IRBuilder<> &inner) -> Value * { return get(inner, "get_committed_triangle_barycentric_coord", v2); });
      Value *committed_front = emitGuardedValue(g, is_tri, g.getFalse(), "rt.query.front",
        [&](IRBuilder<> &inner) -> Value * { return get(inner, "is_committed_triangle_front_facing", i1); });
      out = g.CreateInsertValue(out, committed_bary, 7);
      out = g.CreateInsertValue(out, committed_front, 8);
      Value *world_to_object = get(g, "get_committed_world_to_object_transform", matrix);
      Value *object_to_world = get(g, "get_committed_object_to_world_transform", matrix);
      for (unsigned col = 0; col < 4; ++col) {
        out = g.CreateInsertValue(out, g.CreateExtractValue(world_to_object, col), 9 + col);
        out = g.CreateInsertValue(out, g.CreateExtractValue(object_to_world, col), 13 + col);
      }
      return out;
    });
  return result;
}

Error RaytracingLowerer::lowerTraceRay(CallInst &call, IRBuilder<> &builder) {
  /*
   * `dx.op.traceRay(i32 opcode, %dx.types.Handle, i32 RayFlags, i32 InstanceInclusionMask,
   *                 i32 RayContributionToHitGroupIndex,
   *                 i32 MultiplierForGeometryContributionToHitGroupIndex,
   *                 i32 MissShaderIndex,
   *                 float Origin.x, .y, .z, float TMin, float Direction.x, .y, .z,
   *                 float TMax, <payload>*)`
   *
   * Fifteen semantic operands plus the opcode, i.e. sixteen arguments. The RayDesc
   * order is Origin / TMin / Direction / TMax (DXIL's `RayDesc`, not HLSL's argument
   * order), which is why TMin sits between the origin and the direction; the operand
   * count and every index below were read off DXC's own bitcode
   * (build/owned-direct-20260919 fixtures and /tmp/rt-fixture.ll:268,725).
   */
  if (call.arg_size() != 16)
    return createError(
      "dx.op.traceRay expects 15 operands, got " + Twine(call.arg_size() - 1)
    );
  if (!entry.payload)
    return createError("dxil: dx.op.traceRay has no payload block");

  LLVMContext &llvm_context = module.getContext();
  Type *f32 = Type::getFloatTy(llvm_context);
  Type *u32 = Type::getInt32Ty(llvm_context);
  Type *f32x3 = FixedVectorType::get(f32, 3);
  Type *i8ptr = Type::getInt8PtrTy(llvm_context);

  /*
   * The acceleration structure is TraceRay's first operand, the handle the shader
   * built from its `RaytracingAccelerationStructure` declaration, not a
   * pipeline-wide slot: a shader may name several structures and may select one
   * dynamically, and each trace has to run against the structure *it* was given.
   * `dx.op.createHandleForLib` has already replaced that handle with the
   * structure value this export resolves it to.
   */
  Value *acceleration_structure = call.getArgOperand(1);
  /*
   * A resource handle of a library shader is usually annotated
   * (`dx.op.annotateHandle`) before it is used, and the annotation is only erased
   * later by the resource pass, so the chain is walked here to reach the structure
   * the export actually built. The annotation has no other consumer once this call
   * is gone, so the resource pass drops it.
   */
  while (auto *producer = dyn_cast<CallInst>(acceleration_structure)) {
    uint32_t producer_opcode = 0;
    if (!getDxilOpcode(*producer, producer_opcode) || producer_opcode != OpAnnotateHandle ||
        producer->arg_size() < 2)
      break;
    acceleration_structure = producer->getArgOperand(1);
  }
  if (acceleration_structure->getType() != accelerationStructureType(llvm_context)) {
    /*
     * A DXIL resource handle is `%dx.types.Handle` ({ i8* }) carrying the structure
     * this export built for it, so the operand is unwrapped rather than assumed to
     * be the AIR structure type already. The carrier's element is a plain handle
     * pointer while the structure lives in Metal's acceleration-structure address
     * space (address space 1), so the cast between them is an address-space cast:
     * a bitcast would only be valid within one address space.
     */
    auto *handle_type = dyn_cast<StructType>(acceleration_structure->getType());
    if (!handle_type || handle_type->getNumElements() != 1 ||
        !handle_type->getElementType(0)->isPointerTy())
      return createError(
        "dxil: dx.op.traceRay's acceleration-structure operand is not a "
        "RaytracingAccelerationStructure handle"
      );
    acceleration_structure = builder.CreatePointerBitCastOrAddrSpaceCast(
      builder.CreateExtractValue(acceleration_structure, 0),
      accelerationStructureType(llvm_context)
    );
  }
  Value *ray_flags = call.getArgOperand(2);
  Value *instance_mask = call.getArgOperand(3);
  Value *contribution = call.getArgOperand(4);
  Value *geometry_multiplier = call.getArgOperand(5);
  Value *miss_index = call.getArgOperand(6);

  Value *origin = PoisonValue::get(f32x3);
  Value *direction = PoisonValue::get(f32x3);
  for (unsigned i = 0; i < 3; ++i) {
    origin = builder.CreateInsertElement(
      origin, call.getArgOperand(7 + i), builder.getInt64(i)
    );
    direction = builder.CreateInsertElement(
      direction, call.getArgOperand(11 + i), builder.getInt64(i)
    );
  }
  Value *t_min = call.getArgOperand(10);
  Value *t_max = call.getArgOperand(14);

  /*
   * Every TraceRay runs its shaders on a block of its own, allocated here (once, in
   * the entry block) rather than reusing the caller's. The caller's payload object
   * is copied into that block, the traversal and every continuation shader read and
   * write it, and its bytes are copied back into the caller's object when the trace
   * returns. That is what makes D3D12's payload semantics hold: two payloads can be
   * live at once, a loop's payload object survives the trace, a nested trace owns
   * its own block, and the caller's frame / hit state / candidate control are not
   * overwritten by the shaders this trace runs.
   */
  Value *child = allocateInvocationBlock(builder, "rt.child");
  /*
   * The payload object is read before the call is erased: the operand is what the
   * payload is copied into the child block from and back out to, and it has to
   * survive the erasure of the call that named it.
   */
  Value *payload_operand = call.getArgOperand(15);
  copyPayloadInto(builder, child, payload_operand);
  /*
   * The shaders this trace runs are visible functions, so the state every stage of
   * the pipeline shares - the dispatch constants' address, the two function tables
   * and the root arguments - is written into their block here; the local root entry
   * of the record this trace selects is published after the traversal, when the
   * record is known. The instance-contribution array of the structure being traced
   * is published here too, because a per-candidate callback runs *inside* the
   * traversal and reads the contribution of the instance it is offered.
   */
  publishRuntimeContext(builder, child);
  Value *contributions = contributionsArrayAddress(builder, acceleration_structure);
  if (contributions)
    storeFrameField(
      builder, child, offsetof(SM50_RT_FRAME, ContributionArray), contributions
    );
  /*
   * Carry the complete invocation block, including the frame read by callbacks.
   * Visible shader calls still receive the user payload within this block.
   */

  /* D3D12 ray flags -> traversal configuration. See the file comment for the bit
   * mapping and for why ACCEPT_FIRST_HIT_AND_END_SEARCH is not lowered here. */
  Value *forced_opacity = flagField(builder, ray_flags, 0);
  Value *triangle_cull = triangleCullMode(builder, ray_flags);
  Value *opacity_cull = flagField(builder, ray_flags, 6);
  Value *geometry_cull = flagField(builder, ray_flags, 8);
  /*
   * The pipeline's own SKIP_TRIANGLES / SKIP_PROCEDURAL_PRIMITIVES flags are the
   * same bits as the per-trace ones and apply to every trace of the pipeline, so
   * they are folded into the geometry cull. `pipeline_flags` is a constant for the
   * whole pipeline, so this is a constant OR (usually folded away entirely).
   */
  const uint32_t pipeline_geometry_cull =
    (context.pipeline_flags >> 8) & 0x3u;
  if (pipeline_geometry_cull)
    geometry_cull = builder.CreateOr(geometry_cull, builder.getInt32(pipeline_geometry_cull));

  bool skip_closest_hit = false;
  const bool skip_is_constant = constantFlag(ray_flags, kRayFlagSkipClosestHitShader, skip_closest_hit);
  Value *skip_value = nullptr;
  if (!skip_is_constant)
    skip_value = builder.CreateICmpNE(
      builder.CreateAnd(ray_flags, builder.getInt32(kRayFlagSkipClosestHitShader)),
      builder.getInt32(0)
    );

  FunctionCallee traversal = declareRtIntrinsic(
    module, "air.intersect.instancing.triangle_data.world_space_data",
    FunctionType::get(
      intersectionResultType(llvm_context),
      {f32x3, f32x3, f32, f32,
       accelerationStructureType(llvm_context), u32,
       intersectionFunctionTableType(llvm_context), i8ptr, builder.getInt64Ty(),
       u32, u32, u32, u32, u32, u32, u32, u32, u32,
       builder.getInt1Ty(), builder.getInt1Ty()},
      false
    ),
    /*convergent=*/true
  );

  /* Publish the ray parameters into the block the shaders of this trace run on,
   * before the traversal, so a shader the traversal invokes while it is still
   * running (the per-candidate intersection / any-hit callback) can read them. The
   * result fields are published afterwards, once the traversal has produced them. */
  storeHitState(builder, child, kRtHitWorldOrigin, builder.CreateShuffleVector(
    origin, PoisonValue::get(f32x3), ArrayRef<int>{0, 1, 2, 3}
  ));
  storeHitState(builder, child, kRtHitWorldDirection, builder.CreateShuffleVector(
    direction, PoisonValue::get(f32x3), ArrayRef<int>{0, 1, 2, 3}
  ));
  storeHitState(builder, child, kRtHitTMin, t_min);
  storeHitState(builder, child, kRtHitRayFlags, ray_flags);

  /*
   * The per-candidate control area is published before the traversal too. A
   * candidate callback runs *inside* the traversal, and it has to know which hit
   * group this trace selects and over which ray interval: D3D12 computes the hit
   * group as rayContribution + geometryContribution * multiplier, and
   * `dx.op.reportHit` validates the reported distance against the ray's own
   * interval, which RayTCurrent does not carry until the traversal is over. The
   * decision word is cleared so a stale decision cannot be mistaken for this one's.
   */
  storeCandidateWord(builder, child, kRtCandidateDecision, builder.getInt32(kRtCandidateNone));
  storeCandidateWord(builder, child, kRtCandidateHitGroupBase, contribution);
  storeCandidateWord(builder, child, kRtCandidateGeometryMultiplier, geometry_multiplier);
  /*
   * The active ray's interval is two floats (RtCandidateControl::TMin / TMax), so
   * those two fields are stored with the value's own type: a dword store would name
   * an i32 pointer for a float value, which is not a store this IR defines.
   */
  storeCandidateField(builder, child, kRtCandidateTMin, t_min);
  storeCandidateField(builder, child, kRtCandidateTMax, t_max);

  /* Depth the shaders this trace runs will observe. D3D12's limit is exclusive: the
   * ray generation shader is depth 0 and a trace is legal while the shaders it runs
   * are at most MaxTraceRecursionDepth, so the bound is on the *next* depth. The
   * limit is read from the dispatch constants, which the runtime fills from the
   * pipeline actually being dispatched; a limit of 0 therefore forbids tracing
   * entirely, exactly as D3D12 does. The compile-time value is only the fallback for
   * a module whose dispatch block is not bound. */
  Value *depth = entry.depth;
  if (!depth || !depth->getType()->isIntegerTy(32))
    depth = builder.getInt32(0);
  Value *next_depth = builder.CreateAdd(depth, builder.getInt32(1));
  Value *limit = loadDispatchField(builder, kRtRecursionDepth, u32);
  if (!limit)
    limit = builder.getInt32(context.max_recursion_depth);
  Value *within_depth = builder.CreateICmpULE(next_depth, limit);
  storeFrameWord(builder, child, offsetof(SM50_RT_FRAME, Depth), next_depth);
  storeFrameWord(
    builder, child, offsetof(SM50_RT_FRAME, Flags),
    builder.CreateSelect(
      within_depth, builder.getInt32(0), builder.getInt32(kRtFrameFlagRecursionExceeded)
    )
  );

  /*
   * The traversal cannot be skipped by a select: issuing it would run shaders the
   * pipeline's recursion limit does not allow. Everything after the call moves to
   * its own block so the guarded traversal does not disturb the rest of the shader.
   */
  BasicBlock *head = call.getParent();
  BasicBlock *tail = head->splitBasicBlock(call.getNextNode(), "rt.tail");
  head->getTerminator()->eraseFromParent();
  call.eraseFromParent();

  Error failure = Error::success();
  auto emitTraversal = [&](IRBuilder<> &b) {
    auto issueTraversal = [&](IRBuilder<> &tb) -> Value * {
      if (context.query_callbacks)
        return emitQueryTraversal(tb, child, acceleration_structure, origin, direction, t_min, t_max,
                                  ray_flags, instance_mask, triangle_cull, geometry_cull, opacity_cull, forced_opacity);
      return tb.CreateCall(
      traversal,
      {origin, direction, t_min, t_max,
       acceleration_structure, instance_mask, intersectionFunctionTable(tb),
       bytePointerOf(tb, child), tb.getInt64(layout.block_size),
       /* triangle_front_facing_winding: D3D12's default, reversed per instance by
        * D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE, which the
        * runtime carries in the Metal instance options. */
       tb.getInt32(kWindingClockwise),
       triangle_cull,
       geometry_cull,
       opacity_cull,
       forced_opacity,
       tb.getInt32(kGeometryTypeAll),
       tb.getInt32(kNoCurveBasis),
       tb.getInt32(kNoCurveBasis),
       tb.getInt32(0),
       tb.getInt1(false),
       /* accept_any_intersection is pinned false: see the file comment. */
       tb.getInt1(false)}
      );
    };
    /*
     * D3D12's null acceleration structure (GPU VA 0) misses every ray. Metal's null
     * instance structure behaves the same, but shader validation reports tracing
     * against it as "Invalid Acceleration Structure type Unknown"
     * (build/local/p3/dxrstateobj/probes/nullas.m modes 3/4), so the traversal is not
     * issued at all: a zero result is intersection_type::none, i.e. the miss path.
     */
    Value *gpu_address = accelerationStructureGpuAddress(acceleration_structure);
    Value *result = gpu_address
      ? emitGuardedValue(
          b, b.CreateICmpNE(gpu_address, b.getInt64(0)),
          Constant::getNullValue(intersectionResultType(llvm_context)), "rt.intersect", issueTraversal
        )
      : issueTraversal(b);

    Value *type = b.CreateExtractValue(result, 0);
    Value *distance = b.CreateExtractValue(result, 1);
    Value *primitive = b.CreateExtractValue(result, 2);
    Value *geometry = b.CreateExtractValue(result, 3);
    Value *instance_index = b.CreateExtractValue(result, 5);
    Value *instance_id = b.CreateExtractValue(result, 6);
    Value *barycentrics = b.CreateExtractValue(result, 7);
    Value *front_facing = b.CreateExtractValue(result, 8);
    Value *is_triangle = b.CreateICmpEQ(type, b.getInt32(kIntersectionTypeTriangle));
    /* A traversal result that names no intersection is a miss. */
    Value *hit = b.CreateICmpNE(type, b.getInt32(kIntersectionTypeNone));

    /* A miss observes RayTCurrent() == the ray's TMax. */
    storeHitState(b, child, kRtHitTCurrent, b.CreateSelect(hit, distance, t_max));
    storeHitState(b, child, kRtHitPrimitiveIndex, primitive);
    storeHitState(b, child, kRtHitInstanceID, instance_id);
    storeHitState(b, child, kRtHitInstanceIndex, instance_index);
    storeHitState(b, child, kRtHitGeometryIndex, geometry);
    /*
     * HitKind of the hit this trace committed. A fixed-function triangle hit reports
     * it from its facing. A procedural hit's kind is the `hitKind` operand of the
     * `dx.op.reportHit` that won: the intersection shader the traversal called ran on
     * this very block while the traversal was still running, so the kind the callback
     * layer recorded in this candidate's control area *is* this invocation's. The
     * traversal result carries no kind for a procedural hit, so reading the committed
     * one is the only correct source - selecting 0 there would make a closest-hit
     * shader read a value no shader ever reported.
     */
    Value *triangle_kind = b.CreateSelect(
      front_facing, b.getInt32(kHitKindTriangleFrontFace),
      b.getInt32(kHitKindTriangleBackFace)
    );
    Value *committed_kind = loadCandidateWord(b, child, kRtCandidateHitKind);
    if (committed_kind)
      storeHitState(
        b, child, kRtHitKind, b.CreateSelect(is_triangle, triangle_kind, committed_kind)
      );
    else
      emitConditional(b, is_triangle, "rt.hitkind", [&](IRBuilder<> &inner) {
        storeHitState(inner, child, kRtHitKind, triangle_kind);
      });
    storeHitState(b, child, kRtHitBarycentric, barycentrics);
    storeHitState(b, child, kRtHitFrontFace, b.CreateZExt(front_facing, b.getInt32Ty()));
    /*
     * The committed instance's transforms, and the ray in its object space. A miss leaves
     * them unspecified in D3D12 (no shader can read them), so they are stored unconditionally.
     */
    Value *object_origin = nullptr, *object_direction = nullptr;
    for (unsigned column = 0; column < 4; ++column) {
      Value *w2o = b.CreateExtractValue(result, 9 + column);
      Value *o2w = b.CreateExtractValue(result, 13 + column);
      storeHitState(b, child, kRtHitObjectToWorld + column * 12, o2w);
      storeHitState(b, child, kRtHitWorldToObject + column * 12, w2o);
      /* object = W2O * (world, 1) for the origin and W2O * (world, 0) for the direction. */
      if (column < 3) {
        Value *origin_term = b.CreateFMul(w2o, b.CreateVectorSplat(3, b.CreateExtractElement(origin, uint64_t(column))));
        Value *direction_term = b.CreateFMul(w2o, b.CreateVectorSplat(3, b.CreateExtractElement(direction, uint64_t(column))));
        object_origin = object_origin ? b.CreateFAdd(object_origin, origin_term) : origin_term;
        object_direction = object_direction ? b.CreateFAdd(object_direction, direction_term) : direction_term;
      } else {
        object_origin = b.CreateFAdd(object_origin, w2o);
      }
    }
    storeHitState(b, child, kRtHitObjectOrigin, object_origin);
    storeHitState(b, child, kRtHitObjectDirection, object_direction);

    /*
     * D3D12 selects the hit group as
     *   InstanceContributionToHitGroupIndex + RayContributionToHitGroupIndex
     *     + GeometryIndex * MultiplierForGeometryContributionToHitGroupIndex
     * and the miss shader by its index in the miss table.
     *
     * The instance contribution is the application's per-instance
     * D3D12_INSTANCE_DESC::InstanceContributionToHitGroupIndex, which Metal has no
     * instance field for: the runtime's AS prepass writes it into the acceleration
     * structure's 64 byte header at +8 as a `uint32_t` array indexed by instance
     * index (rt_as_prepass.metal: `contributions[tid] = contribution`), and that
     * address is `AccelerationStructureHeader::contributionsAddress`. The header is
     * the same one the structure itself was loaded through, so it is read from the
     * trace's own structure, not from a pipeline-wide slot.
     *
     * The ray and geometry halves are the trace's own operands. The runtime's
     * preparation pass then turns the sum into the export index of the record to run.
     */
    /* The instance contribution of the traced structure was published into the
     * frame before the traversal (a per-candidate callback reads it while the
     * traversal runs), so the hit group index is computed from the frame's own copy
     * rather than from a second read of the structure. */
    Value *instance_contribution = loadInstanceContribution(b, child, instance_index);
    Value *hit_group_index = b.CreateAdd(
      b.CreateAdd(contribution, b.CreateMul(geometry, geometry_multiplier)),
      instance_contribution
    );
    RtShaderRecord hit_lookup, miss_lookup;
    if (!shaderRecord(b, RtShaderTable::HitGroup, hit_group_index, hit_lookup) ||
        !shaderRecord(b, RtShaderTable::Miss, miss_index, miss_lookup)) {
      failure = createError("dxil: the raytracing dispatch arguments are not bound");
      return;
    }
    Value *hit_record = hit_lookup.export_index;
    Value *miss_record = miss_lookup.export_index;
    if (!context.hit_group_continuations || !context.hit_group_continuation_count) {
      failure = createError("dxil: missing hit-group continuation map");
      return;
    }
    /*
     * The table is padded to at least kMinContinuationEntries with "no record": under
     * Metal shader validation the AGX linker faults (LinkInfo::initialize, null read) on
     * a raygen that indexes a 2- or 4-entry addrspace(2) i32 table with a loaded value
     * and calls through the visible function table (probe:
     * build/local/p3/dxrstateobj/probes/agxlink: sizes 1, 3, 5..128 link). The guard below still
     * bounds the index by the real count, so the padding is never read.
     */
    constexpr uint32_t kMinContinuationEntries = 8;
    const uint32_t continuation_entries =
      std::max<uint32_t>(context.hit_group_continuation_count, kMinContinuationEntries);
    auto *continuation_type = ArrayType::get(b.getInt32Ty(), continuation_entries);
    GlobalVariable *continuation_table = module.getGlobalVariable("dxmt.rt.continuations", true);
    if (!continuation_table) {
      std::vector<uint32_t> indices(context.hit_group_continuations,
                                    context.hit_group_continuations + context.hit_group_continuation_count);
      indices.resize(continuation_entries, kRtRecordIndexNone);
      continuation_table = new GlobalVariable(module, continuation_type, true, GlobalValue::PrivateLinkage,
        ConstantDataArray::get(module.getContext(), indices), "dxmt.rt.continuations", nullptr,
        GlobalValue::NotThreadLocal, 2);
    }
    Value *hit_continuation = emitGuardedValue(b,
      b.CreateAnd(hit, b.CreateICmpULT(hit_record, b.getInt32(context.hit_group_continuation_count))),
      b.getInt32(kRtRecordIndexNone), "rt.continuation", [&](IRBuilder<> &inner) -> Value * {
        return inner.CreateLoad(inner.getInt32Ty(), inner.CreateGEP(continuation_type, continuation_table,
          {inner.getInt32(0), hit_record}));
      });
    Value *record = b.CreateSelect(hit, hit_continuation, miss_record);
    /* The frame's record index describes the shader about to run, which is only
     * known now; the depth and the recursion flag were published before the
     * traversal for the per-candidate callbacks. */
    storeFrameWord(b, child, offsetof(SM50_RT_FRAME, RecordIndex), record);

    /*
     * The local root arguments the shader about to run reads: the bytes after the
     * identifier of the record this trace selected, i.e. the hit group's record
     * (whose position is `hit_group_index`, the instance contribution included) for
     * a hit and the miss shader's record for a miss. It is published before the
     * call because the callee cannot compute it: what it receives is its *export*
     * index, which is its visible function table slot, and the two differ for every
     * table but the raygen's. A record that resolved to no export publishes zero,
     * which the callee's resource lowering reads as "no local root arguments"
     * rather than as the global ones.
     */
    storeFrameField(
      b, child, offsetof(SM50_RT_FRAME, LocalRootEntry),
      b.CreateSelect(hit, hit_lookup.local_entry, miss_lookup.local_entry)
    );

    /*
     * Run the hit group / miss shader, exactly as D3D12 does after the traversal,
     * with two exceptions the flags and the runtime's record sentinel demand:
     * a record the runtime could not resolve to an export must be skipped rather
     * than run (slot 0 of the table is the null entry), and SKIP_CLOSEST_HIT_SHADER
     * leaves a committed hit without a closest-hit invocation while still letting a
     * miss run the miss shader.
     */
    Value *should_call = b.CreateICmpNE(record, b.getInt32(kRtRecordIndexNone));
    if (skip_value)
      should_call = b.CreateAnd(
        should_call, b.CreateOr(b.CreateNot(hit), b.CreateNot(skip_value))
      );
    else if (skip_closest_hit)
      should_call = b.CreateAnd(should_call, b.CreateNot(hit));

    emitConditional(b, should_call, "rt.run", [&](IRBuilder<> &inner) {
      if (!callVisibleFunction(inner, child, record, next_depth))
        failure = createError("dxil: dx.op.traceRay could not reach the visible function table");
    });
    /*
     * The shaders of this trace have written the payload through the child block, so
     * its bytes are copied back into the object the shader named. This is what makes
     * the payload observable after the trace returns without the caller's own
     * payload object ever being shared with a nested trace.
     */
    copyPayloadBack(b, child, payload_operand);
    b.CreateBr(tail);
  };

  if (auto *constant = dyn_cast<ConstantInt>(within_depth)) {
    if (constant->isOne()) {
      BasicBlock *trace = BasicBlock::Create(llvm_context, "rt.trace", head->getParent(), tail);
      IRBuilder<>(head).CreateBr(trace);
      IRBuilder<> trace_builder(trace);
      emitTraversal(trace_builder);
    } else {
      /* Every trace from this invocation would exceed the pipeline's limit. */
      IRBuilder<>(head).CreateBr(tail);
    }
  } else {
    BasicBlock *trace = BasicBlock::Create(llvm_context, "rt.trace", head->getParent(), tail);
    BasicBlock *exceeded =
      BasicBlock::Create(llvm_context, "rt.recursion_exceeded", head->getParent(), tail);
    IRBuilder<>(head).CreateCondBr(within_depth, trace, exceeded);
    IRBuilder<>(exceeded).CreateBr(tail);
    IRBuilder<> trace_builder(trace);
    emitTraversal(trace_builder);
  }

  return failure;
}

Error RaytracingLowerer::lowerCallShader(CallInst &call, IRBuilder<> &builder) {
  /* callShader(shaderIndex, payload) */
  if (call.arg_size() != 3)
    return createError(
      "dx.op.callShader expects 2 operands, got " + Twine(call.arg_size() - 1)
    );
  /*
   * A callable is invoked like a hit group: it runs with the caller's payload
   * contents, at the caller's recursion depth (D3D12 does not count callables
   * against the ray recursion limit), and whatever it leaves in the payload is what
   * the caller observes. It therefore gets a block of its own - the same
   * per-call-site block a trace uses - whose payload is copied in and back out.
   */
  Value *child = allocateInvocationBlock(builder, "rt.callable");
  copyPayloadInto(builder, child, call.getArgOperand(2));
  Value *callable_index = call.getArgOperand(1);
  RtShaderRecord callable;
  if (!shaderRecord(builder, RtShaderTable::Callable, callable_index, callable))
    return createError("dxil: the raytracing dispatch arguments are not bound");
  Value *record = callable.export_index;
  /*
   * The callable's own record, i.e. the local root bytes of the record `CallShader`
   * selected, published into the block it runs on before the call for the same
   * reason a trace publishes its hit group's: the callee receives its export index,
   * not the record's position, and only the caller knows which record it named. A
   * record that resolved to no export publishes zero.
   */
  storeFrameField(
    builder, child, offsetof(SM50_RT_FRAME, LocalRootEntry), callable.local_entry
  );
  /* D3D12 does not count callable shaders against the ray recursion limit, so the
   * callable runs at the caller's depth. A visible function receives the depth of
   * its own invocation as an argument, which *is* the caller's depth, and the ray
   * generation shader has no such argument, so it falls back to the constant the
   * stage builder published (the raygen is depth 0). */
  Value *depth = entry.depth;
  if (!depth || !depth->getType()->isIntegerTy(32))
    depth = builder.getInt32(0);
  emitConditional(
    builder, builder.CreateICmpNE(record, builder.getInt32(kRtRecordIndexNone)),
    "rt.run_callable", [&](IRBuilder<> &inner) {
      callVisibleFunction(inner, child, record, depth);
    }
  );
  copyPayloadBack(builder, child, call.getArgOperand(2));
  call.eraseFromParent();
  return Error::success();
}

Error RaytracingLowerer::lowerCreateHandleForLib(CallInst &call, IRBuilder<> &builder) {
  /*
   * A raytracing export names its resources through library objects. The
   * acceleration structure is resolved here, because the traversal takes the
   * structure as an ordinary operand and the DXIL handle is only the shader's name
   * for it; every other handle (buffer, texture, sampler) belongs to the resource
   * pass, which runs afterwards and knows the root signature binding, so it is left
   * in place.
   *
   * The structure itself comes from the descriptor, not from a pipeline-wide slot:
   * a `RaytracingAccelerationStructure` is an SRV whose descriptor holds the D3D12
   * GPU virtual address of the structure, and that address is what the runtime
   * resolves to a native structure. A shader may therefore name several structures
   * and select one dynamically.
   */
  const ResourceBinding *declaration = accelerationStructureOf(call);
  if (!declaration)
    return Error::success();

  if (call.arg_size() < 2)
    return createError("dxil: dx.op.createHandleForLib expects a resource operand");

  /*
   * The address is read from the descriptor the declaration resolves to. A
   * declaration the export's root signature does not bind is a shader the runtime
   * cannot have dispatched, so it is reported rather than bound to something else.
   */
  Value *address = accelerationStructureAddress(*declaration, builder);
  if (!address)
    return createError(
      "dxil: the acceleration structure '" + declaration->name +
      "' is not bound by the root signature"
    );

  Value *structure = loadAccelerationStructure(address, builder);
  /*
   * The handle value the shader uses is `%dx.types.Handle` ({ i8* }), and the
   * library usually annotates it (`dx.op.annotateHandle`) before the trace. Both
   * the handle and every annotation of it name the same structure, so they are all
   * replaced by it; leaving an annotation behind would leave the resource pass with
   * a handle whose producer no longer exists.
   *
   * The carrier lives in the default address space while the structure is a pointer
   * into Metal's acceleration-structure space (address space 1, see
   * accelerationStructureType), so the carrier is built with an address-space cast:
   * a bitcast is only defined between pointers of the *same* space, so emitting one
   * here would be an invalid instruction (CastInst::Create asserts on exactly that).
   * Nothing dereferences the carrier - it is the shader's name for the structure and
   * every operation that consumes it is lowered in this pass - so the cast states the
   * carrier rather than reinterpreting memory.
   */
  auto *handle_struct = dyn_cast<StructType>(call.getType());
  Value *handle = PoisonValue::get(call.getType());
  if (handle_struct && handle_struct->getNumElements() == 1 &&
      handle_struct->getElementType(0)->isPointerTy()) {
    handle = builder.CreateInsertValue(
      handle,
      builder.CreatePointerBitCastOrAddrSpaceCast(
        structure, handle_struct->getElementType(0)
      ),
      0
    );
  } else if (call.getType()->isPointerTy()) {
    handle = builder.CreatePointerBitCastOrAddrSpaceCast(structure, call.getType());
  } else {
    /*
     * A carrier that is neither a handle struct nor a pointer cannot name the
     * structure: bitcasting a pointer to a non-pointer is not a cast this IR
     * defines, so the failure is reported instead of emitting an invalid one.
     */
    return createError(
      "dxil: dx.op.createHandleForLib's result is not a resource handle"
    );
  }

  SmallVector<CallInst *, 2> annotations;
  for (User *user : call.users()) {
    auto *annotation = dyn_cast<CallInst>(user);
    uint32_t opcode = 0;
    if (annotation && getDxilOpcode(*annotation, opcode) && opcode == OpAnnotateHandle)
      annotations.push_back(annotation);
  }
  call.replaceAllUsesWith(handle);
  call.eraseFromParent();
  for (CallInst *annotation : annotations) {
    annotation->replaceAllUsesWith(handle);
    annotation->eraseFromParent();
  }
  return Error::success();
}

Error RaytracingLowerer::lowerCall(
  CallInst &call, uint32_t opcode, IRBuilder<> &builder
) {
  switch (opcode) {
  case OpTraceRay:
    return lowerTraceRay(call, builder);
  case OpCallShader:
    return lowerCallShader(call, builder);
  case OpDispatchRaysIndex:
    return lowerDispatchRaysIndex(call, builder);
  case OpDispatchRaysDimensions:
    return lowerDispatchRaysDimensions(call, builder);
  case OpCreateHandleForLib:
    return lowerCreateHandleForLib(call, builder);
  case OpInstanceID:
  case OpInstanceIndex:
  case OpGeometryIndex:
  case OpHitKind:
  case OpRayFlags:
  case OpWorldRayOrigin:
  case OpWorldRayDirection:
  case OpObjectRayOrigin:
  case OpObjectRayDirection:
  case OpObjectToWorld:
  case OpWorldToObject:
  case OpRayTMin:
  case OpRayTCurrent:
  case OpPrimitiveIndex:
    return lowerHitStateRead(call, opcode, builder);
  /*
   * The three any-hit / intersection-shader control operations are owned by
   * dxil_rt_callbacks.cpp: they need the per-candidate traversal context, which that
   * translation unit builds from the invocation block this pass hands them.
   */
  case OpIgnoreHit:
  case OpAcceptHitAndEndSearch:
    return LowerAnyHitControlCall(
      module, entry, invocationBlockOf(builder), context, call, opcode
    );
  case OpReportHit:
    return LowerReportHitCall(module, entry, invocationBlockOf(builder), context, call);
  default:
    return unsupported(("unhandled raytracing dx.op opcode " + Twine(opcode)).str());
  }
}

Error RaytracingLowerer::run() {
  SmallVector<std::pair<CallInst *, uint32_t>, 32> worklist;
  SmallVector<CallInst *, 4> inline_traces;
  for (Function &function : module) {
    if (function.isDeclaration())
      continue;
    for (BasicBlock &block : function) {
      for (Instruction &instruction : block) {
        auto *call = dyn_cast<CallInst>(&instruction);
        uint32_t opcode = 0;
        if (call && getDxilOpcode(*call, opcode) && isRaytracingOpcode(opcode))
          worklist.emplace_back(call, opcode);
        else if (call && getDxilOpcode(*call, opcode) && opcode == OpRayQueryTraceRayInline)
          inline_traces.push_back(call);
      }
    }
  }

  for (auto &[call, opcode] : worklist) {
    if (!call->getParent())
      continue;

    IRBuilder<> builder(call);
    if (Error error = lowerCall(*call, opcode, builder)) {

      return error;
    }
  }

  /*
   * A RayQuery's structure is named by the same createHandleForLib handle a TraceRay
   * uses, and that handle was just replaced by the structure this export resolved, so
   * the address it was resolved from is recovered here and handed to the shared inline
   * lowering (LowerRayQueries). A handle this pass did not resolve (a heap-indexed
   * structure) keeps its dx.op form for the resource pass.
   */
  for (CallInst *call : inline_traces) {
    if (call->arg_size() < 3)
      return createError("dx.op.rayQuery_TraceRayInline expects an acceleration structure");
    Value *operand = call->getArgOperand(2);
    while (auto *producer = dyn_cast<CallInst>(operand)) {
      uint32_t producer_opcode = 0;
      if (!getDxilOpcode(*producer, producer_opcode) || producer_opcode != OpAnnotateHandle ||
          producer->arg_size() < 2)
        break;
      operand = producer->getArgOperand(1);
    }
    Value *address = accelerationStructureGpuAddress(operand);
    if (!address)
      continue;
    if (Error error = RewriteTraceRayInline(*call, address))
      return error;
  }

  SmallVector<Function *, 16> dead;
  for (Function &function : module) {
    if (!function.isDeclaration() || !function.getName().startswith("dx.op."))
      continue;
    if (function.use_empty())
      dead.push_back(&function);
  }
  for (Function *function : dead)
    function->eraseFromParent();
  return Error::success();
}

} // namespace

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

/**
 * Recover the arguments of a lowered raytracing export.
 *
 * The resource-shaped arguments are matched by their AIR metadata kind, which the
 * stage builder wrote. `air.visible_input` is shared by every parameter of a
 * visible function, so those are additionally matched by `air.arg_name`; the
 * per-invocation block of the ray generation shader is a function-local
 * allocation, so the stage builder published it in the `dxmt.rt.locals` named
 * metadata node (function, payload block, hit state block, record index, depth).
 * Nothing is guessed by position.
 *
 * The block is the only storage this pass needs: the hit state, the attribute
 * regions and the candidate control all live inside it at
 * `rtInvocationLayout`'s offsets, so the node's hit-state operand is not read
 * (a hit / miss / callable shader receives its payload pointer as an argument and
 * resolves its own block from it).
 */
llvm::Error ResolveRaytracingEntryArguments(
  llvm::Module &module, llvm::StringRef entry_name, RaytracingEntryArguments &out
) {
  Function *function = module.getFunction(entry_name);
  if (!function || function->isDeclaration())
    return createError("dxil: the raytracing export '" + entry_name + "' was not lowered");
  out.function = function;

  MDNode *arguments = nullptr;
  static const char *kEntryKinds[] = {"air.kernel", "air.visible", "air.intersection"};
  for (const char *kind : kEntryKinds) {
    NamedMDNode *named = module.getNamedMetadata(kind);
    if (!named)
      continue;
    for (unsigned i = 0; i < named->getNumOperands() && !arguments; ++i) {
      MDNode *node = named->getOperand(i);
      if (!node || node->getNumOperands() < 3)
        continue;
      auto *value = dyn_cast<ValueAsMetadata>(node->getOperand(0));
      if (!value || value->getValue() != function)
        continue;
      arguments = dyn_cast<MDNode>(node->getOperand(2));
    }
    if (arguments)
      break;
  }
  if (!arguments)
    return createError(
      "dxil: the lowered raytracing export is not registered under an AIR entry kind"
    );

  auto match = [&](StringRef kind, StringRef arg_name) -> Value * {
    for (const MDOperand &operand : arguments->operands()) {
      MDNode *argument = dyn_cast_or_null<MDNode>(operand.get());
      if (!argument || argument->getNumOperands() < 2)
        continue;
      auto *declared_kind = dyn_cast<MDString>(argument->getOperand(1));
      if (!declared_kind || declared_kind->getString() != kind)
        continue;
      if (!arg_name.empty()) {
        bool named = false;
        for (unsigned i = 1; i + 1 < argument->getNumOperands(); ++i) {
          auto *key = dyn_cast<MDString>(argument->getOperand(i));
          auto *value = dyn_cast<MDString>(argument->getOperand(i + 1));
          if (!key || !value || key->getString() != "air.arg_name")
            continue;
          named = value->getString() == arg_name;
          break;
        }
        if (!named)
          continue;
      }
      auto *index = dyn_cast<ConstantAsMetadata>(argument->getOperand(0));
      if (!index)
        continue;
      auto *value = dyn_cast<ConstantInt>(index->getValue());
      if (!value)
        continue;
      unsigned position = static_cast<unsigned>(value->getZExtValue());
      if (position < function->arg_size())
        return function->getArg(position);
    }
    return nullptr;
  };

  /*
   * The dispatch constants are declared as `air.buffer` with arg_name "dispatch"
   * (InputRaytracingDispatchArguments). They are NOT `air.indirect_buffer`: that kind
   * belongs to the root-signature argument `setup_binding_rootsig` declares, which a
   * raytracing export also carries, so matching it would hand the pass the root
   * signature where it expects HitGroupRecordIndexBase / RecursionDepth.
   */
  out.dispatch_arguments = match("air.buffer", "dispatch");
  /*
   * The two entries every root-signature shader of a dispatch shares, and only the
   * ray generation shader declares (see RaytracingEntryArguments): the global root
   * arguments entry, whose AIR kind is the indirect buffer `setup_binding_rootsig`
   * declares as "rootsig", and the dispatch's one static sampler table, declared as
   * an ordinary buffer. Both are matched by their arg_name because `air.buffer` is
   * also the kind of the dispatch constants ("dispatch") and of the local root table
   * ("local_roots" is the indirect one), so the kind alone would hand this pass the
   * wrong argument.
   *
   * A visible function declares neither - its call ABI is fixed - so both stay null
   * there and `publishRuntimeContext` hands the callee the address it inherits out of
   * its own invocation frame instead.
   */
  out.root_arguments = match("air.indirect_buffer", "rootsig");
  out.static_samplers = match("air.buffer", "static_samplers");
  out.intersection_function_table = match("air.intersection_function_table", "");
  out.visible_function_table = match("air.visible_function_table", "");
  out.thread_position = match("air.thread_position_in_grid", "");
  /* The intersection stage carries its payload as `air.payload`; every other stage
   * receives it as an ordinary visible-function parameter. */
  out.payload = match("air.payload", "");
  if (!out.payload)
    out.payload = match("air.visible_input", "payload");
  /* The record index and the depth a visible function runs as are its own
   * arguments; the ray generation shader has neither, so it keeps the values the
   * stage builder published. */
  out.record_index = match("air.visible_input", "record");
  out.depth = match("air.visible_input", "depth");

  NamedMDNode *locals = module.getNamedMetadata("dxmt.rt.locals");
  if (locals) {
    for (unsigned i = 0; i < locals->getNumOperands(); ++i) {
      MDNode *node = locals->getOperand(i);
      if (!node || node->getNumOperands() < 5)
        continue;
      auto *value = dyn_cast<ValueAsMetadata>(node->getOperand(0));
      if (!value || value->getValue() != function)
        continue;
      auto asValue = [&](unsigned operand) -> Value * {
        if (operand >= node->getNumOperands())
          return nullptr;
        if (auto *reference = dyn_cast<ValueAsMetadata>(node->getOperand(operand)))
          return reference->getValue();
        if (auto *constant = dyn_cast<ConstantAsMetadata>(node->getOperand(operand)))
          return constant->getValue();
        return nullptr;
      };
      if (!out.payload)
        out.payload = asValue(1);
      if (!out.record_index)
        out.record_index = asValue(3);
      if (!out.depth)
        out.depth = asValue(4);
      break;
    }
  }
  if (out.payload && !out.payload->getType()->isPointerTy())
    return createError("dxil: the raytracing payload block is not a pointer");
  return llvm::Error::success();
}

llvm::Error LowerRaytracingOperations(
  ModuleInfo &info, const RaytracingExportContext &context, llvm::StringRef entry_name
) {
  RaytracingEntryArguments arguments;
  if (Error error = ResolveRaytracingEntryArguments(*info.module, entry_name, arguments))
    return error;
  RaytracingLowerer lowerer(*info.module, context, arguments, info.resources);
  return lowerer.run();
}

} // namespace dxmt::dxil
