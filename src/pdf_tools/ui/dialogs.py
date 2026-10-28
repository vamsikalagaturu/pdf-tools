from pathlib import Path

from PySide6.QtCore import QFile, QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListView,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QRadioButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from pdf_tools.core.signatures import ink_from_scan, list_signatures, store_signature
from pdf_tools.ui.util import png_bytes


class ExportDialog(QDialog):
    def __init__(self, parent, n_pages, ranges, flat, dpi):
        super().__init__(parent)
        self.setWindowTitle("Export pages")
        form = QFormLayout(self)
        self.pages = QLineEdit(ranges)
        self.pages.setPlaceholderText("e.g. 1-3,7")
        form.addRow(f"Pages (of {n_pages}):", self.pages)
        self.flat = QCheckBox("Flatten to images (no selectable text)")
        self.flat.setChecked(flat)
        form.addRow(self.flat)
        self.dpi = QSpinBox()
        self.dpi.setRange(72, 600)
        self.dpi.setSingleStep(50)
        self.dpi.setValue(dpi)
        self.dpi.setEnabled(flat)
        self.flat.toggled.connect(self.dpi.setEnabled)
        form.addRow("DPI:", self.dpi)
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.button(QDialogButtonBox.Ok).setText("Export...")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)


class PasswordDialog(QDialog):
    def __init__(self, parent):
        super().__init__(parent)
        self.setWindowTitle("Protect with password")
        form = QFormLayout(self)
        form.addRow(QLabel("Saves an AES-256 encrypted copy; it opens only with this password."))
        self.first, self.second = QLineEdit(), QLineEdit()
        for edit in (self.first, self.second):
            edit.setEchoMode(QLineEdit.Password)
            edit.textChanged.connect(self.check)
        form.addRow("Password:", self.first)
        form.addRow("Repeat:", self.second)
        self.hint = QLabel()
        self.hint.setEnabled(False)
        form.addRow("", self.hint)
        self.buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        self.buttons.button(QDialogButtonBox.Ok).setText("Save copy...")
        self.buttons.accepted.connect(self.accept)
        self.buttons.rejected.connect(self.reject)
        form.addRow(self.buttons)
        self.check()

    def check(self):
        a, b = self.first.text(), self.second.text()
        self.hint.setText("" if not b or a == b else "The passwords differ")
        self.buttons.button(QDialogButtonBox.Ok).setEnabled(bool(a) and a == b)

    def password(self):
        return self.first.text()


class SplitDialog(QDialog):
    def __init__(self, parent, pages, ticked):
        super().__init__(parent)
        self.setWindowTitle("Split")
        self.ticked = ticked
        form = QFormLayout(self)
        self.by_count = QRadioButton("Every")
        self.count = QSpinBox()
        self.count.setRange(1, max(1, pages))
        self.count.setValue(1)
        self.count.setSuffix(" page(s)")
        row = QHBoxLayout()
        row.addWidget(self.by_count)
        row.addWidget(self.count)
        row.addStretch()
        form.addRow(row)
        starts = ", ".join(str(i + 1) for i in ticked if i)
        self.by_ticks = QRadioButton(f"Before each ticked page ({starts})" if starts else "Before each ticked page (none ticked)")
        self.by_ticks.setEnabled(bool(starts))
        form.addRow(self.by_ticks)
        (self.by_ticks if starts else self.by_count).setChecked(True)
        form.addRow(QLabel(f"{pages} pages; parts are saved as name-part1.pdf, name-part2.pdf, ... in a folder you choose."))
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.button(QDialogButtonBox.Ok).setText("Split...")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def choice(self):
        """(every, at) for core.pages.split_points."""
        return (None, self.ticked) if self.by_ticks.isChecked() else (self.count.value(), ())


