# GPU 병목 후보와 구조 변경안

2026-10-14. 현재 작업 트리 기준 정적 조사와 보존 REA 증거 대조. 새 제품 코드 구현 및 성능 측정 결과가 아니라, 구현할 설계와 검증 조건이다.

## 결론

지난 세션에는 P1뿐 아니라 shader/native PSO 재사용, pass 관련 변경, sparse 접근 판정 변경 등이 있었다. 사용자가 실제 게임에서 FPS가 조금 상승한 것을 확인했다. 이를 P1 단독 효과나 캐시 전용 변경으로 축소하지 않는다.

이번의 중심 제안은 **GPU 작업 사이의 의존성 분석을 render-pass 병합 가능 여부에서 분리하고, 실제 producer/consumer 관계로 동기화를 만드는 것**이다. 그 다음은 attachment의 마지막 사용을 추적해 불필요한 저장과 resolve pass를 줄이고, 실제로 비싼 shader의 반복 검사·메모리 접근을 줄이는 것이다. 기존 설계에 조건문 몇 개를 추가하는 수준보다 큰 변경이 필요할 수 있다.

다만 현재 자료로 어느 후보가 도시 frame time의 몇 ms를 차지하는지는 확정할 수 없다. 아래 순위는 설계 조사 우선순위이지 측정된 성능 순위가 아니다.

## 지난 변경과 측정의 기준

- 초기 HEAD는 `9668acb`; 기존 미커밋 변경 26개와 untracked 항목 4개가 있었다. 이 문서 외 제품 소스는 이번 조사에서 변경하지 않았다.
- 현재 `d3d12_command_queue.cpp:5408-5467`은 `node.sparse_resource_used`로 해당 노드의 분석 가능 여부를 판정한다. 과거 보고서의 “AnyLive 하나로 모든 ordinary pass가 거부된다”를 현행 미구현 항목으로 재사용하지 않는다.
- 같은 DSV 재바인딩 생략, root dirty tracking, stage/library/native PSO 캐시, 제출 시 render grouping은 이미 있다. 새 기능으로 다시 만들지 않는다.
- `build/local/fps-sequential-20261014-proof/game-C/city-fps.json`에는 60.867초/3,599 samples, 59.118 FPS, p50/p95 16.67ms가 기록돼 있다. 표시 주기나 cap 영향을 의심할 수 있지만, 이것만으로 GPU 병목이 없거나 cap이 확정됐다고 말할 수 없다.
- 해당 `invocation.json`에는 `OWNED_GRAPHICS_TRACE`가 없다. 이 도시 구간의 native pass 수, barrier 수, shader별 GPU 시간은 이 자료에서 얻을 수 없다.
- HUD GPU duration이 presentation interval보다 큰 샘플이 있다는 이유만으로 GPU duration 해석을 부정할 수 없다. 비동기 제출과 프레임 대응을 확인해야 한다. 이번 보고서는 그 열로 utilization을 계산하지 않는다.
- 과거 도시 화면 43.63 FPS와 다른 실행의 22.92ms 샘플이 수학적으로 일치한다고 같은 관측으로 간주하지 않는다. 로그인 pass 수와 도시 frame time도 섞지 않는다.

## 1순위: 의존성 분석과 pass 병합을 분리

### 쉽게 설명하면

서로 합칠 수 없는 작업이라고 해서 반드시 앞 작업 전체가 끝날 때까지 기다려야 하는 것은 아니다. 현재는 “한 render pass로 합칠 수 있는가?”와 “어느 GPU 작업을 기다려야 하는가?”가 상당 부분 묶여 있다. 따라서 합칠 수 없는 pass도 리소스 접근은 정확히 알 수 있는데, 그 정보를 잃고 넓게 기다리는 경우가 생긴다.

### 현재 근거

- `src/d3d12/d3d12_command_queue.cpp:138-171`, `:5408-5425`: `OrdinaryRenderRejection` 결과로 `access_complete`를 초기화한다. 고급 rendering/attachment 조건으로 merge가 불가능한 경우 access 분석까지 막힌다.
- 같은 파일 `:6850-6883`: render 시작 시 queue 공용 `fence_`를 기다린다. complete summary가 있으면 접근 stage union으로 좁히지만 이것은 특정 producer와의 hazard edge만을 뜻하지 않는다.
- 같은 파일 `:7000-7018`: render 끝에서 같은 fence를 갱신한다. compute/blit와 내부 보조 작업에도 fence 경로가 있다.
- 기존 `SubmissionNode.accesses/dependencies`와 allocation별 frontier는 재사용할 수 있다. 기존 planner 전체를 새로 만들 필요는 없다.

