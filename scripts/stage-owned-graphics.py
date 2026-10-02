#!/usr/bin/env python3
"""Stage and verify an isolated Wine runtime using the owned DXMT-derived graphics translator."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import uuid
from pathlib import Path
from owned_graphics_arch import architecture, inspect_pe, macho_architectures, runtime_architecture


SCHEMA = 2
MANIFEST = "owned-graphics-runtime.json"
WINDOWS_DIR = Path("lib/wine/x86_64-windows")
UNIX_DIR = Path("lib/wine/x86_64-unix")
DXMT_WINDOWS_DIR = Path("x86_64-windows")
DXMT_UNIX_DIR = Path("x86_64-unix")
# New-style WoW64: 32-bit PE modules run in i386-windows and reach the single
# x86_64 winemetal.so through Wine's 32-bit unix-call thunks.
WOW64_WINDOWS_DIR = Path("lib/wine/i386-windows")
DXMT_WOW64_DIR = Path("i386-windows")
PE_MACHINE_I386 = 0x14C
NATIVE_MODULE = "winemetal.so"
# The owned DXIL shader compiler is compiled from the owned patch series into
# airconv and linked into winemetal.so, so no owned graphics artifact may depend
# on a separately licensed/native compiler library or the GPTK backend.
FORBIDDEN_DEPENDENCY_TOKENS = ("d3dmetal", "libd3dshared", "metalirconverter", "dxilconv")
# Artifacts the removed external-SDK DXIL flow used to install. Any of these names
# anywhere in a DXMT install tree means a stale or substituted build, not the
# owned compiler.
REMOVED_COMPILER_ARTIFACT_NAMES = (
    "libdxmt-dxilconv.dylib",
    "libmetalirconverter.dylib",
    "metal-shader-converter",
)
# The same leftovers copied into a staged runtime. This is checked only under the
# owned native module directory: a packaged Wine runtime may legitimately carry
# its own D3DMetal.framework copy elsewhere, and that baseline is preserved.
FORBIDDEN_STAGED_ARTIFACTS = (
    UNIX_DIR / "libdxmt-dxilconv.dylib",
    UNIX_DIR / "libmetalirconverter.dylib",
    UNIX_DIR / "licenses/metal-shader-converter",
)
WINDOWS_MODULES = (
    "d3d12.dll",
    "dxgi.dll",
    "d3d11.dll",
    "d3d10core.dll",
    "winemetal.dll",
)
TRANSLATOR_ARTIFACTS = tuple(WINDOWS_DIR / name for name in WINDOWS_MODULES) + (
    UNIX_DIR / "winemetal.so",
)
OWNED_D3D12_MARKER = b"owned-graphics-d3d12"
# Wine's loader only accepts a builtin module whose image carries this marker;
# `winebuild --builtin` writes it, so a target-only build is not installable.
WINE_BUILTIN_MARKER = b"Wine builtin DLL"
OWNED_NGX_D3D12_MARKER = b"own-ngxd3d12"
FSR_MODULE = "amd_fidelityfx_upscaler_dx12.dll"
OWNED_FSR_MARKER = b"dxmt-owned-fsr-metalfx-v1"
FSR_FRAMEGENERATION_MODULE = "amd_fidelityfx_framegeneration_dx12.dll"
OWNED_FSR_FRAMEGENERATION_MARKER = b"dxmt-owned-fsr-framegeneration-metalfx-v1"
FSR_MODULE_MARKERS = {FSR_MODULE: OWNED_FSR_MARKER,
                      FSR_FRAMEGENERATION_MODULE: OWNED_FSR_FRAMEGENERATION_MARKER}
REQUIRED_FSR_EXPORTS = {
    "ffxConfigure": 1,
    "ffxCreateContext": 2,
    "ffxDestroyContext": 3,
    "ffxDispatch": 4,
    "ffxQuery": 5,
}
OWNED_OVERRIDES = "d3d12=b;dxgi=b;d3d11=b;d3d10core=b;winemetal=b"
REQUIRED_NGX_D3D12_EXPORTS = {
    "NVSDK_NGX_D3D12_AllocateParameters",
    "NVSDK_NGX_D3D12_CreateFeature",
    "NVSDK_NGX_D3D12_DestroyParameters",
    "NVSDK_NGX_D3D12_EvaluateFeature",
    "NVSDK_NGX_D3D12_EvaluateFeature_C",
    "NVSDK_NGX_D3D12_GetCapabilityParameters",
    "NVSDK_NGX_D3D12_GetFeatureRequirements",
    "NVSDK_NGX_D3D12_GetParameters",
    "NVSDK_NGX_D3D12_GetScratchBufferSize",
    "NVSDK_NGX_D3D12_Init",
    "NVSDK_NGX_D3D12_Init_Ext",
    "NVSDK_NGX_D3D12_Init_ProjectID",
    "NVSDK_NGX_D3D12_Init_with_ProjectID",
    "NVSDK_NGX_D3D12_ReleaseFeature",
    "NVSDK_NGX_D3D12_Shutdown",
    "NVSDK_NGX_D3D12_Shutdown1",
}
REQUIRED_NVAPI_EXPORTS = {"nvapi_QueryInterface"}
GPTK_ENV = (
    "CX_ACTIVE_GRAPHICS_BACKEND",
    "CX_APPLEGPTK_LIBD3DSHARED_PATH",
    "D3DM_MTL4",
    "D3DM_ENABLE_METALFX",
    "D3DM_SUPPORT_DXR",
    "D3DM_VENDOR_ID",
    "D3DM_DEVICE_ID",
    "D3DM_DEVICE_DESCRIPTION",
    "GPTK_SOURCE",
    "GPTK_D3D10CORE_SHA256",
    "GPTK_D3D11_SHA256",
    "GPTK_D3D12_SHA256",
    "GPTK_DXGI_SHA256",
    "GPTK_FP64_PATCHED_CONVERTER_SHA256",
)
FOREIGN_GRAPHICS_ENV = (
    "WINEDLLPATH",
    "WINEDLLPATH_PREPEND",
    "DYLD_FALLBACK_LIBRARY_PATH",
    "DYLD_PRINT_LIBRARIES",
    "DYLD_INSERT_LIBRARIES",
    "DXVK_CONFIG_FILE",
    "DXVK_STATE_CACHE_PATH",
    "VK_ICD_FILENAMES",
    "VK_DRIVER_FILES",
    "DXMT_ENABLE_NVEXT",
)
PRESERVED_GPTK_FILES = (
    Path("lib/external/D3DMetal.framework"),
    Path("lib/external/libd3dshared.dylib"),
)
SYSTEM_MACHO_PREFIXES = (
    "/System/Library/",
    "/usr/lib/",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def under(path: Path, root: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(root.resolve(strict=False))
        return True
    except ValueError:
        return False


def lexically_under(path: Path, root: Path) -> bool:
    try:
        path.absolute().relative_to(root.absolute())
        return True
    except ValueError:
        return False


def existing_symlink_component(path: Path, stop: Path | None = None) -> Path | None:
    """Return the first symlink from path upward, stopping before *stop*."""
    current = path.absolute()
    stop_abs = stop.absolute() if stop is not None else None
    while True:
        if current.exists() or current.is_symlink():
            if current.is_symlink():
                return current
        if stop_abs is not None and current == stop_abs:
            return None
        parent = current.parent
        if parent == current:
            return None
        current = parent


def validate_destination(dest: Path, wine_source: Path, dxmt_install: Path) -> None:
    if dest.exists() or dest.is_symlink():
        raise ValueError("wine-dest must be a new path and must not be a symlink")
    common = Path(os.path.commonpath((str(dest.absolute()), str(wine_source.absolute()), str(dxmt_install.absolute()))))
    if existing_symlink_component(dest.parent, common) is not None:
        raise ValueError("wine-dest parent path must not traverse a symlink")
    for label, source in (("wine-source", wine_source), ("dxmt-install", dxmt_install)):
        if under(dest, source) or under(source, dest):
            raise ValueError(f"wine-dest and {label} must not contain one another")


def regular_file(path: Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"expected regular non-symlink {label}: {path}")


def wow64_artifacts(with_wow64: bool) -> tuple[Path, ...]:
    return tuple(WOW64_WINDOWS_DIR / name for name in WINDOWS_MODULES) if with_wow64 else ()


def pe_exports(path: Path) -> dict[str, int]:
    """Read named, concrete PE exports and ordinals without external tooling."""
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(f"not a PE file: {path}")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"invalid PE signature: {path}")
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if optional + optional_size > len(data):
        raise ValueError(f"truncated PE optional header: {path}")
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic == 0x20B:
        data_directories = optional + 112
    elif magic == 0x10B:
        data_directories = optional + 96
    else:
        raise ValueError(f"unsupported PE optional-header magic 0x{magic:x}: {path}")
    if data_directories + 8 > optional + optional_size:
        raise ValueError(f"truncated PE data directories: {path}")
    export_rva, export_size = struct.unpack_from("<II", data, data_directories)
    if not export_rva or not export_size:
        return {}
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        base = section_table + index * 40
        if base + 40 > len(data):
            raise ValueError(f"truncated PE section table: {path}")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, base + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_offset(rva: int, size: int = 1) -> int:
        for virtual_address, span, raw_offset, raw_size in sections:
            if virtual_address <= rva < virtual_address + span:
                delta = rva - virtual_address
                if delta + size > raw_size:
                    break
                offset = raw_offset + delta
                if offset + size > len(data):
                    break
                return offset
        raise ValueError(f"PE RVA 0x{rva:x} is not backed by file data: {path}")

    export_offset = rva_offset(export_rva)
    if export_offset + 40 > len(data):
        raise ValueError(f"truncated PE export directory: {path}")
    ordinal_base, function_count, number_of_names, functions_rva, names_rva, ordinals_rva = struct.unpack_from("<IIIIII", data, export_offset + 16)
    if not number_of_names:
        return {}
    names_offset = rva_offset(names_rva, number_of_names * 4)
    ordinals_offset = rva_offset(ordinals_rva, number_of_names * 2)
    functions_offset = rva_offset(functions_rva, function_count * 4)
    exports = {}
    for index in range(number_of_names):
        name_rva = struct.unpack_from("<I", data, names_offset + index * 4)[0]
        name_offset = rva_offset(name_rva)
        end = data.find(b"\0", name_offset)
        if end < 0:
            raise ValueError(f"unterminated PE export name: {path}")
        try:
            name = data[name_offset:end].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError(f"invalid PE export name: {path}") from error
        index_in_functions = struct.unpack_from("<H", data, ordinals_offset + index * 2)[0]
        if index_in_functions >= function_count:
            raise ValueError(f"PE export ordinal outside function table: {path}")
        function_rva = struct.unpack_from("<I", data, functions_offset + index_in_functions * 4)[0]
        if not function_rva or export_rva <= function_rva < export_rva + export_size:
            raise ValueError(f"PE export {name} is missing or forwarded: {path}")
        rva_offset(function_rva)
        if name in exports:
            raise ValueError(f"duplicate PE export {name}: {path}")
        exports[name] = ordinal_base + index_in_functions
    return exports


def optional_modules(with_nvngx: bool, with_nvapi: bool, with_fsr: bool = False,
                     with_fsr_framegeneration: bool = False) -> tuple[str, ...]:
    modules = []
    if with_nvngx:
        modules.append("nvngx.dll")
    if with_nvapi:
        modules.append("nvapi64.dll")
    if with_fsr:
        modules.append(FSR_MODULE)
    if with_fsr_framegeneration:
        modules.append(FSR_FRAMEGENERATION_MODULE)
    return tuple(modules)


def runtime_artifacts(with_nvngx: bool = False, with_nvapi: bool = False,
                      extra: tuple[Path, ...] = (), arch: dict | None = None, *, with_fsr: bool = False,
                      with_fsr_framegeneration: bool = False) -> tuple[Path, ...]:
    arch = arch or architecture("x86_64", "x64")
    windows_dir, unix_dir = Path(arch["windows_directory"]), Path(arch["unix_directory"])
    artifacts = tuple(windows_dir / name for name in (*WINDOWS_MODULES, *optional_modules(with_nvngx, with_nvapi, with_fsr, with_fsr_framegeneration))) + (unix_dir / NATIVE_MODULE,)
    return artifacts + tuple(extra)


def owned_overrides(with_nvngx: bool = False, with_nvapi: bool = False, *, with_fsr: bool | None = False,
                    with_fsr_framegeneration: bool | None = False) -> str:
    # Unselected vendor modules must not fall through to the source Wine
    # package: its NVAPI implementation can load D3DMetal on first use.
    parts = [OWNED_OVERRIDES, "nvngx=b" if with_nvngx else "nvngx=",
             "nvapi64=b" if with_nvapi else "nvapi64="]
    # Absent fsr in retained schema 1/2 manifests denotes the immutable old
    # loader policy. Every new stage records a bool and blocks Wine's foreign FSR.
    if with_fsr is not None:
        parts.append("amd_fidelityfx_upscaler_dx12=b" if with_fsr else "amd_fidelityfx_upscaler_dx12=")
    # No FG field means a retained SR-only policy: preserve its wrapper bytes.
    if with_fsr_framegeneration is not None:
        parts.append("amd_fidelityfx_framegeneration_dx12=b" if with_fsr_framegeneration else "amd_fidelityfx_framegeneration_dx12=")
    return ";".join(parts)


def validate_fsr_module(path: Path) -> None:
    regular_file(path, "owned DXMT FSR module")
    if inspect_pe(path)["windows_abi"] != "x64":
        raise ValueError(f"FSR Windows ABI mismatch: {path}; expected x64")
    payload = path.read_bytes()
    if FSR_MODULE_MARKERS[path.name] not in payload:
        raise ValueError(f"{path.name} is missing the owned MetalFX FSR marker")
    if WINE_BUILTIN_MARKER not in payload:
        raise ValueError(f"{path.name} lacks the Wine builtin marker; winebuild --builtin did not run")
    exports = pe_exports(path)
    invalid = {name: ordinal for name, ordinal in REQUIRED_FSR_EXPORTS.items() if exports.get(name) != ordinal}
    if invalid:
        raise ValueError(f"{path.name} is missing required C ABI exports/ordinals: {invalid}")


def validate_source_runtime(wine_source: Path, arch: dict | None = None) -> None:
    arch = arch or architecture("x86_64", "x64")
    windows_dir, unix_dir = Path(arch["windows_directory"]), Path(arch["unix_directory"])
    if wine_source.is_symlink() or not wine_source.is_dir():
        raise ValueError(f"wine-source must be a real directory: {wine_source}")
    regular_file(wine_source / "bin/wine", "Wine launcher")
    for directory in (wine_source / windows_dir, wine_source / unix_dir):
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f"missing Wine runtime directory: {directory}")
    core_pe = wine_source / windows_dir / "ntdll.dll"
    regular_file(core_pe, "Wine Windows ABI image")
    if inspect_pe(core_pe)["windows_abi"] != arch["windows_abi"]:
        raise ValueError(f"Wine Windows ABI mismatch: {core_pe}")
    real = wine_source / "bin/wine.real"
    launcher = wine_source / "bin/wine"
    if launcher.read_bytes()[:2] == b"#!":
        regular_file(real, "wrapped Wine executable")
    elif real.exists() or real.is_symlink():
        raise ValueError("unwrapped wine-source already contains bin/wine.real; refusing ambiguous launcher layout")
    for image in (real if launcher.read_bytes()[:2] == b"#!" else launcher, wine_source / "bin/wineserver", wine_source / unix_dir / "ntdll.so"):
        regular_file(image, "Wine native image")
        slices = macho_architectures(image)
        # Packaged x86_64 Wine uses an arm64 IPC server on Apple Silicon.
        if arch["native_arch"] not in slices and not (image.name == "wineserver" and arch["native_arch"] == "x86_64" and "arm64" in slices):
            raise ValueError(f"Wine native image lacks {arch['native_arch']}: {image}")


def reject_removed_sdk_artifacts(dxmt_install: Path) -> None:
    """Reject a DXMT install tree that still carries the removed external DXIL compiler."""
    leftovers = sorted(str(path.relative_to(dxmt_install)) for path in dxmt_install.rglob("*")
                       if set(path.relative_to(dxmt_install).parts) & set(REMOVED_COMPILER_ARTIFACT_NAMES))
    if leftovers:
        raise ValueError(
            "DXMT install still contains artifacts of the removed external DXIL compiler: "
            + ", ".join(leftovers)
        )


def resolve_wow64_artifacts(dxmt_install32: Path) -> dict[Path, Path]:
    """Resolve a 32-bit Meson install prefix (i386-windows) into the runtime's WoW64 module directory."""
    if dxmt_install32.is_symlink() or not dxmt_install32.is_dir():
        raise ValueError(f"dxmt-install32 must be a real Meson install directory: {dxmt_install32}")
    reject_removed_sdk_artifacts(dxmt_install32)
    result: dict[Path, Path] = {}
    for name in WINDOWS_MODULES:
        source = dxmt_install32 / DXMT_WOW64_DIR / name
        regular_file(source, f"32-bit DXMT {name}")
        if inspect_pe(source)["windows_abi"] != "x86":
            raise ValueError(f"32-bit DXMT {name} is not an i386 PE image: {source}")
        if WINE_BUILTIN_MARKER not in source.read_bytes():
            raise ValueError(f"32-bit DXMT {name} lacks the Wine builtin marker; winebuild --builtin did not run")
        result[WOW64_WINDOWS_DIR / name] = source
    if OWNED_D3D12_MARKER not in (dxmt_install32 / DXMT_WOW64_DIR / "d3d12.dll").read_bytes():
        raise ValueError("32-bit d3d12.dll is missing the owned backend marker; refusing an unowned/stale DXMT build")
    return result


