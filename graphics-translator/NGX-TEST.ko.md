# Owned nvngx.dll D3D12 frontend integration fixture

이 fixture는 nvngx.dll의 실제 D3D12 export를 통해 owned D3D12 temporal backend까지 들어가는 PE 통합 시험이다. direct COM-only fixture와 달리 NVSDK_NGX_D3D12_Init / AllocateParameters / CreateFeature / EvaluateFeature / ReleaseFeature / Shutdown1를 실제로 동적 호출한다.

## NVIDIA parameter ABI

NVSDK_NGX_Parameter는 C++ virtual interface이고 MSVC ABI를 사용하는 애플리케이션이 소비한다. MinGW C++ vtable 순서를 가정하지 않는다.

fixture는 다음 두 translation unit으로 분리한다.

- graphics-translator/tests/owned-d3d12-ngx.cpp: MinGW Windows PE 본체, D3D12 resource/queue/readback과 nvngx.dll export 호출 담당
- graphics-translator/tests/owned-d3d12-ngx-msvc.cpp: --target=x86_64-pc-windows-msvc로 빌드하며 공식 NVIDIA nvsdk_ngx.h를 include하고 NVSDK_NGX_Parameter::Set/Reset virtual call만 수행

공식 local header provenance:

- build/dlss-translator-20260918/contract/nvidia/include/nvsdk_ngx.h
  - SHA-256 dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848
- nvsdk_ngx_defs.h
  - SHA-256 ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
- nvsdk_ngx_params.h
  - SHA-256 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc

공식 nvsdk_ngx_params.h의 vtable 순서는 ULL/F/D/UI/I/D3D11/D3D12/void Set, 같은 typed Get, 마지막 Reset이다. fixture helper가 이 선언 자체로 컴파일되므로 자체 복제 vtable 선언이 없다.

MSVC helper는 공식 enum에서 result 값도 꺼내 C shim으로 제공한다. 따라서 main fixture의 expected error code도 hard-coded legacy 0x0bad... 값이 아니라 공식 NVIDIA 값이다.

- Success 0x00000001
- FeatureNotSupported 0xBAD00001
- FeatureNotFound 0xBAD00004
- InvalidParameter 0xBAD00005
- NotInitialized 0xBAD00007
- MissingInput 0xBAD0000A
- UnsupportedParameter 0xBAD00010

현재 final frontend source도 D3D12 entry point에 이 공식 값을 사용한다.

## loaded nvngx identity / native fallback 방지

fixture는 import table에서 nvngx.dll을 정적으로 링크하지 않는다. 런타임에 정확히 다음 경로를 요청한다.

    C:\windows\system32\nvngx.dll

로드 후 다음을 모두 요구한다.

1. GetModuleFileNameW 결과가 \windows\system32\nvngx.dll로 끝난다.
2. 로드된 파일 bytes 안에 final frontend marker own-ngxd3d12가 존재한다.
3. 다음 D3D12 export가 모두 존재한다.
   - Init / Init_Ext / Init_ProjectID / Init_with_ProjectID
   - AllocateParameters / DestroyParameters
   - CreateFeature
   - EvaluateFeature / EvaluateFeature_C
   - ReleaseFeature
   - Shutdown1

따라서 같은 이름의 다른 native fallback DLL이나 D3D12 export가 없는 구버전 shim을 성공으로 인정하지 않는다.

현재 final ngx-work-build/nvngx.dll export table에는 위 D3D12 entry point가 실제로 존재하고 binary marker도 확인했다.

## flags74 typed resource contract

resource 생성/readback helper는 CPU-validated owned-d3d12-temporal.cpp를 rename-main include해 재사용한다. temporal feature 호출 자체는 COM extension을 직접 부르지 않고 모두 nvngx.dll D3D12 API를 통해 수행한다.

Create:

- 64x64 input
- 128x128 logical output
- official NVSDK_NGX_Feature_SuperSampling
- NVSDK_NGX_PerfQuality_Value_Balanced
- flags 74 = MVLowRes | DepthInverted | AutoExposure

Evaluate resources:

- color: 64x64 DXGI_FORMAT_R8G8B8A8_UNORM, frame-specific constant source tag
- output: 136x136 RGBA8 UAV backing, logical 128x128
- depth: real DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DSV clear
- motion: 64x64 zero DXGI_FORMAT_R16G16_FLOAT
- jitter 0/0
- MV scale 1/1
- caller Reset=0
- raw DLSS.Pre.Exposure=0
- raw DLSS.Exposure.Scale=0
- render subrect dimensions 64x64

frontend source normalizes both raw zero exposure values to semantic 1.0 before the backend call:

    PreExposure = raw_pre_exposure == 0 ? 1 : raw_pre_exposure
    ExposureScale = raw_exposure_scale == 0 ? 1 : raw_exposure_scale

backend Prepare rejects non-positive PreExposure/ExposureScale, so a successful NGX Evaluate with raw zero values is also an integration proof that the frontend normalization reached the backend.

## continuous 8-frame path

하나의 NGX feature handle로 8 continuous frame을 실행한다.

각 frame은:

