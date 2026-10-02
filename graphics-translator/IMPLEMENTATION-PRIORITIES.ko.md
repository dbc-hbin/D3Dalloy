# 남은 구현 우선순위

기준일: 2026-10-02. 현재 루트 `src/`의 미구현·부분 구현을 기능 단위로 정리한다. **작업 순서는 이 문서**, 지원 선언·완료 증거는 [현재 구현 상태](CORE-IMPLEMENTATION-STATUS.ko.md), 실행 절차는 [smoke guide](SMOKE-TEST.ko.md)가 기준이다. 이 P0–P4는 과거 완료한 동명 개발 단계와 다른 새 backlog다.

원칙은 **기존 API 안전성·정확성 → 공개 계약 완결 → 범위가 좁은 확장 → FL12_2 → 상위 Shader Model → 추가 통합**이다. 연구가 필요한 atomic64나 ARM64/FEX를 독립 작업의 선행 조건으로 만들지 않는다. D3D11 기능 확장은 하지 않으며 D3D12에 필수인 공유 경로만 변경한다. 미지원 cap, 유효한 optional absence, 오래된 TODO와 실제 결함을 구분한다. Preview는 정식 SDK 목표와 분리하고 각 확장 착수 시 선택한 header/SDK revision을 확인한다.

## P0 — 기존 API 경로의 안전성·계약 완결

아래 네 항목은 구현과 공개 consumer 실행이 모두 끝나야 닫는다. 문서의 계획이나 컴파일 성공만으로 완료 표시하지 않는다.

| ID | 작업 | 현재 근거 | 완료 기준 |
|---|---|---|---|
| P0-1 | DXGI 보호 콘텐츠 알림 API의 안전한 미지원 처리 | `src/dxgi/dxgi_adapter.cpp`의 Register/UnregisterHardwareContentProtectionTeardownStatus가 assert-only. Assert 활성 시 중단, 비활성 시 HRESULT 반환 누락의 정적 위험 | Native 보호 기능을 가장하지 않는다. 인자·출력·일관된 미지원 결과, unregister의 안전성, caller event 수명/상태 보존을 공개 API에서 확인한다. 전체 protected session 구현은 P4다. |
| P0-2 | 정확한 adapter LUID 선택 | `src/dxgi/dxgi_factory.cpp::EnumAdapterByLuid`가 불일치 LUID에도 adapter0을 반환. 실제 CPU API probe에서 없는 LUID에 S_OK/non-null/wrong adapter 관찰 | 실제 일치 LUID와 IID는 올바른 COM 객체 반환, 없는 LUID는 DXGI_ERROR_NOT_FOUND와 null output, 잘못된 출력/IID 및 객체 수명 경계를 확인한다. GPU preference의 단일 GPU 정책과 구분한다. |
| P0-3 | Resource1/Resource2/Heap1 공개 계약 | Resource·Heap 객체 QI가 base interface만 노출. 일부 생성·공유 경로는 확장 IID를 받지만 반환 객체와 불일치 | Resource1/Heap1의 GetProtectedResourceSession, Resource2의 GetDesc1을 실제 구현하고 모든 resource/heap 종류, 생성·QI·공유 handle·IUnknown identity·refcount 계약을 이행한다. WriteToSubresource는 이미 base Resource 메서드다. |
| P0-4 | Capability 및 NGX discovery 보고 정합 | 일부 알려진 CheckFeatureSupport 질의는 default E_NOTIMPL. NGX GetFeatureRequirements는 adapter의 temporal support를 조회하지 않아 Init과 범위가 다름 | 실제 선택 header의 빠진 질의를 명시적으로 대조하고 구현/명시적 미지원 상태를 정확히 보고한다. 구조 크기·입력·출력·node/queue/optional 기능 경계를 검증한다. NGX는 supported/unsupported adapter discovery와 Init의 공통 native support 기준을 사용한다. 기능 없는 cap을 올리지 않는다. |

