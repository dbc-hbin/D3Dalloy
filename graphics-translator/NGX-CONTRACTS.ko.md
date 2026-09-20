# Owned NVNGX D3D12 contract 보정

2026-09-19 기준 D3D12 NVNGX frontend의 lifecycle, feature handle, output-subrect 계약을
project-owned translator 쪽에서 보정했다. 이 작업은 upstream 제출용이 아니며 실제 candidate
`build/owned-graphics-20260919/dxmt`는 수정하지 않았다.

## 정확한 baseline

- candidate: `build/owned-graphics-20260919/dxmt`
- DXMT base commit: `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`
- copy 시 candidate `src/nvngx/nvngx_d3d12.cpp` SHA-256:
  `8fae693026f44cab7906e7a2c42c334fd60d52b4f756d2907bdb96af8f8c628d`
- 먼저 적용한 queue-order patch:
  `graphics-translator/patches/0007-queue-order.patch`
- 0007 SHA-256:
  `33ffa6c19792e7aa9b855a806486182bd19f8c11661e69a25ba4e05788fa9dad`
- private implementation tree:
  `build/owned-completion-20260919/ngx-contract-work`
- incremental frontend patch:
  `graphics-translator/patches/0008-ngx-contracts.patch`

`0007`은 NVNGX source를 바꾸지 않는다. 0008은 위 candidate를 복사하고 0007을 적용한 상태를
정확한 baseline으로 삼는 incremental patch다. 검증용 별도 copy
`build/owned-completion-20260919/ngx-contract-verify`에서 0007 뒤 0008을 다시 적용했고,
결과 `src/nvngx/nvngx_d3d12.cpp` SHA-256은 private implementation과 동일한
`f2bda509d271105eee4faef5e1183092a98b8609a6bf229ce43e568da01feede`였다.

## NVIDIA public SDK contract 근거

사용한 local official SDK header는
`build/dlss-translator-20260918/contract/nvidia/include` 아래에 있다.

- `nvsdk_ngx.h` SHA-256:
  `dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848`
- `nvsdk_ngx_params.h` SHA-256:
  `943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc`

`nvsdk_ngx.h`의 Shutdown 설명은 `Shutdown1(device)`가 지정 device instance를 종료하고,
`Shutdown1(nullptr)`가 모든 instance를 종료한다고 명시한다. deprecated `Shutdown()`의 대체 호출도
`Shutdown1(nullptr)`다.

`nvsdk_ngx_defs.h`는 `NVSDK_NGX_Handle`을 내용이 공개되지 않은 incomplete struct로만 선언한다.
공식 문서는 CreateFeature가 unique handle을 반환하고 ReleaseFeature 뒤 그 handle이 invalid라고 규정하며,
handle payload를 dereference하는 public contract는 없다. 따라서 frontend는 backend pointer나 heap object를
public handle로 노출하지 않고 process lifetime 동안 재사용하지 않는 opaque token 값을 registry key로 사용한다.

SDK header 첫 설명은 NGX method가 thread-safe하지 않고 caller가 동기화를 책임진다고 명시한다. 이번 보정은
그 계약을 thread-safe API로 확장하지 않는다. 다만 frontend 내부 lifetime race가 stale state를 registry에
다시 게시하는 문제는 막는다.

## frontend 변경

`NVSDK_NGX_D3D12_Shutdown1(nullptr)`는 이제 `NVNGXD3D12ShutdownAll()`을 호출하고 success를 반환한다.
모든 device, feature, parameter registry가 같이 비워진다. non-null device는 기존 per-device shutdown 경로를
그대로 사용한다.

기존 feature handle은 `HandleToken`을 heap allocation한 뒤 process lifetime vector에 계속 보관했다.
ReleaseFeature는 feature registry entry만 지웠기 때문에 create/release 횟수만큼 token heap이 계속 증가했다.
0008은 이 storage를 제거했다. `uintptr_t` monotonic token을 opaque `NVSDK_NGX_Handle*` 값으로 사용하며
token counter는 Shutdown 때 reset하지 않는다. address reuse 때문에 released stale handle이 새 feature로
해석되는 일이 없고, counter 공간을 모두 소비하면 재사용 대신 CreateFeature가 실패한다.

