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


SCHEMA = 1
MANIFEST = "owned-graphics-runtime.json"
WINDOWS_DIR = Path("lib/wine/x86_64-windows")
UNIX_DIR = Path("lib/wine/x86_64-unix")
DXMT_WINDOWS_DIR = Path("x86_64-windows")
DXMT_UNIX_DIR = Path("x86_64-unix")
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


def pe_exports(path: Path) -> set[str]:
    """Read PE export names without depending on external objdump tooling."""
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
    export_rva, export_size = struct.unpack_from("<II", data, data_directories)
    if not export_rva or not export_size:
        return set()
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        base = section_table + index * 40
        if base + 40 > len(data):
            raise ValueError(f"truncated PE section table: {path}")
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, base + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_offset(rva: int) -> int:
        for virtual_address, span, raw_offset, raw_size in sections:
            if virtual_address <= rva < virtual_address + span:
                delta = rva - virtual_address
                if delta >= raw_size:
                    break
                offset = raw_offset + delta
                if offset >= len(data):
                    break
                return offset
        raise ValueError(f"PE RVA 0x{rva:x} is not backed by file data: {path}")

    export_offset = rva_offset(export_rva)
    if export_offset + 40 > len(data):
        raise ValueError(f"truncated PE export directory: {path}")
    number_of_names = struct.unpack_from("<I", data, export_offset + 24)[0]
    names_rva = struct.unpack_from("<I", data, export_offset + 32)[0]
    names_offset = rva_offset(names_rva)
    exports = set()
    for index in range(number_of_names):
        name_rva = struct.unpack_from("<I", data, names_offset + index * 4)[0]
        name_offset = rva_offset(name_rva)
        end = data.find(b"\0", name_offset)
        if end < 0:
            raise ValueError(f"unterminated PE export name: {path}")
        exports.add(data[name_offset:end].decode("ascii"))
    return exports


def optional_modules(with_nvngx: bool, with_nvapi: bool) -> tuple[str, ...]:
    modules = []
    if with_nvngx:
        modules.append("nvngx.dll")
    if with_nvapi:
        modules.append("nvapi64.dll")
    return tuple(modules)


def runtime_artifacts(with_nvngx: bool = False, with_nvapi: bool = False,
                      extra: tuple[Path, ...] = ()) -> tuple[Path, ...]:
    artifacts = TRANSLATOR_ARTIFACTS + tuple(WINDOWS_DIR / name for name in optional_modules(with_nvngx, with_nvapi))
    return artifacts + tuple(extra)


def owned_overrides(with_nvngx: bool = False, with_nvapi: bool = False) -> str:
    # Unselected vendor modules must not fall through to the source Wine
    # package: its NVAPI implementation can load D3DMetal on first use.
    parts = [OWNED_OVERRIDES, "nvngx=b" if with_nvngx else "nvngx=",
             "nvapi64=b" if with_nvapi else "nvapi64="]
    return ";".join(parts)


def validate_source_runtime(wine_source: Path) -> None:
    if wine_source.is_symlink() or not wine_source.is_dir():
        raise ValueError(f"wine-source must be a real directory: {wine_source}")
    regular_file(wine_source / "bin/wine", "Wine launcher")
    for directory in (wine_source / WINDOWS_DIR, wine_source / UNIX_DIR):
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError(f"missing Wine runtime directory: {directory}")
    real = wine_source / "bin/wine.real"
    launcher = wine_source / "bin/wine"
    if launcher.read_bytes()[:2] == b"#!":
        regular_file(real, "wrapped Wine executable")
    elif real.exists() or real.is_symlink():
        raise ValueError("unwrapped wine-source already contains bin/wine.real; refusing ambiguous launcher layout")


def reject_removed_sdk_artifacts(dxmt_install: Path) -> None:
    """Reject a DXMT install tree that still carries the removed external DXIL compiler."""
    leftovers = sorted(str(path.relative_to(dxmt_install)) for path in dxmt_install.rglob("*")
                       if set(path.relative_to(dxmt_install).parts) & set(REMOVED_COMPILER_ARTIFACT_NAMES))
    if leftovers:
        raise ValueError(
            "DXMT install still contains artifacts of the removed external DXIL compiler: "
            + ", ".join(leftovers)
        )


def resolve_dxmt_artifacts(
    dxmt_install: Path, with_nvngx: bool = False, with_nvapi: bool = False
) -> dict[Path, Path]:
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
        missing = REQUIRED_NGX_D3D12_EXPORTS - pe_exports(source)
        if missing:
            raise ValueError(f"nvngx.dll is missing required D3D12 exports: {sorted(missing)}")
        result[WINDOWS_DIR / "nvngx.dll"] = source
    if with_nvapi:
        source = dxmt_install / DXMT_WINDOWS_DIR / "nvapi64.dll"
        regular_file(source, "owned DXMT nvapi64.dll")
        missing = REQUIRED_NVAPI_EXPORTS - pe_exports(source)
        if missing:
            raise ValueError(f"nvapi64.dll is missing required exports: {sorted(missing)}")
        result[WINDOWS_DIR / "nvapi64.dll"] = source
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