class SignaturePad(QWidget):
    W, H = 1200, 400

    def __init__(self):
        super().__init__()
        self.setMinimumSize(600, 200)
        self.image = QImage(self.W, self.H, QImage.Format_ARGB32_Premultiplied)
        self.clear()

    def clear(self):
        self.image.fill(Qt.transparent)
        self.bounds = QRectF()
        self.last = None
        self.update()

    def to_image(self, pos):
        return QPointF(pos.x() * self.W / self.width(), pos.y() * self.H / self.height())

    def paintEvent(self, e):
        p = QPainter(self)
        p.fillRect(self.rect(), Qt.white)
        p.setPen(QPen(QColor("#c8c8c8"), 1, Qt.DashLine))
        y = int(self.height() * 0.75)
        p.drawLine(20, y, self.width() - 20, y)
        p.drawImage(self.rect(), self.image)

    def mousePressEvent(self, e):
        self.last = self.to_image(e.position())

    def mouseMoveEvent(self, e):
        if self.last is None:
            return
        pt = self.to_image(e.position())
        p = QPainter(self.image)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(QPen(Qt.black, 5, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
        p.drawLine(self.last, pt)
        p.end()
        self.bounds |= QRectF(self.last, pt).normalized().adjusted(-4, -4, 4, 4)
        self.last = pt
        self.update()

    def mouseReleaseEvent(self, e):
        self.last = None

    def png(self):
        if self.bounds.isEmpty():
            return None
        return png_bytes(self.image.copy(self.bounds.toAlignedRect().intersected(self.image.rect())))


class SignatureDialog(QDialog):
    def __init__(self, parent, last):
        super().__init__(parent)
        self.setWindowTitle("Signatures")
        self.resize(640, 420)
        self.list = QListWidget()
        self.list.setViewMode(QListView.IconMode)
        self.list.setIconSize(QSize(240, 80))
        self.list.setResizeMode(QListView.Adjust)
        self.list.setMovement(QListView.Static)
        self.list.setSpacing(8)
        self.list.itemDoubleClicked.connect(lambda _: self.accept())
        draw_btn, import_btn, delete_btn = QPushButton("Draw new..."), QPushButton("Import image..."), QPushButton("Delete")
        draw_btn.clicked.connect(self.draw_new)
        import_btn.clicked.connect(self.import_image)
        delete_btn.clicked.connect(self.delete_selected)
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.button(QDialogButtonBox.Ok).setText("Place")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        lay = QVBoxLayout(self)
        lay.addWidget(self.list)
        row = QHBoxLayout()
        for b in (draw_btn, import_btn, delete_btn):
            row.addWidget(b)
        row.addStretch()
        lay.addLayout(row)
        lay.addWidget(buttons)
        for path in list_signatures():
            self.add_item(path)
        match = self.list.findItems(last or "", Qt.MatchExactly)
        self.list.setCurrentRow(self.list.row(match[0]) if match else self.list.count() - 1)

    def add_item(self, path):
        item = QListWidgetItem(QIcon(str(path)), path.name)
        item.setData(Qt.UserRole, str(path))
        self.list.addItem(item)
        self.list.setCurrentItem(item)

    def store(self, data):
        if data:
            self.add_item(store_signature(data))

    def draw_new(self):
        dlg = QDialog(self)
        dlg.setWindowTitle("Draw signature")
        pad = SignaturePad()
        clear_btn = QPushButton("Clear")
        clear_btn.clicked.connect(pad.clear)
        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(dlg.accept)
        buttons.rejected.connect(dlg.reject)
        lay = QVBoxLayout(dlg)
        lay.addWidget(QLabel("Sign above the line with the mouse or a pen."))
        lay.addWidget(pad)
        row = QHBoxLayout()
        row.addWidget(clear_btn)
        row.addStretch()
        row.addWidget(buttons)
        lay.addLayout(row)
        if dlg.exec() == QDialog.Accepted:
            self.store(pad.png())

    def import_image(self):
        path, _ = QFileDialog.getOpenFileName(self, "Import signature", str(Path.home()), "Images (*.png *.jpg *.jpeg)")
        if path:
            data = ink_from_scan(path)
            if data:
                self.store(data)
            else:
                QMessageBox.warning(self, "Signatures", "No ink found in that image.")

    def delete_selected(self):
        item = self.list.currentItem()
        if not item:
            return
        path = item.data(Qt.UserRole)
        reply = QMessageBox.question(self, "Signatures", f"Move {Path(path).name} to the trash?")
        if reply != QMessageBox.Yes:
            return
        if not QFile.moveToTrash(path):
            QMessageBox.critical(self, "Signatures", f"Could not move {Path(path).name} to the trash.")
            return
        self.list.takeItem(self.list.row(item))

    def selected_path(self):
        item = self.list.currentItem()
        return Path(item.data(Qt.UserRole)) if item else None
