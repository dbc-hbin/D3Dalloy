# RuntimeIdentity axis - deployed runtime/config vs candidate-03 (read-only, no writes, no launches)

All paths absolute. `LIVE` = `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test`. `REPO` = `/Users/hanbinnoh/Documents/dxmt-d3d12`. `CAND` = `REPO/build/local/zzz-fex-opt-20261009` (candidate-03). No file in any tree was written or executed by me; every claim below is a read of an existing artifact or of source.

---

## 1. Current selection (on-disk) vs candidate-03

### 1.1 Which runtime the launcher selected

`LIVE/.storage/wine_tag.neustorage:1` = `wine-11.17-arm64ec-fex-dxmt-optimized-20261009` (46 B, mtime 26m before my read). The selection is materialized on disk by the launcher, traced line-by-line in `LIVE/neutralinojs.log`:

- `:538` `cp LIVE/local-runtimes/wine-11.17-arm64ec-fex-dxmt-optimized-20261009.tar.xz LIVE/wine.staged.48AGAHE36IE7TZWY.tar.xz`
- `:539` `shasum -a 256` of that archive
- `:540` `tar --strip-components=1 -C ...staging -Jxvf ...tar.xz runtime`
- `:542` `rm -rf LIVE/wineprefix`  (prefix recreated)
- `:543` `rm -rf LIVE/wine`
- `:544` `mv ...staging LIVE/wine`
- `:561` (16:29:54,266) the game launch: `WINEDEBUG=fixme-all,err-unwind,+timestamp WINEPREFIX=... MTL_HUD_ENABLED=1 WINE_ENABLE_TIMEOUT_FIX=1 WINEMSYNC=1 DXMT_LOG_PATH=.../ DXMT_CONFIG_FILE=.../dxmt.conf GST_PLUGIN_FEATURE_RANK=atdec:MAX,avdec_h264:MAX .../wine/bin/wine C:\\windows\\system32\\steam.exe Z:\\Applications\\Zenless Zone Zero\\ZenlessZoneZero.exe -use-d3d12 &> .../logs/game_1791530994266.log`

So the deployed runtime is the *packaged* runtime `REPO/build/local/yaagl-latest-20261009/package/runtime`, not the candidate-03 runtime directory. Cross-check: `REPO/build/local/yaagl-latest-20261009/package/runtime/owned-graphics-runtime.json:36` records `d3d12.dll` sha256 `f4ad23dc...` — the same value the LIVE manifest records (`LIVE/wine/owned-graphics-runtime.json:36`).

### 1.2 Module-hash comparison (aarch64-windows, from the two manifests)

| module | LIVE (deployed) sha256[:16] / size | CAND runtime sha256[:16] / size | pre-deploy LIVE backup (wine.before-fex-debt-city-20261008) |
|---|---|---|---|
| d3d12.dll | `f4ad23dc28c23bf4` / 101777408 | `08d16bbbdf4b9460` / 101777408 | `da559ad5ce9a41bf` / 100958208 |
| d3d12core.dll | `d1818c7140ba813d` / 1458176 | `95698c376689e2ea` / 1458176 | `60ca3cb2a78440ce` / 1458176 |
| dxgi.dll | `ff371a79bc6d169b` / 13697024 | `e2ddfac04e4ab865` / 13697024 | `52df7fd9b88d0f99` |
| d3d11.dll | `4e4da46a52cc21f6` / **73449472** | `1680ecbb7d05a625` / **73465856** | `fffe2497d90927af` |
| d3d10core.dll | `e8fd4a55f85c91a1` / 3981312 | `b10f47cf4715f0b5` / 3981312 | `d0916e26f1e2b379` |
| winemetal.dll | `7b648e84cd0bf02c` / 704512 | `54c041a3e79a57b0` / 704512 | `47527abac6112399` |
| nvngx.dll | `a2ba4751cd9d2197` / 7554592 | `42ce6cea03ccce82` / 7554592 | `434bacb398699b14` / 7094272 |
| nvapi64.dll | `93bf63d2d22d1e96` / 9355264 | `8576b80b90622f2f` / 9355264 | `680954142053306b` |
| amd_fidelityfx_upscaler_dx12.dll | `7551c6f760bf6359` / 1130496 | `1896e2baaefce84c` / 1130496 | `b76e7f7f34d504eb` |
| amd_fidelityfx_framegeneration_dx12.dll | `29c73ee4b3749d65` / 2473984 | `81ef791f5a0f575a` / 2473984 | `bd1f7d8b96a1a00e` |
| winemetal.so (aarch64-unix) | `615327c5e4e8fabb` / **31286224** | `028e5ff7c50efb82` / **31287232** | `f443291428cfa4f9` / 31287024 |
| xtajit64.dll (FEX) | `197c9dd2c728d520` / 5513728 | `029e2476317e265e` / 5513728 | `76c6bd49bd516b75` / 5496832 |
| xtajit.dll (FEX x86) | `1efeb821195a3fa1` / 4957184 | `168cb3fbd13558c3` / 4957184 | `0b008788acc36f94` |
| i386 d3d12.dll | `5da7569fdd98e5bb` / 46829568 | `a76e0ef7fb635ba5` / 46833664 | `3b4cf06d9cddebeb` |
| i386 nvngx.dll | `e7a51446ab99ba14` / 3749408 | `0a0703d10ad2da3e` / 3749408 | `a9552d4035a1c03e` / 3563520 |

