#!/bin/sh
# Genuine DXC inputs; usage: sh build-atomic64-helpers.sh <wine> <dxc.exe> <prefix> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
for entry in vs_helpers ps_helpers ps_helpers_sample ps_helpers_early ps_helpers_inclusive; do
  case "$entry" in
    vs_helpers) stage=vs_6_7; name=vs ;;
    ps_helpers) stage=ps_6_7; name=ps ;;
    ps_helpers_sample) stage=ps_6_7; name=sample ;;
    ps_helpers_early) stage=ps_6_7; name=early ;;
    ps_helpers_inclusive) stage=ps_6_7; name=inclusive ;;
  esac
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T "$stage" \
    -E "$entry" "$here/atomic64-helpers.hlsl" -Fo "$out/atomic64-helpers-$name.dxil"
done
