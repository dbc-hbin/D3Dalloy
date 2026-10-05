#!/bin/sh
# Genuine archived producers: DXC v1.8.2306-preview and v1.10.2605.37.
# usage: build-matrix.sh <wine> <retired-dxc.exe> <linalg-dxc.exe> <linalg-inc> <WINEPREFIX> <out-dir>
set -eu
wine=$1 retired=$2 linalg=$3 includes=$4 prefix=$5 out=$6
here=$(cd "$(dirname "$0")" && pwd)
# DXC runs inside Wine: callers must provide paths recognized by Windows,
# e.g. relative paths from the current directory or Z:\ absolute paths.
for entry in cs_wavematrix cs_wavematrix16x64 cs_wavematrix64x16 cs_wavematrix64x64; do
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$retired" -nologo -HV 2021 -enable-16bit-types -T cs_6_8 -E "$entry" "$here/wavematrix.hlsl" -Fo "$out/sm6-$entry.dxil"
done
for entry in cs_matrix_wave cs_matrix_required cs_matrix_vector cs_matrix_group cs_matrix_atomic cs_matrix_formats cs_matrix_groupformats; do
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$linalg" -nologo -HV 2021 -Wno-c++11-extensions -enable-16bit-types -I "$includes" -T cs_6_10 -E "$entry" "$here/matrix.hlsl" -Fo "$out/sm6-$entry.dxil"
done
