# PDF Tools

A PDF viewer and editor for Linux, macOS, Windows and Android, with a command line for scripts and agents.

![Viewer](assets/screenshots/viewer.png)

## Features

- **Redact**: the text, images and drawings under each mark are removed, not covered. Find personal data
  (e-mails, phone numbers, IBANs, card numbers, dates, ID numbers) and mark it in one go. Saving also scrubs
  metadata, attachments, scripts, hidden text and thumbnails.
- **Edit**: highlight and other marks, notes, text, images, drawings, shapes, forms, crop, rotate, reorder,
  insert and remove pages, Bates numbers, watermarks, document properties, outline.
- **Sign**: a picture of your signature, or a digital certificate signature (.p12).
- **More**: OCR (Tesseract), compare two PDFs, combine and split, export pages as PDF or images, protect with a
  password, print. Undo and redo, find with regular expressions (in one tab or all), two pages side by side,
  presentation mode (F5).
- **Read**: line numbers in the margins as LaTeX's lineno sets them (through the document, per page or per
  paragraph); a preview of where a link leads when resting on it; Back and Forward (Alt+Left/Right, mouse side
  buttons) after following links; pinch to zoom on touchpads; the last session's tabs reopen at startup
  (`reopen_tabs`).
- **LaTeX**: open PDFs reload when they change on disk; SyncTeX jumps between the PDF and the source.
- **Other documents**: XPS, comics (CBZ), images, SVG, e-books (EPUB, MOBI, FB2) and Office files (DOCX, XLSX,
  PPTX) open like PDFs; editing one makes a PDF copy, saved next to the original.

## Install

Each release on the [releases page](../../releases) has a Linux AppImage and tarball, a macOS `.dmg`, a Windows
`.zip` and an Android APK, built on GitHub Actions when the release is published. Pull requests only build and
test the app on each platform (Actions > CI > Run workflow does the same for any branch).

### Linux

Both Linux downloads run on Ubuntu 24.04 and later, Arch and other distributions with glibc 2.39 or newer, and use
fontconfig and OpenGL from the system (installed on any desktop). Pick one:

- **AppImage**: `chmod +x PDF_Tools-x86_64.AppImage`, then run it.
- **Tarball**: unpack it anywhere:

  ```sh
  mkdir -p ~/.local/opt && tar xzf PDF-Tools-Linux-x86_64.tar.gz -C ~/.local/opt
  ln -s ~/.local/opt/pdf-tools/bin/pdf-tools ~/.local/bin/    # for the command line
  ```

Run it once with `--install-desktop` to add it to the applications menu and to "Open with" for PDFs
(`--uninstall-desktop` removes it; after moving the AppImage or the folder, run it again).

### macOS

The `.dmg` on the [releases page](../../releases) is for Apple Silicon Macs
with macOS 11 or later (on an Intel Mac, build it with `platform/macos/build.sh`).

1. Download `PDF-Tools.dmg`, open it and drag **PDF Tools** onto **Applications**. Eject the disk image.
2. Open the app the first time. It is not notarized by Apple, so macOS blocks the first start:
   - macOS 15 (Sequoia) and later: open PDF Tools from Applications, click **Done** on the warning, then go to
     **System Settings > Privacy & Security**, scroll to "PDF Tools was blocked", click **Open Anyway** and confirm
     with your password.
   - macOS 14 and earlier: Control-click (or right-click) PDF Tools in Applications, choose **Open**, then **Open**
     again.

   Or, in Terminal, clear the download flag once instead: `xattr -dr com.apple.quarantine "/Applications/PDF Tools.app"`.
3. To open PDFs with it by default: select a PDF in Finder, **File > Get Info**, under **Open with** pick PDF Tools,
   then **Change All...**.
4. For the command line (`pdf-tools info file.pdf`, ...), link the binary into your PATH:

   ```sh
   sudo mkdir -p /usr/local/bin
   sudo ln -sf "/Applications/PDF Tools.app/Contents/MacOS/pdf-tools" /usr/local/bin/pdf-tools
   ```

An update is the same: drag the new version onto Applications and replace the old one.

### Windows

`PDF-Tools-Windows.zip` is for 64-bit Windows 10 (version 1903) or 11.

1. Unpack it anywhere, e.g. to `%LOCALAPPDATA%\Programs`; the `PDF-Tools` folder holds `pdf-tools.exe`, the OCR
   data and the Visual C++ runtime. The program is not signed, so SmartScreen may warn on the first start: **More
   info**, then **Run anyway**.
2. `pdf-tools.exe --install-desktop` adds PDF Tools to "Open with" for PDFs (`--uninstall-desktop` removes it);
   to make it the default, pick it under **Open with > Choose another app** with **Always** ticked.
3. For the command line, add the folder to your PATH.

Settings are in `%APPDATA%\pdf-tools`, reading positions and window messages in `%LOCALAPPDATA%\pdf-tools`.

## Build

```sh
platform/linux/build.sh      # ~/.cache/pdf-tools/linux/out/PDF_Tools-<arch>.AppImage and PDF-Tools-Linux-<arch>.tar.gz
platform/macos/build.sh      # ~/Library/Caches/pdf-tools/macos/out/PDF-Tools.dmg
platform/android/build.sh    # ~/.cache/pdf-tools-native/android/out/pdf-tools.apk (details: platform/android/README.md)
platform/windows/build.sh    # in Git Bash: ~/.cache/pdf-tools/windows/out/PDF-Tools-Windows.zip
```

