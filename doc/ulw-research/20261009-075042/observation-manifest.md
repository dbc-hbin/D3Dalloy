# Observation manifest
All observations made 2026-10-09; source-relative valid time is explicit. Local primary sources use a documented primary-only exception: two web domains cannot independently observe a private game run.

| observation_id | source / line anchor | evidence layer | observer group / independence | observer | observed_at | valid_at | artifact | quote / value | contamination |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| O1 | live neutralinojs.log:537-547,569-571 | launcher execution | launcher log, separate producer from DXMT | lead | 2026-10-09 | 16:25-16:36 KST | tool readback | optimized package staged; game launch16:29:54.266; wineserver wait16:36:53.152 | Launcher selection is not loaded DLL proof |
| O2 | live logs/game_1791530994266.log | game stdout/stderr | DXMT log sink | lead Python verifier | 2026-10-09 | latest run | verify-log-counts.md | 9131 compilation failure lines;1881 IA warnings | Duplicate messages, not unique shaders or frame attribution |
| O3 | live ZenlessZoneZero_d3d12.log | module log | same DXMT producer as O2, NOT independent | lead Python verifier | 2026-10-09 | latest run | verify-log-counts.md | 9122 compilation failure lines | Nine fewer PS-SRV lines; buffering or sink differences unresolved |
| O4 | build/local/zzz-fex-opt-20261009/candidate-03/ZenlessZoneZero_nvngx.log:1-39 | historical API trace | prior run, not this symptom | lead | 2026-10-09 | candidate-03 | original retained log | create1280x720; evaluate624x360;hr0 | Trace ends mid-line, API recording success only |
| O5 | src/d3d12/d3d12_shader_failure.hpp:20-46 | current source | source contract, independent of log producer output | lead | 2026-10-09 | HEAD f04bd8a | source | capture opt-in;64 identities limit;initial VS/PS/CS paths only | Does not characterize uncaptured latest inputs |

## Hypotheses
H1: NGX parse/feature or resource validation fails on a particular input and output stays old. Discriminator: evaluate result and native prepare/encode status with resource ids. Fix if true: repair contract.
H2: MetalFX receives stale/wrong resources or loses ordering/state across SR encode; HUD downstream still changes. Discriminator: input/output frame hashes plus barrier/resource identity, command completion. Fix if true: repair transport.
H3: shader/PSO failure prevents new scene color before DLSS, independent HUD path survives. Discriminator: scene-color hashes before SR plus failed pair replay and frame-correlated compiler events. Fix if true: correct binding.
H4: wrong deployed module or settings confound comparison. Discriminator: launched paths, loaded hashes, current package/candidate identity. Fix if true: correct staging.
H5: FEX/JIT, per-draw bridge calls or synchronization dominates FPS. Discriminator: matched frame-time and CPU/GPU critical-path measurements; existing profiles only bound historical CPU work. Fix if true: reduce work.
