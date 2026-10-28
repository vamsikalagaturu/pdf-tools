import os
import shlex
import statistics
import time
from functools import partial
import tempfile
from pathlib import Path

import pymupdf
from PySide6.QtCore import (
    QByteArray,
    QEvent,
    QFile,
    QFileSystemWatcher,
    QPoint,
    QPointF,
    QProcess,
    QRectF,
    QSize,
    Qt,
    QTimer,
    Signal,
)
from PySide6.QtGui import (
    QAction,
    QActionGroup,
    QBrush,
    QColor,
    QCursor,
    QFont,
    QFontDatabase,
    QIcon,
    QImage,
    QKeySequence,
    QPen,
    QPixmap,
    QPolygonF,
    QShortcut,
)
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QButtonGroup,
    QColorDialog,
    QComboBox,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
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
from pdf_tools.core.edit_list import DOCUMENT, PLACED, count_edits, discard, edit_entries
from pdf_tools.core.edits import apply_edits, flatten, open_pdf, protect, remove_annots
from pdf_tools.core.fonts import (
    DEFAULT_FONTS,
    base_font_name,
    draw_text,
    font_family,
    font_style,
    pdf_font_names,
    resolve_font,
    styled_font,
    text_size,
)
from pdf_tools.core.pages import content_rect
from pdf_tools.core.ranges import format_ranges, parse_ranges
from pdf_tools.core.selection import flow_select, reading_order_words
from pdf_tools.core.synctex import SyncTex
from pdf_tools.ui import icons
from pdf_tools.ui.dialogs import ExportDialog, PasswordDialog, SignatureDialog
from pdf_tools.ui.edits_panel import EditsPanel, shorten
from pdf_tools.ui.form_fields import FormFields
from pdf_tools.ui.page_view import ObjItem, PageView, TextEditItem
from pdf_tools.ui.popover import FormatBar, Popover
from pdf_tools.ui.printing import print_pdf
from pdf_tools.ui.util import hex_to_rgb, png_bytes, rgb_to_hex, swatch_icon, to_qimage

THUMB_ZOOM = 0.22
PREVIEW_SCALE = 4
SIGNATURE_WIDTH = 150
NOTE_SIZE = 18
PAGE_GAP = 12
PIXMAP_CACHE = 12
PATH_WIDTH = 360
THUMB_CHECK_WIDTH = 24
CROP_DIM = QColor(32, 33, 36, 150)
KEEP = object()
IMAGE_FILTER = "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff)"
PAGE_BAR_STYLE ="QToolBar { border: none; border-bottom: 1px solid rgba(128, 128, 128, 70); padding: 2px; spacing: 2px; }"
RELOAD_SETTLE_MS = 300
RELOAD_TRIES = 10
SYNC_MARK = QColor(255, 200, 0, 110)
SYNC_MARK_MS = 1500
# text being typed is laid out at this pixel size and scaled to its point size (Qt font sizes are integers)
TEXT_EDIT_PX = 100
SKIP_ANNOTS = {pymupdf.PDF_ANNOT_POPUP, pymupdf.PDF_ANNOT_WIDGET}
MARKUP_ANNOTS = {
    pymupdf.PDF_ANNOT_HIGHLIGHT, pymupdf.PDF_ANNOT_UNDERLINE, pymupdf.PDF_ANNOT_STRIKE_OUT, pymupdf.PDF_ANNOT_SQUIGGLY,
}
TOOLS = (
    ("view", "View", "V", "Fill forms, drag to select text (Alt-drag: rectangle), click notes to read them (V)"),
    ("redact", "Redact", "R", "Drag over text to redact it; Alt-drag or Snap off for a free rectangle (R)"),
    ("highlight", "Highlight", "H", "Drag over text to highlight it, then add an optional comment (H)"),
    ("note", "Note", "N", "Click to add a sticky note; click one to read or edit it (N)"),
    ("text", "Text", "T", "Click to type on the page, or drag first to set a width; double-click a box to edit it (T)"),
    ("sign", "Sign", "S", "Click to place a signature (S)"),
    ("image", "Image", "I", "Click, or drag a box, to place a picture; drag its corner to resize (I)"),
    ("draw", "Draw", "D", "Draw freehand; strokes made in quick succession form one drawing (D)"),
    ("crop", "Crop", "C", "Drag a rectangle to crop to it; drag the frame or its corner to adjust (C)"),
)
FLOW_COLOURS = {"view": QColor(26, 115, 232, 70), "redact": QColor(0, 0, 0, 90)}


ANNOT_LIST_STYLE = """
QTreeWidget::item { padding: 6px 4px; border-bottom: 1px solid rgba(128, 128, 128, 70); }
QTreeWidget::item:has-children { padding: 8px 4px 4px 4px; border-bottom: none; }
"""
# Edits tab: kind -> (label, icon)
EDIT_KINDS = {
    "redact": ("Redaction", "redact"), "text": ("Text", "text"), "sign": ("Signature", "sign"), "note": ("Note", "note"),
    "image": ("Image", "image"), "ink": ("Drawing", "draw"),
    "form": ("Field", "file"), "deleted": ("Removed", "remove"), "comment": ("Comment", "note"),
    "rotate": ("Rotated", "rotate"), "crop": ("Cropped", "crop"), "remove_page": ("Page removed", "remove"),
    "insert": ("Inserted from", "add"), "reorder": ("Pages reordered", "sort"),
}
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


def file_stamp(path):
    try:
        st = os.stat(path)
    except OSError:
        return None
    return st.st_mtime_ns, st.st_size


PEN_COLOURS = (("Black", (0, 0, 0)), *INK_PRESETS)
PEN_WIDTHS = (("Thin", 1.0), ("Medium", 2.0), ("Thick", 4.0))
# a stroke started within this many seconds of the last one joins its drawing
INK_GROUP_S = 1.5


def ink_bounds(obj):
    """Bounding box of a drawing's strokes, padded by half the pen width."""
    box = pymupdf.Rect(obj["strokes"][0][0], obj["strokes"][0][0])
    for stroke in obj["strokes"]:
        for point in stroke:
            box.include_point(point)
    half = obj["width"] / 2 + 1
    return box + (-half, -half, half, half)


def palette_for(style):
    return HIGHLIGHT_PRESETS if style == "highlight" else INK_PRESETS


def colour_key(style):
    return "highlight_colour" if style == "highlight" else "ink_colour"


