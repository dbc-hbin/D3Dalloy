# Wave 1 harvest — persistent binding and tile/attachment lifetime

Lane: `binding-tile-harvest` (`doc/mass-ulw/20261015-rea-redesign-wave1/graph.json`).
Read-only: no source, test, fixture, runner, or runtime file was changed; no build or GPU run was
performed in this lane. Every `path:line` below was read in this session from the working tree.
Sections are labelled **observed** (read from the tree or a retained artifact) or **inference**.

## 1. Observed — persistent binding ownership (descriptor + root)

Per-command-list root-argument state is a plain array, not a heap object:

| Owner | Anchor | Observed shape |
|---|---|---|
| graphics root staging | `src/d3d12/d3d12_command_list.cpp:493-494` | `Com<MTLD3D12RootSignature,false> rootsig_graphics_` + `uint64_t rootarg_graphics_staging_[kMaxRootArgumentQwords]` |
| compute root staging | `src/d3d12/d3d12_command_list.cpp:497-498` | same, `rootarg_compute_staging_` |
| bound heap slots | `src/d3d12/d3d12_command_list.cpp:6880-6892` | `descriptor_heaps_` is exactly two entries (CBV_SRV_UAV, Sampler); `SetDescriptorHeaps` rejects non-shader-visible, other-device, duplicate-type |
| heap retention | `src/d3d12/d3d12_command_allocator.hpp:91` | `RecordingHead::descriptor_heaps` (`Com<ID3D12DescriptorHeap>`) keeps the heap alive for the recording |
| staging width | `src/d3d12/d3d12_device.hpp:384` | `kMaxRootArgumentQwords = 64 + kHeapQwords(4) + kMaxRootViewStatusQwords(32) + kViewIdQwords(1)` = 101 |

Observed write path for every root setter: mutate the staging array, then set a dirty bit; there is a
value-equality early-out before the dirty bit.

- `SetGraphicsRootDescriptorTable` `src/d3d12/d3d12_command_list.cpp:7014-7018`
- `SetGraphicsRoot32BitConstant` `:7039-7043`; `SetGraphicsRoot32BitConstants` `:7080-7087`
- `SetGraphics/ComputeRootConstantBufferView|ShaderResourceView|UnorderedAccessView` → `UpdateRootView` `:7189-7238`
- root-signature change re-seeds staging: `StageDirectlyIndexedHeaps` + `ClearRootViewStatus` + `StageViewId` `:6948-6956` (compute) and `:6977-6985` (graphics); `ResetState` clears both arrays `:1517-1521`

Observed upload path: `EncodeRootArgument` `src/d3d12/d3d12_command_list.cpp:4145-4160` bump-allocates
`sizeof(uint64_t) * UploadQwords * Count` (64-byte aligned) from the allocator's GPU heap and memcpys
the staging array. It runs only when `DirtyState::GraphicsRootArguments`/`ComputeRootArguments` is set
(graphics `:2656-2673`, compute `:4237-4248`), and the encoded `wmtcmd_render_setbuffer` /
`wmtcmd_compute_setbuffer` points at that heap offset. GPU heap API: `ReserveGPUHeap` /
`AllocateGPUHeap` / `ResolveGPUHeap` / `GPUHeapAddress` `src/d3d12/d3d12_command_allocator.hpp:319-360`.

Observed heap-directly-indexed staging: `StageDirectlyIndexedHeaps`
`src/d3d12/d3d12_command_list.cpp:6904-6926` writes `{base, NumDescriptors}` per heap type into four
root qwords at `MTLD3D12RootSignature::HeapQwordOffset` (`src/d3d12/d3d12_device.hpp:355-365`), so
SM 6.6 `ResourceDescriptorHeap[]`/`SamplerDescriptorHeap[]` bounds against the bound heap.

Observed descriptor payload: a shader-visible heap is a Metal 4 argument buffer plus a texture view
pool, not a per-descriptor native bind.

- `ShaderVisibleDescriptorGPUStorage *mapped_argument_buffer_` `src/d3d12/d3d12_descriptor_heap.cpp:215-217`
- `WMT::Reference<WMT::TextureViewPool> view_pool_` + `SetPoolSlot`/`ResetPoolSlot` `:223-241`
- heap start = argument-buffer GPU address `:359-361`; samplers mirror this at `:803-846`, `:908-911`
- every `Add*View` does `ResetPoolSlot(Index)` then writes CPU storage *and* the GPU argument-buffer slot
  (e.g. SRVTexture `:369-399`, UAVTexture `:422-450`, CBV `:406-415`, SRVBuffer `:545-560`)
