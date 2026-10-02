"""Binary architecture facts and owned Wine layout contracts, not execution proof."""
from __future__ import annotations

from pathlib import Path
import struct

NATIVE_ARCHES = ("x86_64", "arm64")
WINDOWS_ABIS = ("x86", "x64", "arm64", "arm64ec", "arm64x")
PE_MACHINES = {0x14C: "x86", 0x8664: "x64", 0xAA64: "arm64", 0xA641: "arm64ec", 0xA64E: "arm64x"}


def macho_architectures(path: Path) -> list[str]:
    data = path.read_bytes()
    machines = {0x1000007: "x86_64", 0x100000C: "arm64"}
    thin = {b"\xcf\xfa\xed\xfe": "<", b"\xfe\xed\xfa\xcf": ">"}
    fat = {b"\xca\xfe\xba\xbe": (">", 20), b"\xbe\xba\xfe\xca": ("<", 20),
           b"\xca\xfe\xba\xbf": (">", 32), b"\xbf\xba\xfe\xca": ("<", 32)}
    if data[:4] in thin and len(data) >= 32:
        cpu = struct.unpack_from(thin[data[:4]] + "I", data, 4)[0]
        if cpu not in machines:
            raise ValueError(f"unsupported Mach-O CPU 0x{cpu:x}: {path}")
        return [machines[cpu]]
    if data[:4] not in fat or len(data) < 8:
        raise ValueError(f"not a supported Mach-O image: {path}")
    endian, width = fat[data[:4]]
    count = struct.unpack_from(endian + "I", data, 4)[0]
    if not count or 8 + width * count > len(data):
        raise ValueError(f"truncated Mach-O fat header: {path}")
    result = []
    for index in range(count):
        entry = 8 + width * index
        cpu = struct.unpack_from(endian + "I", data, entry)[0]
        offset, size = struct.unpack_from(endian + ("II" if width == 20 else "QQ"), data, entry + 8)
        if cpu not in machines or size < 32 or offset + size > len(data):
            raise ValueError(f"invalid Mach-O fat slice: {path}")
        magic = data[offset:offset + 4]
        if magic not in thin or struct.unpack_from(thin[magic] + "I", data, offset + 4)[0] != cpu:
            raise ValueError(f"Mach-O fat slice CPU mismatch: {path}")
        if machines[cpu] in result:
            raise ValueError(f"duplicate Mach-O fat CPU: {path}")
        result.append(machines[cpu])
    return result