### 바꿀 설계

1. `access_complete`와 `merge_eligible`을 독립 판정한다. 합칠 수 없는 pass라도 접근을 온전히 설명할 수 있으면 dependency 분석에 참여시킨다.
2. native replay 순서와 merge 결과를 기준으로 RAW/WAR/WAW producer/consumer stage를 계산한다. 원래 recorded node 인덱스를 native encoder 인덱스로 그대로 사용하면 안 된다.
3. 정확한 dependency가 있는 곳에 Metal 4 stage barrier 또는 적합한 fence를 배치한다. 의존성이 없는 stage의 실행을 불필요하게 막지 않는 것이 목적이다. queue-stage barrier도 앞선 같은 stage 작업을 넓게 기다릴 수 있으므로, API 이름을 바꾸는 것만으로 개선된다고 간주하지 않는다.
4. 불완전한 분석 구간은 보수적 경계로 남긴다. **known → unknown뿐 아니라 unknown → known도** 올바르게 연결해야 한다. 중간 known pass가 공용 fence 갱신을 생략한 뒤 unknown pass가 오래된 fence만 기다리는 오류를 막는다.
5. command-buffer/Execute 경계, sparse alias, UAV/alias barrier, timeline rewind, cross-queue Wait, 내부 resolve/clear/format-conversion 작업의 접근도 유지한다. 분석 안 된 보조 GPU 작업을 건너뛰지 않는다.

수정 위치: `d3d12_command_queue.cpp`의 planner와 replay, `d3d12_command_encoder.hpp`의 필요한 접근 metadata, `d3d12_command_list.cpp`의 capture. 기존 `src/winemetal/Metal4.hpp`와 native bridge가 필요한 stage barrier를 지원하는지 확인한 뒤에만 ABI 확장을 고려한다.

검증: 독립 attachment 두 pass, RAW/WAR/WAW, fragment→vertex, compute→render, 부분 subresource, known/unknown 왕복, merged replay, Execute 간 의존성을 정확한 GPU readback으로 검사한다. 성능은 per-encoder GPU timeline으로 비교한다. submission의 start/end timestamp만으로 내부 pass 간 gap을 계산하지 않는다.

## 2순위: attachment 생존 기간을 기준으로 store/resolve 결정

### 쉽게 설명하면

나중에 버릴 중간 그림까지 매번 메모리에 저장하고, 마지막에 한 번 처리할 resolve를 별도 pass에서 다시 읽으면 메모리 대역폭을 낭비한다. 그림이 마지막으로 사용되는 시점을 알아야 안전하게 줄일 수 있다.

근거: `src/d3d12/d3d12_command_list.cpp`의 `ResolveTextureRegion` (`:9455-9600`), `BeginRenderPass/EndRenderPass` (`:10110-10270`), `DiscardResource`; `d3d12_command_encoder.hpp`의 `resolve_attachment`; queue의 attachment Store 제한과 resolve replay. 현재 ordinary attachment는 Store 중심이고 ending DISCARD를 no-store로 연결하는 설계가 부족하다.

설계:

- ending access와 attachment 수명을 기록하고, 최종 replay plan에서 마지막 consumer를 확정한다.
- 전체 영역/호환 format/sample/mip/slice의 native average resolve만 우선 producing pass에 접는다. source 보존이 필요하면 StoreAndMultisampleResolve를, 버릴 수 있으면 적합한 resolve-only 동작을 검토한다.
- depth/stencil 각각의 보존, partial resolve, min/max, predication, suspend/resume, alias, query 경계는 별도 판정한다.
- DISCARD는 아무 곳에서나 Store를 DontCare로 바꾸지 않는다. merged pass의 중간 결과와 마지막 결과의 수명을 구분한다.
- 이미 Close된 recorded arena를 제출 때 수정하지 않고 value-owned replay descriptor로 반영한다.

검증: 기존 render-passes/resolve fixture에서 픽셀·depth/stencil 결과와 source 보존을 확인하고 native ResolvePass 수 및 GPU memory traffic을 비교한다. 이 게임에서 MSAA resolve/discard가 없다면 우선순위를 낮춘다.

## 3순위: shader의 반복 검사와 scalar load 줄이기

### 쉽게 설명하면

