"""Per-user app launcher integration: the .desktop entry and icon behind `pdf-tools --install-desktop`."""

import os
import shutil
import sys
from importlib.resources import files
from pathlib import Path

DESKTOP_ID = "pdf-tools"


def data_dir():
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share")


def targets():
    """Where the launcher entry and the scalable icon go: (desktop file, icon file)."""
    base = data_dir()
    return base / "applications" / f"{DESKTOP_ID}.desktop", base / "icons" / "hicolor" / "scalable" / "apps" / f"{DESKTOP_ID}.svg"


def executable():
    # unresolved, so the entry follows uv's ~/.local/bin link across upgrades
    return shutil.which(DESKTOP_ID) or str(Path(sys.argv[0]).absolute())


def install():
    """Write the launcher entry and icon for the current user; returns the written paths."""
    entry, icon = targets()
    assets = files("pdf_tools") / "assets"
    exe = executable()
    quoted = f'"{exe}"' if " " in exe else exe
    text = assets.joinpath(f"{DESKTOP_ID}.desktop").read_text().replace(f"Exec={DESKTOP_ID} ", f"Exec={quoted} ")
    for path, content in ((entry, text), (icon, assets.joinpath("logo.svg").read_text())):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    return entry, icon


def uninstall():
    """Remove the launcher entry and icon; returns the removed paths."""
    removed = [path for path in targets() if path.exists()]
    for path in removed:
        path.unlink()
    return removed
