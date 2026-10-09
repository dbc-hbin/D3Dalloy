# 구현·검증 증거 색인

기준일: **2026-10-09**. 현재 지원·계약은 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md), 남은 작업은 [PRIORITIES](IMPLEMENTATION-PRIORITIES.ko.md), 최신 수정은 [10월7일 리뷰 implementation ledger](../doc/D3D12-REVIEW-AND-DESIGN-2026-10-07.md#follow-up-implementation-and-evidence)를 따른다. 이 문서는 재사용할 과거 증거와 한계만 남긴 색인이다. 아래 과거 결과를 새 build·GPU·게임 실행 결과로 재분류하지 않는다.

## 증거를 읽는 기준

- PASS는 해당 fixture·ABI·manifest·환경의 결과다. Mixed-cut 결과를 최신 단일 cut 전체 회귀, Windows/HLK conformance, 게임 호환성·FPS로 합치지 않는다. FAIL·INCONCLUSIVE·미실행은 이후 수정이 있어도 원본 결과를 바꾸지 않는다.
- **2026-10-02 기존 build/ 전체가 외부 작업으로 삭제됐다.** 삭제 전 P0–P3 원본 runtime/log/JSON은 현재 보유 증거가 아니다. [Retired evidence provenance](../doc/provenance/RETIRED-BUILD-EVIDENCE.md)를 따른다. 이후 recovery/review/SDK05 증거는 별도 실행이다.
- [10월5일 cleanup manifest](../build/local/cleanup-20261005/manifest.json)는 prefix148개·obsolete runtime37개의 payload 정리를 기록한다. 남은 manifest/로그는 실행 가능한 runtime을 뜻하지 않는다. 재실행은 현재 source의 fresh build/stage와 새 output을 사용한다.
- **2026-10-09 현재 보유 범위:** 디스크 정리 뒤 `build/local/`에는 `zzz-ngx-enrollment-20261009`, `zzz-dlss-fix-20261009`, `zzz-fex-research-20261009`, `zzz-fex-opt-20261009`만 남아 있다. 아래 다른 root의 PASS/hash는 당시 기록이며 현재 보유한 원본 결과나 새 재검증이 아니다. 로컬 문서 검사는 없는 build 증거 링크를 별도 집계한다.
- 긴 ledger와 삭제한 handoff/review의 원본을 보관했던 `build/local/repo-cleanup-20261008/originals/`도 현재 없다. 이 문서의 요약을 그 원본 전체가 보존돼 있다는 주장으로 읽지 않는다.

## Native ARM64/FEX 실제 실행 재개 — 2026-10-06

**현재 판정:** ARM64/FEX는 보류가 아니다. [현재 native migration 범위](../doc/ARM64-FEX-IMPLEMENTATION.md)는 optimized Wine11.17·GnuTLS/FreeType closure와 G8 matrix를 구분한다.

SDK05의 동일 manifest `c59930623bc44b5120d6a74534c4d0be2b5704ecf910a67e252b23e4729453dc`에서 [ARM64 ABI](../build/local/arm64x-wow64-sdk05-arm64-abi-gpu-20261006/result.json), [x64 ABI](../build/local/arm64x-wow64-sdk05-x64-abi-gpu-20261006/result.json), [x86 ABI](../build/local/arm64x-wow64-sdk05-x86-abi-gpu-20261006/result.json)와 [ARM64 smoke](../build/local/arm64x-wow64-sdk05-arm64-smoke-20261006/result.json), [x64 smoke](../build/local/arm64x-wow64-sdk05-x64-smoke-20261006/result.json), [x86 smoke](../build/local/arm64x-wow64-sdk05-x86-smoke-20261006/result.json)가 PASS였다. COM owner/scalar·callback·Unix-call111·Metal blit/fence와 각8-frame exact graphics/compute readback, loaded hashes·actual child no-Rosetta·validation0을 확인했다. **No-present/선정 SM50-DXBC smoke이며 game/presentation/full DXIL/G8 acceptance는 아니다.**

Compatibility 경계는 16KB native PE alignment, ARM64X native/EC TLS view fixup·Core/Main generation, ordinary emulated x86 image의 host EXEC와 Windows EXEC 구분, FEX nested JIT write scope, COM sret thunk identity 및 PE32 CPU pointer의 guest-window encoding이다. Native handles/GPU addresses를 CPU pointer처럼 재인코딩하지 않는다. [JIT03 before-main FAIL](../build/local/arm64x-mr11638-fex-jit03-d3d12-smoke-20261006/result.json), [native entry timeout](../build/local/arm64x-wow64-window01-native-entry-20261006/result.json), [SDK05 paired result](../build/local/arm64x-wow64-owned-sdk05-20261006/result.json)을 구분한다. 후속 native x86 FX pointer 수정·frontend proof는 10월7일 리뷰/CORE가 우선하며 SDK05 smoke에서 drawable/전체 FX를 입증했다고 읽지 않는다.

## Composition 재개와 병렬 진단 — 2026-10-05

**현재 판정:** geometry/cadence/lifetime는 지정 증거가 있고 absolute alpha/colour는 미완료다. [양 ABI backing findings](../build/local/resume2-20261005-composition/findings.json), [x64 geometry](../build/local/resume2-20261005-composition/geometry-x86_64.json), [x86 geometry](../build/local/resume2-20261005-composition/geometry-i686.json)는13phase·104/104 backing captures를 기록한다. x64 최초 harness는 consumer 성공 뒤 scoped wineserver `-k` exit1 때문에 FAIL였고 원본은 그대로다. i686 staged hashes/load paths는 확인했지만 teardown 전 actual-prefix hash equality는 수집하지 못했다.

Runtime044의 [cadence/Sync3/mixed observation](../build/local/ulw-resume-20261005-observation/)은 frozen public12/12와 별도 Sync3 public3/3, 두 plane의 native paired times, WindowServer 전달 및 mixed6phase backing geometry를 기록한다. 화면이 가린 mixed tail과 backing capture, presentedTime/WindowServer 전달과 optical panel scanout을 혼동하지 않는다. 이전 runtime034 x64 original false·Root35/36 warmup 실패는 FAIL로 남고 runtime043/044 후속 proof가 이를 현재 실패로 되돌리지 않는다.

[Source/reference 조사](../build/local/resume3-20261005-composition/result.json)에서 Microsoft Advanced Color의 조건부 linear scRGB FP16 domain을 확인했다. Unity UI의 unpremultiply→gamut/paper-white→reassociate/source-over→OETF 순서는 DComposition pixel oracle가 아니며 A=0 RGB 기여를 버린다. Gamut rotation은 transfer decoding이 아니다. Pristine GPTK4.0b2 Composition factory의 unconditional E_NOTIMPL/native stub은 Windows valid-device consumer proof나 색상 oracle가 아니다. Encoded/linear blend가 최대60code 차이나는 관찰에서 owned output에 맞춰 gamma를 선택하지 않았다. Association·FP16 quantization/clipping·profile/capture의 qualified 독립 기준과 물리 색상 proof가 남는다.

## RR: 실패 증거와 이후 해소

**현재 판정:** [10월7일 native validation 재검증](CORE-IMPLEMENTATION-STATUS.ko.md#rr-native-validation-재검증--2026-10-07)이 과거 blocker를 해소했다. Private version0x10000의 Apple non-fused 경로에서 독립 native50frame 및 양 ABI 각각36출력 RGB 오차0·API/Shader Validation0·입력 동등성이 PASS다. 다른 OS/GPU의 private 선택자 계약·실제 게임 loader·화질/성능은 별도다.

과거 [classic YES/arm64](../build/local/rr-classic-20261004.uLqdtY/validation-verdict.txt), [classic NO/x64](../build/local/rr-classic-async-20261004.JXBn3w/validation-verdict.txt), [Metal4 NO/x64](../build/local/rr-m4-async-20261004.yycbQA/validation-verdict.txt)는 **FAIL**이다. Baseline32/replay18의 Conv3x3Stride1U8 invalid load는 framework-created weights 길이12672와 offset12672가 같았다. Finite·non-copy·matrix-sensitive output/exit0은 clean validation PASS가 아니었다. [Capture/observer](../build/local/resume2-20261005-rr/result.json)와 [constants/AIR witness](../build/local/resume3-20261005-rr/result.json)는 actual K44/C16, lane31의8byte K-tail 접근 [12672,12680)를 기록한다. Specialized archive AIR는 actual framework pipeline/hardware transaction의 byte-identical proof가 아니다. Validation zerofill은 validation-disabled 안전성 proof가 아니며 guide 제거·padding·suppression으로 실패를 닫지 않았다. 이 분석을 현재 RR 미해결 판정으로 재사용하지 않는다.

## HDR10+ GAMING: 유지할 compatibility 조사

**현재 판정:** 일반 HDR10/scRGB/PQ·EDR는 구현된 기반이며 Gaming SSTM/driver/readback 계약은 미완료다. DX11은 같은 Presenter/HDR 경로를 쓰고 별도 Composition/RR 구현이 없다. [HDR metadata x64](../build/local/dx11-reuse-20261005/hdr-x64/result.json)·[x86](../build/local/dx11-reuse-20261005/hdr-x86/result.json)의7frame PASS는 D3D12 source readback이지 tone-map drawable/Gaming wire/최종 색상 proof가 아니다.

- [Native/EDID 계약](../build/local/hdr-gaming-20261004-native-readonly/contract-findings.json): Samsung S32B80P(SAM29171, serial810300994)의 genuine256byte EDID SHA256 `112933369a46b8604e3289d66575ebb7ae3ea043060ec92f6d565040bef288aa`는 PQ/정적 HDR을 광고하지만 HDR10+ VSVDB0이다. 모든 firmware/input mode의 영구 하드웨어 미지원 선언이 아니다. External preset0·calibration null과 내부1000nit HDR Video profile은 Gaming reference mode/저지연/측정된 sink 한계가 아니다.
- [Generic native transport](../build/local/resume2-20261005-hdr/contract-result.json): IOAV StartInfoFrame[WithSource]는32byte input/source를 selector0x11로, stop은0x12, static HDR는0x1e로 전달한다. Transport 전체 부재는 아니지만 Gaming payload·driver consumer·mode readback·system-wide owner/restore 계약을 입증하지 못했다. CoreDisplay Boolean HDR/HDR10 getter와 pristine GPTK의 Gaming→PQ/HDR10 collapse/no-op은 working Gaming oracle가 아니다.
- [Samsung source/guide/calibration](../build/local/resume3-20261005-hdr/contract-result.json): APP는 이미 tone-map한 입력, GPU는 MaxCLL>display maximum에서 driver scanout TMO를 적용한다. Plugin은 Gaming/GPU/REC2100/source metadata를 호출할 뿐 자체 packet/TMO 구현을 제공하지 않는다. Peak index0 reserved,1=200nit(projector),2–15=300/400/500/600/800/1000/1200/1500/2000/2500/3000/4000/6000/8000nit다. Full-frame10/20/40/80%의 공개 근거는 normative GPU curve가 아니다. CTS의 SSTM VSIF 최소 매2frame·ALLM1·maxRGB≤EDID peak 요구와 native driver control/readback을 구분한다. **Mandatory sink ACK는 확인된 protocol 요구가 아니다.**

ALLM·SBTM·일반 HDR10+ video·DXGI_HDR_METADATA_HDR10PLUS의 opaque payload는 Gaming SSTM과 대체 관계가 아니다. Supported Gaming payload/SSTM, calibrated GPU tone-map vectors, actual suitable output과 single-owner mode acquire/readback/release·teardown/hotplug 복원이 남는다. APP의 SSTM 결과에 EETF를 다시 적용하지 않는다. Cache/parser/getter echo를 기능 완료로 세지 않는다.

## 후속 개발의 증거 색인

### ZZZ 실제 precompile 진단과 자동 PSO 캐시 재스캔 — 2026-10-09

실제 precompile의 root-signature binding 원인 확정과 자동 PSO disk 회계 재스캔 제거. 계약·수치는 [ZZZ 관찰](ZZZ-COMPATIBILITY.ko.md#2026-10-09-실제-precompile-경로-진단과-자동-pso-캐시-재스캔)과 [CORE 자동 PSO 절](CORE-IMPLEMENTATION-STATUS.ko.md#자동-pso-컴파일-캐시와-focused-proof)에 둔다.

- 원본 실패64쌍은 owned와 pristine GPTK4.0b2에서 모두 거부된다([binding 진단](../build/local/zzz-dlss-fix-20261009/precompile-run-01/binding-diagnosis/result.txt), [replay 전량](../build/local/zzz-dlss-fix-20261009/precompile-run-01/replay/all/aggregate.json)). Unity serializer가 실제로 SRVcount8 root를 생성하고([warmup caller](../build/local/zzz-dlss-fix-20261009/precompile-run-01/warmup-caller/result.txt)) 실패 HRESULT는 무시된 채 job이 계속 진행한다. Compiler validation 완화·fake PSO는 없다.
- 자동 PSO cache는 매 publication마다12k 파일을 directory mutex 아래 재스캔했다.16-byte named shared 회계와 조건부 rescan으로 수정했고([before](../build/local/zzz-dlss-fix-20261009/precompile-run-02/cache-regression/before-final/run.log)·[after](../build/local/zzz-dlss-fix-20261009/precompile-run-02/cache-regression/after-final-03/run.log)), 실게임 fs_usage [run-02](../build/local/zzz-dlss-fix-20261009/precompile-run-02/profile-analysis/summary.json)·[run-03](../build/local/zzz-dlss-fix-20261009/precompile-run-03/profile-analysis/summary.json)는 서로 다른 workload다.
- 최종 [리소스 검사 완료 화면](../build/local/zzz-dlss-fix-20261009/precompile-run-03/progress-during-evidence-update.png)으로 이 흐름의 실제 종료를 확인했고, 완료 뒤 [최종 candidate 도시 출력](../build/local/zzz-dlss-fix-20261009/precompile-run-03/dlss-city-output.png)이 DLSS·성능 우선·FG끄기·RT끄기 설정에서 렌더링됐다. 실행 identity·QA·한계는 [result.json](../build/local/zzz-dlss-fix-20261009/precompile-run-03/result.json)에 있다. 최신 retained payload는 `builds/cache-04/runtime`이며 이전 runtime payload는 retired다. Controlled FPS·frame-time·장시간 stutter는 미측정이다.

### Multi-frame FG와 native borrowed residency — 2026-10-05

**현재 판정:** CORE/PRIORITIES의 native MetalFX generation 최대1개 계약이 우선한다. AMD N1–4 직접 phase 알고리즘은 제거됐으며 옛 proof를 현재 N1–4 지원으로 읽지 않는다. [Root25 native-fault](../build/local/multi-fg-recording-state-20261005/gpu-gc25-native-fault-x64/native-fault-evidence.json)는 borrowed residency traversal 수명 결함, [Root27 GC96 x64](../build/local/multi-fg-recording-state-20261005/gpu-gc27-x86_64/stdout.log)·[x86](../build/local/multi-fg-recording-state-20261005/gpu-gc27-i686/stdout.log)와 Root28 private scope replay proof는 bounded 수명/state 복구다. Root31 x64/Root34 WoW64 full23 native mixed-cut와 Root34 strict public clock는 같은 Root34 전체 양 ABI native PASS가 아니다. Root34 x64 N1 seq2 skipped/original runner false, Root30 WoW64 original false의 saved-capture offline 재검사는 새 GPU 실행이 아니다. [현재 named ledger](CORE-IMPLEMENTATION-STATUS.ko.md#multi-frame-fg와-native-residency-현재-경계)에 정확한 cut/consumer를 둔다.

### P2 #13 conservative Tier3 — 2026-10-04

[Before Tier1 FAIL](../build/local/conservative-tier3-20261004.uFBr69/before-x64-02/result.json)의 post-snap line은 Tier1에서 허용되는 cull이며 Tier1 bug/oracle 오차가 아니다. [Provisional](../build/local/conservative-tier3-20261004.uFBr69/provisional-x64-01/result.json)은 GPU rows를 통과했지만 cap1 terminal 때문에 exit1/pass=false다. Promotion 후 WoW64의 back-face/DepthClip 실패는 DATA32 mirror/copy 누락으로 수정했다. [Final x64](../build/local/conservative-tier3-20261004.uFBr69/runtime03-conservative-x64/result.json)·[x86](../build/local/conservative-tier3-20261004.uFBr69/runtime03-conservative-x86/result.json)는 cap3 PASS다. [CORE의8회](CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof)는 runtime03의5회+변경 없는 runtime02 x64 risk3회인 mixed-cut다. Pristine GPTK Tier0 query는 정적 reference일 뿐 owned/GPTK 실행 proof가 아니다.

### P2 #12 depth bounds — 2026-10-04

[Native probe](../build/local/depth-bounds-native-20261004-01/result.json)는52/56 matches·noDSV mismatch4·exit1이다. D3D noDSV pass를 위해 native bounds를0/1로 복원했다. Provisional D16 EQUAL/raw-value fixture 실패와50case/cap0 terminal FAIL을 [최종50case proof](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)와 구분한다. Obsolete common MSAA UAV rejection만 교정했고80case positive consumer를 유지했다. Early statistics는 exact이고 late invocation을 post-bounds survivor 수로 바꾸지 않았다.

### P2 #12 이전 Apple family depth bounds emulation — 2026-10-05

**현재 완료:** forced/native 각각 양 ABI146case(50+36+32+28), Shared AIR 양 ABI12 GPU runs. [현재 named results](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)는 forced runtime07/08/09·native runtime10 및 shared runtime11/12를 구분한다. [Forced x64 FAIL](../build/local/depth-bounds-emulation-20261004/forced-x64-01/result.json)은 early zero-mask만으로 stencil commit을 막지 못한 문제, [x86 FAIL](../build/local/depth-bounds-emulation-20261004/forced-x86-02/result.json)은 emulation byte53 mirror 누락이었다. Producer visibility scratch 수명 및 resolve fixture Alignment0도 교정했다. **M5 Pro에서 이전 family 경로를 강제한 proof이지 Apple7/8/9 물리 GPU proof가 아니다.**

### 새 P3 #24 compatible BC storage와 clear publication 조사 — 2026-10-04

Independent remap controls의 exit139와 runtime25 volume byte61440 mismatch는 실패 그대로다. [Native parent-view control](../build/local/p3-20261003/tiled-tier4/native-sparse-parent-view-01.log)은32 remap/37 completions·exact decode를 통과했다. Runtime28은 private RTV/BC/UInt shadow의 mapped heap을 consuming recording의 기존 residency set에 포함해 retirement까지 유지했다. Predicate polarity fixture 교정 뒤 [x64](../build/local/p3-20261003/tiled-tier4/public-runtime28-x64-predicate-contract21/result.json)·[x86](../build/local/p3-20261003/tiled-tier4/public-runtime28-x86-predicate-contract21/result.json)는76case/Tier4 PASS다. 현재 Tiled 계약은 CORE를 따르며 native render-area workaround나 barrier/CPU wait 추가로 실패를 감추지 않았다.

### 새 P3 #15 raw Atomic64 stress 진행 보장 — 2026-10-04

Runtime24의 첫65536-thread fence timeout은 native GPU Hang으로 단정하지 않았다. Whole-RMW(initial read+모든 CAS retry)를 elected lane에 넣어 stress를 해결했지만 runtime29 RT intersection PSO는 illegal SIMD intrinsic으로 FAIL였다. Final AIR stage metadata/direct callgraph에서 intersection에 도달하는 wrapper는 scalar로 유지했다. [CORE10suites×양 ABI20회](CORE-IMPLEMENTATION-STATUS.ko.md#atomic64-완료와-전체-계약-proof)가 runtime30 최종 proof다.65536 invocations·target1/16 각각20/20·기존 deadline/oracle를 유지했고 게임 성능으로 일반화하지 않는다.

### 새 P3 #15 atomic-phase physical helper promotion — source cut, 2026-10-03

[Native64×64 bitmap Hang](../build/local/p3-20261003/atomic64/production-bitmap-encoder09-full-once.log)과8×8 contention PASS를 구분한다. Public4096은 순차8×8 application tile의 누적 수이지 동시 native4096이 아니다. Unclipped32×32/evenoffset4/native1-per-virtual4/8의16case physical proof는 모든 sample/GPU/helper 계약의 proof가 아니다. Runtime15 public helper는8/11 mismatch로 FAIL였고 pure quad/SIMD를 write로 오인한 logical-alive guard를 교정한 [runtime17 x64](../build/local/p3-20261003/atomic64/helpers-runtime17-x64-07/result.json)·[x86](../build/local/p3-20261003/atomic64/helpers-runtime17-x86-07/result.json)는11case PASS였다. Runtime15 groupshared operations/stress는 Shader Validation,32KiB capacity는 API Validation만 사용했다. 후속 positive-cap runtime24와 whole-RMW runtime30의 [CORE 최종 ledger](CORE-IMPLEMENTATION-STATUS.ko.md#atomic64-완료와-전체-계약-proof)가 source-only/PSO-only 중간 관찰보다 우선한다.

### 새 P3 #20 matrix runtime·public admission — 2026-10-03

[Runtime05 behavioral x64](../build/local/p3-20261003/matrix/runtime05-public-x64-02/result.json)·[x86](../build/local/p3-20261003/matrix/runtime05-public-x86-01/result.json)는 untouched pinned V1 ABI·conversion·ordering PASS다. 첫 enhanced handoff fixture FAIL은 COPY_DEST→COMMON으로 교정했으며 당시 Tier0/legacy FALSE를 전체 완료로 세지 않았다. [최종 positive-cap CORE](CORE-IMPLEMENTATION-STATUS.ko.md#wavemmalinear-algebra-최종-positive-cap-proof)는 runtime05 behavior+06 numerical+07 enabled-cap의 mixed-cut 완료다. 구형 WaveMMA와 pinned successor preview matrix를 지원하되 ordinary SM6.5를 전체 SM6.10 conformance로 올리지 않는다.

### 새 P3 #16 advanced texture24 — 2026-10-03

[Runtime08 x64](../build/local/p3-20261003/advanced-texture/runtime08-advanced-x64-public-validation/result.json)·[x86](../build/local/p3-20261003/advanced-texture/runtime08-advanced-x86-public-validation/result.json)는24case PASS다. Producer status overload와 NULL mapped-status, depth-cast physical dimension/logical sample-count의 실패를 각각 교정했다. Runtime09 depth8case와 위 runtime28 volume/standard/shared casting proof가 후속 완료이며24case 하나를 전체 stage/casting proof로 확대하지 않는다.

### 게임 공통 자동 PSO 컴파일 캐시 — 2026-10-04

[Final02 x64](../build/local/auto-pso-cache-20261004.2FsF2M/cache-final02-x64/result.json)·[x86](../build/local/auto-pso-cache-20261004.2FsF2M/cache-final02-x86/result.json)의27 GPU values·disk recoveries·single-flight와 별도 gated race cut을 [현재 CORE](CORE-IMPLEMENTATION-STATUS.ko.md#자동-pso-컴파일-캐시와-focused-proof)에 둔다. Frozen ZZZ compute 반복 생성 median220.10→2.81ms(x64),217.38→2.88ms(x86)는 validation-off 단일 입력 측정이지 게임 FPS다. Owned blob은 native Metal archive/PSO object가 아니다. 최신 R4 lock 분리·R6 index와 R7 native cache 보류는 10월7일 리뷰가 우선한다.

### 새 P4 게임 경로 통합 — 2026-10-04

[Runtime02 OPTIONS13 x64](../build/local/p4-resume-20261004-WoVvSo/options13-runtime02-x64/result.json)·[x86](../build/local/p4-resume-20261004-WoVvSo/options13-runtime02-x86/result.json)는9case/copy/fan/immediate replay PASS다. Failed input-layout HRESULT 전에 성공 출력을 읽던 PSO 결함을 교정했다. Reflex·fullscreen/vblank·adapter/priority·SR/alpha/FSR/NGX 표시의 지정 named proof는 CORE 및 [P4 evidence root](../build/local/p4-resume-20261004-WoVvSo/)에 있다. CoreWindow 당시 REACHABLE_OK/feature_complete=false·RR 당시 FAIL은 그대로다. NGX paced x64는12/12 displayed, WoW64는11/12(첫 generated skip1)였으며 submitted id/Present S_OK를 displayed time으로 세지 않았다. 옛 multi-output proof는 현재 native count1 계약을 대체하지 않는다.

### Implementation dispositions — 2026-10-02

11개 finding은 재검증 후 **1–4/7–11 fixed,5–6 already fixed**로 닫았다. 최초 정적 finding은 GPU 재현 자체가 아니며 이후 arena 성장 및 10월7일 fence/cache/queue 수정은 별도다.

| Finding | 처리와 증거 한계 |
| --- | --- |
| 1 argument arena | Checked length/alignment·첫 recording 오류를 전파했다. 최초 수정은4MiB 고정 capacity; 후속 retained-chunk 성장은 CORE 계약이다. Injected allocation failure는 물리 OOM이 아니다. |
| 2 shared rewind crossing | Observer별 high-water를 기존 wait에 latch하고 late wait가 과거 crossing을 물려받지 않게 했다. Suspended-child before의 lost crossing은 실제 FAIL다. |
| 3 reservation/completion | GPU retirement 때 logical completion을 publish한다. Gate는 worker를 이전 reservation 전에서 지연시킨 API-flow proof이며 정확한 old token overtaking의 hardware 재현은 아니다. 독립 racing signaler의 deterministic winner는 보장하지 않는다. |
| 4 structured address wrap | Wide unsigned byte offset/bounds로 수정; baseline OOB atomic의 element0 corruption은 실제 FAIL다. Root-descriptor OOB를 oracle로 쓰지 않았다. |
| 5 RTAS prebuild | Incoming source부터 size-only counts/layout/placeholder가 수정돼 있었다. Actual-handler sizing은 API evidence이지 GPU 실행이 아니다. |
| 6 triangle transform | Incoming source부터 explicit row-major였다. Asymmetric transform/baked-vertex GPU 비교 PASS다. |
| 7 Texture1D SampleGrad | Physical derivative dimension에 zero padding. Baseline signature assertion과 after GPU mip/slice proof를 구분한다. 1D SampleCmpGrad exploratory expansion은 제거됐으며 완료가 아니다. |
| 8 UAV array atomics | Coordinate shape와 별도 array slice 보존. Explicit barrier로 ordered initialization/readback을 검증했다. 최초 unfenced atomic fixture FAIL은 runtime 회귀 proof가 아니다. |
| 9 mapping failure leak | Mapped-view RAII/heap ownership을 수정; x64/x86 각각32 rejection 뒤 control view1→1과 release를 검증했다. Heap OOM injection은 없었다. |
| 10 queue-wide timestamp | 첫 timestamp scope에 queue signal/wait dependency를 넣었다. Baseline도24-submission regression PASS라 failing-before GPU reversal은 입증하지 않았다. |
| 11 view-location uint wrap | LLVM i32 wrapping addition 뒤 unsigned bounds/fallback. Viewport/layer baseline은0 instead of1로 FAIL, after32case PASS. 최초 exploratory indirect-constant 실패는 wrap proof가 아니다. |

Shared metadata는 versioned이고1024 live observer slots를 넘으면 allocation failure다. 짧은 section mutex의 suspended owner는 caller를 stall시킬 수 있다. Abandoned recovery는 process exit이지 arbitrary suspension 복구가 아니다. Layout/CPU timeline proof는 mixed-bitness two-process GPU proof가 아니다.

### Build and execution evidence

[Review evidence root](../build/local/review-20261002/)의 결과는 fresh runtime/prefix·명시한 validation/module provenance의 targeted synthetic evidence다. Runtime payload는 retired될 수 있으며 logs/manifest와 분리한다.

| 범위 | 당시 결과 기록 |
| --- | --- |
| x64 query/view | [queries-after](../build/local/review-20261002/queries-after/result.json) PASS, queries-before02도 PASS. [viewwrap after](../build/local/review-20261002/viewwrap/after/result.json)32case PASS; before-02/before-layer는 실제 FAIL다. |
| x64 DXIL | [after-ordered](../build/local/review-20261002/dxil/after-ordered/result.json)·[final](../build/local/review-20261002/dxil/final/result.json)73word PASS(26 bounds/6 gradients/24 array atomics/17 bounds64). Before strict bounds FAIL·initial after unfenced-fixture FAIL은 그대로다. Raw isolated before runs는 strict owned-native-compiler provenance가 없다. Extra public compile은 GPU proof가 아니다. |
| x64 arena/mapping/sharing | [arena](../build/local/review-20261002/arena-heap/arena-after/result.json), [mapping](../build/local/review-20261002/arena-heap/mapping-after/result.json), [sharing](../build/local/review-20261002/shared-fence/after/result.json) PASS. Arena-before/mapping-before/shared-fence before-02 FAIL; shared-fence first before는 DLL dependency failure라 fence proof가 아니다. |
| x64 RTAS | [after](../build/local/review-20261002/rtas/after/result.json) targeted GPU PASS; before도 findings5/6 already-fixed를 확인했다. |
| WoW64 | Arena-wow64/mapping-wow64/shared-fence wow64만 GPU PASS. 이 review pass는 WoW64 timestamp/view/DXIL/RTAS GPU 실행을 주장하지 않는다. |

Manifest SHA256: baseline **8a95024162b85ad828df6153c96b6b296b3b200a3a6eae7b7152d4708b9cecd4**, fixed x64 **9ba1357563819f26c71ef162b91729619fbcba189e1479d9bec21046265bcc3b**, paired **f86b3c2083198820b81e1ae8e518218469dd6eab044c5eb8b9beeea93b052101**, final **4d05f38bf0c08387c6a22e75db6acc2ad768fb8ae237fb0176aeb1b0a64376a9**. Final은 양 ABI rebuild/install 뒤 영향받은 **x64 DXIL73word만 재실행**했다. 다른 행을 final manifest의 재실행으로 바꾸지 않는다. Python build/staging/diagnostics16/22/59 PASS는 CPU checks다. ASan allocator copy/sizing probes·timeline/layout checks는 actual D3D12 GPU proof와 별도다.

### D3D11 변경 조사와 유지 방침

2026-09-29 조사(upstream7c8dee1c2d73415301ceb7d1fa810861cef4cd67)에서 D3D11 swapchain fullscreen/gamma와 buffer writable 변경·fixtures/ABI 보존 검사를 유지하기로 했다. D3D12가 사용하는 공유 DXBC/compiler/DXGI/queue/presenter 및 NGX의 공식17slot ABI를 이름만 보고 삭제하지 않는다. D3D11 광범위 기능 작업을 뜻하지 않으며 현재 source 지원 범위는 CORE가 우선한다.

### P0에서 고정한 규격·backend 기준표

조회는 **2026-09-21 17:48:48 UTC**다. 당시 정식 목표는 [Agility1.619.6](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.6), [headers1.619.5](https://github.com/microsoft/DirectX-Headers/releases/tag/v1.619.5), [DXC1.9.2607](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2607), SM6.9/DXIL1.9이고 별도 preview는 Agility1.721.3/headers1.721.2/DXC1.10.2605.37/SM6.10/DXIL1.10이었다. Toolchain header SM6.7/OPTIONS18/Device10과 vendored SM6.0/OPTIONS7/Device1, clang22.1.8과 AIR LLVM15.0.7은 서로 다른 기준이다. 당시 macOS27.0/Xcode27.0/M5 Pro 관찰과 최초 FL11_0/SM5.1 선언은 현재 FL12_1/SM6.5나 새 publication 조회가 아니다. 개별 higher bytecode 수용을 public version/conformance로 승격하지 않는다.
