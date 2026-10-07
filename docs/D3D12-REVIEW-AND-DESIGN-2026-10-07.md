# D3D12 translation review, design and implementation ledger

Review baseline: `main@4a3731fe92c08c61451cd69927c323b51d26eabd`, plus the existing Wine/FEX/frontend working-tree changes inspected on 2026-10-07. This document consolidates the preceding source review and records the follow-up implementation separately. Source-backed findings are not reproduced GPU failures; retained results are not new executions.

## Scope and current disposition

The user selected documentation cleanup, preservation of historical handoffs, component-scoped atomic local commits, and implementation of the reviewed correctness fixes and justified optimizations. No upstream PR, push, live-game installation, broad D3D11 expansion or proprietary backend dependency is part of this work.

| ID | Finding or design | Initial disposition | Required acceptance |
| --- | --- | --- | --- |
| R1 | Metal4FX CPU pointers omit the native x86 guest-window base | Confirmed static defect; implementation pending | Actual wrapper argument checks with zero/nonzero base; affected native x86 consumer, not only no-present smoke |
| R2 | Event waits retain failed registrations and losing ANY subscriptions | Confirmed static lifetime defect; implementation pending | Persistent done+never ANY loop, setup failure rollback, immediate/concurrent completion, no duplicate event |
| R3 | Fence completion repeatedly erases a vector and invokes callbacks under its mutex | Confirmed quadratic algorithm and locking hazard; implementation pending | Linear completion structure; reentrant callback/cancellation, rewind/removal and concurrent one-shot tests |
| R4 | Automatic PSO cache metadata mutex covers disk read/validation/write/flush | Confirmed contention path; implementation pending | Memory hits independent of blocked disk I/O; clear/disable/retry generation races; explicit blob errors unchanged |
| R5 | Satisfied queue Wait commits an otherwise empty Metal buffer | Optimization candidate; native visibility/ordering validation required | Cross-queue data and FIFO, retirement/removal/shutdown, trace distinguishes logical wait from GPU submit |
| R6 | Application ShaderCacheSession scans and hashes the whole file on lookup and before append | Confirmed cost structure; workload-dependent optimization | Actual API usage and byte/time measurements before introducing an index or separate cache subsystem |
| R7 | Warm PSO restore still creates native library/function/pipeline objects | Confirmed call path, not proof of repeated native machine-code compilation | Separate phase timing and duplicate-key counts before native-object/archive caching |

The final implementation/evidence section below is authoritative for the status of this follow-up. The initial findings remain dated observations even after correction.

## Architecture and invariants to retain

The D3D12 frontend records typed passes and commands. Queue-local FIFO tasks retain the submitted recording and feed the submission worker; validated GPU completion retires owners through `SubmissionRing<32>`. DXIL/DXBC conversion produces owned AIR/metallib with invocation-local mutable LLVM state. The WineMetal boundary carries fixed-width POD, explicit CPU addresses, native object handles and GPU addresses; those are different types, not interchangeable integers.

Logical D3D12 fence values can rewind. A registered wait latches a qualifying completed value even when a later signal lowers the current value. A new wait must not inherit an old crossing. Queue signal reservation, native commit and validated completion are separate events. Preserve bounded submission backpressure, device-removal wakeups, and allocator/GPU lifetime. Do not substitute global waits, repeated history reset or capability reduction for correctness.

## R1 — typed CPU pointer encoding

At the review baseline, `src/winemetal/Metal4FX.hpp::PresentMetal4Drawable`, `TemporalFeature::Create` and `TemporalFeature::prepare` cast caller structures to `uintptr_t` without adding `WMTGetGuestWindowBase()`. Adjacent `Metal4Pipeline.hpp` and `Metal4.hpp` wrappers do encode this base. `WMTMetal4Call` dispatches Unix slot 314 without interpreting nested arguments; the same native handler serves WoW64. `unix/winemetal_metal4_fx.mm` directly dereferences the create/prepare information and passes the composition pointer to `winemetal_composition_present`, which dereferences `frame->surface`.

Trigger: a native FEX x86 caller with a nonzero guest-window base enters temporal create/prepare, or presents a nonnull Composition frame. A zero-base x64/native caller and the null-frame ordinary HWND route are not this trigger. The SDK05 eight-frame no-present smoke does not exercise these contracts. The later retained FEX x64 NGX/FSR SR success does not establish FEX x86 support.

