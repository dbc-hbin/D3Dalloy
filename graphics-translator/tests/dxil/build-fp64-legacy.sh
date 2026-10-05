#!/bin/sh
# Genuine Windows SDK FXC + its native D3DCompiler_47.dll are required; Wine's compiler is not the producer.
# usage: build-fp64-legacy.sh <wine> <fxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 fxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
for version in 50 51; do
  if [ "$version" = 50 ]; then profile=5_0; else profile=5_1; fi
  for stage in cs vs ps; do
    if [ "$stage" = cs ]; then compute=1; else compute=0; fi
    WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES=d3dcompiler_47=n \
      "$wine" "$fxc" /nologo /Gis /D "FP64_COMPUTE=$compute" /T "${stage}_${profile}" /E "${stage}_fp64_legacy" \
      /Fo "Z:$out/sm6-${stage}_fp64_legacy${version}.dxbc" \
      /Fc "Z:$out/sm6-${stage}_fp64_legacy${version}.asm" "Z:$here/fp64-legacy.hlsl"
  done
done
