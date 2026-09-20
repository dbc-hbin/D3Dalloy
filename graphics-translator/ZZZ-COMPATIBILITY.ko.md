# ZZZ actual-game → owned DXMT 호환성 검증

## 현재 구현 방향

사용자 결정에 따라 공개 Apple Metal Shader Converter 기반 구현은 중단했다. 목표는 DXMT의 `airconv`·LLVM·AIR 생성부를 재사용·확장한 자체 DXIL→Metal 컴파일러와 DX12/DXR 런타임이다. Apple MSC 또는 GPTK/D3DMetal을 제품의 기본 compiler나 fallback으로 남기지 않는다. 공개 Metal·MetalFX API 사용은 이 compiler 의존성 제거와 별개다.

아래 SDK 실험과 과거 runtime의 결과는 역사적 증거이며 자체 compiler 지원을 증명하지 않는다. DX12 리소스·동기화·AS 구현과 유효한 GPU 재현 입력은 보존하되, SDK 전용 descriptor·reflection·SBT 계약은 자체 shader/runtime 계약으로 교체한다. 자체 경로의 실제 GPU 실행과 격리 ZZZ 렌더링이 확인되기 전에는 호환성 완료로 보고하지 않는다.

초기 정적 preflight와 2026-09-19의 격리 actual-game 실행을 구분해 기록한다. 초기 preflight는 읽기 전용이었다. 후속 실행은 게임·prefix·owned runtime의 APFS 복제본만 사용했으며 원본 게임, live prefix, 설치된 런타임은 변경하지 않았다. 실행 창 생성이나 DLL 로딩은 게임 호환성 PASS가 아니다.

현재 접근 가능한 `/Applications/Zenless Zone Zero`의 `ZenlessZoneZero.exe` SHA-256은 `bd809b00609e029f114f8213fc248b5fd2d7466a3251e5ab8beaa2ca15387faf`, `UnityPlayer.dll`은 `c9dec60f119fde57cc5aba605c0a84fe001aebf8ab07af43f9f82fd7ec0bd2f6`이다. 이는 기존 `DLSS-TRANSLATOR-ZZZ-STATIC.ko.md`가 분석한 설치와 일치한다. 이번 shader container 분석에 사용한 `globalgamemanagers.assets`는 70,601,448 bytes, SHA-256 `cbfa24902f86f2fa87f1ebd183beb2685d556430bb5dc454dae613f1537d4ac8`이다.

## 실제 D3D12 device 생성 계약

`UnityPlayer.dll`의 D3D12 초기화 bounded disassembly는 `build/owned-graphics-20260919/zzz-preflight/unity-d3d12-init.disasm.txt`에 남겼다.

Unity는 `D3D12CreateDevice`를 먼저 `D3D_FEATURE_LEVEL_12_0`으로 호출한다. 이 호출이 실패하면 같은 adapter에 `D3D_FEATURE_LEVEL_11_0`으로 다시 호출한다. 따라서 이 Unity build에서 base D3D12 renderer가 받아들이는 최소 fallback level은 **11_0**이고, 12_0은 먼저 시도하는 선호 경로다.

device 생성 뒤 같은 초기화 함수에서 확인되는 `CheckFeatureSupport` 호출은 다음 네 종류다.

| Query | Unity 처리 | 현재 owned DXMT |
| --- | --- | --- |
| `D3D12_FEATURE_D3D12_OPTIONS` (0) | 60-byte struct. 실패하면 D3D12 초기화 실패 경로로 감 | 구현됨 |
| `D3D12_FEATURE_FEATURE_LEVELS` (2) | 11_0, 11_1, 12_0, 12_1 네 level 요청. 실패하면 기록값을 11_0으로 fallback | core-contracts 수정 후 최대 11_0 반환 |
| `D3D12_FEATURE_D3D12_OPTIONS5` (27) | 성공하고 `RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 (0xb)`일 때만 RT device 경로 활성. 실패/낮은 tier는 계속 진행 | 미구현, `E_NOTIMPL` |
| `D3D12_FEATURE_D3D12_OPTIONS7` (32) | MeshShaderTier가 있을 때만 mesh device 경로 활성. 미지원은 계속 진행 | 구현됨. Mesh/Sampler Feedback NOT_SUPPORTED |

