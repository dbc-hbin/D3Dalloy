#!/bin/sh
# Genuine final DXC stages; usage: sh build-atomic64-producers.sh <wine> <dxc.exe> <isolated-prefix> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
for entry in vs_plain vs_atomic hs_plain hs_atomic ds_plain ds_atomic gs_atomic as_atomic ms_atomic ms_payload ps_sample; do
  stage=${entry%%_*}
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "${stage}_6_6" \
    -E "$entry" "$here/atomic64-producers.hlsl" -Fo "$out/atomic64-producers-$entry.dxil"
done
