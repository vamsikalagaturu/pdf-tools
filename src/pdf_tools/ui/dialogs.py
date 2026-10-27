from pathlib import Path

from PySide6.QtCore import QFile, QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPen
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListView,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from pdf_tools.core.fonts import resolve_font
from pdf_tools.core.signatures import ink_from_scan, list_signatures, store_signature
from pdf_tools.ui.util import png_bytes, swatch_icon


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


class TextDialog(QDialog):
    def __init__(self, parent, doc, fonts, text, font, size, rgb):
        super().__init__(parent)
        self.setWindowTitle("Text box")
        self.doc = doc
        self.rgb = rgb
        form = QFormLayout(self)
        self.text = QPlainTextEdit(text)
        self.text.setMinimumSize(380, 110)
        form.addRow("Text:", self.text)
        self.font = QComboBox()
        self.font.addItems(fonts if font in fonts else [font, *fonts])
        self.font.setCurrentText(font)
        form.addRow("Font:", self.font)
        self.used = QLabel()
        self.used.setEnabled(False)
        form.addRow("", self.used)
        self.size = QDoubleSpinBox()
        self.size.setRange(4, 96)
        self.size.setSingleStep(0.5)
        self.size.setValue(size)
        form.addRow("Size (pt):", self.size)
        self.colour = QToolButton()
        self.colour.setIcon(swatch_icon(rgb))
        self.colour.clicked.connect(self.pick_colour)
        form.addRow("Colour:", self.colour)
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)
        self.font.currentTextChanged.connect(lambda _: self.update_used())
        self.text.textChanged.connect(self.update_used)
        self.update_used()

    def update_used(self):
        self.used.setText("Renders with: " + resolve_font(self.doc, self.font.currentText(), self.text.toPlainText())[3])

    def pick_colour(self):
        c = QColorDialog.getColor(QColor(*self.rgb), self, "Text colour")
        if c.isValid():
            self.rgb = (c.red(), c.green(), c.blue())
            self.colour.setIcon(swatch_icon(self.rgb))

    def values(self):
        return self.text.toPlainText().rstrip("\n"), self.font.currentText(), self.size.value(), self.rgb


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
