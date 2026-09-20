# Owned D3D12 resource / view / heap contract 보정

2026-09-19 기준 project-owned D3D12 translator의 resource/view/heap/copy 경로를
DXMT D3D11 구현, Microsoft D3D12 public contract, Apple Metal heap contract와 대조해
active-compatible correctness bug를 보정했다. GPU/Wine/game 실행은 하지 않았다.

## 정확한 baseline과 범위

작업 원본은 prime의
`build/owned-completion-20260919/integration/source`이며 base commit은
`7c8dee1c2d73415301ceb7d1fa810861cef4cd67`이다. 최종 0012 rebase 시점의 live integration은
0001..0007 + 0009 D3D12 core contracts + 0010 temporal output/order + 0013 fence events를 포함한다.
기존 0008 NGX contracts는 prime이 rejected evidence로 보존하고 integration에는 적용하지 않았으므로
0012 baseline에서도 제외했다. live source hash를 authoritative baseline으로 사용했다.

새 isolated tree는 다음 두 개다.

- frozen: `build/owned-completion-20260919/resource-final-baseline`
- work: `build/owned-completion-20260919/resource-final-work`

둘 다 최종 integration/source를 byte-for-byte 복사했고 0012는 resource-final-work에만 적용했다.
정확한 pre-0012 hash와 patch provenance는
`build/owned-completion-20260919/resource-evidence/baseline.txt`에 기록했다.

0012는 다음 파일만 수정/추가한다.

- `src/d3d12/d3d12_buffer.cpp`
- `src/d3d12/d3d12_texture.cpp`
- `src/d3d12/d3d12_heap.cpp`
- `src/d3d12/d3d12_descriptor_heap.cpp`
- `src/d3d12/d3d12_descriptor_heap.hpp`
- copy-specific `src/d3d12/d3d12_command_list.cpp`
- new `src/d3d12/d3d12_resource_contract.hpp`

`d3d12_device.cpp`, pipeline, query heap/handlers, shared integration source와 lock은 수정하지 않았다.

## public contract / DXMT 근거

Microsoft D3D12의 public contract에서 확인한 핵심은 다음과 같다.

- placed resource offset은 resource allocation alignment의 배수여야 하고 allocation이 heap 범위 안에 있어야 한다.
- Metal placement도 `heapBufferSizeAndAlign` / `heapTextureSizeAndAlign`이 반환하는 alignment를 지켜야 한다.
- texture placed footprint offset은 512-byte aligned, RowPitch는 256-byte multiple이며 footprint가 parent buffer 안에 있어야 한다.
- planar depth/stencil footprint는 더 큰 valid RowPitch를 사용할 수 있다. prime이 official Microsoft DirectX-Headers
  pin `505594c6e8d8369d94a3bc31162823daef013467`의 D3DX12 resource helper와 format table을 확인한 결과
  R32G8X24 / D32_FLOAT_S8X24 계열은 planar로 분류되고 512-byte pitch를 선택한다. 0012는 이를 그대로 허용한다.
- typeless texture에 `Create*View(..., nullptr)`로 default view를 만들 수 없으며 explicit compatible typed format이 필요하다.
- 2D/2D-array SRV/UAV/RTV의 PlaneSlice는 resource plane 범위와 일치해야 한다.
- cube-array SRV는 `First2DArrayFace`와 `NumCubes`로 face 범위를 지정하고 cube start는 6-face boundary여야 한다.
- raw buffer SRV/UAV는 `R32_TYPELESS`, structured buffer는 `Format=UNKNOWN + StructureByteStride`, typed buffer는 typed format을 사용한다.
- UAV counter는 structured-buffer UAV에서만 사용하며 counter offset은 `D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT`를 지켜야 한다.
- CopyTextureRegion source box와 destination region은 각각 해당 subresource/footprint bounds 안에 있어야 한다.

DXMT D3D11 view helper도 cube-array의 `First2DArrayFace`, `NumCubes*6`을 그대로 Metal view range에 전달하고,
combined depth/stencil backing에서 typed depth/stencil view format으로 plane을 선택한다. 현재 DXMT format table은
`R32G8X24_TYPELESS` / `D32_FLOAT_S8X24_UINT`를 `Depth32Float_Stencil8` backing으로,
depth/stencil SRV plane을 각각 `R32X8X32` / `X32G8X32` Metal format으로 매핑한다.

## resource / view 수정

### placed buffer / texture

기존 코드는 `size + offset <= heap size`만 보았다. 0012는 overflow-safe range check와 함께
D3D12 resource alignment 및 Metal `heap*SizeAndAlign.align` 모두를 요구한다. Metal size query와 allocation은
기존 resource options를 그대로 사용해 `GetResourceAllocationInfo`와 다른 storage-mode requirement를 새로 만들지 않는다.

