# D3D12 core contracts audit

> **현재 상태 주의 (2026-09-22):** 이 문서는 2026-09-19 import 당시의 audit 기록이다. 이후 루트 `src/`에서 기본 DXIL, timestamp/occlusion query, bundle, predication, multiple-fence completion, residency 확장 등이 구현되었다. 현재 backlog와 우선순위는 `CORE-IMPLEMENTATION-STATUS.ko.md`를 기준으로 한다. 아래의 당시 blocker 설명은 provenance로 보존한다.

이 문서는 `graphics-translator/patches/0009-d3d12-core-contracts.patch`가 고친 D3D12 기본 계약과 아직 남아 있는 ZZZ/Unity 시작·렌더 경로 blocker를 기록한다. 기준은 `build/owned-graphics-20260919/dxmt`를 복제한 뒤 `0007-queue-order.patch`까지 적용한 private snapshot이다. GPU, Wine, 게임 실행은 이 작업 범위에서 하지 않았다.

## 이번 수정

### ID3D12Device4::CreateCommandList1

기존 구현은 `E_NOTIMPL`만 반환했다. Device4를 노출하는 이상 Unity 또는 middleware가 이 진입점을 합법적으로 사용할 수 있다.

이번 구현은 다음 계약을 보장한다.

- `CreateCommandList1`이 만든 command list는 recording 상태가 아니라 closed 상태로 시작한다.
- list type은 allocator가 없더라도 `GetType()`에서 안정적으로 반환된다.
- `NodeMask > 1`, 정의되지 않은 command-list flags, 지원하지 않는 list type은 `E_INVALIDARG`로 거절한다.
- output pointer는 실패 전에 null로 초기화한다.
- 이후 `Reset()`은 같은 device와 같은 list type의 allocator만 허용한다.
- initial PSO가 전달되면 PSO도 같은 device 소유인지 검증한다.

참고 계약: Microsoft `ID3D12Device4::CreateCommandList1`, `ID3D12GraphicsCommandList::Reset` 문서.

### CreateCommittedResource1 / CreateHeap1

protected-resource session 포인터는 optional이다. 기존 코드는 session이 null이어도 무조건 `E_NOTIMPL`이었다.

이번 구현은 session이 null이면 기존 `CreateCommittedResource` / `CreateHeap` 경로로 위임한다. 따라서 일반 리소스/heap 생성과 `ppResource == nullptr` 또는 `ppHeap == nullptr` capability probe의 기존 `S_FALSE` 동작도 그대로 사용한다. 실제 protected-resource session이 전달된 경우에는 지원하지 않는 기능을 성공으로 위장하지 않고 `E_NOTIMPL`을 유지하며 output을 null로 정리한다.

참고 계약: Microsoft `ID3D12Device4::CreateCommittedResource1`, `ID3D12Device4::CreateHeap1` 문서.

### pipeline-state 입력/출력 계약

graphics/compute PSO helper는 null descriptor를 `E_INVALIDARG`로 거절하고 output pointer를 먼저 null로 초기화한다. compute helper는 기존에 실패 시 caller가 넘긴 output 값을 보존할 수 있었다.

### 첫 shader 종류 진단

실제 ZZZ가 첫 PSO에서 DXBC/SM5 계열을 넘기는지 DXIL을 넘기는지는 실제 게임 실행 전에는 확정할 수 없다. 그래서 shader 초기화 진입점에 bounded diagnostic을 추가했다.

프로세스당 처음 16개 shader 초기화만 다음 메타데이터를 `d3d12.log`의 info level로 남긴다.

- stage: `VS`, `PS`, `CS`
- bytecode byte count
- SHA-1
- container chunk: `DXIL`, `SHEX`, `SHDR`, `invalid-container`, 또는 `unknown`
- 최종 초기화 HRESULT

raw shader bytecode는 기록하지 않는다. DXIL을 발견했을 때도 현재와 동일하게 `E_NOTIMPL`을 반환하므로 이 진단은 지원 범위를 바꾸지 않는다.

## ResourceBarrier audit

