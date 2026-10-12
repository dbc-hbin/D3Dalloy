# Remaining D3DMetal optimization evidence

Date: 2026-10-10/11
Scope: static analysis of the D3DMetal reference, Apple Metal/AGX binaries, AMD Windows driver binaries, and the corresponding DXMT paths. The final cache-path parity change is recorded below.

## Artifact identity

Paths below use portable labels. Resolve `$D3DMETAL_REFERENCE` and `$RETAINED_WINE_RUNTIME` locally; SHA-256 values are the stable identities.

| Artifact | Type / architecture | SHA-256 |
|---|---|---|
| `$D3DMETAL_REFERENCE` | Mach-O shared library, x86_64 | `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad` |
| `$RETAINED_WINE_RUNTIME/drive_c/windows/system32/d3d12.dll` | PE DLL, AArch64 | `f4ad23dc28c23bf4d1be316ff403a1c4e4637e495c397d1fdce15b02d2b6f5a5` |
| `$RETAINED_WINE_RUNTIME/drive_c/windows/system32/winemetal.dll` | PE DLL, AArch64 | `7b648e84cd0bf02cf37a674f32a755be6baa56488684cb4fdba551f5ae26da22` |

REA `open_binary` succeeded for all three targets. The D3DMetal Mach-O inspection also produced REA evidence `ev_1dd52bab07e8d654aaae818391445e05efb07be4ff1fdc202fe7c10dde52a238`; it reported UUID `674E662B-6B5C-3FD9-9A8A-F415609D2F6A`, minimum macOS 14.0, and SDK 26.4.

Focused REA operations after the first target import repeatedly hit the 30-second provider timeout: `binary_overview`, procedure search, string search, function analysis, instruction inspection, targeted reopens, and close. Therefore the observations below distinguish REA evidence from supplemental local symbol/disassembly evidence. No REA Evidence IDs are invented for timed-out calls.

## 1. Native PSO sharing

### Reference observations

Supplemental `nm`/demangling identifies these D3DMetal functions:

- `D3DMPipelineStagesCache::Open` at `0x85468`
- `D3DMPipelineStagesCache::Clear` at `0x86de2`
- `D3DMPipelineStagesCache::CompileComputePipelineStages` at `0x86e58`
- `D3DMStageCache::CompileCompute` at `0x872e4`
- `D3DMStageResult::GetAndRetainLibrary` at `0x83926`
- `D3DMComputePipelineState` constructor at `0x10ed2c`
- `D3DMComputePipelineState::CompilePipeline(bool)` at `0x10f062`
- `D3DMGraphicsPipelineState::GetRenderPipelineState` at `0x11215c`

The available supplemental disassembly gives stronger evidence than names alone:

1. `CompileComputePipelineStages` first creates a `PipelineCacheKey`, computes a hash, and performs a hash-table lookup through the `D3DMPipelineStagesCache` object. A hit returns the existing `PipelineStagesCacheValue`; a miss allocates a value and calls `D3DMStageCache::CompileCompute`. This proves a keyed stage-cache lookup and miss path.
2. The compute pipeline-state constructor calls `D3DMPipelineStagesCache::CompileComputePipelineStages`, stores the returned stage value in the pipeline-state object at the object-local field reached from `+0x30`, and then invokes `CompilePipeline(false)` when the descriptor requires immediate compilation.
3. `D3DMComputePipelineState::CompilePipeline(bool)` uses an atomic byte at an object-local variant slot (`this + index * 0x10 + 0x48`) to serialize compilation. It calls `D3DMStageResult::GetAndRetainLibrary` and then `CreateComputePipelineStateWithDescriptor` directly. This is a per-pipeline-state native pipeline compilation/single-flight path, not proof of a device-wide native PSO map.
4. `D3DMGraphicsPipelineState::GetRenderPipelineState` hashes dynamic graphics state and walks a bucket table rooted in fields around `this + 0x4e0`/`this + 0x4e8`, comparing stored state fields before returning a cached render pipeline. The cache is attached to the graphics pipeline-state object.

### Owned-side observations

The retained `d3d12.dll` contains the owned pipeline entry points and `winemetal.dll` contains the native creation wrappers, including:

- `D3D12Device::CreateComputePipelineState` and `CreateGraphicsPipelineState`
- `MTLDevice_newComputePipelineState` and `MTLDevice_newRenderPipelineState`
- `WMTGetPipelineCacheIdentity`

The current source path remains `src/d3d12/d3d12_pipeline_compute.cpp:135-153` for ordinary native compute creation. The PE-focused REA analysis timed out before proving cross-object ownership in those binaries.

### Verdict

- **Stage cache reuse:** proven in D3DMetal and already implemented in DXMT.
- **Dynamic variant reuse:** proven as object-local in D3DMetal and already represented by owned pipeline-state caching.
- **General cross-object/device-wide native PSO sharing:** disproven in the inspected D3DMetal and owned creation/getter paths; stage/library reuse is shared, final native PSOs are object-local.

No further cache implementation is justified from this trace. A benchmark is warranted only if a new runtime path exposes duplicate final-native PSO creation outside the inspected compute/graphics creation and getter paths.

## 2. Queue priority and scheduling

### Reference observations

Relevant D3DMetal symbols:

