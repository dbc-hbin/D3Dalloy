# Owned D3D12 temporal / NGX API correctness review

이 문서는 `build/owned-graphics-20260919/dxmt`의 owned D3D12 temporal backend와
`build/owned-graphics-20260919/ngx-work`의 `nvngx.dll` D3D12 frontend를 실제 소스 기준으로
교차 검토한 결과다. `graphics-translator/NGX-INTEGRATION.ko.md`의 설명보다 현재 소스를 우선했고,
DXMT D3D11 temporal 경로, 로컬 macOS Metal/MetalFX SDK header, 로컬 NVIDIA NGX 공식 header,
기존 public MetalFX validation 결과, ZZZ에서 이전에 수집한 resource metadata를 함께 대조했다.

이 검토는 GPU/game을 실행하지 않았다. 따라서 synthetic fixture 성공이나 과거 native trace와의 형식 일치만으로
현재 ZZZ runtime 호환성을 주장하지 않는다.

## 결론과 현재 우선순위

현재 snapshot에서 가장 큰 correctness blocker는 **동일 temporal feature를 여러 D3D12 queue에서 사용할 때
backend가 추가하는 global Metal shared-event 순서가 D3D12 queue fence 순서와 충돌할 수 있다는 점**이다.
`src/d3d12/d3d12_temporal.cpp:305-344`는 host가 `EncodeOwnedTemporalFrame`에 들어온 순서대로 feature-wide
`core.sequence`를 배정하고, 다음 evaluation이 바로 이전 sequence를 기다리게 한다. D3D12의 queue wait는
GPU-side wait를 enqueue하고 호출자에게 즉시 반환하므로, 미래의 다른 queue signal을 기다리는 제출 순서가 정상이다.
이때 hidden temporal event가 반대 방향 dependency를 추가하면 cycle이 생긴다.

예를 들어 다음은 D3D12에서 허용되는 순서다.

```text
Queue B: Wait(F, 1)
Queue B: Execute(temporal B)
Queue A: Execute(temporal A)
Queue A: Signal(F, 1)
```

host encode 순서가 B temporal = history sequence 1, A temporal = sequence 2가 되면 A temporal은 sequence 1을
기다린다. B queue는 `F=1`을 기다리고, 그 signal은 A temporal 뒤에 있으므로 `B -> A signal`과
`A temporal -> B temporal`이 서로 물려 GPU cycle이 된다. 이 문제는 single-queue fixture로 검출되지 않는다.

수정은 단순히 queue별 history로 분리하면 안 된다. Temporal history는 feature 단위이므로, explicit D3D12
queue wait/signal이 정의한 실행 순서와 양립하도록 submission dependency를 scheduling해야 한다. unresolved queue
wait 때문에 calling thread를 막는 방식도 D3D12 `ID3D12CommandQueue::Wait` 계약과 맞지 않는다. queue API call을
FIFO task로 기록하고, dependency가 만족될 때 Metal commit을 진행하는 device-level scheduler가 필요하다.

deferred submission을 구현할 때는 `ID3D12GraphicsCommandList` 객체를 보관했다가 나중에 `entry`를 다시 읽으면 안 된다.
D3D12 command list는 `ExecuteCommandLists`가 반환된 뒤 Reset될 수 있어서 `entry`, recording id, allocator가 바뀔 수 있다.
enqueue 시점에 recording의 `entry`/encoder 범위와 그 storage를 소유한 allocator를 snapshot-retain해야 한다. 실제 Metal
completion까지 fence/queue state와 `OwnedTemporalFrame`도 유지해야 한다. 같은 recorded frame의 scratch texture가
replay에서 재사용되므로 scratch write/copy까지 feature history ordering 안에 남아 있어야 한다.

## 검토 중 발견되어 현재 candidate에서 이미 수정된 항목

### generation discontinuity 후 replay reset