Sources: `LIVE/wine/owned-graphics-runtime.json:35-503`; `CAND/runtime/owned-graphics-runtime.json:35-503`; `LIVE/wine.before-fex-debt-city-20261008/owned-graphics-runtime.json:35-503`. Only two modules differ in *size* (d3d11.dll +16384 B, winemetal.so +1008 B); the rest differ only in bytes.

### 1.3 The dxmt source is identical; the build inputs are not

`REPO/build/local/yaagl-latest-20261009/package/source-cut.json` (the deployed package's source snapshot) and `REPO/build/local/zzz-dlss-fix-20261009/builds/cache-04/source-cut.json` (candidate-03's dxmt build) are byte-identical on every file I sampled, including the ones that matter for this failure:

- `src/d3d12/d3d12_command_queue.cpp` `88bfca6fd1d27ce1...` (both)
- `src/d3d12/d3d12_command_list.cpp` `4493fb9e842527be...` (both)
- `src/nvngx/nvngx_d3d12.cpp` `38027f9084fce90a...` (both)
- `src/airconv/dxil_resources.cpp` `bb9e9a342fe4fdda...` (both)
- `src/winemetal/unix/winemetal_unix.c` `e641e3b4f6407668...` (both)

Concrete, verifiable build-input difference that *must* change d3d12.dll/d3d12core.dll bytes at identical source: the compiled-in core generation string.

- `REPO/meson.options:16` `option('d3d12_core_generation', type : 'string', value : '')`
- `REPO/src/d3d12/meson.build:42-50`: if the option is empty, meson runs `python3 -c 'import uuid; print(uuid.uuid4().hex)'` and writes `d3d12_core_generation.h`.
- `REPO/src/d3d12/d3d12_core.cpp:9` and `REPO/src/d3d12/d3d12_sdk.cpp:14` export it as `OwnedD3D12CoreGeneration[33]`; `d3d12_sdk.cpp:239-246` and `:292-303` **memcmp it** against the loaded core's export and against the bridge's `generation` field, returning `DXGI_ERROR_UNSUPPORTED`/`E_NOINTERFACE` on mismatch.
- LIVE manifest records `generation` `9928d33fcad1469fb2243a0dda4e5d9c` (aarch64) / `b70793fa966c43ed924ddde7eec1b5cc` (i386) — `LIVE/wine/owned-graphics-runtime.json:537,541`. CAND records `e77414b04e8c468a9bca4f42d8cbe940` / `137faf1523bb4d58a4c5713e7fbd3d99` — `CAND/runtime/owned-graphics-runtime.json:537,541`.
- Candidate's build *pinned* the value: `REPO/build/local/zzz-dlss-fix-20261009/builds/cache-04-arm64x/result.json:54,377` pass `-Dd3d12_core_generation=e77414b04e8c468a9bca4f42d8cbe940`. The package build (`REPO/build/local/yaagl-latest-20261009/package/package-result.json` meson argv, `.../package/logs/owned-setup.log:84` "Configuring d3d12_core_generation.h") passes **no** such flag, so the deployed pair got a random uuid.

Consequence for the axis: the deployed and candidate D3D12 modules are *not the same binaries* even at identical dxmt source, and the pairing contract is self-consistent only inside each runtime (deployed d3d12.dll + deployed d3d12core.dll both carry `9928d33f...`). There is no evidence anywhere that a generation mismatch occurred at runtime; the difference is a build-identity difference, and the live logs show `D3D12_FEATURE_LEVEL_11_1` selection and successful D3D12 module execution (see 5.2), i.e. the pairing check did not reject.

### 1.4 FEX: deployed binary differs from the measured candidate binary

- Deployed FEX provenance: `LIVE/wine/owned-graphics-runtime.json:597-640` -> `build_result_sha256 db130456...`, `module lib/wine/aarch64-windows/xtajit64.dll`, `sha256 197c9dd2...`. That build result is `REPO/../FEX-darwin/build/local/yaagl-latest-20261009/fex-result.json` (its `:476` records the same `197c9dd2...`), whose `logs/source-head.log` = `206d61d076b81fb0cc506e2535965217124b4340` and `logs/source-status.log` is empty.
- Candidate-03 FEX provenance: `CAND/analysis-config.json` -> `dll = /Users/hanbinnoh/Documents/FEX-darwin/build/local/fex-ir-tail-20261009/dlls/xtajit64.dll`; `FEX-darwin/build/local/fex-ir-tail-wine-20261009/result.json:535-538` records `fex_result_sha256 c05cf858...` and `xtajit64.dll 029e2476...`. That build's `logs/source-head.log` = `fbd54488...` with `logs/source-status.log` = `M FEXCore/Source/Interface/IR/IREmitter.cpp` + `M .../IREmitter.h` (i.e. built from uncommitted IR-tail edits).
- `FEX-darwin/.git/logs/HEAD` tail: `fbd54488 -> 206d61d0  commit: Darwin: Avoid repeated IR code-block tail scans`. So the deployed build's recorded HEAD **is** the IR-tail commit, and both `fex-result.json` files list `FEXCore/Source/Interface/IR/IREmitter.h` with the identical hash `a9400ef8c3b235df769d0e81dc51182f0980ac2ee3b25b550d6158b7a9ede7c7` (`yaagl-latest-20261009/fex-result.json:781-782`, `fex-ir-tail-20261009/fex-result.json:781-782`).

