# 구현·검증 이력 (2026-09-22–2026-10-06)

이 문서는 기존 CORE의 **상세 계약·실패·수정·실행 관찰 ledger를 이동한 역사 기록**이다. 현재 지원의 기준은 [CORE 구현 상태](CORE-IMPLEMENTATION-STATUS.ko.md), 미완료 작업·실행 순서는 [구현 우선순위](IMPLEMENTATION-PRIORITIES.ko.md), build/staging 안내는 [루트 README](../README.md)다. 아래의 “현재”, “완료”, “보존”은 각 절을 작성한 당시의 범위다. 뒤의 관찰이나 현재 root source와 충돌하면 당시 기록을 새 현재 판정으로 사용하지 않는다. **아래 완료한 P0–P3 개발 단계와 새 backlog P0–P4는 별개**이며 새 P3 착수가 옛 증거의 취소·재실행을 뜻하지 않는다.

## Native ARM64/FEX 실제 실행 재개 — 2026-10-06

사용자 지시로 ARM64/FEX 실행 보류를 해제했다. Official Wine MR11638 commit `de56343b358617d8319b8510356169ad96555e17`와 FEX-2609.1 commit `9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf`를 pristine 입력으로 두고 root-owned patches와 fresh build/staging을 사용했다. Rosetta·4-GiB PAGEZERO 축소·private security API·live prefix 설치는 사용하지 않았다.

- [Native ARM64](../build/local/arm64x-mr11638-jit03-native-gpu-20261006/result.json)와 [FEX x64](../build/local/arm64x-mr11638-fex-jit03-abi16k-gpu-20261006/result.json)는 실제 guest main, COM/integer/float/pointer/32-byte packet, reverse callback1회, Unix-call111의 macOS27.0.1, native Metal64 bytes=`0xa7`와 fence=`0x123456789`를 통과했다. Exit0, validation errors0, loaded PE/native SHA256 일치와 observed ARM64 processes의 `proc_translated=0`을 확인했다. Native callee만16KB-align했으며 x64 guest는4KB를 유지했다. 이 blit 결과를 shader/game acceptance로 세지 않는다.
- [Emulated-image Wine02](../build/local/arm64x-mr11638-emulated-image02-runtime-20261006/runtime-result.json)는 ordinary non-CHPE x86 image에만 native host EXEC를 제거해16KB host page에서4KB code/IAT 갱신을 허용했다. Windows EXEC와 native/CHPE code 및 MAP_JIT의 의미는 유지했다. Context-rich patch zero-fuzz와 full configure/build/install/closure audit를 통과했다. 첫 patch 적용 실패와 이전 before-main IAT AV 증거는 보존했다.
- [FEX JIT04](../build/local/fex-windows-darwin-jit04-20261006/result.json)에서 unaligned-access recovery의 코드 backpatch 및 JIT tail spinlock에도 기존 nested `JitWriteScope`를 적용했다. 실제 `LDAR X8,[X7]`의 비정렬 VersionPair load가 recovery의 `STLXR` 쓰기에서 fault 난 JIT03 실행과 symbol/disassembly를 먼저 확인했다. [변경하지 않은4KB/fixed-address x64 probe](../build/local/arm64x-mr11638-fex-jit04-shared-data-20261006/result.json)는 scalar/SSE/clock/read-only guest exception 전체 PASS·exit0·no Rosetta·loaded hashes 일치이며 disposable prefix를 종료·정리했다. JIT patch SHA256은 `7d28d09e635b9890264832c329f61fc13d01c3db894febce2c9838828dca9c0b`, xtajit64.dll은 `362db11c41e7591c411f3a5c18fe915215ec4ecf57458a10c67bdb0b27aa30e1`이다.
- [초기 full D3D12 실패](../build/local/arm64x-mr11638-fex-jit03-d3d12-smoke-20261006/result.json)는8frame 전에 DLL 초기화에서 멈췄다. DXGI `__dyn_tls_init+0xc`의 실제 ARM64 명령은 `ldr x8,[x8,#0x900]`지만 동일 바이트의 x64 해석은 `orb %al,0x109f944(%rcx)`이며 로그의 잘못된 write address와 일치한다. CRT refptr 손상으로 단정하거나 개별 포인터를 덮지 않았다. LLVM22.1.8 `lld/COFF/Writer.cpp`의 TLS directory 생성과 `createDynamicRelocs`에 TLS view fixup이 없고, Wine은 첫 ARM64X DVRT record만 적용하는 것을 확인했다.
- [TLS01 paired build](../build/local/arm64x-mr11638-owned-tls01-20261006/result.json)는 원래 ARM64X DVRT record에 EC TLS directory VALUE fixups를 추가했다. 기존 `.reloc`의 zero padding을 검증해 사용하고 base relocations·entries·exports·native TLS view를 보존한다. 두 view의 실제 callback code architecture를 audit하며 real CRT fixture의 native-EC mismatch/corrupted TLS fixup rejection을0.29s에 통과했다. 두 ordinary Meson graphs와 six paired DLL의 fresh build는100.46s PASS다. Fresh package와 실제 graphics/compute shader 실행은 이 기록 시점에 pending이고 native x86 guest-window/Metal gate도 닫히지 않았다.

- [COM owner/scalar 수정 후 native ARM64](../build/local/arm64x-mr11638-sret-fixed-native-gpu-20261006/result.json)와 [x64](../build/local/arm64x-mr11638-sret-fixed-x64-gpu-20261006/result.json)는 exit0·COM owner 반환/refcounts·reverse callback·Unix-call111·Metal readback/fence·validation0·strict no-Rosetta를 통과했다. LLVM22.1.8이 single-field nontrivial `Com<T>` 반환을 scalar `HRESULT`와 같은 entry-thunk 이름으로 합쳐 argument를 shift하는 실제 실패를 확인했고, ARM64EC에서만 zero-size 추가 field로 object size/alignment를 유지하며 thunk identity를 구분했다. 수정 전 실제 x64 fixture timeout도 보존했다.
- [Native x86 WoW64 Metal run03](../build/local/wow64-metal-run03-20261006/result.json)는 실제 PE32 caller/callee의 COM·callback·Unix-call111과 two-node native Metal3 blit64 bytes=`0xa7`·fence=`0x123456789`를 통과했다. Guest memory3932160은 native window base105557411233792를 더한105557415165952로 변환되며 NULL·const/mutable setters·nested next·out-of-window rejection을 검사했다. Exit0, validation0, exact loaded hashes, strict no-Rosetta와 prefix 정리를 확인했다. 이것을 full x86 D3D12 shader acceptance로 확대하지 않는다.
- [x64 smoke 진단](../build/local/arm64x-mr11638-fex-sret01-smoke-diag01-20261006/result.json)은 adapter0 enumeration/GetDesc1을 통과하나 device creation은 `0x887a0004`다. ARM64EC compiler가 `__x86_64__`도 정의하고 SDK raw Core preflight는 AMD64를 요구하는 반면 paired file machine은 ARM6443620이다. 해당 source branch를 수정했으며 fresh pair/build/run 검증은 진행 중이다. [Unified ARM64X+WoW64 Wine](../build/local/arm64x-wow64-window01-runtime-20261006/runtime-result.json)의 full build는 통과했지만 [native guest entry](../build/local/arm64x-wow64-window01-native-entry-20261006/result.json)는 timeout·before-main failure다. Plain guest-window 비교의 execute AV와 함께 보존하며 all-three-guest acceptance로 세지 않는다.

- [SDK02 paired build](../build/local/arm64x-wow64-owned-sdk02-20261006/result.json)는 native/EC 두 Meson graph의 Core/Main generation을 `8bdfc035dc084eec88ae025adc630b09`로 통일하고 raw ARM64X Core machine admission을 수정했다. Installed-file backing identity 검사는 유지하며 두 view의 generation mismatch를 실제 corruption regression으로 거부한다. Six paired DLL fresh build101.65s, metadata/corruption regression0.23s, [unified package](../build/local/arm64x-wow64-package-sdk02-20261006/runtime/owned-graphics-runtime.json)76.29s를 통과했다.
- [SDK02 native ARM64](../build/local/arm64x-wow64-sdk02-native-abi-gpu-20261006/result.json), [FEX x64](../build/local/arm64x-wow64-sdk02-x64-abi-gpu-20261006/result.json), [FEX x86](../build/local/arm64x-wow64-sdk02-x86-abi-gpu-20261006/result.json)는 동일 manifest `7bd2f169c7020cf872f0a52fdfb8d81423c8d622037191ddec07cb7da633e9a6`에서 실제 COM owner/scalar/refcount·reverse callback·Unix-call111·native Metal64-byte readback/fence를 통과했다. Native entry의 execute AV는 native fixture와 paired PE의 raw section/file alignment를16KB로 맞춰 해결했으며 native execute policy를 완화하지 않았다. Runner는 초기 caller만이 아니라 실제 guest child의 동일 PID 내 wine/ntdll.so/winemetal.so hash-matching closure를 검증한다. 세 실행 모두 exit0·validation0·strict no-Rosetta·prefix 정리다.
- [SDK02 native ARM64 graphics/compute](../build/local/arm64x-wow64-sdk02-native-smoke-20261006/result.json)와 [FEX x64 graphics/compute](../build/local/arm64x-wow64-sdk02-x64-smoke-20261006/result.json)는 같은 runtime에서 SM50/DXBC graphics/compute8frame exact readback·exit0·Metal API/Shader Validation0·strict no-Rosetta를 통과했다. `--no-present` 실행이므로 presentation·real-game·전체 DXIL/native regression acceptance로 확대하지 않는다. [PE32 shader 첫 실패](../build/local/arm64x-wow64-sdk02-x86-smoke-20261006/result.json)는 guest main 전 `NtUserInitializeClientPfnArrays`의 raw guest pointers에서 발생했다. Core wow64와 달리 wow64win의 typed memory helpers가 guest window base를 적용하지 않은 실제 경계를 확인해 root-owned patch로 수정했으며 이후 fresh runtime 실행은 별도 기록한다.

- [Window03 Wine build](../build/local/arm64x-wow64-window03-runtime-20261006/runtime-result.json)는 wow64win의 DLL-local guest-base query와 shared typed memory helpers를 추가해 zero-fuzz 적용·full build432.24s를 통과했다. [Fresh package](../build/local/arm64x-wow64-package-window03-20261006/package-result.json)75.76s와 native ARM64/x64의 ABI·graphics/compute8frame은 PASS다. 그러나 [PE32 ABI](../build/local/arm64x-wow64-window03-x86-abi-gpu-20261006/result.json)와 [shader](../build/local/arm64x-wow64-window03-x86-smoke-20261006/result.json)는 guest main 전 exit53으로 실패했다. [Correct wow-channel trace](../build/local/arm64x-wow64-window03-x86-smoke-diag02-20261006/execution.log)에서 actual guest0140의 wow64win DllMain은 return1, base query는 실행됐고 마지막 native 호출은 ProcessImageInformation이다. Prefix helper00e0의 별도 user32/winemac 오류와 구분한다. Loader·Unix loader/process·core wow64 syscall source SHA256은 accepted Window01과 동일함을 확인했다. Window03은 all-three-guest accepted cut으로 세지 않으며 SDK02 성공 증거는 보존한다.

- [Window04 package](../build/local/arm64x-wow64-package-window04-20261006/package-result.json)는 module/callback identities를 보존한 wow64win patch로 full Wine build·fresh packaging76.41s를 통과했지만, default `+loaddll,+seh`의 [PE32 ABI](../build/local/arm64x-wow64-window04-x86-abi-gpu-20261006/result.json)와 [shader](../build/local/arm64x-wow64-window04-x86-smoke-20261006/result.json)는 모두 guest entry 전 exit53이다. Manifest는 `555b094994d6dc75717fcfad8118b380b5a781e2008aac8666d06fc5b4d07b8e`이며 실패 prefix는 종료 후 제거했다.
- [SDK02 default comparison](../build/local/arm64x-wow64-sdk02-x86-abi-comparison02-20261006/result.json)는 같은 PE32 ABI fixture의 COM·Unix-call·native Metal·strict no-Rosetta를 다시 통과했다. 반면 SDK02에서도 `+relay`를 켠 comparison01은 exit53이므로 relay는 이 경로에서 동작을 보존하는 진단이 아니다. Window03/04 회귀를 acceptance로 세지 않고, 후속 patch는 실제 실패한 `NtUserInitializeClientPfnArrays`의 outer CPU arrays만 변환하며 공통 helpers와 callback/module identities는 그대로 둔다.

- [Window05 PE32 default address probe](../build/local/arm64x-wow64-window05-x86-address-diag01-20261006/result.json)는 shared helpers를 유지하고 client outer arrays만 변환해도 guest main 전 exit53이다. Accepted Window01과 Wine source 전체 비교는 `dlls/wow64win/user.c` 하나만 다르다. Native observer가 Mach-O bases를 함께 기록하도록 바꾼 뒤 [SDK02 control](../build/local/arm64x-wow64-sdk02-x86-address-control01-20261006/result.json)은 COM·Unix-call·Metal·strict no-Rosetta를 통과해 observer 변경의 동작 보존을 확인했다.
- Window05 helper process34127에서 native ntdll base `0x1021b8000`, fault PC `0x1021c0778`의 offset `0x8778`을 `atos`로 심볼화하면 `RtlInitCodePageTable +0`이다. 첫 `ptr[1]` 읽기가 raw guest ANSI table `0x250000`의 `0x250002`에서 실패했다. 따라서 PEB BeingDebugged 가설은 기각한다. `ntdll/locale.c`의 PE32 초기화가 native PEB64의 ANSI/OEM/case-table fields에도 raw guest addresses를 게시하는 producer boundary를 확인했다. 후속 owned patch는 native TEB의 full PEB address와 PE32 view 차이로 guest base를 얻어 PEB64에만 physical pointers를 게시하고 PE32 PEB fields는 유지한다. Prefix helper와 실제 requested guest의 exit53 인과관계는 아직 확정하지 않는다.

- [Window07 package](../build/local/arm64x-wow64-package-window07-20261006/package-result.json)76.29s는 native PEB64 NLS publication을 수정한 full Wine build와 fresh stage verify를 통과했다. Manifest `e64b99c20d70c6f36ac632b49fe48040a8a26e544b7f0b1a43adf1bf3336a0cb`의 [PE32 ABI/GPU](../build/local/arm64x-wow64-window07-x86-abi-gpu-20261006/result.json)는 guest entry·COM·Unix-call·native Metal·strict no-Rosetta PASS다. [PE32 shader](../build/local/arm64x-wow64-window07-x86-smoke-20261006/result.json)는 guest main 전 exit66으로 다음 경계를 드러냈다. Actual shader guest014c/host81067의 native opengl32 base `0x60002493c000`, PC `0x6000249af2a0`은 `wow64_thread_attach +4`이며 raw guest TEB32 `0x7ffda000`의 WowTebOffset read `0x7ffdafdc`에서 실패했다.
- 후속 root-owned patch는 실제 D3D12 DLL import initialization에 호출된 `opengl32/unix_private.h`의 shared `get_teb64`에서만 Darwin ARM64 guest-window bias를 복원한다. Rendering backend를 추가하지 않고, 다른 OpenGL API의 미검증 pointer contracts를 구현 완료로 세지 않는다. clangd가 build-source references를 찾지 못해 generated callers와 shared generator를 별도로 확인했다. Observer `-Wall -Wextra -Werror` compile과 runner2 regressions는 PASS다.

- [Window08 package](../build/local/arm64x-wow64-package-window08-20261006/package-result.json)76.82s와 full Wine build는 corrected shared TEB32 dereference를 포함해 PASS다. Manifest `f7f2f91a648155b8b473efea3f0972a05a2d7d6dedcf6e8c3f6ce6a751a36046`의 [PE32 shader execution](../build/local/arm64x-wow64-window08-x86-smoke-20261006/result.json)은 actual guest main·loaded-module markers·owned backend·adapter enumeration hr0까지 진입했다. 이후 Unix-call114 `DispatchData_alloc_init`에서 native memcpy가 raw guest embedded metallib address `0x78e07250`, length `0x99b9d`를 읽어 fault했고 thunk assertion 후 exit3이다. GPU/8frame/strict acceptance는 아직 false다.
- `Metal.hpp`의 guest-memory `newLibrary(const void *)`와 `MakeDispatchData(void *)`만 existing guest base로 physical CPU addresses를 전달하도록 수정했다. Native compiler-owned `uint64_t` buffer overload와 cross-DLL Unix-call114 contract는 유지한다. Fresh SDK03 paired build와 packaged actual execution은 별도 gate다. clangd diagnostics는 include-toolchain 설정의 `cstdint` missing부터 실패해 여기서 green으로 세지 않으며 실제 native/EC/PE32 target builds로 검증한다.

- SDK03 paired six-DLL build99.09s와 corruption/TLS/Core-generation regression0.24s, [package](../build/local/arm64x-wow64-package-sdk03-20261006/package-result.json)76.10s는 PASS다. Manifest `1b27415b38bd8903c2783901927e3228836241ccdbbfd24ceb2cdaeadb283b15`의 [actual PE32 shader](../build/local/arm64x-wow64-sdk03-x86-smoke-20261006/result.json)는 embedded metallib load와 D3D12 device creation hr0까지 복구됐으나 `CreateGraphicsPipelineState` hr0x80004005/exit1로 실패했다. Actual guest007c/host49131의 final native string read는 raw CPU function-name address `0x78dc8a70`이고 `WMTMetal4PipelineLibraryFunction`은 args[1]을 native NSString UTF8 initializer로 전달한다. `Metal4Pipeline.hpp`의 name/constants/info/function-array CPU slots에만 existing guest base를 적용한다. Library/compiler/function handles와 counts, native compiled-buffer uint64 overload는 그대로다. SDK04 actual acceptance는 아직 pending이다.
- 개발 중 사용이 끝난 Window03/04/05 `wine-build/` object trees13.9GB와 obsolete raw/staged runtime `bin/lib/share` payloads를 제거했다. Source snapshots, configure/build logs, runtime results, package manifests와 execution evidence는 보존했고 SDK02 comparison/runtime와 현재 Wine08 source/runtime/build를 유지한다. Cleanup 후 available space는152GiB다.

- SDK04 paired six-DLL build97.16s, corruption/TLS/generation regression0.27s와 [package](../build/local/arm64x-wow64-package-sdk04-20261006/package-result.json)75.50s는 PASS다. Shared native/EC Core/Main generation은 `260d37254468412baca25e437cdbab2c`다. Manifest `f80cff4e08c4b49c538e4f3f8149acd9f0382fa76e9a73d2d849ced7de3ee170`의 [PE32 shader](../build/local/arm64x-wow64-sdk04-x86-smoke-20261006/result.json)는 graphics/compute PSO 생성 뒤 execution/readback으로 진행했지만 readback Map hr0x887a0005와 teardown assertion/exit3이다. Actual child71456의 WineMetal base `0x600040000000`; native LR `0x600040079108`은 `SM50CopyCompiledBitcode`, teardown PC `0x60004001ad6c`는 `m4_commit:509`로 atos가 확인했다. Compiler-owned source는 native address이고 caller destination `0x1e6698/0x1e4e70`는 raw guest CPU address였으며, queue commit의 command-buffer handle array와 retirement info output도 guest CPU memory다.
- `airconv_thunks.c`의 copy destination 및 `Metal4.hpp`의 typed CPU command/commit/info/label inputs만 existing guest base로 인코딩한다. Compiler/Metal handles, GPU addresses, submission owner retention, acquire/release와 completion ordering은 바꾸지 않는다. Null labels/arrays는 null을 유지한다. SDK05 fresh paired build와 actual shader checks는 다음 gate다.

- **SDK05 동일 cut 실제 실행 완료:** [paired build](../build/local/arm64x-wow64-owned-sdk05-20261006/result.json)97.34s, corruption/TLS/Core-generation regression0.27s와 [package/stage verify](../build/local/arm64x-wow64-package-sdk05-20261006/package-result.json)75.94s는 PASS다. Native/EC Core/Main generation `40ddcd07ca794d7d8d1e234d8bc54115`, Wine08 result SHA `0fbbd39833ba083178158619a5aae661875590e555eedcf549a5caf09ff838a1`, paired result SHA `72ecc95e09bc4be650274dab0163b051fcbc14dcb77d0e18a75f84bdf5e74249`다. Runtime `build/local/arm64x-wow64-package-sdk05-20261006/runtime`의 동일 manifest `c59930623bc44b5120d6a74534c4d0be2b5704ecf910a67e252b23e4729453dc`를6개 실제 실행이 사용했다.
- SDK05 ABI/COM/Unix-call/Metal은 [ARM64,15.97s](../build/local/arm64x-wow64-sdk05-arm64-abi-gpu-20261006/result.json), [FEX x64,16.07s](../build/local/arm64x-wow64-sdk05-x64-abi-gpu-20261006/result.json), [FEX x86,16.11s](../build/local/arm64x-wow64-sdk05-x86-abi-gpu-20261006/result.json) 모두 PASS다.32-byte packet·mixed COM owner/scalar·callback1·Unix-call111·64-byte0xa7·fence0x123456789와 PE32 CPU codec/two-node blit를 실제로 확인했다.
- SDK05 graphics/compute는 [ARM64,20.60s](../build/local/arm64x-wow64-sdk05-arm64-smoke-20261006/result.json), [FEX x64,15.94s](../build/local/arm64x-wow64-sdk05-x64-smoke-20261006/result.json), [FEX x86,14.33s](../build/local/arm64x-wow64-sdk05-x86-smoke-20261006/result.json) 모두8 ordered exact-readback frames PASS다.6개 실행 모두 exit0·Metal API/Shader Validation0·exact loaded PE/native hashes·actual child ARM64/proc_translated0·strict no-Rosetta·prefix cleaned다.
- 이 SDK05는 앞의 PE32 startup/compiler/Metal CPU-pointer failures를 실제 실행으로 복구한 완료 cut이다. Root compiler/Metal native object handles와 GPU addresses, guest callbacks/module IDs, queue retirement ordering은 유지했다. `--no-present`/SM50-DXBC selected smoke이므로 game/presentation/full DXIL/native 전체 regression 증거로 확대하지 않는다. README/build authority와 frozen import baseline은 바꾸지 않았고 git commit은 만들지 않았다.

- SDK05 actual acceptance 후 resolved obsolete SDK03/SDK04/Window07/Window08 staged `runtime/bin/lib/share`를 정리했다. 해당 manifests·source cuts·package results·logs·execution evidence는 보존했다. Current SDK05 build/install/runtime, Wine08 build/source/runtime input과 SDK02 known-good comparison은 유지하며 available space는154GiB다. All6 dedicated prefixes는 runner가 process termination을 확인한 뒤 제거했다.

## Composition 재개와 병렬 진단 — 2026-10-05

### 사용자 요청에 따른 산출물 정리와 잔여 목록 통합

현재 실행 작업과 process snapshot을 확인한 뒤 build/local 아래 임시 Wine prefix 148개 및 obsolete staged runtime 37개의 bin/lib/share를 삭제했다. 삭제 대상259개 부재, protected source/frozen input/result/runtime manifest50개 SHA256 불변, retained runtime044·comparison runtime034 stage verify PASS를 확인했다. Build 점유247.4123→57.4746 GiB, filesystem available68.5272→258.7808 GiB(증가190.2537 GiB), 사용률93→72%다. Native034의 WoW64 PASS와 x64 original false는 그대로 보존했다. Source/toolchains·현재 build/install·ARM64 기반/재현·frozen fixtures·logs/result/drawable captures는 삭제 대상 밖에 남겼다.

[Cleanup manifest](../build/local/cleanup-20261005/manifest.json)에 정확한 삭제 경로·용량·50 hash와 before/after를 보존했다. Obsolete runtime의 manifest/provenance는 남지만 payload를 제거해 과거 경로는 실행 불가이며 다음 실행은 fresh stage가 필요하다. [잔여 구현3개·조건부7개·검증/보류·기능 뒤 최적화](IMPLEMENTATION-PRIORITIES.ko.md#현재-남은-구현-목록--2026-10-05-정리)를 정리했다. Source 변경·새 GPU 실행·기능 또는 성능 개선 완료로 세지 않는다.

### DX11 공용 구현 재사용 확인

사용자 지시로 HDR을 직접 확인하고 Composition/RR source mapping을 scout2개로 병렬 확인했다. DX12는 공용 Presenter·HDR shader·native EDR/NVAPI display setting을 이미 소비한다. DX11 Composition factory는 DXGI_ERROR_UNSUPPORTED이며 NGX는 SR feature1만 구현해 별도 RR/Composition 경로를 이식할 수 없다. HDR metadata의 DX11 unconditional S_OK는 재사용하지 않았다. Product source 변경은 없다. 새 HDR metadata fixture 양 ABI build0·runtime044 각7frame·API+Shader Validation/foreign 오류0·exit0/pass=true, diagnostics95tests5.682s PASS다. Source-frame readback을 tone-map drawable나 Gaming transport proof로 확대하지 않았다. Scoped prefix 종료/정리와 [실제 소스·증거](../docs/history/parallel-resume2-20261005.md)를 보존했다.

### 세 번째 병렬 재개 — native K-tail 원인과 웹 source 대조

사용자의 핵심 세 기능 병렬 구현 및 Unity·Unreal 웹 scout 지시로 production 담당3개와 scout3개를 실행했다. [실제 probe/source/원문/실패/정리](../docs/history/parallel-resume2-20261005.md)를 따른다. Production delta나 commit은 없다. Pristine GPTK composition factory의 unconditional E_NOTIMPL을 native stub 실행으로 확인했고 Advanced Color linear scRGB FP16 규격과 Unity HDR UI/OETF 원문을 확보했다. Unity gamut rotation을 transfer decoding으로 해석하거나 UI alpha0 처리로 DXGI additive oracle를 대신하지 않았다.

RR은 기존 trace replay, default.metallib AIR, actual public specialization observer, same-constants nodispatch binary archive를 실행했다. K44/C16 lane31/tap8/channel15 load가 `[12672,12680)`이며 K guard는 output/postprocess 뒤에만 있다. Generic/specialized AIR와 independent arithmetic check0로 원인을 좁혔다. G17s decoder 실패 및 actual framework ISA 미확정은 별개이며 validation zerofill을 uninstrumented 무해성 proof로 세지 않는다.

HDR은 Samsung 공식 UE sample HTTP206 source/guide를 확보하고 APP shader/GPU driver-scanout TMO, calibration index0reserved/1projector200/2..15=300..8000nits를 확인했다. CTS는 VSIF 매2frame·ALLM1·maxRGB peak를 요구하며 mandatory sink ACK는 확인되지 않았다. Xilinx ALLM/일반 HDR10+ source는 Gaming SSTM나 Apple IOAV ABI가 아니라는 적용 범위를 기록했다. 새 isolated probe/PDF/OCR/archive/arithmetic checks를 검토했고 기존95/104·build/stage는 반복하지 않았다. GPU/display resources는 종료했다.

### 두 번째 병렬 재개 — alpha 전체 backing 검증과 native 계약 경계

사용자가 `.omo` handoff/문서 상태를 확인하고 병렬 구현을 계속하도록 지시해 A Composition/B RR/C HDR를 동시에 실행했다. 각 담당자가 자기 QA 실행을 맡고 GPU/display lease만 직렬 배정했다. [실제 변경·명령·실패·증거·다음 외부 조건](../docs/history/parallel-resume2-20261005.md)을 따른다. Production 수정·commit·새 build/stage는 없으며 runtime044를 재사용했다.

- Alpha 양 ABI consumer cases7/gpu14·ordered13phase와52PNG씩, 총104/104 geometry를 통과했다. Graphics validation/foreign errors0이다. Normative alpha-colour oracle는 여전히 없어 임의 transfer 변경은 하지 않았다. X64 cleanup-only `-k` exit1 원본 실패를 보존하고 같은 prefix `-w` exit0/정리를 보완했다. QA harness는 이미 retired server의 `-k`1만 허용하고 `-w`/fixture/capture exit0은 유지한다. 수정된 i686 harness exit0이며 i686 actual-prefix hash는 수집하지 못했다. 기존 Wine 초기화 오류15개는 보존했다.
- RR 진단 capture/API Validation은 exit0·one-frame trace·원래 matrix guard7/32outputs다. 별도 full-validation observer도 guard7을 유지하지만 Conv3x3Stride1U8 offset12672/length12672 오류32회가 남는다. `_bufWeights`는 MetalFX createBuffers/newBufferWithBytes 생성·argument1/offset0·512×64 dispatch다. Owned3328-byte params나 guide producer가 아니다. Capture와 Shader Validation을 합친 성공으로 세지 않았으며 native kernel 대 instrumentation 원인은 미확정이다. 오래된 capture binary의 observe 인자 실패는 보존하고 지원되는 observer로 바꿨다.
- HDR readonly probe는 warnings-as-errors compile0/run0, reference headroom0 및 실제12exports를 확인했다. IOAV generic InfoFrame selector0x11/32byte transport는 존재하지만 Gaming payload/ACK/readback/ownership/SSTM 계약으로 이어지지 않는다. Pristine GPTK4 identity/관련 static path를 확인했지만 working Gaming oracle는 아니다. EDID 재조사나 setter/I2C 전송은 하지 않았다.
- Lead CPU diagnostics95tests PASS. Alpha prefix/cache/control과 native probe processes를 정리하고 GPU/display lease를 해제했다. HWND/사용자 창 geometry 변경은 없으며 로그/JSON/PNG/trace를 보존했다. 세 기능 전체 구현은 외부 oracle/native 계약 조건에 blocked로 남긴다.

### 후속 병렬 실행과 사용자 요청 중단 — runtime044

A Composition/B RR/C HDR는 배타 경로로 동시에 시작했다. 이후 공통 build/stage와 lead GPU/display 검증이 직렬 병목이 됐고 사용자는 “병렬로 하는거 맞냐. 문서 업데이트하고 그만 해라. 너무 느리다.”고 중단을 지시했다. [최신 source delta·실행 증거·실패·정리·미완료 ledger](../docs/history/parallel-implementation-stop-20261005.md)를 따른다. 프로젝트 commit이나 전체 계약 완료 선언은 없다.

Runtime044는 임시 Composition trace/model만 제거했으며 shared NOW deadline/positive-duration/exception HRESULT는 유지했다. 양 ABI build/install/stage/verify0, frozen cadence warmup3/public12/12, 별도 Sync3 public3/3 및 native 약50ms 간격, independent WindowServer 실제 palette 전환을 얻었다. Mixed 양 ABI6phase×2scoped snapshot은 mismatch0이며 FALSE BLUE와 TRUE offset18,12 RED32×24, detach cached BLUE/GREEN 및 Presents1/1/1를 확인했다. x64 screen tail은 새 사용자 창 가림 때문에 미검증이다. 일반 smoke 양 ABI와 composition/present-queues/colorspace 양 ABI6suite는 owned marker/validation/foreign/exit 기준을 통과했다.

Alpha x64는 identity부터 content-pending까지6phase×4PNG만 캡처한 뒤 content-detached READY에서 중단했다. Alpha13phase 전체·i686 capture·calibrated Windows transfer/gamut oracle·normative parity는 미완료다. RR native model weights12672bytes의 argument1/offset0 producer를 관측했으나 원래 guard7/32outputs에도 native invalid load32회가 남았다. HDR 현재 link VSVDB0/calibration null/preset0과 native Gaming negotiation/readback/SSTM 부재는 분리한 limitation이고 production 수정은 없다. Alpha prefix/cache, worker/watchers, screen streams, control 파일, wake assertion을 정리하고 임시 세 창의 원래 geometry를 independent readback으로 복구했다. 이전 runtime043 cut은 아래에 보존한다.

**마지막 cut/handoff:** [runtime043 원본](../build/local/composition-resume-20261005/cadence-absolute43-manual-x86_64/stdout.log)은 duration0을 같은 NOW absolute-time으로 보내 x64 warmup3·critical public12/12·consumer0을 얻었다. Native validation/exception/foreign errors0이며 screen ROI는 가림 없이 이전 warmup에서 새 내용으로 바뀌었다. 빠른 Sync0 capture는 뒤 frame의 색을 잡았으므로 독립 per-frame physical cadence를 주장하지 않는다. 양 ABI cadence·alpha/Presenter 회귀는 남았고 사용자의 다음 세션 병렬 구현 요청으로 중단했다. [소유권·증거·정리·다음 작업](../docs/history/remaining-implementation-20261005.md)을 따른다.

후속 사용자 요청으로 중단 지점부터 재개했고, 파일 소유권이 겹치지 않는 조사 트랙을 병렬화했다. 아래는 이 재개 구간의 실제 실행이며 이후의 중단 snapshot을 덮어쓰지 않는다.

- **SetOffset/z-order와 양 ABI opaque mixed:** 승인 clone `toolchains/wine-game-display`의 visual SetOffsetX/Y, Windows/native target-state24-byte ABI와 offset layout, FALSE-target z-order를 수정했다. Runtime038 [x64](../build/local/composition-resume-20261005/mixed-offset38-x86_64/actual-stable-phase-evidence.json)·[WoW64](../build/local/composition-resume-20261005/mixed-manual38-i686/pixel-verdict.json) 각각6 phase×2 capture PASS. 후자는 고정 sleep 대신 frozen VISUAL_READY 단계에서 직접 캡처·검사했고 consumer0·Metal API/GPU Validation enabled·validation/foreign errors0·actual DLL hash 일치를 확인했다. RED3072/BLUE17408과 offset18x12·32x24 root, cached detach 복원을 확인했으며 inspector edge tolerance·opaque hue/geometry 범위다. Alpha-colour parity나 물리 표시 timestamp proof가 아니다.
- **가림과 cadence 실패 보존:** runtime038에서 가림 제거 후 warmup3은 새 positive event로 측정됐지만 첫 Sync0는 unmeasured였다. Runtime039의 presentAfterMinimumDuration0와040의100µs floor도 warmup3 뒤 같은 critical FAIL이어서 이 실험 변경을 제거했다. 별도 controlled038은 source 창만 같은 출력의 x1810으로 옮겨 first-bound physical/backing PNG를 얻었지만 첫 warmup 실패로 Sync0 hold에 도달하지 못했다. Original verdict/logs/PNGs를 유지하며 환경 변경을 성공으로 가장하지 않는다.
- **Transaction 후보:** 현 SDK의 presentsWithTransaction 계약을 확인한 뒤 explicit flush와 등록/발행 추적을 후보로 만들었다. Runtime041의 실제 build/install/stage/verify는0이며 아직 consumer acceptance를 얻지 못한 후보다. Sync0–4 양 ABI cadence, normative alpha-colour와 일반 Presenter 회귀는 열린 상태다.
- **RR 독립 optional-input 비교:** [isolated classic NO x86_64 control](../build/local/rr-native-resume-20261005/optional-hit-disabled-20261005/comparison.md)의 descriptor hit enable과 binding만 변경하고 원본·9개 allocated texture·입력 bytes·validation·matrix guard를 유지했다. Compile0, baseline1, 동일 pipeline UID·12672/12672 invalid loads32회, finite/non-copy32 outputs, matrix-sensitive pairs7→0. 오류 allocation은 MetalFX Conv3x3Stride1 createBuffers다. Supplied guide 제거·buffer padding·validation suppression을 production 수정으로 채택하지 않았다. Native PID 종료와 GPU slot 반환을 확인했다.
- **HDR fresh read-only evidence:** [native query](../build/local/hdr10plus-resume-20261005/native-query.log)·[EDID hashes](../build/local/hdr10plus-resume-20261005/edid-sha256.log)의 genuine 두 경로가 동일 SHA256 `112933369a46b8604e3289d66575ebb7ae3ea043060ec92f6d565040bef288aa`이며 PQ/static HDR·VSVDB0, external presets0/active-1/calibration null이다. Pristine GPTK4.0b2 identity를 재확인했으나 성공한 Gaming transport/low-latency/readback/SSTM으로 추론하지 않았다. [정확한 missing contracts](../build/local/hdr10plus-resume-20261005/findings.md)는 현재-link hardware gate와 owned 구현 부재를 구분한다.
- **조건부 기록:** [일곱 항목 ledger](../build/local/conditional-resume-20261005/findings.md)는 CoreWindow·Wave64/128·상위 SM·상위 SDK/Agility·stereo·overlay·multi-adapter의 실제 trigger와 prerequisite를 구분한다. Bounded selected-game evidence에서 새 trigger가 확인되지 않았다는 결론이며 일반 게임 부재나 native 불가능성 선언이 아니다. 실제 loader와 optional graphics를 tooling/불필요로 제외하지 않았다.
- **Cleanup:** 실패한038/039/040 autonomous cadence, controlled038와 WoW64 mixed의 dedicated wineserver를 종료/대기하고 disposable prefixes를 제거했다. WoW64 mixed cache/control, controlled control/stdout watcher도 제거했다. 원본 log/verdict/RAW/PNGs·manifest와 최신 검증용 runtime은 유지하며, 진행 중041 QA의 resource는 별도 완료 때 정리한다.

## 사용자 요청에 따른 작업 중단 snapshot — 2026-10-05

사용자가 “작업 일단 멈추고 한데까지 문서 업데이트”를 요청했다. 구현·조사·build/GPU/GUI는 중단했고 소스와 원본 실패를 동결했다. 마지막 양 ABI build/install/stage0은 `runtime-36-bound-source-tearing-permission`이며 actual Source cadence와 mixed consumer는 각각1로 종료했다. 이는 전체 게임/API 완료가 아니며 상세 원본은 아래 multi-frame/native residency ledger에 연결한다.

- 보존한 완료 범위: genuine N1–4 FG·양 ABI140 headless·mixed-cut native x64 Root31 full23/WoW64 Root34 full23, fixed-clock CPU와 strict public 양 ABI, NGX count1 facade public 양 ABI 및 saved-capture bootstrap offline recheck, native residency/private replay 지정 proof, Source-only Root33 양 ABI·양 alpha13-phase geometry/lifetime다.
- 동결한 미완료 범위: Root36 첫 warmup ordinal3 전체9 DISJOINT·native first4/seq5/6 time0, mixed x64 ordinary-visible만1/6 phases PASS 뒤 upper.SetOffsetX(18) E_NOTIMPL, nonempty FALSE/TRUE root pixels·WoW64 mixed·normative Windows alpha-colour parity·physical cadence다. Policy candidate나 PNG를 해결/scanout proof로 바꾸지 않았다.
- 이전 depth bounds/conservative 지정 양 ABI proof는 보존하되 구형 물리 GPU proof는 없다. RR Conv3x3 offset12672/length12672·HDR10+ EDID/VSVDB/선행조건은 미해결이며 conditional CoreWindow/Wave64/128/상위 SM/Agility/stereo/overlay/multi-adapter는 entry criteria만이다. 승인 별도 Wine clone `913e31f…`·분리 toolchains와 wine-game-display frontend 경계는 변경하지 않았다.
- 중단 시 disposable12 prefixes 정리 후 disk93GiB를 확보했고 logs/JSON/RAW/PNGs는 보존했다. 이 문서 작업에서 추가 cleanup·build·GPU·GUI·checker 실행은 하지 않았다.

## 이력 읽기와 증거 가용성

- P0–P3 완료는 당시 선정한 slice와 지정 fixture 범위의 판정이며 전체 D3D12/DXR/Shader Model conformance 완료가 아니다. 당시 정상 결과뿐 아니라 FAIL·INCONCLUSIVE·미실행 및 오류 수정 근거를 그대로 유지했다.
- **2026-10-02 외부 작업으로 기존 build/ 전체가 삭제됐다.** P0–P3 및 삭제 전 Metal 4·UMA·D3D11·presentation 원본 runtime/log/JSON은 현재 보유 증거가 아니다. 경로와 당시 hash는 provenance/관찰 기록으로 남긴 것이며 재실행이나 복원본을 뜻하지 않는다. 이후 metal4/recovery, ARM64 foundation/SDK-plugin, review evidence는 별도 신규 관찰이다. 최신 보유 여부는 해당 manifest와 artifact를 확인해야 한다.
- Recovery 118/118은 runtime-02의 44개 새 실행 + runtime-01의 74개 유지한 결과다. 이 이동 작업에서는 GPU·Wine·빌드·게임을 실행하지 않았다.
- **후속으로 바뀐 계약:** P2 당시 VRS·RayQuery·mesh·feedback·view·WG 거부와 TS+GS indirect/GS stream/DS-only SO 제한은 P3/P4 후속 기록으로 대체됐다. P2의 shared fence token/ring 설명은 [2026-10-02 review disposition](../docs/D3D12-TRANSLATION-REVIEW-2026-10-01.md#implementation-dispositions--2026-10-02)의 observer/high-water/completion-publication 모델로 대체됐다. P4의 view ICB wide-add 설명은 같은 review의 **32-bit wrap 후 bounds 검사**로 대체됐다.
- P2의 compute CalculateLOD sampler clamp “미검증” 설명은 2026-10-02 정적 감사에서 실제 의미 누락으로 재분류됐다. 기존 point-mip sampler LOD 수치 oracle은 규격상 undefined여서 이 계약의 conformance 증거가 아니다. 후속 compute 수정·mip-linear oracle 및 focused proof의 완료 범위는 [CORE의 Compute CalculateLOD 완료](CORE-IMPLEMENTATION-STATUS.ko.md#compute-calculatelod-완료와-focused-proof)를 따른다. 과거 PASS 자체를 지우거나 실패로 고쳐 쓰지 않는다.
- Native full ARM64 Unix plugin/SDK 성공은 마지막 P4 foundation 기록을 우선한다. 이전 “ARM64 compiler/translator 전체 실행 아님”은 해당 native-domain probe의 한계였으며 plugin 성공을 부정하지 않는다. ARM64 Wine/FEX guest end-to-end는 여전히 별도 gate다.
- SR의 고정 output·low-res MV만 지원·masks/sharpening/nonlinear/FG 미구현이라는 이전 경계는 아래 [full SR/FG port](#full-srfg-port--2026-10-02)의 현행 계약으로 대체한다. 이전 manual-R32 proof를 all-effects/FG public acceptance로 승격하지 않는다.
- Full SR/FG port 절의 Exposure.Scale·MV.Offset 거부와 ResizeBuffers1 per-buffer queues 제한은 **그 port cut 당시**의 기록이다. 후속 새 backlog P1은 유효한 비기본 NGX 값의 accept+ignore·per-buffer queues를 지정 범위로 완료했고 #10 memory query는 사용자 제외다([CORE P1](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)). 과거 관찰을 새 runtime proof로 고쳐 쓰지 않는다.
- 아래 historical 재현 명령·toolchain path는 당시 recipe다. 신규 실행 안내가 아니며, 새 검증은 저장소 루트에서 fresh output/runtime/prefix로 한다. 기존 게임/live prefix를 사용하지 않는다.
- Depth bounds의 cap FALSE·setter ignore·활성 PSO 거부라는 과거 기록은 아래 [P2 #12 native 구현](#p2-12-depth-bounds--2026-10-04)으로 지원 장치에서 대체됐다. Apple7/8/9 emulation도 아래 [후속 stored-depth 구현](#p2-12-이전-apple-family-depth-bounds-emulation--2026-10-05)의 지정 forced/native proof로 대체됐지만 구형 물리 GPU 실행 proof는 아니다. 과거 native-only 관찰을 새 runtime 결과로 고쳐 쓰지 않는다.

## 이력 색인

### Multi-frame FG와 native borrowed residency — 2026-10-05

[현재 지원 경계](CORE-IMPLEMENTATION-STATUS.ko.md#multi-frame-fg와-native-residency-현재-경계)를 따른다. 이전 full-port/P4의 single-output MetalFX FG와 그 native drawable PASS는 당시 cut의 관찰로 보존하며 새 multi-frame 구현·표시 acceptance로 전용하지 않는다. 아래는 기존 artifact와 통합 관찰의 문서 정정이며 이 문서 작업에서 새 build/GPU/GUI 실행은 하지 않았다.

- **현재 소스·직접 알고리즘 proof:** MIT AMD FSR3 pin `0836aa7f24058b3f8d035a88c0774457d78fc98e`의 [75-file closure](../build/local/multi-fg-phase-20261004/dependency-closure.json)와39개 FP32 `cs_6_2` generated DXIL을 사용한다. [Producer receipt](../build/local/multi-fg-phase-20261004/producer-receipt.json)와 [실험 provenance](../build/local/multi-fg-phase-20261004/PROVENANCE)를 보존한다. 요청 N1–4의 phase `k/(N+1)`는 immutable real pair에서 독립 계산하며 actual count를 한 batch로 publish한다. Prepare/OF·history advance는 REAL에 한 번, bootstrap/reset/scene cut은 actual0·caller outputs 보존이다. Supplied subrect 바깥은 REAL current다. MetalFX FG path·one-output fast path·같은 output copy 중복을 사용하지 않는다. [GPU06](../build/local/multi-fg-phase-20261004/gpu-06/stdout.log)부터 [GPU09](../build/local/multi-fg-phase-20261004/gpu-09/stdout.log)까지의 직접 probe는 N1–4·geometry/temporal RGB·UI·history·jitter/depth negative control을 통과했다. 이는 pinned AMD midpoint 외 arbitrary-phase 확장에 대한 owned bounded proof이며 AMD 공식 arbitrary-phase API·실게임 품질/성능 동등성 선언이 아니다.
- **SDK public 부분 proof:** grouped N1–4·debug/history/lifetime consumer signature와 initialized SDK static closure를 구현·유지했다. Root26 `fsr-static-26`의 [x64](../build/local/multi-fg-phase-20261004/fsr-static-26/headless-x64/stdout.log)·[WoW64](../build/local/multi-fg-phase-20261004/fsr-static-26/headless-x86/stdout.log)는 bootstrap9→combined10/debug closed replay까지 headless case를 통과한 뒤 stale physical-queue gate fixture에서 중단했다. 전체 run PASS가 아니다. `[A,B,A]` gate fixture는 accepted prior composition outputs 기준 prior3→slot0 A,10→slot1 B,17→slot2 A로 고쳤지만 새 strict presentation proof는 별개다.
- **Root25 실제 native fault:** [native-fault-evidence](../build/local/multi-fg-recording-state-20261005/gpu-gc25-native-fault-x64/native-fault-evidence.json)의 `-[MTL4GPUDebugCommandBuffer applyResidencySets:]`에서 bad allocation `0x6000001e9170`을 확인했다. Native preCommit borrowed hash에는 남았지만 current GLOBAL 및 GPUDebug/일반 committed 양쪽354-key set에는 없었다([committed walk](../build/local/multi-fg-recording-state-20261005/gpu-gc25-native-fault-x64/committed-walk-evidence.json), [all threads at fault](../build/local/multi-fg-recording-state-20261005/gpu-gc25-native-fault-x64/all-threads-at-native-fault.json)). Caller COM retention이나 새 result GC만으로 이 borrowed native traversal 수명을 보장했다고 쓰지 않는다.
- **Root27 수정과 bounded lifetime proof:** [Device helper](../src/d3d12/d3d12_device.cpp)의 `CommitMetal4CommandBuffers`는 기존 `residency_lock_`을 동기 `queue.commit`까지 잡는다. [Queue](../src/d3d12/d3d12_command_queue.cpp)·[workgraph](../src/d3d12/d3d12_work_graph.cpp)·Device sparse publication의 세 caller를 이 경계로 이관한4-file 변경이며 GPU wait/retirement/destruction은 lock 밖이다. [Build x64](../build/local/multi-fg-phase-20261004/build27-x64.log)·[x86](../build/local/multi-fg-phase-20261004/build27-x86.log)의 build/install과 [paired stage](../build/local/multi-fg-phase-20261004/stage-runtime-27-native-lifetime-linear.log)를 통과한 cut은 `runtime-27-native-lifetime-linear`다. [GC96 x64](../build/local/multi-fg-recording-state-20261005/gpu-gc27-x86_64/stdout.log)·[WoW64](../build/local/multi-fg-recording-state-20261005/gpu-gc27-i686/stdout.log)는 max-inflight4·24 batches·24 empty-metadata probes·96 exact public readbacks, pending caller list/output releases와 final batch input release·4 late-drop queries를 통과했다. Frame96 retained 반복 query2회는 actual1/phase0.5, explicit drop 뒤 final_pending0이다. [x64 exit0](../build/local/multi-fg-recording-state-20261005/gpu-gc27-x86_64/exit-code.txt)·[WoW64 exit0](../build/local/multi-fg-recording-state-20261005/gpu-gc27-i686/exit-code.txt)이며 이 controlled workload의 수명 proof로만 세지 장시간·다른 장치·모든 native 경로 성공으로 확대하지 않는다. 첫 GC의 Metal debug/shader-validation 요청에는 `MTL_SHADER_VALIDATION_REPORT_TO_STDERR`가 빠져 full shader stderr clean proof가 아니다.
- **Root27 state admission 실패 기록:** [graphics/compute·predication·cross-queue stdout](../build/local/multi-fg-recording-state-20261005/gpu-all27-x86_64/stdout.log)의 부분 복원은 통과했지만 [WoW64 stderr](../build/local/multi-fg-recording-state-20261005/gpu-all27-i686/stderr.log)의 closed-list replay는 resource933 expected COPY_DEST1024/actual READ64로 거부했다. 당시 GC96 PASS를 all-state suite PASS로 합치지 않으며 이 실패는 아래 Root28 지정 proof로 대체됐다.
- **Root28 일반 scope 수정·전체 private96 proof:** G의5-file 소스 변경은 Begin/End state-event indices와 첫 Begin 이전 ALREADY_STARTED snapshot을 기록한다. Replay에서 private scope events만 queued validation·decay에서 제외하며 first execution·app outside-scope events와 scope 밖 SDK initialization은 strict 검증을 유지한다. Initialized SDK static closure는 유지한다. [x64 stdout](../build/local/multi-fg-recording-state-20261005/gpu-all28-x86_64/stdout.log)·[WoW64 stdout](../build/local/multi-fg-recording-state-20261005/gpu-all28-i686/stdout.log)은 graphics UAV37/RT37/IA18/primitives6/VS18/PS96/CS0, compute6 slots77/CS6과 predicated-false 출력 불변을 통과했다. Cross-queue API는 fences0인 동안 반환하고 retire 뒤 양 fences1이다. Closed replay exact `ff665544`/flag `8899aabb`·outcome 불변, early owner release `ff332211`/flag1, OFF-before-execute actual0·출력 불변을 확인했다. GC96은 max4·24 batches·24 empty-metadata probes·96 public readbacks·late drops4·frame96 retained 반복 actual1/phase0.5/FALSE flag·explicit drop/end0을 통과했다. [x64 exit0](../build/local/multi-fg-recording-state-20261005/gpu-all28-x86_64/exit-code.txt)·[WoW64 exit0](../build/local/multi-fg-recording-state-20261005/gpu-all28-i686/exit-code.txt)이며 MetalDebug·ShaderValidation·enableReporting·REPORT_TO_STDERR를 켠 full-state/private96 지정 proof다. [x64 stderr](../build/local/multi-fg-recording-state-20261005/gpu-all28-x86_64/stderr.log)·[WoW64 stderr](../build/local/multi-fg-recording-state-20261005/gpu-all28-i686/stderr.log)를 보존한다. Root27의 reporting 누락 경계는 그대로 당시 기록으로 유지하며 Root28 proof를 public FG 전체 표시 완료로 확대하지 않는다.
- **FSR/NGX 표시 미완료:** Root27 `fsr-disjoint-observed-27` [x64](../build/local/multi-fg-phase-20261004/fsr-disjoint-observed-27/present-x64/stderr.log)·[WoW64](../build/local/multi-fg-phase-20261004/fsr-disjoint-observed-27/present-x86/stderr.log)는 첫 `FG_PRESENTED` time0, matched actual NSWindow visible1/occlusion_visible0이다. 새로운 native presentation 전체 PASS로 세지 않는다. NGX Root17 [headless x64](../build/local/multi-fg-phase-20261004/ngx-replay-window-17/headless-x64/result.json)·[WoW64](../build/local/multi-fg-phase-20261004/ngx-replay-window-17/headless-x86/result.json)의140 phase+4 scene checks는 PASS지만 strict native present는 legal copied-list release 회귀와 별도이며 Root27 GC fix 뒤 NGX 새 실행 proof는 아직 없다.
- **Composition 부분 관찰의 한계:** Root27은 첫 unbound Present→Commit, actual Float115/ExtendedLinear, 두 plane visible·latchesSource, API7/GPU14 case와26 real-window PNG를 관찰했다. 그러나 [paired-x64-27 geometry/state inspection](../build/local/composition-20261004.vn2CH7/paired-x64-27-geometry-state-inspection-final.json)에 offline5 stale/mismatched state capture와 wide-gamut 색 수치 mismatch가 남았다. Premul alpha0/red64 source가 화면 red149/green255로 보이는 관찰을 clean alpha/색 acceptance로 기록하지 않는다. Scoped SDR NSWindow sRGB lease 수정은 새 proof 대기이며 hardware profile 변경·공개 HDR 지원 선언은 하지 않는다. RR native validation 실패와 선택적 HDR10+ GAMING 미해결은 기존 아래 절의 계약 그대로다.

- **후속 native visibility control·Composition Root28:** [FSR controlled own-window x64 stderr](../build/local/multi-fg-phase-20261004/fsr-controlled-native-27/present-x64/stderr.log)는 acquire/present 모두 occlusion_visible1인데도 first FG_PRESENTED time0이며 [stdout](../build/local/multi-fg-phase-20261004/fsr-controlled-native-27/present-x64/stdout.log)의 public statistics는 measured0이다. Occlusion-only 원인은 반증됐고 strict FSR/NGX physical presentation은 남아 있다. Composition [actual-stable-phase-evidence](../build/local/composition-20261004.vn2CH7/paired-x64-28/actual-stable-phase-evidence.json)는 두 rounds·다섯 poses의 부분 관찰 뒤 content-pending premul GREEN을 Commit 전12 rounds에서 확인했다([pixels](../build/local/composition-20261004.vn2CH7/paired-x64-28/content-pending-premul-pixels.json)). 이는 진짜 pending/layer 결함으로 clone/native 경계에서 조사 중이다. NSWindow sRGB lease는 실제 set됐지만 pixel 변화가 없어 색 fidelity proof가 아니다. Root27 stale capture의 offline 분류와 새 안정 관찰을 혼합하지 않으며 alpha/색·Commit ordering·일반 Presenter 회귀가 닫히기 전 Composition 전체 완료로 세지 않는다.

- **Native auto layer residency 영구 계약:** [Native Metal4 private op44](../src/winemetal/unix/winemetal_metal4.c)의 drawable encode는 actual primary/residual의 auto `CAMetalLayer.residencySet`을 command에서 `useResidencySet`하고 recording lease로 GPU retirement까지 보유한다. 서로 다른 set이면 두 개, 같거나 residual이 없으면 한 개다. Auto set을 직접 mutate하거나 queue persistent residency로 누적하지 않는다. Root25 borrowed-allocation traversal의 commit-lock 수정과 layer-owned drawable set lease는 서로 다른 수명 경계로 유지한다.
- **Root30 NGX count1 public native proof와 offline 판정 교정:** `ngx-actual-contract-30`의 paired 실제 public consumer는 양 ABI exit0이며 full Metal API/Shader Validation·foreign0에서 no-AV/copy-list release·replay/history/reset/UI를 통과했다. Formal drawable9개는 generated phase0.5 frames2/3/4와 REAL frames1–6이며 full RAW·positive times·strict formal critical order를 확인했다. [원래 x64 result](../build/local/multi-fg-phase-20261004/ngx-actual-contract-30/native-x64/result.json)는 PASS다. [원래 WoW64 result](../build/local/multi-fg-phase-20261004/ngx-actual-contract-30/native-x86/result.json)는 consumer exit0이지만 bootstrap seq1/2의 동일 positive timestamp `467332.930750208`을 strict-increase로 거부하여 false다. Checker는 **bootstrap만 nondecreasing**으로 변경하고 formal critical strict/positive/full-RAW 검사는 유지했다. Affected parser test1회 PASS 뒤 같은 saved stdout/stderr/captures만 offline 재분석한 [x64 analysis](../build/local/multi-fg-phase-20261004/ngx-actual-contract-30/native-x64/fg-capture-analysis.bootstrap-clock-recheck-H.json)·[WoW64 analysis](../build/local/multi-fg-phase-20261004/ngx-actual-contract-30/native-x86/fg-capture-analysis.bootstrap-clock-recheck-H.json)는 양쪽 `OWNED_D3D12_NGX_PRESENT_CAPTURE_PASS`·checker exit0이다. [Receipt](../build/local/multi-fg-phase-20261004/ngx-actual-contract-30/bootstrap-clock-offline-recheck-H.json)는 원래 result/analysis/raw log hashes 불변·full_suite_rerun=false를 기록한다. WoW64 원래 failed result는 보존하며 이 PASS를 새 GPU/Wine/native 재실행으로 기록하지 않는다. 이 요청 count1 NGX 지정 acceptance는 FSR all N1–4나 실게임 전체 proof가 아니다.
- **Root30/31 Composition 후속 미완료:** [Root30 actual outcome](../build/local/composition-20261004.vn2CH7/paired-x64-30/actual-outcome.json)은 correct RAW/model·one deferred Present에도 first Commit actual 두 plane GREEN을 닫지 못했다. Source31은 실제 Wine canonical windowDidDrawContent readiness를 연결하는 build 중 cut이며 새 실제 화면 acceptance가 없다. Source29–31 guard 변경을 Composition 전체 완료로 표시하지 않는다. FSR 후속 N1–4 native run도 진행 중이며 RR/HDR 기존 미해결은 유지한다.

- **Composition Root33 readonly target identity·actual geometry/lifetime:** staged macdrv의 slot1은 CreateClientSurface를 호출하며 old source를 숨기므로 readonly NSWindow query는 slot3을 쓴다. Empty target의 actual NSWindow identity를 복원하고 실제 view는 nonnull Commit에서 deferred bind하며 weak NSWindow→UIView identity cache를 FALSE/TRUE/ordinary acquire에 공유했다. [Paired classification](../build/local/composition-20261004.vn2CH7/paired-33-validation-classification.json)과 [x64 stable evidence](../build/local/composition-20261004.vn2CH7/paired-x64-33/actual-stable-phase-evidence.json)·[WoW64 stable evidence](../build/local/composition-20261004.vn2CH7/paired-x86-33/actual-stable-phase-evidence.json)는 identity, matrix-pending, matrix-presented, source-cropped, resized, content-pending, content-detached, root-pending, root-detached, reattached, queued-before-release, queued-captured-identity, content-lifetime의13 phases 모두 양 alpha 각각 연속2회 fresh actual-window pixel/geometry match를 확인했다. 양 ABI consumer exit0·validation_errors=[]이며 WoW64 root-detached는12-round bound 안11 rounds에서 안정화했다. 이는 CGWindow geometry/lifetime proof이고 Windows alpha-colour parity·mixed ordinary-first/nonempty FALSE/TRUE·Sync0–4 physical gap을 증명하지 않는다. Root27–31의 실패를 새 성공으로 고쳐 쓰지 않는다.
- **Composition cadence Root33 실패와 source visibility 경계:** [run-x64-33 stdout](../build/local/composition-cadence-20261005.Bqiyja/run-x64-33/stdout.log)은 public GetFrameStatistics `0x887a000b`/DISJOINT·warmup-frame-not-newly-measured로 실패했다. [Stderr](../build/local/composition-cadence-20261005.Bqiyja/run-x64-33/stderr.log)는 native first4 presented times0·next2 missing이며 요청 Sync2/4 duration `.033333332/.066666664`의 native policy만 확인한다. Actual gap proof가 아니다. [INFERENCE] 가장 유력한 setup 제약은 requested native x1430이 width200 기준 x1296으로 clamp되어 source x1296–1376/80×64가 terminal x3–1427 뒤에 가려진 것이다. Whole NSWindow occlusion-visible1은 blank right strip의 가시성일 수 있어 source visibility proof가 아니다. Throwaway WS_POPUP/no-activation fixture가 실제 source pixels를 노출하도록 수정 중이며 첫 실패를 유지한다. 이 실패로 platform unsupported나 FrameStatistics fixed를 선언하지 않는다.
- **Root34 stable refresh-clock 소스와 검증 경계:** Root33에서는 같은 reached video token `473302731067890`/sync host `473302.732574936`의 반복 query가 per-query QPC correlation으로 `4825489534308→4825489534306`(-2 ticks)이 됐다. [RefreshClockBasis](../src/dxgi/dxgi_refresh_timing.hpp)는 fixed epoch로 native host를 QPC에 투영하고 같은 physical token의 QPC를 exact 재사용하며 진짜 backwards는 DISJOINT로 보존한다. Display/generation/host frequency/video scale 변경과 두 GetFrameStatistics caller의 모든 reset을 연결했다. Old generation field·temporary logger를 제거하고 fake clamp/tolerance/새 clock을 추가하지 않았다. Existing `tests/dxgi_refresh_timing.cpp` captured-edge CPU regression exit0과 [paired x64 build](../build/local/multi-fg-phase-20261004/build34-x64.log)·[x86 build](../build/local/multi-fg-phase-20261004/build34-x86.log)·[stage](../build/local/multi-fg-phase-20261004/stage-runtime-34-stable-statistics-clock.log)는 통과했다. Cut은 `runtime-34-stable-statistics-clock`이며 새 actual FSR 양 ABI 실행은 proof 대기다. CPU/build/stage 성공을 실제 FSR/GPU/clock presentation 전체 PASS로 기록하지 않는다.

- **Root34 실제 public clock acceptance와 native runner 분리:** `runtime-34-stable-statistics-clock`의 [x64 stdout](../build/local/multi-fg-phase-20261004/fsr-stable-clock-34/native-x64/stdout.log)·[WoW64 stdout](../build/local/multi-fg-phase-20261004/fsr-stable-clock-34/native-x86/stdout.log)는 strict per-query clock/count/original guards 아래 양쪽 consumer exit0·callback19/19·builtin UI4/4를 통과했다. Captured-edge CPU regression0과 이 actual public 흐름으로 fixed-epoch QPC bug 수정 proof를 닫았다. [WoW64 result](../build/local/multi-fg-phase-20261004/fsr-stable-clock-34/native-x86/result.json)는23개 exact content·positive ordered native timestamps·runner PASS다. [x64 result](../build/local/multi-fg-phase-20261004/fsr-stable-clock-34/native-x64/result.json)는 public exit0이지만 native runner false이며 [original analysis](../build/local/multi-fg-phase-20261004/fsr-stable-clock-34/native-x64/fg-capture-analysis.json)의 유일한 generated physical phase 미충족은 count/index(1,1)이다. Seq2/drawable1은 zero-time/skipped였고23 raw captures 존재를 실제 표시 proof로 대체하지 않는다. Original native error를 보존하며 동시 양 ABI actual 실행에 따른 physical occlusion은 가능성일 뿐 미입증이다. 외부 counter/native event 조사와 별개로 public clock/count proof를 취소하지 않으며 native runner false를 PASS로 바꾸지도 않는다.
- **현재 per-phase physical proof는 mixed-cut:** [x64 Root31 resize-epoch result](../build/local/multi-fg-phase-20261004/fsr-resize-epoch-31/native-x64/result.json)의 genuine N1–4 physical proof와 WoW64 Root34 native PASS를 함께 보유한다. Root31 fixture는 `fsr-actual-observation-clock-edge-resize-epoch.PCeF3b`다. 이는 양 ABI의 underlying genuine independent-phase physical 증거이지 **Root34 동일 cut 전체 양 ABI native PASS가 아니다.** Root34 x64 count1 physical 미측정과 Composition alpha-colour/mixed-view/cadence·RR/HDR의 별도 acceptance는 계속 분리한다.

- **Root34 counter 정밀도와 FG backend 완료 경계:** 위 callback19/19·UI4/4 shorthand는 submitted19/4와 **통계의 마지막 실제 표시 Present ID19/UI4**가 같다는 뜻이지 positive event의 합계가 아니다. DXGI `PresentCount`는 마지막 positive `event.present_id`를 보고하며 skipped/time0 event를 무시한다. X64 실제 timed events는18 positive callback+4 UI=22이며 seq2/drawable1은 skipped다. 이 seq2는 frame2 generated1의65,536-byte GPU 결과가 byte-exact·REAL과 다른4,096 body channels·marker34/190/220이어도 실제 표시 proof가 아니다. Public frame2의 last ID3 관찰 뒤 DISJOINT, 최종 positive ID19는 이 counter 계약과 모순되지 않는다. 두 ABI NSWindow frame은 x64 `(1356,685,128,160)`/x86 `(1356,507,128,160)`로18px vertical gap이 있으므로 직접 ABI overlap으로 설명하지 않는다. Occlusion 원인은 미입증이다. Root31 x64 all23 positive와 Root34 WoW64 all23 positive의 mixed-cut genuine N1–4·CPU/headless 증거로 선정 FG backend 계약은 닫았으며 Root34 x64 runner false를 수정하거나 같은 Root34 전체 양 ABI native PASS로 기록하지 않는다.
- **Exposed WS_POPUP cadence Root34 actual event 데이터와 Sync0 실패:** [run-x64-popup-34 stdout](../build/local/composition-cadence-20261005.Bqiyja/run-x64-popup-34/stdout.log)·[stderr](../build/local/composition-cadence-20261005.Bqiyja/run-x64-popup-34/stderr.log)의 actual native source windows는 `(1430,803,80,64)`/`(1430,573,80,64)`이며 terminal right1411보다 오른쪽에 실제 source66 pixels를 노출했다. First unbound Sync2/4 time0은 남았지만 source warmup3회는 새 positive public event로 측정됐다. Native paired warm seq5/6는 `475229.700825958`, seq7/8은 `475229.817492083`/`475229.825825417`, seq9/10은 `475229.884158500`이다. 따라서 CAContext는 실제 usable event 데이터를 제공하며 platform unsupported가 아니다. 첫 critical-bound Sync0 ordinal6은8 vblanks 동안 새 eligible event가 없어 `critical-frame-not-newly-measured`로 실패했다. 원본 실패를 보존하고 실제 Sync0 경로를 조사하며 optional visual-control holds는 관찰 도구이지 acceptance 대체가 아니다. Composition Sync0 event·mixed ordinary/HWND native proof는 아직 진행 중이다.

- **Root35 flag/scalar cutover는 실제 warmup FAIL:** 양 ABI build/stage0 뒤 [run-x64-synchronized-sync0-35 stdout](../build/local/composition-cadence-20261005.Bqiyja/run-x64-synchronized-sync0-35/stdout.log)은 `warmup-frame-not-newly-measured`로 Root34보다 이른 첫 warmup에서 실패했다. [Native stderr](../build/local/composition-cadence-20261005.Bqiyja/run-x64-synchronized-sync0-35/stderr.log)는 unbound displaySync1/default YES(이전 Root34는0)과 warm seq5/6 time0을 기록한다. Policy 변경·컴파일만으로 fix/PASS를 선언하지 않으며 이 실제 실패를 유지한다.
- **Root36 actual-bound tearing permission cutover·acceptance FAIL 뒤 사용자 중단:** 현재 [native Source](../src/winemetal/unix/winemetal_unix.c)는 original creation/unbound/offscreen/reset/resize mode를 복원했다. 기존 `publishDeferredWithPeriod`의 `deferred_present && targets.count` guard 안 actual-bound publication의 displaySync만 `(sync != 0)` 대신 validated cached `!allow_tearing`으로 정한다. Sync0는 duration 취소이지 tearing 권한이 아니다. 필수 fifth WMT4 scalar args4의0/1을 [Windows wrapper](../src/winemetal/Metal4FX.hpp) 및 모든 native/Windows 선언·호출 경계로 전달하고 Frame40/Call200 ABI·GPU scheduling은 유지했다. 마지막 runtime36 양 ABI build/install/stage0 뒤 [actual stdout](../build/local/composition-cadence-20261005.Bqiyja/run-x64-bound-nontearing-sync0-36/stdout.log)은 consumer1·첫 warmup ordinal3의9 observations 모두 DISJOINT로 `warmup-frame-not-newly-measured` FAIL이다. [Native stderr](../build/local/composition-cadence-20261005.Bqiyja/run-x64-bound-nontearing-sync0-36/stderr.log)의 first4와 first warmup seq5/6 모두 time0이다. Causal 원인은 미확정이며 actual-bound candidate가 acceptance를 실패한 상태로 사용자 요청에 따라 동결했다. Sync0 해결/Composition PASS를 선언하거나 재probe·rollback하지 않았다.
- **Root34 first-bound optional visual control·mixed fixture 경계:** [run-x64-popup-visual-34 stdout](../build/local/composition-cadence-20261005.Bqiyja/run-x64-popup-visual-34/stdout.log)은 first-bound에서300s hold를 기다렸고 advance 전 만료되어 `visual-control-timeout` FAIL이다. [저장 PNG 경로](../build/local/composition-cadence-20261005.Bqiyja/run-x64-popup-visual-34)는 각 source의 연속2회 fresh native-window surface image에서 blue/public encoded initial ordinal1/2 white fiducial을 보존했다. 이는 scoped CGWindow nativeSurfaceImage이며 actual screen recording/physical scanout/cadence가 아니다. Source x1430–1510의 오른쪽 clip 때문에66 logical pixels만 노출된 사실도 유지한다. [Mixed consumer build receipt](../build/local/composition-mixed-20261005.kbTsD1/build-receipt.json)는 ordinary-first·nonempty FALSE/TRUE의 두 opaque source/두 targets·six phases 빌드만 기록하며 actual GPU/Cua proof는 대기다. Alpha normative colour parity·mixed ordinary/HWND·Composition 완료를 선언하지 않는다.

- **Mixed Root36 x64 실제 partial1/6 phases·원본 public E_NOTIMPL 보존:** [actual-stable-phase-evidence.json](../build/local/composition-mixed-20261005.kbTsD1/run-x64-36/actual-stable-phase-evidence.json)의 ordinary-visible은 fresh PNG 두 장 모두 GREEN20480pixels·geometry matches·PASS다. 이는 ordinary-first 표시 한 phase의 bounded surface-image proof다. Advance 뒤 [public stdout](../build/local/composition-mixed-20261005.kbTsD1/run-x64-36/stdout.log)은 upper.SetOffsetX(18) HRESULT0x80004001/E_NOTIMPL로 target-create-no-commit phase 전 실패했다. Consumer1이고 전체6 중1 phase만 실제 관찰했다. Literal FALSE/TRUE target 생성 API 호출은 성공했지만 nonempty root pixel acceptance는 미실행, WoW64 mixed도 미실행이다. 누락 setter를 fixture workaround로 피하지 않고 원본 실패를 유지한 채 중단했다. Source-only Root33의 기존13 phases 완료와 mixed acceptance를 혼동하지 않는다.

### P2 #13 conservative Tier3 — 2026-10-04

[현재 cap3 계약](CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof)과 [genuine producer/public consumer 재현](SMOKE-TEST.ko.md#conservative-tier3-public-proof)을 따른다. 아래 before와 provisional은 각각 해당 cut의 실제 관찰이며 과거 Tier1 기록을 새 Tier3 proof로 고쳐 쓰지 않는다.

- **Before의 정당한 Tier1 실패:** [before-x64-02](../build/local/conservative-tier3-20261004.uFBr69/before-x64-02/result.json)는 `tier=1`, `vs_dxil-distances-1` triangle48·pixel(14,3)에서 실제 invocation0·coverage0, expected invocation1·coverage1로 실패했다. ClipDistance의 `x+0.1*w` clipping 후 nonzero-area triangle이16.8 snap에서 line이 되며 해당 pixel 내부를 지난다. CullDistance는 primitive 전체를 제거하지 않는다. Tier1은 이 post-snap degenerate를 cull할 수 있고 Tier2/3는 보존해야 하므로 Tier1 기존 계약 위반이나 oracle 오차가 아니다. 기존1/128 ambiguity band는 축소하지 않았다. [stderr22837–22838·isolated redraw22860](../build/local/conservative-tier3-20261004.uFBr69/before-x64-02/stderr.log)는 원래 실패를 보존한다.
- **실제 provisional GPU 의미 proof:** [provisional-x64-01 stdout](../build/local/conservative-tier3-20261004.uFBr69/provisional-x64-01/stdout.log)은 기존 전체 geometry/distance,70 Tier3 family rows·18 early/late interactions·16K precision·degenerate Vertex0/cull/depth·helper·SampleMask를 모두 통과했다. [Result](../build/local/conservative-tier3-20261004.uFBr69/provisional-x64-01/result.json)의 manifest SHA256는 `deaa030f19333705c80e97c73b8770983aeca9721cbaffe523a014488033e254`이며 API+Shader Validation·owned native backend·validation/foreign 오류0이다. Cap1을 유지한 마지막 public Tier3 report 조건만 실패하여 exit1·pass=false다. 전체 runner PASS로 기록하지 않는다.
- **Promotion 후 실제 WoW64 실패→ABI 교정:** GPU 의미 proof 후 `src/d3d12/d3d12_device.cpp:907`을 Tier3로 연결한 runtime02의 x64 conservative는 옛 fixture로 PASS했다. 그러나 [WoW64](../build/local/conservative-tier3-20261004.uFBr69/final-conservative-x86/result.json)는70 family rows·large coordinates 뒤 post-snap degenerate back-face cull에서 실패했고 첫 실패로 batch를 중단하여 당시 x86 회귀는 실행하지 않았다. 원인은 `unix/winemetal_unix.c`의 explicit VERTEX/MESH DATA32 mirrors와 번역 copy에서 `front_counter_clockwise`, PIXEL DATA32에서 `depth_clip_enable`가 누락돼 calloc의 false가 전달된 것이다. 세 mirror/copy를 고치고 PE32 sizeof/offset assertions을 추가했다. Native raster/oracle 오류로 분류하거나 tolerance로 덮지 않았다. AS/MS FrontCounterClockwise cull과 near/far DepthClip ON/OFF4 rows도 public consumer에 추가했다.
- **최종 양 ABI Tier3 완료:** 새 runtime03 [x64](../build/local/conservative-tier3-20261004.uFBr69/runtime03-conservative-x64/result.json)·[WoW64](../build/local/conservative-tier3-20261004.uFBr69/runtime03-conservative-x86/result.json)는 모두 cap3·exit0/pass=true·API+Shader Validation·owned backend·validation/foreign 오류0이다. WoW64 fixture는 machine332/windows_abi=x86·guest_arch=x86이며 실제 PE32 readback이다. 기존70 family rows·18 early/late interaction rows·16K precision·degenerate/helper/SampleMask와 새 AS/MS cull/DepthClip rows가 양 ABI 모두 통과했다. [WoW64 stdout](../build/local/conservative-tier3-20261004.uFBr69/runtime03-conservative-x86/stdout.log)의 DepthClip ON은 near/far 각각 covered28·mandatory10, ClipDistance3e066666·depth00000000/3f800000, OFF는 각각 covered64·mandatory64, ClipDistance3f31999a·depth39000000/3f7ff800으로 exact oracle와 일치했다.
- **회귀8/8의 정확한 source-cut:** Runtime03의 conservative 양 ABI2회와 depth-bounds/pipeline-statistics/multisample-capabilities x86 3회는 manifest `59227040a45afec8a049038264e3ff4dbe39f5133bfc34336811f7406e5e0149`의5회다. 변경 없는 x64 risk3회는 runtime02 manifest `05c99a341000136e37453ae6eb12b5c14ee99e6577534297850374cb77edccc9`의 `final-depth-bounds-x64`, `final-pipeline-statistics-x64`, `final-multisample-x64`를 유지했다. 마지막 수정이 explicit WoW64 copies와 PE32 assertions에 한정되어 x64 risk3회는 재실행하지 않았다. [CORE의8개 named links](CORE-IMPLEMENTATION-STATUS.ko.md#conservative-tier3-현재-계약과-provisional-proof)는 모두 exit0/pass=true·API+Shader Validation·validation/foreign 오류0이다. **5개 runtime03+3개 retained runtime02의 mixed-cut 완료**이며 같은 build8회 실행이 아니다. Runtime02 x64 conservative PASS는 새 consumer edges 전의 역사 기록으로 위8개에는 넣지 않았다. SM6.5/FL12_1·다른 GPU/OS·실게임/FPS·ARM64/FEX 미검증 경계는 유지한다. 최종 [diagnostics-mirrors90 PASS](../build/local/conservative-tier3-20261004.uFBr69/diagnostics-mirrors.log)도 별도 CPU 증거다.
- **Genuine compiler 입력:** [Provenance](tests/dxil/conservative-raster-provenance.json)는 DXC1.9.2609.5의 opcode92 `dx.op.innerCoverage`와 native FXC10.1 ps_5_1의 `vInnerCoverage`/SFI를 확인한다. 두 실제 producer는 mutually exclusive Coverage+InnerCoverage 입력을 거부했다. Fake builtin/container나 OFF/nontriangle InnerCoverage=0 계약은 사용하지 않는다. 기존 conservative-distance HLSL/DXIL은 재생성하지 않은 retained 입력이다.
- **D3D11 경계·CPU proof:** final [x64](../build/local/conservative-tier3-20261004.uFBr69/d3d11-final-x86_64.stdout.log)·[i686](../build/local/conservative-tier3-20261004.uFBr69/d3d11-final-i686.stdout.log)는 ordinary PS S_OK/non-null·InnerCoverage PS E_FAIL/null이다. [Before stderr](../build/local/conservative-tier3-20261004.uFBr69/d3d11-before-x86_64.stderr.log)는 unhandled source operand assert이고 buffered stdout 때문에 exact triggering shader·prior HRESULT는 미관찰이다. 이전 E_FAIL 보존이라는 판정은 하지 않는다. [CPU diagnostics90 PASS](../build/local/conservative-tier3-20261004.uFBr69/diagnostics.log)는 GPU acceptance와 별개다.
- **Pristine GPTK 정적 reference:** 새 [identity SHA256](../build/local/conservative-tier3-20261004.uFBr69/gptk-reference.sha256)는 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`다. [CheckFeatureSupport0x6dc28/case0x6df68](../build/local/conservative-tier3-20261004.uFBr69/gptk-options.disasm)는 [0x374be0 constant의 첫 word0](../build/local/conservative-tier3-20261004.uFBr69/gptk-const.txt)을 읽어 Tier0를 보고한다. 정적 reference일 뿐 owned Tier3 실행 proof·GPTK runtime 실행·구현 의존성은 아니다.

### P2 #12 depth bounds — 2026-10-04

최신 사용자 선택인 **DX12 게임 기능 먼저, 최적화는 그다음**에 따라 depth bounds를 구현했다. [현재 계약·양 ABI50-case proof](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)와 [shader/fixture 재현](SMOKE-TEST.ko.md#depth-bounds-50-case-focused-proof)을 따른다. AIR/compiler·software stencil masking·statistics emulator를 추가하지 않고 regular/Metal4 native `setDepthTestMinBound:maxBound:`와 AppleGPUFamily10(1010)의 OPTIONS2 gate를 연결했다. SM6.5/FL12_1은 그대로이며 다음 기능은 conservative Tier3다.

- **Before:** 기존 runtime-final-02의 [x64](../build/local/depth-bounds-public-20261004/before-x64-01/result.json)·[WoW64](../build/local/depth-bounds-public-20261004/before-x86-01/result.json)는 cap0·enabled PSO `E_INVALIDARG(80070057)`로 GPU case 제출 전에 실패했다.
- **Native 경계 조사:** [regular encoder probe](../build/local/depth-bounds-native-20261004-01/result.json)는56 runs 중52 D3D matches·4 expected noDSV mismatch였다. Native nondefault bounds는 noDSV에서 reject하지만 D3D는 pass해야 하므로 translator 경계에서0/1로 복원했다. All56 command Completed·error=nil이지만 probe exit1이며 전체 PASS/Metal4 실행/독립 hardware PSInvocation proof로 세지 않는다.
- **정적 reference:** pristine GPTK4.0b2 [identity SHA256](../build/local/p2-depth-bounds-20261004.S3Pmiu/gptk-identity.sha256)는 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`다. [SupportDepthBounds cold.1@0x36fa81 disassembly](../build/local/p2-depth-bounds-20261004.S3Pmiu/gptk-depth-bounds-gate.disasm)의0x36faa5는1010을 전달하고 [selector refs](../build/local/p2-depth-bounds-20261004.S3Pmiu/gptk-selector-refs.txt)의0x4c6660→0x467660은 [metadata](../build/local/p2-depth-bounds-20261004.S3Pmiu/gptk-objc-metadata.txt)의 `supportsFamily:`다. Native gate의 정적 참조일 뿐 GPTK runtime proof·구현 의존성·proprietary copy가 아니다.
- **Provisional 실패→fixture 교정:** [x64-01](../build/local/p2-depth-bounds-20261004.S3Pmiu/provisional-x64-01/result.json)은 D16 `.25` 입력의 EQUAL probe, [x64-02](../build/local/p2-depth-bounds-20261004.S3Pmiu/provisional-x64-02/result.json)는 raw expected16384/actual16383로 실패했다. Input을 float `0x3333/65535`로 정하고 raw `0x3333`·EQUAL probe를 exact 비교하도록 고쳤으며 tolerance는 넣지 않았다. [x64-03](../build/local/p2-depth-bounds-20261004.S3Pmiu/provisional-x64-03/result.json)은50 GPU cases를 통과했지만 cap0 때문에 exit1·runner FAIL였다. 최종 native gate를 연결한 runtime-02와 구분한다.
- **Final public cut:** paired runtime-02 manifest SHA256 `19ce6cf8aaad8e1e9c68a9d23016df6d41d6845222cb4fd5d444297597a18afa`의 [x64](../build/local/p2-depth-bounds-20261004.S3Pmiu/final-depth-bounds-x64/result.json)·[WoW64](../build/local/p2-depth-bounds-20261004.S3Pmiu/final-depth-bounds-x86/result.json)는 ABI별 cap1·50 CASE_PASS·replay 포함51 STATS·exit0·API+Shader Validation·owned compiler/backend·validation/foreign 오류0으로 PASS했다. 같은 cut의 pipeline-library-gated·pipeline-statistics 양 ABI4회와 아래 multisample2회까지 총8 GPU runs PASS, diagnostics89/89다([CORE 결과 링크](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)).
- **Multisample stale fixture 실패→수정:** 원래 [final-multisample-x64](../build/local/p2-depth-bounds-20261004.S3Pmiu/final-multisample-x64/result.json)는 앞선 GPU resolve paths 통과 뒤 `MSAA UAV resource passed null-output preflight`라는 rejection assertion에서 실패했다. Common fixture의 obsolete RGBA8 4×UAV rejection 두 줄만 제거했으며 기존 OPTIONS14 writable-MSAA TRUE와 전용80-case positive consumer는 유지했다. Runtime/runner를 변경하지 않은 [final-multisample-fixed x64](../build/local/p2-depth-bounds-20261004.S3Pmiu/final-multisample-fixed-x64/result.json)·[WoW64](../build/local/p2-depth-bounds-20261004.S3Pmiu/final-multisample-fixed-x86/result.json)는 exit0·API+Shader Validation·validation/foreign 오류0으로 PASS했다. 이를 depth-bounds 제품 결함이나 새 writable-MSAA 구현으로 기록하지 않는다.
- **Statistics 범위:** [DepthBounds v1.3](https://microsoft.github.io/DirectX-Specs/d3d/DepthBoundsTest.html)의 exact statistics test는 ForceEarly=true이며 [D3D11.3 §20.4.7](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#PipelineStatistics)은 oDepth 실패를 포함한 실제 shader invocation을 센다. Early count는 exact 검증, late PS count는 로그 관찰이며 post-bounds survivor assertion으로 바꾸지 않았다. Pixels/UAV/depth/stencil/occlusion/binary는 hard assertion이다. Windows/HLK·differential conformance·실게임/FPS·다른 GPU/OS proof는 없다. 이전 Apple family emulation 미구현은 hardware 한계이지 게임 무관성 판정이 아니다.
- **정리:** Main이 runner 종료·wineserver wait를 확인한 owned13 prefixes와 구 runtime-01 bin/lib/share만 정리했고 manifest·logs/results를 보존했다([cleanup](../build/local/p2-depth-bounds-20261004.S3Pmiu/cleanup.log)). 현재 runtime-02와 known-good auto-cache runtime-final-02는 유지한다. Native probe의 throwaway executable·probe.mm·commands.sh는 제거하고 frozen probe.metal·hashes/log/result는 보존했다.

### P2 #12 이전 Apple family depth bounds emulation — 2026-10-05

위2026-10-04의 Apple10 native-only50-case cut과 당시 emulation 미구현 관찰은 그대로 보존한다. 후속 root 구현은 OPTIONS2를 Apple7+에 연결하고 Apple10 native fast path를 유지하며, Apple7/8/9에서는 **live stored depth의 sample별 bounds 판정**을 기존 per-primitive AIR capture/replay로 합친다. R8Uint coverage의 정확한0/1과 buffer의 primitive payload를 분리하여 이전 family가 지원하지 않는 R32Uint MSAA를 전제하지 않는다. 이전 primitive의 depth write가 완료된 뒤 별도 fenced compute pass가 현재 layer/mip DSV를 읽으며 attachment feedback·draw-wide snapshot·새 public SRV·software stencil/statistics emulator는 넣지 않았다.

- **Early stencil 실패→실제 fragment kill:** runtime06 [forced-x64-01](../build/local/depth-bounds-emulation-20261004/forced-x64-01/result.json)은 앞9 cases 뒤 inverted_early_uav의 depth/stencil probe에서 actual0/expected4278255360으로 실패했다. Coverage0을 반환하는 것만으로 explicit-depth pass의 stencil commit을 막지 못했으므로 shared MSL에서 mask0을 depth/marker 반환 전에 discard했다. Runtime07 [forced-x64-02](../build/local/depth-bounds-emulation-20261004/forced-x64-02/result.json)의50 cases가 통과했다. Late PS/UAV는 bounds reject에도 본래 invocation/side effect를 보존하고 native outputs만 mask한다.
- **WoW64 실제 semantic 실패→ABI cutover:** runtime07 [forced-x86-02](../build/local/depth-bounds-emulation-20261004/forced-x86-02/result.json)는 첫 case 뒤 incoming_inside_stored_outside의 color actual4278190335/expected0으로 실패했다. Unix DATA32 mirror의 새 emulation byte53과 copy가 빠져 compiler flag가 FALSE였던 원인을 수정했다. Canonical PE32/PE64·WoW mirror와 pointer-free tile/layout/probe assertions를 동기화했다. Runtime08 [forced-x86-03](../build/local/depth-bounds-emulation-20261004/forced-x86-03/result.json)의50 cases와 forced pixel 양 ABI32 cases가 통과했다.
- **Producer validation 실패→descriptor 소유권 수정:** runtime08의 [x64 stderr](../build/local/depth-bounds-emulation-20261004/route-probes/forced-producers-x64-01/stderr.log)·[WoW64 stderr](../build/local/depth-bounds-emulation-20261004/route-probes/forced-producers-x86-01/stderr.log)는 앞4 VS1×/4× cases 뒤 visibilityResultBuffer length0/offset0의 Metal4 encoder assertion으로 exit232였다. Recorded query state를 replay하는 attachmentless object capture와 mesh/GS/HS/DS producer descriptor에 retained visibility scratch를 전달했다. Query를 끄거나 assertion을 숨기지 않았고 최종 raster pass의 count fanout은 그대로다. Runtime09 forced producer 양 ABI36 cases가 통과했다.
- **Coverage fixture 실패→규격상 Alignment0:** 기존 [x64](../build/local/depth-bounds-emulation-20261004/route-probes/forced-coverage-x64-01/stdout.log)·[WoW64](../build/local/depth-bounds-emulation-20261004/route-probes/forced-coverage-x86-01/stdout.log)는 color resolve용 texture 생성에서 E_INVALIDARG였다. 4× resource GetDesc의 MSAA placement alignment를1× texture에 복사한 fixture만 Alignment0으로 교정했다([receipt](../build/local/depth-bounds-emulation-20261004/route-probes/coverage-alignment02-receipt.json)). 원래 실행·binaries·logs를 보존했고 input shader·resolve format/mode·300 covered/210 inner·UAV/color/depth/stencil/occlusion의 exact oracle·forced+DSV 거부는 완화하지 않았다. Runtime09의 revised forced coverage는 양 ABI28 cases PASS다.
- **Final proof와 cut 경계:** M5 Pro/macOS27에서 forced/native 각각 양 ABI **146 cases/ABI=기본50+producer36+pixel32+coverage28**가 통과했다. Forced 기본 x64는 runtime07, WoW64/pixel은 runtime08, producer/coverage-alignment02는 runtime09의 retained 완료 cut이다. 임시 forced assignment를 원래 Apple10 gate로 복원한 runtime10의 manifest SHA256은 **392ec11687f2578b8a1f56ea541f68bcceb4a6c3a80bd22d83fa8879a798cf6b**이며 [native x64 기본50](../build/local/depth-bounds-emulation-20261004/native-x64-01/result.json)·[native WoW64 기본50](../build/local/depth-bounds-emulation-20261004/native-x86-01/result.json)와 세 route 양 ABI36/32/28을 같은 cut에서 통과했다. 모든 기본 runner는 pass=true·exit0·owned compiler/backend·validation/foreign 오류0이다. Route는 각 stdout의 exact36/32/28 PASS terminal·exit-code.txt0·full stderr API+Shader Validation 오류0을 확인했다([전체 forced/native 결과 표](CORE-IMPLEMENTATION-STATUS.ko.md#depth-bounds-완료와-focused-proof)). 임시 force는 permanent option이 아니며 production native fast path는 유지한다.

이 결과는 **Apple M5 Pro에서 이전 family 경로를 강제한 semantic proof**다. Apple7/8/9의 실제 물리 장치 실행·다른 GPU/OS·Windows/HLK·일반 conformance·실게임/FPS proof는 없으며 SM6.5/FL12_1을 올리지 않았다. 후속 Shared AIR 통합 회귀는 지정146 cases와 별도로 아래 **12 native GPU runs**를 완료했다. 모두 양 ABI runner pass=true·exit0·API+Shader Validation 오류0·foreign 오류0이며 두 runtime의 mixed-cut 증거다.

- **Shared AIR 영향 회귀 완료:** native runtime11(manifest SHA256 **131b5fff417de42ca7ad4a7f64d9fb6a1fddfa4d3ccaf65c599a6505cd9ce7c1**)에서 아래 첫5 suites를 양 ABI10회 실행했다. Geometry는 DXBC+DXIL87 cases, tessellation은 DXBC+DXIL166 cases, atomic64-producers는 genuine DXIL18 cases·native compiler 확인이다. Multisample은 기존 full cases, conservative는 전체 cap3 proof를 그대로 사용했다. Native runtime12(manifest SHA256 **252fa37d96a55376d95426f8bc52aec28bd922d3d2f84e77d966eb7f56425fea**)의 full current mesh는 양 ABI43 ordered cases·native compiler 확인으로 통과했다. [새 current producer receipt](../build/local/depth-bounds-emulation-20261004/shared-mesh-fixture-01/producer-receipt.json)는 같은 current HLSL/consumer snapshot의53 DXIL inputs이며 과거 revision과 섞거나 groupshared-only subset으로 줄이지 않았다. Runtime11/12의 D3D12/AIR/MSAA emulation은 depth proof의 runtime10과 동일하고 native composition.so 변경은 ABI를 유지했다.

| Shared AIR regression | Native cut | x64 result | WoW64 result |
|---|---|---|---|
| Multisample full | runtime11 | [x64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/multisample-x64/result.json) | [WoW64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/multisample-x86/result.json) |
| Geometry87 DXBC+DXIL | runtime11 | [x64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/geometry-x64/result.json) | [WoW64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/geometry-x86/result.json) |
| Tessellation166 DXBC+DXIL | runtime11 | [x64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/tessellation-x64/result.json) | [WoW64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/tessellation-x86/result.json) |
| Atomic64 producer18 genuine DXIL | runtime11 | [x64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/atomic64-producers-x64/result.json) | [WoW64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/atomic64-producers-x86/result.json) |
| Conservative full cap3 | runtime11 | [x64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/conservative-x64/result.json) | [WoW64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/conservative-x86/result.json) |
| Mesh full current43 | runtime12 | [x64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/mesh-shader-x64/result.json) | [WoW64](../build/local/depth-bounds-emulation-20261004/shared-air-regressions-01/mesh-shader-x86/result.json) |

### 게임 공통 자동 PSO 컴파일 캐시 — 2026-10-04

사용자가 선택한 일반 PSO 생성의 자동 불변 결과 재사용·disk persistence·same-key single-flight를 기존 owned v5 blob 위에 구현했다. 현재 계약·한도·scope는 [CORE](CORE-IMPLEMENTATION-STATUS.ko.md#자동-pso-컴파일-캐시와-focused-proof), 운영 compiler/link 지침은 [SMOKE](SMOKE-TEST.ko.md#fixture-runner-suite와-컴파일-차이)를 따른다. 기존 PipelineLibrary/ShaderCacheSession을 대체하거나 native archive·최종 PSO object cache를 추가하지 않았다.

- **Final clean cut:** paired x64/WoW64 full build/install·fresh stage 성공. runtime-final-02 manifest SHA256 **61883069c1ad7dbc3d838a489e583470de7108b887f223cb3cec4a8c0b7fee03**, evidence root는 build/local/auto-pso-cache-20261004.2FsF2M/이다. cache-final02-{x64,x86}은 각각 API+Shader Validation·27 exact GPU values·7 explicit rejections·5 disk child recoveries·same-key concurrency/failure/control PASS다. session-final02-{x64,x86}은 기존 app-session CPU API 회귀 PASS, runner diagnostics는 **88/88**이다. Validation/foreign backend 오류는 없었다.
- **별도 결정적 race cut:** runtime-gated-01 manifest **31ad84a55248a18941af845796270fefc80a2fb24c45b84d2e6bba1005fbc370**의 임시 owner/waiter gates로 양 ABI에서 producer1·실제 등록 waiters7을 확보했다. race-gated-{x64,x86}은 pending 상태의 DISABLE|CLEAR 뒤8개의 S_OK/non-null·GPU31415a5b와 재-enable 뒤 같은 값을 확인했다. Invalid input은8개 모두 E_FAIL/null, 후속 동일 요청도 새 시도 후 E_FAIL였다. INFO trace를 끈 run이므로 retired_complete trace가 있었다고 주장하지 않는다. Gates는 제거했고 final clean cut에는 없다.
- **기존 graphics/library 회귀:** library-gated-{x64,x86}은 gates를 활성화하지 않은 위 별도 cut에서 API+Shader Validation·9 cases/13 numeric GPU rows·8 threads×24 rounds·child restore PASS였다. Final02의 동일 실행으로 고쳐 쓰지 않는다.
- **경계:** memory-entry-limit-x64는513 distinct roots 뒤 latest memory hit/first disk restore·2 GPU outputs로512-entry eviction을 확인했다. Aggregate64MiB RAM은 실제 채우지 않았다. cache-file-verification-{x64,x86}은 sparse 2^32+1-byte 손상 파일을153608-byte 정상 blob으로 복구했다.4개의 invalid64MiB quota fixtures와 abandoned temp를 넣은 뒤 oldest/temp가 제거됐고 published total201480200 bytes였다.4 concurrent processes의 생성과 다음 reader disk restore/public blob SHA 일치를 확인했지만 inter-process single-flight나 allocation/virtual-memory instrumentation의 증거는 아니다.
- **측정:** baseline runtime은 build/local/pso-cost-20261004.hy2vbt/runtime-final-01, manifest **afb6c7f854d397888590723d480d242be8692b6aef67ec331908a3ebf2202ea1**이다.57,396-byte frozen ZZZ compute DXIL SHA256 **dcf8e129460cabddeef7e1631ab1cc923af1acefd219b5490c5ebe0ad5095e1e**와 원본 embedded root를 그대로 사용했다. 같은 executable의 ordinary CreateComputePipelineState6회에서 후속5회 중앙값은 x64 220.1029→2.8069ms, WoW64 217.3839→2.8762ms. 자동 캐시 없는 새 prefix의 첫 생성243.0106/237.7684ms 뒤, 같은 prefix의 새 process 첫 생성10.7170/10.7716ms는 disk hit1·memory hit5·compile0이었다. captured-analysis.json에 각 표본·runtime hashes·module audit를 보존한다. Validation off, before ERROR/after INFO logging 차이, global Metal cache 미초기화이며 creation-only라 GPU correctness/FPS를 대신하지 않는다.
- **실패 보존:** cache-gated-x64의 최초 native-fault 입력은 AIR bitcode4 bytes를 바꿨지만 native 경로가 그대로 성공해 복구를 입증하지 못했다. 캐시 실패로 위장하지 않고 fixture를 MTLB VersionMajor2→65535+유효 PSO digest로 바꿨다. 최종 child는 disk hit→native rejection→fresh retry/publication→exact GPU를 확인했다. cache-final-x64는 consumer exit0이지만 runner가 실제 ENABLE|CLEAR=6을 잘못된5와 비교해 FAIL이었다. Hardcoded call sequence를 제거하고 유효한 성공 제어 coverage와 실제 key별 bypass/hit/rebuild를 검사하도록 고쳤으며 두 원본 실패를 보존했다. 메모리 경계 분석의 stderr mirror/원본 log 중복 집계도 원본 log만 사용해 고쳤고 GPU를 재실행하지 않았다.

실제 게임/live prefix·ARM64/FEX·다른 GPU/OS·native Windows differential·장시간 FPS/전력은 실행하지 않았다. 이 증거는 선택한 owned primary-stage cache 범위이며 모든 lazy shader variant의 캐시나 native pipeline archive 완료가 아니다.

[정리 기록](../build/local/auto-pso-cache-20261004.2FsF2M/cleanup.log): 이 작업의15 disposable prefix는 각 Wine server 종료·대기 후 제거했다. Gated/final01 runtime payload2개와 throwaway probe는 정리하고 raw logs/results·manifests/provenance·frozen 입력을 보존했다. Current final02와 측정에 쓴 pre-change comparison runtime은 남겼으며 다른 작업의 runtime/prefix는 건드리지 않았다.

### 게임 공통 Agility inbox 우선 정책 — 2026-10-04

게임 공통 경로부터 순차적으로 닫는 첫 항목이다. 아래 기존 owned Core618 loader의 지정 proof에 **일반 EXE/base SetSDKVersion의 설치 runtime 우선 정책**을 추가했다. SDK1의 명시적 SxS 선택과 global redist 선택을 혼동하지 않는다.

- **공개 계약:** [Redistributable](https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html)은 요청 version이 inbox 이하이면 설치된 Core를 사용하도록 한다. `SelectGlobalCoreLocked`는 EXE exports와 base Set이 공유하며 검증된 실제 bridge version으로 비교한다. SDK617/618은 installed618, 더 높은 요청은 엄격한 requested-package 선택이다. SDK1 factory와 Apply는 기존 독립 계약을 유지한다.
- **실패→수정:** `build/local/common-games-20261004.yr1aZt/agility-x64`의 alternate Core 실패는 builtin basename redirection이었다. Core만 normal PE + native-only로 배포했다. `agility-fixed-x64`의 ENOENT는 Main의 prefix facade를 실제 설치 경로로 취급한 오류였다. 비실행 data mapping의 `NtAreMappedFilesTheSame`으로 backing identity를 확인하도록 바꿨다. `agility-current-x86`의 ENOENT는 host WINEDLLPATH에 암묵적인 WoW64 sibling 탐색이 빠진 오류였으며, i386 sibling에도 같은 identity 검사를 적용했다. 원본 FAIL JSON은 보존했다.
- **일반 게임 regression:** immutable runtime-02에서 SDK617 exports + `.\missing-sdk\`의 `D3D12CreateDevice`는 `0x887a0004`/null/exit1이었다. 같은 EXE를 새 runtime-03에서 실행하면 `S_OK`/nonnull/exit0이다. 같은 evidence root의 `inbox-before.stdout.log`, `inbox-after.stdout.log`와 loader stderr에 원본 결과를 보존했다.
- **최종 같은 cut의 실제 실행:** fresh paired runtime-03 manifest SHA256 **`d2959ddd33de293628abb52572edb52098ad340be332e43b78517e842f1ca3b1`**. [Agility x64](../build/local/common-games-20261004.yr1aZt/agility-final-x64/result.json)·[WoW64](../build/local/common-games-20261004.yr1aZt/agility-final-x86/result.json), [기본 factory x64](../build/local/common-games-20261004.yr1aZt/diagnostics-final-x64/result.json)·[WoW64](../build/local/common-games-20261004.yr1aZt/diagnostics-final-x86/result.json) **4/4 PASS**, exit0, Metal API+Shader Validation, validation/foreign-backend 오류0이다. GPU proof는 copy/readback이며 DXIL compiler 실행 proof로 확대하지 않는다.
- **Consumer 경계:** ABI별 Agility 6 case·6 module rejection·10 exact readback(`a6180001`–`a618000a`)·32 cold concurrent factory/configuration/FreeUnusedSDKs와 최종 unload. SDK617/618 EXE는 missing/invalid app package 대신 설치본의 실제 path·generation·ABI·hash를 사용한다. SDK619 EXE에서 Set617/618이 exports를 override하며, 뒤의 Set619 실패가 기존 선택을 바꾸지 않는다. 일반 모드 base Set 거부·SDK1 허용, late Set의 global device 제거와 independent device 생존, alternate Apply의 `0x887e0003`도 보존했다.
- **비GPU 검사:** 양 ABI 전체 build/install과 8개 immutable Agility EXE compile 성공. `scripts/test-owned-graphics-diagnostics.py` **87/87 PASS**, native-only/legacy-read-only staging 회귀 **36/36 PASS**는 별도 CPU 검사이며 GPU 성공 수에 합산하지 않는다. 이전 `agility-backing-x64`는 consumer exit0이었지만 낡은 EXE에 새 acceptance marker가 없어 runner FAIL이었던 결과 그대로 남겼다.

완료는 owned618 explicit SDK package와 일반 global inbox 우선 정책의 지정 consumer 범위다. Microsoft SDK618 전체 기능, 임의/foreign Core 실행, Godot/Unreal 등 실게임 전체 실행이나 다른 GPU/OS·ARM64/FEX는 증명하지 않았다. 실행이 끝난 이 작업의 prefix와 runtime-01/02 payload는 정리하고 manifests·원본 logs/results·frozen fixture를 남겼다. 최신 runtime-03은 보존한다. 재현 절차는 [SMOKE](SMOKE-TEST.ko.md#owned-core618-versionpath수명-acceptance)를 따른다.

### 새 P4 게임 경로 통합 — 2026-10-04

증거는 `build/local/p4-resume-20261004-WoVvSo/`에 보존한다. Paired runtime02 manifest SHA256은 `acebd5fb635b9dc2b895561aa696415743e2f6f17507e900322eb0f6f8e25b3d`다. x64/WoW64 build·install·fresh stage를 통과했다. 기존 게임/live prefix와 ARM64/FEX guest 실행은 사용하지 않았다.

- **OPTIONS13/fan:** 초기 `direct_triangle` actual2/expected5는 fixture가 버퍼 fetch 주소를 SV_VertexID로 사용한 오류였다. D3D11.3 §8.16 및 D3D9 fan 순서에 따라 shader ID와 fetch 주소를 분리했다. COPY immediate fixture는 COMMON으로 생성하고 closed list 끝에서 COMMON으로 복원했다. Production queue 검증을 완화하지 않았다.
- **PSO 실제 결함:** `ExtractMTLInputLayoutElements`가 E_INVALIDARG를 반환할 때 미정인 `num_elements`를 `elements.resize`가 먼저 읽었다. WoW64 진단 실행은 `incompatible_pso` 뒤 `exception=vector`를 확인했다. HRESULT 검사 뒤에만 성공 출력을 사용하도록 수정했다. [x64](../build/local/p4-resume-20261004-WoVvSo/options13-runtime02-x64/result.json)·[WoW64](../build/local/p4-resume-20261004-WoVvSo/options13-runtime02-x86/result.json)는 9/9 case, 63 copy/6 차원 간 copy replay·24 bounds 거부·18 fan row/1152 pixel/66 primitive·3 queue immediate snapshot/replay와 공개 cap을 모두 통과했다. OPTIONS13 6개와 TriangleFanSupported는 TRUE, 도구 전용 manual-write-tracking은 FALSE다.
- **Reflex:** Fixture GetSleepStatus ID를 공식 `0xaef96ca1`로 교정했다. 기존 staged runtime01의 [x64](../build/local/p4-resume-20261004-WoVvSo/reflex-x64-02/result.json)·[WoW64](../build/local/p4-resume-20261004-WoVvSo/reflex-x86-02/result.json)는 8 case·90 frame·64 report와 실제 Sleep 31186/31255µs를 통과했다. GPU active-cycle 미측정값은 0으로 유지한다.
- **표시·adapter·priority:** Runtime01의 `fullscreen-{x64,x86}-01`은 D3D12 3888 pixel readback·native vblank 3회·output statistics·resize disjoint·fullscreen 복원을 통과했다. `dxgi-factory-{x64,x86}-01`은 동일 실제 LUID의 3 preference·node/occlusion/stereo/event 수명을, `queue-priority-{x64,x86}-01`은 허용된 S_FALSE 미적용·creation getter·4-thread 동시 호출·native copy를 통과했다. 단일 M5 Pro 결과이며 multi-GPU 순서·hotplug·실제 scheduler priority 적용 증거가 아니다.
- **CoreWindow/Composition 선행 조건:** `corewindow-{x64,x86}-01`은 실제 WinRT가 S_OK/null CoreWindow를, DCompositionCreateDevice가 E_NOTIMPL을 반환했다. Reachable HWND/오류/identity 계약은 통과했지만 `pass=false`, `REACHABLE_OK`, `feature_complete=false`로 기록했다. 외부 Wine 저장소의 실제 객체·visual/target 구현 승인 없이 fake window/분리 swapchain을 만들지 않았다.

위 named GPU 결과는 Metal validation·owned module 검증·foreign backend 오류0 범위다. 개별 결과를 NGX RR·HDR10+ GAMING·Agility loader 또는 실게임 전체 완료로 확대하지 않는다.

**후속 runtime03:** manifest SHA256 `16d36f346752c6331088d9c5384d1d2465ce3d48b0dc433a1b4aa0f2e30b2e62`. Public FSR SR은 runtime02 양 ABI에서, public NGX SR은 runtime02 x64와 runtime03 WoW64에서 GPU/validation을 통과했다. WoW64 NGX의 `LoadLibraryW` 실패 `0x8007045a`는 CRT/parameter ABI 문제가 아니라 `InitializeVendorExtensionNV`의 낡은 `__i386__` 조기 반환이었다. 이 3줄만 제거해 기존 env/registry opt-in과 DllMain의 private DXGI handshake를 양 ABI에 적용했다.

Installed-runtime `CLSID_D3D12DeviceFactory`는 실제 device 생성·factory-local Debug/DRED/experimental 설정 snapshot·기존 독립 device의 global 변경 격리를 구현했다. `agility-runtime03-{x64,x86}/result.json`은 6/6 case와 GPU copy/readback을 통과했다. 이 증거는 nonzero SDK version/path 선택이나 Godot의 SDKConfiguration1 호출 성공을 뜻하지 않는다.

**HDR10+ GAMING readonly native 조사:** [contract-findings.json](../build/local/hdr-gaming-20261004-native-readonly/contract-findings.json)과 [원본 native-query.log](../build/local/hdr-gaming-20261004-native-readonly/native-query.log)·[실제 EDID 재파싱](../build/local/hdr-gaming-20261004-native-readonly/recovered-edid-check.log)을 현재 판단 근거로 삼는다. IOAVServiceCopyEDID의 default external service와 외부 DCPAVServiceProxy 두 경로에서 동일한 genuine256-byte EDID를 얻었다. [SHA256](../build/local/hdr-gaming-20261004-native-readonly/edid-sha256.log)는 `112933369a46b8604e3289d66575ebb7ae3ea043060ec92f6d565040bef288aa`이며 Samsung S32B80P(CGDisplayID3, vendor SAM/product29171, serial810300994)와 일치한다. Header·extension count·128-byte checksum·CTA 경계를 통과했다. **현재 연결의 광고는 PQ·정적 HDR(EOTF bits05, descriptor01), HDR10+ VSVDB OUI0x90848B 없음**이다. 이 결과를 해당 모델의 모든 firmware/input mode에 대한 미지원 선언으로 확대하지 않는다.

외부 native preset count는0(active -1), calibration은 null이다. 내부 패널은51 slots 중11개 유효 preset을 읽었고, [slot2 HDR Video(P3-ST2084)](../build/local/hdr-gaming-20261004-native-readonly/display-1-preset-2.plist)는1000-nit HDR/100-nit SDR·host HDR tone mapping off를 명시한다. 이는 실제 video reference profile이지만 Gaming reference mode·검증된 저지연이 아니다. `PresetSupportedLatencyModes=[0]`에 Gaming 의미를 부여하지 않으며 [내부 calibration](../build/local/hdr-gaming-20261004-native-readonly/calibration-summary.json)의 사용자 휘도/white-point 조정 범위를 측정된 Gaming peak/full-frame 한계로 사용하지 않는다.

Pristine GPTK4.0b2 SHA256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`의 **정적 API 관찰**도 위 계약 보고서에 보존했다. SetOutputMode1(HDR10)/2(Gaming)는 모두 PQ color-space12이고 GetOutputMode는 PQ를1로 보고한다. Nonzero display에서 SetHdrToneMapping은 GPU1에-1, 다른 method에0을 반환하지만 native 호출·상태 변경은 없으며 Get은 APP0이다. SetSourceHdrMetadata는 유효 인수에-6이다. 이러한 no-op/오류와 PQ layer tagging은 물리 Gaming 협상·tone-map 구현 증거가 아니므로 복제하지 않는다.

**미해결 완료 조건:** macOS/DCP/IOAV의 실제 Gaming reference-mode·저지연 협상/readback 계약, 규범적 transport/control·calibration·application-version 의미·peak/full-frame-index table·GPU SSTM/EETF 정의와 입력/conformance vectors가 필요하다. APP/GAME이 이미 SSTM한 출력은 owned passthrough에서 EETF를 다시 적용하지 않아야 하며 GPU 선택은 실제 calibrated sink 한계를 쓰는 별도 규범적 tone map이어야 한다. Gaming capability와 reference/latency/calibration readback을 제공하는 acceptance sink, 승인된 물리 mode 전환, 실제 native override의 단일 소유권·경합 거부·이전 OS mode 반환 및 release/teardown/hotplug 시 복원, 광학 휘도/색·지연 proof가 남아 있다. Local bookkeeping은 native override/복원을 대신하지 않는다. **DXGI HDR10PLUS72-byte opaque sink metadata는 Gaming SSTM과 별도 계약**이다.

보고서의 실행 범위는 readonly native query와 실제 EDID 회수, native C11 probe build, parser9개 및 회수 EDID 재파싱뿐이다. Production/runtime 구현·설치, GPU/rendering, UI/settings·mode override, 물리 전환/복원·광학 측정, Windows 양 ABI public 실행은 하지 않았다. HDR10+ GAMING은 관련 게임 기능의 **미완료**로 유지하며 완료·제외·불필요로 분류하지 않는다.


**RR native 통합:** Metal4 compute fence의 허용 stage mask·producer barrier를 고치고, frontend release가 기록에 보존된 history를 초기화하던 `Dormant` 결함을 제거했다. 독립 oracle도 기존 SR 수명 계약에 맞춰 frame1–8 뒤 frame8을 reset 없이 재실행한다. `ngx-features-runtime03-x64`와 `ngx-features-runtime04-verify-x86`은 각각 public output36장의 finite RGB가 독립 x86_64 native MetalFX와 오차0이고 producer input16세트의 SHA256도 일치한다. 하지만 양쪽 결과는 **FAIL**이다.

`rr-native-x86-validated`는 32 frame 및18 replay에서 내부 `Conv3x3Stride1U8` buffer 길이12672 바깥 읽기를 각각32/18회 보고했다. `rr-native-arm64-validated`도 같은32회 오류다. 후자는 실제128×128·scale1/지원범위1–3·private texture9개·format/usage/device 일치를 확인했다. API/Shader Validation 환경변수4개를 명시한 독립 실행이므로 Wine bridge만이나 Rosetta만의 오류가 아니다. Public RR와 native oracle 오류를 모두 result.json에 보존하고 epsilon 완화·로그 억제로 PASS를 만들지 않는다. 이전 native probe의 validation 환경 미기록은 clean acceptance 증거가 아니다.

**후속 classic/M4 factory 대조와 실제 native 실행:** Apple M5 Pro/macOS27.0.1(26A434), registryID4294969136에서 native classic supportsDevice=1 및 Metal4FX 지원 query 성공을 확인했다. 모든 control은128×128·scale1(지원범위1–3), format/usage가 일치하는 동일 device의 private texture9개를 사용했다. API/Shader Validation과 오류 보고 환경변수4개를 명시했으며 아래 결과는 production RR cutover가 아닌 독립 owned native control이다.

| 실제 실행한 native control | 보존한 정확한 보고서 | Clean validation 결과 |
|---|---|---|
| Classic, requiresSynchronousInitialization=YES, arm64 | [rr-classic-20261004.uLqdtY/validation-verdict.txt](../build/local/rr-classic-20261004.uLqdtY/validation-verdict.txt) | Baseline32/replay18회 위반, FAIL |
| Classic YES, x86_64 | [동일 cut validation-x86_64-verdict.txt](../build/local/rr-classic-20261004.uLqdtY/validation-x86_64-verdict.txt) | Baseline32/replay18회 위반, FAIL |
| Classic NO, 새 x86_64 build와 실제 baseline/replay | [rr-classic-async-20261004.JXBn3w/validation-verdict.txt](../build/local/rr-classic-async-20261004.JXBn3w/validation-verdict.txt) | Baseline32/replay18회 위반, FAIL |
| Metal4 NO, 새 x86_64 compiler-factory build와 실제 baseline/replay | [rr-m4-async-20261004.yycbQA/validation-verdict.txt](../build/local/rr-m4-async-20261004.yycbQA/validation-verdict.txt) | Baseline32/replay18회 위반, FAIL. Classic 결과에서 추론한 것이 아님 |

네 control 모두 `Conv3x3Stride1U8`의 invalid load offset12672, **framework-created buffer length12672**, pipeline UID `4C9FD8F0D3CC3E3D42606174DCD04B7D8126CC0A6E0787BD5A31F1BED564A9AD`가 일치한다. Classic은 `_MFXTemporalDenoisingScalingEffect`와 bare factory, Metal4는 `_M4FXTemporalDenoisingScalingEffect`와 `newTemporalDenoisedScalerWithDevice:compiler:` allocation stack을 실제 보고했다. Baseline32장/replay18장은 finite·non-copy이고 matrix-sensitive frames7, ordinary/mirror의 첫8개 baseline/replay input·output 일치 및 reset0 frame8의 history 변화도 유효하다. **Exit0/PASS 출력 marker와 이 수치/history 결과는 clean RR PASS를 뜻하지 않는다.**

Pristine GPTK4.0b2의 위 SHA256 reference에서 classic descriptor 생성 `0xa44cf`, bare factory selector/call `0xa4678/0xa4682`까지 synchronous setter 없이 default NO를 유지하는 경로를 확인했다([classic factory 원문](../build/local/rr-classic-async-20261004.JXBn3w/reference-classic-factory.txt)). MPL은 sync selector `0x3a3c2`, 인수0 `0x3a3cc`, call `0x3a3ce`, compiler factory selector `0x3a460`/call `0x3a46a`다([MPL 원문](../build/local/rr-classic-async-20261004.JXBn3w/reference-mpl-sync.txt)). 이에 맞춘 **owned classic NO와 별도 M4 NO factory control을 실제로 실행했지만 같은 오류가 남았다.** 이는 GPTK runtime 자체를 실행하여 통과/실패했다는 주장이 아니다.

Native support1인 이 장치/OS에서 유효 입력의 denoiser가 framework model-buffer 위반 없이 실행되는 조건과 public feature13 양 ABI clean acceptance가 여전히 필요하다. Production 변경·가짜 capability·일반 SR/fake fallback·model padding·validation 억제는 하지 않았다. RR은 미지원 하드웨어나 불필요 기능이 아니라 **관련 게임 기능의 미해결**로 유지한다.

**Runtime04:** manifest `e0b5a8618ba0c4c0261755229a5ad85bda607e52bbaf2866c4ffbce0b5c28bb4`. `ngx-alpha-runtime04-verify-{x64,x86}`은 공개 flag128·release-before-execute·replay0/1에서 RGB112/96/80 오차0, 좌/우 alpha48/192를 읽었다. 앞선 `sr-alpha-{x64,x86}-01`의7개 private 수치/수명 case와 public facade proof를 구분한다.

`ngx-contracts-runtime04-verify-{x64,x86}`도 기존 public handle/context/subrect consumer를 API+Shader Validation 오류0으로 통과했다. Runner diagnostics84개와 후속 alpha 관련6개 focused 회귀, native refresh interval CPU 경계를 실행했다. 중복 회귀를 독립 case 수로 합산하지 않는다. Disposable prefix는 각 Wine server 종료 뒤 제거하며, 이전 P4 runtime01/02/03의 payload는 manifest·로그·캡처를 보존한 채 정리했다. Current runtime04와 known-good P3 runtime30은 보존한다.

DXGI 통계는 실제 native presentedTime이 refresh 사이에 있을 때 잘못 disjoint를 반환하던 ±0.4-tick 필터를 제거하고 포함하는 refresh interval로 계산한다. CoreVideo의 미래 output timestamp도 실제 관측 시점까지 지난 refresh로 환산하여 미래 SyncQPCTime을 보고하지 않는다. `tests/dxgi_refresh_timing.cpp`의 경계/실측 표본 회귀를 통과했고, `fsr-fg-runtime04-verify-{x64,x86}`은 실제 drawable 내용·순서·positive displayedTime·pacing/queue proof를 통과했다. 임시 timing 진단 코드는 제거했다.

**Runtime04 최종 public 표시 proof:** `ngx-alpha-runtime04-{x64,x86}`, `fsr-fg-runtime04-{x64,x86}`, `ngx-fg-present-runtime04-{x64,x86}` 6개 결과는 모두 exit0·PASS·validation/foreign 오류0이다. NGX consumer는 FSR API 없이 feature11 결과를 자체 flip swapchain에 제시했다. ABI별12개 positive presentedTime drawable·6개 generated/real pair의 GPU 픽셀과 순서, history/reset/HUDLess/UI·release/replay를 검증했다. Native 표시 간격은 실제로 측정했으며 validation/readback 부하의 숫자를 게임 성능이나 일정 FPS 보장으로 확대하지 않는다.

동일 cut의 `fullscreen-runtime04-{x64,x86}`은 output statistics·vblank·resize/fullscreen disjoint 회귀를, `ngx-contracts-runtime04-x86`은 공개 parameter ABI 계약을 통과했다. `ngx-features-runtime04-x86`도 input16/output36 exact지만 위 독립 native shader 오류 때문에 FAIL이다. `tests/dxgi_refresh_timing.cpp`의 경계·실측 timestamp 회귀와 당시 runner diagnostics84개도 통과했다. 이 범위에서 native 오류 없는 P4 전체 완료를 주장하지 않는다.

**NGX 실제 표시 최종 proof:** `ngx-present-paced-current-{x64,x86}`은 API+Shader Validation/foreign 오류0·PASS다. Public feature11 출력에 caller가 직접 flip swapchain과 Present를 사용하며 FSR facade를 호출하지 않는다. 각각12회 제출/12개 정확한 native capture 중 x64는12개 표시·skip0, WoW64는11개 표시·첫 generated skip1이었다. 모든 real6개와 history/reset/release+replay의 generated4/5/6은 실제 표시됐고 complete generated→real pair는 각각6/5개다. Bootstrap은 별도의 실제 RGB/표시 증거이며 이12개에 합산하지 않았다.

이전 `ngx-present-runtime04-verify-x64`·`ngx-bootstrap-current-x64`의 bounded statistics 실패는 제출 id를 반드시 표시 id로 기다리던 fixture protocol 문제였다. Present S_OK와 GetLastPresentCount는 제출을 나타내며, native presentedTime0은 실제 표시가 아니다. 최종 consumer는 각12개 요청을 한 번씩 제출하고 실제 output WaitForVBlank로 pacing하며 actual/disjoint 통계를 별도 기록한다. Product retry·frame 복제·가짜 표시 count·timeout 연장은 추가하지 않았다. Checker는 모든 capture 내용·UI와 critical 실제 표시·순서·skip 정합을 독립 검사한다. CPU 회귀도 critical frame 누락/순서 반전/잘못된 UI/bootstrap/skip id의 가짜 통계를 거부한다.

### 게임용 owned Core618 loader 완료 — 2026-10-04

동일 evidence root의 runtime08 manifest SHA256은 `063d468d6bae24719ec434d91f3ca2e833641f49f8c513e43bafaa4caa9487df`다. [x64](../build/local/p4-resume-20261004-WoVvSo/agility-runtime08-x64/result.json)·[WoW64](../build/local/p4-resume-20261004-WoVvSo/agility-runtime08-x86/result.json)는 각각 exit0·PASS·Metal API+Shader Validation/foreign 오류0이다. 양 ABI full build/install·fresh stage와 실제 native Core path/PE/hash/generation admission을 통과했다. Godot의 SDK618/package1.618.5 요청을 대상으로 한 owned package identity이며 Microsoft SDK618 전체 conformance, 다른 SDK package 또는 Godot 게임 실행 완료가 아니다.

- **공개 선택·수치:** 6개 case, wrong version/missing path/missing file/non-Core/wrong ABI/different generation의 6개 pre-execution rejection, 8개 exact GPU copy/readback을 통과했다. Cwd를 바꿔도 실제 EXE-relative SDK Core를 선택하고 configuration SDK618·Debug/experimental snapshot·root-signature serializer/deserializer를 확인했다.
- **상태·수명:** Factory/settings/configuration/device/children release 후 기록된 GPU 실행과 module retention, 마지막 owner 해제 후 실제 unload, cold start32회 factory/configuration 생성과 concurrent FreeUnusedSDKs를 검증했다. 선택한 Core의 public GetInterface로 만든 factory만 남겨도 그 SDK module을 유지하고 마지막 해제 뒤 unload한다. Vtable 위치나 source text를 public contract로 pin하지 않는다.
- **프로세스 경계:** Ordinary-mode SDK1 성공과 developer-only base Set 실패를 구분했다. 별도 child의 초기 Set 성공·late Set 실패/global device 제거·independent device 생존·선택 보존, retail Apply/Initialize의 정확한 module binding·다른 live Core Apply의 규정된 `0x887e0003` 실패, EXE의 SDKVersion/SDKPath export만으로 ordinary CreateDevice/GPU 성공을 확인했다. OS registry mode는 복원했다.
- **실제 발견한 loader 결함:** Runtime05에서 Wine builtin Core가 다른 SDK 경로를 같은 module로 합쳤다. Core만 Unix 경계 없는 일반 PE/native-only로 전환했다. Runtime06의 `0x80070002`는 wineboot가 일반 Core를 logical system32에 생성하지 않는 문제였다. 현재 `NtAreMappedFilesTheSame`과 read-lease로 실제 loaded Main 파일을 확인하고 인접 Core를 찾는다. 기존 WINEDLLPATH의 양 ABI 경로는 탐색 hint일 뿐 identity 근거가 아니며 native bridge·새 설정·prefix DLL 복사는 추가하지 않았다.
- **i686 export 결함:** Runtime07 marker 실패를 실제 PE로 조사하니 Core의 GetInterface/marker가 Main IAT thunk로 연결돼 있었다. Stdcall symbol과 Main import name 충돌을 고유한 내부 구현 이름·공개 export alias로 고쳤다. `core-exports-final-*`/`core-bodies-final-*` 및 runtime08 direct-entry sole-owner 회귀가 그 수정 근거다.

Installed-runtime [x64 diagnostics](../build/local/p4-resume-20261004-WoVvSo/diagnostics-runtime07-x64/result.json)·[WoW64 diagnostics](../build/local/p4-resume-20261004-WoVvSo/diagnostics-runtime07-x86/result.json)는 manifest `27cb22abd9963272992d58bc1c381d95191aa3b57c41924b97f60e8e0b704b4b`에서 각각 기존6개 case와 실제 설치된 native Core load/GPU·설정 격리를 통과했다. Ordinary public NGX alpha는 runtime05 양 ABI에서 검증했다. 모든 P4 fixture를 runtime08 한 cut에서 다시 실행한 것은 아니다.

Staging CPU36개·diagnostics CPU87개 및 후속 direct-entry/admission 대상 회귀를 통과했다. 현재 ARM builder의 필수 Core inventory·native/data-export 취급도 이관했다. `build/local/p4-core-arm-planning-v0ea5ver/result.json`은 실제 ARM64/ARM64EC Core object/link·ARM64X paired Core metadata와 build-plan 검사를 보존한다. Full ARM translator 재빌드나 보류된 ARM64/FEX guest 실행의 증거는 아니다. Frozen import builder와 과거 patch는 바꾸지 않았다.

이 Core 검증에 사용한 disposable prefix는 각 Wine server 종료 뒤 제거했다. Worker dependency와 open-file 검사를 마친 runtime05/06/07의 `bin/lib/share`만 retire하고 manifest·provenance·로그·result·frozen SDK DLL은 보존했다. 최신 실행 payload는 runtime08이다.

### 새 P3 #24 volume oracle 규격 교정 — 2026-10-03

Runtime-09의 BC1 첫 실패는 GPU red=84/255를 CPU85/255±0.003과 비교한 잘못된 정밀도 oracle였다. 공개 D3D11.3 §§19.5.2–19.5.6의 MSB extension·정수 reference division·채널별 엄격 오차 범위와 §3.2.3.7의 sRGB 입력 쪽 변환 오차를 적용했다. BC1 값의 lower/upper를 sRGB linearization 전에 만들고 공간/Z/mip/reduction 필터로 전달한다. 정확한0/1·alpha·NULL/transparent와 non-BC 정밀도는 완화하지 않았다. COMMON initial layout 뒤 legacy COPY_DEST transition도 fixture에 명시했다. [x64 spec02](../build/local/p3-20261003/tiled-tier4/public-runtime09-x64-bc1-spec02/result.json)는 DXIL147·DXBC147 volume sampling, implicit LOD, UINT/clear, BC1/sRGB 양 compiler 경로까지 통과했지만 다음 typed-store oracle에서 중단했다.

Typed UINT32→UINT16 store는 §3.2.3.13/§19.1.3.2/§22.4.9에 따라 포화하며 API ClearUnorderedAccessViewUint의 하위 비트 복사와 다르다. 따라서65535를31로 기대한 절삭 oracle를 교정했다. 한 overflow lane을 회전시키고 다른 세 lane은 서로 다른 유효 값을 유지해 channel swap·alias 오류를 계속 검출한다. [x64 spec03](../build/local/p3-20261003/tiled-tier4/public-runtime09-x64-spec03/result.json)는 ordinary parent의 RG32↔RGBA16 same-dispatch·partial-W·guard-byte 검사를 DXIL/DXBC 모두 통과했다. 이어 sparse BC parent의 NULL/OOB 기대0에서 실제0xf800으로 실패해 **전체 #24는 아직 미완료**다. 수정한 양 ABI fixture는 compile exit0이나 WoW64 GPU acceptance는 아직 없다.

### 새 P3 #15 raw Atomic64 stress 진행 보장 — 2026-10-04

Runtime24 full-SM6 x64는 앞선51 numerical cases 뒤 첫65536-thread stress에서60초 fence timeout이었다. 별도 frozen-bytecode first-dispatch probe도 Close/Execute/Signal은 반환했으나 native submission2가 완료되지 않았다. [실제 post-submit sample](../build/local/p3-20261003/atomic64/sm6-first-dispatch-runtime24-x64-03/sample-identity.json)과 native stack은 completion thread의 `winemetal_metal4_call→pthread_cond_wait`, submitter의 wait를 확인했다. Native GPU Hang 오류로 단정하지 않았다.

Native control은65536 invocations·64×1024·C131136/P65536/N327745·1MiB Out/512B Raw와 align4 store의32-bit fragments 및 generic CAS의 unchanged-value publication까지 동일하게 유지했다. 원래 [hint control01](../build/local/p3-20261003/atomic64/sm6-first-dispatch/native-full-hint-target1-01.log)은 API+Shader Validation에서75초 내 feedback이 없었다. Read/CAS helper를 따로 조정하는 대신 **initial read와 모든 CAS retry를 포함한 RMW 전체를 SIMD elected lane 안에서 수행**한 exact08 [target1](../build/local/p3-20261003/atomic64/sm6-first-dispatch/native-full-rmw-target1-exact08.log)·[target16](../build/local/p3-20261003/atomic64/sm6-first-dispatch/native-full-rmw-target16-exact08.log)은 각각 GPU2572.159/178.516ms, full returned-value/final-value oracle·mismatch0·remaining references/reserved0으로 통과했다. 중간02–07은 generic CAS 대신 direct CAS를 쓴 부분 동등 control이므로 owned workload의 최종 proof로 세지 않는다.

Production 변경은 `src/airconv/dxil_atomic64.cpp`의 physical SIMD cooperation이다. Per-lane callback captures·return values와 scalar native helper/allocator/refcount/ABI는 유지했다. Standalone read는 metadata와 convergent 의미만 유지하고 application-write helper attribute를 붙이지 않았다. 새 IR 모양/count를 고정하는 테스트는 남기지 않았으며 기존 phase lowering/linkage/classification·LLVM validity [CPU contracts](../build/local/p3-20261003/atomic64/cooperative-phase-cpu-run.log)를 통과했다.

Paired runtime29(manifest SHA256 `e7ba95e5da1b9e12750c6d69315f7803af20660a5bf3eb518876d21faaf41511`)의 full-SM6 [x64](../build/local/p3-20261003/atomic64/final-runtime29-sm6-x64-01/result.json)·[WoW64](../build/local/p3-20261003/atomic64/final-runtime29-sm6-x86-01/result.json)는51 numerical cases와 stress target1/16 각각20회·65536 invocations를 모두 통과했다. Repetitions·full64 oracle·60초 fence 및600초 runner deadline은 그대로다. ABI별 두 stress 모두 exact20/20, exit0·API+Shader Validation·owned native compiler/backend·validation/foreign 오류0이다. 관측 worst latency는 x64 target1/16 각각2154.9/193.5ms, WoW64는2120.2/216.6ms이며 일반 게임 성능이나 다른 GPU/OS 보장은 아니다.

Runtime29의 영향 경로16회도 통과했지만 [RT x64](../build/local/p3-20261003/atomic64/final-runtime29-raytracing-atomic64-x64-01/stderr.log)는 GPU 실행 전 native PSO 생성에서 `intersection function dxmt.rt.bounding_box uses illegal function (air.get_thread_index_in_simdgroup.i32)`로 실패했다. 모든 stage에 SIMD election을 삽입할 수 있다는 가정이 잘못됐다. 협력 적용을 최종 `LowerAtomic64Phase`로 옮겨 AIR entry metadata와 merged callgraph가 완성된 뒤 판단한다. 실제 `air.intersection` root에서 pointer cast를 포함한 transitive direct callees를 추적하고, 그 READ/RMW wrapper 및 공유 helper는 기존 scalar 알고리즘을 유지한다. 나머지 kernel/graphics wrapper는 whole-RMW 협력을 유지한다. Fixture/export 이름별 예외·oracle 완화·native algorithm/ABI 변경은 없다.

Paired runtime30(manifest SHA256 `a96309ccc9bedb9f1bd31fdd7d4e838ac8fe0a1c2ebf9c95773ac43d48ec51ee`)은 [x64 build](../build/local/p3-20261003/full-build45.log)·[WoW64 build](../build/local/p3-20261003/full-wow64-build31.log)·install/stage 및 원래 [phase CPU contracts](../build/local/p3-20261003/atomic64/cooperative-phase-cpu-run30.log)를 통과했다. 최종 적용 시점이 공통 compiler 경로를 바꿨으므로 [CORE의10 suites×양 ABI20회](CORE-IMPLEMENTATION-STATUS.ko.md#atomic64-완료와-전체-계약-proof)를 이 cut에서 직렬 재실행했다. RT 양 ABI 전체 direct/indirect oracle, full-SM6, typed·phase·producer·work-graph record/typed·coupled pixel·contention·physical helper 전부 PASS다. 모든 result.json은 exit0·API+Shader Validation·owned compiler/backend·validation/foreign 오류0과 동일 manifest를 확인했다.

Runtime30 full-SM6 stress의 각 target은 양 ABI 모두20/20 exact였다. x64 target1/16의 worst/mean은2106.6/1927.8ms 및196.2/181.5ms, WoW64는2198.5/1959.5ms 및211.4/203.4ms다. Invocation65536·20 repetitions·60초 fence·600초 runner deadline과 전체 수치 oracle는 변경하지 않았다. 이 결과와 앞선 groupshared/AS/MS proof로 **새 P3 #15를 닫고 #15–24 전체 선정 계약 완료**로 전환했다. 공개 SM6.5/FL12_1과 ARM64/FEX 보류는 유지한다. 모든 실행의 disposable prefix는 해당 wineserver 종료 뒤 제거했으며 실패/성공 로그·JSON·입력·native 조사 증거는 보존했다.

### 새 P3 #24 compatible BC storage와 clear publication 조사 — 2026-10-04

Runtime23의 sparse bridge226 access violation은 queue/atlas/heap raw 인자를 보존한 뒤 native SEH PC를 symbolicate했다. `-[AGXG17XFamilyComputeContext_mtlnext updateTextureMappings:heap:operations:count:] +0x27`의 receiver 내부 `+0x48`가 null이고 `+0x1830` read에서 실패했다. NSZombie control은 deallocated-object 메시지를 내지 않았다. [CPU symbolication](../build/local/p3-20261003/tiled-tier4/sparse-map-symbolicate.log)과 [실제 SEH](../build/local/p3-20261003/tiled-tier4/public-runtime23-x64-mapping-zombie15/stderr.log)를 보존한다. 이것을 임의 texture retain 추가나 status 무시로 덮지 않았다.

동일 independent BC5 SNORM atlas의 map-only·metadata-blit·bootstrap-barrier native controls는 모두 첫 parent→UInt→SNORM remap에서 exit139였다. 같은 family의 signed view가 native parent storage/mapping을 공유하는 [control](../build/local/p3-20261003/tiled-tier4/native-sparse-parent-view-01.log)은32 remap cycles·37 event completions·exact GPU status bytes와 실제 BC5 signed decode(17/127,34/127)를 API+Shader Validation에서 통과했다. Production은 기존 `AreCopyCompatibleFormats`로 BC family 전체에 이 parent-view 경로를 적용하고 incompatible/block-to-texel transport는 유지했다. Temporary native diagnostics는 제거했다.

Pristine reference는 원본 `D3DMetal.framework-4.0b2.zip` member의 SHA256 `f5b56df1b8fe8b364dd9530651a3769c8aed948bd343be3b4510604d503e2bad`를 다시 확인했다. Read-only `UpdateTileMappings→D3DMCommandQueue→worker` disassembly는 queued mapping command가 native texture/heap을 retain하는 경로를 보여준다([API](../build/local/p3-20261003/tiled-tier4/pristine-update-tile-mappings-annotated.disasm)·[queue](../build/local/p3-20261003/tiled-tier4/pristine-queue-map-annotated.disasm)·[worker](../build/local/p3-20261003/tiled-tier4/pristine-worker-map-annotated.disasm)). Reference는 compatibility evidence이며 owned GPU PASS나 production dependency가 아니다.

[Runtime25 strict Tiled x64](../build/local/p3-20261003/tiled-tier4/public-runtime25-x64-parentview16/result.json)는 BC5/BC6H signed/plain·native-decode·full64KB·NULL/remap 전체를 지난 뒤 volume copy/RTV full-page alias byte61440(actual00/expected20)에서 실패했다. 추가 [clear-only boundary](../build/local/p3-20261003/tiled-tier4/public-runtime25-x64-clear-boundary17/result.json)는 후속 z17 draw 전부터 같은 불일치를 보였다. Native2-layer clear의 [inferred extent](../build/local/p3-20261003/tiled-tier4/native-layered-clear-inferred-01.log)·[logical extent](../build/local/p3-20261003/tiled-tier4/native-layered-clear-logical-01.log)는 각각8192 pixels exact·API+Shader Validation PASS이므로 native render-area workaround를 추측하지 않고 owned publication을 조사한다. Whole Tiled PASS는 아직 아니다.

Runtime27 actual GPU capture에서 real RTV shadow는 firstW15·Wsize2·2 mapped pages·1 heap·native128²였으며 attachment handle과 clear color(32,64,128,255)는 정확했다. 그러나 import 직후와 clear 직후 두 slice의4 quadrant 모두0이었다. [실제 capture](../build/local/p3-20261003/tiled-tier4/public-runtime27-x64-shadow-capture19/stderr.log)의 page baseline은 유효하고 assembled changed/native page만0이어서 publication 이전 shadow backing 문제로 좁혔다. Production과 같은 Untracked·RT/PixelFormatView-only sparse texture, 독립 mapping queue, event wait, blit→Fragment fence를 사용한 native A/B에서 [sparse texture만 resident](../build/local/p3-20261003/tiled-tier4/native-layered-clear-owned-01.log)는 첫 pixel0으로 실패(exit4)했고, **오직 mapped heap을 residency set에 추가한** [control](../build/local/p3-20261003/tiled-tier4/native-layered-clear-owned-resident-01.log)은8192 pixels exact·API+Shader Validation으로 통과했다. 이 causal proof를 production mapped-backing residency 수정의 근거로 사용하며, 최종 public Tiled acceptance를 대체하지 않는다.

Runtime28은 append-only `WMTMetal4UseAllocation`을 통해 private RTV·BC/UInt shadow의 mapped heap을 consuming recording의 기존 allocation/residency set에 넣고 completion retirement까지 보유한다. 별도 residency set·barrier·CPU wait를 추가하지 않았다. 모든 temporary page/shadow/attachment capture를 제거한 [원래 strict fixture](../build/local/p3-20261003/tiled-tier4/public-runtime28-x64-backing-resident20/result.json)는 이전 실패였던 `volume-copyregion-RTV-clear-draw-64KB-alias-byte-oracle`와 후속 xyz mapping·NULL-write/remap exact64KB 검사를 통과했다. 다음 predicated CopyResource에서 드러난 fixture 오류는 별도로 교정했다: 공개 `SetPredication` 계약상 predicate1은 `NOT_EQUAL_ZERO`에서 skip, `EQUAL_ZERO`에서 execute인데 신규 volume fixture의3 operation selectors가 반대였다. 기존 queue와 ordinary predication oracle는 이미 올바르므로 runtime 변경 없이 selector만 수정하고 expected bytes·모든 mip·skip/execute numerical 검사는 유지했다.

최종 [Tiled x64](../build/local/p3-20261003/tiled-tier4/public-runtime28-x64-predicate-contract21/result.json)·[WoW64](../build/local/p3-20261003/tiled-tier4/public-runtime28-x86-predicate-contract21/result.json)는 ABI별76 ordered cases·실제 Tier4·exit0·API+Shader Validation·owned compiler/backend·validation/foreign 오류0으로 전체 suite를 통과했다. 같은 runtime28에서 ordinary volume8·standard8·shared5도 양 ABI6회 모두 PASS했다(`positive-runtime28-relaxed-casting-{volume,volume-standard,shared}-{x64,x86}-01/result.json`; DXBC 입력). Manifest SHA256는 `58775a050b81f9059373b258053496655a5e472453c27b341a726a6a14498c4d`다. #16/#24와 #24에 포함된 필수 Tier3를 완료하며, P2의 depth bounds·conservative Tier3 및 SM/FL 상향은 별개다. 각 prefix는 wineserver 종료 후 제거했고 모든 실패·native control·실행 로그는 보존했다.

### 새 P3 #16 advanced texture24 — 2026-10-03

Immutable runtime-08의 [x64](../build/local/p3-20261003/advanced-texture/runtime08-advanced-x64-public-validation/result.json)·[WoW64](../build/local/p3-20261003/advanced-texture/runtime08-advanced-x86-public-validation/result.json)는 ABI별24 ordered readback cases·exit0·API+Shader Validation·owned compiler verified·validation/foreign errors 없음으로 PASS했다. Manifest SHA256는 `110ae822bc7399bfd2aec49588f0a5d5a9e5cbac94d6251ccf7127668620eb42`이며 각 invocation에 genuine SM6.7 inputs62개와 HLSL/producer hashes를 보존했다. Comparison6 shapes × CS/VS/PS18 및 raw16/32/64 ×2D/array6의 proof이며 전체 stage/GPU·#16 aggregate·cap 완료 선언은 아니다.

이전 `runtime06-x64-public-validation`의 row0 status는 HLSL overload가 output status 대신 입력 offset에 바인딩된 producer 오류였다. [수정 producer audit](../build/local/p3-20261003/advanced-texture/status-overload-consumer-audit.json)와 원래 실패를 보존한다. 수정 후 `runtime06-x64-status-overloads-public-validation`의 NULL row5는 DXIL NULL ResRet가 mapped status까지0으로 만든 별도 root였고 runtime08의 zero-data/mapped-status1로 해결했다. Native AIR residency는 bit0=0이 resident인 기존 polarity를 유지하며 D3D11/global encoding을 뒤집지 않았다.

Depth COMMON fixture의 [runtime08 x64](../build/local/p3-20261003/advanced-texture/runtime08-depth-common-x64-01/result.json)는 이전 illegal enhanced-initial→legacy-barrier handoff를 fixture에서 교정한 뒤 d16/d32 upload·clear4 cases를 통과했다. 이어 d16 MSAA phase0 word3 sample-count `actual2/expected4`로 중단했다. D3D12 depth-cast SRV는 per-sample2D-array shadow를 노출하지만 query는 native-MSAA handle ABI를 쓰던 root를 shared `CreateTextureQuery`에서 고쳤다. 기존 metadata/table로 physical dimension과 logical sample-count를 구분하며 shader/fixture expectation은 바꾸지 않았다.

이 수정이 포함된 runtime-09(manifest SHA256 `f79c530e60e350b8d9858c10eb54c697d030de47642065ae98f03d7fb0b2cdcb`)의 [depth x64](../build/local/p3-20261003/advanced-texture/runtime09-depth-x64-01/result.json)·[WoW64](../build/local/p3-20261003/advanced-texture/runtime09-depth-x86-01/result.json)는 각각 d16/d32 upload·clear·MSAA draw·MSAA clear8 cases, exit0, API+Shader Validation, owned native compiler 확인, validation/foreign errors0으로 PASS했다. 전용 prefix는 Wine 종료를 확인하고 제거했으며 logs/result/inputs는 보존했다. #16 aggregate의 volume/STD/shared casting acceptance는 별도로 남는다.

### 새 P3 #20 matrix runtime·public admission — 2026-10-03

Paired runtime-05의 untouched preview SDK V1 fixture는 API+Shader Validation에서 [matrix x64](../build/local/p3-20261003/matrix/runtime05-public-x64-02/result.json)·[WoW64](../build/local/p3-20261003/matrix/runtime05-public-x86-01/result.json), [work-graphs x64](../build/local/p3-20261003/matrix/work-graphs05-public-x64-01/result.json)·[WoW64](../build/local/p3-20261003/matrix/work-graphs05-public-x86-01/result.json) 모두 PASS였다. Public union56/48 bytes,10 datatype ×16 layouts ×2 replays의 exact raw bits/padding, offset32 S8/64 F16, invalid recording/overflow, GPU-only conversion lifetime·predication·enhanced sync와 CL11 consumer-first cross-queue wait/signal·rewind·async·state handoff를 확인했다. Work-graphs의 기존9 case와 실제 GPU records4의 reset default 복원/max3 rejection도 통과했다.

Public experimental shader GUID를 device 생성 전에 opt-in하고 unknown/malformed·unchanged-set preservation·changed-set two-device removal·pending fence wakeup·destruction lifetime을 확인했다. Owned policy는 Windows Developer Mode registry를 읽거나 쓰지 않는다. SDK는 untouched이며 normal MinGW explicit `RetVal` getter ABI를 사용했다. Fake `_MSC_VER` selector는 사용하지 않았다. 후속 V2 runtime shapes와 혼합하지 않았다.

첫 [x64-01 FAIL](../build/local/p3-20261003/matrix/runtime05-public-x64-01/result.json)은 FLOAT32 enhanced case의 `COPY_DEST` legacy state를 `COMMON`으로 반환하지 않은 fixture 오류였다. Frozen enhanced spec의 interop 규칙에 맞춰 두 buffer handoff를 추가했고 production state validation을 완화하지 않았다. 새 x64-02와 첫 x86 실행으로 수정된 fixture를 검증했다. **이 cut은 Tier0/legacy FALSE의 첫 runtime behavior proof다. Shader oracle와 최종 Tier1 query/readback enabled-cut proof 전에는 #20 전체 완료로 승격하지 않는다.** [재현·범위](SMOKE-TEST.ko.md#matrix-runtime-admission과-pinned-v1-focused-proof)·[현행 runtime 계약](CORE-IMPLEMENTATION-STATUS.ko.md#matrix-runtime-admission과-pinned-v1-focused-proof)을 따른다.

### 새 P3 #20 최종 positive-cap public consumer — 2026-10-03

**#20 구형 WaveMMA와 후속 pinned SM6.10 preview matrix 모두 완료했다.** Runtime-05 behavioral·runtime-06 shader numerical·runtime-07 advertised-cap의 양 ABI mixed-cut 증거로 닫았으며 최신 단일 cut full-suite 성공을 주장하지 않는다. Mandatory atomic 통합 acceptance가 남아 ordinary HighestShaderModel6.5를 유지하고 preview matrix를 전체 SM6.10 conformance로 승격하지 않는다.

Immutable runtime-07의 strict public consumer가 archived retired SDK와 pinned successor SDK를 각각 사용해 [retired x64](../build/local/p3-20261003/matrix/caps07-retired-x64-02/result.json)·[WoW64](../build/local/p3-20261003/matrix/caps07-retired-x86-01/result.json), [successor x64](../build/local/p3-20261003/matrix/caps07-preview-x64-01/result.json)·[WoW64](../build/local/p3-20261003/matrix/caps07-preview-x86-01/result.json) 모두 exit0·pass=true·API+Shader Validation·owned backend·validation/foreign errors 없음으로 PASS였다. Manifest SHA256는 `1a79c00dd85e4d2528bcf5edbdafb0469081e8bd1ad4737b40da6865cee28042`다. Main은 transient gate를 제거하고 query38/77/78과 OPTIONS9 WaveMMATier10을 함께 cutover했다.

Retired query38의12 combinations와 successor tier16·V1 query union56/48 bytes·6 operation arms·10 construction types·native16³ count/fill 및 capacity-tail guards·F16 flags·group/vector/atomic support를 검사했다. 각 successor는 resources/recording을 caller가 해제한 뒤 isolated conversion exact GPU readback을 추가 확인했다. 기존 full runtime/shader bodies를 다시 실행하지 않고 runtime-05 behavioral/runtime-06 numerical proof와 별도 final enabled-cap 증거를 남겼다.

첫 retired-x64-01은 evaluate transport의30초 deadline에 fresh Wine prefix startup이 중단되어 result.json과 consumer output이 없었다. Semantic consumer failure로 세지 않으며 partial logs/invocation을 보존했다. 명시적 충분한 transport deadline의 fresh x64-02부터 네 strict suites를 serial 실행했고 expectation을 완화하지 않았다. Actual exit 후 own processes 없음과 runner cleanup을 확인하고 다섯 dedicated prefixes만 제거했다. [최종 계약](CORE-IMPLEMENTATION-STATUS.ko.md#wavemmalinear-algebra-최종-positive-cap-proof)·[재현과 증거](SMOKE-TEST.ko.md#wavemmalinear-algebra-최종-positive-cap-proof)를 따른다.

### 새 P3 #20 WaveMMA·후속 matrix shader consumer — 2026-10-03

실제 archived WaveMMA와 preview linear-algebra producer의11 frozen entries를 untouched public SDK consumer로 실행했다. [runtime-06 x64](../build/local/p3-20261003/matrix/gpu-06-x64-01/result.json)·[WoW64](../build/local/p3-20261003/matrix/gpu-06-x86-01/result.json)는 각각 ordered11·exit0·API+Shader Validation·owned native compiler/load 확인·validation/foreign backend errors 없음으로 PASS였다. Retired12 ops·4 large shapes·packed mixed signedness·F16 input subnormal/16·32-bit accumulation과 successor two-SIMD group·all10 wave/group construction formats·mandatory Thread quantization/bias·S32/U32/F32 atomic 및 outer/vector readback을 검사했다. Actual public GPU conversion 결과를 poison된 weights resource의 Thread shader가 소비했고 admission/conversion consumer 두 mandatory marker가 통과했다.

첫 [runtime-05 x64](../build/local/p3-20261003/matrix/gpu-05-x64-01/result.json)는 original WaveMMA PASS 뒤 successor wave PSO에서 Metal compiler XPC connection interruption으로 실패했다. Emitted AIR의 unlowered `llvm.roundeven.f32`를 확인하고 matrix cast를 기존 DXIL unary26→AIR rint로 교정했다. Unchanged ABI 재시도를 하지 않았다. Corrected AIR의11 native PSO compile-only 검사를 거쳐 새 paired runtime-06에서 실제 numerical GPU proof를 실행했다. 실패 artifacts와 source/producer hashes를 보존했고 dedicated prefixes만 stop/wait/remove했다.

이 shader cut은 당시 비활성 cap을 전체 positive query proof로 승격하지 않는다. 앞의 runtime-05 behavioral proof와 후속 enabled-cut cap/query proof를 합쳐 #20 closure를 판단한다. Signed/final322–342 producer GPU·전체 SM/FL·다른 GPU/OS·게임·성능·ARM64/FEX/no-Rosetta claim은 아니다. [현행 shader 계약](CORE-IMPLEMENTATION-STATUS.ko.md#wavemma와-linear-algebra-shader-consumer-proof)·[재현 recipe](SMOKE-TEST.ko.md#wavemma와-linear-algebra-shader-focused-proof)를 따른다.

### 새 P3 #15 atomic-phase physical helper promotion — source cut, 2026-10-03

Compute phase admission의 [runtime09 x64](../build/local/p3-20261003/atomic64/phase-runtime09-x64-01/result.json)·[WoW64](../build/local/p3-20261003/atomic64/phase-runtime09-x86-01/result.json)는 API+Shader Validation, exit0/pass=true, owned native compiler 확인, validation/foreign errors0으로 PASS했다. GPU-produced indirect arguments/count/grid/root constants/predicate4 scenarios, ordinary↔atomic crossqueue/shared-fence rewind2 scenarios, alias·persistent-map의 old/final/canonical/ordinary bytes와 untouched guards를 독립 CPU 값에 비교했다. 두 dedicated prefix는 정상 Wine 종료 후 제거했다. Manifest는 runtime09 f79c530e60e350b8d9858c10eb54c697d030de47642065ae98f03d7fb0b2cdcb이며 이 완료 slice를 graphics/TG/typed/RT/work-graph 전체 acceptance로 확대하지 않는다.

Helper public PSO 실패를 genuine VS와 phase capture 옵션으로 분리한 [CPU 재현](../build/local/p3-20261003/atomic64/helpers-vertex-capture09-capture.log)은 private packed vertex-return aggregate store를 user-memory atomic lowering이 거부하는 원인이었다. msaa_vertex의 해당 생성 store에만 기존 phase-conditional transport metadata를 붙였다. Application UAV 검사나 aggregate 폭 제한을 없애지 않았다. 같은 입력의 [fixed native capture PSO](../build/local/p3-20261003/atomic64/helpers-vertex-capture11-native.log)와 actual helper root/signature를 쓴 [logical1/native1 APP PSO](../build/local/p3-20261003/atomic64/helpers-app11-native.log)는 API+Shader Validation에서 생성 PASS, GPU submissions0이다. Public draw의 수치 검증은 이 compiler-only 관찰과 별개다.

그 뒤 [public runtime11 helper](../build/local/p3-20261003/atomic64/helpers-runtime11-x64-02/result.json)는 PS coverage variant0에서 graphics-stage lock 거부를 재현했다. DXIL lowering이 coverage/replay의 application body 제거보다 먼저 실행되므로 atomic64 PS의 모든 variant에 phase와 Metal4.1 옵션을 전달하도록 수정했다. 원래 helper DXIL·root·실제 varying interface로 만든 [coverage/replay native PSO2개](../build/local/p3-20261003/atomic64/helpers-aux12-native02.log)는 API+Shader Validation에서 PASS, GPU submissions0이다. Lock 거부를 지우거나 unsafe graphics lock으로 되돌리지 않았다.

Record64 fixture는 모르는 SM enum에 E_INVALIDARG를 반환하는 runtime에서도 공식 decreasing-model 협상을 하도록 수정했다. [runtime09 재실행](../build/local/p3-20261003/atomic64/workgraph-records-runtime09-x64-02/stderr.log)은 실제 SM6.5/unadvertised 상태로 GPU 제출·완료까지 진행했지만 terminal node의 exchange old/new 상위32비트가 0이 되어 FAIL했다. 앞선 full64 add/xor/compare-exchange와 record 전달 값은 일치했다. 이 결과도 work-graph acceptance가 아니다.

Root cause는 record 연산의 bitwidth가 아니라 backing queue 정렬이었다. 실제 child/terminal/unused-seed 순서의 stride16/16/4가 level_bytes68을 만들고 다음 level의 uint64 필드를 address108(mod8=4)에 놓았다. 기존 checked arithmetic으로 queue stride·offset·level span을 최소8 및 선언된 record alignment에 맞췄다. 해당 span은72, 필드 주소112, backing204→216바이트가 된다. Empty input의 alignment0 sentinel은 보존하며 public backing address 계약은 바꾸지 않았다. [runtime13 x64](../build/local/p3-20261003/atomic64/records-aligned-x64-02/result.json)·[WoW64](../build/local/p3-20261003/atomic64/records-aligned-x86-02/result.json) 모두 CPU/GPU input32 records·384 qwords, old/final/descendant/adjacent guards·queue counts의 independent runner oracle PASS, exit0, API+Shader Validation errors0, foreign0, owned native compiler 확인이다. Manifest는7a3f0a8ecb01002d2f1d3614815fdef00efb89767d42426d2a9177a5da815381. GPU 통과 뒤 드러난 runner의 validation_errors 초기화 순서도 고쳐 이 paired 실제 실행에서 result.json 생성을 확인했다. Dedicated prefixes는 정상 종료 후 제거했다. 이는 record64 slice의 완료이며 typed node/graphics/전체 Atomic64 capability 증명은 아니다.

같은 record layout의 일반 work-graph 회귀는 [runtime15 x64](../build/local/p3-20261003/atomic64/workgraphs-alignment-runtime15-x64-01/result.json)·[WoW64](../build/local/p3-20261003/atomic64/workgraphs-alignment-runtime15-x86-01/result.json)에서 각각 기존9 case 전부 PASS였다. API+Shader Validation, owned backend/native compiler, exit0, validation errors0, foreign0을 확인했다.

Groupshared64의 공개 compute oracle도 runtime15에서 paired 통과했다. [x64 operations/stress](../build/local/p3-20261003/atomic64/groupshared-runtime15-x64-operations-01/result.json)·[WoW64 operations/stress](../build/local/p3-20261003/atomic64/groupshared-runtime15-x86-operations-01/result.json)는9 연산·compare/wrap과64개 group 각각1024 full64 반환 순열·최종값을 API+Shader Validation에서 확인했다. [x64 capacity](../build/local/p3-20261003/atomic64/groupshared-runtime15-x64-capacity-01/result.json)·[WoW64 capacity](../build/local/p3-20261003/atomic64/groupshared-runtime15-x86-capacity-01/result.json)는32KiB의4096 uint64 슬롯 old/final을 API Validation으로 확인했다. Shader Validation은 이 입력에 추가 threadgroup storage를 요구하므로 capacity 결과를 shader-instrumented proof로 기록하지 않는다. 네 결과 모두 exit0·native compiler 확인·validation errors0·foreign0이며 manifest는8495ec184a47f0b72843f5dbcea80a90ea513f96a1fbfa48c618354b65bff7c9다. Dedicated prefixes는 종료 후 제거했다. 이는 compute groupshared slice이며 전체 Atomic64 cap 공개 근거는 아니다.

Public helper runtime12의 native pixel PSO 실패는 [동일 입력의 native 비교](../build/local/p3-20261003/atomic64/helper-metal4-pso-probe-lifetime.log)에서 Metal3·Metal4 모두 PSO 생성 PASS였고, block-local atomic tile vertex descriptor owner가 public PSO 호출 전에 해제되는 lifetime 문제로 좁혀졌다. Owner를 GetMSAAPipelineVariants 범위에 유지하도록 수정했다. Native weak-reference 관찰은 freed handle을 GPU나 compiler에 제출하지 않았다. 실제 draw acceptance는 별도다.

그 다음 runtime14 helper는 direct draw의 비어 있는 indirect-root override를 복사하다 native nil sourceBuffer assertion으로 중단했다. 실제 direct roots는 recorded vertex/fragment/object/mesh root-binding command에 있으므로 기존 non-MSAA reader를 공통 atomic argument reader로 옮겼다. [runtime15 실제11 case](../build/local/p3-20261003/atomic64/helpers-runtime15-x64-05/result.json)는 끝까지 실행했고 modes3/4/9는 PASS였지만 나머지8 case의 logical helper peer 또는 inclusive-wave helper count가 틀려 전체 FAIL이다. Backend/native compiler·API+Shader Validation errors0은 수치 oracle PASS를 대신하지 않는다.

실제 원인은 `guardSampleEffects`가 pure quad/SIMD exchange·ballot 호출을 write로 오인해 logical-alive guard 안에 넣은 것이었다. 해당 pure intrinsic만 helper 실행에 보존하고 UAV/store/atomic/fence의 alive/discard·native-helper 배제는 유지했다. Producer auxiliary variant 전체에 이미 필요한 atomic phase 옵션을 전달한 뒤 드러난 private replay aggregate load도 기존 transport-origin metadata로 구분했다. User-memory scalar-width 검사나 application semantics를 완화하지 않았다. [runtime17 x64](../build/local/p3-20261003/atomic64/helpers-runtime17-x64-07/result.json)·[WoW64](../build/local/p3-20261003/atomic64/helpers-runtime17-x86-07/result.json)는 기존11개 수치 oracle 모두 PASS, exit0, API+Shader Validation, owned backend/native compiler 확인, validation/foreign 오류0이다. Manifest SHA256는 `bc161233e7c575210f5ae8bd0c7ebb14ecc0019b4bf1ac89fcb5da8f94338f0b`이며 dedicated prefixes는 종료 후 제거했다.

AS/MS groupshared64 focused consumer는 [runtime16 x64](../build/local/p3-20261003/atomic64/mesh-groupshared-runtime16-x64-01/result.json)·[WoW64](../build/local/p3-20261003/atomic64/mesh-groupshared-runtime16-x86-01/result.json)에서 각각 `groupshared64_ms`·`groupshared64_as` 모두 full readback oracle PASS였다. API+Shader Validation·owned backend/native compiler·exit0·validation/foreign 오류0을 확인했다. Compute32KiB capacity의 API-only 제한을 이 두 shader-instrumented 결과와 혼동하지 않는다.

Coupled pixel consumer는 [runtime18 x64](../build/local/p3-20261003/atomic64/pixel-runtime18-x64-resolve02/result.json)·[WoW64](../build/local/p3-20261003/atomic64/pixel-runtime18-x86-resolve01/result.json)에서 각각40 ordered cases를 통과했다. ROV/non-ROV ×typed/structured **buffer**64 ×pixel/4×sample-frequency ×late-depth·late-discard·early-depth-reject·early-discard·early-stencil-reject의 조합이며, full64 old/final/high-word carry·같은 buffer ordinary32 alias·color·positive/negative depth/stencil probe·occlusion·IA/PS statistics를 함께 검사했다. API+Shader Validation·실제 owned compiler/backend·exit0·validation/foreign 오류0이며 manifest는 `21a6f5863d71ddc311f692d8bbb7791ef07e788afea32edbad4d49a79c7a7ec3`다. 첫 x64 실행은 single-sample resolve 대상에 source MSAA의4MiB alignment를 재사용해 resource creation에서 중단됐고, 새 대상의 sample/quality/alignment를 `{1,0}`/default0으로 정한 뒤 원래40-case oracle 그대로 통과했다. Typed **texture**의 별도 native compilation/전체 shape acceptance를 이 buffer 결과로 대신하지 않는다.

Raytracing atomic64의 [runtime19 x64](../build/local/p3-20261003/atomic64/rt-runtime19-x64-indirect03/result.json)·[WoW64](../build/local/p3-20261003/atomic64/rt-runtime19-x86-indirect03/result.json)는 direct/indirect 확장 shader·full64 readback의 기존 전체 runner oracle PASS, exit0, API+Shader Validation, owned backend/native compiler 확인, validation/foreign 오류0이다. Manifest는 `9d796fa678e4227ad9fd40ef2d3921b8887a66bafe45273c3192f2156e70932d`다. Runtime15 direct와 runtime17 indirect에서 관찰한 CPU address0x10 fault는 atomic phase가 current pass를 닫은 뒤 trace가 `encoder_current->id`를 참조했기 때문이다. Traversal helper가 allocator-owned 기록을 반환하게 하고 direct/indirect 모두 그 실제 기록 ID를 사용했다. Trace를 끄거나 가짜 ID를 넣지 않았으며 indirect GPU count/predication 의미도 바꾸지 않았다.

Typed2D work-graph node는 [runtime20 x64](../build/local/p3-20261003/atomic64/workgraph-typed-runtime20-x64-abi29-02/result.json)·[WoW64](../build/local/p3-20261003/atomic64/workgraph-typed-runtime20-x86-abi29-02/result.json)에서16texel·48 full64 words·physical-copy guards의 전체 수치 oracle를 통과했다. 기존 Shader Validation compiler XPC 오류의 원인은 AIR2.9 module에 legacy texture-read intrinsic 인자 규약을 넣었던 것이다. 실제 native MSL의 built-in read sampler·dimension별 offset 인자 순서를 적용하고 구버전 AIR 경로는 유지했다. Canonical64 연산을 제거하거나 Shader Validation을 끄지 않았다.

Graphics producer는 [runtime21 x64](../build/local/p3-20261003/atomic64/producers-runtime21-x64-meshtransport06/result.json)·[WoW64](../build/local/p3-20261003/atomic64/producers-runtime21-x86-meshtransport06/result.json)에서9 producer 경로 ×sample1/4의18 case 모두 full64 record·primitive count·pixel oracle를 통과했다. API+Shader Validation, owned backend/native compiler, exit0, validation/foreign 오류0이며 manifest는 `f340fe24d6ab6155feffa17ebd82a5b2a16b904764749fd7c24fd1401d4eb398`다. Fused VS/HS와 GS/DS compilation의 실제 module에도 phase 초기화·lowering을 적용하고, 후삽입된 private mesh capture load/store만 기존 transport metadata로 분류했다. 사용자 aggregate resource 연산을 면제하거나 producer oracle를 줄이지 않았다. Runtime19의 fused-phase 누락과 runtime20의 domain private-aggregate 실패는 원래 실패 기록으로 보존한다.

Typed64 전체 public oracle는 [runtime23 x64](../build/local/p3-20261003/atomic64/typed-runtime23-x64-volume-metadata08/result.json)·[WoW64](../build/local/p3-20261003/atomic64/typed-runtime23-x86-volume-metadata08/result.json)에서6 shape ×signed/unsigned12 조합의 operations·contention·snapshot,40 typed formats, ordinary32/float32 alias를 모두 통과했다. API+Shader Validation, owned backend/native compiler, exit0, validation/foreign 오류0이며 manifest는 `1b200462229a11215341c98cd808d3686a38e944d7ff258c271746bab15b4dac`다. Runtime22의3D pre-submit 정지는 volume header/residency용 내부 AS1 load가 사용자 UAV처럼 canonicalize되어 AIR가 비대해진 문제였다. 기존 private-transport metadata를 두 내부 load에 적용했으며 사용자 texel/buffer/atomic 연산과 oracle는 변경하지 않았다. 이 결과는 당시 cap0 진단 checkpoint이고 최종 양성 capability 검증과 구분한다.

Runtime24에서 실제 positive-cap 소비와 신규 used-binding/live-owner admission을 다음8 suite ×양 ABI로 검증했다. 모두 exit0·API+Shader Validation·owned native compiler/backend·validation/foreign 오류0이며 manifest는 `f63e0c689bc66106f362afd63924059dac45c5454a8eb0ea976eac5ca08e9657`다. Typed fixture는 cap1을 요구하고 과거 probe/negative-cap 우회는 제거했다. 이 표의 focused16회 통과와 별도 full-SM6 x64의 후속 raw compute stress fence timeout은 구분한다.

| 영역 | x64 | WoW64 | 실제 oracle |
|---|---|---|---|
| typed texture 전체 | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-typed-x64-01/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-typed-x86-01/result.json) | 6 shape ×2 sign·40 formats·ordinary32/float32 aliases |
| compute phase | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-phase-x64-01/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-phase-x86-01/result.json) | indirect/predication·cross-queue/shared fence·alias ordering |
| graphics producer | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-producers-x64-01/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-producers-x86-01/result.json) | 9 producer 경로 ×sample1/4의18 numerical cases |
| work-graph record64 | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-workgraphs-atomic64-x64-01/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-workgraphs-atomic64-x86-01/result.json) | CPU/GPU input32 records·384 qwords·old/final/descendant guards |
| work-graph typed2D | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-workgraphs-atomic64-typed-x64-01/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-workgraphs-atomic64-typed-x86-01/result.json) | 16 texels·48 full64 words·copy guards |
| coupled pixel | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-pixel-coupled-x64-02/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-pixel-coupled-x86-02/result.json) | ROV/non-ROV·typed/structured·pixel/4×sample·depth/discard/stencil/query40 cases |
| 4096 pixel contention | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-pixel-contention-x64-02/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-pixel-contention-x86-02/result.json) | ADD/CAS ×1/16 targets4 cases·full old-chain/final/guards/pixels |
| groupshared operations/stress | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-groupshared-x64-03/result.json) | [PASS](../build/local/p3-20261003/atomic64/positive-runtime24-atomic64-groupshared-x86-03/result.json) | cap1·9 operations·64 groups×1024 full64 return permutations/final |

Public graphics contention은 [runtime20 x64](../build/local/p3-20261003/atomic64/pixel-contention-runtime20-x64-01/result.json)·[WoW64](../build/local/p3-20261003/atomic64/pixel-contention-runtime20-x86-01/result.json)에서 ADD/CAS retry-loop ×1/16 target의4 case가 모두 통과했다. 각 case는4096 실제 fragment의 full64 returned-old multiset·final·prefix/suffix/unused guards·4096 RGBA pixels를 검사하며 CAS에는 soft retry cap이 없다. Typed-node와 이 public proof는 모두 API+Shader Validation, owned backend/native compiler, exit0, validation/foreign 오류0이며 runtime20 manifest는 `113bde598864ed08645d6ab198245b579c06516e71572e683ad32c20572789cc`다. 별도 native64×64 control의 read·mismatch-CAS·one-winner-CAS ×1/16 target6 case도 [stage logs](../build/local/p3-20261003/atomic64/bitmap64-stage-read-targets1-01.log)에서 lifetime/bitmap/guards·4096pixels를 통과했다. 이는 아래의 과거 반복-RMW native hang을 재실행하거나 그 결과를 삭제한 것이 아니다.

최신 production bitmap probe는 간접 접근하는 bitmap/node buffer의 read-write residency를 명시했다.64×64 full case의 [encoder09 full-once](../build/local/p3-20261003/atomic64/production-bitmap-encoder09-full-once.log)는 MTLCommandBufferErrorDomain Code2/IOGPU3의 실제 Hang이고, 앞선 Orca Helper recovery와 같은 시각의 InnocentVictim 관측만으로 설명하지 않는다.8×8 참여 규모의 [single-target](../build/local/p3-20261003/atomic64/production-bitmap-encoder09-tile8-stress-1.log)·[16-target](../build/local/p3-20261003/atomic64/production-bitmap-encoder09-tile8-stress-16.log)는 tiny/full/divergent 각각20회, full64·divergent32 반환과 reclamation/free-bitmap 검사를 API+Shader Validation에서 통과했다. 이는 public 전체 Atomic64 또는64×64 contention 통과가 아니다.

Public4096과 native64×64의 차이는 실제 admission이다. `RequiresMSAAExecution`은 atomic pixel shader를 captured-fragment 경로로 보내며, `EncodeMSAARender`는8×8 타일별 application pass를 Fragment fence로 순서화한다. `transformPixel`은32×32 내부 surface의[4,12)²에서만 사용자 body를 실행한다. 따라서4096은 누적 application fragment 수이지 동시 native 참여 수가 아니다. Constant delta·NoInline을 맞춘 [wide Metal4 control](../build/local/p3-20261003/atomic64/bitmap64-metal4-constant-targets1-01.log)도 실제 Hang이었다. 동일 shader·arena·64×64 viewport를 유지하고64개8×8 scissor/encoder와 Fragment fence만 적용한 [target1](../build/local/p3-20261003/atomic64/bitmap64-metal4-tiled-constant-targets1-01.log)·[target16](../build/local/p3-20261003/atomic64/bitmap64-metal4-tiled-constant-targets16-01.log)은 단일 초기화·단일 command buffer에서 누적4096 full64 returned-old chain·final·root/output guards·reclamation·free bitmap을 API+Shader Validation으로 모두 통과했다. Failed0, 최대 retry60/8, feedback error 없음이다. Native probe의1024 cap은 유지했으며 public CAS는 unbounded다. 이 비교는 실제 구현의 bounded/fence-ordered fragment admission을 뒷받침하며, wide native Hang을 삭제하거나4096 동시 참여의 안전성을 주장하지 않는다.

Current compiler09의 [typed first-CS](../build/local/p3-20261003/atomic64/typed-public/first-cs-production09-native.log)와 [ROV/non-ROV SAMPLE_APP](../build/local/p3-20261003/atomic64/rov-norov-production09-native.log)은 native PSO 생성·GPU submissions0의 compiler-only PASS다. Public helper11 fixture를 정식 atomic64-helpers suite에 연결하고 genuine5 DXIL과 소스 hash, ordered mode/sample-count, 실제 fixture mismatch와 terminal result를 검사한다. Runner의 기존 CPU diagnostics75 tests가 [PASS](../build/local/p3-20261003/atomic64/helpers-runner-diagnostics.log)했다. 첫 [public x64](../build/local/p3-20261003/atomic64/helpers-runtime09-x64-01/result.json)는 첫 VS-msaa capture PSO 준비 도중 E_FAIL로 끝났으며 draw/GPU numerical PASS가 아니다.

Actual ROV SAMPLE_APP capture(logicalS4/native1, variant262)에서 후삽입된 private PS-return aggregate store가 commonphase buffer width 검사에 들어간 진단을 해결했다. conditionalStore가 생성하는 정확한 private capture/output/coverage store에만 module phase flag 조건으로 dxil::kAtomic64Metadata를 부여하며, 기존 logical predicate/branch는 유지한다. 사용자 UAV store나 aggregate-width 자체를 면제하지 않는다. 수정 TU CPU compile exit0 후 같은 actual ROV APP capture의 LLVM verification PASS를 Main이 확인했다. 이 결과는 integrated public GPU/ROV ordering proof가 아니다.

통합 source의 partial-edge logical helper는 coverage scratch texture의 실제 AIR width/height로 내부 coverage read를 분기하고 join에서 coverage0으로 합류한다. 사용자 SRV/helper 값이나 common metadata 보호를 생략/대체하지 않으며, normal/replay variant에는 이 bounds 경로를 적용하지 않는다. 기존 compile database의 Darwin x64 flags로 msaa_pixel.cpp를 중앙 build와 별도 artifact에 CPU compile해 exit0을 기록했다(build/local/p3-20261003/atomic64/helpers-cpu-03/msaa_pixel-current-build.log). Central Ninja/install/stage 및 통합 public GPU proof는 Main의 다음 coherent cut에서 수행해야 한다.

Native11 physical proof를 소비하는 후속 production source cut은32×32/unclipped/evenoffset4/native1-per-logical-sample로 변경했다. Logical discard는 실제 native demotion 없이 metadata protection이 끝날 때까지 real lane을 보존하며, active/position ROV metadata consumer는 common converter와 함께 갱신했다. Public fixture는96-byte record의11 case로 확대해 genuine DXIL sample count/position, EvaluateAttributeAtSample, virtual SV_Position/center interpolants, QuadRead X/Y/diagonal, original sample index 및 기존 early-depth/discard/helper-inclusive/default-wave controls를 검사한다. x64/WoW64 PE와 genuine stable DXC의5개 container compile/validation은 승인받은 CPU 작업으로 모두 exit0이고 artifacts는 build/local/p3-20261003/atomic64/helpers-cpu-03/에 있다. 이 source cut에 full Ninja/public GPU를 실행하지 않았고 통합 atomic draw PASS로 기록하지 않는다.

후속 exclusive lease의 **`helpers-native-11/native-gpu-virtual-validated-01.log` native1-per-virtual4/8은16 case 모두 PASS(exit0/failures0)**였다(API/GPU validation enabled): full/sparse ×logical discard0/1 ×promoted/negative baseline이다. Promoted 각4×case의 metadata CAS256·logical140/120/4/0,8×case의 CAS512·logical280/240/8/0이 정확했고, 모든 promoted contributing quad의 native-helper neighbor0 및 ballot/quad/value/sample/derivative/side-effect fault0이었다. Extra helper observation288/576은 whole-helper extras이며 unsafe7×5 baseline은 mixed quad76/152( sparse4/8)로 구분됐다. 이 증거는 unclipped32×32/evenoffset4/native1 sample app의 물리적 helper promotion만 인정한다. Production07 source freeze 중이므로 실제 D3D12 captured interpolation·coverage·early/late depth/stencil·color/query replay와 common immutable atomic protocol을 통합한 public draw acceptance는 아직 별도 미완료다. GPU lease는 결과직후 반환했다.

`helpers-native-09/native-gpu-validated-01.log`의 unclipped/interior native1 candidate는 full/sparse ×logical discard0/1의 promoted4개 strict case를 모두 통과했다: metadata CAS64, logical35/30/1/0, mixed quad0, exact ballot/quad/value/sample/derivative/side-effect fault0이었다(API/GPU validation enabled). Native extra helper observation72는 whole-helper extras이며 contributing quad에는 helper가 없었다. 그 다음 기존 clipped native4 baseline에서 ballot140fault가 관측되어 첫 새 fault에서 종료했으므로 native4 candidate나 native1-per-virtual4/8은 아직 proof되지 않았다.

`helpers-native-11/`은 promoted strict case를 먼저 실행하고, bounded16-record exact ballot-mask 진단과 실패 classification(`strict_promotion_failure` exit1 / `negative_baseline_diagnostic` exit3)을 추가했다. `--virtual-only`는 아직 실행하지 않은 native1-per-virtual4/8의 full/sparse/discard 및 baseline controls만 검증하고, `--native4-only`/`--native8-only`는 별도 hardware-native sample frame을 구분한다. 어떤 promoted oracle도 완화하지 않았으며 모든 실패에서 다음 submit 없이 중단한다. Host arm64 compile은 exit0이고 MSL source는09 CPU compile 성공본과 동일하다.11 GPU 실행은 아직 없으며 production은07 freeze 중 변경하지 않았다.

`helpers-native-07/native-gpu-validated-01.log`의 bounded coordinate 진단도 첫 promoted native1 case에서 exit1/failures16 후 중단했다. 실패 neighbor는 tile 밖이 아니라 내부였다: 예를 들어(2,0)의 native-helper peer(2,1)는 post-filter SRV 값0, derivativeY=-2였고(4,2)/(6,4)에도 같은 대각선 pattern이 있었다. 모든 좌표의 별도 real invocation/CAS가 존재해도 같은 application quad의 필요한 peer를 대신하지 못한다. Oversized fullscreen triangle의 clipping으로 생기는 internal fan이 원인이라는 것은 **[INFERENCE]**이며 아직 production 변경 근거로 인정하지 않는다.

다음 CPU-only candidate는 `helpers-native-09/`이다:32×32 attachmentless surface의 clip 내부 세 vertex(-1,+1),(+1,+1),(-1,-1)로 단일 unclipped triangle을 만들고, quad-aligned physical offset(4,4)의 virtual8×8 tile만 protocol/app에 진입시킨다. SV_Position에서offset을 빼며 세 edge에서 tile 전체가 떨어져 있고 모든 exact ballot/quad/sample/derivative/metadata/user-write/discard oracle를 그대로 유지한다. Arm64 및 MSL compile-only 모두 exit0, GPU submit 없음이다. `helpers-native-08/`는 ambiguous patch 실패 뒤 생성된 이전 diagnostic source의 CPU artifact이지 unclipped candidate가 아니다. Production nativeS 경로는 아직 수정하지 않았으며 genuine candidate proof 전 feature acceptance는 계속 gated다.

후속 native GPU 관측은 PASS가 아니다. 최초 `helpers-cpu-02/native-gpu-01.log`는 MSL reserved identifier `vertex` 때문에 shader library compile에서 exit2였으며 GPU submit은 없었다. Test source rename 후 exclusive lease의 `helpers-native-04/native-gpu-01.log`는 실제 native1 baseline7×5 metadata CAS35, promoted8×8 CAS64를 기록했지만 contributing SIMD wave에 native helper extras가 남았다(helper observation483/342, promoted pre-filter population16..32). 잘못된 zero-extra-helper/고정32 oracle 때문에 첫 promoted case에서 exit1(failures128) 후 중단했고 lease는 즉시 반환했다. 이것만으로 quad completeness나 wave correctness를 인정하지 않는다.

`helpers-native-06/`의 수정 probe는 production과 같은 single triangle·attachmentless tile 및 `inTile && !nativeHelper` filter/discard를 사용한다. Filter 전후 physical/active/logical-helper **정확한 ballot**과 partition/popcount를 비교하고, contributing 각 lane의 세 quad neighbor native-helper flag·좌표·sample·SRV 값·logical helper 상태, derivative1/10, metadata CAS 및 logical/discard user-write mask를 검증한다. Baseline mixed native-helper quad와 promoted mixed quad를 구분하며, extra full-helper quad가 같은 SIMD wave에 존재하는 것은 성공도 실패도 단독으로 결정하지 않는다. Native1/4/8 및 native1-per-virtual4/8 control, sparse coverage와 logical discard를 보존한다. Arm64 compile 및 `--compile-only` MSL library CPU compile 모두 exit0이며 후자는 command queue 생성 전에 종료했다. 후속 exclusive lease에서 `helpers-native-06/native-gpu-validated-01.log`는 API/GPU validation enabled를 기록했으며 첫 promoted native1 case에서 exit1/failures16으로 중단했다. Exact ballot과 side-effect oracle는 각각0fault였지만 **contributing mixed native-helper quad16개/quad fault16개**였다. 실제8×8 surface만으로 required helper values를 보호하는 데 충분하지 않으므로 이 경로는 acceptance 미완료이며 public atomic draw는 미실행이다. `helpers-native-07/`는 최대16개 실패 record의 neighbor 좌표/값/flag/sample/derivative 진단 readback을 추가했고 arm64/MSL CPU compile은 exit0이나 아직 GPU 미실행이다. 현재 production은 nativeS application pass이며 native1-per-virtual-sample 전환은 genuine 전체 proof 전에는 하지 않는다.

후속 CPU proof는 `build/local/p3-20261003/atomic64/helpers-cpu-02/`에 기록했다. Native arm64 probe와 x64/WoW64 public fixture compile은 각각 exit0이고 genuine stable DXC1.9.2609 producer의5개 VS/PS container 생성·validation도 exit0이다. 최초 producer의 HLSL reserved identifier `linear` 오류와 fixture의 `sm6-` filename prefix 오류를 test source에서 수정했다. Native probe는 native1-per-virtual-sample4/8 control 및 contributing SIMD-wave 전체의 native/helper/logical population을 추가했으며, public oracle는 `WaveOpsIncludeHelperLanes`의 sparse/partial-edge helper 참여와 기본 wave의 helper 제외를 구분한다. Native8 sample 미지원은 별도 `UNSUPPORTED`로 출력하며 virtual8/native1 proof와 합치지 않는다. **CPU build는 GPU·physical-helper PASS가 아니다. Production source는 중앙 cut06 freeze 동안 수정하지 않았다.**

Atomic phase의 MSAA pixel/sample application role은 기존 producer capture를 재사용하고, 실제 attachmentless32×32 surface의 unclipped triangle 내부 physical(4,4)에 있는 quad-aligned virtual8×8 tile에서 실행한다. Original tile origin+native position−4로 SV_Position을 복원하고, guard-band의 negative virtual position은 float-to-uint 변환 전에 native float bounds로 제외한다. Sample app는 logical sample마다 native1 encoder를 한 번 실행하며 tile.sample_base가 original SV_SampleIndex·captured sample interpolation/position·coverage bit·pixel×sample output record를 선택한다. GetRenderTargetSampleCount/Position도 original sample count 및 저장한 programmable 위치를 사용한다. Application VS/HS/DS/GS/AS/MS를 다시 실행하지 않고 기존 captured-primitive replay의 color/depth/stencil/sample mask/alpha·predication 및 query fanout 경로를 유지한다. Atomic app의 ROV scratch는 native1 texture 하나만 쓰고 새 logical_position call-operand metadata로 padded native좌표 대신 virtual original좌표를 사용한다. Normal/D3D11 pipeline/target/vertex는 바꾸지 않는다.

Application discard는 logical demotion이고 IsHelperLane 및 기존 logical wave helper exclusion도 이 상태를 소비한다. User UAV 효과는 logical alive 조건으로 whole-operation guard하고, `dxmt.atomic64.metadata` read protection은 그 guard에서 제외한다. Phase update retry wrapper는 guard 전에 inline하지 않는다. Native extra all-helper quads는 wrapper에서 native helper predicate로 application body 진입 전에 제외하므로 suppressed hardware helper CAS에 ordinary SRV read protection을 맡기지 않는다. Normal/D3D11 role에 promotion bit를 설정하지 않는다.

CPU-ready public oracle는 `tests/owned-d3d12-atomic64-helpers.cpp`와 genuine DXC producer `tests/dxil/build-atomic64-helpers.sh`다.7×5 edge,4× sample frequency, sparse last-pixel scissor, early-depth rejection, discard, protected SRV fine/coarse derivatives, quad helper values, logical helper flags 및 UAV64 old/exactly-once 값을 확인한다. `tests/native-atomic64-helpers.mm`은 native7×5/sparse baseline과 실제8×8 promoted surface의1/4/8 samples별 physical metadata CAS writes·helper observations·logical coverage를 비교하는 별도 native probe다. **이 source cut에서는 build/GPU를 실행하지 않았고 PASS로 기록하지 않는다.** 중앙 full build/install/staging 후 exclusive GPU lease에서 native probe와 양 ABI public oracle를 실행해야 실제 physical-helper proof가 된다.

| 기록 | 내용 |
|---|---|
| [기준표](#p0에서-고정한-규격backend-기준표) | P0 publication 조회 시점·OS/GPU/toolchain, 당시 단계 계약 |
| [P0](#10-p0-실행-결과와-재현-근거) | resource·recording·shader·query·queue 기반 및 실패→수정 관찰 |
| [P1](#p1-완료-판정과-남은-계약) | API·presentation·statistics·SO 완료 및 상세 계약 |
| [P2](#p2-완료-결과-2026-10-01) | SM·bindless·barriers·tiled·library·sharing·GS/TS |
| [P3](#p3-완료-결과-2026-10-01) | FL12_1·DXR·mesh·VRS·feedback·WG의 지정 proof |
| [P4](#p4-진행-상태) | Metal 4 recovery mixed-cut acceptance·UMA·ARM64 SDK/plugin |
| [Full SR/FG port](#full-srfg-port--2026-10-02) | Owned native SR·FrameInterpolator·MIT SDK proxy와 실제 acceptance 경계 |
| [후속 P0 공개 계약](#후속-p0-공개-계약--2026-10-02) | 안전한 보호 알림 미지원·정확한 LUID·Resource1/2/Heap1·선택 feature/discovery와 양 ABI consumer proof |
| [후속 P1 범위 결정](#후속-p1-범위-결정과-reference--2026-10-02) | 제한된 확장·NGX accept+ignore·memory 사용자 제외, source/reference와 새 실행 acceptance의 분리 |
| [새 P3 #17](#새-p3-17-meshas-derivatives--2026-10-03) | Paired runtime-01의 양 ABI quad·수치·capability 및 기존 mesh/cache proof |
| [새 P3 #23](#새-p3-23-wave-size-확장--2026-10-03) | Paired runtime-02의 min4/max32·기본32, 양 ABI waves24·NodeWave4 work-graphs9 public GPU proof |
| [새 P3 #22 FP64](#새-p3-22-fp64--2026-10-03) | Exact integer emulation·double cap TRUE, runtime-04 양 ABI DXIL/DXBC compute·VS/PS·wave8-case public proof |
| [새 P3 #18 native vector](#새-p3-18-native-vector--2026-10-03) | Genuine SM6.9/DXIL1.9, runtime-04 양 ABI32-case public readback 및 CPU28 boundary/lowering checks |
| [D3D11 조사](#d3d11-변경-조사와-유지-방침) | 2026-09-29 조사 snapshot과 변경 유지 결정 |
| [새 P3 #19 HitObject](#새-p3-19-hitobject-subset--2026-10-03)·[OMM closeout](#새-p3-19-omm-closeout--2026-10-03) | Runtime-06 양 ABI full DXR·HitObject280+SER512 memory words, OMM modern56,206+legacy3,604, Tier1.2/OPTIONS22/RTAS0 admission으로 전체 #19 완료; 실제 scheduling 가속은 보고하지 않음 |

### 새 P3 #18 native vector — 2026-10-03

Final DXC1.9.2609.5의 genuine SM6.9/DXIL1.9를 소비했다. Native vector2–1024와 허용된 library singleton, helper inline·SSA/phi/dynamic indexing·byte-layout 및 elementwise/wave intrinsic을 scalar AIR로 낮추고 RawBufferVectorLoad/Store303/304와 reductions309–311을 연결했다. Metadata 숫자만 올리거나 proprietary converter를 production fallback으로 쓰지 않았다.

Paired runtime-04의 [x64](../build/local/p3-20261003/vectors/gpu-x64-04/result.json)·[WoW64](../build/local/p3-20261003/vectors/gpu-x86-04/result.json)는 각각32 ordered cases·exit0/pass=true·API+Shader Validation·owned native compiler verified·validation/foreign backend 오류0이다. 동일 manifest SHA256는 **f5f5aa08617aef02a416a48867ad8fdee3ddbe659c7fefa676660a0dce0f8f93**다. Raw/group-shared/helper/wave5·1024-vector4·denormal1·loop/branch/phi8·double/uint64/half 각1·heap3·structured3·VS/PS1·sparse tile/status4의 실제 readback을 독립 host oracle와 비교했다. GPU 실행은 exclusive·양 ABI 직렬이고 소유 prefix의 stop/wait를 완료했다.

[CPU28 checks](../build/local/p3-20261003/vectors/vector-contract.log)는 native-vector ABI/version/shape rejection과 i8/i1 wave legalization을 검증한 별도 CPU domain이다. 최초23-case 양 ABI runtime-01 proof를 최종32-case proof로 소급 바꾸지 않았다. Legacy DXR aggregate를 SM6.9 overload로 오분류했던 실패와 classifier 수정 후 runtime-02 full DXR 양 ABI 회귀는 [HitObject 이력](#새-p3-19-hitobject-subset--2026-10-03)에 보존한다.

이 #18 완료는 compiler 입력6.9와 공개보고6.5의 구분을 유지한다. Mandatory atomic을 포함한 전체 SM/FL·matrix preview·다른 GPU/OS·게임·성능·D3D11·ARM64/FEX guest 성공으로 확대하지 않는다. [현행 계약](CORE-IMPLEMENTATION-STATUS.ko.md#native-vector-완료와-focused-proof)·[재현 recipe](SMOKE-TEST.ko.md#native-vector-focused-proof)를 따른다.

### 새 P3 #22 FP64 — 2026-10-03

Metal native double ALU 부재를 미구현의 근거로 삼지 않고 Berkeley SoftFloat 3e 정수 subset을 owned AIR에 연결했다. AIR의 `long long`은128-bit라 초기 helper가 잘못된 ABI로 생성됐고, constant lookup table도 AS0여서 결과를 기록하지 못했다. 실제64-bit typedef와 AS2 constant로 수정한 뒤 [native-proof-constant.log](../build/local/p3-20261003/fp64/native-proof-constant.log)에 CPU720896·Metal compute720896·graphics1024 exact checks PASS를 기록했다. DXIL representation을 double→i64로 remap하면서 constant·vector·aggregate·SSA·memory·function/typed attribute·named local metadata를 함께 변환했고, binary32 변환의 preserve/ftz도 보존했다.

D3D12 public double cap을 올리기 전 genuine DXBC5.0/5.1의19개 double opcode lowering을 추가했다. Packed xy/zw registers, source swizzle/abs/neg, destination mask, comparisons·saturating conversions, final helper link를 구현했다. Wine의 HLSL compiler 대신 **Microsoft.Windows.SDK.CPP 10.0.28000.2705의 native FXC/D3DCompiler_47**로 CS/VS/PS6개를 만들었고 최종 AIR conversion을 통과했다. Public VS의 DFMA는 runtime VertexID sign bit를 소비하므로 compile-time constant 결과만으로 graphics oracle를 통과시키지 않는다.

- Runtime-03은 cap `double=0` 상태의 첫8-case 양 ABI behavior proof였다. `native_compiler_proof`를 success에 사용했지만 serialized result field의 local assignment를 빠뜨려 `dxil_native_compiler_verified=false`로 기록된 reporting 오류를 수정했다. 이 기존 JSON은 소급 덮어쓰지 않았다.
- 최종 공개 `DoublePrecisionFloatShaderOps=TRUE` 후 **paired runtime-04**의 [x64](../build/local/p3-20261003/fp64/public-runtime04-x64/result.json)·[WoW64](../build/local/p3-20261003/fp64/public-runtime04-x86/result.json)는 ABI별8 case·exit0·`double=1`·API+Shader Validation·`dxil_native_compiler_verified=true`·validation/foreign backend 오류0을 기록했다. 동일 runtime manifest SHA256는 **f5f5aa08617aef02a416a48867ad8fdee3ddbe659c7fefa676660a0dce0f8f93**다.
- Case는 DXIL preserve/ftz compute2개, genuine FXC DXBC5.0/5.1 compute2개, signed-zero/NaN numeric equality·IEEE reductions·bit-preserving shuffle의4-lane wave1개, 실제 DXIL·DXBC5.0/5.1 VS/PS graphics3개다. Compute는 각1024 edge/random 입력, graphics는64 pixels를 독립 host double oracle와 비교한다.

전용 prefix는 stop/wait했고 GPU 실행은 양 ABI 직렬·exclusive였다. [현행 계약](CORE-IMPLEMENTATION-STATUS.ko.md#fp64-완료와-focused-proof)·[SMOKE recipe](SMOKE-TEST.ko.md#fp64-dxil과-dxbc-focused-proof)를 따른다. 과거 FP64 rejection·native helper 실패 기록을 보존하며 전체 SM/FL·다른 GPU/OS·성능·게임·D3D11 cap·ARM64/FEX guest 성공으로 확대하지 않는다.

#### 다중 AIR export의 helper linkage 수정 — 2026-10-03

확장 atomic RT의 native PSO 생성은 `dxmt.fp64.to.f32` 중복 정의로 실패했다. 공통 helper linker에서 LLVM이 실제로 가져온 정의만 module-private으로 바꾸고 사용자 export와 외부 선언은 유지했다. 독립적으로 FP64 lowering한 두 export를 합치는 CPU 회귀와 기존 atomic phase 회귀가 [2/2 PASS](../build/local/p3-20261003/phase-fp64-regression09.log)했다. 같은 17-export·22-function RT 입력의 [native Metal4 PSO 생성](../build/local/p3-20261003/hitobject/atomic-rt/internalized09-native-pso.log)도 API+Shader Validation에서 통과했다. 이는 GPU 제출0인 native link proof이며 #15의 실제 RT 실행·전체 atomic 진행성 완료는 아니다.

#### SoftFloat 3e 배포 고지

정수 core의 원래 저작권·3-clause BSD 조건은 `src/airconv/shaders/fp64_integer.h`에 보존했다. 이 core가 들어간 binary 배포에도 다음 고지를 함께 제공한다.

```text
This C header file is part of the SoftFloat IEEE Floating-Point Arithmetic
Package, Release 3e, by John R. Hauser.

Copyright 2011, 2012, 2013, 2014, 2015, 2016, 2017 The Regents of the
University of California.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions, and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions, and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

 3. Neither the name of the University nor the names of its contributors may
    be used to endorse or promote products derived from this software without
    specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS "AS IS", AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ARE
DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### 새 P3 #21 Writable MSAA — 2026-10-03

후속 paired runtime-07의 [x64](../build/local/p3-20261003/writable-msaa/runtime07-x64-01/result.json)·[WoW64](../build/local/p3-20261003/writable-msaa/runtime07-x86-01/result.json)는 readonly depth-MSAA cast 통합에 따른 shared constructor·Append·residency 변경 뒤 기존80-case suite를 API+Shader Validation으로 각각 PASS했다. Cap1·exit0·native compiler 확인·validation/foreign-backend 오류0이다. Runtime-06 최초 완료와 이 실제 affected-path 회귀를 구분하며 새 depth-cast proof로 합산하지 않는다.

Paired immutable `p3-20261003/runtime-06`의 [x64](../build/local/p3-20261003/writable-msaa/runtime06-x64-01/result.json)·[WoW64](../build/local/p3-20261003/writable-msaa/runtime06-x86-01/result.json)는 **OPTIONS14 WriteableMSAATexturesSupported=TRUE, ordered80 cases, exit0, API+Shader Validation, owned native compiler, validation_errors=[]**를 기록했다. Official SM6.7 DXC42 modules와 실제 public D3D12 consumer를 freeze/hash했다. Sample별 shadow 접근과 raw canonical GPU synchronization으로 UINT/SINT/FLOAT·typed/typeless load, native copy/raster/resolve 및 clear/bounds/null 의미를 연결했다. [CORE의 정확한 범위](CORE-IMPLEMENTATION-STATUS.ko.md#writable-msaa-완료와-focused-proof)를 따른다.

수정 전 [runtime-05 x64](../build/local/p3-20261003/writable-msaa/runtime05-x64-01/result.json)는 일부 readback을 통과한 뒤 mip 없는 DXIL GetDimensions의 undef→physical mip 전달로 width/height0, 잘못된 single-sample destination의 inherited MSAA alignment로 E_INVALIDARG를 관찰했다. Omitted mip은0으로 lowering하고 fixture의 destination alignment를0으로 수정했다. Native validation 또는 resource validation을 끄거나 완화하지 않았고 실패 결과를 보존했다. CPU/native-only probe는 이 end-to-end80-case 완료에 합산하지 않는다. D3D11·reserved MSAA·전체 SM/FL·다른 GPU/OS·게임·성능은 별도다.

### 후속 P0 공개 계약 — 2026-10-02

Docs-first 우선순위 commit **5f88633** 뒤 P0-1–4를 병렬 수정하고 새 **p0-contracts-r5KJ0O/runtime-01**로 닫았다. 과거 P0 개발 단계와 다른 묶음이다. 현재 계약·정확한 결과 경로·검증 한계는 [CORE P0 ledger](CORE-IMPLEMENTATION-STATUS.ko.md#p0-공개-계약-정합), 반복 절차는 [SMOKE](SMOKE-TEST.ko.md#p0-공개-계약-fixture-수동-실행)를 따른다.

- 변경 전: P0-1 public Register assert 중단, P0-2 없는 LUID에 adapter0 성공 반환, P0-3 Resource1 QI E_NOINTERFACE, P0-4 core 양 ABI62 failures·NGX nonowned adapter1 failure를 보존했다. Static finding·실제 CPU 재현·GPU oracle correction을 구분한다.
- 변경 후: P0-1 양 ABI event/cookie/ownership CPU PASS, P0-2 양 ABI5 groups·adapter1 CPU PASS, P0-3 양 ABI31 descriptor/creation/shared/GPU cases PASS, P0-4 core 양 ABI80 checks·NGX x64-only12 checks PASS. 실제 selected38 Feature IDs와 기존 private OPTIONS21을 대조했으며 최신 SDK 전체 enum 지원을 선언하지 않았다.
- Main의 같은 runtime VRS 각146·sharing 각20 cases를 x64/x86 **API+Shader Validation**으로 다시 실행했다. OPTIONS10 true/true·ExistingHeaps true는 기존 실제 GPU consumer와 맞춘 보고이며 새 VRS/heap 기능을 구현한 것으로 기록하지 않는다. Manifest SHA256 **a2f635535aa52e08abf2c3a3a31819626142f42c4bfad910aa056fe7e97b86f0**와 build/install/stage 로그를 보존했다.
- P0-3 after02 실패는 enhanced-layout texture에 COMMON handoff 없이 legacy barrier를 쓴 **잘못된 fixture interop**이다. [공식 계약](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html#interop-with-legacy-resourcebarrier)에 따라 APIs3/6/9의 fixture를 enhanced COPY_DEST→COPY_SOURCE로 수정했고 readback을 유지한 after03 양 ABI가 통과했다. Production source는 바뀌지 않았으며 실패 로그를 삭제하거나 production barrier 결함으로 바꾸지 않았다.
- Resource2 runtime의 explicit WIDL sret과 양 ABI public GetDesc1() 호출을 실제 연결했다. Null-output HRESULT는 프로젝트 안전 경계이며 native-Windows differential proof가 아니다. Native protection·native no-temporal 장치·multi-GPU·다른 GPU/OS·public NGX x86·게임/전체 conformance는 별도 한계다. ARM64/FEX guest는 사용자 보류를 유지했다.
- 시험 소유 prefix만 stop/wait/remove했다. 최신 P0 runtime-01과 비교 full-metalfx-port-20261002/runtime-02는 유지한다. 오래된 parallel-metalfx-cache-20261002/runtime-after의 bin/lib/share payload는 Main이 디스크 정책에 따라 정리하며 manifest/provenance/기존 증거는 보존한다. 이 payload 정리는 과거 실행 결과의 취소나 새 실행이 아니다.

### 후속 P1 범위 결정과 reference — 2026-10-02

P0의 frozen source-only commit **8a72292e047a43e3577296d28a578a0eb5566313** 뒤 새 backlog P1을 구현했다. 사용자 요청으로 병렬 작업을 중단하고 sample positions → samplers → FG 순차 GPU 검증으로 #5–9·#11을 완료했다. 이전 동명 P1 개발 단계와 별개다. 현행 scope와 identity는 [CORE P1](CORE-IMPLEMENTATION-STATUS.ko.md#p1-계약-범위와-reference)를 따른다.

- 사용자는 #10 SR/FG memory query 구현을 제외하고 기존 SR NO_PROVIDER·FG zero/unreported success 유지에 동의했다. Pristine native NGX Stats의 실제 zero-success는 확인됐지만 이를 FFX memory 지원·실제 총량 보고로 바꾸지 않는다.
- #7 Exposure.Scale과 #8 MV.Offset은 GPTK처럼 유효한 비기본 입력을 받아 계속 실행하되 수치 효과는 무시하는 선택이다. Pristine4.0b2 SHA f5b56…의 MPL 미read·legacy warning+continue·MV getter 부재는 [binary reference](../build/local/p1-contracts-tge72c/d3dmetal-reference/result.json)이며 NVIDIA 수식이나 GPU differential proof가 아니다. 설치1075…/core-base125f…의 최초 identity 오판은 최종 reference에서 수정했고 이전 조사 artifact도 보존했다.
- Immutable P0 runtime-01의 before failures와 새 `p1-contracts-tge72c` candidate의 after를 별도 보존한다. 소스·fixture가 준비된 사실, 컴파일, native reference 관찰을 새 공개 API/GPU/replay PASS로 승격하지 않는다.
- 완료 증거는 CORE의 runtime별12개 GPU/public run과 diagnostics67·staging31에 연결했다. 최신 `p1-contracts-tge72c/runtime-03`과 비교 `p0-contracts-r5KJ0O/runtime-01`을 유지한다. 종료한 시험 prefix 및 구 P1 runtime-01/02·full-metalfx-port runtime-02의 bin/lib/share payload만 정리했고 manifest·provenance·실패/성공 로그·drawable captures는 보존했다.

### 새 P3 #17 Mesh/AS derivatives — 2026-10-03

새 backlog #17은 paired `p3-20261003/runtime-01`(manifest SHA256 **2ab82529f066ab6fc58c9cf36c3fa3c97398642026b41a1d903cf23c310cfff2**)의 full mesh x64/WoW64 양 ABI에서 **OPTIONS9 derivatives=1·API+Shader Validation PASS·exit0·validation/foreign-backend 오류0·owned native compiler 확인**으로 닫았다. Six MS/AS1D·2D·3D positive cases의 ABI별320 lanes에서2560 identity·5120 numerical words, quad·f32/f16 derivative·implicit sampling/LOD·실제 payload/render output과 invalid-shape rejection을 검사했다. 기존 mesh suite 및 같은 cut의 양 ABI pipeline-cache 회귀도 PASS다. [정확한 결과 파일·oracle·한계](CORE-IMPLEMENTATION-STATUS.ko.md#mesh-as-derivatives-완료와-focused-proof)는 CORE에 있다. 이전 x64-only 중간 관찰은 그대로 보존하며 **공개 SM6.5/FL12_1 유지**, 다른 P3·전체 conformance·ARM64/FEX guest 완료를 선언하지 않는다.

### 새 P3 #23 Wave size 확장 — 2026-10-03

새 backlog #23은 paired `p3-20261003/runtime-02`(manifest SHA256 **a2708cd565c32a40a6e359467e3f175b35524a50d16a0ef753f48ff87b46988e**)의 public x64/WoW64 GPU consumer로 닫았다. 보고 폭은 **min4/max32·기본32**이며 native SIMD32 안의 virtual4/8/16 실행을 구현했다. 필수64/128 및64–128 전용 range는 **E_FAIL·null PSO**로 명시적 거부한다. Flexible range의 preferred64가 unavailable일 때는 그 range가32를 허용할 때만32를 선택한다.

- [Waves x64](../build/local/p3-20261003/waves/gpu-x64-02/result.json)·[WoW64](../build/local/p3-20261003/waves/gpu-x86-02/result.json)는 ABI별24 case: fixed4/8/16/32·SM6.8 ranges/preferred·divergent ballot/reduction/prefix/Match·i64 및 required64/128 거부, 기존32 quad/helper/VS 회귀를 통과했다.
- [WorkGraphs x64](../build/local/p3-20261003/waves/node-gpu-x64-02/result.json)·[WoW64](../build/local/p3-20261003/waves/node-gpu-x86-02/result.json)는 ABI별9 case: legal coalescing node의 WaveSize4 count4·sum6·ballot0xf·next-lane shuffle을 기존 정확한 record oracle에 연결했다.
- 네 실행은 모두 exit0·PASS·**API+Shader Validation**, `validation_errors=[]`, `foreign_backend_loads=[]`, `dxil_native_compiler_verified=true`다. Exclusive window에서 직렬 실행했고 네 전용 prefix를 stop/wait/remove했다.
- 초기 runtime-01 wave run은19 positive case가 통과했지만 fixture가 기존 shader-initialization E_FAIL을 E_INVALIDARG로 잘못 기대했고 frozen helper VS 입력이 빠져 실패했다. HRESULT expectation과 `vs_tiny` snapshot을 수정한 위 runtime-02 네 실행이 최종 proof이며, 이 초기 실패를 production GPU hang으로 바꾸지 않는다.

[현행 계약·한계](CORE-IMPLEMENTATION-STATUS.ko.md#wave-size-확장-완료와-focused-proof)와 [SMOKE recipe](SMOKE-TEST.ko.md#wave-size와-nodewave4-focused-proof)를 따른다. 아래 과거 P2 checkpoint의 wave32-only·32를 허용하지 않는 WaveSize 거부 기록은 당시 범위로 보존하며 소급 수정하지 않는다. Cross-SIMD64/128·전체 SM/FL·FP64/vector/matrix·다른 GPU/OS·게임·성능·ARM64/FEX guest proof로 확대하지 않는다.

### 새 P3 #19 HitObject subset — 2026-10-03

Final upstream DXC `lib_6_9`의 HitObject opcode262–289를 기존 owned DXR lowering에 연결했다. HitObject는 working-frame pointer가 아닌 immutable aggregate이며, TraceRay의 실제 traversal/any-hit/intersection과 deferred closest-hit/miss Invoke, query committed snapshot, 재바인딩·현재 payload·SBT/local-root·ray/transform getters를 공유 runtime 계약에 연결한다. 규격상 허용된 `MaybeReorderThread` no-op은 성능상 thread reordering 구현을 뜻하지 않는다.

- Native 소비에서 발견된 non-opaque `%dx.types.HitObject` 재정의 assertion은 value/type remap으로, `dxmt.rt.locals`의 LocalAsMetadata assertion은 불필요한 metadata remap 제거로 고쳤다. 이후 [실제 native AIR/MTLLibrary/linked pipeline](../build/local/p3-20261003/hitobject/consumer-run.log)이 `PIPELINE_PASS`였다.
- Immutable `p3-20261003/runtime-02`의 focused [x64](../build/local/p3-20261003/hitobject/public-runtime02-x64/consumer.log)·[WoW64](../build/local/p3-20261003/hitobject/public-runtime02-x86/consumer.log)는4 raygen·ABI별280 exact word와 HitObject PASS, exit0다. 같은 cut의 [full DXR x64](../build/local/p3-20261003/hitobject/full-runtime02-x64/consumer.log)·[WoW64](../build/local/p3-20261003/hitobject/full-runtime02-x86/consumer.log)도 기존 geometry/state-object/16 AS/all-stage RayQuery/root/SBT/coverage/generic linkage와 새 HitObject를 함께 통과했다. 네 실행은 exclusive window에서 직렬로 Metal API+Shader Validation을 켰으며 Metal validation fault를 관찰하지 않았다.
- 최초 runtime-01의 [legacy regression 로그](../build/local/p3-20261003/hitobject/legacy-runtime01.log)는 base DXR readback 뒤 ordinary vector의 잘못된 SM6.9 native-vector 분류로 contract state-object가 거부된 기록이다. Vector classifier 수정 뒤의 위 runtime-02 full proof로 통과했으며, 실패를 은폐하거나 기존 DXR1.1 미완료 finding으로 재분류하지 않는다.
- 당시 genuine [GPU 입력](../build/local/p3-20261003/hitobject/raytracing-hitobject.dxil)과 [frozen hash](../build/local/p3-20261003/hitobject/frozen-input-sha256.txt)를 보존했다. 당시 default full ray suite의 정확히1개의 HitObject PASS·ordered case gate와 [실제 양 ABI capture mutation](../build/local/p3-20261003/hitobject/strict-captured-predicate.log)·[21 ray diagnostics](../build/local/p3-20261003/hitobject/strict-predicate-tests.log)는 missing/duplicate/FAIL를 거부했다. 이후 REORDER_SCOPE memory edge가 추가된 현행 frozen 입력과 이 historical280-word proof의 바이너리를 혼동하지 않는다.

**Runtime-02 당시에는 #19 전체를 닫지 않았다.** 이후 pre-SM6.9 legacy OMM opt-in 경계와 OMM AS/classifier/flag matrix는 runtime-04의 별도 양 ABI public acceptance로 완료됐다. 다음 OMM closeout 증거를 따른다.

### 새 P3 #19 OMM closeout — 2026-10-03

SDK1.619.5 extension ABI, ordered native-free OMM array shadow, OC1 software classification과 TraceRay/HitObject/RayQuery opt-in, index/base/special·ray/instance flags, tools decode 및 array/BLAS lifecycle을 연결했다. Metal compacted-size8-byte write의 OMM marker clobber를 ordered restore로 고쳤고, mixed ordinary+OMM BLAS export는 nonnull array reference만 담아 exact postbuild size와 public relocation을 보존한다.

Immutable runtime-04의 [x64](../build/local/p3-20261003/omm/public-runtime04-x64/result.json)·[WoW64](../build/local/p3-20261003/omm/public-runtime04-x86/result.json) official runner는 각각 modern56,206+legacy3,604 comparisons,27 ordered subdivision cases, exit0/pass=true, API+Shader Validation, owned native compiler load verified·foreign backend 없음·validation_errors=[]다. [runtime-03 실패](../build/local/p3-20261003/omm/runs-03-x64-b/consumer.log)와 [실제 capture mutation16 CPU checks](../build/local/p3-20261003/omm/strict-captured-predicate.log)를 함께 보존했다. HitObject/full DXR와 함께 **OMM·HitObject public proof 완료**로 분류한다. Runtime-04의 tier1.1 보고 및 OPTIONS22 query 부재는 다음 runtime-06 공개 cap/admission proof로 정합화했다. 별도 genuine REORDER_SCOPE/reordercoherent SER consumer acceptance와 Main의 최종 판단 전에는 #19 전체 종료로 세지 않으며, 지정 sample/GPU/OS 경계나 공개 전체 SM/FL 및 성능 경계를 없애지는 않는다. [현행 계약](CORE-IMPLEMENTATION-STATUS.ko.md#opacity-micromap-완료와-public-gpu-proof)·[재현 recipe](SMOKE-TEST.ko.md#opacity-micromap-public-acceptance)를 따른다.

새 immutable runtime-06의 [x64](../build/local/p3-20261003/omm/public-runtime06-x64/result.json)·[WoW64](../build/local/p3-20261003/omm/public-runtime06-x86/result.json) official runner는 두 ABI 모두 tier12·OPTIONS22 정확한16-byte query/8개 rejection·canary, real OMM array/mixed BLAS RTAS0 driver matching 및 null/type/version rejection, modern56,206+legacy3,604/27 ordered cases를 통과했다. Exit0/pass=true, API+Shader Validation, staged owned native compiler verified, foreign backend 없음·validation_errors=[]이며 runtime manifest SHA256는 **7b7e3073cba13d88bfea91d9773a07287030f59dd619175672fcd305bc041ac7**다. 두 전용 prefix의 stop/wait를 완료했다. [최종 production predicate capture mutation24 CPU checks](../build/local/p3-20261003/omm/strict-runtime06-captured-predicate.log)는 두 실제 ABI capture에 missing/duplicate PASS·legacy/level/relocation·cap ABI/hint·driver admission·native compiler 및 explicit failure 변형을 적용했으며 GPU 제출은0개다.

### 새 P3 #19 전체 SER memory closeout — 2026-10-03

**Runtime-06 최종 SER memory consumer:** [x64](../build/local/p3-20261003/hitobject/coherence/public-runtime06-long-x64/result.json)·[WoW64](../build/local/p3-20261003/hitobject/coherence/public-runtime06-long-x86/result.json) official runner는 기존 full DXR·280-word HitObject와 새64-lane×8=512-word oracle를 모두 통과했다. 각 결과는 exit0/pass=true, API+Shader Validation, staged owned native compiler verified, validation_errors=[]·foreign_backend_loads=[]다. Manifest SHA256는 **7b7e3073cba13d88bfea91d9773a07287030f59dd619175672fcd305bc041ac7**이며 두 ABI를 직렬 실행하고 prefix를 stop/wait/remove했다.

실제 reordercoherent UAV에 lane별 두 message를 쓰고 Barrier(UAV_MEMORY,REORDER_SCOPE)와 세 MaybeReorderThread overload를 거친 뒤, deferred closest-hit/miss가 같은 dispatch-index의 두 UAV 값을 실제 읽어 payload를 만든다. 후속 write/fence/read와 hit/miss 상태까지8 words를 독립 host 정수 oracle로 비교한다. 64 invocations > 보고된 WaveLaneCountMax32이므로 한 wave에 제한된 시험이 아니다. 다른 dispatch-index의 global rendezvous를 가정하지 않는다. Genuine final DXC lib_6_9 -O0의 현행 frozen DXIL SHA256는 **568dad8f94656c94f20df9d9ebf1b3d1b9fbb11d9544f5523c5ca6f24226ed28**다. Strict gate는 실제64 ordered rows의512 값을 재계산하며 summary만으로 PASS를 허용하지 않는다.

최초 [x64120초 deadline 결과](../build/local/p3-20261003/hitobject/coherence/public-runtime06-x64/result.json)는 legacy linkage 이후 HitObject 전에 harness timeout과 잘린 trace를 기록했다. Production validation을 완화하거나 memory oracle를 줄이지 않고 명시적600초 deadline으로 같은 전체 consumer를 실행했다. 실패 기록을 보존하며 timeout을 SER memory failure 또는 성공으로 소급 해석하지 않는다.

같은 runtime-06 OMM/Tier1.2/OPTIONS22 admission과 함께 전체 #19를 완료했다. ShaderExecutionReorderingActuallyReorders=FALSE인 conforming no-op scheduling이며 성능상 SER 가속이나 별도 #15 atomic RT, 전체 SM/FL·다른 GPU/OS·ARM64/FEX guest를 선언하지 않는다.

### 직전 구현 단계의 범위


- Resource 생성·heap·view·footprint·allocation·residency.
- Command recording·indirect·predication·copy·resolve·legacy barrier·기본 query.
- 기본 VS/PS/CS shader lowering·root binding·graphics/compute PSO.
- 기존 Queue/Fence 의미와 GPU retirement 수명 유지.

**직전 단계에서 사용자가 분리했던 항목:** 정확한 `PIPELINE_STATISTICS` query. Timestamp·occlusion·clock calibration은 렌더링 코어에 남겼다. 이후 P1-C에서 MSAA 실행을 포함해 구현·검증하고 공개했으며, 더 이상 보류 항목이 아니다.

DXR 확장, GS/HS/DS·mesh 확장, NGX/게임별 호환성, pipeline library 및 캐시 성능 확장은 직전 단계의 완료 판정에 포함하지 않았다. 이는 프로젝트의 최종 지원 범위를 제한하는 목록이 아니다.


### D3D11 변경 조사와 유지 방침

**2026-09-29 사용자 결정: 기존 변경은 삭제하지 않는다. 정말 필요한 경우가 아니면 D3D11은 건드리지 않는다.** D3D12 구현에 필수인 공유 인터페이스·ABI 변경 또는 그 변경으로 깨지는 기존 업스트림 동작 보존만 예외로 하고, 해당 의존성을 코드 근거로 설명한 뒤 최소 범위로 수정한다. 인접 코드에 같은 버그가 있거나 D3D11 테스트에서 별도 문제가 발견됐다는 이유만으로 작업을 확장하지 않는다. D3D10/11 기능 완성을 P1의 독립 완료 조건으로 삼지 않는다.

고정 DXMT 업스트림 `7c8dee1c2d73415301ceb7d1fa810861cef4cd67`, frozen import patch 18개, 초기 import `9e5ffa3`, 후속 commit `6a7f923` 및 현재 작업 트리를 대조했다. `src/d3d10`은 업스트림과 동일하고, `src/d3d11`은 아래 swapchain과 buffer 두 파일만 다르다. 조사 중 시도했던 삭제는 원상복구했으며 코드·fixture·runner·과거 실행 증거를 유지한다.

| 기존 변경 | 추가 이유와 판단 | 현재 처리 |
|---|---|---|
| `src/d3d11/d3d11_swapchain.cpp` fullscreen·`ResizeTarget` 오류/상태 보존·생성 처리 | 당시 P1 문서의 공유 D3D10/11 계약까지 적용한 범위 확장. D3D12 frontend 구현의 필수 의존성이 아님 | 기존 수정 유지, 별도 D3D11 확장 중단 |
| 같은 파일의 공개 `GetGammaControl` 조회와 ramp cache | D3D11 output proxy Present의 내부형 캐스팅 충돌을 실제 재현해 수정. D3D11 전용 버그 | 기존 수정·증거 유지 |
| `owned-dxgi-fullscreen.cpp`와 runner의 D3D11 분기 | 두 frontend의 fullscreen·readback·Present 회귀 검증을 위해 추가 | 기존 fixture 유지, 다음 기능은 D3D12 전용 검증 |
| `src/d3d11/d3d11_buffer.cpp` UAV `.writable = true` (`6a7f923`) | 공유 buffer-view 확장 시 쓰기 속성을 명시. 정상 D3D11 UAV는 기존 allocation flag 분기로도 쓰기 권한을 얻으므로 필수 migration이라고 볼 근거는 없음 | 한 줄 변경 유지, 추가 D3D11 변경의 근거로 사용하지 않음 |
| `src/nvapi/nvapi.cpp`의 D3D11 opcode-query null 검사 (`0016`) | 인접 D3D12 함수 추가 시 null 안전성을 보강한 것으로 추정 `[INFERENCE]`; 독립적인 동기 기록 없음 | 기존 검사 유지 |
| `scripts/test-owned-ngx-frontend.py`의 D3D11 export 검사 | D3D12 NGX를 같은 DLL에 추가할 때 기존 업스트림 export 보존을 확인하려고 작성한 검사. 신규 D3D11 구현은 아님 | 기존 ABI 보존 검사 유지 |

D3D12가 사용하는 DXBC/SM50 compiler, buffer range/writable, Metal bridge, queue/presenter, DXGI/WSI 변경은 D3D11이라는 이름이나 공유 여부만으로 제거하지 않는다. NGX parameter의 `ID3D11Resource` Set/Get 슬롯은 D3D12도 사용하는 공식 17-slot ABI의 일부라 유지해야 한다. 업스트림 `nvngx.cpp`의 D3D11 구현 및 frozen patch/provenance도 보존한다.


### P0에서 고정한 규격·backend 기준표

조회 시점은 **2026-09-21 17:48:48 UTC / 로컬 2026-09-22**다. “최신”은 이 시점에 조회한 공식 publication으로 한정한다. SDK package, standalone header release, compiler release의 patch 번호가 같다고 가정하지 않는다.

| 대상 | 정식 목표 | 별도 preview/experimental 추적 |
|---|---|---|
| Agility SDK | Microsoft.Direct3D.D3D12 **1.619.6**, 2026-09-14 게시 | **1.721.3-preview**, 2026-07-30 게시 |
| DirectX-Headers | **v1.619.5**, 2026-07-31 게시 | **v1.721.2-preview**, 2026-07-08 게시 |
| DXC release | **v1.9.2607**, 2026-07-29 게시 | **v1.10.2605.37**, 2026-08-12 게시 |
| Shader Model | **6.9**, DXC v1.9.2602부터 production support 명시 | **6.10 preview** |
| DXIL | **1.9**, 공식 proposal Completed | **1.10**, 공식 proposal Under Review — 정식 지원 기준으로 취급하지 않음 |

공식 근거:

- [Agility 1.619.6 package](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.619.6), [version/publication feed](https://api.nuget.org/v3/registration5-semver1/microsoft.direct3d.d3d12/index.json).
- [DirectX-Headers v1.619.5](https://github.com/microsoft/DirectX-Headers/releases/tag/v1.619.5), [v1.721.2-preview](https://github.com/microsoft/DirectX-Headers/releases/tag/v1.721.2-preview). 조회 당시 대응하는 1.619.6/1.721.3 standalone header release는 확인하지 못했다.
- [DXC v1.9.2607](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2607), [6.9 production support 명시](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2602), [6.10 preview compiler](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.10.2605.37).
- [SM 6.9 명세](https://github.com/microsoft/DirectX-Specs/blob/5d736f2a61b227ad1e3cc31cd9a718f546041f4e/d3d/HLSL_ShaderModel6_9.md), [DXIL 1.9 proposal](https://github.com/microsoft/hlsl-specs/blob/fb3577a041dc2c146bd447a80b5de501eb0f4509/proposals/0033-dxil19.md), [DXIL 1.10 proposal](https://github.com/microsoft/hlsl-specs/blob/7ba76180209092d644ac74cb66c8a3836001e7b5/proposals/0046-dxil110.md).

| 로컬 계층 | 실제 확인한 기준 | 지원 판정과의 구분 |
|---|---|---|
| 선택된 cross-build header | llvm-mingw toolchain의 `include/d3d12.h`; enum/interface 범위 SM6.7, OPTIONS18, ID3D12Device10 | 정식 목표보다 오래되며 이 범위를 runtime이 모두 지원한다는 뜻이 아님 |
| 저장소 vendored header | `include/native/directx/d3d12.h`: SM6.0, OPTIONS7, ID3D12Device1; upstream revision pin 없음 | 선택된 build header와도 다름. 최신 header 기반 정비는 남은 구현 작업 |
| Cross compiler | llvm-mingw clang **22.1.8**, LLVM commit `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`, x86_64 Windows GNU target | 현재 P0 빌드에 사용한 compiler |
| AIR compiler dependency | LLVM **15.0.7**, `toolchains/llvm-15` | LLVM 버전과 지원 DXIL/SM 버전은 별개 |
| Native OS/SDK | macOS **27.0 (26A428)**, Xcode **27.0 (27A5237l)**, macOS SDK **27.0** | 이 환경의 관찰값이며 다른 OS 조합은 별도 확인 필요 |
| Native compiler | Apple clang **21.0.0**, Metal compiler **32023.921 / metalfe-32023.921.3** | Compiler 존재가 모든 Metal feature 사용을 보장하지 않음 |
| Native GPU | Apple **M5 Pro**, 20 GPU cores, Metal 4. Native query에서 Apple family 1–10/Mac1/Mac2, raytracing, dynamic libraries 지원 응답 | Native capability를 D3D tier 지원으로 승격하지 않음 |
| P0 당시 D3D12 공개 지원 선언 | **FL11_0 / SM5.1**, DXR tier·mesh tier 미지원(P2 완료 뒤 SM 6.5) | 일부 SM6 DXIL 입력을 소비하는 것과 SM6 전체 conformance는 다름 |

Native family 의미는 [Apple `supportsFamily`](https://developer.apple.com/documentation/metal/mtldevice/supportsfamily(_:))와 [raytracing capability](https://developer.apple.com/documentation/metal/mtldevice/supportsraytracing)를 기준으로 구분한다. DXIL parser의 version metadata 처리나 개별 lowering 존재만으로 상위 버전 검증 완료를 주장하지 않는다.

### 실행 순서

우선순위는 **정확성·공통 기반·다른 기능의 의존성**으로 정한다. P0·P1·P2·P3의 기존 backend 완료에 이어 P4의 D3D12 Metal 4 구현 전환·명시한 fixture acceptance를 마쳤다. 성능·실사용 완성도와 ARM64 Wine/FEX 최종 구성은 남은 목표이며, 역사적 증거와 현재 mixed-cut acceptance의 경계를 유지한다.

| 우선순위 | 작업 | 해당 단계의 완료 기준 |
|---|---|---|
| **P0 — 기준 고정과 현재 통합본 안정화** | 규격/backend 기준표 작성, 직전 UNVERIFIED 통합본의 build·ABI·실행 문제 수정. 누락 DXIL 입력 준비. Buffer RTV, depth/stencil resolve, typed/multilayer clear, 실제 placed-resource aliasing, predicate/indirect, queue/fence/residency 경계를 확인한다. | 소스와 실행 artifact가 일치하고 해당 계약의 관찰 가능한 결과·오류·수명·ordering이 재현 가능한 증거로 남는다. 알려진 corruption/deadlock/device-loss 문제를 숨기지 않는다. |
| **P1 — 기본 API·shader·presentation 계약의 완전성** | D3D12의 resource/view/format/heap/map/copy/resolve, root/binding/PSO, recording/query/indirect/predication, queue/fence/residency의 남은 공백을 닫는다. 정확한 pipeline statistics를 다시 포함한다. D3D12가 사용하는 DXGI swapchain/present/resize/fullscreen/color-space/HDR를 기준표에 따라 다룬다. | 기본 경로의 정상·경계·오류·동시성 계약이 맞고, feature/format 지원 보고와 실제 동작이 일치한다. 부분 native counter를 전체 statistics로 가장하지 않는다. |
| **P2 — 현대 기능의 공통 기반** | 최신 Shader Model의 stage/opcode·numeric·atomic·wave/subgroup·memory semantics와 descriptor indexing/bindless 경로를 완성한다. GS/HS/DS, 요구되는 정밀도/정수 폭도 추적한다. Enhanced barriers, tiled/reserved resource·residency, shared handle/cross-adapter의 수명·가시성·동기화 및 PSO/pipeline-library 계약을 구현한다. | 각 기능의 전체 계약과 의존 기반이 연결된다. Shader 변환 성공만이 아니라 resource binding, GPU 결과, synchronization까지 맞는다. Native 지원이 다른 GPU family는 별도 판정한다. |
| **P3 — 최신 고급 rendering/compute 기능** | DXR의 목표 tier 전체, mesh/amplification, VRS, sampler feedback, Work Graphs 및 기준표에서 확인된 추가 최신 기능을 구현한다. 이 목록은 상한이 아니며 새로운 공개 규격을 계속 반영한다. | 기능별 API·compiler·execution·memory 경로와 tier 요건이 모두 충족된다. 한 DXR/mesh sample의 성공으로 전체 tier를 올리지 않는다. |
| **P4 — 성능·안정성·실사용 완성도** | CPU allocation/copy·Rosetta 비용, submission overlap, shader/PSO compile/cache, native archive 활용, memory pressure, 장시간 실행과 다양한 엔진/workload를 개선한다. NVAPI/NGX 같은 연동 계층도 표준 backend 위에서 완성한다. | 동일 결과·동일 workload에서 CPU/GPU 시간·전력/발열·메모리·stutter를 분리 측정한다. Cold/warm cache와 여러 GPU/OS 조합의 회귀를 확인하며 정확성을 성능과 교환하지 않는다. |

P0를 마친 뒤 공통 계약은 P1, 현대 기능의 기반은 P2, 이를 소비하는 고급 기능은 P3 순서로 진행한다. 독립된 기능은 병렬 구현할 수 있지만 의존 기반이 미검증인 상태를 완료로 승격하지 않는다. P4가 뒤에 있다고 불필요한 allocation/copy나 명백한 성능 결함을 방치한다는 뜻은 아니다. 기본적인 효율은 모든 단계의 구현 원칙이다.

새로운 correctness·보안·메모리 수명·data corruption 문제는 단계와 관계없이 먼저 수정한다. 게임 재현은 공통 계약의 버그를 찾는 입력으로 사용하며, 타이틀별 우회나 특정 장면의 FPS를 위해 규격을 왜곡하지 않는다.


## 1. Resource·heap·view·메모리 계약

주요 소스:

- `src/d3d12/d3d12_resource_contract.hpp`
- `src/d3d12/d3d12_resource_helper.cpp`
- `src/d3d12/d3d12_device.hpp`, `d3d12_device.cpp`
- `src/d3d12/d3d12_buffer.cpp`, `d3d12_texture.cpp`, `d3d12_heap.cpp`
- `src/dxmt/dxmt_buffer.hpp`, `dxmt_buffer.cpp`

구현 내용:

- 크기·offset·alignment의 checked add/multiply/align/range 처리 및 DXGI format-family/copy 호환성 도우미.
- Resource dimension·extent·sample·layout·alignment·heap state·node mask 검사. UPLOAD의 `GENERIC_READ`, READBACK의 `COPY_DEST` 계약 반영.
- Optimized clear value의 flag·format 검사, 출력 객체를 요청하지 않는 validation-only 생성의 `S_FALSE` 경로.
- Heap/placed resource의 크기·정렬·범위 검사. Native allocation 실패와 allocation 예외의 오류 전파.
- CUSTOM heap의 CPU page property에 따른 Map 접근 정책.
- SRV/UAV의 resource flag·format family·dimension·sample·subresource·plane·범위 검사.
- Void view 생성 시 목적 descriptor를 먼저 비워 실패 후 이전 descriptor가 남지 않도록 처리. Typed native view를 생성 경로에서 materialize하고 실패를 내부 오류로 전달.
- `GetCopyableFootprints`의 512-byte placement/256-byte row pitch, plane/subresource 및 표현 가능한 필드 폭 검사. 빈 범위·overflow 처리.
- Allocation info의 빈 입력·null·overflow·native size/alignment 처리.

### 64-bit buffer view

Raw/structured buffer slice의 byte range를 64-bit로 유지한다. Typed view도 큰 `FirstElement`를 32-bit로 잘라 저장하지 않는다.

- `BufferViewDescriptor`가 64-bit backing byte offset/length 및 writable 여부를 보유한다.
- Native ranged texture-buffer view는 정렬된 offset에서 생성한다.
- Shader metadata에는 정렬 때문에 생기는 작은 element bias만 남긴다.
- View cache key에 range/writable을 포함한다.
- Integer clear용 재해석 view도 원래 range와 alignment를 보존한다. Whole-buffer alias나 offset 이중 적용을 피한다.

공유 코드 호출부도 함께 변경했다. `src/d3d11/d3d11_buffer.cpp`의 typed UAV와 `src/dxmt/dxmt_command_clear.hpp`의 alternate clear view는 writable을 명시한다. D3D10은 해당 D3D11 구현 경로를 사용한다. 이 공유 프런트엔드 변경 역시 재빌드·실행하지 않았다.

## 2. Buffer render-target view

기존 buffer RTV의 `E_NOTIMPL` 경로 대신 descriptor·render attachment·clear·copy coherence를 연결했다.

추가 관련 소스:

- `src/d3d12/d3d12_descriptor_heap.hpp`
- `src/d3d12/d3d12_command_encoder.hpp`
- `src/d3d12/d3d12_command_list.cpp`, `d3d12_command_queue.cpp`
- `src/dxmt/dxmt_command_clear.hpp`

동작 구조:

1. `ALLOW_RENDER_TARGET`, BUFFER dimension, typed/non-typeless color format 및 full 64-bit byte range를 검사한다.
2. Descriptor에 backing buffer, ranged view, 전체 element/byte range와 format을 보존한다.
3. Metal `TextureBuffer`를 render attachment로 가장하지 않는다. Render pass별 private 2D single-row texture를 만들고 backing buffer 내용을 copy-in한다.
4. 해당 attachment로 렌더링한 뒤 같은 범위를 backing buffer로 copy-out한다. 여러 buffer/texture RTV의 혼합 binding도 attachment별로 처리한다.
5. Typed pass가 native buffer/texture 소유권을 보유해 후속 copy와 GPU retirement까지 연결한다.

Raster window는 `min(NumElements, D3D12_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH)`다. **16384는 일반 texture 크기에서 추정한 제한이 아니라 D3D12의 render-to-buffer window 상수**다. 전체 descriptor range는 유지하며 raster window 밖은 draw로 덮어쓰지 않는다.

`ClearRenderTargetView`는 ranged typed compute clear를 사용하며 predicate를 반영한다. Rect가 없으면 전체 `UINT NumElements`를 처리하고, `LONG` 변환으로 범위를 잘라내지 않는다. Explicit rect와 dispatch group 계산은 별도로 범위/overflow를 처리한다.

참조: [D3D12 constants](https://learn.microsoft.com/en-us/windows/win32/direct3d12/constants), [D3D11.3 functional specification의 buffer view 제한](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).

P0에서 `owned-d3d12-buffer-rtv.cpp`를 추가했다. 서로 다른 `FirstElement`, descriptor copy, 2개 MRT, predicated 부분 clear, 16,384-element 경계와 앞뒤 guard를 실제 GPU readback으로 확인했다. 실행 근거는 아래 P0 증거에 별도로 기록한다.

## 3. Residency

주요 소스:

- `src/d3d12/d3d12_pageable.hpp`
- `src/d3d12/d3d12_device.hpp`, `d3d12_device.cpp`
- Resource/heap 생성·소멸 hook, `d3d12_descriptor_heap.cpp`, `d3d12_query_heap.cpp`

구현 내용:

- Committed resource, explicit heap, descriptor heap, query heap의 device-owner 기반 pageable 등록.
- Residency reference count와 `CREATE_NOT_RESIDENT` 초기 상태. Native residency set은 0↔1 전이에서 변경한다.
- Batch 전체의 null·타 device·unsupported object·refcount underflow/overflow 검사, 중복 항목과 atomic mutation 처리.
- Placed resource는 heap residency를 빌려 사용한다. 독립 residency 대상으로 중복 계상하지 않으며 VA 등록 수명과 residency 등록 수명을 분리한다.
- Priority는 허용 범위 내 값을 저장한다. Metal에 대응 priority API가 없으므로 native eviction 우선순위 보장은 아니다.
- `EnqueueMakeResident`는 device lock 아래 add/commit을 완료한 뒤 fence를 signal한다. 호출 순서와 fence signal 순서를 직렬화하며, return 뒤 별도 residency 작업을 남기지 않는 보수적 구현이다.
- `DENY_OVERBUDGET`는 성공으로 무시하지 않는다. Native committed residency-set bytes와 새로 resident가 될 owner bytes를 `recommendedMaxWorkingSetSize`와 비교한다. 초과하면 변경·fence signal 전에 `E_OUTOFMEMORY`를 반환한다.

Metal bridge에 `MTLAllocation.allocatedSize`와 `MTLResidencySet.allocatedSize`를 추가했다. CPU-only descriptor heap은 native allocation 목록이 비어 있어도 논리 residency 상태를 가진다.

**Timestamp storage 경계:** `MTLCounterSampleBuffer`는 `MTLAllocation`이 아니므로 residency set에 직접 넣지 않는다. Timestamp query heap은 dummy allocation과 논리 refcount로 표현하고 opaque counter storage는 driver가 관리한다. Occlusion heap은 실제 visibility-result allocation을 등록한다.

## 4. Recording·ExecuteIndirect·GPU predication

주요 소스:

- `src/d3d12/d3d12_command_signature.cpp`
- `src/d3d12/d3d12_command_list.cpp`
- `src/d3d12/d3d12_command_allocator.cpp`, `d3d12_command_encoder.hpp`
- `src/d3d12/d3d12_command_queue.cpp`
- `src/dxmt/dxmt_command_context.hpp`, `dxmt_command_clear.hpp`

### Recording와 indirect

- Command signature의 argument graph·stride·packed size·terminal action·VB slot·IB 조합을 검사한다.
- Root signature ownership/local 여부, parameter type 및 root constant span을 검사한다.
- `ExecuteIndirect`의 device/list type·argument/count buffer·alignment·range·root 계약을 검사한다.
- 실행 후 signature가 변경한 root/VB/IB state를 D3D12 계약에 맞게 reset한다.
- Queue descriptor와 타 device의 Execute/Signal/Wait 입력을 검사한다.
- Invalid recording은 첫 오류를 보존한다. **실패한 `Close`로 poison된 list를 `Reset`으로 복구시키는 변경은 하지 않았다.**
- D3D의 비어 있는 viewport/scissor 상태를 Metal의 유효한 offscreen raster 상태로 내린다. 두 배열 중 하나가 바뀌면 함께 재설정해 이전 raster 상태가 남지 않도록 하며, 전체 draw를 생략하지 않아 VS UAV side effect를 보존한다. P0에서 native viewport count 0 assertion을 재현·수정했고, viewport 없는 VS atomic 실행과 일반 buffer RTV draw를 각각 확인했다.

### Predicate 의미

`SetPredication`은 GPU stream에서 64-bit 값을 allocator 소유 snapshot으로 복사한다. 이후 원본 predicate buffer가 바뀌어도 이미 설정한 predicate의 의미를 바꾸지 않는다.

**`D3D12_PREDICATION_OP`는 실행 조건이 아니라 생략 조건이다.** `EQUAL_ZERO`는 0일 때 생략하고 nonzero일 때 실행한다. `NOT_EQUAL_ZERO`는 반대다. P0에서 direct/indirect arguments, copy, resolve, MSAA worker의 반전된 조건을 수정했다. [SetPredication 계약](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication)과 [GPU snapshot 의미](https://learn.microsoft.com/en-us/windows/win32/direct3d12/predication)를 기준으로 삼았다.

원본을 `COPY_DEST`로 전환해 saved nonzero predicate 아래에서 0으로 덮어써도 기존 snapshot은 실행을 허용한다. 다시 `SetPredication`한 뒤에는 새 0을 읽어 생략해야 한다. Fixture는 덮어쓰기와 재설정 이후 destination 보존까지 확인한다.

- Direct Draw/DrawIndexed/Dispatch는 GPU가 생성한 indirect arguments로 실행 또는 zero-work를 선택한다.
- ExecuteIndirect resolver는 동일 snapshot으로 실행 count를 gate한다.
- Bundle draw는 부모 list의 predicate 경로를 사용한다. Bundle 안의 `SetPredication`은 허용하지 않는다.
- Predicate disable은 이후 작업을 unconditional 경로로 복원한다.
- Query Begin/End/ResolveQueryData를 rendering predicate로 생략하지 않는다.

### Clear

RTV/DSV는 conditional indirect draw, UAV Uint/Float는 conditional indirect dispatch로 연결했다. Predicate가 false인 full clear를 unconditional `loadAction=Clear`로 바꾸지 않는다.

Rect/array instance count, DSV의 선택한 depth/stencil component, untouched 영역의 load/store를 처리한다. Buffer RTV clear도 같은 snapshot을 사용한다.

P0에서 buffer RTV clear의 compute-pass 시작 순서를 수정해 null encoder 접근을 제거했다. Indirect dispatch는 threadgroup 단위로 반올림되므로 typed/raw buffer와 2D/array/3D clear kernel 모두 metadata의 실제 rect 크기로 write를 제한한다. GPU allocation 내부라는 이유로 view/rect 바깥의 쓰기를 허용하지 않는다.

## 5. Copy·resolve·legacy barrier

주요 소스는 command list/encoder/queue와 `src/dxmt/dxmt_command.metal`이다.

### Copy

- Buffer/footprint range와 staging arithmetic에 checked helpers를 사용한다.
- DXGI compatible-family copy에서 native pixel format이 다르면 공통 format의 per-subresource view로 맞춘다. 서로 다른 native format의 blit이 우연히 동작한다고 가정하지 않는다.
- Predicated buffer copy는 GPU conditional byte-copy를 사용하며 큰 범위는 나누어 처리한다.
- Non-MSAA texture↔texture, texture↔footprint, footprint↔footprint는 raw staging과 GPU conditional byte-copy로 false predicate의 destination을 보존한다.
- Predicated `CopyResource`는 buffer/subresource 경로로 연결한다.

### 정확한 predicated MSAA copy

Public Metal에는 conditional blit이 없으므로 MSAA raw copy를 shader 색 변환으로 대신하지 않는다.

1. Typed `PredicatedTextureCopy` pass가 source/destination/snapshot을 보유한다.
2. **Submission worker**가 앞선 command-buffer segment를 commit하고 실제 GPU retirement를 기다린다.
3. 완료된 snapshot을 읽고 predicate가 true일 때만 native texture blit을 기록한다.
4. 다음 ordered segment에서 나머지 명령을 계속한다.

API caller는 비동기 상태를 유지한다. 대신 이 드문 경로에서는 worker가 GPU completion을 기다리므로 overlap과 성능 비용이 있다. 각 segment의 allocator/native reference 수명도 유지한다.

### Resolve

- `ResolveSubresource`와 Region validation을 연결한다.
- 부분 rect·destination offset·untouched 영역 보존을 구현한다.
- Color float/normalized AVERAGE/MIN/MAX, signed/unsigned integer MIN/MAX 경로를 추가했다.
- Depth/stencil MIN/MAX는 선택한 plane만 기록한다. 불법 AVERAGE는 거부한다.
- Combined depth/stencil의 sampling view와 destination attachment/PSO format을 분리한다. Stencil sampling의 X32 view를 render attachment format으로 그대로 사용하지 않는다.
- Resolve shader도 predicate snapshot을 반영한다.
- Stencil resolve의 fullscreen triangle은 Metal 기본 winding에서 back face가 될 수 있으므로 front/back 모두 `Always`/`Replace` 상태를 설정한다. 한쪽 face만 설정해 stencil destination이 그대로 남던 문제를 P0에서 수정했다.
- `DECOMPRESS`는 Metal의 uncompressed representation에서 합법적인 in-place 조건을 검사한 뒤 no-op으로 처리한다. 임의의 copy/resolve를 no-op으로 대체한 것이 아니다.

### Barrier

Legacy transition/UAV/aliasing barrier의 type·flag·state·subresource를 검사하고 기존 보수적 encoder 경계를 유지한다. 이 경계와 Metal synchronization이 실제 ordering을 담당한다. 완전한 resource-state debug tracker를 뜻하지 않는다. Enhanced barrier는 [P2-BARRIERS](#p2-barriers--enhanced-barriersdevice614commandlist59render-passgpu-upload-heap) 참조.

## 6. Root signature·binding·PSO

주요 소스:

- `src/d3d12/d3d12_root_signature.cpp`, `d3d12_device.hpp`
- `src/d3d12/d3d12_pipeline_graphics.cpp`, `d3d12_pipeline_compute.cpp`
- Command list의 root/PSO setter

RS 1.0/1.1 serializer와 deserialized root signature 생성이 같은 validator를 사용한다. Pointer/count graph, enum/flags, descriptor range/offset overlap, visibility별 register overlap, static sampler, global **64 DWORD** budget을 검사한다.

Local root는 global budget과 혼동하지 않는다. Shader record 4096 bytes에서 identifier 32 bytes를 뺀 **4064-byte argument payload**와 table/descriptor alignment를 적용하고 기존 DXR local copy 경로를 보존한다.

Root metadata의 type/constant count/local 여부를 setter 및 indirect validation에 연결했다. Index 경계, parameter type, constant span, null data, device ownership과 recording state를 검사한다. Graphics/compute PSO는 bytecode pointer/size, root ownership, sample/fixed-function enum/format 계약을 검사한다.

P0 통합 수정에는 metadata pointer의 backing storage를 root signature 소유 배열에 유지하는 변경도 포함한다. 임시 데이터의 수명에 의존하지 않는다.

## 7. 기본 shader compiler

주요 소스:

- `src/airconv/dxil_resources.cpp`
- `src/airconv/nt/air_builder.hpp`, `air_builder.cpp`
- `src/airconv/nt/dxbc_converter_base.hpp`, `dxbc_converter_base.cpp`
- `src/airconv/dxbc_converter.hpp`, `dxbc_converter.cpp`, `dxbc_signature.cpp`

추가 DXIL lowering:

| Opcode | 처리 |
|---|---|
| 74 | `TextureGatherCmp` |
| 75 | `Texture2DMSGetSamplePosition` |
| 76 | `RenderTargetGetSamplePosition` |
| 77 | `RenderTargetGetSampleCount` |
| 81 | `CalculateLOD` |

기존 AIR/DXBC helper 및 결과 shape를 재사용하고 sample-position helper를 공유한다. 기본 VS/PS/CS DXBC에서 지원하지 않는 application input은 initialize diagnostic으로 전달하도록 정리했다. 정상적인 unsupported 입력을 abort로 처리하는 경로를 줄이고 null-safe 오류 전파를 연결했다.

P0에서 DXIL lowering이 생성한 `dxmt.sample_pos.i32`에 기존 AIR helper 정의를 실제로 link하도록 수정했다. DXBC PS의 SGV system declaration에는 interpolation mode 0이 합법적일 수 있으므로 `SV_SampleIndex` 등을 flat으로 처리한다. 일반 입력의 잘못된 interpolation을 일괄 허용한 변경은 아니다.

이는 모든 shader model/opcode 지원 선언이 아니다. Tiled resource 관련 `CheckAccessFullyMapped`, FP64/wave, GS/HS/DS/mesh 확장 등은 이번 범위에 포함하지 않는다.

### P0 frozen shader 입력과 생성 도구

`graphics-translator/tests/dxil/`에 다음 입력을 추가했다. Texture fixture는 opcode 74/75/76/77/81을 실제로 포함하며, indirect fixture의 CS/VS는 opcode 78 `AtomicBinOp/Add`를 포함한다. Wine HLSL frontend가 `RWByteAddressBuffer.InterlockedAdd`를 컴파일하지 못해 indirect shader를 frozen DXIL로 바꿨다. Atomic을 일반 load/store로 바꾸거나 indirect 결과 기대값을 약화하지 않았다.

| 입력 | 크기 | SHA-256 | 생성 조건 |
|---|---:|---|---|
| `basic-texture-vs.dxil` | 3,148 B | `3d0c8c3126d1b4608fba5f490645afd0c8529e84700d590ea047507d4d1d32a7` | `basic-texture-ops.hlsl`, `-T vs_6_0 -E vsMain -HV 2021 -O0` |
| `basic-texture-ps.dxil` | 5,868 B | `f4fabb44ef94e4961e0e2aa7e617a9fbad0700c739492f4e6339809d6d508539` | 같은 HLSL, `-T ps_6_0 -E psMain -HV 2021 -O0` |
| `predication-indirect-cs.dxil` | 2,984 B | `08dce50dd933af311cfea88dc0489357f3b20df5547d3b8d36824318e4444a50` | `predication-indirect-cs.hlsl`, `-T cs_6_0 -E main -HV 2021 -O0` |
| `predication-indirect-vs.dxil` | 3,404 B | `68d328aa2906e26f7fbdc670ea07522cf6af03df9699285f703a87fe881b01a9` | `predication-indirect-vs.hlsl`, `-T vs_6_0 -E main -HV 2021 -O0` |

생성에는 로컬 `build/local/dxc-probes/dxc.exe`와 같은 디렉터리의 `dxcompiler.dll`·`dxil.dll`을 사용했다. 도구 보고 버전은 **1.9.0.5402 (`0d3ee6b55-dirty`)**이며, 위 최신 정식 DXC release와 같은 배포물이라고 검증한 것은 아니다. 도구 SHA-256은 각각 `980a3a4c6e5c88f5737dde321e548860021d58fa2349790d4e572805cb298293`, `9a5100511e127c6a2fc78edf984f95074a76d35b90c90c4d342430a5ae160e9b`, `feb57253eff0a622561e29b44cedbe86b89fc9a5bc8dc00fa2f98fafd712c2d8`다.

재생성 예시(동일 로컬 도구와 격리된 prefix 사용):

```sh
WINEPREFIX="$PWD/build/local/dxc-wine-prefix" WINEDEBUG=-all \
  toolchains/wine-runtime/bin/wine build/local/dxc-probes/dxc.exe \
  -T cs_6_0 -E main -HV 2021 -O0 \
  graphics-translator/tests/dxil/predication-indirect-cs.hlsl \
  -Fo graphics-translator/tests/dxil/predication-indirect-cs.dxil
```

Runner는 executable과 shader 입력을 evidence 디렉터리에 복사하고 hash·argv를 기록한다. `predication-indirect` argv 순서는 CS, VS이며 `dxil-texture`는 VS, PS다. 두 suite 모두 실행한 staged native module에 들어 있는 owned DXIL compiler의 실제 load를 요구한다.

## 8. Query·clock calibration·WineMetal ABI

Timestamp/occlusion 경로를 유지하고 queue `GetClockCalibration`을 구현했다.

- Null output과 지원하지 않는 queue type을 구분한다.
- Metal의 paired CPU/GPU timestamp 호출 앞뒤를 Wine `QueryPerformanceCounter`로 감싼다.
- 현재 Metal 4 GPU timestamp와 `GetTimestampFrequency`는 native query counter frequency를 사용한다(M5 Pro 관찰 **24000000 Hz**). Paired `sampleTimestamps`의 GPU nanoseconds는 이 counter tick 단위로 변환하므로 calibration과 resolved query가 같은 단위를 쓴다. 기존 backend의 1 GHz 계약과 구분한다.
- CPU timestamp는 주변 Wine QPC의 overflow-safe midpoint를 사용한다. Metal host clock 값을 Wine QPC epoch인 것처럼 직접 반환하지 않는다. 일반적인 bracket 정확도 보장은 하지 않는다. Recovery `integration/runtime02-core{64,32}-risk-01/queries/stdout.log`에서 양 ABI native 24 MHz timestamp, async GPU resolve, 같은 queue의 split command buffer와 GPU fence를 통한 cross-queue ordering, resolve 이전 COM release를 통과했다. Fixture의 calibration 간격 오차는 x64 0.867 µs·x86 1.717 µs의 관찰값이지 전체 환경의 오차 상한이 아니다.

Bridge 관련 파일은 `src/winemetal/winemetal.h`, `winemetal_thunks.h`, `winemetal_thunks.c`, `Metal.hpp`, `unix/winemetal_unix.c`다. Append-only Unix call 번호는 다음과 같다.

| 번호 | 기능 |
|---|---|
| 201 | Device paired timestamp sampling |
| 202 | Allocation allocated size |
| 203 | Residency-set allocated size |

해당 wrapper/parameter/native handler/native·wow64 dispatch 연결을 함께 변경했다. P0의 x86_64 통합 빌드와 query/residency GPU 실행에서 이 경계를 확인했다. Wow64·ARM64EC 실행까지 검증한 것은 아니다. 추가한 render encoder depth/stencil state wrapper는 기존 render-command opcode/decoder를 사용하며 새 Unix call 204를 추가하지 않았다.

`D3D12_QUERY_HEAP_TYPE_SO_STATISTICS`와 `SO_STATISTICS_STREAM0..3`를 추가했다(P1-D). 결과는 16 B, Begin에서 recording별 stream counter를 snapshot하고 End에서 차이를 쓴다. Resolve는 8-byte 정렬 offset을 요구한다.

**`PIPELINE_STATISTICS` heap 생성과 88-byte Begin/End/Resolve를 지원한다.** MSAA 실행을 수정한 최종 격리 runtime에서 11개 필드와 독립 GPU oracle을 포함한 전체 fixture를 통과했다. 범위·비용·실행 증거는 위 P1-C를 참조한다.

## 9. Queue/Fence와 기존 증거의 범위

CPU/GPU rewind, API-time wait latch, ANY/ALL event registration, 실제 GPU completion 기반 consumer wait를 유지한다. SO 오류 전파 수정에서는 native event probe와 signal별 Metal event를 제거했다. Queue completion 스레드가 앞선 submission의 GPU 결과와 SO status를 검증한 뒤에만 논리 fence 값을 공개한다. CPU signal은 같은 논리 timeline mutex로 직렬화한다. Device 오류는 terminal UINT64_MAX로 대기를 종료하며 이후 signal로 rewind하지 않는다. Snapshot/staging/segment는 allocator와 native resource의 retirement 수명에 연결한다.

이전 Fence 코어 단계에는 실제 실행 증거가 있었다.

- `build/local/fence-core-after/result.json`
- `build/local/fence-core-multiqueue-proof/result.json`
- `build/local/fence-core-temporal-proof/result.json`
- 상세: [FENCE-EVENTS.ko.md](FENCE-EVENTS.ko.md), [QUEUE-ORDER.ko.md](QUEUE-ORDER.ko.md)

**이 결과는 이번 전체 코어 통합본의 검증 결과가 아니다.** 일부 fixture/객체의 과거 단독 컴파일과 query baseline 실패 기록도 현재 소스의 성공 근거로 승격하지 않는다. `build/local/fence-core-runtime` 등 기존 runtime을 최신 구현본으로 취급하면 안 된다.

## 10. P0 실행 결과와 재현 근거

설정된 `build/local/build`의 전체 Ninja target 빌드와 install을 통과했다. 마지막 viewport/scissor 수정 빌드는 13-step incremental build 성공으로 확인했으며 별도 log 파일은 저장하지 않았다. 앞선 통합 빌드 log는 `build/local/p0/full-build.log`, `runtime-03-build.log`, `runtime-04-build.log`, `runtime-05-build.log`다. 기존 managed-storage deprecation 및 unused lambda-capture warning은 남아 있다.

아래 **11개 suite는 모두 `--validation`으로 PASS**했다. 경로 기준은 `build/local/p0/`이며 각 evidence 디렉터리에 `result.json`, `invocation.json`, frozen fixture/input, stdout/stderr와 backend trace가 있다. Source revision별 새 runtime 복사본을 만들고 각 실행마다 전용 prefix와 새 evidence를 사용했다. **모든 suite를 마지막 runtime 하나에서 재실행했다는 뜻은 아니다.** 수정 영향이 있는 경로만 다시 실행했으며 실제 사용한 revision을 구분한다.

| Suite | 확인한 계약 | Runtime | PASS evidence 디렉터리 |
|---|---|---|---|
| `core-contracts` | Root signature validation·recording 계약 | `runtime-01` | `proof-01/core-contracts` |
| `queries` | 기존 backend의 역사적 QPC bracket·1 GHz timestamp·occlusion·query 수명(현재 Metal 4는 native frequency) | `runtime-01` | `proof-01/queries` |
| `residency` | Refcount·CREATE_NOT_RESIDENT·budget flag·fence 7·256-byte readback | `runtime-01` | `proof-01/residency` |
| `resources` | Footprint/view 계약과 같은 64 KiB heap offset 0의 placed buffer A→B→A 양방향 data inheritance | `runtime-01` | `proof-01/resources` |
| `fence-events` | CPU/GPU rewind·ANY/ALL·registration latch·teardown | `runtime-01` | `proof-01/fence-events` |
| `multiqueue` | 32회 × 1,024 words의 queue 간 ordering/readback | `runtime-01` | `proof-01/multiqueue` |
| `bundles` | 기존 bundle 실행·상태 계약 | `runtime-01` | `proof-01/bundles` |
| `dxil-texture` | Comparison gather·array·LOD 0/-4·4x sample positions/count | `runtime-03` | `proof-04/dxil-texture` |
| `copy-resolve` | RGBA 전 성분 AVG/MIN/MAX·부분 rect·guard·MSAA predicate snapshot/rebind·depth/stencil MIN/MAX·다른 plane 보존 | `runtime-04` | `proof-04/copy-resolve` |
| `buffer-rtv` | FirstElement·descriptor copy·2 MRT·predicated rect·16,384-element 경계와 guards | `runtime-06` | `proof-08/buffer-rtv` |
| `predication-indirect` | Predicate 두 operation·snapshot/rebind·raw/typed clear·2-layer rect/component 보존·indirect count/root/state·CS atomic 합 9·VS atomic 합 3 | `runtime-06` | `proof-08/predication-indirect` |

Fixture 소스는 `graphics-translator/tests/owned-d3d12-<suite>.cpp`다. Typed multilayer coverage는 RTV `R16G16B16A16_UINT`, DSV `D32_FLOAT_S8X24_UINT`, UAV `R32G32B32A32_UINT` 및 `R32G32B32A32_FLOAT`를 포함한다. 이것이 모든 format/layer/sample-count 조합을 검증했다는 뜻은 아니다.

추가 확인:

- `python3 scripts/test-owned-graphics-diagnostics.py`: 변경된 runner 기준 **42 tests PASS**.
- `queue-order-scheduler.test.cpp`: native CPU 실행 PASS — logical fence, registration, FIFO, lifetime, storage, ring drain, cross-queue.
- `d3d12-resource-contracts.test.cpp`: Windows cross-build 후 격리된 Wine 실행 PASS — range/placement/overflow/footprint, pitch 512.
- `dxil/dxbc-input-error-probe.cpp`: native x86_64 실행 PASS — malformed input은 diagnostic/null shader를 반환하고, 같은 process의 다음 valid input은 성공. 일반 runner suite는 아니다.

### 실제 실패에서 수정까지

| 재현 | 수정 | 통과 근거 |
|---|---|---|
| `predication-skip-before-fix`: 0/EQUAL_ZERO에서 쓰기가 실행됨 | 모든 predicate 경로의 skip 조건 교정 | `proof-08/predication-indirect`, `proof-04/copy-resolve` |
| `proof-02/buffer-rtv`: null compute encoder 접근 | Buffer RTV clear의 pass 시작 순서 수정 | `proof-08/buffer-rtv` |
| `proof-03/buffer-rtv`, `proof-04/predication-indirect`: rect 뒤의 guard 손상 | 반올림된 indirect threadgroup의 typed/raw/texture clear write를 실제 rect 크기로 제한 | `proof-08/buffer-rtv`, `proof-08/predication-indirect` |
| `resolve-stencil-diagnostic`: stencil 10 대신 초기 guard 165 | Stencil resolve front/back state 모두 설정 | `proof-04/copy-resolve` |
| `proof-02/dxil-texture`: undefined `dxmt.sample_pos.i32` | 기존 AIR sample-position helper link | `proof-04/dxil-texture` |
| `proof-06/predication-indirect`: Metal viewport count 0 assertion | Empty raster state의 유효한 native 표현, VS side effect 유지 | `proof-08/predication-indirect`와 일반 draw `proof-08/buffer-rtv` |

Fixture 자체의 잘못된 전제도 구분해 수정했다. 존재하지 않는 environment gate를 제거하고 실제 backend identity를 검사한다. Static sampler의 `MaxAnisotropy=0`을 유효한 1로 바꿨다. Raw UINT clear는 `Values[0]`만 사용하는 oracle로, Float clear는 명확한 typed FLOAT view로 구성했다. CPU UAV descriptor는 non-shader-visible heap에 두고 clear 사이에 UAV barrier를 넣었다.

Gather 기대값은 관찰값에 맞춘 것이 아니라 [GATHER4의 LL/LR/UR/UL 순서](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#inst_GATHER4), [reference와 texel의 비교 순서](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#inst_SAMPLE_C), 업로드한 texel·좌표·offset·CLAMP에서 다시 계산했다. 그 결과 scalar `(0,1,0,1)`, array `(0,1,1,0)`이며 lowering을 바꾸지 않고 잘못된 oracle만 수정했다.

`proof-07`은 harness 알림에 의해 실행이 중단되어 완료 artifact가 없으며 성공/실패 판정에 사용하지 않는다. 같은 확인은 durable job으로 새 `proof-08`에서 완료했다. 실패·진단 evidence는 원인 추적을 위해 보존했다.

### 재현 명령과 남은 경계

예시(동일 도구와 현재 source로 새 artifact 생성):

```sh
export PATH="/opt/llvm-mingw-20260616-ucrt-macos-universal/bin:$PWD/toolchains/tools/bin:$PATH"
mkdir -p build/local/p0-reproduce
x86_64-w64-mingw32-clang++ -std=c++20 -O2 -Wall -Wextra -Werror -static \
  graphics-translator/tests/owned-d3d12-predication-indirect.cpp \
  -ld3d12 -ldxgi -luuid -o build/local/p0-reproduce/predication-indirect.exe
ninja -C build/local/build -j8
meson install -C build/local/build --no-rebuild
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --wine-source toolchains/wine-runtime \
  --wine-dest build/local/p0-reproduce/runtime --with-nvapi --with-nvngx
python3 scripts/test-owned-graphics.py --runtime build/local/p0-reproduce/runtime \
  --suite predication-indirect --fixture build/local/p0-reproduce/predication-indirect.exe \
  --out build/local/p0-reproduce/evidence --validation
```

`runtime`과 `evidence`는 실행 전 존재하지 않아야 한다. 재실행 때 새 경로를 선택한다. 다른 fixture도 위 evidence 표의 source로 빌드하며, buffer RTV/copy-resolve는 runtime HLSL compilation을 위해 `-ld3dcompiler`가 필요하다. 최신 header로의 교체, 전체 format/sample-count matrix, texture/reserved-resource aliasing, 다른 GPU/OS/ABI, Windows reference와의 differential 검증, 일반 D3D10/11 workload 및 presentation은 P0의 검증 범위를 넘어 남아 있다.


### P1 전체 구현 중 확인한 계약 안전성 보강

`build/local/p1-complete/runtime-01`·`runtime-02`는 아래 변경을 통합한 새 격리 runtime이다.
모든 아래 D3D12 실행은 Metal API/Shader Validation을 사용했다. **이 결과는 P1 전체 완료 선언이 아니다.**

| 변경 계약 | 관찰한 결과와 증거 |
|---|---|
| PSO 입력 슬롯·활성 fixed-function 상태 검증 | 유효한 IA slot 16·31은 실제 draw/readback 통과, slot 32·UINT_MAX와 잘못된 활성 enum/write mask·미지원 logic op는 출력 포인터를 비우고 거절. `shader-binding/bundles-after-01` |
| 호환 포맷 `CopyResource` | RGBA8 linear→sRGB, 역방향, R32 UINT→FLOAT의 2 mip×2 array, case당 40 texel을 bit-exact 복사. 서로 다른 native pixel format의 직접 whole-texture blit 대신 subresource view copy 사용. `format/copy-resource-casts-after-03` |
| Heap·CPU texture·allocation | Cross-device placed heap 거절, placed resource의 heap COM 수명 유지, caller heap release 후 GPU alias/copy, row pitch 17·3D row 9/slice 19 및 BC tail의 정확한 CPU 전송, 빈 box 무처리 성공. Small MSAA alignment는 top mip 기준이며 배열·추가 mip 크기로 잘못 거절하지 않음. AllocationInfo1 offset으로 실제 placed resource 생성·사용. `resource-memory/after-02` |
| Recording·queue 종료 | Allocator reset 후 stale closed list가 포함된 batch 전체를 제출 전에 버리고 device removal 공개. 같은 command-list COM 객체를 다른 allocator로 reset/re-record하는 유효한 overlap은 유지. GPU fence 완료 직후 allocator reset 16회, unresolved wait의 queue-local 종료 취소와 독립 peer queue 보존 확인. `execution/shutdown-after-02` |
| Swapchain 출력 제한·geometry | Restrict-output 소유권·COM 수명 및 `GetRestrictToOutput`, source size·rotation·background의 실제 동작과 실패 시 상태 보존을 구현. `dxgi-presentation/present-queues-proof-02`. 별도 public D3D12 fixture에서 fence 뒤에 두 Present를 기록한 뒤 setter를 변경하고 실제 CAMetalLayer drawable을 캡처하여 각 frame의 서로 다른 crop/회전/배경색 snapshot을 확인 |
| Sampler·cache 지원 계약 | Shader-visible sampler heap 2048/2049 경계와 CPU-only 2049 허용, 장치별 살아 있는 고유 static sampler 2032개·중복 공유·해제 후 재사용 확인. Shader cache는 구현된 `SINGLE_PSO`만 보고하며 자동 OS cache를 가장하지 않음. `shader-binding/binding-only-after-02` |

표의 증거 경로는 모두 `build/local/p1-complete/` 아래다.

### 원래 P1 추가 구현과 GPU 검증

2026-09-29 추가한 아래 경로는 이전 `runtime-01`~`runtime-03`의 바이너리와 다르다.
**소스·cross-build·native probe를 공개 D3D12 통합 검증 완료로 승격하지 않는다.**

| 추가 경로 | 현재 증거 | 남은 확인 |
|---|---|---|
| Resource/heap/allocation | **runtime-04 공개 GPU 검증 통과:** 최신 resources v16의 dimension·mip·buffer 한계, 4KB/64KB tile 기준·MSAA RT alignment, null-output·초기 state·heap 수명·CPU 전송·GPU alias. `resource-memory/public-runtime04-v16` | 다른 GPU/OS의 allocation 및 Metal capability 차이는 별도 검증 |
| RGB32·packed 4:2:2 | **runtime-04 공개 GPU 검증 통과:** 논리 footprint↔확장 backing, 1D/2D/3D·array·mip copy와 CPU/GPU/predicated copy, RGB32 typed-buffer remap·OOB, RGB32 texture load·MSAA load·FLOAT resolve, packed filtering와 RGB32 blend 6 case. `format-design/*-runtime04-final.log` | 물리 backing은 확장 표현이며 논리 footprint와 동일하지 않음. Format-support·multisample-capabilities도 통과 |
| STANDARD_SWIZZLE | **runtime-04 공개 GPU 검증 통과:** canonical roundtrip, shader·clear, paired format, texture/texture·texture/buffer alias, placed heap Evict→MakeResident 뒤 GPU 읽기, custom WRITE_BACK Map·CPU transfer 뒤 GPU 읽기. `standard-swizzle/public-runtime04-v3`. 별도 native 2D/3D/BC tile oracle 유지 | 공개 fixture의 명시된 조합과 native layout 증거를 구분하며 모든 GPU family의 conformance는 아님 |
| View/compiler | **공개 GPU 검증 통과:** partial 3D UAV DXBC와 DXIL write→load fence·atomic·bounded clear·dimensions, depth 1D/cube/cube-array의 D16/D24/D32/D32S8, fractional LOD·comparison LOD·1D border, BC volume. `format/partial-dxil-after-fence-05` | ROV nested descriptor-table과 sample-frequency 최종 검증은 아래 runtime-13 결과 참조 |
| Compressed/packed copy | **runtime-05 공개 GPU 검증 통과:** BC1 8×8↔RGBA16UINT 2×2 bitcast의 정확한32 bytes, packed cube-array RGBA77/120/120/255, depth plane4 format. `format/{reinterpret-permanent-05,packed-cubearray-permanent-05,depth-plane-corrected-05}` | Reinterpret의 수정된 유효 fixture는 runtime-03에서 실패했다. 무효 footprint나 UAV flag 누락 fixture를 driver 결함 증거로 사용하지 않음 |
| Recording/queue | **runtime-04 공개 GPU 검증 통과:** immutable recording, stale/duplicate·post-Close, state·split·promotion·decay·COPY handoff, 미래 queue Wait 뒤 query, 같은 COM 객체의 유효 rerecord, 제거 뒤 residency 오류. `execution/runner-common-04` | Native residency priority는 hint이며 Metal의 엄격한 eviction priority scheduling을 보장하지 않음 |
| Presentation | **runtime-06 공개 GPU 검증 통과:** present-queues의16 drawable, latency·DO_NOT_WAIT·제거 wake·rotation·resize·restriction, 최대 latency1에서 stalled queue Wait 앞의 queued RESTART 0ms와 정확한 public credit1, fullscreen 최종 Release의 HWND/style/rect/mode 복원. Dirty/scroll exact pixels(runtime-04), crop/rotation/RTL과 실제 presented callback 뒤 statistics(runtime-05) 통과 | 이미 Metal에 commit된 drawable의 폐기는 P1 종료 결과에서 구현·검증. 물리 HDR scanout·다중 출력 전환은 별도 경계 |

호스트의 x86_64 시작 단계가 커널 대기 상태에 빠져 full Ninja가 `winebuild --builtin`에서 멈췄다.
Wine/LLVM을 사용하지 않는 최소 x86 프로그램도 시작하지 못했고, 승인받은 Rosetta 서비스 재시작으로도 복구되지 않았다.
이후 사용자가 실행 권한을 허용한 뒤 같은 최소 x86 프로그램과 `winebuild --version`이 정상 종료했다.
macOS 재시동 없이 full Ninja/install과 새 `build/local/p1-complete/runtime-04` staging을 완료했다.
이 runtime에서 dynamic sampler2047·static sampler2031의 shader 결과와 ForcedSampleCount4의 ROV 누적27/27은 Metal API/Shader Validation을 통과했다(`shader-binding/tier2-sampler-high-after-04`, `shader-binding/rov-forced-after-04`).
Attachmentless ordinary MSAA4 ROV의 encoder/PSO sample count 불일치는 effective raster count로 pass를 생성하고 count가 바뀌면 pass를 분리해 수정했다. **runtime-05**에서 ROV 27/27과 같은 command list의 sample1→sample4→sample1→Forced4 전환이 Metal API/Shader Validation을 통과했다(`shader-binding/{rov-msaa4-after-05,attachmentless-switch-after-05}`). 새 runtime 전체 검증은 진행 중이며 기존 설치본이나 게임 prefix는 변경하지 않았다.
`execution/{multiqueue-04,residency-04,copy-resolve-04,predication-indirect-04}`도 API/Shader Validation을 통과했다. Multiqueue 32×1024 GPU words, residency fence7/checksum `0x59047300`, color/depth/stencil resolve와 indirect/predication 결과를 확인했다. Descriptor heap 1,000,001 거부·sampler/static sampler 수명·cache flags는 `shader-binding/binding-only-static-after-04`에서 통과했다.
`shader-binding/pipeline-cache-after-05`는 malformed·foreign·root/shader 불일치 blob 거부와 변경 shader 결과 `0x31415813`, unaligned 입력 수명·반환 blob 격리 결과 `0x31415a5b`를 GPU에서 확인했다.
`shader-binding/pipeline-statistics-after-06`도 DXBC·DXIL의11-field independent GPU oracle 전체를 Metal API/Shader Validation으로 통과했다. 이 결과는 이후 short-line 입력 복원·center pattern 변경 이전의 기존 MSAA depth·stencil·derivative·interpolation 경로 보존 증거다.
DXGI의13-frame color-space·7-frame HDR metadata 공개 경로와 native presenter의 tone-map·변환 oracle도 통과했다. 공유 backend 보존 검증은 `runtime06-baseline.log`에서 D3D11 3D UINT/FLOAT48-voxel clear·DXBC compute·PS sample·Present 및 실제 drawable3072 pixels=`ff4080bf`로 확인했다. D3D11 전용 기능 범위를 추가한 것은 아니다.
완료된 시험 prefix는 디스크 공간 회수를 위해 제거했으며 frozen fixture·stdout/stderr·native module log·result.json·drawable capture는 보존했다.
**runtime-10 통합 GPU 통과:** `shader-binding/pipeline-statistics-after-10`의 DXBC 168+DXIL 178=346 GPU case·26 validation case와 `line-after10`, `center-final10-v1`, `center-dxil-final10-v1`이 Metal API/Shader Validation을 통과했다. Line의 W 비율 1:1000 양방향 보간, D24S8 depth/stencil·occlusion, centroid·sample·EvaluateAttribute와 derivative/UAV를 확인했다. Sample-frequency derivative는 76 covered sample과 76 UAV 증가가 일치한다. Center 2/4/8 full/masked RTV·D32 DSV·occlusion과 독립적인 resource/target quality 7조합도 보존했다.
- **Stage linkage:** case-insensitive semantic+index와 element-local component 배치를 사용한다. `mixed-packed-after10`은 DXIL→DXIL·DXBC→DXIL·DXIL→DXBC 및 별도 clip/ordinary packed float2의 5개 pair에서 정확한 RGBA를 확인했다. Clip의 raw NaN builtin은 primitive rejection을 유지하고 user shadow만 유한 값으로 내보낸다. Masked helper의 atomic 반환값은 `poison` 대신 정의된 0을 사용한다. 이전 masked PS counter가 `15<<32`로 잘못 증가한 회귀도 전체 통계 oracle로 닫았다.
- **Forced raster query:** `forced-query-09fixed-evidence`의 15 case가 API/Shader Validation을 통과했다. 저장한 post-PS virtual sample coverage를 numeric/binary occlusion으로 세고, 1× color replay가 Metal visibility scratch를 초기화하기 전에 query 결과를 fanout한다. Mask·discard·tiny edge·indirect predication·layered replay와 pixel-frequency UAV를 포함한다.
- **공유 D3D11 보존:** packed VS→PS의 기존 오류(`ff800000` 대 `ff804020`)를 수정하고 기존 cache_29가 있는 같은 cache 파일에서 cache_30으로 재컴파일하여 readback을 통과했다. `d3d11-shared/runtime10-packed.log`와 실제 drawable 3,072 pixels=`ff4080bf`를 확인했다. GS→PS의 packed float2도 runtime-10에서 `(0,.625,0,.5)`였던 값을 runtime-11에서 `(.25,.625,.875,.5)`로 수정했다(`mesh-lane-before11.log`, `mesh-lane-after11.log`). 최종 mesh 경계 변경을 위해 compiler cache version은 31이다. D3D12 GS/HS/DS 지원을 추가한 것은 아니다.
- **ROV 최종 통과:** nested buffer ROG만으로는 일반 실행의 ordering이 보장되지 않아 private per-pixel/layer/sample R8Uint ROG marker를 사용한다. Texture-array read ABI를 AIR 2.7의 5인자와 AIR 2.8의 sampler·offset을 포함한 7인자로 구분한다. Direct-root helper의 logical-active metadata는 임시 SSA 값 대신 직렬화 가능한 argument index를 저장한다. `rov-stencil/runtime13-{normal,api,validation}-full15`는 일반·API-only·API/Shader Validation에서 각각 15/15 case를 통과했다. Direct/table sample-frequency 두 경로는 각각 8개 word 모두27, layers27/27/90/90, warm cache27/27이며 validation error·foreign backend load는 없다. Exact AIR의 native ShaderValidation PSO도 통과했다. Stencil DXBC/DXIL30/35의 기존 일반·Shader Validation 증거를 유지한다.
- **최종 linkage runner:** `mixed-packed-after13`은 최신 runtime과 exact marker gate로 mixed DXBC/DXIL·clip·packed 5개 pair를 다시 실행하여 Metal API/Shader Validation을 통과했다.

**SSD 정리에 따른 중간 중단 (2026-09-29).** 추가 계약 대조에서 native clip/cull user varying의 Inf가 0으로 전달되고, 살아남는 primitive의 cull NaN도 보존되지 않는 것을 runtime-13/14에서 재현했다. 단순 마스킹 제거는 일반 실행에서도 실패하고 ShaderValidation 오류도 발생했다. 최신 수정은 원래 VS의 raw 값을 캡처하여 point/line/triangle 거리 입력을 직접 보간하고, native replay의 user shadow는 유한 값으로 유지한다. 거리 입력을 소비하는 PS에는 1×에서도 이 경로를 사용하며, 실제 single-sample coverage texture·Metal helper와 Wine32 옵션 decoder를 함께 변경했다. Compiler cache version은 현재 **32**다.

`runtime-15`의 전체 Ninja/install/staging과 `clip-nonfinite/after15-{normal,validation}`이 통과했다. 1×/4× clip/cull Inf, clip NaN primitive rejection, 살아남는 cull NaN의 실제 PS 입력, point/line/triangle 및 `SV_SampleIndex`, 기존 finite IA·clip·packed 결과를 확인했다. Validation 결과는 오류·foreign backend load 없이 통과했다. CPU build/staging/diagnostics 16+21+44개도 통과했다. Wine32 wire layout·native compilation은 확인했지만 실제 32-bit 실행은 당시 미검증이었다(아래 WoW64 결과로 검증).

**재개 검증 (2026-09-30).** 전용 `clip-nonfinite-depth/probe.cpp`는 1× D32에서 early/late depth·pixel/sample-frequency PS 8개 case의 numeric/binary occlusion, UAV 호출 수, NaN cull 입력 보존과 RT readback을 `runtime-15`의 일반 실행과 Metal API/Shader Validation(`clip-nonfinite-depth/run15-{normal,validation}`)에서 모두 통과했다. 같은 runtime에서 이전 MSAA 통합 fixture 9종(346-case statistics, msaa-lines, center 2/4/8 DXBC·DXIL, forced query, ROV 15 case, attachmentless switch, ROV MSAA4, mixed/packed linkage)도 validation으로 통과했다(`msaa-regression-15/`). Linkage는 현재 runner gate가 요구하는 `DISTANCE_PASS samples=1,4`를 출력하는 현재 fixture로 실행했다.
- **Rasterization-disabled SO:** `D3D12_SO_NO_RASTERIZED_STREAM` PSO에서 실행되지 않는 PS의 clip/cull 입력이 `owned_distance_interpolation`을 켜서 불필요한 MSAA capture/replay 경로를 선택했다. `d3d12_pipeline_graphics.cpp`에서 PS가 실제로 실행될 때(`use_fragment`)만 거리 입력을 검사하도록 수정했다. `stream-output`에 finite/non-finite `SV_ClipDistance0` no-raster case 2개를 추가했다. 두 case는 수정 전 runtime-15에서도 정확한 SO 결과로 통과하므로 출력 결함이 아니라 불필요 경로 선택의 수정이다.
- **SO fixture barrier:** 이후 추가된 queue-time state 검증이 `stream-output`·`so-format-support` fixture의 잘못된 barrier를 device removal로 거부했다. Write state로 암시적 승격된 buffer를 barrier 없이 다른 상태로 쓰거나 `COMMON` before-state를 사용한 fixture 오류이며 translator 검증은 spec대로 유지했다. 두 fixture를 명시적 transition으로 수정하고 root UAV/GPU 인자용 default buffer에 `ALLOW_UNORDERED_ACCESS`를 지정했다.
- **runtime-16 통합:** 위 수정을 포함한 전체 Ninja/install/staging 뒤 `regression-16/`에서 stream-output(validation 104, GPU 45, no-raster distance 2), so-format-support, pipeline-statistics, msaa-lines, dxil-ia, raster-order, execution-safety, indexed-restart, queries가 Metal API/Shader Validation을 통과했다. `runtime-15`에서 pipeline-cache·predication-indirect·stencil-reference도 통과했다. CPU build/staging/diagnostics도 통과했다.

SSD 정리로 완료된 시험 prefix **421개**와 과거 복제 runtime **80개**의 `bin/lib/share` payload를 삭제했다. `build/local`은 **458.6→6.8 GiB**, 볼륨 가용 공간은 **7.4→457.7 GiB**가 됐다. 소스·toolchain·현재 build/install, 기본 runtime과 `runtime-13`/`runtime-15`, 기존 runtime manifest·fixture·로그·result.json·drawable capture는 보존했다. 보존한 runtime-13/15는 staging verifier의 manifest·builtin load policy·winemetal dependency 검사를 통과했다. 삭제된 과거 runtime/prefix를 다시 실행하려면 새 staging과 새 시험 prefix가 필요하다. 2026-09-30 P1 종료 뒤에는 최신 `runtime-20`, 비교용 `runtime-16`, WoW64 `build/local/wow64/runtime-final`의 payload를 유지하며, 나머지 runtime은 manifest·provenance만 보존한다.

`Present(RESTART)`는 queue에 남은 frame과 이미 Metal에 commit된 frame을 모두 폐기한다(아래 P1 종료 결과). Core Animation에 이미 넘긴 drawable은 규격상 표시 완료로 취급한다.
HDR10PLUS는 제약으로 미충족, 물리 scanout 색 정확도·다중 출력 전환은 미검증 경계로 남긴다.

### P1 완료 판정과 남은 계약

**P1 완료(2026-09-30).** A–D 및 위에 기록한 resource·format·execution·shader/PSO 결과에 더해, 거리 보간의 1× depth/occlusion·MSAA 통합 회귀, 이미 commit된 frame까지 포함한 `Present(RESTART)` 폐기, WoW64 실행을 닫았다. HDR10PLUS는 제약으로 미충족이며 명시적으로 거부한다. 물리 HDR scanout·다중 출력 전환·다른 GPU/OS는 이 장치에서 실행할 수 없는 미검증 경계다. A는 UINT와 Apple8+ UINT64, B는 present queue rotation·transactional resize, C는 MSAA를 포함한 11-field statistics, D는 VS→SO(stream 0) direct/indirect와 SO statistics 범위다. P1-C의 346 GPU case·26 validation case는 최신 compiler(`runtime-15`, `runtime-16`, `runtime-20`)에서도 다시 통과했다.

#### P1 종료 결과 (2026-09-30)

- **Native `Present(RESTART)`:** 이전에는 Metal에 commit된 present가 하나라도 있으면 `DXGI_ERROR_UNSUPPORTED`를 반환했다. 이제 drawable은 present command buffer의 GPU 완료 handler에서만 Core Animation에 넘긴다. 이때 presenter의 restart epoch를 `MTLSharedEvent` token과 비교해 바뀌었으면 present하지 않고 presentation observer도 취소한다(새 WineMetal call 214 `MTLCommandBuffer_presentDrawableIfCurrent`, 64-bit·WoW64 table 모두). Order event·credit·semaphore 회수는 그대로라 뒤 frame이 막히지 않는다. D3D11 present 경로는 변경하지 않았다. 대가로 present가 GPU 완료 후 handler에서 발생하므로 최대 한 frame의 지연이 늘 수 있다.
  - 증거: `present-queues`의 `RESTART_NATIVE` case(ResizeBuffers1 queue B/A, B 차단, frame 1 queue 대기·frame 2 native commit 뒤 RESTART). `runtime-17`/`runtime-19` 이후 S_OK·0 ms·정확한 credit·count/index와 fullscreen 복원을 통과했고, 이전 `runtime-16`은 같은 fixture에서 `0x887a0004`로 실패했다. Native 결정 trace(`OWNED_GRAPHICS_NATIVE_PRESENT_DECISION`)는 case 구간에서 `present_id=2 presented=0 reason=epoch`, `present_id=3 presented=1 reason=current`만 기록했고 runner가 이를 gate한다(`regression-17/present-queues-decision`). Core Animation `presentedTime`은 이 환경에서 모든 frame에 0을 보고해 표시 증거로 쓰지 않았다.
- **HDR10PLUS 분류:** macOS SDK 27의 `CAEDRMetadata`는 HDR10 정적 MDCV·CLL과 HLG만 받고 ST 2094-40 동적 metadata 입력이 없다. Microsoft는 `DXGI_HDR_METADATA_HDR10PLUS`/`D3DDDI_HDR_METADATA_HDR10PLUS`를 문서화되지 않은 `BYTE Data[72]`로만 정의한다. 의미 보존 변환이 불가능하므로 `DXGI_ERROR_UNSUPPORTED`로 거부하고 상태를 바꾸지 않는다(`hdr-metadata`의 `reject-hdr10-plus`).
- **WoW64(32-bit) D3D12:** 32-bit frontend는 이전에 실행 불가능했다(descriptor handle·shader-visible heap의 aborting `IMPLEMENT_ME`, 4 GiB 위 native 포인터로 인한 NULL `Map`, compiler 출력 역참조). 수정: 32-bit descriptor handle registry(stride 32, 외부·범위 밖 handle 거부), 하위 2 GiB에 할당한 CPU-visible buffer와 heap(WoW64 table의 `_MTLDevice_newBuffer32`), compiler 출력 복사 call 215 `sm50_copy_compiled_bitcode`(64-bit는 기존 zero-copy), 남은 `IMPLEMENT_ME`의 계약 오류 대체. `scripts/stage-owned-graphics.py --dxmt-install32`가 i386 module을 machine type·marker와 함께 staging·검증한다. 최종 tree의 `build/local/wow64/runtime-final`에서 PE32 fixture의 stream-output(validation 104·GPU 45·no-raster distance 2), indexed-restart(DXBC/DXIL 62 case), so-format-support, resources, smoke가 Metal API/Shader Validation으로 통과했다(`build/local/wow64/*-final`). 32-bit present/fullscreen·MSAA·DXR suite와 하위 2 GiB 고갈 동작은 실행하지 않았다.
- **Fixture 수정:** `temporal`의 opposite-order case는 이전 실행이 끝나지 않은 같은 list를 반복 제출해, D3D12 규칙(\"previous execution has not yet completed\"면 drop)을 위반했다. 이번 queue 검증 이전 runtime-15에서도 같은 device removal로 실패했다. iteration마다 별도 recording을 사용하도록 고쳤다. Runner의 stale gate(`present-queues` rejected=12, format-support의 제거된 packed422 marker)도 현재 fixture에 맞췄다. `owned-d3d12-hdr-presenter.cpp`는 frame 사이에 live EDR scale이 바뀌면(관찰값 0.0625→0.075/0.32/0.40) 서로 다른 scale의 highlight 순서를 비교해 실패했다. 순서 비교는 같은 scale일 때만 하고, 모든 frame은 기존의 scale-aware 독립 PQ/EETF oracle로 계속 검사한다. `runtime-20`에서 2회 통과했다. `colorspace-presenter`(11 frame)와 `present-geometry`도 같은 runtime에서 통과했다.
- **최종 64-bit 회귀(`runtime-20`, Metal API/Shader Validation):** smoke(present/no-present), multiqueue, temporal, core-contracts, execution-safety, fence-events, bundles, buffer-rtv, resources, residency, copy-resolve, copy-resource-casts, depth-plane-copy, reinterpret-copy, packed-cubearray, partial-3d-uav-dxil, standard-swizzle, format-support, expanded-formats, capabilities, predication-indirect, pipeline-cache, stencil-reference, indexed-restart, queries, atomic-copy, pipeline-statistics, msaa-lines, multisample-capabilities, dxil, dxil-texture, dxil-ia, raster-order, stream-output, so-format-support, raytracing, ngx, ngx-contracts, hdr-metadata, colorspace, fullscreen이 통과했다(`build/local/p1-complete/regression-20/`). present-queues(native RESTART decision gate 포함)도 최종 `runtime-20`에서 통과했다. 공유 D3D11 baseline(3D clear·DXBC compute·sample·windowed Present `4080bfff`)도 validation 오류 없이 통과했다. CPU build/staging/diagnostics와 NGX frontend 검사도 통과했다.

아래는 기존 P1 정의에 속하는 잔여 항목과 후속 구현 결과다. 2026-09-28의 소스 감사 뒤, 2026-09-29에 fullscreen 오류·전환, D3D12 HDR10 metadata와 color-space 지원·적용 순서를 실제 런타임에서 수정·검증했다. Color-space의 미지원 설정 성공 반환도 이전 runtime에서 재현했다. 이후 작업은 위 D3D11 유지 방침을 따른다.

| 잔여 항목 | 현재 근거와 상태 | 닫아야 할 계약 |
|---|---|---|
| HDR metadata | **D3D12 HDR10 적용·교체·해제 및 기존 tone-map 경로 구현·검증:** 아래 HDR10 결과 참조. D3D11 setter는 변경하지 않음. **HDR10PLUS는 제약으로 미충족:** macOS SDK 27의 `CAMetalLayer.EDRMetadata`/`CAEDRMetadata`는 HDR10 정적 MDCV·CLL과 HLG만 입력으로 받으며 ST 2094-40 동적 metadata 입력이 없다. Microsoft는 `DXGI_HDR_METADATA_HDR10PLUS`/`D3DDDI_HDR_METADATA_HDR10PLUS`를 불투명한 `BYTE Data[72]`로만 정의하고 내부 layout을 문서화하지 않았다. 따라서 의미를 보존하는 변환이 불가능하고, 성공 반환 없이 `DXGI_ERROR_UNSUPPORTED`로 거부하며 상태를 바꾸지 않는다(`hdr-metadata`의 `reject-hdr10-plus`) | native HDR10 metadata 전달·다른 display/GPU 조합은 미검증 |
| Color-space 설정 | **D3D12 지원·설정 일치, 실패 보존·resize·GPU 완료 순서 구현·검증:** 아래 color-space 결과 참조. D3D11 전용 코드는 변경하지 않음 | Core Animation 합성/물리적 scanout의 색 정확도, 다중 출력·EDR 전환, Windows reference differential은 미검증 |
| Fullscreen 오류·전환 | **선정 실패·단일 출력 전환 계약 구현·검증 완료:** 양쪽 frontend의 출력 조회·native 전환·`ResizeTarget` 오류와 상태 보존, fullscreen 생성, resize/readback/present 및 반복 이탈을 검증했다. 아래 fullscreen 결과 참조 | 다중 모니터 target 변경·hot-unplug·OS display-mode 복구 실패의 실제 검증은 남음 |
| Format·지원 보고 | **선정 정합성 수정 구현·검증 완료:** SO 12종, format/view·mip·cast, D24 copy, quality query·resource/PSO preflight와 2×/4×/8× 실행을 순서대로 검증했다. 아래 후속 결과 참조 | 98종 query와 명시한 GPU oracle의 범위이며 모든 format·dimension·연산 조합의 conformance는 아님. 다른 GPU/OS와 Windows reference differential은 미검증 |
| 공유 backend 회귀 | **기존 증거 유지:** D3D11 fullscreen·resize·GPU readback·Present와 출력 proxy 수명을 실제 실행했다. D3D10/11 전용 기능 검증은 별도 P1 목표가 아니다. | D3D12 필수 공유 변경이 기존 동작을 깨뜨리지 않는지 영향 범위만 검증 |

GS/HS/DS 및 이에 의존하는 adjacency/patch·SO 확장, reserved/tiled/shared/cross-adapter·enhanced barrier·pipeline library는 **기존 계획대로 P2**다. 이 항목들을 P1로 옮겨 완료 조건을 늘리지 않는다. UINT64의 Apple8+ 조건, MSAA의 AIR 2.8/macOS 26 target, 실제 GPU 검증이 M5 Pro/macOS 27에 한정된 점은 지원·증거 경계로 별도 유지한다. WoW64(32-bit process) 실행은 아래 P1 종료 결과에서 검증했다. 32-bit component format 12종의 x64 SO 실행은 아래 별도 결과대로 검증했다.

### P1 구현 완료 항목

2026-09-22에 선정한 P1-A/B/C/D와 이후 추가한 indexed restart·SO indirect/오류 전파·buffer UAV clear·RS 1.1 계약을 구현·검증했다. Timestamp/occlusion query, `SetPredication`, `ExecuteBundle`, `SetEventOnMultipleFenceCompletion`, `ResolveSubresourceRegion`, residency 확장과 기본 DXIL 경로도 구현되어 있다. 각 결과의 지원 범위·실행 증거·남은 제약은 아래에 유지한다.

| 항목 | 기능 | 현재 소스 상태 | 구현 계약 |
|---|---|---|---|
| **P1-A** | `AtomicCopyBufferUINT` / `AtomicCopyBufferUINT64` | **UINT와 macOS Apple8+ UINT64 publish 구현.** 실제 GPU 증거와 reader 관찰 한계는 아래 참조 | DIRECT/COMPUTE/COPY에서 primary element의 32/64-bit atomic publish와 dependent resource range ordering을 정확히 구현한다. Bundle에서는 허용하지 않는다. |
| **P1-B** | `IDXGISwapChain3::ResizeBuffers1` / present queue rotation | **구현·검증 완료**(아래 결과 참조) | backbuffer별 queue를 보존하고 `Present`의 buffer 회전과 같은 index로 present queue를 회전한다. Resize를 transactional하게 만들고 실패 시 기존 swapchain 상태를 보존한다. |
| **P1-C** | 정확한 `PIPELINE_STATISTICS` | **공개 구현·GPU 검증 완료.** DXBC 168개·DXIL 178개, 총 346 GPU case와 26개 validation case PASS | 11개 필드와 독립 UAV·RTV oracle을 검증한다. MSAA에서 실제 pixel-frequency 실행을 구현했으며 counter 나누기나 MSAA 제외로 대체하지 않는다. |
| **P1-D** | Stream Output + SO statistics | **VS→SO(stream 0) direct/ExecuteIndirect 구현·검증 완료.** P1 당시 adjacency/patch, GS/HS/DS 조합은 `E_NOTIMPL`이었다. GS/DS SO 후속 검증은 [P3 완료 결과](#p3-완료-결과-2026-10-01) 참조 | SO declaration/binding/write offset/filled-size counter와 `SO_STATISTICS_STREAM0..3`를 compiler/runtime/query까지 end-to-end로 구현한다. |
| **P1 추가 수정** | 일반 non-SO indexed primitive restart | **구현·GPU 검증 완료**(아래 결과 참조). SO 오류 전파/WoW64 및 SO ExecuteIndirect도 구현·검증 | Strip에서 cut disabled 또는 index format과 cut 값 불일치인 경우만 application index pulling; list와 일치 strip은 native 유지 |
| **P1 추가 계약** | 부분 buffer UAV clear / RS 1.1 static descriptor bounds | **구현·검증 완료**(아래 결과 참조) | Clear는 view-relative rect와 predicate를 보존한다. KEEP_BOUNDS flag의 유효 조합과 descriptor-table 경계 밖 접근 규칙을 맞춘다. |

A–D는 기존 P1 정의 안에서 먼저 닫은 개별 묶음이다. 이 목록의 완료가 D3D12 resource/format·DXGI를 포함한 P1 전체 완료를 뜻하지 않는다. 새 correctness·수명·data-corruption 문제는 D3D12 및 그 필수 공유 경로 범위에서 단계와 관계없이 먼저 수정한다.

#### P1-A — AtomicCopyBufferUINT / UINT64 구현

Microsoft 계약상 primary element는 source에서 destination으로 원자적으로 복사되며, 지정한 dependent subresource range는 primary publish 전에 완전히 갱신되어 있어야 한다. `UINT`/`UINT64` source·destination 주소는 각각 자연 정렬을 만족해야 한다. DIRECT/COMPUTE/COPY command list에는 허용되지만 BUNDLE에는 허용되지 않는다.

- record 단계에서 source/destination이 같은 owned device의 buffer인지, 4/8-byte alignment와 bounds, dependency array/count/null 조합, 각 dependent subresource range를 검증한다. Void API이므로 첫 recording error를 기존 방식대로 `Close`까지 보존한다.
- 기존 `ComputeEncoderData` pass에 native buffer binding과 내부 pipeline을 기록한다. queue worker는 command-list 객체를 다시 읽지 않는다. Dependent range는 검증하며, 전체 선행 pass의 write를 완료시키는 encoder fence를 사용하므로 별도 range snapshot은 필요 없다. Application resource는 D3D 계약대로 GPU 완료까지 유효해야 한다.
- dependent payload의 선행 write가 Metal memory model에서 visibility-complete된 뒤 primary를 publish해야 한다. 단순 4/8-byte blit을 D3D12 atomic copy와 같다고 가정하지 않는다. 지원하는 Metal atomic primitive 또는 검증된 equivalent sequence만 사용한다.
- 64-bit atomic 지원은 현재 M5 Pro 한 장치의 성공을 전체 Metal family로 일반화하지 않는다. 필요한 native capability를 명시적으로 확인하고, exact 구현이 불가능한 조합은 silent fallback 없이 unsupported로 남긴다.
- fixture는 direct/compute/copy list, 32/64-bit, dependency 0/N, natural-alignment/OOB/foreign-device 오류, late-latch 형태의 payload-before-primary 관찰을 포함한다.

공식 계약: [AtomicCopyBufferUINT](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist1-atomiccopybufferuint), [command-list API restrictions](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles).

**P1-A 결과 (2026-09-28).**

- 공통 검증 `ValidateAtomicCopy`: 같은 device의 buffer, 자연 정렬, primary bounds, `Dependencies != 0`일 때 두 배열 non-null, dependent resource 소유권, `Range.Begin <= End`, buffer는 `Subresource == 0`·`End <= Width`, texture는 subresource index 범위를 검사한다. 오류는 첫 recording error로 `Close`까지 보존된다. Bundle은 `E_INVALIDARG`.
- `UINT`: 단일 thread compute pass에서 `atomic_load_explicit`/`atomic_store_explicit`(`atomic_uint`, AIR `air.atomic.global.load/store.i32`)로 publish한다. 별도 typed pass이므로 queue의 pass 경계 `waitForFence`/`updateFence`가 앞선 payload write의 완료와 이후 소비자의 관찰 순서를 보장한다. Dependent range는 이 보수적 경계 아래서 별도 동기화가 필요 없으므로 검증만 한다. Predication 대상 연산이 아니므로 predicate를 적용하지 않는다.
- `UINT64`: macOS의 `supportsFamily(Apple8)`를 확인한 뒤 안정된 source의 `ulong` 값을 읽고, 같은 값 `v`로 `atomic_min_explicit(dst, v)` → `atomic_max_explicit(dst, v)`를 실행한다. 단일 publisher에서 중간 `min(old, v)`는 old 또는 v이고 최종 값은 v다. 두 32-bit store나 일반 blit으로 대체하지 않는다. UINT와 같은 encoder fence가 payload-before-primary 순서를 보장하며 predication을 적용하지 않는다. Capability 검사는 내부 PSO를 처음 만들 때만 수행한다.
- 이 조합은 일반적인 다중 writer atomic store가 아니다. [D3D multi-queue 계약](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)의 resource당 단일 writer·writer 전환 시 동기화가 필요하다. [Apple feature table](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)의 Apple8 macOS 64-bit min/max 지원을 기준으로 gate하며 다른 family는 `E_NOTIMPL`이다. 다른 shader용 64-bit atomic load/store 지원을 선언하는 변경은 아니다.
- 기존 UINT 증거: `graphics-translator/tests/owned-d3d12-atomic-copy.cpp`, suite `atomic-copy`. DIRECT/COMPUTE/COPY × dependency 0/1에서 같은 list 안의 source 생성 → payload copy → publish → 후속 copy 관찰, guard 보존과 18개 validation case를 Metal validation 아래 통과했다(`build/local/p1a/runtime-01`, `build/local/p1a/proof-01`).
- 리뷰 반영: command list가 소유한 내부 pipeline(atomic copy와 기존 predication kernel)을 `BeginPredicationComputePass`가 `ComputeEncoderData::internal_pipeline`으로 보유한다. 제출 뒤 list를 해제해도 queue worker가 해제된 PSO handle을 쓰지 않는다. 재검증: `proof-03`.
- Reader 관찰 범위: `Load2`는 32-bit load 두 개이므로 64-bit atomic snapshot의 oracle로 사용하지 않는다. 동시 reader fixture는 high word를 고정하고 globally coherent low-word generation과 해당 immutable payload만 관찰한다. Device memory barrier를 사용하며 유한 반복 내 같은 shader thread의 값 변화가 있어야 관찰 성공으로 판정한다. 다른 GPU/OS와 전체 64-bit shader snapshot은 별도 검증 범위다.
- UINT64 확장 증거: DIRECT/COMPUTE/COPY × dependency 0/2 × 6개 값 조합(36회), GPU compute가 생성한 source/payload, false predication 무시, 33개 validation case, 닫힌 list 재실행 2회와 제출 뒤 list 해제를 통과했다. Ordered reader control은 256개 thread 모두 최종 generation 128과 해당 payload가 정확했다. M5 Pro/macOS 27에서 API+Shader Validation을 켠 `build/local/atomic64-implementation/proof-core-final`, runtime `runtime-05`를 사용했다. 공용 내부 compute pass의 `predication-indirect` 회귀도 같은 validation 모드에서 통과했다(`proof-predication-final`).
- 동시 관찰 증거: `atomic-copy-live --api-validation`에서 256개 thread 중 64개가 같은 shader thread 안의 generation 변화를 관찰했다(전체 관찰 범위 1..128). 관찰된 payload는 모두 일치했고 ordered control도 통과했다(`build/local/atomic64-implementation/proof-live-final`). Native/owned reader의 계측 on/off 대조에서 Shader Validation이 compute 실행을 직렬화하는 것을 확인했다. 따라서 기본 suite는 계측을 켠 계약·수명·ordered control을 검증하고, live suite는 API Validation을 유지한 실제 동시 관찰을 별도로 요구한다. `LIVE_INCONCLUSIVE`는 live suite의 성공이 아니다. 기존 compiler로도 동시 관찰이 통과하여 실험용 coherent lowering 변경은 제거했다.
- Reader 입력: `graphics-translator/tests/dxil/atomic-copy-reader.hlsl`과 frozen `atomic-copy-reader.dxil`(4,124 B, SHA-256 `b075b9d8708a0b8f0fca94428762f4af7ac8b88aa7c4b7397540b6f17f375eb5`). Wine HLSL frontend가 이 reader의 `globallycoherent`/`RWByteAddressBuffer.Load`를 처리하지 못하므로 frozen DXIL을 사용한다. 아래 frozen shader 절과 같은 로컬 DXC 1.9.0.5402로 `-T cs_6_0 -E main -HV 2021 -O0`을 사용해 생성했다. Runner가 입력을 복사하고 실제 owned DXIL compiler load를 확인한다.

재현(README의 방법으로 새 runtime을 stage한 뒤, 매번 새 output 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-atomic-copy.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -ldxguid -lole32 -o build/local/atomic-copy.exe
python3 scripts/test-owned-graphics.py --runtime build/local/runtime \
  --suite atomic-copy --fixture build/local/atomic-copy.exe \
  --out build/local/atomic-copy-core-proof --validation
python3 scripts/test-owned-graphics.py --runtime build/local/runtime \
  --suite atomic-copy-live --fixture build/local/atomic-copy.exe \
  --out build/local/atomic-copy-live-proof --api-validation
```

#### P1-B — ResizeBuffers1 / present queue 구현

`ResizeBuffers1`의 `ppPresentQueue`는 backbuffer 수만큼의 D3D12 queue 배열이며, `Present`가 다음 buffer로 회전할 때 queue도 함께 회전한다. 이전의 queue 배열 무시 경로를 제거하고 아래의 backbuffer별 queue 보존·transactional resize로 교체했다.

- swapchain에 backbuffer별 `present_queues_`를 보존한다. 일반 `ResizeBuffers`는 기본 creation queue를 각 slot에 적용하고, `ResizeBuffers1`은 caller 배열을 검증·snapshot한다.
- `Present1`은 현재 backbuffer index와 동일한 index의 present queue에 task를 enqueue한다. 기존 queue-local FIFO/Wait/Present ordering을 재사용한다.
- `ResizeBuffers`와 `ResizeBuffers1`은 공통 internal transaction으로 합친다. 새 descriptor, queue vector, backbuffer vector와 layer property가 모두 준비된 뒤 commit해 중간 실패가 기존 swapchain을 반쯤 파괴하지 않게 한다.
- node mask/queue ownership/type/count를 먼저 검증하고, 기존 backbuffer reference가 남은 resize의 DXGI 오류 계약을 별도 fixture로 고정한다.
- `ApplyLayerProps`의 이전 flush TODO는 accepted present의 encode 완료 대기로 대체했고, 아래 color-space 후속 작업에서 D3D12 GPU 완료 대기로 강화했다. `SetColorSpace1`은 검증된 다음 색 공간만 저장하며 다음 Present가 같은 GPU 완료 경계에서 적용한다.

공식 계약: [IDXGISwapChain3::ResizeBuffers1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-resizebuffers1).

**P1-B 결과 (2026-09-28).**

- `d3d12_swapchain.cpp`: `present_queues_`와 `current_buffer_`를 backbuffer와 같은 index로 회전한다. `Present1`은 현재 index의 queue에 enqueue하고 성공 시에만 index를 전진한다. `GetCurrentBackBufferIndex`는 `current_buffer_`를 반환한다.
- `ResizeBuffers`/`ResizeBuffers1`은 `ResizeBuffersImpl` 하나로 합쳤다. Buffer count(0=유지, 최대 `DXGI_MAX_SWAP_CHAIN_BUFFERS`, flip model 최소 2), format, 남은 application backbuffer reference, `ResizeBuffers1`의 node mask(0/1만 허용)·queue(같은 device의 DIRECT queue, null 금지)를 먼저 검증하고, 새 desc·queue·backbuffer를 모두 만든 뒤 한 번에 교체한다. 실패는 `DXGI_ERROR_INVALID_CALL` 또는 생성 오류를 반환하고 기존 상태를 그대로 둔다. 성공 시 index는 0이다. 일반 `ResizeBuffers`는 모든 slot을 creation queue로 되돌린다.
- 당시 교체 전 `Presenter::waitPresentsEncoded()`로 이미 받아들인 present의 encode 완료를 기다렸다. 현재는 아래 color-space 후속 작업의 `waitPresentsCompleted()`로 GPU 완료까지 기다린 뒤 layer property를 바꾼다.
- Outstanding reference 검사가 내부 보유를 app reference로 오인하지 않도록 queue의 `PresentTask`와 temporal frame의 resource 보유를 private ref(`Com<MTLD3D12Resource, false>`)로 바꿨다. GPU 완료까지의 수명은 그대로 유지된다.
- 증거: `graphics-translator/tests/owned-d3d12-present-queues.cpp`, suite `present-queues --mode present`. 9개 거부 case 뒤 desc·index 보존, queue B만 fence wait로 막았을 때 buffer 0(queue A) present는 완료되고 buffer 1(queue B) present는 release 전까지 pending, 일반 resize 후 3-buffer rotation이 queue A에서만 완료됨을 Metal validation 아래 확인했다(`build/local/p1b/runtime-01`, `proof-01`). 회귀: `smoke --mode present`, `temporal` 통과(`proof-smoke`, `proof-temporal`).
- 리뷰 반영: 여러 present queue에서 뒤 frame이 앞 frame을 추월하지 않도록 presenter별 `MTLSharedEvent`(`present_order_event_`)로 frame N이 N−1을 GPU에서 기다리고 N을 signal한다. queue worker는 CPU에서 막히지 않는다. encode 완료 watermark는 최대 frame id가 아니라 완료 개수(`frames_encoded_`)라서 `waitPresentsEncoded`가 앞 frame이 남은 채로 반환하지 않는다. `SwapChainFlags`를 반영하고 `FRAME_LATENCY_WAITABLE_OBJECT` 변경은 `DXGI_ERROR_INVALID_CALL`로 거부한다. Fixture는 queue B의 frame이 막힌 동안 queue A의 다음 frame이 완료되지 않음을 검사하며, GPU order wait를 제거한 build에서 `present-order-not-overtaken`으로 실패함을 확인했다(임시 build, 증거 경로 `build/local/p1b/proof-probe`). 재검증: `runtime-03`의 `proof-03`, `proof-smoke-03`, `proof-temporal-03`.
- 남은 경계: multi-node adapter가 없으므로 node mask는 0/1만 허용한다. 단일 출력 fullscreen/resize/present는 아래 후속 fixture로 검증했지만 다중 present queue와 fullscreen 전환을 동시에 조합한 검증은 하지 않았다. 신호되지 않는 queue Wait 뒤에 present가 남아 있으면 resize는 그 present의 GPU 완료까지 기다린다. `PresentTask`의 backbuffer 참조는 기존 경로대로 encode 후 해제되며 GPU retirement까지의 보유는 Metal command buffer의 resource 보유에 의존한다. 500ms pending probe는 지연된 잘못된 queue 실행을 완전히 배제하지 못한다.

#### P1 후속 — Fullscreen 오류·전환 (2026-09-29)

- D3D12와 D3D11의 `SetFullscreenState`/`ResizeTarget` 전체 전환을 mode-set guard로 보호한다. `FALSE`와 non-null target 조합은 `DXGI_ERROR_INVALID_CALL`이며, 출력 `QueryInterface`/`GetDesc`/mode 조회 오류를 검사한다. 재진입은 `DXGI_STATUS_MODE_CHANGE_IN_PROGRESS`다. Window API와 output COM 호출 중 swapchain mutex를 보유하지 않는다.
- Native 전환 성공 뒤에만 `Windowed`·monitor·target·fullscreen descriptor를 게시한다. 실패 시 이전 논리 상태를 유지하고 실제 이전 display mode의 복구를 시도한다. Win32 WSI는 창/monitor 조회, style 변경, 위치 변경의 실패를 반환하며 살아 있는 창의 부분 변경을 되돌린다. `ResizeTarget`은 실패한 mode 변경 뒤 창을 갱신하거나 descriptor를 먼저 바꾸지 않는다.
- Fullscreen-at-creation도 실제 `SetFullscreenState(TRUE)`를 거치고 실패를 caller에 반환한다. 공유 DXGI factory는 파괴된 HWND를 Metal view 생성 전에 거부하고 output을 null로 유지한다. 기존 경로에서 발생한 WineMetal assertion을 숨기지 않고 잘못된 입력이 native layer까지 도달하지 않도록 수정했다.
- D3D11 fullscreen `Present1`은 전달받은 `IDXGIOutput1`을 `MTLDXGIOutput`으로 직접 캐스팅하지 않고 공개 `GetGammaControl`을 사용한다. 유효한 forwarding output proxy로 실제 Present 시 발생한 execute-access fault를 수정했다. Gamma curve가 바뀔 때만 cached ramp를 갱신한다.
- Permanent fixture: [`owned-dxgi-fullscreen.cpp`](tests/owned-dxgi-fullscreen.cpp), runner suite `fullscreen --mode present`. 각 API에서 8개 오류/재진입 경계, 두 번의 fullscreen 진입·이탈, 창 style/rect 복원, fullscreen 생성과 invalid HWND 생성 거부를 확인한다. Native 진입·이탈 중 `WM_STYLECHANGING`에서 HWND를 파괴하여 실패 반환과 pre-call 논리 상태 보존을 검사한다. 72×54 backbuffer의 **3,888픽셀/API**가 독립 clear 기대값과 일치하고 D3D12 fence/D3D11 query 완료 및 Present가 성공했다.
- 수정 전: `build/local/fullscreen-runner-baseline-29`에서 `SetFullscreenState(FALSE, target)`이 `S_OK`를 반환하여 실패. 중간 `fullscreen-proof-29`는 invalid HWND 생성의 assertion, `fullscreen-proof-30`은 D3D11 output proxy Present 충돌을 보존한다. 최종 `build/local/fullscreen-runtime-31` + `build/local/fullscreen-proof-31`은 **D3D12·D3D11 모두 PASS**, Metal API/Shader Validation 오류와 foreign backend 로드 없음. 빌드·install·새 runtime stage도 통과했다.
- 기존 `present-queues` 회귀는 `fullscreen-runtime-29` + `fullscreen-present-queues-proof-29`에서 PASS(거부 10개, queue rotation/blocked ordering, 일반 resize 3-buffer). 이후 변경은 invalid HWND factory 검사와 D3D11 gamma 조회다. Runner diagnostics 44개도 통과했다.
- 증거 경계: M5 Pro/macOS 27, x86_64 Wine, 단일 출력. 다중 모니터 전환·hot-unplug·다른 GPU/OS·일반 D3D10 workload와 실제 게임은 미검증이다. 외부에서 파괴한 HWND 자체는 복원할 수 없으며, OS가 display-mode 복구까지 거부하는 경우 native 복구는 보장하지 않는다. HDR·color-space·전체 format/sample 지원 보고는 여전히 P1 잔여 항목이다.

재실행(출력 경로는 새 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-dxgi-fullscreen.cpp \
  -ld3d12 -ld3d11 -ldxgi -ldxguid -lole32 -o build/local/owned-dxgi-fullscreen.exe
python3 scripts/test-owned-graphics.py --runtime build/local/fullscreen-runtime-31 \
  --fixture build/local/owned-dxgi-fullscreen.exe --suite fullscreen --mode present \
  --out build/local/fullscreen-proof-new --validation --timeout 120
```

#### P1 후속 — D3D12 HDR10 metadata (2026-09-29)

- `SetHDRMetaData`의 무처리 `S_OK`를 제거했다. HDR10은 정확한 `sizeof(DXGI_HDR_METADATA_HDR10)`와 non-null pointer를 요구하고 잘못된 경우 `E_INVALIDARG`를 반환한다. 구조체 전체를 값으로 복사하므로 호출자 메모리를 보유하지 않는다. HDR10PLUS 및 알 수 없는 type은 `DXGI_ERROR_UNSUPPORTED`이며 기존 metadata를 바꾸지 않는다.
- `NONE`은 metadata를 해제한다. 추가적인 size/pointer 제한이 공식 문서에 명시되지 않아 기존 의도대로 나머지 인자를 읽지 않고 무시한다. 위 세부 오류 선택은 이 구현의 정책이며 Windows reference differential 결과를 주장하지 않는다.
- 기존 `PresentState`가 수락한 frame의 수치 metadata를 복사한다. `changeHDRMetadata`는 metadata 유무가 바뀔 때만 PSO를 무효화하고 값 교체에는 재생성하지 않는다. Setter는 pending queue를 flush하지 않는다. 유무가 바뀐 뒤 다음 Present의 경계는 아래 color-space 작업에서 D3D12 GPU 완료까지 강화했다. 기존 display override가 있으면 swapchain metadata보다 우선한다.
- D3D12 활성화에 필요한 공유 수정 두 곳: native layer display query의 no-screen 반환값을 version 0·SRGB·metadata 0·EDR `{1,1}`로 초기화했고, BT.2408 EETF는 `KS >= 1`일 때 shoulder만 건너뛰어 같은 content/display peak의 `0/0`을 막으면서 검정색 remap을 유지한다. D3D11 전용 파일·기존 fixture·NVAPI/NGX 코드는 변경하지 않았다.
- **공개 API 증거:** `owned-d3d12-hdr-metadata.cpp`, suite `hdr-metadata --mode present`. 잘못된 HDR10 인자 4개·미지원 type 2개, 적용/교체/해제·호출자 메모리 변경·resize, fence로 막힌 accepted Present 뒤의 metadata 교체와 정상 release를 검사한다. 서로 다른 7개 frame의 원본 3,072픽셀 readback 및 GPU fence/Present가 성공했다. 이 readback은 tone mapping 전 백버퍼이며 metadata 출력 효과의 증거로 대신하지 않는다.
- **실제 출력 증거:** `owned-d3d12-hdr-presenter.cpp`는 production `Presenter`·`InternalCommandLibrary`에 링크하고 실제 HWND/CAMetalLayer drawable을 같은 Metal command buffer에서 blit/readback한다. A(peak 3000) 수락 뒤 B(10000)로 교체해도 A의 snapshot을 보존하고, B 적용·NONE 해제·B 재적용·content=display 경계의 픽셀을 독립 double-precision PQ/EETF 계산과 비교했다. 관찰된 highlight는 A `0.491211`, B `0.474121`, NONE 및 equal-peak `0.494141`; black은 약 `0.000001`, display peak는 1600, EDR scale은 0.0625였다. 분리된 두 fixture는 public API와 production presenter를 각각 증명하며 compositor 이후 화면/모니터 광출력의 end-to-end 계측은 아니다.
- **재현과 검증:** 이전 `fullscreen-runtime-31` + `hdr-metadata-baseline-02`는 zero-size HDR10에 `S_OK`를 반환하여 실패했다. 최종 `hdr-metadata-runtime-02` + `hdr-metadata-public-04`는 Metal API/Shader Validation으로 PASS(오류·foreign backend 로드 없음). Drawable probe도 `build/local/hdr-presenter-validation-02.stdout.log`에서 5개 frame·detached query PASS, 같은 이름의 stderr에 Metal API/GPU Validation 활성화와 validation 오류 없음이 기록됐다. 빌드·install·새 runtime stage와 runner diagnostics 44개도 통과했다. Detached native query는 이전 runtime에서 NaN/0을 반환했고 수정 후 `{1,1}`을 반환했다. 이 native baseline은 현재 shader를 링크한 probe로 이전 native module만 비교한 것이며, 이전 shader 실행 증거가 아니다.
- **범위:** HDR10을 metadata-guided 기존 tone mapper에 연결한 것이다. 보관한 primaries/white-point/min-luminance 전부를 native HDR10 신호로 전달한 구현이 아니며 현재 tone mapper는 luminance maxima를 소비한다. HDR10PLUS는 미지원이다. Microsoft도 metadata 전달을 보장하지 않고 pixel 해석은 `SetColorSpace1`이 결정한다고 명시한다. 다른 display/GPU·Windows reference differential은 남아 있으며 color-space 후속 결과는 아래에 구분했다.

공식 계약: [SetHDRMetaData](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgiswapchain4-sethdrmetadata). 공개 API fixture 재실행(출력 경로는 새 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-hdr-metadata.cpp \
  -ld3d12 -ldxgi -o build/local/owned-d3d12-hdr-metadata.exe
python3 scripts/test-owned-graphics.py --runtime build/local/hdr-metadata-runtime-02 \
  --fixture build/local/owned-d3d12-hdr-metadata.exe --suite hdr-metadata --mode present \
  --out build/local/hdr-metadata-public-new --validation --timeout 120
```

Production presenter probe는 위 runtime과 같은 빌드의 static library를 사용한다. 빌드 후 다음처럼 새 prefix에서 실행한다(게임 prefix 사용 금지).

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  -Iinclude -Isrc/dxmt -Isrc/winemetal -Isrc/util -Ilibs \
  -D_WIN32_WINNT=0xa00 -DDXMT_PAGE_SIZE=4096 \
  graphics-translator/tests/owned-d3d12-hdr-presenter.cpp \
  build/local/build/src/dxmt/libdxmt.a build/local/build/src/util/libutil.a \
  build/local/build/src/winemetal/winemetal.dll.a -ldxgi -ldxguid -lole32 \
  -o build/local/owned-d3d12-hdr-presenter.exe
WINEPREFIX="$PWD/build/local/hdr-presenter-prefix-new" \
  WINEDLLPATH="$PWD/build/local/hdr-metadata-runtime-02/lib/wine" \
  WINEDLLOVERRIDES=winemenubuilder.exe=d WINEDEBUG=+loaddll \
  MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build/local/hdr-metadata-runtime-02/bin/wine "$PWD/build/local/owned-d3d12-hdr-presenter.exe"
WINEPREFIX="$PWD/build/local/hdr-presenter-prefix-new" \
  build/local/hdr-metadata-runtime-02/bin/wineserver -k
```

#### P1 후속 — D3D12 color-space 지원·적용 순서 (2026-09-29)

- `CheckColorSpaceSupport`와 `SetColorSpace1`은 동일한 DXGI→native mapping과 CGColorSpace 가용성을 사용한다. 지원 RGB는 full-range G22/P709, G10/P709, G2084/P2020, G22/P2020 네 종류다. 선택된 공간은 이후 조회에서도 PRESENT를 보고한다. EDR이 1이라는 이유만으로 HDR 공간을 거부하지 않는다. 미지원 enum의 support는 0이고 setter는 상태를 바꾸지 않고 `DXGI_ERROR_INVALID_CALL`을 반환한다. null support pointer는 `E_INVALIDARG`다.
- Setter는 검증한 native 색 공간을 pending 값으로 보관한다. 다음 실제 Present만 이를 소비하므로 setter 자체의 GPU flush나 매 frame 추가 EDR query가 없다. 설정·HDR metadata 변경·Present capture/index·resize를 기존 swapchain mutex로 직렬화한다. 초기 FLOAT swapchain의 G10, 다른 포맷의 G22 동작은 유지하되 resize가 명시한 색 공간을 덮어쓰지 않는다.
- D3D12의 `synchronizeLayerProperties(true)`는 PSO/layer를 바꾸기 전에 기존 frame의 encode와 실제 Metal command-buffer 완료를 기다린다. Queue는 `Rc<Presenter>`를 inflight에 넘겨 GPU retirement까지 보유한다. 완료 개수 watermark를 사용해 queue completion 스레드의 역순 실행을 처리하고, terminal 취소는 완료 대기도 해제한다. Resize도 같은 완료 경계를 사용한다. D3D11 전용 파일과 기존 D3D11 drain 동작은 변경하지 않았다.
- **공개 API 재현/검증:** 이전 `hdr-metadata-runtime-02`의 `colorspace-baseline-public-02`는 미지원 enum 2에 S_OK를 반환해 실패했다. 최종 `colorspace-runtime-01` + `colorspace-public-02`는 지원 4종, 미지원 23종, null pointer, setter 후 support 조회, 실패한 resize 및 정상 resize, queue rotation 뒤 blocked Present 중 PQ→G10 설정을 통과했다. 8-bit/RGB10A2/RGBA16Float의 13개 frame에서 각각 원본 3,072픽셀과 fence/Present를 검사했다. 원본 readback은 색 변환 출력의 증거가 아니다.
- **실제 native layer 관찰:** 임시 `nextDrawable` observer로 원래 API를 그대로 호출하면서 layer의 CGColorSpace/pixel format/EDR 값을 읽었다. `build/local/colorspace-native-02.stderr.log`의 14개 drawable에는 미지원 setter·실패/동일/변경 format resize 뒤 BT2020 유지, pending PQ frame 뒤 G10, FLOAT(format 115) resize 뒤 PQ 유지가 기록됐다. 공개 fixture stdout은 같은 이름의 `.stdout.log`다. Observer는 제품에 포함하지 않는다.
- **GPU 출력/완료 경계:** `owned-d3d12-colorspace-presenter.cpp`는 production Presenter/shader의 실제 drawable 11개 frame을 독립 PQ oracle과 비교했다. 관찰된 mid 값은 SDR `0.788086`, PQ `0.491211`, scRGB `0.039398`이고, 다음 설정 후에도 이미 수락한 frame은 이전 값을 유지했다. CPU encoding이 끝나도 native shared-event로 막은 GPU 작업이 완료되기 전에는 다음 색 공간 capture가 반환하지 않았다. `retirement_gate=1`로 PASS했다. 이 probe는 새 static library와 native ABI가 같은 `hdr-metadata-runtime-02`, 전용 `colorspace-presenter-probe-prefix-01`에서 Metal API/GPU Validation으로 실행했으며 별도 stdout 파일은 저장하지 않았다.
- **회귀:** 최종 runtime의 `colorspace-present-queues-01`과 `colorspace-hdr-metadata-01` 모두 Metal API/Shader Validation PASS. 빌드·install·새 runtime staging, runner diagnostics 44개도 통과했다. Public suite의 첫 실행은 fixture 자체는 PASS였으나 성공 marker `FAILED_RESIZE`를 `FAIL`로 오인한 runner 판정 문제였고, 오류 marker 경계를 수정한 뒤 `colorspace-public-02`가 PASS했다.
- **경계:** 색 공간이 바뀌는 다음 Present 및 resize는 이전 GPU 작업을 기다릴 수 있다. 그것들이 반환한 뒤에만 이전 queue의 미신호 gate를 해제하는 호출 순서는 보장하지 않는다. GPU completion/native layer 태그/drawable pixels를 검증한 것이며 Core Animation 합성 이후의 색 정확도나 물리적 scanout 완료, undocumented per-drawable 색 공간 latch를 증명한 것은 아니다. 다중 출력·EDR 전환·다른 GPU·Windows reference differential은 미검증이다.

공식 계약: [CheckColorSpaceSupport](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-checkcolorspacesupport), [SetColorSpace1](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1), [CAMetalLayer.colorspace](https://developer.apple.com/documentation/quartzcore/cametallayer/colorspace).

재실행(새 출력/prefix 경로 사용):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-colorspace.cpp \
  -ld3d12 -ldxgi -o build/local/owned-d3d12-colorspace.exe
python3 scripts/test-owned-graphics.py --runtime build/local/colorspace-runtime-01 \
  --fixture build/local/owned-d3d12-colorspace.exe --suite colorspace --mode present \
  --out build/local/colorspace-public-new --validation --timeout 120
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  -Iinclude -Isrc/dxmt -Isrc/winemetal -Isrc/util -Ilibs \
  -D_WIN32_WINNT=0xa00 -DDXMT_PAGE_SIZE=4096 \
  graphics-translator/tests/owned-d3d12-colorspace-presenter.cpp \
  build/local/build/src/dxmt/libdxmt.a build/local/build/src/util/libutil.a \
  build/local/build/src/winemetal/winemetal.dll.a -ldxgi -ldxguid -lole32 \
  -o build/local/owned-d3d12-colorspace-presenter.exe
WINEPREFIX="$PWD/build/local/colorspace-presenter-prefix-new" \
  WINEDLLOVERRIDES=winemenubuilder.exe=d WINEDEBUG=+loaddll \
  MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build/local/colorspace-runtime-01/bin/wine "$PWD/build/local/owned-d3d12-colorspace-presenter.exe"
WINEPREFIX="$PWD/build/local/colorspace-presenter-prefix-new" \
  build/local/colorspace-runtime-01/bin/wineserver -k
```

#### P1 후속 — SO format capability (2026-09-29)

- `SO_BUFFER`를 1–4개 32-bit component의 FLOAT/SINT/UINT 12종에 보고한다. SO는 변환 없이 dword를 출력하므로 packed RGB32 texture의 native 지원 여부와 독립적이다. UNKNOWN/완전 typeless/16-bit/normalized/depth format에는 이 bit를 추가하지 않는다. VS→SO(stream 0) 범위와 공개 feature level은 유지한다.
- `owned-d3d12-so-format-support.cpp`, suite `so-format-support`는 format별 SO 출력과 그 버퍼를 해당 format의 IA 입력으로 다시 사용한 SO 출력을 검사한다. 12종 모두 원시 bit, 출력 byte counter와 앞뒤 guard가 일치했다. Float 음수, signed 음수, high-bit unsigned 값과 3-component stride도 포함한다. 8종 비대상 format은 SO bit가 없다.
- 이전 `colorspace-runtime-01`의 `build/local/so-format-support/baseline-06`에서 GPU 출력·IA 재입력은 모두 통과했지만 R32_FLOAT의 SO bit 누락으로 실패했다. 수정한 `so-format-runtime-01`의 `proof-02`는 12 GPU/12 capability/8 negative/12 IA 검사와 Metal API/Shader Validation을 통과했다. 기존 CPU probe의 SO 미지원 고정 assertion은 삭제했으며 실제 GPU 계약으로 대체했다. D3D11 전용 코드는 변경하지 않았다.

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  graphics-translator/tests/owned-d3d12-so-format-support.cpp \
  -ld3d12 -ldxgi -ld3dcompiler -o build/local/so-format-support/owned-d3d12-so-format-support.exe
python3 scripts/test-owned-graphics.py --runtime build/local/so-format-runtime-01 \
  --fixture build/local/so-format-support/owned-d3d12-so-format-support.exe \
  --suite so-format-support --out build/local/so-format-support/proof-new --validation --timeout 180
```

#### P1 후속 — Format·view 보고와 D24 copy (2026-09-29)

- `MIP`를 filterability와 분리하고 integer·typeless·depth mip storage를 보고한다. DXGI bit-layout family의 fully typed cast, combined DSV와 plane SRV의 역할을 구분했다. Typed buffer의 native layout/access gate를 적용하고 sRGB UAV는 지원 보고·view 생성 모두 거부한다. SO의 12종 지원 범위는 유지했다.
- Packed 4:2:2 두 format은 이 시점에는 미지원으로 거부했다. 이후 `expanded-formats`의 RGBA8 pair backing으로 구현·검증했다(위 RGB32·packed 4:2:2 행). 독립적인 footprint query는 2×1 pixel/4-byte 계산과 짝수 resource width 조건을 따른다.
- **실제 D24 data corruption 수정:** 이전 runtime은 `.25` depth clear의 readback을 packed `0x00400000` 대신 float bit `0x3e800000`으로 반환했고, 반대 방향 plane upload는 호환되지 않는 native view를 만들었다. D24 low24 ↔ native float의 GPU 변환과 native depth/stencil plane blit를 연결했다. 정확한 나눗셈과 nearest-even 정수 변환으로 `0x7fffff` 경계 및 `.1/.3/.9` clear를 보존한다. D32S8 float plane은 변환하지 않는다.
- `format-runtime-02`의 `build/local/format-support/proof-05`: **98종 query**(texture2D 92, buffer 54, compressed 21)와 별도의 shader/readback 검증 통과. 실제 GPU oracle은 depth/stencil aspect, integer·typeless mip, RGBA8 linear/sRGB, R16 UINT→FLOAT, RGBA32 UINT→FLOAT, BC1 linear→sRGB 및 sRGB UAV sentinel이다. 98종의 모든 연산을 GPU로 검증했다는 뜻은 아니다. Native cast 13조합 생성도 별도 로그로 보존했다.
- 같은 runtime의 `build/local/d24-plane-runner-03`: typed/typeless D24·D32S8 4종, 독립 clear/upload·SRV read·texture copy, 두 row/padding·placed offset·mip/array·low24 경계·guard 통과. 유효한 전체 depth subresource copy만 사용하며 금지된 depth subrectangle을 정상 계약으로 시험하지 않는다.
- 기존 `resources`, `copy-resolve`, `capabilities`는 `build/local/format-integration/*-01`에서 Metal API/Shader Validation을 통과했다. Runner diagnostics 44개도 통과했다. 순수 bit-copy helper test는 삭제하고 공개 API/GPU 회귀로 대체했다. D3D11 전용 변경은 없으며, 공용 Metal 파일의 추가 kernel은 D3D12 D24 copy만 호출한다.
- 세부 정책과 fixture 실행 recipe: [CAPABILITIES.ko.md](CAPABILITIES.ko.md). 다른 GPU/OS, Windows reference differential 및 실제 게임 결과는 별도 검증 대상이다. Sample-count와 single-plane depth 역할의 후속 결과는 아래에 기록한다.

#### P1 후속 — Sample-count 조회·생성·실행 정합성 (2026-09-29)

- Quality query, committed/placed 생성과 null-output probe, allocation-info, graphics PSO가 공통 count·quality·native layout·effective backing 판정을 사용한다. BC MSAA, MSAA UAV, RGB32 texture/RTV PSO, 3/16/32/UINT_MAX count를 native 호출 전에 거부한다. RGB32 IA/SO 지원은 유지한다.
- 1×는 ordinary texture layout과 quality 0, MSAA는 software/native/format 모두 지원하는 2×/4×/8×와 quality 0 또는 standard pattern(`0xffffffff`)을 허용한다. Center pattern(`0xfffffffe`)은 미지원으로 거부한다. Query의 Flags를 보존하고 tiled query는 tier와 일치하게 0 level을 반환한다. Typeless depth resource는 실제 flags로 고른 Depth16/Depth32 backing을 사용한다.
- `R32_UINT` MSAA 보고는 Apple9 이상에 한정해 D3D12-local로 보정했다. Integer/depth의 region MAX/MIN 실행을 default `ResolveSubresource` 지원으로 혼동하지 않으며 `MULTISAMPLE_RESOLVE`는 올리지 않는다.
- Typed D16/D32/D24S8/D32S8은 shader view가 아니라 DSV 역할로 판정한다. Depth target에도 MSAA render-target bit를 적용하고, MSAA load는 shader view에만 적용한다. R16_UNORM/R32_FLOAT와 depth plane alias의 comparison 지원을 별도로 판정한다. `build/local/format-support/single-depth-after-02`에서 4종 typed DSV/4종 alias 보고와 유효한 typeless depth resource의 Load·SampleCmp 결과를 검증했다. Invalid descriptor를 shader에 bind하는 undefined/invalid 사용은 성공 fixture에서 제외했다.
- **최종 결과:** `build/local/msaa-capability-runtime-02` + `build/local/msaa-capability-probe-01/runner-msaa-final-01`에서 **45 quality query, 15 거부 descriptor 조합, 21 GPU case PASS**. GPU case는 2×/4×/8×의 RGBA8 UNORM/typeless, R32_UINT, typed/typeless D32, D32S8 및 standard-pattern coverage다. 실제 sample별 readback과 region resolve를 확인했고 Metal API/Shader Validation 오류가 없다.
- 이전 runtime의 BC count4 false-positive와 count1 RGB32 RTV PSO 생성 성공을 각각 baseline과 `runner-msaa-rgb32-before`에서 재현했다. 수정 후에는 GPU native assertion에 맡기지 않고 API preflight에서 실패한다.
- Shared resource preflight 및 D24 방향별 lazy PSO의 회귀는 `build/local/format-integration/{depth,resources,copy-resolve}-final-01`에서 통과했다. 전체 Ninja/install, fresh staging과 runner diagnostics 44개도 통과했다. 최신 build/install 로그는 같은 디렉터리의 `build-msaa-02.log`, `install-msaa-02.log`다. D3D11 전용 소스와 shared inspector의 D3D11 정책은 바꾸지 않았다.
- 지원·증거 경계: M5 Pro/macOS 27의 명시한 조합이며 center pattern, 16× 이상, 모든 format/dimension 조합과 다른 GPU/OS를 지원·검증 완료로 승격하지 않는다. 이 묶음의 완료가 P1 전체 완료는 아니다.

#### P1-C — Pipeline Statistics 구현

M5 Pro는 Metal statistic counter set을 제공하지 않으므로 native counter나 CPU draw 인수 추정값으로 대체하지 않는다. Translator/compiler instrumentation으로 11개 `UINT64` 필드(88 byte)를 집계하며, **공개 `PIPELINE_STATISTICS` heap 생성과 Begin/End/Resolve를 지원한다.**

**MSAA 실행 수정:** 이전 native 경로는 4×4·4× MSAA에서 pixel-frequency PS의 독립 UAV가 16 대신 64를 반환했다. 새 경로는 compute VS capture → primitive 조립·bounds → 8×8 tile의 입력/coverage capture → 1× app PS → 원래 geometry의 sample별 출력 replay다. VS와 app PS를 결과 공유를 위해 중복 실행하지 않는다. Native fragment가 나눠 보고하는 geometric coverage는 atomic OR로 합치며 centroid는 capture 완료 후 규격의 full/partial/helper 순서로 선택한다.

- **Query 계약:** DIRECT list의 Begin/End/Resolve. 기록별 GPU 누적값을 첫 Begin에서 초기화하고, slot별 Begin snapshot과 End 차이를 저장한다. 여러 heap/slot의 겹친 범위, slot 재사용, ClearState, command list 재실행, bundle의 부모 query, 서로 다른 queue의 격리를 지원한다. Resolve는 8-byte 정렬·88-byte stride이며 query/resource/device/range를 검사한다. 실행 task가 allocator·query·결과 자원을 GPU retirement까지 보유한다.
- **IA·clipper:** 실제 VS epilogue가 position과 packed clip/cull distance를 primitive-vertex scratch에 한 번 capture하고, draw 뒤 compute가 list/strip/restart·instance를 조립한다. IAVertices는 cut을 제외한 소비 position, IAPrimitives는 완성 primitive를 센다. CInvocations는 rasterization이 켜진 완성 primitive 수다. CPrimitives는 w>0, depth clip이 켜졌을 때 0≤z≤w, clip/cull distance를 반영한 결과다(삼각형 n각형→n−2, line/point 0/1). x/y는 규격이 허용하는 무한 guard band를 사용하며 backface/zero-area 제거는 이 값에 적용하지 않는다. NaN, 허용된 xyz Infinity discard, w=0 경계를 safe FP로 처리한다.
- **Shader invocation:** DXBC/DXIL VS·PS·CS entry에 전용 buffer slot 25의 counter를 삽입한다. PS discard 전에 세며 early-depth rejection은 실행되지 않은 entry이므로 제외된다. 32-bit low atomic과 overflow carry로 64-bit modulo counter를 구현한다. VS를 재실행하거나 IA position 수로 VS invocation을 대신하지 않는다. Strip에서는 GPU 재사용 여부에 따라 실제 VS invocation이 IA position 수보다 클 수 있다(FS §5.3.9/§20.4.7). DXIL의 `dx.op.sampleIndex.i32`를 AIR sample ID에 연결하고 register가 없는 intrinsic-only signature의 잘못된 초기화를 제거했다. DXIL ShaderFlags의 ForceEarlyDepthStencil도 AIR에 보존한다.
- **실제 실행 기준:** predication으로 건너뛴 draw/dispatch와 GPU count로 제외된 indirect command는 0이다. ExecuteIndirect의 root UAV/constant 변경과 indexed draw를 포함한다. SO capture/overflow/query와 같은 draw에서 통계를 함께 구하며 SO overflow는 raster 통계를 줄이지 않는다.
- **MSAA depth·interpolation:** pixel-frequency·sample-frequency·PS 없는 depth draw 모두 동일한 native gather에서 unclamped center/sample Z를 만든다. 원래 DSV format·bias·viewport를 보존하고 각 draw의 저장된 Z를 PS 입력과 default depth test가 공유한다. OM 출력에서만 viewport 범위로 clamp하며 early DS는 app PS 이전에 한 번, late DS는 출력 시 한 번 수행한다. D16/D32 bias, pixel/sample/no-PS 사이의 `DepthEqual`, stencil·occlusion, mask 0/부분 mask, viewport 밖 PS 입력 Z를 검증했다. Native finite pull 값을 capture해 동적 Sample/Snapped/Centroid 평가를 보존하며 helper 값은 derivative anchor와 claim 이전 quad shuffle로 전달한다.
- **PSO/cache·비용:** query counter variant는 query 구간에서만 lazy 생성한다. MSAA 실행 variant는 query 유무와 무관하며 warm CachedPSO에서도 원 shader로 생성한다. Primitive별 capture·encoder pass·scratch와 GPU bounds readback 비용이 있고, indirect graphics는 GPU 인수 preflight로 실행 시점 count/root/VB/IB를 확정한다. Submission worker는 필요한 GPU 완료를 기다리지만 API caller를 그 완료까지 막지 않는다. MSAA shader variant는 AIR 2.8/macOS 26 target이며 실제 실행 증거는 M5 Pro/macOS 27이다. 성능 향상이나 다른 OS/GPU 지원을 이 검증으로 주장하지 않는다.
- **지원 경계:** 현재 VS/PS/CS stage와 point/line/triangle list·strip 경로를 대상으로 한다. GS/HS/DS·adjacency/patch stage/topology 지원을 이번 query 구현이 추가하지 않는다. GSInvocations/GSPrimitives/HSInvocations/DSInvocations는 해당 stage가 없는 유효 PSO에서 0이며, stage 자체의 미지원 PSO를 허용한 뒤 가짜 0을 반환하지 않는다. PIPELINE_STATISTICS1/mesh 통계는 추가하지 않았다.
- **증거:** permanent fixture `graphics-translator/tests/owned-d3d12-pipeline-statistics.cpp`, suite `pipeline-statistics`. 11개 field와 독립 VS/PS/CS UAV oracle, RTV readback, resolve sentinel을 비교한다. GPU carry probe는 low overflow와 전체 64-bit wraparound 및 다른 10개 counter 보존을 통과했다(`build/local/pipeline-statistics/carry-probe/gpu.log`). Windows reference differential, 다른 GPU, 실제 게임의 정확성·성능은 검증하지 않았다.

최종 MSAA 통합 증거는 `build/local/msaa-implementation/runtime-integration-23`의 새 runtime·전용 prefix에서 수집했다.

| 검증 | 실제 결과 | 증거 |
|---|---|---|
| 전체 `pipeline-statistics` | **DXBC 168 + DXIL 178 = 346 GPU case**, 26 validation case PASS. Metal API/Shader Validation 오류 없음, owned native compiler 로딩 확인 | `build/local/msaa-implementation/proof-msaa-acceptance-23` |
| 원래 8×8·4× MSAA 재현 | 35 covered pixels, 98 covered samples, **UAV atomic 35회**. Counter 보정이 아니라 실제 app shader 실행을 검증 | `build/local/msaa-implementation/proof-original8x8-23-transcript.txt` (최종 runtime 실행 출력 보존) |
| 8× MSAA 경계 | DXBC/DXIL 각각 35 pixels, 189 samples, atomic 35회, sample별 색. DXIL sample positions/count 포함 | 전체 fixture의 `msaa_8x_boundary`와 8× intrinsic case |
| Indexed `ExecuteIndirect` mutation | Close 이후 변경한 count 1/2·root constant/UAV·VB/IB를 사용하고 sparse slot 3, VS/PS UAV 횟수와 모든 sample 색 PASS | `build/local/msaa-indirect-fixture/proof-stage23` |
| 기존 GPU 회귀 | `stream-output`, `indexed-restart`, `predication-indirect`, `queries` 모두 Metal API/Shader Validation PASS | `build/local/msaa-implementation/proof-{stream-output,indexed-restart,predication-indirect,queries}-23` |

최종 전체 Ninja/install 로그는 `build/local/msaa-implementation/full-build-23.log`와 `install-23.log`다. CPU tooling regression은 build 16개, staging 21개, diagnostics 44개를 통과했다. 이전 실패·보류 증거(`build/local/pipeline-statistics/proof-stage-only-15`, `proof-dxil-boundaries-04`, `proof-queries-gated-01`)는 수정 전 상태이며 현재 공개 gate를 나타내지 않는다.

공식 계약: [D3D12_QUERY_DATA_PIPELINE_STATISTICS](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_query_data_pipeline_statistics), Apple [MTLCommonCounterSet.statistic](https://developer.apple.com/documentation/metal/mtlcommoncounterset/statistic).

#### P1-D — VS→Stream Output / SO statistics 구현

Stream Output는 `SOSetTargets` 하나만 구현해서 끝낼 수 없다. PSO의 SO declaration, shader lowering/instrumentation, target binding/offset, filled-size counter, overflow와 `SO_STATISTICS_STREAM0..3`가 하나의 계약이다. P1-C의 나머지 필드보다 먼저 compiler/runtime/query를 함께 변경하는 독립 작업으로 진행했다. 이후 2026-09-29에 12종 32-bit component format의 GPU capture·IA 재입력을 검증하고 `SO_BUFFER` capability도 보고한다(위 SO format capability 결과 참조).

**P1-D 결과 (2026-09-28).** 아래 1–3단계로 GS 없는 VS→SO(stream 0)의 `DrawInstanced`/`DrawIndexedInstanced`를 구현했다. 설계 근거는 D3D11.3 FS ch.14(SO), 8.4/8.12–8.16(IA), §5.3.9·§20.4.7(VS 실행 횟수)이다.

- Compiler ABI: `src/airconv/airconv_public.h`의 `SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE`(인자 type 10), vertex buffer binding 26(app index)·27(`SM50_SO_CAPTURE_CONTROL`, 32 B)·28(scratch). DXIL(`dxil_signature.cpp`)과 DXBC(`dxbc_converter.cpp`) 모두 구현했고, D3D11의 `SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT` 동작은 바꾸지 않았다. DXIL VS도 `SM50_SHADER_GS_PASS_THROUGH.RasterizationDisabled`를 따라 void로 만든다(Metal은 rasterization을 끈 pipeline에서 값을 반환하는 VS를 거부한다).
- 리뷰 반영(ReviewAll High 4·Medium 2): list topology에는 strip cut을 적용하지 않는다. indexed SO draw를 생성한 position buffer로 바꿔 Metal의 고정 restart와 assembly 규칙을 맞췄다. `StartComponent`를 PSO 검증과 compiler 모두 element 첫 성분 기준으로 해석한다. 내부 한계를 overflow로 위장하지 않고 `E_OUTOFMEMORY`로 알린다. 같은 vertex의 중복 VS 실행은 claim word로 record 하나만 남긴다. `ALLOW_STREAM_OUTPUT` 누락은 `E_INVALIDARG`다.
- 남은 경계:
  - **P1 당시 경계:** Adjacency/patch topology, GS/HS/DS와 SO 조합은 recording/PSO `E_NOTIMPL`이었다. GS/DS SO의 현재 후속 결과는 P3 절에 있다. SO PSO의 `ExecuteIndirect` DRAW/DRAW_INDEXED는 아래 추가 결과에서 구현했다.
  - Claim을 못 얻은 record의 primitive는 쓰지 않으며, completion에서 아래 terminal device 오류 경로로 처리한다. 잘못된 SO 결과를 정상 완료로 공개하지 않는다.
  - Indexed SO draw는 app index마다 VS가 한 번씩 실행돼 post-transform 재사용이 없다(규격 no-cache 상한 안). 성능은 측정하지 않았다.
  - SO type 10의 WoW64 인자 변환과 실제 32-bit compiler 호출은 아래 추가 결과에서 검증했다. 전체 32-bit SO GPU draw와 type 11의 WoW64 GPU 실행은 2026-09-30 WoW64 결과에서 검증했다.
  - **당시 경계, 현재 수정:** 이전 `dxil-ia` 회귀는 `build/legacy/...` 바이너리(`docs/provenance/gpu-dxil-ia.json`)에 의존했다(`build/local/p1d/reg/proof-dxil-ia-03`). 지금은 `graphics-translator/tests/owned-d3d12-dxil-ia.cpp`와 `graphics-translator/tests/dxil/dxil-ia-{vs,ps}.hlsl`·새 `.dxil`을 복원하여 소스 빌드 fixture로 검증한다.
- 회귀: `runtime-03`에서 pipeline-cache, queries, bundles, predication-indirect, dxil, atomic-copy, `smoke --mode present`, `present-queues --mode present`가 Metal validation 오류 없이 통과했다(`build/local/p1d/reg3/proof-*`). CPU 스크립트 build/staging/diagnostics도 통과했다.

P1 당시 진행 상황(1단계, runtime plumbing): PSO는 `D3D12_STREAM_OUTPUT_DESC`를 FS ch.14 규칙으로 검증해(`E_INVALIDARG`; GS/HS/DS 조합은 `E_NOTIMPL`) owned layout과 capture record layout을 저장하고, VS 컴파일 인자에 `SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE`를 넣는다. `RasterizedStream = NO_RASTERIZED_STREAM`이면 VS를 void로 만들고 rasterization을 끄며 fragment 단계도 두지 않는다. Pipeline cache key에는 SO declaration/stride/RasterizedStream이 들어가고 blob version은 2다. `SOSetTargets`는 slot 범위, VA 해석, 크기, 4-byte 정렬된 filled-size 위치를 검증한 뒤 allocation을 보존한다. `SO_STATISTICS` heap은 16-byte 결과와 Begin snapshot을 갖고, 결과는 각 recording의 stream counter(`SOStreamCounterBuffer`) 차이로 계산한다.

P1 당시 진행 상황(2단계, draw-time capture·assembly): SO PSO의 `DrawInstanced`/`DrawIndexedInstanced`는 render pass 안에서 VS epilogue capture(indices=vertex buffer 26, control=27, scratch=28)를 실행하고, render pass가 끝나면 draw마다 단일 thread `so_assemble` compute를 API 순서대로 돌린다. Record key는 0-based IA position k이고, record는 `VertexCountPerInstance × InstanceCount`개다. Scratch는 recording 시점에 크기가 정해지며(record 뒤에 record당 claim word 1개), CPU 대기는 없다. `UINT32_MAX` record나 `maxBufferLength`를 넘으면 `E_OUTOFMEMORY`를 기록한다. Indexed SO draw는 app index buffer 대신 생성한 uint32 position buffer(`positions[k] = k`, D3D12 strip cut이 적용되는 위치만 `0xFFFFFFFF`)로 Metal indexed draw를 낸다. VS가 app index를 직접 읽어 `SV_VertexID` = index 값(BaseVertexLocation 제외, FS 8.16), vertex fetch = index + BaseVertexLocation으로 쓴다. 그래서 Metal restart와 assembly cut 규칙이 같다. VS 실행 수는 IA position 수로, D3D가 허용하는 no-cache 상한 안이다. 한 vertex가 여러 번 shade되면 claim word(cmpxchg 0→1)를 얻은 invocation만 record를 쓰므로 record 성분이 섞이지 않는다. Assembly가 claim 없는 record를 만나면 그 primitive를 건너뛰고 status word로 세며, completion에서 로그와 함께 `DXGI_ERROR_DRIVER_INTERNAL_ERROR`를 device에 보존하고 대기 중인 fence를 종료한다. Assembly 규칙: strip은 list로 풀고, 홀수 strip 삼각형은 `(i, i+2, i+1)`로 leading vertex와 winding을 보존한다(MS Triangle Strips 문서, FS 8.14, VK_EXT_provoking_vertex의 D3D11 SO 순서 요구). `IBStripCutValue`에 맞는 cut은 LINESTRIP/TRIANGLESTRIP에만 적용하고(list에서는 cut 값도 일반 index), 미완성 primitive는 버린다(FS 8.12-8.13). Primitive는 참조된 모든 slot에 들어갈 때만 쓰고, overflow 뒤에는 쓰지 않으며 needed만 증가시킨다. 참조 slot이 미바인딩이면 full로, 참조하지 않는 slot이 bound돼 있으면 무시한다(FS 14.5-14.6). 끝나면 filled size를 write back하고 stream 0 counter에 누적한다. Predication은 기존 predicate snapshot을 assembly에도 적용한다. PSO 검증: root signature(명시 또는 embedded)에 `ALLOW_STREAM_OUTPUT`이 없으면 `E_INVALIDARG`이고, `StartComponent`는 packed output register의 `.x`가 아니라 element 첫 성분 기준이다(DXBC/DXIL 동일). 현재 adjacency/patch topology는 recording `E_NOTIMPL`이다. SO PSO의 `ExecuteIndirect`는 아래 추가 결과에서 구현했다. 일반 non-SO indexed restart의 과거 가정은 아래 추가 결과에서 정정했고, 불일치 strip만 application index pulling으로 수정했다. 증거: `graphics-translator/tests/owned-d3d12-stream-output.cpp`, suite `stream-output`(총 87 case, 그중 GPU capture 28개: list/strip/홀수 strip 순서, indexed 반복/restart, instancing, 다중 slot·gap·stride, 두 draw·두 list append, exact fit, overflow, 미바인딩 slot, NO_RASTERIZED vs rasterized RT, non-indexed VS invocation이 SO 없는 동일 draw와 같고 indexed는 no-cache 상한 안, list/DISABLED에서 cut 값은 일반 vertex, sparse R16/R32 index, R32 strip cut, record 단일 invocation(ticket), packed DXIL semantic, `ALLOW_STREAM_OUTPUT` 누락, predication on/off, SO_STATISTICS 0..3), `build/local/p1d/runtime-03`, `build/local/p1d/proof-11`.

### P1 추가 결과 — SO ExecuteIndirect

- VS-only SO PSO의 DRAW/DRAW_INDEXED indirect 명령을 지원한다. GPU argument/count buffer, MaxCommandCount clamp, predication snapshot, command별 root/VB/IB 갱신과 API 후 상태 reset을 유지한다. 이전 `Close=E_NOTIMPL`은 실제 기존 runtime에서 두 draw 형식 모두 재현했다(`build/local/so-indirect/baseline-so-e-notimpl.txt`).
- `d3d12_command_list.cpp`는 SO indirect를 독립 render pass에 기록하고 SO target·layout과 root/VB 초기 상태를 보존한다. Signature는 packed draw/IBV byte offset을 저장한다. `dxmt_command.metal`의 준비 kernel은 GPU가 정한 실행 count와 compact draw/IB 정보를 읽는다. Count/predicate가 있으면 먼저 count만 읽고, 실행되는 명령 수만큼만 readback을 할당한다.
- Queue worker가 producer submission의 실제 retirement를 기다린 뒤 실행별 capture scratch·claim·index positions·control·ICB·root/VB 상태를 만든다. Allocator의 녹화 상태는 수정하지 않고 새 메모리는 GPU completion까지 보유한다. VS는 기존 draw 안에서 한 번만 실행하며 SO record는 기존 epilogue로 capture한다. Indexed draw는 생성한 IA positions 위에서 실행하고 application index/BaseVertex는 VS가 읽는다. Assembly는 명령 순서대로 append·overflow·needed 통계를 반영한다.
- 성능 경계: SO indirect에는 GPU→CPU readback과 queue-worker 대기가 있다. GPU count/predicate가 있으면 count와 draw 정보의 두 준비 단계를 사용한다(count=0이면 두 번째 생략). API caller는 비차단이다. 완전 GPU-only indirect 구현이나 성능 개선을 주장하지 않는다. 실제 record 수가 uint32/Metal buffer 한계를 넘거나 실행 메모리를 할당하지 못하면 device 오류로 전파하며 SO output을 임의로 잘라 성공 처리하지 않는다.
- 리뷰 반영: PSO가 읽는 VB slot뿐 아니라 signature가 갱신하는 slot까지 table을 할당한다(미사용 slot 31 포함). Zero-instance indexed 명령은 큰 index count 때문에 positions 크기 제한이나 IB 접근을 수행하지 않으며 SO ICB resolver도 no-work 명령을 생략한다. Completion용 SO owner 저장 공간도 GPU setup 전에 allocation-error 처리 범위 안에서 확보한다. Zero-instance UINT_MAX case는 이 호스트의 수정 전 runtime-03에서도 통과했으므로 이 경계의 failing-before 증거로 주장하지 않는다. 강제 OOM 실행은 검증하지 않았다.
- 최종 검증: 전체 Ninja build/install 후 새 `build/local/so-indirect/runtime-05`, Metal API/shader validation 아래 `proof-stream-output-05` PASS. **총 102 case 중 GPU capture 43개**이며, 기존 28개에 SO indirect 15개를 추가했다. GPU가 쓴 인수/count(0·clamp), 두 draw 형식, predication, append/overflow·미바인딩 needed 통계, packed DXIL, 혼합 R16/R32 및 root/VB/IB 갱신·slot 31, closed-list 인수 변경 재실행, zero-instance 큰 count를 실제 readback으로 검사한다.
- 관련 회귀: `build/local/so-indirect/proof-indexed-restart-01`(runtime-01, DXBC/DXIL 총 62 GPU case), `proof-predication-indirect-03`(runtime-03) validation PASS. `python3 scripts/test-owned-graphics-diagnostics.py` 42 tests PASS. 두 reviewer의 지적을 수정하고 좁은 재검토에서 남은 지적이 없음을 확인했다. 전체 D3D12 conformance, 실제 게임 성능, 32-bit SO GPU 실행은 이 결과에 포함하지 않는다.

### P1 추가 결과 — SO 오류 전파와 WoW64 capture 인자

- SO assembly의 missing-claim status를 순차 completion 스레드에서 검사하고 최초 `DXGI_ERROR_DRIVER_INTERNAL_ERROR`를 device에 보존한다. `GetDeviceRemovedReason`은 이 원인을, 이후 queue/fence/resource 생성·Map·Present의 오류 반환 경로는 `DXGI_ERROR_DEVICE_REMOVED`를 노출한다. 이미 등록된 fence 대기는 terminal `UINT64_MAX`로 풀고 늦은 GPU/CPU signal이 이를 되돌리지 못하게 한다.
- Fence의 native event probe는 SO 검증 전에 정상 완료를 공개할 수 있어 제거했다. Queue의 앞선 submission 검증을 마친 completion 스레드만 GPU fence 값을 공개하며, consumer worker는 latch를 기다린다. Device 오류로 폐기하는 Present는 shared presentation-order event를 `UINT64_MAX`로 signal하고 frame-latency semaphore credit도 반환한다. 이미 제출된 뒤 frame의 GPU wait와 종료 대기가 남지 않게 한다.
- `winemetal_unix.c`는 SO type 10의 32-bit element 배열과 semantic name을 소유하는 native 저장소로 변환한다. 다섯 compile thunk가 변환 실패를 compiler 오류로 전달하고 부분 할당을 해제한다. Null compiler error에서도 오류 메시지 조회가 안전하다. 32-bit DLL 빌드에서 드러난 RT export-name 크기 포인터는 public uint64/transport size_t ABI를 유지하는 local 변환으로 수정했다. 이 RT 함수의 실제 호출은 검증하지 않았다.
- 실제 GPU 실패 재현: `build/local/so-integrity/probe-baseline-02`는 missing record status=3인데 device reason은 S_OK였다. 수정 `runtime-02`의 `probe-fixed-02`는 같은 status에서 reason `0x887a0020`, 세 fence의 `UINT64_MAX`, 사전 등록 이벤트 해제, 이후 queue Signal/Wait·event 등록의 `0x887a0005`를 확인했다. 정상 SO는 3 vertices/48 bytes였다.
- 다중 queue Present 실패 재현: `probe-present-before-02`는 frame N이 future fence 뒤에 있고 N+1이 GPU에서 N을 기다릴 때 독립 queue의 SO 오류 후 latency wait가 timeout(258)됐다. `probe-present-fixed-01`은 latency wait 성공, 이후 Present의 device-removed 오류, 정상 queue/swapchain 해제와 exit 0을 확인했다. 두 GPU probe는 Metal API/shader validation을 켰으며 production fault-injection 설정을 추가하지 않았다. 임시 probe는 제거하고 로그를 남겼다.
- 실제 Wine WoW64 compiler 경로: 기존 native 모듈은 존재하지 않는 semantic을 묵인했지만 수정 후 이름을 포함한 오류로 거부했다. 유효한 capture element 두 개의 컴파일, 두 번째 element의 범위 오류, null-error 메시지 조회도 확인했다(`build/local/so-integrity/wow64-so-capture-proof.txt`). 전체 32-bit D3D12 SO GPU draw나 강제 OOM 실행을 검증한 것은 아니다.
- 빌드·회귀: 전체 Ninja build/install 및 i686 winemetal.dll build, CPU `queue-order-scheduler.test.cpp` 통과. `runtime-01`의 `proof-stream-output-01`(총 87 case, GPU capture 28), `proof-indexed-restart-01`(DXBC/DXIL 총 62 GPU case), 최종 `runtime-02`의 `proof-fence-events-02`와 `proof-present-queues-02`가 validation PASS다. 경로는 모두 `build/local/so-integrity/` 아래다.

### P1 추가 결과 — 일반 non-SO indexed primitive restart (2026-09-28)

Apple M5 Pro의 native Metal direct draw와 ICB probe에서 list topology는 R16/R32 index 최댓값을 **restart하지 않았다**. 과거의 “Metal은 모든 indexed draw에서 최댓값을 restart한다, 따라서 모든 list/draw를 다시 써야 한다”는 가정은 **오류로 정정한다**. 문제는 strip의 `IBStripCutValue`가 disabled이거나 현재 R16/R32 index format과 cut 값이 불일치할 때 Metal의 고정 restart와 D3D12 계약이 달라지는 경로다. 일치하는 cut-format strip과 list는 direct/ICB 모두 native indexed draw를 유지한다.

- 불일치 strip만 nonindexed Metal draw로 내리고 VS에서 application index를 읽는다. VS의 `SV_VertexID`는 app index+base vertex, input attribute fetch도 같은 값을 사용한다. Index buffer는 GPU에서 원본을 그대로 읽으며, 새 generated IB·prepass·CPU readback은 없다. Predicated direct draw, GPU argument/count `ExecuteIndirect`, command별 IB view·format/root/VB 갱신도 처리한다. 기존 SO indexed capture/assembly 경로는 그대로다.
- Compiler: `src/airconv/airconv_public.h`의 header-only `SM50_SHADER_D3D12_INDEX_PULL`(type 11), vertex bindings 26=`VERTEX_INDICES`, 27=`VERTEX_INDEX_CONTROL`(generic 16 B). SO type 10의 control은 기존 32 B 그대로이며 두 mode는 상호 배타적이다. DXBC/DXIL의 VS index·input lowering은 공통 compiler helper를 사용한다. Type 11의 `sm50_compilation_argument32_convert`도 추가했으나 WoW64 실제 실행은 미검증이다.
- Graphics PSO는 원본 VS source/root signature/input elements를 보존하고 normal/ICB 각각에 대해 index-pull variant를 lazy 생성한다. Variant별 immutable constants를 분리하여 normal/ICB PSO와 혼용하지 않는다. Cold/warm pipeline cache 회귀는 통과했지만 **pull variant 자체는 serialized cached blob에 들어가지 않는다**. 영향을 받는 첫 draw에서 컴파일된다.
- 실패 재현: 기존 runtime `build/local/p1d/indexed-restart-before-01`에서 R16 line strip cut disabled의 pixel 3·5가 expected 1 / actual 0; `build/local/p1d/indexed-restart-mismatch-before-01`에서 R32와 cut FFFF의 불일치. 수정 뒤 `build/local/indexed-restart/proof-indexed-restart-02`은 `runtime-02`에서 초기 28×DXBC/DXIL=56 case PASS. 최종 `build/local/indexed-restart/proof-indexed-restart-03`은 `runtime-02`, Metal validation 아래 DXBC 31 + DXIL 31 = **62 GPU case PASS**. Direct/list/strip, cut disabled/mismatch/match, base vertex·start instance, predication, GPU count/arguments, simple strip R16/R32, command별 R16→R32→R16 ICB native→pull→native와 IB view offset 전환을 포함한다. `runtime-02`는 이전 `runtime-01` 이후 generated resolver scope만 수정한 production build이며 최종 matrix를 위한 추가 runtime 변경은 없다.
- 같은 `runtime-02`의 `build/local/indexed-restart/proof-predication-indirect-02`, `proof-bundles-02`, `proof-pipeline-cache-02`도 validation PASS. 최초 새 runtime의 `proof-dxil-ia-01`과 `proof-stream-output-01`도 PASS. `python3 scripts/test-owned-graphics-diagnostics.py` 42 tests PASS. 기존 SO fixture는 **총 87 case(그중 GPU capture 28개)**이며, 이번 indexed 62개는 별도 suite 결과다.

구현 소스는 `src/d3d12/d3d12_command_list.cpp`, `d3d12_command_signature.cpp`, `d3d12_command_allocator.cpp`, `d3d12_pipeline_graphics.cpp`, `src/airconv/airconv_public.h`, `dxbc_converter.cpp`, `dxil_signature.cpp` 및 공유 compiler helper다. 소스 fixture는 `graphics-translator/tests/owned-d3d12-indexed-restart.cpp`와 `graphics-translator/tests/dxil/indexed-restart.hlsl`·세 DXIL 입력이다. 복원한 `owned-d3d12-dxil-ia.cpp`는 기존 `compute.dxil vertex.dxil pixel.dxil` 다음에 재구성한 `dxil-ia-vs.hlsl`/`dxil-ia-ps.hlsl`에서 새로 빌드한 `dxil-ia-vs.dxil`/`dxil-ia-ps.dxil`을 받는다. 이전 runtime에 대해 소스 빌드 fixture를 `build/local/ia-restore/proof-source-built-01`에서 먼저 검증했다. Runner의 `dxil-ia` suite에는 이제 legacy binary dependency가 없다.

기존 Meson install 뒤 새 runtime/prefix/evidence 경로에서 재현한다(아래 runner가 fixture 옆에 shader 입력을 복사하고 fixture의 argv 순서대로 전달한다):

```sh
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -municode -static \
  -Iinclude/native/directx graphics-translator/tests/owned-d3d12-dxil-ia.cpp \
  -ld3d12 -ldxgi -ldxguid -ld3dcompiler -lole32 -o build/local/dxil-ia.exe
"$LLVM_MINGW/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -static \
  -Iinclude/native/directx graphics-translator/tests/owned-d3d12-indexed-restart.cpp \
  -ld3d12 -ldxgi -ldxguid -ld3dcompiler -lole32 -o build/local/indexed-restart.exe
python3 scripts/stage-owned-graphics.py stage \
  --dxmt-install build/local/install --wine-source toolchains/wine-runtime \
  --wine-dest build/local/indexed-restart/runtime-reproduce --with-nvapi --with-nvngx
python3 scripts/test-owned-graphics.py --runtime build/local/indexed-restart/runtime-reproduce \
  --suite dxil-ia --fixture build/local/dxil-ia.exe \
  --out build/local/indexed-restart/proof-dxil-ia-reproduce --validation
python3 scripts/test-owned-graphics.py --runtime build/local/indexed-restart/runtime-reproduce \
  --suite indexed-restart --fixture build/local/indexed-restart.exe \
  --out build/local/indexed-restart/proof-indexed-restart-reproduce --validation
```

두 fixture의 argv 계약은 각각 `compute.dxil vertex.dxil pixel.dxil dxil-ia-vs.dxil dxil-ia-ps.dxil`, `indexed-restart.hlsl indexed-restart-procedural-vs.dxil indexed-restart-attribute-vs.dxil indexed-restart-ps.dxil`이다. 위 runtime/output은 **실행 전 없는 새 경로**로 바꿔야 한다. 검증은 M5 Pro/macOS 27의 x86_64 Wine 경로이며 다른 ABI/GPU에 대한 선언은 아니다.

### P1 추가 결과 — buffer UAV 사각형 clear와 RS 1.1 descriptor bounds

- `ClearUnorderedAccessViewUint/Float`: rect를 `[0, view width) × [0, view height)`로 클리핑한 뒤 RAW DWORD base 또는 typed native view의 alignment prefix를 한 번만 더한다. 빈 rect는 건너뛰며 predicated dispatch도 같은 클리핑된 크기를 사용한다. 기존 clear shader/ABI는 바꾸지 않았다.
- 수정 전 `FirstElement=8`인 RAW UAV의 rect clear가 뷰 밖 0번 DWORD를 `0x12340000`에서 `0xffffffff`로 덮는 것을 실제 GPU readback으로 재현했다(`proof-uav-before`).
- `predication-indirect`에 **60 GPU case**를 추가했다: RAW UINT, R32/RGBA32 UINT·FLOAT, FirstElement 8/72, 전체/부분 clear, predicate 없음/실행/건너뛰기. 256-byte native view 정렬, 37-element dispatch tail, 음수·넘치는·빈 rect, 전체 backing buffer의 prefix/suffix sentinel과 32-bit UINT 값 보존을 확인한다. FLOAT clear는 typed FLOAT view만 사용한다.
- RS 1.1의 `DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS`와 각 유효한 data flag 조합을 허용한다. `KEEP_BOUNDS | DESCRIPTORS_VOLATILE`, `DESCRIPTORS_VOLATILE | DATA_STATIC`, sampler의 data/KEEP_BOUNDS flag는 `E_INVALIDARG`다. Descriptor table을 bounds 없는 root descriptor로 승격하지 않는다.
- 기존 runtime에서는 유효한 KEEP_BOUNDS 조합 네 개가 `E_INVALIDARG`이고 금지된 `DESCRIPTORS_VOLATILE | DATA_STATIC`은 `S_OK`인 것을 재현했다(`proof-root-before-static`). `core-contracts`의 **22개 조합**은 정상 serialize → deserialize의 범위/flags/type 보존 → CreateRootSignature와 잘못된 조합 거부를 검증한다.
- `dxil` suite는 기존 frozen `null-descriptor.dxil`을 추가로 staging한다. KEEP_BOUNDS table을 실제 dispatch해 null CBV/SRV read와 null/OOB atomic 반환값 0, 16-byte UAV view 바깥·64-KiB allocation 안의 store/atomic 무효화, 전체 allocation sentinel 보존, 정상 table/root UAV write·read를 검증한다. 기존 RS 1.0 table, register-space, compute, graphics 경로도 유지한다.

최종 전체 Ninja build/install 후 새 `build/local/p1-contracts/runtime-01`에서 Metal validation으로 다음을 통과했다. 모든 evidence는 같은 `build/local/p1-contracts/` 아래에 있다.

| Suite | 증거 | 결과 |
|---|---|---|
| `predication-indirect` | `proof-uav-final` | 새 60 GPU case와 기존 clear/indirect 회귀 PASS |
| `core-contracts` | `proof-root-after` | 새 RS 1.1 22개 조합과 기존 기본 계약 PASS |
| `dxil` | `proof-dxil-after` | KEEP_BOUNDS GPU readback과 기존 compiler/binding/graphics 회귀 PASS |

`python3 scripts/test-owned-graphics-diagnostics.py` **42 tests PASS**. 빌드 로그는 `build-01.log`다. `core-contracts`와 `dxil` fixture는 llvm-mingw의 현행 기본 D3D12 header로 `-std=c++20 -O2 -municode -static` 빌드하며, 오래된 `include/native/directx`를 include 경로로 강제하지 않는다. Runner가 필요한 shader를 복사하므로 `--suite core-contracts|dxil|predication-indirect`, 해당 `--fixture`, 새 `--out`, `--validation`으로 재현한다.

검증 범위는 M5 Pro/macOS 27, x86_64 Wine의 격리 fixture다. Windows differential·다른 GPU/ABI·실제 게임 호환성 검증은 아니다. P1 전체 규격 완료나 지원 tier 상승을 뜻하지 않으며, 이후 공개한 `PIPELINE_STATISTICS`와 MSAA의 검증 범위는 위 P1-C에 별도로 기록했다.


## P2 완료 결과 (2026-10-01)

**P2 완료.** 7개 slice(SM, BINDLESS, GSTS, BARRIERS, TILED, LIBRARY, SHARING)를 모두 구현하고 Metal API/Shader Validation 아래 GPU 검증했다. GSTS가 검증된 뒤 보고 Shader Model을 6.5로 올렸다. 모든 증거 경로는 `build/local/p2/` 아래이고, 실제 GPU 증거는 M5 Pro/macOS 27에 한정한다. 각 suite fixture는 `graphics-translator/tests/owned-d3d12-<suite>.cpp`, gate는 `scripts/test-owned-graphics.py`에 있다. **이 절의 cap·거부 동작·제약·미검증 목록은 P2 종료 당시의 역사적 기록이다.** 이후 RayQuery·feedback·view instancing과 GSTS/SO 제한을 변경한 현재 판정은 P3 절을 우선한다.

| Slice | 새 suite | 상태 |
|---|---|---|
| SM | `sm6` | 완료·검증. 보고 SM **6.5** |
| BINDLESS | `bindless` | 완료·검증 |
| BARRIERS | `enhanced-barriers`, `render-passes` | 완료·검증 |
| TILED | `tiled` | 완료·검증 |
| LIBRARY | `pipeline-library` | 완료·검증 |
| SHARING | `sharing` | 완료·검증 |
| GSTS | `geometry`, `tessellation` | 완료·검증 |

### P2-SM — Shader Model 6.x compiler

- **구현 요약:** DXIL compiler는 SM 6.8까지 입력을 받는다(`SM50_MAX_DXIL_SHADER_MODEL=0x68`, 6.9 이상 거부). Wave/quad op는 32 lane 고정이며 `[WaveSize]`가 32를 허용하지 않으면 compile 오류다. PS helper lane은 `WaveOpsIncludeHelperLanes`가 없으면 wave op에서 제외하고 quad op에는 포함한다. int64(전 stage), 16-bit 연산·buffer·stage I/O, min precision, packed op, compute quad derivative·implicit LOD·`CalculateLOD`, barycentrics·`GetAttributeAtVertex`, `SV_Start*Location`, SM6.8 barrier를 구현했다. 64-bit raw/structured UAV atomic은 device 전역 65536-entry lock table로 **compute에서만** emulation한다. Graphics stage의 64-bit atomic, groupshared/typed 64-bit atomic, FP64는 compile 단계에서 거부한다. Wave shader PSO 실패의 원인은 접미사 없는 lane intrinsic이 Metal compiler service를 중단시킨 것이었다(`.i32` 표기로 수정). `TotalLaneCount`는 IOKit GPU core 수×128이며 WineMetal call 256에서 읽는다.
- **보고 cap:** `HighestShaderModel` **6.5**(`kOwnedReportedShaderModel = D3D_SHADER_MODEL_6_5`, `src/d3d12/d3d12_device.hpp:151`). `sm6` gate는 `OWNED_D3D12_SM6_CAPS shader_model=0x65`를 요구한다(`scripts/test-owned-graphics.py:976`). SM6.1–6.5 요구사항을 DXC wiki·DXIL.rst·DirectX-Specs 기준으로 대조했다. Cap으로 gate되지 않는 요구사항은 모두 구현·GPU 검증했다(VS wave는 `vs_wave`). 보고하지 않는 cap-gated 기능은 PSO 생성에서 깨끗하게 실패한다. GS/HS/DS DXIL은 P2-GSTS에서 검증했다. **SM 6.6은 계속 막혀 있다:** 모든 stage의 64-bit atomic을 요구하는데, fragment stage의 lock emulation은 Apple GPU를 hang시킨다(아래 증거). WaveOps TRUE(lane 32/32, TotalLaneCount 2560), Int64ShaderOps TRUE, Barycentrics TRUE, Native16Bit TRUE. DoublePrecision·AtomicInt64OnTyped/GroupShared/DescriptorHeap·OPTIONS9·OPTIONS11 FALSE, WaveMMA·ViewInstancing NOT_SUPPORTED, MinPrecision 16_BIT 유지.
- **6.5 상향 때 추가한 구현:**
  - `SV_ViewID`(6.1), P2 당시: ViewInstancingTier가 NOT_SUPPORTED이므로 규격대로 0을 읽는다(`dxil_signature.cpp`).
  - fp32 denorm mode(6.2): `any`·`ftz`는 native다. M5 Pro는 모든 Metal 옵션(`-fdenorms`, `-fno-fast-math`, `-fmetal-math-mode=safe`, LLVM `denormal-fp-math=ieee`)에서 fp32 denormal 입력과 출력을 flush한다(`smaudit/evidence-denorm-native`). 그래서 `preserve`를 exact software 경로로 구현했다. 이전에는 이 속성을 무시해 flush했다. add/sub/mul/div는 `src/airconv/shaders/air_denorm.metal`의 exact helper로, fcmp는 IEEE bit 순서 비교로, FMad는 fuse하지 않은 exact mul+add로 바꾼다. dot과 float wave Sum/Product도 exact helper로 계산한다(`dxil_operations.cpp`). Groupshared·UAV·SV_Target store는 native로도 denormal bit를 보존한다. DXIL.rst가 flush를 허용하는 op(FMin/FMax, sqrt/rsqrt, round*, frc, log/exp/trig)는 `preserve`에서도 native로 flush한다. fp16 denormal은 native로 보존된다.
  - P2 당시 보고하지 않는 cap-gated 기능을 거부했다. SV_ShadingRate(VRS Tier 2), RayQuery(RaytracingTier 1.1), sampler feedback은 `dxil_metadata.cpp:924-943`에서 명시적 오류 메시지와 함께 `E_FAIL`을 반환한다. 이전에 SV_ShadingRate는 compile에 성공했고, 나머지 둘은 무관한 내부 오류로 실패했다. View instancing(count 2)과 MS subobject는 pipeline stream 단계에서 `E_INVALIDARG`를 반환한다.
- **GPU 증거:** `smaudit/sm6-02`(API/Shader Validation): 26 CASE_PASS, stress 20/20. 새 case는 `sv_viewid`(VS·PS·varying 0), `cap_rejected`(`ps_shadingrate`·`cs_rayquery`·`ps_feedback`·`ps_viewid_vi`·`ms_mesh`), `denorm_any/ftz/preserve`다. `denorm_preserve`는 HLK Denorm*Preserve vector 13종과 denormal·tiny·special 혼합 입력을 쓰는 64 lane case이며, DAZ/FTZ를 끈 host SSE와 bit-exact로 일치한다. Native prototype의 add/sub/mul/div 4,194,304 random pair에서도 mismatch는 0이었다. Case 추가 run은 `sm65cases/run-01`(23 case, Metal validation 오류 0)이다. 회귀: `smaudit/{dxil,dxil-texture}-01`. 이 run들은 6.5 상향 전 binary라 `shader_model=0x51`을 출력한다. `0x65` 보고는 최종 통합 run에서 확인한다(아래 P2 통합). 이전 증거: `sm/sm6-02`·`sm/sm6-03-novalidation`(20 case), `sm/sm6-04`, CS 64-bit atomic stress(65536 thread×1/16 target 20/20 exact), 회귀 `sm/{dxil,dxil-ia,dxil-texture,raster-order,pipeline-statistics,capabilities}-02`.
- **제약 증거:** `sm/evidence-fragment-lock`. Native MSL fragment spin lock은 validation 없이 200회 중 83회만 완료되고 나머지는 GPU hang/victim이었다. `MTL_SHADER_VALIDATION=1`에서는 항상 hang된다. 같은 lock loop는 compute에서 오류가 없다. Apple native u64 min/max는 AIR 2.9에서만 compile되고 반환값이 없다. Typed/threadgroup i64 atomic은 Metal PSO 생성에서 충돌한다. FP64 ALU는 없다. `texture2d_ms`는 read-only라 `TextureStoreSample`(op 225)은 거부한다.
- **미검증:** `SampleCmpLevel/Bias/Grad`·`GatherRaw`·64-bit gather(구현만 있음), sampler MinLOD/MaxLOD clamp를 반영한 `CalculateLOD`(shader에서 보이지 않음), compute sampling의 sampler LOD bias(0만 검증). Denorm `preserve`는 stage와 무관하게 적용되지만 GPU case는 CS뿐이다(VS/PS/GS/HS/DS 미검증). `preserve`에서 f16 denormal 범위 근처의 f32→f16 변환, `preserve` shader의 성능 비용(fp32 연산마다 helper 호출, 정규 operand는 fast path)도 미검증이다.

### P2-BINDLESS — Resource Binding Tier 3·SM 6.6 dynamic resources·RS 1.2

- **구현 요약:** 기존 descriptor heap 배치(shader-visible heap당 Metal buffer 하나, 32-byte descriptor)를 유지하므로 view 수는 heap 크기(1,000,000 view·2048 sampler)로만 제한된다. `CBV_SRV_UAV/SAMPLER_HEAP_DIRECTLY_INDEXED` root signature에는 heap base·count 4개 qword를 root argument 뒤에 붙이고, `SetDescriptorHeaps`와 `Set*RootSignature` 양쪽에서 채운다. `createHandleFromHeap`(218)의 view 형태는 `annotateHandle`(216)의 ResourceProperties에서 얻는다. Lane별로 descriptor를 읽으므로 `NonUniformResourceIndex`에 waterfall이 필요 없다. Heap 범위 밖 index나 heap 미바인딩은 내부 null descriptor로 처리해 read 0, write/atomic 무효이며 GPU가 heap 밖을 읽지 않는다. Null texture/sampler handle은 모든 texture op 전에 guard한다. RS 1.2는 `D3D12_STATIC_SAMPLER_DESC1`(flags)을 serialize/deserialize·버전 변환하고 non-normalized 좌표·UINT border 규칙을 검사한다. Heap-indexed ROV는 heap argument-buffer member에 ROG 속성을 붙여 구현했다.
- **보고 cap:** ResourceBindingTier **3**, ResourceHeapTier 2(변경 없음), ROOT_SIGNATURE HighestVersion **1.2**.
- **GPU 증거:** `bindless/bindless-03`(7 case), heap ROV를 추가한 `bindless/bindless-10`(8 case, `OWNED_D3D12_BINDLESS_ROV triangles=512 pixels=256 expected=0xc71c711c structured_mismatches=0 typed_mismatches=0 texture_mismatches=0`). Heap index 999,999 UAV, 80개 divergent raw UAV, divergent CBV/sampler, SM5.1 unbounded DXBC(space 0–3), heap 전환, 범위 밖 read 0, non-normalized sampler·`OPAQUE_{BLACK,WHITE}_UINT`를 포함한다. ROG 속성만 제거한 음성 대조는 validation 없이 256/256/256 mismatch였다. Metal GPU validation은 fragment를 직렬화해 race를 숨기므로 validation 실행만으로는 ordering을 증명하지 못한다. 회귀: `bindless/reg-{dxil-02,raster-order-03,dxil-texture-03}`, `bindless/reg-{core-contracts,bundles,pipeline-cache,execution-safety}-01`.
- **제약/미검증:** Heap ROV를 쓰는 PS는 모든 resource-heap 접근이 정렬되어 성능 비용이 있을 수 있다. MSAA·sample-frequency PS의 heap ROV와 Texture1D/2DArray/3D heap ROV는 GPU 미검증이다(Texture2D·typed·structured만 검증). Shader/root flag 불일치는 compile 실패로 `E_FAIL`을 반환한다(D3D12는 실패만 요구). Heap-indexed acceleration structure는 구현만 있고 GPU 미검증이다. 기록되지 않은 heap descriptor는 0으로 읽힌다(규격상 값 미정).

### P2-BARRIERS — Enhanced barriers·Device6–14·CommandList5–9·render pass·GPU upload heap

- **구현 요약:** `ID3D12GraphicsCommandList7::Barrier`는 subresource별 layout을 submission·queue를 넘어 추적한다. Legacy와 enhanced 모델은 COMMON으로만 전환한다. 규격 위반 barrier는 `Close`가 `E_INVALIDARG`, 실행 시 layout 불일치는 device removal이다. 순서를 요구하는 barrier는 현재 encoder를 끝내 Metal fence로 ordering·visibility를 만든다. Device10은 initial layout과 castable format 목록으로 resource를 만들고 목록의 format view를 허용한다. Front/back stencil ref·per-face mask, dynamic depth bias, dynamic strip cut을 구현했다(새 WineMetal render command, unix-call index 없음). Device13 `OpenExistingHeapFromAddress1`은 SHARING의 host-memory heap으로, Device14 `CreateRootSignatureFromSubobjectInLibrary`는 DXIL RDAT subobject에서 root signature를 만든다. `BeginRenderPass/EndRenderPass`는 render-pass 규격의 fallback(일반 bind·clear·resolve로 변환)으로 구현했다. Suspend/resume과 local-compute pass를 지원하고 pass 안의 금지 API·barrier는 기록 오류다. `D3D12_HEAP_TYPE_GPU_UPLOAD`는 heap·committed·placed resource에 허용한다. Shared storage·write-combined이며, shared/display/RT/DS flag는 거부한다. `CreateSampler2`의 UINT border는 Metal 고정 border 세 가지(0000/0001/1111)만 허용한다.
- **보고 cap:** EnhancedBarriersSupported **TRUE**, IndependentFrontAndBackStencilRefMaskSupported·DynamicIndexBufferStripCutSupported·DynamicDepthBiasSupported TRUE, RenderPassesTier **TIER_0**, RenderPassesValid **TRUE**, GPUUploadHeapSupported **TRUE**. RelaxedFormatCastingSupported **FALSE**, TriangleFanSupported FALSE, OPTIONS13·OPTIONS17 FALSE. `ID3D12GraphicsCommandList10`(work graphs)은 노출하지 않으며 P3다.
- **GPU 증거:** `barriers/enhanced-barriers-13`(19 case, `INVALID_BARRIERS_PASS rejected=10`, `STENCIL front=11 back=f2`, `DEPTH_BIAS … dynamic=0.251220703 reset=0.251220703 override=0.25`). UAV→UAV/SRV/COPY 4096 word, split barrier, castable RGBA8→R32_UINT의 정확한 bit, subresource 범위 layout, cross-queue COMMON handoff와 누락 시 removal, Device13 host memory GPU write→CPU read, Device14 library root signature dispatch를 포함한다. `barriers/render-passes-02`(8 case): CLEAR→PRESERVE `ff402080`, DISCARD+depth CLEAR `color=ffff0000 depth=0.125`, 4× resolve average `0x50`·min 0.125·max 0.5, suspend/resume `ff204010`, 10개 잘못된 사용 거부, GPU upload committed·placed buffer 1024 word와 `WriteToSubresource` texture, UINT border 3종. 회귀: `barriers/reg-{copy-resolve,core-contracts,msaa-lines,multisample-capabilities,resources}-03`, `barriers/reg-{execution-safety,multiqueue,so-format-support,stream-output,present-queues}-02`.
- **제약 증거:** RelaxedFormatCasting은 BC→비압축 view를 요구하지만, M5 Pro의 `newTextureViewWithPixelFormat:`은 BC1→RG32Uint, BC1→BC4, BC3→BC7을 'not compatible', Depth32Float↔R32Float를 'not castable'로 거부한다. Castable list는 Metal이 허용하는 쌍만 받고 나머지는 생성 시 거부한다. Render pass resolve는 기존 `ResolveSubresourceRegion` draw 경로를 사용하므로 destination에 `ALLOW_RENDER_TARGET`이 필요하다.
- **미검증:** Bundle 안의 enhanced barrier 거부(fixture 없음), queue 전용 layout(DIRECT_QUEUE_*/COMPUTE_QUEUE_*, GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE)의 개별 GPU 실행, castable list를 준 `GetResourceAllocationInfo3`. `CreateReservedResource2`의 GPU 경로는 TILED 범위다. VRS·mesh·sampler feedback·protected session·shader cache session·`AddToStateObject`는 오류를 반환하고 cap을 보고하지 않는다.

### P2-TILED — Reserved/tiled resource

- **구현 요약:** D3D12 64KB tile 하나를 Metal placement-sparse page(64KB) 하나에 대응한다. 모든 D3D12 heap은 placement heap이다. Reserved buffer는 실제 GPU VA를 가진다. Reserved texture는 2D/2D array, single-sample, `64KB_UNDEFINED_SWIZZLE`이며 format별로 Metal `sparseTileSize`가 D3D 표준 shape와 같을 때만 허용한다. Packed mip은 Metal sparse tail로 대응한다. `UpdateTileMappings/CopyTileMappings`는 queue FIFO에서 이전 submission 완료를 기다린 뒤(NO_HAZARD 제외) device 전역 MTL4 queue로 적용하고, shared event를 기다린다(10 s 초과 시 DEVICE_HUNG). 잘못된 호출은 mapping을 바꾸지 않고 버린다. Residency status는 texture에서 AIR residency bit, buffer에서 queue 순서로 기록하는 tile별 byte 배열을 사용한다. Root SRV/UAV는 root signature 뒤의 status qword, RGB32 emulated typed SRV는 descriptor q0로 status를 전달한다. SM5 DXBC의 모든 `*_FEEDBACK` opcode와 `check_access_fully_mapped`, DXIL `CheckAccessFullyMapped`를 lowering한다. Min/max reduction sampler를 추가했다. FXC 입력으로 발견한 `CHECK_ACCESS_FULLY_MAPPED` immediate 타입 오류도 수정했다.
- **보고 cap:** TiledResourcesTier **2**(`supportsPlacementSparse`일 때), `FORMAT_SUPPORT2_TILED`는 reserved 경로가 허용하는 format만 보고한다. **Tier 3·Tier 4와 SRVOnlyTiledResourceTier3는 보고하지 않는다.**
- **GPU 증거:** `tiled/final-tiled`(25 case, `OWNED_D3D12_TILED_CAPS tier=2 srv_only_tier3=0 va_bits_per_resource=48 rgba8_tiled=1`, `OWNED_D3D12_TILED_PASS cases=25 tier=2`). 두 heap mapping, NULL tile read 0·write 무효, REUSE_SINGLE_TILE, CopyTileMappings, ExecuteCommandLists 경계의 mapping 갱신, cross-queue 순서, Evict/MakeResident, `GetResourceTiling`(512²×10 RGBA8: standard 3·packed 7 mip), packed tail mapping, texture·buffer·root view·RGB32 residency status, FXC로 고정한 SM5 feedback 16 opcode(cs·ps), CopyTiles 왕복과 unmapped 0, min/max reduction을 포함한다. 회귀: `tiled/final-{bindless,raytracing}`, `tiled/reg-{bindless,bundles,capabilities,dxil-texture,predication-indirect,raytracing,resources}-02`, `tiled/reg-dxil-03`.
- **제약:** Metal 3D sparse tile은 64KB에서 128×128×1(RGBA8)이고 D3D12 표준 3D tile은 32×32×16이다. Volume tile을 독립적으로 mapping할 수 없으므로 3D reserved texture는 `E_INVALIDARG`이고 Tier 3를 보고하지 않는다. Packed mip이 있는 array는 거부하므로 Tier 4를 보고하지 않는다. MSAA·1D reserved texture도 `E_INVALIDARG`다. Local root signature(DXR shader record)의 root view와 ExecuteIndirect 인수로 설정한 root view는 status qword가 없어 항상 mapped로 보고한다.
- **비용:** MTL4 mapping 갱신은 enqueue 시점에 적용되고 MTL4 wait/barrier를 무시한다. 그래서 NO_HAZARD가 아닌 갱신마다 queue worker가 이전 submission 완료까지 직렬화된다. 같은 heap page를 공유하는 sparse tile은 command buffer 경계나 `MTLSharedEvent` signal+wait에서만 coherent하다. MTLFence·memoryBarrier·일반 MTLEvent로는 부족하다. 그래서 reserved resource가 하나라도 살아 있으면 모든 ordering barrier와 command-list 시작에 alias flush(shared event signal+wait)를 추가한다. Reserved resource가 없으면 비용이 없다.
- **미검증:** 10 s DEVICE_HUNG timeout 경로. RGBA8/R16F/R32F 외 reserved texture format(`sparseTileSize` 일치로만 판정). 32-bit에서는 CPU-visible heap의 `UpdateTileMappings`를 설계상 거부한다.

### P2-LIBRARY — `ID3D12PipelineLibrary/1`

- **구현 요약:** 이름→entry table(`shared_mutex`, 삽입 순서 유지)에 PSO 종류, SHA1 `PipelineCacheKey`, 기존 단일 PSO cache blob(DXMTPSO v3) 사본과 생성된 PSO를 보관한다. Load는 desc에서 key를 다시 계산해 이름·종류·key가 다르면 `E_INVALIDARG`이고, 일치하면 `CachedPSO`로 PSO를 한 번만 생성해 같은 객체를 반환한다. Container `DXMTPLB\0` v1(104-byte header: adapter registryID, compiler UUID·driver build, SHA1 digest)을 검증한다. Magic·크기·digest·경계 오류는 `E_INVALIDARG`, version·identity 불일치는 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`, adapter 불일치는 `D3D12_ERROR_ADAPTER_NOT_FOUND`다. Identity를 digest보다 먼저 검사한다. 생성 시 blob을 복사하므로 앱이 blob을 바로 해제해도 된다. `ID3D12Device2::CreatePipelineState`와 `LoadPipeline`이 같은 stream parser를 쓴다. 이전에 경고 후 무시하던 depth bounds·view instancing과 범위 밖 subobject type(UB)을 이제 `E_INVALIDARG`로 거부한다. Graphics key는 GS/HS/DS bytecode도 포함한다.
- **DXMTPLB는 native Metal binary archive가 아니다.** Load는 owned shader compile을 건너뛰지만 Metal library/function/PSO 생성은 수행한다.
- **보고 cap:** SHADER_CACHE `SINGLE_PSO | LIBRARY`(AUTOMATIC_* 없음), SERIALIZATION HeapSerializationTier **TIER_0**.
- **GPU 증거:** `library/pipeline-library-01`: 9개 CASE_PASS, 13개 GPU line(stored·deserialized·child×4 PSO 종류와 thread entry), `THREADS threads=8 rounds=24 entries=12`, `CHILD_PASS`. 자식 process log는 restore 4·cold 0으로 compile 생략을 확인한다. 회귀: `library/{pipeline-cache,bundles,core-contracts,dxil}-01`.
- **제약:** Depth bounds test(Metal에 없음), narrow quadrilateral line, view instancing, AS/MS bytecode(P3)는 `E_INVALIDARG`. Fractional depth bias와 per-face stencil mask의 거부는 BARRIERS가 해당 cap을 구현하며 제거했다. `OpenExistingHeapFromAddress/FileMapping` 기반 heap serialization은 tier 0이다.
- **미검증:** 실제 다른 GPU·driver의 blob(header 변조로만 시뮬레이션), library에 저장한 GS/HS/DS PSO. Graphics key가 GS/HS/DS bytecode를 포함하므로 GSTS 완료로 경로는 열렸지만, `pipeline-library` fixture에는 GS/HS/DS를 쓰는 case가 없다.

### P2-SHARING — Shared heap·resource·fence

- **구현 요약:** 공유 객체마다 Wine section `Local\dxmt.share.<pid>.<128-bit token>`(64 KiB header + payload)을 만들고 `CreateSharedHandle`은 그 section의 NT handle을 `DuplicateHandle`로 반환한다. `CloseHandle`·복제·상속은 Wine 기본 동작이다. Open 시 object type·이름 prefix·magic/version/kind/payload 경계를 검사하고 이름의 token과 header token을 대조해 위조 header를 거부한다. Heap·committed buffer는 section payload 위의 no-copy buffer다. IOSurface texture와 `MTLSharedEvent`는 process별 mach receive right(`bootstrap_register2`)로 전달하고, Metal identity(IOSurface ID·`globalTraceObjectID`)를 header와 대조한다. D3D name은 per-user salt의 SHA-256 link section으로 대응하며 중복은 `DXGI_ERROR_NAME_ALREADY_EXISTS`다. `CreateHeapFromHostMemory`/`FromFileMapping`(Device3, Device13 경유)도 구현했다.
- **Fence rewind 모델:** `MTLSharedEvent`는 되돌릴 수 없으므로(`supportsRollback=0`) event에는 단조 증가하는 native token만 싣는다. Fence section의 `SharedFenceTimeline`은 atomic `next_token`과 1024-entry `{token, logical}` ring이다. 모든 Signal은 token을 예약하고 logical 값을 기록한 뒤 event를 그 token으로 올린다. 관찰자는 token으로 ring을 읽어 기존 rewind-aware 경로에 logical 값을 공개하므로 `Signal(lower)`가 모든 process에서 일관되게 rewind된다. Ring이 가득 차면 CPU caller(CPU Signal, queue submission worker)만 backpressure로 대기하고 GPU는 기다리지 않는다. Queue Wait는 관찰한 native token을 GPU에서 기다린다.
- **보고 cap:** CrossAdapterRowMajorTextureSupported **TRUE**, CrossNodeSharingTier NOT_SUPPORTED.
- **GPU 증거(64-bit):** `integrate/run-02/sharing`(18 case, `OWNED_D3D12_SHARING_PASS cases=18`)와 WoW64 staging runtime의 64-bit fixture `sharing/sharing-64-w64-01`. Same-process A↔B buffer/texture/fence 4096 word readback·writeback, CPU Signal→GPU Wait, shared heap placed buffer, cross-adapter heap의 ROW_MAJOR texture, named open·중복·누락, 위조 token 등 오류, host memory·file mapping heap, `CreateProcess` 자식과 양방향 data·GPU fence 순서, 핸들 없이 이름으로만 여는 무관한 process(rewind 값 40), fence rewind, 3000 Signal stress를 포함한다. 실행 뒤 누수된 `dxmt.share.*` bootstrap 이름은 0이었다. 이전 회귀: `sharing/{execution-safety,fence-events,multiqueue,residency,resources,temporal}-02`.
- **제약:** IOSurface 공유 texture는 Metal에서 private storage여야 하므로 ROW_MAJOR가 아닌 texture는 shared heap에 둘 수 없다(`E_INVALIDARG`). Host memory heap은 주소·크기가 16 KiB 정렬이어야 하고, Rosetta/Wine의 4 KiB 정렬 할당은 거부한다. `bootstrap_register2(name, NULL)`은 등록을 해제하지 않으므로 receive right 파괴로 해제한다. 공유 fence에서 현재 값보다 낮은 native event 값을 쓰지 않는다(rewind는 logical ring으로 처리).
- **미구현/미검증:** **D3D11 interop(`OpenSharedResource1`)은 하지 않았다.** D3D11은 D3DKMT escape·bootstrap 이름 방식을 쓰므로 D3D11이 section 형식을 해석해야 하고, 이는 최소 공유 변경을 넘는다. 32↔64-bit 혼합 cross-process는 실행하지 않았다(fixed-width 형식이라 동작할 것으로 추정 `[INFERENCE]`). Ring 1024를 넘는 동시 publication은 backpressure로 막히며, 3000 Signal stress 이상의 경합은 미검증이다.
- **공유 `src/dxmt` 변경(D3D11 동작 동일):** `Fence`의 shared timeline 경로(shared event가 있을 때만 활성), `Texture::allocate(flags, placed_memory)`, `Texture::import` 실패 시 assert 대신 nullptr 반환(죽은 port가 debug build를 abort하던 버그 수정).

### P2-GSTS — GS/HS/DS

- **구현 요약:** D3D12 PSO의 VS+GS, VS+HS+DS, VS+HS+DS+GS를 DXBC와 DXIL 모두에서 Metal mesh pipeline으로 실행한다(이전에는 `E_NOTIMPL`). VS+GS는 VS object→GS mesh, VS+HS+DS는 VS+HS object→DS mesh이며 D3D11과 같은 airconv 다단계 entry를 쓴다. DXIL은 두 번째 ABI를 만들지 않는다. `SM50ShaderInternal::body_emitter` hook이 DXBC control flow 대신 DXIL 본문을 이식하고, 네 DXBC converter는 그대로 함수를 만든다(`src/airconv/dxil_pipeline.cpp`, HS/DS는 `dxil_pipeline_ts.cpp`).
  - **TS+GS(VS+HS+DS+GS):** 2-pass capture/compaction/GS mesh 경로다(설계: `MeshPipelineKind::TessellationGeometry`, `src/d3d12/d3d12_command_list.cpp` `RecordTessellationGeometryDraw`). Pass 1은 raster를 끈 VS+HS object와 capture mode DS mesh(`SM50_SHADER_D3D12_TESS_CAPTURE`)다. 각 workload가 DS vertex(R개 int4)와 index를 scratch slot `s = (instance*patches + patch)*W + rank`에 쓴다. Compaction compute kernel(`dxmt_tess_capture_clear`, `dxmt_tess_capture_compact`)은 slot 순서, 즉 D3D primitive 순서로 prefix scan해 flat primitive list와 object grid를 만든다. Pass 2는 capture를 읽는 object(`dsgs_main`, `SM50_SHADER_GS_TESSELLATION_INPUT`)와 DS를 첫 stage로 쓰는 GS mesh이며 `DrawMeshThreadgroupsIndirect`로 그린다. `SV_PrimitiveID`는 DS patch id다. Scratch는 CPU가 알려진 patch·instance 수와 PSO 생성 시 DS layout(W/V/P/R/K)으로 최악 크기를 계산하므로 capture write가 넘칠 수 없다. `maxBufferLength`나 UINT32 slot 범위를 넘는 draw는 `E_OUTOFMEMORY`다.
  - **Statistics:** 각 counter를 정확히 한 번 센다. IA·VS·HS는 pass 1 object, DS는 pass 1 mesh, GSInv·GSPrim·C는 pass 2 GS mesh에서 센다.
  - **PSG1 수정:** DXC는 patch-constant signature를 PSG1 chunk에 저장한다. HS/DS는 PCSG만 읽어 system value가 없는 metadata를 썼고, tess factor가 모두 0이 되어 모든 patch가 cull됐다(DSInvocations 0). `src/airconv/dxil_metadata.cpp:895-902`에서 PSG1을 먼저 읽고 PCSG는 fallback으로 둔다.
  - **Signature 번호:** DXIL signature chunk는 register row마다 entry가 있지만 `dx.op` element id는 element 단위다. 그래서 GS/HS/DS lowering은 entry metadata의 element 목록으로 id를 해석한다.
- **GPU 증거(API/Shader Validation):**
  - `dxilgsts/geometry-04`: `OWNED_D3D12_GEOMETRY_MATRIX_PASS shader=dxbc cases=41`, `shader=dxil cases=41`. Point→quad amplification, cut이 있는 strip, line/lineadj/triadj list·strip 입력, GS instancing·`SV_GSInstanceID`, `SV_PrimitiveID`, line/point SO topology, indexed·indirect draw, pipeline statistics, stream 0 SO capture, MSAA를 포함한다.
  - `dxilgsts/tessellation-04`: `OWNED_D3D12_TESSELLATION_MATRIX_PASS shader=dxbc cases=146`, `shader=dxil cases=146`, `OWNED_D3D12_TESSELLATION_PASS`. 각 경로의 146 case에 TS+GS 10 case가 들어 있다: tri/quad domain, integer/fractional_odd, SO stream 0, 1×/4× MSAA, 출력 불일치 `E_INVALIDARG`, indirect `E_NOTIMPL`. Oracle은 DS domain point를 tessellator oracle과, 이미지(1×와 resolve한 4×)를 GS 없는 같은 HS/DS PSO와 비교한다. Statistics와 SO 순서·내용(patch 순서, 1e-4 안에서 patch 면적 tiling)도 검사한다.
  - DXBC 회귀: `gsts/reg-{stream-output,pipeline-statistics,msaa-lines,multisample-capabilities,indexed-restart,bundles,predication-indirect,raster-order,dxil-texture}-40`, `gsts/reg-geometry-42`. DXIL 회귀: `dxilgsts/reg-{dxil,dxil-ia,dxil-texture,sm6}-04`.
- **공유 D3D11 변경:** `src/d3d11/d3d11_context_impl.cpp`의 `TessellationDraw`·`TessellationDrawIndexed`·`GeometryDraw`·`GeometryDrawIndexed`에 `MeshInstancesPerDraw`/`MeshDrawChunks`를 추가했다(`git diff`: 128줄 추가, 94줄 삭제). 이전에는 Apple7 미만 GPU(`maxObjectThreadgroups()` 1024, `src/dxmt/dxmt_device.cpp:45-48`)에서 object-threadgroup grid가 1024를 넘는 draw를 `WARN`만 남기고 그리지 않았다. 이제 instance 단위로 나눠 여러 mesh draw를 내보낸다. Chunk마다 draw argument block이 따로 있고 `StartInstance`가 chunk만큼 증가하므로 `SV_InstanceID`는 정확하다. Instance 하나가 한도를 넘을 때만 이전처럼 생략한다. 필요한 이유: GSTS 과제가 공유 mesh 경로의 이 silent drop 수정을 요구했다. D3D12는 자체 command list에서 mesh draw를 인코딩하고 Apple7+에서 한도가 없으므로(`d3d12_command_list.cpp`의 `max_threadgroups`) 공유 draw 경로의 사용자는 D3D11뿐이다. 그래서 D3D11 파일만 최소 범위로 바꿨다. **D3D11 baseline 통과:** `gsts/d3d11-40/baseline{,-packed}.stdout.log`에서 두 실행 모두 rc=0이다. `D3D11_BASELINE_DEVICE`, `CLEAR3D_UINT/FLOAT_PASS`, `DXBC_COMPUTE_LOAD_STORE_PASS`, `DXBC_SAMPLE_PASS`, `WINDOWED_PRESENT_PASS`를 출력했다.
- **제약:**
  - **P2 당시 제약(현재 후속 수정):** TS+GS PSO의 `ExecuteIndirect`는 `E_NOTIMPL`이었다. Capture scratch는 overflow가 불가능하도록 encode 전에 patch 수 상한으로 크기를 정해야 한다. Indirect vertex·instance 수는 GPU 값이고 D3D12는 임의 uint32를 허용하므로 CPU가 아는 상한이 없다. 유한한 사전 할당으로는 overflow를 막을 수 없고, 미리 기록한 Metal command stream은 mesh pass buffer를 GPU에서 할당할 수 없다.
  - **P2 당시 제약(현재 후속 수정):** MSAA mesh draw 위의 statistics는 `E_NOTIMPL`이었다(native MSAA가 PSInvocations를 과다 호출).
  - **P2 당시 제약(현재 후속 수정):** GS SO는 stream 0만 지원한다. Stream 1–3은 `E_NOTIMPL`이다(TS+GS와 DXIL GS도 같은 규칙). GS 없는 VS+HS+DS PSO의 SO는 `E_NOTIMPL`이다(`src/d3d12/d3d12_pipeline_graphics.cpp:1179-1181`).
  - TS+GS에서 GS 입력 primitive는 tessellator 출력(point/line/triangle)과 같아야 하며, 다르면 `E_INVALIDARG`다.
- **P2 당시 미검증:** isoline/point 출력을 line/point GS로 받는 TS+GS(ABI는 K=2/1을 지원하지만 fixture는 triangle만 다룬다). Indexed·instanced·predicated TS+GS(코드 경로는 있지만 fixture case가 없다). D3D11 >1024 분할의 Apple7 미만 하드웨어 실행(M5에서는 한도가 없어 시험할 수 없다). DXIL GS의 `SV_ClipDistance/CullDistance`·`SV_RenderTargetArrayIndex/ViewportArrayIndex`(compile만 확인). DXIL GS stream 1–3 거부 실행. 한 PSO에서 DXBC stage와 DXIL stage를 섞는 GS/HS/DS 조합. 이후 GS/TS+GS indirect·stream 1–3·DS SO의 검증 범위는 P3 절에 별도로 기록한다.

### P2 통합·WoW64 결과

- **최종 64-bit 통합:**
  - `integrate/run-03/summary.tsv`(52 suite, `dxil-static-sampler`는 fixture 없음으로 제외): 51개 통과. `tessellation`만 실패했는데, `runtime-03`이 마지막 DXIL 수정(PSG1, TS+GS DXIL routing) 전에 staging됐기 때문이다.
  - `integrate/run-04/summary.tsv`: `tessellation`, `geometry`, `dxil`, `sm6`을 `runtime-04`에서 다시 실행해 모두 통과했다(validation 오류 0, api-and-shader).
  - SM 6.5 보고를 포함한 `runtime-final`의 최종 전체 실행(`integrate/run-final/summary.tsv`): **52/52 suite 통과**, 전부 validation 오류 0(api-and-shader). `sm6`·`capabilities`는 `shader_model=0x65`·`highest=0x65`를 출력한다. CPU 검증 `test-owned-graphics-{build,staging,diagnostics}.py`와 `test-owned-ngx-frontend.py`도 통과했다.
- **이전 64-bit 통합:** `integrate/run-01/summary.tsv`는 51개 suite 중 48개 통과, 3개 실패였다(`fullscreen`, `dxil-static-sampler`는 fixture 없음으로 제외). 실패 3개 중 2개는 새로 보고한 cap에 대한 오래된 fixture 기대값이었다. `pipeline-library`는 `independent_stencil_masks`·`fractional_depth_bias`에 `E_INVALIDARG`를 기대했으므로 이제 `S_OK`를 기대한다. `enhanced-barriers`는 `capabilities` 단계에서 실패했다(run-01 fixture binary가 `GPUUploadHeapSupported=1` 기대값으로 source를 갱신하기 전에 빌드됐다. binary·source 시각 비교에 따른 추정 `[INFERENCE]`). 나머지 하나는 실제 결함이었다. `ValidateHeapProperties`가 GPU_UPLOAD case에서 DEFAULT로 fall-through해 shared DEFAULT heap을 거부했다(`CreateCommittedResource shared buffer hr=0x80070057`). `src/d3d12/d3d12_resource_helper.cpp`에서 수정했다. 세 suite는 `integrate/run-02/summary.tsv`에서 통과했다.
- **WoW64(32-bit), 최종 tree:** `wow64/summary-02.tsv`에서 PE32 suite 17/17이 통과했다. 대상은 geometry, tessellation, stream-output, indexed-restart, so-format-support, resources, smoke, sharing, tiled, enhanced-barriers, render-passes, pipeline-library, bindless, sm6, fence-events, multiqueue, dxil이다. 증거는 `wow64/final-<suite>`이며, `final-geometry`와 `final-tessellation`은 DXBC·DXIL 각각 41·146 case의 `MATRIX_PASS`를 출력한다. 이 실행은 6.5 상향 전이라 `final-sm6`는 `shader_model=0x51`을 출력한다. 상향 뒤 32-bit `sm6`을 다시 실행한 `wow64/final-sm6-65`는 `shader_model=0x65`로 통과했다(validation 오류 0). 32-bit argument 변환은 `src/winemetal/unix/winemetal_unix.c`의 `SM50_SHADER_D3D12_TESS_CAPTURE`(layout_out pointer)·`SM50_SHADER_GS_TESSELLATION_INPUT` 경로다.
- **WoW64 초기 실행과 수정:** `wow64/summary.tsv`의 PE32 fixture 14개 중 12개가 처음에 통과했고, 결함 두 개를 수정했다.
  - `pipeline-library`: fixture가 i686에서 stream을 8 byte로 padding했다(fixture 결함). 재실행 `wow64/run-pipeline-library-02` 통과.
  - `sharing`: `d3d12_buffer.cpp`의 dangling-else로 i386 build에서 fallback allocation chain이 무조건 실행되어, 모든 shared committed buffer가 새 private allocation으로 rename됐다. 할당 chain을 마지막 `else` 안으로 묶었다. `sharing/sharing-32-01`(18 case, 32↔32 cross-process 양방향, 이름으로 여는 무관한 32-bit process, 3000 Signal stress), `sharing/resources-32-01`, `sharing/fence-events-32-01` 통과.


## P3 완료 결과 (2026-10-01)

**P3의 지정 구현과 최종 통합 검증을 Apple M5 Pro/macOS 27의 x64·WoW64 Wine 범위에서 완료했다.** 아래 실행은 별도 표시한 API-only를 제외하고 Metal API/Shader Validation을 사용했다. 최종 공통 runtime-04의 affected suite 통과와 baseline+targeted reachable coverage를 구분하며, 한 snapshot의 전체 suite 통과·전체 D3D12/DXR conformance를 주장하지 않는다.

| 기능 | 현재 검증 범위·보고 값 | 보존된 증거 경로 |
|---|---|---|
| 최종 공통 runtime-04 | Manifest **363308d9269fa9a29140b2f7fdff0505185e0ec143d3f3f13b7eb9582bca0230**의 x64 RT/WG/HDR **3 suite**, WoW64 RT/WG/HDR/query **4 suite** 모두 exit0·API/Shader Validation 오류 없음·foreign backend 없음. RT 25 frozen 입력/contract13/AS16/RayQuery15/formats13, WG OPTIONS21 16-byte·tier10·9 case, HDR7-frame source readback/fence/Present 검증. 전용 prefix stop/wait/cleanup 완료 | `build/local/p3/integrate/runtime-04`, `final64-04/{raytracing,work-graphs,hdr-metadata}/result.json`, `final32-04/{raytracing,work-graphs,hdr-metadata,queries}/result.json`(같은 integrate 디렉터리) |
| x64 최종 reachable coverage | 계획 61 row 중 source가 있는 **60 reachable row 모두 PASS: 59 API/Shader+1 API-only**. Baseline과 enhanced-barrier/atomic 및 최종 common-cut proof의 합산이며 단일 snapshot 전체 suite 통과가 아니다. 기존 source-orphan standalone static-sampler 1 row는 미검증 | `build/local/p3/integrate/aggregate64-final.json` |
| WoW64 최종 reachable coverage | **58 reachable label 모두 PASS: 57 API/Shader+1 API-only**, cleanup 완료. Baseline+affected proof+최종 common-cut 합산이며 단일 snapshot 전체 suite 통과가 아니다. RT/WG 보류 없음. NGX 2 suite는 x64-only ABI, standalone static-sampler는 retained source 부재로 실행 불가 | `build/local/p3/integrate/integration32-final-evidence.json` |
| Feature level | **FL12_1**, 공개 max=0xc100, FL11_1/12_0/12_1 생성 성공·FL12_2 거절, strict capabilities runner PASS | `build/local/p3/integrate/runtime-03`, `build/local/p3/integrate/fl121-cap-01/result.json`·`stdout.log` |
| Conservative raster | **ConservativeRasterizationTier 1**. VS DXBC/DXIL, GS, HS/DS, TS+GS, MS, AS/MS 전체 **10 family**, clip/cull/view·indirect/SO/statistics와 4× depth extrapolation/clamp **1180 sample** 검증. 공개 tier=1·strict runner PASS | `build/local/p3/integrate/conservative-cap-01/result.json`·`stdout.log` |
| 선행 FL12_0 증거(역사적) | **FL12_0**. Logic op, 18 typed UAV load format, UAV-only ForcedSampleCount 1/4/8/16, TIR 16×, 4×/8× RTV의 forced 1×를 GPU 검증. Cap 조회 max=0xc000, FL11_1/12_0 생성 성공·당시 FL12_1 거절 | `build/local/p3/fl2/run-08`, `build/local/p3/fl2/cap-01` |
| DXR | **RaytracingTier 1.1**. 새 통합 runtime에서 `raytracing_tier=11`, 정확한 RayQuery 15 case, AS 16 case, contract/state-object 12 case, vertex format 13종 및 indirect·predication·root arguments·coverage 통과. Validation 오류·foreign backend load 없음. Generic-function collection export/linkage의 후속 x64/WoW64 paired proof는 아래에 별도로 기록하며 전체 DXR conformance 완료가 아님 | `build/local/p3/integrate/runtime-01`, `build/local/p3/integrate/dxr-cap-01/result.json`·`stdout.log`; 개별 증거 `build/local/p3/rqstages/run-08`, `build/local/p3/dxras-resume/run-09` |
| DXR ordinary-function collection(x64/WoW64) | Immutable WG/DXR namespace에서 renamed export·deferred external을 final pipeline 생성 시 연결, hidden internal closure·dependency flags·conflict·immutable root·helper resource metadata 검증. Ordinary function에는 shader identifier를 주지 않음. Upstream DXC `lib_6_6` consumer/helper/final frozen fixture에서 uint/float overload의 unmangled export group rename·mangled signature를 보존하고 float **5.25/7.25/9.25/11.25** bit pattern, reachable helper local-root incompatibility와 global-root conflict를 검증. GPU words **0x2320/0x2323/0x2326/0x2329**와 helper UAV writes, mixed RT-shader+ordinary collection의 renamed raygen만 import해 hidden closure를 유지하는 경로와 flags/hidden/missing/duplicate-global-root/incompatible-helper-local-root negative 계약, 기존 RayQuery15/AS16/formats13 유지. **같은 24 frozen DXIL 입력·typed-overload/root ABI의 x64/WoW64 paired PASS** | `build/local/p3/dxr-linkage/runtime-07`, `build/local/p3/dxr-linkage/after-09/result.json`·`stdout.log`, `build/local/p3/integrate/runtime-rt32-02`·`raytracing32-isolated-03/result.json`; 재생성 `graphics-translator/tests/dxil/build-raytracing-linkage.sh` |
| 후속 DXR named-root/resource binding(x64/WoW64) | Source/ExistingCollection의 embedded named-root alias·filter/old-name-hidden·function/root namespace collision·NumExports0 publication, mixed collection alias와 provider association 검증. Resource가 있는 closed rootless collection 거부; AddToStateObject의 같은 embedded root 재선언 및 기존 GPU oracle 유지. **Contract13/AS16/RayQuery15/formats13**, API/Shader Validation PASS. **같은 native snapshot의 최신 x64/WoW64 paired PASS** | `build/local/p3/dxr-linkage/runtime-13`, `build/local/p3/dxr-linkage/after-16/result.json`, `build/local/p3/integrate/raytracing32-isolated-04/result.json` |
| GS/TS+GS indirect | Command별 root constant·CBV/SRV/UAV·VB/IB 변경, GPU argument/count·predication과 직접 draw의 독립 oracle 비교. Geometry **DXBC 87/DXIL 87**, tessellation **166/166** 통과 | `build/local/p3/gsindirect-contract/geometry-10`, `build/local/p3/gsindirect-contract/tessellation-07` |
| GS SO stream 0–3·TS+GS SO/indirect | Stream별 정확한 bytes·filled size·written/needed·overflow·rasterized stream/NONE, GS SO indirect와 SO 통계의 predication 검증 | `build/local/p3/p2sostreams/geometry-04`·`so-03`, `build/local/p3/tsgsind/geometry-05`·`tessellation-05` |
| Early-Z MSAA statistics | GS·HS/DS·TS+GS·MS의 2×/4×/8× forced-early PSInvocations를 CPU oracle과 비교. Direct/indirect, GS indirect+SO 및 cut/adjacency strip indirect 검증. Mesh의 이 부분 통과는 MeshShaderTier 1 전체 증거가 아님 | `build/local/p3/p2last/geometry-final.log`·`tessellation-final.log`·`mesh-01.log` |
| DS-only SO | **Tessellation DXBC 166/DXIL 166**. Fractional tri/quad·isoline point/line, direct/indirect의 정확한 SO 순서·filled size·overflow·root/VB/IB 변경·no-raster 통과. No-raster에서는 conservative/VRS 설정을 켜도 C/PS=0·clear image 유지 | `build/local/p3/ds-so/proof-10/result.json`·`stdout.log` |
| Sampler feedback·view instancing | **기존 backend:** SamplerFeedbackTier 1.0·83 case, ViewInstancingTier 1·23 case의 VS/GS/HS/DS/PS ViewID·위치·mask·4× MSAA·indirect·bundle·statistics 통과. **현재 Metal 4 view ICB 보정: 공개 GPU 검증 완료(해당 범위).** Encoder view mapping을 무시하는 native ICB에는 전용 DXBC/DXIL AIR epilogue(enum 43)로 기존 index+offset의 64-bit 합산·bounds clamp를 적용하며 logical ViewID·mask·direct/cache를 보존한다. Recovery runtime-02의 두 ABI에서 각각 **26 case**(기존 indirect 2 views/2 commands, masked mapping·direct restore·DRAW-only·authored index/clamp, bundle·statistics·invalid desc 포함)와 SO/indexed-restart/MSAA-lines, 총 **8 suite 실행 PASS**·API/Shader Validation 오류 0·외부 backend 로드 0. 이 개별 proof는 전체 conformance·ARM64 Wine/FEX 완료를 뜻하지 않음 | 과거(유실): `build/local/p3/feedback3/run-08`, `build/local/p3/viewinst2/final-view-instancing`. Source proof: `build/local/metal4/recovery/view-air/acceptance.json`, `build/local/metal4/recovery/integration/view-source-ready-02.json`. 공개 proof: `build/local/metal4/recovery/integration/runtime02-stream-viewx{64,86}-01/{view-instancing,stream-output,indexed-restart,msaa-lines}/{result.json,stdout.log}` |
| Mesh 출력·cull | **공개 MeshShaderTier 1**. 필수 128-scalar(SV_Position 포함) vertex/primitive split, 124 독립 semantic, 32KiB output+28KiB app threadgroup, 1×/4× direct/GPU-count indirect/cache, wide coarse SV_Coverage·pull·ddx/ddy, 8-plane CullDistance의 triangle/line·zero/NaN·primitive cull 등 **34 case** 통과. 새 통합 runtime에서 공개 `tier=10`, exit 0·strict runner PASS·API/Shader validation 오류 없음 | `build/local/p3/integrate/runtime-02`, `build/local/p3/integrate/mesh-cap-01/result.json`·`stdout.log`; 앞선 cap=0 proof는 `build/local/p3/mesh-contract/run-13` |
| VRS | **VariableShadingRateTier 2**, AdditionalShadingRatesSupported TRUE, ShadingRateImageTileSize **8**, PerPrimitiveShadingRateSupportedWithViewportIndexing TRUE. 기존 142 case+VS/GS/MS/AS viewport 4 case, 총 **146 case**를 probe override 없이 공개 cap으로 통과. COMMON rate-image의 Close/queued capture는 S_OK지만 FIFO submission은 implicit COMMON→SRS promotion을 거부해 `DXGI_ERROR_DEVICE_REMOVED`(887a0005) | `build/local/p3/vrs2-finish/out-20-tier2/result.json`·`stdout.log` |
| 선행 Work Graph cap(역사적) | 당시 native/WoW64 node **5 case**는 통과했지만 OPTIONS21은 `E_NOTIMPL`이었다. 현재 공개 Tier1.0·9-case 및 최종 common-cut proof와 구분 | `build/local/p3/workgraph-resume/run-14-final-native/result.json`, `run-15-final-wow64/result.json`, `cap-query-stdout.log`(같은 workgraph-resume 디렉터리) |
| Work Graph Tier 1.0 | **실제 OPTIONS21 16-byte S_OK·tier=10**, 15/20-byte E_INVALIDARG와 adjacent guard 보존. **9 ordered case** x64/WoW64 strict PASS: 기존 exact-min25728/depth32/fanout16 및 shared budgets, per-program roots·ordinary-function/aggregate helper parameter+return·public alias/hidden dependency·same-root AddToStateObject·collision negatives, global/local static-sampler union과 local S0-only/ARG0 검증. Linked A **sum196/count4**, B **248/4**, helper **54/4**, local sampler **68/4**. 일반 Shader Model **6.5** 유지 | `build/local/p3/workgraph-resume/runtime-20`, `run-30-final-cap64/result.json`·`stdout.log`, `run-31-final-cap32/result.json`·`stdout.log`(같은 workgraph-resume 디렉터리) |
| WoW64 production presenter GPU probe | `runtime-03-wow64`에서 HDR **5 drawable frame+detached**, color-space **11 frame+GPU retirement gate**, geometry **12 pixel oracle**(crop·3 rotation·background·RTL·restriction) 통과. 모두 rc=0·API/Shader validation 오류 없음. Public suite gate/physical compositor proof가 아닌 production presenter readback이며 color-space `compositor_proof=0`. 초기 `presenters32-01`의 rc=3은 fixture readback allocation에 기존 `WMT_RESOURCE_CPU_ADDRESSABLE` 옵션을 적용해 수정; pointer truncation·새 allocator/shim 없음 | `build/local/p3/integrate/presenters32-02/{hdr-presenter,colorspace-presenter,present-geometry}/{result.json,stdout.log,invocation.json}` |
| x64 production presenter GPU probe | Color-space **11 drawable frame+retirement gate**와 geometry crop·3 rotation·background·RTL·restriction 통과. HDR 최종 proof는 fractional display peak **603.665771**과 content equality, **5 GPU frame+detached** 통과. 초기 HDR truncation/scale-dependent fixture 실패를 최종 proof와 구분; production drawable readback을 physical compositor proof로 확대하지 않음 | `build/local/p3/integrate/native64-01/results.json`(colorspace·geometry), `build/local/p3/integrate/native64-hdr-03/results.json`·`owned-d3d12-hdr-presenter/run.log` |
| x64 scoped contract 회귀 | Enhanced barriers **19 case·rejected 10** API/Shader Validation PASS. Atomic64 live reader는 **API-only PASS**(observed 256·changed 64·first 1/last 128), **atomic64_snapshot=unproven**. 기존 API+shader live 실행의 INCONCLUSIVE를 통과로 바꾸거나 이 관찰을 atomic 64-bit snapshot 증거로 승격하지 않음 | `build/local/p3/integrate/enhanced64-contract-02/enhanced-barriers/result.json`, `build/local/p3/integrate/atomic64-live-api-01/result.json`·`stdout.log` |
| WoW64 scoped query·atomic 회귀 | Clock calibration monotonic·실제 GPU timestamp 증가와 occlusion **16384,4096,0,12288,1,0** API/Shader Validation PASS. Query는 별도 `runtime-query32-01`에서 실행(초기 query abort는 역사적 실패). Atomic64 live는 **API-only PASS** observed256·changed64·first1/last128, **snapshot unproven**; 초기 API+shader INCONCLUSIVE를 통과로 승격하지 않음. 이 별도 native 재빌드 proof는 최종 combined runtime 전체 회귀가 아님 | `build/local/p3/integrate/queries32-02/result.json`·`stdout.log`, `build/local/p3/integrate/atomic-copy-live32-api-01/result.json`·`stdout.log` |
| 초기 WoW64 실패 증거(역사적) | `all32-01`은 56 run 중 **53 API/Shader PASS**였고 query abort·obsolete barrier-interface assertion·atomic-live Shader Validation INCONCLUSIVE를 보존했다. 해당 경로의 후속 proof와 최종 reachable 58-label 결과는 위 행을 참조하며 원래 실패를 통과로 덮어쓰지 않는다 | `build/local/p3/integrate/integration32-initial-evidence.json`, `all32-01/{summary.json,coverage.json}`(같은 integrate 디렉터리) |
| PSO cache epoch | Serialized PSO **format revision 4 유지**. Graphics key는 P3 당시 **5**, 현재 logic-op 의미 변경으로 **6**이며 format과 별개다. 당시 PSO/library GPU round-trip·child restore 4/cold compile 0·digest가 유효한 nested revision 3의 `D3D12_ERROR_DRIVER_VERSION_MISMATCH`를 확인했다. View ICB standard/index-pulling/statistics 함수·상수는 PSO-local lazy-derived variant이며 persistent cache를 읽거나 쓰지 않아 원래 direct VS/PS blob을 보존한다. Key가 이미 view count·masking·모든 VP/RT offset을 포함하므로 추가 persistent revision은 없고 restored PSO도 ICB variant를 compile할 수 있다. Native binary archive는 아님 | 과거(유실): `build/local/p3/cache-epoch/runtime-after`, `cache-after`·`library-after`·`nested-version-proof`(같은 cache-epoch 디렉터리). 현재 version/key 계약: `src/d3d12/d3d12_pipeline_cache.cpp` |

RayQuery는 CS·VS·PS·HS·DS·GS·AS·MS·raygen·closest-hit·any-hit·intersection·miss·callable과 raygen indirect에서 triangle/procedural scene의 24 lane × 64 word를 비교했다. Native intersection callback 안의 inline query가 허용되지 않는 any-hit/intersection 중첩-query pipeline만 **kernel-side candidate traversal + visible callback**을 쓰며, 다른 pipeline은 native fast path를 유지한다. Base·state-object shader의 IgnoreHit·AcceptHitAndEndSearch·ReportHit 거부/interval·opacity·local root·AddToStateObject도 새 경로를 검증한다. 잘못된 candidate InstanceID mutation은 `build/local/p3/rqstages/mutation-run`에서 추가 10 stage 모두 실패했다. AS 증거는 postbuild·clone/compact·serialize/deserialize·TLAS relocation·visualization·in-place/out-of-place update와 빈 AS/비영 offset subrange를 포함한다.

### P3 완료 범위와 cap·검증 경계

- **MeshShaderTier 1 검증:** `integrate/mesh-cap-01`은 새 `integrate/runtime-02`에서 공개 tier=10과 전체 34 case를 strict runner로 통과했다. 앞선 `mesh-contract/run-13`은 cap=0 때문에 runner가 실패했던 역사적 proof이며 현재 cap 결과와 구분한다. 필수 32 packed row/128 scalar에는 float4 SV_Position이 포함된다. Native 124-scalar 한계를 하드웨어 예외로 인정하지 않고, 초과 출력·cull/활성 query는 device-record capture/replay로 처리한다. 일반 query 없는 native 범위는 fast path를 유지한다. Tier 보고는 이 장치에서 확인한 전체 필수 출력·cull 계약과 cap proof에 근거한다.
- **기능별 proof와 최종 통합:** WG root/ordinary collection/local-only sampler와 DXR typed-overload/mixed collection/named-root/resource-binding은 기능별 paired GPU proof에 더해 공통 runtime-04의 x64/WoW64 affected suite에서도 통과했다. Conservative Tier1·VRS Tier2·mesh Tier1의 기능별 strict proof와 합쳐 P3 완료 판정에 사용하되 전체 D3D12/DXR conformance로 확대하지 않는다.
- **FL12_1과 FL12_2:** 현재 공개 최대는 **FL12_1**이다. FL12_1의 conservative raster Tier 1·ROV 요구를 FL12_2와 혼동하지 않는다. 공식 필수 요건 대조(P3CapAudit)에 따르면 FL12_2는 DXR 1.1·mesh 1·VRS 2·sampler feedback 0.9 이상 외에도 **ConservativeRasterizationTier 3, TiledResourcesTier 3, DepthBoundsTestSupported, CopyQueueTimestampQueriesSupported, OPTIONS8의 UnalignedBlockTexturesSupported(unaligned BC)** 등이 필요하다. 따라서 일부 P3 tier 통과만으로 FL12_2를 올릴 수 없으며 **FL12_2는 미지원**이다. SM 6.6 보고를 이 feature level/tier들의 일괄 선행 조건으로 두지 않는다; HighestShaderModel은 검증된 **6.5**를 유지한다.
- **통합 증거의 범위:** x64 **60=59 API/Shader+1 API-only**, WoW64 **58=57+1**은 baseline+source-warranted targeted proof의 reachable 합산이다. 모든 suite를 최신 단일 snapshot에서 재실행한 것은 아니다. Atomic live의 API-only PASS는 Shader Validation PASS 또는 atomic64 snapshot 보증이 아니며 snapshot은 **unproven**으로 남긴다. 기존 retained source가 없는 standalone static-sampler와 WoW64의 x64-only NGX 2 suite는 검증 불가 경계이지 미완료 P3 기능이 아니다. Production presenter readback은 물리 compositor proof가 아니며 다른 GPU/OS·Windows reference differential·실제 게임 정상 렌더링·성능은 보증하지 않는다.



### P4 진행 상태

**ARM64/FEX foundation track (2026-10-02):** see the English [architecture and acceptance plan](../docs/ARM64-FEX-IMPLEMENTATION.md). Native Mach-O ARM64, plain ARM64 PE, ARM64EC/ARM64X x64 interoperation and x86 WoW64 are separate build/execution contracts. The mixed-cut 118/118 below remains closed only for its documented Metal 4 fixture scope; the ARM64 Wine/FEX end-to-end gates remain separate. Latest review corrections and evidence are recorded in [the review dispositions](../docs/D3D12-TRANSLATION-REVIEW-2026-10-01.md#implementation-dispositions--2026-10-02).

Foundation evidence: native compiler/Metal GPU **11/11**, six real plain ARM64/ARM64EC translator DLLs, schema-2 staging and legacy x64/WoW64/ABI regressions passed. The isolated pinned-source follow-up fixes two x64-only D3DMetal guard inconsistencies without editing the other Wine repository: real ARM64 `winemac.so`, `ntdll.so`, `win32u.so`, and full owned `winemetal.so` now build and load. The actual Unix-plugin compiler/GPU smoke passed **11/11**, `proc_translated=0`, with **495 loaded image headers** verified ARM64. Both real ARM64 ordinary-layer and x64 existing-layer regressions passed; the x64 comparison uses existing Rosetta. A genuine paired ARM64X WineMetal DLL linked manually, but the automated production matrix still requires a paired object/CRT/spec build recipe. Native Wine/FEX host-runtime and guest end-to-end gates are **not closed**.

**최종 fixture acceptance(2026-10-02):** `build/local/metal4/recovery/integration/full-suite-runtime02-combined-acceptance.json`(SHA256 `7228f827e74413e38c59b2eb8fba7df5197fa682b346deb254753e3068124a92`)의 reachable **118/118 PASS**, failed/pending **0**이다. x64 **60 = runtime-02 22 + runtime-01 38**, WoW64 **58 = 22 + 36**; **44**개 실제 새 cut + **74**개 유지한 recovery proof다. **116 API/Shader Validation + 2 atomic-live API-only**, **114 Metal 4 queue 관찰 + 4 no-queue API/capability 예외**이며 118개 prefix 모두 wait 0·제거를 확인했다. 최종 행은 saved-result reconciliation 없이 strict raw PASS다.

- **Provenance:** 새 44개는 prepared-05/runtime-02 manifest `63ef4f29…`·runner `8bef102c…`다. 유지한 74개는 prepared-03/runtime-01·preflight runner `9e388304…`를 기준으로 하며, RT-only `9e388… → 316cc6…` 변경 경계는 기록했지만 process별 실제 loaded runner hash를 증명한 것은 아니다. 기존 **111/118** aggregate와 **7개 false** 결과는 byte-identical하게 보존했고 새 결과로 덮어쓰지 않았다.
- **집계 밖:** standalone `dxil-static-sampler`의 양 ABI source 부재, x86 `ngx`·`ngx-contracts`의 x64-only frontend/staging 제약으로 **4 planned row는 미실행·118에 미포함**이다. 별도 presenter **6**, UMA **2**, classic D3D11 **1** probe는 추가 증거이며 temporal **2**는 이미 118에 포함되어 중복 집계하지 않는다.
- **완료 경계:** Metal 4 구현 전환·위 fixture acceptance를 닫았다. 118개 모두 runtime-02 재실행, 전체 D3D12/DXR conformance, 실제 게임·물리 compositor/HDR signal, 다른 GPU/OS 또는 ARM64 Wine/FEX acceptance를 주장하지 않는다. 측정하지 않은 성능 향상도 주장하지 않는다.

**Mesh indirect ordering·runtime-02 fresh proof:** `RecordMeshShaderIndirect`는 command slot마다 GPU-only encoder 경계를 만들고 attachment store/load와 mesh/fragment fence를 보존한다. 같은 pass의 indirect overwrite는 정확한 grid/count 3에서도 slot0 값 4가 남았고 Mesh→Mesh barrier만으로 해결되지 않았으며, fragment를 포함한 intra-pass barrier는 API validation이 거부했다. Native ARM64의 ordered-pass count 1/3은 각각 8 iteration·128 pixel을 통과했다(`build/local/metal4/mesh-indirect/*log`). 공개 `build/local/metal4/recovery/integration/runtime02-meshx{64,86}-01/mesh-shader/{result.json,stdout.log}`는 동일 manifest `63ef4f29…`의 두 ABI에서 각각 **34 CASE_PASS**·exit 0·API/Shader Validation 오류 0·외부 backend 로드 0이다. Indirect count 3의 occlusion은 predicate 0 → 0, predicate 1 → 12288이고 기존 pixel oracle·active query·early-depth statistics·predication·root update·MSAA도 통과했다. CPU wait·classic fallback이나 기존 oracle 완화 없이 수정했으며, 이 개별 proof만으로 전체 conformance·ARM64 Wine/FEX 완료를 주장하지 않는다.

**Sampler heap retirement·native command buffer 수명:** 완료된 explicit heap을 retire한 뒤 native command buffer를 재사용하면 stale `HeapUsageTable`이 남는 문제를 수정했다. 첫 recording 뒤에는 매번 새 native command buffer를 만들며 WMT context·command allocator·global queue residency set은 유지한다. 정확한 command buffer·allocator·recording lease는 **GPU feedback과 native commit 반환이 모두 도달한 뒤** retire한다. Per-record residency-set snapshot을 현재 해결책으로 사용하지 않는다. `build/local/metal4/recovery/native-core/sampler-heap-retirement/{native-run.log,evidence.json}`의 실제 ARM64/x86_64 API/GPU Validation은 같은 context·allocator, 서로 다른 native command buffer로 15→3 encoder recording을 실행하고 각각 readback 123을 두 번 확인했다. Callback-before-commit-return 순서를 강제로 유발한 시험은 아니므로 그 race의 재현·신뢰성 시험을 통과했다고 주장하지 않는다. 양 ABI 새 runtime-02의 sampler-feedback을 포함한 44 affected 실행은 최종 acceptance에 PASS로 반영했다. ARM64 Wine/FEX acceptance는 아니다.

**Recovery runtime-01의 별도 presenter proof:** `build/local/metal4/recovery/presentation/pe-acceptance.json`은 두 ABI의 geometry·color-space·HDR·temporal, 총 8개 실행의 API/Shader Validation PASS를 기록한다. Temporal은 각 16 frame·59 executions·56 frame retirement를 확인했다. 이는 실제 drawable GPU readback·D3D12 temporal 계약이며 물리 compositor/HDR scanout·게임·ARM64 Wine/FEX acceptance는 아니다. Core·advanced 및 새 runtime-02 affected 결과의 최종 집계는 위 118/118 acceptance를 기준으로 한다.

**ARM64 native 범위:** `build/local/metal4/recovery/arm64/compile.log`은 core/pipeline/query/fx 4 domain과 ABI object가 실제 ARM64 Mach-O로 빌드됨을 확인한다(고정 ABI 200 bytes·args 8·return 136·handle 8). 실제 ARM64 native GPU proof인 `recovery/native-core/visibility-fence.log`는 preceding GPU read 256 보존 → scratch reset 0의 두 bridge fence 경로를 CPU wait·whole-queue barrier 없이 통과했다. `recovery/native-core/as-parity.log`는 unresolved/resolved AS prebuild size 일치·unresolved build 거부·row-major transform과 독립 baked geometry의 traversal 일치 및 column-major control 실패를 확인한다. 이는 native domain compile·GPU 증거이며 전체 ARM64 translator/compiler 실행이나 ARM64 Wine/FEX guest↔native end-to-end 완료를 뜻하지 않는다.

**Metal 4 ICB ordering 수정·fresh 회귀:** SO와 일반 indirect 경로는 vertex-stage resolver가 쓴 ICB·root/VB 인자를 buffer memory barrier(vertex producer → vertex consumer)로 공개한 뒤 indirect execute한다(`src/d3d12/d3d12_command_queue.cpp`, `d3d12_command_allocator.cpp`). 동일 recovery `runtime-01`의 `build/local/metal4/recovery/integration/runtime01-advanced{32,64}-01/{stream-output,indexed-restart,msaa-lines}/{result.json,stdout.log}`에서 두 ABI의 6개 suite 실행이 API/Shader Validation 오류·외부 backend 로드 없이 PASS했다. SO packed DXIL capture는 written/needed 3·filled 36·VS invocations 3, SO 총 capture 43·validation 112를 확인했다. Indexed restart는 DXBC/DXIL 각 31개와 이전 indirect strip count/predicate 경로를 통과했고 MSAA-lines는 3 mode의 direct/indirect pixel·visibility 결과가 일치했다. 4× MSAA 경로는 ICB를 쓰지 않으므로 해당 성공을 ICB barrier의 단독 효과로 돌리지 않는다. 같은 runtime에는 별도 visibility 수정도 포함된다. Generic native oracle은 barrier 유무 양쪽이 통과해 원래 fault의 고립된 인과는 확인하지 못했다. 삭제된 원래 fault 로그는 복원되지 않았으며 과거 관찰은 transcript-only다. 이 회귀 proof만으로 전체 conformance·ARM64 Wine/FEX 완료를 주장하지 않는다.

**Fresh recovery proof(M5 Pro/macOS 27):** `build/local/metal4/recovery/logic-op/acceptance.json`은 source-built `runtime-01`에서 feature-level suite의 x64/WoW64 각 4 case PASS·API/Shader Validation 오류 0·외부 backend 로드 0을 확인한다. 7 UINT format × 16 logic op와 depth/discard·mixed MRT·write mask·UAV/statistics의 9개 경계, forced 1/4/8/16·TIR16의 정확한 occlusion, forced1의 4×/8× RTV mask/statistics, 필수 typed UAV-load 18 format의 texture/buffer 결과를 포함한다. 이전 forced/TIR 실패는 이 fresh suite에서 닫았지만, 이 개별 x64/WoW64 consumer proof는 전체 conformance·ARM64 Wine/FEX acceptance가 아니다. 이전 custom native GPU matrix 로그는 여전히 유실 상태다.

- **D3D12 Metal 4 구현 전환 — 완료:** command allocator·queue·submission의 재사용·완료·retirement, argument table·residency·barrier, graphics/compute 등 모든 PSO의 `MTL4Compiler` 생성·복원, native-frequency query, D3D12 presentation·temporal을 연결했고 위 mixed-cut fixture acceptance로 검증했다. D3D11 classic backend는 유지한다. Native API 314/domain 및 owned AIR 3.1→`MTL4Compiler`의 192-word GPU PASS는 삭제 전 개별 native 관찰이고, P3 baseline `910b992`와 당시 runtime-03/04도 기존 backend의 역사적 proof다. 이들을 현재 통합 proof로 전용하지 않으며 전체 conformance·측정하지 않은 성능 향상을 주장하지 않는다.
  - **2026-10-02 삭제 전 관찰(M5 Pro/macOS 27 한정, artifact 현재 유실):** core는 API/Shader Validation에서 x64/WoW64 각 12, 총 24 case PASS였다. 당시 `build/local/metal4/integration/core-composite-runtime02-03.json`은 runtime-02 기준과 runtime-03 raster-init 수정의 합산이며 단일 runtime 전체 suite 통과가 아니었다. 당시 `build/local/metal4/presentation/pe-acceptance.json`은 geometry·color-space·HDR drawable GPU readback 3 fixture와 temporal consumer를 두 ABI에서 확인했다. x64 temporal은 원래 analyzer 실패를 유지한 상태에서 parent/child submission ID 가정을 고친 저장 trace 재분석으로 PASS(59 executions·56 destroyed frames)했으며 GPU 재실행은 아니었다. 그 원본 실패·trace·재분석 JSON도 현재 유실됐다. Physical compositor/HDR scanout 증거는 아니었다. D3D11 classic의 768-pixel exact ClearView PASS도 당시 `build/local/uma-memory/metal4-d3d11-clearview.stdout.log`에서 확인했으나 로그는 현재 없다. Recovery 시작 시 SO GPU fault·indexed indirect mismatch·RTAS size-only validation assert·visibility reset ordering은 당시 미해결이었다. 해당 fixture 경로는 이후 fresh 실행과 위 최종 118/118 acceptance에서 닫았으며 초기 실패를 현재 blocker로 남기거나 삭제 전 로그를 새 통합 증거로 전용하지 않는다.
- **UMA VRAM 호환성 보고 — 구현·Wine 검증 완료:** `src/dxgi/dxgi_adapter.cpp`·`dxgi_memory.hpp`에서 `R = GlobalMemoryStatusEx().ullTotalPhys`(OS-visible RAM), `G = min(floor(4R/5), max(saturating(R - 16 GiB), floor(R/2)))`를 사용한다. `saturating`은 음수 대신 0으로 제한하며, 4R 계산은 overflow 없이 수행한다. UMA `DedicatedVideoMemory = min(Metal recommendedMaxWorkingSetSize, G, SIZE_T_MAX)`로 32-bit에서도 wrap 대신 포화한다. [Windows 계산 문서](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/calculating-graphics-memory)의 G는 `TotalSystemMemoryAvailableForGraphics`이며 물리 dedicated VRAM이나 Windows carveout이 아니다. 이를 DXGI dedicated 값으로 사용하는 것은 번역기의 명시적 호환성 광고 정책이다. Non-UMA의 기존 변환, `SharedSystemMemory = DedicatedSystemMemory = 0`, `QueryVideoMemoryInfo`의 기존 Metal budget·usage·reservation 동작은 유지한다.
  - CPU proof: `graphics-translator/tests/dxgi-memory.test.cpp` PASS. 16/24/32/48/64/96 GiB, 32/80 GiB ±1 byte, `UINT64_MAX` overflow 경계, Metal cap 및 32-bit 포화를 검증했다.
  - 삭제 전 Wine 관찰(원본 로그 유실): 당시 `build/local/uma-memory/{baseline64-fresh,baseline32-fresh,after64-fresh,after32-fresh}.stdout.log`의 새 prefix x64/WoW64 readback이 모두 PASS였다. `R = 68719472640`(64 GiB host에서 Windows가 보는 값은 4 KiB 작음), Metal recommendation `55662788608`, `G = 51539603456` bytes였다. `GetDesc`·`GetDesc1/2/3` 모두 x64는 이전 `27831394304` → `51539603456`, x86은 이전 wrap `2061590528` → 포화 `4294967295` bytes를 확인했다. Budget은 두 ABI에서 `55662788608` bytes로 그대로였다. 이는 adapter 보고 값의 당시 관찰이지 원본 artifact 보유나 실제 dedicated allocation·게임 호환성·성능 향상 증거가 아니다.
  - Fresh recovery: `build/local/metal4/recovery/uma/runtime01-adapter{64,32}/stdout.log`에서 `GetDesc`·`GetDesc1/2/3` 모두 x64 `51539603456`, x86 `4294967295`, budget `55662788608` bytes와 shared/dedicated-system 0을 다시 확인했다. 같은 경로의 `runtime01-d3d11-clearview64/stdout.log`는 유지한 classic D3D11의 exact RGBA 768 pixels PASS다. 이는 삭제된 before/after 로그 복원이 아니라 새 runtime의 독립 실행이다.


## Full SR/FG port — 2026-10-02

기존 ZZZ Wine SR/FG 경로의 이식은 root-owned D3D12→WineMetal→Metal4FX에 연결한다. SR source는 `zzz-wine-yaagl-pr-runtime/d3dmetal-pso-cache`의 `metalfx-backend.mm/.hpp`, `metalfx-contract.hpp`, `fsr-kernels.metal`에서 가져왔지만 이 저장소의 실행 의존성에 D3DMetal을 추가하지 않았다. D3D11 기능 확대나 unrelated dirty-tree 정리는 하지 않는다.

- **SR cutover:** high-res/jittered MV·reactive/composition masks·공식 NGX input/output offsets·linear/sRGB/PQ·RCAS·manual/auto exposure·dynamic render/output을 shared native backend로 연결했다. Output/layout generation을 성공 후 교체하고 old recording/replay는 자신이 보유한 generation과 GPU-retirement lease를 사용한다. Active input extent·generation/activation 전환·caller reset은 history를 reset한다.
- **FSR scale 계약 변경:** runtime min/max 조회와 centered uniform cap을 사용한다. Native 상한을 넘는 requested output은 active capped 영역과 opaque-black margin으로 채우며 output subrect 바깥은 보존한다. 예전853→2560 무조건 거부·portable scale[1,3] 가정은 현행 계약이 아니다. M5 Pro에서 독립 native descriptor query로 관찰한 min1/max3은 장치별 증거다.
- **Native synchronization:** preprocessing→Metal4FX→finish/copy를 기존 queue recording의 세 native command-buffer segment로 나누고 all-stage/device-visibility producer barriers를 사용한다. 같은 native buffer의 MetalFX와 외부 fence consumer 조합을 제거한다. Windows queue와 shared-event cross-queue timeline은 그대로이며 context/resource final release 이후에도 recording·native execution lease가 retirement까지 수명을 보유한다.
- **FG cutover:** source-owned `amd_fidelityfx_framegeneration_dx12.dll`을 optional FSR group에 포함했다. Configure/Create/Destroy/Dispatch/Query는 reference ordinal1/2/3/4/5다. Pinned MIT GPUOpen SDK `0836aa7f24058b3f8d035a88c0774457d78fc98e`의 swapchain/UI/pacing proxy가 original vendor DLL을 대신하며 owned COM extension으로 Metal4FX FrameInterpolator를 실행한다. Prepare의 depth/MV GPU snapshot, Generate의 camera/rect/transfer/HUDless/history, cross-queue publication과 in-flight lease를 연결했다. Foreign private C++ ABI·D3DMetal/AntiLag vendor fallback은 없다.
- **Full-port cut 당시 한계(후속 P1의 대체 계약은 위 안내 참조):** NGX alpha128·nonidentity Exposure.Scale·nonzero MV.Offset, SR dispatch debug draw, FG single-sample2D/output1개/configured frame ID64개 상한·debug tear-lines/reset-indicators/view/reserved flags·ResizeBuffers1 per-buffer queues·native FG allocation total query 제한은 현행 [CORE 계약](CORE-IMPLEMENTATION-STATUS.ko.md#fsr-facade와-manual-r32-exposure-완료)을 따른다. FSR SR create hints256/512(non-linear color/visualization permission)는 dispatch debug draw와 구분해 허용한다. Public facades는 x64이며 ARM64 Wine/FEX public guest 실행은 기존 user-deferred gate다.

**독립 native 관찰과 알려진 platform 경계:** [scale control](../build/local/full-sr-port-20261002/native-scale-control.log)은 M5 Pro min1/max3을 확인했다. [native16 stderr](../build/local/full-sr-port-20261002/native-sr-16-control.stderr.log)은 production SR backend 없이 public Metal4FX16→48에서도 native `Conv3x3` invalid stores를 재현했다. 따라서 작은 입력의 portable 지원이나 모든 shader-validation size의 성공을 주장하지 않는다. Minimum-size guard나 validation suppression으로 숨기지 않았다. [HDR pattern control](../build/local/full-sr-port-20261002/native-sr-64-pattern.stdout.log)의 manual half exposure3555/3cf0/4248은42550/48924 half components 차이를 보여 주며 constant/reset 입력의 byte sameness를 exposure 미구현의 oracle로 쓰지 않는다. Public suite 판정과 native control은 별도 증거다.

**Fresh private temporal acceptance:** `build/local/full-metalfx-port-20261002/runtime-02` manifest `7e52984319e4636e6443788b61b1f624f7c2a5393617994bdabe4f865eb02342`의 [x64](../build/local/full-temporal-port-20261002/runtime02-x64-validation/result.json)·[WoW64](../build/local/full-temporal-port-20261002/runtime02-x86-validation/result.json)는 exit0, API/Shader errors0, foreign backend0, PASS다. Typed/typeless 각8 format, cross-queue/opposite-order8 iterations, allocator retirement와 gated release,96×112→128×128→96×112 output generation의 old-A poisoned replay/fresh 일치·outside sentinel, GPU R32 exposure3값×3 replay의 R16 maxULP0을 확인했다. Trace analysis는 ABI별69 executions/69 submissions·errors0이다. 이는 private SR transport/lifetime proof이며 public all-effects/FG acceptance나 ARM64 guest·게임/품질 proof로 전용하지 않는다.

**Public SR acceptance:** 같은 runtime02의 public x64 [NGX](../build/local/full-sr-port-20261002/runtime02-ngx-api-shader/result.json)·[contracts](../build/local/full-sr-port-20261002/runtime02-ngx-contracts-api-shader/result.json)는 original strict PASS다. [FSR original](../build/local/full-sr-port-20261002/runtime02-fsr-api-shader/result.json)은 actual GPU exit0·API/Shader errors0·foreign0지만 stale cap/history guards의 pass=false를 그대로 보존했다. Cap fixture는 native64→192를 requested256×224 안의32,16에 두며 reactive/composition/combined history 동등성은 실제2비교다. 현행 runner로 같은 frozen stdout/stderr/backend logs를 재분석한 [derived result](../build/local/full-sr-port-20261002/runtime02-fsr-guard-reanalysis-965874de/result.json)는 PASS, `evidence_mode=recorded-output-reanalysis`·`process_executed=false`다. 이것은 새 GPU 실행이 아니다. Public x86 bootstrap error126은 기존 x64-only frontend의 unavailable ABI로 보존하며 private WoW64 proof와 혼동하지 않는다.

**Public FG acceptance:** [fresh after-validation06](../build/local/full-fg-port-20261002/after-validation-06/result.json)는 같은 runtime02의 실제 새 GPU 실행으로 exit0·API/Shader errors0·foreign0·strict PASS다. Direct FFX Prepare snapshot/history/config64·수명, HWND proxy/callback/UI/resize와 실제 native drawable content/order/time gates를 통과했다. [Independent public Metal4FX comparison](../build/local/full-fg-port-20261002/after-validation-06/fg-capture-native-control.json)의 frame1–4는 byte-identical/max_abs_error0이다.04/05의 premultiplied/double-buffered UI 실패는 SDK가 frame마다 소비하는 등록을 fixture가 재등록하지 않은 것이었고06은 각 UI frame에 등록한 뒤 동일한 pixel·resize·Present 후 원본 mutation oracle을 유지했다.18 attempted drawables 중17개만 positive timestamp이며 첫 generated timestamp0은 skipped/not-displayed로 명시한다. Ordered bounded progress를 증명한 결과이지 exact2×OS cadence·물리 compositor·게임/품질/성능 proof가 아니다.

## 최종 경계

이번 문서는 **무엇을 소스에 구현했고 무엇을 아직 증명하지 않았는지**를 구분한 상태 기록이다.

- 구현 경로가 있다는 이유로 build/runtime success를 선언하지 않는다.
- P1-A·P1-B·P1-C·P1-D(VS→SO), 일반 non-SO indexed restart, SO 오류 전파·WoW64 연결·ExecuteIndirect를 구현·검증했다. P1-C는 실제 MSAA pixel-frequency 실행 수정 후 11-field query를 공개했다. `AtomicCopyBufferUINT64`는 macOS Apple8+ 대상으로 구현했으며 실제 장치·reader 관찰 범위는 P1-A 결과에 명시한다. Pipeline library·tiled·shared·enhanced barrier·GS/HS/DS 등 P2 결과와 남은 제약·미검증은 [P2 완료 결과](#p2-완료-결과-2026-10-01)에 구분한다. P3의 검증된 DXR 1.1·FL12_1·conservative raster 1·feedback 1.0·view instancing 1과 GSTS/SO 후속 결과는 [P3 완료 결과](#p3-완료-결과-2026-10-01)에 있다. P3 전체 및 최종 64-bit/WoW64 통합 회귀 완료는 아직 선언하지 않는다.
- 실제 게임 정상 렌더링, 성능, 전체 D3D12/DXR conformance를 주장하지 않는다.
- 이후 검증을 진행할 때는 새 build/install과 **새 runtime 복사본·전용 prefix·새 evidence 경로**를 사용해야 한다. 기존 게임 또는 live prefix에는 배포하지 않는다.
