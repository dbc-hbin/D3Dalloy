#!/bin/sh
# Rebuilds the frozen DXIL fixtures of the conservative-raster suite from conservative-raster.hlsl with DXC 1.9
# under Wine (6.0 profiles, -HV 2021). owned-d3d12-conservative-raster.exe reads conservative-raster-<entry>.dxil
# beside itself.
# usage: build-conservative-raster.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir> [conservative-distance]
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
source=${5:-conservative-raster}
here=$(cd "$(dirname "$0")" && pwd)
status=0
for entry in vs_main ps_main gs_main hs_main ds_main ms_main ms_as as_main; do
  profile=${entry%%_*}_6_0
  defines=
  case "$entry" in ms_*|as_*) profile=${entry%%_*}_6_5; defines=-DCONSERVATIVE_MESH=1;; esac
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 $defines -T "$profile" -E "$entry" \
      "$here/$source.hlsl" -Fo "$out/$source-$entry.dxil" >/dev/null; then
    echo "failed: $entry" >&2
    status=1
  fi
done
exit $status
