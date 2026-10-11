# Expansion wave 3: lead-owned quantitative and counterclaim verification
Parent leads: ShaderPerformance's timing/cache claims; ArtifactDiscriminators' screen settings and unlocated compressed log.

Executed verify_evidence.py, see verify-identity-timings.md: five on-disk module hashes match deployed manifest;11116 cache files total226.5848MiB with exact mtimes. Root did not substitute file size for loaded-image identity. Historical selected compute events converted using ns/1000000; corrected compile150.2201ms rather than15.02ms for creation143.

Read actual live user.reg1008-1020: current saved Screenmanager Width3840 and Height2160, Use Native1, fullscreen mode1. Historical launcher forced1280x720 and NGX trace independently confirms1280x720 output. This is a settings mismatch, not proof of latest MetalFX/backbuffer resolution. Derived configured pixel-area ratio3840*2160/(1280*720)=9; no9x FPS claim. Baseline D3DMetal/Rosetta settings remain uncaptured.

Read device.cpp1104-1111: advertised automatic-disk flag absent because cache is driver-managed; Player AUTOMATIC_DISK_CACHE0 does not mean no owned disk cache. Read command_list2014-2017: new native render pass dirties all bindings; Streamline disableCLStateTracking1 same in historical candidate, not a distinguishing freeze cause.

Rejected erroneous worker time-window/publication-rate/syscall-latency extrapolations and whole-mesh-cache exclusion. See verify-shader-corrections.md for full rejected assertions and arithmetic check.

Compressed-log lead: exact filename2026-10-09T07-30-01.zip searched with glob under actual game installation and live Yaagl test root; both no files. Read actual Persistent/LogDir: only NAP logs, latestNAP_20261009_163009.log. Closed as unavailable in scoped retained sources; no hypothetical contents asserted.

Closed data gaps: latest input/output content, actual SR dimensions, reset series, Present1 rectangles, failed-pair ids/callers absent. Requires new instrumented run, beyond requested existing-log/static investigation. No need to repeat historical reproductions to pretend these gaps are filled.
Reference temporal comparison remains pending. No new actionable code lead after verified artifact closure. Fresh refinement will check final proposed claim set after all research workers return.