Design: centralize null-preserving CPU-pointer encoding and use it at these boundaries. Never add the guest base indiscriminately to `args[]`: Metal handles, GPU virtual addresses and callback identities must remain unchanged. Keep POD sizes and Unix call indexes stable. Tests should invoke the real wrapper with a captured call, not merely test an unrelated arithmetic copy. Cover null, 32-bit boundary values, a nonzero base, and untouched native handles.

## R2/R3 — cancellation-safe event subscriptions and linear completion

`src/dxmt/dxmt_fence.hpp::FenceWaitValue` initially stores only a latch. `Fence::prepareWaitValue` registers an epoch callback without obtaining its waiter ID. In contrast, queue tickets retain the ID and can cancel. `SetEventOnMultipleFenceCompletion` prepares all waits before creating its internal event and listener. Listener failure marks a registration cancelled but does not unsubscribe existing epoch waiters. A successful ANY registration sets its fired flag but leaves every losing waiter installed.

Legal stress: keep `Fdone=1` and `Fnever=0` alive, repeatedly register `ANY({Fdone,Fnever},{1,1})`, and consume each event. Each call completes, yet the original implementation adds another waiter to Fnever. Existing tests eventually signal both fences or destroy the device, masking accumulation. The finding is retained storage and later fanout, not a claim that `SetEvent` fires twice.

`src/dxmt/dxmt_queue_order.hpp::completeValueLocked` erases qualifying vector entries individually and invokes callbacks before the epoch mutex is released. An all-ready batch therefore has quadratic element movement. Current latch callbacks do not establish a present self-deadlock, but a naive fix that cancels ANY losers from `reach()` would reenter the same epoch mutex.

Design: detach ready callbacks under the epoch lock in linear time without allocation on completion, then invoke them after unlocking. Registration must remain atomic with initial observation and rewind. Add move-only cancellation tokens with weak epoch ownership; failure paths roll back every prepared subscription, successful asynchronous registrations transfer ownership, and ANY completion cancels losers. Avoid strong ownership cycles between registration, latch and epoch. Handle synchronous completion during setup/arming and races between cancellation and detached callbacks. User callbacks/events must never be invoked while holding the registry mutex.

Acceptance: repeated done+never ANY returns pending subscriptions to baseline; injected setup failures leave none. Test callback reentrancy, cancellation of a different and the same epoch, already-completed values, rewind after crossing, late registration after rewind, terminal removal, and concurrent exactly-once completion. Sanitizer success is not a Windows COM or GPU conformance claim.

## R4 — automatic cache metadata and disk transactions

`src/d3d12/d3d12_pipeline_cache.cpp::PipelineCache::Initialize` holds the module-local metadata mutex during disk read and blob validation. `Complete` holds it while `WriteAutomaticPipelineCache` acquires a named mutex, scans/evicts entries, writes, flushes and renames; only afterward are joined requests notified. The named disk lock has a 1000ms timeout, but no one-second stall was measured by the review. An unrelated memory hit can nevertheless wait behind this path.

Design: retain existing owned blobs, identity checks, bounded RAM/disk policy and same-key single-flight. Separate metadata locking from disk work. Publish successful in-memory flight results without making unrelated requests wait for persistence. Serialize disk mutation and clear separately, checking generation/owner identity before delayed publication or invalidation. A delayed old-generation writer must not resurrect a cleared cache or erase a newer flight. Old waiters may still consume their producer's valid result. Explicit application CachedPSO errors remain strict; optional automatic-cache recovery remains bounded and rebuilds a fresh PSO.

Avoid an unbounded writer queue or another caching subsystem merely to move the problem. Add opt-in phase timing for metadata-lock wait, disk read/write and native construction. Validate clear/disable/re-enable against delayed read, write and cold retry, including successful callers joined before the control operation.

## R5–R7 — measured follow-up, not assumed FPS gains

`d3d12_command_queue.cpp::ProcessWait` already waits for actual validated completion, then initially creates a `CommittingScope` only to trace; the scope destructor commits and publishes a new native buffer. Eliminating this submission must preserve the worker gate, FIFO and Metal visibility; it is not permission to replace the gate with a single current-value poll. Update trace diagnostics so a logical wait need not invent a GPU submission ID.

