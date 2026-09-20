# Owned graphics translator 재현 빌드

> 이 문서는 import 당시의 frozen baseline 재현 절차를 기록한다. 현재 저장소에서는 루트 소스가 작업 기준이며, 일상 빌드는 [README.md](../README.md)의 Meson 절차를 사용한다. 여기의 `build/owned-*` 경로와 patch 역수출 지침은 역사적 기록이다.

`scripts/build-owned-graphics.py`는 기존 dirty DXMT candidate를 빌드 입력으로 사용하지 않는다. `graphics-translator/upstream.lock.json`에 고정된 upstream commit과 submodule commit을 새 checkout으로 만들고, lock에 적힌 patch series를 순서대로 적용한 뒤 별도 build/install 디렉터리에서 Meson cross build를 수행한다.

현재 lock의 upstream 기준은 DXMT `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`이며 submodule도 commit 단위로 고정한다. patch 파일은 파일명만 고정하는 것이 아니라 SHA-256까지 lock에 들어간다. `graphics-translator/patches`에 lock에 없는 `.patch`가 추가되거나 기존 patch 내용이 바뀌면 build는 source checkout 전에 실패한다. prime이 patch series를 재배치하거나 새 patch를 넣을 때 `upstream.lock.json`의 ordered `patches.series`와 hash를 같이 갱신해야 한다.

현재 series 순서는 `0001-format-capabilities.patch` → `0002-submission-ring.patch` → `0003-identity-and-barriers.patch` → `0004-d3d12-temporal.patch` → `0005-d3d12-ngx.patch` → `0006-candidate-corrections.patch` → `0007-queue-order.patch` → `0009-d3d12-core-contracts.patch` → `0010-temporal-output-order.patch` → `0013-fence-events.patch` → `0012-d3d12-resources.patch` → `0014-d3d12-bundles.patch` → `0011-d3d12-queries.patch` → `0008-ngx-contracts.patch` → `0015-dxil-compiler.patch` → `0016-nvapi-streamline.patch` → `0017-zzz-ngx-profile.patch` → `0018-d3d12-pipeline-cache.patch`다. `0004`는 2026-09-19 frozen candidate snapshot에서 기존 0001/0002/0003을 제외한 D3D12 residual을 그대로 직렬화한 것으로, feature-level gate/query 수정, temporal backend, command recording/submission 연동, shared `src/d3d12/d3d12_interfaces.hpp`를 포함한다. `0005` D3D12 NGX frontend는 이 shared header를 include하므로 0004 뒤에 와야 한다.

## 이미 검증된 toolchain 입력

2026-09-19의 successful owned build에서 사용한 입력은 다음과 같다.

- Meson 1.10.0
- Ninja 1.13.0 계열
- llvm-mingw x86_64 compiler: clang 22.1.8, target `x86_64-w64-windows-gnu`
- native LLVM: 15.0.7 (`llvmorg-15.0.7`, source commit `8dfdcc7b7bf66834a761bd8de445840ef68e4d1a`)
- DXIL native shader compiler: owned source patch series로 airconv에 구현되어 `x86_64-unix/winemetal.so`에 static link된다. 별도 SDK prefix, wrapper dylib, license/notice, CMake build 입력이 없다.
- installed Wine SDK: `build/wine-crossover/host`
- DXMT cross file: pinned source tree의 `build-win64.txt`
- native build compiler: `/usr/bin/clang`, `/usr/bin/clang++`

lock은 llvm-mingw wrapper/binary, native LLVM의 version header와 핵심 static library, Wine SDK의 `winebuild`/x86_64 import library/native Wine module을 content hash로 확인한다. 경로가 달라도 내용이 같으면 사용할 수 있고, 같은 경로라도 내용이 달라지면 거부한다. Meson/Ninja의 실제 executable hash와 version도 build manifest에 기록한다.

native LLVM 15 build는 이미 `build/owned-graphics-20260919/llvm`에 완료되어 있다. 보통은 이 prefix를 그대로 `--native-llvm`로 전달하면 된다. 매 owned build마다 LLVM을 다시 만드는 것은 재현성에 필요하지 않다.

## 기본 빌드

