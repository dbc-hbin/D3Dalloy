# D3D12 GPU 병목 전수 조사 보고서

작성일: 2026-10-14
범위: 현재 `dxmt-d3d12` 작업 트리의 D3D12 실행·셰이더·Metal bridge·공유 리소스·전문 기능·검증 증거
성격: 읽기 전용 전수 inventory 및 정적 병목 감사. 제품 코드는 수정하지 않았다.

## 1. 조사 범위와 조사 깊이

이번에는 주요 파일 몇 개만 읽는 방식이 아니라, 관련 도메인의 파일 목록을 먼저 만들고 각 파일에 disposition을 부여했다.

| 도메인 | 파일 수 | 결과 | 깊이 |
|---|---:|---|---|
| `src/d3d12` | 85 | 관련 41, 비성능/ABI/빌드 44, 미읽음 0 | 모든 파일 열람, 큰 파일은 함수 목록 + GPU 관련 구간 집중 |
| `src/dxmt` + `src/dxgi` | 81 | 관련/공유 72, 비성능 5, 미읽음 0 | 모든 파일 분류; 3개는 partial-depth 명시 |
| `src/winemetal` | 41 regular files | D3D12 관련 32, 구조적 부분 조사 5, 비성능 4, 미읽음 0 | 321 unix-call slot, 64-bit/WoW64 table, Metal4 op space 추적 |
| `src/airconv` | 88 실제 파일 | GPU 코드 관련 다수, host/build 일부 분리, 미읽음 0 | 모든 파일 분류; 큰 translation unit은 emission site 중심 |
| `src/nvngx`, `src/nvapi`, `src/util`, `libs/DXBCParser` | 71 code/build files | 모두 disposition, 미읽음 0 | 대부분 full; util 4개/DXBC parser 일부는 symbol/section depth |
| `graphics-translator/tests` | 194 top-level + `dxil` 약 745 artifacts | suite/file family disposition | runner와 약 12개 fixture 본문 집중 |
| `scripts` | 23 | 모두 disposition | runner registry/oracle/docstring 중심 |
| `external`, `include/native`, `tests`, `QA`, `doc/mass-ulw`, `build/local` | inventory 및 증거 분류 | 도메인별 relevant/nonperf/retained/unread 구분 | artifact 전수 내용 해석은 아님 |

### 정확한 한계

“전수 조사”는 관련 파일을 누락 없이 inventory하고 GPU 경로를 모두 연결했다는 뜻이다. 모든 1,000개 이상의 DXIL fixture bytecode와 모든 큰 C++ 파일의 모든 줄을 동일한 깊이로 읽었다는 뜻은 아니다.

아직 본문을 읽지 않은 항목은 다음과 같이 명시한다.

- `graphics-translator/tests/dxil/`의 약 933개 DXIL/DXBC/HLSL artifact 본문: 이름·producer script·suite 연결만 조사.
- 컴파일된 fixture exe와 일부 tarball/binary artifact: 파일 inventory만 수행.
- `external/nvapi` 문서·샘플 프로젝트: include/reachability 중심.
- `src/winemetal`의 큰 네이티브 파일 중 순수 CPU helper 일부: 함수/handler 구조와 GPU 경계는 조사했으나 모든 helper body line-by-line은 아님.
- `src/airconv` 큰 translation unit: 모든 GPU emission family와 caller는 조사했으나 모든 host-only 보조문까지 line-by-line은 아님.

## 2. 가장 중요한 구조적 사실

### 2.1 D3D12는 `src/dxmt`의 구형 command queue를 사용하지 않는다

`src/dxmt/dxmt_context.*`, `dxmt_command_queue.*`, `dxmt_command_list.*`, `dxmt_staging.*`, `dxmt_binding_set.hpp`, `dxmt_scaler.*`, `dxmt_shader_cache.*` 등은 D3D11 경로다. `src/d3d12`에서 이 encoder stack으로 가는 참조는 확인되지 않았다.

D3D12는 별도의 모델을 사용한다.

