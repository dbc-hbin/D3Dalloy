# Wave 1 verification

## Integrated source increment
- `src/d3d12/d3d12_command_queue.cpp` now records native visibility scratch/target buffers as conservative full-range `SubmissionAccess` entries keyed by their native buffer handles.
- Visibility-bearing render nodes are no longer rejected as opaque solely because they use visibility scratch/targets.
- Visibility-bearing nodes remain `merge_eligible=false` until internal fanout/reset/accumulate work is represented as explicit graph edges.
- The prior hazard-scoped primary render/compute/blit fence gate remains; unknown auxiliary encoder types and hard-coded helper waits remain conservative.

## Checks
- clangd diagnostics on `src/d3d12/d3d12_command_queue.cpp`: PASS, no errors.
- `git diff --check -- src/d3d12/d3d12_command_queue.cpp`: PASS.
- `python3 scripts/test-owned-graphics-diagnostics.py -q -k execution_graph`: PASS, 1 test.
- `python3 scripts/test-owned-graphics-diagnostics.py -q -k multiqueue`: PASS, 2 tests.
- `python3 scripts/test-owned-graphics-diagnostics.py -q`: 109/110 PASS; one pre-existing `distinct_compute` pipeline-artifact expectation failure. Failure: actual `{'shader_initialize': 1, 'air_compile': 1, 'new_library': 1, 'specialize_function': 0, 'new_function': 0, 'native_compute': 0}` versus expected native/function operations. No source change in this wave touches that artifact parser or pipeline path.

## Runtime limits
- No current-source runtime build or GPU fixture was available in this environment. No native wait reduction, query graph behavior, GPU memory traffic, or FPS result is claimed.
- Next wave: represent query fanout/reset/accumulate and other auxiliary GPU work as explicit producer/consumer edges before removing their hard-coded waits.
- Final query metadata extension: QueryEncoderData counters/source/destination buffers now enter the same conservative native-buffer access identity path; AliasFlush remains opaque because it is a sparse alias publication boundary. Focused checks remained green after this extension.
