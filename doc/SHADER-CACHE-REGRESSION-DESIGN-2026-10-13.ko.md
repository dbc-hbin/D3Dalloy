# 셰이더 프리컴파일 회귀와 생성 결과 재사용 설계

상태: 설계 구현 완료 및 검증 기록. 현재 파싱 수정과 캐시 변경을 함께 설명하며, 게임 FPS 개선 자체는 아직 입증하지 않는다.

## 1. 결론과 이전 설명 정정

함수 실행 결과를 캐싱한다는 방향은 맞다. 이미 성공 결과를 캐싱하는 계층은 유지하고, 실제 반복되는 실패와 native PSO 생성을 추가로 줄인다. 모든 LLVM 함수의 실행을 일반적으로 memoize하는 설계가 아니다.

- 과거 Wine 커스텀에는 최종 PSO 공유뿐 아니라 `ExtractFunctions`와 `LoadGraphicsFunctions`를 통한 함수 추출 결과 캐시도 있었다. 직전 설명은 후자를 빠뜨렸다.
- 현재 DXMT의 `WMT4::Function`은 Metal4 function descriptor이다. D3DMetal의 `MTLFunction` 추출 비용과 동일하지 않다. 일반 `newFunction` 캐시는 descriptor 생성과 이름 확인을 줄이며, 비싼 native 컴파일은 최종 PSO 생성 호출에 있다.
- 전체 PSO blob 적중은 AIR/상수/relocation 재사용이다. 완성된 native PSO 적중이 아니다.
- stage artifact 적중이면 `lower` 자체를 호출하지 않으므로 파싱과 함수 순회도 생략한다. 다른 PSO라는 이유만으로 매번 순회하지 않는다. AIR에 영향을 주는 입력이 바뀌거나 실패 결과가 보존되지 않을 때 다시 실행한다.
- 반복 컴파일 실패는 확정된 중복 작업 후보다. 그것이 지속적인 저 FPS의 주원인인지는 아직 측정하지 않았다.

## 2. 과거 세션과 실제 소스

### 2.1 확인한 이력

- 2026-09-30/10-01, 세션 `01a0f313-b929-70f0-b84e-69ddf7458ab9`: PSO/함수 캐시의 강한 참조가 메모리 압박을 만들 수 있어 약한 참조로 바꾼 기록.
- 2026-10-03, 세션 `01a10059-cb6c-7381-8220-b81a1fc8ee3d`: 바깥 stage 훅 4개는 복원하지 않고 함수 추출 훅 2개를 복원. Compute 경쟁 패배 table의 88바이트 누수는 별도 수정.
- 확인한 커밋: `efc46bb5 Restore selective function caching and fix compute stage loser leak`.
- 현재 DXMT에는 `cb011ec`, `5aafa43`으로 stage/native library/reflection/RT/WG 재사용이 들어왔고, `9668acb`는 stage 디스크 캐시의 반복 디렉터리 스캔을 줄였다. 이 기능을 새로 중복 구현하지 않는다.

과거 구현의 확인 경로는 `/Users/hanbinnoh/Documents/zzz-wine-yaagl-pr-runtime/` 아래이다.

| 근거 | 확인 내용 |
|---|---|
| `d3dmetal-pso-cache/bridge.mm:163-241` | 함수 추출 원본 호출을 감싸고 결과 재사용; graphics/compute 문맥과 device 소멸 연결 |
| `d3dmetal-pso-cache/function-hooks.mm:22-118` | device/library/호출 조건/helper/상수의 실제 값으로 키 작성; 적용 불가 경로는 원본 수행 |
| `d3dmetal-pso-cache/function-cache.mm:100-220,233-421` | 동일 키 동시 생성 병합; 살아 있는 함수의 약한 참조; 실패와 재진입 처리 |
| `d3dmetal-pso-cache/key.mm:439-531` | Metal4 descriptor 주소가 아니라 library/name과 실제 pipeline 필드를 판정 |
| `scripts/test-function-cache.py:175-304` | 수명/장치 종료/상수 구분/실제 GPU 실행 검증 코드 |

