# Auxiliary GPU work and dependency edges before removing queue-wide fence waits

Lane: `auxiliary-scheduler-harvest` (`rea-redesign-wave1-20261015`). Read-only harvest; no source, test, fixture, runtime, or commit was produced outside this file. Every line number below was read in this session. Claims that are inference rather than observation are marked **inferred**.

Scope: which auxiliary GPU work exists in the D3D12 queue, which of it the submission planner cannot see, and what has to be described before any further fence wait can be removed.

---

## 1. Observed source facts

### 1.1 The two synchronization mechanisms being targeted

1. **GPU-side shared fence.** `MTLD3D12CommandQueueImpl::fence_` is a single `WMT::Fence` per queue (declared `src/d3d12/d3d12_command_queue.cpp:622`, created `:1300`). Every native encoder in the replay loop brackets its work with `waitForFence(fence_, stages)` / `updateFence(fence_, stages)`. Because one MTLFence carries one monotonic value, a wait in encoder N+1 cannot retire until the update of encoder N executes.
2. **CPU-side retirement gates.** `CommittingScope::reset()` publishes a submission sequence, and `submission_ring_.waitUntilRetired(producer)` blocks the submission thread until GPU retirement (`src/d3d12/owned_submission_ring.hpp:56-62`; ring API `:25-62`).

### 1.2 What decides whether a node still takes the fence

`src/d3d12/d3d12_command_queue.cpp:5915-5925`:

```cpp
bool native_fence_required = true;
if (current->type != EncoderType::Null) {
  const auto index = replay_node++;
  const auto &node = plan.nodes[index];
  ...
  native_fence_required = !node.access_complete || !node.dependencies.empty() ||
      !node.scopes_before.empty() || !node.clears_before.empty();
  if (!native_fence_required)
    owned_graphics::Trace("native_fence_wait_skipped", ...);
```

The asymmetry matters: **a dependency edge forces a fence; it does not replace one.** A wait disappears only for a node that is simultaneously (a) `access_complete`, (b) has zero dependency edges, (c) has no scope boundary, and (d) has no pending clears. Describing accesses alone can therefore never remove a wait - it only creates the precondition (b) proof. Actual removal additionally requires that the producing work be inside the same native encoder (group member) or that no hazard exists at all.

### 1.3 What the planner describes today

Planner loop `src/d3d12/d3d12_command_queue.cpp:5393-5858`; node construction `:5400-5490`. Branches exist for exactly four encoder types:

| Encoder type | Planner branch |
| --- | --- |
| Render | `:5421-5446` |
| Compute | `:5447-5460` |
| Blit | `:5462-5468` |
| Clear | `:5469-5481` |

`SubmissionNode::access_complete` defaults to `false` (`:87`). Every other `EncoderType` - `Null`, `Resolve`, `PredicatedTextureCopy`, `Temporal`, `Query`, `Raytracing`, `StandardSwizzle`, `WorkGraph`, `FenceBarrier`, `Diagnostic` (enum `src/d3d12/d3d12_command_encoder.hpp:37-52`) - is therefore opaque by construction, clears its accesses (`:5484`), and always takes the fence.

Dependency frontier and edge creation: `:5708-5745`. History is keyed on `access.allocation` (`:5711`, `:5718`); edges are inserted at `:5723-5731`; the partition resets on a boundary or an incomplete node (`:5715`, `:5847`). Group merge eligibility and commutativity checks: `:5764-5808`.

### 1.4 Auxiliary GPU work inside an otherwise eligible node

These helper paths hardcode their own fence wait/update, unconditionally, regardless of `native_fence_required`:

