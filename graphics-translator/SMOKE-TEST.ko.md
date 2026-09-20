# Owned D3D12 + DXGI smoke test

## PSO cache 구현과 실행 검증 (2026-09-21)

최신 canonical root는 `build/owned-direct-canonical-cache-20260921/`이다. 기존 17개 patch는 유지하고 `0018-d3d12-pipeline-cache.patch`를 추가했다. Patch SHA-256은 `0340701953bec5d19491ec0a0c66637b6ccb8862f532e517e5cfa3e0837bec2d`, native `winemetal.so` SHA-256은 `ea487e843a1c7990b8bc0d9d297e9fd3e3f46666788921a6d8fda7ce6fd56e37`이다. Upstream부터 새로 적용한 18-patch build는 `complete`이며 canonical install의 전체 module 세트를 staging했다.

Graphics/compute `GetCachedBlob`과 `CachedPSO`가 연결됐다. Cache에는 metallib와 immutable pool의 원본 bytes/alignment/relocation이 들어가며 GPU 주소를 저장하지 않는다. 유효한 cache는 owned shader compile을 생략하되 reflection, constant-buffer 초기화, Metal library/function/PSO 생성은 수행한다. **Metal native pipeline binary archive나 `ID3D12PipelineLibrary` 구현은 아니다.** 후자는 여전히 `E_NOTIMPL`이다. 상세 호환성·오류 계약은 [RUNTIME.ko.md](RUNTIME.ko.md)의 PSO cache 절에 있다.

- `gpu-cache/populate-result.json`, `restore-result.json`: **별도 프로세스** export/import 모두 PASS/exit 0. 원본 VS와 CS/PS constant-array shader를 사용한 direct → indirect → direct, buffer inheritance ON/OFF에서 CS의 `13579bdf/31415926/fedcba98` 및 PS의 16/24/24 pixels 전체가 일치했다. 입력 cache와 원본 PSO를 해제한 뒤 사용하고, 반환 blob을 덮어쓴 뒤 재수출해도 동작한다. 모두 sentinel 초기화와 Metal API/GPU validation을 사용했다.
- `gpu-pipeline-cache/result.json`: 영구 fixture `tests/owned-d3d12-pipeline-cache.cpp` PASS. Null/nonempty·truncation·payload corruption·foreign blob·root/shader mismatch 6건은 `E_INVALIDARG`와 null output을 확인한다. 변경 shader의 cold GPU control은 `31415813`, unaligned input을 해제한 복원과 반환 blob 격리는 각각 `31415a5b`다. 두 새 DXIL은 `pipeline-cache.hlsl`의 `cacheA/cacheB`를 `cs_6_0 -HV 2021 -O0`로 컴파일한 것이다.
- `gpu-cache/rejections-result.json`: 정상 compute/graphics 2건과 거부 14건 PASS. 실제 이전 native build에서 만든 두 cache의 driver mismatch, format/adapter/OS-build mismatch, 크기·정렬·count overflow, metallib magic, trailing bytes, relocation write/target 범위 초과, pipeline kind mismatch를 검사했다. 이 중 format 변조는 명시적 손상 fixture이며 정상 shader 실행 증거로 세지 않는다.
- 같은 canonical runtime의 `gpu-dxil/`, `gpu-dxil-ia/`, `gpu-dxil-static-sampler/`, `gpu-raytracing/`, `gpu-raygen-layouts/` 모두 원본 fixture/shader로 PASS. DXR의 triangle 10 + procedural 4 lane 및 서로 다른 raygen local-root layout을 유지했다.
- 위 실행은 모두 `validation_errors=[]`, `foreign_backend_loads=[]`이며 stage된 자체 `winemetal.so`의 실제 로드를 확인했다. Build/staging/diagnostics 회귀 16/21/42개도 PASS. 실제 ZZZ 실행·설치된 runtime 변경·fence rewinding 구현은 수행하지 않았다.

Cache 전용 영구 fixture 실행 예시(출력 디렉터리는 매번 새 경로):

```sh
/opt/llvm-mingw-20260616-ucrt-macos-universal/bin/x86_64-w64-mingw32-clang++ \
  -std=c++20 -O2 -municode -static \
  -Ibuild/owned-direct-canonical-cache-20260921/source/include/native/directx \
  graphics-translator/tests/owned-d3d12-pipeline-cache.cpp \
  -ld3d12 -ldxgi -ldxguid -lole32 -o /tmp/owned-pipeline-cache.exe
python3 scripts/test-owned-graphics.py \
  --runtime build/owned-direct-canonical-cache-20260921/runtime \
  --suite pipeline-cache --fixture /tmp/owned-pipeline-cache.exe \
  --out build/owned-pipeline-cache-new-check --validation
```

이전 `owned-direct-canonical-icb-20260920/runtime`과 이번 개발용 runtime 사본은 canonical 검증 후 정리했다. 각 root의 `retired-runtime-manifest.json`과 기존 fixture/source/log/result는 보존했다. 새 canonical runtime은 유지하며 이번 검증의 임시 실행 driver와 완료된 전용 prefix만 정리했다.