Counterclaim falsified: "the deployed runtime lacks the FEX IR-tail optimization that candidate-03 measured". At source-input level the deployed FEX contains the same change. What is **not** established: that deployed `197c9dd2` is byte- or behavior-equivalent to the measured `029e2476` (same size 5,513,728 B, different hash). Both builds also list `source_commit 9fbdc00b` (FEX-2609.1 upstream pin) plus the same nine Darwin patches. The hash difference is an open data gap (see 7).

---

## 2. Environment flags actually in effect

### 2.1 The wrapper is the launch hop, and it rewrites the environment

`LIVE/wine/bin/wine:1-78` (3.3 KB `#!/bin/sh` shim; `wine.real` is the ELF/Mach-O). What it does that is provable from the live logs:

- `:9` `unset CX_ACTIVE_GRAPHICS_BACKEND CX_APPLEGPTK_LIBD3DSHARED_PATH D3DM_MTL4 D3DM_ENABLE_METALFX D3DM_SUPPORT_DXR D3DM_VENDOR_ID D3DM_DEVICE_ID D3DM_DEVICE_DESCRIPTION GPTK_SOURCE ... DYLD_INSERT_LIBRARIES DXVK_CONFIG_FILE ... DXMT_ENABLE_NVEXT`
- `:18` `export WINEDLLPATH="$wine_root/lib/wine/aarch64-windows:$wine_root/lib/wine/i386-windows:$wine_root/lib/wine/aarch64-unix"`
- `:36` `export WINEDLLOVERRIDES="d3d12=b;dxgi=b;d3d11=b;d3d10core=b;winemetal=b;d3d12core=n;nvngx=b;nvapi64=b;nvapi=b;amd_fidelityfx_upscaler_dx12=b;amd_fidelityfx_framegeneration_dx12=b"` (all-builtin; `d3d12core=n` native)
- `:38` `export DXMT_ENABLE_NVEXT=1`
- `:42` `export FEX_HIDEHYPERVISORBIT=1`
- `:53-60` re-enables `DYLD_PRINT_LIBRARIES` only when `OWNED_GRAPHICS_DYLD_AUDIT=1`, and injects `DYLD_INSERT_LIBRARIES` only when `OWNED_FG_OBSERVER` is set
- `:77` `exec /usr/bin/arch -arm64 "$real_wine" "$@"`

Loaded-time proof that the wrapper's environment reached the modules (live run):

- `LIVE/logs/game_1791530994266.log:6,64,65,71-75,6334` `info:  Vendor extension enabled: NVEXT` — emitted only on the `DXMT_ENABLE_NVEXT == "1"` branch of `REPO/src/dxgi/dxgi.cpp:32-42`.
- `:7-10` `Found built-in config: dxgi.customDeviceId = 2d05 / dxgi.customVendorId = 10de / dxgi.customDeviceDesc = NVIDIA GeForce RTX 5060` — that profile is selected *by the same env var* at `REPO/src/util/config/config.cpp:52-58`.
- `:5` `info:  Failed to set Metal cache path, fallback to system default` — `REPO/src/dxgi/dxgi.cpp:50-57`.
- `:22-38` `D3D12 shader probe[N]: stage=CS bytes=... sha1=... chunk=DXIL hr=0` — `REPO/src/d3d12/d3d12_pipeline_graphics.cpp:51-55`.
- `:78+` `err:   Failed to compile ps shader: dxil: shader resource  is not bound by the root signature` — string built in `REPO/src/airconv/dxil_resources.cpp:1516-1518`, i.e. the owned airconv **inside the deployed `winemetal.so`** executed; surfaced by `REPO/src/d3d12/d3d12_pipeline_graphics.cpp:1852-1854`.

Those five markers prove *which code ran in-process* (owned dxgi, owned d3d12, owned winemetal/airconv, the NVEXT branch), and they are the strongest loaded-time evidence available for this run. They do **not** carry file hashes.

### 2.2 Flags present in candidate-03's launcher but absent from the live launch

`CAND/launcher.c` (the research launcher used for candidate-03):

- `:38` `setenv("WINEDEBUG", "fixme-all,err-unwind,+timestamp,+pid,+loaddll", 1)`
- `:43-44` `DXMT_LOG_PATH=<run>` (per-run dir), `DXMT_LOG_LEVEL=info`
- `:47-53` `FEX_DISKCACHE` (=1 only for `on-*` runs), `FEX_DISKCACHEPATH=C:\\fex-research\\`, `FEX_DISKCACHEVALIDATION=0`, `FEX_DISKCACHEPRUNESTALEENTRIES=0`, `FEX_GLOBALJITNAMING=1`, `FEX_BLOCKJITNAMING=0`, `FEX_LIBRARYJITNAMING=0`
- `:54-55` `OWNED_GRAPHICS_TRACE=1`, `OWNED_GRAPHICS_TRACE_LIMIT=64`
- `:56-57` unset `OWNED_GRAPHICS_DYLD_AUDIT`, `OWNED_FG_OBSERVER`
- `:58-59` `... ZenlessZoneZero.exe -use-d3d12 -streamline-d3d12-diag -screen-fullscreen 0 -screen-width 1280 -screen-height 720`

