## 판정
최종 주장 집합은 아래처럼 범위를 한정하면 유지할 수 있다. 최신 실행의 컴파일 실패 로그는 확인됐지만, 화면 정지의 원인이나 실패 단계는 확정되지 않았다. 움직이는 게임 HUD는 모든 화면 출력이 동시에 멈췄다는 설명에 반대되는 사용자 관찰이다. 그러나 scene producer, SR, 합성, 개별 queue 중 어느 단계가 정지했는지는 고르지 못한다. 최신 FPS 저하의 크기와 지배적 원인도 미확정이다.

지정된 10개 문서를 모두 읽고, 현재/과거 Player.log, 최신 launch/error 구간, 저장 해상도, NGX 반환 경로와 temporal 소비 dispatch를 추가로 대조했다. 기존 검증 실행 결과를 검토했으며, 게임 실행·빌드·파일 변경·검증 스크립트 재실행은 하지 않았다.

아래 R은 `doc/ulw-research/20261009-075042`, LIVE는 `/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test`, CAND는 `build/local/zzz-fex-opt-20261009/candidate-03`이다.

## 기준별 차단 조건
1. **C1/C2: 최신 실행의 실패 단계 확정은 차단된다.** `brief.md:33-34`의 최신 실행 구분 및 런타임 근거 조건에 비춰, 과거 hr=0이나 최신 HUD metric 등록을 최신 GPU 완료로 대체할 수 없다. 최신 Evaluate 반환값, temporal encode/completion, 입력·출력·최종 화면의 동기화된 내용 변화가 없다. 이 공백을 명시하면 연구 결론은 성립하지만, 특정 원인 확정은 성립하지 않는다.
2. **C2: 컴파일 실패를 장면 정지 원인으로 확정하는 것은 차단된다.** 실패 줄에는 현재 실패한 shader/root 쌍의 식별값, caller, 해당 장면 draw와의 연결, 실패 시각이 없다. 과거 64쌍의 invalid-binding 진단이나 warmup caller를 최신 9,131줄에 이식할 수 없다. 근거: `R/verify-log-counts.md:7-12`, `R/verify-shader-corrections.md:4`, `wave-1-skeptic.md` §5.
3. **C1/C3: 동일 실행 바이너리·동일 workload를 전제로 한 회귀/FPS 비교는 차단된다.** 현재 파일 해시, 기록된 source-cut 항목, 저장 ScreenManager 값은 실행 당시 loaded image 및 실제 SR 크기와 다르다. matched D3DMetal/Rosetta 측정도 없다. 근거: `R/verify-identity-timings.md:1-10`, `R/verify-build-inputs.md:7-12,41,70,99,128,157`, `R/wave-3-verification.md:6`.
4. **C4: 정정 전 worker 문구를 최종 보고서에 재사용하면 안 된다.** 특히 runtime-identity 보고서의 “trace가 unset이었다”, “따라서 3840×2160을 사용했다”, “모든 소스가 같다”는 확장 해석은 정정 문구로 바꿔야 한다. 이 검토는 fresh attack 및 일부 원문 readback을 제공한다. 최종 claim graph/SYNTHESIS/outcome 검증은 이번 배정 범위 밖이므로 C4 전체 PASS를 대신 선언하지 않는다.

## 최종 보고서에 넣을 정확한 문구

### 1. 컴파일 오류 수 — supported, 원인은 unresolved
“최신 `game_1791530994266.log`에는 컴파일 실패 **9,131줄**, 별도 D3D12 sink에는 **9,122줄**이 기록됐다. POSITION_0 input-layout 경고는 각각 **1,881줄**이다. 두 sink는 같은 DXMT 출력의 중복 관측이므로 합산하지 않는다. 차이는 PS SRV 실패 9줄이며 발생 이유는 확인되지 않았다. 이 수치는 고유 shader·PSO·프레임 수가 아니다. 실패한 shader/root 쌍의 식별값, caller와 시각이 없어 장면 정지 구간이나 실제 scene draw 실패에 연결할 수 없다.”
근거: `R/verify-log-counts.md:7-13`; 최신 game log `:76-86`. “해시가 없다”는 문구는 **실패 shader/root 쌍의 해시가 없다**로 한정해야 한다. 로그 파일 SHA256은 확보됐고 초기 shader-probe에도 해시가 있다. 또한 POSITION 경고를 컴파일 실패와 같은 종류의 실패로 묶지 않는다.

