#!/usr/bin/env python3
"""Run the production MSL capture cursor helpers as C++ with address/UB sanitizers.

This checks scheduler arithmetic and lifetime state, not Metal execution or shader output.
Only address-space qualifiers and Metal attributes are removed from selected functions.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


FUNCTIONS = (
    "publish", "fail", "copy", "index", "cut", "primitives", "end_instance", "finish_batch",
    "query_epoch", "command", "expand", "prepare", "consume", "advance", "clear",
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def extract_function(source, name):
    pattern = re.compile(r"^(?:static\s+\w+|\[\[kernel\]\]\s+void)\s+" + re.escape(name) + r"\s*\(",
                         re.MULTILINE)
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise ValueError(f"expected one production definition of {name}, got {len(matches)}")
    # Preserve offsets while ignoring braces inside comments/string literals.
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: "".join("\n" if char == "\n" else " " for char in match.group()), source,
                    flags=re.DOTALL)
    first = masked.index("{", matches[0].start())
    depth = 1
    end = first + 1
    while depth and end < len(masked):
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    if depth:
        raise ValueError(f"unclosed production definition: {name}")
    original = source[matches[0].start():end]
    translated = re.sub(r"\[\[[^\]]*\]\]", "", original)
    translated = re.sub(r"\b(?:device|constant|thread)\b", "", translated)
    return original, translated


PRELUDE = r"""
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include "capture-frontier-pod.hpp"
using uint = uint32_t;
using ulong = uint64_t;
using uchar = unsigned char;
using namespace d3dalloy;
template <typename T, typename U>
static constexpr std::common_type_t<T, U> min(T a, U b) {
  using R = std::common_type_t<T, U>;
  return std::min(R(a), R(b));
}
template <typename T, typename U>
static constexpr std::common_type_t<T, U> max(T a, U b) {
  using R = std::common_type_t<T, U>;
  return std::max(R(a), R(b));
}
struct D3DALLOYSOAssembly {};
[[noreturn]] static void unexpected_application_consumer() {
  std::fputs("capture frontier CPU probe reached an untested application-output consumer\n", stderr);
  std::abort();
}
static void capture_frontier_vertex_consume(CaptureFrontierState &, CaptureFrontierParams &,
                                             const D3DALLOYSOAssembly *) {
  unexpected_application_consumer();
}
static void capture_frontier_mesh_consume(CaptureFrontierState &, CaptureFrontierParams &,
                                           const D3DALLOYSOAssembly *) {
  unexpected_application_consumer();
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++", help="C++20 compiler (for example clang++ or g++)")
    parser.add_argument("--out", type=Path, help="fresh evidence/output directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source_path = root / "src/d3dalloy/d3d12_capture_frontier.metal"
    pod_path = root / "src/d3d12/d3d12_capture_frontier.hpp"
    test_path = Path(__file__).with_name("capture-frontier-state.test.cpp")
    source, pod, test = source_path.read_bytes(), pod_path.read_bytes(), test_path.read_bytes()
    compiler = shutil.which(args.cxx)
    if compiler is None:
        parser.error(f"compiler is unavailable: {args.cxx}")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    out = (args.out or root / "build/local" / f"capture-frontier-cpu-{stamp}").resolve()
    out.mkdir(parents=True, exist_ok=False)
    extracted, functions = [], {}
    for short_name in FUNCTIONS:
        name = "capture_frontier_" + short_name
        original, translated = extract_function(source.decode(), name)
        functions[name] = digest(original.encode())
        extracted.append(translated)
    (out / "capture-frontier-pod.hpp").write_bytes(pod)
    (out / "capture-frontier-extracted.hpp").write_text(PRELUDE + "\n\n".join(extracted) + "\n")
    # Compile the frozen test and headers; workers can continue editing the live source.
    frozen_test = out / test_path.name
    frozen_test.write_bytes(test)
    executable = out / "capture-frontier-state"
    command = [compiler, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
               "-I", str(out), str(frozen_test), "-o", str(executable)]
    version = subprocess.run([compiler, "--version"], text=True, capture_output=True, check=True)
    build = subprocess.run(command, text=True, capture_output=True)
    (out / "build.stdout").write_text(build.stdout)
    (out / "build.stderr").write_text(build.stderr)
    result = {
        "source": str(source_path), "source_sha256": digest(source), "pod_sha256": digest(pod),
        "test_sha256": digest(test), "functions": functions, "compiler": version.stdout.splitlines()[0],
        "command": command, "build_exit": build.returncode, "sanitizers": ["address", "undefined"],
        "leak_detection": False, "gpu_execution": "not_tested",
    }
    if build.returncode == 0:
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        run = subprocess.run([str(executable)], text=True, capture_output=True, env=env)
        (out / "run.stdout").write_text(run.stdout)
        (out / "run.stderr").write_text(run.stderr)
        result.update(run_exit=run.returncode, stdout=run.stdout, stderr=run.stderr)
    result["source_still_current"] = source_path.read_bytes() == source and pod_path.read_bytes() == pod
    result["success"] = (build.returncode == 0 and result.get("run_exit") == 0 and
                         "CAPTURE_FRONTIER_CPU_PASS " in result.get("stdout", ""))
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"success": result["success"], "evidence": str(out / "result.json"),
                      "source_sha256": result["source_sha256"], "source_still_current": result["source_still_current"]}))
    if not result["success"]:
        sys.stderr.write(build.stderr + result.get("stderr", ""))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
