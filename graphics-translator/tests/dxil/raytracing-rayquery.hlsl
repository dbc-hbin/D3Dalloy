// Inline raytracing (RayQuery, DXR Tier 1.1) fixture for tests/owned-d3d12-raytracing.cpp
// (tests/owned-d3d12-raytracing-rayquery.inc).
//
// Rebuild with upstream Microsoft DXC 1.9 under Wine (see build-raytracing-rayquery.sh):
//   cs_rayquery    cs_6_5   one lane per thread
//   vs_rayquery    vs_6_5   one lane per vertex (SV_VertexID), point list, no rasterized output
//   ps_rayquery    ps_6_5   one lane per pixel (SV_Position.x), 1-row render target, drawn by
//   vs_fullscreen  vs_6_5   (raytracing-rayquery-fs.dxil)
//   lib_rayquery   lib_6_5  RQRayGen: one lane per DispatchRaysIndex().x
//   cs_rays_args   cs_6_5   (raytracing-rayquery-args.dxil) GPU-written ExecuteIndirect DISPATCH_RAYS
//                           arguments + count of the raygen-indirect case, traces nothing
//
// Every stage runs the same `traceLane`: a RayQuery traced with the lane's own record, a Proceed loop
// that records the first candidate's getters and commits / ignores / aborts per the lane's mode, and
// the committed getters afterwards. The output slot of lane L is words [L * 64, L * 64 + 64):
//   0  candidate count            1  first candidate type       2  candidate instance index
//   3  candidate instance id      4  candidate contribution     5  candidate geometry index
//   6  candidate primitive index  7  candidate procedural non-opaque (procedural) / front face (tri)
//   8  candidate triangle RayT (triangle candidates only)
//   9..11 candidate object ray origin    12..14 candidate object ray direction
//   15..26 candidate ObjectToWorld3x4    27..28 candidate barycentrics (triangle)
//   29 committed status   30 committed instance index  31 committed instance id
//   32 committed contribution            33 committed geometry   34 committed primitive
//   35 committed front face              36 committed RayT
//   37..39 committed object ray origin   40..42 committed object ray direction
//   43..54 committed ObjectToWorld3x4    55..56 committed barycentrics
//   57 RayFlags  58 RayTMin  59..61 WorldRayOrigin  62 committed WorldToObject3x4[0][3]
//   63 0x5100 | stage tag (written last); bit 31 set when a 4x3 getter is not the exact transpose
//      of its 3x4 form (compared in-shader for Candidate/Committed ObjectToWorld and WorldToObject)

struct LaneRay {
  float3 origin;
  float tmin;
  float3 direction;
  float tmax;
  uint flags;
  uint mask;
  uint mode;      // 0 commit every candidate, 1 commit none, 2 abort at the first candidate
  float commitT;  // CommitProceduralPrimitiveHit(t)
  uint scene;     // 0 Scene (descriptor table), 1 null structure, 2 root SRV scene
  uint pad[3];
};

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<LaneRay> Rays : register(t1);
RaytracingAccelerationStructure NullScene : register(t2);
RaytracingAccelerationStructure RootScene : register(t3);
RWStructuredBuffer<uint> Out : register(u0);

void store3(uint base, float3 v) {
  Out[base + 0] = asuint(v.x);
  Out[base + 1] = asuint(v.y);
  Out[base + 2] = asuint(v.z);
}

void store3x4(uint base, float3x4 m) {
  [unroll] for (uint r = 0; r < 3; ++r)
    [unroll] for (uint c = 0; c < 4; ++c)
      Out[base + r * 4 + c] = asuint(m[r][c]);
}

bool same(float3x4 a, float4x3 b) {
  bool ok = true;
  [unroll] for (uint r = 0; r < 3; ++r)
    [unroll] for (uint c = 0; c < 4; ++c)
      ok = ok && asuint(a[r][c]) == asuint(b[c][r]);
  return ok;
}

template <typename Q> uint recordCandidate(inout Q q, uint slot) {
  uint bad = 0;
  const uint type = q.CandidateType();
  Out[slot + 1] = type;
  Out[slot + 2] = q.CandidateInstanceIndex();
  Out[slot + 3] = q.CandidateInstanceID();
  Out[slot + 4] = q.CandidateInstanceContributionToHitGroupIndex();
  Out[slot + 5] = q.CandidateGeometryIndex();
  Out[slot + 6] = q.CandidatePrimitiveIndex();
  if (type == CANDIDATE_PROCEDURAL_PRIMITIVE) {
    Out[slot + 7] = q.CandidateProceduralPrimitiveNonOpaque() ? 1 : 0;
  } else {
    Out[slot + 7] = q.CandidateTriangleFrontFace() ? 1 : 0;
    Out[slot + 8] = asuint(q.CandidateTriangleRayT());
    Out[slot + 27] = asuint(q.CandidateTriangleBarycentrics().x);
    Out[slot + 28] = asuint(q.CandidateTriangleBarycentrics().y);
  }
  store3(slot + 9, q.CandidateObjectRayOrigin());
  store3(slot + 12, q.CandidateObjectRayDirection());
  store3x4(slot + 15, q.CandidateObjectToWorld3x4());
  if (!same(q.CandidateObjectToWorld3x4(), q.CandidateObjectToWorld4x3()) ||
      !same(q.CandidateWorldToObject3x4(), q.CandidateWorldToObject4x3()))
    bad = 1;
  return bad;
}

