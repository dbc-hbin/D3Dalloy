#!/usr/bin/env python3
"""Run production DXIL arithmetic lowering and metallib output on native Metal.

Usage: python3 scripts/test-owned-airconv-operations.py --build-root BUILD --output FRESH
Uses the configured native compiler's Meson include/link arguments, as the other
native compiler probes do. This is separate from the public D3D12 fixture suite.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build, output = args.build_root.resolve(), args.output.resolve()
    if output.exists():
        parser.error("--output must be fresh")
    targets = json.loads((build / "meson-info/intro-targets.json").read_text())
    target = next(t for t in targets if t["name"] == "airconv" and t["type"] == "executable")
    compiler = next(s for s in target["target_sources"] if s.get("language") == "cpp")
    linker = next(s for s in target["target_sources"] if s.get("linker"))
    fixture = root / "graphics-translator/tests/owned-airconv-operations"
    library = build / "src/airconv/darwin/libairconv.a"
    output.mkdir(parents=True)
    result = {"pass": False, "build": str(build), "cases": [], "commands": [],
              "sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in (fixture.with_suffix(".mm"), fixture.with_suffix(".metal"), library,
                                   root / "src/airconv/dxil_operations.cpp")}}
    commands = [
        ["xcrun", "metal", "-std=metal3.1", "-S", "-emit-llvm", str(fixture.with_suffix(".metal")),
         "-o", str(output / "contract.ll")],
        ["xcrun", "clang++", "-std=c++20", "-fobjc-arc", "-fno-rtti", "-O1",
         *[p for p in compiler["parameters"] if p.startswith("-I")],
         "-I" + str(root / "src/airconv"), str(fixture.with_suffix(".mm")), str(library),
         "-framework", "Metal", "-framework", "Foundation", *linker["parameters"],
         "-o", str(output / "operations")],
    ]
    environment = os.environ | {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"}
    for name in ("bits", "wide", "carry"):
        commands.append([str(output / "operations"), str(output / "contract.ll"), name])
    for index, command in enumerate(commands):
        completed = subprocess.run(command, cwd=build, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, timeout=180)
        log = output / f"{index}.log"
        log.write_text(completed.stdout)
        print(completed.stdout, end="")
        result["commands"].append({"command": command, "exit_code": completed.returncode, "log": str(log)})
        if index >= 2:
            mode = command[-1]
            passed = (completed.returncode == 0 and f"AIRCONV_OPERATIONS mode={mode}" in completed.stdout
                      and "mismatches=0 PASS" in completed.stdout)
            result["cases"].append({"mode": mode, "pass": passed})
        else:
            passed = completed.returncode == 0
        if not passed:
            (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            return 1
    result["pass"] = True
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
