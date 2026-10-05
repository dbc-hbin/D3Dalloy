#!/bin/sh
# usage: sh build-advanced-texture.sh <wine> <final-dxc.exe> <dedicated-WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
compile() {
  profile=$1 entry=$2 name=$3; shift 3
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" "$@" \
    "$here/advanced-texture.hlsl" -Fo "$out/advanced-texture-$name.dxil"
}
compile vs_6_7 nullVS null-vs
compile ps_6_7 nullPS null-ps
for shape in 0 1 2 3 4 5; do
  for stage in cs vs ps gs hs ds as ms; do
    compile "${stage}_6_7" "${stage}Main" "cmp${shape}-${stage}" -DSHAPE=$shape
  done
  compile lib_6_7 rayMain "cmp${shape}-lib" -DSHAPE=$shape -DLIBRARY
done
for bits in 16 32 64; do
  for shape in 2 3; do
    compile cs_6_7 csMain "raw${bits}-${shape}" -DRAW -DRAW_BITS=$bits -DSHAPE=$shape -enable-16bit-types
  done
done
