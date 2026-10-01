// DXR library compiled to DXIL and consumed by tests/owned-d3d12-raytracing.cpp.
//
// Rebuild with upstream Microsoft DXC. No Apple Metal Shader Converter, no
// D3DMetal/GPTK compiler and no MSC-based fallback is involved at any point:
//
//   dxc -T lib_6_5 -HV 2021 -O0 -D RQ_NESTED_CALLBACKS \
//       -Fo raytracing.dxil raytracing.hlsl
//
// build-raytracing-rayquery-stages.sh freezes this library with DXC 1.9 alongside
// the all-stage query fixtures. Omitting RQ_NESTED_CALLBACKS selects the native
// intersection-callback path with the identical outer callback-control oracle.
//
// Contract this library pins down. Every item is asserted from a GPU readback by the
// fixture, never from "the call returned success":
//
//   * ray generation reads DispatchRaysIndex/DispatchRaysDimensions, writes a UAV
//     through the global root signature, and issues one TraceRay with an explicit
//     ray-contribution-to-hit-group index and miss index.
//   * a triangle closest hit reports PrimitiveIndex, InstanceID, RayTCurrent and the
//     triangle barycentrics, and writes its own UAV log slot.
//   * a triangle ANY-HIT controls candidate acceptance through IgnoreHit and
//     AcceptHitAndEndSearch. It records every candidate's decision in its own
//     per-(lane, primitive) UAV log slot BEFORE the terminating intrinsic, because DXR
//     retains the payload and UAV writes made up to the point an invocation is ended, and
//     because each (lane, primitive) pair is written by exactly one invocation: the log is
//     therefore independent of the unspecified any-hit enumeration order.
//   * the miss shader runs when traversal finds no accepted hit and stamps both the
//     payload and its own UAV log slot, which is what makes "traversal found nothing"
//     distinguishable from "the closest hit shader never ran".
//   * a callable shader invoked from the ray generation shader (CallShader) writes a
//     payload field that survives the following TraceRay, plus its own log slot.
//   * RAY_FLAG_SKIP_CLOSEST_HIT_SHADER is honored per trace. Its value is 0x8 in d3d12.h;
//     0x100 is RAY_FLAG_SKIP_TRIANGLES and is not used here. The hit is still committed and
//     the any-hit still runs, so its payload writes survive, but neither the closest hit nor
//     the miss shader runs and kind stays 0.
//   * RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH (0x4) stops traversal at the first candidate
//     the any-hit accepts, and AcceptHitAndEndSearch() does the same from inside the
//     any-hit. IgnoreHit() on that candidate is the documented exception: the search
//     continues, so a rejected first candidate must not be committed.
//   * nested traversal: the closest hit re-traces along its own ray
//     (WorldRayOrigin + WorldRayDirection * RayTCurrent), which is only legal with
//     D3D12_RAYTRACING_PIPELINE_CONFIG.MaxTraceRecursionDepth >= 2, and the nested ray's
//     miss shader marks the payload.
//   * the hit group carries a local root signature, and both the any-hit and the
//     closest-hit read a root constant out of the bytes that follow the 32-byte shader
//     identifier inside their SBT record; TraceRay's hit-group index selects between two
//     such records.
//
//   * a second, PROCEDURAL bottom-level structure of AABBs with its own instance whose
//     D3D12_RAYTRACING_INSTANCE_DESC::InstanceContributionToHitGroupIndex is NONZERO, so the
//     hit group index that selects the procedural record is not the hit group table base: an
//     implementation that dropped the instance contribution would run the triangle hit group
//     of record 0 and read its local root constant instead. The intersection shader reports
//     candidates through ReportHit with custom attributes and a custom hit kind:
//       - a report inside the ray's own [RayTMin, RayTCurrent] interval that the any-hit
//         accepts is committed, and the procedural closest hit observes the reported
//         attributes, HitKind() and the procedural record's local root constant;
//       - a report the any-hit rejects with IgnoreHit() is not committed, the writes the
//         rejecting any-hit made survive, and the miss shader runs;
//       - a report whose distance lies outside the ray's own interval is rejected before the
//         any-hit shader runs at all, so the any-hit's log slot stays untouched and the miss
//         shader runs on a payload carrying no hit-group writes.
//     The intersection shader writes its own log word per AABB and per report, so "the
//     intersection shader ran for this candidate, this is what ReportHit answered, and the
//     distance was inside the interval the shader itself was reporting into" is observable
//     directly - including for one invocation that reports more than once:
//       - the first accepted report commits the hit and tightens the ray interval, so a later
//         FARTHER report is rejected without running the any-hit shader at all;
//       - a later NEARER report is inside the interval, so its any-hit runs and can reject it,
//         and the committed hit must still be the first accepted report's - the distance, hit
//         kind, attributes and local root constant the closest hit observes are all the first
//         report's, while the rejecting invocation's payload writes survive alongside them.
//
// RQ_NESTED_CALLBACKS adds inline queries in the existing callback-control cases,
// requiring their real accepted hit before payload writes or ReportHit. This keeps
// the same exact oracle on the kernel-side callback traversal path.
// Deliberately NOT used: SM 6.6 features, wave intrinsics, and
// D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE geometry - opaque geometry suppresses any-hit
// invocation, which this fixture has to exercise.

