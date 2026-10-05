# dxmt-dx12

DXMT 기반 D3D12 → Metal 번역기와 자체 DXIL/DXBC → AIR/metallib compiler.
현재 개발 기준은 이 저장소 루트의 `src/`다. Apple Metal Shader Converter, GPTK/D3DMetal,
SPIR-V compiler fallback을 사용하지 않는다.

## 목표와 개발 범위

- **D3D12 게임 번역기:** macOS에서 게임의 렌더링 정확성·호환성·성능·표시와 게임이 쓰는 선택 기능을 구현한다. 공개 규격을 기준으로 삼되 범용 Windows API 완전성 자체를 목표로 삼지 않는다.
- **최종 실행 구성:** ARM64-native 번역기·AIR compiler·Metal bridge + ARM64 Wine + FEX의 x86/x64 guest 실행으로 Rosetta 의존을 제거한다. 현재 x64/WoW64 검증과 native plugin 검증을 이 최종 구성의 완료로 보지 않는다.
- **D3D12 중심:** D3D11은 DXMT 업스트림 구현을 유지한다. D3D12에 필요한 공유 API/ABI 변경과 기존 동작 보존만 최소 범위로 다룬다.
- **정확성 우선:** API 성공, shader 변환 성공, 개별 fixture PASS와 전체 feature/tier conformance를 구분한다. No-op이나 capability 축소를 완전 구현으로 간주하지 않는다.

## 문서 안내

현재 상태는 README와 CORE, 미완료 작업·실행 순서는 PRIORITIES, 날짜별 관찰은 HISTORY에서 관리한다. 기술 문서의 과거 실행 기록은 현행 지원 선언이나 새 실행 recipe를 대신하지 않는다. 과거 완료한 P0–P3와 새 backlog P0–P4는 동명일 뿐 다른 작업이다.

