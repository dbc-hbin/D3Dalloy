#!/bin/sh
# Rebuild the three real DXC libraries for generic collection -> DXR linkage.
# usage: build-raytracing-linkage.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
for variant in consumer helper final; do
  define=
  case $variant in helper) define=-DLINK_HELPER=1 ;; final) define=-DLINK_FINAL=1 ;; esac
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_6 $define \
    "$here/raytracing-linkage.hlsl" -Fo "$out/raytracing-linkage-$variant.dxil" >/dev/null
done
