# D3D12 review: implemented fixes and remaining contracts

Updated 2026-10-09. The 2026-10-07 review started from `4a3731fe92c08c61451cd69927c323b51d26eabd` plus the then-pending Wine/FEX changes. **R1-R6 are implemented; R7 added measurement, not a native PSO object cache.** The original static findings are not current implementation tasks.

Current support is in [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md), remaining work in [PRIORITIES](../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md). The October 7 raw review/GPU results were removed during disk cleanup; the observations below are historical, not newly executed or currently retained logs. [HISTORY](../graphics-translator/IMPLEMENTATION-HISTORY.ko.md#증거를-읽는-기준) records that boundary.

## Follow-up implementation and evidence

| Item | Implemented contract | Recorded acceptance and boundary |
| --- | --- | --- |
| R1 `f3d3dfa` | Null-preserving `Metal4CPUPointer` encodes the three guest CPU structure pointers; native handles and GPU addresses remain unchanged. | Real-wrapper sanitizer checks and native FEX x86/x64 consumers, five cases each. Nonzero x86 guest-window base covered; no drawable, so not rendered Composition/SR/FG/RR proof. |
| R2/R3 `cf0a14e` | Completion detaches ready list nodes under the epoch lock, then calls/destroys callbacks after unlocking. Weak-epoch cancellation tokens roll back failures and cancel losing ANY waits. | 4,096 allocation-forbidden completions, 256 persistent done+never registrations, seven setup failures, reentrancy, rewind/removal and deterministic races; public fence/sharing runs on both ABIs. |
| R4 `23a8a42` | Separate metadata and disk locks; publish joined in-memory results before optional persistence. Generation and exact owner/blob checks protect delayed operations and clear/disable/retry. | Production CPU probes cover blocked disk phases, unrelated hits, stale reads/writes/retries, explicit blob errors and the 512-entry bound; public GPU cache runs on both ABIs. |
| R5 `f06a9bc` | Satisfied logical waits retain validated completion, cancellation and FIFO without submitting an empty Metal buffer. | Same 32-frame workload: 64 executes, 64 signals, 63 waits; submissions/completions 191→128 with exact cross-queue readback. Final shutdown/removal checks do not claim a forced in-flight GPU race. |
| R6 `083b601` | Exclusive open validates records once; normal files use a digest index with selected-record integrity and exact-key checks. Publish index entries only after successful flush. | Collision, duplicate, corruption, I/O rollback and lifecycle CPU checks. Oversized pre-existing files retain bounded-memory scans rather than unbounded indexing. |
| R7 `0556ac1` | Opt-in seven-phase compute PSO timing with creation/key identity and logical byte counts. | 18 samples per ABI, four unique keys and 14 duplicate creations. These are synthetic measurements, not a game's duplicate rate or a native-driver cold-cache study. |

The queue/fence invariants remain: registered waits latch qualifying completion despite rewind; late waits do not inherit old crossings; reservation, native commit and validated completion are distinct. Preserve bounded submission backpressure, removal wakeups and allocator/GPU lifetime. Mutable LLVM IR remains local to each compilation.

### Measurements and follow-up

At 1,024 application-cache entries, recorded store/lookup API costs changed from 1,979.214/3,914.409ms to 35.326/5.771ms on x64, and 2,214.408/4,376.718ms to 52.190/6.793ms on x86. Reopen still performs full validation. These are API timers and logical reads, not physical disk traffic or game FPS.

Owned-blob-hit native PSO p50 was 152.1/110.8µs for x64/x86; misses were 22.644/23.293ms. No global Metal cache was cleared, and logical bytes are not retained/peak native memory. Native-object reuse or archives remain candidates only after game-level cost and lifetime evidence justifies them.

The later **`105f82e`** change removes publication-by-publication automatic PSO directory rescans using shared accounting and conditional recovery. Its retained real WinAPI regression and completed ZZZ resource-check/DLSS flow are documented in [ZZZ](../graphics-translator/ZZZ-COMPATIBILITY.ko.md#2026-10-09-실제-precompile-경로-진단과-자동-pso-캐시-재스캔). Invalid shader/root pairs remain rejected. [FEX results](FEX-PERFORMANCE-2026-10-09.md) separately cover emitter work reduction, not whole-game loading speedup.

### Reverification

Use [SMOKE](../graphics-translator/SMOKE-TEST.ko.md) for fresh runtime/ABI/validation commands. Focused CPU checks include `graphics-translator/tests/fence-event-cancellation.test.cpp` and `python3 graphics-translator/tests/run-pipeline-cache-cpu.py`. `python3 scripts/test-owned-graphics-diagnostics.py` checks evidence parsing, not GPU execution. Partial clangd configuration is not a substitute for real cross-builds.

## Outstanding external/native contracts

- **Composition:** geometry, attachment and cadence observations do not settle alpha association, transfer domain, FP16 clipping/quantization or calibrated display-profile behavior. Advanced Color linear scRGB is a qualified starting point, not a legacy-SDR screen oracle.
- **Ray Reconstruction:** the fused-kernel K44/C16 tail-load blocker was resolved for the tested M5 Pro/macOS 27.0.1 non-fused route. The historical two-ABI, 36-output native parity and clean validation do not establish other OS/GPU or real-game quality/performance. R1's pointer test is a separate contract.
- **HDR10+ GAMING:** metadata, calibrated tone mapping, native Gaming/low-latency controls, transport, output ownership and restoration remain unresolved. Generic InfoFrame transport, ALLM/SBTM or ordinary HDR10+ video is not Gaming SSTM; no mandatory sink ACK was established.

Consult the public SDK/specification and a hash-verified pristine reference when native semantics remain ambiguous. Reference behavior is compatibility evidence, never a production dependency or proof that DXMT passes.

## Primary contracts

- [D3D12 fence synchronization and rewind](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)
- [Multiple-fence completion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-seteventonmultiplefencecompletion)
- [Advanced Color composition](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)
- [Metal 4 command queue](https://developer.apple.com/documentation/metal/mtl4commandqueue)
- [Metal 4 compiler archive lookup](https://developer.apple.com/documentation/metal/mtl4compilertaskoptions/lookuparchives)
