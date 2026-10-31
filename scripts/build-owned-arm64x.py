#!/usr/bin/env python3
"""Build fresh ARM64 + ARM64EC inputs and paired ARM64X boundary/Core DLLs.

The two unmodified Meson graphs retain compiler/library/link checks. ARM64X is
only the final paired DLL link, never a single-EC Meson executable probe. Wine
SDK inputs are validated from a completed external Wine SDK; no guest is run.
Core is an ordinary native PE; only the boundary DLLs are Wine builtins.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import uuid

from owned_graphics_arch import inspect_pe, macho_architectures

MODULES = {
    "winemetal": "src/winemetal/winemetal.dll",
    "dxgi": "src/dxgi/dxgi.dll",
    "d3d11": "src/d3d11/d3d11.dll",
    "d3d10core": "src/d3d10/d3d10core.dll",
    "d3d12": "src/d3d12/d3d12.dll",
    "d3d12core": "src/d3d12/d3d12core.dll",
}
OPTIONAL_MODULES = {
    "nvapi": {"nvapi64": "src/nvapi/nvapi64.dll"},
    "nvngx": {"nvngx": "src/nvngx/nvngx.dll"},
    "fsr": {
        "amd_fidelityfx_upscaler_dx12": "src/fsr/amd_fidelityfx_upscaler_dx12.dll",
        "amd_fidelityfx_framegeneration_dx12": "src/fsrfg/amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_denoiser_dx12": "src/fsrrr/amd_fidelityfx_denoiser_dx12.dll",
    },
}


def build_selection(args: argparse.Namespace) -> tuple[dict[str, str], dict[str, bool]]:
    modules = dict(MODULES)
    options = {}
    for frontend, additions in OPTIONAL_MODULES.items():
        enabled = getattr(args, "enable_" + frontend)
        options["enable_" + frontend] = enabled
        if enabled:
            modules.update(additions)
    return modules, options


def complete_tls_fixups(image: Path, link_map: Path) -> None:
    """Supply LLVM 22's missing ARM64X TLS-directory view fixup."""
    inspect_pe(image)
    data = bytearray(image.read_bytes())
    pe = struct.unpack_from("<I", data, 60)[0]
    directories = pe + 136
    tls_header = directories + 9 * 8
    native_tls, tls_size = struct.unpack_from("<II", data, tls_header)
    if not native_tls and not tls_size:
        return
    if not native_tls or tls_size != 40:
        raise ValueError(f"unexpected paired TLS directory: {image}")
    tls_rvas = {int(rva, 16) for rva in re.findall(
        r"^([0-9a-fA-F]+)\s+00000000\s+0\s+_tls_used$", link_map.read_text(), re.MULTILINE)}
    if len(tls_rvas) != 2 or native_tls not in tls_rvas:
        raise ValueError(f"expected distinct native/EC TLS directories: {image}")
    ec_tls = next(rva for rva in tls_rvas if rva != native_tls)
    sections = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    layout = [struct.unpack_from("<IIII", data, sections + index * 40 + 8)
              for index in range(struct.unpack_from("<H", data, pe + 6)[0])]

    def offset(rva: int, size: int) -> int:
        for _, start, raw_size, raw in layout:
            if start <= rva and rva + size <= start + raw_size:
                return raw + rva - start
        raise ValueError(f"unbacked paired TLS/relocation RVA 0x{rva:x}: {image}")

    offset(ec_tls, tls_size)
    load_rva, _ = struct.unpack_from("<II", data, directories + 10 * 8)
    load = offset(load_rva, 232)
    table_offset, section_index = struct.unpack_from("<IH", data, load + 224)
    virtual_size, _, raw_size, raw = layout[section_index - 1]
    table = raw + table_offset
    version, table_size = struct.unpack_from("<II", data, table)
    symbol, fixup_size = struct.unpack_from("<QI", data, table + 8)
    # The pinned linker places its sole ARM64X record last in .reloc. Extend
    # that record, not a second one: Wine applies only the first ARM64X record.
    if (version != 1 or symbol != 6 or table_size != 12 + fixup_size
            or table_offset + 8 + table_size != virtual_size
            or virtual_size + 20 > raw_size
            or any(data[raw + virtual_size:raw + virtual_size + 20])):
        raise ValueError(f"unexpected pinned-linker ARM64X table layout: {image}")
    block = struct.pack("<IIHIHI", tls_header & ~0xfff, 20,
                        0x9000 | (tls_header & 0xfff), ec_tls,
                        0x9000 | ((tls_header + 4) & 0xfff), tls_size)
    data[raw + virtual_size:raw + virtual_size + len(block)] = block
    struct.pack_into("<I", data, table + 4, table_size + len(block))
    struct.pack_into("<I", data, table + 16, fixup_size + len(block))
    struct.pack_into("<I", data, sections + (section_index - 1) * 40 + 8, virtual_size + len(block))
    image.write_bytes(data)