- `D3D12CommandQueue` constructor: `0x81e0c`
- `D3D12CommandQueue::ExecuteCommandLists`: `0x7ff40`
- `D3DMCommandQueue` constructor: `0x1541a2`
- `D3DMCommandQueueWorker` constructor: `0x15468e`
- `D3DMCommandQueueWorker::JoinThread`: `0x1547e6`
- `D3DMCommandQueue::ExecuteCommandLists`: `0x1591a8`
- `D3DMCommandQueue::Wait`: `0x1591aa`
- `D3DMCommandQueue::Present`: `0x1591b6`
- `D3DMCommandQueue::Signal`: `0x159820`

Supplemental disassembly of the reference `DXGIDevice` priority methods is direct:

```text
DXGIDevice::SetGPUThreadPriority(int) 0x190d92
  validates priority in [-7, +7]
  movl input, 0x20(this)
  returns S_OK

DXGIDevice::GetGPUThreadPriority(int*) 0x190da6
  reads 0x20(this)
  writes the output
  returns S_OK
```

There is no call from these methods to a Metal command queue, queue selection, or QoS operation. The reference imports dispatch/QoS APIs, including `dispatch_queue_attr_make_with_qos_class` and `pthread_attr_set_qos_class_np`, but imports only prove availability, not that D3D12 GPU priority uses them. The worker constructor creates a dedicated worker-thread path; its internal helper mapping to QoS was not recovered.

The supplemental PE string inventory shows that the owned `d3d12.dll` contains `dxmt::thread::set_priority(ThreadPriority)`, `dxmt::ThreadFn::set_priority(ThreadPriority)`, and `dxmt-d3d12-submit-thread`. This is a concrete lead for a new PE caller trace, but strings alone do not prove the queue worker invokes it or which D3D12 priority maps to which thread priority. `winemetal.dll` contains command-queue and command-buffer wrappers but no priority/QoS strings.

### Verdict

- **D3DMetal GPU queue priority optimization:** disproven for the traced dynamic path. SetGPUThreadPriority only stores a value; worker and dispatch QoS are fixed at creation.
- **CPU worker QoS mapping:** proven fixed when enabled: pthread and dispatch queues use QoS value 0x21; the DXGI priority field is not read.
- **Actionable implementation:** none. The reference already uses fixed creation-time QoS, and the owned API intentionally returns S_FALSE instead of substituting submission-thread QoS.

## 3. Submission, pass merge, clear, indirect, and visibility heuristics

### Reference symbol evidence

Supplemental `nm` identifies the exact D3DMetal optimization routines:

- `MPLCommandReplayer::BuildEncoderSubmissionOrder` at `0x2b846`
- `MPLCommandReplayer::MergeEncoders` at `0x2dd08`
- `MPLCommandReplayer::MergeColorAttachments` at `0x2e1d0`
- `MPLCommandList::FlushClears` at `0xec42`
- `MPLCommandList::SetupExecuteIndirect` at `0x1083a`
- `MPLCommandList::BeginVisibilityQuery` at `0x1c95a`
- `MPLCommandList::EndVisibilityQuery` at `0x1ca42`
- `MPLCommandList::ResolveVisibilityQueryResults` at `0x1cac2`
- `EncodeExecuteIndirectSetup` at `0x160a80`
- `EncodeExecuteIndirect` at `0x161d68`

These addresses prove that the reference contains distinct optimization stages. The REA body/assembly requests timed out, so the addresses do not prove the exact eligibility heuristics.

### Owned-side source comparison

The analogous owned mechanisms are integrated rather than named identically:

- `src/d3d12/d3d12_command_queue.cpp:5366-5824`: submission planning, access frontiers, clear collection/supersession, render candidate grouping, and replay ordering.
- The render merge is bounded by a 64-node group budget and 8 candidate budget; it checks ordinary-render eligibility, resource overlap, scopes, dependencies, and attachment compatibility.
- Clear batching supersedes compatible repeated clears, folds eligible clears into render load actions, and flushes at observation/boundary points.
- `src/d3d12/d3d12_command_list.cpp:8038-8166`: retained visibility intervals versus pass invalidation/reset.
- `src/d3d12/d3d12_command_list.cpp:8940` onward: constrained native indirect fast path plus GPU argument/root/count/predicate/statistics preparation and specialized ray/mesh/stream-output paths.

### Verdict

- **Submission planning:** reference predicates proven; owned analogue exists with a different higher-level budget and exact equivalence remains unclaimed.
- **Encoder/render-pass merging:** reference predicates proven, including five-slot collapse and attachment identity/overlap checks; owned integrated analogue exists with different safety checks.
- **Clear batching:** reference subresource/range/plane predicates proven; owned analogue exists, with private offset names still unresolved.
- **Indirect preparation:** reference record/stride/ICB predicates proven; owned specialized analogue exists, with exact cross-implementation equivalence unclaimed.
- **Visibility interval retention:** reference stage/index and begin/end ordering predicates proven; owned retained-interval analogue exists, with exact semantic equivalence unclaimed.

No implementation change is justified from symbol existence alone.

## 4. Items that are not remaining optimizations

The earlier parity audit already established the following as implemented or outside the optimization scope:

- stage-granular persistence
- off-thread complete-PSO publication
- RT/workgraph immutable artifact reuse
- commit-time Present reservation
- specialization, single-flight, allocator recycling, resource-view hashing, argument-table reuse, and residency reuse
- generic `SetProgram`, HDR10+ consumption, shared-handle opening, and atomic-copy/lifetime/meta commands where the pristine reference itself is unsupported
- RR optional guide-resource plumbing remains a conditional feature gap, not a generic optimization gap

