#!/usr/bin/env python3
"""Check optional frontend propagation through the real ARM64 package entry point."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("owned_package", ROOT / "scripts/package-owned-arm64.py")
assert SPEC and SPEC.loader
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


class PackageFrontendTests(unittest.TestCase):
    def test_selected_frontends_reach_both_builds_and_staging(self):
        for enabled, missing_paired, complete_paired in (
            (False, False, False), (True, False, False),
            (False, True, False), (True, True, False), (False, False, True),
        ):
            with self.subTest(enabled=enabled, missing_paired=missing_paired, complete_paired=complete_paired), \
                    tempfile.TemporaryDirectory(dir=ROOT / "build/local") as directory:
                base = Path(directory)
                wine = base / "wine"
                wine.mkdir()
                (wine / "runtime-result.json").write_text(json.dumps({
                    "status": "pass", "native_arch": "arm64", "windows_arch": "aarch64",
                    "windows_abi": "arm64x" if missing_paired or complete_paired else "arm64",
                    "identities": [], "source_commit": "test",
                    "source_archive_sha256": "test",
                }))
                commands = []

                def run(command, **kwargs):
                    commands.append(command)
                    if complete_paired and command[:2] == ["meson", "install"]:
                        (base / "package/owned-install/aarch64-windows").mkdir(parents=True, exist_ok=True)
                    return subprocess.CompletedProcess(command, 0)

                arguments = [str(ROOT / "scripts/package-owned-arm64.py"), "--wine-build", str(wine),
                             "--output", str(base / "package"), "--with-wow64"]
                if enabled:
                    arguments += ["--with-nvapi", "--with-nvngx", "--with-fsr"]
                if missing_paired or complete_paired:
                    paired = base / "paired"
                    paired.mkdir()
                    artifacts = []
                    if enabled or complete_paired:
                        image = paired / "amdxc64.dll"
                        image.write_bytes(b"paired Anti-Lag 2")
                        artifacts.append({"path": str(image), "sha256": package.hashlib.sha256(image.read_bytes()).hexdigest()})
                    (paired / "result.json").write_text(json.dumps({
                        "status": "pass", "windows_abi": "arm64x", "artifacts": artifacts,
                        "owned_source_cut": str(base / "package/source-cut.json"),
                    }))
                    arguments += ["--paired-build", str(paired)]
                with patch.object(sys, "argv", arguments), patch.object(package.subprocess, "run", side_effect=run), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(package.main(), 1 if missing_paired else 0)
                if missing_paired:
                    result = json.loads((base / "package/package-result.json").read_text())
                    self.assertEqual(result["blocker"], "paired build is missing required or selected frontends")
                    self.assertFalse(any("stage" in command for command in commands))
                    continue
                builds = [command for command in commands if command[:2] == ["meson", "setup"]]
                self.assertEqual(len(builds), 2)
                stage = next(command for command in commands if "stage" in command)
                for feature in ("nvapi", "nvngx", "fsr"):
                    for command in builds:
                        self.assertIn(f"-Denable_{feature}={str(enabled).lower()}", command)
                    self.assertEqual(f"--with-{feature}" in stage, enabled)
                self.assertIn("--d3dalloy-install32", stage)
                if complete_paired:
                    self.assertEqual((base / "package/owned-install/aarch64-windows/amdxc64.dll").read_bytes(),
                                     b"paired Anti-Lag 2")


if __name__ == "__main__":
    unittest.main()
