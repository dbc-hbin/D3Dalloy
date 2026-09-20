# Owned D3D12 NGX → MetalFX integration

이 문서는 owned graphics translator에서 빠져 있는 D3D12 NGX 경로를 현재 소스에 맞춰 정의한다. 범위는 NGX frontend, D3D12 command-list 기록 경계, MetalFX temporal scaler command encoding과 lifetime이다. 실제 게임/GPU 실행 결과를 이 문서의 정적 결론으로 간주하지 않는다.

## 확인한 기준

- DXMT reference/candidate base commit: `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`.
- candidate `src/nvngx/nvngx.cpp`는 reference와 SHA-256까지 동일하다: `b7a09375699c8d6de89a2940e75fb9dd8505359bef856dad9036a9f025531d8d`.
- candidate D3D12 queue/list에는 owned submission/tracing 작업이 별도로 진행 중이므로 이 분석에서는 수정하지 않았다.
- local NVIDIA public headers, `d3dmetal-pso-cache/dlss-contract.*`, `dlss-metalfx.*`, ZZZ static analysis와 flags74 fixture는 모두 read-only reference로 사용했다.

## 현재 upstream DXMT에 D3D12 NGX는 없다

`src/nvngx/nvngx.cpp`의 exported frontend는 전부 D3D11이다. 현재 candidate install의 `x86_64-windows/nvngx.dll`도 `NVSDK_NGX_D3D11_*`만 export하고 `NVSDK_NGX_D3D12_*`는 하나도 export하지 않는다. 반면 `nvngx.hpp`와 `ParametersImpl`은 이미 `ID3D12Resource*` parameter slot을 갖고 있어 parameter container 자체는 D3D12 resource를 저장할 수 있다.

현재 D3D11 path는 다음 DXMT 내부 ABI에 직접 의존한다.

1. `NVSDK_NGX_D3D11_CreateFeature`가 `ID3D11DeviceContext`를 `IMTLD3D11ContextExt1`로 QI한다.
2. `CheckFeatureSupport(MTL_FEATURE_METALFX_TEMPORAL_SCALER)`로 MetalFX availability를 확인한다.
3. `EvaluateFeature`가 Color/Output/Depth/Motion/Exposure를 `ID3D11Resource*`로 꺼낸다.
4. `MTL_TEMPORAL_UPSCALE_D3D11_DESC`를 채우고 `IMTLD3D11ContextExt::TemporalUpscale`을 호출한다.
5. D3D11 context 내부가 `TemporalScaler`를 생성/캐시하고 generic DXMT command stream의 `EncoderType::TemporalUpscale`을 기록한다.

따라서 D3D12는 D3D11 함수명을 복사하는 것으로 완성되지 않는다. D3D12는 별도 command allocator/command list/queue encoder stream을 쓰며 현재 `EncoderType`은 `Null`, `Clear`, `Render`, `Blit`, `Compute`, `Resolve`뿐이다.

## public D3D12 NGX ABI

local NVIDIA header `nvsdk_ngx.h`가 요구하는 핵심 D3D12 ABI는 다음과 같다.

| 함수 | 핵심 입력/의미 |
| --- | --- |
| `NVSDK_NGX_D3D12_Init` / `Init_Ext` | `ID3D12Device*`와 application/feature info |
| `GetParameters` / `AllocateParameters` / `GetCapabilityParameters` | `NVSDK_NGX_Parameter**` |
| `DestroyParameters` | allocated parameter map lifetime 종료 |
| `GetScratchBufferSize` | feature별 scratch 요구량, DLSS는 0 가능 |
| `CreateFeature` | **열려 있는 `ID3D12GraphicsCommandList*`**, feature id, create parameters |
| `EvaluateFeature` | **열려 있고 recording 중인 command list**, handle, frame parameters |
| `ReleaseFeature` | feature handle lifetime 종료 |
| `Shutdown` / `Shutdown1` | global/device lifetime 종료 |
| `GetFeatureRequirements` | adapter 기준 feature support |

public header는 D3D12 Evaluate command list가 recording 중이어야 하고, init device를 포함하는 node mask를 가져야 하며, non-copy queue에서 실행돼야 한다고 명시한다. 따라서 owned implementation은 Evaluate 안에서 별도 Metal command buffer를 즉시 commit하면 안 된다. Evaluate는 caller command list의 ordered stream에 temporal work를 **기록**해야 한다.