def macho_architectures(path: Path) -> list[str]:
    return macho_tool("lipo", "-archs", str(path)).split()


def macho_dependency_problems(path: Path, label: str) -> list[str]:
    """Reject build-tree absolute dependencies; an @rpath/system dep is relocatable."""
    problems: list[str] = []
    for dep in macho_dependencies(path):
        if dep.startswith("@") or dep.startswith(SYSTEM_MACHO_PREFIXES):
            continue
        problems.append(f"{label} has non-relocatable external dependency: {dep}")
    return problems


def macho_audit_problems(path: Path, label: str) -> list[str]:
    """Reject build-tree absolute paths or a missing x86_64 slice."""
    problems = macho_dependency_problems(path, label)
    for rpath in macho_rpaths(path):
        if rpath.startswith(("@loader_path", "@executable_path")):
            continue
        problems.append(f"{label} has non-relocatable LC_RPATH: {rpath}")
    architectures = macho_architectures(path)
    if "x86_64" not in architectures:
        problems.append(f"{label} has no x86_64 slice: {architectures}")
    return problems


def render_wrapper(with_nvngx: bool = False, with_nvapi: bool = False) -> str:
    unsets = " ".join((*GPTK_ENV, *FOREIGN_GRAPHICS_ENV))
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
export WINEDLLPATH="$wine_root/lib/wine/x86_64-windows:$wine_root/lib/wine/x86_64-unix"

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
export WINEDLLOVERRIDES="{owned_overrides(with_nvngx, with_nvapi)}"

{"export DXMT_ENABLE_NVEXT=1" if (with_nvngx or with_nvapi) else "unset DXMT_ENABLE_NVEXT"}

# macOS strips DYLD_* inherited across the /bin/sh script boundary. Re-enable
# dyld library tracing here, at the last hop, only when the owned audit is
# explicitly requested. Normal launches also cannot inherit a tracing setting.
if [ "${{OWNED_GRAPHICS_DYLD_AUDIT:-0}}" = "1" ]; then
  export DYLD_PRINT_LIBRARIES=1
else
  unset DYLD_PRINT_LIBRARIES
fi

