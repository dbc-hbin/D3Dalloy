## FEX/Rosetta comparison findings

Owned axis completed read-only. No source edits, prefix changes, game launch, GUI activity, or new files. I re-executed the retained attribution analyzer to `/dev/null`, verified current emitter source hashes, and ran the retained native sanitizer and counted-work binaries. The latest frozen-scene execution was not profiled by this worker. All historical CPU numbers below belong to earlier 2026-10-09 isolated `cache-04` runs, before emitter commit `206d61d07`, not the user's latest `optimized-20261009` execution.

### 1. FEX has measured CPU costs; their share is not its slowdown versus Rosetta

Absolute evidence root: `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-research-20261009/`.

Re-executed command: `python3 build/local/zzz-fex-research-20261009/analyze-time.py build/local/zzz-fex-research-20261009/analysis-config.json /dev/null`. Exit 0; 10.31 seconds. It reproduced all four retained aggregate results exactly:

| Historical interval | All game threads, CPU seconds | Exclusive native FEX | FEX share | ExitFunctionEC | Inclusive fresh-codegen stack |
|---|---:|---:|---:|---:|---:|
| OFF first, city40 | 86.929 | 15.819 | 18.1976095% | 3.185 | 9.659 |
| OFF repeat, city40 | 98.169 | 16.506 | 16.8138618% | 3.841 | 9.614 |
| ON repeat, city40 | 103.608 | 14.467 | 13.9632075% | 3.953 | 7.640 |
| OFF first, startup120 | 116.575 | 10.602 | 9.0946% approximately | 1.811 | 6.438 |

Anchors: `cpu-attribution.json:4-44`, `:1660-1702`, `:3339-3382`, `:5018-5058`; ExitFunctionEC leaves at `:774-775`, `:2434-2435`, `:4102-4103`, `:6157-6158`. `analysis-config.json:1-49` binds PID, map, log, XML, trace window, and duration. `/Users/hanbinnoh/Documents/dxmt-d3d12/doc/FEX-PERFORMANCE-2026-10-09.md:11-28` correctly limits interpretation.

Exact city calculations are native_FEX / total × 100. Inclusive codegen shares are 11.1113667%, 9.7933156%, and 7.3739480%; those include overlapping frames and must not be added to the FEX column. Missing stack/unclassified/map conflict combined are 11.427, 13.274, and 14.251 CPU seconds, or 13.1452105%, 13.5215801%, and 13.7547294% of their denominators. The original analyzer explicitly preserves these gaps (`analyze-time.py:50-80,94-143`).

The denominator is Running sample weight summed over all game threads, not 40 elapsed seconds, and excludes external Metal compiler service processes. Translated application code appears separately in `historical_FEX_code_region`, which also has historical allocation-lifetime ambiguity. Neither those regions nor exclusive native FEX represent the difference from native x86 or Rosetta. Example: OFF repeat had 38.991 CPU seconds in historical code regions plus 16.506 in native FEX; adding them and calling 55.497 seconds an emulation penalty would incorrectly count useful application computation as overhead.

Calibration evidence remains plausible: `calibration-result.json:1-20` reports 2.996 CPU seconds of named work and 2.986 anonymous JIT work, 0.009 other, zero waiting-thread samples. This is retained calibration, not a new calibration run.

### 2. Applied emitter fix is real and structurally verified; no sustained game FPS gain follows from it

FEX source root: `/Users/hanbinnoh/Documents/FEX-darwin/`. Current HEAD is `206d61d076b81fb0cc506e2535965217124b4340`; `git status --short` and `git diff --stat` returned empty. Relevant history: `206d61d07 Darwin: Avoid repeated IR code-block tail scans`, 10 insertions / 2 deletions in two files. Source hashes match retained candidate/current manifests exactly:
- `FEXCore/Source/Interface/IR/IREmitter.cpp`: `573f57c6d7add250ba79a161be997c08138a098846eb7d3be4c3f5772867bac9`.
- `FEXCore/Source/Interface/IR/IREmitter.h`: `a9400ef8c3b235df769d0e81dc51182f0980ac2ee3b25b550d6158b7a9ede7c7`.

Concrete behavior: `.cpp:161-180` uses a cached reachable tail anchor for implicit append and still follows `Next`, so explicit insertion after the anchor remains visible. Reset invalidates at `.cpp:95`; removal invalidates at `.cpp:149-150`; CopyData invalidates at `.h:442`; the field is at `.h:572-573`. This does not shortcut SMC checks or change block order.