```text
D3D12 command list
  -> src/d3d12/d3d12_command_encoder.hpp EncoderData
  -> d3d12_command_queue.cpp SubmissionPlan
  -> Metal4 command buffer / encoder
  -> src/winemetal/unix/winemetal_metal4.c
  -> Metal 4 / MetalFX / RT native operation
```

따라서 D3D11의 `BindingSet`, `CommandChunk`, `EncodingThread` 최적화를 D3D12 FPS 해결책으로 재사용하면 안 된다. D3D12와 실제로 공유되는 것은 `dxmt_buffer`, `dxmt_texture`, `dxmt_allocation`, `dxmt_format`, `dxmt_presenter`, fence timeline, ring allocator, 일부 internal Metal library다.

### 2.2 Metal bridge는 세 단계 ABI다

```text
winemetal.dll PE thunks
  -> WINE_UNIX_CALL(index, POD)
  -> __wine_unix_call_funcs[321] / wow64 table
  -> native handler
  -> Metal4 command buffer / encoder / MetalFX / RT
```

`WMTMetal4Call`은 typed operation 314를 사용하고, pipeline/query/FX/FG/RT가 별도 operation range로 나뉜다. 모든 encoder command는 POD linked list를 C native walker가 순회한다. 따라서 CPU 비용 후보는 “D3D12에서 command node를 몇 개 만들었는가”와 “native walker가 node마다 Objective-C message/residency/table lookup을 몇 번 하는가”를 분리해서 봐야 한다.

## 3. GPU 작업과 대기 경계 전체 지도

### D3D12 core

- Render/compute/blit/clear/resolve encoder: `src/d3d12/d3d12_command_queue.cpp`
- Command recording, root arguments, descriptors, copy, barriers: `src/d3d12/d3d12_command_list.cpp`
- Native submission commit/retirement: `d3d12_command_queue.cpp:CommittingScope`, completion thread
- Common fence wait/update: render/compute/blit/resolve/clear/auxiliary encoder paths
- CPU waits: present ordering, submission ring retirement, work graph readback, RTAS/sparse operations, frame-latency credits, Reflex sleep
- GPU-specialized work: atomic64 import/export/clear, MSAA capture/replay, predication, mesh/tessellation, indirect resolver, query resolve, temporal/FG, RTAS, work graph

### Shared DXMT/DXGI

D3D12-reachable:

- `dxmt_presenter.cpp`: present PSO build, layer property sync, drawable encoding
- `dxmt_texture.cpp`, `dxmt_buffer.cpp`: native allocations, view creation, MSAA auxiliary textures, sparse/renamable resources
- `dxmt_format.cpp`: format queries and size/conversion decisions
- `dxmt_fence.cpp`, `dxmt_queue_order.hpp`: logical fence completion and CV waits
- `dxmt_ring_bump_allocator.hpp`, `dxmt_command_clear.hpp`, `dxmt_command.metal`
- `dxgi_factory/output/adapter/options/memory`: adapter/pacing/budget/notification boundaries

D3D11-only and not part of D3D12 FPS path:

- `dxmt_command_queue.cpp`, `dxmt_context.cpp` main encoder stack
- `dxmt_staging`, `dxmt_binding_set`, `dxmt_scaler`, `dxmt_occlusion_query`, D3D11 dep tracking paths
- D3D11 timestamp dummy-blit/barrier behavior

### WineMetal native

- Per-command linked-list walk and Objective-C message dispatch
- Metal4 `m4_table`, `m4_bind_buffer`, `m4_allocation`, residency set tracking
- Render/compute/blit encoder stage fence/barrier calls
- Commit, submission, completion, drawable/present reservation
- MetalFX SR/RR/FG, FSR kernel, RTAS prepass, RT function tables
- Pipeline/library/function creation and query/timestamp/visibility operations

### AIRConv / final shader

All major families were traced:

- DXBC SM4/5 sampling, raw/structured buffers, atomics, interpolation, tessellation, GS/SO, MSAD
- DXIL resource operations, null descriptors, guarded access, sampling/gradients/feedback, barriers, atomics
- Device i64 atomic cell protocol
- Wave32/64/128, prefix/multi-prefix, helper lane, wide-wave scratch/barrier
- FP64 integerized helpers and matrix/wave matrix
- Mesh/amp, work graph, ray query/RT pipeline, OMM
- MSAA pixel/vertex/mesh/coarse paths
- implicit simdgroup barrier and 16-bit texture lowering
- linked helper shaders for denorm, atomic64, conservative raster, sample positions, work graph, sampler feedback, tessellation, MSAD

## 4. 후보 순위

### A. 실제 GPU/동기화 설계 후보

#### A1. `merge_eligible`와 `access_complete`를 분리하고 실제 dependency로 barrier 계산

근거:

- `src/d3d12/d3d12_command_queue.cpp:84-90,138-171,180-215,5408-5467`
- `:6850-6883`, `:7000-7018`
- `SubmissionNode.accesses/dependencies`, allocation별 writer/reader frontier
- native bridge `src/winemetal/unix/winemetal_metal4.c:1518-1522`

현재는 pass merge가 불가능한 조건이 access analysis까지 opaque로 만들 수 있고, encoder 사이의 공용 fence wait/update가 실제 RAW/WAR/WAW보다 넓은 범위의 직렬화를 만들 수 있다.

설계 방향:

1. merge 가능 여부와 access completeness를 별도 field로 둔다.
2. replay된 native encoder 순서 기준으로 RAW/WAR/WAW dependency를 재계산한다.
3. dependency가 증명된 경우에만 stage-aware native barrier를 사용한다.
4. unknown→known, known→unknown, sparse alias, UAV/alias, cross-queue Wait를 모두 보수적으로 연결한다.
5. 분석이 불완전한 encoder는 기존 fence fallback을 유지한다.

중요한 반증/주의:

- `baseline-render`는 동일 fixture에서 `wrong native fence stage`, `native_render_begin=2` 대 expected 1, `members=[1,1]`를 기록한 실제 FAIL이다.
- `QA/render-fragment-barrier-contract/observed-failure.txt`에는 실제 WAW edge 후 잘못된 merge가 native barrier assertion으로 이어진 증거가 있다.
- 따라서 이 후보는 “fence를 없애면 빨라진다”가 아니라 **현재 barrier/merge contract가 이미 틀릴 수 있으므로 먼저 correctness를 고쳐야 하는 후보**다.
- `final-render`가 green이라는 사실도 도시 FPS 증거가 아니다. baseline과 final runtime hash가 다르므로 runtime-level A/B로만 취급한다.

판정: **확정된 correctness/synchronization 결함 + 성능 후보. 최우선이지만 먼저 red fixture를 고쳐야 한다.**

#### A2. reserved/tiled `AnyLive`와 sparse alias 보수 모드

근거:

- `d3d12_reserved_resource.hpp:92-104`
- `d3d12_command_list.cpp:6485-6504`
- `d3d12_command_queue.cpp:5400-5453,5872-5879`

reserved/tiled resource가 살아 있으면 merge/access analysis와 alias flush가 보수적으로 동작할 수 있다. 하지만 `baseline-tiled`와 `sparse-tiled` fixture가 numeric contract FAIL이고, 도시에서 실제 `AnyLive`가 활성화됐다는 증거는 없다.

판정: **조건부 GPU/동기화 후보. 도시 trace에서 `sparse_alias`, alias flush, opaque node를 확인하기 전에는 구현하지 않는다.**

#### A3. Resolve 별도 pass와 attachment lifetime/store

근거:

- `d3d12_command_list.cpp:9455-9600,10110-10270`
- `d3d12_command_encoder.hpp` resolve attachment fields
- queue resolve encode `:7464-7506`

resolve를 producing render pass에 접거나 마지막 consumer가 없는 attachment를 DontCare store로 만들 수 있는 경로가 후보지만, partial resolve/depth/stencil/predication/suspend/alias 계약을 보존해야 한다.

