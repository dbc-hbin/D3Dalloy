# Owned D3D12 NGX frontend 구현

2026-09-19 기준 `build/owned-graphics-20260919/ngx-work`의 pinned DXMT
`7c8dee1c2d73415301ceb7d1fa810861cef4cd67`에 D3D12 NGX frontend를 독립 구현했다.
Prime candidate와 runtime snapshot은 이 작업에서 수정하지 않았다.

공유 ABI는 Prime이 게시한 `src/d3d12/d3d12_interfaces.hpp`를 byte-for-byte 복사해 read-only로 사용한다.
현재 checksum은 `b103d89b67800b4588c935e1332ce378cf280582c39fb6aa3e9291b25dbde758`이다.
Create/Frame descriptor 모두 첫 `UINT StructSize`에 `sizeof(desc)`를 넣고 backend가 크기를 검증한다.
Command-list extension의 `ValidateRecording()`을 Create/Evaluate 양쪽 진입에서 호출하므로 closed/copy/bundle list를
frontend에서 명시적으로 실패시킬 수 있다.

구현 export는 기존 D3D11 frontend를 그대로 둔 채 다음 D3D12 surface를 추가한다.

- `NVSDK_NGX_D3D12_Init`, `Init_Ext`, `Init_ProjectID`, `Init_with_ProjectID`
- `Shutdown`, `Shutdown1`
- `GetParameters`, `AllocateParameters`, `GetCapabilityParameters`, `DestroyParameters`
- `GetScratchBufferSize`, `GetFeatureRequirements`
- `CreateFeature`, `EvaluateFeature`, `EvaluateFeature_C`, `ReleaseFeature`

기존 DXMT D3D11 shim은 historical `0x0BAD000x` failure 값을 사용한다. 이를 수정하면 기존 D3D11 behavior를
바꾸게 되므로 그대로 보존했다. 새 D3D12 exports는 NVIDIA public `NVSDK_NGX_Result` ABI의 실제
`0xBAD00000` prefix를 별도 상수로 사용한다. 예를 들어 InvalidParameter는 `0xBAD00005`,
UnsupportedParameter는 `0xBAD00010`이다.

또한 upstream `NVNGXParameter` class의 virtual order는 현재 NVIDIA `NVSDK_NGX_Parameter` declaration과 다르다.
D3D12에서 이 MinGW vtable을 그대로 반환하지 않는다. `nvngx_d3d12_msvc.cpp`를
`--target=x86_64-pc-windows-msvc -fno-rtti -fno-exceptions`로 별도 compile해 공식 순서의 facade vftable을 만들고,
모든 Set/Get/Reset을 C linkage callback으로 MinGW `ParametersImpl` storage에 전달한다. 따라서 기존 D3D11 object는
변경하지 않으면서 D3D12 Get/Allocate/CapabilityParameters는 Microsoft C++ ABI object를 반환한다.

`Init` 계열은 `ID3D12Device`에서 project-owned `IMTLD3D12DeviceExt`만 QI하고
`SupportsTemporalUpscale`을 확인한다. Metal/WMT/D3DMetal native pointer는 `nvngx.dll`에 노출되지 않는다.
CreateFeature는 command list의 device가 초기화된 동일 COM identity인지 확인하고, node mask를
`ID3D12Device::GetNodeCount()` 범위 안에서 검증한 뒤 `CreateTemporalUpscaleState`에 logical create contract를 넘긴다.

feature handle은 raw backend pointer가 아니다. process-lifetime unique token을 key로 한 mutex-protected registry에서
`shared_ptr<FeatureState>`를 찾는다. Evaluate는 lock 안에서 strong reference를 복사한 뒤 backend를 호출하므로
동시 ReleaseFeature와 겹쳐도 frontend feature/backend COM state가 호출 도중 사라지지 않는다. Release는 registry에서
active entry만 제거한다. token address는 session 중 재사용하지 않아 stale handle이 새 feature로 우연히 재해석되지 않는다.

현재 backend/WMT가 정확히 표현할 수 있는 첫 기능 범위는 captured flags74 경로다.

