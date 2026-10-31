# 현재 구현 우선순위

기준일: **2026-10-10**. 지원 선언과 증거는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md), 반복 실행 절차는 [SMOKE](SMOKE-TEST.ko.md)와 [RUNTIME](RUNTIME.ko.md), 실제 과거 실행은 [HISTORY](IMPLEMENTATION-HISTORY.ko.md)에 둔다. 단계 번호가 같은 과거 P0–P3와 후속 backlog를 혼동하지 않는다.

## 2026-11-01 후속

[2026-11-01 저위험 최적화·Rust 경계](CORE-IMPLEMENTATION-STATUS.ko.md#확정-저위험-최적화와-남은-rust-경계--2026-11-01) 이후 남은 일: HEAD에서 생긴 bindless·dxil의 Shader Validation null-address load(`AIRCONV_VERSION=45` bounded load 의심) 조사, trace 수집이 필요한 후보(heap view-pool residency 순회, reserved 리소스 commit 분할, Present/Temporal `MarkUntracked`, IB 범위 병합, DEFAULT heap 텍스처 Private storage, table CBV select), DRED 기본값 결정, FSR FG pacing jitter와 동일 장면 FPS 측정, `run-command-recording-cpu.py` 컴파일 오류(`async_targets_epoch_`).

## 2026-10-31 최적화 묶음 후속

[2026-10-31 최적화](CORE-IMPLEMENTATION-STATUS.ko.md#d3d12-경로srfgrrrust-경계-최적화--2026-10-31)는 native ARM64 Wine+FEX의 x64 GPU suite를 통과했다. 남은 일은 세 가지다. trace counter로 동기화 감소를 측정하고, 같은 장면에서 FPS를 비교한다. x86 guest를 검증하고, Reflex fixed-two admission을 측정 probe와 함께 다시 판단하고, 기존 실패 항목인 fullscreen frame statistics·tiled/dxil-resource-review의 Shader Validation 중 compiler XPC 끊김·fsr marker·sm6 입력을 따로 처리한다.

## 현재 관리 범위

이 저장소는 DXMT D3D12 번역기와 그 검증을 관리한다. Wine SDK/runtime 빌드는 별도 Wine-darwin, FEX 소스·빌드는 FEX-darwin의 책임이다. 아래 완료 결과와 미완료 계약을 구분하며, 과거 세션의 실행 순서·중단·위임 지시는 현재 작업 지시로 유지하지 않는다.

## D3DMetal 최적화 이식 후 남은 검증

조사한 19개 중 18개 항목의 적합한 경로를 구현했다. 최종 phase3e의 16개 D3D12 GPU suite와 RR 검증 ON/OFF 각각 51개 출력이 통과했으며, 범위와 증거는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#d3dmetal-비교-기반-최적화-이식--2026-10-10)에 둔다. 전체 D3DMetal 알고리즘 동등성이나 게임 FPS 개선을 뜻하지 않는다.

| 항목 | 현재 판정 | 남은 조건 |
| --- | --- | --- |
| 동일 장면 게임 FPS·frame time | 사용자 요청으로 후속 검증 보류 | Steam 래퍼와 `-use-d3d12`로 실제 D3D12를 확인하고 동일 설정·장면·drawable·계측 조건을 맞춘 비교. 이전 DX11 실행과 불연속 HUD 수치는 제외 |
| Native SR 112×112 | Framework-only Conv3x3 BBR invalid store 재현 | 같은 입력의 독립 native 검증이 먼저 통과해야 한다. 128×128·96×112 controls는 clean이며, 출력 크기를 임의로 올린 뒤 crop하거나 validation을 끄지 않음 |
| RR 초기 cold-cache 경계 | 추가 ResponsivityMask 검사에서 비동기 초기화 timeout을 재현했고 최종 모델 동기 초기화 후32 frames 통과 | 생성 시 CPU 대기는 늘 수 있다. 전역 compiler cache·다른 OS/GPU·실게임 cold-start는 별도이며 모든 과거 timeout의 원인을 하나로 단정하지 않음 |

## 선정 리뷰 항목 처리 결과

현재 FG 작업은 원본 MIT AMD SDK pin `0836aa7f24058b3f8d035a88c0774457d78fc98e`의 source-built swapchain/pacing/UI와 기존 WMTMetal4FG native MetalFX generation을 연결하는 번역이다. x64/WoW64를 유지하고 AMD phase-DXIL N1–4 알고리즘은 제거했다. Generated output 최대1개: FSR2–4는 NO_PROVIDER, NGX count/index>1은 UnsupportedParameter다. OutputDisableInterpolation은 GPU가 history/reset/replay provider hint를 기록한다. Public MetalFX에는 actual-count/status API가 없고 pristine GPTK도 disable key를 무시한다. SDK generated-role bootstrap/reset output은 current-color일 수 있으며 native warm-up·실제 표시는 별도 관찰한다. Closed-list replay는 native 작업을 다시 encode한다. Ordinary ResizeBuffers는 유지하고 ResizeBuffers1 원본 success stub은 E_NOTIMPL로 바꿨다. Custom per-buffer queue나 새 async Present 오류 protocol은 없으며 NGX Present/pacing은 caller 소유다. Root27/28/30/34의 AMD 알고리즘 proof는 역사적으로 보존하되 현재 native FG proof로 세지 않는다. Native residency·Composition 독립 증거는 유지한다. `build/local/fsr-metalfx-20261007-43df9ae7`의 x64/x86 build는 통과했고 양 ABI FSR·NGX GPU·NGX Present 6개 실행이 API+Shader Validation으로 통과했다. 실제 표시 수·warm-up 경계는 CORE의 현재 FG 검증을 따른다.

| 순서 | 항목 | 현재 판정·실제 검증 |
| --- | --- | --- |
| 1 | R1 Metal4FX CPU pointer 인코딩 — 완료 | Null/zero/nonzero-base real wrapper와 FEX x86/x64 create·prepare·Composition descriptor 소비 PASS. Drawable/전체 FX proof는 아님 |
| 2 | R2/R3 fence event 구독·완료 — 완료 | ANY losing subscription·설정 실패 rollback, allocation-free 선형 ready 처리, 잠금 밖 callback, 경합·재진입 CPU/sanitizer와 양 ABI public fence/sharing PASS |
| 3 | R4 자동 PSO cache 잠금 분리 — 완료 | Blocked I/O와 독립 memory hit, clear/disable/retry generation·single-flight·explicit 오류 회귀 및 양 ABI public cache PASS |
| 4 | R5 빈 Wait submission 제거 — 완료 | 같은32-frame workload의 제출191→128, 정확한 cross-queue readback. 양 ABI inline CL11·rewind·수명·shutdown/device removal PASS |
| 5 | R6 application cache index — 완료 | 실제1,024-entry lookup x64 3,914→5.8ms·x86 4,377→6.8ms. Collision·손상·append rollback·작은 budget 재개방 회귀 PASS |
| 6 | R7 native PSO/archive — 계측 완료, cache 보류 | Owned-blob hit의 native PSO p50 x64/x86 152/111µs. 실제 게임 중복률·retained memory·graphics variant 비용이 없어 추가 객체 cache를 정당화하지 않음 |

설계가 존재하거나 컴파일됐다는 것과 public/GPU acceptance를 구분한다. 각 항목의 현재 적용·실행 결과는 리뷰 문서의 implementation ledger로 갱신한다. 기존 ring/FIFO·fence threshold semantics를 전면 재작성하거나 전역 GPU waits로 대체하지 않는다.

## 추가 FG 계약 구현 — 2026-10-10

NGX FG의 독립 Depth/MVecs/color origin, OutputReal, FP16/FP32 및 LowPrecision BidirectionalDistortionField, UI/UIAlpha 재합성은 [구현과 집중 공개 GPU 검증](CORE-IMPLEMENTATION-STATUS.ko.md#ngx-fg-subrectoutputrealdistortion--2026-10-10)을 마쳤다. [VS-only no-raster SO adjacency 4종](CORE-IMPLEMENTATION-STATUS.ko.md#vs-only-so-adjacency--2026-10-10)도 구현했다. 지정 ARM64X/FEX x64 proof이며 WoW64·다른 OS/GPU·실게임 화질/FPS는 별도다. 후속 orthographic·camera-motion 재구성과 GPU disable hint도 [집중 검증](CORE-IMPLEMENTATION-STATUS.ko.md#sm66fl12_2와-대형-계약-확장--2026-10-10)을 통과했다. 임의 시점 MFG N>1은 미구현이다.

## 남은 공통 게임 기능

SampleCmpGrad/Bias는 dynamic cube gradient 수정과 54-case 공개 검증 후 [OPTIONS21 지원을 공개](CORE-IMPLEMENTATION-STATUS.ko.md#samplecmpgradbias-공개-capability--2026-10-10)했다. 이는 상위 SM/FL 전체 admission 상향과 별개다.

| 항목 | 이미 있는 구현/증거 | 남은 조건 |
| --- | --- | --- |
| Composition absolute alpha/colour | Premultiplied FP16·G10/P709·PQ/P2020·G22/P2020 구현, 공개 HDR 계약과 paired drawable32개 수치·기존 Presenter11 frames PASS | 현재 ARM64 Wine DComp stub·target attachment 실패로 최종 compositor/scanout 미검증. Qualified Windows/reference/profile 비교는 별도 |
| NGX Ray Reconstruction | 기존 non-fused native 위임에 ResponsivityMask와 독립 origin 연결. 4 variants×8 frames·243,392 RGB 성분 반응·atlas/tight exact·release/replay PASS | 나머지 ray-direction/DoF/transparency/alpha 등의 guide는 정확한 native consumer가 없어 거부. NVIDIA 모델 동등성·다른 OS/GPU·실게임은 별도 |
| FSR Ray Regeneration | x64 RR 1.0/1.1/1.2 frontend와 MetalFX neural radiance·analytic scalar correction, public `fsr-rr` API+Shader Validation PASS | WoW64 x86·ARM64/FEX 실행, 실제 AMD RR 게임, AMD debug view, context 없는 memory 예측, AMD 모델·화질 동등성 |
| HDR10+ GAMING | 일반 HDR10/scRGB/PQ·EDR, generic transport 조사, Samsung APP/GPU·calibration 근거 | Supported Gaming payload/SSTM·native mode/readback·owner/restore·calibrated GPU tone-map·실제 적합 출력. Mandatory sink ACK를 임의로 요구하지 않음 |

이 항목은 게임 관련 기능의 미완료로 유지한다. ALLM/SBTM/HDR10+ video·DX11 SR·validation suppression·내부 상태 echo로 대체하지 않는다. 근거는 [검증 이력](IMPLEMENTATION-HISTORY.ko.md)과 [현재 리뷰의 native 계약](../doc/D3D12-REVIEW-AND-DESIGN-2026-10-07.md#outstanding-externalnative-contracts)을 따른다.

10월10일 후속 구현은 source transfer를 선형화하고 association·signed residual을 보존하며 paired RGBA16Float planes를 사용한다. 공개 생성과 실제 GPU 출력 oracle는 통과했지만 현재 Wine에서 최종 화면 부착은 실패했다. 설치된 macOS27 `CAEDRMetadata` 공개 API는 HDR10/HLG이며 검증된 Gaming SSTM 출력 제어 경로는 없다. Pristine GPTK4.0b2의 Composition factory는 E_NOTIMPL, SetHDRMetaData는 무처리 S_OK여서 이를 구현으로 이식할 수 없다. 이번에 sink EDID/VSVDB·물리 출력을 새로 측정한 것은 아니며, 플랫폼 전체의 불가능성으로 확대하지 않는다.

## 게임/장치 요구가 확인될 때 확장할 항목

| 항목 | 착수 조건 |
| --- | --- |
| Genuine CoreWindow | HWND/Composition fallback으로 충족되지 않는 실제 game UI/identity 호출 |
| 일반 Wave64/128 | 단일 block compute는 구현했다. Divergent CFG·graphics/RT/WG와 FP64/vector wide collectives는 남아 있으며 실제 shader 요구를 기준으로 확장 |
| 상위 공개 SM/feature level | 현재 SM6.6·조건부 FL12_2를 넘어서는 게임 요구와 해당 mandatory 계약. 기존 cap을 거부하던 public admission 자체는 구현·집중 검증 완료 |
| 상위 Agility package | 실제 EXE/configuration이 owned617/618 외 정확한 package를 요구 |
| Stereo / hardware plane | 일반 presentation/alpha fallback이 불충분한 실제 game/display 요구 |
| Linked node / multi-adapter | 실제 복수 GPU와 resource identity·queue/fence·node 실행 요구 |

Mask1은 node0 선택이지 multi-GPU 사용 증거가 아니다. Capability query만으로 그 기능이 필수라는 결론을 내리지 않는다. 반대로 선정 게임에서 미관측됐다는 이유로 모든 게임에 불필요하다고 단정하지 않는다. WARP/범용 video·capture·광범위 D3D11/Wine 확장은 자동 착수 범위 밖이다.

## 이미 완료한 기반과 증거 한계

선정 P0, P1 #5–9/#11, P2 depth bounds/conservative Tier3와 P3 #15–24는 CORE의 항목별 proof로 완료됐다. P1 #10의 총 GPU memory usage query는 사용자 제외 상태이지 구현 완료가 아니다. Atomic64·FP64·matrix·writable MSAA·wave4–32·Tiled4를 API 이름만 보고 다시 TODO로 넣지 않는다. Public SM6.6/조건부 FL12_2과 개별 higher/preview bytecode 지원은 별개다.

PipelineLibrary/CachedPSO/자동 RAM·disk cache/application ShaderCacheSession, COPY timestamps, programmable offsets, unaligned BC, CalculateLOD, budget notification, retained arena 성장, 선정 SR/alpha/FG/Reflex는 구현된 기반이다. 새로운 수명·ABI·정확성 결함은 이 완료 기록과 별도로 수정한다.

ARM64/FEX는 보류 상태가 아니다. 과거 SDK05·login03 fixture 기록과 별도로 10월9일 격리 ZZZ의 DLSS 도시 출력, 리소스 검사 완료 및 FEX 최적화 회귀를 확인했다([현재 증거](ZZZ-COMPATIBILITY.ko.md)). G8 전체 native migration matrix, 다른 게임·장면·전체 DXIL 및 동일 조건 로딩·스터터링 성능 검증은 남아 있다. Wine/FEX 생산과 입력 경계는 [ARM64/FEX](../doc/ARM64-FEX-IMPLEMENTATION.md)를 따른다.

## 검증과 성능 판정

Cold/warm startup, metadata-lock wait·disk I/O·owned compiler·native pipeline 단계 시간을 분리한다. 같은 workload의 p50/p95/p99 frame time, idle gap, submit 수, allocation/copy bytes, retained/peak memory, input-to-present와 전력은 각각 측정한 것만 주장한다. Synthetic PASS/속도를 전체 게임 FPS나 물리 표시 cadence로 바꾸지 않는다.

Fresh output/runtime·전용 prefix를 쓰며 기존 게임/live prefix에 설치하지 않는다. Runtime/fixture/module hash와 실제 실행 ABI·Metal validation·원본 실패를 보존한다. 테스트 종료 후 scoped Wine 프로세스가 멈췄을 때만 disposable prefix를 정리한다. Root captures와 출처가 불명확한 기존 산출물은 소스 커밋에 넣거나 임의로 삭제하지 않는다.

## 과거 결과의 해석

<a id="현재-남은-구현-목록--2026-10-05-정리"></a>
<a id="2026-10-03-중간-정리"></a>
과거 P0–P4의 중복 표·중간 보류·인수인계 문서는 삭제했다. 현재 판정에 필요한 증거와 당시 cut의 한계만 [검증 이력](IMPLEMENTATION-HISTORY.ko.md)에 남긴다. 과거 관측을 최신 소스의 재검증으로 세지 않는다.

## 2026-10-14 FPS 후속 우선순위

상태: 우선순위와 조건만 기록. P1 구현과 게임 FPS 개선 검증은 아직 하지 않았다.

이번 계측에서 도시 화면은 HUD 43.63 FPS, 22.92ms frame interval로 관측됐다. 그러나 trace가 시작 화면에서 먼저 100,000 event budget을 소진해, render 22개·compute 2개·blit 2개·clear 5개·draw 30개라는 상세 구성은 **로그인 구간 측정**이다. 따라서 이 숫자를 도시의 pass 수나 MetalFX 비용으로 사용하지 않는다. shader cache operation이 안정 구간에 없었다는 관측도 같은 trace 범위에 한정한다.

| 순위 | 과제 | 진행 조건과 금지 범위 |
| --- | --- | --- |
| **P1** | **depth-only render pass 재사용 후보 검증 및 최소 수정** | 기존 OMSetRenderTargets는 같은 RTV/DSV 재바인딩을 이미 생략한다. 먼저 bounded 도시 trace에서 NumRTV == 0의 동일 DSV가 zero_rtvs 이유로 반복 split되는지 확인한다. 실제로 발생하면 동일 retained DSV view/plane/aspect/flags/sample/dimensions/load-store 조건에서만 현재 encoder를 유지한다. 다른 attachment, barrier hazard, resolve, local/suspend, MSAA·visibility·predication·stream-output·temporal 경계는 계속 split한다. 상세 설계와 검증은 [D3D12 review](../doc/D3D12-REVIEW-AND-DESIGN-2026-10-07.md#2026-10-14-실게임-gpu-측정과-render-pass-재사용-설계)에 둔다. |
| **P2** | **도시 pass split reason 계측** | trace event budget을 시작 화면에서 소진하지 않도록 bounded capture 조건을 정한다. render_targets_rebind_rejected, render_targets_rebind_elided, pass_end, clear/load/store, resolve, barrier를 recording/list/encoder와 연결한다. 원인 카운터 없이 전체 barrier tracker나 submission merger를 재작성하지 않는다. |
| **P3** | **생성된 AIR/Metal 셰이더의 GPU 실행 비용** | P1/P2에서 실제 pass 원인이 아니거나 수정 후에도 GPU frame time이 높을 때만 진행한다. 같은 shader 입력의 instruction 수, texture/sample 수, descriptor lookup, register/occupancy를 비교한다. shader parse, function cache, native library cache를 steady-state FPS 해결책으로 다시 추가하지 않는다. |
| **P4** | **MetalFX temporal과 후처리 조합** | P3 셰이더 비용이 원인이 아닐 때 Metal System Trace에서 temporal encode, input preparation, output composite, blit/clear를 frame ID별로 식별한다. MetalFX를 끄는 우회는 하지 않고 같은 입력·출력 해상도와 history/reset 상태로 A/B한다. |
| **P5** | **CPU binding/residency 비용** | GPU가 아닌 CPU frame time이 병목으로 측정될 때만 진행한다. 기존 argument-table cache, residency-set dedup, root binding dirty suppression과 중복되는 새 cache나 D3DMetal IMP cache 복사는 추가하지 않는다. |

P1의 목표는 draw 수를 줄이는 것이 아니라 **실제로 반복되는 동일 depth-only binding을 더 적은 native render encoder로 기록하는 것**이다. 해당 호출이 도시에서 관측되지 않으면 P1을 ZZZ FPS 수정으로 주장하지 않고 P2~P4의 증거 수집으로 되돌린다. output hash·validation·barrier 순서를 보존하지 못하면 기존 split 경로를 유지한다.
