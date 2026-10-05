# D3D12 포맷 capability 정책

2026-10-02 루트 `src/d3d12/`의 format 지원 보고·resource/view 실행 계약을 기록한다. 전역 feature level/Shader Model/tier와 남은 후보는 [현재 상태](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태), 실제 실행의 가용성은 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)를 따른다. 이 갱신은 소스 대조이며 새 GPU 실행이 아니다. Import 당시 모든 format에 `0xffffffff`를 반환하던 정책은 폐기됐고 frozen patch는 최신 루트 구현을 대신하지 않는다.

Source authority: [`d3d12_format_support.hpp`](../src/d3d12/d3d12_format_support.hpp)의 `BuildD3D12FormatSupport`, [`d3d12_device.cpp`](../src/d3d12/d3d12_device.cpp)의 `CheckFeatureSupport`, [`d3d12_resource_helper.cpp`](../src/d3d12/d3d12_resource_helper.cpp)의 backing/sample 판정, [`d3d12_texture.cpp`](../src/d3d12/d3d12_texture.cpp)의 view validation이다. [Resource 계약](D3D12-RESOURCES.ko.md)과 함께 읽는다.

## 판정 입력과 경계

- `MTLQueryDXGIFormat`: DXGI→Metal pixel/vertex format, byte size, typeless·BC·depth/stencil·backbuffer 속성.
- `FormatCapabilityInspector`: 현재 Metal device의 filter/write/color/blend/MSAA/resolve/atomic 및 typed-buffer 접근 capability.
- D3D12 계약: resource layout과 shader view의 역할, DXGI bit-layout family, 실제 compiler/encoder 경로. Native mapping이 존재한다는 이유만으로 모든 연산을 허용하지 않는다.

일반 uncompressed format은 native texel size와 DXGI byte size가 맞아야 직접 접근한다. `R32G32B32_*` texture 경로는 16-byte RGBA32 backing과 12-byte 논리 footprint 사이를 변환하며, typed-buffer SRV는 12-byte stride의 원시 buffer load를 사용한다. 없는 alpha는 논리적 1로 component mapping과 합성한다. RGB32 texture는 UAV/depth flags를 허용하지 않는다. IA/SO 지원은 native typed texture와 독립적이다. BC와 depth/stencil은 block/planar layout으로 별도 판정하며 BC는 장치 지원도 필요하다.

`DXGI_FORMAT_UNKNOWN`의 format query는 `BUFFER`만 반환한다. Unmapped format은 mask를 0으로 초기화한 뒤 `E_INVALIDARG`를 반환한다. Mapped지만 지원하지 않는 format은 0 mask를 반환할 수 있다.

## Resource·view 역할

| 구분 | 계약 |
|---|---|
| SO | 1–4개 32-bit FLOAT/SINT/UINT component의 12종에 `SO_BUFFER`. Native typed texture 지원과 독립적이다. VS/DS 및 GS stream 0–3 실행 경로가 있으며 stage·indirect·statistics 경계는 canonical 상태 문서를 따른다. 과거 VS→SO stream 0만의 제한은 현재 지원 설명이 아니다. |
| Mip | Texture-compatible format의 `MIP`는 filtering 여부와 독립적이다. Integer, typeless, depth/stencil의 mip storage도 포함한다. |
| Typed cast | `GetFormatFamily`의 DXGI bit-layout family를 사용한다. Fully typed resource도 호환 typed view를 가질 수 있으며, 완전 typeless resource만의 기능이 아니다. |
| 완전 typeless | Resource layout·mip·cast를 나타내며 그 format 자체로 shader/RTV/DSV/UAV를 허용하지 않는다. |
| Typed DSV | `D16_UNORM`, `D32_FLOAT`, `D24_UNORM_S8_UINT`, `D32_FLOAT_S8X24_UINT`는 shader load/sample/comparison/gather view가 아니다. MSAA render-target capability는 depth target도 포함하지만 이 format 자체에 MSAA shader load를 보고하지 않는다. |
| Depth/stencil plane SRV | `R24_UNORM_X8_TYPELESS`, `R32_FLOAT_X8X24_TYPELESS`, `X24_TYPELESS_G8_UINT`, `X32_TYPELESS_G8X24_UINT`는 이름에 TYPELESS가 있어도 shader가 읽는 구체적인 aspect view다. Combined DSV format과 구분한다. |
| Depth comparison | Shader depth 역할은 native storage flag가 아니라 `R16_UNORM`, `R32_FLOAT`, `R24_UNORM_X8_TYPELESS`, `R32_FLOAT_X8X24_TYPELESS` alias로 판정한다. Stencil aspect는 comparison 대상이 아니다. |
| Typed buffer | Native byte layout과 read/write capability를 검사한다. RGB32 SRV는 native buffer texture로 가장하지 않고 별도 원시 load·bounds·component mapping 경로를 사용한다. |
| sRGB UAV | D3D12의 RGBA8/BGRA8 sRGB UAV를 보고하지 않고 생성도 거부한다. UNORM reinterpretation으로 우회하지 않는다. Invalid descriptor는 기존 null descriptor 경로를 따른다. |
| Packed 4:2:2 | `R8G8_B8G8_UNORM`, `G8R8_G8B8_UNORM`은 두 pixel이 R/B를 공유하는 4-byte pair를 RGBA8 backing으로 풀고 다시 묶는다. Native packed RGB의 다른 변환 의미를 사용하지 않는다. 기본 resource width는 짝수이며 작은 mip의 홀수 tail을 처리한다. CPU·GPU·predicated copy, filtering, cube-array와 STANDARD_SWIZZLE pair layout 경로가 있으며 MSAA/RTV/DSV/UAV flags는 허용하지 않는다. |