// 15 dwords = 60 bytes: a multiple of 4 as D3D12_RAYTRACING_SHADER_CONFIG requires, and
// strictly inside the ray-payload budget. Every word is written by a shader and asserted by
// the fixture; there is no hidden input field, because the any-hit derives its decision from
// payload.lane (a fact the ray generation shader wrote) instead of from a private channel.
//
// The procedural (AABB / intersection shader) evidence deliberately does NOT widen this
// payload: it lives in the output buffer's own per-stage log regions, so the triangle lanes'
// payload contract stays byte-for-byte what it was. A procedural lane reuses the words that
// have a procedural meaning (kind, primitive, instance, hitT, localRoot, lane, dimensions,
// anyHitTag, rejectTag) and leaves the triangle-only words at the value the ray generation
// shader initialized - barycentrics do not exist for a procedural hit, and asserting that
// they stay zero is itself part of the contract.
struct RayPayload
{
    uint  kind;           // 0 = no hit shader ran, 1 = closest hit, 2 = miss
    uint  primitive;      // PrimitiveIndex(), 0xffffffff when no hit shader wrote it
    uint  instance;       // InstanceID(), 0xffffffff when no hit shader wrote it
    float hitT;           // RayTCurrent()
    uint  callableValue;  // written by the callable shader
    uint  localRoot;      // hit-group local root constant
    uint  lane;           // DispatchRaysIndex().x, set by the ray generation shader
    float baryU;          // triangle barycentrics
    float baryV;
    uint  dimensions;     // DispatchRaysDimensions().x
    uint  depth;          // 1 inside the nested trace issued by the closest hit
    uint  recurse;        // ray generation opts this lane into the nested trace
    uint  nestedMiss;     // set by the miss shader reached through the nested trace
    uint  anyHitTag;      // kAcceptTagBase | primitive, written by an ACCEPTING any-hit
    uint  rejectTag;      // kRejectTagBase | primitive, written by a REJECTING any-hit
};

RaytracingAccelerationStructure Scene : register(t0);
// RWStructuredBuffer, not RWByteAddressBuffer: the fixture binds this as a structured
// UAV (DXGI_FORMAT_UNKNOWN with StructureByteStride 4), which is unambiguous on a
// native D3D12 driver.
RWStructuredBuffer<uint> Output : register(u0);

#ifdef RQ_NESTED_CALLBACKS
#include "raytracing-rayquery-callback-contract.hlsl"
#endif

// Root constants, not a CBV: the hit-group local root signature declares a three-dword
// 32-bit-constants parameter at (b0, space16), so 12 bytes land immediately after the
// 32-byte shader identifier inside each hit-group SBT record. Only localTag is read; the
// two trailing dwords exist so the record is not a degenerate identifier-only record.
cbuffer LocalConstants : register(b0, space16)
{
    uint localTag;
    uint localPad0;
    uint localPad1;
};

// ---------------------------------------------------------------------------------------
// Output layout
// ---------------------------------------------------------------------------------------
// Word indices into Output, one region per observable fact. These must stay in sync with the
// fixture's byte offsets (the C++ side names the same regions as k*Base byte offsets), and every
// word that is not inside a region listed here must still hold the fill pattern once both
// dispatches are done - which is what makes a payload or a log region wider than the fixture
// believes observable instead of silent.
//
//   triangle dispatch (DispatchRays 10x1x1)
//     payload            10 lanes * 15 words -> 0..149
//     any-hit log        10 lanes * 4 slots  -> 160..199
//     closest-hit log    10 lanes            -> 200..209
//     miss log           10 lanes            -> 210..219
//     callable log       10 lanes            -> 220..229
//   procedural dispatch (DispatchRays 4x1x1)
//     payload             4 lanes * 15 words            -> 240..299
//     intersection log    4 primitives * 3 crossings    -> 300..311
//     any-hit log         4 lanes * 3 crossings * 4     -> 320..367
//     closest-hit log     4 lanes                       -> 368..371
//     miss log            4 lanes                       -> 372..375
//     hit log             4 lanes * 8 words             -> 384..415
//
// The two dispatches write disjoint regions, so the ten triangle lanes' readback contract is
// byte-for-byte what it was before the procedural coverage was added.
static const uint kPayloadWords = 15u;
// The two dispatches' payload regions. Each lane's payload is kPayloadWords words starting at
// its own base, so a payload wider than kPayloadWords would spill into the next lane's region
// instead of silently overwriting a log word, and the fixture's overrun guard catches it.
static const uint kPayloadBase = 0u;
static const uint kProceduralPayloadBase = 240u;
static const uint kAnyHitSlots = 4u;
static const uint kAnyHitLogWord = 160u;
static const uint kClosestHitLogWord = 200u;
static const uint kMissLogWord = 210u;
static const uint kCallableLogWord = 220u;

// The procedural dispatch is a DispatchRays of its own, through its own ray generation and miss
// exports and the same hit-group table, so the triangle dispatch's shaders and lanes are
// untouched by this coverage.
static const uint kProceduralLaneCount = 4u;
// How many ReportHit calls one intersection-shader invocation may make. A primitive that reports
// more than once needs one log word per report, so the intersection log is indexed by
// (primitive, crossing) and the any-hit log by (lane, crossing); a single-report primitive only
// ever writes crossing 0, and its remaining crossing slots must stay at the fill pattern - which
// is what makes "this report never ran the any-hit" observable for every lane, not just the
// multi-report one.
static const uint kIntersectionCrossings = 3u;
// One word per (AABB, crossing) of the procedural bottom-level structure, indexed by that AABB's
// own PrimitiveIndex() and by the order the intersection shader reported in. Exactly one lane's
// ray is contained by each AABB, so a word is written by exactly one invocation with a value that
// depends only on that AABB, that crossing and that ray - correct even though DXR allows an
// intersection shader to be invoked redundantly for one ray and one primitive.
static const uint kIntersectionLogWord = 300u;
static const uint kProcAnyHitLogWord = 320u;
static const uint kProcAnyHitCrossingWords = 4u;
static const uint kProcAnyHitLogWords = kIntersectionCrossings * kProcAnyHitCrossingWords;
static const uint kProcClosestHitLogWord = 368u;
static const uint kProcMissLogWord = 372u;
static const uint kProceduralHitLogWord = 384u;
static const uint kProceduralHitLogWords = 8u;