## 자체 DXIL compiler 실행 검증 (2026-09-20)

### Canonical DXR / indirect 실행 증거

2026-09-20 immutable-storage 검증 root는 `build/owned-direct-canonical-icb-20260920/`이다. Upstream부터 17개 patch를 적용한 새 build의 `owned-graphics-build.json`은 `complete`이며 자체 compiler를 포함하고 외부 compiler dependency는 없다. `0015-dxil-compiler.patch` SHA-256은 `af756950f846b88cfd43809ee4c98d41ecc256e978a173d74edcaf1dd09fb472`, native `winemetal.so` SHA-256은 `34f794cd5efb8a4fe270e15b0df1c2dbb563c551f81d4e1d32f706b2529ee075`이다. Staging verifier도 module hash, builtin load policy, native dependency 계약을 통과했다.

- `gpu-dxil/`, `gpu-dxil-ia/`, `gpu-dxil-static-sampler/`는 모두 **PASS/exit 0**이다. Constant-array VS, register space, descriptor table, compute, varying/depth, indexed/nonindexed IA와 static sampler GPU readback을 포함한다.
- `gpu-raytracing/`는 기존 triangle 10 + procedural 4 lane 전체 **PASS/exit 0**이다. Callable, nested trace, GPU SBT/local root 선택, any-hit accept/reject와 committed-hit 보존을 검증했다. Procedural lane 3은 t=1 commit 후 t=5를 any-hit 전에 거부하고 t=.5를 any-hit에서 거부하면서 첫 hit의 attribute/kind/t와 거부 payload 쓰기를 보존한다.
- `gpu-raygen-layouts/`도 **PASS/exit 0**이다. 두 raygen은 각각 `(b0, space16)` 3 DWORD와 `(b1, space16)` 5 DWORD local root를 사용한다. Shader가 서로 다른 첫 값과 5번째 DWORD를 확인한 뒤 기존 14-lane readback을 통과했다.
- 위 5개 실행 모두 Metal API/GPU validation 및 Shader Validation의 stderr 오류 보고를 켰으며 `validation_errors=[]`, `foreign_backend_loads=[]`이다. DXIL/IA/RT의 수정 전후 frozen fixture SHA-256은 동일하다. Fixture, shader, expected readback을 바꾸지 않았다.
- Build/staging/diagnostics 모듈은 각각 **16/21/42개 test**를 통과했다.
- `gpu-multi-indexed-storage/result.json`: 원본 VS/PS로 direct → 최대 4개 indexed indirect command → direct를 검증했다. **16/32-bit index × GPU count 0/3/7**에서 각각 0/3/4개 command가 실행되고 나머지 plane은 sentinel을 유지했다. Argument offset/stride **28/28 bytes**, count-buffer offset **4 bytes**, command별 root constants와 VS/PS immutable pool을 함께 사용했다. 독립 관측을 위해 probe depth 비교는 ALWAYS다. **2,304개 uint 값 전부 일치**, PASS/exit 0, validation/foreign-backend 오류 없음이다.
- `gpu-indexed-storage/result.json`: 원본 VS/PS를 유지한 indexed direct → indirect → direct에서 **16/32-bit index × buffer inheritance ON/OFF** 모두 PASS/exit 0이다. Guard 뒤 `StartIndexLocation=1`, index `2/0/1`, `BaseVertexLocation=5`를 사용하며 각 mode의 **16/24/24 pixels 전체 uint 값**이 일치했다. Sentinel 초기화, validation/foreign-backend 오류 없음이다. 최초 probe의 잘못된 base-vertex 가정(index 3으로 VS 상수 배열 범위 초과)은 `invalid-base-assumption/`에 따로 보존했고 production 회귀로 세지 않는다. Shader와 기대 픽셀 값은 바꾸지 않았다.
- `probes/captured-shaders-current-abi/result.json`: 기존 ZZZ 캡처 shader/root-signature **16쌍(VS 7/CS 4/PS 5)**을 canonical compiler와 새 공용 ABI로 재생했다. 캡처 shader SHA-1/root SHA-256 일치, 반복 컴파일의 metallib·pool·alignment·relocation 일치, Metal library 생성 모두 PASS다. 모두 pool **0 bytes**이므로 무상수 경로의 오프라인 회귀 검증이며, explicit storage GPU 검증이나 실제 게임 렌더링 검증으로 세지 않는다.
- 기존 native `null-descriptor-probe.mm`도 source/shader 변경 없이 canonical 공용 header와 airconv target object로 다시 링크했다. `probes/null-descriptor-current-abi/gpu.log`의 null CBV/raw/structured read, null/OOB atomic, OOB store sentinel, root descriptor read/write **11개 값 모두 일치**, PASS/exit 0이며 API/GPU validation과 stderr 보고를 켰다. 이 실행은 repository runner가 아니라 동일 target object를 사용한 직접 빌드다.