- bulk `CopyDescriptors` uses one `copyTextureViews` for a contiguous run `:641-664`, per-descriptor otherwise `:670-681`

Observed per-draw binding identity captured for the submission planner:
`CaptureStandardSwizzleTables` `:1834-1875` and `CaptureSubmissionAccess` `:1882-1990` produce
`StandardSwizzleTableUse{rootsig, heap, base, parameter, shader_bindings}` plus
`RecordedBufferAccess{address,length,writes,stages}` on the open encoder. The completeness bound is
`kRecordedSubmissionBufferBudget = 64` (`src/d3d12/d3d12_command_encoder.hpp:116`); exceeding it sets
`submission_snapshot_budget` and clears `submission_group_safe` (`:2004`, `:2452-2462`), which the
planner turns into the rejection `recorded_buffer_budget` (`src/d3d12/d3d12_command_queue.cpp:153`).

Observed masked-PSO cache: `RenderEncoderData::masked_pipeline_states`
(`src/d3d12/d3d12_command_encoder.hpp:609`) retains native PSOs created for masked/indirect draws past
list reset (`src/d3d12/d3d12_command_list.cpp:2576-2595`, `:9183-9187`).

Observed submission-side descriptor resolution: `ResolveSubmissionTables`
`src/d3d12/d3d12_command_queue.cpp:216-...` re-reads the heap's **CPU** descriptors
(`table.heap->GetDescriptor(...)`) and resolves each to an allocation + COM owner; owners are retained
in `plan.descriptor_owners` (`:120-130`). Budget `kSubmissionDescriptorBudget = 64` (`:60`), rejection
`descriptor_resolution_budget` (`:143-147`).

**Inference (high confidence):** descriptor *contents* are not part of a recorded command. The recorded
binding identity is `(rootsig, heap, base, parameter)`; the values are read at submission time. That is
deliberate (the encoder header states descriptor tables are resolved under the Execute lease, like
`RecordedBufferAccess`), and it is why `ResolveSubmissionTables` must re-walk the heap and why the heap
is retained per recording.

## 2. Observed — attachment Store/Resolve/lifetime boundaries

Render-encoder creation and rebind elision:

- a render encoder is opened lazily in `PreDraw` `:2241-2244` (`submission_group_safe = true`)
- `OMSetRenderTargets` `:7319-7437` elides rebinding into a compatible open render encoder and traces
  `render_targets_rebind_elided` `:7434`; otherwise `EndSchedulingPass()` `:7437`
- default attachment actions on bind: `load_action = WMTLoadActionLoad`, `store_action = WMTStoreActionStore`
  (color `:2324-2325`, depth `:2345-2346`, stencil `:2352-2353`)
- a `Clear` encoder recorded immediately before is folded into the render encoder's load action
  (`WMTLoadActionClear`) `:2368-2411`, traced `clear_load_folded`; the clear's own encoder is skipped later
  by `folded_into_render`

`BeginRenderPass` / `EndRenderPass` (`:10120-10291`):

- beginning CLEAR → `ClearRenderTargetView`/`ClearDepthStencilView` `:10208-10227`
- ending DISCARD → `store_action = WMTStoreActionDontCare` for color `:10238-10239`, depth `:10245-10246`,
  stencil `:10247-10248` — the only writers of `WMTStoreActionDontCare` in `src/d3d12`
- ending RESOLVE → recorded in `render_pass_resolves_` `:10172-10187` and executed in `EndRenderPass`
  `:10275-10280`, *before* the surfaces are unbound (`OMSetRenderTargets(0,nullptr,...)` `:10285`), with
  `RejectInRenderPass()` returning false while `render_pass_internal_` `:10096-10099`

Consequences of `DontCare` in the planner (`src/d3d12/d3d12_command_queue.cpp`): a `DontCare` store forces
`boundary_before = boundary_after = true` `:5437-5440`, so such a pass is never merged; it is still
accepted by `OrdinaryRenderRejection` `:168`, by node access capture `:5414`, and by the rebind-elision
checks `src/d3d12/d3d12_command_list.cpp:7354`, `:7398`, `:7402`.

