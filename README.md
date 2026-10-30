# PDF Tools

A PDF viewer and editor in C, built on MuPDF and raylib/raygui: redact (content is removed, not covered), find
personal data, fill forms, mark and comment, add text, images, shapes and signatures (pictures or digital
certificate signatures), Bates numbers and watermarks, OCR, compare, merge and split. It runs on Linux, macOS 11+
and Android 11+, and has a command line for scripts and agents (`AGENTS.md`).

| Part | Sources |
| --- | --- |
| Core library: edit model, save pipeline, text, fonts, forms, stamps, OCR, signing, SyncTeX, settings | `src/core/`, `include/pt_core.h` |
| Command line (the `AGENTS.md` contract, plus extras) | `src/cli.c` |
| Window: tabs, tools, sidebar, dialogs, views, printing, window IPC | `src/ui/` |
| Linux build and AppImage, macOS app and .dmg, Android APK | `platform/linux/`, `platform/macos/`, `platform/android/` |
| Third-party versions and checksums for all of them | `platform/versions.sh` |
| Tests (ctest): the core, and the command-line contract | `tests/` |

## Build

`platform/linux/build.sh`, `platform/macos/build.sh` and `platform/android/build.sh` build everything from the
pinned sources in `platform/versions.sh` (MuPDF with Tesseract, PCRE2, OpenSSL, raylib), run the tests and pack an
AppImage, a `.dmg` or an APK. Each push builds the AppImage and the `.dmg` on GitHub Actions (artifacts
`PDF-Tools-Linux` and `PDF-Tools-macOS`); the debug-signed APK is committed at `platform/android/pdf-tools.apk`.

On Linux the script needs a C/C++ compiler, CMake, pkg-config, curl, git and the window system headers (Debian,
Ubuntu: `build-essential cmake pkg-config libfontconfig-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev
libxi-dev libgl1-mesa-dev libegl-dev libxkbcommon-dev libwayland-dev wayland-protocols`). Its build folder,
`~/.cache/pdf-tools/linux/build`, rebuilds incrementally with `cmake --build`.

With your own dependencies, CMake finds them through pkg-config (`mupdf` 1.28, `libpcre2-8`, `fontconfig`,
`libcrypto`; set `PKG_CONFIG_PATH` for a prefix of your own) and raylib 6.2 through `CMAKE_PREFIX_PATH`:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/raylib && cmake --build build && ctest --test-dir build
cmake --install build --prefix ~/.local     # pdf-tools, man page, launcher entry, icon; libptcore, pt_core.h, pdf-tools.pc
```

Build raylib with both of GLFW's Linux backends (`-DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON`): the window runs
natively on Wayland when `WAYLAND_DISPLAY` is set and on X11 otherwise. Without raylib (or with
`-DPT_BUILD_GUI=OFF`) `pdf-tools` runs commands only. `cmake --install --component app` installs the program alone.

On a Mac the app is ad-hoc signed, not notarized: open it the first time with right-click > Open. The command line
is `"/Applications/PDF Tools.app/Contents/MacOS/pdf-tools"`. Android details: `platform/android/README.md`.

## Command line

`pdf-tools COMMAND` runs the commands documented in `AGENTS.md` (`info`, `redact`, `fill`, `pages`, `merge`,
`split`, `protect`, `unprotect`, `flatten`, `synctex`) without a window, prints one JSON object and exits with a
meaningful code. Extras: `search`, `render`, `annotate`, `edit-annotation`, `draw`, `insert-text`, `insert-image`,
`sign` (an image, or `--cert` for a digital signature), `prepare-signature`, `scrub`, `inspect`, `bates`,
`watermark`, `ocr`, `compare`, `export-images`, `redact-text` and `redact --rect`; `pdf-tools COMMAND --help`
lists their options.

OCR needs Tesseract's language data, `LANG.traineddata`, found in `$TESSDATA_PREFIX`,
`$XDG_DATA_HOME/pdf-tools/tessdata`, next to the program, or the system's data folders (`$XDG_DATA_DIRS`, e.g.
Debian/Ubuntu's `tesseract-ocr-eng`). The AppImage, the macOS app and the APK carry English. Logging: set
`PDF_TOOLS_LOG` to `error`, `warn` (default), `info` or `debug`.

## Window

`pdf-tools file.pdf` opens tabs in the window used last; `--new-window`; `--forward FILE LINE SOURCE` for
SyncTeX. Tools: View (links, forms, text selection), Redact (and Find personal data: e-mails, phone numbers,
IBANs, card numbers, dates, ID numbers), Highlight and other marks, Note, Text, Sign (an image, or a certificate
signature made when saving), Image, Draw, Shape, Crop. Also Bates numbers, watermarks, document properties, OCR
(into a copy), Compare (a tab), Combine & split (a tab); a sidebar with Pages, Outline (editable), Annotations and
Edits; find with regular expressions; undo and redo; fit width or height, two pages side by side, presentation
(F5); save, save as, export pages (as PDF or images), save an optimized copy, protect, print. Colours: presets,
favourites, a chooser, and an accent colour per theme. Settings: `~/.config/pdf-tools/settings.toml`. Each file
reopens where it was left. Narrow windows (phones) get a compact toolbar and an overlay sidebar.

LaTeX: open PDFs reload when they change on disk (e.g. `latexmk -pvc`). Compile with `-synctex=1`; Ctrl+click
in the PDF opens the source line in your editor (`editor_command`, e.g. `code -g %f:%l`), and
`pdf-tools --forward file.pdf LINE file.tex` from the editor shows that line in the PDF.

## Safety notes

Redaction removes the text, images and drawings under each mark (MuPDF) and the save scrubs metadata,
attachments, scripts, hidden text (including OCR layers) and thumbnails; check the result before sharing
sensitive documents. A picture signature is not a cryptographic one; use a certificate for that. `flatten`
rasterizes pages. Passwords given on the command line can show in process listings.

**License:** AGPL-3.0-or-later, as MuPDF.
