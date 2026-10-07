> Historical record, migrated on 2026-10-07. The original text below is preserved, not current instructions or current support. See [current priorities](../../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md).

# 병렬 구현 재개 후 사용자 요청 중단 — 2026-10-05

> **후속 재개:** 새 사용자 병렬 구현 지시로 이 중단은 해제됐다. 현재 결과와 남은 외부 계약은 [두 번째 병렬 재개 handoff](parallel-resume2-20261005.md)를 따른다. 아래는 runtime044 당시의 중단 snapshot이며 최신 착수 금지 지시가 아니다.

## 중단 지시와 실행 방식

마지막 지시: “병렬로 하는거 맞냐. 문서 업데이트하고 그만 해라. 너무 느리다.”
추가 구현·build·GPU·GUI 검증은 중단했다. 전체 계약 완료가 아니다.

A Composition, B RR, C HDR를 exclusive source 경로로 동시에 시작했다.
이후 lead의 공통 build/stage/GPU/display 검증이 직렬 병목이 됐다.
사용자가 느리다고 중단했으므로 이 파일을 읽었다는 이유만으로 검증을 자동 재개하지 않는다.
다음 구현 재개에는 새 사용자 지시가 필요하다.

이전 계약·소유권·frozen 입력: [원래 handoff](remaining-implementation-20261005.md).

## 실제 production 변경

- `src/winemetal/unix/winemetal_unix.c`, `winemetal_metal4_fx.mm`의 임시 Composition trace/model/bridge print만 제거했다.
- 성공했던 shared NOW `presentAtTime:`와 positive minimum-duration 경로 및 exception HRESULT는 유지했다.
- session-only delta: `build/local/ulw-resume-20261005-composition/session.diff`.
- RR/HDR production 변경은 없다. 기존 dirty baseline, 다른 worker/worktree 변경은 보존했다.
- 프로젝트 commit은 만들지 않았다. 이번 cleanup은 기존 uncommitted 구현 위의 삭제이며 전체 dirty 파일을 stage하지 않았다.

## 검증된 범위

최신 candidate: `build/local/runtime-044-composition-clean`.
양 ABI ninja/install, fresh stage/verify exit0; owned 모듈 provenance 확인.
일반 Presenter smoke 양 ABI8frames와 composition/present-queues/colorspace 양 ABI6개 suite가
actual runner pass=true/exit0/owned marker true/native validation 및 foreign errors0을 얻었다.
관련 전용 Wine prefixes는 종료 후 제거했다.

- **Frozen cadence 양 ABI:** warmup3, critical public12/12, consumer0. 기존 frozen source/exe는 변경하지 않았다.
  각 ABI bound source의 native positive time15pairs가 두 plane에서 일치한다.
- **Sync3 별도 consumer:** 원래 frozen fixture는0/1/2/4만 검사한다.
  nonfrozen supplemental 양 ABI public3/3 및 native minimum gap 약50.0025ms,
  실제 WindowServer palette3전환을 확인했다.
- **Native/WindowServer timing:** source HWND 원래 ROI를 옮기지 않고 ScreenCaptureKit
  displayTime/실제 BGRA frame과 독립 drawable presentedTime을 기록했다.
  이는 native 및 WindowServer 전달 증거이며 optical panel scanout이나 calibrated colour parity가 아니다.
- **Mixed 양 ABI:**6phase×2fresh scoped window capture 모두 geometry mismatch0.
  GREEN→noCommit GREEN→FALSE BLUE→TRUE RED32×24 at18,12 over BLUE→detach BLUE→detach GREEN.
  ordinary/lower/upper Presents1/1/1, commits4, detachPresents0.
  x64 screen stream은 첫 GREEN/BLUE/RED 상태만 직접 확인했으며 이후 새 사용자 창이 가렸다.
  backing capture6/6과 화면에서 가려진 tail을 혼동하지 않는다.

## Alpha 중단 위치와 미완료

x64만 identity, matrix-pending, matrix-presented, source-cropped, resized,
content-pending6phase에서 각 premul/straight 두 round, 총24fresh PNG 및 geometry 검증을 마쳤다.
content-detached READY에서 사용자 요청으로 fixture를 종료했다.
13phase 전체, queued/lifetime capture, i686 alpha capture는 **미완료**다.

