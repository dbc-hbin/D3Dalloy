#!/bin/sh
# usage: sh build-texture.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
compile() {
  profile=$1 entry=$2 name=$3; shift 3
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -O0 -T "$profile" -E "$entry" "$@" \
    "$here/basic-texture-ops.hlsl" -Fo "$out/$name.dxil"
}
compile vs_6_0 vsMain basic-texture-vs
compile ps_6_0 psMain basic-texture-ps
compile vs_6_0 offsetVS programmable-offsets-vs -DPROGRAMMABLE_OFFSETS
compile ps_6_7 offsetPS programmable-offsets-ps -DPROGRAMMABLE_OFFSETS
compile cs_6_7 offsetCS programmable-offsets-cs -DPROGRAMMABLE_OFFSETS