Retained fixed-work measurements (`build/local/ir-tail-proof-20261009/compare-result.json:155-203`):
- Head-fixed, 8192: 100651008 → 8191 traversals, exactly 12288× fewer; CPU median 113.993 → 0.108 ms, 1055.4907407× ratio for this micro-workload.
- Alternating, 8192: 125806592 → 8191 traversals, 15359.1248932× fewer; CPU median 171.431 → 0.207 ms, 828.1690821× ratio for this micro-workload.

Fresh read-only execution: `current-sanitize` printed `regression,PASS,4f9b6c042a7a22d4`; retained `baseline-counted-timing measure` and `candidate-counted-timing measure` both printed the same regression hash and identical per-size structural hashes. At 8192, fresh counted head traversals remained 100651008 → 8191 and alternating 125806592 → 8191. Fresh counted CPU medians were head 115.291 → 0.106 ms, alternating 171.115 → 0.217 ms. These counted copies are instrumentation checks, not substitutes for the uninstrumented retained timing comparison; `proof-manifest.json:1-15` explicitly says separate counted copies are not used for performance timing.

Fixture design (`Darwin/tests/fex_ir_tail.cpp:110-208,209-248`) covers explicit insertions before/after the anchor, extended lists, SMC conditional exits, cached-node deletion pre/post RA, reset, CopyData between distinct arenas, fixed five-run medians, and hashes/validation outside the timed region. `check-result.json:1-51` records the sanitizer invocation and matching current hashes.

Historical OFF-repeat tail scanning was 2.133 / 98.169 = 2.172783669% of aggregate scheduled CPU (`cpu-attribution.json:2444-2445`). Even this share is not an elapsed-time or FPS prediction: the affected threads may overlap other work, and the present failure changes workload. Candidate-03 QA demonstrates a single DLSS rendered scene, not continuing scene animation or frame-time improvement (`doc/FEX-PERFORMANCE-2026-10-09.md:43-52`). The emitter optimization therefore cannot establish that latest scene freezing is resolved or that FEX now matches Rosetta.

### 3. Remaining EC transition cost is observed; deleting FPCR/suspend work is not justified

Historical ExitFunctionEC represented 3.6639096%, 3.9126404%, and 3.8153424% of aggregate game CPU, or 20.1340161%, 23.2703259%, and 27.3242552% of exclusive native FEX. This is a sampled routine contribution, not all EC transition cost or the cost of one instruction.

`/Users/hanbinnoh/Documents/FEX-darwin/Source/Windows/ARM64EC/Module.S:57-105` concretely clears FPCR NEP/AH, marks InSimulation false, loads/checks SuspendDoorbell, resolves the ARM64EC entry/return thunk, and preserves 8-byte-misaligned x64 stack returns. `Module.cpp:710-717` explains the suspend race and handles the `brk #0xCAFE` resume path. Removing/reordering doorbell handling risks real thread-state correctness.

Important counterclaim to conditional FPCR-write avoidance: `FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp:113-125` calls `FillSpecialRegs` on ARM64EC entry, and `ArchHelpers/Arm64Emitter.cpp:677-700` sets AH and NEP whenever SupportsAFP is true. The normal EC exit therefore may genuinely need to clear them every time. A branch that skips a write only when bits are already zero could simply add work to the common path. `ArchHelpers/Arm64Emitter.cpp:721-738` also clears AFP state on other spills, so entry-state frequency must be measured before choosing an optimization. JIT vector operations rely on these bits for scalar insertion and x86 NaN behavior (`JIT/VectorOps.cpp:280-289,447-450,474-478`). Disabling AFP globally could move cost into every translated vector instruction.

Minimal recommendation: measure transition counts and originating call sites, then consider reducing redundant x64/native boundary crossings rather than weakening ABI state. Conditional FPCR optimization remains a hypothesis; require exact input-state frequency, equal-call-count timing, FP/NaN/DAZ boundary coverage, and suspend/stack regressions. Samples near an `msr fpcr` do not isolate that instruction's latency because sampling skid and adjacent dependency stalls remain possible.

### 4. Branch patching is the strongest untouched compile-time candidate, not a measured FPS fix

`FEXCore/…/cpu-attribution.json` historical leaf values are 1.817, 1.834, and 1.814 CPU seconds for `ARMEmitter::Emitter::Bind(ForwardLabel::Reference const*)`, at `:789-790`, `:2454-2455`, `:4122-4123`. These equal 2.0902116%, 1.8682069%, and 1.7508301% of aggregate scheduled CPU. Unlike tail scans, this code is still unchanged by the applied fix.