- backend가 명시적으로 인식하는 `IsHDR`, `MVLowRes`, `DepthInverted`, `DoSharpening`(deprecated),
  `AutoExposure` bit를 create-time 지원 bit로 허용한다. flags74는 이 집합의 LDR subset이다.
- high-resolution MV mode와 `MVJittered`/Alpha/reserved/unknown bit는 명시적
  `NVNGX_RESULT_FAIL_UnsupportedParameter`로 반환한다. `DoSharpening` bit 자체와 별개로 nonzero `Sharpness` scalar는
  현재 WMT/backend가 sharpening operation을 표현하지 않으므로 unsupported다.
- color/output/depth/motion resource는 필수다.
- render width/height가 없으면 create input extent를 사용하며 한쪽만 존재하면 실패한다.
- color/depth/MV/bias/output 및 translucency subrect offset은 현재 0만 지원한다.
- bias-current mask와 transparency mask가 실제 non-null이면 실패한다.
- jitter는 finite 값을 그대로 보존한다.
- MV scale은 missing 시 1이며 명시적 0/nonfinite는 invalid parameter다.
- raw `DLSS.Pre.Exposure=0`은 semantic `1.0`, raw `DLSS.Exposure.Scale=0`도 semantic `1.0`으로 정규화한다.
- non-identity Exposure.Scale와 nonzero Sharpness는 현재 WMT가 표현하지 못하므로 unsupported를 반환한다.
- Reset missing은 false이며 caller 값만 frame descriptor에 기록한다. 새 scaler generation의 첫-execution reset은 backend/queue가 담당한다.

이 strict 범위는 기능을 조용히 무시하지 않는다. flags74 밖의 public MetalFX 확장이 backend/WMT에 추가되면 frontend
거부 조건을 개별적으로 해제해야 한다.

CPU/PE 검증은 `scripts/test-owned-ngx-frontend.py`가 담당한다. pinned llvm-mingw로 `nvngx.cpp`와
`nvngx_d3d12.cpp`를 `-Werror` compile하고, parameter facade는 MSVC target으로 별도 compile한 뒤 기존 owned cross build의 DXGI/util/winemetal import/static library에 함께 링크한다.
GPU나 D3D12 device를 만들지 않는다. 생성 PE에서 기존 D3D11 exports, 새 D3D12 exports, `own-ngxd3d12` literal marker,
shared COM header 동일성, StructSize 설정, `ValidateRecording`, private D3DMetal ABI 부재를 검사한다. MSVC object에는
`??_7OwnedNgxParameterFacade@@6B@` vftable symbol이 실제로 생성되는지 확인하고, D3D12 failure return constant도
공식 `0xBAD0000x` 값을 사용하는지 검사한다. 추가로 official NVIDIA header를 MSVC target으로 compile한 기존
`ngx-smoke-msvc.cpp`의 virtual call offset과 facade `.rdata` relocation을 비교한다. 현재 확인된 대표 slot은
`Set(D3D12Resource*)=0x08`, `Set(float)=0x30`, `Get(D3D12Resource**)=0x48`, `Get(float)=0x70`, `Reset=0x80`이며 모두 일치한다.

이 CPU/PE PASS는 실제 ZZZ/Streamline loader가 owned `nvngx.dll`을 선택한다는 증명이 아니다. Runtime에서는 Prime이
loader identity, D3D12 NGX calls, queue execution과 MetalFX encode를 별도로 관측해야 한다.

`nvngx.dll`과 `nvapi64.dll`은 DXMT NV extension gate를 공유한다. candidate `dxgi.cpp`는
`DXMT_ENABLE_NVEXT=1`일 때만 NVIDIA extension registry와 `NGXCore\\FullPath`를 설치하고,
`nvngx.dll`의 DllMain과 `NvAPI_Initialize`는 `DXMT_NVEXT_GUID` query가 실패하면 정상 초기화되지 않는다.
따라서 stager의 `--with-nvngx` 또는 `--with-nvapi`가 켜진 runtime wrapper는 이 환경을 내부에서 명시적으로
`1`로 설정한다. 두 옵션이 꺼진 기본 runtime은 외부에서 상속된 `DXMT_ENABLE_NVEXT`를 제거한다.
