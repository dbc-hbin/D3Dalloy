# D3D12 NGX → Metal 4 temporal integration

2026-10-04 현재 루트 소스의 **구현된 경로**를 설명한다. 2026-09-19의 “D3D12 NGX 없음/COM 경계 제안”은 이후 구현으로 대체됐다. 현재 API는 [frontend](D3D12-NGX-FRONTEND.ko.md), lifetime/subrect는 [contracts](NGX-CONTRACTS.ko.md), 실행은 [NGX](NGX-TEST.ko.md)·[temporal](TEMPORAL-TEST.ko.md) 문서를 따른다. 전체 상태는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md) 한 곳에서 관리한다.

## 현재 호출 경로

```text
NVSDK_NGX_D3D12_Init
  → ID3D12Device QI(IMTLD3D12DeviceExt) → SupportsTemporalUpscale
CreateFeature(open list, logical create parameters)
  → ValidateRecording → CreateTemporalUpscaleState
EvaluateFeature(open list, handle, frame parameters)
  → immutable frame descriptor → IMTLD3D12GraphicsCommandListExt::TemporalUpscale
  → PrepareOwnedTemporalFrame → retained TemporalUpscale encoder
Close / ExecuteCommandLists
  → FIFO submission task + recording/allocator lifetime protection
  → Metal 4 command-buffer encode → Metal4FX scaler → output rectangle copy
  → commit → GPU completion → in-flight frame/resource retirement
```

Evaluate는 caller list에 기록한다. 별도 native command buffer를 즉시 commit하거나 GPU completion을 기다리는 API가 아니다. `nvngx.dll`은 Metal/WMT/D3DMetal native pointer나 backend implementation layout을 소비하지 않는다.

## Project-owned COM ABI

권위 소스는 `src/d3d12/d3d12_interfaces.hpp`다. 이것은 NVIDIA public SDK 자체가 아니라 두 owned DLL 사이의 private 프로젝트 계약이다.

| Interface | UUID | IUnknown 뒤 slot 순서 |
|---|---|---|
| `IMTLD3D12TemporalUpscaleState` | `c8a9134e-f27c-4e75-909f-f14439b918e1` | opaque marker; 추가 public method 없음 |
| `IMTLD3D12DeviceExt` | `ed85c4e9-e8fd-491b-9595-d9a369ec12b2` | `SupportsTemporalUpscale`, `CreateTemporalUpscaleState`, `SupportsTemporalFeature` |
| `IMTLD3D12GraphicsCommandListExt` | `d7bc6ed0-6a8a-4a0e-8408-5a26c0b06d0e` | `TemporalUpscale`, `ValidateRecording` |

Create/frame descriptor의 첫 필드는 `UINT StructSize`이며 backend는 정확한 현재 크기를 확인한다. Create는 input/output logical extent, unmodified NGX flag bits, subrect opt-in, creation/visibility node mask를 전달한다. Frame은 D3D12 resource pointers, active input extent, base offsets, jitter, MV scale, semantic exposure와 caller reset을 전달한다.

현재 fixture의 layout assertion은 create40 bytes, frame x64 400/x86 348 bytes, Color offset x64 8/x86 4다. Pointer-bearing COM descriptor의 이 수치를 다른 guest ABI에 무조건 적용하지 않는다. Metal 4 guest/native fixed-width bridge POD와도 별개의 ABI다. Backend는 추가 DLL-private internal QI로 state를 검증하며 arbitrary IUnknown을 concrete state layout으로 재해석하지 않는다.

Public parameter store는 D3D12 resource의 `void*`/`ID3D12Resource*` Set/Get alias를 보존한다. Streamline이 Color/Output/Depth/MotionVectors를 `void*`로 저장해도 required-resource 조회가 이를 누락으로 취급하지 않는다. 숫자나 typed D3D11 resource를 D3D12 resource로 변환하지 않으며, missing key와 저장된 null은 구분한다. CPU 계약 회귀는 `scripts/test-owned-ngx-frontend.py --parameter-cpu-only --out <fresh-output>`으로 실행하고 공식 MSVC facade ABI 검사와 구분한다.

## Record-time resource·generation 준비

`src/d3d12/d3d12_temporal.cpp`의 Prepare는 resource를 resolve/retain하고 scalar·format·bounds를 검사한다.