**0018 이전 PSO cache 제한(현재는 위 구현으로 대체):** 이 17-patch build에서는 D3D12 cache 복원 경로가 구현되지 않았고 shader 생성은 compile/상수 buffer 초기화를 거친다. Staged `d3d12.dll`의 disassembly에서 `CreatePipelineLibrary`는 `E_NOTIMPL`을 반환하지만 graphics/compute `GetCachedBlob`은 `IMPLEMENT_ME`에서 **`abort()`**를 호출한다. 따라서 cache export 요청은 정상적인 miss가 아니라 프로세스 종료 위험이다. 이전 canonical source에도 동일한 함수와 abort macro가 있으므로 explicit storage의 신규 회귀는 아니다. `probes/immutable-constants/pso-cache-api-disassembly.txt`에 binary 증거를 보존했다. API를 실행한 결과나 게임에서 실제로 이 경로를 호출했다는 증거는 아니다.

Shader Validation은 기본적으로 오류를 OS log에 쓴다. Runner는 이제 `MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1`, `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1`을 설정하고 실제 진단 prefix인 `Invalid Metal usage:`도 FAIL로 판정한다. 정상 readback marker가 모두 있어도 invalid load를 거부하는 회귀는 수정 전 FAIL/수정 후 PASS다.

수집 강화 후 이전 root `build/owned-direct-canonical-rt-final-20260920/`의 `gpu-dxil-diagnostics/`, `gpu-dxil-ia-diagnostics/`에서 `INF or NAN detected in interpolant 'reg0_0'`가 드러났다. 원인은 `dxbc_converter_basicblock.cpp::to_desired_type_from_int_vec4`가 부분 출력 마스크 밖의 AIR vector lane을 `undef`로 만든 것이었다. 기존 float4 인터페이스를 유지하면서 **마스크 밖의 padding만 0**으로 내보내도록 수정했다. 정의된 값이나 실제 NaN을 덮어쓰는 sanitization은 없다. 실패 artifact는 보존하고 위 최신 root에서 동일 fixture가 통과한 결과를 기록했다.

Runner의 zero update-scratch 판정도 사용자 승인 후 수정했다. [Microsoft PREBUILD_INFO 계약](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_acceleration_structure_prebuild_info)은 `ALLOW_UPDATE` 미지정 시 `UpdateScratchDataSizeInBytes=0`을 명시한다. 기존 fixture는 `PREFER_FAST_TRACE`만 사용한다. 완전한 prebuild record, 양수 result/build scratch 및 모든 GPU readback 검사를 유지하면서 update scratch만 0을 허용한다. Result/build scratch가 0이면 계속 거부한다. 이전 `gpu-raytracing/`와 `gpu-raytracing-verified/`의 historical artifact는 덮어쓰지 않았다.

이전 root의 추가 실행 증거(runtime 사본은 정리했고 manifest는 `retired-runtime-manifest.json`에 보존했다):

- `probes/as.log`: BLAS/TLAS build, zero-scratch refit, clone/compact, GPU postbuild size/header, 최소 allocation compact readback PASS. Version-mirror slot offset의 buffer binding/kernel index 중복 적용 결함을 수정했다. AS lifetime의 abandoned recording, submission retention, bounded retirement, 100만 mirror-slot 재사용 및 실제 GPU mirror publication도 통과했다.
- `probes/raygen-local.log`: local root 유무가 다른 raygen SBT 선택과 14-lane readback PASS. `probes/local-sampler-{1,2,3,4}.log`: local-only, global/local 중복, 충돌 거절 후 정상 재생성, global RS 없는 local resource 검증. Sampling은 `0x3f000000`, 나머지 sentinel 3개는 유지됐다. 이 probe의 `ExitProcess`는 teardown 증거가 아니다.
- `probes/indirect.log`: API validation에서 compute/render direct → indirect → direct PASS. Compute는 `13579bdf/13579be0/13579be1`, render 세 영역은 16/24/24 pixels이다.

### 명시적 immutable storage와 ICB

Public MSL의 implicit constant-array storage는 이 macOS의 GPU Validation에서 ICB-capable PSO만으로 invalid load를 재현했다. SDK offline compiler와 자체 compiler의 AIR 모두 같은 결과였고, ICB 실행뿐 아니라 해당 PSO의 direct draw도 실패했다. 원래의 실패 증거는 `build/owned-direct-canonical-validated-20260920/probes/icb-boundary/`에 보존했다. 이 root의 중복 runtime 사본은 최신 검증 후 정리했고 manifest는 `retired-runtime-manifest.json`에 남겼다. Apple 내부 원인은 확정하지 않았다.

