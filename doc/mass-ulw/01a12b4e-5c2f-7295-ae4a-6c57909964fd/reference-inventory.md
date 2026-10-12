# D3DMetal 4.0b2 — bounded identity and capability inventory (REA-backed)

Scope: pristine reference artifact only. Read-only. No repository or reference files were modified; nothing was executed. All behavioral claims below cite an REA Evidence ID or are explicitly labeled INFERENCE / UNKNOWN.

---

## 1. Artifact identity (observed)

| Field | Value | Evidence |
|---|---|---|
| Path | `$D3DMETAL_REFERENCE` | ev_54d8e48b27f8c8e595ffe4be3d898960dc479bcea2571228fa5ab29f4fd66f95 |
| SHA-256 | `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad` (matches required reference digest) | ev_54d8e48b… ; open_binary returned the same digest |
| Format / arch | Mach-O thin, `x86_64` only (file_type 6 = dylib), UUID `674E662B-6B5C-3FD9-9A8A-F415609D2F6A` | ev_a01c9fb9df5c104f960caf80985af143a0d672fa4b434eb264d0ea8974a4bdfa (`exhaustive: true`, single slice) |
| Bundle id / version | `com.apple.D3DMetal`, CFBundleVersion + ShortVersion `4.0b2`, executable `D3DMetal`, FMWK, LSMinimumSystemVersion 14.0 | ev_d3bc2ca6deb0ec4d68b9f505b44fc8d7e3e9bbb6fddf77f22679a22805aad6a6 (Info.plist) |
| version.plist | `CFBundleShortVersionString 4.0b2`, `BuildVersion 2`, `ProjectName D3DRendererMetal`, `SourceVersion 33024000000000` | read directly (non-REA, allowed by scope) |
| Code signature | Signed, identifier `com.apple.D3DMetal`, anchor apple, cdhash `a85f8fd9952c2929f717c1c536ea8a44285fdd31`, no entitlements, **hardened runtime false** | ev_5899fd0bc2cb17cfec7367912dac4026e9fad8b5f22293c792f0be4c462fe1a5 |
| Size / code | 29 segments; `__text` 3,601,010 B; 10,224 analyzed procedures; 29,195 string objects | ev_54d8e48b27f8c8e595ffe4be3d898960dc479bcea2571228fa5ab29f4fd66f95 |
| Symbols | 55,324 symtab entries (49,908 local, 4,844 external defs, 572 undefined); Mach-O symbol table is present (not stripped of locals) | mach-o inspection (load commands LC_SYMTAB/LC_DYSYMTAB) |
| Build | platform macOS, minos 14.0, SDK 26.4, ld 1266.8, LC_SOURCE_VERSION 33.24 | same |
| Loaded deps | `@rpath/libmetalirconverter.dylib`, `@rpath/libdxccontainer.dylib`, MetalKit, **MetalFX**, Metal, AppKit, IOKit, CoreAnalytics (private), Foundation, CoreGraphics, libncurses, libc++, libobjc, libSystem | same |
| Bundle payload | `Resources/`: libdxcompiler.dylib (27.7 MB), libmetalirconverter.dylib (29.1 MB), libdxccontainer.dylib, libdxilconv.dylib, default.metallib (442.8 KB) | directory listing (non-REA) |

**INFERENCE (arch):** the artifact contains only an x86_64 slice, so on Apple-silicon hosts it must execute under Rosetta 2 translation; no native arm64 code is present in this artifact.

---

## 2. Evidence index (REA Evidence IDs used below)

| ID | Operation |
|---|---|
| ev_54d8e48b…f66f95 | binary_overview |
| ev_a01c9fb9…4a4bdfa | list_architectures |
| ev_5899fd0b…c462fe1a5 | inspect_signature |
| ev_d3bc2ca6…22805aad6a6 | inspect_plist (Info.plist) |
| ev_c6970a30…5019839f435 | search_procedures "CreateDevice" |
| ev_d6731816…ba51c58b2e | search_procedures `^_D3D12[A-Za-z]` |
| ev_e4fbd556…adc1097d450f | search_procedures `Barrier` |
| ev_5a1f21dd…2af5fb50eda2275 | search_procedures root-sig / descriptor-table |
| ev_b4f3fbc8…41e97edd3008 | search_procedures StageCache/Compile/CacheFile/ShaderCache |
| ev_adc73e27…0a03de33422d8e2a | search_procedures MetalIR/IRCompiler/DXIL/DXBC |
| ev_5ddb7c10…c3fb820ad586 | search_strings `DXIL` |
| ev_e0b59e81…7a7ea265284afa | search_procedures `^DXGISwapChain|^DXGIFactory` |
| ev_c692a73d…12ffee8c84c96799 | search_procedures MetalFX/FrameInterpolat/… |
| ev_1f76416b…f3ef4fcaa5655 | search_procedures EncodeDispatchRays/Graph/Mesh/… |
| ev_038c61f5…41c92a54fbdedb | search_procedures SupportFamily*/D3DMEncodeSignalEvent |
| ev_289e9f6f…98d3ac2c941a1e1 | search_strings `D3DM_` |
| ev_65af6b5b…89ba4630853c18 | search_strings `Optimiz` |
| ev_e600dd7b…23374837a074da98 | pseudocode `MPLCommandList::CheckBarrierDependencies` |
| ev_c6acf54f…a85c0a08f9cc2f58 | pseudocode `D3DMDevice::presentOnMetal4` |
| ev_ef60093e…2c5e35a700968600 | pseudocode `D3DMCompiler::CreateIRCompiler` |
| ev_3ff50d63…6293515914df51 | pseudocode `D3D12BarrierLayoutToD3D12ResourceState` |
| ev_4bd5b2e1…8c5689e769492c | pseudocode `D3DMCommandQueueWorker::DoExecute` |
| ev_18ac569b…e0247cf4ad045135 | pseudocode `D3DMStageCache::GetCachedResult` |
| ev_8d8aff65…4c1bc5e9895ad1aa | pseudocode `_D3D12CreateDevice` |
| ev_951b2782…f25df8d297654a | pseudocode `D3D12Device::CreateStateObject` |
| ev_557799f0…eebbd0618237340d9 | pseudocode `D3DMCommandEncoder::ResolveBarriers` |
| ev_63af347c…4f4f308f136c9b89 | xrefs to `MPL_ENABLE_RESOURCE_BARRIER_OPTIMIZATIONS` (0x4574b3) → single site 0x30fd4 |

