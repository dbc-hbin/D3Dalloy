#!/usr/bin/env python3
"""Source-owned compile/COFF/PE facade ABI proof; never executes a Windows guest.

--abi-only --all-abis compiles NGX, FSR SR/FG/RR and NVAPI production sources with
existing compilers for x86/x64/ARM64/ARM64EC. The parameter vtable is compared
with an independent official-header oracle, including x86 thiscall and all 17
slots. RR 1.2 and legacy 1.0/1.1 PODs are compared across MSVC/MinGW C/C++
frontends. Without --abi-only, link NGX against the selected Meson build libraries.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from owned_graphics_arch import inspect_pe

ROOT = Path(__file__).resolve().parent.parent
TOOLCHAIN = Path("/opt/llvm-mingw-20260616-ucrt-macos-universal/bin")
ARCHES = {
    "x86": ("i686", "i686", 0x14C),
    "x64": ("x86_64", "x86_64", 0x8664),
    "arm64": ("aarch64", "aarch64", 0xAA64),
    "arm64ec": ("arm64ec", "arm64ec", 0xA641),
}
COMMON = [
    *(f"-I{ROOT / path}" for path in ("src/nvngx", "src/dxgi", "src/util", "src/winemetal", "include", "libs", "external/nvapi")),
    "-std=c++20", "-O2", "-g", "-Wall", "-Wextra",
    "-DD3DALLOY_PAGE_SIZE=4096", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", "-fblocks",
    "-Wno-missing-field-initializers", "-Wno-unused-parameter", "-Wno-cast-function-type",
    "-Wno-unused-private-field", "-Wno-microsoft-exception-spec", "-Wno-extern-c-compat",
    "-Wno-unused-const-variable", "-Wno-missing-braces",
]
NGX_SUFFIXES = {
    "AllocateParameters", "CreateFeature", "DestroyParameters", "EvaluateFeature",
    "GetCapabilityParameters", "GetFeatureRequirements", "GetParameters", "GetScratchBufferSize",
    "Init", "Init_Ext", "Init_ProjectID", "Init_with_ProjectID", "ReleaseFeature", "Shutdown", "Shutdown1",
}


def run(command: list[str], log: Path) -> str:
    result = subprocess.run(command, text=True, capture_output=True)
    log.write_text(json.dumps(command) + "\n" + result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); see {log}")
    return result.stdout


def vtable(ir: str) -> list[str]:
    line = next(line for line in ir.splitlines() if line.startswith('@"??_7OwnedNgxParameterFacade@@6B@" ='))
    slots = re.findall(r'ptr @"([^"]+)"', line)
    if len(slots) != 17:
        raise RuntimeError(f"parameter vftable has {len(slots)} slots, expected 17")
    return slots


def verify_abi(abi: str, args: argparse.Namespace, out: Path) -> list[Path]:
    prefix, msvc, machine = ARCHES[abi]
    cc = args.toolchain / (prefix + "-w64-mingw32-gcc")
    cxx = args.toolchain / (prefix + "-w64-mingw32-g++")
    clang = args.toolchain / "clang++"
    objdump = args.toolchain / "llvm-objdump"
    readobj = args.toolchain / "llvm-readobj"
    out.mkdir(parents=True, exist_ok=True)
    flags = [str(clang), f"--target={msvc}-pc-windows-msvc", "-std=c++20", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror", f"-I{args.nvidia_include}"]
    bridge = ROOT / "src/nvngx/nvngx_d3d12_msvc.cpp"
    oracle = ROOT / "graphics-translator/tests/owned-ngx-vtable-reference-msvc.cpp"
    for name, source in (("facade", bridge), ("reference", oracle)):
        run([*flags, "-S", "-emit-llvm", str(source), "-o", str(out / (name + ".ll"))], out / (name + "-ir.log"))
        run([*flags, "-c", str(source), "-o", str(out / (name + ".obj"))], out / (name + "-coff.log"))
    actual_ir = (out / "facade.ll").read_text()
    reference_ir = (out / "reference.ll").read_text()
    layout_source = ROOT / "graphics-translator/tests/owned-facade-pod-abi.cpp"
    layout_symbols = ("owned_facade_pod_layout", "owned_rr_12_pod_layout",
                      "owned_rr_legacy_pod_layout", "owned_rr_bridge_pod_layout")
    layouts = []
    layout_compilers = (
        ("msvc", [flag for flag in flags if flag != "-Werror"]),
        ("mingw", [str(cxx), "-std=c++20"]),
        ("msvc-c", [str(args.toolchain / "clang"), f"--target={msvc}-pc-windows-msvc", "-x", "c", "-std=c11", "-Wall", "-Wextra"]),
        ("mingw-c", [str(cc), "-x", "c", "-std=c11"]),
    )
    for name, compiler_flags in layout_compilers:
        layout_path = out / (name + "-pod.ll")
        run([*compiler_flags, "-ffreestanding", "-S", "-emit-llvm", str(layout_source), "-o", str(layout_path)], out / (name + "-pod.log"))
        layout_ir = layout_path.read_text().splitlines()
        layout = {
            symbol: re.findall(r"i32 (\d+)", next(line for line in layout_ir if symbol in line and "constant" in line))
            for symbol in layout_symbols
        }
        if any(not values for values in layout.values()) or (layouts and layout != layouts[0]):
            raise RuntimeError(f"{abi} {name} public FFX/RR POD layout mismatch: {layout} != {layouts[:1]}")
        layouts.append(layout)
    actual, expected = vtable(actual_ir), vtable(reference_ir)
    # Enum and unsigned return values both lower to i32. Compare parameter
    # identity/order independently of that deliberately different C++ spelling.
    expected = [name.replace('?AW4NVSDK_NGX_Result@@', 'I') for name in expected]
    if actual != expected:
        raise RuntimeError(f"{abi} official-header vtable mismatch: {actual} != {expected}")
    for symbol in actual:
        signature = next(line for line in actual_ir.splitlines() if line.startswith("define ") and f'@"{symbol}"(' in line)
        if abi == "x86" and "x86_thiscallcc" not in signature:
            raise RuntimeError(f"x86 vtable method lacks thiscall: {symbol}")
        if "?Get@" in symbol and not re.search(r'\bi32 @', signature):
            raise RuntimeError(f"parameter Get result is not i32: {symbol}")
    facade_dump = run([str(objdump), "-r", str(out / "facade.obj")], out / "facade-relocations.log")
    # Every compiler-emitted slot must survive into the real COFF object.
    coff_slots = [(symbol.replace("@@", "@@$$h", 1) if abi == "arm64ec" else symbol) for symbol in actual]
    if any(symbol not in facade_dump for symbol in coff_slots):
        raise RuntimeError(f"{abi} COFF parameter vtable is incomplete")
    consumer = ROOT / "graphics-translator/tests/owned-ngx-parameter-consumer-msvc.cpp"
    run([*flags, "-c", str(consumer), "-o", str(out / "consumer.obj")], out / "consumer.log")
    run([str(objdump), "-d", str(out / "consumer.obj")], out / "consumer-disassembly.log")
    objects = []
    compiled = {}
    sources = ["nvngx/nvngx.cpp", "nvngx/nvngx_d3d12.cpp", "fsr/fsr.cpp", "fsrfg/fsrfg.cpp",
               "fsrrr/fsrrr.c", "fsrrr/fsrrr_backend.cpp", "nvapi/nvapi.cpp"]
    sources += ["fsrfg/sdk/" + name for name in ("sdk_backend.cpp", "FrameInterpolationSwapchainDX12.cpp", "FrameInterpolationSwapchainDX12_Helpers.cpp", "FrameInterpolationSwapchainDX12_UiComposition.cpp", "FrameInterpolationSwapchainDX12_DebugPacing.cpp")]
    for source in sources:
        name = source.replace("/", "-")
        obj = out / (name + ".obj")
        compiler, source_flags = cxx, COMMON
        if source.endswith(".c"):
            compiler = cc
            source_flags = [flag for flag in COMMON if flag != "-std=c++20"] + ["-std=c11"]
        run([str(compiler), *source_flags, "-c", str(ROOT / "src" / source), "-o", str(obj)], out / (name + ".log"))
        dump = run([str(readobj), "--file-headers", str(obj)], out / (name + "-headers.log"))
        if f"0x{machine:X}" not in dump:
            raise RuntimeError(f"wrong COFF machine for {abi}: {obj}")
        if source.startswith("nvngx/"):
            objects.append(obj)
        compiled[source] = obj
    spec = importlib.util.spec_from_file_location("stage_owned_graphics", ROOT / "scripts/stage-owned-graphics.py")
    assert spec and spec.loader
    stage = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage)
    pe_facts = {}
    for directory, module in (("fsr", "amd_fidelityfx_upscaler_dx12"), ("fsrfg", "amd_fidelityfx_framegeneration_dx12"),
                              ("fsrrr", "amd_fidelityfx_denoiser_dx12")):
        dll = out / (module + ".dll")
        inputs = [str(obj) for source, obj in compiled.items() if source.startswith(directory + "/")]
        run([str(cxx), "-shared", "-static", *inputs, str(ROOT / "src" / directory / (directory + ".def")),
             "-lole32", "-luuid", "-ldxguid", "-lwinmm", "-ldwmapi", "-o", str(dll)], out / (directory + "-link.log"))
        facts = inspect_pe(dll)
        if facts["windows_abi"] != abi or stage.pe_exports(dll) != stage.REQUIRED_FSR_EXPORTS:
            raise RuntimeError(f"{abi} FSR PE architecture/export/ordinal mismatch: {dll}")
        pe_facts[module] = facts
    # x86 cdecl entries cannot acquire stdcall @N suffixes. EC native C
    # entries must carry the compiler-generated # entry convention.
    for source, names in (("nvapi/nvapi.cpp", {"nvapi_QueryInterface"}),
                          ("fsr/fsr.cpp", stage.REQUIRED_FSR_EXPORTS.keys()),
                          ("fsrfg/fsrfg.cpp", stage.REQUIRED_FSR_EXPORTS.keys()),
                          ("fsrrr/fsrrr.c", stage.REQUIRED_FSR_EXPORTS.keys()),
                          ("fsrrr/fsrrr_backend.cpp", {"rr_backend_call"}),
                          ("nvngx/nvngx_d3d12.cpp", {"NVSDK_NGX_D3D12_" + suffix for suffix in NGX_SUFFIXES})):
        symbols = run([str(args.toolchain / "llvm-nm"), "--defined-only", str(compiled[source])], out / (source.replace("/", "-") + "-symbols.log"))
        actual_names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
        prefix = "_" if abi == "x86" else "#" if abi == "arm64ec" else ""
        if {prefix + name for name in names} - actual_names:
            raise RuntimeError(f"{abi} public C calling convention mismatch: {source}")
    objects.append(out / "facade.obj")
    (out / "result.json").write_text(json.dumps({
        "windows_abi": abi, "source_compile": "pass", "official_parameter_slots": 17,
        "x86_thiscall": abi == "x86", "official_ffx_pod_layout": layouts[0][layout_symbols[0]],
        "rr_pod_layouts": {symbol: layouts[0][symbol] for symbol in layout_symbols[1:]},
        "pod_layout_compilers": [name for name, _ in layout_compilers],
        "fsr_pe": pe_facts, "public_c_calling_conventions": "pass", "guest_execution": "not_run",
    }, indent=2) + "\n")
    print(f"OWNED_FACADE_ABI_PASS abi={abi} slots=17 sources={len(sources)} guest=not_run")
    return objects


def verify_parameter_cpu(args: argparse.Namespace) -> None:
    out = args.out.resolve() / "parameter-cpu"
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "parameter-contract"
    run([os.environ.get("CXX", "clang++"), "-std=c++20", "-Wall", "-Wextra",
         "-Wno-unused-parameter", "-DOWNED_NGX_PARAMETER_CPU_TEST",
         f"-I{ROOT / 'src/nvngx'}",
         str(ROOT / "graphics-translator/tests/owned-ngx-parameter-consumer-msvc.cpp"),
         "-o", str(exe)], out / "compile.log")
    print(run([str(exe)], out / "run.log"), end="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, default=ROOT / "build/local/build")
    parser.add_argument("--out", type=Path, default=ROOT / "build/local/ngx-frontend")
    parser.add_argument("--nvidia-include", type=Path, default=Path(os.environ.get("NGX_SDK_INCLUDE", ROOT / "external/nvngx/include")))
    parser.add_argument("--toolchain", type=Path, default=TOOLCHAIN)
    parser.add_argument("--windows-abi", choices=ARCHES, default="x64")
    parser.add_argument("--all-abis", action="store_true")
    parser.add_argument("--abi-only", action="store_true")
    parser.add_argument("--parameter-cpu-only", action="store_true")
    args = parser.parse_args()
    if args.parameter_cpu_only:
        verify_parameter_cpu(args)
        return 0
    if args.all_abis and not args.abi_only:
        parser.error("--all-abis requires --abi-only; link each ABI against its own --build-root")
    for abi in ARCHES if args.all_abis else (args.windows_abi,):
        out = args.out.resolve() / abi
        objects = verify_abi(abi, args, out)
        if args.abi_only:
            continue
        cxx = args.toolchain / (ARCHES[abi][0] + "-w64-mingw32-g++")
        dll = out / "nvngx.dll"
        libraries = [args.build_root / path for path in ("src/dxgi/dxgi.dll.a", "src/util/libutil.a", "src/winemetal/winemetal.dll.a")]
        x86_flags = ["-Wl,--kill-at", "-Wl,--enable-stdcall-fixup"] if abi == "x86" else []
        run([str(cxx), "-o", str(dll), *(str(obj) for obj in objects), "-shared", "-static", *x86_flags,
             "-Wl,--start-group", *(str(lib) for lib in libraries), "-lkernel32", "-luser32", "-lgdi32", "-lwinspool", "-lshell32", "-lole32", "-loleaut32", "-luuid", "-lcomdlg32", "-ladvapi32", "-Wl,--end-group"], out / "link.log")
        if inspect_pe(dll)["windows_abi"] != abi:
            raise RuntimeError(f"NGX PE ABI mismatch: {dll}")
        dump = run([str(args.toolchain / "llvm-readobj"), "--coff-exports", str(dll)], out / "pe.log")
        exports = set(re.findall(r"^\s+Name: (\S+)$", dump, re.MULTILINE))
        required = {f"NVSDK_NGX_D3D{version}_{suffix}" for version in (11, 12) for suffix in NGX_SUFFIXES} | {"NVSDK_NGX_D3D12_EvaluateFeature_C", "NVSDK_NGX_UpdateFeature"}
        if required != exports or b"own-ngxd3d12\0" not in dll.read_bytes():
            raise RuntimeError(f"NGX PE export/owned marker mismatch: missing={sorted(required - exports)}, extra={sorted(exports - required)}")
        print(f"OWNED_NGX_FRONTEND_PASS pe={dll}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, StopIteration) as error:
        raise SystemExit(f"OWNED_NGX_FRONTEND_FAIL {error}")
