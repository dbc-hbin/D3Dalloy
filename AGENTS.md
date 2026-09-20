# dxmt-dx12 에이전트 지침

## 작업 기준

- 현재 소스는 이 저장소 루트다. `src/`를 직접 수정하고 README.md의 Meson 명령으로 빌드한다.
- `graphics-translator/patches/`와 `upstream.lock.json`은 import 당시 provenance다. 편집 대상이 아니며, 루트 소스를 바꿀 때 다시 패치로 역수출하지 않는다.
- `scripts/build-owned-graphics.py`는 frozen baseline 재현용이다. 현재 코드 빌드에 사용하지 않는다.
- `include/native/directx`, `external/nvapi`는 vendored dependency다. revision을 임의로 바꾸지 않는다.
- 변경 범위는 요청에 한정한다. 관련 없는 리팩터링, 새 추상화, telemetry, 호환 shim을 덧붙이지 않는다.

## 금지

- 게임 설치 디렉터리, live Wine prefix, 사용자 런처 runtime을 수정하지 않는다. staging과 GPU 검증은 caller가 지정한 **새 runtime 복사본**과 **전용 prefix**에서만 한다.
- 외부 D3DMetal/GPTK/D3DShared, Apple Metal Shader Converter, SPIR-V 경로를 fallback으로 추가하지 않는다. 번역기와 DXIL compiler는 자체 구현을 유지한다.
- 근거 없이 게임 렌더링/호환성을 주장하지 않는다. fixture 성공은 D3D12 conformance가 아니며 RaytracingTier 0을 지원 완료로 표현하지 않는다.

## 검증

- CPU/tooling: `python3 scripts/test-owned-graphics-build.py`, `test-owned-graphics-staging.py`, `test-owned-graphics-diagnostics.py`.
- GPU: 새 runtime을 stage한 뒤 `python3 scripts/test-owned-graphics.py --runtime <runtime> --suite <suite> --fixture <exe> --out <새 디렉터리> --validation`. Runner는 기존 `--out`을 거부하므로 evidence는 항상 새 경로에 남긴다.
- Metal API/GPU/Shader validation 오류, 비정상 command completion, 외부 D3DMetal/`libd3dshared` 로드는 실패다. stdout 성공 문자열만으로 PASS가 되지 않는다.
- 동작 변경은 affected suite를 실제 실행해 확인한다. 통과한 검증을 이유 없이 반복하지 않는다.

## 소스 규칙

- MSL/AIR/metallib 공용 계약을 바꾸면 PE caller, Unix thunk, native consumer를 같은 header로 함께 재빌드한다. module 단독 교체는 ABI 불일치다.
- cache format, unix call 번호, module UUID 검사를 바꾸면 관련 검증기와 문서를 같은 변경에서 갱신한다.
- native module은 `@loader_path`로 재배치 가능해야 한다. build-tree absolute `LC_RPATH`를 install 결과에 남기지 않는다.
