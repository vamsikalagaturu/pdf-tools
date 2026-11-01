#!/usr/bin/env bash
# Downloads and builds the third-party libraries pdf-tools links on Linux, from the sources pinned in
# platform/versions.sh: MuPDF (with Tesseract), PCRE2, OpenSSL and raylib (GLFW with both Wayland and X11), plus
# raygui and Tesseract's English data. Everything goes under $PT_LINUX_OUT (default ~/.cache/pdf-tools/linux),
# except the raylib and raygui sources (thirdparty/ in the repo); what is already there is reused.
# build.sh sources this file first and uses the paths it sets; run it alone (platform/linux/deps.sh) to only build
# the libraries, as CI does before caching them.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${PT_LINUX_OUT:-$HOME/.cache/pdf-tools/linux}
JOBS=$(nproc)
ARCH=$(uname -m)
case "$ARCH" in
x86_64 | aarch64) ;;
*) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

for tool in cc c++ make cmake pkg-config curl git sha256sum; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT/dl" "$ROOT/src" "$ROOT/pkgconfig"

step "MuPDF $MUPDF_VERSION"
MUPDF_SRC=$ROOT/src/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$ROOT/mupdf-build-$BUILD_TAG-$FONTS_TAG
fetch "$MUPDF_URL" "$ROOT/dl/mupdf.tar.gz" "$MUPDF_SHA256"
[ -d "$MUPDF_SRC" ] || tar xzf "$ROOT/dl/mupdf.tar.gz" -C "$ROOT/src"
make -C "$MUPDF_SRC" -j"$JOBS" libs build=release tesseract=yes OUT="$MUPDF_OUT" HAVE_X11=no HAVE_GLUT=no \
    HAVE_LIBCRYPTO=no USE_SYSTEM_LIBS=no XCFLAGS="-fPIC $SIZE_CFLAGS $MUPDF_FONT_CFLAGS" \
    >"$ROOT/mupdf-build.log" 2>&1 || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
# MuPDF's makefile writes no pkg-config file for a build tree; its third-party libraries (HarfBuzz, Tesseract)
# are C++
cat >"$ROOT/pkgconfig/mupdf.pc" <<EOF
Name: mupdf
Description: MuPDF $MUPDF_VERSION, static, with Tesseract
Version: $MUPDF_VERSION
Cflags: -I$MUPDF_SRC/include
Libs: -L$MUPDF_OUT -lmupdf -lmupdf-third -lstdc++ -lpthread -lm
EOF

step "PCRE2 $PCRE2_VERSION"
PCRE2_SRC=$ROOT/src/pcre2-$PCRE2_VERSION
PCRE2_OUT=$ROOT/pcre2-install-$BUILD_TAG
fetch "$PCRE2_URL" "$ROOT/dl/pcre2.tar.bz2" "$PCRE2_SHA256"
[ -d "$PCRE2_SRC" ] || tar xjf "$ROOT/dl/pcre2.tar.bz2" -C "$ROOT/src"
if [ ! -f "$PCRE2_OUT/lib/libpcre2-8.a" ]; then
    cmake -S "$PCRE2_SRC" -B "$ROOT/pcre2-build-$BUILD_TAG" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PCRE2_OUT" \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" \
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF >/dev/null
    cmake --build "$ROOT/pcre2-build-$BUILD_TAG" --target install -j"$JOBS" >/dev/null
fi

step "OpenSSL $OPENSSL_VERSION (libcrypto, certificate signatures)"
OPENSSL_SRC=$ROOT/src/openssl-$OPENSSL_VERSION
OPENSSL_OUT=$ROOT/openssl-install-$BUILD_TAG
fetch "$OPENSSL_URL" "$ROOT/dl/openssl.tar.gz" "$OPENSSL_SHA256"
[ -d "$OPENSSL_SRC" ] || tar xzf "$ROOT/dl/openssl.tar.gz" -C "$ROOT/src"
if [ ! -f "$OPENSSL_OUT/lib/libcrypto.a" ]; then
    (cd "$OPENSSL_SRC" && make distclean >/dev/null 2>&1; ./Configure "linux-$ARCH" no-shared no-tests no-apps no-docs \
        --prefix="$OPENSSL_OUT" --libdir=lib $SIZE_CFLAGS \
        && make -j"$JOBS" build_libs && make install_dev) >"$ROOT/openssl-build.log" 2>&1 \
        || { tail -40 "$ROOT/openssl-build.log" >&2; exit 1; }
fi

step "Tesseract English data"
fetch "$TESSDATA_URL" "$ROOT/dl/eng.traineddata" "$TESSDATA_SHA256"

step "raylib $RAYLIB_TAG (Wayland and X11), raygui $RAYGUI_TAG"
[ -d "$REPO/thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$REPO/thirdparty/raylib"
[ -d "$REPO/thirdparty/raygui" ] || git clone --depth 1 --branch "$RAYGUI_TAG" "$RAYGUI_URL" "$REPO/thirdparty/raygui"
RAYLIB_OUT=$ROOT/raylib-install-$BUILD_TAG
if [ ! -f "$RAYLIB_OUT/lib/libraylib.a" ]; then
    cmake -S "$REPO/thirdparty/raylib" -B "$ROOT/raylib-build-$BUILD_TAG" -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON \
        -DCMAKE_INSTALL_PREFIX="$RAYLIB_OUT" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$ROOT/raylib-build-$BUILD_TAG" --target install -j"$JOBS" >/dev/null
fi