Each `build.sh` first runs the `deps.sh` next to it, which downloads and builds the third-party libraries and
skips what is already built; `build.sh check` builds and tests without packing (on Android: compiles the native
library, no APK).

The Windows script needs Visual Studio 2022 or 2026 with the C++ tools and Strawberry Perl, as on GitHub's
`windows-latest` runner; it builds MuPDF through its Visual Studio solution and everything else with MSVC.

The builds leave out MuPDF's CJK and Noto fallback fonts: text in a font a PDF does not embed is drawn with the
system's fonts (fontconfig, /system/fonts, the macOS and Windows font folders). `PT_FONTS=full` before any of the
scripts bundles them as well, for a larger program that needs no installed fonts.

On Linux the script builds one `pdf-tools` binary (`~/.cache/pdf-tools/linux/build/pdf-tools`), with its
dependencies linked in except fontconfig, OpenGL and the window system's libraries, and packs it with the English
OCR data, man page and icon as the AppImage and the tarball. Both run on the build machine's glibc or newer, so
release builds come from Ubuntu 24.04. To install the binary instead, with its man page, desktop entry, icon and OCR data:

```sh
platform/linux/build.sh install ~/.local
```

The scripts download and build the pinned dependencies ([MuPDF](https://mupdf.com) with
[Tesseract](https://github.com/tesseract-ocr/tesseract), [PCRE2](https://github.com/PCRE2Project/pcre2),
[OpenSSL](https://www.openssl.org), [raylib](https://www.raylib.com) and [raygui](https://github.com/raysan5/raygui))
and run the tests. On Debian/Ubuntu the Linux script needs `build-essential cmake pkg-config curl git libfontconfig-dev
libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libegl-dev libxkbcommon-dev
libwayland-dev wayland-protocols`.

With your own dependencies (found through pkg-config: `mupdf` 1.28, `libpcre2-8`,
[`fontconfig`](https://www.freedesktop.org/wiki/Software/fontconfig/), `libcrypto`; raylib 6.2 through
`CMAKE_PREFIX_PATH`):

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/raylib && cmake --build build && ctest --test-dir build
cmake --install build --prefix ~/.local
```

Without raylib, `pdf-tools` is built with the commands only.

## Usage

```sh
pdf-tools file.pdf                          # open in the window
pdf-tools redact in.pdf -o out.pdf --text "Jane Doe" --regex 'DE\d{20}'
pdf-tools info in.pdf
pdf-tools convert book.epub -o book.pdf     # any supported document to PDF
pdf-tools COMMAND --help
pdf-tools --version
```

Commands print one JSON object and exit with a meaningful code; `AGENTS.md` documents them. Others: `search`,
`render`, `annotate`, `draw`, `insert-text`, `insert-image`, `sign`, `bates`, `watermark`, `ocr`, `compare`,
`export-images`, `scrub`, `inspect`, `comments` (annotations with page and line numbers, as Markdown or JSON).

- Settings: `~/.config/pdf-tools/settings.toml`, on Windows `%APPDATA%\pdf-tools\settings.toml` (e.g.
  `editor_command = "code -g %f:%l"` for SyncTeX).
- OCR needs Tesseract's `LANG.traineddata` in `$TESSDATA_PREFIX`, `$XDG_DATA_HOME/pdf-tools/tessdata` or the
  system's data folders; the Linux tarball, the macOS app, the Windows zip and the APK include English.
- LaTeX: compile with `-synctex=1`. Ctrl+click in the PDF opens the source line in your editor;
  `pdf-tools --forward file.pdf LINE file.tex` shows a source line in the PDF.
- Interface size: View > Appearance > Interface size, or Ctrl+Alt++ / Ctrl+Alt+- (Ctrl+Alt+0: automatic); saved as
  `ui_scale` in the settings.
- Logging: `PDF_TOOLS_LOG=error|warn|info|debug`.

### Updates

At startup, at most once a day, the window asks GitHub for the latest release
(`https://api.github.com/repos/vamsikalagaturu/pdf-tools/releases/latest`, with a `pdf-tools/VERSION` user agent
and nothing else). When it is newer, a bar offers **Update**: it downloads that release's package for this install,
checks it against the SHA-256 digest GitHub lists for it, and replaces the AppImage, the tarball's folder, the macOS
app or (after the app closes) the Windows folder; on Android the system installer takes the APK. A
`build.sh install PREFIX` that you can write (e.g. `~/.local`) gets the tarball's files put over its own one by one
(the program, `share/pdf-tools`, the man page, the launcher entry and icon; nothing else in the prefix is touched,
and a launcher entry keeps its `Exec=` line). Installs the app cannot replace itself (a distro package, a prefix
such as `/usr/local` you may not write, a folder it may not write) get the release page instead. Menu > Check for updates... checks now; Menu > Check for updates at startup (`check_updates` in the
settings) turns the check off. Desktop downloads go through the system's `curl`. The commands never use the
network.

## Safety

Check redacted files before sharing them. A picture signature is not a cryptographic one; use a certificate for
that. Passwords given on the command line can show in process listings.

**License:** AGPL-3.0-or-later, as [MuPDF](https://mupdf.com/licensing).
