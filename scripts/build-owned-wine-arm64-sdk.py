#!/usr/bin/env python3
"""Archive pinned Wine, apply the native Metal guard, and build isolated SDK targets.

No installation, Wine prefix, translator import, download, or working-tree copy.
Each output variant is built from the archived commit, including pure ARM64EC.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile

COMMIT = "c2f86161d543acda5be91ce60be881b3efea9658"
TRACKED = ("dlls/winemac.drv/cocoa_window.m", "dlls/winemac.drv/d3dmetal_objc.h",
           "dlls/winemac.drv/d3dmetal_objc.m", "dlls/winemac.drv/event.c",
           "dlls/winemac.drv/d3dmetal.c")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine-repository", type=Path, default=Path("../zzz-wine-yaagl-pr-runtime"))
    parser.add_argument("--llvm-mingw", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal"))
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    source = output / "wine-source"
    if args.jobs < 1 or source.exists() or (output / "sdk-result.json").exists():
        parser.error("jobs must be positive and SDK output must be fresh")
    # Keep the source archive and all generated files inside the ignored build root.
    if not output.is_relative_to(root / "build/local"):
        parser.error("output must be below project build/local")
    ignored = subprocess.run(["git", "check-ignore", "--quiet", str(output)], cwd=root)
    if ignored.returncode:
        parser.error("output must be git-ignored")
    output.mkdir(parents=True, exist_ok=True)
    logs = output / "wine-sdk-logs"
    logs.mkdir()
    environment = os.environ | {
        "PATH": os.pathsep.join((str(args.llvm_mingw.resolve() / "bin"),
                                str(root / "toolchains/tools/bin"), os.environ["PATH"])),
        "CC": "/usr/bin/clang", "CXX": "/usr/bin/clang++",
        "OBJCXX": "/usr/bin/clang++", "OBJCXX_FOR_BUILD": "/usr/bin/clang++",
        "CC_FOR_BUILD": "/usr/bin/clang", "CXX_FOR_BUILD": "/usr/bin/clang++",
    }
    result = {"schema": 1, "source_commit": COMMIT, "commands": [], "variants": {},
              "installed": False, "guest_runtime": "not_run", "status": "building"}

    def run(command: list[str], name: str, cwd: Path = root) -> str:
        log = logs / (name + ".log")
        with log.open("w") as stream:
            process = subprocess.run(command, cwd=cwd, env=environment,
                                     stdout=stream, stderr=subprocess.STDOUT)
        result["commands"].append({"command": command, "cwd": str(cwd),
                                   "returncode": process.returncode, "log": str(log)})
        if process.returncode:
            raise RuntimeError(f"{name} failed ({process.returncode}): {log}")
        return log.read_text()

    try:
        repository = args.wine_repository.resolve()
        result["source_repository"] = str(repository)
        result["source_tree"] = run(["git", "-C", str(repository), "rev-parse", COMMIT + "^{tree}"],
                                     "source-tree").strip()
        archive = output / "wine-source.tar"
        command = ["git", "-C", str(repository), "archive", "--format=tar", COMMIT]
        with archive.open("wb") as stream:
            subprocess.run(command, stdout=stream, check=True)
        result["commands"].append({"command": command, "cwd": str(root), "returncode": 0,
                                   "stdout": str(archive)})
        result["source_archive_sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        source.mkdir()
        with tarfile.open(archive) as snapshot:
            snapshot.extractall(source, filter="data")
        result["original_file_sha256"] = {
            name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in TRACKED}
        original = output / "original-cocoa_window.m"
        original.write_bytes((source / TRACKED[0]).read_bytes())
        patch = root / "scripts/patches/wine-arm64-metal-layer.patch"
        preserved_patch = output / patch.name
        preserved_patch.write_bytes(patch.read_bytes())
        result["patch"] = str(preserved_patch)
        result["patch_sha256"] = hashlib.sha256(patch.read_bytes()).hexdigest()
        run(["/usr/bin/patch", "--batch", "--fuzz=0", "-p1", "-i", str(preserved_patch)],
            "apply-metal-guard", source)
        result["patched_file_sha256"] = {
            name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in TRACKED}
        (output / "wine-source-provenance.json").write_text(json.dumps(result, indent=2) + "\n")
        variants = (("hybrid", "arm64", "aarch64,arm64ec", "aarch64-windows"),
                    ("pure-ec", "arm64", "arm64ec", "arm64ec-windows"),
                    ("x64-check", "x86_64", "x86_64", "x86_64-windows"))
        for name, native, arches, windows in variants:
            build = output / ("wine-build-" + name)
            build.mkdir()
            flags = f"-arch {native} -O1"
            command = [str(source / "configure"), "--enable-archs=" + arches, "--disable-tests",
                       "--prefix=" + str(output / ("uninstalled-" + name)),
                       "CC=/usr/bin/clang", "CXX=/usr/bin/clang++",
                       "CFLAGS=" + flags, "CXXFLAGS=" + flags, "LDFLAGS=-arch " + native]
            if native == "x86_64":
                command.append("--host=x86_64-apple-darwin")
                # The x64 branch check does not render fonts; no ARM64-only Homebrew FreeType.
                command.append("--without-freetype")
            run(command, name + "-configure", build)
            targets = ["tools/winebuild/winebuild", "dlls/ntdll/ntdll.so", "dlls/win32u/win32u.so",
                       "dlls/winemac.drv/winemac.so",
                       f"libs/winecrt0/{windows}/libwinecrt0.a",
                       f"dlls/ntdll/{windows}/libntdll.a", f"dlls/dbghelp/{windows}/libdbghelp.a"]
            run(["make", "-j" + str(args.jobs), *targets], name + "-build", build)
            artifacts = []
            for target in targets:
                path = build / target
                row = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                if not target.endswith(".a"):
                    row["native_arch"] = run(["/usr/bin/lipo", "-archs", str(path)],
                                              name + "-" + path.name + "-arch").strip()
                    if row["native_arch"] != native:
                        raise RuntimeError(f"wrong native architecture: {path}: {row['native_arch']}")
                    row["dependencies"] = run(["/usr/bin/otool", "-L", str(path)],
                                               name + "-" + path.name + "-dependencies")
                else:
                    row["coff_headers_log"] = str(logs / (name + "-" + path.parent.parent.name + "-coff.log"))
                    run([str(args.llvm_mingw.resolve() / "bin/llvm-readobj"), "--file-headers", str(path)],
                        name + "-" + path.parent.parent.name + "-coff")
                artifacts.append(row)
            variant = {"build_path": str(build), "native_arch": native, "windows_arches": arches,
                       "artifacts": artifacts, "status": "pass"}
            result["variants"][name] = variant
            (output / ("sdk-ready-" + name + ".json")).write_text(json.dumps(variant, indent=2) + "\n")
        result["status"] = "pass"
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError) as error:
        result["status"] = "blocked"
        result["blocker"] = str(error)
    (output / "sdk-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
