# Native PDF Tools (C / MuPDF / raylib)

A **work-in-progress native port** of [pdf-tools](https://github.com/vamsikalagaturu/pdf-tools), isolated in `native/` on the `c-raylib-port` branch. The original Python/PySide6 application on `main` is unchanged. This is **not feature parity yet**.

- `src/pdf.c`: shared MuPDF C engine for PDF documents, rendering and changes.
- `src/cli.c`: headless PDF operations.
- `src/gui.c`: minimal desktop editing canvas built with raylib (optional when raylib is installed).
- `tests/`: page-range and PDF round-trip integration tests.

## Build

Install MuPDF headers and library (e.g. `libmupdf-dev`), a C11 compiler and CMake. Install raylib 5+ for the GUI.

```bash
cmake -S native -B build/native -DPT_BUILD_GUI=ON
cmake --build build/native -j
ctest --test-dir build/native --output-on-failure
```

Integration tests use PyMuPDF only for checking output files. The app itself has no Python runtime dependency.

If MuPDF is not discoverable, set `-DMUPDF_INCLUDE_DIR=/path/to/include -DMUPDF_LIBRARY=/path/to/libmupdf.so`.

## CLI

```bash
build/native/pdf-tools-c info input.pdf
build/native/pdf-tools-c merge -o combined.pdf a.pdf b.pdf
build/native/pdf-tools-c split input.pdf --every 2 -d parts
build/native/pdf-tools-c pages input.pdf -o reordered.pdf --order 3,1-2 --rotate 1:90
build/native/pdf-tools-c annotate input.pdf -o marked.pdf --page 1 --type highlight --rect 30,30,140,55
build/native/pdf-tools-c annotate input.pdf -o note.pdf --page 1 --type note --rect 40,40,50,50 --text "Check this"
build/native/pdf-tools-c annotate input.pdf -o text.pdf --page 1 --type text --rect 50,50,300,100 --text "Hello"
build/native/pdf-tools-c draw input.pdf -o ink.pdf --page 1 --points '30,40;60,80;120,45'
build/native/pdf-tools-c redact input.pdf -o clean.pdf --page 1 --rect 10,10,200,80
build/native/pdf-tools-c search input.pdf --text 'example'
build/native/pdf-tools-c render input.pdf -o preview.png --page 1 --dpi 144
build/native/pdf-tools-c protect input.pdf -o private.pdf --new-password '...'
build/native/pdf-tools-c unprotect private.pdf -o open.pdf --password '...'
```

For graphical editing, launch `build/native/pdf-tools-gui input.pdf`. Use the displayed shortcut keys and Save As path argument (`pdf-tools-gui input.pdf output.pdf`). Drawing, rectangle marks and ink are PDF objects (not rasterized). The GUI does not yet support forms, SyncTeX or full thumbnail/multitab UX.

## Feature status

| Feature | Native CLI | raylib UI |
|---|---|---|
| Open/view/zoom/scroll | Render | Basic single-page viewer |
| Merge/split/reorder/rotate | Yes | Not wired yet |
| Highlight/underline/strikeout/squiggly | Yes | Highlight |
| Notes / free-text annotations | Yes | Basic note/text placement |
| Freehand drawing | Yes | Pen |
| Apply redactions | Yes | Rectangle redaction |
| Search | Yes | Not yet |
| Password protect/unprotect | Yes | Not yet |
| Form filling | Yes | Not yet |
| Signatures/image insertion | Not yet | Not yet |
| Undo/redo, SyncTeX, multi-page continuous layout | Not yet | Not yet |

## Safety and licensing

Redaction applies the MuPDF redaction operation; it is **not** a colored rectangle overlay. Thorough information sanitization and redaction verification from the Python implementation are **not yet replicated**, so do not rely on the native port for sensitive-document redaction yet. It writes a separate output file rather than overwriting its input. Passwords passed on the command line can appear in process lists.

The port is licensed AGPL-3.0-or-later, consistent with the original project and MuPDF's AGPL distribution. See the parent repository's `LICENSE`.

**Platforms:** The portable engine can be compiled for Linux, macOS, Windows and Android, but the Android project/packaging and platform-native file pickers have not been ported yet. The raylib UI is initial desktop code, not a verified production-grade editor.
