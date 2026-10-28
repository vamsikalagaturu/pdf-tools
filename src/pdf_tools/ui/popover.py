from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QApplication,
    QButtonGroup,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPlainTextEdit,
    QPushButton,
    QToolButton,
    QVBoxLayout,
)

from pdf_tools.ui import icons
from pdf_tools.ui.util import swatch_icon

STYLE = """
Popover { background: palette(window); border: 1px solid palette(mid); border-radius: 6px; }
QToolButton { border: 2px solid transparent; border-radius: 4px; padding: 1px; }
QToolButton:checked { border-color: palette(highlight); }
"""


class Popover(QFrame):
    """A small in-view editor for a note or annotation comment; Done/click-away commits, Esc cancels."""

    def __init__(
        self, parent, title, text, on_commit, on_delete=None, placeholder="Add a comment...",
        colours=None, colour=None, on_colour=None,
    ):
        super().__init__(parent)
        # a child of the page viewport would otherwise inherit the current tool's cursor
        self.setCursor(Qt.ArrowCursor)
        self.setStyleSheet(STYLE)
        self.original, self.on_commit, self.on_delete = text, on_commit, on_delete
        self.finished = False
        lay = QVBoxLayout(self)
        lay.setContentsMargins(10, 8, 10, 10)
        head = QLabel(title)
        font = head.font()
        font.setBold(True)
        head.setFont(font)
        lay.addWidget(head)
        if colours and on_colour:
            lay.addLayout(self.colour_row(colours, colour, on_colour))
        self.edit = QPlainTextEdit(text)
        self.edit.setPlaceholderText(placeholder)
        self.edit.setFixedSize(280, 96)
        lay.addWidget(self.edit)
        row = QHBoxLayout()
        if on_delete:
            delete = QPushButton("Delete")
            delete.clicked.connect(self.delete)
            row.addWidget(delete)
        row.addStretch()
        done = QPushButton("Done")
        done.setDefault(True)
        done.clicked.connect(lambda: self.dismiss(True))
        row.addWidget(done)
        lay.addLayout(row)
        for button in self.findChildren(QPushButton):
            button.setCursor(Qt.PointingHandCursor)
        QShortcut(QKeySequence("Escape"), self, lambda: self.dismiss(False))
        QShortcut(QKeySequence("Ctrl+Return"), self, lambda: self.dismiss(True))

    def colour_row(self, colours, current, on_colour):
        """Swatches that recolour immediately (and stay open), the current one checked."""
        row = QHBoxLayout()
        row.setSpacing(4)
        group = QButtonGroup(self)
        for name, rgb in colours:
            button = QToolButton()
            button.setIcon(swatch_icon(rgb))
            button.setToolTip(name)
            button.setCheckable(True)
            button.setChecked(tuple(rgb) == tuple(current or ()))
            button.setCursor(Qt.PointingHandCursor)
            button.clicked.connect(lambda _=False, rgb=rgb: on_colour(rgb))
            group.addButton(button)
            row.addWidget(button)
        row.addStretch()
        return row

    def dismiss(self, commit=True):
        if self.finished:
            return
        self.finished = True
        text = self.edit.toPlainText().strip()
        self.hide()
        self.deleteLater()
        if commit and text != self.original.strip():
            self.on_commit(text)

    def delete(self):
        if self.finished:
            return
        self.finished = True
        self.hide()
        self.deleteLater()
        self.on_delete()


class FormatBar(QFrame):
    """Floating font controls over a text box being typed: family, size, bold, italic, colour."""

    def __init__(self, parent, families, family, size, bold, italic, rgb, on_change):
        super().__init__(parent)
        self.setObjectName("formatbar")
        self.setCursor(Qt.ArrowCursor)
        self.setStyleSheet(
            "QFrame#formatbar { background: palette(window); border: 1px solid palette(mid); border-radius: 6px; }"
        )
        self.rgb, self.on_change = tuple(rgb), on_change
        lay = QHBoxLayout(self)
        lay.setContentsMargins(6, 4, 6, 4)
        lay.setSpacing(4)
        self.family = QComboBox()
        self.family.addItems(families if family in families else [family, *families])
        self.family.setCurrentText(family)
        self.size = QDoubleSpinBox()
        self.size.setRange(4, 96)
        self.size.setSingleStep(0.5)
        self.size.setValue(size)
        self.size.setSuffix(" pt")
        self.bold, self.italic, self.colour = QToolButton(), QToolButton(), QToolButton()
        for button, name, tip, on in ((self.bold, "bold", "Bold", bold), (self.italic, "italic", "Italic", italic)):
            button.setCheckable(True)
            button.setChecked(on)
            button.setToolTip(tip)
            icons.bind(button, name)
        self.colour.setIcon(swatch_icon(self.rgb))
        self.colour.setToolTip("Text colour")
        self.hint = QLabel()
        self.hint.setEnabled(False)
        for w in (self.family, self.size, self.bold, self.italic, self.colour, self.hint):
            lay.addWidget(w)
        self.family.currentTextChanged.connect(lambda _: self.changed())
        self.size.valueChanged.connect(lambda _: self.changed())
        self.bold.toggled.connect(lambda _: self.changed())
        self.italic.toggled.connect(lambda _: self.changed())
        self.colour.clicked.connect(self.pick_colour)

    def values(self):
        return self.family.currentText(), self.size.value(), self.bold.isChecked(), self.italic.isChecked(), self.rgb

    def changed(self):
        self.on_change(*self.values())

    def pick_colour(self):
        self.picking = True
        c = QColorDialog.getColor(QColor(*self.rgb), self, "Text colour")
        self.picking = False
        if c.isValid():
            self.rgb = (c.red(), c.green(), c.blue())
            self.colour.setIcon(swatch_icon(self.rgb))
            self.changed()

    def has_focus(self):
        if getattr(self, "picking", False):
            return True
        focused = QApplication.focusWidget()
        return focused is not None and (focused is self or self.isAncestorOf(focused) or self.family.view().isAncestorOf(focused))
