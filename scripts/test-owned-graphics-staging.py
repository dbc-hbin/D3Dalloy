#!/usr/bin/env python3
"""CPU-only tests for stage-owned-graphics.py path and publication policy."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from owned_graphics_arch import architecture, execution_architecture, inspect_pe, runtime_architecture


SCRIPT = Path(__file__).with_name("stage-owned-graphics.py")
SPEC = importlib.util.spec_from_file_location("stage_owned_graphics", SCRIPT)
assert SPEC and SPEC.loader
stage_owned_graphics = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage_owned_graphics)


class StagingPolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.wine = self.root / "wine-source"
        self.dxmt = self.root / "dxmt-install"
        self.dest = self.root / "runtime-new"
        (self.wine / "bin").mkdir(parents=True)
        (self.wine / stage_owned_graphics.WINDOWS_DIR).mkdir(parents=True)
        (self.wine / stage_owned_graphics.UNIX_DIR).mkdir(parents=True)
        macho = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x1000007, 3, 2, 0, 0, 0, 0)
        (self.wine / "bin/wine").write_bytes(macho)
        (self.wine / "bin/wineserver").write_bytes(macho)
        (self.wine / stage_owned_graphics.UNIX_DIR / "ntdll.so").write_bytes(macho)
        (self.wine / stage_owned_graphics.WINDOWS_DIR / "ntdll.dll").write_bytes(self.pe_image(0x8664, b"wine"))
        (self.wine / "bin/wine").chmod(0o755)
        (self.dxmt / stage_owned_graphics.DXMT_WINDOWS_DIR).mkdir(parents=True)
        (self.dxmt / stage_owned_graphics.DXMT_UNIX_DIR).mkdir(parents=True)
        for name in stage_owned_graphics.WINDOWS_MODULES:
            payload = b"MZ-" + name.encode() + b"-" + stage_owned_graphics.WINE_BUILTIN_MARKER
            if name == "d3d12.dll":
                payload += b"-" + stage_owned_graphics.OWNED_D3D12_MARKER
            image = self.core_image(abi="x64", main=name == "d3d12.dll") if name in ("d3d12.dll", "d3d12core.dll") else self.pe_image(0x8664, payload)
            (self.dxmt / stage_owned_graphics.DXMT_WINDOWS_DIR / name).write_bytes(image)
        (self.dxmt / stage_owned_graphics.DXMT_UNIX_DIR / "winemetal.so").write_bytes(macho)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_mesons_install_layout_resolves_to_wine_layout(self) -> None:
        artifacts = stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt)
        self.assertEqual(
            set(artifacts),
            set(stage_owned_graphics.TRANSLATOR_ARTIFACTS),
        )
        self.assertEqual(
            artifacts[stage_owned_graphics.UNIX_DIR / "winemetal.so"],
            self.dxmt / "x86_64-unix/winemetal.so",
        )

    def test_destination_must_not_exist_or_overlap_inputs(self) -> None:
        stage_owned_graphics.validate_destination(self.dest, self.wine, self.dxmt)
        self.dest.mkdir()
        with self.assertRaisesRegex(ValueError, "new path"):
            stage_owned_graphics.validate_destination(self.dest, self.wine, self.dxmt)
        overlap = self.wine / "private-runtime"
        with self.assertRaisesRegex(ValueError, "contain one another"):
            stage_owned_graphics.validate_destination(overlap, self.wine, self.dxmt)

    def test_destination_symlink_and_symlink_parent_are_refused(self) -> None:
        link = self.root / "runtime-link"
        link.symlink_to(self.root / "somewhere")
        with self.assertRaisesRegex(ValueError, "symlink"):
            stage_owned_graphics.validate_destination(link, self.wine, self.dxmt)
        real_parent = self.root / "real-parent"
        real_parent.mkdir()
        parent_link = self.root / "parent-link"
        parent_link.symlink_to(real_parent, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "parent path"):
            stage_owned_graphics.validate_destination(parent_link / "runtime", self.wine, self.dxmt)

    def test_runtime_symlink_target_is_never_overwritten(self) -> None:
        staged = self.root / "staged"
        target_dir = staged / stage_owned_graphics.WINDOWS_DIR
        target_dir.mkdir(parents=True)
        outside = self.root / "outside.dll"
        outside.write_bytes(b"outside")
        target = target_dir / "d3d12.dll"
        target.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "symlink"):
            stage_owned_graphics.ensure_replaceable_target(target, staged)
        self.assertEqual(outside.read_bytes(), b"outside")

    def test_unowned_d3d12_is_refused(self) -> None:
        (self.dxmt / "x86_64-windows/d3d12.dll").write_bytes(b"MZ-plain-dxmt")
        with self.assertRaisesRegex(ValueError, "owned backend marker"):
            stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt)

    def test_optional_nvngx_and_nvapi_require_validated_exports(self) -> None:
        nvngx = self.dxmt / "x86_64-windows/nvngx.dll"
        nvapi = self.dxmt / "x86_64-windows/nvapi64.dll"
        nvngx.write_bytes(self.pe_image(0x8664, stage_owned_graphics.OWNED_NGX_D3D12_MARKER))
        nvapi.write_bytes(self.pe_image(0x8664, b"nvapi"))

        original_exports = stage_owned_graphics.pe_exports
        def exports(path: Path, **kwargs) -> dict[str, int]:
            if path.name == "nvngx.dll":
                return dict.fromkeys(stage_owned_graphics.REQUIRED_NGX_D3D12_EXPORTS, 1)
            if path.name == "nvapi64.dll":
                return dict.fromkeys(stage_owned_graphics.REQUIRED_NVAPI_EXPORTS, 2)
            return original_exports(path, **kwargs)

        with mock.patch.object(stage_owned_graphics, "pe_exports", side_effect=exports):
            artifacts = stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt, True, True)
        self.assertEqual(artifacts[stage_owned_graphics.WINDOWS_DIR / "nvngx.dll"], nvngx)
        self.assertEqual(artifacts[stage_owned_graphics.WINDOWS_DIR / "nvapi64.dll"], nvapi)

    def test_optional_nvngx_refuses_missing_d3d12_export(self) -> None:
        nvngx = self.dxmt / "x86_64-windows/nvngx.dll"
        nvngx.write_bytes(b"MZ-fixture-" + stage_owned_graphics.OWNED_NGX_D3D12_MARKER)
        incomplete = dict.fromkeys(stage_owned_graphics.REQUIRED_NGX_D3D12_EXPORTS, 1)
        del incomplete["NVSDK_NGX_D3D12_EvaluateFeature"]
        with mock.patch.object(stage_owned_graphics, "pe_exports", return_value=incomplete):
            with self.assertRaisesRegex(ValueError, "missing required D3D12 exports"):
                stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt, True, False)

    def fsr_image(self, marker: bytes = stage_owned_graphics.OWNED_FSR_MARKER, abi: str = "x64",
                  exports: tuple[str, ...] = ("ffxConfigure", "ffxCreateContext", "ffxDestroyContext", "ffxDispatch", "ffxQuery")) -> bytearray:
        machine = {"x86": 0x14C, "x64": 0x8664, "arm64": 0xAA64, "arm64ec": 0x8664}[abi]
        image = bytearray(self.pe_image(machine, bytes(0x1000)))
        directories = 0x58 + (96 if abi == "x86" else 112)
        struct.pack_into("<I", image, directories - 4, 16)
        struct.pack_into("<II", image, directories, 0x1100, 0x300)
        if abi == "arm64ec":
            struct.pack_into("<Q", image, 0x58 + 24, 0x180000000)
            struct.pack_into("<II", image, directories + 80, 0x1800, 232)
            struct.pack_into("<I", image, 0xA00, 232)
            struct.pack_into("<Q", image, 0xA00 + 200, 0x180001900)
            struct.pack_into("<III", image, 0xB00, 2, 0x1A00, 1)
            struct.pack_into("<II", image, 0xC00, 0x1501, 16)
        struct.pack_into("<IIIIII", image, 0x300 + 16, 1, len(exports), len(exports), 0x1140, 0x1160, 0x1180)
        cursor = 0x3A0
        for index, name in enumerate(exports):
            ordinal = index + 1
            struct.pack_into("<I", image, 0x340 + (ordinal - 1) * 4, 0x1500 + ordinal)
            struct.pack_into("<I", image, 0x360 + index * 4, cursor + 0xE00)
            struct.pack_into("<H", image, 0x380 + index * 2, ordinal - 1)
            encoded = name.encode() + b"\0"
            image[cursor:cursor + len(encoded)] = encoded
            cursor += len(encoded)
        marker = marker + b"\0" + stage_owned_graphics.WINE_BUILTIN_MARKER
        image[0x800:0x800 + len(marker)] = marker
        return image

    def display_build(self) -> Path:
        source = self.root / "wine-display-source"
        (source / "dlls/dcomp").mkdir(parents=True)
        device = source / "dlls/dcomp/device.c"
        device.write_text("original source\n")
        subprocess.run(["git", "init", "-q", str(source)], check=True)
        subprocess.run(["git", "-C", str(source), "add", "dlls/dcomp"], check=True)
        subprocess.run(["git", "-C", str(source), "-c", "user.name=Staging test",
                        "-c", "user.email=staging@example.invalid", "commit", "-qm", "base"], check=True)
        pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        for name, value in (("WINE_DISPLAY_SOURCE", source), ("WINE_DISPLAY_SOURCE_PIN", pin)):
            patch = mock.patch.object(stage_owned_graphics, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        device.write_text("updated source\n")
        build = self.root / "wine-display-build"
        build.mkdir()
        (build / "Makefile").write_text(f"srcdir = {source}\n")
        for abi, directory in (("x64", "x86_64-windows"), ("x86", "i386-windows")):
            module = build / "dlls/dcomp" / directory / "dcomp.dll"
            module.parent.mkdir(parents=True)
            module.write_bytes(self.dcomp_image(abi))
            os.utime(module, ns=(device.stat().st_mtime_ns + 1, device.stat().st_mtime_ns + 1))
        return build

    def dcomp_image(self, abi: str = "x64") -> bytearray:
        image = self.fsr_image(b"", abi, tuple(sorted(stage_owned_graphics.REQUIRED_DCOMP_EXPORTS)))
        image[0x80:0x200] = image[0x40:0x1C0]
        struct.pack_into("<I", image, 0x3C, 0x80)
        image[64:96] = stage_owned_graphics.WINE_BUILTIN_MARKER.ljust(32, b"\0")
        return image

    def test_display_inputs_reject_missing_wrong_abi_fake_signature_and_exports_before_copy(self) -> None:
        build = self.display_build()
        module = build / "dlls/dcomp/x86_64-windows/dcomp.dll"
        valid = module.read_bytes()
        bad_signature = bytearray(valid)
        bad_signature[64:96] = stage_owned_graphics.WINE_FAKE_MARKER.ljust(32, b"\0")
        missing_export = bytearray(valid)
        missing_export[0x3A0] = ord("X")
        cases = ((None, "regular non-symlink"), (self.dcomp_image("x86"), "ABI mismatch"),
                 (bad_signature, "builtin image signature"), (missing_export, "missing required public exports"))
        for image, error in cases:
            with self.subTest(error=error), mock.patch.object(stage_owned_graphics.shutil, "copytree") as copy:
                if image is None:
                    module.unlink()
                else:
                    module.write_bytes(image)
                with self.assertRaisesRegex(ValueError, error):
                    stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, wine_display_build=build)
                copy.assert_not_called()
                self.assertFalse(self.dest.exists())
                self.assertEqual(list(self.root.glob("runtime-new.staging-*")), [])

    def test_display_build_rejects_wrong_source_pin_pristine_and_stale_outputs(self) -> None:
        build = self.display_build()
        with mock.patch.object(stage_owned_graphics, "WINE_DISPLAY_SOURCE_PIN", "0" * 40):
            with self.assertRaisesRegex(ValueError, "pin mismatch"):
                stage_owned_graphics.resolve_display_artifacts(build, architecture("x86_64", "x64"), False)
        source = stage_owned_graphics.WINE_DISPLAY_SOURCE
        device = source / "dlls/dcomp/device.c"
        device.write_text("original source\n")
        with self.assertRaisesRegex(ValueError, "still pristine"):
            stage_owned_graphics.resolve_display_artifacts(build, architecture("x86_64", "x64"), False)
        device.write_text("updated source\n")
        module = build / "dlls/dcomp/x86_64-windows/dcomp.dll"
        os.utime(module, ns=(1, 1))
        with self.assertRaisesRegex(ValueError, "rebuild required"):
            stage_owned_graphics.resolve_display_artifacts(build, architecture("x86_64", "x64"), False)
        (build / "Makefile").write_text(f"srcdir = {self.wine}\n")
        with self.assertRaisesRegex(ValueError, "authorized"):
            stage_owned_graphics.resolve_display_artifacts(build, architecture("x86_64", "x64"), False)

    def test_display_destination_and_symlink_safety_preserve_sources(self) -> None:
        build = self.display_build()
        original = (self.wine / "bin/wine").read_bytes()
        for dest in (self.wine, build / "new-runtime", stage_owned_graphics.WINE_DISPLAY_SOURCE / "new-runtime"):
            with self.subTest(dest=dest), self.assertRaises(ValueError):
                stage_owned_graphics.stage(self.dxmt, self.wine, dest, wine_display_build=build)
        module = build / "dlls/dcomp/x86_64-windows/dcomp.dll"
        external = self.root / "external.dll"
        external.write_bytes(module.read_bytes())
        module.unlink()
        module.symlink_to(external)
        with self.assertRaisesRegex(ValueError, "symlink"):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, wine_display_build=build)
        self.assertEqual((self.wine / "bin/wine").read_bytes(), original)
        self.assertFalse(self.dest.exists())

    def test_display_pair_requires_i386_image_and_records_real_pe_identity(self) -> None:
        build = self.display_build()
        arch = architecture("x86_64", "x64")
        module = build / "dlls/dcomp/i386-windows/dcomp.dll"
        module.write_bytes(self.dcomp_image("x64"))
        with self.assertRaisesRegex(ValueError, "expected x86"):
            stage_owned_graphics.resolve_display_artifacts(build, arch, True)
        module.unlink()
        with self.assertRaisesRegex(ValueError, "regular non-symlink"):
            stage_owned_graphics.resolve_display_artifacts(build, arch, True)
        module.write_bytes(self.dcomp_image("x86"))
        artifacts, record = stage_owned_graphics.resolve_display_artifacts(build, arch, True)
        self.assertEqual(set(artifacts), {stage_owned_graphics.WINDOWS_DIR / "dcomp.dll", stage_owned_graphics.WOW64_WINDOWS_DIR / "dcomp.dll"})
        for relative, path in artifacts.items():
            self.assertEqual(record["modules"][str(relative)], {"sha256": stage_owned_graphics.sha256(path), "pe": inspect_pe(path)})
        with self.assertRaisesRegex(ValueError, "x86_64/x64"):
            stage_owned_graphics.resolve_display_artifacts(build, architecture("arm64", "arm64"), False)

    def test_display_paired_stage_rejects_missing_input_then_publishes_both_abis(self) -> None:
        build = self.display_build()
        install32 = self.root / "install32"
        directory = install32 / stage_owned_graphics.DXMT_WOW64_DIR
        directory.mkdir(parents=True)
        (self.wine / stage_owned_graphics.WOW64_WINDOWS_DIR).mkdir()
        for name in stage_owned_graphics.WINDOWS_MODULES:
            image = (self.core_image("x86", main=name == "d3d12.dll") if name in ("d3d12.dll", "d3d12core.dll")
                     else self.pe_image(0x14C, stage_owned_graphics.WINE_BUILTIN_MARKER))
            (directory / name).write_bytes(image)
        module = build / "dlls/dcomp/i386-windows/dcomp.dll"
        payload = module.read_bytes()
        module.unlink()
        with mock.patch.object(stage_owned_graphics.shutil, "copytree") as copy:
            with self.assertRaisesRegex(ValueError, "regular non-symlink"):
                stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, dxmt_install32=install32, wine_display_build=build)
            copy.assert_not_called()
        module.write_bytes(payload)
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, dxmt_install32=install32, wine_display_build=build)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            manifest = json.loads((self.dest / stage_owned_graphics.MANIFEST).read_text())
            for relative in stage_owned_graphics.display_artifacts(True, architecture("x86_64", "x64"), True):
                self.assertIn(str(relative), manifest["artifacts"])
                self.assertEqual(inspect_pe(self.dest / relative)["windows_abi"], "x86" if relative.parent == stage_owned_graphics.WOW64_WINDOWS_DIR else "x64")
        before = (self.dest / stage_owned_graphics.MANIFEST).read_bytes()
        with self.assertRaisesRegex(ValueError, "new path"):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, dxmt_install32=install32, wine_display_build=build)
        self.assertEqual((self.dest / stage_owned_graphics.MANIFEST).read_bytes(), before)

    def test_display_unchanged_binary_and_target_symlink_never_modify_existing_runtime(self) -> None:
        build = self.display_build()
        baseline = self.wine / stage_owned_graphics.WINDOWS_DIR / "dcomp.dll"
        payload = (build / "dlls/dcomp/x86_64-windows/dcomp.dll").read_bytes()
        baseline.write_bytes(payload)
        with mock.patch.object(stage_owned_graphics.shutil, "copytree") as copy:
            with self.assertRaisesRegex(ValueError, "unchanged"):
                stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, wine_display_build=build)
            copy.assert_not_called()
        outside = self.root / "existing-dcomp.dll"
        outside.write_bytes(b"existing runtime module")
        baseline.unlink()
        baseline.symlink_to(outside)
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            with self.assertRaisesRegex(ValueError, "symlink"):
                stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, wine_display_build=build)
        self.assertEqual(outside.read_bytes(), b"existing runtime module")
        self.assertTrue(baseline.is_symlink())
        self.assertFalse(self.dest.exists())
        self.assertEqual(list(self.root.glob("runtime-new.staging-*")), [])

    def test_display_stage_preserves_native_overlays_and_reverifies_policy_and_provenance(self) -> None:
        build = self.display_build()
        extra_source = stage_owned_graphics.WINE_DISPLAY_SOURCE / "dlls/dcomp/composition.h"
        extra_source.write_text("untracked module input\n")
        for module in (build / "dlls/dcomp").glob("*-windows/dcomp.dll"):
            os.utime(module, ns=(extra_source.stat().st_mtime_ns + 1, extra_source.stat().st_mtime_ns + 1))
        preserved = [stage_owned_graphics.WINDOWS_DIR / "windows.ui.dll", stage_owned_graphics.UNIX_DIR / "winemac.so",
                     stage_owned_graphics.UNIX_DIR / "win32u.so", Path("bin/wineserver")]
        for relative in preserved[:-1]:
            (self.wine / relative).write_bytes(b"tuned-runtime-overlay")
        before = {str(relative): (self.wine / relative).read_bytes() for relative in preserved}
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, wine_display_build=build)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            for relative, payload in before.items():
                self.assertEqual((self.wine / relative).read_bytes(), payload)
                self.assertEqual((self.dest / relative).read_bytes(), payload)
            path = self.dest / stage_owned_graphics.MANIFEST
            manifest = json.loads(path.read_text())
            snapshot = manifest["wine_display_frontend"]["staging_source_snapshot"]
            self.assertEqual(snapshot["meaning"], "source contents observed at staging; not a compiled-input receipt")
            self.assertNotIn("compiled_input_receipt", manifest["wine_display_frontend"])
            self.assertEqual(snapshot["files"]["wine/dlls/dcomp/device.c"], stage_owned_graphics.sha256(stage_owned_graphics.WINE_DISPLAY_SOURCE / "dlls/dcomp/device.c"))
            self.assertEqual(snapshot["files"]["wine/dlls/dcomp/composition.h"], stage_owned_graphics.sha256(extra_source))
            for relative in stage_owned_graphics.WINE_DISPLAY_ABI_HEADERS:
                self.assertEqual(snapshot["files"][relative], stage_owned_graphics.sha256(SCRIPT.resolve().parents[1] / relative))
            for invalid in ({}, {"meaning": snapshot["meaning"], "files": {"src/dxgi/dxgi_composition.h": "invalid"}}):
                modified = json.loads(path.read_text())
                modified["wine_display_frontend"]["staging_source_snapshot"] = invalid
                path.write_text(json.dumps(modified))
                self.assertIn("manifest Wine display staging source snapshot is incomplete", stage_owned_graphics.verify_runtime(self.dest))
                path.write_text(json.dumps(manifest))
            self.assertIs(manifest["optional_modules"]["dcomp"], True)
            self.assertIn("dcomp=b", manifest["loader"]["winedlloverrides"].split(";"))
            for field, value in (("source_pin", "0" * 40), ("modules", {}), ("source_changes_sha256", "invalid")):
                modified = json.loads(path.read_text())
                modified["wine_display_frontend"][field] = value
                path.write_text(json.dumps(modified))
                self.assertTrue(any("display frontend provenance" in p for p in stage_owned_graphics.verify_runtime(self.dest)))
                path.write_text(json.dumps(manifest))
            manifest["loader"]["winedlloverrides"] = manifest["loader"]["winedlloverrides"].replace("dcomp=b", "dcomp=n,b")
            path.write_text(json.dumps(manifest))
            self.assertTrue(any("loader override" in p for p in stage_owned_graphics.verify_runtime(self.dest)))
            module = self.dest / stage_owned_graphics.WINDOWS_DIR / "dcomp.dll"
            module.write_bytes(module.read_bytes() + b"changed")
            self.assertTrue(any("hash changed" in p for p in stage_owned_graphics.verify_runtime(self.dest)))

    def test_fsr_binary_architecture_contract_matches_selected_abi(self) -> None:
        for abi in ("x86", "x64", "arm64", "arm64ec"):
            for name, marker in stage_owned_graphics.FSR_MODULE_MARKERS.items():
                with self.subTest(abi=abi, name=name):
                    module = self.root / name
                    module.write_bytes(self.fsr_image(marker, abi))
                    stage_owned_graphics.validate_fsr_module(module, abi)
                    wrong = "x64" if abi != "x64" else "x86"
                    with self.assertRaisesRegex(ValueError, "ABI mismatch"):
                        stage_owned_graphics.validate_fsr_module(module, wrong)

    def core_image(self, abi: str = "x64", main: bool = False, version: int = 618, generation: str = "0123456789abcdef0123456789abcdef", builtin: bool = False) -> bytearray:
        marker = stage_owned_graphics.OWNED_D3D12_MARKER if main else stage_owned_graphics.OWNED_D3D12_CORE_MARKER
        image = self.fsr_image(marker, abi)
        if not main:
            offset = image.find(stage_owned_graphics.WINE_BUILTIN_MARKER)
            image[offset:offset + len(stage_owned_graphics.WINE_BUILTIN_MARKER)] = bytes(len(stage_owned_graphics.WINE_BUILTIN_MARKER))
        cursor = 0x3A0
        names = ("D3D12SDKVersion", "D3D12GetInterface", "OwnedD3D12MainGetBridge1" if main else "OwnedD3D12CoreGetBridge1", "OwnedD3D12CoreGeneration", "OwnedGraphicsBackendId")
        for index, name in enumerate(names):
            encoded = name.encode() + b"\0"
            struct.pack_into("<I", image, 0x360 + index * 4, cursor + 0xE00)
            image[cursor:cursor + len(encoded)] = encoded
            cursor += len(encoded)
        struct.pack_into("<I", image, 0x340, 0x1C00)
        struct.pack_into("<I", image, 0x340 + 3 * 4, 0x1D00)
        struct.pack_into("<I", image, 0xE00, version)
        image[0xF00:0xF21] = generation.encode() + b"\0"
        if builtin:
            image[0x80:0x1C0] = image[0x40:0x180]
            struct.pack_into("<I", image, 0x3C, 0x80)
            image[64:96] = stage_owned_graphics.WINE_BUILTIN_MARKER.ljust(32, b"\0")
        return image

    def test_core_requires_real_version_exports_generation_and_both_abis(self) -> None:
        for abi, directory in (("x64", "x86_64-windows"), ("x86", "i386-windows")):
            root = self.root / abi / directory
            root.mkdir(parents=True)
            main, core = root / "d3d12.dll", root / "d3d12core.dll"
            main.write_bytes(self.core_image(abi, main=True))
            core.write_bytes(self.core_image(abi))
            stage_owned_graphics.validate_core_module(core, abi)
            for image, error in ((self.core_image(abi, version=619), "exported SDK version"),
                                 (self.core_image(abi, generation="f" * 32), "generation"),
                                 (self.fsr_image(abi=abi), "owned marker"),
                                 (self.core_image("x86" if abi == "x64" else "x64"), "ABI mismatch")):
                core.write_bytes(image)
                with self.assertRaisesRegex(ValueError, error):
                    stage_owned_graphics.validate_core_module(core, abi)
            for marker in (stage_owned_graphics.WINE_BUILTIN_MARKER, stage_owned_graphics.WINE_FAKE_MARKER):
                marked = self.core_image(abi)
                # Move the synthetic PE header past Wine's real DOS-stub slot.
                marked[0x80:0x1C0] = marked[0x40:0x180]
                struct.pack_into("<I", marked, 0x3C, 0x80)
                marked[64:96] = marker.ljust(32, b"\0")
                core.write_bytes(marked)
                with self.assertRaisesRegex(ValueError, "ordinary owned PE"):
                    stage_owned_graphics.validate_core_module(core, abi)
                core.write_bytes(self.core_image(abi) + marker)
                stage_owned_graphics.validate_core_module(core, abi)
            self.assertIn("d3d12core=n", stage_owned_graphics.owned_overrides().split(";"))
            self.assertNotIn("d3d12core=b", stage_owned_graphics.owned_overrides().split(";"))
        missing = self.dxmt / "x86_64-windows/d3d12core.dll"
        missing.unlink()
        with self.assertRaisesRegex(ValueError, "regular non-symlink"):
            stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt)

    def test_native_core_staging_rejects_loader_fallback_and_builtin_inputs(self) -> None:
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
            path = self.dest / stage_owned_graphics.MANIFEST
            manifest = json.loads(path.read_text())
            self.assertEqual(manifest["schema"], 2)
            self.assertEqual(manifest["agility_sdk"]["delivery"], "native-only")
            core = self.dest / stage_owned_graphics.WINDOWS_DIR / "d3d12core.dll"
            self.assertNotIn(stage_owned_graphics.WINE_BUILTIN_MARKER, core.read_bytes())
            self.assertIn(stage_owned_graphics.WINE_BUILTIN_MARKER, core.with_name("d3d12.dll").read_bytes())
            overrides = manifest["loader"]["winedlloverrides"]
            self.assertIn("d3d12core=n", overrides.split(";"))
            for mode in ("b", "b,n", "n,b"):
                manifest["loader"]["winedlloverrides"] = overrides.replace("d3d12core=n", f"d3d12core={mode}")
                path.write_text(json.dumps(manifest))
                self.assertTrue(any("loader override" in problem for problem in stage_owned_graphics.verify_runtime(self.dest)))
            manifest["loader"]["winedlloverrides"] = overrides
            path.write_text(json.dumps(manifest))
            core.unlink()
            self.assertTrue(any("d3d12core.dll" in problem for problem in stage_owned_graphics.verify_runtime(self.dest)))
            source = self.dxmt / stage_owned_graphics.DXMT_WINDOWS_DIR / "d3d12core.dll"
            source.write_bytes(self.core_image(builtin=True))
            rejected = self.root / "builtin-core-runtime"
            with self.assertRaisesRegex(ValueError, "ordinary owned PE"):
                stage_owned_graphics.stage(self.dxmt, self.wine, rejected)
            self.assertFalse(rejected.exists())

    def test_retained_builtin_core_requires_recorded_image_mode_and_identity(self) -> None:
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
            path = self.dest / stage_owned_graphics.MANIFEST
            manifest = json.loads(path.read_text())
            del manifest["agility_sdk"]["delivery"]
            relative = str(stage_owned_graphics.WINDOWS_DIR / "d3d12core.dll")
            core = self.dest / relative
            core.write_bytes(self.core_image(builtin=True))
            digest = stage_owned_graphics.sha256(core)
            manifest["artifacts"][relative].update(sha256=digest, size=core.stat().st_size)
            manifest["agility_sdk"]["modules"][relative]["sha256"] = digest
            manifest["loader"]["winedlloverrides"] = manifest["loader"]["winedlloverrides"].replace("d3d12core=n", "d3d12core=b")
            wrapper = self.dest / "bin/wine"
            wrapper.write_text(wrapper.read_text().replace("d3d12core=n", "d3d12core=b"))
            manifest["artifacts"]["bin/wine"]["sha256"] = stage_owned_graphics.sha256(wrapper)
            path.write_text(json.dumps(manifest))
            retained = path.read_text()
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            self.assertEqual(path.read_text(), retained)
            core.write_bytes(self.core_image() + stage_owned_graphics.WINE_BUILTIN_MARKER)
            digest = stage_owned_graphics.sha256(core)
            manifest["artifacts"][relative].update(sha256=digest, size=core.stat().st_size)
            manifest["agility_sdk"]["modules"][relative]["sha256"] = digest
            path.write_text(json.dumps(manifest))
            self.assertTrue(any("recorded Wine builtin image mode" in problem for problem in stage_owned_graphics.verify_runtime(self.dest)))

    def test_wow64_nvapi_uses_official_filename_export_and_ordinal(self) -> None:
        install = self.root / "install32"
        directory = install / "i386-windows"
        directory.mkdir(parents=True)
        for name in stage_owned_graphics.WINDOWS_MODULES:
            payload = stage_owned_graphics.WINE_BUILTIN_MARKER
            if name == "d3d12.dll":
                payload += stage_owned_graphics.OWNED_D3D12_MARKER
            (directory / name).write_bytes(self.core_image("x86", main=name == "d3d12.dll") if name in ("d3d12.dll", "d3d12core.dll") else self.pe_image(0x14C, payload))
        image = self.fsr_image(abi="x86")
        struct.pack_into("<III", image, 0x310, 2, 1, 1)
        image[0x3A0:0x3A0 + 21] = b"nvapi_QueryInterface\0"
        module = directory / "nvapi.dll"
        module.write_bytes(image)
        artifacts = stage_owned_graphics.resolve_wow64_artifacts(install, with_nvapi=True)
        self.assertIn(stage_owned_graphics.WOW64_WINDOWS_DIR / "nvapi.dll", artifacts)
        self.assertNotIn(stage_owned_graphics.WOW64_WINDOWS_DIR / "nvapi64.dll", artifacts)
        self.assertIn("nvapi=b", stage_owned_graphics.owned_overrides(with_nvapi=True, wow64_frontends=True).split(";"))
        struct.pack_into("<I", image, 0x310, 1)
        module.write_bytes(image)
        with self.assertRaisesRegex(ValueError, "missing required exports"):
            stage_owned_graphics.resolve_wow64_artifacts(install, with_nvapi=True)

    def test_fsr_rejects_unowned_wrong_abi_or_invalid_exports_before_publication(self) -> None:
        fsr = self.dxmt / "x86_64-windows" / stage_owned_graphics.FSR_MODULE
        with self.assertRaisesRegex(ValueError, "regular non-symlink"):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, with_fsr=True)
        cases = []
        image = self.fsr_image()
        image[0x800:0x800 + len(stage_owned_graphics.OWNED_FSR_MARKER)] = bytes(len(stage_owned_graphics.OWNED_FSR_MARKER))
        cases.append((image, "owned MetalFX FSR marker"))
        image = self.fsr_image()
        image[0x800:] = bytes(len(image) - 0x800)
        image.extend(stage_owned_graphics.OWNED_FSR_MARKER)
        cases.append((image, "Wine builtin marker"))
        image = self.fsr_image()
        struct.pack_into("<H", image, 0x44, 0xAA64)
        cases.append((image, "ABI mismatch"))
        cases.append((self.hybrid_image() + stage_owned_graphics.OWNED_FSR_MARKER, "ABI mismatch"))
        image = self.fsr_image()
        struct.pack_into("<I", image, 0x310, 2)
        cases.append((image, "exports/ordinals"))
        image = self.fsr_image()
        struct.pack_into("<I", image, 0x318, 4)
        cases.append((image, "exports/ordinals"))
        for function_rva in (0, 0x1200):
            image = self.fsr_image()
            struct.pack_into("<I", image, 0x340, function_rva)
            cases.append((image, "missing or forwarded"))
        image = self.fsr_image()
        struct.pack_into("<I", image, 0x320, 0x17FF)
        cases.append((image, "not backed by file data"))
        for image, message in cases:
            with self.subTest(message=message):
                fsr.write_bytes(image)
                with self.assertRaisesRegex(ValueError, message):
                    stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, with_fsr=True)
                self.assertFalse(self.dest.exists())
                self.assertEqual(list(self.root.glob("runtime-new.staging-*")), [])

    def test_fsr_group_requires_owned_framegeneration_before_publication(self) -> None:
        fsr = self.dxmt / "x86_64-windows" / stage_owned_graphics.FSR_MODULE
        fsr.write_bytes(self.fsr_image())
        fg = self.dxmt / "x86_64-windows" / stage_owned_graphics.FSR_FRAMEGENERATION_MODULE
        for image, message in ((None, "regular non-symlink"),
                               (self.fsr_image(), "owned MetalFX FSR marker")):
            with self.subTest(message=message):
                if image is not None:
                    fg.write_bytes(image)
                with self.assertRaisesRegex(ValueError, message):
                    stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, with_fsr=True)
                self.assertFalse(self.dest.exists())

    def test_fsr_stage_inventory_and_contract_are_reverified(self) -> None:
        fsr = self.dxmt / "x86_64-windows" / stage_owned_graphics.FSR_MODULE
        fsr.write_bytes(self.fsr_image())
        fg = self.dxmt / "x86_64-windows" / stage_owned_graphics.FSR_FRAMEGENERATION_MODULE
        fg.write_bytes(self.fsr_image(stage_owned_graphics.OWNED_FSR_FRAMEGENERATION_MARKER))
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, with_fsr=True)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            manifest = json.loads((self.dest / stage_owned_graphics.MANIFEST).read_text())
            relative = str(stage_owned_graphics.WINDOWS_DIR / stage_owned_graphics.FSR_MODULE)
            self.assertIs(manifest["optional_modules"]["fsr"], True)
            self.assertIs(manifest["optional_modules"]["fsr_framegeneration"], True)
            self.assertEqual(manifest["artifacts"][relative]["sha256"], stage_owned_graphics.sha256(fsr))
            fg_relative = str(stage_owned_graphics.WINDOWS_DIR / fg.name)
            self.assertEqual(manifest["artifacts"][fg_relative]["sha256"], stage_owned_graphics.sha256(fg))
            changed_fg = self.fsr_image(stage_owned_graphics.OWNED_FSR_FRAMEGENERATION_MARKER)
            struct.pack_into("<I", changed_fg, 0x340, 0x1200)
            (self.dest / fg_relative).write_bytes(changed_fg)
            problems = stage_owned_graphics.verify_runtime(self.dest)
            self.assertTrue(any("hash changed" in problem and fg.name in problem for problem in problems), problems)
            self.assertTrue(any("missing or forwarded" in problem for problem in problems), problems)
            (self.dest / fg_relative).write_bytes(fg.read_bytes())
            changed = self.fsr_image()
            struct.pack_into("<I", changed, 0x310, 2)
            (self.dest / relative).write_bytes(changed)
            problems = stage_owned_graphics.verify_runtime(self.dest)
            self.assertTrue(any("hash changed" in problem for problem in problems), problems)
            self.assertTrue(any("exports/ordinals" in problem for problem in problems), problems)

    def test_retained_sr_only_manifest_keeps_policy_until_fg_explicitly_selected(self) -> None:
        for name, marker in stage_owned_graphics.FSR_MODULE_MARKERS.items():
            (self.dxmt / "x86_64-windows" / name).write_bytes(self.fsr_image(marker))
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, with_fsr=True)
            path = self.dest / stage_owned_graphics.MANIFEST
            manifest = json.loads(path.read_text())
            del manifest["optional_modules"]["fsr_framegeneration"]
            del manifest["optional_modules"]["wow64_frontends"]
            fg_relative = str(stage_owned_graphics.WINDOWS_DIR / stage_owned_graphics.FSR_FRAMEGENERATION_MODULE)
            # Retained SR-only policy predates Core; reconstruct the actual
            # old inventory and wrapper rather than a mixed-generation fixture.
            del manifest["agility_sdk"]
            core_relative = str(stage_owned_graphics.WINDOWS_DIR / "d3d12core.dll")
            del manifest["artifacts"][core_relative]
            (self.dest / core_relative).unlink()
            del manifest["artifacts"][fg_relative]
            (self.dest / fg_relative).unlink()
            manifest["loader"]["winedlloverrides"] = (
                "d3d12=b;dxgi=b;d3d11=b;d3d10core=b;winemetal=b;nvngx=;nvapi64=;amd_fidelityfx_upscaler_dx12=b")
            wrapper = self.dest / "bin/wine"
            wrapper.write_text(stage_owned_graphics.render_wrapper(with_fsr=True, with_fsr_framegeneration=None, with_core=False))
            manifest["artifacts"]["bin/wine"]["sha256"] = stage_owned_graphics.sha256(wrapper)
            path.write_text(json.dumps(manifest))
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            for policy in (True, False, None, "true"):
                manifest["optional_modules"]["fsr_framegeneration"] = policy
                path.write_text(json.dumps(manifest))
                problems = stage_owned_graphics.verify_runtime(self.dest)
                if isinstance(policy, bool):
                    self.assertIn("manifest loader override does not select the owned builtin modules exactly", problems)
                else:
                    self.assertIn("manifest optional FSR frame-generation selection must be a boolean", problems)

    def test_retained_manifest_without_fsr_preserves_old_loader_policy(self) -> None:
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
            path = self.dest / stage_owned_graphics.MANIFEST
            manifest = json.loads(path.read_text())
            self.assertIs(manifest["optional_modules"]["fsr"], False)
            del manifest["optional_modules"]["fsr_framegeneration"]
            del manifest["optional_modules"]["fsr"]
            del manifest["optional_modules"]["wow64_frontends"]
            del manifest["agility_sdk"]
            core_relative = str(stage_owned_graphics.WINDOWS_DIR / "d3d12core.dll")
            del manifest["artifacts"][core_relative]
            (self.dest / core_relative).unlink()
            manifest["loader"]["winedlloverrides"] = stage_owned_graphics.owned_overrides(with_fsr=None, with_fsr_framegeneration=None, with_core=False)
            wrapper = self.dest / "bin/wine"
            wrapper.write_text(stage_owned_graphics.render_wrapper(with_fsr=None, with_fsr_framegeneration=None, with_core=False))
            manifest["artifacts"]["bin/wine"]["sha256"] = stage_owned_graphics.sha256(wrapper)
            path.write_text(json.dumps(manifest))
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            manifest["schema"] = 1
            del manifest["architecture"]
            for relative in ("bin/wineserver", str(stage_owned_graphics.UNIX_DIR / "ntdll.so"), str(stage_owned_graphics.WINDOWS_DIR / "ntdll.dll")):
                del manifest["artifacts"][relative]
            path.write_text(json.dumps(manifest))
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            manifest["optional_modules"]["fsr"] = False
            path.write_text(json.dumps(manifest))
            problems = stage_owned_graphics.verify_runtime(self.dest)
            self.assertTrue(any("loader override" in problem for problem in problems), problems)
            self.assertTrue(any("launch policy" in problem for problem in problems), problems)
            manifest["optional_modules"]["fsr"] = None
            path.write_text(json.dumps(manifest))
            self.assertIn("manifest optional FSR selection must be a boolean", stage_owned_graphics.verify_runtime(self.dest))

    def test_wrapper_pins_loader_paths_and_does_not_inherit_foreign_backends(self) -> None:
        runtime = self.root / "runtime-env"
        (runtime / "bin").mkdir(parents=True)
        gst_lib = runtime / "lib/GStreamer.framework/Versions/1.0/lib"
        gst_lib.mkdir(parents=True)
        (runtime / "yaagl-wine-p3-runtime.txt").write_text("fixture\n")
        wrapper = runtime / "bin/wine"
        wrapper.write_text(stage_owned_graphics.render_wrapper())
        wrapper.chmod(0o755)
        real = runtime / "bin/wine.real"
        real.write_text(
            '#!/bin/sh\n'
            'printf "WINEDLLPATH=%s\\n" "${WINEDLLPATH-}"\n'
            'printf "WINEDLLPATH_PREPEND=%s\\n" "${WINEDLLPATH_PREPEND-}"\n'
            'printf "DYLD_FALLBACK_LIBRARY_PATH=%s\\n" "${DYLD_FALLBACK_LIBRARY_PATH-}"\n'
            'printf "CX_ACTIVE_GRAPHICS_BACKEND=%s\n" "${CX_ACTIVE_GRAPHICS_BACKEND-}"\n'
            'printf "WINEDLLOVERRIDES=%s\n" "${WINEDLLOVERRIDES-}"\n'
            'printf "DXMT_ENABLE_NVEXT=%s\n" "${DXMT_ENABLE_NVEXT-}"\n'
            'printf "FEX_HIDEHYPERVISORBIT=%s\n" "${FEX_HIDEHYPERVISORBIT-}"\n'
        )
        real.chmod(0o755)
        prefix = self.root / "isolated-prefix"
        prefix.mkdir()
        environment = dict(os.environ)
        environment.update({
            "WINEPREFIX": str(prefix),
            "WINEDLLPATH": "/tmp/foreign-wine/lib/wine",
            "WINEDLLPATH_PREPEND": "/tmp/foreign-prepend",
            "DYLD_FALLBACK_LIBRARY_PATH": "/tmp/foreign-dyld",
            "CX_ACTIVE_GRAPHICS_BACKEND": "d3dmetal",
            "WINEDLLOVERRIDES": "d3d12=n",
            "DXMT_ENABLE_NVEXT": "1",
            "FEX_HIDEHYPERVISORBIT": "0",
        })
        output = subprocess.check_output([str(wrapper)], env=environment, text=True)
        values = dict(line.split("=", 1) for line in output.splitlines())
        self.assertEqual(values["WINEDLLPATH"], ":".join((str(runtime / "lib/wine/x86_64-windows"), str(runtime / "lib/wine/x86_64-unix"))))
        self.assertEqual(values["WINEDLLPATH_PREPEND"], "")
        self.assertEqual(values["CX_ACTIVE_GRAPHICS_BACKEND"], "")
        self.assertEqual(values["WINEDLLOVERRIDES"], stage_owned_graphics.owned_overrides())
        self.assertIn("amd_fidelityfx_upscaler_dx12=", values["WINEDLLOVERRIDES"].split(";"))
        self.assertEqual(values["DXMT_ENABLE_NVEXT"], "")
        self.assertEqual(values["FEX_HIDEHYPERVISORBIT"], "0")
        wrapper.write_text(stage_owned_graphics.render_wrapper(with_wow64=True))
        output = subprocess.check_output([str(wrapper)], env=environment, text=True)
        values = dict(line.split("=", 1) for line in output.splitlines())
        self.assertEqual(values["WINEDLLPATH"], ":".join(str(runtime / path) for path in ("lib/wine/x86_64-windows", "lib/wine/i386-windows", "lib/wine/x86_64-unix")))
        self.assertEqual(values["WINEDLLPATH_PREPEND"], "")
        wrapper.write_text(stage_owned_graphics.render_wrapper(True, True))
        output = subprocess.check_output([str(wrapper)], env=environment, text=True)
        values = dict(line.split("=", 1) for line in output.splitlines())
        self.assertEqual(values["WINEDLLOVERRIDES"], stage_owned_graphics.owned_overrides(True, True))
        self.assertEqual(values["DXMT_ENABLE_NVEXT"], "1")
        wrapper.write_text(stage_owned_graphics.render_wrapper(with_fsr=True, with_fsr_framegeneration=True))
        output = subprocess.check_output([str(wrapper)], env=environment, text=True)
        values = dict(line.split("=", 1) for line in output.splitlines())
        self.assertIn("amd_fidelityfx_upscaler_dx12=b", values["WINEDLLOVERRIDES"].split(";"))
        self.assertIn("amd_fidelityfx_framegeneration_dx12=b", values["WINEDLLOVERRIDES"].split(";"))
        self.assertEqual(values["DXMT_ENABLE_NVEXT"], "")
        wrapper.write_text(stage_owned_graphics.render_wrapper(with_dcomp=True, with_wow64=True))
        output = subprocess.check_output([str(wrapper)], env=environment, text=True)
        values = dict(line.split("=", 1) for line in output.splitlines())
        self.assertIn("dcomp=b", values["WINEDLLOVERRIDES"].split(";"))
        self.assertEqual(values["WINEDLLPATH"], f"{runtime}/lib/wine/x86_64-windows:{runtime}/lib/wine/i386-windows:{runtime}/lib/wine/x86_64-unix")
        wrapper.write_text(stage_owned_graphics.render_wrapper(arch=architecture("arm64", "arm64x")))
        output = subprocess.check_output([str(wrapper)], env=environment, text=True)
        values = dict(line.split("=", 1) for line in output.splitlines())
        self.assertEqual(values["FEX_HIDEHYPERVISORBIT"], "1")
        wrapper.write_text(stage_owned_graphics.render_wrapper(with_fsr=True, with_fsr_framegeneration=True))
        environment["OWNED_FG_OBSERVER"] = str(self.root / "missing-observer.dylib")
        rejected = subprocess.run([str(wrapper)], env=environment, capture_output=True, text=True)
        self.assertEqual(rejected.returncode, 126)
        self.assertIn("missing FG observer", rejected.stderr)
        environment.pop("OWNED_FG_OBSERVER")
        environment.pop("WINEPREFIX")
        rejected = subprocess.run([str(wrapper)], env=environment, text=True, capture_output=True)
        self.assertEqual(rejected.returncode, 64)

    def test_dependency_policy_accepts_relocatable_wine_and_system_paths(self) -> None:
        dependencies = [
            "@rpath/winemetal.so",
            "@rpath/winemac.so",
            "@rpath/ntdll.so",
            "/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
            "/usr/lib/libSystem.B.dylib",
        ]
        self.assertEqual(stage_owned_graphics.dependency_problems(dependencies), [])
        for foreign in ("/tmp/build/libd3dshared.dylib", "@rpath/libmetalirconverter.dylib",
                        "@rpath/libdxmt-dxilconv.dylib",
                        "@rpath/D3DMetal.framework/Versions/A/D3DMetal"):
            with self.subTest(foreign=foreign):
                problems = stage_owned_graphics.dependency_problems(dependencies + [foreign])
                self.assertTrue(any("external compiler/backend library" in problem for problem in problems),
                                problems)

    def test_macho_audit_rejects_absolute_paths_and_wrong_slice(self) -> None:
        def audit(dependencies, rpaths, architectures):
            with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=dependencies), \
                 mock.patch.object(stage_owned_graphics, "macho_rpaths", return_value=rpaths), \
                 mock.patch.object(stage_owned_graphics, "macho_architectures", return_value=architectures):
                return stage_owned_graphics.macho_audit_problems(self.root / "unused", "fixture")

        self.assertEqual(audit(["@rpath/winemac.so", "@rpath/ntdll.so", "/usr/lib/libSystem.B.dylib"],
                               ["@loader_path/"], ["x86_64"]), [])
        problems = audit(["/Users/build/libx.dylib"], ["/Users/build"], ["arm64"])
        self.assertTrue(any("non-relocatable external dependency" in problem for problem in problems))
        self.assertTrue(any("non-relocatable LC_RPATH" in problem for problem in problems))
        self.assertTrue(any("no x86_64 slice" in problem for problem in problems))

    def test_module_without_wine_builtin_marker_is_rejected(self) -> None:
        dependencies = ["@rpath/winemetal.so", "@rpath/winemac.so", "@rpath/ntdll.so",
                        "/usr/lib/libSystem.B.dylib"]
        with mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]), \
             mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=dependencies):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
        staged = self.dest / stage_owned_graphics.WINDOWS_DIR / "d3d12.dll"
        staged.write_bytes(staged.read_bytes().replace(stage_owned_graphics.WINE_BUILTIN_MARKER, b""))
        problems = stage_owned_graphics.verify_runtime(self.dest)
        self.assertTrue(any("Wine builtin marker" in problem for problem in problems), problems)
        self.assertTrue(any("hash changed" in problem for problem in problems), problems)

    def test_non_relocatable_winemetal_is_refused(self) -> None:
        dependencies = ["@rpath/winemetal.so", "@rpath/winemac.so", "@rpath/ntdll.so"]
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=dependencies), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems",
                               return_value=["winemetal.so has non-relocatable LC_RPATH: /Users/build"]):
            with self.assertRaisesRegex(ValueError, "non-relocatable LC_RPATH"):
                stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)

    def test_complete_stage_copies_only_destination_and_verifies_hashes(self) -> None:
        original_wine = (self.wine / "bin/wine").read_bytes()
        dependencies = [
            "@rpath/winemetal.so",
            "@rpath/winemac.so",
            "@rpath/ntdll.so",
            "/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
            "/usr/lib/libSystem.B.dylib",
        ]
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=dependencies), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
        self.assertEqual((self.wine / "bin/wine").read_bytes(), original_wine)
        self.assertFalse((self.wine / "bin/wine.real").exists())
        self.assertEqual((self.dest / "bin/wine.real").read_bytes(), original_wine)
        for relative, source in stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt).items():
            self.assertEqual((self.dest / relative).read_bytes(), source.read_bytes())
        manifest = self.dest / stage_owned_graphics.MANIFEST
        self.assertTrue(manifest.is_file())

    def test_leftover_external_dxil_artifacts_are_refused(self) -> None:
        for relative in (*stage_owned_graphics.FORBIDDEN_STAGED_ARTIFACTS,
                         stage_owned_graphics.DXMT_WINDOWS_DIR / "libmetalirconverter.dylib"):
            with self.subTest(relative=str(relative)):
                path = self.dxmt / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"leftover")
                with self.assertRaisesRegex(ValueError, "removed external DXIL compiler"):
                    stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt)
                path.unlink()
        self.assertTrue(stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt))

    def test_winemetal_linking_a_removed_compiler_library_is_refused(self) -> None:
        dependencies = ["@rpath/winemetal.so", "@rpath/winemac.so", "@rpath/ntdll.so",
                        "@rpath/libdxmt-dxilconv.dylib"]
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=dependencies), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            with self.assertRaisesRegex(ValueError, "external compiler/backend library"):
                stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)

    def test_native_compiler_is_recorded_and_reverified(self) -> None:
        dependencies = ["@rpath/winemetal.so", "@rpath/winemac.so", "@rpath/ntdll.so",
                        "/usr/lib/libSystem.B.dylib"]
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=dependencies), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            manifest = json.loads((self.dest / stage_owned_graphics.MANIFEST).read_text())
            self.assertEqual(manifest["native_compiler"]["ownership"], "self-owned")
            self.assertEqual(manifest["native_compiler"]["path"],
                             str(stage_owned_graphics.UNIX_DIR / stage_owned_graphics.NATIVE_MODULE))
            self.assertEqual(manifest["native_compiler"]["external_dependencies"], [])
            self.assertNotIn("dxil_compiler", manifest)
            # A runtime that regains a removed external compiler artifact must fail verification.
            leftover = self.dest / stage_owned_graphics.FORBIDDEN_STAGED_ARTIFACTS[0]
            leftover.write_bytes(b"leftover")
            problems = stage_owned_graphics.verify_runtime(self.dest)
            self.assertTrue(any("removed external DXIL compiler artifact" in problem for problem in problems),
                            problems)
            leftover.unlink()
            # A manifest that still describes the removed SDK flow must fail verification too.
            manifest_path = self.dest / stage_owned_graphics.MANIFEST
            without_record = json.loads(manifest_path.read_text())
            del without_record["native_compiler"]
            manifest_path.write_text(json.dumps(without_record))
            problems = stage_owned_graphics.verify_runtime(self.dest)
            self.assertTrue(any("self-owned native compiler" in problem for problem in problems), problems)

    @staticmethod
    def pe_image(machine: int, payload: bytes) -> bytes:
        header = bytearray(0x200)
        header[:2] = b"MZ"
        header[0x3C:0x40] = (0x40).to_bytes(4, "little")
        header[0x40:0x44] = b"PE\0\0"
        optional_size = 224 if machine == 0x14C else 240
        struct.pack_into("<HH", header, 0x44, machine, 1)
        struct.pack_into("<H", header, 0x54, optional_size)
        struct.pack_into("<H", header, 0x58, 0x10B if machine == 0x14C else 0x20B)
        section = 0x58 + optional_size
        header[section:section + 5] = b".text"
        struct.pack_into("<IIII", header, section + 8, len(payload), 0x1000, len(payload), 0x200)
        struct.pack_into("<I", header, section + 36, 0x60000020)
        return bytes(header) + payload

    def hybrid_image(self, machine: int = 0x8664, hybrid: bool = False) -> bytearray:
        image = bytearray(self.pe_image(machine, bytes(0x1000)))
        struct.pack_into("<Q", image, 0x58 + 24, 0x180000000)
        struct.pack_into("<I", image, 0x58 + 108, 16)
        struct.pack_into("<II", image, 0x58 + 112 + 80, 0x1100, 232)
        struct.pack_into("<I", image, 0x300, 232)
        struct.pack_into("<Q", image, 0x300 + 200, 0x180001300)
        struct.pack_into("<III", image, 0x500, 2, 0x1400, 1)
        struct.pack_into("<II", image, 0x600, 0x1501, 16)
        if hybrid:
            struct.pack_into("<IH", image, 0x300 + 224, 0x600, 1)
            struct.pack_into("<IIQIIIHH", image, 0x800, 1, 24, 6, 12, 0x1000, 12, 0x4000, 0)
        return image

    def test_final_amd64_chpe_and_arm64x_are_not_plain_x64(self) -> None:
        image = self.root / "hybrid.dll"
        image.write_bytes(self.pe_image(0x8664, b"code"))
        self.assertEqual(inspect_pe(image)["windows_abi"], "x64")
        image.write_bytes(self.hybrid_image())
        fact = inspect_pe(image)
        self.assertEqual(fact["windows_abi"], "arm64ec")
        self.assertEqual(fact["code_ranges"], [{"rva": 0x1500, "size": 16, "architecture": "arm64ec"}])
        image.write_bytes(self.hybrid_image(0xAA64, True))
        self.assertEqual(inspect_pe(image)["windows_abi"], "arm64x")

    def test_chpe_code_map_can_span_executable_alignment_gap(self) -> None:
        image = self.hybrid_image()
        image.extend(bytes(0x400))
        struct.pack_into("<H", image, 0x46, 2)
        first = 0x58 + 240
        struct.pack_into("<II", image, first + 8, 0x1200, 0x1000)
        struct.pack_into("<I", image, first + 16, 0x1200)
        second = first + 40
        struct.pack_into("<IIII", image, second + 8, 16, 0x3000, 16, 0x1400)
        struct.pack_into("<I", image, second + 36, 0x60000020)
        struct.pack_into("<I", image, 0x508, 2)
        struct.pack_into("<II", image, 0x608, 0x2002, 0x1010)
        path = self.root / "gapped.dll"
        path.write_bytes(image)
        self.assertEqual(inspect_pe(path)["code_ranges"][1], {"rva": 0x2000, "size": 0x1010, "architecture": "x64"})
        struct.pack_into("<I", image, second + 36, 0x40000040)
        path.write_bytes(image)
        with self.assertRaises(ValueError):
            inspect_pe(path)

    def test_macho_fat_slice_header_mismatch_is_rejected(self) -> None:
        fat = struct.pack(">IIIIIII", 0xCAFEBABE, 1, 0x100000C, 0, 28, 32, 0)
        image = self.root / "mislabeled-fat"
        image.write_bytes(fat + struct.pack("<IIIIIIII", 0xFEEDFACF, 0x1000007, 3, 2, 0, 0, 0, 0))
        with self.assertRaises(ValueError):
            stage_owned_graphics.macho_architectures(image)

    def test_x64_client_with_arm64_ipc_server_keeps_its_real_native_facts(self) -> None:
        server = self.wine / "bin/wineserver"
        server.write_bytes(struct.pack("<IIIIIIII", 0xFEEDFACF, 0x100000C, 0, 2, 0, 0, 0, 0))
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
            manifest = json.loads((self.dest / stage_owned_graphics.MANIFEST).read_text())
            self.assertEqual(manifest["artifacts"]["bin/wineserver"]["native_architectures"], ["arm64"])
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            manifest["artifacts"]["bin/wineserver"]["native_architectures"] = ["x86_64"]
            (self.dest / stage_owned_graphics.MANIFEST).write_text(json.dumps(manifest))
            self.assertTrue(any("native metadata mismatch" in p for p in stage_owned_graphics.verify_runtime(self.dest)))

    def test_invalid_or_intermediate_hybrid_images_fail_closed(self) -> None:
        image = self.root / "invalid.dll"
        cases = []
        invalid = self.hybrid_image()
        struct.pack_into("<Q", invalid, 0x300 + 200, 0x180100000)
        cases.append(invalid)
        invalid = self.hybrid_image()
        struct.pack_into("<II", invalid, 0x600, 0xFFFFFFF1, 16)
        cases.append(invalid)
        invalid = self.hybrid_image()
        struct.pack_into("<I", invalid, 0x508, 0)
        cases.append(invalid)
        invalid = self.hybrid_image()
        struct.pack_into("<I", invalid, 0x600, 0x1502)
        cases.append(invalid)
        cases.append(self.pe_image(0xA641, b"intermediate"))
        cases.append(b"\x41\xa6" + bytes(30))
        for payload in cases:
            with self.subTest(payload=payload[:8]):
                image.write_bytes(payload)
                with self.assertRaises(ValueError):
                    inspect_pe(image)

    def test_schema1_reads_as_x64_but_schema2_layout_mismatch_fails(self) -> None:
        self.assertEqual(runtime_architecture({"schema": 1}), architecture("x86_64", "x64"))
        malformed = architecture("arm64", "arm64ec")
        malformed["windows_directory"] = "lib/wine/x86_64-windows"
        with self.assertRaisesRegex(ValueError, "layout mismatch"):
            runtime_architecture({"schema": 2, "architecture": malformed})

    def test_machine_mismatch_refuses_publication_and_guest_execution(self) -> None:
        dll = self.dxmt / "x86_64-windows/dxgi.dll"
        dll.write_bytes(self.pe_image(0xAA64, stage_owned_graphics.WINE_BUILTIN_MARKER))
        with self.assertRaisesRegex(ValueError, "ABI mismatch"):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest)
        self.assertFalse(self.dest.exists())
        guest = self.root / "guest.exe"
        guest.write_bytes(self.pe_image(0x8664, b"x64"))
        for abi in ("arm64", "arm64ec", "arm64x"):
            with self.subTest(abi=abi), self.assertRaises(ValueError):
                execution_architecture({"schema": 2, "architecture": architecture("arm64", abi)}, guest)
        with self.assertRaisesRegex(ValueError, "guest-arch"):
            execution_architecture({"schema": 1}, guest, "arm64")
        guest.write_bytes(self.pe_image(0x14C, b"x86"))
        with self.assertRaisesRegex(ValueError, "WoW64"):
            execution_architecture({"schema": 1}, guest)
        guest.write_bytes(self.pe_image(0xAA64, b"arm64"))
        facts = execution_architecture({"schema": 2, "architecture": architecture("arm64", "arm64")}, guest)
        self.assertEqual(facts["guest_arch"], "arm64")
        self.assertFalse(facts["strict_no_rosetta_proven"])
        for abi in ("arm64ec", "arm64x"):
            with self.subTest(abi=abi), self.assertRaises(ValueError):
                execution_architecture({"schema": 2, "architecture": architecture("arm64", abi)}, guest)

    def test_fex_refuses_non_arm64x_runtime_before_publication(self) -> None:
        fex = self.root / "fex-build"
        fex.mkdir()
        with self.assertRaisesRegex(ValueError, "native ARM64/paired ARM64X"):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, fex_build=fex)
        self.assertFalse(self.dest.exists())

    def test_arm64_staging_selects_native_and_windows_axes_independently(self) -> None:
        arch = architecture("arm64", "arm64ec")
        (self.wine / arch["windows_directory"]).mkdir()
        (self.wine / arch["windows_directory"] / "ntdll.dll").write_bytes(self.hybrid_image())
        native = self.wine / arch["unix_directory"]
        native.mkdir()
        macho = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x100000C, 0, 2, 0, 0, 0, 0)
        for path in (self.wine / "bin/wine", self.wine / "bin/wineserver", native / "ntdll.so"):
            path.write_bytes(macho)
        (self.dxmt / "arm64ec-windows").mkdir()
        (self.dxmt / "aarch64-unix").mkdir()
        for name in stage_owned_graphics.WINDOWS_MODULES:
            payload = self.hybrid_image() + stage_owned_graphics.WINE_BUILTIN_MARKER
            if name == "d3d12.dll":
                payload += stage_owned_graphics.OWNED_D3D12_MARKER
            (self.dxmt / "arm64ec-windows" / name).write_bytes(self.core_image("arm64ec", main=name == "d3d12.dll") if name in ("d3d12.dll", "d3d12core.dll") else payload)
        (self.dxmt / "aarch64-unix/winemetal.so").write_bytes(macho)
        for name, marker in stage_owned_graphics.FSR_MODULE_MARKERS.items():
            (self.dxmt / "arm64ec-windows" / name).write_bytes(self.fsr_image(marker, "arm64ec"))
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=["@rpath/winemac.so", "@rpath/ntdll.so"]), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, native_arch="arm64", windows_abi="arm64ec", with_fsr=True)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            manifest = json.loads((self.dest / stage_owned_graphics.MANIFEST).read_text())
            self.assertEqual(manifest["architecture"], arch)
            manifest_file = self.dest / stage_owned_graphics.MANIFEST
            original = manifest_file.read_text()
            manifest["optional_modules"]["fex_x86"] = True
            manifest_file.write_text(json.dumps(manifest))
            problems = stage_owned_graphics.verify_runtime(self.dest)
            self.assertTrue(any("owned WoW64 frontends" in problem for problem in problems), problems)
            self.assertTrue(any("guest-window Wine provenance" in problem for problem in problems), problems)
            self.assertTrue(any("inventory" in problem for problem in problems), problems)
            manifest_file.write_text(original)
            (self.dest / "bin/wine.real").write_bytes(struct.pack("<IIIIIIII", 0xFEEDFACF, 0x1000007, 3, 2, 0, 0, 0, 0))
            self.assertTrue(any("lacks arm64" in p for p in stage_owned_graphics.verify_runtime(self.dest)))

    def test_wow64_modules_are_staged_machine_checked_and_reverified(self) -> None:
        dxmt32 = self.root / "dxmt-install32"
        (dxmt32 / stage_owned_graphics.DXMT_WOW64_DIR).mkdir(parents=True)
        (self.wine / stage_owned_graphics.WOW64_WINDOWS_DIR).mkdir(parents=True)
        for name in stage_owned_graphics.WINDOWS_MODULES:
            payload = name.encode() + b"-" + stage_owned_graphics.WINE_BUILTIN_MARKER
            if name == "d3d12.dll":
                payload += b"-" + stage_owned_graphics.OWNED_D3D12_MARKER
            (dxmt32 / stage_owned_graphics.DXMT_WOW64_DIR / name).write_bytes(
                self.core_image("x86", main=name == "d3d12.dll") if name in ("d3d12.dll", "d3d12core.dll") else self.pe_image(stage_owned_graphics.PE_MACHINE_I386, payload))
        dependencies = ["@rpath/winemetal.so", "@rpath/winemac.so", "@rpath/ntdll.so", "/usr/lib/libSystem.B.dylib"]
        with mock.patch.object(stage_owned_graphics, "macho_dependencies", return_value=dependencies), \
             mock.patch.object(stage_owned_graphics, "macho_audit_problems", return_value=[]):
            # An x86_64 image in the 32-bit prefix is a mislabeled build, not a WoW64 module.
            amd64 = dxmt32 / stage_owned_graphics.DXMT_WOW64_DIR / "dxgi.dll"
            good = amd64.read_bytes()
            amd64.write_bytes(self.pe_image(0x8664, good[0x40 + 24:]))
            with self.assertRaisesRegex(ValueError, "not an i386 PE image"):
                stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, dxmt_install32=dxmt32)
            self.assertFalse(self.dest.exists())
            amd64.write_bytes(good)

            stage_owned_graphics.stage(self.dxmt, self.wine, self.dest, dxmt_install32=dxmt32)
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            staged = self.dest / stage_owned_graphics.WOW64_WINDOWS_DIR / "d3d12.dll"
            self.assertEqual(staged.read_bytes(), (dxmt32 / "i386-windows/d3d12.dll").read_bytes())
            manifest = json.loads((self.dest / stage_owned_graphics.MANIFEST).read_text())
            self.assertIs(manifest["optional_modules"]["wow64"], True)
            self.assertEqual(manifest["loader"]["winedllpath"], ["lib/wine/x86_64-windows", "lib/wine/i386-windows", "lib/wine/x86_64-unix"])
            manifest_file = self.dest / stage_owned_graphics.MANIFEST
            original_manifest = manifest_file.read_text()
            wrapper = self.dest / "bin/wine"
            original_wrapper = wrapper.read_text()
            manifest["loader"]["winedllpath"].append("/tmp/foreign-wine")
            manifest_file.write_text(json.dumps(manifest))
            self.assertTrue(any("discovery paths" in problem for problem in stage_owned_graphics.verify_runtime(self.dest)))
            # Genuine retained paired runtimes lacked the new discovery hint.
            del manifest["loader"]["winedllpath"]
            wrapper.write_text(stage_owned_graphics.render_wrapper(wow64_frontends=True))
            manifest["artifacts"]["bin/wine"]["sha256"] = stage_owned_graphics.sha256(wrapper)
            manifest_file.write_text(json.dumps(manifest))
            self.assertEqual(stage_owned_graphics.verify_runtime(self.dest), [])
            wrapper.write_text(original_wrapper)
            manifest_file.write_text(original_manifest)
            self.assertIn(str(stage_owned_graphics.WOW64_WINDOWS_DIR / "d3d12.dll"), manifest["artifacts"])
            self.assertEqual(manifest["agility_sdk"]["version"], 618)
            self.assertEqual(set(manifest["agility_sdk"]["modules"]), {"lib/wine/x86_64-windows/d3d12core.dll", "lib/wine/i386-windows/d3d12core.dll"})
            core = self.dest / stage_owned_graphics.WOW64_WINDOWS_DIR / "d3d12core.dll"
            original_core = core.read_bytes()
            core.write_bytes(self.core_image("x86", generation="f" * 32))
            self.assertTrue(any("generation" in problem for problem in stage_owned_graphics.verify_runtime(self.dest)))
            core.write_bytes(original_core)
            # A newly selected facade group must also be source-owned in WoW64.
            for name, marker in stage_owned_graphics.FSR_MODULE_MARKERS.items():
                (self.dxmt / "x86_64-windows" / name).write_bytes(self.fsr_image(marker))
            fsr_runtime = self.root / "runtime-wow64-fsr"
            with self.assertRaisesRegex(ValueError, "regular non-symlink"):
                stage_owned_graphics.stage(self.dxmt, self.wine, fsr_runtime, dxmt_install32=dxmt32, with_fsr=True)
            self.assertFalse(fsr_runtime.exists())
            for name, marker in stage_owned_graphics.FSR_MODULE_MARKERS.items():
                (dxmt32 / "i386-windows" / name).write_bytes(self.fsr_image(marker, "x86"))
            stage_owned_graphics.stage(self.dxmt, self.wine, fsr_runtime, dxmt_install32=dxmt32, with_fsr=True)
            self.assertEqual(stage_owned_graphics.verify_runtime(fsr_runtime), [])
            fsr_manifest = json.loads((fsr_runtime / stage_owned_graphics.MANIFEST).read_text())
            self.assertIs(fsr_manifest["optional_modules"]["wow64_frontends"], True)
            for name in stage_owned_graphics.FSR_MODULE_MARKERS:
                self.assertEqual(fsr_manifest["artifacts"][str(stage_owned_graphics.WOW64_WINDOWS_DIR / name)]["pe"]["windows_abi"], "x86")
            # A staged 32-bit module replaced after staging must fail reverification.
            staged.write_bytes(self.pe_image(stage_owned_graphics.PE_MACHINE_I386, b"stale"))
            problems = stage_owned_graphics.verify_runtime(self.dest)
            self.assertTrue(any("hash changed: lib/wine/i386-windows/d3d12.dll" in p for p in problems), problems)

    def test_artifact_hash_detects_mutation(self) -> None:
        artifact = self.root / "artifact"
        artifact.write_bytes(b"one")
        first = stage_owned_graphics.sha256(artifact)
        artifact.write_bytes(b"two")
        self.assertNotEqual(first, stage_owned_graphics.sha256(artifact))


if __name__ == "__main__":
    unittest.main()
