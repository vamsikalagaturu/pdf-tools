from pathlib import Path

import pymupdf
from PySide6.QtCore import QEvent, QPersistentModelIndex, QRect, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QFont, QFontMetrics, QPainter, QPalette, QPixmap
from PySide6.QtWidgets import (
    QAbstractItemView,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QStackedWidget,
    QStyle,
    QStyledItemDelegate,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from pdf_tools.core.edits import merge
from pdf_tools.core.ranges import parse_ranges
from pdf_tools.ui import icons
from pdf_tools.ui.util import to_qimage

PATH_ROLE = Qt.UserRole
PAGES_ROLE = Qt.UserRole + 1
RANGE_ROLE = Qt.UserRole + 2
FOLDER_ROLE = Qt.UserRole + 3
THUMB_ZOOM = 0.12
COLUMN_WIDTH = 820
# neutral grey that reads on both the dark and the light palette
BORDER = QColor(128, 128, 128, 110)
DANGER = QColor(220, 60, 60)
PRIMARY_STYLE = """
QPushButton { background: palette(highlight); color: palette(highlighted-text); border: none;
              border-radius: 6px; padding: 7px 18px; font-weight: bold; }
QPushButton:disabled { background: palette(button); color: rgba(128, 128, 128, 200); }
"""
DROP_ZONE_STYLE = "QFrame#dropzone { border: 2px dashed rgba(128, 128, 128, 120); border-radius: 12px; }"
PILL_EDITOR_STYLE = """
QLineEdit { border: 1px solid palette(highlight); border-radius: 14px; background: palette(window);
            color: palette(text); padding: 0px 10px; }
"""


def range_ok(text, pages):
    try:
        parse_ranges(text, pages)
        return True
    except ValueError:
        return False


class MergeDelegate(QStyledItemDelegate):
    """A merge row: thumbnail, name over folder, page count, editable page-range pill, remove button."""

    ROW_HEIGHT = 70
    remove_requested = Signal(int)
    editing = None
    hover = None

    def sizeHint(self, option, index):
        return QSize(option.rect.width(), self.ROW_HEIGHT)

    def rects(self, r):
        # QListView paints rows viewport-wide minus spacing but gives editors and events visualRect (sizeHint wide)
        view = self.parent()
        r = QRect(r.left(), r.top(), view.viewport().width() - 2 * view.spacing(), r.height())
        r = r.adjusted(4, 3, -4, -3)
        mid = r.center().y()
        remove = QRect(r.right() - 34, mid - 12, 24, 24)
        pill = QRect(remove.left() - 12 - 130, mid - 14, 130, 28)
        pages = QRect(pill.left() - 12 - 80, mid - 10, 80, 20)
        thumb = QRect(r.left() + 12, r.top() + 7, 40, r.height() - 14)
        text = QRect(thumb.right() + 14, r.top(), pages.left() - thumb.right() - 24, r.height())
        return r, thumb, text, pages, pill, remove

    def paint(self, p, option, index):
        p.save()
        p.setRenderHint(QPainter.Antialiasing)
        palette = option.palette
        card, thumb, text, pages_rect, pill, remove = self.rects(option.rect)
        selected = option.state & QStyle.State_Selected
        hovered = option.state & QStyle.State_MouseOver
        if selected:
            fill = QColor(palette.color(QPalette.Highlight))
            fill.setAlpha(70)
        else:
            # a surface one step lighter than the page: white in light mode, raised grey in dark mode
            base, window = palette.color(QPalette.Base), palette.color(QPalette.Window)
            fill = base if base.lightness() > window.lightness() else palette.color(QPalette.Button)
        p.setPen(QColor(palette.color(QPalette.Highlight)) if selected or hovered else BORDER)
        p.setBrush(fill)
        p.drawRoundedRect(card, 8, 8)

        pix = index.data(Qt.DecorationRole)
        if isinstance(pix, QPixmap):
            scaled = pix.scaled(thumb.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
            at = QRect(thumb.left(), thumb.center().y() - scaled.height() // 2, scaled.width(), scaled.height())
            p.drawPixmap(at, scaled)
            p.setPen(palette.color(QPalette.Mid))
            p.setBrush(Qt.NoBrush)
            p.drawRect(at)

        name_font = QFont(option.font)
        name_font.setBold(True)
        p.setFont(name_font)
        p.setPen(palette.color(QPalette.Text))
        name = QFontMetrics(name_font).elidedText(index.data(Qt.DisplayRole), Qt.ElideMiddle, text.width())
        p.drawText(QRect(text.left(), text.top() + 12, text.width(), 22), Qt.AlignLeft | Qt.AlignVCenter, name)
        small = QFont(option.font)
        small.setPointSizeF(small.pointSizeF() * 0.9)
        p.setFont(small)
        p.setPen(palette.color(QPalette.Disabled, QPalette.Text))
        folder = QFontMetrics(small).elidedText(index.data(FOLDER_ROLE), Qt.ElideMiddle, text.width())
        p.drawText(QRect(text.left(), text.center().y() + 2, text.width(), 20), Qt.AlignLeft | Qt.AlignVCenter, folder)
        n = index.data(PAGES_ROLE)
        p.drawText(pages_rect, Qt.AlignRight | Qt.AlignVCenter, f"{n} page{'s' * (n != 1)}")

        # while its range is being edited, the editor (styled as the same pill) replaces the painted one
        if not (self.editing is not None and self.editing.isValid() and self.editing.row() == index.row()):
            spec = index.data(RANGE_ROLE) or ""
            valid = not spec or range_ok(spec, n)
            error = DANGER
            p.setPen(error if not valid else BORDER)
            p.setBrush(palette.color(QPalette.Window))
            # inset half the pen so the outline stays inside the rect, as the editor's border does
            p.drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5), 13.5, 13.5)
            p.setFont(option.font)
            text_colour = palette.color(QPalette.Text) if spec else palette.color(QPalette.Disabled, QPalette.Text)
            p.setPen(error if not valid else text_colour)
            p.drawText(pill, Qt.AlignCenter, spec or "All pages")
        if self.hover is not None and remove.contains(self.hover):
            p.setPen(Qt.NoPen)
            p.setBrush(DANGER)
            p.drawRoundedRect(remove, 6, 6)
            p.drawPixmap(remove.adjusted(4, 4, -4, -4), icons.pixmap("close", "#ffffff"))
        else:
            icons.icon("close").paint(p, remove.adjusted(4, 4, -4, -4))
        p.restore()

    def editorEvent(self, event, model, option, index):
        if event.type() == QEvent.MouseButtonRelease and event.button() == Qt.LeftButton:
            _, _, _, _, pill, remove = self.rects(option.rect)
            pos = event.position().toPoint()
            if remove.contains(pos):
                self.remove_requested.emit(index.row())
                return True
            if pill.contains(pos):
                self.parent().edit(index)
                return True
        return False

    def createEditor(self, parent, option, index):
        editor = QLineEdit(parent)
        editor.setAlignment(Qt.AlignCenter)
        editor.setToolTip(f"Pages to include, e.g. 1-3,7 (blank = all {index.data(PAGES_ROLE)}); Enter to apply, Esc to cancel")
        editor.setStyleSheet(PILL_EDITOR_STYLE)
        self.editing = QPersistentModelIndex(index)
        return editor

    def destroyEditor(self, editor, index):
        self.editing = None
        super().destroyEditor(editor, index)
        self.parent().viewport().update()

    def updateEditorGeometry(self, editor, option, index):
        editor.setGeometry(self.rects(option.rect)[4])

    def setEditorData(self, editor, index):
        editor.setText(index.data(RANGE_ROLE) or "")

    def setModelData(self, editor, model, index):
        model.setData(index, editor.text().strip(), RANGE_ROLE)


class MergeList(QListWidget):
    files_dropped = Signal(list)

    def __init__(self):
        super().__init__()
        self.delegate = MergeDelegate(self)
        self.delegate.remove_requested.connect(lambda row: self.takeItem(row))
        self.setItemDelegate(self.delegate)
        self.setFrameShape(QFrame.NoFrame)
        self.setStyleSheet("QListWidget { background: transparent; }")
        self.setSpacing(3)
        self.setMouseTracking(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)
        self.setDragDropMode(QAbstractItemView.InternalMove)
        self.setEditTriggers(QAbstractItemView.NoEditTriggers)

    def keyPressEvent(self, e):
        if e.key() in (Qt.Key_Delete, Qt.Key_Backspace) and self.state() != QAbstractItemView.EditingState:
            for item in self.selectedItems():
                self.takeItem(self.row(item))
        else:
            super().keyPressEvent(e)

    def mouseMoveEvent(self, e):
        self.delegate.hover = e.position().toPoint()
        self.viewport().update()
        super().mouseMoveEvent(e)

    def leaveEvent(self, e):
        self.delegate.hover = None
        self.viewport().update()
        super().leaveEvent(e)

    def dragEnterEvent(self, e):
        e.acceptProposedAction() if e.mimeData().hasUrls() else super().dragEnterEvent(e)

    def dragMoveEvent(self, e):
        e.acceptProposedAction() if e.mimeData().hasUrls() else super().dragMoveEvent(e)

    def dropEvent(self, e):
        if e.mimeData().hasUrls():
            self.files_dropped.emit([u.toLocalFile() for u in e.mimeData().urls()])
            e.acceptProposedAction()
        else:
            super().dropEvent(e)


class MergePage(QWidget):
    open_requested = Signal(str)

    def __init__(self, start_dir):
        super().__init__()
        self.start_dir = start_dir
        self.list = MergeList()
        self.list.files_dropped.connect(self.add_paths)
        model = self.list.model()
        # bound method, not a lambda: Qt drops it when this page is destroyed (the list empties on teardown)
        for signal in (model.rowsInserted, model.rowsRemoved, model.rowsMoved, model.dataChanged):
            signal.connect(self.on_rows_changed)

        tb = self.toolbar = QToolBar()
        tb.setIconSize(QSize(18, 18))
        tb.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        for text, slot, name in (
            ("Add files...", self.pick_files, "add"),
            ("Sort by name", lambda: self.list.sortItems(Qt.AscendingOrder), "sort"),
            ("Clear", self.clear, "remove"),
        ):
            icons.bind(tb.addAction(text, slot), name)

        title = QLabel("Merge PDFs")
        font = title.font()
        font.setPointSizeF(font.pointSizeF() * 1.4)
        font.setBold(True)
        title.setFont(font)
        hint = QLabel("Files are joined top to bottom. Drag to reorder; click a page range to pick pages, e.g. 1-3,7.")
        hint.setEnabled(False)

        self.empty = self.build_drop_zone()
        self.stack = QStackedWidget()
        self.stack.addWidget(self.empty)
        self.stack.addWidget(self.list)

        self.total = QLabel()
        self.merge_button = QPushButton("Merge...")
        icons.bind(self.merge_button, "merge")
        self.merge_button.setCursor(Qt.PointingHandCursor)
        self.merge_button.clicked.connect(self.run_merge)
        footer = QHBoxLayout()
        footer.addWidget(self.total)
        footer.addStretch()
        footer.addWidget(self.merge_button)

        column = QWidget()
        column.setMaximumWidth(COLUMN_WIDTH)
        col = QVBoxLayout(column)
        col.setContentsMargins(0, 0, 0, 0)
        col.setSpacing(10)
        col.addWidget(title)
        col.addWidget(hint)
        col.addSpacing(6)
        col.addWidget(self.stack, 1)
        col.addLayout(footer)
        outer = QHBoxLayout(self)
        outer.setContentsMargins(24, 20, 24, 20)
        outer.addStretch()
        outer.addWidget(column, 100)
        outer.addStretch()
        self.restyle()
        self.update_total()

    def build_drop_zone(self):
        zone = QFrame()
        zone.setObjectName("dropzone")
        lay = QVBoxLayout(zone)
        lay.addStretch()
        self.drop_icon = QLabel()
        self.drop_icon.setAlignment(Qt.AlignCenter)
        lay.addWidget(self.drop_icon)
        label = QLabel("Drop PDFs here")
        font = label.font()
        font.setPointSizeF(font.pointSizeF() * 1.2)
        font.setBold(True)
        label.setFont(font)
        label.setAlignment(Qt.AlignCenter)
        lay.addWidget(label)
        hint = QLabel("or")
        hint.setEnabled(False)
        hint.setAlignment(Qt.AlignCenter)
        lay.addWidget(hint)
        add = QPushButton("Add files...")
        icons.bind(add, "add")
        add.setMinimumHeight(36)
        add.setCursor(Qt.PointingHandCursor)
        add.clicked.connect(self.pick_files)
        lay.addWidget(add, 0, Qt.AlignCenter)
        lay.addStretch()
        return zone

    def restyle(self):
        """Re-resolve palette() colours after a theme change."""
        self.merge_button.setStyleSheet(PRIMARY_STYLE)
        self.empty.setStyleSheet(DROP_ZONE_STYLE)
        self.drop_icon.setPixmap(icons.icon("merge").pixmap(44, 44))

    def pick_files(self):
        self.add_paths(QFileDialog.getOpenFileNames(self, "Add PDFs", str(self.start_dir), "PDF (*.pdf)")[0])

    def add_paths(self, paths):
        home = str(Path.home())
        for path in paths:
            if not path.lower().endswith(".pdf"):
                continue
            try:
                doc = pymupdf.open(path)
            except Exception as e:
                QMessageBox.critical(self, "Merge PDFs", f"Cannot open {path}:\n{e}")
                continue
            if doc.needs_pass:
                QMessageBox.critical(self, "Merge PDFs", f"{Path(path).name} is password-protected.")
                doc.close()
                continue
            pix = doc[0].get_pixmap(matrix=pymupdf.Matrix(THUMB_ZOOM, THUMB_ZOOM), alpha=False)
            item = QListWidgetItem(Path(path).name)
            item.setData(Qt.DecorationRole, QPixmap.fromImage(to_qimage(pix)))
            item.setData(PATH_ROLE, path)
            item.setData(PAGES_ROLE, len(doc))
            item.setData(RANGE_ROLE, "")
            item.setData(FOLDER_ROLE, str(Path(path).parent).replace(home, "~", 1))
            item.setToolTip(path)
            item.setFlags((item.flags() | Qt.ItemIsEditable) & ~Qt.ItemIsDropEnabled)
            doc.close()
            self.list.addItem(item)
            self.start_dir = Path(path).parent
        self.update_total()

    def remove_selected(self):
        for item in self.list.selectedItems():
            self.list.takeItem(self.list.row(item))

    def parts(self):
        """Return [(path, 0-based pages)] in list order; raises ValueError naming a bad range."""
        out = []
        for i in range(self.list.count()):
            item = self.list.item(i)
            n, text = item.data(PAGES_ROLE), (item.data(RANGE_ROLE) or "").strip()
            try:
                pages = parse_ranges(text, n) if text else list(range(n))
            except ValueError:
                raise ValueError(f"{item.text()}: invalid range '{text}' (file has {n} page{'s' * (n != 1)})")
            out.append((item.data(PATH_ROLE), pages))
        return out

    def clear(self):
        self.list.clear()
        self.update_total()

    def on_rows_changed(self, *_):
        self.update_total()

    def update_total(self):
        self.stack.setCurrentWidget(self.list if self.list.count() else self.empty)
        try:
            parts = self.parts()
        except ValueError as e:
            self.total.setText(str(e))
            self.total.setStyleSheet("color: rgb(220, 60, 60);")
            self.merge_button.setEnabled(False)
            return
        self.total.setStyleSheet("")
        pages = sum(len(p) for _, p in parts)
        self.total.setText(f"{len(parts)} file{'s' * (len(parts) != 1)}, {pages} page{'s' * (pages != 1)}" if parts else "")
        self.merge_button.setEnabled(bool(parts))

    def run_merge(self):
        try:
            parts = self.parts()
        except ValueError as e:
            QMessageBox.critical(self, "Merge PDFs", str(e))
            return
        if not parts:
            return
        out, _ = QFileDialog.getSaveFileName(
            self, "Save merged PDF", str(Path(parts[0][0]).parent / "merged.pdf"), "PDF (*.pdf)"
        )
        if not out:
            return
        if any(Path(out).resolve() == Path(p).resolve() for p, _ in parts):
            QMessageBox.critical(self, "Merge PDFs", "The output cannot be one of the input files.")
            return
        Path(out).write_bytes(merge(parts))
        pages = sum(len(p) for _, p in parts)
        self.window().statusBar().showMessage(f"Merged {len(parts)} file(s), {pages} page(s) into {out}", 8000)
        reply = QMessageBox.question(self, "Merge PDFs", f"Merged {pages} pages.\n\nOpen {Path(out).name} for editing?")
        if reply == QMessageBox.Yes:
            self.open_requested.emit(out)
