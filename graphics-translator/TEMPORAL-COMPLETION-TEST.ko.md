# Owned D3D12 temporal completion regression

이 문서는 queue scheduler, temporal backend completion 변경을 실제 Windows D3D12 fixture에서 검증하기 위해 추가한 regression 범위를 기록한다. 기존 `TEMPORAL-TEST.ko.md`의 flags 74 typed/ZZZ typeless, exact HRESULT, closed-list replay, generation reset 검증은 그대로 유지한다. 이 작업에서는 Wine, Metal GPU, 게임을 실행하지 않았으며 PE cross-build까지만 수행했다.

## baseline / provenance

- project: `/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12`
- copied candidate commit: `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`
- candidate `build/owned-graphics-20260919/dxmt`는 dirty 상태 그대로 보존했다.
- fixture baseline SHA-256: `7b5421ad5120cffea09f73c51db18f7a4935f42b6574497a4733f19e390c5506`
- queue-order patch `graphics-translator/patches/0007-queue-order.patch` SHA-256: `33ffa6c19792e7aa9b855a806486182bd19f8c11661e69a25ba4e05788fa9dad`
- worker source: `build/owned-completion-20260919/worker3-temporal-regression/work/graphics-translator/tests/owned-d3d12-temporal.cpp`
- private candidate copy: `build/owned-completion-20260919/worker3-temporal-regression/dxmt`

private candidate copy에만 `0007-queue-order.patch`를 `git apply --check` 후 적용했다. live candidate에는 적용하지 않았다.

## future-signal same-feature cross-queue

`crossqueue_future_signal`은 실제 `ID3D12CommandQueue`, `ID3D12Fence`, owned temporal COM extension을 사용해 다음 API 순서를 만든다.

```text
B.Wait(F, 1)
B.Execute(temporalB)
A.Execute(temporalA)
A.Signal(F, 1)
```

두 temporal recording은 같은 feature state/generation을 사용한다. B의 `Wait(F,1)`은 아직 producer가 없는 future wait이므로 queue B worker의 CPU encode는 producer Signal commit 전까지 defer되어야 한다. public `Wait`/`ExecuteCommandLists` 호출은 block하지 않아야 하며 전체 caller sequence는 15초 watchdog으로 bounded한다.

B list를 제출한 직후, F를 아직 signal하기 전에 같은 command allocator를 유지한 채 같은 list를 96회 `Reset`/`Close`한다. 최초 submitted recording을 포함하면 allocator에는 97개의 record head가 생기므로 과거 `small_vector<EncoderData, 64>` inline capacity를 넘는다. allocator 자체 `Reset()`은 하지 않는다.

그 뒤 caller의 B list extension/list/allocator COM reference를 모두 release하고 나서 A Execute와 A Signal을 enqueue한다. queue task가 제출 시점의 immutable entry/recording id와 exact allocator를 retain하지 않았다면 B의 deferred encode가 caller Reset generation을 잘못 읽거나 allocator backing lifetime을 잃게 된다. integrated backend는 submitted allocator를 GPU completion까지 `InflightCommandBuffer`에서도 retain하므로, fixture는 caller release 이후 B completion fence가 실제 완료될 때까지 기다린 다음 A/B output readback을 검증한다.

runtime에서 기대하는 ordering evidence는 다음과 같다.

```text
B wait_defer(F=1)
A temporal_encode(...)
A signal / submit / signal_committed(F=1)
B wait / submit(F=1 native GPU wait)
B temporal_encode(...)
B completion signal retires
```

producer의 GPU completion을 CPU에서 기다리는 것은 기대 동작이 아니다. `signal_committed` 이후 B가 native GPU wait를 encode할 수 있어야 한다.

## two-feature opposite-order regression

`opposite_feature_order`은 별도 temporal feature X/Y 두 개를 만들고, 두 direct queue에 서로 반대 순서의 closed list를 준비한다.

```text
queue A list: X -> Y
queue B list: Y -> X
```

XA, YA, YB, XB 각 evaluation은 color/output/depth/motion을 모두 별도로 만들어 app resource race를 제거한다. 두 queue 모두 future CPU-signaled start fence 뒤에 대기시킨 뒤, 각 list를 8회 enqueue하고 같은 start signal로 풀어 CPU encode contention을 반복한다. queue별 completion fence는 각각 15초 안에 끝나야 하며 네 output readback 모두 입력 tag와 loose constant-color tolerance를 만족해야 한다.

