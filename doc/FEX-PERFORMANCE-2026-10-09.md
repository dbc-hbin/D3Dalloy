# FEX 병목 조사와 최적화 결과

2026-10-09의 조사·구현·독립 검토를 통합한 기록이다. 중복 보고서와 완료된 작업 상태는 제거했다. 현재 DXMT 지원은 [CORE](../graphics-translator/CORE-IMPLEMENTATION-STATUS.ko.md), DLSS·셰이더 프리컴파일 진단은 [ZZZ 기록](../graphics-translator/ZZZ-COMPATIBILITY.ko.md)을 따른다.

## 결론

FEX 자체의 코드 생성·IR 처리·ARM64EC 전환은 실제 CPU 비용을 쓴다. 그중 반복 IR 블록 탐색을 수정해 **동일 입력의 작업량과 CPU 비용 감소**를 입증했다. 전체 게임 로딩 시간·FPS·스터터링 개선율은 아직 측정하지 못했다. 디스크 캐시 기본값과 EC 전환 보호는 바꾸지 않았다.

## 실제 게임 CPU 관측

도시 진입 버튼 조작 시작 뒤 각 40초의 Time Profiler Running 표본 가중치 합계다. 단위는 모든 게임 스레드를 합한 CPU초이며, 외부 Metal 컴파일 서비스는 분모에 없다. 같은 길이의 구간이지 같은 양의 순수 로딩 작업이 아니다. ON 반복에는 도시 렌더링이 더 많이 포함됐다.

| 항목 | 캐시 OFF 첫 실행 | OFF 반복 | ON 반복 |
| --- | ---: | ---: | ---: |
| 게임 전체 | 86.929 | 98.169 | 103.608 |
| FEX 자체, 배타적 | 15.819 | 16.506 | 14.467 |
| FEX 자체 비중 | 18.20% | 16.81% | 13.96% |
| ExitFunctionEC, FEX 자체에 포함 | 3.185 | 3.841 | 3.953 |
| 코드 생성 관련 프레임 포함, 별도 가산 금지 | 9.659 | 9.614 | 7.640 |
| 네이티브 winemetal 컴파일러·브리지 | 2.584 | 0.355 | 0.357 |
| 스택 누락·미분류·맵 충돌 | 11.427 | 13.274 | 14.251 |

- **FEX 비중은 성능 손실률이나 로딩 지연 비율이 아니다.** 번역된 게임 코드의 실행은 FEX 자체와 별도로 분류했다. winemetal/AIR/LLVM은 ARM64 네이티브였다.
- DiskCache OFF도 프로세스 내부 JIT 재사용은 유지한다. ON 반복의 `LoadCachedCode` 성공 경로 표본은 캐시 코드 사용의 근거일 뿐, 적중률이나 이전 프로세스의 파일 재사용을 증명하지 않는다.
- 화면으로 좁힌 도시 도달 범위는 OFF 반복 24.5~56.0초, ON 반복 13.5~33.0초로 겹친다. 게임 자산·OS·그래픽 캐시·네트워크·조작 대기가 통제되지 않아 단축률을 계산하지 않았다.
- 조사 런처의 GPU 이름 override 오류는 후속 QA에서 발견했다. 이 조사와 초기 후보 프로파일을 정상 DLSS 전후 성능 비교로 사용하지 않는다.

원본 수치·분류·재계산 입력: [cpu-attribution.json](../build/local/zzz-fex-research-20261009/cpu-attribution.json), [analysis-config.json](../build/local/zzz-fex-research-20261009/analysis-config.json), [analyze-time.py](../build/local/zzz-fex-research-20261009/analyze-time.py), [identity.sha256](../build/local/zzz-fex-research-20261009/identity.sha256). ARM64EC 심볼 별칭 누락을 수정하고 실제 RVA 및 배타적·스레드·구간 합계를 대조한 결과다.

## 적용한 최적화

FEX 커밋 `206d61d07`은 `IREmitter.cpp/.h`의 10줄 추가·2줄 삭제다. OFF 반복에서 `CreateNewCodeBlockAfter`의 배타적 표본 2.133 CPU초 중 대부분이 연결 리스트 탐색에 있었다. 도달 가능한 끝 노드를 캐시하되 후속 명시적 삽입은 계속 따라가며, reset·copy·캐시 노드 제거 시 무효화한다. IR 순서·커서·할당 순서와 SMC 경로를 보존한다.

| 동일 입력 8,192개 블록 | 기존 탐색 횟수 | 수정 후 | 기존 CPU 중앙값 | 수정 후 |
| --- | ---: | ---: | ---: | ---: |
| Head-fixed | 100,651,008 | 8,191 | 113.993ms | 0.108ms |
| Alternating | 125,806,592 | 8,191 | 171.431ms | 0.207ms |

당시 기록에 따르면 실제 emitter를 사용한 baseline·candidate·소스의 구조 해시는 `4f9b6c042a7a22d4`로 같았다. 다섯 번의 고정 작업 측정이며 시간 임계값으로 테스트를 통과시키지 않았다는 보고였다. 위 표와 이 해시는 **2026-10-09의 역사적 기록**이다. 원래 증거 위치는 별도 FEX 저장소의 `build/local/ir-tail-proof-20261009/compare-result.json` 및 `check-result.json`이지만, **2026-11-02 확인 시 두 파일이 없어 현재 재검증할 수 없다**. 파일을 복원하거나 수치를 새로 입증한 것으로 취급하지 않는다. 아래 D3Dalloy에 남은 최종 결과·동일성 자료와 이 누락된 emitter 원본은 구분한다.

## 검증과 독립 검토