사용자 승인에 따라 일반 DXIL VS/PS/CS의 최적화 후 immutable globals를 compiler-owned constant pool로 옮겼다. DataLayout의 배열 stride, struct padding, 명시적/ABI alignment와 정의된 bit pattern을 보존하고 64-bit pointer initializer는 pool-relative relocation으로 내보낸다. Surviving helper에는 pool argument를 전달하며 AIR metadata에 read-only buffer slot **29**를 추가한다. Slot 30의 indirect resolver와 DXR callback ABI는 변경하지 않았다.

`SM50_COMPILED_BITCODE`의 constant data/alignment/relocation 필드는 `SM50DestroyBitcode`까지 borrowed다. Runtime은 shader stage별 PSO 생성 시 한 번 upload하고 GPU 주소 relocation을 적용한다. Direct/ICB PSO 모두 immutable binding을 선언하고, recorded encoder가 buffer reference를 유지한다. ICB의 buffer inheritance가 꺼진 경우 GPU resolver가 slot 29를 명시적으로 설정하고 residency도 선언한다. Immutable globals가 없는 shader에는 추가 GPU buffer나 binding이 없다. 전체 caller/thunk를 같은 header로 재빌드했으며 DXBC와 DXR 결과의 추가 data 필드는 비어 있다.

- `probes/immutable-constants/native-vertex.log`: 원본 vertex DXIL SHA-256 `66ed0ce0ca7a5473864216a3ea563fadd09e670421f314c714557ebb783f83a3`, compiler-produced 24-byte pool, ICB, **64/64 pixels**. Shader/expected pixels는 그대로다.
- `probes/immutable-constants/native-stages.log`: 자체 AIR builder와 같은 lowering으로 만든 추가 CS/PS smoke다. Direct, inherited-buffer ICB, explicit-buffer ICB 모두 CS는 **`7fc12345/80000000/12345678/00abcdef`**, PS는 **64/64 pixels**다. NaN payload, negative zero, 32-byte alignment, struct padding, 실제 GPU pointer relocation과 helper argument 전달을 포함한다. 새 HLSL/DXIL fixture의 검증으로 주장하지 않는다.
- `gpu-stage-storage/result.json`: 기존 DXC producer로 별도 HLSL을 DXIL로 만든 뒤 **D3D12 CS/PS의 실제 pool upload/binding**까지 검증했다. Direct → indirect → direct를 buffer inheritance ON/OFF로 각각 실행해 CS는 **`13579bdf/31415926/fedcba98`**, PS는 세 영역 **16/24/24 pixels 전체의 uint 값**이 일치했다. 매 실행 전 output을 `0xcdcdcdcd`로 초기화해 이전 readback 재사용을 배제했다. 원본 VS는 byte-identical이며 기존 fixture는 변경하지 않았다. **PASS/exit 0**, validation/foreign-backend 오류 없음이다. 재현 source와 전용 verifier도 같은 directory에 보존했다.
- `gpu-stage-switch/result.json`: 같은 encoder에서 **A → B → pool-free → A → B → A** PSO 전환을 검증했다. CS/PS 각각 direct와 indirect를 섞고 inheritance ON/OFF를 모두 검사했다. CS 여섯 값과 PS 여섯 영역 **8/8/8/8/16/16 pixels 전체 값**이 각 PSO의 기대값과 일치했다. Sentinel 초기화, exit 0, validation/foreign-backend 오류 없음이다. `compiler-contract.log`의 canonical `SM50Compile`/`SM50GetCompiledBitcode` 호출은 A/B의 CS/PS 각각 **32-byte pool**, pool-free CS/PS 각각 **size 0 + null data**를 확인했다.
- `gpu-stage-switch/compiler-lifetime.log`: A/B/pool-free의 CS/PS 6개 각각에서 동일 shader 객체에 **8개 동시 `SM50Compile` 호출**을 실행했다. Shader와 입력 byte 소유권을 해제한 뒤 compiled handle별 metallib/pool/alignment/relocation을 기준 결과와 비교하고 handle을 차례로 해제했다. **48개 결과 모두 일치, exit 0**이다. 소비자는 ASan으로 계측했지만 canonical compiler object는 그대로이며, 이를 TSan race 부재 증거로 주장하지 않는다.
- `gpu-large-storage/result.json`: 각 40KiB인 두 배열을 **81,920-byte pool**로 합친 CS의 direct → indirect → direct를 inheritance ON/OFF로 검증했다. 결과는 **`9a821c56/3bbd2516/c1efc372`**, PASS/exit 0, validation/foreign-backend 오류 없음이다. `compiler-layout.log`는 runtime과 같은 Metal 3.1 옵션/entry 이름으로 실제 읽은 pool offset이 **81,916 / 81,904 / 81,892**임을 확인했다. 이 장비에서 64KiB 이후 주소를 실제로 검증한 결과이며 모든 Metal 장비의 최대 크기를 주장하지 않는다.
- `gpu-indirect-transition/stdout.log`: 원본 DXIL을 사용하는 D3D12 direct → indirect → direct에서 compute **`13579bdf/13579be0/13579be1`**, render **16/24/24 pixels**다. Render는 buffer inheritance ON/OFF를 각각 검사했다. 실행 exit 0, validation/foreign-backend 오류 없음이다.
- 위 probe는 graphics를 두 번 실행하므로, 정확히 한 varying/depth set을 요구하는 기존 `dxil` runner의 `result.json`은 **`pass=false`**다. Runner와 probe 출력을 바꾸지 않았다. 이 계약 차이를 기록하고 두 mode의 여섯 영역 및 compute 세 값을 별도로 검사한 결과는 `gpu-indirect-transition/scenario-verification.json`이다. 기존 5개 정규 suite의 `result.json`은 모두 PASS다.

