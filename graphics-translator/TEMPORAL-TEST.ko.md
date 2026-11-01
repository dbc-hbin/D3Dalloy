# D3D12 temporal COM backend 검증

`graphics-translator/tests/owned-d3d12-temporal.cpp`는 NGX frontend 없이 project-owned COM extension을 직접 호출한다. 현재 backend는 **Metal 4/Metal4FX**이며 classic D3D11 scaler와 독립이다. Public ABI·backend 구현은 [NGX-INTEGRATION](NGX-INTEGRATION.ko.md), completion trace 검증은 [TEMPORAL-COMPLETION-TEST](TEMPORAL-COMPLETION-TEST.ko.md), 실제 NGX DLL 검증은 [NGX-TEST](NGX-TEST.ko.md)를 따른다.

현재 지원·실행 증거는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)가 기준이다. 아래 acceptance는 fixture가 요구하는 계약이며 이번 문서 개편에서 새 GPU 실행을 했다는 뜻이 아니다.

## ABI와 typed/typeless baseline

Fixture의 project-owned COM 선언은 루트 `src/d3d12/d3d12_interfaces.hpp`와 일치해야 한다. Device/List/State UUID, `TemporalUpscale` 뒤 `ValidateRecording` slot, descriptor 첫 `StructSize`를 보존한다. 현재 fixture layout assertion은 create40 bytes, frame x64 184/x86 148 bytes, Color offset x64 8/x86 4다. 자세한 UUID·method 순서는 integration 문서에서 관리한다.

Create는 64×64 input, 128×128 logical output, flags74(`MVLowRes|DepthInverted|AutoExposure`), node mask 1을 사용한다. Typed와 `typeless_zzz` 각각 8 continuous frame을 실행한다.

| Resource | Typed | Typeless parent variant |
|---|---|---|
| Color | R8G8B8A8_UNORM | R8G8B8A8_TYPELESS(27) |
| Output | R8G8B8A8_UNORM, 136×136 UAV backing | R8G8B8A8_TYPELESS(27), 같은 backing |
| Motion | R16G16_FLOAT, zero MV | R16G16_TYPELESS(33) |
| Depth | D32_FLOAT_S8X24_UINT, 실제 DSV clear | R32G8X24_TYPELESS(19), typed DSV 20 |

Jitter 0, MV scale 1, semantic exposure 1, optional exposure/bias/transparency null, **8 frame 모두 caller ResetHistory=false**다. Typeless parent variant는 과거 ZZZ resource metadata를 재현한 synthetic case이며 실제 게임 실행이 아니다. Backend는 motion parent를 RG16Float typed view로 정규화한다.

Output 전체를 frame별 sentinel로 채우고 UAV 전후 resource transition과 전체 136×136 readback을 기록한다. Logical rectangle은 최소 75% pixel 변경, finite RGB mean >0.05, input grayscale mean 대비 delta ≤0.20을 요구한다. Rectangle 밖 pixel은 byte-exact sentinel이어야 한다. 이 넓은 mean bound는 same-frame sanity이지 PSNR·감마·샤프닝·DLSS 품질 등가 acceptance가 아니다.

## Manual R32 numeric exposure

AutoExposure off일 때 optional null 또는1×1 R16/R32를 지원하며 auto는 supplied exposure를 무시한다. Fixture는 GPU RTV-produced R32 **0.3333/1.234567/3.14159**와 uploaded half **3555/3cf0/4248**를 비교한다. 매 execution/replay의 GPU numeric conversion이 새 GPU write를 읽어야 하며 record-time cache는 허용하지 않는다. 현재 양 ABI API+Shader proof에서 모든 output maxULP0·changed-exposure sensitivity·unsignaled queue gate 뒤 caller resource/state final release와 list reset-before-completion을 확인했다([CORE 증거](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료)).

## Recording·replay·generation

Valid open-list control이 먼저 exact S_OK여야 한다. Control이 실패하면 공통 early failure로 negative case를 가리는 PASS를 허용하지 않는다.

- state release-before-Execute: Close 뒤 caller COM state를 release하고 Execute/readback한다. Recorded frame이 state/resource/scratch를 retain해야 한다.
- 동일 closed list는 **앞 실행 완료 후** replay한다. Caller reset false이면 같은 generation reset `1,0`, caller true replay는 매번 1이다.
- A(backing 64×64)·B(backing 80×80)를 둘 다 실행 전에 기록한다. Active content는 둘 다 64×64다. A1→B1→A2→A3의 effective reset은 `1,1,1,0`이어야 한다.
- 동적 출력 descriptor 재사용은 1280×720 input에서 1920×1080→1600×900→2560×1440→1920×1080을 실행한다. 각 recording을 두 번 replay하고, caller state 해제 뒤에도 결과와 output 바깥 sentinel을 보존하며 마지막 A가 처음 A와 일치해야 한다.
- 옛 dynamic recording 재실행은 같은 input의 A=1600×900, B=1920×1080에서 A→B→A를 검사한다. B를 retire하고 A output을 sentinel로 다시 채운 뒤 replay한 결과를 독립적인 fresh-reset A와 비교한다. 이 두 검사에서 사용하던 64×64 input·96/112/128 크기의 초소형 동적 출력 시나리오는 제거했다. Typed/typeless·수명·경계 검사용 작은 resource와 업스케일 품질 평가를 혼동하지 않는다.
- Reset은 이미지에서 추정하지 않고 actual `temporal_encode` trace와 비교한다. `last_executed_generation`이 현재 backend 기준이며 옛 generation-local initialized flag 설명으로 돌아가지 않는다.

