# D3D12 NGX lifecycle·parameter·subrect 계약

2026-10-02 현재 루트 `src/nvngx/nvngx_d3d12.cpp`의 계약이다. API 목록/입력 제한은 [frontend](D3D12-NGX-FRONTEND.ko.md), backend 기록·실행은 [integration](NGX-INTEGRATION.ko.md), 실행 레시피는 [NGX-TEST](NGX-TEST.ko.md)가 담당한다. 프로젝트 지원·증거 상태는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md)가 기준이다.

## NVIDIA public SDK 기준

`external/nvngx/include/{nvsdk_ngx.h,nvsdk_ngx_params.h,nvsdk_ngx_defs.h}`를 ABI 기준으로 사용한다. Vendored snapshot·법적 문서는 변경하지 않는다.

- `Shutdown1(device)`는 해당 instance, `Shutdown1(nullptr)`와 deprecated `Shutdown()`은 모든 instance를 종료한다.
- `NVSDK_NGX_Handle`은 incomplete opaque type이다. release 뒤 invalid이며 caller가 payload를 dereference하는 계약은 없다.
- SDK API의 caller 동기화 책임은 그대로다. 내부 registry lock·strong reference는 구현 수명 보호이지 API 전체 thread-safety 확장이 아니다.
- Parameter virtual order는 공식 8 Set + 8 Get + Reset을 따르는 Microsoft C++ ABI다.

## Parameter ownership과 init gate

| 함수 | ownership | 종료 |
|---|---|---|
| `GetParameters` | NGX-managed | app의 Destroy를 거부; 마지막 device/global shutdown에서 정리 |
| `AllocateParameters` | app-managed | `DestroyParameters` |
| `GetCapabilityParameters` | app-managed | `DestroyParameters` |

세 allocation API는 valid output storage를 먼저 null로 초기화하고, initialized device가 없는 상태에서 `NotInitialized`를 반환한다. Facade 준비 뒤 registry lock 안에서 init gate와 publication을 함께 처리하므로 shutdown 뒤 새 parameter를 게시하는 옛 check/insert race가 없다. Parameter의 `Reset()`은 저장한 key/value를 지우며 backend command를 취소하지 않는다.

`DLSS.Enable.Output.Subrects`의 capability/managed map 기본값과 create parser fallback은 **0**이다. 이것은 capability grant가 아니라 create-time opt-in이다. Nvidia helper의 `InEnableOutputSubrects` 값을 명시적으로 기록해야 한다.

## Device registration·feature handle

Device registry key는 canonical `IUnknown` identity다. Init마다 별도 bounded registration identity가 있으며 Create는 다음 두 시점을 연결한다.

1. initialized list device와 registration을 검증해 strong reference를 보관한다.
2. backend state를 registry lock 밖에서 만든다.
3. 게시 직전 같은 COM identity와 **같은 registration identity**가 남았는지 확인한다.

같은 device를 shutdown/re-init해도 registration은 달라져 in-flight old Create는 `NotInitialized`로 끝난다. 단순 COM pointer 비교만으로 옛 SDK session에 feature를 게시하지 않는다.

Handle은 monotonic `uintptr_t` opaque token이다. permanent `HandleToken` heap vector는 없다. Release/global shutdown은 live feature storage를 제거하지만 counter를 되돌리지 않는다. Stale token이 후속 feature에 alias되지 않고 counter 고갈은 실패다. Evaluate는 registry에서 strong reference를 얻은 뒤 backend를 호출한다. **기록된 frame/state/resource의 실행 수명은 public handle의 수명보다 길 수 있다.**

non-null Shutdown1은 해당 device의 feature를 제거한다. 다른 device가 남아 있으면 공통 parameter map은 유지하고, 마지막 device 종료 시 정리한다. Global shutdown은 모든 device·feature·parameter를 정리한다. 이미 기록되어 queue가 retain한 명령을 무효화한다는 뜻은 아니다.

## Output subrect 계약

