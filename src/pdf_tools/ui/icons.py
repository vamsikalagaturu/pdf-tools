"""Lucide icons (ISC, vendored in assets/icons) drawn in the palette's text colour and redrawn on theme change."""

from functools import lru_cache
from importlib.resources import files

from PySide6.QtCore import QByteArray, QRectF, Qt
from PySide6.QtGui import QIcon, QPainter, QPalette, QPixmap
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import QApplication

# app name -> Lucide icon file
LUCIDE = {
    "menu": "menu",
    "back": "chevron-left",
    "prev": "chevron-left",
    "next": "chevron-right",
    "close": "x",
    "undo": "undo-2",
    "redo": "redo-2",
    "view": "mouse-pointer-2",
    "redact": "eraser",
    "highlight": "highlighter",
    "note": "sticky-note",
    "text": "type",
    "sign": "signature",
    "zoom-out": "zoom-out",
    "zoom-in": "zoom-in",
    "fit": "move-horizontal",
    "continuous": "gallery-vertical",
    "find": "search",
    "save": "save",
    "open": "folder-open",
    "export": "file-output",
    "merge": "files",
    "add": "plus",
    "remove": "trash-2",
    "sort": "arrow-down-a-z",
    "underline": "underline",
    "strikeout": "strikethrough",
    "squiggly": "spline",
    "textcolour": "baseline",
    "custom-colour": "palette",
    "file": "file-text",
    "sidebar": "panel-left",
    "copy": "copy",
    "print": "printer",
    "about": "info",
}
SIZE = 48

_bound = []


@lru_cache(maxsize=None)
def svg_source(name):
    return (files("pdf_tools") / "assets" / "icons" / f"{LUCIDE[name]}.svg").read_text()


def pixmap(name, colour):
    svg = svg_source(name).replace("currentColor", colour)
    pm = QPixmap(SIZE, SIZE)
    pm.fill(Qt.transparent)
    painter = QPainter(pm)
    QSvgRenderer(QByteArray(svg.encode())).render(painter, QRectF(0, 0, SIZE, SIZE))
    painter.end()
    return pm


def icon(name):
    """Icon in the current text colour, white when its button is checked (sits on the highlight colour)."""
    palette = QApplication.palette()
    result = QIcon()
    result.addPixmap(pixmap(name, palette.color(QPalette.WindowText).name()), QIcon.Normal, QIcon.Off)
    result.addPixmap(pixmap(name, palette.color(QPalette.HighlightedText).name()), QIcon.Normal, QIcon.On)
    result.addPixmap(pixmap(name, palette.color(QPalette.Disabled, QPalette.WindowText).name()), QIcon.Disabled)
    return result


def bind(target, name):
    """Set `name` on an action or button and remember it so `refresh` can recolour it after a theme change."""
    target.setIcon(icon(name))
    _bound[:] = [(t, n) for t, n in _bound if t is not target]
    _bound.append((target, name))


def refresh():
    alive = []
    for target, name in _bound:
        try:
            target.setIcon(icon(name))
        except RuntimeError:
            continue
        alive.append((target, name))
    _bound[:] = alive
