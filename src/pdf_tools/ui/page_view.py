"""The page canvas of a document tab and the items drawn on it.

PageView turns mouse input into selections, bands, strokes and clicks for EditTab; ObjItem shows one placed edit
(or an existing PDF annotation) and lets it be moved, resized and right-clicked; TextEditItem is a text box being
typed. Scene coordinates are PDF points of the laid-out pages; EditTab converts them to page coordinates.
"""

from PySide6.QtCore import QPoint, QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QBrush, QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QGraphicsItem,
    QGraphicsProxyWidget,
    QGraphicsRectItem,
    QGraphicsScene,
    QGraphicsTextItem,
    QGraphicsView,
)

from pdf_tools.ui import icons

HANDLE_PX = 10
CLICK_SLOP_PX = 4
MOVABLE = {"redact", "text", "sign", "image", "note", "crop", "ink"}
RESIZABLE = {"redact", "sign", "image", "crop"}
KEEP_ASPECT = {"sign", "image"}
EDITABLE = {"text"}
CURSORS = {
    "view": Qt.IBeamCursor,
    "redact": Qt.CrossCursor,
    "highlight": Qt.IBeamCursor,
    "note": Qt.PointingHandCursor,
    "text": Qt.IBeamCursor,
    "sign": Qt.CrossCursor,
    "image": Qt.CrossCursor,
    "crop": Qt.CrossCursor,
    "draw": Qt.CrossCursor,
}


