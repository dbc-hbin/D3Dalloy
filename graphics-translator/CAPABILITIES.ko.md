# D3D12 포맷 capability 정책

이 문서는 owned graphics translator의 `D3D12_FEATURE_FORMAT_SUPPORT` 구현 근거와 현재 광고 범위를 기록한다. 기준 DXMT 소스는 `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`이며, DirectX header submodule은 `9df86f2341616ef1888ae59919feaa6d4fad693d`이다.

## 문제

기준 DXMT의 D3D12 `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)`는 `DXGI_FORMAT_UNKNOWN`만 `D3D12_FORMAT_SUPPORT1_BUFFER`로 처리하고, 나머지 모든 포맷에 `Support1 = 0xffffffff`, `Support2 = 0xffffffff`를 반환한다. 이 값에는 현재 D3D12 backend가 구현하지 않은 stream output, tiled resources, logic op, video 경로와 실제 Metal 포맷이 지원하지 않는 RT/UAV/atomic capability까지 포함된다.

DX11 구현에는 이미 `MTLQueryDXGIFormat`과 `FormatCapabilityInspector`를 이용해 Metal device family별 포맷 capability를 판정하는 경로가 있다. D3D12 device도 초기화 시 동일 inspector를 채우고 `GetMTLPixelFormatCapability()`를 제공하므로, 새 D3D12 경로는 이 데이터를 직접 사용한다.

## 판정 입력

패치는 세 종류의 정보를 함께 사용한다.

1. `MTLQueryDXGIFormat`의 DXGI→Metal mapping: PixelFormat, AttributeFormat, byte size, typeless/BC/backbuffer/depth/stencil flags.
2. 현재 Metal device에서 `FormatCapabilityInspector::Inspect()`가 채운 capability: Filter, Write, Color, Blend, MSAA, Resolve, Atomic, TextureBufferRead/Write/ReadWrite 등.
3. 현재 D3D12 구현의 실제 경로: texture dimension 제약, SRV/UAV/RTV/DSV 생성, resolve encoder, IA index/vertex 처리, stream-output 미구현 상태.

일반 uncompressed 포맷은 `MTLGetTexelSize(PixelFormat) == DXGI BytesPerTexel`일 때만 texture/buffer layout-compatible로 본다. 따라서 `R32G32B32_*`처럼 12-byte DXGI 포맷이 단일 R32 Metal pixel format으로 매핑되는 경우는 IA vertex attribute는 허용할 수 있지만, 해당 mapping만으로 1D/2D/3D texture나 typed buffer를 지원한다고 광고하지 않는다.

BC와 depth/stencil은 block/planar layout 때문에 위 byte-size 비교에서 제외하고, 실제 inspector entry가 있는 경우에만 texture-compatible로 본다. 특히 BC mapping이 존재해도 `supportsBCTextureCompression()` 결과로 inspector capability가 생성되지 않은 device에서는 BC texture 지원을 광고하지 않는다.

## Support1

`DXGI_FORMAT_UNKNOWN`은 기존 동작을 유지해 `BUFFER`만 반환한다. 다른 unmapped format은 mask를 먼저 0으로 만들고 `E_INVALIDARG`를 반환한다.

Mapped format은 다음 범위만 광고한다.

| 비트 | 조건 |
| --- | --- |
| `BUFFER` | BC/depth/stencil이 아니고 native texel layout이 맞으며 TextureBuffer inspector capability가 있음 |
| `IA_VERTEX_BUFFER` | typed format이고 `AttributeFormat` mapping이 있음 |
| `IA_INDEX_BUFFER` | typed `R16_UINT` 또는 `R32_UINT` |
| `TEXTURE1D` | inspected texture support가 있고 BC/depth/stencil이 아님 |
| `TEXTURE2D` | inspected texture support가 있음 |
| `TEXTURE3D` | inspected texture support가 있고 BC/depth/stencil이 아님 |
| `TEXTURECUBE` | inspected texture support가 있고 depth/stencil이 아님 |
| `CAST_WITHIN_BIT_LAYOUT` | texture-compatible typeless mapping |
| `SHADER_LOAD` | typed texture-compatible format |
| `SHADER_SAMPLE`, `SHADER_GATHER`, `MIP` | 위 조건에 더해 inspector `Filter`가 있음 |
| `SHADER_SAMPLE_COMPARISON`, `SHADER_GATHER_COMPARISON` | typed depth format |
| `RENDER_TARGET` | inspector `Color` |
| `BLENDABLE` | inspector `Blend` |
| `DEPTH_STENCIL` | depth 또는 stencil mapping이며 inspector texture support가 있음 |
| `MULTISAMPLE_RESOLVE` | inspector `Resolve`와 `Color`가 둘 다 있음 |
| `MULTISAMPLE_RENDERTARGET` | inspector `MSAA`와 `Color`가 둘 다 있음 |
| `MULTISAMPLE_LOAD` | inspector `MSAA` |
| `DISPLAY` | backbuffer mapping이며 inspector `Color`가 있음 |
| `TYPED_UNORDERED_ACCESS_VIEW` | typed/non-BC/non-DS이고 texture `Write` 또는 typed-buffer store capability가 있음 |

