#!/bin/sh
# Genuine final DXC stages; usage: sh build-atomic64-phase.sh <wine> <dxc.exe> <isolated-prefix> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
for entry in aliases_and_map prepare returning observe32; do
  stem=atomic64-phase-$entry
  if [ "$entry" = aliases_and_map ]; then stem=atomic64-phase; fi
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_6 \
    -E "$entry" "$here/atomic64-phase.hlsl" -Fo "$out/$stem.dxil"
done
