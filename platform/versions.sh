# The third-party sources every build script uses, in one place: versions and SHA-256 checksums.
# Sourced by platform/{linux,macos,android}/build.sh; the CI workflow runs those scripts.

MUPDF_VERSION=1.28.5
MUPDF_SHA256=98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934
MUPDF_URL=https://mupdf.com/downloads/archive/mupdf-$MUPDF_VERSION-source.tar.gz

PCRE2_VERSION=10.45
PCRE2_SHA256=21547f3516120c75597e5b30a992e27a592a31950b5140e7b8bfde3f192033c4
PCRE2_URL=https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$PCRE2_VERSION/pcre2-$PCRE2_VERSION.tar.bz2

OPENSSL_VERSION=3.5.4
OPENSSL_SHA256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz

RAYLIB_TAG=6.2
RAYLIB_URL=https://github.com/raysan5/raylib.git
RAYGUI_TAG=5.0
RAYGUI_URL=https://github.com/raysan5/raygui.git

# Tesseract's English data, the small "fast" model
TESSDATA_VERSION=4.1.0
TESSDATA_SHA256=7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2
TESSDATA_URL=https://github.com/tesseract-ocr/tessdata_fast/raw/$TESSDATA_VERSION/eng.traineddata

APPIMAGETOOL_VERSION=1.9.1
APPIMAGETOOL_SHA256_x86_64=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
APPIMAGETOOL_SHA256_aarch64=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL_VERSION

# download $1 to $2 once and check it against SHA-256 $3
fetch() {
    [ -f "$2" ] || { curl -fL -o "$2.part" "$1" && mv "$2.part" "$2"; }
    if command -v sha256sum >/dev/null; then echo "$3  $2" | sha256sum -c --quiet
    else echo "$3  $2" | shasum -a 256 -c --quiet; fi
}