def resolve_dxmt_artifacts(
    dxmt_install: Path, with_nvngx: bool = False, with_nvapi: bool = False, arch: dict | None = None, *, with_fsr: bool = False
) -> dict[Path, Path]:
    arch = arch or architecture("x86_64", "x64")
    if with_fsr and (arch["native_arch"] != "x86_64" or arch["windows_abi"] != "x64"):
        raise ValueError("owned FSR frontend is only supported by the x86_64/x64 runtime")
    WINDOWS_DIR, UNIX_DIR = Path(arch["windows_directory"]), Path(arch["unix_directory"])
    DXMT_WINDOWS_DIR, DXMT_UNIX_DIR = Path(WINDOWS_DIR.name), Path(UNIX_DIR.name)
    if dxmt_install.is_symlink() or not dxmt_install.is_dir():
        raise ValueError(f"dxmt-install must be a real Meson install directory: {dxmt_install}")
    reject_removed_sdk_artifacts(dxmt_install)
    result: dict[Path, Path] = {}
    for name in WINDOWS_MODULES:
        source = dxmt_install / DXMT_WINDOWS_DIR / name
        regular_file(source, f"DXMT {name}")
        result[WINDOWS_DIR / name] = source
    unixlib = dxmt_install / DXMT_UNIX_DIR / NATIVE_MODULE
    regular_file(unixlib, f"DXMT {NATIVE_MODULE}")
    result[UNIX_DIR / NATIVE_MODULE] = unixlib
    if OWNED_D3D12_MARKER not in (dxmt_install / DXMT_WINDOWS_DIR / "d3d12.dll").read_bytes():
        raise ValueError("d3d12.dll is missing the owned backend marker; refusing an unowned/stale DXMT build")
    if with_nvngx:
        source = dxmt_install / DXMT_WINDOWS_DIR / "nvngx.dll"
        regular_file(source, "owned DXMT nvngx.dll")
        if OWNED_NGX_D3D12_MARKER not in source.read_bytes():
            raise ValueError("nvngx.dll is missing the owned D3D12 NGX marker")
        missing = REQUIRED_NGX_D3D12_EXPORTS - pe_exports(source).keys()
        if missing:
            raise ValueError(f"nvngx.dll is missing required D3D12 exports: {sorted(missing)}")
        result[WINDOWS_DIR / "nvngx.dll"] = source
    if with_nvapi:
        source = dxmt_install / DXMT_WINDOWS_DIR / "nvapi64.dll"
        regular_file(source, "owned DXMT nvapi64.dll")
        missing = REQUIRED_NVAPI_EXPORTS - pe_exports(source).keys()
        if missing:
            raise ValueError(f"nvapi64.dll is missing required exports: {sorted(missing)}")
        result[WINDOWS_DIR / "nvapi64.dll"] = source
    if with_fsr:
        for name in FSR_MODULE_MARKERS:
            source = dxmt_install / DXMT_WINDOWS_DIR / name
            validate_fsr_module(source)
            result[WINDOWS_DIR / name] = source
    for relative, source in result.items():
        if relative.suffix == ".dll" and inspect_pe(source)["windows_abi"] != arch["windows_abi"]:
            raise ValueError(f"DXMT Windows ABI mismatch: {source}; expected {arch["windows_abi"]}")
    return result


