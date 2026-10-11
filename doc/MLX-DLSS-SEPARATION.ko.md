# MLX-DLSS 작업 분리 기록

기준일: 2026-10-11

`dxmt-d3d12`의 MLX-DLSS 작업은 별도 로컬 저장소
`/Users/hanbinnoh/Documents/dxmt-dlss`로 분리했다. 원본 저장소에는 이 작업을 다시 빌드하거나 선택하는 경로를 남기지 않는다. 새 저장소는 아직 commit, remote, push를 만들지 않았다.

## 제거한 원본 경계

다음은 새 저장소로 보존 복사한 뒤 `dxmt-d3d12`에서 삭제했다.

- Swift 공통 bridge: `src/winemetal/unix/DLSSModelBridge/`
- native bridge ABI/loader: `dlss_model_bridge.h`, `dlss_model_bridge_loader.*`
- MLX-DLSS FG graph와 Metal4 외부 작업 helper: `dlss_frame_generation.*`, `dlss-fg-*`, `metal4_external_work.*`
- RR/FG 전용 도구: `scripts/rr_mlx/`, `scripts/test-owned-dlss-rr.py`, `scripts/extract-owned-dlss-fg.py`, `scripts/test-owned-dlss-fg-weights.py`
- MLX Meson 설정, native asset staging, 전용 build/test 문서와 모델 선택 경로
- Swift/MLX/RR/FG/SR·NR 조사에 사용한 `build/local/dlss-*` 전용 산출물
- MLX C++ source/install toolchain: `toolchains/mlx/`, `toolchains/mlx-install/`

`src/winemetal/unix/winemetal_metal4_fx.mm`와 `graphics-translator/tests/owned-fg-debug-native.mm`는 MLX-DLSS bridge/helper 추가분을 제거하고 기존 기준으로 복원했다. 공통 MetalFX temporal backend, 기존 MetalFX RR, D3D12/NGX/FSR 계약은 이 분리의 대상이 아니므로 유지했다.

## 새 저장소의 대응 위치

- Swift bridge: 저장소 루트 `Package.swift`, `Sources/DLSSModelBridge/`, `Tests/`
- DXMT 연결 문맥 snapshot: `integrations/dxmt/`
- FG: `models/fg/`
- SR·NR 조사/패키지: `models/sr/`, `models/nr/`
- RR: `models/rr/`
- 실행 로그와 검증 자료: `evidence/`
- 전체 로컬 replay/build 입력: 각 모델의 `build/local/` 및 `toolchains/`

세부 hash와 원본-목적지 대응은 새 저장소의 `SEPARATION-MANIFEST.md`에 기록했다. 대형 weight, DLL, `.bin`, capture, toolchain은 새 저장소의 `.gitignore`로 추적하지 않는다.

## 확인된 원칙

- `dxmt-d3d12`의 기본 backend는 계속 MetalFX다.
- MLX-DLSS SR/FG/NR/RR 통합은 새 저장소에서 공통 Swift bridge와 각 모델 adapter의 실제 encode 계약을 완성한 뒤, 검토된 DXMT integration snapshot을 기준으로 다시 연결한다.
- package/weight 형식 검증만으로 native inference 지원을 선언하지 않는다. 실제 Metal texture 입력, GPU ordering, history/lifetime, 오류 전파 증거가 있어야 한다.
- 원본에 남은 일반 `DLSS`/`RR` 역사 문서는 MLX-DLSS 구현 경로가 아니므로 삭제하지 않았다.

## 원본 정리 검증

다음 검색에서 `src/`, `scripts/`, `README.md`, `doc/`, `graphics-translator/`에 MLX-DLSS bridge·loader·model selector·MLX staging 참조가 없어야 한다.

```sh
grep -R -n -E 'DLSSModelBridge|dlss_model_bridge|dlss_frame_generation|metal4_external_work|DXMT_DLSS_FG_MODEL|native_mlx|native_dlss_bridge|rr_mlx|MLX-DLSS' \
  src scripts README.md doc graphics-translator
```

분리 후 검증 결과는 다음과 같다.

- 원본의 MLX bridge/loader/model-selector/helper 참조 검색: 0건 (`src/`, `scripts/`, `README.md`, `doc/`, `graphics-translator/`의 새 분리 문서 제외)
- `python3 -m py_compile scripts/stage-owned-graphics.py scripts/test-owned-graphics-staging.py`: PASS
- `python3 scripts/test-owned-graphics-staging.py`: 49 tests, PASS
- `git diff --check`: PASS
- clangd diagnostics for `src/winemetal/unix/metalfx_frame_generation.mm`: PASS
- destination artifact existence/ignore checks: PASS

새 저장소의 Swift contract, FG weight, RR PTX/CLI 검사는 `dxmt-dlss/SEPARATION-MANIFEST.md`에 기록된 상대 경로에서 다시 실행한다.
