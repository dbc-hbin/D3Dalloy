# 남은 구현 우선순위

기준일: 2026-10-05. 현재 루트 `src/`의 미구현·부분 구현을 기능 단위로 정리한다. **작업 순서는 이 문서**, 지원 선언·완료 증거는 [현재 구현 상태](CORE-IMPLEMENTATION-STATUS.ko.md), 실행 절차는 [smoke guide](SMOKE-TEST.ko.md)가 기준이다. 이 P0–P4는 [이력](IMPLEMENTATION-HISTORY.ko.md)의 과거 완료한 동명 개발 단계와 다른 새 backlog다.

**현재 완료 상태: 새 P3 #15–24 구현·검증 완료.** 2026-10-03 후속 사용자 지시로 **구형 WaveMMA와 후속 SM6.10 matrix를 모두 구현했으며 나머지 P3도 양 ABI proof로 닫았다**. 아래 중간 정리는 재개 전의 날짜별 관찰이지 현재 중단·#20 보류 지시가 아니다. P0와 지정 P1 계약은 완료했고 P2 #12 depth bounds는 Apple10 native와 Apple7/8/9 emulation을 [forced/native 각각 양 ABI146-case proof](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)로 완료했다. Emulation은 M5 강제 경로 검증이며 구형 물리 GPU proof는 아니다. #13 conservative Tier3도 [양 ABI cap3 public proof·8/8 mixed-cut acceptance](CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof)로 완료했다. #14 Tier3는 새 P3 #24에서 닫았다. 단계 번호는 의존성이나 엄격한 직렬 실행 순서가 아니다. ARM64/FEX는 사용자 보류를 유지하며 자동 재개하지 않는다. D3D11 기능 확장은 하지 않으며 D3D12에 필수인 공유 경로만 변경한다. 미지원 cap, 유효한 optional absence, 오래된 TODO와 실제 결함을 구분한다. Preview는 정식 SDK 목표와 분리하고 각 확장 착수 시 선택한 header/SDK revision을 확인한다.

## 현재 남은 구현 목록 — 2026-10-05 정리

**ARM64/FEX 후속 지시 완료 범위 — 2026-10-06:** 사용자가 실행 계층과 guest→ARM64 Metal end-to-end를 먼저 구현하도록 명시하여 기존 보류를 해제했다. SDK05의 동일 immutable native runtime에서 ARM64·FEX x64·FEX x86 모두 COM·callback·Unix-call·Metal blit/fence/readback과8-frame D3D12 graphics/compute exact readback을 통과했다.6개 실행 모두 exit0·API/Shader Validation0·strict no-Rosetta다. Game/presentation/full DXIL과 broader native matrix는 별도 검증 범위다. 아래 과거 보류·중단 기록을 현행 지시로 사용하지 않으며 [현재 gate와 증거](../docs/ARM64-FEX-IMPLEMENTATION.md)를 따른다.

**순서: DX12 게임 기능의 구현·정확성 검증 → 실제 게임 통합 → 측정한 병목의 최적화.** 아래 목록이 현재 실행 기준이며, 뒤의 날짜별 재개·중단 기록과 과거 runtime 실패를 최신 상태로 읽지 않는다. 일반 HDR, PSO 자동 RAM/disk 캐시, SR/alpha/선정 FG/Reflex, Depth bounds, Conservative Tier3 및 새 P3 #15–24는 남은 기능으로 다시 넣지 않는다. 완료 범위는 CORE의 지정 proof를 따른다.

### 먼저 닫을 공통 게임 기능 3개

| 항목 | 이미 있는 구현·증거 | 남은 구현/완료 조건 |
|---|---|---|
| Composition의 정확한 alpha·색상 합성 | 공용 Presenter/alpha shader, matrix·attachment·resize·lifetime·cadence API proof, 양 ABI 104 backing captures | 독립 Windows 결과 또는 qualified association·transfer·FP16 rounding/clipping·profile 기준을 확보해 absolute colour parity를 닫는다. Backing geometry와 최종 화면 색상/optical scanout은 별도다. DX11 Composition은 미지원이라 대체 구현이 없다. |
| NGX Ray Reconstruction | Feature13→native MetalFX denoised scaler와 양 ABI public 수치 parity | Actual K44/C16 K-tail 범위 밖 읽기의 native 수정 및 clean API+Shader Validation, history/reset/replay/수명·public 양 ABI acceptance. DX11 SR은 RR 대체가 아니다. Native 오류를 숨기거나 guide를 제거해 닫지 않는다. |
| HDR10+ GAMING | 일반 HDR10/scRGB/PQ·EDR, IOAV generic transport 조사, Samsung UE APP/GPU guide와 calibration 표 | Supported Gaming payload/SSTM·native Gaming/LowLatency/SourceTM 제어·readback, output ownership/복구 및 GPU TMO 소비 경로. 실제 Gaming-capable 출력에서 검증한다. ALLM·SBTM·일반 HDR10+ video 또는 내부 HDR 설정으로 대체하지 않는다. |

`DXGI_HDR_METADATA_HDR10PLUS` 동적 metadata는 Gaming SSTM과 별도다. 게임이 실제로 이 API를 요구하면 payload 규약·native 전달·표시 검증을 해당 HDR 계약으로 추가한다. Mandatory sink ACK는 확인된 요구가 아니다. 세 공통 기능의 외부 기준/native 계약은 아직 미해결이다.

### 실제 게임·장치 요구를 확인한 뒤 착수할 7개

| 항목 | 착수 조건·남은 일 |
|---|---|
| Genuine CoreWindow | 선택 게임이 HWND/Composition fallback으로 충족되지 않는 CoreWindow identity/UI 경로를 실제 요구할 때, genuine 객체→실제 native window→swapchain을 검증한다. |
| 필수 Wave64/128 | 실제 shader가 32를 허용하지 않는 wave size를 요구할 때 cross-SIMD lowering·active lane·ballot/shuffle/reduce·동기화 의미를 구현한다. |
| 상위 공개 Shader Model/feature-level admission | 게임 query/실제 operator가 현재 SM6.5/FL12_1을 거부할 때 mandatory cap와 GPU 계약을 재대조한다. 상위 bytecode 수용만으로 공개 버전을 올리지 않는다. |
| 상위 Agility SDK package | 게임이 owned617/618 밖의 정확한 SDK version을 요구할 때 해당 Core/Main generation·ABI·path/version·수명·GPU 실행을 연결한다. |
| DXGI stereo | 게임이 Stereo=TRUE/좌우 eye 표시를 fallback 없이 요구할 때 물리 stereo display와 native eye 배치·timing을 구현한다. |
| Hardware overlay/plane | 실제 hardware plane이 일반 alpha Composition으로 대체되지 않을 때 장치의 format/colorspace·표시/해제 계약을 구현한다. |
| Linked/cross-node·multi-adapter | 게임의 복수 GPU/node 실행·공유가 실제 필요할 때 두 번째 Metal GPU와 resource identity·queue/fence·device-loss 경로부터 검증한다. |

복수 GPU preference 순서와 adapter hotplug/device-loss는 실제 장치 검증 대기이며 새로운 renderer 구현으로 세지 않는다. Queue priority의 허용된 S_FALSE 미적용도 정상 계약이다. 게임이 native GPU scheduling 적용을 실제 요구할 때만 별도 backend 작업으로 전환한다.

### 구현과 구분할 검증·보류·범위 밖 항목

- **검증 확대:** 실제 게임/Streamline loader·장시간 실행·다른 GPU/OS·구형 Apple GPU의 depth-bounds 경로·Windows differential·물리 HDR/scanout. SampleCmp/GatherRaw/u64 gather의 미시험 조합, 비-CS denorm/f32→f16 경계, library GS/HS/DS·mixed-bitness sharing은 proof 부재를 자동 버그로 바꾸지 않는다. NGX IsHDR/perceptual 입력은 독립 색상 기준으로 검토한다.
- **ARM64/FEX 선택 실행 완료:** SDK05의 동일 native runtime에서 ARM64·x64·x86 COM/Unix/Metal 및 각8-frame D3D12 graphics/compute smoke를 실제 실행 PASS로 확인했다. Narrow Wine client-array/NLS/TEB와 owned compiler/Metal CPU-pointer 경계를 수정했고 native handles·GPU addresses·retirement semantics는 유지한다. Broader shader/game/presentation/native regression은 검증 확대이며 이 완료 proof로 대체하지 않는다.
- **현재 자동 착수 범위 밖:** WARP/CPU renderer, 범용 video/protected media/desktop capture, 광범위 D3D11·Wine/도구 API 확장. 실제 DX12 게임 요구와 별도 승인 없이 새 subsystem을 추가하지 않는다.

### 기능 구현 이후 최적화 후보

**현재 착수 지시가 아니라 기능 뒤의 측정 목록이다.** 같은 게임·설정·장면·GPU/OS에서 cold/warm startup, CPU/GPU frame time, p50/p95/p99 frame time·stutter, allocation/copy bytes, peak memory·장시간 pressure, input-to-present 지연과 전력을 먼저 기록한다. 정확성 실패는 최적화보다 먼저 고친다. API 성공이나 synthetic fixture 속도를 실제 게임 FPS 개선으로 바꾸지 않는다.

