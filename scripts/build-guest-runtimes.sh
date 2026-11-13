#!/bin/sh
# Cross-builds LLVM libunwind/libc++abi/libc++ as arm64 macOS Mach-O dylibs,
# i.e. as *guest* code that machoman translates like any other dylib the
# application ships. This keeps the C++ ABI (this-returning constructors,
# sret, vtables in both directions, exception unwinding) entirely on the
# guest side; only libSystem's C ABI crosses into host code.
# Needs a macOS SDK unpacked at third_party/sdk/MacOSX*.sdk (or $MACOSX_SDK)
# and clang + ld64.lld.
# Output: third_party/guest-runtimes/install/lib/libc++.1.dylib, libc++abi.dylib, libunwind.1.dylib
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/llvm-project"
ARCH="${MACHOMAN_GUEST_ARCH:-arm64}"
case "$ARCH" in arm64|x86_64) ;; *) echo "unsupported guest architecture: $ARCH" >&2; exit 1 ;; esac
RUNTIME_ROOT="$ROOT/third_party/guest-runtimes"
if [ "$ARCH" != arm64 ]; then RUNTIME_ROOT="$RUNTIME_ROOT/$ARCH"; fi
BUILD="$RUNTIME_ROOT/build"
INSTALL="$RUNTIME_ROOT/install"
TAG="${LLVM_TAG:-llvmorg-22.1.8}"
SDK="${MACOSX_SDK:-$(ls -d "$ROOT"/third_party/sdk/MacOSX*.sdk | head -1)}"
TARGET="${MACHOMAN_GUEST_TARGET:-$ARCH-apple-macos12}"

if [ ! -d "$SRC/libunwind" ]; then
    if [ ! -d "$SRC/libcxx" ]; then
        git clone --depth 1 --filter=blob:none --sparse --branch "$TAG" https://github.com/llvm/llvm-project.git "$SRC"
    fi
    git -C "$SRC" sparse-checkout set cmake runtimes libcxx libcxxabi libunwind llvm/cmake llvm/utils/llvm-lit libc/shared libc/src/__support libc/hdr libc/include
fi

# Local patches (each applies cleanly once; re-running the script is a no-op).
for patch in "$ROOT"/scripts/patches/*.patch; do
    [ -f "$patch" ] || continue
    if git -C "$SRC" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$SRC" apply "$patch"
        echo "applied $(basename "$patch")"
    fi
done

cmake -S "$SRC/runtimes" -B "$BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR="$ARCH" \
    -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_COMPILER_TARGET="$TARGET" -DCMAKE_CXX_COMPILER_TARGET="$TARGET" \
    -DCMAKE_ASM_COMPILER_TARGET="$TARGET" \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_NAME_DIR=/usr/lib -DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
    -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
    -DLIBCXX_USE_COMPILER_RT=OFF -DLIBCXXABI_USE_COMPILER_RT=OFF -DLIBUNWIND_USE_COMPILER_RT=OFF \
    -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_ENABLE_STATIC=OFF \
    -DLIBCXXABI_ENABLE_SHARED=ON -DLIBCXXABI_ENABLE_STATIC=OFF \
    -DLIBUNWIND_ENABLE_SHARED=ON -DLIBUNWIND_ENABLE_STATIC=OFF \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBCXX_INSTALL_MODULES=OFF
ninja -C "$BUILD" -j "${MACHOMAN_BUILD_JOBS:-4}" install-unwind install-cxxabi install-cxx
echo "guest runtimes installed to $INSTALL"
