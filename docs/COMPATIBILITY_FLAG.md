# D3D11 compatibility issue flags

These flags belong to the upstream **D3D11 debug HUD**, not the D3D12 capability matrix.
For D3D12, use the [current implementation status](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md)
and the corresponding fixture result.

## Enabling and reading the line

The Meson boolean option `-Ddxmt_debug=true` defines `DXMT_DEBUG`.
The D3D11 swapchain renders a 27-character compatibility field through `HUDState`.
Without `DXMT_DEBUG`, `HUDState::begin`, `printLine`, and `end` do not render the diagnostic lines.

The D3D12 swapchain initializes a HUD heading but does not emit this compatibility field.
An all-dash D3D11 field means only that none of the instrumented flags was raised;
it is **not** proof that rendering or every API contract is correct.

## Defined flags

Positions below are zero-based character indexes in the field.

| Characters | Position | Meaning | Current emission |
|---|---|---|---|
| `G` | 3 | Unsupported geometry draw | Defined/renderable; no current setter |
| `TO` | 5–6 | Unsupported tessellator output primitive | Raised for point/line output in the D3D11 path |
| `IT` | 8–9 | Unsupported indirect tessellation draw | Defined/renderable; no current setter |
| `GT` | 11–12 | Unsupported geometry+tessellation draw | Raised by the D3D11 combined-stage path |
| `A` | 14 | Unsupported `DrawAuto()` | Raised by D3D11 `DrawAuto` |
| `P` | 16 | Unsupported predication | Raised by D3D11 `SetPredication` with a non-null predicate |
| `SA` | 18–19 | Unsupported stream-output appending | Defined/renderable; no current setter |
| `MS` | 21–22 | Unsupported multiple stream output | Defined/renderable; no current setter |

Flags may combine. A declared flag without a setter does not establish support or lack of support for that feature.
In particular, this table must not be used to mark D3D12 predication, GS/HS/DS, or stream output as unimplemented.

## Source references

- [Meson options](../meson.options) and [debug define](../meson.build)
- [FeatureCompatibility and frame statistics](../src/dxmt/dxmt_statistics.hpp)
- [HUD rendering gate](../src/dxmt/dxmt_hud_state.cpp)
- [D3D11 flag rendering](../src/d3d11/d3d11_swapchain.cpp)
- [D3D11 flag setters](../src/d3d11/d3d11_context_impl.cpp)
- [D3D12 swapchain](../src/d3d12/d3d12_swapchain.cpp)

This reference documents the existing D3D11 diagnostic only; it does not expand the project's D3D11 implementation scope.
