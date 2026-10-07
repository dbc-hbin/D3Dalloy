#!/usr/bin/env python3
"""Verify native Wine dependency configuration, closure and relocation in isolation."""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
ROOT = Path(__file__).resolve().parents[1]
BUILD_SPEC = importlib.util.spec_from_file_location(
    "owned_wine_arm64_runtime", Path(__file__).with_name("build-owned-wine-arm64-runtime.py"))
assert BUILD_SPEC and BUILD_SPEC.loader
runtime_builder = importlib.util.module_from_spec(BUILD_SPEC)
BUILD_SPEC.loader.exec_module(runtime_builder)


class RuntimeTests(unittest.TestCase):
    def test_native_dependencies_require_configure_support_and_verified_arm64_closure(self) -> None:
        with tempfile.TemporaryDirectory(dir=ROOT / "build/local") as temporary:
            base = Path(temporary)
            config = base / "config.h"
            for library_name, filename in (("GNUTLS", "libgnutls.30.dylib"), ("FREETYPE", "libfreetype.6.dylib")):
                with self.subTest(library=library_name):
                    config.write_text(f"/* #undef SONAME_LIB{library_name} */\n")
                    with self.assertRaises(ValueError):
                        runtime_builder.require_library(config, library_name)
                    config.write_text(f'#define SONAME_LIB{library_name} "/build/{filename}"\n')
                    self.assertEqual(runtime_builder.require_library(config, library_name), "@loader_path/" + filename)
                    self.assertEqual(config.read_text(), f'#define SONAME_LIB{library_name} "@loader_path/{filename}"\n')
            prefix = base / "prefix"
            libraries = prefix / "lib"
            libraries.mkdir(parents=True)
            destination = base / "relocated"
            destination.mkdir()
            dependency = libraries / "libdependency.dylib"
            library = libraries / "libgnutls.30.dylib"
            source = base / "dependency.c"
            source.write_text("int dependency(void) { return 42; }\n")
            subprocess.run(["/usr/bin/clang", "-arch", "arm64", "-dynamiclib", str(source),
                            "-Wl,-install_name," + str(dependency), "-o", str(dependency)], check=True)
            source.write_text("extern int dependency(void); int tls_probe(void) { return dependency(); }\n")
            subprocess.run(["/usr/bin/clang", "-arch", "arm64", "-dynamiclib", str(source), str(dependency),
                            "-Wl,-install_name," + str(library), "-o", str(library)], check=True)
            provenance = {"artifacts": [{"path": str(path.relative_to(prefix)),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in (library, dependency)]}
            (prefix / "provenance.json").write_text(json.dumps(provenance))

            def run(command, name):
                return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)

            result = runtime_builder.stage_dependencies(prefix, destination, "@loader_path/" + library.name, run)
            self.assertEqual({Path(entry["path"]).name for entry in result["artifacts"]},
                             {library.name, dependency.name})
            for entry in result["artifacts"]:
                self.assertEqual(entry["arch"], "arm64")
                self.assertEqual(entry["sha256"], hashlib.sha256(Path(entry["path"]).read_bytes()).hexdigest())
            dependencies = run(["/usr/bin/otool", "-L", str(destination / library.name)], "dependencies")
            self.assertIn("@loader_path/libdependency.dylib", dependencies)
            self.assertNotIn(str(prefix), dependencies)
            probe = subprocess.check_output([sys.executable, "-c",
                "import ctypes,sys; print(ctypes.CDLL(sys.argv[1]).tls_probe())",
                str(destination / library.name)], text=True)
            self.assertEqual(probe.strip(), "42")
            source.write_text("extern int tls_probe(void); int main(void) { return tls_probe() != 42; }\n")
            font_tool = destination / "font-tool"
            subprocess.run(["/usr/bin/clang", "-arch", "arm64", str(source),
                            str(destination / library.name), "-o", str(font_tool)], check=True)
            subprocess.run([str(font_tool)], check=True)
            library.write_bytes(library.read_bytes() + b"tampered")
            with self.assertRaises(ValueError):
                runtime_builder.stage_dependencies(prefix, destination, library.name, run)
            with mock.patch.object(runtime_builder, "macho_architectures", return_value=["x86_64"]):
                with self.assertRaises(ValueError):
                    runtime_builder.stage_dependencies(prefix, destination, dependency.name, run)


if __name__ == '__main__':
    unittest.main()
