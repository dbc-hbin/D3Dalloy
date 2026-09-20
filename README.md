# dxmt-dx12

DXMT 기반의 D3D12 → Metal 번역기와 자체 DXIL → AIR/metallib compiler.
검증된 18-patch 소스를 독립 저장소로 분리했다. 게임, 런처, Wine runtime, prefix, LLVM/toolchain 및 빌드 산출물은 배포 소스에 포함하지 않는다.

## 소스 기준과 구조

- **현재 개발의 기준은 이 저장소 루트의 소스다.** `src/`를 직접 수정하고 아래 Meson 명령으로 빌드한다.
- DXMT upstream: `7c8dee1c2d73415301ceb7d1fa810861cef4cd67` + owned patch 18개. 최신 PSO cache patch까지 적용되어 있다.
- `src/d3d12`: D3D12 device/queue/resource/PSO/command/DXR 구현.
- `src/airconv`: 자체 DXIL/DXBC compiler, AIR/metallib writer.
- `src/winemetal`, `src/dxmt`, `src/dxgi`: Metal bridge, 공용 런타임, DXGI. D3D10/11 공용 구현과 NVAPI/NGX도 번역기의 빌드·실행 의존성이므로 유지했다.
- `include/native/directx`, `external/nvapi`: 고정 revision의 vendored dependency. 별도 Git repository/submodule이 아니며 라이선스와 원본 소스를 포함한다.
- `graphics-translator/tests`: CPU/PE/GPU regression fixture와 고정 DXIL.
- `scripts`: staging, fixture runner, diagnostics 및 **이전 import baseline 재현용** patch builder.
- `graphics-translator/patches`, `graphics-translator/upstream.lock.json`: import 당시의 frozen provenance. 현재 루트 소스를 변경할 때 다시 패치로 역수출하지 않는다. `scripts/build-owned-graphics.py`는 frozen baseline만 재생성하므로 현재 코드 빌드에 사용하지 않는다.
- `docs/provenance`: 소스별 import hash, 이전 build/runtime manifest와 GPU 결과. 과거 절대 경로는 출처 기록이며 새 저장소의 파일 존재를 뜻하지 않는다.

## 빌드

macOS의 Xcode Command Line Tools/Metal toolchain, Python 3, Meson 1.10.0, Ninja 1.13 계열,
llvm-mingw clang 22.1.8 및 **x86_64 native LLVM 15.0.7** static library, 설치된 Wine SDK가 필요하다.
Apple Silicon에서는 이 x86_64 Wine 경로 실행에 Rosetta가 필요하다.
Apple Metal Shader Converter/GPTK/D3DMetal/SPIR-V compiler fallback은 사용하지 않는다.

아래 변수는 **설치된 toolchain 경로**로 지정한다. LLVM/Wine SDK를 이 저장소에 복사하지 않는다.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
export NATIVE_LLVM=/absolute/path/to/llvm-15-prefix
export WINE_SDK=/absolute/path/to/installed-wine-sdk
export PATH="$LLVM_MINGW:/absolute/path/to/meson-and-ninja-bin:$PATH"
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++

meson setup build/local/build . --cross-file build-win64.txt \
  --prefix "$PWD/build/local/install" --buildtype debugoptimized \
  -Dwine_builtin_dll=true -Denable_d3d12=true \
  -Denable_nvapi=true -Denable_nvngx=true \
  -Dnative_llvm_path="$NATIVE_LLVM" -Dwine_install_path="$WINE_SDK"
ninja -C build/local/build -j8
meson install -C build/local/build --no-rebuild
```

처음에는 새 build 경로를 사용한다. 이후 수정은 Ninja와 install을 다시 실행한다.
현재 호스트에서 재사용 가능한 외부 입력은 `../zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/llvm`,
`../zzz-wine-d3dmetal-dx12/build/wine-crossover/host`,
`../zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/tools/bin`이다.
이는 source 의존성이 아니라 기존 LLVM/Wine/tool 실행 파일의 위치다. 다른 설치 경로로 바꿀 수 있다.

## 검증

CPU/tooling regression:

```sh
python3 scripts/test-owned-graphics-build.py
python3 scripts/test-owned-graphics-staging.py
python3 scripts/test-owned-graphics-diagnostics.py
```

GPU 검증은 caller가 제공하는 Wine runtime의 **새 복사본**과 **전용 prefix**에서만 한다.
기존 게임 설치나 live prefix에 DLL을 복사하지 않는다. Stage는 native dependency/rpath와 module 세트를 검사한다.

```sh
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --wine-source /absolute/path/to/wine-runtime \
  --wine-dest build/local/runtime --with-nvapi --with-nvngx
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -municode -static \
  -Iinclude/native/directx graphics-translator/tests/owned-d3d12-pipeline-cache.cpp \
  -ld3d12 -ldxgi -ldxguid -lole32 -o build/local/pipeline-cache.exe
python3 scripts/test-owned-graphics.py --runtime build/local/runtime \
  --suite pipeline-cache --fixture build/local/pipeline-cache.exe \
  --out build/local/cache-proof --validation
```

Runner의 output은 매번 새 경로여야 한다. 다른 fixture는 `--suite dxil`, `raytracing` 등을 사용한다.
Metal validation 오류, 비정상 command completion, 외부 D3DMetal backend 로드는 실패다.

## 지원 범위와 남은 제한

- Graphics/compute PSO의 `GetCachedBlob` 및 `CachedPSO`: metallib, immutable pool bytes/alignment/relocation 저장·복원.
- 캐시는 adapter ID, native module UUID, OS build와 PSO/shader/root identity를 검사한다. 다른 build의 캐시는 재생성해야 한다.
- Warm cache도 Metal library/function/PSO 생성은 수행한다. Native pipeline binary archive가 아니다.
- `CreatePipelineLibrary`: `E_NOTIMPL`. Fence rewinding 및 미완료 mixed producer 지원은 구현되지 않았다.
- DXR fixture 성공은 전체 D3D12 conformance나 게임 RT 동작 보증이 아니다. OPTIONS5의 RaytracingTier도 0이다.
- 실제 ZZZ 정상 렌더링/호환성은 증명하지 않았다. 이전 draw/present 수치나 검은 창을 성공으로 취급하지 않는다.

상세 계약·이전 조사 기록은 `graphics-translator/*.ko.md`에 보존했다. 해당 문서의 예전 repository/build 경로와 patch 재수출 설명은 **역사적 기록**이다. 현재 개발/빌드 절차는 이 README와 `AGENTS.md`가 기준이다.

## 라이선스

이 저장소는 `LICENSE`의 MIT 라이선스를 따른다. Copyright (c) 2026 dbc-hbin.

`external/nvapi/License.txt`, `include/native/directx/COPYING.MinGW-w64.txt`, `src/util/dxvk.LICENSE`는 vendored dependency의 원본 라이선스이며 그대로 유지한다. 각 파일 상단의 원저작자 copyright 표기는 출처 표기로 남겨 두었다. Git은 새 `main` 저장소로 초기화했으며 remote 추가와 push는 하지 않았다.