ZZZ에 동봉된 `nvngx_dlss.dll`의 static export set은 `D3D12_CreateFeature`, `EvaluateFeature`, `GetFeatureRequirements`, `GetScratchBufferSize`, `Init`, `Init_Ext`, `PopulateParameters_Impl`, `ReleaseFeature`, `Shutdown`, `Shutdown1`이다. 이는 feature plugin export이고, 우리가 만드는 `nvngx.dll` core-style shim의 parameter allocation API와 동일한 export surface라고 가정하면 안 된다. 실제 ZZZ에서 어떤 DLL lookup이 owned `nvngx.dll`로 연결되는지는 runtime loader observation이 필요하다.

## 제안하는 owned COM 경계

D3D12 resource 내부 포인터나 WMT object를 `nvngx.dll`에 노출하지 않는다. `nvngx.dll`과 `d3d12.dll` 사이에는 project-owned COM ABI만 둔다.

새 `src/d3d12/d3d12_interfaces.hpp`에 다음 개념을 두는 것이 가장 작은 안정 경계다.

### `IMTLD3D12DeviceExt`

역할:

- MetalFX temporal support query.
- NGX create-time logical contract를 보관하는 opaque feature state 생성.

개념적 API:

```cpp
struct MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC {
  UINT InputWidth;
  UINT InputHeight;
  UINT OutputWidth;
  UINT OutputHeight;
  UINT FeatureFlags;
  BOOL OutputSubrectsEnabled;
  UINT CreationNodeMask;
  UINT VisibilityNodeMask;
};

IMTLD3D12DeviceExt::CheckFeatureSupport(...);
IMTLD3D12DeviceExt::CreateTemporalUpscaleState(
    const MTL_TEMPORAL_UPSCALE_D3D12_CREATE_DESC*,
    IMTLD3D12TemporalUpscaleState**);
```

`IMTLD3D12TemporalUpscaleState`는 `IUnknown` 기반 opaque marker/state object로 둔다. concrete implementation은 `d3d12.dll` 안에만 있고 MetalFX scaler generation, first-execution state와 향후 private output/motion conversion resource를 소유한다. `nvngx.dll`은 이 concrete type이나 WMT type을 알 필요가 없다.

Scaler는 CreateFeature 시점에 즉시 만들지 않는다. NGX create parameter에는 실제 frame resource format/backing capacity가 없기 때문이다. 첫 `TemporalUpscale` 기록에서 resource를 resolve한 뒤 lazy-create한다. 현재 WMT ABI에 넣을 값은 정확히 다음과 같다.

```text
WMTFXTemporalScalerInfo.color_format = color texture pixelFormat
WMTFXTemporalScalerInfo.output_format = output texture pixelFormat
WMTFXTemporalScalerInfo.depth_format = depth texture pixelFormat
WMTFXTemporalScalerInfo.motion_format = validated motion texture/view pixelFormat
WMTFXTemporalScalerInfo.input_width = color backing width
WMTFXTemporalScalerInfo.input_height = color backing height
WMTFXTemporalScalerInfo.output_width = create-time NGX OutWidth
WMTFXTemporalScalerInfo.output_height = create-time NGX OutHeight
WMTFXTemporalScalerInfo.input_content_min_scale = 1.0f   // current DXMT policy
WMTFXTemporalScalerInfo.input_content_max_scale = 3.0f   // current DXMT policy
WMTFXTemporalScalerInfo.auto_exposure = create flag AutoExposure
WMTFXTemporalScalerInfo.input_content_properties_enabled = true
WMTFXTemporalScalerInfo.requires_synchronous_initialization = true
```

Local public MetalFX translator도 descriptor `inputWidth/inputHeight`를 resolved color view의 backing capacity에서 얻고 `outputWidth/outputHeight`는 NGX create logical output에서 얻는다. 이 분리는 flags74 fixture의 128 logical output / 136 backing contract와 일치한다.

### `IMTLD3D12GraphicsCommandListExt`

