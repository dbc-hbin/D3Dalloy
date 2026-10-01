// Keep the existing callback-control oracle on the kernel query-traversal path.
// A real accepted inline hit is required before the outer callback may change its
// payload, report attributes, or make an any-hit decision. Both triangle and box
// candidates are present in the fixture, and no callback executes for a miss.
bool callbackQueryAccepted()
{
    RayDesc ray;
    ray.Origin = WorldRayOrigin();
    ray.Direction = WorldRayDirection();
    ray.TMin = RayTMin();
    ray.TMax = 1000000.0f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(Scene, 0, 0xff, ray);
    while (query.Proceed())
        if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
            query.CommitProceduralPrimitiveHit(ray.TMin);
    return query.CommittedStatus() != COMMITTED_NOTHING;
}
