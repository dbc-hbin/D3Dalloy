# ARM64-native Wine, owned graphics, and FEX

## Scope and completion boundary

Current source inputs and dated evidence ledger, updated 2026-10-07. The goal is to remove Rosetta 2 from the execution chain: ARM64-native macOS Wine and Unix libraries, native owned AIR compiler/Metal bridge, Windows graphics modules with the loader-required ARM64EC/ARM64X ABI, and verified Darwin FEX integration for x64 and x86 guest instructions.

The canonical translator status is [CORE-IMPLEMENTATION-STATUS.ko.md](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태); [README](../README.md) is the build/entry authority. This document tracks the separate ARM64/Wine/FEX integration boundary, not another graphics support matrix.

**Unified native execution passes on the SDK05 cut.** Native ARM64, FEX x64 and FEX x86 all pass actual COM/Unix-call/Metal readback and the selected D3D12 graphics/compute smoke with eight exact frames on one immutable runtime. Older failures remain dated evidence, not the current disposition. Real-game, presentation, full DXIL and broader native regression acceptance are separate from this no-present smoke.

## Current source and later evidence — 2026-10-07

The runtime builder now pins optimized Wine11.17 `c05971f5ea628cbb8f793662a1c54ba9a2b9d82f` from the existing isolated Wine fork. It requires `--tls-prefix` and `--freetype-prefix`, each containing headers, thin ARM64 libraries and a verified `provenance.json`. It rejects a missing configure library and stages the validated dependency closure with package-relative load paths. The earlier MR11638 pin in the SDK05 table below is historical, not the current production builder input. The older SDK archive route remains a distinct, non-WoW64 build input.

The FEX builder retains baseline2609.1 `9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf` and its Darwin integration, plus 16KB call/return guards and selected upstream thread-termination, PCMPXSTRX and disk-cache patches. [Retained selected validation](../build/local/fex-selected-validation-20261006.json) records nine strict native runs/13 fixture invocations; it is not a new full FEX rebase. Disk cache remains opt-in, memory lookaside defaults to zero, and the main-file default cap is1GiB (not including its index). Owned format0xA609 isolates the changed code-generation contract; exact cache hit counts and game FPS were not measured.

Later retained Wine [Mono installation](../build/local/login03-mono-install-20261007/result.json) and [x64](../build/local/login03-gpu-x64-20261007/result.json)/[x86](../build/local/login03-gpu-x86-20261007/result.json) ABI runs pass on login03. Optional NVAPI/NGX/FSR selections now propagate through both paired graphs, native/PE32 packaging and staging. [Package result](../build/local/arm64x-gpu-features-runtime01-20261007/package-result.json) and [direct FEX x64 NGX/NGX-contracts/FSR SR consumers](../build/local/arm64x-gpu-features03-20261007/feature-results.json) pass; the latter are direct isolated executions because the generic GPU runner rejects FEX. They do not establish native x86 FX, full no-Rosetta child closure for these public suites, FG/RR or G8 acceptance. [Profile consumers](../build/local/arm64x-gpu-features03-20261007/profile-results.json) validate the compatibility identity exposed for ZZZ, not physical NVIDIA/AMD hardware.

The original SDK05 runtime payload was subsequently retired; its results and manifests remain evidence. Build fresh runtimes for new execution. The review identified additional CPU-pointer and fence/cache issues, tracked in [the implementation ledger](D3D12-REVIEW-AND-DESIGN-2026-10-07.md). Do not relabel old results as proof of those later changes.

## Current gate disposition — 2026-10-06

This anchored table preserves the SDK05 gate observation. Current source inputs and later bounded runs are above; G8 remains open.

The owned native compiler/plugin, paired ARM64X production modules and official pinned Wine/FEX builds exist. Native Wine uses relocated shared-data backing without reducing its 4-GiB PAGEZERO or using Rosetta. FEX's generated ARM64 code uses public Darwin MAP_JIT and explicit nested thread write scopes. SDK05 combines the narrow Wine user32/NLS/TEB pointer fixes with owned compiler/Metal CPU-pointer encoding. All three guests pass COM, reverse callback, Unix-call, Metal blit/fence/readback and the selected graphics/compute smoke. Native object handles, GPU addresses, guest callback identities and queue retirement semantics are preserved.

| Gate | Latest observed state | Evidence and limit |
| --- | --- | --- |
| G0 — runtime source/integration | **PASS for owned pinned route** | Wine MR11638 commit de56343b358617d8319b8510356169ad96555e17, FEX-2609.1 commit 9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf, licenses, source/patch hashes and reproducible owned build/staging recipes recorded. Not a vendor Darwin Preview release. |
| G1 — native build identity | **PASS bounded SDK/plugin/native host/package** | Thin ARM64 SDK, owned plugin/compiler and official Wine loader/server/Unix closure are built and audited. Staging verifies the ARM64X/PE32 closure and pinned FEX modules. Actual loader/server/child images are ARM64 with proc_translated=0 for all three selected guest suites. |
| G2 — native compiler/GPU | **PASS for recorded SM50/DXBC probe** | Full plugin, `proc_translated=0`, dispatch 111/314/9, 11 exact GPU words, validation enabled and 495 ARM64 loaded image headers. Direct native dispatch, not PE→Unix/FEX execution or a complete native DXIL/D3D12 matrix. |
| G3 — Windows production modules | **PASS bounded plain ARM64/ARM64EC/automated ARM64X build** | SDK05 builds six paired DLLs from the two ordinary Meson graphs with shared Core/Main generation 40ddcd07ca794d7d8d1e234d8bc54115 and 16KB file/section alignment. Both relocated views have canonical entries/exports/ordinals, code maps and audited TLS callback architecture. The producer completes LLVM 22.1.8's missing TLS-directory DVRT fixup and rejects divergent generations. |
| G4 — architecture-aware tooling | **PASS bounded staging/static/host-observer scope** | Schema 2 separates native/module ABI/guest axes; schema 1 remains legacy x64. The runner accepts all three actual guest ABI/Metal and shader probes only after selected-image architecture, translation state and loaded PE/native SHA256 checks. Failed or timed-out suites retain blocked results and observations. |
| G5 — guest/native ABI | **PASS for all three guests on SDK05** | Native ARM64, FEX x64 and FEX x86 pass COM owner/scalar, reverse callback, Unix-call 111, Metal 64-byte 0xa7 readback and fence 0x123456789. PE32 also passes the typed CPU pointer codec and two-node blit. All use manifest c59930623bc44b5120d6a74534c4d0be2b5704ecf910a67e252b23e4729453dc, matching loaded hashes, complete native child closures, proc_translated=0 and clean validation. |
| G6 — x64 native D3D12 flow | **PASS for selected graphics/compute smoke** | SDK05 preserves raw ARM64X Core machine admission, shared native/EC Core/Main generation and installed-file identity. Native ARM64 and FEX x64 execute eight ordered exact-readback frames, exit 0, with Metal API/Shader Validation and strict no-Rosetta. Runs use --no-present, not game or presentation acceptance. |
| G7 — x86 native WoW64 | **PASS for selected ABI/Metal and graphics/compute smoke** | SDK05 passes actual PE32 COM/Unix/Metal and eight exact graphics/compute frames with strict no-Rosetta. Wine client-function arrays, native PEB64 NLS addresses and TEB32 dereferences preserve guest/native identity. Owned CPU bytecode, pipeline and command/commit/info pointers receive the guest base; native handles/GPU addresses do not. Unexercised Metal4, mesh/DXR, game and presentation contracts remain outside this proof. |
| G8 — native migration matrix | **NOT CLOSED** | No identified native runtime cut executed the selected regression matrix. Existing x64/WoW64 results cannot be relabelled native. |

