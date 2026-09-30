#!/usr/bin/env python3
"""CPU-only tests for stage-owned-graphics.py path and publication policy."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock


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
        (self.wine / "bin/wine").write_bytes(b"\xcf\xfa\xed\xfe-wine")
        (self.wine / "bin/wine").chmod(0o755)
        (self.dxmt / stage_owned_graphics.DXMT_WINDOWS_DIR).mkdir(parents=True)
        (self.dxmt / stage_owned_graphics.DXMT_UNIX_DIR).mkdir(parents=True)
        for name in stage_owned_graphics.WINDOWS_MODULES:
            payload = b"MZ-" + name.encode() + b"-" + stage_owned_graphics.WINE_BUILTIN_MARKER
            if name == "d3d12.dll":
                payload += b"-" + stage_owned_graphics.OWNED_D3D12_MARKER
            (self.dxmt / stage_owned_graphics.DXMT_WINDOWS_DIR / name).write_bytes(payload)
        (self.dxmt / stage_owned_graphics.DXMT_UNIX_DIR / "winemetal.so").write_bytes(b"\xcf\xfa\xed\xfe-winemetal")

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
        nvngx.write_bytes(b"MZ-fixture-" + stage_owned_graphics.OWNED_NGX_D3D12_MARKER)
        nvapi.write_bytes(b"MZ-fixture-nvapi")

        def exports(path: Path) -> set[str]:
            if path.name == "nvngx.dll":
                return set(stage_owned_graphics.REQUIRED_NGX_D3D12_EXPORTS)
            if path.name == "nvapi64.dll":
                return set(stage_owned_graphics.REQUIRED_NVAPI_EXPORTS)
            return set()

        with mock.patch.object(stage_owned_graphics, "pe_exports", side_effect=exports):
            artifacts = stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt, True, True)
        self.assertEqual(artifacts[stage_owned_graphics.WINDOWS_DIR / "nvngx.dll"], nvngx)
        self.assertEqual(artifacts[stage_owned_graphics.WINDOWS_DIR / "nvapi64.dll"], nvapi)
        self.assertEqual(
            stage_owned_graphics.owned_overrides(True, True),
            stage_owned_graphics.OWNED_OVERRIDES + ";nvngx=b;nvapi64=b",
        )

    def test_optional_nvngx_refuses_missing_d3d12_export(self) -> None:
        nvngx = self.dxmt / "x86_64-windows/nvngx.dll"
        nvngx.write_bytes(b"MZ-fixture-" + stage_owned_graphics.OWNED_NGX_D3D12_MARKER)
        incomplete = set(stage_owned_graphics.REQUIRED_NGX_D3D12_EXPORTS)
        incomplete.remove("NVSDK_NGX_D3D12_EvaluateFeature")
        with mock.patch.object(stage_owned_graphics, "pe_exports", return_value=incomplete):
            with self.assertRaisesRegex(ValueError, "missing required D3D12 exports"):
                stage_owned_graphics.resolve_dxmt_artifacts(self.dxmt, True, False)

    def test_wrapper_scrubs_gptk_and_forces_builtin_only_modules(self) -> None:
        wrapper = stage_owned_graphics.render_wrapper()
        for name in stage_owned_graphics.GPTK_ENV:
            self.assertIn(name, wrapper)
        for name in stage_owned_graphics.FOREIGN_GRAPHICS_ENV:
            self.assertIn(name, wrapper)
        self.assertIn(f'export WINEDLLOVERRIDES="{stage_owned_graphics.OWNED_OVERRIDES}', wrapper)
        self.assertIn('if [ -z "${WINEPREFIX:-}" ]', wrapper)
        self.assertNotIn("D3DMetal.framework", wrapper)
        self.assertIn("GStreamer.framework/Versions/1.0", wrapper)
        self.assertIn("DYLD_FALLBACK_LIBRARY_PATH", wrapper)

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
            'printf "CX_ACTIVE_GRAPHICS_BACKEND=%s\\n" "${CX_ACTIVE_GRAPHICS_BACKEND-}"\n'
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
        })
        output = subprocess.check_output([str(wrapper)], env=environment, text=True)
        values = dict(line.split("=", 1) for line in output.splitlines())
        self.assertEqual(values["WINEDLLPATH"], ":".join((str(runtime / "lib/wine/x86_64-windows"), str(runtime / "lib/wine/x86_64-unix"))))
        self.assertEqual(values["WINEDLLPATH_PREPEND"], "")
        self.assertEqual(values["CX_ACTIVE_GRAPHICS_BACKEND"], "")
        # macOS may scrub DYLD_* when the wrapper execs a script interpreter, so
        # validate the rendered policy itself instead of the child environment.
        rendered = stage_owned_graphics.render_wrapper()
        self.assertIn('export DYLD_FALLBACK_LIBRARY_PATH="$dyld_fallback"', rendered)
        self.assertNotIn('$dyld_fallback:$DYLD_FALLBACK_LIBRARY_PATH', rendered)

    def test_wrapper_renders_opt_in_dyld_audit_at_last_hop(self) -> None:
        rendered = stage_owned_graphics.render_wrapper()
        audit = 'if [ "${OWNED_GRAPHICS_DYLD_AUDIT:-0}" = "1" ]; then\n  export DYLD_PRINT_LIBRARIES=1\nelse\n  unset DYLD_PRINT_LIBRARIES\nfi\n\nexec "$real_wine" "$@"'
        self.assertIn(audit, rendered)
        self.assertIn("DYLD_PRINT_LIBRARIES", stage_owned_graphics.FOREIGN_GRAPHICS_ENV)

    def test_wrapper_can_pin_validated_nv_modules(self) -> None:
        rendered = stage_owned_graphics.render_wrapper(True, True)
        self.assertIn(
            'export WINEDLLOVERRIDES="d3d12=b;dxgi=b;d3d11=b;d3d10core=b;winemetal=b;nvngx=b;nvapi64=b"',
            rendered,
        )
        self.assertIn("export DXMT_ENABLE_NVEXT=1", rendered)

    def test_base_wrapper_does_not_inherit_nvext(self) -> None:
        rendered = stage_owned_graphics.render_wrapper(False, False)
        self.assertIn("unset DXMT_ENABLE_NVEXT", rendered)
        self.assertNotIn("export DXMT_ENABLE_NVEXT=1", rendered)

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
        self.assertEqual((self.dest / "bin/wine").read_text(), stage_owned_graphics.render_wrapper())
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
        header = bytearray(0x40)
        header[:2] = b"MZ"
        header[0x3C:0x40] = (0x40).to_bytes(4, "little")
        return bytes(header) + b"PE\0\0" + machine.to_bytes(2, "little") + b"\0" * 18 + payload

    def test_wow64_modules_are_staged_machine_checked_and_reverified(self) -> None:
        dxmt32 = self.root / "dxmt-install32"
        (dxmt32 / stage_owned_graphics.DXMT_WOW64_DIR).mkdir(parents=True)
        (self.wine / stage_owned_graphics.WOW64_WINDOWS_DIR).mkdir(parents=True)
        for name in stage_owned_graphics.WINDOWS_MODULES:
            payload = name.encode() + b"-" + stage_owned_graphics.WINE_BUILTIN_MARKER
            if name == "d3d12.dll":
                payload += b"-" + stage_owned_graphics.OWNED_D3D12_MARKER
            (dxmt32 / stage_owned_graphics.DXMT_WOW64_DIR / name).write_bytes(
                self.pe_image(stage_owned_graphics.PE_MACHINE_I386, payload))
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
            self.assertIn(str(stage_owned_graphics.WOW64_WINDOWS_DIR / "d3d12.dll"), manifest["artifacts"])
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
