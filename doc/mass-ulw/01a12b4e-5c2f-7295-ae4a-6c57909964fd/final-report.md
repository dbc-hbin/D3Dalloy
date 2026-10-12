# D3DMetal D3D12 parity audit

Reference: `D3DMetal.framework/Versions/A/D3DMetal`
SHA-256: `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`
Architecture/version: GPTK D3DMetal 4.0b2, x86_64, analyzed with REA/Ghidra 12.1.4.

## Implemented changes

- Added an append-only Metal4 range texture-view copy operation and a non-overlapping contiguous fast path in descriptor-heap copies. Overlap, mixed native/fallback pools, and non-texture slots retain the existing per-slot path. Destination slots are cleared before the native range copy.
- Added an eight-entry bounded command-list-local cache for non-whole resolve render pipelines. The key includes attachment format, raster sample count, resolve/decompress fragment kind, destination DSV plane, depth-write state, and stencil state. Failed creations are not cached.
- Enabled the existing `SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO` in all ten D3D12 shader-common construction sites.
- Made `GetRootSignatureDescAtVersion` return `E_INVALIDARG` for a null output pointer and added a bindless regression assertion.
- Changed automatic complete-PSO disk publication to a bounded, orderly worker. `PipelineCache::Complete` publishes the in-memory flight without waiting for disk I/O; a full optional publication queue drops the disk job rather than blocking PSO completion. Generation, exact-blob, disable/clear, retry, queue-capacity, and shutdown-drain checks remain enforced.
- Added a separate versioned stage-artifact record format keyed by the full canonical stage key. It persists library bytes, exact output-layout metadata sizes, constants, relocations, compiler identity and AIR-versioned key material, with strict bounds, MTLB, alignment, relocation, checksum and identity validation. Stage publication uses a bounded nonblocking joined worker, generation/enable revalidation, shared clear/write locking, and closed unique atomic temp files; CLEAR removes stage records as well as complete-PSO records.
- The stage budget accounts for all owned `dxmt-stage-*` regular files, including orphaned temporary files left by a crashed writer, while excluding directories.
- Stage budget enumeration is conservative: only a missing glob (`ERROR_FILE_NOT_FOUND`) is treated as empty, and any incomplete Win32 enumeration rejects the write.
- Optional persistence worker startup is failure-safe: thread-construction exceptions disable/drop the worker queue instead of escaping pipeline creation.
- Restored stage artifacts have no compiler owner by design; their process-addressable serialized buffers are consumed directly, with an explicit owner-independent library copy path and the existing direct-pointer path for constants/relocations.
- The sample-NaN shader semantic change invalidates old persisted state: complete-PSO version 7 and compute/graphics key revisions 4/8 reject pre-change blobs.
- Stage writer jobs catch optional persistence exceptions, and CPU test gate callbacks are cleared before their stack-owned gates expire.

## REA dispositions

The six reports in this directory cover API/device, shader/binding, resources/synchronization, queue/presentation, RT/workgraphs/temporal, and cache/allocation behavior. Existing owned implementations were not counted as gaps merely because names or internal class names differ. Reference-unsupported paths were separated from missing owned support.

Implemented after the initial disposition:

- RT/workgraph repeated compilation-result reuse now uses device-local single-flight immutable artifacts. RT artifacts copy metallib, constants, relocations, and function-name metadata; work-graph validation remains uncached and constant-bearing artifacts are copied into owned storage for the existing relocation path. Native Metal libraries, functions, and pipelines remain state-object-local.
- Commit-time Present reservation now creates a native ticket during command-buffer recording. The Metal4 bridge retains the drawable and composition state in the recording and resolves it from MTL4CommitFeedback, suppressing stale restart epochs and failed submissions. The existing Presenter retirement path still owns observer cancellation and frame-completion accounting.

Still unknown or bounded: effective GPU queue-priority scheduling, Agility factory behavior inside the reference framework, programmable-sample Tier 2 semantics, and full compiler-dylib behavior. The reference itself reports several compared interfaces as unsupported, including shared-handle opening, atomic-copy methods, lifetime/protected/meta commands, generic SetProgram, and HDR10+ metadata consumption.

## Residual disposition

- Stage-artifact persistence is a verification gap, not a confirmed source defect. The CPU test intentionally isolates queue/single-flight behavior with test hooks; production serialization has bounds, checksum, identity, relocation and atomic-write checks. No code change is justified from the absent real-file fixture alone.
- Metal4 descriptor range copy, present tickets, and RT/Work Graph artifact reuse are also verification gaps, not confirmed implementation defects. The source includes fallback, append-only ABI operations, ownership retention and stale-epoch/error handling. Do not change these paths without a failing native fixture or a new REA contradiction.
- The remaining actionable work is to run the existing source against a compatible ARM64X runtime and add focused serializer/native fixtures when that environment is available.

## Verification

Passed:

- Fresh ARM64 cross build of the changed D3D12, DXGI, WineMetal, D3D12Core, and full project targets passed before the last worker/budget hardening. A final incremental rebuild was attempted but the host lacked `aarch64-w64-mingw32-g++`; the latest source state is compile-unverified by that native cross compiler.
- `python3 graphics-translator/tests/run-pipeline-cache-cpu.py`, including async publication, generation/retry, corruption, bounded queue, stage flight, and publication-queue assertions.
- The same CPU test now covers single-flight retention for work-graph and raytracing artifact kinds (PIPELINE_COMPILED_ARTIFACT_KINDS_PASS).
- `python3 scripts/test-owned-graphics-build.py`
- `python3 scripts/test-owned-graphics-staging.py` was rerun with `-v` but timed out in `test_wrapper_optional_dyld_settings_reach_native_target` after 46 preceding tests passed; staging remains unproven in this host environment.
- `python3 scripts/test-owned-graphics-diagnostics.py`
- `git diff --check`
- The first review found stage-control and budget races; the corrected tree was rebuilt and the CPU test now covers synchronized worker tracing and a real clear completion assertion.
- The second review found assertion-enabled fallback reachability, stage-handle lifetime, metadata/relocation validation, and test-hook lifetime defects; those were corrected before the final review.
- Final read-only gate `FinalGateAfterAllFixes-2` approved the corrected tree with confidence 0.91 and no concrete source/test blockers.

Not completed:

- Fresh GPU fixture execution for descriptor-view-pool, copy-resolve, NaN sampling, or stage restore. The available retained Wine runtime is ARM64X while this fresh build is plain ARM64, so staging stops at the repository ABI mismatch check.
- Actual game captures, FPS, or frame-time measurements. Clangd diagnostics were not usable because the checked `build/compile_commands.json` is empty; the cross build is the authoritative compile check.
