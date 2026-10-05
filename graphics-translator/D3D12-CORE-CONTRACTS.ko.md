# D3D12 core API 계약

2026-10-02 루트 `src/` 기준의 API 진입점·실패·수명 계약 참조다. 빌드는 [README](../README.md), 전체 지원 보고와 남은 구현 후보는 [현재 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태), 실행 증거의 가용성은 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다. 이 문서의 소스 대조는 새 Wine/GPU 실행이 아니다.

## Command list 생성·기록·제출

- `ID3D12Device4::CreateCommandList1`은 **closed** list를 만든다. `GetType()`은 allocator 존재 여부와 무관하게 생성 시 지정한 type을 반환한다. `NodeMask > 1` 또는 `Flags != NONE`은 `E_INVALIDARG`다. 실패 출력은 먼저 null로 정리한다.
- `Reset`은 같은 device·같은 type의 allocator와 같은 device의 initial PSO를 요구한다. recording 상태, allocator 사용 중 여부, 이전 `Close` 실패를 검증한다. 지원하지 않는 명령이나 잘못된 recording 인수는 first error를 보존하고 `Close`에서 드러낸다.
- Queue는 닫힌 list, queue/list type, device 소유권, allocator recording generation과 제출 수명을 확인한다. 번들은 queue에 직접 제출하지 않고 DIRECT list의 `ExecuteBundle`로 확장한다. 상세는 [bundle 계약](D3D12-BUNDLES.ko.md), 제출·retirement는 [queue 계약](QUEUE-ORDER.ko.md)을 따른다.
- allocator의 root/indirect/immediate payload GPU arena는 **초기 4 MiB retained chunk**에서 시작하고, 부족하면 `max(요청 크기, 4 MiB)` chunk를 추가한다. 기존 CPU pointer·Metal buffer·GPU VA를 이동하지 않고 virtual offset을 실제 chunk의 buffer/local offset/address로 resolve한다. Shared allocator의 여러 list와 제출 lease는 같은 chunk 소유권을 GPU retirement까지 유지한다. Reset은 사용 중이면 거부하고 retirement 후 추가 chunk를 해제하며 첫 chunk를 재사용한다. Native buffer 길이·SIZE_T·allocation 실패는 first recording error와 `Close`의 `E_OUTOFMEMORY`로 전달한다. **무제한 recording 지원이나 물리 메모리 고갈 검증을 뜻하지 않는다.** [성장·replay·실패 주입 focused proof와 남은 경계](CORE-IMPLEMENTATION-STATUS.ko.md#rootindirect-arena-성장과-focused-proof)를 따른다.

소스: [`d3d12_device.cpp`](../src/d3d12/d3d12_device.cpp)의 `CreateCommandList1`, [`d3d12_command_list.cpp`](../src/d3d12/d3d12_command_list.cpp)의 `Reset`/`Close`/`RecordError`, [`d3d12_command_allocator.cpp`](../src/d3d12/d3d12_command_allocator.cpp)의 `ReserveGPUHeap`/`ResolveGPUHeap`, [`d3d12_command_allocator.hpp`](../src/d3d12/d3d12_command_allocator.hpp)의 `AllocateGPUHeap`.

## Resource와 protected session

`CreateCommittedResource1`/`CreateHeap1`의 protected-session 인수는 optional이다. null이면 일반 생성 경로로 위임하고, 유효한 null-output capability probe는 기존 `S_FALSE` 계약을 사용한다. 실제 protected session은 `E_NOTIMPL`이며 output을 null로 정리한다. Resource 생성·view·copy·residency·공유 조건은 [resource 계약](D3D12-RESOURCES.ko.md), format/quality 판정은 [capability 정책](CAPABILITIES.ko.md)을 따른다.

## Shader·PSO·pipeline library

현재 DXBC와 DXIL은 project-owned AIR lowering으로 연결된다. import 당시의 “DXIL chunk를 만나면 즉시 `E_NOTIMPL`”은 현재 동작이 아니다. shader stage/opcode별 지원과 검증 경계는 canonical 상태 문서에만 유지한다.

Graphics/compute PSO 생성은 null descriptor를 거절하고 실패 출력을 null로 정리한다. `GetCachedBlob`/`CachedPSO`와 `CreatePipelineLibrary`는 구현돼 있으며 다음을 구분한다.

| 계약 | 현재 경로 |
|---|---|
| Single-PSO cache | descriptor·root signature·shader identity를 포함하는 key와 backend identity를 검증한다. |
| Pipeline library | `ID3D12PipelineLibrary1`의 `StorePipeline`, graphics/compute load, stream `LoadPipeline`, size/serialize를 구현한다. 같은 device의 PSO만 저장하며 같은 이름·다른 descriptor 또는 kind로 load하면 `E_INVALIDARG`다. |
| Serialized library | adapter identity 불일치는 `D3D12_ERROR_ADAPTER_NOT_FOUND`, driver/backend identity 또는 version 불일치는 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`, 손상된 blob은 `E_INVALIDARG`로 구분한다. 타 GPU/빌드 간 영구 호환 캐시로 취급하지 않는다. |
| Shader-cache session/control | Application-managed MEMORY/DISK sessions, exact binary key/value·cross-device backing·persistence/isolation/version/final deletion을 구현한다. null-output creation은 S_FALSE/no mutation, developer-mode APPLICATION_MANAGED disable/enable/clear를 지원한다. [오류 선택·CPU 양 ABI proof](CORE-IMPLEMENTATION-STATUS.ko.md#shadercachesessioncontrol-완료와-focused-proof)를 따른다. Automatic/driver-managed cache·native Metal archive 지원으로 확대하지 않는다. |

소스: [`d3d12_pipeline_library.cpp`](../src/d3d12/d3d12_pipeline_library.cpp), [`d3d12_pipeline_cache.cpp`](../src/d3d12/d3d12_pipeline_cache.cpp), [`d3d12_shader_cache.cpp`](../src/d3d12/d3d12_shader_cache.cpp), device의 `D3D12_FEATURE_SHADER_CACHE`/`CreateShaderCacheSession`/`ShaderCacheControl`.

## Barrier·query·predication

Legacy transition/UAV/aliasing barrier는 pass 경계를 만들고 queue encoder의 fence ordering과 연결된다. Enhanced barrier·render pass도 구현 경로가 있으므로 “legacy barrier만 있음”이나 “enhanced barrier가 모두 미구현”으로 설명하지 않는다. Enhanced barrier의 access/layout validation은 [`d3d12_enhanced_barrier.hpp`](../src/d3d12/d3d12_enhanced_barrier.hpp)와 command-list `Barrier`가 기준이다. 정확한 지원 범위는 [현재 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태)를 따른다.

Timestamp·occlusion·pipeline/SO statistics와 GPU snapshot predication도 현재 구현 경로다. COPY queue timestamp는 아래 계약으로 구현됐으며 [x64/WoW64 focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#copy-queue-timestamp-완료와-focused-proof)를 따른다. 통계 query 하나의 PASS를 모든 shader stage와 모든 workload의 정확성 증명으로 확대하지 않는다.

### Timestamp heap/list/clock 계약

- OPTIONS3의 `CopyQueueTimestampQueriesSupported=TRUE`. COPY timestamp heap은 기존 native GPU counter storage와 GPU resolve를 재사용한다. Native ABI·D3D11·공개 SM/FL 상한은 변경하지 않는다.
- `EndQuery`와 `ResolveQueryData`의 query type은 `D3D12_QUERY_TYPE_TIMESTAMP`다. **COPY list는 COPY_QUEUE_TIMESTAMP heap만**, **DIRECT/COMPUTE list는 일반 TIMESTAMP heap만** 허용한다. 반대 heap/list 조합 및 timestamp `BeginQuery`는 `E_INVALIDARG` recording 오류를 남기며 `Close`에서 실패한다. COPY 지원을 일반 TIMESTAMP heap 사용 허용으로 해석하지 않는다.
- Heap/device ownership, index·count의 overflow-safe 범위, buffer destination/device ownership, 8-byte resolve offset alignment와 destination 범위를 검사한다. Invalid recording 뒤 `Close` 오류 우선순위와 allocator slot 해제는 기존 계약을 따른다.
- `GetTimestampFrequency`는 DIRECT/COMPUTE/COPY에 기존 native GPU ticks/second를 반환한다. `GetClockCalibration`의 GPU 값은 기존 native paired timestamp sample이고, CPU 값은 native 호출을 감싼 **Wine QueryPerformanceCounter 전후 midpoint**다. Native host CPU tick의 epoch/frequency를 Wine QPC와 동일하다고 가정하지 않는다. Null output은 `E_INVALIDARG`이며 native 정보원 실패는 성공으로 위장하지 않는다.
- Timestamp sample은 기록 지점까지의 queue ordering과 연결하고 resolve는 completion 전에 CPU timestamp로 채우지 않는다. [COPY proof](CORE-IMPLEMENTATION-STATUS.ko.md#copy-queue-timestamp-완료와-focused-proof)는 동일 COPY queue의 sample ordering·duplicate resolve·same-slot reuse·guard·exact copy payload를 확인한다. **COPY GPU workload duration 정확성이나 동기화 없는 queue 간 sample 비교**를 증명한 것은 아니다. 기존 fence-dependent cross-queue proof와 구분한다.

## Device removal과 DXGI memory budget 계약

- `ReportRemoval`은 실제 backend 오류의 reason을 게시하고 등록 fence epoch를 terminal removal로 전환한다. Fence의 `GetCompletedValue`는 `UINT64_MAX`, 오류 후 새 `SetEventOnCompletion`은 `DXGI_ERROR_DEVICE_REMOVED`다. [Fence 계약](FENCE-EVENTS.ko.md)을 참고한다.
- **`ID3D12Device5::RemoveDevice()`는 `ReportRemoval(DXGI_ERROR_DEVICE_REMOVED)`를 호출한다.** 기존 first-error 경로를 재사용해 먼저 기록된 removal reason을 보존하고 등록 fence epoch를 terminal로 전환한다. Pending fence event와 queue wait는 해제되며 fence는 `UINT64_MAX`를 보고한다. Blocked DIRECT/COPY queue의 never-submitted 작업은 discard하되 이미 제출된 GPU 소유권은 기존 retirement 경로를 유지한다. 반복 제거는 멱등이다. 새 event 등록·fence 생성 등 관찰한 오류 경로와 기존 removal guard를 모든 API의 일괄 차단으로 확대하지 않는다. [수정 전 실패 → x64/WoW64 focused 완료 증거와 범위](CORE-IMPLEMENTATION-STATUS.ko.md#removedevice-완료와-focused-proof)를 따른다.
- DXGI budget notification은 [`DxgiBudgetNotifications`](../src/dxgi/owned_budget_notification.hpp)가 실제 retained WMT device의 `recommendedMaxWorkingSetSize`를 관찰한다. 이는 `QueryVideoMemoryInfo.Budget`와 같은 64-bit 정보원이며, 등록이 있는 동안 nominal **500 ms polling**으로 변화를 비교해 event를 signal한다. Native Metal budget-change callback/KVO 보장을 전제하지 않는다. `QueryBudget`·등록·worker의 read/compare/`SetEvent`는 직렬화돼 첫 worker 관찰 전에 일어난 변화를 baseline 갱신으로 삼키지 않는다.
- 등록은 `EVENT_MODIFY_STATE`만 필요한 Event handle을 복제해 소유하고, 같은 event의 중복 등록에도 서로 다른 active opaque cookie를 발급한다. 권한·type 검증은 비변경 `NtQueryObject` 조회이며 강제 initial signal/reset이 없다. Caller가 원본 handle을 닫아도 등록은 유지된다. Unregister는 signal과 직렬화해 해당 duplicate만 닫고, `Stop`은 adapter/device teardown 전에 worker를 join한다.
- 알림은 budget을 다시 조회하라는 hint이며 coalescing과 sample 사이 A→B→A 누락이 가능하다. Windows scheduler budget 전체 구현이나 실제 물리 budget 변화의 positive 관측을 주장하지 않는다. UMA VRAM·usage/reservation 정책은 바꾸지 않았다. [완료와 x64/WoW64 focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#dxgi-budget-notification-완료와-focused-proof), 상세 [resource 계약](D3D12-RESOURCES.ko.md#residency와-dxgi-memory-budget)을 따른다.

## 역사적 audit: 2026-09-19 import

[0009 frozen patch](patches/0009-d3d12-core-contracts.patch)는 private import snapshot의 `CreateCommandList1`, null protected-session 위임, PSO 입력/출력 정리를 보정했다. 당시 source는 `build/owned-graphics-20260919/dxmt` 복제본에 0007까지 적용한 상태였으며 **루트 소스의 최신 patch series가 아니다**.

당시 x86-64 PE cross-build와 [core fixture](tests/owned-d3d12-core-contracts.cpp) compile은 통과했지만 그 audit worker는 Wine/GPU/game을 실행하지 않았다. Fixture는 closed-state/type, wrong-type/foreign allocator, node/flag/type 오류와 output clearing, null PSO, protected-session null과 `S_FALSE` probe를 확인하도록 작성됐다. 이후 실행 기록은 [구현 이력](IMPLEMENTATION-HISTORY.ko.md)에서 구분한다.

당시 발견한 DXIL·bundle·predication·query·multiple-event·pipeline-library blocker는 후속 구현으로 대체됐다. LLVM backend 파일을 링크하는 것만으로 DXIL resource/signature/op lowering이 생기지 않는다는 설계 근거는 여전히 유효하지만, 당시 제안은 현재 남은 구현 목록이 아니다. Shader 첫 16개 diagnostic의 stage/bytecount/SHA-1/container/HRESULT 기록도 당시 bounded 진단 설명이며 실제 게임의 첫 PSO 관측 증거를 대신하지 않는다.

`build/`의 이전 snapshot·binary·로그는 2026-10-02 삭제로 현재 보유하지 않는다. 당시 compile 관찰을 새 실행이나 현재 artifact로 표현하지 않는다. Frozen patch와 provenance는 그대로 보존한다.

## 참고 계약

- [CreateCommandList1](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device4-createcommandlist1)
- [Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset)
- [CreatePipelineLibrary](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-createpipelinelibrary)
- [RemoveDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice)