이전 구현은 각 `Generation`의 `initialized`만 보았기 때문에 `G1 -> G2 -> closed-list G1 replay`에서 마지막 G1
history가 연속이라고 잘못 판단할 수 있었다. 현재 `d3d12_temporal.cpp:38-49, 315-343`은
`Core::last_executed_generation`을 저장하고, 실행 generation이 바뀌면 caller reset과 무관하게 `props.reset=true`로
만든다. 따라서 이 stale-history 문제는 현재 source에서 수정되어 있다.

최소 regression은 두 generation을 A/B로 번갈아 실제 encode 순서에 넣고 reset sequence가 `1,1,1`인지 확인하고,
같은 generation의 연속 실행은 `1,0`인지 확인하는 것이다.

### command allocator final-release 시 TemporalEncoderData 소멸

이전 allocator destructor는 placement-new encoder object의 destructor를 호출하지 않고 arena를 `free()`해서
`TemporalEncoderData::frame`의 `shared_ptr<OwnedTemporalFrame>`과 그 resource/scratch residency가 누수될 수 있었다.
현재 `d3d12_command_allocator.cpp:112-169`은 `ClearRecordedCommands()`를 Reset과 destructor가 공유하며
`TemporalEncoderData` destructor도 호출한다. 연결 포인터도 object lifetime 종료 전에 읽는다. 이 항목은 현재 source에서
수정되어 있다.

### COM output storage 사용

검토 초기에 있던 private QI/GetDevice output storage 문제도 현재 source에서 수정되어 있다.
`d3d12_temporal.cpp:76-80`은 `GetDevice(..., reinterpret_cast<void **>(&owner))`, `294-299`는 private QI를
`reinterpret_cast<void **>(&internal)`로 받는다. `Com::ref()`는 AddRef된 raw pointer를 반환하는 함수이므로 COM out-param
storage로 쓰면 안 된다.

## 현재 남아 있는 definite API/lifetime bugs

### `NVSDK_NGX_D3D12_Shutdown1(nullptr)` 계약 위반

로컬 NVIDIA 공식 `nvsdk_ngx.h`는 `Shutdown1`의 device가 null이면 모든 SDK instance를 shutdown한다고 명시한다.
현재 `ngx-work/src/nvngx/nvngx_d3d12.cpp:609-612`는 null도 `NVNGXD3D12ShutdownDevice`로 보내며,
그 함수는 `553-555`에서 null을 `InvalidParameter`로 반환한다.

최소 수정은 `Shutdown1(nullptr)`에서 `NVNGXD3D12ShutdownAll()`을 호출하고 public success를 반환하는 것이다.
PE regression은 Init 후 `Shutdown1(nullptr)`을 호출하고, 재-init 전 Allocate/Create 계열이 NotInitialized가 되는지
확인하면 된다.

### feature handle token이 release/shutdown 뒤에도 영구 누수

`nvngx_d3d12.cpp:711-718`은 CreateFeature마다 `HandleToken` heap object를 만들어 `g_handle_tokens`에 넣지만,
ReleaseFeature(`757-765`)는 `g_features`만 지우고 token은 제거하지 않는다. `NVNGXD3D12ShutdownAll`도 token vector를
비우지 않는다. 따라서 create/release를 반복하면 process lifetime 동안 token heap allocation이 단조 증가한다.

주소 재사용 때문에 stale handle이 새 feature와 alias되는 것을 피하려는 목적은 타당하지만, 영구 heap object가 유일한
방법은 아니다. dereference하지 않는 monotonically increasing opaque token 또는 generation/tombstone mapping으로 stale
handle을 계속 `FeatureNotFound`로 만들면서 live storage를 bounded하게 유지할 수 있다.

### output subrect enable을 받아들이면서 실제 offset은 전부 거부

