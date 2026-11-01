# 문서 안내

기준일: 2026-10-09. 루트 `src/`가 구현의 기준이며, 개별 fixture나 한 게임의 성공을 전체 호환성으로 확대하지 않는다.

| 내용 | 기준 문서 |
| --- | --- |
| 현재 지원·검증 한계 | [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md) |
| 남은 구현·성능 검증 | [PRIORITIES](../graphics-translator/IMPLEMENTATION-PRIORITIES.ko.md) |
| 빌드·도구·디버깅 | [루트 빌드 절차](../README.md#building), [DEVELOPMENT](DEVELOPMENT.md) |
| 설정과 적용 범위 | [CUSTOMIZATION](CUSTOMIZATION.md) |
| 외부 Wine/FEX와 ARM64 통합 | [ARM64-FEX-IMPLEMENTATION](ARM64-FEX-IMPLEMENTATION.md) |
| 완료된 R1~R6 수정·R7 계측과 남은 계약 | [리뷰 후속 결과](D3D12-REVIEW-AND-DESIGN-2026-10-07.md) |
| FEX 병목 수치·최적화·검토 요약 | [FEX-PERFORMANCE](FEX-PERFORMANCE-2026-10-09.md) |
| 격리 실행·fixture 절차 | [RUNTIME](../graphics-translator/RUNTIME.ko.md), [SMOKE](../graphics-translator/SMOKE-TEST.ko.md) |
| 실게임 DLSS·프리컴파일 관찰 | [ZZZ](../graphics-translator/ZZZ-COMPATIBILITY.ko.md) |
| 과거 증거·현재 보유 범위 | [HISTORY](../graphics-translator/IMPLEMENTATION-HISTORY.ko.md), [증거 가용성](provenance/RETIRED-BUILD-EVIDENCE.md) |

`provenance/*.json`은 frozen import와 당시 실행의 원본 기록이다. 내부 경로·해시를 현재 파일 목록에 맞춰 고치지 않는다. 과거 build 결과 상당수는 디스크 정리로 사라졌으며, 문서의 PASS 기록이 원본 보유나 최신 재실행을 뜻하지 않는다.

문서 검사는 저장소 루트에서 `python3 -B scripts/test-owned-documentation.py`로 실행한다. 원본 증거·빌드 산출물·캡처는 문서와 분리하고, 완료된 작업 상태나 중복 진행 보고서는 다시 만들지 않는다.
