#!/bin/sh
# Rebuilds the frozen DXIL fixtures of the conservative-raster suite from conservative-raster.hlsl with DXC 1.9
# under Wine (6.0 profiles, -HV 2021). owned-d3d12-conservative-raster.exe reads conservative-raster-<entry>.dxil
# beside itself.
# usage: build-conservative-raster.sh <wine> <dxc.exe> <WINEPREFIX> <out-dir> [conservative-distance] [fxc.exe] [evidence-dir]
set -u
wine=$1 dxc=$2 prefix=$3 out=$4
source=${5:-conservative-raster}
evidence=${7:-${prefix%/*}}
here=$(cd "$(dirname "$0")" && pwd)
status=0
entries="vs_main ps_main gs_main hs_main ds_main ms_main ms_as as_main"
if [ "$source" = conservative-raster ]; then entries="$entries ps_inner ps_inner_early ps_inner_late ps_helper cs_stencil"; fi
for entry in $entries; do
  profile=${entry%%_*}_6_0
  defines=
  case "$entry" in ms_*|as_*) profile=${entry%%_*}_6_5; defines=-DCONSERVATIVE_MESH=1;; esac
  case "$entry" in cs_stencil) defines=-DCONSERVATIVE_STENCIL_READER=1;; esac
  if ! WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 $defines -T "$profile" -E "$entry" \
      "$here/$source.hlsl" -Fo "$out/$source-$entry.dxil" >/dev/null; then
    echo "failed: $entry" >&2
    status=1
  fi
done
if [ "$source" = conservative-raster ]; then
  # A real producer must reject the mutually exclusive inputs, not a fabricated container.
  if WINEPREFIX=$prefix WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -DCONSERVATIVE_INVALID_INPUT=1 \
      -T ps_6_0 -E ps_invalid "$here/$source.hlsl" -Fo "$evidence/$source-ps_invalid.dxil" \
      >"$evidence/$source-invalid-dxil.log" 2>&1; then
    echo "invalid Coverage + InnerCoverage accepted by DXC" >&2; status=1
  fi
  if [ -n "${6:-}" ]; then
    for entry in ps_main ps_inner ps_inner_early ps_inner_late ps_helper; do
      if ! WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES=d3dcompiler_47=n "$wine" "$6" /nologo \
          /T ps_5_1 /E "$entry" /Fo "Z:$out/$source-$entry.dxbc" "Z:$here/$source.hlsl"; then
        echo "failed FXC: $entry" >&2; status=1
      fi
    done
    if WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES=d3dcompiler_47=n "$wine" "$6" /nologo \
        /D CONSERVATIVE_INVALID_INPUT=1 /T ps_5_1 /E ps_invalid /Fo "Z:$evidence/$source-ps_invalid.dxbc" \
        "Z:$here/$source.hlsl" >"$evidence/$source-invalid-dxbc.log" 2>&1; then
      echo "invalid Coverage + InnerCoverage accepted by FXC" >&2; status=1
    fi
  else
    echo "missing native FXC ps_5_1 producer (DXIL outputs produced; genuine DXBC proof unavailable)" >&2
    status=2
  fi
fi
exit $status