template <typename Q> void runQuery(inout Q q, RaytracingAccelerationStructure scene, LaneRay lane, uint slot,
                                    uint tag) {
  RayDesc ray;
  ray.Origin = lane.origin;
  ray.Direction = lane.direction;
  ray.TMin = lane.tmin;
  ray.TMax = lane.tmax;
  q.TraceRayInline(scene, lane.flags, lane.mask, ray);
  uint count = 0;
  uint bad = 0;
  while (q.Proceed()) {
    if (count == 0)
      bad |= recordCandidate(q, slot);
    ++count;
    if (lane.mode == 2) {
      q.Abort();
      continue;
    }
    if (lane.mode == 1)
      continue;
    if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      q.CommitNonOpaqueTriangleHit();
      q.CommitNonOpaqueTriangleHit(); // subsequent calls have no effect
    } else {
      q.CommitProceduralPrimitiveHit(lane.commitT);
    }
  }
  Out[slot + 0] = count;
  const uint status = q.CommittedStatus();
  Out[slot + 29] = status;
  Out[slot + 36] = asuint(q.CommittedRayT());
  if (status != COMMITTED_NOTHING) {
    Out[slot + 30] = q.CommittedInstanceIndex();
    Out[slot + 31] = q.CommittedInstanceID();
    Out[slot + 32] = q.CommittedInstanceContributionToHitGroupIndex();
    Out[slot + 33] = q.CommittedGeometryIndex();
    Out[slot + 34] = q.CommittedPrimitiveIndex();
    store3(slot + 37, q.CommittedObjectRayOrigin());
    store3(slot + 40, q.CommittedObjectRayDirection());
    store3x4(slot + 43, q.CommittedObjectToWorld3x4());
    Out[slot + 62] = asuint(q.CommittedWorldToObject3x4()[0][3]);
    if (!same(q.CommittedObjectToWorld3x4(), q.CommittedObjectToWorld4x3()) ||
        !same(q.CommittedWorldToObject3x4(), q.CommittedWorldToObject4x3()))
      bad = 1;
  }
  if (status == COMMITTED_TRIANGLE_HIT) {
    Out[slot + 35] = q.CommittedTriangleFrontFace() ? 1 : 0;
    Out[slot + 55] = asuint(q.CommittedTriangleBarycentrics().x);
    Out[slot + 56] = asuint(q.CommittedTriangleBarycentrics().y);
  }
  Out[slot + 57] = q.RayFlags();
  Out[slot + 58] = asuint(q.RayTMin());
  store3(slot + 59, q.WorldRayOrigin());
  Out[slot + 63] = 0x5100u | tag | (bad << 31);
}

void traceLane(uint lane, uint tag) {
  const LaneRay ray = Rays[lane];
  const uint slot = lane * 64;
  if (ray.scene == 3) {
    // Template flags OR'd with the dynamic ones: RayFlags() reports both.
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    runQuery(q, Scene, ray, slot, tag);
  } else if (ray.scene == 4) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    runQuery(q, Scene, ray, slot, tag);
  } else {
    RayQuery<RAY_FLAG_NONE> q;
    if (ray.scene == 1)
      runQuery(q, NullScene, ray, slot, tag);
    else if (ray.scene == 2)
      runQuery(q, RootScene, ray, slot, tag);
    else
      runQuery(q, Scene, ray, slot, tag);
  }
}

#ifdef RQ_CS
[numthreads(8, 1, 1)]
void cs_rayquery(uint3 id : SV_DispatchThreadID) {
  traceLane(id.x, 1);
}
#endif

#ifdef RQ_VS
float4 vs_rayquery(uint id : SV_VertexID) : SV_Position {
  traceLane(id, 2);
  return float4(2.0f, 2.0f, 0.0f, 1.0f); // outside the viewport
}
#endif

#ifdef RQ_PS
// Full-screen triangle for the pixel case; it traces nothing itself.
float4 vs_fullscreen(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float4 ps_rayquery(float4 position : SV_Position) : SV_Target {
  traceLane(uint(position.x), 3);
  return 0.0f.xxxx;
}
#endif

#ifdef RQ_LIB
[shader("raygeneration")]
void RQRayGen() {
  traceLane(DispatchRaysIndex().x, 4);
}
#endif

#ifdef RQ_ARGS
// Writes the ExecuteIndirect(DISPATCH_RAYS) arguments of the raygen-indirect case on the GPU:
// four D3D12_DISPATCH_RAYS_DESC (104 bytes apart) and the count buffer word at byte 512.
//   slot 0  Width  8                                     runs lanes 0..7
//   slot 1  Width 24, MissShaderTable stride 32 size 0   invalid (absent table not all zero): runs nothing
//   slot 2  Width 16                                     runs lanes 0..15
//   slot 3  Width 24                                     past the count (3): runs nothing
struct ArgsConstants {
  uint recordLo;
  uint recordHi;
};
ConstantBuffer<ArgsConstants> ArgsCB : register(b0);
RWByteAddressBuffer Args : register(u1);

void writeDesc(uint slot, uint width, bool badMiss) {
  const uint base = slot * 104;
  Args.Store2(base + 0, uint2(ArgsCB.recordLo, ArgsCB.recordHi)); // RayGenerationShaderRecord.StartAddress
  Args.Store2(base + 8, uint2(32, 0));                             // RayGenerationShaderRecord.SizeInBytes
  for (uint offset = 16; offset < 88; offset += 8)                 // Miss, HitGroup, Callable: absent
    Args.Store2(base + offset, uint2(0, 0));
  if (badMiss)
    Args.Store2(base + 32, uint2(32, 0));                          // MissShaderTable.StrideInBytes
  Args.Store3(base + 88, uint3(width, 1, 1));
  Args.Store(base + 100, 0);
}

[numthreads(1, 1, 1)]
void cs_rays_args() {
  writeDesc(0, 8, false);
  writeDesc(1, 24, true);
  writeDesc(2, 16, false);
  writeDesc(3, 24, false);
  Args.Store(512, 3);
}
#endif
