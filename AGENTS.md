# PDF Tools for agents

`pdf-tools` is a PDF viewer/editor with a window, plus commands that run without one. Use the
commands: they never prompt, print one JSON object on stdout, and exit with a meaningful code.
`pdf-tools --help` lists the commands, `pdf-tools COMMAND --help` a command's options; this
file (`pdf-tools help`) covers what those do not.

## Conventions

- Pages are 1-based; ranges look like `1-3,7` (`5-1` counts down). Coordinates are PDF points
  (1/72 inch) from the page's top-left corner.
- Output never overwrites the input: `-o` must be another file, also not a hard link to an input.
- An encrypted input needs `--password`; the output keeps that password and the input's
  encryption and permissions (`"owner_password_replaced": true` when the owner password was not
  given: the output gets a new random one; `"user_password_replaced": true` when only the owner
  password was given: it opens the output). `merge` takes `--password-for FILE=PW` per input,
  FILE spelled as on the command line, and writes an unencrypted output.
- To keep a password out of the process list, `--password-file PATH`, `--current-password-file
  PATH` and `--cert-password-file PATH` stand for `--password`, `--current-password` and
  `--cert-password` and read it from the file's first line.

Exit codes: `0` done, `1` error (`{"error": ...}`, also for a wrong or missing option:
`{"error": "usage: ..."}`), `2` redaction written but text was still found under a
redaction (see `text_left_under_redactions`), `3` nothing matched (nothing written). An
error after a command began its output (e.g. `split` failing at part 2) still prints one
object: what was done so far plus `"error"`. A first argument that is not a command, not an
option and not an existing file gets `{"error": "unknown command ..."}` and exit 1, never
the window.

## Behaviour worth knowing

- `redact` deletes the text, images and drawings under each match (not a box drawn on top)
  and removes hidden data from the result: metadata, attachments, scripts, hidden text
  (including invisible OCR layers) and thumbnails. Check `matches` and
  `text_left_under_redactions` in the output. `scrub` does the hidden-data part alone;
  `flatten` turns pages into images, leaving no text.
- `--text` and `--regex` (`redact`, `search`) ignore case unless `--case-sensitive`, and with
  `--ignore-accents` ignore accents (Strüber finds Struber); `--text` also matches across line
  breaks and hyphenation. `--regex` uses MuPDF's regular expressions (JavaScript syntax: `\d`,
  `\b`, `(?:)`, lookahead, no lookbehind) over each line of the page's text, so `^` and `$` are
  line ends.
- `pages`: `--order` keeps only the pages it lists, `--remove` drops pages after that, `--crop`
  deletes what lies outside the box.
- `fill` takes field names from `info` (`fields[].name`); checkboxes take `true`/`false`.
- `sign --cert` writes a PKCS#7 digital signature (incremental save); `sign --image` alone only
  places a picture, which is not a cryptographic signature.
- `bates` numbers count only the selected pages; the output gives `first` and `last`.
- `ocr` needs Tesseract's `LANG.traineddata` (in `--tessdata`, `$TESSDATA_PREFIX`,
  `$XDG_DATA_HOME/pdf-tools/tessdata` (Windows: `%LOCALAPPDATA%\pdf-tools\tessdata`), `tessdata`
  next to the program or the system's data folders); `characters_added` is 0 when the pages
  already had text.
- `comments` lists marked text (with the text under it), notes, text boxes, stamps, carets and
  any other annotation with a comment, each with its replies (`/IRT`), as
  `{"file", "numbering", "comments": [{"page", "lines": [first, last], "type", "author", "date",
  "quote", "comment", "replies": [...]}], "count"}`. Lines are numbered as the window's line
  numbers count them (`--numbering`: through the document or per page); `lines` is null on a
  page without text, `date` is ISO 8601 in UTC. With `-o` the list goes to that file (Markdown
  when it ends in `.md` or with `--format md`, else this JSON) and the output is
  `{"output", "count"}`. No comments: exit 3, nothing written.
- `update` installs the latest release when it is newer, as the window's Update does, and is
  the only command that uses the network (it needs `curl`). The output is `{"current", "latest",
  "update_available", "install", "path", "updated"}`, `"updated": false` with exit 0 when
  already up to date; `--check` only reports. Open windows keep the old version until restarted
  (`"restart_needed": true`). It refuses to replace a development build without `--force`, and
  an install it cannot replace (a distro package, a prefix the user may not write) fails with
  `release_page`. On Windows the folder is swapped just after the command exits, so an open
  window can still block it even though the output says `"updated": true`.
- Other formats, by file extension: XPS/OXPS, CBZ/CBT, images (PNG, JPEG, GIF, BMP, TIFF, JP2,
  JXR, PSD, PNM), SVG, EPUB, MOBI, FB2, DOCX, XLSX, PPTX. The commands that only read (`info`,
  `search`, `inspect`, `comments`, `render`, `export-images`, `compare`, `ocr`) take them as they
  are, e-books and Office documents laid out at A5, 11 pt; the commands that edit fail with
  `"... is not a PDF; convert it first: pdf-tools convert IN -o OUT.pdf"` (`merge` still takes
  images). `convert` keeps the text searchable, links and the outline. Office files come out as
  their text without the layout (MuPDF reads no slide or page layout), so the window does not
  open them; for the real layout use LibreOffice (`soffice --headless --convert-to pdf FILE`).

## The window

Edits made in the window (redactions, text, signatures, forms, page changes) only reach the
file when the user saves; do not expect them on disk. `pdf-tools file.pdf` opens a tab in the
window used last; on Windows `--install-desktop` also adds PDF Tools to "Open with" for PDFs.

## LaTeX syncing

- The window reloads an open PDF whenever the file changes on disk (keeping page, zoom and
  scroll), so `latexmk -pvc` or any rebuild shows up by itself. If the user has unsaved
  edits in that tab, it asks first.
- SyncTeX needs `-synctex=1` (latexmk: `-synctex=1`, or `$pdflatex = 'pdflatex -synctex=1 %O %S'`),
  which writes `name.synctex.gz` next to `name.pdf`. Without it, lookups report no data.
- PDF to source: Ctrl+click in the window runs the user's `editor_command`
  (`~/.config/pdf-tools/settings.toml`, Windows: `%APPDATA%\pdf-tools\settings.toml`; `%f` = file,
  `%l` = line, e.g. `code -g %f:%l`).
- Source to PDF: `pdf-tools --forward main.pdf LINE main.tex`; the running window jumps to that
  line and highlights it briefly (it starts one if none runs).
- Without the window, `pdf-tools synctex` answers the same questions as JSON:
  `--source chapters/intro.tex --line 42` gives `{"page", "rects"}`, `--page 3 --at 120,400`
  gives `{"source", "line"}`. Source paths match by full path, else by file name; a line that
  typesets nothing maps to the next line that does.
- Typical loop for "fix the paragraph on page 3": find its position (`search`), `synctex --page 3
  --at X,Y` for the source line, edit the `.tex`, rebuild with `-synctex=1`, then `--forward` to
  show the user the result.