1. frame-specific constant grayscale color tag와 sentinel-filled 136x136 output을 만든다.
2. 새 NVSDK_NGX_Parameter를 Allocate한다.
3. MSVC helper가 official typed Set으로 color/output/depth/motion와 flags74 frame scalar를 채운다.
4. open D3D12 list에서 NVSDK_NGX_D3D12_EvaluateFeature를 호출한다.
5. Evaluate가 성공한 즉시 parameter Reset() 후 DestroyParameters()한다.
6. 그 뒤 command list를 Close/Execute한다.
7. output 136x136을 readback해 logical 128x128 finite/nonblack/source-mean loose sanity와 outside sentinel preservation을 확인한다.

parameter map은 Execute 전에 이미 파괴되어 있으므로 backend command payload가 caller parameter facade/storage를 빌려 쓰면 통과할 수 없다.

frame 로그는 input_tag, output_tag, sameframe_delta, expected effective reset, params_destroyed_before_execute=1, raw_exposure_zero_normalized=1을 남긴다.

## ReleaseFeature-before-Execute + closed-list replay

별도 feature에서:

1. Evaluate를 open list에 기록한다.
2. parameter Reset + Destroy를 한다.
3. list를 Close한다.
4. NVSDK_NGX_D3D12_ReleaseFeature를 Execute 전에 호출한다.
5. 같은 closed list를 첫 번째 Execute한다.
6. 같은 closed list를 두 번째 Execute한다.

두 실행 모두 readback이 정상이어야 한다. backend trace와 대조할 reset 기대는:

    execute 1: effective reset = 1
    execute 2: effective reset = 0

그 뒤 released handle로 새 open list에서 Evaluate를 호출해 official NVSDK_NGX_Result_FAIL_FeatureNotFound (0xBAD00004)를 요구한다.

## frontend error mapping cases

positive backend encode와 별개로 frontend validation/result ABI를 확인하는 case:

- Init 전 AllocateParameters -> FAIL_NotInitialized
- closed list CreateFeature -> FAIL_InvalidParameter
- unsupported feature ID CreateFeature -> FAIL_FeatureNotSupported
- closed list EvaluateFeature -> FAIL_InvalidParameter
- nonzero output subrect base X -> FAIL_UnsupportedParameter
- NaN jitter X -> FAIL_InvalidParameter
- released handle EvaluateFeature -> FAIL_FeatureNotFound

모든 expected 값은 MSVC helper가 official header enum에서 반환한 값과 exact compare한다.

## 현재 backend failure baseline

prime의 첫 direct temporal fixture GPU run은 초기 candidate에서 TemporalUpscale(record)=E_INVALIDARG로 실패했다. 원인은 owned Com::ref()를 T**처럼 사용한 private QI/GetDevice call이었고 prime이 candidate에서 &internal / &owner로 수정했다.

따라서 이 NGX fixture는 backend positive path가 이미 성공한다고 가정하지 않는다. integrated runtime에서 frontend validation case가 먼저 통과하고, flags74 Evaluate가 backend까지 들어간 뒤 성공 여부를 별도로 판정한다.

## build

산출물은 새 경로만 사용한다.

    mkdir -p build/owned-graphics-20260919/ngx-fixture-build

MSVC ABI helper:

    /opt/llvm-mingw-20260616-ucrt-macos-universal/bin/x86_64-w64-mingw32-g++ \
      --target=x86_64-pc-windows-msvc \
      -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
      -Ibuild/dlss-translator-20260918/contract/nvidia/include \
      -c graphics-translator/tests/owned-d3d12-ngx-msvc.cpp \
      -o build/owned-graphics-20260919/ngx-fixture-build/owned-d3d12-ngx-msvc.obj

fixture PE:

    /opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
      --target=x86_64-w64-mingw32 \
      -std=c++17 -O2 -Wall -Wextra -Werror -static \
      graphics-translator/tests/owned-d3d12-ngx.cpp \
      build/owned-graphics-20260919/ngx-fixture-build/owned-d3d12-ngx-msvc.obj \
      -o build/owned-graphics-20260919/ngx-fixture-build/owned-d3d12-ngx.exe \
      -ld3d12 -luuid

CPU build validation:

- helper: AMD64 COFF object
- main: PE32+ x86-64 console executable
- PE import table: d3d12.dll!D3D12CreateDevice + Windows runtime only
- nvngx.dll은 의도적으로 import table에 없다. exact system32 path + marker를 runtime에서 확인한다.

current build hashes:

- owned-d3d12-ngx-msvc.obj: 7746f526b9f595e155897a2c56c13b0df7ffdec0f3dc1a9389a27978fe95f7d7
- owned-d3d12-ngx.exe: 8a7190faa89458d0ac8bb52b52dc8549b20b278ae160d0c22b8877f70fa345fa

이 worker는 build-only다. GPU/game/setting 변경이나 기존 runner/evidence 실행은 하지 않는다.

## final PASS marker

모든 frontend + backend integration case가 통과하면:

    OWNED_D3D12_NGX_PASS init_allocate_create_evaluate_release=1 flags74=1 continuous_frames=8 params_destroy_before_execute=1 release_before_execute=1 closed_list_replay=1 owned_nvngx_marker=1
