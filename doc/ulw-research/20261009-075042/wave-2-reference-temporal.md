## EXPAND

### Conclusion
The pristine GPTK4.0b2 Metal4 SR replay path is characterized sufficiently to compare synchronization structure with owned SR. GPTK uses a scaler fence and encoder-stage synchronization around `encodeToCommandBuffer:` within the same visible command-buffer object; owned SR deliberately uses separate prefix/scaler/suffix native command buffers, nil scaler fence, and all-stage queue barriers. This is a real static architectural difference, not evidence of an FPS difference, a safe fence-removal opportunity, or the cause of the latest frozen scene.

### Identity, methods and evidence bounds
Reference binary: `build/local/zzz-dlss-fix-20261009/precompile-run-01/binding-diagnosis/reference/D3DMetal.framework/Versions/A/D3DMetal`. Reused lead-verified SHA256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad` and strict codesign PASS from `doc/ulw-research/20261009-075042/expansion-log.md:15`. Executed `file`, `nm -n -C`, section-header inspection, and bounded disassembly with installed Apple LLVM 21.0.0 `xcrun llvm-objdump`. `file` establishes this reference is x86_64 Mach-O, not arm64. All addresses below are unslid image virtual addresses/image offsets; inspected sections map these addresses identically to file offsets (`__text` VMA/file offset both 0xf50). No target launch, attach, prefix change, source edit, dependency addition, or proprietary source dump was performed.

Read the mandatory native-binary and Ghidra references first. Ghidra is installed at `/Users/hanbinnoh/Documents/yaagl-dx12/tools/ghidra_12.1.2_PUBLIC/support/analyzeHeadless`; actually invoked it and received headless usage under OpenJDK 21.0.12. No Ghidra decompilation is claimed. The existing `BindingReference.java` is hardcoded to unrelated binding functions, not temporal addresses; it was not changed. The useful conclusions here come from executed disassembly plus exact Objective-C selector references, not symbol names alone.

### Metal4 SR path: scaler lookup -> command recording -> native encode
Relevant symbols:
- `MPLTemporalScaler::ensureTemporalScaler(...)`: 0xcef0; actual function ends at 0xd149.
- `MPLCommandList::TemporalScale(...)`: 0x1cf94; ends at 0x1d1de.
- `ReplayTemporalScale(...)`: 0x36656; ends at 0x36922.
- `ngx::TemporalSuperSamplerMPL::Evaluate(D3D12GraphicsCommandListMPL&, ...)`: 0xa5a64.
- Legacy counterparts are distinct: `ngx::TemporalSuperSampler::Evaluate(D3D12GraphicsCommandListMTL&, ...)` at 0x100b40 and `EncodeTemporallyScaleMTLFX(D3DMCommandEncoder&, ...)` at 0x12fb55.

Scaler creation/lookup observations (`artifact://201`, particularly 0xcf62-0xcf86 and 0xd033-0xd149): the Metal4 cache contains `id<MTL4FXTemporalScaler>`, and its creation selector is `newTemporalScalerWithDevice:compiler:` (selector-reference 0x4c61c0). A cache hit compares the selected scaler with the current scaler, stores it, and returns a changed-selection flag. A miss builds the descriptor, calls creation at 0xd103, tests the returned object at 0xd116, and only inserts into the cache when nonnil. The failure path returns nil because the return register source was zeroed at 0xd03a. This closes the tempting but incorrect reading that a table pointer could escape as the scaler on allocation failure.

Recording observations (`artifact://217`): 0x1cfb4 calls `ensureTemporalScaler`; 0x1cfb9-0x1cfbc tests nil and branches straight to function return before appending a temporal command. Successful recording calls `FlushClears` (0x1cfd5), `EmitInternalRenderEncoders` (0x1cfdd), `ApplyResourceBarrierDependencies` (0x1d048), conditional `CheckBarrierDependencies` (0x1d061), resource dependency processing, and conditional `EmitBarrierDependencies` (0x1d0eb). It then appends a 0xa0-byte temporal replay record and finishes the encoder. GPTK therefore has substantial resource/dependency bookkeeping; it is not an unbarriered direct scaler shortcut.

Native replay observations (`artifact://200`, 0x3685e-0x36922, and successful selector-annotated replay recovered in `artifact://237`):
1. It binds frame resources, dimensions, motion/jitter/exposure parameters, and reset. The changed-scaler flag in the replay command forces reset at 0x36824-0x36848; otherwise the caller reset value is used.
2. It assigns the fence held at replayer offset 0x6d0 using `setFence:` at 0x36870.
3. The current compute encoder receives `updateFence:afterEncoderStages:` at 0x3688d, with mask 0x08000000.
4. The current encoder ends at 0x368b3.
5. The scaler receives `encodeToCommandBuffer:` at 0x368c3. Its command buffer was loaded from the context object at offset 0x90.
6. A fresh `computeCommandEncoder` is created on that same context command-buffer field at 0x368db, its argument table is restored at 0x368f6, and it receives `waitForFence:beforeEncoderStages:` via the tail call at 0x36922, again with mask 0x08000000.