class EditTab(QWidget):
    """Viewer/editor for one PDF: redactions, text, signatures, highlights, notes, form fields, search."""

    activate_requested = Signal()
    closed = Signal()
    open_dialog_requested = Signal()
    title_changed = Signal(str)
    split_requested = Signal(object)

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
        self.order = ()
        self.rotation = {}
        self.crop = {}
        self.inserted = ()
        self.own = 0
        self.password = None
        self.annot_types = {}
        self.fields = FormFields(self)
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
        self.edits_sig = None
        self.words_cache = {}
        self.thumb_done = set()
        self.flow_items = []
        self.popover = None
        self.text_edit = None
        self.qt_families = {}
        self.pen_rgb, self.pen_width = PEN_COLOURS[0][1], PEN_WIDTHS[1][1]
        self.ink_group = None

        self.thumbs = QListWidget()
        self.thumbs.setIconSize(QSize(130, 184))
        self.thumbs.setSpacing(4)
        self.thumbs.setDragDropMode(QAbstractItemView.InternalMove)
        self.thumbs.setDefaultDropAction(Qt.MoveAction)
        self.thumbs.setSelectionMode(QAbstractItemView.SingleSelection)
        self.thumbs.setContextMenuPolicy(Qt.CustomContextMenu)
        self.thumbs.customContextMenuRequested.connect(self.on_thumb_menu)
        self.thumbs.currentRowChanged.connect(lambda row: row >= 0 and self.goto(self.thumbs.item(row).data(Qt.UserRole)))
        self.thumbs.itemChanged.connect(lambda _: self.update_state())
        self.thumbs.model().rowsMoved.connect(self.on_thumbs_moved)
        self.thumbs.verticalScrollBar().valueChanged.connect(lambda _: self.render_thumbs())

        self.annot_tree = QTreeWidget()
        self.annot_tree.setHeaderHidden(True)
        self.annot_tree.setIndentation(12)
        self.annot_tree.setIconSize(QSize(14, 14))
        self.annot_tree.setStyleSheet(ANNOT_LIST_STYLE)
        self.annot_tree.itemClicked.connect(lambda item, _: self.on_annot_entry(item))
        self.annot_tree.itemActivated.connect(lambda item, _: self.on_annot_entry(item))
        self.edits_tree = EditsPanel()
        self.edits_tree.discard_requested.connect(self.discard)
        self.edits_tree.activated.connect(self.on_edit_entry)
        self.thumbs.viewport().installEventFilter(self)
        self.side = QTabWidget()
        self.side.setDocumentMode(True)
        self.pages_panel = QWidget()
        pages_layout = QVBoxLayout(self.pages_panel)
        pages_layout.setContentsMargins(0, 0, 0, 0)
        pages_layout.setSpacing(0)
        pages_layout.addWidget(self.thumbs)
        self.side.addTab(self.pages_panel, "Pages")
        self.side.addTab(self.annot_tree, "Annotations")
        self.side.addTab(self.edits_tree, "Edits")
        self.side.setMinimumWidth(240)
        self.side.setVisible(settings.get("show_sidebar"))

        self.view = PageView(lambda: self.tool, self.selection_mode)
        self.view.pressed.connect(lambda: self.close_popover())
        self.view.context_requested.connect(self.on_context)
        self.view.band_selected.connect(self.on_band)
        self.view.flow_changed.connect(self.on_flow)
        self.view.clicked.connect(self.on_click)
        self.view.zoom_step.connect(lambda f: self.set_zoom(self.zoom * f))
        self.view.sync_clicked.connect(self.inverse_search)
        self.view.stroke_drawn.connect(self.on_stroke)
        self.sync = None
        self.sync_marks = []
        self.watcher = QFileSystemWatcher(self)
        self.watcher.fileChanged.connect(lambda _: self.reload_timer.start())
        self.reload_timer = QTimer(self)
        self.reload_timer.setSingleShot(True)
        self.reload_timer.setInterval(RELOAD_SETTLE_MS)
        self.reload_timer.timeout.connect(self.reload_if_changed)
        self.disk_stamp = self.seen_stamp = None
        self.reload_tries = 0
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

        self.info = self.build_info()
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

    def build_info(self):
        """Status line: page and edit counts, then the file path (elided to a fixed width) with a copy button."""
        box = QWidget()
        lay = QHBoxLayout(box)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(4)
        self.stats = QLabel()
        self.path_label = QLabel()
        self.path_label.setFixedWidth(PATH_WIDTH)
        self.path_label.setContextMenuPolicy(Qt.ActionsContextMenu)
        copy_action = QAction("Copy path", self.path_label)
        copy_action.triggered.connect(self.copy_path)
        self.path_label.addAction(copy_action)
        copy = QToolButton()
        copy.setAutoRaise(True)
        copy.setIconSize(QSize(14, 14))
        copy.setToolTip("Copy path")
        icons.bind(copy, "copy")
        copy.clicked.connect(self.copy_path)
        for w in (self.stats, self.path_label, copy):
            lay.addWidget(w)
        return box

    def copy_path(self):
        if self.doc_path:
            QApplication.clipboard().setText(str(self.doc_path))
            self.status("Path copied")

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
        self.a_print = self.action("Print...", self.print_doc, QKeySequence.Print)
        self.a_print_preview = self.action("Print preview...", lambda: self.print_doc(preview=True))
        self.a_split = self.action("Split into files...", self.split_doc)
        self.a_protect = self.action("Protect with password...", self.protect_copy)
        self.a_unprotect = self.action("Remove password...", self.unprotect_copy)
        self.a_rotate_left = self.action("Rotate left", lambda: self.rotate_pages(self.target_pages(), -90), "Ctrl+Shift+L")
        self.a_rotate_right = self.action("Rotate right", lambda: self.rotate_pages(self.target_pages(), 90), "Ctrl+Shift+R")
        self.a_remove_pages = self.action("Remove page(s)", lambda: self.remove_pages(self.target_pages()))
        self.a_insert_pages = self.action("Insert pages from PDF...", lambda: self.insert_pages(after=self.pno))
        self.a_fit_content = self.action("Crop to content", lambda: self.crop_to_content(self.target_pages()))
        self.a_uncrop = self.action("Remove crop", lambda: self.set_crops({p: None for p in self.target_pages()}))
        self.a_sidebar = self.action("Show sidebar", self.toggle_sidebar, "F9", checkable=True)
        self.a_sidebar.setChecked(s.get("show_sidebar"))
        self.a_sidebar.setToolTip("Show or hide the sidebar (F9)")
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
        self.a_prev = self.action("Previous page", lambda: self.step_page(-1), ["Left", "PgUp"])
        self.a_next = self.action("Next page", lambda: self.step_page(1), ["Right", "PgDown"])
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
        self.a_highlight_colour = self.action("Custom mark colour...", self.pick_highlight_colour)
        self.a_redact_colour = self.action("Redaction colour...", self.pick_colour)
        self.a_copy_path = self.action("Copy file path", self.copy_path)
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
        w.setStyleSheet("QToolButton { padding: 3px 3px; }")
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
            (self.a_print, "print"), (self.a_print_preview, "print"), (self.a_sidebar, "sidebar"),
            (self.a_split, "split"), (self.a_protect, "lock"), (self.a_unprotect, "unlock"),
            (self.a_rotate_left, "rotate-left"), (self.a_rotate_right, "rotate"), (self.a_remove_pages, "remove"),
            (self.a_insert_pages, "add"), (self.a_fit_content, "crop"), (self.a_uncrop, "close"),
            *((self.tool_actions[key], key) for key in self.tool_actions),
        ):
            icons.bind(a, name)
        tb = self.toolbar = QToolBar()
        tb.installEventFilter(self)
        tb.setIconSize(QSize(18, 18))
        tb.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        tb.addActions([self.a_sidebar, self.a_undo, self.a_redo])
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
        fit_content, uncrop = self.tool_button(self.a_fit_content), self.tool_button(self.a_uncrop)
        for b in (fit_content, uncrop):
            b.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.crop_opts = tb.addWidget(self.option_group(fit_content, uncrop))
        self.draw_opts = tb.addWidget(self.build_pen_options())

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
        menu.addSeparator()
        menu.addActions([self.a_protect, self.a_unprotect])
        menu.addSeparator()
        menu.addActions([self.a_print, self.a_print_preview])
        save.setMenu(menu)
        tb.addWidget(save)
        for a in (
            self.a_sidebar, self.a_undo, self.a_redo, self.a_zoom_out, self.a_zoom_in, self.a_fit, self.a_continuous, self.a_find,
        ):
            tb.widgetForAction(a).setToolButtonStyle(Qt.ToolButtonIconOnly)
        self.pages_panel.layout().insertWidget(0, self.build_page_bar())
        self.on_tool()

    def build_page_bar(self):
        """Page commands above the thumbnails; they act on the ticked pages, else the current one."""
        bar = QToolBar()
        bar.setIconSize(QSize(16, 16))
        bar.setToolButtonStyle(Qt.ToolButtonIconOnly)
        bar.setStyleSheet(PAGE_BAR_STYLE)
        targets = " (ticked pages, or the current page)"
        for a, tip in (
            (self.a_rotate_left, "Rotate left, Ctrl+Shift+L" + targets),
            (self.a_rotate_right, "Rotate right, Ctrl+Shift+R" + targets),
            (None, None),
            (self.a_insert_pages, "Insert pages from a PDF after the current page"),
            (self.a_remove_pages, "Remove, Delete" + targets),
            (None, None),
            (self.a_fit_content, "Crop to content" + targets),
        ):
            if a is None:
                bar.addSeparator()
            else:
                a.setToolTip(tip)
                bar.addAction(a)
        return bar

    def build_pen_options(self):
        """Draw tool options: pen colour swatches and a width choice."""
        group = QButtonGroup(self)
        buttons = []
        for name, rgb in PEN_COLOURS:
            b = self.tool_button(text=" ")
            b.setToolButtonStyle(Qt.ToolButtonIconOnly)
            b.setIcon(swatch_icon(rgb))
            b.setToolTip(name)
            b.setCheckable(True)
            b.setChecked(rgb == self.pen_rgb)
            b.clicked.connect(lambda _=False, rgb=rgb: self.set_pen(rgb=rgb))
            group.addButton(b)
            buttons.append(b)
        self.pen_width_box = QComboBox()
        for name, width in PEN_WIDTHS:
            self.pen_width_box.addItem(name, width)
        self.pen_width_box.setCurrentIndex(1)
        self.pen_width_box.setToolTip("Pen width")
        self.pen_width_box.currentIndexChanged.connect(lambda i: self.set_pen(width=self.pen_width_box.itemData(i)))
        self.set_pen()
        return self.option_group(*buttons, self.pen_width_box)

    def set_pen(self, rgb=None, width=None):
        self.pen_rgb = rgb or self.pen_rgb
        self.pen_width = width or self.pen_width
        self.view.pen = QPen(QColor(*self.pen_rgb), self.pen_width, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin)
        self.ink_group = None

    def on_stroke(self, scene_points):
        """A finished pen stroke: joins the drawing made just before on the same page, else starts one."""
        pno = self.page_at(scene_points[0]) if self.doc else None
        if pno is None:
            return
        points = [self.to_pdf_point(p, pno) for p in scene_points]
        now = time.monotonic()
        snap = self.snapshot()
        group = self.ink_group
        if group and group[0] == pno and now - group[2] < INK_GROUP_S and any(o is group[1] for o in self.items.get(pno, [])):
            obj = group[1]
            obj["strokes"].append(points)
        else:
            obj = {"kind": "ink", "strokes": [points], "rgb": self.pen_rgb, "width": self.pen_width}
            self.items.setdefault(pno, []).append(obj)
        obj["rect"] = ink_bounds(obj)
        self.ink_group = (pno, obj, now)
        self.push_undo(snap)
        self.render()

    def set_ink(self, pno, obj, **values):
        self.push_undo(self.snapshot())
        obj.update(values)
        obj["rect"] = ink_bounds(obj)
        self.render()

    def build_mark_options(self):
        """Highlight tool options: a style menu button, preset colour swatches and a custom colour."""
        self.style_button = QToolButton()
        self.style_button.setPopupMode(QToolButton.InstantPopup)
        self.style_button.setToolButtonStyle(Qt.ToolButtonIconOnly)
        menu = self.style_menu = QMenu("Mark style", self)
        styles = QActionGroup(self)
        for key, label in MARK_STYLES.items():
            a = menu.addAction(label, lambda key=key: self.set_mark_style(key))
            a.setCheckable(True)
            styles.addAction(a)
            icons.bind(a, STYLE_ICONS[key])
        self.style_button.setMenu(menu)
        self.colour_menu = QMenu("Mark colour", self)
        self.colour_menu.aboutToShow.connect(self.fill_colour_menu)
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

    def fill_colour_menu(self):
        """The current mark style's presets, as in the toolbar, plus a custom colour."""
        menu = self.colour_menu
        menu.clear()
        current = self.mark_colour()
        for name, rgb in palette_for(self.mark_style()):
            a = menu.addAction(swatch_icon(rgb), name, lambda rgb=rgb: self.set_mark_colour(rgb))
            a.setCheckable(True)
            a.setChecked(rgb == current)
        menu.addSeparator()
        menu.addAction(self.a_highlight_colour)

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
        self.crop_opts.setVisible(tool == "crop")
        self.draw_opts.setVisible(tool == "draw")
        self.opts_sep.setVisible(tool in ("redact", "highlight", "crop", "draw"))
        self.ink_group = None
        self.view.update_cursor()
        self.close_popover()
        self.clear_flow()
        self.fit_toolbar()
        if self.doc:
            # crop frames are only interactive (and in the way) while the Crop tool is active
            self.render()

    def fit_toolbar(self):
        """Tool buttons drop their labels when the toolbar is too narrow to show them."""
        tb = self.toolbar
        buttons = [tb.widgetForAction(a) for a in self.tool_actions.values()]
        for b in buttons:
            b.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        if tb.sizeHint().width() > tb.width():
            for b in buttons:
                b.setToolButtonStyle(Qt.ToolButtonIconOnly)

    @property
    def tool(self):
        if not hasattr(self, "tool_actions"):
            return "view"
        return next((k for k, a in self.tool_actions.items() if a.isChecked()), "view")

    @property
    def continuous(self):
        return self.a_continuous.isChecked()

    def load(self, path, password=None):
        """Open `path` here; False when it is password protected and the password prompt was cancelled."""
        self.activate_requested.emit()
        self.unload()
        path = Path(path)
        doc = pymupdf.open(path)
        locked, tried = doc.needs_pass, password is not None
        while locked and not doc.authenticate(password or ""):
            prompt = "Wrong password, try again:" if tried else f"{path.name} is password protected.\n\nPassword:"
            password, ok = QInputDialog.getText(self, APP_NAME, prompt, QLineEdit.Password)
            if not ok:
                doc.close()
                return False
            tried = True
        self.doc_path, self.doc = path, doc
        self.password = password if locked else None
        self.disk_stamp = self.seen_stamp = file_stamp(path)
        self.sync = None
        if self.watcher.files():
            self.watcher.removePaths(self.watcher.files())
        self.watcher.addPath(str(path))
        self.settings.set("last_folder", str(path.parent))
        self.settings.add_recent(path.resolve())
        self.own = len(doc)
        self.order = tuple(range(self.own))
        self.pno = 0
        self.rebuild_thumbs()
        self.tool_actions["view"].setChecked(True)
        self.on_tool()
        self.fit_width()
        self.scroll_home()
        # scroll ranges can update after this event; repeat once the view has settled
        QTimer.singleShot(0, self.scroll_home)
        if self.findbar.isVisible():
            self.run_search()
        return True

    def reload_if_changed(self):
        """Reload after the file changed on disk and has stopped changing (TeX writes the PDF in steps)."""
        if not self.doc:
            return
        stamp = file_stamp(self.doc_path)
        if str(self.doc_path) not in self.watcher.files() and stamp:
            # replaced by a new file (rename over it): watch the new one
            self.watcher.addPath(str(self.doc_path))
        if stamp is None or stamp != self.seen_stamp:
            self.seen_stamp = stamp
            self.reload_timer.start()
            return
        if stamp == self.disk_stamp:
            return
        n = self.edit_count()
        if n and QMessageBox.question(
            self, APP_NAME, f"{self.doc_path.name} changed on disk.\n\nReload it and discard {n} unsaved edit(s)?"
        ) != QMessageBox.Yes:
            self.disk_stamp = stamp
            return
        position = self.order.index(self.pno) if self.pno in self.order else 0
        zoom, scroll = self.zoom, self.view.verticalScrollBar().value()
        try:
            loaded = self.load(self.doc_path, self.password)
        except Exception:
            # still being written: try again shortly, a few times
            self.reload_tries += 1
            if self.reload_tries < RELOAD_TRIES:
                self.reload_timer.start()
            return
        self.reload_tries = 0
        if not loaded:
            return
        self.zoom = zoom
        self.pno = self.order[min(position, len(self.order) - 1)]
        self.sync_thumb()
        self.render()
        # after load()'s own deferred scroll to the top
        QTimer.singleShot(0, lambda: self.view.verticalScrollBar().setValue(scroll))
        self.status(f"Reloaded {self.doc_path.name} (changed on disk)")

    def synctex(self):
        """This PDF's SyncTeX data, re-read when the .synctex file changed; None if there is none."""
        if self.sync is None or self.sync.stale():
            self.sync = SyncTex(self.doc_path)
        return self.sync if self.sync.path else None

    def inverse_search(self, scene_pos):
        """Ctrl+click: open the TeX source line typeset at this point."""
        pno = self.page_at(scene_pos) if self.doc else None
        if pno is None or pno >= self.own:
            return
        sync = self.synctex()
        if not sync:
            self.status("No SyncTeX data for this PDF (compile with -synctex=1)")
            return
        point = self.to_pdf_point(scene_pos, pno)
        found = sync.inverse(pno, point.x, point.y)
        if found:
            self.open_source(*found)

    def open_source(self, path, line):
        command = self.settings.get("editor_command")
        if not command:
            command, ok = QInputDialog.getText(
                self, "Open TeX source with",
                "Command to open a file at a line (%f = file, %l = line), for example\n"
                "  code -g %f:%l\n  nvim --server /tmp/nvim.sock --remote-send ':e %f | %l<CR>'",
                text="code -g %f:%l",
            )
            if not ok or not command.strip():
                return
            self.settings.set("editor_command", command.strip())
        args = [a.replace("%f", path).replace("%l", str(line)) for a in shlex.split(command)]
        if QProcess.startDetached(args[0], args[1:]):
            self.status(f"Opened {Path(path).name}:{line}")
        else:
            QMessageBox.warning(self, APP_NAME, f"Could not run {args[0]}.\n\nChange editor_command in {self.settings.path}.")

    def forward_search(self, source, line):
        """Show where `source` line `line` is typeset, flashing its lines."""
        sync = self.synctex() if self.doc else None
        hits = sync.forward(source, line) if sync else []
        if not hits or hits[0][0] not in self.order:
            self.status(f"No typeset text for {Path(source).name}:{line}")
            return
        pno = hits[0][0]
        if not (self.continuous and pno in self.offsets):
            self.goto(pno)
        scene = self.view.scene()
        marks = []
        for _, rect in hits:
            mark = scene.addRect(self.to_scene(rect, pno), Qt.NoPen, QBrush(SYNC_MARK))
            mark.setZValue(3)
            mark.setAcceptedMouseButtons(Qt.NoButton)
            marks.append(mark)
        self.view.centerOn(marks[0])
        self.sync_marks = marks
        QTimer.singleShot(SYNC_MARK_MS, lambda: self.clear_sync_marks(marks))

    def clear_sync_marks(self, marks):
        # render() replaces the list after deleting the scene's items, so only still-current marks are removed
        if self.sync_marks is marks:
            for mark in marks:
                self.view.scene().removeItem(mark)
            self.sync_marks = []

    def rebuild_thumbs(self):
        """One row per page in display order; images are drawn as rows scroll into view (render_thumbs)."""
        ticked = {
            self.thumbs.item(i).data(Qt.UserRole) for i in range(self.thumbs.count())
            if self.thumbs.item(i).checkState() == Qt.Checked
        }
        self.thumbs.blockSignals(True)
        self.thumbs.clear()
        for p in self.order:
            w, h = self.doc[p].rect.width * THUMB_ZOOM, self.doc[p].rect.height * THUMB_ZOOM
            blank = QPixmap(max(1, round(w)), max(1, round(h)))
            blank.fill(QColor(235, 235, 235))
            item = QListWidgetItem(QIcon(blank), "")
            item.setData(Qt.UserRole, p)
            item.setFlags((item.flags() | Qt.ItemIsUserCheckable | Qt.ItemIsDragEnabled) & ~Qt.ItemIsDropEnabled)
            item.setCheckState(Qt.Checked if p in ticked else Qt.Unchecked)
            self.thumbs.addItem(item)
        if self.pno in self.order:
            self.thumbs.setCurrentRow(self.order.index(self.pno))
        self.thumbs.blockSignals(False)
        self.thumb_done = set()
        QTimer.singleShot(0, self.render_thumbs)

    def render_thumbs(self):
        """Draw the thumbnails of rows in (or near) the visible part of the Pages list."""
        if not self.doc:
            return
        view = self.thumbs.viewport().rect()
        near = view.adjusted(0, -view.height(), 0, view.height())
        for row in range(self.thumbs.count()):
            rect = self.thumbs.visualItemRect(self.thumbs.item(row))
            if rect.top() > near.bottom():
                break
            item = self.thumbs.item(row)
            p = item.data(Qt.UserRole)
            if rect.bottom() < near.top() or p in self.thumb_done:
                continue
            pix = self.doc[p].get_pixmap(matrix=pymupdf.Matrix(THUMB_ZOOM, THUMB_ZOOM), alpha=False)
            self.thumbs.blockSignals(True)
            item.setIcon(QIcon(QPixmap.fromImage(to_qimage(pix))))
            self.thumbs.blockSignals(False)
            self.thumb_done.add(p)

    def scroll_home(self):
        self.view.verticalScrollBar().setValue(self.view.verticalScrollBar().minimum())
        self.view.horizontalScrollBar().setValue(self.view.horizontalScrollBar().minimum())

    def unload(self):
        self.close_popover(commit=False)
        if self.text_edit:
            self.text_edit[3].deleteLater()
            self.text_edit = None
        self.flow_items = []
        self.reload_timer.stop()
        if self.watcher.files():
            self.watcher.removePaths(self.watcher.files())
        self.fields.detach()
        # clear the scene before closing the document: closing first crashes PySide (seen with form fields)
        self.view.scene().clear()
        self.sync_marks = []
        if self.doc:
            self.doc.close()
        self.doc = None
        self.items, self.deleted, self.history, self.future = {}, set(), [], []
        self.annot_text, self.words_cache, self.annot_types = {}, {}, {}
        self.order, self.rotation, self.crop, self.inserted, self.own, self.password = (), {}, {}, (), 0, None
        self.annot_tree.clear()
        self.side.setTabText(1, "Annotations")
        self.edits_sig = None
        self.edits_tree.clear()
        self.side.setTabText(2, "Edits")
        self.forms = {}
        self.fields.reset()
        self.matches, self.match_idx = [], -1
        self.text_sel = None
        self.offsets, self.page_items, self.pix_cache = {}, {}, {}
        self.font_cache.clear()
        self.preview_cache.clear()
        self.thumbs.blockSignals(True)
        self.thumbs.clear()
        self.thumbs.blockSignals(False)

    def reload_display(self):
        """Rebuild the display copy the way saving builds the output: inserted pages, removed annotations, rotation."""
        self.finish_text(rerender=False)
        self.fields.detach()
        self.view.scene().clear()
        self.page_items, self.offsets, self.sync_marks = {}, {}, []
        self.fields.clear_scene_refs()
        self.doc.close()
        self.doc = open_pdf(self.doc_path, self.password, self.inserted)
        remove_annots(self.doc, self.deleted)
        for p, degrees in self.rotation.items():
            self.doc[p].set_rotation((self.doc[p].rotation + degrees) % 360)
        self.pix_cache, self.words_cache, self.thumb_done = {}, {}, set()
        self.render_thumbs()

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
        """The output PDF bytes and redaction leaks; `pages` (page numbers, in order) defaults to the page order."""
        return apply_edits(
            self.doc_path, self.items, list(self.order) if pages is None else pages, self.forms, self.fields.shrunk(),
            self.deleted, self.annot_text, rotation=self.rotation, crop=self.crop, inserted=self.inserted,
            password=self.password,
        )

    def scrubs(self, pages=None):
        """Whether writing `pages` removes hidden data (it does when they carry redactions or crops)."""
        pages = self.order if pages is None else pages
        return any(p in self.crop or any(o["kind"] == "redact" for o in self.items.get(p, [])) for p in pages)

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
        if self.password:
            data = protect(data, self.password)
        fd, tmp = tempfile.mkstemp(dir=self.doc_path.parent, prefix=f".{self.doc_path.name}.", suffix=".tmp")
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
        path, password, scrubbed = self.doc_path, self.password, self.scrubs()
        if not QFile.moveToTrash(str(path)):
            os.unlink(tmp)
            QMessageBox.critical(self, APP_NAME, "Could not move the original to the trash; nothing changed.")
            return
        os.replace(tmp, path)
        self.items, self.forms, self.deleted, self.annot_text = {}, {}, set(), {}
        self.report(path, leaks, scrubbed)
        self.load(path, password)

    def save_as(self):
        if self.edit_count():
            self.write_copy(None, f"{self.doc_path.stem}-edited.pdf")

    def ticked_positions(self):
        return [i for i in range(self.thumbs.count()) if self.thumbs.item(i).checkState() == Qt.Checked]

    def export_pages(self):
        if not self.doc:
            return
        s = self.settings
        n = len(self.order)
        ranges = format_ranges(self.ticked_positions()) or f"1-{n}"
        dlg = ExportDialog(self, n, ranges, s.get("export_flatten"), s.get("export_dpi"))
        if dlg.exec() != QDialog.Accepted or not dlg.pages.text().strip():
            return
        text = dlg.pages.text()
        s.set("export_flatten", dlg.flat.isChecked())
        s.set("export_dpi", dlg.dpi.value())
        try:
            positions = parse_ranges(text, n)
        except ValueError:
            QMessageBox.critical(self, APP_NAME, f"Invalid page range: {text}")
            return
        if positions:
            dpi = dlg.dpi.value() if dlg.flat.isChecked() else None
            name = f"{self.doc_path.stem}-p{format_ranges(positions).replace(',', '_')}{'-flat' if dpi else ''}.pdf"
            self.write_copy([self.order[i] for i in positions], name, dpi)

    def write_copy(self, pages, default_name, flatten_dpi=None, password=KEEP):
        """Save the edited `pages` (default: all, in order) to a chosen file; `password` None writes it unprotected."""
        out, _ = QFileDialog.getSaveFileName(self, "Save copy", str(self.doc_path.parent / default_name), "PDF (*.pdf)")
        if not out:
            return
        if Path(out).resolve() == self.doc_path.resolve():
            QMessageBox.critical(self, APP_NAME, "That is the open file; use File > Save to overwrite it.")
            return
        data, leaks = self.output(pages, flatten_dpi, self.password if password is KEEP else password)
        Path(out).write_bytes(data)
        self.report(out, leaks, self.scrubs(pages) or bool(flatten_dpi))

    def output(self, pages, flatten_dpi=None, password=None):
        data, leaks = self.edited(pages)
        if flatten_dpi:
            data = flatten(data, flatten_dpi)
            check = pymupdf.open("pdf", data)
            leaks += [(i + 1, "selectable text after flatten") for i, p in enumerate(check) if p.get_text().strip()]
            check.close()
        return (protect(data, password) if password else data), leaks

    def report(self, out, leaks, scrubbed=False):
        if leaks:
            QMessageBox.warning(self, APP_NAME, f"Saved {out}\n\nText remains under redactions:\n{leaks[:10]}")
            return
        hidden = " Removed hidden data (metadata, attachments, scripts, hidden text)." if scrubbed else ""
        self.status(f"Saved {out} - no text left under any redaction.{hidden}", 10000)

    def split_doc(self):
        if self.doc:
            self.split_requested.emit(self)

    def protect_copy(self):
        if not self.doc:
            return
        dlg = PasswordDialog(self)
        if dlg.exec() == QDialog.Accepted:
            self.write_copy(None, f"{self.doc_path.stem}-protected.pdf", password=dlg.password())

    def unprotect_copy(self):
        if not self.password:
            QMessageBox.information(self, APP_NAME, f"{self.doc_path.name} has no password.")
            return
        self.write_copy(None, f"{self.doc_path.stem}-unlocked.pdf", password=None)

    def print_doc(self, preview=False):
        if self.doc:
            position = self.order.index(self.pno) if self.pno in self.order else 0
            print_pdf(self, lambda: self.edited()[0], len(self.order), position, self.doc_path.name, preview)

    def state(self):
        """The current edits (not copied; snapshot() copies)."""
        return {
            "items": self.items, "deleted": frozenset(self.deleted), "forms": self.forms, "annot_text": self.annot_text,
            "order": self.order, "rotation": self.rotation, "crop": self.crop, "inserted": self.inserted,
        }

    def edit_count(self):
        return count_edits(self.state(), self.own, len(self.doc)) if self.doc else 0

    def page_edits(self, pno):
        keyed = list(self.forms) + list(self.deleted) + list(set(self.annot_text) - self.deleted)
        own = len(self.items.get(pno, [])) + (pno in self.rotation) + (pno in self.crop)
        return own + sum(p == pno for p, _ in keyed)

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

    def eventFilter(self, obj, event):
        if obj is self.thumbs.viewport():
            if event.type() == QEvent.MouseButtonRelease and event.button() == Qt.LeftButton:
                self.on_thumb_clicked(event.position().toPoint())
            elif event.type() == QEvent.Resize:
                QTimer.singleShot(0, self.render_thumbs)
        elif obj is self.toolbar and event.type() == QEvent.Resize:
            self.fit_toolbar()
        return False

    def font_for(self, name, text):
        key = (name, text)
        if key not in self.font_cache:
            self.font_cache[key] = resolve_font(self.doc, name, text)
        return self.font_cache[key]

    def text_image(self, obj):
        key = (obj["text"], obj["font"], obj["size"], obj["rgb"], obj.get("width"))
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
        elif obj["kind"] == "image":
            image = QImage.fromData(obj["data"])
        elif obj["kind"] == "ink":
            origin = rect.topLeft()
            parts = [
                QPolygonF([self.to_scene(pymupdf.Rect(pt, pt), pno).topLeft() - origin for pt in stroke])
                for stroke in obj["strokes"]
            ]
        elif obj["kind"] == "highlight":
            parts = [self.to_scene(r, pno).translated(-rect.topLeft()) for r in obj["rects"]]
        return ObjItem(self, obj, pno, rect, image, parts)

    def scene_pages(self):
        return list(self.order) if self.continuous else [self.pno]

    def render(self, scroll_to=None):
        """Rebuild the scene: page frames, edits, existing annotations, overlays; pixmaps come from the cache."""
        if not self.doc:
            return
        self.close_popover()
        self.finish_text(rerender=False)
        scene = self.view.scene()
        keep = [i.obj for i in scene.selectedItems() if isinstance(i, ObjItem)]
        hpos, vpos = self.view.horizontalScrollBar().value(), self.view.verticalScrollBar().value()
        self.view.verticalScrollBar().blockSignals(True)
        self.flow_items = []
        self.fields.detach()
        scene.clear()
        self.fields.clear_scene_refs()
        self.page_items, self.offsets, self.sync_marks = {}, {}, []
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
            if p in self.crop:
                self.add_crop_items(scene, p)
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

    def add_crop_items(self, scene, p):
        """Dim the page outside its crop; with the Crop tool active, add a frame to move or resize the crop."""
        page, kept = self.page_rect(p), self.to_scene(self.crop[p], p)
        for strip in (
            QRectF(page.left(), page.top(), page.width(), kept.top() - page.top()),
            QRectF(page.left(), kept.bottom(), page.width(), page.bottom() - kept.bottom()),
            QRectF(page.left(), kept.top(), kept.left() - page.left(), kept.height()),
            QRectF(kept.right(), kept.top(), page.right() - kept.right(), kept.height()),
        ):
            if strip.width() > 0 and strip.height() > 0:
                dim = scene.addRect(strip, Qt.NoPen, QBrush(CROP_DIM))
                dim.setZValue(1.8)
                dim.setAcceptedMouseButtons(Qt.NoButton)
        if self.tool == "crop":
            frame = ObjItem(self, {"kind": "crop", "rect": self.crop[p]}, p, kept)
            frame.setZValue(2.5)
            scene.addItem(frame)

    def annot_comment(self, pno, annot):
        return self.annot_text.get((pno, annot.xref), annot.info.get("content", ""))

    def refresh_annotations(self):
        """List notes and highlights (ours and the PDF's) per page in the side panel."""
        tree = self.annot_tree
        tree.clear()
        count = 0
        bold = tree.font()
        bold.setBold(True)
        for position, p in enumerate(self.order):
            entries = []
            for obj in self.items.get(p, []):
                if obj["kind"] in ("highlight", "note", "ink"):
                    rgb = obj.get("rgb", NOTE_RGB)
                    label = {"highlight": MARK_STYLES[obj.get("style", "highlight")], "note": "Note", "ink": "Drawing"}[obj["kind"]]
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
            top = QTreeWidgetItem([f"Page {position + 1}  ({len(entries)})"])
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

    def refresh_edits(self):
        """Rebuild the Edits tab when the set of edits changed."""
        sig = (
            tuple((p, id(o), o.get("text"), o.get("style")) for p, objs in sorted(self.items.items()) for o in objs),
            tuple(sorted(self.forms.items(), key=lambda kv: kv[0])), frozenset(self.deleted),
            tuple(sorted(self.annot_text.items())), self.order, tuple(sorted(self.rotation.items())),
            tuple(sorted((p, tuple(r)) for p, r in self.crop.items())),
        )
        if sig == self.edits_sig:
            return
        self.edits_sig = sig
        entries = edit_entries(self.doc, self.state(), self.own, self.fields.names, self.annot_types)

        def position(page):
            if page is DOCUMENT:
                return -1
            return self.order.index(page) if page in self.order else len(self.order) + page

        groups = []
        for page in sorted(entries, key=position):
            if page is DOCUMENT:
                title = "Document"
            elif page in self.order:
                title = f"Page {self.order.index(page) + 1}"
            else:
                title = f"Removed page (was {page + 1})" if page < self.own else "Removed inserted page"
            rows = [(self.edit_label(kind, name), detail, self.edit_icon(kind, name), ref) for kind, name, detail, ref in entries[page]]
            groups.append((title, ("page", page), rows))
        self.edits_tree.set_groups(groups)
        total = sum(len(rows) for *_, rows in groups)
        self.side.setTabText(2, f"Edits ({total})" if total else "Edits")

    @staticmethod
    def edit_label(kind, name):
        if kind == "highlight":
            return MARK_STYLES.get(name or "highlight", "Highlight")
        label = EDIT_KINDS[kind][0]
        return f"{label} {name}" if name and kind in ("form", "deleted") else label

    @staticmethod
    def edit_icon(kind, name):
        return STYLE_ICONS.get(name or "highlight", "highlight") if kind == "highlight" else EDIT_KINDS[kind][1]

    def discard(self, refs):
        """Drop the given edits (refs from the Edits tab) as one undo step."""
        if self.doc:
            changed = discard(self.snapshot(), refs, self.own)
            # a second copy for the history: `changed` shares its edit dicts with the first
            self.push_undo(self.snapshot())
            self.restore(changed)

    def on_edit_entry(self, ref):
        if not ref:
            return
        kind, p, key = ref
        if p is DOCUMENT or p not in self.order:
            return
        self.goto(p)
        target = None
        if kind in PLACED and key < len(self.items.get(p, [])):
            target = self.scene_item(p, ("obj", self.items[p][key]))
        elif kind == "comment":
            target = self.scene_item(p, ("annot", key[1]))
        elif kind == "form" and key in self.fields.editors:
            self.view.centerOn(self.fields.editors[key].graphicsProxyWidget())
        if target:
            self.view.scene().clearSelection()
            target.setSelected(True)
            self.view.centerOn(target.sceneBoundingRect().center())

    def on_thumb_clicked(self, pos):
        """A click on a page's edit count (right of the checkbox and thumbnail) shows that page in the Edits tab."""
        item = self.thumbs.itemAt(pos)
        if item is None:
            return
        p = item.data(Qt.UserRole)
        caption = self.thumbs.visualItemRect(item).left() + self.thumbs.iconSize().width() + THUMB_CHECK_WIDTH
        if not self.page_edits(p) or pos.x() < caption:
            return
        self.side.setCurrentWidget(self.edits_tree)
        self.edits_tree.scroll_to_group(("page", p))

    def toggle_sidebar(self):
        self.settings.set("show_sidebar", self.a_sidebar.isChecked())
        self.side.setVisible(self.a_sidebar.isChecked())

    def target_pages(self, clicked=None):
        """Pages a page command acts on: the ticked ones (when `clicked` is one of them or not given), else `clicked`."""
        ticked = [self.order[i] for i in self.ticked_positions()]
        if ticked and (clicked is None or clicked in ticked):
            return ticked
        return [self.pno if clicked is None else clicked]

    def change_pages(self, apply, reload=False):
        """Run `apply()` (which changes order / rotation / crop / inserted) as one undo step and refresh the view."""
        if not self.doc:
            return
        snap = self.snapshot()
        if apply() is False:
            return
        self.push_undo(snap)
        if reload:
            self.reload_display()
        if self.pno not in self.order:
            self.pno = self.order[0]
        self.rebuild_thumbs()
        self.render(scroll_to=self.pno if self.continuous else None)

    def rotate_pages(self, pages, degrees):
        def apply():
            for p in pages:
                turned = (self.rotation.get(p, 0) + degrees) % 360
                if turned:
                    self.rotation[p] = turned
                else:
                    self.rotation.pop(p, None)
        self.change_pages(apply, reload=True)

    def remove_pages(self, pages):
        remaining = tuple(p for p in self.order if p not in pages)
        if not remaining:
            QMessageBox.information(self, APP_NAME, "A document needs at least one page.")
            return
        if self.pno in pages:
            position = self.order.index(self.pno)
            self.pno = next((p for p in self.order[position:] if p not in pages), remaining[-1])

        def apply():
            self.order = remaining
        self.change_pages(apply)

    def insert_pages(self, after):
        path, _ = QFileDialog.getOpenFileName(self, "Insert pages from", str(self.doc_path.parent), "PDF (*.pdf)")
        if not path:
            return
        try:
            src = pymupdf.open(path)
            count, locked = len(src), src.needs_pass
            src.close()
        except Exception as e:
            QMessageBox.critical(self, APP_NAME, f"Cannot open {Path(path).name}:\n{e}")
            return
        if locked:
            QMessageBox.information(self, APP_NAME, f"{Path(path).name} is password protected; remove its password first.")
            return
        start = len(self.doc)
        new = tuple(range(start, start + count))
        position = self.order.index(after) + 1 if after in self.order else len(self.order)

        def apply():
            self.inserted = (*self.inserted, (path, count))
            self.order = self.order[:position] + new + self.order[position:]
        self.change_pages(apply, reload=True)
        self.goto(new[0])

    def set_crops(self, crops):
        """crops: {page: Rect, or None to remove its crop}."""
        def apply():
            if all(self.crop.get(p) == r for p, r in crops.items()):
                return False
            for p, rect in crops.items():
                if rect is None:
                    self.crop.pop(p, None)
                else:
                    self.crop[p] = pymupdf.Rect(rect)
        self.change_pages(apply)

    def crop_to_content(self, pages):
        self.set_crops({p: content_rect(self.doc[p]) for p in pages})

    def on_thumbs_moved(self, *_):
        order = tuple(self.thumbs.item(i).data(Qt.UserRole) for i in range(self.thumbs.count()))
        if order != self.order and len(order) == len(self.order):
            # deferred: rebuilding the list inside its own drop handler crashes Qt
            QTimer.singleShot(0, lambda: self.change_pages(lambda: setattr(self, "order", order)))

    def on_thumb_menu(self, pos):
        item = self.thumbs.itemAt(pos)
        if item is None or not self.doc:
            return
        menu = QMenu(self)
        self.add_page_actions(menu, self.target_pages(item.data(Qt.UserRole)))
        menu.exec(self.thumbs.viewport().mapToGlobal(pos))

    def add_page_actions(self, menu, pages):
        """Rotate / crop / insert / remove entries for `pages` (thumbnail and page right-click menus)."""
        label = f"{len(pages)} pages" if len(pages) > 1 else "page"
        for text, act, icon in (
            (f"Rotate {label} left", lambda: self.rotate_pages(pages, -90), "rotate-left"),
            (f"Rotate {label} right", lambda: self.rotate_pages(pages, 90), "rotate"),
            (f"Crop {label} to content", lambda: self.crop_to_content(pages), "crop"),
            ("Crop to an area (Crop tool)", lambda: self.tool_actions["crop"].trigger(), "crop"),
        ):
            icons.bind(menu.addAction(text, act), icon)
        if any(p in self.crop for p in pages):
            menu.addAction(f"Remove crop from {label}", lambda: self.set_crops({p: None for p in pages}))
        menu.addSeparator()
        icons.bind(menu.addAction("Insert pages from PDF after this...", lambda: self.insert_pages(pages[-1])), "add")
        menu.addSeparator()
        icons.bind(menu.addAction(f"Remove {label}", lambda: self.remove_pages(pages)), "remove")

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
                    self.fields.add(scene, p)
            elif p in self.page_items and not self.fields.page_has_focus(p):
                scene.removeItem(self.page_items.pop(p))
                self.fields.remove(p)

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
        row = self.order.index(self.pno) if self.pno in self.order else -1
        if self.thumbs.currentRow() != row:
            self.thumbs.blockSignals(True)
            self.thumbs.setCurrentRow(row)
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
        for a in (
            self.a_print, self.a_print_preview, self.a_split, self.a_protect, self.a_rotate_left, self.a_rotate_right,
            self.a_remove_pages, self.a_insert_pages, self.a_fit_content, self.a_uncrop, self.a_copy_path,
            self.a_redact_colour, self.a_tick, self.a_untick,
        ):
            a.setEnabled(has_doc)
        self.a_unprotect.setEnabled(has_doc and bool(self.password))
        self.info.setVisible(self.active)
        if not self.active:
            return
        shown = self.settings.get("show_sidebar")
        self.side.setVisible(shown)
        self.a_sidebar.setChecked(shown)
        if not has_doc:
            self.window().setWindowTitle(APP_NAME)
            self.zoom_label.setText("")
            self.stats.setText("Open a PDF (Ctrl+O) or drop one on the window")
            self.path_label.clear()
            return
        self.refresh_edits()
        self.window().setWindowTitle(f"{'* ' if n else ''}{self.doc_path.name} - {APP_NAME}")
        self.zoom_label.setText(f"{round(self.zoom * 100)}%")
        self.thumbs.blockSignals(True)
        ticked = 0
        for i in range(self.thumbs.count()):
            item = self.thumbs.item(i)
            k = self.page_edits(item.data(Qt.UserRole))
            item.setText(f"{i + 1}" + (f"\n{k} edit{'s' * (k != 1)}" if k else ""))
            item.setForeground(QColor("#d93025") if k else self.thumbs.palette().text().color())
            ticked += item.checkState() == Qt.Checked
        self.thumbs.blockSignals(False)
        pages = sum(1 for p in self.order if self.page_edits(p))
        position = self.order.index(self.pno) + 1 if self.pno in self.order else 0
        self.stats.setText(
            f"page {position}/{len(self.order)}   |   {n} edit(s) on {pages} page(s)   |   {ticked} ticked   |"
        )
        path = str(self.doc_path)
        self.path_label.setText(self.path_label.fontMetrics().elidedText(path, Qt.ElideMiddle, PATH_WIDTH))
        self.path_label.setToolTip(path)

    def step_page(self, delta):
        if self.doc and self.pno in self.order:
            position = self.order.index(self.pno) + delta
            if 0 <= position < len(self.order):
                self.goto(self.order[position])

    def goto(self, pno):
        if not self.doc or pno not in self.order:
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
            page_w = max(self.doc[p].rect.width for p in self.order)
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
        """A copy of the edits for the undo history (the state() keys)."""
        def copy(o):
            lists = {k: [list(v) for v in o[k]] if k == "strokes" else list(o[k]) for k in ("rects", "strokes") if k in o}
            return dict(o, rect=pymupdf.Rect(o["rect"]), **lists)

        items = {p: [copy(o) for o in objs] for p, objs in self.items.items()}
        return {
            "items": items, "deleted": frozenset(self.deleted), "forms": dict(self.forms),
            "annot_text": dict(self.annot_text), "order": self.order, "rotation": dict(self.rotation),
            "crop": {p: pymupdf.Rect(r) for p, r in self.crop.items()}, "inserted": self.inserted,
        }

    def push_undo(self, snap):
        self.history.append(snap)
        self.future.clear()
        self.fields.edit_key = None

    def begin_change(self):
        self.pending = self.snapshot()

    def end_change(self, clicked=None):
        changed = False
        for it in self.view.scene().items():
            if isinstance(it, ObjItem) and it.kind not in ("annot", "highlight"):
                rect = self.to_pdf(it.mapRectToScene(it.rect()), it.pno)
                if max(abs(a - b) for a, b in zip(rect, it.obj["rect"])) > 0.01:
                    if it.kind == "ink":
                        shift = rect.tl - it.obj["rect"].tl
                        it.obj["strokes"] = [[pt + shift for pt in stroke] for stroke in it.obj["strokes"]]
                    it.obj["rect"] = rect
                    if it.kind == "crop":
                        self.crop[it.pno] = rect
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
        """Make `snap` (a snapshot() dict) the current edits and show the first page it changes."""
        before = self.state()
        self.items, self.deleted, self.forms = snap["items"], set(snap["deleted"]), dict(snap["forms"])
        self.annot_text, self.order, self.rotation = dict(snap["annot_text"]), snap["order"], dict(snap["rotation"])
        self.crop, self.inserted = dict(snap["crop"]), snap["inserted"]
        self.fields.edit_key = None
        layout_changed = (before["rotation"], before["inserted"]) != (self.rotation, self.inserted)
        if layout_changed or before["deleted"] != self.deleted:
            self.reload_display()
        if layout_changed or before["order"] != self.order:
            self.rebuild_thumbs()
        touched = {p for p in set(before["items"]) | set(self.items) if before["items"].get(p) != self.items.get(p)}
        touched |= {p for p, _ in before["deleted"] ^ self.deleted}
        touched |= {key[0] for key, _ in set(before["forms"].items()) ^ set(self.forms.items())}
        touched |= {p for p in set(before["rotation"]) | set(self.rotation) if before["rotation"].get(p) != self.rotation.get(p)}
        touched |= {p for p in set(before["crop"]) | set(self.crop) if before["crop"].get(p) != self.crop.get(p)}
        shown = [p for p in self.order if p in touched]
        if self.pno not in self.order:
            self.pno = self.order[0]
        if shown and shown[0] not in self.offsets:
            self.goto(shown[0])
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
        if self.thumbs.hasFocus() and self.thumbs.currentItem():
            return self.remove_pages(self.target_pages(self.thumbs.currentItem().data(Qt.UserRole)))
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
        self.annot_types.update({(i.pno, i.obj["xref"]): i.obj["type"] for i in selected if i.kind == "annot"})
        for i in selected:
            if i.kind == "crop":
                self.crop.pop(i.pno, None)
            elif i.kind != "annot":
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
        if self.tool in ("crop", "text", "image"):
            return "rect"
        if self.tool == "draw":
            return "draw"
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
        """Right-click on the page: copy / mark / redact the text selection, if any, then the page commands."""
        if not self.doc:
            return
        clicked = self.page_at(scene_pos)
        if not (self.text_sel and self.text_sel[0] in self.offsets):
            if clicked is not None:
                menu = QMenu(self)
                self.add_page_actions(menu, self.target_pages(clicked))
                menu.exec(global_pos)
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
        menu.addSeparator()
        self.add_page_actions(menu.addMenu("Page"), self.target_pages(clicked if clicked is not None else pno))
        menu.exec(global_pos)

    def item_menu(self, obj, pno, kind, pos):
        """Right-click on a placed edit, a PDF annotation or a crop frame: its own commands, then the page's."""
        if not self.doc or pno not in self.offsets:
            return
        menu = QMenu(self)
        delete = partial(self.remove_obj, pno, obj)
        if kind == "text":
            menu.addAction("Edit text", lambda: self.edit_obj(obj))
        elif kind == "image":
            menu.addAction("Replace image...", lambda: self.replace_image(pno, obj))
        elif kind == "ink":
            colours = menu.addMenu("Colour")
            for name, rgb in PEN_COLOURS:
                a = colours.addAction(swatch_icon(rgb), name, lambda rgb=rgb: self.set_ink(pno, obj, rgb=rgb))
                a.setCheckable(True)
                a.setChecked(tuple(obj["rgb"]) == rgb)
            widths = menu.addMenu("Width")
            for name, width in PEN_WIDTHS:
                a = widths.addAction(name, lambda width=width: self.set_ink(pno, obj, width=width))
                a.setCheckable(True)
                a.setChecked(obj["width"] == width)
        elif kind == "note":
            menu.addAction("Edit note...", lambda: self.item_clicked(obj, pno, kind))
        elif kind == "highlight":
            menu.addAction("Comment...", lambda: self.item_clicked(obj, pno, kind))
            styles = menu.addMenu("Style")
            for key, label in MARK_STYLES.items():
                a = styles.addAction(label, lambda key=key: self.restyle_mark(pno, obj, key))
                a.setCheckable(True)
                a.setChecked(obj.get("style", "highlight") == key)
                icons.bind(a, STYLE_ICONS[key])
            colours = menu.addMenu("Colour")
            for name, rgb in palette_for(obj.get("style", "highlight")):
                a = colours.addAction(swatch_icon(rgb), name, lambda rgb=rgb: self.recolour(pno, obj, rgb))
                a.setCheckable(True)
                a.setChecked(tuple(obj["rgb"]) == tuple(rgb))
            colours.addAction("Custom...", lambda: self.pick_obj_colour(pno, obj))
        elif kind == "redact":
            fill = menu.addMenu("Fill colour")
            fill.addAction("Colour around it", lambda: self.recolour(pno, obj, self.sample_background(pno, obj["rect"])))
            fill.addAction(swatch_icon((0, 0, 0)), "Black", lambda: self.recolour(pno, obj, (0, 0, 0)))
            fill.addAction(swatch_icon((255, 255, 255)), "White", lambda: self.recolour(pno, obj, (255, 255, 255)))
            fill.addAction("Custom...", lambda: self.pick_obj_colour(pno, obj))
        elif kind == "annot":
            menu.addAction("Comment...", lambda: self.item_clicked(obj, pno, kind))
            delete = partial(self.delete_annot, (pno, obj["xref"]))
        elif kind == "crop":
            menu.addAction("Crop to content instead", lambda: self.crop_to_content([pno]))
            delete = partial(self.set_crops, {pno: None})
        menu.addSeparator()
        icons.bind(menu.addAction("Remove crop" if kind == "crop" else "Delete", delete), "remove")
        menu.addSeparator()
        self.add_page_actions(menu.addMenu("Page"), self.target_pages(pno))
        menu.exec(pos)

    def restyle_mark(self, pno, obj, style):
        if obj.get("style", "highlight") != style:
            self.push_undo(self.snapshot())
            obj["style"] = style
            obj["rgb"] = self.mark_colour(style)
            self.render()

    def pick_obj_colour(self, pno, obj):
        c = QColorDialog.getColor(QColor(*obj["rgb"]), self, "Colour")
        if c.isValid():
            self.recolour(pno, obj, (c.red(), c.green(), c.blue()))

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
        elif self.tool == "crop":
            self.crop_menu(pno, sel)
        elif self.tool == "text":
            self.start_text(pno, sel.tl, sel.width)
        elif self.tool == "image":
            self.place_image(pno, sel)
        else:
            self.apply_selection(pno, *self.line_rects(sel, pno))

    def crop_menu(self, pno, rect):
        """After drawing a crop rectangle: which pages get it."""
        ticked = [self.order[i] for i in self.ticked_positions()]
        menu = QMenu(self)
        menu.addAction("Crop this page", lambda: self.set_crops({pno: rect}))
        menu.addAction(f"Crop all {len(self.order)} pages to this area", lambda: self.set_crops({p: rect for p in self.order}))
        if ticked:
            menu.addAction(f"Crop {len(ticked)} ticked pages to this area", lambda: self.set_crops({p: rect for p in ticked}))
        menu.addSeparator()
        menu.addAction("Crop this page to its content instead", lambda: self.crop_to_content([pno]))
        menu.exec(QCursor.pos())

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
        annot = self.doc[key[0]].load_annot(key[1])
        self.annot_types[key] = annot.type[1] if annot else "annotation"
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
            self.start_text(pno, pos)
        elif self.tool == "sign":
            self.place_signature(pos, pno)
        elif self.tool == "image":
            self.place_image(pno, pymupdf.Rect(pos, pos))
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

    def font_families(self):
        families = []
        for name in pdf_font_names(self.doc) + DEFAULT_FONTS:
            if font_family(name) not in families:
                families.append(font_family(name))
        return families

    def qt_font(self, name, text):
        """A Qt font close to how PDF font `name` will render `text`, for typing on the page."""
        code, buf, _, _ = self.font_for(name, text or "x")
        font = QFont()
        if buf:
            if code not in self.qt_families:
                ids = QFontDatabase.addApplicationFontFromData(QByteArray(buf))
                families = QFontDatabase.applicationFontFamilies(ids) if ids >= 0 else []
                self.qt_families[code] = families[0] if families else ""
            font.setFamily(self.qt_families[code])
        else:
            hint = {"ti": QFont.Serif, "co": QFont.TypeWriter}.get(code[:2], QFont.SansSerif)
            font.setStyleHint(hint)
            font.setFamily({QFont.Serif: "Serif", QFont.TypeWriter: "Monospace"}.get(hint, "Sans Serif"))
        bold, italic = font_style(name)
        font.setBold(bold)
        font.setItalic(italic)
        font.setPixelSize(TEXT_EDIT_PX)
        return font

    def sized(self, obj):
        w, h = text_size(self.font_for(obj["font"], obj["text"])[2], obj)
        obj["rect"] = pymupdf.Rect(obj["rect"].x0, obj["rect"].y0, obj["rect"].x0 + w, obj["rect"].y0 + h)
        return obj

    def add_obj(self, pno, obj):
        snap = self.snapshot()
        self.items.setdefault(pno, []).append(obj)
        self.push_undo(snap)
        self.render()

    def start_text(self, pno, pos, width=None, obj=None):
        """Type a text box on the page at `pos` (a dragged `width` wraps it); `obj` re-edits a placed one."""
        self.close_popover()
        self.finish_text()
        if obj is None:
            font, size, rgb = self.nearby_style(pos, pno)
            obj = {"kind": "text", "rect": pymupdf.Rect(pos, pos), "text": "", "font": font, "size": size, "rgb": rgb}
            if width and width > 4:
                obj["width"] = width
        style = {k: obj[k] for k in ("font", "size", "rgb")}
        for it in self.view.scene().items():
            if isinstance(it, ObjItem) and it.obj is obj:
                it.hide()
        bar = None
        item = TextEditItem(lambda: bar is not None and bar.has_focus())
        item.setPlainText(obj["text"])
        item.setPos(self.to_scene(pymupdf.Rect(obj["rect"].tl, obj["rect"].tl), pno).topLeft())
        self.view.scene().addItem(item)

        def restyle(family=None, size=None, bold=None, italic=None, rgb=None):
            if family is not None:
                style["font"] = styled_font(family, bold, italic, pdf_font_names(self.doc))
                style["size"], style["rgb"] = size, rgb
                bar.hint.setText(self.font_for(style["font"], item.toPlainText() or "x")[3])
            scale = style["size"] / TEXT_EDIT_PX
            item.setFont(self.qt_font(style["font"], item.toPlainText()))
            item.setDefaultTextColor(QColor(*style["rgb"]))
            item.setScale(scale)
            item.setTextWidth(obj["width"] / scale if obj.get("width") else -1)

        bold, italic = font_style(obj["font"])
        bar = FormatBar(
            self.view.viewport(), self.font_families(), font_family(obj["font"]), obj["size"], bold, italic, obj["rgb"],
            restyle,
        )
        restyle()
        bar.hint.setText(self.font_for(style["font"], obj["text"] or "x")[3])
        bar.adjustSize()
        anchor = self.view.mapFromScene(item.scenePos())
        bar.move(max(4, min(anchor.x(), self.view.viewport().width() - bar.width() - 4)), max(4, anchor.y() - bar.height() - 8))
        bar.show()
        item.finished.connect(self.finish_text)
        self.text_edit = (pno, obj, item, bar, style, self.snapshot())
        self.view.setFocus()
        item.setFocus()

    def finish_text(self, rerender=True):
        """Commit the text box being typed (an empty one is dropped) as one undo step."""
        if not self.text_edit:
            return
        pno, obj, item, bar, style, snap = self.text_edit
        self.text_edit = None
        text = item.toPlainText().rstrip("\n")
        bar.hide()
        bar.deleteLater()
        if item.scene() is not None:
            item.scene().removeItem(item)
        placed = any(o is obj for o in self.items.get(pno, []))
        if not text.strip():
            if not placed:
                if rerender:
                    QTimer.singleShot(0, self.render)
                return
            self.items[pno] = [o for o in self.items[pno] if o is not obj]
        elif text != obj["text"] or any(obj[k] != v for k, v in style.items()) or not placed:
            obj.update(style, text=text)
            self.sized(obj)
            if not placed:
                self.items.setdefault(pno, []).append(obj)
        else:
            if rerender:
                QTimer.singleShot(0, self.render)
            return
        self.push_undo(snap)
        # deferred: this can run from the text item's own key or focus handler, and render() deletes it
        if rerender:
            QTimer.singleShot(0, self.render)

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

    def pick_image(self):
        """Bytes of an image the user picks (PNG/JPEG as they are, other formats as PNG) and its size, or None."""
        path, _ = QFileDialog.getOpenFileName(
            self, "Place image", self.settings.get("last_folder") or str(Path.home()), IMAGE_FILTER
        )
        if not path:
            return None
        data = Path(path).read_bytes()
        image = QImage.fromData(data)
        if image.isNull():
            QMessageBox.warning(self, APP_NAME, f"{Path(path).name} is not an image this app can read.")
            return None
        if not data.startswith((b"\x89PNG", b"\xff\xd8")):
            data = png_bytes(image)
        return data, image.width(), image.height()

    def place_image(self, pno, box):
        """Place a picture: a click puts it there at up to half the page width, a dragged box fits it inside."""
        picked = self.pick_image()
        if not picked:
            return
        data, w, h = picked
        if box.width > 4 and box.height > 4:
            scale = min(box.width / w, box.height / h)
        else:
            # pixels at 96 dpi, but never wider than half the page
            scale = min(72 / 96, self.doc[pno].rect.width / 2 / w)
        rect = pymupdf.Rect(box.x0, box.y0, box.x0 + w * scale, box.y0 + h * scale)
        self.add_obj(pno, {"kind": "image", "rect": rect, "data": data})

    def replace_image(self, pno, obj):
        picked = self.pick_image()
        if picked:
            data, w, h = picked
            self.push_undo(self.snapshot())
            r = obj["rect"]
            obj.update(data=data, rect=pymupdf.Rect(r.x0, r.y0, r.x0 + r.width, r.y0 + r.width * h / w))
            self.render()

    def edit_obj(self, obj):
        self.pending = None
        self.start_text(self.owner(obj), obj["rect"].tl, obj=obj)

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
            for pno in self.order:
                page = self.doc[pno]
                for rect in sorted(page.search_for(query), key=lambda r: (round(r.y0), r.x0)):
                    # search_for ignores case, so re-check the text under the hit
                    if not self.find_case.isChecked() or query in page.get_textbox(rect + (-1, -1, 1, 1)):
                        self.matches.append((pno, rect))
        if self.matches:
            here = self.order.index(self.pno) if self.pno in self.order else 0
            self.match_idx = next((i for i, (p, _) in enumerate(self.matches) if self.order.index(p) >= here), 0)
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
