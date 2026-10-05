# D3D12 bundle 실행과 WriteBufferImmediate

2026-10-02 루트 소스의 bundle state·recording·즉시 쓰기 계약 참조다. Source authority는 [`d3d12_command_list.cpp`](../src/d3d12/d3d12_command_list.cpp)의 `ExecuteBundle`/`RecordBundle`/`WriteBufferImmediate`와 [`owned_bundle_recording.hpp`](../src/d3d12/owned_bundle_recording.hpp)다. 전역 tier/지원표는 [현재 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태), 실제 proof의 가용성은 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다. 이 갱신은 소스 대조이며 새 GPU 실행이 아니다.

## 실행 계약

번들의 명령을 값으로 보관하고, **같은 device의 recording DIRECT 부모**가 `ExecuteBundle`에서 일반 명령 기록 경로로 재생한다. 이때 부모의 root arguments, render target, viewport, scissor를 현재 값으로 상속한다. PSO는 번들 초기 PSO로 바뀌고 topology는 undefined, view-instance mask는 0으로 시작한다. 번들이 변경한 상태는 부모에 남는다. 따라서 같은 번들을 서로 다른 부모 상태로 반복 실행할 수 있고, 부모 allocator가 만들어진 Metal 명령을 보유하므로 제출 후 번들 Reset에도 이미 기록한 출력은 보존된다.

root constant 배열, VB/IB view, blend factor, immediate-write 배열은 호출 때 복사한다. 사용자 COM 객체 수명은 기존 D3D12 규칙을 따른다. 간접 번들 명령은 count buffer가 없고 root/VB/IB 변경을 포함하지 않아야 한다. 번들 내부에서 금지된 명령은 기록 오류를 보존하고 `Close`에 돌려준다.

`SetSamplePositions`의 bundle 기록·부모 상태 상속·reset/replay는 [현행 Tier1 계약](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)을 따른다. 반면 **view instancing은 구현돼** `SetViewInstanceMask`를 bundle에서 복사·재생하며 mask 변경은 부모에 남는다. `RSSetShadingRate`의 값/combiner도 bundle payload로 보관한다. DXR의 `SetPipelineState1`은 state object 참조를 보유하고 `DispatchRays`는 descriptor를 값으로 복사해 부모에서 재생한다. 0014 당시의 “optional view-instance 미구현”과 draw/dispatch-only 설명을 현재 범위로 사용하지 않는다. 고급 stage/feature acceptance 범위는 canonical 상태 문서를 따른다.

부모가 아직 제출하지 않은 bundle 참조를 추적한다. 해당 참조가 하나라도 있으면 bundle Reset은 실패하며, 모든 부모가 제출되거나 Reset/해제되면 풀린다. allocator generation을 보존해 bundle allocator Reset 후의 실행도 거부한다. 실패한 Close는 Reset에서도 같은 오류를 반환한다. 사용 중인 allocator에 대한 Reset 실패는 기존 명령과 bundle 참조를 보존한다. 미완성 command list 해제 시 allocator 기록 슬롯도 해제한다.

번들에서 명시한 shader-visible descriptor heap은 부모 heap과 같아야 한다. graphics PSO의 RTV/DSV format과 sample description도 번들 내에서 일관되어야 한다.

## 즉시 쓰기

`WriteBufferImmediate`를 DIRECT, COMPUTE, COPY, BUNDLE에서 지원한다. 32-bit 값을 allocator의 불변 GPU 메모리에 복사하고 Metal blit 명령으로 목적지에 쓴다. 목적지를 CPU에서 먼저 수정하지 않는다. 두 pass 경계로 순서를 보장하며 marker mode의 최소 보장보다 보수적으로 직렬화한다. GPU 주소는 4-byte alignment와 실제 allocation 범위로 검증한다. 목적지 VA의 빈 구간을 앞선 allocation으로 오인하지 않는다.