Selector references were resolved from the binary's Mach-O selector pointers, then independently confirmed by successful Mach-O symbol-specific disassembly. `MTLCommandEncoder.h:63` in the shipped SDK defines `MTLStageDispatch = 1 << 27`, exactly 0x08000000. Within this complete replay function, there is no command-buffer creation/commit/split call, queue-stage barrier, or new command-buffer assignment. The supported statement is encoder/fence ordering around the scaler on one visible command-buffer object. MetalFX's internal encoders or buffers remain opaque.

### Owned SR comparison
Direct readback of `src/winemetal/unix/metalfx-backend.mm:2113-2188` confirms:
- Prefix preprocessing and input padding precede a `barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll` with device visibility.
- A split obtains a separate scaler command buffer; `configureScalerForFrame(..., nil, effectiveReset)` passes nil fence.
- Optional alpha scaling obtains another split.
- Another split obtains the suffix buffer, emits another all-stage queue barrier, and performs transfer/RCAS and caller-output copy.
- Every failure branch supplies a stage-specific `fx::Error.message`; successful history dimensions/activation update occurs only after finishing succeeds.

The difference is consequently specific: GPTK's inspected Metal4 SR boundary is fence-based and dispatch-stage-scoped, while owned's current boundary is queue-based, all-stage-scoped, and adds at least two split points (three with alpha). Owned's own comment explicitly says this isolation avoids an external same-buffer fence-consumer Metal4FX deadlock. GPTK shows that a different implementation successfully expresses a fence-based structure; static inspection does not establish that its scheme can safely be transplanted into owned's recording, allocator, residency, queue, or feedback lifecycle. It also does not establish whether GPTK's internal MetalFX work is cheaper.

### Failure propagation limits
GPTK's `TemporalScale` function drops recording on nil scaler creation; the inspected branch neither writes replacement output nor sets an explicit error value. Its successful replay has no branch testing a scaler encode return; Apple encode is void. In the NGX MPL evaluator, the relevant virtual call at 0xa61a9 is followed by state update at 0xa61b3, not a local error-result test (executed slice 0xa6150-0xa61d0). The exact vtable identity and complete public NGX return/error contract were not resolved, so do not promote this to a claim that GPTK always reports success on creation failure. The supported narrower claim is that recording-time nil rejection and GPU completion are different boundaries, and this replay itself exposes no GPU completion/output freshness verdict.

Owned's native errors are more explicit internally, but `src/winemetal/unix/winemetal_metal4_fx.mm:35-45` maps only error codes to HRESULT: invalid context/frame/texture -> 0x80070057; scaler creation/unsupported -> 0x887a0004; resource allocation -> 0x8007000e; encode failure -> 0x887a0020. The detailed message is lost at that boundary. Neither reference disassembly nor owned wrapper success proves changing output pixels or sustained latest-game SR success.

### Legacy is a separate comparison
Executed bounded legacy disassembly at 0x12fb55-0x130700 (`artifact://226`, `artifact://229`) shows `InsertSync` at entry (0x12fb7e), conditional `compensateForOffset` calls, encoder flushing (0x12ffcf), acquisition of the current-command-buffer fence, binding it through `setFence:` at 0x130243, `GetExternalCommandBuffer` at 0x130408, scaler encode at 0x13041b, subsequent fence wait, conditional reverse offset compensation at 0x13048c, and another `InsertSync` at 0x130494. Thus legacy reference has its own synchronization and conditional copy/reposition costs. Its offsets, transport, and execution identity must not be substituted for the Metal4 MPL path. No evidence here establishes which reference path a historical FPS comparison actually used.

### Closed leads and remaining data gap
Closed: pristine reference Metal4 SR is not a zero-cost direct call; its fence and dispatch-stage ordering differ materially from owned's isolated buffers and all-stage queue barriers. Legacy contains additional conditional offset compensation and synchronization, not the same path. Reference cache miss/nil handling and recording rejection were inspected, not merely inferred from names.

Unresolved by design: MetalFX internal commands, queue submission cost outside this replay, actual prefix/suffix activation and copy dimensions in the latest run, deadlock reproduction under GPTK's exact fence arrangement, native GPU error/completion status, and matched CPU/GPU frame timings. These require runtime evidence outside this read-only assignment. No FPS ratio, milliseconds saved, or latest frozen-scene cause can be inferred from the structural comparison. Stop condition met: one relevant complete Metal4 call path now supports a bounded structural comparison while rejecting a static performance verdict.