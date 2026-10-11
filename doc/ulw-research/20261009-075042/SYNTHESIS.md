# DLSS 장면 정지와 DXMT/FEX 성능 조사

상태: 기존 로그·정적 분석 조사 완료. 정확한 정지 단계와 최신 FPS 원인 비중은 미확정.

조사 대상: 2026-10-09 16:29:54 KST에 시작한 ZZZ 실행. 최신 로그·현재 소스·보존된 프로파일과 pristine GPTK 4.0b2 바이너리를 읽었으며, 게임이나 런타임을 변경하지 않았다. 출처 번호는 [sources-ledger.md](sources-ledger.md)에 대응한다. [S4] [S17]

## 결론

**HUD는 움직이는데 3D 장면이 멈춘 증상은 게임 전체 정지보다 장면 렌더링·DLSS 입력/출력·최종 합성 중 한 단계가 이전 프레임을 계속 쓰는 경우에 더 잘 맞는다. 그러나 이번 로그만으로 그 단계를 확정할 수는 없다.** 최신 실행에는 프레임별 NGX 호출, MetalFX 입출력, GPU 완료 기록이 없다. 과거 DLSS 성공 기록을 이번 실행의 성공 증거로 대체해서는 안 된다.

이번 실행의 가장 뚜렷한 이상은 **셰이더 컴파일 실패 9,131줄과 POSITION_0 입력 레이아웃 경고 1,881줄**이다. 활성 장면의 PSO 생성 실패가 DLSS에 오래된 입력을 줄 가능성을 먼저 확인해야 한다. 다만 오류에는 실패한 셰이더/루트 서명 식별자와 프레임 연결 정보가 없어, 이 오류가 화면 정지의 원인인지, 워밍업의 무효 조합인지 아직 구분되지 않는다. [S1] [S2] [S3] [S16]

성능 쪽에서는 실제 개선 대상을 찾았다. **과도한 render-pass 분할, 일반 descriptor table 전체 순회, MetalFX 앞뒤 command buffer 분리와 전체 단계 barrier, 캐시 적중 뒤에도 남는 native PSO 생성 비용**이다. FEX에도 추가 후보가 있지만, 보존된 프로파일로 현재 FPS 저하를 FEX 단독 원인이나 일정 비율의 손실로 계산할 수는 없다. [S13] [S14] [S15] [S16] [S17] [S18] [S19]

## 1. 이번 실행에서 확인한 사실

| 항목 | 확인 결과 | 해석 한계 |
|---|---|---|
| 최신 실행 | `game_1791530994266.log`, 시작 16:29:54.266 KST, 16:36경 종료 | 지금 실행 중인 게임의 실시간 관찰은 아님 |
| 선택 런타임 | `wine-11.17-arm64ec-fex-dxmt-optimized-20261009` | 선택 태그만으로 로드된 파일 해시까지 증명하지 않음 |
| 현재 파일 | d3d12/nvngx/winemetal 및 i386 d3d12/nvngx, 5개 실제 파일 SHA256이 배포 manifest와 일치 | 실패 시점 loaded-image 해시가 보존된 것은 아님 |
| 셰이더 실패 | stdout 9,131줄; D3D12 별도 sink 9,122줄 | 중복 로그이므로 합산 금지. 고유 셰이더 수·실패 프레임 수가 아님 |
| 입력 레이아웃 | 각 sink에 POSITION_0 경고 1,881줄 | 실제 장면에 사용된 PSO인지 미확인 |
| DLSS 상세 기록 | NGX evaluate/MetalFX encode·완료 추적 없음, Streamline `diag=0` | 호출되지 않았다는 증거도, 성공했다는 증거도 아님 |
| Metal HUD | 실행 환경에서 켜짐 | `metalfx.timing already exist`는 메트릭 등록 메시지이지 최신 출력 증거가 아님 |
| 화면 설정 | 저장된 Unity 설정 3840×2160, native/fullscreen 선호 | 실제 swapchain·DLSS 출력 크기는 미기록 |
| 이전 비교 실행 | candidate-03은 1280×720 강제 실행, NGX 출력도 1280×720 확인 | 최신 실행 및 D3DMetal/Rosetta와 동일 조건의 FPS 비교가 아님 |