`OPTIONS5` 미구현은 이 call site에서 **base renderer 시작 blocker가 아니다**. Unity가 query 실패를 허용하고 ray-tracing path만 활성화하지 않는다. 이 설치에 DXR 관련 Unity 엔진 문자열은 존재하지만, 그것만으로 ZZZ content가 해당 세션에서 DXR을 요구한다고 볼 수 없다.

반대로 `D3D12_OPTIONS`는 이 초기화 함수에서 실패 허용 경로가 아니다. owned 쪽이 이 query를 구현하고 있다는 점은 base startup에 직접 필요하다.

## feature-level 생성 계약 수정

초기 소스에는 FL12_0 생성 요청을 잘못 성공시키면서 내부 level은 11_0, query max는 11_1로 반환하는 불일치가 있었다. `0009-d3d12-core-contracts.patch` 이후 생성·query 상한은 `kOwnedMaxFeatureLevel = 11_0`으로 맞춰졌다. 아래 actual-game 기준 실행에서도 Unity는 `Direct3D 12.0 [level 11.0]`을 기록했다. 이 수정은 지원하지 않는 FL12 기능을 광고하지 않는다는 뜻이지, ZZZ의 모든 shader/resource 계약을 지원한다는 뜻은 아니다.

## 실제 shader bytecode: SM5와 DXIL이 함께 shipped됨

단순 `strings` 결과가 아니라 DXBC container header, blob offset/size, chunk boundary를 검증해 bounded asset을 훑었다. raw 결과는 다음 파일에 있다.

- `build/owned-graphics-20260919/zzz-preflight/shader-containers.json`
- `build/owned-graphics-20260919/zzz-preflight/legacy-shader-versions.json`
- `build/owned-graphics-20260919/zzz-preflight/dxil-program-versions.json`

`UnityPlayer.dll` 자체에서는 structurally valid DXBC container 36개가 확인됐고 모두 legacy `SHDR`/`SHEX` 계열이다. DXIL chunk는 없었다.

실제 게임 `globalgamemanagers.assets`에서는 structurally valid DXBC container 772개가 확인됐다.

| 실제 container 내용 | 개수 |
| --- | ---: |
| legacy `SHEX`, compute, Shader Model 5.0 | 352 |
| `DXIL`, type 5 compute, Shader Model 6.0 | 356 |
| `DXIL`, type 5 compute, Shader Model 6.5 | 56 |
| `DXIL`, type 6, Shader Model 6.5 | 8 |

DXIL version token 해석은 이 Wine tree의 `libs/vkd3d/libs/vkd3d-shader/dxil.c`와 동일하게 `type = version_token >> 16`, `major = (version_token >> 4) & 0xf`, `minor = version_token & 0xf`를 사용했다. type 6의 세부 의미는 이 preflight에서 이름을 추정하지 않는다.

따라서 ZZZ가 **실제 DXIL bytecode를 shipped한다**는 것은 확정이다. 동시에 같은 bounded asset에 SM5.0 compute bytecode도 352개 존재한다. 개수만으로 각 SM6 shader마다 완전한 SM5 fallback variant가 있다고 증명할 수는 없다.

## owned DXMT의 DXIL 처리 상태

아래 기준 실행의 owned runtime은 `MTLD3D12PipelineState::InitializeShader()`에서 `DXBC_DXIL` chunk를 `E_NOTIMPL`로 거부한다. graphics와 compute PSO가 모두 이 helper를 사용한다. 별도 DXIL compiler 작업 트리의 존재만으로 이 실행본에 DXIL 지원이 통합됐다고 볼 수 없다.

즉 **이 기준 runtime에서는 actual ZZZ session의 DXIL PSO 생성이 hard fail**한다. 후속 compiler 실험과는 구분해야 한다. 이건 일반적인 “SM6 지원 필요” 추측이 아니라 actual game asset과 해당 PSO source가 직접 만드는 blocker다.

2026-09-19 actual-game 기준 실행은 이 조건을 실제로 밟았다. 최초 SHEX VS/PS는 성공했지만 이어지는 DXIL VS/CS는 `hr=2147500033` (`0x80004001`, `E_NOTIMPL`)로 실패했다. 따라서 이 실행에서 DXIL은 **확인된 시작 차단점**이며, SM5 fallback이 자동으로 모든 셰이더를 대체한다는 가정은 성립하지 않는다.

### 격리 actual-game 기준 실행

