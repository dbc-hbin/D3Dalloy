## MetalTemporal findings

Scope: read-only analysis of HEAD f04bd8a native SR implementation, its D3D12 handoff, shipped macOS SDK contracts, and latest retained logs. No game launch, GPU probe, source edit, or test execution. All source paths below are absolute under `/Users/hanbinnoh/Documents/dxmt-d3d12/` unless an SDK/log prefix is given explicitly. Code mechanisms are verified statically; no native MetalFX failure is established for the latest game execution.

### 1. Latest evidence cannot distinguish stale MetalFX output from stale input supplied to MetalFX

I independently searched `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/ZenlessZoneZero_d3d12.log` for `temporal|Temporal|metalfx|MetalFX|NGX|ngx|887a0020|submission failed`: no matches. `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/logs/game_1791530994266.log:76` contains only `Metric com.apple.hud-label.metalfx.timing already exist`, at 16:30:50.850. This proves HUD metric registration, not a completed scaling pass or changing output. Lead-owned executed counts in `/Users/hanbinnoh/Documents/dxmt-d3d12/doc/ulw-research/20261009-075042/verify-log-counts.md:7-17` distinguish 9131 game-sink compile failure lines from 9122 D3D12-sink lines; these are not frame or unique shader counts.

`/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/wineprefix/drive_c/users/hanbinnoh/AppData/LocalLow/miHoYo/ZenlessZoneZero/Player.log:8-19` reports Streamline `diag=0`, owned MetalFX FSR version strings, HWS failure and DLSSG not requested or failed to load, followed by DLSS version 310.6.0. Version strings establish loaded API identity, not evaluation success. FG is therefore excluded from the primary diagnosis; no enabled interpolation evidence was found.

Absence of temporal traces is not absence of temporal calls. `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/owned_trace.hpp:19-35,44-54` requires `OWNED_GRAPHICS_TRACE=1`; the process-global trace limit defaults to 8192 and caps at 1000000, with logging through `Logger::info`. No matching environment strings were found in the latest `neutralinojs.log`; that does not establish the actual process environment. The latest sinks lack the data needed to assert color updates, output updates, input dimensions, generation switches, or reset frequency.

### 2. Direct resource lifetime and in-flight scratch reuse have substantial explicit protection

D3D12 retains both application resources and exact texture views in `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_temporal.cpp:43-74`. Native prepare separately retains color, depth, motion, output, exposure and masks in `/Users/hanbinnoh/Documents/dxmt-d3d12/src/winemetal/unix/metalfx-backend.mm:1948-1957`; destruction releases them at :720-728. Prepared frames retain the scaler generation independently of feature-cache eviction (:690-693).

Native transport attaches `DXMTTemporalExecution`, containing both prepared frame and lease, to the recording before encoding any GPU command: `/Users/hanbinnoh/Documents/dxmt-d3d12/src/winemetal/unix/winemetal_metal4_fx.mm:110-124`. `PreparedFrame::encode` publishes its lease before recording commands, even on subsequent failure: backend :2056-2064. `/Users/hanbinnoh/Documents/dxmt-d3d12/src/winemetal/unix/winemetal_metal4.c:521-539` moves recording ownership into the submission; :494-499 retires leases only after both commit return and feedback receipt. Split buffers share recording and allocator ownership (:133-150).

Completed scratch is returned through `ExecutionLease::Impl` (:666-686), not on encode return. Pools exchange objects out when taken (:449-459), match exact dimensions/format/usage (:334-343), and empty residency sets before general scratch reuse (:516-541). Cached replay bundles are restricted to the same prepared frame (:577-585), with at most two bundles. Dormant-feature handling drains idle objects while retaining active executions (:626-645). Scaler retained bindings are cleared on all encode exits (:1818-1842), while the lease maintains GPU lifetime.

Counterclaim: raw texture reuse or premature feature release is not an established current defect. Changing these mechanisms without runtime evidence risks removing existing safeguards. Lifetime still depends on native feedback accuracy and queue retirement, which are owned by QueuePresent.

### 3. Output placement and texture contracts are actively translated, not blindly passed to MetalFX

