## LatestLogs findings

Read-only investigation completed for the latest run; no game launch, prefix change, product edit, or research-file write. The latest logs do establish a large volume of shader-creation failures and continued game lifecycle activity. They do not establish the exact DLSS evaluate/native-encode failure stage or a measured FPS comparison.

### Sources and temporal scope

Live root: `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test`.

- `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/neutralinojs.log:569` records the actual game launch at **2026-10-09 16:29:54.266 local time**, with `-use-d3d12`, `MTL_HUD_ENABLED=1`, `WINEMSYNC=1`, `WINE_ENABLE_TIMEOUT_FIX=1`, `DXMT_LOG_PATH` pointing at the live root, and redirection to `logs/game_1791530994266.log`. Its full readback contains no explicit `OWNED_GRAPHICS_TRACE` or Streamline diagnostic environment setting. This describes the recorded command, not independently captured inherited process environment.
- `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/logs/game_1791530994266.log`: 11,099 lines; SHA256 `7b2de5a1824d57f43c2c68e5e42b5ca33412083d550da3c69fb0aeff95db6c4d`, as independently executed by Main and recorded in `/Users/hanbinnoh/Documents/dxmt-d3d12/doc/ulw-research/20261009-075042/verify-log-counts.md:7`.
- `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/ZenlessZoneZero_d3d12.log`: 11,030 lines; SHA256 `675b47f07d8a182d158980e147db937fc5b0d3b86232d7248851535328a83d02` in that verification document at line 8.
- Actual Player log: `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/wineprefix/drive_c/users/hanbinnoh/AppData/LocalLow/miHoYo/ZenlessZoneZero/Player.log`, 594 lines. Line 1 identifies `2026-10-09T07-30-01.zip`; lines 3–7 identify the actual `/Applications/Zenless Zone Zero` installation and forced D3D12.
- Additional timestamped game lifecycle source located through the actual configured installation path: `/Applications/Zenless Zone Zero/ZenlessZoneZero_Data/Persistent/LogDir/NAP_20261009_163009.log`, 514 lines. `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/.storage/game_install_dir.neustorage:1` identifies that installation root.
- `/Applications/Zenless Zone Zero/driverError.log:1-3` is dated **2026-09-26**, not this run. Its `initDriver Failed: Error [4,1114,0]` is stale evidence and must not explain the October 9 scene freeze.

### Exact repeated-error arithmetic

I paged both complete current log sinks through `read`, reconstructed lines using the returned `displayContent.startLine` to eliminate overlap, and executed JS frequency calculations over the readback. Final calculations agree with Main's independent direct-file verification:

| Failure message category | Game stdout log | D3D12 file sink |
| --- | ---: | ---: |
| PS shader resource not bound by root signature | 3,856 | 3,847 |
| VS shader resource not bound by root signature | 1,142 | 1,142 |
| VS constant buffer not bound by root signature | 814 | 814 |
| PS constant buffer not bound by root signature | 1,968 | 1,968 |
| PS sampler not bound by root signature | 1,305 | 1,305 |
| `vshs_main` shader resource not bound by root signature | 46 | 46 |
| **Sum of compile-failure log lines** | **9,131** | **9,122** |
| `CreateInputLayout ... expects POSITION_0 ... not in input layout` | 1,881 | 1,881 |

Arithmetic: `3856 + 1142 + 814 + 1968 + 1305 + 46 = 9131`; replacing the first term by 3847 gives 9122. Difference between sinks is nine PS shader-resource failure lines. Do not add the sinks together, describe these as unique shaders or frames, or transfer the historically diagnosed 64 captured shader/root pairs to these 9,131 lines. First compile failure is game log line 78 and D3D12 line 27. Both also contain 16 initial CS shader-probe `hr=0` lines; those initial successes do not establish successful later graphics PSO creation.

These failures are mostly untimestamped, so frequency and ordering are available but a reliable error-per-second rate, compile duration, or attribution to the visible frozen interval is not.

### DLSS/Streamline stage evidence and counterclaims

Current Player log anchors:

- `:8`: `[streamline] init flags=0x89 frameBasedTagging=1 disableCLStateTracking=1 enableCLStateTrackingArg=0 diag=0`.
- `:16-19`: `eErrorOSDisabledHWS`, `eErrorFeatureFailedToLoad`, `DLSSG is not requested or failed to load`, then `DLSS version: 310.6.0`.
- `:51-52`: two more `eErrorFeatureFailedToLoad` messages.
- `:445`: one **`eErrorFeatureMissing`**, without a feature ID or originating call.
- `:446-447`: two `[ffx] Upscaler Context VRAM totalUsageInBytes 8847360.000000 MB aliasableUsageInBytes 7990.252441 MB` lines, immediately after FeatureMissing. The printed units/values cannot safely be interpreted as actual resident GPU memory or OOM evidence. They are a useful clue that an FFX context/settings path ran, not proof that FSR was rendering or that DLSS fell back automatically.
- `:585`: `OnApplicationQuit`; `:593-594`: two additional `eErrorFeatureFailedToLoad` messages during shutdown. Literal-search count for FeatureFailedToLoad is **five total**, three during initial setup and two after quit; FeatureMissing is one; OSDisabledHWS is one; DLSSG-not-requested/failed is one.