출처: [S1] [S2] [S3] [S4] [S5] [S6] [S7] [S8] [S9] [S21], 실행 검증: [verify-log-counts.md](verify-log-counts.md), [verify-identity-timings.md](verify-identity-timings.md).

실패 유형은 PS SRV 3,856줄, VS SRV 1,142줄, VS CBV 814줄, PS CBV 1,968줄, PS sampler 1,305줄, `vshs_main` SRV 46줄이다. 오류 자체에 시각이 없으므로 초당 오류 수나 컴파일이 차지한 시간을 로그 양으로 계산하지 않았다. [S1]

런처는 이번 실행 전에 prefix를 재생성했다. 현재 prefix에는 private shader cache 파일 11,116개, 총 237,591,400바이트(226.58 MiB)가 남아 있다. 따라서 `AUTOMATIC_DISK_CACHE=0` 메시지를 보고 “DXMT 셰이더 캐시가 꺼졌다”고 해석하면 틀린다. 그것은 별도의 OS 자동 캐시 지원 비트이고, owned 캐시는 driver-managed 방식이다. 다만 이번 캐시 적중률과 컴파일 총시간은 없다. [S4] [S16] [S20]

## 2. 화면 정지는 어디까지 좁혔나

```text
장면 draw / PSO
    -> DLSS color·depth·motion 입력
    -> NGX 파라미터 해석·temporal 명령 기록
    -> MetalFX 인코딩·GPU 실행
    -> 출력 복사·장면 합성·Present

HUD가 별도 경로로 갱신된다면, 위 장면 경로가 오래된 그림을
계속 전달해도 HUD 위치 변화와 정지한 3D 장면이 공존할 수 있다.
이번 로그에는 어느 화살표에서 내용 갱신이 끊겼는지 판별할 자료가 없다.
```

### 먼저 구분해야 할 세 가지

1. **DLSS 입력부터 오래된 경우.** DLSS를 켤 때 필요한 셰이더/PSO 조합이 거부되거나 장면 draw가 누락되면, MetalFX가 정상적으로 실행해도 같은 장면을 받는다. 이번 대량 컴파일 실패는 이 가설을 확인해야 할 직접적인 이유다. 그러나 실패한 PSO와 실제 장면 draw의 연결 증거가 아직 없다.
2. **입력은 새롭지만 SR 호출·출력 갱신이 끊긴 경우.** NGX 호출 실패, native MetalFX 실행 문제, 기록된 출력의 재사용 등이 포함된다. 현재 소스는 NGX에서 `TemporalUpscale` 실패를 성공으로 무조건 바꾸지 않는다. 반대로 `S_OK`는 명령 기록 성공일 뿐 GPU 완료나 새 픽셀을 보증하지도 않는다. [S11] [S12]
3. **SR 출력은 새롭지만 복사·합성에서 오래된 장면을 쓰는 경우.** native 후처리, caller output copy, 최종 합성/Present까지 구분해야 한다. 현재 자료에는 실제 리소스 내용과 Present 영역 기록이 없다. dirty-rect 보존이나 별도 queue 대기는 가능성이지 확인된 원인이 아니다. [S13] [S22]

HUD가 실제로 갱신된다는 사용자 관찰은 화면 전체를 막는 단일 queue 교착 설명을 약하게 만들지만, 별도 queue 또는 장면 일부의 정지를 배제하지는 않는다. 엔진의 frame counter도 계속 증가했으나, 이는 실제 present/FPS나 3D 장면 갱신의 증거는 아니다. [S6]

### 지금 원인으로 지목하면 안 되는 메시지

