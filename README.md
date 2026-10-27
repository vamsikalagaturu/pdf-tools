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

## License

AGPL-3.0-or-later, see [LICENSE](LICENSE). Lucide icons are ISC
(`src/pdf_tools/assets/icons/LICENSE`).
