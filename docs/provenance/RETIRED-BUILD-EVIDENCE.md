# 은퇴한 build root와 보존된 증거

이 저장소의 `graphics-translator/*.ko.md`에는 2026-09-19 ~ 09-20 작업 당시의 `build/owned-*` 경로가 인용되어 있다.
그 root들은 현재 canonical runtime `build/owned-direct-canonical-cache-20260921/`과 이 저장소로 대체되었고, 원본 작업 폴더(`zzz-wine-d3dmetal-dx12`)에서 디스크 사용량 때문에 정리했다.

- 은퇴·정리 내역: `zzz-wine-d3dmetal-dx12/build/retired-evidence/retirement-manifest.json`
- 보존된 증거: `zzz-wine-d3dmetal-dx12/build/retired-evidence/<원래 root 경로>/`
  각 root에서 4MB 이하의 `.json`/`.log`/`.md`/`.txt` 파일만 옮겨 보존했다. per-run Wine runtime 복사본, test prefix, 대용량 binary는 삭제했다.
- 정리 대상: `owned-continuation-20260919`, `owned-completion-20260919`, `owned-direct-20260919`, `owned-direct-canonical-{20260920,final-clean-20260920,space-20260920,validated-20260920,rt-final-20260920,icb-20260920}`, `owned-workers`와 `owned-graphics-20260919`/`dlss-translator-20260918`의 per-run runtime 하위 디렉터리.
- 유지: canonical runtime `owned-direct-canonical-cache-20260921`, toolchain 입력(`owned-graphics-20260919/{llvm,tools,install,dxmt}`), `dlss-translator-20260918/{wine-final,contract}`, `wine-crossover`.

따라서 이 문서들의 `build/owned-*` 경로는 **역사적 provenance 표기**이며 현재 파일 존재를 뜻하지 않는다.
현재 canonical runtime에서 다시 얻은 결과는 `docs/provenance/*.json`에 있다.
