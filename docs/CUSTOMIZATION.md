# Configuration

Current D3D12 support and limitations are tracked in the [implementation status](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md).
Configuration changes do not add missing API/compiler features or establish compatibility with a game.

## File, environment, and precedence

Options use `prefix.name = value` syntax. The loader is [config.cpp](../src/util/config/config.cpp).

- `DXMT_CONFIG_FILE=/path/to/dxmt.conf` selects the file. If unset or empty, the filename is `dxmt.conf`, relative to the process working directory.
- `DXMT_CONFIG="d3d12.preferredMaxFrameRate=30;dxgi.forceSDR=True"` supplies inline assignments separated by `;`.
- Values containing spaces require double quotes inside the assignment, for example `DXMT_CONFIG='dxgi.customDeviceDesc="NVIDIA GeForce RTX 5060"'`. Without those inner quotes the value stops at the first whitespace. Prefer the existing application profile over a redundant adapter override; a truncated adapter name hid ZZZ's DLSS controls in an isolated diagnostic run.
- For the same key, precedence is **built-in application profile < config file < `DXMT_CONFIG`**. Environment assignments are parsed after the file; built-in profiles fill keys not already supplied by the user.
- Settings before the first section apply generally. An `[App.exe]` section is active only when its name matches the executable name; a nonmatching section is inactive until another matching section is selected.
- Configuration is initialized through a **module-local** singleton, not a process-global configuration service shared by all DLLs. Set the file/environment before starting the application; this is not a live-reload API.

[dxmt.conf](../dxmt.conf) contains existing example keys. Its `d3d11.*` settings are not D3D12 settings.
For a D3D12 presentation-rate preference, use:

```ini
d3d12.preferredMaxFrameRate = 30
```

Boolean values accept `true`/`false` case-insensitively. A key only has an effect in the component that reads it.

## Scope of current keys

| Keys | Consumer and boundary |
|---|---|
| `d3d12.preferredMaxFrameRate` | D3D12 swapchain presentation-rate preference; currently the D3D12-specific configuration key |
| `dxgi.customVendorId`, `dxgi.customDeviceId`, `dxgi.customDeviceDesc` | Shared DXGI adapter identity reporting; not changes to hardware capability |
| `dxgi.forceSDR` | Shared DXGI output reporting; not proof of physical color/HDR correctness |
| `d3d11.preferredMaxFrameRate` | D3D11 swapchain presentation-rate preference |
| `d3d11.metalSpatialUpscaleFactor` | D3D11 spatial swapchain scaling only |
| `d3d11.ignoreMapFlagNoWait`, `d3d11.sampleNaNToZero`, `d3d11.defuseFma`, `d3d11.maxFeatureLevel`, `d3d11.maxTessFactor` | D3D11-only behavior |
| `dxgi.handleAltTab` | D3D11 swapchain only, despite the `dxgi.` prefix |
| `dxmt.shaderMetalVersion` | Shared DXMT device used by D3D11; D3D12 selects its shader target internally |

The two frame-rate preferences are read independently by their respective swapchains. Prefer a divisor of the display refresh rate;
the setting alone does not establish measured frame pacing or a guaranteed application FPS.

Key consumers are [DXGI options](../src/dxgi/dxgi_options.cpp),
[D3D12 swapchain](../src/d3d12/d3d12_swapchain.cpp),
[D3D11 swapchain](../src/d3d11/d3d11_swapchain.cpp), and
[DXMT device](../src/dxmt/dxmt_device.cpp).
The D3D12 shader target is defined in [d3d12_pipeline_cache.hpp](../src/d3d12/d3d12_pipeline_cache.hpp).

## D3D11 spatial upscaling

`DXMT_METALFX_SPATIAL_SWAPCHAIN=1` requests the **D3D11** MetalFX spatial swapchain path.
`d3d11.metalSpatialUpscaleFactor` defaults to 2.0 and is clamped to at least 1.0 by that path.
This is not the D3D12 NGX/temporal integration; see the [D3D12 integration contract](../graphics-translator/NGX-INTEGRATION.ko.md).

## Diagnostics are separate from configuration keys

- `OWNED_GRAPHICS_TRACE=1` enables D3D12 lifecycle tracing. `OWNED_GRAPHICS_TRACE_LIMIT` controls the bounded trace limit (default 8192, maximum 1,000,000); see [owned_trace.hpp](../src/d3d12/owned_trace.hpp).
- `DXMT_LOG_PATH` and `DXMT_LOG_LEVEL` configure logging, not feature support.
- Metal API/Shader Validation for isolated GPU fixtures is selected through the [test runner](../graphics-translator/SMOKE-TEST.ko.md).
- The UMA advertised-memory policy is implemented in DXGI; there is no documented configuration switch that turns it into dedicated physical VRAM or completes budget-change notifications.

Do not infer D3D12 support from the upstream D3D11 compatibility HUD.
