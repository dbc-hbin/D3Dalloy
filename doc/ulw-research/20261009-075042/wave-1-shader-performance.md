# ShaderPerformance — shader/PSO compiler & cache cost axis (read-only)

Paths: `REPO=/Users/hanbinnoh/Documents/dxmt-d3d12`, `LIVE=/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test`.
All line anchors are exact for HEAD `f04bd8a`. LSP was unavailable in this lane (only `find`/`grep`/`glob`), so symbol navigation was done by literal search.

## summary

Three different things must not be merged:

1. **Automatic PSO cache rescan — already fixed and deployed.** `WriteAutomaticPipelineCache` no longer enumerates the whole directory per publication (`REPO/src/d3d12/d3d12_shader_cache.cpp:520-563`); the shared 16-byte accounting gates rescans to first-run/invalid/abandoned/quota (528-536, 547-553). The latest run used the fixed build (`LIVE/.storage/wine_tag.neustorage:1` = `wine-11.17-arm64ec-fex-dxmt-optimized-20261009`; `LIVE/wine/owned-graphics-runtime.json` source `dxmt_install` = `build/local/yaagl-latest-20261009/package/owned-install`, `d3d12.dll` size 101,777,408 = the deployed 97.1 MiB file; `REPO/build/local/yaagl-latest-20261009/result.json` `dxmt_commit` = `f04bd8a`).
2. **Invalid shader/root warmup is real, large, and *not* the rescan bug.** The latest run's sinks carry **9131** `Failed to compile …shader: dxil: … is not bound by the root signature` lines, all rejected inside resource lowering, spread over **356.572 s** (25.61/s) of a **418.886 s** session. Failures are never retained by the cache (`REPO/graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md:541` "실패는 보유하지 않고 다음 호출에서 재시도한다"; `d3d12_pipeline_cache.hpp:51-53` retry only covers `E_INVALIDARG` after a restored blob), so any re-request of a failing pipeline recompiles.
3. **Steady-state gameplay FPS has no measurement at all.** No FPS/frame-time series exists anywhere in the retained tree or the live installation; the only HUD-related lines are Metal HUD metric *registration* (`LIVE/logs/game_1791530994266.log:53-55`). The latest session was 85.1% covered by the compile-failure window, so this log cannot support a steady-state FPS claim in either direction.

The single largest measured shader-path cost in a real ZZZ run is **native Metal pipeline creation, not the DXMT LLVM compile** (cold compute PSO: compile 3.5–21.2 ms, native pipeline 16–1704 ms), and the automatic cache does not cover it by design.

## architecture

- Graphics/compute PSO creation: `REPO/src/d3d12/d3d12_pipeline_graphics.cpp:1599-1870`, `REPO/src/d3d12/d3d12_pipeline_compute.cpp:57-170`. Both compute a key (`PipelineCacheKey`), call `PipelineCache::Initialize` (`d3d12_pipeline_cache.cpp:527-620`), and — only when `!cache_.loaded()` — run `SM50Compile`.
- Automatic cache: module-local RAM LRU 64 MiB/512 entries (`d3d12_pipeline_cache.cpp:39-40, 52-67`) + per-exe disk namespace under `%TEMP%/dxmt-shader-sessions/<exe-path-SHA1>/dxmt-pso-<key>.bin` (`d3d12_shader_cache.cpp:412-437, 496-585`), cap 64 MiB/entry, 256 MiB/dir (`d3d12_shader_cache.cpp:324-325`), serialized by a cross-process named mutex + shared accounting mapping (`d3d12_shader_cache.cpp:332-390`).
- Compiler: DXIL → `ModuleInfo::Parse` → `LowerMatrices` → `LowerStage` → `LowerResources` → `LowerOperations` → `VerifyLoweredModule` (`src/airconv/dxil_converter.cpp:375-419`), then `LowerAtomic64Phase` + `runOptimizationPasses` + metallib write (`src/airconv/dxbc_converter.cpp:2372-2456`).
- Diagnostics are opt-in: `OWNED_GRAPHICS_TRACE=1` (`src/d3d12/owned_trace.hpp:19-22`), default event limit 8192, shared counter across event types (`owned_trace.hpp:25-36, 45-52`); failure capture requires `DXMT_SHADER_FAILURE_PATH` and is capped at 64 distinct pairs (`src/d3d12/d3d12_shader_failure.hpp:29, 32-36, 47`).