Historical comparison root: `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/candidate-03`.

- Historical `Player.log:8` has exactly the same flags and tracking settings but **`diag=1`**.
- Historical `Player.log:16-19` and `:51-52` have the same HWS/DLSSG/FeatureFailed messages and DLSS version. This is the candidate with retained one-scene DLSS rendering evidence, so those startup messages alone do **not** establish DLSS SR failure or uniquely explain the latest regression.
- Historical Player log has **no** literal `eErrorFeatureMissing` or `Upscaler Context VRAM` match. The latest `:445-447` sequence is therefore a difference worth instrumenting, but the missing feature ID and timestamps prevent calling it the freeze cause.
- Historical `ZenlessZoneZero_nvngx.log:7`: create parse succeeds with `result=1`, input 640×360, output 1280×720, flags 74; `:8` backend `hr=0`; `:9` feature ready. `:13` evaluate parse succeeds for **624×360**, pre-exposure 1, sharpening 0; `:14` backend `hr=0`. Further backend success appears at `:19`, `:24`, `:29`, and `:34`. The preserved file ends in a partial line 38, so do not interpret the truncated final evaluate as a failure.
- Historical `game.log:3116-3118` places a successful evaluate backend just before the HUD `metalfx.timing already exist` message and a D3D12Texture unknown-interface warning. This illustrates why HUD metric registration is not an encode-completion diagnostic.

Latest game log `:76` is the only MetalFX text match: **16:30:50.850**, HUD metric already exists. There are no NGX parse/evaluate/backend trace records in the two current renderer sinks, and no `[SL-D3D12-DIAG]` in current Player.log. Current `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/owned_trace.hpp:19-22` enables traces only when `OWNED_GRAPHICS_TRACE == "1"`; `:45-52` silently returns when disabled or after the event quota. LSP confirmed actual references to this gate in command-queue present decisions and pipeline-cache diagnostics. Relevant history is commit `9e5ffa3` (`dxmt-dx12: standalone D3D12 to Metal translator`), returned by `git log -5 ... -- src/d3d12/owned_trace.hpp`.

Thus the absence is an **observability gap**, not evidence that NGX or MetalFX was never called. The latest logs cannot distinguish rejected evaluate input, successful command recording followed by queue skip/native rejection, stale successful output/resource reuse, or other scene-only rendering failure.

### Resource-lifetime warnings are not a unique regression signature

Current Player.log `:75`, `:296-297` reports LUT textures released while Async SRP is running. `:325-327` reports `moon_bright` release and `_MoonTex` ShaderPassContext mutation during async submit.

Exact historical counterexample: candidate-03 `Player.log:75`, `:236-237` has the same LUT-release categories; `:5614-5616` has the same `moon_bright` fatal/mutation messages. Historical `:5617-5618` then continues with `slSetTagForFrame` and `slEvaluateFeature`, with further evaluations afterward. These warnings may merit investigation, but they neither prove current translator ownership is broken nor discriminate the latest frozen scene from the historically rendered scene.

### Timestamped lifecycle, not a frame-time benchmark

`/Applications/Zenless Zone Zero/ZenlessZoneZero_Data/Persistent/LogDir/NAP_20261009_163009.log` records:

- `:105`: initial `EnterScene`, 16:30:31.796, frame counter 497. This precedes city loading and must not be called city-render completion.
- `:257`: MouseInputEnhancement startup at 16:30:49.486, frame 862.
- `:258`: next logged event at 16:34:25.717, frame 13,108. The **216.231-second log gap** is not a measured loading stall: the frame counter advances and no start/end precompile sentinel is present.
- `:280`: later scene load begins at 16:34:28.554; `:286` avatar loaded at 16:34:32.796; `:289-290` NPC/entity load completion at 16:34:35.367–.370; `:297-298` EnterScene/SceneSwitch completes at **16:34:36.078–.081**, frame 13,303.
- Exact launch-command-to-later-EnterScene interval: **281.812 seconds** (`16:34:36.078 − 16:29:54.266`). Actual later scene-load-to-EnterScene interval: **7.524 seconds** (`16:34:36.078 − 16:34:28.554`). Neither is controlled shader compilation time or FEX overhead.
- `:301`, `:330`, `:359`: serialized-layout errors in the menu background asset path, at 16:35:04.211 and 16:35:14.808–.809. These are an alternative game-side warning category, not established DLSS causal evidence.
- `:392` another scene load starts at 16:35:39.934; `:476-477` EnterScene/SceneSwitch completes at 16:35:56.109–.116, frame 16,219.
- `:478`: another menu asset error at 16:36:36.397, frame 17,791. `:507` OnApplicationQuit at 16:36:47.448, frame 18,292; `:511-512` quit messages at 16:36:52.091–.109.

