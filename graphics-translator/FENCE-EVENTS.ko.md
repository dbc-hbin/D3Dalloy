# D3D12 fence 완료·이벤트·공유 계약

2026-10-02 루트 소스 기준이다. Queue submission/retirement는 [queue 계약](QUEUE-ORDER.ko.md), 전체 지원 상태와 실제 증거의 가용성은 [canonical 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태)·[검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다. 아래 명시적 removal의 focused proof는 새 Wine/GPU 실행 결과이며, 나머지 계약 갱신은 소스 감사와 기존 기록에 근거한다.

## Logical value와 threshold latch

`GetCompletedValue()`는 현재 **logical value**다. CPU/GPU 감소·동일값 signal과 미완료 다중 producer를 지원하므로 이전 최대값을 영구 반환하지 않는다. 등록된 wait는 threshold 이상 값이 한 번 게시되면 latch되며 이후 rewind로 취소되지 않는다. 반대로 rewind 후 **새로** 등록한 wait는 과거 crossing만으로 충족되지 않는다.

GPU signal은 해당 queue의 ordered completion worker가 앞선 Metal submission 및 draw-capture/SO status를 검증하고 inflight/allocator/native lease를 해제·retire한 뒤 공개한다. Native event 값, encode 또는 enqueue는 완료 authority가 아니다. CPU signal은 logical 값을 즉시 게시하지만 GPU allocator retirement를 대신하지 않는다.

소스: [`dxmt_queue_order.hpp`](../src/dxmt/dxmt_queue_order.hpp)의 `FenceOrderEpoch`/`FenceCompletionLatch`, [`dxmt_fence.cpp`](../src/dxmt/dxmt_fence.cpp)의 `prepareQueueWait`/`prepareWaitValue`, [`d3d12_command_queue.cpp`](../src/d3d12/d3d12_command_queue.cpp)의 `CommandBufferWaitingThread`.

## Single·ANY·ALL·null event

- `SetEventOnCompletion`은 threshold latch를 등록한다. `hEvent=nullptr`이면 호출 스레드가 조건 충족까지 동기 대기한다.
- `SetEventOnMultipleFenceCompletion`은 입력 전체를 검증한다. `ALL`(값 0, 기존 NONE 별칭과 같은 의미)은 모든 지정 threshold, `ANY`는 첫 threshold에서 **한 번만** 알린다.
- 같은 fence를 같은 값 또는 다른 값으로 여러 번 지정할 수 있다. 서로 다른 owned device의 fence도 API가 받아들인다.
- Multi-event group은 모든 callback 설치가 성공한 뒤 활성화된다. 일부 등록 성공을 public 성공으로 반환하거나 ANY의 나머지 completion으로 두 번째 알림을 만들지 않는다.
- Null event는 내부 handle을 만들고 대기가 끝나면 닫는다. Callback은 공유 registration/group state를 보유하며 실패한 group은 cancel하여 뒤늦은 completion이 성공 알림을 만들지 않도록 한다. 앱이 넘긴 event handle과 fence/resource의 수명은 D3D12 사용 규칙대로 유지해야 하며 등록이 앱 handle을 자동 복제한다고 가정하지 않는다.

입력의 잘못된 배열/count/flags/fence는 `E_INVALIDARG`, 등록 allocation 실패는 `E_OUTOFMEMORY`다. 내부 event 생성·OS wait 실패는 Win32 error 기반 HRESULT가 될 수 있다. 실제 device 오류 후 새 event 등록은 `DXGI_ERROR_DEVICE_REMOVED`이며 성공 등록이 아니다. Source authority는 device의 `SetEventOnMultipleFenceCompletion`, fence의 `SetEventOnCompletion`, common `EventListener`다.

## Shared fence의 observer 계약

공유 fence는 공유 section의 [`SharedFenceTimeline`](../src/dxmt/dxmt_shared_fence.hpp)과 kernel mutex, Metal shared-event wakeup을 사용한다. **과거 bounded signal-token ring이나 native reservation의 완료값을 사용하는 모델이 아니다.**

1. CPU signal 또는 검증된 queue retirement가 `publish(value)`를 실행한다. Shared timeline의 최신 logical value와 publication serial을 갱신한다.
2. 각 live observer에는 자신이 아직 소비하지 않은 최대값인 **high-water**를 누적한다. 10→5가 observer 실행 사이에 모두 게시돼도 이미 등록한 threshold 10 wait를 놓치지 않는다.
3. `observeSharedLocked`는 observer peak를 기존 local latch에 먼저 반영한 뒤 최신 logical value로 rewind한다. `GetCompletedValue`는 최신값 5를 노출하지만 기존 threshold 10 wait는 완료 상태를 유지한다.
4. 새 local registration도 같은 observe mutex로 보호한다. 이전 high-water를 drain한 뒤 등록하므로 과거 peak만으로 새 wait를 거짓 완료시키지 않는다.
5. Metal event의 monotonic serial은 **깨우기 신호일 뿐**이다. observer는 공유 상태를 다시 읽으므로 native wakeup의 순서가 바뀌어도 logical publication을 복원한다. Serial은 producer enqueue/reservation 때가 아니라 completion publication 때 할당한다.

Observer 용량은 현재 **1024 slot**이다. Slot이 부족하면 attach 전에 죽은 process의 slot을 회수하고, 여전히 부족하면 초기화가 실패한다. 무제한 observer 지원으로 설명하지 않는다. Publication 도중 process가 죽으면 abandoned-owner kernel mutex와 `finishPublication()`의 idempotent replay로 high-water crossing을 보존한다. 종료는 watcher를 정지·join하고 자기 observer slot과 handle/section 소유권을 정리한다.

Source authority: `Fence::initializeShared`, `signal`, `observeSharedLocked`, `markQueueSignalCompleted`, `SharedFenceTimeline::publish`/`take`/`finishPublication`; handle/open 경로는 [`d3d12_sharing.cpp`](../src/d3d12/d3d12_sharing.cpp)다. Sharing proof 하나를 모든 process crash 시점·이종 물리 adapter·OS의 conformance로 확대하지 않는다.

## Device 오류와 취소

실제 backend `ReportRemoval`은 해당 device의 fence epoch를 terminal `UINT64_MAX`로 만들고 기존 single/multi/null/queue 대기를 해제한다. 이후 GPU/CPU signal은 terminal 값을 rewind하지 않는다. 사용자 `Signal(UINT64_MAX)`는 removal과 달라 다시 감소시킬 수 있다.

Queue 종료의 latch wake/cancel은 queue-local이다. 이 취소를 앱 completion event 또는 다른 queue의 성공 signal로 표현하지 않는다. 이미 publish한 native submission의 소유권은 retirement까지 남는다.

**명시적 `ID3D12Device5::RemoveDevice()`는 `ReportRemoval(DXGI_ERROR_DEVICE_REMOVED)`에 연결됐다.** 기존 removal 경로를 재사용하므로 최초 오류 reason을 보존하고 terminal fence 전이·대기 해제를 수행한다. API 계약은 [core API 계약](D3D12-CORE-CONTRACTS.ko.md#device-removal과-dxgi-memory-budget-계약)을 따른다.

2026-10-02 새 fence-events case는 x64와 WoW64 x86에서 single·ANY·ALL 및 각각의 null 동기 대기, shared fence와 opened observer, 낮은 값·`UINT64_MAX` threshold의 대기 해제, 제거 후 새 등록 거부와 반복 호출 멱등을 확인했다. Event gate로 등록·fence 생성과 제거의 경합을 시도하고 모든 관찰 fence의 terminal 상태를 확인했지만, 내부 critical section의 실제 overlap을 보장한 것은 아니다. 수정 전 x64는 removal reason이 `S_OK`라 실패했고 수정 후 두 guest ABI는 API/Shader validation 및 foreign backend 오류 없이 통과했다. 실행 디렉터리·manifest·result의 canonical 근거와 한계는 [RemoveDevice focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#removedevice-완료와-focused-proof)에 기록한다.

## 역사적 실행 기록과 native listener 수명

### 2026-09-19 import: 0013

[0013 frozen patch](patches/0013-fence-events.patch)는 multiple-fence event를 추가하고 공통 native listener의 시작·종료 경합을 보정했다. 당시 10ms 지연 후 raw listener 해제와 run-loop 게시 전 spin은 안전한 수명을 보장하지 못했다. Objective-C retained state, condition lock, run-loop 내 stop, thread join으로 바꾼 설계는 D3D11/native bridge의 listener 경로와 관련되며 현재 D3D12 logical latch의 완료 authority와는 별개다.

PE/Unix `winemetal`은 같은 ABI 빌드여야 한다. 당시 unix-call 104의 배열 payload를 설명한 기록이 현재 전체 ABI revision을 고정하는 것은 아니다. Runtime staging은 [README](../README.md)와 [runtime 안내](RUNTIME.ko.md)의 manifest/짝 모듈 검사를 따른다.

당시 `wine-events-01`/`run-fence-events-01`에서 async ANY/ALL, 이미 완료한 값, 동일 fence의 복수 값, null 동기 대기, cross-device, invalid 인수·부분 등록 방지, 64회 pending/ready device teardown을 관찰했다. ANY를 auto-reset으로 소비한 뒤 나머지 completion이 두 번째 신호를 만들지 않았고 foreign backend/Metal validation 오류는 없었다. 당시 fixture SHA-256은 `a2fdb18af6a79dbe3252a5b94d4c75ce1202c8d81260c6849d97873b3b28298d`다. 이 snapshot의 rewind/mixed producer/shared-process 미지원 설명은 후속 구현으로 대체됐다.

### 후속 logical fence 회귀

[현재 fence fixture](tests/owned-d3d12-fence-events.cpp)의 과거 실행에서 `fence-core-before`는 CPU 10→5 `E_NOTIMPL` 실패를 재현했다. `fence-core-after`는 single/ANY/ALL/null, GPU rewind, mixed/competing producer, API-time wait latch, retained lifetime, 64회 teardown을 통과했다. SO 오류 후 `so-integrity/probe-fixed-02`와 `proof-fence-events-02`는 정상값의 조기 공개를 막는 오류 전파 회귀를 관찰했다. Shared timeline의 CPU 검사 소스는 [shared-fence-timeline.test.cpp](tests/shared-fence-timeline.test.cpp)이며 GPU/process proof와 구분한다.

이전 `build/owned-completion-20260919/...`와 `build/local/...` runtime·JSON·로그는 2026-10-02 삭제로 현재 유실됐다. Frozen patch·fixture 소스와 당시 관찰 기록만으로 새 runtime PASS라고 선언하지 않는다. 현재 retained recovery evidence는 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거), 상세 경로는 [구현 이력](IMPLEMENTATION-HISTORY.ko.md)에서 구분한다. 실제 게임·모든 interleaving의 검증은 아니다.

## 참고 계약

- [SetEventOnCompletion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion)
- [SetEventOnMultipleFenceCompletion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-seteventonmultiplefencecompletion)
- [Multiple fence wait flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_multiple_fence_wait_flags)
- [Metal shared-event notification](https://developer.apple.com/documentation/metal/mtlsharedevent/notify(_:atvalue:block:))
