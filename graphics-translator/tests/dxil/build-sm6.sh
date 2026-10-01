#!/bin/sh
# Rebuilds the frozen SM 6.x DXIL fixtures from sm6*.hlsl with DXC 1.9 under Wine.
# usage: build-sm6.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
compile() {
  file=$1 profile=$2 entry=$3; shift 3
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" "$@" \
    "$here/$file" -Fo "$out/sm6-$entry.dxil" >/dev/null
}
compile sm6.hlsl cs_6_5 cs_wave
compile sm6.hlsl cs_6_5 cs_divergent
compile sm6.hlsl cs_6_6 cs_int64
compile sm6.hlsl cs_6_6 cs_atomic64
compile sm6.hlsl cs_6_6 cs_atomic64_ops
compile sm6-heap.hlsl cs_6_6 cs_atomic64_heap
compile sm6.hlsl cs_6_0 cs_minprec
compile sm6.hlsl cs_6_8 cs_quad_deriv
compile sm6.hlsl vs_6_8 vs_main
compile sm6.hlsl ps_6_7 ps_main
compile sm6.hlsl ps_6_7 ps_helper
compile sm6.hlsl ps_6_7 ps_helper_all
compile sm6.hlsl vs_6_8 vs_tiny
compile sm6.hlsl ps_6_6 ps_atomic64
compile sm6.hlsl ps_6_6 ps_umax64
compile sm6.hlsl vs_6_0 vs_int64
compile sm6.hlsl vs_6_5 vs_wave
compile sm6.hlsl ps_6_0 ps_null
compile sm6.hlsl ps_6_0 ps_int64
compile sm6-16bit.hlsl cs_6_2 cs_16bit -enable-16bit-types
compile sm6-16bit.hlsl cs_6_6 cs_packed -enable-16bit-types
compile sm6-16bit.hlsl vs_6_2 vs_16bit -enable-16bit-types
compile sm6-16bit.hlsl ps_6_2 ps_16bit -enable-16bit-types
compile sm6-reject.hlsl cs_6_6 cs_wavesize32
compile sm6-reject.hlsl cs_6_6 cs_wavesize64
compile sm6-reject.hlsl cs_6_0 cs_fp64
compile sm6-reject.hlsl cs_6_6 cs_gs64
compile sm6-reject.hlsl cs_6_9 cs_sm69
compile sm6.hlsl vs_6_1 vs_viewid
compile sm6.hlsl ps_6_1 ps_viewid

compile sm6-reject.hlsl ps_6_1 ps_viewid_vi
compile sm6-denorm.hlsl cs_6_2 cs_denorm_any -denorm any
compile sm6-denorm.hlsl cs_6_2 cs_denorm_ftz -denorm ftz
compile sm6-denorm.hlsl cs_6_2 cs_denorm_preserve -denorm preserve