This establishes continued engine/lifecycle activity after city entry and is compatible with the user's moving HUD observation. Engine frame counters are not measured presented frames, and scene milestones do not prove updated 3D output. No FPS/GPU/CPU timing series exists in the owned latest renderer logs. HUD `frame-interval`, `present-delay`, `fps`, and MetalFX timing metric registrations are names, not samples. The watermark configuration prints 3840×2160 in Player.log/NAP; that is not a validated render-resolution measurement and must not be compared directly to historical 1280×720 as a proven workload difference.

### Streamline log discovery and limits

I searched the actual configured installation and live prefix for Streamline/sl/NGX log and config filenames and enumerated all live `*.log`, `*.json`, `*.ini`, and `*.conf` paths. No standalone current `sl.log`, `sl.*.log`, Streamline log/config, or current `ZenlessZoneZero_nvngx.log` was found. Current Streamline messages are available in **Player.log**; the game stdout has no `SL[` entries. Prefix `user.reg` has no literal Streamline key; the `system.reg` search found none in the tool's first-4-MB search window, so registry absence outside that window is not claimed. The retained candidate has explicit `ZenlessZoneZero_nvngx.log` and Player diagnostic lines. No native binary probes were needed or performed.

### Minimal recommendations and claim candidates

1. Prioritize a **bounded future diagnostic reproduction** with Streamline diagnostics and owned traces explicitly enabled before launch, capturing feature ID, tag frame, command-list ID, evaluate parse/result, backend HRESULT, native scaler identity/input/output/reset, queue execution, and output presentation. Current assignment prohibits launch; this is the precise missing evidence, not a request to fix by guesswork.
2. Treat current shader failure volume as a separate confirmed problem. Capture representative current failed shader/root pairs with callsite and stage before claiming the previous 64-pair explanation applies or relaxing binding validation. Current logs cannot tell which are warmup-only versus drawn PSOs.
3. Investigate `Player.log:445-447` as the first latest-only feature/context clue. Preserve feature identity and active selected upscaler when reproducing; do not equate FeatureMissing with DLSS SR failure from its unqualified text.
4. For performance, collect matched scene/settings/resolution/cold-or-warm runs and actual CPU/GPU/present timing. These logs provide no numerical D3DMetal/Rosetta-versus-DXMT/FEX ratio.

Supported claim candidates: latest run is D3D12 with HUD enabled; 9,131 compile-failure log lines and 1,881 layout warnings; initial DLSS version reporting occurs despite DLSSG/HWS failures; current diagnostic settings differ from historical candidate; game lifecycle continues through later scene switches and quit. Unsupported/hypothesis-only candidates: FEX is the dominant current FPS bottleneck; every shader error causes scene freezing; startup DLSSG/HWS errors explain latest DLSS SR regression; LUT/moon warnings prove translator resource-lifetime failure; metric registration proves native encode success; missing trace proves NGX was not invoked.

## EXPAND

- **LEAD — latest-only FeatureMissing plus FFX context sequence:** Player.log:445-447. Needs feature ID/active-upscaler/callsite, not obtainable from present logs; NgxBoundary informed.
- **LEAD — latest shader-pair provenance:** 9,131 failures lack shader hash/root dump/callsite. Distinguish warmup-only failures from PSOs required by the frozen scene before applying historical binding diagnosis.
- **LEAD — trace-gated missing temporal evidence:** latest launcher command lacks explicit trace enable and Player diagnostics are off. QueuePresent and NgxBoundary informed; absence cannot settle stage.
- **DEAD END — startup HWS/DLSSG messages as unique freeze cause:** identical errors coexist with historical one-scene DLSS rendering.
- **DEAD END — Async SRP moon/LUT warnings as sufficient cause:** same warnings exist in historical rendered candidate with continued evaluate calls; MetalTemporal and EvidenceSkeptic informed.
- **DEAD END — old driverError.log as current failure:** September 26 timestamp excludes it from October 9 run attribution.
- **DEAD END — FPS inference from log counts, engine counters, or HUD registration:** no presented-frame/timing samples; cannot produce a controlled performance ratio.