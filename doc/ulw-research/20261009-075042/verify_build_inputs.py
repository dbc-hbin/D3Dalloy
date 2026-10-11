"""Compare recorded source inputs and PE code sections without modifying binaries."""
import hashlib
import json
import struct
from pathlib import Path

new = json.loads(Path('build/local/yaagl-latest-20261009/package/source-cut.json').read_text())
old = json.loads(Path('build/local/zzz-dlss-fix-20261009/builds/cache-04/source-cut.json').read_text())
changed = [key for key in sorted(new.keys() | old.keys()) if new.get(key) != old.get(key)]
roots = [Path('/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test/wine/lib/wine/aarch64-windows'), Path('build/local/zzz-fex-opt-20261009/runtime/lib/wine/aarch64-windows')]
output = {}
for name in ('d3d12.dll', 'd3d12core.dll', 'nvngx.dll', 'dxgi.dll', 'd3d11.dll', 'xtajit64.dll'):
    files = []
    for root in roots:
        data = (root / name).read_bytes()
        assert data[:2] == b'MZ'
        pe = struct.unpack_from('<I', data, 60)[0]
        assert data[pe:pe+4] == b'PE\0\0'
        count = struct.unpack_from('<H', data, pe + 6)[0]
        optional_size = struct.unpack_from('<H', data, pe + 20)[0]
        sections = {}
        for index in range(count):
            offset = pe + 24 + optional_size + index * 40
            label = data[offset:offset+8].split(b'\0')[0].decode('ascii')
            virtual_size, rva, size, raw = struct.unpack_from('<IIII', data, offset + 8)
            flags = struct.unpack_from('<I', data, offset + 36)[0]
            if flags & 0x20000000:
                assert raw + size <= len(data)
                sections[label] = {'rva': rva, 'virtual_bytes': virtual_size, 'sha256': hashlib.sha256(data[raw:raw+min(size, virtual_size)]).hexdigest()}
        files.append(sections)
    output[name] = {'executable_sections_equal': files[0] == files[1], 'sections': files}
print(json.dumps({'source_entries_new': len(new), 'source_entries_old': len(old), 'source_differences': changed, 'PE_comparison': output, 'limit': 'Equal code bytes do not imply all data/build inputs or runtime settings are equal; differing code bytes do not by themselves establish a regression.'}, indent=2))
