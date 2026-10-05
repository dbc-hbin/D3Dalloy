#!/bin/sh
# Rebuild genuine DXIL 1.9 vectors with DXC 1.9; inputs are frozen beside the GPU oracle.
# usage: build-vectors.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
compile() {
  profile=$1 entry=$2
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -enable-16bit-types \
    -denorm preserve -T "$profile" -E "$entry" "$here/sm6-vectors.hlsl" \
    -Fo "$out/sm6-$entry.dxil" -Fc "$out/sm6-$entry.ll"
}
for entry in cs_vectors cs_vectors1024 cs_vector_denorm cs_vector_double cs_vector_uint64 cs_vector_half cs_vector_structured cs_vector_heap cs_vector_tiles cs_vector_ssa; do
  compile cs_6_9 "$entry"
done
compile vs_6_9 vs_vectors
compile ps_6_9 ps_vectors
for entry in VECTOR_ROOT VECTOR_HEAP_ROOT; do
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -T rootsig_1_1 -E "$entry" \
    "$here/sm6-vectors.hlsl" -Fo "$out/sm6-$entry.dxbc"
done
