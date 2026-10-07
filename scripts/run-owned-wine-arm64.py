#!/usr/bin/env python3
"""Execute ARM64 or FEX x64 ABI, shared-data, or D3D12 fixtures in a fresh native Wine prefix."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from owned_graphics_arch import inspect_pe

ROOT = Path(__file__).resolve().parents[1]
STAGE_SPEC = importlib.util.spec_from_file_location("owned_runtime_stage", Path(__file__).with_name("stage-owned-graphics.py"))
assert STAGE_SPEC and STAGE_SPEC.loader
stage = importlib.util.module_from_spec(STAGE_SPEC)
STAGE_SPEC.loader.exec_module(stage)
FEX_PROBES = {"thread-termination": "thread_termination", "pcmp": "pcmp_checks", "disk-cache": "cache_checks",
              "metal4fx-pointer": "metal4fx_pointers"}


def fex_probe_evidence(text: str, key: str) -> dict:
    entries = [json.loads(line) for line in text.splitlines() if line.startswith('{"guest_entry":')]
    checks = [json.loads(line) for line in text.splitlines() if line.startswith('{"' + key + '":')]
    entered = len(entries) == 1 and entries[0].get("guest_entry") is True
    passed = (entered and len(checks) == 1 and checks[0].get(key) == "PASS"
              and type(checks[0].get("cases")) is int and checks[0]["cases"] > 0)
    return {"entered": entered, "checks": checks, "passed": passed}


def smoke_evidence(text: str) -> dict:
    frames = [int(value) for value in re.findall(r"^OWNED_D3D12_SMOKE_FRAME frame=(\d+) readback=ok present=skipped$", text, re.M)]
    passed = (frames == list(range(8)) and text.splitlines().count("OWNED_D3D12_SMOKE_PASS") == 1
              and "OWNED_D3D12_SMOKE_FAIL" not in text and "OWNED_D3D12_SMOKE_MISMATCH" not in text)
    return {"frames": frames, "exact_readback": passed}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--suite", choices=("abi", "shared-data", "smoke", "native-jit", "wow64-window", *FEX_PROBES), default="abi")
    parser.add_argument("--guest-arch", choices=("arm64", "x64", "x86"), default="arm64")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--wine-debug", default="", help="additional channels; loaded-image and exception tracing stay enabled")
    parser.add_argument("--gpu", action="store_true", help="require fixture GPU submission/fence/exact readback")
    parser.add_argument("--msync", action="store_true", help="verify with Wine Mach synchronization enabled")
    parser.add_argument("--disk-cache", action="store_true", help="run FEX pcmp/cache probe cold and warm with bounded disk/memory caches")
    args = parser.parse_args()
    runtime, fixture, output = (path.resolve() for path in (args.runtime, args.fixture, args.output))
    if output.exists() or not output.is_relative_to(ROOT / "build/local"):
        parser.error("output must be fresh below build/local")
    if not runtime.is_relative_to(ROOT / "build/local"):
        parser.error("runtime must be isolated below build/local")
    if args.suite == "shared-data" and (args.guest_arch != "x64" or args.gpu):
        parser.error("shared-data requires x64 and does not submit GPU work")
    if args.suite in ("native-jit", "wow64-window") and (args.gpu or args.guest_arch !=
            ("arm64" if args.suite == "native-jit" else "x86")):
        parser.error("native-jit requires ARM64; wow64-window requires x86; neither submits GPU work")
    if args.suite == "smoke":
        args.gpu = True
    if args.suite in FEX_PROBES and (args.guest_arch not in ("x64", "x86") or args.gpu):
        parser.error("FEX probes require x64 or x86 without the GPU-readback option")
    if args.disk_cache and args.suite not in ("pcmp", "disk-cache"):
        parser.error("disk-cache mode requires the pcmp or disk-cache suite")
    executable = {"abi": "abi-guest.exe", "shared-data": "fex_shared_data_probe.exe", "smoke": "owned-d3d12-smoke.exe",
                  "native-jit": "native_jit_probe.exe", "wow64-window": "wow64_guest_window.exe",
                  "thread-termination": "fex_thread_termination.exe", "pcmp": "fex_pcmp_probe.exe",
                  "disk-cache": "fex_disk_cache_lookup.exe", "metal4fx-pointer": "metal4fx-pointer.exe"}[args.suite]
    fixture_modules = (executable, "abi-callee.dll") if args.suite == "abi" else (executable,)
    for name in fixture_modules:
        expected_abi = args.guest_arch if name == executable else ("arm64ec" if args.guest_arch == "x64" else args.guest_arch)
        if inspect_pe(fixture / name)["windows_abi"] != expected_abi:
            parser.error(f"{name} must be {expected_abi}")
    output.mkdir(parents=True)
    images = output / "images"
    images.mkdir()
    prefix = output / "prefix"
    prefix.mkdir()
    observer = output / "observer.dylib"
    subprocess.run(["/usr/bin/clang", "-arch", "arm64", "-dynamiclib", "-O1",
                    str(ROOT / "graphics-translator/tests/arm64_runtime_observer.c"), "-o", str(observer)], check=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("WINE", "DYLD_", "FEX_")) and key not in (*stage.GPTK_ENV, *stage.FOREIGN_GRAPHICS_ENV)}
    loader = runtime / "bin/wine.real" if (runtime / "bin/wine.real").exists() else runtime / "bin/wine"
    environment |= {"WINEPREFIX": str(prefix), "WINEARCH": "win64",
                    "WINEDEBUG": (args.wine_debug + "," if args.wine_debug else "") + "+loaddll,+seh",
                    "WINESERVER": str(runtime / "bin/wineserver"), "WINELOADER": str(loader),
                    "WINEDLLPATH": str(runtime / "lib/wine/aarch64-windows") + ":" + str(runtime / "lib/wine/aarch64-unix"),
                    "WINEDLLOVERRIDES": stage.owned_overrides() + ";mscoree,mshtml=",
                    "DXMT_ARM64_EVIDENCE": str(images),
                    "DYLD_INSERT_LIBRARIES": str(observer), "MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1",
                    "MTL_SHADER_VALIDATION_ENABLE_ERROR_REPORTING": "1", "MTL_SHADER_VALIDATION_REPORT_TO_STDERR": "1"}
    if args.guest_arch == "x86":
        environment["WINEDLLPATH"] += ":" + str(runtime / "lib/wine/i386-windows")
    if args.msync:
        environment["WINEMSYNC"] = "1"
    environment["FEX_DISKCACHE"] = "1" if args.disk_cache else "0"
    cache_root = prefix / "drive_c/fex-cache"
    if args.disk_cache:
        environment |= {"FEX_DISKCACHEPATH": "C:\\fex-cache\\", "FEX_DISKCACHEMEMORYSIZE": "4096",
                        "FEX_DISKCACHEMAXFILESIZE": "65536", "FEX_DISKCACHEPRUNESTALEENTRIES": "1"}
    result = {"schema": 1, "runtime": str(runtime), "fixture": str(fixture), "guest_arch": args.guest_arch,
              "suite": args.suite,
              "emulator": "FEX" if args.guest_arch in ("x64", "x86") else None, "runtime_executed": False, "guest_executed": False, "strict_no_rosetta_proven": False,
              "status": "blocked", "processes": [], "gpu_required": args.gpu,
              "observation_route": "direct_native_loader_with_dyld_observer",
              "environment": {key: value for key, value in environment.items() if key.startswith(("WINE", "DYLD_", "DXMT_", "MTL_", "FEX_"))},
              "loaded_native": [], "loaded_pe": []}
    server = None
    server_log = (output / "server.log").open("w")
    try:
        if (runtime / stage.MANIFEST).exists():
            problems = stage.verify_runtime(runtime)
            if problems:
                raise RuntimeError("invalid staged runtime: " + "; ".join(problems))
            result["manifest_sha256"] = stage.sha256(runtime / stage.MANIFEST)
        if args.guest_arch == "x64" or (args.guest_arch == "x86" and args.suite in ("abi", *FEX_PROBES)):
            manifest = json.loads((runtime / stage.MANIFEST).read_text())
            key = "fex_x64" if args.guest_arch == "x64" else "fex_x86"
            if not manifest.get("optional_modules", {}).get(key):
                raise RuntimeError(f"{args.guest_arch} execution requires the audited staged FEX emulator")
            result["fex"] = manifest["fex" if args.guest_arch == "x64" else "fex_x86"]
        server = subprocess.Popen([str(runtime / "bin/wineserver"), "-f", "-p"], env=environment,
                                  stdout=server_log, stderr=subprocess.STDOUT)
        result["server_pid"] = server.pid
        version = subprocess.check_output(["/usr/bin/sw_vers", "-productVersion"], text=True).strip().split(".")
        version += ["0"] * (3 - len(version))
        parameters = [*version, *(["--gpu"] if args.gpu else [])] if args.suite == "abi" else (["--no-present"] if args.suite == "smoke" else [])
        command = [str(loader), str(fixture / executable), *parameters]
        result["command"] = command
        run_texts = []
        result["runs"] = []
        stale_bucket = None
        for log_name in (("execution.log", "warm.log") if args.disk_cache else ("execution.log",)):
            with (output / log_name).open("w") as stream:
                with subprocess.Popen(command, cwd=fixture, env=environment, stdout=stream, stderr=subprocess.STDOUT) as process:
                    result["caller_pid"] = process.pid
                    try:
                        process.wait(timeout=args.timeout)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        result["timed_out"] = True
            run_text = (output / log_name).read_text(errors="replace")
            run_texts.append(run_text)
            run = {"log": log_name, "pid": process.pid, "exit_code": process.returncode}
            if args.suite in FEX_PROBES:
                run["probe"] = fex_probe_evidence(run_text, FEX_PROBES[args.suite])
            if args.disk_cache:
                run["cache_files"] = [{"path": str(path.relative_to(cache_root)), "size": path.stat().st_size,
                                       "sha256": stage.sha256(path)} for path in sorted(cache_root.rglob("*.foz"))]
            result["runs"].append(run)
            if args.disk_cache and log_name == "execution.log" and cache_root.is_dir():
                stale_bucket = cache_root / ("0" * 16 if not (cache_root / ("0" * 16)).exists() else "1" * 16)
                stale_bucket.mkdir()
                (stale_bucket / "sentinel").write_bytes(b"stale machine cache test\n")
            if process.returncode != 0:
                break
        result["runtime_executed"] = True
        result["exit_code"] = process.returncode
        text = "\n".join(run_texts)
        result["guest_entry"] = next((json.loads(line) for line in text.splitlines() if line.startswith('{"guest_entry":')), None)
        result["abi"] = next((json.loads(line) for line in text.splitlines() if line.startswith('{"abi_checks":')), None)
        result["gpu"] = next((json.loads(line) for line in text.splitlines() if line.startswith('{"gpu_checks":')), None)
        result["memory_codec"] = next((json.loads(line) for line in text.splitlines() if line.startswith('{"memory_codec":')), None)
        result["validation_errors"] = [line for line in text.splitlines()
            if any(term in line for term in ("Validation Error", "Invalid Metal usage:", "failed assertion"))]
        if args.suite == "smoke":
            result["smoke"] = smoke_evidence(text)
        elif args.suite == "shared-data":
            result["shared_data"] = bool(re.search(r"^PASS: fixed-address constant/register scalar 8/16/32/64, SSE128, clock and read-only AV; Windows \d+\.\d+ build \d+$", text, re.M))
        elif args.suite in ("native-jit", "wow64-window"):
            marker = "NATIVE_JIT" if args.suite == "native-jit" else "WOW64_WINDOW"
            result["entry_probe"] = text.splitlines().count(marker + "_ENTER") == 1
            result["address_probe"] = sum(line.startswith(marker + "_PASS:") for line in text.splitlines()) == 1
        for capture in sorted(images.glob("*.images")):
            for line in capture.read_text().splitlines():
                kind, pid, value, path = line.split("\t", 3)
                if kind == "PROCESS":
                    result["processes"].append({"pid": int(pid), "proc_translated": int(value), "executable": path})
                elif kind == "ADDRESS":
                    result.setdefault("image_addresses", []).append({"pid": int(pid), "base": int(value, 16), "path": path})
                else:
                    resolved = Path(path).resolve()
                    row = {"pid": int(pid), "selected_cpu_type": int(value), "path": path,
                           "resolved_path": str(resolved)}
                    if resolved.is_file():
                        row["sha256"] = stage.sha256(resolved)
                    else:
                        row["storage"] = "dyld_shared_cache"
                    result["loaded_native"].append(row)
        pe_paths = [(match.group(1), "wine_loaddll") for match in re.finditer(r'Loaded L"([^"\n]+)"', text)]
        if args.suite == "smoke":
            pe_paths.extend((match.group(1), "guest_GetModuleFileNameW") for match in
                            re.finditer(r"^OWNED_D3D12_SMOKE_MODULE name=\S+ path=([^\r\n]+)", text, re.MULTILINE))
        if args.suite == "abi":
            pe_paths.extend((line.removeprefix("ABI_IMAGE:"), "guest_GetModuleFileNameA")
                            for line in text.splitlines() if line.startswith("ABI_IMAGE:"))
        if args.suite == "wow64-window":
            pe_paths.extend((line.removeprefix("WOW64_WINDOW_IMAGE:"), "guest_GetModuleFileNameA")
                            for line in text.splitlines() if line.startswith("WOW64_WINDOW_IMAGE:"))
        if args.suite in FEX_PROBES:
            pe_paths.extend((line.removeprefix("FEX_PROBE_IMAGE:"), "guest_GetModuleFileNameA")
                            for line in text.splitlines() if line.startswith("FEX_PROBE_IMAGE:"))
        for image_path, route in pe_paths:
            windows = image_path.replace("\\\\", "\\")
            drive, _, relative = windows.partition(":")
            if drive.upper() == "Z":
                path = Path("/" + relative.lstrip("\\").replace("\\", "/"))
            elif drive.upper() == "C":
                path = prefix / "drive_c" / relative.lstrip("\\").replace("\\", "/")
                # PE32 GetModuleFileName reports the logical system32 path.
                # The loader maps the corresponding syswow64 image in this prefix.
                if (args.guest_arch == "x86" and route.startswith("guest_GetModuleFileName") and
                        path.parent == prefix / "drive_c/windows/system32"):
                    path = prefix / "drive_c/windows/syswow64" / path.name
                    route += "_wow64_redirected"
            else:
                continue
            if path.is_file():
                result["loaded_pe"].append({"windows_path": windows, "path": str(path), "observation_route": route,
                    "resolved_path": str(path.resolve()), "sha256": stage.sha256(path), "pe": inspect_pe(path)})
        processes = result["processes"]
        native = result["loaded_native"]
        names = {Path(row["path"]).name for row in native if row["pid"] == result["caller_pid"]}
        server_images = {Path(row["path"]).name for row in native if row["pid"] == result["server_pid"]}
        pe_names = {Path(row["path"]).name.lower() for row in result["loaded_pe"]}
        result["host_no_rosetta_observed"] = bool(native) and (
            {result["caller_pid"], result["server_pid"]}.issubset({row["pid"] for row in processes}) and
            all(row["proc_translated"] == 0 for row in processes) and
            all(row["selected_cpu_type"] == 0x100000c for row in native))
        result["guest_executed"] = bool(result["guest_entry"] and result["guest_entry"]["guest_entry"])
        if args.suite in FEX_PROBES:
            result["guest_executed"] = all(run["probe"]["entered"] for run in result["runs"])
        elif args.suite != "abi":
            result["guest_executed"] = ("FEX_SHARED_DATA_ENTER" in text if args.suite == "shared-data" else
                                        (result["entry_probe"] if args.suite in ("native-jit", "wow64-window") else
                                         any(line.startswith("OWNED_D3D12_SMOKE_") for line in text.splitlines())))
        if result.get("timed_out"):
            raise RuntimeError(f"guest did not exit within {args.timeout} seconds; collected guest and image evidence retained")
        if "failed to map the shared user data: c0000017" in text:
            raise RuntimeError("DARWIN_ARM64_LOW_ADDRESS_UNAVAILABLE: Wine cannot map KUSER_SHARED_DATA at 0x7ffe0000; PE execution was not reached")
        probe_passed = ((len(result["runs"]) == (2 if args.disk_cache else 1) and
                         all(run["probe"]["passed"] and run["exit_code"] == 0 for run in result["runs"]))
                        if args.suite in FEX_PROBES else
                        (bool(result["abi"] and result["abi"]["abi_checks"] == "PASS") if args.suite == "abi" else
                        (result["smoke"]["exact_readback"] if args.suite == "smoke" else
                         (result["address_probe"] if args.suite in ("native-jit", "wow64-window") else result["shared_data"]))))
        if args.disk_cache:
            result["cache_bounds_verified"] = all(
                any(not row["path"].endswith("_idx.foz") and row["size"] > 16 for row in run["cache_files"]) and
                all(row["size"] <= 65536 for row in run["cache_files"] if not row["path"].endswith("_idx.foz"))
                for run in result["runs"])
            result["stale_cache_pruned"] = stale_bucket is not None and not stale_bucket.exists()
            result["current_cache_preserved"] = (len(result["runs"]) == 2 and
                {Path(row["path"]).parts[0] for row in result["runs"][0]["cache_files"]}.issubset(
                {Path(row["path"]).parts[0] for row in result["runs"][1]["cache_files"]}))
            probe_passed &= (result["cache_bounds_verified"] and result["stale_cache_pruned"] and result["current_cache_preserved"])
        required_native = {"wine", "ntdll.so"} | ({"winemetal.so"} if args.suite in ("abi", "smoke", "metal4fx-pointer") else set())
        # Wine may start a new host process for any guest ABI. Require its
        # complete observed native closure, not WineMetal in the initial loader.
        guest_hosts = {row["pid"] for row in native if Path(row["path"]).name == "winemetal.so"}
        result["guest_host_pids"] = sorted(guest_hosts)
        native_guest_closure = (any(required_native.issubset({Path(row["path"]).name for row in native
                                    if row["pid"] == pid and Path(row["path"]).name in required_native and
                                    row.get("sha256") == stage.sha256(runtime / "lib/wine/aarch64-unix" / Path(row["path"]).name)})
                                    for pid in guest_hosts) if args.suite in ("abi", "smoke", "metal4fx-pointer")
                                else required_native.issubset(names))
        required_pe = set(fixture_modules) | ({"winemetal.dll"} if args.suite in ("abi", "smoke", "metal4fx-pointer") else set())
        if args.suite == "smoke":
            required_pe |= {"d3d12.dll", "d3d12core.dll", "dxgi.dll"}
        if args.guest_arch == "x64":
            required_pe.add("xtajit64.dll")
        if args.guest_arch == "x86":
            required_pe |= {"xtajit.dll", "wow64.dll"}
        expected_pe = {name: stage.sha256(fixture / name) for name in fixture_modules}
        expected_pe.update({name: stage.sha256(runtime / ("lib/wine/i386-windows" if
                            args.guest_arch == "x86" and name not in {"xtajit.dll", "wow64.dll"}
                            else "lib/wine/aarch64-windows") / name)
                            for name in required_pe - set(fixture_modules)})
        result["pe_identity_failures"] = [name for name, digest in expected_pe.items()
            if not any(Path(row["path"]).name.lower() == name and row["sha256"] == digest for row in result["loaded_pe"])]
        result["native_identity_failures"] = [name for name in required_native
            if not any(Path(row["path"]).name == name and row.get("sha256") ==
                       stage.sha256(runtime / "lib/wine/aarch64-unix" / name) for row in native)]
        if (process.returncode != 0 or not probe_passed or result["validation_errors"] or
                (args.suite == "metal4fx-pointer" and "Metal API Validation Enabled" not in text) or
                (args.guest_arch == "x86" and args.gpu and args.suite == "abi" and
                 (not result["memory_codec"] or result["memory_codec"]["memory_codec"] != "PASS")) or
                result["pe_identity_failures"] or result["native_identity_failures"] or
                (args.gpu and ((args.suite == "abi" and (not result["gpu"] or result["gpu"]["gpu_checks"] != "PASS")) or
                 "Metal API Validation Enabled" not in text or "Metal GPU Validation Enabled" not in text)) or
                not {result["server_pid"], *(run["pid"] for run in result["runs"])}.issubset({row["pid"] for row in processes}) or
                not processes or any(row["proc_translated"] != 0 for row in processes) or
                any(row["selected_cpu_type"] != 0x100000c for row in native) or
                not native_guest_closure or "wineserver" not in server_images or
                (args.guest_arch == "x64" and "xtajit64.dll" not in pe_names) or
                not required_pe.issubset(pe_names)):
            raise RuntimeError("native ABI execution/child architecture/loaded closure acceptance failed; inspect execution.log and result.json")
        result["strict_no_rosetta_proven"] = True
        result["status"] = "pass"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result["blocker"] = str(error)
    finally:
        try:
            cleanup_environment = environment.copy()
            cleanup_environment.pop("DYLD_INSERT_LIBRARIES")
            subprocess.run([str(runtime / "bin/wineserver"), "-k"], env=cleanup_environment,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
            if server:
                server.wait(timeout=15)
            subprocess.run([str(runtime / "bin/wineserver"), "-w"], env=cleanup_environment,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15, check=True)
            if args.disk_cache and cache_root.exists():
                shutil.copytree(cache_root, output / "cache")
            if prefix.exists():
                shutil.rmtree(prefix)
            result["prefix_cleaned"] = True
        except (OSError, subprocess.SubprocessError) as error:
            result["cleanup_error"] = str(error)
            result["status"] = "blocked"
        server_log.close()
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key not in ("loaded_native", "loaded_pe")}, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
