# D3D12 sustained FPS 정적 병목 감사

- 대상: 현재 `dxmt-d3d12` 작업 트리
- 목적: P1 depth-only `OMSetRenderTargets` 재바인딩 수정 후에도 FPS가 거의 변하지 않는 이유를 찾음
- 방법: D3D12 제출/encoder/resource/AIR/PSO/temporal/specialized 8개 영역의 정적 검토 + pristine D3DMetal REA 대조. 영역 전체를 대상으로 검색하고 주요 실행 경로를 읽었으며 모든 소스 행을 읽은 것은 아니다. 영역별 읽은 파일·범위와 미독 범위는 동명 `.txt` 조사 결과에 남겼다. 이 최종 보고서가 조사자의 초안보다 우선한다.
- D3DMetal reference: `/Users/hanbinnoh/Downloads/D3DMetal.framework/Versions/A/D3DMetal`, x86_64, SHA256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`
- 판정 경계: 코드 동작은 확인했지만 FPS 영향은 측정하지 않은 후보다. 도시와 무관한 로그인 trace의 pass/draw 수와 99.4% utilization은 사용하지 않는다.

## 결론

P1이 겨냥한 동일 DSV 재바인딩이 도시에서 드물거나 제출 planner가 해당 경계를 이미 병합했다면 FPS 개선은 작을 수 있다. `src/d3d12/d3d12_command_list.cpp:7337-7437`의 동일 DSV 유지 조건은 기록 단계의 split을 줄이고, `src/d3d12/d3d12_command_queue.cpp:5725-5819`는 조건이 맞는 recorded encoder를 다시 병합한다. 반대로 planner의 budget/opaque/sparse 조건으로 병합이 거부되는 경계에서는 P1이 native encoder 수도 줄일 수 있다. 따라서 "P1은 GPU work를 줄일 수 없다"는 단정은 틀리며, 어느 경우인지는 도시의 recording/native encoder 수를 연결해야 한다.

현재 sustained GPU FPS의 우선 조사 대상은 **encoder 사이의 fence 의존성과 shader의 반복 memory/sample lowering**이다. `AnyLive()` 보수 모드는 활성 여부를 먼저 확인할 큰 조건부 후보이고, per-draw binding은 CPU 후보로 별도 분류한다. 셰이더 컴파일/PSO 후보는 대부분 first-use hitch 또는 startup이며, 꾸준히 낮은 FPS의 1차 원인으로 분류하면 안 된다. 아래 P0/P1은 이번 보고서의 조사 우선순위이며 기존 설계 문서의 P1 번호와 다르다.

## 우선순위 후보

### P0: 도시 trace에서 즉시 확인할 것

1. **queue-wide MTLFence wait/update가 render encoder마다 실행됨**
   - 근거: `src/d3d12/d3d12_command_queue.cpp:6869`, `:7004`; bridge `src/winemetal/unix/winemetal_metal4.c:1518-1522`.
   - 흐름: `ProcessExecute`가 render encoder를 열 때 `waitForFence(fence_)`, 끝날 때 `updateFence(fence_)`를 넣는다. access summary가 없을 때 일반 render는 Vertex wait / Fragment update이며, SO는 Vertex update, mesh는 Object/Mesh stage가 추가된다. summary가 있으면 접근 stage union을 사용하지만 이것이 직전 producer와의 실제 hazard만을 뜻하지는 않는다.
   - 비용: GPU stage ordering/over-serialization 후보. 실제 hazard가 없는 consecutive pass까지 queue-level fence로 묶이는지는 trace의 `native_render_wait` stage와 replay access 상태로 판정해야 한다.
   - 최소 검증: 도시 10초 bounded trace에서 `native_render_begin/end`, `native_render_wait/update`, submission/encoder ID와 GPU interval을 조인한다. `wait_stages`가 실제 producer stage보다 넓거나, consecutive pass가 fence로 직렬화되는데 resource hazard가 없다면 최우선 수정 후보다.
   - 주의: fence 제거는 금지. stage-aware dependency를 증명한 뒤 범위를 좁혀야 한다.

2. **reserved/tiled resource 한 개가 전체 submission을 보수 모드로 전환할 가능성**
   - 근거: `src/d3d12/d3d12_reserved_resource.{hpp,cpp}`의 process/global `AnyLive()`; `src/d3d12/d3d12_command_queue.cpp:137-170, 5270-5414, 5725-5819`의 ordinary render rejection 및 merge 조건.
   - 흐름: live reserved resource가 있으면 ordinary merged-encoder 경로가 전역적으로 거부되고, sparse ordering을 위한 shared event/barrier가 추가될 수 있다.
   - 비용: GPU pass/encoder 증가와 queue ordering. BC-compressed 3D texture가 내부적으로 reserved 경로를 활성화하는 조건도 확인해야 한다.
   - 최소 검증: trace에서 `reserved`, `tile`, `alias_flush`, `render_rejection`, `native_render_begin`을 correlate하고 `AnyLive` 활성 시점과 도시 frame time을 비교한다. 도시에서 activation이 없으면 폐기.

3. **per-draw root/vertex table 재인코딩과 native argument-table setter 비용**
   - 근거: `src/d3d12/d3d12_command_list.cpp:2638-2673, 4145-4174, 7005-7230`; bridge `src/winemetal/unix/winemetal_metal4.c`의 argument-table binding/state code.
   - 흐름: dirty state가 draw마다 생기면 GPU heap chunk에 root arguments/vertex table을 쓰고 `setbuffer` commands를 기록한다. native bridge는 table/resource setter를 수행하며, texture/buffer는 상태 비교가 있으나 command traversal과 collection/setter 호출은 남는다.
   - 비용: CPU recording/native encoding. 셰이더 GPU time 자체는 줄이지 않지만 CPU-bound FPS 또는 submission latency를 악화시킬 수 있다.
   - 최소 검증: trace에 `tableBind`, `tableSet`, `setbuffer`, `native_render_*` counter를 추가/활성화하고 draw 수 대비 dirty rebinding 비율을 계산한다. 같은 VA/table이 반복되는데 `setbuffer`가 반복되면 P1 다음 수정 대상.

4. **resolve와 64-buffer merge cliff**
   - 근거: `src/d3d12/d3d12_command_list.cpp:9455-9600, 10246-10252`; queue `:7464-7528`; `kRecordedSubmissionBufferBudget=64` in `src/d3d12/d3d12_command_encoder.hpp:116`; merge candidate cap/64-member boundary in queue planner `:5594, :5725-5819`.
   - 비용: producing render pass에 resolve가 접히지 않아 별도 native resolve encoder가 생기고, recorded buffer/access budget 또는 64-member 경계가 replay merge를 끊을 수 있다.
   - 최소 검증: frame별 `native_render_begin`, `resolve`, `native_blit`, replay rejection reason, member count를 histogram화한다. `members=64`, `recorded_buffer_budget`, `resolve` 경계가 자주 나타나는 경우에만 수정.
   - Budget에 도달했을 때 access summary를 버리고도 safe로 표시하는 것은 올바른 최적화가 아니다. 정확한 coverage를 유지하는 압축/중복 제거 또는 입증된 제한 조정만 후보로 삼는다. Resolve도 전체 서브리소스의 호환 native resolve부터 검토하며 부분 영역/min/max/depth/보존 계약을 일괄 병합하지 않는다.

4a. **명시적 ending DISCARD가 tile no-store로 연결되지 않음**
   - `src/d3d12/d3d12_command_list.cpp:10166-10176`은 ending RESOLVE를 수집하고 `:10234-10263`은 resolve/unbind를 수행한다. `DiscardResource`는 `:7903-7909`에서 no-op이다. 일반 attachment의 Store를 이 discard 정보로 DontCare로 바꾸는 경로가 빠져 있다.
   - 버리는 depth/scratch attachment를 계속 저장하는 GPU 대역폭 후보다. 게임이 명시적으로 해당 discard를 사용하는 경우에만 해당하며, partial discard·suspend/resume·PRESERVE_LOCAL·병합된 마지막 consumer를 고려해야 한다. 생산 pass의 Store/resolve destination/후속 접근을 함께 확인한다.

### P1: shader/generated code 확인 대상

5. **DXBC sampling의 SAMPLE_NAN_TO_ZERO**
   - 근거: flags `src/d3d12/d3d12_pipeline_graphics.cpp:825,954,1662,2062,2203,2265,2352,2461`, compute `:99,203`; lowering `src/airconv/nt/air_builder.cpp:801-802,1529-1538,1633-1638,1733-1760,2101-2106,2189-2194,2407-2412`; helper `air_builder.hpp:626-629`.
   - 비용: DXBC 변환에서 sample coordinate/derivative에 isnan/select를 추가할 수 있다. DXIL `ResourceLowering`은 `AIRBuilderOptions{}`를 사용한다(`dxil_resources.cpp:338-345`). 따라서 flags가 PSO에서 설정된다는 사실만으로 모든 DXIL sample에도 비용이 있다고 주장하지 않는다.
   - Counterevidence: it is a deliberate D3D semantic compatibility flag; `getNaNToZero` may fold cheaply. Never remove globally without conformance proof.
   - Verification: DXBC sample-heavy PS/CS의 최종 명령과 GPU 시간을 비교한다. sample이 없는 shader는 이 flag를 꺼도 sample sanitization 작업을 줄이지 못하므로 성능 실험으로 부적절하다. 의미를 보존하려면 좌표의 finite 증명이 필요하다.

6. **per-lane OOB/null descriptor control flow**
   - DXIL: `src/airconv/dxil_resources.cpp:802-829, 937-964`; DXBC: `src/airconv/nt/dxbc_converter_base.cpp:3555-3565`; each buffer lane can get bounds branch/select/PHI. Textures remain table-bound; root buffers can skip some metadata guard.
   - Cost: shader GPU control flow and register pressure for shaders using structured/raw buffers or null/OOB accesses. Not universal.
   - Verification: classify city shader bytecode by raw/structured buffer access and compare generated instruction counts; do not optimize away guards without proving descriptor bounds and D3D null semantics.

7. **Metal4 barrier/fence command overhead in special paths**
   - Ordinary render path does one wait/update each native render encoder, but `m4_stage_barrier` in `src/winemetal/unix/winemetal_metal4.c:417-445` tracks pending/ordered stages and does not emit a barrier when no un-ordered prior stage exists.
   - Conclusion: generic “remove all barriers” is unsupported. Investigate only trace-confirmed barriers in temporal, MSAA, query, sparse, swizzle, or alias paths.

### P2: first-use / startup 비용

8. **lazy PSO variants and native creation lock**
   - 근거: `src/d3d12/d3d12_pipeline_graphics.cpp:2143-2246,2248-2320,2406-2430`; global creation lock `src/d3d12/d3d12_pipeline_cache.cpp:1118-1121`.
   - MSAA/coarse, color-write-mask, mesh/index-pull/statistics/ICB variants can compile on first draw under mutex. This can cause frame hitch, not a steady FPS floor after warmup.
   - Verify `pipeline_artifact_operation` native_render/native_mesh timestamps between first Draw and frame end.

9. **cache disk restore/flush and publication drops**
   - `d3d12_pipeline_cache.cpp:1453-1461` and `d3d12_shader_cache.cpp:428,666,669,799-813`; publication queue limit 8 at pipeline cache `:130-137` and stage writer.
   - Startup/mid-gameplay PSO churn cost only. Existing trace phases `disk_read/disk_write/metadata_wait` can falsify.

### 별도 상시 실행 후보: temporal / present

10. **SR/FG transient work and present completion gates**
   - SR per-frame mutex/view/lease work: `src/d3d12/d3d12_temporal.cpp:59-61,161`, `src/winemetal/unix/metalfx-backend.mm:1282-1283,1478-1484`.
   - FG transient allocation: `src/winemetal/unix/metalfx_frame_generation.mm:804-819` allocates current/output and conditional UI/depth/motion/distortion textures; `:958-980` copies the baseline into `impl_->second`. Baseline copy is a redundancy candidate only when the later scatter covers the whole destination; subrect padding preservation and real-output semantics must remain intact. FG OFF means this FG candidate is inactive.
   - `src/d3d12/d3d12_command_queue.cpp:8204-8208` waits on the preceding present order event before encoding. Current `:8305` also calls `ReserveMetal4Present`; normal presentation is not solely the old completion-thread fallback. `src/winemetal/unix/winemetal_metal4.c:755-812` commits, signals drawables and coordinates feedback/commit-return before finishing tickets. This must be compared with D3DMetal's commit-time scheduling, retaining RESTART/error semantics.
   - These are conditional steady CPU/GPU/pacing costs, not startup-only. Ordinary frame rendering can precede the present wait in FIFO, so the wait does not by itself prove whole-frame serialization.

## 추가 셰이더 후보와 반증

- **정규화 좌표 sampler에도 동적 coordinate-mode 분기:** `src/airconv/nt/air_builder.cpp:1410-1481` checks metadata bit47 and builds pixel-coordinate and normalized sample arms, with null/min-LOD/empty cases and PHIs. Both sample sites exist in generated IR, but only the selected arm executes: this is not two texture samples per pixel. Static-sampler metadata specialization is the narrowest improvement candidate; descriptor-table samplers must retain dynamic behavior. Compare optimized AIR/control flow/register use before attributing FPS.
- **DXIL guard + AIR guard 중복 가능성:** `dxil_resources.cpp:4213-4275` and `airconv_context.cpp:77-115` insert null checks at different layers. Dominating-condition simplification may remove the second guard; source count is not final machine-instruction count. Root buffers and nonnull sampler metadata are not proof that texture handles are nonnull.
- **compute threadgroup implicit barrier:** `transforms/simdgroup_implicit_membarrier.cpp:14-56` uses MemorySSA to insert a simdgroup barrier after a qualifying AS3 store used by a later load. It does not instrument every store; it can still add per-iteration ordering when the qualifying store is in a loop. Confirm presence in the actual compute shaders. Moving such a barrier to a loop preheader is not generally semantics-preserving.
- **raw/structured vector access의 경계 처리:** per-lane guards are real frontend IR. A vector fast path must prove the entire vector is in bounds and preserve a partial-OOB fallback. Testing only the first component or returning zero for the entire partially valid vector changes D3D results.
- **wide wave / wave fallback / denorm preserve:** `dxil_operations.cpp:1792-1919,2012-2036,2183-2193` can emit scratch+barriers or lane shuffle loops; preserve mode at `:660-720,789-830` can invoke emulation. Retain only when game shader metadata/operations activate them.
- **reduced AIR pass list:** `airconv_context.cpp:169-244` lacks a full LLVM loop/vector pipeline, but Metal subsequently compiles AIR. Missing LLVM passes alone do not prove missing final optimization. Descriptor lookup memoization is compile-time; emitted descriptor loads still execute on the GPU.

## Conditional features not to blame without activation proof

- Emulated MSAA/ROV, tessellation/geometry capture+replay, indirect ICB resolver, atomic64 cooperative lock loop, DXR AS helper passes, work graphs, matrix/WaveMMA, FP64 softfloat.
- Sources: `src/d3d12/d3d12_command_list.cpp:2093-2099,3190-3430,3684-3841,8480-8483,9041-9176`, queue `:2907-2943,7129-7197`, `src/airconv/dxil_atomic64.cpp`, `dxil_matrix.cpp`, `dxil_fp64.cpp`.
- In the recorded ZZZ evidence, RT was OFF; no city activation proof exists for tessellation, atomic64, workgraphs, or FP64. These are high-cost if active, not current-cause claims.

## CPU 제출 경로의 후순위 후보

- `d3d12_command_queue.cpp:7830-7885`는 Execute 시 recorded command를 순회해 DRED history를 만들며 `d3d12_diagnostics.cpp:405`의 기본 capture는 FORCED_OFF가 아니면 켜진다. Debug validation OFF와 breadcrumb 비용 없음은 같은 말이 아니다. CPU profile에서 history walk를 먼저 구분한다.
- `d3d12_command_queue.cpp:7969-7984`의 Signal은 completion retirement를 위한 별도 submission을 만든다. marker도 submit 수를 늘릴 수 있다. frame당 Signal/marker 빈도와 native commit CPU 시간을 확인한다.
- `d3d12_command_queue.cpp:8020-8030`의 Wait는 FIFO worker에서 논리 completion을 기다려 consumer submit을 지연시킨다. 단순 shared-event value로 교체하면 rewind·다중 producer·관찰자별 threshold crossing 계약을 깨뜨릴 수 있다. producer validated retirement부터 consumer submit까지의 CPU 지연을 측정하고, 이 지연을 줄여도 의미를 보존하는 설계가 있는지 따로 검토한다.
- `d3d12_device.cpp:2813-2822,3166-3180`의 commit/VA lookup 공유 lock, queue planner와 per-draw access capture의 반복 map/vector 작업, `winemetal_metal4.c:269-289`의 view-pool parent residency walk는 CPU 후보다. 실제 사용하지 않는 bindless resource를 임의로 residency에서 빼거나 이전 recording의 setter equality만으로 소유권 retain을 생략하면 안 된다.
- submission 간 GPU timestamp 간격은 개별 pass의 fence 비용을 직접 측정하지 못한다. submit timestamp 차이 또한 planning CPU 시간과 같지 않다. 정확한 CPU 구간 계측 또는 stack sampling, encoder별 GPU timeline을 사용한다.

## D3DMetal REA comparison

REA confirmed the pristine binary identity above. Relevant reference observations:

- Stage cache lookup exists as `D3DMStageCache::GetCachedResult` at `0x995de`, with per-stage lookup dispatch for vertex/fragment/hull/domain/mesh/amplification/geometry/compute/stream-out.
- Reference graphics stage compile is `D3DMPipelineStagesCache::CompileGraphicsPipelineStages` at `0x89188`; compute pipeline compilation is guarded by an atomic attempted flag in `D3DMComputePipelineState::CompilePipeline` at `0x10f062` and then creates a native compute PSO.
- Reference has encoder IMP caches: `RenderCommandEncoderIMPCache::UpdateIMPCache` `0x5826a` and `ComputeCommandEncoderIMPCache::UpdateIMPCache` `0x5810c`; these cache Objective-C method implementations, not application state and are not proof of a missing DXMT feature.
- Reference `D3D12GraphicsCommandListMPL::DrawIndexedInstanced` at `0xda7ca` calls `SetRenderState` before dispatching the draw, so a per-draw state setup path also exists in D3DMetal. Reference `OMSetRenderTargets` at `0xdb4f8` compares/stores attachment metadata and marks dirty state; it still invokes the encoder-side setter. This weakens claims that DXMT's OM bookkeeping alone explains FPS.
- Reference `D3DMCommandQueueWorker::DoExecute` `0x154e40` batches events and encodes command lists; `DoPresent` `0x1556a4` uses a blit/triangle present path, fences when needed, and commits. Reference behavior confirms the general architecture but does not prove a performance delta without matched runtime traces.
- Reference barrier analysis (`MPLCommandList::CheckBarrierDependencies` `0x1f31a`, `EmitBarrierDependencies` `0x2085c`, replay `0x36263/0x3632c`) shows dependency-aware barrier pruning and stage-specific barriers. This supports investigating DXMT's `native_render_wait/update` widths, not deleting barriers wholesale.
- Reference clear PSO is keyed/cached in `D3DMCommandEncoder::GetClearPSO` `0x17a03a`. DXMT also has cached clear/pipeline artifacts; no missing clear-cache claim is established.

REA limitations: pseudocode is shipped-artifact evidence, not source; no matched ZZZ D3DMetal trace exists; binary is x86_64 only.

## Required measurement before code changes

1. Run one fresh bounded city trace with the same runtime hash, resolution, MetalFX/FG/VSync/cache state as the previous measurement. Do not use the login trace.
2. Export per-frame/per-submission counts: native render/compute/blit begin/end, replay group members and rejection reason, render wait/update stages, fence/barrier count, descriptor table bind/set, root/vertex setbuffer, resolve/copy bytes, present reserve/retire, PSO native artifact operations.
3. Correlate these with GPU start/end timestamps and frame time p50/p95. Static code alone cannot distinguish CPU-bound rebinding from GPU-bound shader work.
4. A/B one candidate at a time. The first experiment should be trace-only and counter instrumentation; the first code experiment should be a narrow, semantically safe change only after activation is proven.

## 독립 검증에서 확정한 정정

별도 reviewer가 같은 소스 경로를 다시 읽고 다음을 확인했다.

- `OMSetRenderTargets` depth-only 수정은 native pass 수에 영향을 줄 **수는 있지만**, replay merge가 가능한 경우에는 pre-fix와 post-fix 모두 `native_render_begin` 하나가 될 수 있다. `pass_end`만 줄고 `native_render_begin`이 그대로면 GPU 병목이 아니다.
- 일반 render의 fence update 기본 stage는 모든 stage가 아니라 Fragment다. Vertex는 capture write, Object/Mesh는 mesh 경로에서 추가된다. 따라서 fence의 병목성은 stage mask와 실제 access hazard를 같이 봐야 한다.
- root/vertex binding은 매 draw 무조건 실행되지 않는다. command-list dirty state와 Metal4 value cache가 같은 값을 억제한다. `table_set/table_bind`를 draw 수와 비교하기 전에는 per-draw 병목으로 부르지 않는다.
- `SAMPLE_NAN_TO_ZERO`는 실제 영향을 받는 DXBC sample coordinate/gradient에만 적용된다. sample 없는 shader와 기본 DXIL `ResourceLowering` 경로에 일반화하지 않는다.
- null descriptor guard는 operation 단위이며 per-lane이라고 부르면 안 된다. per-lane 경계 분기는 raw/structured buffer OOB lowering의 후보로 분리한다.
- SR/FG present는 completion thread 하나로만 설명되지 않는다. Metal4 present reservation, commit-return drawable signal, feedback, D3D12 retirement가 함께 동작한다.

Reviewer가 남긴 가장 먼저 확인할 counters는 `submission_access_opaque(reason=sparse_alias|recorded_buffer_budget)`, `submission_render_merged`, `native_render_begin`, `ResolvePass`, `msaa_producer_wait`, `native_render_wait/update` stage, `table_set/table_bind`, `pipeline_artifact_operation`, 그리고 present-order wait duration이다. 이 항목들은 모두 조건부 후보이며 현재 도시 trace가 없으므로 활성 상태나 FPS 개선량을 확정하지 않는다.

## Not implemented

No production source was changed. This request was an investigation, and all FPS impact statements remain hypotheses until the matched city trace/A-B is run.