def ensure_replaceable_target(path: Path, runtime_root: Path) -> None:
    if not lexically_under(path, runtime_root):
        raise ValueError(f"replacement target escapes runtime: {path}")
    relative = path.relative_to(runtime_root)
    current = runtime_root
    for part in relative.parts:
        current = current / part
        if current.is_symlink():
            raise ValueError(f"refusing to overwrite symlink in runtime graphics path: {current}")
    if path.exists() and not path.is_file():
        raise ValueError(f"refusing to replace non-file runtime graphics target: {path}")


def macho_dependencies(path: Path) -> list[str]:
    completed = subprocess.run(["otool", "-L", str(path)], check=True, text=True, capture_output=True)
    dependencies: list[str] = []
    for line in completed.stdout.splitlines()[1:]:
        stripped = line.strip()
        # Fat binaries interleave "(architecture <arch>):" header lines and repeat
        # each slice's dependencies; collapse them into one ordered list.
        if not stripped or stripped.endswith("):"):
            continue
        dependency = stripped.split(" (compatibility version", 1)[0]
        if dependency not in dependencies:
            dependencies.append(dependency)
    return dependencies


def dependency_problems(dependencies: list[str]) -> list[str]:
    problems: list[str] = []
    basenames = {Path(dep).name for dep in dependencies}
    for required in ("winemac.so", "ntdll.so"):
        if required not in basenames:
            problems.append(f"{NATIVE_MODULE} is missing required Wine dependency {required}")
    for dep in dependencies:
        lowered = dep.lower()
        if any(token in lowered for token in FORBIDDEN_DEPENDENCY_TOKENS):
            problems.append(f"{NATIVE_MODULE} links an external compiler/backend library: {dep}")
            continue
        if dep.startswith("@") or dep.startswith(SYSTEM_MACHO_PREFIXES):
            continue
        problems.append(f"{NATIVE_MODULE} has non-relocatable external dependency: {dep}")
    return problems