당시 세션에 기록된 반복 추출 측정은 단순 Pixel 약 0.875us, 테셀레이션 helper 63.2us에서 0.247us, stream-output 형태 114.6us에서 0.328us이다. 이는 당시 원본 D3DMetal을 호출한 테스트 셰이더의 기록이며 ZZZ FPS 측정이나 이번 DXMT 측정이 아니다. 이번 조사에서는 그 실험을 재실행하지 않았다.

### 2.2 가져올 것과 가져오지 않을 것

가져올 것은 정확한 의미 기반 키, 동일 입력 동시 요청 병합, 살아 있는 결과 공유, 입력 자원 수명 유지, device 소멸 정리, 실제 GPU 결과 검증이다.

바이너리 오프셋, TLS 호출 위치 판정, 후킹 코드, Objective-C private weak ABI, 바깥 stage 캐시를 하나 더 만드는 코드는 가져오지 않는다. owned source에서는 직접 타입과 호출 인자를 사용한다. 당시 88바이트 누수는 그 바이너리 고유 경로이며 현재 DXMT에도 존재한다고 간주하지 않는다.

## 3. 현재 코드베이스 대조

| 계층 | 현재 구현과 근거 | 보완 판정 |
|---|---|---|
| reflection/argument bindings | `d3d12_pipeline_cache.cpp:779-812`, source bytes + AIRCONV_VERSION | 성공 캐시 유지 |
| root별 ROV marker | `d3d12_pipeline_cache.cpp:814-829` | 기존 캐시 유지 |
| ordinary/linked stage | `d3d12_pipeline_cache.cpp:841-939`, exact sources/family/entry/args/native identity/version | 성공 캐시 유지; 확정된 binding 실패 추가 |
| WG/RT compiler artifact | `d3d12_pipeline_cache.cpp:685-777` | 기존 캐시 유지; ordinary 오류 수정에 묶어 확장하지 않음 |
| native library | `d3d12_pipeline_cache.cpp:966-988`, compiler handle + metallib bytes | 기존 캐시 유지 |
| sample-mask function | `d3d12_pipeline_cache.cpp:990-1006` | 기존 특수화 재사용 유지 |
| 일반 function descriptor | graphics `:1744,1892`; compute `:130,223` | 일반 entry도 같은 device cache로 공유 가능; 단독 FPS 효과를 가정하지 않음 |
| 최종 ordinary render/compute PSO | graphics `:1947,1970`; compute `:145` | 의미 기반 native key와 weak completed cache로 서로 다른 PSO 객체도 공유 |
| ICB/index-pull/statistics/color-mask | graphics `:2116-2374`; compute `:172-236` | 같은 device cache의 native function/PSO 재사용으로 연결 |
| MSAA/mesh variants | graphics `:794-1190,2379-2810` | object-local maps/attempt flags와 stage 캐시 유지; native 공유 연결 |
| RT/WG native library | state object `:831`; work graph `:827` | compiler artifact 적중 후에도 기존 library 캐시 우회. 해당 경로 사용이 확인되면 기존 Library로 연결 |
| RT/WG native PSO | state object의 linked compute, work graph `:829` | linked functions/stack depth가 키에 들어가기 전에는 ordinary cache와 합치지 않음 |
| 내부 helper pipelines | command list/allocator/queue/atomic64 phase의 자체 보관 | 매 draw 재생성이라고 간주하지 않음; 실제 반복 생성이 잡힌 helper만 연결 |
| 전체 PSO automatic cache | `d3d12_pipeline_cache.cpp:1257-1335` | blob/동시 결과 공유에 native function/PSO single-flight를 추가 |
| 명명 pipeline library | `owned-d3d12-pipeline-library.cpp:417-420,590-592` | 같은 이름의 COM 객체는 이미 재사용. 다른 CreatePSO 호출과 구분 |

`ArtifactCache<T>`는 각 종류별 256개/32MiB 상한과 generation/disable/clear를 갖는다(`d3d12_pipeline_cache.cpp:493-653`). native library/function의 `size()`는 0이며 키 바이트만 계산하므로 32MiB가 native GPU 메모리 상한이라는 뜻은 아니다.

Metal4 경계의 실제 의미: `unix/winemetal_metal4_pipeline.c:49-73`은 library function/specialization descriptor를 만들고, `:75-151`에서 native compute/render/mesh PSO 컴파일을 호출한다. 최종 캐시 키 설계는 이 변환과 함께 검토한다.