// One procedural lane's intersection-shader evidence, one word per (primitive, crossing) where a
// crossing is one ReportHit call in the order the shader made it. A single-report primitive only
// writes crossing 0; the multi-report primitive writes all three. The word is written only after
// that crossing's own intersection test succeeded, and each AABB is entered by exactly one lane's
// ray, so a word is written by exactly one invocation with a value that depends only on that
// primitive, that crossing and that ray: the write stays correct even though DXR allows an
// intersection shader to be invoked redundantly for a given ray and primitive.
//
//   bit 31    the shader ran and reached the write
//   bit 0     ReportHit() returned true (the hit was accepted)
//   bit 1     the reported distance was inside the ray's own [RayTMin(), RayTCurrent()]
//   bits 8-15 the primitive the shader reported for
//
// A word still holding the fill pattern means the shader never reported for that crossing, which
// is a different fact from "it reported and the report was rejected".
static const uint kIsInvoked = 0x80000000u;
static const uint kIsReportAccepted = 0x1u;
static const uint kIsDistanceInInterval = 0x2u;
static const uint kIsPrimitiveShift = 8u;
// One crossing's intersection-log word. Both the single-report and the multi-report path use it,
// so the two cannot drift apart, and a crossing the shader never reached is simply never written.
void storeIntersectionLog(uint primitive, uint crossing, bool accepted, bool withinInterval)
{
    Output[kIntersectionLogWord + primitive * kIntersectionCrossings + crossing] = kIsInvoked
        | (accepted ? kIsReportAccepted : 0u)
        | (withinInterval ? kIsDistanceInInterval : 0u)
        | (primitive << kIsPrimitiveShift);
}

// One procedural lane's any-hit evidence, in the order the procedural any-hit shader writes it.
// The slot is indexed by (lane, crossing), where the crossing is the intersection shader's own
// report counter: a crossing whose report was rejected before the any-hit ran leaves its slot at
// the fill pattern, which is how "the any-hit never ran for that report" becomes observable
// rather than inferred.
static const uint kProcAnyHitDecision = 0u;  // kLogAccepted | (PrimitiveIndex() << 1) | IgnoreHit
static const uint kProcAnyHitMarker = 1u;    // attributes.marker
static const uint kProcAnyHitHitKind = 2u;   // HitKind()
static const uint kProcAnyHitDistance = 3u;  // asuint(attributes.distance)

// One procedural lane's hit evidence, written by the procedural closest-hit shader out of the
// attributes ReportHit delivered. The closest hit can only observe these bytes if the
// intersection shader reported them, which is what makes "the intersection shader ran and
// ReportHit was accepted" a fact a GPU readback can settle.
static const uint kProcHitMarker = 0u;             // attributes.marker
static const uint kProcHitAttributeDistance = 1u;  // asuint(attributes.distance)
static const uint kProcHitRayTCurrent = 2u;        // asuint(RayTCurrent())
static const uint kProcHitKind = 3u;               // HitKind()
static const uint kProcHitPrimitiveIndex = 4u;     // PrimitiveIndex()
static const uint kProcHitInstanceID = 5u;         // InstanceID()
static const uint kProcHitAnyHitTag = 6u;          // the payload tag the any-hit shader left
static const uint kProcHitRejectTag = 7u;          // the payload tag a rejecting any-hit left

// The procedural bottom-level structure: one AABB per procedural primitive, and the ray each
// procedural lane casts. The intersection shader derives the x centre of both the AABB the
// fixture built and the shape inside it from PrimitiveIndex() with the same arithmetic, so the
// shape the shader tests cannot drift out of step with the box the traversal culled against.
// The fixture fills the AABB buffer with this x centre, a half unit of extent in x and y, and
// its own deliberately generous z extents (see the C++ side's kAabbMinZ / kAabbMaxZ).
//
// The AABBs are one hundred units apart in x and each lane's ray is contained by exactly one of
// them, so a lane's traversal can only report the candidate that lane's ray actually entered.
//
// The AABB is deliberately a generous conservative bound in z - it spans far past both the ray's
// extent and the shape - while the shape inside it is a one-unit box. That is how procedural
// geometry is meant to work: the AABB is only a cull volume, and the shape is whatever the
// intersection shader says it is.
static const float kAabbBaseX = 100.0f;
static const float kAabbSpacingX = 100.0f;
// The shape the intersection shader tests is a box of the same half size centred on the AABB's
// x/y centre; only its z centre differs per primitive.
static const float kShapeHalfSize = 0.5f;
static const float kShapeBaseZ = 0.0f;
// The primitive whose shape the intersection shader deliberately places far outside the ray's
// own interval. DXR rejects such a report without committing a hit and without running the
// any-hit shader at all, so that lane must end in the miss shader on a payload no hit group
// wrote.
static const uint kOutOfIntervalPrimitive = 2u;
static const float kOutOfIntervalZCenter = 30.0f;
// The multi-report primitive: a single intersection-shader invocation that calls ReportHit three
// times along the same ray, in the order the fixture asserts.
//
//   crossing 0  t = 1.0, accepted. The hit is committed, so DXR tightens RayTCurrent to 1.0.
//   crossing 1  t = 5.0, farther than the committed hit. It is now outside the tightened ray
//               interval, so ReportHit must return false WITHOUT running the any-hit shader: this
//               is the tightening the callback path has to implement.
//   crossing 2  t = 0.5, nearer than the committed hit and therefore inside the interval. The
//               any-hit shader runs and calls IgnoreHit, so it must return false, the committed
//               hit must stay crossing 0's, and the rejecting invocation's writes must survive.
//
// The primitive's shape is deliberately enormous in z so one invocation really can meet the ray
// three times; the z offsets are an application-defined property of the primitive, exactly as a
// procedural primitive's geometry is, and nothing here depends on lane, ray flags or the
// unspecified order in which primitives are enumerated - there is only one candidate.
//
// The ray starts at z = -1 and travels +z, so a shape whose z span starts at zMin is reported at
// t = zMin + 1. The three crossings are therefore chosen by their z spans:
//   crossing 0  z in [0.0, 1.0]  -> t = 1.0, accepted and committed
//   crossing 1  z in [4.0, 5.0]  -> t = 5.0, farther than the committed hit
//   crossing 2  z in [-0.5, 0.5] -> t = 0.5, nearer than the committed hit
static const uint kMultiReportPrimitive = 3u;
static const float kMultiReportZ0 = 0.5f;
static const float kMultiReportZ1 = 4.5f;
static const float kMultiReportZ2 = 0.0f;
// The crossing whose report the any-hit shader rejects. The other crossing that does not reach
// the any-hit shader is the farther one, which DXR itself rejects against the tightened interval.
static const uint kMultiReportRejectCrossing = 2u;
// The distances the two non-default crossings report, as their bit patterns. The any-hit shader
// decodes which report it is handling from the distance ReportHit delivered, and it can do that
// exactly because the intersection shader computed that distance from these same constants.
// Crossing 0 is the decoder's fallback, so it needs no constant of its own here.
static const float kMultiReportEntry1 = 5.0f;
static const float kMultiReportEntry2 = 0.5f;
// The ray each procedural lane casts: an axis-aligned +z ray whose x is a quarter unit off the
// centre of that lane's AABB. A quarter unit is inside the half-unit AABB in x and outside every
// other AABB (they are a hundred units apart), so which candidate the traversal enumerates is
// decided by the geometry alone and never by the lane index.
static const float kProceduralRayOffsetX = 0.25f;
static const float kProceduralRayY = 0.25f;
static const float kProceduralRayZ = -1.0f;
static const float kProceduralRayTMin = 0.001f;
static const float kProceduralRayTMax = 10.0f;

