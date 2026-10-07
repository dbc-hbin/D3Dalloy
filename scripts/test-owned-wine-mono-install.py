#!/usr/bin/env python3
"""Check fresh Wine Mono installation, its ARM64EC INF helper, and wineboot update."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    runtime, output = args.runtime.resolve(), args.output.resolve()
    root = Path(__file__).resolve().parents[1]
    if output.exists() or not output.is_relative_to(root / "build/local"):
        parser.error("output must be fresh below build/local")
    output.mkdir(parents=True)
    prefix = output / "prefix"
    environment = {k: v for k, v in os.environ.items() if not k.startswith(("WINE", "DYLD_", "FEX"))}
    environment.update(WINEPREFIX=str(prefix), WINEARCH="win64", WINEMSYNC="1",
                       WINEDEBUG="fixme-all,err-unwind,+timestamp,+pid,+loaddll")
    result = {"status": "fail", "runtime": str(runtime), "commands": []}

    def run(name: str, arguments: list[str]) -> str:
        command = [str(runtime / "bin/wine"), *arguments]
        log = output / (name + ".log")
        with log.open("wb") as stream:
            process = subprocess.run(command, env=environment, stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        result["commands"].append({"command": command, "returncode": process.returncode, "log": str(log)})
        text = log.read_text(errors="replace")
        if process.returncode or "Unhandled page fault" in text or "Unhandled exception" in text:
            raise RuntimeError(f"{name} failed; inspect {log}")
        return text

    try:
        result["ntdll_sha256"] = hashlib.sha256((runtime / "lib/wine/aarch64-unix/ntdll.so").read_bytes()).hexdigest()
        run("fresh-install", ["wineboot", "-u"])
        helper = prefix / "drive_c/windows/mono/mono-2.0/support/installinf-x86_64.exe"
        result["helper_sha256"] = hashlib.sha256(helper.read_bytes()).hexdigest()
        registry = run("mono-registration", [r"C:\windows\system32\reg.exe", "query",
                                             r"HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall",
                                             "/s", "/reg:32"])
        if "Wine Mono Windows Support" not in registry:
            raise RuntimeError("Mono support MSI did not register successfully")
        fake_dll = prefix / "drive_c/windows/Microsoft.NET/Framework64/v4.0.30319/fusion.dll"
        if not fake_dll.is_file():
            raise RuntimeError("Mono INF did not install the 64-bit .NET DLLs")
        inf = output / "helper.inf"
        inf.write_text('[Version]\nSignature="$Windows NT$"\n[DefaultInstall]\nAddReg=Check\n'
                       '[Check]\nHKCU,"Software\\DXMT\\InstallInfRegression","Installed",0x00010001,1\n')
        run("inf-helper", [str(helper), "Z:" + str(inf).replace("/", "\\")])
        value = run("inf-result", [r"C:\windows\system32\reg.exe", "query",
                                  r"HKCU\Software\DXMT\InstallInfRegression", "/v", "Installed"])
        if not any(line.split() == ["Installed", "REG_DWORD", "0x1"] for line in value.splitlines()):
            raise RuntimeError("ARM64EC INF helper did not create the expected registry value")
        run("repeat-update", ["wineboot", "-u"])
        result["status"] = "pass"
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
    finally:
        try:
            for option in ("-k", "-w"):
                subprocess.run([str(runtime / "bin/wineserver"), option], env=environment,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=True)
            if result["status"] == "pass":
                shutil.rmtree(prefix)
                result["prefix_cleaned"] = True
        except (OSError, subprocess.SubprocessError) as error:
            result.update(status="fail", cleanup_error=str(error))
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