역할은 한 가지다: 이미 생성된 feature state와 이번 Evaluate의 immutable frame descriptor를 현재 command list에 기록한다.

개념적 API:

```cpp
struct MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC {
  ID3D12Resource* Color;
  ID3D12Resource* Output;
  ID3D12Resource* Depth;
  ID3D12Resource* MotionVector;
  ID3D12Resource* ExposureTexture;
  ID3D12Resource* BiasCurrentColorMask;
  ID3D12Resource* TransparencyMask;

  UINT InputContentWidth;
  UINT InputContentHeight;
  UINT ColorBaseX, ColorBaseY;
  UINT DepthBaseX, DepthBaseY;
  UINT MotionBaseX, MotionBaseY;
  UINT BiasBaseX, BiasBaseY;
  UINT OutputBaseX, OutputBaseY;

  FLOAT JitterOffsetX, JitterOffsetY;
  FLOAT MotionVectorScaleX, MotionVectorScaleY;
  FLOAT PreExposure;
  FLOAT ExposureScale;
  BOOL ResetHistory;
};

IMTLD3D12GraphicsCommandListExt::TemporalUpscale(
    IMTLD3D12TemporalUpscaleState*,
    const MTL_TEMPORAL_UPSCALE_D3D12_FRAME_DESC*);
```

Create flags처럼 feature lifetime 동안 고정인 값은 state에 둔다. 매 frame 변하는 scalar와 resource만 frame desc에 둔다. 이 인터페이스는 `ID3D12Resource*`까지만 받으며 native `MTLTexture`, `D3DMetal` pointer, private GPTK ABI는 전혀 건너지 않는다.

## D3D12 command recording

`MTLD3D12GraphicsCommandListImpl::QueryInterface`가 새 command-list extension을 제공해야 한다. `TemporalUpscale` 진입 시 다음 순서를 사용한다.

1. command list를 `IMTLD3D12GraphicsCommandListExt`로 QI하고 `ValidateRecording()`을 호출한다. 이 shared ABI가 owned backend의 현재 recording 상태와 DIRECT/COMPUTE 조건을 비파괴적으로 검증한다.
2. list type이 `DIRECT` 또는 `COMPUTE`인지 확인한다. `COPY`는 NGX public contract와 맞지 않으므로 실패한다.
3. 전달된 resources가 null이 아니고 texture resource인지, 같은 owned D3D12 device에 속하는지 확인한다.
4. resource의 `MTLD3D12Resource::texture`를 **d3d12.dll 내부에서만** 사용한다.
5. 각 texture의 현재 allocation/view를 `TextureViewRef`로 capture한다. raw `ID3D12Resource*` 또는 parameter map pointer를 encoder data에 남기지 않는다.
6. `allocator_->InvalidateCurrentPass()`로 앞 encoder를 끝낸다.
7. 새 `EncoderType::TemporalUpscale` payload를 allocator CPU heap에 placement-new한다.
8. feature generation/state, retained texture views, normalized scalar snapshot을 payload에 저장한다.
9. 다시 `InvalidateCurrentPass()`해 다음 draw/compute가 temporal pass와 같은 encoder로 합쳐지지 않게 한다.

`ResourceBarrier`는 현재 candidate에서 이미 pass boundary를 만든다. 따라서 app이 NGX 전후에 기록한 transition/UAV barrier의 순서는 preserved 된다. 이 path가 D3D12 state validator를 새로 만들 필요는 없다.

## 새 D3D12 encoder payload의 lifetime

generic DXMT D3D11 encoder의 `TemporalUpscaleData`를 그대로 복사하면 안 되는 가장 큰 이유가 lifetime이다. generic context는 encode 후 data를 파괴하지만 D3D12 closed command list는 다시 실행할 수 있다.

D3D12 `TemporalUpscaleEncoderData`는 최소 다음을 retain해야 한다.

- `TextureViewRef` color/output/depth/motion, optional exposure.
- feature/scaler generation의 strong reference.
- frame scalar snapshot.
- 향후 필요한 per-record immutable conversion/shadow description.

이 object의 destructor는 **queue execute에서 호출하지 않는다**. `MTLD3D12CommandAllocatorImpl::Reset()`의 type switch에 `TemporalUpscale` destructor case를 추가한다. D3D12 contract상 allocator는 그 allocator로 기록한 GPU work가 끝나기 전에 reset할 수 없으므로 이 lifetime이 GPU use까지 resource/state를 보존한다.