CreateFeature는 backend state를 만드는 동안 device가 shutdown/re-init될 수 있다. Init 때 device registry entry에
bounded `shared_ptr` registration identity를 부여하고, CreateFeature가 state를 public registry에 넣기 직전에
같은 COM identity와 같은 registration identity가 아직 존재하는지 다시 확인한다. shutdown된 registration은
active registry에서 사라지고, in-flight Create가 잡은 reference가 끝나면 해제된다. 같은 device COM identity가
re-init되더라도 새 registration identity를 받으므로 중간 shutdown/re-init은 `FAIL_NotInitialized`로 끝나며
late feature publication을 하지 않는다.

AllocateParameters/GetCapabilityParameters/GetParameters도 기존의 별도 `hasInitializedDevices()` check와
registry insertion 사이 race를 없앴다. parameter facade를 만든 뒤 registry lock 안에서 device 존재 여부를
검증하고 삽입한다. 세 함수는 실패 전에 caller output pointer를 null로 초기화한다.

D3D12 capability map의 `DLSS.Enable.Output.Subrects` 기본값은 1에서 0으로 바꿨다. NVIDIA D3D12 helper는
Create 시 caller의 `InEnableOutputSubrects` bool을 이 key에 명시적으로 기록하고, frontend 자체 parse fallback도
0이다. 따라서 capability map만 가져온 상태가 임의로 output-subrect opt-in이 되는 동작을 제거했다.
D3D11 parameter map과 ABI는 변경하지 않았다.

OutputBaseX/Y는 더 이상 무조건 UnsupportedParameter가 아니다. create-time
`DLSS.Enable.Output.Subrects=0`이면 nonzero output base는 계속 `FAIL_UnsupportedParameter`다.
`DLSS.Enable.Output.Subrects=1`이면 값을 backend frame descriptor에 그대로 전달한다. frontend가 output resource
boundary를 추측해서 clamp/reject하지 않으며, backend가 logical output extent와 backing resource extent를 보고
invalid boundary를 `E_INVALIDARG`로 거절하도록 한다. color/depth/motion/bias/translucency input offset의 기존
unsupported 정책은 그대로다.

## 새 regression fixture

기존 `owned-d3d12-ngx.cpp`와 temporal fixture는 수정하지 않았다. 새 fixture는 다음 두 파일이다.

- `graphics-translator/tests/owned-d3d12-ngx-contracts-msvc.cpp`
- `graphics-translator/tests/owned-d3d12-ngx-contracts.cpp`

MSVC ABI translation unit은 official `nvsdk_ngx.h`를 직접 include하고 `NVSDK_NGX_Parameter::Set/Get/Reset`을
호출한다. 따라서 parameter vtable 순서와 key 이름을 자체 복제하지 않는다. MinGW PE 쪽은 parameter virtual을
직접 호출하지 않고 C helper를 통해서만 접근한다.

fixture runtime case는 다음을 exact result로 확인하도록 작성했다.

1. Init 전 AllocateParameters/GetCapabilityParameters/GetParameters는 `FAIL_NotInitialized`이고 output pointer는 null이다.
2. 두 개의 D3D12 device를 Init한 뒤 capability map과 NGX-managed GetParameters map 모두
   `DLSS.Enable.Output.Subrects` 기본값은 0이다.
3. create flag가 disabled인 feature의 nonzero OutputBaseX는 `FAIL_UnsupportedParameter`다.
4. create flag가 enabled인 feature에서 136x136 backing / 128x128 logical output의 X=8 offset은 success다.
5. 같은 enabled feature에서 X=9는 backend boundary validation을 거쳐 `FAIL_InvalidParameter`다.
6. 64회 create/release 동안 opaque handle token은 released handle 값을 재사용하지 않고 stale Release는
   `FAIL_FeatureNotFound`다.
7. 서로 다른 COM identity의 두 device에 live feature를 하나씩 남긴 상태에서 `Shutdown1(nullptr)`를 호출한다.
   그 뒤 Allocate/GetCapabilityParameters/GetParameters는 `FAIL_NotInitialized`이고 양쪽 live handle 모두
   `FAIL_FeatureNotFound`다.
8. 그 뒤 첫 device만 다시 Init하면 Allocate/Destroy는 다시 성공하지만 두 번째 device command list의
   CreateFeature는 `FAIL_NotInitialized`다. 마지막 per-device Shutdown1도 success를 요구한다.

fixture는 nvngx를 import table에 넣지 않고 runtime에 `C:\\windows\\system32\\nvngx.dll`을 직접 load한다.
loaded DLL에 `own-ngxd3d12` marker가 있는지도 확인한다.
`requireOwnedBackend()`는 D3D12 backend id `owned-graphics-d3d12`를 요구하며 최종 성공 marker는
`OWNED_D3D12_NGX_CONTRACTS_PASS`다.

