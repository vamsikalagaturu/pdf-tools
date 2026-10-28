"""Lucide icons (ISC, vendored in assets/icons) drawn in the palette's text colour and redrawn on theme change,
and the app's mouse cursors, drawn from the same icons so they do not depend on the desktop's cursor theme."""

from functools import lru_cache
from importlib.resources import files

from PySide6.QtCore import QByteArray, QEvent, QObject, QRectF, Qt
from PySide6.QtGui import QCursor, QIcon, QPainter, QPalette, QPixmap
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import QApplication, QWidget

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
    "rotate": "rotate-cw",
    "rotate-left": "rotate-ccw",
    "crop": "crop",
    "lock": "lock",
    "unlock": "lock-open",
    "split": "scissors",
    "bold": "bold",
    "italic": "italic",
    "image": "image",
    "draw": "pencil",
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
    """Recolour every bound icon for the current palette, forgetting targets Qt has deleted meanwhile."""
    alive = []
    for target, name in _bound:
        try:
            target.setIcon(icon(name))
        except RuntimeError:
            continue
        alive.append((target, name))
    _bound[:] = alive


# Qt cursor shape -> (Lucide file, hotspot in the icon's 24x24 grid[, filled[, turn]])
CURSORS = {
    # Lucide's arrowhead is symmetric about the diagonal; turned 24.4 degrees about its tip its left edge stands
    # upright and the other edge lies at ~44 degrees, the classic pointer shape
    Qt.ArrowCursor: ("mouse-pointer-2", (4.3, 4.3), True, 24.4),
    Qt.IBeamCursor: ("text-cursor", (12, 12)),
    Qt.CrossCursor: ("crosshair", (12, 12)),
    Qt.PointingHandCursor: ("pointer", (8, 2)),
    Qt.OpenHandCursor: ("hand", (12, 12)),
    Qt.ClosedHandCursor: ("grab", (12, 12)),
    Qt.SizeAllCursor: ("move", (12, 12)),
    Qt.SizeFDiagCursor: ("move-diagonal-2", (12, 12)),
    Qt.SizeBDiagCursor: ("move-diagonal", (12, 12)),
    Qt.SizeHorCursor: ("move-horizontal", (12, 12)),
    Qt.SplitHCursor: ("move-horizontal", (12, 12)),
    Qt.SizeVerCursor: ("move-vertical", (12, 12)),
    Qt.SplitVCursor: ("move-vertical", (12, 12)),
    Qt.ForbiddenCursor: ("ban", (12, 12)),
    Qt.WaitCursor: ("loader-circle", (12, 12)),
    Qt.BusyCursor: ("loader-circle", (12, 12)),
}
# app cursors that are not a Qt shape
PEN_CURSOR = ("pencil", (2, 22))
CURSOR_PX = 24


@lru_cache(maxsize=None)
def cursor(spec, ratio=1.0):
    """A cursor drawn from a Lucide icon: a black line with a white outline, readable on any background and
    independent of the desktop's cursor theme. `spec` is a Qt cursor shape or a (file, hotspot[, filled[, turn]])
    tuple; a filled one (the arrow) is solid black inside its outline, and `turn` rotates the icon (degrees,
    clockwise) about its hotspot, which then sits at (4, 3)."""
    file, (hx, hy), *extra = CURSORS[spec] if spec in CURSORS else spec
    filled, turn = (list(extra) + [False, 0])[:2]
    source = (files("pdf_tools") / "assets" / "icons" / f"{file}.svg").read_text()
    if filled:
        source = source.replace('fill="none"', 'fill="#000000"')
    size = round(CURSOR_PX * ratio)
    scale = size / 24
    pm = QPixmap(size, size)
    pm.fill(Qt.transparent)
    painter = QPainter(pm)
    painter.setRenderHint(QPainter.Antialiasing)
    if turn:
        # the hotspot moves to (4, 3) so the turned shape and its outline stay inside the cursor image
        painter.translate(4 * scale, 3 * scale)
        painter.rotate(turn)
        painter.translate(-hx * scale, -hy * scale)
        hx, hy = 4, 3
    # the icon's own 2-unit stroke, laid over a wider white one that gives the outline
    for colour, width in (("#ffffff", "4.5"), ("#000000", "2")):
        svg = source.replace("currentColor", colour).replace('stroke-width="2"', f'stroke-width="{width}"')
        QSvgRenderer(QByteArray(svg.encode())).render(painter, QRectF(0, 0, size, size))
    painter.end()
    pm.setDevicePixelRatio(ratio)
    return QCursor(pm, round(hx * CURSOR_PX / 24), round(hy * CURSOR_PX / 24))


class AppCursors(QObject):
    """Application-wide event filter that swaps every standard Qt cursor shape for the app's own (see `cursor`):
    those set by Qt widgets themselves (text fields, splitters, the page tools) and the default arrow of each
    top-level window, so no cursor depends on the desktop's cursor theme."""

    def eventFilter(self, obj, event):
        if isinstance(obj, QWidget):
            kind = event.type()
            if kind == QEvent.CursorChange and obj.testAttribute(Qt.WA_SetCursor):
                self.replace(obj)
            elif kind == QEvent.Show and obj.isWindow() and not obj.testAttribute(Qt.WA_SetCursor):
                obj.setCursor(Qt.ArrowCursor)
        return False

    @staticmethod
    def replace(widget):
        shape = widget.cursor().shape()
        # a bitmap cursor is already ours (or a deliberate custom one); setting it again re-sends CursorChange
        if shape != Qt.BitmapCursor and shape in CURSORS:
            widget.setCursor(cursor(shape, widget.devicePixelRatioF()))


def install_cursors(app):
    """Use the app's own cursors from now on (call once, before windows are shown)."""
    app._cursors = AppCursors(app)
    app.installEventFilter(app._cursors)