이 방식은 다음 실제 fixture 요구도 만족한다.

- `EvaluateFeature` 직후 parameter map을 reset/destroy해도 replay payload가 영향을 받지 않는다.
- `ReleaseFeature`가 호출돼도 이미 기록된 command list는 retained generation으로 실행될 수 있다.
- 같은 closed command list를 여러 번 Execute해도 encoder payload가 첫 실행 후 사라지지 않는다.

## queue-side MetalFX encoding

prime이 소유한 D3D12 queue switch에는 `EncoderType::TemporalUpscale` case가 필요하다. 기존 queue-wide `fence_`와 scaler의 fence를 다음처럼 bridge한다.

1. begin blit encoder: `waitForFence(queue fence)` → `updateFence(scaler fence)` → end.
2. 같은 Metal command buffer에서 `encodeTemporalScale(...)`.
3. end blit encoder: `waitForFence(scaler fence)` → `updateFence(queue fence)` → end.

이 순서는 기존 generic DXMT temporal encode가 쓰는 scaler-fence pattern을 D3D12 queue의 현재 pass-order fence chain에 연결한 것이다. 별도 command buffer commit이나 CPU wait는 필요하지 않다.

현재 WMT per-execution property와 encode ABI는 다음 필드만 제공한다.

```text
WMTFXTemporalScalerProps.input_content_width
WMTFXTemporalScalerProps.input_content_height
WMTFXTemporalScalerProps.reset
WMTFXTemporalScalerProps.depth_reversed
WMTFXTemporalScalerProps.motion_vector_scale_x
WMTFXTemporalScalerProps.motion_vector_scale_y
WMTFXTemporalScalerProps.jitter_offset_x
WMTFXTemporalScalerProps.jitter_offset_y
WMTFXTemporalScalerProps.pre_exposure

MTLCommandBuffer_encodeTemporalScale(
  cmdbuf, scaler, color, output, depth, motion, exposure, scaler_fence, props)
```

따라서 `Exposure.Scale`, offsets, reactive mask 같은 필드는 frame desc에는 보존하되 현재 WMT가 표현할 수 없는 non-identity 값이면 frontend/backend 경계에서 명시적으로 실패시켜야 한다.

단, 동일 feature/scaler를 서로 다른 D3D12 command queue에서 동시에 사용할 때의 cross-queue history synchronization은 현재 WMT `Fence` contract만으로 증명되지 않는다. 첫 구현은 same graphics queue 사용을 runtime observation으로 확인하고, 그렇지 않다면 shared-event 기반 설계가 추가로 필요하다. static ZZZ 분석만으로 single-queue 사용을 확정하지 않는다.

## scaler state와 replay 시 reset

NGX caller의 `Reset=0`을 record 시점에서 그대로 `WMTFXTemporalScalerProps.reset`에 박아두는 것만으로는 D3D12 replay를 정확히 처리할 수 없다. local flags74 fixture는 첫 call에서도 caller reset을 false로 유지하고 translator가 새 temporal generation의 첫 실행을 초기화하도록 요구한다.

따라서 feature state는 scaler **generation**을 갖고 queue 실행 시점에 effective reset을 계산해야 한다.

```text
effectiveReset = recordedCallerReset || generationHasNeverExecuted
```

`generationHasNeverExecuted`는 record count가 아니라 실제 queue execution count에 기반해야 한다. 같은 recorded list를 재실행하면 첫 GPU execution만 implicit reset이고 다음 execution은 caller reset이 false인 한 history를 이어가야 한다. scaler가 format/extent/API requirement 변화 때문에 교체되면 새 generation으로 돌아가 첫 execution reset을 다시 적용한다.

## resource → MetalFX mapping

`ID3D12Resource*`의 native mapping은 `d3d12.dll` 내부에서 `MTLD3D12Resource::texture`와 `TextureViewRef`로 끝낸다.

