#!/usr/bin/env bash
# Native macOS compiler, cross-compiling for the PlayStation (not a Linux binary).
set -euo pipefail

if [[ $(uname -s) != Darwin ]]; then
    echo 'This script builds a native macOS toolchain.' >&2
    exit 1
fi
SDK_SOURCE=$(cd "$(dirname "$0")/.." && pwd)
BUILD_ROOT=${BUILD_ROOT:-"$SDK_SOURCE/build/toolchain-macos"}
PREFIX=${PREFIX:-"$BUILD_ROOT/install"}
GCC_VERSION=${GCC_VERSION:-16.1.0}
BINUTILS_VERSION=${BINUTILS_VERSION:-2.46.0}
GCC_TARGET=${GCC_TARGET:-mipsel-none-elf}
JOBS=${JOBS:-4}
MAKE=${MAKE:-gmake}
for tool in brew curl tar xcrun "$MAKE"; do
    command -v "$tool" >/dev/null || { echo "Missing prerequisite: $tool" >&2; exit 1; }
done
for package in gmp mpfr libmpc; do
    test -d "$(brew --prefix "$package")/include" || {
        echo "Install prerequisites: brew install make gmp mpfr libmpc" >&2
        exit 1
    }
done
mkdir -p "$BUILD_ROOT" "$PREFIX"
BUILD_ROOT=$(cd "$BUILD_ROOT" && pwd)
PREFIX=$(cd "$PREFIX" && pwd)
export PATH="$PREFIX/bin:$PATH"
export CC=${CC:-$(xcrun --find clang)}
export CXX=${CXX:-$(xcrun --find clang++)}
export SDKROOT=${SDKROOT:-$(xcrun --show-sdk-path)}
# Keep older configure probes compatible with recent Apple Clang defaults.
export CFLAGS=${CFLAGS:-'-O2 -std=gnu17'}
# Let each GCC subproject choose its C++ dialect (libcody checks for C++11).
export CXXFLAGS=${CXXFLAGS:-'-O2'}
mkdir -p "$BUILD_ROOT/sources" "$BUILD_ROOT/binutils-$BINUTILS_VERSION-build" "$BUILD_ROOT/gcc-$GCC_VERSION-build"

fetch_source() {
    local package=$1 url=$2
    if [[ ! -f "$BUILD_ROOT/sources/$package.tar.xz" ]]; then
        curl --fail --location --retry 3 "$url" -o "$BUILD_ROOT/sources/$package.tar.xz.part"
        mv "$BUILD_ROOT/sources/$package.tar.xz.part" "$BUILD_ROOT/sources/$package.tar.xz"
    fi
    if [[ ! -d "$BUILD_ROOT/sources/$package" ]]; then
        tar -xf "$BUILD_ROOT/sources/$package.tar.xz" -C "$BUILD_ROOT/sources"
    fi
}
fetch_source "binutils-$BINUTILS_VERSION" "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz"
fetch_source "gcc-$GCC_VERSION" "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz"

cd "$BUILD_ROOT/binutils-$BINUTILS_VERSION-build"
"$BUILD_ROOT/sources/binutils-$BINUTILS_VERSION/configure" \
        --prefix="$PREFIX" --target="$GCC_TARGET" \
        --disable-nls --disable-werror --disable-gprofng --with-float=soft \
        --with-system-zlib
"$MAKE" -j "$JOBS" MAKEINFO=true
"$MAKE" install-strip MAKEINFO=true

cd "$BUILD_ROOT/gcc-$GCC_VERSION-build"
"$BUILD_ROOT/sources/gcc-$GCC_VERSION/configure" \
        --prefix="$PREFIX" --target="$GCC_TARGET" \
        --with-gmp="$(brew --prefix gmp)" --with-mpfr="$(brew --prefix mpfr)" \
        --with-mpc="$(brew --prefix libmpc)" \
        --disable-nls --disable-werror --disable-libada --disable-libssp \
        --disable-libquadmath --disable-threads --disable-libgomp \
        --disable-libstdcxx-pch --disable-hosted-libstdcxx --disable-multilib \
        --enable-languages=c,c++ --without-isl --without-headers \
        --with-arch=mips1 --with-abi=32 --with-float=soft --with-gnu-as --with-gnu-ld \
        --with-system-zlib
"$MAKE" -j "$JOBS" MAKEINFO=true
"$MAKE" install-strip MAKEINFO=true
"$PREFIX/bin/$GCC_TARGET-gcc" --version
"$PREFIX/bin/$GCC_TARGET-g++" -std=c++23 -march=r3000 -mabi=32 -msoft-float \
    -mno-abicalls -fno-pic -G0 -O2 -ffreestanding -fno-exceptions -fno-rtti \
    -c "$SDK_SOURCE/tools/toolchain-smoke.cpp" -o "$BUILD_ROOT/smoke.o"
"$PREFIX/bin/$GCC_TARGET-gcc" -march=r3000 -mabi=32 -msoft-float \
    -mno-abicalls -fno-pic -G0 -nostdlib -Wl,-e,toolchainSmoke \
    "$BUILD_ROOT/smoke.o" -lgcc -o "$BUILD_ROOT/smoke.elf"
"$PREFIX/bin/$GCC_TARGET-objdump" -f "$BUILD_ROOT/smoke.elf"
echo "Toolchain prefix: $PREFIX"
echo "SDK configuration: cmake --preset default -DPSN00BSDK_TC=$PREFIX"
