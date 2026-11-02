#!/usr/bin/env python3
"""Compile the actual access/VA/native-state implementations with sanitizers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="submission-foundation-", dir=root / "build") as temporary:
    output = Path(temporary)
    for name, extra in (
        ("submission-foundation", []),
        ("native-state-cache", ["src/d3dalloy/d3dalloy_sampler.cpp"]),
    ):
        command = ["clang++", "-std=c++20", "-fms-extensions", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                   "-isystem", "include/native/windows", "-isystem", "include/native/directx",
                   "-Isrc/d3d12", "-Isrc/d3dalloy", "-Isrc/util", "-Isrc/winemetal",
                   f"graphics-translator/tests/{name}.test.cpp", *extra, "-o", str(output / name)]
        subprocess.run(command, cwd=root, check=True)
        subprocess.run([str(output / name)], cwd=root, check=True, timeout=90)