| 대상 | 측정한 병목일 때만 수행할 일 | 보존할 계약/판정 |
|---|---|---|
| Native PSO 생성·재시작 비용 | 기존 owned RAM/disk·single-flight 뒤에 남는 native library/function/pipeline 생성 시간을 분리한다. 그 비용이 크면 native Metal pipeline/archive 재사용을 검토한다. | 현재 cache/PipelineLibrary/ShaderCacheSession을 재구현하지 않는다. Shader·descriptor·ABI identity, 손상 거부·동시성·cache 용량과 cold/warm GPU 결과를 유지한다. |
| Queue overlap·submission | CPU recording/submit과 실제 GPU workload의 idle gap·queue serialization을 측정하고 불필요한 전역 대기·중복 barrier/flush를 줄인다. | Queue FIFO·fence rewind/threshold·cross-queue dependency와 실제 GPU completion을 보존한다. Commit을 completion으로 취급하지 않는다. |
| Retirement·backpressure | Retained submissions/objects 수와 ring/FIFO 대기·callback/CPU retirement 비용을 측정해 병목을 줄인다. | 제출 소유권·bounded backpressure·완료 순서·device removal·shutdown을 유지한다. Pending GPU owner를 조기 해제하지 않는다. |
| Allocation·copy·temporal 변환 | Frame별 buffer/texture allocation, upload/readback·staging·format/guide conversion의 bytes/time을 측정해 중복 작업을 제거한다. | SR/RR/FG 입력·subrect·alpha·history/reset/replay·device identity·GPU lifetime을 유지한다. 미측정 pool/새 cache는 추가하지 않는다. |
| Sparse mapping·alias flush | Tiled mapping/remap 빈도, logical page 변환과 alias flush·queue 직렬화 비용을 측정해 불필요한 작업을 줄인다. | 64KB 논리 tile, NULL/remap/alias·status/filtering·다른 queue의 visibility를 유지한다. |
| Memory pressure·cache 크기 | 장시간 실행의 live/retired memory와 PSO/shader/temporal cache 성장·native budget·eviction 효과를 측정한다. | 소유권·retirement·OOM/recovery·budget notification을 유지한다. Current cache 용량 정책부터 조정하고 누수를 cache 효과로 숨기지 않는다. |
| Shader/emulation hot path | 실제 게임에 쓰인 depth-bounds/conservative/FP64/virtual-wave 등 경로의 pass 수·GPU 시간을 측정한 뒤 해당 hot path만 개선한다. | 지정 numerical/coverage/depth/stencil/UAV/wave 계약과 native/emulated 회귀를 유지한다. 기능 사용 근거 없이 전체 compiler를 재작성하지 않는다. |
| FG/present pacing·Reflex·전력 | Frame generation·real/UI frame의 spacing, tail latency·stutter와 thermal sustained 성능을 측정한다. | 실제 표시 순서·generated phase·history/UI·tearing/SyncInterval·정직한 clock/statistics를 유지한다. Synthetic Present ID를 FPS/physical scanout으로 해석하지 않는다. |

기존 PSO 생성/재시작 최적화는 완료된 측정 결과다. 이 표의 후보들은 아직 새 성능 baseline이나 개선 효과가 확정되지 않았으며 기능 blocker를 대체하는 자동 다음 작업이 아니다.

### 산출물 정리와 다음 재현

2026-10-05 임시 prefix 148개와 과거 runtime 37개의 bin/lib/share를 제거했다. Build 점유는 247.4→57.5 GiB, 디스크 available은 68.5→258.8 GiB다. 현재 build/install·ARM64 기반 산출물·소스/toolchains·frozen inputs·logs/result/captures를 보존했다. 최신 runtime044와 비교용 `multi-fg-phase-20261004/runtime-34-stable-statistics-clock`는 stage verify PASS다. Runtime034의 WoW64 native PASS를 x64까지 확대하지 않는다. 폐기 runtime의 manifest/provenance는 남아 있으나 실행 payload는 없으므로 새 실행은 현재 소스에서 fresh stage해야 한다. 정확한 목록·50 보호 hash·259 삭제 대상 확인은 [cleanup manifest](../build/local/cleanup-20261005/manifest.json)에 있다. 정리는 기능 acceptance나 새 GPU 재실행이 아니다.

## 최신 병렬 재개와 다음 착수 조건 — 2026-10-05

**DX11 재사용 지시 확인:** 일반 HDR 공용 Presenter/shader/EDR은 DX12에 이미 연결되어 있고 새 HDR metadata 양 ABI public fixture는 PASS다. DX11에는 Composition factory와 RR denoiser가 없어 추가 이식할 구현이 없으며 NVAPI 내부 HDR 설정도 Gaming wire/driver 계약을 제공하지 않는다. [재사용 경로와 새 검증](../.omo/handoffs/parallel-resume2-20261005.md)을 기록했다. Production 변경 없이 일반 HDR 기반을 확인했으며 아래 세 기능의 잔여 조건은 유지한다.

**세 번째 재개 정정:** [최신 native+Unity/Unreal 웹 조사](../.omo/handoffs/parallel-resume2-20261005.md)에서 Composition의 Advanced Color linear domain, RR의 actual K44/C16 unmasked K-tail load, Samsung 공식 UE guide의 APP/GPU·peak calibration을 확보했다. Product source correction은 아직 없다. Composition은 association/rounding/profile가 qualified된 기준, RR은 native kernel/MetalFX 수정, HDR은 supported Gaming payload/macOS driver consumer가 다음 조건이다. Mandatory sink ACK를 요구하거나 ALLM/SBTM/일반 HDR10+ packet을 Gaming SSTM으로 대체하지 않는다. Public engine source는 게임 소비 경로 근거이며 DWM pixel oracle나 native driver 구현을 대신하지 않는다.

새 사용자 지시로 Composition/RR/HDR를 병렬 재개했다. [최신 handoff](../.omo/handoffs/parallel-resume2-20261005.md)가 현재 cut이며 아래 중단 기록은 보존된 snapshot이다. Alpha 양 ABI13phase·104captures backing geometry는 통과했지만 normative parity는 닫지 않았다. Product source 변경 없이 QA cleanup harness만 수정했고 CPU diagnostics95tests를 통과했다. 이전 runtime044의 cadence/일반 Presenter proof는 유지한다.

- **Composition:** 다음은 matched native Windows frozen-stimuli capture 또는 qualified blend transfer/quantization oracle 확보다. Owned output에서 gamma를 골라 normative parity로 세지 않는다. i686 actual-prefix module hash는 teardown 전에 수집하지 못했고 staged hash/load-path proof와 구분한다.
- **RR:** actual K44/C16 specialization과 generic/specialized AIR에서 K-tail mask 없는8byte load `[12672,12680)`를 확인했다. 같은 native validation 오류32회가 남으며 public weights/backend selector는 없다. Vendor-corrected kernel/MetalFX 또는 native kernel/compiler 수정 근거가 필요하다. Actual runtime framework GPU ISA와 hardware transaction proof는 별개이며 padding/guide 제거/validation suppression으로 닫지 않는다.
- **HDR10+ GAMING:** IOAV generic InfoFrame transport와 공식 Samsung guide의 APP/GPU·peak calibration은 확보했다. 다음은 supported Gaming payload·native Gaming/LowLatency/SourceTM control/readback·output owner transaction·GPU TMO consumer 계약 확보다. Mandatory sink ACK는 미입증이라 제약으로 고정하지 않는다. ALLM/일반 HDR10+ packet은 Gaming SSTM이 아니며 현재 link hardware gate와 source/native 계약 부재는 별개다.

세 기능의 production correction/전체 acceptance는 위 외부 조건에 blocked다. 조건부 일곱 항목과 ARM64/FEX 보류는 그대로이며 새로운 trigger 없이 범위를 확장하지 않는다.

## 사용자 요청에 따른 작업 중단 — 2026-10-05

아래는 사용자 요청으로 **실행을 중단했던 시점의 snapshot**이다. 후속 사용자 지시로 재개했으며 이 단락 자체가 신규 착수 지시는 아니다. 마지막 `runtime-36-bound-source-tearing-permission` 양 ABI build/install/stage0과 actual consumer FAIL을 분리한다. 당시 Source cadence는 첫 warmup ordinal3의9 DISJOINT·native seq1–6 time0, mixed x64는 ordinary-visible1/6 phases PASS 뒤 upper.SetOffsetX(18) E_NOTIMPL이었다. 당시 policy candidate와 fixture·원본 실패는 보존한다.

