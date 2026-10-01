// DXR coverage library for tests/owned-d3d12-raytracing.cpp (the "coverage" cases of
// owned-d3d12-raytracing-coverage.inc).
//
// Rebuild with upstream Microsoft DXC 1.9 (see build-raytracing-coverage.sh):
//   dxc -T lib_6_5 -HV 2021 -Fo raytracing-coverage.dxil raytracing-coverage.hlsl
//
// Every export runs with the same local root signature (built through the API):
//   RootConstants(num32BitConstants = 1, b0, space = 1), DescriptorTable(SRV(t0, space = 1))
// so each shader record carries a root constant tag and a descriptor table whose structured
// buffer holds one word of record data. Both are written into the lane's slot by the shader that
// ran, so the record the runtime resolved (table base, stride, index) is observable.
//
// The lane is the linearized DispatchRaysIndex over DispatchRaysDimensions, computed in every
// stage (any-hit and intersection included), so a 3D grid's lanes never alias.
//
// Output slot of lane L: words [L * 32, L * 32 + 32)
//   0..2  DispatchRaysIndex   3..5 DispatchRaysDimensions   (ray generation)
//   6     ray generation record tag   7 its record data
//   8     ray generation CallShader accumulator   9 its data sum
//   10    kind: 1 triangle closest hit, 3 procedural closest hit, 2 miss
//   11    hit-group / miss record tag  12 its record data
//   13    GeometryIndex   14 PrimitiveIndex   15 InstanceID           (closest hit)
//   16    any-hit invocation count (InterlockedAdd onto the 0xffffffff fill)
//   17    0xA00 | GeometryIndex() seen by the any-hit shader
//   18    0xB00 | GeometryIndex() seen by the intersection shader
//   19    closest-hit / miss CallShader accumulator   20 its data sum
//   21    0x100 | payload.kind as the ray generation shader read it back after TraceRay
//   22..25 inline RayQuery result (see runQuery)

struct Payload
{
    uint lane;
    uint kind;
};

// Callable parameter: a chain of `remaining + 1` callables starting at table index `index`; every
// invocation folds its record's tag into `acc` and its record data into `data`, then calls the
// next callable record ((own index + 1) % 3) while `remaining` is non-zero - CallShader recursion
// through the same export.
struct CallParams
{
    uint index;
    uint remaining;
    uint acc;
    uint data;
};

// 64 bytes, filled by the fixture. A call word is 0 (no call) or 0x80000000 | remaining << 8 | index.
struct LaneRay
{
    float3 origin;
    float tmin;
    float3 direction;
    float tmax;
    uint flags;
    uint mask;
    uint contribution;
    uint multiplier;
    uint missIndex;
    uint callRayGen;
    uint callHit;
    uint query;     // 0 none, 1 RayQuery<RAY_FLAG_SKIP_TRIANGLES>, 2 RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>
};

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<LaneRay> Rays : register(t1);
RWStructuredBuffer<uint> Out : register(u0);

cbuffer LocalConstants : register(b0, space1)
{
    uint localTag;
};
StructuredBuffer<uint> LocalData : register(t0, space1);

static const uint kSlotWords = 32u;
static const uint kCallEnabled = 0x80000000u;

uint laneIndex()
{
    const uint3 index = DispatchRaysIndex();
    const uint3 dims = DispatchRaysDimensions();
    return index.x + dims.x * (index.y + dims.y * index.z);
}

CallParams callChain(uint word)
{
    CallParams params;
    params.index = word & 0xffu;
    params.remaining = (word >> 8) & 0xffu;
    params.acc = 0u;
    params.data = 0u;
    CallShader(params.index, params);
    return params;
}

// Inline query over the same ray with the template flags of `q`: procedural candidates are committed
// at t 3 (the procedural boxes' entry distance), non-opaque triangle candidates are committed.
//   22 0x300 | CommittedStatus   23 CommittedGeometryIndex (0xff: nothing)   24 RayFlags()
//   25 candidates seen
template <uint Flags>
void runQuery(RayQuery<Flags> q, RayDesc ray, uint base)
{
    q.TraceRayInline(Scene, 0u, 0xffu, ray);
    uint candidates = 0u;
    while (q.Proceed())
    {
        ++candidates;
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
            q.CommitProceduralPrimitiveHit(3.0f);
        else
            q.CommitNonOpaqueTriangleHit();
    }
    Out[base + 22u] = 0x300u | q.CommittedStatus();
    Out[base + 23u] = q.CommittedStatus() == COMMITTED_NOTHING ? 0xffu : q.CommittedGeometryIndex();
    Out[base + 24u] = q.RayFlags();
    Out[base + 25u] = candidates;
}