---

## 3. Domain findings

### 3.1 API / device

**Observed**
- Exported D3D12 C entry points: `_D3D12CreateDevice` (0x70aae), `_D3D12GetDebugInterface` (0x6ec67), `_D3D12EnableExperimentalFeatures` (0x6ec6a), `_D3D12SerializeRootSignature` (0x16faaa), `_D3D12SerializeVersionedRootSignature` (0x16feb3), `_D3D12CreateRootSignatureDeserializer` (0x16fbf7), `_D3D12CreateVersionedRootSignatureDeserializer` (0x16fd58). Also `_D3D11CreateDevice`, `_D3D11CreateDeviceAndSwapChain`, `_CreateDXGIFactory2`, `_GetDXGISwapChainTotalFramesPresented`. [ev_d6731816…, ev_c6970a30…]
- The device interface is exported up to **ID3D12Device14** — the full 1:1 vtable wrapper set (`ID3D12Device14_CreateCommandQueue` … `CreateSampler2`, `GetResourceAllocationInfo3`, `CreateRootSignatureFromSubobjectInLibrary`, `OpenExistingHeapFromAddress1`, `CreateShaderCacheSession`, `ShaderCacheControl`, `AddToStateObject`, `CreateStateObject`, `CreateMetaCommand`, `EnumerateMetaCommands`, `CreateLifetimeTracker`, `RemoveDevice`). [ev_d6731816… plus the Device14 enumeration visible in the D3D12 procedure set]
- `_D3D12CreateDevice` (decompiled, ev_8d8aff65…): (a) calls `D3DMWaitForDebugger()` via `dispatch_once`; (b) rejects a null output pointer with `S_FALSE` (1); (c) validates the `riid` against a hardcoded table of 16 interface GUIDs and returns 1 if unrecognized; (d) accepts only `MinimumFeatureLevel` ∈ {0xb000, 0xb100, 0xc000, 0xc100, 0xc200} (= D3D_FEATURE_LEVEL_11_0/11_1/12_0/12_1/12_2), else `0x80070057` (E_INVALIDARG); (e) resolves an adapter via DXGIFactory2 (or uses the supplied `IDXGIAdapter4`), then constructs `D3D12Device` under an `os_unfair_lock` around a **process-global singleton** `D3D12CreateDevice::staticDevice` (allocation size 0xcf8); (f) returns `0x887a0020` (DXGI_ERROR_UNSUPPORTED) with no adapter, `0x887a0004` (DXGI_ERROR_DEVICE_REMOVED) when the adapter's Metal device is null.
- Device-downlevel and debug surfaces exist as separate interface wrappers: `D3D12DeviceDownLevel` (ID3D12DeviceDownLevel, `QueryVideoMemoryInfo`), `D3D12DebugDevice` → **ID3D12DebugDevice2** (`SetFeatureMask`, `GetFeatureMask`, `ReportLiveDeviceObjects`, `SetDebugParameter`, `GetDebugParameter`), `D3D12Debug_StartCapture/StopCapture`, `D3D12Debug_StartTest/FinishTest/StartSubTest/FinishSubTest`. [ev_d6731816…, ev_289e9f6f…]
- Adapter/output objects: `DXGIAdapter` (IDXGIAdapter4), `DXGIFactory` (IDXGIFactory7), `DXGIDevice` (IDXGIDevice4), `DXGIOutput` (IDXGIOutput6), `DXGIResource`. Exported IIDs cover IDXGIAdapter1–4, IDXGIDevice1–4, IDXGIFactory1–7, IDXGIOutput1–6, IDXGISwapChain1–4. [ev_e0b59e81…, ev_d6731816…]
- Runtime environment knobs exist as strings: `D3DM_MTL4`, `D3DM_SUPPORT_DXR`, `D3DM_ENABLE_METALFX`, `D3DM_NO_WINDOW`, `D3DM_MAX_FPS`, `D3DM_SHOW_HUD_STATS`, `D3DM_VENDOR_ID/DEVICE_ID/DEVICE_REVISION/DEVICE_SUBSYS/DEVICE_DESCRIPTION`, `D3DM_EXE_OVERRIDE`, `D3DM_NOT_IMPLEMENTED`, plus shader-semantics knobs `D3DM_BOUNDS_CHECK`, `D3DM_FLUSH_POS_INF_TO_NAN`, `D3DM_SAMPLE_NAN_TO_ZERO`, `D3DM_POSITION_INVARIANCE`, `D3DM_FORCE_RTZ_TEXWRITE`, `D3DM_MIN_LOD_CLAMP`, `D3DM_LOD_BIAS`, `D3DM_DXIL_PROCESS_DEBUG_INFORMATION`, `D3DM_IGNORE_D3D11_RENDER_BARRIERS`, `D3DM_NVNGX_PATH`. [ev_289e9f6f…]
- The bundle is a full D3D11 + D3D12 + DXGI + D3D10Blob + NVAPI + NGX(DLSS) surface: exported `_nvapi_QueryInterface`, `_nvapi_Direct_GetMethod`, `_NVSDK_NGX_D3D12_*` and `_NVSDK_NGX_D3D11_*` feature APIs. [inspect_macho imports/exports; corroborated by procedure names in ev_e4fbd556… and the D3D12 literal procedure set]

**Inference**
- `D3DM_DXIL_PROCESS_DEBUG_INFORMATION` and `D3DM_IGNORE_D3D11_RENDER_BARRIERS` read as debug/diagnostic switches, not feature gates (they are consumed alongside logging strings).
- The process-global singleton in `_D3D12CreateDevice` implies one D3D12 device per process (all `QueryInterface` calls hand out the same object); this is an architectural difference from DXMT, which the audit should confirm on the DXMT side.

**Unknown**
- Which GUIDs the 16-entry `riid` table actually is — the decompiler emitted only raw LUID/`_GUID` field comparisons; the exact interface list was not resolved to names. Coverage limit: needed `read_bytes` on the GUID table, not performed.
- `_D3D12EnableExperimentalFeatures` and `_D3D12GetDebugInterface` bodies were not decompiled; only their presence and address are established.
- No `CheckFeatureSupport` decompilation was performed, so per-feature-level capability results are UNKNOWN.

---

### 3.2 Shader / binding (translation + optimization entry points)