## 1. Already fixed: automatic PSO cache rescan (deployed in the latest run)

Code (all in `REPO/src/d3d12/d3d12_shader_cache.cpp`):
- Pre-fix behaviour was a full directory enumeration per publication; the fix keeps `AutomaticDiskAccounting{bytes,valid}` in a named mapping `Local\\dxmt-pso-accounting-v1-<dirhash>` (332-335, 358-362) and rescans only when `!accounting.valid` (528-536) or on quota pressure (547-553). Eviction batch headroom 32 MiB, entry 64 MiB, dir 256 MiB, atomic temp→replace unchanged (553-578).

Evidence:
- CPU real-WinAPI regression, same test, 256 fresh 1 KiB publications: **before `metadata_enumerations=256` and FAIL at replacement-at-cap** (`REPO/build/local/zzz-dlss-fix-20261009/precompile-run-02/cache-regression/before-final/run.log:2-3`) → **after `stores=256 metadata_enumerations=0`, all invariants PASS** (`…/after-final-03/run.log:2-6`). The test compiles the production file verbatim (`REPO/graphics-translator/tests/run-automatic-cache-cpu.py:16-27`, `tests/automatic-cache-disk.test.cpp:13-22`).
- Real-game fs_usage, pre-fix window: 12,246 `.bin` files, 656,479 `lstat64` + 1,312,959 `listxattr` = 1,969,438 rows (94.4% of all rows) in 5.249429 s (`…/precompile-run-02/profile-analysis/summary.json`, `summary.txt:18-40`). Arithmetic: 656,479 / 12,246 = **53.61 scans per file ⇒ 53.61 publications in 5.249 s ⇒ 10.21 publications/s**, i.e. ≈36,738 metadata syscalls and ≈97.9 ms per publication at the measured 375,171 rows/s.
- Post-fix window: 425 unique `.tmp` in 5.248979 s and **0 owned `lstat64`/`listxattr`** (`…/precompile-run-03/profile-analysis/summary.json`). The two windows are different workloads (427 vs 12,246 files), so this is **not** a controlled speedup — the doc says so (`REPO/graphics-translator/ZZZ-COMPATIBILITY.ko.md:40`, `…/precompile-run-03/result.json` limits[3]).
- Counter-claim check ("maybe the fix is not in the running binary"): the deployed `d3d12.dll` is 97.1 MiB = 101,777,408 B, matching the manifest entry for the `optimized-20261009` package whose `dxmt_commit` is `f04bd8a`; the pre-2026-10-09 runtime manifest (`LIVE/wine.before-fex-debt-city-20261008/owned-graphics-runtime.json`) lists a *different* 100,958,208 B `d3d12.dll`. Limit: no shell was available, so I could not re-hash the 97.1 MiB PE (`grep` only scans the first 4 MB of large files); the identity chain is manifest + size + launcher tag, not an independent digest.

## 2. Latest run: invalid shader/root warmup (executed evidence)

Sources: `LIVE/logs/game_1791530994266.log` (SHA256 `7b2de5a1…`, 11099 lines — parent-verified in `REPO/doc/ulw-research/20261009-075042/verify-log-counts.md`) and `LIVE/ZenlessZoneZero_d3d12.log` (9122 lines, PS-SRV 3847 vs 3856).

Timing anchors (game log):
- `:76` `2026-10-09 16:30:50.850` (last non-failure line before the block) → `:11094` `2026-10-09 16:36:47.422` (first line after the block). Failure block = **356.572 s**; 9131 lines ⇒ **25.61 failures/s**.
- Launch `16:29:54.266` (`LIVE/neutralinojs.log` tail: `wine … steam.exe Z:\Applications\Zenless Zone Zero\ZenlessZoneZero.exe -use-d3d12`) → `wineserver -w` at `16:36:53.152` = **418.886 s** session; the failure stream covers **85.1%** of it and ends 5.7 s before teardown.
- Composition (parent): PS 7129 (SRV 3856, CBV 1968, sampler 1305), VS 2002 (SRV 1142, CBV 814, `vshs_main` 46) = 9131.