| Helper | Definition | Fence pair |
| --- | --- | --- |
| Stream-output assembly | `EncodeStreamOutputAssembly` `:1627-1677` | wait `:1630`, update `:1675` |
| ROV marker textures | `PrepareROVMarkers` `:1706-1763` | wait `:1757`, update `:1758` |
| MSAA object capture | `EncodeObjectCapture` `:1764-1797` | wait `:1777`, update `:1779` |
| MSAA vertex capture | `EncodeMSAAProducer` `:1834-2042` | waits `:1910`, `:1946`, `:1965`, `:1983`; updates `:1930`, `:1952`, `:1979`, `:2032` |
| MSAA replay | `EncodeMSAARender` `:2043-` | waits `:2349`, `:2469`, `:2490`, `:2526`, `:2539`, `:2560`, `:2574`, `:2600`, `:2647`, `:2715`; updates `:2371`, `:2471`, `:2497`, `:2532`, `:2547`, `:2567`, `:2581`, `:2626`, `:2655`, `:2744` |
| Swizzle / cast transfer | `EncodeWritableMSAA` `:3547`, `EncodeStandardSwizzle` `:3803`, `EncodeBlockCast` `:4008`, `EncodeReservedVolumeRender` `:4541` | per-helper |
| Swizzle pass begin/end | `BeginStandardSwizzlePass` `:4809-4882`, `EndStandardSwizzlePass` `:4884-4941` | baseline copy wait `:4860` / update `:4861`; dirty-flag fill wait `:4920` / update `:4922` |
| AS helper kernels | `EncodeASHelper` `:4944-4989` | wait `:4948`, update `:4987` |
| Query scratch fill | inside Render case `:6853-6864` | wait `:6854`, update `:6861` |
| Query reset | inside Render case `:7073-7087` | wait `:7074`, update `:7082` |
| Query accumulate | inside Render case `:7089-7120` | wait `:7090`, update `:7116` |
| Buffer-attachment copy-in | `:6725-6742` | wait `:6726`, update `:6741` |
| Buffer-attachment copy-out | `:7049-7067` | wait `:7050`, update `:7066` |
| Indirect resolver draw | inline in the render encoder `:6966-7035` | render encoder wait `:6904-6905`, update `:7041` |

The outer encoders use the gated form: render wait `:6904-6905` / update `:7041`; blit wait `:7242-7244` / update `:7248`; compute wait `:7416-7418` / update `:7442`; clear wait `:6613` / update `:6619` and clear batch wait `:5866` / update `:5872`.

### 1.5 Auxiliary work that also gates the CPU

`read_atomic_buffer` `:5102-5116` ends with a full queue drain:

```cpp
const auto producer = scope->seq; scope.reset(); submission_ring_.waitUntilRetired(producer);   // :5115
```

Call sites (each is a queue-wide fence wait in CPU form):

| Site | Line | Purpose |
| --- | --- | --- |
| Atomic64 phase retire | `:4995-4999` | phase context handoff |
| Atomic64 exclusive acquire | `:5070-5073` | import/export gate |
| Work graph | `:5971-5974` | executor handoff |
| Predicated texture copy | `:6034-6035` | predicate snapshot |
| SO indirect count preflight | `:6071-6072` | GPU-written draw count |
| SO indirect draw preflight | `:6097-6100` | GPU-written draw args |
| Mesh capture preflight | `:6137-6140` | capture sizing |
| Mesh grid readback | `:6193` plus `:6209-6221` | 16-byte grid copy then wait |
| MSAA producer | `:6365-6372` | producer handoff |
| RTAS source header | `:6427-6431` | AS header readback |
| Compute/ray indirect args | `:7321-7346`, `:7364-7392` | `read_atomic_buffer` per argument/count/predicate/record |

### 1.6 List-side recording metadata that owns this work

- Visibility scratch allocation: `src/d3d12/d3d12_command_list.cpp:1738` (hazard tracking `Untracked`, `:1737`) and `:2259`.
- Visibility targets (non-interval path): `:2146-2147`; interval path: `:2264-2267`; interval count: `:2141-2144`; interval eligibility: `:2253-2254`.
- Query reset target: `:8100`, with `visibility_interval_dirty` `:8102` and `submission_group_safe = false` `:8103`.
- Buffer RTV staging: `:2311` (`rt.buffer_attachment`), `:2316-2319` (`rt.buffer_backing`, offset, bytes-per-row, width).
- MSAA draw record: `:3371`; SO indirect record: `:8462`; capture-indirect record: `:8689-8690`.
- Swizzle table capture: `:2656-2659` (graphics), `:4237-4240` (compute).

---

## 2. Retained D3DMetal / REA facts and evidence paths

### 2.1 Pristine reference identity