P0-2 수정 전 실행: [`remaining-inventory-stYzia/adapter-luid.log`](../build/local/remaining-inventory-stYzia/adapter-luid.log), 최신 port runtime02 사용. `missing_luid_hr=00000000`, expected `887a0002`, output_nonnull=1, returned_matches_request=0. 이것은 CPU 공개 API 재현이며 GPU 실패나 실제 게임 장애 증거가 아니다. P0-1은 이 문서 작성 당시 정적 확인이지 crash 재현이 아니다.

### 병렬 작업 경계

- P0-1은 DXGI adapter, P0-2는 DXGI factory로 분리한다.
- P0-3은 resource/heap class·QI·생성/공유 반환을 소유한다.
- P0-4는 feature-query 계약과 NGX discovery를 소유한다. 공통 `d3d12_device.cpp`의 적용은 단일 편집 소유자가 통합한다.
- x64/WoW64 core API와 x64-only NGX facade를 구분한다. 현재 staged runtime으로 변경 전 증거를 얻고, 변경 후에는 새 runtime을 stage한다. Caller event·COM 수명과 실제 texture/buffer readback을 포함한다.

## P1 — 기존 구조를 재사용하는 제한된 확장

| 순서 | 작업 | 남은 계약 |
|---|---|---|
| 5 | Programmable sample positions | 현재 SetSamplePositions 미지원. 가능한 tier부터 native sample pattern, rasterization, GetSamplePosition, MSAA/resolve를 함께 연결한다. 기존 center-pattern 지원과 구분한다. |
| 6 | Non-normalized coordinate samplers | Sampler 생성, 좌표·LOD lowering, descriptor metadata와 emulation 경로의 정합. |
| 7 | NGX Exposure.Scale | 현재 1만 허용. 공개 의미 확정 후 GPU exposure 전처리와 closed-list replay에 반영한다. |
| 8 | NGX MV.Offset.X/Y | 현재 0만 허용. 이미 구현된 입력 subrect offset과 다른 motion-vector 값 bias이며 scale/jitter 적용 순서를 정해야 한다. |
| 9 | FG ResizeBuffers1 per-buffer queues | 일반 resize는 완료. Queue 지정, ownership, 기존 frame drain과 비동기 동기화를 추가한다. |
| 10 | SR/FG GPU memory usage query | SR은 미지원, native FG context는 zero/unreported success. 실제 계상 가능 범위와 native 내부 미관측 범위를 구분해 보고 계약을 완성한다. |
| 11 | GetSharedResourceAdapterLuid | 기존 공유 handle metadata와 adapter identity를 연결한다. |

## P2 — FL12_2의 남은 핵심 계약

| 순서 | 작업 | 현재 경계와 필요한 구현 |
|---|---|---|
| 12 | Depth bounds | cap FALSE, 활성 PSO 거부. Dynamic min/max, depth/stencil write, early-Z, UAV side effect와 occlusion/statistics를 보존해야 한다. 단순 fragment discard는 충분하지 않다. |
| 13 | Conservative Rasterization Tier3 | 현재 Tier1. Degenerate geometry, full coverage, SV_InnerCoverage와 compiler/raster 실행 계약을 추가한다. |
| 14 | Tiled Resources Tier3 | 현재 Tier2. D3D volume tile과 native tile shape 차이를 logical page-table/filtering/LOD/residency/remap/alias/queue order로 해결해야 한다. |

COPY timestamp와 OPTIONS8 unaligned BC는 완료했다. Programmable sample positions는 위 세 가지와 별도의 선택 기능이며 FL12_2의 추가 필수 조건으로 잘못 넣지 않는다. 구현 후 전체 필수 cap을 재대조한 다음 feature level을 변경한다. SM6.6 미보고 자체는 FL12_2의 blocker가 아니다.

## P3 — 상위 shader·GPU 기능

현재 공개 상한은 **SM6.5/FL12_1**, compiler 입력 상한은 **DXIL/SM6.8**이다. 6.6–6.8의 개별 shader는 operation/stage 지원 범위 안에서 수용한다. 일부 opcode lowering만으로 공개 model을 올리지 않는다.