| NGX | owned D3D12/MetalFX |
| --- | --- |
| Color | retained current full/content view → MetalFX color texture |
| Output | retained output texture; logical output extent는 NGX `OutWidth/OutHeight`, resource backing size와 분리 |
| Depth | retained depth texture; create flag의 reverse-depth 의미를 scaler props에 전달 |
| MotionVectors | retained motion texture; flags74는 low-resolution MV이므로 별도 high-res downsample 불필요 |
| ExposureTexture | auto exposure에서는 null 허용; manual path는 format conversion contract가 별도 필요 |

특히 `OutputWidth/Height`를 `output resource->width()/height()`로 다시 계산하면 안 된다. flags74 game-contract fixture는 logical output `128x128`에 backing resource `136x136`을 사용하고 outside sentinel이 보존되는지 확인한다. scaler descriptor는 create-time logical output extent를 사용해야 한다.

## flags74가 요구하는 최소 functional milestone

flags74는 `MVLowRes | DepthInverted | AutoExposure`, HDR bit off이다. local fixture가 요구하는 concrete mapping은 다음과 같다.

| 항목 | fixture/contract | 첫 owned D3D12 path 처리 |
| --- | --- | --- |
| create input | 64x64 | logical input extent로 보존 |
| create output | 128x128 | logical output extent로 보존; 136x136 backing과 분리 |
| Color/Output | DXGI 28 / Metal 70 | retained texture views |
| Motion | DXGI 34 / Metal 65 | low-res MV 직접 전달 |
| Depth | DXGI 20 / Metal 260 | depth-stencil texture 그대로, `depth_reversed=true` |
| Auto exposure | true | MetalFX `autoExposureEnabled=true`; exposure texture null |
| caller reset | 첫 frame 포함 false | generation 첫 **execution**에서 implicit reset |
| raw PreExposure | 0 | semantic 1.0으로 normalize 후 MetalFX 전달 |
| raw Exposure.Scale | 0 | semantic 1.0; 현재 WMT에 별도 property가 없으므로 identity는 no-op |
| offsets | 0 | supported |
| bias/reactive mask | null | supported as absent |
| alpha upscaling | false | supported; alpha flag가 오면 현재 backend는 명시적 unsupported |

이 subset은 upstream WMT temporal scaler ABI로 표현할 수 있다. 따라서 **build 가능성**과 **flags74 synthetic compatibility 구현 가능성**에는 구조적 blocker가 없다.

그러나 이 결론은 실제 ZZZ session이 raw flags74를 생성한다는 뜻이 아니다. static 원본 분석은 UnityPlayer가 Streamline에 HDR, auto exposure, preExposure, exposureScale, mode/output dimensions, common camera constants를 동적으로 전달하고 `sl.dlss`가 raw NGX contract를 만든다는 것까지만 증명한다. raw flags74는 이전 runtime capture/fixture contract다.

## flags74 밖에서 upstream WMT ABI에 빠진 것

현재 `winemetal.h`의 `WMTFXTemporalScalerInfo/Props`와 `MTLCommandBuffer_encodeTemporalScale`은 다음 public DLSS/modern MetalFX 의미를 표현하지 못한다.

- color/depth/MV/output content base offsets와 output subrect origin.
- output-resolution motion-vector enable flag를 직접 전달하는 modern API.
- jittered-motion-vector enable flag.
- bias-current/reactive mask.
- non-identity `DLSS.Exposure.Scale`.
- manual exposure resource의 documented MetalFX format으로의 numeric conversion.
- alpha upscaling.
- scaler가 요구하는 output texture usage/private shadow negotiation.

첫 flags74 implementation에서는 이 항목을 조용히 무시하면 안 된다. identity/absent/zero 조건만 허용하고 그렇지 않으면 explicit unsupported/invalid result를 반환한다. 이후 public MetalFX API를 WMT에 확장할 때 하나씩 해제한다.

## actual ZZZ static contract와의 경계

ZZZ UnityPlayer static 분석은 다음을 증명한다.

