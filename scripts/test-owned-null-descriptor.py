#!/usr/bin/env python3
"""Native owned-compiler GPU proof for the D3D12 null / out-of-range descriptor contract.

Builds graphics-translator/tests/dxil/null-descriptor-probe.mm against the
libairconv.a of a configured Meson build root and runs it on the real Metal
device. Include and link flags come from the Meson targets that build
libairconv.a, so the probe is compiled in the same header/link environment as the
library it links; the caller supplies no LLVM paths.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path,
                        default=Path("build/local/build"),
                        help="Meson build directory that configured and built libairconv.a")
    parser.add_argument("--output", type=Path,
                        help="fresh directory receiving the probe and gpu.log "
                             "(default: <build-root>/probes/null-descriptor)")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    build = (args.build_root if args.build_root.is_absolute()
             else source / args.build_root).resolve()
    output = (args.output or build / "probes/null-descriptor").resolve()
    if output.exists():
        parser.error(f"--output must not exist yet: {output}")
    targets_path = build / "meson-info/intro-targets.json"
    if not targets_path.is_file():
        raise SystemExit(f"OWNED_NULL_DESCRIPTOR_FAIL {targets_path} is missing; "
                         f"configure and build {build} first")

    fixture = source / "graphics-translator/tests/dxil"
    shader = fixture / "null-descriptor.dxil"
    if not shader.is_file():
        raise SystemExit(f"OWNED_NULL_DESCRIPTOR_FAIL {shader} is missing")
    binary = output / "null-descriptor-probe"
    output.mkdir(parents=True)

    targets = json.loads(targets_path.read_text())
    target = next((t for t in targets
                   if t["name"] == "airconv" and t["type"] == "executable"), None)
    if target is None:
        raise SystemExit(f"OWNED_NULL_DESCRIPTOR_FAIL no airconv executable target in {targets_path}")
    compiler = next((s for s in target["target_sources"] if s.get("language") == "cpp"), None)
    linker = next((s for s in target["target_sources"] if s.get("linker")), None)
    if compiler is None or linker is None:
        raise SystemExit(f"OWNED_NULL_DESCRIPTOR_FAIL airconv target in {targets_path} "
                         "carries no compiler or linker parameters")

    subprocess.run([
        "xcrun", "clang++", "-arch", "x86_64", "-std=c++20", "-fobjc-arc", "-O1",
        *[p for p in compiler["parameters"] if p.startswith("-I")],
        "-I" + str(source / "src/airconv"), "-I" + str(fixture),
        str(fixture / "null-descriptor-probe.mm"),
        str(build / "src/airconv/darwin/libairconv.a"),
        "-framework", "Metal", "-framework", "Foundation", *linker["parameters"],
        "-o", str(binary)], cwd=build, check=True)

    # Validation is part of this proof, not merely successful compilation.
    environment = os.environ | {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"}
    result = subprocess.run([str(binary), str(shader)], env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (output / "gpu.log").write_text(result.stdout)
    print(result.stdout, end="")
    if result.returncode != 0 or "NULL_DESCRIPTOR_PROBE_PASS" not in result.stdout:
        print(f"OWNED_NULL_DESCRIPTOR_FAIL exit={result.returncode} {output}", flush=True)
        return result.returncode or 1
    print(f"OWNED_NULL_DESCRIPTOR_PASS {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
