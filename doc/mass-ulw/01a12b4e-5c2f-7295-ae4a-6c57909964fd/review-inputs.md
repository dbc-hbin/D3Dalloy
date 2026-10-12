# Final review inputs

## Goal
Reassess REA-confirmed D3DMetal behavior without treating existing tests as product semantics; implement statically reasonable D3D12 translation/optimization differences; update tests; verify.

## Reference
`$D3DMETAL_REFERENCE`
SHA-256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`.
Resolve `$D3DMETAL_REFERENCE` to the local reference copy; the digest, not the machine-specific path, identifies the artifact.
REA evidence and six parity reports are in this directory. Cache evidence is `cache-parity.md`.

## Final changed product/test files
- `graphics-translator/tests/owned-d3d12-bindless.cpp`
- `graphics-translator/tests/pipeline-cache-locks.test.cpp`
- `scripts/test-owned-graphics.py`
- `src/d3d12/d3d12_command_list.cpp`
- `src/d3d12/d3d12_command_queue.cpp`
- `src/d3d12/d3d12_descriptor_heap.cpp`
- `src/d3d12/d3d12_pipeline_cache.cpp`
- `src/d3d12/d3d12_pipeline_cache.hpp`
- `src/d3d12/d3d12_pipeline_compute.cpp`
- `src/d3d12/d3d12_pipeline_graphics.cpp`
- `src/d3d12/d3d12_root_signature.cpp`
- `src/d3d12/d3d12_shader_cache.cpp`
- `src/d3d12/d3d12_shader_cache.hpp`
- `src/d3d12/d3d12_state_object.cpp`
- `src/d3d12/d3d12_work_graph.cpp`
- `src/d3d12/d3d12_work_graph.hpp`
- `src/winemetal/Metal4.hpp`
- `src/winemetal/Metal4FX.hpp`
- `src/winemetal/unix/cache.c`
- `src/winemetal/unix/winemetal_metal4.c`
- `src/winemetal/unix/winemetal_metal4_fx.mm`
- `src/winemetal/winemetal_metal4.h`
- `src/winemetal/winemetal_metal4_fx.h`

## QA matrix
- CPU cache contract: `python3 graphics-translator/tests/run-pipeline-cache-cpu.py` -> PASS, including async publication, generation/retry, corruption, stage flights, nonblocking bounded queues, synchronized worker tracing, deterministic hook lifetime, and a real clear assertion (phases=3, clear=1).
- Repository build regression: `python3 scripts/test-owned-graphics-build.py` -> PASS (16 tests). Diagnostics regression: `python3 scripts/test-owned-graphics-diagnostics.py` -> PASS. The staging suite was rerun with `-v` and timed out at `test_wrapper_optional_dyld_settings_reach_native_target` after the preceding 46 tests passed; staging is UNPROVEN in this environment.
- Fresh full ARM64 cross build: the configured build passed before the last worker/budget hardening. A final incremental rebuild was attempted but the host no longer had `aarch64-w64-mingw32-g++` on PATH; the latest source state is therefore compile-unverified by the native cross compiler.
- GPU fixture execution: UNPROVEN. Fresh plain ARM64 build cannot stage against retained ARM64X Wine runtime; repository ABI check blocks execution.
- Game performance: UNPROVEN; no game capture/FPS measurement.
- Diff hygiene: `git diff --check` -> PASS.
- Final read-only gate `FinalGateAfterAllFixes-2`: APPROVE, confidence 0.91, no concrete source/test blockers.

## Review fixes applied after the first gate

- Stage restore is performed only by the enabled/generation-gated artifact cache path; the compile callback no longer bypasses that control.
- Queued stage publications carry the generation and revalidate enablement while holding the in-process disk transaction lock. Stage writes and stage clear share the cross-process automatic-cache mutex.
- CLEAR removes both complete-PSO and stage-artifact files, including temporary stage files.
- The stage budget scan and atomic rename now run under the same cross-process lock, so concurrent writers cannot both consume one remaining budget.
- The asynchronous test worker hook is copied under a mutex, and the blocked-clear test checks the disk map after the operation rather than asserting a constant.
- A failed native descriptor range copy now falls through to the existing per-slot path after the destination slots are reset, so destination bookkeeping is not committed from a zero native resource ID.
- The fallible range bridge no longer uses the asserting Metal4Invoke helper; a bridge failure returns zero so the heap can take that fallback.
- Stage metadata records now carry and validate exact output-layout sizes; stage relocation offsets and targets are checked before restore. Temporary stage handles close before cleanup or atomic rename.
- The final gate found orphaned `dxmt-stage-*.tmp` files were outside the stage budget scan. The scan now covers every `dxmt-stage-*` regular file and skips directories, so crash leftovers consume the same bounded budget.
- The next gate found the CPU test hook still read the global publication promise from the worker. `phaseHook` now captures a `shared_ptr` at installation, matching `armPublication`, so reset cannot destroy the promise while the worker signals it.
- The final gate found partial Win32 stage-directory enumeration could over-admit a write. Empty-directory handling now accepts only `ERROR_FILE_NOT_FOUND`; a completed scan must end with `ERROR_NO_MORE_FILES`, and all other enumeration errors reject the publication.
- The next gate found optional worker construction could propagate `std::system_error`. Both worker factories now catch startup failure, mark the queue stopped, and return without enabling persistence; producer paths remain successful.
- The next gate questioned restored stage ownership. Restored artifacts use process-addressable serialized library/constant/relocation buffers; `DeviceShaderArtifactCache::Library` now makes the owner-independent direct-copy branch explicit when no compiler handle exists, while existing `SM50ReadCompiledBitcode` direct-pointer handling covers constants and relocations.
- The final gate found the new sample-NaN shader flag needed cache invalidation. Complete-PSO version is now 7, and compute/graphics pipeline-key revisions are now 4/8, invalidating pre-flag blobs and stage keys.
- The next gate found two optional-path lifetime holes. Stage writer jobs now catch publication exceptions, and every `phaseHook` use clears the worker callback before its stack `Gate` expires.

## Resolved items
- RT/WG compiler-result reuse is implemented with device-local immutable copied artifacts. RT copies metallib, constants, relocations and function metadata; Work Graph validation remains uncached.
- Commit-time Present reservation is implemented with a native Metal4 ticket retained through recording and resolved from `MTL4CommitFeedback`, including stale restart-epoch and failed-submission handling.

## Known residuals
- The latest source state has not received a final ARM64 cross build because `aarch64-w64-mingw32-g++` is unavailable in the current host environment.
- GPU fixture execution remains blocked by the available ARM64X runtime versus the fresh plain-ARM64 build.
- Queue priority, programmable sample Tier 2, Agility factory internals, and full compiler dylib behavior remain static-analysis unknowns.

This file was originally generated as read-only review input. The status and path metadata above were updated after implementation; source files remain unchanged by this documentation update.