float4를 읽을 때 네 번 검사하고 네 번 읽는 대신, 안전한 일반 경우에는 한 번 검사하고 묶어서 읽는다. 다만 끝부분 두 성분만 유효한 경우까지 통째로 0으로 만들면 화면이 달라지므로 느린 경로도 남겨야 한다.

근거: `src/airconv/dxil_resources.cpp:864-921`의 `loadBufferWords`는 사용 component마다 bounds condition과 guarded access를 만든다. DXBC raw/structured 경로도 component 단위 접근을 사용한다. `airconv_context.cpp`의 null guard와 DXIL guard가 최종 AIR에서 중복되는지도 확인 대상이다.

설계:

- 연속 mask, 보장된 정렬, 전체 접근 범위가 유효한 경우에만 vector fast path를 사용한다.
- **부분 OOB는 기존 component별 zero/read 또는 write-drop 의미를 보존**한다. unaligned/coherent/atomic/sparse status도 무조건 vector화하지 않는다.
- 중복 null guard는 실제 optimized AIR에 남아 있는지 먼저 확인한다. compiler가 이미 제거한다면 별도 tag나 pass를 추가하지 않는다.
- 변경 시 compiler/cache identity를 갱신해 이전 metallib가 재사용되지 않게 한다.

수정 위치: `dxil_resources.cpp`, `nt/dxbc_converter_base.cpp`, 필요할 때만 `airconv_context.cpp`. 대표 게임 shader의 AIR와 최종 GPU 실행시간으로 검증한다. IR 명령 감소가 Metal ISA나 FPS 감소를 보장하지는 않는다.

## 조건부이지만 큰 설계 변경 후보

### Bindless와 root SRV의 정확한 접근 요약

`d3d12_command_list.cpp:1883-1913`은 직접 인덱싱 root signature를 거부하며, root SRV를 buffer와 TLAS/transitive BLAS로 구분하지 못해 opaque로 남긴다. 단순히 이 guard를 지우면 안 된다.

reflection에 실제 resource 종류와 stage/read/write를 전달하고, plain root buffer는 전체 backing 범위로 보수적으로 추적한다. bindless는 mutable descriptor의 제출 시점 내용, alias, 실제 shader 접근 범위, owner 수명을 함께 처리한다. “heap 객체가 다르다”는 “가리키는 리소스가 독립이다”의 증거가 아니다. sampler-only direct indexing과 resource direct indexing도 별도 판정할 수 있다.

### Atomic64가 일반 draw까지 무거운 replay로 전환

`d3d12_device.hpp`의 `RequiresMSAAExecution`, pipeline의 `uses_atomic64`, `dxil_atomic64.cpp`의 phase lowering이 조사 대상이다. 활성 PSO에서는 capture/replay 및 memory cell protocol 비용이 클 수 있다. 하지만 이 게임에서 사용됐다는 증거는 없다.

bool 하나 대신 도달 가능한 stage/resource별 atomic 사용 정보를 전달하고, alias 가능성을 증명할 수 없는 메모리는 보수적으로 유지한다. native 지원 또는 다른 progress 보장 알고리즘이 입증돼야 raster replay를 줄일 수 있다. `uses_atomic64` 조건 삭제는 설계안이 아니다.

### Wave 및 implicit groupshared barrier

`dxil_operations.cpp`의 lane loop/wide-wave lowering은 일부 opcode에서 많은 shuffle/shared access를 만든다. `transforms/simdgroup_implicit_membarrier.cpp:14-59`는 `air.kernel` 모듈에서 특정 MemorySSA Store→Load 관계를 찾고 store별 중복 방지 후 barrier를 넣는다. 모든 threadgroup store 뒤에 무조건 넣는 것은 아니다.

실제 hot shader에서 활성화될 때 tree reduction/scan과 uniformity-aware barrier placement를 검토한다. 기존 warp-coherency 호환성이 필요해 들어간 pass이므로 explicit DXIL barrier만 남기고 삭제하면 안 된다. 부동소수 연산 순서와 inactive/helper lane 의미도 보존한다.

### Predicated texture copy의 row별 dispatch 묶기

`d3d12_command_list.cpp:4825`의 `if (predication_enabled_)` 안에서만 `conditional_copy`가 `EncodePredicatedBufferCopy`를 호출한다. **비-predicated 일반 copy도 row마다 compute를 만든다는 초안 후보는 오탐으로 폐기했다.**

이 경로가 실제로 뜨거우면 row/slice pitch를 받는 2D/3D copy kernel로 묶는다. predicate snapshot과 format/planar 변환 순서는 유지한다. 일반 copy용 새 fast path는 이미 있는 기능과 중복된다.

