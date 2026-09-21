#!/usr/bin/env python3
"""CPU/PE-only validation for the DXMT D3D12 NGX frontend.

Compiles the current repository source with the pinned llvm-mingw toolchain,
links a nvngx.dll from the Meson cross-build import libraries, and validates the
resulting PE against the public NVIDIA NGX ABI:

* the D3D11 export set is preserved and the D3D12 entry points exist,
* the owned `own-ngxd3d12` marker is embedded,
* the MSVC-ABI parameter facade exposes the official 17-slot vtable surface in
  the official order, and the official-header consumer resolves each vcall slot
  to the matching facade method.

No GPU, Wine runtime or D3D12 device is created.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "build/local/build"
DEFAULT_OUT = ROOT / "build/local/ngx-frontend"
NVIDIA_INCLUDE = ROOT / "external/nvngx/include"
CONSUMER = ROOT / "graphics-translator/tests/owned-ngx-parameter-consumer-msvc.cpp"
DEFAULT_TOOLCHAIN = Path("/opt/llvm-mingw-20260616-ucrt-macos-universal/bin")

COMMON = [
    f"-I{ROOT / 'src/nvngx'}",
    f"-I{ROOT / 'src/dxgi'}",
    f"-I{ROOT / 'src/util'}",
    f"-I{ROOT / 'src/winemetal'}",
    f"-I{ROOT / 'include'}",
    f"-I{ROOT / 'libs'}",
    "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST",
    "-D_FILE_OFFSET_BITS=64",
    "-Wall", "-Wextra", "-Werror", "-std=c++20", "-O2", "-g",
    "-DDXMT_PAGE_SIZE=4096", "-Wimplicit-fallthrough",
    "-Wno-missing-field-initializers", "-Wno-unused-parameter",
    "-Wno-cast-function-type", "-Wno-unused-private-field",
    "-Wno-microsoft-exception-spec", "-Wno-extern-c-compat",
    "-Wno-unused-const-variable", "-Wno-missing-braces",
    "-fblocks", "-DNOMINMAX", "-D_WIN32_WINNT=0xa00", "-gdwarf-4",
]

REQUIRED_D3D11 = {
    "NVSDK_NGX_D3D11_AllocateParameters", "NVSDK_NGX_D3D11_CreateFeature",
    "NVSDK_NGX_D3D11_DestroyParameters", "NVSDK_NGX_D3D11_EvaluateFeature",
    "NVSDK_NGX_D3D11_GetCapabilityParameters", "NVSDK_NGX_D3D11_GetFeatureRequirements",
    "NVSDK_NGX_D3D11_GetParameters", "NVSDK_NGX_D3D11_GetScratchBufferSize",
    "NVSDK_NGX_D3D11_Init", "NVSDK_NGX_D3D11_Init_Ext",
    "NVSDK_NGX_D3D11_Init_ProjectID", "NVSDK_NGX_D3D11_Init_with_ProjectID",
    "NVSDK_NGX_D3D11_ReleaseFeature", "NVSDK_NGX_D3D11_Shutdown",
    "NVSDK_NGX_D3D11_Shutdown1",
}

REQUIRED_D3D12 = {
    "NVSDK_NGX_D3D12_AllocateParameters", "NVSDK_NGX_D3D12_CreateFeature",
    "NVSDK_NGX_D3D12_DestroyParameters", "NVSDK_NGX_D3D12_EvaluateFeature",
    "NVSDK_NGX_D3D12_EvaluateFeature_C", "NVSDK_NGX_D3D12_GetCapabilityParameters",
    "NVSDK_NGX_D3D12_GetFeatureRequirements", "NVSDK_NGX_D3D12_GetParameters",
    "NVSDK_NGX_D3D12_GetScratchBufferSize", "NVSDK_NGX_D3D12_Init",
    "NVSDK_NGX_D3D12_Init_Ext", "NVSDK_NGX_D3D12_Init_ProjectID",
    "NVSDK_NGX_D3D12_Init_with_ProjectID", "NVSDK_NGX_D3D12_ReleaseFeature",
    "NVSDK_NGX_D3D12_Shutdown", "NVSDK_NGX_D3D12_Shutdown1",
}

# Official NVSDK_NGX_Parameter vtable surface: 8 Set, 8 Get, Reset. Slot
# offsets come from the official header via the consumer fixture below; this map
# records which facade method must back each parameter type.
FACADE_SET = [
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBD_K@Z",
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBDM@Z",
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBDN@Z",
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBDI@Z",
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBDH@Z",
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBDPEAUID3D11Resource@@@Z",
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBDPEAUID3D12Resource@@@Z",
    "?Set@OwnedNgxParameterFacade@@UEAAXPEBDPEAX@Z",
]
FACADE_GET = [
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEA_K@Z",
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEAM@Z",
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEAN@Z",
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEAI@Z",
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEAH@Z",
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEAPEAUID3D11Resource@@@Z",
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEAPEAUID3D12Resource@@@Z",
    "?Get@OwnedNgxParameterFacade@@UEBAIPEBDPEAPEAX@Z",
]
FACADE_RESET = "?Reset@OwnedNgxParameterFacade@@UEAAXXZ"

# Facade method that must back each official-header vcall emitted by the
# consumer fixture. The slot offsets themselves are read from the compiled
# consumer, so the official header stays the single source of truth.
CONSUMER_SLOTS = {
    "ngx_smoke_set_float": FACADE_SET[1],
    "ngx_smoke_set_uint": FACADE_SET[3],
    "ngx_smoke_set_int": FACADE_SET[4],
    "ngx_smoke_set_resource": FACADE_SET[6],
    "ngx_smoke_get_float": FACADE_GET[1],
    "ngx_smoke_get_uint": FACADE_GET[3],
    "ngx_smoke_get_int": FACADE_GET[4],
    "ngx_smoke_get_resource": FACADE_GET[6],
    "ngx_smoke_reset": FACADE_RESET,
}


def run(args: list[str]) -> str:
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def parse_relocations(dump: str, section: str) -> list[tuple[int, str, str]]:
    """Return (offset, type, symbol) records for one COFF section."""
    records = []
    current = None
    for line in dump.splitlines():
        header = re.match(r"RELOCATION RECORDS FOR \[(.*)\]:", line)
        if header:
            current = header.group(1)
            continue
        if current != section:
            continue
        fields = line.split()
        if len(fields) == 3 and re.fullmatch(r"[0-9a-f]+", fields[0]):
            records.append((int(fields[0], 16), fields[1], fields[2]))
    return records


def fail(message: str) -> None:
    raise SystemExit(f"OWNED_NGX_FRONTEND_FAIL {message}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, default=DEFAULT_BUILD,
                        help="Meson cross build directory of the current source")
    parser.add_argument("--out", type=Path,
                        help="scratch directory for compiled objects and the PE")
    parser.add_argument("--nvidia-include", type=Path,
                        default=Path(os.environ.get("NGX_SDK_INCLUDE", NVIDIA_INCLUDE)),
                        help="directory holding the official nvsdk_ngx*.h headers")
    parser.add_argument("--toolchain", type=Path, default=DEFAULT_TOOLCHAIN,
                        help="llvm-mingw bin directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build = (args.build_root if args.build_root.is_absolute()
             else ROOT / args.build_root).resolve()
    out = (args.out or DEFAULT_OUT)
    out = (out if out.is_absolute() else ROOT / out).resolve()
    toolchain = args.toolchain
    nvidia_include = (args.nvidia_include if args.nvidia_include.is_absolute()
                      else ROOT / args.nvidia_include).resolve()

    cxx = toolchain / "x86_64-w64-mingw32-g++"
    objdump = toolchain / "llvm-objdump"
    nm = toolchain / "llvm-nm"
    msvc_cxx = toolchain / "clang++"
    for tool in (cxx, objdump, nm, msvc_cxx):
        if not tool.is_file():
            fail(f"pinned llvm-mingw toolchain is missing {tool}")

    nvngx_header = nvidia_include / "nvsdk_ngx.h"
    if not nvngx_header.is_file():
        fail(f"official NGX header not found at {nvngx_header}; pass --nvidia-include")

    dxgi_implib = build / "src/dxgi/dxgi.dll.a"
    util_implib = build / "src/util/libutil.a"
    winemetal_implib = build / "src/winemetal/winemetal.dll.a"
    for implib in (dxgi_implib, util_implib, winemetal_implib):
        if not implib.is_file():
            fail(f"cross-build import library is missing {implib}; build first")

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    objects = []
    for name in ("nvngx.cpp", "nvngx_d3d12.cpp"):
        obj = out / f"{name}.obj"
        run([str(cxx), *COMMON, "-c", str(ROOT / "src/nvngx" / name), "-o", str(obj)])
        objects.append(obj)

    msvc_obj = out / "nvngx_d3d12_msvc.obj"
    run([
        str(msvc_cxx), "--target=x86_64-pc-windows-msvc", "-std=c++20",
        "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
        "-c", str(ROOT / "src/nvngx/nvngx_d3d12_msvc.cpp"), "-o", str(msvc_obj),
    ])
    symbols = run([str(nm), str(msvc_obj)])
    if "??_7OwnedNgxParameterFacade@@6B@" not in symbols:
        fail("MSVC parameter facade vftable was not emitted")
    if symbols.count("?Set@OwnedNgxParameterFacade@@") != len(FACADE_SET):
        fail("MSVC parameter facade does not expose the official 8 Set slots")
    if symbols.count("?Get@OwnedNgxParameterFacade@@") != len(FACADE_GET):
        fail("MSVC parameter facade does not expose the official 8 Get slots")
    objects.append(msvc_obj)

    # The vftable must carry exactly the official overload set, and each
    # official-header vcall slot must resolve to the facade method whose
    # parameter type matches. The facade's own declaration order is free; the
    # official header, as interpreted by the same MSVC-ABI compiler, pins the
    # slots that an MSVC-built application will call.
    vftable = parse_relocations(run([str(objdump), "-r", str(msvc_obj)]), ".rdata")
    facade_slots = {offset: value for offset, kind, value in vftable
                    if kind == "IMAGE_REL_AMD64_ADDR64"
                    and "OwnedNgxParameterFacade" in value}
    if len(facade_slots) != len(FACADE_SET) + len(FACADE_GET) + 1:
        fail(f"facade vftable has {len(facade_slots)} slots, expected 17")
    expected_symbols = set(FACADE_SET + FACADE_GET + [FACADE_RESET])
    if set(facade_slots.values()) != expected_symbols:
        fail(f"facade vftable symbol set drift: {sorted(facade_slots.values())}")

    # Compile the repository's official-header consumer bridge with the same
    # Microsoft C++ ABI. The vcall offsets it emits come from NVIDIA's public
    # declaration; each one must resolve in the facade vftable to the facade
    # method with the same parameter type. This is link/run independent and
    # catches drift between the owned facade and the pinned official header.
    consumer_obj = out / "owned-ngx-parameter-consumer-msvc.obj"
    run([
        str(msvc_cxx), "--target=x86_64-pc-windows-msvc", "-std=c++20",
        "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
        f"-I{nvidia_include}", "-c", str(CONSUMER), "-o", str(consumer_obj),
    ])
    consumer_disassembly = run([str(objdump), "-d", str(consumer_obj)])
    for function, symbol in CONSUMER_SLOTS.items():
        block = re.search(rf"<{re.escape(function)}>:(.*?)(?=\n\n|\Z)",
                          consumer_disassembly, re.S)
        if not block:
            fail(f"official-header consumer function missing from object: {function}")
        calls = re.findall(r"callq?\s+\*0x([0-9a-f]+)\(%rax\)", block.group(1))
        if not calls:
            fail(f"official-header consumer does not emit a vcall: {function}")
        for offset in {int(value, 16) for value in calls}:
            if facade_slots.get(offset) != symbol:
                fail(f"official-header vcall slot {offset:#x} ({function}) maps to "
                     f"{facade_slots.get(offset)}, expected {symbol}")

    dll = out / "nvngx.dll"
    run([
        str(cxx), "-o", str(dll), *(str(x) for x in objects), "-shared",
        "-Wl,--start-group", f"-Wl,--out-implib={out / 'nvngx.dll.a'}", "-static",
        "-Wl,--file-alignment=4096",
        str(dxgi_implib), str(util_implib), str(winemetal_implib),
        "-lkernel32", "-luser32", "-lgdi32", "-lwinspool", "-lshell32",
        "-lole32", "-loleaut32", "-luuid", "-lcomdlg32", "-ladvapi32",
        "-Wl,--end-group",
    ])

    dump = run([str(objdump), "-p", str(dll)])
    exports = set(re.findall(r"\b(NVSDK_NGX_D3D(?:11|12)_[A-Za-z0-9_]+)\s*$", dump, re.MULTILINE))
    missing11 = sorted(REQUIRED_D3D11 - exports)
    missing12 = sorted(REQUIRED_D3D12 - exports)
    if missing11 or missing12:
        fail(f"missing exports: d3d11={missing11} d3d12={missing12}")
    if b"own-ngxd3d12\x00" not in dll.read_bytes():
        fail("owned D3D12 NGX marker missing from PE")

    print(f"OWNED_NGX_FRONTEND_PASS pe={dll}")
    print(f"D3D11_EXPORTS_PRESERVED={len(REQUIRED_D3D11)} D3D12_EXPORTS={len(REQUIRED_D3D12)}")
    print("MSVC_PARAMETER_VFTABLE=1 MSVC_OFFICIAL_HEADER_CONSUMER=1 MSVC_VCALL_SLOTS=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
