#pragma once

/*
 * Per-candidate ray tracing callbacks: the AIR side of the D3D12 any-hit /
 * intersection contract.
 *
 * D3D12 runs a per-candidate hook that public Metal only exposes through an
 * `air.intersection` function installed in the intersection function table, whose
 * result is the packed `<{ i1 accept, i1 continue_search }>` proven with the public
 * toolchain (`build/owned-direct-20260919/probes/rt-compiler`, /tmp/rtfix/c1.metal):
 *
 *   define <{ i1, i1 }> @isect(<2 x float> bary, i32 primitive_id, i32 geometry_id,
 *                              i32 instance_id, i32 user_instance_id, float distance,
 *                              i1 front_facing, %Payload addrspace(5)* payload)
 *     with !air.intersection = !{!{<{i1,i1}> (...)* @isect, <outputs>, <inputs>,
 *                                  "air.triangle", "air.instancing", "air.triangle_data"}}
 *
 * so a hit group is decided *while* the traversal runs, not after it. This
 * translation unit owns the two DXIL operations that express that decision from
 * inside a shader:
 *
 *   dx.op.ignoreHit / dx.op.acceptHitAndEndSearch   (any-hit shader)
 *     -> write the candidate's decision and end the any-hit body, so whatever
 *        invoked the any-hit shader (the shared candidate callback of a triangle hit
 *        group, or `dx.op.reportHit` of a procedural one) returns accept=false /
 *        accept=true+continue_search=false for that candidate.
 *
 *   dx.op.reportHit                                 (intersection shader)
 *     -> validate the reported distance against the active ray interval, stage the
 *        reported attributes, run the hit group's any-hit shader for the candidate,
 *        commit the hit when it is the closest accepted one so far, and return the
 *        acceptance flag to the DXIL body. `reportHit` is a value-producing call
 *        that may run any number of times in one intersection invocation; it is
 *        never a whole-shader return.
 *
 * Invocation block layout. Every region a callback touches lives inside the single
 * per-invocation block the tracing shader allocates, because the hardware traversal
 * carries exactly one pointer: the payload. `rtInvocationLayout` is the single
 * authority for the offsets - the stage builder sizes and allocates the block with
 * it, the traversal lowering publishes the candidate fields with it, and the
 * callback layer addresses every region from the `block` base it is handed
 * (`SM50_RT_FRAME` sits at +0, the shader-visible payload at +frame_size).
 *
 * Two attribute regions are required, not one: an any-hit shader reads the
 * attributes of the candidate it is judging (a rejected candidate must not disturb
 * the committed hit), while a closest-hit shader reads the attributes of the hit
 * that actually won. `candidate_attribute` is what the any-hit shader is bound to,
 * `attribute` is what the closest-hit shader is bound to, and a hit is copied from
 * the former to the latter only when it becomes the committed hit.
 *
 * Address spaces. A visible function's payload / hit-state parameters are declared
 * in the address space of the block that calls it. The traversal hands an
 * intersection function its payload in Metal's ray-data space (address space 5,
 * which is what the public front end emits for `ray_data T &`), so the any-hit
 * export is reached from address space 5 and this translation unit emits its calls
 * with the caller's own space rather than casting between spaces.
 */

#include <cstddef>
#include <cstdint>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include "dxil_converter.hpp"
#include "dxil_raytracing.h"

