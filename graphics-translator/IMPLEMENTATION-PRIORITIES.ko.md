# 현재 구현 우선순위

기준일: **2026-10-10**. 지원 선언과 증거는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md), 반복 실행 절차는 [SMOKE](SMOKE-TEST.ko.md)와 [RUNTIME](RUNTIME.ko.md), 실제 과거 실행은 [HISTORY](IMPLEMENTATION-HISTORY.ko.md)에 둔다. 단계 번호가 같은 과거 P0–P3와 후속 backlog를 혼동하지 않는다.

## 현재 관리 범위

이 저장소는 DXMT D3D12 번역기와 그 검증을 관리한다. Wine SDK/runtime 빌드는 별도 Wine-darwin, FEX 소스·빌드는 FEX-darwin의 책임이다. 아래 완료 결과와 미완료 계약을 구분하며, 과거 세션의 실행 순서·중단·위임 지시는 현재 작업 지시로 유지하지 않는다.

## D3DMetal 최적화 이식 후 남은 검증

조사한 19개 중 18개 항목의 적합한 경로를 구현했다. 최종 phase3e의 16개 D3D12 GPU suite와 RR 검증 ON/OFF 각각 51개 출력이 통과했으며, 범위와 증거는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#d3dmetal-비교-기반-최적화-이식--2026-10-10)에 둔다. 전체 D3DMetal 알고리즘 동등성이나 게임 FPS 개선을 뜻하지 않는다.

| 항목 | 현재 판정 | 남은 조건 |
| --- | --- | --- |
| 동일 장면 게임 FPS·frame time | 사용자 요청으로 후속 검증 보류 | Steam 래퍼와 `-use-d3d12`로 실제 D3D12를 확인하고 동일 설정·장면·drawable·계측 조건을 맞춘 비교. 이전 DX11 실행과 불연속 HUD 수치는 제외 |
| Commit 시점 Present 예약 | 미이식, completion 기반 표시 유지 | 이미 예약한 표시의 취소 또는 epoch 조건부 표시와 GPU 오류 시 억제를 보존하는 native 계약. D3DMetal의 더 약한 RESTART 처리를 그대로 이식하지 않음 |
| Native SR 112×112 | Framework-only Conv3x3 BBR invalid store 재현 | 같은 입력의 독립 native 검증이 먼저 통과해야 한다. 128×128·96×112 controls는 clean이며, 출력 크기를 임의로 올린 뒤 crop하거나 validation을 끄지 않음 |
| RR 초기 cold-cache 경계 | 비동기 초기화와 기존 public fence 연결, 최종 ON/OFF 각 51개 whole-buffer exact | 전역 compiler cache와 최초 생성 readiness를 통제한 관찰, 다른 OS/GPU 및 실제 게임 검증. 과거 timeout을 동기 설정이나 fence 하나의 원인으로 단정하지 않음 |

## 선정 리뷰 항목 처리 결과

현재 FG 작업은 원본 MIT AMD SDK pin `0836aa7f24058b3f8d035a88c0774457d78fc98e`의 source-built swapchain/pacing/UI와 기존 WMTMetal4FG native MetalFX generation을 연결하는 번역이다. x64/WoW64를 유지하고 AMD phase-DXIL N1–4 알고리즘은 제거했다. Generated output 최대1개: FSR2–4는 NO_PROVIDER, NGX count/index>1 및 non-null OutputDisableInterpolation은 UnsupportedParameter다. Public MetalFX에는 actual-count/status API가 없고 pristine GPTK도 disable key를 무시한다. SDK generated-role bootstrap/reset output은 current-color일 수 있으며 native warm-up·실제 표시는 별도 관찰한다. Closed-list replay는 native 작업을 다시 encode한다. Ordinary ResizeBuffers는 유지하고 ResizeBuffers1 원본 success stub은 E_NOTIMPL로 바꿨다. Custom per-buffer queue나 새 async Present 오류 protocol은 없으며 NGX Present/pacing은 caller 소유다. Root27/28/30/34의 AMD 알고리즘 proof는 역사적으로 보존하되 현재 native FG proof로 세지 않는다. Native residency·Composition 독립 증거는 유지한다. `build/local/fsr-metalfx-20261007-43df9ae7`의 x64/x86 build는 통과했고 양 ABI FSR·NGX GPU·NGX Present 6개 실행이 API+Shader Validation으로 통과했다. 실제 표시 수·warm-up 경계는 CORE의 현재 FG 검증을 따른다.

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

