# PDF Tools — native C port (MuPDF + raylib)

The C version of PDF Tools: a core library, the agent-facing command line and the window, built on MuPDF and
raylib/raygui. It replaces the PyMuPDF/PySide6 application (`src/pdf_tools`, being phased out). The same window
sources build for Linux desktops, macOS 11+ (`macos/`) and, through `android/`, Android 11+ phones and tablets.

| Part | Sources | Target |
| --- | --- | --- |
| Core: edit model, save pipeline, text, forms, fonts, SyncTeX, settings | `src/core/`, `include/pt_core.h` | `ptcore` |
| Command line (the `AGENTS.md` contract, plus extras) | `src/cli.c` | `ptcli`, `pdf-tools-c` |
| Window: tabs, tools, sidebar, dialogs, printing, IPC, Combine & split | `src/ui/` | `pdf-tools-gui` (binary `pdf-tools`) |
| Android APK | `android/` | `android/build.sh` (see `android/README.md`) |
| macOS app and .dmg | `macos/`, `src/ui/macos.c` | `macos/build.sh` (on a Mac; CI uploads the .dmg as an artifact) |
| Core tests | `tests/test_core.c` | `test_core` (ctest) |

## Build

Requirements: C11, CMake 3.20+, MuPDF 1.28 headers and library (with Tesseract for OCR, as PyMuPDF's is),
and, all optional: PCRE2 (regex search, redaction, finding personal data), fontconfig (installed fonts for text
boxes) and OpenSSL libcrypto (certificate signatures). The window needs raylib 6.2 and raygui 5.0 in
`thirdparty/` (not tracked); build raylib with both of GLFW's Linux backends, so the window runs natively on
Wayland and on X11 (it picks Wayland when `WAYLAND_DISPLAY` is set):

```sh
git clone --depth 1 --branch 6.2 https://github.com/raysan5/raylib.git native/thirdparty/raylib
git clone --depth 1 --branch 5.0 https://github.com/raysan5/raygui.git native/thirdparty/raygui
cmake -S native/thirdparty/raylib -B ~/.cache/pdf-tools-native/raylib-build -DBUILD_EXAMPLES=OFF \
  -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON -DCMAKE_INSTALL_PREFIX=$PWD/native/thirdparty/raylib-install
cmake --build ~/.cache/pdf-tools-native/raylib-build --target install -j
```

MuPDF can come from the PyMuPDF wheel in the repo's `.venv` (as in CI):

```sh
D=$PWD/.venv/lib/python3.14/site-packages/pymupdf
cmake -S native -B ~/.cache/pdf-tools-native/build \
  -DMUPDF_INCLUDE_DIR=$D/mupdf-devel/include -DMUPDF_LIBRARY=$D/libmupdf.so.28.5 \
  -Draylib_DIR=$PWD/native/thirdparty/raylib-install/lib/cmake/raylib -DPython3_EXECUTABLE=$PWD/.venv/bin/python
cmake --build ~/.cache/pdf-tools-native/build -j
```

Keep the build folder outside the repo. `-DPT_BUILD_GUI=OFF` builds the core and the command line only.

### macOS

On a Mac with the Xcode command line tools and CMake, `native/macos/build.sh` builds MuPDF (with Tesseract), PCRE2, OpenSSL
and raylib from source, links them statically, bundles the English OCR data and makes `PDF Tools.app` and `PDF-Tools.dmg` (macOS 11+, the Mac's own
architecture) in `~/Library/Caches/pdf-tools-native/macos/out`. Every push also builds it on GitHub Actions; the
`.dmg` is the run's `PDF-Tools-macOS` artifact. The app is ad-hoc signed, not notarized: open it the first time
with right-click > Open. Platform pieces: Retina rendering (`ui.density`), the system file chooser through
`osascript`, `~/.Trash`, `open` for links, Core Text for installed fonts, window IPC over socket files in
`/tmp/pdf-tools-<uid>`, and documents opened from Finder (`src/ui/macos.c`). The command line is
`"/Applications/PDF Tools.app/Contents/MacOS/pdf-tools" info file.pdf`.

## Test

```sh
ctest --test-dir ~/.cache/pdf-tools-native/build --output-on-failure            # core
PDF_TOOLS_BIN=~/.cache/pdf-tools-native/build/pdf-tools-c .venv/bin/python -m pytest tests/test_cli.py   # CLI contract
```

`.github/workflows/native.yml` runs both on every change under `native/`.

## Command line

`pdf-tools COMMAND` (the window binary) and `pdf-tools-c COMMAND` run the commands documented in `AGENTS.md`
(`info`, `redact`, `fill`, `pages`, `merge`, `split`, `protect`, `unprotect`, `flatten`, `synctex`) with the
same options, JSON output and exit codes. C-only extras: `search`, `render`, `annotate`, `edit-annotation`,
`draw`, `insert-text`, `insert-image`, `sign` (an image, or `--cert` for a digital signature), `prepare-signature`,
`scrub`, `inspect`, `bates`, `watermark`, `ocr`, `compare`, `export-images`, `redact-text` and `redact --rect`;
`pdf-tools-c COMMAND --help` lists their options.

OCR needs Tesseract's language data: `LANG.traineddata` in `$TESSDATA_PREFIX`, `~/.local/share/pdf-tools/tessdata`,
next to the program, or the system's `tessdata` folder (on Debian/Ubuntu `tesseract-ocr-eng`). The macOS app and
the APK carry English (tessdata_fast). Logging: set `PDF_TOOLS_LOG` to `error`, `warn` (default), `info` or `debug`.

## Window

Opening a PDF: `pdf-tools file.pdf` (tabs go to the window used last), `--new-window`, `--forward FILE LINE
SOURCE` for SyncTeX. Features: View (links, forms, text selection), Redact (with Find personal data: e-mails,
phone numbers, IBANs, card numbers, dates, ID numbers), Highlight and other marks, Note, Text, Sign (an image,
or a digital signature from a .p12/.pfx certificate, made when saving), Image, Draw, Shape (rectangle, ellipse,
line, arrow), Crop; Bates numbers, watermarks, document properties, OCR (a recognized copy), Compare with
another PDF; a sidebar with Pages, Outline (editable), Annotations and Edits; find with regex; undo/redo;
fit width or height, two pages side by side, presentation (F5); save, save as, export pages (as PDF or images),
save optimized copy, protect, print; Combine & split. Colours: presets, favourites (`favourite_colours` in
settings.toml), a colour chooser, and an accent colour per theme (View > Appearance).
Each file reopens where it was left (`$XDG_DATA_HOME/pdf-tools/positions.tsv`). Narrow windows (phones) get a
compact toolbar and an overlay sidebar.

## Safety notes

Redaction removes the text, images and drawings under each mark (MuPDF) and the save scrubs metadata,
attachments, scripts, hidden text and thumbnails; check the result before sharing sensitive documents. `sign`
places a visual signature, not a cryptographic one. `flatten` rasterizes pages. Passwords given on the command
line can show in process listings.

**License:** AGPL-3.0-or-later, as the parent repository and MuPDF.