Resolve is always its own encoder and its own native pass:

- `ResolveTextureRegion` `:9458-...` ends with `allocator_->InvalidateCurrentPass(); auto *resolve =
  allocator_->AllocatePass<ResolveEncoderData>()` `:9596-9600`
- `whole_average` `:9612-9618` decides the *mechanism*: true ⇒ no software `pipeline` (native MSAA
  store-action resolve), false ⇒ a full-screen `dxmt_resolve_*` fragment pipeline (`pipeline` set)
- the queue encodes the no-pipeline case as source attachment `load=Load`,
  `store=WMTStoreActionStoreAndMultisampleResolve`, `resolve_texture = dst`
  `src/d3d12/d3d12_command_queue.cpp:7538-7542`, labelled `ResolvePass` `:7551`
- the pipeline case encodes `dst` as the color/depth/stencil attachment and draws one triangle `:7520-7574`
- `RenderEncoderColorAttachmentData::resolve_attachment` (`src/d3d12/d3d12_command_encoder.hpp:157`) is
  **read but never written** anywhere in `src/d3d12` (reads: `d3d12_command_list.cpp:7352`;
  `d3d12_command_queue.cpp:167`, `:6650-6652`, `:6762`, `:6835`) — the "fold the resolve into the
  producing pass" capability exists in the encoder/queue plumbing but is unwired on the recording side

Merged render groups (the D3DMetal-style encoder collapse) are the tile-lifetime lever:

- `RenderReplayGroup` / `SubmissionPlan` `src/d3d12/d3d12_command_queue.cpp:106-131`;
  `AppendRenderMember` `:285-323` requires identical attachment view set and dimensions; a later member
  must be `WMTLoadActionLoad`; the group descriptor's store action is overwritten by each member, i.e. the
  **last** member's store wins; `support_color_attachment_mapping = 1` and each member gets its own
  `setColorAttachmentMap` `:6921-6940`
- budgets: `kSubmissionCandidateBudget = 8`, `kSubmissionGroupBudget = 64`, forced boundary every 64
  nodes (`:60-63`, `:5624`)
- merged members are replayed inside one native encoder: `native_render_begin` `:6868-6870`, per-member
  `native_render_member` `:6956-6959`
- inline barrier between members of one group: `:6942-6953` → `MTL4RenderCommandEncoder::memoryBarrier`
  (`src/winemetal/Metal4.hpp:179-183`) → `WMTRenderCommandMemoryBarrier` →
  `[encoder barrierAfterEncoderStages:stages_after beforeEncoderStages:stages_before ...]`
  `src/winemetal/unix/winemetal_metal4.c:1431-1434`
- merge guard: an internal (same-group) non-attachment dependency must satisfy
  `InlineRenderDependencySafe` = attachment-only or Vertex→Vertex `:102-105`, enforced at `:5776-5779`
- per-pass fence: `updateFence(fence_)` at the end of every render encode `:7043-7044`,
  `waitForFence(fence_, stages)` at the start `:6902-6908`, and `native_fence_wait_skipped` when the node
  has no known hazard `:5921-5925`

Observed TBDR tile-size surface that D3D12 does not use: `WMTRenderPassInfo.tile_width`/`tile_height`
(`src/winemetal/winemetal.h:875-876`) reach `descriptor.tileWidth/tileHeight`
(`src/winemetal/unix/winemetal_metal4.c:532`), but no `src/d3d12` file writes them. The only
`WMTRenderCommandDispatchThreadsPerTile` / `WMTTileRenderPipelineInfo` user in the tree is the D3D11
shared path (`src/dxmt/dxmt_command.cpp:404-425`); D3D12 has no tile-dispatch path. MSAA attachmentless
draws instead size a software coverage target to GPU-produced primitive bounds
(`src/d3d12/d3d12_command_queue.cpp:2065-2072`, `rov_marker_attachmentless` set at
`d3d12_command_list.cpp:2364`).