def macho_tool(executable: str, *args: str) -> str:
    completed = subprocess.run([executable, *args], check=False, text=True, capture_output=True)
    if completed.returncode:
        raise ValueError(f"{executable} failed on {args[-1]}: {completed.stdout.strip()}{completed.stderr.strip()}")
    return completed.stdout


def macho_rpaths(path: Path) -> list[str]:
    rpaths: list[str] = []
    pending = False
    for line in macho_tool("otool", "-l", str(path)).splitlines():
        stripped = line.strip()
        if stripped.startswith("cmd LC_RPATH"):
            pending = True
        elif pending and stripped.startswith("path "):
            rpaths.append(stripped[len("path "):].split(" (offset", 1)[0].strip())
            pending = False
        elif stripped.startswith("cmd "):
            pending = False
    return rpaths


def macho_dependency_problems(path: Path, label: str) -> list[str]:
    """Reject build-tree absolute dependencies; an @rpath/system dep is relocatable."""
    problems: list[str] = []
    for dep in macho_dependencies(path):
        if dep.startswith("@") or dep.startswith(SYSTEM_MACHO_PREFIXES):
            continue
        problems.append(f"{label} has non-relocatable external dependency: {dep}")
    return problems


def macho_audit_problems(path: Path, label: str, native_arch: str = "x86_64") -> list[str]:
    """Reject build-tree absolute paths or a missing selected native slice."""
    problems = macho_dependency_problems(path, label)
    for rpath in macho_rpaths(path):
        if rpath.startswith(("@loader_path", "@executable_path")):
            continue
        problems.append(f"{label} has non-relocatable LC_RPATH: {rpath}")
    architectures = macho_architectures(path)
    if native_arch not in architectures:
        problems.append(f"{label} has no {native_arch} slice: {architectures}")
    return problems


