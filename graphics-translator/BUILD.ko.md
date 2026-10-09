# 빌드 경로 선택과 frozen import 재현

## 현재 소스 빌드

**현재 개발의 기준은 저장소 루트 소스이며, 빌드 명령의 기준은 [README.md](../README.md#빌드)다.** LLVM/Wine SDK 설정은 [DEVELOPMENT.md](../doc/DEVELOPMENT.md), 설치 결과의 격리 스테이징은 [RUNTIME.ko.md](RUNTIME.ko.md), fixture 실행은 [SMOKE-TEST.ko.md](SMOKE-TEST.ko.md)를 사용한다.

`src/` 변경을 `graphics-translator/patches/`로 역수출하지 않는다. `upstream.lock.json`, frozen patches, `doc/provenance`는 import 출처 기록이며 현재 소스에 맞춰 갱신할 대상이 아니다. `scripts/build-owned-graphics.py`는 **현재 루트 소스를 빌드하지 않는다**. 이 스크립트로 성공한 baseline을 최신 Metal 4·DXR·ARM64 Wine/FEX 구현의 빌드/검증 결과로 보고하면 안 된다.

현재 기능·검증 경계의 기준은 [CORE-IMPLEMENTATION-STATUS.ko.md](CORE-IMPLEMENTATION-STATUS.ko.md)다. 아래는 frozen provenance를 다시 조사해야 할 때만 사용하는 별도 절차다.

## 역사적 frozen baseline

2026-09-19~21 import baseline은 DXMT `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`와 고정 submodule commit, ordered owned patch 18개로 구성됐다. 정확한 적용 순서·파일 hash는 [upstream.lock.json](upstream.lock.json)이 authority다. 숫자 파일명 정렬이 아니라 lock의 `patches.series` 배열 순서로 적용한다. `0004`는 2026-09-19 candidate의 기존 0001/0002/0003 외 D3D12 residual snapshot이며, `0015`는 자체 DXIL compiler, `0018`은 당시 PSO cached-blob 경로다. 이 이름만으로 후속 루트 소스의 기능을 설명하지 않는다.

당시 성공한 입력:

| 입력 | 역사적 기록 |
|---|---|
| Meson / Ninja | 1.10.0 / 1.13 계열 |
| Windows compiler | llvm-mingw clang 22.1.8, `x86_64-w64-windows-gnu` |
| Native LLVM | 15.0.7, commit `8dfdcc7b7bf66834a761bd8de445840ef68e4d1a` |
| Native C/C++ | `/usr/bin/clang`, `/usr/bin/clang++` |
| Wine SDK | 당시 `build/wine-crossover/host`의 설치 내용 |
| Cross file | frozen source의 `build-win64.txt` |

**당시 prefix가 지금 존재하거나 재사용 가능하다는 뜻이 아니다.** 과거 `build/owned-*`, `build/legacy`, `build/retired-evidence` 경로는 provenance 식별자다. 이후 `build/` 삭제로 이전 runtime/log가 유실됐으며, retained import 기록은 `doc/provenance`와 [검증·증거 가용성](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)을 기준으로 구분한다. 단순히 같은 LLVM 버전을 새로 빌드한 prefix가 아래 content hash gate를 통과한다고 보장하지 않는다.

## frozen 재현 입력 검증

스크립트는 upstream/submodule을 새 checkout으로 만들고 ordered patch를 `git apply --index`로 적용한다. root의 dirty 소스나 설치된 게임/runtime을 입력으로 쓰지 않는다.

- Patch SHA-256/size를 검증하고 lock에 없는 `.patch`도 거부한다. 실패를 피하려고 patch나 lock을 수정하지 않는다.
- llvm-mingw wrapper/binary, LLVM version header와 핵심 static archives, Wine SDK의 `winebuild`·import library·native module을 content hash로 검사한다. 경로는 달라도 되지만 내용은 lock과 일치해야 한다.
- Meson/Ninja의 실제 실행 파일 hash/version 및 native compiler identity는 manifest에 기록한다.
- 선택적 `--source-ref`는 locked commit을 포함하는 **clean Git checkout**의 clone source다. 이 독립 저장소의 현재 루트 소스를 내보내는 옵션이 아니다. 초기화된 submodule도 clean/locked commit일 때만 clone source로 재사용하며 나머지는 lock의 URL에서 받는다.
- `--source-ref` 생략 시 locked upstream URL에서 clone한다. 입력 checkout의 reset/clean/checkout 변경은 하지 않으며 patch 적용은 새 `<root>/source`에서만 한다.

## frozen 재현 실행

모든 예시는 저장소 루트에서 실행한다. 아래 로컬 prefix는 사용자가 **lock과 같은 내용을 별도로 확보한 경우**의 경로 예시이지 제공되는 toolchain이 아니다. `build/frozen-repro/run-001`은 존재하지 않아야 한다.

```sh
python3 scripts/build-owned-graphics.py \
  --root build/frozen-repro/run-001 \
  --native-llvm toolchains/frozen-llvm-15 \
  --wine-sdk toolchains/frozen-wine-sdk \
  --llvm-mingw-bin /opt/llvm-mingw-20260616-ucrt-macos-universal/bin \
  --meson toolchains/tools/bin/meson \
  --ninja toolchains/tools/bin/ninja
```

locked LLVM 설치 내용이 없을 때만 명시적 bootstrap을 선택한다. `--native-llvm` 대신 `--bootstrap-llvm`을 쓰고, 필요한 경우 clean pinned checkout을 `--llvm-source-ref`로 제공한다.

```sh
python3 scripts/build-owned-graphics.py \
  --root build/frozen-repro/run-bootstrap-001 \
  --bootstrap-llvm \
  --wine-sdk toolchains/frozen-wine-sdk \
  --llvm-mingw-bin /opt/llvm-mingw-20260616-ucrt-macos-universal/bin \
  --meson toolchains/tools/bin/meson \
  --ninja toolchains/tools/bin/ninja
```

bootstrap은 lock의 LLVM commit을 detached checkout하고 `<root>/toolchain/llvm`에 설치한다. 역사적 설정은 Release/static, EH/RTTI off, terminfo/zlib on, zstd off, empty projects/targets다. 결과도 lock의 LLVM file hashes를 만족해야 translator 빌드로 넘어간다. 일반 현재 소스 재빌드에는 이 bootstrap을 쓰지 않는다.

생성 구조:

```text
<root>/
  source/                       locked upstream + frozen patch series
  build/                        frozen Meson build tree
  install/                      이 재현만의 설치 결과
  commands.log                  command와 출력
  owned-graphics-build.json     provenance와 artifact inventory
```

lock의 Meson 설정은 `debugoptimized`, D3D12/NVAPI/NVNGX enable, validated LLVM/Wine prefix다. `CC_FOR_BUILD=/usr/bin/clang`, `CXX_FOR_BUILD=/usr/bin/clang++`를 쓰며 지정 llvm-mingw/Meson/Ninja directory를 PATH 앞에 둔다. `meson install` 뒤 이 재현이 만든 `winemetal.so`의 build-tree absolute `LC_RPATH`를 제거하고 absolute dependency/rpath·잘못된 architecture slice·외부 compiler dependency가 남으면 실패한다.

자체 compiler는 frozen `src/airconv` static library를 통해 `x86_64-unix/winemetal.so`에 링크된다. 별도 Apple SDK 다운로드, compiler wrapper dylib, license tree 복사는 없고 제거된 `libdxmt-dxilconv`/`libmetalirconverter`/D3DMetal/`libd3dshared` 의존은 거부한다. 현재 compiler 지원 범위는 이 frozen frontend 설명이 아니라 CORE에서 확인한다.

## fresh-root와 resume

기본 실행은 이미 존재하는 root를 자동 삭제하거나 덮어쓰지 않는다. 중단된 build만 **같은 인자와 `--resume`**으로 이어갈 수 있다. 이전 manifest와 다음 입력이 모두 일치해야 한다.

- Lock hash, upstream/submodule pin, ordered patch 이름/hash/size.
- Meson build options, toolchain/SDK content hashes, Meson/Ninja/native compiler identity.
- Patched source fingerprint와 기존 build/install 계약.

Source checkout/patch 단계 실패로 staged fingerprint가 완성되지 않은 root는 resume 대상이 아니다. Manifest 생성 전 LLVM bootstrap 실패에는 manifest가 없을 수도 있다. 실패 기록을 보존하고 새 root로 재시작한다. 경로를 덮어쓰거나 lock 검증을 약화해 이어가는 절차는 없다.

## build manifest와 증명의 한계

`owned-graphics-build.json`은 lock/upstream/submodule/patch provenance, patched binary diff fingerprint, tools/SDK hashes, 실제 argv/cwd/exit/duration, per-file size/SHA-256/symlink target, native compiler provenance와 normalized module dependency, stable artifact inventory hash를 기록한다. 최소 sanity gate는 `d3d12.dll`, `dxgi.dll`, `winemetal.so`이며 optional artifacts도 inventory에 남는다.

`created_utc`/`completed_utc`와 절대 경로 자체는 bit-reproducibility 주장이 아니다. Artifact 일치는 per-file hash로 비교한다. 성공한 frozen build는 provenance/설치 산출물의 증거일 뿐 GPU·Present·게임·현재 전체 규격 지원의 증거가 아니다. 이 스크립트는 Wine launcher, 게임 디렉터리, prefix에 stage/install하지 않고 GPU도 실행하지 않는다.

Frozen builder의 CPU 회귀 검사는 `python3 scripts/test-owned-graphics-build.py`다. 임시 local Git repo로 pin/ordered patch 적용, dirty/unlisted/hash drift 거부, root overwrite/resume 제한, content fingerprint, Meson argv, module dependency/architecture/rpath gate를 검사한다. 이것을 현재 translator의 shader/GPU conformance 검사로 세지 않는다.
