# Shader lane audit and corrections
Lead read the original report and challenged unsupported calculations before synthesis.

1. Latest DXMT compile-error lines have no timestamps. Bracketing HUD/AppKit lines do not measure compilation duration, uniform failure rate, or fraction of session spent compiling. Reject356.572s as compile duration,25.61failures/s,85.1% session coverage and31.2publications/s. Worker acknowledged retraction. No claim that latest session never reached steady state survives.
2. File-count/page-size estimates do not establish exact cache bytes or publication rate. Root verify_evidence.py performs an exact census. Manifest+file size is not actual loaded-image proof; root hashes on-disk modules separately.
3. observe-run creation143 event362 says compile ns150220100 =150.2201ms, not15.02ms. Pipeline event366 ns1704065000 =1704.065ms. Root inspected lines716-722 and executes JSON parsing to preserve units. Broad claim3.53-21.16ms compile range is wrong for these five rows.
4. stages=(mesh?0:Vertex)|(use_fragment?Fragment:0) means fragment cache remains eligible for mesh with a fragment. Only object/mesh variants are outside that serialized shader-stage cache; whole-mesh-cache exclusion is false.
5. Native binary archive is not the only way to reduce a native PSO stall. Reuse, prewarm and asynchronous work scheduling are alternatives; archives do not guarantee eliminating all pipeline creation cost. Any prioritization is a candidate based on historical measurements, not proof of latest FPS cause.
6. Reject extrapolations converting fs_usage row frequency into per-publication latency, aggregate hypothetical syscall time or current FPS. Historical workload windows differ.
7. No absolute claim that no FPS series exists anywhere on disk. Scoped inspected logs lack a matched latest presented-frame/CPU/GPU series. Historical screenshots have isolated HUD samples, not a benchmark.

No source/test was edited to conceal a failure. These were research claims rejected by primary-source readback and units checks.