def render_wrapper(with_nvngx: bool = False, with_nvapi: bool = False, arch: dict | None = None, *, with_fsr: bool | None = False,
                   with_fsr_framegeneration: bool | None = False) -> str:
    arch = arch or architecture("x86_64", "x64")
    unsets = " ".join((*GPTK_ENV, *FOREIGN_GRAPHICS_ENV))
    fg_observer = '''# Explicit test-only drawable observation at the final native launch hop.
if [ -n "${OWNED_FG_OBSERVER:-}" ]; then
  if [ ! -f "$OWNED_FG_OBSERVER" ]; then
    echo "owned graphics runtime: missing FG observer" >&2
    exit 126
  fi
  export DYLD_INSERT_LIBRARIES="$OWNED_FG_OBSERVER"
fi

''' if with_fsr_framegeneration is True else ""
    return f'''#!/bin/sh
set -eu

wrapper_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
real_wine="$wrapper_dir/wine.real"
if [ ! -x "$real_wine" ]; then
  echo "owned graphics runtime: missing $real_wine" >&2
  exit 126
fi
if [ -z "${{WINEPREFIX:-}}" ]; then
  echo "owned graphics runtime: set an explicit isolated WINEPREFIX" >&2
  exit 64
fi
wine_root=$(CDPATH= cd -- "$wrapper_dir/.." && pwd)

# Remove launcher state that selects or configures the GPTK/D3DMetal backend.
unset {unsets}

# Wine derives the same installed DLL root from ntdll.so. Pin the environment
# copy to this staged runtime too, so an inherited WINEDLLPATH cannot redirect
# builtin modules to another Wine/GPTK tree before our owned modules are found.
export WINEDLLPATH="$wine_root/{arch["windows_directory"]}:$wine_root/{arch["unix_directory"]}"

# Keep the packaged Wine runtime's non-graphics dependency lookup contract. These
# paths provide bundled libraries and GStreamer plugins, not a graphics backend.
if [ -f "$wine_root/yaagl-wine-p3-runtime.txt" ]; then
  wine_lib="$wine_root/lib"
  gst_root="$wine_lib/GStreamer.framework/Versions/1.0"
  dyld_fallback="$wine_lib"
  if [ -d "$gst_root/lib" ]; then
    dyld_fallback="$gst_root/lib:$dyld_fallback"
  fi
  export DYLD_FALLBACK_LIBRARY_PATH="$dyld_fallback"
  if [ -d "$gst_root/lib/gstreamer-1.0" ]; then
    export GST_PLUGIN_SYSTEM_PATH_1_0="$gst_root/lib/gstreamer-1.0"
  fi
  if [ -x "$gst_root/libexec/gstreamer-1.0/gst-plugin-scanner" ]; then
    export GST_PLUGIN_SCANNER="$gst_root/libexec/gstreamer-1.0/gst-plugin-scanner"
  fi
fi

# Environment load order wins over prefix registry overrides. Builtin-only means
# a missing owned module is a hard load failure rather than a native/GPTK fallback.
export WINEDLLOVERRIDES="{owned_overrides(with_nvngx, with_nvapi, with_fsr=with_fsr, with_fsr_framegeneration=with_fsr_framegeneration)}"

{"export DXMT_ENABLE_NVEXT=1" if (with_nvngx or with_nvapi) else "unset DXMT_ENABLE_NVEXT"}

# macOS strips DYLD_* inherited across the /bin/sh script boundary. Re-enable
# dyld library tracing here, at the last hop, only when the owned audit is
# explicitly requested. Normal launches also cannot inherit a tracing setting.
if [ "${{OWNED_GRAPHICS_DYLD_AUDIT:-0}}" = "1" ]; then
  export DYLD_PRINT_LIBRARIES=1
else
  unset DYLD_PRINT_LIBRARIES
fi

{fg_observer}exec {"/usr/bin/arch -arm64 " if arch["native_arch"] == "arm64" else ""}"$real_wine" "$@"
'''


def prepare_real_wine(runtime: Path, with_nvngx: bool = False, with_nvapi: bool = False, arch: dict | None = None, *, with_fsr: bool = False) -> None:
    launcher = runtime / "bin/wine"
    real = runtime / "bin/wine.real"
    if launcher.read_bytes()[:2] == b"#!":
        regular_file(real, "copied wrapped Wine executable")
    else:
        if real.exists() or real.is_symlink():
            raise ValueError("staged runtime has ambiguous bin/wine.real")
        launcher.replace(real)
    launcher.write_text(render_wrapper(with_nvngx, with_nvapi, arch, with_fsr=with_fsr, with_fsr_framegeneration=with_fsr))
    launcher.chmod(0o755)