- GPTK 4.0b2, Mach-O x86_64, SHA256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`.
- Path (verified present): `build/local/zzz-dlss-fix-20261009/precompile-run-01/binding-diagnosis/reference/D3DMetal.framework/Versions/A/D3DMetal`.
- Recorded in `graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md:548-560`.

### 2.2 Retained REA evidence paths

- `build/local/d3d12-gap-rea-20261010/coverage-final.json` - provider Ghidra 12.1.4, artifact digest equal to the pristine SHA above.
- `build/local/d3dmetal-rea-audit-DxksP9/evidence-framework-final.json` - 42.3 MB retained evidence framework.
- Reference symbols and evidence IDs already harvested in `doc/mass-ulw/01a12b4e-5c2f-7295-ae4a-6c57909964fd/queue-parity.md`:
  - `MPLCommandList::StageBarrierImpl` `0x1bb66`, `ev_4854eda360771ce939cccf06cb74fb5f0882d136a0edf94657fc0bcf1f0912d1` - accumulates stage masks and predecessor encoder indices rather than one global barrier.
  - `SetupExecuteIndirect` `0x1083a`, `ev_794ef1dae75332ce4f27ed66433f343e62b7d3a8fbe3864ee8ce1043de3de0b0`.
  - `BeginVisibilityQuery` `0x1c95a`, `ev_85dc81da87cc294cfbb93eaf867a2652836f45a260427a2a8929edbb963a8bc0`; `EndVisibilityQuery` `0x1ca42`, `ev_05e1737b54648cb37d3b953f1921e17fe55ee86a20b70ea5f2cbb252717287b1`.
  - `MPLCommandReplayer::BuildEncoderSubmissionOrder` `0x2b846`, `MergeEncoders` `0x2dd08`, `MergeColorAttachments` `0x2e1d0`, `FlushClears` `0xec42` - responses truncated at the tool boundary, so full equivalence to the owned planner is **unknown**, not established.

### 2.3 Reference barrier contract and the retained failure

`QA/render-fragment-barrier-contract/contract.json` (same pristine SHA, hash verified before the REA open):

- `dependent_fragment_uav_requires_native_render_split: true`
- `same_pass_fragment_completion_barrier: "unsupported"`
- `same_pass_fence_replacement: "invalid; fences order different passes"`
- Legal mechanisms: producer fence update, `endEncoding`, new render encoder, consumer fence wait; or `endEncoding` plus a consumer queue barrier with Device visibility.
- Prior REA evidence: `MPLCommandList::Draw` `0x132a6` `ev_1948210b...`, `ReplayInlineMemoryBarrier` `0x36263` `ev_d0a29294...`, `ReplayBackwardStageBarrier` `0x3632c` `ev_061e47c3...`, `RenderEncoder::End` `0x29110` `ev_d7c48827...`.

Retained observed failure (`QA/render-fragment-barrier-contract/observed-failure.txt`, source `build/local/d3dmetal-opt-port-20261009/gpu-phase2c-render-passes/stderr.log`): submission 3, encoder 210, `submission_access_edge` WAW `stages_before=2 stages_after=2` at events 1371/1372, `submission_render_merged members=2` at 1373, then

```
[MTL4DebugCommandEncoder barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:]:213: failed assertion
afterEncoderStages must be a valid combination of MTLStages for the current encoder type
(MTLStageVertex | MTLStageObject | MTLStageMesh).
```

This is a real regression of the merge/inline-barrier contract, not a hypothesis. It is the strongest correctness constraint on any change in this area.

### 2.4 Other retained queue evidence

- `build/local/d3dmetal-opt-port-20261009/rr-sync-native.log` - RR reuse/sync run with Metal API and GPU validation enabled.
- `build/local/d3dmetal-opt-port-20261009/phase3e-rr-review.json` - `status: PASS`.
- `doc/GPU-BOTTLENECK-AUDIT-EXHAUSTIVE-2026-10-14.ko.md:71-74` maps the CPU waits and GPU-specialized work families; `:129` states the shared fence wait/update can be wider than the real RAW/WAR/WAW; `:299-301` states the actionable finding is a dependency/merge contract problem that must be fixed before fence cost is evaluated as a performance candidate.
- `doc/mass-ulw/01a135f0-ec60-7040-ad16-8458a34bf704/QueueSync.txt:110` records the open reference question: does D3DMetal place one per-queue fence wait/update around every pass, or derive per-resource dependencies? **Unknown** - the reference bodies were truncated.

---

## 3. Missing `SubmissionNode` edges, per auxiliary family

For each family: the retained owner metadata, the native work, and the `SubmissionAccess` entries the planner never receives. Today the node is `access_complete=false`, so the absence is masked by the fence; it becomes a silent hazard the moment the gate is relaxed. None of the allocations below can be resolved by `AppendSubmissionBuffer` (`:188-191`, which requires `LookupBufferByVA`).

| Family | Retained owner | Native work | Allocations never appended |
| --- | --- | --- | --- |
| Query visibility scratch / reset / accumulate | `RenderEncoderData::visibility_scratch` `d3d12_command_encoder.hpp:643`, `visibility_targets` `:644`, `QueryVisibilityTarget` `:183-190` | scratch fill `queue:6853-6864`; per-target reset fill `:7073-7087`; per-target accumulate dispatch reading scratch and writing destination `:7089-7120`; native visibility writes during the render pass | `visibility_scratch` (read+write, Fragment and Dispatch); each `target.destination` byte range `index*8 .. +8` (write, Blit/Dispatch) |
| Query encoder ops | `QueryEncoderData` `:803-818`, `QueryEncoderOp` `:781-801` | resets/fills, resolve copies, binary-occlusion and SO-statistics dispatches at `queue:7572-7700` | `counters`, `source`, `destination`, `snapshot_offset` ranges; no planner branch exists at all |
| Swizzle / cast | `StandardSwizzleEncoderData` `:117-122`, `StandardSwizzleTableUse` `:99-108` | `EncodeStandardSwizzle` `:3803`, `EncodeBlockCast` `:4008`, `EncodeReservedVolumeRender` `:4541`, `EncodeWritableMSAA` `:3547`, `Begin/EndStandardSwizzlePass` `:4809-4941` | canonical shadow `resource->buffer` over `swizzle_layout->size_bytes` (read+write); `use.baseline` copy source; `dirty_flags`; the cast/MSAA sibling views |
| MSAA capture / replay | `MSAADrawData` `:522-596`; `EncodeMSAAProducer` `:1834`, `EncodeMSAARender` `:2043`, `EncodeObjectCapture` `:1764` | capture compute, tile/primitive replays, object records, 16-byte grid readback `:6209-6221` | capture scratch/records/tiles/positions, `mesh_grid_buffer`, `object_records`, `statistics_claim`; MSAA attachments cannot be described at all - `AppendSubmissionTexture` rejects `sampleCount() != 1` `:206` |
| RTAS | `RTASEncoderData` `:840-874` | prepass compute `:7189-7201`; native build/refit/copy plus `compactedSize` `:7205-7217`; postbuild helpers `:7224-7234`; `EncodeASHelper` `:4944` | `scratch`, `header`, `liveMirror`, `sourceInstances`, the instance buffer, `destination.contributionBuffer()`, postbuild destinations, and the helper's parameter/geometry/job tables |
| Indirect resolver | `SOIndirectEncoderData` `:431-447`; resolver draw `:6966-7035` | ICB resolve draw in the vertex stage then `ExecuteCommandsInBuffer`; preflight readbacks `:6071-6072`, `:6097-6100` | `execution.storage` (read+write, Vertex), `execution.icb` (read+write), `static_sampler_buffer`, per-draw `scratch`/`positions`/`index_buffer`, `statistics_heap`; `MeshCaptureIndirectData` `:457-517` holds the same class of buffers |
| Buffer-attachment copies | `RenderEncoderColorAttachmentData::buffer_backing` / `buffer_attachment` `:146-147` | copy-in `:6725-6742`, render into the staging texture, copy-out `:7049-7067` | `buffer_backing` over `[buffer_offset, buffer_offset + buffer_bytes_per_row)` (read then write); the private staging texture `buffer_attachment` (write then read). `OrdinaryRenderRejection` rejects the node outright: `return "attachment_buffer"` `:165` |
| Stream-output assembly | `StreamOutputDrawData` `:391-427` | `EncodeStreamOutputAssembly` `:1627-1677` | `scratch`, `positions`, `heap`, `counters_heap`, `predicate_heap`, `statistics_heap`, and the four `targets` / `filled_sizes` ranges |

### 3.1 Two structural gaps behind the whole table

1. **No allocation-identity append path.** `AppendSubmissionAccess` already takes `const void *allocation` directly (`:129-142`, range budget rejection `:139`), but every caller resolves through `AppendSubmissionBuffer` to `device->LookupBufferByVA` (`:188-191`). Translator-internal buffers (`visibility_scratch`, ICB storage, AS scratch/header/mirror, staging textures, capture scratch) have no D3D12 VA, so there is no path to describe them.
2. **The access model rejects the shapes several families need.** `AppendSubmissionTexture` / `AppendSubmissionBuffer` return false for `sampleCount() != 1`, `canonicalHeap`, `Shared`, `PlacementSparse` (`:204-215`) and set `sparse_resource_used`, which forces `access_complete = false` (`:5483`). MSAA and sparse-aliased attachments therefore need a model extension, not just new call sites.

---

## 4. Recommended smallest next implementation increment

**Describe the query-visibility family as real `SubmissionAccess` entries and add the matching planner branch, without changing `native_fence_required`.**

Why this family first:

- Its allocations are already retained on the encoder metadata (`visibility_scratch`, `visibility_targets`; header `:643-644`), so no new ownership is introduced.
- The work is fully determined at record time - interval count, target indices and source offsets are recorded, not GPU-produced.
- Its destinations are byte-range buffers, which `AccessOverlaps` already handles (`:67-71`), and they are not MSAA, sparse, canonical-heap, or Shared, so no access-model extension is required.
- The hazard class is a plain byte-range WAW/RAW between the scratch fill/accumulate and the render pass's native visibility writes.

Bounded steps (single file, `src/d3d12/d3d12_command_queue.cpp`):

1. Add `bool AppendSubmissionAllocation(SubmissionNode &node, const void *allocation, uint64_t byte_length, uint64_t offset, uint64_t bytes, bool writes, uint64_t stages)` beside `AppendSubmissionBuffer` (`:188`), calling `AppendSubmissionAccess` directly so no VA lookup is needed. Reuse `kSubmissionRangeBudget` (`:61`) and the existing `access_range_budget` rejection (`:139`).
2. In the Render planner branch (`:5421-5446`), when `data->visibility_scratch` is set, append it once as read+write for Fragment and Dispatch; for each `data->visibility_targets` entry append `{destination, offset = index * sizeof(uint64_t), bytes = sizeof(uint64_t), writes = true, stages = Blit|Dispatch}`.
3. Do **not** touch `node.access_complete`, `node.merge_eligible`, `OrdinaryRenderRejection`, or the gate at `:5921-5922`. The immediate observable effect is new `submission_access_edge` events covering the scratch and destination allocations, and a correct partition reset (`:5715`, `:5847`) whenever such a node is opaque.
4. Follow-up increment (explicitly not this one): add the `EncoderType::Query` planner branch, then relax the gate for a node whose accesses are complete **and** whose dependency list is empty.

Out of scope for this increment: MSAA (`sampleCount() != 1`), sparse/Shared/canonical aliasing, RTAS, swizzle/cast, indirect resolver, buffer-attachment copies, and the CPU readback gates in 1.5. Each needs the access model extended first, and each readback gate additionally needs the GPU-produced value to become consumable on the GPU timeline rather than through a CPU wait.

Keep `EncodeAliasFlush` (`:5904-5907`, `:5942-5945`, `:7576-7579`) outside the edge model. It is a shared-event signal/wait for sparse alias visibility (`d3d12_reserved_resource.hpp` semantics), not an allocation dependency.

---

## 5. Invariants

1. **Access completeness is not merge eligibility.** `SubmissionNode` documents them separately (`:84-87`); they are set separately at `:5424` and `:5443`. A node may be fully described and still require its own native encoder.
2. **Opaque nodes break the frontier.** An incomplete node clears its accesses (`:5484`) and resets the dependency partition (`:5715`, `:5847`). Any new access path must preserve this: a later node must never form an edge across an opaque node.
3. **The gate stays conservative.** `native_fence_required` (`:5921-5922`) must keep firing for any node with a dependency, scope, or clear. Adding accesses can only add edges, never remove waits.
4. **Nonattachment inline barriers are Vertex-to-Vertex only.** `InlineRenderDependencySafe` `:102-105`. A dependent fragment producer must end its render encoder. Retained authority: `QA/render-fragment-barrier-contract/contract.json` (`dependent_fragment_uav_requires_native_render_split: true`, `same_pass_fence_replacement: "invalid; fences order different passes"`).
5. **Stage mapping direction.** The producer edge goes into `stages_after` and the consumer into `stages_before` (`:6941-6946`); the contract's `source_obligations` name this explicitly. The retained failure at `QA/render-fragment-barrier-contract/observed-failure.txt` is a violation of exactly this rule.
6. **Wait/update stage masks must cover what the encoder produces and consumes.** Render: `wait_stages` `:6896-6905`, `update_stages` `:7036-7041` (mesh widens to Object and Mesh, stream-output capture widens to Vertex). Blit `:7242-7248`, compute `:7416-7442`, clear `:6613-6619`.
7. **Sparse alias ordering is not a fence edge.** It uses the alias-flush event plus shared-event visibility (`:5901-5907`, `:5942-5945`).
8. **A fenced submission rejects every render and compute node.** `fenced` at `:5292` forces `:5423`, `:5451`, `:5483`. Cross-queue `Wait`/`Signal` therefore keeps whole submissions conservative.
9. **Translator-internal buffers have no VA.** Access identity must come from the retained allocation, never from `LookupBufferByVA`.
10. **Ownership must not weaken.** Allocations stay alive through retirement via the `InflightCommandBuffer` vectors (queue `:878-885`: `so_draws`, `so_indirect_buffers`, `so_indirect_icbs`, `msaa_buffers`, `msaa_textures`). Adding edges must not replace those retentions.
11. **Recorded opaque endpoints stay boundaries.** `endpoint_reason == Opaque` forces `boundary_after` (`:5482`); new auxiliary accesses must not clear it.
12. **Unknown stays fallback.** Any encoder whose access analysis is incomplete must keep the existing fence path. No gate flip without both access completeness and an empty dependency list.

---

## 6. Exact checks

### Static / diagnostic

- `clangd` diagnostics on `src/d3d12/d3d12_command_queue.cpp` using the compilation database under `build/` (`.clangd` sets `CompilationDatabase: build/`). Present and verified: `build/local/build/compile_commands.json`, `build/local/arm64ec-native/compile_commands.json`.
- Documented build (README `:107-115`): `ninja -C build/local/build -j8`. `build/local/build/build.ninja` is present and verified.
- `git diff --check`.

### Behavioral

- **Edge visibility.** Run a fixture with `OWNED_GRAPHICS_TRACE=1` and confirm the new `submission_access_edge` events appear for the scratch/destination allocations, that `submission_access_opaque` still fires for the same nodes, and that `native_fence_wait_skipped` does **not** begin firing for them - this increment must not remove a wait.
- **Retained-failure regression guard.** Re-run the render-pass fixture that produced `QA/render-fragment-barrier-contract/observed-failure.txt` and assert no `MTL4DebugCommandEncoder barrierAfterEncoderStages` assertion and no `submission_render_merged` with `members=2` spanning a WAW edge. Command shape:

  ```sh
  python3 scripts/test-owned-graphics.py --runtime <fresh staged runtime> \
    --suite <affected suite> --fixture <built fixture> --out <fresh output> --validation
  ```

- **Query-family behavior.** A fixture that issues occlusion/timestamp queries inside a render pass with retained visibility intervals; assert `query_scratch_fill`, `query_reset`, and `query_accumulate` events still fire and query results are byte-identical to the pre-change run.
- **CPU gates untouched.** For this increment, the `submission_ring_.waitUntilRetired` sites listed in 1.5 must be unchanged; a diff touching them is out of scope.

---

## 7. Unknowns and risks

- The reference question in `QueueSync.txt:110` remains open: whether D3DMetal derives per-resource dependencies or also uses a coarse per-queue fence. The retained REA responses for `BuildEncoderSubmissionOrder`, `MergeEncoders`, and `FlushClears` were truncated, so reference equivalence is unknown rather than established. Do not claim reference parity here.
- No measurement exists for the cost of the shared fence. The audit document itself (`doc/GPU-BOTTLENECK-AUDIT-EXHAUSTIVE-2026-10-14.ko.md:299-301`) states the actionable finding is a correctness/contract problem first.
- The retained failure proves the current merge/inline-barrier contract can already be wrong. Adding edges raises the chance that a previously unmodeled hazard now forms an edge - that is the intent, but it means the increment must be trace-verified before any gate flip, and the regression guard above is mandatory.
- Buffer-attachment and MSAA families cannot be described with today's access model at all; treat them as a model extension, not a call-site addition.
- `fenced` submissions and `ReservedResource::AnyLive()` keep whole submissions conservative; a game frame using D3D12 barriers or sparse resources will not benefit from any of this until those gates are addressed separately.

Skipped: no reference-binary execution, no REA re-open, no build, and no runtime run in this read-only lane. Risk: the auxiliary families outside the recommended increment (MSAA, RTAS, swizzle, indirect resolver, buffer attachments) still have no dependency-edge model, so their fences cannot be removed by this step alone.
