#!/bin/sh
# usage: build-writable-msaa.sh <wine> <dxc.exe> <isolated-WINEPREFIX> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
for samples in 2 4 8; do
  for shape in single array; do
    definitions=""
    test "$shape" != single || definitions="-D SINGLE_MSAA=1"
    for entry in cs_msaa_store cs_msaa_read cs_msaa_heap cs_msaa_srv cs_msaa_oob; do
      WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -T cs_6_7 -E "$entry" \
        -D SAMPLE_COUNT="$samples" $definitions "$here/owned-d3d12-writable-msaa.hlsl" \
        -Fo "$out/sm6-$entry$samples$shape.dxil"
    done
  done
done
for entry in cs_msaa_store cs_msaa_read; do
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -T cs_6_7 -E "$entry" \
    -D SAMPLE_COUNT=4 -D MSAA_RAW16=1 "$here/owned-d3d12-writable-msaa.hlsl" \
    -Fo "$out/sm6-$entry"raw16.dxil
done
for scalar in sint float; do
  definition=MSAA_SINT
  test "$scalar" != float || definition=MSAA_FLOAT
  for entry in cs_msaa_store cs_msaa_read cs_msaa_srv ps_msaa; do
    profile=cs_6_7
    test "$entry" != ps_msaa || profile=ps_6_7
    WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -T "$profile" -E "$entry" \
      -D SAMPLE_COUNT=4 -D "$definition"=1 "$here/owned-d3d12-writable-msaa.hlsl" \
      -Fo "$out/sm6-$entry$scalar.dxil"
  done
done
for entry in vs_msaa ps_msaa; do
  profile=vs_6_7
  test "$entry" != ps_msaa || profile=ps_6_7
  WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -T "$profile" -E "$entry" \
    "$here/owned-d3d12-writable-msaa.hlsl" -Fo "$out/sm6-$entry.dxil"
done
WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -T cs_6_7 -E cs_msaa_unused \
  "$here/owned-d3d12-writable-msaa.hlsl" -Fo "$out/sm6-cs_msaa_unused.dxil"
