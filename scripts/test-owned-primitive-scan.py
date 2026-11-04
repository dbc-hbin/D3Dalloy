#!/usr/bin/env python3
"""Validate the actual strip/tessellation helper MSL against independent serial GPU-output oracles."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="fresh evidence directory")
    parser.add_argument("--skip-large", action="store_true", help="omit the 16,777,217-slot third-level GPU case")
    parser.add_argument("--timeout", type=int, default=600, help="GPU probe timeout in seconds")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output.exists():
        parser.error(f"--output must not exist yet: {output}")
    source = root / "src/d3d12/d3d12_command_list.cpp"
    header = root / "src/d3d12/owned_primitive_scan.hpp"
    fixture = root / "graphics-translator/tests/primitive-scan-probe.mm"
    text = source.read_text()
    section = text[text.index("bool EnsurePredicationPipelines()") :]
    match = re.search(r'constexpr auto source = R"\((.*?)\)";', section, re.DOTALL)
    if not match:
        raise SystemExit("cannot extract EnsurePredicationPipelines helper MSL")
    output.mkdir(parents=True)
    metal_source = output / "helpers.metal"
    metal_source.write_text(match.group(1))
    binary = output / "primitive-scan-probe"
    compile_command = ["xcrun", "clang++", "-std=c++20", "-fobjc-arc", "-O2",
                       "-I" + str(header.parent), str(fixture), "-framework", "Metal",
                       "-framework", "Foundation", "-o", str(binary)]
    run_command = [str(binary), str(metal_source), *(["--skip-large"] if args.skip_large else [])]
    result = {"status": "fail", "large_case": not args.skip_large,
              "compile_command": compile_command, "run_command": run_command,
              "source_sha256": {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in (source, header, fixture, Path(__file__).resolve())},
              "metal_sha256": hashlib.sha256(metal_source.read_bytes()).hexdigest()}
    started = time.monotonic()
    try:
        compiled = subprocess.run(compile_command, cwd=root, capture_output=True, text=True, timeout=120)
        (output / "build.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise RuntimeError(f"probe compilation failed: exit={compiled.returncode}")
        environment = os.environ | {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"}
        run = subprocess.run(run_command, cwd=root, env=environment, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=args.timeout)
        (output / "gpu.log").write_text(run.stdout)
        print(run.stdout, end="")
        result["exit_code"] = run.returncode
        passed = re.search(r"^PRIMITIVE_SCAN_PROBE_PASS cases=(\d+) max_levels=(\d+)$", run.stdout, re.MULTILINE)
        cases = re.findall(r"^PRIMITIVE_SCAN_CASE_PASS .+$", run.stdout, re.MULTILINE)
        result["cases"] = cases
        validation = ("Metal API Validation Enabled" in run.stdout and "Metal GPU Validation Enabled" in run.stdout)
        validation_errors = any(x in run.stdout.lower() for x in
                                ("validation error", "shader validation failed",
                                 "execution of the command buffer was aborted"))
        native = bool(re.search(r"NATIVE_PROCESS native_arch=(arm64|x86_64) proc_translated=0", run.stdout))
        if (run.returncode or not passed or int(passed[1]) != len(cases) or
                int(passed[2]) != (2 if args.skip_large else 3) or not validation or validation_errors or not native):
            raise RuntimeError("GPU output, validation, native-process, or case-completeness check failed")
        result.update(status="pass", case_count=len(cases), max_levels=int(passed[2]), validation=True)
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        if isinstance(error, subprocess.TimeoutExpired) and error.stdout:
            partial = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else error.stdout
            (output / ("gpu.log" if error.cmd == run_command else "build.log")).write_text(partial)
        result["error"] = str(error)
        print(f"OWNED_PRIMITIVE_SCAN_FAIL {error}", flush=True)
    finally:
        result["elapsed_seconds"] = time.monotonic() - started
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    if result["status"] == "pass":
        print(f"OWNED_PRIMITIVE_SCAN_PASS {output}", flush=True)
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