현재 `ResourceBarrier`는 transition/UAV/aliasing barrier를 받을 때 command allocator의 현재 Metal encoder를 끝내는 경계를 만든다. 이 D3D12 경로의 texture/buffer는 hazard tracking이 필요한 곳에서 Metal `untracked` 모드를 사용하므로, encoder 경계 뒤 queue encoder가 fence update/wait를 넣는 현재 방식은 valid legacy barrier에 대해 보수적인 실행 순서를 만든다.

Apple의 Metal 문서도 `MTLHazardTrackingMode.untracked` 리소스에서는 애플리케이션이 barrier/fence/event로 안전한 접근 순서를 직접 보장해야 한다고 명시하고, `MTLFence`를 producer pass에서 update한 뒤 consumer pass에서 wait하는 방식으로 pass 사이 메모리 연산을 순서화한다고 설명한다. 현재 DXMT-derived queue encoder의 fence update/wait 구조는 이 계약과 방향이 일치한다.

이 방식은 resource state를 세밀하게 추적하지 않고 필요한 경우보다 더 자주 encoder를 나누지만, 이번 감사에서 barrier를 생략해 write/read ordering을 깨뜨리는 근거는 찾지 못했다. 따라서 `0009`에서 barrier 본문을 바꾸지 않았다.

split transition barrier는 command list 하나에 국한되지 않는다. `BEGIN_ONLY` 이후 다른 command list에서 `END_ONLY`가 올 수 있으므로 list-local pending-state map을 추가하면 잘못된 검증기가 된다. 현재 구현은 BEGIN과 END 모두 encoder boundary로 처리해 transition을 일찍 완료할 수 있으나 순서 안정성은 보존한다. 정확한 split-barrier state machine을 추가하려면 queue submission 범위의 resource-state tracking이 필요하다.

참고 계약: Microsoft `Using Resource Barriers to Synchronize Resource States in Direct3D 12`, DirectX-Specification resource barrier 문서, Apple `MTLHazardTrackingMode.untracked`, `MTLFence`, `Synchronizing passes with a fence` 문서.

## DXIL blocker와 가능한 project-owned 경로

현재 PSO frontend는 DXBC container를 읽은 뒤 `DXIL` chunk를 발견하면 즉시 `E_NOTIMPL`을 반환한다. `SHEX`/`SHDR` 경로만 `SM50Initialize`와 기존 `airconv`를 통해 AIR/Metal library로 변환된다. 따라서 실제 ZZZ PSO가 DXIL을 사용하면 이것은 즉시 렌더 시작 blocker다.

로컬 `build/owned-graphics-20260919/llvm-project`는 LLVM `llvmorg-15.0.7` 기반이며 DirectX target, DXIL writer/op-lowering, DXContainer parser, generic LLVM bitcode reader를 포함한다. 그러나 현재 project source에는 기존 DXIL container를 읽어 resource/signature metadata와 `dx.op.*` 의미를 project의 AIR resource/root-signature ABI로 낮추는 frontend가 없다. LLVM 15의 DirectX backend 파일을 링크하는 것만으로 DXIL→Metal 지원이 생기지 않는다.

project-owned 구현 경로는 다음 순서가 현실적이다.

1. 현재 upstream LLVM의 DXIL read/upgrade 구성요소를 project-owned LLVM에 port하거나 project LLVM을 해당 reader가 있는 버전으로 올려, DXIL chunk 안의 LLVM 3.7 계열 bitcode와 DXIL metadata/ops를 현대 LLVM IR로 복원한다.
2. shader model, entry point/signature, SRV/UAV/CBV/sampler metadata와 resource handle 연산을 현재 D3D12 root-signature/descriptor ABI로 매핑하는 DXIL frontend를 `airconv`에 추가한다.
3. `dx.op.*`별 semantic lowering을 AIR intrinsics와 Metal resource access로 구현하고 VS/PS/CS부터 단계적으로 검증한다.
4. 현재 SM5 path와 동일하게 Metal library 생성, reflection, cache key, root-signature specialization에 연결한다.
5. 실제 ZZZ first-PSO probe가 DXIL을 확인한 뒤 그 shader hash부터 fixture로 고정해 compiler coverage를 확장한다.

