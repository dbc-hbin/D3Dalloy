# dxmt-dx12

DXMT 기반의 D3D12 → Metal 번역기와 자체 DXIL → AIR/metallib compiler.
검증된 18-patch 소스를 독립 저장소로 분리했다. 게임, 런처, Wine runtime, prefix, LLVM/toolchain 및 빌드 산출물은 배포 소스에 포함하지 않는다.

## 소스 기준과 구조

**프로젝트 목표는 ZZZ 실행이 아니라 최신 공개 Direct3D/DXGI·shader 규격을 정확하고 완전하게 구현하는 범용 번역기다.**
특정 게임의 실행이나 FPS를 완료 기준으로 삼지 않는다. 최신 규격 revision과 backend별 지원/미검증/미충족 상태를 구분한다.

개발 우선순위는 **P0 기준 고정·통합 안정화 → P1 기본 계약 완전성 → P2 현대 기능 기반 → P3 DXR·mesh·VRS·sampler feedback·Work Graphs 등 최신 기능 → P4 성능·실사용 완성도**다.
정확한 pipeline statistics와 과거 단계에서 미뤘던 기능도 최종 구현 범위에 포함한다.
단계별 작업·완료 기준은 [구현 상태 문서의 개발 목표와 우선순위](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#개발-목표와-우선순위)에 정리했다.

- **현재 개발의 기준은 이 저장소 루트의 소스다.** `src/`를 직접 수정하고 아래 Meson 명령으로 빌드한다.
- DXMT upstream: `7c8dee1c2d73415301ceb7d1fa810861cef4cd67` + owned patch 18개. 최신 PSO cache patch까지 적용되어 있다.
- `src/d3d12`: D3D12 device/queue/resource/PSO/command/DXR 구현.
- `src/airconv`: 자체 DXIL/DXBC compiler, AIR/metallib writer.
- `src/winemetal`, `src/dxmt`, `src/dxgi`: Metal bridge, 공용 런타임, DXGI. D3D10/11 공용 구현과 NVAPI/NGX도 번역기의 빌드·실행 의존성이므로 유지했다.
- `include/native/directx`, `external/nvapi`, `external/nvngx`: 고정 revision의 vendored dependency. 별도 Git repository/submodule이 아니며 라이선스와 원본 소스를 포함한다.
- `graphics-translator/tests`: CPU/PE/GPU regression fixture와 고정 DXIL.
- `scripts`: staging, fixture runner, diagnostics 및 **이전 import baseline 재현용** patch builder.
- `graphics-translator/patches`, `graphics-translator/upstream.lock.json`: import 당시의 frozen provenance. 현재 루트 소스를 변경할 때 다시 패치로 역수출하지 않는다. `scripts/build-owned-graphics.py`는 frozen baseline만 재생성하므로 현재 코드 빌드에 사용하지 않는다.
- `docs/provenance`: 소스별 import hash, 이전 build/runtime manifest와 GPU 결과. 과거 절대 경로는 출처 기록이며 새 저장소의 파일 존재를 뜻하지 않는다.
- `build/legacy`, `build/retired-evidence`: 이전 작업 root와 보존 증거. 역사적 provenance 표기 전용이며 현재 빌드 입력이 아니다.

## 빌드

macOS의 Xcode Command Line Tools/Metal toolchain, Python 3, Meson 1.10.0, Ninja 1.13 계열,
llvm-mingw clang 22.1.8 및 **x86_64 native LLVM 15.0.7** static library, 설치된 Wine SDK가 필요하다.
Apple Silicon에서는 이 x86_64 Wine 경로 실행에 Rosetta가 필요하다.
Apple Metal Shader Converter/GPTK/D3DMetal/SPIR-V compiler fallback은 사용하지 않는다.

이 저장소는 로컬 toolchain을 `toolchains/` 아래에 두고 상대 경로로 참조한다. `toolchains/`는 Git에서 제외된다.
절대 경로는 Meson이 `native_llvm_path`를 `join_paths`로 처리하는 경로에서 문제를 일으킬 수 있으므로 상대 경로를 사용한다.

```sh
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
export PATH="$LLVM_MINGW:$PWD/toolchains/tools/bin:$PATH"
export CC_FOR_BUILD=/usr/bin/clang CXX_FOR_BUILD=/usr/bin/clang++

meson setup build/local/build . --cross-file build-win64.txt \
  --prefix "$PWD/build/local/install" --buildtype debugoptimized \
  -Dwine_builtin_dll=true -Denable_d3d12=true \
  -Denable_nvapi=true -Denable_nvngx=true \
  -Dnative_llvm_path=toolchains/llvm-15 \
  -Dwine_install_path=toolchains/wine-sdk
ninja -C build/local/build -j8
meson install -C build/local/build --no-rebuild
```

필요한 로컬 입력은 다음 위치에 둔다. 셋 다 소스 의존성이 아니라 설치된 실행 파일/prefix 위치이며, 다른 경로로 바꿀 수 있다.

- `toolchains/llvm-15`: x86_64 native LLVM 15.0.7 install prefix (`docs/DEVELOPMENT.md`의 절차로 생성)
- `toolchains/tools`: Meson 1.10.0 / Ninja 1.13 계열이 설치된 Python venv
- `toolchains/wine-sdk`: `-Dwine_install_path`로 넘길 설치된 Wine SDK
- `toolchains/wine-runtime`: GPU 검증에서 staging 기준으로 쓰는 Wine runtime 사본

처음에는 새 build 경로를 사용한다. 이후 수정은 Ninja와 install을 다시 실행한다.

## 검증

CPU/tooling regression:

```sh
python3 scripts/test-owned-graphics-build.py
python3 scripts/test-owned-graphics-staging.py
python3 scripts/test-owned-graphics-diagnostics.py
python3 scripts/test-owned-ngx-frontend.py
```

`test-owned-ngx-frontend.py`는 GPU 없이 현재 소스에서 `nvngx.dll`을 CPU/PE로 다시 만들고, D3D11/D3D12 export
집합과 MSVC parameter facade의 공식 NGX ABI slot을 검증한다.

GPU 검증은 caller가 제공하는 Wine runtime의 **새 복사본**과 **전용 prefix**에서만 한다.
기존 게임 설치나 live prefix에 DLL을 복사하지 않는다. Stage는 native dependency/rpath와 module 세트를 검사한다.
이 저장소의 로컬 staging 기준 runtime은 `toolchains/wine-runtime`이다.

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

Runner의 output은 매번 새 경로여야 한다. 다른 fixture는 `--suite dxil`, `raytracing` 등을 사용한다.
Metal validation 오류, 비정상 command completion, 외부 D3DMetal backend 로드는 실패다.

## 지원 범위와 남은 제한

최근 렌더링 코어의 **P0 구현·검증 결과와 남은 경계**는
[CORE-IMPLEMENTATION-STATUS.ko.md](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md)에 정리했다.
문서 선행 단계 이후 P0 통합 빌드와 11개 contract suite를 Metal validation 아래 통과했다. Predicate skip 조건, clear rect 경계, buffer RTV clear의 encoder 시작 순서, stencil resolve, sample-position helper link, empty viewport의 VS side effect 보존을 수정했다. Suite별 runtime/evidence, frozen DXIL 입력의 생성 도구·hash, 재현 명령은 상태 문서에 기록했다. 전체 최신 규격 지원 선언은 아니며, 다음 우선순위는 **P1 기본 API·shader·presentation 계약의 완전성**이다.

현재 루트 소스 재대조 기준 P1의 다음 구현 묶음은 **`AtomicCopyBufferUINT/UINT64` → `ResizeBuffers1`의 backbuffer별 present queue 계약 → 정확한 `PIPELINE_STATISTICS`** 순이다. 그 다음은 Stream Output과 SO statistics를 compiler/runtime/query 전체로 연결한다. 과거 조사 문서에 미구현으로 남은 timestamp/occlusion query, predication, bundle, multiple-fence event, `ResolveSubresourceRegion`, residency 확장은 현재 루트에서 이미 구현되어 있으므로 현재 backlog 판정에는 사용하지 않는다. 상세 설계는 상태 문서의 `P1 다음 구현 후보와 실행 순서`를 따른다.

- Graphics/compute PSO의 `GetCachedBlob` 및 `CachedPSO`: metallib, immutable pool bytes/alignment/relocation 저장·복원.
- 캐시는 adapter ID, native module UUID, OS build와 PSO/shader/root identity를 검사한다. 다른 build의 캐시는 재생성해야 한다.
- Warm cache도 Metal library/function/PSO 생성은 수행한다. Native pipeline binary archive가 아니다.
- `CreatePipelineLibrary`: `E_NOTIMPL`.
- `AtomicCopyBufferUINT` / `AtomicCopyBufferUINT64`: 현재 `IMPLEMENT_ME`; P1-A 대상.
- `ResizeBuffers1`: 현재 D3D12 node/present-queue 배열을 무시하므로 P1-B에서 backbuffer별 queue rotation과 transactional resize를 구현한다.
- `PIPELINE_STATISTICS`: 미구현. 일부 Metal statistic counter를 전체 D3D12 statistics로 가장하지 않고 P1-C에서 정확한 query/instrumentation 경로를 구현한다.
- Stream Output: PSO와 `SOSetTargets`가 미지원이며 SO statistics까지 함께 구현하기 전에는 `SO_BUFFER`를 광고하지 않는다.
- Fence는 논리값과 Metal event를 분리해 CPU/GPU rewind 및 미완료 다중 producer를 지원한다. Queue wait와 완료 이벤트는 등록 뒤 도달한 값을 latch하므로 이후 rewind로 취소되지 않는다.
- Queue wait는 API caller를 막지 않지만 consumer worker의 후속 encode는 producer GPU 완료까지 보류한다. 기존 commit-only gate보다 CPU/GPU overlap이 줄 수 있으며 성능은 별도 측정 대상이다.
- DXR fixture 성공은 전체 D3D12 conformance나 게임 RT 동작 보증이 아니다. OPTIONS5의 RaytracingTier도 0이다.
- 실제 ZZZ 정상 렌더링/호환성은 증명하지 않았다. ZZZ는 범용 호환성 평가의 한 workload이며 개발 목표를 제한하지 않는다. 이전 draw/present 수치나 검은 창을 성공으로 취급하지 않는다.

상세 계약·이전 조사 기록은 `graphics-translator/*.ko.md`에 보존했다. 해당 문서의 예전 repository/build 경로와 patch 재수출 설명은 **역사적 기록**이다. 현재 개발/빌드 절차는 이 README가 기준이다.

## 라이선스

이 저장소는 `LICENSE`의 MIT 라이선스를 따른다. Copyright (c) 2026 dbc-hbin.

`external/nvapi/License.txt`, `external/nvngx/LICENSE.txt`, `include/native/directx/COPYING.MinGW-w64.txt`, `src/util/dxvk.LICENSE`는 vendored dependency의 원본 라이선스이며 그대로 유지한다. 각 파일 상단의 원저작자 copyright 표기는 출처 표기로 남겨 두었다. Git은 새 `main` 저장소로 초기화했으며 remote 추가와 push는 하지 않았다.
