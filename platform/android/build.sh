#!/usr/bin/env bash
# Builds MuPDF (with Tesseract), PCRE2, OpenSSL, raylib and libpdf-tools.so for arm64-v8a from the sources pinned in
# platform/versions.sh and packages a debug-signed APK, without Gradle.
# Output: $PT_ANDROID_OUT/out/pdf-tools.apk (default ~/.cache/pdf-tools-native/android, which also keeps the debug
# signing key: a new key would stop the APK installing over an older one). Re-runs are incremental.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/../versions.sh"
ROOT=${PT_ANDROID_OUT:-$HOME/.cache/pdf-tools-native/android}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/28.2.13676358}
BT=$SDK/build-tools/36.1.0
ANDROID_JAR=$SDK/platforms/android-36.1/android.jar
# Android 11: all-files storage access, WindowInsets types and the NDK font matcher
MIN_API=30
# 35+ enforces edge-to-edge: system bars and display cutouts would cover the window, which raylib cannot inset
TARGET_API=34
ABI=arm64-v8a
JOBS=$(nproc)

MUPDF_SRC=$ROOT/src/mupdf-$MUPDF_VERSION-source
MUPDF_OUT=$ROOT/mupdf-build-$BUILD_TAG
PCRE2_SRC=$ROOT/src/pcre2-$PCRE2_VERSION
PCRE2_OUT=$ROOT/pcre2-install-$BUILD_TAG
OPENSSL_SRC=$ROOT/src/openssl-$OPENSSL_VERSION
OPENSSL_OUT=$ROOT/openssl-install-$BUILD_TAG
TESSDATA=$ROOT/dl/eng.traineddata

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
for tool in "$TC/clang" "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$ANDROID_JAR"; do
    [ -e "$tool" ] || { echo "missing: $tool" >&2; exit 1; }
done
for tool in javac keytool zip cmake ninja make curl; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT/dl" "$ROOT/src"

step "MuPDF $MUPDF_VERSION"
fetch "$MUPDF_URL" "$ROOT/dl/mupdf-$MUPDF_VERSION-source.tar.gz" "$MUPDF_SHA256"
[ -d "$MUPDF_SRC" ] || tar xzf "$ROOT/dl/mupdf-$MUPDF_VERSION-source.tar.gz" -C "$ROOT/src"
# MuPDF's Makerules probes the host with pkg-config; nothing from the host may leak into the Android build.
mkdir -p "$ROOT/nopkgconfig"
printf '#!/bin/sh\nexit 1\n' > "$ROOT/nopkgconfig/pkg-config"
chmod +x "$ROOT/nopkgconfig/pkg-config"
PATH="$ROOT/nopkgconfig:$PATH" make -C "$MUPDF_SRC" -j"$JOBS" libs \
    OS=android build=release tesseract=yes OUT="$MUPDF_OUT" \
    CC="$TC/aarch64-linux-android$MIN_API-clang" CXX="$TC/aarch64-linux-android$MIN_API-clang++" \
    AR="$TC/llvm-ar" RANLIB="$TC/llvm-ranlib" \
    HAVE_X11=no HAVE_GLUT=no HAVE_LIBCRYPTO=no HAVE_OBJCOPY=no USE_SYSTEM_LIBS=no \
    XCFLAGS="-fPIC -Wno-unused-but-set-variable $SIZE_CFLAGS" >"$ROOT/mupdf-build.log" 2>&1 \
    || { tail -40 "$ROOT/mupdf-build.log" >&2; exit 1; }
ls -l "$MUPDF_OUT/libmupdf.a" "$MUPDF_OUT/libmupdf-third.a"

step "PCRE2 $PCRE2_VERSION"
fetch "$PCRE2_URL" "$ROOT/dl/pcre2-$PCRE2_VERSION.tar.bz2" "$PCRE2_SHA256"
[ -d "$PCRE2_SRC" ] || tar xjf "$ROOT/dl/pcre2-$PCRE2_VERSION.tar.bz2" -C "$ROOT/src"
if [ ! -f "$PCRE2_OUT/lib/libpcre2-8.a" ]; then
    cmake -S "$PCRE2_SRC" -B "$ROOT/pcre2-build-$BUILD_TAG" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$MIN_API -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" \
        -DCMAKE_INSTALL_PREFIX="$PCRE2_OUT" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_TESTS=OFF -DPCRE2_SUPPORT_JIT=OFF \
        >"$ROOT/pcre2-build.log" 2>&1 || { tail -40 "$ROOT/pcre2-build.log" >&2; exit 1; }
    cmake --build "$ROOT/pcre2-build-$BUILD_TAG" --target install -j"$JOBS" >>"$ROOT/pcre2-build.log" 2>&1 \
        || { tail -40 "$ROOT/pcre2-build.log" >&2; exit 1; }
fi
ls -l "$PCRE2_OUT/lib/libpcre2-8.a"

step "OpenSSL $OPENSSL_VERSION (libcrypto, certificate signatures)"
fetch "$OPENSSL_URL" "$ROOT/dl/openssl-$OPENSSL_VERSION.tar.gz" "$OPENSSL_SHA256"
[ -d "$OPENSSL_SRC" ] || tar xzf "$ROOT/dl/openssl-$OPENSSL_VERSION.tar.gz" -C "$ROOT/src"
if [ ! -f "$OPENSSL_OUT/lib/libcrypto.a" ]; then
    (cd "$OPENSSL_SRC" && make distclean >/dev/null 2>&1; PATH="$TC:$PATH" ANDROID_NDK_ROOT="$NDK" ./Configure \
        android-arm64 -D__ANDROID_API__=$MIN_API no-shared no-tests no-apps no-docs --prefix="$OPENSSL_OUT" --libdir=lib \
        $SIZE_CFLAGS \
        && PATH="$TC:$PATH" ANDROID_NDK_ROOT="$NDK" make -j"$JOBS" build_libs && make install_dev) \
        >"$ROOT/openssl-build.log" 2>&1 || { tail -40 "$ROOT/openssl-build.log" >&2; exit 1; }
fi
ls -l "$OPENSSL_OUT/lib/libcrypto.a"

step "Tesseract English data (packed into the APK as an asset)"
fetch "$TESSDATA_URL" "$TESSDATA" "$TESSDATA_SHA256"

step "libpdf-tools.so (raylib, core, window)"
[ -d "$HERE/../../thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$HERE/../../thirdparty/raylib"
[ -d "$HERE/../../thirdparty/raygui" ] || git clone --depth 1 --branch "$RAYGUI_TAG" "$RAYGUI_URL" "$HERE/../../thirdparty/raygui"
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

KS=$ROOT/debug.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
mkdir -p "$ROOT/out"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
    --out "$ROOT/out/pdf-tools.apk" "$APK/aligned.apk"
"$BT/apksigner" verify "$ROOT/out/pdf-tools.apk"
ls -l "$ROOT/out/pdf-tools.apk"
