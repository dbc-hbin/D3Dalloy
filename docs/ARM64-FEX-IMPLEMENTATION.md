# ARM64 translator integration with Wine and FEX

## Repository boundaries

DXMT owns graphics translation, AIR compilation, the WineMetal bridge, ARM64/ARM64EC/ARM64X modules, staging, and graphics/ABI validation. Wine and FEX are external build inputs, not vendored build implementations.

| Repository | Build ownership | DXMT input |
| --- | --- | --- |
| [Wine-darwin](../../Wine-darwin/README.md) | Wine SDK/runtime, Wine patches, native dependencies, generic Wine probes, Composition frontend | Completed `sdk-result.json`, `runtime-result.json`, or display build/source |
| [FEX-darwin](../../FEX-darwin/README.md) | Darwin FEX source, patches, build and CPU probes | `Darwin/build.py` output with `fex-result.json` and verified emulator modules |
| This repository | Canonical Meson translator graphs and owned runtime package | External SDK/runtime/FEX inputs are validated before use |

The local layout uses sibling repositories. Build options accept explicit paths; there is no automatic Wine/FEX build inside DXMT. Source checkouts for Wine now live in Wine-darwin's ignored `toolchains/`. Existing DXMT `build/local/` logs, source snapshots and manifests remain at their recorded paths; they are historical evidence, not relocated outputs. Existing installed SDK/runtime dependencies remain valid inputs.

Native Mach-O architecture, Windows PE ABI and guest instruction set are separate axes. `build-arm64.txt`, `build-arm64ec-only.txt` and `build-native-arm64.txt` select the translator graphs. An ARM64EC raw PE machine or ARM64 object alone does not establish Rosetta-free execution. The ARM64X producer pairs canonical ARM64/EC graphs and retains Core/Main generation, relocation, TLS and export checks.

## Native Wine build and package recipe

Build Wine and FEX using their own README instructions first. The Wine runtime must match the intended guest/ABI and optional WoW64 configuration. From this DXMT repository, the following consumes that runtime's preserved Wine source and external FEX output. Every output must be fresh; the names are examples, not claimed existing artifacts.

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

`build-owned-arm64-foundation.py` also requires the completed external `--wine-sdk`; neither DXMT orchestrator configures or builds Wine. The SDK consumer verifies the source cut, artifact hashes, native winebuild architecture and both ABI archive formats. The packager still builds DXMT's native and PE32 graphs and validates the assembled runtime; it does not belong in the Wine repository.

The translated Composition path consumes Wine-darwin's separately built `dcomp.dll`. Pass its `--wine-display-build` and `--wine-display-source` to DXMT staging. The source pin, source/header hashes, PE builtin/exports, stale output rejection and prohibition on replacing native Wine overlays remain enforced. See [runtime staging](../graphics-translator/RUNTIME.ko.md).

## Native Wine host evidence and execution boundary

DXMT keeps `preflight-owned-arm64.py`, its regression, and `arm64_abi_probe_*` because they validate translator COM, Unix-call and Metal integration. Generic JIT, address-space, guest-window and runtime-observer sources moved to Wine-darwin; FEX-only probes belong to FEX-darwin.

The moved Wine runner accepts external runtime and fixture paths. Graphics, audited FEX, and any staged runtime require explicit `--dxmt-root` so the existing fail-closed staging audit is retained:

```sh
python3 ../Wine-darwin/scripts/run-owned-wine-arm64.py \
  --dxmt-root . --runtime "$PACKAGE/runtime" \
  --fixture build/local/arm64-abi-new/arm64 --suite abi --gpu \
  --output ../Wine-darwin/build/local/arm64-abi-check-new
```

The generic DXMT GPU runner still rejects native FEX execution. The moved observer runner and retained direct public-suite runs are separate evidence. Native JIT and WoW64 guest-window probes can run in Wine-darwin without importing DXMT. All runs use dedicated fresh prefixes and preserve results after process shutdown and prefix cleanup.

## Current gate disposition — 2026-10-09

The SDK05 and login03 results are historical bounded ABI/Metal and selected frontend observations; their local result files are no longer retained here. **G8, the complete native migration regression matrix, remains open.** These fixture records do not establish full DXIL, native x86 FX or native FG/RR acceptance.

Separate 2026-10-09 isolated ZZZ runs establish owned NGX loading, DLSS city rendering and completion of the resource-check/precompile flow. The FEX IR-tail candidate also passed 34 Wine probes and actual ZZZ DLSS rendering. Its equal-input emitter benchmark measures reduced JIT bookkeeping, not whole-game loading or stutter improvement. See [current game results and limits](../graphics-translator/ZZZ-COMPATIBILITY.ko.md#2026-10-09-fex-최적화와-재검증). This does not close G8 or establish other games and scenes.

The dated gate table, source pins, hashes, commands and prior failures are recorded in [the Wine/FEX historical ledger](../../Wine-darwin/docs/history/ARM64-FEX-IMPLEMENTATION-20261007.md). Historical DXMT build paths do not imply that their payloads or result files still exist. Current graphics status stays in [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md), and remaining work in [PRIORITIES](../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md).