Observed sparse tile lifetime (the other meaning of "tile" in this tree): `ReservedResource::AnyLive()`
is a process-wide counter (`src/d3d12/d3d12_reserved_resource.hpp:90-105`, `:250`); while any reserved
resource exists, the planner rejects render nodes with `sparse_alias`
(`src/d3d12/d3d12_command_queue.cpp:5441`) and barriers record `QueryEncoderOp::AliasFlush`. Tile mapping
updates are queue tasks applied on the submission thread with a shared-event signal/wait
(`TileMappingTask` `:809-816`, `ProcessTileMapping` `:1162-1201`).

## 3. Observed — retained D3DMetal / REA facts and evidence paths

Pristine reference identity (unchanged from the wave ledger
`doc/mass-ulw/20261015-rea-redesign-wave1/ledger.jsonl`): D3DMetal SHA256
`f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`, recorded in
`QA/render-fragment-barrier-contract/contract.json` (`binary.sha256`,
`hash_verified_before_rea_open: true`) and in `QA/render-fragment-barrier-contract/input-sha256.txt`.

Retained REA symbol/evidence registry — `QA/render-fragment-barrier-contract/prior-rea-registry.txt`:
the render-collapse machinery is `MergeEncoders` `0x2dd08` (`ev_2170a5299709666ae2baee4254720110d6871c7c423358d9b4bb3d6d79b867cd`),
`MergeColorAttachments` `0x2e1d0` (`ev_df21f6ca40f5ef986489f7f7f51d73b99aa197c2959f7bc274ede72080a0610d`),
`BuildEncoderSubmissionOrder` `0x2b846` (`ev_f7e2bc23570009ed40064fae6526e2312d0b9707ae1dc4e27ad59ee5fa86f3ce`),
`MaxCollapse` `0x2ebce` (`ev_0d7e622ee3ddb5024974f9d00d102c88fc24e61a718902e68f7e49d1539ff138`),
`ReplayEncoders` `0x2e95a` (`ev_d8e736d5581decb4fa0625ebb36fb5b5856c5fe4bafece078b7fb2b056f2aaae`),
`IsIdenticalRenderTarget` `0x2f854` (`ev_8a24c35e9fb8b64802141e4cf863ff5aa8a472c2fdf6cfb1bd63eec8c914e127`),
`ReplayBeginRenderRange` `0x33850` (`ev_f02947951647ecf5ddf3ff3fbb3ef1becd05bc618ccc56ed5125631e05d6d19a`),
native `RenderEncoder Begin` `0x28436` / `End` `0x29110`
(`ev_188feabe3b0431657f6b8f868ac8e8104c97d5d268263e3ca06d22c997a9c59f` /
`ev_d7c488271eef3b2d239231236cfef5834bf10714672052300ab211655494bbb1`), plus the two barrier replays
`ReplayInlineMemoryBarrier` `0x36263` (`ev_d0a29294f0920c2fa745c89f553a627f4e7c05bace4c1c76c5746ecda471851a`) and
`ReplayBackwardStageBarrier` `0x3632c` (`ev_061e47c3b95673108a21b7449532a1d54de9b334a4f82da0e4f325eb8e093348`).

