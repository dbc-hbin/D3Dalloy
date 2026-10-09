# Owned graphics runtime 스테이징 계약

현재 루트 소스의 빌드 기준은 [README.md](../README.md#빌드)다. 이 문서는 **같은 빌드 세대의 graphics module 전체**를 caller가 제공한 Wine의 새 사본에 stage하는 계약을 설명한다. 게임 설치·live prefix 변경은 포함하지 않는다. 기능 지원과 실제 실행 경계는 [CORE-IMPLEMENTATION-STATUS.ko.md](CORE-IMPLEMENTATION-STATUS.ko.md), fixture 실행은 [SMOKE-TEST.ko.md](SMOKE-TEST.ko.md)를 참조한다.

## 아키텍처와 schema 2

`stage-owned-graphics.py stage`는 새 `owned-graphics-runtime.json`에 **schema 2**와 `architecture`를 기록한다. Native Mach-O architecture, Windows DLL ABI, 실행할 guest ISA는 서로 다른 정보다.

| `--native-arch` | `--windows-abi` | Windows runtime directory | Unix runtime directory |
|---|---|---|---|
| `x86_64` | `x64` | `lib/wine/x86_64-windows` | `lib/wine/x86_64-unix` |
| `arm64` | `arm64` | `lib/wine/aarch64-windows` | `lib/wine/aarch64-unix` |
| `arm64` | `arm64ec` | `lib/wine/arm64ec-windows` | `lib/wine/aarch64-unix` |
| `arm64` | `arm64x` | `lib/wine/aarch64-windows` | `lib/wine/aarch64-unix` |

Defaults are `x86_64`/`x64`. 그 외 조합은 거부한다. 표는 **staging layout 허용값**이지 ARM64EC/ARM64X 또는 FEX 실행 완료 표가 아니다. Windows install directory는 표에서 `lib/wine/`을 뺀 위치이고 native install directory도 같은 방식이다.

- PE machine과 최종 CHPE metadata/code ranges, ARM64X dynamic relocations를 검사한다. 중간 ARM64EC/ARM64X label만 붙인 파일은 허용하지 않는다.
- Wine의 `ntdll.dll` ABI, native launcher/`ntdll.so`/`wineserver`의 architecture를 검사한다. x64 packaged Wine의 ARM64 IPC `wineserver`는 명시적 예외다.
- Generic GPU runner의 `--guest-arch {x86,x64,arm64}`는 ISA assertion이지 FEX 활성화 옵션이 아니다. Native FEX의 선정 ABI/smoke 실행은 [Wine-darwin](../../Wine-darwin/README.md)의 `scripts/run-owned-wine-arm64.py --dxmt-root <DXMT 레포>` 경로로 검증한다. Generic runner의 FEX 거부를 native 실행 전체 미구현으로 해석하지 않으며, 10월7일 direct public frontend 실행도 generic runner acceptance와 구분한다.
- **Schema 1은 legacy x86_64/x64 manifest를 읽는 호환 경로만 남아 있다.** 새 stage는 schema 1을 생성하지 않는다. 기존 manifest를 손으로 schema 2로 relabel하지 말고 실제 binaries에서 새 stage를 만든다.

ARM64 compiler/bridge·Wine SDK/plugin의 관측과 native Wine/FEX 최종 acceptance는 [ARM64-FEX-IMPLEMENTATION.md](../docs/ARM64-FEX-IMPLEMENTATION.md)에서 분리한다. Schema 2나 ARM64 launch 명령만으로 `strict_no_rosetta_proven`을 주장하지 않는다.

## 빌드 입력과 함께 교체할 모듈

`enable_d3d12`는 기본 false이므로 README의 현재 Meson recipe가 명시적으로 켠다. `wine_builtin_dll=true` install이 필요하며 Main/DXGI/WineMetal 등 Wine builtin PE에는 `Wine builtin DLL` marker가 있어야 한다. **Core만 ordinary owned PE**이며 Unix-call boundary가 없어 builtin postprocess를 하지 않는다.

| Install 파일(선택 ABI/native directory 아래) | 역할 |
|---|---|
| `d3d12.dll` | Owned D3D12, `owned-graphics-d3d12` marker 필요 |
| `d3d12core.dll` | 필수 same-generation owned Agility compatibility Core, actual DATA SDK version618 |
| `dxgi.dll` | DXGI |
| `d3d11.dll`, `d3d10core.dll` | 기존 공용 구현/의존성 |
| `winemetal.dll` | PE Unix-call thunk |
| `winemetal.so` | 같은 세대의 native Metal bridge·자체 compiler |

`winemetal.dll`은 `__wine_init_unix_call()`로 같은 basename의 Unix plugin을 호출한다. `winemetal.so`가 요구하는 Wine `winemac.so`/`ntdll.so`는 선택 native architecture와 재배치 가능한 dependency 계약을 만족해야 한다. AIR compiler는 `winemetal.so`에 static link된다. 빌드 트리의 `airconv` CLI는 별도 stage/install runtime executable이 아니다.

**PE caller, Unix thunk/compiler, native consumer는 같은 공용 ABI header로 함께 재빌드한다.** 2026-09-20 immutable-storage 결과 ABI가 `SM50_COMPILED_BITCODE`에 pool data/size/alignment/relocation을 추가한 것처럼, 단일 DLL이나 plugin 교체는 buffer 크기·call 계약을 깨뜨릴 수 있다. 같은 install 세트 전체를 stage하며 이전 canonical runtime과 혼합하지 않는다.

DXMT install 어디든 제거된 `libdxmt-dxilconv.dylib`, `libmetalirconverter.dylib`, `metal-shader-converter` artifact가 있으면 거부한다. Owned plugin이 외부 compiler/GPTK backend (`dxilconv`, `metalirconverter`, D3DMetal, `libd3dshared`)에 링크해도 거부한다. 별도 SDK dylib/license tree를 복사하는 단계는 없다.

## 스테이징

Wine SDK/runtime 생산은 별도 [Wine-darwin](../../Wine-darwin/README.md)의 책임이다. DXMT는 완성된 runtime을 입력받는다. Composition frontend가 필요하면 Wine-darwin의 `scripts/build-display.py --dxmt-root <DXMT 레포>`로 만든 `wine-build`와 `wine-source`를 stage의 `--wine-display-build`·`--wine-display-source`에 각각 전달한다. Pin·소스/ABI header hash·PE builtin/exports·freshness 검사 후 `dcomp.dll`만 교체하며 native Wine overlays는 건드리지 않는다.

Core는 신규 optional 기능/CLI toggle이 아니라 `enable_d3d12` 설치 세트의 필수 모듈이다. Stage는 양 ABI 각각 `D3D12SDKVersion` **실제 DATA=618**, concrete `D3D12GetInterface`/owned bridge exports, owned marker와 `OwnedD3D12CoreGeneration` DATA char[33]을 검사한다. Core는 Wine DOS-stub의 정확한 32-byte builtin/placeholder signature를 거부하는 ordinary PE여야 한다. 같은 ABI의 Main/Core generation이 일치해야 한다. 새 schema2 manifest의 필수 `agility_sdk` record는 ownership/version, `delivery=native-only`와 ABI별 Core path/hash/generation을 저장한다. Verify도 이를 재검사한다. Core 도입 전 immutable schema1/2 manifest 및 실제 retained builtin-Core manifest의 read-only 검증은 유지하지만 새 stage에서 Core를 생략하거나 builtin으로 배포할 수는 없다.

일반 EXE의 `D3D12SDKVersion`/`D3D12SDKPath` exports와 base `SetSDKVersion`은 [공개 redist 우선 규칙](https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html)을 따른다. 유효한 요청 version이 실제 설치된 Core version 이하이면 app-local 파일이 없어도 설치된 Core를 선택한다. 현재 SDK617/618 요청은 설치된 owned618로 실행되며, 더 높은 요청만 SDK path에서 정확히 일치하는 owned Core를 찾는다. 판정은 고정 호환 범위가 아니라 검증된 Core bridge의 실제 version을 사용한다.

SDK1 `CreateDeviceFactory`의 명시적 nonzero version/path 선택은 이 global 우선 정책과 별개다. SDK path는 cwd가 아닌 EXE directory 기준이며 요청한 파일의 ABI·version·generation·전체 image와 로드된 실제 경로를 설치된 owned Core에 대조한다. 현재 이 SxS 경로는 owned618만 지원하며 foreign Microsoft/GPTK Core 또는 다른 version/generation/ABI로 fallback하지 않는다. SDK618은 실제 Godot1.618.5 요청을 위한 owned compatibility identity이지 전체 SDK618 conformance가 아니다. SDK1 factory는 ordinary mode에서 동작하지만 base SetSDKVersion은 OS Developer Mode가 필요하다. [SMOKE의 paired consumer](SMOKE-TEST.ko.md#owned-core618-versionpath수명-acceptance)는 dedicated prefix에서 OS registry state를 저장·설정·복구해 둘을 구분한다.

Wine의 Main module filename은 실제 runtime 파일 대신 prefix의 `system32` facade를 가리킬 수 있다. Selector는 `WINEDLLPATH`를 위치 힌트로만 사용하고, 읽기 전용 data mapping과 `NtAreMappedFilesTheSame`으로 **실제 로드된 Main의 backing file**과 동일한 파일인지 확인한 뒤 그 옆의 Core를 찾는다. Byte-identical prefix copy는 이 identity 검사에서 거부된다. 성공한 위치와 deny-write/delete Main handle을 보존하므로 factory마다 경로를 재탐색하지 않으며, mapping은 imports/TLS/DllMain을 실행하지 않는다. WoW64는 pinned x86_64 Windows/Unix directory의 `i386-windows` sibling도 찾되 동일한 backing-file identity 검사를 통과해야 한다.

아래는 README의 current install을 사용하는 x64 예시다. 저장소 루트에서 실행하며 `build/local/runtime-001`은 **존재하지 않는 새 경로**여야 한다. 재빌드 후에는 새 runtime 이름을 사용한다.

```sh
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install \
  --native-arch x86_64 --windows-abi x64 \
  --wine-source toolchains/wine-runtime \
  --wine-dest build/local/runtime-001
python3 scripts/stage-owned-graphics.py verify \
  --runtime build/local/runtime-001
```

- Wine source와 install은 읽기만 하고 새 사본에 교체한다. Destination은 입력과 포함 관계가 아니어야 한다.
- Destination 또는 부모 symlink, 교체 module 경로의 symlink를 거부한다. 외부 파일을 따라가 덮어쓰지 않는다.
- 기존 packaged `bin/wine.real`은 보존한다. 일반 Wine의 binary `bin/wine`은 **사본 안에서만** `wine.real`로 옮기고 owned wrapper를 생성한다.
- 임시 directory에서 복사·검증을 모두 마친 뒤 최종 destination으로 publish한다. 입력 runtime과 install을 수정하지 않는다.
- Stage는 prefix를 만들거나 초기화하지 않는다. Runner는 매 실행 `--out/prefix`를 새로 만들고 그 prefix의 wineserver만 종료한다. 기존 게임 prefix를 clone하거나 전역 wineserver를 종료하는 절차를 정상 smoke 예제로 사용하지 않는다.

### WoW64

x86 fixture는 `build-win32.txt`로 별도 생성한 **같은 세대** i386 install을 `--dxmt-install32`로 함께 stage한다. x86_64/x64와 arm64/ARM64X native WoW64 구성을 구분하며, ARM64X에는 해당 runtime의 pinned FEX modules와 nonzero guest-window CPU-pointer 계약이 추가된다. `--guest-arch x86`만 붙여 없던 i386 modules를 만들 수 없다. SDK05와 후속 native ABI proof는 [ARM64/FEX](../docs/ARM64-FEX-IMPLEMENTATION.md)를 따른다.

### 선택적 owned NVIDIA extension

현재 recipe가 NVNGX/NVAPI를 빌드하더라도 stage 선택은 별도다. 필요할 때 위 stage 명령에 `--with-nvngx --with-nvapi`를 추가한다.

`nvngx.dll`은 `own-ngxd3d12` marker 및 required D3D12 Init/Create/Evaluate/Release/parameter/export set을 PE export table에서 확인한다. D3D11-only stale frontend는 거부한다. `nvapi64.dll`은 실제 `nvapi_QueryInterface` export가 필요하다. 선택에 따라 manifest의 `optional_modules`, wrapper overrides 및 `DXMT_ENABLE_NVEXT=1`을 기록한다. 두 옵션 모두 없으면 inherited NVEXT를 제거하고 `nvngx`/`nvapi64` 외부 fallback을 비활성화한다. 기능 의미는 [NGX 문서](NGX-INTEGRATION.ko.md)의 범위이며 staging 통과를 NVIDIA DLSS 품질·호환 증거로 세지 않는다.

`--with-nvngx`는 OpenSSL과 `osslsigncode`가 필요하다. 선택한 64-bit/32-bit owned NGX 사본을 임시 owned-publisher 인증서로 실제 Authenticode 서명한 뒤 최종 hash를 기록한다. 개인키는 staging 종료 시 제거하고 공개 DER만 `share/dxmt/ngx-runtime.cer`에 둔다. `wine.inf`의 기본 prefix 설치가 해당 공개 인증서를 **그 Wine prefix의 HKCU Root store에만** 등록한다. macOS 신뢰 저장소나 원본 Wine/게임 파일은 변경하지 않는다. `verify`는 서명된 DLL·공개 인증서·등록 정보의 manifest 정합성을 검사한다. 이는 NVIDIA publisher를 가장하거나 앱의 서명 검증을 끄는 경로가 아니다.

### 선택적 owned FSR SR/FG

README로 FSR-enabled x64 install을 만들고 stage에 `--with-fsr`를 추가한다. SR과 FG DLL을 한 optional group으로 선택하며 stage는 각 builtin marker·required exports/ordinals를 검사한다. Stage/verify 통과는 GPU/presentation proof가 아니다. 현재 architecture·효과·제약은 [CORE SR/FG 계약](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료), 재실행은 [SMOKE recipe](SMOKE-TEST.ko.md#전체-srpublic-fg와-shader-cache-전용-recipe)를 따른다.

## wrapper 로드 정책

Wrapper는 명시적 `WINEPREFIX`가 없으면 실패한다. `WINEDLLPATH`는 manifest가 선택한 stage의 Windows/native module directories로 고정하며 core override는 다음과 같다.

```text
d3d12=b;dxgi=b;d3d11=b;d3d10core=b;winemetal=b;d3d12core=n
```

`b`는 builtin-only다. Core의 `n`은 **native-only ordinary owned PE**로, builtin fallback은 없다. Pinned Wine은 builtin-marked 동일 basename Core를 coalesce하므로, 서로 다른 SDK directory의 actual Core module instance를 유지하려면 이 순수 PE route가 필요하다. Selector는 로드 전 exact owned image/ABI/generation/path를 검사하므로 `n`이 foreign Microsoft/GPTK Core 지원이나 fallback을 뜻하지 않는다. 선택한 optional NVIDIA modules와 FSR SR/FG DLL에는 builtin override를 추가한다. 환경의 `WINEDLLOVERRIDES`는 wrapper 정책으로 교체되므로 caller가 임의 override를 상속해 backend gate를 우회할 수 없다.

Wrapper는 inherited `CX_ACTIVE_GRAPHICS_BACKEND`, `CX_APPLEGPTK_LIBD3DSHARED_PATH`, 알려진 `D3DM_*`/packaging `GPTK_*`, `WINEDLLPATH_PREPEND`, `DYLD_INSERT_LIBRARIES`와 DXVK/Vulkan 주입 경로를 제거한다. `DXMT_CONFIG`/`DXMT_CONFIG_FILE`은 owned 설정이므로 wrapper 자체는 제거하지 않는다(공용 fixture runner는 별도로 inherited `DXMT_*` 전체를 정리한다).

P3 packaged Wine marker가 있는 source의 일반 package-relative fallback library/GStreamer 경로는 유지한다. 원본 runtime에 포함된 D3DMetal.framework/`libd3dshared.dylib`는 다른 consumer의 필요성을 배제할 수 없어 물리적으로 보존하지만 backend 선택은 차단한다. 보존은 로드 승인이나 fallback 허용이 아니다.

## manifest 검증과 rollback

Schema 2는 architecture/layout, source install/runtime paths, loader 정책, optional modules, graphics modules와 `bin/wine`/`wine.real`/`wineserver`·Wine `ntdll` images의 size/hash, PE metadata/native slices, plugin dependency 및 self-owned `native_compiler` record를 저장한다. `verify`는 기록한 hash뿐 아니라 wrapper 내용, markers/exports, 실제 PE/Mach-O 구조, dependency/rpath와 제거된 artifact 재등장을 검사한다.

```sh
python3 scripts/stage-owned-graphics.py verify --runtime build/local/runtime-001
```

이는 **CPU/파일 수준 load contract**다. 실제 module 로드는 runner의 dyld trace로, GPU correctness는 해당 fixture의 exact output으로 별도 확인한다. `verify` 성공은 D3D12 기능 완성·게임·Metal command 제출·Present 성공이 아니다. Rollback은 private runtime을 사용하지 않고 원본 Wine을 선택하는 것으로 끝난다.

## PSO cache 계약

Current PSO cached blobs와 `ID3D12PipelineLibrary1`은 구현된 별개 경로다. 예전 문서의 “CreatePipelineLibrary는 E_NOTIMPL”은 2026-09-21 frozen baseline의 역사적 상태이며 **현재 상태가 아니다**. 최신 완료 범위와 관측은 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태) 및 [P2 library 구현 이력](IMPLEMENTATION-HISTORY.ko.md)에 따른다. `pipeline-cache`와 `pipeline-library` suite를 별도로 선택한다.

Cached blobs는 owned compiler의 stage metallib와 원본 immutable pool/alignment/relocations를 보존하고 새 allocation에 GPU 주소를 다시 적용한다. Native Metal binary archive 자체를 뜻하지 않으며 cache identity는 format, adapter, compiler/runtime/OS-build에 묶인다. 손상/mismatch를 숨겨 cold compile로 바꾸는 성공 판정을 하지 않는다. 이전 native build cache와 새 runtime을 섞은 결과도 현재 same-build 검증과 구분한다. `ID3D12ShaderCacheSession`/control의 application-managed MEMORY/DISK 지원은 이 PSO 경로와 별개이며 [완료 계약·CPU proof](CORE-IMPLEMENTATION-STATUS.ko.md#shadercachesessioncontrol-완료와-focused-proof)를 따른다.

- Key는 shader bytes, serialized root signature, pipeline kind/state와 target 계약에서 만들며 pointer/struct padding을 identity로 쓰지 않는다.
- CachedBlobSizeInBytes가 0이면 explicit blob 없이 자동 RAM/disk cache를 조회하며 miss/disabled일 때 cold compile한다. Explicit nonempty/null, 손상 layout/checksum/metallib, shader/root/stage mismatch 및 invalid relocation은 `E_INVALIDARG`; adapter mismatch는 `D3D12_ERROR_ADAPTER_NOT_FOUND`; format version/native identity mismatch는 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`다. Native identity 조회 실패는 성공으로 위장하지 않는다. Optional automatic-cache의 손상 거부와 한정 cold retry는 explicit CachedPSO 오류를 숨기는 정책이 아니다.
- `GetCachedBlob(nullptr)`는 `E_POINTER`다. Returned blob은 caller-owned이고 unaligned input/lifetime·반환 blob 격리는 cache fixture의 별도 계약이다. 오류를 자동 cold fallback으로 숨기지 말고 caller가 필요한 경우 cache 없이 명시적으로 재생성한다.
