# D3D12 렌더링 코어 소스 구현 상태

## 판정 기준

**현재 판정: P0 기준 고정·통합 안정화 완료. 아래에 명시한 11개 suite의 계약 검증 통과. 전체 Direct3D/DXGI·Shader Model conformance 완료는 아님.**

최초 버전은 사용자 요청에 따라 구현 뒤 검증보다 먼저 작성했다. 이후 다음 우선순위 구현 요청으로 P0 빌드·실행 검증을 재개했고, 컴파일 오류와 실제 GPU 재현에서 발견한 문제를 수정했다. 아래의 **소스 구현**과 **실행으로 확인한 계약**을 구분한다. 검증은 Apple M5 Pro/macOS 27의 x86_64 Wine 경로에 한정하며, suite별 runtime과 evidence를 10절에 기록했다.

현재 개발 기준은 루트 `src/`다. `graphics-translator/patches/`와 과거 조사 문서는 frozen import 및 역사적 기록이며, 이번 변경을 재생성하거나 배포하는 입력으로 사용하지 않았다. 기존 설치본·staged runtime은 현재 소스와 구분해야 한다.

### 직전 구현 단계의 범위

- Resource 생성·heap·view·footprint·allocation·residency.
- Command recording·indirect·predication·copy·resolve·legacy barrier·기본 query.
- 기본 VS/PS/CS shader lowering·root binding·graphics/compute PSO.
- 기존 Queue/Fence 의미와 GPU retirement 수명 유지.

**직전 단계에서 사용자가 분리했던 항목:** 정확한 `PIPELINE_STATISTICS` query. Timestamp·occlusion·clock calibration은 렌더링 코어에 남겼다. 이 분리는 영구 제외가 아니며, 아래의 새 장기 목표에서는 정확한 statistics도 구현 대상이다.

DXR 확장, GS/HS/DS·mesh 확장, NGX/게임별 호환성, pipeline library 및 캐시 성능 확장은 직전 단계의 완료 판정에 포함하지 않았다. 이는 프로젝트의 최종 지원 범위를 제한하는 목록이 아니다.

## 개발 목표와 우선순위

### 목표 변경: 특정 게임 실행 → 최신 규격의 완전한 번역기

**최종 목표는 ZZZ 실행이 아니라, 최신 공개 Direct3D/DXGI 및 shader 규격의 의미를 정확하고 완전하게 구현하는 범용 Direct3D → Metal 번역기다.** ZZZ는 여러 실제 workload 중 하나이며, 실행·화면 출력·FPS 달성이 프로젝트 완료 기준이나 기능 우선순위를 대신하지 않는다.

- D3D12와 자체 DXIL/DXBC → AIR/metallib compiler를 중심으로 API, shader, memory, synchronization, presentation을 end-to-end로 완성한다. 공유 D3D10/11·DXGI 경로도 올바르게 유지한다.
- GPTK/D3DMetal, Apple Metal Shader Converter, SPIR-V fallback에 의존하지 않는 기존 아키텍처를 유지한다.
- “완벽”은 목표이지 현재 달성 주장이나 성공률 수치가 아니다. 누락 기능, 부정확한 emulation, 미검증 조합을 숨기지 않고 남은 작업으로 유지한다.
- Native Metal 대응 기능이 없으면 의미를 보존하는 emulation을 우선 설계한다. 정확한 구현이 불가능한 경우 제약·영향·미충족 계약을 명시한다. No-op, 근사 결과, capability 축소만으로 완전 지원을 달성했다고 판단하지 않는다.

### “최신 기준”의 관리

최신이라는 표현을 특정 버전 번호에 영구 고정하지 않는다. 구현 착수 시점의 **Microsoft 공식 공개 규격과 정식 SDK/Agility SDK, DXIL/Shader Model 명세**를 확인해 기준 revision을 기록한다. Preview/experimental 규격은 정식 기준과 분리해 추적하며 무시하거나 정식 지원으로 섞지 않는다. 아래는 P0에서 조회한 기준이며, 지원 달성 선언이 아니다.

기준표에는 다음을 함께 기록한다.

| 축 | 기록할 내용 |
|---|---|
| D3D12/DXGI | SDK·Agility SDK revision, interface, feature level, OPTIONS/tier, format 및 presentation 계약 |
| Shader | DXIL/DXBC revision, Shader Model, stage/opcode, numeric·memory·subgroup 의미 |
| Metal backend | macOS/SDK·Metal·GPU family, native/emulated 경로, 기능 및 수치 제약 |
| 판정 | 미구현 / 소스 구현·미검증 / 검증 완료 / 제약으로 미충족, 근거 fixture와 실행 환경 |

Vendored header에 enum이 있다는 것, API가 성공을 반환한다는 것, fixture 하나가 통과했다는 것은 지원 증거를 대체하지 않는다. 규격이 갱신되면 차이를 backlog에 반영하며, 현재 지원 선언과 앞으로의 목표를 분리한다.

### P0에서 고정한 규격·backend 기준표

조회 시점은 **2026-09-21 17:48:48 UTC / 로컬 2026-09-22**다. “최신”은 이 시점에 조회한 공식 publication으로 한정한다. SDK package, standalone header release, compiler release의 patch 번호가 같다고 가정하지 않는다.

| 대상 | 정식 목표 | 별도 preview/experimental 추적 |
|---|---|---|
| Agility SDK | Microsoft.Direct3D.D3D12 **1.619.6**, 2026-09-14 게시 | **1.721.3-preview**, 2026-07-30 게시 |
| DirectX-Headers | **v1.619.5**, 2026-07-31 게시 | **v1.721.2-preview**, 2026-07-08 게시 |
| DXC release | **v1.9.2607**, 2026-07-29 게시 | **v1.10.2605.37**, 2026-08-12 게시 |
| Shader Model | **6.9**, DXC v1.9.2602부터 production support 명시 | **6.10 preview** |
| DXIL | **1.9**, 공식 proposal Completed | **1.10**, 공식 proposal Under Review — 정식 지원 기준으로 취급하지 않음 |

공식 근거:

- [Agility 1.619.6 package](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.6), [version/publication feed](https://api.nuget.org/v3/registration5-semver1/microsoft.direct3d.d3d12/index.json).
- [DirectX-Headers v1.619.5](https://github.com/microsoft/DirectX-Headers/releases/tag/v1.619.5), [v1.721.2-preview](https://github.com/microsoft/DirectX-Headers/releases/tag/v1.721.2-preview). 조회 당시 대응하는 1.619.6/1.721.3 standalone header release는 확인하지 못했다.
- [DXC v1.9.2607](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2607), [6.9 production support 명시](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2602), [6.10 preview compiler](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.10.2605.37).
- [SM 6.9 명세](https://github.com/microsoft/DirectX-Specs/blob/5d736f2a61b227ad1e3cc31cd9a718f546041f4e/d3d/HLSL_ShaderModel6_9.md), [DXIL 1.9 proposal](https://github.com/microsoft/hlsl-specs/blob/fb3577a041dc2c146bd447a80b5de501eb0f4509/proposals/0033-dxil19.md), [DXIL 1.10 proposal](https://github.com/microsoft/hlsl-specs/blob/7ba76180209092d644ac74cb66c8a3836001e7b5/proposals/0046-dxil110.md).

| 로컬 계층 | 실제 확인한 기준 | 지원 판정과의 구분 |
|---|---|---|
| 선택된 cross-build header | llvm-mingw toolchain의 `include/d3d12.h`; enum/interface 범위 SM6.7, OPTIONS18, ID3D12Device10 | 정식 목표보다 오래되며 이 범위를 runtime이 모두 지원한다는 뜻이 아님 |
| 저장소 vendored header | `include/native/directx/d3d12.h`: SM6.0, OPTIONS7, ID3D12Device1; upstream revision pin 없음 | 선택된 build header와도 다름. 최신 header 기반 정비는 남은 구현 작업 |
| Cross compiler | llvm-mingw clang **22.1.8**, LLVM commit `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`, x86_64 Windows GNU target | 현재 P0 빌드에 사용한 compiler |
| AIR compiler dependency | LLVM **15.0.7**, `toolchains/llvm-15` | LLVM 버전과 지원 DXIL/SM 버전은 별개 |
| Native OS/SDK | macOS **27.0 (26A428)**, Xcode **27.0 (27A5237l)**, macOS SDK **27.0** | 이 환경의 관찰값이며 다른 OS 조합은 별도 확인 필요 |
| Native compiler | Apple clang **21.0.0**, Metal compiler **32023.921 / metalfe-32023.921.3** | Compiler 존재가 모든 Metal feature 사용을 보장하지 않음 |
| Native GPU | Apple **M5 Pro**, 20 GPU cores, Metal 4. Native query에서 Apple family 1–10/Mac1/Mac2, raytracing, dynamic libraries 지원 응답 | Native capability를 D3D tier 지원으로 승격하지 않음 |
| 현재 D3D12 공개 지원 선언 | **FL11_0 / SM5.1**, DXR tier·mesh tier 미지원 | 일부 SM6 DXIL 입력을 소비하는 것과 SM6 전체 conformance는 다름 |

Native family 의미는 [Apple `supportsFamily`](https://developer.apple.com/documentation/metal/mtldevice/supportsfamily(_:))와 [raytracing capability](https://developer.apple.com/documentation/metal/mtldevice/supportsraytracing)를 기준으로 구분한다. DXIL parser의 version metadata 처리나 개별 lowering 존재만으로 상위 버전 검증 완료를 주장하지 않는다.

### 실행 순서

우선순위는 **정확성·공통 기반·다른 기능의 의존성**으로 정한다. 아래는 단계별 작업과 완료 기준이다. P0는 10절의 명시된 범위에서 완료했으며 P1–P4는 남은 계획이다. 후순위도 최종 범위에 포함한다.

| 우선순위 | 작업 | 해당 단계의 완료 기준 |
|---|---|---|
| **P0 — 기준 고정과 현재 통합본 안정화** | 규격/backend 기준표 작성, 직전 UNVERIFIED 통합본의 build·ABI·실행 문제 수정. 누락 DXIL 입력 준비. Buffer RTV, depth/stencil resolve, typed/multilayer clear, 실제 placed-resource aliasing, predicate/indirect, queue/fence/residency 경계를 확인한다. | 소스와 실행 artifact가 일치하고 해당 계약의 관찰 가능한 결과·오류·수명·ordering이 재현 가능한 증거로 남는다. 알려진 corruption/deadlock/device-loss 문제를 숨기지 않는다. |
| **P1 — 기본 API·shader·presentation 계약의 완전성** | Resource/view/format/heap/map/copy/resolve, root/binding/PSO, recording/query/indirect/predication, queue/fence/residency의 남은 공백을 닫는다. 정확한 pipeline statistics를 다시 포함한다. DXGI swapchain/present/resize/fullscreen/color-space/HDR 및 공유 D3D10/11 경로를 기준표에 따라 다룬다. | 기본 경로의 정상·경계·오류·동시성 계약이 맞고, feature/format 지원 보고와 실제 동작이 일치한다. 부분 native counter를 전체 statistics로 가장하지 않는다. |
| **P2 — 현대 기능의 공통 기반** | 최신 Shader Model의 stage/opcode·numeric·atomic·wave/subgroup·memory semantics와 descriptor indexing/bindless 경로를 완성한다. GS/HS/DS, 요구되는 정밀도/정수 폭도 추적한다. Enhanced barriers, tiled/reserved resource·residency, shared handle/cross-adapter의 수명·가시성·동기화 및 PSO/pipeline-library 계약을 구현한다. | 각 기능의 전체 계약과 의존 기반이 연결된다. Shader 변환 성공만이 아니라 resource binding, GPU 결과, synchronization까지 맞는다. Native 지원이 다른 GPU family는 별도 판정한다. |
| **P3 — 최신 고급 rendering/compute 기능** | DXR의 목표 tier 전체, mesh/amplification, VRS, sampler feedback, Work Graphs 및 기준표에서 확인된 추가 최신 기능을 구현한다. 이 목록은 상한이 아니며 새로운 공개 규격을 계속 반영한다. | 기능별 API·compiler·execution·memory 경로와 tier 요건이 모두 충족된다. 한 DXR/mesh sample의 성공으로 전체 tier를 올리지 않는다. |
| **P4 — 성능·안정성·실사용 완성도** | CPU allocation/copy·Rosetta 비용, submission overlap, shader/PSO compile/cache, native archive 활용, memory pressure, 장시간 실행과 다양한 엔진/workload를 개선한다. NVAPI/NGX 같은 연동 계층도 표준 backend 위에서 완성한다. | 동일 결과·동일 workload에서 CPU/GPU 시간·전력/발열·메모리·stutter를 분리 측정한다. Cold/warm cache와 여러 GPU/OS 조합의 회귀를 확인하며 정확성을 성능과 교환하지 않는다. |

P0를 마친 뒤 공통 계약은 P1, 현대 기능의 기반은 P2, 이를 소비하는 고급 기능은 P3 순서로 진행한다. 독립된 기능은 병렬 구현할 수 있지만 의존 기반이 미검증인 상태를 완료로 승격하지 않는다. P4가 뒤에 있다고 불필요한 allocation/copy나 명백한 성능 결함을 방치한다는 뜻은 아니다. 기본적인 효율은 모든 단계의 구현 원칙이다.

새로운 correctness·보안·메모리 수명·data corruption 문제는 단계와 관계없이 먼저 수정한다. 게임 재현은 공통 계약의 버그를 찾는 입력으로 사용하며, 타이틀별 우회나 특정 장면의 FPS를 위해 규격을 왜곡하지 않는다.

### 공통 완료 기준

1. **계약:** 대상 규격/revision, 유효 입력·오류·수치 허용 오차·ordering·수명·지원 조합이 정의되어 있다.
2. **구현:** API → compiler → Metal 실행 → readback/presentation이 연결되고 지원 선언이 실제 구현과 일치한다. Silent fallback이나 성공하는 stub은 완료가 아니다.
3. **증거:** 독립적으로 기대값을 판단하는 contract fixture, 가능한 경우 reference D3D 실행과의 differential 결과, native validation 및 실제 workload 증거를 구분해 남긴다. Undefined behavior나 규격상 허용 오차를 무조건 bit-exact 비교하지 않는다.
4. **범위:** 한 장치의 통과를 전체 Metal/GPU family 지원으로 일반화하지 않는다. 미검증 조합과 hardware/OS 제약은 기준표에 남긴다.
5. **회귀:** 새로운 계약·경계·오류를 방어하는 재현을 유지한다. 구현 내부나 로그 문구만 검사하는 항목으로 정확성 증거를 대신하지 않는다.

**현재 단계:** 이번 변경으로 P0를 실행·완료했다. 다음 구현 우선순위는 **P1 기본 API·shader·presentation 계약의 완전성**이다. P0의 선별 계약 검증을 P1–P4 완료나 최신 SDK 전체 지원으로 승격하지 않는다. 공개 feature level/Shader Model/tier는 올리지 않았다.

### P1 다음 구현 후보와 실행 순서

2026-09-22 현재 루트 소스를 다시 대조했다. 과거 문서에서 미구현으로 기록한 timestamp/occlusion query, `SetPredication`, `ExecuteBundle`, `SetEventOnMultipleFenceCompletion`, `ResolveSubresourceRegion`, residency 확장과 기본 DXIL 경로는 이후 구현되었으므로 다음 후보에서 제외한다. 현재 소스에서 직접 확인되는 P1 공백과 권장 실행 순서는 다음과 같다.

| 순서 | 후보 | 현재 소스 상태 | 구현 목표 |
|---|---|---|---|
| **P1-A** | `AtomicCopyBufferUINT` / `AtomicCopyBufferUINT64` | `d3d12_command_list.cpp`에서 두 메서드 모두 `IMPLEMENT_ME` | DIRECT/COMPUTE/COPY에서 primary element의 32/64-bit atomic publish와 dependent resource range ordering을 정확히 구현한다. Bundle에서는 허용하지 않는다. |
| **P1-B** | `IDXGISwapChain3::ResizeBuffers1` / present queue rotation | `d3d12_swapchain.cpp`가 `pCreationNodeMask`와 `ppPresentQueue`를 무시하고 단일 `queue_`만 사용 | backbuffer별 queue를 보존하고 `Present`의 buffer 회전과 같은 index로 present queue를 회전한다. Resize를 transactional하게 만들고 실패 시 기존 swapchain 상태를 보존한다. |
| **P1-C** | 정확한 `PIPELINE_STATISTICS` | timestamp/occlusion은 구현됐지만 pipeline-statistics heap/query/result는 없음 | 11개 D3D12 statistics 필드를 정확히 산출한다. 일부 Metal counter만 채운 결과를 전체 statistics 지원으로 광고하지 않는다. |
| **P1-D** | Stream Output + SO statistics | graphics PSO가 `StreamOutput.NumEntries != 0`을 `E_NOTIMPL`, `SOSetTargets`는 `IMPLEMENT_ME` | SO declaration/binding/write offset/filled-size counter와 `SO_STATISTICS_STREAM0..3`를 compiler/runtime/query까지 end-to-end로 구현한다. |

이 순서는 기능 중요도의 절대 순위가 아니라 **현재 구현에서 닫기 좋은 독립 공백 → presentation 기본 계약 → query/compiler를 관통하는 큰 공통 기반** 순서다. 새 correctness·수명·data-corruption 문제가 발견되면 이 순서보다 우선한다.

#### P1-A — AtomicCopyBufferUINT / UINT64 설계

Microsoft 계약상 primary element는 source에서 destination으로 원자적으로 복사되며, 지정한 dependent subresource range는 primary publish 전에 완전히 갱신되어 있어야 한다. `UINT`/`UINT64` source·destination 주소는 각각 자연 정렬을 만족해야 한다. DIRECT/COMPUTE/COPY command list에는 허용되지만 BUNDLE에는 허용되지 않는다.

- record 단계에서 source/destination이 같은 owned device의 buffer인지, 4/8-byte alignment와 bounds, dependency array/count/null 조합, 각 dependent subresource range를 검증한다. Void API이므로 첫 recording error를 기존 방식대로 `Close`까지 보존한다.
- 새 typed pass(예: `AtomicCopyEncoderData`)가 source/destination과 dependent resource/range를 immutable snapshot으로 보존한다. queue worker가 나중에 command-list 객체를 다시 읽지 않도록 기존 deferred-submission lifetime 규칙을 따른다.
- dependent payload의 선행 write가 Metal memory model에서 visibility-complete된 뒤 primary를 publish해야 한다. 단순 4/8-byte blit을 D3D12 atomic copy와 같다고 가정하지 않는다. 지원하는 Metal atomic primitive 또는 검증된 equivalent sequence만 사용한다.
- 64-bit atomic 지원은 현재 M5 Pro 한 장치의 성공을 전체 Metal family로 일반화하지 않는다. 필요한 native capability를 명시적으로 확인하고, exact 구현이 불가능한 조합은 silent fallback 없이 unsupported로 남긴다.
- fixture는 direct/compute/copy list, 32/64-bit, dependency 0/N, natural-alignment/OOB/foreign-device 오류, late-latch 형태의 payload-before-primary 관찰을 포함한다.

공식 계약: [AtomicCopyBufferUINT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist1-atomiccopybufferuint), [command-list API restrictions](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles).

#### P1-B — ResizeBuffers1 / present queue 설계

`ResizeBuffers1`의 `ppPresentQueue`는 backbuffer 수만큼의 D3D12 queue 배열이며, `Present`가 다음 buffer로 회전할 때 queue도 함께 회전한다. 현재 구현은 이 배열을 전부 무시하므로 D3D12-specific swapchain 계약이 빠져 있다.

- swapchain에 backbuffer별 `present_queues_`를 보존한다. 일반 `ResizeBuffers`는 기본 creation queue를 각 slot에 적용하고, `ResizeBuffers1`은 caller 배열을 검증·snapshot한다.
- `Present1`은 현재 backbuffer index와 동일한 index의 present queue에 task를 enqueue한다. 기존 queue-local FIFO/Wait/Present ordering을 재사용한다.
- `ResizeBuffers`와 `ResizeBuffers1`은 공통 internal transaction으로 합친다. 새 descriptor, queue vector, backbuffer vector와 layer property가 모두 준비된 뒤 commit해 중간 실패가 기존 swapchain을 반쯤 파괴하지 않게 한다.
- node mask/queue ownership/type/count를 먼저 검증하고, 기존 backbuffer reference가 남은 resize의 DXGI 오류 계약을 별도 fixture로 고정한다.
- `ApplyLayerProps`/`SetColorSpace1`의 queue flush TODO는 매-frame global wait가 아니라 swapchain mutation 이전의 accepted present work drain/ordering 문제로 다룬다.

공식 계약: [IDXGISwapChain3::ResizeBuffers1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1).

#### P1-C — 정확한 Pipeline Statistics 설계

`D3D12_QUERY_DATA_PIPELINE_STATISTICS`는 `IAVertices`, `IAPrimitives`, VS/GS/clipper/PS/HS/DS/CS invocation 등 11개의 `UINT64` 필드를 요구한다. 현재 WineMetal에는 timestamp counter sample buffer만 노출되어 있고 D3D12 query heap은 timestamp/occlusion만 생성한다.

- `D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS`, `D3D12_QUERY_TYPE_PIPELINE_STATISTICS`를 query heap/list/queue encode/resolve에 연결한다. query slot은 Begin~End 범위의 logical accumulator와 generation/lifetime을 보유한다.
- Apple `MTLCommonCounterSetStatistic`에는 vertex/fragment/compute invocation 및 clipper 관련 counter가 있지만 D3D12 11개 필드와 1:1로 완전 대응하지 않는다. Native statistic counter는 capability를 확인한 뒤 정확성이 입증된 필드에만 사용한다.
- `IAVertices`/`IAPrimitives`와 direct draw에서 정확히 결정 가능한 값은 translator 실행 경로에서 집계할 수 있다. 그러나 predication과 `ExecuteIndirect`는 실제 GPU 실행 결과가 정해지기 전에 CPU recording 시점에서 합산하면 틀리므로 existing predicate/indirect execution path에 GPU-side accumulation을 연결한다.
- shader invocation 필드가 native counter만으로 정확하지 않거나 queue 간 isolation을 보장할 수 없으면 compiler instrumentation을 사용한다. 일부 native 값과 추정치를 섞어 전체 `PIPELINE_STATISTICS`를 지원한다고 광고하지 않는다.
- GS/HS/DS처럼 현재 stage 자체가 미지원인 조합과, 해당 stage가 없는 정상 PSO에서 결과가 0인 경우를 구분한다. 이후 stage 구현 시 statistics contract도 함께 확장한다.
- fixture는 direct/compute, predication true/false, ExecuteIndirect count, multiple query ranges, multi-queue contamination, resolve offset/range 및 Windows reference differential을 포함한다.

공식 계약: [D3D12_QUERY_DATA_PIPELINE_STATISTICS](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_query_data_pipeline_statistics), Apple [MTLCommonCounterSet.statistic](https://developer.apple.com/documentation/metal/mtlcommoncounterset/statistic).

#### P1-D — Stream Output는 별도 묶음으로 진행

Stream Output는 `SOSetTargets` 하나만 구현해서 끝낼 수 없다. PSO의 SO declaration, shader lowering/instrumentation, target binding/offset, filled-size counter, overflow와 `SO_STATISTICS_STREAM0..3`가 하나의 계약이다. 따라서 P1-A/B/C 뒤에 compiler/runtime/query를 함께 변경하는 독립 작업으로 진행한다. `SO_BUFFER` capability는 이 end-to-end 경로가 검증될 때까지 광고하지 않는다.

## 1. Resource·heap·view·메모리 계약

주요 소스:

- `src/d3d12/d3d12_resource_contract.hpp`
- `src/d3d12/d3d12_resource_helper.cpp`
- `src/d3d12/d3d12_device.hpp`, `d3d12_device.cpp`
- `src/d3d12/d3d12_buffer.cpp`, `d3d12_texture.cpp`, `d3d12_heap.cpp`
- `src/dxmt/dxmt_buffer.hpp`, `dxmt_buffer.cpp`

구현 내용:

- 크기·offset·alignment의 checked add/multiply/align/range 처리 및 DXGI format-family/copy 호환성 도우미.
- Resource dimension·extent·sample·layout·alignment·heap state·node mask 검사. UPLOAD의 `GENERIC_READ`, READBACK의 `COPY_DEST` 계약 반영.
- Optimized clear value의 flag·format 검사, 출력 객체를 요청하지 않는 validation-only 생성의 `S_FALSE` 경로.
- Heap/placed resource의 크기·정렬·범위 검사. Native allocation 실패와 allocation 예외의 오류 전파.
- CUSTOM heap의 CPU page property에 따른 Map 접근 정책.
- SRV/UAV의 resource flag·format family·dimension·sample·subresource·plane·범위 검사.
- Void view 생성 시 목적 descriptor를 먼저 비워 실패 후 이전 descriptor가 남지 않도록 처리. Typed native view를 생성 경로에서 materialize하고 실패를 내부 오류로 전달.
- `GetCopyableFootprints`의 512-byte placement/256-byte row pitch, plane/subresource 및 표현 가능한 필드 폭 검사. 빈 범위·overflow 처리.
- Allocation info의 빈 입력·null·overflow·native size/alignment 처리.

### 64-bit buffer view

Raw/structured buffer slice의 byte range를 64-bit로 유지한다. Typed view도 큰 `FirstElement`를 32-bit로 잘라 저장하지 않는다.

- `BufferViewDescriptor`가 64-bit backing byte offset/length 및 writable 여부를 보유한다.
- Native ranged texture-buffer view는 정렬된 offset에서 생성한다.
- Shader metadata에는 정렬 때문에 생기는 작은 element bias만 남긴다.
- View cache key에 range/writable을 포함한다.
- Integer clear용 재해석 view도 원래 range와 alignment를 보존한다. Whole-buffer alias나 offset 이중 적용을 피한다.

공유 코드 호출부도 함께 변경했다. `src/d3d11/d3d11_buffer.cpp`의 typed UAV와 `src/dxmt/dxmt_command_clear.hpp`의 alternate clear view는 writable을 명시한다. D3D10은 해당 D3D11 구현 경로를 사용한다. 이 공유 프런트엔드 변경 역시 재빌드·실행하지 않았다.

## 2. Buffer render-target view

기존 buffer RTV의 `E_NOTIMPL` 경로 대신 descriptor·render attachment·clear·copy coherence를 연결했다.

추가 관련 소스:

- `src/d3d12/d3d12_descriptor_heap.hpp`
- `src/d3d12/d3d12_command_encoder.hpp`
- `src/d3d12/d3d12_command_list.cpp`, `d3d12_command_queue.cpp`
- `src/dxmt/dxmt_command_clear.hpp`

동작 구조:

1. `ALLOW_RENDER_TARGET`, BUFFER dimension, typed/non-typeless color format 및 full 64-bit byte range를 검사한다.
2. Descriptor에 backing buffer, ranged view, 전체 element/byte range와 format을 보존한다.
3. Metal `TextureBuffer`를 render attachment로 가장하지 않는다. Render pass별 private 2D single-row texture를 만들고 backing buffer 내용을 copy-in한다.
4. 해당 attachment로 렌더링한 뒤 같은 범위를 backing buffer로 copy-out한다. 여러 buffer/texture RTV의 혼합 binding도 attachment별로 처리한다.
5. Typed pass가 native buffer/texture 소유권을 보유해 후속 copy와 GPU retirement까지 연결한다.

Raster window는 `min(NumElements, D3D12_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH)`다. **16384는 일반 texture 크기에서 추정한 제한이 아니라 D3D12의 render-to-buffer window 상수**다. 전체 descriptor range는 유지하며 raster window 밖은 draw로 덮어쓰지 않는다.

`ClearRenderTargetView`는 ranged typed compute clear를 사용하며 predicate를 반영한다. Rect가 없으면 전체 `UINT NumElements`를 처리하고, `LONG` 변환으로 범위를 잘라내지 않는다. Explicit rect와 dispatch group 계산은 별도로 범위/overflow를 처리한다.

참조: [D3D12 constants](https://learn.microsoft.com/en-us/windows/win32/direct3d12/constants), [D3D11.3 functional specification의 buffer view 제한](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

P0에서 `owned-d3d12-buffer-rtv.cpp`를 추가했다. 서로 다른 `FirstElement`, descriptor copy, 2개 MRT, predicated 부분 clear, 16,384-element 경계와 앞뒤 guard를 실제 GPU readback으로 확인했다. 실행 근거는 아래 P0 증거에 별도로 기록한다.

## 3. Residency

주요 소스:

- `src/d3d12/d3d12_pageable.hpp`
- `src/d3d12/d3d12_device.hpp`, `d3d12_device.cpp`
- Resource/heap 생성·소멸 hook, `d3d12_descriptor_heap.cpp`, `d3d12_query_heap.cpp`

구현 내용:

- Committed resource, explicit heap, descriptor heap, query heap의 device-owner 기반 pageable 등록.
- Residency reference count와 `CREATE_NOT_RESIDENT` 초기 상태. Native residency set은 0↔1 전이에서 변경한다.
- Batch 전체의 null·타 device·unsupported object·refcount underflow/overflow 검사, 중복 항목과 atomic mutation 처리.
- Placed resource는 heap residency를 빌려 사용한다. 독립 residency 대상으로 중복 계상하지 않으며 VA 등록 수명과 residency 등록 수명을 분리한다.
- Priority는 허용 범위 내 값을 저장한다. Metal에 대응 priority API가 없으므로 native eviction 우선순위 보장은 아니다.
- `EnqueueMakeResident`는 device lock 아래 add/commit을 완료한 뒤 fence를 signal한다. 호출 순서와 fence signal 순서를 직렬화하며, return 뒤 별도 residency 작업을 남기지 않는 보수적 구현이다.
- `DENY_OVERBUDGET`는 성공으로 무시하지 않는다. Native committed residency-set bytes와 새로 resident가 될 owner bytes를 `recommendedMaxWorkingSetSize`와 비교한다. 초과하면 변경·fence signal 전에 `E_OUTOFMEMORY`를 반환한다.

Metal bridge에 `MTLAllocation.allocatedSize`와 `MTLResidencySet.allocatedSize`를 추가했다. CPU-only descriptor heap은 native allocation 목록이 비어 있어도 논리 residency 상태를 가진다.

**Timestamp storage 경계:** `MTLCounterSampleBuffer`는 `MTLAllocation`이 아니므로 residency set에 직접 넣지 않는다. Timestamp query heap은 dummy allocation과 논리 refcount로 표현하고 opaque counter storage는 driver가 관리한다. Occlusion heap은 실제 visibility-result allocation을 등록한다.

## 4. Recording·ExecuteIndirect·GPU predication

주요 소스:

- `src/d3d12/d3d12_command_signature.cpp`
- `src/d3d12/d3d12_command_list.cpp`
- `src/d3d12/d3d12_command_allocator.cpp`, `d3d12_command_encoder.hpp`
- `src/d3d12/d3d12_command_queue.cpp`
- `src/dxmt/dxmt_command_context.hpp`, `dxmt_command_clear.hpp`

### Recording와 indirect

- Command signature의 argument graph·stride·packed size·terminal action·VB slot·IB 조합을 검사한다.
- Root signature ownership/local 여부, parameter type 및 root constant span을 검사한다.
- `ExecuteIndirect`의 device/list type·argument/count buffer·alignment·range·root 계약을 검사한다.
- 실행 후 signature가 변경한 root/VB/IB state를 D3D12 계약에 맞게 reset한다.
- Queue descriptor와 타 device의 Execute/Signal/Wait 입력을 검사한다.
- Invalid recording은 첫 오류를 보존한다. **실패한 `Close`로 poison된 list를 `Reset`으로 복구시키는 변경은 하지 않았다.**
- D3D의 비어 있는 viewport/scissor 상태를 Metal의 유효한 offscreen raster 상태로 내린다. 두 배열 중 하나가 바뀌면 함께 재설정해 이전 raster 상태가 남지 않도록 하며, 전체 draw를 생략하지 않아 VS UAV side effect를 보존한다. P0에서 native viewport count 0 assertion을 재현·수정했고, viewport 없는 VS atomic 실행과 일반 buffer RTV draw를 각각 확인했다.

### Predicate 의미

`SetPredication`은 GPU stream에서 64-bit 값을 allocator 소유 snapshot으로 복사한다. 이후 원본 predicate buffer가 바뀌어도 이미 설정한 predicate의 의미를 바꾸지 않는다.

**`D3D12_PREDICATION_OP`는 실행 조건이 아니라 생략 조건이다.** `EQUAL_ZERO`는 0일 때 생략하고 nonzero일 때 실행한다. `NOT_EQUAL_ZERO`는 반대다. P0에서 direct/indirect arguments, copy, resolve, MSAA worker의 반전된 조건을 수정했다. [SetPredication 계약](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication)과 [GPU snapshot 의미](https://learn.microsoft.com/en-us/windows/win32/direct3d12/predication)를 기준으로 삼았다.

원본을 `COPY_DEST`로 전환해 saved nonzero predicate 아래에서 0으로 덮어써도 기존 snapshot은 실행을 허용한다. 다시 `SetPredication`한 뒤에는 새 0을 읽어 생략해야 한다. Fixture는 덮어쓰기와 재설정 이후 destination 보존까지 확인한다.

- Direct Draw/DrawIndexed/Dispatch는 GPU가 생성한 indirect arguments로 실행 또는 zero-work를 선택한다.
- ExecuteIndirect resolver는 동일 snapshot으로 실행 count를 gate한다.
- Bundle draw는 부모 list의 predicate 경로를 사용한다. Bundle 안의 `SetPredication`은 허용하지 않는다.
- Predicate disable은 이후 작업을 unconditional 경로로 복원한다.
- Query Begin/End/ResolveQueryData를 rendering predicate로 생략하지 않는다.

### Clear

RTV/DSV는 conditional indirect draw, UAV Uint/Float는 conditional indirect dispatch로 연결했다. Predicate가 false인 full clear를 unconditional `loadAction=Clear`로 바꾸지 않는다.

Rect/array instance count, DSV의 선택한 depth/stencil component, untouched 영역의 load/store를 처리한다. Buffer RTV clear도 같은 snapshot을 사용한다.

P0에서 buffer RTV clear의 compute-pass 시작 순서를 수정해 null encoder 접근을 제거했다. Indirect dispatch는 threadgroup 단위로 반올림되므로 typed/raw buffer와 2D/array/3D clear kernel 모두 metadata의 실제 rect 크기로 write를 제한한다. GPU allocation 내부라는 이유로 view/rect 바깥의 쓰기를 허용하지 않는다.

## 5. Copy·resolve·legacy barrier

주요 소스는 command list/encoder/queue와 `src/dxmt/dxmt_command.metal`이다.

### Copy

- Buffer/footprint range와 staging arithmetic에 checked helpers를 사용한다.
- DXGI compatible-family copy에서 native pixel format이 다르면 공통 format의 per-subresource view로 맞춘다. 서로 다른 native format의 blit이 우연히 동작한다고 가정하지 않는다.
- Predicated buffer copy는 GPU conditional byte-copy를 사용하며 큰 범위는 나누어 처리한다.
- Non-MSAA texture↔texture, texture↔footprint, footprint↔footprint는 raw staging과 GPU conditional byte-copy로 false predicate의 destination을 보존한다.
- Predicated `CopyResource`는 buffer/subresource 경로로 연결한다.

### 정확한 predicated MSAA copy

Public Metal에는 conditional blit이 없으므로 MSAA raw copy를 shader 색 변환으로 대신하지 않는다.

1. Typed `PredicatedTextureCopy` pass가 source/destination/snapshot을 보유한다.
2. **Submission worker**가 앞선 command-buffer segment를 commit하고 실제 GPU retirement를 기다린다.
3. 완료된 snapshot을 읽고 predicate가 true일 때만 native texture blit을 기록한다.
4. 다음 ordered segment에서 나머지 명령을 계속한다.

API caller는 비동기 상태를 유지한다. 대신 이 드문 경로에서는 worker가 GPU completion을 기다리므로 overlap과 성능 비용이 있다. 각 segment의 allocator/native reference 수명도 유지한다.

### Resolve

- `ResolveSubresource`와 Region validation을 연결한다.
- 부분 rect·destination offset·untouched 영역 보존을 구현한다.
- Color float/normalized AVERAGE/MIN/MAX, signed/unsigned integer MIN/MAX 경로를 추가했다.
- Depth/stencil MIN/MAX는 선택한 plane만 기록한다. 불법 AVERAGE는 거부한다.
- Combined depth/stencil의 sampling view와 destination attachment/PSO format을 분리한다. Stencil sampling의 X32 view를 render attachment format으로 그대로 사용하지 않는다.
- Resolve shader도 predicate snapshot을 반영한다.
- Stencil resolve의 fullscreen triangle은 Metal 기본 winding에서 back face가 될 수 있으므로 front/back 모두 `Always`/`Replace` 상태를 설정한다. 한쪽 face만 설정해 stencil destination이 그대로 남던 문제를 P0에서 수정했다.
- `DECOMPRESS`는 Metal의 uncompressed representation에서 합법적인 in-place 조건을 검사한 뒤 no-op으로 처리한다. 임의의 copy/resolve를 no-op으로 대체한 것이 아니다.

### Barrier

Legacy transition/UAV/aliasing barrier의 type·flag·state·subresource를 검사하고 기존 보수적 encoder 경계를 유지한다. 이 경계와 Metal synchronization이 실제 ordering을 담당한다. 완전한 resource-state debug tracker나 enhanced barrier 구현을 뜻하지 않는다.

## 6. Root signature·binding·PSO

주요 소스:

- `src/d3d12/d3d12_root_signature.cpp`, `d3d12_device.hpp`
- `src/d3d12/d3d12_pipeline_graphics.cpp`, `d3d12_pipeline_compute.cpp`
- Command list의 root/PSO setter

RS 1.0/1.1 serializer와 deserialized root signature 생성이 같은 validator를 사용한다. Pointer/count graph, enum/flags, descriptor range/offset overlap, visibility별 register overlap, static sampler, global **64 DWORD** budget을 검사한다.

Local root는 global budget과 혼동하지 않는다. Shader record 4096 bytes에서 identifier 32 bytes를 뺀 **4064-byte argument payload**와 table/descriptor alignment를 적용하고 기존 DXR local copy 경로를 보존한다.

Root metadata의 type/constant count/local 여부를 setter 및 indirect validation에 연결했다. Index 경계, parameter type, constant span, null data, device ownership과 recording state를 검사한다. Graphics/compute PSO는 bytecode pointer/size, root ownership, sample/fixed-function enum/format 계약을 검사한다.

P0 통합 수정에는 metadata pointer의 backing storage를 root signature 소유 배열에 유지하는 변경도 포함한다. 임시 데이터의 수명에 의존하지 않는다.

## 7. 기본 shader compiler

주요 소스:

- `src/airconv/dxil_resources.cpp`
- `src/airconv/nt/air_builder.hpp`, `air_builder.cpp`
- `src/airconv/nt/dxbc_converter_base.hpp`, `dxbc_converter_base.cpp`
- `src/airconv/dxbc_converter.hpp`, `dxbc_converter.cpp`, `dxbc_signature.cpp`

추가 DXIL lowering:

| Opcode | 처리 |
|---|---|
| 74 | `TextureGatherCmp` |
| 75 | `Texture2DMSGetSamplePosition` |
| 76 | `RenderTargetGetSamplePosition` |
| 77 | `RenderTargetGetSampleCount` |
| 81 | `CalculateLOD` |

기존 AIR/DXBC helper 및 결과 shape를 재사용하고 sample-position helper를 공유한다. 기본 VS/PS/CS DXBC에서 지원하지 않는 application input은 initialize diagnostic으로 전달하도록 정리했다. 정상적인 unsupported 입력을 abort로 처리하는 경로를 줄이고 null-safe 오류 전파를 연결했다.

P0에서 DXIL lowering이 생성한 `dxmt.sample_pos.i32`에 기존 AIR helper 정의를 실제로 link하도록 수정했다. DXBC PS의 SGV system declaration에는 interpolation mode 0이 합법적일 수 있으므로 `SV_SampleIndex` 등을 flat으로 처리한다. 일반 입력의 잘못된 interpolation을 일괄 허용한 변경은 아니다.

이는 모든 shader model/opcode 지원 선언이 아니다. Tiled resource 관련 `CheckAccessFullyMapped`, FP64/wave, GS/HS/DS/mesh 확장 등은 이번 범위에 포함하지 않는다.

### P0 frozen shader 입력과 생성 도구

`graphics-translator/tests/dxil/`에 다음 입력을 추가했다. Texture fixture는 opcode 74/75/76/77/81을 실제로 포함하며, indirect fixture의 CS/VS는 opcode 78 `AtomicBinOp/Add`를 포함한다. Wine HLSL frontend가 `RWByteAddressBuffer.InterlockedAdd`를 컴파일하지 못해 indirect shader를 frozen DXIL로 바꿨다. Atomic을 일반 load/store로 바꾸거나 indirect 결과 기대값을 약화하지 않았다.

| 입력 | 크기 | SHA-256 | 생성 조건 |
|---|---:|---|---|
| `basic-texture-vs.dxil` | 3,148 B | `3d0c8c3126d1b4608fba5f490645afd0c8529e84700d590ea047507d4d1d32a7` | `basic-texture-ops.hlsl`, `-T vs_6_0 -E vsMain -HV 2021 -O0` |
| `basic-texture-ps.dxil` | 5,868 B | `f4fabb44ef94e4961e0e2aa7e617a9fbad0700c739492f4e6339809d6d508539` | 같은 HLSL, `-T ps_6_0 -E psMain -HV 2021 -O0` |
| `predication-indirect-cs.dxil` | 2,984 B | `08dce50dd933af311cfea88dc0489357f3b20df5547d3b8d36824318e4444a50` | `predication-indirect-cs.hlsl`, `-T cs_6_0 -E main -HV 2021 -O0` |
| `predication-indirect-vs.dxil` | 3,404 B | `68d328aa2906e26f7fbdc670ea07522cf6af03df9699285f703a87fe881b01a9` | `predication-indirect-vs.hlsl`, `-T vs_6_0 -E main -HV 2021 -O0` |

생성에는 로컬 `build/local/dxc-probes/dxc.exe`와 같은 디렉터리의 `dxcompiler.dll`·`dxil.dll`을 사용했다. 도구 보고 버전은 **1.9.0.5402 (`0d3ee6b55-dirty`)**이며, 위 최신 정식 DXC release와 같은 배포물이라고 검증한 것은 아니다. 도구 SHA-256은 각각 `980a3a4c6e5c88f5737dde321e548860021d58fa2349790d4e572805cb298293`, `9a5100511e127c6a2fc78edf984f95074a76d35b90c90c4d342430a5ae160e9b`, `feb57253eff0a622561e29b44cedbe86b89fc9a5bc8dc00fa2f98fafd712c2d8`다.

재생성 예시(동일 로컬 도구와 격리된 prefix 사용):

```sh
WINEPREFIX="$PWD/build/local/dxc-wine-prefix" WINEDEBUG=-all \
  toolchains/wine-runtime/bin/wine build/local/dxc-probes/dxc.exe \
  -T cs_6_0 -E main -HV 2021 -O0 \
  graphics-translator/tests/dxil/predication-indirect-cs.hlsl \
  -Fo graphics-translator/tests/dxil/predication-indirect-cs.dxil
```

Runner는 executable과 shader 입력을 evidence 디렉터리에 복사하고 hash·argv를 기록한다. `predication-indirect` argv 순서는 CS, VS이며 `dxil-texture`는 VS, PS다. 두 suite 모두 실행한 staged native module에 들어 있는 owned DXIL compiler의 실제 load를 요구한다.

## 8. Query·clock calibration·WineMetal ABI

Timestamp/occlusion 경로를 유지하고 queue `GetClockCalibration`을 구현했다.

- Null output과 지원하지 않는 queue type을 구분한다.
- Metal의 paired CPU/GPU timestamp 호출 앞뒤를 Wine `QueryPerformanceCounter`로 감싼다.
- GPU timestamp는 기존 1 GHz frequency 계약으로 반환한다.
- CPU timestamp는 주변 Wine QPC의 overflow-safe midpoint를 사용한다. Metal host clock 값을 Wine QPC epoch인 것처럼 직접 반환하지 않는다. 이 bracket 기반 대응의 실제 오차는 미측정이다.

Bridge 관련 파일은 `src/winemetal/winemetal.h`, `winemetal_thunks.h`, `winemetal_thunks.c`, `Metal.hpp`, `unix/winemetal_unix.c`다. Append-only Unix call 번호는 다음과 같다.

| 번호 | 기능 |
|---|---|
| 201 | Device paired timestamp sampling |
| 202 | Allocation allocated size |
| 203 | Residency-set allocated size |

해당 wrapper/parameter/native handler/native·wow64 dispatch 연결을 함께 변경했다. P0의 x86_64 통합 빌드와 query/residency GPU 실행에서 이 경계를 확인했다. Wow64·ARM64EC 실행까지 검증한 것은 아니다. 추가한 render encoder depth/stencil state wrapper는 기존 render-command opcode/decoder를 사용하며 새 Unix call 204를 추가하지 않았다.

**`PIPELINE_STATISTICS`는 여전히 미구현이다.** Native counter 몇 개로 정확한 전체 D3D12 통계를 가장하지 않는다. 위 P1-C 설계처럼 query slot, predication/indirect-aware GPU 집계, 필요한 compiler instrumentation과 native counter의 정확한 대응을 함께 구현해야 한다.

## 9. Queue/Fence와 기존 증거의 범위

기존 논리 fence value와 native monotonic token 분리, CPU/GPU rewind, API-time wait latch, ANY/ALL event registration, 실제 GPU completion 기반 consumer wait를 유지한다. 이번 추가 snapshot/staging/segment도 allocator와 native resource의 retirement 수명에 연결했다.

이전 Fence 코어 단계에는 실제 실행 증거가 있었다.

- `build/local/fence-core-after/result.json`
- `build/local/fence-core-multiqueue-proof/result.json`
- `build/local/fence-core-temporal-proof/result.json`
- 상세: [FENCE-EVENTS.ko.md](FENCE-EVENTS.ko.md), [QUEUE-ORDER.ko.md](QUEUE-ORDER.ko.md)

**이 결과는 이번 전체 코어 통합본의 검증 결과가 아니다.** 일부 fixture/객체의 과거 단독 컴파일과 query baseline 실패 기록도 현재 소스의 성공 근거로 승격하지 않는다. `build/local/fence-core-runtime` 등 기존 runtime을 최신 구현본으로 취급하면 안 된다.

## 10. P0 실행 결과와 재현 근거

설정된 `build/local/build`의 전체 Ninja target 빌드와 install을 통과했다. 마지막 viewport/scissor 수정 빌드는 13-step incremental build 성공으로 확인했으며 별도 log 파일은 저장하지 않았다. 앞선 통합 빌드 log는 `build/local/p0/full-build.log`, `runtime-03-build.log`, `runtime-04-build.log`, `runtime-05-build.log`다. 기존 managed-storage deprecation 및 unused lambda-capture warning은 남아 있다.

아래 **11개 suite는 모두 `--validation`으로 PASS**했다. 경로 기준은 `build/local/p0/`이며 각 evidence 디렉터리에 `result.json`, `invocation.json`, frozen fixture/input, stdout/stderr와 backend trace가 있다. Source revision별 새 runtime 복사본을 만들고 각 실행마다 전용 prefix와 새 evidence를 사용했다. **모든 suite를 마지막 runtime 하나에서 재실행했다는 뜻은 아니다.** 수정 영향이 있는 경로만 다시 실행했으며 실제 사용한 revision을 구분한다.

| Suite | 확인한 계약 | Runtime | PASS evidence 디렉터리 |
|---|---|---|---|
| `core-contracts` | Root signature validation·recording 계약 | `runtime-01` | `proof-01/core-contracts` |
| `queries` | QPC bracket·1 GHz timestamp·occlusion·query 수명 | `runtime-01` | `proof-01/queries` |
| `residency` | Refcount·CREATE_NOT_RESIDENT·budget flag·fence 7·256-byte readback | `runtime-01` | `proof-01/residency` |
| `resources` | Footprint/view 계약과 같은 64 KiB heap offset 0의 placed buffer A→B→A 양방향 data inheritance | `runtime-01` | `proof-01/resources` |
| `fence-events` | CPU/GPU rewind·ANY/ALL·registration latch·teardown | `runtime-01` | `proof-01/fence-events` |
| `multiqueue` | 32회 × 1,024 words의 queue 간 ordering/readback | `runtime-01` | `proof-01/multiqueue` |
| `bundles` | 기존 bundle 실행·상태 계약 | `runtime-01` | `proof-01/bundles` |
| `dxil-texture` | Comparison gather·array·LOD 0/-4·4x sample positions/count | `runtime-03` | `proof-04/dxil-texture` |
| `copy-resolve` | RGBA 전 성분 AVG/MIN/MAX·부분 rect·guard·MSAA predicate snapshot/rebind·depth/stencil MIN/MAX·다른 plane 보존 | `runtime-04` | `proof-04/copy-resolve` |
| `buffer-rtv` | FirstElement·descriptor copy·2 MRT·predicated rect·16,384-element 경계와 guards | `runtime-06` | `proof-08/buffer-rtv` |
| `predication-indirect` | Predicate 두 operation·snapshot/rebind·raw/typed clear·2-layer rect/component 보존·indirect count/root/state·CS atomic 합 9·VS atomic 합 3 | `runtime-06` | `proof-08/predication-indirect` |

Fixture 소스는 `graphics-translator/tests/owned-d3d12-<suite>.cpp`다. Typed multilayer coverage는 RTV `R16G16B16A16_UINT`, DSV `D32_FLOAT_S8X24_UINT`, UAV `R32G32B32A32_UINT` 및 `R32G32B32A32_FLOAT`를 포함한다. 이것이 모든 format/layer/sample-count 조합을 검증했다는 뜻은 아니다.

추가 확인:

- `python3 scripts/test-owned-graphics-diagnostics.py`: 변경된 runner 기준 **42 tests PASS**.
- `queue-order-scheduler.test.cpp`: native CPU 실행 PASS — logical fence, registration, FIFO, lifetime, storage, ring drain, cross-queue.
- `d3d12-resource-contracts.test.cpp`: Windows cross-build 후 격리된 Wine 실행 PASS — range/placement/overflow/footprint, pitch 512.
- `dxil/dxbc-input-error-probe.cpp`: native x86_64 실행 PASS — malformed input은 diagnostic/null shader를 반환하고, 같은 process의 다음 valid input은 성공. 일반 runner suite는 아니다.

### 실제 실패에서 수정까지

| 재현 | 수정 | 통과 근거 |
|---|---|---|
| `predication-skip-before-fix`: 0/EQUAL_ZERO에서 쓰기가 실행됨 | 모든 predicate 경로의 skip 조건 교정 | `proof-08/predication-indirect`, `proof-04/copy-resolve` |
| `proof-02/buffer-rtv`: null compute encoder 접근 | Buffer RTV clear의 pass 시작 순서 수정 | `proof-08/buffer-rtv` |
| `proof-03/buffer-rtv`, `proof-04/predication-indirect`: rect 뒤의 guard 손상 | 반올림된 indirect threadgroup의 typed/raw/texture clear write를 실제 rect 크기로 제한 | `proof-08/buffer-rtv`, `proof-08/predication-indirect` |
| `resolve-stencil-diagnostic`: stencil 10 대신 초기 guard 165 | Stencil resolve front/back state 모두 설정 | `proof-04/copy-resolve` |
| `proof-02/dxil-texture`: undefined `dxmt.sample_pos.i32` | 기존 AIR sample-position helper link | `proof-04/dxil-texture` |
| `proof-06/predication-indirect`: Metal viewport count 0 assertion | Empty raster state의 유효한 native 표현, VS side effect 유지 | `proof-08/predication-indirect`와 일반 draw `proof-08/buffer-rtv` |

Fixture 자체의 잘못된 전제도 구분해 수정했다. 존재하지 않는 environment gate를 제거하고 실제 backend identity를 검사한다. Static sampler의 `MaxAnisotropy=0`을 유효한 1로 바꿨다. Raw UINT clear는 `Values[0]`만 사용하는 oracle로, Float clear는 명확한 typed FLOAT view로 구성했다. CPU UAV descriptor는 non-shader-visible heap에 두고 clear 사이에 UAV barrier를 넣었다.

Gather 기대값은 관찰값에 맞춘 것이 아니라 [GATHER4의 LL/LR/UR/UL 순서](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#inst_GATHER4), [reference와 texel의 비교 순서](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#inst_SAMPLE_C), 업로드한 texel·좌표·offset·CLAMP에서 다시 계산했다. 그 결과 scalar `(0,1,0,1)`, array `(0,1,1,0)`이며 lowering을 바꾸지 않고 잘못된 oracle만 수정했다.

`proof-07`은 harness 알림에 의해 실행이 중단되어 완료 artifact가 없으며 성공/실패 판정에 사용하지 않는다. 같은 확인은 durable job으로 새 `proof-08`에서 완료했다. 실패·진단 evidence는 원인 추적을 위해 보존했다.

### 재현 명령과 남은 경계

예시(동일 도구와 현재 source로 새 artifact 생성):

```sh
export PATH="/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:$PWD/toolchains/tools/bin:$PATH"
mkdir -p build/local/p0-reproduce
x86_64-w64-mingw32-clang++ -std=c++17 -O2 -Wall -Wextra -Werror -static \
  graphics-translator/tests/owned-d3d12-predication-indirect.cpp \
  -ld3d12 -ldxgi -luuid -o build/local/p0-reproduce/predication-indirect.exe
ninja -C build/local/build -j8
meson install -C build/local/build --no-rebuild
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --wine-source toolchains/wine-runtime \
  --wine-dest build/local/p0-reproduce/runtime --with-nvapi --with-nvngx
python3 scripts/test-owned-graphics.py --runtime build/local/p0-reproduce/runtime \
  --suite predication-indirect --fixture build/local/p0-reproduce/predication-indirect.exe \
  --out build/local/p0-reproduce/evidence --validation
```

`runtime`과 `evidence`는 실행 전 존재하지 않아야 한다. 재실행 때 새 경로를 선택한다. 다른 fixture도 위 evidence 표의 source로 빌드하며, buffer RTV/copy-resolve는 runtime HLSL compilation을 위해 `-ld3dcompiler`가 필요하다. 최신 header로의 교체, 전체 format/sample-count matrix, texture/reserved-resource aliasing, 다른 GPU/OS/ABI, Windows reference와의 differential 검증, 일반 D3D10/11 workload 및 presentation은 P0의 검증 범위를 넘어 남아 있다.

## 최종 경계

이번 문서는 **무엇을 소스에 구현했고 무엇을 아직 증명하지 않았는지**를 구분한 상태 기록이다.

- 구현 경로가 있다는 이유로 build/runtime success를 선언하지 않는다.
- P1의 다음 구체 작업은 `AtomicCopyBufferUINT/UINT64` → `ResizeBuffers1`/present-queue rotation → 정확한 pipeline statistics 순으로 잡는다. Stream Output/SO statistics는 그 다음 compiler/runtime/query 묶음이다. Pipeline library, tiled/shared/cross-adapter·enhanced barrier 등 P2 이후 기능도 계속 구현 대상으로 유지한다.
- 실제 게임 정상 렌더링, 성능, 전체 D3D12/DXR conformance를 주장하지 않는다.
- 이후 검증을 진행할 때는 새 build/install과 **새 runtime 복사본·전용 prefix·새 evidence 경로**를 사용해야 한다. 기존 게임 또는 live prefix에는 배포하지 않는다.