### SDK05 immutable acceptance evidence

Runtime: `build/local/arm64x-wow64-package-sdk05-20261006/runtime`. All six runs use manifest SHA256 `c59930623bc44b5120d6a74534c4d0be2b5704ecf910a67e252b23e4729453dc`. Wine08 result SHA256 is `0fbbd39833ba083178158619a5aae661875590e555eedcf549a5caf09ff838a1`; paired SDK05 result SHA256 is `72ecc95e09bc4be650274dab0163b051fcbc14dcb77d0e18a75f84bdf5e74249`. FEX source/modules are unchanged from the pinned guest-window03 build.

| Actual guest | ABI/COM/Unix-call/Metal | Graphics/compute exact readback |
| --- | --- | --- |
| Native ARM64 | [PASS,15.97s](../build/local/arm64x-wow64-sdk05-arm64-abi-gpu-20261006/result.json) | [PASS,8 frames,20.60s](../build/local/arm64x-wow64-sdk05-arm64-smoke-20261006/result.json) |
| FEX x64 | [PASS,16.07s](../build/local/arm64x-wow64-sdk05-x64-abi-gpu-20261006/result.json) | [PASS,8 frames,15.94s](../build/local/arm64x-wow64-sdk05-x64-smoke-20261006/result.json) |
| FEX x86 | [PASS,16.11s](../build/local/arm64x-wow64-sdk05-x86-abi-gpu-20261006/result.json) | [PASS,8 frames,14.33s](../build/local/arm64x-wow64-sdk05-x86-smoke-20261006/result.json) |

Every run exits 0, passes Metal API/Shader Validation, matches loaded PE/native hashes and proves `strict_no_rosetta_proven=true` for the actual native child closure. Dedicated prefixes are removed after Wine processes stop. ABI checks include a 32-byte packet, one reverse callback, 64-byte 0xa7 readback and fence 0x123456789; PE32 also passes the typed CPU codec. Shader runs use `--no-present`. The paired build (97.34s), metadata/TLS/generation corruption regression (0.27s) and fresh package/stage verify (75.94s) pass. Clangd is not counted green: its configured header context lacks `cstdint`; real native/EC/PE32 target builds and executions are the acceptance here.

Retained foundation/SDK/paired evidence roots are `build/local/arm64-foundation-20261002/`, `build/local/arm64-sdk-plugin-20261002/` and `build/local/arm64x-paired-production-20261002/`. They are local, ignored artifacts, not files guaranteed by a clean checkout. The dated ledgers below distinguish preserved results/logs from retired runtime copies and the explicitly lost initial compile log. Documentation integration consumes the observed results; it does not rerun builds, Wine or GPU probes.

## Remaining implementable work and external input

The requested selected execution slice is implemented and verified on SDK05. Source inputs are available; the old vendor-input gate is not a blocker. The completed contracts and separate wider validation are:

1. **Preserve the ordinary 4KB x64 image/shared-data acceptance.** The emulated-image protection patch retains Windows execute permission while removing native host execute permission only for non-CHPE x86 image bytes. JIT04 also scopes native writes in the unaligned-access backpatch handler, including its embedded spinlock. The unchanged fixed-address/4KB guest passed scalar/SSE/clock/read-only exception checks with matching loaded hashes and no Rosetta. This does not establish dynamic guest-JIT or general PE-layout acceptance.
2. **Completed selected ARM64/x64/x86 shader flow.** SDK05 executes eight exact graphics/compute readback frames for each guest with native owned compiler evidence, matching Core/Main identity, Metal API/Shader Validation and strict no-Rosetta. Broader shader, game and presentation acceptance remain separate.
3. **Completed PE32 user32/compiler/Metal boundaries.** SDK05 closes the exercised client-array, NLS, TEB, embedded-bytecode, pipeline, compiler-copy and command/submission CPU-pointer faults. All three ABI/Metal and shader checks pass on the same immutable cut. Module/callback identities, native objects and GPU addresses are not biased. SDK02 remains the known-good comparison; dated failed cuts stay in history.
4. **Select and execute the native regression cut after independent x64/x86 acceptance.** Old Rosetta/x64/WoW64 results stay separate. Packaging, source provenance and CPU decoder checks are not substitutes for guest execution.

## Concrete acceptance gates

| Gate | Required evidence | What it closes |
| --- | --- | --- |
| G0 — source provenance | Verified official Wine/FEX source URL and immutable revision/digest; documented macOS integration patches and license | Eligibility to build/run that runtime, not execution |
| G1 — native build identity | AIR/bridge Mach-O arm64 artifacts and ARM64-capable dependency closure; build process native identity | Native build only |
| G2 — native compiler/GPU | ARM64 process, no translation, actual owned shader-to-AIR compilation with input family/entry points identified, Metal pipeline and exact GPU readback under validation | Only the recorded compiler/Metal probe; wider DXIL/D3D12 coverage requires its own matrix |
| G3 — Windows modules | Actual D3D12/DXGI/WineMetal target DLLs; plain ARM64 separated from final ARM64EC/ARM64X CHPE/native-range evidence | Windows target build only |
| G4 — architecture-aware tooling | Mismatch rejection and old-manifest tests; isolated x64/WoW64 regression stage/run; package manifest and execution result accurately distinguish native, module ABI and guest axes | Staging/runner foundation |
| G5 — guest/native ABI | Actual native Wine+FEX x64 execution crossing ARM64EC calls, callbacks/COM and native boundary; exact values/lifetimes; no Rosetta | x64 ABI integration only |
| G6 — native D3D12 guest flow | x64 guest D3D12 create/compile/submit/fence/readback across real ARM64EC and native WineMetal, validation clean; no global waits added | Bounded x64 end-to-end smoke |
| G7 — x86 WoW64 | Independent x86 guest pointer/handle/callback and D3D12 readback under the native Wine/FEX configuration | Bounded x86 end-to-end smoke |
| G8 — migration acceptance | Explicitly selected regression matrix on identified native runtime cuts, including async ordering, resource lifetime, indirect indices and recent review regressions | Only that matrix; not general conformance/game compatibility |

Strict no-Rosetta acceptance requires the launched process chain and loaded native dependency path to remain ARM64. A parent process reporting native execution does not prove a child. Universal binaries need actual selected-slice evidence; Mach-O ARM64 labels cannot substitute for FEX/guest observations. ARM64EC metadata proves code availability, not that execution used that code.

## Reproduction

All commands run from the repository root. Use fresh output names beneath ignored `build/local/`; never reuse recorded evidence destinations, live games or installed runtimes. The current runtime recipe requires a checkout containing the pinned optimized Wine commit below `toolchains/`, verified GnuTLS/FreeType prefixes, llvm-mingw, matching native LLVM15, Meson/Ninja and the Apple SDK. The original SDK/foundation recipes below have narrower purposes and do not themselves execute native guests.

### Current optimized runtime and optional frontend package

Set `WINE_REPOSITORY`, `TLS_PREFIX` and `FREETYPE_PREFIX` to existing verified local inputs; the command does not discover, download or fabricate them. All four output paths below must be new. Omit optional frontend flags consistently from both paired build and packaging when those modules are not selected.