증거: `build/owned-continuation-20260919/evidence/zzz-baseline-startup-02/`. `result.json`의 runtime manifest SHA-256은 `5668fe08a076902c7b61c5c97d67dda867a0776c93bc23f6b499f11af4e0eb29`이다. 실행본은 기존 `wine-bundles-02`를 복제한 기준 런타임이며 후속 DXIL/NGX/query 수정 후보가 아니다.

- 기존 prefix 복제본을 이 runtime의 `wineboot.exe -u`로 준비한 뒤 builtin `d3d12.dll`, `DXGI.dll`, `winemetal.dll`과 private runtime의 `winemetal.so`가 로드됐다.
- `player.log`는 D3D12 level 11.0 초기화와 graphics/compute PSO의 `80004001` 실패를 기록한다. `window.png`의 게임 콘텐츠는 검은 화면이다.
- `ZenlessZoneZero_d3d12.log`는 최초 16개 shader probe를 기록한다. 아래 6종 DXIL 입력은 실제 trace SHA-1과 원본 asset에서 추출한 전체 container SHA-1이 일치한다.
- DXMT built-in ZZZ profile은 `dxgi.customVendorId=1002`, `customDeviceId=7340`, `customDeviceDesc=AMD Radeon Pro 5300M`을 적용했다. NVEXT 활성화만으로 이를 덮어쓰지 않는다. Streamline은 이 실행에서 unsupported device 및 DLSS feature load 실패를 기록했다.
- 120초 관측 뒤 해당 private prefix의 wineserver만 종료했다. `timed_out=true`, cleanup exit 0은 관측 종료 정보이지 정상 게임 종료나 호환성 PASS가 아니다.

| Stage | Bytes | Runtime SHA-1 | 추출된 원본 |
| --- | ---: | --- | --- |
| VS | 2476 | `b7ca45bd2d9c97131a6f26b933b480d5c23cb92e` | `Hidden/Universal Render Pipeline/DepthPiramid` |
| VS | 1812 | `7d97cd42b457596c9e5bbdb28beb3626978ddebb` | `Hidden/Universal Render Pipeline/ScreenSpaceReflection` |
| VS | 2996 | `d84d5ac214565aaeb45d9eae3d86cca8d6bd1b1c` | `Hidden/BlitCopy` |
| CS | 1764 | `ede9c0e31ec1b771e27f4903e404c30763ef368a` | `globalgamemanagers.assets`, offset 69605416 |
| CS | 6312 | `76c42ee4d7bd030f7afb83947a93b8c1e05d42e9` | `globalgamemanagers.assets`, offset 68678668 |
| CS | 7424 | `9c3c1108a1b79c3cb290dd22ca92df0b8e9edbfe` | `globalgamemanagers.assets`, offset 68685256 |

추출 provenance는 `build/owned-continuation-20260919/actual-startup-shaders/{manifest,compressed-manifest}.json`에 있다. VS는 UnityPy 1.25.3과 LZ4로 platform 25 Shader blob을 풀어 찾았다. 게임 bytecode는 로컬 ignored build 입력이며 소스 배포물에 포함하지 않는다. 이 6개 입력의 독립 `dxil-spirv --validate` 변환은 성공했지만, 이는 GPU 실행·D3D12 binding·Metal 출력의 검증을 대체하지 않는다.

### 후속 DXIL GPU 증거와 실제 root signature

공개 SDK 전환 전의 별도 DXIL 경로는 실제 GPU에서 root-descriptor compute 쓰기, VS/PS varying·derivative 및 depth readback을 통과했다. 증거는 `build/owned-continuation-20260919/dxil/evidence/gpu-baseline/`이다. 이 실행은 descriptor table을 여전히 거부하므로 전체 ZZZ 호환성 증거가 아니다.

그 경로에 한정된 opt-in root blob 관찰만 추가한 actual-game 실행은 `build/owned-continuation-20260919/evidence/zzz-root-contract-startup-04/`에 있다. runtime manifest SHA-256은 `2c8ac2bbf4e28a9a65209d18bf03b4364f9117b42ac0282bb7e292a4ff1d9b44`다. 실제 DXIL VS `7d97cd42...`는 `hr=0`으로 진행됐지만 뒤의 PS와 다른 binding 경로는 계속 실패했다.

