# D3D12 COMPUTE/COPY multi-queue fixture

[owned-d3d12-multiqueue.cpp](tests/owned-d3d12-multiqueue.cpp)는 별도 COMPUTE/COPY queue의 future fence wait, 32-frame shared UAV 재사용, cross-queue copy와 CPU exact readback을 확인하는 focused fixture다. 전체 D3D12 conformance·게임·성능 시험이 아니다. 현재 queue/fence 구현은 [queue 계약](QUEUE-ORDER.ko.md)과 [fence 계약](FENCE-EVENTS.ko.md), 실제 실행 판정은 [canonical 검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다.

## Workload와 oracle

Fixture는 frame별 COMPUTE allocator/list, COPY allocator/list, READBACK buffer를 마지막 GPU completion까지 유지한다. 한 DEFAULT structured UAV만 32 frame이 공유한다. HLSL은 fixture 안에 있으며 runtime `D3DCompile`의 `cs_5_0` DXBC를 사용한다. NGX/GPTK/D3DMetal API를 사용하지 않으며 `OwnedGraphicsBackendId() == owned-graphics-d3d12`를 확인한다.

Frame N의 순서:

1. COPY queue에 `producerFence=N+1` Wait를 먼저 enqueue한다. Producer signal은 아직 제출하지 않았다.
2. N>0이면 COMPUTE queue가 `copyFence=N`을 Wait하여 이전 frame의 readback 소비 전 UAV overwrite를 막는다.
3. COMPUTE list가 UAV state로 전이하고 frame/index별 deterministic uint32 pattern을 dispatch한다. Frame 0은 초기 UAV state라 첫 전이를 생략한다.
4. COMPUTE list가 COPY_SOURCE로 전이하고 queue가 `producerFence=N+1`을 Signal한다.
5. COPY queue가 frame 전용 READBACK으로 복사하고 `copyFence=N+1`을 Signal한다.

CPU는 frame마다 fence를 기다리지 않는다. 32 frame을 모두 제출한 뒤 `copyFence=32`의 event와 최대 15초 wait를 한 번 수행하고, `producerFence=32`도 완료됐는지 확인한다. 그 뒤 **32×1024=32768 word**를 expected pattern과 bit-exact 비교한다. 두 queue의 `GetDesc().Type`과 64개 list의 `GetType()`도 requested type과 일치해야 한다. 현재 list type은 생성 시 저장한 값이며 allocator의 현재 pointer에 의존하지 않는다.

## 현재 synchronization과 ring 범위

Public queue Wait는 caller를 차단하지 않고 API-time threshold latch를 등록한다. Consumer worker는 qualifying **logical completion publication**까지 뒤 encode를 보류한다. GPU signal은 앞선 Metal 4 결과/status 검증과 resource/allocator lease retirement 뒤 공개된다. “native encodeSignalEvent/encodeWaitForEvent만으로 fence completed value를 결정한다”는 import 설명은 현재 구현이 아니다.

Future signal보다 Wait를 먼저 enqueue하는 이유는 host signal-before-wait 우연에 의존하지 않는지 확인하기 위해서다. 이 fixture는 fence별 monotonic single producer 패턴이며, rewind/mixed producer/shared-process 의미는 별도 `fence-events`/`sharing` proof가 필요하다.

현재 queue ring은 32 slot이다. Logical frame 하나가 여러 task/submission을 만들 수 있으며 frame 수=slot 수가 아니다. Fixture는 frame별 CPU wait를 생략하여 reserve backpressure와 completion retirement 후 slot reuse를 압박한다. 실제 native submission 수와 overlap/성능은 trace·측정으로 확인해야 하며 이 workload의 PASS marker만으로 추론하지 않는다.

## 새 격리 실행 예시

모든 명령은 repo root에서 실행한다. 먼저 [README](../README.md)의 현재 루트 빌드/staging 절차로 **새 runtime**을 준비한다. 아래 `RUN_ROOT`와 `RUNTIME`은 아직 사용하지 않은 경로로 바꾸고 기존 로그/결과를 덮어쓰지 않는다. Runner가 output 아래 전용 prefix를 준비하므로 live game prefix를 전달하지 않는다.

```sh
set -eu
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
export RUN_ROOT=build/local/multiqueue-contract-new
export RUNTIME=build/local/multiqueue-runtime-new
# RUNTIME은 README 절차로 새로 staging한 경로여야 한다.
test ! -e "$RUN_ROOT"
mkdir -p "$RUN_ROOT/fixtures"
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++17 -O2 -Wall -Wextra -Werror -static \
  graphics-translator/tests/owned-d3d12-multiqueue.cpp \
  -o "$RUN_ROOT/fixtures/owned-d3d12-multiqueue.exe" \
  -ld3d12 -ld3dcompiler -luuid
python3 scripts/test-owned-graphics.py \
  --runtime "$RUNTIME" --suite multiqueue \
  --fixture "$RUN_ROOT/fixtures/owned-d3d12-multiqueue.exe" \
  --out "$RUN_ROOT/result" --validation
```

이는 재실행 절차이며 이 문서 갱신에서 실행한 결과가 아니다. Compile 성공과 실제 Wine/GPU PASS를 분리한다. Runner의 `result.json`, 모든 frame exact marker, timeout/exit code, owned-module provenance, Metal validation 및 foreign-backend 판정을 함께 확인한다.

성공 summary 형식:

```text
OWNED_D3D12_MULTIQUEUE_PASS frames=32 words_per_frame=1024 cross_queue_wait_signal=ok exact_readback=ok
```

`OWNED_D3D12_MULTIQUEUE_FAIL` 또는 `_MISMATCH`는 nonzero 종료다. Summary 한 줄만 있고 frame marker/readback이 누락된 출력은 성공 증거가 아니다.

## 역사적 compile 기록: 2026-09-19

당시 llvm-mingw x86-64 cross-compile은 통과했고 PE32+ import에서 D3D12 device/root-signature와 `D3DCOMPILER_47!D3DCompile`을 확인했다. DXGI/NGX/GPTK/D3DMetal import는 없었다. 해당 worker는 GPU를 실행하지 않았으며 그 기록은 compile/import proof다.

당시 output은 `build/owned-graphics-20260919/multiqueue-build/owned-d3d12-multiqueue.exe`, SHA-256은 `dee675a497d8babd7a854f1623b337a4f68fe8de3fd1caba016e204ec757288b`였다. 이전 `build/` 삭제로 현재 binary 보유를 뜻하지 않는다. 후속 관찰된 runtime proof와 현재 retained mixed-cut recovery evidence는 [구현 이력](IMPLEMENTATION-HISTORY.ko.md) 및 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)에서 구분한다. Frozen import 실행 경로를 최신 staging 입력으로 재사용하지 않는다.