## Final classification

| Candidate | Classification | Safe next action |
|---|---|---|
| Cross-object native PSO sharing | Disproven in inspected creation/getter paths | Do not add a global native PSO map; benchmark only if a new runtime path is found |
| Effective queue priority | Disproven for dynamic propagation; fixed worker QoS proven | Preserve explicit S_FALSE boundary; do not substitute submission QoS |
| Queue/pass/clear/indirect/visibility predicates | Proven in reference bodies; owned equivalence unresolved | Keep current safety checks unless a concrete game workload justifies a parity change |
| RR optional guide surface | Feature gap, not optimization | Implement only after a real game/engine call path is established |

No source implementation was changed during this binary-evidence pass.

## Extended-timeout retry

The focused REA retry was repeated with a 300-second outer tool/CLI timeout and a fresh worker. REA successfully reopened the D3DMetal target, but the provider still terminated analyze_function(0x10f062) and procedure_pseudo_code(0x10f062) at its internal 30-second transport boundary. The conditional 0x86e58 query was not attempted after the primary function failed. No function-level REA Evidence ID or pseudocode was produced.

The local rea CLI was also checked. Its doctor --json reports no configured Ghidra installation, no Hopper launcher, and no IDA registration, so it cannot provide a separate long-running deep-provider route in this host. The MCP REA composite provider remains the only available deep-analysis route, and its exposed request schema provides no per-call timeout override. The observed limitation is therefore provider-side, not the outer agent timeout.

## Disassembly follow-up

The focused REA function requests still stopped at the provider's internal 30-second boundary, so the following is supplemental local disassembly from the same hashed artifacts. It was obtained with xcrun llvm-objdump -d --demangle and is not represented as REA function-level evidence.

### Reference queue batching and submission backpressure

D3D12CommandQueue::ExecuteCommandLists at 0x7ff40 does more than forward the list array:

- It calls each command-list vtable entry at +0x18 and materializes internal D3DMCommandList pointers.
- It chooses a submission partition count from the list count, caps it at four, and uses a single partition below ten lists. It then divides the list array and calls D3DMCommandQueue::ExecuteCommandLists at 0x158372 for each partition.
- The reference therefore has an explicit CPU-side batching policy before the worker queue, independent of the later encoder merge pass.

The body at 0x158372 shows the queue-side backpressure path directly. It checks a queue counter at this +0x40 with an atomic decrement loop, allows a bounded count through 0x3f, and otherwise compares an elapsed value against 0xfa1 before retrying or waiting. It then atomically increments the submission sequence at this +0x30 and inserts sequence/index state into a dense hash table rooted near this +0x88. This is direct evidence for bounded asynchronous submission and sequence tracking, not just a worker-thread symbol.

### Reference encoder and pass planning

MPLCommandReplayer::BuildEncoderSubmissionOrder at 0x2b846 allocates two dense hash tables, sums the per-command-list encoder count from the list record at +0x24e8, resizes the encoder vector, and initializes per-submit bookkeeping. The function then walks every list/encoder record rather than replaying the original list order blindly.

MPLCommandReplayer::MergeEncoders at 0x2dd08 is gated by debug options, iterates encoder candidates, uses a dynamic bitset's FindFirstSet, and builds/updates SubmissionIndex mappings for records spaced at 0x228 bytes. This proves a dependency/index-driven merge phase. The disassembly does not expose enough type information to recover every eligibility predicate, so it does not justify changing the owned planner's safety checks.

MPLCommandReplayer::MergeColorAttachments at 0x2e1d0 is also debug-option gated. It walks the encoder sequence, checks per-encoder flags at offsets corresponding to attachment eligibility, and updates submission-index mappings through the same dense table. This is direct evidence of an optional render-pass attachment merge heuristic.

### Reference clear, indirect, and visibility paths

MPLCommandList::FlushClears at 0xec42 first checks the pending-clear range at +0x2b98/+0x2ba0, calls SubresourceNeedsClearing, and exits without work when the range or eligibility check is empty. When work exists, it iterates the pending records and dispatches resource-specific clear handling through indirect calls. The reference therefore defers and batches clears; it does not emit every clear immediately.

MPLCommandList::SetupExecuteIndirect at 0x1083a reads command-signature metadata, computes the allocation size from the argument stride and count, calls MPLCommandAllocator::Allocate, and calls GetIndirectCommandBuffer. It then writes compact indirect records in separate layouts depending on the command-buffer flag. This proves a native indirect-command preparation path with allocator reuse and format specialization.

MPLCommandList::BeginVisibilityQuery at 0x1c95a looks up the query index in a dense hash table at +0x2c90, derives a barrier range from the existing usage record, and calls StageBarrierImpl. It updates dirty/query state around +0x1d50 through +0x1d5c. EndVisibilityQuery at 0x1ca42 invalidates or updates the same state and inserts the query usage record. The reference consequently tracks visibility-query resource hazards rather than treating query begin/end as isolated markers.

### Owned-side disassembly comparison

The owned MTLD3D12CommandQueueImpl::Initialize at 0x180068ac8 validates and copies the complete D3D12_COMMAND_QUEUE_DESC into the object at +0x80; the descriptor priority field is read during validation. The later initialization tail creates the owned event/shared-event state and traces the saved descriptor, but contains no native Metal queue-priority or QoS call.