`root-contract/captured-root-contracts.txt`의 16개 serialized DXBC/RTS0 기록은 모두 descriptor-table 기반이었다. CBV b0, SRV t0..t5, UAV u0..u2 및 sampler s0..s1 범위가 포함된다. 이는 관찰된 16개 PSO의 계약이며 게임 전체에 root constants·static sampler가 없다는 뜻은 아니다. 새로 드러난 PS 5개도 실제 bytecode SHA-1로 추출했고, `actual-startup-shaders/compressed-manifest-run04.json`에 provenance를 남겼다.

### 공개 Apple compiler 선택과 RT 경계

2026-09-19 사용자는 Apple **공개 Metal Shader Converter SDK 사용을 승인**했다. D3D12·NGX·RT runtime은 자체 구현하며, 비공개 D3DMetal runtime fallback은 사용하지 않는다. 기존 오픈소스 DXIL compiler의 실험 증거는 보존하되 정규 구현은 공개 SDK로 전환한다.

- 공식 Apple Developer의 Metal Shader Converter 3.0 패키지이며 실제 CLI 버전은 3.0.6이다. Apple 서명·trusted timestamp를 확인하고 시스템 설치 없이 로컬 prefix로 추출했다.
- 원본 SDK `.dylib`, 헤더, 동봉 라이선스·notices는 `upstream.lock.json`으로 고정한다. GPTK framework 안의 동명 라이브러리를 대신 사용하지 않는다.
- 동봉 계약 2.B는 Apple 시스템에서 shader conversion 목적의 dynamic-library 사용·배포를 허용한다. 2.E의 변경 금지에 맞춰 SDK 원본을 수정·thin·재서명하지 않는다. 자체 wrapper의 재배치 처리는 별개다.
- 실제 RayTracingShader 8개 container의 18개 raygen/miss entry 중 14개는 SDK에서 metallib와 export를 생성했다. 이것은 CPU compiler 증거이며 AS·shader table·DispatchRays의 GPU 실행 증거가 아니다. material hit shader와 실제 scene 경로도 별도로 확인해야 한다.
- RTXGI의 나머지 4개 entry는 FP64 UNORM24 변환에서 실패했다. 계수는 `1/(2^24-1)`이며 `2^-24`가 아니다. 예를 들어 입력 8388607에서 원래 double→float 결과는 `0.4999999701976776`, 단순 float 곱은 `0.5`다. SDK를 수정하거나 부정확한 float 곱으로 대체하지 않고 자체 DXIL 사전 변환의 정확성을 검증한다.

### NVIDIA 호환 identity만으로는 DLSS 로딩이 해결되지 않음

동일 기준 runtime으로 `dxgi.customVendorId=10de;dxgi.customDeviceId=2684;dxgi.customDeviceDesc="DXMT Metal NGX"`만 명시한 후속 실행을 수행했다. 이는 기존 NVAPI의 AD100/AD102 호환 identity와 맞춘 진단 설정이며 NVIDIA 하드웨어나 DLSS 모델을 제공한다는 뜻이 아니다. 증거는 `build/owned-continuation-20260919/evidence/zzz-nvext-startup-03/`이다.

`player.log`에서 renderer 변경은 확인됐지만 DLSS plugin은 계속 제외됐다. `stderr.log`의 구체적인 앞선 실패는 `NvAPI_GetLogicalGPUFromPhysicalGPU not implemented`와 Streamline `commonInterface.cpp:285[getSystemCaps]`의 error `-3`이다. 뒤이어 `sl.dlss not supported on current hardware`가 기록됐다. 따라서 AMD profile만 바꾸면 게임 NGX 경로가 완성된다고 볼 수 없다. 실제 adapter와 일치하는 NVAPI physical/logical topology 및 후속 capability 계약도 필요하다. 같은 실행에서 `NvAPI_D3D12_IsNvShaderExtnOpCodeSupported` 미구현도 확인됐다. 지원하지 않는 opcode를 지원한다고 반환해서는 안 된다.

## 실제 ZZZ resource format과 corrected format-capability patch

이전 actual-game DLSS translator trace의 `resolved_resources` 이벤트는 synthetic fixture와 별개로 실제 게임 resource parent format을 기록했다. 두 actual run 모두 다음 값을 반복해서 보였다.

- color: DXGI 27 = `R8G8B8A8_TYPELESS`
- output: DXGI 27 = `R8G8B8A8_TYPELESS`
- depth: DXGI 19 = `R32G8X24_TYPELESS`
- motion vectors: DXGI 33 = `R16G16_TYPELESS`

