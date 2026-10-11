#!/bin/sh
# Rebuilds the frozen DXIL fixtures of the feature-level suite from feature-level.hlsl with DXC 1.9 under Wine
# (raster 6.0, admission 6.6, -HV 2021). The fixture reads feature-level-<entry>.dxil beside itself.
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
for width in 4 8 16 32; do
  entry=cs_admission$width
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_6 -E "$entry" \
      -DFEATURE_SM66 "$here/feature-level.hlsl" -Fo "$out/feature-level-$entry.dxil" >/dev/null; then
    echo "failed: $entry" >&2
    status=1
  fi
done
if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_6 \
    -enable-payload-qualifiers "$here/raytracing-paq.hlsl" -Fo "$out/raytracing-paq.dxil" >/dev/null; then
  echo "failed: PAQ library" >&2
  status=1
fi
exit $status
