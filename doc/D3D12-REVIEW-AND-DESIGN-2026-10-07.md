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
## 2026-10-14 실게임 GPU 측정과 render pass 재사용 설계

상태: 설계만 작성. 구현·배포·성능 개선 검증은 아직 하지 않았다.

### 측정 해석 정정

- `build/local/zzz-measure-1791894277406.trace`는 도시 화면에서 PID 40590에 연결한 10초 Metal System Trace다. 별도 화면 관측은 43.63 FPS, 22.92ms, MetalFX 2256x1272 -> 3840x2160이다. 동시 같은 프레임의 값으로 취급하지 않는다.
- **render 22개, compute 2개, blit 2개, 별도 clear encoder 5개, draw 30개, dispatch 2개와 GPU 평균 6.98ms는 로그인 구간의 측정이다.** 시작부터 켠 D3D12 trace가 100,000건에서 소진됐다. 이를 도시의 pass 구성이나 MetalFX 비용으로 설명한 이전 보고는 정정한다. clear encoder도 render 작업이므로 일반 render 22개와 별도로 센다.
- event 90,000 이후 cache operation이 없었던 것도 해당 로그인 trace 범위의 관측이다. 도시 전체의 compilation miss 부재나 프리컴파일 완료 시간의 증거가 아니다.
- 이전 99.4% 계산은 XML의 ref/id를 완전히 복원하지 않고 D3D12 label이 직접 있는 행의 interval을 합친 값이다. 중첩 GPU interval이 순수 실행인지, wait를 포함하는지 검증하지 않았으므로 **확정 GPU utilization 또는 shader busy 비율로 사용하지 않는다.** 원본 trace와 export는 보존하고 frame/process/queue별로 다시 해석한다.
- 동일 장면 GPTK4 trace는 없다. draw당 pass 수가 높다는 사실만으로 과도한 split이나 11ms의 제거 가능한 비용을 확정할 수 없다. 서로 다른 render target을 사용하는 후처리는 원래 별도 pass가 필요할 수 있다.

### 기존 구현과 실제 누락

`src/d3d12/d3d12_command_list.cpp`의 `OMSetRenderTargets`에는 이미 retained view·DSV flags·dimensions·sample·load/store 비교와 `render_targets_rebind_elided/rejected`가 있다. 같은 RTV/DSV 반복 bind를 생략하는 기능을 새로 만들지 않는다. `PreDraw`는 sample/feature 제약을 검사하고, 현재 render encoder가 없을 때만 새 pass를 만든다. `EndSchedulingPass`는 `InvalidateCurrentPass`로 recording 경계를 만든다.

구체적인 누락 후보는 재사용 진입 조건의 `NumRTV`다. **RTV가 0개이고 같은 DSV만 반복 지정하는 depth-only draw는 비교를 시도하지 않고 항상 split한다.** shadow/depth prepass에 해당할 수 있지만 ZZZ 도시에서 이 호출이 얼마나 발생하는지는 아직 확인되지 않았다.

제출 단계에는 이미 `src/d3d12/d3d12_command_queue.cpp`의 `AppendRenderMember`, `RenderReplayGroup`와 resource overlap/feedback/boundary 검사도 있다. recording pass 감소가 native encoder 감소로 이어지는지 따로 확인한다. 기존의 안전한 attachment mapping을 없애거나 별도 merger를 만들지 않는다.

### 1순위 수정안: 동일 DSV의 depth-only 재바인딩 유지