판정: **확정된 구조적 extra-pass 후보, 실게임 activation 미확인.**

#### A4. Predication/atomic64/MSAA/mesh/RT/work graph 내부 readback

D3D12 scan에서 다음 CPU↔GPU round trip이 확인됐다.

- work graph node별 `waitUntilCompleted`/readback: `d3d12_work_graph.cpp:927,946-951`
- atomic64 import/export/clear: `d3d12_atomic64_phase.cpp`, queue atomic phase
- MSAA producer/preflight waits: queue `:5949,6007` 등
- RTAS/sparse/volume operations의 event waits
- tessellation/geometry/SO/mesh replay and preflight

각각은 실제 해당 기능을 쓰는 장면에서만 병목이다. fixture activation은 있지만 ZZZ 도시 activation은 없다.

판정: **조건부 높은 위험 후보. 기능별 trace counter가 먼저다.**

### B. Shader-generated GPU cost 후보

#### B1. DXIL/DXBC guarded resource access

- `src/airconv/dxil_resources.cpp:803-829,864-921`
- `src/airconv/nt/dxbc_converter_base.cpp:2738-2896,3555-3565`

component별 bounds branch/phi/load가 생성된다. aligned full-vector access만 fast path로 합칠 수 있다. OOB, unaligned, coherent, sparse, atomic은 기존 의미를 유지한다.

판정: **실제 shader가 hot path에서 사용할 때의 GPU 후보. 현재 게임 shader activation 미확인.**

#### B2. Null descriptor guard 중복 여부

- `airconv_context.cpp:79-122`
- `dxil_resources.cpp:4213-4263`

두 단계가 동일한 guard를 생성할 가능성은 있지만 LLVM/Metal 최적화가 제거할 수 있다. 최종 AIR/ISA를 보기 전에는 중복 비용을 확정하지 않는다.

#### B3. Wave/wide-wave/prefix/implicit barrier

- `dxil_operations.cpp:1771-2160`
- `dxil_matrix.cpp:151,321,487-496`
- `dxil_mesh.cpp:633-806`
- `dxil_workgraph.cpp:634-948`
- `transforms/simdgroup_implicit_membarrier.cpp:14-59`

wide wave scratch/barrier, lane-loop expansion, mesh/workgraph barriers, groupshared synchronization이 포함된다. 조사 과정에서 확인한 pass는 모든 store 뒤 무조건 barrier를 넣는 것이 아니라 MemorySSA Store→Load 관계를 찾고 store별 중복을 방지한다.

판정: **shader activation 및 AIR/ISA 확인 전에는 보류.**

#### B4. Atomic64 shader protocol

- `dxil_atomic64.cpp:21-36,80-333`
- `d3d12_atomic64_phase.cpp`
- `air_atomic64.metal`

module-level atomic64 detection이 resource access와 replay path를 확장할 수 있다. atomic64 조건을 삭제하는 것은 올바른 최적화가 아니다. atomic resource/range/stage를 더 세밀하게 전달하는 설계가 필요하다.

판정: **정확성 위험이 큰 조건부 후보.**

### C. CPU 제출/생성/시작 비용 후보

- root/static sampler/vertex table upload와 descriptor view pool creation
- `CaptureSubmissionAccess` per-draw scan
- native `m4_allocation`/table sweep
- D3D12 DRED breadcrumb walk와 submission planning allocation
- `MTLQueryDXGIFormat` 반복 호출
- MSAA auxiliary texture/view creation
- lazy graphics variant/PSO creation lock
- pipeline/stage/function cache/disk I/O
- NGX parameter string hash/temporary allocation
- FSR/FG/RR setup and present-thread PSO creation

이 중 cache/PSO artifact는 주로 startup/first-use hitch이고, per-frame GPU 병목과 분리해야 한다. CPU 제출 후보는 GPU critical path가 확인된 후 우선순위를 정한다.