Live launch (`LIVE/neutralinojs.log:561`) sets only `WINEDEBUG=fixme-all,err-unwind,+timestamp` (no `+loaddll`), `WINEPREFIX`, `MTL_HUD_ENABLED=1`, `WINE_ENABLE_TIMEOUT_FIX=1`, `WINEMSYNC=1`, `DXMT_LOG_PATH=LIVE/`, `DXMT_CONFIG_FILE=LIVE/dxmt.conf`, `GST_PLUGIN_FEATURE_RANK=...`, and passes only `-use-d3d12`.

Measured consequence (literal counts, my own greps):

- `LIVE/logs/game_1791530994266.log` and `LIVE/ZenlessZoneZero_d3d12.log`: **zero** lines matching `OWNED_GRAPHICS`, **zero** matching `loaddll`, **zero** matching `trace:`. `OWNED_GRAPHICS_TRACE` is read only as an exact `"1"` at `REPO/src/d3d12/owned_trace.hpp:19-21` and `REPO/src/winemetal/unix/winemetal_unix.c:5573-5576`; it was never set for this launch.
- `CAND/candidate-03/game.log` has `trace:loaddll:build_module Loaded L"C:\\windows\\system32\\xtajit64.dll"` (e.g. `:148,224,261,1588,1669,1785,2015,2125,2361,8362,15807`) and `OWNED_GRAPHICS_TRACE {…}` / `OWNED_GRAPHICS_NATIVE_PRESENT_DECISION …` (e.g. `:336-338`, `:1742-1745`, `:3117-3178`).

Therefore: the absence of NGX parse/evaluate diagnostics and of present-decision lines in the live run is an **instrumentation-coverage fact, not a runtime fact**. The live run also lacks `-streamline-d3d12-diag`, which is why its `Player.log` prints `[streamline] init flags=0x89 frameBasedTagging=1 disableCLStateTracking=1 enableCLStateTrackingArg=0 diag=0` (live `Player.log:8`) while candidate-03 prints `diag=1` (`CAND/candidate-03/Player.log:8`) and then emits `[SL-D3D12-DIAG] slSetTagForFrame/slEvaluateFeature` per frame (`CAND/candidate-03/Player.log:300+`, 19,913 lines).

### 2.3 Config file that does not exist

`DXMT_CONFIG_FILE` points at `LIVE/dxmt.conf`, which **does not exist** (directory listing of `LIVE/` contains no `.conf`; only the nine `*.log` files). `REPO/src/util/config/config.cpp:365-390` logs `Found config file: <path>` when it opens one; the live logs contain only `Found built-in config:` and never `Found config file:` or `Found config env:`. So the live run's dxmt config is exactly the built-in profile, no file/env overrides — the same conclusion candidate-03's own QA correction reached (`CAND/result.json` `qa_environment_correction`).

---

## 3. Device identity

- Reported adapter identity comes from the built-in ZZZ profile (`REPO/src/util/config/config.cpp:52-58`): `NVIDIA GeForce RTX 5060`, vendor `10de`, device `2d05`, selected when `DXMT_ENABLE_NVEXT == "1"`; the alternate branch is `AMD Radeon RX 9070 / 1002 / 7550`. Consumers: `REPO/src/dxgi/dxgi_adapter.cpp:154-161` (description) and `REPO/src/nvapi/nvapi.cpp:58-59, 693-696` (NVAPI adapter string defaults to the same key).
- Live evidence: `LIVE/logs/game_1791530994266.log:7-10, 12-15, 41-44, 48-51` (four `Found built-in config` blocks, all `2d05/10de/RTX 5060`); `LIVE/wineprefix/drive_c/users/hanbinnoh/AppData/LocalLow/miHoYo/ZenlessZoneZero/Player.log:22` `Renderer:        NVIDIA GeForce RTX 5060 (ID=0x2d05)`.
- Both runs report `Vendor:` **blank** (`Player.log:23`), `VRAM: 0 MB`, `App VRAM Budget: 53084 MB`, `Tile Based Renderer`, `Unified Memory Architecture` (`Player.log:24-27`). The blank vendor field is identical in candidate-03 (`CAND/candidate-03/Player.log:22-27`), so it is not a live-run regression.
- D3DMetal/GPTK identity is explicitly *not* in play: the wrapper unsets `D3DM_VENDOR_ID/D3DM_DEVICE_ID/D3DM_DEVICE_DESCRIPTION/CX_APPLEGPTK_*` (`LIVE/wine/bin/wine:9`), and the manifest records `gptk_backend.selected = false` with reason "the Wine runtime may contain unrelated consumers; selection is blocked by the owned wrapper" (`LIVE/wine/owned-graphics-runtime.json:574-578`). The staging script also strips those names (`REPO/scripts/stage-owned-graphics.py:117-121`).
- Wine-side device/host identity present in the prefix: `wine_netbiosname` = `DESKTOP-FYB35S3`, `wine_state` = `ready`, `config_use_d3d12` = `true`, `config_retina` = `true`, `config_metalHud` = `true`, `config_block_net` = `false`, `config_steam_patch` = `true`, `config_timeout_fix` = `true`, `uiLocale` = `ko_kr`, `game_install_dir` = `/Applications/Zenless Zone Zero` (`LIVE/.storage/*.neustorage`). `winedrv_config.bat:2-3` sets `RetinaMode=y`, `LeftCommandIsCtrl=n`, and the prefix confirms it (`LIVE/wineprefix/user.reg:4569-4572`).
- Guest/OS identity as seen by the game: `device_model Mac17,8 (Apple Inc.)`, `system_version Windows 10 (10.0.19045) 64bit`, `platform PC` (live `Player.log:107`). `config.ini` (`/Applications/Zenless Zone Zero/config.ini`) is `game_version=3.2.0, channel=1, sub_channel=0, cps=mihoyo`; the game nevertheless logs `config.ini is not exist` (it looks elsewhere) — same in candidate-03.

