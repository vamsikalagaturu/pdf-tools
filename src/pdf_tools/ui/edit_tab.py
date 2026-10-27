import os
import statistics
import tempfile
from pathlib import Path

import pymupdf
from PySide6.QtCore import QEvent, QFile, QPoint, QPointF, QRectF, QSize, Qt, QTimer, Signal
from PySide6.QtGui import (
    QAction,
    QActionGroup,
    QBrush,
    QColor,
    QFontMetrics,
    QIcon,
    QImage,
    QKeySequence,
    QPixmap,
    QShortcut,
    QTextLayout,
    QTextOption,
)
from PySide6.QtWidgets import (
    QApplication,
    QButtonGroup,
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QRadioButton,
    QMenu,
    QSizePolicy,
    QSplitter,
    QTabWidget,
    QToolBar,
    QToolButton,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from pdf_tools import APP_NAME
from pdf_tools.core.edits import apply_edits, flatten, remove_annots
from pdf_tools.core.fonts import DEFAULT_FONTS, base_font_name, draw_text, pdf_font_names, resolve_font, text_size
from pdf_tools.core.forms import is_on
from pdf_tools.core.ranges import format_ranges, parse_ranges
from pdf_tools.core.selection import flow_select, reading_order_words
from pdf_tools.ui import icons
from pdf_tools.ui.dialogs import ExportDialog, SignatureDialog, TextDialog
from pdf_tools.ui.page_view import ObjItem, PageView
from pdf_tools.ui.popover import Popover
from pdf_tools.ui.util import hex_to_rgb, rgb_to_hex, swatch_icon, to_qimage

THUMB_ZOOM = 0.22
PREVIEW_SCALE = 4
MIN_FIELD_PX = 5
SIGNATURE_WIDTH = 150
NOTE_SIZE = 18
PAGE_GAP = 12
PIXMAP_CACHE = 12
FIELD_STYLE = "background: rgb(220, 232, 255); color: black; border: none; padding: 0px;"
SKIP_ANNOTS = {pymupdf.PDF_ANNOT_POPUP, pymupdf.PDF_ANNOT_WIDGET}
MARKUP_ANNOTS = {
    pymupdf.PDF_ANNOT_HIGHLIGHT, pymupdf.PDF_ANNOT_UNDERLINE, pymupdf.PDF_ANNOT_STRIKE_OUT, pymupdf.PDF_ANNOT_SQUIGGLY,
}
TOOLS = (
    ("view", "View", "V", "Fill forms, drag to select text (Alt-drag: rectangle), click notes to read them (V)"),
    ("redact", "Redact", "R", "Drag over text to redact it; Alt-drag or Snap off for a free rectangle (R)"),
    ("highlight", "Highlight", "H", "Drag over text to highlight it, then add an optional comment (H)"),
    ("note", "Note", "N", "Click to add a sticky note; click one to read or edit it (N)"),
    ("text", "Text", "T", "Click to add a text box; double-click one to edit (T)"),
    ("sign", "Sign", "S", "Click to place a signature (S)"),
)
FLOW_COLOURS = {"view": QColor(26, 115, 232, 70), "redact": QColor(0, 0, 0, 90)}


ANNOT_LIST_STYLE = """
QTreeWidget::item { padding: 6px 4px; border-bottom: 1px solid rgba(128, 128, 128, 70); }
QTreeWidget::item:has-children { padding: 8px 4px 4px 4px; border-bottom: none; }
"""
NOTE_RGB = (255, 213, 79)
MARK_STYLES = {
    "highlight": "Highlight",
    "underline": "Underline",
    "strikeout": "Strikethrough",
    "squiggly": "Squiggly",
    "textcolour": "Text colour",
}
STYLE_ICONS = {
    "highlight": "highlight", "underline": "underline", "strikeout": "strikeout",
    "squiggly": "squiggly", "textcolour": "textcolour",
}
# tints for background highlights on white paper; text under them stays black
HIGHLIGHT_PRESETS = (
    ("Yellow", (255, 228, 92)),
    ("Green", (168, 230, 161)),
    ("Blue", (167, 211, 255)),
    ("Pink", (255, 179, 217)),
    ("Orange", (255, 196, 140)),
)
# inks for lines and text colour: each >= 4.5:1 contrast on white (WCAG 2.1 AA, 1.4.3)
INK_PRESETS = (
    ("Red", (198, 40, 40)),
    ("Blue", (21, 101, 192)),
    ("Green", (46, 125, 50)),
    ("Purple", (106, 27, 154)),
    ("Orange", (196, 62, 0)),
)


def palette_for(style):
    return HIGHLIGHT_PRESETS if style == "highlight" else INK_PRESETS


def colour_key(style):
    return "highlight_colour" if style == "highlight" else "ink_colour"


def shorten(text, limit):
    text = " ".join(text.split())
    return text if len(text) <= limit else text[: limit - 3] + "..."


class EditTab(QWidget):
    """Viewer/editor for one PDF: redactions, text, signatures, highlights, notes, form fields, search."""

    activate_requested = Signal()
    closed = Signal()
    open_dialog_requested = Signal()
    title_changed = Signal(str)

    def __init__(self, settings):
        super().__init__()
        self.settings = settings
        self.active = True
        self.doc = None
        self.doc_path = None
        self.pno = 0
        self.zoom = 1.5
        self.manual_rgb = (0, 0, 0)
        self.items = {}
        self.deleted = set()
        self.forms = {}
        self.form_original = {}
        self.form_editors = {}
        self.form_proxies = {}
        self.form_fit = {}
        self.form_edit_key = None
        self.history = []
        self.future = []
        self.matches = []
        self.match_idx = -1
        self.text_sel = None
        self.pending = None
        self.offsets = {}
        self.page_items = {}
        self.pix_cache = {}
        self.font_cache = {}
        self.preview_cache = {}
        self.annot_text = {}
        self.words_cache = {}
        self.flow_items = []
        self.popover = None

        self.thumbs = QListWidget()
        self.thumbs.setIconSize(QSize(130, 184))
        self.thumbs.setSpacing(4)
        self.thumbs.currentRowChanged.connect(self.goto)
        self.thumbs.itemChanged.connect(lambda _: self.update_state())

        self.annot_tree = QTreeWidget()
        self.annot_tree.setHeaderHidden(True)
        self.annot_tree.setIndentation(12)
        self.annot_tree.setIconSize(QSize(14, 14))
        self.annot_tree.setStyleSheet(ANNOT_LIST_STYLE)
        self.annot_tree.itemClicked.connect(lambda item, _: self.on_annot_entry(item))
        self.annot_tree.itemActivated.connect(lambda item, _: self.on_annot_entry(item))
        self.side = QTabWidget()
        self.side.setDocumentMode(True)
        self.side.addTab(self.thumbs, "Pages")
        self.side.addTab(self.annot_tree, "Annotations")
        self.side.setMinimumWidth(240)

        self.view = PageView(lambda: self.tool, self.selection_mode)
        self.view.pressed.connect(lambda: self.close_popover())
        self.view.context_requested.connect(self.on_context)
        self.view.band_selected.connect(self.on_band)
        self.view.flow_changed.connect(self.on_flow)
        self.view.clicked.connect(self.on_click)
        self.view.zoom_step.connect(lambda f: self.set_zoom(self.zoom * f))
        self.view.verticalScrollBar().valueChanged.connect(lambda _: self.on_scroll())

        self.viewer = QWidget()
        vlay = QVBoxLayout(self.viewer)
        vlay.setContentsMargins(0, 0, 0, 0)
        vlay.setSpacing(0)
        self.findbar = self.build_findbar()
        vlay.addWidget(self.findbar)
        vlay.addWidget(self.view)

        split = QSplitter()
        split.addWidget(self.side)
        split.addWidget(self.viewer)
        split.setStretchFactor(1, 1)
        split.setSizes([250, 950])
        self.layout_ = QVBoxLayout(self)
        self.layout_.setContentsMargins(0, 0, 0, 0)
        self.layout_.setSpacing(0)
        self.layout_.addWidget(split)

        self.info = QLabel()
        self.build_actions()
        self.update_state()

    def action(self, text, slot, shortcut=None, checkable=False):
        a = QAction(text, self)
        a.triggered.connect(lambda checked=False: slot())
        if isinstance(shortcut, (list, tuple)):
            a.setShortcuts([QKeySequence(s) for s in shortcut])
        elif shortcut:
            a.setShortcut(QKeySequence(shortcut))
        a.setCheckable(checkable)
        return a

    def status(self, text, ms=4000):
        self.window().statusBar().showMessage(text, ms)

    def build_findbar(self):
        bar = QWidget()
        lay = QHBoxLayout(bar)
        lay.setContentsMargins(6, 4, 6, 4)
        self.find_edit = QLineEdit()
        self.find_edit.setPlaceholderText("Find text")
        self.find_edit.setClearButtonEnabled(True)
        self.find_edit.setMinimumWidth(260)
        self.find_case = QToolButton()
        self.find_case.setText("Aa")
        self.find_case.setCheckable(True)
        self.find_case.setToolTip("Match case")
        prev_btn, next_btn, close_btn = QToolButton(), QToolButton(), QToolButton()
        prev_btn.setToolTip("Previous match (Shift+Enter)")
        next_btn.setToolTip("Next match (Enter)")
        for button, name in ((prev_btn, "prev"), (next_btn, "next"), (close_btn, "close")):
            icons.bind(button, name)
            button.setAutoRaise(True)
        close_btn.setToolTip("Close (Esc)")
        self.find_count = QLabel()
        self.find_count.setMinimumWidth(90)
        self.mark_this_btn = QPushButton("Redact this")
        self.mark_all_btn = QPushButton("Redact all")
        for w in (self.find_edit, self.find_case, prev_btn, next_btn, self.find_count):
            lay.addWidget(w)
        lay.addStretch()
        for w in (self.mark_this_btn, self.mark_all_btn, close_btn):
            lay.addWidget(w)

        self.find_timer = QTimer(self)
        self.find_timer.setSingleShot(True)
        self.find_timer.setInterval(250)
        self.find_timer.timeout.connect(self.run_search)
        self.find_edit.textChanged.connect(lambda _: self.find_timer.start())
        self.find_edit.returnPressed.connect(lambda: self.step_match(1))
        self.find_case.toggled.connect(lambda _: self.run_search())
        prev_btn.clicked.connect(lambda: self.step_match(-1))
        next_btn.clicked.connect(lambda: self.step_match(1))
        close_btn.clicked.connect(lambda: self.hide_find())
        self.mark_this_btn.clicked.connect(lambda: self.mark_current_match())
        self.mark_all_btn.clicked.connect(lambda: self.mark_all_matches())
        back = QShortcut(QKeySequence("Shift+Return"), self.find_edit)
        back.setContext(Qt.WidgetShortcut)
        back.activated.connect(lambda: self.step_match(-1))
        esc = QShortcut(QKeySequence("Escape"), bar)
        esc.setContext(Qt.WidgetWithChildrenShortcut)
        esc.activated.connect(self.hide_find)
        bar.hide()
        return bar

    def build_actions(self):
        s = self.settings
        self.a_find = self.action("Find...", self.show_find, QKeySequence.Find)
        self.a_open = self.action("Open...", self.open_dialog_requested.emit, QKeySequence.Open)
        self.a_close = self.action("Close", self.close_doc, "Ctrl+W")
        self.a_save = self.action("Save (overwrite original)", self.save, QKeySequence.Save)
        self.a_save_as = self.action("Save As...", self.save_as, QKeySequence.SaveAs)
        self.a_export = self.action("Export pages...", self.export_pages, "Ctrl+E")
        self.a_undo = self.action("Undo", self.undo, QKeySequence.Undo)
        self.a_redo = self.action("Redo", self.redo, [QKeySequence(QKeySequence.Redo).toString(), "Ctrl+Y"])
        self.a_copy = self.action("Copy text", self.copy_text, QKeySequence.Copy)
        self.a_delete = self.action("Delete selected", self.delete_selected, QKeySequence.Delete)
        self.a_clear_page = self.action("Clear page", self.clear_page)
        self.a_clear_all = self.action("Clear all", self.clear_all)
        self.a_tick = self.action("Tick all pages", lambda: self.tick_all(True))
        self.a_untick = self.action("Untick all pages", lambda: self.tick_all(False))
        self.a_zoom_in = self.action("Zoom in", lambda: self.set_zoom(self.zoom * 1.25), QKeySequence.ZoomIn)
        self.a_zoom_out = self.action("Zoom out", lambda: self.set_zoom(self.zoom / 1.25), QKeySequence.ZoomOut)
        self.a_fit = self.action("Fit width", self.fit_width, "Ctrl+0")
        self.a_prev = self.action("Previous page", lambda: self.goto(self.pno - 1), ["Left", "PgUp"])
        self.a_next = self.action("Next page", lambda: self.goto(self.pno + 1), ["Right", "PgDown"])
        self.a_continuous = self.action("Continuous", self.toggle_continuous, checkable=True)
        self.a_continuous.setChecked(s.get("view_mode") == "continuous")
        self.a_continuous.setToolTip("Scroll through all pages (off: one page at a time)")

        tools = QActionGroup(self)
        self.tool_actions = {}
        for key, label, shortcut, tip in TOOLS:
            a = self.action(label, self.on_tool, shortcut, checkable=True)
            a.setToolTip(tip)
            tools.addAction(a)
            self.tool_actions[key] = a
        self.tool_actions["view"].setChecked(True)
        self.a_snap = self.action("Snap to text", lambda: s.set("redact_snap_to_text", self.a_snap.isChecked()), checkable=True)
        self.a_snap.setChecked(s.get("redact_snap_to_text"))
        self.a_snap.setToolTip("Redact the text you drag over (off: a free rectangle)")
        self.a_auto = self.action("Auto fill", lambda: s.set("redact_auto_fill", self.a_auto.isChecked()), checkable=True)
        self.a_auto.setChecked(s.get("redact_auto_fill"))
        self.a_auto.setToolTip("Fill each redaction with the colour sampled around it")
        self.a_highlight_colour = self.action("Highlight colour...", self.pick_highlight_colour)
        self.build_toolbar()

    def tool_button(self, action=None, text=None):
        b = QToolButton()
        if action:
            b.setDefaultAction(action)
        if text:
            b.setText(text)
        return b

    def option_group(self, *widgets):
        w = QWidget()
        lay = QHBoxLayout(w)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(2)
        for x in widgets:
            lay.addWidget(x)
        return w

    def build_toolbar(self):
        """[undo redo] [tools] [active tool's options] ... [zoom] [page mode] [find] [save menu]"""
        for a, short in (
            (self.a_undo, "Undo"), (self.a_redo, "Redo"), (self.a_zoom_out, "-"), (self.a_zoom_in, "+"), (self.a_fit, "Fit"),
            (self.a_snap, "Snap"), (self.a_find, "Find"), (self.a_save, "Save"),
        ):
            a.setIconText(short)
        for a, name in (
            (self.a_undo, "undo"), (self.a_redo, "redo"), (self.a_zoom_out, "zoom-out"), (self.a_zoom_in, "zoom-in"),
            (self.a_fit, "fit"), (self.a_continuous, "continuous"), (self.a_find, "find"), (self.a_save, "save"),
            (self.a_save_as, "save"), (self.a_export, "export"), (self.a_open, "open"), (self.a_close, "close"),
            *((self.tool_actions[key], key) for key in self.tool_actions),
        ):
            icons.bind(a, name)
        tb = self.toolbar = QToolBar()
        tb.setIconSize(QSize(18, 18))
        tb.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        tb.addActions([self.a_undo, self.a_redo])
        tb.addSeparator()
        tb.addActions(list(self.tool_actions.values()))

        self.swatch = self.tool_button(text=" ")
        self.swatch.setIcon(swatch_icon(self.manual_rgb))
        self.swatch.setToolTip("Manual redaction colour (turns Auto fill off)")
        self.swatch.clicked.connect(lambda: self.pick_colour())
        self.opts_sep = tb.addSeparator()
        self.redact_opts = tb.addWidget(
            self.option_group(self.tool_button(self.a_snap), self.tool_button(self.a_auto), self.swatch)
        )
        self.highlight_opts = tb.addWidget(self.build_mark_options())

        spacer = QWidget()
        spacer.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        tb.addWidget(spacer)
        self.zoom_label = QLabel()
        self.zoom_label.setMinimumWidth(44)
        self.zoom_label.setAlignment(Qt.AlignCenter)
        tb.addAction(self.a_zoom_out)
        tb.addWidget(self.zoom_label)
        tb.addActions([self.a_zoom_in, self.a_fit])
        tb.addSeparator()
        tb.addAction(self.a_continuous)
        tb.addAction(self.a_find)
        tb.addSeparator()
        save = QToolButton()
        save.setDefaultAction(self.a_save)
        save.setPopupMode(QToolButton.MenuButtonPopup)
        save.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        menu = QMenu(save)
        menu.addActions([self.a_save, self.a_save_as, self.a_export])
        save.setMenu(menu)
        tb.addWidget(save)
        for a in (self.a_undo, self.a_redo, self.a_zoom_out, self.a_zoom_in, self.a_fit, self.a_continuous, self.a_find):
            tb.widgetForAction(a).setToolButtonStyle(Qt.ToolButtonIconOnly)
        self.on_tool()

    def build_mark_options(self):
        """Highlight tool options: a style menu button, preset colour swatches and a custom colour."""
        self.style_button = QToolButton()
        self.style_button.setPopupMode(QToolButton.InstantPopup)
        self.style_button.setToolButtonStyle(Qt.ToolButtonIconOnly)
        menu = QMenu(self.style_button)
        styles = QActionGroup(self)
        for key, label in MARK_STYLES.items():
            a = menu.addAction(label, lambda key=key: self.set_mark_style(key))
            a.setCheckable(True)
            styles.addAction(a)
            icons.bind(a, STYLE_ICONS[key])
        self.style_button.setMenu(menu)
        self.colour_group = QButtonGroup(self)
        self.colour_buttons = []
        for i in range(len(HIGHLIGHT_PRESETS)):
            b = self.tool_button(text=" ")
            b.setToolButtonStyle(Qt.ToolButtonIconOnly)
            b.setCheckable(True)
            b.clicked.connect(lambda _=False, i=i: self.set_mark_colour(palette_for(self.mark_style())[i][1]))
            self.colour_group.addButton(b)
            self.colour_buttons.append(b)
        self.custom_colour = self.tool_button(text="Custom colour...")
        self.custom_colour.setToolButtonStyle(Qt.ToolButtonIconOnly)
        self.custom_colour.setCheckable(True)
        self.custom_colour.setToolTip("Custom colour...")
        self.custom_colour.clicked.connect(lambda: self.pick_highlight_colour())
        self.colour_group.addButton(self.custom_colour)
        self.sync_mark_options()
        return self.option_group(self.style_button, *self.colour_buttons, self.custom_colour)

    def mark_style(self):
        style = self.settings.get("highlight_style")
        return style if style in MARK_STYLES else "highlight"

    def mark_colour(self, style=None):
        return hex_to_rgb(self.settings.get(colour_key(style or self.mark_style())))

    def sync_mark_options(self):
        """Show the active style and its palette (background tints for Highlight, dark inks for the others)."""
        style = self.mark_style()
        self.style_button.setText(MARK_STYLES[style])
        self.style_button.setToolTip(f"Mark style: {MARK_STYLES[style]}")
        icons.bind(self.style_button, STYLE_ICONS[style])
        for a in self.style_button.menu().actions():
            a.setChecked(a.text() == MARK_STYLES[style])
        current = self.mark_colour(style)
        preset = None
        for b, (name, rgb) in zip(self.colour_buttons, palette_for(style)):
            b.setIcon(swatch_icon(rgb))
            b.setToolTip(name)
            if rgb == current:
                preset = b
        (preset or self.custom_colour).setChecked(True)
        if preset:
            icons.bind(self.custom_colour, "custom-colour")
        else:
            self.custom_colour.setIcon(swatch_icon(current))

    def set_mark_style(self, style):
        self.settings.set("highlight_style", style)
        self.sync_mark_options()

    def set_mark_colour(self, rgb):
        self.settings.set(colour_key(self.mark_style()), rgb_to_hex(rgb))
        self.sync_mark_options()

    def on_tool(self):
        tool = self.tool
        self.redact_opts.setVisible(tool == "redact")
        self.highlight_opts.setVisible(tool == "highlight")
        self.opts_sep.setVisible(tool in ("redact", "highlight"))
        self.view.update_cursor()
        self.close_popover()
        self.clear_flow()

    @property
    def tool(self):
        if not hasattr(self, "tool_actions"):
            return "view"
        return next((k for k, a in self.tool_actions.items() if a.isChecked()), "view")

    @property
    def continuous(self):
        return self.a_continuous.isChecked()

    def load(self, path):
        self.activate_requested.emit()
        self.unload()
        self.doc_path = Path(path)
        self.settings.set("last_folder", str(self.doc_path.parent))
        self.settings.add_recent(self.doc_path.resolve())
        self.doc = pymupdf.open(path)
        self.pno = 0
        self.thumbs.blockSignals(True)
        for i, page in enumerate(self.doc):
            pix = page.get_pixmap(matrix=pymupdf.Matrix(THUMB_ZOOM, THUMB_ZOOM), alpha=False)
            item = QListWidgetItem(QIcon(QPixmap.fromImage(to_qimage(pix))), str(i + 1))
            item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
            item.setCheckState(Qt.Unchecked)
            self.thumbs.addItem(item)
        self.thumbs.setCurrentRow(0)
        self.thumbs.blockSignals(False)
        self.tool_actions["view"].setChecked(True)
        self.on_tool()
        self.fit_width()
        self.scroll_home()
        # scroll ranges can update after this event; repeat once the view has settled
        QTimer.singleShot(0, self.scroll_home)
        if self.findbar.isVisible():
            self.run_search()

    def scroll_home(self):
        self.view.verticalScrollBar().setValue(self.view.verticalScrollBar().minimum())
        self.view.horizontalScrollBar().setValue(self.view.horizontalScrollBar().minimum())

    def unload(self):
        self.close_popover(commit=False)
        self.flow_items = []
        self.detach_form_editors(self.form_editors.values())
        # clear the scene before closing the document: closing first crashes PySide (seen with form fields)
        self.view.scene().clear()
        if self.doc:
            self.doc.close()
        self.doc = None
        self.items, self.deleted, self.history, self.future = {}, set(), [], []
        self.annot_text, self.words_cache = {}, {}
        self.annot_tree.clear()
        self.side.setTabText(1, "Annotations")
        self.form_edit_key = None
        self.forms, self.form_original, self.form_editors, self.form_proxies, self.form_fit = {}, {}, {}, {}, {}
        self.matches, self.match_idx = [], -1
        self.text_sel = None
        self.offsets, self.page_items, self.pix_cache = {}, {}, {}
        self.font_cache.clear()
        self.preview_cache.clear()
        self.thumbs.blockSignals(True)
        self.thumbs.clear()
        self.thumbs.blockSignals(False)

    def reload_display(self):
        """Reopen the document for display with deleted annotations removed (never saved)."""
        self.doc.close()
        self.doc = pymupdf.open(self.doc_path)
        remove_annots(self.doc, self.deleted)
        self.pix_cache = {}

    def close_doc(self):
        if self.doc and self.confirm_discard():
            self.unload()
            self.update_state()
            self.closed.emit()

    def confirm_discard(self):
        n = self.edit_count()
        if not n:
            return True
        reply = QMessageBox.question(self, APP_NAME, f"Discard {n} unsaved edit(s)?")
        return reply == QMessageBox.Yes

    def edited(self, pages=None):
        return apply_edits(
            self.doc_path, self.items, pages, self.forms, self.shrunk_fields(), self.deleted, self.annot_text
        )

    def save(self):
        if not self.edit_count():
            return
        reply = QMessageBox.warning(
            self,
            "Overwrite original",
            f"Overwrite {self.doc_path.name} with the edited version?\n\n"
            "The original goes to the trash (restore with trash-restore).",
            QMessageBox.Yes | QMessageBox.Cancel,
            QMessageBox.Cancel,
        )
        if reply != QMessageBox.Yes:
            return
        data, leaks = self.edited()
        fd, tmp = tempfile.mkstemp(dir=self.doc_path.parent, prefix=f".{self.doc_path.name}.", suffix=".tmp")
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        path = self.doc_path
        if not QFile.moveToTrash(str(path)):
            os.unlink(tmp)
            QMessageBox.critical(self, APP_NAME, "Could not move the original to the trash; nothing changed.")
            return
        os.replace(tmp, path)
        self.items, self.forms, self.deleted, self.annot_text = {}, {}, set(), {}
        self.report(path, leaks)
        self.load(path)

    def save_as(self):
        if self.edit_count():
            self.write_copy(None, f"{self.doc_path.stem}-edited.pdf")

    def export_pages(self):
        if not self.doc:
            return
        s = self.settings
        ticked = [i for i in range(self.thumbs.count()) if self.thumbs.item(i).checkState() == Qt.Checked]
        ranges = format_ranges(ticked) or f"1-{len(self.doc)}"
        dlg = ExportDialog(self, len(self.doc), ranges, s.get("export_flatten"), s.get("export_dpi"))
        if dlg.exec() != QDialog.Accepted or not dlg.pages.text().strip():
            return
        text = dlg.pages.text()
        s.set("export_flatten", dlg.flat.isChecked())
        s.set("export_dpi", dlg.dpi.value())
        try:
            pages = parse_ranges(text, len(self.doc))
        except ValueError:
            QMessageBox.critical(self, APP_NAME, f"Invalid page range: {text}")
            return
        if pages:
            dpi = dlg.dpi.value() if dlg.flat.isChecked() else None
            name = f"{self.doc_path.stem}-p{format_ranges(pages).replace(',', '_')}{'-flat' if dpi else ''}.pdf"
            self.write_copy(pages, name, dpi)

    def write_copy(self, pages, default_name, flatten_dpi=None):
        out, _ = QFileDialog.getSaveFileName(self, "Save copy", str(self.doc_path.parent / default_name), "PDF (*.pdf)")
        if not out:
            return
        if Path(out).resolve() == self.doc_path.resolve():
            QMessageBox.critical(self, APP_NAME, "That is the open file; use File > Save to overwrite it.")
            return
        data, leaks = self.edited(pages)
        if flatten_dpi:
            data = flatten(data, flatten_dpi)
            check = pymupdf.open("pdf", data)
            leaks += [(i + 1, "selectable text after flatten") for i, p in enumerate(check) if p.get_text().strip()]
            check.close()
        Path(out).write_bytes(data)
        self.report(out, leaks)

    def report(self, out, leaks):
        if leaks:
            QMessageBox.warning(self, APP_NAME, f"Saved {out}\n\nText remains under redactions:\n{leaks[:10]}")
        else:
            self.status(f"Saved {out} - no text left under any redaction.", 8000)

    def edit_count(self):
        if not self.doc:
            return 0
        changed = len(set(self.annot_text) - self.deleted)
        return sum(len(v) for v in self.items.values()) + len(self.forms) + len(self.deleted) + changed

    def page_edits(self, pno):
        keyed = list(self.forms) + list(self.deleted) + list(set(self.annot_text) - self.deleted)
        return len(self.items.get(pno, [])) + sum(p == pno for p, _ in keyed)

    def page_rect(self, pno):
        r, o = self.doc[pno].rect, self.offsets[pno]
        return QRectF(o.x(), o.y(), r.width, r.height)

    def to_scene(self, rect, pno):
        d = (rect * self.doc[pno].rotation_matrix).normalize()
        o = self.offsets[pno]
        return QRectF(d.x0 + o.x(), d.y0 + o.y(), d.width, d.height)

    def to_pdf(self, r, pno):
        o = self.offsets[pno]
        rect = pymupdf.Rect(r.left() - o.x(), r.top() - o.y(), r.right() - o.x(), r.bottom() - o.y())
        return (rect * self.doc[pno].derotation_matrix).normalize()

    def to_pdf_point(self, pt, pno):
        o = self.offsets[pno]
        return pymupdf.Point(pt.x() - o.x(), pt.y() - o.y()) * self.doc[pno].derotation_matrix

    def page_at(self, pt):
        if not self.offsets:
            return None
        return min(self.offsets, key=lambda p: 0 if self.page_rect(p).contains(pt) else abs(self.page_rect(p).center().y() - pt.y()))

    def visible_rect(self):
        return self.view.mapToScene(self.view.viewport().rect()).boundingRect()

    def scroll_to_page(self, pno):
        top = self.view.mapFromScene(QPointF(0, self.offsets[pno].y() - PAGE_GAP / 2)).y()
        bar = self.view.verticalScrollBar()
        bar.setValue(bar.value() + top)

    def page_pix(self, pno, scale):
        cached = self.pix_cache.get(pno)
        if cached and cached[0] == scale:
            return cached[1]
        pix = self.doc[pno].get_pixmap(matrix=pymupdf.Matrix(scale, scale), alpha=False)
        self.pix_cache[pno] = (scale, pix)
        while len(self.pix_cache) > PIXMAP_CACHE:
            far = max(self.pix_cache, key=lambda p: abs(p - self.pno))
            del self.pix_cache[far]
        return pix

    def add_form_editors(self, scene, pno):
        proxies = []
        for widget in self.doc[pno].widgets():
            style = FIELD_STYLE
            kind, key = widget.field_type, (pno, widget.xref)
            self.form_original.setdefault(key, widget.field_value)
            value = self.forms.get(key, widget.field_value)
            rect = self.to_scene(widget.rect, pno)
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
            self.form_editors[key] = editor
            proxies.append(proxy)
        self.form_proxies[pno] = proxies

    def eventFilter(self, obj, event):
        # the wheel over a form field scrolls/zooms the page instead of the field (or changing a dropdown)
        if event.type() != QEvent.Wheel:
            return False
        if event.modifiers() & Qt.ControlModifier:
            self.set_zoom(self.zoom * (1.25 if event.angleDelta().y() > 0 else 0.8))
        else:
            bar = self.view.verticalScrollBar()
            bar.setValue(bar.value() - event.angleDelta().y())
        return True

    def page_has_focus(self, pno):
        focused = QApplication.focusWidget()
        return any(e is focused or e.isAncestorOf(focused) for k, e in self.form_editors.items() if k[0] == pno)

    def remove_form_editors(self, pno):
        self.detach_form_editors(e for k, e in self.form_editors.items() if k[0] == pno)
        for proxy in self.form_proxies.pop(pno, []):
            self.view.scene().removeItem(proxy)
        self.form_editors = {k: e for k, e in self.form_editors.items() if k[0] != pno}

    def detach_form_editors(self, editors):
        """Silence field widgets before the scene destroys them: signals or filter calls from a half-destroyed
        widget crash PySide."""
        for editor in list(editors):
            editor.blockSignals(True)
            editor.removeEventFilter(self)
            if isinstance(editor, QPlainTextEdit):
                editor.viewport().removeEventFilter(self)

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
        self.form_fit[key] = px if px < start else None

    def on_text(self, editor, key, size):
        self.fit_text(editor, key, size)
        self.set_form(key, editor.toPlainText() if isinstance(editor, QPlainTextEdit) else editor.text())

    def shrunk_fields(self):
        return {k: px for k, px in self.form_fit.items() if px and k in self.forms}

    def set_form(self, key, value, coalesce=True, record=True):
        """Store a field value; consecutive typing in one field is a single undo step."""
        if record and not (coalesce and key == self.form_edit_key):
            self.push_undo(self.snapshot())
        self.form_edit_key = key if coalesce else None
        original = self.form_original.get(key)
        if isinstance(value, bool):
            original = is_on(original)
        if value == original or (not value and not original):
            self.forms.pop(key, None)
        else:
            self.forms[key] = value
        self.update_state()

    def set_check(self, key, name, on):
        self.set_form(key, on, coalesce=False)
        if not (on and isinstance(self.form_editors.get(key), QRadioButton)):
            return
        for other, editor in self.form_editors.items():
            if other != key and isinstance(editor, QRadioButton) and editor.toolTip() == name:
                editor.blockSignals(True)
                editor.setChecked(False)
                editor.blockSignals(False)
                self.set_form(other, False, coalesce=False, record=False)

    def font_for(self, name, text):
        key = (name, text)
        if key not in self.font_cache:
            self.font_cache[key] = resolve_font(self.doc, name, text)
        return self.font_cache[key]

    def text_image(self, obj):
        key = (obj["text"], obj["font"], obj["size"], obj["rgb"])
        if key not in self.preview_cache:
            w, h = obj["rect"].width, obj["rect"].height
            tmp = pymupdf.open()
            page = tmp.new_page(width=w + 2, height=h + 2)
            draw_text(page, dict(obj, rect=pymupdf.Rect(0, 0, w, h)), self.doc)
            matrix = pymupdf.Matrix(PREVIEW_SCALE, PREVIEW_SCALE)
            pix = page.get_pixmap(matrix=matrix, alpha=True, clip=pymupdf.Rect(0, 0, w, h))
            self.preview_cache[key] = to_qimage(pix)
            tmp.close()
        return self.preview_cache[key]

    def obj_item(self, obj, pno):
        rect = self.to_scene(obj["rect"], pno)
        image, parts = None, ()
        if obj["kind"] == "text":
            image = self.text_image(obj)
        elif obj["kind"] == "sign":
            image = QImage.fromData(obj["png"])
        elif obj["kind"] == "highlight":
            parts = [self.to_scene(r, pno).translated(-rect.topLeft()) for r in obj["rects"]]
        return ObjItem(self, obj, pno, rect, image, parts)

    def scene_pages(self):
        return list(range(len(self.doc))) if self.continuous else [self.pno]

    def render(self, scroll_to=None):
        """Rebuild the scene: page frames, edits, existing annotations, overlays; pixmaps come from the cache."""
        if not self.doc:
            return
        self.close_popover()
        scene = self.view.scene()
        keep = [i.obj for i in scene.selectedItems() if isinstance(i, ObjItem)]
        hpos, vpos = self.view.horizontalScrollBar().value(), self.view.verticalScrollBar().value()
        self.view.verticalScrollBar().blockSignals(True)
        self.flow_items = []
        self.detach_form_editors(self.form_editors.values())
        scene.clear()
        self.page_items, self.form_editors, self.form_proxies, self.offsets = {}, {}, {}, {}
        pages = self.scene_pages()
        width = max(self.doc[p].rect.width for p in pages)
        y = 0.0
        for p in pages:
            r = self.doc[p].rect
            self.offsets[p] = QPointF((width - r.width) / 2, y)
            frame = scene.addRect(self.page_rect(p), Qt.NoPen, QBrush(Qt.white))
            frame.setZValue(-1)
            y += r.height + PAGE_GAP
        scene.setSceneRect(QRectF(-PAGE_GAP, -PAGE_GAP, width + 2 * PAGE_GAP, y + PAGE_GAP))
        self.view.resetTransform()
        self.view.scale(self.zoom, self.zoom)
        for p in pages:
            for obj in self.items.get(p, []):
                it = self.obj_item(obj, p)
                scene.addItem(it)
                it.setSelected(any(o is obj for o in keep))
            for annot in self.doc[p].annots():
                if annot.type[0] not in SKIP_ANNOTS:
                    label = self.annot_comment(p, annot) or f"{annot.type[1]} annotation - click to comment or delete"
                    obj = {"kind": "annot", "xref": annot.xref, "rect": annot.rect, "label": label, "type": annot.type[1]}
                    scene.addItem(ObjItem(self, obj, p, self.to_scene(annot.rect, p)))
        overlays = [
            (p, rect, QColor(255, 140, 0, 130) if i == self.match_idx else QColor(255, 225, 0, 110))
            for i, (p, rect) in enumerate(self.matches) if p in self.offsets
        ]
        if self.text_sel and self.text_sel[0] in self.offsets:
            overlays += [(self.text_sel[0], r, QColor(26, 115, 232, 70)) for r in self.text_sel[1]]
        for p, rect, colour in overlays:
            hl = scene.addRect(self.to_scene(rect, p), Qt.NoPen, QBrush(colour))
            hl.setZValue(2)
            hl.setAcceptedMouseButtons(Qt.NoButton)
        self.view.horizontalScrollBar().setValue(hpos)
        self.view.verticalScrollBar().setValue(vpos)
        if scroll_to is not None:
            self.scroll_to_page(scroll_to)
        self.view.verticalScrollBar().blockSignals(False)
        self.render_visible()
        self.refresh_annotations()
        self.update_state()

    def annot_comment(self, pno, annot):
        return self.annot_text.get((pno, annot.xref), annot.info.get("content", ""))

    def refresh_annotations(self):
        """List notes and highlights (ours and the PDF's) per page in the side panel."""
        tree = self.annot_tree
        tree.clear()
        count = 0
        bold = tree.font()
        bold.setBold(True)
        for p in range(len(self.doc)):
            entries = []
            for obj in self.items.get(p, []):
                if obj["kind"] in ("highlight", "note"):
                    rgb = obj.get("rgb", NOTE_RGB)
                    label = MARK_STYLES[obj.get("style", "highlight")] if obj["kind"] == "highlight" else "Note"
                    entries.append((label, obj.get("quote", ""), obj.get("text", ""), rgb, ("obj", obj)))
            page = self.doc[p]
            for annot in page.annots():
                if annot.type[0] in SKIP_ANNOTS:
                    continue
                quote = page.get_textbox(annot.rect).strip() if annot.type[0] in MARKUP_ANNOTS else ""
                stroke = (annot.colors or {}).get("stroke") or (0.6, 0.6, 0.6)
                rgb = tuple(round(c * 255) for c in stroke[:3])
                entries.append((annot.type[1], quote, self.annot_comment(p, annot), rgb, ("annot", annot.xref)))
            if not entries:
                continue
            top = QTreeWidgetItem([f"Page {p + 1}  ({len(entries)})"])
            top.setFlags(Qt.ItemIsEnabled)
            top.setFont(0, bold)
            for kind, quote, comment, rgb, ref in entries:
                lines = [kind]
                if quote:
                    lines.append(f'"{shorten(quote, 34)}"')
                if comment:
                    lines.append(f"Comment: {shorten(comment, 30)}")
                child = QTreeWidgetItem(["\n".join(lines)])
                child.setIcon(0, swatch_icon(rgb))
                child.setData(0, Qt.UserRole, (p, ref))
                child.setToolTip(0, "\n\n".join(x for x in (quote, comment) if x) or kind)
                top.addChild(child)
                count += 1
            tree.addTopLevelItem(top)
            top.setExpanded(True)
        self.side.setTabText(1, f"Annotations ({count})" if count else "Annotations")

    def scene_item(self, pno, ref):
        for it in self.view.scene().items():
            if isinstance(it, ObjItem) and it.pno == pno:
                if (ref[0] == "obj" and it.obj is ref[1]) or (ref[0] == "annot" and it.kind == "annot" and it.obj["xref"] == ref[1]):
                    return it
        return None

    def on_annot_entry(self, entry):
        data = entry.data(0, Qt.UserRole)
        if not data:
            return
        pno, ref = data
        if pno not in self.offsets:
            self.goto(pno)
        it = self.scene_item(pno, ref)
        if it:
            self.view.scene().clearSelection()
            it.setSelected(True)
            self.view.centerOn(it.sceneBoundingRect().center())
            self.item_clicked(it.obj, pno, it.kind)

    def render_visible(self):
        """Attach page pixmaps and form editors for pages near the viewport; drop far ones."""
        if not self.doc:
            return
        scene = self.view.scene()
        visible = self.visible_rect()
        near = visible.adjusted(0, -visible.height(), 0, visible.height())
        scale = self.zoom * self.devicePixelRatioF()
        self.scale = scale
        for p in list(self.offsets):
            if self.page_rect(p).intersects(near):
                if p not in self.page_items:
                    item = scene.addPixmap(QPixmap.fromImage(to_qimage(self.page_pix(p, scale))))
                    item.setScale(1 / scale)
                    item.setPos(self.offsets[p])
                    item.setTransformationMode(Qt.SmoothTransformation)
                    item.setZValue(0)
                    self.page_items[p] = item
                    self.add_form_editors(scene, p)
            elif p in self.page_items and not self.page_has_focus(p):
                scene.removeItem(self.page_items.pop(p))
                self.remove_form_editors(p)

    def on_scroll(self):
        if not self.doc:
            return
        self.close_popover()
        self.render_visible()
        if not self.continuous:
            return
        p = self.page_at(self.view.mapToScene(self.view.viewport().rect().center()))
        if p is not None and p != self.pno:
            self.pno = p
            self.sync_thumb()
            self.update_state()

    def sync_thumb(self):
        if self.thumbs.currentRow() != self.pno:
            self.thumbs.blockSignals(True)
            self.thumbs.setCurrentRow(self.pno)
            self.thumbs.blockSignals(False)

    def update_state(self):
        if self.doc:
            self.title_changed.emit(f"{'* ' if self.edit_count() else ''}{self.doc_path.name}")
        has_doc = self.doc is not None and self.active
        n = self.edit_count() if self.active else 0
        for a in (
            self.a_close, self.a_export, self.a_find, self.a_zoom_in, self.a_zoom_out, self.a_fit, self.a_prev,
            self.a_next, self.a_delete, self.a_clear_page, self.a_continuous, *self.tool_actions.values(),
        ):
            a.setEnabled(has_doc)
        m = len(self.matches)
        self.find_count.setText(
            f"{self.match_idx + 1} of {m}" if m else ("No matches" if self.find_edit.text().strip() else "")
        )
        self.mark_this_btn.setEnabled(m > 0)
        self.mark_all_btn.setEnabled(m > 0)
        self.mark_all_btn.setText(f"Redact all ({m})" if m else "Redact all")
        for a in (self.a_save, self.a_save_as, self.a_clear_all):
            a.setEnabled(n > 0)
        self.a_undo.setEnabled(has_doc and bool(self.history))
        self.a_redo.setEnabled(has_doc and bool(self.future))
        self.a_copy.setEnabled(has_doc and bool(self.text_sel))
        self.info.setVisible(self.active)
        if not self.active:
            return
        if not has_doc:
            self.window().setWindowTitle(APP_NAME)
            self.zoom_label.setText("")
            self.info.setText("Open a PDF (Ctrl+O) or drop one on the window")
            return
        self.window().setWindowTitle(f"{'* ' if n else ''}{self.doc_path.name} - {APP_NAME}")
        self.zoom_label.setText(f"{round(self.zoom * 100)}%")
        self.thumbs.blockSignals(True)
        ticked = 0
        for i in range(self.thumbs.count()):
            item = self.thumbs.item(i)
            k = self.page_edits(i)
            item.setText(f"{i + 1}" + (f"\n{k} edit{'s' * (k != 1)}" if k else ""))
            item.setForeground(QColor("#d93025") if k else self.thumbs.palette().text().color())
            ticked += item.checkState() == Qt.Checked
        self.thumbs.blockSignals(False)
        pages = sum(1 for i in range(len(self.doc)) if self.page_edits(i))
        self.info.setText(
            f"page {self.pno + 1}/{len(self.doc)}   |   {n} edit(s) on {pages} page(s)   |   "
            f"{ticked} ticked for export   |   {self.doc_path}"
        )

    def goto(self, pno):
        if not self.doc or not 0 <= pno < len(self.doc):
            return
        self.pno = pno
        self.sync_thumb()
        if self.continuous and pno in self.offsets:
            self.scroll_to_page(pno)
            self.update_state()
        else:
            self.render()
            self.view.verticalScrollBar().setValue(0)

    def toggle_continuous(self):
        self.settings.set("view_mode", "continuous" if self.continuous else "single")
        if self.doc:
            self.render(scroll_to=self.pno)

    def set_zoom(self, z):
        if self.doc:
            self.zoom = min(max(z, 0.3), 6)
            self.render(scroll_to=self.pno if self.continuous else None)

    def fit_width(self):
        if self.doc:
            width = self.view.viewport().width()
            page_w = max(page.rect.width for page in self.doc)
            self.set_zoom((width - 40) / page_w if width > 100 else 1.5)

    def tick_all(self, value):
        for i in range(self.thumbs.count()):
            self.thumbs.item(i).setCheckState(Qt.Checked if value else Qt.Unchecked)

    def pick_colour(self):
        c = QColorDialog.getColor(QColor(*self.manual_rgb), self, "Redaction colour")
        if c.isValid():
            self.manual_rgb = (c.red(), c.green(), c.blue())
            self.swatch.setIcon(swatch_icon(self.manual_rgb))
            self.a_auto.setChecked(False)
            self.settings.set("redact_auto_fill", False)

    def pick_highlight_colour(self):
        c = QColorDialog.getColor(QColor(*self.mark_colour()), self, f"{MARK_STYLES[self.mark_style()]} colour")
        if c.isValid():
            self.set_mark_colour((c.red(), c.green(), c.blue()))
        else:
            self.sync_mark_options()

    def snapshot(self):
        items = {
            p: [dict(o, rect=pymupdf.Rect(o["rect"]), **({"rects": list(o["rects"])} if "rects" in o else {})) for o in objs]
            for p, objs in self.items.items()
        }
        return items, frozenset(self.deleted), dict(self.forms), dict(self.annot_text)

    def push_undo(self, snap):
        self.history.append(snap)
        self.future.clear()
        self.form_edit_key = None

    def begin_change(self):
        self.pending = self.snapshot()

    def end_change(self, clicked=None):
        changed = False
        for it in self.view.scene().items():
            if isinstance(it, ObjItem) and it.kind not in ("annot", "highlight"):
                rect = self.to_pdf(it.mapRectToScene(it.rect()), it.pno)
                if max(abs(a - b) for a, b in zip(rect, it.obj["rect"])) > 0.01:
                    it.obj["rect"] = rect
                    changed = True
        if changed and self.pending is not None:
            self.push_undo(self.pending)
        self.pending = None
        # deferred: called from the item's own release handler, and both paths rebuild the scene
        if changed:
            QTimer.singleShot(0, self.render)
        elif clicked is not None and clicked.kind in ("note", "highlight", "annot"):
            obj, pno, kind = clicked.obj, clicked.pno, clicked.kind
            QTimer.singleShot(0, lambda: self.item_clicked(obj, pno, kind))

    def restore(self, snap):
        before, before_deleted, before_forms = self.items, self.deleted, self.forms
        items, deleted, forms, annot_text = snap
        self.items, self.deleted, self.forms, self.annot_text = items, set(deleted), dict(forms), dict(annot_text)
        self.form_edit_key = None
        if self.deleted != before_deleted:
            self.reload_display()
        touched = {p for p in set(before) | set(self.items) if before.get(p) != self.items.get(p)}
        touched |= {p for p, _ in before_deleted ^ self.deleted}
        touched |= {key[0] for key, _ in set(before_forms.items()) ^ set(self.forms.items())}
        first = min(touched) if touched else None
        if first is not None and first not in self.offsets:
            self.goto(first)
        else:
            self.render()

    def undo(self):
        if self.doc and self.history:
            self.future.append(self.snapshot())
            self.restore(self.history.pop())

    def redo(self):
        if self.doc and self.future:
            self.history.append(self.snapshot())
            self.restore(self.future.pop())

    def owner(self, obj):
        return next(p for p, objs in self.items.items() if any(o is obj for o in objs))

    def delete_selected(self):
        entry = self.annot_tree.currentItem()
        if self.annot_tree.hasFocus() and entry and entry.data(0, Qt.UserRole):
            pno, ref = entry.data(0, Qt.UserRole)
            if pno not in self.offsets:
                self.goto(pno)
            it = self.scene_item(pno, ref)
            if it:
                self.view.scene().clearSelection()
                it.setSelected(True)
        selected = [i for i in self.view.scene().selectedItems() if isinstance(i, ObjItem)]
        if not selected:
            return
        self.push_undo(self.snapshot())
        annots = {(i.pno, i.obj["xref"]) for i in selected if i.kind == "annot"}
        for i in selected:
            if i.kind != "annot":
                self.items[i.pno] = [o for o in self.items.get(i.pno, []) if o is not i.obj]
        if annots:
            self.deleted |= annots
            self.reload_display()
        self.render()

    def clear_page(self):
        if self.doc and self.items.get(self.pno):
            self.push_undo(self.snapshot())
            del self.items[self.pno]
            self.render()

    def clear_all(self):
        n = sum(len(v) for v in self.items.values()) + len(self.deleted)
        if n and QMessageBox.question(self, APP_NAME, f"Remove all {n} edit(s)?") == QMessageBox.Yes:
            self.push_undo(self.snapshot())
            had_deleted = bool(self.deleted)
            self.items, self.deleted = {}, set()
            if had_deleted:
                self.reload_display()
            self.render()

    def sample_background(self, pno, rect):
        cached = self.pix_cache.get(pno)
        scale, pix = cached if cached else (2.0, self.page_pix(pno, 2.0))
        d = (rect * self.doc[pno].rotation_matrix).normalize() * scale
        x0, y0, x1, y1 = int(d.x0) - 4, int(d.y0) - 4, int(d.x1) + 4, int(d.y1) + 4
        w, h = pix.width, pix.height
        ring = [(x, y) for x in range(x0, x1 + 1) for y in (y0, y1)]
        ring += [(x, y) for y in range(y0, y1 + 1) for x in (x0, x1)]
        px = [pix.pixel(x, y) for x, y in ring if 0 <= x < w and 0 <= y < h]
        if not px:
            return (255, 255, 255)
        return tuple(int(statistics.median(ch)) for ch in zip(*px))

    def add_redaction(self, pno, rect):
        if rect.is_empty or rect.width < 1 or rect.height < 1:
            return False
        if any(o["kind"] == "redact" and o["rect"] == rect for o in self.items.get(pno, [])):
            return False
        rgb = self.sample_background(pno, rect) if self.a_auto.isChecked() else self.manual_rgb
        self.items.setdefault(pno, []).append({"kind": "redact", "rect": rect, "rgb": rgb})
        return True

    def line_rects(self, sel, pno):
        lines, words = {}, {}
        for x0, y0, x1, y1, word, block, line, wno in self.doc[pno].get_text("words"):
            w = pymupdf.Rect(x0, y0, x1, y1)
            if w.intersects(sel):
                lines.setdefault((block, line), pymupdf.Rect(w)).include_rect(w)
                words.setdefault((block, line), []).append((wno, word))
        order = sorted(lines, key=lambda k: (lines[k].y0, lines[k].x0))
        text = "\n".join(" ".join(w for _, w in sorted(words[k])) for k in order)
        return [lines[k] for k in order], text

    def selection_mode(self, modifiers):
        """'flow' (text like a viewer), 'rect' (Alt-drag, or Redact with Snap off) or None (click tools)."""
        alt = bool(modifiers & Qt.AltModifier)
        if self.tool in ("view", "highlight"):
            return "rect" if alt else "flow"
        if self.tool == "redact":
            return "rect" if alt or not self.a_snap.isChecked() else "flow"
        return None

    def page_words(self, pno):
        if pno not in self.words_cache:
            self.words_cache[pno] = reading_order_words(self.doc[pno])
        return self.words_cache[pno]

    def clear_flow(self):
        scene = self.view.scene()
        for item in self.flow_items:
            scene.removeItem(item)
        self.flow_items = []

    def on_flow(self, start, end, done):
        pno = self.page_at(start) if self.doc else None
        if pno is None:
            return
        rects, text = flow_select(self.page_words(pno), self.to_pdf_point(start, pno), self.to_pdf_point(end, pno))
        self.clear_flow()
        if not done:
            colour = FLOW_COLOURS.get(self.tool) or QColor(*self.mark_colour(), 120)
            for rect in rects:
                item = self.view.scene().addRect(self.to_scene(rect, pno), Qt.NoPen, QBrush(colour))
                item.setZValue(2)
                item.setAcceptedMouseButtons(Qt.NoButton)
                self.flow_items.append(item)
            return
        self.apply_selection(pno, rects, text)

    def apply_selection(self, pno, rects, text):
        if self.tool == "view":
            self.text_sel = (pno, rects, text) if text else None
            if text:
                self.status(f"Selected {len(text)} characters - Ctrl+C to copy")
            self.render()
        elif self.tool == "highlight" and rects:
            self.add_highlight(pno, rects, text, ask_comment=True)
        elif self.tool == "redact":
            self.redact_rects(pno, rects)

    def add_highlight(self, pno, rects, text, ask_comment=False, style=None):
        bounds = pymupdf.Rect(rects[0])
        for rect in rects[1:]:
            bounds.include_rect(rect)
        style = style if style in MARK_STYLES else self.mark_style()
        rgb = self.mark_colour(style)
        obj = {"kind": "highlight", "style": style, "rect": bounds, "rects": rects, "rgb": rgb, "quote": text, "text": ""}
        self.add_obj(pno, obj)
        if ask_comment:
            self.show_mark_popover(pno, obj, f"{MARK_STYLES[style]} - comment (optional)")

    def show_mark_popover(self, pno, obj, title):
        self.show_popover(
            title, obj.get("text", ""), self.to_scene(obj["rect"], pno),
            lambda t: self.set_comment(pno, obj, t), lambda: self.remove_obj(pno, obj),
            colours=palette_for(obj.get("style", "highlight")), colour=obj["rgb"],
            on_colour=lambda rgb: self.recolour(pno, obj, rgb),
        )

    def recolour(self, pno, obj, rgb):
        """Change a placed mark's colour in place, without rebuilding the scene (which would close the popover)."""
        if tuple(obj["rgb"]) == tuple(rgb):
            return
        self.push_undo(self.snapshot())
        obj["rgb"] = tuple(rgb)
        for it in self.view.scene().items():
            if isinstance(it, ObjItem) and it.obj is obj:
                it.update()
        self.refresh_annotations()
        self.update_state()

    def redact_rects(self, pno, rects):
        snap = self.snapshot()
        if [rect for rect in rects if self.add_redaction(pno, rect)]:
            self.push_undo(snap)
        self.render()

    def on_context(self, scene_pos, global_pos):
        """Right-click on a text selection: copy, highlight (optionally with a comment) or redact it."""
        if not (self.doc and self.text_sel and self.text_sel[0] in self.offsets):
            return
        pno, rects, text = self.text_sel
        menu = QMenu(self)
        menu.addAction("Copy", self.copy_text)
        menu.addSeparator()
        for key, label in MARK_STYLES.items():
            a = menu.addAction(label, lambda key=key: self.from_selection(lambda: self.add_highlight(pno, rects, text, style=key)))
            icons.bind(a, STYLE_ICONS[key])
        colours = menu.addMenu(f"{MARK_STYLES[self.mark_style()]} colour")
        current = self.mark_colour()
        for name, rgb in palette_for(self.mark_style()):
            a = colours.addAction(swatch_icon(rgb), name, lambda rgb=rgb: self.set_mark_colour(rgb))
            a.setCheckable(True)
            a.setChecked(rgb == current)
        menu.addAction(
            "Mark with comment...",
            lambda: self.from_selection(lambda: self.add_highlight(pno, rects, text, ask_comment=True)),
        )
        menu.addSeparator()
        menu.addAction("Redact", lambda: self.from_selection(lambda: self.redact_rects(pno, rects)))
        menu.exec(global_pos)

    def from_selection(self, act):
        self.text_sel = None
        act()

    def on_band(self, r):
        if not self.doc:
            return
        pno = self.page_at(r.center())
        if pno is None:
            return
        sel = self.to_pdf(r, pno)
        if self.tool == "redact":
            snap = self.snapshot()
            if self.add_redaction(pno, sel):
                self.push_undo(snap)
            self.render()
        else:
            self.apply_selection(pno, *self.line_rects(sel, pno))

    def close_popover(self, commit=True):
        pop, self.popover = self.popover, None
        if pop:
            pop.dismiss(commit)

    def show_popover(self, title, text, anchor, on_commit, on_delete=None, **colour_args):
        """Open the comment editor under `anchor` (a scene rect), clamped inside the view."""
        self.close_popover()
        pop = Popover(self.view.viewport(), title, text, on_commit, on_delete, **colour_args)
        pop.adjustSize()
        vp = self.view.viewport().rect()
        at = self.view.mapFromScene(anchor.bottomLeft()) + QPoint(0, 6)
        if at.y() + pop.height() > vp.bottom():
            at.setY(self.view.mapFromScene(anchor.topLeft()).y() - pop.height() - 6)
        at.setX(min(max(at.x(), 4), vp.right() - pop.width() - 4))
        at.setY(min(max(at.y(), 4), vp.bottom() - pop.height() - 4))
        pop.move(at)
        pop.show()
        pop.edit.setFocus()
        self.popover = pop

    def item_clicked(self, obj, pno, kind):
        if not self.doc or pno not in self.offsets:
            return
        anchor = self.to_scene(obj["rect"], pno)
        if kind == "annot":
            key = (pno, obj["xref"])
            page = self.doc[pno]
            annot = page.load_annot(obj["xref"])
            text = self.annot_comment(pno, annot) if annot else ""
            self.show_popover(
                f"{obj.get('type', 'Annotation')} (from the PDF)", text, anchor,
                lambda t: self.set_annot_text(key, t), lambda: self.delete_annot(key),
            )
        elif kind == "note":
            self.show_popover(
                "Note", obj.get("text", ""), anchor,
                lambda t: self.set_comment(pno, obj, t), lambda: self.remove_obj(pno, obj),
            )
        else:
            self.show_mark_popover(pno, obj, f"{MARK_STYLES[obj.get('style', 'highlight')]} - comment")

    def set_comment(self, pno, obj, text):
        snap = self.snapshot()
        if obj["kind"] == "note" and not text:
            self.items[pno] = [o for o in self.items.get(pno, []) if o is not obj]
        else:
            obj["text"] = text
        self.push_undo(snap)
        self.render()

    def remove_obj(self, pno, obj):
        snap = self.snapshot()
        self.items[pno] = [o for o in self.items.get(pno, []) if o is not obj]
        self.push_undo(snap)
        self.render()

    def set_annot_text(self, key, text):
        snap = self.snapshot()
        self.annot_text[key] = text
        self.push_undo(snap)
        self.render()

    def delete_annot(self, key):
        snap = self.snapshot()
        self.deleted.add(key)
        self.reload_display()
        self.push_undo(snap)
        self.render()

    def on_click(self, pt):
        if not self.doc:
            return
        if self.text_sel:
            self.text_sel = None
            self.render()
        pno = self.page_at(pt)
        if pno is None or not self.page_rect(pno).contains(pt):
            return
        pos = self.to_pdf_point(pt, pno)
        if self.tool == "text":
            self.place_text(pos, pno)
        elif self.tool == "sign":
            self.place_signature(pos, pno)
        elif self.tool == "note":
            self.place_note(pos, pno)

    def copy_text(self):
        if self.text_sel:
            QApplication.clipboard().setText(self.text_sel[2])
            self.status("Copied", 2000)

    def nearby_style(self, pos, pno):
        best, style = None, ("Helvetica", 10.0, (0, 0, 0))
        for block in self.doc[pno].get_text("dict")["blocks"]:
            for line in block.get("lines", []):
                for span in line["spans"]:
                    if not span["text"].strip():
                        continue
                    r = pymupdf.Rect(span["bbox"])
                    dx = max(r.x0 - pos.x, 0, pos.x - r.x1)
                    dy = max(r.y0 - pos.y, 0, pos.y - r.y1)
                    d = dx * dx + dy * dy
                    if best is None or d < best:
                        c = span["color"]
                        best = d
                        rgb = (c >> 16 & 255, c >> 8 & 255, c & 255)
                        style = (base_font_name(span["font"]), round(span["size"] * 2) / 2, rgb)
        return style

    def font_choices(self):
        names = pdf_font_names(self.doc)
        return names + [f for f in DEFAULT_FONTS if f not in names]

    def text_dialog(self, text, font, size, rgb):
        dlg = TextDialog(self, self.doc, self.font_choices(), text, font, size, rgb)
        return dlg.values() if dlg.exec() == QDialog.Accepted else None

    def sized(self, obj):
        w, h = text_size(self.font_for(obj["font"], obj["text"])[2], obj)
        obj["rect"] = pymupdf.Rect(obj["rect"].x0, obj["rect"].y0, obj["rect"].x0 + w, obj["rect"].y0 + h)
        return obj

    def add_obj(self, pno, obj):
        snap = self.snapshot()
        self.items.setdefault(pno, []).append(obj)
        self.push_undo(snap)
        self.render()

    def place_text(self, pos, pno):
        font, size, rgb = self.nearby_style(pos, pno)
        values = self.text_dialog("", font, size, rgb)
        if values and values[0].strip():
            text, font, size, rgb = values
            obj = {"kind": "text", "rect": pymupdf.Rect(pos, pos), "text": text, "font": font, "size": size, "rgb": rgb}
            self.add_obj(pno, self.sized(obj))

    def place_note(self, pos, pno):
        rect = pymupdf.Rect(pos.x, pos.y, pos.x + NOTE_SIZE, pos.y + NOTE_SIZE)
        self.show_popover(
            "New note", "", self.to_scene(rect, pno),
            lambda t: t and self.add_obj(pno, {"kind": "note", "rect": rect, "text": t}),
        )

    def place_signature(self, pos, pno):
        dlg = SignatureDialog(self, self.settings.get("last_signature"))
        if dlg.exec() != QDialog.Accepted or not dlg.selected_path():
            return
        path = dlg.selected_path()
        self.settings.set("last_signature", path.name)
        data = path.read_bytes()
        img = QImage.fromData(data)
        w = SIGNATURE_WIDTH
        h = w * img.height() / max(img.width(), 1)
        rect = pymupdf.Rect(pos.x - w / 2, pos.y - h / 2, pos.x + w / 2, pos.y + h / 2)
        self.add_obj(pno, {"kind": "sign", "rect": rect, "png": data})

    def edit_obj(self, obj):
        self.pending = None
        pno = self.owner(obj)
        values = self.text_dialog(obj["text"], obj["font"], obj["size"], obj["rgb"])
        if not values:
            return
        snap = self.snapshot()
        if values[0].strip():
            obj.update(zip(("text", "font", "size", "rgb"), values))
            self.sized(obj)
        else:
            self.items[pno] = [o for o in self.items[pno] if o is not obj]
        self.push_undo(snap)
        self.render()

    def show_find(self):
        if not self.doc:
            return
        self.findbar.show()
        self.find_edit.setFocus()
        self.find_edit.selectAll()
        if self.find_edit.text().strip():
            self.run_search()

    def hide_find(self):
        self.findbar.hide()
        self.matches, self.match_idx = [], -1
        if self.doc:
            self.render()
            self.view.setFocus()

    def run_search(self):
        self.find_timer.stop()
        self.matches, self.match_idx = [], -1
        query = self.find_edit.text()
        if self.doc and query.strip():
            for pno, page in enumerate(self.doc):
                for rect in sorted(page.search_for(query), key=lambda r: (round(r.y0), r.x0)):
                    # search_for ignores case, so re-check the text under the hit
                    if not self.find_case.isChecked() or query in page.get_textbox(rect + (-1, -1, 1, 1)):
                        self.matches.append((pno, rect))
        if self.matches:
            self.match_idx = next((i for i, (p, _) in enumerate(self.matches) if p >= self.pno), 0)
            self.show_match()
        elif self.doc:
            self.render()
        else:
            self.update_state()

    def step_match(self, delta):
        if self.find_timer.isActive():
            return self.run_search()
        if self.matches:
            self.match_idx = (self.match_idx + delta) % len(self.matches)
            self.show_match()

    def show_match(self):
        pno, rect = self.matches[self.match_idx]
        if pno not in self.offsets:
            self.pno = pno
            self.sync_thumb()
        self.render()
        self.view.centerOn(self.to_scene(rect, pno).center())

    def mark_current_match(self):
        if self.matches:
            snap = self.snapshot()
            pno, rect = self.matches[self.match_idx]
            if self.add_redaction(pno, rect):
                self.push_undo(snap)
            self.render()

    def mark_all_matches(self):
        snap = self.snapshot()
        added = [pno for pno, rect in self.matches if self.add_redaction(pno, rect)]
        if added:
            self.push_undo(snap)
        self.render()
        self.status(f"Redacted {len(added)} match(es) on {len(set(added))} page(s)", 6000)
