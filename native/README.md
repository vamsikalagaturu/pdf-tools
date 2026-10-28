# Native PDF Tools — C / MuPDF / raylib

Native PDF-processing library and command-line interface. Developed in `native/`
(on the `c-raylib-port` branch) alongside the original PyMuPDF/PySide6 application.
The original application on `main` is unchanged. The backend can be reused by a
future desktop/mobile raylib UI; the existing raylib editor is only a prototype.

## Build and test

Requirements: C11, CMake >=3.20, MuPDF C development headers and library.
Optional: PCRE2 (regular-expression redaction), raylib (prototype GUI),
`synctex` executable (LaTeX source synchronization).

```sh
cmake -S native -B build/native -DPT_BUILD_GUI=OFF
cmake --build build/native -j
ctest --test-dir build/native --output-on-failure
```

If MuPDF is not found automatically, set `-DMUPDF_INCLUDE_DIR=...` and
`-DMUPDF_LIBRARY=...`. Library targets: `ptcore` (C API), `pdf-tools-c` (CLI).
All mutating CLI commands write a new file; never overwrite the original.

## Commands

```sh
pdf-tools-c info input.pdf
pdf-tools-c inspect input.pdf                           # annotations and form field names/values
pdf-tools-c search input.pdf --text 'hello'
pdf-tools-c merge -o out.pdf a.pdf:1-3 b.pdf scan.png  # selected pages or image sources
pdf-tools-c split input.pdf --every 2 -d parts
pdf-tools-c split input.pdf --at 3,7 -d parts
pdf-tools-c pages input.pdf -o out.pdf --order 3,1,2 --remove 2 --rotate 1:90
pdf-tools-c pages input.pdf -o out.pdf --crop 1:20,20,400,500
pdf-tools-c pages input.pdf -o out.pdf --crop-content 1-3
pdf-tools-c fill input.pdf -o filled.pdf --field name=Jane
pdf-tools-c annotate input.pdf -o out.pdf --page 1 --type highlight --rect 30,40,200,70
pdf-tools-c annotate input.pdf -o out.pdf --page 1 --type note --rect 30,40,60,70 --text 'Review'
pdf-tools-c edit-annotation input.pdf -o out.pdf --page 1 --index 0 --text 'Updated comment'
pdf-tools-c edit-annotation input.pdf -o out.pdf --page 1 --index 0 --remove
pdf-tools-c draw input.pdf -o out.pdf --page 1 --points '10,20;40,60;100,25'
pdf-tools-c redact input.pdf -o out.pdf --page 1 --rect 10,10,100,60
pdf-tools-c redact-text input.pdf -o out.pdf --text 'Secret'
pdf-tools-c redact-text input.pdf -o out.pdf --regex 'AB[0-9]+'    # when compiled with PCRE2
pdf-tools-c insert-text input.pdf -o out.pdf --page 1 --at 40,150 --text 'Hello' --size 18
pdf-tools-c insert-image input.pdf -o out.pdf --page 1 --image photo.png --rect 30,40,120,100
pdf-tools-c prepare-signature scan.jpg -o signature.png     # remove white background
pdf-tools-c sign input.pdf -o out.pdf --page 1 --image signature.png --rect 30,40,140,90
pdf-tools-c flatten input.pdf -o out.pdf --dpi 200          # rasterize pages
pdf-tools-c scrub input.pdf -o out.pdf                      # remove common hidden data
pdf-tools-c render input.pdf -o page.png --page 1 --dpi 144
pdf-tools-c protect input.pdf -o out.pdf --new-password '...'
pdf-tools-c unprotect input.pdf -o out.pdf --password '...'
pdf-tools-c synctex input.pdf --source chapter.tex --line 42
pdf-tools-c synctex input.pdf --page 2 --at 100,100
```

Numbers and page ranges are 1-based; annotation indices are 0-based. Coordinates
are points (1/72 inch) relative to the page's top-left corner. Output is JSON
where applicable. `--password` can unlock encrypted inputs.

## Functional coverage and limitations

**Implemented in the C core:** page rendering, text search, merge/split (including
selected pages and images), reorder/rotate/crop, crop-to-content, form values,
markup/free-text/note/shape/ink annotations, update/remove annotations, text and
regex redaction, direct ASCII text content insertion, image and visual signature
insertion, scanned signature conversion, password protection, flattening, common
metadata/script/attachment cleanup, and page/source SyncTeX lookups on desktop.
The merge and page operations preserve simple AcroForm widgets.

**Not yet at full parity:** rich Unicode and multi-line font-accurate text insertion;
editing existing PDF text objects; full PDF metadata/font inventory; image/text
object replacement; exact preservation of complex forms/signatures in every merge;
secure deletion outside crops; exhaustive forensic redaction sanitization;
platform-native Android/macOS file integrations; integrated SyncTeX parser (the
CLI currently calls external `synctex`); complete undo/redo/edit-state model;
and the production-quality UI.

**Redaction:** MuPDF removes content at redaction rectangles and saves with garbage
collection. The scrub routine additionally removes common metadata, embedded-file,
script, and thumbnail references. This is **not** a guarantee that every hidden or
historical data source was removed. Verify results before handling sensitive PDFs.

**Signatures:** `sign` places a visual signature image. It does not create or
verify cryptographic PDF signatures. `flatten` is rasterization (discards searchable
text and form interactivity). The crop commands currently change page visibility;
content outside the box may still exist in the PDF. Password arguments can be
visible in process listings.

**License:** AGPL-3.0-or-later (same as the parent repository and MuPDF's AGPL use).
