#!/usr/bin/env python3
"""CPU regression using production depth/stencil methods and WMT ownership."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "src/d3d12/d3d12_pipeline_graphics.cpp").read_text()


def declaration(marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="depth-stencil-cpu-", dir=root / "build") as temporary:
    directory = Path(temporary)
    maps = directory / "production-maps.inc"
    maps.write_text("\n".join(declaration(marker) + ";" for marker in (
        "constexpr WMTCompareFunction kCompareFunctionMap[]",
        "constexpr WMTStencilOperation kStencilOperationMap[]")))
    methods = directory / "production-methods.inc"
    methods.write_text("void\n" + declaration("InitializeDSSO(") +
                       "\nvirtual WMT::DepthStencilState\n" + declaration("GetDepthStencilState(UINT"))
    executable = directory / "depth-stencil-cpu"
    command = ["clang++", "-std=c++20", "-fms-extensions", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
               f'-DPRODUCTION_MAPS="{maps}"', f'-DPRODUCTION_METHODS="{methods}"',
               "-isystem", "include/native/windows", "-isystem", "include/native/directx",
               "-Isrc/winemetal", "graphics-translator/tests/depth-stencil-state.test.cpp",
               "-o", str(executable)]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(executable)], cwd=root, check=True, timeout=60)