```sh
: "${WINE_REPOSITORY:?isolated pinned Wine checkout required}"
: "${TLS_PREFIX:?verified ARM64 GnuTLS prefix required}"
: "${FREETYPE_PREFIX:?verified ARM64 FreeType prefix required}"
FEX=build/local/fex-review-new
WINE=build/local/wine-review-new
PAIRED=build/local/paired-review-new
PACKAGE=build/local/package-review-new
python3 scripts/build-owned-fex-windows.py --output "$FEX" --jobs 8
python3 scripts/build-owned-wine-arm64-runtime.py \
  --wine-repository "$WINE_REPOSITORY" --output "$WINE" \
  --windows-abi arm64x --wow64 --fex-result "$FEX/fex-result.json" \
  --tls-prefix "$TLS_PREFIX" --freetype-prefix "$FREETYPE_PREFIX" --jobs 8
python3 scripts/build-owned-arm64x.py --output "$PAIRED" \
  --wine-source "$WINE/wine-source" --with-nvapi --with-nvngx --with-fsr --jobs 8
python3 scripts/package-owned-wine-arm64.py --wine-build "$WINE" \
  --paired-build "$PAIRED" --fex-build "$FEX" --output "$PACKAGE" \
  --with-wow64 --with-nvapi --with-nvngx --with-fsr --jobs 8
```

Package success is not guest success. Use the selected ABI/smoke runner and a fresh result directory; public frontend probes require their exact fixture/ABI/validation harness. The generic GPU runner must not have its native rejection removed merely to relabel an unsupported test route as accepted.

### Current native SDK and full Unix-plugin recipe

From the repository root, choose fresh output names (the helper also builds the x64 regression SDK using existing Rosetta):

```sh
export PATH="/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:$PWD/toolchains/tools/bin:$PATH"
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++
SDK=build/local/arm64-sdk-plugin-reproduce01
OWNED=$SDK/owned-plugin
python3 scripts/build-owned-wine-arm64-sdk.py --output "$SDK" --jobs 8
python3 scripts/test-owned-wine-arm64-sdk.py --wine-root "$SDK" --output "$SDK/layer-check"
meson setup "$OWNED" . --cross-file build-arm64.txt --native-file build-native-arm64.txt \
  -Dnative_arch=arm64 -Dnative_llvm_path=toolchains/llvm-15-arm64/llvm@15/15.0.7 \
  -Dwine_build_path="$SDK/wine-build-hybrid" -Denable_d3d12=true \
  -Dwine_builtin_dll=true --buildtype=debugoptimized
ninja -C "$OWNED" -j8 src/winemetal/unix/winemetal.so \
  src/airconv/darwin/airconv src/winemetal/winemetal.dll.postproc
python3 scripts/test-owned-null-descriptor.py --build-root "$OWNED" --native-arch arm64 \
  --unix-plugin "$OWNED/src/winemetal/unix/winemetal.so" \
  --wine-build "$SDK/wine-build-hybrid" --output "$SDK/native-plugin-smoke"
```

This recipe closes only the SDK/plugin and recorded SM50/DXBC GPU scope. It creates no Wine prefix and never launches a Windows guest. Its x64 layer comparison intentionally uses existing Rosetta; that comparison is not part of the strict native acceptance claim.

### Automatic paired ARM64X recipe

After preparing the pinned SDK source snapshot, select a fresh output:

```sh
python3 scripts/build-owned-arm64x.py \
  --output build/local/arm64x-paired-reproduce01 \
  --wine-source "$SDK/wine-source" --jobs 8
python3 scripts/test-owned-arm64x.py --build-root build/local/arm64x-paired-reproduce01
```

This builds actual paired Wine CRT/spec/import inputs and both canonical owned Meson graphs; it does not reuse previous production objects or launch a PE guest. It audits the real relocated native/EC views, not only a metadata fixture.

### Native Wine build and package recipe

After the SDK recipe, use two fresh output roots:

```sh
WINE=build/local/arm64-runtime-reproduce01
PACKAGE=build/local/arm64-owned-package-reproduce01
python3 scripts/build-owned-wine-arm64-runtime.py --sdk "$SDK" --output "$WINE" \
  --tls-prefix "$TLS_PREFIX" --freetype-prefix "$FREETYPE_PREFIX" --jobs 8
python3 scripts/package-owned-wine-arm64.py --wine-build "$WINE" --output "$PACKAGE" --jobs 8
```

The runtime helper rebuilds an isolated archive of the local pinned Wine fork, configures, builds and installs real loader/server and native/plain ARM64 PE closure. `--without-x` and `--without-ffmpeg` bound optional closure; the latter avoids auto-detected unpinned Homebrew FFmpeg, not a new multimedia support claim. The packager builds/installs the complete canonical plain ARM64 owned graph, including unchanged D3D10/11, native WineMetal and AIR compiler, stages schema 2 and verifies the normal owned wrapper with `--version`. Package completion does not require successful guest execution.

The separate execution observer can be invoked against a freshly compiled plain ARM64 ABI pair (the two files must be `abi-guest.exe` and `abi-callee.dll`):

```sh
python3 scripts/run-owned-wine-arm64.py --runtime "$PACKAGE/runtime" \
  --fixture build/local/arm64-abi-reproduce01/arm64 \
  --output build/local/arm64-host-observation-reproduce01
```

Use the actual fixture output directory. The original low-address failure belongs to the older SDK package, not the current optimized guest-window runtime. `--gpu` additionally requires the ABI fixture GPU checks and cannot turn host-only observations into GPU acceptance. The package-wrapper and direct real-loader observer are distinct routes: direct observation preserves the audited wrapper policy through explicit environment and manifest hash; a parent-only architecture check is insufficient.

### Foundation diagnostic and ABI compile-only recipe

For the current SDK input, after completing the recipe above, choose another fresh output:

```sh
python3 scripts/build-owned-arm64-foundation.py \
  --output build/local/arm64-foundation-reproduce01 \
  --wine-build "$SDK/wine-build-hybrid" \
  --wine-ec-build "$SDK/wine-build-pure-ec" \
  --wine-source "$SDK/wine-source" \
  --jobs 8

python3 scripts/preflight-owned-arm64.py --compile-probes \
  --out build/local/arm64-abi-reproduce01
```

The current foundation helper exercises scoped native/PE builds and a GPU probe and delegates hybrid production work to `scripts/build-owned-arm64x.py` with the pinned Wine source input. Retained initial foundation failures are dated evidence, not a claim that paired automation remains incomplete. The compile-only preflight returns exit 2 / BLOCKED, even when all six ABI artifacts compile, because it does not execute the native guest chain. Neither helper downloads or launches a new Wine/FEX runtime.

The initial recorded foundation command used the older unpatched `native-build/wine-sdk-attempt` and `native-build/wine-ec-sdk-attempt` outputs under `arm64-foundation-20261002/`. Their window-driver/full-plugin failure belongs to that old cut; use the patched SDK inputs above for current build diagnostics. The native runtime observer recipe below documents host execution with blocked PE startup, not a working FEX/native guest command.

## Architecture: three independent axes

| Axis | Values / meaning | Must not imply |
| --- | --- | --- |
| Native macOS process architecture | Mach-O `arm64` for Wine loader/server, Unix modules, owned AIR compiler and Metal bridge | The architecture of a Windows application or DLL |
| Windows module ABI | Plain ARM64 PE, ARM64EC x64-compatible PE, ARM64X hybrid image, AMD64 PE, I386 PE | A Mach-O host architecture or proof of emulator execution |
| Application guest instruction set | x64, x86, or a separate native ARM64 application | x86 WoW64 and x64 ARM64EC are interchangeable |

Target x64 path:

```text
x64 application instructions
    -> verified macOS FEX execution + Wine ARM64EC dispatch
    -> ARM64EC D3D12/DXGI/Windows WineMetal DLLs
    -> Wine Unix-call ABI (fixed-width POD / handles)
    -> ARM64 Mach-O winemetal + owned AIR compiler
    -> Apple Metal 4 driver / GPU
```

The Unix-call interface is not an arbitrary C++ cross-architecture ABI. Its numbered entry points, parameter layouts, handles, pointer conversion, callback conventions, and native/WoW64 dispatch tables must agree. COM and guest callbacks on the Windows side must follow ARM64EC interoperation rules. LLVM IR/context ownership remains per compilation. Submission ownership and GPU completion semantics remain unchanged; architecture work must not add global CPU/GPU waits.

### Plain ARM64, ARM64EC, ARM64X, and x86

- **Plain ARM64 PE** is a useful native-Windows-ABI build/smoke target. It is not a drop-in DLL for an x64 application.
- **ARM64EC** permits native ARM64 code to interoperate with emulated x64 code and x64-compatible data/calling conventions. The Windows target is `arm64ec-w64-mingw32`; the native macOS target is independently `arm64-apple-darwin`.
- **ARM64X** packages compatible views/code for ARM64/ARM64EC loading. `scripts/build-owned-arm64x.py` combines canonical Meson builds from `build-arm64.txt` and `build-arm64ec-only.txt`, real Wine CRT/spec/import inputs and both MinGW C++ startup/runtime sets. The obsolete single-EC hybrid config was deleted after fresh paired proof; `-marm64x` belongs to the paired link, not compile-only probes. Actual guest loader selection still needs execution evidence.
- An ARM64EC final PE can advertise **AMD64 `0x8664`** and be described as x86-64 by `file`. Audit final load-config **CHPE/ARM64X metadata and native code ranges**, not just COFF Machine. An intermediate object with machine **`0xA641`** alone is not a valid final DLL proof. Record `llvm-readobj --file-headers --coff-load-config` evidence and relevant code maps.
- **x86 WoW64** is a distinct 32-bit Windows guest path with pointer-width conversion, Unix-call thunks, loader and emulator integration requirements. A working ARM64EC x64 process does not prove x86 support.

## Safety, supervision and isolation

- No unknown-origin executable, license acceptance, paid product requirement, security/JIT policy modification, or modification of installed games/live prefixes.
- Any essential restricted dependency requires a specific request identifying source, license/action and why it blocks the next gate. Continue independent reachable work first.
- Use fresh, clearly named output directories beneath ignored `build/local/`; reproduction names above must not already exist. Never replace current comparison runtimes or another worker's artifacts.
- Preserve logs, manifests, readbacks and failures. Remove disposable prefixes only after their scoped Wine processes stop; retain only runtimes with an active verification/comparison purpose.
- Do not change rendering semantics speculatively to make native execution look correct. Failures must be traced to a concrete ABI/build/runtime contract.
- Report per-gate complete, failed, or blocked evidence. No public push/merge is requested.

## Initial foundation execution disposition

The remainder is a historical ledger. Past blockers, pins and then-proposed implementation slices below do not override the current source/recipe or the later accepted SDK05/login03/frontend runs above.

