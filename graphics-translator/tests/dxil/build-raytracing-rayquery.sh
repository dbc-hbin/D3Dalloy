#!/bin/sh
# Rebuilds the frozen RayQuery DXIL fixtures from raytracing-rayquery.hlsl with DXC 1.9 under Wine.
# usage: build-raytracing-rayquery.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
compile() {
  profile=$1 entry=$2 define=$3 name=$4
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" ${entry:+-E "$entry"} -D "$define" \
    "$here/raytracing-rayquery.hlsl" -Fo "$out/$name" >/dev/null
}
compile cs_6_5 cs_rayquery RQ_CS raytracing-rayquery-cs.dxil
compile vs_6_5 vs_rayquery RQ_VS raytracing-rayquery-vs.dxil
compile ps_6_5 ps_rayquery RQ_PS raytracing-rayquery-ps.dxil
compile lib_6_5 "" RQ_LIB raytracing-rayquery-lib.dxil
compile vs_6_5 vs_fullscreen RQ_PS raytracing-rayquery-fs.dxil
compile cs_6_5 cs_rays_args RQ_ARGS raytracing-rayquery-args.dxil
