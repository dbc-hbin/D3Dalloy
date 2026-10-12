#!/usr/bin/env python3
"""Compile production automatic disk-cache code verbatim; run real WinAPI without a GPU."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runtime", type=Path, required=True)
parser.add_argument("--out", type=Path, required=True)
parser.add_argument("--source", type=Path)
parser.add_argument("--compiler-bin", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal/bin"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=False)
runtime = args.runtime.resolve()
source_path = args.source or root / "src/d3d12/d3d12_shader_cache.cpp"
source = source_path.read_text()
# Exclude application sessions and Device COM wrappers, not any automatic-cache operation.
helpers = source[source.index("struct SearchHandle {"):source.index("struct Cache {")]
private = source[source.index("std::wstring Hex("):source.index("class ShaderCacheSession final")]
public = source[source.index("HRESULT ReadAutomaticPipelineCache("):source.index("HRESULT CreateShaderCacheSession(")]
production = out / "production-cache.inc"
production.write_text("namespace dxmt {\nnamespace {\n" + helpers + private + "\n}\n" + public + "\n}\n")
(out / "source.sha256").write_text(hashlib.sha256(source.encode()).hexdigest() + "\n")
executable = out / "automatic-cache-disk.exe"
command = [str(args.compiler_bin / "x86_64-w64-mingw32-clang++"), "-std=c++20", "-O2",
           "-Wall", "-Wextra", "-Werror", "-static", f'-DPRODUCTION_CACHE="{production}"',
           "-Isrc/util", "-Isrc/airconv", "-Isrc/d3d12", "-Isrc/winemetal", "graphics-translator/tests/automatic-cache-disk.test.cpp",
           "src/util/sha1/sha1_util.cpp", "-x", "c++", "src/util/sha1/sha1.c", "-ld3dcompiler", "-o", str(executable)]
with (out / "build.log").open("w") as log:
    subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
prefix = out / "prefix"
temporary = out / "temp"
temporary.mkdir()
windows_temp = "Z:" + str(temporary).replace("/", "\\")
env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG="-all", WINEDLLOVERRIDES="mscoree,mshtml=",
           TEMP=windows_temp, TMP=windows_temp)
try:
    with (out / "run.log").open("w") as log:
        subprocess.run([str(runtime / "bin/wine"), str(executable), "--operations"], cwd=root, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
        subprocess.run([str(runtime / "bin/wine"), str(executable)], cwd=root, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
finally:
    subprocess.run([str(runtime / "bin/wineserver"), "-k"], env=env, check=True, timeout=15)
    subprocess.run([str(runtime / "bin/wineserver"), "-w"], env=env, check=True, timeout=30)
    shutil.rmtree(prefix)
    shutil.rmtree(temporary)