Identity sRGB numeric samples:
premul/straight half는 모두 `[76,139,167,255]`;
alpha0 additive premul은 `[173,255,0,255]`, straight와 background는 `[0,255,2,255]`.
Association 동등성과 alpha0 기여 차이는 관측됐지만 절대 Windows blend transfer/gamut/quantization
oracle를 대신하지 않는다. **Normative alpha-colour parity는 닫지 않았다.**
동작이 검증되지 않은 수정, 임의 gamma 선택이나 getter echo를 구현으로 내지 않았다.

## RR/HDR 결과

**RR:** full Shader Validation과 MTLCaptureManager는
`Capturing Metal Shader Validation is not supported.`로 첫 capture 시작을 거부했다.
별도 full-validation observation은 MetalFX `Conv3x3Stride1._bufWeights`12672bytes,
`newBufferWithBytes`/`createBuffers+1107`, argument1/offset0을 확인했다.
원래 matrix-sensitive guard7 및32outputs는 유지됐으나 invalid load32회는 남았다.
Owned3328-byte params와 다른 native model buffer다. Proven correction/GPU trace/public 양 ABI acceptance는 없다.
추가 CPU turn의 provider 오류는 기술적 unsupported 판정 근거가 아니다.
패딩·guide 제거·validation suppression·cap collapse는 하지 않았다.

**HDR10+ GAMING:** fresh dual-source EDID hash112933… 동일, VSVDB0,
external calibration null/preset slots0/active-1. Pristine GPTK4 identity/관련 call-path를 확인했다.
현재 link prerequisite과 native negotiation/reference-mode/low-latency/readback/SSTM 부재를
분리한 limitation이다. 생산 코드 변경이나 Gaming support 선언은 없다.

CoreWindow, Wave64/128, higher SM, Agility SDK/device factory, stereo, overlay,
multi-adapter는 기존 game/engine/device trigger 조건을 유지한다. 이번 fixture가 새 trigger를 입증하지 않는다.

## 증거와 QA 실패 구분

- `build/local/ulw-resume-20261005-composition/{session.diff,acceptance.md,sync3-build-receipt.md}`
- `build/local/ulw-resume-20261005-observation/`: cadence/Sync3 native stderr, display BGRA/log/PNG,
  mixed scoped PNG/verdict, alpha24PNG, paired smoke/6suite result 및 invocation logs.
- `build/local/ulw-resume-20261005-rr-capture/{findings.md,capture01-result.md,observe01/}`
- `build/local/ulw-resume-20261005-hdr10plus/{findings.md,verification.md,native-query.log}`
- Notepad:
  `/var/folders/v5/z8dnxwrn5zl6tql92cn1y4_c0000gn/T/ulw-20261005-172816.XXXXXX.md.kiCJAFF3VD`.

Lead QA 실패도 보존한다: sampler 처음 READY 미도달은 delegate lifetime/runloop 수정으로 해결;
Sync3 sampler를 ABI 사이에 오래 유지한 timeout은 fresh per-ABI capture로 누락 증거만 보완;
native colorspace-presenter를 D3D12 suite에 잘못 연결한 실패는 올바른 D3D12 consumer로 수정했다.
`main()` consumer에 잘못 적용한 `-municode` compile 실패도 flag만 수정했다.
Fixture/test/runner 검증 조건을 낮추지 않았다.

## 정리와 다음 미완료 작업

모든 작업자는 terminal이고 lane watcher는 종료됐다. Alpha fixture를 중단하고 정확한
prefix/cache의 wineserver retirement 및 삭제를 확인했다.
옮겼던 세 기존 창은 원래 geometry와 independent readback으로 복구했고 새 사용자 창은 건드리지 않았다.
모든 screen stream/wake assertion/Cua lifecycle 및 control 파일은 정리했다.
사용자 창이 가린 이후의 무관한 raw image10154개는 제거하고 source35frame/metadata는 보존했다.
Latest runtime044와 comparison043/038 및 source/log/capture 증거는 유지한다.

미완료는 alpha13phase 양 ABI 전체/qualified normative oracle, panel-level 주장에 필요한 별도 증거,
mixed tail의 물리 화면 검증(사용자 ROI 가림), RR clean native validation/public 양 ABI,
HDR 실제 Gaming negotiation/readback/SSTM이다. 자동 재실행하지 말고 다음 사용자 지시를 기다린다.