The owned ExecuteCommandLists at 0x18006a444 acquires the queue mutex at +0x200, allocates/reserves a ListSubmission vector, and constructs submission records before releasing the mutex. This is a real submission planner and is not a direct one-list-to-one-command-buffer forwarder, although the disassembly does not prove the same four-partition policy as the reference.

The owned priority methods are materially weaker than the reference's CPU-side SetGPUThreadPriority storage:

- SetProcessPriority at 0x18006c038 validates the queue capability flag and accepts only the supported enum; it stores no new value and calls no thread or Metal API.
- SetGlobalPriority at 0x18006c08c only validates the enum/capability and likewise performs no update or native call.
- GetProcessPriority and GetGlobalPriority read the existing boolean-like state at +0x84.

This proves that an owned process/global queue-priority implementation is not present in this binary. It still does not prove that D3DMetal's public GPU-thread-priority setter changes worker QoS: the reference setter at 0x190d92 only validates [-7,+7], stores at this +0x20, and returns success.

The owned compute pipeline initializer at 0x1800d296c calls PipelineCacheKey (0x1800d7c74), initializes the per-pipeline cache (PipelineCache::Initialize at 0x1800dad80), obtains a stage view (PipelineCache::view at 0x1800dbedc), and then creates/stores the native WMT4 function through the device/compiler path. This corroborates the source reading: keyed shader/stage reuse exists, while the native PSO object remains owned by the individual pipeline-state object. winemetal.dll wrappers at 0x180004d00, 0x1800054f0, 0x1800056fc, and 0x180009e98 only marshal numbered calls for Metal queue, compute/render PSO, and MTL4 queue creation; they contain no cache or sharing policy.

## Disassembly-adjusted conclusions

1. Reference queue batching, queue backpressure, encoder submission indexing, deferred clear flushing, native indirect preparation, and visibility hazard tracking are directly confirmed at function-body level, including their main branch predicates. DXMT has structural analogues; exact semantic equivalence of the different higher-level planners remains unproven.
2. The owned queue priority setters are intentionally unsupported/no-op after validation, and the reference uses fixed creation-time QoS rather than propagating SetGPUThreadPriority. There is no proven dynamic GPU-priority optimization to implement.
3. A device-wide final-native-PSO cache is disproven in the inspected D3DMetal and owned creation paths. Both use keyed stage/library reuse followed by object-local native pipeline creation/caching.
4. No source or test files were changed in this disassembly pass, so no test run is applicable. The report now contains the recovered addresses, control-flow facts, and the remaining evidence boundaries.

## Complete residual trace

The remaining claims were followed through the worker, cache, helper, and owned-counterpart bodies rather than left at symbol-level evidence.

### Worker QoS and priority flow

The D3DMetal worker constructor at 0x15468e-0x154793 provides the missing call chain:

- When queue byte +0x18 is one, it calls pthread_attr_init at 0x37088e, then pthread_attr_set_qos_class_np at 0x370894 with qos value 0x21 and relative priority zero, and finally pthread_create at 0x37089a with ThreadStart at 0x1548f8.
- The constructor never reads the DXGI priority storage field at device +0x20 or queue +0x20. It only copies queue +0x18 and a device-derived capability byte.
- The queue constructor independently creates a concurrent dispatch queue at 0x15452e-0x154550 and a serial dispatch queue at 0x154557-0x154574. Both use qos value 0x21.
- JoinThread at 0x1547e6-0x154815 only wakes and joins the worker; it has no QoS or priority update.

The imported-symbol audit found no dynamic QoS setter such as pthread_set_qos_class_np or dispatch_set_qos_class_floor/ceiling. Therefore worker QoS is proven to be a fixed creation-time policy, conditional on the async-worker flag. Dynamic SetGPUThreadPriority is not propagated to the worker or dispatch queues. The effective D3DMetal GPU-priority optimization is disproven for the traced path, while fixed worker/dispatch QoS is proven.

The owned d3d12.dll is stronger evidence of an intentional boundary: SetProcessPriority at 0x18006c038/0x1802a0e54 and SetGlobalPriority at 0x18006c08c/0x1802a0eb0 validate supported values and return S_FALSE; the source explicitly states that Metal 4 has no GPU scheduling-priority setter and must not substitute submission-thread QoS. There is no missing winemetal priority wrapper to implement.

### Native PSO ownership

The reference compute constructor at 0x10ed2c adds 0x420 to a device/compiler field at +0x710 and calls the stage cache at 0x86e58. That stage cache performs a PipelineCacheKey hash lookup at 0x86e58 and a second stage-key lookup in D3DMStageCache::CompileCompute at 0x872e4. A hit returns a shared stage result/library; a miss compiles and inserts a stage result. Open at 0x85468 also validates a persisted cache file and repopulates stage entries.

The final native PSO path is separate. CompilePipeline at 0x10f062 uses a variant slot at this + index * 0x10 + 0x48, single-flights that slot with an atomic byte, obtains the shared library through GetAndRetainLibrary at 0x83926, calls CreateComputePipelineStateWithDescriptor at 0x16c499, and stores the native result in that compute PSO object's variant slot. Graphics GetRenderPipelineState at 0x11215c hashes state into bucket roots at this +0x4e0/+0x4e8 and compares nodes before returning a render PSO. Those roots are fields of the graphics PSO object.