Code path of each failure:
- Graphics: VS compile+failure `d3d12_pipeline_graphics.cpp:1707-1715`, PS `:1846-1855` (`ps_name = "ps_main" + sha1[0..8]`, `:1802`); compute: `d3d12_pipeline_compute.cpp:107-112`. `vshs_main` is the tessellation/GS mesh-stage path (`d3d12_pipeline_graphics.cpp:1069-1075`).
- Error text is produced by `ResourceLowering::lookupDescriptors` when the binding map has no descriptor: SRV/UAV `src/airconv/dxil_resources.cpp:1516-1518, 1526-1528`, CBV `:1481`, sampler `:1488`; reached from the worklist loop `dxil_resources.cpp:4380-4385` inside `LowerResources` (`:4465-4469`), i.e. **before** `LowerOperations`/`VerifyLoweredModule`/`runOptimizationPasses`/metallib write (`dxil_converter.cpp:403-419`, `dxbc_converter.cpp:2372` vs `:2439, 2451`).
- Consequence for the counter-claim "a failed compile is cheap": it is *partially* true — the LLVM pass pipeline and metallib writer never run for these pairs — but `ModuleInfo::Parse`, `LowerMatrices`, `LowerStage` and a prefix of `LowerResources` do. **No per-failure timing exists** (see limits); the only related retained data are 64 captured pairs from a *different* run (`REPO/build/local/zzz-dlss-fix-20261009/precompile-run-03/shaders/256-PS-*/`), which must not be generalised to the 9131 lines.
- The live run had **no** `DXMT_SHADER_FAILURE_PATH` (launch env, `LIVE/neutralinojs.log`), so no per-pair identity was captured: whether 9131 = 9131 distinct pairs or repeated attempts of fewer pairs is **unknown**.

## 3. Automatic cache behaviour in the latest run vs earlier runs

Latest run (no trace enabled — see §5):
- `LIVE/wineprefix/drive_c/users/hanbinnoh/AppData/Local/Temp/dxmt-shader-sessions/57a461904bc14837b3dbef13813d41086c5b48e7` holds **11,116 `dxmt-pso-*.bin`** entries (listing: 13 shown + "11103 more"). Sampled sizes over 7 pages (7.5–137.9 KB; means 17.2/51.5/13.6/11.0/19.7/13.2/18.6 KB) give **≈20.7 KB mean ⇒ ≈224.7 MiB ≈ 0.22 GiB**, inside the 256 MiB cap (`d3d12_shader_cache.cpp:325`).
- **Every entry's mtime lies inside one ~3–4 minute window** (first read: newest 19 min, oldest 22 min; later reads: 23–24 min). No entry is older. Combined with oldest-first eviction, the consistent reading is that the whole set was (re)written by the latest session; publication stream (11,116 over ≈356 s ⇒ **31.2/s**) overlaps the failure stream window. Interpretation is an inference from coarse relative mtimes, not a timestamped census.
- Consequence: cross-session reuse of the *preceding* runtime's entries is not visible; whether the 2026-10-09 runtime change invalidated keys (e.g. `kD3D12ShaderMetalVersion`/`kVersion` in `PipelineCacheKey`, `d3d12_pipeline_cache.cpp:36-38, 536`) or the cache was otherwise cold is unresolved. The cache-hit counter would settle it.

Earlier same-day runs *with* the trace enabled prove the mechanism works in-game:
- `REPO/build/local/zzz-dlss-fix-20261009/observe-run/game.log` (02:39): many `pipeline_cache_auto_begin` → `pipeline_cache_cold` → `pipeline_cache_auto_publish` triples (e.g. `:363-364`, `:663`, `:679`, `:695`) plus `pipeline_compute_timing` phases.
- `REPO/build/local/zzz-dlss-fix-20261009/precompile-run-01/game.log` (09:41): **≥60 `pipeline_cache_auto_hit` with `"source":"disk"`** (`:515, 524, 532, 540, 555, 573, 596, 606, 617, 624, 631, 638, 645, 652, 659, 664, 896, 911, 1288, 1307, 1325, 1349, 1372, 1398, 1417, 1433, 1449, 1465, 1481, 1498, 1517, 1534, 1550, 1568, 1585, 1603, 1627, 1645, 1674, 1691 …`) with `pipeline_cache_restore` and `loaded:1` compute timings, and **no `pipeline_cache_cold` in the searched window** — i.e. cross-process/restart disk reuse is real. Limit: `grep` searched only the first 4 MB of that file.