모든 새 GPU smoke는 Metal API/GPU validation과 stderr 오류 보고를 켰다. 기존 5개 fixture의 모든 SHA-256은 직전 검증 root와 동일하다. 테스트/셰이더/기대값이나 validation 판정을 바꿔 통과시키지 않았다.

과거 기록의 “validation 오류 없음”은 당시 수집한 로그 범위에 한정된다. Stderr 보고를 켜지 않았던 실행을 Shader Validation 전체 오류 부재의 증거로 쓰지 않는다. 임시 실행 파일과 빌드 보조 script는 정리하고 새 재현 source/AIR/원본 로그는 `probes/immutable-constants/`에 보존했다. Runtime에 진단용 compiler 계측을 남기지 않았다.

이 결과는 synthetic 계약의 증거이며 전체 DXR Tier 1.1 또는 실제 ZZZ 렌더링·DLSS·RT 호환 완료를 뜻하지 않는다. 이 작업에서는 실제 게임, 설치된 Yaagl/runtime, live Wine prefix를 변경하지 않았다.

### 기존 검증 기록

- SDK-free 정식 patch build: `build/owned-direct-canonical-space-20260920/owned-graphics-build.json`. 최신 `gpu-proof/result.json`은 원본 register가 같고 space가 다른 CBV 두 개, descriptor table, compute, VS/PS varying, depth를 실제 Wine/Metal GPU readback으로 검증한다. 외부 shader compiler가 로드되지 않았는지도 runner가 검사한다. 직전 정식 build의 raster·IA·static sampler 3개 suite와 최신 compiler 개발 build의 동일 3개 suite도 통과했다.
- Register-space 회귀: `b0,space2`와 `b0,space9`가 둘 다 두 번째 상수를 읽던 결함을 declaration ID 기반 lookup으로 수정했다. 수정 전 실제 GPU 값은 `2468ace0,2468ace0`, 수정 후는 `13579bdf,2468ace0`이다. `tests/owned-d3d12-dxil.cpp`와 `tests/dxil/register-space.{hlsl,dxil}`에 회귀를 보존했다. runner는 이 실행 결과가 없는 stale fixture도 거부한다.
- 실제 ZZZ shader/root signature 16쌍은 자체 compiler와 Metal `newLibrary`를 통과했다. 동일 shader 객체를 두 번 컴파일한 binary도 일치했다. 결과: `build/owned-direct-20260919/probes/captured-roots-final-results.json`. 이것은 해당 16쌍의 GPU 출력 정확성 증명은 아니다.
- 별도 AS 개발 source의 D3D12 GPU probe는 BLAS/TLAS build, zero-scratch refit, clone/compact, postbuild size, 최소 allocation compact를 통과했다. 결과: `build/owned-direct-20260919/probes/rt-as-gpu/wine-probe-fixed.log`. RT shader/DispatchRays 검증은 아니다.
- 격리 ZZZ 실행 중 발견한 sample offset 인덱스, 미변환 gather, conditional discard, constant address-space 오류를 수정했다. 마지막 `run-owned-direct-008`의 60초 관측에서 startup PSO 오류는 사라졌지만 창은 검은 화면이었다. 실행 증거는 `/private/tmp/owned-zzz-probe-01a0b8b9/run-owned-direct-008/`에 보존했다. 실제 게임 렌더링·DLSS·RT 호환 성공으로 판정하지 않는다.

게임 원본과 live Wine prefix는 수정하지 않았다. 테스트는 기존 private clone에서 실행하고 해당 prefix의 wineserver만 종료했다.

후속 `run-owned-direct-009-validation`은 Metal API/GPU validation을 켠 60초 실행이다. validation이 활성화된 로그, draw/Present 진행, backend 오류 부재를 확인했지만 이 결과만으로 픽셀 정확성을 증명하지 않는다.

`run-owned-direct-013-capture/frame.gputrace`는 실제 submission worker에서 GPU queue를 drain한 뒤 캡처한 완결된 trace다. Xcode replay에서 19개 command buffer, 111개 draw, 6개 dispatch와 검은 최종 출력을 확인했다. 캡처 실행에서만 shader validation을 껐다(Metal은 둘의 동시 실행을 거부한다). 임시 capture 계측은 source에서 제거했다. 최신 정식 runtime의 `run-owned-direct-014-canonical`은 API/GPU validation을 켜고 60초 실행했으며 backend 오류·외부 compiler 로드가 없고 private prefix 종료도 성공했다. 이 실행은 새 screenshot을 확보하지 않았으므로 시각적 성공으로 판정하지 않는다.

