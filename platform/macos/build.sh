#!/usr/bin/env bash
# Builds "PDF Tools.app" and PDF-Tools.dmg on a Mac, for that Mac's architecture (Apple Silicon or Intel), macOS 11+.
# MuPDF (with Tesseract), PCRE2, OpenSSL and raylib are built from the sources pinned in platform/versions.sh and
# linked statically: the app needs nothing installed. Needs the Xcode command line tools (xcode-select --install)
# and CMake. Output: $PT_MACOS_OUT/out (default ~/Library/Caches/pdf-tools/macos/out). Re-runs are incremental.
# The app is not signed with a Developer ID: open it the first time with right-click > Open.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${PT_MACOS_OUT:-$HOME/Library/Caches/pdf-tools/macos}
JOBS=$(sysctl -n hw.ncpu)
export MACOSX_DEPLOYMENT_TARGET=11.0

for tool in cc make cmake curl git shasum hdiutil codesign iconutil sips; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT/dl" "$ROOT/src" "$ROOT/out"

step "MuPDF $MUPDF_VERSION"
MUPDF_SRC=$ROOT/src/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$ROOT/mupdf-build-tesseract
fetch "$MUPDF_URL" "$ROOT/dl/mupdf.tar.gz" "$MUPDF_SHA256"
[ -d "$MUPDF_SRC" ] || tar xzf "$ROOT/dl/mupdf.tar.gz" -C "$ROOT/src"
# libs for the app; mutool (apps) only to draw the icon from logo.svg
make -C "$MUPDF_SRC" -j"$JOBS" libs apps build=release tesseract=yes OUT="$MUPDF_OUT" HAVE_X11=no HAVE_GLUT=no USE_SYSTEM_LIBS=no \
    XCFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" XLDFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
    >"$ROOT/mupdf-build.log" 2>&1 || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
ls -l "$MUPDF_OUT/libmupdf.a" "$MUPDF_OUT/libmupdf-third.a"

step "PCRE2 $PCRE2_VERSION"
PCRE2_SRC=$ROOT/src/pcre2-$PCRE2_VERSION
PCRE2_OUT=$ROOT/pcre2-install
fetch "$PCRE2_URL" "$ROOT/dl/pcre2.tar.bz2" "$PCRE2_SHA256"
[ -d "$PCRE2_SRC" ] || tar xjf "$ROOT/dl/pcre2.tar.bz2" -C "$ROOT/src"
if [ ! -f "$PCRE2_OUT/lib/libpcre2-8.a" ]; then
    cmake -S "$PCRE2_SRC" -B "$ROOT/pcre2-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PCRE2_OUT" \
        -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF >/dev/null
    cmake --build "$ROOT/pcre2-build" --target install -j"$JOBS" >/dev/null
fi

step "OpenSSL $OPENSSL_VERSION (libcrypto, certificate signatures)"
OPENSSL_SRC=$ROOT/src/openssl-$OPENSSL_VERSION
OPENSSL_OUT=$ROOT/openssl-install
fetch "$OPENSSL_URL" "$ROOT/dl/openssl.tar.gz" "$OPENSSL_SHA256"
[ -d "$OPENSSL_SRC" ] || tar xzf "$ROOT/dl/openssl.tar.gz" -C "$ROOT/src"
if [ ! -f "$OPENSSL_OUT/lib/libcrypto.a" ]; then
    (cd "$OPENSSL_SRC" && ./Configure "darwin64-$(uname -m)-cc" no-shared no-tests no-apps no-docs --prefix="$OPENSSL_OUT" \
        --libdir=lib "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" && make -j"$JOBS" build_libs && make install_dev) \
        >"$ROOT/openssl-build.log" 2>&1 || { tail -40 "$ROOT/openssl-build.log" >&2; exit 1; }
fi

step "Tesseract English data"
fetch "$TESSDATA_URL" "$ROOT/dl/eng.traineddata" "$TESSDATA_SHA256"

step "raylib $RAYLIB_TAG, raygui $RAYGUI_TAG"
[ -d "$REPO/thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$REPO/thirdparty/raylib"
[ -d "$REPO/thirdparty/raygui" ] || git clone --depth 1 --branch "$RAYGUI_TAG" "$RAYGUI_URL" "$REPO/thirdparty/raygui"
RAYLIB_OUT=$ROOT/raylib-install
if [ ! -f "$RAYLIB_OUT/lib/libraylib.a" ]; then
    cmake -S "$REPO/thirdparty/raylib" -B "$ROOT/raylib-build" -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
        -DCMAKE_INSTALL_PREFIX="$RAYLIB_OUT" >/dev/null
    cmake --build "$ROOT/raylib-build" --target install -j"$JOBS" >/dev/null
fi

step "PDF Tools"
cmake -S "$REPO" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release \
    -DMUPDF_INCLUDE_DIR="$MUPDF_SRC/include" -DMUPDF_LIBRARY="$MUPDF_OUT/libmupdf.a" \
    -DMUPDF_THIRD_LIBRARY="$MUPDF_OUT/libmupdf-third.a" \
    -DPCRE2_INCLUDE_DIR="$PCRE2_OUT/include" -DPCRE2_LIBRARY="$PCRE2_OUT/lib/libpcre2-8.a" \
    -DOPENSSL_ROOT_DIR="$OPENSSL_OUT" -DOPENSSL_USE_STATIC_LIBS=ON \
    -DPT_USE_FONTCONFIG=OFF -Draylib_DIR="$RAYLIB_OUT/lib/cmake/raylib" >/dev/null
cmake --build "$ROOT/build" -j"$JOBS"
ctest --test-dir "$ROOT/build" --output-on-failure

step "PDF Tools.app"
APP="$ROOT/out/PDF Tools.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/build/pdf-tools" "$APP/Contents/MacOS/pdf-tools"
VERSION=$(git -C "$REPO" describe --tags --always 2>/dev/null || echo 0.1.0)
sed "s/@VERSION@/$VERSION/g" "$HERE/Info.plist" > "$APP/Contents/Info.plist"
mkdir -p "$APP/Contents/Resources/tessdata"
cp "$ROOT/dl/eng.traineddata" "$APP/Contents/Resources/tessdata/"
# the icon: logo.svg drawn by mutool at 1024 px, then the iconset sizes
ICONSET=$ROOT/pdf-tools.iconset
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
if "$MUPDF_OUT/mutool" draw -q -o "$ROOT/icon-1024.png" -w 1024 -h 1024 "$REPO/assets/logo.svg" 2>/dev/null \
    || "$MUPDF_OUT/mutool" draw -o "$ROOT/icon-1024.png" -w 1024 -h 1024 "$REPO/assets/logo.svg"; then
    for s in 16 32 128 256 512; do
        sips -z $s $s "$ROOT/icon-1024.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$ROOT/icon-1024.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/pdf-tools.icns"
else
    echo "could not draw the icon; the app gets the default one"
fi
# ad-hoc signature: required for arm64 binaries; not a Developer ID
codesign --force --sign - "$APP"

step "PDF-Tools.dmg"
STAGE=$ROOT/dmg
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "PDF Tools" -srcfolder "$STAGE" -ov -format UDZO "$ROOT/out/PDF-Tools.dmg" >/dev/null
ls -l "$ROOT/out/PDF-Tools.dmg"
echo "App: $APP"