def artifact_records(
    runtime: Path, with_nvngx: bool = False, with_nvapi: bool = False, extra: tuple[Path, ...] = (), arch: dict | None = None, *, with_fsr: bool = False
) -> dict[str, dict]:
    records: dict[str, dict] = {}
    arch = arch or architecture("x86_64", "x64")
    for relative in (*runtime_artifacts(with_nvngx, with_nvapi, extra, arch, with_fsr=with_fsr, with_fsr_framegeneration=with_fsr),
                     Path("bin/wine"), Path("bin/wine.real"), Path("bin/wineserver"),
                     Path(arch["unix_directory"]) / "ntdll.so", Path(arch["windows_directory"]) / "ntdll.dll"):
        path = runtime / relative
        regular_file(path, f"staged artifact {relative}")
        records[str(relative)] = {"sha256": sha256(path), "size": path.stat().st_size}
        if relative.suffix == ".dll":
            records[str(relative)]["pe"] = inspect_pe(path)
        elif relative != Path("bin/wine"):
            records[str(relative)]["native_architectures"] = macho_architectures(path)
    return records


def preserved_gptk_state(runtime: Path) -> list[str]:
    return [str(path) for path in PRESERVED_GPTK_FILES if (runtime / path).exists()]


def verify_runtime(runtime: Path) -> list[str]:
    problems: list[str] = []
    manifest_path = runtime / MANIFEST
    if manifest_path.is_symlink() or not manifest_path.is_file():
        return [f"missing {MANIFEST}"]
    try:
        data = json.loads(manifest_path.read_text())
    except json.JSONDecodeError as error:
        return [f"invalid {MANIFEST}: {error}"]
    try:
        arch = runtime_architecture(data)
    except ValueError as error:
        return [str(error)]
    WINDOWS_DIR, UNIX_DIR = Path(arch["windows_directory"]), Path(arch["unix_directory"])
    optional = data.get("optional_modules", {})
    with_nvngx = optional.get("nvngx") is True
    with_nvapi = optional.get("nvapi64") is True
    with_wow64 = optional.get("wow64") is True
    fsr_policy = optional.get("fsr")
    if "fsr" in optional and not isinstance(fsr_policy, bool):
        problems.append("manifest optional FSR selection must be a boolean")
    with_fsr = fsr_policy is True
    fg_policy = optional.get("fsr_framegeneration")
    if "fsr_framegeneration" in optional and not isinstance(fg_policy, bool):
        problems.append("manifest optional FSR frame-generation selection must be a boolean")
    with_fsr_framegeneration = fg_policy is True
    if with_fsr_framegeneration and not with_fsr:
        problems.append("owned FSR frame generation requires the owned FSR module group")
    if (with_fsr or with_fsr_framegeneration) and (arch["native_arch"] != "x86_64" or arch["windows_abi"] != "x64"):
        problems.append("owned FSR frontend is only supported by the x86_64/x64 runtime")
    native_record = data.get("native_compiler")
    if "dxil_compiler" in data:
        problems.append("manifest still records the removed external DXIL compiler")
    if not isinstance(native_record, dict) or native_record.get("ownership") != "self-owned":
        problems.append("manifest does not record the self-owned native compiler")
    elif native_record.get("path") != str(UNIX_DIR / NATIVE_MODULE):
        problems.append("manifest native compiler path is not the owned winemetal module")
    elif native_record.get("external_dependencies"):
        problems.append("manifest records external native compiler dependencies")
    for original in FORBIDDEN_STAGED_ARTIFACTS:
        relative = UNIX_DIR / original.relative_to(Path("lib/wine/x86_64-unix"))
        leftover = runtime / relative
        if leftover.exists() or leftover.is_symlink():
            problems.append(f"staged runtime still contains a removed external DXIL compiler artifact: {relative}")
    if data.get("loader", {}).get("winedlloverrides") != owned_overrides(with_nvngx, with_nvapi, with_fsr=fsr_policy, with_fsr_framegeneration=fg_policy):
        problems.append("manifest loader override does not select the owned builtin modules exactly")
    if data.get("gptk_backend", {}).get("selected") is not False:
        problems.append("manifest does not record the GPTK backend as unselected")
    recorded = data.get("artifacts")
    if not isinstance(recorded, dict):
        problems.append("manifest artifact inventory is missing")
        return problems
    if with_wow64 and arch["native_arch"] != "x86_64":
        problems.append("WoW64 staging is only established for the x86_64 Wine layout")
    expected = {str(path) for path in (*runtime_artifacts(with_nvngx, with_nvapi, wow64_artifacts(with_wow64), arch, with_fsr=with_fsr, with_fsr_framegeneration=with_fsr_framegeneration),
                                       Path("bin/wine"), Path("bin/wine.real"))}
    if data["schema"] == 2:
        expected.update(("bin/wineserver", str(UNIX_DIR / "ntdll.so"), str(WINDOWS_DIR / "ntdll.dll")))
    if set(recorded) != expected:
        problems.append("manifest artifact inventory is incomplete or contains unknown entries")
    for relative in sorted(expected & set(recorded)):
        path = runtime / relative
        record = recorded[relative]
        if not isinstance(record, dict):
            problems.append(f"invalid artifact record: {relative}")
            continue
        try:
            regular_file(path, f"runtime artifact {relative}")
            actual = sha256(path)
        except (OSError, ValueError) as error:
            problems.append(str(error))
            continue
        if record.get("sha256") != actual:
            problems.append(f"runtime artifact hash changed: {relative}")
        if data["schema"] == 2:
            try:
                if path.suffix == ".dll" and record.get("pe") != inspect_pe(path):
                    problems.append(f"runtime artifact PE metadata mismatch: {relative}")
                elif path.suffix != ".dll" and relative != "bin/wine" and record.get("native_architectures") != macho_architectures(path):
                    problems.append(f"runtime artifact native metadata mismatch: {relative}")
            except (OSError, ValueError) as error:
                problems.append(str(error))
    wrapper = runtime / "bin/wine"
    if wrapper.is_file() and not wrapper.is_symlink() and wrapper.read_text() != render_wrapper(with_nvngx, with_nvapi, arch, with_fsr=fsr_policy, with_fsr_framegeneration=fg_policy):
        problems.append("bin/wine no longer matches the owned launch policy")
    d3d12 = runtime / WINDOWS_DIR / "d3d12.dll"
    if d3d12.is_file() and OWNED_D3D12_MARKER not in d3d12.read_bytes():
        problems.append("staged d3d12.dll no longer carries the owned backend marker")
    for name in WINDOWS_MODULES:
        module = runtime / WINDOWS_DIR / name
        if module.is_file() and not module.is_symlink() and WINE_BUILTIN_MARKER not in module.read_bytes():
            problems.append(f"staged {name} lacks the Wine builtin marker; winebuild --builtin postprocess did not run")
    for name in (*WINDOWS_MODULES, *optional_modules(with_nvngx, with_nvapi, with_fsr, with_fsr_framegeneration), "ntdll.dll"):
        module = runtime / WINDOWS_DIR / name
        try:
            if inspect_pe(module)["windows_abi"] != arch["windows_abi"]:
                problems.append(f"staged {name} Windows ABI mismatch")
        except (OSError, ValueError) as error:
            problems.append(str(error))
    for image in (runtime / "bin/wine.real", runtime / "bin/wineserver", runtime / UNIX_DIR / "ntdll.so"):
        try:
            regular_file(image, "Wine native image")
            slices = macho_architectures(image)
            if arch["native_arch"] not in slices and not (image.name == "wineserver" and arch["native_arch"] == "x86_64" and "arm64" in slices):
                problems.append(f"Wine native image lacks {arch['native_arch']}: {image}")
        except (OSError, ValueError) as error:
            problems.append(str(error))
    for relative in wow64_artifacts(with_wow64):
        module = runtime / relative
        if not module.is_file() or module.is_symlink():
            continue
        try:
            if inspect_pe(module)["windows_abi"] != "x86":
                problems.append(f"staged {relative} is not an i386 PE image")
        except (OSError, ValueError) as error:
            problems.append(str(error))
        if WINE_BUILTIN_MARKER not in module.read_bytes():
            problems.append(f"staged {relative} lacks the Wine builtin marker; winebuild --builtin postprocess did not run")
    if with_wow64:
        d3d12_32 = runtime / WOW64_WINDOWS_DIR / "d3d12.dll"
        if d3d12_32.is_file() and OWNED_D3D12_MARKER not in d3d12_32.read_bytes():
            problems.append("staged 32-bit d3d12.dll no longer carries the owned backend marker")
    if with_nvngx:
        nvngx = runtime / WINDOWS_DIR / "nvngx.dll"
        if nvngx.is_file() and not nvngx.is_symlink():
            if OWNED_NGX_D3D12_MARKER not in nvngx.read_bytes():
                problems.append("staged nvngx.dll no longer carries the owned D3D12 NGX marker")
            try:
                missing = REQUIRED_NGX_D3D12_EXPORTS - pe_exports(nvngx).keys()
            except (OSError, ValueError) as error:
                problems.append(f"cannot inspect staged nvngx.dll exports: {error}")
            else:
                if missing:
                    problems.append(f"staged nvngx.dll is missing D3D12 exports: {sorted(missing)}")
    if with_nvapi:
        nvapi = runtime / WINDOWS_DIR / "nvapi64.dll"
        if nvapi.is_file() and not nvapi.is_symlink():
            try:
                missing = REQUIRED_NVAPI_EXPORTS - pe_exports(nvapi).keys()
            except (OSError, ValueError) as error:
                problems.append(f"cannot inspect staged nvapi64.dll exports: {error}")
            else:
                if missing:
                    problems.append(f"staged nvapi64.dll is missing exports: {sorted(missing)}")
    for name in optional_modules(False, False, with_fsr, with_fsr_framegeneration):
        try:
            validate_fsr_module(runtime / WINDOWS_DIR / name)
        except (OSError, ValueError) as error:
            problems.append(str(error))
    winemetal = runtime / UNIX_DIR / NATIVE_MODULE
    if winemetal.is_file() and not winemetal.is_symlink():
        try:
            dependencies = macho_dependencies(winemetal)
            relocation = macho_audit_problems(winemetal, NATIVE_MODULE, arch["native_arch"])
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            problems.append(f"cannot inspect {NATIVE_MODULE} dependencies: {error}")
        else:
            problems.extend(dependency_problems(dependencies))
            problems.extend(relocation)
            if dependencies != data.get("winemetal_dependencies"):
                problems.append(f"{NATIVE_MODULE} dependency list changed since staging")
    return problems