당시 독립 검토는 다음 C1~C3 모두 **APPROVE, blocker 0**이었다. 아래는 원본 검토의 요약이며 문서 이전 중 검토나 게임을 다시 실행한 결과가 아니다.

| 기준 | 확인한 범위 |
| --- | --- |
| C1 정확성·작업량 | 실제 emitter 순서·삽입·삭제·reset/copy·SMC 회귀와 ASan/UBSan 통과. ARM64EC/WoW64 빌드·PE 검사 및 Wine CPU/ABI 34개 회귀 통과. |
| C2 실게임 | 격리 ZZZ 시작 화면과 도시 실내 렌더링. DLSS/성능 우선, FG·RT 끄기, MetalFX 624×360→1280×720. NGX parse result 1, backend hr 0. |
| C3 동일성·정리 | 검증 소스와 빌드 DLL 일치. 세 게임 정상 종료, 전용 prefix·복제 게임·진단 앱·소유 임시 JIT 맵 정리. 실제 설치 게임과 live prefix는 미수정. |

[최종 결과](../build/local/zzz-fex-opt-20261009/result.json), [DLSS 설정](../build/local/zzz-fex-opt-20261009/candidate-03/dlss-settings-confirmed.png), [실제 출력](../build/local/zzz-fex-opt-20261009/candidate-03/city-dlss-01.png), [동일성](../build/local/zzz-fex-opt-20261009/final-identity.txt), [정리 기록](../build/local/zzz-fex-opt-20261009/cleanup.txt)을 보존한다. 최신 후보는 `build/local/zzz-fex-opt-20261009/runtime`, 비교 기준은 DXMT `cache-04`다. 빌드 당시 기준 HEAD는 `fbd54488a`와 emitter 변경분이며 이후 `206d61d07`로 커밋했다.

검증 범위는 한 장면의 렌더링 회귀다. 다른 시각의 같은 화면을 여러 카메라 각도로 세지 않았으며, 명시적 Metal validation-layer·장시간 플레이·전체 precompile 재실행·성능 향상은 주장하지 않는다.

재검사 명령은 FEX 저장소 기준 `python3 Darwin/tests/fex_ir_tail.py check`다. 빌드는 `python3 Darwin/build.py --output <새 build 경로> --jobs 8`, 통합 검사는 `python3 Darwin/test_wine.py --runtime <검증된 Wine runtime> --build-output <FEX build 경로> --output <새 검사 경로>`를 사용했다. 로컬 회귀 fixture는 FEX 저장소의 기존 정책에 따라 Git 제외 상태다.

## 남은 조사와 적용하지 않은 변경

- **캐시 용량:** 파일이 약 1GiB라는 이유만으로 포화를 단정할 수 없다. cap 거부·miss 원인·실행 버퍼 세대 교체·무효화와 같은 블록의 재요청을 먼저 계측해야 한다. 캐시 기본 ON, 전역 크기 증가, relocation/SMC 검증 우회는 근거가 없다.
- **EC 전환:** `ExitFunctionEC` 표본은 전체 전환 비용이 아니다. FPCR 직후 주소에 표본이 몰려도 FPCR 쓰기의 단독 비용으로 해석할 수 없다. AH/NEP 처리와 suspend doorbell 순서는 보존해야 하며, 조건부 생략은 진입 상태 빈도·결정적 경계 회귀·동일 호출 수 비교가 먼저다.
- **Emitter 분기 패치:** `B`·`TEST_BRANCH`·`BC/RELATIVE_LOAD`의 범위·정렬 검사를 unsigned rotate로 합치는 후보는 미구현이다. 경계 입력, 실패 시 명령어 미변경, 실패 뒤 다른 참조도 패치하는 계약 및 실제 생성 명령 감소가 필요하다. `LONG_ADDRESS_GEN`은 해당 표본에서 근거가 없었다.
- **전체 체감 성능:** 같은 장면·설정·cold/warm 상태의 로딩 milestone과 frame-time, 컴파일·EC 전환 이벤트를 함께 측정해야 한다. 현재 자료만으로 FEX 대 DXMT의 직렬 병목 비율이나 스터터 원인별 비율을 계산할 수 없다.

QA에서는 공백이 있는 `dxgi.customDeviceDesc`를 따옴표 없이 지정해 이름이 `NVIDIA`로 잘리고 DLSS 메뉴가 숨겨진 일이 있었다. 중복 override를 제거해 내장 ZZZ 프로필을 사용하자 복구됐다. [설정 문법](CUSTOMIZATION.md#file-environment-and-precedence)을 따른다. `eErrorFeatureFailedToLoad` 경고만으로 DLSS SR 실패를 판정하지 않는다.

## 관련 DXMT 커밋과 검증

`ec1b197` 문서 정리, `afb9d18` 외부 Wine/FEX 소비·owned NGX 로딩, `1df0505` 코어 단순화, `b7c11e6` native MetalFX FG/RR, `105f82e` 자동 PSO 캐시·셰이더 실패 진단으로 나눠 커밋했다. 568개 소스/빌드 입력이 기존 ARM64·PE32 검증 빌드와 같았고, 문서 검사·ARM64X 4개·패키징 1개·staging 48개·diagnostics 96개·NGX x64 ABI/PE·벡터/유틸 sanitizer·DXGI CPU 검사를 통과했다. 이는 중간 커밋 각각의 전체 GPU 재실행을 뜻하지 않는다. 삭제된 과거 FG/RR 원본 결과는 [역사적 기록](../graphics-translator/IMPLEMENTATION-HISTORY.ko.md#증거를-읽는-기준)으로만 남긴다.
