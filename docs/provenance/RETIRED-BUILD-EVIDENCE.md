# 은퇴한 build root와 보존된 증거

이 저장소의 `graphics-translator/*.ko.md`에는 2026-09-19 ~ 09-20 작업 당시의 `build/owned-*` 경로가 인용되어 있다.
그 root들은 현재 canonical runtime과 이 저장소의 루트 소스로 대체되었고, 원본 작업 폴더(`zzz-wine-d3dmetal-dx12`)에서
디스크 사용량 때문에 정리했다. 문서에 남은 `build/owned-*` 경로는 **역사적 provenance 표기**이며 현재 파일 존재를 뜻하지 않는다.

## 이 저장소로 옮긴 위치

- 보존 증거: `build/retired-evidence/<원래 root 경로>/`
  각 root에서 4MB 이하의 `.json`/`.log`/`.md`/`.txt` 파일만 옮겨 보존했다. per-run Wine runtime 복사본, test prefix,
  대용량 binary는 삭제했다. 은퇴·정리 내역은 `build/retired-evidence/retirement-manifest.json`에 있다.
- 이전 작업 root: `build/legacy/owned-graphics-20260919`, `build/legacy/owned-direct-canonical-cache-20260921{,-development}`
  소스 snapshot, patch 역수출 입력, 이전 build/runtime 기록을 그대로 보존한 역사적 root다. 현재 빌드 입력이 아니다.
- 정리 대상이었던 root: `owned-continuation-20260919`, `owned-completion-20260919`, `owned-direct-20260919`,
  `owned-direct-canonical-{20260920,final-clean-20260920,space-20260920,validated-20260920,rt-final-20260920,icb-20260920}`,
  `owned-workers`와 `owned-graphics-20260919`/`dlss-translator-20260918`의 per-run runtime 하위 디렉터리.

## 현재 입력

- toolchain: `toolchains/llvm-15`, `toolchains/tools`, `toolchains/wine-sdk` (Git 제외)
- staging 기준 runtime: `toolchains/wine-runtime`
- 현재 소스에서 다시 얻은 결과: `docs/provenance/*.json`

과거 `zzz-wine-d3dmetal-dx12` 쪽에 남아 있는 Wine/D3DMetal 작업 폴더는 이 저장소 소관이 아니다.