`d3d12_shader_cache.cpp::Cache::locate` scans and hashes all records, even after a match, and `store` calls it before append. Equal-sized sequential insertions have quadratic cumulative scanning. If this API is used materially, validate once at exclusive open, index verified records and retain on-read integrity, collision, budget and failure semantics. The application-session cache and implicit PSO cache remain independent.

`d3d12_pipeline_compute.cpp::Initialize` skips owned compilation on a blob hit but still initializes reflection/constants and calls native library/function/PSO constructors. Measure these separately. A process-local immutable backend-object cache must be device-scoped and keyed by shader/entry/constants/pipeline options, not reuse COM wrapper identity or private-data state. Native pipeline archives are a later layer, not a substitute for the owned compiler or proof of cold/warm performance.

Report cold and warm phase times, unique and duplicate keys, p50/p95/p99 latency, allocation/copy volume and retained memory before claiming a game benefit. Do not relabel synthetic timing as FPS, scanout cadence or input latency.

## Outstanding external/native contracts

Composition: retain geometry, attachment and cadence results while independently establishing alpha association, transfer domain, FP16 quantization/clipping and display-profile expectations. Microsoft's documented Advanced Color linear scRGB composition domain is a qualified starting point, not a legacy-SDR or calibrated screen oracle. Do not choose gamma from the owned output being tested.

Ray Reconstruction: the retained investigation identifies a K44/C16 unmasked tail load over a 12672-byte MetalFX weight buffer. Standalone specialized AIR and the arithmetic witness do not establish byte identity of the live framework pipeline or hardware transactions. Isolate pre-load masking and compiler behavior in a minimal native experiment; preserve the failing public/native validation evidence. Fixing R1 is not fixing this independent native kernel contract. No guide removal, padding hook, SR substitution or validation suppression constitutes completion.

HDR10+ GAMING: separate metadata parsing, calibrated APP/GPU tone mapping, native Gaming/low-latency/source-TM control, payload transport, output ownership and restoration. Generic InfoFrame transport, ALLM, SBTM or ordinary HDR10+ video are not Gaming SSTM. A mandatory sink ACK was not established and must not be invented as a prerequisite. No unused parser/getter echo should be presented as a working display feature.

The detailed original observations are preserved in [historical records](history/README.md). These limitations do not justify rewriting the renderer or reopening already-implemented API families.

## Retained evidence before follow-up implementation

The preceding review ran `submission-ring`, `queue-order-scheduler` and `shared-fence-timeline` CPU probes under ASan/UBSan. Logs are in `build/local/review-cpu-20261006-i_zjh9g5/`; the latter two initially failed to compile because the review command omitted `-Isrc/dxmt`, then passed after correcting the command. The cost benchmark source existed but its execution was blocked; no measured fence cost was reported.

On 2026-10-07 the documentation pass inspected, but did not rerun, `build/local/fex-selected-validation-20261006.json`, `login03-mono-install-20261007/result.json`, `login03-gpu-{x64,x86}-20261007/result.json` headers, and `arm64x-gpu-features03-20261007/{feature-results,profile-results}.json`. The latter public NGX/NGX-contracts/FSR SR results use direct isolated FEX x64 execution, not the generic runner or a proven whole-process no-Rosetta migration matrix. Preserve this scope.

## Follow-up implementation and evidence

The 2026-10-07 follow-up implemented R1-R6 in priority order and added R7 measurement. Local commits are `f3d3dfa` (R1), `cf0a14e` (R2/R3), `23a8a42` (R4), `f06a9bc` (R5), `083b601` (R6), and `0556ac1` (R7 instrumentation). No native-object cache or native pipeline archive was added: the measurements below do not yet justify their additional device/key/lifetime state.

Eight unused external worktrees were removed first. Three dirty trees were preserved as `refs/archive/worktree-cleanup-20261007/{4e4375d1-main,ca44362e-main,9c6727c0-main}`; ignored build evidence was moved rather than deleted. One worktree with an interactive shell was retained. The [cleanup receipt](../build/local/worktree-cleanup-20261007/result.json) records paths and archive identities. The 30 root `fg-*.bin` captures were not staged or deleted.

### Implemented contracts and focused acceptance

