#!/usr/bin/env bash
# Builds pdf-tools for Linux and packs it as an AppImage, for this machine's architecture (x86_64 or aarch64).
# MuPDF (with Tesseract), PCRE2, OpenSSL and raylib (GLFW with both Wayland and X11) are built from pinned sources
# and linked statically; fontconfig, OpenGL and the window system's libraries come from the system at run time.
# Needs a C/C++ compiler, make, CMake, curl, git and the X11/Wayland development headers (see README.md).
# Output: $PT_LINUX_OUT/out (default ~/.cache/pdf-tools/linux). Re-runs are incremental.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
ROOT=${PT_LINUX_OUT:-$HOME/.cache/pdf-tools/linux}
JOBS=$(nproc)
ARCH=$(uname -m)

MUPDF_VERSION=1.28.5
MUPDF_SHA256=98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934
PCRE2_VERSION=10.45
PCRE2_SHA256=21547f3516120c75597e5b30a992e27a592a31950b5140e7b8bfde3f192033c4
OPENSSL_VERSION=3.5.4
OPENSSL_SHA256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
RAYLIB_TAG=6.2
RAYGUI_TAG=5.0
# Tesseract's English data, the small "fast" model (tessdata_fast 4.1.0)
TESSDATA_SHA256=7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2
APPIMAGETOOL_VERSION=1.9.1
case "$ARCH" in
x86_64) APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 ;;
aarch64) APPIMAGETOOL_SHA256=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158 ;;
*) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

for tool in cc c++ make cmake curl git sha256sum; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
fetch() {   # url file sha256
    [ -f "$2" ] || { curl -fL -o "$2.part" "$1" && mv "$2.part" "$2"; }
    echo "$3  $2" | sha256sum -c --quiet
}
mkdir -p "$ROOT/dl" "$ROOT/src" "$ROOT/out"

step "MuPDF $MUPDF_VERSION"
MUPDF_SRC=$ROOT/src/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$ROOT/mupdf-build-tesseract
fetch "https://mupdf.com/downloads/archive/mupdf-$MUPDF_VERSION-source.tar.gz" "$ROOT/dl/mupdf.tar.gz" "$MUPDF_SHA256"
[ -d "$MUPDF_SRC" ] || tar xzf "$ROOT/dl/mupdf.tar.gz" -C "$ROOT/src"
make -C "$MUPDF_SRC" -j"$JOBS" libs build=release tesseract=yes OUT="$MUPDF_OUT" HAVE_X11=no HAVE_GLUT=no \
    HAVE_LIBCRYPTO=no USE_SYSTEM_LIBS=no XCFLAGS="-fPIC" >"$ROOT/mupdf-build.log" 2>&1 || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
ls -l "$MUPDF_OUT/libmupdf.a" "$MUPDF_OUT/libmupdf-third.a"

step "PCRE2 $PCRE2_VERSION"
PCRE2_SRC=$ROOT/src/pcre2-$PCRE2_VERSION
PCRE2_OUT=$ROOT/pcre2-install
fetch "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$PCRE2_VERSION/pcre2-$PCRE2_VERSION.tar.bz2" \
    "$ROOT/dl/pcre2.tar.bz2" "$PCRE2_SHA256"
[ -d "$PCRE2_SRC" ] || tar xjf "$ROOT/dl/pcre2.tar.bz2" -C "$ROOT/src"
if [ ! -f "$PCRE2_OUT/lib/libpcre2-8.a" ]; then
    cmake -S "$PCRE2_SRC" -B "$ROOT/pcre2-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PCRE2_OUT" \
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF >/dev/null
    cmake --build "$ROOT/pcre2-build" --target install -j"$JOBS" >/dev/null
fi

step "OpenSSL $OPENSSL_VERSION (libcrypto, certificate signatures)"
OPENSSL_SRC=$ROOT/src/openssl-$OPENSSL_VERSION
OPENSSL_OUT=$ROOT/openssl-install
fetch "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz" \
    "$ROOT/dl/openssl.tar.gz" "$OPENSSL_SHA256"