## build validation

fresh Meson configure는 cross compiler path를 명시해서 수행했다.

```sh
env PATH=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/tools/bin:$PATH \
  build/owned-graphics-20260919/tools/bin/meson setup --wipe \
  build/owned-completion-20260919/ngx-contract-meson \
  build/owned-completion-20260919/ngx-contract-work \
  --cross-file build/owned-completion-20260919/ngx-contract-work/build-win64.txt \
  -Dnative_llvm_path=/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/llvm \
  -Dwine_install_path=/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/wine-crossover/host \
  -Denable_d3d12=true -Denable_nvapi=true -Denable_nvngx=true \
  --prefix=/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-completion-20260919/install \
  --buildtype=debugoptimized
```

frontend DLL build:

```sh
cd build/owned-completion-20260919/ngx-contract-meson
env PATH=/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:/Users/hanbinnoh/Documents/zzz-wine-d3dmetal-dx12/build/owned-graphics-20260919/tools/bin:$PATH \
  ninja src/nvngx/nvngx.dll.postproc
```

MSVC ABI helper build:

```sh
/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/x86_64-w64-mingw32-g++ \
  --target=x86_64-pc-windows-msvc -std=c++20 -fno-exceptions -fno-rtti \
  -Wall -Wextra -Werror \
  -Ibuild/dlss-translator-20260918/contract/nvidia/include \
  -c graphics-translator/tests/owned-d3d12-ngx-contracts-msvc.cpp \
  -o build/owned-completion-20260919/ngx-contract-fixture/owned-d3d12-ngx-contracts-msvc.obj
```

PE fixture build:

```sh
/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
  --target=x86_64-w64-mingw32 -std=c++17 -O2 -Wall -Wextra -Werror -static \
  graphics-translator/tests/owned-d3d12-ngx-contracts.cpp \
  build/owned-completion-20260919/ngx-contract-fixture/owned-d3d12-ngx-contracts-msvc.obj \
  -o build/owned-completion-20260919/ngx-contract-fixture/owned-d3d12-ngx-contracts.exe \
  -ld3d12 -luuid
```

검증 결과:

- nvngx Meson target compile/link/postprocess: PASS
- contract MSVC helper: AMD64 COFF object, `-Wall -Wextra -Werror`: PASS
- contract fixture: PE32+ x86-64 console executable, `-Wall -Wextra -Werror`: PASS
- fixture import table에는 `d3d12.dll!D3D12CreateDevice`와 Windows runtime만 있고 `nvngx.dll` static import는 없음
- built `nvngx.dll` export table에 Shutdown1/AllocateParameters/GetCapabilityParameters/CreateFeature/
  EvaluateFeature/ReleaseFeature 존재
- built nvngx SHA-256:
  `20aa227073264acf54f579516288663aa4600ca691b1bfabaeca0a22ecbd57dd`
- MSVC helper SHA-256:
  `f90c0ba950e507cca3a148d90c8f1d01e944869858f0cfe7eb9d74704ca1bfcc`
- PE fixture SHA-256:
  `9e55a545ad3f8ce5e9cdfd1b9fafdec2d52cce7af56e49716ac94345fd7931cd`
- 0008 patch SHA-256:
  `446de97c2c0e510b78cfea17e7c2483cef0d1fc29dfc8a0e7faa878f7624bc63`

0008 stack 재현 검증:

```sh
cp -a build/owned-graphics-20260919/dxmt \
  build/owned-completion-20260919/ngx-contract-verify-v2
git -C build/owned-completion-20260919/ngx-contract-verify-v2 apply \
  ../../../graphics-translator/patches/0007-queue-order.patch
git -C build/owned-completion-20260919/ngx-contract-verify-v2 apply --check \
  ../../../graphics-translator/patches/0008-ngx-contracts.patch
git -C build/owned-completion-20260919/ngx-contract-verify-v2 apply \
  ../../../graphics-translator/patches/0008-ngx-contracts.patch
```

runtime fixture는 이 worker에서 실행하지 않았다. GPU/Wine/game 실행은 prime 소유다. 특히 enabled X=8 success와
X=9 InvalidParameter case는 prime의 backend output-subrect 구현이 합쳐진 뒤 실행해야 한다. 현재 0008의 frontend는
두 값을 backend까지 정확히 전달하고 create-time opt-in만 검사한다.
