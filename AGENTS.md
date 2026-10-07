# Repository Guidelines

## Project Overview

DXMT-based D3D12-to-Metal translator for Wine on macOS, with an owned DXIL/DXBC-to-AIR/metallib compiler. D3D10/11, DXGI, NVAPI, and NGX support share the runtime. No GPTK/D3DMetal, Apple Metal Shader Converter, or SPIR-V fallback.

The current product scope is running D3D12 games on macOS through Metal, not general Windows graphics/API completeness. Prioritize game rendering correctness, compatibility, performance, presentation, and game-used upscaling/frame-generation/latency paths. A backlog entry or missing API alone does not authorize implementation: establish its relevance to D3D12 games before expanding scope.

Game scope includes optional graphics, display, and performance features, not only the minimum needed to launch. Keep game-used features such as HDR10+ in scope even when uncommon or unsupported by the current native backend. Distinguish game relevance from implementation feasibility: record missing native/platform support as an unresolved limitation, never as evidence that the game feature is unnecessary.
Game-facing runtime/loader APIs such as Agility SDK configuration and device factories are not developer-only tooling. Use actual game/engine call paths to classify mixed groups such as SDK/Tools or CoreWindow/Composition; do not exclude the whole group by name.

Do not add a WARP/CPU renderer, standalone video decode/process/encode backend, desktop-duplication/capture subsystem, or broad Wine/DirectComposition/tooling implementation merely to close API or P4 checklists. These are outside the current scope unless a concrete D3D12 game requirement is established and the user explicitly approves the expansion. Keep unsupported capabilities honest; do not mark excluded work as implemented.

Do not expand scope into D3D11. Change shared/D3D11 or Wine code only when necessary for a concrete D3D12 game path, keep changes minimal, and obtain explicit approval before extending work into another repository.

Edit root `src/` directly. `README.md` describes the current workflow; `graphics-translator/patches/`, its upstream lock, and `docs/provenance/` preserve the frozen import baseline. Do not regenerate current sources with `scripts/build-owned-graphics.py` or export changes back into historical patches.

## Architecture & Data Flow

- **API/device:** exported DLL entry points validate arguments, obtain a DXMT DXGI adapter, and construct an API-specific COM device over the shared backend.
- **D3D12 execution:** command lists record typed encoder passes and command records; `Close` seals recording. Queue-local FIFO tasks (`Execute`, `Signal`, `Wait`, `Present`) retain submission ownership and feed a submission thread. A second thread retires Metal completions in order through `SubmissionRing<32>`. D3D11 uses the separate shared `src/dxmt` encoder/dependency-tracking path.
- **Shaders:** `src/airconv` parses bytecode, lowers DXIL in place using per-compilation `ModuleInfo`, validates it, and emits AIR/metallib. Mutable LLVM IR must remain local to one compilation/context.
- **Metal boundary:** Windows WineMetal wrappers send POD parameters through numbered `WINE_UNIX_CALL`s to native Metal handlers. Change wrappers, parameter layouts, call indexes, handlers, and call tables together.
- **NGX/NVAPI:** translation facades use private DXMT interfaces rather than independent renderers. Preserve cross-DLL ABI contracts.
- **D3D12 fences:** logical values can rewind. GPU queue signals publish logical completion through validated retirement, not native-token reservation. Waits latch qualifying completion at API registration or afterward; shared fences retain per-observer threshold crossings, while native shared events only wake observers. Consumer workers wait for actual completion, not just producer commit; API callers remain asynchronous. D3D11 retains its separate native fence path.

## Key Directories

