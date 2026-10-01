#!/bin/sh
# Rebuilds the frozen DXIL fixtures of the vrs suite from vrs.hlsl with DXC 1.9 under Wine (6.1 profiles, -HV 2021).
# owned-d3d12-vrs.exe reads vrs-<entry>.dxil beside itself.
# usage: build-vrs.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
status=0
for entry in vs_main ps_main ps_snapped ps_discard ps_coverage ps_depth ps_flat vs_rate ps_rate ps_centroid ps_sample ps_wide_snapped gs_main hs_main ds_main ms_main as_main ms_as gs_rate ms_rate ms_as_rate vs_rate_view gs_rate_view ms_rate_view ms_as_rate_view; do
  profile=${entry%%_*}_6_1
  case "$entry" in vs_rate*|ps_rate|gs_rate*) profile=${entry%%_*}_6_4 ;; ms_*|as_*) profile=${entry%%_*}_6_5 ;; esac
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" \
      "$here/vrs.hlsl" -Fo "$out/vrs-$entry.dxil" >/dev/null; then
    echo "failed: $entry" >&2
    status=1
  fi
done
exit $status