Metal heap 생성 실패도 이제 residency registration 전에 `E_OUTOFMEMORY`로 끝난다. 기존 residency model은 유지한다:
heap 자체가 residency set에 등록되고, heap-backed Buffer/Texture allocation은 `AllocatedOnHeap`으로 표시되어
heap allocation을 다시 independent residency allocation처럼 등록하지 않는다.

### buffer descriptors

buffer SRV/UAV는 element offset/length multiplication과 parent buffer range를 overflow-safe하게 검증한다.
internal BufferSlice가 32-bit byte range이므로 표현할 수 없는 >4GiB slice는 truncate하지 않고 `E_NOTIMPL`로 거절한다.

raw/structured/typed descriptor 규칙을 분리했고 raw는 `R32_TYPELESS`, structured는 `Format=UNKNOWN`, typed는
explicit typed format을 요구한다. UAV counter는 structured UAV에서만 허용하고 4096-byte placement alignment와
counter resource bounds를 확인한다. descriptor-heap internal counter offset도 `UINT`에서 `UINT64`로 넓혀
public 64-bit `CounterOffsetInBytes`를 truncate하지 않는다.

buffer RTV는 이전 `IMPLEMENT_ME` 후 `S_OK` 경로를 제거해 internal `E_NOTIMPL`, buffer DSV는 `E_INVALIDARG`로
끝낸다. 지원하지 않는 descriptor를 성공했다고 가장하지 않는다.

### texture mip / array / plane views

SRV/UAV/RTV/DSV의 mip와 array range를 parent resource bounds에 맞게 검증한다. `UINT_MAX` remainder form은
`total - first`로 계산하므로 non-zero first slice에서 full array length를 잘못 사용하는 기존 RTV/DSV bug도 없앴다.

CubeArray SRV는 기존에 빠졌던 `firstArraySlice = First2DArrayFace`를 설정하고 6-face boundary 및
`NumCubes*6` 범위를 검증한다.

2D/2D-array SRV/UAV/RTV는 PlaneSlice를 검증한다. combined depth/stencil resource에서는 depth typed view가 plane 0,
stencil typed view가 plane 1이어야 한다. typeless resource의 implicit/default view는 거절하고 explicit typed format을
요구한다. DSV는 depth-capable format과 valid DSV flags를 확인한다.

3D UAV는 현재 Metal view layer가 W-slice subrange를 표현하지 않으므로 full depth range만 지원한다.
partial FirstWSlice/WSize는 silent ignore 대신 internal `E_NOTIMPL`이다. 3D RTV는 W range를 검증하고 실제
RenderTargetArrayLength에 전달한다.

## descriptor heap 수정

shader-visible descriptor/sampler backing allocation과 mapped pointer failure를 확인해 `E_OUTOFMEMORY`로 처리한다.
CPU-side descriptor storage는 uninitialized `malloc` 대신 zero-initialized `calloc`을 사용한다.

typed buffer SRV의 GPU storage를 UAV union member 이름으로 쓰던 코드는 SRV member로 수정했다.
layout은 같더라도 descriptor kind와 일치하는 storage를 사용한다.

null resource SRV/UAV는 valid non-null view description이 있을 때 zero/null descriptor를 만든다.
resource와 descriptor가 모두 null인 invalid call은 internal `E_INVALIDARG`이며 descriptor memory는 null state로 남긴다.

## copy bounds / footprint 수정

`CopyBufferRegion`은 source/destination 모두 buffer인지, offset+length가 각 parent buffer 안에 있는지 확인한 뒤에만
blit encoder를 만든다.

`CopyTextureRegion`은 다음을 encoding 전에 검증한다.

- copy-location type과 resource 존재 여부
- texture subresource mip/array/plane index 범위
- placed footprint parent resource가 buffer인지
- footprint offset 512 alignment, RowPitch 256 multiple, minimum row bytes, parent buffer range
- source box가 source extent 안인지
- destination origin + copy extent가 destination extent 안인지
- BC block-aligned source/destination boundaries
- 같은 resource의 invalid same-subresource / placed-footprint self copy

512-byte planar RowPitch는 256의 배수이므로 정상 통과한다. 256으로 강제하지 않는다.

placed-footprint → placed-footprint는 이전 unsupported/no-effect path 대신 실제 row/slice buffer copy command를 기록한다.
source/destination block width와 bytes-per-block이 일치해야 하며, validated footprint row/slice pitch로 offset을 계산한다.

`CopyResource`는 buffer size 또는 texture dimensions/mip/sample shape가 일치하는지 확인한 뒤 encode한다.
현재 translator가 구현하지 않은 reinterpret-format copy는 기존처럼 warning 후 encode하지 않는다.

## regression tests

새 native CPU test:

`graphics-translator/tests/d3d12-resource-contracts.test.cpp`

