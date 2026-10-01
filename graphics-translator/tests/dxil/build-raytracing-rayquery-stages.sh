#!/bin/sh
# Freeze all-stage RayQuery DXIL with Microsoft DXC 1.9 under Wine.
# usage: build-raytracing-rayquery-stages.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
compile() {
  profile=$1 entry=$2 define=$3 name=$4
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" ${entry:+-E "$entry"} -D "$define" \
    -I "Z:$here" "$here/raytracing-rayquery-stages.hlsl" -Fo "$out/raytracing-rayquery-stages-$name.dxil" >/dev/null
}
compile vs_6_5 stage_vs RQS_NONE vs
compile hs_6_5 stage_hs RQS_HS hs
compile hs_6_5 stage_hs RQS_NONE hs-pass
compile ds_6_5 stage_ds RQS_DS ds
compile ds_6_5 stage_ds RQS_NONE ds-pass
compile gs_6_5 stage_gs RQS_NONE gs
compile as_6_5 stage_as RQS_NONE as
compile ms_6_5 stage_ms RQS_NONE ms
compile ms_6_5 stage_ms_sink RQS_NONE ms-sink
compile lib_6_5 "" RQS_LIB lib
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_5 -O0 -D RQ_NESTED_CALLBACKS \
  -I "Z:$here" "$here/raytracing.hlsl" -Fo "$out/raytracing.dxil" >/dev/null
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_5 -O0 -D RQ_NESTED_CALLBACKS \
  -I "Z:$here" "$here/raytracing-stateobj.hlsl" -Fo "$out/raytracing-stateobj.dxil" >/dev/null
