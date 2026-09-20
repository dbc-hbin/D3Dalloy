# Owned D3D12 temporal COM backend fixture

이 fixture는 NGX frontend를 거치지 않고 owned D3D12 COM extension을 직접 호출한다. 목적은 D3D12 command recording/replay와 MetalFX temporal backend 경계를 먼저 독립 검증하는 것이다. NGX, GPTK, D3DMetal native/private ABI를 사용하지 않는다.

## ABI provenance

테스트 파일 graphics-translator/tests/owned-d3d12-temporal.cpp에는 현재 candidate src/d3d12/d3d12_interfaces.hpp의 public ABI를 그대로 복사했다.

- IMTLD3D12TemporalUpscaleState: c8a9134e-f27c-4e75-909f-f14439b918e1
- IMTLD3D12DeviceExt: ed85c4e9-e8fd-491b-9595-d9a369ec12b2
- IMTLD3D12GraphicsCommandListExt: d7bc6ed0-6a8a-4a0e-8408-5a26c0b06d0e
- ListExt vtable 순서: TemporalUpscale(...), 마지막 슬롯 ValidateRecording()
- 두 descriptor 모두 첫 필드는 UINT StructSize

x86-64 fixture는 create desc 36 bytes, frame desc 144 bytes, frame Color pointer offset 8을 static_assert한다.

## flags74 typed baseline

Create contract:

- input 64x64
- logical output 128x128
- feature flags decimal 74 = MVLowRes | DepthInverted | AutoExposure
- caller output subrect base offsets는 0
- creation/visibility node mask 1

Frame resources:

- color: 64x64 DXGI_FORMAT_R8G8B8A8_UNORM, frame별 constant grayscale tag
- motion: 64x64 DXGI_FORMAT_R16G16_FLOAT, 모든 motion vector 0
- depth: 실제 DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DSV를 만들어 depth/stencil clear 후 shader-read state로 전이
- output: logical 128x128, backing resource 136x136 DXGI_FORMAT_R8G8B8A8_UNORM, UAV 허용
- optional exposure/bias/transparency resources는 null
- jitter 0/0, MV scale 1/1, semantic preExposure 1, exposureScale 1
- 첫 frame을 포함한 8 frame 모두 caller ResetHistory=FALSE

output backing 전체는 frame별 sentinel RGBA로 먼저 채운다. Temporal record 전에 COPY_SOURCE -> UNORDERED_ACCESS, record 후 UNORDERED_ACCESS -> COPY_SOURCE transition을 넣고 전체 136x136을 readback한다. logical 128x128 밖의 8-pixel right/bottom margin은 sentinel이 byte-exact 그대로여야 한다.

logical output은 RGBA8 전체를 읽어 다음을 확인한다.

- logical 영역의 최소 75% pixel이 sentinel에서 변경됨
- RGB mean이 finite이고 0.05보다 커 nonblack임
- constant input grayscale mean과 output RGB mean의 절대 차이가 0.20 이하

이 0.20은 image-quality tuning 값이 아니다. 기존 local MetalFX flags74 synthetic 결과가 constant/near-constant input mean을 보존한다는 사실에 맞춘 넓은 sanity bound이며 PSNR/샤프닝/감마/보정 pass를 요구하지 않는다.

각 continuous frame 로그는 input_tag, input_mean, output_tag=round(output_mean*255), output_mean, sameframe_delta를 같이 출력한다. 같은 frame 입력/출력 tag를 runtime trace와 대조할 수 있게 하기 위한 진단 값이며 temporal 품질 점수로 사용하지 않는다.

## actual-ZZZ typeless parent variant

typed baseline을 그대로 유지한 뒤 같은 flags74 계약을 typeless parent resource로 8 frame 더 실행한다.

- Color parent: DXGI_FORMAT_R8G8B8A8_TYPELESS (27)
- Output parent: DXGI_FORMAT_R8G8B8A8_TYPELESS (27), backing 136x136 / logical 128x128
- Motion parent: DXGI_FORMAT_R16G16_TYPELESS (33)
- Depth parent: DXGI_FORMAT_R32G8X24_TYPELESS (19)
- Depth view/clear: typed DXGI_FORMAT_D32_FLOAT_S8X24_UINT (20)

현재 format table에서 27은 RGBA8Unorm Metal pixel format으로, 33은 RG16Uint parent로, 19는 Depth32Float_Stencil8로 매핑된다. temporal backend의 motion normalization은 RG16Uint parent를 RG16Float view로 만든다. 따라서 이 variant는 typeless parent object를 실제 게임처럼 유지하면서 MetalFX가 요구하는 typed motion/depth 의미를 backend 내부 view에서 복구하는 경로를 검사한다.