production `d3d12_resource_contract.hpp`를 직접 include해 다음을 검사한다.

- mip/array remainder range와 OOB
- D3D + Metal placed-resource alignment와 exact heap edge
- integer overflow-safe range/add/multiply
- footprint row/slice arithmetic와 parent buffer range
- valid RowPitch 256 및 512 acceptance
- invalid RowPitch 384, invalid 512 placement offset, one-byte footprint overrun rejection
- BC 4x4 block footprint row arithmetic

성공 marker:

`D3D12_RESOURCE_CONTRACTS_CPU_PASS ranges=1 placement=1 overflow=1 footprint=1 pitch512=1`

새 Windows PE fixture:

`graphics-translator/tests/owned-d3d12-resources.cpp`

public D3D12 API로 다음 case를 구성한다.

- misaligned placed buffer/texture `CreatePlacedResource` exact `E_INVALIDARG`, aligned control success
- `R32G8X24_TYPELESS` depth texture array+mips에서 depth plane0 SRV, stencil plane1 SRV, typed DSV
- typed / structured / raw buffer SRV와 structured UAV
- valid null texture SRV/UAV descriptors
- 4x4 BC1 texture tail mip(2x2, 1 block) upload→readback: `GetCopyableFootprints` padded 4x4 footprint과 null pSrcBox 복사, GPU readback 값 일치

성공 marker:

`OWNED_D3D12_RESOURCES_PASS owned_backend=1 placed_alignment=1 depth_planes=1 mip_array_views=1 typed_buffer_descriptors=1 null_descriptors=1 bc_tail_copy=1`

PE는 이 worker에서 실행하지 않았다. prime이 integrated runtime에서 CPU-side API regression으로 실행할 수 있다.

## build / validation

fresh Meson cross build:

```sh
env PATH=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/tools/bin:$PATH \
  build/owned-graphics-20260919/tools/bin/meson setup \
  build/owned-completion-20260919/resource-final-build \
  build/owned-completion-20260919/resource-final-work \
  --cross-file build/owned-completion-20260919/resource-final-work/build-win64.txt \
  -Dnative_llvm_path=/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/llvm \
  -Dwine_install_path=/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/wine-crossover/host \
  -Denable_d3d12=true -Denable_nvapi=true -Denable_nvngx=true \
  --prefix=/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-completion-20260919/resource-final-install \
  --buildtype=debugoptimized
```

D3D12 build는 사용자 요구대로 2 jobs로 수행했다.

```sh
cd build/owned-completion-20260919/resource-final-build
env PATH=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/tools/bin:$PATH \
  ninja -j2 src/d3d12/d3d12.dll.postproc
```

native CPU test:

```sh
c++ -std=c++20 -O2 -Wall -Wextra -Werror \
  -Ibuild/owned-completion-20260919/resource-final-work/src/d3d12 \
  graphics-translator/tests/d3d12-resource-contracts.test.cpp \
  -o build/owned-completion-20260919/resource-tests/d3d12-resource-contracts-final
build/owned-completion-20260919/resource-tests/d3d12-resource-contracts-final
```

PE compile-only validation:

```sh
/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
  --target=x86_64-w64-mingw32 -std=c++17 -O2 -Wall -Wextra -Werror -static \
  graphics-translator/tests/owned-d3d12-resources.cpp \
  -o build/owned-completion-20260919/resource-tests/owned-d3d12-resources-final.exe \
  -ld3d12 -luuid
```

0012 replay verification은 fresh `resource-final-verify` copy에서 `git apply --check` 후 실제 apply를 수행했고
working source와 7개 source file SHA-256이 byte-for-byte 일치했다.

최종 hash:

- `0012-d3d12-resources.patch`: `08fab031ee1f37ca2e903a40806eb5dea87684851a608ffa6f7893afdc994639`
- final `d3d12.dll`: `6d656941e0784ea3a373e943e6b818f724094b856d4ead020973a01d33f222e2`
- native CPU test binary: `8a5a3dc7ee6e5fded15430b6fe8c2535725eac49585c42ec905114027c3cd841`
- PE fixture: `a78b187d85300f985325d1e4253c264fcb08a442982e6fb3d28c371442695682`

## 남은 범위

`GetCopyableFootprints`는 worker2/prime 소유 `d3d12_device.cpp`이므로 0012에서 수정하지 않았다. official oracle에 따라
현재 512-byte combined depth/stencil planar pitch는 유지한다.

Tiled-resource APIs, `CopyTiles`, query/pipeline/device4, bundle 등 이 assignment 밖의 미구현 API도 변경하지 않았다.
Metal heap storage-mode policy도 `GetResourceAllocationInfo`에 heap type input이 없는 상태에서 source별 size/alignment를
다르게 만들 수 있으므로 이번 patch에서 추측으로 바꾸지 않았다.