- `eErrorOSDisabledHWS`, `eErrorFeatureFailedToLoad`, `DLSSG is not requested or failed to load`는 이전에 도시가 보인 실행에도 있다. 특히 DLSSG는 frame generation이며 DLSS SR 실패와 같은 뜻이 아니다.
- `disableCLStateTracking=1`, LUT 해제·moon_bright mutation 경고도 이전 실행과 공통이다. 단독으로 이번 정지를 설명하지 못한다.
- 과거 candidate-03의 11개 NGX 기록 성공과 도시 캡처는 과거의 제한된 성공 증거다. 두 캡처에는 NPC/idle pose 변화도 있어 그때부터 장면이 영구 정지했다는 해석은 맞지 않지만, 이번 실행의 지속적인 SR 성공도 증명하지 않는다. [S9] [S10]
- 이전에 보존한 실패 shader/root pair 64개는 pristine D3DMetal에서도 거부됐다. 이번 9,131줄이 그 64개와 같다는 식별 증거가 없으므로, “전부 정상적인 워밍업 실패”나 “전부 DXMT 검증 버그”라고 일반화할 수 없다. [S3] [S9] [S10] [S16] [S17]

## 3. D3DMetal과 비교해 실제로 다른 부분

참조는 GPTK 4.0b2의 pristine x86_64 D3DMetal이며 SHA256과 코드 서명을 확인했다. `ReplayTemporalScale` 전체 함수 `0x36656..0x36922`를 직접 역어셈블해 다음 차이를 확인했다. 바이너리를 제품 코드에 포함하거나 코드를 복제하지 않았다. [S17]

| SR 경계 | D3DMetal Metal4 경로 | 현재 owned DXMT |
|---|---|---|
| scaler fence | scaler에 fence 지정 | scaler에는 nil fence |
| 앞뒤 순서 보장 | dispatch 단계 fence update/wait | 전체 단계 queue barrier |
| 보이는 command buffer 구조 | 같은 command buffer에서 encoder 종료 → scaler encode → 새 encoder | prefix → 별도 scaler buffer → suffix로 분할; alpha 경로는 추가 분할 |
| 후처리 | 별도 legacy 경로에는 offset 보정도 존재 | 입력 준비·padding·색 변환/RCAS·caller output copy가 조건에 따라 추가 |

참조의 주요 호출은 `setFence:` 0x36870, `updateFence:afterEncoderStages:` 0x3688d, `encodeToCommandBuffer:` 0x368c3, `waitForFence:beforeEncoderStages:` 0x36922다. fence stage mask는 `0x08000000`, dispatch 단계다. [verify-reference.md](verify-reference.md)

**이 차이는 SR 경로 최적화의 구체적인 출발점이다.** 하지만 현재 DXMT의 분리는 같은 buffer 안에서 external fence consumer가 교착했던 문제를 피하려고 넣은 것이다. GPTK와 같은 모양으로 합치는 것만으로 올바른 동기화가 되는 것은 아니다. allocator·residency·completion 순서를 포함해 교착이 없는 경우를 입증한 뒤 줄여야 한다. MetalFX 내부 command buffer와 최신 GPU 비용은 이 정적 분석에서 보이지 않는다. [S13] [S17]

## 4. 개선 우선순위

순위는 예상 FPS 증가량이 아니라, 발견한 코드 비용과 검증·수정 위험을 기준으로 한다. 이번 실행에는 CPU/GPU frame-time 시계열이나 조건을 맞춘 Rosetta 대조가 없어 개선율을 제시하지 않는다.