---

## 4. Game settings

### 4.1 What is on disk in the live prefix

`LIVE/wineprefix/user.reg:1012-1018` (Unity ScreenManager, in `[Software\\miHoYo\\ZenlessZoneZero]`):

- `Screenmanager Fullscreen mode_h3630240806 = 1` (fullscreen)
- `Screenmanager Resolution Width_h182942802 = 0x00000f00 = 3840`
- `Screenmanager Resolution Height_h2627697771 = 0x00000870 = 2160`
- `Screenmanager Resolution Use Native_h1405027254 = 1`, `...Use Native Default_h1405981789 = 1`
- defaults: `Width Default_h680557497 = 0x780 = 1920`, `Height Default_h1380706816 = 0x438 = 1080`

There is **no** DLSS/upscaler/frame-generation/ray-tracing key anywhere in the live `user.reg` (searches for `DLSS`, `upscal`, `SuperResolution`, `GRAPHICS`, `GraphicsSettings`, `FrameGen`, `RayTracing`, `Vulkan` all return nothing except unrelated Internet-Explorer/ScreenManager hits). ZZZ keeps its graphics settings outside this key; the live prefix's per-title data I could reach is `LIVE/wineprefix/drive_c/users/hanbinnoh/AppData/LocalLow/miHoYo/ZenlessZoneZero/{Player.log,output_log.txt,logs/MiHoYoSDK.log}` — no settings file there.

### 4.2 Output resolution: what is and is not evidenced

- The live launch passes **no** `-screen-width/-screen-height` (`LIVE/neutralinojs.log:561`), unlike candidate-03 which forced 1280x720 (`CAND/launcher.c:59`). So the live run took its window/resolution from the prefix registry above (3840x2160, fullscreen, use-native).
- The live `Player.log:303-306` lines `wm_setEnable: … {"enable":false,…,"width":3840,"height":2160}`, `wm_setWidth: set to:3840`, `wm_setHeight: set to:2160` are **watermark API parameters** (the in-game watermark overlay), and I do not treat them as render/output-resolution proof. The only *hard* resolution evidence for the live run would be an NGX parse line or a swapchain size line; neither exists because tracing is off.
- For comparison, candidate-03 *does* have it: `CAND/candidate-03/game.log:3106-3117` `ngx_d3d12_create_parse … input_width:640,input_height:360,output_width:1280,output_height:720,flags:74`, then `ngx_d3d12_evaluate_parse … input_width:624,input_height:360`, plus the Metal HUD label line at `:3117`. The candidate's `dlss-settings-confirmed.png` and `city-dlss-*.png` are its settings evidence (`CAND/result.json` C2). The live run has **no** settings screenshot, no NGX parse, and no equivalent settings confirmation.
- Both runs load the owned upscaler/framegeneration providers and report their version strings, which is a real (if narrow) settings-relevant fact: live `Player.log:9-10` `[ffx] Upscaler Version: MetalFX (DXMT owned temporal upscaler; dxmt-owned-fsr-metalfx-v1)` and `[ffx] FrameGen Version: MetalFX frame generation (dxmt-owned-fsr-framegeneration-metalfx-v1)` — those exact strings are constants in `REPO/src/fsr/fsr.cpp:15` and `REPO/src/fsrfg/fsrfg.cpp:14`, so the owned upscaler and FG modules were queried in the live process.
- Both runs also log `[streamline] error: Result::eErrorOSDisabledHWS` / `eErrorFeatureFailedToLoad` / `DLSSG is not requested or failed to load.` / `[streamline] DLSS version: 310.6.0` (live `Player.log:16-19, 51-52, 593-594`; candidate `Player.log` same block). Those strings are the game's own Streamline messages; `310.6.0` is not produced by any string in `REPO/src` (grep for `310\.6` finds nothing), so it is not owned-module output.
- The single MetalFX mention in the live *log sinks* is the Metal HUD metric label: `LIVE/logs/game_1791530994266.log:76` (16:30:50.850) `[libMTLHud] Metric com.apple.hud-label.metalfx.timing already exist`, enabled by `MTL_HUD_ENABLED=1` (`neutralinojs.log:561`) + `config_metalHud=true`. Useful only as a temporal anchor: it sits at line 76, immediately before the compile-failure flood that starts at line 78. It does not by itself prove a DLSS-SR evaluate call (hypothesis: the label is created when a MetalFX scaler object is instantiated; both runs create one).

---

## 5. Manifest claim vs loaded-module proof (the explicit split)

### 5.1 Manifest / on-disk claim (strong, already executed by the lead)

