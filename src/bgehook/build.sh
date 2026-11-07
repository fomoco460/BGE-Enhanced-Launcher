#!/bin/sh
# Builds the 32-bit d3d9.dll proxy. Needs an i686 MinGW-w64 toolchain (llvm-mingw or GCC),
# e.g. https://github.com/mstorsjo/llvm-mingw - put its bin folder on PATH, or set CC.
set -e
CC=${CC:-i686-w64-mingw32-gcc}
cd "$(dirname "$0")"
mkdir -p ../../build
$CC -O2 -shared -static -Wall -o ../../build/d3d9.dll bgehook.c bgehook.def -lwinmm -Wl,--enable-stdcall-fixup
echo "built build/d3d9.dll"
