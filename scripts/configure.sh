#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if command -v brew >/dev/null; then
    brew_prefix=$(brew --prefix)
    export PKG_CONFIG_PATH="$brew_prefix/opt/libraw/lib/pkgconfig:$brew_prefix/opt/libtiff/lib/pkgconfig:$brew_prefix/opt/jpeg-turbo/lib/pkgconfig:$brew_prefix/opt/little-cms2/lib/pkgconfig:$brew_prefix/opt/openexr/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    cxx=${CXX:-$brew_prefix/bin/g++-16}
    cc=${CC:-$brew_prefix/bin/gcc-16}
else
    cxx=${CXX:-g++}
    cc=${CC:-gcc}
fi
if command -v xmake >/dev/null; then
    build_tool=$(command -v xmake)
else
    build_tool="$PWD/.tools/install/bin/xmake"
fi
exec "$build_tool" f --toolchain=gcc --cc="$cc" --cxx="$cxx" --ld="$cxx" "$@"
