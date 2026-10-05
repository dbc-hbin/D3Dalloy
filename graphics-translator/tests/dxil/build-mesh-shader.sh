#!/bin/sh
# Rebuilds the frozen mesh-shader DXIL fixtures from mesh-shader.hlsl with DXC 1.9 under Wine.
# usage: build-mesh-shader.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
include="Z:$here"
compile() {
  profile=$1 entry=$2
  shift 2
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" \
    "$here/mesh-shader.hlsl" -I "$include" "$@" -Fo "$out/mesh-shader-$entry.dxil" >/dev/null
}
for entry in ms_basic ms_grid ms_payload ms_cull ms_prim ms_layer ms_lines ms_max ms_count ms_partial ms_viewid ms_rtai \
    ms_rootk ms_early ms_contract124 ms_contract125 ms_contract128_perspective ms_contract128_affine \
    ms_contract128_flat ms_contract128_scalars ms_contract128_budget ms_contract128_split ms_contract_cull ms_contract_cull_line; do
  compile ms_6_5 "$entry"
done
for shape in 1d 2d 3d; do
  compile ms_6_6 "ms_derivatives_$shape" -enable-16bit-types
  compile as_6_6 "as_derivatives_$shape" -enable-16bit-types
done
compile ms_6_6 ms_derivative_payload
compile ms_6_6 ms_groupshared64
compile as_6_6 as_groupshared64
compile ms_6_6 ms_derivatives_invalid -enable-16bit-types -D DERIVATIVE_INVALID_SHAPE -Vd
compile as_6_6 as_derivatives_invalid -enable-16bit-types -D DERIVATIVE_INVALID_SHAPE -Vd
compile as_6_5 as_main
compile as_6_5 as_count
for entry in ps_color ps_prim ps_layer ps_max ps_rows ps_early ps_contract124 ps_contract125 \
    ps_contract128_perspective ps_contract128_affine ps_contract128_flat ps_contract128_scalars \
    ps_contract128_pull_perspective ps_contract128_pull_affine ps_contract128_split ps_contract_cull; do
  compile ps_6_5 "$entry"
done
