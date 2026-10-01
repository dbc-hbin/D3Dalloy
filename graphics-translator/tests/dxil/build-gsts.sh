#!/bin/sh
# Rebuilds the frozen SM 6.x DXIL fixtures of the geometry and tessellation suites from geometry.hlsl and
# tessellation.hlsl with DXC 1.9 under Wine (vs/gs/hs/ds/ps_6_0, -HV 2021). The fixtures read
# <suite>-<entry>.dxil beside the executable and run their DXIL matrix when <suite>-vs_pass.dxil exists.
# usage: build-gsts.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
status=0
compile() {
  suite=$1 entry=$2
  profile=${entry%%_*}_6_0
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" \
      "$here/$suite.hlsl" -Fo "$out/$suite-$entry.dxil" >/dev/null; then
    echo "failed: $suite $entry" >&2
    status=1
  fi
}
for entry in vs_pass gs_quad gs_tri gs_linein gs_lineadj gs_triadj gs_line gs_points gs_streams2 gs_streams4 ps_color \
  ps_early; do
  compile geometry "$entry"
done
# owned-d3d12-stream-output.cpp GS multi-stream cases.
for entry in vs_gs gs_multi; do
  compile stream-output-gs "$entry"
done
for entry in vs_pass ps_color ps_early ps_tsgs_early ds_tri ds_quad ds_isoline ds_patch gs_tsgs \
  hs_patch1 hs_patch2 hs_patch5 hs_patch8 hs_patch11 hs_patch16 hs_patch17 hs_patch32; do
  compile tessellation "$entry"
done
for entry in vs_main gs_point gs_strip gs_adj hs_main ds_main gs_tri ps_main; do
  compile gsindirect "$entry"
done
for partition in integer pow2 fractional_odd fractional_even; do
  compile tessellation "hs_tri_$partition"
  compile tessellation "hs_quad_$partition"
  compile tessellation "hs_tri_${partition}_point"
  compile tessellation "hs_quad_${partition}_point"
  compile tessellation "hs_isoline_${partition}_line"
  compile tessellation "hs_isoline_${partition}_point"
done
exit $status