NVIDIA helper/create struct의 `InEnableOutputSubrects`는 `DLSS.Enable.Output.Subrects`에 전달된다. 현재 frontend는
`parseCreate`(`363-366`)에서 값 1을 정상적으로 받아들이고 capability parameter map에도 기본값 1을 넣는다(`333`).
그러나 `parseFrame`(`422-428`)은 non-zero `DLSS.Output.Subrect.Base.X/Y`를 항상 UnsupportedParameter로 거부하고,
backend도 output offset을 E_NOTIMPL로 거부한다.

지원 구현 전까지는 CreateFeature에서 `OutputSubrectsEnabled=1`을 명시적으로 거부하거나, MetalFX `outputOffsetX/Y`와
scratch/copy rectangle까지 구현해야 한다. enable=1을 성공시킨 뒤 enable이 허용하는 실제 offset 사용을 모두 실패시키는
상태는 API contract와 일치하지 않는다. 이전 ZZZ 관찰에서는 output base가 0이었으므로 flags74 game path의 현재 blocker로
분류하지 않는다.

## flags 74와 실제 ZZZ parent resource mapping

`74 == MVLowRes(2) | DepthInverted(8) | AutoExposure(64)`이며 HDR, MVJittered, sharpening, alpha-upscaling bit는 꺼져 있다.
이 조합 자체는 frontend/backend의 현재 supported flag subset 안에 있다.

이전 ZZZ actual translator trace와 이를 요약한 preflight에서 관찰한 parent DXGI format/resource flag,
현재 DXMT allocation/view 결과는 다음과 같다. 첫 matching actual event는 color/depth/motion/output D3D12 flags가
각각 `1/2/1/5`였고 `DENY_SHADER_RESOURCE(8)`는 하나도 없었다.

| 역할 | ZZZ parent DXGI | D3D12 flags | DXMT backing / temporal view | MetalFX format 값 | 판정 |
| --- | --- | ---: | --- | ---: | --- |
| color | `R8G8B8A8_TYPELESS` (27) | 1 | `RGBA8Unorm` | 70 | 현재 경로와 일치 |
| depth | `R32G8X24_TYPELESS` (19) | 2 | `Depth32Float_Stencil8` | 260 | combined depth/stencil 사용 |
| motion | `R16G16_TYPELESS` (33) | 1 | backing RG16 계열 → temporal `RG16Float` view | 65 | D3D11 보정 규칙과 일치 |
| output | `R8G8B8A8_TYPELESS` (27) | 5 | `RGBA8Unorm`; MetalFX는 private scratch에 출력 | 70 | 현재 scratch format과 일치 |

`dxmt_format.cpp:555-567`에서 typeless `R32G8X24_TYPELESS`와 typed `D32_FLOAT_S8X24_UINT`는 둘 다 Metal
`Depth32Float_Stencil8` backing으로 간다. D3D11 temporal 경로도 backing depth format을 descriptor에 그대로 전달한다.
또한 기존 public MetalFX GPU fixture는 typed `D32_FLOAT_S8X24_UINT`/Metal260을 정상 처리했다. 따라서 현재 증거로는
temporal depth를 임의의 depth-only Metal view로 강제해야 한다고 결론 낼 수 없다.

다만 실제 ZZZ parent는 typeless이고 synthetic fixture는 typed resource였다. 둘이 같은 Metal backing format으로 내려간다는
사실은 좋은 구조적 증거지만, typeless parent + actual game view/resource state의 live 호환성을 증명하지는 않는다.

## MetalFX texture usage와 private output scratch

Apple public MetalFX 문서는 scaler의 `colorTextureUsage`, `depthTextureUsage`, `motionTextureUsage`,
`outputTextureUsage`가 caller texture descriptor에 필요한 **minimum usage bits**라고 정의한다. 이 기기에서 실행한 기존
public MetalFX probe는 color/depth/motion/output minimum을 `1/1/1/7`로 반환했다.