예를 들어 `build/dlss-translator-20260918/actual-game/run-20260918T165912553899Z-translator1/translator.jsonl`의 첫 evaluation은 2256×1272 input / 3840×2160 output에서 위 parent format을 기록했고, `build/nameplate-regression-20260919/actual-game/run-20260918T175709409117Z-translator1-cache-on/translator.jsonl`도 1536×984 / 2624×1696에서 같은 DXGI parent format 조합을 기록했다.

따라서 `0001-format-capabilities.patch`의 typeless spot case는 실제 ZZZ와 직접 관련 있다. 새 `owned-capabilities.cpp`는 최소한 다음을 구분한다.

- `R8G8B8A8_TYPELESS`: texture layout/cast는 허용하되 typeless format 자체에 shader-load/RT/UAV를 허위 광고하지 않음
- `R16G16_TYPELESS`: 같은 typeless 규칙
- `R32G8X24_TYPELESS`: 2-plane depth/stencil parent layout/cast와 typed capability를 분리
- `D32_FLOAT_S8X24_UINT`: DSV/shader-load는 허용하되 RT/UAV/color resolve는 금지
- `R8G8B8A8_UNORM`, `R16G16_FLOAT`: 실제 typed-view 후보에 필요한 sampled texture baseline
- `R32G32B32_FLOAT`: 12-byte vertex attribute를 R32 Metal pixel-format alias 때문에 texture/buffer로 잘못 광고하지 않음
- `BC1_UNORM`: BC 지원 여부는 device runtime capability로 출력하고 buffer/RT/UAV write capability는 금지

actual trace는 parent resource를 증명하지만 모든 typed view descriptor를 기록한 것은 아니다. typed-view 전체를 실제 게임 관측으로 바꿔 표현하지 않는다.

## 실제 DLL import/load surface

bounded PE import summary는 `build/owned-graphics-20260919/zzz-preflight/import-modules.json`에 있다.

`ZenlessZoneZero.exe`, `UnityPlayer.dll`, `GameAssembly.dll`은 `d3d12.dll`이나 `dxgi.dll`을 정적 import하지 않는다. `UnityPlayer.dll`은 `LoadLibraryA`/`GetProcAddress` 경로를 사용하며 실제 binary에 다음 D3D12 이름이 존재한다.

- `D3D12CreateDevice`
- `D3D12GetDebugInterface`
- `D3D12SerializeRootSignature`

DXGI factory는 `CreateDXGIFactory2` → `CreateDXGIFactory1` → `CreateDXGIFactory` fallback 계열이 엔진에 있다. owned `dxgi.def`는 세 export를 모두 제공하고 `CreateDXGIFactory`/`1`은 `CreateDXGIFactory2(0, ...)`로 연결한다.

`D3D12GetDebugInterface`는 owned에서 `E_NOINTERFACE`다. Unity binary에는 `D3D12GetDebugInterface is unavailable` 및 debug-interface failure 처리 문자열이 별도로 있어 이 함수의 실제 debug 기능을 base gameplay blocker로 분류하지 않는다.

Streamline 쪽 actual static dependency는 별도다. `/Applications/Zenless Zone Zero/sl.common.dll`은 다음 graphics import를 가진다.

- `d3d12.dll!D3D12SerializeRootSignature`
- `dxgi.dll!CreateDXGIFactory`

요약은 `build/owned-graphics-20260919/zzz-preflight/sl-common-graphics-imports.json`에 있다. 현재 owned `d3d12.def`/`dxgi.def`는 둘 다 export하고 실제 구현도 존재한다. `sl.interposer.dll` 자체는 이 두 DLL을 정적 import하지 않지만 기존 ZZZ static 분석에서 D3D12/DXGI proxy export와 Streamline resolver 경로가 확인됐다.

`UnityPlayer.dll`은 `WinPixEventRuntime.dll`도 정적 import한다. 현재 게임 root에 해당 DLL이 실제로 존재하고 SHA-256은 `81adcfd8253c3489be720da7e30f16004dc9a1f02a8b418c6c3aef4993032e6d`이다. 이는 owned D3D12가 새로 제공해야 하는 export가 아니라 game-shipped dependency다.

## CPU-only owned capability PE

`graphics-translator/tests/owned-capabilities.cpp`를 다음 compiler로 Windows PE로 만들었다.

