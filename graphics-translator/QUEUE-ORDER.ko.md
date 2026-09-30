# Owned D3D12 cross-queue CPU encode ordering

## 현재 루트 구현: 검증 후 공개하는 logical fence timeline

`src/dxmt/dxmt_queue_order.hpp`, `dxmt_fence.*`, `src/d3d12/d3d12_command_queue.cpp`는 아래 역사적 0007의 fence 제한을 대체한다.

- CPU/GPU 감소 signal과 미완료 다중 queue/CPU producer를 허용한다. GPU signal은 순차 completion 스레드가 앞선 submission의 Metal 결과와 SO status를 검증한 뒤 논리값으로 공개한다. SO 검증을 우회할 수 있던 native event probe와 signal별 Metal event는 제거했다.
- CPU signal과 검증된 GPU 완료는 같은 logical timeline mutex로 직렬화한다. 이미 공개한 producer는 다시 적용하지 않아 후속 CPU rewind를 덮어쓰지 않는다.
- Queue Wait는 API 호출 시 threshold latch를 등록한다. 이미 충족됐거나 이후 어느 producer가 충족한 wait도 rewind 후 유지된다. 첫 blocked producer에 wait를 고정하지 않는다.
- consumer worker는 실제 qualifying completion까지 후속 encode를 보류한다. API caller는 비차단이며 다른 queue는 계속 진행한다. 기존 commit-only gate보다 보수적이며 CPU/GPU overlap 비용은 별도 측정해야 한다.
- enqueue 실패 시 공개되지 않은 ticket은 소멸한다. inflight submission은 fence/ticket을 GPU retirement까지 보유한다. 별도 producer registry나 native event polling은 없다.
- single/ANY/ALL/null completion event도 같은 logical latch를 사용한다.
- Device 오류는 fence의 terminal `UINT64_MAX`로 모든 등록 대기를 종료하며 늦은 signal로 rewind하지 않는다. 일반 사용자의 `Signal(UINT64_MAX)`는 여전히 rewind할 수 있다. 취소된 Present는 shared order event, GPU-completion watermark와 frame-latency credit을 함께 해제한다.
- D3D12 color-space 전환(2026-09-29): setter는 검증된 다음 색 공간만 저장하며 GPU 완료를 직접 기다리지 않는다. Swapchain mutex로 설정·Present capture·resize를 직렬화한다. 다음 Present의 PSO/layer 변경과 resize는 이미 수락한 Present의 CPU encode뿐 아니라 GPU 완료까지 기다린다. `Rc<Presenter>`는 inflight command buffer가 보유하고 completion 스레드가 실제 Metal 완료 후 `notifyPresentCompleted()`를 호출한다. 여러 queue의 완료 스레드가 역순으로 진행해도 누락된 frame을 완료로 취급하지 않도록 최대 frame id가 아닌 완료 개수를 사용한다. D3D11의 기존 drain 경로는 바꾸지 않았다.
- 색 공간이 바뀌는 다음 Present와 resize는 미신호 queue Wait 뒤의 이전 Present를 기다릴 수 있다. 따라서 그것들이 반환한 뒤에만 gate를 signal하는 호출 순서는 보장하지 않는다. GPU retirement는 물리적 scanout 완료나 Core Animation의 per-drawable 색 공간 latch를 의미하지 않는다. 공개 API·GPU/native 검증은 구현 상태 문서의 color-space 결과를 따른다.

SO 오류 전파 이후 회귀: `build/local/so-integrity/proof-fence-events-02`, `proof-present-queues-02` 및 실제 fault `probe-fixed-02`, `probe-present-fixed-01`은 최종 runtime-02에서 Metal validation과 함께 통과했다. Device-loss 원인과 오류 후 종료 범위는 `CORE-IMPLEMENTATION-STATUS.ko.md`의 SO 추가 결과를 따른다.

검증: `build/local/fence-core-before/result.json`에서 CPU 10→5가 `E_NOTIMPL`로 실패했고, 수정 후 `fence-core-after`, `fence-core-multiqueue-proof`, `fence-core-temporal-proof`의 `result.json`은 모두 PASS다. 각각 새 prefix, Metal API/shader validation, owned runtime 검사를 사용했다. CPU `queue-order-scheduler.test.cpp`도 PASS다. 실제 게임 및 성능 검증은 아니다.

## 아래 내용은 0007 import 시점의 역사적 설계·증거

아래 commit gate, `FenceSignalSchedule`, rewind 거부 설명은 현재 계약이 아니다.