def inspect_pe(path: Path) -> dict:
    """Inspect final PE headers, CHPE code map and ARM64X dynamic relocations.

    AMD64+CHPE is ARM64EC, not x64. An ARM64X hybrid is separately recorded;
    an intermediate ARM64EC/ARM64X machine label alone is not a final image.
    """
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"not a PE file: {path}")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"invalid PE signature: {path}")
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if machine not in PE_MACHINES or size < (96 if machine == 0x14C else 112) or optional + size + count * 40 > len(data):
        raise ValueError(f"unsupported/truncated PE headers: {path}")
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic not in (0x10B, 0x20B) or (machine == 0x14C) != (magic == 0x10B):
        raise ValueError(f"PE machine/optional-header mismatch: {path}")
    directories = optional + (96 if magic == 0x10B else 112)
    directory_count = struct.unpack_from("<I", data, directories - 4)[0]
    if directories + directory_count * 8 > optional + size:
        raise ValueError(f"truncated PE data directories: {path}")
    sections = []
    for index in range(count):
        entry = optional + size + index * 40
        vsize, rva, rawsize, raw = struct.unpack_from("<IIII", data, entry + 8)
        flags = struct.unpack_from("<I", data, entry + 36)[0]
        if raw + rawsize > len(data):
            raise ValueError(f"truncated PE section: {path}")
        sections.append((rva, vsize, raw, rawsize, flags))

    def offset(rva: int, length: int) -> int:
        for start, _, raw, rawsize, _ in sections:
            if start <= rva and rva + length <= start + rawsize:
                return raw + rva - start
        raise ValueError(f"unbacked PE RVA 0x{rva:x}: {path}")

    abi = PE_MACHINES[machine]
    result = {"machine": machine, "windows_abi": abi, "chpe_metadata_rva": None,
              "code_ranges": [], "arm64x_relocations": False}
    if magic == 0x20B and directory_count > 10:
        load_rva, load_size = struct.unpack_from("<II", data, directories + 80)
        if bool(load_rva) != bool(load_size) or (load_size and load_size < 4):
            raise ValueError(f"invalid PE load configuration directory: {path}")
        if load_rva and load_size:
            load = offset(load_rva, load_size)
            declared = struct.unpack_from("<I", data, load)[0]
            if declared > load_size:
                raise ValueError(f"truncated PE load configuration: {path}")
            image_base = struct.unpack_from("<Q", data, optional + 24)[0]
            if declared >= 208:
                chpe = struct.unpack_from("<Q", data, load + 200)[0]
                if chpe:
                    meta_rva = chpe - image_base
                    meta = offset(meta_rva, 12)
                    version, code_map, code_count = struct.unpack_from("<III", data, meta)
                    if version not in (1, 2) or not code_count:
                        raise ValueError(f"unsupported/empty ARM64EC code map: {path}")
                    table = offset(code_map, code_count * 8)
                    ranges = []
                    for index in range(code_count):
                        start, length = struct.unpack_from("<II", data, table + index * 8)
                        kind, start = start & 3, start & ~3
                        if kind > 2 or not length:
                            raise ValueError(f"invalid ARM64EC code range: {path}")
                        # LLD coalesces same-kind regions across alignment gaps
                        # (.text to .hexpthk). Endpoints must be file-backed code.
                        offset(start, 1)
                        offset(start + length - 1, 1)
                        if any(rva < start + length and start < rva + rawsize and not flags & 0x20000000
                               for rva, _, _, rawsize, flags in sections):
                            raise ValueError(f"ARM64EC range is not executable: {path}")
                        ranges.append({"rva": start, "size": length, "architecture": ("arm64", "arm64ec", "x64")[kind]})
                    if not any(r["architecture"] == "arm64ec" for r in ranges):
                        raise ValueError(f"CHPE image lacks native ARM64EC code ranges: {path}")
                    result.update(chpe_metadata_rva=meta_rva, code_ranges=ranges)
                    abi = "arm64ec"
            # Modern linkers use the section+offset form of the DVRT pointer.
            if declared >= 232:
                dvrt_offset = struct.unpack_from("<I", data, load + 224)[0]
                dvrt_section = struct.unpack_from("<H", data, load + 228)[0]
                if dvrt_section:
                    if dvrt_section > len(sections):
                        raise ValueError(f"invalid ARM64X relocation section: {path}")
                    dvrt = sections[dvrt_section - 1][0] + dvrt_offset
                elif declared >= 200:
                    pointer = struct.unpack_from("<Q", data, load + 192)[0]
                    dvrt = pointer - image_base if pointer else 0
                else:
                    dvrt = 0
                if dvrt:
                    table = offset(dvrt, 8)
                    version, table_size = struct.unpack_from("<II", data, table)
                    if version != 1:
                        raise ValueError(f"unsupported dynamic relocation version {version}: {path}")
                    offset(dvrt, table_size + 8)
                    cursor, end = table + 8, table + 8 + table_size
                    while cursor < end:
                        if cursor + 12 > end:
                            raise ValueError(f"truncated dynamic relocation entry: {path}")
                        symbol, fixup_size = struct.unpack_from("<QI", data, cursor)
                        cursor += 12
                        if not fixup_size or cursor + fixup_size > end:
                            raise ValueError(f"invalid dynamic relocation size: {path}")
                        if symbol == 6:
                            block, block_end, fixups = cursor, cursor + fixup_size, 0
                            while block < block_end:
                                if block + 8 > block_end:
                                    raise ValueError(f"truncated ARM64X relocation block: {path}")
                                _, block_size = struct.unpack_from("<II", data, block)
                                if block_size < 8 or block_size % 4 or block + block_size > block_end:
                                    raise ValueError(f"invalid ARM64X relocation block size: {path}")
                                record = block + 8
                                while record < block + block_size:
                                    word = struct.unpack_from("<H", data, record)[0]
                                    if not word and not any(data[record:block + block_size]):
                                        break  # DWORD alignment padding
                                    kind, size_bits = (word >> 12) & 3, word >> 14
                                    if kind > 2 or (kind != 2 and not size_bits):
                                        raise ValueError(f"invalid ARM64X fixup record: {path}")
                                    width = 2 + ((1 << size_bits) if kind == 1 else 2 if kind == 2 else 0)
                                    if record + width > block + block_size:
                                        raise ValueError(f"truncated ARM64X fixup value: {path}")
                                    record += width
                                    fixups += 1
                                block += block_size
                            if not fixups:
                                raise ValueError(f"empty ARM64X relocation map: {path}")
                            result["arm64x_relocations"] = True
                        cursor += fixup_size
                    if result["arm64x_relocations"]:
                        abi = "arm64x"
    if machine in (0xA641, 0xA64E) and not result["chpe_metadata_rva"]:
        raise ValueError(f"intermediate ARM64EC/ARM64X label lacks final CHPE metadata: {path}")
    if abi == "arm64x" and not result["arm64x_relocations"]:
        raise ValueError(f"ARM64X label lacks hybrid dynamic relocations: {path}")
    result["windows_abi"] = abi
    return result


