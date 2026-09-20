# D3D12 bundle 실행과 WriteBufferImmediate

`0014-d3d12-bundles.patch`는 `integration/source`와 `0012-d3d12-resources.patch`를 합친 기준본에 적용한다. 기준 파일 해시와 최종 패치 해시는 `build/owned-completion-20260919/bundle-evidence/baseline.json`, `patch-manifest.json`에 기록했다. 패치에는 9개 translator 파일이 포함되며 기존 prime source는 직접 수정하지 않았다.

## 실행 계약

번들의 명령을 값으로 보관하고, `ExecuteBundle`에서 부모의 일반 명령 기록 경로로 재생한다. 이때 부모의 root arguments, render target, viewport, scissor를 현재 값으로 상속한다. PSO는 번들 초기 PSO로 바뀌고 topology는 undefined로 시작한다. 번들이 변경한 상태는 부모에 남는다. 따라서 같은 번들을 서로 다른 부모 상태로 반복 실행할 수 있고, 부모 allocator가 만들어진 Metal 명령을 보유하므로 제출 후 번들 Reset에도 이미 기록한 출력은 보존된다.

root constant 배열, VB/IB view, blend factor, immediate-write 배열은 호출 때 복사한다. 사용자 COM 객체 수명은 기존 D3D12 규칙을 따른다. 지원되는 간접 번들 명령은 count buffer가 없고 root/VB/IB 변경을 포함하지 않는 단일 draw/dispatch signature다. 번들 내부에서 금지된 명령은 `Close`에 오류를 돌려준다. optional sample-position/view-instance 기능은 구현되지 않았으므로 bundle에서도 성공으로 위장하지 않는다.

부모가 아직 제출하지 않은 bundle 참조를 추적한다. 해당 참조가 하나라도 있으면 bundle Reset은 실패하며, 모든 부모가 제출되거나 Reset/해제되면 풀린다. allocator generation을 보존해 bundle allocator Reset 후의 실행도 거부한다. 실패한 Close는 Reset에서도 같은 오류를 반환한다. 사용 중인 allocator에 대한 Reset 실패는 기존 명령과 bundle 참조를 보존한다. 미완성 command list 해제 시 allocator 기록 슬롯도 해제한다.

번들에서 명시한 shader-visible descriptor heap은 부모 heap과 같아야 한다. graphics PSO의 RTV/DSV format과 sample description도 번들 내에서 일관되어야 한다.

## 즉시 쓰기

`WriteBufferImmediate`를 DIRECT, COMPUTE, COPY, BUNDLE에서 지원한다. 32-bit 값을 allocator의 불변 GPU 메모리에 복사하고 Metal blit 명령으로 목적지에 쓴다. 목적지를 CPU에서 먼저 수정하지 않는다. 두 pass 경계로 순서를 보장하며 marker mode의 최소 보장보다 보수적으로 직렬화한다. GPU 주소는 4-byte alignment와 실제 allocation 범위로 검증한다. `LookupBufferByVA`가 VA 빈 구간을 앞선 buffer로 잘못 반환하던 범위 검사도 보정했다.

## 검증 결과

`graphics-translator/tests/owned-d3d12-bundles.cpp`를 llvm-mingw로 `-Wall -Wextra -Werror` 빌드했다. translator 전체 191단계 cross-build와 후속 변경 rebuild가 성공했다.

`run-bundles-02/result.json`은 `pass: true`, `exit_code: 0`, `validation: true`, `validation_errors: []`, `foreign_backend_loads: []`다. 전용 `wine-bundles-02` runtime에서 다음 실제 GPU 결과를 확인했다.

- indexed draw의 세 영역 모든 pixel, 상속된 root 인수와 viewport/scissor, 번들 변경 상태의 부모 전파, 호출 뒤 입력 배열 변경 격리.
- queue wait로 실행을 보류한 상태에서 제출 뒤 bundle Reset을 수행해도 이전 출력 보존.
- busy allocator Reset 실패 뒤 원래 명령의 실제 pixel 출력과 pending bundle 참조 보존.
- compute/indirect dispatch 10개 결과, 네 command-list 종류의 immediate write와 guard word.
- invalid command, heap mismatch, allocator generation, cross-device, 여러 부모 참조의 exact HRESULT.

수정 전 `wine-events-01`에 동일 v1 fixture를 실행한 `run-bundles-baseline-01`은 PSO attachment mismatch가 잘못 성공하는 지점을 검출해 예상대로 실패했다. 최종 fixture SHA-256은 `dd28a12da9811fce3a9b4624a18fdb2d1d5f9e8a8b5c84ef6418b6704431464f`다.

이 결과는 해당 synthetic GPU/API 계약의 검증이다. ZZZ 전체 실행, 전체 DX12 API, frame-generation 지원을 입증하지 않는다. Queue의 잘못된 제출을 device-removed 상태로 연결하는 처리는 아직 별도 검토 대상이다.

## 참고 명세

- [Creating and recording command lists and bundles](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles)
- [ExecuteBundle](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executebundle)
- [Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset)
- [ExecuteIndirect](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect)
- [WriteBufferImmediate](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist2-writebufferimmediate)
- [Write-buffer immediate modes](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_writebufferimmediate_mode)

DXMT D3D11의 `ExecuteCommandList`/`FinishCommandList` 명령 재생과 명령 payload 수명 구조를 비교했고, bundle 고유 상속 규칙은 위 D3D12 명세에 맞췄다.
