#!/usr/bin/env bash
# Build Intel ISA-L (isa-l.dll) and isal_zip.exe with MinGW-w64 + NASM + CMake.
# Intended for MSYS2 / MinGW on Windows. For a native Windows shell use build.ps1.
#
#   ./build.sh                 # clones ISA-L v2.32.1 into .build/ and builds
#   ISAL_SRC=/path/to/isa-l ./build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$ROOT/.build}"
ISAL_SRC="${ISAL_SRC:-}"
ISAL_REF="${ISAL_REF:-v2.32.1}"
CMAKE="${CMAKE:-cmake}"

mkdir -p "$WORK"
if [ -z "$ISAL_SRC" ]; then
    ISAL_SRC="$WORK/isa-l"
    if [ ! -d "$ISAL_SRC/igzip" ]; then
        echo "Cloning Intel ISA-L $ISAL_REF ..."
        git clone --depth 1 --branch "$ISAL_REF" https://github.com/intel/isa-l.git "$ISAL_SRC"
    fi
fi

BLD="$WORK/build"
rm -rf "$BLD"; mkdir -p "$BLD"

echo "Configuring ISA-L ..."
"$CMAKE" -G "MinGW Makefiles" -S "$ISAL_SRC" -B "$BLD" \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_ASM_NASM_COMPILER=nasm \
    -DISAL_BUILD_TESTS=OFF -DISAL_BUILD_PERF_TESTS=OFF -DISAL_BUILD_ISAL_SHIM=OFF

echo "Building isa-l.dll ..."
"$CMAKE" --build "$BLD" -j

echo "Building isal_zip.exe ..."
gcc -O2 "$ROOT/src/isal_zip.c" "$BLD/isa-l.dll.a" \
    -I "$BLD/build_include" -I "$ISAL_SRC/include" -o "$ROOT/isal_zip.exe"

cp "$BLD/isa-l.dll" "$ROOT/"
echo "OK -> $ROOT/isal_zip.exe  +  $ROOT/isa-l.dll"