namespace d3dalloy::dxil {

/**
 * The AIR address space a traversal hands a shader its payload in: `ray_data` /
 * object data, which is address space 5.
 *
 * The public front end declares the payload and hit-state parameters of a visible or
 * intersection function as pointers into it (/tmp/rtfix/fix.ll, /tmp/rtfix/c1.ll), and
 * the traversal passes a candidate callback exactly such a pointer. Every payload /
 * hit-state parameter of a lowered shader is therefore declared in this space, and a
 * caller whose own block is thread-space storage casts into it - the cast states the
 * ABI rather than inventing a second one.
 */
inline constexpr unsigned kRtRayDataAddressSpace = 5;

/* -------------------------------------------------------------------------- */
/* Candidate state                                                            */
/* -------------------------------------------------------------------------- */

/** Outcome of one candidate, as the traversal and the hit shaders read it. */
enum RtCandidateDecision : uint32_t {
  /** No decision was taken; the candidate is accepted (no any-hit shader ran). */
  kRtCandidateNone = 0,
  /** `dx.op.ignoreHit`: the candidate is rejected and the search continues. */
  kRtCandidateReject = 1,
  /** The candidate is accepted and the search continues. */
  kRtCandidateAccept = 2,
  /** `dx.op.acceptHitAndEndSearch`: the candidate is accepted and the search ends. */
  kRtCandidateAcceptAndEndSearch = 3,
};

/**
 * The per-candidate control block.
 *
 * Written by three parties, all through this layout: the traversal lowering fills
 * the fields describing the active trace before issuing it, the callback layer fills
 * the outcome of every candidate, and whoever builds the intersection function's
 * result reads the outcome. All fields are 4 byte and the order is part of the
 * shader ABI.
 */
struct RtCandidateControl {
  /** RtCandidateDecision of the candidate currently under consideration. */
  uint32_t Decision;
  /**
   * Distance of the committed hit: the closest accepted `reportHit` so far.
   *
   * Committed state, never the last reported candidate: only a hit that won writes
   * here, so a candidate the any-hit shader rejected leaves the value standing. It is
   * also the shrinking upper bound a later report is tested against (see TMax).
   */
  float Distance;
  /** D3D12 hit kind of the committed hit (`dx.op.reportHit`'s operand). */
  uint32_t HitKind;
  /** Attribute bytes the committed hit reports (D3D12 MaxAttributeSize bound). */
  uint32_t AttributeSize;
  /**
   * `rayContributionToHitGroupIndex` of the trace this candidate belongs to. The hit
   * group of a candidate is `HitGroupBase + geometry * GeometryMultiplier`, which is
   * what a shared candidate callback resolves the hit group with.
   */
  uint32_t HitGroupBase;
  /** `multiplierForGeometryContributionToHitGroupIndex` of that trace. */
  uint32_t GeometryMultiplier;
  /** The active ray's interval, as `dx.op.traceRay` declared it. */
  float TMin;
  float TMax;
  /**
   * Set once a `reportHit` of this invocation was accepted. An intersection
   * function's accept flag is this word: a candidate whose every report was
   * rejected (outside the interval) must not commit, while a rejected report after
   * an accepted one leaves the accepted hit standing.
   */
  uint32_t Committed;
  /**
   * Set once an any-hit shader ended the search. Sticky, because `Decision` is
   * cleared for every new candidate and the end-search must outlive that.
   */
  uint32_t EndSearch;
  /**
   * Identity of the *committed* hit.
   *
   * An intersection shader publishes every candidate's ids into the hit state so the
   * any-hit shader it runs reads that candidate's system values (D3D12 defines them
   * per candidate). A candidate that is then rejected must not leave its own ids
   * behind - the closest-hit shader that runs after the traversal reads the
   * committed hit - so the winning candidate's ids are snapshotted here and written
   * back over the hit state after the decision.
   */
  uint32_t CommittedPrimitiveIndex;
  uint32_t CommittedGeometryIndex;
  uint32_t CommittedInstanceIndex;
  uint32_t CommittedInstanceID;
  /** Kernel-side traversal: an opaque procedural candidate suppresses any-hit. */
  uint32_t SkipAnyHit;
  uint32_t Reserved;
};

static_assert(
  sizeof(RtCandidateControl) == 64,
  "RtCandidateControl layout is part of the shader ABI"
);

/**
 * Size of the per-candidate control block, and the total size of the per-candidate
 * region (the block plus the candidate's own attribute region).
 *
 * The candidate attributes live *after* the control block, not before it: the
 * traversal lowering addresses the control fields as `candidateOffset() + {0x00,
 * 0x10, 0x14, 0x18, 0x1c}`, so the control block has to start exactly where the
 * region does.
 */
inline constexpr uint32_t kRtCandidateControlSize = sizeof(RtCandidateControl);

inline constexpr uint32_t kRtCandidateRegionSize(uint32_t attribute_size) {
  return kRtCandidateControlSize + attribute_size;
}

/** Byte offsets of RtCandidateControl, for the traversal lowering's stores. */
enum RtCandidateField : uint32_t {
  kRtCandidateDecision = offsetof(RtCandidateControl, Decision),
  kRtCandidateDistance = offsetof(RtCandidateControl, Distance),
  kRtCandidateHitKind = offsetof(RtCandidateControl, HitKind),
  kRtCandidateAttributeSize = offsetof(RtCandidateControl, AttributeSize),
  kRtCandidateHitGroupBase = offsetof(RtCandidateControl, HitGroupBase),
  kRtCandidateGeometryMultiplier = offsetof(RtCandidateControl, GeometryMultiplier),
  kRtCandidateTMin = offsetof(RtCandidateControl, TMin),
  kRtCandidateTMax = offsetof(RtCandidateControl, TMax),
  kRtCandidateCommitted = offsetof(RtCandidateControl, Committed),
  kRtCandidateEndSearch = offsetof(RtCandidateControl, EndSearch),
  kRtCandidateCommittedPrimitiveIndex =
    offsetof(RtCandidateControl, CommittedPrimitiveIndex),
  kRtCandidateCommittedGeometryIndex = offsetof(RtCandidateControl, CommittedGeometryIndex),
  kRtCandidateCommittedInstanceIndex = offsetof(RtCandidateControl, CommittedInstanceIndex),
  kRtCandidateCommittedInstanceID = offsetof(RtCandidateControl, CommittedInstanceID),
  kRtCandidateSkipAnyHit = offsetof(RtCandidateControl, SkipAnyHit),
};

/* -------------------------------------------------------------------------- */
/* Invocation block layout                                                    */
/* -------------------------------------------------------------------------- */

/**
 * Byte offsets of a per-invocation block's regions.
 *
 * Traversal carries `block_size` bytes starting at `SM50_RT_FRAME`. Hardware
 * callbacks normalize that block pointer to the user payload before invoking
 * visible shaders. The tracing shader allocates the same complete block.
 */
struct RtInvocationLayout {
  /** `SM50_RT_FRAME` bytes in front of the payload pointer. */
  uint32_t frame_size;
  /** The shader-visible payload object. */
  uint32_t payload_size;
  /** Bytes per attribute region (D3D12 MaxAttributeSize). */
  uint32_t attribute_size;
  /** `SM50_RT_HIT_STATE` of this invocation. */
  uint32_t hit_state;
  /** Committed hit's attributes; what a closest-hit shader reads. */
  uint32_t attribute;
  /** Candidate's attributes; what an any-hit shader reads. */
  uint32_t candidate_attribute;
  /** RtCandidateControl. */
  uint32_t candidate_control;
  /** Bytes from the block head to the end of the block. */
  uint32_t block_size;
};

RtInvocationLayout rtInvocationLayout(const RaytracingExportContext &context);

/* -------------------------------------------------------------------------- */
/* DXIL opcodes                                                               */
/* -------------------------------------------------------------------------- */

/** The `dx.op` opcodes this translation unit owns (DXIL.rst / DxilOperations.h). */
enum RtCallbackOpcode : uint32_t {
  kRtOpIgnoreHit = 155,
  kRtOpAcceptHitAndEndSearch = 156,
  kRtOpReportHit = 158,
};

/* -------------------------------------------------------------------------- */
/* Shared callback primitives                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Call one export through the visible function table, with the callback ABI the
 * lowered visible functions use:
 *
 *   %fp = call i8* @air.get_function_pointer_visible_function_table(vft, slot)
 *   call void %fp(payload, hit_state, i32 record, i32 depth)
 *
 * Slot 0 is always the null entry, so export index N lives in slot N + 1. The
 * callee's signature is built from the payload and hit-state pointers that are
 * passed in, i.e. in the caller's own address space, so no address-space cast is
 * ever emitted. Unconditional; the caller guards the call itself.
 *
 * Returns null when the export has no visible function table.
 */
llvm::Value *EmitVisibleFunctionCall(
  llvm::Module &module, llvm::Value *visible_function_table, llvm::Value *record_index,
  llvm::Value *depth, llvm::Value *payload, llvm::Value *hit_state, llvm::IRBuilder<> &builder
);

/**
 * Run the hit group's any-hit shader for the candidate whose attributes are already
 * in the candidate attribute region, and return whether the candidate is accepted.
 *
 * The candidate's Decision is cleared first, so the answer belongs to this candidate
 * alone; a hit group without an any-hit shader accepts unconditionally without
 * calling anything. Unconditional (the caller guards the call itself), which is what
 * makes it usable both from `dx.op.reportHit` and from the shared candidate callback
 * that decides a triangle hit group.
 *
 * Returns null when the hit group has an any-hit shader but the export cannot reach
 * it (no visible function table argument).
 */
llvm::Value *EmitAnyHitCandidate(
  llvm::Module &module, const RaytracingEntryArguments &entry,
  const RaytracingExportContext &context, llvm::Value *block, llvm::IRBuilder<> &builder
);

/**
 * The candidate's own system values, as the intersection shader declares them.
 *
 * They are per candidate, so they are published into the hit state before the
 * any-hit shader runs and snapshotted into RtCandidateControl when the candidate
 * becomes the committed hit (see EmitCandidateCommit).
 */
struct RtCandidateIdentity {
  llvm::Value *primitive_id = nullptr;
  llvm::Value *geometry_id = nullptr;
  llvm::Value *instance_index = nullptr;
  llvm::Value *instance_id = nullptr;
};

/**
 * Commit the candidate when it wins: copy its attributes into the committed region
 * and record its distance, hit kind, attribute size, identity and the committed flag.
 *
 * `accepted` is the any-hit verdict for this candidate; the hit wins when it is
 * accepted and is closer than everything committed before it, so the committed
 * region, the committed distance and the committed identity always describe the same
 * hit. Nothing is written when the candidate does not win, so a rejected candidate
 * cannot overwrite - or clear - the hit that already stands.
 */
void EmitCandidateCommit(
  llvm::Module &module, const RaytracingExportContext &context, llvm::Value *block,
  llvm::Value *distance, llvm::Value *hit_kind, uint32_t attribute_size,
  const RtCandidateIdentity &identity, llvm::Value *accepted, llvm::IRBuilder<> &builder
);

/**
 * Publish the *committed* hit back into the hit state, and tighten the ray interval
 * to it.
 *
 * An intersection shader writes every candidate's system values into the hit state
 * (that is what the any-hit shader it runs reads), so after the candidate has been
 * decided the hit state has to be restored from the committed snapshot: a rejected
 * candidate must not leave its ids, distance or kind behind for the closest-hit
 * shader that runs after the traversal. A committed hit also ends the search for
 * anything farther away, so TMax is lowered to the committed distance - which is what
 * makes a later, farther `dx.op.reportHit` return false.
 *
 * Emits nothing when no hit has been committed yet, so the first report sees the
 * ray's own interval unchanged.
 */
void EmitCommittedHitStateRestore(
  llvm::Module &module, const RaytracingExportContext &context, llvm::Value *block,
  llvm::IRBuilder<> &builder
);

/**
 * Address of the RtCandidateControl region inside `block`, as an i8* in the block's
 * own address space.
 *
 * Published so the stage builder / shared candidate callback reads and writes the
 * candidate state through the same offsets this translation unit uses, instead of
 * repeating the arithmetic.
 */
llvm::Value *CandidateControlPointer(
  llvm::Module &module, const RaytracingExportContext &context, llvm::Value *block,
  llvm::IRBuilder<> &builder
);

/**
 * Build the packed intersection result `<{ i1 accept, i1 continue_search }>` public
 * Metal requires of an `air.intersection` function (proven with the toolchain, see
 * the file comment).
 *
 * `accept` is the caller's verdict for the candidate - `Committed != 0` for an
 * intersection shader that reported through `dx.op.reportHit`, or the any-hit
 * verdict for the shared callback that decides a triangle hit group - and
 * `continue_search` is the negation of the sticky RtCandidateControl::EndSearch, so
 * `dx.op.acceptHitAndEndSearch` ends the search while `dx.op.ignoreHit` keeps it
 * running.
 */
llvm::Value *EmitIntersectionResult(
  llvm::Module &module, const RaytracingExportContext &context, llvm::Value *block,
  llvm::Value *accept, llvm::IRBuilder<> &builder
);

/* -------------------------------------------------------------------------- */
/* DXIL operations                                                            */
/* -------------------------------------------------------------------------- */

/**
 * Lower `dx.op.ignoreHit` (155) / `dx.op.acceptHitAndEndSearch` (156) of an any-hit
 * export.
 *
 * Both are `noreturn` in DXIL: the candidate's decision is written into
 * RtCandidateControl and the any-hit body ends there, so nothing the shader wrote
 * after the call runs. `block` is the invocation block the any-hit shader was
 * invoked with.
 */
llvm::Error LowerAnyHitControlCall(
  llvm::Module &module, const RaytracingEntryArguments &entry, llvm::Value *block,
  const RaytracingExportContext &context, llvm::CallInst &call, uint32_t opcode
);

/**
 * Lower `dx.op.reportHit` (158) of an intersection export.
 *
 * The reported distance is validated against the active ray interval
 * (RtCandidateControl::TMin / TMax, which the traversal lowering publishes before
 * issuing the trace, because `RayTCurrent` does not carry it until the traversal is
 * over), the reported attributes against the pipeline's MaxAttributeSize, and the
 * hit is only committed when the hit group's any-hit shader accepts it and it is the
 * closest accepted hit so far. The call's result is that acceptance, exactly as
 * D3D12 defines it. `block` is the invocation block the intersection shader runs
 * with.
 */
llvm::Error LowerReportHitCall(
  llvm::Module &module, const RaytracingEntryArguments &entry, llvm::Value *block,
  const RaytracingExportContext &context, llvm::CallInst &call
);

/** The four D3D12 shader tables a dispatch names (SM50_RT_DISPATCH_ARGUMENTS). */
enum class RtShaderTable { RayGeneration, Miss, HitGroup, Callable };

/**
 * One shader record, resolved on the GPU straight out of the application's table.
 *
 * `export_index` is the export the record's 32 byte identifier names, or
 * SM50_RT_INDEX_NONE when the table is absent, the index is outside the table, or the
 * identifier was not handed out by the pipeline being dispatched (magic, version,
 * kind, SM50_RT_DISPATCH_ARGUMENTS::PipelineToken, export < ExportCount).
 * `local_entry` is the D3D12 GPU virtual address of the record's local root
 * arguments (the record + 32, in the D3D12 local root layout the compiled local root
 * signature uses), or 0 together with an invalid export.
 */
struct RtShaderRecord {
  llvm::Value *export_index = nullptr;
  llvm::Value *local_entry = nullptr;
};

/**
 * Resolve record `index` of `table` through the dispatch constants `dispatch` (an i8
 * pointer in any address space). A table with a stride of 0 resolves every index to its
 * single record, as D3D12 defines it. No load is issued through an absent table.
 */
RtShaderRecord EmitShaderRecordLookup(
  llvm::IRBuilder<> &builder, llvm::Value *dispatch, RtShaderTable table, llvm::Value *index
);

} // namespace d3dalloy::dxil