현재 workspace의 검증된 입력을 쓰는 예시는 다음과 같다. output root는 **존재하지 않는 새 경로**여야 한다.

```sh
python3 scripts/build-owned-graphics.py \
  --root build/owned-graphics-repro/run-001 \
  --source-ref build/dlss-translator-20260918/contract/dxmt \
  --native-llvm build/owned-graphics-20260919/llvm \
  --wine-sdk build/wine-crossover/host \
  --llvm-mingw-bin /opt/llvm-mingw-20260616-ucrt-macos-universal/bin \
  --meson build/owned-graphics-20260919/tools/bin/meson \
  --ninja build/owned-graphics-20260919/tools/bin/ninja
```

lock에는 external DXIL compiler 항목이 없다. driver는 Apple Metal Shader Converter SDK를 download/검증/복사하지 않고, `--native-dxil-compiler`/`--metal-shader-converter-sdk` 같은 prefix 입력도 받지 않는다. DXIL compiler는 pinned source와 patch series만으로 Meson build에 포함되므로 build root에는 별도 compiler toolchain directory가 생기지 않는다. 남아 있는 외부 compiler/SDK dependency(`libdxmt-dxilconv.dylib`, `libmetalirconverter.dylib`, D3DMetal/`libd3dshared`)는 install 단계에서 거부된다.

`--source-ref`는 선택 사항이다. 주면 그 local Git checkout을 clone source로만 사용한다. 이 checkout은 untracked file을 포함해 완전히 clean이어야 하고 locked commit을 object database에 포함해야 한다. initialized submodule도 locked commit에 정확히 있고 clean하면 local clone source로 재사용한다. 조건이 맞지 않는 submodule은 lock의 upstream URL에서 받는다.

`--source-ref`를 생략하면 DXMT upstream URL에서 locked commit을 clone한다. 어느 경우에도 입력 checkout을 reset, clean, checkout 변경하지 않는다. 실제 patch 적용은 새 `<root>/source`에서만 한다.

## 생성 디렉터리

fresh build root의 구조는 다음과 같다.

```text
<root>/
  source/                       pinned DXMT + staged owned patch series
  build/                        Meson build tree
  install/                      이 빌드만의 설치 결과
  commands.log                  실행한 build command와 출력
  owned-graphics-build.json     provenance + source/tool/artifact manifest
```

`meson install`의 prefix는 항상 `<root>/install`이다. 스크립트는 Wine runtime, launcher, game directory, Wine prefix에 DLL을 stage하거나 설치하지 않는다. GPU/game 실행도 하지 않는다.

install 직후 driver는 installed `x86_64-unix/winemetal.so`에서 build-tree를 가리키는 absolute `LC_RPATH`를 `install_name_tool -delete_rpath`로 제거하고, 남은 absolute dependency/rpath가 있으면 build를 실패시킨다. 이 정규화 덕분에 install/staged runtime은 APFS copy로 다른 경로에 옮겨도 `@loader_path`로 native module을 찾는다.

현재 build options는 lock에서 다음과 같이 고정된다.

```text
--buildtype debugoptimized
-Denable_d3d12=true
-Denable_nvapi=true
-Denable_nvngx=true
-Dnative_llvm_path=<validated LLVM 15 prefix>
-Dwine_install_path=<validated Wine SDK prefix>
```

llvm-mingw `bin`과 지정 Ninja/Meson directory를 build process의 `PATH` 앞에 두고, native helper build에는 `CC_FOR_BUILD=/usr/bin/clang`, `CXX_FOR_BUILD=/usr/bin/clang++`를 사용한다.

## fresh-root와 `--resume` 계약

기본 실행은 output root가 조금이라도 이미 존재하면 실패한다. 기존 build directory를 자동 삭제하거나 덮어쓰지 않는다.

중단된 build를 이어야 할 때만 같은 인자에 `--resume`을 붙인다.

```sh
python3 scripts/build-owned-graphics.py \
  --root build/owned-graphics-repro/run-001 \
  --native-llvm build/owned-graphics-20260919/llvm \
  --wine-sdk build/wine-crossover/host \
  --llvm-mingw-bin /opt/llvm-mingw-20260616-ucrt-macos-universal/bin \
  --meson build/owned-graphics-20260919/tools/bin/meson \
  --ninja build/owned-graphics-20260919/tools/bin/ninja \
  --resume
```