| Path | Responsibility |
| --- | --- |
| `src/d3d12/` | Devices, queues/lists, resources, descriptors, PSOs/cache, queries, temporal scaling, DXR |
| `src/airconv/` | Shader conversion, LLVM lowering, AIR/metallib output |
| `src/dxmt/`, `src/dxgi/` | Shared renderer, dependency tracking, adapters, presentation |
| `src/winemetal/` | Windows/native Metal bridge; native handlers under `unix/` |
| `src/d3d10/`, `src/d3d11/`, `src/nvapi/`, `src/nvngx/` | Other API frontends and compatibility facades |
| `src/util/` | COM/reference ownership, Wine-aware threading, logging, configuration |
| `graphics-translator/tests/` | D3D12 fixture executables, standalone CPU probes, frozen DXIL inputs |
| `scripts/` | Build/staging regressions, isolated runtime staging, GPU evidence runners |
| `include/native/`, `external/`, `libs/` | Vendored headers and libraries; not submodules |

## Development Commands

Use [README: build](README.md#빌드) for current root-source build/install commands, [Runtime](graphics-translator/RUNTIME.ko.md) for fresh staging and architecture/module selection, and [Smoke test](graphics-translator/SMOKE-TEST.ko.md) for fixture compilation, CPU/tooling checks and isolated GPU execution. Do not keep a second recipe here.

Current support and evidence belong in [CORE](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md); pending work and current user-selected execution belong in [PRIORITIES](graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md). Dated phases and pauses in [HISTORY](graphics-translator/IMPLEMENTATION-HISTORY.ko.md) and [historical handoffs](docs/history/README.md) are not current instructions. Do not duplicate mutable progress or pause state here. The current source review and implementation ledger is [the 2026-10-07 review](docs/D3D12-REVIEW-AND-DESIGN-2026-10-07.md).

There is no configured lint target or general application `run` command. Respect applicable `.clang-format` files; do not invent `ninja lint` or package-manager commands.
Runtime destinations and evidence output directories must be fresh. Never install DLLs into an existing game or live Wine prefix; use a copied runtime and dedicated prefix.

## Code Conventions & Common Patterns

- C++20; preserve subsystem style. D3D12 formatting uses 2 spaces, no tabs, 120 columns; root formatting disables include sorting. Types/API functions use PascalCase, implementations often end in `Impl`, and locals/members commonly use snake_case. Keep Microsoft ABI spelling intact.
- Use existing ownership types: `Com<T>` for COM, `Rc<T>` for shared DXMT objects, `WMT::Reference<T>` for Metal handles, and standard smart pointers where already established. Deferred tasks must retain owners until GPU retirement; CPU publication is not GPU completion.
- API boundaries initialize output pointers and return contract-appropriate `HRESULT`s. Void recording methods retain the first recording error. Airconv is exception-free: propagate or consume `llvm::Error`/`llvm::Expected<T>`. Do not use aborting `IMPLEMENT_ME` for normal unsupported API requests.
- Preserve queue acquire/release publication, stable record storage, backpressure, and shutdown order: close FIFO, join submitter, retire published GPU work, publish stop sentinel, join completion thread. Use `dxmt::thread`/mutex wrappers where Wine initialization matters; respect AppKit main-thread dispatch.
- Dependencies flow through COM/private interfaces and constructors. Keep state explicit in device/list/allocator objects; configuration singletons are module-local, not process-global across DLLs. Reuse `TRACE`, `DEBUG`, `WARN`, `ERR`, and structured lifecycle tracing.

## Important Files

- `meson.build`, `meson.options`, `src/meson.build`: build composition and feature gates. `build-win64.txt` is the documented cross configuration; win32 and ARM64EC alternatives also exist.
- `src/d3d12/d3d12.cpp`, `src/d3d11/d3d11.cpp`, and adjacent `.def` files: public entry points/exports.
- `src/d3d12/d3d12_interfaces.hpp`: private cross-DLL temporal ABI. `owned_submission_ring.hpp`, `owned_queue_task_fifo.hpp`, and `owned_rtas_lifetime.hpp` in that directory document concurrency/lifetime invariants.
- `src/airconv/airconv_public.h`, `src/airconv/dxil_converter.hpp`: compiler ABI and lowering contract.
- `dxmt.conf`, `docs/CUSTOMIZATION.md`: configuration, including `DXMT_CONFIG_FILE`/`DXMT_CONFIG`. `docs/DEVELOPMENT.md`: debugging background; prefer root README build instructions over older recipes.

## Runtime/Tooling Preferences

Supported local stack: macOS Xcode/Metal tools, Python 3, Meson 1.10.0, Ninja 1.13 family, llvm-mingw clang 22.1.8, x86_64 native LLVM 15.0.7 static libraries, and an installed Wine SDK. Apple Silicon needs Rosetta for this x86_64 Wine path. Meson's declared minimum is 1.3.0, not the documented toolchain version.

Keep local dependencies under ignored `toolchains/` and outputs under `build/`. Current Meson supports both relative and absolute `native_llvm_path`; examples use `-Dnative_llvm_path=toolchains/llvm-15`. LLVM must be 15.0.7 and match the selected native architecture. No Node/Bun runtime or language package manager is required. `.clangd` points at `build/`; explicitly select the configured build directory (for example `build/local/build`) as the compilation-database directory when needed.

### Artifact Cleanup During Development

- Clean intermediate artifacts as work progresses, not only at task completion or after the SSD fills. Check available space before creating another full runtime or test prefix.
- After a verification run finishes, preserve its evidence and remove its disposable Wine prefix once that run's Wine processes have stopped. Retain a prefix only while it is needed for an unresolved reproduction.
- Keep the current build/install, the latest staged runtime, and one known-good comparison runtime. Retire obsolete runtime payloads (`bin/`, `lib/`, `share/`) while preserving their manifests and provenance. Additional copies must have an active verification purpose; fresh staging/output directory requirements still apply.
- Preserve source, `toolchains/`, frozen fixtures, logs, `result.json`, and drawable captures. Limit cleanup to identified disposable artifacts under `build/`; never delete installed games, live prefixes, or artifacts still used by another worker. Coordinate shared cleanup with parallel agents before deleting their outputs.

## Testing & QA

- When API semantics, native mappings, or completion criteria remain ambiguous or appear blocked after checking the public specification and owned code, MUST consult the GPTK 4 D3DMetal binary before declaring the contract unsupported or unclosable. Inspect the relevant call path and, where executable, use an isolated behavioral probe; distinguish observed behavior from inference.
- Use a verified pristine reference, not a Yaagl/owned-hooked copy. The recorded GPTK 4.0b2 path, SHA256, and prior evidence are in `graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md` under "P1 계약 범위와 reference"; verify identity before a new investigation. Record the version/hash, relevant symbols or offsets, inputs, and findings. GPTK behavior is compatibility evidence, not a replacement for the public specification or proof that our implementation passes. Do not copy proprietary code or add GPTK as a production dependency/fallback.
- The build/staging/diagnostics regression scripts listed in the smoke guide use Python stdlib `unittest`; NGX frontend validation separately checks PE exports and MSVC parameter-facade ABI without GPU execution.
- `-Denable_tests=true` builds manual DX11 samples under `tests/dx11/`; it registers no Meson `test()` cases. `meson test` is not the project regression suite.
- D3D12 fixtures use `scripts/test-owned-graphics.py`; select the affected suite and enable Metal validation. The runner verifies staged modules, rejects foreign backends/validation errors, and preserves evidence. Standalone `*.test.cpp` files are manual CPU probes, not automatically discovered tests.
- `scripts/test-owned-metallib.py` and `scripts/test-owned-null-descriptor.py` are native GPU probes requiring built compiler artifacts and Metal tooling.
- No numeric coverage threshold is configured. Verify the changed behavioral contract; fixture success is not general D3D12/DXR conformance or real-game compatibility. `CreatePipelineLibrary` and `ID3D12PipelineLibrary1` are implemented. Their owned serialized PSO blobs are not native Metal pipeline archives; restoring them still creates native Metal libraries/functions/PSOs. Current support and remaining contracts are in `graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md`; detailed dated evidence is in `graphics-translator/IMPLEMENTATION-HISTORY.ko.md`.
