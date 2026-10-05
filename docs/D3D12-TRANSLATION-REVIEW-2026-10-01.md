# D3D12 Translation Review — 2026-10-01

## Document role and index

This is a **dated review and its disposition ledger**, not a living list of open defects or a current support matrix. As of the 2026-10-02 implementation pass, all eleven findings below are **fixed or already fixed**. That initial memory-safety correction retained the fixed 4 MiB capacity. A subsequent retained-chunk growth implementation is recorded in the [current arena contract and focused evidence](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#rootindirect-arena-성장과-focused-proof); it does not retroactively change the original finding or claim unlimited recording.

- [Current support and evidence boundaries](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md); [pending work and execution order](../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md). The later [RemoveDevice correction](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#removedevice-완료와-focused-proof), [compute CalculateLOD correction and focused proof](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#compute-calculatelod-완료와-focused-proof), and [DXGI budget notification completion and focused proof](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#dxgi-budget-notification-완료와-focused-proof) are complete within their stated scopes. Budget notification uses actual-source polling and owned event/cookie lifetimes; positive physical/native budget transitions were not observed or forced. These are separate from the closed eleven-findings review. The original point-mip LOD numerical oracle was undefined and not conformance proof; the corrected mip-linear compute proof is recorded in the linked canonical CalculateLOD section, without revising historical outcomes.
- [Original static review](#scope-and-evidence) and [original priorities](#priority-summary). Present-tense code descriptions in findings 1–9 describe the 2026-10-01 observation, not the current source.
- [Overnight static follow-up](#overnight-metal-4-follow-up--2026-10-02), findings 10–11.
- [Implementation dispositions](#implementation-dispositions--2026-10-02) and [manifest-specific execution evidence](#build-and-execution-evidence).
- [Detailed P0–P4 implementation history](../graphics-translator/IMPLEMENTATION-HISTORY.ko.md) and [ARM64/FEX architecture gates](ARM64-FEX-IMPLEMENTATION.md).

Source line numbers are historical observation coordinates. Current root source is authoritative; neither this document nor frozen import patches reproduce the current implementation. This documentation refresh did not rerun builds, GPU tests or Wine. Earlier build artifacts deleted on 2026-10-02 are not restored by preserving their observations; the fresh review evidence described below is a distinct, manifest-qualified record.

## Scope and evidence

**Implementation follow-up (2026-10-02):** the original observations below are historical, not the current defect list. Independent contract revalidation, corrections, focused regressions, and fresh runtime evidence are recorded in the disposition section at the end. Existing unrelated working-tree changes were preserved.

This is a read-only static review of D3D12-to-Metal translation, DXIL-to-AIR compilation, and shared components that affect D3D12. It is not a D3D11 feature audit.

Source observations were collected on 2026-10-01 and the cited finding ranges were reread at approximately 15:31–15:34 UTC. The repository was being actively modified by another agent, so line numbers describe those observations rather than an immutable commit. Revalidate each finding against the current tree before changing code.

No implementation files were changed, no builds or GPU tests were run, and no existing worker or process was interrupted. This document is the only repository addition made for this review. Findings are source-backed static conclusions, not reproduced runtime failures or a full conformance certification.

At the original observation time, README marked the D3D12 Metal 4 transition as incomplete. Findings 5 and 6 concerned that work-in-progress path and did not invalidate the previously recorded P3 classic-backend evidence. Later implementation and designated Metal 4 fixture acceptance supersede that description; consult CORE for the current state. Broad subsystem coverage does not mean every line or vendored dependency was exhaustively audited.

## Priority summary

The original recommended order was recording memory safety, structured-buffer bounds and shared-fence publication; then the two Metal 4 RTAS regressions before relying on that path's DXR validation; then shader-shape and failed-construction regressions. This order records the review recommendation, **not outstanding work**. The dispositions below supersede the original defect status.

### 1. P1 — Unchecked root-argument uploads can overflow the fixed 4 MiB arena

Locations:
- src/d3d12/d3d12_command_allocator.hpp:274–281
- src/d3d12/d3d12_command_allocator.cpp:62–70
- src/d3d12/d3d12_command_list.cpp:3446–3452 and 3516–3524

AllocateGPUHeap advances an offset into a fixed malloc-backed arena and relies on an assertion for its capacity boundary. Ordinary root-argument upload callers do not call CanAllocateGPUHeap or propagate a checked allocation failure.

A valid long recording that repeatedly changes root constants and dispatches/draws eventually exhausts this arena. Checking builds abort; assertion-disabled builds can return an out-of-allocation pointer subsequently used by memcpy. There is no corresponding 4 MiB D3D12 command-recording limit.

Suggested direction: use a growing/segmented retained argument arena, or consistently propagate checked allocation failure into the command list's first recording error. Audit all callers, including static-sampler and vertex-table uploads.

Suggested regression, not run: repeatedly dirty root constants and record dispatches past 4 MiB, including multiple lists using one allocator before Reset. Check graceful allocation handling and retained GPU argument lifetime.

### 2. P1 — Shared-fence observation loses qualifying signals followed by rewind

Locations:
- src/dxmt/dxmt_fence.cpp:96–115 and 133–144

observeShared reads only the current native shared-event token and publishes only that token's logical value. It does not preserve intermediate threshold crossings for another process's already-registered waits.

Trigger: process B opens a shared fence and registers a wait for 10 while its value is 0. Process A completes Signal(10), then Signal(0), before B's watcher runs. B can observe only the final 0, leaving its qualifying wait blocked.

Suggested direction: preserve shared completion publications and the threshold crossings relevant to registered observers. Polling only the latest value cannot implement rewind-aware latching.

Suggested regression, not run: coordinate two processes so the observer cannot poll between the qualifying signal and rewind. Exercise both SetEventOnCompletion and queue Wait. The current sharing fixture waits for both objects to observe 10 before rewinding, which avoids this failure window.

### 3. P1 — Shared-fence native token reservation order is not completion order

Locations:
- src/d3d12/d3d12_command_queue.cpp:4960–4964
- src/dxmt/dxmt_fence.hpp:95–103

A GPU signal reserves its monotonically increasing native token before GPU completion. A faster CPU or second GPU producer may reserve and complete a higher token first. When the older GPU producer finally completes, its lower token is ignored by the native shared event. Repeating the same token during retirement cannot restore the lost logical publication.

This can lose a qualifying logical signal or retain the wrong completed logical value. D3D12 permits logical rewind and requires completion-order semantics; native monotonic token order cannot stand in for producer completion order.

Suggested direction: separate native producer completion identity from publication of the completed logical fence value.

Suggested regression, not run: deliberately delay one GPU producer, complete a later-reserved CPU/second-queue signal first, and verify both last-completed value and pre-registered wait latches.

References:
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization
- https://developer.apple.com/documentation/metal/mtlsharedevent/signaledvalue

### 4. P1 — Structured-buffer bounds are checked after 32-bit address wrap

Locations:
- src/airconv/dxil_resources.cpp:818–829
- Atomic index path:748–764

bufferByteOffset calculates element index * structure stride + member offset as i32. The bounds path widens only the already-wrapped value. The atomic path has the equivalent ordering problem.

Trigger: a bounded descriptor-table StructuredBuffer<uint> or RWStructuredBuffer<uint>, stride 4, runtime index 0x40000000, member offset 0. The computed byte offset wraps to zero. An OOB load reads element 0, or an OOB store/atomic changes it. No large allocation is needed.

Use volatile descriptor flags or DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS in a regression so the bounds contract is unambiguous. This finding does not rely on root-descriptor OOB behavior.

Suggested direction: validate the structured element index or retain wide unsigned byte arithmetic before narrowing, including per-lane additions.

Suggested regression, not run: a one-element sentinel buffer with dynamic indices 0, 1, 0x40000000 and 0xffffffff; verify OOB zero/no-write semantics for loads, stores and atomics.

References:
- https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst#bufferload
- https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-structured--sm5---asm-

### 5. P1 — Metal 4 rejects nonempty RTAS prebuild size queries

Locations:
- src/winemetal/unix/winemetal_metal4.c:356,368,376
- src/d3d12/d3d12_device.cpp:2785–2788 and 3084–3087

GetRaytracingAccelerationStructurePrebuildInfo converts inputs without resolving GPU addresses, then creates a size-query descriptor with null native buffer handles. The new triangle/AABB/instance setters reject null handles whenever the corresponding count is nonzero.

The path therefore asserts in checking wrappers or returns failure and leaves public prebuild sizes zero, preventing ordinary nonempty BLAS/TLAS allocation.

Suggested direction: distinguish size-only descriptor construction from execution-input validation. Preserve correct counts/layouts without requiring real input buffers for a size query.

Suggested regression, not run: public prebuild queries for nonempty triangle BLAS, AABB BLAS and instance TLAS, followed by a real build using the reported sizes.

Classification: regression in the unfinished Metal 4 transition.

### 6. P1 — Metal 4 triangle transforms retain the wrong matrix layout

Location:
- src/winemetal/unix/winemetal_metal4.c:357–361

The triangle descriptor receives the D3D12 Transform3x4 buffer without explicitly selecting RowMajor. Metal 4's descriptor defaults to ColumnMajor. The classic bridge explicitly selects RowMajor at src/winemetal/unix/rt_acceleration_structure.c:135.

Non-symmetric or translated transforms therefore describe incorrect geometry and ray intersections.

Suggested direction: preserve the explicit row-major setting in the new descriptor.

Suggested regression, not run: use a translated, non-symmetric Transform3x4 and compare resulting intersections with independently transformed vertex positions.

Reference:
- https://developer.apple.com/documentation/metal/mtl4accelerationstructuretrianglegeometrydescriptor/transformationmatrixlayout

Classification: regression in the unfinished Metal 4 transition.

### 7. P2 — Texture1D SampleGrad derivatives do not match the physical AIR ABI

Locations:
- src/airconv/dxil_resources.cpp:3218–3233
- src/airconv/nt/air_builder.cpp:577–585

Logical Texture1D/Texture1DArray resources lower to physical Metal 2D/2D-array textures. Coordinates are padded, but SampleGrad builds one-component derivatives from the logical resource kind. AIRBuilder declares two-component derivative parameters for the physical texture and receives the one-component values unchanged.

Valid SM6.0+ SampleGrad shaders can therefore assert in a checking LLVM build or produce invalid AIR/PSO compilation failure. The corresponding DXBC path already zero-pads derivatives at nt/dxbc_converter_base.cpp:1996–2015.

Suggested regression, not run: 1D and 1D-array explicit-gradient sampling with distinct mip/slice colors; verify successful compilation and expected output.

Reference:
- https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplegrad

### 8. P2 — DXIL UAV array-texture atomics are unconditionally rejected

Locations:
- src/airconv/dxil_resources.cpp:2253–2263
- Failure paths:2599–2601 and 2659–2661

atomicAddress handles texture1d, texture2d and texture3d, but omits texture1d_array and texture2d_array. It returns null before the existing array-index and native atomic support can be used.

Valid SM6.0+ InterlockedAdd/Exchange/CompareExchange operations on R32_UINT RWTexture1DArray or RWTexture2DArray cannot compile. This is separate from unsupported 64-bit typed atomics.

Suggested direction: implement the missing coordinate-shape cases while preserving the separate array-slice operand.

Suggested regression, not run: in-bounds increment and compare-exchange in two array slices, checking both final values and returned original values.

Reference:
- https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedadd

### 9. P2 — Failed file-mapping heap creation leaks its mapped view

Locations:
- src/d3d12/d3d12_heap.cpp:253–270 and 293–296

CreateHeapFromFileMapping maps a view, then delegates to CreateHostMemoryHeap. Validation failure or allocation failure returns before owned_view_ is assigned to the heap. The mapped view has no cleanup owner on those paths.

Repeated rejected mappings retain address space and section references.

Suggested direction: acquire a cleanup guard immediately after MapViewOfFile succeeds and transfer ownership only after heap construction.

Suggested regression, not run: repeatedly submit a successfully mapped but unsupported-size/alignment mapping, plus an injected allocation failure, and verify view/reference cleanup.

## Other review observations and limits

- Native and WoW64 Unix call tables both had 315 slots with the Metal 4 entry at slot 314. The inspected 194 numbered wrappers had no call-index mismatch apart from expected naming aliases.
- AGENTS.md:114 still says CreatePipelineLibrary is unsupported, while README.md:169 and d3d12_pipeline_library.cpp describe/implement it. Update this stale guidance separately when coordinating documentation; do not remove the implementation based on the older statement.
- Earlier P3 runtime proofs must not be reused as proof of the new Metal 4 path. Current README/status documentation already makes this distinction.
- Bitfield AIR operand ordering and handle-merge auxiliary metadata remain investigation leads, not included in the nine findings. AIR-specific evidence or a validator-valid advertised-shader-model trigger is still needed.
- No runtime reproduction is claimed. Recheck live source changes and use isolated, fresh test runtimes/prefixes according to repository instructions.

## Overnight Metal 4 follow-up — 2026-10-02

Read-only source observations on 2026-10-01 23:43–23:49 UTC (2026-10-02 08:43–08:49 UTC+9), compared with HEAD 910b992 and its uncommitted working tree. The following are two additional static findings, not GPU reproductions. Earlier findings above are historical observations and must be revalidated; they are not asserted to remain unfixed. Current README records later Metal 4 validation, superseding the earlier work-in-progress description for that evidence set. Preserve unrelated ongoing work.

### 10. P2 — Timestamp-only submissions lack ordering against preceding queue work

Location: src/d3d12/d3d12_command_queue.cpp:4672–4674 (SampleTimestamp).

The new branch calls WMT::writeTimestamp without establishing a dependency on preceding native command buffers. ProcessExecute begins a fresh native recording (3393–3435); an initial Null pass encodes nothing (4039–4040). The native write handler in src/winemetal/unix/winemetal_metal4_query.c:43–52 only invokes writeTimestampIntoHeap:atIndex:, and the commit wrapper does not add a cross-submission wait. The old event signal/wait sampling sequence was removed.

Valid trigger: ExecuteCommandLists(A) with a long compute/render workload and timestamp 0 at its end, immediately followed by ExecuteCommandLists(B) beginning with timestamp 1 and resolution, without a CPU wait. Apple's primitive guarantees completion of preceding work in that command buffer; D3D12 timestamp queries require bottom-of-pipe completion of preceding queue workload. B can sample before A completes, underreporting the interval or reversing timestamps. A later resolve barrier cannot retroactively order the sample.

Suggested direction: establish GPU-only queue-wide ordering before sampling, including a timestamp at the beginning of a fresh native command buffer. Do not introduce a global CPU/GPU wait. Verify the exact Metal 4 synchronization contract on hardware.

Regression to add: separate workload/end-sample and timestamp-only successor into distinct ExecuteCommandLists calls; test monotonic bottom-of-pipe timestamps and same-slot resolve/reuse across submissions. The existing split query fixture at owned-d3d12-queries.cpp:620–675 splits the resolver, but keeps both samples with the workload in the producer list, missing this trigger.

References:
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing
- https://developer.apple.com/documentation/metal/mtl4commandbuffer/writetimestamp(counterheap:index:)
- https://developer.apple.com/documentation/metal/resource-synchronization

### 11. P2 — Indirect view-location addition must wrap as uint32 before bounds checking

Location: src/airconv/dxbc_converter.cpp:2287–2292, applyIndirectViewLocations.

The native-ICB vertex epilogue zero-extends shader-authored viewport/layer indices and PSO offsets to i64, adds them, checks the wide sum against the bound, and only then truncates. The D3D12 View Instancing specification requires wrapping UINT32 addition followed by the range check.

Valid trigger: a native ExecuteIndirect VS-to-PS draw with SV_ViewportArrayIndex = 0xffffffffu, per-view viewport offset 2, and at least two distinct bound viewports/scissors. The wrapped result is 1, but this code tests 0x100000001 and falls back to 0. The same error affects SV_RenderTargetArrayIndex with layer offset 2 and a multi-slice RTV. A triangle-list pipeline without GS/tessellation/MSAA isolates this path; SV_ViewID is not necessary.

Reachability: d3d12_pipeline_graphics.cpp:1462–1466 enables nonzero view offsets, 1905–1912 passes them to lowering, and 1988–1992 selects the rewritten native-indirect function. dxbc_signature.cpp:223–239 passes shader-authored indices without prior clamping. d3d12_command_list.cpp:6013–6020 supplies effective bounds.

Suggested direction: perform wrapping i32 addition before unsigned bounds comparison, or truncate the wide sum before comparing the wrapped value. Do not add no-wrap flags.

Regression to add: full-width authored indices, including 0xffffffff + 2 -> 1, separately for viewport and layer, plus ordinary in-range and nonwrapping out-of-bounds cases. The current view-instancing-indices.hlsl masks indices to four bits, so the new fixture cannot exercise the wrap. This is a concrete new lowering defect, not proof that the previous runtime already handled the edge case correctly.

References:
- https://microsoft.github.io/DirectX-Specs/d3d/ViewInstancing.html#view-instance-locations
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_view_instancing_desc

### Follow-up review limits and rejected candidates

- No confirmed new UMA-budget arithmetic defect was found; the advertised-memory compatibility policy is documented separately from physical dedicated VRAM.
- Metal 4 argument-table snapshot behavior is intentional. Native and WoW64 bridge slot checks did not reveal a new ABI mismatch.
- The suspected DXBC UINT-RTV logic-op float-output problem is already handled by signature output typing; it is not a finding. DXIL-specific equivalence was not established in this review slice.
- The suspected null-scratch RTAS refit failure is avoided by the deferred-update minimum scratch allocation; it is not a finding.
- No source fixes, builds, or GPU tests were performed by the reviewer. For each accepted finding, add a targeted regression and record the actual staged runtime and validation evidence. Do not replace failed evidence with a claim of broad conformance.

## Implementation dispositions — 2026-10-02

The implementation pass independently compared the then-current source with the official contracts. Findings **1–4, 7–11 remained valid at revalidation and were corrected in this pass**; **5 and 6 were already fixed** in the incoming working tree. All eleven findings therefore have a closed implementation disposition. No finding was rejected outright. Finding 3 needs the race qualification below: D3D12 does not promise a deterministic winner for independently racing signalers. This pass did not modify the existing RTAS corrections or replace historical import patches. Closure applies to these findings and their stated evidence, not full API or shader conformance.

### Findings 1–9

The following rows preserve the **initial safety disposition**. Finding 1's fixed-capacity description is dated; the subsequent retained-chunk growth, replay/retirement and injected native-failure evidence is maintained in [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#rootindirect-arena-성장과-focused-proof), separately from the original overflow proof.

| Finding | Disposition and current implementation | Focused evidence |
| --- | --- | --- |
| 1 — argument arena | Fixed memory safety, retaining the existing capacity. `d3d12_command_allocator.hpp::CanAllocateGPUHeap/AllocateGPUHeap` check wide lengths/alignment before advancing the offset; exact-end allocation is valid, failure does not mutate state. `d3d12_command_list.cpp` propagates failed root/static-sampler/vertex-table uploads through ordinary draw/dispatch, indirect, work-graph and raytracing paths; `WriteBufferImmediate` and allocator indirect payloads are checked. SO reservation ordering and indirect alignment budget holes were corrected. | Baseline aborted at `gpu_heap_offset_ < kGPUHeapSize`. The new `owned-d3d12-arena-heap.cpp` records past capacity, checks first-error precedence, multiple lists sharing an allocator, reset/reuse, and GPU readback of 2,000 distinct retained arguments. x64 and WoW64 pass with API+shader validation. |
| 2 — lost shared rewind crossings | Fixed. `dxmt_shared_fence.hpp::SharedFenceTimeline` retains the latest logical value and an independent high-water mark for each open fence observer. `Fence::observeSharedLocked` drains peaks into existing waits before restoring the current value; registration holds the same local observation lock, so later waits cannot inherit old peaks. `GetCompletedValue` returns the serialized observation, not a transient synthetic peak. | Baseline suspended-child run failed `child unobserved crossing event`. Updated sharing fixture suspends the registered observer outside its shared critical section, publishes 100 then 3,000 rewinds, resumes it, and checks event/queue latches, current 0, and a late wait that must remain blocked. x64/WoW64 pass. |
| 3 — reservation/completion confusion | Fixed. `ProcessSignal` no longer reserves/signals a shared native token before GPU completion. The existing validated retirement path publishes the logical value; the native shared event only wakes observers. `ProcessWait` uses the actual-completion latch, not a potentially unrelated higher native token. | CPU timeline probe proves preparation order cannot swallow later completion publications. GPU sharing fixture covers an older queued signal delayed behind a gate, later CPU/second-queue completion, then old completion and rewind. This gate delays the worker **before** the former reservation point: it is useful API-flow proof, not a hardware reproduction of the precise old native-token overtaking window. |
| 4 — structured address wrap | Fixed. `dxil_resources.cpp::bufferByteOffset` widens unsigned index/member arithmetic before multiplication/addition. Loads, stores, per-lane additions, 32-bit atomics/CAS and locked 64-bit atomics use the same wide byte/bounds path; guarded accesses do not issue OOB memory operations. Root-descriptor OOB behavior is not used as an oracle. | Volatile descriptor-table regressions include index 0, 1, `0x40000000`, `0xffffffff`, raw lane-end overflow and 64-bit stride wrap. Baseline GPU readback showed OOB atomic writes corrupting element 0. Frozen shader/fixture: `resource-review.hlsl`, `owned-d3d12-dxil-resource-review.cpp`. |
| 5 — nonempty RTAS prebuild | Already fixed. `winemetal_metal4.c` builds size-only descriptors with real counts/layouts and a native placeholder where required; unresolved application buffers are rejected only on execution. `GetRaytracingAccelerationStructurePrebuildInfo` does not dereference GPU addresses. | Existing public triangle/AABB/TLAS queries return nonzero sizes and subsequent builds pass. Native actual-handler probe, API validation, count 1,000: triangle AS/scratch 92608/8064, AABB 80576/32000, TLAS 216832/20416. Native probe is sizing/API evidence, not GPU execution. |
| 6 — triangle matrix layout | Already fixed. `WMTMetal4ASTriangles` explicitly sets `MTLMatrixLayoutRowMajor`. | Existing `geometry-transform-row-major` compares an asymmetric translated transform against independently baked vertices and passes the fresh raytracing GPU suite. |
| 7 — Texture1D SampleGrad | Fixed. `lowerSampleGrad` constructs derivatives for the physical texture dimensions and zero-pads the extra coordinate for logical 1D/1D-array textures, preserving logical derivatives. | Baseline isolated PSO compilation aborts with LLVM “Calling a function with a bad signature!”. New GPU gradient oracle checks distinct mip and array-slice colors for both shapes. |
| 8 — UAV array atomics | Fixed coordinate-shape omission in `atomicAddress`: 1D-array coordinates are padded to physical 2D; 2D-array coordinates retain two components; the existing separate array-slice operand is preserved. | Baseline isolated PSO creation fails with `dxil: atomicBinOp on an unsupported resource shape`. New fixture checks increment, exchange, successful/failed CAS, returned originals and final values independently in two slices of each shape. Explicit `DeviceMemoryBarrier` orders ordinary texture initialization/readback with atomics; the first unfenced fixture run was invalid evidence for that ordering. |
| 9 — mapped-view failure leak | Fixed. `CreateHostMemoryHeap` acquires `unique_ptr`/`UnmapViewOfFile` ownership before validation/allocation, then transfers it to the heap. Borrowed host memory stays borrowed; the no-copy Metal buffer still releases before unmapping. | Baseline leaked views on 4 KiB mapping rejection. x64/WoW64 regression: 32 failures retain exactly the original control view (1→1), output pointers clear, successful mapping stays owned until heap release, and rejected interface cleanup works. Heap-allocation OOM was not injected; that path is covered by the same RAII ownership statically. |

Official contract sources checked for these dispositions:

- [Command-list Close](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-close): recording failures, including `E_OUTOFMEMORY`, are returned by `Close`. The initial safety correction retained **4 MiB per allocator**. The subsequent retained-chunk growth keeps checked native-length/allocation errors and does not claim unlimited recording; its injected native allocation failure proof is not physical memory exhaustion.
- [D3D12 user-mode heap synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization): logical rewind, qualifying outstanding waits, immediate CPU signal versus queue-ordered signal; independently racing signals have no promised deterministic winner. [Metal shared-event signaledValue](https://developer.apple.com/documentation/metal/mtlsharedevent/signaledvalue) only advances for a larger value.
- [DXIL buffer operations](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst#bufferload), [structured loads](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/ld-structured--sm5---asm-), [SampleGrad](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplegrad), and [InterlockedAdd](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedadd). Interlocked operations do not imply a memory fence.
- [Prebuild info](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-getraytracingaccelerationstructureprebuildinfo), [D3D12 triangle geometry](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_geometry_triangles_desc), and [Metal transformationMatrixLayout](https://developer.apple.com/documentation/metal/mtl4accelerationstructuretrianglegeometrydescriptor/transformationmatrixlayout).
- [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile) / [UnmapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile): mapped-view lifetime is distinct from the mapping handle lifetime.

Shared-fence limits: metadata is versioned; incompatible old payloads are rejected. There are **1,024 live observer slots per shared fence**; exhaustion returns allocation failure rather than overwriting an observer. Detached/dead-process slots can be reclaimed. Publications do not wait for observer acknowledgement or GPU completion, but do take a short per-section named kernel mutex. Suspending a process while it owns that mutex can stall callers until it resumes; abandoned-owner recovery covers process exit, not arbitrary suspension. D3D11 retains its separate fence path.

### Finding 10 — queue-wide bottom-of-pipe timestamps

Accepted against [D3D12 timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing) and the installed official Metal SDK contracts: `MTL4CommandBuffer.h` says `writeTimestampIntoHeap:atIndex:` waits for preceding work **in that command buffer**; `MTL4CommandQueue.h` says `signalEvent:value:` follows all preceding queue GPU work and `waitForEvent:value:` gates all future GPU work. The original path had no such dependency for an initial sample in a fresh buffer.

`d3d12_command_queue.cpp` now owns a queue-local event and monotonic value. At the first timestamp in each `CommittingScope`, it schedules a GPU signal/wait pair before that scope commits. This orders previously committed work ahead of the entire current segment; the timestamp primitive orders work within it. Later samples in the same segment reuse that dependency. No CPU completion wait, device-global event, or synchronization on unrelated queues was added. Timestamp-free scopes do not emit the pair.

`owned-d3d12-queries.cpp` adds eight rounds of separate workload/end-sample, initial-successor-sample/resolve, and same-slot sample/reuse/resolve submissions (24 separate `ExecuteCommandLists` calls). Each workload records 2,048 draws; only the final completion fence is CPU-waited. All readback rows must be nonzero, non-error and nondecreasing, including across rounds. **The baseline hardware also passed this regression.** The defect is contract-backed static reasoning; the after run proves the corrected path executes successfully, not a demonstrated before/after GPU reversal.

### Finding 11 — wrapping view-location addition

Accepted against the [official View Instancing specification](https://microsoft.github.io/DirectX-Specs/d3d/ViewInstancing.html#view-instance-locations), which explicitly requires 32-bit UINT addition (which can wrap), then bounds checking and index-zero fallback. `dxbc_converter.cpp::applyIndirectViewLocations` now uses ordinary LLVM i32 `CreateAdd` without `nuw`/`nsw`, then unsigned comparison of the wrapped result.

The native-ICB fixture no longer masks authored indices to four bits. Six cases independently cover viewport/layer `0xffffffff + 2 → 1`, ordinary `1 + 2 → 3`, and nonwrapping OOB `2 + 2 → 4 → 0`, with four viewport bands and four RTV slices. It uses triangle-list VS→PS and 1× sampling. Baseline viewport and layer runs separately rendered at index 0 instead of 1; the corrected full suite passes all 32 cases. The first exploratory baseline stopped earlier because the modified fixture relied on partial indirect constant preservation; it is retained but is **not** wrap evidence. The finalized existing authored-index case supplies all three constants in its indirect command.

### Build and execution evidence

All paths in this section are relative to **`build/local/review-20261002/`**. These are fresh copied runtimes and fresh per-run Wine prefixes, not installed games or live prefixes. Each runner evidence directory retains `result.json`, exact fixture/shader hashes, runtime manifest hash, stdout/stderr and native-module/validation checks. Passing rows below have exit 0, no foreign backend and no Metal validation errors.

| Check | Actual outcome / evidence |
| --- | --- |
| x64 Ninja + Meson install | Pass: `build-fixed.log`, `install.log`. Initial `build.log` caught stale shared-fence queue callsites after concurrent edits; callers were reconciled before the successful build, not suppressed. |
| WoW64 Ninja + Meson install | Pass: `build-wow64.log`, `install-wow64.log`. |
| Build/staging/diagnostics Python suites | **16 / 22 / 59 tests pass**, respectively: `test-build.log`, `test-staging.log`, `test-diagnostics.log`. Diagnostics include rejection of missing/corrupt new readback/order evidence. |
| x64 timestamps | `queries-after/result.json`: API+shader validation; eight cross-submission/reuse rounds, existing query/clock/occlusion cases and split same-/cross-queue resolves pass. Baseline `queries-before02` also passes; no failing-before timestamp claim. |
| x64 view indices | `viewwrap/after/result.json`: API+shader validation; all 32 cases pass, including all six new wrap/valid/OOB cases. Specific failing-before proofs: `viewwrap/before-02` (viewport), `viewwrap/before-layer` (layer). |
| x64 DXIL resources | `dxil/after-ordered/result.json`: API+shader validation and owned native compiler verified; all **73** exact words pass: bounds 26, gradients 6, array atomics 24, bounds64 17. `dxil/before` is strict-runner bounds failure proof. `dxil/after` preserves the initial unfenced atomic fixture failure; it is not presented as a runtime regression. |
| x64 arena/mapping | `arena-heap/arena-after/result.json`, `arena-heap/mapping-after/result.json`: API+shader validation pass. Failures before correction retained at `arena-before` and `mapping-before`. |
| x64 shared fences | `shared-fence/after/result.json`: all 20 cases pass with API+shader validation, including event/queue/late-wait latch checks. `shared-fence/before-02` reproduces the lost crossing; `before` was a fixture DLL dependency error, not fence evidence. |
| x64 RTAS | `rtas/after/result.json`: full targeted raytracing fixture passes on the rebuilt runtime with API+shader validation and owned compiler proof. `rtas/before/result.json` independently confirms findings 5/6 were already fixed. |
| WoW64 targeted boundary coverage | `arena-heap/arena-wow64/result.json`, `arena-heap/mapping-wow64/result.json`, `shared-fence/wow64/result.json`: API+shader validation passes. No WoW64 timestamp/view/DXIL/RTAS execution claim for this pass. |

Runtime manifests:

- Baseline `runtime-before/owned-graphics-runtime.json`: SHA-256 `8a95024162b85ad828df6153c96b6b296b3b200a3a6eae7b7152d4708b9cecd4`.
- Fixed x64 `runtime-after/owned-graphics-runtime.json`: SHA-256 `9ba1357563819f26c71ef162b91729619fbcba189e1479d9bec21046265bcc3b`. Redundant payload retired after execution; manifest/evidence retained.
- Combined x64/WoW64 `runtime-after-wow64/owned-graphics-runtime.json`: SHA-256 `f86b3c2083198820b81e1ae8e518218469dd6eab044c5eb8b9beeea93b052101`. The x64 artifacts are the same fixed install; WoW64 cases above ran from this manifest. Superseded payload retired; manifest/evidence retained.
- Latest retained `runtime-final/owned-graphics-runtime.json`: SHA-256 `4d05f38bf0c08387c6a22e75db6acc2ad768fb8ae237fb0176aeb1b0a64376a9`. After excluding the comparison-gradient expansion described below, both builds/installations passed again (`build-final.log`, `install-final.log`, `build-wow64-final.log`, `install-wow64-final.log`). The affected x64 shader suite was rerun: **`dxil/final/result.json` passes all 73 words with API+shader validation and verified owned native compiler**. Other rows retain the earlier manifest-specific evidence; they were not all rerun on `runtime-final`.

Additional evidence and limits:

- The same physical derivative-shape correction covers compute implicit sampling. A valid SM6.6 2×2-thread Texture1D/Texture1DArray implicit-sampling shader passed upstream DXC and actual `SM50Initialize`→`SM50Compile` (5,325-byte metallib); `dxil/extra-public-compile.log` is compiler smoke, not GPU proof. An exploratory SampleCmpGrad change was removed: the existing compiler rejects 1D comparison sampling as non-depth before constructing the gradient ABI. That separate unsupported path remains unchanged and is not claimed fixed.
- `dxil/isolated-before-1/` reproduces the gradient signature assertion; `isolated-before-2/` reproduces the array-atomic unsupported-shape PSO failure; `isolated-before-3/` shows the 64-bit OOB alias. These raw isolated runs record the staged manifest and builtin DLL loads but **do not establish the strict runner’s owned native-compiler provenance**. The final integrated shader run does.
- `rtas/sizing-probe.log` is native actual-handler sizing evidence under API validation, not GPU work. `arena-heap/allocator-probe.log` is an ASan/UBSan arithmetic/lifetime smoke using copied allocator methods, not the D3D12 GPU path. Actual arena GPU proof is the runner row above.
- `graphics-translator/tests/shared-fence-timeline.test.cpp` was compiled and run: unobserved 5,000-rewind latching, completion publication, independent/new/reused observers, interrupted-publication recovery and capacity boundary pass. A compiled x64/i686 layout probe independently establishes size 16,416 bytes, observer stride 16, and header offsets 0/4/8/16/24/32; it does not claim a mixed-bitness two-process GPU run.
- All disposable verification prefixes were removed only after their scoped Wine servers stopped. Throwaway source probes were removed; logs, binaries, manifests and frozen evidence remain. Existing other-agent runtimes/work were not deleted.
- This is targeted synthetic regression evidence, **not full D3D12/DXR conformance or real-game compatibility**. No push, merge or deployment was performed.