`BUFFER`/IA index·vertex/texture dimension/RTV/blending/display/typed UAV/atomic 보고는 해당 mapping·장치 capability와 D3D12 역할을 함께 사용한다. `OutputMergerLogicOp=TRUE`이며 renderable UINT format에 logic-op bit를 보고한다. 실제 PS epilogue는 framebuffer fetch로 연산하고 UINT component 폭을 반영한다. Non-UINT target에는 일반 PS output을 유지하며 blend와 logic-op 동시 활성화 또는 independent-blend logic-op는 거절한다. Native Metal logic-op flag만으로 지원하는 구조가 아니다.

Tiled 보고는 native `supportsPlacementSparse()`를 조건으로 한다. Texture-compatible typed single-plane·비emulated storage에 표준 64 KiB sparse tile shape가 맞을 때 `FORMAT_SUPPORT2_TILED`를 올린다. Options tier와 개별 format bit, 실제 reserved resource 생성 조건을 함께 확인한다. Reserved dimension/sample/packed-tail 제약과 공개 tier는 [canonical 지원표](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태)를 따른다. 개별 format bit나 native sparse 생성만으로 상위 tier를 광고하지 않는다.

Depth dimension은 1D를 one-row 2D backing으로, cube/cube-array를 2D array의 view로 표현한다. 부분 3D UAV의 W-slice offset·bounds는 shader load/store/atomic과 bounded clear에 전달한다. 이 경로들의 과거 공개 GPU 관찰은 아래 역사적 기록에 분리했으며 “통합 검증 전”인 import 설명은 현재 판정이 아니다.

## D24 plane 복사

DXGI R24G8 depth plane의 footprint는 `R32_TYPELESS` family의 4-byte texel이지만 **내용은 하위 24-bit UNORM 정수와 0인 상위 X8**이다. IEEE float32 데이터라는 뜻이 아니다. Stencil plane은 `R8_TYPELESS` family의 1-byte texel이다.

Metal의 `Depth32Float_Stencil8` backing과 경계를 맞추기 위해 D3D12 buffer→texture에서 low24를 float로 변환하고, texture→buffer에서는 nearest-even 정수 변환을 수행한다. 정밀 나눗셈과 2의 거듭제곱 분해를 사용해 24-bit 경계값 및 `.1/.3/.9` clear를 보존한다. Conversion scratch와 command 소유권은 GPU retirement까지 유지한다. D32S8은 float plane blit를 유지한다. 두 family 모두 footprint format을 native texture view format으로 오인한 `R32Uint`/`R8Uint` view 생성을 피한다.

Depth/stencil `CopyTextureRegion`의 유효 계약은 전체 subresource rectangle, destination XYZ=0, source box=null이다. Fixture는 비정렬 depth subrectangle을 정상 사용 사례로 취급하지 않는다.

## Sample-count 조회와 실행

