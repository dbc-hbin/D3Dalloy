#!/usr/bin/env python3
"""Exercise D3Dalloy metallib serialization with real linked Metal RT functions.

The public Metal frontend produces an AIR contract fixture only. DXIL production
compilation never invokes it. The library under test is emitted by our writer.

Include and link flags are taken from the Meson target that builds libairconv.a,
so the fixture is compiled in the same header/link environment as the library it
links; no llvm-config derived path is required from the caller.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import json
import os
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path,
                        default=Path("build/local/build"),
                        help="Meson build directory that configured and built libairconv.a")
    parser.add_argument("--output", type=Path, required=True,
                        help="fresh directory receiving contract.ll, the fixture and gpu.log")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root
    build = (args.build_root if args.build_root.is_absolute()
             else root / args.build_root).resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error(f"--output must not exist yet: {output}")
    targets_path = build / "meson-info/intro-targets.json"
    if not targets_path.is_file():
        raise SystemExit(f"OWNED_METALLIB_RT_FAIL {targets_path} is missing; "
                         f"configure and build {build} first")

    fixture = root / "graphics-translator/tests/owned-metallib-rt"
    air = output / "contract.ll"
    binary = output / "owned-metallib-rt"
    output.mkdir(parents=True)
    subprocess.run(["xcrun", "metal", "-std=metal3.1", "-S", "-emit-llvm",
                    str(fixture.with_suffix(".metal")), "-o", str(air)], check=True)

    targets = json.loads(targets_path.read_text())
    target = next((t for t in targets
                   if t["name"] == "airconv" and t["type"] == "executable"), None)
    if target is None:
        raise SystemExit(f"OWNED_METALLIB_RT_FAIL no airconv executable target in {targets_path}")
    compiler = next((s for s in target["target_sources"] if s.get("language") == "cpp"), None)
    linker = next((s for s in target["target_sources"] if s.get("linker")), None)
    if compiler is None or linker is None:
        raise SystemExit(f"OWNED_METALLIB_RT_FAIL airconv target in {targets_path} "
                         "carries no compiler or linker parameters")

    archive = build / "src/airconv/darwin/libairconv.a"
    arches = subprocess.check_output(["/usr/bin/lipo", "-archs", archive], text=True).split()
    subprocess.run([
        "xcrun", "clang++", *(arg for arch in arches for arg in ("-arch", arch)), "-std=c++20", "-fobjc-arc", "-O1",
        *[p for p in compiler["parameters"] if p.startswith("-I")],
        "-I" + str(source / "src/airconv"),
        str(fixture.with_suffix(".mm")), str(archive),
        "-framework", "Metal", "-framework", "Foundation", *linker["parameters"],
        "-o", str(binary)], cwd=build, check=True)

    # Validation is part of this proof, not merely successful compilation.
    environment = os.environ | {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"}
    result = subprocess.run([str(binary), str(air)], env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (output / "gpu.log").write_text(result.stdout)
    print(result.stdout, end="")
    if result.returncode != 0:
        print(f"OWNED_METALLIB_RT_FAIL exit={result.returncode} {output}", flush=True)
        return result.returncode
    print(f"OWNED_METALLIB_RT_PASS {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
