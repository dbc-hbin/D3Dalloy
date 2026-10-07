#!/usr/bin/env python3
"""Exercise the production standard/atomic64 table loops without Metal."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="bindless-collection-", dir=root / "build") as temporary:
    directory = Path(temporary)
    for name, path, function in (
        ("standard", "d3d12_command_queue.cpp", "void AppendStandardSwizzleTables("),
        ("atomic", "d3d12_atomic64_phase.cpp", "HRESULT CollectAtomic64PhaseBindings("),
    ):
        source = (root / "src/d3d12" / path).read_text()
        start = source.index("for (const auto &table : tables)", source.index(function))
        end = source.index("{", start) + 1
        depth = 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        (directory / f"{name}.inc").write_text(source[start:end])
    executable = directory / "probe"
    subprocess.run([
        "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        f'-DSTANDARD_LOOP="{directory / "standard.inc"}"',
        f'-DATOMIC_LOOP="{directory / "atomic.inc"}"',
        "graphics-translator/tests/bindless-collection.test.cpp", "-o", str(executable),
    ], cwd=root, check=True)
    subprocess.run([str(executable)], check=True, timeout=60)
