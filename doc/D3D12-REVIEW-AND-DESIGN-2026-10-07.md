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

## 2026-11-02 정적 감사 확정 후보 구현

기준은 `06538a3f16d0a8b1de53c5b861625b79ed69dd41`와 당시 작업 트리다. 기존 Anti-Lag·Reflex·packaging 변경은 보존했다. 별도 정적 감사의 후보 번호를 아래 구현에 대응시킨다. 160개 후보의 일괄 완료나 게임 FPS 개선을 뜻하지 않는다. 현재 검증 결과와 제한은 [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#정적-감사-확정-최적화--2026-11-02)가 관리한다.

| 순서 / 후보 | 적용한 변경 | 보존 조건 및 미포함 범위 |
| --- | --- | --- |
| 1 · L01 일부 | `MTLD3D12Device`가 고정 predication·mesh-indirect·strip·tess helper 8 PSO와 sampler-feedback encode/decode 2 PSO를 소유한다. `EnsurePredicationPipelines`·`DispatchFeedbackTranscode`는 list-local 참조를 사용하고 최초 누락만 device mutex 아래 생성한다. | Device/compiler 격리, partial-success 보존과 실패 후 재시도. Argument table·상수·recording state는 공유하지 않는다. 이 행은 1차 범위이며 나머지 fixed helper는 아래 2차에서 확장했다. |
| 2 · T01 | SR은 기존 weak device-library cache에서 필요한 PSO와 exposure PSO도 공유한다. AMD RR도 기존 kernel-library cache에 10개 불변 PSO를 둔다. | Library/PSO lazy 생성 및 실패 재시도. 준비된 SR frame은 shared cache mutex를 다시 잡지 않는다. Scaler·history·fence·mutable bindings는 feature별이다. |
| 3 · Q02 | `ProcessExecute`가 closed recording의 descriptor/root backing 목록과 RTAS pass 수를 task당 한 번 수집한다. | 각 segment의 `useAllocation`, allocator lease, RTAS snapshot/retirement는 유지한다. Descriptor 내용이나 GPU-write 데이터를 캐시하지 않는다. 큐 잠금·commit·wait 순서는 바꾸지 않았다. |
| 4 · L17 | Partial/non-folded resolve는 cache key를 먼저 조회하고 miss에서만 native function/PSO를 만든다. 동일 key의 depth/stencil state도 함께 보존한다. | Format·sample count·fragment kind·DSV plane 및 write/stencil 조건의 기존 key 사용. 완성된 객체만 캐시에 게시한다. Whole-average resolve folding은 기존 동작이다. |
| 5 · R01 | Texture SRV/UAV의 native pooled view 교체에서 선행 clear를 제거한다. | Pool 부적격 fallback은 clear하고, native set 실패도 명시적으로 null slot을 남긴다. Parent residency/lifetime 및 buffer/null descriptor 동작은 그대로다. |
| 6 · T02 | NGX RR retired scratch를 고정 3-slot pool로 관리하고 현재 working-set bytes의 3배로 idle budget을 제한한다. Exact extent 우선 선택, 불필요해진 scratch 해제, Dormant 후 late-retirement 재삽입 금지를 적용한다. | 사용 중인 prepared/execution lease는 상한과 무관하게 보존한다. Dormant 뒤 이미 기록된 frame의 replay는 자체 자원으로 실행한다. Model 내부 메모리는 이 byte budget에 포함하지 않는다. |
| 7 · T03 | AMD RR의 현재 neural extent와 직전 대체 extent 한 개를 보존해 A→B→A에서 모델 재생성을 피한다. | Generation이 바뀌면 history reuse를 명시적으로 끈다. 성공한 encode만 current를 갱신하고 Dormant에서 두 cache 참조를 해제한다. Evicted generation의 GPU lease는 계속 유효하다. |
| 8 · T14 일부 / T15 | NGX RR은 실제 input-conversion dispatch에 필요한 parameter slot만 작성한다. Device min/max scale capability는 feature 생성 시 한 번 조회한다. | 별도 output parameter slot과 native-format direct input을 유지한다. Packed normal/roughness kernel fusion은 이번 변경이 아니다. |

### 제외하거나 남긴 항목

초기 목록 P20의 CRC32 전제는 현재 `d3d12_shader_cache.cpp`와 맞지 않는다. 현재 구현은 SHA-1 검증 경로이므로 CRC lookup-table 최적화를 새로 넣지 않았다. 이미 구현된 no-UI Present bypass, SR zero-copy/generation cache, native command-buffer 재생성의 MetalTools 우회책도 중복 변경하지 않았다.

1차에서는 shader/root immutable ownership, 나머지 helper/resolve 공유를 후속으로 남겼고 아래 2차에서 이를 진행했다. 전역 temporal submission mutex 범위 축소, AMD RR cold zero-albedo clear의 CPU wait 제거, FG snapshot/normalization 및 identity pass 결합, wide-wave/Work Graph 알고리즘은 계속 후속이다. 외부 Wine/FEX 소스, public ABI, 해상도·노출·jitter·history reset 정책 및 품질 옵션을 변경하지 않았다.

### 회귀 검증의 역할

`run-static-optimization-cpu.py`는 production helper/descriptor 함수 본문을 실행해 동시 초기화·device 격리·부분 실패 재시도·replace/fallback/null의 native 호출 수를 검사한다. ASan/UBSan CPU stub 검증이며 GPU proof는 아니다. `owned-metalfx-optimization-native.mm --pipelines-only`는 실제 Metal library/PSO 호출을 관찰해 같은 device의 feature 공유와 동시 최초 생성, 예외 재시도를 검사한다. Default full image oracle은 별도로 유지한다. `owned-rr-motion-native.mm`은 bounded pool 정책, Dormant/late-retirement, conversion↔direct scratch 전환과 실제 NGX RR generation 출력 parity를 검사한다. 이 NGX 검사를 AMD A→B→A의 직접 증거로 바꾸지 않는다.

## 2026-11-02 정적 최적화 2차

1차 다음 순서인 불변 객체·원본 공유를 먼저 적용하고, 이어 의미가 같은 SR 준비 패스를 결합했다. 아래 변경은 게임 FPS 수치가 아니라 제거한 작업·보존한 계약을 기준으로 설명한다. 빌드/GPU 결과와 실패 경계의 단일 원장은 [CORE 2차](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#정적-최적화-2차--2026-11-02)다.

| 순서 | 구현 | 보존 조건 |
| --- | --- | --- |
| 1 · 나머지 fixed command helper | `SharedCommandHelper`와 device의 16-slot cache로 D24 왕복, RGB32·packed422 변환, SO assembly/index/preflight/GS, MSAA assembly/bounds, atomic copy32/64, matrix conversion, rays-indirect PSO를 공유한다. 1차 10개와 합쳐 command-list의 고정 compute helper 26종이다. | Kind별 소스·entry가 고정이고 compiler/library identity는 device가 고정한다. List-local hot 참조는 유지하고 miss만 잠근다. 네 inline-source library도 재사용하며 생성 실패 후 PSO만 재시도한다. 동적 application PSO나 모든 하위 시스템 helper의 완료를 뜻하지 않는다. |
| 2 · device resolve cache | 기존 8-entry list cache 뒤에 8-entry device cache를 둬 서로 다른 list도 native function/PSO/DS state를 공유한다. | 기존 full key와 실패 처리를 유지하고 완성된 entry만 게시한다. Local hit에는 device mutex가 없고 eviction 후에도 list/recorded pass의 참조는 유효하다. |
| 3 · shader/root 원본 공유 | `SharedShaderBytecode`·device별 `ShaderBytecodePool`로 root signature와 compute/graphics의 lazy statistics·index-pull·mesh·GS·HS/DS 원본을 공유한다. PS 이름과 root trace는 보관한 digest를 사용한다. | SHA-1은 lookup bucket일 뿐이며 **길이와 전체 bytes**가 같아야 공유한다. Caller 주소를 key로 쓰지 않고 입력을 소유한다. Weak metadata 최대256개, 가득 차면 신규 보관을 거절하는 대신 정상 owning snapshot을 반환한다. Pool 수명과 기존 PSO의 bytes 수명은 독립적이다. Mutable shader/LLVM handles는 공유하지 않는다. |
| 4 · native library key 직접 작성 | `DeviceShaderArtifactCache::Library`가 compiler bytecode를 canonical key의 최종 위치에 직접 복사한다. 기존 full-size 임시 문자열 한 개와 그 중간 복사를 제거한다. | Key prefix·byte 순서·disk schema가 동일하다. `SM50ReadCompiledBitcode`를 통해 직접 접근 가능한 cache blob과 WoW64 외부 native pointer를 모두 처리한다. |
| 5 · SR linearize/alpha 결합 | `yaagl_sr_linearize_alpha`가 같은 color subrect를 한 번 읽어 linear color와 선형 alpha coverage를 동시에 쓴다. 두 기능을 동시에 요청한 prepared frame에만 새 PSO를 준비한다. | 같은 `inverse_transfer` 수학·texture write format·hardware sRGB 해석을 사용한다. Alpha는 노출/색 변환을 받지 않는다. Padded alpha는 clamp한 원본 coverage를 쓰고, color padding은 기존 변환 완료값 복사와 barrier를 유지한다. Alpha-only/linearize-only 경로, history·queue ordering·finish pass는 그대로다. |

### 2차 검증과 남은 범위

Production-method sanitizer 검사는 추가 helper16종의 12-list 동시 요청, source library4개·builtin library1개 및 실패 재시도를 다룬다. 실제 bytecode header는 정상 SHA-1과 **모든 digest를 같게 만드는 test-only hash** 두 모드에서 caller mutation·동시 snapshot·크기/내용 충돌·256-entry admission·pool 종료 후 참조 유효성을 검사한다. 동시 cache miss에서 일시적 owning copy까지 하나만 발생한다는 주장은 하지 않는다.

`owned-metalfx-optimization-native.mm --prepasses-only`는 production `makeLease`/`encodePrepassMetal4`를 사용한다. RGBA16F·RGBA8·sRGB view × sRGB/PQ transfer × full/padded/얇은/nonzero-origin rect의24조건에서 standalone 두 pass와 fused pass의 **color/alpha 전체 byte 일치**, native dispatch 정확히1개 감소를 확인한다. 기존 pipeline failure/concurrent-create proof도 먼저 실행한다. 이 집중 검사를 기본 full SR image oracle나 게임 화질 전체 검증으로 바꾸지 않는다.

2차에서는 canonical stage-key, serialized payload, RT/WorkGraph 원본, SR parameter arena와 RR/FG·temporal 후속을 남겼다. 아래3차에서 구체적인 하위 계약을 구현했다. Persistent digest-only key 전환이나 모든 복사·동기화 제거를 뜻하지 않는다.

## 2026-11-02 정적 최적화 3차와 실패 경계 해소

| 영역 | 구현과 제거한 작업 | 보존한 계약 |
| --- | --- | --- |
| Stage key | `CanonicalStageKey`가 작은 scalar만 보관하고 큰 입력은 동기 lookup 동안 span으로 참조한다. Admission에서만 immutable owner를 잡는다. Streaming SHA-1은 lookup·trace·출력 identity가 재사용하고 disk miss에서만 기존 contiguous key를 만든다. | Hash는 identity가 아니다. Segmentation과 무관한 전체 byte equality, canonical 순서·persistent format·기존 digest를 유지한다. 호출당 최초 입력 hash는 남는다. |
| Native library·RT/WG 원본 | 접근 가능한 restored artifact는 library key에서 참조하고 외부 native pointer만 copy ABI로 가져온다. RT library와 Work Graph collection 원본도 `SharedShaderBytecode`로 공유한다. | Caller lifetime과 immutable owner를 분리한다. Mutable compiler/LLVM 객체와 GPU 작성 값은 공유하지 않는다. |
| Serialized payload | PSO별 private immutable blob을 한 번 만들고 automatic cache·PipelineLibrary가 참조한다. Deserialized named entries는 하나의 입력 snapshot에 대한 slice다. | 공개 `GetCachedBlob`은 매번 독립적인 writable copy다. Private owner는 앱에 노출하지 않는다. Caller 해제·동명이 아닌 alias·serialize determinism·실패 재시도를 유지한다. |
| SR parameter arena | 최대6개 소형 buffer 대신 실행별1536-byte arena와256-byte 정렬 slice를 쓰며 GPU 퇴역 뒤 pool로 돌려준다. | 살아 있는 execution/replay 사이에 writable constants를 공유하지 않는다. 주소·정렬·수학·padding barrier를 유지한다. |
| NGX RR packed 변환 | 같은 source/rect에서 normal·roughness를 변환할 때 한 dispatch가 원본을 한 번 읽는다. | Roughness는 rounded normal scratch가 아닌 원본 .w다. 다른 rect/extent, unpacked·direct normal은 기존 경로다. NaN/Inf를 보정하지 않는다. |
| AMD RR 초기화 | Zero-albedo용 legacy queue·commit·CPU wait를 없애고 최초 owned GPU prefix가 clear한다. 성공한 encode만 initialized를 게시한다. | Prefix barrier·native segment ordering·residency·lease로 첫 소비를 보호한다. 실패 뒤 initialized를 잘못 게시하지 않는다. Model 생성 자체의 비동기화는 아니다. |
| FG decode·복사 | 동일한 present/HUD-less view·rect·format이며 native usage가 read-only이면 decode 결과를 공유한다. Full real-output baseline 뒤 이동 ROI를 덮는 복사는 ROI 밖4개 band와 ROI로 대체한다. | 다른 입력·view·rect는 합치지 않는다. Output 전체와 바깥 sentinel을 보존하고 동일 위치·subrect-only는 그대로다. |
| Temporal CPU 잠금 | 첫 temporal 이전의 일반 prefix를 device temporal mutex 없이 인코딩한다. 첫 temporal 직전 잠그고 이후 commit·후속 segment의 기존 보유 규칙을 따른다. | History 조회는 잠금 뒤이며 wait/signal·partial encode·GPU barrier·commit 수는 동일하다. 첫 temporal 이후까지 per-feature 동시화한 것은 아니다. |

### 실패 원인과 검증 경계

SR standalone `runSR`와 `paddingOracle`는 D3D12 device가 제공하는 caller residency set을 붙이지 않아 첫 offset 출력이0이었다. 같은 명시적 residency/lifetime을 테스트에 제공해 default full oracle를 통과했다. 픽셀 허용치·forced-staging·padding 전체 byte 비교는 그대로다.

FG standalone도 caller residency와 관찰기의 native pointer 재사용을 정리했다. Cold-cache 검사 전에 replay 수명을 종료하고 현행 device6-PSO cache를 반영했다. 서로 다른 native content grid64/128은 모델 history가 다르므로 같은 출력이라고 가정하지 않는다. 대신 각 grid 안의 direct/scaled-motion/Prepare 뒤 입력변경을6프레임 비교하고 이전 provider와 후보의36개 출력도 비교했다. Dilation·fused normalization은 별도 CPU 수식 대비 kernel oracle를 유지한다.

외부 FEX emitter JSON2개는 현재 없다. 해당 문서는 수치·hash를 역사적 기록으로 명시하고 사라진 자료를 유효한 링크로 제시하지 않도록 고쳤다. 복원이나 외부 재검증이 아니다. 검사·hash·scope는 [CORE3차](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#정적-최적화-3차--2026-11-02)에 모은다.

3차에서 미완료였던 FG 준비 경로·feature별 제출·정수 wide-wave·Work Graph 및 atomic64 topology의 하위 계약은 아래4차에서 구현했다. 모든 입력의 무조건 변환 생략이나 임의 그래프 전체의 GPU scheduler를 의미하지 않는다. 게임 FPS·다른 OS/GPU·x86 guest FX는 독립 검증 범위다.

## 2026-11-02 정적 최적화 4차

| 대상 | 구현 | 순서·수명·정확성 조건 |
| --- | --- | --- |
| FG 준비 | Generate 기록이 rect/grid/포맷·camera 조건을 확정했고 Prepare가 아직 실행되지 않은 경우, Prepare에서 최종 depth/motion snapshot을 바로 생성한다. 단순 복사 후 재변환하는 중간 경로를 없앤다. | Jittered motion은 encode-time reset에 따라 보정이 달라져 기존 경로다. 이미 실행된 Prepare·직접 입력도 보존한다. 실패한 Generate는 plan을 게시하지 않으며 최종 snapshot은 GPU 퇴역까지 보존한다. 원본 source offset·invalid vector·camera·reset 수학은 공유 helper로 일원화했다. |
| Temporal 동시성 | device 전역 mutex 대신 `OwnedTemporalSubmissionLocks`가 feature별 예약을 command-buffer commit 및 timeline 게시까지 유지한다. 독립 feature는 병렬 인코딩한다. | 같은 feature는 이전 commit을 넘지 못한다. 여러 feature가 반대 순서로 필요하면 두 번째 잠금에 대기하기 전에 현재 segment를 commit·해제하고 빈 segment에서 예약한다. Ring capacity 대기 중 다른 예약을 잡지 않으며 cross-queue split 뒤 history를 다시 읽는다. GPU barrier/wait/signal은 유지한다. |
| Wide wave | 64·128-lane의 정확한 정수 reduction/prefix를 물리32-lane SIMD 결과와2/4개 partial로 구성한다. i64는32-lane shuffle tree, i16/i32는 native reduction, vote/ballot/count는 subgroup ballot을 사용한다. | 부분 logical/physical wave를 포함한다. 정수 wrap과 signed min/max를 보존한다. Float reassociation, 임의 numeric multiprefix와 divergent admission은 기존 ordered/fallback 경로다. |
| Work Graph | 자식 queue가 없는 terminal node의 독립 그룹을 일반65535개, atomic64는32개까지 묶는다. Scheduler는 dynamic dispatch-grid metadata만 읽고 나머지 record는 GPU에서 소비한다. | 원래 group coordinates·record completion counter·full input bounds를 유지한다. 출력 큐가 있는 node는 overflow/drain semantics를 위해 기존1그룹씩 처리한다. Atomic 참여자·노드 용량은 batch 크기에 맞춰 계산한다. |
| Atomic64 topology | 같은 owner/layout의 연속8-byte cell을 canonical ID도 연속일 때만 하나의 interval로 묶는다. | First-seen ID와 alias·subresource·sparse hole 도메인은 원래 cell map과 동일하다. Phase 사이 값이나 allocation identity를 재사용하지 않는다. Imported/exported 값은 매번 갱신한다. |

### 검증에서 드러난 기존 atomic64 계약 오류

새 typed consumer는 변경 전 runtime에서도 실패했다. 원인은 phase metadata가 ordinary native texture view ID로 색인됐지만 shader는 descriptor-pool slot ID를 사용한 것이었다. 실제 slot ID를 수집하고, 같은 물리 view의 서로 다른 slot에는 같은 canonical map을 가리키는 alias metadata를 게시한다. Import/export는 물리 view당 한 번 유지한다. Resource가 없는/null descriptor는 유효한 원본으로 꾸미지 않는다.

별도로 raw/structured bounded load는 최적화 마지막에 실제 LLVM load가 돼 atomic64 phase rewriting을 빠져나갔다. 같은 dispatch 안에서 atomic 결과는 shadow에 있는데 ordinary read가 오래된 backing을 읽던 오류다. Phase shader에서만 `lowerBoundedLoads`를 먼저 실행하여 읽기도 canonical cell을 사용하게 했다. 일반 shader의 delayed bounded-load 최적화, 범위 밖0·padding·4-byte 정렬 계약은 그대로다.

### 동등하지 않은 후보의 제외

Linear transfer·luminance0..80이어도 FG의 nominal identity decode/encode는 모든 half bitpattern에 대해 항등이 아니다. 실제65536개 half 패턴×4channel 검사에서 NaN canonicalization이 있었고 encode는 RGB negative zero도 바꿨다. 따라서 무조건 color/output 직접 연결은 기존 출력 bits를 보존하지 못한다. 해당 후보는 미구현 TODO가 아니라 **반증된 전제**로 제외한다. 별도 유효값 계약 없이 생략하지 않는다.

실제 통과/실패 로그, 입력 producer hash, native 및 public proof는 [CORE4차](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#정적-최적화-4차--2026-11-02)에 둔다. Float wave의 순서 변경·nonterminal 그래프의 GPU frontier·무제한 phase 간 topology cache는 이번 완료 범위가 아니다. 이는 기존처럼 임의 입력에 동일하다고 가정할 수 없는 별도 설계다.

## 2026-11-02 독립 개선과 제출 기반 재설계

43개 후속 검토 항목 중 사용자가 선택한 첫 번째 독립 개선과 두 번째 일반 D3D12 기반 재설계를 적용했다. 중단 전 stage5 변경을 그대로 이어받아 재검토했고, 기존 저지연·4차 변경은 보존했다. 현재 실행 결과와 실패 경계는 [CORE5차](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#독립-개선과-제출-기반-재설계--2026-11-02)에 둔다. 게임 FPS 개선율이나 다른 기능별 재설계 완료를 뜻하지 않는다.

### 독립 개선

| 경로 | 적용 | 보존 조건 |
| --- | --- | --- |
| Sampler·DS state | `OwnedNativeStates`가 device별 semantic key로 native sampler와 depth/stencil state를 최대256개씩 보존한다. | Padding·output ID는 key가 아니다. Sampler bias와 UINT border 등 caller metadata는 분리하고 reduction/compare/clamp는 key에 포함한다. Disabled stencil의 무의미한 필드만 무시한다. 실패는 재시도하며 eviction 뒤 caller 참조는 유효하다. |
| 퇴역 저장소 | CPU recording slab 최대3개·ICB를 allocator reset 뒤 재사용한다. Inflight vector는 참조를 해제하되256KiB 이하 capacity를 재사용한다. ICB의 capacity·semantic flags·binding 한도·options로 재사용하고 idle50epoch/총65536commands 정책으로 정리한다. | Outstanding GPU lease가 있으면 reset할 수 없다. GPU가 아직 읽는 기록/ICB를 재사용하지 않는다. Native Metal4 command-buffer 재생성의 기존 MetalTools 우회책은 유지한다. |
| Clear·query | Pending clear는 실제로 겹치는 자원의 소비 시점에만 확정한다. Render 뒤 독립 visibility target의 인접 run을 최대64개씩 한 encoder로 누산한다. | Reset/accumulation 경계와 같은 counter의 반복 쓰기·scratch alias에서는 분리한다. Heap texture는 물리 관계를 알아도 한 render pass에 합치지 않고 clear를 제자리에 둔다. 일반 query/timestamp 전체를 재정렬하지 않는다. |
| View·CPU texture 이동 | Texture view cache hit에서 exclusive publication을 피한다. Emulated format/unaligned pitch의 row transfer를 최대256KiB strip으로 묶는다. | Miss에서 재검사·소유권 게시가 필요하다. Format 변환·원래 row/slice pitch·경계와 native private CPU transfer 경로를 유지한다. |
| Shader-cache I/O | 전역 registry/control은 shared/exclusive lock, cache별 read/store I/O는 별도 mutex로 분리했다. | 같은 cache는 직렬이며 disable/clear/control은 전역 exclusive로 이전 실행과 순서를 보장한다. Public HRESULT·파일 format·cross-process 계약을 바꾸지 않는다. |

### 기반 재설계의 데이터 흐름

`Close → ClosedPassShape → Execute-time resolved bindings → PhysicalAccess/interval frontier → native submission → GPU retirement`로 책임을 나눴다. 파일 분할만 한 리팩토링이나 closed command list의 무조건 native replay cache가 아니다.

| 단계 | 현행 표현과 처리 | 안전성 경계 |
| --- | --- | --- |
| Close plan | `RecordingHead`가 encoder별 고정 형태, descriptor span, compute-command 해석 완전성, RTAS pass count와 record index를 소유한다. | Descriptor 값·GPU indirect argument는 Close에서 snapshot하지 않는다. `Close`의 allocation failure는 HRESULT로 전파하고 부분 plan을 게시하지 않는다. Record/heap/PSO 소유권은 기존 allocator lease로 유지한다. |
| 실행 binding | 현재 heap slot·root/VA·resource를 Execute에서 해석하고 native owner를 pin한다. | 같은 closed list를 descriptor 변경 후 재실행하면 새 값을 사용한다. Version을 근거 없이 가정하는 persistent resolved-binding cache는 넣지 않았다. |
| Physical access | `PhysicalIdentity`로 native buffer/texture·heap·feature history 도메인을 구분한다. Buffer는 byte range, texture는 mip/slice/aspect, placed buffer는 heap-offset interval로 표현한다. | Native owners를 GPU 퇴역까지 붙잡아 handle ABA를 막는다. Placed texture는 정확한 tiled byte footprint를 추측하지 않고 whole-heap 접근으로 보수 처리한다. Sparse/shared/cast/swizzle/불명확 indirect는 기존 opaque 경로다. |
| 의존성 frontier | `OwnedAccessFrontier`의 domain별 interval map이 겹치는 producer/reader만 방문한다. 계획 단계에서는 같은 노드의 stage를 보존하며 완전히 덮인 오래된 접근을 교체한다. | Partial overwrite의 reader와 다른 stage를 잃지 않는다. Inflight frontier는 아직 완료되지 않은 모든 producer를 retirement까지 보존한다. 기존64-node 합성 partition과 bounded opaque fallback은 유지한다. |
| Sparse publication | 실제 list의 resolved access가 sparse·heap·unknown을 쓰지 않는다고 확인된 경우만 process-wide AnyLive 때문에 발생하던 flush를 생략한다. | 현재 descriptor/등록 상태로 매 Execute 판단한다. 실제 mapping·heap alias·unknown 소비 및 pending volume publication은 기존 flush/wait를 유지한다. Sparse mapping 결과를 세대 검증 없이 캐시하지 않는다. |
| VA registry | `OwnedVARegistry<Rc<BufferAllocation>>`가 독립 shared lock에서 주소를 찾으면서 owner를 pin한다. 등록/해제에 generation을 갱신하고 lookup interval을 다음 등록 시작점에서 자른다. | raw pointer를 잠금 해제 후 쓰지 않는다. Native residency mutation/commit lock과 분리하되 native preCommit의 borrowed allocation 순회 보호는 유지한다. |
| Residency 게시 | 추가 allocation은 최초 native 소비 전 한 번에 commit한다. `GetGlobalResidencySet`과 실제 queue commit이 pending add를 게시한다. | 제거·명시적 MakeResident/Evict·오류/완료 semantics는 유지한다. CPU 게시를 GPU 완료로 취급하지 않는다. |

### 증거가 요구하는 것

`run-submission-foundation-cpu.py`는 실제 세 header와 sampler bridge를 사용한다. Byte/texture/heap interval30720개를 원래 전체 scan과 비교하고 같은 노드 stage·partial WAR·모든 inflight producer 보존을 검사한다.4096개 disjoint interval에서 실제 방문1개, VA registry의4-reader/20000-generation publication과 unregister 이후 owner 수명,12-thread sampler 중복 생성 제거 및 disabled-DS/padding/device/eviction/실패를 검사한다. 이는 CPU proof이며 GPU conformance가 아니다.

Public `submission-foundation`은 Close1회/descriptor4회 변경 replay, root-update ICB의 reset3epoch 재사용, 적법한 placed texture alias clear2개, nested occlusion32/16과 실제 fanout batch, reserved buffer2회 remap 후 alias read 및256KiB를 넘는 RGBA8/RGB32F strip의 원래값·pitch/pointer padding을 검사한다. 단순 dispatch-indirect shortcut을 ICB 재사용으로 세지 않도록 실제 root-update signature를 사용한다. 전체 render-pass의 disjoint256/512노드에서는 예전8064/16128개 history 비교 대신0개를 요구하며 opaque fallback으로 퇴행한 결과를 거부한다. Hazard가 있는 경우의 픽셀·원래 barrier/merge 경계는 기존 oracle로 계속 검사한다.

### 적용 밖과 기존 실패

정밀한 placed-texture byte footprint, sparse/shared/cast의 완전한 hazard 모델, descriptor version 기반 실행값 캐시, 모든 query kind의 native command-chain 결합, native Metal4 pipeline archive, RR working-set·strip/scan·custom MSAA·GPU frontier 재설계는 이 두 트랙의 완료로 주장하지 않는다. Unknown을 known으로 꾸미거나 synchronization을 지워 성능을 얻지 않았다. Tiled full suite의 3D UAV shader compiler XPC 오류는 동일 fixture의 이전 runtime에서도 재현되어 별도 기존 실패로 기록한다. 해당 실패를 PASS로 변경하거나 shader validation을 끄지 않는다.

## 2026-11-02 PSO 지연과 DXR 실행 객체 재설계

앞선 후보 B04 native pipeline archive와 B05 DXR executable bundle을 구현했다. D3D12 descriptor/AIR cache와 native GPU executable cache를 서로 다른 계층으로 유지한다. 전체 shader의 강제 사전 컴파일·게임별 품질/해상도 변경·ray tracing 알고리즘 변경은 없다. 생성 요청이 실제로 온 PSO만 수집한다. 현재 결과는 [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#pso-지연과-dxr-실행-객체--2026-11-02)에 둔다.

### PSO: 명시적 Metal4 archive 계층

`winemetal_metal4_pipeline.c`의 compute·linked compute·render·mesh 생성은 선택적 `metal4_pipeline_archive.inc`를 사용한다. 기존 `MTL4Compiler`는 실제 native 객체 그대로 MetalFX에 전달한다. Capture는 별도의 compiler/serializer가 수행해 외부 MetalFX compile과 serializer flush가 공유되지 않는다. 먼저 archive factory로 PSO를 요청하고, miss/부적합/오류면 현행 descriptor와 dynamic linkage로 컴파일한다. Capture 또는 archive 옵션이 실패하면 ordinary compiler+nil options로 다시 시도한다. 실패한 archive 때문에 유효한 PSO가 실패하는 계약은 만들지 않았다.

Writer가 serializer를 비우는 동안 새 요청은 ordinary compiler를 사용한다. 일반 compile caller는 디스크 저장·writer completion을 기다리지 않는다. Active capture가 끝난 idle window에서 serial writer가 flush하고, 마지막 device 해제는 명시적인 flush 경계다. 파일 publication용 잠금과 compile metadata 잠금을 분리했다. Delay1초는 GPU frame pacing이 아닌 optional disk harvest debounce다.

Namespace는 native module UUID·device registry ID·OS build·Wine prefix·validation 설정으로 나뉜다. Shard당64MiB, directory16개/256MiB와 live archive owner budget을 제한하고 pending/active capture도64개로 제한한다. 이는 serialized file byte 예산이며 실제 driver GPU memory 계측값이 아니다. UUID temporary file을 먼저 같은 device에서 다시 읽어 호환성을 확인한 뒤 flock·atomic rename으로 게시한다. Own regular file만 정리하며 symlink나 외부 파일을 archive로 신뢰하지 않는다.

성공한 public `ShaderCacheControl(IMPLICIT_DRIVER_MANAGED, ...)`의 disable/enable/clear를 native 계층에 전달한다. Clear/disable은 generation을 먼저 retire해 늦은 serialization이 이전 cache를 다시 채우지 못하게 한다. 이미 사용 중인 PSO/handle 수명은 유지한다. 자동화된 전역 GPU wait나 cache를 위한 게임 설정 강제는 없다. `D3DALLOY_METAL4_ARCHIVE=0`으로 선택적 경로를 끌 수 있고, 절대경로 `D3DALLOY_METAL4_ARCHIVE_DIR`과 opt-in `D3DALLOY_METAL4_ARCHIVE_TRACE=1`은 보관/진단용이다.

### DXR: 불변 native 실행 코드와 mutable binding 분리

기존 `RaytracingArtifact`는 CPU compiler의 bytecode·function/relocation metadata를 보존한다. `NativeRaytracingArtifact`는 그 다음 계층인 retained native library, library function descriptors, linked compute PSO와 function handles만 공유한다. Device별 weak single-flight cache가 동시 동일 요청을 하나의 native 생성으로 합친다. 마지막 live state/recording owner가 사라지면 큰 driver 객체를 cache가 계속 붙잡지 않는다.

Key는 compiler와 retained library identity, call-stack depth, **순서가 있는 모든 function name/Kind/ExportIndex/TableIndex/Flags**다. Entry 이름만 비교하거나 application state-object 주소를 identity로 쓰지 않는다. Library가 실행 코드·root layout·compiler options·constant ABI를 고정하고 native owner가 pointer ABA를 막는다. 일부 함수/handle 생성이 실패한 bundle은 ready entry로 게시하지 않으며 이후 요청은 재시도한다.

State-object token과 shader identifier, local-root 자료, relocation을 적용한 constant buffer, visible/intersection function table은 기존 state별로 유지한다. Dispatch binding과 GPU retirement 소유권도 공유 cache로 옮기지 않는다. Cache clear는 lookup membership만 retire하므로 살아 있는 이전 state object의 dispatch는 유효하다. Native table layout이나 shader arithmetic, recursion·hit/miss/intersection 의미를 바꾸지 않았다.

### 검증과 native 제한의 처리

Standalone actual bridge 검사는 cold process와 새 warm process를 분리한다. Warm에서는 native compiler 함수를 실패하도록 막았는데도 compute·specialized·linked·render·mesh의 GPU 출력이 정확해야 한다. 따라서 단순히 빠른 driver compile을 archive-hit라고 세지 않는다. Writer를 정지한 동안 clear와 다른 PSO compile을 실제 완료시켜 late publication과 I/O 대기 문제를 검사한다. 손상 파일·disabled·invalid directory·capture failure fallback도 별도다.

현재 macOS27.2/M5 Pro에서는 Shader Validation으로 instrument한 archive를 같은 public API가 deployment-target 호환성 오류로 거부했다. D3Dalloy를 포함하지 않는 최소 public control에서도 재현했다. Production은 그 bytes를 게시하지 않고 해당 compiler의 추가 capture를 중단하며, Shader Validation 설정을 끄지 않고 ordinary pipeline으로 실행한다. API-validation의 cold/warm archive proof와 API+Shader-validation의 fallback/GPU proof를 명시적으로 구분한다. 나중 OS에서 validated archive가 지원되면 검사도 실제 hit를 검증하며, 현재 실패를 영구 규격으로 hardcode하지 않는다.

Public `rt-executable-cache`는 동일 state4개 동시 생성→native link1개, warm 추가0개, local-root variant2개→link1개, clear 뒤 새 link1개를 trace로 확인한다. Triangle/any-hit/procedural/miss·root constant/CBV 출력과 살아 있는 이전 state·caller release-before-submit까지 검증한다. Relocated constant storage를 공유하지 않은 것은 소스 소유권 계약이며 synthetic10dispatch를 모든 상수/게임 workload의 전체 증명으로 확대하지 않는다.

### 남은 범위

실게임 frame-time/최초 사용 지연의 wall-time 수치는 별도다. Cold 첫 사용 자체와 archive miss는 여전히 compile이 필요하다. 전체 LLVM module의 export별 clone/DCE 재설계, application PSO 예측·오프라인 일괄 precompile, GPU RT traversal 재작성, MetalFX 모델 내부 최적화는 이번 scope가 아니다. Shader Validation archive 문제는 public-API 결과와 fallback으로 처리했으며 private header·proprietary runtime·반환 바이트 patch를 추가하지 않았다.

## 2026-11-02 독립 최적화 3종

사용자가 남은 후보 중 첫 추천 구현 묶음을 선택했다. Production 변경은 `owned_async_pipeline.hpp`, `d3d12_command_queue.cpp`, `d3d12_atomic64_phase.cpp`의 세 경로다. 기존 작업과 이 변경을 구분하는 [원본 대비 diff](../build/local/independent-opt-evidence-20261102/implementation.diff)를 보존한다.

### Async PSO worker의 소유권과 진행

`AsyncCompileJob::TryRun()`은 mutex 아래 `Queued`를 선점한 caller만 work를 실행한다. 이미 `Running`/`Done`이면 compile completion을 기다리지 않는다. Queue worker만 이 경로를 사용하고, 기존 `Run()`은 선점에 실패하면 `Done`을 기다리는 wrapper로 남긴다. 따라서 synchronous consumer의 `Wait()`와 owner teardown의 `Cancel()`은 기존 완료·수명 계약을 유지한다. `Finish()`의 release store와 status acquire, callback 예외의 Failed publication도 유지한다.

새 CPU 회귀는 consumer가 첫 job을 선점해 promise에서 멈춘 뒤 단일 worker를 시작한다. 첫 job을 release하기 전에 둘째 callback 완료를 요구하고 두 callback이 각각 한 번 실행됐는지 확인한다. Sleep 없이 순서를 고정하며5초 deadline은 hang watchdog이다. [수정 전 실패/수정 후 통과](../build/local/independent-opt-evidence-20261102/async-queue-before-after.json)를 실제 header와 ASan/UBSan으로 확인했고 기존 `run-static-optimization-cpu.py`에 포함했다.

### Custom MSAA 상태 chain

`EncodeMSAARender()`마다 query 참여와 gather 여부를 key로 최대4개 variant를 lazy 생성한다. 허용된 typed record를 `vector<variant<...>>`에 복사하며 모든 growth가 끝난 뒤 active record 주소로 `next`를 연결한다. 그 뒤 vector를 이동·확장하지 않으며 원본 recording과 bytes/viewport의 소유권을 유지한다. 빈 chain도 prepared로 기억하고 allocation 실패는 caller가 native encoder를 종료할 수 있도록 기존 false 경로로 전달한다.

Application PSO·scissor·이전 amplification을 제외하는 기존 필터, mesh producer 재실행 금지, line cull/conservative clamp와 gather 전용 depth bias를 그대로 적용한다. Query enable의 reset write와 memory barrier는 원래 순서대로 남기며 중복 상태도 제거하지 않는다. Native handler는 각 command를 순서대로 소비하므로 chain 전체를 한 번 전달한다. Draw/tile별 root·constant·scratch·coverage binding은 뒤에서 기존대로 수행한다. 이는 상태 replay의 CPU/bridge 비용 변경이며 tile execution algorithm 변경이 아니다.

### Atomic64 texture import 경계

각 transfer의 source는 읽기 전용이고 staging buffer는 독립 allocation이다. 첫 nonempty transfer에서 blit encoder와 fence wait를 만들고 모든 기존 copy를 순서대로 encode한 뒤 fence update·end를 한 번 수행한다. Copy할 transfer가 없으면 encoder를 만들지 않는다. Import compute dispatch의 fence wait와 GPU 퇴역 소유권은 유지한다. Destination alias 순서가 별도 의미를 갖는 export는 변경하지 않았다.

### 검증과 적용 경계

[CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#독립-최적화-3종--2026-11-02)의 paired/native/WoW64 build, CPU9개 출력, 같은 runtime의 x64 GPU6개 suite가 통과했다. 두 MSAA suite는 query·depth bias·line 경로, typed suite는6 shape×2 sign과 ordinary alias/format, phase suite는 GPU indirect·canonical/ordinary 값과 fence handoff를 검증한다. 모두 API+Shader Validation이며 [acceptance](../build/local/independent-opt-evidence-20261102/acceptance.json)에 source/runtime identity를 기록한다. 별도 대규모 테스트 체계나 production 계측을 추가하지 않았다.

최초 variant allocation·cold compile 비용은 남는다. 구조상 반복 작업 제거를 실게임 frame-time/FPS 개선율이나 x86·다른 GPU/OS의 검증으로 바꾸지 않는다. 최초 Rust 환경 실패와 DXIL preflight 거부는 원래 로그를 유지하며 최종 GPU oracle를 완화하지 않았다.