- `IsD3D12TextureSampleSupported`를 quality query, committed/placed 생성(null-output 포함), allocation-info, graphics PSO 검증에 사용한다. RGB32는 확장한 RGBA32 backing을 기준으로 판정한다. Packed 4:2:2 MSAA는 허용하지 않는다.
- MSAA count는 compiler/replay의 2/4/8 한계, native 장치 count, 실제 backing pixel format의 MSAA capability를 모두 만족해야 한다. BC MSAA는 거부한다. MSAA UAV는 OPTIONS14 `WriteableMSAATexturesSupported=TRUE`와 함께 sample별 shadow/GPU synchronization으로 지원하며 [runtime-06 양 ABI80-case proof](CORE-IMPLEMENTATION-STATUS.ko.md#writable-msaa-완료와-focused-proof)를 따른다. Typeless query는 가능한 typed interpretation을 확인하며, 실제 resource는 `ALLOW_DEPTH_STENCIL` 등 flags로 선택한 동일 backing을 생성·검증에 사용한다.
- 지원하는 ordinary quality level은 1개(`Quality=0`)다. Multisampled resource/PSO는 `0xffffffff` standard pattern도 허용한다. `0xfffffffe` center pattern은 native 장치의 programmable sample-position 지원을 확인한 뒤 모든 sample을 `(0.5, 0.5)`에 배치한다. Quality 0으로 대체하지 않으며 sample ID·mask는 각각 유지한다. Center pattern의 sample-frequency PS·antialiased line 조합은 D3D 계약에 따라 거절한다. Single-sample은 quality 0만 허용한다.
- Quality query의 `Flags` 입력을 보존한다. `TILED_RESOURCE` query는 reserved MSAA 지원 여부에 따라 판정하며 tiled tier만으로 ordinary MSAA quality를 복사하지 않는다. Count 0은 `E_FAIL`/0, 정의되지 않은 flags는 `E_INVALIDARG`/0, 유효하지만 미지원 조합은 `S_OK`/0을 반환한다.
- R32_UINT 및 RGBA32_UINT/SINT MSAA correction은 Apple9 이상에 한정한 D3D12-local 판정이다. D3D11 inspector table을 이 설명으로 확대하지 않는다. 실제 GPU 증거는 아래 M5 Pro에 한정한다.
- `MULTISAMPLE_LOAD`는 shader view, `MULTISAMPLE_RENDERTARGET`는 color/depth target 역할을 따른다. Integer/depth의 region MIN/MAX resolve가 가능하다는 이유로 `MULTISAMPLE_RESOLVE`를 올리지 않는다. 이 bit는 기존 typed color의 native resolve capability를 따른다.

## 역사적 실행 기록

아래는 2026-09-29 전후 실제 관찰한 결과이며 이 문서 갱신에서 다시 실행하지 않았다. 이전 `build/local/` runtime·로그·JSON은 2026-10-02 삭제로 현재 유실됐다. 현재 retained recovery proof와 mixed-cut 범위는 [검증 범위](CORE-IMPLEMENTATION-STATUS.ko.md#검증-범위와-증거)에서 별도로 확인한다. 과거 path를 현재 artifact 보유로 읽지 않는다.

호스트 x86 실행 복구 후 격리 `p1-complete/runtime-04`에서 RGB32 FLOAT resolve·FLOAT/UINT/SINT MSAA load, paired 4:2:2, STANDARD_SWIZZLE, DXBC 부분 3D UAV, depth dimension의 공개 GPU/API 검사를 관찰했다. DXIL 부분 3D UAV write→load 가시성 결함은 AIR texture fence 보정 후 `runtime-05`에서 load202·OOB0·atomic-before85·OOB-atomic0·slice guard·bounded clear·dimensions를 통과했다. 같은 runtime에서 BC1↔RGBA16UINT bitcast exact 32 bytes와 packed cube-array RGBA77/120/120/255를 확인했다. 이는 해당 fixture 범위이지 전체 format/operation conformance가 아니다.

Center pattern은 새 `build/local/p1-complete/runtime-08`에서 Metal API/Shader Validation을 통과했다. `center-final08-v2`는 2/4/8 sample의 full/masked RTV, centroid, D32 depth, occlusion·MAX resolve와 reordered semantic RGBA를 확인한다. `center-dxil-final08-v1`은 resource와 render-target quality가 독립적인 7개 조합의 `GetSamplePosition`을 확인한다. Unix-call 213의 x86/x64 layout assertion도 통과했으나 실제 32-bit GPU 실행은 검증하지 않았다.

아래는 확장 storage를 추가하기 **이전** M5 Pro/macOS 27의 격리 Wine runtime/prefix에서 Metal API 및 Shader Validation으로 실행한 증거다. Packed/RGB32 거부 항목과 query 개수는 당시 runtime에만 해당하며 새 기능의 완료 증거가 아니다. Query matrix와 실제 shader/readback 검증 범위를 구분한다.

| 검증 | 결과·증거 |
|---|---|
| SO capture·IA 재입력 | FLOAT/SINT/UINT 12종의 원시 bit, filled-size counter, stride와 guard 검증. `build/local/so-format-support/proof-02` |
| Format query matrix | 98종 query 중 texture2D 92, buffer 54, compressed 21. 이것은 98종의 모든 GPU 연산 검증이 아니다. `build/local/format-support/proof-05` |
| Shader/readback | Depth/stencil aspect 읽기, integer/typeless mip, depth mip 보고, RGBA8 linear/sRGB 및 R16 UINT→FLOAT·RGBA32 UINT→FLOAT·BC1 linear→sRGB reinterpretation. 같은 `proof-05` |
| 미지원 연산 | RGBA8/BGRA8 sRGB UAV의 64-byte sentinel 보존, packed 4:2:2 생성 거부·pair footprint·오류 출력 검사. 같은 `proof-05` |
| Native cast compatibility | R8/R16/RG16/RGBA8/RGBA16/RGBA32/RGB10A2/BGRA8/BC1/BC4/BC5/BC6의 13개 native view 조합 생성. GPU shader oracle와는 별도 증거다. `build/local/format-support/native-casts.log` |
| Single-plane depth 역할 | 4종 typed DSV의 shader bit 금지, 4종 depth alias의 comparison 보고, typeless R32/R16의 DSV clear 뒤 R32_FLOAT/R16_UNORM `Load`와 서로 반대 결과의 `SampleCmpLevelZero`. `build/local/format-support/single-depth-after-02` |
| D24/D32S8 copy | Typed/typeless 4종, 독립 clear·upload·shader load·texture copy, 8×2 padded row, 비영 placed offset, mip/array, low24 경계와 guard. `build/local/d24-plane-runner-03` |
| Sample-count 정합성 | 45개 quality query, 15개 거부 descriptor 조합의 committed/placed/null-output/allocation 검사, 2×/4×/8× 21개 GPU case. Standard-pattern edge coverage, sample별 load, region MAX 및 depth backing을 검증했다. `build/local/msaa-capability-probe-01/runner-msaa-final-01`, runtime `msaa-capability-runtime-02` |
| 기존 계약 회귀 | `resources`, `copy-resolve`, `capabilities` 모두 통과. `build/local/format-integration/{resources,copy-resolve,capabilities}-01` |
| 최종 shared preflight·D24 lazy PSO | `msaa-capability-runtime-01`에서 `build/local/format-integration/{depth,resources,copy-resolve}-final-01` 통과. D24 kernel PSO는 사용하는 변환 방향만 lazy 생성한다. |

이전 runtime에서 mip/cast/aspect 보고 누락, packed footprint 오류·잘못된 생성 성공, D24 float bit readback과 invalid native plane view를 재현했다. D24 경계 upload에서 발견한 1-LSB 반올림 오류도 수정 후 회귀에 포함했다. 순수 helper에 인위적인 속성을 넣어 bit를 복사 확인하던 `format-capabilities.cpp`는 삭제하고 공개 API/GPU fixture로 대체했다.

## 새 격리 실행 예시

```sh
set -eu
export LLVM_MINGW=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin
export RUN_ROOT=build/local/capability-contract-new
export RUNTIME=build/local/capability-runtime-new
# README 절차로 RUNTIME을 새로 staging한 뒤 실행한다.
test ! -e "$RUN_ROOT"
mkdir -p "$RUN_ROOT/fixtures"
for fixture in format-support depth-plane-copy so-format-support multisample-capabilities; do
  "$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
    "graphics-translator/tests/owned-d3d12-$fixture.cpp" \
    -ld3d12 -ldxgi -ld3dcompiler -o "$RUN_ROOT/fixtures/owned-d3d12-$fixture.exe"
  python3 scripts/test-owned-graphics.py --runtime "$RUNTIME" \
    --suite "$fixture" --fixture "$RUN_ROOT/fixtures/owned-d3d12-$fixture.exe" \
    --out "$RUN_ROOT/$fixture" --validation
done
```

이 예시는 repo root에서 새 runtime/output을 사용하는 재실행 절차이며 새 PASS 보고가 아니다. Runner는 output 아래 새 prefix를 만든다. Live game 설치나 prefix는 사용하지 않는다. Windows reference differential, 다른 Metal GPU family/OS, 실제 게임의 호환성·성능은 위 역사적 검증 범위가 아니다. 현재 단계와 후속 결과는 [구현 상태 문서](CORE-IMPLEMENTATION-STATUS.ko.md#현재-지원-상태)를 기준으로 한다.
