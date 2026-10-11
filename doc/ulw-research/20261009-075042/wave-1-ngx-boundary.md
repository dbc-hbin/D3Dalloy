## NgxBoundary findings

Scope: read-only analysis of HEAD f04bd8a and retained logs. No game launch, native attach, source edits, or tests. All source paths below are absolute under `/Users/hanbinnoh/Documents/dxmt-d3d12/`.

### 1. Latest run does not expose the NGX failure/success boundary

The latest launcher command at `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/neutralinojs.log:569` explicitly sets `MTL_HUD_ENABLED=1`, `DXMT_LOG_PATH`, and `DXMT_CONFIG_FILE`, but does not set `OWNED_GRAPHICS_TRACE`. The absence of this variable in the recorded command does not exclude inheritance from the launcher environment. Nevertheless, the latest game and D3D12 logs have no owned NGX parse/backend or temporal record/encode events. The sole MetalFX match in `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/logs/game_1791530994266.log:76` is `Metric com.apple.hud-label.metalfx.timing already exist` at 16:30:50.850. That is not a successful Evaluate, fresh output, or frame timing measurement.

The observability gap is explained by the static logging contract: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/owned_trace.hpp:19-21` enables tracing only when the environment value equals `1`; `:25-35` defaults to 8,192 events and caps a requested limit at 1,000,000; `:38-54` uses a shared whole-DLL counter. Thus no trace cannot be interpreted as no NGX call, and even an enabled trace can stop before the failure when other resource/present events exhaust its budget.

**Claim candidate:** The newest run cannot yet distinguish NGX input rejection, accepted recording with later native failure, or successful upscaling of stale scene input. Latest shader compile failures are runtime evidence from another lane; NGX failure remains unproven.

### 2. D3D12 does propagate record-time failures; it does not falsely return success for a rejected frame

`/Users/hanbinnoh/Documents/dxmt-d3d12/src/nvngx/nvngx_d3d12.cpp:1012-1052` resolves a live feature, validates the caller command list and device, retrieves owned parameter storage, parses a fresh descriptor, and returns immediately on errors. At `:1050-1052` it returns `mapBackendResult(extension->TemporalUpscale(...), false)`.

The backend call at `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_command_list.cpp:1637-1652` validates recording and calls `PrepareOwnedTemporalFrame`. A failed preparation returns before `AllocatePass`; only a successful preparation creates the temporal encoder. The mapping in `/Users/hanbinnoh/Documents/dxmt-d3d12/src/nvngx/nvngx_d3d12.cpp:285-295` is:

- Successful HRESULT -> `0x00000001`.
- E_INVALIDARG/E_POINTER -> `0xBAD00005`.
- E_NOINTERFACE/E_NOTIMPL/DXGI_ERROR_UNSUPPORTED -> `0xBAD00001`.
- Other evaluation failures -> `0xBAD00002`; other creation failures -> `0xBAD0000B`.
- Missing required key/type -> `0xBAD0000A`; present null required resource -> `0xBAD00005` (`:237-246`).

The older D3D11 implementation in `/Users/hanbinnoh/Documents/dxmt-d3d12/src/nvngx/nvngx.cpp:155-161` calls its context TemporalUpscale and then returns success without a corresponding HRESULT. That is a separate path and must not be attributed to this D3D12 run.

Accepted recording is still not GPU success. Native encode happens later in `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_temporal.cpp:225-243`; an encode error at `:239` reports device removal and `:242` clears executed-generation tracking. It cannot retroactively change the NGX success already returned to the game. A game continuing to composite HUD over an untouched previous output is therefore structurally possible after a rejected Evaluate, but the game’s response to that return and the actual latest return value are missing. This is a hypothesis, not an established cause.

### 3. One concrete SR state-contract omission, not a demonstrated frozen-output mechanism

`/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_command_list.cpp:1602-1613` records all FG resource uses with `RecordImplicitUse`, whereas SR `TemporalUpscale` at `:1637-1652` does not record any implicit input/output use before appending the temporal pass.

QueuePresent independently checked that `RecordImplicitUse` at `d3d12_command_list.cpp:5754-5758` feeds promotion/decay/state validation, not actual native synchronization. Its expected consequence is a later before-state bookkeeping mismatch, rather than automatically stale native output. Actual temporal GPU ordering still exists: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_command_queue.cpp:6434-6478` splits submission segments, retains the frame in-flight, and executes its native encode; `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_temporal.cpp:230-242` waits/signals a per-feature timeline and serializes history.

