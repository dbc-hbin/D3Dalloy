#!/usr/bin/env python3
"""Build native ARM64 Wine with ARM64 or ARM64X PE modules from a pinned source."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile

from owned_graphics_arch import inspect_pe, macho_architectures

COMMIT = "c2f86161d543acda5be91ce60be881b3efea9658"
WINE_COMMIT = "c05971f5ea628cbb8f793662a1c54ba9a2b9d82f"


def stage_dependencies(prefix: Path, destination: Path, soname: str, run) -> dict:
    """Bundle a verified thin ARM64 library closure beside Wine Unix modules."""
    provenance = json.loads((prefix / "provenance.json").read_text())
    artifacts = {entry["path"]: entry for entry in provenance["artifacts"]}
    pending = [Path(soname).name]
    bundled = {}
    while pending:
        name = pending.pop()
        if name in bundled:
            continue
        source = (prefix / "lib" / name).resolve()
        if not source.is_relative_to(prefix):
            raise ValueError(f"native dependency escapes its prefix: {name}")
        identity = artifacts.get(str(source.relative_to(prefix)))
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if not identity or identity["sha256"] != digest or macho_architectures(source) != ["arm64"]:
            raise ValueError(f"native dependency lacks verified thin ARM64 provenance: {source}")
        target = destination / name
        shutil.copyfile(source, target)
        command = ["/usr/bin/install_name_tool", "-id", "@loader_path/" + name]
        dependencies = run(["/usr/bin/otool", "-L", str(target)], name + "-original-dependencies")
        for line in dependencies.splitlines()[1:]:
            dependency = line.strip().split(" (", 1)[0]
            if dependency.startswith(("/System/Library/", "/usr/lib/")):
                continue
            dependency_name = Path(dependency).name
            command += ["-change", dependency, "@loader_path/" + dependency_name]
            pending.append(dependency_name)
        run(command + [str(target)], name + "-relocate")
        run(["/usr/bin/codesign", "--force", "--sign", "-", str(target)], name + "-sign")
        bundled[name] = {"path": str(target), "source_sha256": digest,
                         "sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "arch": "arm64"}
    return {"soname": "@loader_path/" + Path(soname).name, "provenance": provenance,
            "artifacts": list(bundled.values())}


def require_library(config: Path, library: str) -> str:
    text = config.read_text()
    match = re.search(rf'^#define SONAME_LIB{library} "([^"\n]+)"$', text, re.M)
    if not match:
        raise ValueError(f"Wine configure did not enable {library} support")
    soname = "@loader_path/" + Path(match[1]).name
    config.write_text(text[:match.start(1)] + soname + text[match.end(1):])
    return soname


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--sdk", type=Path)
    inputs.add_argument("--wine-repository", type=Path, help="isolated checkout containing pinned ZZZ-optimized Wine 11.17")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-abi", choices=("arm64", "arm64x"), default="arm64")
    parser.add_argument("--wow64", action="store_true", help="also build i386 PE modules for native WoW64")
    parser.add_argument("--fex-result", type=Path, help="audited pinned FEX build result containing native xtajit.dll")
    parser.add_argument("--tls-prefix", type=Path, required=True,
                        help="isolated ARM64 GnuTLS headers, libraries and verified provenance.json")
    parser.add_argument("--freetype-prefix", type=Path, required=True,
                        help="isolated ARM64 FreeType headers, libraries and verified provenance.json")
    parser.add_argument("--llvm-mingw", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal"))
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if args.jobs < 1 or output.exists() or not output.is_relative_to(root / "build/local"):
        parser.error("output must be fresh below build/local and jobs positive")
    if args.sdk:
        sdk = args.sdk.resolve()
        provenance = json.loads((sdk / "wine-source-provenance.json").read_text())
        archive = sdk / "wine-source.tar"
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if provenance["source_commit"] != COMMIT or provenance["source_archive_sha256"] != digest:
            parser.error("SDK archive does not match pinned provenance")
    else:
        repository = args.wine_repository.resolve()
        if not repository.is_relative_to(root / "toolchains"):
            parser.error("Wine checkout must be isolated below toolchains")
        tree = subprocess.check_output(["git", "-C", str(repository), "rev-parse", WINE_COMMIT + "^{tree}"],
                                       text=True).strip()
        provenance = {"source_commit": WINE_COMMIT, "source_tree": tree,
                      "source_url": "https://github.com/dbc-hbin/wine-yaagl-d3dmetal"}
    output.mkdir(parents=True)
    source, build, runtime = (output / name for name in ("wine-source", "wine-build", "runtime"))
    source.mkdir()
    build.mkdir()
    logs = output / "logs"
    logs.mkdir()
    if not args.sdk:
        archive = output / "wine-source.tar"
        with archive.open("wb") as stream:
            subprocess.run(["git", "-C", str(repository), "archive", "--format=tar", WINE_COMMIT],
                           stdout=stream, check=True)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    environment = os.environ | {"PATH": str(args.llvm_mingw.resolve() / "bin") + os.pathsep + os.environ["PATH"],
                                "WINEPREFIX": str(output / "identity-prefix"),
                                "PKG_CONFIG_PATH": str(args.freetype_prefix.resolve() / "lib/pkgconfig") +
                                    os.pathsep + str(args.tls_prefix.resolve() / "lib/pkgconfig") +
                                    os.pathsep + os.environ.get("PKG_CONFIG_PATH", "")}
    result = {"schema": 1, "source_commit": provenance["source_commit"], "source_tree": provenance["source_tree"],
              "source_archive_sha256": digest, "runtime": str(runtime),
              "native_arch": "arm64", "windows_arch": "aarch64", "windows_abi": args.windows_abi,
              "commands": [], "status": "building"}
    if args.sdk:
        result["sdk"] = str(sdk)
    else:
        result["source_url"] = provenance["source_url"]

    def run(command: list[str], name: str, cwd: Path = build) -> str:
        log = logs / (name + ".log")
        with log.open("w") as stream:
            process = subprocess.run(command, cwd=cwd, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        result["commands"].append({"command": command, "cwd": str(cwd), "returncode": process.returncode, "log": str(log)})
        (output / "runtime-result.json").write_text(json.dumps(result, indent=2) + "\n")
        if process.returncode:
            raise RuntimeError(f"{name} failed ({process.returncode}): {log}")
        return log.read_text()

    try:
        with tarfile.open(archive) as snapshot:
            snapshot.extractall(source, filter="data")
        license_file = source / "COPYING.LIB"
        result["source_license"] = {"license": "LGPL-2.1-or-later", "path": str(license_file),
                                    "sha256": hashlib.sha256(license_file.read_bytes()).hexdigest()}
        result["patches"] = []
        patches = (("wine-arm64-metal-layer.patch", "wine-arm64-runtime-loader.patch") if args.sdk else
                   ("wine-zzz-11.17-native-woa.patch", "wine-arm64-native-woa.patch", "wine-arm64-native-jit.patch",
                    "wine-arm64-emulated-image-protection.patch", "wine-arm64-native-jit-allocation.patch",
                    "wine-zzz-11.17-metal-layer.patch", "wine-zzz-11.17-arm64ec.patch",
                    "wine-arm64ec-varargs.patch"))
        if args.wow64:
            if args.sdk:
                raise ValueError("native WoW64 guest windows require the pinned optimized Wine 11.17 source")
            patches += ("wine-arm64-guest-window-thunks.patch", "wine-arm64-guest-window.patch",
                        "wine-arm64-wow64win-guest-window.patch", "wine-arm64-wow64-network.patch")
        for name in patches:
            patch = root / "scripts/patches" / name
            result["patches"].append({"path": str(patch), "sha256": hashlib.sha256(patch.read_bytes()).hexdigest()})
            run(["/usr/bin/patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)], name, source)
        arches = "aarch64,arm64ec" if args.windows_abi == "arm64x" else "aarch64"
        if args.wow64:
            arches += ",i386"
        result["wow64"] = args.wow64
        tls_prefix = args.tls_prefix.resolve()
        if not (tls_prefix / "include/gnutls/gnutls.h").is_file():
            raise ValueError(f"missing GnuTLS development headers: {tls_prefix}")
        freetype_prefix = args.freetype_prefix.resolve()
        if not (freetype_prefix / "include/freetype2/ft2build.h").is_file():
            raise ValueError(f"missing FreeType development headers: {freetype_prefix}")
        run([str(source / "configure"), "--enable-archs=" + arches, "--disable-tests", "--without-x", "--without-ffmpeg", "--with-gnutls", "--with-freetype",
             "--prefix=" + str(runtime), "CC=/usr/bin/clang", "CXX=/usr/bin/clang++",
             "CFLAGS=-arch arm64 -O1", "CXXFLAGS=-arch arm64 -O1", "LDFLAGS=-arch arm64",
             "GNUTLS_CFLAGS=-I" + str(tls_prefix / "include"),
             "GNUTLS_LIBS=-L" + str(tls_prefix / "lib") + " -lgnutls",
             "FREETYPE_CFLAGS=-I" + str(freetype_prefix / "include/freetype2"),
             "FREETYPE_LIBS=-L" + str(freetype_prefix / "lib") + " -lfreetype"], "configure")
        tls_soname = require_library(build / "include/config.h", "GNUTLS")
        freetype_soname = require_library(build / "include/config.h", "FREETYPE")
        font_tool = build / "tools/sfnt2fon"
        font_tool.mkdir(parents=True, exist_ok=True)
        result["font_tool"] = stage_dependencies(
            freetype_prefix, font_tool, freetype_soname, lambda command, name: run(command, "font-tool-" + name))
        run(["make", "-j" + str(args.jobs)], "build")
        run(["make", "install"], "install")
        result["tls"] = stage_dependencies(tls_prefix, runtime / "lib/wine/aarch64-unix", tls_soname, run)
        result["freetype"] = stage_dependencies(freetype_prefix, runtime / "lib/wine/aarch64-unix", freetype_soname, run)
        if args.fex_result:
            if not args.wow64:
                raise ValueError("native xtajit.dll staging requires --wow64")
            fex = json.loads(args.fex_result.read_text())
            if fex.get("status") != "pass" or fex.get("source_commit") != "9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf":
                raise ValueError("native WoW64 requires a passing pinned FEX-2609.1 build")
            artifact = fex["artifacts"]["xtajit.dll"]
            module = Path(artifact["path"])
            if hashlib.sha256(module.read_bytes()).hexdigest() != artifact["sha256"] or inspect_pe(module)["windows_abi"] != "arm64":
                raise ValueError("native xtajit.dll identity does not match its FEX build result")
            shutil.copyfile(module, runtime / "lib/wine/aarch64-windows/xtajit.dll")
            result["fex"] = fex
        identities = []
        for relative in ("bin/wine", "bin/wineserver", "lib/wine/aarch64-unix/wine", "lib/wine/aarch64-unix/ntdll.so"):
            path = runtime / relative
            arch = run(["/usr/bin/lipo", "-archs", str(path)], relative.replace("/", "-") + "-arch").strip()
            if arch != "arm64":
                raise RuntimeError(f"wrong runtime architecture: {path}: {arch}")
            identities.append({"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "arch": arch})
        result["identities"] = identities
        if not args.sdk:
            metadata = run(["/usr/bin/vtool", "-show-build", str(runtime / "lib/wine/aarch64-unix/wine")], "loader-build-version")
            result["loader_build_version"] = dict(re.findall(r"^\s*(platform|minos|sdk)\s+(\S+)", metadata, re.M))
            if result["loader_build_version"] != {"platform": "MACOS", "minos": "12.0", "sdk": "12.0"}:
                raise RuntimeError("native loader does not declare the macOS 12 SDK x18 ABI")
        result["native_closure"] = []
        for path in sorted((runtime / "lib/wine/aarch64-unix").iterdir()):
            if not path.is_file() or path.is_symlink():
                continue
            if macho_architectures(path) != ["arm64"]:
                raise RuntimeError(f"native closure lacks thin ARM64 identity: {path}")
            dependencies = run(["/usr/bin/otool", "-L", str(path)], path.name + "-dependencies")
            for line in dependencies.splitlines()[1:]:
                dependency = line.strip().split(" (", 1)[0]
                if dependency.startswith(("/System/Library/", "/usr/lib/")):
                    continue
                if dependency.startswith(("@rpath/", "@loader_path/")) and (path.parent / dependency.split("/", 1)[1]).is_file():
                    continue
                raise RuntimeError(f"unpackaged native dependency: {path}: {dependency}")
            result["native_closure"].append({"path": str(path.relative_to(runtime)),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "arch": "arm64", "dependencies": dependencies})
        result["pe_closure"] = []
        for path in sorted((runtime / "lib/wine/aarch64-windows").iterdir()):
            if path.suffix.lower() not in (".dll", ".exe", ".drv", ".ocx", ".cpl", ".acm"):
                continue
            identity = inspect_pe(path)
            accepted = {"arm64", "arm64ec", "arm64x"} if args.windows_abi == "arm64x" else {"arm64"}
            if identity["windows_abi"] not in accepted or (path.name == "ntdll.dll" and identity["windows_abi"] != args.windows_abi):
                raise RuntimeError(f"wrong Windows ABI in {args.windows_abi} PE closure: {path}: {identity['windows_abi']}")
            result["pe_closure"].append({"path": str(path.relative_to(runtime)),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "identity": identity})
        if args.wow64:
            result["wow64_pe_closure"] = []
            for path in sorted((runtime / "lib/wine/i386-windows").iterdir()):
                if path.suffix.lower() not in (".dll", ".exe", ".drv", ".ocx", ".cpl", ".acm"):
                    continue
                identity = inspect_pe(path)
                if identity["windows_abi"] != "x86":
                    raise RuntimeError(f"wrong guest ABI in i386 PE closure: {path}")
                result["wow64_pe_closure"].append({"path": str(path.relative_to(runtime)),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "identity": identity})
        result["loader_version"] = run([str(runtime / "bin/wine"), "--version"], "loader-version").strip()
        result["status"] = "pass"
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError) as error:
        result["status"] = "blocked"
        result["blocker"] = str(error)
    (output / "runtime-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
