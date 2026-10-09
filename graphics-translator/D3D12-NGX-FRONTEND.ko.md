# D3D12 NGX frontend API

현재 루트 `src/nvngx`를 기준으로 한 API 안내다(2026-10-02). D3D12 NGX는 **구현되어 있다**. 전체 지원 선언·완료 범위는 [현재 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태), 실행 증거는 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)가 기준이다. 기존 D3D11 frontend의 기능 확대는 이 문서의 범위가 아니다.

## 구현과 export

| 현재 파일 | 역할 |
|---|---|
| `src/nvngx/nvngx.cpp` | 기존 D3D11 exports·DLL 초기화·NV extension gate |
| `src/nvngx/nvngx_d3d12.cpp` | D3D12 device/parameter/feature registry, public result mapping, create/frame parsing |
| `src/nvngx/nvngx_d3d12_msvc.cpp` | Microsoft C++ ABI parameter facade와 C linkage storage bridge |
| `src/nvngx/meson.build` | x64/i686/ARM64/ARM64EC별 MSVC facade object와 MinGW frontend를 같은 builtin DLL에 링크 |
| `src/d3d12/d3d12_interfaces.hpp` | DLL 간 project-owned COM 계약 |

구현된 `NVSDK_NGX_D3D12_` export는 다음 **16개**다.

- `Init`, `Init_Ext`, `Init_ProjectID`, `Init_with_ProjectID`
- `Shutdown`, `Shutdown1`
- `GetParameters`, `AllocateParameters`, `GetCapabilityParameters`, `DestroyParameters`
- `GetScratchBufferSize`, `GetFeatureRequirements`
- `CreateFeature`, `EvaluateFeature`, `EvaluateFeature_C`, `ReleaseFeature`

