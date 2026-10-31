#!/usr/bin/env bash
# Builds pdf-tools.exe for 64-bit Windows with MSVC and packs it as PDF-Tools-Windows.zip. Runs in Git Bash, as on
# the windows-latest CI runner: needs Visual Studio 2022 or 2026 with the C++ tools (MSBuild, cl, nmake, and the CMake and
# Ninja it ships), Strawberry Perl (OpenSSL's Configure), curl and git.
# MuPDF (with Tesseract, through its Visual Studio solution), PCRE2, OpenSSL and raylib are built from the sources
# pinned in platform/versions.sh and linked statically. The zip holds the program, the English OCR data and the
# Visual C++ runtime DLLs it needs, so nothing has to be installed. Output: $PT_WINDOWS_OUT/out (default
# ~/.cache/pdf-tools/windows). Re-runs are incremental.
# MuPDF's CJK and Noto fallback fonts are left out (fonts a PDF does not embed come from %WINDIR%\Fonts);
# PT_FONTS=full bundles them.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${PT_WINDOWS_OUT:-$HOME/.cache/pdf-tools/windows}
JOBS=$(nproc)

for tool in curl git tar sha256sum cygpath cmd powershell; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done
PERL=/c/Strawberry/perl/bin/perl.exe
[ -x "$PERL" ] || PERL=$(command -v perl)
"$PERL" -e 'exit($^O eq "MSWin32" ? 0 : 1)' || { echo "OpenSSL needs a Windows Perl (Strawberry Perl)" >&2; exit 1; }

step() { printf '\n== %s\n' "$*"; }
# native tools get Windows paths, with forward slashes
win() { cygpath -m "$1"; }
mkdir -p "$ROOT/dl" "$ROOT/src" "$ROOT/out"

step "MSVC environment"
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
VS=$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
    -property installationPath | tr -d '\r')