**Initial cut, 2026-10-02 (before the SDK/plugin follow-up).** This section preserves the initial foundation cut and its failures, not the current blockers. The [native SDK/Unix-plugin follow-up](#native-sdkunix-plugin-follow-up) below supersedes its G1/G2 window-driver/plugin blocker; guest gates remain open.

### Implemented build and binary contracts

- Native Mach-O selection is explicit (`-Dnative_arch=arm64`), independent from the Windows ABI. `build-native-arm64.txt` selects the native build; cross builds also pass the project option explicitly because Meson native-file options belong to the build-machine namespace.
- In the initial cut, `build-arm64.txt` targeted plain ARM64, `build-arm64ec-only.txt` pure ARM64EC, and the then-present `build-arm64ec.txt` attempted a single-EC ARM64X link. That last config is now deleted and replaced by the paired helper; this historical configuration is not a current command/input. `-marm64x` is link-only: the installed clang 22 rejects it for compile-only invocations.
- `build_native_components=true` remains the default. Explicit `false` is a cross-build-only PE target mode, not a dummy Unix library or a full-runtime success. It uses the canonical Meson D3D12/DXGI/WineMetal targets and native Wine builtin postprocessing.
- `scripts/build-owned-arm64-foundation.py` builds the current canonical native AIR/Metal sources, audits LLVM/tool and artifact architectures/dependencies, runs the native GPU probe, and attempts full and independently scoped PE target builds. It keeps failures visible and exits nonzero while full configurations remain blocked. The temporary object-only source/flag inventory was removed after actual PE targets became buildable.

The final recorded run is `build/local/arm64-foundation-20261002/native-build/closure/result.json`. It contains actual plain ARM64 **and** pure ARM64EC `d3d12.dll`, `dxgi.dll`, and `winemetal.dll`: six linked and Wine-builtin-postprocessed production PE images, not just objects or toy DLLs. The three EC files have raw Machine `0x8664`, CHPE metadata and native ARM64EC code ranges. Independent `llvm-readobj --file-headers --coff-load-config --coff-imports --coff-exports` dumps are retained beside all six images as `*.dll.readobj.txt`; LLVM identifies the EC images as `COFF-ARM64EC` and may normalize its printed Machine to ARM64EC. Raw-header and semantic identities are deliberately recorded separately.

The paired ARM64X metadata image has real ARM64/EC/X64 maps and dynamic relocations, but is explicitly a no-entry metadata fixture. It is **not** a production translator or guest execution result. The full ARM64X Meson configuration still fails its native-half CRT entry-point link requirement (`mainCRTStartup`); the pure EC success does not close that configuration.

Exact diagnostic: `ld.lld: error: <root>: undefined symbol: mainCRTStartup (native symbol)`, retained in `native-build/closure/windows-arm64x/meson-logs/meson-log.txt`. Meson subsequently reports `C shared or static library 'winecrt0' not found`; the archive exists, but its link probe fails. This is not an absent-on-disk SDK diagnosis.

### Native compiler and Metal proof

`native-build/closure/gpu/gpu.log` records a thin ARM64 process with `proc_translated=0`, the exact loaded standalone `winemetal.dylib`, both Metal API and shader validation, and Apple M5 Pro execution. The existing null-descriptor probe compiled through the actual public owned SM50 compiler entry points, reported Metal 4 support/queue creation, and matched **11/11 exact GPU readback words** (null descriptors, atomics, out-of-range stores and root-descriptor round-trip).

The native compiler executable and standalone bridge depend only on recorded system libraries/frameworks; the probe additionally loads the audited ARM64 bridge. Dependency and install-name dumps, LLVM archive architecture checks, hashes, compile/link logs and GPU output are retained under `native-build/closure/`. This proves the native compiler/standalone Metal path. It does not prove a loadable Wine Unix plugin or any emulated guest.

<a id="real-local-wine-sdk-attempt-and-remaining-native-plugin-blocker"></a>

### Real local Wine SDK attempt and initial native plugin blocker

The installed x64 SDK was not treated as the end of the investigation. The existing local Wine fork at `c2f86161d543acda5be91ce60be881b3efea9658` successfully configured for native ARM64 with hybrid `aarch64,arm64ec` targets, and separately for pure `arm64ec`. Isolated builds produced real native ARM64 `winebuild` and `ntdll.so`, plus the corresponding Wine CRT/import archives. No SDK was installed into a live runtime.

In this initial cut, the fork's native window driver failed at `dlls/winemac.drv/cocoa_window.m:911`: `d3dmetal_objc.h` and its implementation declare `WineMetalLayer` only inside `#if defined(__x86_64__)`, but the window code uses the class unconditionally. This is a concrete fork-specific D3DMetal architecture-guard inconsistency, not a missing include, lack of Apple ARM64 signal handling, or a 16-KiB-page impossibility. At that point the external source was not patched and no link stub was substituted. Full native `winemetal.so` setup was therefore blocked by missing real `winemac.so`, while PE-only builds passed. The later isolated snapshot patch and real SDK/plugin build resolve this specific blocker; they do not close guest integration.

Hybrid Wine SDK archives are merged under `aarch64-windows`; pure EC archives use `arm64ec-windows`. The `native_archs` guards in Wine `tools/makedep.c` matter: inspecting its EC path string alone gives the wrong hybrid SDK layout. Configure/build logs are retained as `native-build/wine-sdk-attempt-*` and `native-build/wine-ec-sdk-attempt-*`.

### Architecture-aware staging and preflight

- Stage CLI: `--native-arch x86_64|arm64 --windows-abi x64|arm64|arm64ec|arm64x`. Existing defaults stay x64; `--dxmt-install32` remains the established legacy WoW64 configuration.
- Schema 2 records native/module ABI and directories, hashes the Wine loader/server, native and Windows ntdll, and owned modules, and records/rechecks PE metadata and Mach-O slices. Schema 1 still means the documented legacy x64 package. A genuinely ARM64 wineserver in the existing x64 runtime is recorded without mislabeling the Wine loader.
- Runner `--guest-arch x86|x64|arm64` is an optional assertion checked against the actual fixture. Plain ARM64 execution has a distinct valid path when supplied with its real runtime. Unverified native x86/x64 FEX execution is rejected; binary labels never set `strict_no_rosetta_proven=true`.
- `scripts/preflight-owned-arm64.py` compiles real ABI fixtures and performs a **bounded static audit** of required native paths and the selected Unix directory. It does not reject unused Intel sibling payloads. It requires pinned external-source records and matching artifact hashes; those records are not an independent attestation that a binary was built from the claimed source. Owned bridge provenance instead uses verified staging ownership/hash records. Transitive dyld closure, actual child architecture and loaded-chain observations remain explicit unproven prerequisites.
- The real fixtures cover mixed integer/floating/pointer calls, fixed-width high-bit structures, COM identity and heap ownership (`Release` to zero), reverse COM callbacks and actual owned `WMTGetOSVersion` Unix-call 111. Six guest/DLL artifacts were compiled across x64→EC, plain ARM64, and separate x86 WoW64 pairs. There is no fake emulator or successful unexecuted guest result.

### Executed regressions and retained evidence

All paths below are relative to `build/local/arm64-foundation-20261002/`:

| Evidence | Observed result | Boundary |
| --- | --- | --- |
| `native-build/closure/result.json`, `gpu/gpu.log` beneath that directory | Native compiler/Metal 11/11; six production plain ARM64/EC PE DLLs | Initial overall run was blocked for full Wine plugin/hybrid configuration; plugin blocker superseded below |
| `staging/runtime-complete` | Fresh schema-2 combined x64/WoW64 stage and verify passed | Manifest SHA256 `7c1f2778a0dc9601efcd334c03d101366c699f926cbf2b8183657ea7be17322c` |
| `staging/complete-capabilities-x64/result.json`, `staging/complete-capabilities-x86/result.json` | Both actual capabilities runners passed with API/shader validation enabled | Legacy x64/WoW64 regression, not native FEX |
| `staging/rejected-guest.log` | ARM64 assertion against an x64 fixture rejected before prefix creation | Architecture mismatch denial |
| `staging/binary-audit.json` | Real plain ARM64, final AMD64-labelled EC, and paired ARM64X classification | Static binary proof only |
| `integration-reviewed/result.json` | All six ABI artifacts compiled; exit 2 / BLOCKED | Native guest execution not performed |
| `integration-reviewed/owned-provenance-smoke.json`, `integration-reviewed-stage/result.json` | Actual owned-module provenance accepted; incompatible legacy candidate blocked | Static audit only |
| `integration-legacy-wow64/result.json` | Actual x86 guest/DLL ABI checks and Unix-call passed, exit 0; 32-bit pointer, 32-byte packet, one callback, OS `[27,0,1]` | Legacy WoW64 only; ARM64EC/native FEX not exercised |
| `integration-research/vendor-research.json` | Vendor/upstream URLs, pins, observations and missing Preview source recorded | Research, not runtime acceptance |

Tooling regressions passed: `test-owned-graphics-build.py` **16/16**, `test-owned-graphics-staging.py` **26/26**, `test-owned-graphics-diagnostics.py` **59/59**, and `test-owned-arm64-preflight.py` **14/14**. Schema-1 verification of the retained review runtime also passed. The ABI run preserved fresh-prefix Vulkan/OLE/RpcSs bootstrap errors; its fixture assertions passed, but this is not a claim of globally warning-free Wine. All disposable test prefixes were removed only after scoped Wine shutdown/wait. Superseded copied runtime payloads were retired while preserving their evidence.

Actual legacy-default Meson reconfiguration and Ninja D3D12/DXGI/WineMetal PE plus Unix-plugin targets also passed (`native-build/legacy-prefix-setup.log`, `legacy-prefix-build.log`). Introspection retained `native_arch=x86_64`, `build_native_components=true`, `native_bridge_only=false`, and the Unix plugin remains thin x86_64. The existing static LLVM SDK has no `llvm-config`; its official LLVM CMake metadata is now supported without a new executable requirement or duplicate library inventory. ARM64 reconfiguration/targets and the native GPU probe passed after that final metadata change too (`native-build/closure/gpu-after-sdk-metadata/gpu.log`).

### Reproduction and initial gate disposition

The [foundation diagnostic recipe](#foundation-diagnostic-and-abi-compile-only-recipe) above separates current patched SDK input from the unpatched initial cut. The dispositions here remain the initial results, intentionally not rewritten as later PASS.

| Gate | Initial disposition |
| --- | --- |
| G0 | Official upstream pins inspected; exact vendor macOS Preview FEX source/integration revision not established. Upstream Windows PE FEX-on-Darwin remains a separate unvalidated route. |
| G1 | Native standalone AIR/Metal artifacts and real native Wine core SDK built. Full Wine Unix plugin blocked by the fork window-driver error. |
| G2 | PASS for the recorded standalone native compiler/Metal probe only. |
| G3 | PASS for actual plain ARM64 and pure EC D3D12/DXGI/WineMetal PE targets. Production ARM64X remains blocked; its metadata fixture is not a substitute. |
| G4 | PASS for recorded architecture-aware staging, mismatch denial and legacy regression scope. |
| G5–G8 | NOT CLOSED: no verified native Wine/FEX guest chain, no native guest D3D12 readback, no native x86 WoW64 acceptance, and no migrated native regression matrix. |

The initially proposed bounded step was an isolated pinned-source snapshot, the architecture-guard correction, real ARM64 `winemac.so`, and full owned `winemetal.so` with dependency auditing. That step is now completed within the native SDK/plugin scope below; the original Wine repository and live runtimes remain untouched.

At the initial cut, remaining work was native Wine host/runner bring-up followed by FEX integration and ABI execution. Host/package tooling and native process observations now supersede that backlog; the low-address compatibility prerequisite and G5–G8 remain open. No paid CrossOver requirement is imposed. This dated authorization does not resume the currently held execution track. Restricted acquisition, license acceptance, security/JIT changes or live-prefix mutation require a specific separate request.

## Native SDK/Unix-plugin follow-up

**Follow-up cut, 2026-10-02 (after the initial foundation).** Evidence root: `build/local/arm64-sdk-plugin-20261002/`. `native-plugin-result.json` records the exact full-plugin build/smoke commands, hashes and outcomes; `sdk-result.json`, `wine-source-provenance.json` and `layer-regression03/result.json` record SDK and branch evidence. This closes the bounded native SDK/Unix-plugin build/load/compiler/GPU step, **not a native Wine host or FEX guest acceptance gate**.

### Isolated source and minimal patch

`scripts/build-owned-wine-arm64-sdk.py` archives the exact local commit `c2f86161d543acda5be91ce60be881b3efea9658`, tree `41c06ff9731721fca49869750c9acb588bde4f78`, rather than copying the other repository's dirty working tree. It restricts generated source/build outputs to this project's ignored `build/local/` area and performs no installation. The archive SHA256 is `9311369e3447b3a2cae772cabe318609b700ff4989c1f3458e79e3219ac664ac`.

The retained [source patch](../scripts/patches/wine-arm64-metal-layer.patch), SHA256 `75e9f3bf093e750af3ca30ba7b016bd86c1f7526d11c43c0626b6063ea7f665a`, has only two architecture-guard changes:

1. `WineMetalView::makeBackingLayer` keeps the existing x64 `WineMetalLayer` factory and uses ordinary `CAMetalLayer` otherwise. Device, framebuffer, filtering, background and retina behavior are unchanged.
2. Real ARM64 linking exposed `macdrv_client_surface_presented` as another undefined x64-only implementation. The `CLIENT_SURFACE_PRESENTED` dispatch case now has the same x64 guard. Its sole producer is `WineMetalLayer::nextDrawable` in x64-only `d3dmetal_objc.m`; the handler in `d3dmetal.c` and event release in `cocoa_event.m` are likewise x64-only. No ordinary ARM64 notification is discarded and no stub is introduced. The traced contract is retained in `metal-layer-guard-contract.json`.

Original/patched file hashes and exact archive/configure/build commands are preserved. The exact first ARM64 link diagnostic is retained in `wine-sdk-logs/initial-arm64-link-failure.txt`; its initial full compile log was overwritten during iteration and is explicitly **not** claimed preserved. Final build logs and both patch hunks are retained; reverse dry-run with `--fuzz=0` passed. No GPTK internals were imported into the owned translator.

### Verified outputs and branch behavior

- Fresh hybrid and pure-EC SDK configurations produced real thin ARM64 `winebuild`, `ntdll.so`, `win32u.so`, `winemac.so` and their corresponding PE import/CRT archives. A separate x64 driver/SDK comparison also built; only that comparison disables unavailable x64 FreeType, not Metal or D3DMetal.
- `scripts/test-owned-wine-arm64-sdk.py` dlopens each real driver and instantiates its actual `WineMetalView`. ARM64 selects `CAMetalLayer`; x64 retains `WineMetalLayer` and its distinct `nextDrawable` override. Both pass device/framebuffer/filter/background, retina 2→1, actual 32×32 drawable and completed GPU-clear checks. The x64 comparison runs through existing Rosetta; it is explicitly separate from native acceptance. This is layer/driver regression evidence, not D3DMetal rendering acceptance.
- Canonical Meson/Ninja built the full owned ARM64 Unix plugin against the new SDK. `owned-plugin/src/winemetal/unix/winemetal.so` SHA256: `85f3f7ddde5e79e2a4bfb31ac117dfdb626ee9f6bbd5602cfa7ecb940cc3da22`. No placeholder window driver, fake entry point, or standalone-library substitution was used.
- The new `--unix-plugin` mode of `scripts/test-owned-null-descriptor.py` loaded that exact plugin and its isolated real Wine dependencies. The process reported `proc_translated=0`; all **495 loaded Mach-O image headers** were ARM64. Actual dispatch entries **111/314/9** verified OS version, Metal 4 capability and real command-queue creation. The plugin's public SM50 compiler produced metallib and **11/11 exact GPU readbacks** passed with Metal API and shader validation. No Wine prefix was created. This is direct native dispatch, not a PE-to-Unix/FEX call.
- `native-dependencies.log` and `native-plugin-load-commands.log` retain static dependency/load-command audits. The only non-system dependency chain is the actual isolated `winemetal.so` → `winemac.so`/`ntdll.so`, with `winemac.so` → `win32u.so` → `ntdll.so`; loaded paths and thin ARM64 hashes are recorded. The pre-existing standalone ARM64 probe mode also passed after the probe changes.

### Reproduction

Use the [current native SDK/full-plugin recipe](#current-native-sdk-and-full-unix-plugin-recipe) above. It is the same retained build/dispatch scope, not guest acceptance.

### Historical independent ARM64X diagnosis

`arm64x/result.json` and `arm64x/commands.json` show that single-EC-object executable probes with `-marm64x` lack the native startup/runtime half. The defect is broader than `find_library`: supported-link-argument checks also fail `mainCRTStartup`. A genuine DLL-shaped probe and real production WineMetal DLL linked using paired ARM64/EC objects, official Wine spec objects and both real runtime sets; native Wine builtin postprocessing passed. The manual WineMetal image has nonzero entry points, CHPE ARM64/EC/X64 maps and ARM64X dynamic relocations. It reused the prior owned production objects/SDK with 39 input hashes, not a fresh paired production build from this new snapshot.

No shared Meson probe was weakened: a one-line bypass would conceal the missing paired production recipe. At this SDK follow-up cut, automated ARM64X D3D12/DXGI/WineMetal was still incomplete. The later [fresh paired production proof](#automated-paired-arm64x-production-proof) closes that bounded build gap; the manual image remains historical evidence, not guest proof.

**Boundary of this SDK follow-up:** native SDK/full Unix-plugin G1 and recorded plugin compiler/GPU G2 pass. No native Wine PE caller or FEX guest ran in this step. The later host/package follow-up is separate; G5–G8 stay open. No live runtime replacement, other-repository edits, restricted acquisition, license/security change, or public push occurred.

## Automated paired ARM64X production proof

**Fresh production cut, 2026-10-02.** Authoritative root: `build/local/arm64x-paired-production-20261002/`. Observed command:

```sh
python3 scripts/build-owned-arm64x.py \
  --output build/local/arm64x-paired-production-20261002 \
  --wine-source build/local/arm64-sdk-plugin-20261002/wine-source --jobs 8
python3 scripts/test-owned-arm64x.py --build-root build/local/arm64x-paired-production-20261002
```

These are recorded commands, not destinations to overwrite. `result.json` reports **PASS, 54 commands, zero failures**; `source-cut.json` identifies **503 owned inputs**, all unchanged before/after the build. Logs and per-image maps/readobj dumps retain the compiler/linker and binary audits. Wine source-import provenance remains the isolated local snapshot, not official FEX provenance.

- Fresh real Wine CRT, spec/startup and import archives were built for both ABIs; MinGW contributes actual paired C++ startup/runtime inputs. Canonical `build-arm64.txt` and `build-arm64ec-only.txt` Meson graphs provide current production owned objects. Foundation now delegates to this recipe.
- `-defarm64native` emits canonical native exports/ordinals. `aarch64-windows/winemetal.dll`, `dxgi.dll` and `d3d12.dll` have real native and EC entry targets, both code halves plus x64 thunks, CHPE code maps, ARM64X dynamic relocations and Wine builtin markers. Both dynamically relocated views are audited for canonical exports and ordinals; metadata alone is not treated as proof.
- The real-artifact regression passed destructive-input rejection for zero entries, missing native exports, missing builtin marker and ordinal drift. No Meson compiler/library checks were bypassed.

This closes **bounded G3 automatic three-DLL production build**. It does not establish which hybrid view a real guest loader uses, PE→Unix ABI execution, FEX execution or a Rosetta-free application. The obsolete single-EC hybrid cross config was removed after this proof; the dated initial configuration/diagnostics above remain history.

## Native Wine host evidence and execution boundary

**Host build cut, 2026-10-02.** `build/local/arm64-runtime-package-final-20261002/runtime-result.json` reports **PASS, 43 commands** and actual loader `wine-11.17`. The source is the isolated archive of local Wine commit `c2f86161d543acda5be91ce60be881b3efea9658`, tree `41c06ff9731721fca49869750c9acb588bde4f78`, archive SHA256 `9311369e3447b3a2cae772cabe318609b700ff4989c1f3458e79e3219ac664ac`; it is **not official Darwin FEX provenance**. The source-import identity and earlier SDK/branch evidence are unchanged.

**Complete owned package cut:** `build/local/arm64-owned-package-cut-20261002/package-result.json` reports **PASS, six commands, zero failures**. `source-cut.json` records the stable current owned source inputs; `owned-build/` and `owned-install/` contain the fresh complete canonical plain ARM64 graph (D3D10/11 unchanged, D3D12, DXGI, WineMetal, native Unix plugin and AIR compiler). The staged `runtime/owned-graphics-runtime.json` is **schema 2**, independently verified by the existing stager. The real normal owned wrapper `--version` returns `wine-11.17`. The Wine builder records **33 native closure images / 751 plain ARM64 PE images**. Earlier two owned-package setup failures remain dated logs, not current acceptance or a new blocker.

The actual package command was:

```sh
python3 scripts/package-owned-wine-arm64.py \
  --wine-build build/local/arm64-runtime-package-final-20261002 \
  --output build/local/arm64-owned-package-cut-20261002 --jobs 8
```

This completes the user-approved **packaging boundary**; the execution layer is a separate task. `smoke-summary.json` records the normal-wrapper smoke, observed execution-result path and payload retirement. Only three superseded runtime copies created in this task were removed (`arm64-runtime-20261002/runtime`, `arm64-runtime-final-20261002/runtime`, `arm64-runtime-package-20261002/runtime`); source/build directories, logs/results, the address-space probe and compiled GPU fixture remain. Use the accepted staged runtime, not a retired payload path.

The build applies the existing [Metal-layer guard patch](../scripts/patches/wine-arm64-metal-layer.patch) and [native loader patch](../scripts/patches/wine-arm64-runtime-loader.patch) (SHA256 `7cd15564d48f4693eec9dadf4c39d5858baf84b270448a3db34ca6c23e7a5ebd`). The latter retains the required **4-GiB PAGEZERO** and real Wine loader info plist. Earlier small-PAGEZERO loader SIGKILL is fixed; lowering PAGEZERO is not a valid solution to guest low-VA requirements. The final bounded configure excludes optional auto-detected FFmpeg and X, not Metal or native macOS window-driver support.

Real native loader and foreground server observations report `proc_translated=0` with required loaded images selected and hashed **per PID**, rather than inferring a child from the parent. However, Wine fails to map the shared page at `0x7ffe0000` before PE guest entry with status `c0000017`. The retained `build/local/arm64-runtime-20261002/address-space.json` directly records task minimum **4298129408**, range-create **46 / KERN_NOT_SUPPORTED**, Mach allocate/map **1 / KERN_INVALID_ADDRESS**, and mmap **errno 12 / ENOMEM**. These are measured host API outcomes, not an inference that a smaller reserve or emulator toggle works.

`build/local/arm64-owned-package-abi-20261002/result.json` records the accepted staged-runtime observation using the retained `arm64-foundation-20261002/integration-reviewed/arm64` ABI pair: **BLOCKED**, `runtime_executed=true`, `guest_executed=false`, `host_no_rosetta_observed=true`, `strict_no_rosetta_proven=false`. It records 864 loaded-native image rows across the observed process chain, not 864 distinct libraries, plus the manifest hash, explicit environment, execution/server logs and per-PID captures. Required images are checked for each actual process; `proc_translated=0` includes both native dispatcher and real loader in the caller PID and the separate foreground server. Host-only no-Rosetta observation is not a Rosetta-free PE/FEX chain. The normal owned-wrapper `--version` package smoke is distinct from the observer's direct native loader route, which records explicit isolated owned policy and manifest identity because SIP shell execution removes the DYLD observer. G5/G6/G7/G8 stay open. The user deferred the execution layer; no emulator or low-address compatibility subsystem was added in this packaging work.

The independent **uninstrumented normal owned-wrapper ABI attempt** also fails before guest entry with the same fixed shared-page error/exit 1 (`arm64-owned-package-cut-20261002/logs/wrapper-abi-attempt.log`). Thus the observer route is not the source of that blocker. Its disposable prefix and the observer prefix were cleaned after scoped Wine shutdown; the accepted package remains. The separately compiled GPU ABI fixture is not a GPU execution result.

## Evidence and dependencies inspected before downloads

**Initial dependency/source investigation, 2026-10-02.** These observations predate the completed SDK/plugin follow-up and preserve the reasons for its bounded scope. They are not a current implementation backlog. Vendor availability statements below are dated observations, not a fresh network check.

Starting repository observations:

- Root `meson.build` initially derived `native_compiler_arch` from the Windows `host_machine.cpu_family()` and derived the Unix install directory from that same family. This coupled two independent axes; explicit native architecture/layout selection subsequently separated them while retaining x64/WoW64 defaults.
- `src/airconv/darwin/meson.build` builds the owned compiler against a selected native LLVM prefix. Its native dependencies must match the requested Mach-O architecture.
- `src/winemetal/unix/meson.build` links actual Wine `ntdll.so` and `winemac.so`. Compiling Metal handler objects without these runtime dependencies is not proof of a loadable Wine Unix bridge.
- `scripts/stage-owned-graphics.py` initially hardcodes x86_64 Windows/Unix paths, native slice checks and wrapper search paths. The implemented runner/provenance now select layout from explicit architecture contracts and audited artifacts.
- Local `toolchains/llvm-15-arm64/` exists alongside x86_64 `toolchains/llvm-15/`. Its ignored `build-owned-air.sh` has a hand-maintained old source list and references obsolete probes; it is not the reproducible build contract.
- The installed llvm-mingw clang 22.1.8 executable has actual arm64 and x86_64 Mach-O slices; its default invocation reports `arm64-apple-darwin27.0.0`. The supervisor process check reported `sysctl.proc_translated=0`. Neither establishes the architecture of a subsequently launched Wine process.
- `toolchains/wine-sdk` and `toolchains/wine-runtime` are available locally. Their native architectures, dependencies and provenance must be audited before using them for a native claim. Do not download a replacement merely because an existing path is named x64 or ARM64.

Official sources and their evidentiary roles:

1. [Microsoft ARM64EC](https://learn.microsoft.com/en-us/windows/arm/arm64ec), particularly [identifying ARM64EC binaries and apps](https://learn.microsoft.com/en-us/windows/arm/arm64ec#identifying-arm64ec-binaries-and-apps): interoperation contract and final PE identification.
2. [FEX ARM64EC development documentation](https://wiki.fex-emu.com/index.php/Development:ARM64EC): actual Wine/FEX ARM64EC build/integration expectations. Linux instructions are not a Darwin runtime recipe.
3. [CodeWeavers, July 31 2026: The Right to Bear ARM64 on Mac](https://www.codeweavers.com/blog/mjohnson/2026/7/31/crossover-preview-the-right-to-bear-arm64-on-mac): vendor announcement of custom macOS FEX and ARM64 DXMT. This invalidates a blanket claim that macOS integration is impossible, but is not a source release/revision or permission to install the product.
4. CodeWeavers' public source page at investigation time advertised stable **26.3.0** (supervisor-verified). No specific macOS ARM64 Preview FEX fork/commit has yet been verified. Do not assume the stable archive includes that port or require paid CrossOver.
5. Upstream Wine already contains Apple ARM64 signal support and distinguishes Windows 4 KiB pages from `host_page_size`. Pin and inspect the relevant source paths before deciding a specific incompatibility. Host page size alone is not a reason to declare Wine impossible.
6. Upstream FEX's Darwin CMake rejection is evidence about a native-Darwin configuration of that revision, not proof against its separate Windows PE/ARM64EC build route or CodeWeavers' macOS port. The upstream PE route still needs an explicit Darwin Wine integration recipe and execution proof; it is not ruled out and was not validated here.

Source retrieval is allowed only from verified official upstream/vendor locations. Record URL, exact revision/archive digest, license and dependency provenance. A public announcement without available source remains an explicit integration gate. It does not block independent compiler, PE build, staging or ABI fixture work.

### Verified vendor-source investigation

The July 31 announcement was read in the browser at its exact official URL. It describes a custom macOS FEX port, ARM64 DXMT, universal Intel/ARM64 Wine builds, a macOS 26.5+ native requirement, and Intel/Rosetta fallback on older systems. It describes Preview/new-bottle testing and D3D12 as forthcoming there; none of that is a source revision or proof of this repository’s native D3D12 path.

The official source page links [crossover-sources-26.3.0.tar.gz](https://media.codeweavers.com/pub/crossover/source/crossover-sources-26.3.0.tar.gz). Its HTTP metadata reports 149,054,023 bytes and Last-Modified 2026-07-21, before the July 31 announcement. That archive was not assumed to contain the Preview port, downloaded for execution, or substituted for a verified FEX revision. The official [Preview Center](https://www.codeweavers.com/preview) is a vendor lead, not an authorization to acquire/install a product or accept a license. Public directory access returned 403 and no matching public vendor fork/commit was established. **G0 for the macOS Preview FEX port remains blocked on an identifiable official source revision and build/integration contract.**

Initial local artifact inspection found x86_64 Mach-O `wine.real` and SDK `ntdll.so`, but ARM64 `wineserver`. This mixed native chain does not meet no-Rosetta acceptance. There was no installed CrossOver application under the inspected system/user Applications directories. No installed runtime was modified or launched to infer native compatibility.

Local Wine source was inspected without downloading a replacement: `../zzz-wine-yaagl-pr-runtime` at `c2f86161d543acda5be91ce60be881b3efea9658` contains Apple branches in `dlls/ntdll/unix/signal_arm64.c` and Windows page-size versus dynamic ARM64 host-page handling in `dlls/ntdll/unix/virtual.c` (`host_page_size = sysconf(...)`). These are the user’s Wine forks, not a verified Preview FEX source release. Their existence rules out treating a missing native local SDK or a 16 KiB host page as a blanket architecture impossibility. Upstream FEX was pinned for inspection at `0df84d3844bcdb87bb7d3f5b8fb0959cd009c038`; its supported build conditions must not be silently reinterpreted as a vendor-port recipe.

Official Wine was independently pinned at [`13625e51fd62639c8a60d7c2774ae70d93eaf446`](https://github.com/wine-mirror/wine/tree/13625e51fd62639c8a60d7c2774ae70d93eaf446). Its [ARM64 signal implementation](https://github.com/wine-mirror/wine/blob/13625e51fd62639c8a60d7c2774ae70d93eaf446/dlls/ntdll/unix/signal_arm64.c) contains Apple `uc_mcontext->__ss` register and exception-state handling. Its [virtual-memory implementation](https://github.com/wine-mirror/wine/blob/13625e51fd62639c8a60d7c2774ae70d93eaf446/dlls/ntdll/unix/virtual.c) defines Windows page shift 12 separately from dynamic ARM64 `host_page_size`. These are concrete source capabilities, not a built or validated Darwin Wine/FEX configuration. The inspected local Wine `tools/makedep.c` also distinguishes `arm64ec-windows` import libraries from `aarch64-windows`; the PE SDK/install axis must preserve that distinction independently from `aarch64-unix`.

## Three bounded implementation slices

Original foundation plan, 2026-10-02, retained to explain the delivered build/tooling/static-ABI scope. These requirements are not three wholly unimplemented tasks: latest completion and remaining execution work are listed at the top.

### Foundation 1 — reproducible native and Windows build targets

1. Make the native Mach-O architecture independent from the Windows ABI. Preserve x64/WoW64 defaults and support explicit ARM64-native builds.
2. Use the current Meson source lists for AIR and Metal domains, not a second stale source inventory. Validate the selected LLVM libraries and Wine SDK architecture before link.
3. Provide plain ARM64 and ARM64EC/ARM64X translator target recipes. Build D3D12, DXGI and Windows/native bridge targets where dependencies permit; report missing native Wine SDK dependencies precisely instead of treating object compilation as a final library.
4. Audit actual output machines, PE load-config/native ranges, dynamic dependencies/rpaths and import requirements.
5. Run a native owned compiler-to-Metal exact-readback probe with native execution identity and Metal validation. Preserve compile, link, dependency, validation and GPU evidence.

### Foundation 2 — architecture-aware staging and runner

1. Represent native architecture, Windows module ABI and optional x86 WoW64 modules explicitly in package manifests. Record the actual application architecture separately in execution results; a staged package does not prove that any guest executed. Preserve `arm64ec-windows` for pure EC imports/modules, `aarch64-windows` for plain ARM64/ARM64X modules, and `aarch64-unix` for native ARM64 independently.
2. Read existing schema-1 manifests as their documented x64 baseline; do not reinterpret historical proofs as native ARM64.
3. Derive module paths and wrapper environment from audited layout. Reject conflicting artifacts, unsupported combinations, missing native slices/dependencies and ambiguous ARM64EC classification.
4. Keep fresh destination/prefix requirements, owned-backend verification, compiler provenance and validation failure handling.
5. Preserve existing x64/WoW64 regression behavior through targeted tests and actual isolated staging/runner execution. Synthetic fixture files in unit tests are not runtime proof.

### Foundation 3 — actual integration preflight and ABI smoke

1. Identify the official macOS FEX source/revision and compatible Wine integration, or report that exact input as unavailable. Inspect pinned Wine signal/page-size code rather than relying on architecture folklore.
2. Build real guest and native ABI fixtures: integer/floating-point arguments, fixed-width structure round-trip, pointer ownership, reverse callback/COM calls and module identities. Audit final ARM64EC code metadata.
3. Preflight must reject the currently translated/x64 native chain for strict no-Rosetta acceptance. File names, an arm64 wineserver, or an ARM64 bridge object alone cannot satisfy it.
4. With a verified native runtime, exercise an x64 guest through FEX into ARM64EC/native code and back. Record actual process/module architectures and translation state. Test x86 separately; never silently fall back to Rosetta.
5. If the runtime source/SDK is unavailable, return an explicit blocked/non-passing execution result while still producing useful compiled fixtures and artifact/preflight evidence. No fake emulator, mock ABI echo, or empty success path.
