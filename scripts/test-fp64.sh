#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out=${1:-build/local/fp64-proof}
mkdir -p "$out"
xcrun metal -x c++ -std=c++14 -O3 --target=air64-apple-macos14.0 -fno-fast-math -c src/airconv/shaders/air_fp64.cpp -o "$out/air_fp64.air"
xcrun metal -std=metal3.1 --target=air64-apple-macos14.0 -fno-fast-math -c graphics-translator/tests/fp64.metal -o "$out/fp64.air"
xcrun metallib "$out/air_fp64.air" "$out/fp64.air" -o "$out/fp64.metallib"
xcrun clang++ -std=c++20 -O2 -ffp-contract=off -fno-fast-math -fobjc-arc graphics-translator/tests/fp64-oracle.mm -framework Foundation -framework Metal -o "$out/fp64-oracle"
MTL_DEBUG_LAYER=1 "$out/fp64-oracle" "$out/fp64.metallib"