| 우선순위 | 개선 대상과 근거 | 최소 개선 방향 | 효과 판정 |
|---|---|---|---|
| 선행 | 장면 정지와 비교 조건 | 실패한 shader/root/PSO를 실제 draw 및 DLSS 프레임과 연결. 실제 출력 해상도·DLSS 모드·장면·캐시 상태를 맞춤 | 3D 장면이 계속 갱신되고 비교 프레임의 작업량이 같음 |
| 1: CPU/encoder | `ResourceBarrier`가 유효한 비어 있지 않은 barrier 호출에서 현재 pass를 무효화. 소스에도 과도한 동기화라고 명시 | 안전성이 입증되는 read-only 전이부터 불필요한 pass 종료를 줄임. UAV·alias·write hazard와 untracked resource 동기화 보존 | 같은 장면에서 encoder/pass 수와 CPU frame time 감소, Metal validation 오류 없음 |
| 1: CPU | 일반 descriptor table의 전체 SRV/UAV 범위 순회와 resource owner 조회 mutex. dirty flag 검사 밖에서도 캡처 수행 | 실제 shader 사용 범위, 겹치는 범위의 중복 캡처, 같은 pass의 유효한 재사용부터 축소 | 캡처한 descriptor/lookup 수와 CPU 비용 감소, 수명·descriptor 변경 동작 유지 |
| 2: SR/GPU, 높은 수정 위험 | 위 D3DMetal 대비 buffer 분할·전체 단계 barrier·조건부 복사 | 실제 활성화된 padding/변환/copy를 먼저 계측해 불필요한 작업부터 제거. fence 범위 축소·buffer 통합은 교착 재현을 통과한 뒤 | SR GPU 시간·복사 바이트·buffer 수 감소, 입력/출력 갱신 및 completion 정상 |
| 별도: 로딩/스터터 | compiler cache hit 뒤에도 native library/function/PSO 생성이 남음 | 이미 만든 native PSO 재사용, 정확한 warmup, 가능한 비동기 준비부터. native archive는 지원 범위·키 검증 후 검토 | cold/warm 생성 지연과 frame-time spike 감소. 평균 FPS와 별도 집계 |
| 후속: FEX | 과거 profile의 ExitEC·JIT code generation·label binding 비용 | 실제 hot boundary 왕복 수 감소, 남은 Bind 처리 비용 검토. FPCR·AH/NEP·suspend 의미는 유지 | 현재 build의 CPU sampling 및 동일 workload 대조, 생성 코드 correctness 검사 |

코드 위치:
- barrier: `src/d3d12/d3d12_command_list.cpp:5870-5914`.
- descriptor: `src/d3d12/d3d12_command_list.cpp:1761-1805`, `src/d3d12/d3d12_command_queue.cpp:3441-3502`, `src/d3d12/d3d12_device.cpp:2983-2989`.
- SR: `src/winemetal/unix/metalfx-backend.mm:2094-2188`, `src/d3d12/d3d12_command_queue.cpp:6434-6481`.
- native PSO: `src/d3d12/d3d12_pipeline_compute.cpp:127-166`.

descriptor 사용 범위를 줄일 때는 같은 pass 안에서 바뀌는 **모든 PSO의 사용 범위와 dynamic/unbounded indexing**을 보존해야 한다. 현재 캡처 중복 판정에는 PSO가 포함되지 않으므로 `restrict_to_shader`만 켜면 뒤에 사용된 PSO의 자원을 누락할 수 있다. mutex를 제거하거나 수명 검증 없는 raw-pointer cache를 넣는 방식도 권고하지 않는다. 새 교차검토에서 이 조건을 확인했다.

이미 반영된 **shader cache 전체 재검색 제거, bindless heap 중복 처리 축소, FEX IR block-tail scan 수정**을 새 개선안으로 다시 제안하지 않았다. 실패한 binding 검증을 느슨하게 하거나 모든 barrier를 없애는 방안도 제외했다.

### PSO와 FEX 숫자는 이렇게 해석해야 한다

과거 `observe-run`의 compute creation 143은 compiler 구간 **150.2201 ms**, native pipeline 구간 **1,704.065 ms**였다. native PSO가 큰 로딩/스터터 비용이 될 수 있다는 실측 증거지만, 이번 실행의 steady-state FPS 비용은 아니다. 다른 선택 사례도 native 16.1~148.5 ms였으며, 선택된 사례를 전체 분포로 일반화하지 않았다. [S15]

보존된 city profile을 재계산하면 native FEX 함수에 귀속된 running CPU sample은 전체 합계의 약 **14~18%**였다. 이 자료는 IR-tail 수정 이전이며, 일부는 네트워크 thread다. 번역된 게임 코드의 비효율까지 이 분류에 모두 잡히는 것도 아니므로 **“FEX가 FPS를 14~18% 깎는다” 또는 “그 이상 영향은 없다”는 뜻이 아니다.** 이전 profile의 OFF/ON은 FEX disk cache 조건이지 DLSS OFF/ON 대조도 아니다. [S18] [S19]

