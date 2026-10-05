#!/bin/sh
# CPU-only official DXC recipe: <wine> <dxc.exe> <dedicated-prefix> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
for entry in vs probe; do
  stage=ps_6_6
  [ "$entry" != vs ] || stage=vs_6_6
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$stage" \
    -E "$entry" "$here/atomic64-pixel.hlsl" -Fo "$out/atomic64-pixel-$entry.dxil"
done
for rov in 0 1; do for typed in 0 1; do for sample in 0 1; do for early in 0 1; do
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T ps_6_6 -E ps \
    -D ROV=$rov -D TYPED=$typed -D SAMPLE=$sample -D EARLY=$early \
    "$here/atomic64-pixel.hlsl" -Fo "$out/atomic64-pixel-r$rov-t$typed-s$sample-e$early.dxil"
done; done; done; done