- `OutputSubrectsEnabled=0`: nonzero OutputBaseX/Y는 frontend `UnsupportedParameter`다.
- `OutputSubrectsEnabled=1`: output base를 backend frame descriptor에 그대로 전달한다. frontend는 추측해 clamp하지 않는다.
- backend는 `base + logical output`이 backing extent 안에 들어가는지 overflow-safe subtraction으로 검사하고 범위를 벗어나면 `E_INVALIDARG`를 반환한다. Frontend mapping은 `InvalidParameter`다.
- 실제 scaler 출력은 logical-size private scratch에 쓰고, 요청 rectangle만 app output의 base로 복사한다. rectangle 밖은 변경하지 않는다.
- 공식 **input** color/depth/motion/bias/translucency offsets도 지원하며 각 resource view 안의 active rectangle을 독립 검사한다. Output의 create-time opt-in과 input bounds는 다른 계약이다([입력 범위](D3D12-NGX-FRONTEND.ko.md#현재-입력-범위)).

136×136 backing / 128×128 logical의 X=8은 유효, X=9는 범위 초과다. 이 경계는 **record-time contract fixture**가 검사한다. 실제 nonzero output rectangle의 GPU readback/sentinel은 temporal fixture의 `(3,5)` case가 담당한다.

## Contract fixture의 판정

소스는 `graphics-translator/tests/owned-d3d12-ngx-contracts.cpp`와 `owned-d3d12-ngx-contracts-msvc.cpp`다. 후자는 공식 header를 직접 include하며 main PE는 C helper를 통해 virtual ABI를 소비한다. `nvngx.dll`을 static import하지 않고 system32에서 동적 load하며 owned marker와 backend identity를 확인한다.

Exact result를 요구하는 case:

1. Init 전 Allocate/GetCapability/GetParameters: NotInitialized + null output.
2. 두 distinct D3D12 device Init: capability/managed map의 subrect default 0.
3. disabled feature의 nonzero output base: UnsupportedParameter.
4. enabled feature의 X=8: Success, X=9: InvalidParameter.
5. 64회 create/release: token 재사용 없음, stale Release는 FeatureNotFound.
6. 양 device에 feature를 남긴 뒤 Shutdown1(nullptr): 모든 allocation은 NotInitialized + null, 양 handle 모두 FeatureNotFound.
7. 첫 device만 re-init: Allocate/Destroy 성공, 두 번째 device list의 Create는 NotInitialized, 첫 device per-device Shutdown은 성공.

최종 marker는 `OWNED_D3D12_NGX_CONTRACTS_PASS`다. **이 fixture 자체는 Evaluate GPU output을 검증하지 않는다.** Lifecycle/subrect record 계약 PASS와 [NGX 8-frame GPU fixture](NGX-TEST.ko.md)는 서로 다른 증거다.

## 2026-09-19 보정 이력

Frozen `graphics-translator/patches/0008-ngx-contracts.patch`는 `0007-queue-order.patch` 뒤의 당시 candidate에 적용하는 incremental provenance다. 현재 루트에 다시 적용하는 절차가 아니다.

- 당시 DXMT base: `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`
- 보정 전 frontend SHA-256: `8fae693026f44cab7906e7a2c42c334fd60d52b4f756d2907bdb96af8f8c628d`
- 당시 0007 SHA-256: `33ffa6c19792e7aa9b855a806486182bd19f8c11661e69a25ba4e05788fa9dad`
- 당시 보정 후 frontend SHA-256: `f2bda509d271105eee4faef5e1183092a98b8609a6bf229ce43e568da01feede`

그 작업의 Meson/COFF/PE compile·export 검사는 build-only 관찰이었다. 옛 `build/owned-completion-20260919/ngx-contract-*` 경로는 현재 retained artifact를 보장하지 않는다. Shutdown-null/token leak/subrect 미구현을 **현재 defect로 다시 나열하지 않는다**. 현재 proof 경계와 유실/retained 구분은 [CORE 검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다.