현재 D3D12 texture allocation은 shader-resource가 금지되지 않은 input에 ShaderRead(1)를 준다. 위 actual ZZZ parent
flags에는 `DENY_SHADER_RESOURCE`가 없으므로 이 검사를 통과할 구조다. typeless resource에는 PixelFormatView bit가 추가될
수 있고 depth/render-target usage도 추가된다. MetalFX에 직접 넘기는 output은 app output이
아니라 `d3d12_temporal.cpp:226-245`의 private scratch이며 usage는
`ShaderRead | ShaderWrite | RenderTarget == 7`이다. 따라서 **현재 기기/모델에서 관찰한 minimum usage에는 맞는다.**

MetalFX encode 뒤 `330-342`는 scaler fence를 기다린 다음 logical output 크기만 scratch origin에서 app output origin으로
blit한다. app output backing이 더 큰 경우 logical rectangle 바깥을 건드리지 않는다. frame은 queue inflight entry가
completion까지 `shared_ptr`로 유지하므로 scratch와 retained input/output resource도 그때까지 산다.

제한은 WMT `TemporalScaler` wrapper가 MetalFX의 runtime usage getter를 노출하지 않는다는 점이다. 미래 OS/device/model이
7보다 큰 output minimum을 요구할 경우 현재 hardcoded scratch usage는 자동 적응하지 못한다. 이것은 현재 probe 기준
flags74 bug가 아니라 portability gap이다.

## dynamic input scale

Apple SDK 27의 `MTLFXTemporalScalerDescriptor`는 `inputContentPropertiesEnabled`로 dynamic resolution을 활성화하고
`inputContentMinScale`/`MaxScale` 범위를 지정한다. 현재 기기의 public probe는 supported range `[1,3]`을 반환했다.
backend가 사용하는 `[1,3]`은 이 기기와 일치하며, 이전 ZZZ에서 관찰한 render/output ratio도 이 범위 안이었다.

backend는 descriptor input width/height에 color backing capacity를 넣고, evaluation마다 active
`InputContentWidth/Height`를 props로 전달한다. 이는 public MetalFX validation에서 larger backing + smaller active content가
성공한 형태와 일치한다.

일반 API 범위에서는 두 제한이 있다.

1. supported min/max를 device에서 query하지 않고 `[1,3]`으로 고정한다. 현재 device에서는 맞지만 portability 보장은 없다.
2. `d3d12_temporal.cpp:174-178`은 low-resolution motion backing capacity까지 color/depth와 정확히 같아야 한다고 요구한다.
   public MetalFX probe는 active rect가 bounds 안일 때 low-res MV backing capacity가 color와 달라도 성공했다. 따라서 이 검사는
   유효한 API shape를 과도하게 거부한다. 이전 ZZZ 관찰 shape에는 해당하지 않았다.

## HDR flag

NVIDIA 공개 DLSS 계약에서 `IsHDR=0`은 단순히 8-bit format을 뜻하지 않고 perceptual LDR 표현을 뜻한다. 반대로
MetalFX temporal public API에는 DLSS `IsHDR`와 동일한 transfer-function selector가 없다. 현재 backend는 HDR bit를
Create에서 허용하지만 descriptor/encode 동작을 바꾸지 않는다.

따라서 HDR bit 지원의 의미 동일성은 **입증되지 않았다.** 그렇다고 현재 API 근거만으로 gamma 2.2 변환 같은 보정을
추가할 수도 없다. 실제 flags74에서는 HDR bit가 꺼져 있으므로 이 항목을 flags74 blocker나 시각 회귀 원인으로 단정하지
않는다.

## exposure, masks, motion-vector 범위

flags74의 AutoExposure는 MetalFX descriptor `auto_exposure=true`로 전달된다. frontend는 raw pre-exposure 0을 NVIDIA helper
sentinel 의미대로 1로 정규화하고, backend는 MetalFX `preExposure`로 전달한다. `Exposure.Scale != 1`은 public MetalFX의
동등한 scalar를 입증하지 못했으므로 명시적으로 Unsupported 처리한다. manual exposure는 현재 R16Float 1x1만 지원한다.

