# 구현 이력

### 후속 P0 공개 계약 — 2026-10-02

Docs-first 우선순위 commit **5f88633** 뒤 P0-1–4를 병렬 수정하고 새 **p0-contracts-r5KJ0O/runtime-01**로 닫았다. 과거 P0 개발 단계와 다른 묶음이다. 현재 계약·정확한 결과 경로·검증 한계는 [CORE P0 ledger](CORE-IMPLEMENTATION-STATUS.ko.md#p0-공개-계약-정합), 반복 절차는 [SMOKE](SMOKE-TEST.ko.md#p0-공개-계약-fixture-수동-실행)를 따른다.

- 변경 전: P0-1 public Register assert 중단, P0-2 없는 LUID에 adapter0 성공 반환, P0-3 Resource1 QI E_NOINTERFACE, P0-4 core 양 ABI62 failures·NGX nonowned adapter1 failure를 보존했다. Static finding·실제 CPU 재현·GPU oracle correction을 구분한다.
- 변경 후: P0-1 양 ABI event/cookie/ownership CPU PASS, P0-2 양 ABI5 groups·adapter1 CPU PASS, P0-3 양 ABI31 descriptor/creation/shared/GPU cases PASS, P0-4 core 양 ABI80 checks·NGX x64-only12 checks PASS. 실제 selected38 Feature IDs와 기존 private OPTIONS21을 대조했으며 최신 SDK 전체 enum 지원을 선언하지 않았다.
- Main의 같은 runtime VRS 각146·sharing 각20 cases를 x64/x86 **API+Shader Validation**으로 다시 실행했다. OPTIONS10 true/true·ExistingHeaps true는 기존 실제 GPU consumer와 맞춘 보고이며 새 VRS/heap 기능을 구현한 것으로 기록하지 않는다. Manifest SHA256 **a2f635535aa52e08abf2c3a3a31819626142f42c4bfad910aa056fe7e97b86f0**와 build/install/stage 로그를 보존했다.
- P0-3 after02 실패는 enhanced-layout texture에 COMMON handoff 없이 legacy barrier를 쓴 **잘못된 fixture interop**이다. [공식 계약](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html#interop-with-legacy-resourcebarrier)에 따라 APIs3/6/9의 fixture를 enhanced COPY_DEST→COPY_SOURCE로 수정했고 readback을 유지한 after03 양 ABI가 통과했다. Production source는 바뀌지 않았으며 실패 로그를 삭제하거나 production barrier 결함으로 바꾸지 않았다.
- Resource2 runtime의 explicit WIDL sret과 양 ABI public GetDesc1() 호출을 실제 연결했다. Null-output HRESULT는 프로젝트 안전 경계이며 native-Windows differential proof가 아니다. Native protection·native no-temporal 장치·multi-GPU·다른 GPU/OS·public NGX x86·게임/전체 conformance는 별도 한계다. ARM64/FEX guest는 사용자 보류를 유지했다.
- 시험 소유 prefix만 stop/wait/remove했다. 최신 P0 runtime-01과 비교 full-metalfx-port-20261002/runtime-02는 유지한다. 오래된 parallel-metalfx-cache-20261002/runtime-after의 bin/lib/share payload는 Main이 디스크 정책에 따라 정리하며 manifest/provenance/기존 증거는 보존한다. 이 payload 정리는 과거 실행 결과의 취소나 새 실행이 아니다.

