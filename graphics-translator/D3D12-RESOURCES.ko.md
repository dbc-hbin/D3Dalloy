# D3D12 resource·view·heap·copy 계약

2026-10-02 루트 [`src/d3d12/`](../src/d3d12/) 기준이다. 이전 0012 import patch 설명을 현재 resource 계약과 분리했다. 전역 지원표·완료 판정은 [현재 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태), format·sample quality는 [capability 정책](CAPABILITIES.ko.md), 빌드·runtime 준비는 [README](../README.md)를 따른다. 이 갱신은 소스 대조이며 새 GPU 실행이 아니다.

## 생성·allocation·placed heap

Committed/placed 생성, allocation-info 및 유효한 null-output probe는 공통 descriptor·heap property·initial state·optimized-clear 검증을 사용한다. 지원되지 않는 생성 조합을 output 생략만으로 성공시키지 않는다.

- placed offset은 D3D12 resource alignment와 Metal `heapBufferSizeAndAlign`/`heapTextureSizeAndAlign`의 alignment를 모두 만족해야 한다.
- offset/size arithmetic은 overflow-safe이며 allocation 전체가 heap 안에 들어가야 한다. Resource kind와 heap의 DENY flags도 일치해야 한다.
- heap 생성 실패는 residency 등록 전에 `E_OUTOFMEMORY`로 끝난다. Heap-backed allocation은 `AllocatedOnHeap`으로 표시하여 heap과 resource를 독립 allocation처럼 이중 계상하지 않는다.
- allocation-info는 실제 backing의 size/alignment를 사용한다. RGB32·packed storage, depth backing, MSAA 같은 emulation 조건은 논리 footprint와 native heap 크기가 같다는 가정을 허용하지 않는다.
- protected-session null은 일반 생성 경로이며 실제 session은 미지원이다. 상세는 [core API 계약](D3D12-CORE-CONTRACTS.ko.md)을 따른다.

소스: [`d3d12_device.cpp`](../src/d3d12/d3d12_device.cpp), [`d3d12_heap.cpp`](../src/d3d12/d3d12_heap.cpp), [`d3d12_buffer.cpp`](../src/d3d12/d3d12_buffer.cpp), [`d3d12_texture.cpp`](../src/d3d12/d3d12_texture.cpp), [`d3d12_resource_contract.hpp`](../src/d3d12/d3d12_resource_contract.hpp).

## Descriptor·view 범위

| 영역 | 현재 계약 |
|---|---|
| Buffer SRV/UAV | raw=`R32_TYPELESS`, structured=`UNKNOWN`+stride, typed=explicit typed format으로 구분한다. element offset/length multiplication과 parent buffer 범위를 검사한다. 현재 `BufferSlice.byteOffset`/`byteLength`는 64-bit다. 0012 당시 32-bit byte slice 제한을 현재 제약으로 사용하지 않는다. |
| UAV counter | structured UAV에서만 허용하며 4096-byte placement alignment와 counter resource 범위를 검사한다. `CounterOffsetInBytes`는 64-bit로 전달한다. |
| Buffer RTV/DSV | Buffer RTV의 현재 typed view·draw 경로는 구현돼 있다. 0012 당시 `E_NOTIMPL`은 더 이상 현재 설명이 아니다. Buffer DSV는 depth texture view로 가장하지 않는다. |
| Mip/array | first와 count를 parent 범위에 맞춰 검사한다. `UINT_MAX` remainder는 `total-first`다. |
| CubeArray SRV | `First2DArrayFace`와 `NumCubes*6`을 적용하며 cube 시작은 6-face boundary여야 한다. |
| Depth/stencil plane | Combined backing의 depth typed alias는 plane 0, stencil alias는 plane 1이다. Fully typeless resource의 implicit/default view는 거절하고 compatible typed descriptor를 요구한다. |
| Partial 3D UAV | `FirstWSlice`/`WSize`를 mip depth에 맞춰 검증하고 slice offset/extent를 descriptor ABI로 전달한다. shader load/store/atomic 및 bounded clear에 적용한다. 전체 depth만 허용하던 0012 제한은 제거됐다. |
| 3D RTV | W 범위를 검증하고 `DepthPlane`/`RenderTargetArrayLength`에 전달한다. |
| Null descriptor | 유효한 non-null description과 null resource는 null/zero view를 만든다. resource와 descriptor가 모두 null이면 invalid call이며 descriptor memory는 null state로 남는다. |

