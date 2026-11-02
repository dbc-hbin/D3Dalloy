#!/bin/sh
# usage: build-work-graphs.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 \
  "$here/work-graphs.hlsl" -Fo "$out/work-graphs-nodes.dxil"
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 \
  "$here/work-graphs-sharing.hlsl" -Fo "$out/work-graphs-sharing.dxil"
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 \
  "$here/work-graphs-linkage.hlsl" -Fo "$out/work-graphs-link-nodes.dxil"
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 -D WG_LINK_HELPERS \
  "$here/work-graphs-linkage.hlsl" -Fo "$out/work-graphs-link-helpers.dxil"
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 -D WG_LINK_ROOT_SUBOBJECTS \
  "$here/work-graphs-linkage.hlsl" -Fo "$out/work-graphs-root-subobjects.dxil"
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 "$here/workgraph-batches.hlsl" -Fo "$out/workgraph-batches.dxil"