Concrete source `/Users/hanbinnoh/Documents/FEX-darwin/CodeEmitter/CodeEmitter/Emitter.h:694-742` performs signed-range plus alignment checks for B, TEST_BRANCH, and BC/RELATIVE_LOAD. A combined unsigned-range/alignment formulation or rotate-based check could reduce host instructions, but this remains an unimplemented performance hypothesis. Preserve accepted byte ranges B [-134217728,134217724], TEST_BRANCH [-32768,32764], and aligned BC/RELATIVE_LOAD [-1048576,1048572]. The textual BC upper bound is 1048575, but alignment reduces the actual final accepted offset to 1048572. Check both extremes, one step beyond, misalignment, no instruction modification on failure, and `Bind(ForwardLabel*)`'s non-short-circuit behavior (`:795-806`, `Bound &= Bind(...)` continues later references after one fails). Verify generated host code, then equal-work timing. LONG_ADDRESS_GEN (`:743-786`) has no separate retained hotspot evidence; do not expand that algorithm speculatively.

### 5. Cache turnover/invalidations are actionable investigation leads, not proof that the 1 GiB file causes low FPS

`on-repeat/cache-inventory.txt:1-4` records 1048612 KiB main file allocation and 32772 KiB index allocation. The former is 36 KiB above nominal 1048576 KiB, but this is allocated-block inventory, not exact file byte length or a cap-rejection counter. `FEXCore/Source/Interface/Config/Config.json.in:211-216` caps main file bytes at 1073741824, explicitly excluding index. `DiskCache.cpp:301-308` accounts for wrapper and blob under file lock; `:1085-1086` and `:1343-1348` handle full-cache paths. This cannot prove saturation caused missed hot blocks.

Persistent cache and active executable cache differ. `SharedCodeBufferManager.cpp:15-17,86-96` grows 16 → 32 → 64 → 128 MiB and remains at 128 MiB to keep shorter branch encodings. `JIT/JIT.cpp:821-842` switches guest-to-host mapping when a code buffer changes and clears/retries when allocation fails. `LookupCache.h:175-178` clears local caches upon shared-map change. All three retained global maps contain ten 128 MiB ranges plus 16/32/64 MiB and a 10048-byte dispatcher: 1392.0095825195312 MiB of historical ranges. These are not simultaneous resident/committed memory totals; there are no allocation lifetimes. They support investigating repeated buffer generations, not assuming a 1.36 GiB leak. Increasing the 128 MiB maximum changes branch-distance assumptions and is not a minimal safe fix.

Cache hits still do useful work: Core.cpp:903-934 loads bytes and restores entrypoint mappings; JIT.cpp:1171-1187 allocates, copies host bytes, and clears the instruction cache. DiskCache.cpp:638-659 decodes/hashes anonymous keys, :780-795 hashes candidate footprints, and :849-887 allocates/copies/validates the blob. The historical successful-load sample (`cached-load-sample.json:1-25`) is exactly one 1 ms CPU-weight sample at RVA 0xce0cd inside the successful load body; `cpu-attribution.json:3381-3382` reports 0.001 inclusive seconds. It proves an observed cached load, not a hit ratio, persistent-versus-transient origin, or negligible total lookup cost.

Historical invalidation leaves were 0.701 CPU seconds OFF repeat and 0.729 ON repeat (`cpu-attribution.json:2484-2485,4142-4143`). `LookupCache.h:352-369` invalidates blocks intersecting pages; `Core.cpp:1082-1094` can discard call-ret stack state. Repeated SMC/page invalidation and recompilation remain hypotheses without counts and guest RIP correlation. Existing history already fixed cache-entry lifetime (`4de0b305c`, snapshots and safe pruning); do not rediscover that historical bug as an unpatched present cause.

Built-in instrumentation should be considered before creating new infrastructure: `FEXCore/include/FEXCore/Utils/SHMStats.h:56-79` already has JIT time/count, signal time, SMC count, cache-lock time, and disk hit/miss/lookup fields; `Source/Windows/ARM64EC/Module.cpp:686-692,1101-1103` initializes slots under ProfileStats on Wine. Core.cpp:924-944 records actual hits/misses/JIT counts. However the Windows UnixLib allocation path must work in the Darwin runtime (`Source/Windows/Common/SHMStats.cpp:28-36`, `FEXUnixLib.cpp:49-83,158-172`); no slot data or successful Darwin stats attachment is retained here, so `FEX_PROFILESTATS=1` is a candidate measurement route, not verified deployment advice. Additional cap-rejection, buffer-generation, EC count, and repeated-RIP reason counters are absent from the observed evidence.

