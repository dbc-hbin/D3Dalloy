# Glossary

Terms used by this repository's owned D3D12 implementation. A D3D12 object is not interchangeable with a similarly named Metal object.
Current reported support belongs in the [implementation status](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md), not this glossary.

## Recording, submission, and completion

| Term | Meaning here |
|---|---|
| Command-list recording | Typed encoder passes and command records accumulated while a D3D12 list is open. Void recording methods retain the first error for `Close`. |
| `Close` | Seals recording; it does not submit or complete GPU work. |
| Bundle | Recorded commands replayed through the parent list's recording path, with D3D12 state inheritance rules. See [bundles](../graphics-translator/D3D12-BUNDLES.ko.md). |
| Queue task FIFO | Per-queue ordered `Execute`, `Signal`, `Wait`, and `Present` tasks. A submission worker consumes retained recording snapshots, not mutable command-list state after the API returns. |
| Commit | Publication of native work to a Metal queue. A successful commit is not evidence of GPU completion. |
| Submission ring | Bounded storage for in-flight work, with separate publication and retirement cursors. Backpressure prevents reusing a live slot. |
| Retirement | Ordered processing of native GPU completion, result validation, and release of retained owners. API calls, commit return, and retirement are different boundaries. |
| Replay | Executing an already recorded command list again; per-execution scratch and ownership cannot be assumed to be one-shot recording data. |

Sources: [queue order](../graphics-translator/QUEUE-ORDER.ko.md),
[queue task FIFO](../src/d3d12/owned_queue_task_fifo.hpp),
[submission ring](../src/d3d12/owned_submission_ring.hpp).

## Fences and ordering

- **Logical fence value:** the value exposed by D3D12. CPU/queue signals can rewind it; it is not a monotonically increasing native token.
- **Completion latch:** a registered wait remembers a qualifying completed value even if a later signal rewinds the current value. Registration after an old threshold crossing must not inherit that old crossing.
- **Shared-fence observer:** a process-local observer of the shared logical timeline. Per-observer threshold history preserves crossings that occur between observations.
- **Native event/token:** a monotonic Metal synchronization mechanism. Reservation order is not completion order. For shared D3D12 fences, the native shared event wakes observers; validated retirement publishes GPU logical completion.
- **Metal fence:** an encoder dependency primitive, not the public D3D12 logical fence object.
- **Device-removal state:** the terminal state reported through existing error/removal infrastructure; its public fence behavior is described in [fence events](../graphics-translator/FENCE-EVENTS.ko.md). Whether an API actually triggers that state is a separate implementation contract.

D3D11 retains a separate native fence path. See [multiqueue](../graphics-translator/MULTIQUEUE.ko.md) for D3D12 producer/consumer ordering.

## Shaders and pipelines

| Term | Meaning here |
|---|---|
| DXBC | Direct3D shader container/bytecode handled by the owned compiler. SM5 shader instructions and DXIL payloads are distinct inputs. |
| DXIL | Shader Model 6.x LLVM-based IR plus DirectX metadata/operations. It must be lowered; Metal does not execute it as-is. |
| AIR | Apple intermediate representation produced by the owned DXIL/DXBC lowering. AIR stage metadata and intrinsic ABI must match the native Metal compiler. |
| metallib | Serialized Metal library emitted by the owned compiler and loaded by Metal. Not a D3D12 PSO cache blob or a native pipeline archive. |
| `SM50*` ABI | The historical name of the public owned compiler interface, used by DXBC and DXIL paths. The name is not the reported Shader Model ceiling. |
| Immutable constant pool | Per-PSO constant bytes and relocation metadata. Runtime addresses are reconstructed on load rather than serialized as reusable GPU pointers. |
| PSO | D3D12 pipeline state. Translation may require several native pipeline/shader variants for indirect, statistics, MSAA, or other execution paths. |
| `GetCachedBlob` / `CachedPSO` | The owned per-PSO serialization contract. Restoring skips owned shader translation but still creates native libraries/functions/pipelines. |
| Pipeline library | `ID3D12PipelineLibrary/1` named entries over owned PSO blobs, with identity and descriptor validation. |
| Native pipeline archive | Metal's native compiled-pipeline reuse mechanism; not what the current owned pipeline-library format stores. |

