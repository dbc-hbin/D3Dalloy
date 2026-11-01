# D3D12 queue ordering·retirement 계약

2026-10-02 루트 Metal 4 D3D12 경로의 현재 계약이다. 지원 범위와 실행 증거의 authoritative 판정은 [현재 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태)와 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다. Fence event/shared semantics는 [별도 참조](FENCE-EVENTS.ko.md), 기본 COMPUTE/COPY workload는 [multi-queue fixture](MULTIQUEUE.ko.md)에 기록한다. 아래 명시적 removal의 focused proof는 새 runtime 실행이며, 나머지 계약 갱신은 소스 감사와 기존 기록에 근거한다.

## Queue-local FIFO와 public API

각 queue는 immutable task FIFO, submission worker, ordered completion worker를 가진다. Public `ExecuteCommandLists`/`Signal`/`Wait`/Present capture는 짧은 enqueue mutex로 linearize하며 실제 encode·submission-ring reserve·GPU 완료 대기를 caller의 Wait API에 넣지 않는다. 같은 queue의 뒤 task는 FIFO 순서다. 다른 queue의 task가 자동으로 같은 순서가 되는 것은 아니므로 공유 자원 의존성에는 fence/barrier가 필요하다.

| task | capture·실행 계약 |
|---|---|
| Execute | exact recording/storage, allocator·resource/native payload 수명을 보유하고 worker가 encoder chain을 실행한다. worker는 뒤늦게 바뀐 command-list recording을 다시 읽지 않는다. |
| Signal | 값과 fence/ticket을 capture한다. 앞선 work가 완료·검증·retire된 뒤 public logical value를 게시한다. enqueue·native signal encode만으로 완료가 아니다. |
| Wait | **API-time threshold latch**를 등록한다. threshold가 이미 충족되거나 이후 어느 CPU/GPU producer가 충족하면 한 번 latch된다. consumer worker만 그 completion까지 보류하고 caller는 반환한다. |
| Present | presenter/backbuffer, PresentState snapshot, latency-credit 수명을 보유한다. Wait 뒤의 drawable/native Present encode도 앞질러 실행하지 않는다. |
| Tile mapping | mapping payload와 heap/resource 수명을 보유하며 sparse mapping과 alias hazard ordering을 queue에서 적용한다. |

소스: [`d3d12_command_queue.cpp`](../src/d3d12/d3d12_command_queue.cpp)의 task/`SubmissionThread`/`ProcessSignal`/`ProcessWait`, [`owned_queue_task_fifo.hpp`](../src/d3d12/owned_queue_task_fifo.hpp).

## Logical fence completion과 rewind

[`FenceOrderEpoch`](../src/d3dalloy/d3dalloy_queue_order.hpp)는 CPU/GPU 감소·동일값 signal, 미완료 다중 queue/CPU producer를 허용한다. Public completed value는 최신 게시값이며 monotonic maximum이 아니다. CPU signal과 검증된 GPU completion은 같은 logical mutex로 직렬화한다. 이미 완료한 producer를 다시 적용하지 않아 이후 CPU rewind를 덮어쓰지 않는다.

Wait를 첫 blocked producer에 고정하지 않는다. 다음과 같은 순서에서는 A의 signal이 실제 완료되기 전까지 B의 뒤 encode가 보류된다.

```text
caller: B.Wait(F, 1) -> return
caller: B.Execute(consumer) -> return
caller: A.Execute(producer) -> return
caller: A.Signal(F, 1) -> return
A completion: GPU 결과 검증 -> 수명 해제/retire -> F=1 게시
B worker: latch 충족 -> consumer encode/submit
```

이 completion gate는 temporal history의 CPU encode order와 GPU 의존성을 모순되지 않게 한다. **과거 commit-only gate보다 보수적**이며 CPU/GPU overlap 비용은 별도 성능 측정 대상이다. D3D12 fence wait를 native Metal event 값 snapshot으로 대신하거나 commit-only로 완화한 설계가 현재 구현이라는 설명은 틀리다.