Backend :894-949 validates subrect bounds, active dimensions, creation capacity, motion-resolution mode and device scale range. Native descriptor input dimensions represent observed capacity, while per-frame content dimensions are set separately (:1190-1203,1860-1861). Nonzero input offsets, smaller active extents or incompatible usage cause exact-capacity staging (:1975-2004). Output receives a private exact-size shadow when shared, larger than the logical output, offset, sharpened/transferred, capped, debug-rendered or alpha-upscaled (:2005-2012); copyback writes the caller subrect at :1788-1806. Exposure not already 1x1 R16F is converted numerically (:126-129,135-144).

Cross-check: `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/MetalFX.framework/Versions/A/Headers/MTLFXTemporalScaler.h:204-214` permits changing texture identities as long as descriptor formats and dimensions match; :271-284 requires private output and 1x1 R16F exposure. The code handles these requirements. A repeated texture object ID therefore does not mean repeated contents or frozen output.

Input padding is not simply left stale: backend :1632-1668 extends active edges by doubling copied strips, with explicit blit barriers (:1671-1677). For capacity gaps dx and dy, padding uses `1 + ceil(log2(dx))` horizontal blits when dx > 0 and `1 + ceil(log2(dy))` vertical blits when dy > 0. This is a static command-count calculation, not measured runtime cost.

### 4. History reset is already implemented across creation, dimensions and replay transitions

Backend :1134-1144 reactivates alternate mask-presence generations with an activation increment. :2070-2075 resets on force/caller reset, activation mismatch, or active-input extent change. Successful encode alone updates last dimensions and activation (:2180-2185). D3D12 also resets whenever executed generation changes (:233-237), serializes each feature using a shared-event history timeline (:229-232), and zeroes last executed generation after encode failure (:243).

Dynamic output dimensions create a replacement feature in `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_temporal.cpp:163-178`; old recordings retain their old native feature through prepared-frame ownership. The public regression source `/Users/hanbinnoh/Documents/dxmt-d3d12/graphics-translator/tests/owned-d3d12-temporal.cpp:1195-1259` explicitly tests A,B,A dynamic-output replay against fresh-reset output. :1128-1192 covers generation reset sequence 1,1,1,0; :1500-1542 checks eight consecutive changing input tags, typed and typeless resources. These are existing test intentions, not tests I ran or evidence for latest gameplay.

Hypothesis: dimension or mask-presence churn could cause repeated synchronous scaler creation/reset and harm FPS. Descriptor initialization is explicitly synchronous (:1197-1201); the SDK documents additional creation-time compilation at `MTLFXTemporalScaler.h:52-74`. No latest dimension/generation series supports this hypothesis. Another contract gap is aspect-ratio variation: the SDK says dynamic scaling assumes unchanged aspect ratio (:76-84), while current validation checks axes independently and high-water capacity is componentwise (:1147-1173). An actual aspect-ratio-changing sequence would be needed before treating this as a current bug.

### 5. Failures can leave caller output stale, but silent native failure is not proven

Prepare preallocates the first complete lease before returning success (:2014-2019), so initial scratch/residency allocation failure is synchronous. `/Users/hanbinnoh/Documents/dxmt-d3d12/src/d3d12/d3d12_command_list.cpp:1637-1652` propagates prepare failure and appends no temporal command. It does not clear or replace the caller output on rejection. Whether the game subsequently presents that unchanged resource depends on NGX/game HRESULT handling, not established in this lane.

During native encode, prefix copies/conversion, MetalFX and output finishing occupy separate native recording segments (:2113-2166). The MetalFX scaler intentionally receives nil fence (:2131-2132); prefix/suffix queue-stage barriers supply ordering. This is not evidence of a missing fence: the source explicitly isolates MetalFX to avoid an external same-buffer fence-consumer deadlock. A failure before output copy/finish leaves no successful new caller-output write, and partial commands/resources remain retained. D3D12 reports `DXGI_ERROR_DRIVER_INTERNAL_ERROR` on native HRESULT failure (:239), always commits/signals the history sequence (:240-242), and resets generation history. Thus the simple story “encode errors silently keep returning success forever” conflicts with this source path.