| 읽을 내용 | 문서 |
|---|---|
| 현재 지원 상태·구현·검증 한계 | [구현 상태](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md) |
| 미완료 작업·사용자 선택에 따른 실행 순서 | [구현 우선순위](graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md) |
| 단계별 구현·실행의 상세 이력 | [구현 이력](graphics-translator/IMPLEMENTATION-HISTORY.ko.md) |
| ARM64/ARM64EC/ARM64X·Wine/FEX 구성과 남은 실행 단계 | [ARM64/FEX](docs/ARM64-FEX-IMPLEMENTATION.md) |
| 최신 번역기 리뷰의 원래 지적·수정 판정·후속 증거 | [2026-10-01 리뷰와 10-02 수정](docs/D3D12-TRANSLATION-REVIEW-2026-10-01.md) |
| Resource·format·view·API 계약 | [Capabilities](graphics-translator/CAPABILITIES.ko.md), [Resources](graphics-translator/D3D12-RESOURCES.ko.md), [Core contracts](graphics-translator/D3D12-CORE-CONTRACTS.ko.md), [Bundles](graphics-translator/D3D12-BUNDLES.ko.md) |
| Queue·fence·여러 queue의 순서와 수명 | [Queue order](graphics-translator/QUEUE-ORDER.ko.md), [Fence events](graphics-translator/FENCE-EVENTS.ko.md), [Multiqueue](graphics-translator/MULTIQUEUE.ko.md) |
| 현재 빌드·LLVM 설정·디버깅 | 아래 [빌드](#빌드), [Development](docs/DEVELOPMENT.md) |
| 격리 runtime staging·fixture 빌드/실행 | [Runtime](graphics-translator/RUNTIME.ko.md), [Smoke test](graphics-translator/SMOKE-TEST.ko.md) |
| NGX frontend·temporal 연동·테스트 | [NGX frontend](graphics-translator/D3D12-NGX-FRONTEND.ko.md), [Integration](graphics-translator/NGX-INTEGRATION.ko.md), [Contracts](graphics-translator/NGX-CONTRACTS.ko.md), [NGX tests](graphics-translator/NGX-TEST.ko.md), [Temporal tests](graphics-translator/TEMPORAL-TEST.ko.md), [Completion tests](graphics-translator/TEMPORAL-COMPLETION-TEST.ko.md) |
| 설정·용어·업스트림 HUD 구분 | [Customization](docs/CUSTOMIZATION.md), [Glossary](docs/GLOSSARY.md), [Compatibility flags](docs/COMPATIBILITY_FLAG.md) |
| Import 재현·과거 조사 | [Frozen build](graphics-translator/BUILD.ko.md), [Temporal/NGX review](graphics-translator/API-REVIEW.ko.md), [ZZZ historical analysis](graphics-translator/ZZZ-COMPATIBILITY.ko.md), [Upstream origin](docs/UPSTREAM-README.md) |

## 현재 상태의 읽는 법

**2026-10-02 기준, 과거 개발 단계의 지정 P0–P3 범위와 D3D12 Metal 4 전환의 fixture acceptance는 완료했다.**
새 backlog의 P3는 별개다. **#17 Mesh/AS derivatives는 paired runtime-01의 x64/WoW64 수치·capability proof로 완료했으며**, 기능별 후속 완료와 남은 통합 acceptance는 아래와 PRIORITIES에서 구분한다. 2026-10-03 후속 사용자 지시로 **구형 WaveMMA와 후속 SM6.10 matrix를 모두 구현하고 남은 P3를 닫는 작업을 재개했다**. [구현 우선순위](graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md)의 현재 실행 선택과 날짜별 중간 관찰을 구분한다.
이것은 최신 규격 전체가 구현됐다는 뜻이 아니다. 현재 공개 상한은 **FL12_1 / SM6.5**이며,
DXR 1.1·mesh 1·VRS 2·sampler feedback 1.0·view instancing 1·Work Graphs 1.0을 보고한다.
Matrix는 별도 pinned preview 계약으로 **WaveMMATier1.0(10) / LinearAlgebraTier1.0(16)**을 보고한다.
**ConservativeRasterizationTier3도 완료했다(2026-10-04).** Genuine DXIL/DXBC InnerCoverage·degenerate/full-coverage를 최종 runtime03의 x64/WoW64 cap3 public consumer로 검증했다. [현행 계약·named 증거](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof)의8/8 acceptance는 **5개 runtime03+3개 retained runtime02 x64 회귀**이며 같은 build의8회 실행이 아니다. **SM6.5/FL12_1은 변경하지 않았다.**
기능별 조건, 상위 tier 공백, 남아 있는 API no-op과 compiler 의미 누락은
[현재 상태와 남은 구현 후보](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#남은-구현-후보)에서 구분한다.
Pipeline library, tiled resource, GS/HS/DS, pipeline statistics는 이미 구현되어 있으므로 통째로 미구현 목록에 넣지 않는다.

**새 P3 #20 구형 WaveMMA와 후속 pinned SM6.10 preview matrix 모두 완료했다(2026-10-03).** 서로 다른 actual public SDK/producer 계약을 유지하며 runtime-05 양 ABI public runtime behavioral, runtime-06 양 ABI11-case numerical/converted-weight shader consumer, runtime-07 양 ABI strict query38/77/78·OPTIONS9 및 successor conversion readback을 결합했다. [정확한 계약·named 증거·한계](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#wavemmalinear-algebra-최종-positive-cap-proof)를 따른다. 이는 최신 단일 cut full-suite 재실행이 아니다. Mandatory atomic 통합 acceptance가 남아 ordinary HighestShaderModel은6.5이며 pinned preview matrix를 전체 SM6.10 conformance·다른 GPU/OS·게임/성능·ARM64/FEX/no-Rosetta 완료로 확대하지 않는다.

**새 P3 #22 FP64도 완료했다(2026-10-03).** Binary64 정수 emulation으로 `DoublePrecisionFloatShaderOps=TRUE`를 보고하며, paired runtime-04에서 실제 DXIL과 native FXC DXBC5.0/5.1의 compute·VS/PS·4-lane wave8 case가 x64/WoW64 모두 API+Shader Validation으로 통과했다. [정확한 계약·증거·한계](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#fp64-완료와-focused-proof)를 따른다. 이 완료로 공개 SM/FL 상한이나 다른 P3·D3D11·게임·성능 지원을 상향 선언하지 않는다.

**새 P3 #21 Writable MSAA도 완료했다(2026-10-03).** OPTIONS14 `WriteableMSAATexturesSupported=TRUE`를 보고하며 paired runtime-06의 x64/WoW64 각각80-case public consumer가 API+Shader Validation으로 통과했다. Sample별 UAV write/load·typed alias/SRV·copy/raster/MIN/MAX resolve·clear와 typeless half의 raw-bit 보존을 GPU emulation으로 연결했다. [정확한 계약·증거·한계](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#writable-msaa-완료와-focused-proof)를 따르며 native writable-MSAA·D3D11·reserved MSAA·전체 SM/FL·게임/성능 지원으로 확대하지 않는다.

- Metal 4 recovery **118/118**은 x64 60 + WoW64 58, **새 runtime-02 실행 44 + 유지한 runtime-01 실행 74**의 mixed-cut 증거다. 116개는 API/Shader Validation, 2개 atomic-live는 API-only다. 최신 단일 snapshot에서 118개를 모두 재실행한 결과가 아니다.
- 이후 리뷰 수정은 별도 focused evidence다. 전체 D3D12/DXR conformance, Windows reference differential, 실제 게임, 성능 향상, 다른 GPU/OS, 물리 compositor/HDR scanout·다중 출력은 이 숫자로 증명되지 않는다.
- ARM64/plain ARM64EC production DLL 6개, 실제 ARM64 Wine SDK/`winemetal.so` compiler·GPU probe **11/11**, 새 paired ARM64X D3D12/DXGI/WineMetal **3개 자동 빌드**를 확인했다. ARM64X 결과는 **54 commands / 0 failure**, owned 입력 **503개**의 빌드 전후 hash가 일치한 단일 source cut이다. 이것은 G3의 DLL 빌드 증거이지 native Wine/FEX guest 실행 증거가 아니다.
- 격리 local Wine `c2f86161…`의 native runtime과 현재 full plain ARM64 owned install을 **schema-2 package로 완료**했다. `build/local/arm64-owned-package-cut-20261002/package-result.json`은 **6 commands / 0 failure**, normal owned wrapper는 `wine-11.17`이다. 실제 loader/server의 `proc_translated=0`은 관찰했지만 shared page `0x7ffe0000` low-VA mapping 실패로 PE guest는 시작하지 못했다(`runtime_executed=true`, `guest_executed=false`, `strict_no_rosetta_proven=false`). 패키징까지만 완료하고 실행 계층은 별도 작업이며, G5–G8/FEX와 Rosetta-free guest 완료를 선언하지 않는다.
- `ShaderCacheSession`/`ShaderCacheControl`은 application memory/disk session으로 구현됐다. 자동 driver cache나 native Metal archive는 아니다. R32 manual exposure는 GPU 변환으로 지원하며 CPU fixup·tonemapping을 추가하지 않는다. optional x64 FSR frontend와 NGX는 같은 retained MetalFX backend를 사용한다. 정확한 지원 범위·GPU/cache 증거는 [현재 상태](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md), [Core contracts](graphics-translator/D3D12-CORE-CONTRACTS.ko.md), [Temporal/NGX contracts](graphics-translator/NGX-CONTRACTS.ko.md)를 따른다.
- 과거 `build/` 삭제로 유실된 P3 등의 실행은 관찰 기록으로만 남는다. 문서의 옛 절대 경로나 “보존” 표기를 현재 파일 존재로 해석하지 않는다. 현재 남아 있는 recovery/review/native-plugin 증거와 구분한다.

## 소스 구조

| 경로 | 역할 |
|---|---|
| `src/d3d12/` | Device·resource·PSO·command list/queue·query·DXR·temporal |
| `src/airconv/` | 자체 DXIL/DXBC compiler, AIR/metallib 출력 |
| `src/winemetal/` | Windows/native Metal ABI와 Metal 4 backend |
| `src/dxmt/`, `src/dxgi/` | 공유 backend·동기화·adapter·presentation |
| `src/nvapi/`, `src/nvngx/` | 공용 backend를 사용하는 연동 facade |
| `src/d3d10/`, `src/d3d11/` | 유지하는 업스트림 frontend |
| `graphics-translator/tests/` | 공개 D3D12 fixture·CPU probe·frozen DXIL/DXBC |
| `scripts/` | Build/staging/diagnostics·격리 runtime·GPU evidence runner |
| `include/native/`, `external/`, `libs/` | Vendored 의존성; Git submodule이 아님 |
| `graphics-translator/patches/`, `graphics-translator/upstream.lock.json`, `docs/provenance/` | Frozen import baseline과 출처 기록 |

Import baseline은 DXMT `7c8dee1c2d73415301ceb7d1fa810861cef4cd67` + owned patch 18개다.
현재 소스를 `scripts/build-owned-graphics.py`로 재생성하거나 수정 내용을 역사적 patch로 역수출하지 않는다.
이 스크립트는 frozen baseline 재현 전용이다. `build/legacy`·`build/retired-evidence`의 과거 경로도 현재 빌드 입력이 아니다.

## 빌드

### x64/WoW64 중간 baseline

확인한 로컬 도구는 macOS Xcode/Metal tools, Python 3, Meson **1.10.0**, Ninja **1.13 계열**,
llvm-mingw clang **22.1.8**, native LLVM **15.0.7** static libraries와 설치된 Wine SDK다.
Meson의 선언상 최소 버전은 1.3.0이며 검증에 사용한 버전과 구분한다.
아래 x86_64 native Wine 경로는 Apple Silicon에서 Rosetta가 필요하다.

D3D12 일반 shader의 AIR 타깃은 **Metal 3.2**다. macOS 27 최소 환경에서
`DeviceMemoryBarrier()`를 실행 동기화 없는 device/texture fence로 낮춘다.
Atomic64 특수 variant의 Metal 4.1 타깃과 Metal 4 backend API 사용은 별개이며,
D3D11의 upstream 타깃은 변경하지 않는다. [양 ABI 실제 fence·alias proof](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#d3d12-shader-타깃과-non-execution-uav-fence)를 따른다.

로컬 입력은 다음과 같이 두며 source 배포에는 포함하지 않는다.

| 입력 | 예시 위치 |
|---|---|
| x86_64 native LLVM 15.0.7 | `toolchains/llvm-15` |
| Meson/Ninja | `toolchains/tools/bin` |
| 설치된 Wine SDK | `toolchains/wine-sdk` |
| staging 원본 Wine runtime | `toolchains/wine-runtime` |

현재 Meson은 `native_llvm_path`의 상대·절대 경로를 모두 처리한다. 아래는 저장소 내부 상대 경로 관례다.
LLVM과 Wine native libraries의 아키텍처는 선택한 native target과 일치해야 한다.
LLVM 구축과 clangd 설정은 [Development](docs/DEVELOPMENT.md)에 있다.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
export PATH="$LLVM_MINGW:$PWD/toolchains/tools/bin:$PATH"
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++ OBJCXX_FOR_BUILD=/usr/bin/clang++

meson setup build/local/build . --cross-file build-win64.txt \
  --prefix "$PWD/build/local/install" --buildtype debugoptimized \
  -Dwine_builtin_dll=true -Denable_d3d12=true \
  -Denable_nvapi=true -Denable_nvngx=true \
  -Dnative_llvm_path=toolchains/llvm-15 \
  -Dwine_install_path=toolchains/wine-sdk
ninja -C build/local/build -j8
meson install -C build/local/build --no-rebuild
```

처음에는 새 build directory를 configure한다. 이후에는 같은 설정에서 Ninja와 install만 다시 실행한다.
WoW64는 `build-win32.txt`의 별도 i686 build/install을 만들고 staging의
`--dxmt-install32`로 추가한다. PE32만으로 native Unix library를 대체하지 않는다.
`d3d12.dll`과 같은 build generation의 **owned `d3d12core.dll`도 필수 설치 항목**이다.
Core는 Wine Unix 경계가 없는 일반 PE이며 staging은 `d3d12core=n`으로 실제 SDK 경로를
선택한다. 나머지 graphics DLL의 builtin 정책은 유지한다. 별도 Core 활성화 옵션은 없다.
SDK618은 선택한 게임 경로용 owned package identity이며 Microsoft SDK618 전체 conformance가 아니다.
SDK 경로 배포·공개 API 검증은 [Runtime](graphics-translator/RUNTIME.ko.md)과
[Core618 acceptance](graphics-translator/SMOKE-TEST.ko.md#owned-core618-versionpath수명-acceptance)를 따른다.

Optional **x64/WoW64 FSR**은 각 Meson 설정에 `-Denable_fsr=true`를 추가하고 staging에
`--with-fsr`를 전달한다(`--with-nvngx`와 함께 사용 가능). 공개 AMD **4.0.3 API**의
SR facade/owned MetalFX backend와 source-owned `amd_fidelityfx_framegeneration_dx12.dll`을 함께 빌드·stage한다.
SR은 masks, high-resolution/jittered motion vectors, input/output subrect, RCAS sharpening,
linear/sRGB/PQ transfer, manual/auto exposure와 dynamic render/output을 연결한다. FSR 출력이
장치의 temporal scale 상한을 넘으면 종횡비를 유지한 capped 영역을 중앙에 놓고 나머지는 opaque black으로 채운다.
FG는 MetalFX FrameInterpolator의 Prepare GPU snapshot→Generate와 MIT GPUOpen SDK 기반
swapchain/UI/pacing proxy를 사용한다. 원래 vendor DLL·D3DMetal fallback이나 AMD FSR 모델 실행이 아니다.
NGX Exposure.Scale·MV.Offset은 유효한 비기본 입력을 받아 무시하는 호환 계약이며 수치 효과를 구현한 것은 아니다.
FG ResizeBuffers1의 per-buffer queue 선택·수명·rollback은 GPU/실제 drawable로 검증했다.
SR debug-view dispatch, FG의 여러 generated output·일부 flags는 명시적 제한이다.
M5 Pro의 public native Metal4FX16→48에서도 Conv3x3 invalid stores가 관찰돼 작은 입력의
portable 지원은 보장하지 않는다. 이 platform 경계를 validation suppression이나 크기 guard로 숨기지 않는다.
구현 계약과 실제 검증 범위는 [현재 상태](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료),
실행 절차는 [Smoke test](graphics-translator/SMOKE-TEST.ko.md#전체-srpublic-fg와-shader-cache-전용-recipe)를 따른다.
Private alpha와 public NGX alpha/SR·FSR SR/FG·NGX FG는 x64/WoW64 지정 consumer를 실제 실행했다.
FSR proxy와 NGX caller-owned Present는 각각 native drawable 순서·픽셀을 확인했으며 서로의 증거를 대신하지 않는다.
Public RR은 양 ABI 독립 native 수치와 일치하지만, Wine 밖 ARM64/x86_64 MetalFX에서도
내부 shader validation 오류가 재현되어 완료 판정은 보류한다.
최신 증거는 [P4 실행 이력](graphics-translator/IMPLEMENTATION-HISTORY.ko.md#새-p4-게임-경로-통합--2026-10-04)에 있다.
지정 fixture 성공은 OS의 정확한2× 표시 cadence·게임 성능을 보장하지 않는다.
세부 계약은 [NGX frontend](graphics-translator/D3D12-NGX-FRONTEND.ko.md)와
[Integration](graphics-translator/NGX-INTEGRATION.ko.md)에 기록한다.

### ARM64-native 실행 구성

Native Mach-O 아키텍처, Windows PE ABI, guest instruction set은 독립된 축이다.
`build-arm64.txt`, `build-arm64ec-only.txt`, `build-native-arm64.txt`의 목적과
실제 실행 가능 범위를 [ARM64/FEX 지침](docs/ARM64-FEX-IMPLEMENTATION.md)에서 확인한다.
ARM64EC DLL의 raw PE machine 값이나 ARM64 object 생성만으로 Rosetta-free 실행을 판정하지 않는다.
ARM64X는 별도 single-EC cross config가 아니라 canonical ARM64/EC Meson 입력을 함께 빌드하는
production helper를 사용한다. Foundation helper도 이 paired 경로에 위임한다.

```sh
python3 scripts/build-owned-arm64x.py \
  --output build/local/arm64x-paired-reproduce01 \
  --wine-source build/local/arm64-sdk-plugin-20261002/wine-source --jobs 8
python3 scripts/test-owned-arm64x.py --build-root build/local/arm64x-paired-reproduce01
```

위 output은 새 목적지여야 한다. 보존한 실제 실행은 `build/local/arm64x-paired-production-20261002/`의
`result.json`, `source-cut.json`, logs/maps를 따른다. native Wine packaging과 실행 계층의 남은 gate는
[ARM64/FEX](docs/ARM64-FEX-IMPLEMENTATION.md#current-gate-disposition--2026-10-02)에 분리한다.
Native runtime builder는 `scripts/build-owned-wine-arm64-runtime.py`, 완전 owned install/staging은
`scripts/package-owned-wine-arm64.py`를 사용한다. [재현 절차](docs/ARM64-FEX-IMPLEMENTATION.md#native-wine-build-and-package-recipe)와
[package/host 증거](docs/ARM64-FEX-IMPLEMENTATION.md#native-wine-host-evidence-and-execution-boundary)를 따른다.

## 검증

### CPU/tooling

```sh
python3 scripts/test-owned-graphics-build.py
python3 scripts/test-owned-graphics-staging.py
python3 scripts/test-owned-graphics-diagnostics.py
python3 scripts/test-owned-ngx-frontend.py
```

NGX frontend 검증은 cross compiler로 현재 `nvngx.dll`을 다시 만들고 PE export·MSVC parameter ABI를 확인한다.
GPU 검증은 아니다. 일반 lint target이나 Meson regression suite는 없다.
`-Denable_tests=true`의 업스트림 DX11 샘플 및 수동 `*.test.cpp` probe와 이 회귀 경로를 혼동하지 않는다.

### 격리 GPU 실행 예시

아래 `build/local/runtime`과 `build/local/cache-proof`는 **존재하지 않는 새 목적지**일 때만 사용한다.
다음 실행에는 새 이름을 선택한다. 기존 게임·설치 runtime·live Wine prefix에는 DLL을 복사하지 않는다.

```sh
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --wine-source toolchains/wine-runtime \
  --wine-dest build/local/runtime --with-nvapi --with-nvngx
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -municode -static \
  -Iinclude/native/directx graphics-translator/tests/owned-d3d12-pipeline-cache.cpp \
  -ld3d12 -ldxgi -ldxguid -lole32 -o build/local/pipeline-cache.exe
python3 scripts/test-owned-graphics.py --runtime build/local/runtime \
  --suite pipeline-cache --fixture build/local/pipeline-cache.exe \
  --out build/local/cache-proof --validation
```

Fixture마다 entry point(`main`/`wmain`), header revision, link library, frozen shader 입력이 다르다.
다른 suite에 위 컴파일 명령을 그대로 적용하지 말고 [Smoke test](graphics-translator/SMOKE-TEST.ko.md)를 따른다.
`--validation`은 Metal API/Shader Validation을 켠다. 동시 reader 관찰용 `atomic-copy-live`의
`--api-validation` 예외와 atomic64 snapshot 미증명 범위도 그 문서에 분리했다.

실행 전에 여유 공간을 확인하고, 실행 후 해당 Wine 프로세스가 멈춘 것을 확인한 뒤 disposable prefix를 제거한다.
Manifest·로그·`result.json`·capture는 보존한다. 현재 build/install, 최신 staged runtime,
비교용 known-good runtime 외의 사본은 검증 목적이 끝나면 runtime payload만 정리한다.

## 라이선스와 기여

루트 [LICENSE](LICENSE)와 각 소스의 원저작자 copyright를 유지한다.
`external/nvapi/License.txt`, `external/nvngx/LICENSE.txt`,
`include/native/directx/COPYING.MinGW-w64.txt`, `src/util/dxvk.LICENSE`는 각각의 vendored 라이선스다.
[CONTRIBUTING.md](CONTRIBUTING.md)는 semantic commit과 실명 sign-off를 요구하며
AI/LLM 작성·공동 작성 contribution을 받지 않는다고 명시한다. 로컬 변경을 upstream PR에 제출할 수 있다고 가정하지 않는다.
