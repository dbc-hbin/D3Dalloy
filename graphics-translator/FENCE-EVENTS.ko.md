# D3D12 fence 완료 이벤트와 알림 스레드 수명

## 현재 루트 구현 (2026-09-22)

D3D12 단일/다중 완료 이벤트는 이제 native shared-event 값 snapshot이 아니라 logical fence threshold latch를 사용한다. CPU/GPU rewind, 미완료 다중 producer, 동일값 signal을 지원하고 이미 충족된 알림은 이후 rewind로 취소되지 않는다. ANY/ALL 등록은 모든 callback 설치가 성공한 뒤 활성화되며 한 번만 알린다. D3D11/native bridge의 기존 listener는 별도 경로다.

`graphics-translator/tests/owned-d3d12-fence-events.cpp`의 새 회귀는 기존 runtime에서 CPU 10→5 `E_NOTIMPL` 실패를 재현했고(`build/local/fence-core-before`), 수정 runtime에서 single/ANY/ALL/null, GPU rewind, mixed producer, competing queues, API-time wait latch, retained lifetime 및 64회 teardown을 통과했다(`build/local/fence-core-after/result.json`). Metal API/shader validation 오류와 foreign backend 로딩은 없었다. 프로세스 밖 공유 fence와 실제 게임 검증은 이 결과에 포함되지 않는다.

## 아래 내용은 0013 import 시점의 역사적 구현·증거

아래 native listener 설명과 rewind 제한은 당시 snapshot이며 현재 D3D12 경로에는 적용되지 않는다.

`0013-fence-events.patch`는 `ID3D12Device1::SetEventOnMultipleFenceCompletion`을 구현하고, DXMT 공통 Metal shared-event listener의 시작·종료 경합을 수정한다.

## API 계약

`ALL`과 `NONE`은 모든 지정 값이 도달했을 때 한 번 알린다. `ANY`는 첫 완료에서 한 번만 알린다. 동일한 fence를 다른 값 또는 같은 값으로 여러 번 지정할 수 있다. `hEvent == nullptr`이면 내부 이벤트를 사용해 호출 스레드에서 조건 충족까지 기다린다. 서로 다른 owned device의 fence도 수용한다.

공식 계약:

- Microsoft, [SetEventOnMultipleFenceCompletion](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device1-seteventonmultiplefencecompletion)
- Microsoft, [D3D12_MULTIPLE_FENCE_WAIT_FLAGS](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_multiple_fence_wait_flags)
- Apple, [MTLSharedEvent notification](https://developer.apple.com/documentation/metal/mtlsharedevent/notify(_:atvalue:block:))

등록 전에 입력 배열 전체를 검증한다. 따라서 중간의 잘못된 fence가 발견돼도 앞쪽 fence에 일부 이벤트를 남기지 않는다. Native Metal 알림 그룹의 남은 완료 수는 listener의 condition lock으로 보호한다. `ANY`의 나머지 알림은 이미 완료된 그룹을 다시 신호하지 않는다.

## 종료 경합 수정

기존 코드는 장치 종료 때 10ms를 기다린 뒤 raw listener 구조체를 해제했다. Metal 콜백은 그 raw 포인터를 참조하면서 run loop가 설정될 때까지 spin했다. 짧은 대기는 스레드 시작 완료를 보장하지 않으며, 대기 중이거나 늦게 도착한 콜백의 수명도 보호하지 못한다.

새 native 상태는 Objective-C 객체다. 시작할 스레드용 참조는 스레드 실행 전에 예약한다. 복사된 Metal·CFRunLoop 블록은 해당 상태를 보유한다. 상태의 run loop 게시, 종료 플래그, 콜백의 이벤트 전달은 같은 condition lock을 사용한다. 종료 중에는 대기 콜백을 깨우고 추가 이벤트 전달을 막는다. 종료 요청 자체를 run loop 안의 블록으로 실행하므로 `CFRunLoopRun` 진입 직전에 Stop이 유실되는 경합도 없앤다. 마지막으로 Wine 스레드를 join한다. 임의의 10ms 지연과 native spin loop는 제거했다.

공통 WMT 함수는 같은 unix-call 번호 104를 사용하되, 새 PE·Unix 모듈을 함께 빌드한 배열 기반 payload로 전달한다. 기존 단일 fence wrapper도 같은 경로를 사용한다. 서로 다른 빌드의 `winemetal.dll`과 `winemetal.so`를 혼합하면 안 된다. staging manifest가 두 모듈의 해시를 함께 확인한다.

## 실행 증거

실제 검증 런타임:

`build/owned-completion-20260919/wine-events-01`

실행 결과와 DLL 로딩 증거:

`build/owned-completion-20260919/run-fence-events-01/{invocation.json,result.json,stdout.log,stderr.log}`

PE fixture:

`graphics-translator/tests/owned-d3d12-fence-events.cpp`

`build/owned-completion-20260919/fence-events-fixture/owned-d3d12-fence-events.exe`

PE SHA-256: `a2fdb18af6a79dbe3252a5b94d4c75ce1202c8d81260c6849d97873b3b28298d`

Metal debug/shader validation을 켠 별도 Wine prefix에서 async ANY/ALL, 이미 완료된 값, 동일 fence의 여러 값, null 이벤트 동기 대기, 교차 장치, 잘못된 인수와 부분 등록 방지, pending/ready 알림을 포함한 64회 즉시 장치 종료가 통과했다. ANY 이벤트를 auto-reset으로 소비한 뒤 나머지 fence를 완료해도 두 번째 알림이 발생하지 않았다. owned native 모듈 로딩을 확인했고 foreign D3DMetal 로딩과 Metal validation 오류는 없었다.

이 검사는 실제 게임 검증이나 모든 스레드 interleaving의 증명이 아니다. 프로세스 밖 공유 fence와 fence rewind/multiple unfinished producer 제한은 별도 구현 과제로 남아 있다. 이번 패치는 `0007`의 queue signal 정책을 바꾸지 않는다.

## 진단기의 실패 검출

`scripts/test-owned-graphics-diagnostics.py`의 CPU 검사 14개도 통과했다. 성공 문자열이 있어도 누락된 프레임, timeout, 다른 backend 로딩, DLL 전용 로그의 GPU 오류, status=5 완료, 손상된 trace를 성공으로 판정하지 않는지 확인한다. temporal trace 검사는 잘못된 history reset, 기록 후 자원 변경, 누락된 producer commit, GPU 완료 전 프레임 해제와 프레임 누수를 의도적으로 주입해 실패 판정을 확인한다. 이 검사는 Wine이나 GPU를 실행하지 않는다.