**Observed**
- Translation pipeline is a two-library chain, confirmed by both import records and internal procedures: `libdxccontainer` (`DXCompiler::DXBCToDXIL`, `DXCompiler::HLSLToDXIL`, `DXCompiler::GetDxilPartByType`) → `libmetalirconverter` (`IRObjectCreateFromDXIL`, `DXIL2AIR`, `DXILContainer`, `IRCompiler*`, `IRMetalLibBinary`, `IRShaderReflection`). [ev_adc73e27…, ev_5ddb7c10…]
- `D3DMCompiler::CreateIRCompiler` (0x87c36, decompiled, ev_ef60093e…) is the concrete translation-configuration point. Observed call sequence on a fresh `_IRCompilerCreate()`:
  - `_IRCompilerSetCompatibilityFlags` from a bitfield built out of 10 D3DMCompiler flag bytes plus a constant `+0x200`;
  - `IRCompilerEnableGeometryDebugging`, `IRCompilerForce4WideVaryings`, `IRCompilerUnsafePositionInvariance`, `IRCompilerSupportRGB32TypedBuffers`, `IRCompilerFlushFragmentColorNanToZero`, `IRCompilerSupportUnAlignedTypedBuffers`, `IRCompilerPatchFloatToUnorm`, `IRCompilerSupportGloballyCoherent(…, true)`, `IRCompilerEnableShaderDetection`, `IRCompilerEnableGOSPayloadReadWA`, `IRCompilerEnableFP64Emulation` — each driven by a distinct D3DMCompiler field;
  - `_IRCompilerSetStageInGenerationMode(p, 1)`, `IRCompilerSetShaderID(p, name)`, `_IRCompilerSetValidationFlags(p, 0)`, `IRCompilerSetTargetTriple(p, str)`, `_IRCompilerSetMinimumGPUFamily(p, u32)`, `_IRCompilerIgnoreRootSignature(p, flag)`, `_IRCompilerIgnoreDebugInformation(p, !flag)`, and `IRCompilerEnableFeedback(p, shaderID == configuredFeedbackID)` (string compare).
- Root signature / binding objects are first-class: `D3D12RootSignature` (incl. `FindRootSignaturesInLibrary`), `D3D12RootSignatureDeserializer`, `D3D12VersionedRootSignatureDeserializer`, `D3D12RootSignatureBlob`, `D3D12Blob`, `D3D12ShaderBlob`, and conversion helpers `D3D12ToD3DMRootSignature`, `D3D12StreamToD3DMPipelineStateDesc`, `D3D12ToD3DMGraphicsPipelineStateDesc`, `D3D12ToD3DMComputePipelineStateDesc`, `D3D12ToD3DMSamplerDesc` (from `D3D12_SAMPLER_DESC` and `D3D12_STATIC_SAMPLER_DESC`). [ev_5a1f21dd…, ev_adc73e27…]
- Descriptor/binding plumbing observed: `D3D12DescriptorHeap` (`GetDesc`, `GetCPUDescriptorHandleForHeapStart`, `GetGPUDescriptorHandleForHeapStart`), root descriptor-table setters at three layers — `D3D12GraphicsCommandListMTL::Set{Compute,Graphics}RootDescriptorTable`, `…CommandListMPL::…`, `D3D12CommandBundle::…`, plus `EncodeBundledSet{Compute,Graphics}RootDescriptorTable` and `EncodeSetDescriptorHeaps`. [ev_5a1f21dd…, ev_1f76416b…]
- Pipeline-state objects: `D3D12PipelineState` (+`GetCachedBlob`), `D3D12GraphicsPipelineState`, `D3D12ComputePipelineState`, `D3D12PipelineLibrary` (ID3D12PipelineLibrary1: `StorePipeline`, `LoadGraphicsPipeline`, `LoadComputePipeline`, `LookupPipeline`, `Serialize`, `GetSerializedSize`, `LoadPipeline`). [ev_5a1f21dd…, plus D3D12 procedure set]
- Shader-stage handling includes emulated stages as distinct compile units: `CompileStageIn`, `CompileStreamOut`, `CompileGeometry`, `CompileHull`, `CompileDomain`, `CompileMesh`, `CompileAmplification`, `CompileVertex`, `CompileFragment`, `CompileCompute`, `CompileRTFunction`, `CompileIntersectionWrapperFunction`, `CompileNodes`, `CompileAndLinkStage`. [ev_b4f3fbc8…]
- GPU-family feature gating exists as explicit predicates `SupportFamily10Features`, `SupportFamily11Features`, `SupportFamily13Features` (MTLDevice → bool). [ev_038c61f5…]

**Inference**
- The `IRCompilerSetCompatibilityFlags` value is assembled from per-flag booleans, so it encodes the D3D12→Metal semantic-compatibility policy (NaN/INF, RTZ, position invariance, 4-wide varyings, typed-buffer alignment, unorm patching, FP64 emulation, globally coherent). The exact bit assignments are not recoverable from this pseudocode alone.
- `CompileAndLinkStage` plus `IRCompilerAllocCompileAndLink` indicate a Metal **static-linking** strategy for multi-function stage groups rather than per-function dynamic linking.
- Shader-detection (`IRCompilerEnableShaderDetection`) plus `MTLSetShaderCachePath`/`MTLMakeShaderCacheWritableByAllUsers` imports indicate Metal's own shader cache is pointed at a D3DMetal-controlled path.

**Unknown**
- The concrete `IRCompilerSetTargetTriple` value and `SetMinimumGPUFamily` value at runtime (fields, not constants) — UNKNOWN from static analysis.
- Whether HLSL source is ever compiled in-band (`HLSLToDXIL` is imported but no internal caller was traced) — UNKNOWN; DXBC→DXIL is the path evidenced by `DXBCToDXIL` + `IRObjectCreateFromDXIL`.
- Descriptor-table → Metal argument-buffer/`MTL4ArgumentTable` lowering internals — not decompiled (only entry points established).

---

### 3.3 Resources / barriers

