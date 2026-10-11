"""Read-only quantitative proof for the captured ZZZ log sources."""
import collections
import hashlib
import json
from pathlib import Path

ROOT = Path('/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test')
paths = [ROOT / 'logs/game_1791530994266.log', ROOT / 'ZenlessZoneZero_d3d12.log']
result = {}
for path in paths:
    raw = path.read_bytes()
    lines = raw.decode('utf-8', errors='replace').splitlines()
    errors = collections.Counter(line.strip() for line in lines if 'Failed to compile ' in line)
    evidence = []
    terms = ('temporal', 'ngx', 'metalfx', 'command buffer', 'device removed', 'encoding failed', 'ExecuteCommandLists', 'Close failed')
    for number, line in enumerate(lines, 1):
        if any(term.lower() in line.lower() for term in terms):
            evidence.append({'line': number, 'text': line})
    result[str(path)] = {
        'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw), 'lines': len(lines),
        'compile_error_total': sum(errors.values()), 'compile_error_messages': dict(errors),
        'input_layout_warnings': sum('CreateInputLayout: Vertex shader expects POSITION_0' in line for line in lines),
        'temporal_error_evidence': evidence,
        'shader_probe_success': sum('D3D12 shader probe[' in line and 'hr=0' in line for line in lines),
    }
a, b = result.values()
same = a['compile_error_messages'] == b['compile_error_messages']
print(json.dumps({'observed_at': '2026-10-09', 'same_error_multiset': same, 'counting_rule': 'Report each sink independently; never add overlapping logs', 'sources': result}, ensure_ascii=False, indent=2))
