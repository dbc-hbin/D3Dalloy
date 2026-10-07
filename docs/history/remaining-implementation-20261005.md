> Historical record, migrated on 2026-10-07. The original text below is preserved, not current instructions or current support. See [current priorities](../../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md).

# 남은 구현 병렬 실행 handoff — 2026-10-05

> **최신 결과:** 새 사용자 지시로 두 번째 병렬 재개를 실행했다. [현재 alpha 전체 backing 검증·RR/HDR native 계약 경계와 다음 조건](parallel-resume2-20261005.md)을 먼저 읽는다. 아래 실행 지시/중단 언급은 원래 소유권과 과거 cut이다.

> **후속 최신 중단:** 이 지시로 A/B/C 병렬 작업을 시작했지만 사용자가 느리다고 문서 갱신 후 중단을 요청했다. 새 세션은 [runtime044 실행 결과와 실제 미완료 범위](parallel-implementation-stop-20261005.md)를 먼저 읽는다. 아래 “즉시 구현 시작” 지시는 과거 재개 지시이며, 최신 사용자 중단을 무시하는 자동 실행 권한이 아니다.

## 다음 세션의 첫 지시

이 파일을 먼저 읽고 현재 worktree를 확인한 뒤, 아래 A/B/C 소유권으로 **병렬 구현**을 시작한다. 준비 조사만 반복하지 않는다. 확정된 구현 단위는 담당자가 직접 수정·검증하고, native/platform 원인이 미확정인 단위는 원인을 구분하는 검증부터 실행한다. 공통 파일은 담당자를 한 명만 둔다. 마지막 사용자 지시는 “너무 느리니까 handoff 남기고 다음 세션으로 넘기자. 다음 세션에선 병렬로 구현시킬거야.”였다. 이번 세션은 그 요청에 따라 중단하며 전체 목표 완료가 아니다.

## 전체 목표와 완료 기준

`graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md`의 남은 작업을 닫는다:

1. Composition: approved Wine clone의 SetOffsetX/Y, FALSE/TRUE nonempty root pixels, x64/WoW64 mixed, normative alpha-colour parity, Sync0–4 physical cadence, 기존 일반 Presenter 회귀.
2. RR: Conv3x3Stride1U8 offset12672/length12672 native validation 오류를 해결하고 public 양 ABI acceptance/회귀.
3. HDR10+ GAMING: native Gaming reference-mode·저지연 협상·readback·SSTM. 현재 sink의 hardware gate와 owned 구현 부재를 구분하고 불가능한 검증은 증거와 함께 미해결로 남긴다.
4. CoreWindow·Wave64/128·상위 SM/SDK·stereo·overlay·multi-adapter: 실제 게임/engine 또는 장치 trigger가 없으면 조건만 기록한다.

규격 계약 + API→compiler→Metal→readback/presentation + 독립 oracle + native validation + 회귀가 필요하다. source/compile/probe/finite output만으로 완료 선언하지 않는다. 병렬 준비/구현은 허용되지만 계약별 완료 판정은 생략하지 않는다. 원래 goal/todo의 미완료 항목을 작은 새 목표로 대체하지 않는다.

## 가장 중요한 마지막 결과

**Runtime043 absolute-time 후보가 x64 frozen cadence consumer를 처음 통과했다.**

