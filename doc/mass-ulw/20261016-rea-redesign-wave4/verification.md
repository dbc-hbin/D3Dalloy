# Wave 4 Verification

## Scope
Render-pass fence correctness and grouped replay propagation.

## Evidence
- `build/local/wave4-render-output-001/stdout.log` reaches `CLEAR_PRESERVE pixel=ff402080` and all 38 case markers.
- `build/local/wave4-render-output-001/result.json` records the remaining validator mismatch from the first implementation, which used a render boundary and omitted expected native waits in graph-boundary cases.
- `build/local/wave2-render-output/render-passes-x64_d3d12.log` and the two Wave 4 REA/source audits identify untracked attachment ordering as the root cause.
- `src/d3d12/d3d12_command_list.cpp` records `render_pass_requires_fence`; `src/d3d12/d3d12_command_queue.cpp` carries it through `force_fence_before` and ORs it across grouped members before replay skips nonleaders.
- Remove-ai-slops follow-up approved the grouped propagation; clangd diagnostics are clean except the existing override warnings; `git diff --check` passes.
- Changed D3D12 objects compile in `build/local/wave5-build`. Full link is blocked by the existing winemetal error: `undefined reference to __imp_WMTGetGuestWindowBase`.

## Status
Static implementation review: PASS. Current-source runtime proof: BLOCKED by the unrelated winemetal link failure. No FPS claim.