**Observed**
- `D3D12BarrierLayoutToD3D12ResourceState` (0x16c26c, decompiled, ev_3ff50d63…) is a **29-entry lookup table** at `DAT_0042cf80` mapping `D3D12_BARRIER_LAYOUT` (1..29) → `D3D12_RESOURCE_STATES`, returning 0 out of range. This is direct evidence that D3D12 **enhanced barriers** (`D3D12_BARRIER_GROUP`) are handled, not just legacy `ResourceBarrier`.
- Barrier interface coverage: `ID3D12GraphicsCommandList6_ResourceBarrier`, `ID3D12GraphicsCommandList10_ResourceBarrier`, `ID3D12GraphicsCommandList10_Barrier` (enhanced), `D3D12CommandBundle::Barrier`, `D3D12CommandBundle::ResourceBarrier`, `EncodeResourceBarrier`, `EncodeTransitionBarrier`, `EncodeInternalBarrier`, `EncodeBundledResourceBarrier`. [ev_e4fbd556…, ev_1f76416b…]
- Two independent barrier-tracking stacks exist:
  1. **MPL layer** (`MPLCommandList`): `StageBarrier`, `StageBarrierImpl`, `BufferBarrier`, `TextureBarrier`, `EnqueueResourceBarrier`, and a dependency engine `CheckBarrierDependencies` / `EmitBarrierDependencies` / `ApplyResourceBarrierDependencies`, with per-subresource maps `MPLSubresource → {ResourceBarrier, NoProducerBarrier, PendingNoProducerBarrier}` and an `EncoderDependenciesStack`. [ev_e4fbd556…]
  2. **D3DM layer** (`D3DMCommandEncoder`): `EnqueueGlobalBarrier`, `EnqueueTransitionBarrier`, `EnqueueInternalBarrier`, `ResolveBarriers`, `ReactivateTransitionBarriers`, `DeactivateTransitionBarriers`, keyed by `Subresource → TransitionBarrier` (ankerl::unordered_dense maps). [ev_e4fbd556…]
- `MPLCommandList::CheckBarrierDependencies` (decompiled, ev_e600dd7b…) is a real **bitset dependency solver**: it walks per-encoder-type dependency bitsets (`MPLDynamicBitset<unsigned long long>`), masks bits above the consumer stage (`MaskBitsAbove`), ORs producer sets into an accumulator, and calls `EndEncoder` when a dependency is already satisfied by the current encoder. This is dead-barrier/encoder-split elimination logic, not a pass-through.
- `D3DMCommandEncoder::ResolveBarriers` (0x176eb0, decompiled, ev_557799f0…) confirms barrier **coalescing and deferral**: it builds an EncoderType mask (1→3, 2→4, 4→8, 8→0x10), early-outs when no barrier bits are pending, walks pending transition/internal barrier vectors, computes aggregate stage masks, and then emits Metal barriers conditionally:
  - `memoryBarrierWithScope:` (scope 3) for blit-type work,
  - `memoryBarrierWithScope:afterStages:beforeStages:` (scope 3, after 0x11, before 9) for compute,
  - with a **serial-decrement** (`uVar27 - 1`) path that defers a barrier to a later encoder when the tracked serial still matches, i.e. explicit redundant-barrier suppression.
  It also reads global policy flags `D3DMDevice::ForceBarriersSync`, `HonorUAVBarrierForBlit`, `NeedsFenceWorkaround`, `SupportMeshShaders`, and updates HUD counters (`DAT_00523fac`, `DAT_00523fb0`).
- Optimization toggles exist as strings `MPL_ENABLE_RESOURCE_BARRIER_OPTIMIZATIONS` (0x4574b3) and `MPL_ENABLE_CLEAR_OPTIMIZATIONS` (0x4573cc); the barrier string has exactly one xref, to 0x30fd4. [ev_65af6b5b…, ev_63af347c…]
- Resource object model: `D3D12Resource`, `D3D12Buffer` (ID3D12Resource2), `D3D12Texture` (ID3D12Resource2), `D3D12Heap` (ID3D12Heap1), `D3D12AccelerationStructure` (ID3D12Resource2), `D3D12Fence` (ID3D12Fence1), `D3D12QueryHeap`/`D3D12QueryHeapMPL`, `D3D12CommandAllocator`, plus `D3DMResource`/`D3DMHeap`/`D3DMTexture`/`D3DMBuffer` and a `d3dm_memory_allocator.mm` unit. [ev_d6731816…, ev_289e9f6f…]
- Device-level resource creation covers the full modern surface: `CreateCommittedResource`/`1`/`2`/`3`, `CreatePlacedResource`/`1`/`2`, `CreateReservedResource`/`1`/`2`, `CreateHeap`/`1`, `OpenExistingHeapFromAddress`/`1`, `OpenExistingHeapFromFileMapping`, `GetResourceAllocationInfo`/`1`/`2`/`3`, `MakeResident`/`Evict`/`EnqueueMakeResident`, `SetResidencyPriority`, `CreateProtectedResourceSession`/`1`, `CreateSamplerFeedbackUnorderedAccessView`, `GetCopyableFootprints`/`1`. [D3D12 procedure set from the D3D12 literal search; addresses in ev_d6731816… for the vtable layer]

**Inference**
- The MPL layer (a Metal-Pipeline-Layer style command replayer: `MPLCommandList`, `MPLCommandReplayer`, `Replay*` functions, `MPLContext`, `MPLFrameInterpolator`) appears to be a second, newer encoding path alongside the older `D3D12GraphicsCommandListMTL` path — the class split `…CommandListMTL` vs `…CommandListMPL` and the `D3DM_MTL4` string suggest the MPL path is the Metal-4-era path.
- `MPL_ENABLE_RESOURCE_BARRIER_OPTIMIZATIONS` being read at exactly one site implies a single global gate for the barrier-elimination logic in `CheckBarrierDependencies`/`ResolveBarriers`; whether it is env-driven or default-on was not resolved.

**Unknown**
- The literal contents of the 29-entry layout table (readable via `read_bytes` at 0x42cf80) — not read; mapping is observed as table-driven only.
- Default value/source of `MPL_ENABLE_RESOURCE_BARRIER_OPTIMIZATIONS` and whether the optimization is on by default — UNKNOWN.
- `EncodeTransitionBarrier` / `EncodeInternalBarrier` bodies not decompiled; the Metal API calls they emit were not observed directly.

---

### 3.4 Queue / presentation