def stage(
    dxmt_install: Path, wine_source: Path, dest: Path, with_nvngx: bool = False, with_nvapi: bool = False,
    dxmt_install32: Path | None = None,
    native_arch: str = "x86_64", windows_abi: str = "x64",
    *, with_fsr: bool = False,
) -> None:
    arch = architecture(native_arch, windows_abi)
    if with_fsr and (native_arch != "x86_64" or windows_abi != "x64"):
        raise ValueError("owned FSR frontend is only supported by the x86_64/x64 runtime")
    UNIX_DIR = Path(arch["unix_directory"])
    if dxmt_install32 is not None and native_arch != "x86_64":
        raise ValueError("WoW64 staging is only established for the x86_64 Wine layout")
    validate_destination(dest, wine_source, dxmt_install)
    if dxmt_install32 is not None:
        validate_destination(dest, wine_source, dxmt_install32)
    validate_source_runtime(wine_source, arch)
    with_wow64 = dxmt_install32 is not None
    if with_wow64:
        wow64_dir = wine_source / WOW64_WINDOWS_DIR
        if wow64_dir.is_symlink() or not wow64_dir.is_dir():
            raise ValueError(f"missing Wine WoW64 runtime directory: {wow64_dir}")
    dxmt_artifacts = resolve_dxmt_artifacts(dxmt_install, with_nvngx, with_nvapi, arch, with_fsr=with_fsr)
    if with_wow64:
        dxmt_artifacts.update(resolve_wow64_artifacts(dxmt_install32))
    winemetal_source = dxmt_artifacts[UNIX_DIR / NATIVE_MODULE]
    dependencies = macho_dependencies(winemetal_source)
    problems = dependency_problems(dependencies)
    problems.extend(macho_audit_problems(winemetal_source, NATIVE_MODULE, native_arch))
    if problems:
        raise ValueError("; ".join(problems))

    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.parent / f"{dest.name}.staging-{uuid.uuid4().hex}"
    try:
        shutil.copytree(wine_source, temporary, symlinks=True)
        prepare_real_wine(temporary, with_nvngx, with_nvapi, arch, with_fsr=with_fsr)
        for relative, source in dxmt_artifacts.items():
            target = temporary / relative
            ensure_replaceable_target(target, temporary)
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                target.unlink()
            shutil.copy2(source, target)
        records = artifact_records(temporary, with_nvngx, with_nvapi, wow64_artifacts(with_wow64), arch, with_fsr=with_fsr)
        manifest = {
            "schema": SCHEMA,
            "architecture": arch,
            "implementation": "owned-dxmt-derived",
            "source": {
                "dxmt_install": str(dxmt_install),
                "dxmt_install32": str(dxmt_install32) if with_wow64 else None,
                "wine_source": str(wine_source),
            },
            "loader": {
                "winedlloverrides": owned_overrides(with_nvngx, with_nvapi, with_fsr=with_fsr, with_fsr_framegeneration=with_fsr),
                "explicit_wineprefix_required": True,
            },
            "optional_modules": {
                "nvngx": with_nvngx,
                "nvapi64": with_nvapi,
                "fsr": with_fsr,
                "fsr_framegeneration": with_fsr,
                "wow64": with_wow64,
            },
            "artifacts": records,
            "winemetal_dependencies": dependencies,
            "native_compiler": {
                "ownership": "self-owned",
                "path": str(UNIX_DIR / NATIVE_MODULE),
                "sha256": sha256(winemetal_source),
                "integration": "the DXIL compiler is compiled from the owned patch series into airconv and linked into this module",
                "external_dependencies": [],
            },
            "airconv_runtime": "statically linked into winemetal.so; no separate runtime executable",
            "gptk_backend": {
                "selected": False,
                "preserved_files": preserved_gptk_state(temporary),
                "preservation_reason": "the Wine runtime may contain unrelated consumers; selection is blocked by the owned wrapper",
            },
        }
        (temporary / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n")
        verify_problems = verify_runtime(temporary)
        if verify_problems:
            raise RuntimeError("staged runtime verification failed: " + "; ".join(verify_problems))
        os.rename(temporary, dest)
    except BaseException:
        if temporary.exists():
            shutil.rmtree(temporary)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    stage_parser = subparsers.add_parser("stage", help="create a new isolated owned-graphics Wine runtime")
    stage_parser.add_argument("--dxmt-install", required=True, type=Path, help="Meson install prefix matching the selected Windows/native layout")
    stage_parser.add_argument("--native-arch", choices=("x86_64", "arm64"), default="x86_64")
    stage_parser.add_argument("--windows-abi", choices=("x64", "arm64", "arm64ec", "arm64x"), default="x64")
    stage_parser.add_argument("--dxmt-install32", type=Path, help="optional 32-bit Meson install prefix containing i386-windows; stages WoW64 modules into lib/wine/i386-windows")
    stage_parser.add_argument("--wine-source", required=True, type=Path, help="untouched Wine runtime to copy")
    stage_parser.add_argument("--wine-dest", required=True, type=Path, help="new destination; must not already exist")
    stage_parser.add_argument("--with-nvngx", action="store_true", help="require and stage owned nvngx.dll only if D3D12 exports/marker validate")
    stage_parser.add_argument("--with-nvapi", action="store_true", help="require and stage owned nvapi64.dll only if its QueryInterface export validates")
    stage_parser.add_argument("--with-fsr", action="store_true", help="require and stage owned x64 MetalFX FSR upscaling and frame-generation DLLs, each with builtin marker and five C ABI exports/ordinals")
    verify_parser = subparsers.add_parser("verify", help="verify hashes, launcher policy and Mach-O dependencies")
    verify_parser.add_argument("--runtime", required=True, type=Path)
    args = parser.parse_args()

    if args.command == "verify":
        runtime = args.runtime.expanduser().resolve()
        problems = verify_runtime(runtime)
        if problems:
            for problem in problems:
                print(f"FAIL: {problem}")
            return 1
        print(f"PASS: {runtime} matches {MANIFEST}; owned builtin load policy and winemetal dependencies are intact")
        return 0

    dxmt_install = args.dxmt_install.expanduser().resolve()
    wine_source = args.wine_source.expanduser().resolve()
    dest = args.wine_dest.expanduser().absolute()
    dxmt_install32 = args.dxmt_install32.expanduser().resolve() if args.dxmt_install32 else None
    stage(dxmt_install, wine_source, dest, args.with_nvngx, args.with_nvapi, dxmt_install32, args.native_arch, args.windows_abi, with_fsr=args.with_fsr)
    print(f"PRIVATE owned-graphics runtime staged: {dest}")
    print("Use it only with an explicit isolated WINEPREFIX; the wrapper forces owned builtin graphics modules.")
    print(f"Verify later with: {Path(__file__).name} verify --runtime {dest}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