- Source의 duration0 분기를 `present`에서 두 drawables에 같은 `presentAtTime:CACurrentMediaTime()` NOW deadline으로 변경했다. SyncInterval>0의 minimum duration은 그대로다. 임의 duration floor·통계 제조·fixture 변경이 아니다.
- `build/local/runtime-043-composition-absolute-time` build/install/stage/verify exit0.
- `build/local/composition-resume-20261005/cadence-absolute43-manual-x86_64/stdout.log`: warmup3, critical_requested12, **critical_public_measured12**, presents17, public_unsupported0. Consumer monitor 실제 exit0.
- Native API/Shader Validation enabled. 오류/invalid-load/bridge-exception/foreign backend 검색0.
- `native_timing_verdict=unverified`는 그대로다. **WoW64 cadence, 독립 physical interval 분석, alpha-colour parity, suite/일반 Presenter 회귀는 아직 안 했다. 전체 Composition 완료가 아니다.**
- `sync0-screen-state.json`은 Wine ROI1430,100,80x64를 가리는 opaque 앱이 없음을 보여준다. `sync0-screen.png`/`sync0-window.png`은 이전 warmup 보라색에서 새 내용으로 바뀌었다. 빠른 연속 Sync0 중 capture가 뒤 frame의 황금색을 잡았으므로 ordinal6의 정확한 RED와 per-frame physical cadence를 증명한다고 주장하면 안 된다.
- Runtime043은 **temporary diagnostic traces를 아직 포함**한다. 최종 제출 전 불필요한 임시 traces 제거→새 runtime→관련 양 ABI 검증이 필요하다.
- 사용자의 handoff 지시 후 새 구현/test는 시작하지 않았다. 진행 중 검증은 종료되기 직전 위 결과를 남겼으며 그 출력만 읽었다.

## 현재 worktree와 소유권

HEAD: `8a72292e047a43e3577296d28a578a0eb5566313`. 이 세션의 project git commit은 없다. 기존 dirty worktree를 보존했다. 다음 파일에 이번 세션 변경이 있지만 **그 파일의 전체 diff가 이번 세션 변경이라고 가정하면 안 된다**:

- `src/winemetal/winemetal.h`: target-state에 float offset_x/y, size16→24.
- `src/winemetal/winemetal_thunks.h`: matching24-byte assertion.
- `src/winemetal/unix/winemetal_unix.c`: native offset layout, FALSE-target z-order, composition displaySync policy, 현재 absolute-time zero-duration branch, 임시 acquire/observe/publish/method/send/commit/presented traces.
- `src/winemetal/unix/winemetal_metal4_fx.mm`: 임시 normal bridge-return/exception trace. 기존 exception return0/ret1 HRESULT는 유지했다.
- approved separate clone `toolchains/wine-game-display/dlls/dcomp/{device.c,Makefile.in}`: visual SetOffsetX/Y 저장·Commit 전달, reproducible include paths.
- CORE/HISTORY/PRIORITIES: 재개 결과 및 병렬 조사 반영. 일부 본문은 runtime041 시점이므로 이 handoff의 runtime043 cut이 더 최신이다.
- ignored local QA harness `build/local/composition-resume-20261005/run-cadence.py`: optional fourth CLI argument를 기존 frozen `--control-file`로 전달하도록 수정했다.

실패한 duration0 timed call,100µs floor, explicit CATransaction flush는 기각/제거했다. 현재 native source에 floor/flush는 없다. 다른 작업자의 source를 되돌리거나 통째로 stage/commit하지 않는다.

## 다음 세션의 병렬 구현 배분

| 담당 | 독점 write paths | 첫 작업과 종료 조건 |
| --- | --- | --- |
| A Composition | `src/winemetal/unix/winemetal_unix.c`, `winemetal_metal4_fx.mm`, `src/winemetal/{winemetal.h,winemetal_thunks.h,Metal4FX.hpp}`, approved Wine `dlls/dcomp/`; 필요한 D3D12 presentation/shared Presenter 수정도 A에 단일 배정 | Runtime043 근거를 읽고 WoW64 cadence·독립 native interval·normative alpha-colour·composition/일반 Presenter 회귀를 닫는다. 임시 instrumentation 제거 후 최종 runtime 양 ABI 재검증. frozen fixture 변경 금지. |
| B RR | `src/winemetal/unix/metalfx_rr.inc`, 필요한 `src/nvngx/`의 구체적 파일, 자기 evidence dir | 아래 RR findings의 next capture로 offending buffer binding/producer를 확정하고 evidence-supported production fix를 구현한다. A 소유 fx.mm/header가 필요하면 직접 수정하지 말고 integration 요청. Guide 제거/padding/error suppression 금지. |
| C HDR10+ | `src/nvapi/` 중 HDR display facade 파일, 자기 evidence dir | fresh EDID/native/GPTK evidence부터 native negotiation/readback/SSTM 가능한 계약을 구현한다. 현재 hardware gate는 없애서 꾸미지 않는다. A 소유 native dispatch/ABI 수정은 integration 요청. 소비되지 않는 parser·bookkeeping getter만 추가해 feature 완료로 세지 않는다. |
| Lead integration/docs/QA | 공통 status docs, build/install/stage/output namespace와 native GPU/display lease | A/B/C를 동시에 시작하되 각 명확한 파일을 한 명에게만 배정한다. shared builds/install/stage는 단일 coordinator가 처리하고 GUI/timing/GPU 검증은 slot으로 직렬화한다. CPU 소스 작업·독립 분석은 병렬. |

