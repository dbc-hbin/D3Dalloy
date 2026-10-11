# EvidenceSkeptic findings

Scope: read-only examination of the latest retained live logs, older successful DLSS runs, source at HEAD f04bd8a, and retained CPU attribution. No game launch, GUI action, native binary probe, build, edit, or journal write was performed. Current-runtime binary identity is the RuntimeIdentity lane's responsibility; the source contracts below are at HEAD unless separately tied to retained evidence.

## 1. Latest-run NGX silence is an observability gap, not proof NGX stopped

The exact launcher invocation is retained at `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/neutralinojs.log:569`. It explicitly sets MTL_HUD_ENABLED, Wine flags, DXMT_LOG_PATH and DXMT_CONFIG_FILE, but does not explicitly set OWNED_GRAPHICS_TRACE, OWNED_GRAPHICS_TRACE_LIMIT, DXMT_SHADER_FAILURE_PATH or pass `-streamline-d3d12-diag`. The associated Player.log confirms `diag=0` at `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/wineprefix/drive_c/users/hanbinnoh/AppData/LocalLow/miHoYo/ZenlessZoneZero/Player.log:8`.

The trace contract at `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/owned_trace.hpp:19-35,45-54` requires OWNED_GRAPHICS_TRACE exactly `1`; absent that value it emits nothing. The default event limit is 8192, and the counter bounds the entire DLL rather than each event type. The latest launch line is not a full inherited-environment dump, so the defensible wording is: **the launcher did not explicitly enable these diagnostics, the application reports its diagnostics off, and missing NGX trace records cannot identify a failure stage.** Do not upgrade this to a proven full process-environment value.

By contrast, `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/launcher.c:43-59` explicitly enables OWNED_GRAPHICS_TRACE=1, limits it to 64, and passes the Streamline diagnostic argument. The historical D3D12 trace already reaches event 64 during initial pipeline work at `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/candidate-03/ZenlessZoneZero_d3d12.log:3-68`, long before DLSS begins. Thus even the successful run cannot supply a complete late temporal_record -> temporal_encode -> complete chain. Increasing only the interpretation of those old logs cannot recover events suppressed at capture time.

Additional artifact-quality issue: the candidate-03 `ZenlessZoneZero_nvngx.log:38` ends inside a JSON field, at `input_width:62`. Its stdout counterpart has the complete event and subsequent backend success at `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/candidate-03/game.log:3147-3151`. Treat the dedicated file as a partial sink, not a complete end-of-run history. The game stdout records backend event IDs 14,19,...,64: `(64-14)/5+1 = 11` sampled successful evaluations, not proof that evaluation stopped after 11 frames.

## 2. hr=0 proves recording acceptance, not fresh displayed output

The source meaning is precise. `/Users/hanbinnoh/Documents/dxmt-d3d12/src/nvngx/nvngx_d3d12.cpp:1033-1050` parses the frame, calls `extension->TemporalUpscale`, logs that HRESULT, then maps it to the NGX return. `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_command_list.cpp:1637-1651` prepares a frame, allocates an EncoderType::Temporal pass, and returns S_OK. It does not wait for GPU execution or verify output pixels.