`--resume`은 단순히 directory 존재 여부만 보지 않는다. 이전 `owned-graphics-build.json`과 현재 입력을 다시 계산해서 다음이 모두 같아야 한다.

- lock 파일 hash와 upstream/submodule pin
- ordered patch 이름/hash/size
- Meson build options
- llvm-mingw/native LLVM/Wine SDK content hashes
- Meson/Ninja/native compiler identity
- native compiler provenance(upstream commit, patch series SHA-256, staged source fingerprint, native LLVM pin)
- jobs와 output root
- `<root>/source` HEAD/submodule commit
- staged owned diff의 SHA-256
- unstaged/untracked source가 없음

하나라도 다르면 resume을 거부한다. source checkout/patch 단계에서 실패하면 `status=failed` manifest와 command log는 남지만 staged source fingerprint가 완성되지 않으므로 `--resume` 대상이 아니다. explicit LLVM bootstrap이 manifest 생성 전에 실패한 경우에는 manifest가 없을 수도 있다. 이런 root는 실패 증거를 보존하거나 사용자가 직접 제거한 뒤 새 root로 다시 시작해야 한다.

## build manifest

`owned-graphics-build.json`은 다음 provenance를 가진다.

- lock path/SHA-256
- upstream commit과 submodule URL/commit
- ordered patch series SHA-256
- patched source의 staged binary diff SHA-256과 name-status
- llvm-mingw compiler hash/version/target
- native LLVM content hashes/version
- Wine SDK content hashes
- Meson/Ninja/native build compiler path/hash/version
- native compiler provenance(upstream commit, patch series SHA-256, staged source fingerprint, native LLVM pin)
- Meson setup/compile/install, rpath 정규화(`install_name_tool -delete_rpath`)를 포함한 실제 argv와 cwd, exit code, duration
- `install/` 아래 regular file의 size/SHA-256과 symlink target
- installed `winemetal.so`(LC_RPATH 없음, external compiler dependency 없음)와 전체 artifact inventory
- 전체 artifact inventory의 stable SHA-256

최소 install sanity check로 `x86_64-windows/d3d12.dll`, `x86_64-windows/dxgi.dll`, `x86_64-unix/winemetal.so` 존재를 요구한다. DXIL compiler는 별도 module이 아니라 `winemetal.so` 안에 있으므로, 추가로 요구하는 compiler artifact는 없고 `winemetal.so`가 외부 compiler/SDK library를 링크하면 build는 실패한다. NVAPI/NVNGX는 lock에서 enable되어 있고 실제 install artifact 전체가 manifest에 기록되므로 결과에서 별도로 확인할 수 있다.

manifest의 `created_utc`/`completed_utc`나 build directory absolute path 자체는 bit-reproducibility 주장이 아니다. artifact hash가 같은지는 manifest의 per-file SHA-256으로 비교한다.

## explicit LLVM 15 bootstrap

검증된 native LLVM prefix가 없을 때만 `--bootstrap-llvm`을 명시한다.

```sh
python3 scripts/build-owned-graphics.py \
  --root build/owned-graphics-repro/run-bootstrap-001 \
  --bootstrap-llvm \
  --llvm-source-ref build/owned-graphics-20260919/llvm-project \
  --wine-sdk build/wine-crossover/host \
  --llvm-mingw-bin /opt/llvm-mingw-20260616-ucrt-macos-universal/bin \
  --meson build/owned-graphics-20260919/tools/bin/meson \
  --ninja build/owned-graphics-20260919/tools/bin/ninja
```

bootstrap source도 lock의 llvm-project commit `8dfdcc7b7bf66834a761bd8de445840ef68e4d1a`로 detached checkout하고, source-ref를 주면 clean/pin 검증 뒤 clone source로만 쓴다. install prefix는 `<root>/toolchain/llvm`이다.

현재 lock의 LLVM CMake 설정은 이미 성공한 15.0.7 build에서 회수했다: Release, static libs, EH/RTTI off, terminfo/zlib on, zstd off, empty `LLVM_ENABLE_PROJECTS`/`LLVM_TARGETS_TO_BUILD`. bootstrap 결과도 lock의 native-LLVM file hashes를 만족해야 다음 DXMT build로 넘어간다.

