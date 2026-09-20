# Owned D3D12 COMPUTE/COPY multi-queue smoke

이 테스트는 owned D3D12 backend의 별도 COMPUTE queue와 COPY queue 사이 GPU fence ordering, 32-frame in-flight submission, shared UAV 재사용, cross-queue copy와 CPU exact readback을 검증한다. 기존 smoke/PASS evidence는 수정하거나 재실행하지 않는다.

## 파일과 산출물

- 테스트 소스: graphics-translator/tests/owned-d3d12-multiqueue.cpp
- 빌드 산출물 전용 경로: build/owned-graphics-20260919/multiqueue-build
- backend marker: d3d12.dll!OwnedGraphicsBackendId() == owned-graphics-d3d12

HLSL compute shader는 이 C++ 파일 안에 포함되어 있으며 런타임 D3DCompile cs_5_0으로 DXBC를 만든다. NGX, GPTK, D3DMetal API를 사용하지 않는다.

## 검증 계약

테스트는 32 frame 각각에 별도 COMPUTE allocator/list, COPY allocator/list, READBACK buffer를 만든 뒤 마지막 GPU completion까지 모두 보유한다. 한 개의 DEFAULT heap shared structured UAV만 frame 사이에 재사용한다.

각 frame N의 GPU submission 순서는 다음과 같다.

1. COPY queue에 producerFence=N+1 Wait를 먼저 enqueue한다. 이 시점에는 해당 producer Signal이 아직 제출되지 않았다.
2. N>0이면 COMPUTE queue가 copyFence=N을 Wait한다. 이전 frame COPY가 shared buffer를 private readback으로 소비하기 전 shared UAV를 덮어쓰지 못하게 한다.
3. COMPUTE list가 shared buffer를 COPY_SOURCE -> UNORDERED_ACCESS로 전이하고, frame/index가 들어간 deterministic uint32 pattern을 UAV에 Dispatch한다. frame 0은 초기 UAV state라 첫 전이를 생략한다.
4. COMPUTE list가 shared UAV를 UNORDERED_ACCESS -> COPY_SOURCE로 전이한다.
5. COMPUTE queue가 producerFence=N+1을 Signal한다.
6. 이미 Wait를 걸어둔 COPY queue가 shared COPY_SOURCE buffer를 frame N 전용 READBACK buffer로 CopyBufferRegion한다.
7. COPY queue가 copyFence=N+1을 Signal한다.

CPU에서는 frame별 fence wait를 하지 않는다. 32 frame의 Wait/Execute/Signal을 모두 제출한 후 copyFence=32에 대해서만 SetEventOnCompletion + 최대 15초 WaitForSingleObject를 한 번 수행한다. final copy fence가 완료되면 producerFence=32도 완료되어 있어야 한다.

그 뒤 32개의 READBACK buffer를 순회해 frame당 1024개 uint32_t, 총 32768개 word를 CPU expected pattern과 bit-exact 비교한다.

## queue/list type 검증

두 queue는 각각 D3D12_COMMAND_LIST_TYPE_COMPUTE와 D3D12_COMMAND_LIST_TYPE_COPY로 CreateCommandQueue한다. 생성 직후 ID3D12CommandQueue::GetDesc().Type을 읽어 requested type과 정확히 일치하지 않으면 실패한다.

각 frame에서 만든 ID3D12GraphicsCommandList에도 GetType()을 호출해 COMPUTE list가 COMPUTE, COPY list가 COPY를 반환하는지 검사한다. 32 frame 전체 64개 command list가 모두 이 검사를 통과해야 submission으로 진행한다.

현재 owned source에서 queue Initialize는 requested desc를 저장하고 NodeMask만 1로 normalize하며, queue GetDesc는 저장된 desc를 반환한다. command-list GetType은 현재 allocator의 type_을 반환한다. pinned upstream DXMT는 GetType을 항상 DIRECT로 반환했기 때문에 이 검사는 최근 owned GetType fix를 PE 레벨에서 직접 구분한다.

## cross-queue fence 근거

현재 owned backend의 D3D12 fence는 WMT SharedEvent를 사용한다. GPU Signal은 command buffer에 encodeSignalEvent를 넣고, GPU Wait는 encodeWaitForEvent를 넣는다. 서로 다른 command queue가 동일 ID3D12Fence 값을 통해 synchronization할 수 있는 구조다.

COPY consumer Wait를 producer Signal보다 먼저 API submission하는 것은 의도적이다. D3D12 queue Wait가 future fence value를 기다리는 정상 계약을 확인하고, host-side signal-before-wait 순서에 우연히 의존하는 구현을 통과시키지 않는다.

## 32-slot submission ring과 이 테스트

현재 owned command queue의 submission ring capacity는 queue당 32이다. Wait, ExecuteCommandLists, Signal은 각각 별도 GPU command-buffer submission이므로 32 logical frame은 각 queue에 32개를 훨씬 넘는 submission을 만든다.

테스트 자체는 마지막 copy fence 전까지 CPU fence wait를 하지 않는다. 구현 내부 SubmissionRing::reserve()가 capacity에서 backpressure하고 completion thread가 retire한 slot을 재사용하는 경로는 그대로 실행된다. 따라서 frame마다 CPU wait하는 직렬 smoke와 달리 in-flight queue capacity와 slot reuse를 함께 압박한다.

## 빌드

generic clang++의 host 기본 target은 macOS이므로 x86_64 MinGW target을 명시한다.

    mkdir -p build/owned-graphics-20260919/multiqueue-build

    /opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
      --target=x86_64-w64-mingw32 \
      -std=c++17 -O2 -Wall -Wextra -Werror -static \
      graphics-translator/tests/owned-d3d12-multiqueue.cpp \
      -o build/owned-graphics-20260919/multiqueue-build/owned-d3d12-multiqueue.exe \
      -ld3d12 -ld3dcompiler -luuid

CPU-side cross-compile은 위 명령으로 통과했다. 산출물은 PE32+ x86-64이며 SHA-256은 다음과 같다.

    dee675a497d8babd7a854f1623b337a4f68fe8de3fd1caba016e204ec757288b

import 검사에서는 d3d12.dll의 D3D12CreateDevice/D3D12SerializeRootSignature와 D3DCOMPILER_47.dll의 D3DCompile을 확인했다. DXGI, NGX, GPTK, D3DMetal import는 없다.

이 worker의 검증 범위는 PE cross-compile과 import/정적 검토까지다. 실제 Metal/GPU 실행은 prime이 순차 검증한다.

## 성공 출력

queue type이 맞고 마지막 fence가 bounded wait 안에 완료되며 모든 frame/word가 정확하면 마지막 줄은 다음 형태다.

    OWNED_D3D12_MULTIQUEUE_PASS frames=32 words_per_frame=1024 cross_queue_wait_signal=ok exact_readback=ok

실패는 OWNED_D3D12_MULTIQUEUE_FAIL 또는 OWNED_D3D12_MULTIQUEUE_MISMATCH로 nonzero 종료한다.