이 변경은 D3D12 `ID3D12CommandQueue::Wait`가 아직 미래인 fence value를 기다리는 동안 **API caller를 block하지 않고**, 그 queue의 뒤쪽 command list/temporal encode만 CPU 측에서 보류하도록 만든다. GPU queue wait 자체는 제거하지 않는다. producer signal이 Metal command buffer까지 commit된 뒤 consumer worker가 원래의 `encodeWaitForEvent`를 기록하므로 최종 GPU ordering은 native fence wait가 계속 담당한다.

작업은 private owned derivative의 isolated `build/owned-graphics-20260919/queue-order-work`에서만 수행했다. prime candidate, game, Wine prefix, runtime staging, GPU 설정은 수정하지 않았다.

## 문제를 만든 실제 ordering

기존 queue는 `ExecuteCommandLists`, `Signal`, `Wait`, `Present` API 호출 스레드에서 즉시 `StartCommitting()`하고 Metal command buffer를 encode/commit한다. 반면 temporal backend의 feature-wide history timeline은 `EncodeOwnedTemporalFrame()`의 **CPU encode 순서**로 sequence를 정한다.

다음은 D3D12에서 가능한 future-signal ordering이다.

```text
B.Wait(F1)
B.Execute(temporalB)
A.Execute(temporalA)
A.Signal(F1)
```

정상 의미는 B의 GPU work가 F1을 기다리고, A temporal + A Signal이 먼저 실행된 뒤 B가 이어지는 것이다. 그러나 기존 CPU encode는 B의 `Wait` command buffer를 즉시 commit하고 바로 `temporalB`까지 encode한다. temporal feature의 global history timeline에서 B가 A보다 먼저 sequence를 받으면, A/B queue의 explicit fence dependency와 temporal SharedEvent dependency가 반대 방향이 되어 deadlock을 만들 수 있다.

`EvaluateFeature` 시점에는 command list가 어느 queue에 제출될지 알 수 없으므로 frontend/recording 단계에서 이 ordering을 거부하는 방식은 올바른 해결이 아니다.

## queue-local worker FIFO

각 `MTLD3D12CommandQueueImpl`에 queue-local `QueueTaskFifo`와 submission worker thread 하나를 둔다. public queue API는 Metal command buffer를 만들지 않고 immutable task만 capture해서 FIFO에 넣고 반환한다.

queue API는 free-threaded이므로 public task capture에는 별도의 짧은 `mutex_enqueue_`를 사용한다. 같은 queue를 여러 CPU thread가 동시에 호출해도 **task capture / fence ticket preparation / FIFO push**가 하나의 linearization order를 갖는다. 이 mutex 범위에는 `StartCommitting`, Metal encode, submission-ring `reserve()`가 들어가지 않는다. 따라서 thread A가 Signal(5) ticket을 먼저 만들고 push 전에 멈춘 사이 thread B의 Signal(10)이 FIFO에서 앞질러 10→5 순서로 encode되는 race를 만들지 않는다.

Signal은 한 단계 더 강하게 fence scheduling mutex도 ticket 준비부터 FIFO push 성공까지 유지한다. 다른 queue/CPU producer는 그 중간 scheduling state를 볼 수 없다. host allocation 실패 또는 이미 닫힌 queue 때문에 FIFO push가 실패하면 준비 전 `FenceSignalSchedule::State`로 rollback하고 해당 HRESULT를 반환한다. 즉 **실제 queue task로 publish되지 않은 Signal은 fence producer/value cursor에도 남지 않는다.**

worker가 처리하는 task는 네 종류다.

| task | API-time capture | worker 동작 |
| --- | --- | --- |
| Execute | 각 list의 `EncoderData *entry`, owned list id, recording id, **exact allocator strong COM ref** | 기존 encoder chain을 그대로 encode/commit |
| Signal | fence strong ref, fence id, exact signal ticket/value | Metal signal command buffer commit 후 producer-committed 게시 |
| Wait | fence strong ref, fence id, exact wait ticket/value | producer commit/CPU signal/completion까지 worker만 defer한 뒤 native Metal GPU wait encode |
| Present | D3D11과 같은 caller-side `PresentState` snapshot, `Rc<Presenter>`, backbuffer strong COM ref, duplicated latency handle, duration | 앞 queue tasks 뒤에서 drawable/Metal Present encode/commit |

queue FIFO이므로 같은 queue 안의 API order는 그대로 유지된다. unresolved Wait가 front task이면 그 queue worker만 멈추고 뒤 Execute/Signal/Present의 CPU encode는 진행하지 않는다. 다른 queue의 worker와 application caller는 계속 진행할 수 있다.

## submission ring backpressure