자식은 독립 evidence directory를 사용한다. 공통 파일이 필요한 변경은 integration owner가 한 번만 적용한다. 파일 path 없는 광범위 “알아서 구현”이나 처음부터 다시 조사만 하는 위임을 피한다. 이 worktree에 unrelated 변경이 많으므로 isolation/merge 방식을 먼저 확인한다. 사용자 승인 없는 다른 repo 확장은 금지다.

## Composition: 확정된 증거와 남은 검증

Opaque mixed 양 ABI 지정 proof는 끝났다:

- x64: `build/local/composition-resume-20261005/mixed-offset38-x86_64/actual-stable-phase-evidence.json`.
- WoW64: `.../mixed-manual38-i686/pixel-verdict.json`,12 PNGs, stdout/stderr/invocation.
- Runtime038, 각6 phase×2 capture PASS. Original GREEN→no-Commit GREEN→FALSE BLUE→TRUE RED32x24 at18x12 over BLUE→upper detach cached BLUE→lower detach original GREEN. Detach 새 Present0.
- Nonempty counts BLUE17408/RED3072 at Retina160x128; inspector edge tolerance2 physical pixels. Alpha-colour normative parity나 scanout timestamp proof가 아니다.
- WoW64 actual prefix d3d12.dll SHA256 `3bc26c3282f512067a2f3b3f0ccfe2218ef558c3e32818aed61263ab146adf92`; dcomp.dll `b3409bf43555a081d572aca9685be9ae9c1cf85f733082f92fee7548c5dde496`. Staged i386 modules와 같고 native winemetal/d3d12core path도 owned였다. Consumer0/validation errors0.

실패 원인 구분용 ledger:

- `cadence-offset38-unoccluded-x86_64`, `cadence-offset39-x86_64`, `cadence-floor40-x86_64`: warmup3 후 first Sync0 FAIL.
- `cadence-sync0-screen38-x86_64`, `cadence-flush41b-x86_64`: native test-window를 Cua로 옮기고 hold한 뒤 FIRST Sync1 warmup FAIL. Window-move/hold/title precondition의 별도 divergence다. Sync0 자체 결론과 혼동하지 않는다.
- `cadence-flush41-x86_64`: parent preparation이 frozen300s deadline을 넘겨 control timeout. Candidate 효과 증거가 아니다.
- `cadence-flush41c-static-x86_64`: original HWND/static, warmup3 후 Sync0 FAIL. Late tail-F capture는 no-Wine/no-match라 제외했다.
- **`cadence-boundary42-manual-x86_64`**: direct stdout pipe로 observation4에 timely capture. No occluder, physical/window backing 둘 다 이전 warmup PURPLE. ID5 accepted1/IDs4+4, selector present/main1, 양 plane send·commit·bridge status0 정상 반환, exception/callback 없음. 즉 통계만의 누락이 아니라 새 content 미표시. Prefix/cache/임시 창 정리 완료.
- **`cadence-absolute43-manual-x86_64`**: 현재 x64 public12/12 결과. 원본 파일과 code changes를 그대로 보존한다.

Frozen exes/source:

- `build/local/composition-mixed-20261005.kbTsD1/composition-mixed-{x86_64,i686}.exe`
- `build/local/composition-cadence-20261005.Bqiyja/composition-cadence-{x86_64,i686}.exe`
- cadence x64 SHA256 `99682c9a28059d5f7746b9eded5dba4795c3ae911277318a36366f333e743aef`
- mixed i686 SHA256 `15f105ce84a1c906d9128030986c8e2b0d387b231b12b2a856fc74b3c6c60680`