vendor module을 staging하지 않은 경우 `nvapi64`/`nvngx`를 Wine override로 비활성화한다. 그렇지 않으면 source Wine의 NVAPI가 D3DMetal을 불러올 수 있다. 실제 loader probe에서 두 DLL 모두 `ERROR_MOD_NOT_FOUND(126)`로 거부됨을 확인했다. 선택한 경우에만 자체 builtin DLL을 로드한다.

이 테스트는 graphics-translator의 owned D3D12/DXGI 경로만 확인하는 작은 Windows PE이다. NGX, DLSS, GPTK, D3DMetal API를 호출하지 않는다. 실행 성공을 실제 owned backend 성공으로 취급하기 전에 d3d12.dll의 동적 export OwnedGraphicsBackendId를 반드시 확인한다.

## 소스와 참조 provenance

- 테스트: graphics-translator/tests/owned-d3d12-smoke.cpp
- 그래픽 셰이더: graphics-translator/tests/owned-d3d12-graphics.hlsl
- 컴퓨트 셰이더: graphics-translator/tests/owned-d3d12-compute.hlsl
- 읽기 전용 DXMT 참조: build/dlss-translator-20260918/contract/dxmt
- 확인한 DXMT commit: 7c8dee1c2d73415301ceb7d1fa810861cef4cd67

DXMT 참조는 구현 계약과 미완성 영역을 확인하는 용도로만 읽었다. upstream 기여나 upstream 코드 수정은 하지 않는다.

## backend identity 계약

owned d3d12.dll은 다음 ABI를 export해야 한다.

    extern "C" __declspec(dllexport)
    const char* WINAPI OwnedGraphicsBackendId(void);

반환 문자열은 정확히 다음 값이어야 한다.

    owned-graphics-d3d12

테스트는 이 심볼을 정적 import하지 않고 GetProcAddress로 찾는다. export가 없거나 반환값이 다르면 즉시 nonzero로 실패한다. 따라서 기존 D3DMetal이나 시스템 D3D12가 우연히 API smoke를 통과해 owned backend 성공으로 기록되는 경로는 없다.

## 테스트 내용

기본 모드는 8프레임 동안 다음 경로를 실행한다.

1. 실제 로드된 d3d12.dll/dxgi.dll 경로를 출력하고 OwnedGraphicsBackendId를 CreateDXGIFactory1보다 먼저 검증한다.
2. CreateDXGIFactory1로 adapter를 열고 동일 adapter에서 D3D12CreateDevice를 만든 뒤 DXGI_ADAPTER_DESC1::Description을 출력한다.
3. HLSL 파일을 메모리로 읽은 뒤 D3DCompile로 vs_5_0, ps_5_0, cs_5_0 DXBC를 만든다.
4. graphics PSO/root constants로 16x16 R8G8B8A8_UNORM render target에 frame별 4색 pattern을 draw한다.
5. render target을 RENDER_TARGET -> NON_PIXEL_SHADER_RESOURCE로 전이한다.
6. compute PSO/root descriptor table/root constants를 바인드하고 SRV를 읽어 RWStructuredBuffer<uint>에 frame/pixel tag를 기록한다.
7. result buffer를 UNORDERED_ACCESS -> COPY_SOURCE로 전이하고 readback heap으로 CopyBufferRegion한다.
8. 기본 Present 모드에서는 current backbuffer를 GetBuffer로 잡은 채 target을 COPY_SOURCE, backbuffer를 COPY_DEST로 전이하고 CopyResource로 실제 current backbuffer에 draw 결과를 복사한다.
9. 같은 current backbuffer를 COPY_SOURCE로 전이해 texture readback heap으로 CopyTextureRegion하고 다시 PRESENT 상태로 전이한다.
10. command queue fence를 기다린 뒤 compute 결과 256개 uint32_t와 Present 직전 backbuffer의 16x16 RGBA8 전 픽셀을 각각 CPU expected 값과 bit-exact 비교한다.
11. 같은 backbuffer ComPtr를 유지한 채 Present(0, 0)를 호출하고 IDXGISwapChain3::GetCurrentBackBufferIndex()가 매 frame 다음 index로 이동하는지 확인한다.
12. 8번째 마지막 Present 이후 같은 D3D12 queue에 fence Signal/Wait를 추가해 Present command buffer까지 drain된 뒤 teardown되도록 한다.

셰이더 출력은 0 또는 1 채널만 사용하고 compute 단계는 > 0.5 비교로 4-bit mask를 만든다. 따라서 색상 float 오차에 기대는 허용 오차 검사가 아니라 최종 readback의 exact uint32_t를 검사한다.