```sh
/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
  --target=x86_64-w64-mingw32 \
  -std=c++17 -O2 -Wall -Wextra -municode -static \
  graphics-translator/tests/owned-capabilities.cpp \
  -o build/owned-graphics-20260919/zzz-preflight/owned-capabilities.exe \
  -ld3d12 -ldxgi -luuid
```

빌드는 성공했고 파일은 `PE32+ executable (console) x86-64, for MS Windows`다. import table의 graphics entry는 `d3d12.dll!D3D12CreateDevice`와 `dxgi.dll!CreateDXGIFactory2`다. `OwnedGraphicsBackendId`는 의도적으로 static import하지 않고 실행 시 `GetProcAddress`로 찾으며 값이 정확히 `owned-graphics-d3d12`가 아니면 실패한다.

이 PE는 실행 시 다음을 한 번에 관찰한다.

1. 실제 로드된 `d3d12.dll` path와 owned marker.
2. `CreateDXGIFactory2` adapter → `D3D12CreateDevice(..., FL11_0)`.
3. required `D3D12_OPTIONS`, `FEATURE_LEVELS`와 reported Shader Model.
4. optional `OPTIONS5`, `OPTIONS7` 결과.
5. reported max FL보다 높은 FL12_0 support probe가 잘못 성공하는지.
6. 위 ZZZ-related typeless/typed/depth format과 96-bit/BC negative spot case.

이번 worker는 PE를 **실행하지 않았다**. `owned-capabilities.pe.txt`는 CPU-side PE header/import 검증 결과다.

## source-backed known / unknown

확정된 항목:

- ZZZ UnityPlayer는 FL12_0을 먼저 요청하고 실패 시 FL11_0으로 fallback한다.
- base init에서 `D3D12_OPTIONS` 성공은 필요하다. `FEATURE_LEVELS` failure에는 11_0 fallback이 있다.
- `OPTIONS5` DXR 1.1과 `OPTIONS7` mesh support는 optional detection이다.
- 초기 requested feature-level 불일치는 core-contracts 패치로 수정됐고, 기준 실행은 FL11_0으로 초기화됐다.
- actual game asset은 SM5.0 compute와 DXIL SM6.x bytecode를 모두 shipped한다.
- 기준 runtime은 실제 선택된 DXIL VS/CS를 `E_NOTIMPL`로 거부한다. 검은 게임 창과 PSO 실패가 함께 관측됐다.
- actual prior DLSS evaluation에서 color/output/motion/depth parent는 위 typeless DXGI formats였다.
- actual Streamline `sl.common.dll`이 요구하는 D3D12/DXGI static exports는 현재 owned DLL에 존재한다.

현재 증거로 모르는 항목:

- 후속 DXIL compiler와 전체 binding 경로를 통합한 runtime에서 실제 ZZZ 렌더링이 성공하는지.
- 352개 SM5 compute와 420개 DXIL container가 kernel별로 완전한 fallback pair를 이루는지.
- Unity 엔진에 포함된 DXR/ExecuteIndirect code가 이 게임 세션의 scene/UI에서 실제 호출되는지. 기존 이름표 분석도 ExecuteIndirect actual-game 사용 여부는 unknown으로 남겼다.
- actual game의 모든 typed SRV/UAV/RTV/DSV format. 기존 actual trace가 직접 기록한 것은 parent DXGI resource format이다.

## 다음 통합 실행의 검증 범위

DXIL compiler·descriptor binding을 실제 runtime에 통합한 뒤 같은 private 게임/prefix 경로로 PSO 오류와 화면을 함께 확인한다. 이 기준 실행에서 확인된 요구사항에는 typed-buffer SRV/UAV, CBV, vertex input 및 graphics-stage varying 연결이 포함된다. CPU 변환 성공만으로 해당 GPU 계약이 완성됐다고 판단하지 않는다.

DLSS/MetalFX 검증은 Streamline의 adapter 지원 판단, 공개 NGX ABI, Create/Evaluate/Release, 실제 temporal 출력까지 이어져야 한다. NVEXT enable이나 직접 COM temporal fixture만으로 실제 게임의 NGX 성공을 대신하지 않는다.

DXR/mesh의 optional detection은 base 초기화가 실패를 허용한다는 증거일 뿐이다. 전체 ZZZ 기능 목표에서 레이 트레이싱을 제외하거나 지원하지 않는 capability를 성공으로 광고하는 근거가 아니다.
