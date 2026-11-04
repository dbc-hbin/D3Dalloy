#!/usr/bin/env python3
"""Check per-export AIR definitions using the configured native compiler library."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True, help="native Meson build with libairconv.a")
    parser.add_argument("--out", type=Path, required=True, help="fresh evidence directory")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build, output = args.build_root.resolve(), args.out.resolve()
    if output.exists():
        parser.error(f"--out must not exist: {output}")
    targets = json.loads((build / "meson-info/intro-targets.json").read_text())
    target = next(t for t in targets if t["name"] == "airconv" and t["type"] == "executable")
    compiler = next(s for s in target["target_sources"] if s.get("language") == "cpp")
    linker = next(s for s in target["target_sources"] if s.get("linker"))
    library = build / "src/airconv/darwin/libairconv.a"
    fixture = root / "graphics-translator/tests/metallib-export-closure.test.cpp"
    if not library.is_file():
        parser.error(f"build the native compiler first: {library}")
    output.mkdir(parents=True)
    binary = output / "metallib-export-closure"
    command = [*compiler["compiler"], *compiler["parameters"], "-I" + str(root / "src/airconv"),
               str(fixture), str(library), *linker["parameters"], "-o", str(binary)]
    with (output / "build.log").open("w") as log:
        compiled = subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    result = {"command": command, "build_exit_code": compiled.returncode, "pass": False,
              "inputs_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in (fixture, library, root / "src/airconv/metallib_writer.cpp")}}
    if compiled.returncode == 0:
        run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output / "cpu.log").write_text(run.stdout)
        print(run.stdout, end="")
        expected = [f"METALLIB_EXPORT_CLOSURE_PASS mode={mode} baseline=0 " for mode in range(4)]
        lines = run.stdout.splitlines()
        result.update(exit_code=run.returncode, **{"pass": run.returncode == 0 and len(lines) == 4 and
                      all(line.startswith(prefix) for line, prefix in zip(lines, expected))})
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"{'PASS' if result['pass'] else 'FAIL'} metallib export closure: {output}")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
