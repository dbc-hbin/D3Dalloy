# D3D12 temporal·NGX API review와 현재 경계

2026-10-02 현재 루트 `src/nvngx`, `src/d3d12/d3d12_temporal.cpp`, command list/allocator/queue와 Metal 4 FX bridge를 기준으로 정리했다. Source inspection, full SR/FG port의 기존 runtime 증거, 후속 P0 discovery proof를 구분한다. 문서 편집 자체는 새 GPU/game 실행이 아니다.

전체 프로젝트 지원·남은 우선순위는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#남은-구현-후보)가 기준이다. 이 문서는 temporal/NGX의 contract review에 한정하며 D3D11 기능 감사나 최신 규격 전체 conformance 판정이 아니다.

## 2026-09-19 review findings의 현재 disposition

당시 `build/owned-graphics-20260919/{dxmt,ngx-work}` snapshot의 결함을 현재 priority로 재사용하지 않는다. Frozen patches는 provenance이며 루트 소스가 현재 구현의 권위다.

| 당시 관찰 | 현재 disposition | 검사 경로 |
|---|---|---|
| host encode order의 hidden temporal event와 future D3D12 wait/signal cycle | FIFO submission/deferred wait·device temporal encode→commit serialization 구현 | temporal future-signal·opposite-order + trace |
| deferred worker가 mutable list entry/allocator를 다시 읽는 수명 위험 | 제출 시 recording snapshot·lease·exact allocator retention, GPU completion까지 보호 | 96 list resets·97 heads·caller release |
| G1→G2→G1 replay가 old history를 연속으로 취급 | last_executed_generation 기반 reset | reset `1,1,1,0` |
| allocator final release에서 TemporalEncoderData destructor 생략 | recorded command cleanup이 Reset/destructor에서 payload를 정리 | temporal frame destroy/retirement trace |
| private QI/GetDevice에 `Com::ref()`를 out storage로 사용 | 실제 COM output storage 사용으로 수정 | valid positive record control |
| Shutdown1(nullptr)를 InvalidParameter로 반환 | global shutdown + success | distinct two-device contract fixture |
| heap HandleToken을 process lifetime vector에 축적 | monotonic opaque token, no permanent token heap vector | 64 cycles·stale FeatureNotFound |
| output-subrect opt-in 기본 1·nonzero base 전부 거부 | 기본 0, opt-in enabled base 전달·backend bounds·rectangle copy | NGX record boundary + temporal `(3,5)` GPU sentinel |
| shutdown/re-init 중 late feature/parameter publication | registration identity recheck·init gate/insertion critical section | 현재 source 수명 계약; 모든 race interleaving proof를 주장하지 않음 |

현재 fixture coverage와 acceptance 해석은 [NGX-TEST](NGX-TEST.ko.md), [TEMPORAL-TEST](TEMPORAL-TEST.ko.md), [TEMPORAL-COMPLETION-TEST](TEMPORAL-COMPLETION-TEST.ko.md)를 따른다. 같은 source fix·fixture 존재·실제 runtime PASS는 서로 다른 증거 층이다.

## 현재 source-backed 제한과 구현 후보

아래는 **미구현/의미·portability 경계**다. 현재 게임 failure라고 단정하지 않고, flags74 baseline 밖의 일반 API 의미를 source와 공식 계약으로 재검증할 대상이다.

### 1. HDR 의미와 support reporting

Create는 `IsHDR` bit를 허용한다. Full SR port는 explicit linear/sRGB/PQ transfer와 GPU 전후처리를 구현했으므로 “transfer selector/변환 전체가 없다”는 과거 판정은 현재 backend에 적용하지 않는다. 다만 NGX HDR bit 하나를 FSR의 explicit transfer/luminance 계약과 동일시하거나 HDR/LDR의 DLSS↔MetalFX 모델·품질 동등성을 주장하지 않는다. Gamma 2.2 같은 임의 보정을 추가할 근거도 없다.