Retained contract decision — `QA/render-fragment-barrier-contract/contract.json`
(`decision` block): dependent fragment UAV requires a native render split;
`same_pass_fragment_completion_barrier: "unsupported"`; `same_pass_fence_replacement: "invalid; fences
order different passes"`; legal mechanisms are producer-fence→endEncoding→new encoder→consumer wait,
endEncoding→new encoder→consumer queue barrier with Device visibility, or producer queue barrier with
Device visibility→endEncoding→new encoder; `new_command_buffer_required: false`. Apple's rule is
transcribed in `QA/render-fragment-barrier-contract/apple-intrapass.md` ("Metal doesn't support intrapass
barriers that wait for the tile or fragment stages on devices that have a TBDR architecture") and the
observed Metal 4 selector is `barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:`.

Retained observed failure — `QA/render-fragment-barrier-contract/observed-failure.txt`: submission 3,
`native_render_begin` encoder 210 with `members: 2`, `submission_render_merged` group 0 members 2,
`submission_access_edge before:1 after:2 waw:1 stages_before:2 stages_after:2`, then the debug-layer
assertion `afterEncoderStages must be a valid combination of MTLStages for the current encoder type
(MTLStageVertex | MTLStageObject | MTLStageMesh)`. No `native_render_inline_barrier` event precedes it —
consistent with the trace call sitting after `memoryBarrier()` in the emit path (`:6949-6952`), so the
assertion aborts before the trace line is written.

Retained native-binding receipt — `build/local/d3dmetal-opt-port-20261009/native-binding-proof.json`:
12 numeric cases; first-use counts `{tables: 2, setters: 22, binds: 4, residency_new: 1, add: 8, remove: 8,
commit: 2}`, retired counts `{tables: 0, setters: 6, binds: 1, residency_new: 0, add: 4, remove: 4,
commit: 2}`, `positive_validation_errors: 0`, and three negative arms that all fail with
`Compute Function(copy_word): Buffer binding at index 2 for optional cannot be null.` (exit 134).

Retained render-pass evidence (CPU-only receipts, no GPU authority):
`build/local/d3dmetal-opt-port-20261009/phase2-verifier/phase2e-lifetime-render-passes-v2.json`
(`verdict: PASS`, `cpu_only: true`, `validation_errors: []`) and
`build/local/d3dmetal-opt-port-20261009/phase2-verifier/phase2e-render-passes.json`
(`verdict: FAIL`, semantic failure `graph GPU readback matrix missing, duplicated, reordered or incorrect`).
Execution receipt `build/local/d3dmetal-opt-port-20261009/gpu-phase2d-render-passes/execution.json`
records the fixture run (`exit: 1`, `semantic_adjudication: "pending; fixture exit alone is not
acceptance"`).

Prior read-only harvest notes reused here (line numbers in them drift; the claims below were re-checked
against the tree in this lane): `doc/mass-ulw/01a135f0-ec60-7040-ad16-8458a34bf704/RenderPass.txt`
(render-pass/encoder-cost audit, `C1`-`C6`), `Resources.txt` (binding/descriptor cost audit, `C1`-`C3`),
`QueueSync.txt`, `PsoCache.txt`, and the REA per-symbol dumps `rea-0x3632c.json`, `rea-0x36263.json`,
`rea-0x2085c.json`, `rea-0x1f31a.json` in the same directory.

**Correction to that prior note (observed):** `RenderPass.txt` angle 3 states
`WMTStoreActionDontCare` appears only in `src/dxmt/dxmt_context.cpp`. That is wrong for the current tree:
`src/d3d12/d3d12_command_list.cpp:10239`, `:10246`, `:10248` write it from `BeginRenderPass` ending-access
`DISCARD`. The D3D11 writers it cites also exist (`src/dxmt/dxmt_context.cpp:1352-1355`).

## 4. Inference — current binding ownership and cache behaviour, in one paragraph

A recorded command list owns two fixed staging arrays (graphics/compute), two retained shader-visible
heap pointers, and a set of per-encoder metadata structures; it owns no descriptor *values*. Root argument
values are re-uploaded into a bump-allocated GPU heap chunk only when the corresponding dirty bit is set,
so the steady-state cost is one memcpy of `UploadQwords * 8` bytes plus up to four `setbuffer` commands
per dirty draw (graphics binds to vertex/fragment, and to object/mesh for mesh pipelines). Descriptor
contents live in the application's heap as a Metal 4 argument buffer, so there is no per-descriptor native
bind; the shader-visible heap is retained for the whole recording because the submission planner re-reads
its CPU descriptors to build the dependency graph. The only binding cache is the masked-PSO vector on the
open render encoder. Everything else is dirty-flag gated.

## 5. Recommended next increment (one)

**Fold the native store-action MSAA resolve into the pass that produced the source attachment, inside the
submission planner (`src/d3d12/d3d12_command_queue.cpp`).**

Why this one: it is a tile-lifetime boundary reduction with an in-repo precedent and no new mechanism.
The precedent is the D3D11 encoder-list optimizer, which already does exactly this fold
(`src/dxmt/dxmt_context.cpp:1357-1365`: `result.src->store_action = WMTStoreActionStoreAndMultisampleResolve;
result.src->resolve_attachment = result.dst;`). The D3D12 plumbing is present but unused:
`ResolveEncoderData` carries `src`, `dst`, `dsv_flags`, `sample_positions`, `parameters.mode`,
`parameters.predicate_*`, `predicate`, `destination_x/y`, `width`, `height`
(`src/d3d12/d3d12_command_encoder.hpp:698-720`), and the queue already consumes
`color.resolve_attachment` (`:6762`) and the group descriptor's store action (`:296-317`). Today the same
resolve costs a whole extra native pass: the source MSAA attachment is loaded from memory, stored again,
and the resolve store runs (`:7538-7542`) — precisely the "unfolded resolve" cost already flagged in
`RenderPass.txt` candidate `C3`.

Shape of the increment (planner-side, no recording-side change, no new field):

1. When the node being merged is `EncoderType::Resolve` with `data->pipeline == nullptr` (the native
   store-action case chosen by `whole_average`, `d3d12_command_list.cpp:9612-9618`), and the immediately
   preceding merge-eligible `EncoderType::Render` node has that resolve's `src` view as one of its color
   attachments at the same level/slice/plane and dimensions, and the resolve has no `dsv_flags`, no
   `predicate`, `destination_x == destination_y == 0`, `width/height` equal to the render node's
   `render_target_width/height`, and is not a member of an existing group: mark the render node's group
   with `descriptor.colors[i].store_action = WMTStoreActionStoreAndMultisampleResolve` and copy
   `resolve_texture/level/slice/depth_plane` from `data->dst`.
2. The resolve node then emits no native pass of its own; it contributes its accesses to the render node
   (the resolve destination becomes a write access of the producing node) so the dependency graph is
   unchanged for every consumer.
3. Anything not matching step 1 keeps today's separate `ResolvePass` (pipeline resolves, DSV-plane
   resolves, decompress/copy-states, partial rects, predicates, dimensions mismatch, `sparse_alias`,
   non-ordinary render nodes, a group whose last member would no longer be the resolve source).

