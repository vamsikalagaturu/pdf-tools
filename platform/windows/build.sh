#!/usr/bin/env bash
# Builds pdf-tools.exe for 64-bit Windows with MSVC and packs it as PDF-Tools-Windows.zip. Runs in Git Bash, as on
# the windows-latest CI runner: needs Visual Studio 2022 or 2026 with the C++ tools (MSBuild, cl, nmake, and the CMake and
# Ninja it ships), Strawberry Perl (OpenSSL's Configure), curl and git.
# The MSVC environment and the libraries come from deps.sh (MuPDF with Tesseract, PCRE2, OpenSSL and raylib, built
# from the sources pinned in platform/versions.sh) and are linked statically. The zip holds the program, the English
# OCR data and the Visual C++ runtime DLLs it needs, so nothing has to be installed. Output: $PT_WINDOWS_OUT/out
# (default ~/.cache/pdf-tools/windows). Re-runs are incremental. `build.sh check` builds and runs the tests without
# packing.
# MuPDF's CJK and Noto fallback fonts are left out (fonts a PDF does not embed come from %WINDIR%\Fonts);
# PT_FONTS=full bundles them.
set -euo pipefail

MODE=${1:-}
case "$MODE" in
'' | check) ;;
*) echo "usage: $0 [check]" >&2; exit 1 ;;
esac

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/deps.sh"

step "PDF Tools"
cmake --fresh -S "$(win "$REPO")" -B "$(win "$ROOT/build")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DPT_USE_FONTCONFIG=OFF \
    -DMUPDF_INCLUDE_DIR="$(win "$MUPDF_SRC/include")" -DMUPDF_LIBRARY="$(win "$MUPDF_OUT/libmupdf.lib")" \
    -DMUPDF_THIRD_LIBRARY="$MUPDF_THIRD" \
    -DPCRE2_INCLUDE_DIR="$(win "$PCRE2_OUT/include")" -DPCRE2_LIBRARY="$(win "$PCRE2_LIB")" \
    -DOPENSSL_ROOT_DIR="$(win "$OPENSSL_OUT")" -DOPENSSL_USE_STATIC_LIBS=ON \
    -Draylib_DIR="$(win "$RAYLIB_OUT/lib/cmake/raylib")" -DPT_TESSDATA="$(win "$ROOT/dl/eng.traineddata")" >/dev/null
cmake --build "$(win "$ROOT/build")" -j"$JOBS"
ctest --test-dir "$(win "$ROOT/build")" --output-on-failure
if [ "$MODE" = check ]; then exit 0; fi

step "PDF-Tools-Windows.zip"
mkdir -p "$ROOT/out"
STAGE=$ROOT/PDF-Tools
rm -rf "$STAGE"
mkdir -p "$STAGE/tessdata"
cp "$ROOT/build/pdf-tools.exe" "$STAGE/"
cp "$ROOT/dl/eng.traineddata" "$STAGE/tessdata/"
cp "$REPO/LICENSE" "$STAGE/LICENSE.txt"
# the Visual C++ runtime next to the program (app-local deployment, as the Visual Studio license allows)
CRT=$(ls -d "$(cygpath -u "$VCToolsRedistDir")"/x64/Microsoft.VC*.CRT | head -1)
cp "$CRT"/*.dll "$STAGE/"
ZIP=$ROOT/out/PDF-Tools-Windows.zip
rm -f "$ZIP"
powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$STAGE")' -DestinationPath '$(cygpath -w "$ZIP")'"
ls -l "$ZIP"
