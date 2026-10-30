# PDF Tools for agents

`pdf-tools` is a PDF viewer/editor with a window, plus commands that run without one. Use the
commands: they never prompt, print one JSON object on stdout, and exit with a meaningful code.
`pdf-tools help` prints this file; `pdf-tools COMMAND --help` lists a command's options.

Conventions: pages are 1-based; ranges look like `1-3,7`; coordinates are PDF points
(1/72 inch) measured from the page's top-left corner; output never overwrites the input
(`-o` must be another file). An encrypted input needs `--password`, and the output keeps
that password.

Exit codes: `0` done, `1` error (`{"error": ...}`), `2` redaction written but text was
still found under a redaction (see `text_left_under_redactions`), `3` nothing matched
(nothing written).

## Commands

```sh
pdf-tools info in.pdf                     # pages, sizes, fonts, form fields, annotations, metadata, encrypted, synctex
pdf-tools redact in.pdf -o out.pdf --text "Jane Doe" --regex 'DE\d{20}' [--pages 1-3] [--case-sensitive] [--fill '#000000']
pdf-tools fill in.pdf -o out.pdf --field name=Jane --field agree=true
pdf-tools pages in.pdf -o out.pdf [--order 3,1,2] [--remove 4-5] [--rotate 2:90] [--crop 1:x0,y0,x1,y1] [--crop-content [PAGES]]
pdf-tools merge a.pdf b.pdf:1-3 scan.jpg -o out.pdf          # images become pages
pdf-tools split in.pdf (--every 2 | --at 3,7) -d outdir/      # outdir/in-part1.pdf, ...
pdf-tools protect in.pdf -o out.pdf --password NEW [--current-password OLD]
pdf-tools unprotect in.pdf -o out.pdf --password PW
pdf-tools flatten in.pdf -o out.pdf [--dpi 200]               # pages become images, no text left
pdf-tools synctex in.pdf --page 2 --at 150,133                # -> {"source": ..., "line": ...}
pdf-tools synctex in.pdf --source main.tex --line 11          # -> {"page": ..., "rects": [[x0,y0,x1,y1]]}
```

Notes:

- `redact` deletes the text, images and drawings under each match (not a box drawn on top)
  and removes hidden data from the result: metadata, attachments, scripts, hidden text
  (including invisible OCR layers) and thumbnails. Check `matches` and
  `text_left_under_redactions` in the output.
- `--text` is matched without regard to case unless `--case-sensitive`; `--regex` uses
  PCRE2 (Perl-compatible) syntax over each page's text in reading order.
- `pages`: `--order` lists the pages to keep in their new order (`5-1` counts down);
  `--remove` drops pages after that; `--crop` deletes what lies outside the box.
- Field names for `fill` come from `info` (`fields[].name`); checkboxes take `true`/`false`.

## More commands

```sh
pdf-tools search in.pdf --text "total" [--pages 1-3] [--case-sensitive]   # -> pages and boxes of each match
pdf-tools inspect in.pdf                                     # annotations (with per-page index) and form fields
pdf-tools render in.pdf -o page.png [--page 1] [--dpi 150]
pdf-tools export-images in.pdf -d outdir/ [--pages 1-3] [--format png|jpeg] [--dpi 150]   # outdir/in-page1.png, ...
pdf-tools ocr in.pdf -o out.pdf [--pages 1-3] [--language deu+eng] [--tessdata DIR]       # invisible, searchable text layer
pdf-tools compare a.pdf b.pdf [--password2 PW]               # -> per page: boxes only_in_first / only_in_second
pdf-tools bates in.pdf -o out.pdf [--prefix ABC] [--suffix X] [--start 1] [--digits 6] [--position bottom-right] [--pages 1-3]
pdf-tools watermark in.pdf -o out.pdf --text CONFIDENTIAL [--position diagonal] [--opacity 0.3] [--pages 1-3]
pdf-tools annotate in.pdf -o out.pdf --page 1 --type highlight --rect x0,y0,x1,y1 [--text "comment"] [--color '#ffd400']
pdf-tools draw in.pdf -o out.pdf --page 1 --points '10,10;50,60;90,20' [--width 2]
pdf-tools insert-text in.pdf -o out.pdf --page 1 --at 72,72 --text "Hello" [--size 11] [--font Helvetica] [--width 300]
pdf-tools insert-image in.pdf -o out.pdf --page 1 --image logo.png --rect x0,y0,x1,y1
pdf-tools sign in.pdf -o out.pdf --page 1 --rect x0,y0,x1,y1 --image signature.png
pdf-tools sign in.pdf -o out.pdf --page 1 --rect x0,y0,x1,y1 --cert me.p12 --cert-password PW [--reason "Approved"]
pdf-tools scrub in.pdf -o out.pdf                            # metadata, attachments, scripts, hidden text, thumbnails
```

- `ocr` needs Tesseract's `LANG.traineddata` (in `--tessdata`, `$TESSDATA_PREFIX`,
  `$XDG_DATA_HOME/pdf-tools/tessdata` or the system's data folders); `characters_added` in
  the output is 0 when the pages already had text.
- `sign --cert` writes a PKCS#7 digital signature (incremental save); `--image` alone only
  places a picture, which is not a cryptographic signature.
- `bates` numbers count only the selected pages; the output gives `first` and `last`.

## Windowed use

```sh
pdf-tools file.pdf ...                         # opens tabs in the window used last (starts the app if needed)
pdf-tools --new-window file.pdf ...            # opens them in a new window
pdf-tools --forward file.pdf LINE source.tex   # scrolls the window showing that PDF to the source line
```

Edits made in the window (redactions, text, signatures, forms, page changes) only reach the
file when the user saves; do not expect them on disk.

## LaTeX syncing

The window follows a LaTeX build:

- It reloads an open PDF whenever the file changes on disk (keeping page, zoom and
  scroll), so `latexmk -pvc` or any rebuild shows up by itself. If the user has unsaved
  edits in that tab, it asks first.
- SyncTeX needs `-synctex=1` (latexmk: `-synctex=1`, or `$pdflatex = 'pdflatex -synctex=1 %O %S'`),
  which writes `name.synctex.gz` next to `name.pdf`. Without it, lookups report no data.
- PDF to source: Ctrl+click in the window runs the user's `editor_command`
  (`~/.config/pdf-tools/settings.toml`, `%f` = file, `%l` = line, e.g. `code -g %f:%l`).
- Source to PDF: run `pdf-tools --forward main.pdf LINE main.tex`; the running window jumps
  to that line and highlights it briefly. Without a running window it starts one.
- Without the window, `pdf-tools synctex` answers the same questions as JSON, e.g. to find
  which source line produced text on a page, or which page a line lands on:

  ```sh
  pdf-tools synctex main.pdf --source chapters/intro.tex --line 42
  pdf-tools synctex main.pdf --page 3 --at 120,400
  ```

  Source paths match by full path, else by file name. A line that typesets nothing maps
  to the next line that does.
- Typical agent loop for "fix the paragraph on page 3": get its position (e.g. from
  `info`/text extraction), `synctex --page 3 --at X,Y` to find the source line, edit the
  `.tex`, rebuild with `-synctex=1`, then `--forward` to show the user the result.