- **R1:** Null-preserving `Metal4CPUPointer` now encodes only the three CPU structure pointers. The real-wrapper ASan/UBSan probe covers zero/nonzero bases, null and 32-bit boundary values while preserving native handles. The [native FEX x86](../build/local/review-implementation-20261007/r1/native-x86-02/result.json) and [FEX x64](../build/local/review-implementation-20261007/r1/native-x64-02/result.json) consumers each pass five cases with verified PE/native module identities, API validation and no Rosetta. The observed x86 base is `0x600100000000`, x64 base zero. Create/prepare and null/non-null Composition descriptor consumption are covered; there is no drawable in this probe, so it is not a rendered Composition, SR/FG or RR acceptance. The first x86 evidence-admission failure is retained separately.
- **R2/R3:** Registration-time list nodes let completion splice a ready batch without allocation and dispatch/destroy callbacks after releasing the epoch lock. Move-only weak-epoch tokens roll back preparation, transfer into one-shot registrations and cancel ANY losers without latch cycles. The new `fence-event-cancellation.test.cpp` passes strict warnings and ASan/UBSan: 4,096 allocation-forbidden completions, 256 persistent done+never ANY registrations, seven setup allocation failures, partial rollback, reentrant callbacks/destructors, rewind/removal, detached cancellation and deterministic concurrent arm/ANY/ALL races. Existing queue-order/shared-fence CPU probes pass. Public `fence-events` and `sharing` pass both ABIs before and after R5. Per-ID cancellation still scans an epoch; this is linear batch completion, not a claim that every arbitrary cancellation fanout is linear.
- **R4:** Automatic cache disk transactions have a separate mutex; metadata does not remain locked during I/O, validation or serialization. Completed in-memory flights are published before optional persistence. Generation plus exact owner/blob checks protect delayed read/write/retry and control operations. `python3 graphics-translator/tests/run-pipeline-cache-cpu.py` passes strict production-code probes for blocked disk phases, unrelated memory hits, joined callers, clear/disable/re-enable, stale reads/writes/retries, explicit errors and the 512-entry bound. The [x64](../build/local/review-implementation-20261007/pipeline-cache-x64-after/result.json) and [x86](../build/local/review-implementation-20261007/pipeline-cache-x86-after/result.json) public cache suites pass API/Shader Validation with exact outputs and no foreign backend.
- **R5:** `ProcessWait` retains the validated-completion gate, cancellation and FIFO, but does not reserve/commit an empty native buffer. The same 32-frame multiqueue workload has 64 executions, 64 signals and 63 logical waits: submissions/completions fall from 191 in the x64 baseline to 128 on both ABIs, with exact cross-queue readback. The runner now enforces these counts and rejects a submission ID on a logical wait; all 96 diagnostics tests pass. Both ABI `matrix-runtime` runs also pass inline CL11 consumer-first wait/signal, rewind, released recording/resource owners and state handoff. Queue shutdown/device removal acceptance is recorded with the final-cut execution-safety results below.
- **R6:** An exclusive open verifies all records once. Normal files receive a per-Cache digest index; lookups still validate the selected header/payload and compare exact keys. Index storage is prepared before append and published only after successful flush. Real SHA1 collisions, duplicate physical records, short buffers, WriteFile/FlushFileBuffers failures and rollback poisoning, corruption, version/clear and oversized existing files pass the extracted production CPU probe on both ABIs. A pre-existing file larger than the reopening budget retains constant-scratch full scans; normal record count is bounded by `floor((disk_budget - 16) / 48)`, with allocator overhead separate from payload limits. The [x64](../build/local/r6-cache-20261007/indexed-x64/result.json) and [x86](../build/local/r6-cache-20261007/indexed-x86/result.json) public API suites pass all existing contracts plus 1,344 exact 1 KiB values each.

### Measured costs and R7 decision

[Raw phase/byte aggregates](../build/local/review-implementation-20261007/cache-measurements.json) retain the full nearest-rank p50/p95/p99 samples and R6 before/after API times. At 1,024 application-cache entries, store/lookup costs are x64 **1,979.214/3,914.409 ms -> 35.326/5.771 ms** and x86 **2,214.408/4,376.718 ms -> 52.190/6.793 ms**. Reopen remains a full validation (x64 3.880 -> 4.566 ms; x86 4.307 -> 5.589 ms). Production CPU probes observe zero record-read bytes before unique appends and 2,150,400 lookup bytes instead of the baseline path's calculated 1,129,316,352 bytes. These are API timers and logical ReadFile bytes, not physical disk traffic or game FPS.