BiasCurrentColorMask/TransparencyMask, non-zero input/output subrect offsets, high-resolution MV, alpha upscaling은 현재
명시적 unsupported subset이다. 실제 flags74 관찰에서는 exposure/bias mask가 null이고 low-res MV였다. unsupported API를
silent emulation하지 않는 방향은 맞지만, 위 output-subrect처럼 Create에서 지원한다고 받아들인 뒤 evaluate에서 항상 실패하는
경우는 별도로 정리해야 한다.

## MSVC parameter ABI와 COM identity

로컬 NVIDIA 공식 `nvsdk_ngx_params.h`의 `NVSDK_NGX_Parameter` virtual method 순서는 Set
`ULL,float,double,uint,int,D3D11*,D3D12*,void*`, Get 동일 순서, 마지막 Reset이다.
`nvngx_d3d12_msvc.cpp`의 MSVC facade는 이 순서를 그대로 구현하고 실제 storage 호출을 C ABI bridge로 넘긴다. MinGW C++
vtable layout을 가정하지 않는 구조다. 현재 pinned header와 facade 사이에 확인된 vtable-order mismatch는 없다.

D3D12 frontend의 device registry는 `QueryInterface(IUnknown)`으로 얻은 canonical COM identity를 key로 사용한다.
Create/Evaluate 때 command list의 `GetDevice` 결과도 canonical IUnknown identity로 feature device와 비교한다. backend private
state QI도 현재 정상적인 COM out-param storage를 사용한다. prime의 output-pointer 수정 이후 이 범위에서 남은 definite COM
identity bug는 확인하지 못했다.

## NGX parameter ownership

공식 NGX header는 `GetParameters`가 반환한 map은 NGX-managed라 app이 DestroyParameters를 호출하면 안 되고,
`AllocateParameters`와 `GetCapabilityParameters` map은 app-managed라 Destroy해야 한다고 명시한다. 현재 frontend는 전자를
`managed=true`, 후자를 `managed=false`로 등록하고 Destroy 시 managed map을 거부하므로 이 부분은 공식 lifetime 규칙과
일치한다.

## 필요한 최소 검증

현재 GPU/game을 실행하지 않고도 추가할 수 있는 regression은 다음과 같다.

- 두 D3D12 queue의 future-signal wait graph에서 temporal hidden dependency가 cycle을 만들지 않는 scheduler test.
- deferred submission이 command-list Reset 후에도 enqueue 시 snapshot한 recording/allocator storage만 읽는 lifetime test.
- generation A/B/A replay reset sequence `1,1,1`과 same-generation `1,0` test.
- command allocator를 Reset 없이 release해도 TemporalEncoderData와 frame/resource/scratch retain이 해제되는 test.
- official MSVC NVIDIA header로 `Shutdown1(nullptr)` all-device behavior를 검사하는 PE test.
- repeated CreateFeature/ReleaseFeature 뒤 token backing storage가 bounded한지와 stale handle이 계속 FeatureNotFound인지 검사.
- `OutputSubrectsEnabled=1`을 지원한다면 non-zero output base를 실제 MetalFX output offset + scratch copy까지 검증하고,
  지원하지 않을 동안은 CreateFeature에서 명시적 unsupported인지 검사.

실제 ZZZ compatibility의 마지막 확인은 별도다. actual game의 typeless parent resources, resource states, queue wait/signal 순서,
closed-list replay가 이 경로에 들어오는 것을 runtime trace에서 확인해야 한다. synthetic 8/10-frame fixture의 성공만으로 그 결론을
대체하지 않는다.

## review snapshot

정확한 source hash와 base commit은 `build/owned-graphics-20260919/api-review/source-snapshot.txt`에 기록했다.
검토 도중 prime이 수정한 generation reset, allocator final-release, COM out-param 항목은 위에서 **현재 수정됨**으로 분리했고,
hash는 그 수정이 들어간 source를 기준으로 한다. cross-queue ordering과 NGX lifecycle/subrect/token 항목은 이 snapshot에서
남아 있었다.
