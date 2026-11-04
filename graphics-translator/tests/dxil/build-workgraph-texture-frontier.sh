#!/bin/sh
# usage: build-workgraph-texture-frontier.sh <wine> <dxc.exe> <dedicated-prefix> <fresh-out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
# The staged wrapper pins DLL overrides. Bootstrap without Mono/Gecko first,
# matching the owned graphics runner, so a fresh producer prefix cannot stall.
wine_dir=$(dirname "$wine")
if [ -x "$wine_dir/wine.real" ]; then
  WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
    WINEDLLPATH="$wine_dir/../lib/wine" "$wine_dir/wine.real" wineboot -i
fi
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 \
  "$here/workgraph-texture-frontier.hlsl" -Fo "$out/workgraph-texture-frontier.dxil"
