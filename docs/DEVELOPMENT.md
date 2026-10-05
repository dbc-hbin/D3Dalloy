# Development, toolchains and debugging

The [root README](../README.md#빌드) is the canonical recipe for building **the current root source**. This document explains the inputs and debugging workflow; it is not a second build baseline. Run examples from the repository root. The imported upstream CI is not present in this repository, and upstream minimum OS/toolchain statements are not evidence that the current Metal 4 implementation runs on those versions.

For current capabilities, observed acceptance and remaining boundaries, use [CORE-IMPLEMENTATION-STATUS.ko.md](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md); pending work and the user-selected new P3 execution order belong in [PRIORITIES](../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md). Completed historical P0–P3 phases are not that backlog. The [ARM64 Wine/FEX track](ARM64-FEX-IMPLEMENTATION.md) separates native Mach-O architecture, Windows module ABI and guest instruction set. A successful ARM64 compiler/bridge or PE build does not establish native Wine/FEX guest execution.

## Prerequisites

- macOS with Xcode/Command Line Tools and the Metal toolchain (`xcrun -sdk macosx metal` and `metallib`). Use an SDK exposing the Metal 4 APIs used by current sources. The recorded Metal 4 fixture scope is Apple M5 Pro/macOS 27; this is not a blanket minimum-OS or cross-device compatibility claim.
- Python 3, Meson and Ninja. `meson.build` declares Meson >=1.3.0; the README's known build recipe uses Meson 1.10.0/Ninja 1.13-series, not an assertion that every older Meson release was exercised.
- **LLVM 15.0.7 exactly**, with headers, static component libraries and their dependency metadata, built for the selected **native Mach-O architecture**. It is distinct from the newer llvm-mingw compiler that emits Windows PE.
- For Wine cross builds: llvm-mingw and a matching Wine SDK with headers, `winebuild`, import archives and native `ntdll.so`/`winemac.so`. The root recipe uses an installed SDK (`wine_install_path`). Meson also accepts `wine_build_path`; do not set both as competing input authorities.
- CMake and Ninja when building LLVM from source. CMake 3.27+ is the documented setup choice below.

Vendored DirectX/NVAPI/NGX inputs are already included as source directories. Do not replace them by an upstream submodule checkout to follow an old development guide. Apple Metal Shader Converter, GPTK/D3DMetal and SPIR-V compiler fallbacks are not build inputs for the owned compiler.

## Setup LLVM

The intermediate x64 Wine recipe needs **x86_64 LLVM**, even on an Apple Silicon build machine. The ARM64-native track needs **arm64 LLVM**, in a separate prefix; changing Windows ABI does not change this requirement by itself.

Example for the README's `toolchains/llvm-15` prefix (all three directories must be fresh):

```sh
mkdir -p toolchains
git clone --depth 1 --branch llvmorg-15.0.7 \
  https://github.com/llvm/llvm-project.git toolchains/llvm-project-15
cmake -S toolchains/llvm-project-15/llvm -B toolchains/llvm-build-15 -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$PWD/toolchains/llvm-15" \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF \
  -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_ENABLE_EH=OFF -DLLVM_ENABLE_RTTI=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=ON -DLLVM_ENABLE_TERMINFO=ON \
  -DLLVM_ENABLE_PROJECTS="" -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=OFF
cmake --build toolchains/llvm-build-15
cmake --install toolchains/llvm-build-15
```

For native ARM64, use `arm64` for `CMAKE_OSX_ARCHITECTURES` and `LLVM_HOST_TRIPLE=arm64-apple-darwin`, and distinct source/build/install paths such as `toolchains/llvm-15-arm64`. The full implementation-track commands and Wine SDK construction belong in [ARM64-FEX-IMPLEMENTATION.md](ARM64-FEX-IMPLEMENTATION.md), not in the x64 quickstart.

`src/airconv/darwin/meson.build` resolves the static dependency graph in either of two ways:

1. If `<prefix>/bin/llvm-config` is available, its version must be `15.0.7`. Meson reads `--link-static --libs bitwriter passes` and `--link-static --system-libs`.
2. Otherwise, the source-built SDK's installed `<prefix>/lib/cmake/llvm` package supplies the same component graph via CMake. Its reported version must also be `15.0.7`. `LLVM_BUILD_TOOLS=OFF` is therefore supported; a missing `llvm-config` alone is not a broken SDK.

Keep the install metadata and required system libraries with the static archives. Copying a few `libLLVM*.a` files is not a complete SDK. A zstd-enabled prebuilt LLVM additionally needs the matching static zstd dependency. Neither this recipe nor a version match satisfies the frozen import builder's historical **content hashes**; that separate procedure is in [BUILD.ko.md](../graphics-translator/BUILD.ko.md).

Relative `native_llvm_path` values resolve from the repository source root. Current Meson also accepts absolute paths. Relative paths under `toolchains/` remain the local convention, not a workaround for a current absolute-path defect.

## Build and install workflow

Follow [README: build](../README.md#빌드) for the current D3D12/NVAPI/NGX/FSR cross build. Begin with a fresh Meson build directory and install prefix. After source edits, recompile and reinstall the **whole graphics module set**, then stage a new private runtime. PE callers, Unix thunks and native compiler consumers share ABI headers; replacing only `d3d12.dll` or only `winemetal.so` with a different generation is unsafe.

The D3D12 build also installs owned `d3d12core.dll` beside `d3d12.dll` for each PE ABI. Core is an ordinary **native PE** DLL, not a Wine builtin: it has no Unix-library boundary, and builtin resolution can redirect separate SDK paths by basename. Do not run `winebuild --builtin` on Core. The private runtime requires `d3d12core=n` (no builtin fallback); Main and its graphics bridge remain owned builtins. Core's concrete `D3D12SDKVersion` DATA export is **618**, the selected Godot compatibility-package identity, not a claim of complete Microsoft SDK 618 feature conformance. Core owns device-factory configuration and operations; versioned POD callbacks reuse Main's canonical GPU backend and process-global state. Both DLLs export the same concrete 32-character `OwnedD3D12CoreGeneration` DATA string, generated together by Meson; reinstall both after a reconfiguration. Do not replace either with a foreign Agility package.

`ID3D12SDKConfiguration1::CreateDeviceFactory` permits retail game selection without Developer Mode. A nonzero version must exactly match the actual owned Core export, and its directory resolves relative to the executable, not the working directory. Before loading, the runtime read-locks the requested and installed Core files against writing/deletion, verifies installed PE ABI/version/generation, and compares complete images; it then verifies the precise live module path and bridge identity. Factories and settings retain their immutable selected Core binding, devices copy that binding, and device children retain the device. Main-hosted COM release thunks never unload their current code: `FreeUnusedSDKs` alone evicts unused module leases, while selected global Core remains retained. `InitializeFromGlobalState` copies mutable settings without rebinding an independent factory; `ApplyToGlobalState` explicitly publishes that factory's Core and settings, rejects a different Core while global devices/creation are live, and never removes independent devices. Base `SetSDKVersion` retains the existing OS Developer Mode predicate (`AllowDevelopmentWithoutDevLicense`); late calls fail and remove only global devices without changing the selection.

Global EXE exports and base `SetSDKVersion` follow the [redist inbox precedence rule](https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html): requests at or below the validated installed Core version select that installed Core, even when the app-local package is absent. Only a newer request selects the requested package strictly. Read the actual bridge version rather than maintaining a separate version range. SDK1 explicit SxS selection and factory `ApplyToGlobalState` retain their own contracts; they do not use this substitution policy.

Core's public `D3D12GetInterface` and `OwnedGraphicsBackendId` export names alias uniquely named Core implementations. Preserve those aliases: on i686, undecorated exports can otherwise bind imported Main thunks instead of the Core stdcall bodies, even though their thunk addresses lie inside Core.

On Wine, Main's reported module path can name a prefix facade rather than its physical installed image. Installed-Core discovery therefore uses pinned `WINEDLLPATH` directories only as hints: a non-executable data mapping must match Main's actual loaded image via `NtAreMappedFilesTheSame`, and the matching Main file remains read-locked. The Core baseline is adjacent to that verified physical Main file, not a prefix copy or an image admitted by matching bytes/generation alone. WoW64 also checks the `i386-windows` sibling of pinned x86_64 Windows/Unix directories, with the same backing-file identity requirement. This does not copy DLLs into live prefixes.

Global SDK transition failures follow the [Independent Devices contract](https://microsoft.github.io/DirectX-Specs/d3d/IndependentDevices.html): `InitializeFromGlobalState` rejects an older global Core with `D3D12_ERROR_INVALID_REDIST` (`0x887e0003`), and `ApplyToGlobalState` uses the same error for a downgrade or a different Core while global devices/creation remain live. Ordinary argument, module-selection and base `SetSDKVersion` errors retain their own HRESULTs.

The native MetalFX backend is Objective-C++. Before cross-build setup, pin all native compilers, including the Objective-C++ compiler, rather than letting the llvm-mingw-first `PATH` choose one:

```sh
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++
export OBJCXX_FOR_BUILD=/usr/bin/clang++
```

For the native arm64 bridge, use `--native-file build-native-arm64.txt`; that file pins `c`, `cpp` and `objcpp` to Apple compilers. Native arm64 bridge/control checks do not establish ARM64 Wine guest execution or change the public FSR frontend's current x64 runtime requirement.

Relevant current options in [meson.options](../meson.options):

| Option | Meaning |
|---|---|
| `enable_d3d12` | Defaults to false; the owned D3D12 recipe explicitly enables it. |
| `wine_builtin_dll` | Defaults to true; installable Wine builtin images require the Wine marker. |
| `native_arch` | `x86_64` (default) or `arm64`; independent of PE ABI. |
| `build_native_components=false` | PE-only cross build; not a usable runtime by itself. |
| `native_bridge_only=true` | Native macOS compiler/standalone Metal bridge build, not a Windows cross build. |
| `enable_nvapi`, `enable_nvngx` | Build optional vendor frontends; stage them explicitly when needed. |
| `enable_fsr` | Build owned public SR and frame-generation DLLs; `stage --with-fsr` selects both, currently for x86_64-native/x64-Windows runtimes. |
| `enable_tests` | Meson tests under `tests/`; not automatic execution of the separate PE/GPU fixture collection. |

The x64 and x86 cross files are `build-win64.txt` and `build-win32.txt`. New-style WoW64 uses i386 PE modules with the single x86_64 Unix plugin; it is not a separate 32-bit native Metal backend. ARM64/ARM64EC/ARM64X layouts and prerequisites must follow the architecture track rather than copying this recipe and relabeling binaries.

For `clangd`, use the compilation database from the relevant Meson build. A query-driver allowlist such as `--query-driver=**/x86_64-w64-mingw32-**` can help resolve the cross sysroot; select the actual target compiler for other ABIs.

## Verification and evidence

[SMOKE-TEST.ko.md](../graphics-translator/SMOKE-TEST.ko.md) owns CPU/tooling checks, PE compile details, suite selection and isolated GPU runner operation. [RUNTIME.ko.md](../graphics-translator/RUNTIME.ko.md) owns staging, architecture/schema validation and loader policy. Use fresh runtime copies and runner-created prefixes; never copy these DLLs into a live game installation or prefix.

The full SR suite is `--suite fsr`; public FG presentation is `--suite fsr-fg --mode present --fg-observer <universal dylib>`. Use the smoke guide's exact fixture/observer commands. New `--with-fsr` stages both `amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll`, including builtin/export/ordinal gates. Verification of a legacy manifest with no `fsr_framegeneration` field preserves its prior loader policy and runtime bytes; it does not upgrade that runtime or enable the FG suite. Restage into a new directory to select FG.

CPU/tooling PASS is not GPU execution proof. Named historical-phase/Metal 4 acceptance is not full D3D12/DXGI/shader conformance. Historical logs may no longer exist: use the current status document's evidence-availability qualifications rather than treating old `build/owned-*` paths as current inputs.

## Debugging

Prefer runner `--validation` for Metal API **and** shader validation, or the explicitly justified `--api-validation` concurrency case. The runner clears inherited `DXMT_*`, `DYLD_*` and foreign-backend injection state and sets its own log/cache/trace paths. Exporting extra `DXMT_*` variables before a normal runner invocation does not override that policy.

FG's observer is an explicit **test-only** input, frozen and hashed by `--fg-observer`, not product telemetry. Build it universal (`x86_64` + `arm64`): native arm64 wineserver inherits the injection environment even when the fixture runs as x64. The runner audits each slice's dependencies (macOS system libraries/frameworks only), injects only this observer, and records actual drawable pixels, presentation order and native timestamps under `fg-capture/`. Preserve zero [`MTLDrawable.presentedTime`](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime) rows as unmeasured/skipped, not positive timestamp proof or proof that source pixels were absent. The shared `FgActualPresentationObservation` snapshots `GetLastPresentCount` and `GetFrameStatistics` once: [the former counts Present calls](https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getlastpresentcount), whereas [`DXGI_FRAME_STATISTICS.PresentCount` counts displayed frames](https://learn.microsoft.com/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics), not submitted IDs. Never wait for equality or require per-request counter advancement. Preserve `DXGI_ERROR_FRAME_STATISTICS_DISJOINT` with zero unmeasured statistics; validate monotonic measured counts, refresh counts and QPC bounds independently. Physical acceptance still requires every generated index for N=1..4 at its normalized phase, repeated positively timestamped generated-before-real pairs, critical real/reset/error and built-in UI outputs, and unchanged raw-pixel/ordering/count checks. Legal skips remain explicit and cannot stand in for unexercised generated phases. Callback readbacks or `Present` QPC duration alone are not actual-presentation proof. Keep `fg-capture-analysis.json` and `result.json` together.

Public NGX/FSR FG fixtures position only their own HWND at the work area's right edge with no activation or z-order change, reserve vertically disjoint ABI positions, verify the actual rectangle, and reject insufficient work areas. This controlled geometry adds no warmup Presents or timestamp retries and is not visibility or physical-presentation proof: native occlusion metadata and the existing raw-pixel/critical-positive-timestamp checks remain necessary. Other applications and the shared HWND constructor are unchanged. Queue-gate proof checks the selected GPU fence remains pending, the idle queue progresses, and callback readback retains its GPU sentinel until release; public Present calls may be accepted asynchronously while GPU work is blocked. Before/during submitted-call counts need not be equal, and earlier accepted native presentations may complete while the gate is held. These counters cannot replace the full native pixel/phase/timestamp proof.

`OWNED_FSR_NATIVE_MAX_SCALE` is an independent native-control measurement used by the SR fixture's cap oracle, recorded in `invocation.json`; query the current Metal device and preserve that control log before setting it. A measured value of 3 on M5 Pro is not a portable constant. Neither this oracle value nor a native-control PASS replaces the public SR suite's validation/readback gates. Runner cleanup kills only the wineserver for its newly created prefix, including on failure/timeout; never use a global wineserver kill or reuse a live prefix. Require the staged owned `winemetal.so` load and absence of D3DMetal/`libd3dshared` before accepting either public suite.

NGX's separately numbered real-only bootstrap accepts equal positive native timestamp samples: measured bootstrap clocks must be nondecreasing and bounded, not strictly increasing. Equal samples do not establish distinct-frame cadence. This does not relax strict formal generated-before-real ordering, critical positive timestamps, or full scene-pixel validation. Rechecking saved captures writes a new analysis file and preserves original run results and raw evidence.

For a separate manual debug session, use only a newly staged runtime, a fresh dedicated prefix and a private fixture copy. These variables are source-supported controls, not claims that every capture path was exercised on Metal 4:

| Variable | Purpose |
|---|---|
| `MTL_DEBUG_LAYER=1` | Metal API validation. |
| `MTL_SHADER_VALIDATION=1` | Shader instrumentation; also enable `MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1` and `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1` to collect faults. |
| `MTL_CAPTURE_ENABLED=1` + `DXMT_CAPTURE_EXECUTABLE=<basename without extension>` | Enable capture state for that executable; F10 schedules a frame where the backend wires this control. |
| `DXMT_CAPTURE_FRAME=<n>` | Automatic capture scheduling in the shared classic command-queue path; do not assume this proves D3D12 Metal 4 capture support. |
| `DXMT_LOG_LEVEL=none\|error\|warn\|info\|debug\|trace` | Logging threshold. |
| `DXMT_LOG_PATH=<private directory>` | Separate executable/module logs; `none` disables files, not logging. |
| `DXMT_SHADER_CACHE=0` | Disable the shared internal shader cache. This is not an `ID3D12ShaderCacheSession` implementation. |
| `DXMT_SHADER_CACHE_PATH=<absolute private directory>` | Shared shader-cache storage override; runner uses its own per-run path. |

Do not combine a capture investigation with shader instrumentation and call it the validation run: earlier capture work required disabling shader validation. Collect API/shader-validation evidence separately. Under Wine, logs go through Wine's console output; when `DXMT_LOG_PATH` is set, retain both `stderr.log` and per-module logs (`<exe>_d3d12.log`, `<exe>_dxgi.log`, etc.). A clean stderr with a backend command error is not PASS.
