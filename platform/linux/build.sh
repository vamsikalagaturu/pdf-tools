#!/usr/bin/env bash
# Builds pdf-tools for Linux and packs it as an AppImage, for this machine's architecture (x86_64 or aarch64).
# MuPDF (with Tesseract), PCRE2, OpenSSL and raylib (GLFW with both Wayland and X11) are built from the sources
# pinned in platform/versions.sh and linked statically; CMake finds them through pkg-config. fontconfig, OpenGL and
# the window system's libraries come from the system at run time.
# Needs a C/C++ compiler, make, CMake, pkg-config, curl, git and the X11/Wayland development headers (README.md).
# Output: $PT_LINUX_OUT/out (default ~/.cache/pdf-tools/linux). Re-runs are incremental.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${PT_LINUX_OUT:-$HOME/.cache/pdf-tools/linux}
JOBS=$(nproc)
ARCH=$(uname -m)
case "$ARCH" in
x86_64) APPIMAGETOOL_SHA256=$APPIMAGETOOL_SHA256_x86_64 ;;
aarch64) APPIMAGETOOL_SHA256=$APPIMAGETOOL_SHA256_aarch64 ;;
*) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

for tool in cc c++ make cmake pkg-config curl git sha256sum; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT/dl" "$ROOT/src" "$ROOT/out" "$ROOT/pkgconfig"

step "MuPDF $MUPDF_VERSION"
MUPDF_SRC=$ROOT/src/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$ROOT/mupdf-build-tesseract
fetch "$MUPDF_URL" "$ROOT/dl/mupdf.tar.gz" "$MUPDF_SHA256"
[ -d "$MUPDF_SRC" ] || tar xzf "$ROOT/dl/mupdf.tar.gz" -C "$ROOT/src"
make -C "$MUPDF_SRC" -j"$JOBS" libs build=release tesseract=yes OUT="$MUPDF_OUT" HAVE_X11=no HAVE_GLUT=no \
    HAVE_LIBCRYPTO=no USE_SYSTEM_LIBS=no XCFLAGS="-fPIC" >"$ROOT/mupdf-build.log" 2>&1 || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
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
PCRE2_OUT=$ROOT/pcre2-install
fetch "$PCRE2_URL" "$ROOT/dl/pcre2.tar.bz2" "$PCRE2_SHA256"
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
fetch "$OPENSSL_URL" "$ROOT/dl/openssl.tar.gz" "$OPENSSL_SHA256"
[ -d "$OPENSSL_SRC" ] || tar xzf "$ROOT/dl/openssl.tar.gz" -C "$ROOT/src"
if [ ! -f "$OPENSSL_OUT/lib/libcrypto.a" ]; then
    (cd "$OPENSSL_SRC" && ./Configure "linux-$ARCH" no-shared no-tests no-apps no-docs --prefix="$OPENSSL_OUT" --libdir=lib \
        && make -j"$JOBS" build_libs && make install_dev) >"$ROOT/openssl-build.log" 2>&1 \
        || { tail -40 "$ROOT/openssl-build.log" >&2; exit 1; }
fi

step "Tesseract English data"
fetch "$TESSDATA_URL" "$ROOT/dl/eng.traineddata" "$TESSDATA_SHA256"

step "raylib $RAYLIB_TAG (Wayland and X11), raygui $RAYGUI_TAG"
[ -d "$REPO/thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$REPO/thirdparty/raylib"
[ -d "$REPO/thirdparty/raygui" ] || git clone --depth 1 --branch "$RAYGUI_TAG" "$RAYGUI_URL" "$REPO/thirdparty/raygui"
RAYLIB_OUT=$ROOT/raylib-install
if [ ! -f "$RAYLIB_OUT/lib/libraylib.a" ]; then
    cmake -S "$REPO/thirdparty/raylib" -B "$ROOT/raylib-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
        -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON -DCMAKE_INSTALL_PREFIX="$RAYLIB_OUT" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$ROOT/raylib-build" --target install -j"$JOBS" >/dev/null
fi

step "PDF Tools"
# what was built here comes first; fontconfig from the system after it
export PKG_CONFIG_PATH="$ROOT/pkgconfig:$PCRE2_OUT/lib/pkgconfig:$OPENSSL_OUT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -S "$REPO" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$RAYLIB_OUT" >/dev/null
cmake --build "$ROOT/build" -j"$JOBS"
ctest --test-dir "$ROOT/build" --output-on-failure

step "PDF_Tools-$ARCH.AppImage"
APPDIR=$ROOT/AppDir
rm -rf "$APPDIR"
DESTDIR=$APPDIR cmake --install "$ROOT/build" --prefix /usr --component app >/dev/null
strip "$APPDIR/usr/bin/pdf-tools"
mkdir -p "$APPDIR/usr/share/pdf-tools/tessdata"
cp "$ROOT/dl/eng.traineddata" "$APPDIR/usr/share/pdf-tools/tessdata/"
cp "$REPO/assets/pdf-tools.desktop" "$APPDIR/pdf-tools.desktop"
cp "$REPO/assets/logo.svg" "$APPDIR/pdf-tools.svg"
ln -s usr/bin/pdf-tools "$APPDIR/AppRun"
fetch "$APPIMAGETOOL_URL/appimagetool-$ARCH.AppImage" "$ROOT/dl/appimagetool.AppImage" "$APPIMAGETOOL_SHA256"
chmod +x "$ROOT/dl/appimagetool.AppImage"
# extract-and-run: no FUSE needed (containers, CI)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=$ARCH "$ROOT/dl/appimagetool.AppImage" --no-appstream "$APPDIR" \
    "$ROOT/out/PDF_Tools-$ARCH.AppImage" >"$ROOT/appimage.log" 2>&1 || { tail -20 "$ROOT/appimage.log" >&2; exit 1; }
ls -l "$ROOT/out/PDF_Tools-$ARCH.AppImage"
