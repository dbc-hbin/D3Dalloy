#!/usr/bin/env python3
"""Build isolated native ARM64 AIR/Metal artifacts and attempt real Windows translator targets.

No downloads, Wine process, prefix, or security setting is involved. Production
PE targets use the canonical Meson graph, including fresh paired ARM64X DLLs.
Core links as a native PE; all other Windows modules retain builtin postprocessing.
A blocked full build makes the exit nonzero; building DLLs is not guest runtime proof.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from owned_graphics_arch import inspect_pe, macho_architectures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="fresh isolated evidence/build directory")
    parser.add_argument("--native-llvm", type=Path, default=Path("toolchains/llvm-15-arm64/llvm@15/15.0.7"))
    parser.add_argument("--llvm-mingw", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal"))
    parser.add_argument("--wine-sdk", type=Path, default=Path("toolchains/wine-sdk"))
    parser.add_argument("--wine-build", type=Path, help="use an isolated real Wine build instead of an installed SDK")
    parser.add_argument("--wine-ec-build", type=Path, help="separate pure-EC Wine build for the ARM64EC configuration")
    parser.add_argument("--wine-source", type=Path, help="pinned Wine source snapshot for fresh paired ARM64X SDK inputs")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    llvm = args.native_llvm.resolve()
    mingw = args.llvm_mingw.resolve()
    sdk = args.wine_sdk.resolve()
    if output.exists() or args.jobs < 1:
        parser.error("output must be fresh and jobs must be positive")
    output.mkdir(parents=True)
    environment = os.environ | {
        "PATH": os.pathsep.join((str(mingw / "bin"), str(root / "toolchains/tools/bin"), os.environ["PATH"])),
        "CC": "/usr/bin/clang", "CXX": "/usr/bin/clang++",
        "OBJCXX": "/usr/bin/clang++", "OBJCXX_FOR_BUILD": "/usr/bin/clang++",
        "CC_FOR_BUILD": "/usr/bin/clang", "CXX_FOR_BUILD": "/usr/bin/clang++",
    }
    result = {"schema": 1, "native_arch": "arm64", "guest_arch": None,
              "scope": "native standalone bridge and compiler; Windows build artifacts only",
              "commands": [], "artifacts": [], "windows": [], "native_gpu": "not_run", "blockers": []}

    def run(command: list[str], name: str, cwd: Path = root) -> dict:
        process = subprocess.run(command, cwd=cwd, env=environment, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = output / (name + ".log")
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(process.stdout)
        row = {"command": command, "cwd": str(cwd), "returncode": process.returncode, "log": str(log)}
        return row

    def checked(command: list[str], name: str) -> dict:
        row = run(command, name)
        result["commands"].append(row)
        if row["returncode"]:
            raise RuntimeError(f"{name} failed: {row['log']}")
        return row

    def native_artifact(path: Path) -> None:
        arches = macho_architectures(path)
        if arches != ["arm64"]:
            raise RuntimeError(f"native artifact must be thin ARM64: {path}: {arches}")
        row = checked(["/usr/bin/otool", "-L", str(path)], path.name + "-dependencies")
        identity = checked(["/usr/bin/otool", "-D", str(path)], path.name + "-identity")
        identities = Path(identity["log"]).read_text().splitlines()[1:]
        dependencies = [line.strip().split(" (", 1)[0]
                        for line in Path(row["log"]).read_text().splitlines()[1:]]
        local_dependencies = []
        for dependency in dependencies:
            if dependency in identities or dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if dependency == "@rpath/winemetal.dylib":
                candidates = [path.parent.parent / "native/src/nativemetal/winemetal.dylib"]
            elif dependency.startswith("@loader_path/"):
                candidates = [path.parent / dependency.removeprefix("@loader_path/")]
            elif dependency.startswith("@rpath/"):
                name = dependency.removeprefix("@rpath/")
                candidates = ([*args.wine_build.resolve().glob("dlls/*/" + name)] if args.wine_build
                              else [sdk / "lib/wine/aarch64-unix" / name])
            else:
                candidates = [Path(dependency)]
            candidates = [candidate.resolve() for candidate in candidates if candidate.is_file()]
            if len(candidates) != 1:
                raise RuntimeError(f"cannot uniquely resolve native dependency: {path}: {dependency}")
            resolved = candidates[0]
            if "arm64" not in macho_architectures(resolved):
                raise RuntimeError(f"dependency has no ARM64 slice: {resolved}")
            local_dependencies.append({"install_name": dependency, "path": str(resolved),
                                       "sha256": hashlib.sha256(resolved.read_bytes()).hexdigest(),
                                       "native_arch": macho_architectures(resolved)})
        result["artifacts"].append({"path": str(path), "native_arch": arches,
                                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                    "dependencies": dependencies, "local_dependencies": local_dependencies})

    try:
        for executable in (llvm / "bin/llvm-config", mingw / "bin/clang", mingw / "bin/llvm-readobj", mingw / "bin/lld-link"):
            if "arm64" not in macho_architectures(executable.resolve()):
                raise RuntimeError(f"tool has no native ARM64 slice: {executable}")
        config = checked([str(llvm / "bin/llvm-config"), "--link-static", "--libs", "bitwriter", "passes"], "llvm-libraries")
        libraries = Path(config["log"]).read_text().split()
        for library in libraries:
            if not library.startswith("-lLLVM"):
                raise RuntimeError(f"unexpected LLVM library: {library}")
            checked(["/usr/bin/lipo", "-verify_arch", "arm64", str(llvm / "lib" / ("lib" + library[2:] + ".a"))],
                    "llvm-architecture/" + library[2:])
        meson = shutil.which("meson", path=environment["PATH"])
        ninja = shutil.which("ninja", path=environment["PATH"])
        if not meson or not ninja:
            raise RuntimeError("local Meson/Ninja unavailable")
        native = output / "native"
        checked([meson, "setup", str(native), str(root), "--native-file", str(root / "build-native-arm64.txt"),
                 "-Dnative_bridge_only=true", "-Dnative_llvm_path=" + os.path.relpath(llvm, root),
                 "--buildtype=debugoptimized"], "native-setup")
        checked([ninja, "-C", str(native), "-j", str(args.jobs)], "native-build")
        compiler = native / "src/airconv/darwin/airconv"
        bridge = native / "src/nativemetal/winemetal.dylib"
        native_artifact(compiler)
        native_artifact(bridge)
        checked(["/usr/bin/lipo", "-verify_arch", "arm64", str(native / "src/airconv/darwin/libairconv.a")], "airconv-archive")
        checked(["/usr/bin/lipo", "-verify_arch", "arm64", str(native / "libs/DXBCParser/libDXBCParserNative.a")], "dxbc-parser-archive")
        checked([sys.executable, str(root / "scripts/test-owned-null-descriptor.py"), "--build-root", str(native),
                 "--native-arch", "arm64", "--bridge", str(bridge), "--output", str(output / "gpu")], "native-gpu")
        gpu_log = (output / "gpu/gpu.log").read_text()
        if (f"compiler_image={bridge}" not in gpu_log or
                "Metal API Validation Enabled" not in gpu_log or "Metal GPU Validation Enabled" not in gpu_log):
            raise RuntimeError("GPU log does not prove actual bridge image and both validation layers")
        native_artifact(output / "gpu/null-descriptor-probe")
        result["native_gpu"] = "pass"

        # Full production graph first: never substitute the toolchain probe for this attempt.
        configurations = (("arm64", "build-arm64.txt"),
                          ("arm64ec", "build-arm64ec-only.txt"))
        wine_option = ("-Dwine_build_path=" + str(args.wine_build.resolve()) if args.wine_build
                       else "-Dwine_install_path=" + str(sdk))
        for abi, cross in configurations:
            build = output / ("windows-" + abi)
            selected_wine = ("-Dwine_build_path=" + str(args.wine_ec_build.resolve())
                             if abi == "arm64ec" and args.wine_ec_build else wine_option)
            setup = run([meson, "setup", str(build), str(root), "--cross-file", str(root / cross),
                         "--native-file", str(root / "build-native-arm64.txt"),
                         "-Dnative_arch=arm64", "-Dnative_llvm_path=" + os.path.relpath(llvm, root), selected_wine,
                         "-Denable_d3d12=true", "-Dwine_builtin_dll=true"], "windows-" + abi + "-setup")
            row = {"requested_windows_abi": abi, "setup": setup,
                   "full_targets": ["d3d12.dll", "d3d12core.dll", "dxgi.dll", "winemetal.dll", "winemetal.so"],
                   "full_translator_build": "blocked", "guest_runtime": "not_run"}
            if not setup["returncode"]:
                built = run([ninja, "-C", str(build), "-j", str(args.jobs), "src/d3d12/d3d12.dll.postproc",
                             "src/d3d12/d3d12core.dll",
                             "src/dxgi/dxgi.dll.postproc", "src/winemetal/winemetal.dll.postproc", "src/winemetal/unix/winemetal.so"],
                            "windows-" + abi + "-build")
                row["build"] = built
                if not built["returncode"]:
                    row["full_translator_build"] = "pass"
                    row["pe_artifacts"] = []
                    for relative in ("src/d3d12/d3d12.dll", "src/d3d12/d3d12core.dll",
                                     "src/dxgi/dxgi.dll", "src/winemetal/winemetal.dll"):
                        image = build / relative
                        facts = inspect_pe(image)
                        if facts["windows_abi"] != abi:
                            raise RuntimeError(f"actual PE ABI differs from requested {abi}: {image}: {facts}")
                        row["pe_artifacts"].append({"path": str(image), "sha256": hashlib.sha256(image.read_bytes()).hexdigest(), **facts})
                        row["pe_artifacts"][-1]["readobj"] = checked([str(mingw / "bin/llvm-readobj"),
                            "--file-headers", "--coff-load-config", "--coff-imports", "--coff-exports", str(image)],
                            "windows-" + abi + "/" + image.name + "-readobj")
                    native_artifact(build / "src/winemetal/unix/winemetal.so")
            if row["full_translator_build"] == "pass":
                row["pe_only"] = {"production_pe_build": "pass", "scope": "already proven by full build; no duplicate PE compilation",
                                  "artifacts": row["pe_artifacts"], "guest_runtime": "not_run"}
                result["windows"].append(row)
                continue
            failure = row.get("build", setup)
            result["blockers"].append(f"full {abi} translator build blocked; inspect {failure['log']}")
            # PE production targets have no dependency on a native Unix window driver.
            # Preserve the strict full-build result and prove these artifacts independently.
            pe_build = output / ("pe-" + abi)
            pe_setup = run([meson, "setup", str(pe_build), str(root), "--cross-file", str(root / cross),
                            "--native-file", str(root / "build-native-arm64.txt"), "-Dnative_arch=arm64",
                            "-Dbuild_native_components=false", selected_wine, "-Denable_d3d12=true",
                            "-Dwine_builtin_dll=true"], "pe-" + abi + "-setup")
            row["pe_only"] = {"setup": pe_setup, "production_pe_build": "blocked", "guest_runtime": "not_run"}
            if not pe_setup["returncode"]:
                relatives = ("src/d3d12/d3d12.dll", "src/d3d12/d3d12core.dll",
                             "src/dxgi/dxgi.dll", "src/winemetal/winemetal.dll")
                pe_built = run([ninja, "-C", str(pe_build), "-j", str(args.jobs),
                                *[relative if relative.endswith("/d3d12core.dll") else relative + ".postproc"
                                  for relative in relatives]], "pe-" + abi + "-build")
                row["pe_only"]["build"] = pe_built
                if not pe_built["returncode"]:
                    artifacts = []
                    for relative in relatives:
                        image = pe_build / relative
                        facts = inspect_pe(image)
                        if facts["windows_abi"] != abi:
                            raise RuntimeError(f"actual production PE ABI differs from {abi}: {image}: {facts}")
                        artifacts.append({"path": str(image), "sha256": hashlib.sha256(image.read_bytes()).hexdigest(), **facts})
                        artifacts[-1]["readobj"] = checked([str(mingw / "bin/llvm-readobj"),
                            "--file-headers", "--coff-load-config", "--coff-imports", "--coff-exports", str(image)],
                            "pe-" + abi + "/" + image.name + "-readobj")
                    row["pe_only"].update(production_pe_build="pass", artifacts=artifacts)
            if row["pe_only"]["production_pe_build"] != "pass":
                failure = row["pe_only"].get("build", pe_setup)
                result["blockers"].append(f"production {abi} PE build blocked; inspect {failure['log']}")
            result["windows"].append(row)

        # Hybrid DLLs need two complete production/CRT halves; an EC-only
        # executable check or a /noentry metadata fixture cannot prove them.
        wine_source = (args.wine_source.resolve() if args.wine_source else
                       args.wine_build.resolve().parent / "wine-source" if args.wine_build else
                       sdk / "wine-source")
        paired = output / "paired-arm64x"
        hybrid = run([sys.executable, str(root / "scripts/build-owned-arm64x.py"),
                      "--output", str(paired), "--wine-source", str(wine_source),
                      "--llvm-mingw", str(mingw), "--jobs", str(args.jobs)], "paired-arm64x-build")
        result["commands"].append(hybrid)
        if hybrid["returncode"]:
            result["blockers"].append(f"paired ARM64X production build failed: {hybrid['log']}")
        else:
            evidence = json.loads((paired / "result.json").read_text())
            result["windows"].append({"requested_windows_abi": "arm64x", "paired_build": hybrid,
                                      "pe_only": {"production_pe_build": "pass", "artifacts": evidence["artifacts"]},
                                      "guest_runtime": "not_run", "evidence": str(paired / "result.json")})
    except (OSError, ValueError, RuntimeError) as error:
        result["blockers"].append(str(error))
    result["status"] = "pass" if not result["blockers"] else "blocked"
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "native_gpu": result["native_gpu"], "result": str(output / "result.json"),
                      "blockers": result["blockers"]}, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