--no-present는 window/swapchain 부분만 생략한다. device, queue, graphics draw, resource transition 호출, compute dispatch, descriptor/root binding, copy/readback, fence, 8-frame exact validation은 그대로 실행된다.

Present 모드의 final-consumer proof는 CPU-side render target 값만 보는 것이 아니다. 실제 GetCurrentBackBufferIndex()로 고른 swapchain backbuffer에 CopyResource한 뒤 그 backbuffer 자체를 exact readback하고, 동일 backbuffer 객체를 Present에 넘긴다. 따라서 PASS의 present_backbuffer=exact는 Present가 소비할 resource에 예상 frame이 들어갔음을 의미한다. 마지막 Present 뒤 queue drain까지 완료되어야 최종 PASS로 진행한다.

--barrier-stress를 추가하면 기본 8-frame 검증 뒤 같은 command list 안에서 compute producer dispatch -> UAV barrier -> compute consumer dispatch를 실행한다. 두 dispatch는 같은 RWStructuredBuffer의 각 element에 대해 write -> read/modify/write 의존성을 갖는다. 최종 256개 uint32_t를 CPU에서 bit-exact 검증하므로 UAV barrier가 실제 ordering/visibility를 만들지 못하면 실패한다.

## 빌드

generic bin/clang++는 이 호스트에서 기본 target이 macOS이므로 MinGW target을 명시해야 Windows 헤더와 import library를 찾는다.

    mkdir -p build/owned-graphics-20260919/smoke-build
    cp graphics-translator/tests/owned-d3d12-graphics.hlsl \
      build/owned-graphics-20260919/smoke-build/
    cp graphics-translator/tests/owned-d3d12-compute.hlsl \
      build/owned-graphics-20260919/smoke-build/

    /opt/llvm-mingw-20260616-ucrt-macos-universal/bin/clang++ \
      --target=x86_64-w64-mingw32 \
      -std=c++17 -O2 -Wall -Wextra -municode -static \
      graphics-translator/tests/owned-d3d12-smoke.cpp \
      -o build/owned-graphics-20260919/smoke-build/owned-d3d12-smoke.exe \
      -ld3d12 -ldxgi -ld3dcompiler -luuid

현재 CPU-side 검증에서 산출물은 PE32+ executable (console) x86-64, for MS Windows로 확인했다. import에는 d3d12.dll!D3D12CreateDevice, dxgi.dll!CreateDXGIFactory1, D3DCOMPILER_47.dll!D3DCompile이 있고 NGX/GPTK import는 없다. OwnedGraphicsBackendId는 의도대로 동적 lookup이므로 import table에 나타나지 않는다.

HLSL은 Wine d3dcompiler 경로가 다루는 Shader Model 5.0 범위만 사용한다. include, SM6/DXIL, wave intrinsic, bindless 문법은 쓰지 않고 Texture2D SRV, RWStructuredBuffer UAV, root constant에 대응하는 b0 cbuffer와 SV_VertexID/SV_DispatchThreadID만 사용한다.

## 실행 모드

기본 present 모드:

    owned-d3d12-smoke.exe

bounded headless subset:

    owned-d3d12-smoke.exe --no-present

UAV barrier ordering stress:

    owned-d3d12-smoke.exe --barrier-stress

present 없이 barrier만 추가해 확인:

    owned-d3d12-smoke.exe --no-present --barrier-stress

HLSL을 exe와 다른 폴더에서 읽어야 할 때:

    owned-d3d12-smoke.exe --shader-dir Z:\path\to\shader\folder

성공 시 마지막 줄은 다음과 같다.

    OWNED_D3D12_SMOKE_PASS

실패는 OWNED_D3D12_SMOKE_FAIL 또는 OWNED_D3D12_SMOKE_MISMATCH와 함께 nonzero exit code를 반환한다. 실제 GPU/Wine 실행은 이 worker 검증 범위에 포함하지 않았다.

## owned translator가 제공해야 하는 기능

이 smoke가 통과하려면 최소한 다음 D3D12/DXGI 기능이 실제로 동작해야 한다.

- hardware adapter enumeration과 D3D12CreateDevice
- direct command queue, allocator, graphics command list reset/close/execute
- committed default/readback buffer, render-target texture
- RTV와 shader-visible CBV/SRV/UAV descriptor heap
- root signature 1.0 serialization/creation
- graphics PSO + DrawInstanced
- compute PSO + Dispatch
- graphics/compute 32-bit root constants
- compute descriptor table의 texture SRV + structured-buffer UAV
- ResourceBarrier transition 호출
- optional --barrier-stress에서 same-list UAV barrier의 compute write -> read ordering
- default-buffer CopyBufferRegion -> readback heap
- render target -> current swapchain backbuffer CopyResource
- current backbuffer -> placed-footprint readback CopyTextureRegion과 exact RGBA8 검증
- fence Signal, GetCompletedValue, SetEventOnCompletion
- CreateSwapChainForHwnd, GetBuffer, Present, IDXGISwapChain3::GetCurrentBackBufferIndex
- 마지막 Present 이후 동일 command queue fence drain