The lane-scoped fallback, if synthesis must not add a planner node kind: relax the `DontCare`
`boundary_before/after` rule (`:5437-5440`) to "a `DontCare` member may only be the group's *last*
member" so a discarding pass can still be the tail of a merged group and the group's store elides. That
is queue-only too, but its safety rests on a D3D12 contract reading (DISCARD ⇒ contents undefined after
the pass) that this lane could not exercise on a GPU, so it is second choice.

**Do not** implement the recording-side fold (`ResolveTextureRegion` setting `resolve_attachment`) in this
wave: the synthesis node's `write_scope` is `src/d3d12/d3d12_command_queue.cpp` only
(`doc/mass-ulw/20261015-rea-redesign-wave1/graph.json`), and the planner fold is the same GPU effect.

## 6. Invariants the increment must not break

1. A merged group's members must have identical attachment views and dimensions; members after the first
   must be `WMTLoadActionLoad`; the group's store action is the last member's (`:285-323`).
2. Any non-attachment dependency *inside* a group must be attachment-only or Vertex→Vertex
   (`InlineRenderDependencySafe`, `:102-105`, enforced `:5776-5779`); TBDR cannot complete Fragment/Tile
   in-pass (Apple rule; contract.json `decision`).
3. A pass carrying `resolve_attachment` or a `DontCare` store must never be merged with anything that
   follows it (`:167-170`, `:5437-5440`); the rebind-elision checks (`d3d12_command_list.cpp:7352-7354`)
   must keep rejecting them.
4. Recorded handles (attachment views, `masked_pipeline_states`, `WMT::Reference`/`Rc` owners) must
   outlive list reset and public PSO release until GPU retirement.
5. Root argument staging is per-list state; the uploaded GPU-heap copy is immutable until the allocator's
   safe retirement (`d3d12_command_allocator.hpp:318-330`).
6. Descriptor values are resolved at submission from the retained heap; a recorded binding identity is
   `(rootsig, heap, base, parameter)` only.
7. The 64-entry budgets (`kRecordedSubmissionBufferBudget`, `kSubmissionDescriptorBudget`) are
   completeness bounds: exceeding one must degrade to "no merge", never to a merge with an incomplete
   access set.
8. Fences order *passes*, never stages inside one pass (contract.json `decision`).

## 7. Risks

- **The retained failure may predate the guard.** `observed-failure.txt` (2 days old) shows a
  Fragment→Fragment internal edge being merged and then emitting an illegal
  `afterEncoderStages: Fragment`; the current tree rejects that merge (`InlineRenderDependencySafe`,
  `:5776-5779`). No git history was available in this lane, so which came first is unresolved. Do not
  treat the merged-group inline barrier as proven safe without the trace check in §8.
