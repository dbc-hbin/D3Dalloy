#!/bin/sh
# CPU-only official DXC recipe: <wine> <dxc.exe> <dedicated-prefix> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" --version > "$out/atomic64-pixel-contention-dxc-version.txt"
WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T vs_6_6 -E vs \
  "$here/atomic64-pixel-contention.hlsl" -Fo "$out/atomic64-pixel-contention-vs.dxil"
for cas in 0 1; do
  entry=add
  [ "$cas" != 1 ] || entry=cas
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T ps_6_6 -E ps -D CAS=$cas \
    "$here/atomic64-pixel-contention.hlsl" -Fo "$out/atomic64-pixel-contention-$entry.dxil"
done
python3 - "$dxc" "$here" "$out" <<'PY'
import hashlib, json, pathlib, sys
compiler, source, output = map(pathlib.Path, sys.argv[1:])
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
inputs = [source / "atomic64-pixel-contention.hlsl", source / "build-atomic64-pixel-contention.sh"]
shaders = [output / f"atomic64-pixel-contention-{entry}.dxil" for entry in ("vs", "add", "cas")]
record = {"schema": 1, "producer": "official-stable-DXC", "compiler": str(compiler.resolve()),
          "compiler_sha256": digest(compiler),
          "compiler_version": (output / "atomic64-pixel-contention-dxc-version.txt").read_text().strip(),
          "source_sha256": {path.name: digest(path) for path in inputs},
          "dxil_sha256": {path.name: digest(path) for path in shaders},
          "compile_flags": ["-nologo", "-HV", "2021", "-T", "vs_6_6|ps_6_6", "-E", "vs|ps", "-D", "CAS=0|1"]}
(output / "atomic64-pixel-contention-provenance.json").write_text(json.dumps(record, indent=2) + "\n")
PY