Void `Create*View`의 내부 validation 결과를 공개 HRESULT API처럼 표현하지 않는다. 잘못된 view는 기존 null-descriptor 경로로 처리한다. Descriptor heap의 CPU storage는 초기화되며 shader-visible backing/mapping 실패를 확인한다. 실제 resource·descriptor·사용자 COM 객체의 수명은 앱이 D3D12 규칙대로 유지해야 한다.

## Footprint와 copy

- placed texture footprint의 offset은 512-byte aligned, RowPitch는 **256의 배수**다. 512-byte planar pitch도 유효하며 256으로 강제하지 않는다. minimum row bytes·slice pitch·parent buffer 범위를 검사한다.
- `CopyBufferRegion`은 양쪽 buffer kind 및 source/destination offset+length를 encoding 전에 검사한다.
- `CopyTextureRegion`은 location kind, subresource mip/array/plane, source box, destination XYZ+extent, BC block boundaries와 footprint 산술을 검증한다. BC의 논리 크기와 rounded physical block extent는 null/explicit source box 모두에서 구분하며 아래 계약을 따른다.
- footprint→footprint copy는 실제 row/slice buffer copy를 기록한다. Block shape/bytes와 양쪽 pitch를 사용하며 unsupported/no-effect로 끝내던 import 경로가 아니다.
- depth/stencil copy의 정상 계약은 whole subresource, destination XYZ=0, source box=null이다. R24G8 depth plane은 4-byte low24 UNORM+zero X8이며 float32 bit pattern이 아니다. D24↔native float 변환, D32S8 float/stencil plane 경계는 [capability 정책](CAPABILITIES.ko.md#d24-plane-복사)에 기록한다.
- `CopyResource`는 shape/size/sample과 compatible format 관계를 검사한다. DXGI family copy 및 BC↔정수 texel/R9G9B9E5↔R32 integer bitcast 경로가 있으므로 “reinterpret-format copy 전체 미지원”은 현재 설명이 아니다. 같은 byte 크기만으로 임의 두 format을 허용하지 않는다.
- predicated copy와 emulated RGB32/paired 4:2:2/STANDARD_SWIZZLE conversion도 별도 실행 경로다. scratch/native buffer와 recording storage는 GPU retirement까지 유지한다.

소스 기준은 command-list의 `CopyBufferRegion`/`CopyTextureRegion`/`CopyResource`, device의 `GetCopyableFootprints`와 resource-contract helper다. Copy가 가능한 format과 sample count는 API 조회 및 실제 resource flags와 함께 판정한다.

## Unaligned BC texture와 physical block copy

OPTIONS8의 `UnalignedBlockTexturesSupported`는 native `supportsBCTextureCompression()`으로 gate한다. Null output·정확하지 않은 struct size는 `E_INVALIDARG`다. 이 cap은 **top-level width/height가4의 배수가 아니어도 BC texture 생성이 가능한 계약**이며 임의 BC format·BC1D·RelaxedFormatCasting·OPTIONS13의 pitch/offset 완화를 뜻하지 않는다.

- Resource descriptor·SRV·shader `GetDimensions`는 논리 크기를 유지한다. 예를 들어7×5의 mip는7×5→3×2→1×1이며 physical copy/footprint의 block extent는 각각4×4 block 경계로 올림한다.
- `CopyTextureRegion`은 logical extent와 rounded physical block extent를 구분한다. Null box와 explicit physical box 모두 이 검증을 거친다. Block-aligned origin/interior boundary·physical bounds는 유지하고 buffer↔texture의 native 전송은 **source/destination 논리 extent에 독립 clamp**한다. Mixed8×8↔7×5 texture copy와 predicated staging도 source/destination의 서로 다른 논리 edge를 보존한다.
- Footprint offset512-byte alignment·row pitch256-byte 배수는 그대로다. CPU `ReadFromSubresource`/`WriteToSubresource`의 pitch 계약을 GPU placed footprint 규칙과 혼동하지 않는다. Invalid copy origin/interior boundary·physical range·pitch/offset는 destination을 변경하지 않는 경로로 검증했으며 void copy의 결과를 HRESULT 실패로 표현하지 않는다.
- BC1–7 typed/typeless21 variant와 지정2D array/placed·BC1/BC3 volume·BC1 cube의 실제 shader/copy 증거는 [CORE focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#options8-unaligned-bc-완료와-focused-proof)를 따른다. 기존 format reinterpretation 경로는 확장하지 않았으며 같은 runtime의 BC1↔RGBA16UINT32-byte round-trip은 adjacent regression이다. BC1D·D3D11·공개 SM/FL cap은 변경하지 않았다.

## Reserved/tiled·shared resource

Reserved buffer/2D texture와 `GetResourceTiling`, queue `UpdateTileMappings`/`CopyTileMappings`, list `CopyTiles`는 구현돼 있다. Native placement sparse 지원이 없으면 tiled tier를 광고하지 않는다. 지원하는 texture는 64 KiB tile과 `64KB_UNDEFINED_SWIZZLE` 제약을 사용한다. Reserved dimension/sample·packed-tail의 현재 제약과 tier는 [canonical 지원표](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태)를 따르며, committed `64KB_STANDARD_SWIZZLE` emulation과 reserved sparse layout을 혼동하지 않는다. Mapping update는 queue task로 적용하고 필요한 이전 submission retirement/alias ordering을 지킨다. `NO_HAZARD`는 앱의 hazard 책임을 없애는 플래그가 아니다.

Shared heap/resource/fence와 handle open 경로는 [`d3d12_sharing.cpp`](../src/d3d12/d3d12_sharing.cpp)에 있다. Shared/cross-adapter resource flag와 heap 조건을 검증한다. `ALLOW_CROSS_ADAPTER`는 `SHARED_CROSS_ADAPTER` heap 조건과 연결되지만 단일 Metal adapter에서 한 cross-device/process proof를 이종 물리 GPU 전체의 검증으로 확대하지 않는다. Fence publication은 [fence 계약](FENCE-EVENTS.ko.md#shared-fence의-observer-계약)을 따른다.

## Residency와 DXGI memory budget

`MakeResident`, `Evict`, `SetResidencyPriority`, `EnqueueMakeResident`는 Metal residency-set accounting과 연결된다. Heap-backed allocation의 중복 등록을 피하고 제출 수명을 유지하는 구현과 OS가 물리 VRAM을 강제로 pin/evict한다는 주장은 다르다.

[`dxgi_adapter.cpp`](../src/dxgi/dxgi_adapter.cpp)의 현재 동작은 다음과 같다.

- `QueryVideoMemoryInfo`: Budget=`recommendedMaxWorkingSetSize`, CurrentUsage=`currentAllocatedSize`, AvailableForReservation=0. LOCAL/NON_LOCAL은 같은 native budget/usage를 사용하지만 reservation 저장값은 segment별이다.
- `SetVideoMemoryReservation`: 유효한 node/segment를 검사한 뒤 requested 값을 저장한다. 실제 native budget 변경·우선순위 제어 또는 allocation 확보를 의미하지 않는다.
- Budget notification은 [`owned_budget_notification.hpp`](../src/dxgi/owned_budget_notification.hpp)의 `DxgiBudgetNotifications`가 구현한다. 실제 retained WMT device에서 `recommendedMaxWorkingSetSize`를 읽으며 `QueryVideoMemoryInfo.Budget`와 같은 **64-bit budget**을 비교한다. Usage나 reservation 변경을 budget 변경으로 대신 signal하지 않는다. Source getter와 native ABI는 바꾸지 않았다.
- Native Metal budget-change callback이나 KVO의 전달 보장을 전제하지 않는다. 등록이 있을 때 worker를 lazy 시작하고 nominal **500 ms polling**으로 budget을 관찰한다. `QueryBudget`·등록·worker의 read/compare/`SetEvent`는 같은 직렬화 경로를 사용하므로 첫 worker 관찰 전에 일어난 budget 변화를 baseline 갱신으로 잃지 않는다.
- `RegisterVideoMemoryBudgetChangeNotificationEvent`는 **`EVENT_MODIFY_STATE`만** 있는 Event handle도 받으며, `SYNCHRONIZE`/wait 권한을 요구하지 않는다. 비변경 `NtQueryObject`로 권한과 object type을 검증하고 Event duplicate를 소유한다. 검증·등록 시 `SetEvent`/`ResetEvent`를 호출해 initial signal을 강제하거나 caller의 기존 event 상태를 바꾸지 않는다.
- 성공한 각 등록은 서로 다른 **active opaque cookie**를 갖는다. 같은 event를 여러 번 등록해도 독립 등록이며, caller가 원본 handle을 닫아도 소유한 duplicate로 notification 수명이 유지된다.
- `UnregisterVideoMemoryBudgetChangeNotification`는 비교·signal과 직렬화해 해당 cookie의 등록을 제거하고 **그 duplicate만** 닫는다. 반환 뒤 제거된 등록을 통해 더 signal하지 않으며 다른 등록이나 caller handle을 닫지 않는다. Adapter destruction의 `Stop`은 worker를 join한 다음 adapter/device를 teardown해 source와 event 수명을 보호한다.
- Event는 **budget 재조회 hint**이지 모든 전이를 담는 queue가 아니다. Event signal은 coalesce할 수 있고 sample 사이의 A→B→A 전이는 놓칠 수 있다. 500 ms는 nominal 간격이지 정확한 전달 deadline이 아니며, Windows scheduler budget 전체 의미나 물리 VRAM pin/evict를 구현했다는 뜻이 아니다.

UMA advertised VRAM와 기존 usage/reservation 정책은 notification 구현과 별개로 유지한다. [완료와 focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#dxgi-budget-notification-완료와-focused-proof)는 x64/WoW64 실제 residency fixture와 production helper의 real-Win32-event/controlled-source 관찰을 구분한다. 실제 native fixture에서는 budget 변화와 wake가 모두 0이었으며, **실제 물리/native budget의 positive transition을 관측하거나 강제하지 않았다**. Controlled source의 8 GiB→4 GiB→8 GiB·수명·race 증거를 물리 변화 증거로 확대하지 않으며, 0 reservation 복원을 nonzero reservation test로 부르지 않는다.

## 역사적 보정·증거: 2026-09-19 이후

[0012 frozen patch](patches/0012-d3d12-resources.patch)는 import integration(0001–0007+0009+0010+0013, rejected 0008 제외)에서 placed alignment/overflow, mip-array-plane/cube range, buffer/counter descriptor, heap/mapping failure, copy bounds/footprint를 보정했다. 당시 partial 3D UAV와 buffer RTV의 명시적 거부는 silent ignore/false success를 없애기 위한 중간 단계였으며 후속 구현으로 대체됐다.

당시 `resource-final-build` D3D12 cross-link, [CPU helper test](tests/d3d12-resource-contracts.test.cpp)와 [PE fixture](tests/owned-d3d12-resources.cpp) compile, fresh patch replay와 7개 source hash 일치를 관찰했다. 그 audit worker는 PE를 실행하지 않았다. CPU test는 range/placement/overflow, 256·512 pitch 허용, 384 pitch·misaligned offset·one-byte overrun 거부 및 BC4×4 산술을 검사했다. PE fixture는 placed alignment exact error, depth/stencil plane, typed/raw/structured/null view 및 BC1 tail upload/readback을 검증하도록 작성됐다.

역사적 식별값(현재 binary 보유·새 실행을 뜻하지 않음):

| 산출물 | 당시 SHA-256 |
|---|---|
| 0012 patch | `08fab031ee1f37ca2e903a40806eb5dea87684851a608ffa6f7893afdc994639` |
| D3D12 DLL | `6d656941e0784ea3a373e943e6b818f724094b856d4ead020973a01d33f222e2` |
| CPU test | `8a5a3dc7ee6e5fded15430b6fe8c2535725eac49585c42ec905114027c3cd841` |
| PE fixture | `a78b187d85300f985325d1e4253c264fcb08a442982e6fb3d28c371442695682` |

후속 GPU 결과·failure→fix는 [capability 기록](CAPABILITIES.ko.md#역사적-실행-기록)과 [구현 이력](IMPLEMENTATION-HISTORY.ko.md)에서 확인한다. 이전 `build/owned-completion-20260919/resource-*`와 `build/local/` 증거는 2026-10-02 삭제로 유실됐으며, 현재 retained recovery evidence는 canonical 상태 문서가 구분한다. 오래된 절대 경로/private-tree 빌드 명령을 현재 루트 빌드 절차로 재사용하지 않는다.
