# D3Dalloy

D3Dalloy translates Direct3D 12 to Metal for Windows games running through Wine on macOS.
It is built on [DXMT](https://github.com/3Shain/dxmt), with its own DXIL/DXBC-to-AIR shader compiler.
It does not depend on GPTK/D3DMetal, Apple Metal Shader Converter or a SPIR-V fallback.

The project is experimental. Game compatibility, image quality and performance vary by GPU and macOS version.
See the [implementation status](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md) for supported contracts
and known limits, and the [roadmap](graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md) for remaining work.

## Features

| Area | Implementation |
|---|---|
| Direct3D 12 | Graphics and compute, ray tracing, mesh shaders, variable rate shading, reserved resources and pipeline libraries. |
| Shaders | DXIL/DXBC compilation to AIR/metallib, with Rust parsers for binary formats and cache records. |
| Game integrations | NVAPI, NGX and FSR interfaces for upscaling, frame generation, ray reconstruction and latency features. |
| Direct3D 10/11 | Inherited DXMT frontends; current development focuses on D3D12 games. |
| Native ARM64 | ARM64/ARM64EC paired DLLs packaged as ARM64X, with native Wine and FEX for x86/x64 guests. |

Upscaling and frame-generation integrations use MetalFX and D3Dalloy's own processing paths, not AMD or NVIDIA
models. Individual feature tests do not establish full API conformance or compatibility with every game.

## Requirements

- macOS 27 and a GPU/compiler supporting Metal 4 for the current D3D12 backend.
- A matching Wine runtime: native ARM64 Wine with FEX, or x86_64 Wine with Rosetta on Apple silicon.
- Matching graphics DLLs and native libraries from the same build, including `d3d12core.dll`.

Wine SDK/runtime and emulator builds are maintained separately in [Wine-darwin](../Wine-darwin/README.md)
and [FEX-darwin](../FEX-darwin/README.md). D3Dalloy consumes their built artifacts.

## Building

Builds need Xcode and Metal tools, Python 3, Meson, Ninja, llvm-mingw, native LLVM **15.0.7** static libraries,
Rust **1.85.1** with the selected target libraries, and a Wine SDK. LLVM and Wine libraries must match the
native architecture. Dependencies are vendored; no Git submodule initialization is needed.

For the x86_64 native baseline, prepare `toolchains/llvm-15` and `toolchains/wine-sdk`, put llvm-mingw,
Rust, Meson and Ninja on `PATH`, and install Rust's `x86_64-apple-darwin` and `x86_64-pc-windows-gnu` targets:

```sh
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++ OBJCXX_FOR_BUILD=/usr/bin/clang++
meson setup build/local/build . --cross-file build-win64.txt \
  --prefix "$PWD/build/local/install" --buildtype debugoptimized \
  -Dwine_builtin_dll=true -Denable_d3d12=true \
  -Denable_nvapi=true -Denable_nvngx=true \
  -Dnative_llvm_path=toolchains/llvm-15 \
  -Dwine_install_path=toolchains/wine-sdk
ninja -C build/local/build
meson install -C build/local/build --no-rebuild
```

Add `-Denable_fsr=true` to build the optional FSR modules. Use a fresh directory for the initial setup;
subsequent builds only need Ninja and install. See [Development](doc/DEVELOPMENT.md) for toolchain details.

For native ARM64, follow the [ARM64/Wine/FEX build and packaging guide](doc/ARM64-FEX-IMPLEMENTATION.md#native-wine-build-and-package-recipe).
It uses `scripts/build-owned-arm64x.py` for paired DLLs and `scripts/package-owned-arm64.py` for the runtime.

## Running and testing

Follow [Runtime staging](graphics-translator/RUNTIME.ko.md) to assemble a fresh private Wine runtime,
then [Smoke tests](graphics-translator/SMOKE-TEST.ko.md) to build and run fixtures with Metal validation.
Use dedicated test prefixes rather than replacing DLLs in an installed game or live prefix.

Build, CPU and GPU checks are separate; `meson test` is not the project's regression suite.
Keep logs, `result.json` and captures when reporting a failure.

## Settings

[`d3dalloy.conf`](d3dalloy.conf) lists the options. Use `D3DALLOY_CONFIG_FILE` to select a configuration file
or `D3DALLOY_CONFIG` for inline settings. `D3DALLOY_LOG_PATH` selects the log directory; each module writes
`<executable>_d3dalloy-<module>.log`. See [Customization](doc/CUSTOMIZATION.md) for precedence and diagnostics.

## Documentation

The [documentation index](doc/README.md) links the architecture, API contracts, validation history and game
reports. Many detailed documents are currently in Korean. Active development uses the root `src/` tree;
`graphics-translator/patches/` and `doc/provenance/` preserve the frozen import, not the current build workflow.

## Credits and license

D3Dalloy builds on DXMT's Direct3D frontends, shader converter and Metal bridge. The imported baseline is
[`7c8dee1`](https://github.com/3Shain/dxmt/commit/7c8dee1c2d73415301ceb7d1fa810861cef4cd67);
exact source and patch records are in [source provenance](doc/provenance/source-import.json).

See the root [MIT license](LICENSE). Imported and vendored code retains its original copyright and license
notices, including [NVAPI](external/nvapi/License.txt), [NGX](external/nvngx/LICENSE.txt),
[DirectX headers](include/native/directx/COPYING.MinGW-w64.txt) and [DXVK utilities](src/util/dxvk.LICENSE).
