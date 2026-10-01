#!/bin/sh
# Rebuilds the frozen DXR coverage library from raytracing-coverage.hlsl with DXC 1.9 under Wine.
# usage: build-raytracing-coverage.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_5 \
  "$here/raytracing-coverage.hlsl" -Fo "$out/raytracing-coverage.dxil" >/dev/null
