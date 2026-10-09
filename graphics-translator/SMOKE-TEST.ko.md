# Owned D3D12 + DXGI fixture 검증

## 문서 범위와 판정

현재 소스 빌드는 [README.md](../README.md#빌드), LLVM/Wine SDK와 debug controls는 [DEVELOPMENT.md](../doc/DEVELOPMENT.md), staging은 [RUNTIME.ko.md](RUNTIME.ko.md)가 기준이다. 여기서는 **현재 fixture를 다시 빌드해 새 private runtime·prefix에서 실행하는 방법**과 기록된 관측을 분리한다. 아래 명령은 반복 절차이며 실행한 named result만 PASS 증거다. 새 P0 공개 계약·VRS/sharing 결과는 [CORE P0 ledger](CORE-IMPLEMENTATION-STATUS.ko.md#p0-공개-계약-정합)를 따른다.

지원 matrix·검증 경계는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거), 미완료 작업·실행 선택은 [PRIORITIES](IMPLEMENTATION-PRIORITIES.ko.md), 날짜별 관찰은 [HISTORY](IMPLEMENTATION-HISTORY.ko.md)에서 관리한다. 과거 완료한 P0–P3 개발 단계와 새 backlog P0–P4는 다르다. Named acceptance·mixed-cut recovery를 전체 conformance나 최신 단일cut 실행으로 읽지 않는다.

## CPU/tooling 검증

저장소 루트에서 필요한 범위만 실행한다.

문서/packaging 회귀에는 `scripts/test-owned-documentation.py`, `test-owned-arm64x-build.py`, `test-owned-arm64-package.py`도 있다. ARM64X의 선택적 실제 SDK graph 검증은 외부 Wine SDK를 가리키는 `--sdk-root` 없이는 skip되며, mock/CPU PASS를 target build나 GPU 실행으로 합산하지 않는다. Wine runner 회귀는 [Wine-darwin](../../Wine-darwin/README.md)의 `scripts/test-owned-wine-arm64-runner.py`로 이동했다. 새 correctness 회귀와 실제 적용 결과는 [리뷰 ledger](../doc/D3D12-REVIEW-AND-DESIGN-2026-10-07.md)를 따른다.

```sh
python3 scripts/test-owned-graphics-build.py
python3 scripts/test-owned-graphics-staging.py
python3 scripts/test-owned-graphics-diagnostics.py
python3 scripts/test-owned-ngx-frontend.py
```

- `build`: frozen import builder의 pin/patch/input/artifact 계약이며 현재 루트 translator 전체 빌드가 아니다.
- `staging`: 격리 복사·manifest·ABI/architecture·wrapper·dependency gate.
- `diagnostics`: runner의 suite marker/readback/validation 판정.
- `ngx-frontend`: 현재 소스에서 frontend DLL을 CPU/PE로 만들고 exports/MSVC parameter ABI를 검사한다. GPU나 NVIDIA DLSS 품질 증거가 아니다.

`enable_tests=true`의 업스트림 DX11 samples 및 수동 CPU `*.test.cpp` probes와 공용 PE/GPU runner는 별개다. 일반 project-wide lint/자동 GPU suite가 있다고 가정하지 않는다.

자동 PSO disk cache의 실제 WinAPI·파일·교차 프로세스 회귀는 준비된 private runtime과 **존재하지 않는 output**으로 실행한다. Production cache 코드를 그대로 컴파일한 CPU probe이며 GPU 또는 실게임 완료 증거는 아니다.

```sh
python3 graphics-translator/tests/run-automatic-cache-cpu.py \
  --runtime build/local/runtime-smoke-001 \
  --out build/local/automatic-cache-cpu-001
```

내용 readback, 256MiB 한도에서 같은 key 교체, 다른 프로세스의 저장·clear, rename 실패와 mutex 소유자 종료 뒤 복구, foreign file 보존을 검사한다. Runner는 전용 prefix/TEMP를 만들고 해당 Wine 종료 뒤 제거한다. `metadata_enumerations`는 256회 작은 저장의 관측값이며 시간 기반 합격 기준이 아니다.

실제 셰이더 실패 입력이 필요하면 미리 만든 **Windows 경로**를 `DXMT_SHADER_FAILURE_PATH`로 지정한다. 초기 VS/PS/CS 컴파일 실패에서 stage·bytecode·root-signature 조합별 최대64개를 `.failure` 디렉터리에 저장한다(`shader.cso`, `rootsig.bin`, compilation arguments의 `metadata.txt`, `error.txt`, module-relative caller `stack.txt`). 기본값은 꺼짐이며 모든 shader variant/native PSO 실패를 포괄하지 않는다. 게임 소유 bytecode와 실행 경로가 포함되므로 private evidence로 보관하고 저장소에 추가하거나 외부에 전송하지 않는다.

## Conservative Tier3 public proof

현재 cap은 **ConservativeRasterizationTier3**, 공개 상한은 **SM6.5/FL12_1**이다. [현재 완료 계약·8개 named 결과](CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof), [before/provisional·WoW64 실패→DATA32 교정·D3D11/GPTK 이력](IMPLEMENTATION-HISTORY.ko.md#p2-13-conservative-tier3--2026-10-04)을 구분한다. 최종 conservative는 runtime03 양 ABI cap3·exit0/pass=true·API+Shader Validation·validation/foreign 오류0으로 PASS했다. 전체 acceptance8/8은 **runtime03 conservative2회+depth-bounds/statistics/multisample x86 3회와 retained runtime02 x64 risk3회**이며 동일 build8회가 아니다. Runtime03 manifest는 `59227040a45afec8a049038264e3ff4dbe39f5133bfc34336811f7406e5e0149`, runtime02는 `05c99a341000136e37453ae6eb12b5c14ee99e6577534297850374cb77edccc9`다. Provisional cap1 terminal exit1과 새 edges 전 runtime02 x64 conservative PASS는 역사 증거이고 위8개에는 세지 않는다. [최종 CPU diagnostics90 PASS](../build/local/conservative-tier3-20261004.uFBr69/diagnostics-mirrors.log)·D3D11 PS 생성 smoke는 별도 관찰이다.

`tests/owned-d3d12-conservative-raster.cpp`는 `main` entry다. 공식 llvm-mingw의 현대 DirectX headers로 x86_64/i686을 `-std=c++20 -O2 -static`·**`-municode` 없이** 빌드하며 아래 기존 link flags를 쓴다. Native DXC1.9.2609.5의 genuine DXIL opcode92와 Microsoft FXC10.1/native `d3dcompiler_47=n`의 ps_5_1 DXBC InnerCoverage/SFI를 사용한다. [Producer hashes·commands·frozen 입력 provenance](tests/dxil/conservative-raster-provenance.json)를 따른다. Coverage+InnerCoverage의 동시 입력은 두 실제 producer가 거부해야 하며 fabricated blob이나 fake builtin으로 대체하지 않는다. Existing conservative-distance DXIL은 retained 입력이다. 재생성 시 `tests/dxil/build-conservative-raster.sh <wine> <dxc.exe> <전용-prefix> <존재하는-out-dir> conservative-raster <native-fxc.exe> <존재하는-evidence-dir>`를 사용하고 retained distance 입력도 같은 fixture directory에 제공한다.

아래는 fresh paired runtime·새 output을 사용하는 재현 recipe이며 보관된 결과를 덮어쓰지 않는다. `CONSERVATIVE_RUNTIME`은 paired stage, `CONSERVATIVE_WORK`는 새 출력 디렉터리, `LLVM_MINGW`는 공식 compiler bin 경로다. Exclusive GPU window에서 Main 단일 runner가 두 ABI를 직렬 실행하고 첫 실패에서 중단한다.

```sh
mkdir -p "$CONSERVATIVE_WORK"
for abi in x64 x86; do
  case "$abi" in x64) target=x86_64;; x86) target=i686;; esac
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-d3d12-conservative-raster.cpp \
    -ld3d12 -ldxgi -ld3dcompiler -ldxguid -lole32 -luuid \
    -o "$CONSERVATIVE_WORK/conservative-tier3-$target.exe" || exit "$?"
  python3 scripts/test-owned-graphics.py --runtime "$CONSERVATIVE_RUNTIME" \
    --suite conservative-raster --validation --guest-arch "$abi" --timeout 600 \
    --fixture "$CONSERVATIVE_WORK/conservative-tier3-$target.exe" \
    --dxil-dir graphics-translator/tests/dxil --mode no-present \
    --out "$CONSERVATIVE_WORK/public-$abi" || exit "$?"
done
```

최종 acceptance는 actual cap3·exit0/pass=true·API+Shader Validation·owned backend 확인·validation/foreign 오류0과 retained 전체 geometry/ClipDistance/CullDistance matrix를 요구한다. Numeric oracle는10 families×7=70 Tier3 grid rows·18 DXIL/DXBC early/late depth/stencil/UAV/occlusion interactions·16K precision·degenerate point/line·winding/cull/Vertex0/depth·helper·SampleMask·forced4·OFF/ON toggle·indirect/bundle/replay와 새 AS/MS FrontCounterClockwise cull·near/far DepthClip ON/OFF4 rows를 검사한다. ON은 각 covered28·mandatory10, ClipDistance3e066666·depth00000000/3f800000, OFF는 각 covered64·mandatory64, ClipDistance3f31999a·depth39000000/3f7ff800의 exact comparison이다. Before의 post-snap-line 실패는 Tier1이 허용하는 cull이며 과거 Tier1 proof를 무효화하지 않는다. 기존1/128 ambiguity band는 유지하고 Tier3-specific exact-grid bounds는 별도로 검사한다. OFF/nontriangle에서 undefined인 InnerCoverage 특정 상수는 요구하지 않는다. 새 재현에서는 paired cut의 `depth-bounds`, `pipeline-statistics`, `multisample-capabilities`도 ABI별 직렬 실행하고 cut/manifest를 각각 기록한다. 위 관찰된8회는5개 runtime03+3개 retained runtime02의 mixed-cut이지 이 새 recipe를 동일 cut에서 실행했다는 주장이 아니다.

## Depth bounds 50-case focused proof

아래는 최초 native50-case의 보존된 recipe/관찰이다. 현행 지원은 Apple7+ emulation과 Apple10 native이며, 최종 forced/native 각각 양 ABI146-case proof는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)를 따른다. 아래 당시 native-only/이전 family 미지원 표현을 현재 capability로 읽지 않는다.

최종 실제 결과는 runtime-02 [x64](../build/local/p2-depth-bounds-20261004.S3Pmiu/final-depth-bounds-x64/result.json)·[WoW64](../build/local/p2-depth-bounds-20261004.S3Pmiu/final-depth-bounds-x86/result.json)이며 ABI별 cap1·50 CASE_PASS·replay 포함51 STATS·exit0·API+Shader Validation·validation/foreign 오류0이다. 같은 cut의 library/statistics/multisample 양 ABI 회귀까지 총8 GPU runs가 PASS했다([CORE ledger](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)). Apple M5 Pro/macOS27의 native AppleGPUFamily10 gate 범위이며 SM6.5/FL12_1·이전 Apple family 미지원은 그대로다.

`owned-d3d12-depth-bounds.cpp`는 `main` entry다. 공식 llvm-mingw의 현대 DirectX headers로 x86_64/i686을 `-std=c++20 -O2 -static`·**`-municode` 없이** 빌드하고 d3d12/dxgi/d3dcompiler/dxguid/ole32/uuid를 link한다. 관측한 executables는 `build/local/depth-bounds-public-20261004/depth-bounds-{x86_64,i686}.exe`다. 아래는 새 paired runtime·fresh output의 재현 recipe이며 보관된 결과를 덮어쓰지 않는다. `DEPTH_RUNTIME`은 fresh paired stage, `DEPTH_WORK`는 새 디렉터리, `LLVM_MINGW`는 공식 compiler bin 경로다. ABI 실행은 직렬로 하고 첫 실패에서 중단한다.

```sh
mkdir -p "$DEPTH_WORK"
for abi in x64 x86; do
  case "$abi" in x64) target=x86_64;; x86) target=i686;; esac
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-d3d12-depth-bounds.cpp \
    -ld3d12 -ldxgi -ld3dcompiler -ldxguid -lole32 -luuid \
    -o "$DEPTH_WORK/depth-bounds-$target.exe" || exit "$?"
  python3 scripts/test-owned-graphics.py --runtime "$DEPTH_RUNTIME" \
    --suite depth-bounds --validation --guest-arch "$abi" --timeout 180 \
    --fixture "$DEPTH_WORK/depth-bounds-$target.exe" \
    --dxil-dir graphics-translator/tests/dxil --mode no-present \
    --out "$DEPTH_WORK/public-$abi" || exit "$?"
  WINEPREFIX="$DEPTH_WORK/public-$abi/prefix" "$DEPTH_RUNTIME/bin/wineserver" -k
  WINEPREFIX="$DEPTH_WORK/public-$abi/prefix" "$DEPTH_RUNTIME/bin/wineserver" -w
done
```

기존 frozen DXIL9개는 `depth-bounds.hlsl`과 `build-depth-bounds.sh`를 함께 runner snapshot/hash에 보존한다. Producer를 바꾸지 않으면 재컴파일하지 않는다. 필요한 경우에만 `sh graphics-translator/tests/dxil/build-depth-bounds.sh <wine> <official-dxc.exe> <dedicated-CPU-WINEPREFIX> <fresh-output-dir>`를 쓰고 그 output을 `--dxil-dir`로 지정한다. Pinned 공식 NuGet **1.9.2609.5**의 실행 파일은 `toolchains/dxc-1.9.2609.5/package/build/native/bin/x64/dxc.exe`이며 [version](../build/local/depth-bounds-public-20261004/producer.version.log)·[hashes](../build/local/depth-bounds-public-20261004/producer.sha256)·producer stdout/stderr는 같은 evidence root에 있다. DXIL과 HLSL/script 입력을 함께 보존하고 live/game prefix를 producer에 쓰지 않는다.