- owned same-device 2D, single slice, single sample texture를 요구한다. Color/depth/MV/mask backing extent는 서로 같을 필요가 없으며 각각의 active subrect가 해당 view bounds 안에 들어가야 한다.
- output은 logical output + 선택 base를 수용해야 한다. Color/depth/MV와 output의 같은 resource alias는 거부한다.
- Color/depth/MV는 ShaderRead usage가 필요하다. Motion parent의 RG16 계열은 RG16Float, RG32 계열은 RG32Float typed view로 정규화한다. 다른 motion format은 E_NOTIMPL이다.
- manual exposure는 AutoExposure off일 때 optional null 또는1×1 R16Float/R32Float texture를 지원한다. R32는 매 execution/replay에서 선행 GPU writes 뒤 수치적으로 R16으로 변환한다. AutoExposure는 supplied exposure를 무시한다.
- native MetalFX descriptor에서 장치별 min/max scale과 usage를 조회한다. FSR의 max 초과 output은 centered uniform cap·opaque-black margin으로 처리한다. 고정[1,3] portable 계약이나 arbitrary device 지원이 아니다([현행 SR/FG 계약](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료)).

Scaler generation은 첫 유효한 record에서 준비하고 format·backing·output/layout 변경 시 교체한다. Old recording/replay는 자신이 retain한 generation·scratch·residency와 GPU-retirement lease를 사용한다. Active content·generation/activation·caller reset의 history 계약과 native scale 제한은 [현행 CORE 계약](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료)에만 유지한다.

Frame은 Core/Generation, retained app resources, logical-size private scratch output, residency와 immutable props를 소유한다. Scratch는 ShaderRead|ShaderWrite|RenderTarget usage다. Scaler는 scratch origin에 출력하고, blit은 logical rectangle만 caller output base로 복사한다. Larger backing의 바깥 pixel은 보존한다.

R32 conversion은 `d3d12_temporal.cpp`의 lazy Metal4 compute pipeline과 private retained R16 scratch를 사용한다. 기존 bridge/ABI를 그대로 쓰며 clamp·tone correction·CPU readback·record-time cache가 없다. Scratch/pipeline은 GPU completion까지 보호된다. [양 ABI numeric/replay·public NGX proof](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료)는 게임/image-quality equivalence와 별개다.

## Owned FSR public API와 provenance

`src/fsr`의 x64/WoW64 `amd_fidelityfx_upscaler_dx12.dll`은 AMD public **upscaler API4.0.3** header 기반 C facade다. [`fsr.def`](../src/fsr/fsr.def)의5 exports는 `ffxConfigure@1`, `ffxCreateContext@2`, `ffxDestroyContext@3`, `ffxDispatch@4`, `ffxQuery@5`이며 provider **`4d46580000000001`**/MetalFX를 보고한다. Backend는 위 owned temporal COM 경로를 재사용한다. Public AMD API를 소비한다는 사실은 AMD FSR 모델 실행·image-quality equivalence가 아니다.