## 4. P0: 오류의 원인을 먼저 분리

대표 캡처는 `build/local/shader-capture-20261012/live-11293/failures/11293-PS-e63d9184c814140ed9fe9c8926c86ad3e331095550909a2fa253def5c67cd6dc.failure`이다. explicit root가 sampler s0..6을 제공하고 셰이더 메타데이터는 s0..8을 선언하며, replay에서 sampler binding 실패를 재현했다. 이는 입력 조합과 현재 compiler가 실패한다는 증거이지, 게임이 잘못됐다는 증거가 아니다.

두 가능성을 모두 검사한다.

1. 게임이 전달한 root와 compiler가 받은 root가 달라졌거나 잘못 선택되었다. CreateRootSignature 원본 blob, CreatePSO의 실제 explicit/embedded 선택, compiler 인자의 길이와 hash를 같은 요청에 연결한다.
2. 입력 전달은 맞지만 사용하지 않는 함수/분기/handle 경로까지 lowering하여 binding을 과잉 요구한다. 실패하는 resource consumer부터 entry/helper 호출과 제어 흐름을 역추적한다.

`LowerResources`는 모든 defined function/block의 resource 호출을 수집한다(`dxil_resources.cpp:4333-4387`). entry에 createHandle(s7)이 있다는 사실만으로 동적 사용을 입증할 수 없다. select/phi의 양쪽 추적과 dead handle 정리 순서도 검사한다. CFG에서 실제 제외되는 경로만 제거하며, root에서 빠진 리소스를 임의로 null로 바꾸거나 오류를 성공으로 취급하지 않는다.

현재의 bounded failure recorder(`d3d12_shader_failure.hpp:29-51`)를 확장하여 full stage-key digest와 options/entry/resource class/id/register/space/visibility를 연결한다. 기존 64개 한도를 유지하고 별도 캡처 시스템을 만들지 않는다. full key를 만들 수 없는 요청은 완전한 replay라고 표시하지 않는다.

공개 규약과 owned 코드만으로 계약이 모호해지면 CORE에 기록된 pristine GPTK4 reference의 version/hash를 검증하고 같은 입력을 비교한다. reference 동작은 호환성 근거이며 production dependency나 proprietary 코드 이식으로 쓰지 않는다.

완료 기준: 대표 실패가 root 전달 문제인지 과잉 binding인지 구분되고, 적용한 원인 수정이 해당 replay와 실제 GPU readback을 통과한다. 정당한 미바인딩 resource 사용은 여전히 실패해야 한다. 현재 metadata/tag 파싱 수정이 이 binding 오류까지 해결했다고 주장하지 않는다.

## 5. P1: 확정된 컴파일 실패도 결과로 공유

ordinary stage cache는 typed root-binding 실패도 artifact로 보존하며, producer는 원래 오류를 유지하고 waiter는 clone으로 같은 실패를 받는다(`d3d12_pipeline_cache.cpp:935-943`). 전체 PSO의 동시 실패 HRESULT 공유(`:1303-1309`)와 별개로, 나중의 반복 요청과 다른 전체 PSO에 속한 같은 stage도 보호한다.

- 성공 artifact와 typed deterministic root-binding failure를 동일한 기존 full stage key/flight에서 판정한다.
- 실패는 현재 compiler와 해당 입력에서의 결과다. D3D 입력의 유효성 판정으로 확대하지 않는다.
- 문자열 검색 대신 binding 오류 생성 지점에서 kind를 지정하고 LLVM Error -> SM50ErrorInternal 경계를 지나도 보존한다.
- 캐시는 불변 오류 payload를 소유한다. caller마다 별도 해제 가능한 오류 handle을 복제하여 반환한다. 손실 없이 복제하는 API 하나를 선택하고, 생성 API까지 동시에 추가하지 않는다. kind 조회와 복제의 필요한 PE/Unix ABI 연결은 함께 변경한다.
- `SM50GetErrorMessageString`은 256바이트 임시 버퍼를 쓰므로 캐시 payload 복사에 사용하지 않는다. 원본 handle의 전체 메시지를 보존하는 복제를 사용한다.
- OOM/native compiler/device/disk 오류와 아직 분류하지 않은 오류는 영구 실패로 보관하지 않는다. 초기화 실패까지 같은 key를 적용하려면 별도 증거와 테스트가 필요하다.
- RAM의 기존 stage 상한과 generation/disable/clear를 적용한다. 실패는 디스크에 쓰지 않는다. 성공용 metadata 검증과 disk load를 건너뛰지 않는다.
- 캐시 retention 실패는 기존 compiler 결과를 보존한다. 보관되지 않는 경우 재컴파일이 생길 수 있으며, 보관되는 정상 경로에서만 한 번 실행을 보장한다.