## 4. Remaining compiler costs (measured, retained traces)

Per-phase `pipeline_compute_timing` (nanoseconds in the logs; converted):

| run / line | lib bytes | phase cache | reflection | **compile** | library | **native pipeline** |
|---|---:|---:|---:|---:|---:|---:|
| observe-run `:652-658` (loaded 0) | 5,293 | 2.19 ms | 1.44 ms | 3.53 ms | 21.6 µs | 16.12 ms |
| observe-run `:668-674` (loaded 0) | 10,093 | 1.98 ms | 1.67 ms | 3.72 ms | 9.2 µs | 30.03 ms |
| observe-run `:684-690` (loaded 0) | 29,757 | 1.64 ms | 1.28 ms | 13.02 ms | 25.7 µs | 138.10 ms |
| observe-run `:700-706` (loaded 0) | 29,405 | 2.08 ms | 1.37 ms | 21.16 ms | 35.6 µs | 148.49 ms |
| observe-run `:716-722` (loaded 0) | 101,725 | 1.89 ms | 1.22 ms | 15.02 ms | 49.5 µs | **1704.07 ms** |
| run-01 `:677-683` (loaded 1) | 29,757 | 5.45 ms | 9.63 ms | ≈0.0001 ms | 13.9 µs | 0.83 ms |
| run-01 `:717-723` (loaded 1) | 112,845 | 5.08 ms | 7.33 ms | 0.0001 ms | 137.8 µs | 4.81 ms |
| run-01 `:804-810` (loaded 1) | 102,109 | 3.79 ms | 8.86 ms | 0 | 53.3 µs | 8.45 ms |
| run-01 `:1326-1332` (loaded 1) | 3,517 | 0.40 ms | 0.11 ms | 0 | 8.8 µs | 0.107 ms |

Other measured per-operation costs (same logs): `pipeline_cache_auto_timing phase:"disk_write"` 3.28/4.13/4.63/7.41 ms (serialized under the cross-process named mutex, `d3d12_shader_cache.cpp:525-578`); `phase:"disk_read"` 0.27–1.17 ms (run-01) and 0.80–3.14 ms (observe-run), serialized process-wide by `disk_mutex` (`d3d12_pipeline_cache.cpp:575-580`); `phase:"metadata_wait"` typically 2–80 µs.

Structural costs that remain regardless of cache hits:
1. **Native Metal library/function/pipeline creation runs on every PSO creation, even a full hit** — `d3d12_pipeline_graphics.cpp:1722-1729` (VS), `:1862-1869` (PS), `d3d12_pipeline_compute.cpp:127-155`. This is the 16–1704 ms term above; the cache removes only the AIR/LLVM compile + metallib write.
2. **Memory LRU is 512 entries** (`d3d12_pipeline_cache.cpp:40, 62-66`) against ~11k PSOs, so in-session re-creation of an already-seen PSO normally goes to disk (open+read+SHA1 `BlobDigest`+relocation checks, `d3d12_pipeline_cache.cpp:628-660`) under the global `disk_mutex`.
3. **Mesh/tessellation PSOs bypass the automatic cache entirely**: `stages = (mesh ? 0u : 1u << Vertex) | …` (`d3d12_pipeline_graphics.cpp:1616`) makes `if (stages && identity.valid)` false (`d3d12_pipeline_cache.cpp:534`); variants are memoized only per PSO object in `std::map<uint32_t, MeshPipelineVariant> mesh_variants` (`:624, 2295-2307`), and a failed variant is not retried (`:2300-2302`). The 46 `vshs_main` failures show this path is live in ZZZ.
4. **Failures are never cached** (CORE `:541`; `CanRetryAutomatic` covers only restored-blob `E_INVALIDARG`, `d3d12_pipeline_cache.hpp:51-53`), so repeated requests of a failing pair re-run the parse/lower prefix each time.
5. **All automatic disk I/O is serialized** by one process-global `disk_mutex` plus one cross-process named mutex (`d3d12_shader_cache.cpp:372-390, 525-526`); at the observed 31 publications/s this is ~32 ms of serialized write+flush+rename per entry.