R7 adds opt-in successful compute initialization timing for cache, reflection, owned compilation, constants, native library, function and pipeline, with a creation ID, key and logical byte counts. Both public cache runs have 18 complete seven-phase samples, four unique keys and 14 duplicate creations. Each ABI has four owned-blob misses and 14 hits. On hits, x64/x86 native PSO p50 is **152.1/110.8 us**, p95/p99 **343.0/318.8 us**; library p50 is 13.7/16.3 us and function p50 2.3/3.5 us. Miss native PSO p50 is 22.644/23.293 ms. No global Metal cache was cleared: miss/hit describes the owned blob, not driver cold/warm state. Small sample p95/p99 values coincide with the maximum.

Each ABI reports 62,346 library bytes, 576 constant bytes and 60,108 reflection/statistics copy bytes across these creations. These are logical inputs/copies, not allocations or retained/peak native memory. The workload intentionally repeats keys and is not a real game's duplicate rate; graphics, lazy variants, failure costs, frame times and power were not measured. Native constructor calls alone therefore do not justify a device-scoped object cache or archive. Keep those candidates pending until game-level cost and retention data warrant them.

### Build identities, evidence boundaries and cleanup

The final-cut [x64](../build/local/review-implementation-20261007/execution-safety-x64-final/result.json) and [x86](../build/local/review-implementation-20261007/execution-safety-x86-final/result.json) execution-safety suites pass API/Shader Validation. Unsignalled-Wait shutdown cancels only the destroyed queue, preserves its peer, retires prior work and does not publish post-Wait work. Explicit removal retires the control copy, cancels two pending copies, terminally completes five fences and destroys both queues without executing the pending copies. This is not an artificially forced still-in-flight native GPU race.

All x64/x86 DLL builds pass. A fresh [paired ARM64/ARM64EC build and link audit](../build/local/review-implementation-20261007/arm64x/result.json) passes for the ten selected core/NVAPI/NGX/FSR DLLs; its source cut is frozen beside it. This is build evidence, not a new all-guest native runtime matrix. Only R1's separate native consumer probe establishes the FEX observations above.

The Rosetta/native-x86_64 Wine baseline manifest SHA256 is `167e03f03b76bf1392006e59b4cec2dac321244853f2c649d814efdad8df578a`. The R5/R7 public GPU cut is `788a9d701658a31923e756f3e5398b660b8fcbecfd1de5863f272ff0f784a5e8`; it includes R6's initial index before the oversized-file fallback. The final R6 cut is `5f71795a6b6189a80956a323e3fc39d9d2b549ec739d50109cc003153eb3c6d6`. Results, frozen fixtures, invocation/module hashes and original failures are retained under `build/local/review-implementation-20261007/` and `build/local/r6-cache-20261007/`. Completed disposable prefixes are removed only after scoped wineserver exit. The intermediate runtime's bin/lib/share payload is retired; its manifest/provenance remains alongside the final and comparison runtimes.

Clangd lacks the configured cross-target standard headers; real compilers and runtime checks, not those incomplete diagnostics, establish this cut. Existing compiler warnings remain unsuppressed. The temporal analyzer's pre-existing synthetic test still models consumer encoding before producer completion; its renamed logical-wait field is not proof of current worker ordering. Actual multiqueue/inline/public fence evidence is used instead. Composition absolute colour, the native RR tail load and HDR10+ GAMING remain unresolved contracts, not consequences fixed by these changes.

## Primary contracts

- [D3D12 fence synchronization and rewind](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)
- [Multiple-fence completion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-seteventonmultiplefencecompletion)
- [Advanced Color composition](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)
- [Metal 4 command queue](https://developer.apple.com/documentation/metal/mtl4commandqueue)
- [Metal 4 compiler archive lookup](https://developer.apple.com/documentation/metal/mtl4compilertaskoptions/lookuparchives)

Consult the installed public SDK and a hash-verified pristine reference when native semantics remain ambiguous, as required by AGENTS. Reference behavior is compatibility evidence, never a production dependency or a replacement for the public contract.