The owned CreateComputePipelineState and CreateGraphicsPipelineState paths allocate one new pipeline-state object before Initialize and have no native PSO map lookup in their creation prologues. The source confirms per-object PipelineCache and device/compiler-scoped shader artifact/library reuse, with lazy indirect/statistics native variants under per-object locks.

Classification: stage artifact reuse across PSOs in the owning compiler/device context is proven; final native PSO sharing across pipeline-state objects or devices is disproven in the inspected creation paths. The only residual is an unexamined private helper outside these creation and getter paths; no symbol, table root, or call edge for such a native map was found.

### Exact reference heuristic predicates

The previously unresolved heuristic bodies now have concrete predicates:

- BuildEncoderSubmissionOrder at 0x2bd0d-0x2bd43 uses a 64-bit dependency-word fast path. When the compared scope has fewer than 64 bits it accepts the no-overlap path; at or above 64 it scans words and retries on overlap. Resource propagation keeps five stage/slot maxima.
- MergeEncoders at 0x2dd08 requires debug option byte +1, limits the collapse loop to four candidate positions, scans dependency bits with FindFirstSet, and calls MaxCollapse at 0x2ebce. All five stage-slot collapse IDs must match. Successful merges set encoder flags at +0x48 through +0x4b and predecessor state at +0x44.
- MergeColorAttachments at 0x2e1d0 requires debug bytes +1 and +6. It examines up to four slots, rejects allocation/mip/array-range overlap, and calls IsIdenticalRenderTarget at 0x2f854. Width, height, depth/stencil resource pointer plus level/slice, and every color attachment pointer plus level/slice must match; attachment mode two is rejected.
- FlushClears at 0xec42 routes encoder type zero through a pending-flag check and nonzero types through SubresourceNeedsClearing. For type two, the helper at 0x2160c scans 0x18-byte accesses, accepts the relevant access flag at byte +0xd, constructs a pending-clear key, and requires mip-range intersection. The downstream emitter compares clear mode, texture usage, resource ranges, planes, and pending flags before issuing the indirect native clear calls. No count-only heuristic was found.
- SetupExecuteIndirect at 0x1083a exits for an empty vector, selects record stride 0x38 or 0x28 from record +0x8, allocates an ICB only when record +0x20 is nonzero, masks six packed fields to 0x3f, and performs optional encoder setup only when the underlying object byte +0xa8 is one.
- BeginVisibilityQuery at 0x1c95a begins a barrier when a prior begin exists and the stored end is absent or no later than that begin. Mode one maps to one active stage and other modes map to two. It marks the command list dirty when stage or query index changes. EndVisibilityQuery at 0x1ca42 marks dirty when the active stage/index differs and advances the stored end only under the observed begin/end ordering rule.

These rules are now proven for the reference body. What remains unprovable from stripped private offsets is the semantic name of every encoded bit and whether DXMT's higher-level planner intentionally subsumes the same logic. The owned planner uses explicit budgets of 64 descriptors, 32 ranges, 8 candidates, and 64 groups, while the reference uses the 64-bit dependency gate, five-slot MaxCollapse, and four-slot attachment loop. This is a proven implementation difference, not an unverified reference predicate.

### Owned counterpart coverage

The owned queue has a complete analogue through MTLD3D12CommandQueueImpl::SubmissionThread at 0x1802a7890, ProcessExecute at 0x1802aa2b0, QueueTaskFifo, SubmissionRing<32>, and 32 native encoding/in-flight slots. The owned BeginQuery/EndQuery helpers at 0x18002c894/0x18002cc1c validate heap state, query type, index bounds, and specialized pipeline-statistics/stream-output paths. ExecuteIndirect at 0x18002d9a0 validates signature and argument/count/predicate resources before the owned GPU preflight/root/count/statistics paths. Native queue and PSO wrappers in winemetal.dll are ordinary numbered ABI marshaling functions and contain no hidden cache or QoS policy.

Thus the owned implementations are real analogues, not missing call edges. Exact behavioral equivalence to the private D3DMetal heuristics remains unclaimed because the owned design has different higher-level records and safety budgets.

### Final residual boundary

The remaining static-analysis limits are: private field names/enum meanings in the stripped D3DMetal binary, the complete unneeded tail of graphics Initialize after its allocation prologue, and runtime activation/performance behavior inside the private IOAccelerator/AGX driver for different compiler/device identities. RR guide-resource plumbing is a feature-contract question, not a remaining D3DMetal queue/PSO optimization; no new optimization conclusion is inferred from it.

REA open_binary succeeded for all three original exact artifacts, but focused REA body/decompile requests repeatedly failed at the provider's Ghidra handshake/internal timeout. All address-level findings in this section are therefore explicitly supplemental llvm-nm/llvm-objdump evidence, with no invented REA function Evidence IDs.

## Driver-level follow-up

This pass opened the actual Apple user-space Metal artifacts rather than treating D3DMetal as the driver. The host reports Apple M5 Pro and Metal 4. The sealed dyld shared cache at /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e was split into Mach-O images for analysis. REA open_binary accepted these extracted images:

- Metal framework: SHA-256 d71a036749aa194d64024e41268bf671e7cb30c8f04231fa17f207c2b9d3a2d1.
- MetalTools framework: SHA-256 86d22345d53509b419852e085df72b2493b1d3d7b88d00b0dae788057f00479a.
- AGXCompilerCore framework: SHA-256 cb7682d1ee20e86f7ed91542ed30e73e304e4b6a62521dbb4a967de19ca2646d.
- The thinned arm64e MTLCompilerService image was also accepted by REA, SHA-256 3fa8042f8babadff6b1e49e3b9d4f832478511364a9b822aa7892ba086e32440.

