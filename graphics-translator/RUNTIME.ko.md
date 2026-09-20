# Owned graphics runtime 스테이징 계약

이 문서는 DXMT의 D3D11 구현과 진행 중인 D3D12 구현을 바탕으로 만든 owned graphics translator를 기존 Wine 실행 파일과 결합하는 비파괴 런타임 계약을 정리한다. 실제 게임 실행과 prefix 변경은 이 스테이징 단계에 포함하지 않는다.

## 빌드 입력

기준 DXMT 소스는 `build/dlss-translator-20260918/contract/dxmt`의 commit `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`에서 확인했다. 해당 소스의 `meson.options`에서 `wine_builtin_dll=true`가 기본이고 `enable_d3d12=false`가 기본이다. 따라서 owned D3D12 빌드는 `-Denable_d3d12=true`를 명시해야 한다.

x86_64 Wine cross build의 설치 prefix는 다음 파일을 제공해야 한다.

| Meson install 경로 | staged Wine 경로 | 역할 |
| --- | --- | --- |
| `x86_64-windows/d3d12.dll` | `lib/wine/x86_64-windows/d3d12.dll` | owned D3D12 |
| `x86_64-windows/dxgi.dll` | `lib/wine/x86_64-windows/dxgi.dll` | DXGI |
| `x86_64-windows/d3d11.dll` | `lib/wine/x86_64-windows/d3d11.dll` | D3D11 |
| `x86_64-windows/d3d10core.dll` | `lib/wine/x86_64-windows/d3d10core.dll` | D3D10 core |
| `x86_64-windows/winemetal.dll` | `lib/wine/x86_64-windows/winemetal.dll` | PE unix-call thunk |
| `x86_64-unix/winemetal.so` | `lib/wine/x86_64-unix/winemetal.so` | Metal/MetalFX Unix side |

DXMT `src/winemetal/main.c`는 `__wine_init_unix_call()`을 초기화하고 PE thunk가 `WINE_UNIX_CALL`로 Unix side를 호출한다. 따라서 `winemetal.dll`과 같은 basename의 `x86_64-unix/winemetal.so`가 함께 있어야 한다. `winemetal.so`는 빌드 시 Wine의 `winemac.so`와 `ntdll.so`에 링크되며, 스테이저는 두 dependency가 존재하고 `@rpath` 또는 macOS 시스템 경로처럼 재배치 가능한 형태인지 검사한다.

Airconv의 Darwin 구현은 `src/airconv/darwin/meson.build`에서 static library로 만들어져 `winemetal.so`에 링크된다. 별도의 `airconv` 실행 파일은 Meson install 대상으로 선언되어 있지 않으므로 런타임에 따로 복사하지 않는다. 빌드 자체에는 문서대로 LLVM 15의 x86_64 headers/static libraries가 필요하다.

2026-09-20의 explicit immutable-storage 변경은 host-side `SM50_COMPILED_BITCODE`에 constant data/size/alignment/relocation 필드를 추가했다. **이 결과 struct를 쓰는 PE caller, Unix thunk/compiler, native consumer는 같은 공용 header로 함께 재빌드해야 한다.** 예전 `d3d11.dll`/`d3d12.dll`과 새 `winemetal.so`를 섞거나 그 반대로 단일 module만 교체하면 결과 buffer의 크기 계약이 맞지 않는다. 같은 canonical install에서 graphics module 세트 전체를 staging한다. DXR shader callback ABI를 유지했다는 것은 이 host-side 결과 ABI까지 이전 binary와 호환된다는 뜻이 아니다.

owned DXIL compiler는 별도 dylib이 아니라 owned source patch series에서 `src/airconv`로 구현되어 `winemetal.so`에 static link된다. 스테이저는 별도의 compiler wrapper/SDK dylib/license tree를 요구하지 않으며, DXMT install에 `x86_64-unix/libdxmt-dxilconv.dylib`, `x86_64-unix/libmetalirconverter.dylib`, `x86_64-unix/licenses/metal-shader-converter/` 같은 제거된 external compiler artifact가 남아 있으면 그 install 전체를 거부한다. `winemetal.so`가 `libdxmt-dxilconv`/`libmetalirconverter`/`D3DMetal`/`libd3dshared` 같은 외부 compiler/backend library를 링크해도 stage는 실패한다.

owned 빌드의 `d3d12.dll`에는 `owned-graphics-d3d12` marker가 있어야 한다. 스테이저는 이 marker가 없는 vanilla/stale DXMT 산출물을 거부한다.

## PSO cache 계약

`0018-d3d12-pipeline-cache.patch`부터 graphics/compute PSO의 `GetCachedBlob`과 생성 descriptor의 `CachedPSO`를 지원한다. Cache는 owned compiler의 stage별 metallib, unpatched immutable pool bytes, alignment 및 relocation offsets를 보존한다. 주소는 복원 시 새 GPU allocation에 맞춰 다시 적용한다. Metal library/function/PSO 생성까지 건너뛰는 native binary archive는 아니며 `CreatePipelineLibrary`는 여전히 `E_NOTIMPL`이다.

