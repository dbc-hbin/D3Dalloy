#!/usr/bin/env python3
"""Native owned-compiler GPU proof for the D3D12 null / out-of-range descriptor contract.

Builds graphics-translator/tests/dxil/null-descriptor-probe.mm against the
libairconv.a, --bridge public API, or --unix-plugin of a configured Meson build root and runs it on the real Metal
device. Include and link flags come from the Meson targets that build
libairconv.a, so the probe is compiled in the same header/link environment as the
library it links; the caller supplies no LLVM paths.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def plugin_images(plugin: Path, wine: Path) -> list[Path]:
    dependencies = [wine / "dlls/ntdll/ntdll.so", wine / "dlls/winemac.drv/winemac.so",
                    wine / "dlls/win32u/win32u.so"]
    # dyld resolves a packaged plugin's imports from its own directory first.
    # Only accept those paths for a manifest-identified owned compiler.
    unix = plugin.parent
    manifest = unix.parent.parent.parent / "owned-graphics-runtime.json"
    if unix.parent.name == "wine" and unix.parent.parent.name == "lib" and manifest.is_file():
        compiler = json.loads(manifest.read_text())["native_compiler"]
        if (manifest.parent / compiler["path"]).resolve() == plugin:
            if compiler["ownership"] != "self-owned" or sha256(plugin) != compiler["sha256"]:
                raise SystemExit(f"OWNED_NULL_DESCRIPTOR_FAIL staged compiler differs from {manifest}")
            staged = [plugin.with_name(p.name) for p in dependencies]
            for path, built in zip(staged, dependencies):
                if not path.is_file() or sha256(path) != sha256(built):
                    raise SystemExit(f"OWNED_NULL_DESCRIPTOR_FAIL staged {path} differs from {built}")
            dependencies = staged
    return [plugin, *dependencies]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path,
                        default=Path("build/local/build"),
                        help="Meson build directory that configured and built libairconv.a")
    parser.add_argument("--output", type=Path,
                        help="fresh directory receiving the probe and gpu.log "
                             "(default: <build-root>/probes/null-descriptor)")
    parser.add_argument("--native-arch", choices=("arm64", "x86_64"), default="x86_64")
    bridge_options = parser.add_mutually_exclusive_group()
    bridge_options.add_argument("--bridge", type=Path, help="link the public compiler API from the built native Metal bridge")
    bridge_options.add_argument("--unix-plugin", type=Path, help="link the real Wine Unix plugin and exercise its dispatch table")
    parser.add_argument("--wine-build", type=Path, help="isolated Wine build supplying actual Unix-plugin dependencies")
    args = parser.parse_args()
    if args.unix_plugin and not args.wine_build:
        parser.error("--unix-plugin requires --wine-build for explicit native dependencies")
    if args.wine_build and not args.unix_plugin:
        parser.error("--wine-build is only valid with --unix-plugin")
    bridge = args.unix_plugin or args.bridge
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
        "xcrun", "clang++", "-arch", args.native_arch, "-std=c++20", "-fobjc-arc", "-O1",
        *(["-DD3DALLOY_UNIX_PLUGIN_PROBE=1" if args.unix_plugin else "-DD3DALLOY_NATIVE_BRIDGE_PROBE=1",
           "-I" + str(source / "src/winemetal")] if bridge else []),
        *[p for p in compiler["parameters"] if p.startswith("-I")],
        "-I" + str(source / "src/airconv"), "-I" + str(fixture),
        str(fixture / "null-descriptor-probe.mm"),
        str(bridge.resolve() if bridge else build / "src/airconv/darwin/libairconv.a"),
        *(["-Wl,-rpath," + str(bridge.resolve().parent)] if bridge else []),
        "-framework", "Metal", "-framework", "Foundation",
        *(linker["parameters"] if not bridge else []),
        "-o", str(binary)], cwd=build, check=True)

    # Validation is part of this proof, not merely successful compilation.
    environment = os.environ | {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1"}
    expected_images = []
    if args.unix_plugin:
        wine = args.wine_build.resolve()
        expected_images = plugin_images(args.unix_plugin.resolve(), wine)
        for image in expected_images:
            subprocess.run(["/usr/bin/lipo", "-verify_arch", args.native_arch, str(image)], check=True)
        environment["DYLD_LIBRARY_PATH"] = os.pathsep.join(str(image.parent) for image in expected_images)
        environment["WINEPREFIX"] = str(output / "prefix")
    result = subprocess.run([str(binary), str(shader)], env=environment,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (output / "gpu.log").write_text(result.stdout)
    print(result.stdout, end="")
    native_verified = args.native_arch != "arm64" or "NATIVE_PROCESS native_arch=arm64 proc_translated=0" in result.stdout
    plugin_verified = not args.unix_plugin or (
        "NATIVE_UNIX_PLUGIN os_version=" in result.stdout and
        "NATIVE_UNIX_PLUGIN_IMAGES checked=" in result.stdout and
        all(any(line.endswith(" path=" + str(image)) for line in result.stdout.splitlines()
                if line.startswith("NATIVE_LOADED_IMAGE ")) for image in expected_images) and
        "compiler_image=" + str(args.unix_plugin.resolve()) + " " in result.stdout and
        "Metal API Validation Enabled" in result.stdout and "Metal GPU Validation Enabled" in result.stdout)
    validation_errors = any(marker in result.stdout.lower() for marker in
                            ("validation error", "shader validation failed", "execution of the command buffer was aborted"))
    if (result.returncode != 0 or "NULL_DESCRIPTOR_PROBE_PASS" not in result.stdout or
            not native_verified or not plugin_verified or validation_errors):
        print(f"OWNED_NULL_DESCRIPTOR_FAIL exit={result.returncode} {output}", flush=True)
        return result.returncode or 1
    print(f"OWNED_NULL_DESCRIPTOR_PASS {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