| 순서 | 작업 | 남은 계약 |
|---|---|---|
| 15 | 64-bit atomic 전체 계약 | Compute raw/structured emulation 외 graphics-stage returning atomic, typed resource, groupshared 및 capability 완결. 상호 atomicity와 forward progress feasibility가 선행한다. |
| 16 | RelaxedFormatCasting / AdvancedTextureOps | 일부 sample/compare/gather lowering은 존재. Resource/view casting 계약과 해당 cap의 전체 조건을 닫는다. Programmable offsets는 재구현하지 않는다. |
| 17 | Mesh/AS derivatives | Quad 배치·derivative·implicit sample/LOD lowering. 기존 MeshShaderTier1의 필수 출력/cull 구현과 별개다. |
| 18 | SM6.9/DXIL1.9 vector 기능 | Long vector, elementwise intrinsic, vector load/bounds/status, reduction, AIR legalization과 입력 version gate. |
| 19 | SER / HitObject / opacity micromap | Compiler, AS 표현, traversal/continuation ABI, pipeline 및 실제 oracle. 기존 DXR1.1과 구분한다. |
| 20 | WaveMMA / matrix 연산 | Native 대응 또는 정확한 emulation의 타당성 및 compiler 연결. |
| 21 | Writable MSAA | Sample별 write와 이후 load/resolve 가시성. 현재 cap FALSE와 store 거부. |
| 22 | FP64 | 현재 double IR/연산 거부. Native 대응 부재 시 software emulation의 정확성과 비용을 검토한다. |
| 23 | Wave32 외 wave size | 별도 실행·동기화 모델. 현재 정직한 wave32 제한 자체는 결함이 아니다. |
| 24 | Tiled Tier4 | Tier3와 별도로 packed-array 등의 추가 계약. |

연구 의존성이 큰 atomic64, MMA, writable MSAA, FP64, wave-size 확장을 다른 준비된 항목의 선행 조건으로 삼지 않는다.

## P4 — 추가 공개 API·통합

### Rendering / D3D12

- **OPTIONS13 선택 계약 6종:** texel 단위 buffer/texture copy pitch와 offset, 완화된 vertex-element alignment, inverted viewport Y/Z, 차원 간 texture copy, constant-alpha blend factor. 기본 copy·viewport·alpha blending이 없다는 뜻이 아니다.
- **Triangle fan·manual write tracking** 등 현재 미지원으로 보고하는 선택 계약.
- **Device5 LifetimeTracker:** 실제 owner/object association, state callback, 재진입과 destruction 순서. Shell-only 구현을 완료로 보지 않으며 자동 다음 순위로 지정하지 않는다.
- **최신 queue priority 인터페이스:** ID3D12CommandQueue1의 process/global priority. Swapchain 생성은 owned private factory 경로이며 이 인터페이스의 누락 메서드가 아니다. 정식/preview SDK revision과 native 대응을 먼저 확정한다.
- **Debug / InfoQueue / DRED / SDK·Tools 인터페이스**, queue marker의 native 진단 연결. 기존 Metal validation과 별개다.
- **Protected resources/sessions:** 실제 보호 메모리·접근·출력 보장이 필요하다. P0-1의 안전한 미지원 반환과 다른 작업이다.
- **Video decode/process/encode:** 현재 별도 D3D12 video backend와 resource/queue 연결이 없다.

### DXGI / 표시

- CoreWindow/Composition swapchain과 관련 matrix transform.
- Desktop duplication, display-surface readback, 일부 output statistics.
- Occlusion/stereo 알림, Factory7 adapter-change 알림.
- 다중 GPU preference 선택과 cross-node 계약.
- HDR10PLUS, stereo/overlay/hardware composition의 native 대응.
- WARP/software adapter는 현재 제공하지 않는다. 별도 renderer 없이 이름만 추가하지 않는다.

### SR / FG / NGX / NVAPI

