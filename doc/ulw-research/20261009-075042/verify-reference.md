# Pristine GPTK4.0b2 Metal4 temporal proof
Reference binary: build/local/zzz-dlss-fix-20261009/precompile-run-01/binding-diagnosis/reference/D3DMetal.framework/Versions/A/D3DMetal.
SHA256:f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad. Root shasum and codesign --verify --strict --verbose=2 returned exit0, valid on disk, satisfies Designated Requirement.

Root repeated exact symbol-specific inspection after worker returned evidence:
```
xcrun llvm-objdump --macho -d --dis-symname __ZL19ReplayTemporalScaleR18MPLCommandReplayerRK10MPLCommand build/local/zzz-dlss-fix-20261009/precompile-run-01/binding-diagnosis/reference/D3DMetal.framework/Versions/A/D3DMetal
```
Exit0;0.06s. Entire ReplayTemporalScale function0x36656..0x36922 inspected. Selected observed selectors/calls:
-0x36869 selector setFence:, call0x36870.
-0x36881 selector updateFence:afterEncoderStages:, mask0x08000000, call0x3688d.
-0x368ac endEncoding, call0x368b3.
-0x368b6 encodeToCommandBuffer:, call0x368c3, buffer loaded from context+0x90 at0x368a5.
-0x368cd loads same context+0x90; computeCommandEncoder call0x368db.
-0x3690e waitForFence:beforeEncoderStages:, mask0x08000000, tail call0x36922.
The SDK maps0x08000000 to dispatch stage. No buffer split/new buffer assignment occurs within this replay function. Native MetalFX internals are opaque; no claim about all reference queue work.

Owned src/winemetal/unix/metalfx-backend.mm:2113-2166 instead splits prefix/scaler/suffix, passes nil scaler fence and uses all-stage queue visibility barriers. It explicitly documents a prior same-buffer fence-consumer deadlock, so comparison identifies an optimization research target, NOT safe permission to remove barriers or proof of latest FPS cause. No proprietary implementation was incorporated into product code.

No target execution, attach, analysis database, script, binary modification, server or temporary resource. Worker Ghidra invocation printed usage only; no decompilation claimed.
