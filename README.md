# PDF Tools

PDF viewer and editor for Linux (PySide6 + PyMuPDF): redact (content is removed,
not covered), add text, sign, fill forms, mark and comment, search, merge, export.

## Install

Needs Python 3.11+ and [uv](https://docs.astral.sh/uv/).

```sh
uv tool install git+https://github.com/vamsikalagaturu/pdf-tools
pdf-tools --install-desktop   # app launcher entry + icon, for the current user
pdf-tools [file.pdf ...]
```

uv clones the repo, builds the package, installs it with its dependencies into an
isolated environment and puts `pdf-tools` in `~/.local/bin`. Pin a version with
`...pdf-tools@v0.1.0`; update with `uv tool upgrade pdf-tools`.

`--install-desktop` writes `~/.local/share/applications/pdf-tools.desktop` (pointing
at the installed `pdf-tools`, so it survives upgrades) and the icon. To remove
everything: `pdf-tools --uninstall-desktop && uv tool uninstall pdf-tools`.

From a checkout, `uv tool install --editable .` makes source edits apply live.

## Use

Settings: `~/.config/pdf-tools/settings.toml`. Tests: `uv run pytest`.

Scripts and agents: `pdf-tools info|redact|fill|pages|merge|split|protect|unprotect|flatten|synctex`
work without the window and print JSON; see [AGENTS.md](AGENTS.md) or `pdf-tools help`.

### LaTeX

Open PDFs reload when they change on disk (e.g. `latexmk -pvc`). Compile with
`-synctex=1`, then:

- **PDF to source:** Ctrl+click in the PDF opens that line in your editor; the command
  is asked on first use (`editor_command`, e.g. `code -g %f:%l`).
- **Source to PDF:** have your editor run `pdf-tools --forward file.pdf LINE file.tex`;
  the open window jumps there. For VS Code LaTeX Workshop, set
  `latex-workshop.view.pdf.external.synctex.command` to `pdf-tools` and its args to
  `["--forward", "%PDF%", "%LINE%", "%TEX%"]`.

## License

AGPL-3.0-or-later, see [LICENSE](LICENSE). Lucide icons are ISC
(`src/pdf_tools/assets/icons/LICENSE`).