- Alpha upscaling: native output alpha 보존과 다른 미지원 기능.
- SR debug-view dispatch, FG tear/reset/debug-view 등의 미지원 시각화 flag.
- FG 다중 generated outputs: 현재 한 개.
- NGX Frame Generation/Ray Reconstruction 등 SuperSampling 이외 facade. **FSR 공개 FG 완료는 NGX/DLSSG 완료가 아니다.**
- 실제 Reflex 대기·스케줄링·latency report. 현재 호환용 no-op/빈 보고이며 FSR FG pacing과 다르다.
- Public facade x86/ARM64/ARM64EC: 현재 x64-only이며 private temporal WoW64와 구분한다.

NVAPI의 NVIDIA 전용 clock/DRS/Mosaic/SLI/CUDA/Cubin 등 광범위 기능은 대부분 미지원이지만 범용 D3D12 번역기의 필수 scope로 자동 편입하지 않는다. Meta command가 0개인 것은 허용된 optional absence이며 임의의 vendor command를 만들어 지원으로 표시하지 않는다.

## 별도 보류 트랙 — ARM64/FEX 실행

이전 사용자 보류 결정을 이 목록으로 해제하지 않는다. [ARM64/FEX gate](../docs/ARM64-FEX-IMPLEMENTATION.md)를 따른다.

1. Darwin low-address 호환 계층: KUSER_SHARED_DATA `0x7ffe0000` mapping 실패로 PE 진입 전 막힌다.
2. Plain ARM64 guest→COM→Unix-call의 실제 값·callback·ownership acceptance.
3. 검증 가능한 Darwin FEX source revision/license/Wine integration patch/build·run recipe.
4. x64 및 x86 guest→ARM64 translator/compiler/Metal end-to-end를 각각 검증한 뒤 명시한 native matrix로 확대.

Native compiler/Unix plugin, production PE, ARM64X paired build, native Wine package는 이미 완료된 기반이다. Host 실행을 guest 성공으로 바꾸지 않는다.

## 성능 및 검증-only 트랙

- **구체적 추가 구현:** native Metal pipeline/archive 영속 캐시. 이미 구현한 owned PSO serialization/PipelineLibrary/ShaderCacheSession과 별개다.
- **측정 후 선택:** sparse mapping, queue overlap, retirement, allocation/copy, memory pressure, cold/warm 생성 비용, stutter·전력. 미측정 성능 개선을 약속하지 않는다.
- **검증 확대:** 다른 GPU/OS, Windows differential, 실제 Streamline/game loader, 물리 HDR/scanout, 장시간·성능. SampleCmp 확장/GatherRaw/u64 gather, 비-CS denorm, library GS/HS/DS, mixed-bitness sharing은 구현과 proof 부재를 구분한다.
- **NGX 색상 계약 검토:** IsHDR는 HDR display/PQ selector가 아니다. LDR/perceptual 입력과 MetalFX 계약을 독립 oracle로 대조할 후보이며 unused bit만 보고 임의 gamma 변환이나 HDR 거부를 추가하지 않는다. FG의 dispatch transfer/async queue는 이미 연결돼 있다.
- **Known native 경계:** Metal4FX 16→48의 Conv3x3 validation 오류는 독립 native에서도 재현됐다. 누락된 port로 재분류하거나 validation을 숨기지 않는다.

## 완료·오분류 제외

Full SR/FG port의 MV/masks/subrect/transfer/sharpening/dynamic output/history/UI/proxy/pacing, ShaderCacheSession/Control, PipelineLibrary/CachedPSO, COPY timestamp, programmable offsets, unaligned BC, compute CalculateLOD, budget notification, root/indirect arena, 기존 DXR1.1/Mesh1/VRS2/feedback1.0/workgraphs1.0은 남은 구현에 다시 넣지 않는다.

현재 D3D12 1D comparison sampling은 logical 1D→depth2D 변환으로 구현돼 있다. 1D gather는 요구할 유효한 HLSL operation이 아니다. RenderPassTier0는 BeginRenderPass 부재가 아니며 culled MS primitive 통계 flag FALSE는 mesh 통계 부재가 아니다. 기본 view의 PlaneSlice=0 및 오래된 DXBC TODO만으로 일반 입력 결함을 확정하지 않는다. D3D12 device가 IDXGIDevice를 노출하지 않는 것 자체도 불일치가 아니다.
