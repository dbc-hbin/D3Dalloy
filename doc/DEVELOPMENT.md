# Development, toolchains and debugging

Use the [root README](../README.md#빌드) to build current root sources, [RUNTIME](../graphics-translator/RUNTIME.ko.md) to stage them, and [SMOKE](../graphics-translator/SMOKE-TEST.ko.md) to run fixtures. Current support and pending work belong in [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md) and [PRIORITIES](../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md).

## Prerequisites

- macOS, Xcode/Command Line Tools and Metal tools exposing the APIs used by current sources. Recorded M5 Pro/macOS 27 results are not cross-device or minimum-OS conformance.
- Python 3, Meson 1.10.0 and Ninja 1.13-series are the documented local stack; Meson's declared minimum is 1.3.0.
- llvm-mingw clang 22.1.8 emits Windows PE. Native LLVM must be **15.0.7**, with static libraries and dependency metadata matching the selected native Mach-O architecture.
- Wine SDK/runtime and FEX come from their [sibling repositories](ARM64-FEX-IMPLEMENTATION.md). Select either `wine_install_path` or `wine_build_path`, not competing SDK authorities.
- Vendored DirectX/NVAPI/NGX headers are already present. GPTK/D3DMetal, Apple Metal Shader Converter and SPIR-V are not compiler fallbacks.

## Setup LLVM

The x86_64 native Wine path needs x86_64 LLVM even on Apple Silicon. Native ARM64 needs a separate arm64 prefix. Example for fresh x86_64 source/build/install directories:

```sh
git clone --depth 1 --branch llvmorg-15.0.7 \
  https://github.com/llvm/llvm-project.git toolchains/llvm-project-15
cmake -S toolchains/llvm-project-15/llvm -B toolchains/llvm-build-15 -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$PWD/toolchains/llvm-15" \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF \
  -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=ON -DLLVM_ENABLE_TERMINFO=ON \
  -DLLVM_ENABLE_PROJECTS="" -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=OFF
cmake --build toolchains/llvm-build-15
cmake --install toolchains/llvm-build-15
```

For ARM64, use `arm64`, `arm64-apple-darwin` and distinct directories. The [LLVM dependency resolver](../src/airconv/darwin/meson.build) accepts either `llvm-config` or installed CMake metadata, checking version 15.0.7 in both cases. Keep required system libraries with the archives; zstd-enabled LLVM also needs static zstd. A version match does not satisfy the frozen builder's historical content hashes.

`native_llvm_path` accepts absolute paths or source-root-relative paths. Keep local dependencies under ignored `toolchains/` and outputs under `build/`.

## Build and install workflow

Rebuild/install the whole graphics module set and stage a fresh private runtime. PE callers, Unix thunks and native consumers share ABI contracts; do not mix module generations or install into live games/prefixes.

Owned `d3d12core.dll` is a normal native PE beside Main, with `d3d12core=n`; do not apply `winebuild --builtin` to Core. Its SDK618 export is a selected package identity, not complete Microsoft SDK618 conformance. Preserve Main/Core generation, file/module identity and export aliases. Detailed selection, lifetime and error contracts are in [Core618 acceptance](../graphics-translator/SMOKE-TEST.ko.md#owned-core618-versionpath수명-acceptance).

Before x64 cross setup, pin native compilers so llvm-mingw-first PATH cannot select them:

```sh
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++
export OBJCXX_FOR_BUILD=/usr/bin/clang++
```

Native ARM64 uses `--native-file build-native-arm64.txt`. Windows ABI, native architecture and guest instruction set remain separate; use the [ARM64 guide](ARM64-FEX-IMPLEMENTATION.md) rather than relabeling x64 binaries.

The [Meson options](../meson.options) enable D3D12 and optional NVAPI/NGX/FSR explicitly. `enable_fsr` builds both SR and FG DLLs; stage selected frontends explicitly. PE-only and standalone native-bridge builds are not complete runtimes. `enable_tests` builds manual DX11 samples, not the D3D12 regression suite; there is no general lint target.

For clangd, select the actual Meson compilation database, such as `--compile-commands-dir=build/local/build`, and the matching cross-compiler query-driver. `.clangd` otherwise points at `build/`.

## Verification and debugging

Prefer the fixture runner's `--validation` for Metal API and shader validation. Use `--api-validation` only for the justified cases documented in SMOKE. The runner sanitizes inherited DXMT/DYLD injection state and assigns private logs/caches; environment overrides outside that policy are not reliable.

For a separate private manual session:

| Control | Scope |
| --- | --- |
| `MTL_DEBUG_LAYER=1` | Metal API validation |
| `MTL_SHADER_VALIDATION=1` plus `MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1`, `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1` | Shader faults |
| `DXMT_LOG_LEVEL`, `DXMT_LOG_PATH` | Logging threshold and separate module logs |
| `OWNED_GRAPHICS_TRACE=1` | D3D12 lifecycle trace; see [configuration](CUSTOMIZATION.md#diagnostics-are-separate-from-configuration-keys) |
| `MTL_CAPTURE_ENABLED=1`, `DXMT_CAPTURE_EXECUTABLE` | Executable-scoped capture where the backend supports it; shared classic `DXMT_CAPTURE_FRAME` is not proof of D3D12 Metal 4 capture |
| `DXMT_SHADER_CACHE=0`, `DXMT_SHADER_CACHE_PATH` | Shared internal shader cache, not application `ID3D12ShaderCacheSession` |

Capture and shader-instrumented validation are separate runs. Keep stderr and per-module logs; clean stderr cannot override a backend error. FG's universal observer is test-only, audited input, not product telemetry; preserve unmeasured zero timestamps and do not equate GPU readback with physical display cadence. Use SMOKE's current fixture/observer commands rather than duplicating them here.

CPU/build success is not GPU or real-game acceptance. Preserve hashes, inputs, logs and actual readback/screenshots. Stop only the run's own wineserver, then remove its disposable prefix and obsolete payloads; retain the latest candidate, one needed comparison and evidence.