Payload는 부모 allocator GPU retirement까지 유지한다. Arena는 초기4 MiB 이후 retained chunk를 추가하며 기존 payload 주소를 이동하지 않는다. 성장·reset·allocation failure는 [core 계약](D3D12-CORE-CONTRACTS.ko.md#command-list-생성기록제출)을 따른다. 제출 후 bundle Reset 허용은 부모 allocator/resource의 early Reset·해제를 허용하는 의미가 아니다.

## 역사적 실행 기록: 2026-09-19 import

[0014 frozen patch](patches/0014-d3d12-bundles.patch)는 당시 `integration/source`+0012 기준본의 9개 translator 파일에 적용했다. Baseline/patch 해시는 `build/owned-completion-20260919/bundle-evidence/{baseline.json,patch-manifest.json}`에 기록했지만 이전 `build/`는 2026-10-02 삭제로 현재 유실됐다. 아래는 당시 관찰 기록이며 현재 artifact 보유나 이 문서 갱신의 새 실행을 뜻하지 않는다.

`graphics-translator/tests/owned-d3d12-bundles.cpp`를 llvm-mingw로 `-Wall -Wextra -Werror` 빌드했다. translator 전체 191단계 cross-build와 후속 변경 rebuild가 성공했다.

`run-bundles-02/result.json`은 `pass: true`, `exit_code: 0`, `validation: true`, `validation_errors: []`, `foreign_backend_loads: []`다. 전용 `wine-bundles-02` runtime에서 다음 실제 GPU 결과를 확인했다.

- indexed draw의 세 영역 모든 pixel, 상속된 root 인수와 viewport/scissor, 번들 변경 상태의 부모 전파, 호출 뒤 입력 배열 변경 격리.
- queue wait로 실행을 보류한 상태에서 제출 뒤 bundle Reset을 수행해도 이전 출력 보존.
- busy allocator Reset 실패 뒤 원래 명령의 실제 pixel 출력과 pending bundle 참조 보존.
- compute/indirect dispatch 10개 결과, 네 command-list 종류의 immediate write와 guard word.
- invalid command, heap mismatch, allocator generation, cross-device, 여러 부모 참조의 exact HRESULT.

수정 전 `wine-events-01`에 동일 v1 fixture를 실행한 `run-bundles-baseline-01`은 PSO attachment mismatch가 잘못 성공하는 지점을 검출해 예상대로 실패했다. 최종 fixture SHA-256은 `dd28a12da9811fce3a9b4624a18fdb2d1d5f9e8a8b5c84ef6418b6704431464f`다.

이 결과는 해당 synthetic GPU/API 계약의 검증이다. 실제 게임, 전체 DX12 API, frame-generation 지원을 입증하지 않는다. 현재 queue validation·backend 오류 전파는 [queue 계약](QUEUE-ORDER.ko.md), `ReportRemoval(DXGI_ERROR_DEVICE_REMOVED)`에 연결된 명시적 `RemoveDevice()`는 [core 계약](D3D12-CORE-CONTRACTS.ko.md#device-removal과-dxgi-memory-budget-계약)을 따른다. 명시적 제거의 새 fence-events/execution-safety focused proof는 [canonical RemoveDevice 증거](CORE-IMPLEMENTATION-STATUS.ko.md#removedevice-완료와-focused-proof)에 기록하며, 위 bundle fixture가 그 경로를 실행했다는 뜻은 아니다. 후속 advanced bundle/runtime proof와 retained recovery evidence는 [구현 이력](IMPLEMENTATION-HISTORY.ko.md) 및 canonical 검증 범위를 따른다.

## 참고 명세

- [Creating and recording command lists and bundles](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)
- [ExecuteBundle](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executebundle)
- [Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset)
- [ExecuteIndirect](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect)
- [WriteBufferImmediate](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist2-writebufferimmediate)
- [Write-buffer immediate modes](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_writebufferimmediate_mode)

DXMT D3D11의 `ExecuteCommandList`/`FinishCommandList` 명령 재생과 명령 payload 수명 구조를 비교했고, bundle 고유 상속 규칙은 위 D3D12 명세에 맞췄다.
