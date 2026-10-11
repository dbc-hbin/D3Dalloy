# Executed latest-log verification
Invocation: `python3 doc/ulw-research/20261009-075042/verify_logs.py`.
Observed: 2026-10-09. Exit 0 after correcting an initial unsupported assumption that the two log sinks have identical error multisets. Initial assertion FAILED; final analysis preserves differences instead of adding duplicate events.

| Source | SHA256 | Bytes | Lines | Compile failure lines | POSITION_0 warnings |
| --- | --- | ---: | ---: | ---: | ---: |
| live logs/game_1791530994266.log | 7b2de5a1824d57f43c2c68e5e42b5ca33412083d550da3c69fb0aeff95db6c4d | 1059402 | 11099 | 9131 | 1881 |
| live ZenlessZoneZero_d3d12.log | 675b47f07d8a182d158980e147db937fc5b0d3b86232d7248851535328a83d02 | 1063701 | 11030 | 9122 | 1881 |

Live root: `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test`.
Game log counts: PS SRV 3856, VS SRV 1142, VS CBV 814, PS CBV 1968, PS sampler 1305, vshs_main SRV 46. D3D12 file differs only in PS SRV (3847). These are failure LOG LINES, not unique shaders, frames, or independent events between sinks.
Both have 16 initial shader-probe successes. Game log has one MetalFX text match at line 76, HUD metric already exists at 16:30:50.850. No NGX parse/encode completion or temporal failure diagnostics in these two files. This absence does not prove the stage was not called: diagnostics may be opt-in.
No FPS or GPU timing series was extracted by this probe; no benchmark conclusion.