class ObjItem(QGraphicsRectItem):
    """An edit on page `pno` (redaction, text, signature, image, mark, note, drawing, crop frame) or an existing
    PDF annotation; `obj` is the edit's dict in EditTab.items, shared, not copied."""

    def __init__(self, editor, obj, pno, rect, image=None, parts=()):
        """`rect` is the scene rect; `image` the rendered preview (text, signature, image); `parts` the line rects
        of a mark or the strokes of a drawing, in item coordinates."""
        super().__init__(0, 0, rect.width(), rect.height())
        self.editor, self.obj, self.pno, self.image, self.parts = editor, obj, pno, image, parts
        self.kind = obj["kind"]
        self.setPos(rect.topLeft())
        flags = QGraphicsItem.ItemIsSelectable
        if self.kind in MOVABLE:
            flags |= QGraphicsItem.ItemIsMovable
        self.setFlags(flags)
        self.setAcceptHoverEvents(True)
        self.setZValue(1)
        self.setToolTip(obj.get("text") or obj.get("label") or "")
        self.resizing = False
        self.hovered = False

    def resizable(self):
        return self.kind in RESIZABLE and (self.isSelected() or self.kind == "crop")

    def handle_rect(self):
        """The bottom-right resize handle, HANDLE_PX screen pixels whatever the zoom."""
        s = HANDLE_PX / self.scene().views()[0].transform().m11()
        r = self.rect()
        return QRectF(r.right() - s, r.bottom() - s, s, s)

    def paint(self, p, option, widget=None):
        """Draw the edit as it will look when saved, plus a dashed frame (and handle) when selected or hovered."""
        r = self.rect()
        if self.kind == "redact":
            p.fillRect(r, QColor(*self.obj["rgb"]))
            p.setPen(QPen(QColor("#d93025"), 0, Qt.DashLine))
            p.drawRect(r)
        elif self.kind == "highlight":
            self.paint_mark(p, self.obj.get("style", "highlight"), QColor(*self.obj["rgb"]))
        elif self.kind == "note":
            p.setPen(QPen(QColor("#8a6d00"), 0))
            p.setBrush(QColor("#ffd54f"))
            p.drawRoundedRect(r, 2, 2)
            for i in (1, 2, 3):
                y = r.top() + r.height() * i / 4.5
                p.drawLine(QPointF(r.left() + 3, y), QPointF(r.right() - 3, y))
        elif self.kind == "ink":
            p.setPen(QPen(QColor(*self.obj["rgb"]), self.obj["width"], Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
            p.setBrush(Qt.NoBrush)
            for stroke in self.parts:
                p.drawPolyline(stroke)
        elif self.kind == "crop":
            p.setPen(QPen(QColor("#1a73e8"), 0, Qt.DashLine))
            p.setBrush(Qt.NoBrush)
            p.drawRect(r)
            p.fillRect(self.handle_rect(), QColor("#1a73e8"))
            return
        elif self.image is not None:
            p.drawImage(r, self.image)
        if self.isSelected() or (self.kind == "annot" and self.hovered):
            p.setPen(QPen(QColor("#1a73e8"), 0, Qt.DashLine))
            p.setBrush(Qt.NoBrush)
            p.drawRect(r)
            if self.resizable():
                p.fillRect(self.handle_rect(), QColor("#1a73e8"))

    def paint_mark(self, p, style, colour):
        """Preview a text markup the way the saved annotation renders (fill, line or wave per line rect)."""
        if style in ("highlight", "textcolour"):
            # Multiply darkens like a highlighter; Lighten recolours dark glyphs only (the PDF uses the same blend)
            mode = QPainter.CompositionMode_Lighten if style == "textcolour" else QPainter.CompositionMode_Multiply
            p.setCompositionMode(mode)
            for part in self.parts:
                p.fillRect(part, colour)
            p.setCompositionMode(QPainter.CompositionMode_SourceOver)
            return
        for part in self.parts:
            h = part.height()
            p.setPen(QPen(colour, max(h * 0.07, 0.6), Qt.SolidLine, Qt.RoundCap))
            if style == "underline":
                y = part.bottom() - h * 0.08
                p.drawLine(QPointF(part.left(), y), QPointF(part.right(), y))
            elif style == "strikeout":
                y = part.top() + h * 0.55
                p.drawLine(QPointF(part.left(), y), QPointF(part.right(), y))
            else:
                step, amp, y = h * 0.18, h * 0.06, part.bottom() - h * 0.1
                path = QPainterPath(QPointF(part.left(), y))
                x, up = part.left(), True
                while x < part.right():
                    x = min(x + step, part.right())
                    path.lineTo(QPointF(x, y - amp if up else y + amp))
                    up = not up
                p.setBrush(Qt.NoBrush)
                p.drawPath(path)

    def hoverEnterEvent(self, e):
        self.hovered = True
        self.update()

    def hoverLeaveEvent(self, e):
        self.hovered = False
        self.update()

    def hoverMoveEvent(self, e):
        on_handle = self.resizable() and self.handle_rect().contains(e.pos())
        self.setCursor(Qt.SizeFDiagCursor if on_handle else Qt.SizeAllCursor if self.kind in MOVABLE else Qt.ArrowCursor)

    def mousePressEvent(self, e):
        """Start a move or (on the handle) a resize; EditTab snapshots the state for undo."""
        self.editor.begin_change()
        self.resizing = self.resizable() and self.handle_rect().contains(e.pos())
        self.press, self.start = e.pos(), QRectF(self.rect())
        super().mousePressEvent(e)

    def mouseMoveEvent(self, e):
        """Resize from the handle (signatures and images keep their aspect ratio); otherwise Qt moves the item."""
        if not self.resizing:
            return super().mouseMoveEvent(e)
        w = max(4, self.start.width() + e.pos().x() - self.press.x())
        h = max(4, self.start.height() + e.pos().y() - self.press.y())
        if self.kind in KEEP_ASPECT:
            h = w * self.start.height() / self.start.width()
        self.setRect(0, 0, w, h)

    def mouseReleaseEvent(self, e):
        """Hand the new geometry (or a plain click) to EditTab.end_change, which records it."""
        super().mouseReleaseEvent(e)
        self.resizing = False
        self.editor.end_change(self)

    def contextMenuEvent(self, e):
        # deferred: most entries re-render the scene, which deletes this item
        obj, pno, kind, pos = self.obj, self.pno, self.kind, e.screenPos()
        QTimer.singleShot(0, lambda: self.editor.item_menu(obj, pno, kind, pos))

    def mouseDoubleClickEvent(self, e):
        # deferred: editing re-renders the scene, which deletes this item
        if self.kind in EDITABLE:
            QTimer.singleShot(0, lambda obj=self.obj: self.editor.edit_obj(obj))


class TextEditItem(QGraphicsTextItem):
    """A text box being typed on the page; `finished` fires on Esc, Ctrl+Enter or when focus leaves it and `bar`."""

    finished = Signal()

    def __init__(self, bar_has_focus):
        """`bar_has_focus()` tells whether the format bar holds focus, which does not count as leaving."""
        super().__init__()
        self.bar_has_focus = bar_has_focus
        self.setTextInteractionFlags(Qt.TextEditorInteraction)
        self.document().setDocumentMargin(0)
        self.setZValue(4)

    def paint(self, p, option, widget=None):
        super().paint(p, option, widget)
        p.setPen(QPen(QColor("#1a73e8"), 0, Qt.DashLine))
        p.setBrush(Qt.NoBrush)
        r = self.boundingRect()
        p.drawRect(r.adjusted(-2, -2, 2, 2) if r.width() > 4 else QRectF(-2, -2, 12, r.height() + 4))

    def keyPressEvent(self, e):
        if e.key() == Qt.Key_Escape or (e.key() in (Qt.Key_Return, Qt.Key_Enter) and e.modifiers() & Qt.ControlModifier):
            self.finished.emit()
            return
        super().keyPressEvent(e)

    def focusOutEvent(self, e):
        """Losing focus may finish the box; decided once focus has settled (check_focus)."""
        super().focusOutEvent(e)
        # deferred: focus has not reached its new owner yet
        QTimer.singleShot(0, self.check_focus)

    def check_focus(self):
        """Finish unless focus came back here or went to the format bar."""
        if self.scene() is not None and not self.hasFocus() and not self.bar_has_focus():
            self.finished.emit()


class PageView(QGraphicsView):
    """Page canvas. A drag on empty page becomes a rectangle band, a text-flow selection or a pen stroke, as
    `selection_mode` decides; the results leave as signals (scene coordinates) for EditTab to act on.

    Ctrl+click is reserved for SyncTeX (sync_clicked) and Ctrl+wheel for zoom (zoom_step).
    """

    pressed = Signal()
    context_requested = Signal(QPointF, QPoint)
    band_selected = Signal(QRectF)
    flow_changed = Signal(QPointF, QPointF, bool)
    clicked = Signal(QPointF)
    zoom_step = Signal(float)
    sync_clicked = Signal(QPointF)
    stroke_drawn = Signal(list)

    def __init__(self, tool, selection_mode):
        """`tool()` names the active tool (for the cursor); `selection_mode(modifiers)` returns "flow", "rect",
        "draw" or None for what a drag does."""
        super().__init__()
        self.tool = tool
        self.selection_mode = selection_mode
        self.setScene(QGraphicsScene(self))
        self.setBackgroundBrush(QColor("#5f6368"))
        self.setRenderHints(QPainter.Antialiasing | QPainter.SmoothPixmapTransform)
        self.setAlignment(Qt.AlignHCenter | Qt.AlignTop)
        # embedded form widgets leave stale pixels with partial (scroll-blit) updates
        self.setViewportUpdateMode(QGraphicsView.FullViewportUpdate)
        self.origin = None
        self.press_pos = None
        self.band = None
        self.flowing = False
        self.stroke = None
        self.pen = QPen(Qt.black, 2, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin)
        self.update_cursor()

    def update_cursor(self):
        tool = self.tool()
        if tool == "draw":
            self.viewport().setCursor(icons.cursor(icons.PEN_CURSOR, self.devicePixelRatioF()))
        else:
            self.viewport().setCursor(CURSORS[tool])

    def on_object(self, pos):
        """Whether view position `pos` is on something that handles the mouse itself (an edit, a form field or
        the text box being typed) rather than on bare page."""
        return any(isinstance(i, (ObjItem, QGraphicsProxyWidget, TextEditItem)) for i in self.items(pos))

    def mousePressEvent(self, e):
        """Start a band, a stroke or a flow selection on bare page; leave presses on objects to the scene."""
        pos = e.position().toPoint()
        if e.button() == Qt.LeftButton and e.modifiers() & Qt.ControlModifier:
            self.sync_clicked.emit(self.mapToScene(pos))
            return
        if e.button() == Qt.LeftButton:
            self.pressed.emit()
        if e.button() != Qt.LeftButton or self.on_object(pos):
            return super().mousePressEvent(e)
        self.scene().clearSelection()
        if self.scene().focusItem():
            self.scene().setFocusItem(None)
        self.origin, self.press_pos = self.mapToScene(pos), pos
        mode = self.selection_mode(e.modifiers())
        if mode == "rect":
            colour = QColor(26, 115, 232)
            fill = QColor(colour)
            fill.setAlpha(40)
            self.band = self.scene().addRect(QRectF(self.origin, self.origin), QPen(colour, 0, Qt.DashLine), QBrush(fill))
            self.band.setZValue(3)
        elif mode == "draw":
            self.stroke = [self.origin]
            self.stroke_item = self.scene().addPath(QPainterPath(self.origin), self.pen)
            self.stroke_item.setZValue(3)
        self.flowing = mode == "flow"

    def moved(self, pos):
        return (pos - self.press_pos).manhattanLength() > CLICK_SLOP_PX

    def mouseMoveEvent(self, e):
        """Grow the stroke or band, or report the flow selection so far (flow_changed with done=False)."""
        pos = e.position().toPoint()
        if self.stroke is not None:
            self.stroke.append(self.mapToScene(pos))
            path = self.stroke_item.path()
            path.lineTo(self.stroke[-1])
            self.stroke_item.setPath(path)
        elif self.band:
            self.band.setRect(QRectF(self.origin, self.mapToScene(pos)).normalized())
        elif self.flowing and self.moved(pos):
            self.flow_changed.emit(self.origin, self.mapToScene(pos), False)
        else:
            super().mouseMoveEvent(e)

    def mouseReleaseEvent(self, e):
        """Finish the gesture: emit stroke_drawn, band_selected or the final flow_changed; a press that did not
        move counts as clicked."""
        origin, self.origin = self.origin, None
        pos = e.position().toPoint()
        if self.stroke is not None:
            stroke, self.stroke = self.stroke, None
            self.scene().removeItem(self.stroke_item)
            if len(stroke) > 1:
                self.stroke_drawn.emit(stroke)
        elif self.band:
            rect = self.band.rect()
            self.scene().removeItem(self.band)
            self.band = None
            if self.moved(pos):
                self.band_selected.emit(rect)
            else:
                self.clicked.emit(origin)
        elif self.flowing:
            self.flowing = False
            if self.moved(pos):
                self.flow_changed.emit(origin, self.mapToScene(pos), True)
            else:
                self.clicked.emit(origin)
        elif origin is not None and e.button() == Qt.LeftButton:
            self.clicked.emit(origin)
        else:
            super().mouseReleaseEvent(e)

    def contextMenuEvent(self, e):
        if self.on_object(e.pos()):
            return super().contextMenuEvent(e)
        self.context_requested.emit(self.mapToScene(e.pos()), e.globalPos())

    def wheelEvent(self, e):
        if e.modifiers() & Qt.ControlModifier:
            self.zoom_step.emit(1.25 if e.angleDelta().y() > 0 else 0.8)
        else:
            super().wheelEvent(e)