// Which procedural lane's any-hit rejects its candidate, keyed off payload.lane exactly as the
// triangle any-hit is, so the decision input is part of the payload contract the fixture reads
// back. The other lanes' decisions follow from the shape geometry alone: lane 0's report is
// inside the ray interval and accepted, lane 2's report is outside it and rejected by DXR before
// the any-hit shader runs, and lane 3's nearer third report is the one its any-hit rejects.
static const uint kProceduralRejectLane = 1u;
static const uint kProceduralMultiReportLane = 3u;

// Procedural hit-group payload tags, distinct from the triangle tags so a payload word can never
// be satisfied by the triangle hit group. The instance that carries the AABB structure
// contributes a NONZERO D3D12_RAYTRACING_INSTANCE_DESC::InstanceContributionToHitGroupIndex, so
// the hit group index D3D12 derives for a procedural hit is not the hit group table's base. The
// local root constant the procedural hit group's record carries is therefore the evidence that
// the instance contribution took part in selecting the record: an implementation that dropped it
// would run the triangle hit group at the table base and read THAT record's constant instead.
static const uint kProceduralAcceptTagBase = 0xC0000000u;
static const uint kProceduralRejectTagBase = 0xD0000000u;
// The hit kind an intersection shader reports is an arbitrary 0..127 value D3D12 never
// interprets, so each box tags its reports with its own primitive index.
static const uint kProceduralHitKindBase = 0x70u;
// Reported attributes are entirely application-defined; the marker can only come from the
// intersection shader, so it is what proves the hit shaders read the reported bytes rather than
// the triangle barycentrics the traversal would have produced for a triangle hit.
static const uint kProceduralAttributeMarker = 0x5eeda77u;

// Bit 31 keeps an accepted hit distinguishable from an untouched (fill-pattern) slot,
// because an accepted primitive 0 would otherwise encode as the same value as the
// 0xffffffff fill, and the low bit records IgnoreHit so accept and reject of one
// primitive never collide.
static const uint kLogAccepted = 0x80000000u;
static const uint kAcceptTagBase = 0xA0000000u;
static const uint kRejectTagBase = 0xB0000000u;

// D3D12_RAY_FLAGS, copied from d3d12.h. SKIP_CLOSEST_HIT_SHADER is 0x8; 0x100 is
// RAY_FLAG_SKIP_TRIANGLES.
static const uint kRayFlagAcceptFirstHitAndEndSearch = 0x4u;
static const uint kRayFlagSkipClosestHitShader = 0x8u;

// Which candidate this lane's any-hit rejects, and whether it ends the search itself. The
// policy is keyed off payload.lane, which the ray generation shader wrote before TraceRay, so
// the any-hit's input is part of the same payload contract the fixture reads back rather than
// a private side channel.
static const uint kRejectPrimitiveNone = 0xffu;