이 case는 per-device temporal submission serialization이 하나의 command buffer 안에서 X/Y와 Y/X feature mutex 순서를 교차시켜 deadlock을 만들지 않는지 검증한다. integrated source에서는 submission-ring `reserve()` 뒤에 device temporal mutex를 잡고, temporal command buffer의 CPU encode부터 `commit()`까지 유지한다. GPU completion wait은 그 mutex 안에 들어가지 않는다.

## positive output subrect

기존 `OutputBaseX=1 -> E_NOTIMPL` negative를 backend completion contract에 맞춰 교체했다.

- create: `OutputSubrectsEnabled = TRUE`
- logical output: 128x128
- backing output: 136x136
- output base: `(3, 5)`
- expected record HRESULT: exact `S_OK`

output validator도 optional base X/Y를 받도록 확장했으며 default는 `(0,0)`이므로 NGX fixture의 기존 호출은 source-compatible하다. readback은 `(3,5)`에서 시작하는 128x128 rectangle 내부가 실제 출력으로 바뀌었는지 확인하고, 그 rectangle 밖의 **모든** 136x136 backing pixel은 원래 sentinel이 보존되어야 한다.

input offset은 아직 지원하지 않는다. `ColorBaseX=1`은 exact `E_NOTIMPL`을 요구하고, 기존 null bias resource + `BiasBaseX=1` negative도 유지한다.

## 기존 replay / format regression 보존

다음 coverage는 삭제하거나 완화하지 않았다.

- valid open-list `TemporalUpscale` exact `S_OK` positive control
- unknown feature flag `E_NOTIMPL`
- closed list `ValidateRecording` / `TemporalUpscale` exact `E_FAIL`
- NaN jitter `E_INVALIDARG`
- nonidentity exposure scale `E_NOTIMPL`
- unsupported input/bias offsets `E_NOTIMPL`
- single-node invalid command-list mask / allocator type mismatch `E_INVALIDARG`
- release-before-execute state lifetime + closed-list replay
- caller `ResetHistory=TRUE` replay
- same feature generation A/B/A/A expected effective reset sequence `1,1,1,0`
- typed 8-frame baseline
- ZZZ typeless parent baseline: color/output 27, motion 33, depth 19 + typed DSV 20

## build-only validation

temporal PE:

```sh
/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
  --target=x86_64-w64-mingw32 \
  -std=c++17 -O2 -Wall -Wextra -Werror -static \
  build/owned-completion-20260919/worker3-temporal-regression/work/graphics-translator/tests/owned-d3d12-temporal.cpp \
  -o build/owned-completion-20260919/worker3-temporal-regression/pe/owned-d3d12-temporal-completion.exe \
  -ld3d12 -luuid
```

NGX include compatibility PE는 helper의 defaulted signature 변경이 기존 `owned-d3d12-ngx.cpp` include path를 깨지 않는지 확인하기 위해 같은 private source directory에서 재컴파일한다. NGX source 자체는 변경하지 않는다.

```sh
/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/x86_64-w64-mingw32-g++ \
  --target=x86_64-pc-windows-msvc \
  -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
  -Ibuild/dlss-translator-20260918/contract/nvidia/include \
  -c build/owned-completion-20260919/worker3-temporal-regression/work/graphics-translator/tests/owned-d3d12-ngx-msvc.cpp \
  -o build/owned-completion-20260919/worker3-temporal-regression/pe/owned-d3d12-ngx-msvc.obj

/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
  --target=x86_64-w64-mingw32 \
  -std=c++17 -O2 -Wall -Wextra -Werror -static \
  build/owned-completion-20260919/worker3-temporal-regression/work/graphics-translator/tests/owned-d3d12-ngx.cpp \
  build/owned-completion-20260919/worker3-temporal-regression/pe/owned-d3d12-ngx-msvc.obj \
  -o build/owned-completion-20260919/worker3-temporal-regression/pe/owned-d3d12-ngx-completion.exe \
  -ld3d12 -luuid
```

최종 PE hash와 import/marker audit는 같은 worker evidence directory의 `provenance.txt`에 기록한다. GPU/Wine/game 실행 결과는 포함하지 않는다.

## runtime PASS contract

전체 GPU regression이 통과하면 temporal fixture 마지막 marker에는 다음 항목이 포함된다.

```text
OWNED_D3D12_TEMPORAL_PASS ... output_subrect=1 crossqueue_future_signal=1 submitted_list_reset=1 record_head_growth=1 allocator_retention=1 opposite_feature_order=1
```

`crossqueue_future_signal`과 `opposite_feature_order` 모두 API caller와 GPU completion을 각각 bounded한다. actual ordering/reset 판정은 output success만으로 추정하지 않고 owned backend trace와 함께 확인해야 한다. GPU 실행과 최종 trace 대조는 prime validation 단계 소유다.
