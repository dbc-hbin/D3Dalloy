> 현재 실행 순서와 P0 완료 기준은 [구현 우선순위](IMPLEMENTATION-PRIORITIES.ko.md)를 따른다. 아래 지원 상태·과거 증거와 작업 순서를 구분한다.

# D3D12 렌더링 코어 소스 구현 상태

## 판정 기준

**현재 판정: 검증한 Apple M5 Pro/macOS 27 범위에서 P0·P1·P2·P3 완료(2026-10-01).** 현재 보고 값은 Shader Model **6.5**, feature level **12_1**, RaytracingTier **1.1**, SamplerFeedbackTier **1.0**, ViewInstancingTier **1**, MeshShaderTier **1**, VariableShadingRateTier **2**, ConservativeRasterizationTier **1**, WorkGraphsTier **1.0**이다. P2의 7개 slice는 구현·GPU 검증을 마쳤으며, 그 당시 증거와 제한은 [P2 완료 결과](#p2-완료-결과-2026-10-01), 후속 수정과 최종 통합 증거·경계는 [P3 완료 결과](#p3-완료-결과-2026-10-01)에서 구분한다.

- 제약: SM 6.6은 모든 stage의 64-bit atomic이 필요해 막혀 있다([P2-SM](#p2-sm--shader-model-6x-compiler)). TiledResourcesTier 3/4와 RelaxedFormatCasting은 Metal 제약으로 보고하지 않는다([P2-TILED](#p2-tiled--reservedtiled-resource), [P2-BARRIERS](#p2-barriers--enhanced-barriersdevice614commandlist59render-passgpu-upload-heap)). TS+GS ExecuteIndirect, GS SO stream 0–3, early-Z GS/DS/MS MSAA statistics와 DS-only SO의 과거 제한은 후속 GPU 증거로 닫았다. MeshShaderTier **1**은 필수 128-scalar 출력·cull 구현과 공개 cap strict gate를 확인한 뒤 보고한다. Native 124-scalar 한계를 필수 요건의 예외로 인정하지 않는다. D3D11 shared-resource interop은 구현하지 않았다([P2-SHARING](#p2-sharing--shared-heapresourcefence)).
- 미검증 경계: 전체 D3D12/DXR conformance, 물리 compositor·HDR scanout, 다중 출력, 다른 GPU/OS, Windows reference differential, 실제 게임과 성능. Reachable baseline+affected-path 통합 proof를 한 snapshot의 전체 suite 통과로 확대하지 않는다. API-only atomic live reader와 source/ABI가 없어 실행하지 못한 fixture의 범위는 P3 절에 별도로 명시한다.
- **FL12_2는 미지원**이다. FL12_1과 FL12_2는 별도 계약이며, SM 6.6 미보고 자체를 둘의 필수 선행 조건으로 혼동하지 않는다. 상세 필수 cap 경계는 P3 절에 기록한다.

P1 기준으로는 기본 API·shader·presentation 계약의 구현 공백이 닫혔다. HDR10PLUS는 native 입력 경로가 없어 제약으로 미충족으로 분류하고 명시적으로 거부한다. 남은 항목은 이 장치에서 실행할 수 없는 하드웨어·환경 조합(다중 모니터, 물리 HDR scanout, 다른 GPU/OS, Windows reference differential)의 **미검증 경계**다. [P1 완료 판정과 남은 계약](#p1-완료-판정과-남은-계약)에서 구분한다.

최초 버전은 사용자 요청에 따라 구현 뒤 검증보다 먼저 작성했다. 이후 P0 통합 빌드와 11개 suite, P1 개별 기능의 실행 검증을 마쳤다. 아래의 **소스 구현**과 **실행으로 확인한 계약**을 구분한다. 실제 GPU 증거는 Apple M5 Pro/macOS 27의 x86_64 Wine 경로에 한정하며, P1 증거는 각 결과 절, P0 증거는 10절에 기록했다.

현재 개발 기준은 루트 `src/`다. `graphics-translator/patches/`와 과거 조사 문서는 frozen import 및 역사적 기록이며, 이번 변경을 재생성하거나 배포하는 입력으로 사용하지 않았다. 기존 설치본·staged runtime은 현재 소스와 구분해야 한다.

### 직전 구현 단계의 범위

- Resource 생성·heap·view·footprint·allocation·residency.
- Command recording·indirect·predication·copy·resolve·legacy barrier·기본 query.
- 기본 VS/PS/CS shader lowering·root binding·graphics/compute PSO.
- 기존 Queue/Fence 의미와 GPU retirement 수명 유지.

**직전 단계에서 사용자가 분리했던 항목:** 정확한 `PIPELINE_STATISTICS` query. Timestamp·occlusion·clock calibration은 렌더링 코어에 남겼다. 이후 P1-C에서 MSAA 실행을 포함해 구현·검증하고 공개했으며, 더 이상 보류 항목이 아니다.

DXR 확장, GS/HS/DS·mesh 확장, NGX/게임별 호환성, pipeline library 및 캐시 성능 확장은 직전 단계의 완료 판정에 포함하지 않았다. 이는 프로젝트의 최종 지원 범위를 제한하는 목록이 아니다.

## 개발 목표와 우선순위

### 목표 변경: 특정 게임 실행 → 최신 규격의 완전한 번역기

**최종 목표는 ZZZ 실행이 아니라, 최신 공개 Direct3D/DXGI 및 shader 규격의 의미를 정확하고 완전하게 구현하는 범용 Direct3D → Metal 번역기다.** ZZZ는 여러 실제 workload 중 하나이며, 실행·화면 출력·FPS 달성이 프로젝트 완료 기준이나 기능 우선순위를 대신하지 않는다.

- D3D12와 자체 DXIL/DXBC → AIR/metallib compiler를 중심으로 API, shader, memory, synchronization, presentation을 end-to-end로 완성한다. 공유 DXGI/backend는 D3D12에 필요한 범위에서 수정한다. D3D11은 업스트림 구현을 사용하며 아래 예외 외에는 변경하지 않는다.
- GPTK/D3DMetal, Apple Metal Shader Converter, SPIR-V fallback에 의존하지 않는 기존 아키텍처를 유지한다.
- “완벽”은 목표이지 현재 달성 주장이나 성공률 수치가 아니다. 누락 기능, 부정확한 emulation, 미검증 조합을 숨기지 않고 남은 작업으로 유지한다.
- Native Metal 대응 기능이 없으면 의미를 보존하는 emulation을 우선 설계한다. 정확한 구현이 불가능한 경우 제약·영향·미충족 계약을 명시한다. No-op, 근사 결과, capability 축소만으로 완전 지원을 달성했다고 판단하지 않는다.

### D3D11 변경 조사와 유지 방침

**2026-09-29 사용자 결정: 기존 변경은 삭제하지 않는다. 정말 필요한 경우가 아니면 D3D11은 건드리지 않는다.** D3D12 구현에 필수인 공유 인터페이스·ABI 변경 또는 그 변경으로 깨지는 기존 업스트림 동작 보존만 예외로 하고, 해당 의존성을 코드 근거로 설명한 뒤 최소 범위로 수정한다. 인접 코드에 같은 버그가 있거나 D3D11 테스트에서 별도 문제가 발견됐다는 이유만으로 작업을 확장하지 않는다. D3D10/11 기능 완성을 P1의 독립 완료 조건으로 삼지 않는다.

고정 DXMT 업스트림 `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`, frozen import patch 18개, 초기 import `9e5ffa3`, 후속 commit `6a7f923` 및 현재 작업 트리를 대조했다. `src/d3d10`은 업스트림과 동일하고, `src/d3d11`은 아래 swapchain과 buffer 두 파일만 다르다. 조사 중 시도했던 삭제는 원상복구했으며 코드·fixture·runner·과거 실행 증거를 유지한다.

| 기존 변경 | 추가 이유와 판단 | 현재 처리 |
|---|---|---|
| `src/d3d11/d3d11_swapchain.cpp` fullscreen·`ResizeTarget` 오류/상태 보존·생성 처리 | 당시 P1 문서의 공유 D3D10/11 계약까지 적용한 범위 확장. D3D12 frontend 구현의 필수 의존성이 아님 | 기존 수정 유지, 별도 D3D11 확장 중단 |
| 같은 파일의 공개 `GetGammaControl` 조회와 ramp cache | D3D11 output proxy Present의 내부형 캐스팅 충돌을 실제 재현해 수정. D3D11 전용 버그 | 기존 수정·증거 유지 |
| `owned-dxgi-fullscreen.cpp`와 runner의 D3D11 분기 | 두 frontend의 fullscreen·readback·Present 회귀 검증을 위해 추가 | 기존 fixture 유지, 다음 기능은 D3D12 전용 검증 |
| `src/d3d11/d3d11_buffer.cpp` UAV `.writable = true` (`6a7f923`) | 공유 buffer-view 확장 시 쓰기 속성을 명시. 정상 D3D11 UAV는 기존 allocation flag 분기로도 쓰기 권한을 얻으므로 필수 migration이라고 볼 근거는 없음 | 한 줄 변경 유지, 추가 D3D11 변경의 근거로 사용하지 않음 |
| `src/nvapi/nvapi.cpp`의 D3D11 opcode-query null 검사 (`0016`) | 인접 D3D12 함수 추가 시 null 안전성을 보강한 것으로 추정 `[INFERENCE]`; 독립적인 동기 기록 없음 | 기존 검사 유지 |
| `scripts/test-owned-ngx-frontend.py`의 D3D11 export 검사 | D3D12 NGX를 같은 DLL에 추가할 때 기존 업스트림 export 보존을 확인하려고 작성한 검사. 신규 D3D11 구현은 아님 | 기존 ABI 보존 검사 유지 |

D3D12가 사용하는 DXBC/SM50 compiler, buffer range/writable, Metal bridge, queue/presenter, DXGI/WSI 변경은 D3D11이라는 이름이나 공유 여부만으로 제거하지 않는다. NGX parameter의 `ID3D11Resource` Set/Get 슬롯은 D3D12도 사용하는 공식 17-slot ABI의 일부라 유지해야 한다. 업스트림 `nvngx.cpp`의 D3D11 구현 및 frozen patch/provenance도 보존한다.

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
| P0 당시 D3D12 공개 지원 선언 | **FL11_0 / SM5.1**, DXR tier·mesh tier 미지원(P2 완료 뒤 SM 6.5) | 일부 SM6 DXIL 입력을 소비하는 것과 SM6 전체 conformance는 다름 |

Native family 의미는 [Apple `supportsFamily`](https://developer.apple.com/documentation/metal/mtldevice/supportsfamily(_:))와 [raytracing capability](https://developer.apple.com/documentation/metal/mtldevice/supportsraytracing)를 기준으로 구분한다. DXIL parser의 version metadata 처리나 개별 lowering 존재만으로 상위 버전 검증 완료를 주장하지 않는다.

### 실행 순서

우선순위는 **정확성·공통 기반·다른 기능의 의존성**으로 정한다. 아래는 단계별 작업과 완료 기준이다. P0·P1·P2는 명시된 장치·환경 범위에서 완료했고, 현재 P3의 기능별 전체 계약과 최종 통합 회귀를 닫는 중이다. P4도 최종 범위에 포함한다.

| 우선순위 | 작업 | 해당 단계의 완료 기준 |
|---|---|---|
| **P0 — 기준 고정과 현재 통합본 안정화** | 규격/backend 기준표 작성, 직전 UNVERIFIED 통합본의 build·ABI·실행 문제 수정. 누락 DXIL 입력 준비. Buffer RTV, depth/stencil resolve, typed/multilayer clear, 실제 placed-resource aliasing, predicate/indirect, queue/fence/residency 경계를 확인한다. | 소스와 실행 artifact가 일치하고 해당 계약의 관찰 가능한 결과·오류·수명·ordering이 재현 가능한 증거로 남는다. 알려진 corruption/deadlock/device-loss 문제를 숨기지 않는다. |
| **P1 — 기본 API·shader·presentation 계약의 완전성** | D3D12의 resource/view/format/heap/map/copy/resolve, root/binding/PSO, recording/query/indirect/predication, queue/fence/residency의 남은 공백을 닫는다. 정확한 pipeline statistics를 다시 포함한다. D3D12가 사용하는 DXGI swapchain/present/resize/fullscreen/color-space/HDR를 기준표에 따라 다룬다. | 기본 경로의 정상·경계·오류·동시성 계약이 맞고, feature/format 지원 보고와 실제 동작이 일치한다. 부분 native counter를 전체 statistics로 가장하지 않는다. |
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

**현재 단계:** P0·P1·P2·P3 완료(공통 완료 기준 4의 장치·환경 경계 명시). Shader Model **6.5**, feature level **12_1**과 위 공개 tier를 유지한다. P3 구현·기능별 cap proof에 더해 최신 WG/DXR 변경을 합친 공통 runtime-04의 x64/WoW64 affected suite를 검증했다. Reachable coverage는 baseline+targeted evidence 합산이며, 단일 snapshot 전체 suite·전체 conformance 완료를 뜻하지 않는다.

### P1 전체 구현 중 확인한 계약 안전성 보강

`build/local/p1-complete/runtime-01`·`runtime-02`는 아래 변경을 통합한 새 격리 runtime이다.
모든 아래 D3D12 실행은 Metal API/Shader Validation을 사용했다. **이 결과는 P1 전체 완료 선언이 아니다.**

| 변경 계약 | 관찰한 결과와 증거 |
|---|---|
| PSO 입력 슬롯·활성 fixed-function 상태 검증 | 유효한 IA slot 16·31은 실제 draw/readback 통과, slot 32·UINT_MAX와 잘못된 활성 enum/write mask·미지원 logic op는 출력 포인터를 비우고 거절. `shader-binding/bundles-after-01` |
| 호환 포맷 `CopyResource` | RGBA8 linear→sRGB, 역방향, R32 UINT→FLOAT의 2 mip×2 array, case당 40 texel을 bit-exact 복사. 서로 다른 native pixel format의 직접 whole-texture blit 대신 subresource view copy 사용. `format/copy-resource-casts-after-03` |
| Heap·CPU texture·allocation | Cross-device placed heap 거절, placed resource의 heap COM 수명 유지, caller heap release 후 GPU alias/copy, row pitch 17·3D row 9/slice 19 및 BC tail의 정확한 CPU 전송, 빈 box 무처리 성공. Small MSAA alignment는 top mip 기준이며 배열·추가 mip 크기로 잘못 거절하지 않음. AllocationInfo1 offset으로 실제 placed resource 생성·사용. `resource-memory/after-02` |
| Recording·queue 종료 | Allocator reset 후 stale closed list가 포함된 batch 전체를 제출 전에 버리고 device removal 공개. 같은 command-list COM 객체를 다른 allocator로 reset/re-record하는 유효한 overlap은 유지. GPU fence 완료 직후 allocator reset 16회, unresolved wait의 queue-local 종료 취소와 독립 peer queue 보존 확인. `execution/shutdown-after-02` |
| Swapchain 출력 제한·geometry | Restrict-output 소유권·COM 수명 및 `GetRestrictToOutput`, source size·rotation·background의 실제 동작과 실패 시 상태 보존을 구현. `dxgi-presentation/present-queues-proof-02`. 별도 public D3D12 fixture에서 fence 뒤에 두 Present를 기록한 뒤 setter를 변경하고 실제 CAMetalLayer drawable을 캡처하여 각 frame의 서로 다른 crop/회전/배경색 snapshot을 확인 |
| Sampler·cache 지원 계약 | Shader-visible sampler heap 2048/2049 경계와 CPU-only 2049 허용, 장치별 살아 있는 고유 static sampler 2032개·중복 공유·해제 후 재사용 확인. Shader cache는 구현된 `SINGLE_PSO`만 보고하며 자동 OS cache를 가장하지 않음. `shader-binding/binding-only-after-02` |

표의 증거 경로는 모두 `build/local/p1-complete/` 아래다.

### 원래 P1 추가 구현과 GPU 검증

2026-09-29 추가한 아래 경로는 이전 `runtime-01`~`runtime-03`의 바이너리와 다르다.
**소스·cross-build·native probe를 공개 D3D12 통합 검증 완료로 승격하지 않는다.**

| 추가 경로 | 현재 증거 | 남은 확인 |
|---|---|---|
| Resource/heap/allocation | **runtime-04 공개 GPU 검증 통과:** 최신 resources v16의 dimension·mip·buffer 한계, 4KB/64KB tile 기준·MSAA RT alignment, null-output·초기 state·heap 수명·CPU 전송·GPU alias. `resource-memory/public-runtime04-v16` | 다른 GPU/OS의 allocation 및 Metal capability 차이는 별도 검증 |
| RGB32·packed 4:2:2 | **runtime-04 공개 GPU 검증 통과:** 논리 footprint↔확장 backing, 1D/2D/3D·array·mip copy와 CPU/GPU/predicated copy, RGB32 typed-buffer remap·OOB, RGB32 texture load·MSAA load·FLOAT resolve, packed filtering와 RGB32 blend 6 case. `format-design/*-runtime04-final.log` | 물리 backing은 확장 표현이며 논리 footprint와 동일하지 않음. Format-support·multisample-capabilities도 통과 |
| STANDARD_SWIZZLE | **runtime-04 공개 GPU 검증 통과:** canonical roundtrip, shader·clear, paired format, texture/texture·texture/buffer alias, placed heap Evict→MakeResident 뒤 GPU 읽기, custom WRITE_BACK Map·CPU transfer 뒤 GPU 읽기. `standard-swizzle/public-runtime04-v3`. 별도 native 2D/3D/BC tile oracle 유지 | 공개 fixture의 명시된 조합과 native layout 증거를 구분하며 모든 GPU family의 conformance는 아님 |
| View/compiler | **공개 GPU 검증 통과:** partial 3D UAV DXBC와 DXIL write→load fence·atomic·bounded clear·dimensions, depth 1D/cube/cube-array의 D16/D24/D32/D32S8, fractional LOD·comparison LOD·1D border, BC volume. `format/partial-dxil-after-fence-05` | ROV nested descriptor-table과 sample-frequency 최종 검증은 아래 runtime-13 결과 참조 |
| Compressed/packed copy | **runtime-05 공개 GPU 검증 통과:** BC1 8×8↔RGBA16UINT 2×2 bitcast의 정확한32 bytes, packed cube-array RGBA77/120/120/255, depth plane4 format. `format/{reinterpret-permanent-05,packed-cubearray-permanent-05,depth-plane-corrected-05}` | Reinterpret의 수정된 유효 fixture는 runtime-03에서 실패했다. 무효 footprint나 UAV flag 누락 fixture를 driver 결함 증거로 사용하지 않음 |
| Recording/queue | **runtime-04 공개 GPU 검증 통과:** immutable recording, stale/duplicate·post-Close, state·split·promotion·decay·COPY handoff, 미래 queue Wait 뒤 query, 같은 COM 객체의 유효 rerecord, 제거 뒤 residency 오류. `execution/runner-common-04` | Native residency priority는 hint이며 Metal의 엄격한 eviction priority scheduling을 보장하지 않음 |
| Presentation | **runtime-06 공개 GPU 검증 통과:** present-queues의16 drawable, latency·DO_NOT_WAIT·제거 wake·rotation·resize·restriction, 최대 latency1에서 stalled queue Wait 앞의 queued RESTART 0ms와 정확한 public credit1, fullscreen 최종 Release의 HWND/style/rect/mode 복원. Dirty/scroll exact pixels(runtime-04), crop/rotation/RTL과 실제 presented callback 뒤 statistics(runtime-05) 통과 | 이미 Metal에 commit된 drawable의 폐기는 P1 종료 결과에서 구현·검증. 물리 HDR scanout·다중 출력 전환은 별도 경계 |

호스트의 x86_64 시작 단계가 커널 대기 상태에 빠져 full Ninja가 `winebuild --builtin`에서 멈췄다.
Wine/LLVM을 사용하지 않는 최소 x86 프로그램도 시작하지 못했고, 승인받은 Rosetta 서비스 재시작으로도 복구되지 않았다.
이후 사용자가 실행 권한을 허용한 뒤 같은 최소 x86 프로그램과 `winebuild --version`이 정상 종료했다.
macOS 재시동 없이 full Ninja/install과 새 `build/local/p1-complete/runtime-04` staging을 완료했다.
이 runtime에서 dynamic sampler2047·static sampler2031의 shader 결과와 ForcedSampleCount4의 ROV 누적27/27은 Metal API/Shader Validation을 통과했다(`shader-binding/tier2-sampler-high-after-04`, `shader-binding/rov-forced-after-04`).
Attachmentless ordinary MSAA4 ROV의 encoder/PSO sample count 불일치는 effective raster count로 pass를 생성하고 count가 바뀌면 pass를 분리해 수정했다. **runtime-05**에서 ROV 27/27과 같은 command list의 sample1→sample4→sample1→Forced4 전환이 Metal API/Shader Validation을 통과했다(`shader-binding/{rov-msaa4-after-05,attachmentless-switch-after-05}`). 새 runtime 전체 검증은 진행 중이며 기존 설치본이나 게임 prefix는 변경하지 않았다.
`execution/{multiqueue-04,residency-04,copy-resolve-04,predication-indirect-04}`도 API/Shader Validation을 통과했다. Multiqueue 32×1024 GPU words, residency fence7/checksum `0x59047300`, color/depth/stencil resolve와 indirect/predication 결과를 확인했다. Descriptor heap 1,000,001 거부·sampler/static sampler 수명·cache flags는 `shader-binding/binding-only-static-after-04`에서 통과했다.
`shader-binding/pipeline-cache-after-05`는 malformed·foreign·root/shader 불일치 blob 거부와 변경 shader 결과 `0x31415813`, unaligned 입력 수명·반환 blob 격리 결과 `0x31415a5b`를 GPU에서 확인했다.
`shader-binding/pipeline-statistics-after-06`도 DXBC·DXIL의11-field independent GPU oracle 전체를 Metal API/Shader Validation으로 통과했다. 이 결과는 이후 short-line 입력 복원·center pattern 변경 이전의 기존 MSAA depth·stencil·derivative·interpolation 경로 보존 증거다.
DXGI의13-frame color-space·7-frame HDR metadata 공개 경로와 native presenter의 tone-map·변환 oracle도 통과했다. 공유 backend 보존 검증은 `runtime06-baseline.log`에서 D3D11 3D UINT/FLOAT48-voxel clear·DXBC compute·PS sample·Present 및 실제 drawable3072 pixels=`ff4080bf`로 확인했다. D3D11 전용 기능 범위를 추가한 것은 아니다.
완료된 시험 prefix는 디스크 공간 회수를 위해 제거했으며 frozen fixture·stdout/stderr·native module log·result.json·drawable capture는 보존했다.
**runtime-10 통합 GPU 통과:** `shader-binding/pipeline-statistics-after-10`의 DXBC 168+DXIL 178=346 GPU case·26 validation case와 `line-after10`, `center-final10-v1`, `center-dxil-final10-v1`이 Metal API/Shader Validation을 통과했다. Line의 W 비율 1:1000 양방향 보간, D24S8 depth/stencil·occlusion, centroid·sample·EvaluateAttribute와 derivative/UAV를 확인했다. Sample-frequency derivative는 76 covered sample과 76 UAV 증가가 일치한다. Center 2/4/8 full/masked RTV·D32 DSV·occlusion과 독립적인 resource/target quality 7조합도 보존했다.
- **Stage linkage:** case-insensitive semantic+index와 element-local component 배치를 사용한다. `mixed-packed-after10`은 DXIL→DXIL·DXBC→DXIL·DXIL→DXBC 및 별도 clip/ordinary packed float2의 5개 pair에서 정확한 RGBA를 확인했다. Clip의 raw NaN builtin은 primitive rejection을 유지하고 user shadow만 유한 값으로 내보낸다. Masked helper의 atomic 반환값은 `poison` 대신 정의된 0을 사용한다. 이전 masked PS counter가 `15<<32`로 잘못 증가한 회귀도 전체 통계 oracle로 닫았다.
- **Forced raster query:** `forced-query-09fixed-evidence`의 15 case가 API/Shader Validation을 통과했다. 저장한 post-PS virtual sample coverage를 numeric/binary occlusion으로 세고, 1× color replay가 Metal visibility scratch를 초기화하기 전에 query 결과를 fanout한다. Mask·discard·tiny edge·indirect predication·layered replay와 pixel-frequency UAV를 포함한다.
- **공유 D3D11 보존:** packed VS→PS의 기존 오류(`ff800000` 대 `ff804020`)를 수정하고 기존 cache_29가 있는 같은 cache 파일에서 cache_30으로 재컴파일하여 readback을 통과했다. `d3d11-shared/runtime10-packed.log`와 실제 drawable 3,072 pixels=`ff4080bf`를 확인했다. GS→PS의 packed float2도 runtime-10에서 `(0,.625,0,.5)`였던 값을 runtime-11에서 `(.25,.625,.875,.5)`로 수정했다(`mesh-lane-before11.log`, `mesh-lane-after11.log`). 최종 mesh 경계 변경을 위해 compiler cache version은 31이다. D3D12 GS/HS/DS 지원을 추가한 것은 아니다.
- **ROV 최종 통과:** nested buffer ROG만으로는 일반 실행의 ordering이 보장되지 않아 private per-pixel/layer/sample R8Uint ROG marker를 사용한다. Texture-array read ABI를 AIR 2.7의 5인자와 AIR 2.8의 sampler·offset을 포함한 7인자로 구분한다. Direct-root helper의 logical-active metadata는 임시 SSA 값 대신 직렬화 가능한 argument index를 저장한다. `rov-stencil/runtime13-{normal,api,validation}-full15`는 일반·API-only·API/Shader Validation에서 각각 15/15 case를 통과했다. Direct/table sample-frequency 두 경로는 각각 8개 word 모두27, layers27/27/90/90, warm cache27/27이며 validation error·foreign backend load는 없다. Exact AIR의 native ShaderValidation PSO도 통과했다. Stencil DXBC/DXIL30/35의 기존 일반·Shader Validation 증거를 유지한다.
- **최종 linkage runner:** `mixed-packed-after13`은 최신 runtime과 exact marker gate로 mixed DXBC/DXIL·clip·packed 5개 pair를 다시 실행하여 Metal API/Shader Validation을 통과했다.

**SSD 정리에 따른 중간 중단 (2026-09-29).** 추가 계약 대조에서 native clip/cull user varying의 Inf가 0으로 전달되고, 살아남는 primitive의 cull NaN도 보존되지 않는 것을 runtime-13/14에서 재현했다. 단순 마스킹 제거는 일반 실행에서도 실패하고 ShaderValidation 오류도 발생했다. 최신 수정은 원래 VS의 raw 값을 캡처하여 point/line/triangle 거리 입력을 직접 보간하고, native replay의 user shadow는 유한 값으로 유지한다. 거리 입력을 소비하는 PS에는 1×에서도 이 경로를 사용하며, 실제 single-sample coverage texture·Metal helper와 Wine32 옵션 decoder를 함께 변경했다. Compiler cache version은 현재 **32**다.

`runtime-15`의 전체 Ninja/install/staging과 `clip-nonfinite/after15-{normal,validation}`이 통과했다. 1×/4× clip/cull Inf, clip NaN primitive rejection, 살아남는 cull NaN의 실제 PS 입력, point/line/triangle 및 `SV_SampleIndex`, 기존 finite IA·clip·packed 결과를 확인했다. Validation 결과는 오류·foreign backend load 없이 통과했다. CPU build/staging/diagnostics 16+21+44개도 통과했다. Wine32 wire layout·native compilation은 확인했지만 실제 32-bit 실행은 당시 미검증이었다(아래 WoW64 결과로 검증).

**재개 검증 (2026-09-30).** 전용 `clip-nonfinite-depth/probe.cpp`는 1× D32에서 early/late depth·pixel/sample-frequency PS 8개 case의 numeric/binary occlusion, UAV 호출 수, NaN cull 입력 보존과 RT readback을 `runtime-15`의 일반 실행과 Metal API/Shader Validation(`clip-nonfinite-depth/run15-{normal,validation}`)에서 모두 통과했다. 같은 runtime에서 이전 MSAA 통합 fixture 9종(346-case statistics, msaa-lines, center 2/4/8 DXBC·DXIL, forced query, ROV 15 case, attachmentless switch, ROV MSAA4, mixed/packed linkage)도 validation으로 통과했다(`msaa-regression-15/`). Linkage는 현재 runner gate가 요구하는 `DISTANCE_PASS samples=1,4`를 출력하는 현재 fixture로 실행했다.
- **Rasterization-disabled SO:** `D3D12_SO_NO_RASTERIZED_STREAM` PSO에서 실행되지 않는 PS의 clip/cull 입력이 `owned_distance_interpolation`을 켜서 불필요한 MSAA capture/replay 경로를 선택했다. `d3d12_pipeline_graphics.cpp`에서 PS가 실제로 실행될 때(`use_fragment`)만 거리 입력을 검사하도록 수정했다. `stream-output`에 finite/non-finite `SV_ClipDistance0` no-raster case 2개를 추가했다. 두 case는 수정 전 runtime-15에서도 정확한 SO 결과로 통과하므로 출력 결함이 아니라 불필요 경로 선택의 수정이다.
- **SO fixture barrier:** 이후 추가된 queue-time state 검증이 `stream-output`·`so-format-support` fixture의 잘못된 barrier를 device removal로 거부했다. Write state로 암시적 승격된 buffer를 barrier 없이 다른 상태로 쓰거나 `COMMON` before-state를 사용한 fixture 오류이며 translator 검증은 spec대로 유지했다. 두 fixture를 명시적 transition으로 수정하고 root UAV/GPU 인자용 default buffer에 `ALLOW_UNORDERED_ACCESS`를 지정했다.
- **runtime-16 통합:** 위 수정을 포함한 전체 Ninja/install/staging 뒤 `regression-16/`에서 stream-output(validation 104, GPU 45, no-raster distance 2), so-format-support, pipeline-statistics, msaa-lines, dxil-ia, raster-order, execution-safety, indexed-restart, queries가 Metal API/Shader Validation을 통과했다. `runtime-15`에서 pipeline-cache·predication-indirect·stencil-reference도 통과했다. CPU build/staging/diagnostics도 통과했다.

SSD 정리로 완료된 시험 prefix **421개**와 과거 복제 runtime **80개**의 `bin/lib/share` payload를 삭제했다. `build/local`은 **458.6→6.8 GiB**, 볼륨 가용 공간은 **7.4→457.7 GiB**가 됐다. 소스·toolchain·현재 build/install, 기본 runtime과 `runtime-13`/`runtime-15`, 기존 runtime manifest·fixture·로그·result.json·drawable capture는 보존했다. 보존한 runtime-13/15는 staging verifier의 manifest·builtin load policy·winemetal dependency 검사를 통과했다. 삭제된 과거 runtime/prefix를 다시 실행하려면 새 staging과 새 시험 prefix가 필요하다. 2026-09-30 P1 종료 뒤에는 최신 `runtime-20`, 비교용 `runtime-16`, WoW64 `build/local/wow64/runtime-final`의 payload를 유지하며, 나머지 runtime은 manifest·provenance만 보존한다.

`Present(RESTART)`는 queue에 남은 frame과 이미 Metal에 commit된 frame을 모두 폐기한다(아래 P1 종료 결과). Core Animation에 이미 넘긴 drawable은 규격상 표시 완료로 취급한다.
HDR10PLUS는 제약으로 미충족, 물리 scanout 색 정확도·다중 출력 전환은 미검증 경계로 남긴다.

### P1 완료 판정과 남은 계약

**P1 완료(2026-09-30).** A–D 및 위에 기록한 resource·format·execution·shader/PSO 결과에 더해, 거리 보간의 1× depth/occlusion·MSAA 통합 회귀, 이미 commit된 frame까지 포함한 `Present(RESTART)` 폐기, WoW64 실행을 닫았다. HDR10PLUS는 제약으로 미충족이며 명시적으로 거부한다. 물리 HDR scanout·다중 출력 전환·다른 GPU/OS는 이 장치에서 실행할 수 없는 미검증 경계다. A는 UINT와 Apple8+ UINT64, B는 present queue rotation·transactional resize, C는 MSAA를 포함한 11-field statistics, D는 VS→SO(stream 0) direct/indirect와 SO statistics 범위다. P1-C의 346 GPU case·26 validation case는 최신 compiler(`runtime-15`, `runtime-16`, `runtime-20`)에서도 다시 통과했다.

#### P1 종료 결과 (2026-09-30)

- **Native `Present(RESTART)`:** 이전에는 Metal에 commit된 present가 하나라도 있으면 `DXGI_ERROR_UNSUPPORTED`를 반환했다. 이제 drawable은 present command buffer의 GPU 완료 handler에서만 Core Animation에 넘긴다. 이때 presenter의 restart epoch를 `MTLSharedEvent` token과 비교해 바뀌었으면 present하지 않고 presentation observer도 취소한다(새 WineMetal call 214 `MTLCommandBuffer_presentDrawableIfCurrent`, 64-bit·WoW64 table 모두). Order event·credit·semaphore 회수는 그대로라 뒤 frame이 막히지 않는다. D3D11 present 경로는 변경하지 않았다. 대가로 present가 GPU 완료 후 handler에서 발생하므로 최대 한 frame의 지연이 늘 수 있다.
  - 증거: `present-queues`의 `RESTART_NATIVE` case(ResizeBuffers1 queue B/A, B 차단, frame 1 queue 대기·frame 2 native commit 뒤 RESTART). `runtime-17`/`runtime-19` 이후 S_OK·0 ms·정확한 credit·count/index와 fullscreen 복원을 통과했고, 이전 `runtime-16`은 같은 fixture에서 `0x887a0004`로 실패했다. Native 결정 trace(`OWNED_GRAPHICS_NATIVE_PRESENT_DECISION`)는 case 구간에서 `present_id=2 presented=0 reason=epoch`, `present_id=3 presented=1 reason=current`만 기록했고 runner가 이를 gate한다(`regression-17/present-queues-decision`). Core Animation `presentedTime`은 이 환경에서 모든 frame에 0을 보고해 표시 증거로 쓰지 않았다.
- **HDR10PLUS 분류:** macOS SDK 27의 `CAEDRMetadata`는 HDR10 정적 MDCV·CLL과 HLG만 받고 ST 2094-40 동적 metadata 입력이 없다. Microsoft는 `DXGI_HDR_METADATA_HDR10PLUS`/`D3DDDI_HDR_METADATA_HDR10PLUS`를 문서화되지 않은 `BYTE Data[72]`로만 정의한다. 의미 보존 변환이 불가능하므로 `DXGI_ERROR_UNSUPPORTED`로 거부하고 상태를 바꾸지 않는다(`hdr-metadata`의 `reject-hdr10-plus`).
- **WoW64(32-bit) D3D12:** 32-bit frontend는 이전에 실행 불가능했다(descriptor handle·shader-visible heap의 aborting `IMPLEMENT_ME`, 4 GiB 위 native 포인터로 인한 NULL `Map`, compiler 출력 역참조). 수정: 32-bit descriptor handle registry(stride 32, 외부·범위 밖 handle 거부), 하위 2 GiB에 할당한 CPU-visible buffer와 heap(WoW64 table의 `_MTLDevice_newBuffer32`), compiler 출력 복사 call 215 `sm50_copy_compiled_bitcode`(64-bit는 기존 zero-copy), 남은 `IMPLEMENT_ME`의 계약 오류 대체. `scripts/stage-owned-graphics.py --dxmt-install32`가 i386 module을 machine type·marker와 함께 staging·검증한다. 최종 tree의 `build/local/wow64/runtime-final`에서 PE32 fixture의 stream-output(validation 104·GPU 45·no-raster distance 2), indexed-restart(DXBC/DXIL 62 case), so-format-support, resources, smoke가 Metal API/Shader Validation으로 통과했다(`build/local/wow64/*-final`). 32-bit present/fullscreen·MSAA·DXR suite와 하위 2 GiB 고갈 동작은 실행하지 않았다.
- **Fixture 수정:** `temporal`의 opposite-order case는 이전 실행이 끝나지 않은 같은 list를 반복 제출해, D3D12 규칙(\"previous execution has not yet completed\"면 drop)을 위반했다. 이번 queue 검증 이전 runtime-15에서도 같은 device removal로 실패했다. iteration마다 별도 recording을 사용하도록 고쳤다. Runner의 stale gate(`present-queues` rejected=12, format-support의 제거된 packed422 marker)도 현재 fixture에 맞췄다. `owned-d3d12-hdr-presenter.cpp`는 frame 사이에 live EDR scale이 바뀌면(관찰값 0.0625→0.075/0.32/0.40) 서로 다른 scale의 highlight 순서를 비교해 실패했다. 순서 비교는 같은 scale일 때만 하고, 모든 frame은 기존의 scale-aware 독립 PQ/EETF oracle로 계속 검사한다. `runtime-20`에서 2회 통과했다. `colorspace-presenter`(11 frame)와 `present-geometry`도 같은 runtime에서 통과했다.
- **최종 64-bit 회귀(`runtime-20`, Metal API/Shader Validation):** smoke(present/no-present), multiqueue, temporal, core-contracts, execution-safety, fence-events, bundles, buffer-rtv, resources, residency, copy-resolve, copy-resource-casts, depth-plane-copy, reinterpret-copy, packed-cubearray, partial-3d-uav-dxil, standard-swizzle, format-support, expanded-formats, capabilities, predication-indirect, pipeline-cache, stencil-reference, indexed-restart, queries, atomic-copy, pipeline-statistics, msaa-lines, multisample-capabilities, dxil, dxil-texture, dxil-ia, raster-order, stream-output, so-format-support, raytracing, ngx, ngx-contracts, hdr-metadata, colorspace, fullscreen이 통과했다(`build/local/p1-complete/regression-20/`). present-queues(native RESTART decision gate 포함)도 최종 `runtime-20`에서 통과했다. 공유 D3D11 baseline(3D clear·DXBC compute·sample·windowed Present `4080bfff`)도 validation 오류 없이 통과했다. CPU build/staging/diagnostics와 NGX frontend 검사도 통과했다.

아래는 기존 P1 정의에 속하는 잔여 항목과 후속 구현 결과다. 2026-09-28의 소스 감사 뒤, 2026-09-29에 fullscreen 오류·전환, D3D12 HDR10 metadata와 color-space 지원·적용 순서를 실제 런타임에서 수정·검증했다. Color-space의 미지원 설정 성공 반환도 이전 runtime에서 재현했다. 이후 작업은 위 D3D11 유지 방침을 따른다.

| 잔여 항목 | 현재 근거와 상태 | 닫아야 할 계약 |
|---|---|---|
| HDR metadata | **D3D12 HDR10 적용·교체·해제 및 기존 tone-map 경로 구현·검증:** 아래 HDR10 결과 참조. D3D11 setter는 변경하지 않음. **HDR10PLUS는 제약으로 미충족:** macOS SDK 27의 `CAMetalLayer.EDRMetadata`/`CAEDRMetadata`는 HDR10 정적 MDCV·CLL과 HLG만 입력으로 받으며 ST 2094-40 동적 metadata 입력이 없다. Microsoft는 `DXGI_HDR_METADATA_HDR10PLUS`/`D3DDDI_HDR_METADATA_HDR10PLUS`를 불투명한 `BYTE Data[72]`로만 정의하고 내부 layout을 문서화하지 않았다. 따라서 의미를 보존하는 변환이 불가능하고, 성공 반환 없이 `DXGI_ERROR_UNSUPPORTED`로 거부하며 상태를 바꾸지 않는다(`hdr-metadata`의 `reject-hdr10-plus`) | native HDR10 metadata 전달·다른 display/GPU 조합은 미검증 |
| Color-space 설정 | **D3D12 지원·설정 일치, 실패 보존·resize·GPU 완료 순서 구현·검증:** 아래 color-space 결과 참조. D3D11 전용 코드는 변경하지 않음 | Core Animation 합성/물리적 scanout의 색 정확도, 다중 출력·EDR 전환, Windows reference differential은 미검증 |
| Fullscreen 오류·전환 | **선정 실패·단일 출력 전환 계약 구현·검증 완료:** 양쪽 frontend의 출력 조회·native 전환·`ResizeTarget` 오류와 상태 보존, fullscreen 생성, resize/readback/present 및 반복 이탈을 검증했다. 아래 fullscreen 결과 참조 | 다중 모니터 target 변경·hot-unplug·OS display-mode 복구 실패의 실제 검증은 남음 |
| Format·지원 보고 | **선정 정합성 수정 구현·검증 완료:** SO 12종, format/view·mip·cast, D24 copy, quality query·resource/PSO preflight와 2×/4×/8× 실행을 순서대로 검증했다. 아래 후속 결과 참조 | 98종 query와 명시한 GPU oracle의 범위이며 모든 format·dimension·연산 조합의 conformance는 아님. 다른 GPU/OS와 Windows reference differential은 미검증 |
| 공유 backend 회귀 | **기존 증거 유지:** D3D11 fullscreen·resize·GPU readback·Present와 출력 proxy 수명을 실제 실행했다. D3D10/11 전용 기능 검증은 별도 P1 목표가 아니다. | D3D12 필수 공유 변경이 기존 동작을 깨뜨리지 않는지 영향 범위만 검증 |

GS/HS/DS 및 이에 의존하는 adjacency/patch·SO 확장, reserved/tiled/shared/cross-adapter·enhanced barrier·pipeline library는 **기존 계획대로 P2**다. 이 항목들을 P1로 옮겨 완료 조건을 늘리지 않는다. UINT64의 Apple8+ 조건, MSAA의 AIR 2.8/macOS 26 target, 실제 GPU 검증이 M5 Pro/macOS 27에 한정된 점은 지원·증거 경계로 별도 유지한다. WoW64(32-bit process) 실행은 아래 P1 종료 결과에서 검증했다. 32-bit component format 12종의 x64 SO 실행은 아래 별도 결과대로 검증했다.

### P1 구현 완료 항목

2026-09-22에 선정한 P1-A/B/C/D와 이후 추가한 indexed restart·SO indirect/오류 전파·buffer UAV clear·RS 1.1 계약을 구현·검증했다. Timestamp/occlusion query, `SetPredication`, `ExecuteBundle`, `SetEventOnMultipleFenceCompletion`, `ResolveSubresourceRegion`, residency 확장과 기본 DXIL 경로도 구현되어 있다. 각 결과의 지원 범위·실행 증거·남은 제약은 아래에 유지한다.

| 항목 | 기능 | 현재 소스 상태 | 구현 계약 |
|---|---|---|---|
| **P1-A** | `AtomicCopyBufferUINT` / `AtomicCopyBufferUINT64` | **UINT와 macOS Apple8+ UINT64 publish 구현.** 실제 GPU 증거와 reader 관찰 한계는 아래 참조 | DIRECT/COMPUTE/COPY에서 primary element의 32/64-bit atomic publish와 dependent resource range ordering을 정확히 구현한다. Bundle에서는 허용하지 않는다. |
| **P1-B** | `IDXGISwapChain3::ResizeBuffers1` / present queue rotation | **구현·검증 완료**(아래 결과 참조) | backbuffer별 queue를 보존하고 `Present`의 buffer 회전과 같은 index로 present queue를 회전한다. Resize를 transactional하게 만들고 실패 시 기존 swapchain 상태를 보존한다. |
| **P1-C** | 정확한 `PIPELINE_STATISTICS` | **공개 구현·GPU 검증 완료.** DXBC 168개·DXIL 178개, 총 346 GPU case와 26개 validation case PASS | 11개 필드와 독립 UAV·RTV oracle을 검증한다. MSAA에서 실제 pixel-frequency 실행을 구현했으며 counter 나누기나 MSAA 제외로 대체하지 않는다. |
| **P1-D** | Stream Output + SO statistics | **VS→SO(stream 0) direct/ExecuteIndirect 구현·검증 완료.** P1 당시 adjacency/patch, GS/HS/DS 조합은 `E_NOTIMPL`이었다. GS/DS SO 후속 검증은 [P3 완료 결과](#p3-완료-결과-2026-10-01) 참조 | SO declaration/binding/write offset/filled-size counter와 `SO_STATISTICS_STREAM0..3`를 compiler/runtime/query까지 end-to-end로 구현한다. |
| **P1 추가 수정** | 일반 non-SO indexed primitive restart | **구현·GPU 검증 완료**(아래 결과 참조). SO 오류 전파/WoW64 및 SO ExecuteIndirect도 구현·검증 | Strip에서 cut disabled 또는 index format과 cut 값 불일치인 경우만 application index pulling; list와 일치 strip은 native 유지 |
| **P1 추가 계약** | 부분 buffer UAV clear / RS 1.1 static descriptor bounds | **구현·검증 완료**(아래 결과 참조) | Clear는 view-relative rect와 predicate를 보존한다. KEEP_BOUNDS flag의 유효 조합과 descriptor-table 경계 밖 접근 규칙을 맞춘다. |

A–D는 기존 P1 정의 안에서 먼저 닫은 개별 묶음이다. 이 목록의 완료가 D3D12 resource/format·DXGI를 포함한 P1 전체 완료를 뜻하지 않는다. 새 correctness·수명·data-corruption 문제는 D3D12 및 그 필수 공유 경로 범위에서 단계와 관계없이 먼저 수정한다.

#### P1-A — AtomicCopyBufferUINT / UINT64 구현

Microsoft 계약상 primary element는 source에서 destination으로 원자적으로 복사되며, 지정한 dependent subresource range는 primary publish 전에 완전히 갱신되어 있어야 한다. `UINT`/`UINT64` source·destination 주소는 각각 자연 정렬을 만족해야 한다. DIRECT/COMPUTE/COPY command list에는 허용되지만 BUNDLE에는 허용되지 않는다.

- record 단계에서 source/destination이 같은 owned device의 buffer인지, 4/8-byte alignment와 bounds, dependency array/count/null 조합, 각 dependent subresource range를 검증한다. Void API이므로 첫 recording error를 기존 방식대로 `Close`까지 보존한다.
- 기존 `ComputeEncoderData` pass에 native buffer binding과 내부 pipeline을 기록한다. queue worker는 command-list 객체를 다시 읽지 않는다. Dependent range는 검증하며, 전체 선행 pass의 write를 완료시키는 encoder fence를 사용하므로 별도 range snapshot은 필요 없다. Application resource는 D3D 계약대로 GPU 완료까지 유효해야 한다.
- dependent payload의 선행 write가 Metal memory model에서 visibility-complete된 뒤 primary를 publish해야 한다. 단순 4/8-byte blit을 D3D12 atomic copy와 같다고 가정하지 않는다. 지원하는 Metal atomic primitive 또는 검증된 equivalent sequence만 사용한다.
- 64-bit atomic 지원은 현재 M5 Pro 한 장치의 성공을 전체 Metal family로 일반화하지 않는다. 필요한 native capability를 명시적으로 확인하고, exact 구현이 불가능한 조합은 silent fallback 없이 unsupported로 남긴다.
- fixture는 direct/compute/copy list, 32/64-bit, dependency 0/N, natural-alignment/OOB/foreign-device 오류, late-latch 형태의 payload-before-primary 관찰을 포함한다.

공식 계약: [AtomicCopyBufferUINT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist1-atomiccopybufferuint), [command-list API restrictions](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles).

**P1-A 결과 (2026-09-28).**

- 공통 검증 `ValidateAtomicCopy`: 같은 device의 buffer, 자연 정렬, primary bounds, `Dependencies != 0`일 때 두 배열 non-null, dependent resource 소유권, `Range.Begin <= End`, buffer는 `Subresource == 0`·`End <= Width`, texture는 subresource index 범위를 검사한다. 오류는 첫 recording error로 `Close`까지 보존된다. Bundle은 `E_INVALIDARG`.
- `UINT`: 단일 thread compute pass에서 `atomic_load_explicit`/`atomic_store_explicit`(`atomic_uint`, AIR `air.atomic.global.load/store.i32`)로 publish한다. 별도 typed pass이므로 queue의 pass 경계 `waitForFence`/`updateFence`가 앞선 payload write의 완료와 이후 소비자의 관찰 순서를 보장한다. Dependent range는 이 보수적 경계 아래서 별도 동기화가 필요 없으므로 검증만 한다. Predication 대상 연산이 아니므로 predicate를 적용하지 않는다.
- `UINT64`: macOS의 `supportsFamily(Apple8)`를 확인한 뒤 안정된 source의 `ulong` 값을 읽고, 같은 값 `v`로 `atomic_min_explicit(dst, v)` → `atomic_max_explicit(dst, v)`를 실행한다. 단일 publisher에서 중간 `min(old, v)`는 old 또는 v이고 최종 값은 v다. 두 32-bit store나 일반 blit으로 대체하지 않는다. UINT와 같은 encoder fence가 payload-before-primary 순서를 보장하며 predication을 적용하지 않는다. Capability 검사는 내부 PSO를 처음 만들 때만 수행한다.
- 이 조합은 일반적인 다중 writer atomic store가 아니다. [D3D multi-queue 계약](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)의 resource당 단일 writer·writer 전환 시 동기화가 필요하다. [Apple feature table](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)의 Apple8 macOS 64-bit min/max 지원을 기준으로 gate하며 다른 family는 `E_NOTIMPL`이다. 다른 shader용 64-bit atomic load/store 지원을 선언하는 변경은 아니다.
- 기존 UINT 증거: `graphics-translator/tests/owned-d3d12-atomic-copy.cpp`, suite `atomic-copy`. DIRECT/COMPUTE/COPY × dependency 0/1에서 같은 list 안의 source 생성 → payload copy → publish → 후속 copy 관찰, guard 보존과 18개 validation case를 Metal validation 아래 통과했다(`build/local/p1a/runtime-01`, `build/local/p1a/proof-01`).
- 리뷰 반영: command list가 소유한 내부 pipeline(atomic copy와 기존 predication kernel)을 `BeginPredicationComputePass`가 `ComputeEncoderData::internal_pipeline`으로 보유한다. 제출 뒤 list를 해제해도 queue worker가 해제된 PSO handle을 쓰지 않는다. 재검증: `proof-03`.
- Reader 관찰 범위: `Load2`는 32-bit load 두 개이므로 64-bit atomic snapshot의 oracle로 사용하지 않는다. 동시 reader fixture는 high word를 고정하고 globally coherent low-word generation과 해당 immutable payload만 관찰한다. Device memory barrier를 사용하며 유한 반복 내 같은 shader thread의 값 변화가 있어야 관찰 성공으로 판정한다. 다른 GPU/OS와 전체 64-bit shader snapshot은 별도 검증 범위다.
- UINT64 확장 증거: DIRECT/COMPUTE/COPY × dependency 0/2 × 6개 값 조합(36회), GPU compute가 생성한 source/payload, false predication 무시, 33개 validation case, 닫힌 list 재실행 2회와 제출 뒤 list 해제를 통과했다. Ordered reader control은 256개 thread 모두 최종 generation 128과 해당 payload가 정확했다. M5 Pro/macOS 27에서 API+Shader Validation을 켠 `build/local/atomic64-implementation/proof-core-final`, runtime `runtime-05`를 사용했다. 공용 내부 compute pass의 `predication-indirect` 회귀도 같은 validation 모드에서 통과했다(`proof-predication-final`).
- 동시 관찰 증거: `atomic-copy-live --api-validation`에서 256개 thread 중 64개가 같은 shader thread 안의 generation 변화를 관찰했다(전체 관찰 범위 1..128). 관찰된 payload는 모두 일치했고 ordered control도 통과했다(`build/local/atomic64-implementation/proof-live-final`). Native/owned reader의 계측 on/off 대조에서 Shader Validation이 compute 실행을 직렬화하는 것을 확인했다. 따라서 기본 suite는 계측을 켠 계약·수명·ordered control을 검증하고, live suite는 API Validation을 유지한 실제 동시 관찰을 별도로 요구한다. `LIVE_INCONCLUSIVE`는 live suite의 성공이 아니다. 기존 compiler로도 동시 관찰이 통과하여 실험용 coherent lowering 변경은 제거했다.
- Reader 입력: `graphics-translator/tests/dxil/atomic-copy-reader.hlsl`과 frozen `atomic-copy-reader.dxil`(4,124 B, SHA-256 `b075b9d8708a0b8f0fca94428762f4af7ac8b88aa7c4b7397540b6f17f375eb5`). Wine HLSL frontend가 이 reader의 `globallycoherent`/`RWByteAddressBuffer.Load`를 처리하지 못하므로 frozen DXIL을 사용한다. 아래 frozen shader 절과 같은 로컬 DXC 1.9.0.5402로 `-T cs_6_0 -E main -HV 2021 -O0`을 사용해 생성했다. Runner가 입력을 복사하고 실제 owned DXIL compiler load를 확인한다.

재현(README의 방법으로 새 runtime을 stage한 뒤, 매번 새 output 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-atomic-copy.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -ldxguid -lole32 -o build/local/atomic-copy.exe
python3 scripts/test-owned-graphics.py --runtime build/local/runtime \
  --suite atomic-copy --fixture build/local/atomic-copy.exe \
  --out build/local/atomic-copy-core-proof --validation
python3 scripts/test-owned-graphics.py --runtime build/local/runtime \
  --suite atomic-copy-live --fixture build/local/atomic-copy.exe \
  --out build/local/atomic-copy-live-proof --api-validation
```

#### P1-B — ResizeBuffers1 / present queue 구현

`ResizeBuffers1`의 `ppPresentQueue`는 backbuffer 수만큼의 D3D12 queue 배열이며, `Present`가 다음 buffer로 회전할 때 queue도 함께 회전한다. 이전의 queue 배열 무시 경로를 제거하고 아래의 backbuffer별 queue 보존·transactional resize로 교체했다.

- swapchain에 backbuffer별 `present_queues_`를 보존한다. 일반 `ResizeBuffers`는 기본 creation queue를 각 slot에 적용하고, `ResizeBuffers1`은 caller 배열을 검증·snapshot한다.
- `Present1`은 현재 backbuffer index와 동일한 index의 present queue에 task를 enqueue한다. 기존 queue-local FIFO/Wait/Present ordering을 재사용한다.
- `ResizeBuffers`와 `ResizeBuffers1`은 공통 internal transaction으로 합친다. 새 descriptor, queue vector, backbuffer vector와 layer property가 모두 준비된 뒤 commit해 중간 실패가 기존 swapchain을 반쯤 파괴하지 않게 한다.
- node mask/queue ownership/type/count를 먼저 검증하고, 기존 backbuffer reference가 남은 resize의 DXGI 오류 계약을 별도 fixture로 고정한다.
- `ApplyLayerProps`의 이전 flush TODO는 accepted present의 encode 완료 대기로 대체했고, 아래 color-space 후속 작업에서 D3D12 GPU 완료 대기로 강화했다. `SetColorSpace1`은 검증된 다음 색 공간만 저장하며 다음 Present가 같은 GPU 완료 경계에서 적용한다.

공식 계약: [IDXGISwapChain3::ResizeBuffers1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1).

**P1-B 결과 (2026-09-28).**

- `d3d12_swapchain.cpp`: `present_queues_`와 `current_buffer_`를 backbuffer와 같은 index로 회전한다. `Present1`은 현재 index의 queue에 enqueue하고 성공 시에만 index를 전진한다. `GetCurrentBackBufferIndex`는 `current_buffer_`를 반환한다.
- `ResizeBuffers`/`ResizeBuffers1`은 `ResizeBuffersImpl` 하나로 합쳤다. Buffer count(0=유지, 최대 `DXGI_MAX_SWAP_CHAIN_BUFFERS`, flip model 최소 2), format, 남은 application backbuffer reference, `ResizeBuffers1`의 node mask(0/1만 허용)·queue(같은 device의 DIRECT queue, null 금지)를 먼저 검증하고, 새 desc·queue·backbuffer를 모두 만든 뒤 한 번에 교체한다. 실패는 `DXGI_ERROR_INVALID_CALL` 또는 생성 오류를 반환하고 기존 상태를 그대로 둔다. 성공 시 index는 0이다. 일반 `ResizeBuffers`는 모든 slot을 creation queue로 되돌린다.
- 당시 교체 전 `Presenter::waitPresentsEncoded()`로 이미 받아들인 present의 encode 완료를 기다렸다. 현재는 아래 color-space 후속 작업의 `waitPresentsCompleted()`로 GPU 완료까지 기다린 뒤 layer property를 바꾼다.
- Outstanding reference 검사가 내부 보유를 app reference로 오인하지 않도록 queue의 `PresentTask`와 temporal frame의 resource 보유를 private ref(`Com<MTLD3D12Resource, false>`)로 바꿨다. GPU 완료까지의 수명은 그대로 유지된다.
- 증거: `graphics-translator/tests/owned-d3d12-present-queues.cpp`, suite `present-queues --mode present`. 9개 거부 case 뒤 desc·index 보존, queue B만 fence wait로 막았을 때 buffer 0(queue A) present는 완료되고 buffer 1(queue B) present는 release 전까지 pending, 일반 resize 후 3-buffer rotation이 queue A에서만 완료됨을 Metal validation 아래 확인했다(`build/local/p1b/runtime-01`, `proof-01`). 회귀: `smoke --mode present`, `temporal` 통과(`proof-smoke`, `proof-temporal`).
- 리뷰 반영: 여러 present queue에서 뒤 frame이 앞 frame을 추월하지 않도록 presenter별 `MTLSharedEvent`(`present_order_event_`)로 frame N이 N−1을 GPU에서 기다리고 N을 signal한다. queue worker는 CPU에서 막히지 않는다. encode 완료 watermark는 최대 frame id가 아니라 완료 개수(`frames_encoded_`)라서 `waitPresentsEncoded`가 앞 frame이 남은 채로 반환하지 않는다. `SwapChainFlags`를 반영하고 `FRAME_LATENCY_WAITABLE_OBJECT` 변경은 `DXGI_ERROR_INVALID_CALL`로 거부한다. Fixture는 queue B의 frame이 막힌 동안 queue A의 다음 frame이 완료되지 않음을 검사하며, GPU order wait를 제거한 build에서 `present-order-not-overtaken`으로 실패함을 확인했다(임시 build, 증거 경로 `build/local/p1b/proof-probe`). 재검증: `runtime-03`의 `proof-03`, `proof-smoke-03`, `proof-temporal-03`.
- 남은 경계: multi-node adapter가 없으므로 node mask는 0/1만 허용한다. 단일 출력 fullscreen/resize/present는 아래 후속 fixture로 검증했지만 다중 present queue와 fullscreen 전환을 동시에 조합한 검증은 하지 않았다. 신호되지 않는 queue Wait 뒤에 present가 남아 있으면 resize는 그 present의 GPU 완료까지 기다린다. `PresentTask`의 backbuffer 참조는 기존 경로대로 encode 후 해제되며 GPU retirement까지의 보유는 Metal command buffer의 resource 보유에 의존한다. 500ms pending probe는 지연된 잘못된 queue 실행을 완전히 배제하지 못한다.

#### P1 후속 — Fullscreen 오류·전환 (2026-09-29)

- D3D12와 D3D11의 `SetFullscreenState`/`ResizeTarget` 전체 전환을 mode-set guard로 보호한다. `FALSE`와 non-null target 조합은 `DXGI_ERROR_INVALID_CALL`이며, 출력 `QueryInterface`/`GetDesc`/mode 조회 오류를 검사한다. 재진입은 `DXGI_STATUS_MODE_CHANGE_IN_PROGRESS`다. Window API와 output COM 호출 중 swapchain mutex를 보유하지 않는다.
- Native 전환 성공 뒤에만 `Windowed`·monitor·target·fullscreen descriptor를 게시한다. 실패 시 이전 논리 상태를 유지하고 실제 이전 display mode의 복구를 시도한다. Win32 WSI는 창/monitor 조회, style 변경, 위치 변경의 실패를 반환하며 살아 있는 창의 부분 변경을 되돌린다. `ResizeTarget`은 실패한 mode 변경 뒤 창을 갱신하거나 descriptor를 먼저 바꾸지 않는다.
- Fullscreen-at-creation도 실제 `SetFullscreenState(TRUE)`를 거치고 실패를 caller에 반환한다. 공유 DXGI factory는 파괴된 HWND를 Metal view 생성 전에 거부하고 output을 null로 유지한다. 기존 경로에서 발생한 WineMetal assertion을 숨기지 않고 잘못된 입력이 native layer까지 도달하지 않도록 수정했다.
- D3D11 fullscreen `Present1`은 전달받은 `IDXGIOutput1`을 `MTLDXGIOutput`으로 직접 캐스팅하지 않고 공개 `GetGammaControl`을 사용한다. 유효한 forwarding output proxy로 실제 Present 시 발생한 execute-access fault를 수정했다. Gamma curve가 바뀔 때만 cached ramp를 갱신한다.
- Permanent fixture: [`owned-dxgi-fullscreen.cpp`](tests/owned-dxgi-fullscreen.cpp), runner suite `fullscreen --mode present`. 각 API에서 8개 오류/재진입 경계, 두 번의 fullscreen 진입·이탈, 창 style/rect 복원, fullscreen 생성과 invalid HWND 생성 거부를 확인한다. Native 진입·이탈 중 `WM_STYLECHANGING`에서 HWND를 파괴하여 실패 반환과 pre-call 논리 상태 보존을 검사한다. 72×54 backbuffer의 **3,888픽셀/API**가 독립 clear 기대값과 일치하고 D3D12 fence/D3D11 query 완료 및 Present가 성공했다.
- 수정 전: `build/local/fullscreen-runner-baseline-29`에서 `SetFullscreenState(FALSE, target)`이 `S_OK`를 반환하여 실패. 중간 `fullscreen-proof-29`는 invalid HWND 생성의 assertion, `fullscreen-proof-30`은 D3D11 output proxy Present 충돌을 보존한다. 최종 `build/local/fullscreen-runtime-31` + `build/local/fullscreen-proof-31`은 **D3D12·D3D11 모두 PASS**, Metal API/Shader Validation 오류와 foreign backend 로드 없음. 빌드·install·새 runtime stage도 통과했다.
- 기존 `present-queues` 회귀는 `fullscreen-runtime-29` + `fullscreen-present-queues-proof-29`에서 PASS(거부 10개, queue rotation/blocked ordering, 일반 resize 3-buffer). 이후 변경은 invalid HWND factory 검사와 D3D11 gamma 조회다. Runner diagnostics 44개도 통과했다.
- 증거 경계: M5 Pro/macOS 27, x86_64 Wine, 단일 출력. 다중 모니터 전환·hot-unplug·다른 GPU/OS·일반 D3D10 workload와 실제 게임은 미검증이다. 외부에서 파괴한 HWND 자체는 복원할 수 없으며, OS가 display-mode 복구까지 거부하는 경우 native 복구는 보장하지 않는다. HDR·color-space·전체 format/sample 지원 보고는 여전히 P1 잔여 항목이다.

재실행(출력 경로는 새 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-dxgi-fullscreen.cpp \
  -ld3d12 -ld3d11 -ldxgi -ldxguid -lole32 -o build/local/owned-dxgi-fullscreen.exe
python3 scripts/test-owned-graphics.py --runtime build/local/fullscreen-runtime-31 \
  --fixture build/local/owned-dxgi-fullscreen.exe --suite fullscreen --mode present \
  --out build/local/fullscreen-proof-new --validation --timeout 120
```

#### P1 후속 — D3D12 HDR10 metadata (2026-09-29)

- `SetHDRMetaData`의 무처리 `S_OK`를 제거했다. HDR10은 정확한 `sizeof(DXGI_HDR_METADATA_HDR10)`와 non-null pointer를 요구하고 잘못된 경우 `E_INVALIDARG`를 반환한다. 구조체 전체를 값으로 복사하므로 호출자 메모리를 보유하지 않는다. HDR10PLUS 및 알 수 없는 type은 `DXGI_ERROR_UNSUPPORTED`이며 기존 metadata를 바꾸지 않는다.
- `NONE`은 metadata를 해제한다. 추가적인 size/pointer 제한이 공식 문서에 명시되지 않아 기존 의도대로 나머지 인자를 읽지 않고 무시한다. 위 세부 오류 선택은 이 구현의 정책이며 Windows reference differential 결과를 주장하지 않는다.
- 기존 `PresentState`가 수락한 frame의 수치 metadata를 복사한다. `changeHDRMetadata`는 metadata 유무가 바뀔 때만 PSO를 무효화하고 값 교체에는 재생성하지 않는다. Setter는 pending queue를 flush하지 않는다. 유무가 바뀐 뒤 다음 Present의 경계는 아래 color-space 작업에서 D3D12 GPU 완료까지 강화했다. 기존 display override가 있으면 swapchain metadata보다 우선한다.
- D3D12 활성화에 필요한 공유 수정 두 곳: native layer display query의 no-screen 반환값을 version 0·SRGB·metadata 0·EDR `{1,1}`로 초기화했고, BT.2408 EETF는 `KS >= 1`일 때 shoulder만 건너뛰어 같은 content/display peak의 `0/0`을 막으면서 검정색 remap을 유지한다. D3D11 전용 파일·기존 fixture·NVAPI/NGX 코드는 변경하지 않았다.
- **공개 API 증거:** `owned-d3d12-hdr-metadata.cpp`, suite `hdr-metadata --mode present`. 잘못된 HDR10 인자 4개·미지원 type 2개, 적용/교체/해제·호출자 메모리 변경·resize, fence로 막힌 accepted Present 뒤의 metadata 교체와 정상 release를 검사한다. 서로 다른 7개 frame의 원본 3,072픽셀 readback 및 GPU fence/Present가 성공했다. 이 readback은 tone mapping 전 백버퍼이며 metadata 출력 효과의 증거로 대신하지 않는다.
- **실제 출력 증거:** `owned-d3d12-hdr-presenter.cpp`는 production `Presenter`·`InternalCommandLibrary`에 링크하고 실제 HWND/CAMetalLayer drawable을 같은 Metal command buffer에서 blit/readback한다. A(peak 3000) 수락 뒤 B(10000)로 교체해도 A의 snapshot을 보존하고, B 적용·NONE 해제·B 재적용·content=display 경계의 픽셀을 독립 double-precision PQ/EETF 계산과 비교했다. 관찰된 highlight는 A `0.491211`, B `0.474121`, NONE 및 equal-peak `0.494141`; black은 약 `0.000001`, display peak는 1600, EDR scale은 0.0625였다. 분리된 두 fixture는 public API와 production presenter를 각각 증명하며 compositor 이후 화면/모니터 광출력의 end-to-end 계측은 아니다.
- **재현과 검증:** 이전 `fullscreen-runtime-31` + `hdr-metadata-baseline-02`는 zero-size HDR10에 `S_OK`를 반환하여 실패했다. 최종 `hdr-metadata-runtime-02` + `hdr-metadata-public-04`는 Metal API/Shader Validation으로 PASS(오류·foreign backend 로드 없음). Drawable probe도 `build/local/hdr-presenter-validation-02.stdout.log`에서 5개 frame·detached query PASS, 같은 이름의 stderr에 Metal API/GPU Validation 활성화와 validation 오류 없음이 기록됐다. 빌드·install·새 runtime stage와 runner diagnostics 44개도 통과했다. Detached native query는 이전 runtime에서 NaN/0을 반환했고 수정 후 `{1,1}`을 반환했다. 이 native baseline은 현재 shader를 링크한 probe로 이전 native module만 비교한 것이며, 이전 shader 실행 증거가 아니다.
- **범위:** HDR10을 metadata-guided 기존 tone mapper에 연결한 것이다. 보관한 primaries/white-point/min-luminance 전부를 native HDR10 신호로 전달한 구현이 아니며 현재 tone mapper는 luminance maxima를 소비한다. HDR10PLUS는 미지원이다. Microsoft도 metadata 전달을 보장하지 않고 pixel 해석은 `SetColorSpace1`이 결정한다고 명시한다. 다른 display/GPU·Windows reference differential은 남아 있으며 color-space 후속 결과는 아래에 구분했다.

공식 계약: [SetHDRMetaData](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgiswapchain4-sethdrmetadata). 공개 API fixture 재실행(출력 경로는 새 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-hdr-metadata.cpp \
  -ld3d12 -ldxgi -o build/local/owned-d3d12-hdr-metadata.exe
python3 scripts/test-owned-graphics.py --runtime build/local/hdr-metadata-runtime-02 \
  --fixture build/local/owned-d3d12-hdr-metadata.exe --suite hdr-metadata --mode present \
  --out build/local/hdr-metadata-public-new --validation --timeout 120
```

Production presenter probe는 위 runtime과 같은 빌드의 static library를 사용한다. 빌드 후 다음처럼 새 prefix에서 실행한다(게임 prefix 사용 금지).

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  -Iinclude -Isrc/dxmt -Isrc/winemetal -Isrc/util -Ilibs \
  -D_WIN32_WINNT=0xa00 -DDXMT_PAGE_SIZE=4096 \
  graphics-translator/tests/owned-d3d12-hdr-presenter.cpp \
  build/local/build/src/dxmt/libdxmt.a build/local/build/src/util/libutil.a \
  build/local/build/src/winemetal/winemetal.dll.a -ldxgi -ldxguid -lole32 \
  -o build/local/owned-d3d12-hdr-presenter.exe
WINEPREFIX="$PWD/build/local/hdr-presenter-prefix-new" \
  WINEDLLPATH="$PWD/build/local/hdr-metadata-runtime-02/lib/wine" \
  WINEDLLOVERRIDES=winemenubuilder.exe=d WINEDEBUG=+loaddll \
  MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build/local/hdr-metadata-runtime-02/bin/wine "$PWD/build/local/owned-d3d12-hdr-presenter.exe"
WINEPREFIX="$PWD/build/local/hdr-presenter-prefix-new" \
  build/local/hdr-metadata-runtime-02/bin/wineserver -k
```

#### P1 후속 — D3D12 color-space 지원·적용 순서 (2026-09-29)

- `CheckColorSpaceSupport`와 `SetColorSpace1`은 동일한 DXGI→native mapping과 CGColorSpace 가용성을 사용한다. 지원 RGB는 full-range G22/P709, G10/P709, G2084/P2020, G22/P2020 네 종류다. 선택된 공간은 이후 조회에서도 PRESENT를 보고한다. EDR이 1이라는 이유만으로 HDR 공간을 거부하지 않는다. 미지원 enum의 support는 0이고 setter는 상태를 바꾸지 않고 `DXGI_ERROR_INVALID_CALL`을 반환한다. null support pointer는 `E_INVALIDARG`다.
- Setter는 검증한 native 색 공간을 pending 값으로 보관한다. 다음 실제 Present만 이를 소비하므로 setter 자체의 GPU flush나 매 frame 추가 EDR query가 없다. 설정·HDR metadata 변경·Present capture/index·resize를 기존 swapchain mutex로 직렬화한다. 초기 FLOAT swapchain의 G10, 다른 포맷의 G22 동작은 유지하되 resize가 명시한 색 공간을 덮어쓰지 않는다.
- D3D12의 `synchronizeLayerProperties(true)`는 PSO/layer를 바꾸기 전에 기존 frame의 encode와 실제 Metal command-buffer 완료를 기다린다. Queue는 `Rc<Presenter>`를 inflight에 넘겨 GPU retirement까지 보유한다. 완료 개수 watermark를 사용해 queue completion 스레드의 역순 실행을 처리하고, terminal 취소는 완료 대기도 해제한다. Resize도 같은 완료 경계를 사용한다. D3D11 전용 파일과 기존 D3D11 drain 동작은 변경하지 않았다.
- **공개 API 재현/검증:** 이전 `hdr-metadata-runtime-02`의 `colorspace-baseline-public-02`는 미지원 enum 2에 S_OK를 반환해 실패했다. 최종 `colorspace-runtime-01` + `colorspace-public-02`는 지원 4종, 미지원 23종, null pointer, setter 후 support 조회, 실패한 resize 및 정상 resize, queue rotation 뒤 blocked Present 중 PQ→G10 설정을 통과했다. 8-bit/RGB10A2/RGBA16Float의 13개 frame에서 각각 원본 3,072픽셀과 fence/Present를 검사했다. 원본 readback은 색 변환 출력의 증거가 아니다.
- **실제 native layer 관찰:** 임시 `nextDrawable` observer로 원래 API를 그대로 호출하면서 layer의 CGColorSpace/pixel format/EDR 값을 읽었다. `build/local/colorspace-native-02.stderr.log`의 14개 drawable에는 미지원 setter·실패/동일/변경 format resize 뒤 BT2020 유지, pending PQ frame 뒤 G10, FLOAT(format 115) resize 뒤 PQ 유지가 기록됐다. 공개 fixture stdout은 같은 이름의 `.stdout.log`다. Observer는 제품에 포함하지 않는다.
- **GPU 출력/완료 경계:** `owned-d3d12-colorspace-presenter.cpp`는 production Presenter/shader의 실제 drawable 11개 frame을 독립 PQ oracle과 비교했다. 관찰된 mid 값은 SDR `0.788086`, PQ `0.491211`, scRGB `0.039398`이고, 다음 설정 후에도 이미 수락한 frame은 이전 값을 유지했다. CPU encoding이 끝나도 native shared-event로 막은 GPU 작업이 완료되기 전에는 다음 색 공간 capture가 반환하지 않았다. `retirement_gate=1`로 PASS했다. 이 probe는 새 static library와 native ABI가 같은 `hdr-metadata-runtime-02`, 전용 `colorspace-presenter-probe-prefix-01`에서 Metal API/GPU Validation으로 실행했으며 별도 stdout 파일은 저장하지 않았다.
- **회귀:** 최종 runtime의 `colorspace-present-queues-01`과 `colorspace-hdr-metadata-01` 모두 Metal API/Shader Validation PASS. 빌드·install·새 runtime staging, runner diagnostics 44개도 통과했다. Public suite의 첫 실행은 fixture 자체는 PASS였으나 성공 marker `FAILED_RESIZE`를 `FAIL`로 오인한 runner 판정 문제였고, 오류 marker 경계를 수정한 뒤 `colorspace-public-02`가 PASS했다.
- **경계:** 색 공간이 바뀌는 다음 Present 및 resize는 이전 GPU 작업을 기다릴 수 있다. 그것들이 반환한 뒤에만 이전 queue의 미신호 gate를 해제하는 호출 순서는 보장하지 않는다. GPU completion/native layer 태그/drawable pixels를 검증한 것이며 Core Animation 합성 이후의 색 정확도나 물리적 scanout 완료, undocumented per-drawable 색 공간 latch를 증명한 것은 아니다. 다중 출력·EDR 전환·다른 GPU·Windows reference differential은 미검증이다.

공식 계약: [CheckColorSpaceSupport](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-checkcolorspacesupport), [SetColorSpace1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1), [CAMetalLayer.colorspace](https://developer.apple.com/documentation/quartzcore/cametallayer/colorspace).

재실행(새 출력/prefix 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-colorspace.cpp \
  -ld3d12 -ldxgi -o build/local/owned-d3d12-colorspace.exe
python3 scripts/test-owned-graphics.py --runtime build/local/colorspace-runtime-01 \
  --fixture build/local/owned-d3d12-colorspace.exe --suite colorspace --mode present \
  --out build/local/colorspace-public-new --validation --timeout 120
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  -Iinclude -Isrc/dxmt -Isrc/winemetal -Isrc/util -Ilibs \
  -D_WIN32_WINNT=0xa00 -DDXMT_PAGE_SIZE=4096 \
  graphics-translator/tests/owned-d3d12-colorspace-presenter.cpp \
  build/local/build/src/dxmt/libdxmt.a build/local/build/src/util/libutil.a \
  build/local/build/src/winemetal/winemetal.dll.a -ldxgi -ldxguid -lole32 \
  -o build/local/owned-d3d12-colorspace-presenter.exe
WINEPREFIX="$PWD/build/local/colorspace-presenter-prefix-new" \
  WINEDLLOVERRIDES=winemenubuilder.exe=d WINEDEBUG=+loaddll \
  MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build/local/colorspace-runtime-01/bin/wine "$PWD/build/local/owned-d3d12-colorspace-presenter.exe"
WINEPREFIX="$PWD/build/local/colorspace-presenter-prefix-new" \
  build/local/colorspace-runtime-01/bin/wineserver -k
```

#### P1 후속 — SO format capability (2026-09-29)

- `SO_BUFFER`를 1–4개 32-bit component의 FLOAT/SINT/UINT 12종에 보고한다. SO는 변환 없이 dword를 출력하므로 packed RGB32 texture의 native 지원 여부와 독립적이다. UNKNOWN/완전 typeless/16-bit/normalized/depth format에는 이 bit를 추가하지 않는다. VS→SO(stream 0) 범위와 공개 feature level은 유지한다.
- `owned-d3d12-so-format-support.cpp`, suite `so-format-support`는 format별 SO 출력과 그 버퍼를 해당 format의 IA 입력으로 다시 사용한 SO 출력을 검사한다. 12종 모두 원시 bit, 출력 byte counter와 앞뒤 guard가 일치했다. Float 음수, signed 음수, high-bit unsigned 값과 3-component stride도 포함한다. 8종 비대상 format은 SO bit가 없다.
- 이전 `colorspace-runtime-01`의 `build/local/so-format-support/baseline-06`에서 GPU 출력·IA 재입력은 모두 통과했지만 R32_FLOAT의 SO bit 누락으로 실패했다. 수정한 `so-format-runtime-01`의 `proof-02`는 12 GPU/12 capability/8 negative/12 IA 검사와 Metal API/Shader Validation을 통과했다. 기존 CPU probe의 SO 미지원 고정 assertion은 삭제했으며 실제 GPU 계약으로 대체했다. D3D11 전용 코드는 변경하지 않았다.

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-so-format-support.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -o build/local/so-format-support/owned-d3d12-so-format-support.exe
python3 scripts/test-owned-graphics.py --runtime build/local/so-format-runtime-01 \
  --fixture build/local/so-format-support/owned-d3d12-so-format-support.exe \
  --suite so-format-support --out build/local/so-format-support/proof-new --validation --timeout 180
```

#### P1 후속 — Format·view 보고와 D24 copy (2026-09-29)

- `MIP`를 filterability와 분리하고 integer·typeless·depth mip storage를 보고한다. DXGI bit-layout family의 fully typed cast, combined DSV와 plane SRV의 역할을 구분했다. Typed buffer의 native layout/access gate를 적용하고 sRGB UAV는 지원 보고·view 생성 모두 거부한다. SO의 12종 지원 범위는 유지했다.
- Packed 4:2:2 두 format은 이 시점에는 미지원으로 거부했다. 이후 `expanded-formats`의 RGBA8 pair backing으로 구현·검증했다(위 RGB32·packed 4:2:2 행). 독립적인 footprint query는 2×1 pixel/4-byte 계산과 짝수 resource width 조건을 따른다.
- **실제 D24 data corruption 수정:** 이전 runtime은 `.25` depth clear의 readback을 packed `0x00400000` 대신 float bit `0x3e800000`으로 반환했고, 반대 방향 plane upload는 호환되지 않는 native view를 만들었다. D24 low24 ↔ native float의 GPU 변환과 native depth/stencil plane blit를 연결했다. 정확한 나눗셈과 nearest-even 정수 변환으로 `0x7fffff` 경계 및 `.1/.3/.9` clear를 보존한다. D32S8 float plane은 변환하지 않는다.
- `format-runtime-02`의 `build/local/format-support/proof-05`: **98종 query**(texture2D 92, buffer 54, compressed 21)와 별도의 shader/readback 검증 통과. 실제 GPU oracle은 depth/stencil aspect, integer·typeless mip, RGBA8 linear/sRGB, R16 UINT→FLOAT, RGBA32 UINT→FLOAT, BC1 linear→sRGB 및 sRGB UAV sentinel이다. 98종의 모든 연산을 GPU로 검증했다는 뜻은 아니다. Native cast 13조합 생성도 별도 로그로 보존했다.
- 같은 runtime의 `build/local/d24-plane-runner-03`: typed/typeless D24·D32S8 4종, 독립 clear/upload·SRV read·texture copy, 두 row/padding·placed offset·mip/array·low24 경계·guard 통과. 유효한 전체 depth subresource copy만 사용하며 금지된 depth subrectangle을 정상 계약으로 시험하지 않는다.
- 기존 `resources`, `copy-resolve`, `capabilities`는 `build/local/format-integration/*-01`에서 Metal API/Shader Validation을 통과했다. Runner diagnostics 44개도 통과했다. 순수 bit-copy helper test는 삭제하고 공개 API/GPU 회귀로 대체했다. D3D11 전용 변경은 없으며, 공용 Metal 파일의 추가 kernel은 D3D12 D24 copy만 호출한다.
- 세부 정책과 fixture 실행 recipe: [CAPABILITIES.ko.md](CAPABILITIES.ko.md). 다른 GPU/OS, Windows reference differential 및 실제 게임 결과는 별도 검증 대상이다. Sample-count와 single-plane depth 역할의 후속 결과는 아래에 기록한다.

#### P1 후속 — Sample-count 조회·생성·실행 정합성 (2026-09-29)

- Quality query, committed/placed 생성과 null-output probe, allocation-info, graphics PSO가 공통 count·quality·native layout·effective backing 판정을 사용한다. BC MSAA, MSAA UAV, RGB32 texture/RTV PSO, 3/16/32/UINT_MAX count를 native 호출 전에 거부한다. RGB32 IA/SO 지원은 유지한다.
- 1×는 ordinary texture layout과 quality 0, MSAA는 software/native/format 모두 지원하는 2×/4×/8×와 quality 0 또는 standard pattern(`0xffffffff`)을 허용한다. Center pattern(`0xfffffffe`)은 미지원으로 거부한다. Query의 Flags를 보존하고 tiled query는 tier와 일치하게 0 level을 반환한다. Typeless depth resource는 실제 flags로 고른 Depth16/Depth32 backing을 사용한다.
- `R32_UINT` MSAA 보고는 Apple9 이상에 한정해 D3D12-local로 보정했다. Integer/depth의 region MAX/MIN 실행을 default `ResolveSubresource` 지원으로 혼동하지 않으며 `MULTISAMPLE_RESOLVE`는 올리지 않는다.
- Typed D16/D32/D24S8/D32S8은 shader view가 아니라 DSV 역할로 판정한다. Depth target에도 MSAA render-target bit를 적용하고, MSAA load는 shader view에만 적용한다. R16_UNORM/R32_FLOAT와 depth plane alias의 comparison 지원을 별도로 판정한다. `build/local/format-support/single-depth-after-02`에서 4종 typed DSV/4종 alias 보고와 유효한 typeless depth resource의 Load·SampleCmp 결과를 검증했다. Invalid descriptor를 shader에 bind하는 undefined/invalid 사용은 성공 fixture에서 제외했다.
- **최종 결과:** `build/local/msaa-capability-runtime-02` + `build/local/msaa-capability-probe-01/runner-msaa-final-01`에서 **45 quality query, 15 거부 descriptor 조합, 21 GPU case PASS**. GPU case는 2×/4×/8×의 RGBA8 UNORM/typeless, R32_UINT, typed/typeless D32, D32S8 및 standard-pattern coverage다. 실제 sample별 readback과 region resolve를 확인했고 Metal API/Shader Validation 오류가 없다.
- 이전 runtime의 BC count4 false-positive와 count1 RGB32 RTV PSO 생성 성공을 각각 baseline과 `runner-msaa-rgb32-before`에서 재현했다. 수정 후에는 GPU native assertion에 맡기지 않고 API preflight에서 실패한다.
- Shared resource preflight 및 D24 방향별 lazy PSO의 회귀는 `build/local/format-integration/{depth,resources,copy-resolve}-final-01`에서 통과했다. 전체 Ninja/install, fresh staging과 runner diagnostics 44개도 통과했다. 최신 build/install 로그는 같은 디렉터리의 `build-msaa-02.log`, `install-msaa-02.log`다. D3D11 전용 소스와 shared inspector의 D3D11 정책은 바꾸지 않았다.
- 지원·증거 경계: M5 Pro/macOS 27의 명시한 조합이며 center pattern, 16× 이상, 모든 format/dimension 조합과 다른 GPU/OS를 지원·검증 완료로 승격하지 않는다. 이 묶음의 완료가 P1 전체 완료는 아니다.

#### P1-C — Pipeline Statistics 구현

M5 Pro는 Metal statistic counter set을 제공하지 않으므로 native counter나 CPU draw 인수 추정값으로 대체하지 않는다. Translator/compiler instrumentation으로 11개 `UINT64` 필드(88 byte)를 집계하며, **공개 `PIPELINE_STATISTICS` heap 생성과 Begin/End/Resolve를 지원한다.**

**MSAA 실행 수정:** 이전 native 경로는 4×4·4× MSAA에서 pixel-frequency PS의 독립 UAV가 16 대신 64를 반환했다. 새 경로는 compute VS capture → primitive 조립·bounds → 8×8 tile의 입력/coverage capture → 1× app PS → 원래 geometry의 sample별 출력 replay다. VS와 app PS를 결과 공유를 위해 중복 실행하지 않는다. Native fragment가 나눠 보고하는 geometric coverage는 atomic OR로 합치며 centroid는 capture 완료 후 규격의 full/partial/helper 순서로 선택한다.

- **Query 계약:** DIRECT list의 Begin/End/Resolve. 기록별 GPU 누적값을 첫 Begin에서 초기화하고, slot별 Begin snapshot과 End 차이를 저장한다. 여러 heap/slot의 겹친 범위, slot 재사용, ClearState, command list 재실행, bundle의 부모 query, 서로 다른 queue의 격리를 지원한다. Resolve는 8-byte 정렬·88-byte stride이며 query/resource/device/range를 검사한다. 실행 task가 allocator·query·결과 자원을 GPU retirement까지 보유한다.
- **IA·clipper:** 실제 VS epilogue가 position과 packed clip/cull distance를 primitive-vertex scratch에 한 번 capture하고, draw 뒤 compute가 list/strip/restart·instance를 조립한다. IAVertices는 cut을 제외한 소비 position, IAPrimitives는 완성 primitive를 센다. CInvocations는 rasterization이 켜진 완성 primitive 수다. CPrimitives는 w>0, depth clip이 켜졌을 때 0≤z≤w, clip/cull distance를 반영한 결과다(삼각형 n각형→n−2, line/point 0/1). x/y는 규격이 허용하는 무한 guard band를 사용하며 backface/zero-area 제거는 이 값에 적용하지 않는다. NaN, 허용된 xyz Infinity discard, w=0 경계를 safe FP로 처리한다.
- **Shader invocation:** DXBC/DXIL VS·PS·CS entry에 전용 buffer slot 25의 counter를 삽입한다. PS discard 전에 세며 early-depth rejection은 실행되지 않은 entry이므로 제외된다. 32-bit low atomic과 overflow carry로 64-bit modulo counter를 구현한다. VS를 재실행하거나 IA position 수로 VS invocation을 대신하지 않는다. Strip에서는 GPU 재사용 여부에 따라 실제 VS invocation이 IA position 수보다 클 수 있다(FS §5.3.9/§20.4.7). DXIL의 `dx.op.sampleIndex.i32`를 AIR sample ID에 연결하고 register가 없는 intrinsic-only signature의 잘못된 초기화를 제거했다. DXIL ShaderFlags의 ForceEarlyDepthStencil도 AIR에 보존한다.
- **실제 실행 기준:** predication으로 건너뛴 draw/dispatch와 GPU count로 제외된 indirect command는 0이다. ExecuteIndirect의 root UAV/constant 변경과 indexed draw를 포함한다. SO capture/overflow/query와 같은 draw에서 통계를 함께 구하며 SO overflow는 raster 통계를 줄이지 않는다.
- **MSAA depth·interpolation:** pixel-frequency·sample-frequency·PS 없는 depth draw 모두 동일한 native gather에서 unclamped center/sample Z를 만든다. 원래 DSV format·bias·viewport를 보존하고 각 draw의 저장된 Z를 PS 입력과 default depth test가 공유한다. OM 출력에서만 viewport 범위로 clamp하며 early DS는 app PS 이전에 한 번, late DS는 출력 시 한 번 수행한다. D16/D32 bias, pixel/sample/no-PS 사이의 `DepthEqual`, stencil·occlusion, mask 0/부분 mask, viewport 밖 PS 입력 Z를 검증했다. Native finite pull 값을 capture해 동적 Sample/Snapped/Centroid 평가를 보존하며 helper 값은 derivative anchor와 claim 이전 quad shuffle로 전달한다.
- **PSO/cache·비용:** query counter variant는 query 구간에서만 lazy 생성한다. MSAA 실행 variant는 query 유무와 무관하며 warm CachedPSO에서도 원 shader로 생성한다. Primitive별 capture·encoder pass·scratch와 GPU bounds readback 비용이 있고, indirect graphics는 GPU 인수 preflight로 실행 시점 count/root/VB/IB를 확정한다. Submission worker는 필요한 GPU 완료를 기다리지만 API caller를 그 완료까지 막지 않는다. MSAA shader variant는 AIR 2.8/macOS 26 target이며 실제 실행 증거는 M5 Pro/macOS 27이다. 성능 향상이나 다른 OS/GPU 지원을 이 검증으로 주장하지 않는다.
- **지원 경계:** 현재 VS/PS/CS stage와 point/line/triangle list·strip 경로를 대상으로 한다. GS/HS/DS·adjacency/patch stage/topology 지원을 이번 query 구현이 추가하지 않는다. GSInvocations/GSPrimitives/HSInvocations/DSInvocations는 해당 stage가 없는 유효 PSO에서 0이며, stage 자체의 미지원 PSO를 허용한 뒤 가짜 0을 반환하지 않는다. PIPELINE_STATISTICS1/mesh 통계는 추가하지 않았다.
- **증거:** permanent fixture `graphics-translator/tests/owned-d3d12-pipeline-statistics.cpp`, suite `pipeline-statistics`. 11개 field와 독립 VS/PS/CS UAV oracle, RTV readback, resolve sentinel을 비교한다. GPU carry probe는 low overflow와 전체 64-bit wraparound 및 다른 10개 counter 보존을 통과했다(`build/local/pipeline-statistics/carry-probe/gpu.log`). Windows reference differential, 다른 GPU, 실제 게임의 정확성·성능은 검증하지 않았다.

최종 MSAA 통합 증거는 `build/local/msaa-implementation/runtime-integration-23`의 새 runtime·전용 prefix에서 수집했다.

| 검증 | 실제 결과 | 증거 |
|---|---|---|
| 전체 `pipeline-statistics` | **DXBC 168 + DXIL 178 = 346 GPU case**, 26 validation case PASS. Metal API/Shader Validation 오류 없음, owned native compiler 로딩 확인 | `build/local/msaa-implementation/proof-msaa-acceptance-23` |
| 원래 8×8·4× MSAA 재현 | 35 covered pixels, 98 covered samples, **UAV atomic 35회**. Counter 보정이 아니라 실제 app shader 실행을 검증 | `build/local/msaa-implementation/proof-original8x8-23-transcript.txt` (최종 runtime 실행 출력 보존) |
| 8× MSAA 경계 | DXBC/DXIL 각각 35 pixels, 189 samples, atomic 35회, sample별 색. DXIL sample positions/count 포함 | 전체 fixture의 `msaa_8x_boundary`와 8× intrinsic case |
| Indexed `ExecuteIndirect` mutation | Close 이후 변경한 count 1/2·root constant/UAV·VB/IB를 사용하고 sparse slot 3, VS/PS UAV 횟수와 모든 sample 색 PASS | `build/local/msaa-indirect-fixture/proof-stage23` |
| 기존 GPU 회귀 | `stream-output`, `indexed-restart`, `predication-indirect`, `queries` 모두 Metal API/Shader Validation PASS | `build/local/msaa-implementation/proof-{stream-output,indexed-restart,predication-indirect,queries}-23` |

최종 전체 Ninja/install 로그는 `build/local/msaa-implementation/full-build-23.log`와 `install-23.log`다. CPU tooling regression은 build 16개, staging 21개, diagnostics 44개를 통과했다. 이전 실패·보류 증거(`build/local/pipeline-statistics/proof-stage-only-15`, `proof-dxil-boundaries-04`, `proof-queries-gated-01`)는 수정 전 상태이며 현재 공개 gate를 나타내지 않는다.

공식 계약: [D3D12_QUERY_DATA_PIPELINE_STATISTICS](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_query_data_pipeline_statistics), Apple [MTLCommonCounterSet.statistic](https://developer.apple.com/documentation/metal/mtlcommoncounterset/statistic).

#### P1-D — VS→Stream Output / SO statistics 구현

Stream Output는 `SOSetTargets` 하나만 구현해서 끝낼 수 없다. PSO의 SO declaration, shader lowering/instrumentation, target binding/offset, filled-size counter, overflow와 `SO_STATISTICS_STREAM0..3`가 하나의 계약이다. P1-C의 나머지 필드보다 먼저 compiler/runtime/query를 함께 변경하는 독립 작업으로 진행했다. 이후 2026-09-29에 12종 32-bit component format의 GPU capture·IA 재입력을 검증하고 `SO_BUFFER` capability도 보고한다(위 SO format capability 결과 참조).

**P1-D 결과 (2026-09-28).** 아래 1–3단계로 GS 없는 VS→SO(stream 0)의 `DrawInstanced`/`DrawIndexedInstanced`를 구현했다. 설계 근거는 D3D11.3 FS ch.14(SO), 8.4/8.12–8.16(IA), §5.3.9·§20.4.7(VS 실행 횟수)이다.

- Compiler ABI: `src/airconv/airconv_public.h`의 `SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE`(인자 type 10), vertex buffer binding 26(app index)·27(`SM50_SO_CAPTURE_CONTROL`, 32 B)·28(scratch). DXIL(`dxil_signature.cpp`)과 DXBC(`dxbc_converter.cpp`) 모두 구현했고, D3D11의 `SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT` 동작은 바꾸지 않았다. DXIL VS도 `SM50_SHADER_GS_PASS_THROUGH.RasterizationDisabled`를 따라 void로 만든다(Metal은 rasterization을 끈 pipeline에서 값을 반환하는 VS를 거부한다).
- 리뷰 반영(ReviewAll High 4·Medium 2): list topology에는 strip cut을 적용하지 않는다. indexed SO draw를 생성한 position buffer로 바꿔 Metal의 고정 restart와 assembly 규칙을 맞췄다. `StartComponent`를 PSO 검증과 compiler 모두 element 첫 성분 기준으로 해석한다. 내부 한계를 overflow로 위장하지 않고 `E_OUTOFMEMORY`로 알린다. 같은 vertex의 중복 VS 실행은 claim word로 record 하나만 남긴다. `ALLOW_STREAM_OUTPUT` 누락은 `E_INVALIDARG`다.
- 남은 경계:
  - **P1 당시 경계:** Adjacency/patch topology, GS/HS/DS와 SO 조합은 recording/PSO `E_NOTIMPL`이었다. GS/DS SO의 현재 후속 결과는 P3 절에 있다. SO PSO의 `ExecuteIndirect` DRAW/DRAW_INDEXED는 아래 추가 결과에서 구현했다.
  - Claim을 못 얻은 record의 primitive는 쓰지 않으며, completion에서 아래 terminal device 오류 경로로 처리한다. 잘못된 SO 결과를 정상 완료로 공개하지 않는다.
  - Indexed SO draw는 app index마다 VS가 한 번씩 실행돼 post-transform 재사용이 없다(규격 no-cache 상한 안). 성능은 측정하지 않았다.
  - SO type 10의 WoW64 인자 변환과 실제 32-bit compiler 호출은 아래 추가 결과에서 검증했다. 전체 32-bit SO GPU draw와 type 11의 WoW64 GPU 실행은 2026-09-30 WoW64 결과에서 검증했다.
  - **당시 경계, 현재 수정:** 이전 `dxil-ia` 회귀는 `build/legacy/...` 바이너리(`docs/provenance/gpu-dxil-ia.json`)에 의존했다(`build/local/p1d/reg/proof-dxil-ia-03`). 지금은 `graphics-translator/tests/owned-d3d12-dxil-ia.cpp`와 `graphics-translator/tests/dxil/dxil-ia-{vs,ps}.hlsl`·새 `.dxil`을 복원하여 소스 빌드 fixture로 검증한다.
- 회귀: `runtime-03`에서 pipeline-cache, queries, bundles, predication-indirect, dxil, atomic-copy, `smoke --mode present`, `present-queues --mode present`가 Metal validation 오류 없이 통과했다(`build/local/p1d/reg3/proof-*`). CPU 스크립트 build/staging/diagnostics도 통과했다.

P1 당시 진행 상황(1단계, runtime plumbing): PSO는 `D3D12_STREAM_OUTPUT_DESC`를 FS ch.14 규칙으로 검증해(`E_INVALIDARG`; GS/HS/DS 조합은 `E_NOTIMPL`) owned layout과 capture record layout을 저장하고, VS 컴파일 인자에 `SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE`를 넣는다. `RasterizedStream = NO_RASTERIZED_STREAM`이면 VS를 void로 만들고 rasterization을 끄며 fragment 단계도 두지 않는다. Pipeline cache key에는 SO declaration/stride/RasterizedStream이 들어가고 blob version은 2다. `SOSetTargets`는 slot 범위, VA 해석, 크기, 4-byte 정렬된 filled-size 위치를 검증한 뒤 allocation을 보존한다. `SO_STATISTICS` heap은 16-byte 결과와 Begin snapshot을 갖고, 결과는 각 recording의 stream counter(`SOStreamCounterBuffer`) 차이로 계산한다.

P1 당시 진행 상황(2단계, draw-time capture·assembly): SO PSO의 `DrawInstanced`/`DrawIndexedInstanced`는 render pass 안에서 VS epilogue capture(indices=vertex buffer 26, control=27, scratch=28)를 실행하고, render pass가 끝나면 draw마다 단일 thread `so_assemble` compute를 API 순서대로 돌린다. Record key는 0-based IA position k이고, record는 `VertexCountPerInstance × InstanceCount`개다. Scratch는 recording 시점에 크기가 정해지며(record 뒤에 record당 claim word 1개), CPU 대기는 없다. `UINT32_MAX` record나 `maxBufferLength`를 넘으면 `E_OUTOFMEMORY`를 기록한다. Indexed SO draw는 app index buffer 대신 생성한 uint32 position buffer(`positions[k] = k`, D3D12 strip cut이 적용되는 위치만 `0xFFFFFFFF`)로 Metal indexed draw를 낸다. VS가 app index를 직접 읽어 `SV_VertexID` = index 값(BaseVertexLocation 제외, FS 8.16), vertex fetch = index + BaseVertexLocation으로 쓴다. 그래서 Metal restart와 assembly cut 규칙이 같다. VS 실행 수는 IA position 수로, D3D가 허용하는 no-cache 상한 안이다. 한 vertex가 여러 번 shade되면 claim word(cmpxchg 0→1)를 얻은 invocation만 record를 쓰므로 record 성분이 섞이지 않는다. Assembly가 claim 없는 record를 만나면 그 primitive를 건너뛰고 status word로 세며, completion에서 로그와 함께 `DXGI_ERROR_DRIVER_INTERNAL_ERROR`를 device에 보존하고 대기 중인 fence를 종료한다. Assembly 규칙: strip은 list로 풀고, 홀수 strip 삼각형은 `(i, i+2, i+1)`로 leading vertex와 winding을 보존한다(MS Triangle Strips 문서, FS 8.14, VK_EXT_provoking_vertex의 D3D11 SO 순서 요구). `IBStripCutValue`에 맞는 cut은 LINESTRIP/TRIANGLESTRIP에만 적용하고(list에서는 cut 값도 일반 index), 미완성 primitive는 버린다(FS 8.12-8.13). Primitive는 참조된 모든 slot에 들어갈 때만 쓰고, overflow 뒤에는 쓰지 않으며 needed만 증가시킨다. 참조 slot이 미바인딩이면 full로, 참조하지 않는 slot이 bound돼 있으면 무시한다(FS 14.5-14.6). 끝나면 filled size를 write back하고 stream 0 counter에 누적한다. Predication은 기존 predicate snapshot을 assembly에도 적용한다. PSO 검증: root signature(명시 또는 embedded)에 `ALLOW_STREAM_OUTPUT`이 없으면 `E_INVALIDARG`이고, `StartComponent`는 packed output register의 `.x`가 아니라 element 첫 성분 기준이다(DXBC/DXIL 동일). 현재 adjacency/patch topology는 recording `E_NOTIMPL`이다. SO PSO의 `ExecuteIndirect`는 아래 추가 결과에서 구현했다. 일반 non-SO indexed restart의 과거 가정은 아래 추가 결과에서 정정했고, 불일치 strip만 application index pulling으로 수정했다. 증거: `graphics-translator/tests/owned-d3d12-stream-output.cpp`, suite `stream-output`(총 87 case, 그중 GPU capture 28개: list/strip/홀수 strip 순서, indexed 반복/restart, instancing, 다중 slot·gap·stride, 두 draw·두 list append, exact fit, overflow, 미바인딩 slot, NO_RASTERIZED vs rasterized RT, non-indexed VS invocation이 SO 없는 동일 draw와 같고 indexed는 no-cache 상한 안, list/DISABLED에서 cut 값은 일반 vertex, sparse R16/R32 index, R32 strip cut, record 단일 invocation(ticket), packed DXIL semantic, `ALLOW_STREAM_OUTPUT` 누락, predication on/off, SO_STATISTICS 0..3), `build/local/p1d/runtime-03`, `build/local/p1d/proof-11`.

### P1 추가 결과 — SO ExecuteIndirect

- VS-only SO PSO의 DRAW/DRAW_INDEXED indirect 명령을 지원한다. GPU argument/count buffer, MaxCommandCount clamp, predication snapshot, command별 root/VB/IB 갱신과 API 후 상태 reset을 유지한다. 이전 `Close=E_NOTIMPL`은 실제 기존 runtime에서 두 draw 형식 모두 재현했다(`build/local/so-indirect/baseline-so-e-notimpl.txt`).
- `d3d12_command_list.cpp`는 SO indirect를 독립 render pass에 기록하고 SO target·layout과 root/VB 초기 상태를 보존한다. Signature는 packed draw/IBV byte offset을 저장한다. `dxmt_command.metal`의 준비 kernel은 GPU가 정한 실행 count와 compact draw/IB 정보를 읽는다. Count/predicate가 있으면 먼저 count만 읽고, 실행되는 명령 수만큼만 readback을 할당한다.
- Queue worker가 producer submission의 실제 retirement를 기다린 뒤 실행별 capture scratch·claim·index positions·control·ICB·root/VB 상태를 만든다. Allocator의 녹화 상태는 수정하지 않고 새 메모리는 GPU completion까지 보유한다. VS는 기존 draw 안에서 한 번만 실행하며 SO record는 기존 epilogue로 capture한다. Indexed draw는 생성한 IA positions 위에서 실행하고 application index/BaseVertex는 VS가 읽는다. Assembly는 명령 순서대로 append·overflow·needed 통계를 반영한다.
- 성능 경계: SO indirect에는 GPU→CPU readback과 queue-worker 대기가 있다. GPU count/predicate가 있으면 count와 draw 정보의 두 준비 단계를 사용한다(count=0이면 두 번째 생략). API caller는 비차단이다. 완전 GPU-only indirect 구현이나 성능 개선을 주장하지 않는다. 실제 record 수가 uint32/Metal buffer 한계를 넘거나 실행 메모리를 할당하지 못하면 device 오류로 전파하며 SO output을 임의로 잘라 성공 처리하지 않는다.
- 리뷰 반영: PSO가 읽는 VB slot뿐 아니라 signature가 갱신하는 slot까지 table을 할당한다(미사용 slot 31 포함). Zero-instance indexed 명령은 큰 index count 때문에 positions 크기 제한이나 IB 접근을 수행하지 않으며 SO ICB resolver도 no-work 명령을 생략한다. Completion용 SO owner 저장 공간도 GPU setup 전에 allocation-error 처리 범위 안에서 확보한다. Zero-instance UINT_MAX case는 이 호스트의 수정 전 runtime-03에서도 통과했으므로 이 경계의 failing-before 증거로 주장하지 않는다. 강제 OOM 실행은 검증하지 않았다.
- 최종 검증: 전체 Ninja build/install 후 새 `build/local/so-indirect/runtime-05`, Metal API/shader validation 아래 `proof-stream-output-05` PASS. **총 102 case 중 GPU capture 43개**이며, 기존 28개에 SO indirect 15개를 추가했다. GPU가 쓴 인수/count(0·clamp), 두 draw 형식, predication, append/overflow·미바인딩 needed 통계, packed DXIL, 혼합 R16/R32 및 root/VB/IB 갱신·slot 31, closed-list 인수 변경 재실행, zero-instance 큰 count를 실제 readback으로 검사한다.
- 관련 회귀: `build/local/so-indirect/proof-indexed-restart-01`(runtime-01, DXBC/DXIL 총 62 GPU case), `proof-predication-indirect-03`(runtime-03) validation PASS. `python3 scripts/test-owned-graphics-diagnostics.py` 42 tests PASS. 두 reviewer의 지적을 수정하고 좁은 재검토에서 남은 지적이 없음을 확인했다. 전체 D3D12 conformance, 실제 게임 성능, 32-bit SO GPU 실행은 이 결과에 포함하지 않는다.

### P1 추가 결과 — SO 오류 전파와 WoW64 capture 인자

- SO assembly의 missing-claim status를 순차 completion 스레드에서 검사하고 최초 `DXGI_ERROR_DRIVER_INTERNAL_ERROR`를 device에 보존한다. `GetDeviceRemovedReason`은 이 원인을, 이후 queue/fence/resource 생성·Map·Present의 오류 반환 경로는 `DXGI_ERROR_DEVICE_REMOVED`를 노출한다. 이미 등록된 fence 대기는 terminal `UINT64_MAX`로 풀고 늦은 GPU/CPU signal이 이를 되돌리지 못하게 한다.
- Fence의 native event probe는 SO 검증 전에 정상 완료를 공개할 수 있어 제거했다. Queue의 앞선 submission 검증을 마친 completion 스레드만 GPU fence 값을 공개하며, consumer worker는 latch를 기다린다. Device 오류로 폐기하는 Present는 shared presentation-order event를 `UINT64_MAX`로 signal하고 frame-latency semaphore credit도 반환한다. 이미 제출된 뒤 frame의 GPU wait와 종료 대기가 남지 않게 한다.
- `winemetal_unix.c`는 SO type 10의 32-bit element 배열과 semantic name을 소유하는 native 저장소로 변환한다. 다섯 compile thunk가 변환 실패를 compiler 오류로 전달하고 부분 할당을 해제한다. Null compiler error에서도 오류 메시지 조회가 안전하다. 32-bit DLL 빌드에서 드러난 RT export-name 크기 포인터는 public uint64/transport size_t ABI를 유지하는 local 변환으로 수정했다. 이 RT 함수의 실제 호출은 검증하지 않았다.
- 실제 GPU 실패 재현: `build/local/so-integrity/probe-baseline-02`는 missing record status=3인데 device reason은 S_OK였다. 수정 `runtime-02`의 `probe-fixed-02`는 같은 status에서 reason `0x887a0020`, 세 fence의 `UINT64_MAX`, 사전 등록 이벤트 해제, 이후 queue Signal/Wait·event 등록의 `0x887a0005`를 확인했다. 정상 SO는 3 vertices/48 bytes였다.
- 다중 queue Present 실패 재현: `probe-present-before-02`는 frame N이 future fence 뒤에 있고 N+1이 GPU에서 N을 기다릴 때 독립 queue의 SO 오류 후 latency wait가 timeout(258)됐다. `probe-present-fixed-01`은 latency wait 성공, 이후 Present의 device-removed 오류, 정상 queue/swapchain 해제와 exit 0을 확인했다. 두 GPU probe는 Metal API/shader validation을 켰으며 production fault-injection 설정을 추가하지 않았다. 임시 probe는 제거하고 로그를 남겼다.
- 실제 Wine WoW64 compiler 경로: 기존 native 모듈은 존재하지 않는 semantic을 묵인했지만 수정 후 이름을 포함한 오류로 거부했다. 유효한 capture element 두 개의 컴파일, 두 번째 element의 범위 오류, null-error 메시지 조회도 확인했다(`build/local/so-integrity/wow64-so-capture-proof.txt`). 전체 32-bit D3D12 SO GPU draw나 강제 OOM 실행을 검증한 것은 아니다.
- 빌드·회귀: 전체 Ninja build/install 및 i686 winemetal.dll build, CPU `queue-order-scheduler.test.cpp` 통과. `runtime-01`의 `proof-stream-output-01`(총 87 case, GPU capture 28), `proof-indexed-restart-01`(DXBC/DXIL 총 62 GPU case), 최종 `runtime-02`의 `proof-fence-events-02`와 `proof-present-queues-02`가 validation PASS다. 경로는 모두 `build/local/so-integrity/` 아래다.

### P1 추가 결과 — 일반 non-SO indexed primitive restart (2026-09-28)

Apple M5 Pro의 native Metal direct draw와 ICB probe에서 list topology는 R16/R32 index 최댓값을 **restart하지 않았다**. 과거의 “Metal은 모든 indexed draw에서 최댓값을 restart한다, 따라서 모든 list/draw를 다시 써야 한다”는 가정은 **오류로 정정한다**. 문제는 strip의 `IBStripCutValue`가 disabled이거나 현재 R16/R32 index format과 cut 값이 불일치할 때 Metal의 고정 restart와 D3D12 계약이 달라지는 경로다. 일치하는 cut-format strip과 list는 direct/ICB 모두 native indexed draw를 유지한다.

- 불일치 strip만 nonindexed Metal draw로 내리고 VS에서 application index를 읽는다. VS의 `SV_VertexID`는 app index+base vertex, input attribute fetch도 같은 값을 사용한다. Index buffer는 GPU에서 원본을 그대로 읽으며, 새 generated IB·prepass·CPU readback은 없다. Predicated direct draw, GPU argument/count `ExecuteIndirect`, command별 IB view·format/root/VB 갱신도 처리한다. 기존 SO indexed capture/assembly 경로는 그대로다.
- Compiler: `src/airconv/airconv_public.h`의 header-only `SM50_SHADER_D3D12_INDEX_PULL`(type 11), vertex bindings 26=`VERTEX_INDICES`, 27=`VERTEX_INDEX_CONTROL`(generic 16 B). SO type 10의 control은 기존 32 B 그대로이며 두 mode는 상호 배타적이다. DXBC/DXIL의 VS index·input lowering은 공통 compiler helper를 사용한다. Type 11의 `sm50_compilation_argument32_convert`도 추가했으나 WoW64 실제 실행은 미검증이다.
- Graphics PSO는 원본 VS source/root signature/input elements를 보존하고 normal/ICB 각각에 대해 index-pull variant를 lazy 생성한다. Variant별 immutable constants를 분리하여 normal/ICB PSO와 혼용하지 않는다. Cold/warm pipeline cache 회귀는 통과했지만 **pull variant 자체는 serialized cached blob에 들어가지 않는다**. 영향을 받는 첫 draw에서 컴파일된다.
- 실패 재현: 기존 runtime `build/local/p1d/indexed-restart-before-01`에서 R16 line strip cut disabled의 pixel 3·5가 expected 1 / actual 0; `build/local/p1d/indexed-restart-mismatch-before-01`에서 R32와 cut FFFF의 불일치. 수정 뒤 `build/local/indexed-restart/proof-indexed-restart-02`은 `runtime-02`에서 초기 28×DXBC/DXIL=56 case PASS. 최종 `build/local/indexed-restart/proof-indexed-restart-03`은 `runtime-02`, Metal validation 아래 DXBC 31 + DXIL 31 = **62 GPU case PASS**. Direct/list/strip, cut disabled/mismatch/match, base vertex·start instance, predication, GPU count/arguments, simple strip R16/R32, command별 R16→R32→R16 ICB native→pull→native와 IB view offset 전환을 포함한다. `runtime-02`는 이전 `runtime-01` 이후 generated resolver scope만 수정한 production build이며 최종 matrix를 위한 추가 runtime 변경은 없다.
- 같은 `runtime-02`의 `build/local/indexed-restart/proof-predication-indirect-02`, `proof-bundles-02`, `proof-pipeline-cache-02`도 validation PASS. 최초 새 runtime의 `proof-dxil-ia-01`과 `proof-stream-output-01`도 PASS. `python3 scripts/test-owned-graphics-diagnostics.py` 42 tests PASS. 기존 SO fixture는 **총 87 case(그중 GPU capture 28개)**이며, 이번 indexed 62개는 별도 suite 결과다.

구현 소스는 `src/d3d12/d3d12_command_list.cpp`, `d3d12_command_signature.cpp`, `d3d12_command_allocator.cpp`, `d3d12_pipeline_graphics.cpp`, `src/airconv/airconv_public.h`, `dxbc_converter.cpp`, `dxil_signature.cpp` 및 공유 compiler helper다. 소스 fixture는 `graphics-translator/tests/owned-d3d12-indexed-restart.cpp`와 `graphics-translator/tests/dxil/indexed-restart.hlsl`·세 DXIL 입력이다. 복원한 `owned-d3d12-dxil-ia.cpp`는 기존 `compute.dxil vertex.dxil pixel.dxil` 다음에 재구성한 `dxil-ia-vs.hlsl`/`dxil-ia-ps.hlsl`에서 새로 빌드한 `dxil-ia-vs.dxil`/`dxil-ia-ps.dxil`을 받는다. 이전 runtime에 대해 소스 빌드 fixture를 `build/local/ia-restore/proof-source-built-01`에서 먼저 검증했다. Runner의 `dxil-ia` suite에는 이제 legacy binary dependency가 없다.

기존 Meson install 뒤 새 runtime/prefix/evidence 경로에서 재현한다(아래 runner가 fixture 옆에 shader 입력을 복사하고 fixture의 argv 순서대로 전달한다):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -municode -static \
  -Iinclude/native/directx graphics-translator/tests/owned-d3d12-dxil-ia.cpp \
  -ld3d12 -ldxgi -ldxguid -ld3dcompiler -lole32 -o build/local/dxil-ia.exe
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  -Iinclude/native/directx graphics-translator/tests/owned-d3d12-indexed-restart.cpp \
  -ld3d12 -ldxgi -ldxguid -ld3dcompiler -lole32 -o build/local/indexed-restart.exe
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --wine-source toolchains/wine-runtime \
  --wine-dest build/local/indexed-restart/runtime-reproduce --with-nvapi --with-nvngx
python3 scripts/test-owned-graphics.py --runtime build/local/indexed-restart/runtime-reproduce \
  --suite dxil-ia --fixture build/local/dxil-ia.exe \
  --out build/local/indexed-restart/proof-dxil-ia-reproduce --validation
python3 scripts/test-owned-graphics.py --runtime build/local/indexed-restart/runtime-reproduce \
  --suite indexed-restart --fixture build/local/indexed-restart.exe \
  --out build/local/indexed-restart/proof-indexed-restart-reproduce --validation
```

두 fixture의 argv 계약은 각각 `compute.dxil vertex.dxil pixel.dxil dxil-ia-vs.dxil dxil-ia-ps.dxil`, `indexed-restart.hlsl indexed-restart-procedural-vs.dxil indexed-restart-attribute-vs.dxil indexed-restart-ps.dxil`이다. 위 runtime/output은 **실행 전 없는 새 경로**로 바꿔야 한다. 검증은 M5 Pro/macOS 27의 x86_64 Wine 경로이며 다른 ABI/GPU에 대한 선언은 아니다.

### P1 추가 결과 — buffer UAV 사각형 clear와 RS 1.1 descriptor bounds

- `ClearUnorderedAccessViewUint/Float`: rect를 `[0, view width) × [0, view height)`로 클리핑한 뒤 RAW DWORD base 또는 typed native view의 alignment prefix를 한 번만 더한다. 빈 rect는 건너뛰며 predicated dispatch도 같은 클리핑된 크기를 사용한다. 기존 clear shader/ABI는 바꾸지 않았다.
- 수정 전 `FirstElement=8`인 RAW UAV의 rect clear가 뷰 밖 0번 DWORD를 `0x12340000`에서 `0xffffffff`로 덮는 것을 실제 GPU readback으로 재현했다(`proof-uav-before`).
- `predication-indirect`에 **60 GPU case**를 추가했다: RAW UINT, R32/RGBA32 UINT·FLOAT, FirstElement 8/72, 전체/부분 clear, predicate 없음/실행/건너뛰기. 256-byte native view 정렬, 37-element dispatch tail, 음수·넘치는·빈 rect, 전체 backing buffer의 prefix/suffix sentinel과 32-bit UINT 값 보존을 확인한다. FLOAT clear는 typed FLOAT view만 사용한다.
- RS 1.1의 `DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS`와 각 유효한 data flag 조합을 허용한다. `KEEP_BOUNDS | DESCRIPTORS_VOLATILE`, `DESCRIPTORS_VOLATILE | DATA_STATIC`, sampler의 data/KEEP_BOUNDS flag는 `E_INVALIDARG`다. Descriptor table을 bounds 없는 root descriptor로 승격하지 않는다.
- 기존 runtime에서는 유효한 KEEP_BOUNDS 조합 네 개가 `E_INVALIDARG`이고 금지된 `DESCRIPTORS_VOLATILE | DATA_STATIC`은 `S_OK`인 것을 재현했다(`proof-root-before-static`). `core-contracts`의 **22개 조합**은 정상 serialize → deserialize의 범위/flags/type 보존 → CreateRootSignature와 잘못된 조합 거부를 검증한다.
- `dxil` suite는 기존 frozen `null-descriptor.dxil`을 추가로 staging한다. KEEP_BOUNDS table을 실제 dispatch해 null CBV/SRV read와 null/OOB atomic 반환값 0, 16-byte UAV view 바깥·64-KiB allocation 안의 store/atomic 무효화, 전체 allocation sentinel 보존, 정상 table/root UAV write·read를 검증한다. 기존 RS 1.0 table, register-space, compute, graphics 경로도 유지한다.

최종 전체 Ninja build/install 후 새 `build/local/p1-contracts/runtime-01`에서 Metal validation으로 다음을 통과했다. 모든 evidence는 같은 `build/local/p1-contracts/` 아래에 있다.

| Suite | 증거 | 결과 |
|---|---|---|
| `predication-indirect` | `proof-uav-final` | 새 60 GPU case와 기존 clear/indirect 회귀 PASS |
| `core-contracts` | `proof-root-after` | 새 RS 1.1 22개 조합과 기존 기본 계약 PASS |
| `dxil` | `proof-dxil-after` | KEEP_BOUNDS GPU readback과 기존 compiler/binding/graphics 회귀 PASS |

`python3 scripts/test-owned-graphics-diagnostics.py` **42 tests PASS**. 빌드 로그는 `build-01.log`다. `core-contracts`와 `dxil` fixture는 llvm-mingw의 현행 기본 D3D12 header로 `-std=c++20 -O2 -municode -static` 빌드하며, 오래된 `include/native/directx`를 include 경로로 강제하지 않는다. Runner가 필요한 shader를 복사하므로 `--suite core-contracts|dxil|predication-indirect`, 해당 `--fixture`, 새 `--out`, `--validation`으로 재현한다.

검증 범위는 M5 Pro/macOS 27, x86_64 Wine의 격리 fixture다. Windows differential·다른 GPU/ABI·실제 게임 호환성 검증은 아니다. P1 전체 규격 완료나 지원 tier 상승을 뜻하지 않으며, 이후 공개한 `PIPELINE_STATISTICS`와 MSAA의 검증 범위는 위 P1-C에 별도로 기록했다.

## P3 완료 결과 (2026-10-01)

**P3의 지정 구현과 최종 통합 검증을 Apple M5 Pro/macOS 27의 x64·WoW64 Wine 범위에서 완료했다.** 아래 실행은 별도 표시한 API-only를 제외하고 Metal API/Shader Validation을 사용했다. 최종 공통 runtime-04의 affected suite 통과와 baseline+targeted reachable coverage를 구분하며, 한 snapshot의 전체 suite 통과·전체 D3D12/DXR conformance를 주장하지 않는다.

| 기능 | 현재 검증 범위·보고 값 | 보존된 증거 경로 |
|---|---|---|
| 최종 공통 runtime-04 | Manifest **363308d9269fa9a29140b2f7fdff0505185e0ec143d3f3f13b7eb9582bca0230**의 x64 RT/WG/HDR **3 suite**, WoW64 RT/WG/HDR/query **4 suite** 모두 exit0·API/Shader Validation 오류 없음·foreign backend 없음. RT 25 frozen 입력/contract13/AS16/RayQuery15/formats13, WG OPTIONS21 16-byte·tier10·9 case, HDR7-frame source readback/fence/Present 검증. 전용 prefix stop/wait/cleanup 완료 | `build/local/p3/integrate/runtime-04`, `final64-04/{raytracing,work-graphs,hdr-metadata}/result.json`, `final32-04/{raytracing,work-graphs,hdr-metadata,queries}/result.json`(같은 integrate 디렉터리) |
| x64 최종 reachable coverage | 계획 61 row 중 source가 있는 **60 reachable row 모두 PASS: 59 API/Shader+1 API-only**. Baseline과 enhanced-barrier/atomic 및 최종 common-cut proof의 합산이며 단일 snapshot 전체 suite 통과가 아니다. 기존 source-orphan standalone static-sampler 1 row는 미검증 | `build/local/p3/integrate/aggregate64-final.json` |
| WoW64 최종 reachable coverage | **58 reachable label 모두 PASS: 57 API/Shader+1 API-only**, cleanup 완료. Baseline+affected proof+최종 common-cut 합산이며 단일 snapshot 전체 suite 통과가 아니다. RT/WG 보류 없음. NGX 2 suite는 x64-only ABI, standalone static-sampler는 retained source 부재로 실행 불가 | `build/local/p3/integrate/integration32-final-evidence.json` |
| Feature level | **FL12_1**, 공개 max=0xc100, FL11_1/12_0/12_1 생성 성공·FL12_2 거절, strict capabilities runner PASS | `build/local/p3/integrate/runtime-03`, `build/local/p3/integrate/fl121-cap-01/result.json`·`stdout.log` |
| Conservative raster | **ConservativeRasterizationTier 1**. VS DXBC/DXIL, GS, HS/DS, TS+GS, MS, AS/MS 전체 **10 family**, clip/cull/view·indirect/SO/statistics와 4× depth extrapolation/clamp **1180 sample** 검증. 공개 tier=1·strict runner PASS | `build/local/p3/integrate/conservative-cap-01/result.json`·`stdout.log` |
| 선행 FL12_0 증거(역사적) | **FL12_0**. Logic op, 18 typed UAV load format, UAV-only ForcedSampleCount 1/4/8/16, TIR 16×, 4×/8× RTV의 forced 1×를 GPU 검증. Cap 조회 max=0xc000, FL11_1/12_0 생성 성공·당시 FL12_1 거절 | `build/local/p3/fl2/run-08`, `build/local/p3/fl2/cap-01` |
| DXR | **RaytracingTier 1.1**. 새 통합 runtime에서 `raytracing_tier=11`, 정확한 RayQuery 15 case, AS 16 case, contract/state-object 12 case, vertex format 13종 및 indirect·predication·root arguments·coverage 통과. Validation 오류·foreign backend load 없음. Generic-function collection export/linkage의 후속 x64/WoW64 paired proof는 아래에 별도로 기록하며 전체 DXR conformance 완료가 아님 | `build/local/p3/integrate/runtime-01`, `build/local/p3/integrate/dxr-cap-01/result.json`·`stdout.log`; 개별 증거 `build/local/p3/rqstages/run-08`, `build/local/p3/dxras-resume/run-09` |
| DXR ordinary-function collection(x64/WoW64) | Immutable WG/DXR namespace에서 renamed export·deferred external을 final pipeline 생성 시 연결, hidden internal closure·dependency flags·conflict·immutable root·helper resource metadata 검증. Ordinary function에는 shader identifier를 주지 않음. Upstream DXC `lib_6_6` consumer/helper/final frozen fixture에서 uint/float overload의 unmangled export group rename·mangled signature를 보존하고 float **5.25/7.25/9.25/11.25** bit pattern, reachable helper local-root incompatibility와 global-root conflict를 검증. GPU words **0x2320/0x2323/0x2326/0x2329**와 helper UAV writes, mixed RT-shader+ordinary collection의 renamed raygen만 import해 hidden closure를 유지하는 경로와 flags/hidden/missing/duplicate-global-root/incompatible-helper-local-root negative 계약, 기존 RayQuery15/AS16/formats13 유지. **같은 24 frozen DXIL 입력·typed-overload/root ABI의 x64/WoW64 paired PASS** | `build/local/p3/dxr-linkage/runtime-07`, `build/local/p3/dxr-linkage/after-09/result.json`·`stdout.log`, `build/local/p3/integrate/runtime-rt32-02`·`raytracing32-isolated-03/result.json`; 재생성 `graphics-translator/tests/dxil/build-raytracing-linkage.sh` |
| 후속 DXR named-root/resource binding(x64/WoW64) | Source/ExistingCollection의 embedded named-root alias·filter/old-name-hidden·function/root namespace collision·NumExports0 publication, mixed collection alias와 provider association 검증. Resource가 있는 closed rootless collection 거부; AddToStateObject의 같은 embedded root 재선언 및 기존 GPU oracle 유지. **Contract13/AS16/RayQuery15/formats13**, API/Shader Validation PASS. **같은 native snapshot의 최신 x64/WoW64 paired PASS** | `build/local/p3/dxr-linkage/runtime-13`, `build/local/p3/dxr-linkage/after-16/result.json`, `build/local/p3/integrate/raytracing32-isolated-04/result.json` |
| GS/TS+GS indirect | Command별 root constant·CBV/SRV/UAV·VB/IB 변경, GPU argument/count·predication과 직접 draw의 독립 oracle 비교. Geometry **DXBC 87/DXIL 87**, tessellation **166/166** 통과 | `build/local/p3/gsindirect-contract/geometry-10`, `build/local/p3/gsindirect-contract/tessellation-07` |
| GS SO stream 0–3·TS+GS SO/indirect | Stream별 정확한 bytes·filled size·written/needed·overflow·rasterized stream/NONE, GS SO indirect와 SO 통계의 predication 검증 | `build/local/p3/p2sostreams/geometry-04`·`so-03`, `build/local/p3/tsgsind/geometry-05`·`tessellation-05` |
| Early-Z MSAA statistics | GS·HS/DS·TS+GS·MS의 2×/4×/8× forced-early PSInvocations를 CPU oracle과 비교. Direct/indirect, GS indirect+SO 및 cut/adjacency strip indirect 검증. Mesh의 이 부분 통과는 MeshShaderTier 1 전체 증거가 아님 | `build/local/p3/p2last/geometry-final.log`·`tessellation-final.log`·`mesh-01.log` |
| DS-only SO | **Tessellation DXBC 166/DXIL 166**. Fractional tri/quad·isoline point/line, direct/indirect의 정확한 SO 순서·filled size·overflow·root/VB/IB 변경·no-raster 통과. No-raster에서는 conservative/VRS 설정을 켜도 C/PS=0·clear image 유지 | `build/local/p3/ds-so/proof-10/result.json`·`stdout.log` |
| Sampler feedback·view instancing | **SamplerFeedbackTier 1.0**, **83 case** oracle 통과. **ViewInstancingTier 1**, **23 case**: VS/GS/HS/DS/PS ViewID, view 위치·mask·4× MSAA·indirect·bundle·statistics 검증 | `build/local/p3/feedback3/run-08`, `build/local/p3/viewinst2/final-view-instancing` |
| Mesh 출력·cull | **공개 MeshShaderTier 1**. 필수 128-scalar(SV_Position 포함) vertex/primitive split, 124 독립 semantic, 32KiB output+28KiB app threadgroup, 1×/4× direct/GPU-count indirect/cache, wide coarse SV_Coverage·pull·ddx/ddy, 8-plane CullDistance의 triangle/line·zero/NaN·primitive cull 등 **34 case** 통과. 새 통합 runtime에서 공개 `tier=10`, exit 0·strict runner PASS·API/Shader validation 오류 없음 | `build/local/p3/integrate/runtime-02`, `build/local/p3/integrate/mesh-cap-01/result.json`·`stdout.log`; 앞선 cap=0 proof는 `build/local/p3/mesh-contract/run-13` |
| VRS | **VariableShadingRateTier 2**, AdditionalShadingRatesSupported TRUE, ShadingRateImageTileSize **8**, PerPrimitiveShadingRateSupportedWithViewportIndexing TRUE. 기존 142 case+VS/GS/MS/AS viewport 4 case, 총 **146 case**를 probe override 없이 공개 cap으로 통과. COMMON rate-image의 Close/queued capture는 S_OK지만 FIFO submission은 implicit COMMON→SRS promotion을 거부해 `DXGI_ERROR_DEVICE_REMOVED`(887a0005) | `build/local/p3/vrs2-finish/out-20-tier2/result.json`·`stdout.log` |
| 선행 Work Graph cap(역사적) | 당시 native/WoW64 node **5 case**는 통과했지만 OPTIONS21은 `E_NOTIMPL`이었다. 현재 공개 Tier1.0·9-case 및 최종 common-cut proof와 구분 | `build/local/p3/workgraph-resume/run-14-final-native/result.json`, `run-15-final-wow64/result.json`, `cap-query-stdout.log`(같은 workgraph-resume 디렉터리) |
| Work Graph Tier 1.0 | **실제 OPTIONS21 16-byte S_OK·tier=10**, 15/20-byte E_INVALIDARG와 adjacent guard 보존. **9 ordered case** x64/WoW64 strict PASS: 기존 exact-min25728/depth32/fanout16 및 shared budgets, per-program roots·ordinary-function/aggregate helper parameter+return·public alias/hidden dependency·same-root AddToStateObject·collision negatives, global/local static-sampler union과 local S0-only/ARG0 검증. Linked A **sum196/count4**, B **248/4**, helper **54/4**, local sampler **68/4**. 일반 Shader Model **6.5** 유지 | `build/local/p3/workgraph-resume/runtime-20`, `run-30-final-cap64/result.json`·`stdout.log`, `run-31-final-cap32/result.json`·`stdout.log`(같은 workgraph-resume 디렉터리) |
| WoW64 production presenter GPU probe | `runtime-03-wow64`에서 HDR **5 drawable frame+detached**, color-space **11 frame+GPU retirement gate**, geometry **12 pixel oracle**(crop·3 rotation·background·RTL·restriction) 통과. 모두 rc=0·API/Shader validation 오류 없음. Public suite gate/physical compositor proof가 아닌 production presenter readback이며 color-space `compositor_proof=0`. 초기 `presenters32-01`의 rc=3은 fixture readback allocation에 기존 `WMT_RESOURCE_CPU_ADDRESSABLE` 옵션을 적용해 수정; pointer truncation·새 allocator/shim 없음 | `build/local/p3/integrate/presenters32-02/{hdr-presenter,colorspace-presenter,present-geometry}/{result.json,stdout.log,invocation.json}` |
| x64 production presenter GPU probe | Color-space **11 drawable frame+retirement gate**와 geometry crop·3 rotation·background·RTL·restriction 통과. HDR 최종 proof는 fractional display peak **603.665771**과 content equality, **5 GPU frame+detached** 통과. 초기 HDR truncation/scale-dependent fixture 실패를 최종 proof와 구분; production drawable readback을 physical compositor proof로 확대하지 않음 | `build/local/p3/integrate/native64-01/results.json`(colorspace·geometry), `build/local/p3/integrate/native64-hdr-03/results.json`·`owned-d3d12-hdr-presenter/run.log` |
| x64 scoped contract 회귀 | Enhanced barriers **19 case·rejected 10** API/Shader Validation PASS. Atomic64 live reader는 **API-only PASS**(observed 256·changed 64·first 1/last 128), **atomic64_snapshot=unproven**. 기존 API+shader live 실행의 INCONCLUSIVE를 통과로 바꾸거나 이 관찰을 atomic 64-bit snapshot 증거로 승격하지 않음 | `build/local/p3/integrate/enhanced64-contract-02/enhanced-barriers/result.json`, `build/local/p3/integrate/atomic64-live-api-01/result.json`·`stdout.log` |
| WoW64 scoped query·atomic 회귀 | Clock calibration monotonic·실제 GPU timestamp 증가와 occlusion **16384,4096,0,12288,1,0** API/Shader Validation PASS. Query는 별도 `runtime-query32-01`에서 실행(초기 query abort는 역사적 실패). Atomic64 live는 **API-only PASS** observed256·changed64·first1/last128, **snapshot unproven**; 초기 API+shader INCONCLUSIVE를 통과로 승격하지 않음. 이 별도 native 재빌드 proof는 최종 combined runtime 전체 회귀가 아님 | `build/local/p3/integrate/queries32-02/result.json`·`stdout.log`, `build/local/p3/integrate/atomic-copy-live32-api-01/result.json`·`stdout.log` |
| 초기 WoW64 실패 증거(역사적) | `all32-01`은 56 run 중 **53 API/Shader PASS**였고 query abort·obsolete barrier-interface assertion·atomic-live Shader Validation INCONCLUSIVE를 보존했다. 해당 경로의 후속 proof와 최종 reachable 58-label 결과는 위 행을 참조하며 원래 실패를 통과로 덮어쓰지 않는다 | `build/local/p3/integrate/integration32-initial-evidence.json`, `all32-01/{summary.json,coverage.json}`(같은 integrate 디렉터리) |
| PSO cache epoch | Serialized PSO **revision 4**. Graphics compile key **revision 5**는 이 blob version과 별개다. Current PSO/library GPU round-trip, child restore 4·cold compile 0, digest가 유효한 nested revision 3도 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`로 거부. 기존 캐시 폐기·재생성 필요 | `build/local/p3/cache-epoch/runtime-after`, `cache-after`·`library-after`·`nested-version-proof`(같은 cache-epoch 디렉터리) |

RayQuery는 CS·VS·PS·HS·DS·GS·AS·MS·raygen·closest-hit·any-hit·intersection·miss·callable과 raygen indirect에서 triangle/procedural scene의 24 lane × 64 word를 비교했다. Native intersection callback 안의 inline query가 허용되지 않는 any-hit/intersection 중첩-query pipeline만 **kernel-side candidate traversal + visible callback**을 쓰며, 다른 pipeline은 native fast path를 유지한다. Base·state-object shader의 IgnoreHit·AcceptHitAndEndSearch·ReportHit 거부/interval·opacity·local root·AddToStateObject도 새 경로를 검증한다. 잘못된 candidate InstanceID mutation은 `build/local/p3/rqstages/mutation-run`에서 추가 10 stage 모두 실패했다. AS 증거는 postbuild·clone/compact·serialize/deserialize·TLAS relocation·visualization·in-place/out-of-place update와 빈 AS/비영 offset subrange를 포함한다.

### P3 완료 범위와 cap·검증 경계

- **MeshShaderTier 1 검증:** `integrate/mesh-cap-01`은 새 `integrate/runtime-02`에서 공개 tier=10과 전체 34 case를 strict runner로 통과했다. 앞선 `mesh-contract/run-13`은 cap=0 때문에 runner가 실패했던 역사적 proof이며 현재 cap 결과와 구분한다. 필수 32 packed row/128 scalar에는 float4 SV_Position이 포함된다. Native 124-scalar 한계를 하드웨어 예외로 인정하지 않고, 초과 출력·cull/활성 query는 device-record capture/replay로 처리한다. 일반 query 없는 native 범위는 fast path를 유지한다. Tier 보고는 이 장치에서 확인한 전체 필수 출력·cull 계약과 cap proof에 근거한다.
- **기능별 proof와 최종 통합:** WG root/ordinary collection/local-only sampler와 DXR typed-overload/mixed collection/named-root/resource-binding은 기능별 paired GPU proof에 더해 공통 runtime-04의 x64/WoW64 affected suite에서도 통과했다. Conservative Tier1·VRS Tier2·mesh Tier1의 기능별 strict proof와 합쳐 P3 완료 판정에 사용하되 전체 D3D12/DXR conformance로 확대하지 않는다.
- **FL12_1과 FL12_2:** 현재 공개 최대는 **FL12_1**이다. FL12_1의 conservative raster Tier 1·ROV 요구를 FL12_2와 혼동하지 않는다. 공식 필수 요건 대조(P3CapAudit)에 따르면 FL12_2는 DXR 1.1·mesh 1·VRS 2·sampler feedback 0.9 이상 외에도 **ConservativeRasterizationTier 3, TiledResourcesTier 3, DepthBoundsTestSupported, CopyQueueTimestampQueriesSupported, OPTIONS8의 UnalignedBlockTexturesSupported(unaligned BC)** 등이 필요하다. 따라서 일부 P3 tier 통과만으로 FL12_2를 올릴 수 없으며 **FL12_2는 미지원**이다. SM 6.6 보고를 이 feature level/tier들의 일괄 선행 조건으로 두지 않는다; HighestShaderModel은 검증된 **6.5**를 유지한다.
- **통합 증거의 범위:** x64 **60=59 API/Shader+1 API-only**, WoW64 **58=57+1**은 baseline+source-warranted targeted proof의 reachable 합산이다. 모든 suite를 최신 단일 snapshot에서 재실행한 것은 아니다. Atomic live의 API-only PASS는 Shader Validation PASS 또는 atomic64 snapshot 보증이 아니며 snapshot은 **unproven**으로 남긴다. 기존 retained source가 없는 standalone static-sampler와 WoW64의 x64-only NGX 2 suite는 검증 불가 경계이지 미완료 P3 기능이 아니다. Production presenter readback은 물리 compositor proof가 아니며 다른 GPU/OS·Windows reference differential·실제 게임 정상 렌더링·성능은 보증하지 않는다.


## P2 완료 결과 (2026-10-01)

**P2 완료.** 7개 slice(SM, BINDLESS, GSTS, BARRIERS, TILED, LIBRARY, SHARING)를 모두 구현하고 Metal API/Shader Validation 아래 GPU 검증했다. GSTS가 검증된 뒤 보고 Shader Model을 6.5로 올렸다. 모든 증거 경로는 `build/local/p2/` 아래이고, 실제 GPU 증거는 M5 Pro/macOS 27에 한정한다. 각 suite fixture는 `graphics-translator/tests/owned-d3d12-<suite>.cpp`, gate는 `scripts/test-owned-graphics.py`에 있다. **이 절의 cap·거부 동작·제약·미검증 목록은 P2 종료 당시의 역사적 기록이다.** 이후 RayQuery·feedback·view instancing과 GSTS/SO 제한을 변경한 현재 판정은 P3 절을 우선한다.

| Slice | 새 suite | 상태 |
|---|---|---|
| SM | `sm6` | 완료·검증. 보고 SM **6.5** |
| BINDLESS | `bindless` | 완료·검증 |
| BARRIERS | `enhanced-barriers`, `render-passes` | 완료·검증 |
| TILED | `tiled` | 완료·검증 |
| LIBRARY | `pipeline-library` | 완료·검증 |
| SHARING | `sharing` | 완료·검증 |
| GSTS | `geometry`, `tessellation` | 완료·검증 |

### P2-SM — Shader Model 6.x compiler

- **구현 요약:** DXIL compiler는 SM 6.8까지 입력을 받는다(`SM50_MAX_DXIL_SHADER_MODEL=0x68`, 6.9 이상 거부). Wave/quad op는 32 lane 고정이며 `[WaveSize]`가 32를 허용하지 않으면 compile 오류다. PS helper lane은 `WaveOpsIncludeHelperLanes`가 없으면 wave op에서 제외하고 quad op에는 포함한다. int64(전 stage), 16-bit 연산·buffer·stage I/O, min precision, packed op, compute quad derivative·implicit LOD·`CalculateLOD`, barycentrics·`GetAttributeAtVertex`, `SV_Start*Location`, SM6.8 barrier를 구현했다. 64-bit raw/structured UAV atomic은 device 전역 65536-entry lock table로 **compute에서만** emulation한다. Graphics stage의 64-bit atomic, groupshared/typed 64-bit atomic, FP64는 compile 단계에서 거부한다. Wave shader PSO 실패의 원인은 접미사 없는 lane intrinsic이 Metal compiler service를 중단시킨 것이었다(`.i32` 표기로 수정). `TotalLaneCount`는 IOKit GPU core 수×128이며 WineMetal call 256에서 읽는다.
- **보고 cap:** `HighestShaderModel` **6.5**(`kOwnedReportedShaderModel = D3D_SHADER_MODEL_6_5`, `src/d3d12/d3d12_device.hpp:151`). `sm6` gate는 `OWNED_D3D12_SM6_CAPS shader_model=0x65`를 요구한다(`scripts/test-owned-graphics.py:976`). SM6.1–6.5 요구사항을 DXC wiki·DXIL.rst·DirectX-Specs 기준으로 대조했다. Cap으로 gate되지 않는 요구사항은 모두 구현·GPU 검증했다(VS wave는 `vs_wave`). 보고하지 않는 cap-gated 기능은 PSO 생성에서 깨끗하게 실패한다. GS/HS/DS DXIL은 P2-GSTS에서 검증했다. **SM 6.6은 계속 막혀 있다:** 모든 stage의 64-bit atomic을 요구하는데, fragment stage의 lock emulation은 Apple GPU를 hang시킨다(아래 증거). WaveOps TRUE(lane 32/32, TotalLaneCount 2560), Int64ShaderOps TRUE, Barycentrics TRUE, Native16Bit TRUE. DoublePrecision·AtomicInt64OnTyped/GroupShared/DescriptorHeap·OPTIONS9·OPTIONS11 FALSE, WaveMMA·ViewInstancing NOT_SUPPORTED, MinPrecision 16_BIT 유지.
- **6.5 상향 때 추가한 구현:**
  - `SV_ViewID`(6.1), P2 당시: ViewInstancingTier가 NOT_SUPPORTED이므로 규격대로 0을 읽는다(`dxil_signature.cpp`).
  - fp32 denorm mode(6.2): `any`·`ftz`는 native다. M5 Pro는 모든 Metal 옵션(`-fdenorms`, `-fno-fast-math`, `-fmetal-math-mode=safe`, LLVM `denormal-fp-math=ieee`)에서 fp32 denormal 입력과 출력을 flush한다(`smaudit/evidence-denorm-native`). 그래서 `preserve`를 exact software 경로로 구현했다. 이전에는 이 속성을 무시해 flush했다. add/sub/mul/div는 `src/airconv/shaders/air_denorm.metal`의 exact helper로, fcmp는 IEEE bit 순서 비교로, FMad는 fuse하지 않은 exact mul+add로 바꾼다. dot과 float wave Sum/Product도 exact helper로 계산한다(`dxil_operations.cpp`). Groupshared·UAV·SV_Target store는 native로도 denormal bit를 보존한다. DXIL.rst가 flush를 허용하는 op(FMin/FMax, sqrt/rsqrt, round*, frc, log/exp/trig)는 `preserve`에서도 native로 flush한다. fp16 denormal은 native로 보존된다.
  - P2 당시 보고하지 않는 cap-gated 기능을 거부했다. SV_ShadingRate(VRS Tier 2), RayQuery(RaytracingTier 1.1), sampler feedback은 `dxil_metadata.cpp:924-943`에서 명시적 오류 메시지와 함께 `E_FAIL`을 반환한다. 이전에 SV_ShadingRate는 compile에 성공했고, 나머지 둘은 무관한 내부 오류로 실패했다. View instancing(count 2)과 MS subobject는 pipeline stream 단계에서 `E_INVALIDARG`를 반환한다.
- **GPU 증거:** `smaudit/sm6-02`(API/Shader Validation): 26 CASE_PASS, stress 20/20. 새 case는 `sv_viewid`(VS·PS·varying 0), `cap_rejected`(`ps_shadingrate`·`cs_rayquery`·`ps_feedback`·`ps_viewid_vi`·`ms_mesh`), `denorm_any/ftz/preserve`다. `denorm_preserve`는 HLK Denorm*Preserve vector 13종과 denormal·tiny·special 혼합 입력을 쓰는 64 lane case이며, DAZ/FTZ를 끈 host SSE와 bit-exact로 일치한다. Native prototype의 add/sub/mul/div 4,194,304 random pair에서도 mismatch는 0이었다. Case 추가 run은 `sm65cases/run-01`(23 case, Metal validation 오류 0)이다. 회귀: `smaudit/{dxil,dxil-texture}-01`. 이 run들은 6.5 상향 전 binary라 `shader_model=0x51`을 출력한다. `0x65` 보고는 최종 통합 run에서 확인한다(아래 P2 통합). 이전 증거: `sm/sm6-02`·`sm/sm6-03-novalidation`(20 case), `sm/sm6-04`, CS 64-bit atomic stress(65536 thread×1/16 target 20/20 exact), 회귀 `sm/{dxil,dxil-ia,dxil-texture,raster-order,pipeline-statistics,capabilities}-02`.
- **제약 증거:** `sm/evidence-fragment-lock`. Native MSL fragment spin lock은 validation 없이 200회 중 83회만 완료되고 나머지는 GPU hang/victim이었다. `MTL_SHADER_VALIDATION=1`에서는 항상 hang된다. 같은 lock loop는 compute에서 오류가 없다. Apple native u64 min/max는 AIR 2.9에서만 compile되고 반환값이 없다. Typed/threadgroup i64 atomic은 Metal PSO 생성에서 충돌한다. FP64 ALU는 없다. `texture2d_ms`는 read-only라 `TextureStoreSample`(op 225)은 거부한다.
- **미검증:** `SampleCmpLevel/Bias/Grad`·`GatherRaw`·64-bit gather(구현만 있음), sampler MinLOD/MaxLOD clamp를 반영한 `CalculateLOD`(shader에서 보이지 않음), compute sampling의 sampler LOD bias(0만 검증). Denorm `preserve`는 stage와 무관하게 적용되지만 GPU case는 CS뿐이다(VS/PS/GS/HS/DS 미검증). `preserve`에서 f16 denormal 범위 근처의 f32→f16 변환, `preserve` shader의 성능 비용(fp32 연산마다 helper 호출, 정규 operand는 fast path)도 미검증이다.

### P2-BINDLESS — Resource Binding Tier 3·SM 6.6 dynamic resources·RS 1.2

- **구현 요약:** 기존 descriptor heap 배치(shader-visible heap당 Metal buffer 하나, 32-byte descriptor)를 유지하므로 view 수는 heap 크기(1,000,000 view·2048 sampler)로만 제한된다. `CBV_SRV_UAV/SAMPLER_HEAP_DIRECTLY_INDEXED` root signature에는 heap base·count 4개 qword를 root argument 뒤에 붙이고, `SetDescriptorHeaps`와 `Set*RootSignature` 양쪽에서 채운다. `createHandleFromHeap`(218)의 view 형태는 `annotateHandle`(216)의 ResourceProperties에서 얻는다. Lane별로 descriptor를 읽으므로 `NonUniformResourceIndex`에 waterfall이 필요 없다. Heap 범위 밖 index나 heap 미바인딩은 내부 null descriptor로 처리해 read 0, write/atomic 무효이며 GPU가 heap 밖을 읽지 않는다. Null texture/sampler handle은 모든 texture op 전에 guard한다. RS 1.2는 `D3D12_STATIC_SAMPLER_DESC1`(flags)을 serialize/deserialize·버전 변환하고 non-normalized 좌표·UINT border 규칙을 검사한다. Heap-indexed ROV는 heap argument-buffer member에 ROG 속성을 붙여 구현했다.
- **보고 cap:** ResourceBindingTier **3**, ResourceHeapTier 2(변경 없음), ROOT_SIGNATURE HighestVersion **1.2**.
- **GPU 증거:** `bindless/bindless-03`(7 case), heap ROV를 추가한 `bindless/bindless-10`(8 case, `OWNED_D3D12_BINDLESS_ROV triangles=512 pixels=256 expected=0xc71c711c structured_mismatches=0 typed_mismatches=0 texture_mismatches=0`). Heap index 999,999 UAV, 80개 divergent raw UAV, divergent CBV/sampler, SM5.1 unbounded DXBC(space 0–3), heap 전환, 범위 밖 read 0, non-normalized sampler·`OPAQUE_{BLACK,WHITE}_UINT`를 포함한다. ROG 속성만 제거한 음성 대조는 validation 없이 256/256/256 mismatch였다. Metal GPU validation은 fragment를 직렬화해 race를 숨기므로 validation 실행만으로는 ordering을 증명하지 못한다. 회귀: `bindless/reg-{dxil-02,raster-order-03,dxil-texture-03}`, `bindless/reg-{core-contracts,bundles,pipeline-cache,execution-safety}-01`.
- **제약/미검증:** Heap ROV를 쓰는 PS는 모든 resource-heap 접근이 정렬되어 성능 비용이 있을 수 있다. MSAA·sample-frequency PS의 heap ROV와 Texture1D/2DArray/3D heap ROV는 GPU 미검증이다(Texture2D·typed·structured만 검증). Shader/root flag 불일치는 compile 실패로 `E_FAIL`을 반환한다(D3D12는 실패만 요구). Heap-indexed acceleration structure는 구현만 있고 GPU 미검증이다. 기록되지 않은 heap descriptor는 0으로 읽힌다(규격상 값 미정).

### P2-BARRIERS — Enhanced barriers·Device6–14·CommandList5–9·render pass·GPU upload heap

- **구현 요약:** `ID3D12GraphicsCommandList7::Barrier`는 subresource별 layout을 submission·queue를 넘어 추적한다. Legacy와 enhanced 모델은 COMMON으로만 전환한다. 규격 위반 barrier는 `Close`가 `E_INVALIDARG`, 실행 시 layout 불일치는 device removal이다. 순서를 요구하는 barrier는 현재 encoder를 끝내 Metal fence로 ordering·visibility를 만든다. Device10은 initial layout과 castable format 목록으로 resource를 만들고 목록의 format view를 허용한다. Front/back stencil ref·per-face mask, dynamic depth bias, dynamic strip cut을 구현했다(새 WineMetal render command, unix-call index 없음). Device13 `OpenExistingHeapFromAddress1`은 SHARING의 host-memory heap으로, Device14 `CreateRootSignatureFromSubobjectInLibrary`는 DXIL RDAT subobject에서 root signature를 만든다. `BeginRenderPass/EndRenderPass`는 render-pass 규격의 fallback(일반 bind·clear·resolve로 변환)으로 구현했다. Suspend/resume과 local-compute pass를 지원하고 pass 안의 금지 API·barrier는 기록 오류다. `D3D12_HEAP_TYPE_GPU_UPLOAD`는 heap·committed·placed resource에 허용한다. Shared storage·write-combined이며, shared/display/RT/DS flag는 거부한다. `CreateSampler2`의 UINT border는 Metal 고정 border 세 가지(0000/0001/1111)만 허용한다.
- **보고 cap:** EnhancedBarriersSupported **TRUE**, IndependentFrontAndBackStencilRefMaskSupported·DynamicIndexBufferStripCutSupported·DynamicDepthBiasSupported TRUE, RenderPassesTier **TIER_0**, RenderPassesValid **TRUE**, GPUUploadHeapSupported **TRUE**. RelaxedFormatCastingSupported **FALSE**, TriangleFanSupported FALSE, OPTIONS13·OPTIONS17 FALSE. `ID3D12GraphicsCommandList10`(work graphs)은 노출하지 않으며 P3다.
- **GPU 증거:** `barriers/enhanced-barriers-13`(19 case, `INVALID_BARRIERS_PASS rejected=10`, `STENCIL front=11 back=f2`, `DEPTH_BIAS … dynamic=0.251220703 reset=0.251220703 override=0.25`). UAV→UAV/SRV/COPY 4096 word, split barrier, castable RGBA8→R32_UINT의 정확한 bit, subresource 범위 layout, cross-queue COMMON handoff와 누락 시 removal, Device13 host memory GPU write→CPU read, Device14 library root signature dispatch를 포함한다. `barriers/render-passes-02`(8 case): CLEAR→PRESERVE `ff402080`, DISCARD+depth CLEAR `color=ffff0000 depth=0.125`, 4× resolve average `0x50`·min 0.125·max 0.5, suspend/resume `ff204010`, 10개 잘못된 사용 거부, GPU upload committed·placed buffer 1024 word와 `WriteToSubresource` texture, UINT border 3종. 회귀: `barriers/reg-{copy-resolve,core-contracts,msaa-lines,multisample-capabilities,resources}-03`, `barriers/reg-{execution-safety,multiqueue,so-format-support,stream-output,present-queues}-02`.
- **제약 증거:** RelaxedFormatCasting은 BC→비압축 view를 요구하지만, M5 Pro의 `newTextureViewWithPixelFormat:`은 BC1→RG32Uint, BC1→BC4, BC3→BC7을 'not compatible', Depth32Float↔R32Float를 'not castable'로 거부한다. Castable list는 Metal이 허용하는 쌍만 받고 나머지는 생성 시 거부한다. Render pass resolve는 기존 `ResolveSubresourceRegion` draw 경로를 사용하므로 destination에 `ALLOW_RENDER_TARGET`이 필요하다.
- **미검증:** Bundle 안의 enhanced barrier 거부(fixture 없음), queue 전용 layout(DIRECT_QUEUE_*/COMPUTE_QUEUE_*, GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE)의 개별 GPU 실행, castable list를 준 `GetResourceAllocationInfo3`. `CreateReservedResource2`의 GPU 경로는 TILED 범위다. VRS·mesh·sampler feedback·protected session·shader cache session·`AddToStateObject`는 오류를 반환하고 cap을 보고하지 않는다.

### P2-TILED — Reserved/tiled resource

- **구현 요약:** D3D12 64KB tile 하나를 Metal placement-sparse page(64KB) 하나에 대응한다. 모든 D3D12 heap은 placement heap이다. Reserved buffer는 실제 GPU VA를 가진다. Reserved texture는 2D/2D array, single-sample, `64KB_UNDEFINED_SWIZZLE`이며 format별로 Metal `sparseTileSize`가 D3D 표준 shape와 같을 때만 허용한다. Packed mip은 Metal sparse tail로 대응한다. `UpdateTileMappings/CopyTileMappings`는 queue FIFO에서 이전 submission 완료를 기다린 뒤(NO_HAZARD 제외) device 전역 MTL4 queue로 적용하고, shared event를 기다린다(10 s 초과 시 DEVICE_HUNG). 잘못된 호출은 mapping을 바꾸지 않고 버린다. Residency status는 texture에서 AIR residency bit, buffer에서 queue 순서로 기록하는 tile별 byte 배열을 사용한다. Root SRV/UAV는 root signature 뒤의 status qword, RGB32 emulated typed SRV는 descriptor q0로 status를 전달한다. SM5 DXBC의 모든 `*_FEEDBACK` opcode와 `check_access_fully_mapped`, DXIL `CheckAccessFullyMapped`를 lowering한다. Min/max reduction sampler를 추가했다. FXC 입력으로 발견한 `CHECK_ACCESS_FULLY_MAPPED` immediate 타입 오류도 수정했다.
- **보고 cap:** TiledResourcesTier **2**(`supportsPlacementSparse`일 때), `FORMAT_SUPPORT2_TILED`는 reserved 경로가 허용하는 format만 보고한다. **Tier 3·Tier 4와 SRVOnlyTiledResourceTier3는 보고하지 않는다.**
- **GPU 증거:** `tiled/final-tiled`(25 case, `OWNED_D3D12_TILED_CAPS tier=2 srv_only_tier3=0 va_bits_per_resource=48 rgba8_tiled=1`, `OWNED_D3D12_TILED_PASS cases=25 tier=2`). 두 heap mapping, NULL tile read 0·write 무효, REUSE_SINGLE_TILE, CopyTileMappings, ExecuteCommandLists 경계의 mapping 갱신, cross-queue 순서, Evict/MakeResident, `GetResourceTiling`(512²×10 RGBA8: standard 3·packed 7 mip), packed tail mapping, texture·buffer·root view·RGB32 residency status, FXC로 고정한 SM5 feedback 16 opcode(cs·ps), CopyTiles 왕복과 unmapped 0, min/max reduction을 포함한다. 회귀: `tiled/final-{bindless,raytracing}`, `tiled/reg-{bindless,bundles,capabilities,dxil-texture,predication-indirect,raytracing,resources}-02`, `tiled/reg-dxil-03`.
- **제약:** Metal 3D sparse tile은 64KB에서 128×128×1(RGBA8)이고 D3D12 표준 3D tile은 32×32×16이다. Volume tile을 독립적으로 mapping할 수 없으므로 3D reserved texture는 `E_INVALIDARG`이고 Tier 3를 보고하지 않는다. Packed mip이 있는 array는 거부하므로 Tier 4를 보고하지 않는다. MSAA·1D reserved texture도 `E_INVALIDARG`다. Local root signature(DXR shader record)의 root view와 ExecuteIndirect 인수로 설정한 root view는 status qword가 없어 항상 mapped로 보고한다.
- **비용:** MTL4 mapping 갱신은 enqueue 시점에 적용되고 MTL4 wait/barrier를 무시한다. 그래서 NO_HAZARD가 아닌 갱신마다 queue worker가 이전 submission 완료까지 직렬화된다. 같은 heap page를 공유하는 sparse tile은 command buffer 경계나 `MTLSharedEvent` signal+wait에서만 coherent하다. MTLFence·memoryBarrier·일반 MTLEvent로는 부족하다. 그래서 reserved resource가 하나라도 살아 있으면 모든 ordering barrier와 command-list 시작에 alias flush(shared event signal+wait)를 추가한다. Reserved resource가 없으면 비용이 없다.
- **미검증:** 10 s DEVICE_HUNG timeout 경로. RGBA8/R16F/R32F 외 reserved texture format(`sparseTileSize` 일치로만 판정). 32-bit에서는 CPU-visible heap의 `UpdateTileMappings`를 설계상 거부한다.

### P2-LIBRARY — `ID3D12PipelineLibrary/1`

- **구현 요약:** 이름→entry table(`shared_mutex`, 삽입 순서 유지)에 PSO 종류, SHA1 `PipelineCacheKey`, 기존 단일 PSO cache blob(DXMTPSO v3) 사본과 생성된 PSO를 보관한다. Load는 desc에서 key를 다시 계산해 이름·종류·key가 다르면 `E_INVALIDARG`이고, 일치하면 `CachedPSO`로 PSO를 한 번만 생성해 같은 객체를 반환한다. Container `DXMTPLB\0` v1(104-byte header: adapter registryID, compiler UUID·driver build, SHA1 digest)을 검증한다. Magic·크기·digest·경계 오류는 `E_INVALIDARG`, version·identity 불일치는 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`, adapter 불일치는 `D3D12_ERROR_ADAPTER_NOT_FOUND`다. Identity를 digest보다 먼저 검사한다. 생성 시 blob을 복사하므로 앱이 blob을 바로 해제해도 된다. `ID3D12Device2::CreatePipelineState`와 `LoadPipeline`이 같은 stream parser를 쓴다. 이전에 경고 후 무시하던 depth bounds·view instancing과 범위 밖 subobject type(UB)을 이제 `E_INVALIDARG`로 거부한다. Graphics key는 GS/HS/DS bytecode도 포함한다.
- **DXMTPLB는 native Metal binary archive가 아니다.** Load는 owned shader compile을 건너뛰지만 Metal library/function/PSO 생성은 수행한다.
- **보고 cap:** SHADER_CACHE `SINGLE_PSO | LIBRARY`(AUTOMATIC_* 없음), SERIALIZATION HeapSerializationTier **TIER_0**.
- **GPU 증거:** `library/pipeline-library-01`: 9개 CASE_PASS, 13개 GPU line(stored·deserialized·child×4 PSO 종류와 thread entry), `THREADS threads=8 rounds=24 entries=12`, `CHILD_PASS`. 자식 process log는 restore 4·cold 0으로 compile 생략을 확인한다. 회귀: `library/{pipeline-cache,bundles,core-contracts,dxil}-01`.
- **제약:** Depth bounds test(Metal에 없음), narrow quadrilateral line, view instancing, AS/MS bytecode(P3)는 `E_INVALIDARG`. Fractional depth bias와 per-face stencil mask의 거부는 BARRIERS가 해당 cap을 구현하며 제거했다. `OpenExistingHeapFromAddress/FileMapping` 기반 heap serialization은 tier 0이다.
- **미검증:** 실제 다른 GPU·driver의 blob(header 변조로만 시뮬레이션), library에 저장한 GS/HS/DS PSO. Graphics key가 GS/HS/DS bytecode를 포함하므로 GSTS 완료로 경로는 열렸지만, `pipeline-library` fixture에는 GS/HS/DS를 쓰는 case가 없다.

### P2-SHARING — Shared heap·resource·fence

- **구현 요약:** 공유 객체마다 Wine section `Local\dxmt.share.<pid>.<128-bit token>`(64 KiB header + payload)을 만들고 `CreateSharedHandle`은 그 section의 NT handle을 `DuplicateHandle`로 반환한다. `CloseHandle`·복제·상속은 Wine 기본 동작이다. Open 시 object type·이름 prefix·magic/version/kind/payload 경계를 검사하고 이름의 token과 header token을 대조해 위조 header를 거부한다. Heap·committed buffer는 section payload 위의 no-copy buffer다. IOSurface texture와 `MTLSharedEvent`는 process별 mach receive right(`bootstrap_register2`)로 전달하고, Metal identity(IOSurface ID·`globalTraceObjectID`)를 header와 대조한다. D3D name은 per-user salt의 SHA-256 link section으로 대응하며 중복은 `DXGI_ERROR_NAME_ALREADY_EXISTS`다. `CreateHeapFromHostMemory`/`FromFileMapping`(Device3, Device13 경유)도 구현했다.
- **Fence rewind 모델:** `MTLSharedEvent`는 되돌릴 수 없으므로(`supportsRollback=0`) event에는 단조 증가하는 native token만 싣는다. Fence section의 `SharedFenceTimeline`은 atomic `next_token`과 1024-entry `{token, logical}` ring이다. 모든 Signal은 token을 예약하고 logical 값을 기록한 뒤 event를 그 token으로 올린다. 관찰자는 token으로 ring을 읽어 기존 rewind-aware 경로에 logical 값을 공개하므로 `Signal(lower)`가 모든 process에서 일관되게 rewind된다. Ring이 가득 차면 CPU caller(CPU Signal, queue submission worker)만 backpressure로 대기하고 GPU는 기다리지 않는다. Queue Wait는 관찰한 native token을 GPU에서 기다린다.
- **보고 cap:** CrossAdapterRowMajorTextureSupported **TRUE**, CrossNodeSharingTier NOT_SUPPORTED.
- **GPU 증거(64-bit):** `integrate/run-02/sharing`(18 case, `OWNED_D3D12_SHARING_PASS cases=18`)와 WoW64 staging runtime의 64-bit fixture `sharing/sharing-64-w64-01`. Same-process A↔B buffer/texture/fence 4096 word readback·writeback, CPU Signal→GPU Wait, shared heap placed buffer, cross-adapter heap의 ROW_MAJOR texture, named open·중복·누락, 위조 token 등 오류, host memory·file mapping heap, `CreateProcess` 자식과 양방향 data·GPU fence 순서, 핸들 없이 이름으로만 여는 무관한 process(rewind 값 40), fence rewind, 3000 Signal stress를 포함한다. 실행 뒤 누수된 `dxmt.share.*` bootstrap 이름은 0이었다. 이전 회귀: `sharing/{execution-safety,fence-events,multiqueue,residency,resources,temporal}-02`.
- **제약:** IOSurface 공유 texture는 Metal에서 private storage여야 하므로 ROW_MAJOR가 아닌 texture는 shared heap에 둘 수 없다(`E_INVALIDARG`). Host memory heap은 주소·크기가 16 KiB 정렬이어야 하고, Rosetta/Wine의 4 KiB 정렬 할당은 거부한다. `bootstrap_register2(name, NULL)`은 등록을 해제하지 않으므로 receive right 파괴로 해제한다. 공유 fence에서 현재 값보다 낮은 native event 값을 쓰지 않는다(rewind는 logical ring으로 처리).
- **미구현/미검증:** **D3D11 interop(`OpenSharedResource1`)은 하지 않았다.** D3D11은 D3DKMT escape·bootstrap 이름 방식을 쓰므로 D3D11이 section 형식을 해석해야 하고, 이는 최소 공유 변경을 넘는다. 32↔64-bit 혼합 cross-process는 실행하지 않았다(fixed-width 형식이라 동작할 것으로 추정 `[INFERENCE]`). Ring 1024를 넘는 동시 publication은 backpressure로 막히며, 3000 Signal stress 이상의 경합은 미검증이다.
- **공유 `src/dxmt` 변경(D3D11 동작 동일):** `Fence`의 shared timeline 경로(shared event가 있을 때만 활성), `Texture::allocate(flags, placed_memory)`, `Texture::import` 실패 시 assert 대신 nullptr 반환(죽은 port가 debug build를 abort하던 버그 수정).

### P2-GSTS — GS/HS/DS

- **구현 요약:** D3D12 PSO의 VS+GS, VS+HS+DS, VS+HS+DS+GS를 DXBC와 DXIL 모두에서 Metal mesh pipeline으로 실행한다(이전에는 `E_NOTIMPL`). VS+GS는 VS object→GS mesh, VS+HS+DS는 VS+HS object→DS mesh이며 D3D11과 같은 airconv 다단계 entry를 쓴다. DXIL은 두 번째 ABI를 만들지 않는다. `SM50ShaderInternal::body_emitter` hook이 DXBC control flow 대신 DXIL 본문을 이식하고, 네 DXBC converter는 그대로 함수를 만든다(`src/airconv/dxil_pipeline.cpp`, HS/DS는 `dxil_pipeline_ts.cpp`).
  - **TS+GS(VS+HS+DS+GS):** 2-pass capture/compaction/GS mesh 경로다(설계: `MeshPipelineKind::TessellationGeometry`, `src/d3d12/d3d12_command_list.cpp` `RecordTessellationGeometryDraw`). Pass 1은 raster를 끈 VS+HS object와 capture mode DS mesh(`SM50_SHADER_D3D12_TESS_CAPTURE`)다. 각 workload가 DS vertex(R개 int4)와 index를 scratch slot `s = (instance*patches + patch)*W + rank`에 쓴다. Compaction compute kernel(`dxmt_tess_capture_clear`, `dxmt_tess_capture_compact`)은 slot 순서, 즉 D3D primitive 순서로 prefix scan해 flat primitive list와 object grid를 만든다. Pass 2는 capture를 읽는 object(`dsgs_main`, `SM50_SHADER_GS_TESSELLATION_INPUT`)와 DS를 첫 stage로 쓰는 GS mesh이며 `DrawMeshThreadgroupsIndirect`로 그린다. `SV_PrimitiveID`는 DS patch id다. Scratch는 CPU가 알려진 patch·instance 수와 PSO 생성 시 DS layout(W/V/P/R/K)으로 최악 크기를 계산하므로 capture write가 넘칠 수 없다. `maxBufferLength`나 UINT32 slot 범위를 넘는 draw는 `E_OUTOFMEMORY`다.
  - **Statistics:** 각 counter를 정확히 한 번 센다. IA·VS·HS는 pass 1 object, DS는 pass 1 mesh, GSInv·GSPrim·C는 pass 2 GS mesh에서 센다.
  - **PSG1 수정:** DXC는 patch-constant signature를 PSG1 chunk에 저장한다. HS/DS는 PCSG만 읽어 system value가 없는 metadata를 썼고, tess factor가 모두 0이 되어 모든 patch가 cull됐다(DSInvocations 0). `src/airconv/dxil_metadata.cpp:895-902`에서 PSG1을 먼저 읽고 PCSG는 fallback으로 둔다.
  - **Signature 번호:** DXIL signature chunk는 register row마다 entry가 있지만 `dx.op` element id는 element 단위다. 그래서 GS/HS/DS lowering은 entry metadata의 element 목록으로 id를 해석한다.
- **GPU 증거(API/Shader Validation):**
  - `dxilgsts/geometry-04`: `OWNED_D3D12_GEOMETRY_MATRIX_PASS shader=dxbc cases=41`, `shader=dxil cases=41`. Point→quad amplification, cut이 있는 strip, line/lineadj/triadj list·strip 입력, GS instancing·`SV_GSInstanceID`, `SV_PrimitiveID`, line/point SO topology, indexed·indirect draw, pipeline statistics, stream 0 SO capture, MSAA를 포함한다.
  - `dxilgsts/tessellation-04`: `OWNED_D3D12_TESSELLATION_MATRIX_PASS shader=dxbc cases=146`, `shader=dxil cases=146`, `OWNED_D3D12_TESSELLATION_PASS`. 각 경로의 146 case에 TS+GS 10 case가 들어 있다: tri/quad domain, integer/fractional_odd, SO stream 0, 1×/4× MSAA, 출력 불일치 `E_INVALIDARG`, indirect `E_NOTIMPL`. Oracle은 DS domain point를 tessellator oracle과, 이미지(1×와 resolve한 4×)를 GS 없는 같은 HS/DS PSO와 비교한다. Statistics와 SO 순서·내용(patch 순서, 1e-4 안에서 patch 면적 tiling)도 검사한다.
  - DXBC 회귀: `gsts/reg-{stream-output,pipeline-statistics,msaa-lines,multisample-capabilities,indexed-restart,bundles,predication-indirect,raster-order,dxil-texture}-40`, `gsts/reg-geometry-42`. DXIL 회귀: `dxilgsts/reg-{dxil,dxil-ia,dxil-texture,sm6}-04`.
- **공유 D3D11 변경:** `src/d3d11/d3d11_context_impl.cpp`의 `TessellationDraw`·`TessellationDrawIndexed`·`GeometryDraw`·`GeometryDrawIndexed`에 `MeshInstancesPerDraw`/`MeshDrawChunks`를 추가했다(`git diff`: 128줄 추가, 94줄 삭제). 이전에는 Apple7 미만 GPU(`maxObjectThreadgroups()` 1024, `src/dxmt/dxmt_device.cpp:45-48`)에서 object-threadgroup grid가 1024를 넘는 draw를 `WARN`만 남기고 그리지 않았다. 이제 instance 단위로 나눠 여러 mesh draw를 내보낸다. Chunk마다 draw argument block이 따로 있고 `StartInstance`가 chunk만큼 증가하므로 `SV_InstanceID`는 정확하다. Instance 하나가 한도를 넘을 때만 이전처럼 생략한다. 필요한 이유: GSTS 과제가 공유 mesh 경로의 이 silent drop 수정을 요구했다. D3D12는 자체 command list에서 mesh draw를 인코딩하고 Apple7+에서 한도가 없으므로(`d3d12_command_list.cpp`의 `max_threadgroups`) 공유 draw 경로의 사용자는 D3D11뿐이다. 그래서 D3D11 파일만 최소 범위로 바꿨다. **D3D11 baseline 통과:** `gsts/d3d11-40/baseline{,-packed}.stdout.log`에서 두 실행 모두 rc=0이다. `D3D11_BASELINE_DEVICE`, `CLEAR3D_UINT/FLOAT_PASS`, `DXBC_COMPUTE_LOAD_STORE_PASS`, `DXBC_SAMPLE_PASS`, `WINDOWED_PRESENT_PASS`를 출력했다.
- **제약:**
  - **P2 당시 제약(현재 후속 수정):** TS+GS PSO의 `ExecuteIndirect`는 `E_NOTIMPL`이었다. Capture scratch는 overflow가 불가능하도록 encode 전에 patch 수 상한으로 크기를 정해야 한다. Indirect vertex·instance 수는 GPU 값이고 D3D12는 임의 uint32를 허용하므로 CPU가 아는 상한이 없다. 유한한 사전 할당으로는 overflow를 막을 수 없고, 미리 기록한 Metal command stream은 mesh pass buffer를 GPU에서 할당할 수 없다.
  - **P2 당시 제약(현재 후속 수정):** MSAA mesh draw 위의 statistics는 `E_NOTIMPL`이었다(native MSAA가 PSInvocations를 과다 호출).
  - **P2 당시 제약(현재 후속 수정):** GS SO는 stream 0만 지원한다. Stream 1–3은 `E_NOTIMPL`이다(TS+GS와 DXIL GS도 같은 규칙). GS 없는 VS+HS+DS PSO의 SO는 `E_NOTIMPL`이다(`src/d3d12/d3d12_pipeline_graphics.cpp:1179-1181`).
  - TS+GS에서 GS 입력 primitive는 tessellator 출력(point/line/triangle)과 같아야 하며, 다르면 `E_INVALIDARG`다.
- **P2 당시 미검증:** isoline/point 출력을 line/point GS로 받는 TS+GS(ABI는 K=2/1을 지원하지만 fixture는 triangle만 다룬다). Indexed·instanced·predicated TS+GS(코드 경로는 있지만 fixture case가 없다). D3D11 >1024 분할의 Apple7 미만 하드웨어 실행(M5에서는 한도가 없어 시험할 수 없다). DXIL GS의 `SV_ClipDistance/CullDistance`·`SV_RenderTargetArrayIndex/ViewportArrayIndex`(compile만 확인). DXIL GS stream 1–3 거부 실행. 한 PSO에서 DXBC stage와 DXIL stage를 섞는 GS/HS/DS 조합. 이후 GS/TS+GS indirect·stream 1–3·DS SO의 검증 범위는 P3 절에 별도로 기록한다.

### P2 통합·WoW64 결과

- **최종 64-bit 통합:**
  - `integrate/run-03/summary.tsv`(52 suite, `dxil-static-sampler`는 fixture 없음으로 제외): 51개 통과. `tessellation`만 실패했는데, `runtime-03`이 마지막 DXIL 수정(PSG1, TS+GS DXIL routing) 전에 staging됐기 때문이다.
  - `integrate/run-04/summary.tsv`: `tessellation`, `geometry`, `dxil`, `sm6`을 `runtime-04`에서 다시 실행해 모두 통과했다(validation 오류 0, api-and-shader).
  - SM 6.5 보고를 포함한 `runtime-final`의 최종 전체 실행(`integrate/run-final/summary.tsv`): **52/52 suite 통과**, 전부 validation 오류 0(api-and-shader). `sm6`·`capabilities`는 `shader_model=0x65`·`highest=0x65`를 출력한다. CPU 검증 `test-owned-graphics-{build,staging,diagnostics}.py`와 `test-owned-ngx-frontend.py`도 통과했다.
- **이전 64-bit 통합:** `integrate/run-01/summary.tsv`는 51개 suite 중 48개 통과, 3개 실패였다(`fullscreen`, `dxil-static-sampler`는 fixture 없음으로 제외). 실패 3개 중 2개는 새로 보고한 cap에 대한 오래된 fixture 기대값이었다. `pipeline-library`는 `independent_stencil_masks`·`fractional_depth_bias`에 `E_INVALIDARG`를 기대했으므로 이제 `S_OK`를 기대한다. `enhanced-barriers`는 `capabilities` 단계에서 실패했다(run-01 fixture binary가 `GPUUploadHeapSupported=1` 기대값으로 source를 갱신하기 전에 빌드됐다. binary·source 시각 비교에 따른 추정 `[INFERENCE]`). 나머지 하나는 실제 결함이었다. `ValidateHeapProperties`가 GPU_UPLOAD case에서 DEFAULT로 fall-through해 shared DEFAULT heap을 거부했다(`CreateCommittedResource shared buffer hr=0x80070057`). `src/d3d12/d3d12_resource_helper.cpp`에서 수정했다. 세 suite는 `integrate/run-02/summary.tsv`에서 통과했다.
- **WoW64(32-bit), 최종 tree:** `wow64/summary-02.tsv`에서 PE32 suite 17/17이 통과했다. 대상은 geometry, tessellation, stream-output, indexed-restart, so-format-support, resources, smoke, sharing, tiled, enhanced-barriers, render-passes, pipeline-library, bindless, sm6, fence-events, multiqueue, dxil이다. 증거는 `wow64/final-<suite>`이며, `final-geometry`와 `final-tessellation`은 DXBC·DXIL 각각 41·146 case의 `MATRIX_PASS`를 출력한다. 이 실행은 6.5 상향 전이라 `final-sm6`는 `shader_model=0x51`을 출력한다. 상향 뒤 32-bit `sm6`을 다시 실행한 `wow64/final-sm6-65`는 `shader_model=0x65`로 통과했다(validation 오류 0). 32-bit argument 변환은 `src/winemetal/unix/winemetal_unix.c`의 `SM50_SHADER_D3D12_TESS_CAPTURE`(layout_out pointer)·`SM50_SHADER_GS_TESSELLATION_INPUT` 경로다.
- **WoW64 초기 실행과 수정:** `wow64/summary.tsv`의 PE32 fixture 14개 중 12개가 처음에 통과했고, 결함 두 개를 수정했다.
  - `pipeline-library`: fixture가 i686에서 stream을 8 byte로 padding했다(fixture 결함). 재실행 `wow64/run-pipeline-library-02` 통과.
  - `sharing`: `d3d12_buffer.cpp`의 dangling-else로 i386 build에서 fallback allocation chain이 무조건 실행되어, 모든 shared committed buffer가 새 private allocation으로 rename됐다. 할당 chain을 마지막 `else` 안으로 묶었다. `sharing/sharing-32-01`(18 case, 32↔32 cross-process 양방향, 이름으로 여는 무관한 32-bit process, 3000 Signal stress), `sharing/resources-32-01`, `sharing/fence-events-32-01` 통과.

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

Legacy transition/UAV/aliasing barrier의 type·flag·state·subresource를 검사하고 기존 보수적 encoder 경계를 유지한다. 이 경계와 Metal synchronization이 실제 ordering을 담당한다. 완전한 resource-state debug tracker를 뜻하지 않는다. Enhanced barrier는 [P2-BARRIERS](#p2-barriers--enhanced-barriersdevice614commandlist59render-passgpu-upload-heap) 참조.

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

`D3D12_QUERY_HEAP_TYPE_SO_STATISTICS`와 `SO_STATISTICS_STREAM0..3`를 추가했다(P1-D). 결과는 16 B, Begin에서 recording별 stream counter를 snapshot하고 End에서 차이를 쓴다. Resolve는 8-byte 정렬 offset을 요구한다.

**`PIPELINE_STATISTICS` heap 생성과 88-byte Begin/End/Resolve를 지원한다.** MSAA 실행을 수정한 최종 격리 runtime에서 11개 필드와 독립 GPU oracle을 포함한 전체 fixture를 통과했다. 범위·비용·실행 증거는 위 P1-C를 참조한다.

## 9. Queue/Fence와 기존 증거의 범위

CPU/GPU rewind, API-time wait latch, ANY/ALL event registration, 실제 GPU completion 기반 consumer wait를 유지한다. SO 오류 전파 수정에서는 native event probe와 signal별 Metal event를 제거했다. Queue completion 스레드가 앞선 submission의 GPU 결과와 SO status를 검증한 뒤에만 논리 fence 값을 공개한다. CPU signal은 같은 논리 timeline mutex로 직렬화한다. Device 오류는 terminal UINT64_MAX로 대기를 종료하며 이후 signal로 rewind하지 않는다. Snapshot/staging/segment는 allocator와 native resource의 retirement 수명에 연결한다.

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
x86_64-w64-mingw32-clang++ -std=c++20 -O2 -Wall -Wextra -Werror -static \
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
- P1-A·P1-B·P1-C·P1-D(VS→SO), 일반 non-SO indexed restart, SO 오류 전파·WoW64 연결·ExecuteIndirect를 구현·검증했다. P1-C는 실제 MSAA pixel-frequency 실행 수정 후 11-field query를 공개했다. `AtomicCopyBufferUINT64`는 macOS Apple8+ 대상으로 구현했으며 실제 장치·reader 관찰 범위는 P1-A 결과에 명시한다. Pipeline library·tiled·shared·enhanced barrier·GS/HS/DS 등 P2 결과와 남은 제약·미검증은 [P2 완료 결과](#p2-완료-결과-2026-10-01)에 구분한다. P3의 검증된 DXR 1.1·FL12_1·conservative raster 1·feedback 1.0·view instancing 1과 GSTS/SO 후속 결과는 [P3 완료 결과](#p3-완료-결과-2026-10-01)에 있다. P3 전체 및 최종 64-bit/WoW64 통합 회귀 완료는 아직 선언하지 않는다.
- 실제 게임 정상 렌더링, 성능, 전체 D3D12/DXR conformance를 주장하지 않는다.
- 이후 검증을 진행할 때는 새 build/install과 **새 runtime 복사본·전용 prefix·새 evidence 경로**를 사용해야 한다. 기존 게임 또는 live prefix에는 배포하지 않는다.
