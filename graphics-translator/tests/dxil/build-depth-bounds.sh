#!/bin/sh
# usage: sh build-depth-bounds.sh <wine> <official-dxc.exe> <dedicated-WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
for entry in vs plain seed late early lateDiscard earlyDiscard read; do
  stage=ps
  [ "$entry" != vs ] || stage=vs
  [ "$entry" != read ] || stage=cs
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "${stage}_6_0" -E "$entry" \
    "$here/depth-bounds.hlsl" -Fo "$out/depth-bounds-$entry.dxil"
done
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_0 -E read -DMSAA \
  "$here/depth-bounds.hlsl" -Fo "$out/depth-bounds-read-msaa.dxil"