### Present의 GPU 완료 대기와 예약 시점

`d3d12_command_queue.cpp:8210-8221`은 이전 present order event를 CPU에서 기다리고, `:8290-8314`는 completion 시점의 presentation 상태를 보존한다. GPU 실행시간 자체보다는 제출 overlap과 지연 후보이다.

commit 예약과 completion accounting을 분리하는 방향을 검토하되, 예약 뒤 RESTART/resize/device failure 때 취소·억제가 실제 native에서 가능한지 확인해야 한다. `PresentReservation`이라는 C++ 타입을 만드는 것만으로 OS가 이미 받은 present를 취소할 수 있는 것은 아니다. 사용자 frame-latency/Reflex 계약을 무시해 프레임 수만 늘리지 않는다.

## D3DMetal 근거와 미확인 부분

Reference: `/Users/hanbinnoh/Downloads/D3DMetal.framework/Versions/A/D3DMetal`, GPTK 4.0b2 x86_64, SHA256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`.

이번에 REA open으로 hash를 재확인했다. procedure 검색은 30초 transport timeout으로 실패했고 대상은 close했다. 새 함수 body를 조사했다고 주장하지 않는다.

보존 증거:

- `doc/mass-ulw/01a12b4e-5c2f-7295-ae4a-6c57909964fd/queue-parity.md`: StageBarrierImpl `0x1bb66`, BuildEncoderSubmissionOrder `0x2b846`, MergeEncoders `0x2dd08`, DoPresent `0x1556a4`. 대형 body 일부는 불완전하다.
- 같은 디렉터리 `resource-parity.md`: resolve PSO 재사용과 barrier/copy mapping. PSO cache는 resolve 자체를 producing pass에 접는다는 증거가 아니다.
- 같은 디렉터리 `shader-parity.md`: D3DMetal도 sample-NaN-to-zero compatibility를 켠다. 정확성 옵션을 꺼서만 reference보다 빨라지는 방향은 제외한다. 외부 compiler wrapper만으로 generated shader의 우열을 판단할 수 없다.
- `doc/mass-ulw/01a135f0-ec60-7040-ad16-8458a34bf704/findings.ko.md`: 이전 후보 색인. 현재 sparse 등 변경된 항목은 현행 소스로 다시 판정했다.

동일 장면 D3DMetal 비교 trace는 없다. 따라서 지금은 “D3DMetal보다 느린 원인 확정”이나 “몇 % 향상”을 주장하지 않는다.

## 실행 순서와 합격 조건

1. 이전 변경 전체의 source-cut/runtime hash와 현재 tree 차이를 고정한다. 이전 결과를 현재 소스의 proof로 재사용하지 않는다.
2. 같은 도시 위치/카메라/해상도/SR/FG/Reflex/표시 제한 조건을 기록한다. 기본 frame과 generated frame FPS를 분리한다.
3. 도시 구간에서만 bounded trace를 수집해 startup이 budget을 소진하지 않도록 한다. 진단 실행은 `OWNED_GRAPHICS_TRACE=1 OWNED_GRAPHICS_TRACE_LIMIT=200000`의 native render/merge/access-opaque/present 이벤트와 Metal per-encoder timeline을 연결한다. trace OFF 성능 실행은 별도로 한다.
4. 1순위 설계는 먼저 ordinary known render/compute와 양쪽 unknown 경계의 정확성을 fixture로 입증한다. 이후 native wait와 GPU critical path가 실제로 감소하는지 측정한다.
5. attachment lifetime/resolve와 shader lowering은 별도 증분으로 적용한다. 각각 correctness readback, Metal validation, 실제 frame time p50/p95를 확인한다.
6. REA provider가 회복되면 위 네 reference 함수의 미회수 body부터 비교한다. 게임 비교는 동일 설정의 pristine reference 실행을 별도로 확보한다.

정확성은 기존 `scripts/test-owned-graphics.py`의 render-passes, enhanced-barriers, multiqueue/fence 및 변경 shader/resource suite로 검증한다. 이번 조사에서는 실행하지 않았으며 실제 runtime/staging을 결정하기 전의 실행 인자를 꾸며 적지 않는다.

완료 기준은 native 호출 개수 감소만이 아니라, **같은 출력과 계약을 유지하면서 대상 게임의 GPU critical path 또는 제출 공백이 줄고 반복 가능한 frame-time 개선이 나오는 것**이다. 큰 구조 변경을 허용하되 활성화되지 않는 특수 기능부터 고치지는 않는다.