def runtime_architecture(manifest: dict) -> dict[str, str]:
    if not isinstance(manifest, dict):
        raise ValueError("runtime manifest must be an object")
    if manifest.get("schema") == 1:
        return architecture("x86_64", "x64")
    if manifest.get("schema") != 2 or not isinstance(manifest.get("architecture"), dict):
        raise ValueError("unsupported runtime manifest schema or missing architecture")
    record = manifest["architecture"]
    expected = architecture(record.get("native_arch"), record.get("windows_abi"))
    if record != expected:
        raise ValueError("runtime architecture/layout mismatch")
    return expected


def architecture(native_arch: str, windows_abi: str) -> dict[str, str]:
    if native_arch not in NATIVE_ARCHES or windows_abi not in ("x64", "arm64", "arm64ec", "arm64x"):
        raise ValueError("unsupported native_arch/windows_abi")
    if (native_arch == "x86_64") != (windows_abi == "x64"):
        raise ValueError("unsupported Wine native/Windows layout combination")
    windows = {"x64": "x86_64", "arm64": "aarch64", "arm64ec": "arm64ec", "arm64x": "aarch64"}[windows_abi]
    unix = "x86_64" if native_arch == "x86_64" else "aarch64"
    return {"native_arch": native_arch, "windows_abi": windows_abi,
            "windows_directory": f"lib/wine/{windows}-windows",
            "unix_directory": f"lib/wine/{unix}-unix"}


def execution_architecture(manifest: dict, fixture: Path, guest_arch: str | None = None) -> dict:
    record = runtime_architecture(manifest)
    pe = inspect_pe(fixture)
    inferred = {"x86": "x86", "x64": "x64", "arm64": "arm64"}.get(pe["windows_abi"])
    if inferred is None:
        raise ValueError("hybrid/ARM64EC fixture guest execution is not established")
    if guest_arch is not None and guest_arch != inferred:
        raise ValueError("--guest-arch does not match the fixture machine/ABI")
    if record["native_arch"] == "arm64":
        if inferred in ("x86", "x64"):
            raise ValueError("x86/x64 guest execution requires supported Darwin FEX runtime integration evidence; unavailable")
        if record["windows_abi"] != "arm64":
            raise ValueError("plain ARM64 guest requires the plain ARM64 translator ABI; ARM64EC/ARM64X interoperation is not established")
    elif inferred not in ("x64", "x86"):
        raise ValueError("guest architecture cannot execute with this Wine runtime")
    if inferred == "x86" and manifest.get("optional_modules", {}).get("wow64") is not True:
        raise ValueError("x86 fixture requires staged WoW64 modules")
    return {**record, "guest_arch": inferred, "fixture": pe, "strict_no_rosetta_proven": False}