Capture helpers: `build/local/composition-resume-20261005/{capture-window,screen-state,inspect-mixed}`. Window backing만으로 물리 표시를 증명하지 않는다. capture-window는 sRGB 정규화되어 있다. 원래 HWND 위치에서 가림을 통제한다. Cua로 Wine native 창 자체를 이동한 검증은 warmup 전제조건도 바뀌었다.

Local run-mixed.py contains fixed sleeps/log polling; do not trust those as synchronization. The manual WoW64 proof used exact frozen VISUAL_READY and inspected both captures. For autonomous cadence, tail-F filesystem polling is too late for the8-vblank failure window: subscribe directly to consumer stdout pipe **before launch** (see manual invocation/monitor commands in notepad/session).

## RR: parallel diagnostic result

Read:

- `build/local/rr-native-resume-20261005/findings.md`
- `build/local/rr-native-resume-20261005/optional-hit-disabled-20261005/comparison.md`

The invalid12672-byte buffer is MetalFX Conv3x3Stride1 createBuffers, not our3328-byte preprocessing parameter buffer. Classic YES/NO, native arm64 and Metal4 controls already reproduce it without Wine/NGX. Native support1 or finite/non-copy outputs is not validation acceptance.

Latest isolated classic NO x64 variant disabled only optional specular-hit descriptor/binding, preserving original source/9 allocated textures/input bytes/validation/matrix guard. Compile0, baseline1,32 identical invalid loads at12672/12672, same pipeline UID/allocation stack; all32 finite/non-copy; matrix-sensitive pairs7→0. The unchanged guard remains FAIL. Optional guide removal is not a remedy. No production RR edit exists from this diagnostic. Next is programmatic native Metal capture to identify offending buffer binding and producer, not padding or guessed flags. Its native process is gone and GPU slot released.

Pristine GPTK4.0b2 reference identity was rechecked by RR/HDR children. Reference path is `build/local/p4-20261004-N7DnYe/reference/D3DMetal.framework`; exact hash/version/symbol evidence is in their findings and CORE. Reverify before new analysis. Compatibility evidence only; no proprietary code import or production fallback.

## HDR10+ and conditional items

Read `build/local/hdr10plus-resume-20261005/findings.md`, raw `native-query.log`, `edid-sha256.log`, `recovered-edid-check.log`.

- Genuine dual-source EDIDs SHA256 `112933369a46b8604e3289d66575ebb7ae3ea043060ec92f6d565040bef288aa` match.
- Current S32B80P link: PQ/static HDR but VSVDB0, external preset slots0/active-1, calibration null.
- This is a current-link prerequisite failure, not proof that optional game HDR10+ is irrelevant or that a native path is impossible.
- Native negotiation/readback/per-display mode ownership/low-latency/SSTM remain separate missing contracts. Do not add an unconsumed EDID parser or report recorded request state as platform mode readback.

`build/local/conditional-resume-20261005/findings.md` covers EACH CoreWindow, Wave64/128, higher SM, higher SDK/Agility, stereo, overlay, multi-adapter. Bounded selected-game evidence did not establish a new trigger; no proved worldwide absence. Records are in CORE/HISTORY/PRIORITIES. Mask1 selects existing node0 and is not a multi-GPU trigger. No extra implementation is currently authorized merely to fill a checklist.

## Build/runtime commands

Root sources only. Do not regenerate historical patches.

```sh
export PATH=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:$PWD/toolchains/tools/bin:$PATH
ninja -C build/local/build -j8
meson install -C build/local/build --no-rebuild
ninja -C build/local/build32 -j8
meson install -C build/local/build32 --no-rebuild
sh build/local/wine-game-display-build/build-command.sh
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --dxmt-install32 build/local/install32 \
  --wine-source toolchains/wine-runtime --wine-dest <FRESH_RUNTIME> \
  --wine-display-build build/local/wine-game-display-build
python3 scripts/stage-owned-graphics.py verify --runtime <FRESH_RUNTIME>
```