- Cache key는 pipeline kind, shader bytecode, serialized root signature, 지원되는 graphics PSO state, NodeMask/Flags 및 Metal target version으로 만든다. 포인터나 C++ struct padding을 hash하지 않는다.
- Private format version, 실제 MTLDevice registry ID, native compiler/runtime Mach-O UUID 및 macOS build가 일치해야 한다. UUID/OS-build 조회는 PE/Unix 공용 call 199, allocation 상한 조회는 call 200을 사용한다. 기존 call 번호는 바꾸지 않았지만 **PE DLL과 Unix module을 서로 다른 build 세대로 섞어서는 안 된다.**
- Cache length가 0이면 cold compile이다. Nonempty/null pointer, 손상된 layout/checksum, PSO mismatch 및 유효하지 않은 library는 `E_INVALIDARG`; adapter mismatch는 `D3D12_ERROR_ADAPTER_NOT_FOUND`; version/native UUID/OS-build mismatch는 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`다. 실패한 생성의 출력 포인터는 null이다. 오류를 숨겨 자동 cold compile로 fallback하지 않는다. Caller는 필요하면 cache 없이 명시적으로 재생성한다.
- Unaligned 입력도 읽을 수 있고, 생성 성공 뒤 caller가 입력 blob을 해제해도 PSO가 소유한 복사본은 유효하다. 매 `GetCachedBlob` 결과는 별도 application-owned blob이며 수정·해제가 PSO나 다른 반환 blob에 영향을 주지 않는다. Null output argument는 `E_POINTER`다.
- 크기 산술, region 경계, alignment, relocation write/target 범위를 검사하고 constant buffer allocation은 해당 device의 `maxBufferLength`를 넘지 않게 한다. Cold PSO는 compiler artifact handle을 보유하고 export할 때 직렬화하며, warm PSO는 검증된 단일 blob을 소유한다.

검증된 canonical install/runtime은 `build/owned-direct-canonical-cache-20260921/`이다. 별도 프로세스 GPU 복원과 손상 cache의 실제 HRESULT 기록은 [SMOKE-TEST.ko.md](SMOKE-TEST.ko.md)에 정리했다. Cache 호환성은 이 명시적 identity에 한정되며 버전 간 영구 호환을 약속하지 않는다.

## 스테이징

`scripts/stage-owned-graphics.py`는 원본 Wine과 DXMT install prefix를 읽기만 하고 새 목적지에 복사본을 만든다.

```sh
python3 scripts/stage-owned-graphics.py \
  stage \
  --dxmt-install build/owned-graphics-20260919/install \
  --wine-source build/dlss-translator-20260918/wine-final \
  --wine-dest build/owned-graphics-20260919/runtime