Sources: [compiler ABI](../src/airconv/airconv_public.h),
[DXIL lowering contract](../src/airconv/dxil_converter.hpp),
[pipeline cache](../src/d3d12/d3d12_pipeline_cache.hpp),
[pipeline library](../src/d3d12/d3d12_pipeline_library.cpp).
Mutable LLVM IR/context belongs to one compilation; immutable published shader data has a separate lifetime.

## Resource and binding model

- **Root signature:** the application's resource-binding layout and visibility contract. **Root arguments** are values/descriptors used by a particular recording; they are not the signature itself.
- **Descriptor heap/table:** D3D12 descriptors and their indexed binding ranges. They are not interchangeable with a Metal heap or argument table.
- **Resource view:** the typed/ranged interpretation of a resource. Logical DXGI layout may differ from native backing layout.
- **Memory heap:** storage for placed resources. A committed resource owns its storage relationship; a reserved resource obtains mappings separately.
- **Reserved/tiled resource:** logical address space whose 64 KiB tile mappings are controlled separately from resource creation. D3D and native sparse tile shapes must preserve the same semantics.
- **Residency:** the API/accounting and native-resource availability contract. It is distinct from object reference ownership, sparse mapping, and physical dedicated VRAM.
- **Upload arena:** allocator-owned retained chunks for recording payloads, resolved without moving earlier CPU pointers or GPU addresses. The initial 4 MiB chunk is not a total recording-capacity limit. Growth, reset, lifetime and allocation-failure behavior follow the [core contract](../graphics-translator/D3D12-CORE-CONTRACTS.ko.md#command-list-생성기록제출).

See [resources](../graphics-translator/D3D12-RESOURCES.ko.md),
[formats/views](../graphics-translator/CAPABILITIES.ko.md), and
[core contracts](../graphics-translator/D3D12-CORE-CONTRACTS.ko.md).

## Support and evidence

- **Feature level:** a required set of API capabilities. A single advanced feature cannot establish the whole level.
- **Shader Model:** a versioned shader contract. Accepting a higher-version input containing a few implemented operations does not prove complete support for that model.
- **Tier/option:** a specific queried capability and its required semantics, separate from the shader input-version limit.
- **Source implementation:** a code path exists. It may still be incomplete or lack runtime proof.
- **Fixture acceptance:** observed results for named inputs, runtime, architecture, GPU/OS, and validation mode. Not blanket conformance or game compatibility.
- **Mixed-cut evidence:** an aggregate using explicitly identified results from different runtime snapshots, not a single-snapshot full rerun.
- **API-only validation:** Metal API checks without Shader Validation; it must not be reported as passing both.
- **Historical provenance:** a record of the original source/build or a past observation. An old artifact path does not imply the file still exists.

## Native architecture and cross-DLL boundaries

Three independent axes apply:

1. **Native host architecture:** Mach-O architecture of the Wine host, Unix libraries, compiler, and Metal bridge.
2. **Windows module ABI:** AMD64, I386, plain ARM64, ARM64EC, or ARM64X PE ABI/metadata.
3. **Guest instruction set:** the application's x64/x86/native ARM64 instructions and the actual execution/emulation route.

A linked ARM64EC DLL, native parent process, or native compiler probe does not prove Rosetta-free guest execution.
[ARM64/FEX](ARM64-FEX-IMPLEMENTATION.md) defines the separate gates.

**Unix call** means a numbered Wine PE↔native entry with coordinated parameter layouts, wrappers, handlers, and call tables;
it is not permission to pass arbitrary C++ objects across architectures.

**Owned temporal ABI** is the project COM boundary between `nvngx.dll` and `d3d12.dll`.
It uses the descriptors/interfaces in [d3d12_interfaces.hpp](../src/d3d12/d3d12_interfaces.hpp), not private Metal/D3DMetal pointers.
Evaluation records retained work on the application's command list; actual execution/history/retirement follows submission.
See [NGX integration](../graphics-translator/NGX-INTEGRATION.ko.md).
