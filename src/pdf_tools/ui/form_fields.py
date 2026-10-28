"""Fillable PDF form fields in the page view.

Each AcroForm widget gets a real Qt editor (line/text edit, checkbox, radio, combo) placed over the page; typed
values become edits of the tab (`tab.forms`), shrink to fit like auto-size fields, and are written on save.
"""

import pymupdf
from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtGui import QFontMetrics, QTextLayout, QTextOption
from PySide6.QtWidgets import QApplication, QCheckBox, QComboBox, QLineEdit, QPlainTextEdit, QRadioButton

from pdf_tools.core.forms import is_on

MIN_FIELD_PX = 5
FIELD_STYLE = "background: rgb(220, 232, 255); color: black; border: none; padding: 0px;"


class FormFields(QObject):
    """Live editors over a document's AcroForm fields; values go to `tab.forms` as undoable edits."""

    def __init__(self, tab):
        super().__init__(tab)
        self.tab = tab
        self.reset()

    def reset(self):
        self.original, self.editors, self.proxies, self.fit, self.names = {}, {}, {}, {}, {}
        self.edit_key = None

    def add(self, scene, pno):
        """Put editors over every supported field of page `pno`, showing the edited value where there is one.

        Remembers each field's value from the file (to tell edits from no-ops) and its label for the Edits tab.
        """
        tab = self.tab
        proxies = []
        for widget in tab.doc[pno].widgets():
            style = FIELD_STYLE
            kind, key = widget.field_type, (pno, widget.xref)
            self.original.setdefault(key, widget.field_value)
            self.names[key] = widget.field_label or widget.field_name
            value = tab.forms.get(key, widget.field_value)
            rect = tab.to_scene(widget.rect, pno)
            if kind == pymupdf.PDF_WIDGET_TYPE_TEXT:
                if widget.field_flags & pymupdf.PDF_TX_FIELD_IS_MULTILINE:
                    editor = QPlainTextEdit(value or "")
                    editor.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
                    editor.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
                    editor.document().setDocumentMargin(1)
                else:
                    editor = QLineEdit(value or "")
                    editor.setTextMargins(1, 0, 1, 0)
                editor.textChanged.connect(lambda *_, e=editor, key=key, s=widget.text_fontsize: self.on_text(e, key, s))
            elif kind in (pymupdf.PDF_WIDGET_TYPE_CHECKBOX, pymupdf.PDF_WIDGET_TYPE_RADIOBUTTON):
                editor = QCheckBox() if kind == pymupdf.PDF_WIDGET_TYPE_CHECKBOX else QRadioButton()
                editor.setAutoExclusive(False)
                editor.setChecked(is_on(value))
                editor.toggled.connect(lambda v, key=key, name=widget.field_name: self.set_check(key, name, v))
                side = max(4, int(min(rect.width(), rect.height())) - 2)
                style += f" QCheckBox::indicator, QRadioButton::indicator {{ width: {side}px; height: {side}px; }}"
            elif kind in (pymupdf.PDF_WIDGET_TYPE_COMBOBOX, pymupdf.PDF_WIDGET_TYPE_LISTBOX):
                editor = QComboBox()
                editor.addItems([c[0] if isinstance(c, (list, tuple)) else str(c) for c in widget.choice_values or []])
                editor.setEditable(bool(widget.field_flags & pymupdf.PDF_CH_FIELD_IS_EDIT))
                editor.setCurrentText(value or "")
                editor.currentTextChanged.connect(lambda v, key=key: self.set_form(key, v, coalesce=False))
            else:
                continue
            editor.installEventFilter(self)
            if isinstance(editor, QPlainTextEdit):
                editor.viewport().installEventFilter(self)
            editor.setToolTip(widget.field_name)
            editor.setStyleSheet(style)
            editor.setMinimumSize(0, 0)
            editor.setFixedSize(round(rect.width()), round(rect.height()))
            if isinstance(editor, (QPlainTextEdit, QLineEdit)):
                self.fit_text(editor, key, widget.text_fontsize)
            if isinstance(editor, QLineEdit):
                editor.setCursorPosition(0)
            if isinstance(editor, QComboBox):
                self.fit_combo(editor)
            proxy = scene.addWidget(editor)
            proxy.setGeometry(rect)
            proxy.setZValue(1.5)
            self.editors[key] = editor
            proxies.append(proxy)
        self.proxies[pno] = proxies

    def eventFilter(self, obj, event):
        # the wheel over a form field scrolls/zooms the page instead of the field (or changing a dropdown)
        if event.type() != QEvent.Wheel:
            return False
        tab = self.tab
        if event.modifiers() & Qt.ControlModifier:
            tab.set_zoom(tab.zoom * (1.25 if event.angleDelta().y() > 0 else 0.8))
        else:
            bar = tab.view.verticalScrollBar()
            bar.setValue(bar.value() - event.angleDelta().y())
        return True

    def page_has_focus(self, pno):
        focused = QApplication.focusWidget()
        return any(e is focused or e.isAncestorOf(focused) for k, e in self.editors.items() if k[0] == pno)

    def remove(self, pno):
        self.detach(e for k, e in self.editors.items() if k[0] == pno)
        for proxy in self.proxies.pop(pno, []):
            self.tab.view.scene().removeItem(proxy)
        self.editors = {k: e for k, e in self.editors.items() if k[0] != pno}

    def detach(self, editors=None):
        """Silence field widgets before the scene destroys them: a half-destroyed widget's signals crash PySide."""
        for editor in list(self.editors.values() if editors is None else editors):
            editor.blockSignals(True)
            editor.removeEventFilter(self)
            if isinstance(editor, QPlainTextEdit):
                editor.viewport().removeEventFilter(self)

    def clear_scene_refs(self):
        self.editors, self.proxies = {}, {}

    def fit_combo(self, editor):
        font = editor.font()
        px = max(MIN_FIELD_PX, min(12, round(editor.height() * 0.75)))
        longest = max([editor.itemText(i) for i in range(editor.count())] + [editor.currentText()], key=len)
        font.setPixelSize(px)
        while px > MIN_FIELD_PX and QFontMetrics(font).horizontalAdvance(longest) > editor.width() - 22:
            px -= 1
            font.setPixelSize(px)
        editor.setFont(font)

    def text_fits(self, editor, font, text):
        if isinstance(editor, QLineEdit):
            return QFontMetrics(font).horizontalAdvance(text) <= editor.width() - 4
        # count wrapped lines the way QPlainTextEdit lays them out (one QTextLayout per paragraph)
        margin = editor.document().documentMargin()
        width = max(editor.width() - 2 * margin - 4, 1)
        option = QTextOption()
        option.setWrapMode(QTextOption.WrapAtWordBoundaryOrAnywhere)
        lines = 0
        for para in text.split("\n"):
            layout = QTextLayout(para, font)
            layout.setTextOption(option)
            layout.beginLayout()
            while (line := layout.createLine()).isValid():
                line.setLineWidth(width)
                lines += 1
            layout.endLayout()
        return lines * QFontMetrics(font).lineSpacing() <= editor.height() - 2 * margin - 2

    def fit_text(self, editor, key, size):
        """Use the field's font size, shrinking it (as auto-size fields do) until the text fits the box."""
        multiline = isinstance(editor, QPlainTextEdit)
        text = editor.toPlainText() if multiline else editor.text()
        start = round(size) if size else (12 if multiline else min(12, round(editor.height() * 0.75)))
        font = editor.font()
        px = max(start, MIN_FIELD_PX)
        font.setPixelSize(px)
        while px > MIN_FIELD_PX and not self.text_fits(editor, font, text):
            px -= 1
            font.setPixelSize(px)
        editor.setFont(font)
        self.fit[key] = px if px < start else None

    def on_text(self, editor, key, size):
        self.fit_text(editor, key, size)
        self.set_form(key, editor.toPlainText() if isinstance(editor, QPlainTextEdit) else editor.text())

    def shrunk(self):
        return {k: px for k, px in self.fit.items() if px and k in self.tab.forms}

    def set_form(self, key, value, coalesce=True, record=True):
        """Store a field value; consecutive typing in one field is a single undo step."""
        tab = self.tab
        if record and not (coalesce and key == self.edit_key):
            tab.push_undo(tab.snapshot())
        self.edit_key = key if coalesce else None
        original = self.original.get(key)
        if isinstance(value, bool):
            original = is_on(original)
        if value == original or (not value and not original):
            tab.forms.pop(key, None)
        else:
            tab.forms[key] = value
        tab.update_state()

    def set_check(self, key, name, on):
        """Record a checkbox or radio button; turning a radio on turns its group's others off (same undo step)."""
        self.set_form(key, on, coalesce=False)
        if not (on and isinstance(self.editors.get(key), QRadioButton)):
            return
        for other, editor in self.editors.items():
            if other != key and isinstance(editor, QRadioButton) and editor.toolTip() == name:
                editor.blockSignals(True)
                editor.setChecked(False)
                editor.blockSignals(False)
                self.set_form(other, False, coalesce=False, record=False)