typeless output도 typed baseline과 동일하게 logical 128x128 밖의 sentinel을 byte-exact 보존해야 하며 별도 품질 보정이나 허용오차 변경은 없다. helper 함수는 typed format을 default argument로 유지하므로 이 파일을 rename-main include하는 NGX fixture의 기존 호출은 source-compatible하다.

## command recording / lifetime / replay

모든 temporal list는 먼저 QueryInterface(IMTLD3D12GraphicsCommandListExt) 후 열린 상태에서 ValidateRecording() 성공을 요구한다.

negative case보다 먼저 positive_valid_temporal_record를 실행해 동일 typed resource/state와 완전히 유효한 frame descriptor가 정확히 S_OK인지 확인한다. 이 control이 먼저 실패하면 이후 negative를 성공으로 세지 않고 즉시 종료하므로 early validation failure가 E_NOTIMPL/E_INVALIDARG 구분을 가리는 회귀를 막는다.

release_before_execute case는 temporal command와 output readback copy까지 list에 기록하고 Close한 뒤 caller의 IMTLD3D12TemporalUpscaleState reference를 Reset()하여 release한 다음 처음 Execute한다. 출력이 정상이어야 하므로 recorded payload/generation이 feature release와 무관하게 실행까지 retained되어야 한다.

그 동일 closed command list를 두 번째로 다시 Execute한다. caller reset은 두 실행 모두 false다. backend reset contract는 다음과 같다.

    execution 1: effectiveReset = true  (new generation first actual execution)
    execution 2: effectiveReset = false (same generation replay; implicit reset must not repeat)

COM API에는 effective reset query가 없으므로 fixture는 두 실행을 실제로 발생시키고 case log에 기대값을 남긴다. prime runtime validation은 owned backend trace의 generation/reset field와 이 case log를 대조해야 한다. 이미지 값에 맞춰 undocumented temporal accumulation 수치를 추정해 reset 여부를 판정하지 않는다.

별도 caller_reset closed-list replay case는 recorded ResetHistory=TRUE로 같은 list를 두 번 Execute한다. 이 경우 두 execution 모두 effective reset=true여야 한다. 이것이 implicit first-generation reset과 caller-requested reset의 차이를 분리한다.

현재 backend source에서 이 계약은 직접 확인된다. recorded frame은 shared_ptr Core/Generation과 retained D3D12 resources를 소유한다. queue encode 시 props.reset = recorded caller reset || !generation.initialized 로 계산하고, temporal encode와 output copy가 끝난 뒤에만 generation.initialized=true로 바꾼다. temporal_encode trace에는 caller_reset과 실제 reset 값이 같이 기록된다. 따라서 위 replay case는 source와 trace를 직접 대조할 수 있다.

현재 regression backend는 generation discontinuity까지 포함해 last_executed_generation을 사용한다. fixture는 같은 feature에 A(64x64 backing)와 B(80x80 backing)를 둘 다 실행 전에 먼저 기록한다. logical input content는 둘 다 64x64이고 depth/motion backing도 각 color backing과 동일하다.

실행 순서는 다음과 같고 reset은 이미지에서 추정하지 않고 backend trace와 비교할 expected tag로만 기록한다.

    A1 (G1): expected reset 1
    B1 (G2): expected reset 1
    A2 (G1 replay after G2): expected reset 1
    A3 (G1 consecutive replay): expected reset 0

각 실행은 자체 output readback/sentinel validation을 통과해야 한다. 현재 backend source는 props.reset = callerReset || last_executed_generation != generation.id 로 계산하고 encode 종료 시 last_executed_generation = generation.id 로 갱신하므로 기대 sequence는 정확히 1,1,1,0이다.

## negative cases

단순 FAILED 여부가 아니라 exact HRESULT를 요구한다.

| case | exact expected HRESULT |
| --- | --- |
| valid open-list control | S_OK |
| create flags에 unknown high bit 0x80000000 | E_NOTIMPL |
| Close된 list ValidateRecording() | E_FAIL |
| Close된 list TemporalUpscale() | E_FAIL |
| JitterOffsetX = NaN | E_INVALIDARG |
| ExposureScale = 2.0f | E_NOTIMPL |
| optional bias resource null + BiasBaseX = 1 | E_NOTIMPL |
| OutputBaseX = 1 | E_NOTIMPL |
| single-node ID3D12Device::CreateCommandList(NodeMask=2, DIRECT, directAllocator) | E_INVALIDARG |
| DIRECT allocator + COMPUTE command-list type mismatch | E_INVALIDARG |

