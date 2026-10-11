"""Read-only census and source-bound historical timing extraction; no benchmark inference."""
import collections
import datetime
import hashlib
import json
from pathlib import Path

live = Path('/Users/hanbinnoh/Library/Application Support/Yaagl ZZZ OS Test')
runtime = live / 'wine'
manifest = json.loads((runtime / 'owned-graphics-runtime.json').read_text())
identity = []
for name, info in manifest['artifacts'].items():
    if any(part in name for part in ('d3d12.dll', 'nvngx.dll', 'winemetal.so', 'libairconv')):
        data = (runtime / name).read_bytes()
        actual = hashlib.sha256(data).hexdigest()
        identity.append({'path': name, 'sha256': actual, 'expected': info['sha256'], 'match': actual == info['sha256']})
assert identity and all(row['match'] for row in identity)
cache = live / 'wineprefix/drive_c/users/hanbinnoh/AppData/Local/Temp/dxmt-shader-sessions/57a461904bc14837b3dbef13813d41086c5b48e7'
stats = [p.stat() for p in cache.glob('dxmt-pso-*.bin')]
assert stats
utc = lambda value: datetime.datetime.fromtimestamp(value, datetime.timezone.utc).isoformat()
timing_path = Path('build/local/zzz-dlss-fix-20261009/observe-run/game.log')
rows = collections.defaultdict(dict)
for number, line in enumerate(timing_path.read_text(errors='replace').splitlines(), 1):
    marker = 'OWNED_GRAPHICS_TRACE '
    if marker not in line:
        continue
    payload = line.split(marker, 1)[1]
    try:
        event = json.loads(payload)
    except json.JSONDecodeError:
        continue
    if event.get('event') == 'pipeline_compute_timing' and event['creation'] in range(139, 144):
        rows[event['creation']][event['phase']] = {'line': number, 'ns': event['ns'], 'ms': event['ns'] / 1_000_000}
assert len(rows) == 5 and all('compile' in phases and 'pipeline' in phases for phases in rows.values())
print(json.dumps({'scope': 'On-disk identities now, cache residue now, historical observe-run timings; NOT loaded-image or latest FPS proof', 'identity': identity, 'cache': {'files': len(stats), 'bytes': sum(s.st_size for s in stats), 'MiB': sum(s.st_size for s in stats) / 1048576, 'mtime_min_utc': utc(min(s.st_mtime for s in stats)), 'mtime_max_utc': utc(max(s.st_mtime for s in stats))}, 'historical_timing_source': str(timing_path), 'timings': rows}, indent=2))