## 5. Why the latest run has no cache/NGX diagnostics (asked by lead)

Confirmed absence-of-flags, not absence-of-activity. The exact launch env (`LIVE/neutralinojs.log`, entry `2026-10-09 16:29:54.266`) is:
`WINEDEBUG=fixme-all,err-unwind,+timestamp WINEPREFIX=…/wineprefix MTL_HUD_ENABLED=1 WINE_ENABLE_TIMEOUT_FIX=1 WINEMSYNC=1 DXMT_LOG_PATH=…/ DXMT_CONFIG_FILE=…/dxmt.conf GST_PLUGIN_FEATURE_RANK=… wine steam.exe …ZenlessZoneZero.exe -use-d3d12`.
- **No `OWNED_GRAPHICS_TRACE`, no `OWNED_GRAPHICS_TRACE_LIMIT`, no `DXMT_SHADER_FAILURE_PATH`, no `DXMT_LOG_LEVEL`** ⇒ all `pipeline_cache_*`, `pipeline_compute_timing`, `temporal_*` events are off by design (`owned_trace.hpp:19-22`), and no `.failure` pairs are written (`d3d12_shader_failure.hpp:32-33`). `LIVE/dxmt.conf` does not exist, so `DXMT_CONFIG_FILE` is inert and defaults apply (cache enabled).
- Positive control that the owned D3D12 path *is* loaded: 16 unconditional `D3D12 shader probe[i]` lines (`d3d12_pipeline_graphics.cpp:39-55`) in both sinks, `LIVE/ZenlessZoneZero_d3d12.log:3-19`.
- Note for any future measurement: even with `OWNED_GRAPHICS_TRACE=1`, the **default limit 8192 truncates**; with ~11k publications × 2 phase events + hits + draws, the counter is exhausted early. Set `OWNED_GRAPHICS_TRACE_LIMIT` (max 1e6, `owned_trace.hpp:25-36`).

## 6. Steady-state FPS: what this axis can and cannot say

- No FPS or frame-time series exists: a sweep of `REPO/build/local` and `LIVE` for `fps|frame_time|frame-time|frametime` returns only `[libMTLHud] Metric com.apple.hud-stat.* already exist` registration lines (e.g. `LIVE/logs/game_1791530994266.log:53-55`, and the same lines in every retained run). `LIVE/.storage/config_metalHud.neustorage` = `true` and `MTL_HUD_ENABLED=1`, so the HUD rendered on screen but was never captured to a file.
- `REPO/build/local/zzz-dlss-fix-20261009/precompile-run-03/result.json` limits[3] and `audit.json:8` state "Controlled startup/FPS and long-session stutter remain unmeasured"; `REPO/graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md:18` repeats it.
- The native `sample` of the precompile phase (`…/precompile-run-03/precompile-native.sample.txt:1-40, 350-372`) shows compile worker threads with 33–44% of on-CPU samples inside `thunk_SM50Compile`→`SM50Compile`→`runOptimizationPasses` (`dxbc_converter.cpp:2439`) and 46–62% inside `winemetal_metal4_pipeline_call`, while process-wide leaf samples are dominated by `read` (36.0%)/`kevent_id` (16.9%)/`semaphore_wait_signal_trap` (5.9%) — i.e. during warmup the shader compile + native pipeline creation dominate the *worker* threads, and the process is largely waiting on services. This is a 0.2 s sample of a small (427-entry) workload, not a steady-state profile.
- Therefore: the shader axis explains **warmup cost and hitches** with measured numbers, and cannot explain the reported steady-state FPS deficit; attributing the FPS gap to shader compilation would be a hypothesis, and the D3DMetal/Rosetta comparison has no matching sample in this repo (`REPO/doc/FEX-PERFORMANCE-2026-10-09.md:14-16` explicitly refuses a FEX-vs-DXMT serial-bottleneck ratio).

## 7. Alternatives (ranked by evidence, shader axis only)

