#!/usr/bin/env python3
"""Compile real guest ABI probes and audit candidates without launching Wine/FEX.

Static eligibility is NOT execution or no-Rosetta proof. Every invocation records
BLOCKED execution until actual child architecture, loaded native chain and all
fixture computations have been observed by an integration runner.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
from pathlib import Path
from urllib.parse import urlparse

from owned_graphics_arch import architecture, inspect_pe, macho_architectures, runtime_architecture

ROOT = Path(__file__).resolve().parents[1]
STAGE_SPEC = importlib.util.spec_from_file_location('owned_arm64_stage', Path(__file__).with_name('stage-owned-graphics.py'))
assert STAGE_SPEC and STAGE_SPEC.loader
stage = importlib.util.module_from_spec(STAGE_SPEC)
STAGE_SPEC.loader.exec_module(stage)
DEFAULT_TOOLCHAIN = Path('/opt/llvm-mingw-20260616-ucrt-macos-universal/bin')
MATRIX = (
    ('x64', 'arm64ec', 'x86_64', 'arm64ec'),
    ('arm64', 'arm64', 'aarch64', 'aarch64'),
    ('x86', 'x86', 'i686', 'i686'),
)
MACHO_MAGICS = {b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe',
                b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf', b'\xbf\xba\xfe\xca'}


def official_source(url: str) -> bool:
    parsed = urlparse(url)
    if parsed.scheme != 'https' or parsed.username or parsed.password or parsed.port:
        return False
    if parsed.hostname == 'github.com':
        return any(parsed.path.startswith(prefix) for prefix in
                   ('/wine-mirror/wine/', '/FEX-Emu/FEX/', '/llvm/llvm-project/', '/mstorsjo/llvm-mingw/'))
    if parsed.hostname == 'gitlab.winehq.org':
        return parsed.path.startswith('/wine/wine/')
    if parsed.hostname in ('media.codeweavers.com', 'ftp.codeweavers.com'):
        return parsed.path.startswith('/pub/crossover/source/') and parsed.path.endswith('.tar.gz')
    return False


def verified_owned_artifacts(runtime: Path) -> dict:
    manifest = runtime / stage.MANIFEST
    if not manifest.exists():
        return {}
    data = json.loads(manifest.read_text())
    if not isinstance(data, dict):
        raise ValueError('staged owned manifest must be a JSON object')
    try:
        problems = stage.verify_runtime(runtime)
    except (TypeError, KeyError, AttributeError) as error:
        raise ValueError(f'invalid staged owned manifest: {error}') from error
    if problems:
        raise ValueError('STAGED_OWNED_PROVENANCE_INVALID: ' + '; '.join(problems))
    if data.get('implementation') != 'owned-dxmt-derived' or not isinstance(data.get('source'), dict):
        raise ValueError('staged manifest lacks repo-owned source provenance')
    if not isinstance(data['source'].get('dxmt_install'), str) or not data['source']['dxmt_install']:
        raise ValueError('staged manifest lacks owned installation source')
    layout = runtime_architecture(data)
    owned = [f"{layout['unix_directory']}/winemetal.so", f"{layout['windows_directory']}/winemetal.dll"]
    if data.get('optional_modules', {}).get('wow64'):
        owned.append('lib/wine/i386-windows/winemetal.dll')
    return {relative: {**data['artifacts'][relative], 'source': data['source'],
                       'provenance_type': 'verified-owned-stage'} for relative in owned}


def verify_provenance(path: Path, runtime: Path, records: dict, owned_records: dict | None = None) -> dict:
    relative = str(path.relative_to(runtime))
    record = records.get(relative)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if path.name in ('winemetal.so', 'winemetal.dll'):
        owned = (owned_records or {}).get(relative)
        if not isinstance(owned, dict):
            raise ValueError(f'unknown repo-owned stage provenance: {relative}')
        if owned.get('sha256') != digest:
            raise ValueError(f'owned stage artifact hash mismatch: {relative}')
        return {'path': relative, **owned, 'sha256': digest}
    if not isinstance(record, dict) or not isinstance(record.get('source_url'), str):
        raise ValueError(f'unknown runtime provenance: {relative}')
    if not official_source(record['source_url']):
        raise ValueError(f'non-official runtime source: {relative}')
    if not isinstance(record.get('revision'), str) or not record['revision'].strip():
        raise ValueError(f'unpinned runtime source: {relative}')
    if urlparse(record['source_url']).hostname in ('github.com', 'gitlab.winehq.org') and not re.fullmatch('[0-9a-f]{40}', record['revision']):
        raise ValueError(f'runtime git revision must be a full commit: {relative}')
    if not re.fullmatch('[0-9a-f]{64}', str(record.get('sha256', ''))) or record['sha256'] != digest:
        raise ValueError(f'runtime provenance hash mismatch: {relative}')
    return {'path': relative, 'sha256': digest, **record}


def audit_candidate(runtime: Path, windows_abi: str, guest_arch: str, provenance: dict) -> dict:
    runtime = runtime.resolve()
    layout = architecture('arm64', windows_abi)
    blockers = []
    facts = []
    try:
        owned_records = verified_owned_artifacts(runtime)
    except (ValueError, OSError) as error:
        owned_records = {}
        blockers.append(str(error))
    records = provenance.get('artifacts', {}) if provenance.get('schema') == 1 else {}
    if not isinstance(records, dict):
        records = {}
    if not records:
        blockers.append('RUNTIME_PROVENANCE_UNKNOWN: supply pinned official source URLs and per-artifact SHA256 records')
    expected = {'x64': {'arm64ec', 'arm64x'}, 'arm64': {'arm64', 'arm64x'}, 'x86': {'arm64', 'arm64x'}}
    if windows_abi not in expected[guest_arch]:
        blockers.append(f'WINDOWS_ABI_MISMATCH: {guest_arch} guest cannot use {windows_abi} core DLLs')

    def check(path: Path, kind: str, allowed: set[str] | None = None) -> None:
        try:
            if not path.is_file():
                raise ValueError(f'missing {kind}: {path.relative_to(runtime)}')
            if not path.resolve().is_relative_to(runtime):
                raise ValueError(f'runtime symlink escapes audited root: {path}')
            if kind == 'native':
                detail = {'native_arch': macho_architectures(path)}
                if 'arm64' not in detail['native_arch']:
                    raise ValueError(f'ROSETTA_REQUIRED: no arm64 Mach-O slice: {path.relative_to(runtime)}')
            else:
                detail = inspect_pe(path)
                if detail['windows_abi'] not in allowed:
                    raise ValueError(f'PE_ABI_MISMATCH: {path.relative_to(runtime)} is {detail["windows_abi"]}, expected {sorted(allowed)}')
            origin = verify_provenance(path, runtime, records, owned_records)
            facts.append({'path': str(path.relative_to(runtime)), **detail, 'provenance': origin})
        except (ValueError, OSError) as error:
            blockers.append(str(error))

    loader = runtime / 'bin/wine'
    if (runtime / 'bin/wine.real').is_file():
        loader = runtime / 'bin/wine.real'
    native = {loader, runtime / 'bin/wineserver', runtime / layout['unix_directory'] / 'ntdll.so',
              runtime / layout['unix_directory'] / 'winemetal.so'}
    # Bounded selected Unix scope, not unused Intel siblings in a universal runtime.
    # This is not a dyld transitive-dependency or loaded-image closure audit.
    if runtime.is_dir():
        for path in sorted((runtime / layout['unix_directory']).rglob('*')):
            if path.is_file():
                try:
                    with path.open('rb') as stream:
                        magic = stream.read(4)
                    if magic in MACHO_MAGICS:
                        native.add(path)
                except OSError as error:
                    blockers.append(f'unreadable runtime artifact: {path}: {error}')
    else:
        blockers.append(f'RUNTIME_MISSING: {runtime}')
    for path in sorted(native):
        check(path, 'native')
    core_dir = runtime / layout['windows_directory']
    check(core_dir / 'ntdll.dll', 'PE', expected[guest_arch])
    translator_dir = runtime / 'lib/wine/i386-windows' if guest_arch == 'x86' else core_dir
    check(translator_dir / 'winemetal.dll', 'PE', {'x86'} if guest_arch == 'x86' else expected[guest_arch])
    if guest_arch == 'x64':
        check(core_dir / 'xtajit64.dll', 'FEX x64 PE module', {'arm64ec', 'arm64x'})
    elif guest_arch == 'x86':
        check(core_dir / 'xtajit.dll', 'FEX WoW64 PE module', {'arm64'})
        check(runtime / 'lib/wine/i386-windows/ntdll.dll', 'PE', {'x86'})
    return {'native_arch': 'arm64', 'windows_abi': windows_abi, 'guest_arch': guest_arch,
            'layout': layout, 'artifacts': facts, 'static_eligible': not blockers, 'blockers': blockers,
            'native_audit_scope': 'required loader/server and selected Unix directory Mach-O images',
            'native_dependency_closure_verified': False,
            'execution_status': 'BLOCKED', 'strict_no_rosetta_proven': False,
            'execution_blockers': ['Actual child architecture and loaded native dependency chain not observed',
                                   'Transitive dyld dependency closure not audited',
                                   'Callable, COM callback, struct and Unix-call fixture not executed']}


def compile_probes(toolchain: Path, out: Path) -> list[dict]:
    artifacts = []
    for guest_arch, callee_abi, guest_target, callee_target in MATRIX:
        folder = out / guest_arch
        folder.mkdir()
        commands = [
            [str(toolchain / f'{callee_target}-w64-mingw32-clang++'), '-std=c++20', '-O2', '-shared',
             '-static-libgcc', '-static-libstdc++', str(ROOT / 'graphics-translator/tests/arm64_abi_probe_dll.cpp'),
             '-luuid', '-o', str(folder / 'abi-callee.dll')],
            [str(toolchain / f'{guest_target}-w64-mingw32-clang++'), '-std=c++20', '-O2',
             '-static-libgcc', '-static-libstdc++', str(ROOT / 'graphics-translator/tests/arm64_abi_probe_guest.cpp'),
             '-luuid', '-o', str(folder / 'abi-guest.exe')],
        ]
        if callee_target in ('aarch64', 'arm64ec'):
            commands[0].insert(1, '-Wl,--section-alignment,16384')
            commands[0].insert(1, '-Wl,--file-alignment,16384')
        if guest_target == 'aarch64':
            commands[1].insert(1, '-Wl,--section-alignment,16384')
            commands[1].insert(1, '-Wl,--file-alignment,16384')
        if callee_target == 'i686':
            commands[0].insert(1, '-Wl,--kill-at')
            definition = folder / 'winemetal.def'
            definition.write_text('LIBRARY winemetal.dll\nEXPORTS\n WMTGetGuestWindowBase\n')
            library = folder / 'libwinemetal.a'
            subprocess.run([str(toolchain / 'llvm-dlltool'), '-m', 'i386', '-d', str(definition),
                            '-l', str(library)], check=True)
            commands[1].insert(-2, str(library))
        for index, command in enumerate(commands):
            if not Path(command[0]).is_file():
                raise ValueError(f'missing approved llvm-mingw compiler: {command[0]}')
            completed = subprocess.run(command, text=True, capture_output=True, check=False)
            (folder / f'compile-{index}.log').write_text(completed.stdout + completed.stderr)
            if completed.returncode:
                raise ValueError(f'probe compilation failed: {folder}/compile-{index}.log')
        guest, callee = folder / 'abi-guest.exe', folder / 'abi-callee.dll'
        guest_pe, callee_pe = inspect_pe(guest), inspect_pe(callee)
        if guest_pe['windows_abi'] != guest_arch or callee_pe['windows_abi'] != callee_abi:
            raise ValueError(f'compiled probe ABI mismatch: {guest_pe}, {callee_pe}')
        for path in (guest, callee):
            result = subprocess.run([str(toolchain / 'llvm-readobj'), '--file-headers', '--coff-load-config',
                                     str(path)], text=True, capture_output=True, check=True)
            (folder / (path.name + '.readobj.txt')).write_text(result.stdout)
        artifacts.append({'guest_arch': guest_arch, 'callee_windows_abi': callee_abi,
                          'guest': {'path': str(guest), **guest_pe}, 'callee': {'path': str(callee), **callee_pe},
                          'commands': commands, 'compiled': True, 'executed': False,
                          'boundary': 'x86 WoW64, not direct x86-to-ARM64 DLL' if guest_arch == 'x86' else 'guest-to-native PE ABI'})
    return artifacts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='fresh evidence directory')
    parser.add_argument('--llvm-mingw-bin', type=Path, default=DEFAULT_TOOLCHAIN)
    parser.add_argument('--compile-probes', action='store_true')
    parser.add_argument('--wine-runtime', type=Path)
    parser.add_argument('--windows-abi', choices=('arm64', 'arm64ec', 'arm64x'), default='arm64ec')
    parser.add_argument('--guest-arch', choices=('x64', 'arm64', 'x86'), default='x64')
    parser.add_argument('--provenance', type=Path, help='schema1 artifacts mapping relative paths to source_url/revision/sha256')
    args = parser.parse_args()
    if args.out.exists() or args.out.is_symlink():
        parser.error('--out must be a fresh path')
    if args.wine_runtime:
        output, runtime = args.out.resolve(), args.wine_runtime.resolve()
        if output.is_relative_to(runtime) or runtime.is_relative_to(output):
            parser.error('--out and --wine-runtime must not overlap; runtime inspection is read-only')
    args.out.mkdir(parents=True)
    result = {'schema': 1, 'status': 'BLOCKED', 'scope': 'static ABI preflight, no runtime launch',
              'strict_no_rosetta_proven': False, 'runtime_executed': False, 'probes': [], 'candidate': None,
              'blockers': [], 'required_evidence': ['Verified official Darwin Wine/FEX source or build provenance',
                                                   'Reproducible isolated execution recipe and child/loaded-chain observation'],
              'required_approvals': []}
    try:
        if args.compile_probes:
            result['probes'] = compile_probes(args.llvm_mingw_bin.resolve(), args.out.resolve())
        provenance = json.loads(args.provenance.read_text()) if args.provenance else {}
        if not isinstance(provenance, dict):
            raise ValueError('provenance must be a JSON object')
        if args.wine_runtime:
            result['candidate'] = audit_candidate(args.wine_runtime, args.windows_abi, args.guest_arch, provenance)
            result['blockers'] += result['candidate']['blockers'] + result['candidate']['execution_blockers']
        else:
            result['blockers'].append('NATIVE_RUNTIME_UNAVAILABLE: no audited Darwin ARM64 Wine runtime supplied')
        result['blockers'].append('RUNTIME_NOT_EXERCISED: compilation/static eligibility is not guest ABI or no-Rosetta proof')
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        result['status'] = 'FAILED'
        result['blockers'].append(str(error))
    (args.out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 1 if result['status'] == 'FAILED' else 2


if __name__ == '__main__':
    raise SystemExit(main())