기존 32-slot `SubmissionRing`의 `reserve()`는 capacity가 가득 차면 slot retirement를 기다린다. 이전 구현에서는 public queue API 호출 스레드가 `StartCommitting()` 안에서 이 backpressure를 직접 밟았다.

새 구조에서는 `StartCommitting()` 호출이 다음 worker-side 네 함수에만 있다.

```text
ProcessExecute
ProcessSignal
ProcessWait
ProcessPresent
```

public `ExecuteCommandLists`, `Signal`, `Wait`, `Present`는 `task_fifo_.push()`까지만 수행한다. 따라서 ring full이 application/device/global lock을 오래 잡고 block시키는 경로를 queue worker로 이동한다.

`mutex_enqueue_`는 queue-local capture serialization만 담당하고 ring backpressure와 무관하다.

task FIFO는 현재 unbounded host deque다. 이는 GPU backlog를 caller-thread stall로 바꾸지 않는 대신 host memory pressure를 무제한으로 흡수할 수 있다는 tradeoff가 있다. 별도 bounded task queue/backpressure 정책은 이번 deadlock correctness fix의 범위에 넣지 않았다.

## command list / allocator lifetime

D3D12 command list object는 `ExecuteCommandLists` 반환 직후 caller가 `Reset`해서 새 recording을 시작할 수 있다. 따라서 worker가 나중에 `list->entry`나 `list->owned_recording`을 다시 읽으면 잘못된 generation을 encode할 수 있다.

추가로 기존 allocator의 record-head container는 `small_vector<EncoderData, 64>`였다. command list를 같은 allocator로 Reset하는 것은 allocator Reset이 아니므로 GPU 완료 전에도 허용되는데, 65번째 head append에서 `small_vector::reserve()`가 기존 head object를 move하면 이미 queue task가 잡은 `entry` pointer가 invalid될 수 있다. strong allocator ref만으로는 이 문제를 막지 못한다.

따라서 record head만 `StableSubmissionStorage<EncoderData>` (`std::deque`)로 바꿔 append가 기존 head reference/pointer를 이동시키지 않게 한다. allocator `Reset()`은 여전히 모든 records를 clear하고 payload를 파괴하므로 GPU 완료 전 allocator Reset 금지 계약은 그대로다.

이번 변경은 API call에서 다음을 snapshot한다.

- `EncoderData *entry`
- list owned id
- recording id
- 그 entry를 소유한 exact `ID3D12CommandAllocator`의 strong COM reference

`MTLD3D12GraphicsCommandList` base에는 최소 coordination field로 raw `ID3D12CommandAllocator *owned_allocator` 하나를 추가하고 list `Initialize`/`Reset` 시 현재 allocator로 갱신한다. queue task는 이를 즉시 strong COM ref로 바꾼다. worker는 제출 뒤 command-list object를 다시 참조하지 않는다.

이 retain은 allocator **파괴**를 막는 것이지 D3D12의 allocator Reset 규칙을 완화하지 않는다. app은 그 allocator의 GPU work가 끝나기 전에 allocator를 Reset하면 안 된다. 기존 allocator CPU heap에는 encoder payload가 있고 early Reset은 invalid application behavior다. 이번 patch는 prime-owned allocator destruction/reset policy를 바꾸지 않는다.

worker의 Execute task가 strong allocator ref를 놓는 시점은 CPU Metal encoding/commit 뒤다. temporal prepared frame처럼 GPU completion까지 별도 retain이 필요한 payload는 기존 `InflightCommandBuffer::temporal_frames`가 계속 잡는다.

## fence producer-commit gate

각 D3D12 fence의 WMT SharedEvent와 함께 CPU-side `FenceOrderEpoch`를 둔다. queue Signal API는 signal ticket을 만들지만 아직 committed로 표시하지 않는다.

Signal worker 순서는 다음과 같다.

1. `StartCommitting()`으로 command buffer/slot 확보.
2. ticket이 잡은 SharedEvent/value에 native signal encode.
3. `CommittingScope`를 끝내 Metal command buffer `commit()` + submission-ring `publish()`.
4. 그 뒤에만 epoch의 `committed_value`를 올리고 condition variable을 깨운다.

Wait worker는 다음 중 하나가 참이 될 때까지 CPU encoding을 defer한다.

- 같은 epoch에서 committed producer signal value가 target 이상이다.
- CPU `ID3D12Fence::Signal`이 target 이상을 즉시 signal했다.
- underlying SharedEvent의 실제 completed/signaled value가 이미 target 이상이다.

ready가 된 뒤에도 consumer command buffer에는 `encodeWaitForEvent(event, value)`를 넣는다. producer **GPU completion**을 CPU에서 기다리지 않는다. producer command buffer commit만 확인하면 GPU dependency는 Metal shared-event wait가 해결한다.

