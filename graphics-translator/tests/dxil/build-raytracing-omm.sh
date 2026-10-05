#!/bin/sh
# Final upstream Microsoft DXC 1.9 produces both opt-in contracts.
# usage: build-raytracing-omm.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_9 -O0 \
  "$here/raytracing-omm.hlsl" -Fo "$out/raytracing-omm.dxil" -Fc "$out/raytracing-omm.ll"
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 -O0 -D OMM_LEGACY_QUERY=1 \
  "$here/raytracing-omm.hlsl" -Fo "$out/raytracing-omm-legacy.dxil" -Fc "$out/raytracing-omm-legacy.ll"