1. 기존 `OMSetRenderTargets` 비교 진입 조건을 color pass 또는 유효한 DSV가 있는 depth-only pass로 확장한다. `NumRTV == 0 && DSV && DSV->ptr`인 경우에도 기존 비교를 수행하되, 이전 `render_target_count` 역시 0이어야 한다. RTV/DSV 모두 없는 unbind는 계속 경계로 남긴다.
2. RTV loop는 0회 실행하고, 기존 DSV 비교로 retained TextureView, format/view가 나타내는 mip·slice 범위, depth/stencil aspects, plane, descriptor flags, sample count/pattern을 확인한다. CPU descriptor 주소만 비교하지 않는다. 같은 slot의 overwrite나 allocation 교체는 retained view 불일치로 split한다.
3. dimensions와 array length는 DSV descriptor에서 계산해 현재 pass와 비교한다. 기존 단일 sample·ordinary pass 조건, read-only/feature 제약, load/store 검사를 그대로 유지한다. 새로운 compatibility key/cache/result type은 만들지 않는다. 이미 보존된 view와 metadata를 사용한다.
4. 일치하면 기존 성공 경로로 OM bookkeeping만 갱신하고 현재 encoder를 유지한다. 이전 load-action Clear는 pass 시작에서 한 번만 적용된 상태이므로 이를 다시 실행하지 않는다. PSO/viewport/root binding 변경은 기존 dirty-state 경로로 반영한다.
5. 불일치하면 기존 split 동작을 유지한다. barrier/alias/UAV/split barrier, query/timestamp, predication, clear/resolve, explicit Begin/EndRenderPass와 suspend/resume/local, MSAA·atomic64·SO·mesh·temporal 경계는 이번에 완화하지 않는다. draw 이후 clear를 pass 시작 load-action으로 옮기지 않는다.

동일 RTV+DSV는 기존 기능이며 이번 변경의 성능 이득으로 세지 않는다. null RTV slot, read-only DSV 조건 완화와 attachment 없는 draw의 sample 호환성은 별도 증거가 있을 때 후속으로 검토한다.

### 구현 파일과 검증

- Production: `src/d3d12/d3d12_command_list.cpp`의 OM 조건만 우선 수정한다. allocator/queue/compiler/Winemetal ABI와 exports, config는 변경하지 않는다. 다른 저장소의 수정이나 새 의존성은 없다.
- Regression: `graphics-translator/tests/owned-d3d12-render-passes.cpp`의 `redundantDepthStencilBindings` 옆에 color attachment 없는 반복 DSV case를 추가한다. 기존 case는 RTV 1개이므로 누락을 잡지 못한다. depth write/test 순서와 stencil 보존의 GPU readback, 동일 DSV alias, descriptor overwrite, 다른 slice/resource, unbind 후 rebind를 포함한다.
- 같은 DSV 반복 N회에서 recording render pass 1개를 확인하고 실제 `native_render_begin`도 대조한다. 변경된 DSV/flags/unbind/barrier는 기존 경계를 유지해야 한다. 정확한 픽셀·depth 결과뿐 아니라 불필요한 pass 재생성도 regression이 검출해야 한다.
- Runner: 기존 `scripts/test-owned-graphics.py`의 render-passes 결과 검사에 신규 machine-readable case와 counts를 연결한다. parser를 바꾸면 `scripts/test-owned-graphics-diagnostics.py`도 검증한다. 현재 다른 작업의 runner 수정은 보존한다.
- Build/GPU: 현행 SMOKE/RUNTIME 절차로 해당 ABI 빌드, render-passes와 영향 있는 query/enhanced-barrier controls를 Metal validation ON으로 실행한다. fence 완료 후 readback하며 sleep으로 맞추지 않는다. validation OFF 성능 실행은 correctness 검증과 분리한다.

### 도시에서 효과를 인정하는 조건

구현 전 도시의 bounded trace를 확보한다. 시작 화면에서 전역 event budget을 소진한 trace는 제외한다. 기존 reject/elided 이벤트와 recording/list/encoder ID를 연결해 depth-only 동일 DSV의 `zero_rtvs` 거부가 실제 발생하는지 확인한다. `EndSchedulingPass`의 line 213만으로 호출자를 판정하지 않는다. 필요한 경우 이 wrapper의 source_location 전달만 보완한다.

도시에서 해당 case가 없다면 이 수정은 일반 경로의 작은 개선일 뿐 ZZZ FPS 해결책으로 주장하지 않는다. 이때 남은 이유 중 실제 비용이 큰 경계만 후속 대상으로 고른다. 무조건 큰 pass merger로 확대하지 않는다.

같은 runtime hash·도시 위치/카메라·해상도·MetalFX·FG·VSync·캐시 상태에서 trace OFF A/B를 수행한다. frame time p50/p95, native encoder 수, 해당 pass의 GPU 구간을 비교한다. 전체 GPU active 비율은 100% 가까이 유지돼도 FPS가 개선될 수 있으므로 감소 자체를 성공 조건으로 삼지 않는다. correctness 보존과 제거 대상 경계 감소, 재현 가능한 frame-time 개선이 실게임 완료 조건이다.