Shared fence도 CPU/retirement publication이 logical authority이며 native event는 wakeup용 serial이다. observer별 high-water가 놓친 threshold crossing을 latch하고 최신 rewind 값을 노출한다. 상세와 capacity 제한은 [shared fence 참조](FENCE-EVENTS.ko.md#shared-fence의-observer-계약)를 따른다.

## Device admission과 atomic64 phase

2026-10-03 구현은 동일 D3D12 device의 일반 GPU 제출에 shared admission ticket을 부여하고, 실제 GPU 완료·inflight 수명 해제까지 보유한다. Atomic64 phase는 앞선 제출이 실제 retire된 뒤 exclusive ticket을 얻어 import → 원래 application shader 1회 실행 → export·canonical view publication → context clear → 실제 GPU retirement를 직렬화한다. Ticket은 counter/CV 기반으로 completion thread에 양도할 수 있으며, thread 소유권이 있는 `std::shared_mutex` lock을 다른 thread에서 해제하지 않는다. Device removal은 대기 중인 admission을 취소한다.

알 수 없는 future producer를 기다리면서 shared ticket을 보유하지 않는다. Present order 대기는 CPU worker에서 ticket 획득 전에 처리하며, 이미 exclusive phase에 들어온 encoding thread는 lexical device admission을 재사용한다. 특히 phase 내부 sparse view 생성이 다시 ordinary shared ticket을 획득하면 안 된다. 이 device-local gate는 다른 process/device와의 합법적인 공유 자원 fence/barrier handoff를 대신하지 않는다.

Phase의 participant bound는 실제 direct/indirect grid와 원래 VS·HS·DS·GS·AS·MS workload에서 계산한다. GPU-written argument/root/SBT 값은 앞선 producer 완료 후 읽으며, resident SIMD 수나 더 큰 retry cap으로 progress를 대신하지 않는다. 72-byte context는 device 수명 동안 주소가 고정되고 GPU timeline에서 publish/clear된다. 일반 RT/WG 포함 D3D12 shader baseline은 Metal 3.2이며, 실제 device atomic64 application role만 phase compilation argument 44로 Metal 4.1을 선택한다.

Work graph의 device-atomic64 node는 queue의 기존 `BeginStandardSwizzlePass`/`EndStandardSwizzlePass`를 재사용해 canonical texture와 cast/MSAA mirror를 import·publish한다. Executor fence와 queue transfer fence를 명시적으로 연결하며, 원래 node → atomic export → canonical publisher → 기존 GPU `Atomic64Phase::ClearContext` 순서로 encode한다. Clear는 CPU mapped context를 조기 수정하는 동작이 아니라 fence 뒤의 GPU fill이며, context·phase·publisher scratch와 admission ticket은 실제 native submission 완료까지 살아 있다. 다음 group의 scratch 재사용도 앞선 group retirement 뒤에만 가능하다. 합법적인 검증 경계는 DispatchGraph 뒤 API barrier·Copy/readback 또는 shared-fence handoff이며, 한 DispatchGraph 안의 임의 overlapping texture/raw alias 실행을 합법적이라고 가정하지 않는다.

이 단락은 현재 구현 계약과 paired-ABI CPU syntax/model 확인을 기록한다. 통합 public GPU progress·helper·typed/group/shared-alias proof나 P3 #15 완료를 뜻하지 않으며, atomic capability promotion은 전체 acceptance 결과를 별도로 요구한다.

소스: [`GPUExecutionGate`](../src/d3d12/d3d12_device.hpp), `CommittingScope`/`AtomicPhasePermit`/`ProcessExecuteCommandLists`, [`d3d12_atomic64_phase.cpp`](../src/d3d12/d3d12_atomic64_phase.cpp), [`d3d12_work_graph.cpp`](../src/d3d12/d3d12_work_graph.cpp).

## GPU 완료·수명·32-slot ring

Queue당 submission ring은 32 slot이다. Slot reserve는 이전 slot의 retirement까지 backpressure하며 Metal 4 command allocator/buffer를 재사용한다. Frame 개수와 native submission 개수는 같지 않다.

Ordered completion worker의 순서는 다음과 같다.

1. Metal 4 submission 완료를 기다리고 status/error를 확인한다.
2. draw-capture/SO status를 검사한다. 누락된 capture record 등은 `ReportRemoval`로 전파한다.
3. Present order/latency credit, RTAS observation/lease 등 completion 소유권을 처리한다.
4. inflight native/resource/allocator lease를 해제하고 ring slot을 retire한다.
5. device가 정상일 때만 pending fence signal을 public logical timeline에 게시한다.

**Public GPU fence completion은 preceding allocator retirement watermark이기도 하다.** 앱이 정상 완료를 관측한 뒤 해당 이전 allocator를 Reset할 수 있어야 한다. CPU `Fence::Signal`로 값을 직접 올리는 것은 GPU work가 완료됐다는 증거가 아니며 이 수명 보장을 대신하지 않는다. 앱의 early allocator Reset이나 resource 조기 파괴를 허용하는 계약도 아니다.

소스: `CommandBufferWaitingThread`, [`owned_submission_ring.hpp`](../src/d3d12/owned_submission_ring.hpp), [`owned_submission_storage.hpp`](../src/d3d12/owned_submission_storage.hpp).

## Removal·queue 종료·Present 경계

Backend 오류의 `ReportRemoval`은 terminal fence `UINT64_MAX`와 pending latch 해제를 연결한다. 늦은 signal은 terminal removal을 rewind하지 않는다. 사용자 `Signal(UINT64_MAX)`는 removal이 아니어서 다시 감소시킬 수 있다.

Queue destructor는 stopping/FIFO close를 게시하고 blocked queue-local latch를 깨워 unresolved Wait를 취소한다. 이때 다른 queue나 앱의 completion event를 성공 signal하지 않는다. 취소 task의 waiter를 해제하고 Present order·frame-latency credit을 돌려준 뒤 worker를 join한다. 이미 publish한 submission은 실제 retirement까지 기다리고 ring/completion worker를 종료한다. “영원히 미신호인 Wait 때문에 destructor가 반드시 block한다”는 0007 설명은 현재 cancellation 경로로 대체됐다.

**공개 `ID3D12Device5::RemoveDevice()`는 `ReportRemoval(DXGI_ERROR_DEVICE_REMOVED)`을 호출한다.** 기존 backend removal 경로를 재사용해 최초 오류 reason과 native submission의 retirement 소유권을 보존한다. 명시적 제거의 API 계약은 [core API 계약](D3D12-CORE-CONTRACTS.ko.md#device-removal과-dxgi-memory-budget-계약)을 따른다.

2026-10-02 새 execution-safety case는 x64와 WoW64 x86에서 실제 GPU copy/readback이 완료·retire된 control을 먼저 확인한 뒤, 미신호 Wait로 막힌 DIRECT/COPY queue의 아직 native submit되지 않은 pending copy 2개가 제거 후 실행되지 않는 것, fence 5개의 terminal 전이와 queue 2개의 bounded destruction을 확인했다. 수정 전 x64는 removal reason이 `S_OK`라 실패했고 수정 후 두 guest ABI는 API/Shader validation 및 foreign backend 오류 없이 통과했다. 이 proof는 **아직 in-flight인 native GPU submission과 제거의 경합을 강제로 만들지 않았다.** 실행 디렉터리·manifest·result의 canonical 근거와 한계는 [RemoveDevice focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#removedevice-완료와-focused-proof)를 따른다.

Color-space setter는 지원값을 저장하며 직접 GPU wait하지 않는다. 다음 Present의 layer/PSO 변경과 resize는 이미 수락한 Present의 encode/GPU retirement와 직렬화한다. Color 전환 또는 resize가 반환한 뒤에만 앞선 Wait gate를 signal하는 순서는 보장하지 않는다. Presenter/inflight ownership, 취소 order와 latency credit을 함께 처리하지만 GPU retirement는 물리 scanout 완료를 뜻하지 않는다. 물리 presentation 경계는 canonical 상태 문서에서 구분한다.

## 역사적 설계와 failure 기록

### 2026-09-19: 0007 commit-only gate

[0007 frozen patch](patches/0007-queue-order.patch)는 다음 future-signal 문제를 해결하려고 queue-local worker를 도입했다.

```text
B.Wait(F1); B.Execute(temporalB);
A.Execute(temporalA); A.Signal(F1);
```

이전 API-thread 즉시 encode는 B temporal history sequence를 먼저 배정하여 D3D12 의존성 A→B와 temporal event 의존성 B→A를 반대로 만들고 deadlock을 유발할 수 있었다. Recording/EvaluateFeature 시점에는 제출 queue를 모르므로 그 시점 거부가 해법은 아니었다.

당시 worker는 producer **commit** 뒤 consumer native GPU wait를 encode했다. `FenceSignalSchedule`은 decreasing signal과 unfinished mixed producer를 `E_NOTIMPL`로 거절하고 settled handoff만 허용했다. allocator strong ref는 task encode까지만 보유했고 cancellation/removal은 별도 과제였다. **이 정책은 현재 완료 계약이 아니다.** `signal_committed` trace는 현재도 native submission commit 진단에 남지만 logical completion을 뜻하지 않는다. 과거 `signal_order_unsupported` 제한을 현재 요구사항으로 사용하지 않는다. Frozen patch는 import provenance로만 남긴다.

당시 CPU scheduler/storage/ring 검사와 D3D12·NVNGX isolated cross-link를 관찰했다. GPU/game 실행 증거가 아니며 private `queue-order-work`를 현재 build source로 재사용하지 않는다.

### 후속 logical fence·SO 오류 전파

문서에 기록된 과거 실행에서 `fence-core-before`는 CPU 10→5의 `E_NOTIMPL` 실패를 재현했다. 후속 `fence-core-after`, `fence-core-multiqueue-proof`, `fence-core-temporal-proof`는 logical latch/rewind/mixed producers 경로를 통과했다. SO integrity의 `probe-fixed-02`, `probe-present-fixed-01`, `proof-fence-events-02`, `proof-present-queues-02`는 오류 공개와 Present/Fence 후속 회귀를 관찰했다.

이전 `build/local/...` runtime·로그·JSON은 2026-10-02 삭제로 현재 유실됐다. 관찰된 failure→fix 기록과 현재 retained recovery proof를 [구현 이력](IMPLEMENTATION-HISTORY.ko.md) 및 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)에서 분리한다. 새 실행·성능·전체 스레드 interleaving의 증명으로 확대하지 않는다.