관련 변경 범위: `dxil_resources.cpp`, 오류 flattening/handle 정의를 가진 `dxbc_converter.cpp`/`dxil_converter.hpp`, `airconv_public.h`, `winemetal/airconv_thunks.c/.h`, `winemetal/unix/winemetal_unix.c`의 native/32-bit wrappers와 양쪽 call table, `d3d12_pipeline_cache.cpp/.hpp`, 기존 CPU+GPU fixtures. 기존 절대 call index를 밀지 않으며 새 operation의 배치는 현재 call 표에서 확정한다.

## 6. P2: 완성된 native PSO 공유

### 6.1 descriptor identity부터 안정화

기존 device-owned function cache를 일반 entry descriptor에도 확장한다. library handle + entry name + 존재 여부를 포함한 상수 index/type/정확한 값으로 구분한다. 기존 sample-mask 규약과 root별 분리를 유지한다. 함수를 보유하는 artifact는 key의 library 수명을 유지한다. 같은 descriptor 결과를 공유해도 생성 이후 수정하지 않는다.

ordinary render/compute를 먼저 연결한 뒤 그 경로의 ICB/statistics/index-pull/color-mask를 같은 생성 진입점으로 연결한다. mesh/MSAA도 동일 원칙으로 이어지되 기존 object-local lock/attempt semantics는 유지한다. RT/WG는 일반 shader 경로에서 관측되지 않았다면 먼저 만들지 않는다.

### 6.2 최종 PSO key

`WMT*PipelineInfo` 전체 메모리를 그대로 hash하지 않는다. padding, pointer, 주소 재사용이 false miss 또는 잘못된 공유를 만든다. kind + compiler identity + 각 의미 필드와 살아 있는 function artifact identity를 명시적으로 직렬화한다. 키의 parent artifacts는 결과 bundle이 함께 보유한다.

- Compute: compute function, ICB 지원, threadgroup-width 조건, stack depth. linked compute는 ordered linked-function 목록까지 필요하며 초기 ordinary cache에서 제외한다.
- Render: VS/PS, color 8개의 format/blend/write mask, alpha-to-coverage, rasterization/sample count, topology, ICB, vertex amplification, color-attachment mapping 등 생성 인자.
- Mesh: object/mesh/PS와 위 color/raster 조건, payload length와 object/mesh threadgroup 조건.
- bridge가 아직 소비하지 않는 depth/stencil/tessellation/immutable-buffer 필드도 처음에는 보수적으로 구분한다. backend 변경이 동일 키를 다른 뜻으로 바꾸지 않도록 키 작성과 native 변환을 같이 수정한다.
- binary archive 조회/serialization side effect가 있는 요청은 초기 공유 대상에서 제외한다. 단순 handle 공유로 그 side effect를 생략하지 않는다.
- 서로 다른 root에서 결과 bytes가 같더라도 PSO의 runtime root/bindings/constants 저장소를 합치지 않는다. native pipeline의 생성 의미가 동일할 때만 native 결과를 공유한다.

### 6.3 소유권과 메모리

COM PSO 객체는 매 요청 따로 만든다. root bindings, constant buffer, statistics, view/variant bookkeeping도 객체별로 유지한다. 공유하는 것은 immutable native pipeline 결과와 그 parent artifacts뿐이다.

과거 캐시의 메모리 회귀를 반복하지 않도록 최종 PSO의 완료 항목은 약한 참조로 둔다. C++에서 `shared_ptr` bundle을 PSO 소유자가 보유하고 device map은 `weak_ptr`로 조회하는 방식으로, Objective-C private weak ABI 없이 구현한다. pending flight와 waiter가 보유한 결과는 강한 참조로 유지하고, 완료된 map은 결과를 강하게 붙잡지 않는다. live owner가 없어졌으면 다음 요청에서 재생성한다.

