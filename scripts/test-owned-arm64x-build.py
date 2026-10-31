#!/usr/bin/env python3
"""CPU selection tests and optional real Meson graph/NGX bridge checks."""
from __future__ import annotations

import argparse
import ast
import copy
import hashlib
import importlib.util
import itertools
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("owned_arm64x", ROOT / "scripts/build-owned-arm64x.py")
assert SPEC and SPEC.loader
paired = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(paired)
SDK_ROOT = None
LLVM_MINGW = Path("/opt/llvm-mingw-20260616-ucrt-macos-universal")


class PairedFrontendBuildTests(unittest.TestCase):
    def test_external_sdk_rejects_incomplete_tampered_and_wrong_abi_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source = root / "source"
            source.mkdir()
            (source / "configure").write_text("snapshot")
            manifest = root / "source-cut.json"
            manifest.write_text(json.dumps({"configure": hashlib.sha256((source / "configure").read_bytes()).hexdigest()}))
            result = {"schema": 1, "kind": "paired-wine-sdk", "status": "pass", "variants": {},
                      "source": {"path": str(source), "manifest": str(manifest),
                                 "sha256": hashlib.sha256(manifest.read_bytes()).hexdigest()}}
            for abi, target, windows in (("arm64", "aarch64", "aarch64-windows"),
                                          ("arm64ec", "arm64ec", "arm64ec-windows")):
                build = root / ("wine-" + abi)
                artifacts = []
                for relative in ("tools/winebuild/winebuild", f"libs/winecrt0/{windows}/libwinecrt0.a",
                                 f"dlls/ntdll/{windows}/libntdll.a", f"dlls/dbghelp/{windows}/libdbghelp.a"):
                    path = build / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(struct.pack("<II", 0xfeedfacf, 0x100000c) + bytes(24)
                                     if path.name == "winebuild" else abi.encode())
                    artifacts.append({"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
                result["variants"][abi] = {"status": "pass", "build_path": str(build), "native_arch": "arm64",
                                            "windows_abi": abi, "windows_arches": target, "artifacts": artifacts}

            def headers(command, **kwargs):
                abi = "ARM64EC" if "wine-arm64ec" in command[-1] else "ARM64"
                crt = command[-1].endswith("libwinecrt0.a")
                formats = "Format: COFF-" + abi if crt else "Format: COFF-ARM64\nFormat: COFF-import-file-" + abi
                return subprocess.CompletedProcess(command, 0, formats + "\nMachine: IMAGE_FILE_MACHINE_" + (abi if crt else "ARM64"))

            with patch.object(paired.subprocess, "run", side_effect=headers):
                (root / "sdk-result.json").write_text(json.dumps(result))
                self.assertEqual(paired.validate_wine_sdk(root, LLVM_MINGW / "bin/llvm-readobj"), result)
                for mutate in (
                    lambda row: row.update(status="building"),
                    lambda row: row["source"].update(sha256="0" * 64),
                    lambda row: row["variants"].pop("arm64ec"),
                    lambda row: row["variants"]["arm64ec"].update(windows_abi="arm64"),
                    lambda row: row["variants"]["arm64"]["artifacts"].pop(),
                    lambda row: row["variants"]["arm64"]["artifacts"][0].update(sha256="0" * 64),
                ):
                    invalid = copy.deepcopy(result)
                    mutate(invalid)
                    (root / "sdk-result.json").write_text(json.dumps(invalid))
                    with self.assertRaises(ValueError):
                        paired.validate_wine_sdk(root, LLVM_MINGW / "bin/llvm-readobj")
                (root / "sdk-result.json").write_text(json.dumps(result))
                with patch.object(paired.subprocess, "run", return_value=subprocess.CompletedProcess([], 0,
                                  "Machine: IMAGE_FILE_MACHINE_AMD64")):
                    with self.assertRaisesRegex(ValueError, "COFF archive"):
                        paired.validate_wine_sdk(root, LLVM_MINGW / "bin/llvm-readobj")
                def wrong_short_import(command, **kwargs):
                    row = headers(command, **kwargs)
                    row.stdout = row.stdout.replace("COFF-import-file-ARM64EC", "COFF-import-file-ARM64")
                    return row
                with patch.object(paired.subprocess, "run", side_effect=wrong_short_import):
                    with self.assertRaisesRegex(ValueError, "COFF archive"):
                        paired.validate_wine_sdk(root, LLVM_MINGW / "bin/llvm-readobj")
                (source / "configure").write_text("changed")
                with self.assertRaisesRegex(ValueError, "source snapshot changed"):
                    paired.validate_wine_sdk(root, LLVM_MINGW / "bin/llvm-readobj")
                (source / "configure").write_text("snapshot")
                archive = Path(result["variants"]["arm64"]["artifacts"][1]["path"])
                archive.write_bytes(b"tampered")
                with self.assertRaisesRegex(ValueError, "hash mismatch"):
                    paired.validate_wine_sdk(root, LLVM_MINGW / "bin/llvm-readobj")
                archive.write_bytes(b"arm64")
                helper = Path(result["variants"]["arm64"]["artifacts"][0]["path"])
                helper.write_bytes(struct.pack("<II", 0xfeedfacf, 0x1000007) + bytes(24))
                result["variants"]["arm64"]["artifacts"][0]["sha256"] = hashlib.sha256(helper.read_bytes()).hexdigest()
                (root / "sdk-result.json").write_text(json.dumps(result))
                with self.assertRaisesRegex(ValueError, "thin ARM64"):
                    paired.validate_wine_sdk(root, LLVM_MINGW / "bin/llvm-readobj")

    def test_cli_and_orchestrators_only_consume_external_sdk(self):
        for name in ("build-owned-arm64x.py", "build-owned-arm64-foundation.py"):
            tree = ast.parse((ROOT / "scripts" / name).read_text())
            main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "main")
            literals = [node.value for node in ast.walk(main) if isinstance(node, ast.Constant)]
            self.assertIn("--wine-sdk", literals)
            for forbidden in ("--wine-source", "make", "configure", "build-paired-sdk.py"):
                self.assertNotIn(forbidden, literals)
            sdk_argument = next(node for node in ast.walk(main) if isinstance(node, ast.Call)
                                and isinstance(node.func, ast.Attribute) and node.func.attr == "add_argument"
                                and node.args and isinstance(node.args[0], ast.Constant) and node.args[0].value == "--wine-sdk")
            self.assertTrue(next(keyword.value.value for keyword in sdk_argument.keywords if keyword.arg == "required"))

    def test_explicit_selections_preserve_defaults_and_include_both_fsr_frontends(self):
        expected = {
            "nvapi": {"nvapi64": "src/nvapi/nvapi64.dll"},
            "nvngx": {"nvngx": "src/nvngx/nvngx.dll"},
            "fsr": {
                "amd_fidelityfx_upscaler_dx12": "src/fsr/amd_fidelityfx_upscaler_dx12.dll",
                "amd_fidelityfx_framegeneration_dx12": "src/fsrfg/amd_fidelityfx_framegeneration_dx12.dll",
                "amd_fidelityfx_denoiser_dx12": "src/fsrrr/amd_fidelityfx_denoiser_dx12.dll",
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
        sdk_result = paired.validate_wine_sdk(SDK_ROOT, LLVM_MINGW / "bin/llvm-readobj")
        with tempfile.TemporaryDirectory(prefix="owned-arm64x-config-") as directory:
            for abi, cross, windows, machine in (
                ("arm64", "build-arm64.txt", "aarch64-windows", "IMAGE_FILE_MACHINE_ARM64"),
                ("arm64ec", "build-arm64ec-only.txt", "arm64ec-windows", "IMAGE_FILE_MACHINE_ARM64EC"),
            ):
                with self.subTest(abi=abi):
                    build = Path(directory).resolve() / abi
                    run("meson", "setup", str(build), str(ROOT), "--cross-file", str(ROOT / cross),
                        "--native-file", str(ROOT / "build-native-arm64.txt"), "-Dnative_arch=arm64",
                        "-Dbuild_native_components=false", "-Dwine_build_path=" + sdk_result["variants"][abi]["build_path"],
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