- The manifest is a *staged artifact record*: `LIVE/wine/owned-graphics-runtime.json` (schema 2, 767 lines) lists per-module sha256 + size + PE machine/ABI/code ranges, `agility_sdk` (`version 618`, self-owned, per-module `generation`), `native_compiler` (`lib/wine/aarch64-unix/winemetal.so`, `integration`: "the DXIL compiler is compiled from the owned patch series into airconv and linked into this module"), `airconv_runtime` ("statically linked into winemetal.so; no separate runtime executable"), `ngx_signing` (authenticode, subject `DXMT Owned NGX Runtime`, cert `share/dxmt/ngx-runtime.cer`), `fex`/`fex_x86` (build result hash + module hash + nine patch hashes), and `gptk_backend.selected=false`.
- Provenance chain of the deployed tree is traced end to end: `wine_tag` -> `local-runtimes/<tag>.tar.xz` -> `cp/shasum/tar/mv` (`neutralinojs.log:538-544`) -> `LIVE/wine`. The package manifest equals the live manifest for the modules I compared (`f4ad23dc…` d3d12.dll in both).
- The lead's executed readback (`doc/ulw-research/20261009-075042/verify-identity-timings.md`, cited by the parent message) shows the *current on-disk* aarch64 d3d12/nvngx/winemetal and i386 d3d12/nvngx all match manifest SHA256. I did not re-run it; I cite it as the lead's executed evidence and note my own reads agree on every hash I sampled (`LIVE/wine/owned-graphics-runtime.json` values match the module sizes shown in the directory listings of `LIVE/wine/lib/wine/aarch64-windows` and `.../aarch64-unix`).

### 5.2 Loaded-module proof (weak, and this is the honest limit)

- No `+loaddll` in the live `WINEDEBUG` (`neutralinojs.log:561`) and no `DYLD_PRINT_LIBRARIES` (wrapper gates it behind `OWNED_GRAPHICS_DYLD_AUDIT`, `LIVE/wine/bin/wine:53-60`; not set). Therefore **no loaded path, image base, or hash for any module exists in the live run**. The live sinks contain zero `loaddll` lines.
- What *is* proven loaded, by in-process behavior only: owned `dxgi.dll` (NVEXT branch, Metal-cache-path failure, built-in profile), owned `d3d12.dll` (shader-probe lines, PS/VS compile-failure lines with the `ps shader`/`vs shader` prefixes from `d3d12_pipeline_graphics.cpp:1713-1714,1852-1854`), owned `winemetal.so`/airconv (the `dxil: … is not bound by the root signature` text), owned `amd_fidelityfx_upscaler_dx12.dll`/`..._framegeneration_dx12.dll` (the two `[ffx] … Version:` strings).
- `nvngx.dll` (the NGX boundary) has **no** positive loaded-evidence marker in the live run: every NGX trace is gated by `OWNED_GRAPHICS_TRACE` (`REPO/src/nvngx/nvngx_d3d12.cpp:932` and `:1018`), and there is no non-traced NGX log line. The `ngx_signing` block in the manifest proves the *packaged* `nvngx.dll` was signed/enrolled, not that it was loaded and queried in this session. The game's own `[streamline] DLSS version: 310.6.0` is not owned-module output. So: NGX module presence is an on-disk claim; NGX *invocation* in the latest run is unproven either way.
- `xtajit64.dll` (FEX) likewise has no live marker: no `+loaddll`, no FEX banner, no `FEX_*` env, and the only x86-related text in the live log is Wine's own `err:vulkan:vulkan_init_once` and `warn: nvapi: unsupported shader extension opcode 71`. The live game binary is x86_64 (`Z:\\Applications\\Zenless Zone Zero\\ZenlessZoneZero.exe`), so FEX must be the execution path, but the *loaded FEX hash* is inferred, not observed.
- The one loaded-path datum that does exist is indirect and weak: the msync bootstrap token `wine-8025fc3-msync-v2` (live `game_1791530994266.log:1`), which is `snprintf("wine-%" PRIxMAX "-msync-v%u", st.st_ino(config_dir_fd), MSYNC_SHM_WIRE_VERSION)` in `Wine-darwin/build/local/zzz-recovery-20261009/wine-source/server/msync.c:941-944,978`. It proves which config directory (the recreated prefix) the server used, not which module files were mapped. Note the token changed from `wine-7d84ccc-msync-v2` (all runs through 16h ago) to `wine-8025fc3-msync-v2` for this run — consistent with the 16:25 prefix recreation, and therefore *not* usable as a module-identity fingerprint.

---

## 6. Differences from candidate-03, condensed

