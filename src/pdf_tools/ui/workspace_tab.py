from pathlib import Path

import pymupdf
from PySide6.QtCore import QEvent, QRect, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QFontMetrics, QPainter, QPalette, QPen, QPixmap
from PySide6.QtWidgets import (
    QAbstractItemView,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListView,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QStackedWidget,
    QStyle,
    QStyledItemDelegate,
    QToolBar,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from pdf_tools.core.edits import protect
from pdf_tools.core.ranges import format_ranges
from pdf_tools.ui import icons
from pdf_tools.ui.util import to_qimage

SRC_ROLE = Qt.UserRole
PAGE_ROLE = Qt.UserRole + 1
CUT_ROLE = Qt.UserRole + 2
PART_ROLE = Qt.UserRole + 3
THUMB = QSize(148, 200)
GAP = 16
CELL = QSize(GAP + THUMB.width() + 12, THUMB.height() + 58)
THUMBS_PER_TICK = 6
IMAGE_SUFFIXES = (".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff")
# one band colour per source file, distinguishable in both themes
SOURCE_COLOURS = [QColor(26, 115, 232), QColor(217, 48, 37), QColor(30, 142, 62), QColor(249, 171, 0), QColor(161, 66, 244), QColor(0, 150, 160)]
PRIMARY_STYLE = """
QPushButton { background: palette(highlight); color: palette(highlighted-text); border: none;
              border-radius: 6px; padding: 7px 18px; font-weight: bold; }
QPushButton:disabled { background: palette(mid); }
"""
DROP_ZONE_STYLE = "QFrame#dropzone { border: 2px dashed rgba(128, 128, 128, 120); border-radius: 12px; }"


class PageDelegate(QStyledItemDelegate):
    """A page card: source colour band, thumbnail, page and file label; the strip left of it toggles a cut."""

    cut_toggled = Signal(int)
    hover = None

    def sizeHint(self, option, index):
        return CELL

    def rects(self, r):
        gap = QRect(r.left(), r.top(), GAP, r.height())
        card = QRect(r.left() + GAP, r.top() + 4, r.width() - GAP - 4, r.height() - 8)
        thumb = QRect(card.left() + (card.width() - THUMB.width()) // 2, card.top() + 8, THUMB.width(), THUMB.height())
        label = QRect(card.left() + 4, thumb.bottom() + 4, card.width() - 8, card.bottom() - thumb.bottom() - 6)
        return gap, card, thumb, label

    def paint(self, p, option, index):
        p.save()
        p.setRenderHint(QPainter.Antialiasing)
        palette = option.palette
        gap, card, thumb, label = self.rects(option.rect)
        highlight = palette.color(QPalette.Highlight)
        if option.state & QStyle.State_Selected:
            fill = QColor(highlight)
            fill.setAlpha(60)
            p.setPen(Qt.NoPen)
            p.setBrush(fill)
            p.drawRoundedRect(card, 6, 6)
        band = SOURCE_COLOURS[index.data(SRC_ROLE) % len(SOURCE_COLOURS)]
        p.fillRect(QRect(card.left() + 6, card.top() + 2, card.width() - 12, 3), band)
        pix = index.data(Qt.DecorationRole)
        if isinstance(pix, QPixmap) and not pix.isNull():
            scaled = pix.scaled(thumb.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation)
            at = QRect(thumb.left() + (thumb.width() - scaled.width()) // 2, thumb.top(), scaled.width(), scaled.height())
            p.drawPixmap(at, scaled)
            p.setPen(QPen(QColor(128, 128, 128, 110)))
            p.setBrush(Qt.NoBrush)
            p.drawRect(at)
        else:
            p.fillRect(thumb, QColor(128, 128, 128, 40))
        font = QFont(option.font)
        font.setBold(True)
        p.setFont(font)
        p.setPen(palette.color(QPalette.Text))
        top = QRect(label.left(), label.top(), label.width(), label.height() // 2)
        p.drawText(top, Qt.AlignHCenter | Qt.AlignVCenter, f"Page {index.data(PAGE_ROLE) + 1}")
        small = QFont(option.font)
        small.setPointSizeF(small.pointSizeF() * 0.85)
        p.setFont(small)
        p.setPen(palette.color(QPalette.Disabled, QPalette.Text))
        bottom = QRect(label.left(), top.bottom(), label.width(), label.height() - top.height())
        name = QFontMetrics(small).elidedText(index.data(Qt.DisplayRole) or "", Qt.ElideMiddle, bottom.width())
        p.drawText(bottom, Qt.AlignHCenter | Qt.AlignTop, name)
        cut = index.data(CUT_ROLE)
        hovered = self.hover is not None and gap.contains(self.hover) and index.row() > 0
        if cut or hovered:
            colour = QColor(highlight)
            if not cut:
                colour.setAlpha(110)
            p.setPen(QPen(colour, 2, Qt.DashLine))
            x = gap.center().x()
            p.drawLine(x, card.top(), x, card.bottom())
            icons.icon("split").paint(p, QRect(x - 7, card.top() - 2, 14, 14))
        if index.data(PART_ROLE) and (cut or index.row() == 0):
            tag = QRect(card.left() + 8, card.top() + 10, 52, 18)
            p.setPen(Qt.NoPen)
            p.setBrush(highlight)
            p.drawRoundedRect(tag, 9, 9)
            p.setPen(palette.color(QPalette.HighlightedText))
            p.setFont(small)
            p.drawText(tag, Qt.AlignCenter, f"Part {index.data(PART_ROLE)}")
        p.restore()

    def editorEvent(self, event, model, option, index):
        if event.type() == QEvent.MouseButtonRelease and event.button() == Qt.LeftButton and index.row() > 0:
            if self.rects(option.rect)[0].contains(event.position().toPoint()):
                self.cut_toggled.emit(index.row())
                return True
        return False


class PageGrid(QListWidget):
    files_dropped = Signal(list)

    def __init__(self):
        super().__init__()
        self.delegate = PageDelegate(self)
        self.setItemDelegate(self.delegate)
        self.setViewMode(QListView.IconMode)
        self.setResizeMode(QListView.Adjust)
        self.setGridSize(CELL)
        self.setUniformItemSizes(True)
        # Static: a drop reorders the list (QListWidget's InternalMove) instead of placing the item freely
        self.setMovement(QListView.Static)
        self.setDragDropMode(QAbstractItemView.InternalMove)
        self.setDefaultDropAction(Qt.MoveAction)
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)
        self.setMouseTracking(True)
        self.setFrameShape(QFrame.NoFrame)
        self.setStyleSheet("QListWidget { background: transparent; }")

    def keyPressEvent(self, e):
        if e.key() in (Qt.Key_Delete, Qt.Key_Backspace):
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


class WorkspacePage(QWidget):
    """Combine, split and extract: pages of one or more PDFs in one grid; cuts make parts, each part is a file."""

    open_requested = Signal(str)

    def __init__(self, start_dir):
        super().__init__()
        self.start_dir = Path(start_dir)
        self.sources = []
        self.names = {}
        self.pending = []
        self.grid = PageGrid()
        self.grid.files_dropped.connect(self.add_paths)
        self.grid.delegate.cut_toggled.connect(self.toggle_cut)
        model = self.grid.model()
        # bound method, not a lambda: Qt drops it when this page is destroyed (the grid empties on teardown)
        for signal in (model.rowsInserted, model.rowsRemoved, model.rowsMoved):
            signal.connect(self.on_rows_changed)

        tb = self.toolbar = QToolBar()
        tb.setIconSize(QSize(18, 18))
        tb.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        icons.bind(tb.addAction("Add files...", self.pick_files), "add")
        icons.bind(tb.addAction("Remove selected", self.remove_selected), "remove")
        tb.addSeparator()
        self.every = QSpinBox()
        self.every.setRange(1, 9999)
        self.every.setPrefix("Every ")
        self.every.setSuffix(" page(s)")
        tb.addWidget(self.every)
        icons.bind(tb.addAction("Cut", lambda: self.cut_every(self.every.value())), "split")
        tb.addAction("Clear cuts", lambda: self.cut_every(None))
        tb.addSeparator()
        icons.bind(tb.addAction("Clear all", self.clear), "close")

        title = QLabel("Combine & split")
        font = title.font()
        font.setPointSizeF(font.pointSizeF() * 1.4)
        font.setBold(True)
        title.setFont(font)
        hint = QLabel(
            "Pages of every file you add, in order. Drag to reorder, Delete to drop pages, click the strip "
            "left of a page to start a new file there. No cuts: one combined file."
        )
        hint.setWordWrap(True)
        hint.setEnabled(False)

        self.parts = QTreeWidget()
        self.parts.setHeaderLabels(["File", "Pages", "Name (double-click to rename)"])
        self.parts.setRootIsDecorated(False)
        self.parts.header().setSectionResizeMode(2, QHeaderView.Stretch)
        self.parts.setMaximumHeight(140)
        self.parts.itemChanged.connect(self.on_rename)
        content = QWidget()
        lay = QVBoxLayout(content)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self.grid, 1)
        lay.addWidget(self.parts)
        self.empty = self.build_drop_zone()
        self.stack = QStackedWidget()
        self.stack.addWidget(self.empty)
        self.stack.addWidget(content)

        self.total = QLabel()
        self.save_button = QPushButton("Save...")
        icons.bind(self.save_button, "save")
        self.save_button.setCursor(Qt.PointingHandCursor)
        self.save_button.clicked.connect(self.run_save)
        footer = QHBoxLayout()
        footer.addWidget(self.total)
        footer.addStretch()
        footer.addWidget(self.save_button)

        col = QVBoxLayout(self)
        col.setContentsMargins(24, 20, 24, 20)
        col.setSpacing(10)
        col.addWidget(title)
        col.addWidget(hint)
        col.addWidget(self.stack, 1)
        col.addLayout(footer)
        self.thumb_timer = QTimer(self)
        self.thumb_timer.timeout.connect(self.draw_thumbs)
        self.restyle()
        self.refresh()

    def build_drop_zone(self):
        zone = QFrame()
        zone.setObjectName("dropzone")
        lay = QVBoxLayout(zone)
        lay.addStretch()
        self.drop_icon = QLabel()
        self.drop_icon.setAlignment(Qt.AlignCenter)
        lay.addWidget(self.drop_icon)
        label = QLabel("Drop PDFs or images here")
        font = label.font()
        font.setPointSizeF(font.pointSizeF() * 1.2)
        font.setBold(True)
        label.setFont(font)
        label.setAlignment(Qt.AlignCenter)
        lay.addWidget(label)
        hint = QLabel("to combine them, split one, or take some pages out")
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
        self.save_button.setStyleSheet(PRIMARY_STYLE)
        self.empty.setStyleSheet(DROP_ZONE_STYLE)
        self.drop_icon.setPixmap(icons.icon("merge").pixmap(44, 44))
        self.grid.viewport().update()

    def pick_files(self):
        patterns = " ".join(f"*{s}" for s in (".pdf", *IMAGE_SUFFIXES))
        paths, _ = QFileDialog.getOpenFileNames(
            self, "Add PDFs or images", str(self.start_dir), f"PDFs and images ({patterns});;PDF (*.pdf)"
        )
        self.add_paths(paths)

    def add_paths(self, paths):
        """Add PDFs, and images as pages (one per image, or per frame of a multi-page TIFF)."""
        for path in paths:
            suffix = Path(path).suffix.lower()
            if suffix not in (".pdf", *IMAGE_SUFFIXES):
                continue
            try:
                doc = pymupdf.open(path)
                if suffix != ".pdf":
                    doc = pymupdf.open("pdf", doc.convert_to_pdf())
            except Exception as e:
                QMessageBox.critical(self, "Combine & split", f"Cannot open {path}:\n{e}")
                continue
            password = None
            while doc.needs_pass and not doc.authenticate(password or ""):
                password, ok = QInputDialog.getText(
                    self, "Combine & split", f"{Path(path).name} is password protected.\n\nPassword:", QLineEdit.Password
                )
                if not ok:
                    break
            if doc.needs_pass and doc.is_encrypted:
                doc.close()
                continue
            unlocked = pymupdf.open("pdf", doc.tobytes())
            doc.close()
            self.start_dir = Path(path).parent
            self.add_document(unlocked, Path(path), password)

    def add_document(self, doc, name, password=None):
        """Append all pages of `doc` (an open document, e.g. a tab's edited output) named after file `name`."""
        src = len(self.sources)
        self.sources.append({"doc": doc, "name": Path(name), "password": password})
        for page in range(len(doc)):
            item = QListWidgetItem(Path(name).name)
            item.setData(SRC_ROLE, src)
            item.setData(PAGE_ROLE, page)
            item.setData(CUT_ROLE, False)
            item.setFlags((item.flags() | Qt.ItemIsDragEnabled) & ~Qt.ItemIsDropEnabled)
            item.setToolTip(f"{name}, page {page + 1}")
            self.grid.addItem(item)
            self.pending.append(item)
        self.thumb_timer.start(0)
        self.refresh()

    def draw_thumbs(self):
        """Render thumbnails a few at a time so long documents stay responsive."""
        batch, self.pending = self.pending[:THUMBS_PER_TICK], self.pending[THUMBS_PER_TICK:]
        if not batch:
            self.thumb_timer.stop()
            return
        for item in batch:
            page = self.sources[item.data(SRC_ROLE)]["doc"][item.data(PAGE_ROLE)]
            zoom = min(THUMB.width() / page.rect.width, THUMB.height() / page.rect.height) * self.devicePixelRatioF()
            pix = page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom), alpha=False)
            image = QPixmap.fromImage(to_qimage(pix))
            image.setDevicePixelRatio(self.devicePixelRatioF())
            item.setData(Qt.DecorationRole, image)

    def items(self):
        return [self.grid.item(i) for i in range(self.grid.count())]

    def part_rows(self):
        parts = []
        for row, item in enumerate(self.items()):
            if row == 0 or item.data(CUT_ROLE):
                parts.append([])
            parts[-1].append(item)
        return parts

    def toggle_cut(self, row):
        item = self.grid.item(row)
        item.setData(CUT_ROLE, not item.data(CUT_ROLE))
        self.refresh()

    def cut_every(self, n):
        for row, item in enumerate(self.items()):
            item.setData(CUT_ROLE, bool(n) and row > 0 and row % n == 0)
        self.refresh()

    def remove_selected(self):
        for item in self.grid.selectedItems():
            self.grid.takeItem(self.grid.row(item))

    def clear(self):
        self.grid.clear()
        for source in self.sources:
            source["doc"].close()
        self.sources, self.names, self.pending = [], {}, []
        self.refresh()

    def on_rows_changed(self, *_):
        self.refresh()

    def default_name(self, k, count, part):
        used = {item.data(SRC_ROLE) for item in part}
        base = self.sources[next(iter(used))]["name"].stem if len(used) == 1 else "combined"
        if count == 1:
            return f"{base}-pages.pdf" if len(used) == 1 else "combined.pdf"
        return f"{base}-part{k + 1}.pdf"

    def part_key(self, part):
        return part[0].data(SRC_ROLE), part[0].data(PAGE_ROLE)

    def file_name(self, k, count, part):
        return self.names.get(self.part_key(part)) or self.default_name(k, count, part)

    def refresh(self):
        items = self.items()
        self.stack.setCurrentIndex(1 if items else 0)
        parts = self.part_rows()
        for k, part in enumerate(parts):
            for item in part:
                item.setData(PART_ROLE, k + 1 if len(parts) > 1 else 0)
        self.parts.blockSignals(True)
        self.parts.clear()
        for k, part in enumerate(parts):
            keys = [(i.data(SRC_ROLE), i.data(PAGE_ROLE)) for i in part]
            pages = ", ".join(
                f"{self.sources[src]['name'].name} {format_ranges(sorted(p for s, p in keys if s == src))}"
                for src in dict.fromkeys(s for s, _ in keys)
            )
            row = QTreeWidgetItem([str(k + 1), pages, self.file_name(k, len(parts), part)])
            row.setFlags(row.flags() | Qt.ItemIsEditable)
            row.setToolTip(1, pages)
            row.setData(2, Qt.UserRole, self.part_key(part))
            self.parts.addTopLevelItem(row)
        self.parts.blockSignals(False)
        files = len({i.data(SRC_ROLE) for i in items})
        if items:
            self.total.setText(
                f"{len(items)} page{'s' * (len(items) != 1)} from {files} file{'s' * (files != 1)}"
                f" into {len(parts)} file{'s' * (len(parts) != 1)}"
            )
        else:
            self.total.setText("")
        self.save_button.setText("Save..." if len(parts) <= 1 else f"Save {len(parts)} files...")
        self.save_button.setEnabled(bool(items))
        self.grid.viewport().update()

    def on_rename(self, row, column):
        if column == 2:
            name = row.text(2).strip()
            self.names[row.data(2, Qt.UserRole)] = name if name.lower().endswith(".pdf") else f"{name}.pdf"
            self.refresh()

    def build(self, part):
        out = pymupdf.open()
        for item in part:
            src = self.sources[item.data(SRC_ROLE)]["doc"]
            out.insert_pdf(src, from_page=item.data(PAGE_ROLE), to_page=item.data(PAGE_ROLE))
        data = out.tobytes(garbage=4, deflate=True)
        out.close()
        used = {self.sources[i.data(SRC_ROLE)]["password"] for i in part}
        # one protected source keeps its password; mixed sources are written unprotected
        password = used.pop() if len(used) == 1 else None
        return protect(data, password) if password else data

    def run_save(self):
        parts = self.part_rows()
        if not parts:
            return
        names = [self.file_name(k, len(parts), part) for k, part in enumerate(parts)]
        if len(parts) == 1:
            out, _ = QFileDialog.getSaveFileName(self, "Save PDF", str(self.start_dir / names[0]), "PDF (*.pdf)")
            if not out:
                return
            outputs = [Path(out)]
        else:
            folder = QFileDialog.getExistingDirectory(self, f"Save {len(parts)} files in", str(self.start_dir))
            if not folder:
                return
            outputs = [Path(folder) / name for name in names]
            existing = [p.name for p in outputs if p.exists()]
            if existing and QMessageBox.question(self, "Combine & split", f"Overwrite {', '.join(existing)}?") != QMessageBox.Yes:
                return
        for out, part in zip(outputs, parts):
            out.write_bytes(self.build(part))
        pages = sum(len(p) for p in parts)
        where = outputs[0] if len(outputs) == 1 else outputs[0].parent
        self.window().statusBar().showMessage(f"Saved {pages} page(s) in {len(outputs)} file(s): {where}", 8000)
        reply = QMessageBox.question(self, "Combine & split", f"Saved {len(outputs)} file(s).\n\nOpen {outputs[0].name}?")
        if reply == QMessageBox.Yes:
            self.open_requested.emit(str(outputs[0]))
