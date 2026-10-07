> Historical record, migrated on 2026-10-07. The original text below is preserved, not current instructions or current support. See [current priorities](../../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md).

# 병렬 구현 재개 결과 — 2026-10-05

## 최신 산출물 정리와 잔여 작업 목록

사용자 지시로 사용이 끝난 build/local 임시 prefix 148개와 과거 runtime 37개의 bin/lib/share만 제거했다. Build 247.4→57.5 GiB, disk available 68.5→258.8 GiB(증가190.3 GiB), 사용률93→72%다. 최신 runtime044와 comparison runtime034, 현재 build/install·ARM64 기반·소스/toolchains/frozen inputs·로그·결과·drawable captures를 보존했다. Runtime034는 WoW64 native proof가 PASS이며 x64 original false를 전체 양 ABI PASS로 바꾸지 않는다.

259 삭제 대상 부재와 보호 파일50개 hash 불변, 보존 runtime 두 개 stage verify PASS를 확인했다. [정확한 정리 manifest](../../build/local/cleanup-20261005/manifest.json)를 보존했다. Retired runtime root의 manifest/provenance는 증거로 남지만 bin/lib/share가 없어 실행할 수 없다. 과거 recipe의 runtime 경로를 그대로 재실행하지 말고 현재 source에서 fresh stage한다. 이번 정리는 production 수정·새 GPU acceptance가 아니다.