**Minimal recommendation:** separately correct SR implicit resource-use bookkeeping with the existing mechanism and a COMMON/promotion/decay contract regression if implementation is authorized. Do not present that correction as the scene-freeze fix without a matching state failure. The currently available sinks contain no corroborating state mismatch or device-removal evidence.

### 4. Parameter alias bug was already fixed; latest recurrence is not proved

`/Users/hanbinnoh/Documents/dxmt-d3d12/src/nvngx/nvngx_parameter.hpp:85-100` explicitly preserves `void* <-> ID3D12Resource*` aliases, addressing Streamline’s pointer overload. Missing keys/type mismatches fail, while a stored null remains a successful lookup and is rejected by the required-resource frontend. Numeric values and typed D3D11 resources are not converted to D3D12 resources. The store is overwritten by Set and cleared by Reset (`:201-238`); it does not cache one selected scene texture inside FeatureState.

Git commit `afb9d18` introduced these aliases and a CPU fixture. The fixture at `/Users/hanbinnoh/Documents/dxmt-d3d12/graphics-translator/tests/owned-ngx-parameter-consumer-msvc.cpp:17-38` covers void-to-D3D12, typed-to-void, cross-API rejection, missing/null distinction, numeric rejection, and Reset. It was read, not rerun here. Latest runtime loaded-image identity is RuntimeIdentity’s lane; source correctness alone does not prove its loaded DLL.

The public Microsoft ABI facade has the same Set/Get/Reset virtual order as the pinned official header: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/nvngx/nvngx_d3d12_msvc.cpp:32-51` versus `/Users/hanbinnoh/Documents/dxmt-d3d12/external/nvngx/include/nvsdk_ngx_params.h:54-77`; `:58-78` forwards directly to the common store. D3D12 public Get callbacks translate legacy store success/failure into the proper public result constants (`nvngx_d3d12.cpp:749-765`). The legacy internal failure mask is not evidence of incorrect public D3D12 results.

Counterclaim exhausted: the unsynchronized map is not by itself an API-contract defect. Official `/Users/hanbinnoh/Documents/dxmt-d3d12/external/nvngx/include/nvsdk_ngx.h:18-19` explicitly says the library is not thread-safe and the caller must enforce thread safety. There is no current evidence of simultaneous mutation of the same map. Adding locks based only on this observation would be speculative.

### 5. Frame contract and plausible rejection alternatives

`/Users/hanbinnoh/Documents/dxmt-d3d12/src/nvngx/nvngx_d3d12.cpp:441-527` reconstructs the frame descriptor on every Evaluate:

- Color, Output, Depth, MotionVectors are required. Exposure, bias/reactive mask, transparency/composition mask are optional (`:449-461`).
- Render dimensions fall back to create dimensions only when both are absent; exactly one present is missing input; zero dimensions are invalid (`:463-473`).
- All input offsets and output bases are read anew. Nonzero output base without create-time subrect opt-in returns UnsupportedParameter (`:479-499`). Capability parameters do not pre-enable output subrects (`:373-378`).
- Jitter, motion scales, exposure values, sharpness, and motion offsets must be finite. Zero motion scales, negative exposure, or sharpness outside [0,1] are rejected (`:501-517`). Raw PreExposure zero becomes 1 (`:519`). Nonzero sharpness without the sharpening create flag is rejected (`:522-523`). Caller Reset is forwarded (`:526`).
- Validated Exposure.Scale and MV.Offset are intentionally ignored according to the source’s GPTK compatibility policy (`:520-521`). This could matter for another application’s quality contract, but latest values and any effect are not recorded; there is no frozen-scene proof.

Backend preparation further rejects wrong device, non-2D/sliced/multisampled/zero textures, unsupported motion formats, output aliasing inputs, invalid scalars, or mask offsets without their masks: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_temporal.cpp:34-62,94-123`. Resource/view references and immutable native frame properties are retained before returning (`:65-72,124-184`). This is not a borrowed parameter-map-after-return design.

Optional getters (`nvngx_d3d12.cpp:249-282`) conflate absent keys and incompatible stored types by using defaults. Thus a mistyped optional Reset or offset can silently default, whereas mistyped required resources are rejected. This is a static compatibility risk, not proof of a legal caller regression; capture actual parameter types before changing acceptance semantics.

The backend records input texture identities, not the texture’s pixel contents. Successful repeated Evaluate with the same resource pointer is normal and does not establish fresh scene rendering. A fresh/stale input comparison would need correlated scene-color and temporal-output pixel evidence or GPU producer evidence, not resource identity alone.

### 6. Historical positive evidence narrows, but does not close, the latest diagnosis