기존 캐시의 full-key/single-flight/generation 논리를 재사용하되 completed retention만 native 결과에 맞춘다. 캐시 전체를 새 framework로 다시 쓰지 않는다. 메타데이터 entry 수는 기존 256개 상한을 사용하고 pending flights는 완료 전 eviction하지 않는다. eviction/clear/device 소멸은 lookup membership만 제거하며, GPU retirement까지 이미 보유된 PSO 참조를 무효화하지 않는다.

native 생성은 metadata lock 밖에서 수행한다. 키가 달라도 같은 compiler에 대한 기존 생성 직렬화가 필요한 곳은 별도 증거 없이 제거하지 않는다. native 실패는 immutable error를 flight가 보유하고 각 waiter에 유효한 참조로 전달한다. 이후 요청에서는 재시도하며 디스크에 native handle을 저장하지 않는다.

## 7. P3: 불변 분석 결과 캐시 - 비용을 확인한 뒤

현재 `PrepareShader -> ParseMetadata`와 `LowerShader -> ModuleInfo::Parse`는 각각 bitcode를 파싱한다(`dxil_converter.cpp:305,388`; `dxil_metadata.cpp:1231-1272`). reflection hit 뒤 새로운 AIR 입력에서 `InitializeCompiler`가 다시 실행될 수 있다(`d3d12_pipeline_cache.cpp:831-855`). stage hit 뒤에는 이 작업이 발생하지 않는다.

이를 모든 함수의 공통 캐시로 덮지 않는다. P0/P1/P2 뒤 남은 parse/metadata 비용을 분리해 계측한다. 비용이 남으면 source bytes + entry + AIRCONV_VERSION으로 pointer-free immutable ShaderMetadata/resource declaration/원본 분석 결과만 재사용하고 새 compilation의 bookkeeping에 복사한다.
현재 `ShaderMetadata`에는 `ResourceBinding::symbol`이라는 `llvm::Value *`가 포함된다(`dxil_converter.hpp:253`). `ParseMetadata`도 resources를 그대로 move한다(`dxil_metadata.cpp:1324`). 따라서 현 struct 전체를 pointer-free 결과로 간주하지 않는다. 캐시에 보존하는 projection에서 symbol을 제외하고, 새 IR에서 필요한 symbol은 그 IR에 다시 연결한다.

`ModuleInfo`, LLVMContext/Module/Function/Value, mutation 이후 heap-use 분석, `SM50ShaderInternal` 전체, body emitter/handler를 그대로 공유하지 않는다. LowerShader는 실제 변환할 module이 필요하므로 metadata 캐시만 추가해도 두 번째 bitcode parse가 사라지지는 않는다. mutable IR 공유 또는 다른 context로 단순 clone하는 해법을 선택하지 않는다.

진단/변환은 선택된 entry와 필요한 helper를 기준으로 하며 새 전수 순회를 매 요청 추가하지 않는다. 기존 resource lowering 순회가 잘못된 binding을 만드는 경우 P0에서 그 원인을 수정한다. 독립적인 loop fusion/새 callgraph cache는 프로파일로 남은 비용이 확인될 때만 수행한다.

## 8. 검증과 수용 기준

### 8.1 기능/동시성

기존 `pipeline-cache-locks.test.cpp`/`run-pipeline-cache-cpu.py`에서 성공+typed 실패 single-flight, 다른 root/args/entry/key 분리, clear/disable/old-generation completion, eviction, transient retry를 검사한다. barrier/event/condition을 사용해 정확한 flight에 waiter가 붙은 뒤 producer를 완료하며 sleep으로 동시성을 맞추지 않는다.

실제 airconv를 연결한 회귀는 caller별 오류 해제 순서, 긴 오류 메시지 복제, kind 보존, dead helper/dead branch/select/phi와 실제 미바인딩 consumer를 검사한다. 캐시 stub 검사만으로 compiler 계약을 통과했다고 하지 않는다.

기존 `owned-d3d12-pipeline-library.cpp`/`owned-d3d12-pipeline-cache.cpp`에 서로 다른 COM 객체의 동등한 native 생성, descriptor field 하나씩 변경, 같은/다른 specialization, parent library eviction, clear 뒤 기존 객체 사용, owner 전부 해제 후 재생성, ICB/statistics/mesh/MSAA 분리를 추가한다. native 생성 호출 횟수와 GPU readback 둘 다 검사한다. RT function tables/셰이더 identifiers/dispatch-local state는 공유하지 않는다.

