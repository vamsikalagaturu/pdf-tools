import os
import shutil
import tomllib
from pathlib import Path

APP_DIR = "pdf-tools"
LEGACY_DIR = "pdf-redact"
# key: (default, comment written above it in settings.toml)
SETTINGS = {
    "theme": ("dark", "dark | light"),
    "view_mode": ("continuous", "continuous | single"),
    "show_sidebar": (True, "Show the Pages / Annotations / Edits panel"),
    "export_flatten": (False, "Export pages as images without selectable text"),
    "export_dpi": (200, "Resolution of flattened exports"),
    "redact_snap_to_text": (True, "Redact whole lines of the words dragged over instead of a free rectangle"),
    "redact_auto_fill": (True, "Fill redactions with the colour sampled around them"),
    "highlight_colour": ("#ffe45c", "Colour of new background highlights"),
    "ink_colour": ("#c62828", "Colour of new underline / strikethrough / squiggly / text-colour marks"),
    "highlight_style": ("highlight", "highlight | underline | strikeout | squiggly | textcolour"),
    "last_folder": ("", "Folder the Open dialog starts in"),
    "last_signature": ("", "Signature preselected in the Sign dialog"),
    "recent_files": ([], "Recently opened PDFs, newest first"),
}
RECENT_LIMIT = 10


def config_dir():
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / APP_DIR


def legacy_dir():
    return config_dir().parent / LEGACY_DIR


def toml_value(value):
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(v) for v in value) + "]"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value)
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


class Settings:
    """App settings in a commented, hand-editable ~/.config/pdf-tools/settings.toml."""

    def __init__(self, path=None):
        self.path = Path(path) if path else config_dir() / "settings.toml"
        self.values = {key: default for key, (default, _) in SETTINGS.items()}
        try:
            loaded = tomllib.loads(self.path.read_text())
        except (FileNotFoundError, tomllib.TOMLDecodeError):
            loaded = None
        for key, value in (loaded or {}).items():
            default = SETTINGS.get(key, (None,))[0]
            if default is None or type(value) is not type(default):
                continue
            if isinstance(value, list) and not all(isinstance(v, str) for v in value):
                continue
            self.values[key] = value
        if loaded is None:
            self.save()

    def get(self, key):
        return self.values[key]

    def set(self, key, value):
        if self.values.get(key) != value:
            self.values[key] = value
            self.save()

    def add_recent(self, path):
        path = str(path)
        self.set("recent_files", [path] + [p for p in self.values["recent_files"] if p != path][: RECENT_LIMIT - 1])

    def save(self):
        lines = ["# PDF Tools settings. Edit while the app is closed; unknown keys and wrong types are ignored.", ""]
        for key, (default, comment) in SETTINGS.items():
            lines += [f"# {comment} (default: {toml_value(default)})", f"{key} = {toml_value(self.values[key])}", ""]
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text("\n".join(lines))


def migrate_legacy():
    """Copy signatures from the old ~/.config/pdf-redact once; the old folder is left in place."""
    old, new = legacy_dir() / "signatures", config_dir() / "signatures"
    if old.is_dir() and not new.exists():
        shutil.copytree(old, new)
