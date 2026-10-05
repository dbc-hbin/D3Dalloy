#!/bin/sh
# usage: sh build-resource-review.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
for entry in cs_bounds cs_gradients cs_array_atomics; do
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_0 -E "$entry" \
    "$here/resource-review.hlsl" -Fo "$out/resource-review-$entry.dxil"
done
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_6 -E cs_bounds64 -DREVIEW64 \
  "$here/resource-review.hlsl" -Fo "$out/resource-review-cs_bounds64.dxil"