### 6. Apples-to-apples limitations and latest frozen-scene claim candidates

The research launcher explicitly set an unquoted GPU description (`launcher.c:41`); retained `off-01/Player.log:21-24` shows `Renderer: NVIDIA`, not the full GPU identity. The document notes that this hid DLSS in initial research and early candidate profiles (`doc/FEX-PERFORMANCE-2026-10-09.md:25,62`). Therefore these are not valid DLSS-ON versus corrected DLSS-ON FPS comparisons. OFF/ON also mean disk cache, not DLSS. OFF retains in-process JIT reuse (`Core.cpp:890-894`).

City intervals overlap: OFF repeat [24.505,56.005] seconds (`off-02/milestones.json:31-34`); ON repeat [13.520,32.955] (`on-repeat/milestones.json:18-21`), intersection [24.505,32.955]. No measured loading reduction can be calculated from those observation bounds. Sampling windows included different amounts of city rendering and uncontrolled graphics/assets/OS/network/interaction warming. ON's higher total CPU weight, 103.608 versus 98.169, does not demonstrate regression either.

No retained matched Rosetta/D3DMetal CPU/frame-time run was found in the scoped research artifacts or document. Comparing user-observed D3DMetal/Rosetta FPS to DXMT/FEX varies renderer, CPU translator, Wine ABI/runtime, pipeline caches, shader correctness, upscaler, and potentially resolution/settings simultaneously. A causal translator ratio needs the same application workload and renderer or a separately isolated CPU workload; a renderer comparison needs matched completed rendering with the same scene, output/internal resolution, FG/RT/VSync/FPS cap, and cache state. The current frozen scene and observed latest shader failures make useful completed-work equivalence especially doubtful. An FPS counter can continue advancing while scene content is stale.

Supported wording: historical FEX native code generation and EC transitions consumed observable CPU; a specific emitter micro-workload was optimized and structurally checked; latest low FPS can still include FEX costs, but no measured FEX-versus-Rosetta penalty or latest-run causal allocation is available. Unsupported wording: FEX causes the current frozen scene; FEX is no longer a bottleneck; 14–18% of FPS was lost to FEX; the 1000× emitter result applies to game FPS; DLSS worked continuously because one candidate screenshot rendered; lack of NGX trace text proves NGX was never called. Parent's latest-run compilation failures are separate evidence and must not be merged with these older profiles.

LSP limitation: clangd reported ready but sibling definition/reference requests returned enclosing namespace locations, not requested symbols; retained proof-manifest also describes missing generated includes/cross-build configuration. I used literal variation searches and exact source ranges after that failure, plus relevant git history; the bad LSP locations were not used as evidence.

## EXPAND

- LEAD: Obtain latest-run sampled CPU/frame-time attribution synchronized to sustained correct scene output. Precise gap: existing profiles predate emitter fix and have GPU-description/DLSS configuration differences; no same-scene Rosetta baseline exists in this lane.
- LEAD: Equal-work branch Bind optimization at Emitter.h:694-742. Retained hotspot exists; boundary/failure contract and emitted-code/timing evidence are still missing.
- LEAD: Correlate cache misses, cap rejects, executable-buffer generations, invalidations and repeated guest RIPs. Historical maps and invalidation CPU justify measurement, not capacity changes.
- LEAD: Validate existing Wine SHMStats route before new counters. Slot allocation on the exact Darwin runtime remains unexecuted; after that, add only missing event reasons/counts.
- LEAD: EC transition call-site/count profiling. Reduce unnecessary crossings if a repeated hot boundary is found; conditional FPCR avoidance remains contingent on state-frequency evidence.
- DEAD END: Treating FPCR write omission as an obvious free win. AFP is deliberately enabled on normal EC entry, and vector semantics plus suspend ordering constrain the exit.
- DEAD END: Claiming cache saturation from du-sized 1 GiB inventory or resident memory from historical maps. Neither exact logical size/cap rejection nor allocation lifetime is captured.
- DEAD END: Another emitter-tail fix. Current source hashes match committed candidate, sanitizer and counted-work proofs pass; game performance effect remains a measurement gap, not an unimplemented tail scan.
- DEAD END: Attributing current frozen scene or quantitative D3DMetal/Rosetta FPS difference from the historical FEX profiles. Temporal/configuration/work-completion mismatches prevent that inference.