exec "$real_wine" "$@"
'''


def prepare_real_wine(runtime: Path, with_nvngx: bool = False, with_nvapi: bool = False) -> None:
    launcher = runtime / "bin/wine"
    real = runtime / "bin/wine.real"
    if launcher.read_bytes()[:2] == b"#!":
        regular_file(real, "copied wrapped Wine executable")
    else:
        if real.exists() or real.is_symlink():
            raise ValueError("staged runtime has ambiguous bin/wine.real")
        launcher.replace(real)
    launcher.write_text(render_wrapper(with_nvngx, with_nvapi))
    launcher.chmod(0o755)


def artifact_records(
    runtime: Path, with_nvngx: bool = False, with_nvapi: bool = False, extra: tuple[Path, ...] = ()
) -> dict[str, dict[str, str | int]]:
    records: dict[str, dict[str, str | int]] = {}
    for relative in (*runtime_artifacts(with_nvngx, with_nvapi, extra),
                     Path("bin/wine"), Path("bin/wine.real")):
        path = runtime / relative
        regular_file(path, f"staged artifact {relative}")
        records[str(relative)] = {"sha256": sha256(path), "size": path.stat().st_size}
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
    if data.get("schema") != SCHEMA:
        problems.append(f"manifest schema is {data.get('schema')!r}, expected {SCHEMA}")
    optional = data.get("optional_modules", {})
    with_nvngx = optional.get("nvngx") is True
    with_nvapi = optional.get("nvapi64") is True
    native_record = data.get("native_compiler")
    if "dxil_compiler" in data:
        problems.append("manifest still records the removed external DXIL compiler")
    if not isinstance(native_record, dict) or native_record.get("ownership") != "self-owned":
        problems.append("manifest does not record the self-owned native compiler")
    elif native_record.get("path") != str(UNIX_DIR / NATIVE_MODULE):
        problems.append("manifest native compiler path is not the owned winemetal module")
    elif native_record.get("external_dependencies"):
        problems.append("manifest records external native compiler dependencies")
    for relative in FORBIDDEN_STAGED_ARTIFACTS:
        leftover = runtime / relative
        if leftover.exists() or leftover.is_symlink():
            problems.append(f"staged runtime still contains a removed external DXIL compiler artifact: {relative}")
    if data.get("loader", {}).get("winedlloverrides") != owned_overrides(with_nvngx, with_nvapi):
        problems.append("manifest loader override does not select the owned builtin modules exactly")
    if data.get("gptk_backend", {}).get("selected") is not False:
        problems.append("manifest does not record the GPTK backend as unselected")
    recorded = data.get("artifacts")
    if not isinstance(recorded, dict):
        problems.append("manifest artifact inventory is missing")
        return problems
    expected = {str(path) for path in (*runtime_artifacts(with_nvngx, with_nvapi),
                                       Path("bin/wine"), Path("bin/wine.real"))}
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
    wrapper = runtime / "bin/wine"
    if wrapper.is_file() and not wrapper.is_symlink() and wrapper.read_text() != render_wrapper(with_nvngx, with_nvapi):
        problems.append("bin/wine no longer matches the owned launch policy")
    d3d12 = runtime / WINDOWS_DIR / "d3d12.dll"
    if d3d12.is_file() and OWNED_D3D12_MARKER not in d3d12.read_bytes():
        problems.append("staged d3d12.dll no longer carries the owned backend marker")
    for name in WINDOWS_MODULES:
        module = runtime / WINDOWS_DIR / name
        if module.is_file() and not module.is_symlink() and WINE_BUILTIN_MARKER not in module.read_bytes():
            problems.append(f"staged {name} lacks the Wine builtin marker; winebuild --builtin postprocess did not run")
    if with_nvngx:
        nvngx = runtime / WINDOWS_DIR / "nvngx.dll"
        if nvngx.is_file() and not nvngx.is_symlink():
            if OWNED_NGX_D3D12_MARKER not in nvngx.read_bytes():
                problems.append("staged nvngx.dll no longer carries the owned D3D12 NGX marker")
            try:
                missing = REQUIRED_NGX_D3D12_EXPORTS - pe_exports(nvngx)
            except (OSError, ValueError) as error:
                problems.append(f"cannot inspect staged nvngx.dll exports: {error}")
            else:
                if missing:
                    problems.append(f"staged nvngx.dll is missing D3D12 exports: {sorted(missing)}")
    if with_nvapi:
        nvapi = runtime / WINDOWS_DIR / "nvapi64.dll"
        if nvapi.is_file() and not nvapi.is_symlink():
            try:
                missing = REQUIRED_NVAPI_EXPORTS - pe_exports(nvapi)
            except (OSError, ValueError) as error:
                problems.append(f"cannot inspect staged nvapi64.dll exports: {error}")
            else:
                if missing:
                    problems.append(f"staged nvapi64.dll is missing exports: {sorted(missing)}")
    winemetal = runtime / UNIX_DIR / NATIVE_MODULE
    if winemetal.is_file() and not winemetal.is_symlink():
        try:
            dependencies = macho_dependencies(winemetal)
            relocation = macho_audit_problems(winemetal, NATIVE_MODULE)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            problems.append(f"cannot inspect {NATIVE_MODULE} dependencies: {error}")
        else:
            problems.extend(dependency_problems(dependencies))
            problems.extend(relocation)
            if dependencies != data.get("winemetal_dependencies"):
                problems.append(f"{NATIVE_MODULE} dependency list changed since staging")
    return problems


def stage(
    dxmt_install: Path, wine_source: Path, dest: Path, with_nvngx: bool = False, with_nvapi: bool = False
) -> None:
    validate_destination(dest, wine_source, dxmt_install)
    validate_source_runtime(wine_source)
    dxmt_artifacts = resolve_dxmt_artifacts(dxmt_install, with_nvngx, with_nvapi)
    winemetal_source = dxmt_artifacts[UNIX_DIR / NATIVE_MODULE]
    dependencies = macho_dependencies(winemetal_source)
    problems = dependency_problems(dependencies)
    problems.extend(macho_audit_problems(winemetal_source, NATIVE_MODULE))
    if problems:
        raise ValueError("; ".join(problems))

    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.parent / f"{dest.name}.staging-{uuid.uuid4().hex}"
    try:
        shutil.copytree(wine_source, temporary, symlinks=True)
        prepare_real_wine(temporary, with_nvngx, with_nvapi)
        for relative, source in dxmt_artifacts.items():
            target = temporary / relative
            ensure_replaceable_target(target, temporary)
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                target.unlink()
            shutil.copy2(source, target)
        records = artifact_records(temporary, with_nvngx, with_nvapi)
        manifest = {
            "schema": SCHEMA,
            "implementation": "owned-dxmt-derived",
            "source": {
                "dxmt_install": str(dxmt_install),
                "wine_source": str(wine_source),
            },
            "loader": {
                "winedlloverrides": owned_overrides(with_nvngx, with_nvapi),
                "explicit_wineprefix_required": True,
            },
            "optional_modules": {
                "nvngx": with_nvngx,
                "nvapi64": with_nvapi,
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
    stage_parser.add_argument("--dxmt-install", required=True, type=Path, help="Meson install prefix containing x86_64-windows and x86_64-unix")
    stage_parser.add_argument("--wine-source", required=True, type=Path, help="untouched Wine runtime to copy")
    stage_parser.add_argument("--wine-dest", required=True, type=Path, help="new destination; must not already exist")
    stage_parser.add_argument("--with-nvngx", action="store_true", help="require and stage owned nvngx.dll only if D3D12 exports/marker validate")
    stage_parser.add_argument("--with-nvapi", action="store_true", help="require and stage owned nvapi64.dll only if its QueryInterface export validates")
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
    stage(dxmt_install, wine_source, dest, args.with_nvngx, args.with_nvapi)
    print(f"PRIVATE owned-graphics runtime staged: {dest}")
    print("Use it only with an explicit isolated WINEPREFIX; the wrapper forces owned builtin graphics modules.")
    print(f"Verify later with: {Path(__file__).name} verify --runtime {dest}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