### Apple Metal framework boundary

The extracted Metal framework imports the actual IOAccelerator boundary symbols:

- _IOAccelCommandQueueCreateWithQoS
- _IOAccelCommandQueueSetPriorityAndBackground
- _IOAccelCommandQueueSubmitCommandBuffers
- _IOAccelResourceGetGPUVirtualAddress, _IOAccelResourceGetResidentDataSize, and _IOAccelResourceCreateAllocationIdentifierSet
- _IOAccelResourceListAddResource, _IOAccelResourceTestEvent, _IOAccelSharedAllocateFenceMemory
- IOSurface shared-event add/notify/remove listener functions

The same image contains Metal-side queue and command-buffer classes, including MTLIOAccelCommandQueue, MTLIOAccelCommandBuffer, _MTLCommandQueue, and _MTLCommandBuffer. Its private queue descriptor selectors include qosLevel, commitQueue, commitSynchronously, and completionQueue. It also imports weak GPU compiler hash hooks for compute, fragment, mesh, object, vertex, and generic function/pipeline scripts, plus _MTLGPUCompilerTimeStamp.

This proves that the Metal framework has a real driver-facing QoS/priority submission boundary, resource residency/addressing boundary, shared-event/fence boundary, and compiler pipeline-hash hooks. In the body of _MTLCommandQueue initWithDevice:descriptor: at 0x18d54a6e0, the queue setup reaches a private objc_msgSend with QoS argument 0x21 and relative priority 0 at 0x18d54a9b4-0x18d54a9bc. The private selector target is not recovered, so this is evidence of a fixed QoS request at the Metal boundary, not proof of a specific scheduler result. It does not prove that every D3DMetal queue priority is propagated to the GPU, nor that the driver shares final PSOs across devices. Those remain runtime/driver-policy questions.

### AGX compiler evidence

The extracted AGXCompilerCore image contains vtables and program-key types for concrete driver/compiler programs, including:

- AGCStatelessPSODynamicLibraryDriverShader
- AGCExecuteIndirectCDMPatchUpShader
- AGCExecuteIndirectESLGenerationVertexShader
- AGCExecuteIndirectRangeExecutionComputeShader and AGCExecuteIndirectRangeExecutionVertexShader
- AGCBlitFastClearDriverShader and AGCBlitVertexFastClearDriverShader
- AGCClearVisibilityVertexShader
- AGCComputeTGSizeOptimizationShader
- AGCBVHBuilderDriverShader
- context-save/context-restore, sparse-blit, and driver-intersection shader modules

These symbols are stronger evidence than the D3DMetal call sites for driver-owned implementations of fast clear, visibility clear, execute-indirect range/patch-up handling, dynamic PSO library support, threadgroup-size optimization, sparse operations, and acceleration-structure helper shaders. Symbol/vtable presence alone does not establish activation conditions, cache hit rates, or performance, so no DXMT code change is justified from these names alone.

### Windows AMD user-mode driver

For a cross-check, the official AMD download directory provided the September 10, 2025 amdxx64.dll UMD (59.1 MiB): https://download.amd.com/dir/bin/amdxx64.dll/6854E1B139eb000/amdxx64.dll, SHA-256 ca79a8ea93a521403e37f63bfa411f3d0c9b37d38b66301e284e251dcb46428a. The official AMD amdxc64.dll compiler component from August 18, 2025 was also downloaded, SHA-256 dc2660abc7fe0cbefe7ab6e8169a9d18beb674bb9c6a468880f2be9404f81029.

REA open_binary accepted amdxc64.dll. Focused REA analysis of that 100 MiB PE and REA opening of the 59.1 MiB amdxx64.dll timed out in the provider, so no fabricated REA procedure IDs are claimed for the Windows files. Supplemental PE export/disassembly evidence shows amdxx64.dll exports OpenAdapter10 at RVA 0xa0300 and OpenAdapter10_2 at RVA 0xa0310; the latter selects the version-two adapter dispatch and contains explicit HRESULT/error-result branches. Its exported C++ serialization types include SC_COMPILE_FLAGS, SC_CACHE_POLICY, SC_CACHE_POLICY_ATOMIC, SC_CACHE_POLICY_CONTROL, SC_CACHE_POLICY_READ, SC_CACHE_POLICY_WRITE, SC_SRC_PIPELINE, SC_SRCSHADER, SC_SHADERSTATE, and SC_CI_TRANSFORM_SHADER_DESC. The compiler DLL exports the same cache/pipeline/shader-state families and contains a :DXIL string.

This is evidence that the AMD Windows UMD/compiler stack has explicit serialized shader/pipeline state and cache-policy machinery. It is not evidence that DXMT should reproduce AMD's private cache format, and the exact D3D12 runtime path was not proven from this artifact because its REA provider timed out during PE import/analysis.

### Driver-level conclusion

The previous statement that Metal-driver behavior was entirely unexamined is superseded. The actual Apple Metal and AGX user-space binaries have now been opened and statically surveyed. They prove additional driver-facing mechanisms and concrete internal helper modules, but not a missing DXMT feature by themselves. The remaining unverified claims are narrower: runtime activation and performance of those paths, the private IOAccelerator implementation behind the imported calls, and exact AMD D3D12 call-path equivalence. No source implementation change is justified without a controlled runtime trace showing that DXMT fails to trigger a required path.