대안으로 오픈소스 DXIL→SPIR-V frontend와 SPIR-V→MSL backend를 vendoring할 수 있지만, 현재 root-signature/descriptor/AIR ABI와 별도 backend를 접합해야 하므로 현 소스 구조에서는 작은 integration이 아니다. 어떤 경로도 현재 `E_NOTIMPL`을 성공으로 바꾸는 방식으로 대체해서는 안 된다.

관련 upstream 참고: LLVM DirectX/DXIL Architecture, LLVM DXContainer, Microsoft DirectXShaderCompiler DXIL specification.

## 당시 blocker와 현재 루트의 대응

이 절의 원래 blocker 목록은 이후 구현으로 상당 부분 해소됐다. 현재 루트 기준으로는 다음처럼 읽어야 한다.

| 2026-09-19 당시 항목 | 2026-09-22 현재 상태 |
|---|---|
| DXIL PSO hard failure | 기본 VS/PS/CS DXIL frontend/lowering과 실제 frozen-DXIL GPU fixture가 추가됐다. 모든 DXIL/SM 지원은 아니다. |
| query heap / BeginQuery / EndQuery / ResolveQueryData 없음 | timestamp와 occlusion/binary-occlusion이 구현·검증됐다. 정확한 `PIPELINE_STATISTICS`는 아직 미구현이다. |
| `ExecuteBundle` 미구현 | bundle recording/replay와 GPU fixture가 구현·검증됐다. |
| `SetPredication` 미구현 | GPU snapshot 기반 predication과 direct/indirect/copy/resolve/clear 경로가 구현·검증됐다. |
| `SetEventOnMultipleFenceCompletion` 미구현 | logical fence latch 기반 single/ANY/ALL/null event와 CPU/GPU rewind가 구현·검증됐다. |
| `CreatePipelineLibrary / GetCachedBlob` 미구현 | Graphics/compute `GetCachedBlob`과 `CachedPSO`는 구현됐다. `CreatePipelineLibrary`는 여전히 `E_NOTIMPL`이다. |
| residency extensions 미구현 | `MakeResident`, `Evict`, `SetResidencyPriority`, `EnqueueMakeResident`와 residency-set accounting이 구현됐다. |

현재 P1에서 직접 남은 기본 공백은 `AtomicCopyBufferUINT/UINT64`, D3D12 `ResizeBuffers1` present-queue 계약, 정확한 pipeline statistics, Stream Output/SO statistics 등이다. reserved/tiled/shared-handle/enhanced barrier와 pipeline library 전체 계약은 P2 공통 기반으로 추적한다.

기존 actual-game static 조사(`ZZZ-COMPATIBILITY.ko.md`)에서 Unity 초기화가 확인한 feature query는 `D3D12_OPTIONS`, `FEATURE_LEVELS`, `D3D12_OPTIONS5`, `D3D12_OPTIONS7` 네 종류다. 앞의 둘과 OPTIONS7은 구현되어 있고, OPTIONS5 실패는 해당 Unity 경로에서 RT 기능을 비활성화한 뒤 계속 진행한다. 따라서 OPTIONS5의 `E_NOTIMPL`을 이번 core startup blocker로 취급하지 않았다.

## 검증

CPU-only x86-64 Windows cross build에서 `src/d3d12/d3d12.dll` 링크가 통과했다. 별도 `graphics-translator/tests/owned-d3d12-core-contracts.cpp` PE fixture도 `-Wall -Wextra`로 빌드했다. fixture는 다음을 검증하도록 작성했다.

- `CreateCommandList1` closed-state와 `GetType()`
- matching / wrong-type / foreign-device allocator `Reset`
- invalid node mask / flags / list type와 output clearing
- null PSO descriptor와 output clearing
- null protected session의 `CreateCommittedResource1` / `CreateHeap1`
- null output capability probe의 `S_FALSE`

이 worker의 범위에서는 Wine/GPU/game을 실행하지 않았으므로 fixture runtime result와 first-PSO shader kind는 prime의 실제 runtime gate에서 확인해야 한다.