I checked the consuming dispatch, not only the producer: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_command_queue.cpp:6434-6481` has an explicit temporal branch, retains the frame, and calls EncodeOwnedTemporalFrame. `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_temporal.cpp:225-245` performs the native encode later and reports removal on a negative result. Actual submission completion is separately observed at `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_command_queue.cpp:623-654`.

Therefore historical `ngx_d3d12_evaluate_backend hr=0` establishes successful preparation/recording for those calls. It does not prove fresh input color, correct texture identity, native encoding completion, a changed output, or the use of that output by the presented scene. Even `OWNED_GRAPHICS_NATIVE_PRESENT_DECISION ... presented=1` means the completed/current submission was sent through PresentMetal4Drawable (`d3d12_command_queue.cpp:659-679`), not that its scene pixels differ from the prior present.

The latest log's MetalFX HUD metric-registration message at `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/logs/game_1791530994266.log:76` is weaker still. It is not a frame-completion or freshness marker.

## 3. The prior visual success is real but narrower than sustained motion or FPS validation

I inspected both retained images directly:
- `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/candidate-03/city-dlss-01.png`
- `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/candidate-03/city-dlss-02.png`

Both show textured city-interior geometry, the character and NPC, and MetalFX temporal 624x360 -> 1280x720. The second image visibly differs in the NPC's head orientation and the player character's idle pose; these are not merely different HUD numbers. This is useful counterevidence against an assertion that the historical candidate's scene was already permanently frozen throughout both captures. It does not establish continuous movement, camera response, long-duration stability, or the latest scene's behavior. The images have the same camera view, as explicitly acknowledged at `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/result.json:14-22` and `/Users/hanbinnoh/Documents/dxmt-d3d12/doc/FEX-PERFORMANCE-2026-10-09.md:51-53`.

The images display HUD FPS 60.00 and frame interval 16.67 ms, with GPU 19.24 ms and 18.23 ms respectively. These are individual HUD samples, not a time series, and are not a matched D3DMetal/Rosetta comparison. Do not turn them into a sustained-60-FPS claim or use GPU time versus frame interval from stills to derive a bottleneck ratio. The retained launcher also forces 1280x720; the latest launch has no corresponding explicit resolution arguments. The latest Player.log watermark dimensions of 3840x2160 are watermark configuration, not sufficient proof of render/output resolution.

## 4. FeatureFailedToLoad and several alarming application warnings fail the historical counterexample test

The latest Player.log has eErrorFeatureFailedToLoad at lines 17,51,52 and adjacent `DLSSG is not requested or failed to load` at line 18. The historical successful candidate has the same combination at `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/candidate-03/Player.log:16-19,51-52`, while its owned NGX calls and rendered scene are independently present. Consequently the warning string alone does not diagnose DLSS SR failure. The neighboring DLSSG message helps distinguish features, but unlabelled FeatureFailedToLoad lines cannot all be assigned to one feature from adjacency alone.

The same caution applies to alternative-cause claims:
- Latest `moon_bright is released while Async SRP still running` and ShaderPassContext mutation at latest Player.log:325-327 also occur in candidate-03 Player.log:5614-5616, followed by further slSetTagForFrame/slEvaluateFeature records at 5617-5619.
- Latest serialization-layout warning at Player.log:340 also occurs in candidate-03 Player.log:19881 onward. Its stack concerns UIMainPageBG3DModelController/menu opening, not a demonstrated general world-render freeze.
- Latest `eErrorFeatureMissing` at Player.log:445 also occurs in the prior successful DLSS off/on run at `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-dlss-fix-20261009/dlss-run-02/Player.log:20459`.

These messages warrant attention only with temporal/resource correlation; they are not individually distinguishing evidence. Historical coexistence does not prove harmlessness in every run, but defeats a string-only diagnosis. The latest `[ffx] Upscaler Context` entries at Player.log:446-447 also mean one should not assume a single unchanged upscaler configuration across the whole log. They do not themselves identify which upscaler was active during the freeze.

## 5. Shader failures are a concrete latest-run difference, but not yet the frozen-scene cause

The latest game log starts reporting root-binding compile failures at `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/logs/game_1791530994266.log:78` and continues to its final error region. Literal searches of candidate-03 game.log and ZenlessZoneZero_d3d12.log found no `Failed to compile`, `not bound by the root signature`, or POSITION_0 warning matches. This difference is stronger evidence than generic Streamline warnings.

The lead's executed count artifact `/Users/hanbinnoh/Documents/dxmt-d3d12/doc/ulw-research/20261009-075042/verify-log-counts.md:1-15` establishes 9131 compile-failure log lines in latest stdout, versus 9122 in the dedicated D3D12 sink, and 1881 POSITION_0 warnings in each. These are neither unique shaders nor frames, and the two sinks must not be summed. Missing resource names in repeated message text make per-shader deduplication impossible from the messages alone.

At HEAD, matching VS/PS compiler errors return E_FAIL from initialization: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_pipeline_graphics.cpp:1706-1715,1846-1854`. This confirms a genuine failed compilation path, not a harmless success diagnostic. However, it does not establish that the failed creations were required current scene draws rather than warmup attempts.