- The prior harvest note's grep claim about `WMTStoreActionDontCare` is wrong for the current tree (§3).
  Other line numbers in `RenderPass.txt`/`Resources.txt` may drift similarly; re-check before citing.
- Making a resolve-bearing pass mergeable re-opens the Fragment→Fragment hazard. The fold must set the
  store action on the *producing* node and must not create a new member.
- Metal's store-action resolve is average-only and whole-attachment; MIN/MAX/DECOMPRESS, partial-rect,
  DSV-plane, and single-sample decompress resolves must keep the software pipeline path.
- `native_full_rect` (`d3d12_command_list.cpp:9613-9618`) tests `allocator_->encoder_last`, while
  `EndRenderPass` resolves *before* unbinding and while the pass's render encoder is still
  `encoder_current` (`:10275-10285`, `RejectInRenderPass` returns false while `render_pass_internal_`,
  `:10096-10099`). **Inference:** for the ending-access RESOLVE spelling this may select the software
  resolve path when the native one was intended. Confirm before relying on ending-access resolves for the
  fold.
- No GPU authority: every retained render-pass receipt in §3 is `cpu_only`, and the strongest one that
  could speak to correctness (`phase2e-render-passes.json`) is `FAIL`. Nothing here supports an FPS claim.

## 8. Exact checks

For the increment (run after synthesis, not in this lane):

1. Build/diagnostics on the single changed file (`src/d3d12/d3d12_command_queue.cpp`) and
   `git diff --check`.
2. `scripts/test-owned-graphics.py` render-passes suite with Metal validation enabled (`MTL_DEBUG_LAYER=1`,
   per `AGENTS.md`), on a fresh staged runtime and a fresh prefix.
3. Trace A/B with `OWNED_GRAPHICS_TRACE=1`:
   - expect the `native_render_begin` count (`:6868`) to drop by the number of folded resolves and
     `ResolvePass` labels (`:7551`) to disappear for those resolves;
   - expect **zero** `native_render_inline_barrier` events (`:6950`) with `after == 2` or `before == 2`
     (Fragment) — this is the check that the retained failure's shape cannot recur;
   - expect unchanged `render_pass_begin` (`d3d12_command_list.cpp:10251`) and `native_clear_load` (`:6877`)
     counts.
4. Semantic check for the resolve result itself: the render-passes fixture's readback matrix must pass;
   `phase2e-render-passes.json`'s `graph GPU readback matrix missing, duplicated, reordered or incorrect`
   must not appear, and `phase2e-lifetime-render-passes-v2.json`'s `PASS` must be re-earned on the new
   binary rather than cited.
5. Source-shape checks: `resolve_attachment` must gain exactly one writer if the recording-side variant is
   ever taken; the `WMTStoreActionDontCare` writer set (`d3d12_command_list.cpp:10239/10246/10248`) must
   stay unchanged; `kRecordedSubmissionBufferBudget` and `kSubmissionDescriptorBudget` must stay 64.

## 9. Verification of this handoff

- Every `path:line` above was produced by a read or grep in this session against the working tree.
- Cited evidence files were opened in this session:
  `QA/render-fragment-barrier-contract/{contract.json,prior-rea-registry.txt,observed-failure.txt,apple-intrapass.md}`,
  `doc/mass-ulw/20261015-rea-redesign-wave1/{graph.json,ledger.jsonl}`,
  `build/local/d3dmetal-opt-port-20261009/native-binding-proof.json`,
  `build/local/d3dmetal-opt-port-20261009/phase2-verifier/phase2e-lifetime-render-passes-v2.json`,
  `build/local/d3dmetal-opt-port-20261009/phase2-verifier/phase2e-render-passes.json`,
  `build/local/d3dmetal-opt-port-20261009/gpu-phase2d-render-passes/execution.json`,
  `doc/mass-ulw/01a135f0-ec60-7040-ad16-8458a34bf704/{RenderPass.txt,Resources.txt}`.
- Not verified in this lane (no GPU, no build, no shell): the correctness of the fold, the
  `native_full_rect` reading in §7, and whether the retained failure predates the merge guard. Each has
  an explicit distinguishing check above.
