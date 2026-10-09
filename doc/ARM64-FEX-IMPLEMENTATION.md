# ARM64 translator integration with Wine and FEX

DXMT owns graphics translation, AIR/Metal, ARM64/ARM64EC/ARM64X modules and graphics staging/validation. [Wine-darwin](../../Wine-darwin/README.md) produces Wine SDK/runtime, native dependencies and generic Wine probes; [FEX-darwin](../../FEX-darwin/README.md) produces emulator modules and CPU probes. DXMT validates their completed manifests and artifacts; it does not rebuild Wine/FEX internally.

Native Mach-O architecture, Windows PE ABI and guest instruction set are separate axes. ARM64EC raw PE machine values or ARM64 object creation alone do not prove Rosetta-free execution. The ARM64X producer pairs `build-arm64.txt` and `build-arm64ec-only.txt`, with `build-native-arm64.txt` for native components, preserving Core/Main generation, relocation, TLS and export checks.

## Current gate disposition — 2026-10-09

- **Observed:** isolated ZZZ owned NGX loading, DLSS city rendering and resource-check/precompile completion. The FEX IR-tail candidate passed 34 Wine probes and DLSS rendering; its benchmark proves reduced emitter work, not whole-game loading/stutter improvement. See [game evidence](../graphics-translator/ZZZ-COMPATIBILITY.ko.md) and [FEX analysis](FEX-PERFORMANCE-2026-10-09.md).
- **Open:** G8's complete native migration matrix, full DXIL, native x86 FX/FG/RR coverage, other games/scenes and matched performance.
- **Historical:** SDK05/login03 bounded ABI/Metal/frontend observations are recorded in the external [Wine/FEX ledger](../../Wine-darwin/docs/history/ARM64-FEX-IMPLEMENTATION-20261007.md); their former DXMT result paths no longer imply retained files.

## Native Wine build and package recipe

Build matching Wine runtime and FEX modules in their repositories first. Example paths below must be replaced by the chosen inputs; every output directory must be fresh.

```sh
WINE=../Wine-darwin/build/local/runtime-build
SDK=../Wine-darwin/build/local/paired-sdk
FEX=../FEX-darwin/build/local/darwin01
PAIRED=build/local/arm64x-paired-new
PACKAGE=build/local/arm64-package-new
python3 ../Wine-darwin/scripts/build-paired-sdk.py \
  --wine-source "$WINE/wine-source" --output "$SDK" --jobs 8
python3 scripts/build-owned-arm64x.py --wine-sdk "$SDK" \
  --output "$PAIRED" --with-nvapi --with-nvngx --with-fsr --jobs 8
python3 scripts/test-owned-arm64x.py --build-root "$PAIRED"
python3 scripts/package-owned-arm64.py --wine-build "$WINE" \
  --paired-build "$PAIRED" --fex-build "$FEX" --output "$PACKAGE" \
  --with-wow64 --with-nvapi --with-nvngx --with-fsr --jobs 8
```

The foundation helper also consumes `--wine-sdk`. SDK validation checks source cut, artifact hashes, native winebuild architecture and both ABI archives. The packager builds DXMT's native/PE32 components and audits the assembled runtime. Optional translated Composition consumes Wine-darwin's `--wine-display-build` and `--wine-display-source`; source/header/PE identity checks remain required. See [RUNTIME](../graphics-translator/RUNTIME.ko.md).

## Native Wine host evidence and execution boundary

DXMT retains its preflight and `arm64_abi_probe_*` integration fixtures. Generic JIT/guest-window/observer probes belong to Wine-darwin; FEX-only probes belong to FEX-darwin. The generic DXMT GPU runner rejects native FEX, so use the external observer runner for that route, with explicit `--dxmt-root` to retain staging audits:

```sh
python3 ../Wine-darwin/scripts/run-owned-wine-arm64.py \
  --dxmt-root . --runtime "$PACKAGE/runtime" \
  --fixture build/local/arm64-abi-new/arm64 --suite abi --gpu \
  --output ../Wine-darwin/build/local/arm64-abi-check-new
```

Use private runtimes and fresh prefixes; preserve evidence and remove disposable prefixes after shutdown. Packaging success, native host identity and actual guest rendering are distinct acceptance conditions. Current support remains in [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md), remaining work in [PRIORITIES](../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md).
