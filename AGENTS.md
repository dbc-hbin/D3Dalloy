# Repository Guidelines

## Project Overview

DXMT-based D3D12-to-Metal translator for Wine on macOS, with an owned DXIL/DXBC-to-AIR/metallib compiler. D3D10/11, DXGI, NVAPI, and NGX support share the runtime. No GPTK/D3DMetal, Apple Metal Shader Converter, or SPIR-V fallback.

Edit root `src/` directly. `README.md` describes the current workflow; `graphics-translator/patches/`, its upstream lock, and `docs/provenance/` preserve the frozen import baseline. Do not regenerate current sources with `scripts/build-owned-graphics.py` or export changes back into historical patches.

## Architecture & Data Flow

- **API/device:** exported DLL entry points validate arguments, obtain a DXMT DXGI adapter, and construct an API-specific COM device over the shared backend.
- **D3D12 execution:** command lists record typed encoder passes and command records; `Close` seals recording. Queue-local FIFO tasks (`Execute`, `Signal`, `Wait`, `Present`) retain submission ownership and feed a submission thread. A second thread retires Metal completions in order through `SubmissionRing<32>`. D3D11 uses the separate shared `src/dxmt` encoder/dependency-tracking path.
- **Shaders:** `src/airconv` parses bytecode, lowers DXIL in place using per-compilation `ModuleInfo`, validates it, and emits AIR/metallib. Mutable LLVM IR must remain local to one compilation/context.
- **Metal boundary:** Windows WineMetal wrappers send POD parameters through numbered `WINE_UNIX_CALL`s to native Metal handlers. Change wrappers, parameter layouts, call indexes, handlers, and call tables together.
- **NGX/NVAPI:** translation facades use private DXMT interfaces rather than independent renderers. Preserve cross-DLL ABI contracts.
- **D3D12 fences:** logical values can rewind; GPU signals use independent monotonic native tokens. Waits latch qualifying completion at API registration or afterward. Consumer workers wait for actual completion, not just producer commit; API callers remain asynchronous. D3D11 retains its separate native fence path.

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

Run from the repository root with the prerequisites below. Configure a fresh build directory once; subsequently rerun Ninja and install.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
export PATH="$LLVM_MINGW:$PWD/toolchains/tools/bin:$PATH"
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++
meson setup build/local/build . --cross-file build-win64.txt \
  --prefix "$PWD/build/local/install" --buildtype debugoptimized \
  -Dwine_builtin_dll=true -Denable_d3d12=true \
  -Denable_nvapi=true -Denable_nvngx=true \
  -Dnative_llvm_path=toolchains/llvm-15 \
  -Dwine_install_path=toolchains/wine-sdk
ninja -C build/local/build -j8
meson install -C build/local/build --no-rebuild

python3 scripts/test-owned-graphics-build.py
python3 scripts/test-owned-graphics-staging.py
python3 scripts/test-owned-graphics-diagnostics.py
# Requires a configured cross build and llvm-mingw:
python3 scripts/test-owned-ngx-frontend.py
```

There is no configured lint target or general application `run` command. Respect applicable `.clang-format` files; do not invent `ninja lint` or package-manager commands.

For GPU execution, first stage a **new** runtime:

```sh
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --wine-source toolchains/wine-runtime \
  --wine-dest build/local/runtime --with-nvapi --with-nvngx
```

Compile the selected fixture using the recipe in `README.md`, then run, for example:

```sh
python3 scripts/test-owned-graphics.py --runtime build/local/runtime \
  --suite pipeline-cache --fixture build/local/pipeline-cache.exe \
  --out build/local/cache-proof --validation
```

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
- `CONTRIBUTING.md`: semantic commit subjects and legal-name sign-off; it prohibits AI/LLM-authored or co-authored contributions. Do not assume generated changes are eligible for an upstream PR.

## Runtime/Tooling Preferences

Supported local stack: macOS Xcode/Metal tools, Python 3, Meson 1.10.0, Ninja 1.13 family, llvm-mingw clang 22.1.8, x86_64 native LLVM 15.0.7 static libraries, and an installed Wine SDK. Apple Silicon needs Rosetta for this x86_64 Wine path. Meson's declared minimum is 1.3.0, not the documented toolchain version.

Keep local dependencies under ignored `toolchains/` and outputs under `build/`. Use relative `-Dnative_llvm_path=toolchains/llvm-15` to avoid the documented absolute-path handling issue. No Node/Bun runtime or language package manager is required. `.clangd` points at `build/`; explicitly select `build/local/build` as the compilation-database directory when needed.

## Testing & QA

- The three graphics build/staging/diagnostics scripts above use Python stdlib `unittest`; NGX frontend validation separately checks PE exports and MSVC parameter-facade ABI without GPU execution.
- `-Denable_tests=true` builds manual DX11 samples under `tests/dx11/`; it registers no Meson `test()` cases. `meson test` is not the project regression suite.
- D3D12 fixtures use `scripts/test-owned-graphics.py`; select the affected suite and enable Metal validation. The runner verifies staged modules, rejects foreign backends/validation errors, and preserves evidence. Standalone `*.test.cpp` files are manual CPU probes, not automatically discovered tests.
- `scripts/test-owned-metallib.py` and `scripts/test-owned-null-descriptor.py` are native GPU probes requiring built compiler artifacts and Metal tooling.
- No numeric coverage threshold is configured. Verify the changed behavioral contract; fixture success is not general D3D12/DXR conformance or real-game compatibility. `CreatePipelineLibrary` remains unsupported; do not confuse implemented cached PSO blobs with native Metal pipeline archives.
