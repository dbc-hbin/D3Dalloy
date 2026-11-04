#!/bin/sh
# usage: build-workgraph-frontier.sh <wine> <dxc.exe> <dedicated-prefix> <fresh-out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T lib_6_8 \
  "$here/workgraph-frontier.hlsl" -Fo "$out/workgraph-frontier.dxil"