- UnityPlayer는 `sl.interposer.dll`의 Streamline API를 동적으로 resolve한다.
- ExecuteDLSS는 `slSetConstants`, `slDLSSSetOptions`, four resource tags, `slEvaluateFeature`를 호출한다.
- SR resource tag는 ScalingInputColor, ScalingOutputColor, Depth, MotionVectors 네 개이며 모두 eOnlyValidNow다.
- `sl.dlss.dll`에는 raw NGX Width/Height/Out*, flags, Color/Output/Depth/MV, jitter/reset/MV scale, PreExposure/Exposure.Scale, optional masks/exposure, subrect parameter 문자열이 존재한다.
- Unity option의 HDR, auto exposure, preExposure, exposureScale, mode/output size는 동적이며 alpha upscaling은 false다.

static 분석만으로는 다음을 증명하지 않는다.

- 현재 session에서 DLSS가 실제 실행됐는지.
- Streamline가 최종 생성한 raw flags와 per-frame 값.
- Streamline/NGX loader가 owned `nvngx.dll`을 선택했는지.
- feature가 어느 command queue에서 몇 번 실행됐는지.

따라서 첫 real integration validation에는 loader identity와 NGX D3D12 call trace가 필요하지만, 그것은 prime이 소유한 runtime/GPU 단계다.

## runtime publish 변경점

D3D12 NGX frontend가 완성되기 전에는 현재 `nvngx.dll`을 owned runtime의 D3D12 NGX implementation으로 광고하면 안 된다. 현재 install의 파일은 D3D11 export만 갖는다.

frontend가 구현된 뒤 runtime staging에는 최소 다음이 추가돼야 한다.

- `x86_64-windows/nvngx.dll`을 `lib/wine/x86_64-windows/nvngx.dll`로 stage/hash/verify.
- `WINEDLLOVERRIDES`에 `nvngx=b` 추가하여 game/native fallback을 막음.
- DXMT vendor extension registry setup이 필요한 실제 loader path라면 `DXMT_ENABLE_NVEXT=1`을 owned wrapper policy로 명시.
- staged `nvngx.dll` export verification으로 required D3D12 entry points 존재 확인.

`DXMT_ENABLE_NVEXT=1`일 때 candidate `dxgi.cpp`는 NVIDIA extension registry와 `NGXCore\\FullPath=C:\\Windows\\System32`를 설정한다. ZZZ Streamline가 이 경로를 실제로 사용하는지 여부는 runtime trace로 확인해야 하며 static 추측으로 `nvngx_dlss.dll`을 교체하지 않는다.

## 최소 구현 순서

1. **D3D12 COM interface**: `IMTLD3D12DeviceExt`, opaque temporal state, `IMTLD3D12GraphicsCommandListExt`를 정의하고 device/list QI를 구현한다.
2. **D3D12 encoder type**: `TemporalUpscaleEncoderData`와 allocator Reset destructor를 추가한다. queue case는 prime queue owner가 현재 global fence chain에 scaler fence bridge를 추가한다.
3. **D3D12 NGX frontend**: existing `ParametersImpl` D3D12 slots를 사용해 Init/Create/Evaluate/Release/parameter/capability entry points를 구현한다. Create는 logical create contract를 state에 저장하고 Evaluate는 immutable frame snapshot만 command list extension으로 넘긴다.
4. **flags74 strict subset**: low-res MV, reverse depth, auto exposure, zero offsets, no optional masks/exposure, alpha off, semantic exposure identity만 허용한다. output logical extent와 backing extent를 분리한다.
5. **CPU/PE validation**: D3D12 exports, parameter ABI, Create/Release lifetime, command-list-open rejection, unsupported-field rejection, parameter reset-after-Evaluate, command-list replay lifetime을 fixture로 검증한다.
6. **runtime publish**: 위 CPU 계약이 통과한 뒤에만 stager에 `nvngx.dll`과 `nvngx=b`를 추가한다.
7. **real runtime validation**: prime이 loader identity → D3D12 NGX calls → queue execution → MetalFX encode 순서로 확인한다. synthetic flags74 PASS와 실제 ZZZ compatibility는 별개의 verdict로 유지한다.

현재 구조에서 가장 먼저 합의할 interface boundary는 `nvngx.dll`이 WMT/native resource를 직접 보지 않고 `ID3D12Device`/`ID3D12GraphicsCommandList`의 owned COM extension만 호출하는 것이다. 이 경계를 지키면 GPTK/D3DMetal private ABI 없이 D3D12 NGX frontend와 backend를 독립적으로 구현/검증할 수 있다.