[ -n "$VS" ] || { echo "no Visual Studio with the C++ tools" >&2; exit 1; }
# MuPDF's solution targets an older toolset: build it with the installed Visual Studio's own
case $("$VSWHERE" -latest -products '*' -property installationVersion | tr -d '\r' | cut -d. -f1) in
17) MSVC_TOOLSET=v143 ;; # Visual Studio 2022
18) MSVC_TOOLSET=v145 ;; # Visual Studio 2026
*) echo "unknown Visual Studio version at $VS" >&2; exit 1 ;;
esac
# vcvars64.bat only runs in cmd: take over what it adds to cmd's environment
printf '@set\r\n' >"$ROOT/env-before.bat"
printf '@call "%s\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\n@set\r\n' "$(cygpath -w "$VS")" >"$ROOT/env-msvc.bat"
cmd_env() { cmd //c "$(cygpath -w "$1")" | tr -d '\r' | sort; }
while IFS= read -r line; do
    name=${line%%=*} value=${line#*=}
    case "$name" in
    Path | PATH) PATH=$(cygpath -u -p "$value") ;;
    *) if [[ $name =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then export "$name=$value"; fi ;;
    esac
done < <(comm -13 <(cmd_env "$ROOT/env-before.bat") <(cmd_env "$ROOT/env-msvc.bat"))
# vcvars64.bat sets up the compiler, not MSBuild: vswhere says where that is
MSBUILD=$("$VSWHERE" -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    tr -d '\r' | head -1)
[ -n "$MSBUILD" ] || MSBUILD=$(find "$(cygpath -u "$VS")/MSBuild" -iname msbuild.exe -ipath '*/bin/msbuild.exe' | head -1)
echo "Visual Studio: $VS ($("$VSWHERE" -latest -products '*' -property catalog_productDisplayVersion | tr -d '\r'))"
echo "MSBuild: ${MSBUILD:-not found}"
# by its full path: Git Bash does not find MSBuild.exe as `msbuild`
[ -n "$MSBUILD" ] && [ -x "$(cygpath -u "$MSBUILD")" ] || { echo "no MSBuild in $VS" >&2; exit 1; }
MSBUILD=$(cygpath -u "$MSBUILD")
export PATH CC=cl CXX=cl
for tool in cl link nmake cmake ctest ninja; do
    command -v $tool >/dev/null || { echo "missing after vcvars64.bat: $tool" >&2; exit 1; }
done
cmake --version | head -1

step "MuPDF $MUPDF_VERSION"
# a source tree per font mode: the solution builds inside its tree
MUPDF_SRC=$ROOT/src/mupdf-$FONTS_TAG/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$MUPDF_SRC/platform/win32/x64/Release
fetch "$MUPDF_URL" "$ROOT/dl/mupdf.tar.gz" "$MUPDF_SHA256"
mkdir -p "$ROOT/src/mupdf-$FONTS_TAG"
# the symbolic links in the archive are in parts the library does not use, and need rights Windows rarely grants
[ -d "$MUPDF_SRC" ] || tar xzf "$ROOT/dl/mupdf.tar.gz" -C "$ROOT/src/mupdf-$FONTS_TAG" \
    --exclude='*/thirdparty/freeglut/progs' --exclude='*/thirdparty/zxing-cpp/wrappers'
# cl.exe adds the options in CL to every command line
CL="$MUPDF_FONT_CFLAGS" "$MSBUILD" "$(win "$MUPDF_SRC/platform/win32/mupdf.sln")" -t:libmupdf -p:Configuration=Release -p:Platform=x64 \
    -p:PlatformToolset=$MSVC_TOOLSET -m -nologo -v:minimal >"$ROOT/mupdf-build.log" 2>&1 \
    || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
ls -l "$MUPDF_OUT"/*.lib
# the libraries libmupdf.lib was built from, in case it does not carry them all itself
MUPDF_THIRD=
for lib in libthirdparty libresources libtesseract libleptonica libharfbuzz libextract libpkcs7 libmubarcode libzxing; do
    [ -f "$MUPDF_OUT/$lib.lib" ] && MUPDF_THIRD="$MUPDF_THIRD${MUPDF_THIRD:+;}$(win "$MUPDF_OUT/$lib.lib")"
done

step "PCRE2 $PCRE2_VERSION"
PCRE2_SRC=$ROOT/src/pcre2-$PCRE2_VERSION
PCRE2_OUT=$ROOT/pcre2-install
fetch "$PCRE2_URL" "$ROOT/dl/pcre2.tar.bz2" "$PCRE2_SHA256"
[ -d "$PCRE2_SRC" ] || tar xjf "$ROOT/dl/pcre2.tar.bz2" -C "$ROOT/src"
if ! ls "$PCRE2_OUT"/lib/pcre2-8*.lib >/dev/null 2>&1; then
    cmake -S "$(win "$PCRE2_SRC")" -B "$(win "$ROOT/pcre2-build")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$(win "$PCRE2_OUT")" -DBUILD_SHARED_LIBS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF \
        -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF >"$ROOT/pcre2-build.log" 2>&1 \
        || { tail -40 "$ROOT/pcre2-build.log" >&2; exit 1; }
    cmake --build "$(win "$ROOT/pcre2-build")" --target install -j"$JOBS" >>"$ROOT/pcre2-build.log" 2>&1 \
        || { tail -40 "$ROOT/pcre2-build.log" >&2; exit 1; }
fi
PCRE2_LIB=$(ls "$PCRE2_OUT"/lib/pcre2-8*.lib | head -1)
ls -l "$PCRE2_LIB"

step "OpenSSL $OPENSSL_VERSION (libcrypto, certificate signatures)"
OPENSSL_SRC=$ROOT/src/openssl-$OPENSSL_VERSION
OPENSSL_OUT=$ROOT/openssl-install
fetch "$OPENSSL_URL" "$ROOT/dl/openssl.tar.gz" "$OPENSSL_SHA256"
[ -d "$OPENSSL_SRC" ] || tar xzf "$ROOT/dl/openssl.tar.gz" -C "$ROOT/src"
if [ ! -f "$OPENSSL_OUT/lib/libcrypto.lib" ]; then
    # no-asm: the assembler versions need NASM, and signing a PDF does not need their speed
    (cd "$OPENSSL_SRC" && "$PERL" Configure VC-WIN64A no-shared no-tests no-apps no-docs no-asm \
        --prefix="$(win "$OPENSSL_OUT")" --openssldir="$(win "$OPENSSL_OUT")/ssl" \
        && nmake build_libs && nmake install_dev) >"$ROOT/openssl-build.log" 2>&1 \
        || { tail -40 "$ROOT/openssl-build.log" >&2; exit 1; }
fi
ls -l "$OPENSSL_OUT/lib/libcrypto.lib"

step "Tesseract English data"
fetch "$TESSDATA_URL" "$ROOT/dl/eng.traineddata" "$TESSDATA_SHA256"

step "raylib $RAYLIB_TAG, raygui $RAYGUI_TAG"
[ -d "$REPO/thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$REPO/thirdparty/raylib"
[ -d "$REPO/thirdparty/raygui" ] || git clone --depth 1 --branch "$RAYGUI_TAG" "$RAYGUI_URL" "$REPO/thirdparty/raygui"
RAYLIB_OUT=$ROOT/raylib-install
if [ ! -f "$RAYLIB_OUT/lib/raylib.lib" ]; then
    cmake -S "$(win "$REPO/thirdparty/raylib")" -B "$(win "$ROOT/raylib-build")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$(win "$RAYLIB_OUT")" >"$ROOT/raylib-build.log" 2>&1 \
        || { tail -40 "$ROOT/raylib-build.log" >&2; exit 1; }
    cmake --build "$(win "$ROOT/raylib-build")" --target install -j"$JOBS" >>"$ROOT/raylib-build.log" 2>&1 \
        || { tail -40 "$ROOT/raylib-build.log" >&2; exit 1; }
fi

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

step "PDF-Tools-Windows.zip"
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
