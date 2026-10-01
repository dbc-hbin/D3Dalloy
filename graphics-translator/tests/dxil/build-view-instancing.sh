#!/bin/sh
# Rebuilds the frozen DXIL fixtures of the view-instancing suite from view-instancing.hlsl with DXC 1.9 under Wine
# (6.1 profiles, -HV 2021). owned-d3d12-view-instancing.exe reads view-instancing-<entry>.dxil beside itself.
# usage: build-view-instancing.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
status=0
for entry in vs_main ps_uint ps_unorm gs_view vs_tess hs_view ds_view; do
  profile=${entry%%_*}_6_1
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" \
      "$here/view-instancing.hlsl" -Fo "$out/view-instancing-$entry.dxil" >/dev/null; then
    echo "failed: $entry" >&2
    status=1
  fi
done
exit $status
