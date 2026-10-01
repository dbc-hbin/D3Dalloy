// DXR state-object library for tests/owned-d3d12-raytracing.cpp (the "stateobj" cases).
//
// Rebuild with upstream Microsoft DXC 1.9 (build-raytracing-rayquery-stages.sh):
//   dxc -T lib_6_5 -HV 2021 -O0 -D RQ_NESTED_CALLBACKS \
//       -Fo raytracing-stateobj.dxil raytracing-stateobj.hlsl
// Nested queries keep the existing opacity/local-root/addition oracle on the
// kernel-side callback traversal path; without the define it uses native callbacks.
//
// The library carries its own subobjects (root signatures, associations, configs, hit groups),
// so a state object made of nothing but this DXIL library is a complete raytracing pipeline.
// Every shader writes what it observed into the lane's 32-word output slot.
//
// Output slot of lane L: words [L * 32, L * 32 + 32)
//   0  kind          1 triangle closest hit, 3 procedural closest hit, 2 miss, 4 raygen without trace
//   1  GeometryIndex   2 PrimitiveIndex   3 InstanceID   4 HitKind
//   5  RayFlags        6 RayTMin          7 RayTCurrent
//   8..10 WorldRayOrigin   11..13 WorldRayDirection
//   14 local root constant (b0, space1) of the record that ran
//   15 0x100 | payload.kind as the ray generation shader read it back after TraceRay
//   16 any-hit invocation count (InterlockedAdd onto the 0xffffffff pre-fill)

struct Payload
{
    uint lane;
    uint kind;
};

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
    uint pad0;
    uint pad1;
    uint pad2;
};

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<LaneRay> Rays : register(t1);
RWStructuredBuffer<uint> Out : register(u0);

#ifdef RQ_NESTED_CALLBACKS
#include "raytracing-rayquery-callback-contract.hlsl"
#endif

cbuffer LocalConstants : register(b0, space1)
{
    uint localTag;
};

// ---- Subobjects ---------------------------------------------------------------------------

GlobalRootSignature SoGlobalRS =
{
    "DescriptorTable(SRV(t0, numDescriptors = 2), UAV(u0))"
};

// The tag is a root constant ...
LocalRootSignature SoLocalRS =
{
    "RootConstants(num32BitConstants = 1, b0, space = 1)"
};

// ... or, for the exports associated with this one, a root CBV whose buffer holds the tag. The
// two layouts write different bytes into the shader record, so which one an export got is
// observable.
LocalRootSignature SoLocalRSCbv =
{
    "CBV(b0, space = 1)"
};

SubobjectToExportsAssociation SoLocalAssoc =
{
    "SoLocalRS", "SoHitGroup;SoProcGroup;SoMiss"
};

SubobjectToExportsAssociation SoCbvAssoc =
{
    "SoLocalRSCbv", "SoMissCbv"
};

// Kept separate so an export list without SoMissAdded can leave this association out too.
SubobjectToExportsAssociation SoAddedAssoc =
{
    "SoLocalRS", "SoMissAdded;SoRayGenNoTrace"
};

RaytracingShaderConfig SoShaderConfig =
{
    8, // max payload size
    8  // max attribute size
};

RaytracingPipelineConfig SoPipelineConfig =
{
    1 // max trace recursion depth
};

TriangleHitGroup SoHitGroup =
{
    "SoAnyHit",    // AnyHit
    "SoClosestHit" // ClosestHit
};

ProceduralPrimitiveHitGroup SoProcGroup =
{
    "",                 // AnyHit
    "SoProcClosestHit", // ClosestHit
    "SoIntersection"    // Intersection
};

StateObjectConfig SoConfig =
{
    STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS
};

// ---- Shaders ------------------------------------------------------------------------------

static const uint kSlotWords = 32u;

void storeFloat3(uint base, float3 value)
{
    Out[base + 0u] = asuint(value.x);
    Out[base + 1u] = asuint(value.y);
    Out[base + 2u] = asuint(value.z);
}

void storeRay(uint base)
{
    Out[base + 5u] = RayFlags();
    Out[base + 6u] = asuint(RayTMin());
    Out[base + 7u] = asuint(RayTCurrent());
    storeFloat3(base + 8u, WorldRayOrigin());
    storeFloat3(base + 11u, WorldRayDirection());
    Out[base + 14u] = localTag;
}

[shader("raygeneration")]
void SoRayGen()
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
    TraceRay(Scene, source.flags, source.mask, source.contribution, source.multiplier, source.missIndex,
             ray, payload);
    Out[lane * kSlotWords + 15u] = 0x100u | payload.kind;
}

// A ray generation shader that never traces: the only kind a MaxTraceRecursionDepth 0 pipeline
// can run, since depth 0 disallows TraceRay altogether.
[shader("raygeneration")]
void SoRayGenNoTrace()
{
    const uint lane = DispatchRaysIndex().x;
    Out[lane * kSlotWords + 0u] = 4u;
    Out[lane * kSlotWords + 1u] = DispatchRaysDimensions().x;
}

[shader("closesthit")]
void SoClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    const uint base = payload.lane * kSlotWords;
    Out[base + 0u] = 1u;
    Out[base + 1u] = GeometryIndex();
    Out[base + 2u] = PrimitiveIndex();
    Out[base + 3u] = InstanceID();
    Out[base + 4u] = HitKind();
    storeRay(base);
    payload.kind = 1u;
}

[shader("anyhit")]
void SoAnyHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
#ifdef RQ_NESTED_CALLBACKS
    if (!callbackQueryAccepted()) return;
#endif
    uint previous;
    InterlockedAdd(Out[payload.lane * kSlotWords + 16u], 1u, previous);
}

[shader("closesthit")]
void SoProcClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    const uint base = payload.lane * kSlotWords;
    Out[base + 0u] = 3u;
    Out[base + 1u] = GeometryIndex();
    Out[base + 2u] = PrimitiveIndex();
    Out[base + 3u] = InstanceID();
    Out[base + 4u] = HitKind();
    storeRay(base);
    payload.kind = 3u;
}

// Unit box [0,1]^3 in object space; reports its entry distance with hit kind 5.
[shader("intersection")]
void SoIntersection()
{
#ifdef RQ_NESTED_CALLBACKS
    if (!callbackQueryAccepted()) return;
#endif
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
    attributes.barycentrics = float2(0.0f, 0.0f);
    ReportHit(entry, 5u, attributes);
}

[shader("miss")]
void SoMiss(inout Payload payload)
{
    const uint base = payload.lane * kSlotWords;
    Out[base + 0u] = 2u;
    storeRay(base);
    payload.kind = 2u;
}

[shader("miss")]
void SoMissCbv(inout Payload payload)
{
    const uint base = payload.lane * kSlotWords;
    Out[base + 0u] = 2u;
    storeRay(base);
    payload.kind = 2u;
}

// Added later through AddToStateObject: an export the base state object does not hold.
[shader("miss")]
void SoMissAdded(inout Payload payload)
{
    const uint base = payload.lane * kSlotWords;
    Out[base + 0u] = 5u;
    storeRay(base);
    payload.kind = 5u;
}