이 구분이 핵심이다. CPU scheduler의 목적은 temporal history의 **CPU encode order**를 D3D12 dependency와 모순되지 않게 만드는 것이고, GPU synchronization 자체를 CPU wait로 직렬화하는 것이 아니다.

## future signal example

문제 ordering에서 실행은 다음처럼 바뀐다.

```text
caller: B.Wait(F1) enqueue -> return
caller: B.Execute(temporalB) enqueue -> return
B worker: Wait(F1) producer 아직 없음 -> CPU encode defer

caller: A.Execute(temporalA) enqueue -> return
A worker: temporalA encode/commit
caller: A.Signal(F1) enqueue -> return
A worker: F1 signal encode/commit -> committed(F1) 게시

B worker: wake -> native GPUWait(F1) encode/commit
B worker: temporalB encode/commit
```

따라서 temporal feature history timeline도 A가 먼저 sequence를 얻고 B가 뒤를 따른다.

## fence value rewind / multi-producer 제한

D3D12 fence value는 원칙적으로 감소 signal도 가능하고 여러 queue/CPU producer가 같은 fence를 signal할 수 있다. Metal `MTLSharedEvent`는 monotonic timeline이므로, 서로 다른 producer가 overlap한 상태에서 완료 순서가 뒤집혀 D3D12 logical value가 내려가는 semantics를 그대로 표현할 수 없다.

기존 코드는 lower value를 보면 새 SharedEvent로 바꾸는 방식이었지만, 이미 queued된 producer/wait와 새 event 사이의 logical fence를 완전히 bridge하지 못하므로 cross-queue rewind를 정확히 보장하지 않았다.

이번 변경은 이를 S_OK로 조용히 처리하지 않는다. `FenceSignalSchedule` 정책은 다음과 같다.

- decreasing value: `E_NOTIMPL`
- 같은 GPU queue에서 equal/increasing value: 지원, equal signal도 실제 producer로 encode하고 elide하지 않음
- 이전 producer의 마지막 scheduled value가 아직 completion 전인데 다른 GPU queue 또는 CPU가 signal: `E_NOTIMPL`
- 이전 scheduled value가 이미 completed된 뒤 producer를 다른 queue/CPU로 handoff: 지원
- initial fence value는 producer ownership을 고정하지 않음

이 제한은 queue `Signal`과 CPU `ID3D12Fence::Signal`에서 HRESULT로 드러나며 queue trace에는 `signal_order_unsupported`가 남는다. 따라서 non-monotonic/mixed-overlap behavior를 조용히 틀리게 실행하지 않는다.

현재 `MULTIQUEUE.ko.md` fixture처럼 fence마다 한 queue가 monotonic producer인 패턴은 이 제한에 걸리지 않는다.

## Present와 queue order

Wait 뒤의 Present도 CPU encode가 앞질러서는 안 되므로 Present 역시 FIFO task다. enqueue 시:

- D3D11과 동일하게 `presenter->synchronizeLayerProperties()`의 `PresentState`를 caller side에서 snapshot
- `Rc<Presenter>` retain
- backbuffer COM retain
- swapchain latency semaphore handle duplicate

를 잡는다. Presenter의 layer/color/pipeline state에는 별도 queue mutex가 없고 D3D11도 이 snapshot을 Present caller에서 만든 뒤 command encode만 defer한다. 따라서 D3D12에서도 display/layer synchronization 자체를 worker thread로 옮기지 않는다. unresolved queue Wait가 defer하는 것은 실제 drawable/Metal command encoding이다.

worker가 실제 Present command buffer를 만들 때 snapshot metadata를 사용하고, encode 직후 `PresentState`를 release해 기존 `frame_presented_` CPU ordering을 알린 다음 command buffer를 commit한다. duplicate handle ownership은 existing `InflightCommandBuffer`에 넘기고 기존 completion thread가 GPU completion 시 signal/close한다.

Present API의 request trace는 swapchain caller 쪽에 남고 실제 `present_encode`는 queue worker 시점에 기록된다.

## queue destruction / cancellation

destructor는 다음 순서다.

1. task FIFO를 close해서 새 task를 받지 않음.
2. submission worker가 이미 accepted된 task를 FIFO 순서로 drain하도록 join.
3. worker가 publish한 마지막 submission sequence까지 `waitUntilRetired`해서 GPU completion thread가 retained temporal frame/Present semaphore를 정리하도록 함.
4. 그 뒤 submission ring을 stop하고 completion thread join.

accepted task나 이미 published된 GPU submission을 destructor가 조용히 버리지는 않는다.