### 8.2 버전과 disk

구현 후 `AIRCONV_VERSION`은 42이다. parser/lowering 동작 변경에 맞춰 올렸다. graphics/stage key와 달리 compute PSO description key는 이를 직접 넣지 않으므로 native image UUID 검증을 함께 확인하고, 명시적인 이전 blob 거부가 필요하면 기존 `kVersion`도 올린다. 독립적인 새 version 체계는 만들지 않는다.

실패가 disk에 없는지, 성공 stage의 output metadata/constants/relocations가 그대로 복원되는지, explicit CachedPSO 엄격 검증과 automatic 재시도가 유지되는지 검사한다. `9668acb`의 directory accounting/rescan 회귀도 유지한다.

### 8.3 실제 게임 성능

기존 `pipeline_artifact_operation/hit/wait`와 `pipeline_compute_timing`을 확장해 full-key별 compile attempts, deterministic failure hits, native descriptor 생성과 final PSO 생성/적중/대기/소요 시간을 분리한다. verbose 전수 로그를 성능 측정 중 켜지 않는다.

같은 런타임/씬/카메라/해상도/upscaler/프레임 제한에서 다음을 분리 비교한다.

1. 완전히 새 전용 캐시의 프리컴파일 총 시간과 unique 성공/실패.
2. 같은 프로세스의 warm PSO 재생성에서 실제 AIR/Metal 생성 횟수.
3. 재시작 후 disk-warm 시간. native handle cache는 프로세스 간 유지되지 않는다.
4. 프리컴파일 종료 뒤 정상 플레이의 CPU frame time, GPU time, frame-time p50/p95/p99, FPS와 메모리 압박/스왑.

GPU 작업의 완료를 새 캐시 성공과 혼동하지 않는다. 이미 준비된 PSO를 계속 사용하는 플레이에서 생성 호출이 없다면 캐싱은 그 구간의 GPU 시간을 줄이지 못한다. 이 경우 낮은 FPS는 캐시 성공 여부와 별도로 CPU submission과 GPU 작업 프로파일로 분리한다.

fresh staging과 복사 runtime/전용 prefix를 사용한다. 실행 중인 게임의 DLL/캐시를 덮어쓰거나 live prefix를 지우지 않는다. 결과/log/readback은 보존하고 disposable prefix는 해당 Wine 프로세스 종료 뒤 정리한다.

## 9. 구현 순서와 종료 조건

1. P0: full-key/root 연결 증거와 대표 오류 원인 수정. P1 준비와 병행 가능하나 실패 캐시로 오류를 해결했다고 처리하지 않는다.
2. P1: ordinary deterministic binding 실패 공유와 caller별 error handle 복제.
3. P2: 일반 descriptor identity 공유와 ordinary 최종 render/compute native PSO 공유. 기존 variant callers를 빠뜨리지 않으며 RT/WG 확장은 게임 사용 근거로 선택한다.
4. P3: 남은 parse 비용이 유의미할 때만 불변 분석 재사용.
5. 동일 조건 게임 A/B로 프리컴파일/CPU/GPU/FPS/메모리 효과를 따로 판정.

각 increment는 관련 테스트+실제 native/GPU surface를 통과해야 한다. 코드가 캐시를 찾았다는 로그만으로 완료하지 않는다. 이번 문서의 종료 조건은 세션/소스 대조 및 설계 검토 완료이며, 구현/새 빌드/FPS 재측정은 이번 요청에 포함하지 않는다.

## 10. 이번 설계 검토

독립 reviewer가 과거 Wine 소스와 현재 compiler/Metal4 경계를 대조했다. 구현 후 reviewer가 찾은 두 문제(typed 실패 producer의 중복 재컴파일, GPU 검증기의 native 생성량 미검증)를 수정하고 재검증했다. 승인은 캐시 소유권/키/오류/IR 경계와 검증 계약에 대한 것이며, 실제 게임 FPS 개선을 의미하지 않는다. CPU cache/ABI/diagnostics 검사, x64 Metal fixture, ARM64X production build를 실행했다.
