#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# System packages are canonical. Merely installing brew must not change the build.
case "${PHOTO_OPS_DEPS:-system}" in
    system)
        cxx=${CXX:-g++}
        cc=${CC:-gcc}
        ;;
    brew)
        if ! command -v brew >/dev/null; then
            echo "PHOTO_OPS_DEPS=brew requires Homebrew" >&2
            exit 1
        fi
        brew_prefix=$(brew --prefix)
        export PKG_CONFIG_PATH="$brew_prefix/opt/libraw/lib/pkgconfig:$brew_prefix/opt/libtiff/lib/pkgconfig:$brew_prefix/opt/jpeg-turbo/lib/pkgconfig:$brew_prefix/opt/little-cms2/lib/pkgconfig:$brew_prefix/opt/openexr/lib/pkgconfig:$brew_prefix/opt/opencv/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
        cxx=${CXX:-$brew_prefix/bin/g++-16}
        cc=${CC:-$brew_prefix/bin/gcc-16}
        ;;
    *)
        echo "PHOTO_OPS_DEPS must be system or brew" >&2
        exit 1
        ;;
esac
if command -v xmake >/dev/null; then
    build_tool=$(command -v xmake)
else
    build_tool="$PWD/.tools/install/bin/xmake"
fi
exec "$build_tool" f --toolchain=gcc --cc="$cc" --cxx="$cxx" --ld="$cxx" "$@"