명시적 제한이 하나 있다. queue front에 **실제로 영원히 만족되지 않는 Wait**가 있으면 submission worker drain도 끝날 수 없으므로 queue destruction이 block될 수 있다. 기존 구현도 그런 GPU Wait를 이미 commit한 뒤 completion thread에서 영원히 기다릴 수 있었다. 이번 patch는 device removal/cancel 의미를 새로 발명하거나 task를 drop하지 않는다. device removal 기반 cancellation은 별도 설계가 필요하다.

## CPU scheduler test

새 테스트:

```text
graphics-translator/tests/queue-order-scheduler.test.cpp
```

production의 두 CPU-only helper (`FenceSignalSchedule`/`FenceOrderEpoch`, `QueueTaskFifo`)를 직접 include해서 검사한다.

검증 항목:

1. producer commit 전 wait not-ready, commit 후 ready.
2. event epoch 간 commit state가 섞이지 않음.
3. decreasing signal 거부, overlapping mixed producer 거부, settled producer handoff 허용.
4. equal-value same-producer signal 허용.
5. FIFO close 뒤 새 task 거부 + 이미 accepted된 64 task 정확한 FIFO drain.
6. queued task가 submission storage를 strong-retain하고 worker consume 뒤 release.
7. 같은 allocator에 511번 추가 recording을 append해도 첫 submitted record-head 주소가 유지됨.
8. published submission을 completion thread가 retire한 뒤에만 ring stop sentinel을 보내는 destructor drain 계약.
9. 실제 deadlock 순서 `B.Wait -> B.Execute -> A.Execute -> A.Signal`에서 caller enqueue는 진행되고 event 순서가 `A Execute < A Signal commit < B Wait encode < B Execute`가 됨.

native CPU build/run:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -pthread \
  -Ibuild/owned-graphics-20260919/queue-order-work/src/dxmt \
  -Ibuild/owned-graphics-20260919/queue-order-work/src/d3d12 \
  graphics-translator/tests/queue-order-scheduler.test.cpp \
  -o /tmp/queue-order-scheduler-test

/tmp/queue-order-scheduler-test
```

PASS marker:

```text
QUEUE_ORDER_SCHEDULER_PASS
```

동일 테스트는 `-fsanitize=thread`에서도 PASS했고 normal binary 100회 반복에서도 모두 PASS했다.

## isolated DXMT build validation

source baseline은 pinned DXMT + 0001/0002/0003/0004/0005 + prime의 두 temporal COM output-address fix다. candidate는 수정하지 않았다.

기존 LLVM 15/Wine SDK/llvm-mingw를 사용해 isolated Meson tree `build/owned-graphics-20260919/queue-order-build`을 구성하고 다음 target을 GPU 실행 없이 cross build했다.

```text
d3d12.dll
nvngx.dll
```

두 target 모두 link까지 성공했다. 이 검증은 source/ABI/build correctness이며 Metal GPU execution이나 actual game compatibility를 의미하지 않는다.

## 변경 범위

0007 source delta는 queue scheduling/fence ordering에만 한정한다.

- `src/d3d12/d3d12_command_queue.cpp`
- `src/d3d12/d3d12_command_allocator.hpp`
- `src/d3d12/d3d12_command_list.cpp`
- `src/d3d12/d3d12_device.hpp`
- `src/d3d12/d3d12_fence.cpp`
- `src/d3d12/owned_queue_task_fifo.hpp` (new)
- `src/d3d12/owned_submission_ring.hpp`
- `src/d3d12/owned_submission_storage.hpp` (new)
- `src/dxmt/dxmt_fence.cpp`
- `src/dxmt/dxmt_fence.hpp`
- `src/dxmt/dxmt_queue_order.hpp` (new)

temporal.cpp, command allocator destruction/reset implementation, device.cpp, NGX frontend는 0007에서 수정하지 않는다.

## 남은 runtime 관측

prime의 GPU validation에서 가장 직접적인 확인값은 queue trace 순서다.

```text
B: wait_defer(F1)
A: temporal_encode(...)
A: signal / submit / signal_committed(F1)
B: wait / submit(F1 GPUWait)
B: temporal_encode(...)
```

여기서 producer Signal의 GPU completion까지 CPU에서 기다리면 안 된다. `signal_committed`는 command buffer commit 직후여야 하고, B의 실제 GPU wait가 completion dependency를 유지해야 한다.

또 기존 multi-queue 32-frame fixture를 다시 돌릴 경우 fence별 producer queue가 하나인지와 `signal_order_unsupported`가 0회인지 확인해야 한다. 실제 GPU/game 실행과 최종 staging은 prime 소유다.