### 2. HUD 움직임 — narrowed
“게임 내 위치 HUD가 계속 바뀐다는 사용자 관찰을 전제로 하면, 해당 UI/출력 경로는 갱신을 계속하고 있다. 이는 모든 렌더링과 표시가 동시에 영구 정지했다는 설명과 맞지 않는다. 오래된 장면 입력, 오래된 SR 출력, 잘못 선택된 합성 입력, scene/temporal 경로의 개별 queue 문제는 모두 양립한다. 현재 자료만으로 이들 중 하나를 우선 원인으로 확정하거나 MetalFX stall을 입증할 수 없다.”
근거: `wave-1-skeptic.md` §6; recording/encoding의 분리는 `src/d3d12/d3d12_command_list.cpp:1637-1652`, 소비 branch는 `src/d3d12/d3d12_command_queue.cpp:6434-6481`. “favors”는 전역 정지보다 부분 경로 문제에 부합한다는 뜻으로만 사용하고, 후보들 사이의 입증된 우선순위로 표현하지 않는다. 최신 HUD/scene 동기 영상은 없다.

### 3. NGX trace 부재와 hr=0 — supported, 최신 invocation은 unresolved
“검토한 최신 renderer 로그에는 NGX parse/evaluate 및 temporal record/encode 결과가 없다. launch command에는 trace 활성화가 명시되지 않았고 Player.log는 `diag=0`을 기록한다. 전체 inherited environment는 확보되지 않았으므로 `OWNED_GRAPHICS_TRACE`가 반드시 unset이었다고 단정하지 않는다. trace는 opt-in이고 수량 제한도 있으므로 기록 부재는 호출 부재의 증거가 아니다. 과거 candidate의 `ngx_d3d12_evaluate_backend hr=0`은 그 호출의 준비·명령 기록 수락을 뜻하며 GPU 완료나 출력 내용 갱신을 뜻하지 않는다.”
근거: `LIVE/neutralinojs.log:569`, 최신 Player.log `:8`, `src/d3d12/owned_trace.hpp:19-54`, `src/nvngx/nvngx_d3d12.cpp:1033-1052`, `src/d3d12/d3d12_command_list.cpp:1637-1652`. 소비 dispatch는 Temporal을 명시적으로 처리하고 `EncodeOwnedTemporalFrame`을 호출한다(`d3d12_command_queue.cpp:6434-6481`). 실제 native encode 및 오류 처리는 나중에 실행된다(`d3d12_temporal.cpp:225-243`). 따라서 producer만 확인하고 소비가 누락됐다고 추정할 근거도 없다.

### 4. 현재 파일 identity — supported, loaded identity는 unresolved
“검증 시점의 현재 디스크 파일 5개(aarch64 d3d12.dll/nvngx.dll/winemetal.so, i386 d3d12.dll/nvngx.dll)는 배포 manifest SHA256과 일치했다. 이는 현재 파일과 manifest의 일치이며, 최신 게임 프로세스에 실제 매핑됐던 이미지의 해시 증명은 아니다. 이 5개 검사에는 xtajit64.dll이 포함되지 않는다.”
근거: `R/verify-identity-timings.md:1-10`. “현재 runtime 전체 검증 완료”나 “최신 실행의 NGX/FEX hash 확인”으로 확대하지 않는다.

### 5. 해상도 — saved settings는 supported, 실제 workload는 unresolved
“현재 prefix에는 ScreenManager 3840×2160 및 Use Native=1이 저장돼 있다. 과거 candidate launcher는 1280×720을 강제했고, 과거 NGX trace도 1280×720 output과 624×360 active input을 기록했다. 최신 launch에는 해당 크기 강제가 없다. 이 차이는 설정 비교상의 혼입 요인이지만 최신 backbuffer/SR 입력·출력 크기를 입증하지 않는다. 저장 크기끼리의 픽셀 면적 비율 9는 실제 처리량이나 FPS 비율이 아니다.”
근거: `LIVE/wineprefix/user.reg:1008-1020`, `CAND/game.log:3105-3118`, `R/wave-3-verification.md:6`. 최신 watermark 크기도 실제 SR 크기의 대체 증거로 쓰지 않는다.

