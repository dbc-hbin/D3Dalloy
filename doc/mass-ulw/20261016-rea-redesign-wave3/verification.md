# Wave 3 verification

## Scope

Wave 3 applies the beta3 `D3DMCommandQueueWorker::DoExecute` `FoldedSignal` observation to DXMT. Only consecutive `SignalTask` items already present in the queue are folded. The first non-signal task is held and processed after the batch, so Execute, Wait, Present, marker, tile-mapping, and close boundaries are not crossed.

Each logical signal remains in `InflightCommandBuffer::fence_completions`. The completion thread retires every fence ticket only after the shared native empty submission completes.

## Checks

- Fresh current-source build: `ninja -C build/local/wave2-build -j8` passed with the repository's existing 14 missing-override warnings.
- Install and fresh runtime staging passed: `build/local/wave3-runtime-002`.
- Query GPU scenario passed: `build/local/wave3-query-output-002/result.json` has `exit_code=0`, `validation_errors=[]`, native-only owned modules, and `pass=true`.
- Focused diagnostics passed: `execution_graph` 1/1 and `multiqueue` 2/2.
- `git diff --check` passed on the Wave 3 source files.
- remove-ai-slops review found two real issues and both were fixed: unattached render slots were incorrectly treated as DISCARD boundaries, and signal vector allocation could escape the worker's error handling.

## Limits

The render-passes current-source fixture remains a separate Wave 2 blocker at `build/local/wave2-render-output/result.json`; it emitted no graph markers and failed graph phase/frontier/readback validation. No game FPS or D3DMetal superiority claim is made.
