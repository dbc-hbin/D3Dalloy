# D3D12 NGX 검증 레시피

모든 명령은 저장소 루트에서 실행한다. **현재 루트 소스**를 빌드한 Meson install을 사용하고 매번 새 runtime/output/prefix를 만든다. 옛 `ngx-work`/private candidate 또는 게임 설치 DLL을 입력으로 쓰지 않는다. 프로젝트 빌드·toolchain 준비는 [README](../README.md), 현재 관찰된 PASS 범위는 [CORE 검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)가 기준이다. 아래 명령은 재검증 절차이며 이번 문서 개편의 실행 결과가 아니다.

## 검사 층 구분

| 검사 | 증명하는 범위 | 증명하지 않는 범위 |
|---|---|---|
| `test-owned-ngx-frontend.py` | 현재 frontend PE export·marker·official MSVC parameter slot ABI | device/GPU·실제 game loader |
| `--suite ngx` | system32 owned DLL → public D3D12 NGX → Metal 4 temporal GPU/readback·lifetime | 실제 Streamline lookup·DLSS 품질 |
| `--suite ngx-contracts` | init/shutdown·parameter ownership/default·opaque handle·output offset record-time 경계 | output-subrect GPU readback |
| `--suite temporal` | frontend를 우회한 COM backend·typed/typeless readback·subrect·replay·cross-queue trace | NGX public ABI·game loader |

현재 `ngx`·`ngx-contracts`의 MSVC facade와 staging proof는 **x64**다. WoW64 temporal PASS를 x86 NGX PASS로 옮기지 않는다. x86 NGX/NGX-contracts planned rows는 실행되지 않았고 118 집계에 포함되지 않는다. ARM64 Wine/FEX 최종 proof와도 분리한다.

## CPU/PE ABI 검사

README 절차로 현재 루트의 cross build를 먼저 완성한다. `CROSS_BUILD`는 `src/dxgi/dxgi.dll.a`, `src/util/libutil.a`, `src/winemetal/winemetal.dll.a`가 있는 **그 빌드 디렉터리**다. Compiler 기본값은 pinned llvm-mingw이고 다른 실제 설치 위치는 `--toolchain`으로 지정한다.

```sh
: "${CROSS_BUILD:?현재 루트의 완료된 Meson cross build 경로}"
mkdir -p build
NGX_WORK="$(mktemp -d "$PWD/build/ngx-check.XXXXXX")"
python3 scripts/test-owned-ngx-frontend.py \
  --build-root "$CROSS_BUILD" \
  --nvidia-include external/nvngx/include \
  --out "$NGX_WORK/frontend"
```

이 script는 지정 out이 이미 있으면 지운다. 따라서 위처럼 **전용 새 child 경로**를 지정하며 기존 evidence 경로를 절대 재사용하지 않는다. `NGX_SDK_INCLUDE` 또는 `--nvidia-include`는 official header 입력 변경이며 다른 SDK 사용 시 해당 provenance를 기록한다.

검사는 기존 D3D11 15 exports와 D3D12 16 exports·`own-ngxd3d12` marker, MSVC facade vftable, 공식 header로 컴파일한 consumer의 virtual call offsets를 대조한다. COFF/PE PASS는 GPU 실행이 아니다.

## 현재 fixture 빌드

위에서 생성한 새 `NGX_WORK`를 이어 사용한다. `TOOLCHAIN`은 실제 llvm-mingw bin 디렉터리다. Fixture main은 MinGW PE, official parameter helper는 x64 MSVC ABI object다. SDK 임시 복사본 대신 vendored header를 사용한다.

```sh
TOOLCHAIN="${TOOLCHAIN:-/opt/llvm-mingw-20260616-ucrt-macos-universal/bin}"
mkdir -p "$NGX_WORK/fixtures"
for name in owned-d3d12-ngx owned-d3d12-ngx-contracts; do
  "$TOOLCHAIN/x86_64-w64-mingw32-g++" \
    --target=x86_64-pc-windows-msvc \
    -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
    -Iexternal/nvngx/include \
    -c "graphics-translator/tests/$name-msvc.cpp" \
    -o "$NGX_WORK/fixtures/$name-msvc.obj"
  "$TOOLCHAIN/clang++" --target=x86_64-w64-mingw32 \
    -std=c++17 -O2 -Wall -Wextra -Werror -static \
    "graphics-translator/tests/$name.cpp" \
    "$NGX_WORK/fixtures/$name-msvc.obj" \
    -o "$NGX_WORK/fixtures/$name.exe" -ld3d12 -ldxgi -lole32 -luuid
done
```