[shader("raygeneration")]
void CvRayGen()
{
    const uint lane = laneIndex();
    const uint base = lane * kSlotWords;
    const uint3 index = DispatchRaysIndex();
    const uint3 dims = DispatchRaysDimensions();
    Out[base + 0u] = index.x;
    Out[base + 1u] = index.y;
    Out[base + 2u] = index.z;
    Out[base + 3u] = dims.x;
    Out[base + 4u] = dims.y;
    Out[base + 5u] = dims.z;
    Out[base + 6u] = localTag;
    Out[base + 7u] = LocalData[0];
    const LaneRay source = Rays[lane];
    if (source.callRayGen & kCallEnabled)
    {
        const CallParams params = callChain(source.callRayGen);
        Out[base + 8u] = params.acc;
        Out[base + 9u] = params.data;
    }
    RayDesc ray;
    ray.Origin = source.origin;
    ray.TMin = source.tmin;
    ray.Direction = source.direction;
    ray.TMax = source.tmax;
    Payload payload;
    payload.lane = lane;
    payload.kind = 0u;
    TraceRay(Scene, source.flags, source.mask, source.contribution, source.multiplier, source.missIndex,
             ray, payload);
    Out[base + 21u] = 0x100u | payload.kind;
    if (source.query == 1u)
    {
        RayQuery<RAY_FLAG_SKIP_TRIANGLES> q;
        runQuery(q, ray, base);
    }
    else if (source.query == 2u)
    {
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        runQuery(q, ray, base);
    }
}

void storeHitOrMiss(uint lane, uint kind)
{
    const uint base = lane * kSlotWords;
    Out[base + 10u] = kind;
    Out[base + 11u] = localTag;
    Out[base + 12u] = LocalData[0];
    const LaneRay source = Rays[lane];
    if (source.callHit & kCallEnabled)
    {
        const CallParams params = callChain(source.callHit);
        Out[base + 19u] = params.acc;
        Out[base + 20u] = params.data;
    }
}

[shader("closesthit")]
void CvClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    const uint kind = HitKind() == 5u ? 3u : 1u;
    const uint base = payload.lane * kSlotWords;
    Out[base + 13u] = GeometryIndex();
    Out[base + 14u] = PrimitiveIndex();
    Out[base + 15u] = InstanceID();
    storeHitOrMiss(payload.lane, kind);
    payload.kind = kind;
}

[shader("anyhit")]
void CvAnyHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    const uint base = laneIndex() * kSlotWords;
    uint previous;
    InterlockedAdd(Out[base + 16u], 1u, previous);
    Out[base + 17u] = 0xA00u | GeometryIndex();
}

// The procedural BLAS holds one AABB per geometry, geometry g being the unit box at object-space
// x 2g. The slab test uses that box, so a wrong GeometryIndex() here makes the ray miss.
[shader("intersection")]
void CvIntersection()
{
    const uint geometry = GeometryIndex();
    Out[laneIndex() * kSlotWords + 18u] = 0xB00u | geometry;
    const float3 lower = float3(2.0f * geometry, 0.0f, 0.0f);
    const float3 origin = ObjectRayOrigin() - lower;
    const float3 direction = ObjectRayDirection();
    float entry = RayTMin();
    float exit = RayTCurrent();
    [unroll] for (uint axis = 0u; axis < 3u; ++axis)
    {
        if (direction[axis] == 0.0f)
        {
            if (origin[axis] < 0.0f || origin[axis] > 1.0f)
                return;
            continue;
        }
        const float inverse = 1.0f / direction[axis];
        const float t0 = (0.0f - origin[axis]) * inverse;
        const float t1 = (1.0f - origin[axis]) * inverse;
        entry = max(entry, min(t0, t1));
        exit = min(exit, max(t0, t1));
    }
    if (entry > exit)
        return;
    BuiltInTriangleIntersectionAttributes attributes;
    attributes.barycentrics = float2(0.0f, 0.0f);
    ReportHit(entry, 5u, attributes);
}

[shader("miss")]
void CvMiss(inout Payload payload)
{
    storeHitOrMiss(payload.lane, 2u);
    payload.kind = 2u;
}

[shader("callable")]
void CvCallable(inout CallParams params)
{
    const uint own = localTag & 0xfu;
    params.acc = params.acc * 16u + own;
    params.data += LocalData[0];
    if (params.remaining != 0u)
    {
        params.remaining -= 1u;
        CallShader((own + 1u) % 3u, params);
    }
}
