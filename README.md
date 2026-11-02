# D3Dalloy

D3Dalloy translates Direct3D 12 to Metal for Windows games running through Wine on macOS.
Built on [DXMT](https://github.com/3Shain/dxmt), it includes its own DXIL/DXBC-to-AIR/metallib compiler.
It does not use GPTK/D3DMetal, Apple Metal Shader Converter, or a SPIR-V fallback.

**Experimental:** game compatibility, image quality, and performance vary by GPU and macOS version.
Implemented features and passing fixtures do not imply full Direct3D conformance or support for every game.

## Current scope

- D3D12 graphics and compute, ray tracing, mesh shaders, variable rate shading, and reserved resources.
- Pipeline libraries, shader caches, and an optional Metal4 native pipeline archive cache.
- NVAPI, NGX, FSR, and AMD Anti-Lag interfaces for game-used rendering and latency features.
- Upscaling, frame generation, and ray reconstruction through MetalFX and owned processing paths,
  not AMD or NVIDIA models.
- Native ARM64 Wine with ARM64/ARM64EC paired DLLs packaged as ARM64X and FEX for x86/x64 guests.
  An x86_64 Wine path using Rosetta on Apple silicon is also available.

Inherited D3D10/11 frontends remain in the runtime; active development focuses on D3D12 games.

## Building

The current D3D12 backend requires macOS 27 and a GPU/compiler supporting Metal 4.
Build tools include Xcode/Metal tools, Python 3, Meson, Ninja, llvm-mingw, native LLVM **15.0.7**
static libraries, Rust **1.85.1** with the selected target libraries, and a matching Wine SDK.
LLVM and Wine libraries must match the native architecture. Keep local dependencies in `toolchains/`
and build outputs in `build/`.

For native ARM64, first prepare the paired Wine SDK, Wine build result, and FEX build result in their
respective projects. Set `WINE_SDK`, `WINE_BUILD`, and `FEX_BUILD` to those directories, put the build
tools on `PATH`, and use fresh output directories:

```sh
python3 scripts/build-owned-arm64x.py --wine-sdk "$WINE_SDK" \
  --output build/local/paired --with-nvapi --with-nvngx --with-fsr
python3 scripts/package-owned-arm64.py --wine-build "$WINE_BUILD" \
  --paired-build build/local/paired --fex-build "$FEX_BUILD" \
  --output build/local/package --with-wow64 --with-nvapi --with-nvngx --with-fsr
```

These scripts build the root `src/` tree and package matching graphics modules, including `d3d12core.dll`.
They consume existing Wine/FEX artifacts rather than building Wine or FEX. Run either script with `--help`
for input options. The x86_64 Meson cross configuration is [`build-win64.txt`](build-win64.txt).

## Configuration and testing

[`d3dalloy.conf`](d3dalloy.conf) lists runtime settings. Use `D3DALLOY_CONFIG_FILE` for a configuration
file, `D3DALLOY_CONFIG` for inline settings, and `D3DALLOY_LOG_PATH` for the log directory.

D3D12 fixtures live in [`graphics-translator/tests/`](graphics-translator/tests/).
Run built fixtures with [`scripts/test-owned-graphics.py`](scripts/test-owned-graphics.py) and `--validation`;
use `--help` for suite and runtime options. Build, CPU, and GPU checks are separate; `meson test` is not
the regression suite. Use copied runtimes and dedicated prefixes, not an installed game's live prefix.
Keep logs, `result.json`, and captures when reporting failures.

## Credits and license

D3Dalloy builds on DXMT's Direct3D frontends, shader converter, and Metal bridge.
See [LICENSE](LICENSE). Imported and vendored components retain their original license and copyright notices.