Main fixture는 `owned-d3d12-temporal.cpp`의 resource/readback helpers를 rename-main include한다. Temporal source 변경 뒤에는 NGX fixture도 함께 다시 빌드한다. PE에 `nvngx.dll`을 static import하지 않는다.

## Fresh staging과 runner

`OWNED_INSTALL`은 README로 현재 루트를 **NGX 활성화해** build/install한 x64 prefix, `WINE_SOURCE`는 복제할 원본 Wine package다. Staging은 원본을 수정하지 않는다. Destination은 아직 없는 새 경로여야 한다. NVAPI topology 시험이 필요하지 않은 이 fixture에서는 `--with-nvapi`를 임의로 추가하지 않는다.

현재 D3D12 install에는 같은 ABI/generation의 필수 owned `d3d12core.dll`(ordinary native-only PE, actual exported SDK618)이 포함돼야 한다. Core는 `d3d12core=n`, Main과 NGX 등 기존 builtin modules는 `b`로 고정한다. NGX stage에도 Core를 함께 복사·검증하며 별도 `--with-core` toggle은 없다. NGX GPU PASS를 Agility version/path loader 증거로 세지 않고 [SMOKE의 paired `agility` consumer](SMOKE-TEST.ko.md#owned-core618-versionpath수명-acceptance)를 따로 실행한다.

```sh
: "${OWNED_INSTALL:?현재 루트의 x64 Meson install 경로}"
: "${WINE_SOURCE:?복제할 원본 Wine runtime 경로}"
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install "$OWNED_INSTALL" \
  --wine-source "$WINE_SOURCE" \
  --wine-dest "$NGX_WORK/runtime" \
  --native-arch x86_64 --windows-abi x64 --with-nvngx
python3 scripts/stage-owned-graphics.py verify --runtime "$NGX_WORK/runtime"

python3 scripts/test-owned-graphics.py \
  --runtime "$NGX_WORK/runtime" --guest-arch x64 \
  --fixture "$NGX_WORK/fixtures/owned-d3d12-ngx.exe" \
  --suite ngx --mode no-present --validation \
  --out "$NGX_WORK/run-ngx" --timeout 120
python3 scripts/test-owned-graphics.py \
  --runtime "$NGX_WORK/runtime" --guest-arch x64 \
  --fixture "$NGX_WORK/fixtures/owned-d3d12-ngx-contracts.exe" \
  --suite ngx-contracts --mode no-present --validation \
  --out "$NGX_WORK/run-contracts" --timeout 120
```

이 x64 staging은 현재 검증 baseline이며 최종 Rosetta-free ARM64 Wine/FEX 구성의 레시피가 아니다. Runner는 각 out 아래 fresh prefix를 만들고 fixture를 freeze하며 runtime manifest·fixture·lock hash, loaded owned/native module identity, stdout/stderr·validation 상태를 기록한다. `--out`은 기존 directory/symlink를 거부한다. 종료 시 이 호출이 만든 prefix의 wineserver만 정리한다. `--guest-arch`는 ABI assertion이지 guest translation switch가 아니다.

## NGX GPU fixture acceptance

`owned-d3d12-ngx.cpp`는 `C:\\windows\\system32\\nvngx.dll`을 요청하고 loaded path suffix·file marker·required exports를 검사한다. `OwnedGraphicsBackendId()==owned-graphics-d3d12`도 요구한다. 이것은 **이 PE의 DLL 선택**을 증명하며 게임 lookup을 대신하지 않는다.

- Create: 64×64 input, 128×128 logical output, SuperSampling/Balanced, flags74.
- Frame: RGBA8 color, 136×136 output UAV backing, real D32_FLOAT_S8X24_UINT DSV-clear depth, zero R16G16_FLOAT motion, caller reset 0, raw preExposure/Exposure.Scale 0.
- 하나의 handle로 8 frame. Evaluate 직후 parameter Reset/Destroy하고 **그 뒤** Close/Execute한다. Backend가 borrowed parameter storage를 사용하면 안 된다.
- ReleaseFeature-before-Execute + 동일 closed-list replay: 첫 implicit reset 1, 다음 0. Output readback·outside sentinel을 함께 검사하고 released handle Evaluate는 exact FeatureNotFound를 요구한다.
- Init 전 Allocate는 NotInitialized, closed Create/Evaluate·NaN jitter는 InvalidParameter, unsupported feature는 FeatureNotSupported, **subrect opt-in이 꺼진 feature**의 nonzero output base는 UnsupportedParameter다.

최종 marker `OWNED_D3D12_NGX_PASS`와 정확히 0..7 frame marker를 runner가 검사한다. Loose mean/sentinel 검증은 same-frame nonblack sanity이지 DLSS/MetalFX 품질 동등성 지표가 아니다. Contract fixture의 case와 final marker는 [NGX-CONTRACTS](NGX-CONTRACTS.ko.md#contract-fixture의-판정)에 있다.

## NGX feature11 caller-owned native presentation

`owned-d3d12-ngx-features.exe --mode present`는 RR oracle 없이 실행하는 별도 FG 경로다. Runner suite는 `ngx-fg-present --mode present`다. 기존 `ngx-features --mode no-present`의 FG/RR/alpha case 수·protocol·RR oracle 요구는 그대로 유지된다.

공식 NGX D3D12 ABI에는 Present export가 없다. Fixture가 HWND flip-discard `IDXGISwapChain4`를 만들고, **NGX feature11 EvaluateFeature → DLSSG.OutputInterpolated**를 generated texture로 사용한 뒤 generated/real texture를 실제 backbuffer에 copy하고 `Present(1,0)`한다. FFX public proxy를 호출하지 않으며 product NGX swapchain export도 추가하지 않는다. 사용한 `DLSSG.HUDLess`, `DLSSG.Backbuffer`, MV/jitter/reset keys는 현재 facade가 수용하는 runtime key이며 vendored official header가 그 문자열을 정의한다는 뜻은 아니다.

아래에서 `MSVC_HELPERS`는 기존 paired NGX helper object가 있는 디렉터리, runtime 두 개는 현재 owned source를 NGX 활성화해 fresh stage한 경로다. `PRESENT_WORK`는 새 경로여야 한다.

```sh
TOOLCHAIN="${TOOLCHAIN:-/opt/llvm-mingw-20260616-ucrt-macos-universal/bin}"
: "${MSVC_HELPERS:?owned-d3d12-ngx-msvc-x64.obj 및 -x86.obj 디렉터리}"
: "${OWNED_RUNTIME_X64:?x64 fresh staged runtime}"
: "${OWNED_RUNTIME_X86:?WoW64 x86 fresh staged runtime}"
PRESENT_WORK="$(mktemp -d "$PWD/build/local/ngx-present-XXXXXXXX")"
mkdir -p "$PRESENT_WORK/fixtures"
for pair in x64:x86_64 x86:i686; do
  arch=${pair%%:*}; target=${pair#*:}
  "$TOOLCHAIN/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-d3d12-ngx-features.cpp \
    "$MSVC_HELPERS/owned-d3d12-ngx-msvc-$arch.obj" \
    -o "$PRESENT_WORK/fixtures/owned-d3d12-ngx-features-$arch.exe" \
    -ld3d12 -ldxgi -lole32 -luuid -luser32
done
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -arch x86_64 -arch arm64 \
  -dynamiclib -fblocks -framework Foundation -framework Metal -framework QuartzCore \
  graphics-translator/tests/owned-fg-drawable-observer.mm \
  -o "$PRESENT_WORK/fixtures/owned-fg-drawable-observer.dylib"
for arch in x64 x86; do
  if [ "$arch" = x64 ]; then runtime=$OWNED_RUNTIME_X64; else runtime=$OWNED_RUNTIME_X86; fi
  python3 scripts/test-owned-graphics.py --runtime "$runtime" --guest-arch "$arch" \
    --fixture "$PRESENT_WORK/fixtures/owned-d3d12-ngx-features-$arch.exe" \
    --suite ngx-fg-present --mode present --api-validation --timeout 180 \
    --fg-observer "$PRESENT_WORK/fixtures/owned-fg-drawable-observer.dylib" \
    --out "$PRESENT_WORK/run-$arch"
done
```

Runner는 observer dylib/source/checker/fixture 입력을 freeze하고 `OWNED_FG_OBSERVER`, `FG_CAPTURE_DIR`, `FG_CAPTURE_CONSUMER=<실제 frozen PE 이름>`, `FG_CAPTURE_SUITE=ngx-fg-present`를 설정한다. Native observer는 Wine service가 아니라 해당 consumer만 관찰하며 PE 파일명과 무관하게 suite protocol을 선택한다. PASS log 단독으로는 통과하지 않는다.

Formal NGX frame을 시작하기 전에 caller-authored solid RGB real frame을 제출해 public DXGI actual statistics로 실제 표시를 확인한다. Bootstrap은 별도 `BOOTSTRAP attempt=N`/`BOOTSTRAP_READY` protocol이며 최대 64 real submissions·15초 범위에서 실제 표시가 없으면 실패한다. Bootstrap 성공 뒤에도 native drawable은 합법적으로 skipped(`presentedTime=0`)일 수 있다. `Present(S_OK)`/`GetLastPresentCount`는 제출, frame-latency credit은 GPU retirement이며 실제 표시를 보장하지 않는다. 따라서 **각 제출 id가 반드시 actual statistics가 될 때까지 기다린 옛 방식은 fixture protocol bug**다. Product의 skipped 처리와 shared FSR helper는 그대로 두고, caller는 containing output의 실제 `WaitForVBlank`로 pacing하며 정확히 9 requests를 한 번씩 제출한다. Actual statistics는 positive 표시의 snapshot 또는 disjoint로 따로 기록하며 해당 id의 scanout을 기다리거나 skipped frame을 재제출하지 않는다. 마지막 GPU credit retirement와 실제 output refresh 3회로 drain한다. Formal requests는 real1..6과 실제 GPU flag0인 generated2/3/4뿐이다. First/reset1/5/6의 flag1·actual0은 sentinel 출력을 보존하며 generated Present를 생략한다.

Expected artifacts: 각 run의 `fixture/fg-ngx-bootstrap-{1..B}.bin`, `fixture/fg-ngx-{1..6}-{1,0}.bin`, `fg-ngx-fresh-{4,5}.bin`, `fg-ngx-replay-6.bin`, 총 **B+9**개의 `fg-capture/drawable-*.bin`, `fg-capture-analysis.json`, `result.json`, `stdout.log`, `stderr.log`. Suppressed1/5/6의 generated image는 초기 sentinel(모든 byte19)과 같아야 하며 native drawable이 없어야 한다. Checker는 bootstrap 독립 solid RGB/native pixels·event identity·마지막 positive presentedTime, formal source/native pixels·phase/status·순서를 검증한다. Real6개 전부와 유효 generated2/3/4의 실제 표시 및 최소3 complete generated→real pair를 요구한다. 합법적인 native skip은 표시로 계산하지 않으며 이 bounded acceptance의 핵심 표시가 빠지면 PASS가 아니다. Caller-authored checkerboard/현재 opaque HUD의 RGB oracle, generated!=real, fresh actual0/sentinel, reset output untouched, release 후 closed-list replay, submitted count와 actual-statistics snapshot 및 실제 output pacing을 구분한다. Cadence는 positive native timestamp 간격만 기록하며 고정 refresh rate나 실게임 FPS를 주장하지 않는다.

Bootstrap 뒤에도 각 formal Present 전과 마지막 refresh drain 중에 `PeekMessageW`/`DispatchMessageW`로 실제 HWND의 window/driver event를 계속 처리한다. GPU wait·vblank pacing은 message loop를 대신하지 않으며, 이 처리는 추가 제출이나 foreground 전환 없이 동일한 9 requests의 실제 표시를 관찰하기 위한 caller lifecycle이다. Native time0은 여전히 표시 증거가 아니다.

CPU parser regression: `python3 scripts/test-owned-graphics-diagnostics.py NgxPresentationEvidence`. 이는 synthetic parser 검사이며 GPU 증거가 아니다. 이 native public consumer의 PASS는 실제 게임 Streamline loader/proxy compatibility를 증명하지 않는다.

## 독립 public FG indexed-output acceptance

동일 closed-list replay는 이미 시작된 Prepare/Generate/Publish를 모두 건너뛰어 원래 GPU outcome과 history를 보존한다. 따라서 정상 frame5의 flag0·모든 pixel은 그대로이며 bootstrap frame0와 stale frame6의 flag1·untouched sentinel도 replay 후 동일해야 한다. 별도의 **새 Evaluate에 stale real ID를 전달한 경우**의 actual0/flag1과 구분한다. Replay flag 실패 시 실제/기대 flag와 원래/현재 RGBA probe를 함께 기록한다.

NGX FG의 별도 headless public consumer는 같은 `owned-d3d12-ngx-features` PE에 `--fg-only`를 전달한다. N1..4의 `DLSSG.MultiFrameIndex=1..N`을 각기 단일 `OutputInterpolated`로 소비하며 independent list와 한 grouped list를 모두 기록한다. 실제 GPU disable-buffer 첫 byte와 output readback으로 first/reset/disabled/stale actual0의 untouched sentinel, 정상 actual phase의 flag0·distinct outputs와 static interior의 독립 temporal red oracle(이전 red→현재 red의 normalized-phase 선형 보간, 오차 ≤1 byte), immutable guide/index/flag-buffer 경계, parameter/feature release-before-submit과 replay를 확인한다. `OWNED_D3D12_NGX_PHASE` 140 rows는 frame/count/index/normalized phase/actual/grouped/actual_count/output_flag/untouched/probe_red를 기록한다. 별도의 13 black real frames→white real frame 전환은 caller reset 없이 N1..4의 모든 phase가 GPU flag1·untouched로 억제되는지 `OWNED_D3D12_NGX_SCENE` 4 rows로 검사한다. terminal은 `OWNED_D3D12_NGX_FG_PASS capacity=4 independent=1 grouped=1`이다. 이 경로는 RR/SR alpha·native presentation·실게임 FPS 증거를 대신하지 않는다.

```sh
: "${OWNED_RUNTIME:?현재 NGX fresh staged runtime}"
: "${NGX_FEATURES_PE:?동일 public features source로 빌드한 x64 또는 x86 PE}"
: "${GUEST_ARCH:?x64 또는 x86}"
: "${FG_OUT:?아직 존재하지 않는 evidence 디렉터리}"
python3 scripts/test-owned-graphics.py --runtime "$OWNED_RUNTIME" --guest-arch "$GUEST_ARCH" \
  --fixture "$NGX_FEATURES_PE" --suite ngx-fg --mode no-present \
  --validation --timeout 180 --out "$FG_OUT"
```

`ngx-fg`는 RR oracle 없이 위 indexed-output acceptance만 실행한다. 일반 owned backend/native compiler/loaded builtin nvngx/Metal validation/foreign-backend 차단과 frozen source·PE hash receipt는 그대로 요구한다. `stdout.log`, `stderr.log`, `result.json`이 evidence이며 CPU `GameSuiteEvidence`는 synthetic protocols의 누락·중복·잘못된 phase/count/flag/sentinel/color/scene/reset과 full/RR/alpha 혼용을 거부한다. GPU 실행 결과는 이 fixture와 일치하는 실제 staged runtime의 artifacts로 별도 판단해야 한다.

## 독립 public alpha acceptance

동일 `owned-d3d12-ngx-features.cpp`/paired MSVC helper로 빌드한 PE의 `--alpha-only`는 기존 `runPublicAlpha`만 NGX Init → feature1 Create/Evaluate → parameter/feature release → GPU execute/closed-list replay → Shutdown 순서로 실행한다. 별도 fixture나 RR fallback을 만들지 않는다.

```sh
: "${OWNED_RUNTIME:?현재 NGX fresh staged runtime}"
: "${NGX_FEATURES_PE:?동일 public features source로 빌드한 x64 또는 x86 PE}"
: "${GUEST_ARCH:?x64 또는 x86}"
: "${ALPHA_OUT:?아직 존재하지 않는 evidence 디렉터리}"
python3 scripts/test-owned-graphics.py --runtime "$OWNED_RUNTIME" --guest-arch "$GUEST_ARCH" \
  --fixture "$NGX_FEATURES_PE" --suite ngx-alpha --mode no-present \
  --validation --timeout 180 --out "$ALPHA_OUT"
```

Strict acceptance는 기존 `public_alpha` case 하나, replay 0/1의 실제 `NGX_ALPHA_PIXELS` left/right RGBA와 caller-authored RGB(112,96,80)의 최대 오차 ≤8·비불투명 alpha contrast, `NGX_ALPHA_PASS cases=1`을 요구한다. 일반 owned backend/native compiler/loaded builtin nvngx/Metal validation gates도 유지한다. `stdout.log`, `stderr.log`, frozen source/PE hashes, `result.json`이 evidence다. CPU parser 검사는 기존 `GameSuiteEvidence`의 missing/duplicate/failure/numeric mutation matrix를 재사용한다.

현재 public alpha fixture의 alpha contrast 범위는 left≤96, right144..240이고 입력 flat-region alpha는48/192다. RGB 최대 오차는 보고된 필드를 믿지 않고 실제 두 pixel 값으로 다시 계산한다. 이 bounded public RGBA8 acceptance는 private SR alpha의 7-case scalar-oracle proof나 image-quality equivalence를 대신하지 않는다. Focused/full RR/private alpha 증거 혼용·extra terminal·GPU device failure를 거부하는 CPU regression은 `python3 scripts/test-owned-graphics-diagnostics.py GameSuiteEvidence.test_ngx_alpha_cannot_replace_full_rr_or_private_alpha_evidence`다.

이 suite는 RR을 실행하지 않아 A3 alpha acceptance를 RR과 독립적으로 판단한다. **기존 `ngx-features`의 7 case·RR oracle·native shader validation 실패 gate는 바뀌지 않으며** RR native fault를 alpha PASS로 가리거나 RR PASS로 바꾸지 않는다.

## Manual R32 public ABI proof

[Original x64 public NGX regression](../build/local/r32-exposure-20261002-ngx-original-shader/result.json)은 API+Shader Validation PASS다. 같은 directory의 [manual-R32 MSVC ABI smoke stdout](../build/local/r32-exposure-20261002-ngx-original-shader/stdout-manual-r32.log)은1.23456705 exposure, mean0.439215686 및 execute 전 feature/parameters/source release를 확인했다. 이것은 추가 smoke이며 영구 suite 전체 결과나 DLSS image-quality equivalence가 아니다. R32 numeric half 비교·GPU replay refresh·gated completion은 [양 ABI complete temporal proof](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료)가 담당한다. Retained original NGX DLL은 proprietary prebuilt binary로 source를 복사한 owned 구현이 아니다.

## Evidence 판독과 역사

Success marker 하나만으로 PASS를 정하지 않는다. `result.json`의 pass/exit/timeout, runtime verification, loaded backend/native identity, foreign fallback 부재, validation errors와 stdout의 expected cases를 함께 확인한다. Temporal ordering/lifetime을 검증하려면 [TEMPORAL-TEST](TEMPORAL-TEST.ko.md)와 [completion trace](TEMPORAL-COMPLETION-TEST.ko.md)를 실행한다.

2026-09-19 최초 fixture 작성자는 build-only 검사를 수행했고 당시 QI/GetDevice out-param 오류 때문에 첫 backend positive run이 실패했다. 그 오류는 수정됐다. 당시 “positive backend가 아직 실패 baseline”이라는 설명은 **현재 runtime 상태가 아니다**. 이후 Metal 4 temporal fixture 결과는 이미 관찰됐지만 전체 conformance·physical presentation·actual-game proof가 아니다. 삭제된 옛 PE hash를 현재 빌드의 expected hash로 고정하지 않는다.