// Canonical DXR triangle any-hit signature: payload plus the triangle intersection
// attributes, which is the form every DXR sample and the owned frontend's hit-group
// lowering expect. The attributes are part of the required signature and are not read
// here: the closest hit asserts them, and this shader's job is the accept/reject decision.
[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
#ifdef RQ_NESTED_CALLBACKS
    if (!callbackQueryAccepted()) return;
#endif
    const uint lane = payload.lane;
    const uint primitive = PrimitiveIndex();

    uint rejectPrimitive = kRejectPrimitiveNone;
    bool acceptFirst = false;
    if (lane == 2u)
        rejectPrimitive = 1u;
    else if (lane == 3u || lane == 9u)
        rejectPrimitive = 0u;
    else if (lane == 8u)
        acceptFirst = true;
    const bool reject = primitive == rejectPrimitive;

    // Authoritative per-candidate evidence, written unconditionally and BEFORE any
    // terminating intrinsic. The slot is indexed by (lane, primitive), so no two
    // invocations ever write the same word and the result does not depend on the
    // unspecified order in which candidates are enumerated.
    if (primitive < kAnyHitSlots)
        Output[kAnyHitLogWord + lane * kAnyHitSlots + primitive] =
            kLogAccepted | (primitive << 1u) | (reject ? 1u : 0u);

    // Read out of this hit group's own SBT record, which proves the local root signature is
    // in force for the any-hit as well as for the closest hit.
    payload.localRoot = localTag;

    if (reject)
    {
        // Written before IgnoreHit and retained afterwards: DXR keeps the payload
        // modifications an invocation made up to the point it rejected the hit, while the
        // hit itself (distance and attributes) is not committed.
        payload.rejectTag = kRejectTagBase | primitive;
        IgnoreHit();
    }
    else
    {
        payload.anyHitTag = kAcceptTagBase | primitive;
        if (acceptFirst)
            AcceptHitAndEndSearch();
    }
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    // Written by the closest hit itself: the authoritative "this stage ran for this lane"
    // evidence, independent of any payload word.
    Output[kClosestHitLogWord + payload.lane] = 1u;

    // The nested trace re-enters the shader, so the outer results are captured first and
    // re-stamped after it returns.
    const uint outerPrimitive = PrimitiveIndex();
    const uint outerInstance = InstanceID();
    const float outerHitT = RayTCurrent();
    const float outerBaryU = attributes.barycentrics.x;
    const float outerBaryV = attributes.barycentrics.y;
    const uint outerLocalRoot = localTag;

    if (payload.recurse != 0u && payload.depth == 0u)
    {
        // A second TraceRay from inside a hit shader: depth 1 is this invocation, so the
        // pipeline must declare MaxTraceRecursionDepth >= 2. The origin is the point this
        // ray already hit, offset along the ray direction, so the nested ray leaves the
        // triangle and is only satisfied by the miss shader.
        payload.depth = 1u;
        const float3 nestedOrigin = WorldRayOrigin() + WorldRayDirection() * (outerHitT + 0.001f);
        RayDesc nestedRay;
        nestedRay.Origin = nestedOrigin;
        nestedRay.Direction = WorldRayDirection();
        nestedRay.TMin = 0.001f;
        nestedRay.TMax = 10.0f;
        TraceRay(Scene, 0u, 0xffu, 0u, 1u, 0u, nestedRay, payload);
    }

    payload.kind = 1u;
    payload.primitive = outerPrimitive;
    payload.instance = outerInstance;
    payload.hitT = outerHitT;
    payload.localRoot = outerLocalRoot;
    payload.baryU = outerBaryU;
    payload.baryV = outerBaryV;
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    // Written by the miss shader itself, so "the miss shader ran" is distinguishable from
    // "no hit shader ran at all" (which leaves this slot at its fill pattern) - the exact
    // distinction RAY_FLAG_SKIP_CLOSEST_HIT_SHADER needs.
    Output[kMissLogWord + payload.lane] = 1u;

    // A miss reached from inside the closest hit is the nested trace coming back.
    if (payload.depth == 1u)
        payload.nestedMiss = 1u;
    payload.kind = 2u;
    payload.hitT = 0.0f;
}

[shader("callable")]
void Callable0(inout RayPayload payload)
{
    Output[kCallableLogWord + payload.lane] = 1u;
    payload.callableValue = 0xca11ab1eu ^ payload.lane;
}

// ---------------------------------------------------------------------------------------
// Procedural (AABB) hit group
// ---------------------------------------------------------------------------------------
// The attributes an intersection shader reports. DXR defines this struct entirely, so its
// contents are what the intersection shader chose to put there; 8 bytes keeps the pipeline's
// maximum attribute size at the value the triangle hit group already needs, so the triangle
// coverage's shader configuration is untouched. `marker` carries the marker pattern plus the
// primitive the intersection shader reported for, so the any-hit and closest-hit shaders can
// assert the bytes they read are the ones the intersection shader wrote AND that they describe
// the same primitive the traversal reported - two independent facts from one word.
struct ProceduralAttributes
{
    uint  marker;    // kProceduralAttributeMarker + PrimitiveIndex()
    float distance;  // the distance the intersection shader reported
};

// A value standing in for "no bound on this axis" in the slab test below. The ray this fixture
// casts is axis aligned, so two of its three direction components are exactly zero and the slab
// test for those axes has no finite bound. A large finite sentinel is used instead of an
// infinity so min/max can never produce a NaN through 0 * inf, and it is far outside every t
// this fixture's rays can reach (the longest reported distance is 31.5).
static const float kSlabUnbounded = 1.0e30f;

// The ray/slab interval for one axis. A direction component of exactly zero means the ray never
// leaves the plane it started on: the axis constrains nothing when the origin is inside the
// slab, and no t on the ray can satisfy the slab when it is outside. The empty interval is
// expressed as (entry > exit) so the caller's single min/max comparison rejects it.
float2 slabInterval(float origin, float direction, float lo, float hi)
{
    if (direction == 0.0f)
        return (origin >= lo && origin <= hi) ? float2(-kSlabUnbounded, kSlabUnbounded)
                                              : float2(kSlabUnbounded, -kSlabUnbounded);
    const float inverse = 1.0f / direction;
    const float first = (lo - origin) * inverse;
    const float second = (hi - origin) * inverse;
    return float2(min(first, second), max(first, second));
}