def audit_image(image: Path, dump: str, expected_exports: dict[str, dict[str, int]],
                *, builtin: bool = True, data_exports: tuple[str, ...] = ()) -> dict:
    facts = inspect_pe(image)
    architectures = {row["architecture"] for row in facts["code_ranges"]}
    data = image.read_bytes()
    pe = struct.unpack_from("<I", data, 60)[0]
    entry = struct.unpack_from("<I", data, pe + 40)[0]
    characteristics = struct.unpack_from("<H", data, pe + 22)[0]
    if (facts["windows_abi"] != "arm64x" or not {"arm64", "arm64ec"} <= architectures
            or facts["machine"] != 0xAA64 or not facts["arm64x_relocations"]
            or not entry or not characteristics & 0x2000):
        raise ValueError(f"not a paired production DLL with a real entry: {image}: {facts}")
    if not any(row["rva"] <= entry < row["rva"] + row["size"]
               and row["architecture"] == "arm64" for row in facts["code_ranges"]):
        raise ValueError(f"native entry is outside the ARM64 code map: {image}")
    if (b"Wine builtin DLL" in data) != builtin or "HybridObject {" not in dump:
        raise ValueError(f"wrong builtin delivery or missing relocated hybrid view: {image}")
    data_ranges = []
    section_layout = []
    sections = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    for index in range(struct.unpack_from("<H", data, pe + 6)[0]):
        section = sections + index * 40
        rva, rawsize = struct.unpack_from("<II", data, section + 12)
        flags = struct.unpack_from("<I", data, section + 36)[0]
        raw = struct.unpack_from("<I", data, section + 20)[0]
        section_layout.append((rva, rawsize, raw))
        if flags & 0x40 and not flags & 0x20000000:
            data_ranges.append((rva, rawsize))
    # llvm-readobj materializes the actual ARM64X DVRT fixups as HybridObject.
    # Its printed ARM64X/EC Machine names are synthetic; raw machines must be
    # ARM64 -> AMD64 for the pinned Wine loader (virtual.c/update_arm64x_mapping).
    native, hybrid = dump.split("HybridObject {", 1)
    fixups = {int(rva, 16): (int(size, 16), int(value, 16)) for rva, size, value in
              re.findall(r"Entry \[\s+RVA: (0x[0-9A-Fa-f]+)\s+Type: VALUE\s+Size: (0x[0-9A-Fa-f]+)\s+Value: (0x[0-9A-Fa-f]+)", native)}
    alternate = re.search(r"AddressOfEntryPoint: (0x[0-9A-Fa-f]+)", hybrid)
    if alternate is None:
        raise ValueError(f"hybrid view lacks alternate entry: {image}")
    ec_entry = int(alternate[1], 16)
    if fixups.get(pe + 4) != (2, 0x8664) or fixups.get(pe + 40) != (4, ec_entry):
        raise ValueError(f"ARM64X fixups do not establish the AMD64 entry view: {image}")
    redirects = {int(start, 16): int(end, 16) for start, end in
                 re.findall(r"(0x[0-9A-Fa-f]+) -> (0x[0-9A-Fa-f]+)", native)}
    ec_target = redirects.get(ec_entry, ec_entry)
    if not any(row["rva"] <= ec_target < row["rva"] + row["size"]
               and row["architecture"] == "arm64ec" for row in facts["code_ranges"]):
        raise ValueError(f"alternate entry does not reach real EC code: {image}")
    def offset(rva: int, size: int) -> int:
        for start, raw_size, raw in section_layout:
            if start <= rva and rva + size <= start + raw_size:
                return raw + rva - start
        raise ValueError(f"unbacked TLS RVA 0x{rva:x}: {image}")

    image_base = struct.unpack_from("<Q", data, pe + 48)[0]
    views = {}
    for abi, text in (("arm64", native), ("arm64ec", hybrid)):
        tls = re.search(r"TLSTableRVA: (0x[0-9A-Fa-f]+)\s+TLSTableSize: (0x[0-9A-Fa-f]+)", text)
        if tls is None:
            raise ValueError(f"missing {abi} TLS-directory facts: {image}")
        tls_rva, tls_size = int(tls[1], 16), int(tls[2], 16)
        callbacks = []
        if tls_rva or tls_size:
            if not tls_rva or tls_size != 40:
                raise ValueError(f"invalid {abi} TLS-directory size: {image}")
            callback_va = struct.unpack_from("<Q", data, offset(tls_rva, 40) + 24)[0]
            if callback_va:
                cursor = callback_va - image_base
                while target := struct.unpack_from("<Q", data, offset(cursor, 8))[0]:
                    callback = target - image_base
                    target_rva = redirects.get(callback, callback) if abi == "arm64ec" else callback
                    allowed = ("arm64ec", "x64") if abi == "arm64ec" else ("arm64",)
                    if not any(row["rva"] <= target_rva < row["rva"] + row["size"]
                               and row["architecture"] in allowed for row in facts["code_ranges"]):
                        raise ValueError(f"TLS callback does not reach {abi} code: {image}: 0x{callback:x}")
                    callbacks.append(callback)
                    cursor += 8
        exports = {name: {"ordinal": int(ordinal), "rva": int(rva, 16)} for ordinal, name, rva in
                   re.findall(r"Export \{\n\s+Ordinal: (\d+)\n\s+Name: (.+)\n\s+RVA: (0x[0-9A-Fa-f]+)", text)}
        imports = re.findall(r"Import \{\n\s+Name: (.+)", text)
        if not imports or not expected_exports[abi].keys() <= exports.keys():
            raise ValueError(f"missing {abi} import/export contract: {image}: {sorted(expected_exports[abi].keys() - exports.keys())}")
        for name, ordinal in expected_exports[abi].items():
            if exports[name]["ordinal"] != ordinal:
                raise ValueError(f"export {name} changed {abi} ordinal: {image}")
            target = exports[name]["rva"]
            if name in data_exports:
                if not any(start <= target < start + size for start, size in data_ranges):
                    raise ValueError(f"export {name} is not backed by initialized data: {image}")
                continue
            if abi == "arm64ec":
                target = redirects.get(target, target)
            if not any(row["rva"] <= target < row["rva"] + row["size"]
                       and row["architecture"] == abi for row in facts["code_ranges"]):
                raise ValueError(f"export {name} does not reach {abi} code: {image}")
        views[abi] = {"entry_rva": entry if abi == "arm64" else ec_entry,
                      "entry_target_rva": entry if abi == "arm64" else ec_target,
                      "tls_directory_rva": tls_rva, "tls_callbacks": callbacks,
                      "imports": imports, "exports": exports}
    if "OwnedD3D12CoreGeneration" in data_exports:
        for view in views.values():
            start = offset(view["exports"]["OwnedD3D12CoreGeneration"]["rva"], 33)
            generation = data[start:start + 33]
            if not re.fullmatch(rb"[0-9a-f]{32}\x00", generation):
                raise ValueError(f"invalid Core generation: {image}")
            view["core_generation"] = generation[:-1].decode("ascii")
        if views["arm64"]["core_generation"] != views["arm64ec"]["core_generation"]:
            raise ValueError(f"split native/EC Core generation: {image}")
    return {**facts, "path": str(image), "sha256": hashlib.sha256(data).hexdigest(),
            "entry_rva": entry, "builtin": builtin, "views": views}


