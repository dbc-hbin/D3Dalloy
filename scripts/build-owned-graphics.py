#!/usr/bin/env python3
"""Reproduce the frozen import baseline from pinned upstream and patches.

This is provenance reproduction, not a build of current root src/. Use the root
README for current builds and graphics-translator/BUILD.ko.md for frozen inputs.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCK = REPOSITORY_ROOT / "graphics-translator" / "upstream.lock.json"
MANIFEST_NAME = "owned-graphics-build.json"
COMMAND_LOG_NAME = "commands.log"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
RUNTIME_UNIX_DIR = "x86_64-unix"
# The owned DXIL shader compiler is built from the owned patch series into
# airconv and linked into winemetal.so; no external SDK/dylib may appear as a
# dependency of any installed graphics artifact.
NATIVE_MODULE = "winemetal.so"
FORBIDDEN_NATIVE_DEPENDENCY_TOKENS = ("d3dmetal", "libd3dshared", "metalirconverter", "dxilconv")
# Artifacts the removed external-SDK DXIL flow used to install. Nothing in the
# owned build may publish them again, at any path in the install tree.
REMOVED_COMPILER_ARTIFACT_NAMES = (
    "libdxmt-dxilconv.dylib",
    "libmetalirconverter.dylib",
    "metal-shader-converter",
)
MACHO_SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/", "/System/")


class BuildError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stable_json_hash(value: Any) -> str:
    data = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    return sha256_bytes(data)


def load_lock(path: Path) -> dict[str, Any]:
    try:
        lock = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError(f"cannot read lock file {path}: {error}") from error
    if lock.get("schema") != 1:
        raise BuildError(f"unsupported lock schema in {path}: {lock.get('schema')!r}")
    upstream = lock.get("upstream", {})
    if not isinstance(upstream.get("url"), str) or not HEX40.fullmatch(str(upstream.get("commit", ""))):
        raise BuildError("lock upstream url/commit is invalid")
    seen_submodules: set[str] = set()
    for submodule in upstream.get("submodules", []):
        subpath = submodule.get("path")
        if not isinstance(subpath, str) or not subpath or subpath.startswith("/") or ".." in Path(subpath).parts:
            raise BuildError(f"invalid locked submodule path: {subpath!r}")
        if subpath in seen_submodules:
            raise BuildError(f"duplicate locked submodule path: {subpath}")
        seen_submodules.add(subpath)
        if not isinstance(submodule.get("url"), str) or not HEX40.fullmatch(str(submodule.get("commit", ""))):
            raise BuildError(f"invalid lock entry for submodule {subpath}")
    series = lock.get("patches", {}).get("series")
    if not isinstance(series, list) or not series:
        raise BuildError("lock patch series must be a non-empty list")
    seen_patches: set[str] = set()
    for entry in series:
        name = entry.get("file")
        digest = entry.get("sha256")
        if not isinstance(name, str) or Path(name).name != name or not name.endswith(".patch"):
            raise BuildError(f"invalid patch file in lock: {name!r}")
        if name in seen_patches:
            raise BuildError(f"duplicate patch in lock: {name}")
        seen_patches.add(name)
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise BuildError(f"invalid sha256 for patch {name}")
    return lock


def validate_patch_series(lock: dict[str, Any], repository_root: Path) -> list[dict[str, Any]]:
    patch_dir = repository_root / lock["patches"]["directory"]
    if not patch_dir.is_dir():
        raise BuildError(f"patch directory does not exist: {patch_dir}")
    records: list[dict[str, Any]] = []
    expected_names: set[str] = set()
    for index, entry in enumerate(lock["patches"]["series"]):
        path = patch_dir / entry["file"]
        expected_names.add(entry["file"])
        if not path.is_file():
            raise BuildError(f"locked patch is missing: {path}")
        actual = sha256_file(path)
        if actual != entry["sha256"]:
            raise BuildError(f"locked patch hash mismatch: {entry['file']}: expected {entry['sha256']}, got {actual}")
        records.append({
            "index": index + 1,
            "file": entry["file"],
            "path": str(path.resolve()),
            "sha256": actual,
            "size": path.stat().st_size,
        })
    if lock["patches"].get("reject_unlisted", False):
        actual_names = {path.name for path in patch_dir.glob("*.patch") if path.is_file()}
        if actual_names != expected_names:
            extra = sorted(actual_names - expected_names)
            missing = sorted(expected_names - actual_names)
            details = []
            if extra:
                details.append("unlisted=" + ",".join(extra))
            if missing:
                details.append("missing=" + ",".join(missing))
            raise BuildError("patch directory does not match locked series: " + " ".join(details))
    return records


def capture(command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, env=env, check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise BuildError(f"command failed ({result.returncode}): {shlex.join(command)}\n{result.stdout.rstrip()}")
    return result.stdout.strip()


def git_capture(repo: Path, *args: str) -> str:
    return capture(["git", "-C", str(repo), *args])


def ensure_clean_git_ref(path: Path, expected_commit: str | None = None) -> dict[str, str]:
    if not path.is_dir() or not (path / ".git").exists():
        raise BuildError(f"local source ref is not a git checkout: {path}")
    status = git_capture(path, "status", "--porcelain=v1", "--untracked-files=all")
    if status:
        raise BuildError(f"local source ref is dirty: {path}")
    head = git_capture(path, "rev-parse", "HEAD")
    if expected_commit:
        result = subprocess.run(["git", "-C", str(path), "cat-file", "-e", f"{expected_commit}^{{commit}}"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode:
            raise BuildError(f"local source ref does not contain locked commit {expected_commit}: {path}")
    return {"path": str(path.resolve()), "head": head}


def verify_expected_files(root: Path, expected: dict[str, str], label: str) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for relative, wanted in sorted(expected.items()):
        path = root / relative
        if not path.is_file():
            raise BuildError(f"missing {label} file: {path}")
        actual = sha256_file(path)
        if actual != wanted:
            raise BuildError(f"{label} hash mismatch for {relative}: expected {wanted}, got {actual}")
        records.append({"path": relative, "sha256": actual, "size": path.stat().st_size})
    return records


def executable_record(path: Path, version_args: list[str]) -> dict[str, Any]:
    if not path.is_file() or not os.access(path, os.X_OK):
        raise BuildError(f"missing executable: {path}")
    return {
        "path": str(path.resolve()),
        "sha256": sha256_file(path),
        "size": path.stat().st_size,
        "version": capture([str(path), *version_args]),
    }


def validate_tools(lock: dict[str, Any], meson: Path, ninja: Path, llvm_mingw_bin: Path,
                   native_llvm: Path, wine_sdk: Path) -> dict[str, Any]:
    meson_record = executable_record(meson, ["--version"])
    ninja_record = executable_record(ninja, ["--version"])
    if meson_record["version"] != lock["tools"]["meson"]["version"]:
        raise BuildError(f"Meson version mismatch: expected {lock['tools']['meson']['version']}, got {meson_record['version']}")
    if lock["tools"]["meson"].get("sha256") and meson_record["sha256"] != lock["tools"]["meson"]["sha256"]:
        raise BuildError(f"Meson executable hash mismatch: expected {lock['tools']['meson']['sha256']}, got {meson_record['sha256']}")
    if not ninja_record["version"].startswith(lock["tools"]["ninja"]["version_prefix"]):
        raise BuildError(f"Ninja version mismatch: expected prefix {lock['tools']['ninja']['version_prefix']}, got {ninja_record['version']}")
    if lock["tools"]["ninja"].get("sha256") and ninja_record["sha256"] != lock["tools"]["ninja"]["sha256"]:
        raise BuildError(f"Ninja executable hash mismatch: expected {lock['tools']['ninja']['sha256']}, got {ninja_record['sha256']}")

    mingw_records = verify_expected_files(llvm_mingw_bin, lock["tools"]["llvm_mingw"]["files"], "llvm-mingw")
    compiler = llvm_mingw_bin / "x86_64-w64-mingw32-g++"
    compiler_version = capture([str(compiler), "--version"])
    for needle in (lock["tools"]["llvm_mingw"]["compiler_version_contains"],
                   lock["tools"]["llvm_mingw"]["target_contains"]):
        if needle not in compiler_version:
            raise BuildError(f"llvm-mingw compiler identity missing {needle!r}: {compiler_version}")

    llvm_records = verify_expected_files(native_llvm, lock["tools"]["native_llvm"]["files"], "native LLVM")
    config = native_llvm / "include/llvm/Config/llvm-config.h"
    config_text = config.read_text(errors="replace")
    version = lock["tools"]["native_llvm"]["version"]
    if f'#define LLVM_VERSION_STRING "{version}"' not in config_text:
        raise BuildError(f"native LLVM prefix is not locked LLVM {version}: {native_llvm}")

    wine_records = verify_expected_files(wine_sdk, lock["tools"]["wine_sdk"]["files"], "Wine SDK")
    native_compilers: dict[str, Any] = {}
    for name, value in sorted(lock["build"]["native_build_compilers"].items()):
        native_compilers[name] = executable_record(Path(value), ["--version"])

    return {
        "meson": meson_record,
        "ninja": ninja_record,
        "llvm_mingw": {"bin": str(llvm_mingw_bin.resolve()), "compiler_version": compiler_version,
                       "files": mingw_records},
        "native_llvm": {"prefix": str(native_llvm.resolve()), "version": version, "files": llvm_records},
        "wine_sdk": {"prefix": str(wine_sdk.resolve()), "files": wine_records},
        "native_build_compilers": native_compilers,
    }


def macho_tool(executable: str, *args: str, label: str) -> str:
    resolved = shutil.which(executable)
    if not resolved:
        raise BuildError(f"cannot inspect {label}: {executable} is not available")
    result = subprocess.run([resolved, *args], check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise BuildError(f"cannot inspect {label}: {executable} failed: {result.stdout.strip()}")
    return result.stdout


def macho_dependencies(path: Path) -> list[str]:
    lines = macho_tool("otool", "-L", str(path), label=str(path)).splitlines()[1:]
    dependencies: list[str] = []
    for line in lines:
        stripped = line.strip()
        # Fat binaries interleave "(architecture <arch>):" header lines and repeat
        # each slice's dependencies; collapse them into one ordered list.
        if not stripped or stripped.endswith("):"):
            continue
        dependency = stripped.split(" (compatibility version", 1)[0]
        if dependency not in dependencies:
            dependencies.append(dependency)
    return dependencies


def macho_rpaths(path: Path) -> list[str]:
    rpaths: list[str] = []
    pending = False
    for line in macho_tool("otool", "-l", str(path), label=str(path)).splitlines():
        stripped = line.strip()
        if stripped.startswith("cmd LC_RPATH"):
            pending = True
        elif pending and stripped.startswith("path "):
            rpaths.append(stripped[len("path "):].split(" (offset", 1)[0].strip())
            pending = False
        elif stripped.startswith("cmd "):
            pending = False
    return rpaths


def macho_architectures(path: Path) -> list[str]:
    return macho_tool("lipo", "-archs", str(path), label=str(path)).split()


def macho_dependency_problems(path: Path, label: str) -> list[str]:
    """Reject build-tree absolute dependencies; an @rpath/system dep is relocatable."""
    problems: list[str] = []
    for dependency in macho_dependencies(path):
        if dependency.startswith("@") or dependency.startswith(MACHO_SYSTEM_PREFIXES):
            continue
        problems.append(f"{label} has non-relocatable dependency: {dependency}")
    return problems


def macho_relocation_problems(path: Path, label: str) -> list[str]:
    """Reject build-tree absolute paths that would only resolve on this machine."""
    problems = macho_dependency_problems(path, label)
    for rpath in macho_rpaths(path):
        if rpath.startswith(("@loader_path", "@executable_path")):
            continue
        problems.append(f"{label} has non-relocatable LC_RPATH: {rpath}")
    return problems


def absolute_macho_rpaths(path: Path) -> list[str]:
    return [rpath for rpath in macho_rpaths(path)
            if not rpath.startswith(("@loader_path", "@executable_path"))]


def foreign_dependency_problems(path: Path, label: str) -> list[str]:
    """Reject any dependency that would pull an external DXIL/SDK/GPTK backend in."""
    problems: list[str] = []
    for dependency in macho_dependencies(path):
        lowered = dependency.lower()
        for token in FORBIDDEN_NATIVE_DEPENDENCY_TOKENS:
            if token in lowered:
                problems.append(f"{label} depends on an external compiler/SDK library: {dependency}")
                break
    return problems


def sanitize_runtime_modules(install: Path, env: dict[str, str],
                             commands: list[dict[str, Any]], log_path: Path) -> None:
    """Drop build-tree LC_RPATH entries so a copied runtime still resolves its native modules."""
    tool = shutil.which("install_name_tool")
    if not tool:
        raise BuildError("install_name_tool is required to make installed runtime modules relocatable")
    path = install / RUNTIME_UNIX_DIR / NATIVE_MODULE
    if path.is_symlink() or not path.is_file():
        return
    for rpath in absolute_macho_rpaths(path):
        run_logged([tool, "-delete_rpath", rpath, str(path)], step=f"strip-rpath-{path.name}",
                   cwd=install.parent, env=env, records=commands, log_path=log_path)
    remaining = absolute_macho_rpaths(path)
    if remaining:
        raise BuildError(f"{path.name} still has non-relocatable LC_RPATH: {remaining}")


def verify_installed_runtime_modules(install: Path) -> dict[str, Any]:
    """Check the installed native module is relocatable and self-contained.

    The owned DXIL compiler is compiled from the owned patch series into airconv
    and linked into this module, so the module must resolve nothing but Wine's own
    native modules and the system: any external compiler/SDK dependency means a
    leftover SDK path is still being built.
    """
    winemetal = install / RUNTIME_UNIX_DIR / NATIVE_MODULE
    if winemetal.is_symlink() or not winemetal.is_file():
        raise BuildError(f"installed {NATIVE_MODULE} is missing: {winemetal}")
    label = f"installed {NATIVE_MODULE}"
    problems = macho_relocation_problems(winemetal, label)
    problems.extend(foreign_dependency_problems(winemetal, label))
    architectures = macho_architectures(winemetal)
    if "x86_64" not in architectures:
        problems.append(f"{label} has no x86_64 slice: {architectures}")
    if problems:
        raise BuildError("; ".join(problems))
    dependencies = macho_dependencies(winemetal)
    return {
        "native_module": {
            "path": f"{RUNTIME_UNIX_DIR}/{NATIVE_MODULE}",
            "sha256": sha256_file(winemetal),
            "architectures": architectures,
            "dependencies": dependencies,
            "rpaths": macho_rpaths(winemetal),
            "external_compiler_dependencies": [],
        },
    }


def native_compiler_record(install: Path, lock: dict[str, Any], patches: list[dict[str, Any]],
                           tools: dict[str, Any], source_record: dict[str, Any]) -> dict[str, Any]:
    """Describe the self-owned DXIL compiler: owned source provenance and LLVM pins only.

    There is no SDK/compiler prefix input and no separately licensed binary in this
    build, so the record carries the pinned upstream commit, the applied patch
    SHA-256s, the staged source fingerprint, and the native LLVM pins the compiler
    is built against.
    """
    winemetal = install / RUNTIME_UNIX_DIR / NATIVE_MODULE
    return {
        "ownership": "self-owned",
        "integration": f"built from the owned patch series into airconv and linked into {RUNTIME_UNIX_DIR}/{NATIVE_MODULE}",
        "external_dependencies": [],
        "artifact": {"path": f"{RUNTIME_UNIX_DIR}/{NATIVE_MODULE}", "sha256": sha256_file(winemetal)},
        "upstream_commit": lock["upstream"]["commit"],
        "patch_series": [{"file": item["file"], "sha256": item["sha256"]} for item in patches],
        "staged_diff_sha256": source_record["staged_diff_sha256"],
        "native_llvm": {"version": tools["native_llvm"]["version"],
                        "prefix": tools["native_llvm"]["prefix"],
                        "files": [entry["path"] for entry in tools["native_llvm"]["files"]]},
    }


def prepare_environment(lock: dict[str, Any], llvm_mingw_bin: Path, meson: Path, ninja: Path) -> dict[str, str]:
    env = dict(os.environ)
    prefix = [str(llvm_mingw_bin.resolve()), str(ninja.resolve().parent), str(meson.resolve().parent)]
    env["PATH"] = os.pathsep.join(prefix + [env.get("PATH", "")])
    env.update({name: value for name, value in lock["build"]["native_build_compilers"].items()})
    return env


def gitmodules(source: Path) -> dict[str, dict[str, str]]:
    path = source / ".gitmodules"
    if not path.is_file():
        return {}
    names_output = capture(["git", "config", "-f", str(path), "--get-regexp", r"^submodule\..*\.path$"])
    result: dict[str, dict[str, str]] = {}
    for line in names_output.splitlines():
        key, subpath = line.split(None, 1)
        name = key[len("submodule."):-len(".path")]
        url = capture(["git", "config", "-f", str(path), "--get", f"submodule.{name}.url"])
        result[subpath] = {"name": name, "url": url}
    return result


def verify_submodules(source: Path, lock: dict[str, Any]) -> list[dict[str, Any]]:
    locked = {item["path"]: item for item in lock["upstream"].get("submodules", [])}
    declared = gitmodules(source)
    if set(declared) != set(locked):
        raise BuildError(f".gitmodules paths differ from lock: declared={sorted(declared)}, locked={sorted(locked)}")
    records = []
    for subpath in sorted(locked):
        entry = locked[subpath]
        if declared[subpath]["url"] != entry["url"]:
            raise BuildError(f"submodule URL drift for {subpath}: {declared[subpath]['url']} != {entry['url']}")
        checkout = source / subpath
        if not checkout.is_dir():
            raise BuildError(f"submodule checkout missing: {checkout}")
        head = git_capture(checkout, "rev-parse", "HEAD")
        if head != entry["commit"]:
            raise BuildError(f"submodule commit mismatch for {subpath}: expected {entry['commit']}, got {head}")
        status = git_capture(checkout, "status", "--porcelain=v1", "--untracked-files=all")
        if status:
            raise BuildError(f"submodule is dirty: {subpath}")
        records.append({"path": subpath, "url": entry["url"], "commit": head})
    return records


def command_record(records: list[dict[str, Any]], step: str, command: list[str], cwd: Path) -> dict[str, Any]:
    record = {"step": step, "cwd": str(cwd.resolve()), "argv": [str(value) for value in command]}
    records.append(record)
    return record


def run_logged(command: list[str], *, step: str, cwd: Path, env: dict[str, str],
               records: list[dict[str, Any]], log_path: Path) -> None:
    record = command_record(records, step, command, cwd)
    started = time.monotonic()
    print("+", shlex.join(command), flush=True)
    with log_path.open("a", encoding="utf-8") as log:
        log.write(f"\n[{step}] cwd={cwd}\n$ {shlex.join(command)}\n")
        log.flush()
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, bufsize=1)
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(line)
            log.write(line)
        process.stdout.close()
        returncode = process.wait()
    record["exit_code"] = returncode
    record["duration_seconds"] = round(time.monotonic() - started, 3)
    if returncode:
        raise BuildError(f"{step} failed with exit code {returncode}; see {log_path}")


def clone_args(source_ref: Path | None, url: str, destination: Path) -> list[str]:
    if source_ref:
        return ["git", "clone", "--no-hardlinks", "--no-checkout", str(source_ref.resolve()), str(destination)]
    return ["git", "clone", "--filter=blob:none", "--no-checkout", url, str(destination)]


def local_submodule_overrides(source_ref: Path | None, source: Path, lock: dict[str, Any]) -> list[str]:
    if not source_ref:
        return []
    declared = gitmodules(source)
    result: list[str] = ["-c", "protocol.file.allow=always"]
    for entry in lock["upstream"].get("submodules", []):
        local = source_ref / entry["path"]
        if not (local / ".git").exists() and not (local / "HEAD").exists():
            continue
        ensure_clean_git_ref(local, entry["commit"])
        if git_capture(local, "rev-parse", "HEAD") != entry["commit"]:
            continue
        name = declared[entry["path"]]["name"]
        result.extend(["-c", f"submodule.{name}.url={local.resolve()}"])
    return result


def source_patch_fingerprint(source: Path) -> str:
    data = subprocess.run(["git", "-C", str(source), "diff", "--cached", "--binary"],
                          check=True, stdout=subprocess.PIPE).stdout
    return sha256_bytes(data)


def verify_patched_source(source: Path, lock: dict[str, Any], expected_diff: str | None = None) -> dict[str, Any]:
    head = git_capture(source, "rev-parse", "HEAD")
    if head != lock["upstream"]["commit"]:
        raise BuildError(f"source HEAD drift: expected {lock['upstream']['commit']}, got {head}")
    if subprocess.run(["git", "-C", str(source), "diff", "--quiet"], check=False).returncode:
        raise BuildError("source has unstaged modifications")
    untracked = git_capture(source, "ls-files", "--others", "--exclude-standard")
    if untracked:
        raise BuildError(f"source has untracked files: {untracked.splitlines()[0]}")
    submodules = verify_submodules(source, lock)
    diff_hash = source_patch_fingerprint(source)
    if expected_diff and diff_hash != expected_diff:
        raise BuildError(f"staged source patch fingerprint changed: expected {expected_diff}, got {diff_hash}")
    names = git_capture(source, "diff", "--cached", "--name-status").splitlines()
    return {"commit": head, "submodules": submodules, "staged_diff_sha256": diff_hash,
            "staged_name_status": names}


def prepare_source(source: Path, lock: dict[str, Any], patches: list[dict[str, Any]],
                   source_ref: Path | None, env: dict[str, str], commands: list[dict[str, Any]],
                   log_path: Path) -> dict[str, Any]:
    acquisition: dict[str, Any]
    if source_ref:
        ref = ensure_clean_git_ref(source_ref, lock["upstream"]["commit"])
        acquisition = {"mode": "local-ref", **ref}
    else:
        acquisition = {"mode": "remote", "url": lock["upstream"]["url"]}
    run_logged(clone_args(source_ref, lock["upstream"]["url"], source), step="clone-upstream",
               cwd=source.parent, env=env, records=commands, log_path=log_path)
    run_logged(["git", "checkout", "--detach", lock["upstream"]["commit"]], step="checkout-upstream",
               cwd=source, env=env, records=commands, log_path=log_path)
    declared = gitmodules(source)
    locked_paths = {item["path"] for item in lock["upstream"].get("submodules", [])}
    if set(declared) != locked_paths:
        raise BuildError(f".gitmodules paths differ from lock: declared={sorted(declared)}, locked={sorted(locked_paths)}")
    overrides = local_submodule_overrides(source_ref, source, lock)
    submodule_command = ["git", *overrides, "submodule", "update", "--init", "--recursive"]
    run_logged(submodule_command, step="checkout-submodules", cwd=source, env=env,
               records=commands, log_path=log_path)
    verify_submodules(source, lock)
    for patch in patches:
        run_logged(["git", "apply", "--index", "--whitespace=error-all", patch["path"]],
                   step=f"apply-{patch['file']}", cwd=source, env=env, records=commands, log_path=log_path)
    run_logged(["git", "diff", "--cached", "--check"], step="check-patched-source", cwd=source,
               env=env, records=commands, log_path=log_path)
    record = verify_patched_source(source, lock)
    record["acquisition"] = acquisition
    return record


def make_meson_commands(lock: dict[str, Any], meson: Path, source: Path, build: Path, install: Path,
                        native_llvm: Path, wine_sdk: Path, jobs: int) -> list[tuple[str, list[str]]]:
    setup = [str(meson.resolve()), "setup", str(build.resolve()), str(source.resolve()),
             "--cross-file", str((source / lock["build"]["cross_file"]).resolve()),
             "--prefix", str(install.resolve()), "--buildtype", lock["build"]["buildtype"],
             f"-Dnative_llvm_path={native_llvm.resolve()}", f"-Dwine_install_path={wine_sdk.resolve()}"]
    for name, value in sorted(lock["build"]["meson_options"].items()):
        rendered = "true" if value is True else "false" if value is False else str(value)
        setup.append(f"-D{name}={rendered}")
    return [
        ("meson-setup", setup),
        ("meson-compile", [str(meson.resolve()), "compile", "-C", str(build.resolve()), "--jobs", str(jobs)]),
        ("meson-install", [str(meson.resolve()), "install", "-C", str(build.resolve()), "--no-rebuild"]),
    ]


def installed_artifacts(install: Path, lock: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    if not install.is_dir():
        raise BuildError(f"install directory was not produced: {install}")
    records: list[dict[str, Any]] = []
    for path in sorted(install.rglob("*"), key=lambda item: item.as_posix()):
        relative = path.relative_to(install).as_posix()
        if path.is_symlink():
            records.append({"path": relative, "type": "symlink", "target": os.readlink(path)})
        elif path.is_file():
            records.append({"path": relative, "type": "file", "size": path.stat().st_size,
                            "sha256": sha256_file(path)})
    required = {"x86_64-windows/d3d12.dll", "x86_64-windows/dxgi.dll",
                f"{RUNTIME_UNIX_DIR}/{NATIVE_MODULE}"}
    if lock:
        if lock["build"]["meson_options"].get("enable_nvapi"):
            required.add("x86_64-windows/nvapi64.dll")
        if lock["build"]["meson_options"].get("enable_nvngx"):
            required.add("x86_64-windows/nvngx.dll")
    present = {entry["path"] for entry in records}
    missing = sorted(required - present)
    if missing:
        raise BuildError("expected installed graphics artifacts are missing: " + ", ".join(missing))
    leftovers = sorted(entry["path"] for entry in records
                       if set(Path(entry["path"]).parts) & set(REMOVED_COMPILER_ARTIFACT_NAMES))
    if leftovers:
        raise BuildError("install tree still contains artifacts of the removed external DXIL compiler: "
                         + ", ".join(leftovers))
    return records


def write_manifest(path: Path, manifest: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def ensure_output_root(root: Path, resume: bool) -> None:
    if resume:
        if not root.is_dir():
            raise BuildError(f"--resume requires an existing build root: {root}")
        if not (root / MANIFEST_NAME).is_file():
            raise BuildError(f"--resume requires {MANIFEST_NAME}: {root}")
    else:
        if root.exists():
            raise BuildError(f"refusing to overwrite existing build root: {root}")


def validate_resume_contract(manifest: dict[str, Any], contract: dict[str, Any], fingerprint: str) -> None:
    if manifest.get("input_fingerprint") != fingerprint or manifest.get("input") != contract:
        raise BuildError("--resume refused: current source/config/patch/tool contract differs from manifest")
    if not manifest.get("source", {}).get("staged_diff_sha256"):
        raise BuildError("--resume refused: manifest has no staged source fingerprint")


def input_contract(root: Path, lock_path: Path, lock: dict[str, Any], patches: list[dict[str, Any]],
                   tools: dict[str, Any], jobs: int, bootstrap_llvm: bool) -> dict[str, Any]:
    return {
        "output_root": str(root.resolve()),
        "lock": {"path": str(lock_path.resolve()), "sha256": sha256_file(lock_path)},
        "upstream": lock["upstream"],
        "patches": [{key: item[key] for key in ("index", "file", "sha256", "size")} for item in patches],
        "build": lock["build"],
        "tools": tools,
        "jobs": jobs,
        "native_llvm_mode": "bootstrap" if bootstrap_llvm else "provided",
    }


def clone_pinned_repo(url: str, commit: str, destination: Path, source_ref: Path | None,
                      env: dict[str, str], commands: list[dict[str, Any]], log_path: Path,
                      step_prefix: str) -> None:
    if source_ref:
        ensure_clean_git_ref(source_ref, commit)
    run_logged(clone_args(source_ref, url, destination), step=f"{step_prefix}-clone", cwd=destination.parent,
               env=env, records=commands, log_path=log_path)
    run_logged(["git", "checkout", "--detach", commit], step=f"{step_prefix}-checkout", cwd=destination,
               env=env, records=commands, log_path=log_path)
    if git_capture(destination, "rev-parse", "HEAD") != commit:
        raise BuildError(f"{step_prefix} checkout did not reach pinned commit {commit}")


def bootstrap_native_llvm(root: Path, lock: dict[str, Any], ninja: Path, jobs: int,
                          env: dict[str, str], commands: list[dict[str, Any]], log_path: Path,
                          source_ref: Path | None) -> Path:
    spec = lock["tools"]["native_llvm"]
    toolchain = root / "toolchain"
    toolchain.mkdir()
    source = toolchain / "llvm-project"
    build = toolchain / "llvm-build"
    install = toolchain / "llvm"
    clone_pinned_repo(spec["source_url"], spec["source_commit"], source, source_ref, env, commands,
                      log_path, "llvm")
    cmake = shutil.which("cmake", path=env.get("PATH"))
    if not cmake:
        raise BuildError("--bootstrap-llvm requires cmake in PATH")
    configure = [cmake, "-S", str(source / "llvm"), "-B", str(build), "-G", "Ninja",
                 f"-DCMAKE_MAKE_PROGRAM={ninja.resolve()}", f"-DCMAKE_INSTALL_PREFIX={install}",
                 "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
                 *spec["bootstrap"]["cmake_options"]]
    run_logged(configure, step="llvm-cmake", cwd=root, env=env, records=commands, log_path=log_path)
    run_logged([cmake, "--build", str(build), "--target", "install", "--parallel", str(jobs)],
               step="llvm-build-install", cwd=root, env=env, records=commands, log_path=log_path)
    verify_expected_files(install, spec["files"], "bootstrapped native LLVM")
    return install


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="fresh build root; contains source/build/install/manifest")
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--source-ref", type=Path, help="optional clean local DXMT checkout used only as clone source")
    parser.add_argument("--native-llvm", type=Path, help="pinned native LLVM 15 prefix")
    parser.add_argument("--bootstrap-llvm", action="store_true", help="explicitly build pinned LLVM 15 into this build root")
    parser.add_argument("--llvm-source-ref", type=Path, help="optional clean local llvm-project checkout for --bootstrap-llvm")
    parser.add_argument("--wine-sdk", type=Path, required=True, help="installed Wine SDK prefix used only for build/link inputs")
    parser.add_argument("--llvm-mingw-bin", type=Path, required=True)
    parser.add_argument("--meson", type=Path, required=True)
    parser.add_argument("--ninja", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=max(1, min(os.cpu_count() or 1, 16)))
    parser.add_argument("--resume", action="store_true", help="resume only when manifest/source/config/tool hashes still match")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.jobs < 1:
            raise BuildError("--jobs must be at least 1")
        if args.bootstrap_llvm and args.native_llvm:
            raise BuildError("choose either --native-llvm or --bootstrap-llvm, not both")
        if not args.bootstrap_llvm and not args.native_llvm:
            raise BuildError("--native-llvm is required unless --bootstrap-llvm is explicit")
        if args.llvm_source_ref and not args.bootstrap_llvm:
            raise BuildError("--llvm-source-ref requires --bootstrap-llvm")

        root = args.root.resolve()
        lock_path = args.lock.resolve()
        lock = load_lock(lock_path)
        patches = validate_patch_series(lock, REPOSITORY_ROOT)
        ensure_output_root(root, args.resume)
        if args.source_ref:
            ensure_clean_git_ref(args.source_ref.resolve(), lock["upstream"]["commit"])

        env = prepare_environment(lock, args.llvm_mingw_bin.resolve(), args.meson.resolve(), args.ninja.resolve())
        commands: list[dict[str, Any]] = []
        log_path = root / COMMAND_LOG_NAME
        tools: dict[str, Any] | None = None
        source = root / "source"
        build = root / "build"
        install = root / "install"

        if args.resume:
            prior = json.loads((root / MANIFEST_NAME).read_text())
            native_llvm = Path(prior["input"]["tools"]["native_llvm"]["prefix"])
            if args.bootstrap_llvm and native_llvm != root / "toolchain/llvm":
                raise BuildError("resume manifest native LLVM prefix does not match --bootstrap-llvm layout")
            if not args.bootstrap_llvm and native_llvm.resolve() != args.native_llvm.resolve():
                raise BuildError("resume native LLVM prefix differs from manifest")
            tools = validate_tools(lock, args.meson.resolve(), args.ninja.resolve(), args.llvm_mingw_bin.resolve(),
                                   native_llvm, args.wine_sdk.resolve())
            contract = input_contract(root, lock_path, lock, patches, tools, args.jobs, args.bootstrap_llvm)
            fingerprint = stable_json_hash(contract)
            manifest = json.loads((root / MANIFEST_NAME).read_text())
            validate_resume_contract(manifest, contract, fingerprint)
            expected_diff = manifest.get("source", {}).get("staged_diff_sha256")
            source_record = verify_patched_source(source, lock, expected_diff)
            source_record["acquisition"] = manifest.get("source", {}).get("acquisition", {"mode": "unknown"})
            commands = list(manifest.get("commands", []))
        else:
            # Validate everything that already exists before creating the output
            # root so a bad provided toolchain cannot leave an unusable root.
            if not args.bootstrap_llvm:
                native_llvm = args.native_llvm.resolve()
                tools = validate_tools(lock, args.meson.resolve(), args.ninja.resolve(),
                                       args.llvm_mingw_bin.resolve(), native_llvm, args.wine_sdk.resolve())
            root.parent.mkdir(parents=True, exist_ok=True)
            root.mkdir()
            log_path.write_text("")
            manifest = {
                "schema": 1,
                "status": "preparing-source",
                "created_utc": datetime.now(timezone.utc).isoformat(),
                "source": {},
                "layout": {"source": str(source), "build": str(build), "install": str(install)},
                "commands": commands,
                "artifacts": [],
            }
            write_manifest(root / MANIFEST_NAME, manifest)
            try:
                if args.bootstrap_llvm:
                    native_llvm = bootstrap_native_llvm(root, lock, args.ninja.resolve(), args.jobs, env, commands,
                                                        log_path, args.llvm_source_ref.resolve() if args.llvm_source_ref else None)
                source_record = prepare_source(
                    source, lock, patches, args.source_ref.resolve() if args.source_ref else None,
                    env, commands, log_path)
            except Exception as error:
                manifest["commands"] = commands
                manifest["status"] = "failed"
                manifest["error"] = str(error)
                if log_path.is_file():
                    manifest["command_log"] = {"path": COMMAND_LOG_NAME, "size": log_path.stat().st_size,
                                               "sha256": sha256_file(log_path)}
                write_manifest(root / MANIFEST_NAME, manifest)
                raise
            manifest["source"] = source_record
            manifest["commands"] = commands
            manifest["status"] = "prepared"
            if tools is None:
                tools = validate_tools(lock, args.meson.resolve(), args.ninja.resolve(),
                                       args.llvm_mingw_bin.resolve(), native_llvm, args.wine_sdk.resolve())
            contract = input_contract(root, lock_path, lock, patches, tools, args.jobs, args.bootstrap_llvm)
            fingerprint = stable_json_hash(contract)
            manifest["input"] = contract
            manifest["input_fingerprint"] = fingerprint
            write_manifest(root / MANIFEST_NAME, manifest)

        meson_steps = make_meson_commands(lock, args.meson.resolve(), source, build, install, native_llvm,
                                          args.wine_sdk.resolve(), args.jobs)
        setup_needed = not (build / "meson-private/coredata.dat").is_file()
        try:
            for step, command in meson_steps:
                if step == "meson-setup" and not setup_needed:
                    continue
                run_logged(command, step=step, cwd=root, env=env, records=commands, log_path=log_path)
                manifest["commands"] = commands
                manifest["status"] = "configured" if step == "meson-setup" else "compiled" if step == "meson-compile" else "installed"
                write_manifest(root / MANIFEST_NAME, manifest)
            sanitize_runtime_modules(install, env, commands, log_path)
            artifacts = installed_artifacts(install, lock)
            manifest["artifacts"] = artifacts
            manifest["artifact_set_sha256"] = stable_json_hash(artifacts)
            manifest["runtime_modules"] = verify_installed_runtime_modules(install)
            final_source = verify_patched_source(source, lock, source_record["staged_diff_sha256"])
            final_source["acquisition"] = source_record.get("acquisition", {"mode": "unknown"})
            manifest["source"] = final_source
            manifest["native_dxil_compiler"] = native_compiler_record(install, lock, patches, tools, final_source)
            manifest["commands"] = commands
            manifest["command_log"] = {"path": COMMAND_LOG_NAME, "size": log_path.stat().st_size,
                                       "sha256": sha256_file(log_path)}
            manifest["status"] = "complete"
            manifest["completed_utc"] = datetime.now(timezone.utc).isoformat()
            write_manifest(root / MANIFEST_NAME, manifest)
        except Exception as error:
            manifest["commands"] = commands
            manifest["status"] = "failed"
            manifest["error"] = str(error)
            if log_path.is_file():
                manifest["command_log"] = {"path": COMMAND_LOG_NAME, "size": log_path.stat().st_size,
                                           "sha256": sha256_file(log_path)}
            write_manifest(root / MANIFEST_NAME, manifest)
            raise

        print(f"owned graphics build complete: {root}")
        print(f"manifest: {root / MANIFEST_NAME}")
        return 0
    except (BuildError, OSError, KeyError, ValueError, json.JSONDecodeError) as error:
        print(f"build-owned-graphics: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
