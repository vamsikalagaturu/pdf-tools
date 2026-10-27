# PDF Tools

PDF viewer and editor for Linux (PySide6 + PyMuPDF): redact (content is removed,
not covered), add text, sign, fill forms, mark and comment, search, merge, export.

## Install

Needs Python 3.11+ and [uv](https://docs.astral.sh/uv/).

```sh
uv tool install git+https://github.com/vamsikalagaturu/pdf-tools
pdf-tools [file.pdf ...]
```

uv clones the repo, builds the package, installs it with its dependencies into an
isolated environment and puts `pdf-tools` in `~/.local/bin`. Pin a version with
`...pdf-tools@v0.1.0`; update with `uv tool upgrade pdf-tools`, remove with
`uv tool uninstall pdf-tools`.

From a checkout, `uv tool install --editable .` makes source edits apply live.

To add it to your app launcher:

```sh
curl -fsSLo ~/.local/share/applications/pdf-tools.desktop --create-dirs \
  https://raw.githubusercontent.com/vamsikalagaturu/pdf-tools/main/data/pdf-tools.desktop
curl -fsSLo ~/.local/share/icons/hicolor/scalable/apps/pdf-tools.svg --create-dirs \
  https://raw.githubusercontent.com/vamsikalagaturu/pdf-tools/main/src/pdf_tools/assets/logo.svg
```

## Use

Settings: `~/.config/pdf-tools/settings.toml`. Tests: `uv run pytest`.

## License

AGPL-3.0-or-later, see [LICENSE](LICENSE). Lucide icons are ISC
(`src/pdf_tools/assets/icons/LICENSE`).