Important observability limit: native `fx::Error.message` is discarded by `/Users/hanbinnoh/Documents/dxmt-d3d12/src/winemetal/unix/winemetal_metal4_fx.mm:35-45,101-104,120-125`. Multiple native allocation/descriptor/encode reasons collapse to one HRESULT. Apple native `encodeToCommandBuffer` returns void (`.../MTL4FXTemporalScaler.h:28-34`); successful wrapper return means command recording did not raise a caught exception, not that GPU output changed. GPU completion feedback and output inspection are required.

### 6. Aliasing and overhead candidates, with limits

Prepare rejects any input COM resource exactly equal to output at d3d12_temporal.cpp:120-123. It does not compare underlying native parent texture/buffer identities or overlapping placed-resource allocation ranges. Backend texture validation also does not reject underlying input/output overlap. This is an unverified aliasing possibility, not evidence that ZZZ supplies overlapping resources. Counterclaim: separate D3D12 resource identity alone cannot prove distinct backing; application aliasing contracts and explicit barriers must also be considered.

Static overhead includes minimum two additional native command-buffer splits per SR encode, prefix and suffix all-stage queue barriers, per-feature history event waits/signals, and staged-input/output copies whenever texture backing exceeds logical extent. Each single uncompressed texture copy entails approximately `2 * width * height * bytesPerPixel` bytes of reads plus writes, excluding cache effects, padding and MetalFX's internal traffic. No actual latest dimensions/formats or GPU timings are present, so no numeric bandwidth cost or FPS improvement is claimed. Generation replacement on dynamic output and synchronous compilation are additional conditional overhead, not proven steady-state bottlenecks.

Relevant history: 7a307b605fe29788be12c104f4a4f9eb02e5819b changed motion-view retention from twice to once, preserving unsupported-format rejection; b7c11e6 removed legacy SR command-mode branches and retained this Metal4 path. Its commit text explicitly labels dated FG/RR GPU observations historical because artifacts were removed and not rerun. Neither commit proves sustained latest-game SR output.

### Minimal recommendations and claim candidates

1. Establish temporal_record/temporal_encode, prepare HRESULT, native stage/error.message, native GPU feedback and changing input/output evidence for the same frozen-scene interval. Trace flags and bounded limits must be verified in the actual process. Existing traces alone record resource IDs, not content hashes.
2. If output stays fixed while color changes and native feedback succeeds, inspect native output shadow then caller output to distinguish scaler/history from suffix copy/finish. If color itself stays fixed, prioritize producer shader/draw failures rather than resetting MetalFX blindly.
3. Measure staging flags, dimensions, generations, reset frequency, segment counts and GPU times before changing synchronous compilation, staging or synchronization. Use existing temporal regression cases to preserve old-record replay and lifetime semantics.
4. Add underlying-alias or aspect-ratio handling only after an actual violating frame is captured; these are code-contract leads, not justified freeze fixes today.

Supported observation: the current source has explicit retained-resource lifetime, scratch replay separation, first-frame preallocation and reset logic. Supported observation: latest logs expose no SR evaluation outcome or output-content evidence. Unsupported causal claim: MetalFX history is frozen, native output allocation failed, or FEX/MetalFX staging explains the current FPS gap.

## EXPAND

LEAD: Same-run input/private-output/caller-output content progression and GPU feedback are the decisive missing evidence. No permissible retained capture in this lane supplies it.
LEAD: Native error messages are discarded; exact stage logging would discriminate allocation, copy, split and exception failures without interpreting one generic HRESULT.
LEAD: Underlying placed-resource alias overlap and dynamic aspect-ratio change are untested contract possibilities requiring actual frame metadata.
LEAD: Synchronous generation churn, scratch staging, two native splits and full-stage barriers are measurable performance candidates; latest logs provide no timing attribution.
DEAD END: Simple premature-release or same-PreparedFrame in-flight scratch reuse diagnosis conflicts with the retained frame/lease/recording/submission ownership chain.
DEAD END: 'No reset implementation' conflicts with native activation/input-extent resets and D3D12 generation reset.
DEAD END: HUD MetalFX metric and FSR/DLSS version strings are not encode or sustained-output proof.
DEAD END: FG freeze attribution lacks enabled-DLSSG evidence in latest Player.log; FG internals were not expanded.