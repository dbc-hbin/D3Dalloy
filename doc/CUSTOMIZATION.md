# Configuration

Configuration affects only components that read the key; it cannot add API/compiler support. See [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md) for capabilities and [d3dalloy.conf](../d3dalloy.conf) for examples.

## File, environment, and precedence

The [loader](../src/util/config/config.cpp) uses `prefix.name = value` syntax:

- `D3DALLOY_CONFIG_FILE` selects a file; unset/empty means `d3dalloy.conf` in the process working directory.
- `D3DALLOY_CONFIG="d3d12.preferredMaxFrameRate=30;dxgi.forceSDR=True"` supplies semicolon-separated assignments.
- Values with spaces need inner double quotes: `D3DALLOY_CONFIG='dxgi.customDeviceDesc="NVIDIA GeForce RTX 5060"'`. Without them the value stops at whitespace. Prefer the built-in game profile over redundant adapter overrides.
- Precedence is **built-in application profile < file < environment**. Keys before any section are general; `[App.exe]` applies only to the matching executable name.
- Configuration is module-local and initialized before use, not a process-global live-reload service. Set inputs before launching. Booleans accept case-insensitive `true`/`false`.

## Scope of current keys

| Keys | Effect and boundary |
| --- | --- |
| `d3d12.preferredMaxFrameRate` | D3D12 presentation-rate preference, not guaranteed FPS or measured pacing |
| `d3d12.lowLatency` | Opt-in forced adaptive Present pacing (default False). Bounds application-frame runahead and predicts the next CPU admission from measured completion feedback. Actual Reflex/Anti-Lag 2 Sleep calls take over input-before-render pacing without a duplicate wait |
| `d3d12.asyncPipelineCompile` | Opt-in (default False). Defers the final native link of ordinary graphics PSOs and omits qualifying pending direct draws on targets recurring across 5 Presents; changes rendering while linking, not a measured FPS/stutter gain |
| `d3d12.asyncPipelineCompileThreads` | Worker count for `d3d12.asyncPipelineCompile` (default 0 = max(1, logical CPUs / 2)). Workers run at lowest thread priority |
| `d3d12.enableAutoBreadcrumbs` | Records DRED auto-breadcrumbs for `SYSTEM_CONTROLLED` (default False, as on Windows). App `FORCED_ON`/`FORCED_OFF` settings take precedence |
| `dxgi.customVendorId`, `dxgi.customDeviceId`, `dxgi.customDeviceDesc` | Reported adapter identity, not hardware capability |
| `dxgi.forceSDR` | Output reporting, not proof of physical colour/HDR correctness |
| `d3d11.*` | D3D11 behavior; does not configure D3D12 |
| `dxgi.handleAltTab` | D3D11 swapchain only despite the prefix |
| `d3dalloy.shaderMetalVersion` | Shared device's D3D11 target; D3D12 selects its target internally |

For a 30Hz D3D12 preference use `d3d12.preferredMaxFrameRate = 30`; prefer a divisor of display refresh. Consumers: [DXGI options](../src/dxgi/dxgi_options.cpp), [D3D12 swapchain](../src/d3d12/d3d12_swapchain.cpp), [D3D11 swapchain](../src/d3d11/d3d11_swapchain.cpp), [D3Dalloy device](../src/d3dalloy/d3dalloy_device.cpp) and [D3D12 shader target](../src/d3d12/d3d12_pipeline_cache.hpp).

## Launcher-controlled low latency

Set `D3DALLOY_CONFIG="d3d12.lowLatency=True"` in the game process environment
before launch, or put `d3d12.lowLatency = True` in its selected configuration file.
Launchers should merge this assignment with their other inline settings, not discard
them. The default is False; no application profile automatically forces it on.
Set False or remove the assignment to disable forced pacing on the next launch.
This setting does not disable a game's own Reflex or Anti-Lag 2 request.

Forced pacing delays the return of a successfully submitted application-frame
Present, outside the swapchain lock. TEST, rejected, and DO_NOT_WAIT Presents
do not acquire that post-submit wait. Supported generated/real frame pairs share
one application frame. An actual enabled Reflex Sleep or Anti-Lag 2 Update call
uses the same scheduler before input; if those calls stop, explicitly enabled
forced pacing resumes. Setting a vendor option alone does not transfer ownership.
Game-requested low latency or a minimum frame interval still uses Present fallback
when Sleep is omitted, even with launcher force off. The interval survives that
fallback; a real input-before-render Sleep takes over without a duplicate delay.

Windows timed waits use interruptible NT relative timeouts. The predictor learns
wake lateness and advances adaptive deadlines without shortening a game-requested
minimum interval. Measured GPU gaps can increase that advance, while submitted work
already waiting for the GPU reduces it; wake lateness is an upper bound, not an
unconditional advance. Calibrated GPU completion timestamps guide prediction; CPU
retirement still controls resource ownership. CPU preparation and native submission
tail are measured separately. Present reserves capacity before queue/layer
preparation, rather than holding a device-wide lock while waiting for that work.
Frame selection and real-Present closure remain atomic inside the short capture
section. Out-of-order GPU completions cannot rewind the throughput predictor.

The owned `amdxc64.dll` facade is built and staged with D3D12 and loaded on device
creation for the Anti-Lag 2 SDK's `GetModuleHandle` discovery. It accepts the SDK's
mode/FPS settings, input delay, end-of-render and generated-frame-type packets.
It is not an AMD rendering driver. Reflex Boost, AMD control strings and the
private input-index query are not implemented and return unsupported errors.
AMD's public API has no application frame IDs; unrelated render threads submitting
frames out of order cannot be attributed beyond the SDK's FIFO frame boundaries.

This controls queued work, not GPU clocks or physical scanout. A game with a
separate simulation thread may still sample input early. GPU completion measurements
are not input-to-photon measurements, and synthetic improvements do not establish
a latency improvement in ZZZ or another game.

## D3D11 spatial upscaling

`D3DALLOY_METALFX_SPATIAL_SWAPCHAIN=1` and `d3d11.metalSpatialUpscaleFactor` (default 2.0, minimum 1.0) affect only the D3D11 spatial swapchain. They are not [D3D12 NGX/temporal integration](../graphics-translator/NGX-INTEGRATION.ko.md).

## Diagnostics are separate from configuration keys

- `OWNED_GRAPHICS_TRACE=1` enables lifecycle tracing; `OWNED_GRAPHICS_TRACE_LIMIT` defaults to 8192, maximum 1,000,000 ([source](../src/d3d12/owned_trace.hpp)).
- `D3DALLOY_LOG_PATH` and `D3DALLOY_LOG_LEVEL` control logs, not support.
- Metal API/Shader Validation is selected through the [fixture runner](../graphics-translator/SMOKE-TEST.ko.md); see [debugging](DEVELOPMENT.md#verification-and-debugging).
- Adapter memory reporting does not turn UMA into physical dedicated VRAM. No documented configuration switch completes budget notifications or changes that hardware contract.
