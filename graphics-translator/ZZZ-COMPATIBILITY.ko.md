# ZZZ workload의 역사적 static·runtime 관찰

## 현재 문서의 역할

이 문서는 **2026-09-18~19의 특정 ZZZ 설치·당시 runtime 관찰 기록**이다. 현재 프로젝트의 완료 목표나 지원 matrix가 아니다. 현재 목표는 최신 공개 D3D12/DXGI/shader 의미를 구현하는 범용 자체 번역기와 ARM64 Wine/FEX 기반 Rosetta 제거다([CORE 목표](CORE-IMPLEMENTATION-STATUS.ko.md#개발-목표와-우선순위)). ZZZ는 여러 실제 workload 중 하나다.

현재 compiler는 DXMT airconv·LLVM·자체 AIR 경로다. Apple Metal Shader Converter, SPIR-V, GPTK/D3DMetal compiler/runtime fallback을 현재 제품 경로로 남기지 않는다. 아래 Apple SDK·dxil-spirv 실험은 **당시 선택과 CPU/GPU 실험의 역사**이며 현재 지원 증거가 아니다. 공개 Metal/MetalFX 사용은 자체 compiler와 양립한다.

현재 D3D12 NGX와 temporal COM 경로는 구현됐고 Metal 4 named fixture acceptance도 관찰됐다. 옛 FL11_0/OPTIONS5 미구현/DXIL E_NOTIMPL 표를 현재 support로 읽지 않는다. 현재 선언·검증 범위는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태), NGX current integration은 [NGX-INTEGRATION](NGX-INTEGRATION.ko.md)가 기준이다.

**현재 루트로 실제 ZZZ 렌더링·Streamline loader 선택이 성공했다는 새로운 관찰은 없다.** Synthetic GPU readback, DLL load, 창 생성, vendor identity 변경만으로 게임 PASS를 선언하지 않는다. Physical compositor/HDR scanout도 미검증이다. 이번 문서 개편은 게임·GPU를 실행하지 않았다.

아래 `build/owned-*`/`build/dlss-*` 경로·hash는 당시 식별자다. Build 삭제로 유실된 옛 logs/JSON/PE를 현재 retained artifact처럼 표현하지 않는다. 관찰 기록은 유지하되 [CORE 증거 ledger](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)의 recovery retained proof와 분리한다. Frozen patch/upstream lock/provenance는 변경하지 않는다.

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

## 아직 필요한 실제 workload acceptance

다음은 **계획/미관찰 acceptance**이며 이번 작업에서 실행한 결과가 아니다.

1. 현재 root compiler·binding·runtime의 hash를 고정하고 원본과 분리한 runtime/game/prefix에서 PSO/resource 오류와 실제 content 출력을 함께 관찰한다.
2. Streamline adapter 판단 → loaded owned nvngx identity → public D3D12 NGX calls → queue execution → Metal4FX encode → 같은 frame output을 연결한다.
3. 필요한 실제 typed view·resource states·queue wait/signal·replay shape를 기록한다. Static engine DXR/ExecuteIndirect 문자열을 actual scene usage로 간주하지 않는다.
4. Physical presentation/HDR signal·performance는 GPU texture readback과 다른 관찰을 요구한다.

Live 게임 설치/설정/기존 prefix를 수정하는 CLI recipe는 제공하지 않는다. 먼저 [NGX-TEST](NGX-TEST.ko.md)·[TEMPORAL-TEST](TEMPORAL-TEST.ko.md)의 fresh synthetic recipe를 사용한다. 현재 범용 번역기 우선순위는 [CORE 남은 구현 후보](CORE-IMPLEMENTATION-STATUS.ko.md#남은-구현-후보)가 관리하며, 이 역사적 게임의 startup 성공을 전체 프로젝트 완료로 삼지 않는다.
