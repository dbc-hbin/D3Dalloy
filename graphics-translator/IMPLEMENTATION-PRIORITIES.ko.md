# 현재 구현 우선순위

기준일: **2026-10-07**. 지원 선언과 증거는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md), 반복 실행 절차는 [SMOKE](SMOKE-TEST.ko.md)와 [RUNTIME](RUNTIME.ko.md), 실제 과거 실행은 [HISTORY](IMPLEMENTATION-HISTORY.ko.md)에 둔다. 단계 번호가 같은 과거 P0–P3와 후속 backlog를 혼동하지 않는다.

## 현재 사용자 선택

문서를 현행화하고 인수인계를 보존 이관한 뒤 Wine/FEX/DXMT 변경을 논리적인 로컬 커밋으로 분리한다. 이어 [2026-10-07 리뷰·설계](../docs/D3D12-REVIEW-AND-DESIGN-2026-10-07.md)의 결함과 근거가 있는 최적화를 구현·검증한다. 과거 중단 지시는 현행 보류가 아니다. Web GPT Agent의 agents 도구는 사용하지 않는다. 사용 가능한 독립 실행 도구가 없는 경우 위임했다고 주장하지 않는다.

## 먼저 처리할 리뷰 항목

| 순서 | 항목 | 완료 기준 |
| --- | --- | --- |
| 1 | R1 Metal4FX의 CPU pointer 인코딩 | Null/zero-base/nonzero-base actual wrapper 계약과 해당 native x86 소비 경로; GPU 주소·native handle은 변환하지 않음 |
| 2 | R2/R3 fence event 구독 정리와 완료 비용 | ANY losing subscriptions·설정 실패 rollback, 선형 ready 처리, 잠금 밖 callback, rewind/removal/경합·재진입 회귀 |
| 3 | R4 자동 PSO cache 잠금 분리·계측 | 디스크 지연이 무관한 memory hit를 막지 않음; clear/disable/retry generation·single-flight·explicit CachedPSO 오류 보존 |
| 측정 후 | R5 빈 Wait submission, R6 application cache index, R7 native PSO/archive | 실제 사용/단계 비용을 측정하고 각 순서·수명·identity·용량 계약의 회귀로 정당화. 미측정 FPS 효과를 약속하지 않음 |

설계가 존재하거나 컴파일됐다는 것과 public/GPU acceptance를 구분한다. 각 항목의 현재 적용·실행 결과는 리뷰 문서의 implementation ledger로 갱신한다. 기존 ring/FIFO·fence threshold semantics를 전면 재작성하거나 전역 GPU waits로 대체하지 않는다.

## 남은 공통 게임 기능

| 항목 | 이미 있는 구현/증거 | 남은 조건 |
| --- | --- | --- |
| Composition absolute alpha/colour | 공용 Presenter, geometry·matrix·attachment·resize/lifetime·cadence, 양 ABI104 backing captures | Qualified Windows/reference transfer·association·FP16 quantization/clipping·profile oracle. Backing 결과와 물리 scanout은 별도 |
| NGX Ray Reconstruction | Feature13 native denoised scaler, 기존 양 ABI numerical parity, K44/C16 tail 접근 조사 | Native 오류 없는 API/Shader Validation와 history/reset/replay·public acceptance. R1 수정은 native K-tail 해결과 다름 |
| HDR10+ GAMING | 일반 HDR10/scRGB/PQ·EDR, generic transport 조사, Samsung APP/GPU·calibration 근거 | Supported Gaming payload/SSTM·native mode/readback·owner/restore·calibrated GPU tone-map·실제 적합 출력. Mandatory sink ACK를 임의로 요구하지 않음 |

이 항목은 게임 관련 기능의 미완료로 유지한다. ALLM/SBTM/HDR10+ video·DX11 SR·validation suppression·내부 상태 echo로 대체하지 않는다. 현행 조사는 [이관된 원본](../docs/history/parallel-resume2-20261005.md)과 리뷰의 별도 blocker 설계를 따른다.

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

ARM64/FEX는 보류 상태가 아니다. SDK05의 세 guest ABI/Metal·각8-frame no-present proof가 있고, 그 runtime payload는 이후 정리된 이력과 구분한다. 현재 builder의 Wine11.17/GnuTLS/FreeType, FEX 선정 패치 및 10월7일 frontend 증거는 [ARM64/FEX](../docs/ARM64-FEX-IMPLEMENTATION.md)를 따른다. G8 native migration matrix·실제 게임/presentation·전체 DXIL 검증은 아직 남아 있다.

## 검증과 성능 판정

Cold/warm startup, metadata-lock wait·disk I/O·owned compiler·native pipeline 단계 시간을 분리한다. 같은 workload의 p50/p95/p99 frame time, idle gap, submit 수, allocation/copy bytes, retained/peak memory, input-to-present와 전력은 각각 측정한 것만 주장한다. Synthetic PASS/속도를 전체 게임 FPS나 물리 표시 cadence로 바꾸지 않는다.

Fresh output/runtime·전용 prefix를 쓰며 기존 게임/live prefix에 설치하지 않는다. Runtime/fixture/module hash와 실제 실행 ABI·Metal validation·원본 실패를 보존한다. 테스트 종료 후 scoped Wine 프로세스가 멈췄을 때만 disposable prefix를 정리한다. Root captures와 출처가 불명확한 기존 산출물은 소스 커밋에 넣거나 임의로 삭제하지 않는다.

## 역사적 문서로 이동한 내용

<a id="현재-남은-구현-목록--2026-10-05-정리"></a>
<a id="2026-10-03-중간-정리"></a>
과거 P0–P4의 세부 표, #20 중간 보류, runtime033–044 실패/재개와 오래된 ARM64/FEX 보류는 [2026-10-06까지의 원본 우선순위 기록](../docs/history/implementation-priorities-20261006.md)에 보존했다. 해당 기록의 현재형 표현은 당시 상태이며 이 문서의 실행 선택을 덮어쓰지 않는다. [인수인계 원본](../docs/history/README.md)도 같은 원칙을 따른다.
