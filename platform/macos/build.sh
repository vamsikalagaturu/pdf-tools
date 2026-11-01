#!/usr/bin/env bash
# Builds "PDF Tools.app" and PDF-Tools.dmg on a Mac, for that Mac's architecture (Apple Silicon or Intel), macOS 11+.
# The libraries come from deps.sh (MuPDF with Tesseract, PCRE2, OpenSSL and raylib, built from the sources pinned in
# platform/versions.sh) and are linked statically: the app needs nothing installed. Needs the Xcode command line
# tools (xcode-select --install) and CMake. Output: $PT_MACOS_OUT/out (default ~/Library/Caches/pdf-tools/macos/out).
# Re-runs are incremental. `build.sh check` builds and runs the tests without packing.
# The app is not signed with a Developer ID: open it the first time with right-click > Open.
# MuPDF's CJK and Noto fallback fonts are left out (fonts a PDF does not embed come from /System/Library/Fonts);
# PT_FONTS=full bundles them.
set -euo pipefail

MODE=${1:-}
case "$MODE" in
'' | check) ;;
*) echo "usage: $0 [check]" >&2; exit 1 ;;
esac

if [ -z "$MODE" ]; then
    for tool in hdiutil codesign iconutil sips; do
        command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
    done
fi

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/deps.sh"

step "PDF Tools"
cmake --fresh -S "$REPO" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-dead_strip" \
    -DMUPDF_INCLUDE_DIR="$MUPDF_SRC/include" -DMUPDF_LIBRARY="$MUPDF_OUT/libmupdf.a" \
    -DMUPDF_THIRD_LIBRARY="$MUPDF_OUT/libmupdf-third.a" \
    -DPCRE2_INCLUDE_DIR="$PCRE2_OUT/include" -DPCRE2_LIBRARY="$PCRE2_OUT/lib/libpcre2-8.a" \
    -DOPENSSL_ROOT_DIR="$OPENSSL_OUT" -DOPENSSL_USE_STATIC_LIBS=ON \
    -DPT_USE_FONTCONFIG=OFF -Draylib_DIR="$RAYLIB_OUT/lib/cmake/raylib" >/dev/null
cmake --build "$ROOT/build" -j"$JOBS"
ctest --test-dir "$ROOT/build" --output-on-failure
if [ "$MODE" = check ]; then exit 0; fi

step "PDF Tools.app"
mkdir -p "$ROOT/out"
APP="$ROOT/out/PDF Tools.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/build/pdf-tools" "$APP/Contents/MacOS/pdf-tools"
sed "s/@VERSION@/$PT_VERSION/g" "$HERE/Info.plist" > "$APP/Contents/Info.plist"
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
