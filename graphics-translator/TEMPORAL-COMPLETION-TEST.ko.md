# D3D12 temporal completion·ordering 검증

현재 `graphics-translator/tests/owned-d3d12-temporal.cpp`의 completion case와 `scripts/analyze-owned-temporal-trace.py`의 판정 계약이다(2026-10-02). 별도 private completion source/PE를 요구하지 않는다. 빌드·fresh runtime/prefix·runner recipe는 [TEMPORAL-TEST](TEMPORAL-TEST.ko.md#현재-루트에서-fresh-빌드실행) 한 곳에 있다.

현재 Metal 4 temporal GPU 결과는 이미 관찰된 증거이며 이번 문서 개편에서 다시 실행한 결과가 아니다. 관찰/retained/유실 경계는 [CORE 검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다. 아래 시퀀스는 acceptance 계약이며 실제 게임·physical presentation·전체 conformance의 PASS 선언이 아니다.

## Future-signal same-feature cross-queue

실제 queue/fence와 같은 feature/generation으로 다음 public API 순서를 만든다.

```text
B.Wait(F, 1)
B.Execute(temporalB)
A.Execute(temporalA)
A.Signal(F, 1)
```

아직 producer가 없는 B의 future Wait는 worker submission을 defer해야 한다. **public Wait/Execute caller를 막으면 안 된다.** Caller sequence와 completion wait는 15초 watchdog 안에 끝나야 한다.

B 제출 직후 F signal 전, allocator 자체는 Reset하지 않고 같은 B list를 96회 Reset/Close한다. 최초 recording 포함 97 record heads는 과거 inline 64 capacity를 넘는다. 그 뒤 caller의 B list extension/list/allocator reference를 release하고 A Execute/Signal을 enqueue한다. Worker는 제출 때 snapshot한 entry/recording identity/allocator를 사용해야 하며 caller의 새 list entry를 다시 읽으면 안 된다. GPU completion까지 exact allocator와 recording lease를 retain해야 한다.

요구 ordering은 다음과 같다. Producer GPU completion을 CPU에서 기다려야 한다는 뜻은 아니다.

```text
B wait_defer(F=1)
A temporal_encode
A signal / commit
A completion validation / retirement / logical publication(F=1)
B registered threshold latch satisfied
B temporal_encode
B completion retires
```

A/B 각 output의 same-frame sanity·outside sentinel도 확인한다. `crossqueue_future_signal` case log의 post_submit_list_reset, reset_count=96, record_heads=97, released caller objects를 backend trace와 대조한다.

## Two-feature opposite-order

별도 feature X/Y와 두 queue에 서로 반대 순서를 기록한다.

```text
queue A: X → Y
queue B: Y → X
```

XA/YA/YB/XB는 서로 다른 app-visible resources를 사용한다. 두 queue는 future CPU-signaled start fence 뒤에서 시작하고 각각 8 iteration을 실행한다. **iteration마다 별도 recording/list를 사용한다.** 완료되지 않은 같은 list를 start gate 뒤에 반복 제출하던 과거 fixture는 D3D12의 pending re-execution 규칙을 위반했고 수정됐다. Closed-list replay는 앞 execution 완료 후 수행하는 별도 case다.

각 queue completion fence는 15초 안에 끝나고 네 output 모두 input tag/sentinel 계약을 통과해야 한다. 목적은 per-device temporal CPU encode→commit serialization이 X/Y와 Y/X의 feature mutex ordering cycle을 만들지 않는지 확인하는 것이다. Ring reservation을 temporal mutex보다 먼저 수행하고 GPU/ring completion wait를 그 mutex 안에 두지 않는 현재 구조와 대조한다.

## Lifetime·generation·rectangle

같은 fixture에서 함께 확인한다.

- feature/state release-before-Execute와 완료 뒤 closed-list replay: retained command payload, resource, scaler generation, scratch residency가 살아 있어야 한다.
- caller ResetHistory=false 같은 generation: reset `1,0`.
- caller ResetHistory=true replay: reset 매번 1.
- G1→G2→G1→G1: actual encode reset `1,1,1,0`.
- typed/typeless 각각 8 continuous frame, caller reset 모두 false.
- output subrect enabled base `(3,5)`의 logical 128×128만 갱신, 136×136 backing의 나머지 sentinel 보존.
- exact HRESULT positive/negative control을 유지한다. Nonzero input offset 자체는 지원하며 각 view의 active rectangle bounds를 검사한다. 이 baseline의64×64 backing에서64×64 active input에 추가 offset을 주면 범위 초과다([현재 입력 계약](D3D12-NGX-FRONTEND.ko.md#현재-입력-범위)).

NGX parameter Reset/Destroy-before-Execute와 public feature Release는 [NGX-TEST](NGX-TEST.ko.md)의 public API fixture가 별도로 검사한다. Completion COM fixture 자체를 NGX facade proof로 세지 않는다.

## Metal 4 trace analyzer

`test-owned-graphics.py --suite temporal`은 성공한 fixture run의 owned backend trace가 정확히 하나인지 확인하고 analyzer를 자동 호출한다. Manual 재분석은 **같은 frozen trace**에 대해 새 report 경로를 사용한다.

```sh
: "${TEMPORAL_TRACE:?fresh temporal run의 실제 owned backend JSONL 경로}"
: "${ANALYSIS_OUT:?아직 없는 분석 JSON 경로}"
python3 scripts/analyze-owned-temporal-trace.py \
  "$TEMPORAL_TRACE" --out "$ANALYSIS_OUT"
```

Analyzer는 다음 consumer-visible 관계를 검사한다.

- `execute_list`와 logical encoder에 현재 temporal encode parent가 있다. Physical Metal 4 submission split과 logical Execute identity를 구분하며 이전 execute의 stale encoder를 재사용해 연결하지 않는다.
- record/evaluation identity와 실제 execution, generation/reset history가 맞고 replay·transition coverage가 있다.
- temporal command-buffer encode/commit 순서가 interleave되지 않고 해당 submission이 성공적으로 complete한다.
- future signal defer→producer commitment→consumer encode 관계와 post-submit list reset/allocator retention 경계가 맞다.
- recorded frame destroy와 completion/retain 수명이 일치한다.

`temporal-trace-analysis.json`의 `pass`, `errors`, histories/crossqueue와 runner `result.json`을 함께 확인한다. Missing trace·case coverage 또는 ordering/lifetime mismatch는 PASS가 아니다. Analyzer는 화면 품질·compositor·실제 loader를 관찰하지 않는다.

## 2026-09-19 completion 이력

당시 worker는 private candidate에 frozen `0007-queue-order.patch`를 적용하고 completion regression PE와 NGX include-compatible PE를 cross-build했다. Wine/GPU/game은 실행하지 않은 **build-only** 단계였다. 그때 제안한 future wait FIFO, immutable recording/allocator retention, generation reset, output subrect는 현재 루트 구현·fixture에 포함돼 있다.

옛 `build/owned-completion-20260919/worker3-temporal-regression/` 경로는 historical provenance이지 현재 source authority나 retained evidence 경로가 아니다. 현재 루트에 patches를 다시 적용하지 않는다. 이후 classic·Metal 4 runtime observations, 삭제 전 artifact 유실, recovery proof는 CORE의 ledger와 구분한다.

2026-10-02 recovery temporal은 양 ABI 각각 16 continuous frames·59 executions·56 frame retirement를 기록한 범위이며 combined acceptance에 들어간 두 건이다. 이는 실제 GPU/trace 관찰이지만 전체 API conformance·physical presentation·ARM64 Wine/FEX 최종 완료로 확대하지 않는다.