### 6. source-cut 및 executable sections — supported, 회귀는 unresolved
“비교한 두 source-cut 기록은 각각 568항목이며 항목 차이가 없다. 비교한 aarch64-windows PE 실행 섹션은 nvngx.dll, dxgi.dll, d3d12core.dll, d3d11.dll에서 같고, d3d12.dll과 xtajit64.dll에서 다르다. 이는 기록된 소스 항목 및 해당 실행 섹션 비교 결과다. 모든 빌드 입력·비실행 데이터·설정·로드된 코드의 동일성을 뜻하지 않으며, 다른 실행 바이트만으로 동작 회귀를 증명하지 않는다.”
근거: `R/verify-build-inputs.md:7-12,41,70,99,128,157`; 비교 범위는 `R/verify_build_inputs.py:7-34`. core generation 차이만으로 d3d12/FEX의 모든 코드 차이를 설명하거나, 같은 실행 섹션을 근거로 해당 모듈의 모든 동작 차이를 배제하지 않는다.

### 7. 공통 startup 오류와 tracking — 단독 원인 주장은 rejected
“최신 및 과거 장면 렌더링 증거가 있는 candidate 모두 `disableCLStateTracking=1`, HWS/DLSSG 관련 startup 메시지와 FeatureFailedToLoad를 기록한다. 따라서 이 값·문자열만으로 최신 장면 정지의 고유하거나 충분한 원인을 진단할 수 없다. 과거 공존은 모든 상황에서 무해하다는 증명은 아니며, 다른 조건과 결합한 영향은 미확정이다.”
근거: 최신 및 `CAND/Player.log:8,16-19,51-52`; `R/wave-3-verification.md:8`. 추가 주의: 최신 `Player.log:445`의 FeatureMissing은 candidate-03과 비교하면 차이지만, 다른 과거 `build/local/zzz-dlss-fix-20261009/dlss-run-02/Player.log:20459`에도 있다. 이를 “최신 실행에만 발생한 오류”라고 쓰면 안 된다. 바로 뒤 FFX context 기록(`:446-447`)도 feature ID·시각·active upscaler를 제공하지 않아 자동 fallback이나 OOM의 증거가 아니다.

### 8. FPS 및 최적화 우선순위 — 현재 원인은 unresolved
“검토한 자료에는 최신 DXMT/FEX와 D3DMetal/Rosetta를 같은 장면·설정·실제 해상도·cold/warm 상태에서 비교한 presented-frame/CPU/GPU 시계열이 없다. 오류 개수, cache 파일 mtime, 과거 단일 HUD 샘플이나 FEX CPU 비중으로 최신 FPS 손실률을 계산할 수 없다. 과거 비용 측정과 정적 경로는 조사·최적화 후보의 근거일 뿐 최신 병목이나 예상 FPS 개선의 증명이 아니다.”
근거: `R/verify-shader-corrections.md:4-10`, `R/verify-identity-timings.md:17-27`, `wave-1-skeptic.md` §7. 특히 과거 creation143의 compile은 150.2201 ms, native pipeline은 1704.065 ms이며 최신 실행 측정이 아니다.

## 남은 데이터 공백
- **정지 단계:** 알려진 입력/카메라 변화와 동기화한 scene-color, temporal output, presented scene의 내용 변화; resource·feature·evaluation·queue·submission 식별자, Evaluate 반환값과 encode/completion 결과. 같은 resource pointer만으로 내용 freshness를 판단할 수 없다.
- **컴파일 오류의 역할:** 최신 실패 shader/root 쌍, compilation variant와 caller stack, warmup/실제 draw 구분, 해당 장면 frame과의 시각 상관관계.
- **실행 identity/workload:** 실행 당시 loaded module 경로·해시, active upscaler와 설정 전환 시각, 실제 SR/backbuffer 크기, reset 이력 및 관련 present 영역.
- **성능:** 동일 조건의 DXMT/FEX 및 D3DMetal/Rosetta frame-time·GPU·CPU·wait 측정. 기존 FEX 샘플만으로 frame-critical 지연이나 최신 FPS 차이를 배분할 수 없다.

이 공백은 원인 확정을 막지만, 기존 로그와 정적 분석만 수행하는 이번 연구를 막지는 않는다. 위 문구를 적용하면 모든 제시 주장이 supported/narrowed/unresolved 중 하나로 정리되며, 근거 없는 확정 원인은 남지 않는다.