# DLSS scene freeze and DXMT/FEX performance research
Started: 2026-10-09T07:50:42Z

## Goal
Investigate the user's latest ZZZ execution logs: HUD character position continues updating while the DLSS scene is frozen. Explain verified causes versus hypotheses, and identify improvements for low FPS versus D3DMetal and Rosetta using this run and static analysis. No production edits, live-prefix changes, new game launch, or commits.
Ideal end state: latest run identified; failure stage supported by exact runtime evidence and source contracts; ranked performance recommendations with measurements and explicit limits, not invented speedups.
Tier: HEAVY, explicit exhaustive investigation and concurrency/cache/temporal resource analysis.
Stop when all research axes and expansion leads are closed, contested claims are verified or explicitly unresolved, and the cited Korean report is delivered.

## Skills and topology
ulw-research: explicit research workflow. debugging: read-only causal investigation. Native binary references apply to binary proof lanes. No implementation skill workflow. Lead owns log identity, executed quantitative verification, journal and synthesis. Nine first-wave read-only lanes cover distinct code/evidence axes; a fresh refinement lane attacks combined claims. No worker edits products or journals, and no nested delegation.

## Axes and assigned lanes
1. LatestLogs (default): latest runtime log timeline and error frequencies.
2. NgxBoundary (default): NGX parse/feature/parameter API and temporal call boundaries.
3. MetalTemporal (default): native scaler output, resource lifetime, texture aliasing and reset.
4. QueuePresent (default): D3D12 queue submission, fence and presentation stalls.
5. RenderPerformance (default): command encoding, barriers, descriptors and CPU/GPU performance candidates.
6. ShaderPerformance (scout): shader/cache/PSO compilation costs and past fixes.
7. FexComparison (default): FEX/Rosetta observed profiles and static EC/JIT costs, read-only sibling repository.
8. EvidenceSkeptic (reviewer): historical evidence limits, alternative explanations, invalid performance comparisons.
9. RuntimeIdentity (scout): latest deployed runtime/config versus verified candidate and D3DMetal references.
All sources are local code, shipped headers, runtime logs and retained profiles. External web/social/browsing research: no, not required to attribute this local failure. Document render only may use browser lane later.

## Plan
1. Identify latest game log and deployed runtime; capture bounded hashes and settings.
2. Run all nine independent read-only axes in one wave; journal claims and leads.
3. Expand resource-flow and performance leads in two successive evidence-driven waves; execute log arithmetic/static proofs and consult pristine GPTK if contract ambiguity requires it.
4. Fresh refinement debate locks claims; write cited Korean findings and recommendations.
5. Verify report citations and evidence, record unresolved gaps, complete delivery manifest and clean owned resources.

## Success criteria + exact scenarios
C1: read latest game_1791530994266.log, ZenlessZoneZero_d3d12.log and runtime manifest under /Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test; verify timestamp, selected runtime and failure stage with exact quoted lines. PASS means run identity is explicit and no historical success is substituted for this run.
C2: execute a local evidence analysis script against captured logs, trace NGX -> temporal command -> native MetalFX in source and compare old successful logs. PASS means causal claims name runtime support and unsupported possibilities remain hypotheses.
C3: execute frame/error statistics from available logs and recheck retained FEX attribution inputs; analyze render and compiler hot paths. PASS means improvements cite concrete costs or static mechanisms and no controlled FPS ratio is claimed without matching samples.
C4: reviewer attacks all supported claims; primary-source readback and report-tools outcome verify pass. Evidence: wave reports, verification output, claim graph and SYNTHESIS.md.

## Deliverable
Lane: no-format (answered_by: request).
State: new (answered_by: request).
Formats: markdown (answered_by: default).
Destination: repository research evidence plus chat answer (answered_by: request).
Audience: project owner (answered_by: request).
Template: none (answered_by: default).
Format description: Korean engineering findings with source paths, ranked fix candidates and unresolved limits; no PDF/DOCX requested (answered_by: request).

## Initial evidence
HEAD f04bd8a. No tracked edits initially; pre-existing fg-*.bin untracked captures are preserved. Prior candidate-03 verified one DLSS scene, not sustained output or FPS. Latest launcher delivery explicitly did not launch the game. Latest live log identified as game_1791530994266.log; live D3D12 log about 1 MiB.
