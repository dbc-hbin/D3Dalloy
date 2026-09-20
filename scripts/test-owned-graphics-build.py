#!/usr/bin/env python3
"""CPU-only tests for the reproducible owned graphics build driver."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("owned_graphics_build", ROOT / "scripts/build-owned-graphics.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)


def run(*args: str, cwd: Path) -> str:
    completed = subprocess.run(args, cwd=cwd, check=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    return completed.stdout.strip()


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class OwnedGraphicsBuildTests(unittest.TestCase):
    def make_upstream(self, root: Path) -> tuple[Path, str]:
        upstream = root / "upstream"
        upstream.mkdir()
        run("git", "init", "-q", cwd=upstream)
        run("git", "config", "user.email", "fixture@example.invalid", cwd=upstream)
        run("git", "config", "user.name", "fixture", cwd=upstream)
        (upstream / "value.txt").write_text("base\n")
        (upstream / "build-win64.txt").write_text("[host_machine]\nsystem='windows'\n")
        run("git", "add", ".", cwd=upstream)
        run("git", "commit", "-q", "-m", "fixture", cwd=upstream)
        return upstream, run("git", "rev-parse", "HEAD", cwd=upstream)

    def make_patch_root(self, root: Path) -> tuple[Path, list[dict[str, str]]]:
        repository = root / "owner"
        patch_dir = repository / "graphics-translator/patches"
        patch_dir.mkdir(parents=True)
        patches = [
            ("0001-one.patch", "base", "one"),
            ("0002-two.patch", "one", "two"),
        ]
        series = []
        for name, old, new in patches:
            path = patch_dir / name
            path.write_text(
                "diff --git a/value.txt b/value.txt\n"
                "--- a/value.txt\n"
                "+++ b/value.txt\n"
                "@@ -1 +1 @@\n"
                f"-{old}\n"
                f"+{new}\n"
            )
            series.append({"file": name, "sha256": digest(path)})
        return repository, series

    def fixture_lock(self, upstream: Path, commit: str, series: list[dict[str, str]]) -> dict:
        return {
            "schema": 1,
            "upstream": {"url": str(upstream), "commit": commit, "submodules": []},
            "patches": {
                "directory": "graphics-translator/patches",
                "reject_unlisted": True,
                "series": series,
            },
            "build": {
                "cross_file": "build-win64.txt",
                "buildtype": "debugoptimized",
                "meson_options": {"enable_d3d12": True, "enable_nvapi": False, "enable_nvngx": True},
                "native_build_compilers": {"CC_FOR_BUILD": "/usr/bin/clang", "CXX_FOR_BUILD": "/usr/bin/clang++"},
            },
            "tools": {
                "meson": {"version": "fixture"},
                "ninja": {"version_prefix": "fixture"},
                "llvm_mingw": {"compiler_version_contains": "fixture", "target_contains": "fixture", "files": {}},
                "native_llvm": {"version": "15.0.7", "source_url": str(upstream), "source_commit": commit,
                                "source_tag": "fixture", "files": {}, "bootstrap": {"cmake_options": []}},
                "wine_sdk": {"files": {}},
            },
        }

    def test_clean_pin_and_locked_patch_order_are_real_git_operations(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            upstream, commit = self.make_upstream(root)
            repository, series = self.make_patch_root(root)
            lock = self.fixture_lock(upstream, commit, series)
            records = build.validate_patch_series(lock, repository)
            destination = root / "result/source"
            destination.parent.mkdir()
            log = root / "commands.log"
            log.write_text("")
            commands = []
            source = build.prepare_source(destination, lock, records, upstream, dict(os.environ), commands, log)

            self.assertEqual((destination / "value.txt").read_text(), "two\n")
            self.assertEqual(run("git", "rev-parse", "HEAD", cwd=destination), commit)
            self.assertEqual([record["file"] for record in records], ["0001-one.patch", "0002-two.patch"])
            self.assertEqual([record["step"] for record in commands if record["step"].startswith("apply-")],
                             ["apply-0001-one.patch", "apply-0002-two.patch"])
            self.assertEqual(source["commit"], commit)
            self.assertEqual(source["submodules"], [])
            self.assertEqual(source["acquisition"]["mode"], "local-ref")
            self.assertEqual(source["acquisition"]["head"], commit)
            self.assertTrue(source["staged_diff_sha256"])
            self.assertIn("M\tvalue.txt", source["staged_name_status"])
            verified = build.verify_patched_source(destination, lock, source["staged_diff_sha256"])
            self.assertEqual(verified["commit"], source["commit"])
            self.assertEqual(verified["staged_diff_sha256"], source["staged_diff_sha256"])

    def test_dirty_local_source_ref_is_refused_before_clone(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            upstream, commit = self.make_upstream(root)
            (upstream / "untracked.txt").write_text("dirty\n")
            with self.assertRaisesRegex(build.BuildError, "dirty"):
                build.ensure_clean_git_ref(upstream, commit)

    def test_unlisted_patch_and_hash_drift_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            upstream, commit = self.make_upstream(root)
            repository, series = self.make_patch_root(root)
            lock = self.fixture_lock(upstream, commit, series)
            extra = repository / "graphics-translator/patches/9999-extra.patch"
            extra.write_text("extra\n")
            with self.assertRaisesRegex(build.BuildError, "unlisted"):
                build.validate_patch_series(lock, repository)
            extra.unlink()
            first = repository / "graphics-translator/patches/0001-one.patch"
            first.write_text(first.read_text() + "# drift\n")
            with self.assertRaisesRegex(build.BuildError, "hash mismatch"):
                build.validate_patch_series(lock, repository)

    def test_existing_root_and_invalid_resume_are_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "owned"
            root.mkdir()
            with self.assertRaisesRegex(build.BuildError, "overwrite"):
                build.ensure_output_root(root, False)
            with self.assertRaisesRegex(build.BuildError, build.MANIFEST_NAME):
                build.ensure_output_root(root, True)
            (root / build.MANIFEST_NAME).write_text("{}\n")
            build.ensure_output_root(root, True)

    def test_unstaged_source_drift_breaks_resume_fingerprint(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            upstream, commit = self.make_upstream(root)
            repository, series = self.make_patch_root(root)
            lock = self.fixture_lock(upstream, commit, series)
            records = build.validate_patch_series(lock, repository)
            destination = root / "result/source"
            destination.parent.mkdir()
            log = root / "commands.log"
            log.write_text("")
            source = build.prepare_source(destination, lock, records, upstream, dict(os.environ), [], log)
            (destination / "value.txt").write_text("unrecorded\n")
            with self.assertRaisesRegex(build.BuildError, "unstaged"):
                build.verify_patched_source(destination, lock, source["staged_diff_sha256"])

    def test_meson_command_generation_is_deterministic_and_isolated(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            upstream, commit = self.make_upstream(root)
            _, series = self.make_patch_root(root)
            lock = self.fixture_lock(upstream, commit, series)
            source = root / "source"
            source.mkdir()
            (source / "build-win64.txt").write_text("cross\n")
            commands = build.make_meson_commands(
                lock, Path("/tools/meson"), source, root / "build", root / "install",
                root / "llvm", root / "wine-sdk", 7,
            )
            self.assertEqual([step for step, _ in commands], ["meson-setup", "meson-compile", "meson-install"])
            setup = commands[0][1]
            self.assertEqual(setup[0], "/tools/meson")
            self.assertIn(str((root / "install").resolve()), setup)
            self.assertIn(f"-Dnative_llvm_path={(root / 'llvm').resolve()}", setup)
            self.assertIn(f"-Dwine_install_path={(root / 'wine-sdk').resolve()}", setup)
            self.assertEqual([arg for arg in setup if arg.startswith("-D")],
                             [f"-Dnative_llvm_path={(root / 'llvm').resolve()}",
                              f"-Dwine_install_path={(root / 'wine-sdk').resolve()}",
                              "-Denable_d3d12=true", "-Denable_nvapi=false", "-Denable_nvngx=true"])
            self.assertEqual(commands[1][1][-2:], ["--jobs", "7"])
            self.assertEqual(commands[2][1][-1], "--no-rebuild")

    def test_submodule_commit_pin_is_verified(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dependency = root / "dependency"
            dependency.mkdir()
            run("git", "init", "-q", cwd=dependency)
            run("git", "config", "user.email", "fixture@example.invalid", cwd=dependency)
            run("git", "config", "user.name", "fixture", cwd=dependency)
            (dependency / "dep.txt").write_text("one\n")
            run("git", "add", ".", cwd=dependency)
            run("git", "commit", "-q", "-m", "one", cwd=dependency)
            first = run("git", "rev-parse", "HEAD", cwd=dependency)
            (dependency / "dep.txt").write_text("two\n")
            run("git", "commit", "-q", "-am", "two", cwd=dependency)
            second = run("git", "rev-parse", "HEAD", cwd=dependency)

            upstream, commit = self.make_upstream(root)
            run("git", "-c", "protocol.file.allow=always", "submodule", "add", "-q", str(dependency), "deps/sub",
                cwd=upstream)
            run("git", "checkout", "-q", first, cwd=upstream / "deps/sub")
            run("git", "add", ".gitmodules", "deps/sub", cwd=upstream)
            run("git", "commit", "-q", "-m", "pin submodule", cwd=upstream)
            commit = run("git", "rev-parse", "HEAD", cwd=upstream)
            _, series = self.make_patch_root(root)
            lock = self.fixture_lock(upstream, commit, series)
            lock["upstream"]["submodules"] = [{"path": "deps/sub", "url": str(dependency), "commit": first}]
            records = build.verify_submodules(upstream, lock)
            self.assertEqual(records[0]["commit"], first)
            run("git", "checkout", "-q", second, cwd=upstream / "deps/sub")
            with self.assertRaisesRegex(build.BuildError, "commit mismatch"):
                build.verify_submodules(upstream, lock)

    def test_expected_file_hash_validation_is_content_based(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "tool/file.bin"
            path.parent.mkdir()
            path.write_bytes(b"pinned")
            expected = {"tool/file.bin": digest(path)}
            records = build.verify_expected_files(root, expected, "fixture")
            self.assertEqual(records[0]["sha256"], expected["tool/file.bin"])
            path.write_bytes(b"drift")
            with self.assertRaisesRegex(build.BuildError, "hash mismatch"):
                build.verify_expected_files(root, expected, "fixture")

    def test_input_fingerprint_changes_on_locked_configuration_change(self):
        base = {"output_root": "/tmp/a", "patches": [{"file": "a", "sha256": "1"}], "build": {"x": 1}}
        changed = json.loads(json.dumps(base))
        changed["build"]["x"] = 2
        self.assertNotEqual(build.stable_json_hash(base), build.stable_json_hash(changed))

    def test_resume_requires_exact_input_contract_and_source_fingerprint(self):
        contract = {"source": "locked", "patches": ["one", "two"]}
        fingerprint = build.stable_json_hash(contract)
        manifest = {"input": contract, "input_fingerprint": fingerprint,
                    "source": {"staged_diff_sha256": "abc"}}
        build.validate_resume_contract(manifest, contract, fingerprint)
        drift = json.loads(json.dumps(contract))
        drift["patches"].append("three")
        with self.assertRaisesRegex(build.BuildError, "contract differs"):
            build.validate_resume_contract(manifest, drift, build.stable_json_hash(drift))
        without_source = json.loads(json.dumps(manifest))
        without_source["source"] = {}
        with self.assertRaisesRegex(build.BuildError, "staged source fingerprint"):
            build.validate_resume_contract(without_source, contract, fingerprint)

    def test_fat_macho_architecture_headers_are_not_dependencies(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "library.dylib"
            path.write_bytes(b"fat")
            output = (
                "library.dylib (architecture x86_64):\n"
                "\t@rpath/library.dylib (compatibility version 0.0.0, current version 0.0.0)\n"
                "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1359.0.0)\n"
                "library.dylib (architecture arm64):\n"
                "\t@rpath/library.dylib (compatibility version 0.0.0, current version 0.0.0)\n"
                "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1359.0.0)\n"
            )
            with mock.patch.object(build, "macho_tool", return_value=output):
                dependencies = build.macho_dependencies(path)
            self.assertEqual(dependencies, ["@rpath/library.dylib", "/usr/lib/libSystem.B.dylib"])
            self.assertEqual(build.macho_dependency_problems(path, "library.dylib"), [])

    def test_absolute_macho_paths_are_refused_and_sanitized(self):
        with tempfile.TemporaryDirectory() as temporary:
            install = Path(temporary) / "install"
            unix = install / "x86_64-unix"
            unix.mkdir(parents=True)
            winemetal = unix / "winemetal.so"
            winemetal.write_bytes(b"macho")
            with mock.patch.object(build, "macho_dependencies", return_value=["@rpath/ntdll.so"]), \
                 mock.patch.object(build, "macho_architectures", return_value=["x86_64"]), \
                 mock.patch.object(build, "macho_rpaths", return_value=["/Users/build/tree", "@loader_path/"]):
                with self.assertRaisesRegex(build.BuildError, "non-relocatable LC_RPATH"):
                    build.verify_installed_runtime_modules(install)
            records: list[list[str]] = []
            with mock.patch.object(build, "macho_rpaths",
                                   side_effect=[["/Users/build/tree", "@loader_path/"], ["@loader_path/"]]), \
                 mock.patch.object(build, "run_logged",
                                   side_effect=lambda command, **kwargs: records.append(command)):
                build.sanitize_runtime_modules(install, dict(os.environ), [],
                                               Path(temporary) / "commands.log")
            self.assertEqual(records, [["/usr/bin/install_name_tool", "-delete_rpath", "/Users/build/tree", str(winemetal)]])

    def test_installed_native_module_rejects_foreign_compiler_dependencies(self):
        base = ["@rpath/winemac.so", "@rpath/ntdll.so", "/usr/lib/libSystem.B.dylib"]
        foreign = (
            "@rpath/libdxmt-dxilconv.dylib",
            "@rpath/libmetalirconverter.dylib",
            "/System/Library/PrivateFrameworks/D3DMetal.framework/Versions/A/D3DMetal",
            "@rpath/libd3dshared.dylib",
        )
        for dependency in (None, *foreign):
            with self.subTest(dependency=dependency):
                with tempfile.TemporaryDirectory() as temporary:
                    install = Path(temporary) / "install"
                    unix = install / "x86_64-unix"
                    unix.mkdir(parents=True)
                    (unix / "winemetal.so").write_bytes(b"macho")
                    dependencies = base if dependency is None else [*base, dependency]
                    with mock.patch.object(build, "macho_relocation_problems", return_value=[]), \
                         mock.patch.object(build, "macho_architectures", return_value=["x86_64"]), \
                         mock.patch.object(build, "macho_dependencies", return_value=dependencies), \
                         mock.patch.object(build, "macho_rpaths", return_value=[]):
                        if dependency is None:
                            record = build.verify_installed_runtime_modules(install)
                        else:
                            with self.assertRaisesRegex(build.BuildError, "external compiler/SDK library"):
                                build.verify_installed_runtime_modules(install)
                            continue
                    self.assertEqual(record["native_module"]["external_compiler_dependencies"], [])
                    self.assertEqual(record["native_module"]["dependencies"], base)

    def test_native_compiler_record_is_source_pinned_without_sdk_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            install = Path(temporary) / "install"
            unix = install / "x86_64-unix"
            unix.mkdir(parents=True)
            (unix / "winemetal.so").write_bytes(b"macho")
            lock = {"upstream": {"commit": "c" * 40}}
            patches = [{"index": 1, "file": "0001-one.patch", "path": "/patches/0001-one.patch",
                        "sha256": "a" * 64, "size": 12}]
            tools = {"native_llvm": {"version": "15.0.7", "prefix": "/llvm",
                                     "files": [{"path": "include/llvm/Config/llvm-config.h"}]}}
            source = {"staged_diff_sha256": "d" * 64}
            record = build.native_compiler_record(install, lock, patches, tools, source)
            self.assertEqual(record["ownership"], "self-owned")
            self.assertEqual(record["external_dependencies"], [])
            self.assertEqual(record["upstream_commit"], "c" * 40)
            self.assertEqual(record["patch_series"], [{"file": "0001-one.patch", "sha256": "a" * 64}])
            self.assertEqual(record["staged_diff_sha256"], "d" * 64)
            self.assertEqual(record["native_llvm"]["version"], "15.0.7")
            self.assertEqual(record["artifact"]["sha256"], digest(unix / "winemetal.so"))
            self.assertNotIn("sdk", json.dumps(record).lower())

    def test_install_inventory_requires_enabled_owned_components(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in ("x86_64-windows/d3d12.dll", "x86_64-windows/dxgi.dll",
                             "x86_64-unix/winemetal.so", "x86_64-windows/nvapi64.dll"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(relative.encode())
            lock = {"build": {"meson_options": {"enable_nvapi": True, "enable_nvngx": True}}}
            with self.assertRaisesRegex(build.BuildError, "nvngx.dll"):
                build.installed_artifacts(root, lock)
            nvngx = root / "x86_64-windows/nvngx.dll"
            nvngx.write_bytes(b"nvngx")
            inventory = build.installed_artifacts(root, lock)
            self.assertIn("x86_64-windows/nvngx.dll", {item["path"] for item in inventory})

    def test_install_inventory_rejects_removed_external_compiler_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in ("x86_64-windows/d3d12.dll", "x86_64-windows/dxgi.dll",
                             "x86_64-unix/winemetal.so"):
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(relative.encode())
            lock = {"build": {"meson_options": {}}}
            self.assertTrue(build.installed_artifacts(root, lock))
            for relative in ("x86_64-unix/libdxmt-dxilconv.dylib",
                             "x86_64-unix/libmetalirconverter.dylib",
                             "x86_64-unix/licenses/metal-shader-converter/License.rtf"):
                with self.subTest(relative=relative):
                    leftover = root / relative
                    leftover.parent.mkdir(parents=True, exist_ok=True)
                    leftover.write_bytes(b"leftover")
                    with self.assertRaisesRegex(build.BuildError, "removed external DXIL compiler"):
                        build.installed_artifacts(root, lock)
                    leftover.unlink()


if __name__ == "__main__":
    unittest.main()