def validate_wine_sdk(sdk_root: Path, readobj: Path) -> dict:
    """Reject incomplete or changed producer inputs before any translator link."""
    result = json.loads((sdk_root / "sdk-result.json").read_text())
    if result.get("schema") != 1 or result.get("kind") != "paired-wine-sdk" or result.get("status") != "pass":
        raise ValueError("Wine SDK must be a completed paired-wine-sdk schema 1 result")
    source = result.get("source", {})
    manifest = Path(source.get("manifest", ""))
    if (not manifest.is_file() or hashlib.sha256(manifest.read_bytes()).hexdigest() != source.get("sha256")
            or not (Path(source.get("path", "")) / "configure").is_file()):
        raise ValueError("Wine SDK source identity is missing or changed")
    source_path = Path(source["path"])
    source_cut = json.loads(manifest.read_text())
    current = {str(path.relative_to(source_path)): hashlib.sha256(path.read_bytes()).hexdigest()
               for path in sorted(source_path.rglob("*")) if path.is_file() and ".git" not in path.relative_to(source_path).parts}
    if not source_cut or current != source_cut:
        raise ValueError("Wine SDK source snapshot changed since production")
    variants = result.get("variants", {})
    for abi, target, windows, machine in (
        ("arm64", "aarch64", "aarch64-windows", "IMAGE_FILE_MACHINE_ARM64"),
        ("arm64ec", "arm64ec", "arm64ec-windows", "IMAGE_FILE_MACHINE_ARM64EC"),
    ):
        variant = variants.get(abi, {})
        if (variant.get("status") != "pass" or variant.get("native_arch") != "arm64"
                or variant.get("windows_abi") != abi or variant.get("windows_arches") != target):
            raise ValueError(f"Wine SDK variant is incomplete or has wrong ABI: {abi}")
        build = Path(variant.get("build_path", "")).resolve()
        if not build.is_relative_to(sdk_root.resolve()):
            raise ValueError(f"Wine SDK build is outside the completed SDK root: {abi}")
        artifacts = {row["path"]: row for row in variant.get("artifacts", [])}
        required = [build / "tools/winebuild/winebuild", build / f"libs/winecrt0/{windows}/libwinecrt0.a",
                    build / f"dlls/ntdll/{windows}/libntdll.a", build / f"dlls/dbghelp/{windows}/libdbghelp.a"]
        for path in required:
            row = artifacts.get(str(path), {})
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != row.get("sha256"):
                raise ValueError(f"Wine SDK artifact missing or hash mismatch: {path}")
            if path == required[0]:
                if macho_architectures(path) != ["arm64"]:
                    raise ValueError(f"winebuild is not thin ARM64: {path}")
            else:
                headers = subprocess.run([str(readobj), "--file-headers", str(path)],
                                         capture_output=True, text=True, check=True).stdout
                machines = re.findall(r"Machine: (\w+)", headers)
                formats = set(re.findall(r"^Format: (\S+)", headers, re.MULTILINE))
                # EC import descriptors are ARM64 COFF; short import records carry the EC ABI.
                expected_machine = machine if path.name == "libwinecrt0.a" else "IMAGE_FILE_MACHINE_ARM64"
                expected_formats = {"COFF-" + abi.upper()} if path.name == "libwinecrt0.a" else {
                    "COFF-ARM64", "COFF-import-file-" + abi.upper()}
                if not machines or set(machines) != {expected_machine} or formats != expected_formats:
                    raise ValueError(f"wrong {abi} COFF archive: {path}: {set(machines)}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="fresh directory below build/local")
    parser.add_argument("--wine-sdk", type=Path, required=True, help="completed external paired Wine SDK root, read-only")
    parser.add_argument("--llvm-mingw", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal"))
    parser.add_argument("--jobs", type=int, default=8)
    for frontend in OPTIONAL_MODULES:
        parser.add_argument("--with-" + frontend, dest="enable_" + frontend, action="store_true",
                            help="build and pair the owned " + frontend + " frontends")
    args = parser.parse_args()
    modules, meson_options = build_selection(args)
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    sdk_root = args.wine_sdk.resolve()
    mingw = args.llvm_mingw.resolve()
    if output.exists() or args.jobs < 1 or not output.is_relative_to(root / "build/local"):
        parser.error("output must be fresh under build/local and jobs must be positive")
    if subprocess.run(["git", "check-ignore", "--quiet", str(output)], cwd=root).returncode:
        parser.error("output must be git-ignored")
    output.mkdir(parents=True)
    logs = output / "logs"
    logs.mkdir()
    environment = os.environ | {
        "PATH": os.pathsep.join((str(mingw / "bin"), str(root / "toolchains/tools/bin"), os.environ["PATH"])),
        "CC_FOR_BUILD": "/usr/bin/clang", "CXX_FOR_BUILD": "/usr/bin/clang++",
    }
    result = {"schema": 1, "status": "building", "native_arch": "arm64", "windows_abi": "arm64x",
              "guest_runtime": "not_run", "scope": "fresh production PE build; not Wine/FEX execution acceptance",
              "commands": [], "inputs": [], "artifacts": [], "wine_sdk": str(sdk_root),
              "meson_options": meson_options, "modules": modules}

    def run(command: list[str], name: str, cwd: Path = root) -> str:
        log = logs / (name + ".log")
        with log.open("w") as stream:
            process = subprocess.run(command, cwd=cwd, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        result["commands"].append({"command": command, "cwd": str(cwd), "returncode": process.returncode, "log": str(log)})
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if process.returncode:
            raise RuntimeError(f"{name} failed: {log}")
        return log.read_text()

    def record(path: Path, role: str) -> None:
        result["inputs"].append({"path": str(path), "role": role,
                                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})

    try:
        sdk_result = validate_wine_sdk(sdk_root, mingw / "bin/llvm-readobj")
        result["wine_sdk_source"] = sdk_result["source"]
        record(sdk_root / "sdk-result.json", "external paired Wine SDK result")
        record(Path(sdk_result["source"]["manifest"]), "Wine source identity")
        record(Path(__file__).resolve(), "paired build orchestration")
        for name in ("clang", "ld.lld", "llvm-readobj"):
            executable = (mingw / "bin" / name).resolve()
            if "arm64" not in macho_architectures(executable):
                raise RuntimeError(f"tool has no native ARM64 slice: {executable}")
            record(executable, "toolchain")
        meson = shutil.which("meson", path=environment["PATH"])
        ninja = shutil.which("ninja", path=environment["PATH"])
        if not meson or not ninja:
            raise RuntimeError("local Meson/Ninja unavailable")
        # Refuse a mixed source cut when another worker/user changes owned code
        # during either half. The retained manifest identifies the exact graph.
        owned_sources = [path for directory in ("src", "include", "libs")
                         for path in (root / directory).rglob("*") if path.is_file() and path.name != ".git"]
        owned_sources.extend(root / name for name in ("meson.build", "meson.options", "build-arm64.txt",
                                                      "build-arm64ec-only.txt", "build-native-arm64.txt", "version.h.in"))
        source_cut = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in owned_sources}
        (output / "source-cut.json").write_text(json.dumps(source_cut, indent=2, sort_keys=True) + "\n")
        result["owned_source_cut"] = str(output / "source-cut.json")
        builds = {}
        wine = {}
        runtimes = {}
        commands = {}
        baselines = {}
        core_generation = uuid.uuid4().hex
        result["core_generation"] = core_generation
        for abi, target, windows, cross in (("arm64", "aarch64", "aarch64-windows", "build-arm64.txt"),
                                           ("arm64ec", "arm64ec", "arm64ec-windows", "build-arm64ec-only.txt")):
            sdk = Path(sdk_result["variants"][abi]["build_path"])
            archives = [sdk / "libs/winecrt0" / windows / "libwinecrt0.a",
                        sdk / "dlls/ntdll" / windows / "libntdll.a",
                        sdk / "dlls/dbghelp" / windows / "libdbghelp.a"]
            wine[abi] = archives
            for archive in archives:
                record(archive, "external Wine " + abi + " CRT/import archive")
                run([str(mingw / "bin/llvm-readobj"), "--file-headers", str(archive)], abi + "-" + archive.stem + "-headers")
            winebuild = sdk / "tools/winebuild/winebuild"
            if macho_architectures(winebuild) != ["arm64"]:
                raise RuntimeError(f"winebuild is not thin ARM64: {winebuild}")
            record(winebuild, "external Wine native helper")
            build = output / ("pe-" + abi)
            builds[abi] = build
            run([meson, "setup", str(build), str(root), "--cross-file", str(root / cross),
                 "--native-file", str(root / "build-native-arm64.txt"), "-Dnative_arch=arm64",
                 "-Dbuild_native_components=false", "-Dwine_build_path=" + str(sdk),
                 "-Denable_d3d12=true", "-Dwine_builtin_dll=true", "-Dd3d12_core_generation=" + core_generation,
                 *["-D" + name + "=" + str(enabled).lower() for name, enabled in meson_options.items()],
                 "--buildtype=debugoptimized"], abi + "-meson-setup")
            run([ninja, "-C", str(build), "-j", str(args.jobs),
                 *[relative if module == "d3d12core" else relative + ".postproc"
                   for module, relative in modules.items()]], abi + "-meson-build")
            commands[abi] = {}
            baselines[abi] = {}
            for module, relative in modules.items():
                text = run([ninja, "-C", str(build), "-t", "commands", relative], abi + "-" + module + "-graph")
                link = shlex.split(text.splitlines()[-1])
                if "-shared" not in link or relative not in link:
                    raise RuntimeError(f"expected canonical DLL link command: {relative}")
                commands[abi][module] = link
                facts = inspect_pe(build / relative)
                if facts["windows_abi"] != abi:
                    raise RuntimeError(f"wrong Meson input ABI: {relative}: {facts}")
                dump = run([str(mingw / "bin/llvm-readobj"), "--coff-exports", str(build / relative)], abi + "-" + module + "-exports")
                baselines[abi][module] = {name: int(ordinal) for ordinal, name in
                                          re.findall(r"Export \{\n\s+Ordinal: (\d+)\n\s+Name: (.+)", dump)}
            # Query the real compiler driver rather than pinning a second CRT library list.
            driver = run([str(mingw / "bin" / (target + "-w64-mingw32-g++")), "-###", "-shared", "-static",
                          "-x", "c++", "/dev/null", "-o", str(output / "unused.dll")], abi + "-runtime-driver")
            linker = shlex.split(driver.splitlines()[-1])
            paths = [Path(arg[2:]) for arg in linker if arg.startswith("-L")]
            runtime = []
            for arg in linker:
                if arg.startswith("-l"):
                    name = arg[3:] if arg.startswith("-l:") else "lib" + arg[2:] + ".a"
                    found = next((path / name for path in paths if (path / name).is_file()), None)
                    if found is None:
                        raise RuntimeError(f"cannot resolve real runtime archive: {arg}")
                    runtime.append(found)
                elif arg.endswith((".a", ".o")) and Path(arg).is_file():
                    runtime.append(Path(arg))
            if not any(path.name == "dllcrt2.o" for path in runtime):
                raise RuntimeError(f"driver did not provide real DLL startup: {abi}")
            runtimes[abi] = runtime
            for path in runtime:
                record(path, "toolchain " + abi + " runtime")

        winebuild = Path(sdk_result["variants"]["arm64"]["build_path"]) / "tools/winebuild/winebuild"
        probe = output / "paired-dll-probe.c"
        probe.write_text('#include <windows.h>\n__declspec(dllexport) DWORD owned_arm64x_probe(void) { return GetCurrentProcessId(); }\n'
                         'BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID p) { return TRUE; }\n')
        paired = output / "aarch64-windows"
        paired.mkdir()
        for module in ("paired-dll-probe", *modules):
            objects = []
            libraries = []
            specs = []
            definitions = []
            for abi, target in (("arm64", "aarch64"), ("arm64ec", "arm64ec")):
                if module == "paired-dll-probe":
                    obj = output / (abi + "-probe.o")
                    run([str(mingw / "bin" / (target + "-w64-mingw32-gcc")), "-c", str(probe), "-o", str(obj)], abi + "-dll-probe-compile")
                    current = [obj]
                    deps = wine[abi]
                else:
                    build = builds[abi]
                    current = []
                    deps = []
                    for arg in commands[abi][module][1:]:
                        if arg.startswith("-"):
                            if arg.startswith("-l"):
                                library = mingw / (target + "-w64-mingw32/lib") / ("lib" + arg[2:] + ".a")
                                deps.append(library)
                            continue
                        path = (build / arg).resolve()
                        if arg.endswith((".obj", ".o")):
                            # Resources have no dual code ABI; retain one canonical resource object.
                            if not arg.endswith("version.o") or abi == "arm64":
                                current.append(path)
                        elif arg.endswith(".a"):
                            deps.append(path)
                        elif arg.endswith(".def") and path not in definitions:
                            definitions.append(path)
                objects.extend(current)
                libraries.extend(deps)
                if module in ("winemetal", "paired-dll-probe"):
                    spec = output / (module + "-" + abi + "-spec.o")
                    run([str(winebuild), "--dll", "-F", module + ".dll", "-b", target + "-windows",
                         "--cc-cmd=" + str(mingw / "bin" / (target + "-w64-mingw32-clang")),
                         "-o", str(spec), *map(str, current)], module + "-" + abi + "-spec")
                    specs.append(spec)
                    libraries.extend(path for path in runtimes[abi] if path.suffix == ".a" and path.name != "libmingw32.a")
                else:
                    objects.extend(path for path in runtimes[abi] if path.suffix == ".o")
                    libraries.extend(path for path in runtimes[abi] if path.suffix == ".a")
            for path in (*objects, *specs, *libraries, *definitions):
                record(path, module + " paired link input")
            image = paired / (module + ".dll")
            compiler = mingw / ("bin/arm64ec-w64-mingw32-gcc" if module in ("winemetal", "paired-dll-probe")
                                else "bin/arm64ec-w64-mingw32-g++")
            # Both real CRT halves are explicit. No /noentry, ignored undefined symbols,
            # probe fallback or Meson check bypass can manufacture a production image.
            command = [str(compiler), "-marm64x", "-shared", "-static", "-nostdlib", "-o", str(image),
                       "-Wl,--file-alignment=16384", "-Wl,--section-alignment=16384", "-Wl,-e,DllMainCRTStartup",
                       "-Wl,-Map=" + str(paired / (module + ".map")),
                       "-Wl,--out-implib=" + str(paired / (module + ".dll.a")),
                       *map(str, definitions), *map(str, specs), *map(str, objects),
                       "-Wl,--start-group", *map(str, dict.fromkeys(libraries)), "-Wl,--end-group"]
            # MinGW's .def argument targets only the EC view in ARM64X mode.
            # Let its real driver lower the link, then also give COFF the same
            # canonical definition for the native view (including ordinals).
            driver = run([*command, "-###"], module + "-paired-driver")
            lowered = shlex.split(driver.splitlines()[-1])
            coff = shlex.split(run([lowered[0], "-###", *lowered[1:]], module + "-paired-coff").strip())
            run([str(mingw / "bin/lld-link"), *coff[1:],
                 *["-defarm64native:" + str(path) for path in definitions]], module + "-paired-link")
            complete_tls_fixups(image, paired / (module + ".map"))
            if module != "d3d12core":
                run([str(winebuild), "--builtin", str(image)], module + "-builtin")
            dump = run([str(mingw / "bin/llvm-readobj"), "--file-headers", "--coff-load-config", "--coff-imports",
                        "--coff-exports", "--coff-basereloc", str(image)], module + "-final-audit")
            expected = {abi: {"owned_arm64x_probe": 1} if module == "paired-dll-probe" else baselines[abi][module]
                        for abi in ("arm64", "arm64ec")}
            data_exports = tuple(name for path in definitions for name in
                                 re.findall(r"^\s+(\w+)\s+DATA\s*$", path.read_text(), re.MULTILINE))
            facts = audit_image(image, dump, expected, builtin=module != "d3d12core", data_exports=data_exports)
            if module == "d3d12core" and facts["views"]["arm64"]["core_generation"] != core_generation:
                raise ValueError("paired Core does not match the configured Core/Main generation")
            facts["map"] = str(paired / (module + ".map"))
            if module == "paired-dll-probe":
                result["paired_dll_probe"] = facts
            else:
                result["artifacts"].append(facts)
        if any(hashlib.sha256((root / name).read_bytes()).hexdigest() != digest
               for name, digest in source_cut.items()):
            raise RuntimeError("owned sources changed during paired build; this is not a single source cut")
        result["status"] = "pass"
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        result["status"] = "blocked"
        result["blocker"] = str(error)
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "scope": result["scope"],
                      "guest_runtime": result["guest_runtime"], "result": str(output / "result.json"),
                      "artifacts": [{"path": row["path"], "sha256": row["sha256"],
                                     "entries": {abi: view["entry_target_rva"] for abi, view in row["views"].items()},
                                     "export_counts": {abi: len(view["exports"]) for abi, view in row["views"].items()}}
                                    for row in result["artifacts"]]}, indent=2))
    if result["status"] != "pass":
        print(result["blocker"], file=sys.stderr)
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