## Direct disassembly of the timed-out targets

The timed-out images were traced again outside the REA Ghidra provider with the Xcode LLVM disassembler. For the Apple shared-cache images, an enriched extraction was also made with IPSW so that the inspected bytes came from a separately hashed Mach-O image rather than from a guessed address range:

- enriched AGXCompilerCore: `build/rea-metal-enriched.lLhQwe/AGXCompilerCore`, SHA-256 `f3a7b4c19f243033cf035fb5da94dc809e49f63153094d039cfcd8f933c960a0`;
- enriched Metal: `build/rea-metal-enriched.lLhQwe/Metal`, SHA-256 `4dc640b75c473259f50c8b7acd88b71cb80649d5f8633a46b2fb59f260381320`.

### Apple Metal queue state and submission

Direct arm64 disassembly of `-[MTLIOAccelCommandQueue initWithDevice:descriptor:]` at `0x18d54a5d8` shows the queue object being created through private Objective-C calls, device/descriptor-derived values being written into ivar-offset fields, and fixed state values one and two being stored at offsets ending in `0xefc` and `0xf00`. A null result takes the cleanup path at `0x18d54a6c0-0x18d54a6d0`; this is an actual queue-object ownership path, not just a class-name reference.

`-[_MTLCommandQueue initWithDevice:descriptor:]` at `0x18d54a6e0` contains descriptor validation and queue setup branches. At `0x18d54a9b0-0x18d54a9bc` it passes `x0 = 0`, QoS `w1 = 0x21` (33), and relative priority `w2 = 0` to a private `objc_msgSend`; it stores the returned handle and has explicit null-result fallback allocations at `0x18d54a9d0-0x18d54aa08`. The private selector is not recoverable from the stripped stub, so the safe claim is a fixed QoS request at this Metal boundary, not a claim about the scheduler's final priority.

The command-buffer path is also concrete. `-[_MTLCommandBuffer enqueue]` at `0x18d5591dc` checks an existing storage/queue field, conditionally calls a helper when a state byte equals one, stores the resulting object, and writes the queued flag at `0x18d559248-0x18d55924c`. `-[_MTLCommandQueue enqueueCommandBuffer:]` at `0x18d55926c` rejects a null buffer at `0x18d559280`, performs a failure-mode branch, compares the queue's device with the buffer's device at `0x18d55929c-0x18d5592b0`, and only then obtains queue state and submits the buffer through another private message at `0x18d5592d0-0x18d5592d8`. This proves object/device ownership checks before enqueue and a distinct submission transition.

`-[_MTLCommandQueue getAndIncrementNumCommandBuffers]` at `0x18d558bd8` loads the queue counter ivar at `0xb28` and calls `_MTLUnsignedAtomicIncrement` at `0x18d558bf0`; the count is not an unsynchronized statistic. `_MTLIOAccelCommandBufferStoragePoolCreateStorage` begins at `0x18d558c00`, calls the storage allocator, tests the returned storage, and continues through the pool's retained-storage path. Together with the imported IOAccel resource-list, GPU-virtual-address, residency-size, shared-fence, and shared-event symbols, these bodies establish queue/resource bookkeeping at the Metal boundary even though the private IOAccelerator implementation remains outside the image.

### Apple AGX feature bodies and the remaining disassembly limit

The enriched AGX image retains concrete vtable/program-key symbols for `AGCExecuteIndirectCDMPatchUpShader`, `AGCExecuteIndirectESLGenerationVertexShader`, `AGCExecuteIndirectRangeExecutionComputeShader`, `AGCExecuteIndirectRangeExecutionVertexShader`, `AGCBlitFastClearDriverShader`, `AGCBlitVertexFastClearDriverShader`, `AGCClearVisibilityVertexShader`, `AGCComputeTGSizeOptimizationShader`, `AGCBVHBuilderDriverShader`, and `AGCStatelessPSODynamicLibraryDriverShader`. Those are driver-owned feature modules, not D3DMetal-local labels. The image is arm64e with authenticated vtable/fixup data; the local macOS `dyld_info` decoder rejects the extracted shared-cache image's malformed `__objc_stubs` alignment, and REA's focused analysis still times out. No raw vtable word is therefore promoted to a guessed callable address. The direct conclusion is feature-module presence, not an invented instruction trace or activation claim.

### AMD UMD and compiler traces

The Windows UMD sections are `.text` at `0x180001000` (size `0xfe5a24`), `.rdata` at `0x180fe7000`, `.data` at `0x1832cb000`, and `.pdata` at `0x18389c000`. `OpenAdapter10` at `0x1800a0300` clears the version selector and jumps to common code; `OpenAdapter10_2` at `0x1800a0310` sets it to one and jumps to the same path. The common adapter constructor/dispatcher at `0x1800a1a60` allocates a `0x54b8`-sized object through a helper at `0x180d0de58`, copies the incoming adapter fields, initializes object offsets around `0x2ae4-0x2b30`, and calls the initialization routine at `0x1800a2000`.

The `0x1800a2000` disassembly performs a large stack/object setup, calls a system or runtime boundary through the import slot at `0x180fe7138`, tests the result at `0x1800a20a0`, and then reads a capability/adapter result table at `0x1830befa0` and `0x18136b9b0`. It compares the returned value against table entries in a bounded loop ( `0x1800a20d3-0x1800a20e2`) and takes the failure/cleanup branch at `0x1800a2143` when no match is found. This is direct evidence of versioned adapter initialization and capability selection; it is not a guessed D3D12 feature table.

