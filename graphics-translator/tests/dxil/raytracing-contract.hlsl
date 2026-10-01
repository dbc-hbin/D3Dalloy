// DXR contract library for tests/owned-d3d12-raytracing.cpp (the "contract" cases).
//
// Rebuild with upstream Microsoft DXC 1.9 (see build-raytracing.sh):
//   dxc -T lib_6_5 -HV 2021 -Fo raytracing-contract.dxil raytracing-contract.hlsl
//
// One generic ray generation shader traces one ray per lane, with every TraceRay operand taken
// from the lane's LaneRay record, so each contract case is a CPU-built acceleration structure plus
// a table of rays. The hit shaders write every system value of the committed hit into the lane's
// own 64-word output slot, so a case asserts exact values rather than "something was hit".
//
// Output slot of lane L: words [L * 64, L * 64 + 64)
//   0  kind            1 closest hit (triangle hit group), 3 closest hit (procedural), 2 miss
//   1  PrimitiveIndex   2 InstanceID   3 InstanceIndex   4 GeometryIndex   5 HitKind
//   6  RayTCurrent      7 RayFlags     8 RayTMin
//   9..11  WorldRayOrigin      12..14 WorldRayDirection
//   15..17 ObjectRayOrigin     18..20 ObjectRayDirection
//   21..32 ObjectToWorld3x4 [r][c] at 21 + r*4 + c
//   33..44 WorldToObject3x4 [r][c] at 33 + r*4 + c
//   45,46  barycentrics / reported attributes
//   47     local root constant of the record that ran (hit group or miss)
//   48     any-hit invocation count (atomic)
//   49..60 ObjectToWorld4x3 [r][c] at 49 + r*3 + c
//   62     0x100 | payload.kind as the ray generation shader read it back after TraceRay
//
// Intersection log of the procedural instance with InstanceID I: words [3072 + I*4, +4)
//   0 invocation count (atomic), 1..3 WorldRayOrigin observed inside the intersection shader

struct Payload
{
    uint lane;
    uint kind;
};

// 64 bytes, filled by the fixture.
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
    uint scene;     // 0 Scene, 1 EmptyScene (a TLAS with NumDescs 0), 2 NullScene (GPUVA 0)
    uint pad1;
    uint pad2;
};

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<LaneRay> Rays : register(t1);
RaytracingAccelerationStructure EmptyScene : register(t2);
// D3D12 defines a NULL acceleration structure (Location 0): every ray traced against it misses.
RaytracingAccelerationStructure NullScene : register(t3);
RWStructuredBuffer<uint> Out : register(u0);

// One root constant in every hit-group and miss record, after the 32-byte identifier.
cbuffer LocalConstants : register(b0, space1)
{
    uint localTag;
};

static const uint kSlotWords = 64u;

static const uint kAnyHitCountWord = 48u;
static const uint kIntersectionLogWord = 3072u;

void storeFloat3(uint base, float3 value)
{
    Out[base + 0u] = asuint(value.x);
    Out[base + 1u] = asuint(value.y);
    Out[base + 2u] = asuint(value.z);
}

void storeHit(uint lane, uint kind, float2 attributes)
{
    const uint base = lane * kSlotWords;
    Out[base + 0u] = kind;
    Out[base + 1u] = PrimitiveIndex();
    Out[base + 2u] = InstanceID();
    Out[base + 3u] = InstanceIndex();
    Out[base + 4u] = GeometryIndex();
    Out[base + 5u] = HitKind();
    Out[base + 6u] = asuint(RayTCurrent());
    Out[base + 7u] = RayFlags();
    Out[base + 8u] = asuint(RayTMin());
    storeFloat3(base + 9u, WorldRayOrigin());
    storeFloat3(base + 12u, WorldRayDirection());
    storeFloat3(base + 15u, ObjectRayOrigin());
    storeFloat3(base + 18u, ObjectRayDirection());
    const float3x4 objectToWorld = ObjectToWorld3x4();
    const float3x4 worldToObject = WorldToObject3x4();
    const float4x3 objectToWorld43 = ObjectToWorld4x3();
    [unroll] for (uint r = 0u; r < 3u; ++r)
    {
        [unroll] for (uint c = 0u; c < 4u; ++c)
        {
            Out[base + 21u + r * 4u + c] = asuint(objectToWorld[r][c]);
            Out[base + 33u + r * 4u + c] = asuint(worldToObject[r][c]);
        }
    }
    [unroll] for (uint r4 = 0u; r4 < 4u; ++r4)
    {
        [unroll] for (uint c3 = 0u; c3 < 3u; ++c3)
            Out[base + 49u + r4 * 3u + c3] = asuint(objectToWorld43[r4][c3]);
    }
    Out[base + 45u] = asuint(attributes.x);
    Out[base + 46u] = asuint(attributes.y);
    Out[base + 47u] = localTag;
}

[shader("raygeneration")]
void ContractRayGen()
{
    const uint lane = DispatchRaysIndex().x;
    const LaneRay source = Rays[lane];
    RayDesc ray;
    ray.Origin = source.origin;
    ray.TMin = source.tmin;
    ray.Direction = source.direction;
    ray.TMax = source.tmax;
    Payload payload;
    payload.lane = lane;
    payload.kind = 0u;
    if (source.scene == 1u)
        TraceRay(EmptyScene, source.flags, source.mask, source.contribution, source.multiplier,
                 source.missIndex, ray, payload);
    else if (source.scene == 2u)
        TraceRay(NullScene, source.flags, source.mask, source.contribution, source.multiplier,
                 source.missIndex, ray, payload);
    else
        TraceRay(Scene, source.flags, source.mask, source.contribution, source.multiplier,
                 source.missIndex, ray, payload);
    Out[lane * kSlotWords + 62u] = 0x100u | payload.kind;
}

[shader("closesthit")]
void ContractTriangleClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    storeHit(payload.lane, 1u, attributes.barycentrics);
    payload.kind = 1u;
}

[shader("closesthit")]
void ContractProceduralClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    storeHit(payload.lane, 3u, attributes.barycentrics);
    payload.kind = 3u;
}

[shader("anyhit")]
void ContractAnyHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    uint previous;
    InterlockedAdd(Out[payload.lane * kSlotWords + kAnyHitCountWord], 1u, previous);
}

// Hit kind every procedural report carries.
static const uint kProceduralHitKind = 5u;

// The procedural primitive is the unit box [0,1]^3 in OBJECT space; the fixture's AABB is that
// same box. The slab test runs on ObjectRayOrigin/ObjectRayDirection, so the reported t (which
// D3D12 defines in the parametric space shared by the world and the object ray) is only right when
// the object-space ray really is the instance-transformed one.
[shader("intersection")]
void ContractIntersection()
{
    // The payload is not visible to an intersection shader, so its evidence is indexed by the
    // instance's own InstanceID: every procedural instance the fixture builds has a distinct one.
    const uint log = kIntersectionLogWord + InstanceID() * 4u;
    uint previous;
    InterlockedAdd(Out[log + 0u], 1u, previous);
    storeFloat3(log + 1u, WorldRayOrigin());

    const float3 origin = ObjectRayOrigin();
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
    attributes.barycentrics = float2(0.125f, 0.625f);
    ReportHit(entry, kProceduralHitKind, attributes);
}

[shader("miss")]
void ContractMiss(inout Payload payload)
{
    const uint base = payload.lane * kSlotWords;
    Out[base + 0u] = 2u;
    Out[base + 7u] = RayFlags();
    Out[base + 47u] = localTag;
    payload.kind = 2u;
}
