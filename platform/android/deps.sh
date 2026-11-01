#!/usr/bin/env bash
# Downloads and builds the third-party libraries libpdf-tools.so links, for arm64-v8a with the NDK, from the
# sources pinned in platform/versions.sh: MuPDF (with Tesseract), PCRE2 and OpenSSL, plus the raylib and raygui
# sources (thirdparty/ in the repo; build.sh compiles raylib for Android) and Tesseract's English data. Everything
# else goes under $PT_ANDROID_OUT (default ~/.cache/pdf-tools-native/android); what is already there is reused.
# build.sh sources this file first and uses the paths it sets; run it alone (platform/android/deps.sh) to only build
# the libraries, as CI does before caching them.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/../versions.sh"
ROOT=${PT_ANDROID_OUT:-$HOME/.cache/pdf-tools-native/android}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/28.2.13676358}
# Android 11: WindowInsets types and the NDK font matcher
MIN_API=30
ABI=arm64-v8a
JOBS=$(nproc)

MUPDF_SRC=$ROOT/src/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$ROOT/mupdf-build-$BUILD_TAG-$FONTS_TAG
PCRE2_SRC=$ROOT/src/pcre2-$PCRE2_VERSION
PCRE2_OUT=$ROOT/pcre2-install-$BUILD_TAG
OPENSSL_SRC=$ROOT/src/openssl-$OPENSSL_VERSION
OPENSSL_OUT=$ROOT/openssl-install-$BUILD_TAG
TESSDATA=$ROOT/dl/eng.traineddata

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -e "$TC/clang" ] || { echo "missing: $TC/clang" >&2; exit 1; }
for tool in cmake ninja make curl git; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT/dl" "$ROOT/src"

step "MuPDF $MUPDF_VERSION"
fetch "$MUPDF_URL" "$ROOT/dl/mupdf-$MUPDF_VERSION-source.tar.gz" "$MUPDF_SHA256"
[ -d "$MUPDF_SRC" ] || tar xzf "$ROOT/dl/mupdf-$MUPDF_VERSION-source.tar.gz" -C "$ROOT/src"
# MuPDF's Makerules probes the host with pkg-config; nothing from the host may leak into the Android build.
mkdir -p "$ROOT/nopkgconfig"
printf '#!/bin/sh\nexit 1\n' > "$ROOT/nopkgconfig/pkg-config"
chmod +x "$ROOT/nopkgconfig/pkg-config"
PATH="$ROOT/nopkgconfig:$PATH" make -C "$MUPDF_SRC" -j"$JOBS" libs \
    OS=android build=release tesseract=yes OUT="$MUPDF_OUT" \
    CC="$TC/aarch64-linux-android$MIN_API-clang" CXX="$TC/aarch64-linux-android$MIN_API-clang++" \
    AR="$TC/llvm-ar" RANLIB="$TC/llvm-ranlib" \
    HAVE_X11=no HAVE_GLUT=no HAVE_LIBCRYPTO=no HAVE_OBJCOPY=no USE_SYSTEM_LIBS=no \
    XCFLAGS="-fPIC -Wno-unused-but-set-variable $SIZE_CFLAGS $MUPDF_FONT_CFLAGS" >"$ROOT/mupdf-build.log" 2>&1 \
    || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
ls -l "$MUPDF_OUT/libmupdf.a" "$MUPDF_OUT/libmupdf-third.a"

step "PCRE2 $PCRE2_VERSION"
fetch "$PCRE2_URL" "$ROOT/dl/pcre2-$PCRE2_VERSION.tar.bz2" "$PCRE2_SHA256"
[ -d "$PCRE2_SRC" ] || tar xjf "$ROOT/dl/pcre2-$PCRE2_VERSION.tar.bz2" -C "$ROOT/src"
if [ ! -f "$PCRE2_OUT/lib/libpcre2-8.a" ]; then
    cmake -S "$PCRE2_SRC" -B "$ROOT/pcre2-build-$BUILD_TAG" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$MIN_API -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" \
        -DCMAKE_INSTALL_PREFIX="$PCRE2_OUT" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF \
        >"$ROOT/pcre2-build.log" 2>&1 || { tail -40 "$ROOT/pcre2-build.log" >&2; exit 1; }
    cmake --build "$ROOT/pcre2-build-$BUILD_TAG" --target install -j"$JOBS" >>"$ROOT/pcre2-build.log" 2>&1 \
        || { tail -40 "$ROOT/pcre2-build.log" >&2; exit 1; }
fi
ls -l "$PCRE2_OUT/lib/libpcre2-8.a"

step "OpenSSL $OPENSSL_VERSION (libcrypto, certificate signatures)"
fetch "$OPENSSL_URL" "$ROOT/dl/openssl-$OPENSSL_VERSION.tar.gz" "$OPENSSL_SHA256"
[ -d "$OPENSSL_SRC" ] || tar xzf "$ROOT/dl/openssl-$OPENSSL_VERSION.tar.gz" -C "$ROOT/src"
if [ ! -f "$OPENSSL_OUT/lib/libcrypto.a" ]; then
    (cd "$OPENSSL_SRC" && make distclean >/dev/null 2>&1; PATH="$TC:$PATH" ANDROID_NDK_ROOT="$NDK" ./Configure \
        android-arm64 -D__ANDROID_API__=$MIN_API no-shared no-tests no-apps no-docs --prefix="$OPENSSL_OUT" --libdir=lib \
        $SIZE_CFLAGS \
        && PATH="$TC:$PATH" ANDROID_NDK_ROOT="$NDK" make -j"$JOBS" build_libs && make install_dev) \
        >"$ROOT/openssl-build.log" 2>&1 || { tail -40 "$ROOT/openssl-build.log" >&2; exit 1; }
fi
ls -l "$OPENSSL_OUT/lib/libcrypto.a"

step "Tesseract English data (packed into the APK as an asset)"
fetch "$TESSDATA_URL" "$TESSDATA" "$TESSDATA_SHA256"

step "raylib $RAYLIB_TAG, raygui $RAYGUI_TAG sources"
[ -d "$HERE/../../thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$HERE/../../thirdparty/raylib"
[ -d "$HERE/../../thirdparty/raygui" ] || git clone --depth 1 --branch "$RAYGUI_TAG" "$RAYGUI_URL" "$HERE/../../thirdparty/raygui"