// The z centre of the shape this intersection shader tests for one (primitive, crossing) pair.
// A crossing is one ReportHit call in the order the invocation makes it. Its z centre is a
// property of the primitive and the report - not of the lane, the ray or the ray flags - exactly
// as a procedural primitive's geometry is a property of the primitive in any real application.
// Three primitives place their single shape inside the ray's own interval, one places it far
// beyond the ray's extent, and the multi-report primitive places its three shapes at three
// distinct distances along the same ray so one invocation really can meet it three times.
float shapeCenterZ(uint primitive, uint crossing)
{
    if (primitive == kMultiReportPrimitive)
        return crossing == 0u ? kMultiReportZ0 : (crossing == 1u ? kMultiReportZ1 : kMultiReportZ2);
    return primitive == kOutOfIntervalPrimitive ? kOutOfIntervalZCenter : kShapeBaseZ;
}

// How many reports this primitive's invocation makes: one for every primitive except the
// multi-report one, whose single invocation makes all three of its reports.
uint crossingCount(uint primitive)
{
    return primitive == kMultiReportPrimitive ? kIntersectionCrossings : 1u;
}

float3 shapeMinimum(uint primitive, uint crossing)
{
    const float centerX = kAabbBaseX + primitive * kAabbSpacingX;
    const float centerZ = shapeCenterZ(primitive, crossing);
    return float3(centerX - kShapeHalfSize, -kShapeHalfSize, centerZ - kShapeHalfSize);
}

float3 shapeMaximum(uint primitive, uint crossing)
{
    const float centerX = kAabbBaseX + primitive * kAabbSpacingX;
    const float centerZ = shapeCenterZ(primitive, crossing);
    return float3(centerX + kShapeHalfSize, kShapeHalfSize, centerZ + kShapeHalfSize);
}

// A procedural primitive is described to DXR by an AABB and the surface inside it is whatever
// this shader says it is. The shapes here are boxes and the AABB the fixture built contains all
// of them, so every distance reported is a distance to a shape the acceleration structure really
// found: no constant is invented for the test. ReportHit is called whenever the ray meets a
// shape, including when the meeting point lies outside the ray's own interval - that report is
// then DXR's to reject, and ReportHit answers false, which is one of the rejection paths this
// coverage pins down. Nothing here is conditional on the lane or on the ray flags.
//
// The shader keeps its own copy of the ray interval's upper bound and lowers it whenever a report
// is accepted, exactly as the DXR samples' custom intersection iterators do (their `curT`).
// Computing the interval bit from that copy rather than from a fresh RayTCurrent() keeps this
// shader's own evidence independent of whether a runtime exposes the tightened value to the
// intersection shader, while the ReportHit return values still pin the runtime's own tightening.
[shader("intersection")]
void ProceduralIntersection()
{
#ifdef RQ_NESTED_CALLBACKS
    if (!callbackQueryAccepted()) return;
#endif
    const uint primitive = PrimitiveIndex();
    const float3 origin = WorldRayOrigin();
    const float3 direction = WorldRayDirection();
    const float tMin = RayTMin();
    float intervalTMax = RayTCurrent();

    const uint crossings = crossingCount(primitive);
    for (uint crossing = 0u; crossing < crossings; ++crossing)
    {
        const float3 shapeLo = shapeMinimum(primitive, crossing);
        const float3 shapeHi = shapeMaximum(primitive, crossing);

        const float2 x = slabInterval(origin.x, direction.x, shapeLo.x, shapeHi.x);
        const float2 y = slabInterval(origin.y, direction.y, shapeLo.y, shapeHi.y);
        const float2 z = slabInterval(origin.z, direction.z, shapeLo.z, shapeHi.z);
        const float entry = max(max(x.x, y.x), z.x);
        const float exit = min(min(x.y, y.y), z.y);
        const bool shapeHit = exit >= entry && exit >= tMin;
        const bool withinInterval = entry >= tMin && entry <= intervalTMax;

        ProceduralAttributes attributes;
        attributes.marker = kProceduralAttributeMarker + primitive;
        attributes.distance = entry;

        bool accepted = false;
        if (shapeHit)
            accepted = ReportHit(entry, kProceduralHitKindBase | primitive, attributes);

        // The candidate's identity is carried inside the word by the primitive index and the
        // crossing is the report's own position in this invocation, so a word depends only on the
        // primitive, the crossing and the ray: DXR may invoke an intersection shader redundantly
        // for one ray and one primitive, and every such invocation writes the same values.
        storeIntersectionLog(primitive, crossing, accepted, withinInterval);

        // An accepted report commits a hit, so the ray interval this shader is reporting into
        // tightens to that distance for every later report of this invocation.
        if (accepted)
            intervalTMax = entry;
        if (!shapeHit)
            break;
    }
}

// Which of the primitive's reports this any-hit invocation is handling, decoded from the
// distance ReportHit delivered. That distance is the report's own identity - the same way a real
// application's intersection iterator knows which parameter of the primitive produced the hit -
// so the slot a decision is recorded in follows from the report rather than from any private
// channel, and the decoding is exact because the intersection shader computed the distance from
// these very constants.
uint crossingOfDistance(uint primitive, float distance)
{
    if (primitive != kMultiReportPrimitive)
        return 0u;
    const uint bits = asuint(distance);
    if (bits == asuint(kMultiReportEntry2))
        return 2u;
    if (bits == asuint(kMultiReportEntry1))
        return 1u;
    return 0u;
}