남은 구현은 Composition absolute alpha-colour, RR clean native validation, HDR10+ GAMING driver/wire의3개다. CoreWindow·필수 Wave64/128·상위 공개 SM/FL·상위 Agility·stereo·hardware plane·linked/multi-adapter는 실제 game/device 요구에 따른7개 조건부 항목이며 ARM64/FEX 실행은 보류를 유지한다. 검증-only와 기능 뒤 최적화 후보는 [현재 통합 목록](../../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md#현재-남은-구현-목록--2026-10-05-정리)을 따른다. 아래 재개 기록은 당시의 증거이며 삭제된 runtime payload의 현존을 뜻하지 않는다.

## 최신 DX11 구현 재사용 확인

사용자가 HDR과 나머지 blocked 기능에 DX11 구현을 재사용하도록 지시했다. Lead가 HDR을 확인하고 Composition/RR read-only scout를 병렬 실행했다. **DX12는 일반 HDR의 공용 Presenter·shader·native EDR 구현을 이미 사용한다. 새로 연결할 구현을 찾지 못해 production source는 변경하지 않았다.**

- HDR: `src/d3d12/d3d12_swapchain.cpp:387`의 Presenter 생성, `:1446`의 pending 색공간 적용, `:1682`의 metadata 전달은 `src/dxmt/dxmt_presenter.cpp`와 `src/dxmt/dxmt_command.metal`의 기존 HDR 경로를 사용한다. 공용 NVAPI display setting도 같은 Presenter에서 읽는다. DX11 `SetHDRMetaData`의 unconditional S_OK를 이식하지 않았다.
- Composition: `src/dxgi/dxgi_factory.cpp:224`는 D3D12 factory private interface가 없는 DX11 device에 DXGI_ERROR_UNSUPPORTED를 반환한다. DX11 HWND Presenter는 composition=false/alpha=Opaque 기본값을 사용한다. DX12의 alpha association과 residual transport는 이미 공용 Presenter/shader에 연결되어 있으므로 DX11에서 가져올 별도 합성 구현이나 독립 색상 기준은 없다.
- RR: `src/nvngx/nvngx.cpp:49`의 DX11 CreateFeature는 SuperSampling feature1만 처리하고 나머지는 실패한다. DX11 SR은 classic temporal scaler이며 RR denoiser가 아니다. DX12 RR은 이미 `winemetal_metal4_fx.mm`→`metalfx_rr.inc`→native denoised scaler에 연결된다. SR로 교체하거나 validation을 숨기지 않았다.
- HDR10+ GAMING: 공용 NVAPI/EDR 경로는 내부 HDR10/scRGB/PQ 설정이며 Gaming SSTM packet·native Gaming mode/readback 구현을 제공하지 않는다. 일반 HDR 재사용은 완료된 기반이고 이전 Gaming blocker를 닫는 구현은 아니다.

새로 빌드한 `owned-d3d12-hdr-metadata.cpp`의 x64/WoW64 실행은 runtime044에서 각각7frame·invalid4/unsupported2·set/replace/clear/resize·pending setter를 통과했다. Metal API+Shader Validation/foreign 오류0, exit0/pass=true다. 증거는 `build/local/dx11-reuse-20261005/{hdr-x64,hdr-x86}/result.json`; runtime manifest SHA256은 `2dd216a6390857d48c82ba84688c24795c471d4484d393341c19fc1b48e10009`다. 이 fixture의 GPU readback은 D3D12 source frame이며 tone-map drawable·최종 display 색상·Gaming wire의 새 proof가 아니다. CPU diagnostics95tests도5.682s PASS했다. 두 disposable prefix는 scoped wineserver -w 종료 뒤 제거했고 logs/result/fixture를 보존했다. Root build/stage와 Composition/RR GPU 재실행은 production delta가 없어 반복하지 않았다. 세 기능의 전체 acceptance는 여전히 미완료다.

## 최신 세 번째 재개: native shader 분석과 Unity·Unreal 웹 scout

사용자는 핵심 세 기능의 병렬 구현에 이어 scout로 Unity·Unreal 게임의 웹 구현을 찾도록 명시했다. Production 담당3개와 read-only 웹 scout3개를 실행했다. **이번 production 수정은 없고 세 기능 전체 acceptance는 미완료다.** 아래 resume2 기록은 과거 cut이다.

### Composition

공식 Microsoft Advanced Color 문서는 HDR DWM 및 해당 SDR Advanced Color 상태에서 linear scRGB FP16 CCCS 변환 후 합성한다고 명시한다. 이 조건부 domain은 이제 확보됐으며 legacy SDR·association/rounding/profile까지 자동 포함하지 않는다. Verified pristine GPTK4.0b2 factory0x105b94는 인자를 무시하는 unconditional E_NOTIMPL이다. Warnings-as-errors x86_64 probe의 두 native stub calls도 HRESULT0x80004001/NULL이다. Valid-device Windows COM consumer proof는 아니지만 이 factory는 composition 색상 oracle가 될 수 없다.

Scout와 lead가 Unity 공식 `HDROutput.hlsl::SceneUIComposition`과 URP `BlitHDROverlay.shader` 원문을 읽었다. UI unpremultiply→gamut rotation/paper-white scale→reassociate/source-over→OETF 순서다. **Gamut rotation은 transfer decoding이 아니다. Shader 본문만으로 sampled UI가 encoded domain이라고 단정하지 않는다.** Unity UI는 A=0 RGB 기여를 버리므로 DXGI additive-alpha0 oracle를 대체하지 못한다. Unity Windows reference white80/Metal100과 현재 presenter의 scRGB0.8 변환도 확인했다. 임의 gamma 변경은 하지 않았다. 다음 조건은 frozen stimuli의 qualified native Windows 결과 또는 association·FP16 rounding/clipping·profile/capture 독립 vector다. Native presentedTime과 optical scanout은 구분한다.

증거: `build/local/resume3-20261005-composition/{result.json,official-contracts.json,pristine-composition.c,pristine-result.log}`. Source: [Unity HDR output](https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.render-pipelines.core/ShaderLibrary/HDROutput.hlsl), [Unity URP overlay](https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.render-pipelines.universal/Shaders/Utils/BlitHDROverlay.shader), [Microsoft Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range), [Unreal HDR output](https://dev.epicgames.com/documentation/en-us/unreal-engine/high-dynamic-range-display-output-in-unreal-engine).

### RR

기존 rr.gputrace를 Xcode background controls로 실제 replay했다. Effect는 opaque public encode call86이며 내부 weights/pipeline은 trace에 없다. Replay validation 설정은 확정하지 않아 acceptance로 세지 않는다. MetalFX default.metallib의 uninstrumented AIR를 추출하고 public constant setters를 read-only 관측했다. Actual H/W16,K44,C16,NmatsHW/K2/2,inputIsQuantized0,blockMajorWeights0이며 원래 matrix guard7/32outputs/full-validation failure를 유지했다.

SIMD lane31,Ktile1,matrixK0,tap8,channel15,Kout44의 주소는 `2*(8*16*44 +15*44 +44)=12672`. 8-byte load는 `[12672,12680)`, buffer는 `[0,12672)`다. Aligned C16 branch는 load mask를 생략하고 K44 guard는 이후 postprocess/store만 보호한다. 초기 handwritten lane30은 lane31로 정정했고 lead의 `bun build/local/resume3-20261005-rr/access-witness.mjs`도 exit0이다. Same-library/actual-constants public MTLBinaryArchive는 nodispatch compile0, specialized AIR의 unmasked loads와 G17s compute5792bytes를 확보했다. Metal-objdump는 G17s decoder 초기화에 실패한다. Standalone archive가 actual framework pipeline과 byte-identical하거나 hardware transaction을 증명한다고 주장하지 않는다. **Shader 접근 전체 불가/ownership-only 진단은 최신 상태가 아니다.**

Web scout와 lead가 Apple26/27 release notes·Shader Validation docs·Streamline RR source를 대조했다. 확인한 공개 자료에 이 private K-tail kernel의 app-side correction은 없다. 다른 ray-tracing/MPP 문제의 workaround를 이 오류의 공식 fix로 세지 않는다. `zerofill`은 validation enabled에서 invalid read를0으로 바꾸는 동작이며 validation-disabled 출력의 안전성/무해성 proof가 아니다. Private padding/hook·guide 제거·validation suppression은 하지 않았다. 다음 조건은 vendor-corrected native kernel/MetalFX 또는 해당 kernel/compiler 수정 근거다.

증거: `build/local/resume3-20261005-rr/{result.json,constants-run/,kernel-disassembly.log,archive-air29.log,conv3x3.metalar,archive-g17s.log,access-witness.mjs}`. Source: [Apple Shader Validation](https://developer.apple.com/documentation/xcode/validating-your-apps-metal-shader-usage.md), [macOS27 notes](https://developer.apple.com/documentation/macos-release-notes/macos-27-release-notes.md), [Streamline RR](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md).

### HDR10+ GAMING

공식 Samsung UE5.7 sample636118872bytes의 directory57145bytes와 source/guide HTTP206 범위만 확보했다. Source42members의 inflate/length를 검증하고 relevant4files/Developer Guide V1.0.2를 보존했다. Assets 전체 다운로드와 개인정보 form 제출은 하지 않았다. Guide p31-32: APP는 shader가 이미 tone-map한 입력, GPU는 MaxCLL>display maximum일 때 driver scanout TMO를 구성·적용한다. 실제 plugin은 SetOutputMode(Gaming)→SetHdrToneMapping(GPU)→SetSourceColorSpace(REC2100) 및 SetSourceHdrMetadata만 호출하고 자체 Gaming packet/TMO는 없다.

Guide p33 peak index0 reserved,1=200nits(projector only),2..15=300/400/500/600/800/1000/1200/1500/2000/2500/3000/4000/6000/8000nits를 확보했다. Full-frame10/20/40/80%는 libdisplay-info 공개 구현 근거이며 normative GPU curve는 아니다. Calibration table 전체 부재라는 이전 표현은 정정한다. CTS p34는 SSTM VSIF 최소 매2frame·ALLM1·maxRGB≤EDID peak를 physical analyzer로 검사한다. **Mandatory sink ACK protocol은 이 근거에 없어 완료 조건으로 고정하지 않는다.** Native driver control/API readback/실제 packet 전달 proof와 구분한다.

Scout가 Xilinx HDMI Forum VSIF ALLM bit1/mask0x02와 static HDR/일반 HDR10+ video builders를 찾았고 lead도 source를 읽었다. ALLM announcement는 Gaming SSTM payload/ACK가 아니며 HDMI SBTM·일반 HDR10+ video·Gaming SSTM은 대체할 수 없다. 이 source로 Apple IOAV32byte Gaming ABI를 추측하지 않았다. 다음 조건은 supported Gaming payload/source protocol과 macOS Gaming/LowLatency/SourceTM driver consumer·mode/ownership/복구다. APP/GPU 요구를 분리하고 cache/parser만으로 feature를 만들지 않았다.

증거: `build/local/resume3-20261005-hdr/{contract-result.json,vendor-source/,vendor-guide-extract.txt,calibration-table-ocr.txt,cts-source-requirements.txt,ioav-query-disassembly.txt}`. Source: [Samsung sample](https://www.fab.com/listings/82da4a90-e93b-47a8-b001-28c763265b80), [CTS](https://www.unigraf.fi/app/uploads/2023/06/HDR10-CTS-User-Manual.pdf), [Xilinx VSIF](https://raw.githubusercontent.com/Xilinx/embeddedsw/master/XilinxProcessorIPLib/drivers/v_hdmi_common/src/xv_hdmic_vsif.c).

### 검증과 정리

담당자3개와 웹 scout3개는 terminal이며 pending GPU/display work는 없다. Native constants/archive processes는 정상 종료했다. Xcode workload/새 trace window/Cua session을 종료하고 Xcode app process 자체는 남겼다. Foreground escalation은 하지 않았다. Production delta가 없어 root build/stage·기존 CPU95/alpha104/public acceptance는 반복하지 않았다. 새 probe compile/runtime·archive·PDF/OCR·AIR arithmetic checks는 위 증거를 따른다. Clean RR validation/세 기능 production 완료/새 게임 호환성은 주장하지 않는다. Commit은 만들지 않았다.

## 이전 두 번째 재개 기록

최신 사용자 지시: `.omo의 handoff 문서와 문서상태들을 확인하고 병렬로 구현 계속해줘.`
이 지시로 이전 사용자 중단을 해제하고 Composition/RR/HDR 세 담당자를 동시에 실행했다.
원래 계약과 frozen 입력은 [원래 handoff](remaining-implementation-20261005.md),
이전 runtime044 cut은 [중단 snapshot](parallel-implementation-stop-20261005.md)을 따른다.
이번 결과는 기능 전체 완료나 새 사용자 중단 지시가 아니다. 남은 구현은 아래 외부 계약에 blocked다.

## 실제 변경과 실행 방식

- **Production 변경 없음.** 현재 근거로 semantics-preserving source correction을 입증하지 못했다.
- QA harness `build/local/ulw-resume-20261005-composition/capture-events.mjs`만 수정했다.
  이미 종료된 scoped wineserver의 `-k` exit1을 정리 실패로 세던 결함을 고쳤다.
  `-k`만0/1을 허용하고 `-w`, fixture/capture 명령의 exit0 요구는 유지한다.
- 기존 dirty worktree와 frozen fixture/source를 보존했다. Commit/build/install/stage는 하지 않았다.
  Production delta가 없어 기존 runtime044를 재사용했다.
- CPU 분석은 병렬, GPU/display lease는 RR capture → alpha 양 ABI → RR full-validation 순서로 배정했다.
  각 담당자가 자신의 실행·캡처·정리를 맡아 lead 실행 병목을 줄였다.

## Composition: alpha 양 ABI 전체 backing 검증

각 ABI consumer cases7/gpu14 PASS, ordered READY/ADVANCED13, premul/straight×2round로52PNG를 얻었다.
기존 inspector/tolerance를 유지한 geometry 검사 총104/104가 통과했다.
Identity부터 content-lifetime까지 matrix/crop/resize/commit/detach/queued/lifetime 단계를 포함한다.
Actual Metal API/GPU Validation enabled, graphics validation/foreign backend 오류0, owned marker와
runtime044 winemetal 및 ABI별 d3d12core 실제 load를 확인했다. 두 캡처를 lead도 직접 확인했다.

x64 첫 harness는 consumer 성공 뒤 `-k` exit1만으로 실패했다. 원본 failure JSON을 보존했고,
같은 prefix의 독립 `-w` exit0과 disposable 정리를 확인했다. 수정 후 i686 harness는 exit0이다.
x64 actual-prefix d3d12/dcomp hashes는 staged와 일치한다. i686 actual-prefix hashes는 teardown
전에 수집하지 못했다. i686 staged hashes/load paths/owned marker와 prefix hash equality를 혼동하지 않는다.
기존 Wine 초기화 오류15개는 이전 runtime044와 같으며 숨기지 않았다.

양 ABI association 동등성과 premul alpha0 additive 기여를 관측했다. Half samples는
`[76,139,167,255]`, premul alpha0는 `[173,255,0,255]`, straight/background는 `[0,255,2,255]`다.
이는 절대 Windows alpha-colour parity가 아니다. 같은 UNORM8 입력의 encoded source-over와
linear-light source-over scalar 결과는60code까지 다르며 owned output로 oracle를 선택하지 않았다.
공개 계약과 현재 workspace에는 calibrated DComposition transfer/quantization/profile oracle나
승인된 native Windows reference가 없다. 임의 gamma/CAFilter 변경은 하지 않았다.
Optical panel scanout, capture freshness의 독립 proof, normative colour는 여전히 미검증이다.

증거: `build/local/resume2-20261005-composition/{findings.json,geometry-x86_64.json,geometry-i686.json,alpha-x86_64/,alpha-i686/,blend-transfer.mjs}`.
재실행 명령:

```sh
bun build/local/ulw-resume-20261005-composition/capture-events.mjs alpha x86_64 build/local/runtime-044-composition-clean build/local/resume2-20261005-composition/alpha-x86_64
bun build/local/ulw-resume-20261005-composition/capture-events.mjs alpha i686 build/local/runtime-044-composition-clean build/local/resume2-20261005-composition/alpha-i686
```

위 output은 이미 존재하므로 새 실행은 반드시 fresh output 이름을 사용한다.
다음은 같은 frozen stimuli의 matched native Windows reference capture 또는 독립적으로
qualified transfer/quantization oracle 확보다. Geometry104/104를 normative acceptance로 세지 않는다.

## RR: 새 capture와 full-validation 관측

Shader Validation disabled인 진단 capture는 API Validation을 유지하고 one-frame `rr.gputrace`를 생성했다.
별도 full-validation observer는 동일 `Conv3x3Stride1U8` offset12672/length12672 invalid load32회를 재현했다.
양 실행은 원래 matrix_sensitive_frames7 guard와32outputs를 유지했다. Exit0은 clean acceptance가 아니다.

Weights는 MetalFX `Conv3x3Stride1.createBuffers/newBufferWithBytes`가 생성한 `_bufWeights`12672bytes다.
`mfx::enc::bindConvStandard+154`, argument1/offset0, grid512×1×1/group64×1×1을 확인했다.
Owned preprocessing params3328bytes나 Wine/NGX guide producer가 아니다. Public descriptor에 internal
weights/backend selector는 없으며 기존 초기화 YES/NO·classic/Metal4·arm64/x64 controls도 실패했다.
진짜 native kernel OOB와 Shader Validation instrumentation 문제는 아직 구분하지 못했다.
Trace는 캡처했지만 replay/shader-disassembly하지 않았다. 다른 OS/MetalFX build 또는 실제 native
shader/access 근거가 다음 조건이다. Padding/guide 제거/error suppression은 채택하지 않았다.

`observe01`은 오래된 capture binary의 미지원 observe 인자로 GPU 이전에 실패했다.
원본을 보존하고 지원되는 별도 observer/fresh `observe02`로 검증했다.
Pristine GPTK4.0b2 hash/signature와 bounded factory path를 재확인했지만 pristine 실행 parity는 없다.

증거: `build/local/resume2-20261005-rr/{result.json,capture01/rr.gputrace,capture01/,observe01/,observe02/}`.
미완료: clean native Shader Validation, public x64/i686 RR acceptance, kernel/validation 원인 구분.

## HDR10+ GAMING: transport 존재와 missing Gaming 계약 구분

새 readonly native probe는 clang warnings-as-errors build0/runtime0이며 NSScreen getter와12 export lookup만 실행했다.
`IOAVServiceStartInfoFrame[WithSource]`는 실제32-byte input과 source scalar를 selector0x11로 전달한다.
Stop은0x12, static HDR metadata는0x1e다. Native transport 전체 부재라는 주장은 하지 않는다.
그러나 확인한 wrapper/public SDK는 Gaming payload, reference/low-latency ACK, sink readback,
exclusive output ownership/restoration 계약을 제공하지 않는다. Generic transport만으로 Gaming을 구현할 수 없다.

현재 external/builtin reference headroom은0, potential EDR은 각각6.03665781/16이다.
이는 현재 reference-rendering mode 관찰이며 하드웨어 영구 불가능 판정이 아니다.
CoreDisplay Boolean HDR state와 HDR10 getter는 SDR/HDR10/Gaming 삼분 readback이 아니다.
Pristine GPTK4.0b2 static path에서도 HDR10/Gaming 요청은 PQ color space로 합쳐지고 getter는 HDR10이다.
Working Gaming oracle로 세지 않는다. 미확인 private DCP 경로의 부재까지 단정하지 않는다.

증거: `build/local/resume2-20261005-hdr/{contract-result.json,reference-transport.log,identity.log,iokit-disassembly.log,coredisplay-disassembly.log}`.
다음 조건: supported Gaming payload/driver ACK/readback, calibrated SSTM vectors, system-wide output owner acquire/query/release 계약.
기존 VSVDB0/preset0/calibrationnull hardware gate는 별개이며 EDID 재조사는 하지 않았다.

## 통합 검증과 정리

Lead `python3 scripts/test-owned-graphics-diagnostics.py`: **95tests PASS**,6.077s.
이는 CPU parser/gate 회귀이며 GPU/feature proof가 아니다.
Production 수정이 없어 새 root build·staging 및 이미 통과한 cadence/일반 Presenter 회귀는 반복하지 않았다.
각 GPU/display lease는 해제했고 alpha scoped wineserver retirement 및 prefix/cache/control 제거를 확인했다.
사용자/HWND geometry는 바꾸지 않았다. Source/log/JSON/PNG/trace와 runtime044를 보존했다.
조건부 CoreWindow/Wave64/128/상위 SM/Agility/stereo/overlay/multi-adapter의 새 trigger는 없으며,
ARM64/FEX·WARP/video/D3D11·다른 Wine 저장소로 확장하지 않았다.