## Output subrect와 exact HRESULT

Positive output subrect는 opt-in true, base `(3,5)`, logical 128×128, backing 136×136이다. Record S_OK 뒤 rectangle 내부 output과 **rectangle 밖 전체** sentinel을 GPU readback한다. Nonzero output base를 무조건 E_NOTIMPL로 적은 옛 fixture 설명은 대체됐다.

| Case | Exact HRESULT |
|---|---|
| 유효한 open-list control·enabled output rectangle | S_OK |
| unknown create flag `0x80000000` | E_NOTIMPL |
| closed ValidateRecording/TemporalUpscale | E_FAIL |
| NaN jitter | E_INVALIDARG |
| 현재64×64 input backing을 넘는 ColorBaseX=1·DepthBaseY=1 | E_INVALIDARG |
| MotionBaseX=UINT_MAX·missing output·opt-in 없는 output base | E_INVALIDARG |
| single-node CreateCommandList NodeMask=2 | E_INVALIDARG |
| DIRECT allocator + COMPUTE list mismatch | E_INVALIDARG |

Future-signal same-feature cross-queue, submitted list Reset/allocator retention, opposite X/Y order도 **같은 현재 fixture**에 포함된다. 별도 옛 completion PE를 빌드하지 않는다. 상세 시퀀스는 [completion](TEMPORAL-COMPLETION-TEST.ko.md)에 있다.

## 현재 루트에서 fresh 빌드·실행

모든 명령은 저장소 루트에서 실행한다. [README](../README.md)의 현재 루트 build/install을 먼저 완료한다. `OWNED_INSTALL`과 `WINE_SOURCE`는 각각 실제 x64 Meson install·복제할 Wine package다. 새 destination만 사용한다. 이 COM fixture에는 `--with-nvngx`가 필요하지 않다.

```sh
: "${OWNED_INSTALL:?현재 루트의 x64 Meson install 경로}"
: "${WINE_SOURCE:?복제할 원본 Wine runtime 경로}"
TOOLCHAIN="${TOOLCHAIN:-/opt/llvm-mingw-20260616-ucrt-macos-universal/bin}"
mkdir -p build
TEMPORAL_WORK="$(mktemp -d "$PWD/build/temporal-check.XXXXXX")"
"$TOOLCHAIN/clang++" --target=x86_64-w64-mingw32 \
  -std=c++17 -O2 -Wall -Wextra -Werror -static \
  graphics-translator/tests/owned-d3d12-temporal.cpp \
  -o "$TEMPORAL_WORK/owned-d3d12-temporal.exe" -ld3d12 -ldxgi -lole32 -luuid
python3 scripts/stage-owned-graphics.py stage \
  --d3dalloy-install "$OWNED_INSTALL" --wine-source "$WINE_SOURCE" \
  --wine-dest "$TEMPORAL_WORK/runtime" \
  --native-arch x86_64 --windows-abi x64
python3 scripts/test-owned-graphics.py \
  --runtime "$TEMPORAL_WORK/runtime" --guest-arch x64 \
  --fixture "$TEMPORAL_WORK/owned-d3d12-temporal.exe" \
  --suite temporal --mode no-present --validation \
  --out "$TEMPORAL_WORK/run" --timeout 120
```

Runner는 fresh out/prefix, runtime verification·fixture freeze·hash·load identity·validation을 요구하고 성공한 temporal run에서 trace analyzer도 자동 실행한다. API wait/queue completion case는 fixture 내부 15초 watchdog으로 bounded다. Runner timeout은 전체 run 제한이다.

WoW64은 matching i386 fixture를 `--target=i686-w64-mingw32`로 빌드하고 matching x64+i386 staged runtime을 사용한다. `--d3dalloy-install32`를 stage에 추가하고 runner guest assertion은 `--guest-arch x86`이다. **x64 PE를 x86 옵션으로 바꾸는 것만으로 WoW64 시험이 되지 않는다.** ARM64 Wine/FEX acceptance와도 별개다.

## PASS·증거의 의미

Current final marker는 `OWNED_D3D12_TEMPORAL_PASS`이며 typed_frames=8, typeless_frames=8 외에도 output_subrect, crossqueue_future_signal, submitted_list_reset, record_head_growth, allocator_retention, opposite_feature_order coverage를 포함한다. Runner는 typed/typeless frame 0..7을 정확히 검사하고 temporal trace ordering/lifetime 오류가 있으면 PASS를 거부한다.

`result.json`, stdout/stderr, owned backend trace, `temporal-trace-analysis.json`을 함께 판독한다. GPU readback/trace PASS는 physical compositor·HDR scanout·실제 게임·다른 GPU/OS 지원을 증명하지 않는다. `--mode present`를 임의로 붙여 이 offscreen temporal fixture를 physical presentation 시험으로 바꾸지 않는다.

2026-09-19 최초 regression의 COFF/PE hash와 private `temporal-regression-build` 경로는 build-only 역사다. 옛 기록을 현재 retained artifact라고 부르거나 현재 PE expected hash로 쓰지 않는다. 이후 Metal 4 temporal의 양 ABI 실행은 이미 관찰됐으며 현재 recovery temporal proof 두 건은 combined acceptance에 **이미 포함**된다. 별도로 더해 집계를 부풀리지 않는다([CORE](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)).
