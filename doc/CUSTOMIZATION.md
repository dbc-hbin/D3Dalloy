# Configuration

Configuration affects only components that read the key; it cannot add API/compiler support. See [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md) for capabilities and [dxmt.conf](../dxmt.conf) for examples.

## File, environment, and precedence

The [loader](../src/util/config/config.cpp) uses `prefix.name = value` syntax:

- `DXMT_CONFIG_FILE` selects a file; unset/empty means `dxmt.conf` in the process working directory.
- `DXMT_CONFIG="d3d12.preferredMaxFrameRate=30;dxgi.forceSDR=True"` supplies semicolon-separated assignments.
- Values with spaces need inner double quotes: `DXMT_CONFIG='dxgi.customDeviceDesc="NVIDIA GeForce RTX 5060"'`. Without them the value stops at whitespace. Prefer the built-in game profile over redundant adapter overrides.
- Precedence is **built-in application profile < file < environment**. Keys before any section are general; `[App.exe]` applies only to the matching executable name.
- Configuration is module-local and initialized before use, not a process-global live-reload service. Set inputs before launching. Booleans accept case-insensitive `true`/`false`.

## Scope of current keys

| Keys | Effect and boundary |
| --- | --- |
| `d3d12.preferredMaxFrameRate` | D3D12 presentation-rate preference, not guaranteed FPS or measured pacing |
| `d3d12.asyncPipelineCompile` | Opt-in (default False). Defers the final native link of ordinary graphics PSOs and omits qualifying pending direct draws on targets recurring across 5 Presents; changes rendering while linking, not a measured FPS/stutter gain |
| `d3d12.asyncPipelineCompileThreads` | Worker count for `d3d12.asyncPipelineCompile` (default 0 = max(1, logical CPUs / 2)). Workers run at lowest thread priority |
| `dxgi.customVendorId`, `dxgi.customDeviceId`, `dxgi.customDeviceDesc` | Reported adapter identity, not hardware capability |
| `dxgi.forceSDR` | Output reporting, not proof of physical colour/HDR correctness |
| `d3d11.*` | D3D11 behavior; does not configure D3D12 |
| `dxgi.handleAltTab` | D3D11 swapchain only despite the prefix |
| `dxmt.shaderMetalVersion` | Shared device's D3D11 target; D3D12 selects its target internally |

For a 30Hz D3D12 preference use `d3d12.preferredMaxFrameRate = 30`; prefer a divisor of display refresh. Consumers: [DXGI options](../src/dxgi/dxgi_options.cpp), [D3D12 swapchain](../src/d3d12/d3d12_swapchain.cpp), [D3D11 swapchain](../src/d3d11/d3d11_swapchain.cpp), [DXMT device](../src/dxmt/dxmt_device.cpp) and [D3D12 shader target](../src/d3d12/d3d12_pipeline_cache.hpp).

## D3D11 spatial upscaling

`DXMT_METALFX_SPATIAL_SWAPCHAIN=1` and `d3d11.metalSpatialUpscaleFactor` (default 2.0, minimum 1.0) affect only the D3D11 spatial swapchain. They are not [D3D12 NGX/temporal integration](../graphics-translator/NGX-INTEGRATION.ko.md).

## Diagnostics are separate from configuration keys

- `OWNED_GRAPHICS_TRACE=1` enables lifecycle tracing; `OWNED_GRAPHICS_TRACE_LIMIT` defaults to 8192, maximum 1,000,000 ([source](../src/d3d12/owned_trace.hpp)).
- `DXMT_LOG_PATH` and `DXMT_LOG_LEVEL` control logs, not support.
- Metal API/Shader Validation is selected through the [fixture runner](../graphics-translator/SMOKE-TEST.ko.md); see [debugging](DEVELOPMENT.md#verification-and-debugging).
- Adapter memory reporting does not turn UMA into physical dedicated VRAM. No documented configuration switch completes budget notifications or changes that hardware contract.
