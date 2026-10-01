#!/bin/sh
# Rebuilds the frozen DXIL fixtures of the feature-level suite from feature-level.hlsl with DXC 1.9 under Wine
# (6.0 profiles, -HV 2021). owned-d3d12-feature-level.exe reads feature-level-<entry>.dxil beside itself.
# usage: build-feature-level.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
status=0
for entry in vs_main ps_coverage ps_color; do
  profile=${entry%%_*}_6_0
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" \
      "$here/feature-level.hlsl" -Fo "$out/feature-level-$entry.dxil" >/dev/null; then
    echo "failed: $entry" >&2
    status=1
  fi
done
exit $status
