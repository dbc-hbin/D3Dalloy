#!/bin/sh
# Genuine stable DXC/validator inputs, not hand-edited DXIL.
# usage: build-atomic64-typed.sh <wine> <dxc.exe> <dedicated-prefix> <out-dir>
set -eu
wine=$1 dxc=$2 prefix=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
compile() {
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_6 \
    -D SHAPE="$1" -D SIGNED="$2" -E "$3" "$here/atomic64-typed.hlsl" \
    -Fo "$out/atomic64-typed-$1-$2-$3.dxil" \
    -Fc "$out/atomic64-typed-$1-$2-$3.ll"
}
for shape in 0 1 2 3 4 5; do
  for signed in 0 1; do
    compile "$shape" "$signed" operations
    compile "$shape" "$signed" contention
    compile "$shape" "$signed" snapshot
  done
done
compile 0 0 aliases
for kind in 0 1 2; do
  WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_6 \
    -D KIND="$kind" -E formats "$here/atomic64-typed-format.hlsl" \
    -Fo "$out/atomic64-typed-format-$kind.dxil"
done
# Preserve exact signed-zero/NaN payloads through float bitwise32 aliases.
WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_6 \
  -E float_atomics "$here/atomic64-typed-float.hlsl" \
  -Fo "$out/atomic64-typed-float.dxil"
# Official DXIL.rst AtomicBinOp/AtomicCompareExchange valid-resource tables
# exclude RWTexture2DMS{Array}; do not invent a sample operand. DXC1.9 currently
# fails with llvm::cast incompatible rather than an intentional diagnostic.
for array in 0 1; do
  if WINEPREFIX="$prefix" WINEDEBUG=-all "$wine" "$dxc" -nologo -HV 2021 -T cs_6_7 \
      -D ARRAY="$array" -E atomic_sample "$here/atomic64-typed-msaa.hlsl" \
      -Fo "$out/atomic64-typed-msaa-$array.dxil" >"$out/atomic64-typed-msaa-$array.log" 2>&1; then
    echo "Unexpected producer acceptance of WritableMSAA Typed64 array=$array" >&2
    exit 1
  fi
done
