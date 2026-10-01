#!/bin/sh
# Rebuilds the frozen DXIL fixtures of the sampler-feedback suite from sampler-feedback.hlsl with DXC 1.9 under
# Wine (ps_6_5/cs_6_5, -HV 2021). owned-d3d12-sampler-feedback.exe reads sampler-feedback-<entry>.dxil beside
# itself; <entry> is vs_main or <shader>_<kind>[_array] with kind minmip or used.
# usage: build-sampler-feedback.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
status=0
compile() {
  profile=$1 entry=$2 name=$3; shift 3
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$profile" -E "$entry" "$@" \
      "$here/sampler-feedback.hlsl" -Fo "$out/sampler-feedback-$name.dxil" >/dev/null; then
    echo "failed: $name" >&2
    status=1
  fi
}
compile vs_6_5 vs_main vs_main
for kind in minmip used; do
  case $kind in
  minmip) define=SAMPLER_FEEDBACK_MIN_MIP ;;
  used) define=SAMPLER_FEEDBACK_MIP_REGION_USED ;;
  esac
  for array in "" _array; do
    flags="-D KIND=$define"
    [ -n "$array" ] && flags="$flags -D ARRAY=1"
    compile cs_6_5 cs_main "cs_main_$kind$array" $flags
    for ps in ps_implicit ps_clamp ps_bias; do
      compile ps_6_5 $ps "${ps}_$kind$array" $flags
    done
  done
done
exit $status