| axis | live (deployed, 2026-10-09 16:29:54) | candidate-03 (14:53-15:00) | evidence |
|---|---|---|---|
| runtime selection | `optimized-20261009` staged from `local-runtimes/…-optimized-20261009.tar.xz` | `CAND/runtime` (staged from `zzz-dlss-fix…/builds/cache-04/owned-install`) | `LIVE/.storage/wine_tag.neustorage:1`; `neutralinojs.log:538`; `CAND/runtime/owned-graphics-runtime.json:11` |
| dxmt source | HEAD `f04bd8a` snapshot | same snapshot, byte-identical source-cut | `package/source-cut.json` vs `cache-04/source-cut.json` |
| dxmt binaries | `f4ad23dc…` d3d12 / `d9928d33f…` core generation (random uuid) | `08d16bbb…` d3d12 / `e77414b0…` core generation (pinned) | manifests `:36,537`; `cache-04-arm64x/result.json:54` |
| FEX module | `197c9dd2…` (yaagl-latest FEX build; HEAD `206d61d0`) | `029e2476…` (ir-tail FEX build; HEAD `fbd54488` + working-tree edits) | manifests `:597-640`; `fex-result.json:476` both |
| game window | registry 3840x2160 fullscreen use-native, no CLI override | `-screen-width 1280 -screen-height 720 -screen-fullscreen 0` | `user.reg:1012-1018`; `launcher.c:59` |
| trace flags | none (`OWNED_GRAPHICS_TRACE` unset, no `+loaddll`) | `OWNED_GRAPHICS_TRACE=1`, `LIMIT=64`, `+loaddll`, `+pid` | `neutralinojs.log:561`; `launcher.c:38,54-55` |
| Streamline diag | off (`diag=0`) | on (`-streamline-d3d12-diag`, `diag=1`, per-frame `[SL-D3D12-DIAG]`) | live `Player.log:8`; `CAND/candidate-03/Player.log:8,300+` |
| FEX cache env | none | `FEX_DISKCACHE*`, `FEX_*JITNAMING` | `launcher.c:47-53` |
| dxmt config | `DXMT_CONFIG_FILE` -> nonexistent file; built-in profile only | `DXMT_CONFIG`/`FILE` explicitly unset; built-in profile only | `config.cpp:365-390`; logs show no `Found config file` |
| prefix | recreated 16:25:50 (`rm -rf wineprefix`) | disposable QA prefix under `CAND/prefix` | `neutralinojs.log:542`; `CAND/cleanup.txt` |
| shader-cache state | fresh (new prefix); cache dir now holds 11,116 entries written 07:33-07:36Z (lead-executed) | 64 captured failure pairs + 11k-entry cache | lead artifact `verify-identity-timings.md` |

---

## 7. Alternatives, counterclaims, and what is still open

Falsified by evidence:

1. "The deployed runtime is candidate-03's runtime." Falsified: every module hash differs (§1.2), the dxmt core generation differs (§1.3), and the FEX module differs (§1.4).
2. "The deployed runtime lacks the FEX IR-tail optimization candidate-03 measured." Falsified at source-input level: deployed FEX build records HEAD `206d61d0` = "Darwin: Avoid repeated IR code-block tail scans", and both builds list the identical `IREmitter.h` hash `a9400ef8…` (§1.4).
3. "The deployed stack is D3DMetal/GPTK." Falsified: wrapper unsets `D3DM_*`/`CX_APPLEGPTK_*`/`GPTK_*` (`LIVE/wine/bin/wine:9`), manifest `gptk_backend.selected=false`, and the live run's adapter identity comes from the owned NVEXT profile.
4. "The live run's missing NGX/MetalFX diagnostics prove NGX was not called." Falsified as an inference: `OWNED_GRAPHICS_TRACE` was never set for this launch and `-streamline-d3d12-diag` was not passed, so those diagnostics could not have been emitted (§2.2). Absence here is coverage, not runtime state.
5. "The live run's output resolution is 3840x2160 because of the watermark lines." Not evidence: `wm_setWidth/wm_setHeight` are watermark-overlay parameters (§4.2). The live run's render/output resolution is **not** established by any retained artifact.

Open / unresolved (with the precise gap):

- **A. Binary hash difference at identical source, no size change.** `d3d12core.dll`, `dxgi.dll`, `nvngx.dll`, `nvapi64.dll`, `d3d10core.dll`, both FSR DLLs and `winemetal.dll` all differ between LIVE and CAND at identical source-cut entries and identical sizes. Candidate explanation: DWARF debug info and/or embedded build paths (`--buildtype=debugoptimized`, `-debug:dwarf`, `-build-id` — see `cache-04-arm64x/result.json:3793-3795`) differ because the build directories differ (`package/owned-build` vs `cache-04-arm64x/pe-arm64`). I could not confirm this: `grep` for `build/local/(yaagl-latest-20261009|zzz-dlss-fix-20261009)` inside `nvngx.dll` and for the generation strings inside `d3d12.dll` returned no matches, but the grep tool searched only the first 4 MiB of those large files, so the negative is not conclusive. Needs a full-file byte scan (`strings`/`grep -a` over the whole file) to close.
- **B. Size differences not explained.** `d3d11.dll` 73,449,472 (LIVE) vs 73,465,856 (CAND) — a 16,384-byte delta, exactly one `-filealign:16384` unit; `winemetal.so` 31,286,224 vs 31,287,232 — 1,008 bytes. Neither is explained by the core-generation string (that lives only in d3d12/d3d12core). Open.
- **C. FEX binary equivalence.** `197c9dd2…` vs `029e2476…`, same size, same recorded source inputs. An incremental/stale-object build in `FEX-darwin/build/local/yaagl-latest-20261009` cannot be excluded from the retained logs. Open.
- **D. Which modules the latest session actually mapped.** Unprovable from retained artifacts (no `+loaddll`, no `DYLD_PRINT_LIBRARIES`). Behavioral markers prove four owned modules executed; `nvngx.dll` and `xtajit64.dll` are inferred from the on-disk tree only.
- **E. Live-run graphics/DLSS settings.** No settings file, screenshot, or NGX trace exists for the latest run; the only settings-shaped data is the registry ScreenManager block (§4.1).
- **F. Metal HUD `metalfx.timing` label semantics.** Whether `[libMTLHud] Metric com.apple.hud-label.metalfx.timing` implies an instantiated MetalFX temporal scaler (hypothesis, suggested by its co-location with the NGX evaluate sequence in candidate-03 `game.log:3117`) or merely reflects `MTL_HUD_ENABLED=1` is unresolved without a controlled A/B.