The UMD exports the shader/pipeline serialization families at these RVAs: `SC_SRC_PIPELINE` `0x56d0d0`, `SC_SRCSHADER` `0x56d100`, `SC_SHADERSTATE` `0x5719e0`, `SC_SI_OPTIMIZATIONS` `0x57f2c0`, `SC_CI_TRANSFORM_SHADER_DESC` `0x57f230`, `T_SC_COMPILE_FLAGS` `0x57f2f0`, `T_SC_LAUNCH_MODE_FLAGS` `0x57f290`, and the explicit optimization selectors `T_E_SC_SI_OPT_IR_MODE_SELECTOR` `0x58cf80`, `T_E_SC_SI_OPT_MINIMIZE_VGPRS_STRATEGY` `0x58d070`, `T_E_SC_SI_OPT_NPI_DISABLE` `0x58cf50`, `T_E_SC_SI_OPT_THREAD_LATENCY_STRATEGY` `0x58d040`, and `T_E_SC_SI_OPT_USE_MORE_D16_SELECTOR` `0x58cfb0`. These names and export addresses establish that shader compiler options are serialized as first-class state in the UMD.

The separate AMD compiler image has the same families at image VAs `0x180e47aa0` (SC_SRC_PIPELINE), `0x180e47ad0` (SC_SRCSHADER), `0x180e4be40` (SC_SHADERSTATE), `0x180e51370` (SC_SI_SHADERSTATE), `0x180e58c90` (SC_CI_TRANSFORM_SHADER_DESC), and `0x180e58d50` (SC_SI_OPTIMIZATIONS). Its cache policy serializers are at `0x180e67590` (SC_CACHE_POLICY_CONTROL), `0x180e71820` (SC_CACHE_POLICY), `0x180e71850` (SC_CACHE_POLICY_ATOMIC), `0x180e71880` (SC_CACHE_POLICY_WRITE), and `0x180e718b0` (SC_CACHE_POLICY_READ). Direct disassembly of the `SC_SRC_PIPELINE` and adjacent serializer bodies shows calls to Boost archive `load_object_data` followed by writes of static type/object descriptors; the cache-policy bodies are likewise serialization machinery, not proof of a runtime PSO-cache lookup in the D3D12 command path.

### Trace result

## Final cross-library trace and source change

The Apple-side compiler split is not an additional DXMT backend to reproduce. D3DMetal submits native Metal pipeline creation, Metal dispatches compilation through its own MTLCompilerService, and AGXCompilerCore owns the driver modules for fast clear, visibility, indirect execution, threadgroup sizing, BVH construction, and dynamic PSO libraries. DXMT already reaches those layers through the same Metal resource, encoder, pipeline, indirect, and acceleration-structure entry points. Calling AGX or MTLCompilerService directly would bypass the supported Metal ABI and duplicate Apple's driver policy.

One concrete parity gap remained at the existing framework-cache hook. D3DMetal's x86_64 lazy imports contain both _MTLSetShaderCachePath (D3DMetal stub 0x370300, GOT 0x4ae2d8) and _MTLMakeShaderCacheWritableByAllUsers (D3DMetal stub 0x3702fa, GOT 0x4ae2d0). The macOS SDK Metal.tbd exports both symbols. A full chunked disassembly scan found no direct call instruction to either stub in this stripped reference, so the import is not claimed as proof of runtime activation; however, the paired import and the existing DXMT path setter identify the only compatible integration point without private driver calls.

src/winemetal/unix/cache.c now declares and calls MTLMakeShaderCacheWritableByAllUsers() immediately after MTLSetShaderCachePath(resolved_path). The call is confined to the existing private-API build path, keeps the existing path readback result contract, and lets Metal's shared cache use the caller-selected per-game directory when multiple Wine processes participate. No new WineMetal call index or ABI layout was needed.

The AMD trace does not add another source change. AMD's serialized SC_CACHE_POLICY*, SC_SRC_PIPELINE, and SC_SHADERSTATE families prove that compiler/cache policy is first-class state, but their format is private Boost serialization. DXMT already includes pipeline, adapter, compiler identity, and shader-stage identity in its owned cache keys; reproducing AMD's settings blob or option names would not be a valid Metal implementation.

Validation: the exact generated arm64 Objective-C compile command for cache.c passed with clang -fsyntax-only. The configured build tree cannot be rebuilt in this environment because meson and ninja are not installed, and no GPU/Wine fixture was run. The focused CPU pipeline-cache test does not observe Metal's process-wide cache permission side effect, so it was not changed to assert unrelated prose or an unobservable implementation detail.

The timeout was not silently treated as absence. Direct disassembly recovered Apple queue QoS, queue/device ownership, atomic command-buffer counting, storage-pool entry, AMD versioned adapter initialization, bounded capability selection, and AMD shader/cache state serialization. The still-unproven portions are exactly the ones that require either authenticated AGX pointer decoding or runtime observation: the internal AGX implementation bodies behind the named helper modules, the private IOAccelerator calls, AMD UMD command-vtable dispatch after adapter construction, cache hit/miss behavior, and performance. The only source-level parity action supported by the complete trace is the narrow framework-cache permission call recorded above; direct AGX/IOAccelerator/AMD private integration remains unjustified.
