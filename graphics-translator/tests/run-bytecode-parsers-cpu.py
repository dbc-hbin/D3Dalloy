#!/usr/bin/env python3
"""Host-only probe of the production DXBC/RDAT/pipeline-library parsers under ASan/UBSan."""
from pathlib import Path
import argparse
import os
import platform
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", type=Path)
parser.add_argument("--wine-include", type=Path, default=root / "toolchains/wine-sdk/include/wine/windows",
                    help="directory with a d3d12.h that declares DXR types (the vendored one predates them)")
args = parser.parse_args()
arch = "arm64" if platform.machine() == "arm64" else "x86_64"
target = {"arm64": "aarch64-apple-darwin", "x86_64": "x86_64-apple-darwin"}[arch]
sanitize = "-fsanitize=address,undefined"
if args.out:
    args.out.mkdir(parents=True, exist_ok=False)
with tempfile.TemporaryDirectory(prefix="bytecode-parsers-") as temporary:
    directory = args.out.resolve() if args.out else Path(temporary)
    codec = directory / "libshader_bytecode.a"
    subprocess.run(["rustc", "--edition=2021", "--crate-type=staticlib", "-Cpanic=abort", "-Copt-level=2",
                    "--target", target, "src/airconv/shader_bytecode.rs", "-o", str(codec)], cwd=root, check=True)
    executable = directory / "bytecode-parsers"
    sources = ["graphics-translator/tests/bytecode-parsers.test.cpp", "libs/DXBCParser/DXBCUtils.cpp",
               "libs/DXBCParser/BlobContainer.cpp", "libs/DXBCParser/ShaderBinary.cpp",
               "src/d3d12/d3d12_dxil_subobjects.cpp"]
    subprocess.run(["/usr/bin/clang++", "-std=c++20", "-arch", arch, "-O1", "-g", sanitize, "-fno-sanitize-recover=all",
                    "-fms-extensions", "-Wall", "-Wextra", "-Werror", "-Wno-unknown-pragmas", "-Wno-unused-parameter",
                    "-isystem", "include/native/windows", "-isystem", "include/native/directx",
                    "-include", str(args.wine_include / "d3d12.h"), "-Ilibs", *sources, str(codec),
                    "-o", str(executable)], cwd=root, check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    subprocess.run([str(executable), "graphics-translator/tests/dxil/raytracing-stateobj.dxil"], cwd=root,
                   env=env, check=True, timeout=60)
