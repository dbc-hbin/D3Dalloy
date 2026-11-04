#!/bin/sh
# usage: build-stream-output-frontier.sh <wine> <dxc.exe> <dedicated-prefix> <fresh-out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -E vs_main -T vs_6_8 \
  "$here/stream-output-frontier.hlsl" -Fo "$out/stream-output-frontier-vs.dxil"
