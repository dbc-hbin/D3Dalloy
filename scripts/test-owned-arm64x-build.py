#!/usr/bin/env python3
"""CPU selection tests and optional real Meson graph/NGX bridge checks."""
from __future__ import annotations

import argparse
import importlib.util
import itertools
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("owned_arm64x", ROOT / "scripts/build-owned-arm64x.py")
assert SPEC and SPEC.loader
paired = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(paired)
SDK_ROOT = None
LLVM_MINGW = Path("/opt/llvm-mingw-20260616-ucrt-macos-universal")


class PairedFrontendBuildTests(unittest.TestCase):
    def test_explicit_selections_preserve_defaults_and_include_both_fsr_frontends(self):
        expected = {
            "nvapi": {"nvapi64": "src/nvapi/nvapi64.dll"},
            "nvngx": {"nvngx": "src/nvngx/nvngx.dll"},
            "fsr": {
                "amd_fidelityfx_upscaler_dx12": "src/fsr/amd_fidelityfx_upscaler_dx12.dll",
                "amd_fidelityfx_framegeneration_dx12": "src/fsrfg/amd_fidelityfx_framegeneration_dx12.dll",
            },
        }
        for flags in itertools.product((False, True), repeat=3):
            options = dict(zip(("enable_nvapi", "enable_nvngx", "enable_fsr"), flags))
            with self.subTest(options=options):
                modules, actual_options = paired.build_selection(argparse.Namespace(**options))
                self.assertEqual(actual_options, options)
                additions = {module: path for frontend, rows in expected.items()
                             if options["enable_" + frontend] for module, path in rows.items()}
                self.assertEqual(modules, paired.MODULES | additions)
        self.assertEqual(len(paired.MODULES), 6)

    def test_real_paired_graphs_install_contract_and_msvc_bridge_abi(self):
        if SDK_ROOT is None:
            self.skipTest("pass --sdk-root for real Meson configuration checks")
        environment = os.environ | {
            "PATH": os.pathsep.join((str(LLVM_MINGW / "bin"), str(ROOT / "toolchains/tools/bin"), os.environ["PATH"])),
        }

        def run(*command: str) -> str:
            result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout

        modules, options = paired.build_selection(argparse.Namespace(enable_nvapi=True, enable_nvngx=True, enable_fsr=True))
        with tempfile.TemporaryDirectory(prefix="owned-arm64x-config-") as directory:
            for abi, cross, windows, machine in (
                ("arm64", "build-arm64.txt", "aarch64-windows", "IMAGE_FILE_MACHINE_ARM64"),
                ("arm64ec", "build-arm64ec-only.txt", "arm64ec-windows", "IMAGE_FILE_MACHINE_ARM64EC"),
            ):
                with self.subTest(abi=abi):
                    build = Path(directory).resolve() / abi
                    run("meson", "setup", str(build), str(ROOT), "--cross-file", str(ROOT / cross),
                        "--native-file", str(ROOT / "build-native-arm64.txt"), "-Dnative_arch=arm64",
                        "-Dbuild_native_components=false", "-Dwine_build_path=" + str(SDK_ROOT / ("wine-" + abi)),
                        "-Denable_d3d12=true", "-Dwine_builtin_dll=true", "--buildtype=debugoptimized",
                        *["-D" + name + "=" + str(enabled).lower() for name, enabled in options.items()])
                    installed = json.loads(run("meson", "introspect", "--installed", str(build)))
                    for module, relative in modules.items():
                        graph = run("ninja", "-C", str(build), "-t", "commands", relative)
                        self.assertIn("-shared", graph.splitlines()[-1])
                        self.assertIn(relative, graph.splitlines()[-1])
                        target = relative if module == "d3d12core" else relative + ".postproc"
                        run("ninja", "-C", str(build), "-t", "commands", target)
                        self.assertIn(str(Path(windows) / (module + ".dll")), installed[str(build / relative)])
                        if module == "nvngx":
                            self.assertIn("nvngx_d3d12_msvc.obj", graph.splitlines()[-1])
                    # Only this tiny ABI object is compiled, never the full graphics build.
                    run("ninja", "-C", str(build), "src/nvngx/nvngx_d3d12_msvc.obj")
                    headers = run(str(LLVM_MINGW / "bin/llvm-readobj"), "--file-headers",
                                  str(build / "src/nvngx/nvngx_d3d12_msvc.obj"))
                    self.assertIn(machine, headers)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", type=Path, help="existing paired Wine SDK root, read-only")
    parser.add_argument("--llvm-mingw", type=Path, default=LLVM_MINGW)
    args, remaining = parser.parse_known_args()
    SDK_ROOT = args.sdk_root.resolve() if args.sdk_root else None
    LLVM_MINGW = args.llvm_mingw.resolve()
    unittest.main(argv=[__file__, *remaining])