## pinned DXMT D3D12에서 확인한 실제 미완성 지점

7c8dee1c... 기준으로 smoke에 직접 관련된 가장 큰 gap은 src/d3d12/d3d12_command_list.cpp:1023-1026이다. ResourceBarrier가 TODO이며 모든 barrier를 무시한다. 따라서 현재 DXMT 참조에서 같은 readback이 성공하더라도 D3D12 resource-state tracking이 맞다는 증거가 되지 않는다. owned translator에서는 최소한 이 smoke가 사용하는 RENDER_TARGET <-> NON_PIXEL_SHADER_RESOURCE와 UNORDERED_ACCESS <-> COPY_SOURCE 전이를 올바르게 처리해야 한다.

반대로 같은 commit에서 DrawInstanced (d3d12_command_list.cpp:495+), Dispatch (:608+), CopyBufferRegion (:630+), command-list 실행 (d3d12_command_queue.cpp:201+), fence signal/event 경로, Present1 (d3d12_swapchain.cpp:705+)과 GetCurrentBackBufferIndex (:830+)에는 구체 구현이 있다.

추가로 bundle, tiled-resource 계열, 일부 swapchain property API 등에는 IMPLEMENT_ME가 남아 있지만 이 smoke는 그 기능들을 요구하지 않는다. 이 테스트의 통과는 여기 적힌 좁은 D3D12/DXGI 계약만 검증하며, 실제 게임 호환성이나 일반 shader workload coverage를 뜻하지 않는다.

## fixture runner suite

`scripts/test-owned-graphics.py`는 staged owned runtime에서 fixture PE를 fresh isolated prefix로 실행하고 증거를 남기는 공용 runner다. `--suite`로 다음을 구분한다.

| suite | fixture | 비고 |
| --- | --- | --- |
| `smoke` | `owned-d3d12-smoke.exe` | `--barrier-stress`와 `--no-present`/`--present` |
| `multiqueue` | `owned-d3d12-multiqueue.exe` | |
| `capabilities` | `owned-capabilities.exe` | |
| `temporal` | `owned-d3d12-temporal*.exe` | trace analysis 수행 |
| `ngx` / `ngx-contracts` | NGX fixture | temporal backend identity line 공유 |
| `core-contracts` / `fence-events` / `bundles` / `queries` / `resources` | 대응 fixture | `OWNED_D3D12_*` marker |
| `pipeline-cache` | `owned-d3d12-pipeline-cache.exe` | `--dxil-dir`의 `pipeline-cache-a.dxil pipeline-cache-b.dxil`; 손상/PSO mismatch 거부와 cache 메모리 수명의 실제 GPU 검증 |
| `dxil` | `owned-d3d12-dxil.exe` | `--dxil-dir`의 `compute.dxil vertex.dxil pixel.dxil`을 argv로 전달하고, 함께 freeze한 `register-space.dxil`로 동일 register·다른 space의 실제 GPU 값을 검사 |
| `dxil-ia` | vertex IA fixture | 공용 DXIL 3개와 fixture 옆 `fixture-vs.dxil fixture-ps.dxil`을 함께 freeze하고 실제 stride/indexed draw marker를 정확히 검증 |
| `dxil-static-sampler` | static sampler fixture | fixture 옆 `vs.dxil ps.dxil`을 freeze하고 원본 64-DWORD root signature, visibility/space, RGBA readback을 검증. 자체 compiler는 SDK용 hidden root parameter를 추가하지 않음 |

공통 규칙은 stdout의 `<PREFIX>_BACKEND id=owned-graphics-d3d12`(NGX는 temporal prefix)와 `<PREFIX>_PASS`이며, 독립형 `dxil-static-sampler`만 backend line 대신 exact GPU marker와 `_PASS`를 요구한다. dyld trace에서 owned native module `lib/wine/x86_64-unix/winemetal.so`이 실제로 로드되었는지 확인하고, foreign D3DMetal/`libd3dshared` 로드가 있으면 실패로 판정한다. owned DXIL compiler는 이 module 안에 static link되어 있으므로 DXIL suite도 별도 compiler dylib을 요구하지 않고, 그 module이 stage된 runtime의 파일이고 dyld가 그 경로를 실제로 로드한 경우에만 각 fixture의 실제 계약(compute/descriptor-table/varying/depth, `dxil-ia`의 IA 결과, `dxil-static-sampler`의 static-sampler marker)을 평가한다. runner의 `--lock`은 실행 evidence에 경로/hash로만 기록된다. 아직 실패 중인 typed descriptor fixture는 성공 gate로 광고하지 않는다. descriptor table의 `E_NOTIMPL` 또는 unsupported 보고는 성공으로 인정하지 않는다. CPU-only 회귀 검사는 `scripts/test-owned-graphics-diagnostics.py`가 담당하며 성공 문자열만으로 PASS가 되지 않는지 확인한다.