### D. Present/temporal/MetalFX 후보

`dxmt_presenter.cpp:199-204`의 layer property 변경 시 GPU 완료 대기, `:327-386`의 synchronous present PSO 2개 생성, `d3d12_swapchain.cpp` frame-latency credit, `d3d12_command_queue.cpp:8213` present order wait가 후보이다.

MetalFX backend는 SR/RR/FG 모두 실제 GPU 작업을 수행하므로 “frontend는 GPU 비용이 없다”와 “backend feature는 비용이 없다”를 혼동하지 않는다. 다만 현재 city run에는 matching activation trace가 없다.

## 5. 검증 증거와 반증

### 이미 있는 강한 correctness 증거

- `render-passes`: 38-case graph/frontier/clear matrix
- `enhanced-barriers`: 26 cases + invalid barrier cases
- `multiqueue`: 64 execute, 64 signal, 63 wait, 128 submit/complete contract
- tiled, predication-indirect, multisample, atomic64, raytracing, bindless, temporal, FSR/FG, present-order, view-pool, binding-reuse fixture families
- runner는 fixture의 PASS 문자열만 믿지 않고 GPU readback을 다시 계산한다.

### 실제로 남은 문제

- `baseline-render/result.json`: wrong native fence stage, native render 2 vs expected 1
- `QA/render-fragment-barrier-contract/observed-failure.txt`: WAW edge 후 merge와 native barrier assertion
- `baseline-tiled`/`sparse-tiled`: tiled numeric contract FAIL
- 동일 scene `OWNED_GRAPHICS_TRACE` 없음
- 동일 scene GPTK/D3DMetal trace 없음
- candidate별 A/B 없음
- 일부 runtime hash가 다르므로 fixture green/red를 단순 source regression으로 해석할 수 없음

### 최우선 다음 probe

```text
OWNED_GRAPHICS_TRACE=1
OWNED_GRAPHICS_TRACE_LIMIT=200000
```

같은 도시 구간에서 다음을 frame ID로 join한다.

- `submission_access_edge`
- `submission_access_opaque`
- `submission_render_merged`
- `native_render_begin`
- `native_render_wait/update` stage
- `ResolvePass`
- `msaa_producer_wait`
- `present_request/present_encode/complete`
- Metal System Trace per-encoder interval

## 6. 구현 우선순위

1. **fence/merge correctness red를 먼저 해결**한다. 현재는 성능 최적화 이전에 stage/barrier contract가 깨질 수 있다.
2. city trace에서 `sparse_alias`, MSAA/atomic64/mesh/temporal/resolve가 실제 활성화되는지 확인한다.
3. 활성화된 경우에만 dependency-based barrier redesign, resolve/store lifetime, specialized replay를 각각 하나씩 구현한다.
4. shader AIR/ISA에서 guarded access, wave expansion, implicit barrier가 실제 hot shader에 있는지 확인한다.
5. CPU submission/view/PSO/cache 후보는 GPU critical path와 분리해 측정한다.
6. 마지막에 동일 scene/settings의 pristine D3DMetal trace와 matched run을 비교한다.

## 7. 최종 판단

이번 전수 조사로 “어디를 읽었는가”는 이전보다 훨씬 명확해졌다. 관련 도메인의 파일 inventory와 GPU path graph는 완성됐고, 대형 파일과 fixture artifact의 read depth도 표시했다.

현재 가장 강한 actionable finding은 **queue-wide fence 자체가 곧 FPS 병목이라는 주장**이 아니다. 더 정확한 결론은 다음이다.

> 현재 queue의 dependency/merge/fence contract에 실제 red correctness evidence가 있다. 이 구조를 올바르게 고친 뒤에야 불필요한 GPU 직렬화와 pass 증가를 성능 후보로 평가할 수 있다.

그 다음이 attachment lifetime/resolve, 실제 활성화된 shader lowering, feature-specific readback이다. 도시 trace 없이 어느 것이 FPS를 지배한다고 단정하지 않는다.