Directly read retained `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/candidate-03/game.log:3105-3178`. It shows feature1 creation with 640x360 input, 1280x720 output, flags74, parse result1, backendhr0. Eleven complete evaluation records show active624x360, PreExposure1, sharpening0, list/handle validation success, and backendhr0. That count follows event IDs10..64: `(64 - 9) / 5 = 11` complete Evaluate groups. The smaller NVNGX sink cuts mid-event38 and should not be used to infer the full count.

Flags74 = 64 AutoExposure + 8 jittered MV + 2 low-resolution MV. The create scale is2 in both dimensions; the active content width ratio is1280/624 = 2.051282051..., height ratio720/360 =2. This existing successful record is a counterclaim to assuming that the624-wide active content is automatically invalid because creation width is640.

These records are from the earlier isolated diagnostic run. They prove accepted recording during those eleven calls, not all subsequent frames, current live execution, long-play stability, native completion, or FPS. Its retained result explicitly limits the visual proof to one scene and says the later screenshot was the same camera view: `/Users/hanbinnoh/Documents/dxmt-d3d12/build/local/zzz-fex-opt-20261009/result.json:10,15-18`. The matching MetalFX HUD metric also appears immediately after the first historical accepted Evaluate at `candidate-03/game.log:3117`, which makes the latest similar metric suggestive but not sufficient to infer its actual return.

### 7. Performance boundary observations, not measured bottlenecks

For normal feature1 evaluation, source performs three global-registry lock acquisitions: feature lookup (`:530-536`), device-initialization check inside list validation (`:188-199,223`), and parameter lookup (`:112-118,1036`). `parseFrame` executes31 named parameter lookups:4 required resources +3 optional resources +2 dimensions +10 input/mask offsets +2 output offsets +9 float scalars +1 Reset. The unordered map uses `std::string` keys (`nvngx_parameter.hpp:207-223`), so this path has hashing/string-conversion work. Backend additionally performs resource metadata checks and view resolution for every supplied texture.

These are static per-Evaluate CPU costs. No executed attribution sample here identifies them as material, and no quantitative FPS improvement can be promised by caching them. View lifetime work was already simplified by commit `7a307b6`, which removed a second motion resource retention/view-selection pass and retained unsupported-format rejection. GPU temporal work also causes submission segmentation, but QueuePresent/RenderPerformance own that cost. Fixing the verified shader-input production failures is more evidence-based than optimizing the31-lookups boundary or skipping temporal validations.

## Minimal next diagnostic recommendation

On a separately authorized diagnostic reproduction, enable existing NGX and temporal tracing with enough budget to cover the failure, correlate each Evaluate return to its temporal_record/temporal_encode resource and generation IDs, and record any native error/device-removal result. Capture scene-color versus output freshness at the freeze. Do not automatically retry, force Reset every frame, fabricate success, or bypass resource validation: each can obscure which boundary failed and none has latest-run causal support.

## EXPAND

- LEAD: Exact latest Evaluate result and first frame that stops producing fresh output. Missing data: launcher did not explicitly enable OWNED_GRAPHICS_TRACE; latest sinks lack parse/backend/record/encode results. Existing diagnostics need a future authorized reproduction, not another game launch in this research task.
- LEAD: SR implicit promotion/decay bookkeeping omission at command_list.cpp:1637-1652. Concrete static omission cross-checked with QueuePresent; low-confidence freeze cause because native ordering exists and the latest sinks have no matching state error.
- LEAD: Distinguish stale scene input from stale temporal output. Resource IDs alone cannot prove pixel freshness; shader producer failures provide a competing hypothesis. Needs correlated input/output evidence at the actual freeze.
- LEAD: Mistyped optional parameters default silently. Needs actual Set type/key capture before declaring a caller-compatible static defect or changing accepted input behavior.
- DEAD END: Previously missing void*/D3D12-resource alias. Fixed in current source by afb9d18 and historically positive game boundary evidence; recurrence requires contrary loaded-DLL identity evidence.
- DEAD END: D3D12 unconditionally swallowing record-time failure. Source propagates it; only later asynchronous encode can fail after prior NGX success.
- DEAD END: Unsynchronized parameter storage as self-evident SDK defect. Official API is explicitly not thread-safe; no runtime data race proof.
- DEAD END: Legacy D3D11 unconditional success and legacy failure constants as explanation of this D3D12 run. Paths and public result conversions are separate.
- DEAD END: Existing MetalFX HUD metric, source support flags, or historical candidate PASS as proof of current successful output or an FPS ratio. Those evidence scopes do not establish those claims.