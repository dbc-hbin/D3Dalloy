# Executed identity, census and historical timing proof
Invocation: `python3 doc/ulw-research/20261009-075042/verify_evidence.py`, exit0,0.23s. Scope: on-disk files now; not loaded-image snapshot or latest FPS proof.

## On-disk SHA256 equals runtime manifest
- aarch64-windows/d3d12.dll: f4ad23dc28c23bf4d1be316ff403a1c4e4637e495c397d1fdce15b02d2b6f5a5
- aarch64-windows/nvngx.dll: a2ba4751cd9d2197c504b8a0d58accda32842f2b0ca9b3ba83239c55d3be71a6
- aarch64-unix/winemetal.so:615327c5e4e8fabb139fd03b86978ab91747b4581b739c0304517b6ee9a044cf
- i386-windows/d3d12.dll:5da7569fdd98e5bb1cc24af82aafc4cb6ca458ce37b9a022b377598852b98fba
- i386-windows/nvngx.dll:e7a51446ab99ba14cb4ba8de3869fdcd43469519ceef1edcda194e413bcd0960
All five PASS. Prefix: live Yaagl ZZZ OS Test/wine/lib/wine.

## Exact cache census
Live prefix AppData/Local/Temp/dxmt-shader-sessions/57a461904bc14837b3dbef13813d41086c5b48e7:
11116 files,237591400 logical bytes,226.5848159790039MiB.
Minimum mtime2026-10-09T07:33:47.405324Z; maximum07:36:45.586267Z. These are residue timestamps, not per-call durations/hit rates. Launcher had recreated prefix before this run; do not infer cache corruption or capacity pressure from residue.

## Historical compute phases
Source: build/local/zzz-dlss-fix-20261009/observe-run/game.log. MEASURED nanoseconds from traces; DERIVED milliseconds =ns/1000000. Selected consecutive creation ids139-143, not population extremes or latest run.

| creation | compile ms / line | native pipeline ms / line |
| --- | ---: | ---: |
|139|3.5319 /654|16.1211 /658|
|140|3.7206 /670|30.0251 /674|
|141|13.0204 /686|138.101 /690|
|142|21.1598 /702|148.4863 /706|
|143|150.2201 /718|1704.065 /722|

Root source read src/d3d12/d3d12_pipeline_compute.cpp:127-166 confirms native library/function/PSO creation remains after shader cache load and timing phase boundaries. Native pipeline time is elapsed call duration, not necessarily active CPU alone. Reuse/prewarming/async scheduling/native archive are candidates, not proven latest FPS gains.