SR의 dynamic render/output·masks·high-res/jittered MV·RCAS·linear/sRGB/PQ·manual exposure는 기존 temporal 경로다. FG는 원본 MIT AMD SDK pin `0836aa7f24058b3f8d035a88c0774457d78fc98e`의 source-built swapchain/UI/pacing과 기존 WMTMetal4FG native MetalFX generation을 사용한다. SDK 원문·local adaptations의 출처는 [`src/fsrfg/PROVENANCE`](../src/fsrfg/PROVENANCE)에 있다. 지원 범위와 역사적/public 증거는 [CORE SR/FG 계약](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료), 현행 native FG acceptance 보류는 [현재 경계](CORE-IMPLEMENTATION-STATUS.ko.md#multi-frame-fg와-native-residency-현재-경계)를 따른다. API query나 native 기능 존재를 실제 dispatch·presentation·품질 증거로 대신하지 않는다.

Headers의 read-only reference는 `/Users/hanbinnoh/Documents/zzz-wine-yaagl-pr-runtime/d3dmetal-pso-cache/third-party/fidelityfx/Kits/FidelityFX`다. 아래4 files는 reference와 byte-identical로 확인했으며 원문 notices/bytes를 유지한다. [AMD MIT license](../src/fsr/LICENSE)는 header에 적용되고 owned implementation의 SPDX는 LGPL-2.1-or-later다. Reference backend/D3DMetal/proprietary 구현 소스를 복사하지 않았다. Retained original NGX DLL은 **proprietary prebuilt binary**이며 source-copied 구현이 아니다. Frozen import lock/patches/provenance는 변경하지 않는다.

| `src/fsr/include/` 아래 public header | SHA256 |
|---|---|
| `api/include/ffx_api.h` | `6b23d642141cbbad01f22a9f2e44e0f9df51fbe00a7fd490b06d06ca7bd42967` |
| `api/include/ffx_api_types.h` | `68534c808a0b981b92b1381bc39d58e85ff3d47159c27afd6d0da3caefd0f5cb` |
| `api/include/dx12/ffx_api_dx12.h` | `6b0b377c43ebcb3c3b820008b3f05e5cf5e16f08ad41935eabcbe764970c6c88` |
| `upscalers/include/ffx_upscale.h` | `5d0fed5deaf7e243136c70dac117d6b74c632a6f2d144fa6c3ea30c08b6c7523` |

빌드는 `-Denable_fsr=true -Denable_nvngx=true`, 새 x64 staging은 `--with-fsr --with-nvngx`로 선택한다. Stage는 builtin marker·5 names/ordinals를 검사한다. [전용 smoke recipe](SMOKE-TEST.ko.md#전체-srpublic-fg와-shader-cache-전용-recipe)의 `--suite fsr`는 public AMD headers로 컴파일한 consumer를 사용한다. FSR 선택만으로 NVEXT/NGX를 요구하는 backend는 아니며 동시 stage는 두 public frontends의 회귀 입력을 갖추기 위한 선택이다.

## Submission·history·completion

`d3d12_command_queue.cpp`는 Execute 때 immutable recording entry/identity/allocator·recording lease를 task에 담고 GPU completion까지 protection을 유지한다. Caller list의 후속 Reset을 deferred worker가 다시 읽지 않는다.

Future queue wait는 FIFO worker의 submission을 qualifying logical completion publication까지 defer한다([queue 계약](QUEUE-ORDER.ko.md#queue-local-fifo와-public-api)). Public Wait/Execute 호출 thread를 producer GPU completion까지 막는 방식이 아니다. Temporal submission은 ring reservation 뒤 device temporal mutex를 얻고 CPU encoding부터 commit까지 유지한다. Ring capacity/GPU completion wait를 그 mutex 안에 두지 않는다. Feature history timeline을 D3D12 queue dependency와 양립하는 실제 submit 순서로 encode한다.

Metal 4 경로는 필요한 wait/signal 경계에서 physical submission을 나눌 수 있다. Trace 분석은 logical Execute parent와 physical submission을 구분한다. Temporal frame은 `InflightCommandBuffer`가 completion까지 retain한다. Frontend parameter Reset/Destroy, feature Release 또는 caller state release가 이미 기록된 frame의 resource/scratch를 해제하지 않는다.

Effective reset은 encode 시점에 다음과 같이 결정한다.

```text
effectiveReset = recordedCallerReset || lastExecutedGeneration != thisGeneration
```

새 generation 첫 execution은 reset, 같은 generation의 연속 replay는 caller false이면 no reset이다. G1→G2→G1→G1은 `1,1,1,0`, caller true replay는 매번 1이다. Record 횟수나 이미지 accumulation 추정이 아니라 `temporal_encode` trace의 caller_reset/reset을 비교한다([completion 검증](TEMPORAL-COMPLETION-TEST.ko.md)).

## Runtime publication

`stage-owned-graphics.py stage --with-nvngx`는 owned marker `own-ngxd3d12`와 required D3D12 exports를 검증하고, [실제 Authenticode 서명·prefix 한정 공개 인증서 등록](RUNTIME.ko.md#선택적-owned-nvidia-extension)을 포함한 DLL을 stage/hash한다. Wrapper는 `nvngx=b`를 사용하고 vendor extension 선택 시 `DXMT_ENABLE_NVEXT=1`을 내부에서 설정한다. NVNGX/NVAPI를 둘 다 선택하지 않으면 inherited NVEXT를 제거하고 vendor DLL fallback을 막는다. DXGI NVEXT setup의 `NGXCore\\FullPath` 등록만으로 실제 Streamline lookup이 증명되지는 않는다.

`nvngx_dlss.dll`의 feature-plugin exports와 core-style `nvngx.dll` API는 동일 surface가 아니다. Synthetic PE는 system32 core DLL을 직접 요청한다. 실제 앱의 loader 선택, Streamline adapter 판단, public NGX 호출, queue encode, 출력까지의 관찰을 별도로 요구한다. 게임 plugin을 static 추측만으로 교체하지 않는다.

## 증거와 남은 경계

Metal 4 temporal readback·replay·completion은 [CORE 검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)의 지정 fixture 증거다. 별도로 2026-10-09 ARM64 Wine/FEX 격리 ZZZ에서 일반 Streamline 로딩·NGX 입력 파싱·DLSS 도시 출력과 끄기/켜기 재생성을 [실제 관찰](ZZZ-COMPATIBILITY.ko.md#2026-10-09-owned-ngx-로딩과-dlss-검은-화면-복구)했다. 이를 다른 게임·모델 품질·프리컴파일 완료로 일반화하지 않는다.

- `temporal` COM fixture는 x64/WoW64 범위와 NGX frontend proof를 구분한다.
- `ngx`/`ngx-contracts`는 현재 x64 frontend 범위다. ARM64 Wine/FEX guest↔native NGX end-to-end proof가 아니다.
- flags74 synthetic 결과는 DLSS image quality·HDR transfer 동등성·physical compositor/HDR scanout·실제 게임 loader 선택을 증명하지 않는다.
- Full SR port는 input/output subrect offsets, masks, jittered/high-res MV와 RCAS sharpening을 지원한다. Alpha upscaling flag128은 alpha를 RGB로 복제한 독립 native temporal scaler에서 neutral exposure·동일 depth/MV/jitter/reset으로 재구성한 뒤 alpha만 병합한다. P1 Exposure.Scale·MV.Offset은 유효한 nondefault를 받아 수치 효과 없이 계속 실행하는 호환 계약이며 수식 구현이 아니다. 완료한 public x64 5쌍×3 replay와 backend x64/WoW64 회귀, pristine GPTK binary reference는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)에서 구분한다. Runtime usage/range는 장치에서 조회하고 HDR 의미/모델 품질·다른 장치 검증 경계는 [API-REVIEW](API-REVIEW.ko.md)를 따른다.

## Public non-SR feature inventory

NVIDIA public SDK defs와 June2026 FG/August2026 RR integration guide의 실제 enum·key를 따른다. SR 성공을 임의 feature 지원으로 일반화하지 않는다.

| ID | Public feature | 필요한 backend / 현재 계약 |
|---|---|---|
| 2 | InPainting | image-inpainting 합성 backend 없음; Available0, FeatureNotSupported |
| 3 | ImageSuperResolution | NGX still-image SR backend 없음; temporal SR alias 없음 |
| 4 | SlowMotion | NGX video sequence/time-domain slow-motion backend 없음; realtime FG alias 없음 |
| 5 | VideoSuperResolution | NGX video SR backend 없음; temporal SR alias 없음 |
| 9 | ImageSignalProcessing | NGX image signal-processing backend 없음 |
| 10 | DeepResolve | NGX DeepResolve backend 없음; ordinary SR alias 없음 |
| 11 | FrameGeneration | 기존 WMTMetal4FG → native MetalFX FrameInterpolator; 최대 count/index1, caller-owned 단일 output·Present/pacing |
| 12 | DeepDVC | NGX 색/채도 enhancement backend 없음 |
| 13 | RayReconstruction | 실제 Metal4FX TemporalDenoisedScaler; noisy color+diffuse/specular albedo+world normals+roughness+depth/MV+copied matrices; optional specular hit distance |

Reserved0/6–8/14–18, Count, SDK/Core/Unknown sentinel은 feature가 아니다. 미지원 enum의 Create/GetScratch/GetFeatureRequirements는 FeatureNotSupported다. RR availability의 공식 이름은 `SuperSamplingDenoising.Available`이다. RR/FG는 실제 native device capability를 조회하며 기존 MSVC parameter ABI·opaque handles를 유지한다.

공식 [NVIDIA/DLSS `3749594` public params](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/include/nvsdk_ngx_params_dlssg.h)의 `MultiFrameCount`·`MultiFrameIndex` 중 현재 지원은 count=1/index=1뿐이다. `DLSSG.MultiFrameCountMax=1`; higher count/index는 UnsupportedParameter, zero는 InvalidParameter다. `DLSSG.OutputInterpolated`는 해당 Evaluate의 단일 출력이며 AMD phase DXIL·multi-phase output bank는 없다. Shared FSR/NGX generation은 `d3d12_frame_generation.cpp`를 통해 기존 native MetalFX로 번역한다.

공식 optional `DLSSG.OutputDisableInterpolation`의 non-null 요청은 UnsupportedParameter다. Public MetalFX는 output texture를 쓰지만 generated-versus-copied actual-count/status API를 제공하지 않는다. Pristine GPTK도 이 key를 무시한다. Bootstrap/reset의 current-color output이나 encode 성공에서 flag/count를 꾸미지 않는다. SDK generated-role callback, native warm-up과 실제 physical display는 독립 관찰 대상이다. NGX caller가 output을 소비하고 Present/pacing을 소유하며 ordinary DXGI Present에 generated frame을 주입하지 않는다.

이미 기록된 native command는 resources/context를 GPU retirement까지 보유한다. Closed-list replay는 native 작업을 다시 encode하며 immutable-result/no-op replay가 아니다. Context metadata retirement는 기록된 GPU 작업을 취소하지 않는다. Native history reset, snapshot·queue ordering과 수명 보호는 번역 책임이며 별도 AMD scene-cut/actual-count classifier를 유지하지 않는다.

FSR의 presentation/UI/pacing은 원본 pinned MIT AMD SDK source-built 경로이며 x64/WoW64를 유지한다. Generated output 최대1개이고 FSR2–4 요청은 NO_PROVIDER다. SDK ordinary ResizeBuffers를 유지하고 ResizeBuffers1 success-only stub은 E_NOTIMPL로 바꿨다. Custom per-buffer queue 확장이나 새 async Present 오류 protocol은 없다. Root27/28/30/34의 AMD 알고리즘 proof는 역사적 기록이다. 현재 `build/local/fsr-metalfx-20261007-43df9ae7/runtime-verified`의 양 ABI FSR·NGX GPU·NGX Present 6개 실행이 API+Shader Validation으로 통과했다. FSR22/23(bootstrap1 skipped)·NGX formal9/9의 실제 표시와 한정된 genuine interpolation proof는 CORE를 따르며 실게임·native residency·Composition 증거와 구분한다.

RR은 unpacked roughness.R 또는 packed normals.w, view-space/HW depth, guide subrect를 기록한다. Row-major left-multiplication matrices는 Evaluate에서 복사해 caller mutation과 분리한다. RR guide §3.7에 따라 SR exposure/auto-exposure는 neutral이며 ordinary SR를 실행하지 않는다. 별도 native 입력이 없는 RR2 guide/reflection-MV, alpha와 sharpening은 UnsupportedParameter로 거부한다.
RR converter → native denoiser → output copy는 독립 native recording segments 사이의 queue producer barrier와 device visibility로 순서를 보장한다. Queue barrier의 `MTLStageAll`과 encoder-local fence scope는 구분하며, compute encoder의 fence update는 Dispatch/Blit/AccelerationStructure 범위만 사용한다. Fence·frame·scratch는 recording retirement까지 유지한다.
Feature release는 idle ownership만 내려놓으며 이미 기록된 generation의 history를 reset하지 않는다. Caller reset=false인 동일 frame의 재실행은 immutable 입력을 유지하면서 history를 한 단계 진행한다. 독립 control의 `replay` 모드는 native1..8 뒤 동일8을 reset=false로 다시 실행하며, frame8과 replay를 각각 비교한다. `.inputs`는 baseline/mirror의 uploaded guide bytes와 독립 literal SIMD matrix·jitter·reset이 public 입력과 byte-identical인지 검사한다. Wine x64와 비교할 native control은 `-arch x86_64`로 빌드하고 동일한 API+Shader validation 환경에서 실행해야 하며, ARM64 control은 별도 비교 자료다. `rotate`/`rotate-replay`는 이전 texture versions를 유지한 채 매 frame texture identity만 교체하는 독립 원인분리 control이다.

현재 RR 생성부와 독립 control은 private `setVersion:0x10000`으로 Apple non-fused Metal4 GPU backend를 선택한 뒤 public factory/encode를 사용한다. Control은 `requested_private_version=65536`을 별도 기록하며 기존 `source=public-metalfx-denoiser` marker만으로 public-only 설정이라고 해석하지 않는다. 선택자가 없으면 기존 Unsupported 경로로 거부한다. macOS27.0.1/M5 Pro의 native50 frame·양 ABI 각각36출력은 validation 오류0·native RGB 오차0으로 통과했다. [현재 proof](CORE-IMPLEMENTATION-STATUS.ko.md#rr-native-validation-재검증--2026-10-07)이며 아래 기본 fused 경로의 과거 실패 기록을 덮어쓰지 않는다.

Native control의 frame 로그는 encode 전 제출한 `reset`과 encode 후 getter의 `reset_after_encode`를 구분한다. Scene1·5는 reset=true를 제출하며, MetalFX가 request를 소비한 뒤의 getter=false를 제출값으로 해석하지 않는다.

2026-10-04 RR reference 대조는 `build/local/p4-20261004-N7DnYe/reference/D3DMetal.framework/Versions/A/D3DMetal`의 pristine GPTK4.0b2 x86_64 binary(SHA-256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`)를 대상으로 했다. `rr-evaluate-macho.disasm.txt`의 Metal4 MPL factory `0x3a291–0x3a46a`는 같은 public denoised descriptor·compiler factory를 사용하고 input/output dimensions를 직접 전달한다. `supportsMetal4FX:` query는 `0x31d63`, `ngx::convertMatrix`는 `0x16405b–0x1640a1`에서 64 bytes를 transpose 없이 복사하며 `rr-encode-selected.txt`의 matrix setters `0x12d596–0x12d610`도 그대로 전달한다. Pristine factory는 synchronous initialization=false, 독립 control은 true를 사용한다. 이 bounded static 대조는 reference binary의 해당 경로를 실제 실행했다는 증거가 아니다.

Main의 `build/local/p4-resume-20261004-WoVvSo/rr-native-arm64-validated` 실행에서 Apple M5 Pro의 128×128 1× texture preconditions와 queried scale range1..3이 확인됐지만, 32 frames 모두 native 내부 `Conv3x3Stride1U8` shader의 offset12672/length12672 invalid load를 보고했다. 별도 `rr-native-x86-validated`의 32-frame baseline과 18-frame history replay에서도 같은 fault가 관찰됐다. 따라서 이 환경의 fault는 Wine bridge나 Rosetta에만 국한되지 않는다. Native API 조건 충족·finite output·control completion은 shader validation PASS를 대신하지 않으며, 해당 결과로 RR acceptance 완료를 주장하지 않는다.

RR runner는 native baseline/replay의 stdout·stderr를 함께 freeze한다. Native shader validation fault가 있더라도 public 입력 hash·RGB 비교를 진단용으로 실행하지만, fault는 `rr_native_validation_errors`와 전체 `validation_errors`에 기록되며 결과는 반드시 FAIL이다. Missing/malformed/nonfinite oracle, 실행 증거와 exact RGB의 acceptance gate는 그대로 유지한다. 독립 control은 queried scale range와 128×128 1× private texture의 descriptor format·usage·device 조건을 확인한다.

FG는 normalized MV scale을 pixels로 변환하고 HDR을 display-ready PQ로 전달한다(FG guide는 scRGB/scene-linear 제외). 공개 SDK에 delta-time key가 없어 real QPC evaluate cadence를 snapshot한다. 추가 generated frames, UIR/distortion, nonzero input/output base, optional real/disable-interpolation output은 현재 명시적 UnsupportedParameter다. Record는 parameters/feature release 뒤에도 retained resource·native history 수명을 유지한다.

`owned-d3d12-ngx-features.cpp`는 public11/13/alpha128, exact capabilities, matrix/parameter snapshot, release-before-execute/replay를 검사한다. RR의 `public-rr-frame-N.bin`을 `owned-rr-native-control.mm`의 independent native output과 비교해야 한다. 구현 설명만으로 실행 전 fixture pass, NVIDIA neural-model 동등성, 게임 loader나 scanout을 주장하지 않는다.

2026-09-19 설계는 COM boundary, retained encoder, strict flags74 parsing, MSVC facade, staging, lifecycle/subrect 보정으로 구현됐다. Frozen patches/upstream lock은 역사적 provenance로 유지하며 현재 루트에 다시 적용하는 설치 지침으로 쓰지 않는다. ZZZ의 과거 static/실행 관찰은 [ZZZ-COMPATIBILITY](ZZZ-COMPATIBILITY.ko.md)에 날짜와 당시 runtime 경계를 유지한다.