현재 backend source에서 non-finite scalar는 E_INVALIDARG, non-identity ExposureScale/nonzero offsets는 E_NOTIMPL, unknown create flag는 E_NOTIMPL, closed list는 ValidateRecording에서 E_FAIL을 반환한다. allocator/list type mismatch도 E_INVALIDARG이며 single-node CreateCommandList는 NodeMask > 1을 E_INVALIDARG로 거부한다.

이 exact 비교 때문에 과거처럼 공통 early E_INVALIDARG가 여러 negative를 동시에 통과로 위장할 수 없다.

## regression build

기존 build/owned-graphics-20260919/temporal-build/owned-d3d12-temporal.exe와 run-temporal-01/02 evidence는 그대로 보존한다. regression 산출물은 새 경로만 사용한다.

    mkdir -p build/owned-graphics-20260919/temporal-regression-build

    /opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
      --target=x86_64-w64-mingw32 \
      -std=c++17 -O2 -Wall -Wextra -Werror -static \
      graphics-translator/tests/owned-d3d12-temporal.cpp \
      -o build/owned-graphics-20260919/temporal-regression-build/owned-d3d12-temporal-regression.exe \
      -ld3d12 -luuid

runtime에서는 먼저 d3d12.dll!OwnedGraphicsBackendId() == owned-graphics-d3d12를 요구한다. GPU wait는 각 fixture command submission마다 최대 15초로 bounded된다.

이 worker는 PE build만 수행하고 GPU 실행은 하지 않는다. 실제 sequential Metal/validation run은 prime 소유다.

helper signature에 backing/parent-format default argument를 추가했으므로 rename-main include를 쓰는 기존 NGX fixture도 별도 새 경로에서 재컴파일한다. NGX source 자체는 수정하지 않는다.

    mkdir -p build/owned-graphics-20260919/ngx-fixture-regression-build

    /opt/llvm-mingw-20260616-ucrt-macos-universal/bin/x86_64-w64-mingw32-g++ \
      --target=x86_64-pc-windows-msvc \
      -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
      -Ibuild/dlss-translator-20260918/contract/nvidia/include \
      -c graphics-translator/tests/owned-d3d12-ngx-msvc.cpp \
      -o build/owned-graphics-20260919/ngx-fixture-regression-build/owned-d3d12-ngx-msvc.obj

    /opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
      --target=x86_64-w64-mingw32 \
      -std=c++17 -O2 -Wall -Wextra -Werror -static \
      graphics-translator/tests/owned-d3d12-ngx.cpp \
      build/owned-graphics-20260919/ngx-fixture-regression-build/owned-d3d12-ngx-msvc.obj \
      -o build/owned-graphics-20260919/ngx-fixture-regression-build/owned-d3d12-ngx-regression.exe \
      -ld3d12 -luuid

두 PE 모두 CPU-side -Werror cross-compile을 통과해야 하며 GPU 실행은 하지 않는다.

최종 build-only 검증 결과:

- temporal regression PE: PE32+ x86-64
  - SHA-256 ac817e3a68a10403f3a620af05b92487c9f8a5143ba88f18c14722f474e4b090
- NGX include-compat regression PE: PE32+ x86-64
  - SHA-256 2e880da8c2925df2fec3f21c43eb5858d8905b60159e7c18c204af9197362b93
- NGX MSVC ABI helper object:
  - SHA-256 c29f8a5656182923ee2c2dcfb7608a779395a793a16cbc45cb8de34acf55a5e6

두 PE import table은 d3d12.dll의 D3D12CreateDevice와 Windows runtime만 포함한다. D3DCompiler, 정적 nvngx, GPTK, D3DMetal import는 없다.

보존 대상 기존 temporal-build PE의 SHA-256은 여전히 다음 값이다.

    06b678360d7b45eba7d16de7c8379b0afa157157a402b9440d4eeb77dea320f9

즉 regression build 과정에서 기존 temporal-build PE를 덮어쓰지 않았다.

## final PASS

모든 case가 통과하면 마지막에 다음 marker를 출력한다.

    OWNED_D3D12_TEMPORAL_PASS flags74=1 typed_frames=8 typeless_frames=8 release_before_execute=1 closed_list_replay=1 sentinel=1 negatives_exact_hr=1 positive_control=1 typeless_zzz=1 generation_transition=1 command_list_validation=1
