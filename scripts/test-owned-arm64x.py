#!/usr/bin/env python3
"""Reject loader-visible corruption of the real paired DLL acceptance fixture."""
from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import re
import struct
import subprocess
import tempfile

SPEC = importlib.util.spec_from_file_location("owned_arm64x", Path(__file__).with_name("build-owned-arm64x.py"))
assert SPEC and SPEC.loader
paired = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(paired)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True, help="completed fresh paired build evidence")
    parser.add_argument("--llvm-mingw", type=Path, default=Path("/opt/llvm-mingw-20260616-ucrt-macos-universal"))
    args = parser.parse_args()
    image = args.build_root / "aarch64-windows/paired-dll-probe.dll"
    expected = {abi: {"owned_arm64x_probe": 1} for abi in ("arm64", "arm64ec")}

    def audit(path: Path, contract: dict = expected, *, builtin: bool = True, data_exports: tuple[str, ...] = ()) -> dict:
        dump = subprocess.run([str(args.llvm_mingw / "bin/llvm-readobj"), "--file-headers", "--coff-load-config",
                               "--coff-imports", "--coff-exports", str(path)], check=True, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
        return paired.audit_image(path, dump, contract, builtin=builtin, data_exports=data_exports)

    original = bytearray(image.read_bytes())
    facts = audit(image)
    pe = struct.unpack_from("<I", original, 60)[0]
    alternate = facts["views"]["arm64ec"]["entry_rva"]
    record = struct.pack("<HI", 0x9000 | ((pe + 40) & 0xfff), alternate)
    location = original.find(record)
    assert location >= 0 and original.find(record, location + 1) == -1, "expected unique entry DVRT value"
    marker = original.index(b"Wine builtin DLL")
    export_rva = struct.unpack_from("<I", original, pe + 136)[0]
    section_count = struct.unpack_from("<H", original, pe + 6)[0]
    sections = pe + 24 + struct.unpack_from("<H", original, pe + 20)[0]
    export_offset = None
    for index in range(section_count):
        _, rva, size, raw = struct.unpack_from("<IIII", original, sections + index * 40 + 8)
        if rva <= export_rva < rva + size:
            export_offset = raw + export_rva - rva
            break
    assert export_offset is not None, "expected a native export table"
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "invalid.dll"
        for name in ("native entry", "alternate entry", "builtin marker", "native exports", "export ordinal"):
            corrupted = original.copy()
            if name == "native entry":
                struct.pack_into("<I", corrupted, pe + 40, 0)
            elif name == "alternate entry":
                struct.pack_into("<I", corrupted, location + 2, 0)
            elif name == "builtin marker":
                corrupted[marker:marker + len(b"Wine builtin DLL")] = bytes(len(b"Wine builtin DLL"))
            elif name == "native exports":
                struct.pack_into("<II", corrupted, pe + 136, 0, 0)
            else:
                struct.pack_into("<I", corrupted, export_offset + 16, 2)
            path.write_bytes(corrupted)
            try:
                audit(path)
            except ValueError:
                continue
            raise AssertionError(f"accepted corrupted {name}")
        source = args.build_root / "aarch64-windows/dxgi.dll"
        path.write_bytes(source.read_bytes())
        dump = subprocess.run([str(args.llvm_mingw / "bin/llvm-readobj"), "--file-headers",
                               "--coff-load-config", str(path)], check=True, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
        tls_rvas = [int(rva, 16) for rva in re.findall(r"TLSTableRVA: (0x[0-9A-Fa-f]+)", dump)]
        assert len(tls_rvas) == 2 and tls_rvas[0], "expected a real CRT TLS fixture"
        if tls_rvas[0] == tls_rvas[1]:
            try:
                audit(path, {abi: {} for abi in expected})
            except ValueError:
                pass
            else:
                raise AssertionError("accepted native TLS callbacks in the EC view")
            paired.complete_tls_fixups(path, source.with_suffix(".map"))
        tls_facts = audit(path, {abi: {} for abi in expected})
        native_tls = tls_facts["views"]["arm64"]["tls_directory_rva"]
        ec_tls = tls_facts["views"]["arm64ec"]["tls_directory_rva"]
        assert native_tls != ec_tls and all(view["tls_callbacks"] for view in tls_facts["views"].values())
        corrupted = bytearray(path.read_bytes())
        tls_header = struct.unpack_from("<I", corrupted, 60)[0] + 208
        record = struct.pack("<HI", 0x9000 | (tls_header & 0xfff), ec_tls)
        location = corrupted.find(record)
        assert location >= 0 and corrupted.find(record, location + 1) == -1, "expected unique TLS DVRT value"
        struct.pack_into("<I", corrupted, location + 2, native_tls)
        path.write_bytes(corrupted)
        try:
            audit(path, {abi: {} for abi in expected})
        except ValueError:
            pass
        else:
            raise AssertionError("accepted a TLS fixup selecting native callbacks in the EC view")
        source = args.build_root / "aarch64-windows/d3d12core.dll"
        contract = {abi: {} for abi in expected}
        generation_export = ("OwnedD3D12CoreGeneration",)
        core = audit(source, contract, builtin=False, data_exports=generation_export)
        corrupted = bytearray(source.read_bytes())
        pe = struct.unpack_from("<I", corrupted, 60)[0]
        sections = pe + 24 + struct.unpack_from("<H", corrupted, pe + 20)[0]
        target = core["views"]["arm64ec"]["exports"][generation_export[0]]["rva"]
        for index in range(struct.unpack_from("<H", corrupted, pe + 6)[0]):
            _, rva, size, raw = struct.unpack_from("<IIII", corrupted, sections + index * 40 + 8)
            if rva <= target < rva + size:
                start = raw + target - rva
                corrupted[start] = ord("b") if corrupted[start] == ord("a") else ord("a")
                break
        else:
            raise AssertionError("EC Core generation is not backed by a section")
        path.write_bytes(corrupted)
        try:
            audit(path, contract, builtin=False, data_exports=generation_export)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted different native and EC Core generations")
    print("PASS: rejects zero native/EC entries, missing builtin/native exports and changed export ordinal")
    print("PASS: distinct real CRT TLS views; rejects native TLS callbacks in the EC view")
    print("PASS: shared Core generation; rejects a changed EC generation")


if __name__ == "__main__":
    main()