NGX FG의 독립 Depth/MVecs/Backbuffer subrect, OutputReal, FP16/FP32 BidirectionalDistortionField는 [구현과 공개/native 검증](CORE-IMPLEMENTATION-STATUS.ko.md#ngx-fg-subrectoutputrealdistortion--2026-10-10)을 마쳤다. 지정 ARM64X/FEX x64 proof이며, 새 계약의 WoW64·실게임 렌즈 왜곡 품질·LowPrecision 변환·독립 InputBackbuffer/HUDLess/OutputInterpolated origin은 별도 미검증/미지원 경계다. 기존 단일 generated output 제한과 truthful status 정책은 유지한다.

## 남은 공통 게임 기능

SampleCmpGrad/Bias는 dynamic cube gradient 수정과 54-case 공개 검증 후 [OPTIONS21 지원을 공개](CORE-IMPLEMENTATION-STATUS.ko.md#samplecmpgradbias-공개-capability--2026-10-10)했다. 이는 상위 SM/FL 전체 admission 상향과 별개다.

| 항목 | 이미 있는 구현/증거 | 남은 조건 |
| --- | --- | --- |
| Composition absolute alpha/colour | 공용 Presenter, geometry·matrix·attachment·resize/lifetime·cadence, 양 ABI104 backing captures | Qualified Windows/reference transfer·association·FP16 quantization/clipping·profile oracle. Backing 결과와 물리 scanout은 별도 |
| NGX Ray Reconstruction | Native validation blocker 해소. 비공개 version0x10000으로 Apple non-fused GPU 경로 선택; 독립 native50 frame·양 ABI 각각36출력 RGB 오차0·API/Shader Validation 오류0·입력 동등성 PASS | 다른 OS/GPU에서 private 선택자의 계약, 실제 게임 loader·화질/성능 검증. 자체 RR 알고리즘 추가가 아닌 native 위임을 유지한다. [현재 증거](CORE-IMPLEMENTATION-STATUS.ko.md#rr-native-validation-재검증--2026-10-07) |
| HDR10+ GAMING | 일반 HDR10/scRGB/PQ·EDR, generic transport 조사, Samsung APP/GPU·calibration 근거 | Supported Gaming payload/SSTM·native mode/readback·owner/restore·calibrated GPU tone-map·실제 적합 출력. Mandatory sink ACK를 임의로 요구하지 않음 |

이 항목은 게임 관련 기능의 미완료로 유지한다. ALLM/SBTM/HDR10+ video·DX11 SR·validation suppression·내부 상태 echo로 대체하지 않는다. 근거는 [검증 이력](IMPLEMENTATION-HISTORY.ko.md)과 [현재 리뷰의 native 계약](../doc/D3D12-REVIEW-AND-DESIGN-2026-10-07.md#outstanding-externalnative-contracts)을 따른다.

10월10일 재확인에서 native FP16 drawable 자체는 사용 가능하지만 premultiplied FP16의 기본 G10 색공간과 SDR residual 경로의 정합 oracle는 확보하지 못했다. 형식 제한만 해제하지 않는다. 설치된 macOS27 `CAEDRMetadata` 공개 API는 HDR10/HLG이며 검증된 Gaming SSTM 출력 제어 경로는 없다. Pristine GPTK4.0b2의 Composition factory는 E_NOTIMPL, SetHDRMetaData는 무처리 S_OK여서 이를 구현으로 이식할 수 없다. 이번에 sink EDID/VSVDB·물리 출력을 새로 측정한 것은 아니며, 플랫폼 전체의 불가능성으로 확대하지 않는다.

## 게임/장치 요구가 확인될 때 확장할 항목

| 항목 | 착수 조건 |
| --- | --- |
| Genuine CoreWindow | HWND/Composition fallback으로 충족되지 않는 실제 game UI/identity 호출 |
| 필수 Wave64/128 | 실제 shader의 fixed width 또는32를 허용하지 않는 range |
| 상위 공개 SM/feature level | 현재 SM6.5/FL12_1 admission을 거부하는 게임과 해당 mandatory 계약 검증 |
| 상위 Agility package | 실제 EXE/configuration이 owned617/618 외 정확한 package를 요구 |
| Stereo / hardware plane | 일반 presentation/alpha fallback이 불충분한 실제 game/display 요구 |
| Linked node / multi-adapter | 실제 복수 GPU와 resource identity·queue/fence·node 실행 요구 |

Mask1은 node0 선택이지 multi-GPU 사용 증거가 아니다. Capability query만으로 그 기능이 필수라는 결론을 내리지 않는다. 반대로 선정 게임에서 미관측됐다는 이유로 모든 게임에 불필요하다고 단정하지 않는다. WARP/범용 video·capture·광범위 D3D11/Wine 확장은 자동 착수 범위 밖이다.

## 이미 완료한 기반과 증거 한계

선정 P0, P1 #5–9/#11, P2 depth bounds/conservative Tier3와 P3 #15–24는 CORE의 항목별 proof로 완료됐다. P1 #10의 총 GPU memory usage query는 사용자 제외 상태이지 구현 완료가 아니다. Atomic64·FP64·matrix·writable MSAA·wave4–32·Tiled4를 API 이름만 보고 다시 TODO로 넣지 않는다. Public SM6.5/FL12_1과 개별 higher/preview bytecode 지원은 별개다.

PipelineLibrary/CachedPSO/자동 RAM·disk cache/application ShaderCacheSession, COPY timestamps, programmable offsets, unaligned BC, CalculateLOD, budget notification, retained arena 성장, 선정 SR/alpha/FG/Reflex는 구현된 기반이다. 새로운 수명·ABI·정확성 결함은 이 완료 기록과 별도로 수정한다.

ARM64/FEX는 보류 상태가 아니다. 과거 SDK05·login03 fixture 기록과 별도로 10월9일 격리 ZZZ의 DLSS 도시 출력, 리소스 검사 완료 및 FEX 최적화 회귀를 확인했다([현재 증거](ZZZ-COMPATIBILITY.ko.md)). G8 전체 native migration matrix, 다른 게임·장면·전체 DXIL 및 동일 조건 로딩·스터터링 성능 검증은 남아 있다. Wine/FEX 생산과 입력 경계는 [ARM64/FEX](../doc/ARM64-FEX-IMPLEMENTATION.md)를 따른다.

## 검증과 성능 판정

Cold/warm startup, metadata-lock wait·disk I/O·owned compiler·native pipeline 단계 시간을 분리한다. 같은 workload의 p50/p95/p99 frame time, idle gap, submit 수, allocation/copy bytes, retained/peak memory, input-to-present와 전력은 각각 측정한 것만 주장한다. Synthetic PASS/속도를 전체 게임 FPS나 물리 표시 cadence로 바꾸지 않는다.

Fresh output/runtime·전용 prefix를 쓰며 기존 게임/live prefix에 설치하지 않는다. Runtime/fixture/module hash와 실제 실행 ABI·Metal validation·원본 실패를 보존한다. 테스트 종료 후 scoped Wine 프로세스가 멈췄을 때만 disposable prefix를 정리한다. Root captures와 출처가 불명확한 기존 산출물은 소스 커밋에 넣거나 임의로 삭제하지 않는다.

## 과거 결과의 해석

<a id="현재-남은-구현-목록--2026-10-05-정리"></a>
<a id="2026-10-03-중간-정리"></a>
과거 P0–P4의 중복 표·중간 보류·인수인계 문서는 삭제했다. 현재 판정에 필요한 증거와 당시 cut의 한계만 [검증 이력](IMPLEMENTATION-HISTORY.ko.md)에 남긴다. 과거 관측을 최신 소스의 재검증으로 세지 않는다.
