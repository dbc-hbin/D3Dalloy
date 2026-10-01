# dxmt-dx12

DXMT 기반의 D3D12 → Metal 번역기와 자체 DXIL → AIR/metallib compiler.
검증된 18-patch 소스를 독립 저장소로 분리했다. 게임, 런처, Wine runtime, prefix, LLVM/toolchain 및 빌드 산출물은 배포 소스에 포함하지 않는다.

## 소스 기준과 구조

**프로젝트 목표는 ZZZ 실행이 아니라 최신 공개 Direct3D/DXGI·shader 규격을 정확하고 완전하게 구현하는 범용 번역기다.**
특정 게임의 실행이나 FPS를 완료 기준으로 삼지 않는다. 최신 규격 revision과 backend별 지원/미검증/미충족 상태를 구분한다.

**개발 대상은 D3D12와 그 compiler·공유 backend다. D3D11은 DXMT 업스트림 구현을 사용하며, 정말 필요한 경우가 아니면 변경하지 않는다.** D3D12 구현에 필수인 공유 API/ABI 변경이나 그로 인한 기존 동작 보존만 최소 범위로 다루고, D3D11 전용 기능·버그 수정·검증을 독립 작업으로 확장하지 않는다. 기존 D3D11 변경은 삭제하지 않고 [조사 결과와 유지 방침](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#d3d11-변경-조사와-유지-방침)에 기록했다.

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
`raytracing`은 RayQuery의 CS·VS·PS·HS·DS·GS·AS·MS·raygen·closest-hit·any-hit·intersection·miss·callable
stage와 raygen indirect를 기존 triangle/procedural scene의 24 lane × 64 word CPU 기준값과 비교한다.
`graphics-translator/tests/dxil/build-raytracing-rayquery-stages.sh`는 DXC 1.9로 추가 stage 입력과
기존 callback-control/state-object 입력을 고정한다. Any-hit/intersection의 inline query가 있는
pipeline만 kernel query traversal과 visible callback을 사용하며, 나머지는 native traversal을 유지한다.
기존 IgnoreHit·AcceptHitAndEndSearch·ReportHit 거부/interval·opacity·local root·AddToStateObject
기준값도 새 경로를 검증한다. Metal API/Shader Validation 증거는
`build/local/p3/rqstages/run-08/result.json`이며, 잘못된 candidate InstanceID 입력은
`mutation-run`에서 추가 10 stage 모두 실패했다. 이후 전체 DXR 증거를 통합한
`build/local/p3/integrate/dxr-cap-01`에서 공개 RaytracingTier **1.1**(`raytracing_tier=11`)을 확인했다.
P1 계약 회귀에는 `--suite copy-resource-casts`와 `--suite execution-safety`도 있다.
각 소스는 `graphics-translator/tests/owned-d3d12-copy-resource-casts.cpp`와
`graphics-translator/tests/owned-d3d12-execution-safety.cpp`이며, 둘 다 `main()`을 사용하므로
위 컴파일 예제의 `-municode`를 제외한다. 실제 증거와 미완료 경계는 아래 상태 문서를 참조한다.
추가 소스의 통합 검증용 suite는 `expanded-formats`와 `standard-swizzle`이다.
각 fixture는 `graphics-translator/tests/owned-d3d12-{expanded-formats,standard-swizzle}.cpp`이며
`-ld3dcompiler`도 링크한다. Expanded-formats는 `-municode`, standard-swizzle은
`-Iinclude/native/directx`를 사용하고 `-municode`를 제외한다. 두 fixture는 호스트 x86 실행 복구 후
새 격리 `runtime-04`에서 Metal API/Shader Validation을 통과했다. 이 결과만으로 P1 전체 통과를 선언하지 않는다.

부분 3D UAV의 DXIL write→load·atomic·slice 경계, packed cube-array sampling,
BC1↔비압축 bitcast의 회귀 suite는 다음과 같다. 세 fixture 모두 `main` entry이며
`-std=c++20 -O2 -static -Iinclude/native/directx`로 빌드하고 `-ld3d12 -ldxgi -ldxguid`를
링크한다. `packed-cubearray`에는 `-ld3dcompiler`도 필요하다.

| Suite | Fixture | 추가 입력 |
|---|---|---|
| `partial-3d-uav-dxil` | `owned-d3d12-partial-3d-uav-dxil.cpp` | Runner가 `tests/dxil/partial-3d-uav.dxil`을 복사하고 argv로 전달 |
| `packed-cubearray` | `owned-d3d12-packed-cubearray.cpp` | 없음 |
| `reinterpret-copy` | `owned-d3d12-reinterpret-copy.cpp` | 없음 |

각 fixture는 `graphics-translator/tests/`에 있으며 `--validation`으로 실행한다.
이 세 suite는 새 격리 `runtime-05`에서 실제 GPU 결과를 통과했다.
Metal validation 오류, 비정상 command completion, 외부 D3DMetal backend 로드는 실패다.
`--validation`은 Metal API/Shader Validation을 모두 켠다. `atomic-copy-live`의 동시 관찰은
`--api-validation`으로 실행한다. Shader Validation이 compute queue를 직렬화하는 장치에서는
실제 동시 관찰이 불가능하므로 API Validation만 유지한다. 기본 `atomic-copy` 계약 검사는
여전히 `--validation`으로 실행하며, live suite는 같은 reader thread의 값 변화가 없으면 실패한다.

P2 suite는 다음과 같다. Fixture는 모두 `graphics-translator/tests/owned-d3d12-<suite>.cpp`이며 `-std=c++20 -O2 -static`, `-ld3d12 -ldxgi -ldxguid -ld3dcompiler -luuid -lole32`로 빌드한다. `wmain` fixture만 `-municode`를 쓴다. Device5+·CommandList5+·OPTIONS12+ 타입이 필요하므로 `-Iinclude/native/directx` 없이 llvm-mingw header를 사용한다. 필요한 frozen DXIL/DXBC는 runner가 `graphics-translator/tests/dxil/`에서 복사한다.

| Suite | Entry | 범위 |
|---|---|---|
| `sm6` | `wmain` | SM 6.x wave/quad·int64·16-bit·barycentrics·compute 64-bit atomic·SV_ViewID·fp32 denorm mode·cap-gated 거부 |
| `bindless` | `wmain` | Binding Tier 3, SM 6.6 dynamic resources, RS 1.2, heap ROV |
| `enhanced-barriers` | `main` | Enhanced barriers, Device10–14, CommandList5–9 dynamic state |
| `render-passes` | `main` | Render pass fallback, GPU upload heap, UINT border |
| `tiled` | `main` | Reserved/tiled resource Tier 2 |
| `pipeline-library` | `wmain` | `ID3D12PipelineLibrary1`, cross-process reuse |
| `sharing` | `wmain` | Shared heap/resource/fence NT handle, cross-process·named |
| `geometry`, `tessellation` | `main` | GS/HS/DS와 TS+GS(DXBC·DXIL), 완료·검증 |

WoW64(32-bit) 실행은 i686 Meson build(`--cross-file build-win32.txt`)의 install을 `stage-owned-graphics.py stage --dxmt-install32 <install32>`로 함께 staging하고, `i686-w64-mingw32-clang++`로 빌드한 PE32 fixture를 같은 runner로 실행한다.

## 지원 범위와 남은 제한

최근 렌더링 코어의 **P0·P1·P2·P3 완료 결과와 검증 경계**는
[CORE-IMPLEMENTATION-STATUS.ko.md](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md)에 정리했다.
**P0·P1(2026-09-30), P2·P3(2026-10-01)는 검증한 Apple M5 Pro/macOS 27 범위에서 완료했다.** 현재 보고 값은 Shader Model **6.5**, feature level **12_1**, RaytracingTier **1.1**, MeshShaderTier **1**, VariableShadingRateTier **2**, SamplerFeedbackTier **1.0**, ViewInstancingTier **1**, WorkGraphsTier **1.0**, ConservativeRasterizationTier **1**이다. P2의 7개 slice와 P3 후속 구현·최종 통합 증거는 [상태 문서](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#p3-완료-결과-2026-10-01)에 기록했다. SM 6.6은 모든 stage의 64-bit atomic이 필요해 막혀 있으며 fragment lock emulation은 Apple GPU를 hang시킨다. 완료 판정은 전체 D3D12/DXR conformance·실제 게임 호환성 보증이 아니다. HDR10PLUS는 macOS에 ST 2094-40 입력 경로가 없어 미지원이며 `DXGI_ERROR_UNSUPPORTED`로 거부한다. 물리 HDR scanout·다중 출력·다른 GPU/OS는 미검증이다.

P1-A(`AtomicCopyBufferUINT/UINT64`), P1-B(`ResizeBuffers1` present queue 계약), P1-C(`PIPELINE_STATISTICS`), P1-D(VS→Stream Output과 SO statistics)를 구현·GPU 검증했다. MSAA는 원래 VS를 한 번 capture하고 primitive별 8×8 tile에서 pixel-frequency PS를 픽셀당 한 번 실행한 뒤 sample별 출력을 재생한다. 통계를 sample 수로 나누거나 MSAA를 제외하지 않는다. 일반 non-MSAA indexed restart 수정은 필요한 strip만 VS index pulling을 사용하며, list와 cut 값이 일치하는 strip은 native indexed draw를 유지한다.

- Format capability: 12종 32-bit FLOAT/SINT/UINT SO, mip·typed cast·depth/stencil·sRGB UAV의 기존 GPU 증거를 유지한다. RGB32→RGBA32 backing, packed 4:2:2→RGBA8 pair 변환과 STANDARD_SWIZZLE canonical backing은 `runtime-04`의 공개 D3D12 GPU 검증을 통과했다. RGB32 typed-buffer remap·OOB, 1D/2D/3D shader load, CPU/GPU·predicated copy, packed filtering, MSAA load·FLOAT resolve, blend 6 case와 canonical alias·residency 전이를 포함한다. Partial 3D UAV의 DXIL write→load visibility는 texture fence로 수정해 `runtime-05`에서 DXBC와 함께 GPU 검증을 통과했다. BC↔uncompressed bitcast와 packed cube-array도 통과했다. [CAPABILITIES.ko.md](graphics-translator/CAPABILITIES.ko.md) 참조.
- D24 plane copy: buffer의 packed low24와 native float depth 사이의 양방향 GPU 변환, 정확한 나눗셈·nearest-even 반올림, D32S8 plane blit를 구현했다(`depth-plane-copy`). Typed/typeless 4종의 독립 clear/upload·shader read·copy, mip/array·row padding·offset·24-bit 경계와 guard를 Metal validation으로 검증했다.
- Sample-count: quality 0·standard pattern과 native programmable sample-position 지원 장치의 center pattern(2×/4×/8×)을 지원한다. `runtime-10`의 `multisample-capabilities`·`dxil-texture`는 center full/masked color·depth·occlusion과 독립적인 resource/target sample-position 조합을 Metal API/Shader Validation으로 검증했다. RGB32 MSAA load/FLOAT resolve도 GPU 검증을 통과했다. BC MSAA·MSAA UAV·16× 이상은 미지원이다. 실제 GPU 증거는 M5 Pro/macOS 27이다.
- Line 입력 복원: `runtime-10`의 `msaa-lines`는 짧은 line과 W 비율 1:1000의 양방향 보간, D24S8 depth/stencil, centroid·sample·EvaluateAttribute·derivative와 covered-only UAV를 검증했다. Sample-frequency derivative case는 76 covered sample과 76 UAV 증가가 일치한다. 같은 runtime의 전체 346-case statistics도 통과했다. ForcedSampleCount의 numeric/binary occlusion·indirect predication·layered replay는 `forced-query-09fixed-evidence`에서 통과했다.
- Shader linkage: DXBC/DXIL stage 간 semantic 이름·index를 맞추고 각 semantic의 component를 독립적으로 pack/unpack한다. `dxil-ia`는 mixed DXBC↔DXIL, reordered clip-distance와 packed float2의 실제 RGBA를 확인한다. Fixture 링크에 `-ld3dcompiler`가 필요하다. 공유 D3D11 VS→PS·GS→PS 회귀도 확인했다. 후속 clip/cull Inf·NaN 보존은 raw capture 기반 보간으로 변경하여 `runtime-15`에서 1×/4× point·line·triangle·sample-frequency를 일반 실행과 Metal API/Shader Validation으로 검증했다. 최신 compiler cache version은 32이며, 새 1× depth/query와 MSAA 통합 회귀도 통과했다. Rasterization-disabled SO는 실행되지 않는 PS의 거리 입력으로 capture 경로를 선택하지 않는다.
- Graphics/compute PSO의 `GetCachedBlob` 및 `CachedPSO`: metallib, immutable pool bytes/alignment/relocation 저장·복원.
- Rasterizer-ordered buffer: direct root·nested descriptor table·unbounded·register space·texture 혼합·layer·warm cache와 4× sample-frequency direct/table 경로를 구현했다. `raster-order` suite의 15 case가 `runtime-13`에서 일반·API-only·API/Shader Validation을 모두 통과했다. Sample-frequency의 두 경로는 각각 8개 word 모두27을 확인했다. AIR 2.7/2.8의 ROG marker texture-read ABI와 helper metadata를 구분한다.
- 캐시는 adapter ID, native module UUID, OS build와 PSO/shader/root identity를 검사한다. 다른 build의 캐시는 재생성해야 한다. P3 hidden-resource/signature AIR ABI 변경으로 D3D12 serialized PSO revision을 4로 올렸다. 이전 blob은 `CachedPSO`와 이를 포함한 `CreatePipelineLibrary` 모두 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`로 거부하므로 저장된 캐시를 버리고 다시 생성해야 한다. Graphics compile key revision **5**는 별도 identity revision이며 serialized PSO/blob revision **4**와 혼동하지 않는다.
- Warm cache도 Metal library/function/PSO 생성은 수행한다. Native pipeline binary archive가 아니다.
- `CreatePipelineLibrary`: `ID3D12PipelineLibrary1` 구현(`pipeline-library`). `DXMTPLB` container는 기존 PSO cache blob의 모음이며 native Metal binary archive가 아니다.
- P3 cache revision 검증: 새 격리 `build/local/p3/cache-epoch/runtime-after`에서 `pipeline-cache`·`pipeline-library`가 Metal API/Shader Validation을 통과했다(`cache-after`·`library-after`). 현재 PSO/library의 GPU round-trip과 child의 4 restore·0 cold compile을 확인했다. `nested-version-proof`는 현재 library의 identity/key를 유지하고 digest를 재계산한 revision 3 payload도 `DRIVER_VERSION_MISMATCH`로 거부함을 확인한다.
- P2 기반 cap(현재 유지): HighestShaderModel **6.5**(SM 6.6은 모든 stage의 64-bit atomic이 필요해 막힘), ResourceBindingTier 3·RS 1.2, TiledResourcesTier 2(3D tile shape 차이로 Tier 3/4 미보고), EnhancedBarriers TRUE·RelaxedFormatCasting FALSE, RenderPassesTier 0·RenderPassesValid TRUE, GPUUploadHeap TRUE. GS/HS/DS와 TS+GS는 DXBC·DXIL 모두 지원하며, P3에서 TS+GS ExecuteIndirect·GS SO stream 0–3·DS-only SO와 early-Z MSAA statistics의 기존 제한을 구현·GPU 검증으로 닫았다. Shared handle은 Wine section NT handle로 cross-process·이름 열기를 지원하며 D3D11 interop은 하지 않았다. WorkGraphsTier **1.0**은 P3의 실제 OPTIONS21·9-case x64/WoW64 GPU proof 뒤 보고한다. P2 당시 결과와 P3 후속 증거·경계는 상태 문서에서 구분한다.
- `AtomicCopyBufferUINT`와 `AtomicCopyBufferUINT64`: 구현(`atomic-copy`, `atomic-copy-live`). UINT64는 macOS Apple8 이상에서 같은 값의 64-bit atomic min/max로 publish한다. D3D의 단일 writer·queue 간 writer 동기화 계약을 전제로 하며 다른 Metal family는 `E_NOTIMPL`이다. M5 Pro에서 36개 UINT64 조합, 33개 API validation case, PSO 수명·재실행과 동시 low-word generation/payload 관찰을 검증했다. 일반적인 shader용 64-bit atomic load/store 지원을 추가한 것은 아니다.
- `ResizeBuffers1`: backbuffer별 present queue rotation과 transactional resize 구현(`present-queues` suite, `--mode present`). Node mask는 단일 node(0/1)만 허용.
- D3D12 HDR metadata: HDR10 값 복사·교체·NONE 해제와 인자 검사를 구현했다(`hdr-metadata`). 7-frame API/queue 검증과 별도 production presenter drawable GPU readback으로 실제 tone-map 효과·snapshot·검정색/동일 peak 경계를 확인했다. HDR10PLUS는 미지원이며 native HDR10 신호 전달을 뜻하지 않는다.
- D3D12 color-space: 지원 조회·설정의 일치, 미지원 설정 실패 시 상태 보존, resize 후 명시한 색 공간 유지, 이전 Present의 GPU 완료 후 layer 변경을 구현했다(`colorspace`). 지원 4종·미지원 23종, 3개 포맷의 13-frame 공개 API와 native layer 태그, 별도 11-frame drawable/GPU-retirement probe를 검증했다. D3D11 전용 코드는 변경하지 않았다. 실제 화면의 색 정확도·다중 출력 전환은 미검증이며 format/지원 보고의 후속 결과는 별도 항목으로 기록한다.
- Fullscreen: D3D12·D3D11의 출력 조회/native 전환/`ResizeTarget` 오류 시 상태 보존, 재진입 차단, fullscreen 생성과 invalid HWND 거부를 구현했다. `fullscreen --mode present` suite에서 반복 전환·창 복원, API별 3,888픽셀 readback·Present, native 진입/이탈 실패를 Metal validation으로 검증했다. D3D11은 출력 proxy의 gamma를 공개 인터페이스로 읽어 Present 충돌을 방지한다. 다중 모니터·hot-unplug·HDR·color-space는 이 검증 범위가 아니다. 실행 recipe와 증거는 상태 문서의 **P1 후속 — Fullscreen 오류·전환** 참조.
- Buffer UAV clear: 사각형을 뷰 좌표로 클리핑한 뒤 RAW/typed buffer offset을 적용한다. 부분 뷰의 앞·뒤 데이터를 보존하며 전체/부분 clear와 predicate 실행·건너뛰기를 60 GPU case로 검증했다(`predication-indirect`).
- Root Signature 1.1: `DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS`와 유효한 data flag 조합을 허용하고 상충하는 volatility 조합은 거부한다. 22개 API 계약과 실제 DXIL의 null read·범위 밖 store/atomic·sentinel 보존을 검증했다(`core-contracts`, `dxil`).
- `PIPELINE_STATISTICS`: 공개 heap 생성과 DIRECT list의 Begin/End/Resolve(11개 `UINT64`, 88 byte)를 지원한다. `pipeline-statistics` suite는 DXBC 168개·DXIL 178개 GPU case와 26개 validation case를 Metal API/Shader Validation으로 통과했다. 2×/4×/8× MSAA의 UAV 실행·sample별 색, early/late depth·stencil·occlusion, centroid·동적 interpolation·helper derivative, indirect·bundle·query 수명을 검증했다. MSAA는 query 유무와 무관하게 새 실행 경로를 사용하며 추가 capture/encoder pass와 submission-worker readback 비용이 있다. 해당 shader variant는 AIR 2.8/macOS 26 target이며 실제 GPU 검증은 M5 Pro/macOS 27에 한정한다. 비용·증거·지원 경계는 상태 문서 P1-C 참조.
- Stream Output(P1-D): VS-only PSO(stream 0)에서 `DrawInstanced`/`DrawIndexedInstanced`와 `ExecuteIndirect` DRAW/DRAW_INDEXED의 SO capture·assembly, `BufferFilledSize` append/overflow, `SO_STATISTICS_STREAM0..3`를 구현했다(`stream-output` suite). VS는 D3D draw 한 번만 실행하고 capture는 VS epilogue에서 한다. Indexed SO draw는 생성한 IA position buffer로 draw하고 VS가 app index를 직접 읽으므로, strip cut은 strip topology에서만 적용된다. Record는 claim word로 invocation 하나의 값만 담는다. Root signature에 `ALLOW_STREAM_OUTPUT`이 없으면 `E_INVALIDARG`이고, packed semantic의 `StartComponent`는 element 기준이다. 남은 제한: adjacency/patch topology의 VS-only SO는 `E_NOTIMPL`이다. GS SO stream 0–3·TS+GS SO/indirect·GS 없는 HS/DS SO는 P3 후속 증거에서 별도로 검증했다.
- 일반 non-SO indexed restart: `indexed-restart` fixture의 최종 `build/local/indexed-restart/proof-indexed-restart-03`에서 `runtime-02` + Metal validation으로 DXBC/DXIL 각 31개, 총 **62 GPU case PASS**. 이전 실패는 `build/local/p1d/indexed-restart-before-01`(R16 line strip cut disabled, pixel 3·5 expected 1 / actual 0) 및 `build/local/p1d/indexed-restart-mismatch-before-01`(R32/cut FFFF). 새 generated index buffer/prepass/readback 없이 필요한 strip만 VS pulling하고, normal/ICB PSO variant를 lazy 생성한다. Pull variant는 cached blob에 serialize되지 않으므로 영향을 받는 첫 draw에 컴파일된다.
- `dxil-ia` suite: `graphics-translator/tests/owned-d3d12-dxil-ia.cpp`와 복원한 `graphics-translator/tests/dxil/dxil-ia-{vs,ps}.hlsl` 및 새로 빌드한 `.dxil`을 사용한다. Runner는 이전 바이너리에 의존하지 않는다. 이전 runtime 대비 소스 fixture는 `build/local/ia-restore/proof-source-built-01`, 새 runtime의 회귀는 `build/local/indexed-restart/proof-dxil-ia-01`에서 검증했다.
- Fence는 CPU/GPU rewind 및 미완료 다중 producer를 지원한다. GPU signal은 queue의 순차 completion 스레드가 앞선 GPU 결과까지 검증한 뒤 논리값으로 공개한다. Native event 조기 관측 경로는 없다. Queue wait와 완료 이벤트는 등록 뒤 도달한 값을 latch하므로 이후 rewind로 취소되지 않는다. Device 오류 시 fence는 terminal `UINT64_MAX`가 되고 등록된 대기를 해제한다. 취소된 Present도 GPU present 순서 대기와 frame-latency credit을 해제한다.
- Queue wait는 API caller를 막지 않지만 consumer worker의 후속 encode는 producer GPU 완료까지 보류한다. 기존 commit-only gate보다 CPU/GPU overlap이 줄 수 있으며 성능은 별도 측정 대상이다.
- OPTIONS5의 RaytracingTier는 **1.1**이다. 일반 함수 collection의 typed-overload·export/linkage·hidden dependency와 named-root alias/filter/collision·resource-binding 계약을 구현했다. 최종 공통 `build/local/p3/integrate/runtime-04`의 `final64-04/raytracing`·`final32-04/raytracing`이 25개 frozen DXIL 입력, contract/state-object **13**, AS **16**, RayQuery **15**, vertex format **13**을 API/Shader Validation 아래 통과했다. 전체 DXR conformance·게임 RT 보증으로 확대하지 않는다.
- 실제 ZZZ 정상 렌더링/호환성은 증명하지 않았다. ZZZ는 범용 호환성 평가의 한 workload이며 개발 목표를 제한하지 않는다. 이전 draw/present 수치나 검은 창을 성공으로 취급하지 않는다.

### P3 완료 증거와 검증 범위

- **FL12_1·conservative raster Tier 1:** 새 `build/local/p3/integrate/runtime-03`의 `fl121-cap-01`·`conservative-cap-01` strict runner가 통과했다. 최대 FL=0xc100·FL12_1 생성 성공·FL12_2 거절, conservative 전체 10 family와 clip/cull/view·indirect/SO/statistics·depth 1180 sample을 검증했다. 선행 FL12_0 증거 `build/local/p3/fl2/run-08`·`cap-01`은 당시 기록이며 Shader Model **6.5**는 유지한다.
- **DXR 1.1:** 최종 공통 `build/local/p3/integrate/runtime-04`의 x64/WoW64 runner에서 공개 tier=11, RayQuery **15**·AS **16**·contract/state-object **13** case와 vertex format **13**종을 검증했다. Generic-function collection·named-root namespace·indirect·predication·root arguments·coverage를 포함한다.
- **P2의 과거 제한 해소:** `build/local/p3/gsindirect-contract/geometry-10`(**87/87**)·`tessellation-07`(**166/166**)에서 root/VB/IB 변경 indirect를, `build/local/p3/ds-so/proof-10`(**166/166**)에서 DS-only SO 순서·filled size·overflow·no-raster를 검증했다. GS SO stream 1–3·TS+GS indirect·early-Z GS/DS/MS MSAA statistics의 선행 증거도 상태 문서에 보존했다.
- **Feedback 1.0·view instancing 1:** `build/local/p3/feedback3/run-08`의 **83 case**, `build/local/p3/viewinst2/final-view-instancing`의 **23 case**를 검증했다.

**P3 구현과 최종 통합 검증을 이 장치 범위에서 완료했다.** Mesh의 필수 128-scalar 출력(SV_Position 포함)·cull과 공개 Tier 1의 **34 case**, VRS Tier 2의 **146 case**, conservative Tier 1의 전체 **10 family**를 검증했다. Native 124-scalar 한계를 예외로 인정하지 않고 초과 출력·cull을 capture/replay로 처리한다. WorkGraphsTier **1.0**은 실제 OPTIONS21 16-byte cap(`tier=10`)과 root/ordinary collection/local-only static sampler를 포함한 **9 case**를 x64/WoW64에서 통과했다.

최종 공통 `build/local/p3/integrate/runtime-04`에서 `final64-04`의 RT/WG/HDR 3 suite, `final32-04`의 RT/WG/HDR/query 4 suite가 모두 exit 0·API/Shader Validation 오류 없음·foreign backend load 없음으로 통과했다. Baseline+변경 경로의 최종 proof를 합산한 reachable coverage는 **x64 60(59 API/Shader+1 API-only), WoW64 58(57 API/Shader+1 API-only)**이다. **모든 suite를 단일 snapshot에서 재실행한 결과는 아니다.** Atomic live reader의 API-only 통과를 shader-validation 통과나 64-bit snapshot 보증으로 승격하지 않는다. 기존 source가 없는 standalone static-sampler fixture와 WoW64의 x64-only NGX 2 suite는 검증 불가 경계이며 미완료 P3 기능이 아니다.

**FL12_2는 미지원**이다: conservative raster 3·tiled resources 3·depth bounds·copy-queue timestamps·OPTIONS8 unaligned BC와 필수 전체 tier 등을 추가 요구한다. Shader Model **6.5**를 유지하며 SM 6.6을 FL12_1/12_2의 일괄 선행 조건으로 혼동하지 않는다. 물리 compositor·실제 게임·다른 GPU/OS·Windows reference differential·성능은 이 결과로 보증하지 않는다. [상세 증거와 경계](graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md#p3-완료-결과-2026-10-01) 참조.


상세 계약·이전 조사 기록은 `graphics-translator/*.ko.md`에 보존했다. 해당 문서의 예전 repository/build 경로와 patch 재수출 설명은 **역사적 기록**이다. 현재 개발/빌드 절차는 이 README가 기준이다.

## 라이선스

이 저장소는 `LICENSE`의 MIT 라이선스를 따른다. Copyright (c) 2026 dbc-hbin.

`external/nvapi/License.txt`, `external/nvngx/LICENSE.txt`, `include/native/directx/COPYING.MinGW-w64.txt`, `src/util/dxvk.LICENSE`는 vendored dependency의 원본 라이선스이며 그대로 유지한다. 각 파일 상단의 원저작자 copyright 표기는 출처 표기로 남겨 두었다. Git은 새 `main` 저장소로 초기화했으며 remote 추가와 push는 하지 않았다.