Historical counterevidence is important: `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-dlss-fix-20261009/precompile-run-01/warmup-caller/result.txt:24-48` documents retained stack evidence identifying a direct warmup routine whose parent progresses and whose job counter decrements after ordinary return, independent of PSO HRESULT. The same report distinguishes a normal draw-PSO creator with different error behavior. This identifies a concrete alternative to 'compile rejection stalls the entire game.' That old caller proof cannot be assigned to the latest failures without current stacks.

Likewise `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-dlss-fix-20261009/precompile-run-01/binding-diagnosis/result.txt:1-10` proves the exact captured historical 64 shader/root pairs were invalid and rejected by both owned and pristine GPTK compilers. It does not prove the latest 9131 log lines are the same pairs. Nor were those 64 an exhaustive population: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_shader_failure.hpp:20-46` records only the initial VS/PS/CS compile calls, stops after 64 identities/attempts, and deduplicates by stage+shader+root bytes. It does not cover every stage/variant or preserve every distinct compilation argument for a repeated pair. In particular, do not use that capture set as comprehensive proof about the latest vshs_main failures.

Minimal recommendation: preserve strict root validation. Obtain current pair hashes and a caller stack before attributing the latest errors to the old warmup bug or changing binding logic. Scene-input starvation, repeated failing PSO work, and unrelated warmup errors are all still hypotheses for this run.

## 6. Moving in-game HUD contradicts an undifferentiated whole-renderer freeze, not every queue/resource failure

The live motion claim is the user's observation; these artifacts do not include a new synchronized video or pixel capture of the reported freeze. If the in-game character-position HUD continues changing on screen, at least the relevant application/UI/presentation path continues to deliver changes. That is inconsistent with a claim that all visible rendering and presentation are permanently stopped at that time. A stale 3D scene texture composited with a fresh HUD remains consistent, as do stale temporal inputs, stale output selection, failed scene-pass production, or a queue-specific dependency problem.

Do not overcorrect to 'no GPU/queue problem is possible.' Moving UI does not establish progress of the scene-producing queue or temporal feature, and a Metal HUD overlay alone would be an even weaker oracle than changing in-game UI. Current source has separate recording, temporal encoding and completion stages, so logical stage separation is supported; none of the candidate stale-resource mechanisms is runtime-confirmed here.

Literal searches found no latest `Metal 4 submission failed`, `Metal 4 submission did not complete`, device-removed/error-code marker, or normal `CreateGraphicsPipelineState failed` application diagnostic in the inspected three live logs. Absence is not a deadlock disproof: the source error check runs after waitUntilCompleted returns (`d3d12_command_queue.cpp:629-643`), so a wait that never returns need not emit that error. Conversely, no log supports asserting a global deadlock as the observed cause.

## 7. FEX CPU share is neither FPS loss nor even a bound on all translation effects

I re-evaluated the retained JSON numerically through the read tool's jq filter. Source: `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-research-20261009/cpu-attribution.json`, run anchors at lines 4,1660,3339; first-run inputs are at lines 7,22-44.

- OFF first: `15.819 / 86.929 * 100 = 18.197609543420494%` native FEX share; `86.929 / 40 = 2.173225` average sampled running CPU-core equivalents.
- OFF repeat: `16.506 / 98.169 * 100 = 16.81386180973627%`; `98.169 / 40 = 2.454225` core equivalents.
- ON repeat: `14.467 / 103.608 * 100 = 13.963207474326307%`; `103.608 / 40 = 2.5902` core equivalents.

These denominators are summed CPU-running weights across threads, not wall-clock frame budgets. Native FEX's inclusive code-generation stacks (9.659,9.614,7.640 CPU seconds) overlap other attribution and must not be added as independent costs. Translated game-code execution is a separate category, so native FEX share also does not bound the impact of generated-code quality relative to Rosetta.

A useful additional falsifier: the JSON's `Network Thread` alone accounts for 4.899,4.791,4.728 native-FEX CPU seconds respectively. Executed ratios are `4.899/15.819 = 30.969087805803145%`, `4.791/16.506 = 29.025808796801165%`, and `4.728/14.467 = 32.68127462500863%` of native-FEX attribution. Thus equating all FEX self-time with frame-critical graphics-thread delay is specifically unjustified. The thread label does not prove this work is off the critical path; it proves a critical-path analysis is missing.

The retained report already documents non-matched loading/rendering windows, omitted external Metal compiler processes, and confounded GPU-identity/TAA profiling at `/Users/hanbinnoh/Documents/dxmt-d3d12/doc/FEX-PERFORMANCE-2026-10-09.md:11-27`. Its IR-tail microbenchmark proves equal-input work reduction, not a game-FPS speedup. Latest-runtime low FPS versus D3DMetal/Rosetta remains the user's valid comparative observation, with no controlled ratio or subsystem attribution supplied by these artifacts.

## 8. Existing-artifact checks and smallest next evidence

Completed from existing artifacts: historical/latest warning cross-comparison; actual viewing of both old scene images; trace-limit and partial-sink checks; numerical CPU/share recalculation; producer-to-consumer temporal contract tracing; relevant git history review, including 105f82e's explicit statement that invalid pairs remain rejected and profiling windows are unmatched. No native executable or binary probe was needed.

Further safe analysis possible without launching the game:
1. Use candidate-03 game.log, not its truncated nvngx sink, to enumerate the bounded NGX observations. Treat D3D12's event-64 ceiling as censoring, not as an execution stop. Correlate old image scene-only regions separately from HUD regions if quantitative visual difference evidence is desired; do not infer duration from filenames.
2. Reuse retained exact-input shader/root replay results only for matching hashes. Search for a latest-run failure-capture directory/manifest if another lane identifies one. Without current pair identities and callers, the old 64-pair proof cannot settle the latest errors.
3. Reuse the retained CPU JSON's thread/time bins to identify candidate costs. Without aligned frame times, GPU timings and completion/wait intervals from the same window, no arithmetic can establish how many FPS those costs lost.

The minimal missing runtime evidence is a bounded sequence spanning a known input/camera change: input color content/identity, temporal record and encode identity, output content/identity, submission completion, and presented scene content, with the HUD region tracked separately. If fresh input reaches temporal encoding but output stays unchanged, investigate temporal/backend state; if input is already stale, investigate scene production; if output changes but display does not, investigate downstream selection/composition. This is a recommended future discriminating test, not an executed finding. A matched D3DMetal/Rosetta comparison additionally needs the same scene, resolution, settings, cold/warm state and frame-time window. Neither test was authorized to launch here.

## EXPAND

- LEAD: Current failure-pair and caller attribution. Strongest latest-run differentiator is the shader-error stream, but current identities/stacks are absent. Existing historical captures distinguish warmup from normal rendering and provide a concrete comparison method, not a universal diagnosis.
- LEAD: Freshness across input -> temporal output -> presented scene. Source and symptom permit multiple localized failure stages; no retained current content sequence selects one. Exact missing evidence is described above.
- LEAD: Diagnostic capture design. The old 64-event per-DLL limit is exhausted before temporal work; a repeat using the same limit would again fail to capture the needed chain. The dedicated historical nvngx file is also truncated, so retain complete stdout and capture identity.
- LEAD: Frame-critical performance attribution. Existing CPU bins are usable for candidate selection, but current FPS/GPU/wait measurements and a matched Rosetta/D3DMetal baseline are missing. Network-thread FEX share specifically blocks interpreting total FEX percentage as graphics delay.
- DEAD END: FeatureFailedToLoad, FeatureMissing, moon_bright/SRP, or serialization-warning strings alone. Each has a retained successful-run counterexample; none uniquely identifies this freeze.
- DEAD END: hr=0, native presented=1, or a MetalFX HUD registration message as a freshness oracle. Their source contracts do not observe changed scene pixels.
- DEAD END: Whole-GPU-deadlock assertion from a frozen scene alone. Sustained changing in-game HUD is counterevidence to an undifferentiated global stop; localized queue/resource issues remain open.
- DEAD END: Historical 60-FPS stills, JIT microbenchmark speedup, or summed CPU shares as a current game speedup/FPS-loss ratio. They measure different quantities and unmatched workloads.