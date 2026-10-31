#!/usr/bin/env bash
# Builds pdf-tools for Linux and packs it as an AppImage and as PDF-Tools-Linux-<arch>.tar.gz (a pdf-tools/
# folder with bin/ and share/), for this machine's architecture (x86_64 or aarch64). Both run on systems with this
# machine's glibc or newer, so release builds come from the oldest supported one (CI: ubuntu-24.04).
# The libraries come from deps.sh (MuPDF with Tesseract, PCRE2, OpenSSL and raylib with both Wayland and X11, built
# from the sources pinned in platform/versions.sh) and are linked statically; CMake finds them through pkg-config.
# fontconfig, OpenGL and the window system's libraries come from the system at run time.
# Needs a C/C++ compiler, make, CMake, pkg-config, curl, git and the X11/Wayland development headers (README.md).
# Output: $PT_LINUX_OUT/out (default ~/.cache/pdf-tools/linux). Re-runs are incremental.
# MuPDF's CJK and Noto fallback fonts are left out (fonts a PDF does not embed come from fontconfig);
# PT_FONTS=full bundles them.
# `build.sh check` builds and runs the tests without packing; `build.sh install PREFIX` installs the program with
# its man page, launcher entry, icon and OCR data into PREFIX (e.g. ~/.local) instead of packing.
set -euo pipefail

USAGE="usage: $0 [appimage | check | install PREFIX]"
MODE=${1:-appimage}
case "$MODE" in
appimage | check) ;;
install) PREFIX=${2:?$USAGE} ;;
*) echo "$USAGE" >&2; exit 1 ;;
esac

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/deps.sh"

step "PDF Tools"
# what was built here comes first; fontconfig from the system after it
export PKG_CONFIG_PATH="$ROOT/pkgconfig:$PCRE2_OUT/lib/pkgconfig:$OPENSSL_OUT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake --fresh -S "$REPO" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$RAYLIB_OUT" \
    -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections" -DPT_TESSDATA="$ROOT/dl/eng.traineddata" >/dev/null
cmake --build "$ROOT/build" -j"$JOBS"
ctest --test-dir "$ROOT/build" --output-on-failure

case "$MODE" in
check) exit 0 ;;
install)
    step "Install into $PREFIX"
    cmake --install "$ROOT/build" --prefix "$PREFIX" --component app --strip
    exit 0
    ;;
esac

step "PDF_Tools-$ARCH.AppImage"
case "$ARCH" in
x86_64) APPIMAGETOOL_SHA256=$APPIMAGETOOL_SHA256_x86_64 ;;
aarch64) APPIMAGETOOL_SHA256=$APPIMAGETOOL_SHA256_aarch64 ;;
esac
mkdir -p "$ROOT/out"
APPDIR=$ROOT/AppDir
rm -rf "$APPDIR"
DESTDIR=$APPDIR cmake --install "$ROOT/build" --prefix /usr --component app --strip >/dev/null
cp "$REPO/assets/pdf-tools.desktop" "$APPDIR/pdf-tools.desktop"
cp "$REPO/assets/logo.svg" "$APPDIR/pdf-tools.svg"
ln -s usr/bin/pdf-tools "$APPDIR/AppRun"
fetch "$APPIMAGETOOL_URL/appimagetool-$ARCH.AppImage" "$ROOT/dl/appimagetool.AppImage" "$APPIMAGETOOL_SHA256"
chmod +x "$ROOT/dl/appimagetool.AppImage"
# extract-and-run: no FUSE needed (containers, CI)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=$ARCH "$ROOT/dl/appimagetool.AppImage" --no-appstream "$APPDIR" \
    "$ROOT/out/PDF_Tools-$ARCH.AppImage" >"$ROOT/appimage.log" 2>&1 || { tail -20 "$ROOT/appimage.log" >&2; exit 1; }
ls -l "$ROOT/out/PDF_Tools-$ARCH.AppImage"

# the same files as one folder, runnable wherever it is unpacked: the program finds its OCR data in
# bin/../share/pdf-tools
TARBALL=PDF-Tools-Linux-$ARCH.tar.gz
step "$TARBALL"
STAGE=$ROOT/stage
rm -rf "$STAGE"
cmake --install "$ROOT/build" --prefix "$STAGE/pdf-tools" --component app --strip >/dev/null
tar czf "$ROOT/out/$TARBALL" -C "$STAGE" --owner=0 --group=0 pdf-tools
ls -l "$ROOT/out/$TARBALL"
