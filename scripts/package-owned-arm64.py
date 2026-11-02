#!/usr/bin/env python3
"""Build canonical plain ARM64 owned graphics and stage an audited native Wine cut."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wine-build", type=Path, required=True, help="completed Wine-darwin runtime build output")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--paired-build", type=Path, help="accepted build-owned-arm64x.py production closure for an ARM64X runtime")
    parser.add_argument("--fex-build", type=Path, help="accepted FEX-darwin Darwin/build.py output")
    parser.add_argument("--with-wow64", action="store_true", help="compile owned PE32 frontends against this native Wine SDK")
    parser.add_argument("--with-nvapi", action="store_true", help="build and stage the owned NVAPI frontend")
    parser.add_argument("--with-nvngx", action="store_true", help="build and stage the owned NGX frontend")
    parser.add_argument("--with-fsr", action="store_true", help="build and stage owned FSR upscaling and frame generation")
    parser.add_argument("--llvm-mingw", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal"))
    parser.add_argument("--native-llvm", type=Path, default=Path("toolchains/llvm-15-arm64/llvm@15/15.0.7"))
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    optional = {"nvapi": args.with_nvapi, "nvngx": args.with_nvngx, "fsr": args.with_fsr}
    feature_options = [f"-Denable_{name}={str(enabled).lower()}" for name, enabled in optional.items()]
    root = Path(__file__).resolve().parents[1]
    output, wine = args.output.resolve(), args.wine_build.resolve()
    if args.jobs < 1 or output.exists() or not output.is_relative_to(root / "build/local"):
        parser.error("output must be fresh below build/local and jobs positive")
    wine_result = json.loads((wine / "runtime-result.json").read_text())
    windows_abi = wine_result.get("windows_abi", "arm64")
    if wine_result["status"] != "pass" or wine_result["native_arch"] != "arm64" or wine_result["windows_arch"] != "aarch64":
        parser.error("Wine build must be a completed native/plain ARM64 runtime")
    if (windows_abi == "arm64x") != bool(args.paired_build):
        parser.error("ARM64X Wine requires the accepted paired owned build")
    for identity in wine_result["identities"]:
        if hashlib.sha256(Path(identity["path"]).read_bytes()).hexdigest() != identity["sha256"]:
            parser.error("Wine runtime changed since builder acceptance")
    output.mkdir(parents=True)
    logs = output / "logs"
    logs.mkdir()
    owned, install, staged = (output / name for name in ("owned-build", "owned-install", "runtime"))
    paths = [path for directory in ("src", "libs", "include") for path in (root / directory).rglob("*") if path.is_file()]
    paths.extend(root / name for name in ("meson.build", "meson.options", "build-arm64.txt", "build-native-arm64.txt", "version.h.in"))
    if args.with_wow64:
        paths.append(root / "build-win32.txt")
    if args.paired_build:
        paths.append(root / "build-arm64ec-only.txt")
    source_cut = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    (output / "source-cut.json").write_text(json.dumps(source_cut, indent=2, sort_keys=True) + "\n")
    environment = os.environ | {"PATH": os.pathsep.join((str(args.llvm_mingw.resolve() / "bin"),
                                str(root / "toolchains/tools/bin"), os.environ["PATH"])),
                                "WINEPREFIX": str(output / "wrapper-prefix")}
    result = {"schema": 1, "status": "building", "wine_build": str(wine), "runtime": str(staged),
              "wine_source_commit": wine_result["source_commit"], "wine_archive_sha256": wine_result["source_archive_sha256"],
              "wine_build_result_sha256": hashlib.sha256((wine / "runtime-result.json").read_bytes()).hexdigest(),
              "owned_source_cut": str(output / "source-cut.json"), "commands": [],
              "scope": "packaged plain ARM64/native Wine and owned graphics; PE execution is a separate gate"}

    def run(command: list[str], name: str) -> str:
        log = logs / (name + ".log")
        with log.open("w") as stream:
            process = subprocess.run(command, cwd=root, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        result["commands"].append({"command": command, "returncode": process.returncode, "log": str(log)})
        (output / "package-result.json").write_text(json.dumps(result, indent=2) + "\n")
        if process.returncode:
            raise RuntimeError(f"{name} failed ({process.returncode}): {log}")
        return log.read_text()

    try:
        llvm_path = args.native_llvm.resolve()
        llvm_option = str(llvm_path.relative_to(root)) if llvm_path.is_relative_to(root) else str(llvm_path)
        wine_sdk = wine / "wine-build"
        wine_option = str(wine_sdk.relative_to(root)) if wine_sdk.is_relative_to(root) else str(wine_sdk)
        run(["meson", "setup", str(owned), str(root), "--cross-file", str(root / "build-arm64.txt"),
             "--native-file", str(root / "build-native-arm64.txt"), "--prefix", str(install),
             "-Dnative_arch=arm64", "-Dnative_llvm_path=" + llvm_option,
             "-Dwine_build_path=" + wine_option, "-Denable_d3d12=true",
             "-Dwine_builtin_dll=true", *feature_options, "--buildtype=debugoptimized"], "owned-setup")
        run(["ninja", "-C", str(owned), "-j" + str(args.jobs)], "owned-build")
        run(["meson", "install", "-C", str(owned), "--no-rebuild"], "owned-install")
        if args.with_wow64:
            owned32 = output / "owned-build32"
            install32 = output / "owned-install32"
            run(["meson", "setup", str(owned32), str(root), "--cross-file", str(root / "build-win32.txt"),
                 "--native-file", str(root / "build-native-arm64.txt"), "--prefix", str(install32),
                 "-Dnative_arch=arm64", "-Dbuild_native_components=false",
                 "-Dwine_build_path=" + wine_option, "-Denable_d3d12=true",
                 "-Dwine_builtin_dll=true", *feature_options, "--buildtype=debugoptimized"], "owned32-setup")
            run(["ninja", "-C", str(owned32), "-j" + str(args.jobs)], "owned32-build")
            run(["meson", "install", "-C", str(owned32), "--no-rebuild"], "owned32-install")
            result["owned_install32"] = str(install32)
        if any(hashlib.sha256((root / name).read_bytes()).hexdigest() != digest for name, digest in source_cut.items()):
            raise RuntimeError("owned sources changed during build; refusing a mixed source cut")
        if args.paired_build:
            paired = args.paired_build.resolve()
            evidence = json.loads((paired / "result.json").read_text())
            paired_cut = json.loads(Path(evidence["owned_source_cut"]).read_text())
            if (evidence["status"] != "pass" or evidence["windows_abi"] != "arm64x" or
                    any(source_cut.get(name) != digest for name, digest in paired_cut.items())):
                raise RuntimeError("paired DLLs must match the accepted owned source cut")
            required_modules = {"amdxc64.dll"}
            if args.with_nvapi:
                required_modules.add("nvapi64.dll")
            if args.with_nvngx:
                required_modules.add("nvngx.dll")
            if args.with_fsr:
                required_modules.update(("amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll",
                                         "amd_fidelityfx_denoiser_dx12.dll"))
            if not required_modules.issubset(Path(artifact["path"]).name for artifact in evidence["artifacts"]):
                raise RuntimeError("paired build is missing required or selected frontends")
            for artifact in evidence["artifacts"]:
                image = Path(artifact["path"])
                if hashlib.sha256(image.read_bytes()).hexdigest() != artifact["sha256"]:
                    raise RuntimeError("paired DLL changed since build acceptance")
                shutil.copy2(image, install / "aarch64-windows" / image.name)
            result["paired_build_result_sha256"] = hashlib.sha256((paired / "result.json").read_bytes()).hexdigest()
        run([sys.executable, str(root / "scripts/stage-owned-graphics.py"), "stage", "--d3dalloy-install", str(install),
             "--wine-source", str(wine / "runtime"), "--wine-dest", str(staged),
             "--native-arch", "arm64", "--windows-abi", windows_abi,
             *[f"--with-{name}" for name, enabled in optional.items() if enabled],
             *(["--d3dalloy-install32", str(install32)] if args.with_wow64 else []),
             *(["--fex-build", str(args.fex_build.resolve())] if args.fex_build else [])], "stage")
        run([sys.executable, str(root / "scripts/stage-owned-graphics.py"), "verify", "--runtime", str(staged)], "verify")
        result["wrapper_version"] = run([str(staged / "bin/wine"), "--version"], "wrapper-version").strip()
        result["owned_install"] = str(install)
        result["status"] = "pass"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result["status"] = "blocked"
        result["blocker"] = str(error)
    (output / "package-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