**Observed**
- `D3D12CommandQueue` implements ID3D12CommandQueue in full: `ExecuteCommandLists`, `UpdateTileMappings`, `CopyTileMappings`, `Signal`, `Wait`, `GetTimestampFrequency`, `GetClockCalibration`, `SetMarker`/`BeginEvent`/`EndEvent`, `GetDesc`, `CreateEncoderInstance`. Device-side `CreateCommandQueue` / `CreateCommandQueue1` also exist. [ev_d6731816…, ev_c6970a30… (queue family), ev_289e9f6f…]
- Submission is asynchronous on a dedicated worker: `D3DMCommandQueueWorker` with `ThreadStart`/`RunThread`, `DoExecute`, `DoPresent`, `DoEvent`, `DoUpdateTextureMappings`/`DoUpdateBufferMappings`, `DoCopyTextureMappings`/`DoCopyBufferMappings`, `FlushEvents`, `Encode`/`EncodeExecute`/`EncodePresent`/`EncodeSignal`/`EncodeWait`, and command classes `ExecuteCommandListsCommand`, `EventCommand`, `PresentCommand`, `UpdateTextureMappingsCommand`, `CopyBufferMappingsCommand`. [D3D12 procedure set; class names also in mach-o export/typeinfo list]
- `D3DMCommandQueueWorker::DoExecute` (decompiled, ev_4bd5b2e1…) is the submission core. Observed behavior:
  - blocks on a `dispatch_semaphore_wait` on the command's semaphore, then `FlushEvents`;
  - if `D3DMDevice::ResidencySetsEnabled == 1`, takes an `os_unfair_lock` and sends `commit` to a Metal residency set (`*(…+200)`);
  - **two commit paths selected by `this[0x1a]`**: when 1 → allocates `MTL4CommitOptions`, `addFeedbackHandler:`, `D3DMEncodeWaitForEvent`, then `commit:count:options:` and `signalEvent:value:` (Metal 4 / `MTL4CommandQueue`); when 0 → `addCompletedHandler:` + `commit` on an `MTLCommandBuffer`;
  - before commit it walks pending event commands and calls `D3DMFence::EncodeSignalMTL3` for those already satisfied;
  - after commit it releases the completion objects and wakes waiters via `std::__cxx_atomic_notify_one`/`notify_all` and a per-queue pending counter.
- Event/fence encoding is split by queue flavor, matching the two commit paths: `D3DMEncodeSignalEvent` / `D3DMEncodeWaitForEvent` each exist twice — once taking `MTL4CommandQueue`, once taking `MTLCommandBuffer` (0x1733cc/0x17334a vs 0x173461/0x1733df). [ev_038c61f5…]
- Queue lifecycle helpers: `D3DMCommandQueue::WaitUntilSubmission`, `WaitUntilDrained`, `AcquireEncoder`, `AcquireMPLCommandReplayer` / `ReturnMPLCommandReplayer`, `ReturnEncoder`, `RetainResourceForCurrentCommandBuffer`, `FlushResourceInitializationAndWait`, `PrepareForPresent`. [D3D12/queue procedure set]
- Presentation: `DXGISwapChain` implements IDXGISwapChain…IDXGISwapChain4 — `Present`, `Present1`, `GetBuffer`, `ResizeBuffers`/`1`, `ResizeTarget`, `SetFullscreenState`, `GetDesc`/`GetDesc1`/`GetFullscreenDesc`, `GetHwnd`, `GetCoreWindow`, `Set/GetBackgroundColor`, `Set/GetRotation`, `Set/GetSourceSize`, `Set/GetMaximumFrameLatency`, `GetFrameLatencyWaitableObject`, `Set/GetMatrixTransform`, `GetCurrentBackBufferIndex`, `CheckColorSpaceSupport`, `SetColorSpace1`, `SetHDRMetaData`, plus internals `ReleaseBackbuffers`, `GetStagingBackbuffer`, `ResizeBuffersInternal`, `UpdateHUD`, `CurrentColorSpace`. `DXGIFactory` (IDXGIFactory7) provides `CreateSwapChain`, `CreateSwapChainForHwnd`, `CreateSwapChainForComposition`, `EnumAdapters`/`EnumAdapters1`/`EnumAdapterByLuid`/`EnumAdapterByGpuPreference`/`EnumWarpAdapter`, `MakeWindowAssociation`, `CheckFeatureSupport`. [ev_e0b59e81…]
- A HUD/overlay path exists (`D3DM_SHOW_HUD_STATS`, `D3DM_MAX_FPS`, `DXGISwapChain::UpdateHUD`). [ev_289e9f6f…, ev_e0b59e81…]

**Inference**
- `D3DMDevice::presentOnMetal4` (ev_c6acf54f…) decompiled to a single field load (`return this[0xac8]`) — it is an inline getter, not the present implementation; the real present work is in `D3DMCommandQueueWorker::DoPresent` / `EncodePresent` (evidenced by name and by the DoExecute path). Labeled as inference because `DoPresent` itself was not decompiled.
- The presence of both `MTL4CommitOptions`/`commit:count:options:` and classic `MTLCommandBuffer commit`, gated by a single per-worker flag, plus the `D3DM_MTL4` string, implies runtime selectable Metal-4 vs Metal-3 submission.

**Unknown**
- `DXGISwapChain::Present` body not decompiled — the drawable acquisition, CAMetalLayer vs MTL4 present path, and vsync/frame-latency semantics are UNKNOWN. (`CAMetalDrawable` does appear in the Mach-O typeinfo/`std::function` signatures.)
- Whether presentation uses `CAMetalLayer.nextDrawable` or an `MTL4CommandQueue` present-command pair is UNKNOWN; `D3DMDevice::GetMTL4PresentCmdPair`/`ReturnMTL4PresentCmdPair` and `D3DMCommandQueue::GetMetal4ArgumentTableForPresent`/`GetBlitPipeline` exist but their call sites were not traced.
- `D3DM_NO_WINDOW`/`D3DM_EXE_OVERRIDE` semantics (offscreen vs windowed) — UNKNOWN beyond the error string that recommends `D3DM_NO_WINDOW=1` when no drawable is available for an HWND.

---

### 3.5 RT / advanced features

**Observed**
- `D3D12Device::CreateStateObject` (0x697ee, decompiled, ev_951b2782…) dispatches on `D3D12_STATE_OBJECT_TYPE` and returns E_INVALIDARG-class codes for unknown types:
  - type 0 → `D3D12CollectionObject::CreateCollectionObject`,
  - type 3 → `D3D12RaytracingStateObject::CreateRaytracingStateObject`,
  - type 4 → `D3D12ExecutableStateObject::CreateExecutableStateObject`,
  then `QueryInterface` and Release on failure. `AddToStateObject` and `GetRaytracingAccelerationStructurePrebuildInfo` are implemented alongside it.