// The procedural hit group's any-hit shader. It reads the reported attributes - not the
// triangle barycentrics - and its decision is keyed off payload.lane, a word the ray generation
// shader wrote before the trace, so the input it decides on is part of the same payload contract
// the fixture reads back rather than a private channel.
[shader("anyhit")]
void ProceduralAnyHit(inout RayPayload payload, in ProceduralAttributes attributes)
{
#ifdef RQ_NESTED_CALLBACKS
    if (!callbackQueryAccepted()) return;
#endif
    const uint lane = payload.lane;
    const uint primitive = PrimitiveIndex();
    const uint crossing = crossingOfDistance(primitive, attributes.distance);
    const bool reject = lane == kProceduralRejectLane
        || (lane == kProceduralMultiReportLane && crossing == kMultiReportRejectCrossing);

    // Written unconditionally and before the terminating intrinsic, so the attributes the
    // report delivered are recorded even when the candidate is rejected. The slot is indexed by
    // (lane, crossing), so no two invocations of one lane ever write the same word and the
    // decisions of a multi-report invocation are all observable at once.
    const uint base = kProcAnyHitLogWord + lane * kProcAnyHitLogWords
        + crossing * kProcAnyHitCrossingWords;
    Output[base + kProcAnyHitDecision] = kLogAccepted | (primitive << 1u) | (reject ? 1u : 0u);
    Output[base + kProcAnyHitMarker] = attributes.marker;
    Output[base + kProcAnyHitHitKind] = HitKind();
    Output[base + kProcAnyHitDistance] = asuint(attributes.distance);

    // Read out of the procedural hit group's own SBT record, which is selected by the hit group
    // index the instance contribution takes part in: an implementation that dropped that
    // contribution would run the triangle hit group and this word would carry its tag.
    payload.localRoot = localTag;

    if (reject)
    {
        payload.rejectTag = kProceduralRejectTagBase | primitive;
        IgnoreHit();
    }
    else
    {
        payload.anyHitTag = kProceduralAcceptTagBase | primitive;
    }
}

// The procedural hit group's closest-hit shader. It observes the attributes the accepted report
// committed, the hit kind the intersection shader chose, and the system values the hit group
// index and the reported hit describe - none of which exist for a triangle hit.
[shader("closesthit")]
void ProceduralClosestHit(inout RayPayload payload, in ProceduralAttributes attributes)
{
    const uint lane = payload.lane;
    Output[kProcClosestHitLogWord + lane] = 1u;

    const uint base = kProceduralHitLogWord + lane * kProceduralHitLogWords;
    Output[base + kProcHitMarker] = attributes.marker;
    Output[base + kProcHitAttributeDistance] = asuint(attributes.distance);
    Output[base + kProcHitRayTCurrent] = asuint(RayTCurrent());
    Output[base + kProcHitKind] = HitKind();
    Output[base + kProcHitPrimitiveIndex] = PrimitiveIndex();
    Output[base + kProcHitInstanceID] = InstanceID();
    Output[base + kProcHitAnyHitTag] = payload.anyHitTag;
    Output[base + kProcHitRejectTag] = payload.rejectTag;

    payload.kind = 1u;
    payload.primitive = PrimitiveIndex();
    payload.instance = InstanceID();
    payload.hitT = RayTCurrent();
    payload.localRoot = localTag;
    // The barycentric words are deliberately left at the zeroes the ray generation shader
    // initialized: a procedural hit has no barycentrics, and a runtime that filled them in
    // would be reporting triangle attributes for a procedural hit.
}

// The procedural dispatch's own miss shader, selected by the miss index the ray generation
// shader passes. It is a separate export from the triangle miss shader so the two dispatches'
// logs stay disjoint and each dispatch's miss is observable on its own.
[shader("miss")]
void ProceduralMiss(inout RayPayload payload)
{
    Output[kProcMissLogWord + payload.lane] = 1u;
    payload.kind = 2u;
    payload.hitT = 0.0f;
}

// One lane's payload, written to the output region its dispatch owns. Both ray generation
// shaders use it, so the two dispatches' payload layouts cannot drift apart, and every word is
// stored unconditionally: a field no hit shader wrote is still read back and asserted.
void storePayload(uint base, in RayPayload payload)
{
    Output[base + 0u] = payload.kind;
    Output[base + 1u] = payload.primitive;
    Output[base + 2u] = payload.instance;
    Output[base + 3u] = asuint(payload.hitT);
    Output[base + 4u] = payload.callableValue;
    Output[base + 5u] = payload.localRoot;
    Output[base + 6u] = payload.lane;
    Output[base + 7u] = asuint(payload.baryU);
    Output[base + 8u] = asuint(payload.baryV);
    Output[base + 9u] = payload.dimensions;
    Output[base + 10u] = payload.depth;
    Output[base + 11u] = payload.recurse;
    Output[base + 12u] = payload.nestedMiss;
    Output[base + 13u] = payload.anyHitTag;
    Output[base + 14u] = payload.rejectTag;
}