```

목적지는 존재하지 않아야 하고 Wine/DXMT 입력과 서로 포함 관계가 아니어야 한다. 목적지 자체나 그 부모 경로가 symlink이면 거부한다. 복사된 runtime 안에서도 교체할 graphics module 경로에 symlink가 있으면 외부 파일을 따라가 덮어쓰지 않고 실패한다.

Wine source가 현재 프로젝트의 packaged runtime처럼 `bin/wine` wrapper와 `bin/wine.real`을 갖고 있으면 `wine.real`을 그대로 보존한다. 일반 Wine처럼 `bin/wine`이 실행 바이너리이면 복사본 안에서만 이를 `bin/wine.real`로 옮긴다. 새 `bin/wine`은 owned 전용 wrapper로 교체한다.

wrapper는 실행마다 `WINEPREFIX`가 명시되어 있지 않으면 실패한다. prefix 자체를 만들거나 수정하는 정책은 caller가 결정한다. 실제 게임 검증에서는 기존 Yaagl prefix와 분리된 전용 prefix를 명시해야 한다.

기존 prefix를 clone해 쓰는 경우, 그 clone은 과거 runtime에서 만들어져 `system32/winemetal.dll` placeholder가 없을 수 있다. 스테이징된 owned runtime으로 그 clone을 한 번 초기화해야 한다.

```sh
WINEPREFIX=<cloned-prefix> <staged-runtime>/bin/wineserver -k || true
WINEPREFIX=<cloned-prefix> <staged-runtime>/bin/wine wineboot.exe -u
```

이 단계는 caller가 명시적으로 수행하며 stager는 prefix를 만들거나 수정하지 않는다. 초기화 뒤에는 같은 runtime으로 game을 실행한다.

Wine loader는 `WINEDLLOVERRIDES` 환경 설정을 prefix registry의 DLL override보다 먼저 본다. wrapper는 아래 load order를 강제한다.

```text
d3d12=b;dxgi=b;d3d11=b;d3d10core=b;winemetal=b
```

여기서 `b`는 builtin-only다. owned 모듈이 없거나 로드되지 않으면 native/GPTK DLL로 자동 fallback하지 않고 실패한다. 환경의 `WINEDLLOVERRIDES`는 이 고정 값으로 교체된다. 다른 DLL의 별도 정책이 필요하면 이 다섯 모듈과 충돌하지 않는 prefix registry 설정을 사용한다.

기존 GPTK launcher에서 상속될 수 있는 `CX_ACTIVE_GRAPHICS_BACKEND`, `CX_APPLEGPTK_LIBD3DSHARED_PATH`, 알려진 `D3DM_*` 및 packaging용 `GPTK_*` 상태와 `WINEDLLPATH_PREPEND`를 wrapper가 제거한다. `DXMT_CONFIG`와 `DXMT_CONFIG_FILE`은 owned translator의 설정 경로이므로 제거하지 않는다.

기존 P3 Wine wrapper가 설정하던 package-relative `DYLD_FALLBACK_LIBRARY_PATH`, GStreamer plugin path와 scanner 경로는 graphics backend 선택과 무관하고 packaged Wine의 일반 런타임 의존성에 필요할 수 있다. 새 wrapper는 P3 marker가 있는 경우 이 부분을 유지한다. 반대로 외부 graphics 주입 경로인 `DYLD_INSERT_LIBRARIES`, DXVK/Vulkan loader 관련 환경은 제거한다.

원본 packaged Wine에는 `D3DMetal.framework`와 `libd3dshared.dylib`가 다른 Wine/launcher 기능과 함께 배포되어 있다. 현재 정적 근거만으로 이 파일들을 삭제해도 runtime 전체가 안전하다고 단정할 수 없으므로 스테이저는 물리적으로 보존한다. owned wrapper와 builtin-only load order가 이들을 graphics fallback으로 선택하지 못하게 한다. manifest에는 보존 여부와 이 이유를 기록한다.

## 검증과 rollback

스테이지가 성공하면 root에 `owned-graphics-runtime.json`을 기록한다. manifest에는 입력 경로, loader 정책, staged graphics DLL/Unix module과 `bin/wine*`의 SHA-256/크기, `winemetal.so`의 실제 Mach-O dependency 목록, 그리고 이 module 안에 들어 있는 self-owned DXIL compiler 기록(`native_compiler`)을 저장한다. staging 직후 동일 검증을 통과해야만 임시 디렉터리를 최종 목적지 이름으로 publish한다.

나중에 다음 명령으로 파일 변조, wrapper 정책 drift, owned marker, `winemetal.so` dependency drift를 다시 검사할 수 있다. 검증기는 manifest가 스스로 기록한 값만 믿지 않고 staged `winemetal.so`를 다시 열어 dependency와 재배치 가능성을 확인하고, 제거된 external DXIL compiler artifact가 runtime에 다시 나타나면 실패로 판정한다.

```sh
python3 scripts/stage-owned-graphics.py \
  verify \
  --runtime build/owned-graphics-20260919/runtime
```

이 검증은 CPU/파일 수준의 load contract만 확인한다. D3D12 기능 완성도, 실제 ZZZ 호환성, Metal command 제출/Present 동작은 증명하지 않는다. 실제 Wine/GPU/game 실행은 별도 단계에서 검증해야 한다. Rollback은 이 private runtime을 사용하지 않고 원본 Wine을 다시 선택하는 것으로 끝나며, 원본 Wine과 DXMT install tree에는 스테이저가 어떤 변경도 하지 않는다.

## 선택적 owned NVIDIA extension

D3D12 NGX frontend와 NVAPI까지 owned runtime에 넣을 때만 다음 옵션을 추가한다.

```sh
python3 scripts/stage-owned-graphics.py \
  stage \
  --dxmt-install build/owned-graphics-20260919/install \
  --wine-source <wine-runtime> \
  --wine-dest <new-runtime> \
  --with-nvngx \
  --with-nvapi
```

`--with-nvngx`는 파일 존재만으로 승인하지 않는다. `nvngx.dll`에 literal `own-ngxd3d12` marker와
`NVSDK_NGX_D3D12_Init/CreateFeature/EvaluateFeature/ReleaseFeature`를 포함한 현재 required D3D12 export set 전체가
PE export table에 실제로 있어야 한다. 현재 upstream D3D11-only `nvngx.dll`은 이 gate에서 거부된다.
`--with-nvapi`도 `nvapi64.dll`의 실제 `nvapi_QueryInterface` export를 확인한다.

검증을 통과한 경우에만 wrapper override에 `nvngx=b`와/또는 `nvapi64=b`를 추가한다. DXMT의 `nvngx` DllMain과
`NvAPI_Initialize`는 둘 다 DXGI의 NVIDIA extension gate를 요구하므로, 둘 중 하나를 stage한 wrapper는
`DXMT_ENABLE_NVEXT=1`을 내부에서 설정한다. 두 옵션이 모두 꺼져 있으면 외부에서 상속된 `DXMT_ENABLE_NVEXT`를 제거한다.
manifest의 `optional_modules`와 `WINEDLLOVERRIDES` 값이 이 선택을 기록하며 `verify`가 marker/export를 다시 검사한다.
