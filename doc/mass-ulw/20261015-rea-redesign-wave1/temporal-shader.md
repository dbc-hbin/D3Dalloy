# Temporal/present and shader/indirect redesign harvest (wave 1)

INTENDED PATH (copy verbatim): /Users/hanbinnoh/Documents/dxmt-d3d12/doc/mass-ulw/20261015-rea-redesign-wave1/temporal-shader.md
(Lane `temporal-shader-harvest`; this lane's write tool exposes only `local://` scratch and `xd://` devices, so the working-tree file must be created by Main from this content.)

- Repository: `/Users/hanbinnoh/Documents/dxmt-d3d12` at the working tree of 2026-10-15.
- Method: read-only. Current source read directly, retained REA/D3DMetal and GPU evidence re-read, no build, no runtime, no edit outside this file.
- Queue implementation lane owns `src/d3d12/d3d12_command_queue.cpp`; nothing below asks that file to change.

## 0. Evidence labels used

| Label | Meaning |
| --- | --- |
| `[OBSERVED]` | Read directly from the current working tree in this session; path:line given. |
| `[MEASURED]` | Retained controlled observation with a preserved receipt under `build/local/`. Numbers quoted verbatim from that receipt. |
| `[REA]` | Retained reverse-engineering evidence for the pristine D3DMetal binary. Shipped-artifact evidence, not source. |
| `[INFERRED]` | Performance consequence or activation likelihood that no retained capture proves. Never a claim of FPS. |

Pristine reference identity (re-verified this session from the retained receipt): `/Users/hanbinnoh/Downloads/D3DMetal.framework/Versions/A/D3DMetal`, x86_64, SHA-256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad` (`build/local/d3dmetal-opt-port-20261009/present-contract/identity.json`, `verified_by: ["shasum -a 256", "REA open_binary"]`, `execution: "Static only; no GPU, Wine, game or GUI launched."`).

## 1. Observed source facts

### 1.1 Temporal upscale (SR/RR) - `src/d3d12/d3d12_temporal.cpp`

- `[OBSERVED]` One frame retains 12 input resources; each retained input resolves a texture view through `createView` + `view` on the resource (`d3d12_temporal.cpp:59-61`), for `Color, Depth, MotionVector, Output, Exposure, BiasCurrentColorMask, TransparencyMask, DiffuseAlbedo, SpecularAlbedo, Normals, Roughness, SpecularHitDistance`.
- `[OBSERVED]` Frame prepare takes the per-feature mutex (`:161`) and, when `DynamicOutputEnabled` and the frame's output extent differs from the active feature's, creates a **new** `WMT4::TemporalFeature`, prepares the frame on it, then `markDormant()`s the previous feature and swaps it in (`:162-178`). `core_->feature_output_width/height` are the only key.
- `[OBSERVED]` The frame's `generation` is an **out** value of the native prepare: `WMT4::TemporalFeature::prepare(const WMTMetal4FXTemporalFrame &, int32_t &result, uint64_t &generation)` (`src/winemetal/Metal4FX.hpp:54-62`, `generation = call.ret[2]`), written at `d3d12_temporal.cpp:171` from `call->ret[2] = frame->generationId()` (`src/winemetal/unix/winemetal_metal4_fx.mm:102`), which is the scaler-generation serial (`metalfx-backend.mm:2020-2022`, `impl_->generation->serial`).
- `[OBSERVED]` Encode takes the same feature mutex, waits on the feature timeline for the previous evaluation, and computes `reset = frame->props.reset || core.last_executed_generation != frame->generation` (`:230-233`), then always signals the timeline after the native encode (`:242-243`).
- `[OBSERVED]` The native feature is strongly owned by prepared frames (`metalfx-backend.mm:689-690`, `std::shared_ptr<Feature::Impl> feature`), and `markDormant()` only drains pools and stops future pooling (`metalfx-backend.mm:626-641`); the 8-slot scaler-generation cache (`metalfx-backend.mm:423`, `1124-1136`, `1259-1261`) and all idle scratch are destroyed with `~Impl` (`metalfx-backend.mm:647-653`).
- `[OBSERVED]` Device-level serialization exists for temporal submissions: `std::mutex owned_temporal_submission_mutex` (`d3d12_device.hpp:972-973`) is taken by a queue worker after reserving its ring slot (`d3d12_command_queue.cpp:5051-5052`) and held across the temporal segment.
- `[OBSERVED]` FSR3 sets `create.DynamicOutputEnabled = TRUE` (`src/fsr/fsr.cpp:284`) with `create.OutputWidth/Height = maxUpscaleSize` (`:276-277`) and passes per-frame `frame.OutputWidth/Height = upscaleSize` (`:200-201`).

### 1.2 Frame generation - `src/d3d12/d3d12_frame_generation.cpp`

- `[OBSERVED]` `Prepare` (`:120-159`) and `Generate` (`:160-240`) each build a fresh `OwnedFrameGenerationFrame` per call and retain their inputs through `retainTexture`: 2 inputs at `:125-126`, up to 7 at `:171-176`; each then issues one native request under the per-core mutex.
- `[OBSERVED]` The FG native context is created once (`WMTFG_CREATE`, `d3d12_frame_generation.cpp:249-252`) and the FG encode shares the same per-core timeline pattern as temporal: wait for the previous sequence, encode, signal (`:287-300`).
- `[OBSERVED]` The FG state ABI is separate from temporal (`src/d3d12/d3d12_frame_generation_interfaces.hpp`, GUIDs `b2a31090-...-20/21/22`), so a temporal change does not move FG.

### 1.3 Present and drawable

- `[OBSERVED]` `EncodePresent` first blocks until the predecessor present's order event reaches `frame_id - 1`, polling in 100 ms slices and aborting on stop/device removal (`d3d12_command_queue.cpp:8243-8251`).
- `[OBSERVED]` Composition (flip-sequential) history is copied into the current history texture with previous/dirty-rect blits before the presenter runs (`:8270-8318`), and after presenting, the composed result is mirrored back into the backbuffer (`:8319-8332`).
- `[OBSERVED]` The presenter encodes the present and the queue then blocks on `queue_.waitForDrawable(drawable)` (`:8329`), sets `scope.signal_event = order_event; scope.signal_value = frame_id` (`:8330-8331`), calls `WMT4::ReserveMetal4Present(...)` (`:8346-8350`) and hands the drawable to the completion thread; Core Animation receives it only after the command buffer completes and only if no `DXGI_PRESENT_RESTART` advanced the epoch (`:8340-8341` comment, decision at `:965-983`).
- `[OBSERVED]` Presenter contract: "GPU timeline ordering presents in request order when they are encoded on different queues: frame N waits for N-1 and signals N" (`src/dxmt/dxmt_presenter.hpp:160-163`); terminal release via `cancelPresentOrder()` = signal `UINT64_MAX` (`:167-170`) and `skipQueuedPresent(frame_id)` (`:234-237`); restart credits are returned only for frames still owned by a queue worker (`restartableQueuedCredits` / `restartPresentQueue`, `:171-196`).
- `[OBSERVED]` Admission cap: `DXMTPresentLatency::reserve` limits outstanding presents to `maximum_ + replaceable_queued` (`d3d12_swapchain.cpp:68-89`), set to 3 for non-waitable swapchains (`:392-393`) and by `SetMaximumFrameLatency` (`:1556-1560`).
- `[OBSERVED]` `scale_factor` is initialized to 1.0 and never reassigned (`d3d12_swapchain.cpp:306`), so drawable geometry is client-size (`:1111`, `:1380-1381`); the presenter picks `present_blit_` when the source is the full frame with no rotation/scaling/restrict-output (`src/dxmt/dxmt_presenter.cpp:315-318`), and compiles both present pipelines lazily once (`:372-375`).

### 1.4 Shader compilation and lowering (`src/airconv`, PSO flags)

- `[OBSERVED]` `SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO` is forced at every graphics compile site (`d3d12_pipeline_graphics.cpp:825, 954, 1662, 2062, 2203, 2265, 2352, 2461`) and both compute sites (`d3d12_pipeline_compute.cpp:99, 203`). Lowering applies `select(isnan(x), 0, x)` (`nt/air_builder.hpp:626-629`) at the DXBC sample sites (`nt/air_builder.cpp:1530, 1634, 1734, 1754-1756, 2102, 2190, 2408`) and unconditionally in the address-mode path (`nt/air_builder.cpp:801-802`).
- `[OBSERVED]` Asymmetry: the DXIL resource path builds `llvm::air::AIRBuilderOptions{}` with defaults (`dxil_resources.cpp:344-345`), so DXIL sampling does not carry the DXBC flag; the flag reaches lowering only through `SM50_SHADER_COMMON_DATA`.
- `[OBSERVED]` Every sample site emits both a pixel-coordinate and a normalized arm: `AIRBuilder::CreateCoordinateSample` (`nt/air_builder.cpp:1411-1481`) builds `sample.pixel`, `sample.pixel.view`, `sample.pixel.access`, `sample.pixel.empty`, `sample.normalized` and two 4-input PHIs, branching on metadata bit 47. Its condition operand is a **loaded** value in both sampler paths: static samplers load the metadata qword from the static-sampler table (`dxbc_binding_rootsig.cpp:259-272` via `GetSamplerDescriptor`, `:596-648`), table samplers load it from the heap descriptor (`dxbc_binding_rootsig.cpp:277-285`, `dxbc_binding_sm50.cpp:42-44`).
- `[OBSERVED]` Two independent null-descriptor guard layers: the AIR-level pass inserts a guard per declared `air.*` call whose first argument is a texture pointer, for modules carrying `dxmt.d3d12.root-signature` (`airconv_context.cpp:78-115`, flag set at `dxbc_binding_rootsig.cpp:1225-1226`), and the DXIL lowering emits its own guard per access (`dxil_resources.cpp:4239-4240`).
- `[OBSERVED]` Raw/structured buffer accesses are scalarized per component, each behind its own bound check: DXBC `CreateGEPInt32WithBoundCheck` = `icmp ult index, trunc(Metadata)>>2` + `select ptr, null` per component (`nt/dxbc_converter_base.cpp:2751-2767, 2799-2815, 2855-2894`, helper at `:3555-3578`); DXIL `loadBufferWords` calls `emitGuardedAccess` per lane, which emits `SplitBlockAndInsertIfThen` + one PHI per component (`dxil_resources.cpp:864-905`, helper at `:803-812`).
- `[OBSERVED]` Native pipeline creation is device-global: `native_creation_mutex` (`d3d12_pipeline_cache.cpp:675`) is held across `compiler.newComputePipelineState/newRenderPipelineState` (`:1118-1121`). Base MSAA variants are prebuilt at PSO creation (`d3d12_pipeline_graphics.cpp:2018`), while statistics/coarse variants stay lazy behind `msaa_pso_mutex` (`:639-642`, `:2422-2430`).
- `[OBSERVED]` Cache persistence drops silently at a fixed queue depth of 8: `d3d12_pipeline_cache.cpp:129-132` (`kPublicationQueueLimit = 8`) and `d3d12_shader_cache.cpp:871-879` (`limit = 8`, `return` when full). Durable writes use `FlushFileBuffers` + `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` (`d3d12_shader_cache.cpp:666, 669, 808, 813`) with 64 MiB per-entry and 256 MiB per-directory caps (`:328-329`).

### 1.5 Indirect execution

- `[OBSERVED]` `simple_indirect` is the only condition that can avoid the GPU resolver: no count buffer, no root/vertex/index updates, no draw-argument offset, no predication, no statistics, no stream-output query (`d3d12_command_list.cpp:9025-9027`).
- `[OBSERVED]` Caps on the direct native path: compute `simple_indirect && MaxCommandCount == 1` (`:9041`); graphics `simple_indirect && MaxCommandCount <= 64` (`:9173-9176`, carrying a `ponytail:` comment that larger batches use the GPU ICB resolver). Everything else records an ICB + a resolver dispatch (`d3d12_command_allocator.cpp:349` compute, `:465` render) and executes it through the native bridge (`src/winemetal/unix/winemetal_metal4.c:1187` compute, `:1550-1551` render).
- `[OBSERVED]` Resolver kernels are Metal source compiled once per command signature: root-argument resolver (`d3d12_command_signature.cpp:156-227`), graphics/compute command resolver (`:396-399`, `:632-635`), PSOs at `:661-684`.
- `[OBSERVED]` Capture/stream-output/MSAA indirect creates a **fresh ICB per execution** (`d3d12_command_queue.cpp:2929`) plus per-slot scratch/position buffers inside the command loop (`:2931`, `:2943` region) - this is the queue lane's file, listed here only so the synthesis lane does not double-book it.
- `[OBSERVED]` MSAA/capture indirect is reachable from `RequiresMSAAExecution()` (`d3d12_command_list.cpp:6110-6113`: PSO requirement, non-zero coarse shading rate, or `TRIANGLEFAN`) and from stream-output/statistics bindings (`:9155-9157`).

## 2. Retained REA and D3DMetal facts

- `[REA]` Present: `D3DMCommandQueueWorker::DoPresent` at `0x1556a4`, body `0x1556a4-0x15612e` (2699 bytes, contiguous), signature `undefined __thiscall DoPresent(D3DMCommandQueueWorker * this, PresentCommand * param_1)`, Ghidra 12.1.4, from a local analysis snapshot (`doc/mass-ulw/01a135f0-ec60-7040-ad16-8458a34bf704/rea-present.json`). Recorded limitations: pseudocode only, two terminal calls (`0x156073` to `std::__throw_bad_function_call`, `0x156115` to `_abort`) are Ghidra FunctionManager flags, and the analysis was not re-run for that receipt.
- `[REA]` Present contract evidence: `build/local/d3dmetal-opt-port-20261009/present-contract/rea-evidence.json` (96.6 KB) with `identity.json` in the same directory.
- `[REA]` Carried from the 2026-10-14 wave (`doc/mass-ulw/01a135f0-ec60-7040-ad16-8458a34bf704/findings.ko.md`, "D3DMetal REA comparison"): stage cache `D3DMStageCache::GetCachedResult` `0x995de`; graphics stage compile `D3DMPipelineStagesCache::CompileGraphicsPipelineStages` `0x89188`; compute compile guarded by an atomic attempted flag `D3DMComputePipelineState::CompilePipeline` `0x10f062`; encoder IMP caches `RenderCommandEncoderIMPCache::UpdateIMPCache` `0x5826a`, `ComputeCommandEncoderIMPCache::UpdateIMPCache` `0x5810c`; barrier dependency analysis `MPLCommandList::CheckBarrierDependencies` `0x1f31a`, `EmitBarrierDependencies` `0x2085c`, replay `0x36263`/`0x3632c`; clear PSO cache `D3DMCommandEncoder::GetClearPSO` `0x17a03a`; queue worker `DoExecute` `0x154e40`, `DoPresent` `0x1556a4` (blit/triangle present path, fences when needed, then commit).
- `[MEASURED]` Indirect cost, `build/local/d3dmetal-opt-port-20261009/indirect-cost-probe/phase3c-comparison-report.md`: 7 warm closed-list replays per case, 10 MHz QPC, one process per runtime, 54 exact output/guard/fence-checked executions. 4097 slots: no-count median 2399.6 us, counted median 2386.0 us (paired ratio 1.0057); 32 slots: 303.8 / 305.4 us; count 1: 227.7 / 295.7 us. The report states explicitly that these are CPU/wall-clock completion latencies including equal GPU producer/consumer work, "**not FPS, native GPU duration or a gameplay-speedup claim**", that command tracing was OFF so native path counts are UNOBSERVED, and that the 3c/3 differences cannot be attributed to one branch.
- `[MEASURED]` The same probe documents the policy it measured against a source cut whose hash is `ebef9acdc9575854fb35762f87f6a50350eed7c3a3fc693594e584a7f77603ac`, with the direct simple no-count compute branch restricted to `MaxCommandCount == 1`. The current tree's cap sits at `d3d12_command_list.cpp:9041`, i.e. the cut moved; do not treat the retained numbers as current-source measurements.
- `[MEASURED]` FG normalization proof, `build/local/d3dmetal-opt-port-20261009/fg-normalization-proof/report.json`: production `src/winemetal/unix/metalfx_frame_generation.mm` compiled with Metal API + GPU validation enabled, 0 validation errors, 6 frames per mode; mode 0 identity = 0 depth/0 motion dispatches + 12 private snapshot copies, mode 1 low-res scaled motion = 6 depth + 6 motion dispatches, 3 frames passed the strict genuine-interpolation gate (changed pixels 2046/3026/2559), 12 comparisons bit-exact (max abs 0). `limitations` include "No timing/performance benefit measurement is claimed".
- `[MEASURED]` Temporal async contract, `build/local/d3dmetal-opt-port-20261009/temporal-async-contract/all-primary-summary.json`: 8 native arms, adjudications `02-YES-after-encode`, `03-NO-before-encode`, `04-YES-before-encode`, `05-NO-after-completion`, `06-YES-after-completion`, `07-NO-before-completion`, `08-YES-before-completion`, 72 exact RR events, 576 legacy uploads, `GPU_rerun: false`; `limits` include "RR remains syncYES" and "No production producer/conversion/split/cache activation path was exercised".
- `[MEASURED]` `build/local/d3dmetal-opt-port-20261009/metalfx-optimization-proof.json` (normalized receipt, not a raw log): SR 6 frames / 4 public frames, 278784 components per comparison, history max 0.00146484375, outside pixels 4160 per frame, primary copies public 2 / forced staging 3 (115200 / 230400 bytes), generation cache sequence `A,B,C,A` with 3 creations against capacity 8; FG 6 frames, changed pixels on frame 2 = 1680, `all_three_parity_max: 0`.
- `[MEASURED]` Other retained receipts in the same evidence root that a temporal/shader verification can reuse: `gpu-foundation-temporal/console.log`, `temporal-integration-verification.json`, `phase3e-rr-review.json`, `phase3e-integration-acceptance.json`, `native-binding-proof.json`, `view-pool-contract.log`, `rr-async-proof.json`, `phase3c-adjudication/final-adjudication.json`, `phase3e-rr/probe-off/` and `probe-on/`.

## 3. Present/drawable/temporal overlap constraints

Any redesign of this area must keep all of the following; each is enforced in code today.

| # | Constraint | Owner / evidence |
| --- | --- | --- |
| 1 | Presents on different queues keep request order on the GPU timeline: frame N waits for N-1 and signals N through one shared event carrying `frame_id`. | `dxmt_presenter.hpp:160-163`; wait `d3d12_command_queue.cpp:8245`; signal `:8330-8331` |
| 2 | A queue may abandon an accepted present terminally; waiters must be released, not left blocking. | `dxmt_presenter.hpp:167-170` (`UINT64_MAX`), `:234-237` |
| 3 | The drawable reaches Core Animation only after its command buffer completes, and only if the restart epoch is unchanged; a discarded frame withdraws its observation instead of presenting. | `d3d12_command_queue.cpp:8340-8341` comment, `:965-983` |
| 4 | Admission is bounded (default 3 outstanding for non-waitable swapchains); a restart returns credits only for frames still owned by a queue worker. | `d3d12_swapchain.cpp:68-89, 392-393, 1556-1560`; `dxmt_presenter.hpp:171-196` |
| 5 | Flip-sequential composition copies previous/dirty rects before the present and mirrors the composed frame back into the backbuffer. | `d3d12_command_queue.cpp:8270-8332` |
| 6 | A temporal feature's evaluations are ordered by its own timeline: each encode waits on the previous sequence of the same feature and signals after recording, even when the encode failed ("no stranded timeline"). | `d3d12_temporal.cpp:230-243` |
| 7 | History reset is forced whenever the scaler generation serial changes, not only on the caller's `ResetHistory`; the comparison spans features because it lives on `Core`. | `d3d12_temporal.cpp:233` with `Metal4FX.hpp:61`, `winemetal_metal4_fx.mm:102` |
| 8 | Lock order is device submission lock, then feature mutex; the device-level temporal submission mutex is acquired by a queue worker only after it reserved a submission-ring slot. | `d3d12_command_queue.cpp:5051-5052, 7495-7497`; `d3d12_device.hpp:972-973` |
| 9 | The temporal encoder gets its own submission segment (scope reset + `start_segment`), and the standard-swizzle pass brackets it. | `d3d12_command_queue.cpp:7460-7500` |
| 10 | In-flight frames keep their feature, scratch and residency alive through GPU completion; `markDormant` drains pools but never touches active leases, and CPU publication is not GPU completion. | `metalfx-backend.hpp:88-91`; `metalfx-backend.mm:689-690, 626-641` |

## 4. Shader/PSO/indirect bottleneck candidates

| ID | Candidate | Trigger (observed) | Class | Confidence | Counterevidence / falsifier |
| --- | --- | --- | --- | --- | --- |
| T1 | Native temporal feature is destroyed and rebuilt on every output-extent change | `DynamicOutputEnabled` + extent change (`d3d12_temporal.cpp:163-178`), set by FSR3 (`fsr.cpp:284`) | Native object creation + full history reset, per transition | high on code; activation `[INFERRED]` | No retained capture shows a title oscillating `upscaleSize`. Falsifier: count feature creations vs extent transitions in a trace. |
| S1 | Per-sample NaN clamp forced on every DXBC pipeline | 8 graphics + 2 compute sites | GPU ALU, steady | high on code; magnitude `[INFERRED]` | Deliberate D3D conformance flag; `getNaNToZero` is one select. Not removable without a conformance decision - **not a candidate for this wave**. |
| S2 | Every sample site carries both coordinate arms (4 extra blocks + 2 PHIs) | `air_builder.cpp:1411-1481` | GPU code size / branch per sample, steady | high on code; low-medium on FPS | **Correction:** the condition is a loaded value even for static samplers, so compile-time specialization is not available (see 8.1). Leave alone. |
| S3 | Raw/structured buffer reads scalarized per component behind per-component bound checks | DXBC `:2751-2767, 2855-2894`; DXIL `dxil_resources.cpp:864-905` | GPU issue slots / branches, steady | high on code; magnitude `[INFERRED]` | A vector fast path must prove the whole vector in bounds and keep a scalar fallback for partial OOB; a wrong fold changes D3D results. Falsifier: ISA diff on one fixed CS. |
| S4 | Two independent null-descriptor guard layers | `airconv_context.cpp:81-115` + `dxil_resources.cpp:4239-4240` | GPU control flow / code size | high on code; low on magnitude | The AIR guard is the correctness boundary for query/sample on a null texture; the DXIL guard has its own reasons. Simplifying requires proving one dominates. |
| S5 | Compute kernels get a simdgroup barrier after each qualifying threadgroup store | `transforms/simdgroup_implicit_membarrier.cpp` (pass run last in the cleanup pipeline, `airconv_context.cpp:236`) | GPU compute serialization | high on code; activation unknown | Hoisting is correctness-sensitive; the pass deliberately suppresses only duplicates on the defining store. |
| S6 | Lazy native variant builds under a device-wide creation mutex | `d3d12_pipeline_cache.cpp:675, 1118-1121`; `d3d12_pipeline_graphics.cpp:639-642, 2422-2430` | CPU hitch at first use | high on code; magnitude `[INFERRED]` | Base MSAA variants are already eager (`:2018`); only statistics/coarse/write-mask/mesh-key permutations remain. Trace `pipeline_artifact_operation` to falsify. |
| S7 | Cache persistence silently drops beyond queue depth 8 | `d3d12_pipeline_cache.cpp:129-132`; `d3d12_shader_cache.cpp:871-879` | Startup on the *next* launch | high on code | Overlaps the active design `doc/SHADER-CACHE-REGRESSION-DESIGN-2026-10-13.ko.md`, which explicitly says not to re-implement existing stage/native reuse. Do not take it in this wave. |
| I1 | Indirect beyond the simple-path caps costs ~2.4 ms per 4097-slot call | caps at `d3d12_command_list.cpp:9041, 9173` | CPU+GPU mixed, per call | `[MEASURED]` timing only | The retained report forbids attributing the difference to one component and calls a threshold change "unmeasured tuning". Reject as an increment. |
| I2 | Capture/SO/MSAA indirect creates a fresh ICB plus per-slot buffers per execution | `d3d12_command_queue.cpp:2929, 2931, 2943` | CPU native resource creation, per execution | high on code | Lives in the queue lane's file - out of this lane. |
| P1 | Present order-event gate + Reflex completion gate on the game thread | `d3d12_command_queue.cpp:8245`, `src/d3d12/d3d12_reflex.hpp` | CPU pacing | high on code | Lives in the queue lane's file - out of this lane; the synthesis lane owns the decision. |

## 5. One recommended next increment (non-overlapping with the queue lane)

**Retain native temporal features by output extent in `src/d3d12/d3d12_temporal.cpp` (this file only).**

- What changes: `Core` keeps a small, bounded cache of `{output_width, output_height, WMT::Reference<WMT4::TemporalFeature>}` in addition to the active feature. On a frame whose extent differs from the active feature, first look for a cached feature with that exact extent; reuse it, and only `markDormant()` + evict when a new feature must be created and the cache is full. The active-feature pointer, `feature_output_width/height`, the prepare/encode paths, the timeline protocol and the reset rule stay as they are.
- Why this one: the extent-change path today performs a full native feature creation (MetalFX temporal scaler creation, plus an alpha scaler when the alpha flag is set) and drops the previous feature's 8-slot scaler-generation cache and all pooled scratch/residencies (`metalfx-backend.mm:423, 647-653, 1112-1262`). Reuse keeps both the scaler generation and the pools. It is the only avoidable native-object creation this lane found that is reachable from a documented code path and confined to one non-queue file.
- Invariant that makes reuse safe: returning to an older feature must still reset history. That is already guaranteed, because the reset condition compares the *scaler generation serial* against `Core::last_executed_generation` (`d3d12_temporal.cpp:233`), and a non-consecutive return necessarily carries a different serial than the last executed one. Do not weaken or remove that comparison.
- Bounded by construction: no ABI change, no new native call, no change to `WMT4::TemporalFeature` or the FG path. When the extent never changes the change is inert except for one retained feature.
- Explicit non-goals: do not change the reset policy, the timeline ordering, the present order event, the submission segment structure, or any file under `src/winemetal/`.
- `[INFERRED]` The payoff is conditional: with no retained evidence that a title oscillates `upscaleSize`, the expected saving is one scaler creation + history reset per extent transition, not a steady per-frame win.

## 6. Invariants a change here must not break

1. Present order across queues and its terminal release paths (constraints 1-2).
2. Drawable hand-off only after command-buffer completion and only for the current restart epoch (constraint 3).
3. Admission bounds and restart credit accounting (constraint 4).
4. Flip-sequential composition copy/mirror semantics (constraint 5).
5. Per-feature timeline ordering with a signal even after a failed encode (constraint 6).
6. History reset on any non-consecutive scaler generation (constraint 7).
7. Device lock before feature lock; temporal submission mutex only after a ring slot is reserved (constraint 8).
8. The temporal encoder's own submission segment and its swizzle bracket (constraint 9).
9. Frames and leases retain their feature and scratch until GPU completion; `markDormant` never touches an active lease (constraint 10).
10. D3D12 semantics preserved in lowering: out-of-bounds reads stay zero, null descriptors stay zero-filled, non-normalized-coordinate samplers keep their pixel arm.

## 7. Risks

- No retained evidence proves any title changes its temporal output extent at runtime; the increment may be inert. The cost when inert is one retained native feature (its idle pools and up to 8 scaler generations).
- Reusing a feature reuses its scratch capacity; a retained feature is real device memory, so the cache must stay at 1-2 entries and must not become a general pool.
- Any change that also removed the `last_executed_generation` comparison would silently keep stale history across an extent change - the highest-severity failure mode here.
- The present gate, Reflex sleep and the per-execution ICB creation all live in `src/d3d12/d3d12_command_queue.cpp`; a synthesis increment there and this change touch disjoint files, but they share the temporal submission mutex and the order event at runtime, so they must not be verified only in isolation.
- Everything in section 4 except I1's timing is code-shape evidence; no FPS claim is made anywhere in this handoff.

## 8. Exact checks

### 8.1 Corrections verified this session (do not repeat these mistakes)

- The earlier remedy "specialize the coordinate diamond for statically known samplers" is **not viable**: static-sampler metadata is a loaded table qword (`dxbc_binding_rootsig.cpp:259-272` reaching `GetSamplerDescriptor` at `:596-648`), so metadata bit 47 is never a compile-time constant in either sampler path.
- The temporal `generation` field is **not** dead code: it is an out-parameter of the native prepare (`Metal4FX.hpp:54-62`) filled from `PreparedFrame::generationId()` (`winemetal_metal4_fx.mm:102`, `metalfx-backend.mm:2020-2022`) and consumed at `d3d12_temporal.cpp:233`. Do not "clean it up".
- Line numbers moved since the 2026-10-14 wave and since the phase3c cut: present order wait 8204 -> 8245, drawable wait 8334 -> 8329, `ReserveMetal4Present` 8305 -> 8346, compute indirect cap 8980 -> 9041, graphics cap at 9173. Cite the current numbers.

### 8.2 Checks to run for the recommended increment

1. `clangd` diagnostics on `src/d3d12/d3d12_temporal.cpp` using the configured build directory (`build/local/build` per `.clangd`).
2. `git diff --check` (root formatting disables include sorting; D3D12 uses 2 spaces, 120 columns).
3. The repository build recipe from `README.md` (no new files, no Meson option change).
4. Focused runtime check with the same shape as the retained native temporal probes (`temporal-async-contract`, `fg-normalization-proof`): drive `Prepare` with extents A, B, A on one temporal state and assert
   - the feature-creation count does not grow per transition (trace `temporal_record` / `temporal_frame_destroy` / feature ids),
   - the third frame's `temporal_encode` line carries `"reset":1` while the second frame's carries `"reset":0` (unless the caller set `ResetHistory`),
   - the third frame's output is bit-exact against a reference run of the same sequence.
5. Preserve the run's evidence under a fresh `build/local/<date>-<name>/` directory, including the raw logs, `result.json` and any drawable capture; retire the disposable Wine prefix once its processes stop.
6. Explicitly re-run the present/temporal overlap assertions that exist today (the 8 arms in `temporal-async-contract/all-primary-summary.json`) rather than assuming they still hold.

### 8.3 Checks that would falsify a section-4 candidate before anyone implements it

- S3/S4/S5: dump AIR for one fixed PS and one threadgroup-heavy CS with and without the pass and compare basic-block/branch/instruction counts; if the Metal backend already folds the difference, drop the candidate.
- S6: `OWNED_GRAPHICS_TRACE=1` and check for `pipeline_artifact_operation` (`kind=native_render`/`native_mesh`) between the first `Draw` and frame end during gameplay.
- S7: count `pipeline_cache_auto_begin` vs `pipeline_cache_auto_publish` per key across a level load; a gap proves a drop.
- I1: count `ExecuteIndirect` calls and `MaxCommandCount` per frame; if every call is simple or <= 64 commands, the cap is not the bottleneck.
- T1: count feature creations against `upscaleSize` transitions in one bounded trace.

## 9. Limits of this handoff

- Static source reading plus retained receipts only: no build, no Wine run, no GPU capture, no FPS measurement in this session.
- Not audited here (other lanes or out of scope): `msaa_*` lowering bodies, `dxil_rt_*`, work graphs, DXR acceleration-structure helpers, the D3D11 shared path, and the queue lane's scheduler internals.
- The D3DMetal reference is x86_64 shipped pseudocode; it supports contract questions, not performance deltas.
