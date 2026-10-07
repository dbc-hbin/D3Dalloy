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

Documentation migration is in progress. Production fixes have not yet been applied at creation of this ledger. Record each actual implementation, test command/result, source/runtime identity and remaining acceptance here; do not mark pending GPU or performance work complete from a compilation alone.

## Primary contracts

- [D3D12 fence synchronization and rewind](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)
- [Multiple-fence completion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-seteventonmultiplefencecompletion)
- [Advanced Color composition](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)
- [Metal 4 command queue](https://developer.apple.com/documentation/metal/mtl4commandqueue)
- [Metal 4 compiler archive lookup](https://developer.apple.com/documentation/metal/mtl4compilertaskoptions/lookuparchives)

Consult the installed public SDK and a hash-verified pristine reference when native semantics remain ambiguous, as required by AGENTS. Reference behavior is compatibility evidence, never a production dependency or a replacement for the public contract.
