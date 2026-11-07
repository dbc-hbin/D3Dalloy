#!/bin/sh
# Rebuilds the frozen root-argument-changing DISPATCH_RAYS DXIL fixture from raytracing-rootargs.hlsl with DXC 1.9
# under Wine.
# usage: build-raytracing-rootargs.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_5 \
  "$here/raytracing-rootargs.hlsl" -Fo "$out/raytracing-rootargs.dxil" >/dev/null
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_0 -E TransitionCompute \
  -D TRANSITION_COMPUTE=1 "$here/raytracing-rootargs.hlsl" -Fo "$out/raytracing-transition-cs.dxil" >/dev/null
