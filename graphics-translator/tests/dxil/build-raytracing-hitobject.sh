#!/bin/sh
# Rebuild with final upstream Microsoft DXC 1.9 (SM 6.9), never a vendor converter.
# usage: build-raytracing-hitobject.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_9 -O0 \
  "$here/raytracing-hitobject.hlsl" -Fo "$out/raytracing-hitobject.dxil" \
  -Fc "$out/raytracing-hitobject.ll"