- **A (supported by measurement).** Cold-PSO compile storm in the first session after a runtime/cache-namespace change: ~11.1k published + 9.1k failed attempts in ≈6 min; cold compute PSOs measured at 16–1704 ms native pipeline + 3.5–21 ms compile.
- **B (structural, measured).** Native Metal pipeline creation is paid on every PSO creation, cache hit or not; on a hit it is 0.8–8.5 ms and it dominates the restored path (compile ≈ 0).
- **C (hypothesis, mechanism proven, cost unmeasured).** Failing pairs are recompiled per request with no negative caching; the observed 25.61/s failure rate is therefore a *lower bound* on recompile work only if the game does not retry. Distinct-vs-repeated is unknown.
- **D (structural).** Mesh/tessellation PSOs are outside the automatic cache and recompile per session.
- **E (out of axis).** FEX translation/JIT, queue/present, temporal/MetalFX — other lanes; the FEX doc's own numbers (`FEX-PERFORMANCE-2026-10-09.md:5-40`) are JIT-only work reductions, and its `ExitFunctionEC`/cache-cap items remain unmeasured.

## 8. Minimal recommendations

1. **No code change, one run:** enable `OWNED_GRAPHICS_TRACE=1` and `OWNED_GRAPHICS_TRACE_LIMIT=1000000` in the launcher env for one session; count `pipeline_cache_auto_hit{source=memory|disk}` vs `pipeline_cache_cold`/`auto_begin`/`auto_publish` and sum `pipeline_compute_timing` phases. This answers §3's open question (cross-session reuse, cold fraction) with existing instrumentation.
2. **If failure cost matters:** the count already comes from the log (9131); per-pair identity needs `DXMT_SHADER_FAILURE_PATH` (64-pair cap, `d3d12_shader_failure.hpp:29`). Raise the cap only if the *distribution* is the question.
3. **Do not** add negative caching, weaken compiler validation, or return fake PSOs for the rejected pairs — the same 64 pairs are rejected by pristine GPTK4.0b2 (`REPO/graphics-translator/ZZZ-COMPATIBILITY.ko.md:30-33`), so the root is undersized on the game side, and CORE forbids it (`:542`).
4. **Highest-value structural candidate:** the automatic cache's remaining cold term is native pipeline creation (1.7 s observed for a 101.7 KB library), which only a native Metal binary archive would remove. CORE's gate for that work ("Native archive는 추가 native 생성 비용이 실제 병목일 때만 검토한다", `:248`) is now *supported* by §4 — but it is a design task (device/identity scoping, invalidation, packaging), not a small patch.
5. **Cheap instrumentation gap to close:** the graphics path has no per-phase timing (only compute has `pipeline_compute_timing`, `d3d12_pipeline_compute.cpp:158-166`); graphics PSO cost is currently inferable only from `pipeline_cache_auto_*` phases.

## 9. Claim candidates (with temporal scope and label)

- **C1 — VERIFIED (executed).** Latest run (`LIVE/logs/game_1791530994266.log`, 2026-10-09 16:29:54–16:36:53, runtime `optimized-20261009` = HEAD `f04bd8a`): 9131 `Failed to compile …` lines over 356.572 s = 25.61/s, covering 85.1% of the session; PS 7129 / VS 2002.
- **C2 — VERIFIED (executed).** The automatic PSO disk cache for the game's exe token holds 11,116 entries, all written inside one ~3–4 min window, ≈0.22 GiB (7-page sample), under the 256 MiB cap.
- **C3 — VERIFIED (retained real-game traces, executed).** Cold compute PSO: compile 3.53–21.16 ms, native pipeline 16.12–1704.07 ms. Restored compute PSO: compile ≈ 0, native pipeline 0.11–8.45 ms. Native pipeline creation, not the LLVM compile, dominates; the automatic cache does not cover it.
- **C4 — VERIFIED (retained real-game traces, executed).** Cross-process disk reuse occurs in real ZZZ runs: ≥60 `pipeline_cache_auto_hit source:"disk"` in `precompile-run-01/game.log` with no `pipeline_cache_cold` in the searched window; `observe-run/game.log` shows the cold+publish side of the same mechanism.
- **C5 — HYPOTHESIS.** The 9131 failures are a sustained stream rather than a startup burst (rate uniform within the single measurable window); distinct-pair count is unknown.
- **C6 — HYPOTHESIS.** Per-failure cost = DXIL parse + `LowerMatrices` + `LowerStage` + a prefix of `LowerResources`, excluding LLVM opt/metallib (`dxil_converter.cpp:403` vs `dxbc_converter.cpp:2439`); magnitude unmeasured.
- **C7 — HYPOTHESIS.** The latest session never reached a long steady-state phase (failure stream ends 5.7 s before teardown), so its log cannot attribute the user-reported FPS deficit to gameplay or to shader compilation.
- **C8 — VERIFIED (code + deploy manifest).** The 2026-10-09 `WriteAutomaticPipelineCache` rescan fix is in the running binary; the pre-fix cost for an 11k-entry directory would have been ≈1.9e8 metadata syscalls (Σ 3·k for k=0..11,116) ≈ 494 s at the measured 375,171 rows/s — larger than the whole session.

