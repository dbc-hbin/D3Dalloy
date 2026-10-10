#!/usr/bin/env python3
"""Host-only automatic-cache probe; keep cache/validation code verbatim, stub external APIs."""
from pathlib import Path
import argparse
from contextlib import nullcontext
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--out", type=Path)
args = parser.parse_args()
source = (root / "src/d3d12/d3d12_pipeline_cache.cpp").read_text()
# Device/key construction needs Windows-only device declarations. It is unrelated
# to automatic-cache concurrency; compile every cache and blob function unchanged.
source = source[source.index("#include <algorithm>"):]
begin = source.index("class KeyBuilder {")
end = source.index("bool PipelineCacheIdentity(")
artifacts = source[source.index("class StageKeyBuilder {"):source.index("struct NativeLibraryArtifact {")]
source = source[:begin] + artifacts + "} // namespace\n\n" + source[end:]
if args.out:
    args.out.mkdir(parents=True, exist_ok=False)
with nullcontext(str(args.out.resolve())) if args.out else tempfile.TemporaryDirectory(prefix="pipeline-cache-cpu-") as temporary:
    directory = Path(temporary)
    production = directory / "production-cache.inc"
    production.write_text(source)
    executable = directory / "pipeline-cache-cpu"
    command = ["clang++", "-std=c++20", "-fms-extensions", "-pthread", "-Wall", "-Wextra", "-Werror",
               f'-DPRODUCTION_CACHE="{production}"',
               "-isystem", "include/native/windows", "-isystem", "include/native/directx", "-Isrc/util", "-Isrc/airconv",
               "-Isrc/winemetal", "-Isrc/d3d12", "graphics-translator/tests/pipeline-cache-locks.test.cpp",
               "src/util/sha1/sha1_util.cpp", "-x", "c++", "src/util/sha1/sha1.c", "-o", str(executable)]
    subprocess.run(command, cwd=root, check=True)
    subprocess.run([str(executable)], cwd=root, check=True, timeout=60)