Current staged comparison runtime038; latest043. Do not overwrite them or install into a live game/prefix. Free disk was77Gi before these final stages; check again before new full payloads. Retire identified obsolete payloads while preserving manifests/evidence, never another worker's artifacts.

```sh
OWNED_CADENCE_RUNTIME=$PWD/build/local/runtime-043-composition-absolute-time \
  python3 build/local/composition-resume-20261005/run-cadence.py i686 <FRESH_TAG>
```

This helper itself does not arrange display exposure or prove physical cadence. Use fresh prefix/out and native validation. The manual x64 run's full env, stream capture, restoration metadata is in invocation.json and notepad.

Existing regression runner:

```sh
python3 scripts/test-owned-graphics.py --runtime <RUNTIME> \
  --suite composition --fixture <COMPILED_FIXTURE> --out <FRESH_OUT> --validation
```

Read root README/RUNTIME/SMOKE guide for exact related fixture/Presenter suite selection. Do not substitute meson test, DX11 samples or a made-up lint target. C++ LSP errors0 on latest changes; Python/JSON LSP executables were absent, so no Python/JSON typecheck PASS is claimed.

## Pause state, cleanup and memory

- All child tasks are terminal: RR/HDR/conditional/callback investigation. No child owns an unintegrated edit.
- Absolute43 consumer completed0 before handoff cleanup. Its monitor termination request must not be misreported as the test failing; actual completion and stdout prove the narrow public result.
- Five temporary occluders restored with confirmed readback only if current geometry still matched the agent-applied geometry. All matched. No user focus/size change. Native fixture HWNDs stayed at original coordinates in final042/043 runs.
- All final dedicated Wine prefixes/cache/control files and stream watchers from finished QA were torn down. Original logs/PNGs/RAW/provenance/manifests retained. Latest build/install/staged runtimes are intentional retained artifacts.
- The registered whole goal is not complete; tool status was blocked with engine reason “user interrupted the turn”. This is user-directed session handoff, not a claim of a technical impasse. Preserve objective on resume; do not call goal complete.
- 이번 세션의 미완료 실행 todo는 사용자의 중단/handoff 요청 때문에 dropped로 닫았다. 기능 완료나 범위 삭제가 아니다. Todo의 “29/29 done,0 open” 집계는 dropped를 포함하므로 목표 완료 증거로 사용하지 않는다. 다음 세션은 아래 실제 미완료 항목을 새 실행 todo로 복구한다.
  - Sync0 해결의 최종 판정, 양 ABI cadence/독립 physical proof, composition 및 일반 Presenter 회귀, 최종 build/tests, 남은 disposable artifact 확인, normative alpha-colour parity.
  - RR clean native validation와 양 ABI acceptance, HDR10+ native/platform 가능한 구현과 미해결 증거.
  - 최종 CORE/HISTORY/PRIORITIES 증거 정합 및 전체 목표의 최종 보고.
- Persistent facts: `notes/composition-cadence-20261005.md`, `notes/remaining-contracts-parallel-20261005.md`, recalled `notes/facts/2026-10.md`.
- Full append-only notepad: `/var/folders/v5/z8dnxwrn5zl6tql92cn1y4_c0000gn/T/ulw-20261005-composition.XXXXXX.md.MsZO9OXUtJ`. It is long and lower-level; this handoff is the new entry point. Latest043 result supersedes earlier “unverified candidate” notes, not their raw failure evidence.

## First concrete next actions

1. Launch A/B/C with exclusive files and their evidence, not another broad planning/ack-only wave.
2. A: inspect043 native timestamps/validation and complete WoW64 plus normative physical cadence/alpha/Presenter regression; keep the successful current algorithm while removing temporary debug traces only with new verification.
3. B: identify internal failing RR binding/producer under native capture; implement only a proven contract fix.
4. C: establish feasible native HDR negotiation/actual readback against current hardware gate; do not manufacture qualified sink support.
5. Lead: serialize GPU/display slots/build staging, integrate cross-boundary patches/docs and preserve original failure artifacts. Declare contracts complete only at their full required scope.