- Raytracing state-object building is a full subobject graph: `StateObjectBuilder` with `ParseStateObjectInto`, `CreateDXILContainer`, `CompareFunctionAssociations`, `isFullyCompiled`, and subobject classes `HitGroup`, `PipelineConfig`, `ShaderConfig`, `ObjectConfig`, `GlobalRootSignature`, `LocalRootSignature`, `NodeMask`, `Function`, `Export`, `DXILContainer`, `StateObject`; plus `NodeShaderDXILLibrary`, `NodeShaderDescriptor`, `ShaderIdentifier`, `D3D12StateObjectCompiledFunction`. [ev_c692a73d…, ev_5ddb7c10…]
- Acceleration structures: `MPLCommandList::BuildAccelerationStructure`, `RefitAccelerationStructure`, `CopyAccelerationStructure`, `WriteCompactedAccelerationStructureSize`, `BeginUseAccelerationStructure`/`EndUseAccelerationStructure`; `MPLCommandReplayer::AccelerationStructureEncoder`; `ReplayBuildAccelerationStructure`/`ReplayRefitAccelerationStructure`/`ReplayCopyAccelerationStructure`/`ReplayWriteCompactedAccelerationStructureSize`; `EncodeBundledBuildRaytracingAccelerationStructure`/`…CopyRaytracingAccelerationStructure`/`…EmitRaytracingAccelerationStructurePostbuildInfo`; and `InitialiseBVH`, `CopyAndCompactHeader`, `WriteSizesCore`, `BVHResourceID`, `RemapDescriptorIndirect(D3D12_RAYTRACING_INSTANCE_DESCR…)`, `AppendPostbuildInfo`, `AccelerationStructureSubResource`. [ev_c692a73d…, ev_5ddb7c10…]
- RT dispatch encoders: `EncodeDispatchRays` (0x15fd0d/0x15fd12), `DispatchRaysIndirect` (0x16015b), `EncodeDispatchRaysExecuteIndirect` (0x16319b/0x1631b0), plus `EncodeBuildRaytracingAccelerationStructure`, `EncodeCopyRaytracingAccelerationStructure`, `EncodeSerializeRaytracingAccelerationStructure`, `EncodeEmitRaytracingAccelerationStructurePostbuildInfo`. [ev_1f76416b…, ev_c692a73d…]
- Work graphs: `D3D12::WorkGraphCommandEncoder`, `D3D12WorkGraphProperties`, `D3D12ExecutableBaseStateObject(+Properties)`, `EncodeDispatchGraph` (0x1607bf/0x1607c4), `NodeShaderDescriptor`, `NodeShaderDXILLibrary`, `CompileNodes`, `getMaxInputRecordsPerNode`, `DumpNodeOutputs`, `OverrideNodeID`, `NodeShaderDescriptorOverrides`, `d3dm_node_shader_descriptor.mm/.cpp` + `_debug.cpp`. [ev_1f76416b…, ev_c692a73d…, ev_5ddb7c10…, ev_289e9f6f…]
- Mesh shaders / amplification: `EncodeDispatchMesh` (0x15fb96/0x15fb9b), `EncodeBundledDispatchMesh`, `CompileMesh`, `CompileAmplification`, `SupportMeshShaders` flag consumed in barrier resolution (ev_557799f0…). [ev_1f76416b…, ev_b4f3fbc8…]
- MetalFX integration is real and compiled in (MetalFX.framework is an LC_LOAD_DYLIB): `EncodeTemporallyScaleMTLFX`, `EncodeSpatiallyScaleMTLFX`, `EncodeInterpolateFrameMTLFX`, `EncodeDenoiseMTLFX`; `MPLFrameInterpolator` (`ensureFrameInterpolator`, keyed hash map), `MPLContext::CreateFrameInterpolator`, `MPLCommandList::FrameInterpolate`, `ReplayFrameInterpolate`, `D3DMCommandEncoder::CreateRenderCommandEncoderForFrameInterpolationBidirectionalDistortionField`. Imported descriptor classes: `MTLFXSpatialScalerDescriptor`, `MTLFXTemporalScalerDescriptor`, `MTLFXTemporalDenoisedScalerDescriptor`, `MTLFXFrameInterpolatorDescriptor`. [ev_c692a73d…, mach-o imports]
- NGX/DLSS shim: `nvapi::NvAPI_D3D12_GetRaytracingCaps`, `…BuildRaytracingAccelerationStructureEx`, `…GetRaytracingAccelerationStructurePrebuildInfoEx`, `…SetNvShaderExtnSlotSpace`, `…SetCreatePipelineStateOptions`, `…NotifyOutOfBandCommandQueue`, `…QueryCpuVisibleVidmem`; `ngx::Feature`, `ngx::FrameGenerator`, `ngx::ImageSignalProcessor`, `ngx::frameInterpolationRuntimeCheck`, and exported `_NVSDK_NGX_D3D12_*`. [ev_e4fbd556…, ev_c692a73d…, mach-o exports]
- `D3D12Device::BuildNullAccelerationStructure` and `D3D12Device::AccelerationStructuresAreCPUAccessible` exist as device-side helpers. [D3D12 procedure set]
- Advanced device features also present: `CreateMetaCommand`/`EnumerateMetaCommands`/`EnumerateMetaCommandParameters`, `SetBackgroundProcessingMode`, `CreateProtectedResourceSession(1)`, `CreateLifetimeTracker`, `SetEventOnMultipleFenceCompletion`, `SetStablePowerState`, `CheckDriverMatchingIdentifier`, `CreateSamplerFeedbackUnorderedAccessView`, `EncodeSetSamplePositions`, `GetResourceTiling`, `CreateCommandSignature` + `D3D12CommandSignature(MPL)`. [D3D12 procedure set]

**Inference**
- `D3DM_SUPPORT_DXR` reads as the gate for enabling the DXR path; its default is unknown. The RT code is unambiguously compiled in regardless (procedures + decompiled `CreateStateObject` dispatch), so the flag most plausibly controls runtime enablement/reporting rather than code presence.
- RT shader compilation is cached through the same stage-cache infrastructure with its own key type (`D3DMStageCache::RaytracingStageCacheKey`, `CompileRTFunction`, `CompileIntersectionWrapperFunction`), i.e. RT is not a separate compilation pipeline.
- `MTL4AccelerationStructureDescriptor*` types in `MPLCommandList::BuildAccelerationStructure` indicate the MPL path builds ASes with the Metal-4 descriptor API, while the legacy path uses `MTLAccelerationStructure{Primitive,Instance}Descriptor`.