Runner는 ordered50 cases·51 query rows·positive cap·terminal을 요구한다. Stored/incoming/oDepth, inclusive/inverted/NaN·state reset/bundle·noDSV/readonly/noPS, early/late UAV/discard/write/query, direct/indexed/indirect/replay·live overlapping primitives·mip/array·D16/D24/D32·4×MSAA per-sample의 exact GPU oracle를 유지한다. D16은 float `0x3333/65535`와 raw `0x3333`·EQUAL probe를 사용하며 tolerance를 넣지 않는다. [DepthBounds v1.3](https://microsoft.github.io/DirectX-Specs/d3d/DepthBoundsTest.html)의 exact statistics case는 ForceEarly=true다. Early PS count는 exact 검증하되 late PSInvocations는 실제 invocation 관찰값을 기록하며 post-bounds survivor 수와 같다고 assert하지 않는다([D3D11.3 §20.4.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#PipelineStatistics)). Pixels/UAV/depth/stencil/occlusion/binary hard assertions는 유지한다.

같은 fresh cut에서 `pipeline-library`·`pipeline-statistics`·`multisample-capabilities`를 각 paired fixture·새 output·API+Shader Validation으로 직렬 확인한다. Common multisample fixture의 낡은 RGBA8 4×UAV rejection은 제거됐고 기존 OPTIONS14 writable-MSAA TRUE 및 [전용80-case positive consumer](#writable-msaa-focused-proof)를 대신 거부 계약으로 바꾸지 않는다. 원래 실패와 수정된 최종 PASS, native probe52/56·noDSV mismatch4·exit1, GPTK static reference와 owned13-prefix cleanup은 [이력](IMPLEMENTATION-HISTORY.ko.md#p2-12-depth-bounds--2026-10-04)에 구분했다. Windows/HLK·differential conformance·실게임/FPS proof가 아니다.

## Advanced texture와 depth casting focused proof

Advanced의 실제 완료 증거는 runtime08 [x64](../build/local/p3-20261003/advanced-texture/runtime08-advanced-x64-public-validation/result.json)·[WoW64](../build/local/p3-20261003/advanced-texture/runtime08-advanced-x86-public-validation/result.json)의 ABI별24 ordered cases다. Runner `--suite advanced-texture`는 frozen `advanced-texture-*.dxil`62개와 producer HLSL/script를 복사·hash하지만 GPU body는 comparison6 shapes × CS/VS/PS18 + raw16/32/64 ×2D/array6이다. Opaque feedback의 숫자 자체가 아니라 `CheckAccessFullyMapped`와 data/guards를 비교한다. [현재 focused 범위와 depth/casting 연결](CORE-IMPLEMENTATION-STATUS.ko.md#advanced-texture-24-case-focused-proof)을 유지한다.

재현은 `owned-d3d12-advanced-texture.cpp` 또는 `owned-d3d12-depth-casting.cpp`를 x86_64/i686 paired consumer로 빌드하고, 실행마다 fresh output을 지정한다. Depth는 public `CreateCommittedResource3(COMMON)` 뒤 legacy transition을 사용하며 7 genuine shaders와8 data/status/per-sample oracles를 유지한다. Advanced consumer build는 현대 llvm-mingw DirectX headers를 사용하고 오래된 `-Iinclude/native/directx` override를 넣지 않는다. 최종 Advanced consumer는 device 생성 직후 실제 OPTIONS12/OPTIONS14 query에서 `RelaxedFormatCastingSupported`와 `AdvancedTextureOpsSupported`가 모두 TRUE인지 확인한 뒤 기존24 numerical cases를 실행한다. `HighestShaderModel`은6.5를 유지한다. 아래는 새 실행 recipe이며 기존 PASS를 다시 실행했다는 뜻이 아니다.

```sh
python3 scripts/test-owned-graphics.py --runtime "$TEXTURE_RUNTIME" \
  --suite advanced-texture --guest-arch x64 --fixture "$ADVANCED_X64_FIXTURE" \
  --dxil-dir graphics-translator/tests/dxil --validation --timeout 120 \
  --mode no-present --out "$FRESH_ADVANCED_OUT"
WINEPREFIX="$FRESH_ADVANCED_OUT/prefix" "$TEXTURE_RUNTIME/bin/wineserver" -k
WINEPREFIX="$FRESH_ADVANCED_OUT/prefix" "$TEXTURE_RUNTIME/bin/wineserver" -w
```

x64 PASS 뒤 `--guest-arch x86`와 실제 i686 fixture·다른 fresh output으로 직렬 실행한다. Depth는 `--suite depth-casting`·해당 executable로 바꾼다. First failure에서 중단하고 prefix를 stop/wait한 뒤 원본 logs/result/invocation/inputs를 보존한다. Runtime08 depth x64의 MSAA sample-count2/4 실패는 source query correction 이전 관찰이다. 수정된 runtime09의 `advanced-texture/runtime09-depth-{x64,x86}-01/result.json`은 각각8 cases·API+Shader Validation PASS이며, volume/STD/shared casting 전체 acceptance로 확대하지 않는다.

Reserved BC volume casting은 ordinary BC volume의 dense canonical registry와 별개다. 실제 reserved parent에는 canonical buffer를 추가하지 않고, 기존 opaque texture→buffer→texture blit으로 queue-ordered mapped sparse atlas pages를 UInt/BC 양방향 publish한다. NULL pages는 제외하고 8/16-byte block bits·full64KB pitch를 보존한다. 모든 source page를 먼저 캡처한 뒤 destination을 publish해 logical/mip tile과 physical heap alias가 staging 도중 서로의 입력을 덮어쓰지 않게 한다. `owned-d3d12-tiled-volume.inc`의 기존 RG32/RGBA16 same-dispatch, partial-W guards, native BC parent decode 및 cross-resource alias numerical oracles를 그대로 유지하며 x64·i686 GPU proof 전 완료로 세지 않는다. 기존 native parent dispatch와 같은 command list에서 public CopyTiles를 읽고 BC1/BC5의 해당64KB page 전체를 CPU bytes와 정확히 비교한다. OOB probe는 실제 edge page의 untouched bytes를, NULL page는 zero bytes를 검사한다. TEMP printf·추가 submission/wait 없이 RGBA/status와 opaque byte oracle을 함께 통과해야 한다.

Aliased logical pages의 전체 snapshot을 차례로 publish하면 unchanged sibling page가 실제 writer를 덮어쓸 수 있다. Queue encoding 시점의 `(native heap, physical tile)`로 같은 page를 묶고 pre-pass baseline을 보존한다. 공통 physical staging page의 초기값도 반드시 UInt import 이전의 BC baseline이어야 한다. 같은 heap의 UInt layout이 이미 BC interpretation을 바꾸므로 post-dispatch native BC snapshot을 초기값으로 쓰면 untouched selector bytes와 partial-W OOB page가 손상된다. 기존 `d3d12_cast_merge`가 변경 byte만 baseline에 합친 뒤 모든 logical alias에 동일한 결과를 upload한다. Runtime20은 i3의 stale alias를, Runtime21은 alias 복구 뒤에도 i2 upper selector byte와 i11 untouched page의 permutation을 관찰했다. Pre-import 초기값 변경 자체는 새 paired GPU numerical·full-byte proof 없이 완료 증거로 세지 않는다.

Private sparse RTV와 BC/UInt cast shadow는 texture owner 유지뿐 아니라 **현재 mapped backing heap을 실제 Metal4 submission residency set에 포함**해야 한다. `MTL4CommandBuffer::useAllocation`은 기존 native recording의 allocation/lease dedup을 사용하며 completion retirement까지 heap을 유지한다. Untracked RT-only sparse shadow에 texture만 resident로 등록한 native control은 clear/readback0이었고, mapped heap만 추가한 control은2 slices·8192 pixels exact를 API+Shader Validation에서 통과했다. Parent-view BC alias는 원래 parent mapping을 공유하고 별도 sparse map을 하지 않는다. 이 native proof를 public 전체 Tiled PASS로 대신하지 말고 기존 full64KB·NULL/remap·partial-W·RTV/copy oracles를 양 ABI에서 그대로 실행한다.

Clean runtime28의 `tiled-tier4/public-runtime28-{x64,x86}-predicate-contract21/result.json`은 이 원래 full64KB oracle를 포함한 전체76 cases를 API+Shader Validation에서 통과했다. Predicate 값1은 `NOT_EQUAL_ZERO`에서 skip, `EQUAL_ZERO`에서 execute다. Volume fixture의 입력 operation을 이 공개 규격에 맞췄으며 expected bytes·모든 mip·skip/execute 검사는 바꾸지 않았다. 같은 runtime의 `positive-runtime28-relaxed-casting-{volume,volume-standard,shared}-{x64,x86}-01/result.json`도 각각8/8/5 cases를 통과했다.

## Relaxed format casting positive-cap acceptance

최종 `owned-d3d12-relaxed-casting.cpp` consumer는 모든 mode에서 실제 OPTIONS12 query의 `RelaxedFormatCastingSupported=TRUE`를 필수로 요구한다. Default `--suite relaxed-casting`24 numerical cases는 committed/placed·legacy/explicit format list·native BC/UInt alias·sparse guards/replay와 기존 negative creation 계약을 그대로 검사한다. `relaxed-casting-shared`5 cases·`relaxed-casting-volume`8 cases·`relaxed-casting-volume-standard`8 cases도 같은 positive cap을 요구한다. 별도 capability-only 승격 mode는 없다.

각 ABI의 `main` consumer를 `-std=c++20 -O2 -static`(`-municode` 없이), d3d12/d3dcompiler/dxguid/ole32/uuid link로 빌드하고 `scripts/test-owned-graphics.py --suite <casting-suite> --runtime <fresh-paired-runtime> --guest-arch x64|x86 --fixture <paired-casting-exe> --validation --timeout 600 --out <fresh-output>`을 직렬 실행한다. `enhanced-barriers`의 OPTIONS12도 relaxed casting TRUE를 요구하며 나머지 barrier/interface/dynamic-state 계약은 유지한다. 지원되는 BC→UInt cast는 별도 relaxed24 numerical suite로 검증하며, enhanced-barriers의 nonBC→BC·unit-size mismatch·invalid layout 거부 계약은 그대로 유지한다. 새 positive-cap recipe는 이전 runtime의 false-cap diagnostic 기록을 소급 변경하지 않는다.

## Atomic64 physical-helper acceptance

Public helper fixture는 tests/owned-d3d12-atomic64-helpers.cpp다. tests/dxil/build-atomic64-helpers.sh의 기존 wine, genuine dxc.exe, dedicated prefix, output-directory 인자로5개 DXIL을 생성하고, x86_64/i686 clang++로 각각 -std=c++20 -O2 -static -municode 및 d3d12/dxgi/d3dcompiler/ole32/uuid를 link한다. Runner는 --suite atomic64-helpers --fixture <paired-exe> --dxil-dir <producer-output> --guest-arch x64 또는 x86 --runtime <fresh-owned-runtime> --validation --out <fresh-output>을 사용한다. 각 ABI를 직렬 실행하며 source/producer/container hashes, owned native compiler,11개 ordered mode와1×/4×/8× sample 계약, terminal result 및 실제 numerical mismatch를 검사한다.

CPU diagnostics는 scripts/test-owned-graphics-diagnostics.py이며 public GPU proof와 별개다. Runtime09의 첫 x64 실행은 VS capture PSO 준비 E_FAIL로 중단했다. Native bitmap의8×8 contention PASS·64×64 Hang도 별도 관측이다. 최신 상태와 실패/수정 증거는 [HISTORY의 physical helper 기록](IMPLEMENTATION-HISTORY.ko.md#새-p3-15-atomic-phase-physical-helper-promotion--source-cut-2026-10-03)을 따른다. 단일 helper 실행을 전체 Shader Model conformance 증거로 확대하지 않는다.

## Atomic64 groupshared acceptance

`tests/owned-d3d12-atomic64.cpp`를 동일한 paired fixture flags (`-std=c++20 -O2 -static -municode`, d3d12/dxgi/d3dcompiler/ole32/uuid)로 빌드한다. Frozen `sm6-cs_atomic64_groupops.dxil`, `sm6-cs_atomic64_groupstress.dxil`, `sm6-cs_atomic64_groupcapacity.dxil`의 producer는 `tests/dxil/atomic64-groupshared.hlsl` 및 기존 `build-sm6.sh`다.

- `--suite atomic64-groupshared --validation`:9 연산·compare/wrap,64개 group 각각1024 full64 반환 순열·최종값.
- `--suite atomic64-groupshared-capacity --api-validation`:32KiB groupshared4096 슬롯의 old/final. Runner가 fixture에 `--capacity`를 전달한다. Shader Validation의 추가 threadgroup storage 때문에 이 검사는 API-only임을 결과에 명시한다.

두 suite 모두 `--fixture <paired-exe> --dxil-dir graphics-translator/tests/dxil --runtime <fresh-runtime> --guest-arch x64|x86 --out <fresh-output>`를 사용한다. ABI·suite는 직렬 실행한다. Runner는 실제 backend identity, native compiler, 정확한 case 순서, full oracle result 및 terminal을 모두 요구한다. Runtime15 paired4 runs는 [HISTORY](IMPLEMENTATION-HISTORY.ko.md#새-p3-15-atomic-phase-physical-helper-promotion--source-cut-2026-10-03)에 기록했다. Graphics/typed/전체 Atomic64 cap 증명과 구분한다.

## Atomic64 whole-RMW / native intersection 회귀

`dxil_atomic64.cpp`는 initial read와 모든 CAS retry를 포함한 RMW 전체를 physical SIMD elected lane에서 실행한다. READ/Update wrapper를 먼저 scalar로 만들고 최종 phase legalization에서 협력을 적용한다. 이 시점의 실제 `air.intersection` entry와 transitive direct-call graph에 속한 wrapper는 native intersection에서 SIMD intrinsic이 금지되므로 기존 scalar 알고리즘을 유지한다. 공유 helper도 intersection에서 도달 가능하면 scalar다. Shader 이름·fixture 입력에 따른 예외나 atomic 의미 변경은 없다.

Full `sm6` consumer는 기존 genuine input 집합과 `owned-d3d12-sm6.cpp` PE로 실행한다. `--suite sm6 --validation --timeout 600`에서51 numerical cases와 raw stress target1/16 각각65536 invocations·20 repetitions, full64 returned/final oracle를 모두 요구한다. Fixture의60초 fence deadline이나 invocation 수를 늘리거나 줄여 통과시키지 않는다. Groupshared32KiB의 별도 API-only 경계와 이 API+Shader Validation stress를 혼동하지 않는다.

RT는 `owned-d3d12-raytracing.cpp`의 기존 전체 atomic consumer를 `--suite raytracing-atomic64 --validation --timeout 600`으로 실행한다. `--dxil-dir`에는 genuine `raytracing-contract.dxil`·`raytracing-atomic64.dxil`을 함께 둔다. Runner가 `--atomic64` mode를 선택하며 native intersection PSO 생성뿐 아니라 direct/indirect raygen·hit/miss·callable·intersection/any-hit·alias의 실제 full64 oracle를 통과해야 한다.

두 suite 모두 `scripts/test-owned-graphics.py --runtime <fresh-paired-runtime> --guest-arch x64|x86 --fixture <matching-paired-exe> --dxil-dir <genuine-inputs> --suite <sm6|raytracing-atomic64> --validation --timeout 600 --out <fresh-output>`을 ABI별 직렬 실행한다. Whole-operation 협력이나 적용 시점을 바꾸면 typed·phase·producer·work-graph record/typed·coupled pixel·contention·physical-helper consumer도 함께 확인한다. [실패→수정 및 실제 proof](IMPLEMENTATION-HISTORY.ko.md#새-p3-15-raw-atomic64-stress-진행-보장--2026-10-04)를 보존한다.

## Atomic64 positive-cap consumer cutover

최종 consumer와 runner는 OPTIONS9의 `AtomicInt64OnTypedResourceSupported`·`AtomicInt64OnGroupSharedSupported`, OPTIONS11의 `AtomicInt64OnDescriptorHeapResourceSupported`를 TRUE로 요구한다. `HighestShaderModel` query는 보수적으로6.5를 유지하며, 일부 optional 기능이나 `lib_6_8` 입력 실행을 SM6.9/6.10 전체 mandatory conformance로 해석하지 않는다.

`tests/owned-d3d12-atomic64-typed.cpp`를 위 paired fixture/link flags로 빌드하고 기존 `tests/dxil/build-atomic64-typed.sh` producer output과 함께 `scripts/test-owned-graphics.py --suite atomic64-typed --runtime <fresh-paired-runtime> --guest-arch x64|x86 --fixture <paired-typed-exe> --dxil-dir <producer-output> --validation --timeout 600 --out <fresh-output>`을 ABI별 직렬 실행한다. Consumer는6 shapes ×2 signs의 operation/contention·ordinary32/raw alias·format/float32 oracle 전체를 실행한다. `--first-case`는 실제 submission 오류를 좁히는 fixture 직접 실행용이며 positive typed capability를 우회하지 않는다. Runner는 first-case 결과를 full acceptance로 인정하지 않는다.

`workgraphs-atomic64`·`workgraphs-atomic64-typed`도 기존 pinned preview SDK와 `main` entry/link 방식(`-municode` 없이)을 사용한다. `library_profile=lib_6_8`은 실제 입력 profile이며 typed/heap cap FALSE를 전제로 하지 않는다. 기존 record old/final/count/guard 및 typed physical-copy oracle는 그대로 요구한다.

## Atomic64 mesh groupshared / coupled pixel acceptance

Deferred compute recording은 각 SetPSO의 실제 native compiled pipeline을 recording lease에서 소유한다. 현재 command-list PSO를 바꾸거나 Reset해도 이전 ordinary/statistics/indirect command의 raw handle이 먼저 해제되지 않으며 replay 종료까지 유지된다. Typed64 consumer의 첫 operations 결과만으로 이를 검증할 수 없다: 같은 recording에서 transient contention PSO를 snapshot PSO로 교체한 뒤 1024 full64 반환 순열·final을 읽는 기존 `atomic64_typed_contention`까지 paired GPU 실행해야 한다. Runtime17의 operation12/type3(SetPSO) AccessViolation은 이 lifetime 회귀의 실제 실패 증거이며 수정 이후 pass와 구분한다.

Typed atomic phase의 ordinary compute/graphics PSO는 기존 shader argument reflection의 실제 register space·lower bound·count·stage visibility로 static descriptor tables와 root descriptors를 선택한다. 선언 range-ID bitmask를 D3D12 register 번호로 해석하지 않는다. Indirect graphics/compute도 같은 used-binding 계약을 사용하며 recording이 reflection을 소유한 PSO를 유지한다. Direct-indexed heap과 기존 library dispatch의 descriptor 수집은 기존 device weak registry를 확장한 live-owner 조회를 거친다: descriptor의 `owned_id`로 pointer ABA를 거부하고 private COM owner lease를 GPU retirement까지 유지한다. 미사용/만료 descriptor를 fixture에서 지우거나 heap 전체가 resource를 영구 보유하게 만들지 않는다. 별도 snapshot shader의 readonly UAV load도 UAV라는 logical resource class에 따라 native mip0를 선택한다; access_read optimization flag를 readwrite로 강제하지 않는다. 검증은 기존12 shape/sign·40 typed-format·float32 oracle 및 full64 contention→snapshot을 그대로 ABI별 직렬 실행하며, fixture oracle는 변경하지 않는다.

Volume atlas의 runtime layout header와 GPU residency status load는 기존 atomic64 transport metadata로 표시한다. 이 값들은 application UAV 주소가 아니므로 phase의 AS1 canonical backing 변환 대상에서 제외하며, 실제 texel·buffer load/store 및 atomic 연산의 canonical coherence는 그대로 유지한다. Shape 3 첫 dispatch 전 PSO 생성에서 header/status load까지 application access로 재작성되어 AIR가 폭증하던 경로를 차단한다. Native Metal4 PSO-only 검증은 GPU 제출 없이 genuine unsigned·signed 3D operations의 API/Shader Validation을 확인하지만, full typed 반환·contention·snapshot oracle과 WoW 경로의 paired GPU acceptance를 대체하지 않는다.

`tests/owned-d3d12-mesh-shader.cpp`의 `--groupshared64-only`는 기존 MS·AS full64 연산/compare/wrap/128개 반환 순열·최종값 및 payload pixels oracle만 실행한다. 전체 mesh 실행은 변경하지 않는다. Paired `-std=c++20 -O2 -static -municode` fixture에 runner `--suite atomic64-mesh-groupshared --validation --dxil-dir graphics-translator/tests/dxil`을 사용한다. 네 genuine DXIL(`ms_groupshared64`, `as_groupshared64`, `ms_derivative_payload`, `ps_color`)과 tracked producer/fixture provenance를 freeze하고 두 ordered numerical cases 및 focused terminal을 모두 요구한다.

PS acceptance는 별도 `tests/owned-d3d12-atomic64-pixel.cpp`와 `tests/dxil/atomic64-pixel.hlsl`이다. `sh tests/dxil/build-atomic64-pixel.sh <wine> <official-dxc.exe> <dedicated-CPU-prefix> <producer-output>`로18 genuine DXIL을 생성한다. 위 paired fixture flags 및 d3d12/dxgi/d3dcompiler/dxguid/ole32/uuid로 빌드하고, repo root에서 `scripts/test-owned-graphics.py --runtime <fresh-paired-runtime> --guest-arch x64|x86 --fixture <paired-pixel-exe> --dxil-dir <producer-output> --suite atomic64-pixel-coupled --validation --timeout 600 --out <fresh-output>`를 ABI별 직렬 실행한다. 기존11개 helper cases와 fixture는 그대로 유지한다.

40 ordered cases는 ROV/ordinary UAV × typed/structured64 × pixel/4× sample-frequency × late-depth-export/late-discard/early-depth-reject/early-discard/early-stencil-reject이다. CPU constants로 full64 old/final·high32 carry·동일 buffer의 ordinary32 alias 및 coupled color를 검사한다. ROV는 겹치는 두 primitive의 같은 pixel/sample 순서를 요구하고 non-ROV는 primitive별 cell을 분리하여 임의 GPU 실행 순서를 기대하지 않는다. 실제 depth/stencil은 CPU expected depth/ref의 positive visibility probes와 틀린 depth/ref의 negative probes로 확인하며 application occlusion·IA/PS query counts에서 내부 auxiliary pass나 논리적 helper의 효과가 새면 실패한다. 모든 verdict는 실제 GPU readback에 기반한다. Typed flag가 숨겨진 상태에서도 실행하는 명시적 probe이며 advertised Typed64 cap 완료로 해석하지 않는다. CPU build/producer 생성과 public paired GPU 통과 증거는 별개다.

## Pixel Atomic64 4096-fragment contention regression

같은 `owned-d3d12-atomic64-pixel.cpp`의 `--contention-only`는 기존40-case coupled oracle를 대체하지 않는다. 새 `tests/dxil/atomic64-pixel-contention.hlsl`은64×64 single-sample, non-ROV structured64 UAV에서 returning `InterlockedAdd`와 실제 무제한 CAS retry-loop add를 각각1/16 target cells로 실행한다. CAS의 첫 관측도 `InterlockedOr(..., 0, ...)` atomic이다. 초기값 `0xfffffffffffff000`, 증가값 `0x100000001`의 unsigned full64 wrap/carry를 검사하며, CPU는 target별 full64 returned-old multiset(합계4096개)·최종값·unused cells·buffer prefix/suffix guards와4096개 픽셀을 독립적으로 비교한다.

`sh graphics-translator/tests/dxil/build-atomic64-pixel-contention.sh <wine> <official-stable-dxc.exe> <dedicated-CPU-prefix> <producer-output>`로 genuine VS/Add/CAS DXIL3개 및 compiler version/SHA256·source/container hashes provenance를 만든다. 기존 paired fixture link flags로 같은 C++ source를 빌드한 뒤 `scripts/test-owned-graphics.py --suite atomic64-pixel-contention --runtime <fresh-paired-runtime> --guest-arch x64|x86 --fixture <paired-pixel-exe> --dxil-dir <producer-output> --validation --timeout 600 --out <fresh-output>`을 ABI별 직렬 실행한다. Runner는 `--contention-only`를 전달하고 producer provenance를 freeze하며4 ordered begin/result rows·full oracle case markers·terminal·실제 backend/native compiler를 모두 요구한다. 이 public backend-path 회귀는 별도 native bitmap64×64 Hang 재현/해결 증명을 대체하지 않으며, GPU 실행 전에는 CPU-ready로만 기록한다.

## Writable MSAA focused proof

실제 완료 증거는 paired runtime-06의 [x64](../build/local/p3-20261003/writable-msaa/runtime06-x64-01/result.json)·[WoW64](../build/local/p3-20261003/writable-msaa/runtime06-x86-01/result.json), ABI별80 ordered cases다. 아래는 재현 recipe이며 실행 전 latest owned paired runtime과 official DXC executable을 준비한다. Producer는 isolated CPU-only Wine prefix, GPU runner는 실행마다 별도 fresh prefix를 소유한다. GPU suites는 직렬로 실행한다.

```sh
: "${MSAA_RUNTIME:?fresh paired owned runtime absolute path}"
: "${MSAA_DXC:?official dxc.exe absolute path}"
LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
MSAA_WORK="$(mktemp -d "$PWD/build/local/writable-msaa-check.XXXXXX")"
graphics-translator/tests/build-writable-msaa.sh "$PWD/toolchains/wine-runtime/bin/wine" \
  "$MSAA_DXC" "$MSAA_WORK/producer-prefix" "$MSAA_WORK"
WINEPREFIX="$MSAA_WORK/producer-prefix" toolchains/wine-runtime/bin/wineserver -k || true
WINEPREFIX="$MSAA_WORK/producer-prefix" toolchains/wine-runtime/bin/wineserver -w
for pair in x64:x86_64 x86:i686; do
  abi="${pair%%:*}"; target="${pair#*:}"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static -municode \
    graphics-translator/tests/owned-d3d12-writable-msaa.cpp \
    -ld3d12 -ldxgi -ld3dcompiler -ldxguid -luuid -o "$MSAA_WORK/writable-msaa-$abi.exe"
  python3 scripts/test-owned-graphics.py --runtime "$MSAA_RUNTIME" \
    --fixture "$MSAA_WORK/writable-msaa-$abi.exe" --dxil-dir "$MSAA_WORK" \
    --suite writable-msaa --validation --timeout 180 --out "$MSAA_WORK/gpu-$abi"
done
```

Runner는 Options14 cap1, exact80-case 순서,2/4/8×single/array와 SINT/FLOAT marker, owned native compiler와 validation 오류0을 모두 요구한다. Typeless half NaN/subnormal/negative-zero 원시 bit와 float SRV, native CopyResource/raster/MIN/MAX resolve, uint/float clear를 실제 readback한다. Controlled CPU rejection seam은 `python3 scripts/test-owned-graphics-diagnostics.py ReviewEvidenceFailures.test_writable_msaa_requires_every_shape_scalar_and_native_compiler`이며 GPU 실행 증거가 아니다.

## Installed-runtime factory / diagnostics

`owned-d3d12-diagnostics.cpp`는 SDK version을 추정하지 않고 `D3D12GetInterface`의 공개 `CLSID_D3D12DeviceFactory`로 **현재 설치된 owned runtime**을 선택한다. 서로 다른 factory와 같은 factory의 설정 변경 전후 device를 생성하며 Debug/DRED snapshot 격리, global 적용 시 independent device 보존, factory Release 뒤 실제 upload→GPU copy→readback을 검사한다. 기존 여섯 case는 유지하며, 별도의 `agility` suite가 nonzero SDK618/version/path 선택과 실제 owned Core 수명을 검사한다.

공통 llvm-mingw 옵션으로 `graphics-translator/tests/owned-d3d12-diagnostics.cpp`를 `-std=c++17 -O2 -static -ld3d12 -ldxgi -ldxguid -luuid`로 빌드한 뒤 새 stage에서 직렬 실행한다:

Factory CLSID `114863bf-c386-4aee-b39d-8f0bbb062955`는 Microsoft [DirectX-Headers의 `d3d12.idl`](https://github.com/microsoft/DirectX-Headers/blob/main/include/directx/d3d12.idl)의 `DEFINE_GUID(CLSID_D3D12DeviceFactory, ...)`에서 가져왔다. 선택된 MinGW header에는 해당 CLSID 선언이 없어 consumer가 같은 public GUID를 선언한다. SDKConfiguration1은 실제 same-generation owned Core selector를 제공한다. Foreign/version 불일치는 계속 거부하며, header의 SDK 상수를 actual Core exported version으로 해석하지 않는다.

```sh
python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" \
  --fixture "$FIXTURE" --suite diagnostics --mode no-present \
  --api-validation --validation --out "$NEW_OUTPUT"
```

`OWNED_D3D12_DIAGNOSTICS_CASE_PASS case=agility_installed_factory_gpu_isolation`을 포함한 정확한 6개 case와 `PASS cases=6`, 실제 staged `winemetal.so` load를 모두 요구한다. CPU parser 회귀는 `python3 scripts/test-owned-graphics-diagnostics.py RunnerFailures.test_diagnostics_requires_factory_gpu_isolation_and_all_contracts`이며 GPU 증거가 아니다.

### Owned Core618 version/path·수명 acceptance

Core는 same-generation ordinary owned PE/native-only(`d3d12core=n`)이고 Main·나머지 builtin modules는 기존 `b` 계약을 유지한다. Core에 Wine builtin/placeholder DOS-stub signature를 붙이면 Wine의 basename coalescing이 서로 다른 SDK directory를 같은 module로 바꾸므로 새 stage가 이를 거부한다. 이 delivery 구분은 foreign Core fallback을 허용하지 않는다.

Runner의 실제 Wine module-load gate도 Core basename을 blanket whitelist하지 않는다. 해당 guest ABI의 installed physical Core 경로와 agility fixture의 `sdk-owned`/`sdk-alternate` 두 경로만 허용하고, 각 실제 PE의 ABI·SHA256을 same-generation manifest와 대조한다. Diagnostics는 복사 SDK directory 없이 installed native Core load를 요구한다. `owned_core_module_loads`에 경로·hash·generation·ABI를 기록하고 다른 경로/image 또는 builtin Core load는 foreign backend로 거부한다. CPU admission 회귀는 `python3 scripts/test-owned-graphics-diagnostics.py RunnerFailures.test_native_core_loads_require_owned_paths_bytes_and_guest_abi`이며 GPU 증거가 아니다.

새 paired stage의 wrapper는 `WINEDLLPATH`를 해당 runtime의 `x86_64-windows:i386-windows:x86_64-unix` 절대 경로로 고정하고 manifest `loader.winedllpath`에 상대 경로 정책을 기록한다. 이 값은 actual Main image와 같은 파일임을 입증할 installed-path discovery hint일 뿐 Core identity 우회나 foreign fallback이 아니다. Hint 없는 retained runtime은 원본 wrapper 계약대로 read-only 검증한다.

기존 lifetime/unload case에는 barrier-synchronized 두 thread의 bounded32 rounds를 포함한다. 매 round는 실제 Core가 미로드인 상태에서 SDK1 factory/configuration 생성과 `FreeUnusedSDKs`를 동시에 시작하고 SDK618·debug/experimental 값·public feature GUID·실제 Core module identity를 검사한다. Barrier 종료까지 factory/settings/configuration을 유지한 뒤 모두 해제하고 Core가 exact-path `GetModuleHandle`에서 실제로 사라져야 다음 round를 시작한다. Sleep·bare not-throw 판정은 없으며 cache의 cold-create/cleanup race와 live operation/handle lifetime을 함께 다룬다.

같은 lifetime case는 selected Core의 실제 public `D3D12GetInterface`를 직접 호출해 factory와 SDK618 configuration을 얻는다. 원래 device/child/settings/configuration-only 수명 검사를 그대로 수행한 뒤 direct factory만 selected module의 owner로 남겨 `FreeUnusedSDKs` 후에도 module과 public configuration이 유효함을 검사하고, 그 factory의 최종 Release 뒤 actual unload를 요구한다. 이는 Main import thunk가 Core export 주소처럼 보이는 경우를 public 동작으로 잡으며 vtable/module pin이나 assembly byte pattern에 의존하지 않는다.

Godot `e7cfa294a0b81bed7986be04a848cc1832a3f083`의 `project_settings.cpp:1863` 요청인 SDK618/package1.618.5를 대상으로 한 **owned compatibility-package identity**다. Microsoft SDK618 전체 기능 conformance나 foreign Microsoft/GPTK Core 지원이 아니다. 현재 루트를 양 ABI로 함께 build/install한 뒤 `--dxmt-install32`를 포함해 새 paired runtime을 stage한다. 기존 game/live prefix는 변경하지 않는다.

```sh
LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
: "${AGILITY_RUNTIME:?새 paired owned Core618 runtime 절대 경로}"
AGILITY_WORK="$(mktemp -d "$PWD/build/local/agility-public.XXXXXX")"
for pair in x64:x86_64 x86:i686; do
  abi="${pair%%:*}"; target="${pair#*:}"
  for suite in diagnostics agility; do
    "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -Wall -Wextra -static \
      "graphics-translator/tests/owned-d3d12-$suite.cpp" \
      -ld3d12 -ldxgi -ldxguid -luuid -ladvapi32 -o "$AGILITY_WORK/$suite-$abi.exe"
    if [ "$suite" = agility ]; then
      for version in 617 618 619; do
        "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -Wall -Wextra -static \
          -DOWNED_AGILITY_EXECUTABLE_SELECTION="$version" graphics-translator/tests/owned-d3d12-agility.cpp \
          -ld3d12 -ldxgi -ldxguid -luuid -ladvapi32 -o "$AGILITY_WORK/agility-$abi-executable-$version.exe"
      done
    fi
    python3 scripts/test-owned-graphics.py --runtime "$AGILITY_RUNTIME" --guest-arch "$abi" \
      --fixture "$AGILITY_WORK/$suite-$abi.exe" --suite "$suite" --mode no-present \
      --validation --out "$AGILITY_WORK/$suite-$abi" --timeout 120
  done
done
```

Runner는 **실제 staged owned Core**를 frozen EXE 옆 `sdk-owned/d3d12core.dll`로 복사하고 source/copy hash·generation을 기록한다. Consumer는 cwd를 다른 directory로 바꾼 뒤 `CreateDeviceFactory(618, ".\\sdk-owned\\", ...)`를 호출한다. 실제 loaded module path, concrete `D3D12GetInterface`, DATA `D3D12SDKVersion=618`, factory/device의 공개 `ID3D12DeviceConfiguration::GetDesc()`와 debug/experimental snapshot, root-signature serializer/deserializer roundtrip을 확인한다. Factory 해제와 `FreeUnusedSDKs` 뒤 retained settings/configuration/device 및 device 외부 참조 없이 남은 queue/list/resource/fence로 실제 GPU copy/readback을 수행한다. 마지막 live configuration까지 해제한 뒤 exact-path `GetModuleHandle`로 unused Core unload를 확인한다. Vtable이 어느 DLL에 있는지는 public API 계약이 아니므로 pin하지 않는다.

Fresh child는 Wine-visible OS registry `HKLM/Software/Microsoft/Windows/CurrentVersion/AppModelUnlock/AllowDevelopmentWithoutDevLicense`의 기존 value/type을 저장한다. Ordinary mode에서 base `SetSDKVersion`의 실패·미로드 및 SDK1 factory 성공을 확인한 뒤 Developer Mode를 켜 device 생성 전 Set 성공을 검사한다. 유효한 alternate Core path로 late Set을 호출하면 실패와 **기존 global device 제거**, independent factory device 생존·GPU copy, 선택 미변경과 future global SDK618을 요구한다. 다른 fresh retail child는 Developer Mode 없이 factory `ApplyToGlobalState`로 Core618 선택·global lease·GPU를 확인하고 alternate Core factory의 `InitializeFromGlobalState`를 검사한다. Live global과 다른 Core의 Apply는 실패하며 global/independent device 둘 다 살아 있고 선택은 유지돼야 한다. 두 child 모두 OS state를 복구한다. 실패 HRESULT는 실제 값을 기록하고 `FAILED`를 요구하지만 공개 규약이 특정 값을 지정하지 않은 incidental code는 pin하지 않는다. 이것은 전용 prefix의 공개 계약 시험이며 live/game prefix나 DXMT-only Developer Mode switch를 사용하지 않는다.

같은 source의 `-DOWNED_AGILITY_EXECUTABLE_SELECTION=617/618/619` 세 compile variant는 immutable EXE exports를 제공한다. SDK617은 missing app path, SDK618은 invalid app-local Core에도 ordinary `D3D12CreateDevice`가 실제 설치된 owned618을 선택하여 GPU readback을 통과해야 한다. SDK619는 유효한 owned619 package가 없으므로 실패·null device를 요구한다. Setter child도 SDK619 EXE에서 시작하여 Developer Mode의 SetSDKVersion617/618이 exports보다 우선하는지 검증한다. Setter가 no-op이면 EXE619 선택 실패로 잡힌다. Runner는 `$FIXTURE` stem에 `-executable-617.exe`, `-executable-618.exe`, `-executable-619.exe`를 붙인 세 companion PE를 모두 freeze/hash하고, global inbox 선택의 실제 installed path·generation·ABI·hash와 explicit/alternate native Core loads를 검사한다.

Different-Core/no-downgrade Apply의 공개 Independent Devices 규약은 `D3D12_ERROR_INVALID_REDIST`를 지정하므로 그 경우에는 정확한 `0x887e0003`을 요구한다. SDK Set의 unspecified failure/removal HRESULT와 구분한다. Pinned MinGW에는 이 이름이 없어 consumer의 named constant가 Microsoft public numeric identity를 사용한다.

Wrong version, missing directory/file, non-Core owned DXGI image, **반대 ABI의 실제 staged Core**, generation DATA를 변조한 실제 Core는 failing HRESULT·output=null·module 미로드를 요구한다. Proprietary DLL을 fixture에 복사하지 않는다. Parser는 여섯 case·여섯 negative·열 GPU 값·32-round concurrent factory/config/FreeUnusedSDKs·selection/unload·fresh-child와 OS-mode/actual HRESULT 결과를 정확히 한 번씩 요구한다. CPU-only parser 회귀는 `python3 scripts/test-owned-graphics-diagnostics.py RunnerFailures.test_agility_parser_requires_real_selection_gpu_lifetime_unload_and_child_contracts`이며 실행 proof와 구분한다.

## P0 공개 계약 fixture 수동 실행

이 네 public fixture는 현재 공용 runner의 신규 suite가 아니라 **직접 Wine 실행** 경로다. README로 현재 양 ABI DLL·NGX를 빌드/install하고 fresh runtime을 stage한 다음 저장소 루트에서 실행한다. 기존 game/live prefix는 쓰지 않는다. Target compiler는 x64의 x86_64, WoW64의 i686이며 모두 main이라 -municode를 넣지 않는다. NGX --ngx는 **x64-only**다.

Resource2의 runtime 구현은 WIDL explicit aggregate-return **GetDesc1(D3D12_RESOURCE_DESC1 *result)** sret ABI를 사용한다. 특히 x86 hidden return-pointer/COM 호출 규약을 수동 cast나 가짜 vtable로 우회하지 않는다. 아래 public consumer는 양 ABI 모두 공식 header의 **GetDesc1()**를 사용하며 **WIDL_EXPLICIT_AGGREGATE_RETURNS를 정의하지 않는다**. Pointer-bearing feature query도 target별 실제 layout(16/12,56/36 bytes)을 따른다.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
: "${P0_RUNTIME:?새 x64+x86 runtime의 절대 경로; NGX 포함}"
P0_WORK="$(mktemp -d "$PWD/build/local/p0-public-check.XXXXXX")"
python3 scripts/stage-owned-graphics.py verify --runtime "$P0_RUNTIME"
for pair in x64:x86_64 x86:i686; do
  abi="${pair%%:*}"; target="${pair#*:}"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    -Iinclude/native/directx graphics-translator/tests/owned-dxgi-protection-notification.cpp \
    -ldxgi -lole32 -o "$P0_WORK/protection-$abi.exe"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-dxgi-adapter-selection.cpp \
    -ld3d12 -ldxgi -o "$P0_WORK/selection-$abi.exe"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-d3d12-resource-interfaces.cpp \
    -ld3d12 -ldxgi -luuid -o "$P0_WORK/interfaces-$abi.exe"
  "$LLVM_MINGW/$target-w64-mingw32-g++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-d3d12-capability-discovery.cpp \
    -ld3d12 -ldxgi -lole32 -luuid -o "$P0_WORK/discovery-$abi.exe"
done
for row in protection:x64 protection:x86 selection:x64 selection:x86 interfaces:x64 interfaces:x86 discovery:x64 discovery:x86 ngx:x64; do
  (
    set -eu
    kind="${row%%:*}"; abi="${row#*:}"
    run="$(mktemp -d "$P0_WORK/$kind-$abi.XXXXXX")"
    export WINEPREFIX="$run/prefix"
    trap '"$P0_RUNTIME/bin/wineserver" -k; "$P0_RUNTIME/bin/wineserver" -w; rm -rf "$WINEPREFIX"' EXIT
    env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/sbin:/sbin TMPDIR="${TMPDIR:-/tmp}" \
      WINEPREFIX="$WINEPREFIX" WINEDEBUG=-all \
      "$P0_RUNTIME/bin/wine" wineboot -u >"$run/bootstrap.log" 2>&1
    exe="$P0_WORK/$kind-$abi.exe"; set --
    if [ "$kind" = ngx ]; then exe="$P0_WORK/discovery-x64.exe"; set -- --ngx; fi
    set +e
    env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/sbin:/sbin TMPDIR="${TMPDIR:-/tmp}" \
      WINEPREFIX="$WINEPREFIX" WINEDEBUG=+loaddll DXMT_LOG_LEVEL=error DXMT_LOG_PATH="$run" \
      MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
      OWNED_GRAPHICS_DYLD_AUDIT=1 \
      "$P0_RUNTIME/bin/wine" "$exe" "$@" >"$run/stdout.log" 2>"$run/stderr.log"
    status=$?
    printf 'exit=%s\nruntime=%s\nfixture=%s\n' "$status" "$P0_RUNTIME" "$exe" >"$run/result.txt"
    exit "$status"
  ) || exit "$?"
done
```

각 exit0 외에 protection의 DXGI_PROTECTION_NOTIFICATION_OK, selection의5 CASE_PASS groups/final PASS, interfaces의31 rows/RESOURCE_INTERFACES_PASS, discovery의80 checks·0 failures, NGX의12 checks·0 failures를 확인한다. Runtime manifest·source/PE hashes·stdout/stderr/result·actual owned load와 no foreign backend·validation/completion 오류도 보존한다. Unknown feature negative checks의 unhandled 메시지는 의도한 E_INVALIDARG 경계이며 blanket stderr-zero gate를 쓰지 않는다. Public query fixture 성공 자체는 positive GPU consumer proof가 아니다. OPTIONS10/ExistingHeaps는 기존 **vrs·sharing** suites를 같은 runtime에서 x64/x86 --validation으로 별도 실행하며 compile 지침은 기존 suite source/runner 패턴을 따른다.

P0-3의 enhanced creation APIs3/6/9는 enhanced COPY_DEST→COPY_SOURCE barrier를 사용한다. Enhanced texture를 legacy ResourceBarrier에 넘기려면 공식 COMMON handoff가 필요하므로 -02의 잘못된 fixture 혼용을 그대로 재사용하지 않는다. 새 실행은 [CORE P0 판정·정확한 한계](CORE-IMPLEMENTATION-STATUS.ko.md#p0-공개-계약-정합)와 비교하되 과거 로그를 새 PASS로 재작성하지 않는다.

## P1 fixture 재실행과 before/after 분리

P1의 완료 범위·runtime별 실행 결과·reference 결정은 [CORE P1](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)를 따른다. 아래는 **반복 recipe**이며 실행 결과가 아니다. README의 current x64/WoW64 build/install과 새 paired runtime stage/verify를 먼저 완료한다. NGX/FSR frontend는 현재 x64/WoW64이며 stage에 `--with-nvngx`/`--with-fsr`가 필요하다. 기존 immutable P0 runtime을 candidate로 덮어쓰지 않는다.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
: "${P1_RUNTIME:?검증된 새 paired x64+x86 runtime 절대 경로}"
P1_WORK="$(mktemp -d "$PWD/build/local/p1-public-check.XXXXXX")"
mkdir -p "$P1_WORK/fixtures"
python3 scripts/stage-owned-graphics.py verify --runtime "$P1_RUNTIME"
for pair in x64:x86_64 x86:i686; do
  abi="${pair%%:*}"; target="${pair#*:}"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    -Iinclude/native/directx graphics-translator/tests/owned-d3d12-multisample-capabilities.cpp \
    -ld3d12 -ldxgi -ld3dcompiler -ldxguid -luuid \
    -o "$P1_WORK/fixtures/multisample-$abi.exe"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static -municode \
    graphics-translator/tests/owned-d3d12-bindless.cpp \
    -ld3d12 -ldxgi -ld3dcompiler -lole32 -ldxguid -luuid \
    -o "$P1_WORK/fixtures/bindless-$abi.exe"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static -municode \
    graphics-translator/tests/owned-d3d12-sharing.cpp \
    -ld3d12 -ldxgi -ldxguid -luuid \
    -o "$P1_WORK/fixtures/sharing-$abi.exe"
  for suite in multisample-capabilities bindless sharing; do
    case "$suite" in multisample-capabilities) name=multisample;; *) name="$suite";; esac
    python3 scripts/test-owned-graphics.py \
      --runtime "$P1_RUNTIME" --guest-arch "$abi" \
      --fixture "$P1_WORK/fixtures/$name-$abi.exe" --suite "$suite" \
      --mode no-present --validation --timeout 180 \
      --out "$P1_WORK/after-$suite-$abi" || exit "$?"
  done
done
```

`bindless`는 기존 전체 suite를 실행하며 runner가 기존 fixed inputs와 `bindless-nonnormalized.hlsl`, `bindless-nonnormalized-{cs,vs,ps}.dxil`, `bindless-nonnormalized-raw.{hlsl,dxil}`을 snapshot/hash한다. 새 sampler 판정은 RS1.2 안의 DXBC/DXIL CS/PS × table/static readback이며 uploaded texel oracle·one-mip view·descriptor copy/보존·null·정규화 sampler 회귀를 유지한다. Non-normalized 유효 범위는 non-array1D/2D one-mip SRV, MinLOD=MaxLOD=0, 동일 min/mag point 또는 linear + mip-point의 standard/min/max filter, U/V clamp 또는 border, sample offsets 없음이다. MipLODBias와 W를 추가로 금지하지 않는다. Native normalized samplers를 실제 one-mip SRV view 크기로 나눈 좌표·explicitLOD0와 함께 쓰는 lowering이며 native unnormalized sampler 지원 선언이 아니다. UINT_BORDER_COLOR와 NON_NORMALIZED_COORDINATES의 유효 조합도 허용한다. Frozen DXIL의 재생성은 pinned DXC1.9.2609.5에서 `bindless-nonnormalized.hlsl -HV 2021 -T cs_6_5/vs_6_5/ps_6_5 -E cs/vs/ps` 각각으로 수행한다. GatherRaw는 table/static 모두 실제 픽셀 좌표의10,11,1,0과 min-LOD 제외 view의0을 검사한다. Null view는 dimension query 이전에0을 반환해야 하며 API/Shader Validation 오류가 없어야 한다.

`multisample-capabilities`는 기존 format/quality gates와 새 Tier1 consumer를 함께 실행한다. 1 pixel의2/4/8 positions는 signed xy[-8,7]/16, pixel-center-relative·Y down이다. Custom render surface GetSamplePosition/SamplePos는 [공식 undefined](https://raw.githubusercontent.com/microsoft/DirectX-Specs/master/d3d/ProgrammableSamplePositions.md)이므로 특정 수치로 re-pin하지 않는다. 실제 raster coverage·sample-frequency interpolation·resolve·reset/replay와 고정 texture의 독립 sample pattern이 oracle다. Tier1의 full ResolveSubresourceRegion은 MIN/MAX, 허용 AVERAGE, sample-preserving DECOMPRESS의 rect/origin/plane·depth pattern을 포함한다. Runner gates는 programmable GPU6·float/depth resolve18·integer/stencil resolve12·DECOMPRESS12, lifecycle markers와 정확한 error13이다. DECOMPRESS는1×도 포함하며 separate-resource sample 보존과 in-place same-rect/origin을 구분한다.

`sharing`의 `adapter-luid-lifetime-errors`와 `OWNED_D3D12_SHARING_ADAPTER_LUID_PASS`를 기존 전체 GPU sharing 판정에 추가한다. Actual creator LUID를 enumeration/GetAdapterLuid와 비교하고 duplicate/named/by-name/cross-process query 수명 및 invalid/forged/copied/wrongtype 경계를 확인한다. 기존 named GPU import-after-last-COM-owner 제한은 이 query-only 작업의 완료 범위가 아니다.

Public NGX는 [MSVC+MinGW fixture recipe](NGX-TEST.ko.md#현재-fixture-빌드)로 다시 빌드해 `ngx`/`ngx-contracts`를 x64 `--validation`으로 실행한다. `ngx`는 `exposure_scale_auto`, `exposure_scale_texture`, `exposure_scale_no_texture`, `mv_value_offsets_lowres`, `mv_value_offsets_highres_jittered` 각각 replay0–2의 default-vs-nondefault GPU 출력을 비교한다(max_byte_delta≤1·active contrast·outside sentinel·live inputs·parameter destroy·release-before-submit). Negative/nonfinite scalar 거부도 output sentinel을 보존해야 한다. 이것은 accept+ignore 호환 증거이며 exposure/MV 수식 또는 pristine/NVIDIA GPU differential proof가 아니다. Backend `temporal` x64/WoW64 실행은 public NGX ABI proof와 별도로 보존한다.

Public FG의 옛 `OWNED_D3D12_FSR_FG_QUEUES_PASS`·`QUEUE_PROOF`·`RECORDING_ROLLBACK` per-buffer queue recipe는 제거된 custom extension의 역사적 검증이다. 현재는 [fsr-fg present/observer recipe](#전체-srpublic-fg와-shader-cache-전용-recipe)로 원본 SDK의 단일 generated-role/real/UI 순서와 native interpolation·drawable 표시를 분리 검증한다. ResizeBuffers1은 E_NOTIMPL, ordinary ResizeBuffers는 유지한다. Success HRESULT만으로 generation/presentation을 증명하지 않는다.

Before는 immutable `build/local/p0-contracts-r5KJ0O/runtime-01`에 **같은 최종 fixture**를 별도 `before-*` output으로 실행해 보존하고, candidate는 새 `after-*`로 실행한다. Before의 nonzero/timeout·실패 JSON/stdout/stderr를 PASS로 고쳐 쓰거나 candidate 결과와 합산하지 않는다. 각 cut의 source/PE/input hashes·runtime manifest·build/install/stage logs·actual owned load/no foreign backend·API+Shader Validation/completion 판정을 함께 보존한다. Failed candidate iteration도 원인과 후속 새 실행을 별도 기록한다.

## 기본 smoke 빌드와 실행

먼저 README로 current install을 만든다. 다음 예시의 `build/local/runtime-smoke-001`, fixture directory, 각 run output은 **새 경로**여야 한다. 기존 게임 설치·runtime·live prefix에는 파일을 복사하지 않는다.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
mkdir -p build/local/fixtures-smoke-001
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++20 -O2 -Wall -Wextra -municode -static \
  graphics-translator/tests/owned-d3d12-smoke.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -luuid \
  -o build/local/fixtures-smoke-001/owned-d3d12-smoke.exe
cp graphics-translator/tests/owned-d3d12-graphics.hlsl \
   graphics-translator/tests/owned-d3d12-compute.hlsl build/local/fixtures-smoke-001/
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install \
  --wine-source toolchains/wine-runtime \
  --wine-dest build/local/runtime-smoke-001
python3 scripts/test-owned-graphics.py \
  --runtime build/local/runtime-smoke-001 --guest-arch x64 \
  --suite smoke --fixture build/local/fixtures-smoke-001/owned-d3d12-smoke.exe \
  --mode no-present --out build/local/smoke-headless-001 --validation
python3 scripts/test-owned-graphics.py \
  --runtime build/local/runtime-smoke-001 --guest-arch x64 \
  --suite smoke --fixture build/local/fixtures-smoke-001/owned-d3d12-smoke.exe \
  --mode present --out build/local/smoke-present-001 --validation
```

Target-named compiler를 사용한다. generic llvm-mingw `clang++`가 macOS를 기본 target으로 잡는 환경에서는 명시적 Windows target 없이는 잘못된 입력이다. Smoke는 `wmain`이므로 `-municode`가 필요하다. HLSL 두 개는 executable 옆에서 runner가 찾아 freeze하므로 누락하지 않는다. 이 fixture는 Wine `D3DCompile`의 SM5 DXBC이며 자체 DXIL compiler 검증을 대신하지 않는다.

### backend identity 계약

Fixture는 `GetProcAddress`로 다음 export를 찾으며 factory/device 생성 전에 반환값을 확인한다.

```cpp
extern "C" __declspec(dllexport)
const char* WINAPI OwnedGraphicsBackendId(void);
```

정확한 값은 `owned-graphics-d3d12`다. Export 누락/다른 값은 실패다. 이것은 PE identity gate이며 실제 Unix plugin 로드는 별도로 확인한다.

### 테스트 내용

8프레임 동안 adapter/device → direct queue/allocator/list → root signature·graphics/compute PSO·descriptor binding을 만든다. 16×16 RGBA8 render target에 frame별 pattern을 draw하고 `RENDER_TARGET → NON_PIXEL_SHADER_RESOURCE`, compute write 후 `UNORDERED_ACCESS → COPY_SOURCE` 전이를 거쳐 256개 uint readback을 CPU expected 값과 exact 비교한다.

Present mode는 **실제 current swapchain backbuffer**에 CopyResource하고 그 backbuffer 자체를 CopyTextureRegion으로 exact readback한 뒤 PRESENT로 전이한다. 같은 backbuffer 객체를 유지한 채 Present하며 current index 이동과 마지막 Present 이후 같은 queue의 fence drain까지 확인한다. `present_backbuffer=exact`는 Present가 소비할 resource의 값 증거이지 물리 scanout/compositor/HDR 출력 증거가 아니다.

Runner는 smoke에 항상 `--barrier-stress`를 추가한다. Same-list compute producer → UAV barrier → consumer ordering을 추가로 검증한다. 기본 runner mode는 `no-present`; fixture를 직접 실행할 때의 기본 mode는 `present`이므로 혼동하지 않는다. `--mode`는 smoke의 window/swapchain 선택이지 모든 suite의 headless switch가 아니다.

성공 marker는 `OWNED_D3D12_SMOKE_PASS`, 오류는 `_FAIL`/`_MISMATCH`와 nonzero다. Marker 한 줄만으로 PASS를 기록하지 않고 다음 runner 판정을 사용한다.

## 공용 runner와 증거

```sh
python3 scripts/test-owned-graphics.py --help
python3 scripts/stage-owned-graphics.py stage --help
```

`--suite`의 현재 전체 집합은 runner help/parser가 authority다. `--runtime`, `--fixture`, `--out`이 필수다. `--timeout`은 1~600초(기본 120)이며 timeout을 성공으로 취급하지 않는다. `--dxil-dir` 기본은 `graphics-translator/tests/dxil`; `--lock`은 frozen lock 경로/hash를 증거에 기록할 뿐 **current root source를 frozen patches로 재생성하거나 동일하다고 증명하지 않는다**.

Runner는 먼저 stage verifier를 실행하고 fixture·suite별 fixed shader/source 입력을 `--out/fixture`에 복사한다. `--out/prefix`의 새 prefix, per-run shader-cache/log paths를 사용하고 inherited foreign backend/`DXMT_*`/`DYLD_*` state를 정리한다. 종료/오류에도 **해당 prefix의** wineserver만 종료한다. 다른 prefix나 전역 wineserver를 종료하지 않는다.

출력에서 다음을 함께 보존한다.

| 증거 | 의미 |
|---|---|
| `runtime-verification.log` | Manifest/hash/loader/dependency preflight |
| `fixture/` 및 input hashes | 실제 사용한 PE/shader snapshot |
| `stdout.log`, `stderr.log`, `*_d3d12.log` | Exact markers/readbacks, dyld, backend/validation/trace |
| `result.json` | Command, architecture/guest, runtime manifest/lock hashes, exit/timeout, suite 판정, validation mode/errors |
| temporal trace analysis | Temporal suite의 ordering/lifetime 후처리 결과 |

PASS는 exit 0, suite-specific exact result/marker, staged owned `winemetal.so` 실제 dyld load, 외부 D3DMetal/`libd3dshared` load 없음 및 validation/command-completion 오류 없음이 함께 필요하다. 독립형 `dxil-static-sampler`는 backend line 대신 exact GPU marker gate를 사용한다. Module identity/외부 로드 부재는 shader 출력 correctness 자체를 증명하지 않는다. 각 fixture의 oracle·경계·거부 판정이 별도로 필요하다.

`--validation`은 API **및 Shader Validation**, stderr error reporting을 켠다. `--api-validation`은 API만 켜고 shader instrumentation을 명시적으로 끈다. `atomic-copy-live`는 instrumentation이 compute queues를 직렬화하는 장치에서 reader 동시 진행을 관찰하기 위해 API-only를 사용한다. 같은 reader thread의 실제 값 변화가 없으면 실패하며, 이것을 전부 API+shader 검증한 결과로 합치지 않는다. 기본 `atomic-copy`는 `--validation`으로 수행한다. 동시 값 관찰은 다중 DWORD atomic64 snapshot 전체의 불가분성 증거가 아니다.

Runtime manifest **schema 2**와 runner result **schema 1**은 서로 다른 형식이다. `--guest-arch`는 실행 ISA assertion이지 FEX 활성화가 아니며, architecture 조건과 거부 경계는 [RUNTIME](RUNTIME.ko.md#아키텍처와-schema-2)에 따른다.

## fixture runner suite와 컴파일 차이

Fixture는 보통 `graphics-translator/tests/owned-d3d12-<suite>.cpp`이지만 이름/entry/header/link를 일괄 가정하지 않는다. 아래는 기존 README의 상세 compile 지침을 이관한 운영 표다. 모두 `-std=c++20 -O2 -static`을 기본으로 하고 suite의 실제 source entry를 확인한다.

| Suite / fixture | Entry/header/link 지침 | 입력·범위 |
|---|---|---|
| `pipeline-cache` / `owned-d3d12-pipeline-cache.cpp` | `wmain`, `-municode`; 최신 compiler headers, `-ld3d12 -ldxgi -ldxguid -luuid -lole32 -ladvapi32` | Explicit blobs·자동 RAM/disk·restart/corruption/identity/native retry·동시 생성/실패·driver control. `pipeline-cache-a.dxil`/`-b.dxil`; fresh isolated prefix 필수 |
| `copy-resource-casts`, `execution-safety` | `main`, `-municode` 제외 | P1 casts/execution 계약 |
| `expanded-formats` | `wmain`, `-municode`; `-ld3dcompiler` 추가 | Expanded formats |
| `standard-swizzle` | `main`, `-municode` 제외; `-Iinclude/native/directx`, `-ld3dcompiler` 추가 | Standard swizzle |
| `partial-3d-uav-dxil`, `packed-cubearray`, `reinterpret-copy` | 모두 `main`, `-Iinclude/native/directx`; `-ld3d12 -ldxgi -ldxguid`; cubearray만 `-ld3dcompiler` 추가 | Partial 3D UAV write/load/atomic/slice, cube packing, BC1↔uncompressed bitcast |
| `dxil`, `dxil-ia` | 각각 fixture entry/link 확인 | CS/VS/PS 및 space/null-descriptor, IA stride/indexed 결과 |
| `dxil-static-sampler`, `dxil-resource-review` | 각각 fixture entry/link 확인 | Frozen inputs와 exact sampler/texture/resource readback |
| `dxil-texture` / `owned-d3d12-dxil-texture.cpp` | `wmain`, `-municode`, `-ld3dcompiler`; 아래 전용 recipe | Basic VS/PS와 programmable-offsets VS/PS/CS frozen inputs; 기존 texture gates와 ABI별2816-component offset oracle |
| `multiqueue`, `capabilities`, `fence-events`, `core-contracts`, `bundles` | 대응 source별 entry/link 확인 | Queue/capability/수명·거부 계약 |
| `resources` / `owned-d3d12-resources.cpp` | `main`, `-municode` 제외, `-ld3dcompiler`; 아래 전용 recipe | 기존 resource 계약과 unaligned BC21 variants의 physical copy/logical view oracle |
| `queries` / `owned-d3d12-queries.cpp` | `main`, `-municode` 제외; 아래 전용 recipe | DIRECT/COMPUTE 기존 query와 COPY timestamp·clock·heap/list exclusivity·정확한 거부 및 GPU readback |
| `residency` / `owned-d3d12-residency.cpp` | `main`, `-municode` 제외; `-ld3d12 -ldxgi -ldxguid -luuid` | Native Budget 조회·notification 등록/수명 및 기존 residency fence·exact GPU readback; 아래 전용 recipe |
| `temporal`, `ngx`, `ngx-contracts` | [전용 지침](TEMPORAL-TEST.ko.md), [NGX](NGX-TEST.ko.md) | Temporal backend identity/trace; NV modules 필요 시 stage 옵션 명시 |
| `fsr` / `owned-d3d12-fsr.cpp` | `main`, `-Isrc/fsr/include`, `-ld3d12 -ldxgi -lole32`, `-municode` 제외 | x64 public SR 전체 효과·GPU/replay·독립 native max-scale oracle; 아래 전용 recipe |
| `fsr-fg` / `owned-d3d12-fsr-fg.cpp` | `main`; 아래 PE/observer compile recipe | Public FG prepare/generate·history/lifetime·실제 SDK proxy/UI/pacing·native drawable pixels/order/timestamps |
| `shader-cache` / `owned-d3d12-shader-cache.cpp` | `wmain`, `-municode`, `-ld3d12 -ldxgi -ldxguid -luuid -ladvapi32`; 최신 compiler headers | CPU Device9 MEMORY/DISK·실제 child processes·isolated developer control; 아래 전용 recipe |
| `arena-heap-arena`, `arena-heap-mapping` | 공용 `owned-d3d12-arena-heap.cpp` fixture | Runner가 `arena`/`mapping` mode를 전달. Arena는 single 20,000/shared 24,000/indirect 20,000 exact word와 replay/reset/first error; mapping은 기존 shared-view cleanup 계약 |
| `raytracing` / `owned-d3d12-raytracing.cpp` | `wmain`, `-municode`, 아래 전용 recipe; frozen HitObject 입력도 필수 | DXR callbacks/state objects/ray queries 및 SM6.9 HitObject280-word oracle; 단일 AS probe와 구분 |
| `mesh-shader` / `owned-d3d12-mesh-shader.cpp` | `wmain`, `-municode`, `-ld3d12 -ldxgi -ld3dcompiler`; 아래 전용 recipe | 기존 mesh 계약과 SM6.6 MS/AS derivative·quad·implicit sampling·LOD GPU oracle |
| `waves` / `owned-d3d12-sm6.cpp` | Shared `wmain` fixture, `-municode`, 아래 공통 링크; runner가 `--waves-only` 전달 | 24 case: min4/max32·기본32, fixed/ranged·divergence·i64·required64/128 거부와32 quad/helper/VS 회귀 |
| `fp64` / `owned-d3d12-sm6.cpp` | Shared `wmain`, `-municode`, `-fno-fast-math -ffp-contract=off`; 아래 FP64 recipe | Double cap TRUE·DXIL/DXBC5.0/5.1 compute·VS/PS·4-lane wave8 case; exact binary64 oracle |
| `work-graphs` / `owned-d3d12-work-graphs.cpp` | `main`, `-municode` 제외, 아래 공통 링크; frozen library inputs | 기존9 case 및 coalescing NodeWave4 count/sum/ballot/shuffle·record oracle |
| `view-instancing`, `vrs`, `sampler-feedback` | 각 fixture source 및 runner fixed input 집합 확인 | Named modern-feature contracts; feature tier를 conformance와 혼동하지 않음 |

### Opacity micromap public acceptance

`raytracing-omm`은 별도 public fixture다. Final upstream DXC의 modern `lib_6_9`와 `OMM_LEGACY_QUERY=1 lib_6_8` 입력으로 TraceRay·HitObject·opted-in/opted-out RayQuery와 legacy opt-in을 비교한다. OC1 2/4-state subdivision0–12, independent physical-microtriangle oracle, GPU-produced input FIFO/lifetime, implicit/8/16/32-bit index·BaseLocation·special state, ray/instance flag precedence, tools decode, array와 mixed ordinary+OMM BLAS clone/compact/serialize·public relocation, null-array uniform serialization, in-place/out-of-place linkage update를 검사한다. Levels0–5는 모든 microtriangle, levels6–12는 level별32개만 검사한다.

Runner는 modern26·legacy1 ordered subdivision, modern56,206/legacy3,604 exact output comparisons와 zero-failure result, mixed BLAS nonnull array pointer1개, 최종 PASS 정확히1개 및 staged native compiler load를 요구한다. 새 admission gate는 OPTIONS5 tier12, SDK1.619.5 OPTIONS22의 정확한16-byte ABI·ActuallyReorders=FALSE·ByteOffsetViews=FALSE·기존1D dispatch/mesh limit65535, null/잘못된size8개와 output canary, real serialized array/mixed BLAS의 RTAS0 driver matching 및 null/type/version rejection을 추가로 요구한다. 기존 `raytracing` full regression은 별도다.

```sh
RUNTIME=build/local/p3-20261003/runtime-06
LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal
OUT=build/local/p3-20261003/omm-recheck
mkdir -p "$OUT"
for abi in x64 x86; do
  target=x86_64-w64-mingw32
  test "$abi" != x86 || target=i686-w64-mingw32
  "$LLVM_MINGW/bin/$target-clang++" -std=c++20 -O2 -static -municode \
    graphics-translator/tests/owned-d3d12-raytracing-omm.cpp \
    -o "$OUT/owned-d3d12-raytracing-omm-$abi.exe" \
    -ld3d12 -ld3d11 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32
  python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" --suite raytracing-omm \
    --fixture "$OUT/owned-d3d12-raytracing-omm-$abi.exe" --guest-arch "$abi" \
    --dxil-dir graphics-translator/tests/dxil --out "$OUT/$abi" --validation
done
```

두 ABI는 exclusive GPU window에서 직렬 실행한다. Genuine producer는 [build-raytracing-omm.sh](tests/dxil/build-raytracing-omm.sh)의 `<wine> <final upstream dxc.exe> <전용 prefix> <존재하는 out-dir>` 계약이다. Frozen [modern DXIL](tests/dxil/raytracing-omm.dxil)의 SHA256는 **50983278107950c5cb93dc87bfbfd0f9eff257aadb47bfa003b96d06f03a86bd**, [legacy DXIL](tests/dxil/raytracing-omm-legacy.dxil)은 **732f3df3b268b515f8d480f17f351be3d21b3c51625594cf235211d20deb6276**다. Producer·AIR 생성·guest compile·runner wiring만으로 GPU acceptance나 Tier1.2를 선언하지 않는다.

실제 runtime-04 official runner의 [x64 result](../build/local/p3-20261003/omm/public-runtime04-x64/result.json)·[WoW64 result](../build/local/p3-20261003/omm/public-runtime04-x86/result.json)는 exit0/pass=true, API+Shader Validation, owned native compiler verified·foreign backend 없음·validation_errors=[]다. 각 ABI의 modern56,206+legacy3,604 비교와27 ordered cases가 통과했다. [Captured predicate16 mutation checks](../build/local/p3-20261003/omm/strict-captured-predicate.log)는 GPU를 제출하지 않는 CPU-only gate 검사다. 이 historical cut은 tier12/OPTIONS22 admission gate 추가 전이므로 현행 fixture를 runtime-04에 재실행하면 cap assertion에서 의도대로 거부된다. 이 기록을 해당 query proof로 세지 않으며 다음 새 runtime-06 proof를 따른다.

새 immutable runtime-06의 [x64](../build/local/p3-20261003/omm/public-runtime06-x64/result.json)·[WoW64](../build/local/p3-20261003/omm/public-runtime06-x86/result.json) official runner는 두 ABI 모두 tier12·OPTIONS22 정확한16-byte query/8개 rejection·canary, real OMM array/mixed BLAS RTAS0 driver matching 및 null/type/version rejection, modern56,206+legacy3,604/27 ordered cases를 통과했다. Exit0/pass=true, API+Shader Validation, staged owned native compiler verified, foreign backend 없음·validation_errors=[]이며 runtime manifest SHA256는 **7b7e3073cba13d88bfea91d9773a07287030f59dd619175672fcd305bc041ac7**다. 두 전용 prefix의 stop/wait를 완료했다. [최종 production predicate capture mutation24 CPU checks](../build/local/p3-20261003/omm/strict-runtime06-captured-predicate.log)는 두 실제 ABI capture에 missing/duplicate PASS·legacy/level/relocation·cap ABI/hint·driver admission·native compiler 및 explicit failure 변형을 적용했으며 GPU 제출은0개다.

### HitObject와 기존 DXR focused proof

Full raytracing suite는 기존24 입력 뒤 [raytracing-hitobject.dxil](tests/dxil/raytracing-hitobject.dxil)을 필수 입력으로 전달한다. 기존4 raygen은 copy/aggregate/동일 trace callsite 반복, deferred hit/miss/NOP·repeated Invoke, query Reset 뒤 snapshot·SBT/local-root·ray/transform의280 exact words를 검사한다. 다섯 번째 raygen은 실제 reordercoherent UAV와 Barrier(UAV_MEMORY,REORDER_SCOPE)를 소비해64 invocations의512 words를 검사한다. 두 completed cases, 하나의 memory summary, 실제64 ordered8-word rows와 기존 full ordered cases가 모두 있어야 PASS다.

```sh
LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal
: "${RUNTIME:?Set RUNTIME to a retained fresh owned runtime cut; historical runtime-02 payload is retired}"
OUT=build/local/p3-20261003/hitobject-recheck
mkdir -p "$OUT"
for abi in x64 x86; do
  target=x86_64-w64-mingw32
  test "$abi" != x86 || target=i686-w64-mingw32
  "$LLVM_MINGW/bin/$target-clang++" -std=c++20 -O2 -static -municode \
    graphics-translator/tests/owned-d3d12-raytracing.cpp \
    -o "$OUT/owned-d3d12-raytracing-$abi.exe" \
    -ld3d12 -ld3d11 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32
  python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" --suite raytracing \
    --fixture "$OUT/owned-d3d12-raytracing-$abi.exe" --guest-arch "$abi" \
    --dxil-dir graphics-translator/tests/dxil --out "$OUT/full-$abi" --validation --timeout 600
done
```

GPU 재검사가 필요할 때만 **exclusive window에서 위 두 ABI를 직렬 실행**한다. 변경 없는 DXIL의 복사/runner wiring을 이유로 GPU 작업을 다시 제출하지 않는다. Frozen input 재생성은 [`build-raytracing-hitobject.sh`](tests/dxil/build-raytracing-hitobject.sh)의 `<wine> <final upstream dxc.exe> <전용 prefix> <존재하는 out-dir>` 계약을 사용한다. Genuine `lib_6_9 -O0` producer이며 proprietary converter를 사용하지 않는다. 아래 runtime-02의 [historical GPU input](../build/local/p3-20261003/hitobject/raytracing-hitobject.dxil) [SHA256](../build/local/p3-20261003/hitobject/frozen-input-sha256.txt)은 **1ac2ebeec95c0fdc11c7993edaa7bba5fdb256002794f5334e72e5419119e87f**다. REORDER_SCOPE memory edge가 추가된 현행 frozen 입력의 public GPU acceptance와 구분한다.

실제로 기록된 runtime-02 focused [x64](../build/local/p3-20261003/hitobject/public-runtime02-x64/consumer.log)·[WoW64](../build/local/p3-20261003/hitobject/public-runtime02-x86/consumer.log) 및 full [x64](../build/local/p3-20261003/hitobject/full-runtime02-x64/consumer.log)·[WoW64](../build/local/p3-20261003/hitobject/full-runtime02-x86/consumer.log)는 모두 exit0·PASS, Metal API+Shader Validation enabled이며 Metal validation fault를 관찰하지 않았다. 이는 **direct public GPU fixture logs**이고 공식 runner의 loader-verification JSON을 새로 실행했다는 주장이 아니다. 별도 [실제 capture predicate 검사](../build/local/p3-20261003/hitobject/strict-captured-predicate.log)와 [21 ray diagnostics](../build/local/p3-20261003/hitobject/strict-predicate-tests.log)는 CPU-only proof다.

Runtime-06 OMM·Tier1.2·OPTIONS22/RTAS0 admission과 아래 실제 SER memory readback을 함께 사용하여 **전체 P3 #19 완료**로 판단한다. ActuallyReorders=FALSE는 conforming no-op scheduling의 정직한 보고다. Deferred hit/miss와 reorder-coherent visibility는 실제 구현하지만 성능상 thread reordering, 전체 SM/FL 상향, 별도 #15 atomic RT, 다른 GPU/OS·ARM64/FEX guest 완료를 선언하지 않는다. [현행 계약](CORE-IMPLEMENTATION-STATUS.ko.md#opacity-micromap-완료와-public-gpu-proof)을 따른다.

**Runtime-06 최종 SER memory consumer:** [x64](../build/local/p3-20261003/hitobject/coherence/public-runtime06-long-x64/result.json)·[WoW64](../build/local/p3-20261003/hitobject/coherence/public-runtime06-long-x86/result.json) official runner는 기존 full DXR·280-word HitObject와 새64-lane×8=512-word oracle를 모두 통과했다. 각 결과는 exit0/pass=true, API+Shader Validation, staged owned native compiler verified, validation_errors=[]·foreign_backend_loads=[]다. Manifest SHA256는 **7b7e3073cba13d88bfea91d9773a07287030f59dd619175672fcd305bc041ac7**이며 두 ABI를 직렬 실행하고 prefix를 stop/wait/remove했다.

실제 reordercoherent UAV에 lane별 두 message를 쓰고 Barrier(UAV_MEMORY,REORDER_SCOPE)와 세 MaybeReorderThread overload를 거친 뒤, deferred closest-hit/miss가 같은 dispatch-index의 두 UAV 값을 실제 읽어 payload를 만든다. 후속 write/fence/read와 hit/miss 상태까지8 words를 독립 host 정수 oracle로 비교한다. 64 invocations > 보고된 WaveLaneCountMax32이므로 한 wave에 제한된 시험이 아니다. 다른 dispatch-index의 global rendezvous를 가정하지 않는다. Genuine final DXC lib_6_9 -O0의 현행 frozen DXIL SHA256는 **568dad8f94656c94f20df9d9ebf1b3d1b9fbb11d9544f5523c5ca6f24226ed28**다. Strict gate는 실제64 ordered rows의512 값을 재계산하며 summary만으로 PASS를 허용하지 않는다.

최초 [x64120초 deadline 결과](../build/local/p3-20261003/hitobject/coherence/public-runtime06-x64/result.json)는 legacy linkage 이후 HitObject 전에 harness timeout과 잘린 trace를 기록했다. Production validation을 완화하거나 memory oracle를 줄이지 않고 명시적600초 deadline으로 같은 전체 consumer를 실행했다. 실패 기록을 보존하며 timeout을 SER memory failure 또는 성공으로 소급 해석하지 않는다.

### Matrix runtime admission과 pinned V1 focused proof

**2026-10-03 runtime-05의 첫 runtime behavioral proof이며 #20 전체 완료가 아니다.** Untouched public preview SDK v1.721.2-preview와 대응 V1 runtime spec을 사용한다. Query union은 x64 **56 bytes**, x86 **48 bytes**이고 conversion info는56 bytes다. 후속 V2 shape를 섞지 않는다. SDK header는 수정하지 않으며 MinGW의 기본 explicit `RetVal` aggregate-return 선언을 그대로 호출한다. 가짜 `_MSC_VER`로 by-value getter 선언을 선택하지 않는다. Clang `-fms-extensions`/UUID bridge는 wire layout이나 vtable을 바꾸지 않는다.

실제 API+Shader Validation 결과:

| Suite | x64 | WoW64 |
|---|---|---|
| `matrix-runtime` | [runtime05-public-x64-02/result.json](../build/local/p3-20261003/matrix/runtime05-public-x64-02/result.json) | [runtime05-public-x86-01/result.json](../build/local/p3-20261003/matrix/runtime05-public-x86-01/result.json) |
| `work-graphs` | [work-graphs05-public-x64-01/result.json](../build/local/p3-20261003/matrix/work-graphs05-public-x64-01/result.json) | [work-graphs05-public-x86-01/result.json](../build/local/p3-20261003/matrix/work-graphs05-public-x86-01/result.json) |

각 matrix run은 모든10 datatype ×16 source/destination layout pairs ×2 replays의 raw bits·padding, S8 destination offset32/F16 offset64, F32 misalignment와10 invalid recordings/footprint overflow, GPU-VA allocation/recording release lifetime, predication suppress/execute, 실제 enhanced conversion sync를 확인했다. CL11 cross-queue consumer-first wait·signal·CPU rewind·비동기 제출·state handoff·caller lifetime도 통과했다. Work-graphs는 기존9 case를 그대로 확인하고, GPU input4 records의 reset 후 default 복원과 max3 rejection/device removal을 추가 확인했다.

Public `D3D12EnableExperimentalFeatures`에 `D3D12ExperimentalShaderModels` GUID를 **device 생성 전** 넘긴다. Unknown GUID/invalid configuration, unchanged set의 device 보존, changed set의 두 device removal·pending fence wakeup 및 device 파괴 뒤 set 변경을 검증했다. Owned backend는 GUID의 직접 opt-in을 구현하며 Windows Developer Mode registry를 읽거나 쓰지 않는다. Windows retail Developer Mode 정책의 conformance 선언이 아니다.

양 ABI fixture 빌드 예(저장소 루트, pinned header가 이미 존재해야 함):

~~~sh
LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal
for abi in x64 x86; do
  target=x86_64-w64-mingw32
  test "$abi" != x86 || target=i686-w64-mingw32
  for suite in matrix-runtime work-graphs; do
    extra=
    test "$suite" != work-graphs || extra=-ld3dcompiler
    "$LLVM_MINGW/bin/$target-clang++" -std=c++20 -fms-extensions -O2 -static \
      -static-libgcc -static-libstdc++ \
      '-DDXMT_MATRIX_PREVIEW_SDK_HEADER="../../build/local/p3-20261003/matrix/preview-d3d12.h"' \
      "graphics-translator/tests/owned-d3d12-$suite.cpp" -ld3d12 -ldxgi -lole32 $extra \
      -o "build/local/p3-20261003/matrix/$suite-public-$abi.exe"
  done
done
~~~

새 output과 승인된 exclusive GPU window에서 `python3 scripts/test-owned-graphics.py --suite matrix-runtime --runtime <fresh-cut> --fixture <public-exe> --dxil-dir build/local/p3-20261003/matrix --out <fresh-output> --validation`을 양 ABI 직렬 실행한다. Work graphs는 `--suite work-graphs --dxil-dir graphics-translator/tests/dxil`을 사용한다. Runner가 executable/source/untouched SDK와 producer inputs를 freeze한다. 통과한 unchanged body를 반복 실행하는 지시가 아니다.

Runtime-05는 **LinearAlgebraTier0 / legacy support FALSE**를 정직하게 보고한 채 runtime oracles를 실행했다. Shader oracle·최종 Tier1/native shape public query proof가 끝나기 전에는 이 결과를 구형 WaveMMA와 후속 SM6.10 matrix의 전체 #20 closure나 SM/FL 상향으로 세지 않는다. 최종 enabled cut에서는 query와 readback을 다시 확인해야 한다. 첫 x64 enhanced case의 [실패 기록](../build/local/p3-20261003/matrix/runtime05-public-x64-01/result.json)은 보존했다. Frozen enhanced spec의 interop 규칙대로 초기 `COPY_DEST→COMMON` handoff를 fixture에 추가했고 runtime validation을 약화하지 않았다.

최종 positive-cap cut의 작은 재검증은 [`owned-d3d12-matrix-capabilities.cpp`](tests/owned-d3d12-matrix-capabilities.cpp)를 양 ABI, 서로 다른 untouched SDK로 각각 빌드한다. Retired branch는 `DXMT_MATRIX_RETIRED_SDK_HEADER="../../build/local/p3-20261003/matrix/original-d3d12.h"`, successor branch는 기존 `DXMT_MATRIX_PREVIEW_SDK_HEADER`를 사용한다. 두 SDK를 한 translation unit에 섞지 않으며 `_MSC_VER`를 정의하지 않는다. 위의 동일한 compile/link flags를 쓰고 output은 `capabilities-retired-{x64,x86}.exe` / `capabilities-preview-{x64,x86}.exe`로 구분한다.

승인된 최종 enabled runtime·exclusive GPU window에서 `--suite wave-mma-capabilities`와 `--suite matrix-capabilities`를 각 해당 fixture와 `--dxil-dir build/local/p3-20261003/matrix --out <fresh-output> --validation`으로 직렬 실행한다. Retired branch는 actual query38의 3 input types ×2 M ×2 N, K16·accumulator flags·wave32와 OPTIONS9 WaveMMATier10을 요구한다. Successor branch는 actual query77 tier16, query78의6 operation arms·10 construction types·native16³ count/fill tail 보존·F16 emulated-output flags·group limits32–1024·vector transpose·atomic buffer/group를 요구하고, 기존 isolated conversion exact readback을 하나만 재사용한다. 기존160 layout pairs·predication·fence barrier body 전체를 반복하지 않는다. Runner는 두 branch의 SDK/source/ABI/strict singleton markers를 freeze한다. **이 fixture의 CPU compile은 positive GPU proof가 아니며 final enabled run 전에는 PASS를 주장하지 않는다.**

### WaveMMA·linear-algebra 최종 positive-cap proof

**새 P3 #20 구형·후속 두 계약 완료.** Runtime-05의 양 ABI full behavioral proof, runtime-06의 양 ABI11-case shader consumer proof, 아래 runtime-07의 최종 positive-cap proof를 결합한다. 단일 cut 전체 suite 재실행이 아니며 HighestShaderModel6.5·pinned preview 경계는 유지한다.

실제 immutable paired runtime-07에서 작은 strict public consumer 네 runs가 모두 PASS였다.

| Actual SDK / suite | x64 | WoW64 |
|---|---|---|
| Retired / `wave-mma-capabilities` | [caps07-retired-x64-02/result.json](../build/local/p3-20261003/matrix/caps07-retired-x64-02/result.json) | [caps07-retired-x86-01/result.json](../build/local/p3-20261003/matrix/caps07-retired-x86-01/result.json) |
| Successor V1 / `matrix-capabilities` | [caps07-preview-x64-01/result.json](../build/local/p3-20261003/matrix/caps07-preview-x64-01/result.json) | [caps07-preview-x86-01/result.json](../build/local/p3-20261003/matrix/caps07-preview-x86-01/result.json) |

모두 API+Shader Validation·exit0·pass=true·owned native backend·validation_errors=[]·foreign_backend_loads=[]다. 공통 manifest SHA256는 `1a79c00dd85e4d2528bcf5edbdafb0469081e8bd1ad4737b40da6865cee28042`다. Actual public query38/77/78와 OPTIONS9의 WaveMMATier10/LinearAlgebraTier16을 확인했고, successor는 한 isolated conversion의 lifetime/bit-exact readback도 확인했다. SDK와 V1 ABI는 위 recipe대로 분리했다. Runtime-05의 full behavioral suite와 runtime-06의 shader consumer suite는 unchanged 재실행하지 않았다.

첫 retired-x64-01은 fresh prefix startup 중 evaluate transport deadline으로 중단되어 consumer output/result가 없으며 proof가 아니다. Partial artifacts는 유지하고 explicit 충분한 transport deadline의 fresh x64-02부터 네 runs를 직렬 완료했다. Long runner는 background job의 실제 completion을 기다리거나 충분한 evaluate deadline을 명시해야 한다. Actual exit/runner cleanup 이후 own processes가 없었고 dedicated prefixes만 제거했으며 invocation/source/SDK/log/result는 보존했다. 전체 SM/FL·다른 GPU/OS·ARM64/FEX/no-Rosetta·게임/성능 proof는 아니다.

### WaveMMA와 linear-algebra shader focused proof

`matrix-shaders`는 [`owned-d3d12-matrices.cpp`](tests/owned-d3d12-matrices.cpp)의 strict11-case suite다. Genuine archived producer와 [provenance](tests/dxil/matrix-producers.json)의11 frozen DXIL을 사용하며 retired12 ops/packed S8/U8/F16 subnormal·4 shapes, successor wave/two-SIMD group·10 formats·vector/outer/atomic을 독립 whole-buffer numerical/byte oracle와 guard로 확인한다. Device 전에 actual public experimental GUID opt-in이 필수다. Thread vector weights는 actual public conversion 뒤 GPU copy한 MulOptimal bytes이고 원래 input은 poison한다. Admission과 converted-weight consumer의 두 PASS marker를 runner가 요구하며 missing/extra/reordered case·wrong result size·mismatch·validation fault·foreign backend·timeout/nonzero exit는 실패다.

실제 immutable paired `runtime-06`의 [x64](../build/local/p3-20261003/matrix/gpu-06-x64-01/result.json)·[WoW64](../build/local/p3-20261003/matrix/gpu-06-x86-01/result.json)는 각각11 cases·exit0·`pass=true`·API+Shader Validation·`dxil_native_compiler_verified=true`·`validation_errors=[]`·`foreign_backend_loads=[]`다. Manifest SHA256는 `7b7e3073cba13d88bfea91d9773a07287030f59dd619175672fcd305bc041ac7`다. 승인된 exclusive GPU window에서 직렬 실행한 뒤 dedicated prefixes를 stop/wait하고 제거했으며 logs/manifests/frozen sources/inputs는 보존했다. 통과한 unchanged11 body를 다시 실행하라는 지시가 아니다.

변경 후 재현 예(untouched pinned SDK header와 fresh paired runtime이 이미 필요):

```sh
set -eu
: "${LLVM_MINGW:?LLVM-MinGW bin 절대 경로}"
: "${MATRIX_RUNTIME:?fresh paired runtime 절대 경로}"
: "${MATRIX_PREVIEW_SDK:?untouched preview-d3d12.h 절대 경로}"
MATRIX_WORK="$(mktemp -d "$PWD/build/local/matrix-check.XXXXXX")"
for pair in x64:x86_64 x86:i686; do
  abi="${pair%%:*}"; target="${pair#*:}"
  "$LLVM_MINGW/$target-w64-mingw32-g++" -std=c++20 -O2 -static -municode -fms-extensions \
    "-DDXMT_MATRIX_PREVIEW_SDK_HEADER=\"$MATRIX_PREVIEW_SDK\"" \
    graphics-translator/tests/owned-d3d12-matrices.cpp \
    -ld3d12 -ldxgi -ldxguid -ld3dcompiler -o "$MATRIX_WORK/matrices-$abi.exe"
  python3 scripts/test-owned-graphics.py --runtime "$MATRIX_RUNTIME" \
    --fixture "$MATRIX_WORK/matrices-$abi.exe" --guest-arch "$abi" \
    --suite matrix-shaders --validation --timeout 180 --out "$MATRIX_WORK/public-$abi"
  WINEPREFIX="$MATRIX_WORK/public-$abi/prefix" "$MATRIX_RUNTIME/bin/wineserver" -k
  WINEPREFIX="$MATRIX_WORK/public-$abi/prefix" "$MATRIX_RUNTIME/bin/wineserver" -w
done
```

No fake `_MSC_VER` 또는 older project SDK include-path override를 사용한다. [`build-matrix.sh`](tests/dxil/build-matrix.sh)의 `<wine> <retired-dxc.exe> <linalg-dxc.exe> <linalg-inc> <dedicated-prefix> <existing-out-dir>` 계약으로 genuine producers를 재생성한다. Retired `cs_6_8`와 preview `cs_6_10` opcode/version/header ABI를 서로 바꾸지 않는다. [`matrix-contract.cpp`](tests/dxil/matrix-contract.cpp)의 CPU wrong-version/opcode/metadata rejection·LLVM verification과 native Metal PSO compile-only는 위 실제 GPU proof를 대신하지 않는다. Runtime/cap acceptance는 앞의 별도 pinned V1/public query proof를 함께 따른다. 지정 Apple M5 Pro/macOS27의 synthetic D3D12 acceptance이며 signed final SM6.10·전체 SM/FL·게임·성능·다른 GPU/OS·ARM64/FEX/no-Rosetta 성공을 선언하지 않는다.

### Native vector focused proof

`vectors`는 [`owned-d3d12-vectors.cpp`](tests/owned-d3d12-vectors.cpp)의32-case suite다. Genuine final SM6.9/DXIL1.9 frozen12 DXIL과2 root signatures를 사용하며 raw/structured/heap·groupshared·helper·SSA/phi·1024-component indexing·half/double/uint64·VS/PS·sparse tile/status를 독립 readback oracle와 비교한다. Metadata를 바꾼 SM6.8 입력이나 compiler-only 성공을 GPU proof로 세지 않는다.

```sh
set -eu
: "${LLVM_MINGW:?LLVM-MinGW bin 절대 경로}"
: "${VECTOR_RUNTIME:?fresh paired x64+x86 runtime 절대 경로}"
VECTOR_WORK="$(mktemp -d "$PWD/build/local/vector-check.XXXXXX")"
for pair in x64:x86_64 x86:i686; do
  abi="${pair%%:*}"; target="${pair#*:}"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static -municode \
    graphics-translator/tests/owned-d3d12-vectors.cpp \
    -ld3d12 -ldxgi -ld3dcompiler -lole32 -o "$VECTOR_WORK/vectors-$abi.exe"
  python3 scripts/test-owned-graphics.py --runtime "$VECTOR_RUNTIME" \
    --suite vectors --guest-arch "$abi" --validation --timeout 240 \
    --fixture "$VECTOR_WORK/vectors-$abi.exe" --out "$VECTOR_WORK/public-$abi"
  WINEPREFIX="$VECTOR_WORK/public-$abi/prefix" "$VECTOR_RUNTIME/bin/wineserver" -k
  WINEPREFIX="$VECTOR_WORK/public-$abi/prefix" "$VECTOR_RUNTIME/bin/wineserver" -w
done
```

GPU가 필요한 변경에만 exclusive window에서 양 ABI를 직렬 실행한다. Frozen input 재생성은 [`build-vectors.sh`](tests/dxil/build-vectors.sh)의 `<wine> <genuine final dxc.exe> <전용 prefix> <존재하는 out-dir>` 계약이며 cs/vs/ps_6_9·HV2021·native16bit·denorm preserve를 사용한다. [`vector-contract.cpp`](tests/dxil/vector-contract.cpp)는 existing native airconv/LLVM에 link하는 CPU-only28 ABI/lowering checks이고 실제 frozen legacy raytracing-contract DXIL 경로를 첫 argument로 받는다. [`compile-vectors.mm`](tests/dxil/compile-vectors.mm)의 native Metal PSO 생성은 public readback을 대신하지 않는다.

Strict runner는 정확한32 case 순서와 단1개의 VECTORS_PASS, owned backend/native compiler load를 요구한다. Missing/extra case·mismatch·validation fault·foreign backend·timeout/nonzero exit는 PASS가 아니다. 최종 [runtime-04 x64](../build/local/p3-20261003/vectors/gpu-x64-04/result.json)·[WoW64](../build/local/p3-20261003/vectors/gpu-x86-04/result.json)는 각각32 case·exit0/pass=true·API+Shader Validation·`dxil_native_compiler_verified=true`·`validation_errors=[]`·`foreign_backend_loads=[]`다. Manifest SHA256는 `f5f5aa08617aef02a416a48867ad8fdee3ddbe659c7fefa676660a0dce0f8f93`다. Compiler 입력6.9와 공개보고6.5는 별개이며 전체 SM/FL·다른 GPU/OS·게임·성능·ARM64/FEX guest·no-Rosetta proof로 확대하지 않는다. [현행 계약](CORE-IMPLEMENTATION-STATUS.ko.md#native-vector-완료와-focused-proof)을 따른다.

### FP64 DXIL과 DXBC focused proof

`fp64`는 shared `owned-d3d12-sm6.cpp`에 `--fp64-only`를 전달하는8-case suite다. `DoublePrecisionFloatShaderOps=TRUE`를 요구하며 binary64 bit oracle를 실제 compute·VS/PS·4-lane wave consumer에서 비교한다. DXC1.9의5개 DXIL과 native FXC의 CS/VS/PS ×5.0/5.1 **6개 실제 DXBC**를 별도로 freeze한다. DXC에5.x profile을 주거나 DXIL metadata를 바꾸는 것은 legacy proof가 아니다.

Producer `WINE`/`DXC_EXE`는 기존 SM6 recipe와 같은 CPU Wine·genuine DXC 경로를 사용하고 그 Wine package의 library 환경을 설정한다. `FXC_EXE`는 native `d3dcompiler_47.dll`과 같은 디렉터리에 둔다. 검증에 사용한 [Microsoft.Windows.SDK.CPP 10.0.28000.2705](https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/10.0.28000.2705/microsoft.windows.sdk.cpp.10.0.28000.2705.nupkg)의 `c/bin/10.0.28000.0/x64/fxc.exe` SHA256는 `7920b6459ac0800c71bbb9815fa33b1e2a7932f86f5a8f17cff267dc6e8ef7f5`, adjacent DLL은 `c1de76af7f724c671cb393889a899268e17fd76d0d50a1244678af57567255a1`이다. Legacy script는 `WINEDLLOVERRIDES=d3dcompiler_47=n`으로 Wine builtin producer를 사용하지 않는다.

```sh
set -eu
: "${LLVM_MINGW:?LLVM-MinGW bin 절대 경로}"
: "${FP64_RUNTIME:?double cap TRUE인 fresh paired x64+x86 runtime 절대 경로}"
: "${WINE:?CPU producer Wine 절대 경로}"
: "${DXC_EXE:?genuine DXC1.9 dxc.exe 절대 경로}"
: "${FXC_EXE:?genuine SDK fxc.exe 절대 경로; adjacent native D3DCompiler_47.dll 필요}"
FP64_WORK="$(mktemp -d "$PWD/build/local/fp64-check.XXXXXX")"
mkdir -p "$FP64_WORK/fixtures"
sh graphics-translator/tests/dxil/build-sm6.sh \
  "$WINE" "$DXC_EXE" "$FP64_WORK/producer-prefix" "$FP64_WORK/fixtures"
sh graphics-translator/tests/dxil/build-fp64-legacy.sh \
  "$WINE" "$FXC_EXE" "$FP64_WORK/producer-prefix" "$FP64_WORK/fixtures"
for source in fp64.hlsl fp64-legacy.hlsl build-sm6.sh build-fp64-legacy.sh; do
  cp "graphics-translator/tests/dxil/$source" "$FP64_WORK/fixtures/$source"
done
WINEPREFIX="$FP64_WORK/producer-prefix" "$(dirname "$WINE")/wineserver" -k
WINEPREFIX="$FP64_WORK/producer-prefix" "$(dirname "$WINE")/wineserver" -w
for pair in x64:x86_64 x86:i686; do
  abi="${pair%%:*}"; target="${pair#*:}"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" \
    -std=c++20 -O2 -fno-fast-math -ffp-contract=off -static -municode \
    graphics-translator/tests/owned-d3d12-sm6.cpp \
    -ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32 \
    -o "$FP64_WORK/fixtures/owned-d3d12-sm6-$abi.exe"
  python3 scripts/test-owned-graphics.py --runtime "$FP64_RUNTIME" \
    --suite fp64 --guest-arch "$abi" --validation --timeout 180 \
    --fixture "$FP64_WORK/fixtures/owned-d3d12-sm6-$abi.exe" \
    --dxil-dir "$FP64_WORK/fixtures" --out "$FP64_WORK/public-$abi"
done
```

ABI별 정확한 순서는 `fp64`, `fp64_ftz`, `fp64_legacy50`, `fp64_legacy51`, `fp64_wave`, `fp64_graphics`, `fp64_graphics_legacy50`, `fp64_graphics_legacy51`다. 앞의 compute4 case는 각각1024 edge/random 입력의 binary64 arithmetic·comparison·f32/int 변환·groupshared·select를 확인한다. Graphics3 case는 실제 VS DFMA/fma와 nointerpolation uint2 varying을 소비한 PS의64 pixels를 비교한다. Wave는 +0/-0 numeric equality·NaN unordered·sum/product/min/max와 readfirst/shuffle payload bits를 확인한다. Unexpected missing/extra case, cap FALSE, foreign backend, native compiler proof 누락, Metal/API/Shader 오류, exit/timeout 실패는 PASS가 아니다.

최종 [runtime-04 x64](../build/local/p3-20261003/fp64/public-runtime04-x64/result.json)·[runtime-04 WoW64](../build/local/p3-20261003/fp64/public-runtime04-x86/result.json)는 각각8 case·`double=1`·exit0·API+Shader Validation·`dxil_native_compiler_verified=true`·`validation_errors=[]`·`foreign_backend_loads=[]`다. 동일 runtime manifest SHA256는 `f5f5aa08617aef02a416a48867ad8fdee3ddbe659c7fefa676660a0dce0f8f93`이며 전용 prefix는 stop/wait했다. 이 proof는 Apple M5 Pro/macOS27의 named consumer acceptance이고 다른 GPU/OS·게임·성능·전체 SM/FL·D3D11 cap·ARM64/FEX guest 또는 no-Rosetta 성공을 주장하지 않는다.

### Wave size와 NodeWave4 focused proof

`waves`는 별도 fixture source가 아니라 `owned-d3d12-sm6.cpp`의 focused slice다. Min4/max32·기본32를 요구하며, 필수64/128 및64–128 전용 range는 **E_FAIL·null PSO**를 요구한다. Unavailable preferred64를32로 선택하는 것은 해당 flexible range가32를 허용할 때뿐이다. Frozen wave 입력은 `build-sm6.sh`의 genuine SM6.6 fixed·SM6.8 ranged producer이며, `build-work-graphs.sh`의 `lib_6_8` coalescing node는 `[WaveSize(4)]`와 count4·sum6·ballot0xf·next-lane shuffle을 기존 record oracle에 연결한다.

공통 최신 llvm-mingw header/link를 사용한다(`-Iinclude/native/directx` 제외). 다음 recipe는 새 output과 허용된 exclusive GPU window에서 양 ABI를 직렬 실행하는 예다. 통과한 폭 matrix를 변경 없이 반복 실행하는 지시가 아니다.

```sh
RUNTIME=build/local/p3-20261003/runtime-02
OUT=build/local/waves-smoke-001
mkdir -p "$OUT/fixtures"
for abi in x64 x86; do
  case "$abi" in x64) target=x86_64 ;; x86) target=i686 ;; esac
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static -municode \
    graphics-translator/tests/owned-d3d12-sm6.cpp \
    -ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32 \
    -o "$OUT/fixtures/sm6-$abi.exe"
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-d3d12-work-graphs.cpp \
    -ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32 \
    -o "$OUT/fixtures/work-graphs-$abi.exe"
  python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" --suite waves \
    --fixture "$OUT/fixtures/sm6-$abi.exe" --guest-arch "$abi" \
    --out "$OUT/waves-$abi" --validation
  python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" --suite work-graphs \
    --fixture "$OUT/fixtures/work-graphs-$abi.exe" --guest-arch "$abi" \
    --out "$OUT/work-graphs-$abi" --validation
done
```

2026-10-03 retained proof의 runtime-02 manifest SHA256는 **a2708cd565c32a40a6e359467e3f175b35524a50d16a0ef753f48ff87b46988e**다. [Waves x64](../build/local/p3-20261003/waves/gpu-x64-02/result.json)·[WoW64](../build/local/p3-20261003/waves/gpu-x86-02/result.json)는 각24 case, [WorkGraphs NodeWave4 x64](../build/local/p3-20261003/waves/node-gpu-x64-02/result.json)·[WoW64](../build/local/p3-20261003/waves/node-gpu-x86-02/result.json)는 각9 case PASS다. 네 실행은 exit0·**API+Shader Validation**·`validation_errors=[]`·`foreign_backend_loads=[]`·`dxil_native_compiler_verified=true`이며 전용 prefix를 종료·제거했다. [CORE의 정확한 계약과 한계](CORE-IMPLEMENTATION-STATUS.ko.md#wave-size-확장-완료와-focused-proof)를 따르며 전체 SM/FL·cross-SIMD64/128·다른 P3 proof로 합산하지 않는다.

### Mesh·amplification shader derivatives

`tests/dxil/build-mesh-shader.sh`는 기존 SM6.5 입력을 유지하고 SM6.6 derivative 입력을 추가한다. 양성 입력 7개는 genuine DXC와 validator를 통과하며, `ms_derivatives_invalid`·`as_derivatives_invalid`만 의도적으로 `-Vd`로 생성한다. Fixture는 feature bit만 확인하지 않고 다음 결과를 CPU oracle와 비교한다.

- `derivatives_ms_1d`, `derivatives_ms_2d`, `derivatives_ms_3d`, `derivatives_as_1d`, `derivatives_as_2d`, `derivatives_as_3d`: `(32,1,1)`·`(8,4,1)`·`(4,2,2)` group shape에서 `DispatchMesh(2,1,1)`을 실행한다. ABI별 320 invocations의 identity 2,560 words와 numeric 5,120 words를 검증한다.
- 검증 값은 logical `SV_GroupThreadID`·`SV_GroupIndex`·`SV_DispatchThreadID`, f32 fine/coarse X/Y derivatives, f16 fine derivatives, quad X/Y/diagonal/lane reads, clamped/unclamped `CalculateLevelOfDetail`, `Sample`·`SampleBias`·`SampleCmp`이다. 네 mip의 독립 업로드 값과 비교하며, AS payload의 실제 mesh dispatch·green target 및 MS zero-primitive clear target도 확인한다.
- `derivatives_invalid_shape`: `(3,1,1)` MS/AS derivative PSO가 각각 `E_INVALIDARG`로 거부되어야 한다. 기존 mesh rendering, MSAA/statistics, predication/indirect, cache, view-instancing, full-range RTAI, output-budget/cull-distance cases도 동일 full suite에서 유지한다.

재현 시 fresh runtime과 새 output을 사용하고 ABI별 PE를 따로 빌드한다.

```sh
: "${LLVM_MINGW:?LLVM-MinGW bin 경로}"
: "${RUNTIME:?fresh paired owned runtime 경로}"
MESH_WORK="$(mktemp -d "$PWD/build/local/mesh-derivatives.XXXXXX")"
for ABI in x64 x86; do
  if [ "$ABI" = x64 ]; then TRIPLE=x86_64; else TRIPLE=i686; fi
  "$LLVM_MINGW/$TRIPLE-w64-mingw32-clang++" \
    -std=c++20 -O2 -Wall -Wextra -Werror -static -municode \
    graphics-translator/tests/owned-d3d12-mesh-shader.cpp \
    -ld3d12 -ldxgi -ld3dcompiler -o "$MESH_WORK/mesh-$ABI.exe"
  python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" \
    --guest-arch "$ABI" --fixture "$MESH_WORK/mesh-$ABI.exe" \
    --suite mesh-shader --dxil-dir graphics-translator/tests/dxil \
    --out "$MESH_WORK/run-$ABI" --validation --timeout 240
done
```

2026-10-03 final evidence: `build/local/p3-20261003/runtime-01`의 manifest SHA-256은 `2ab82529f066ab6fc58c9cf36c3fa3c97398642026b41a1d903cf23c310cfff2`이다. `build/local/p3-20261003/mesh/final-mesh-shader-{x64,x86}/result.json`에서 두 guest ABI 모두 OPTIONS9 `derivatives=1`, full suite PASS, exit 0, `api-and-shader`, `validation_errors=[]`, owned native compiler 확인을 기록했다. 같은 stage의 `final-pipeline-cache-{x64,x86}/result.json`도 PASS이며 shared 64-byte sampler descriptor 변경 후 mesh sampling/cache 회귀가 없었다. 모든 output의 frozen inputs·로그·hashes를 보존하고, 정지한 test prefix만 제거했다. 이는 synthetic contract 증거이며 게임 호환성이나 strict no-Rosetta 증거가 아니다.

### 전체 SR·public FG와 shader-cache 전용 recipe

새 build/install/runtime/output만 사용한다. [README toolchain](../README.md#빌드)을 준비한 뒤 **현재 root source**를 `enable_fsr=true`와 NGX enabled로 빌드한다. Frozen import builder/patch 재적용은 이 경로가 아니다.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
export PATH="$LLVM_MINGW:$PWD/toolchains/tools/bin:$PATH"
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++
export OBJCXX_FOR_BUILD=/usr/bin/clang++
: "${WINE_SOURCE:?복제할 Wine package 경로}"
mkdir -p build/local
FX_WORK="$(mktemp -d "$PWD/build/local/metalfx-full-check.XXXXXX")"
meson setup "$FX_WORK/build" . --cross-file build-win64.txt \
  --prefix "$FX_WORK/install" --buildtype debugoptimized \
  -Dwine_builtin_dll=true -Denable_d3d12=true \
  -Denable_fsr=true -Denable_nvngx=true \
  -Dnative_llvm_path=toolchains/llvm-15 \
  -Dwine_install_path=toolchains/wine-sdk
ninja -C "$FX_WORK/build" -j8
meson install -C "$FX_WORK/build" --no-rebuild
mkdir -p "$FX_WORK/fixtures"
xcrun clang++ -std=c++17 -fobjc-arc \
  -framework Metal -framework MetalFX -framework Foundation \
  -x objective-c++ - -o "$FX_WORK/fixtures/native-scale-control" <<'MM'
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstdio>
int main() { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || ![MTLFXTemporalScalerDescriptor supportsDevice:device]) return 2;
    std::printf("NATIVE_METALFX_SCALE device=%s min=%.9g max=%.9g\n", device.name.UTF8String,
        [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:device],
        [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:device]);
} }
MM
"$FX_WORK/fixtures/native-scale-control" > "$FX_WORK/native-scale-control.log"
export OWNED_FSR_NATIVE_MAX_SCALE="$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).read_text().split(" max=")[1].strip())' "$FX_WORK/native-scale-control.log")"
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++17 -O2 -static -Isrc/fsr/include \
  graphics-translator/tests/owned-d3d12-fsr.cpp \
  -ld3d12 -ldxgi -lole32 -o "$FX_WORK/fixtures/owned-d3d12-fsr.exe"
"$LLVM_MINGW/x86_64-w64-mingw32-g++" \
  -std=c++20 -O2 -Wall -Wextra -Wno-unknown-pragmas \
  -Wno-deprecated-declarations -static -Isrc/fsr/include \
  graphics-translator/tests/owned-d3d12-fsr-fg.cpp \
  -o "$FX_WORK/fixtures/owned-d3d12-fsr-fg.exe" \
  -ld3d12 -ldxgi -ldxguid -lole32 -luser32
xcrun clang++ -std=c++20 -Wall -Wextra -Werror \
  -arch x86_64 -arch arm64 -dynamiclib -fblocks \
  -framework Foundation -framework Metal -framework QuartzCore \
  graphics-translator/tests/owned-fg-drawable-observer.mm \
  -o "$FX_WORK/fixtures/owned-fg-drawable-observer.dylib"
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++20 -O2 -static -municode \
  graphics-translator/tests/owned-d3d12-shader-cache.cpp \
  -ld3d12 -ldxgi -ldxguid -luuid -ladvapi32 \
  -o "$FX_WORK/fixtures/owned-d3d12-shader-cache.exe"
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install "$FX_WORK/install" --wine-source "$WINE_SOURCE" \
  --wine-dest "$FX_WORK/runtime" --native-arch x86_64 --windows-abi x64 \
  --with-fsr --with-nvngx
python3 scripts/stage-owned-graphics.py verify --runtime "$FX_WORK/runtime"
python3 scripts/test-owned-graphics.py \
  --runtime "$FX_WORK/runtime" --guest-arch x64 \
  --fixture "$FX_WORK/fixtures/owned-d3d12-fsr.exe" \
  --suite fsr --mode no-present --validation --timeout 180 \
  --out "$FX_WORK/run-fsr"
python3 scripts/test-owned-graphics.py \
  --runtime "$FX_WORK/runtime" --guest-arch x64 \
  --fixture "$FX_WORK/fixtures/owned-d3d12-fsr-fg.exe" \
  --suite fsr-fg --mode present \
  --fg-observer "$FX_WORK/fixtures/owned-fg-drawable-observer.dylib" \
  --validation --timeout 180 --out "$FX_WORK/run-fsr-fg"
python3 scripts/test-owned-graphics.py \
  --runtime "$FX_WORK/runtime" --guest-arch x64 \
  --fixture "$FX_WORK/fixtures/owned-d3d12-shader-cache.exe" \
  --suite shader-cache --mode no-present --timeout 180 \
  --out "$FX_WORK/run-shader-cache"
```

SR/FG fixture는 current temporal source를 rename-main include한다. Public headers/license와 reference checksums는 [integration](NGX-INTEGRATION.ko.md#owned-fsr-public-api와-provenance)에 있다. `enable_fsr=true`는 owned SR/FG frontend를 함께 빌드하고 stage의 `--with-fsr`는 **두 DLL 모두** 선택한다. Stage는 builtin marker와 각 five names/ordinals를 검증한다. FG reference ordinals는 Configure=1/Create=2/Destroy=3/Dispatch=4/Query=5다. 선택하지 않은 새 runtime에서는 두 foreign fallback을 차단한다. 과거 manifest에 `fsr_framegeneration` 필드가 없으면 verifier는 과거 SR-only loader 정책과 runtime bytes를 그대로 보존한다. 이는 FG 선택/자동 업그레이드가 아니므로 새 경로에 다시 stage해야 `fsr-fg`를 실행할 수 있다. `--with-nvngx`는 NGX 회귀 DLL도 선택하며 public NGX fixture는 [별도 MSVC compile recipe](NGX-TEST.ko.md#현재-fixture-빌드)를 사용한다. 현재 public SR/FG는 x64/WoW64를 유지한다. WoW64는 matching i686 fixture/install을 paired stage하고 `--guest-arch x86`으로 실행한다.

`fsr`의 full-SR 판정은 high-resolution/jittered motion·nonzero scale/effect, reactive/composition masks·history/reset, linear/sRGB/PQ, sharpening constant/edge/alpha, automatic/manual exposure·GPU-produced R32 replay, dynamic output generation, native scale cap·centered black-opaque margins, inflight release·command-list reset discard와 rejection sentinel을 함께 요구한다. `OWNED_FSR_NATIVE_MAX_SCALE`은 **같은 장치에서 독립 public MetalFX descriptor로 측정한 값**이어야 한다. M5 Pro에서 측정한 3을 portable constant로 복사하지 않는다. 현재 cap fixture는 input64×64의 native-extent control과 caller256×224의 중심 결과를 비교하고264×232 외부 sentinel을 보존한다.

`fsr-fg --mode present`는 `--fg-observer`가 필수다. Observer는 explicit **test-only** input이며 product telemetry가 아니다. x64 fixture에서도 arm64 wineserver가 injection 환경을 상속하므로 위 compile recipe처럼 x86_64+arm64 universal Mach-O를 사용한다. Runner는 각 slice의 macOS system-library/framework 의존성만 허용하고 observer/checker를 freeze·hash한 뒤 실제 drawable pixels/order/native presentation timestamps를 `fg-capture/`에 수집한다. `MTLDrawable.presentedTime=0`은 미표시/skipped로 보존하며 presentation 시도 수와 실제 positive-timestamp 표시 수를 구분한다. 반복 generated-before-real 표시 쌍과 only-generated 표시를 별도로 요구하지만 모든 시도의 scanout이나 정확한2× OS cadence를 보장하지 않는다. Callback GPU readback이나 `Present` QPC 소요시간만으로 실제 presentation을 주장하지 않는다. 원본 SDK의 generated-role-before-real·only-generated·disabled/drain, premultiplied UI GPU snapshot/doublebuffer/resize와 history/lifetime/config 경계도 runner의 별도 gate다. Generated-role callback은 actual interpolation count의 증거가 아니다. Ordinary ResizeBuffers는 유지하고 ResizeBuffers1은 E_NOTIMPL이다. Custom per-buffer queue extension이나 새 async Present 오류 protocol을 요구하지 않는다. NGX Present/pacing은 caller 책임이다.

보존할 FG 증거는 `fixture/fg-*.bin`, `fg-capture/drawable-*.bin`, `fg-capture-analysis.json`/`.log`, `invocation.json`의 observer hash·architectures·per-slice dependencies, `result.json`이다. 새 stage/output/prefix만 사용하고 runner가 해당 prefix의 wineserver를 종료하게 둔다. Live prefix 재사용/전역 wineserver 종료는 하지 않는다. 실제 staged owned `winemetal.so` load와 D3DMetal/`libd3dshared` 부재가 SR/FG 모두의 전제다.

SDK internal-double-buffer UI 등록은 **현재 Present에서 소비**된다. Pinned SDK의 `copyUiResource`가 `currentUiSurface`를 비우므로 UI를 표시할 각 Present 직전에 다시 등록해야 한다. 한 번 등록한 persistent binding이 아니다. Current fixture는 매 UI 프레임마다 재등록하며 strict native-pixel·Present 후 source mutation oracle을 유지한다. 이는 sample 사용 계약이지 production backend 버그 수정이 아니다.

Public DXGI FG 통계는 native positive `presentedTime`을 그대로 사용한다. Unsynced drawable의 refresh 내부 위상은 표시 실패가 아니며, CV refresh interval에 `floor`로 대응한다. CoreVideo output callback은 미래 refresh를 예고할 수 있으므로 `SyncRefreshCount`/`SyncQPCTime`은 caller가 관측한 host time까지 실제 경과한 마지막 refresh를 가리킨다. Shared public fixture는 마지막 Present count의 실제 표시와 `PresentRefreshCount <= SyncRefreshCount`, 관측 QPC보다 미래가 아닌 `SyncQPCTime`을 요구한다. 대기 상한을 늘리거나 QPC를 native timestamp 대신 사용하는 구현은 이 계약을 충족하지 않는다.

CPU-only 위상·refresh 경계 회귀 검사는 GPU 없이 실행한다:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror tests/dxgi_refresh_timing.cpp -o /tmp/dxgi-refresh-timing
/tmp/dxgi-refresh-timing
```

현재 public FG는 원본 pinned AMD SDK source-built swapchain/UI/pacing + 기존 WMTMetal4FG의 native MetalFX generation이다. Generated output 최대1개이며 FSR2–4 요청은 NO_PROVIDER, NGX count/index>1과 non-null OutputDisableInterpolation은 UnsupportedParameter다. SDK generated-role bootstrap/reset output은 current-color일 수 있다. Native warm-up과 actual drawable display는 별도로 관찰하며 public MetalFX에 없는 actual-count/status를 만들어 검사하지 않는다(pristine GPTK도 disable key를 무시한다). Closed-list replay는 native work를 다시 encode한다. 아래 독립 control은 Wine/FFX/production backend를 링크하지 않는 public MetalFX oracle다. Matching native inputs/reset/history의 비교와 SDK callback 순서/UI/pacing·실제 drawable pixels/order/timestamps를 별도 gate로 기록한다. Root27/28/30/34의 AMD phase/actual-count proof와 과거 checker의 `--native` 거부는 역사적 기록이지 현행 acceptance가 아니다. 현재 최종 root는 `build/local/fsr-metalfx-20261007-43df9ae7`이며 양 ABI FSR·NGX GPU·NGX Present 6개 실행이 API+Shader Validation으로 통과했다. 실제 표시 수·warm-up 경계는 CORE의 현재 FG 검증을 따른다.

```sh
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal -framework MetalFX \
  graphics-translator/tests/owned-fg-native-control.mm \
  -o "$FX_WORK/fixtures/owned-fg-native-control"
mkdir "$FX_WORK/native-fg-control"
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  "$FX_WORK/fixtures/owned-fg-native-control" "$FX_WORK/native-fg-control" \
  > "$FX_WORK/native-fg-control/stdout.log" \
  2> "$FX_WORK/native-fg-control/stderr.log"
```

Shader-cache fixture는 최신 llvm-mingw Device9 headers를 사용하므로 `-Iinclude/native/directx`를 앞세우지 않는다. **CPU API-only** suite라 위 명령은 GPU validation을 요청하지 않는다. Runner가 fresh prefix에만 `OWNED_SHADER_CACHE_CONTROL_ISOLATED=1`을 설정하며 fixture는 developer registry 값을 복원한다. 이 marker를 수동으로 live prefix에 설정하지 않는다. WoW64 cache는 같은 source를 `i686-w64-mingw32-clang++`로 compile하고 matching i686 install을 stage의 `--dxmt-install32`로 추가한 뒤 `--guest-arch x86 --suite shader-cache`로 실행한다.

위 명령은 **현재 full-SR/public-FG 검증 절차**이며 이번 문서 갱신 자체가 두 suite의 PASS를 뜻하지 않는다. 관측된 source/runtime cut과 최종 validation 결과는 [CORE status](CORE-IMPLEMENTATION-STATUS.ko.md)·[이력](IMPLEMENTATION-HISTORY.ko.md)이 authority다. 과거 SR의4 changed/reset/dynamic-render frames·destroyed-context replay3회·null/R16/R32 unit equality와 cache의4 case groups+5 child-process 관측을 새 full-SR/public-FG 결과로 합산하지 않는다. R32 numeric/replay/completion은 [complete temporal fixture](TEMPORAL-TEST.ko.md)에서 양 ABI로 검증한다. Native MetalFX tiny16→48 control에서도6 `Conv3x3` Shader Validation OOB stores가 재현된 platform 경계가 있다. 이를 suppress하거나 임의 minimum-size rejection으로 바꾸지 않으며64→192 cap fixture PASS가 tiny-input 무오류를 뜻하지 않는다. 이는 game/image-quality equivalence 또는 native-Windows cache parity가 아니다.

### Resources와 unaligned BC 검증

`owned-d3d12-resources.cpp`는 `main`이고 inline cs_5_0 shader를 `D3DCompile`하므로 **-ld3dcompiler**가 필요하다. `-municode`는 사용하지 않으며 별도 frozen shader 파일은 없다.

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++20 -O2 -Wall -Wextra -static \
  graphics-translator/tests/owned-d3d12-resources.cpp \
  -ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid \
  -o build/local/fixtures-smoke-001/resources-x64.exe
python3 scripts/test-owned-graphics.py \
  --runtime build/local/runtime-smoke-001 --guest-arch x64 \
  --suite resources --fixture build/local/fixtures-smoke-001/resources-x64.exe \
  --out build/local/resources-x64-001 --validation
```

WoW64는 같은 source를 `i686-w64-mingw32-clang++`로 빌드하고 staged i686 install이 포함된 같은 runtime에 `--guest-arch x86`로 실행한다. 기존 resource checks와 BC1–7 typed/typeless21 variant의 logical7×5 mips/array/placed·CPU/GPU copy·SRV/`GetDimensions`, mixed8×8↔7×5 direct/predicated copy, null/explicit physical boxes·edge·guards 및 volume/cube를 검증한다. OPTIONS8 native gate, null/short/long-size 오류와 copy block/pitch/offset 계약은 [resource 문서](D3D12-RESOURCES.ko.md#unaligned-bc-texture와-physical-block-copy)를 따른다. [최종 paired evidence](CORE-IMPLEMENTATION-STATUS.ko.md#options8-unaligned-bc-완료와-focused-proof)는 query-only baseline과 실제 physical-copy byte mismatch를 구분한다.

### DXIL texture와 programmable offsets 검증

`owned-d3d12-dxil-texture.cpp`는 `wmain`이다. Frozen inputs는 basic-texture VS/PS와 **programmable-offsets VS/PS/CS3개**이며 runner가 모두 snapshot/hash한다. 기존 `build-texture.sh`로 validator-enabled genuine DXC inputs를 생성한다. VS는vs_6_0, programmable PS/CS는ps_6_7/cs_6_7이고 SM6.7 metadata를 수동 변경하지 않는다.

```sh
sh graphics-translator/tests/dxil/build-texture.sh \
  "$WINE" "$DXC_EXE" "$DXC_PREFIX" build/local/texture-inputs-001
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++20 -O2 -Wall -Wextra -Werror -static -municode \
  graphics-translator/tests/owned-d3d12-dxil-texture.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -luuid \
  -o build/local/fixtures-smoke-001/dxil-texture-x64.exe
python3 scripts/test-owned-graphics.py \
  --runtime build/local/runtime-smoke-001 --guest-arch x64 \
  --suite dxil-texture --fixture build/local/fixtures-smoke-001/dxil-texture-x64.exe \
  --dxil-dir build/local/texture-inputs-001 \
  --out build/local/dxil-texture-x64-001 --validation --timeout 240
```

`$WINE`/`$DXC_EXE`은 검증된 producer, `$DXC_PREFIX`/output은 새 격리 경로다. 이미 frozen inputs를 사용하면 producer 재실행은 필요 없다. WoW64는 같은 source를 i686 compiler로 빌드하고 같은 inputs/runtime에 `--guest-arch x86`로 실행한다.

Original gather/LOD/sample-pattern gates와 `OWNED_D3D12_DXIL_TEXTURE_OFFSETS_PASS batches=2 lanes=16 families=22 components=2816`가 함께 필요하다. Offsets는 SRV buffer에서 lane/batch별로 읽고 독립 CPU uploaded-texel oracle과 비교한다. Load/Sample/compare signedlow4와 Gather/GatherCmp signedlow6를 구분하며 GatherRaw low6는 일반 gather 상속 해석이다. NPOT/view mip·dimension1·address5 modes·huge UV·1DArray·MS corner 경계와 실제 PS/compute 경로의 [최종 paired proof·입력 provenance 및 한계](CORE-IMPLEMENTATION-STATUS.ko.md#programmable-texture-offsets-완료와-focused-proof)를 따른다. `build-sm6.sh`나 compiler-only probe는 이 suite의 producer/GPU acceptance를 대신하지 않는다.

### Queries와 COPY timestamp 검증

`owned-d3d12-queries.cpp`는 `main`이므로 `-municode` 없이 빌드한다. Frozen shader 입력은 없으며 fixture가 DXBC shader를 컴파일한다. x64 recipe는 다음과 같고 WoW64는 같은 source를 `i686-w64-mingw32-clang++`로 빌드해 staged i686 install이 포함된 같은 runtime에 `--guest-arch x86`로 실행한다.

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++20 -O2 -Wall -Wextra -static \
  graphics-translator/tests/owned-d3d12-queries.cpp \
  -o build/local/fixtures-smoke-001/queries-x64.exe \
  -ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32
python3 scripts/test-owned-graphics.py \
  --runtime build/local/runtime-smoke-001 --guest-arch x64 \
  --suite queries --fixture build/local/fixtures-smoke-001/queries-x64.exe \
  --out build/local/queries-x64-001 --validation --timeout 240
```

별도 COPY-only mode 없이 **full queries**를 실행한다. OPTIONS3 cap TRUE, COPY 전용 heap↔COPY list의 End/Resolve와 일반 heap↔DIRECT/COMPUTE 상호 배타성, timestamp Begin 및 index/count/alignment/bounds/ownership의 정확한 오류를 확인한다. COPY는 8 round×3개 별도 Execute, 중간 CPU wait 없음·한 final fence·completion 전 release, timestamp-only successor·same-slot reuse·duplicate resolve·각 resolve 사이 guard·nonzero offsets와 총524288-byte exact copy payload를 검사한다.

`OWNED_D3D12_QUERY_COPY_VALUES` 8개, `COPY_CALIBRATION` 및 `COPY_PASS`와 기존 `QUERY_PASS`가 모두 필요하다. Runner는 sample ordering·중복 resolve equality·calibration bracket을 판정하며 frequency는 nonzero·반복 조회 일치를 요구할 뿐 특정 장치의24MHz로 고정하지 않는다. 이는 **COPY duration 정확성/성능 또는 동기화 없는 cross-queue 비교 proof가 아니다**. 최종 clean fixture의 실제 [x64/WoW64 evidence와 한계](CORE-IMPLEMENTATION-STATUS.ko.md#copy-queue-timestamp-완료와-focused-proof)를 따른다. Baseline-only throwaway source/mode는 production 실행 옵션이 아니다.

### Root/indirect arena 성장 검증

`owned-d3d12-arena-heap.cpp`는 `main`이므로 `-municode` 없이 빌드한다. `-std=c++20 -O2 -Wall -Wextra -static`과 `-ld3d12 -ldxgi -ld3dcompiler -lpsapi -luuid`를 사용한다. x64는 `$LLVM_MINGW/x86_64-w64-mingw32-clang++`, WoW64 x86는 `$LLVM_MINGW/i686-w64-mingw32-clang++`다. 별도 shader 파일은 필요 없고 packed 62-DWORD cbuffer의 끝 DWORD까지 실제로 읽는 DXBC shader를 fixture에서 컴파일한다.

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++20 -O2 -Wall -Wextra -static \
  graphics-translator/tests/owned-d3d12-arena-heap.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -lpsapi -luuid \
  -o build/local/fixtures-smoke-001/arena-heap.exe
python3 scripts/test-owned-graphics.py \
  --runtime build/local/runtime-smoke-001 --guest-arch x64 \
  --suite arena-heap-arena --fixture build/local/fixtures-smoke-001/arena-heap.exe \
  --out build/local/arena-growth-x64-001 --validation --timeout 180
```

WoW64는 같은 source를 i686 compiler로 빌드하고 staged i686 install이 있는 runtime에 `--guest-arch x86`로 실행한다. Output은 항상 새 경로다. 현재 marker는 `single=20000 shared=24000 indirect=20000 reexecute=2 reset=1 first_error=E_INVALIDARG`이며 과거 fixed-capacity OOM marker를 재사용하지 않는다. Replay 전에 GPU output을 지워 재실행을 증명하고, queue gate로 pending Reset 거부와 retirement 후 Reset/new recording을 확인한다. `arena-heap-mapping`은 별도 mapping cleanup 계약이다.

전체 payload acceptance는 이 suite 하나로 대체하지 않는다. Predication/graphics의 indirect root/static sampler/VB, WG/DXR 경계와 격리 one-shot native allocation failure proof는 [CORE의 완료 범위·mixed-cut 실제 증거](CORE-IMPLEMENTATION-STATUS.ko.md#rootindirect-arena-성장과-focused-proof)를 따른다. 실패 주입은 production hook/config가 아니며 물리 메모리 고갈 proof도 아니다.

### Residency budget notification 검증

현재 source install과 위의 staging recipe를 재사용한다. 양 ABI 실행에는 같은 generation의 i686 install을 `stage --dxmt-install32`로 함께 넣은 runtime이 필요하다([WoW64](#wow64)). 아래 새 fixture/output 이름은 retained evidence 경로를 덮어쓰지 않는 예시이며, 재사용 시에도 존재하지 않는 이름으로 바꾼다. `residency`는 `main`이므로 **`-municode`를 붙이지 않는다.** 별도 HLSL/DXIL producer나 새로운 일반 build target은 필요 없다.

```sh
(
  set -eu
  mkdir build/local/fixtures-residency-budget-002
  for guest in x64 x86; do
    case "$guest" in
      x64) target=x86_64 ;;
      x86) target=i686 ;;
    esac
    fixture="build/local/fixtures-residency-budget-002/residency-$guest.exe"
    "$LLVM_MINGW/$target-w64-mingw32-clang++" \
      -std=c++20 -O2 -Wall -Wextra -static \
      graphics-translator/tests/owned-d3d12-residency.cpp \
      -ld3d12 -ldxgi -ldxguid -luuid -o "$fixture" \
      > "build/local/fixtures-residency-budget-002/build-$guest.log" 2>&1
    python3 scripts/test-owned-graphics.py \
      --runtime build/local/runtime-smoke-001 --guest-arch "$guest" \
      --suite residency --fixture "$fixture" \
      --out "build/local/residency-budget-$guest-002" --validation
  done
)
```

새 required marker만 보고 기존 GPU acceptance를 생략하지 않는다. 공용 runner의 backend/load·exit0·validation gate와 다음 값이 **함께** 필요하다.

| Gate / 보존 출력 | 판정 |
|---|---|
| `OWNED_D3D12_RESIDENCY_BUDGET_PASS` | 새 필수 marker가 정확히1회. Event type/권한·독립 active cookie·같은 event의 중복 등록·caller-close·unregister·adapter teardown 계약 |
| `OWNED_D3D12_RESIDENCY_BUDGET_INFO`, `_MEASUREMENT`, `_TEARDOWN` | Local/nonlocal Budget·usage·reservation, 관찰 시간·실제 변화 수·event wakes, teardown 시간·남은 refs를 보존. `physical_change_required=0`: 실제 native 변화가 없다고 실패시키지 않으며, zero wake PASS를 positive transition proof로 세지 않음 |
| `OWNED_D3D12_RESIDENCY_CASE validation=1 refcounts=1 create_not_resident=1 priority=1` | 기존 argument/refcount·create-not-resident·priority 검사 유지 |
| `OWNED_D3D12_RESIDENCY_FENCE value=7 completed=7` | 기존 enqueue residency fence completion 유지 |
| `OWNED_D3D12_RESIDENCY_READBACK bytes=256 checksum=0x59047300` 및 `OWNED_D3D12_RESIDENCY_PASS` | 기존 exact GPU output gate 유지; budget CPU 검사로 대체 불가 |

Reservation은 실제 측정한 값을 기록한다. 모두0인 restore를 nonzero reservation proof로 소개하지 않는다. Native source와 공개 계약, 실패→수정→x64/WoW64 결과 및 manifest는 [CORE의 DXGI budget notification 완료와 focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#dxgi-budget-notification-완료와-focused-proof)가 단일 기준이다. 이 새 실행은 과거 recovery118 집계와 별개다.

#### Standalone controlled-source CPU PE probe

`graphics-translator/tests/dxgi-budget-notification.test.cpp`는 **실제 production helper를 PE에 compile하고 실제 Win32 Event를 쓰는** CPU probe다. Native WMT/Metal 값 대신 제어 가능한64-bit source를 사용하므로 실제 물리 budget 변화나 GPU 결과의 proof가 아니다. Entry가 `main`이며 **`-municode` 없이** `-std=c++20 -O2 -Wall -Wextra -static -Isrc/util`로 양 ABI를 만든다. 공용 graphics runner의 suite가 아니므로 Wine으로 직접 실행한다.

다음 명령은 위에서 준비한 private runtime을 쓰되 **guest마다 새 output/prefix**를 만든다. 같은 runtime의 scoped wineserver만 `-k` 후 `-w`로 정리하며 stdout/stderr·exit status·cleanup 로그·PE/compile 로그는 삭제하지 않는다. `env -i`는 inherited foreign backend/loader 설정이 probe에 섞이지 않게 한다.

```sh
(
  set -eu
  runtime="$PWD/build/local/runtime-smoke-001"
  mkdir build/local/fixtures-budget-cpu-002
  for guest in x64 x86; do
    case "$guest" in
      x64) target=x86_64 ;;
      x86) target=i686 ;;
    esac
    fixture="$PWD/build/local/fixtures-budget-cpu-002/budget-$guest.exe"
    "$LLVM_MINGW/$target-w64-mingw32-clang++" \
      -std=c++20 -O2 -Wall -Wextra -static -Isrc/util \
      graphics-translator/tests/dxgi-budget-notification.test.cpp -o "$fixture" \
      > "build/local/fixtures-budget-cpu-002/build-$guest.log" 2>&1
    (
      out="$PWD/build/local/budget-cpu-$guest-002"
      mkdir "$out"
      run_wine() {
        env -i PATH="$PATH" HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" \
          WINEPREFIX="$out/prefix" WINEDLLPATH="$runtime/lib/wine" \
          WINEDEBUG=+loaddll WINEDLLOVERRIDES='winemenubuilder.exe=d' "$@"
      }
      trap 'run_wine "$runtime/bin/wineserver" -k >> "$out/cleanup.log" 2>&1 ||
              printf "wineserver -k failed\n" >> "$out/cleanup.log";
            run_wine "$runtime/bin/wineserver" -w >> "$out/cleanup.log" 2>&1 ||
              printf "wineserver -w failed\n" >> "$out/cleanup.log"' EXIT
      status=0
      run_wine "$runtime/bin/wine" "$fixture" \
        > "$out/stdout.log" 2> "$out/stderr.log" || status=$?
      printf '%s\n' "$status" > "$out/exit-status.txt"
      exit "$status"
    )
  done
)
```

Exit0와 `DXGI_BUDGET_NOTIFICATION_PASS`뿐 아니라 **6 subcase marker**도 확인한다: `DXGI_BUDGET_TRANSITIONS_COOKIES_PASS`, `DXGI_BUDGET_HANDLE_RIGHTS_LIFETIME_PASS`, `DXGI_BUDGET_REGISTER_PENDING_CHANGE_PASS`, `DXGI_BUDGET_REAL_WORKER_UNREGISTER_ORDER_PASS`, `DXGI_BUDGET_STOP_ORDER_PASS`, `DXGI_BUDGET_DESTRUCTION_ORDER_PASS`. Low32 bits가 같은8GiB→4GiB→8GiB·unchanged sample·같은/다른 event 구독·modify-only/caller-close 수명과 deterministic source callback barrier를 통한 등록/worker/unregister/Stop/destruction ordering이 대상이다. Exhaustion fault injection이나 native callback/KVO 보장 검증은 아니다.

최종 retained proof는 [`observer-x64-ordered/stdout.log`](../build/local/budget-notification-20261002/observer-x64-ordered/stdout.log)와 [`observer-x86-ordered/stdout.log`](../build/local/budget-notification-20261002/observer-x86-ordered/stdout.log)다. 실제 native residency의 zero-change 관찰과 이 controlled-source transition proof를 합쳐 물리 변화가 관찰됐다고 하지 않는다.

### 기존 shader·resource suite compile recipe

아래 fixture들은 `graphics-translator/tests/owned-d3d12-<suite>.cpp`이며 공통 링크는 `-ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32`다. Device5+·CommandList5+·OPTIONS12+ 타입을 위해 **`-Iinclude/native/directx` 없이 llvm-mingw의 더 최신 headers**를 사용한다. Vendored header를 무조건 앞세우면 현재 fixture interface가 사라질 수 있다. `wmain`에만 `-municode`를 붙인다.

| Suite | Entry | 범위 |
|---|---|---|
| `sm6` | `wmain` | Wave/quad, int64/16-bit, barycentrics, compute atomic64·groupshared64 full64 oracle, SV_ViewID, denorm, required64/128 wave 거부와 compute CalculateLOD. `calculate_lod`·`calculate_lod_shapes` 포함; graphics Atomic64 positive proof는 별도 pixel40/producers18 readback suite를 사용하며 실행별 case 수·확장은 CORE 증거를 따름 |
| `waves` | Shared `sm6`의 `wmain` | `--waves-only` focused24 case; min4/max32·기본32와 fixed/ranged·divergence·i64·명시적64/128 거부·기존32 graphics 회귀 |
| `fp64` | Shared `sm6`의 `wmain` | `--fp64-only` focused8 case; double cap TRUE·DXIL/DXBC5.0/5.1 compute·VS/PS·wave의 exact binary64 numerical oracle |
| `atomic64-phase` | `wmain` | GPU-written indirect count/grid/root/predicate의 nonzero·zero3경로, shared-resource/fence future-producer 양방향 single-writer handoff, ordinary32/+4 vector alias 및 canonical copy. 최종 positive Atomic64 cap runtime에서 실행하며 개별 phase PASS를 전체 Shader Model conformance로 확대하지 않음 |
| `work-graphs` | `main` | Full9 case; coalescing NodeWave4의 실제 wave count/sum/ballot/shuffle 및 기존 record oracle |
| `bindless` | `wmain` | Tier3 resources, dynamic resources, RS1.2, heap ROV; 이 fixture만으로 SM6.6 전체 지원 선언하지 않음 |
| `enhanced-barriers` | `main` | Device10–14/CommandList5–9와 barrier/dynamic-state 계약 |
| `render-passes` | `main` | Fallback, GPU upload heap, UINT border |
| `tiled` | `main` | Reserved/tiled resources |
| `pipeline-library` | `wmain` | `ID3D12PipelineLibrary1`, serialization/cross-process reuse |
| `depth-bounds` | `main` | Native-gated OPTIONS2·DESC1/2·dynamic state·early/late·1×/4× exact GPU50 cases |
| `sharing` | `wmain` | Shared heap/resource/fence NT handles, cross-process/named |
| `geometry`, `tessellation` | `main` | DXBC/DXIL GS/HS/DS, TS+GS |

예를 들어 `pipeline-library`의 fresh fixture compile/run:

```sh
mkdir -p build/local/fixtures-library-001
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" \
  -std=c++20 -O2 -static -municode \
  graphics-translator/tests/owned-d3d12-pipeline-library.cpp \
  -ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32 \
  -o build/local/fixtures-library-001/owned-d3d12-pipeline-library.exe
python3 scripts/test-owned-graphics.py \
  --runtime build/local/runtime-smoke-001 --suite pipeline-library \
  --fixture build/local/fixtures-library-001/owned-d3d12-pipeline-library.exe \
  --out build/local/pipeline-library-001 --validation
```

`sm6`의 CalculateLOD 갱신에서는 fixture PE와 함께 **`cs_quad_deriv`·`cs_lod`·`cs_lod_shapes` 세 shader를 재빌드해야 한다.** 기존 `graphics-translator/tests/dxil/build-sm6.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>` recipe를 사용하며 이 세 entry는 `cs_6_8`이다. 기록된 focused proof의 DXC는 **1.9.2609.5**다. 새 output의 `sm6-cs_quad_deriv.dxil`·`sm6-cs_lod.dxil`·`sm6-cs_lod_shapes.dxil`을 포함한 suite 입력 집합을 runner의 `--dxil-dir`로 전달하고 오래된 shader를 섞지 않는다. Script 실행은 GPU PASS가 아니며 fixture를 실제 x64/WoW64 guest에서 각각 실행해야 한다.

신규 `calculate_lod`는20개 analytical 2D case의 sampler bias·Min/MaxLOD·view clamp·anisotropic footprint, `calculate_lod_shapes`는1D/1Darray/2Darray/3D/cube/cubearray와 cube6 face/scale·signed quotient 진단을 검사한다. Array LOD는 spatial coordinates만 받으며 slice 제외 검증으로 소개하지 않는다. SM6·dxil-texture의 LOD oracle은 **mip-linear**로 정정했고 undefined POINT-mip LOD 결과를 근거로 세지 않는다. ABI별28 case PASS를 포함한 새4실행·before 실패·manifest·정확한 지원 한계는 [CORE의 Compute CalculateLOD 완료와 focused proof](CORE-IMPLEMENTATION-STATUS.ko.md#compute-calculatelod-완료와-focused-proof)가 단일 기준이다. 이4실행은 과거 recovery118 집계와 별개다.

`partial-3d-uav-dxil`의 `partial-3d-uav.dxil` 및 과거 P2 개발 단계의 frozen DXIL/DXBC는 runner가 기본 shader directory에서 가져온다. DXIL producer를 변경할 작업이 아니라면 shader를 임의 재컴파일하지 않는다. 위 CalculateLOD의 세 shader 갱신은 필요한 입력 변경이며 frozen import/provenance를 수정한다는 뜻이 아니다. `build-raytracing-rayquery-stages.sh`는 DXC 1.9로 추가 stage/callback/state-object 입력을 만들던 producer이며, 해당 script 실행만으로 GPU PASS를 얻은 것은 아니다. Current source와 frozen input의 정확한 coverage는 CORE와 이력에서 확인한다.

### CoreWindow/Composition reachable-only consumer

`corewindow-composition`은 실제 `RoGetActivationFactory`와 `ICoreWindowStatic::GetForCurrentThread`를 호출한다. 전체 `windows.ui.core.h` projection 대신 공개 IID `4d239005-3c2a-41b1-9022-536bb9cf93b1`과 `IInspectable` 뒤의 단일 `STDMETHODCALLTYPE` slot만 선언한다. `ICoreWindow`는 opaque pointer이며 실제 runtime object의 inherited identity만 사용한다. 가짜 CoreWindow, vendor SDK 수정, generated header guard suppression은 없다.

```sh
for pair in x86_64:x64 i686:x86; do
  target=${pair%:*}; arch=${pair#*:}
  "$LLVM_MINGW/$target-w64-mingw32-clang++" -std=c++20 -O2 -static \
    graphics-translator/tests/owned-d3d12-corewindow-composition.cpp \
    -ld3d12 -ldxgi -ldcomp -lruntimeobject -lole32 -luuid \
    -o "$FIXTURES/corewindow-composition-$arch.exe"
  python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" \
    --suite corewindow-composition --guest-arch "$arch" --mode present --validation \
    --fixture "$FIXTURES/corewindow-composition-$arch.exe" \
    --out "$RESULTS/corewindow-composition-$arch"
done
```

`FIXTURES`는 미리 만든 directory, `RESULTS`는 ABI별 새 evidence directory를 담을 parent다. GPU 실행은 다른 suite와 겹치지 않게 한다. Runner는 기존 runtime 검증·입력 snapshot/hash·fresh prefix·native-load/validation 검사·prefix cleanup을 그대로 사용한다.

최종 producer marker는 **`OWNED_D3D12_COREWINDOW_COMPOSITION_REACHABLE_OK composition=blocked matrix_pixel_oracle=blocked`**이며 `_PASS`가 아니다. Reachable 계약 검증 성공의 exit code는0이지만 `result.json`은 **`result=REACHABLE_OK`, `reachable_only_verified=true`, `pass=false`, `feature_complete=false`**다. `corewindow_composition`에는 `actual_corewindow=0/1`, 실제 CoreWindow/DComposition runtime HRESULT, `composition=blocked`, `matrix_pixel_oracle=blocked`, prerequisite details를 남긴다. 실제 object가 없으면 CoreWindow GPU present를 실행한 것으로 세지 않는다. Object가 있어 identity·present·resize를 검증해도 native DComposition visual/target attachment와 transformed-pixel oracle는 여전히 미검증이므로 전체 P4 CoreWindow/Composition 완료로 확대하지 않는다.

CPU-only parser 회귀는 `python3 scripts/test-owned-graphics-diagnostics.py RunnerFailures.test_corewindow_reachable_contracts_never_claim_feature_completion`으로 실행한다. Synthetic parser input은 실제 runtime/GPU 증거가 아니다.

### WoW64

`atomic64-phase` 입력은 `sh graphics-translator/tests/dxil/build-atomic64-phase.sh "$WINE" "$DXC_EXE" "$DXC_PREFIX" "$PHASE_OUT"`로 final DXC의 validator-enabled `cs_6_6` 4개 entry를 생성한다. 같은 output에 `atomic64-phase.hlsl`·`build-atomic64-phase.sh`를 복사해 runner snapshot에 포함한다. `owned-d3d12-atomic64-phase.cpp`를 위 공통 llvm-mingw/link 옵션과 `-municode`로 x64·i686 각각 빌드하고, exclusive GPU window에서 `scripts/test-owned-graphics.py --runtime "$RUNTIME" --fixture "$PHASE_OUT/phase-x64.exe" --dxil-dir "$PHASE_OUT" --suite atomic64-phase --validation --out <새-결과경로>`로 실행한다(x86은 `phase-x86.exe --guest-arch x86`). Producer prefix는 해당 runtime의 `wineserver -k`·`-w` 후 정리한다. Runner는 PASS 문구가 아니라 모든 old/final/canonical/ordinary 값·GPU argument21words×4·work count·guard·+4 raw/vector7words×6·future wait의 미완료 fence/event와 retirement/rewind를 독립 oracle로 비교한다. CPU parser corruption check는 `python3 scripts/test-owned-graphics-diagnostics.py Atomic64PhaseEvidence`이며 GPU 결과를 대체하지 않는다.

`i686-w64-mingw32-clang++`로 PE32 fixture를 만들고 entry/link 원칙은 동일하게 적용한다. 같은 generation의 i686 Meson install을 `stage --dxmt-install32`로 추가한 runtime에 `--guest-arch x86`으로 실행한다. x64 fixture 실행을 WoW64 결과로 세거나 ARM64 Wine에 이 옵션만 붙여 x86 guest 실행이 가능하다고 하지 않는다.

## 역사적 관측과 retained evidence

아래는 **2026-09-19~21 관측 요약**이며 최신 source/runtime의 재검증이 아니다. 과거 `build/owned-*`와 `/private/tmp/owned-zzz-*`는 실행 출처 식별자이고 현재 파일 존재를 뜻하지 않는다. 당시 runtime·prefix·대형 binaries를 정리했고 이후 `build/` 삭제로 많은 log도 유실됐다. 예전 “retired-evidence에 보존했다”는 당시 조치이지 현재 availability 보증이 아니다. `doc/provenance`에 실제 남아 있는 manifest/result JSON과 CORE의 [증거 가용성](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)을 구분한다.

### PSO cache 구현과 실행 검증 (2026-09-21)

당시 canonical root는 `build/owned-direct-canonical-cache-20260921`, 18-patch build였다. Native plugin hash는 `ea487e843a1c7990b8bc0d9d297e9fd3e3f46666788921a6d8fda7ce6fd56e37`; 당시 `0018` hash는 `0340701953bec5d19491ec0a0c66637b6ccb8862f532e517e5cfa3e0837bec2d`였다. Retained [import build](../doc/provenance/import-build.json), [runtime manifest](../doc/provenance/import-runtime.json)은 frozen import 식별 자료다.

- 별도 process export/import PASS: original VS와 constant-array CS/PS의 direct→indirect→direct, inheritance ON/OFF, CS `13579bdf/31415926/fedcba98` 및 PS 16/24/24 pixels. Cache/원본 PSO 해제, 반환 blob 변조 뒤 재export도 검사했다. Retained [populate](../doc/provenance/cache-populate-result.json), [restore](../doc/provenance/cache-restore-result.json).
- Permanent fixture는 null/nonempty, truncation/corruption/foreign blob, root/shader mismatch 거부와 unaligned input 수명·blob 격리를 검사했다. Retained [pipeline-cache result](../doc/provenance/gpu-pipeline-cache.json).
- 정상 2건 + 거부 14건은 native build/version/adapter/OS identity, size/alignment/count/relocation/layout/PSO kind 경계를 검사했다. Format 변조는 rejection fixture이지 정상 shader output 증거가 아니다. Retained [rejections](../doc/provenance/cache-rejections-result.json).
- 그 당시 `CreatePipelineLibrary`는 `E_NOTIMPL`이었다. **현재에는 구현돼 있으며 별도 `pipeline-library` suite로 검증한다.** CachedPSO PASS와 library PASS를 같은 결과로 취급하지 않는다.
- 당시 build/staging/diagnostics 16/21/42 PASS는 해당 frozen cut의 역사적 count다. 현재 test count로 재인용하지 않는다.

### 자체 DXIL compiler 실행 검증 (2026-09-20)

당시 immutable-storage canonical root는 `build/owned-direct-canonical-icb-20260920`, 17-patch build였다. Native plugin hash는 `34f794cd5efb8a4fe270e15b0df1c2dbb563c551f81d4e1d32f706b2529ee075`, compiler patch hash는 `af756950f846b88cfd43809ee4c98d41ecc256e978a173d74edcaf1dd09fb472`였다.

- DXIL/IA/static-sampler exact GPU results, triangle 10 + procedural 4 DXR lanes 및 raygen별 다른 local-root layout PASS. Retained [DXIL](../doc/provenance/gpu-dxil.json), [IA](../doc/provenance/gpu-dxil-ia.json), [sampler](../doc/provenance/gpu-dxil-static-sampler.json), [RT](../doc/provenance/gpu-raytracing.json), [raygen layouts](../doc/provenance/gpu-raygen-layouts.json).
- `UpdateScratchDataSizeInBytes=0`은 `ALLOW_UPDATE`가 없는 prebuild contract에서 정상이다. 당시 fixture의 positive result/build scratch와 전체 readback은 유지하고 update scratch만 0을 허용했다. Zero result/build scratch를 성공으로 바꾼 것이 아니다.
- Indexed direct→indirect→direct는 16/32-bit index와 inheritance ON/OFF, 16/24/24 pixels를 검사했다. Multi-indexed는 GPU count 0/3/7 → 실제 0/3/4 commands, stride/argument offset 28 bytes, count offset 4 bytes, 2,304 uint exact였다. 잘못된 최초 base-vertex probe는 implementation regression으로 세지 않았다.

### 명시적 immutable storage와 ICB

당시 implicit constant-array AIR storage는 ICB-capable PSO의 direct draw에서도 invalid load를 재현했다. Apple 내부 원인은 확정하지 않았다. Compiler-owned immutable pool은 배열 stride/struct padding/alignment/bit pattern과 pointer relocation을 보존하고 buffer slot 29로 전달했다. Slot30 indirect resolver/DXR callback ABI와는 별개이며 모든 caller/thunk를 함께 재빌드했다.

| 역사적 probe | 관측과 제한 |
|---|---|
| Native vertex/stages | 원본 VS 24-byte pool과 64/64 pixels; 추가 native CS/PS에서 NaN payload/negative zero/alignment/padding/relocation/helper 전달 검사. 추가 native AIR smoke를 새 HLSL/DXIL 검증으로 세지 않음. |
| D3D12 stage storage/switch | CS `13579bdf/31415926/fedcba98`, PS16/24/24; A→B→pool-free→A→B→A에서 CS6개/PS6영역과 A/B32-byte pool, pool-free size0/null 검사. Sentinel로 stale readback 배제. |
| Compiler lifetime | 6 shaders × 8 concurrent compiles = 48 결과 일치. Consumer ASan은 compiler 자체의 TSan race 부재 증거가 아님. |
| Large storage | 81,920-byte pool, offsets81916/81904/81892, CS `9a821c56/3bbd2516/c1efc372` exact. 해당 장비의 64KiB 이후 주소 검증이지 모든 장비의 상한이 아님. |
| Indirect transition | Probe는 graphics를 두 번 실행해 기존 runner의 단일-set 계약과 달랐고 `result.json pass=false`였다. 별도 scenario 판정은 성공했지만 실패 report/기대값을 바꿔 PASS로 덮지 않았음. |
| AS/local root/sampler | BLAS/TLAS/refit/clone/compact/postbuild와 local-root 선택, sampler `0x3f000000`/sentinel 검사. `ExitProcess` probe는 정상 teardown 증거가 아님. |

이 표의 probe log/source가 전부 지금 남아 있다고 주장하지 않는다. Retained 정규 suite JSON과 역사적 상세 관측은 별개다. Stderr shader error reporting을 켜지 않았던 실행의 “오류 없음”을 전체 Shader Validation 부재 증거로 사용하지 않는다.

### 기존 검증 기록과 게임 관측의 한계

- Register-space 결함은 `b0,space2`/`b0,space9`의 GPU 값 `2468ace0,2468ace0` → `13579bdf,2468ace0`로 확인·수정했다. 원본 shader/fixture 정확성을 유지한 역사적 회귀다.
- 실제 ZZZ shader/root signature 16쌍의 compile + Metal newLibrary 및 반복 compile 일치는 해당 shader GPU output 정확성·게임 성공 증거가 아니다.
- 격리 game probe에서 startup PSO 오류 해소 뒤에도 검은 창이었다. Xcode trace는 19 command buffers/111 draw/6 dispatch와 검은 최종 출력이었다. Shader Validation은 capture 실행에서 껐고 별도 validation run과 구분했다. Backend error가 없던 60초 관찰이나 screenshot 없는 후속 canonical run도 시각적 성공으로 세지 않았다.
- 이 역사적 game probe는 현재 운영 예제가 아니며 새 검증을 위해 live 설치/prefix를 사용하지 않는다. 게임 호환성·DLSS·RT 완성을 위 관측에서 추론하지 않는다.

### pinned DXMT D3D12에서 확인한 실제 미완성 지점

Upstream `7c8dee1c...`의 ResourceBarrier TODO와 bundle/tiled 등 gaps는 import 전 참조 분석이었다. **현재 루트의 ResourceBarrier·bundles·tiled 구현이 미완성이라는 진술이 아니다.** 같은 readback이 upstream에서 성공해도 state tracking correctness는 증명되지 않는다는 역사적 판단만 유지한다. 현재 구현·소스 감사 후보는 CORE에 따른다.