P0-4 수정 후 `GetFeatureRequirements`는 adapter의 owned native Metal device에 대해 Init과 동일한 `WMT4::TemporalFeature::Supports` predicate를 사용한다. 유효한 미지원/비owned adapter의 SuperSampling discovery는 **Success + AdapterUnsupported(4)**이며 native 지원을 가장하지 않는다. Null 인자와 미지원 feature ID의 오류는 별도다. 완료 증거와 scope는 [P0 공개 계약](CORE-IMPLEMENTATION-STATUS.ko.md#p0-공개-계약-정합)을 따른다.

### 2. Device/format portability

Full SR backend는 runtime MetalFX descriptor의 min/max scale과 texture usage를 조회한다. FSR의 max 초과 output은 균일 배율로 floor한 centered active 영역과 opaque-black margin으로 처리하며 output subrect 밖은 보존한다. 특정 장치의 max3을 고정 [1,3] backend 계약이나 모든 device/OS의 보장으로 해석하지 않는다. NGX quality callback의 별도 sizing policy와 native scaler capability도 구분한다.

2026-09-19 public classic MetalFX probe의 range [1,3], minimum usage color/depth/motion/output `1/1/1/7`은 **그 probe의 관찰**이다. 현재 모든 Metal4FX device/OS/model에 같은 최소 요구가 있다고 추론하지 않는다. Factory rejection과 현재 M5 Pro fixture PASS는 다른 device portability proof가 아니다.

Color/depth/MV/mask texture backing extent는 같을 필요가 없다. 현재 계약은 각 active subrect가 해당 view bounds 안에 들어가는 것이다. 현재 full-port fixture의 offset·독립 mask rect·dynamic extent proof를 근거로 삼으며, 과거 classic probe만으로 Metal4FX 지원을 추론하지 않는다.

### 3. 명시적으로 좁힌 NGX 입력 의미

High-res/jittered MV, reactive/composition masks, 공식 color/depth/MV/mask/output subrect offsets와 RCAS sharpening은 full port로 구현됐다. NGX DoSharpening bit는 native descriptor 기능이 아니라 별도 GPU post-operation을 켠다(비활성 상태의 nonzero Sharpness는 거부). Alpha-upscale bit128은 별도 alpha history로 지원하며 reserved/unknown flags는 거부한다. P1의 사용자 선택은 유효한 nonidentity Exposure.Scale과 nonzero MV.Offset.X/Y를 받아 실행을 계속하되 수치 효과를 무시하는 **호환 계약**이다. Exposure 곱셈이나 MV 값 bias 구현으로 부르지 않으며 이미 지원하는 MV texture subrect offset과 혼동하지 않는다. Public x64의 기본/비기본 5쌍×3 live GPU replay와 invalid-output 보존을 통과했다([완료 증거](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)).

Manual exposure는 AutoExposure off일 때 optional null 또는1×1 R16Float/R32Float를 지원한다. R32는 매 execution/replay에서 선행 GPU writes 뒤 private R16 scratch로 수치 변환하며 ABI 변경·clamp·tone correction·CPU readback·record-time cache가 없다. AutoExposure는 supplied exposure를 무시한다. [양 ABI numeric·수명·public NGX proof](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료)가 완료돼 R32 conversion은 남은 후보가 아니다. Output subrect도 opt-in/bounds/scratch copy를 이미 지원한다.

Pristine GPTK D3DMetal4.0b2의 reference는 MPL이 Exposure.Scale을 읽지 않고 legacy Metal은 non1에 warning 후 실행을 계속하며 MV.Offset을 읽지 않는다는 bounded binary 관찰이다. [정확한 identity·증거·memory 제외](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)는 NVIDIA 의도 수식·GPU differential·conformance 증거가 아니다. 일반 NGX 수치 의미를 확장하려면 별도 public 계약·numeric semantics·backend bridge·실제 GPU oracle이 필요하다. Flags74 synthetic sanity PASS나 게임 parent format 일치만으로 전체 NGX/DLSS image quality를 증명하지 않는다.

### 4. Architecture와 실제 consumer

MSVC parameter facade의 현재 Meson target은 x64/i686/ARM64/ARM64EC를 구분하고 paired ARM64X packaging을 지원한다. 10월7일 direct isolated FEX x64 NGX/NGX-contracts/FSR SR의 지정 실행 기록이 있으며 ARM64/FEX는 과거 보류 상태가 아니다. Native x86 Metal4FX의 CPU-pointer 누락과 아직 실행되지 않은 경로는 [새 리뷰](../docs/D3D12-REVIEW-AND-DESIGN-2026-10-07.md)에 분리한다. 다른 ABI의 build/기존 실행을 이 경로의 acceptance로 바꾸지 않는다.

Synthetic loader는 system32 DLL을 직접 요청한다. 실제 게임/Streamline의 owned core DLL 선택·public NGX calls·queue encode·출력의 연결 관찰은 아직 필요하다. Vendor identity 또는 registry gate만으로 실제 loader 성공을 선언하지 않는다.

## 2026-10-01 전체 번역기 review와의 관계

별도 [D3D12 translation review](../docs/D3D12-TRANSLATION-REVIEW-2026-10-01.md#implementation-dispositions--2026-10-02)의 **11건은 수정 또는 이미 수정됨**으로 disposition됐다. 그 문서의 initial finding/제안과 마지막 disposition을 구분한다. 당시 fixed 4 MiB arena의 memory-safety 수정 뒤 **retained chunk 성장**을 구현했다. 초기 4 MiB와 `max(요청, 4 MiB)` 추가 chunk는 기존 pointer·Metal buffer·GPU VA를 보존하고, native 길이·allocation 실패는 checked recording error/Close failure로 처리한다. [후속 focused proof와 경계](CORE-IMPLEMENTATION-STATUS.ko.md#rootindirect-arena-성장과-focused-proof)는 unlimited recording·물리 메모리 고갈·새 temporal/game proof가 아니다.

Shared fence publication, structured OOB, Metal 4 RTAS, shader shape, mapped-view lifetime, queue-wide timestamp, wrapping view-location의 과거 finding을 현재 temporal blocker로 복사하지 않는다. Timestamp baseline hardware도 after regression을 통과했다는 기록은 static contract fix와 failing-before GPU proof를 구분하는 사례다.

새 root audit에서 발견한 `RemoveDevice` no-op은 후속 수정과 x64/WoW64 focused 실행으로 [완료](CORE-IMPLEMENTATION-STATUS.ko.md#removedevice-완료와-focused-proof)했으며 열린 후보가 아니다. DXGI budget notification도 actual-source polling·owned event/cookie 수명 계약과 x64/WoW64 focused proof 범위에서 [완료](CORE-IMPLEMENTATION-STATUS.ko.md#dxgi-budget-notification-완료와-focused-proof)했다. 실제 물리/native budget의 positive 변화 관측은 주장하지 않는다. ShaderCacheSession/Control도 application-managed MEMORY/DISK와 양 ABI CPU proof 범위에서 [완료](CORE-IMPLEMENTATION-STATUS.ko.md#shadercachesessioncontrol-완료와-focused-proof)했다. 남은 runtime 후보는 CORE에서 관리한다. 초기 소스 감사와 후속 실행은 별도 증거이며 이 NGX review가 그 실패를 새로 실행해 관측했다는 뜻은 아니다.

## 증거·역사 경계

- 2026-09-19 static review는 GPU/game을 실행하지 않았다. 당시 source snapshot/hash와 `api-review/source-snapshot.txt` 경로는 역사적 provenance이고 삭제된 build artifact를 현재 보유한다는 뜻이 아니다.
- 이후 Metal 4 temporal의 typed/typeless·subrect·replay·future-wait·lifetime readback/trace는 이미 관찰된 named fixture 증거다. Recovery 결과·mixed-cut 집계와 retained 여부는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다.
- Full SR/FG port는 기존 **full-metalfx-port-20261002/runtime-02**와 [FG after-validation06](../build/local/full-fg-port-20261002/after-validation-06/result.json)의 지정 acceptance를 완료했다. Masks·high-res/jittered MV·sharpening·dynamic extent·FG 전체를 backlog로 되돌리지 않는다. 새 P0 runtime-01의 [NGX discovery12 checks](../build/local/p0-contracts-r5KJ0O/capability-discovery/after-ngx-x64/stdout.log)는 x64 public discovery/Init 정합 proof이며 SR/FG GPU suite를 이번 runtime에서 다시 실행한 증거가 아니다.
- 현재 남은 경계는 전체 conformance, HDR transfer/physical scanout, 다른 GPU/OS, Windows reference differential, performance, 실제 game loader와 ARM64 Wine/FEX 최종 통합이다. Plans/proposed acceptance를 observed PASS로 적지 않는다.