**Unknown**
- `D3D12RaytracingStateObject::CreateRaytracingStateObject` body was not decompiled — actual subobject validation/compilation order and failure modes are UNKNOWN.
- Whether RT is functional on the target GPU, and whether `RaytracingShaderConfig`/`RaytracingPipelineConfig` limits are enforced, is UNKNOWN (RDAT reader accessors exist at 0xf80/0x1000/0x1140 for `MaxAttributeSizeInBytes` and `MaxTraceRecursionDepth`, but no consumer was traced).
- MetalFX feature enablement requires `D3DM_ENABLE_METALFX` (string, ev_289e9f6f…); default and per-game behavior UNKNOWN.
- No evidence was gathered on `D3D12_WORK_GRAPHS` dispatch semantics beyond encoder presence.

---

### 3.6 Cache / scheduling

**Observed**
- `D3DMStageCache::GetCachedResult` (decompiled, ev_18ac569b…) is a stage-indexed dispatcher over per-stage hash maps. Observed stage indices: 1 Vertex, 2 Fragment, 3 Hull, 4 Domain, 5 Mesh, 6 Amplification, 7 Geometry, 8 Compute, 0xF StreamOut, 0x10 StageIn, 0x11 Node; unknown/zero keys return null. (Raytracing is **not** in this switch.)
- Per-stage compile entry points: `CompileVertex`, `CompileFragment`, `CompileGeometry`, `CompileHull`, `CompileDomain`, `CompileMesh`, `CompileAmplification`, `CompileCompute`, `CompileStageIn`, `CompileStreamOut`, `CompileNodes`, `CompileRTFunction`, `CompileIntersectionWrapperFunction`, `CompileAndLinkStage`. Several compile signatures carry emulation/config parameters, e.g. `CompileVertex(bytecode, rootsig, bool, bool, IRStripCutIndex, bool, bool, int, int)` and `CompileFragment(bytecode, rootsig, bool, bool, u8, u8, u32)`. [ev_b4f3fbc8…]
- Distinct cache-key types exist per stage and per concern: `VertexStageCacheKey`, `FragmentStageCacheKey`, `HullStageCacheKey`, `DomainStageCacheKey`, `MeshStageCacheKey`, `AmplificationStageCacheKey`, `GeometryStageCacheKey`, `ComputeStageCacheKey`, `StageInCacheKey`, `StreamOutCacheKey`, `NodeStageCacheKey`, `RaytracingStageCacheKey`, and `EmulationCacheKey` — each with its own `Hash`/`Equal`/`convertToString`. [ev_b4f3fbc8…, ev_c692a73d…]
- Disk persistence: `D3DMCacheFile::Open/Close/View/Resize/Begin/End` plus `SerializeKey<StageCacheKey>(dispatch_queue, D3DMCacheFile&, IRShaderStage, key, D3DMStageResult*)` instantiations for Vertex, Fragment, Hull, Domain, Mesh, Amplification, Geometry, Compute, StageIn, StreamOut, Node, Raytracing keys. `D3DMStageCache::Open`, `EnableMemoryCache`, `Clear`, `GetCachedResult`, `Stage<Key>::lookup`, `lookupOrCreate`, `Stage<Key>::ValueWrapper::getValue/setValue`. [ev_b4f3fbc8…, ev_c692a73d…]
- D3D12-level cache APIs implemented: `D3D12Device::CreateShaderCacheSession` (ID3D12Device14_CreateShaderCacheSession, from `D3D12_SHADER_CACHE_SESSION_DESC`), `D3D12Device::ShaderCacheControl` (ID3D12Device14_ShaderCacheControl, from `D3D12_SHADER_CACHE_KIND_FLAGS` + `D3D12_SHADER_CACHE_CONTROL_FLAGS`), `D3D12PipelineState::GetCachedBlob`, and `D3D12PipelineLibrary` (Store/Load/Lookup/Serialize). [ev_b4f3fbc8…, ev_5a1f21dd…]
- Metal-side cache control is used directly: `MTLSetShaderCachePath` and `MTLMakeShaderCacheWritableByAllUsers` are imported from Metal. [ev_289e9f6f…, mach-o imports]
- Content-addressed hashing is available in-process: XXH32/XXH64/XXH3/XXH128 families are exported (`_XXH3_128bits*`, `_XXH128*`, `_XXH64*`, `_XXH32*`). [mach-o exports]
- Additional caching/scheduling infrastructure: `D3DMStageCache::CompileAndLinkStage`; `D3DMCompiler::CreateIRCompiler` with `IRShaderReflectionSerializeToBinary`/`DeserializeFromBinary` imports (reflection caching); a `d3dm_pipeline_cache.mm` translation unit; a `d3dm_pipeline_recording.mm` unit and `D3DMCommandQueue::AcquireMPLCommandReplayer`/`ReturnMPLCommandReplayer` (command-list recording/replay pool). [ev_adc73e27…, ev_289e9f6f…]
- Scheduling/threading primitives observed: dedicated worker thread (`pthread_create`, `pthread_setname_np`, `pthread_attr_set_qos_class_np`), `dispatch_semaphore_*`, `dispatch_apply`, `dispatch_source_*` timers, `os_unfair_lock*`, `std::condition_variable`, `std::recursive_mutex`, `std::__cxx_atomic_notify_one/all` + `os_sync_wait_on_address` (weak). [mach-o imports; ev_4bd5b2e1…]
- Optimization toggles: `MPL_ENABLE_CLEAR_OPTIMIZATIONS`, `MPL_ENABLE_RESOURCE_BARRIER_OPTIMIZATIONS`. [ev_65af6b5b…]

**Inference**
- The stage cache is a two-tier (in-memory hash map + on-disk `D3DMCacheFile`) content-addressed shader cache keyed by stage-specific keys derived from bytecode + root signature + emulation/emulation-affecting flags; `EnableMemoryCache`/`Clear` imply the memory tier is separately controllable.
- `SerializeKey<Key>` taking a `dispatch_queue` and writing to a `D3DMCacheFile` implies asynchronous background serialization of compiled stages.
- Because `GetCachedResult` omits the Raytracing key while `Stage<RaytracingStageCacheKey>::lookupOrCreate` exists, RT caching most likely goes through the `lookupOrCreate` path rather than the indexed lookup — this is an inference from the decompiled switch shape.