`SO_BUFFER`는 광고하지 않는다. 현재 D3D12 graphics pipeline은 `StreamOutput.NumEntries != 0`이면 `E_NOTIMPL`을 반환하고 `SOSetTargets`도 미구현이다.

Depth/stencil `MULTISAMPLE_RESOLVE`는 현재도 capability에서 광고하지 않는다. 다만 이 문서 작성 당시와 달리 현재 루트에는 `ResolveSubresourceRegion`과 depth/stencil MIN/MAX shader resolve가 구현되어 있다. `BuildD3D12FormatSupport`는 여전히 `properties.resolve && properties.color`인 경우에만 `MULTISAMPLE_RESOLVE`를 설정하므로, depth/stencil resolve의 실제 지원 범위와 capability 보고를 별도 matrix로 검증해 일치시키기 전까지 보수적으로 비광고 상태를 유지한다.

Typeless format은 resource layout/casting 용도의 `BUFFER`/texture dimension/`CAST_WITHIN_BIT_LAYOUT`까지만 가능하고, 해당 typeless format 자체에 shader load, RT, DSV, UAV 같은 typed-view capability를 부여하지 않는다.

## Support2

`UAV_TYPED_LOAD`와 `UAV_TYPED_STORE`는 각각 inspector의 `TextureBufferRead`/`TextureBufferWrite`/`TextureBufferReadWrite`에서 가져온다. `Atomic` capability가 있고 typed UAV 자체가 가능한 포맷에만 다음 6개 atomic 비트를 함께 광고한다.

- `UAV_ATOMIC_ADD`
- `UAV_ATOMIC_BITWISE_OPS`
- `UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE`
- `UAV_ATOMIC_EXCHANGE`
- `UAV_ATOMIC_SIGNED_MIN_OR_MAX`
- `UAV_ATOMIC_UNSIGNED_MIN_OR_MAX`

`OUTPUT_MERGER_LOGIC_OP`는 광고하지 않는다. 현재 D3D12 options가 `OutputMergerLogicOp = FALSE`다. `TILED`도 광고하지 않는다. 현재 options가 `TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED`이고 tiled command 경로도 구현되지 않았다. `MULTIPLANE_OVERLAY` 및 기타 이 구현에서 근거가 없는 Support2 비트도 0으로 둔다.

## 검증

`graphics-translator/tests/format-capabilities.cpp`는 helper 자체를 CPU-only로 컴파일해 다음 구분을 확인한다.

- `UNKNOWN`과 unmapped format
- 일반 filterable color/RT/UAV format
- typeless format
- BC compressed format 및 mapping만 있고 inspector support가 없는 BC format
- depth format과 color-only resolve 제한
- `R32_UINT` typed UAV/atomic
- 96-bit `R32G32B32_FLOAT`의 IA-only 처리
- SO/tiled/logic-op 비트의 비광고

검증에 사용한 명령:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror \
  -Ibuild/owned-graphics-20260919/capability-work/include/native/windows \
  -Ibuild/owned-graphics-20260919/capability-work/include/native/directx \
  -Ibuild/owned-graphics-20260919/capability-work/src/d3d12 \
  graphics-translator/tests/format-capabilities.cpp \
  -o /tmp/dxmt-format-capabilities-test
/tmp/dxmt-format-capabilities-test
```

Helper 테스트와 `git diff --check`는 통과했다. `src/dxmt/dxmt_format.cpp`도 동일 scratch tree/header로 syntax-only compile을 통과했다.

전체 `d3d12_device.cpp`의 standalone native syntax compile은 기준 checkout의 header 조합 때문에 완료할 수 없었다. `include/native/directx`의 고정 submodule revision에는 이 WIP D3D12 source가 참조하는 `ID3D12Device4`, `ID3D12Fence1`, `D3D12_PIPELINE_STATE_STREAM_DESC` 등 newer D3D12 선언이 없어서 capability 변경부에 도달하기 전에 baseline type 오류가 난다. 이 제약은 helper CPU 테스트와 별개이며, 실제 translator build에서는 프로젝트가 사용하는 cross-build header/toolchain 조합으로 다시 확인해야 한다.

현재 prime cross build metadata에는 `x86_64-w64-mingw32-g++` clang 22.1.8 compile command가 기록되어 있지만, worker shell의 PATH에서는 해당 wrapper를 찾을 수 없어 (`command not found`) scratch `d3d12_device.cpp`를 같은 cross command로 재컴파일하지 못했다. 이는 source compiler error가 아니라 이 worker 프로세스의 toolchain executable 가시성 제한이다.

이 검증은 capability 산출 정책과 compile-time 계약만 확인한다. 실제 게임 호환성, Metal resource 생성 성공률, GPU correctness 또는 D3D12 conformance를 증명하지 않는다.
