#!/usr/bin/env bash
# Builds MuPDF, raylib and libpdf-tools.so for arm64-v8a and packages a debug-signed APK, without Gradle.
# Output: $PT_ANDROID_OUT/out/pdf-tools.apk (default ~/.cache/pdf-tools-native/android). Re-runs are incremental.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=${PT_ANDROID_OUT:-$HOME/.cache/pdf-tools-native/android}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/28.2.13676358}
BT=$SDK/build-tools/36.1.0
ANDROID_JAR=$SDK/platforms/android-36.1/android.jar
MIN_API=26
# 35+ enforces edge-to-edge: system bars and display cutouts would cover the window, which raylib cannot inset
TARGET_API=34
ABI=arm64-v8a
JOBS=$(nproc)

MUPDF_VERSION=1.28.5
MUPDF_SHA256=98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934
MUPDF_TAR=$ROOT/dl/mupdf-$MUPDF_VERSION-source.tar.gz
MUPDF_SRC=$ROOT/src/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$ROOT/mupdf-build

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
for tool in "$TC/clang" "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$ANDROID_JAR"; do
    [ -e "$tool" ] || { echo "missing: $tool" >&2; exit 1; }
done
for tool in javac keytool zip cmake ninja make; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }

step "MuPDF $MUPDF_VERSION"
mkdir -p "$ROOT/dl" "$ROOT/src"
if [ ! -f "$MUPDF_TAR" ]; then
    curl -fL -o "$MUPDF_TAR.part" "https://mupdf.com/downloads/archive/mupdf-$MUPDF_VERSION-source.tar.gz"
    mv "$MUPDF_TAR.part" "$MUPDF_TAR"
fi
echo "$MUPDF_SHA256  $MUPDF_TAR" | sha256sum -c --quiet
[ -d "$MUPDF_SRC" ] || tar xzf "$MUPDF_TAR" -C "$ROOT/src"
# MuPDF's Makerules probes the host with pkg-config; nothing from the host may leak into the Android build.
mkdir -p "$ROOT/nopkgconfig"
printf '#!/bin/sh\nexit 1\n' > "$ROOT/nopkgconfig/pkg-config"
chmod +x "$ROOT/nopkgconfig/pkg-config"
PATH="$ROOT/nopkgconfig:$PATH" make -C "$MUPDF_SRC" -j"$JOBS" libs \
    OS=android build=release OUT="$MUPDF_OUT" \
    CC="$TC/aarch64-linux-android$MIN_API-clang" CXX="$TC/aarch64-linux-android$MIN_API-clang++" \
    AR="$TC/llvm-ar" RANLIB="$TC/llvm-ranlib" \
    HAVE_X11=no HAVE_GLUT=no HAVE_LIBCRYPTO=no HAVE_OBJCOPY=no USE_SYSTEM_LIBS=no \
    XCFLAGS="-fPIC -Wno-unused-but-set-variable" >"$ROOT/mupdf-build.log" 2>&1 \
    || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
ls -l "$MUPDF_OUT/libmupdf.a" "$MUPDF_OUT/libmupdf-third.a"

step "libpdf-tools.so (raylib, core, window)"
cmake -S "$HERE" -B "$ROOT/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$MIN_API -DCMAKE_BUILD_TYPE=Release \
    -DMUPDF_INCLUDE_DIR="$MUPDF_SRC/include" -DMUPDF_LIBRARY="$MUPDF_OUT/libmupdf.a" \
    -DMUPDF_THIRD_LIBRARY="$MUPDF_OUT/libmupdf-third.a" >/dev/null
cmake --build "$ROOT/build" --target pdf-tools -j"$JOBS"

step "dex"
APK=$ROOT/apk
mkdir -p "$APK/classes" "$APK/dex" "$APK/res" "$APK/lib/$ABI"
javac -nowarn --release 11 -Xlint:-options -cp "$ANDROID_JAR" -d "$APK/classes" \
    $(find "$HERE/java" -name '*.java')
"$BT/d8" --release --min-api $MIN_API --lib "$ANDROID_JAR" --output "$APK/dex" $(find "$APK/classes" -name '*.class')

step "resources"
LOGO=$HERE/../../src/pdf_tools/assets/logo.svg
PY=$HERE/../../.venv/bin/python
MANIFEST=$HERE/AndroidManifest.xml
RES_FLAGS=()
if [ -x "$PY" ] && "$PY" -I -c 'import pymupdf' 2>/dev/null; then
    # launcher icon from the app logo (the venv's PyMuPDF renders SVG); without it the APK has the default icon
    for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
        mkdir -p "$APK/res/mipmap-${d%%:*}"
        "$PY" -I -c 'import sys, pymupdf
doc = pymupdf.open(sys.argv[1]); page = doc[0]; px = int(sys.argv[2])
page.get_pixmap(matrix=pymupdf.Matrix(px / page.rect.width, px / page.rect.height), alpha=True).save(sys.argv[3])' \
            "$LOGO" "${d##*:}" "$APK/res/mipmap-${d%%:*}/ic_launcher.png"
    done
    "$BT/aapt2" compile --dir "$APK/res" -o "$APK/res.zip"
    RES_FLAGS=(-R "$APK/res.zip")
else
    echo "no PyMuPDF in .venv: building without a launcher icon"
    sed '/android:icon=/d' "$HERE/AndroidManifest.xml" > "$APK/AndroidManifest.xml"
    MANIFEST=$APK/AndroidManifest.xml
fi
"$BT/aapt2" link -o "$APK/unsigned.apk" -I "$ANDROID_JAR" --manifest "$MANIFEST" "${RES_FLAGS[@]}" \
    --min-sdk-version $MIN_API --target-sdk-version $TARGET_API --debug-mode --auto-add-overlay

step "package"
"$TC/llvm-strip" --strip-unneeded -o "$APK/lib/$ABI/libpdf-tools.so" "$ROOT/build/libpdf-tools.so"
cp "$APK/dex/classes.dex" "$APK/classes.dex"
(cd "$APK" && zip -q -r unsigned.apk classes.dex "lib/$ABI/libpdf-tools.so")
"$BT/zipalign" -P 16 -f 4 "$APK/unsigned.apk" "$APK/aligned.apk"

KS=$ROOT/debug.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
mkdir -p "$ROOT/out"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
    --out "$ROOT/out/pdf-tools.apk" "$APK/aligned.apk"
"$BT/apksigner" verify "$ROOT/out/pdf-tools.apk"
ls -l "$ROOT/out/pdf-tools.apk"