[ -d "$OPENSSL_SRC" ] || tar xzf "$ROOT/dl/openssl.tar.gz" -C "$ROOT/src"
if [ ! -f "$OPENSSL_OUT/lib/libcrypto.a" ]; then
    (cd "$OPENSSL_SRC" && ./Configure "linux-$ARCH" no-shared no-tests no-apps no-docs --prefix="$OPENSSL_OUT" --libdir=lib \
        && make -j"$JOBS" build_libs && make install_dev) >"$ROOT/openssl-build.log" 2>&1 \
        || { tail -40 "$ROOT/openssl-build.log" >&2; exit 1; }
fi

step "Tesseract English data"
fetch "https://github.com/tesseract-ocr/tessdata_fast/raw/4.1.0/eng.traineddata" "$ROOT/dl/eng.traineddata" "$TESSDATA_SHA256"

step "raylib $RAYLIB_TAG (Wayland and X11), raygui $RAYGUI_TAG"
[ -d "$REPO/thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" https://github.com/raysan5/raylib.git "$REPO/thirdparty/raylib"
[ -d "$REPO/thirdparty/raygui" ] || git clone --depth 1 --branch "$RAYGUI_TAG" https://github.com/raysan5/raygui.git "$REPO/thirdparty/raygui"
RAYLIB_OUT=$ROOT/raylib-install
if [ ! -f "$RAYLIB_OUT/lib/libraylib.a" ]; then
    cmake -S "$REPO/thirdparty/raylib" -B "$ROOT/raylib-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
        -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON -DCMAKE_INSTALL_PREFIX="$RAYLIB_OUT" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$ROOT/raylib-build" --target install -j"$JOBS" >/dev/null
fi

step "PDF Tools"
cmake -S "$REPO" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release \
    -DMUPDF_INCLUDE_DIR="$MUPDF_SRC/include" -DMUPDF_LIBRARY="$MUPDF_OUT/libmupdf.a" \
    -DMUPDF_THIRD_LIBRARY="$MUPDF_OUT/libmupdf-third.a" \
    -DPCRE2_INCLUDE_DIR="$PCRE2_OUT/include" -DPCRE2_LIBRARY="$PCRE2_OUT/lib/libpcre2-8.a" \
    -DOPENSSL_ROOT_DIR="$OPENSSL_OUT" -DOPENSSL_USE_STATIC_LIBS=ON \
    -Draylib_DIR="$RAYLIB_OUT/lib/cmake/raylib" >/dev/null
cmake --build "$ROOT/build" -j"$JOBS"
ctest --test-dir "$ROOT/build" --output-on-failure

step "PDF_Tools-$ARCH.AppImage"
APPDIR=$ROOT/AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/pdf-tools/tessdata" "$APPDIR/usr/share/applications" \
    "$APPDIR/usr/share/icons/hicolor/scalable/apps"
cp "$ROOT/build/pdf-tools" "$APPDIR/usr/bin/pdf-tools"
strip "$APPDIR/usr/bin/pdf-tools"
cp "$ROOT/dl/eng.traineddata" "$APPDIR/usr/share/pdf-tools/tessdata/"
cp "$REPO/assets/pdf-tools.desktop" "$APPDIR/usr/share/applications/pdf-tools.desktop"
cp "$REPO/assets/pdf-tools.desktop" "$APPDIR/pdf-tools.desktop"
cp "$REPO/assets/logo.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/pdf-tools.svg"
cp "$REPO/assets/logo.svg" "$APPDIR/pdf-tools.svg"
ln -s usr/bin/pdf-tools "$APPDIR/AppRun"
fetch "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION/appimagetool-$ARCH.AppImage" \
    "$ROOT/dl/appimagetool.AppImage" "$APPIMAGETOOL_SHA256"
chmod +x "$ROOT/dl/appimagetool.AppImage"
# extract-and-run: no FUSE needed (containers, CI)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=$ARCH "$ROOT/dl/appimagetool.AppImage" --no-appstream "$APPDIR" \
    "$ROOT/out/PDF_Tools-$ARCH.AppImage" >"$ROOT/appimage.log" 2>&1 || { tail -20 "$ROOT/appimage.log" >&2; exit 1; }
ls -l "$ROOT/out/PDF_Tools-$ARCH.AppImage"