현재 배포 소스에는 IR-tail 수정이 들어 있다. candidate와 최신 package의 기록된 DXMT 입력 568개는 같지만 전체 바이너리는 다르다. 별도 확인에서 nvngx/dxgi/d3d12core/d3d11의 실행 section은 같았고, d3d12/FEX는 달랐다. 이는 “옛 빌드를 그대로 돌렸다”는 결론도 “동작이 완전히 같다”는 결론도 허용하지 않는다. [S8] [verify-build-inputs.md](verify-build-inputs.md)

## 5. 원인을 확정하는 데 부족한 관측

다음 진단은 로그를 많이 찍는 것이 아니라 같은 frame ID에 아래 네 경계를 연결해야 한다.

1. 장면 color 입력의 내용 변화와 producer PSO/draw 성공 여부.
2. NGX evaluate 입력·반환, render/output 크기, reset, resource/subresource 식별자.
3. MetalFX 이후 내부 출력과 caller output의 내용 변화, native command completion/error.
4. 최종 scene 합성·present 결과. HUD 변화와 3D 변화는 따로 판정.

입력부터 같으면 producer/PSO를, 입력은 바뀌는데 SR 출력이 같으면 SR 호출·실행·history를, SR 출력은 바뀌는데 화면이 같으면 copy/합성/present를 조사하면 된다. 포인터가 같다는 것만으로 내용이 같다고 판정하면 안 된다.

기존 `OWNED_GRAPHICS_TRACE=1`, `-streamline-d3d12-diag`, module-load audit는 호출·로드 관측에 쓸 수 있지만, **그것만으로 픽셀 내용의 갱신이나 GPU 완료까지 모두 보이지 않는다.** 과거 trace limit 64는 초기 pipeline 단계에서 소진되기도 했다. 또한 `winemetal_metal4_fx.mm:35-45`는 native 상세 `error.message`를 버리고 HRESULT만 전달하므로 이 메시지를 보존하는 진단이 유용하다. [S9] [S11] [S12] [S13] [S16]

이번 요청 범위에서는 새로운 게임 실행이나 코드를 변경하지 않았다. 따라서 화면 정지의 정확한 원인, 최신 CPU/GPU 병목 비중, Rosetta 대비 FPS 차이는 **미확정**이다. 대신 최신 실패를 과거 성공으로 덮지 않고, 다음 수정이 겨냥해야 할 경계와 D3DMetal 대비 구체적인 구조 차이를 확보했다.

## 조사 방법과 검증

- 최신 로그 계수·해시, 실제 배포 모듈 해시, cache census, 과거 timing 단위 변환을 읽기 전용 스크립트로 실행했다.
- 보존된 FEX attribution 계산과 기존 native emitter regression/sanitizer 확인을 실행했다. 이번에 만든 기능의 성능 테스트가 아니다.
- pristine D3DMetal의 해시·서명을 확인하고 관련 함수 전체를 직접 역어셈블했다.
- 분야별 조사, 확장 조사, 새로운 원인/성능 교차검토를 분리했다. 잘못된 오류율·시간 환산·캐시 해석은 [verify-shader-corrections.md](verify-shader-corrections.md)에 배제 사유를 남겼다.
- 게임·Wine prefix·제품 소스는 변경하지 않았고, 새 게임/GPU 재현은 수행하지 않았다. 수정을 검증했다거나 FPS가 개선됐다고 주장하지 않는다.

## 출처

출처별 경로·줄 범위와 증거 한계는 [sources-ledger.md](sources-ledger.md)에 정리했다. 원문 수치 검증은 [로그 계수](verify-log-counts.md), [파일·timing 검증](verify-identity-timings.md), [바이너리 비교](verify-reference.md), [최종 주장 목록](claim-graph.md)을 참조한다.
