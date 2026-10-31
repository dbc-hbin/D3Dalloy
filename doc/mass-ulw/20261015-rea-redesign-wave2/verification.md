# Wave 2 verification

## Source and build

- Fresh Meson configuration: `build/local/wave2-build` using `/opt/llvm-mingw-20260616-ucrt-macos-universal/bin`, `toolchains/llvm-15`, and `toolchains/wine-sdk`.
- `ninja -C build/local/wave2-build -j8`: passed; only existing compiler warnings were emitted.
- `meson install -C build/local/wave2-build --no-rebuild`: passed.
- The older `build/local/build` was not reused because its stale Homebrew mingw/lld configuration failed at link with undefined `pthread_once` and `pthread_mutex_*`.

## Implemented design

- `SubmissionNode::boundary_before` now forces a native fence wait after opaque/unresolved predecessors and explicit global/alias boundaries.
- Visibility scratch initialization is treated as a boundary before the render that consumes it.
- Query resolve destinations retain the canonical `BufferAllocation` and exact range, so resolve writes can connect to later ordinary D3D12 buffer accesses instead of relying only on native Metal handle identity.
- BeginRenderPass DISCARD state is retained until the lazy render encoder is created, so ending DISCARD is not lost when setup leaves a Clear encoder active.

## GPU evidence

- Query fixture compile: `x86_64-w64-mingw32-clang++ ... owned-d3d12-queries.cpp -o build/local/wave2-fixtures/queries-x64.exe ...` passed.
- Query scenario: `python3 scripts/test-owned-graphics.py --runtime build/local/wave2-runtime-002 --guest-arch x64 --suite queries --fixture build/local/wave2-fixtures/queries-x64.exe --out build/local/wave2-query-output-002 --validation --timeout 240` passed.
- Artifact: build/local/wave2-query-output-002/result.json has exit_code=0, validation_errors=[], native-only owned module loads, and pass=true.
- Review follow-up: EndRenderPass now clears retained DISCARD state, and SO query resolves record sizeof(D3D12_QUERY_DATA_SO_STATISTICS) with a 64-bit destination range calculation.
- Final artifact: build/local/wave2-query-output-003/result.json has exit_code=0, validation_errors=[], native-only owned module loads, and pass=true after the review fixes.
- Render fixture was compiled and run against a fresh current-source runtime. Artifact `build/local/wave2-render-output/result.json` loaded the owned modules but failed before graph markers, reporting missing graph phase/frontier/readback validation. This is recorded as a blocker; it is not treated as an FPS or correctness pass.

## Review limits

- No matched D3DMetal-vs-DXMT game A/B was run, so no FPS superiority claim is made.
- The remaining render fixture failure needs a separate diagnosis before more fence elision is enabled on render paths.
