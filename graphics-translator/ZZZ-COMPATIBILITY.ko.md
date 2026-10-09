# ZZZ workload의 실행·호환성 관찰

## 현재 문서의 역할

이 문서는 **2026-10-09의 격리 DLSS·precompile·FEX 최적화 증거와 2026-09-18~19의 역사적 관찰 기록**을 구분한다. 현재 제품 범위는 Metal을 통한 D3D12 게임 실행이며, ZZZ 한 workload의 성공을 전체 지원 matrix나 프로젝트 완료로 일반화하지 않는다([CORE 목표](CORE-IMPLEMENTATION-STATUS.ko.md#개발-목표와-우선순위)).

현재 compiler는 DXMT airconv·LLVM·자체 AIR 경로다. Apple Metal Shader Converter, SPIR-V, GPTK/D3DMetal compiler/runtime fallback을 현재 제품 경로로 남기지 않는다. 아래 Apple SDK·dxil-spirv 실험은 **당시 선택과 CPU/GPU 실험의 역사**이며 현재 지원 증거가 아니다. 공개 Metal/MetalFX 사용은 자체 compiler와 양립한다.

현재 D3D12 NGX와 temporal COM 경로는 구현됐고 Metal 4 named fixture acceptance도 관찰됐다. 옛 FL11_0/OPTIONS5 미구현/DXIL E_NOTIMPL 표를 현재 support로 읽지 않는다. 현재 선언·검증 범위는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태), NGX current integration은 [NGX-INTEGRATION](NGX-INTEGRATION.ko.md)가 기준이다.

**2026-10-09에는 owned NGX의 실제 Streamline 로딩과 DLSS 도시 출력·끄기/켜기 재생성을 확인했다.** 아래 지정 실행의 증거이며, 장시간 성능·DLSS 모델 동등성·physical HDR scanout은 이 판정에 포함하지 않는다. 같은 날 별도 실제 precompile 경로의 root-signature binding 진단·자동 PSO 캐시 재스캔·리소스 검사 완료와 그 뒤 최종 candidate 도시 출력은 [아래 별도 절](#2026-10-09-실제-precompile-경로-진단과-자동-pso-캐시-재스캔)에 구분해 기록한다.

아래 `build/owned-*`/`build/dlss-*` 경로·hash는 당시 식별자다. Build 삭제로 유실된 옛 logs/JSON/PE를 현재 retained artifact처럼 표현하지 않는다. 관찰 기록은 유지하되 [CORE 증거 ledger](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)의 recovery retained proof와 분리한다. Frozen patch/upstream lock/provenance는 변경하지 않는다.

## 2026-10-09 owned NGX 로딩과 DLSS 검은 화면 복구

Live 설치와 prefix를 건드리지 않은 APFS 복제본에서 Wine11.17/ARM64X/FEX, owned D3D12·NGX·MetalFX 경로를 실행했다. [실게임 결과](../build/local/zzz-dlss-fix-20261009/dlss-run-02/result.json)는 source-cut·runtime manifest·서명된 NGX의 SHA-256과 실제 호출·화면·종료 증거를 묶는다.

- **초기화:** 게임의 Streamline2.11.1.da40c631은 unsigned core를 `CRYPT_E_NO_MATCH`로 거부했다. Owned publisher의 실제 Authenticode 서명과 해당 Wine prefix 안의 공개 인증서 등록으로 일반 SDK 경로를 통과했다. NVIDIA 서명 가장, 검증 우회, 게임 DLL 교체는 없다. Fresh-prefix 기본 Wine 등록 증거는 [enrollment](../build/local/zzz-ngx-enrollment-20261009/EVIDENCE.txt), 배포 계약은 [Runtime](RUNTIME.ko.md#선택적-owned-nvidia-extension)을 따른다.
- **매 프레임 실패:** feature 생성은 성공했지만 `EvaluateFeature`가 첫 required-resource 조회에서 `MissingInput(0xbad0000a)`을 반환했다. Streamline의 `Set(void*)`와 owned store의 `Get(ID3D12Resource**)` 사이 alias 변환 누락을 수정했다. 이후 실제 게임의 parse는 input624×360·pre_exposure1로 성공하고 backend는 `HRESULT 0`을 반환했다.
- **실제 출력:** DLSS/성능 우선, FG·RT 끄기에서 캐릭터·건물·텍스처를 확인했다. 같은 process·순찰 부스 위치의 [끄기](../build/local/zzz-dlss-fix-20261009/dlss-run-02/dlss-city-off.png)와 [다시 켜기](../build/local/zzz-dlss-fix-20261009/dlss-run-02/dlss-city-reenabled.png) 모두 렌더링되며 후자는 MetalFX temporal624×360→1280×720을 표시한다. 이는 NVIDIA neural model 실행이나 품질·성능 동등성 증거가 아니다.

도시 실행의 실패-input directory는 비어 있었다. 도시 성공은 사용자가 보고한 프리컴파일의 root-signature binding 오류나 완료 여부를 해결했다는 증거가 아니며, 그 입력은 아래 별도 실제 precompile 경로에서 64쌍 전부 재현·진단했다. 목적을 마친 이전 실험 payload는 삭제하고 [retirement 기록](../build/local/zzz-dlss-fix-20261009/dlss-run-02/retirement.log)과 로그·manifest·화면을 보존했다. 이 진단의 최종 `builds/cache-04/runtime`은 후속 FEX 최적화의 비교 기준본이며, 최신 통합 후보는 아래 FEX 절을 따른다.

## 2026-10-09 실제 precompile 경로 진단과 자동 PSO 캐시 재스캔

`precompile-run-01`~`03`은 위 도시 실행과 **별개의 실제 precompile 관찰**이다. 증거 root는 `build/local/zzz-dlss-fix-20261009`다.

### root-signature binding 진단

- **원인 확정:** 원본 실패64쌍은 [binding 진단](../build/local/zzz-dlss-fix-20261009/precompile-run-01/binding-diagnosis/result.txt)과 [전량 replay](../build/local/zzz-dlss-fix-20261009/precompile-run-01/replay/all/aggregate.json)에서 owned compiler와 pristine GPTK4.0b2 **양쪽 모두 64/64 거부**됐다. 대표 PS는 root가 t0..t7만 덮는데 shader가 absolute register8(t8)을 실제로 사용한다. Owned binding lookup의 off-by-one·class·rangeID/stage 오류는 없으며 root 확장이나 permissive mapping으로 고치지 않았다.
- **게임 쪽 입력:** [warmup caller 분석](../build/local/zzz-dlss-fix-20261009/precompile-run-01/warmup-caller/result.txt)은 Unity가 direct warmup 경로에서 실제 serializer에 SRVcount8·register0·space0을 넘겨 이 root를 직접 생성함을 관찰했다. 실패한 `CreateGraphicsPipelineState` HRESULT는 그 routine에서 로그·retry·전파 없이 무시되고 job 진행 counter는 계속 감소한다. 따라서 이 실패만으로 전체 진행이 멈춘다고 단정하지 않는다.
- **금지 유지:** compiler validation 완화·실패 은폐·fake PSO로 이 쌍을 통과시키지 않는다. 진단용 synthetic root는 배포물이 아니다.

### 자동 PSO 캐시 재스캔 제거

- **실제 원인:** `WriteAutomaticPipelineCache`가 매 성공 publication마다 directory mutex 아래 12k 파일 전체를 다시 스캔했다. [실게임 fs_usage window](../build/local/zzz-dlss-fix-20261009/precompile-run-02/profile-analysis/summary.json)는 5.249429초 동안 owned `lstat64`+`listxattr` **1,969,438**행(전체의94.4%)을 기록했다.
- **수정:** 16-byte named shared `AutomaticDiskAccounting`(bytes+valid)을 유지하고 rescan은 첫 실행·invalid·abandoned·quota 압박에만 한다. 퇴출은32MiB headroom batch, entry64MiB·disk256MiB 한도와 atomic temp→replace는 그대로다. 이미 존재하는 entry를 교체할 때 이중 계상하던 회계를 고쳤다.
- **CPU realWinAPI proof:** 수정 전 [before-final](../build/local/zzz-dlss-fix-20261009/precompile-run-02/cache-regression/before-final/run.log)은 replacement-at-cap에서 실패했고, 수정 후 [after-final-03](../build/local/zzz-dlss-fix-20261009/precompile-run-02/cache-regression/after-final-03/run.log)는 contents/quota/cross-process/clear/failed-rename/abandonment/foreign 보존 invariants PASS다. 같은 test에서 fresh1KiB publication256회의 metadata enumeration이 before **256**회 → after **0**회다(초기 clear 제외).
- **한계:** 최종 [실게임 window](../build/local/zzz-dlss-fix-20261009/precompile-run-03/profile-analysis/summary.json)는 5.248979초·owned `lstat64`/`listxattr` **0**행이지만 두 window는 서로 다른 workload다. **Controlled speedup·CPU-time·FPS 증거가 아니다.**
- **Native profile:** 최종 native [sample](../build/local/zzz-dlss-fix-20261009/precompile-run-03/precompile-native.sample.txt)은 cache directory 전수 enumeration 대신 SM50/LLVM compile 경로와 다수 thread의 service 대기가 지배한다.

### 리소스 검사 완료 상태

Root가 [최종 리소스 검사 dialog](../build/local/zzz-dlss-fix-20261009/precompile-run-03/progress-during-evidence-update.png)를 직접 확인했다. "클라이언트가 완전해 복구가 필요하지 않습니다"와 green 확인이 표시되고 별도 compile progress는 없다. 앞서 관찰한 진행률1.13→1.98→10.76에 이어 이 리소스 검사/precompile 흐름은 cache-04에서 **실제로 종료**했다([identity](../build/local/zzz-dlss-fix-20261009/precompile-run-03/identity.sha256), [retirement](../build/local/zzz-dlss-fix-20261009/precompile-run-03/retirement.log)).

이것은 **모든 invalid shader/root 쌍이 컴파일됐다는 뜻이 아니다.** 위64쌍은 계속 올바르게 거부되고, 같은 실행에서도 실패 입력64개가 다시 기록됐다(run-01과62개 중복, 2개는 새 입력). 남은 compile 오류와 이 완료 판정을 구분한다.

**최종 candidate 도시 출력:** 완료된 precompile 뒤 같은 실행의 [도시 출력](../build/local/zzz-dlss-fix-20261009/precompile-run-03/dlss-city-output.png)에서 캐릭터·건물·보도·나무·텍스처가 렌더링되고 MetalFX temporal624×360→1280×720이 표시된다. 같은 실행의 [그래픽 설정](../build/local/zzz-dlss-fix-20261009/precompile-run-03/graphics-settings-visible.png)은 초해상도 DLSS·품질 등급 성능 우선·프레임 생성 끄기·레이 트레이싱 끄기다. 실행 identity·QA·한계는 [result.json](../build/local/zzz-dlss-fix-20261009/precompile-run-03/result.json)이 묶는다. 이는 controlled FPS·frame-time·장시간 stutter 완료 증거가 아니며 duration을 추론하지 않는다.

## 2026-10-09 FEX 최적화와 재검증

별도 FEX-darwin에서 IR 블록 끝의 반복 탐색을 줄였다. 실제 emitter의 동일 입력 8,192개 블록 비교에서 head-fixed 탐색은 100,651,008회에서 8,191회로, CPU 시간 중앙값은 113.993ms에서 0.108ms로 감소했다. 출력 일치·ASan/UBSan과 Wine CPU/ABI 회귀 34개를 통과했다. 이는 해당 JIT 작업의 비용 감소이지 게임 전체 로딩·FPS·스터터링 개선율이 아니다.

[최종 결과](../build/local/zzz-fex-opt-20261009/result.json)는 소스/DLL identity, 검사와 한계를 묶는다. `candidate-03`의 [DLSS 설정](../build/local/zzz-fex-opt-20261009/candidate-03/dlss-settings-confirmed.png)과 [실제 출력](../build/local/zzz-fex-opt-20261009/candidate-03/city-dlss-01.png)에서 DLSS/성능 우선·FG/RT 끄기, 캐릭터·NPC·실내 텍스처와 MetalFX temporal624×360→1280×720을 확인했다. NGX resource parse는 result1, backend는 hr0이었다. 한 장면의 렌더링 회귀이며 장시간 플레이·명시적 Metal validation-layer·전체 precompile 재실행 검증은 아니다.

초기 진단 런처가 공백이 있는 `dxgi.customDeviceDesc`를 따옴표 없이 덮어써 GPU 이름이 `NVIDIA`로 잘리고 DLSS 항목이 숨겨졌다. 중복 override를 제거하고 내장 ZZZ 프로필을 쓰자 같은 FEX/runtime에서 복구됐다([설정 문법](../docs/CUSTOMIZATION.md#file-environment-and-precedence)). Streamline의 `eErrorFeatureFailedToLoad` 경고는 정상 실행에도 있으므로 그 문자열만으로 DLSS SR 실패를 판정하지 않는다. 이 잘린 이름/TAA 상태에서 얻은 앞선 FEX 프로파일은 DLSS 성능 증거가 아니며, 서로 다른 로딩 window를 전후 속도 비교로 합치지 않는다.

최신 후보는 `build/local/zzz-fex-opt-20261009/runtime`이다. 임시 게임·prefix·진단 앱과 소유한 임시 JIT 맵은 종료 확인 뒤 정리했고, 증거와 비교 기준본을 보존했다. 실사용 게임·prefix는 수정하지 않았다.

## 2026-09-19 정적 preflight의 입력

읽기 전용 preflight가 당시 분석한 설치의 식별자다. 지금 같은 파일이 설치돼 있다고 새로 확인한 값이 아니다.

| 입력 | 당시 SHA-256 |
|---|---|
| `ZenlessZoneZero.exe` | `bd809b00609e029f114f8213fc248b5fd2d7466a3251e5ab8beaa2ca15387faf` |
| `UnityPlayer.dll` | `c9dec60f119fde57cc5aba605c0a84fe001aebf8ab07af43f9f82fd7ec0bd2f6` |
| `globalgamemanagers.assets` (70,601,448 bytes) | `cbfa24902f86f2fa87f1ebd183beb2685d556430bb5dc454dae613f1537d4ac8` |
| game-shipped `WinPixEventRuntime.dll` | `81adcfd8253c3489be720da7e30f16004dc9a1f02a8b418c6c3aef4993032e6d` |

당시 DXMT base는 `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`이다. `zzz-preflight/`의 bounded disassembly·container/import summaries는 이 입력에 한정된 provenance다. 새 버전 게임 전체에 일반화하지 않는다.

## 2026-09-19 Unity 초기화의 static 계약

당시 bounded disassembly는 Unity가 D3D12CreateDevice FL12_0을 먼저 요청하고 실패 시 **같은 adapter의 FL11_0**을 다시 요청하는 것을 확인했다. 초기화 뒤 query는 다음과 같았다.

| Query | 그 Unity build의 처리 | 당시 owned baseline |
|---|---|---|
| D3D12_OPTIONS | 60-byte struct; 실패 시 base init 실패 | 구현됨 |
| FEATURE_LEVELS | 11_0/11_1/12_0/12_1; 실패 시 기록값 11_0 | core-contracts 후 max 11_0 |
| OPTIONS5 | 성공 + DXR tier≥1.1일 때 RT 경로; 실패/낮은 tier 허용 | E_NOTIMPL |
| OPTIONS7 | mesh tier가 있으면 mesh 경로; 미지원 허용 | mesh/feedback NOT_SUPPORTED |

당시 `0009-d3d12-core-contracts.patch`는 생성 요청 성공·내부 level·query max의 불일치를 FL11_0으로 맞췄다. 그 **역사적 기준 실행**의 `Direct3D 12.0 [level 11.0]` 기록과 현재 FL/tier 선언은 별개다. OPTIONS5/7 optional detection은 그 초기화 call site가 실패를 허용했다는 증거이지 전체 게임의 DXR/mesh 요구 부재나 프로젝트 scope 축소의 근거가 아니다.

## 2026-09-19 shader container·binding 관찰

DXBC header·chunk bounds를 확인한 bounded scan이다. `UnityPlayer.dll`의 valid container 36개는 모두 legacy SHDR/SHEX, DXIL은 없었다. `globalgamemanagers.assets`의 valid container **772개**는 다음과 같았다.

| Container | 개수 |
|---|---:|
| SHEX compute SM5.0 | 352 |
| DXIL type5 compute SM6.0 | 356 |
| DXIL type5 compute SM6.5 | 56 |
| DXIL type6 SM6.5 | 8 |

Version token은 `type=token>>16`, `major=(token>>4)&0xf`, `minor=token&0xf`로 읽었다. Type6의 세부 의미를 이 scan만으로 추정하지 않았다. **실제 DXIL을 shipped했다**는 증거이며 SM5 container 개수만으로 kernel별 완전한 fallback pair를 증명하지 않는다.

### 역사적 startup 실패와 실제 입력

`zzz-baseline-startup-02` runtime manifest SHA-256은 `5668fe08a076902c7b61c5c97d67dda867a0776c93bc23f6b499f11af4e0eb29`다. 기존 `wine-bundles-02`를 복제한 당시 baseline으로, 후속 compiler/NGX/query 후보가 아니었다.

- 원본 게임·live prefix·설치 runtime을 바꾸지 않고 APFS 복제본의 prefix를 wineboot -u로 준비했다.
- Owned builtin D3D12/DXGI/winemetal과 private native module이 로드됐다. 최초 SHEX VS/PS 뒤 DXIL VS/CS가 `0x80004001`(E_NOTIMPL)로 실패했고 게임 콘텐츠는 검은 화면이었다.
- 그 runtime의 InitializeShader가 DXIL chunk를 거부했다. **그 실행에서 확인된 blocker**이며 현재 루트 compiler가 DXIL을 거부한다는 주장이 아니다.
- 120초 관측 뒤 해당 private prefix의 wineserver만 종료했다. timed_out/cleanup exit 0은 정상 게임 종료나 PASS가 아니다.

당시 runtime trace SHA-1과 asset에서 추출한 전체 container SHA-1이 일치한 6개 입력:

| Stage | Bytes | SHA-1 | 추출 origin |
|---|---:|---|---|
| VS | 2476 | `b7ca45bd2d9c97131a6f26b933b480d5c23cb92e` | Hidden/Universal Render Pipeline/DepthPiramid |
| VS | 1812 | `7d97cd42b457596c9e5bbdb28beb3626978ddebb` | Hidden/Universal Render Pipeline/ScreenSpaceReflection |
| VS | 2996 | `d84d5ac214565aaeb45d9eae3d86cca8d6bd1b1c` | Hidden/BlitCopy |
| CS | 1764 | `ede9c0e31ec1b771e27f4903e404c30763ef368a` | asset offset 69605416 |
| CS | 6312 | `76c42ee4d7bd030f7afb83947a93b8c1e05d42e9` | asset offset 68678668 |
| CS | 7424 | `9c3c1108a1b79c3cb290dd22ca92df0b8e9edbfe` | asset offset 68685256 |

VS 추출은 UnityPy 1.25.3/LZ4로 platform25 Shader blob을 풀었다. 당시 `actual-startup-shaders/{manifest,compressed-manifest}.json`이 provenance였다. 게임 bytecode는 local ignored input이며 소스 배포물로 옮기지 않는다. 여섯 입력의 `dxil-spirv --validate` 성공은 당시 CPU 실험일 뿐 현재 compiler/GPU/binding proof가 아니다.

### 역사적 후속 root-contract 실행

SDK 전환 전 별도 DXIL 경로에서 root-descriptor compute, VS/PS varying·derivative·depth GPU readback이 관찰됐지만 descriptor table을 거부하므로 ZZZ 호환성 증거가 아니었다.

`zzz-root-contract-startup-04` manifest SHA-256은 `2c8ac2bbf4e28a9a65209d18bf03b4364f9117b42ac0282bb7e292a4ff1d9b44`다. VS `7d97cd42...`는 hr=0, 뒤 PS/다른 binding은 실패했다. Opt-in root blob의 **16개** DXBC/RTS0 기록은 descriptor tables, CBV b0, SRV t0..t5, UAV u0..u2, sampler s0..s1을 포함했다. 이것은 그 16개 PSO의 계약이며 게임 전체에 root constants/static samplers가 없다는 뜻이 아니다. 추가 PS 다섯 입력의 당시 provenance는 `compressed-manifest-run04.json`이었다.

### 역사적 Apple SDK 선택과 폐기된 제품 방향

2026-09-19에는 공개 Metal Shader Converter 사용을 승인해 공식 3.0 package/CLI3.0.6의 Apple signature/trusted timestamp를 확인하고 local prefix에 추출했다. SDK 원본·license/notices는 frozen provenance로 보존한다. SDK 변경·thin·재서명이나 GPTK 동명 library 대체를 허용하는 기록이 아니다.

당시 RayTracingShader 8 container의 raygen/miss 18 entry 중 14개가 metallib/export를 생성했고 4개 RTXGI entry는 FP64 UNORM24 변환에서 실패했다. `1/(2^24-1)`과 `2^-24`는 다르며 입력 8388607의 double→float `0.4999999701976776`을 단순 float 곱 `0.5`로 바꾸면 의미가 달라진다는 분석을 남겼다. 이는 **당시 CPU compiler 결과**로 AS/SBT/DispatchRays GPU나 scene proof가 아니다. 이후 자체 airconv/AIR 방향으로 바뀌었으므로 “정규 구현을 SDK로 전환한다”는 옛 지침은 더 이상 유효하지 않다.

## 2026-09-18~19 DLSS parent resource 관찰

과거 별도 actual-game translator trace의 `resolved_resources`는 다음 parent를 기록했다.

- color/output: R8G8B8A8_TYPELESS(27)
- depth: R32G8X24_TYPELESS(19)
- motion: R16G16_TYPELESS(33)

관찰 input/output은 한 run의 2256×1272/3840×2160, 다른 run의 1536×984/2624×1696이었다. 당시 경로는 `dlss-translator-20260918/actual-game/.../translator.jsonl`과 `nameplate-regression-20260919/actual-game/.../translator.jsonl`이다. 이것은 **옛 별도 경로에서의 parent metadata**이며 현재 owned NGX가 그 게임 session을 처리했다는 증거가 아니다. 모든 typed SRV/UAV/RTV/DSV descriptor도 관찰한 것이 아니다.

이 입력은 frozen format-capability patch와 현재 temporal `typeless_zzz` fixture의 재현 shape에 영향을 줬다. Typeless layout/cast와 typed shader/RT/UAV capability를 분리해야 하며, 96-bit R32G32B32_FLOAT alias나 BC write capability를 허위 광고하면 안 된다는 계약은 그대로 유효하다. Current fixture 지원과 tier는 CORE를 따른다.

## 2026-09-19 DLL·Streamline 관찰

당시 bounded import/static 분석:

- EXE/UnityPlayer/GameAssembly는 D3D12/DXGI를 static import하지 않았다. Unity는 LoadLibrary/GetProcAddress의 D3D12CreateDevice/GetDebugInterface/SerializeRootSignature 이름과 DXGI Factory2→1→base fallback을 가졌다.
- sl.common.dll은 D3D12SerializeRootSignature·CreateDXGIFactory를 static import했다. Owned exports가 당시 존재했다는 사실과 실제 Streamline loader success는 다르다.
- sl.interposer에는 proxy/resolver 경로, Unity ExecuteDLSS에는 slSetConstants/slDLSSSetOptions/four resource tags/slEvaluateFeature 호출이 있었다. Tags는 color/output/depth/MV, eOnlyValidNow였다.
- sl.dlss의 NGX parameter 문자열은 raw flags·jitter/exposure/masks/subrect의 가능성을 보여줄 뿐 **session의 실제 값/호출 여부**를 증명하지 않았다. nvngx_dlss feature plugin과 nvngx core shim의 export surface도 다르다.
- WinPixEventRuntime은 game-shipped dependency였다. Debug-interface unavailable 처리도 분석됐으나 현재 debug API 구현 판정은 이 옛 static 문서에서 하지 않는다.

### 역사적 vendor-identity 진단

Baseline ZZZ profile은 AMD 1002/7340이었다. 후속 `zzz-nvext-startup-03`은 NVIDIA-compatible 10de/2684/“DXMT Metal NGX” identity만 변경했다. Renderer identity는 바뀌었지만 DLSS plugin은 계속 제외됐다. 당시 `NvAPI_GetLogicalGPUFromPhysicalGPU not implemented`, Streamline getSystemCaps error -3, `sl.dlss not supported on current hardware`, D3D12 NV shader-extension opcode query 미구현이 관찰됐다.

이 결과는 **vendor identity만으로 당시 loader를 해결하지 못했다**는 증거다. 현재 NVAPI root 구현을 확인하지 않고 그 항목을 미구현 priority로 다시 나열하지 않는다. 지원하지 않는 opcode를 지원한다고 광고해서도 안 된다. Registry/NVEXT gate가 있는 현재 staging policy와 실제 게임 loader 선택 관찰은 별개다.

## 확인한 범위와 남은 실제 workload 검증

10월9일 지정 실행에서는 runtime/module identity, shader/root 실패 입력, owned NGX 호출과 MetalFX 도시 출력을 확인했다. 아래 항목은 이 제한된 성공과 별개로 남아 있다.

1. 다른 장면·장시간 실행의 typed view·resource state·queue wait/signal·replay와 오류 경로. Static engine DXR/ExecuteIndirect 문자열은 actual scene usage 증거가 아니다.
2. 전체 호출·queue·native encode·실제 표시를 동일 frame identity로 연결하는 완전한 추적. 현재 NGX 로그·HUD·화면 관찰을 이 추적 전체의 증명으로 확대하지 않는다.
3. 동일 조건의 cold/warm 로딩, frame-time 분포·스터터링·화질 및 physical presentation/HDR signal. 개별 JIT 벤치마크와 GPU texture readback은 이를 대신하지 않는다.

Live 게임 설치/설정/기존 prefix를 수정하는 CLI recipe는 제공하지 않는다. 먼저 [NGX-TEST](NGX-TEST.ko.md)·[TEMPORAL-TEST](TEMPORAL-TEST.ko.md)의 fresh synthetic recipe를 사용한다. 현재 범용 번역기 우선순위는 [CORE 남은 구현 후보](CORE-IMPLEMENTATION-STATUS.ko.md#남은-구현-후보)가 관리하며, 이 역사적 게임의 startup 성공을 전체 프로젝트 완료로 삼지 않는다.
