#!/usr/bin/env bash
# Builds libpdf-tools.so (raylib, core, window) for arm64-v8a against the libraries from deps.sh (MuPDF with
# Tesseract, PCRE2 and OpenSSL, built from the sources pinned in platform/versions.sh) and packages a debug-signed
# APK, without Gradle. `build.sh check` only compiles libpdf-tools.so (no APK).
# Output: $PT_ANDROID_OUT/out/pdf-tools.apk (default ~/.cache/pdf-tools-native/android, which also keeps the debug
# signing key: a new key would stop the APK installing over an older one). Re-runs are incremental.
# With PT_ANDROID_KEYSTORE (a .jks) and PT_ANDROID_KEYSTORE_PASSWORD set, the APK is signed with that keystore's key
# `pdf-tools` (key password = store password) instead, as releases are (README.md).
# MuPDF's CJK and Noto fallback fonts are left out (fonts a PDF does not embed come from /system/fonts);
# PT_FONTS=full bundles them.
set -euo pipefail

MODE=${1:-}
case "$MODE" in
'' | check) ;;
*) echo "usage: $0 [check]" >&2; exit 1 ;;
esac

SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
BT=$SDK/build-tools/36.1.0
ANDROID_JAR=$SDK/platforms/android-36.1/android.jar
# 35+ enforces edge-to-edge: system bars and display cutouts would cover the window, which raylib cannot inset
TARGET_API=34
if [ -z "$MODE" ]; then
    for tool in "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$ANDROID_JAR"; do
        [ -e "$tool" ] || { echo "missing: $tool" >&2; exit 1; }
    done
    for tool in javac keytool zip; do
        command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
    done
    if [ -n "${PT_ANDROID_KEYSTORE:-}${PT_ANDROID_KEYSTORE_PASSWORD:-}" ]; then
        [ -n "${PT_ANDROID_KEYSTORE:-}" ] && [ -n "${PT_ANDROID_KEYSTORE_PASSWORD:-}" ] \
            || { echo "PT_ANDROID_KEYSTORE and PT_ANDROID_KEYSTORE_PASSWORD go together" >&2; exit 1; }
        [ -f "$PT_ANDROID_KEYSTORE" ] || { echo "missing: $PT_ANDROID_KEYSTORE" >&2; exit 1; }
    fi
fi

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/deps.sh"

step "libpdf-tools.so (raylib, core, window)"
cmake --fresh -S "$HERE" -B "$ROOT/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$MIN_API -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--gc-sections" \
    -DMUPDF_INCLUDE_DIR="$MUPDF_SRC/include" -DMUPDF_LIBRARY="$MUPDF_OUT/libmupdf.a" \
    -DMUPDF_THIRD_LIBRARY="$MUPDF_OUT/libmupdf-third.a" \
    -DPCRE2_INCLUDE_DIR="$PCRE2_OUT/include" -DPCRE2_LIBRARY="$PCRE2_OUT/lib/libpcre2-8.a" \
    -DOPENSSL_ROOT_DIR="$OPENSSL_OUT" -DOPENSSL_USE_STATIC_LIBS=ON -DCMAKE_FIND_ROOT_PATH="$OPENSSL_OUT" \
    -DPKG_CONFIG_EXECUTABLE="$ROOT/nopkgconfig/pkg-config" >/dev/null
cmake --build "$ROOT/build" --target pdf-tools -j"$JOBS"
if [ "$MODE" = check ]; then exit 0; fi

step "dex"
APK=$ROOT/apk
mkdir -p "$APK/classes" "$APK/dex" "$APK/res" "$APK/lib/$ABI"
javac -nowarn --release 11 -Xlint:-options -cp "$ANDROID_JAR" -d "$APK/classes" \
    $(find "$HERE/java" -name '*.java')
"$BT/d8" --release --min-api $MIN_API --lib "$ANDROID_JAR" --output "$APK/dex" $(find "$APK/classes" -name '*.class')

step "resources"
MANIFEST=$HERE/AndroidManifest.xml
# the launcher icons in res/ are assets/logo.svg drawn at each density
"$BT/aapt2" compile --dir "$HERE/res" -o "$APK/res.zip"
mkdir -p "$APK/assets/tessdata"
cp "$TESSDATA" "$APK/assets/tessdata/eng.traineddata"
# versionCode must grow with each release: 1.2.3 -> 10203
IFS=. read -r V_MAJOR V_MINOR V_PATCH <<<"$PT_VERSION"
"$BT/aapt2" link -o "$APK/unsigned.apk" -I "$ANDROID_JAR" --manifest "$MANIFEST" -R "$APK/res.zip" -A "$APK/assets" \
    --min-sdk-version $MIN_API --target-sdk-version $TARGET_API --debug-mode --auto-add-overlay \
    --version-name "$PT_VERSION" --version-code $((V_MAJOR * 10000 + V_MINOR * 100 + V_PATCH))

step "package"
"$TC/llvm-strip" --strip-unneeded -o "$APK/lib/$ABI/libpdf-tools.so" "$ROOT/build/libpdf-tools.so"
cp "$APK/dex/classes.dex" "$APK/classes.dex"
(cd "$APK" && zip -q -r unsigned.apk classes.dex "lib/$ABI/libpdf-tools.so")
"$BT/zipalign" -P 16 -f 4 "$APK/unsigned.apk" "$APK/aligned.apk"

mkdir -p "$ROOT/out"
if [ -n "${PT_ANDROID_KEYSTORE:-}" ]; then
    # env: keeps the password off the command line
    "$BT/apksigner" sign --ks "$PT_ANDROID_KEYSTORE" --ks-key-alias pdf-tools \
        --ks-pass env:PT_ANDROID_KEYSTORE_PASSWORD --key-pass env:PT_ANDROID_KEYSTORE_PASSWORD \
        --out "$ROOT/out/pdf-tools.apk" "$APK/aligned.apk"
else
    KS=$ROOT/debug.keystore
    [ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
    "$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
        --out "$ROOT/out/pdf-tools.apk" "$APK/aligned.apk"
fi
"$BT/apksigner" verify "$ROOT/out/pdf-tools.apk"
ls -l "$ROOT/out/pdf-tools.apk"