완료한 FG N1–4·양 ABI140 headless·mixed-cut native23/23, fixed-clock strict public, NGX count1 및 Source-only13-phase geometry/lifetime proof는 유지한다. Mixed nonempty FALSE/TRUE·WoW64·normative alpha-colour·Sync0–4 physical cadence, RR/HDR 조건은 미완료다. CoreWindow/Wave64/128/상위 SM/Agility/stereo/overlay/multi-adapter는 조건부 entry criteria일 뿐 구현 완료가 아니다. 상세 frozen ledger는 [이력](IMPLEMENTATION-HISTORY.ko.md#사용자-요청에-따른-작업-중단-snapshot--2026-10-05)을 따른다.

### 재개 진행 — 2026-10-05

**최신 handoff:** [runtime044 병렬 실행 후 사용자 요청 중단](../.omo/handoffs/parallel-implementation-stop-20261005.md)을 먼저 읽는다. A/B/C는 병렬로 시작했지만 lead의 공통 GPU/화면 검증이 오래 걸려 사용자가 문서 갱신 후 중단을 요청했다. 양 ABI frozen cadence12/12·별도 Sync3 3/3·mixed scoped6phase/offsets·일반 smoke/6개 회귀는 검증했으며, normative alpha-colour 전체·panel-level 주장·가려진 mixed screen tail·RR clean validation/public acceptance·HDR 실제 Gaming negotiation/readback/SSTM는 닫지 않았다. 추가 구현/검증은 새 지시 없이 자동 재개하지 않는다. [이전 소유권·계약 handoff](../.omo/handoffs/remaining-implementation-20261005.md)는 기준 입력으로 보존한다.

후속 지시로 Composition부터 재개했으며 파일 소유권이 겹치지 않는 RR·HDR·조건부 조사도 병렬화했다. 완료 판정은 여전히 각 계약의 public 양 ABI·실제 GPU/표시·독립 oracle·native validation·회귀를 요구한다. 준비 조사나 컴파일을 다음 계약의 완료로 세지 않는다.

- Runtime038의 mixed x64/WoW64는 nonempty FALSE/TRUE roots·offset18x12·cached detach를 각각6 phase×2 capture로 통과했다. Opaque hue/geometry와 inspector edge tolerance 범위이며 normative alpha-colour·physical cadence·일반 Presenter 회귀는 남아 있다.
- Exposed autonomous cadence038/039/040은 warmup3 뒤 첫 Sync0 FAIL이다. Zero/tiny duration 후보를 기각했고 transaction-flush041은 build/stage/verify0까지만 확인한 진행 중 후보다.
- RR absent-hit control은 같은 MetalFX buffer 오류32회와 matrix-sensitivity0으로 실패했다. Guide를 버리거나 validation을 숨기는 변경은 채택하지 않는다. HDR fresh genuine EDID 두 경로는 같은 SHA·VSVDB0·external calibration/preset 부재이며 native Gaming 협상/readback/SSTM 조건은 미해결이다.
- [조건부 일곱 항목의 재검토](../build/local/conditional-resume-20261005/findings.md)는 새 selected-game trigger를 확정하지 못했다. 실제 CoreWindow·wide-wave shader·상위 model/SDK 요구와 stereo/plane/복수 GPU 조건이 확인되면 해당 계약만 재개하며, 부재·불필요나 구현 완료로 표시하지 않는다.

실행 증거와 실패 원본은 [현재 CORE의 재개 절](CORE-IMPLEMENTATION-STATUS.ko.md#composition-재개와-병렬-진단--2026-10-05) 및 [이력](IMPLEMENTATION-HISTORY.ko.md#composition-재개와-병렬-진단--2026-10-05)을 따른다.

## P0 — 기존 API 경로의 안전성·계약 완결

**P0-1–4 완료:** 새 `p0-contracts-r5KJ0O/runtime-01`에서 core 공개 fixture x64/WoW64, NGX x64-only, 기존 VRS·sharing 양 ABI GPU consumer를 실행했다. [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#p0-공개-계약-정합)를 따른다. 아래는 닫힌 계약 기록이며 P1도 아래 지정 범위로 완료했다. 현재 착수 대상은 위의 사용자 선택을 따른다. 계획·컴파일만으로 닫은 항목이 아니다.

| ID | 작업 | 현재 소스 계약 | 공개 검증 범위 |
|---|---|---|---|
| P0-1 | DXGI 보호 콘텐츠 알림 API의 안전한 미지원 처리 | Register는 supplied cookie를0으로 지우고 null cookie·null/INVALID_HANDLE_VALUE event는 E_INVALIDARG, 그 외는 DXGI_ERROR_UNSUPPORTED. Assert 제거·Unregister는 안전한 no-op | Caller event의 handle 수명·signaled state·manual/auto reset·budget-cookie 분리. Native 보호 기능이나 protected session 지원을 가장하지 않는다. |
| P0-2 | 정확한 adapter LUID 선택 | 기존 adapter enumeration에서 LUID 양 half가 정확히 일치할 때만 선택 객체 QI. 불일치는 DXGI_ERROR_NOT_FOUND + null output, adapter0 fallback 없음 | 관측 adapter의 IID·IUnknown identity·retained lifetime·D3D12 GetAdapterLuid 및 양 half의 독립 불일치. 단일 GPU preference 정책과 구분한다. |
| P0-3 | Resource1/Resource2/Heap1 공개 계약 | 실제 확장 vtable·GetDesc1 logical desc/정확한 feedback region·unprotected GetProtectedResourceSession의 E_NOINTERFACE + null | 생성·QI·공유 반환 경로와 retained interface·IUnknown identity·descriptor·GPU readback. Null-output E_POINTER는 프로젝트 경계이며 native differential proof가 아니다. |
| P0-4 | Capability 및 NGX discovery 보고 정합 | 선택 header의38 public Feature IDs + 기존 private OPTIONS21, 빠진9 query 구현. OPTIONS10 true/true·ExistingHeaps true는 기존 VRS·heap 실행 경로에 근거. NGX discovery와 Init은 동일 native temporal predicate | Exact size·입력·node/queue·unsupported 경계, x64-only NGX의 Success + AdapterUnsupported(4), 기존 VRS·sharing 양 ABI GPU consumer 회귀. 최신 SDK 전체 enum/conformance 선언이 아니다. |

수정 전 증거는 보존한다. P0-2의 [`remaining-inventory-stYzia/adapter-luid.log`](../build/local/remaining-inventory-stYzia/adapter-luid.log)는 port runtime02에서 `missing_luid_hr=00000000`, expected `887a0002`, output_nonnull=1, returned_matches_request=0이었다. P0-1은 최초 문서 작성 시 정적 finding이었고 후속 [public before-x64 stderr](../build/local/p0-contracts-r5KJ0O/protection-notification/before-x64-02.stderr.log)에서 assert 중단을 실제 관찰했다. CPU 공개 API 증거이며 GPU/게임 실패로 확대하지 않는다. 후속 완료 판정은 [CORE P0 증거](CORE-IMPLEMENTATION-STATUS.ko.md#p0-공개-계약-정합)를 따른다.

### 병렬 작업 기록

- P0-1은 DXGI adapter, P0-2는 DXGI factory로 분리했다.
- P0-3은 resource/heap class·QI·생성/공유 반환을 소유했다.
- P0-4는 feature-query 계약과 NGX discovery를 소유했고 공통 `d3d12_device.cpp` 적용은 단일 편집 소유자가 통합했다.
- 기존 port runtime02의 변경 전 증거와 fresh runtime-01의 변경 후 증거를 분리했다. x64/WoW64 core API·x64-only NGX facade, caller event·COM 수명·실제 texture/buffer readback 및 기존 VRS·sharing consumer를 각각 검증했다.

## P1 — 기존 구조를 재사용하는 제한된 확장

**#5–9·#11 완료, #10 사용자 제외.** 병렬 작업 중단 후 남은 항목은 순차 검증했다. Runtime별 실제 결과와 한계는 [CORE P1](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)를 따른다.

| 순서 | 작업 | 완료 범위·경계 |
|---|---|---|
| 5 | Programmable sample positions | Tier1의 1-pixel 2/4/8 pattern·raster/sample interpolation·full ResolveSubresourceRegion·reset/replay/bundle을 x64/WoW64 검증했다. Custom GetSamplePosition/SamplePos는 공식 undefined이며 Tier2는 미지원이다. |
| 6 | Non-normalized coordinate samplers | OPTIONS17·CreateSampler2·RS1.2 static/table·DXBC/DXIL CS/PS·GatherRaw·null/empty-view·copy/replay를 x64/WoW64 검증했다. 유효 non-array1D/2D one-mip view·LOD0 범위다. |
| 7 | NGX Exposure.Scale — 호환 계약 완료 | 사용자 선택대로 유효한 nondefault를 받아 무시한다. Exposure 곱셈 지원이 아니며 public x64 GPU 출력·closed-list replay와 invalid-input 보존을 검증했다. |
| 8 | NGX MV.Offset.X/Y — 호환 계약 완료 | 유효한 nonzero를 받아 무시한다. MV 값 bias 수식은 추가하지 않았고 low-res·high-res/jittered GPU/replay를 검증했다. 기존 MV texture subrect offset과 별개다. |
| 9 | FG ResizeBuffers1 per-buffer queues | Same-device DIRECT queue retain/rotation·generated/real/UI 선택·count0·rollback·old-frame drain·worker 진행을 public x64 GPU/실제 native drawable로 검증했다. |
| 10 | SR/FG GPU memory usage query — 사용자 제외 | 기존 SR NO_PROVIDER·FG valid-input zero/unreported success 유지. 실제 총량 구현·완료로 세지 않는다. Native GPTK NGX Stats zero-success와 FFX API는 별개다. |
| 11 | GetSharedResourceAdapterLuid | Creator identity·duplicate/named/cross-process query 수명·invalid/forged/stale 오류와 기존 GPU sharing을 x64/WoW64 각21/21 검증했다. Named GPU import-after-last-COM-owner 및 mixed-bitness GPU sharing 범위는 확장하지 않는다. |

## P2 — FL12_2의 남은 핵심 계약

| 순서 | 작업 | 현재 경계와 필요한 구현 |
|---|---|---|
| 12 | Depth bounds — native/emulation 지정 계약 완료 | Apple7+ cap TRUE, Apple10 native fast path, Apple7/8/9 R8Uint coverage·live stored-DSV per-sample compute probe. 기본50+producer36+pixel32+coverage28을 [forced/native 각각 양 ABI146 cases](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)로 닫았다. Forced는 M5 Pro 의미 proof이며 구형 물리 GPU·HLK/게임/FPS 완료로 확대하지 않는다. Apple7 미만 cap FALSE·활성 PSO E_INVALIDARG는 유지한다. |
| 13 | Conservative Rasterization Tier3 — 완료 | 현재 cap3. Degenerate/full-coverage·genuine DXIL/DXBC InnerCoverage를 [runtime03 양 ABI70 rows·18 interactions·16K precision·새 AS/MS cull/DepthClip edges](CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof)로 닫았다. Acceptance8/8은5개 runtime03+3개 retained runtime02 x64 depth-bounds/statistics/multisample의 mixed-cut 증거다. |
| 14 | Tiled Resources Tier3 — #24에서 완료 | #24의64KB logical volume page-table/filtering/LOD/residency/remap/alias/queue order와 양 ABI full76-case Tiled proof로 닫았다. Conservative Tier3도 지정 proof로 완료했으나 depth-bounds emulation의 지정 proof와 전체 필수 cap 재대조는 별개이며 FL12_2를 자동 선언하지 않는다. |

COPY timestamp와 OPTIONS8 unaligned BC는 완료했다. Programmable sample positions는 위 세 가지와 별도의 선택 기능이며 FL12_2의 추가 필수 조건으로 잘못 넣지 않는다. 구현 후 전체 필수 cap을 재대조한 다음 feature level을 변경한다. SM6.6 미보고 자체는 FL12_2의 blocker가 아니다.

## P3 — 상위 shader·GPU 기능

검증된 baseline의 공개 상한은 **SM6.5/FL12_1**, 일반 compiler 입력 상한은 **SM6.9/DXIL1.9**이다. **#18 native vector와 #22 FP64의 double cap TRUE는 paired runtime-04, #23 wave size 4–32는 paired runtime-02의 양 ABI focused acceptance를 완료했다. #20 구형 WaveMMA와 후속 pinned SM6.10 preview matrix도 모두 완료했다.** Matrix는 별도 pinned preview 계약이며 전체 SM6.10 conformance가 아니다. Atomic64를 포함한 이번 P3 선정 계약 완료와 전체 Shader Model mandatory conformance는 별개다. 상위 model의 전체 필수 계약을 대조·검증하기 전에는 공개 model을 올리지 않는다.

| 순서 | 작업 | 완료 계약·검증 경계 |
|---|---|---|
| 15 | 64-bit atomic 전체 계약 — 완료 | Graphics·typed·groupshared·heap·RT/WG 구현과 세 cap TRUE. Runtime30 양 ABI full-SM6·RT·typed·phase·producer·work-graph·pixel·helper20/20 PASS. 원래65536-thread raw stress target1/16 각각20/20·full64 oracle·60초 fence 유지. [현재 증거와 capacity 경계](CORE-IMPLEMENTATION-STATUS.ko.md#atomic64-완료와-전체-계약-proof). |
| 16 | RelaxedFormatCasting / AdvancedTextureOps — 완료 | 두 cap TRUE. 양 ABI positive casting24·advanced24 및 runtime28 volume8·standard8·shared5·full Tiled76의 reserved-volume full64KB copy/RTV/alias/oracle 통과. Programmable offsets는 기존 완료 경로를 재사용한다. |
| 17 | Mesh/AS derivatives — 완료 | OPTIONS9 cap TRUE. Paired runtime-01의 x64/WoW64 Mesh/AS 1D·2D·3D quad·수치·implicit sampling/LOD·invalid-shape 및 기존 mesh/cache proof 완료. [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#mesh-as-derivatives-완료와-focused-proof)를 따른다. 기존 MeshShaderTier1 필수 출력/cull, #20 matrix 및 공개 SM/FL 상향과 별개다. |
| 18 | SM6.9/DXIL1.9 vector 기능 — 완료 | Native vector2–1024·library singleton, elementwise/wave intrinsic·raw/structured/heap load/store·bounds/status·reduction·SSA/byte-layout AIR legalization. Runtime-04 양 ABI32 case와 CPU28 boundary/lowering checks 완료. [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#native-vector-완료와-focused-proof)를 따른다. 입력6.9와 공개보고6.5는 별개다. |
| 19 | SER / HitObject / opacity micromap — 완료 | Runtime-06 양 ABI Tier1.2·final OPTIONS22·RTAS0 admission·OMM modern56,206+legacy3,604 및 HitObject280+multi-wave REORDER_SCOPE512 exact words와 full DXR를 API+Shader Validation으로 검증했다. ActuallyReorders=FALSE인 conforming no-op scheduling이며 성능상 SER 가속이나 별도 #15 atomic RT를 주장하지 않는다. [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#hitobject-완료와-focused-proof)를 따른다. |
| 20 | WaveMMA / matrix 연산 — 두 규약 완료 | Retired WaveMMA와 pinned successor SM6.10 preview matrix compiler/runtime/public ABI. Runtime-05 양 ABI runtime behavioral +runtime-06 양 ABI11-case numerical/converted-weight shader +runtime-07 양 ABI strict query38/77/78·OPTIONS9·conversion readback의 mixed-cut proof. WaveMMATier10/LinearAlgebraTier16을 보고하며 [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#wavemmalinear-algebra-최종-positive-cap-proof)를 따른다. HighestSM6.5 유지·전체 SM6.10 conformance 아님. |
| 21 | Writable MSAA — 완료 | OPTIONS14 `WriteableMSAATexturesSupported=TRUE`. Paired runtime-06의 x64/WoW64 각80 case에서 2/4/8 sample별 write·alias/load·copy/raster/MIN/MAX resolve·clear·bounds/null·typed/typeless bit 계약을 API+Shader Validation으로 검증했다. [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#writable-msaa-완료와-focused-proof)를 따른다. |
| 22 | FP64 — 완료 | Binary64 정수 emulation과 `DoublePrecisionFloatShaderOps=TRUE`. Runtime-04에서 DXIL·genuine FXC DXBC5.0/5.1 compute·VS/PS·4-lane wave8 case가 각각 x64/WoW64 API+Shader Validation PASS. [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#fp64-완료와-focused-proof)를 따른다. 공개 SM/FL 상향·성능 선언과 별개다. |
| 23 | Wave32 외 wave size — 완료 | 공개 min4/max32·기본32, native SIMD32 내 virtual4/8/16. Runtime-02에서 waves24 case·NodeWave4 포함 work-graphs9 case가 각각 x64/WoW64 PASS. 필수64/128은 명시적 실패이며, preferred64는 허용 range에32가 있을 때만32를 선택한다. [CORE의 정확한 증거·한계](CORE-IMPLEMENTATION-STATUS.ko.md#wave-size-확장-완료와-focused-proof)를 따른다. |
| 24 | Tiled Tier4와 필수 Tier3 — 완료 | Native placement-sparse gate의 Tier4 보고. Runtime28 양 ABI76 cases·API+Shader Validation·owned compiler/backend·오류0. DXIL/DXBC volume filtering/status, BC5/BC6H signed/plain/native-decode·full64KB aliases·NULL/remap·RTV/copy/predication·cross-queue와 packed-array tail을 검증했다. [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#tiled-tier34-완료와-full-proof). |

새 P3는 완료했으며 다음 작업을 자동 착수하지 않는다. P2 잔여 계약과 ARM64/FEX 보류는 위의 현행 상태를 따른다.

### WaveMMA / matrix #20 완료 — 2026-10-03

구형·후속 두 계약을 구현하고 public experimental admission·runtime 변환/수명/ordering·실제 producer shader numerical consumer·최종 positive-cap를 연결했다. [Runtime-05 behavioral proof](CORE-IMPLEMENTATION-STATUS.ko.md#matrix-runtime-admission과-pinned-v1-focused-proof), [runtime-06 paired11 shader proof](CORE-IMPLEMENTATION-STATUS.ko.md#wavemma와-linear-algebra-shader-consumer-proof), [runtime-07 paired archived/preview final-cap proof](CORE-IMPLEMENTATION-STATUS.ko.md#wavemmalinear-algebra-최종-positive-cap-proof)를 결합한다. Final cap은 WaveMMATier1.0(10)/LinearAlgebraTier1.0(16)이며 unchanged full suites를 최신 단일 cut에서 다시 실행하지 않았다. 아래 중간 정리의 #20 보류/미구현은 당시 기록이다. Mandatory atomic이 남아 HighestSM6.5는 유지하고 pinned SM6.10 matrix를 전체6.10 지원·다른 GPU/OS·게임/성능·ARM64/FEX/no-Rosetta proof로 확대하지 않는다.

### FP64 #22 완료 — 2026-10-03

Paired `p3-20261003/runtime-04`의 [x64](../build/local/p3-20261003/fp64/public-runtime04-x64/result.json)·[WoW64](../build/local/p3-20261003/fp64/public-runtime04-x86/result.json)는 ABI별8 case, `double=1`, exit0, API+Shader Validation·owned native compiler 확인·validation/foreign backend 오류0을 기록했다. Double→i64 representation과 정확한 arithmetic/conversion helper, DXBC packed register lowering, VS/PS bit varying 및 numeric/bit wave 계약을 실제 public consumer로 닫았다. 아래 중간 정리의 helper ABI 실패·미통합 기록은 당시 관찰이며 최신 완료를 취소하지 않는다. 비용·다른 GPU/OS·전체 SM/FL conformance는 이 proof로 주장하지 않는다.

### Wave size #23 완료 — 2026-10-03

Paired `p3-20261003/runtime-02`의 [waves x64](../build/local/p3-20261003/waves/gpu-x64-02/result.json)·[waves WoW64](../build/local/p3-20261003/waves/gpu-x86-02/result.json)는 ABI별24 case, [work-graphs x64](../build/local/p3-20261003/waves/node-gpu-x64-02/result.json)·[work-graphs WoW64](../build/local/p3-20261003/waves/node-gpu-x86-02/result.json)는 NodeWave4를 포함한 ABI별9 case를 통과했다. 네 실행 모두 exit0·API+Shader Validation·`validation_errors=[]`·owned native compiler 확인이며, 전용 prefix는 종료·제거했다. 아래 중간 정리의 wave32 제한·미통합 기록은 해당 시점의 관찰로 보존한다. 이 완료는 cross-SIMD64/128·전체 shader-model/feature-level·다른 P3 완료 선언이 아니다.

### 2026-10-03 중간 정리

**재개 전 중간 정리 시점의 관찰:** 당시 사용자 요청으로 신규 구현 확장을 멈췄으며 아래는 그 시점의 증거·미해결 경계다. 이후 위의 후속 사용자 지시로 두 matrix 규약과 남은 P3 구현을 재개했다. 위 표의 기존 제한은 마지막 검증 baseline이며, 미검증 작업 트리의 일부 변경을 완료 선언으로 읽지 않는다. **이 중간 정리 시점에는 작업 트리의 통합 빌드·실행 가능 상태가 검증되지 않았다.**

- **#17:** 기존 derivative lowering과 새 fixture로 x64 Mesh/AS 1D·2D·3D quad, sample/LOD, invalid-shape 및 기존 mesh suite를 실제 통과했다. [result.json](../build/local/p3-20261003/mesh/before-validation03/result.json)은 API+Shader Validation·exit0·validation 오류0이다. 공개 cap·정식 runner 통합과 최종 양 ABI 실행은 남아 있다.
- **#16/#18/#19/#21/#22/#23:** 구현·fixture 변경이 작업 트리에 있으나 공통 sampler/descriptor/compiler ABI, device/runner 연결과 전체 빌드·새 runtime GPU acceptance가 남아 있다. 독립 CPU·native Metal·DXC producer 검증을 번역기 end-to-end 성공으로 합산하지 않는다. FP64 native helper 실행은 출력 미기록으로 실패했고 helper 정수 ABI 수정이 필요하다.
- **#15:** returning atomic64의 graphics forward progress와 typed/groupshared 상호 atomicity를 보장할 구현이 없다. 실제 native 및 pristine GPTK probe에서도 해결 근거를 얻지 못했으며 기존 cap·거부를 유지한다.
- **#24:** packed-array tail의 slice별 처리와 fixture만 변경했다. 새 fixture 입력 연결·GPU 실행 및 Tier3 logical-volume compiler/runtime 전체 경로가 남아 있어 Tier4 완료가 아니다.
- **#20 중간 정리 당시:** 구형·후속 matrix 모두 사용자 보류 상태였다. 후속 지시로 두 규약 모두 재개했으며, 당시 [조사 결과](../build/local/p3-20261003/matrix/research-result.json)는 public producer·서로 다른 ABI·native 가능성과 그 시점의 미구현 경계를 구분한다.

HLSL/DXIL 규약과 Metal backend 세대는 별개다. Matrix 가능성 probe는 기존 MTLCommandQueue와 simdgroup_matrix/simd_shuffle을 사용했다. Metal4 사용 자체가 D3D 의미나 지원 완료를 보장하지 않으며, 이번 결과로 이전 Metal/OS/GPU의 최소 지원 버전을 선언하지 않는다.

## P4 — DX12 게임 실행에 필요한 추가 계약

**2026-10-04 사용자 범위 정정:** P4는 일반 Windows 그래픽 API 완전성 목록이 아니다. DX12 게임의 렌더링·표시·업스케일링·프레임 생성·지연 제어에 쓰이는 항목을 남긴다. 최소 실행 조건뿐 아니라 게임의 선택적 화질·출력·성능 기능도 포함한다. 드물거나 현재 native 대응이 없다는 이유로 게임 기능을 불필요로 분류하지 않는다. 반대로 API가 없거나 capability가 FALSE라는 이유만으로 작업을 추가하지 않는다. 별도 subsystem 확장은 구체적인 게임 요구와 사용자 승인을 따른다. 작업 트리의 새 구현은 통합 실행 증거 없이 완료로 표시하지 않는다.

**상태 구분:** “소스 변경”은 지원 완료가 아니다. 후속 [runtime02 x64](../build/local/p4-resume-20261004-WoVvSo/options13-runtime02-x64/result.json)·[WoW64](../build/local/p4-resume-20261004-WoVvSo/options13-runtime02-x86/result.json)는 OPTIONS13 전체 9 case를 통과했고, 6개 OPTIONS13 cap과 triangle fan cap은 TRUE다. 초기 fan oracle의 shader ID/버퍼 주소 혼동, COPY fixture 상태, WoW64에서 드러난 PSO 실패 출력의 미초기화 읽기를 수정했다. 나머지 P4는 아래 항목별 증거와 미해결 조건으로 판단하며 전체 완료로 합산하지 않는다.

### A. 현재 게임 렌더링·업스케일링 경로 완결

**2026-10-05 multi-frame 정정:** Owned AMD FSR3 N1–4의 genuine phase/CPU/headless·mixed-cut 양 ABI physical proof로 선정 FG backend 계약을 닫았다. Root28 private96·Root30 NGX count1 지정 native proof와 Root34 public strict clock/count proof는 각각의 cut/consumer로 구분한다. Root30 WoW64 saved-capture offline 재분석과 Root34 x64 count/index(1,1) 미측정의 original false를 보존하며 새 GPU 재실행·같은 Root34 전체 양 ABI native PASS로 바꾸지 않는다. `PresentCount`19/UI4는 마지막 실제 표시 Present ID이지19/4개 표시의 합계가 아니다. Composition Sync0 event·mixed ordinary/HWND native 및 alpha-colour parity/cadence는 계속 진행 중이다. [현재 CORE](CORE-IMPLEMENTATION-STATUS.ko.md#multi-frame-fg와-native-residency-현재-경계)와 [ledger](IMPLEMENTATION-HISTORY.ko.md#multi-frame-fg와-native-borrowed-residency--2026-10-05)를 따르며 전체 실게임 기능 완료로 확대하지 않는다.

| 기능 | 게임에서 쓰는 경로·현재 상태 | 구현 및 완료 검증 |
|---|---|---|
| OPTIONS13 선택 계약 6종 | **지정 양 ABI acceptance 완료·cap TRUE.** Texel copy pitch/offset, vertex alignment, inverted viewport Y/Z, 차원 간 copy, constant-alpha blending | Runtime02: 63 copy와 6 차원 간 copy의 범위·guard·replay, 24개 bounds 거부, raster 수치와 cap/구조체 크기 검증. 잘못된 alignment layout은 예외 대신 E_INVALIDARG·null PSO를 반환한다. |
| Triangle fan | **지정 양 ABI acceptance 완료·cap TRUE.** Direct/indexed/indirect/bundle 조립 | 18 row·1152 pixel·66 primitive의 독립 CPU oracle: provoking vertex·winding·restart·degenerate·GS·occlusion/statistics·replay 통과. 다른 GPU/전체 conformance 선언은 아니다. |
| Alpha upscaling | **지정 양 ABI acceptance 완료.** 별도 temporal alpha 재구성과 공개 NGX flag128 | Private7-case 수치/수명 proof 및 공개 release-before-execute/replay RGB112/96/80 오차0·좌우 alpha48/192를 별도로 검증했다. Native output alpha 보존만으로 대체하지 않으며 공개 RGBA8 fixture의 bounded 영역과 private HDR/동적 입력 proof를 구분한다. |
| NGX Frame Generation | **Root30 요청 count1의 지정 양 ABI public native proof 완료.** NGX feature11 → OutputInterpolated → caller-owned flip swapchain | 양 ABI consumer exit0·full Metal API/Shader Validation·foreign0, formal drawable9개/generated0.5 frames2/3/4+REAL1–6 full RAW/positive times와 copy-list release/replay/history/reset/UI를 확인했다. x64 original result PASS, WoW64 original false는 bootstrap equal-clock parser 교정 뒤 saved-capture offline PASS이며 원본을 보존했다. 새 GPU 재실행·다른 count·**FSR FG 전체 proof**·Streamline 실게임·NVIDIA 모델 동등성은 아니다. |
| NGX Ray Reconstruction | **양 ABI public 수치 parity 완료, clean native validation 미해결.** 실제 feature13→MetalFX denoised scaler. M5 Pro/macOS27.0.1(26A434)의 native support1은 미지원 하드웨어가 아니라 미해결 native 실행 조건이다. | 각 ABI output36장 finite RGB 오차0·producer input16세트 SHA256 일치. Classic YES arm64/x86_64 및 새로 빌드·실행한 [classic NO x86_64](../build/local/rr-classic-async-20261004.JXBn3w/validation-verdict.txt)·[M4 NO compiler-factory x86_64](../build/local/rr-m4-async-20261004.yycbQA/validation-verdict.txt)는 모두 baseline32/replay18회 Conv3x3Stride1U8 범위 밖 읽기로 FAIL이다. Finite/non-copy·matrix-sensitive7·replay history만으로 완료하지 않는다. Pristine GPTK factory에 맞춘 owned control의 실패이지 GPTK runtime 실행 판정이 아니다. Native 오류 없는 실제 feature13과 public 양 ABI acceptance를 닫아야 하며 validation 억제·SR/fake fallback·production cutover로 대신하지 않는다. |
| FG 다중 generated outputs | **Owned N1–4 genuine phase·CPU/headless·mixed-cut 양 ABI native의 선정 backend proof 완료.** MetalFX FG/출력 복제가 아님 | Root34 public consumers 양 ABI exit0·통계의 마지막 실제 표시 Present ID19/UI4는 표시 합계가 아니다. WoW64 native23 exact/positive times PASS, x64는18 positive callback+4 UI=22 timed·seq2 skipped/count(1,1) unexercised·original false다. x64 Root31+WoW64 Root34의 physical proof를 같은 Root34 전체 양 ABI native PASS·Composition·실게임 전체 완료로 합산하지 않는다. |
| Reflex | **지정 양 ABI public consumer 완료.** 8 case·90 frame·최신 64 report, 실제 Sleep 약31ms | Device 격리·retirement 대기·present·removal 깨우기 검증. Native GPU active-cycle은 측정하지 않아 0이며 CPU/QPC나 elapsed GPU span으로 만들어내지 않는다. Boost/flash 지원은 주장하지 않는다. |
| x64/WoW64 public facade | **기존 SR/alpha/Reflex·Root30 NGX count1·Root34 FSR 양 ABI public clock/count proof 유지.** 같은 Root34 양 ABI 전체 native PASS는 아님 | Root30 WoW64는 saved GPU captures의 offline 재분석으로 PASS이며 원래 failed result를 보존한다. Root34 FSR public consumer exit0과 x64 native runner false를 분리하며 physical phase는 x64 Root31+WoW64 Root34 mixed-cut이다. RR native validation·Composition 잔여 acceptance와 ARM64/FEX 보류는 유지한다. |
| 게임 표시·pacing | **지정 양 ABI consumer 완료.** Fullscreen output statistics·native vblank, factory occlusion/event 수명 | D3D12 fullscreen 3888 pixel readback·실제 present/refresh counter·QPC·resize disjoint·fullscreen 해제 검증. 물리 scanout/HDR 측정이나 desktop capture 완료를 뜻하지 않는다. |
| 실제 GPU preference 선택 | 단일 M5 Pro의 양 ABI enumeration/LUID/device identity·lifetime 검증 완료. 실제 저전력/고성능 다중 GPU 순서는 **장치 조건 대기** | 복수 Metal GPU 장치에서 실제 preference 순서를 확인한다. 단일 GPU 결과나 순서를 바꾸는 가짜 adapter로 다중 GPU proof를 주장하지 않는다. |

### B. 복원한 게임 표시·런타임 기능

| 기능 | 게임 사용 근거·현재 경계 | 구현 계획·완료 조건 |
|---|---|---|
| **HDR10+ GAMING — 선행 계약 미해결** | [공식 게임 목록](https://hdr10plus.org/games/)의 The First Descendant 등 실제 게임 기능. NVAPI Gaming SSTM은 일반 HDR10+ sink metadata와 별도다. [실제 native/EDID 계약 보고서](../build/local/hdr-gaming-20261004-native-readonly/contract-findings.json)의 Samsung S32B80P(CGDisplayID3/SAM29171/serial810300994) 현재256-byte EDID는 PQ·정적 HDR(EOTF05), VSVDB0x90848B 없음이다. 외부 preset0/calibration null 및 내부1000-nit HDR Video profile은 Gaming/저지연 proof가 아니며 모든 firmware/input mode의 미지원으로 확대하지 않는다. | Native Gaming reference-mode/저지연 협상과 readback, 규범적 transport/control·calibration·application-version 의미·peak/full-frame 휘도 table·GPU SSTM/EETF와 conformance vectors를 확보해야 한다. APP/GAME SSTM은 passthrough에서 EETF를 중복 적용하지 않으며 GPU 선택은 실제 calibrated sink 한계로 별도 규범적 tone map을 수행해야 한다. Gaming capability·reference/latency/calibration readback을 제공하는 acceptance sink에서 공개 NVAPI 양 ABI consumer, 승인된 물리 전환·경합 소유권·실제 이전 mode 반환/복원, 광학 휘도/색·지연을 검증한다. Local 상태나 PQ layer tag·일반 HDR10 성공·pristine no-op으로 완료를 꾸미지 않는다. |
| **CoreWindow swapchain — 게임별 조건부 미완료** | [공식 Store/UWP 표시 경로](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcorewindow). Translator는 genuine ICoreWindow·ICoreWindowInterop QI와 실제 get_WindowHandle HWND를 요구하며 잘못된 객체는 DXGI_ERROR_INVALID_CALL이다. Desktop thread의 GetForCurrentThread **S_OK/null은 정상 계약**이며 genuine CoreApplication UI thread의 객체 생성 proof가 아니다. | **착수 trigger:** 선택 게임이 실제 CreateSwapChainForCoreWindow를 호출하고 HWND/Composition fallback으로 충족할 수 없는 객체 identity·foreground/UI 요구를 가진 경우. 승인된 별도 `toolchains/wine-game-display` clone에서 필요한 CoreApplication UI-thread 경로를 구현한 뒤 attachment·GetCoreWindow identity·resize·present·수명을 검증한다. 기존 Wine worktree는 보존하며 현재 HWND를 가짜 CoreWindow로 activation하거나 hidden HWND로 성공 처리하지 않는다. 요청된 공통 기능 뒤 조건부 착수이며 Godot DComposition 사용만으로 CoreWindow 요구를 추정하지 않는다. |
| **Composition swapchain·matrix transform** | [Godot D3D12 renderer PR #99387](https://github.com/godotengine/godot/pull/99387)는 `CreateSwapChainForComposition`과 DirectComposition을 실제 사용. 원래 Wine baseline의 DComposition device/visual 생성은 `E_NOTIMPL` | 사용자 승인된 별도 Wine clone에서 게임에 필요한 device/target/visual attachment와 swapchain을 함께 연결하고 alpha 합성·scale/translation·commit·resize·replay·파괴 순서를 실제 화면으로 검증. Matrix는 해당 공개 계약의 scale/translation 범위이며 임의 skew/rotation을 지원한다고 하지 않는다. 승인·소스 구현만으로 표시 acceptance를 완료했다고 하지 않는다. |
| **Agility SDK·device-factory 런타임 계약** | [Godot의 실제 SDKConfiguration1 호출](https://github.com/godotengine/godot/blob/e7cfa294a0b81bed7986be04a848cc1832a3f083/drivers/d3d12/rendering_context_driver_d3d12.cpp)의 **owned SDK618 선택 계약 완료**. Runtime08 양 ABI public consumer와 runtime07 installed-factory GPU proof를 확보했다. | 실제 native Core의 version·EXE-relative path·동일 generation/ABI/image, 독립 factory/config/device snapshot, Set/Apply/Initialize·EXE exports, direct Core entry 및 동시 FreeUnusedSDKs/최종 unload를 검증했다. Version/path 무시, foreign Core 실행이나 fake success가 아니다. SDK618 전체 기능 conformance·다른 SDK package·Godot 실게임 실행까지 완료한 것은 아니다([증거](IMPLEMENTATION-HISTORY.ko.md#게임용-owned-core618-loader-완료--2026-10-04)). |

HDR10+ GAMING/SSTM과 `DXGI_HDR_METADATA_HDR10PLUS`의 72-byte opaque payload는 같은 API가 아니다. NVAPI는 두 capability를 별도로 정의한다. CAEDRMetadata의 정적 HDR10/HLG만으로 일반 HDR10+ 동적 metadata 전달을 완료했다고 주장하지 않으며, **그 제약만으로 게임용 SSTM 구현까지 불가능하다고 단정하지 않는다.** 실제 게임이 DXGI payload 경로를 요구하면 payload 규약·native 전달·표시 검증을 별도 확인한다.

### C. 게임·장치 조건에 따라 적용할 선택 계약

아래는 “게임에 불필요”로 삭제하지 않는다. 다만 API 존재만으로 새 renderer/OS 기능을 자동 구현하지도 않는다. 구체적인 게임/engine consumer와 장치 조건을 확인한 뒤 해당 계약에 필요한 최소 경로를 구현·검증한다.

2026-10-04 조건 검토에서 현재 선정된 ZZZ/Godot 증거에는 genuine CoreWindow·필수 Wave64/128·상위 공개 SM/SDK·stereo·hardware-plane·linked/cross-node 실행 요구가 확인되지 않았다. 이는 **선정 게임 근거 미확인**이지 불필요·사용하지 않음·사용자 제외의 증거가 아니다. 아래 확장은 조건부 미구현이며 B의 CoreWindow도 조건부 미완료다. Godot의 실제 Composition FLIP_DISCARD·DComposition은 별도 승인된 진행 중 경로이고 [runtime03 x64 public consumer](../build/local/composition-20261004.vn2CH7/composition-x64-03/stdout.log)의 초기 Commit·alpha·matrix case는 통과했지만 전체 양 ABI·실제 화면 acceptance는 남아 있으므로 완료로 세지 않는다. Godot SDKConfiguration1의 SDK618 요청은 이미 완료한 loader 계약이며 CoreWindow나 상위 SM 필요성의 증거가 아니다.

| 기능 | 현재 경계 | 조건부 구현·검증 계획 |
|---|---|---|
| Queue process/global priority | **양 ABI 지정 consumer 완료.** Setters는 규격상 허용된 S_FALSE 미적용, getters는 creation priority. Native GPU priority 적용은 주장하지 않음 | 4-thread setter/getter와 실제 native copy 통과. 게임이 실제 scheduler 적용을 요구하면 대응 backend를 별도 확보하며 CPU QoS로 GPU priority를 가장하지 않는다. |
| Factory7 adapter-change | 실제 native adapter observer 연결. 양 ABI IsCurrent·event/cookie·unregister lifetime 통과, 실제 GPU 제거/전환은 **removable GPU 조건 대기** | 실제 removable GPU에서 device-loss/re-enumeration·변경 통지를 검증한다. 현재 단일 M5 Pro에 가짜 hotplug 이벤트를 넣어 완료하지 않는다. |
| 필수 Wave64/128 | WaveOps TRUE·WaveLaneCountMin=4/Max=32. 필수 WaveSize(64/128)와 64–128-only range는 E_FAIL·null PSO이며 matrix는32에 고정된다. Preferred64도32를 허용하는 range에서만32로 실행한다. | **Trigger:** 실제 게임 DXIL의 fixed-width64/128 또는32를 허용하지 않는 range. **Proof/의존성:** compiler lowering과 Metal cross-SIMD64/128의 ballot·shuffle·reduce·active-lane·동기화 의미를 해당 shader 및 경계 workload로 검증한 뒤 cap을 연결한다. TotalLaneCount나 cap만 늘리지 않는다. 요청된 공통 기능 뒤 착수하며 상위 SM/SDK loader와 독립이다. |
| 상위 공개 Shader Model | Compiler 입력 상한 SM6.9/DXIL1.9와 공개 HighestShaderModel6.5/FL12_1은 별개. 지정 atomic64·FP64·matrix proof는 버전 전체 mandatory conformance가 아니다. | **Trigger:** 게임의 D3D12_FEATURE_SHADER_MODEL query가6.5를 거부하거나 실제 shader가 상위 모델의 미충족 operator를 사용. **Proof/의존성:** 정확한 게임 사용 operator의 compiler→GPU 결과와 해당 모델의 mandatory/optional feature table·각 reported cap 실행 계약을 재대조한 뒤 공개 상한을 변경한다. 입력 header 수용만으로 상향하지 않는다. 요청된 공통 기능 뒤 착수하며 필수 Wave64/128 proof를 대신하지 않는다. |
| 상위 Agility SDK package | Godot SDKConfiguration1의 owned618 선택과 SDK617/618 global inbox loader는 완료. 없는619는 거부하며 다른 SDK package·foreign Core·Microsoft SDK618 전체 conformance는 미완료다. | **Trigger:** 실제 게임 EXE의 D3D12SDKVersion/Path 또는 SetSDKVersion/CreateDeviceFactory가618보다 높은 정확한 SDK를 요청. **Proof/의존성:** 요청 version의 owned native-only 일반 PE Core와 Main의 generation·ABI/image·공개 entry 계약, path/version 선택·격리·수명·GPU 실행을 검증한다.618로 무시하거나 foreign Core를 실행하지 않는다. 요청된 공통 기능 뒤 별도 Core 계약으로 착수하며 SM 상향과 혼동하지 않는다. |
| DXGI stereo 표시 | IsWindowedStereoEnabled FALSE·mode/desc Stereo FALSE. Stereo swapchain 및 Present1 stereo flags는 미지원이며 알림의 no-transition 계약은 실제 stereo 출력이 아니다. | **Trigger:** 게임의 Stereo=TRUE swapchain 또는 좌우 eye present가 fallback 없이 필요. **Proof/의존성:** stereo를 실제 출력할 물리 display와 native 표시/합성 backend에서 eye 배치·resize·present timing을 검증한 뒤 cap을 연결한다. 요청된 공통 Composition/표시 기능 뒤 장치 조건에 따라 착수한다. DXGI stereo를 OpenXR/VR 전체 구현과 동일시하지 않는다. |
| Overlay/hardware composition | SupportsOverlays FALSE·CheckOverlaySupport/CheckOverlayColorSpaceSupport flags=0/S_OK. 일반 alpha Composition은 hardware plane 보장이 아니다. | **Trigger:** 게임의 overlay capability query·foreground UI 요구에서 alpha fallback이 불충분하고 실제 hardware plane을 요구. **Proof/의존성:** 해당 물리 display의 실제 native plane/compositor·format/colorspace·표시/해제 계약을 검증한 범위만 보고한다. 요청된 공통 Composition/표시 기능 뒤 착수한다. Optional absence로 정상 동작하면 plane을 강제하지 않지만 모든 게임에 불필요하다고 일반화하지 않는다. |
| Linked-node/cross-node·multi-adapter | GetNodeCount=1·CrossNodeSharingTier NOT_SUPPORTED. CrossAdapterRowMajorTextureSupported TRUE는 multi-node 실행 proof가 아니다. 별도 GPU 선택·cross-adapter 공유·linked-node 실행은 서로 다르다. | **Trigger:** 게임의 복수 adapter/LUID 선택·node0 밖을 선택하는 mask 또는 node/queue/heap/share/fence 호출로 복수 GPU 실행/공유가 실제 필요. Mask1은 기존 node0 선택이므로 그 값만으로 trigger가 되지 않는다. **Proof/의존성:** 실제 두 번째 Metal GPU와 선택 backend부터 확보하고 요구된 경로의 자원 identity·이동/공유·queue/fence 동기화·device loss·GPU readback을 검증한다. 요청된 공통 기능 뒤 장치 조건에 따라 착수하며 preference 순서 proof와 구분한다. 가짜 node 증가 금지. 새 multi-GPU backend 확장은 별도 범위 승인을 따른다. |

### 착수 순서와 제외 경계

**2026-10-04 최신 사용자 선택: DX12 게임에 쓰는 기능을 먼저 구현·검증하고, 그다음 최적화한다.** 게임 공통 경로부터 한 항목씩 닫되, 이미 완료한 PSO 캐시 뒤에 추가 성능 작업을 이어 붙이지 않는다. Depth bounds는 native 지원 장치에서 완료했고 Conservative Tier3도 양 ABI cap3 proof와8/8 mixed-cut acceptance로 완료했다. 다음 실제 단계는 확인된 Godot Composition이며 genuine CoreWindow는 별도 게임별 조건이다. 요청된 잔여 공통 기능을 조건부 항목으로 대체하거나 조용히 건너뛰지 않는다. 게임별 사용률을 측정한 순위는 아니며, 선택 기능을 대부분 게임의 필수 조건으로 주장하지 않는다.

| 순서 | 대상 | 착수·완료 기준 |
|---|---|---|
| 1 — 완료 | **Agility SDK 게임 진입 경로** | [Owned618 SxS와 global inbox 우선 정책의 양 ABI acceptance](IMPLEMENTATION-HISTORY.ko.md#게임-공통-agility-inbox-우선-정책--2026-10-04)를 통과했다. SDK617/618 EXE·Set 요청은 설치된618을 선택하고, 없는619 요청은 실패한다. 실제 Core path/version·설정 격리·수명·동시 해제·GPU readback과 기본 factory 회귀를 검증했다. 모든 SDK package·실게임 완료로 확대하지 않는다. |
| 완료 이력 — 후속 최적화는 기능 뒤 | **공통 PSO 생성·재시작 비용** | [기존 owned 결과의 자동 RAM/disk·single-flight와 양 ABI GPU/재시작 proof](CORE-IMPLEMENTATION-STATUS.ko.md#자동-pso-컴파일-캐시와-focused-proof)를 완료했다. 보관된 ZZZ compute 입력의 반복 생성 중앙값은 x64 220.10→2.81ms, WoW64 217.38→2.88ms. Native archive는 새로 만들지 않았으며 추가 native 생성이 병목일 때만 검토한다. 모든 shader variant·실게임 FPS 완료로 확대하지 않는다. |
| 3 — 지정 계약 완료 | **Depth bounds·Conservative Tier3** | Depth bounds의 Apple10 native·Apple7/8/9 emulation과 forced/native 각각 양 ABI146 cases는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)를 따른다. Conservative Tier3도 [양 ABI cap3 public proof·8/8 mixed-cut acceptance](CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof)로 완료했다. 이8회는5개 runtime03+3개 retained runtime02 x64 회귀이며 동일 build 실행이 아니다. 구형 물리 Apple GPU의 depth-bounds 실행 proof는 별개다. 현재 ZZZ/Unity에서 두 cap이 필수라는 증거는 없으나 게임 무관성으로 제외하지 않는다. 정상 fallback은 결함이 아니며 전체 필수 cap 재대조 전 SM6.5/FL12_1을 유지한다. |
| 4 — 사용자 중단·미완료 | **Composition와 기존 일반 Presenter 회귀** | Root33 양 ABI·양 alpha geometry/lifetime13 phases와 Root34 warmup3 positive events는 유지한다. Root35와 마지막 Root36 actual first-warmup FAIL을 보존한다. Root36 actual-bound validated tearing permission candidate는 acceptance를 실패했다. Mixed x64는 ordinary-visible1/6 phases만 GREEN20480pixels 두 captures PASS 뒤 SetOffsetX(18) E_NOTIMPL, WoW64 미실행이다. FALSE/TRUE root pixels·normative alpha-colour·physical cadence·일반 Presenter 회귀는 미완료이며 재probe/fixture 우회 없이 동결했다. |
| 5 — FG 선정계약 proof 완료·RR/HDR 미해결 | **다중 FG proof 보존·RR·HDR10+ GAMING** | Genuine N1–4 phase·CPU/headless·mixed-cut 양 ABI native로 FG backend 계약은 닫았다. Root34 public 마지막 실제 표시 Present ID19/UI4와 x64 seq2 skipped/original false를 별도 ledger로 보존한다. 같은 Root34 전체 양 ABI native PASS를 주장하거나 닫힌 backend 구현을 다시 TODO로 세지 않는다. RR native validation과 HDR10+ GAMING의 display/VSVDB·SSTM·출력은 미해결이며 Composition Sync0/mixed-HWND는 앞의 진행 중 표시 track이다. 전체 실게임 완료·기능 제외로 확대하지 않는다. |
| 6 — 공통 기능 뒤 게임별 조건부 | **CoreWindow·필수 Wave64/128·상위 SM/SDK·stereo/overlay·multi-adapter** | B/C의 정확한 게임 호출 trigger와 native/backend/물리 장치 조건을 확인한 항목만 착수한다. 미확인 선정 게임 근거는 불필요의 증거가 아니다. CoreWindow는 승인 clone의 genuine CoreApplication UI thread, Wave64/128은 cross-SIMD 의미, SM은 operator/feature table, 상위 SDK는 별도 native Core, stereo/plane/multi-GPU는 실제 장치 proof가 각각 필요하다. 복수 GPU preference/adapter-change 검증도 해당 실제 장치가 필요하며 linked-node proof와 구분한다. |

Composition Root33 geometry/lifetime13 phases는 닫았지만 Windows alpha-colour parity·mixed ordinary/HWND·actual cadence는 별도다. Source pixels를 노출한 [WS_POPUP Root34](../build/local/composition-cadence-20261005.Bqiyja/run-x64-popup-34/stdout.log)의 warmup3 positive public events와 native warm seq5–10 positive times는 CAContext가 usable event data를 제공함을 보여준다. First unbound Sync2/4 time0·첫 critical Sync0 ordinal6은8 vblanks 동안 새 eligible event가 없어 실패했으며 원본을 보존한다([이력](IMPLEMENTATION-HISTORY.ko.md#multi-frame-fg와-native-borrowed-residency--2026-10-05)). 이어 Root35/36도 첫 warmup FAIL이고 사용자 요청으로 작업을 중단했다. 플랫폼 미지원·Sync0 해결·전체 cadence 완료로 단정하지 않는다.

Root34 fixed-epoch host→QPC·same physical-token reuse·진짜 backwards DISJOINT는 CPU0·paired build/stage0·양 ABI public strict per-query guards/consumer exit0으로 검증됐다. 통계는 **마지막 실제 표시 Present ID19/UI4**이지 positive event 합계가 아니다. [WoW64 native23 PASS](../build/local/multi-fg-phase-20261004/fsr-stable-clock-34/native-x86/result.json)와 [x64 count/index(1,1) unexercised·original false](../build/local/multi-fg-phase-20261004/fsr-stable-clock-34/native-x64/fg-capture-analysis.json)를 분리한다. X64는18 positive callback+4 UI=22 timed이며 GPU byte-exact seq2가 skipped된 사실을 숨기지 않는다. 두 ABI native windows는18px vertical gap이 있어 direct overlap으로 설명하지 않으며 occlusion 원인은 미입증이다. x64 Root31+WoW64 Root34는 mixed-cut 선정 backend proof이지 **동일 Root34 전체 양 ABI native PASS가 아니다.** 구형 forced-emulation을 실제 구형 hardware proof로 확대하거나 quantified 실게임 FPS/전체 완성을 주장하지 않는다.

**Composition 최종 중단 경계:** Root36 양 ABI build/install/stage0은 [actual 첫 warmup FAIL](../build/local/composition-cadence-20261005.Bqiyja/run-x64-bound-nontearing-sync0-36/stdout.log)을 대체하지 않는다. Required fifth scalar args4·cached validated permission·Frame40/Call200·GPU scheduling 유지 cut은 native acceptance를 실패한 candidate다. Root34 optional visual-control PNG는300s timeout FAIL의 scoped CGWindow surface image로서 physical scanout/cadence proof가 아니다. [Mixed x64 actual evidence](../build/local/composition-mixed-20261005.kbTsD1/run-x64-36/actual-stable-phase-evidence.json)는 ordinary-visible1/6만 PASS이며 다음 phase 전 public SetOffsetX E_NOTIMPL이다. FALSE/TRUE nonempty root pixels와 WoW64 proof는 없다. [Frozen failure ledger](IMPLEMENTATION-HISTORY.ko.md#multi-frame-fg와-native-borrowed-residency--2026-10-05)를 보존하고 user pause 상태에서 alpha-colour parity·Composition 완료를 선언하지 않는다.

한 항목의 구현과 x64/WoW64 공개 consumer·실제 GPU/표시 acceptance 및 관련 회귀를 끝낸 뒤 다음 항목으로 이동한다. 소스 변경·컴파일·독립 native probe만으로 완료하지 않는다. 이미 닫힌 A의 계약을 다시 미구현으로 넣지 않으며, 새 게임 통합에서 관찰한 시작 실패·렌더링 오류는 성능 작업보다 먼저 수정한다.

B/C의 CoreWindow·필수 Wave64/128·상위 SM/SDK·stereo·hardware-plane·linked/cross-node는 요청된 공통 기능 뒤 확인된 게임/engine 요구와 native/backend·실제 장치 조건에 따라 착수한다. 미지원 cap·규격상 허용된 absence를 무조건 결함으로 바꾸지 않는다. 항목별 증거는 CORE/HISTORY에 반영한다. Composition/CoreWindow에는 위 별도 Wine clone에서 필요한 선택 게임 경로 수정만 명시적으로 승인됐으며 가짜 현재-HWND activation·기존 Wine worktree 수정·새 multi-GPU/backend 확장·보류된 ARM64/FEX 실행 재개 승인은 아니다.

WARP/CPU renderer, 범용 video·보호 미디어·desktop capture subsystem, LifetimeTracker·manual write tracking·포괄적 Debug/InfoQueue/DRED/Tools 구현, SR/FG 개발용 debug-view·tear/reset 표시선은 현재 자동 착수 대상에서 제외한다. **게임 runtime의 SDK loader, 실제 history reset·resize·replay·수명 보존은 제외하지 않는다.** 기존 구현·증거·정직한 미지원 반환은 보존하며, 제외 항목을 다른 자동 착수 backlog로 옮기지 않는다.

NVAPI의 NVIDIA 전용 clock/DRS/Mosaic/SLI/CUDA/Cubin 등 광범위 기능은 대부분 미지원이지만 DX12 게임 번역기의 필수 scope로 자동 편입하지 않는다. Meta command가 0개인 것은 허용된 optional absence이며 임의의 vendor command를 만들어 지원으로 표시하지 않는다.

## 별도 보류 트랙 — ARM64/FEX 실행

사용자의 명시적 후속 지시로 실행 트랙을 재개했다. [ARM64/FEX gate](../docs/ARM64-FEX-IMPLEMENTATION.md)의 최신 관찰과 실제 runtime cut을 따른다.

1. **완료한 unified slice:** SDK02의 동일 native runtime에서 ARM64·FEX x64·FEX x86 모두 COM·integer/float/pointer/packet·reverse callback·Unix-call 및 Metal64-byte readback/fence를 통과했다. X86은 CPU pointer codec와 two-node blit도 PASS다. 같은 manifest, loaded hashes, 실제 native child closure와 strict no-Rosetta를 확인했다.
2. **확보·실행한 입력:** official Wine MR11638/FEX-2609.1 immutable pins, licenses, owned Darwin shared-data/MAP_JIT patches와 fresh build/stage/run recipe. Vendor Preview 입력 대기를 선행 조건으로 유지하지 않는다.
3. **추가 완료:** native raw-section16KB alignment, raw ARM64X Core machine admission와 공유 Core/Main generation을 적용해 native ARM64/x64 graphics·compute8-frame exact readback을 통과했다. 선택한 `--no-present` smoke이며 game/presentation proof는 아니다.
4. **현재 구현:** 실제 PE32 shader 실행에서 실패한 `NtUserInitializeClientPfnArrays`의 unbased memory pointer를 `wow64win` shared helper에서 변환한다. Fresh Wine cut을 빌드하고 실제 shader·validation·loaded identity를 다시 검증한다.

Native compiler/Unix plugin, production PE, ARM64X paired build, native Wine package는 이미 완료된 기반이다. Host 실행을 guest 성공으로 바꾸지 않는다.

## 성능 및 검증-only 트랙

- **완료한 선택:** 기존 owned PSO blob의 자동 RAM/disk·same-key single-flight. 생성/재시작 비용과 identity·손상·동시성·용량 경계를 검증했다. **추가 후보**인 native Metal pipeline/archive는 남은 native 생성 비용이 병목일 때만 착수한다. 기존 serialization/PipelineLibrary/ShaderCacheSession을 재구현하지 않는다.
- **측정 후 선택:** sparse mapping, queue overlap, retirement, allocation/copy, memory pressure, cold/warm 생성 비용, stutter·전력. 미측정 성능 개선을 약속하지 않는다.
- **검증 확대:** 다른 GPU/OS, Windows differential, 실제 Streamline/game loader, 물리 HDR/scanout, 장시간·성능. SampleCmp 확장/GatherRaw/u64 gather, 비-CS denorm, library GS/HS/DS, mixed-bitness sharing은 구현과 proof 부재를 구분한다.
- **NGX 색상 계약 검토:** IsHDR는 HDR display/PQ selector가 아니다. LDR/perceptual 입력과 MetalFX 계약을 독립 oracle로 대조할 후보이며 unused bit만 보고 임의 gamma 변환이나 HDR 거부를 추가하지 않는다. FG의 dispatch transfer/async queue는 이미 연결돼 있다.
- **Known native 경계:** Metal4FX 16→48의 Conv3x3 validation 오류는 독립 native에서도 재현됐다. 누락된 port로 재분류하거나 validation을 숨기지 않는다.

## 완료·오분류 제외

Full SR/FG port의 MV/masks/subrect/transfer/sharpening/dynamic output/history/UI/proxy/pacing, ShaderCacheSession/Control, PipelineLibrary/CachedPSO, COPY timestamp, programmable offsets, unaligned BC, compute CalculateLOD, budget notification, root/indirect arena, 기존 DXR1.1/Mesh1/VRS2/feedback1.0/workgraphs1.0은 남은 구현에 다시 넣지 않는다.

현재 D3D12 1D comparison sampling은 logical 1D→depth2D 변환으로 구현돼 있다. 1D gather는 요구할 유효한 HLSL operation이 아니다. RenderPassTier0는 BeginRenderPass 부재가 아니며 culled MS primitive 통계 flag FALSE는 mesh 통계 부재가 아니다. 기본 view의 PlaneSlice=0 및 오래된 DXBC TODO만으로 일반 입력 결함을 확정하지 않는다. D3D12 device가 IDXGIDevice를 노출하지 않는 것 자체도 불일치가 아니다.