---

## 8. Minimal recommendations (identity/config axis only)

1. When a run's DLSS/NGX behavior must be attributable, launch it the way candidate-03 was launched: `OWNED_GRAPHICS_TRACE=1` (+`OWNED_GRAPHICS_TRACE_LIMIT`) and `WINEDEBUG=…,+loaddll`. Without both, module identity and NGX invocation are unprovable — this is the single cheapest fix and it is already implemented (`owned_trace.hpp:19-21`; the flags are only absent from the launcher's environment).
2. Pin `-Dd3d12_core_generation=<hex>` in the package build the way `build-owned-arm64x.py:365` does, so a rebuilt runtime is hash-comparable to the runtime that was measured. Today the deployed package silently randomizes it, which makes d3d12.dll/d3d12core.dll hashes non-reproducible even at identical source.
3. Remove or fix `DXMT_CONFIG_FILE=LIVE/dxmt.conf`: the launcher advertises a config file that does not exist, so every launch silently runs built-in-profile-only. Either ship the file or drop the variable (it is a no-op that misleads log readers).
4. Do not treat `wine_tag` alone as identity. It resolves to an archive name, and the archive's contents are only comparable via the manifest + module hashes; a one-line manifest diff (generation string) already separates two runtimes that share a tag lineage.

---

## 9. Evidence limits of this report

- Everything here is a read of retained files; I launched nothing, wrote nothing, and modified no prefix or registry.
- The on-disk readback of LIVE module hashes is the lead's executed artifact (`verify-identity-timings.md`), which I cite rather than re-ran.
- Greps over `LIVE/wine/lib/wine/aarch64-windows/d3d12.dll`, `nvngx.dll` and `CAND/runtime/.../d3d12.dll` are limited to the first 4 MiB of each file by the search tool; my "no match" results on those binaries are therefore **not** proof of absence and are reported as inconclusive (§7A).
- Performance/causal statements are deliberately absent from this report: this axis produced no timing data. The only temporal facts asserted are log-line order and the timestamps quoted verbatim.
- Line numbers for `LIVE/neutralinojs.log` are from the file as read at 581 lines; the file is append-only and grows with each launcher action, so anchors `:538-544, :547, :561, :562` refer to the 16:25-16:36 window of 2026-10-09.

## EXPAND

- **LEAD (high value, cheap): full-file byte scan of LIVE vs CAND `nvngx.dll` / `dxgi.dll` / `d3d12core.dll` for embedded build-path strings.** Closes §7A and would let the report state definitively whether the hash differences are pure debug-info nondeterminism or a real content difference. Method: `strings -a`/`grep -a` over the entire file (not the first 4 MiB), comparing for `yaagl-latest-20261009` vs `zzz-dlss-fix-20261009` path fragments and for the two `OWNED_D3D12_CORE_GENERATION` hex strings.
- **LEAD (medium value): diff the deployed `wine-11.17-arm64ec-fex-dxmt-optimized-20261009.tar.xz` archive member hashes against `LIVE/wine/owned-graphics-runtime.json`.** This would upgrade "manifest claim" to "archive content claim" independently of the live tree. The archive is 398 MB (exceeds the 256 MB in-memory read limit), so it needs a streaming listing (`tar -Jtvf` or `python tarfile`) rather than `read`.
- **LEAD (medium value): inspect the two d3d11.dll builds for a 16,384-byte section-size delta.** §7B is the only size anomaly besides winemetal.so; a `llvm-readobj --sections`/`--coff-load-config` comparison of the two d3d11.dll files would say whether the delta is DWARF (`.debug_*`) or a real code section.
- **LEAD (low cost, closes D partially): check whether `LIVE/wine/lib/wine/aarch64-windows/xtajit64.dll` mtime/content equals the archive member for `197c9dd2…`,** and whether any FEX configuration file (`FEX_*` defaults) is materialized in the recreated prefix (`LIVE/wineprefix/drive_c/…`) — this would tell whether the live run's FEX settings differed from candidate-03's `FEX_DISKCACHE*`/`FEX_*JITNAMING` set even though neither sets them in the log.
- **DEAD END: reading the live run's `Player.log` for DLSS/NGX settings or output resolution.** Exhausted: `diag=0`, no `[SL-D3D12-DIAG]` lines, no NGX trace, and the only resolution-shaped numbers are watermark parameters (§4.2). No further reading of that file will produce the answer; it requires a new run with the flags from recommendation 1.
- **DEAD END: using `wine_tag.neustorage` or the msync port token as a module-identity fingerprint.** Exhausted: the tag resolves to an archive name that both the pre- and post-deploy trees can claim, and the token derives from the prefix directory inode (`msync.c:941-944`), which the launcher recreates on every deploy (`neutralinojs.log:542`).
- **DEAD END: attributing `[streamline] DLSS version: 310.6.0` to an owned module.** Exhausted: no `310.6` string exists in `REPO/src`; the string is the game's own Streamline output and appears identically in the candidate run.
