#!/bin/sh
# usage: sh build-depth-casting.sh <wine> <final-dxc.exe> <dedicated-WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
for bits in 16 32; do
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_7 -E main \
    -denorm preserve -enable-16bit-types -DBITS=$bits "$here/depth-casting.hlsl" -Fo "$out/depth-casting-$bits.dxil"
  for stage in cs ps; do
    entry=main
    if [ "$stage" = ps ]; then entry=psMain; fi
    WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "${stage}_6_7" -E "$entry" \
      -denorm preserve -DMSAA -DBITS=$bits "$here/depth-casting.hlsl" -Fo "$out/depth-casting-$bits-msaa-$stage.dxil"
  done
done
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T vs_6_7 -E vsMain \
  -DMSAA -DBITS=32 "$here/depth-casting.hlsl" -Fo "$out/depth-casting-msaa-vs.dxil"