지원 feature는 `SuperSampling`(id1), `FrameGeneration`(id11), `RayReconstruction`(id13)이다. 실제 backend는 각각 Metal4FX TemporalScaler, owned MetalFX FrameInterpolator, Metal4FX TemporalDenoisedScaler다. `GetScratchBufferSize`는 셋에0을 반환하며 owned backend scratch가 없다는 뜻이 아니다. 나머지 enum의 unsupported inventory와 정확한 입력 경계는 [integration](NGX-INTEGRATION.ko.md#public-non-sr-feature-inventory)을 따른다. Init metadata는 별도 모델/서비스 선택에 쓰이지 않는다. NVIDIA neural model·NGX 전체·게임 loader 지원 선언이 아니다.

D3D12의 success는 `0x00000001`, failure는 NVIDIA public `0xBAD00000` 계열이다. 기존 D3D11 shim의 historical `0x0BAD000x` 값은 변경하지 않는다. 두 결과 공간을 혼용하지 않는다.

## Microsoft parameter ABI

`NVSDK_NGX_Parameter`는 COM이 아닌 C++ virtual interface다. D3D12 호출자에게 MinGW `NVNGXParameter` vtable을 직접 반환하지 않는다. MSVC-target facade가 다음 공식 순서로 storage에 전달한다.

1. Set 8개: `unsigned long long`, `float`, `double`, `unsigned int`, `int`, `ID3D11Resource*`, `ID3D12Resource*`, `void*`
2. Get 8개: 같은 typed 순서
3. Reset

D3D12만 사용해도 D3D11 resource slot은 **공식 ABI의 일부**이므로 보존한다. 공식 SDK snapshot은 `external/nvngx/include/`이고 라이선스는 `external/nvngx/LICENSE.txt`다. Slot 검사에는 공식 header로 컴파일한 consumer object를 사용한다([검증 레시피](NGX-TEST.ko.md#cpupe-abi-검사)).

현재 Meson은 x64/i686/ARM64/ARM64EC에 각각 맞는 MSVC facade target을 선택한다. ARM64X paired helper와 package는 `--with-nvngx`를 양 ABI build/stage에 전달한다. 기존 x64/WoW64 public proof와 10월7일 direct isolated FEX x64 NGX/NGX-contracts proof는 별도이며, 이를 native x86 temporal/Composition 및 전체 migration matrix 성공으로 확대하지 않는다. [현재 실행·증거](../doc/ARM64-FEX-IMPLEMENTATION.md)를 따른다.

## Device와 feature 수명

Init은 `ID3D12Device`의 canonical `IUnknown` identity를 등록하고 `IMTLD3D12DeviceExt::SupportsTemporalUpscale`을 확인한다. Create/Evaluate는 열린 non-copy/non-bundle list와 initialized device를 요구한다. Evaluate는 feature의 device identity와 list device identity도 비교한다.

Feature handle은 backend pointer나 dereference 가능한 payload가 아니다. 재사용하지 않는 monotonic opaque token을 registry key로 사용한다. Release는 live registry entry를 지우고 stale handle은 `FeatureNotFound`가 된다. token counter는 shutdown 때도 reset하지 않으며 공간 고갈 시 재사용 대신 실패한다. Evaluate가 잡은 strong reference와 이미 기록된 backend frame은 Release와 별개로 살아 있다.

Create는 backend state 생성 후 **같은 Init registration이 아직 유효한지** 다시 확인하고 feature를 게시한다. 중간 shutdown/re-init은 같은 device COM identity라도 옛 registration에 늦게 feature를 게시하지 못한다. Parameter publication도 init gate와 insertion을 같은 registry lock에서 수행한다. 이것이 NVIDIA API의 caller 동기화 책임을 일반 thread-safe API 보장으로 바꾸지는 않는다.

`Shutdown()`과 `Shutdown1(nullptr)`는 전체 device/feature/parameter registry를 비운다. non-null `Shutdown1(device)`는 그 device의 feature와 registration을 지우고 마지막 device가 사라지면 parameter registry도 비운다. Ownership·출력 포인터·subrect의 정확한 계약은 [NGX-CONTRACTS](NGX-CONTRACTS.ko.md)에 있다.

## 현재 SR 입력 범위

- Create extent 네 값은 필수이며 0이 아니어야 한다. creation/visibility node mask는 device node 범위 내의 nonzero 값이어야 한다.
- 허용 create bit: `IsHDR`, `MVLowRes`, `DepthInverted`, `MVJittered`, deprecated `DoSharpening`, `AutoExposure`. MVLowRes off의 high-resolution MV와 jittered MV도 full SR port에서 지원한다. flags74는 `2 | 8 | 64`이며 fixture의 LDR baseline이다.
- SR `AlphaUpscaling` flag128은 independent native alpha history·neutral exposure로 지원한다. Reserved/unknown bit는 `UnsupportedParameter`다. DoSharpening은 별도 RCAS post-operation이며 native descriptor flag가 아니다. HDR bit를 허용한다는 사실만으로 DLSS↔MetalFX transfer-function 동등성을 주장하지 않는다.
- Color/Output/Depth/MotionVector는 필수다. missing과 명시적 null은 구분한다. Optional bias/transparency mask는 full SR port의 reactive/composition 경로에 연결한다.
- render width/height가 둘 다 없으면 create input extent를 사용한다. 한쪽만 있으면 `MissingInput`, 0이면 `InvalidParameter`다.
- 공식 color/depth/MV/bias/translucency subrect base를 전달하고 backend가 각 view의 active rectangle/bounds를 검사한다. **output base는 create-time subrect opt-in을 켠 경우 지원**한다. MV texture subrect base는 accept+ignore하는 MV.Offset 값 bias와 다르다.
- jitter는 finite 값 그대로다. MV scale은 missing 시 1, 명시적 0/nonfinite는 invalid다.
- raw `DLSS.Pre.Exposure=0`/missing은 semantic1로 정규화한다. `DLSS.Exposure.Scale`은 missing·raw0·finite nonnegative를 허용하되 수치 효과는 무시한다. 성공한 float getter의 음수/nonfinite는 InvalidParameter다. Optional float getter 실패(missing/지원하지 않는 variant)는 기존 fallback 정책을 유지한다. `MV.Offset.X/Y`도 missing 또는 모든 finite 값을 허용·무시하며 성공한 float getter의 nonfinite는 InvalidParameter다. Nonzero Sharpness는 sharpening flag가 꺼져 있으면 unsupported다. P1의 accept+ignore 계약이며 Exposure 곱셈·MV 값 bias 지원이 아니다([완료 scope·GPU/replay 증거](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)).
- missing Reset은 false다. first execution·generation transition reset은 backend가 실행 시점에 결정한다.
- AutoExposure off의 optional exposure는 null 또는1×1 R16/R32를 지원한다. R32는 실행/replay마다 GPU numeric R16 conversion을 수행하고 auto는 supplied exposure를 무시한다([구현·proof](NGX-INTEGRATION.ko.md#record-time-resourcegeneration-준비)). Exposure.Scale accept+ignore와 이 live resource format/conversion 지원은 별개다.

Backend resource/scale/manual-exposure 제한과 Metal 4 encoding은 [NGX-INTEGRATION](NGX-INTEGRATION.ko.md), source-backed 남은 의미·portability 경계는 [API-REVIEW](API-REVIEW.ko.md)에 정리한다.

## 증거와 역사

2026-09-19 독립 `ngx-work` 구현과 frozen frontend/contract patch는 provenance다. 당시 작업 트리 복사·ABI checksum을 **현재 루트 빌드 절차로 사용하지 않는다**. `build/` 삭제 전 CPU/PE·GPU 관찰은 역사적 증거이며 현재 artifact 보유와 다르다. 현재 코드는 루트에서 빌드하고 [NGX-TEST](NGX-TEST.ko.md)의 fresh staging/runner를 사용한다.

Synthetic fixture는 system32 owned DLL을 직접 요청한다. **실제 게임/Streamline loader가 같은 DLL을 선택한다는 관찰은 아직 없다.** 게임 DLL 교체, live prefix 변경 또는 vendor identity만으로 loader 성공을 가정하지 않는다.