LLVM bootstrap은 시간이 많이 드는 별도 toolchain build다. 일반 owned translator 재빌드 validation에는 기존 검증 prefix를 사용한다.

## DXIL native compiler (self-owned, in-tree)

`0015-dxil-compiler.patch`는 DXMT LLVM 15/AIR 기반 자체 DXIL frontend다. 기존 SPIR-V 경유 compiler와 Apple Metal Shader Converter는 사용하지 않는다. DXBC/SHEX와 같은 `SM50Initialize`/`SM50Compile` ABI를 사용하며, VS/PS/CS stage signature, root signature/descriptor binding, resource access, arithmetic/wave operation을 AIR로 변환한다. 컴파일별 LLVM module과 stage 상태를 분리해 같은 shader 객체의 반복 PSO 컴파일이 상태를 공유하지 않는다.

정식 patch에는 미완성 DXR state-object/compiler thunk를 넣지 않는다. 자체 RT compiler와 DispatchRays 전체 구현은 아직 완료되지 않았으며, AS 실험의 성공을 DXR 지원으로 취급하지 않는다.

DXIL compiler는 owned source patch series의 일부로 `src/airconv`에 구현되고, `src/airconv/darwin/meson.build`가 만드는 native airconv library를 통해 `winemetal.so`에 link된다. 따라서 build driver에는 external SDK download, prefix CLI, CMake wrapper, license 복사, wrapper/SDK hash 검증 단계가 없다. provenance는 lock의 upstream commit, ordered patch SHA-256, staged source fingerprint, native LLVM 15 pin으로만 구성되며, 완료된 manifest의 `native_dxil_compiler` 항목이 이를 기록한다.

`meson install` 뒤 driver는 이 build가 만든 native module만 정규화한다. installed `winemetal.so`의 build-tree absolute `LC_RPATH`를 `install_name_tool -delete_rpath`로 제거하고, 남은 absolute dependency/rpath 또는 외부 compiler/SDK dependency가 있으면 build를 실패시킨다. SDK dylib을 복사하거나 license/notice를 install tree에 넣는 단계는 없다.

## patch series를 갱신할 때

patch 파일을 추가·이름 변경·내용 수정했다면 최종 series 순서를 먼저 확정하고 SHA-256을 다시 구한다.

```sh
find graphics-translator/patches -maxdepth 1 -type f -name '*.patch' -print | sort
shasum -a 256 graphics-translator/patches/*.patch
```

그 결과를 `graphics-translator/upstream.lock.json`의 `patches.series`에 원하는 적용 순서대로 기록한다. 숫자 파일명 정렬 순서가 아니라 **lock array 순서가 실제 적용 순서**다. `reject_unlisted=true`이므로 directory에 남은 실험 patch가 있으면 build는 거부된다.

lock 갱신 뒤 full build 전에 CPU-only driver 검증을 먼저 실행한다.

```sh
PYTHONWARNINGS=error::ResourceWarning python3 scripts/test-owned-graphics-build.py
python3 -m py_compile scripts/build-owned-graphics.py scripts/test-owned-graphics-build.py
python3 -m json.tool graphics-translator/upstream.lock.json >/dev/null
```

fixture test는 임시 local Git repo만 사용한다. 실제 DXMT full compile, GPU, Wine/game launch는 실행하지 않으며 다음을 검사한다.

- locked upstream HEAD checkout
- ordered patch의 실제 `git apply --index`
- dirty local source-ref 거부
- unlisted/hash-drift patch 거부
- submodule commit pin 검증
- existing output root overwrite 거부
- unstaged source drift에 대한 resume 거부 조건
- content-hash validation
- deterministic Meson setup/compile/install argv 생성
- external compiler/SDK dependency를 가진 installed `winemetal.so` 거부
- absolute dependency/잘못된 slice 거부와 owned module 정규화 command 생성
- input contract가 바뀌면 fingerprint가 바뀌는지

이 build path는 owned source provenance와 CPU/build artifact 재현을 위한 것이다. 설치 결과를 게임에 연결하거나 실제 ZZZ 호환성을 증명하는 단계는 별도 runtime validation이다.