[shader("raygeneration")]
void RayGen()
{
    const uint lane = DispatchRaysIndex().x;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    // A wrong dispatch extent leaves this lane's output at its fill pattern, so the
    // fixture sees a mismatch instead of a plausible-looking result.
    if (lane >= dimensions.x)
        return;

    RayPayload payload;
    payload.kind = 0u;
    payload.primitive = 0xffffffffu;
    payload.instance = 0xffffffffu;
    payload.hitT = 0.0f;
    payload.callableValue = 0u;
    payload.localRoot = 0u;
    payload.lane = lane;
    payload.baryU = 0.0f;
    payload.baryV = 0.0f;
    payload.dimensions = dimensions.x;
    payload.depth = 0u;
    payload.recurse = 0u;
    payload.nestedMiss = 0u;
    payload.anyHitTag = 0u;
    payload.rejectTag = 0u;

    const float3 direction = float3(0.0f, 0.0f, 1.0f);
    float3 origin;
    uint rayContributionToHitGroupIndex = 0u;
    uint rayFlags = 0u;

    if (lane == 0u)
    {
        // Inside triangle 0 only (0,0)-(1,0)-(0,1); the any-hit accepts.
        origin = float3(0.25f, 0.25f, -1.0f);
    }
    else if (lane == 1u)
    {
        // Outside every triangle: the miss shader runs on a pristine payload.
        origin = float3(10.0f, 10.0f, -1.0f);
    }
    else if (lane == 2u)
    {
        // Inside triangle 1 only (2,0)-(3,0)-(2,1); the any-hit rejects it, so traversal
        // continues, finds nothing else and ends in the miss shader.
        origin = float3(2.25f, 0.25f, -1.0f);
    }
    else if (lane == 3u)
    {
        // A ray that meets TWO candidates: triangle 0 at t=1 and triangle 2 at t=7
        // ((0.1,0.1) is inside both). The any-hit rejects triangle 0 and accepts triangle 2,
        // so the accepted invocation's payload writes must be what reaches the closest hit,
        // whichever order the candidates were enumerated in.
        origin = float3(0.1f, 0.1f, -1.0f);
    }
    else if (lane == 4u)
    {
        // Triangle 0 with RAY_FLAG_SKIP_CLOSEST_HIT_SHADER (0x8, not 0x100 - that is
        // RAY_FLAG_SKIP_TRIANGLES). DXR defines this flag as skipping execution of the
        // closest hit shader only: the hit is still committed and the any hit still runs, so
        // its payload writes survive, while neither the closest hit nor the miss shader runs
        // and kind therefore stays 0.
        origin = float3(0.25f, 0.25f, -1.0f);
        rayFlags = kRayFlagSkipClosestHitShader;
    }
    else if (lane == 5u)
    {
        // Triangle 0 again, but through hit-group SBT record 1 so a different local
        // root constant is read. The callable runs first.
        origin = float3(0.5f, 0.25f, -1.0f);
        rayContributionToHitGroupIndex = 1u;
    }
    else if (lane == 6u)
    {
        // Triangle 0 with the nested trace enabled from the closest hit.
        origin = float3(0.25f, 0.25f, -1.0f);
        payload.recurse = 1u;
    }
    else if (lane == 7u)
    {
        // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH (0x4) on the two-candidate ray with no
        // rejection: traversal must stop at the first candidate the any-hit accepts, so the
        // OTHER candidate is never visited (its any-hit log slot stays at the fill pattern)
        // and the committed hit is the accepted one. DXR does not define which candidate is
        // enumerated first, so the fixture accepts either committed triangle instead of
        // assuming a near/far report order.
        origin = float3(0.1f, 0.1f, -1.0f);
        rayFlags = kRayFlagAcceptFirstHitAndEndSearch;
    }
    else if (lane == 8u)
    {
        // The same early termination driven from inside the shader by
        // AcceptHitAndEndSearch() instead of by the ray flag.
        origin = float3(0.1f, 0.1f, -1.0f);
    }
    else
    {
        // Lane 9: RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH together with an any-hit that
        // rejects triangle 0. IgnoreHit is the documented exception to the flag: the search
        // continues, so triangle 0 must not be committed and triangle 2 must be.
        origin = float3(0.1f, 0.1f, -1.0f);
        rayFlags = kRayFlagAcceptFirstHitAndEndSearch;
    }

    if (lane == 5u)
        CallShader(0u, payload);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001f;
    ray.TMax = 10.0f;
    TraceRay(Scene, rayFlags, 0xffu, rayContributionToHitGroupIndex, 1u, 0u, ray, payload);

    storePayload(kPayloadBase + lane * kPayloadWords, payload);
}

// ---------------------------------------------------------------------------------------
// Procedural dispatch ray generation
// ---------------------------------------------------------------------------------------
// The procedural coverage is a DispatchRays of its own through its own ray generation and miss
// exports, so the triangle dispatch's ten lanes are untouched by it: the two dispatches write
// disjoint regions of the output buffer and share only the hit-group table.
//
// The ray is the same axis-aligned +z ray for every procedural lane and its origin differs only
// in x, so which AABB the traversal enumerates is decided by the geometry alone. Each lane's
// origin sits a quarter unit off the centre of exactly one AABB, inside that AABB's x/y extent
// and outside every other one, so a lane can only ever be handed its own primitive.
[shader("raygeneration")]
void ProceduralRayGen()
{
    const uint lane = DispatchRaysIndex().x;
    const uint2 dimensions = DispatchRaysDimensions().xy;
    if (lane >= dimensions.x || lane >= kProceduralLaneCount)
        return;

    RayPayload payload;
    payload.kind = 0u;
    payload.primitive = 0xffffffffu;
    payload.instance = 0xffffffffu;
    payload.hitT = 0.0f;
    payload.callableValue = 0u;
    payload.localRoot = 0u;
    payload.lane = lane;
    // Left at zero on purpose: a procedural hit has no barycentrics, and the fixture asserts
    // that these words are still zero, so a runtime that filled them in from a triangle path
    // cannot pass.
    payload.baryU = 0.0f;
    payload.baryV = 0.0f;
    payload.dimensions = dimensions.x;
    payload.depth = 0u;
    payload.recurse = 0u;
    payload.nestedMiss = 0u;
    payload.anyHitTag = 0u;
    payload.rejectTag = 0u;

    RayDesc ray;
    ray.Origin = float3(kAabbBaseX + lane * kAabbSpacingX + kProceduralRayOffsetX, kProceduralRayY,
                        kProceduralRayZ);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = kProceduralRayTMin;
    ray.TMax = kProceduralRayTMax;
    // Hit group index 0 here; the instance's own NONZERO
    // InstanceContributionToHitGroupIndex is what moves the selection onto the procedural record,
    // so the two contributions are separately observable. Miss index 1 selects the procedural
    // dispatch's own miss shader.
    TraceRay(Scene, 0u, 0xffu, 0u, 1u, 1u, ray, payload);

    storePayload(kProceduralPayloadBase + lane * kPayloadWords, payload);
}