**Unknown**
- The on-disk cache format, key serialization encoding, and cache file location — UNKNOWN (no `D3DMCacheFile` body decompiled).
- Whether the XXH hashes are used for cache keys or only for pipeline-library/`D3D12UniqueObjectId` identity — UNKNOWN; no xref trace from the XXH exports was performed.
- Whether `D3DMStageCache` is shared across D3D12 and D3D11 paths and its concurrency model — UNKNOWN.
- Scheduler policy (queue priorities, number of worker threads, back-pressure) — UNKNOWN beyond the single-worker `D3DMCommandQueueWorker` shape.

---

## 4. Coverage limits (explicit)

1. **Static only.** Nothing was executed; no `observe_native_calls`, `capture_process_scenario`, or UI scenario was run. No runtime behavior is claimed.
2. **Provider.** All Ghidra-backed results come from Ghidra 12.1.4, ephemeral source-immutable import, `ghidra-default` analyzers, one Program per session. Pseudocode is Ghidra decompiler output, not original source, and is not text-equivalent to Hopper output. The original executable bytes were never written.
3. **Architecture.** The artifact is thin x86_64; there is no arm64 slice, so no native-ARM code path can be claimed from this file.
4. **Imports/exports completeness.** `inspect_macho` merges `dyld_info` and `nm`; its import list is explicitly **not exhaustive** (`total: null, exhaustive: false`) and may omit chained-fixup or toolchain-unsupported metadata. Export/procedure searches are Ghidra name-based and complete over the analyzed symbol inventory.
5. **Name-vs-behavior rule.** Symbols, source-unit names (`__GLOBAL__sub_I_d3d12_*.mm`), and environment strings were used only to locate entry points. Every capability statement above that is not marked INFERENCE/UNKNOWN is backed by a decompiled body or an exhaustive name-inventory operation with its Evidence ID.
6. **Not inspected.** `Resources/libdxcompiler.dylib`, `libmetalirconverter.dylib`, `libdxccontainer.dylib`, `libdxilconv.dylib`, `default.metallib`, `Resources/LICENSE`, and `_CodeSignature/CodeResources` were **not** analyzed — they are separate artifacts. Consequently, all DXIL→AIR / DXBC→DXIL *implementation* claims are limited to the import surface and to `D3DMCompiler::CreateIRCompiler`'s configuration calls; the converter internals are outside this inventory.
7. **Not decompiled (named only).** `D3D12Device::CheckFeatureSupport`, `DXGISwapChain::Present`, `D3DMCommandQueueWorker::DoPresent`/`EncodePresent`, `D3D12RaytracingStateObject::CreateRaytracingStateObject`, `D3D12CommandQueue::ExecuteCommandLists`, `MPLCommandList::BuildAccelerationStructure`, all `Compile*` bodies, all `Encode*` bodies. Claims about these are limited to their existence, address, signature, and call relationships.
8. **Session left open.** The REA target remains bound to `D3DMetal` (composite provider: ghidra+hopper+ida+native-macos+rea-artifact-graph+rea-dotnet-static). It was deliberately not closed because this appears to be a shared audit session; call `close_binary` when the audit finishes.
9. **Evidence paging.** Three large results were elided by the transport (`binary_overview` full, `inspect_macho` 1.5 MB, and the literal `D3D12` procedure search 898 KB). Retained at `artifact://20` (binary_session), `artifact://21` (inspect_macho), `artifact://22` (D3D12 procedure search), `artifact://23`–`artifact://25`, `artifact://28` for re-reading. Facts drawn from elided regions are cited via the smaller targeted operations listed in §2.

---

## 5. Bottom line for the DXMT comparison

- D3DMetal 4.0b2 is a single thin-x86_64 dylib exporting a complete D3D11 + D3D12 + DXGI + D3D10Blob + NVAPI + NGX surface, with D3D12 device coverage through **ID3D12Device14**, command list through **ID3D12GraphicsCommandList10**, and full state-object support for **collection / raytracing / executable (work-graph)** object types.
- Translation is DXBC→DXIL (libdxccontainer) → **DXIL→AIR** (libmetalirconverter) with ~20 explicit `IRCompiler` compatibility/emulation flags set per shader from `D3DMCompiler` state — this is the primary shader-translation surface to diff against DXMT.
- Optimization is concentrated in two places: (a) a **bitset dependency solver** in `MPLCommandList::CheckBarrierDependencies` plus **barrier coalescing/deferral with serial-decrement suppression** in `D3DMCommandEncoder::ResolveBarriers` (gated by `MPL_ENABLE_RESOURCE_BARRIER_OPTIMIZATIONS`, `MPL_ENABLE_CLEAR_OPTIMIZATIONS`), and (b) a **per-stage, two-tier content-addressed shader cache** (`D3DMStageCache` memory + `D3DMCacheFile` disk, 12 stage key types incl. raytracing and node/work-graph).
- Submission supports **two runtime-selectable Metal paths** — MTL4 (`MTL4CommitOptions`, `commit:count:options:`, `signalEvent:value:`, MTL4 event encode) and classic `MTLCommandBuffer` (`addCompletedHandler:` + `commit`) — chosen by a per-worker flag, with Metal residency sets (`D3DMDevice::ResidencySetsEnabled`) committed alongside.
- Presentation is a complete IDXGISwapChain4 implementation with HDR/colorspace, rotation, source-size, matrix-transform, frame-latency and overlay/HUD surfaces; MetalFX (spatial/temporal scaling, frame interpolation, denoise) is compiled in.

### Note on risk / what was skipped
I did not decompile the `Present`, `CheckFeatureSupport`, `ExecuteCommandLists`, `DoPresent`, `BuildAccelerationStructure`, or any `Encode*`/`Compile*` bodies, and I did not read the 29-entry barrier-layout table or any GUID table bytes; those are the concrete next REA calls if the audit needs per-feature *results* rather than entry points and configuration. The four bundled converter dylibs and `default.metallib` remain entirely unanalyzed, so no claim about DXIL→AIR lowering quality or Metal shader-library contents is possible from this inventory.
