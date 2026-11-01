#!/usr/bin/env python3
"""Compile the production application Cache verbatim and run without a graphics device."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--compiler-bin", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal/bin"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
runtime = args.runtime.resolve()
source = (root / "src/d3d12/d3d12_shader_cache.cpp").read_text()
source = source[source.index("#include <algorithm>"):source.index("std::map<std::wstring, std::weak_ptr<Cache>> registry;")]
production = out / "production-cache.inc"
production.write_text(source + "\n} // namespace\n} // namespace d3dalloy\n")
for abi, target in (("x64", "x86_64"), ("x86", "i686")):
    executable = out / f"shader-cache-index-{abi}.exe"
    subprocess.run([str(args.compiler_bin / f"{target}-w64-mingw32-clang++"), "-std=c++20", "-O2",
                    "-Wall", "-Wextra", "-Werror", "-static", f'-DPRODUCTION_CACHE="{production}"',
                    "-Isrc/util", "graphics-translator/tests/shader-cache-index.test.cpp",
                    "src/util/sha1/sha1_util.cpp", "-x", "c++", "src/util/sha1/sha1.c", "-o", str(executable)],
                   cwd=root, check=True)
    env = dict(os.environ, WINEPREFIX=str(out / f"prefix-{abi}"), WINEDEBUG="-all")
    try:
        subprocess.run([str(runtime / "bin/wine"), str(executable)], cwd=root, env=env, check=True, timeout=90)
    finally:
        subprocess.run([str(runtime / "bin/wineserver"), "-k"], env=env, timeout=15)
        subprocess.run([str(runtime / "bin/wineserver"), "-w"], env=env, check=True, timeout=30)