## Evidence limits / data gaps

- No FPS/frame-time/present-cadence series exists for any ZZZ run; `MTL_HUD_ENABLED=1` produced on-screen HUD only.
- No per-failure compile timing; failure identity absent in the latest run (no `DXMT_SHADER_FAILURE_PATH`); the 64-pair cap means captured pairs are a sample, not a census.
- Graphics-path PSO creation has no phase timing (compute only).
- Latest run has no cache hit/miss/publish counters (trace off) — cache effectiveness in *that* run is unmeasured; only the on-disk residue is observable.
- Deployed `d3d12.dll` identity rests on manifest + size + launcher tag; no independent re-hash (no shell; `grep` reads only the first 4 MB of large files).
- Directory mtimes are coarse/relative, so the "~3–4 minute publication window" and the 31.2/s rate are estimates, not a census.
- `LIVE/…/dxmt-shader-sessions` contains exactly one exe-token directory; other processes (e.g. the 32-bit browser) publish nothing, so no cross-process contention evidence.
- Earlier traced runs (`observe-run` 02:39, `precompile-run-01` 09:41, `dlss-run-02` 03:06) are *different* cuts/prefixes from the latest deployment; their measurements are used for mechanism and cost, never as a controlled comparison against the latest run.

## EXPAND

- **LEAD — measure the cold fraction with existing instrumentation.** One launcher session with `OWNED_GRAPHICS_TRACE=1` + `OWNED_GRAPHICS_TRACE_LIMIT=1000000`; count `auto_hit{memory|disk}` vs `cold`/`begin`/`publish` and sum `pipeline_compute_timing` per phase. Resolves C5/C7 and §3's cross-session-reuse question without touching code. Cost: one game session.
- **LEAD — native Metal pipeline creation is the residual cost.** §4 shows `phase:"pipeline"` at 16–1704 ms cold and 0.1–8.5 ms restored; only a native archive removes it. CORE `:248` already names this as the gate. Needs a design note (identity scoping, invalidation) before any implementation.
- **LEAD — graphics PSO phase timing.** `pipeline_compute_timing` exists only for compute; the graphics warmup is the one producing the 9131 failures and the 11.1k publications. A graphics equivalent (or a per-session counter of cold/hit/failed PSO creations) would convert C6 from hypothesis to measurement.
- **LEAD — mesh/tessellation variants are outside the automatic cache** (`d3d12_pipeline_graphics.cpp:1616`, `d3d12_pipeline_cache.cpp:534`); quantify their share before considering a variant key.
- **DEAD END — "the automatic rescan is still costing us".** Fixed, deployed, and independently reproduced at CPU level (256→0 enumerations) and in-game (1,969,438→0 owned rows). Any further work here is measurement noise; the two fs_usage windows are explicitly not comparable.
- **DEAD END — "the failure HRESULT aborts the warmup / stalls progress".** Already refuted by retained static analysis: the direct warmup routine ignores the HRESULT, logs nothing, and its job counters advance (`precompile-run-01/warmup-caller/result.txt`, `OBSERVE03` section). The failures consume CPU, not progress.
- **DEAD END — generalising the 64 captured pairs to the 9131 log lines.** The 64 is a capture cap (`d3d12_shader_failure.hpp:29`), and the pairs come from a different run/prefix; the parent already flagged this.
- **DEAD END — inferring steady-state FPS from this axis.** No FPS series exists anywhere, and the latest session is 85.1% warmup window. Any FPS number here would be invented.
